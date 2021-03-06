/**
 * This file is part of the CernVM File System.
 */

#define __STDC_FORMAT_MACROS

#include "sync_mediator.h"

#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cassert>

#include "compression.h"
#include "smalloc.h"
#include "hash.h"
#include "fs_traversal.h"
#include "util.h"

using namespace std;  // NOLINT

namespace publish {


PublishFilesCallback::PublishFilesCallback(SyncMediator *mediator) {
  assert(mediator);
  mediator_ = mediator;
}


void PublishFilesCallback::Callback(const std::string &path, int retval,
                                    const std::string &digest)
{
  LogCvmfs(kLogPublish, kLogVerboseMsg,
           "Spooler callback for %s, digest %s, retval %d",
           path.c_str(), digest.c_str(), retval);
  if (retval != 0) {
    LogCvmfs(kLogPublish, kLogStderr, "Spool failure for %s (%d)",
             path.c_str(), retval);
    abort();
  }
  hash::Any hash(hash::kSha1, hash::HexPtr(digest));

  pthread_mutex_lock(&mediator_->lock_file_queue_);
  SyncItemList::iterator itr = mediator_->file_queue_.find(path);
  assert(itr != mediator_->file_queue_.end());
  itr->second.SetContentHash(hash);
  pthread_mutex_unlock(&mediator_->lock_file_queue_);
  mediator_->catalog_manager_->AddFile(itr->second.CreateCatalogDirent(),
                                       itr->second.relative_parent_path());
}


PublishHardlinksCallback::PublishHardlinksCallback(SyncMediator *mediator) {
  assert(mediator);
  mediator_ = mediator;
}


void PublishHardlinksCallback::Callback(const std::string &path, int retval,
                                        const std::string &digest)
{
  LogCvmfs(kLogPublish, kLogVerboseMsg,
           "Spooler callback for hardlink %s, digest %s, retval %d",
           path.c_str(), digest.c_str(), retval);
  if (retval != 0) {
    LogCvmfs(kLogPublish, kLogStderr, "Spool failure for %s (%d)",
             path.c_str(), retval);
    abort();
  }
  hash::Any hash(hash::kSha1, hash::HexPtr(digest));

  bool found = false;
  for (unsigned i = 0; i < mediator_->hardlink_queue_.size(); ++i) {
    if (mediator_->hardlink_queue_[i].master.GetUnionPath() == path) {
      found = true;
      mediator_->hardlink_queue_[i].master.SetContentHash(hash);
      SyncItemList::iterator j;
      j = mediator_->hardlink_queue_[i].hardlinks.begin();
      for (j = mediator_->hardlink_queue_[i].hardlinks.begin();
           j != mediator_->hardlink_queue_[i].hardlinks.end(); ++j)
      {
        j->second.SetContentHash(hash);
      }
      break;
    }
  }
  assert(found);
}


SyncMediator::SyncMediator(catalog::WritableCatalogManager *catalogManager,
                           const SyncParameters *params) :
  catalog_manager_(catalogManager),
  params_(params)
{
  int retval = pthread_mutex_init(&lock_file_queue_, NULL);
  assert(retval == 0);

  params->spooler->SetCallback(new PublishFilesCallback(this));
  LogCvmfs(kLogPublish, kLogStdout, "Processing changes...");
}


/**
 * Add an entry to the repository.
 * Added directories will be traversed in order to add the complete subtree.
 */
void SyncMediator::Add(SyncItem &entry) {
  if (entry.IsDirectory()) {
    AddDirectoryRecursively(entry);
    return;
  }

  if (entry.IsRegularFile() || entry.IsSymlink()) {
    // Create a nested catalog if we find a new catalog marker
    if (entry.IsCatalogMarker() && entry.IsNew())
      CreateNestedCatalog(entry);

    // A file is a hard link if the link count is greater than 1
    if (entry.GetUnionLinkcount() > 1)
      InsertHardlink(entry);
    else
      AddFile(entry);
    return;
  }

  PrintWarning("'" + entry.GetRelativePath() + "' cannot be added. "
               "Unregcognized file type.");
}


/**
 * Touch an entry in the repository.
 */
void SyncMediator::Touch(SyncItem &entry) {
  if (entry.IsDirectory()) {
    TouchDirectory(entry);
    return;
  }

  if (entry.IsRegularFile() || entry.IsSymlink()) {
    Replace(entry);
    return;
  }

  PrintWarning("'" + entry.GetRelativePath() + "' cannot be touched. "
               "Unregcognized file type.");
}


/**
 * Remove an entry from the repository. Directories will be recursively removed.
 */
void SyncMediator::Remove(SyncItem &entry) {
  if (entry.IsDirectory()) {
    RemoveDirectoryRecursively(entry);
    return;
  }

  if (entry.IsRegularFile() || entry.IsSymlink()) {
		// First remove the file...
		RemoveFile(entry);

		// ... then the nested catalog (if needed)
    if (entry.IsCatalogMarker() && !entry.IsNew()) {
      RemoveNestedCatalog(entry);
    }

    return;
	}

  PrintWarning("'" + entry.GetRelativePath() + "' cannot be deleted. "
               "Unregcognized file type.");
}


/**
 * Remove the old entry and add the new one.
 */
void SyncMediator::Replace(SyncItem &entry) {
	Remove(entry);
	Add(entry);
}


void SyncMediator::EnterDirectory(SyncItem &entry) {
	HardlinkGroupMap new_map;
	hardlink_stack_.push(new_map);
}


void SyncMediator::LeaveDirectory(SyncItem &entry)
{
  CompleteHardlinks(entry);
	AddLocalHardlinkGroups(GetHardlinkMap());
	hardlink_stack_.pop();
}


/**
 * Do any pending processing and commit all changes to the catalogs.
 * To be called after change set traversal is finished.
 */
Manifest *SyncMediator::Commit() {
  LogCvmfs(kLogPublish, kLogStdout,
           "Waiting for upload of files before committing...");
  while (!params_->spooler->IsIdle())
    sleep(1);

  if (!hardlink_queue_.empty()) {
    LogCvmfs(kLogPublish, kLogStdout, "Processing hardlinks...");
    params_->spooler->UnsetCallback();
    params_->spooler->SetCallback(new PublishHardlinksCallback(this));
    for (HardlinkGroupList::const_iterator i = hardlink_queue_.begin(),
         iEnd = hardlink_queue_.end(); i != iEnd; ++i)
    {
      LogCvmfs(kLogPublish, kLogVerboseMsg, "Spooling hardlink group %s",
               i->master.GetUnionPath().c_str());
      params_->spooler->SpoolProcess(i->master.GetUnionPath(), "data", "");
    }

    while (!params_->spooler->IsIdle())
      sleep(1);

    for (HardlinkGroupList::const_iterator i = hardlink_queue_.begin(),
         iEnd = hardlink_queue_.end(); i != iEnd; ++i)
    {
      LogCvmfs(kLogPublish, kLogVerboseMsg, "Processing hardlink group %s",
               i->master.GetUnionPath().c_str());
      AddHardlinkGroup(*i);
    }
  }

  params_->spooler->UnsetCallback();

  LogCvmfs(kLogPublish, kLogStdout, "Committing file catalogs...");
  if (params_->spooler->num_errors() > 0) {
    LogCvmfs(kLogPublish, kLogStderr, "failed to commit files");
    return NULL;
  }

	catalog_manager_->PrecalculateListings();
	return catalog_manager_->Commit();
}


void SyncMediator::InsertHardlink(SyncItem &entry) {
  uint64_t inode = entry.GetUnionInode();
  LogCvmfs(kLogPublish, kLogVerboseMsg, "found hardlink %"PRIu64" at %s",
           inode, entry.GetUnionPath().c_str());

  // Find the hard link group in the lists
  HardlinkGroupMap::iterator hardlink_group = GetHardlinkMap().find(inode);

  if (hardlink_group == GetHardlinkMap().end()) {
    // Create a new hardlink group
    GetHardlinkMap().insert(
      HardlinkGroupMap::value_type(inode, HardlinkGroup(entry)));
  } else {
    // Append the file to the appropriate hardlink group
    hardlink_group->second.AddHardlink(entry);
  }
}


void SyncMediator::InsertLegacyHardlink(SyncItem &entry) {
  // Check if found file has hardlinks (nlink > 1)
  // As we are looking through all files in one directory here, there might be
  // completely untouched hardlink groups, which we can safely skip.
  // Finally we have to see if the hardlink is already part of this group

  if (entry.GetUnionLinkcount() < 2)
    return;

  uint64_t inode = entry.GetUnionInode();
  HardlinkGroupMap::iterator hl_group;
  hl_group = GetHardlinkMap().find(inode);

  if (hl_group != GetHardlinkMap().end()) { // touched hardlinks in this group?
    bool found = false;

		// search for the entry in this group
    for (SyncItemList::const_iterator i = hl_group->second.hardlinks.begin(),
         iEnd = hl_group->second.hardlinks.end(); i != iEnd; ++i)
    {
      if (i->second == entry) {
        found = true;
        break;
      }
    }

    if (!found) {
      // Hardlink already in the group?
      // If one element of a hardlink group is edited, all elements must be
      // replaced.  Here, we remove an untouched hardlink and add it to its
      // hardlink group for re-adding later
      LogCvmfs(kLogPublish, kLogVerboseMsg, "Picked up legacy hardlink %s",
               entry.GetUnionPath().c_str());
      Remove(entry);
      hl_group->second.AddHardlink(entry);
    }
  }
}


/**
 * Create a recursion engine which DOES NOT recurse into directories.
 * It basically goes through the current directory (in the union volume) and
 * searches for legacy hardlinks which has to be connected to the new
 * or edited ones.
 */
void SyncMediator::CompleteHardlinks(SyncItem &entry) {
  // If no hardlink in this directory was changed, we can skip this
	if (GetHardlinkMap().empty())
    return;

  LogCvmfs(kLogPublish, kLogVerboseMsg, "Post-processing hard links in %s",
           entry.GetUnionPath().c_str());

  // Look for legacy hardlinks
  const set<string> ignore;
  FileSystemTraversal<SyncMediator> traversal(this, union_engine_->union_path(),
                                              false, ignore);
  traversal.fn_new_file =
    &SyncMediator::LegacyRegularHardlinkCallback;
  traversal.fn_new_symlink = &SyncMediator::LegacySymlinkHardlinkCallback;
  traversal.Recurse(entry.GetUnionPath());
}


void SyncMediator::LegacyRegularHardlinkCallback(const string &parent_dir,
                                                 const string &file_name)
{
  SyncItem entry(parent_dir, file_name, kItemFile, union_engine_);
  InsertLegacyHardlink(entry);
}


void SyncMediator::LegacySymlinkHardlinkCallback(const string &parent_dir,
                                                  const string &file_name)
{
  SyncItem entry(parent_dir, file_name, kItemSymlink, union_engine_);
  InsertLegacyHardlink(entry);
}


void SyncMediator::AddDirectoryRecursively(SyncItem &entry) {
	AddDirectory(entry);

	// Create a recursion engine, which recursively adds all entries in a newly
  // created directory
	FileSystemTraversal<SyncMediator> traversal(
    this, union_engine_->scratch_path(), true,
    union_engine_->GetIgnoreFilenames());
	traversal.fn_enter_dir = &SyncMediator::EnterAddedDirectoryCallback;
	traversal.fn_leave_dir = &SyncMediator::LeaveAddedDirectoryCallback;
	traversal.fn_new_file = &SyncMediator::AddFileCallback;
	traversal.fn_new_symlink = &SyncMediator::AddSymlinkCallback;
	traversal.fn_new_dir_prefix = &SyncMediator::AddDirectoryCallback;
	traversal.Recurse(entry.GetScratchPath());
}


bool SyncMediator::AddDirectoryCallback(const std::string &parent_dir,
                                        const std::string &dir_name)
{
  SyncItem entry(parent_dir, dir_name, kItemDir, union_engine_);
  AddDirectory(entry);
  return true;  // The recursion engine should recurse deeper here
}


void SyncMediator::AddFileCallback(const std::string &parent_dir,
                                   const std::string &file_name)
{
  SyncItem entry(parent_dir, file_name, kItemFile, union_engine_);
  Add(entry);
}


void SyncMediator::AddSymlinkCallback(const std::string &parent_dir,
                                      const std::string &link_name)
{
  SyncItem entry(parent_dir, link_name, kItemSymlink, union_engine_);
  Add(entry);
}


void SyncMediator::EnterAddedDirectoryCallback(const std::string &parent_dir,
                                               const std::string &dir_name)
{
  SyncItem entry(parent_dir, dir_name, kItemDir, union_engine_);
  EnterDirectory(entry);
}


void SyncMediator::LeaveAddedDirectoryCallback(const std::string &parent_dir,
                                               const std::string &dir_name)
{
  SyncItem entry(parent_dir, dir_name, kItemDir, union_engine_);
  LeaveDirectory(entry);
}


void SyncMediator::RemoveDirectoryRecursively(SyncItem &entry) {
	// Delete a directory AFTER it was emptied here,
	// because it would start up another recursion

	const bool recurse = false;
  const set<string> ignore;
  FileSystemTraversal<SyncMediator> traversal(
    this, union_engine_->rdonly_path(), recurse, ignore);
  traversal.fn_new_file = &SyncMediator::RemoveFileCallback;
  traversal.fn_new_dir_postfix =
    &SyncMediator::RemoveDirectoryCallback;
  traversal.fn_new_symlink = &SyncMediator::RemoveSymlinkCallback;
  traversal.Recurse(entry.GetRdOnlyPath());

	// The given directory was emptied recursively and can now itself be deleted
  RemoveDirectory(entry);
}


void SyncMediator::RemoveFileCallback(const std::string &parent_dir,
                                      const std::string &file_name)
{
  SyncItem entry(parent_dir, file_name, kItemFile, union_engine_);
  Remove(entry);
}


void SyncMediator::RemoveSymlinkCallback(const std::string &parent_dir,
                                         const std::string &link_name)
{
  SyncItem entry(parent_dir, link_name, kItemSymlink, union_engine_);
  Remove(entry);
}


void SyncMediator::RemoveDirectoryCallback(const std::string &parent_dir,
                                           const std::string &dir_name)
{
  SyncItem entry(parent_dir, dir_name, kItemDir, union_engine_);
  RemoveDirectory(entry);
}


void SyncMediator::CreateNestedCatalog(SyncItem &requestFile) {
  if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogStdout, "[add] Nested catalog at %s",
             GetParentPath(requestFile.GetUnionPath()).c_str());
	if (!params_->dry_run) {
    bool retval = catalog_manager_->CreateNestedCatalog(
                    requestFile.relative_parent_path());
    assert(retval);
  }
}


void SyncMediator::RemoveNestedCatalog(SyncItem &requestFile) {
  if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogStdout, "[rem] Nested catalog at %s",
             GetParentPath(requestFile.GetUnionPath()).c_str());
	if (!params_->dry_run) {
    bool retval = catalog_manager_->RemoveNestedCatalog(
                    requestFile.relative_parent_path());
    assert(retval);
  }
}


void SyncMediator::AddFile(SyncItem &entry) {
  if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogStdout, "[add] %s", entry.GetUnionPath().c_str());

	if (entry.IsSymlink() && !params_->dry_run) {
    // Symlinks are completely stored in the catalog
    catalog_manager_->AddFile(entry.CreateCatalogDirent(),
                              entry.relative_parent_path());
	} else {
	  // Push the file to the spooler, remember the entry for the path
    pthread_mutex_lock(&lock_file_queue_);
    file_queue_[entry.GetUnionPath()] = entry;
    pthread_mutex_unlock(&lock_file_queue_);
    // Spool the file
    params_->spooler->SpoolProcess(entry.GetUnionPath(), "data", "");
  }
}


void SyncMediator::RemoveFile(SyncItem &entry) {
	if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogStdout, "[rem] %s", entry.GetUnionPath().c_str());
	if (!params_->dry_run) {
    if (entry.GetRdOnlyLinkcount() > 1) {
      LogCvmfs(kLogPublish, kLogVerboseMsg, "remove %s from hardlink group",
               entry.GetUnionPath().c_str());
      bool retval =
        catalog_manager_->ShrinkHardlinkGroup(entry.GetRelativePath());
      assert(retval);
    }
    catalog_manager_->RemoveFile(entry.GetRelativePath());
  }
}


void SyncMediator::TouchFile(SyncItem &entry) {
	if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogDebug, "[tou] %s", entry.GetUnionPath().c_str());
	if (!params_->dry_run) {
    catalog_manager_->TouchFile(entry.CreateCatalogDirent(),
                                entry.GetRelativePath());
  }
}


void SyncMediator::AddDirectory(SyncItem &entry) {
	if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogStdout, "[add] %s", entry.GetUnionPath().c_str());
	if (!params_->dry_run) {
    catalog_manager_->AddDirectory(entry.CreateCatalogDirent(),
                                   entry.relative_parent_path());
  }
}


void SyncMediator::RemoveDirectory(SyncItem &entry) {
	if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogStdout, "[rem] %s", entry.GetUnionPath().c_str());
	if (!params_->dry_run)
    catalog_manager_->RemoveDirectory(entry.GetRelativePath());
}


void SyncMediator::TouchDirectory(SyncItem &entry) {
	if (params_->print_changeset)
    LogCvmfs(kLogPublish, kLogStdout, "[tou] %s", entry.GetUnionPath().c_str());
	if (!params_->dry_run)
    catalog_manager_->TouchDirectory(entry.CreateCatalogDirent(),
                                     entry.GetRelativePath());
}


/**
 * All hardlinks in the current directory have been picked up.  Now they are
 * added to the catalogs.
 */
void SyncMediator::AddLocalHardlinkGroups(const HardlinkGroupMap &hardlinks) {
	for (HardlinkGroupMap::const_iterator i = hardlinks.begin(),
       iEnd = hardlinks.end(); i != iEnd; ++i)
  {
    if (i->second.hardlinks.size() != i->second.master.GetUnionLinkcount()) {
      LogCvmfs(kLogPublish, kLogStderr, "Hardlinks across directories (%s)",
               i->second.master.GetUnionPath().c_str());
      abort();
    }

    if (params_->print_changeset) {
      LogCvmfs(kLogPublish, kLogStdout | kLogNoLinebreak,
               "[add] hardlink group around: (%s)",
               i->second.master.GetUnionPath().c_str());
			for (SyncItemList::const_iterator j = i->second.hardlinks.begin(),
           jEnd = i->second.hardlinks.end(); j != jEnd; ++j)
      {
				LogCvmfs(kLogPublish, kLogStdout | kLogNoLinebreak, " %s",
                 j->second.filename().c_str());
			}
			LogCvmfs(kLogPublish, kLogStdout, "");
		}

		if (params_->dry_run)
      continue;

    if (i->second.master.IsSymlink())
      AddHardlinkGroup(i->second);
    else
      hardlink_queue_.push_back(i->second);
  }
}


void SyncMediator::AddHardlinkGroup(const HardlinkGroup &group) {
  // Create a DirectoryEntry list out of the hardlinks
  catalog::DirectoryEntryList hardlinks;
  for (SyncItemList::const_iterator i = group.hardlinks.begin(),
       iEnd = group.hardlinks.end(); i != iEnd; ++i)
  {
    hardlinks.push_back(i->second.CreateCatalogDirent());
  }
  catalog_manager_->AddHardlinkGroup(hardlinks,
                                     group.master.relative_parent_path());
}

}  // namespace publish
