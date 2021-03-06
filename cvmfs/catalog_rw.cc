/**
 * This file is part of the CernVM File System.
 */

#define __STDC_FORMAT_MACROS

#include "catalog_rw.h"

#include <inttypes.h>
#include <cstdio>
#include <cstdlib>

#include "logging.h"
#include "util.h"

using namespace std;  // NOLINT

namespace catalog {

WritableCatalog::WritableCatalog(const string &path, Catalog *parent) :
  Catalog(PathString(path.data(), path.length()), parent),
  dirty_(false)
{ }


WritableCatalog::~WritableCatalog() {
  // CAUTION HOT!
  // (see Catalog.h - near the definition of FinalizePreparedStatements)
  FinalizePreparedStatements();
}


void WritableCatalog::Transaction() {
  Sql transaction(database(), "BEGIN;");
  bool retval = transaction.Execute();
  assert(retval == true);
}


void WritableCatalog::Commit() {
  Sql commit(database(), "COMMIT;");
  bool retval = commit.Execute();
  assert(retval == true);
  dirty_ = false;
}


void WritableCatalog::InitPreparedStatements() {
  Catalog::InitPreparedStatements(); // polymorphism: up call

  bool retval = Sql(database(), "PRAGMA foreign_keys = ON").Execute();
  assert(retval);
  sql_insert_ = new SqlDirentInsert(database());
  sql_touch_ = new SqlDirentTouch(database());
  sql_unlink_ = new SqlDirentUnlink(database());
  sql_update_ = new SqlDirentUpdate(database());
  sql_max_link_id_ = new SqlMaxHardlinkGroup(database());
  sql_inc_linkcount_ = new SqlIncLinkcount(database());
}


void WritableCatalog::FinalizePreparedStatements() {
  // no polymorphism: no up call (see Catalog.h -
  // near the definition of this method)
  delete sql_insert_;
  delete sql_touch_;
  delete sql_unlink_;
  delete sql_update_;
  delete sql_max_link_id_;
  delete sql_inc_linkcount_;
}


/**
 * Find out the maximal hardlink group id in this catalog.
 */
uint32_t WritableCatalog::GetMaxLinkId() const {
  int result = -1;

  if (sql_max_link_id_->FetchRow()) {
    result = sql_max_link_id_->GetMaxGroupId();
  }
  sql_max_link_id_->Reset();

  return result;
}


/**
 * Adds a direcotry entry.
 * @param entry the DirectoryEntry to add to the catalog
 * @param entry_path the full path of the DirectoryEntry to add
 * @param parent_path the full path of the containing directory
 * @return true if DirectoryEntry was added, false otherwise
 */
bool WritableCatalog::AddEntry(const DirectoryEntry &entry,
                               const string &entry_path,
                               const string &parent_path)
{
  SetDirty();

  hash::Md5 path_hash((hash::AsciiPtr(entry_path)));
  hash::Md5 parent_hash((hash::AsciiPtr(parent_path)));

  LogCvmfs(kLogCatalog, kLogVerboseMsg, "add entry %s", entry_path.c_str());

  bool result =
    sql_insert_->BindPathHash(path_hash) &&
    sql_insert_->BindParentPathHash(parent_hash) &&
    sql_insert_->BindDirent(entry) &&
    sql_insert_->Execute();

  sql_insert_->Reset();

  return result;
}


/**
 * Set the mtime of a DirectoryEntry in the catalog to the current time
 * (utime or what the 'touch' command does)
 * @param entry the entry structure which will be touched
 * @param entry_path the full path of the entry to touch
 * @return true on successful touching, false otherwise
 */
bool WritableCatalog::TouchEntry(const DirectoryEntry &entry,
                                 const std::string &entry_path) {
  SetDirty();

  // perform a touch operation for the given path
  hash::Md5 path_hash = hash::Md5(hash::AsciiPtr(entry_path));
  bool result =
    sql_touch_->BindPathHash(path_hash) &&
    sql_touch_->BindTimestamp(entry.mtime()) &&
    sql_touch_->Execute();

  sql_touch_->Reset();

  return result;
}


/**
 * Removes the specified entry from the catalog.
 * Note: removing a directory which is non-empty results in dangling entries.
 *       (this should be treated in upper layers)
 * @param entry_path the full path of the DirectoryEntry to delete
 * @return true if entry was removed, false otherwise
 */
bool WritableCatalog::RemoveEntry(const string &file_path) {
  SetDirty();

  hash::Md5 path_hash = hash::Md5(hash::AsciiPtr(file_path));
  bool result =
    sql_unlink_->BindPathHash(path_hash) &&
    sql_unlink_->Execute();

  sql_unlink_->Reset();

  return result;
}


bool WritableCatalog::IncLinkcount(const string &path_within_group,
                                   const int delta)
{
  SetDirty();

  hash::Md5 path_hash = hash::Md5(hash::AsciiPtr(path_within_group));

  bool result =
    sql_inc_linkcount_->BindPathHash(path_hash) &&
    sql_inc_linkcount_->BindDelta(delta) &&
    sql_inc_linkcount_->Execute();

  sql_inc_linkcount_->Reset();

  return result;
}


bool WritableCatalog::UpdateEntry(const DirectoryEntry &entry,
                                  const hash::Md5 &path_hash) {
  SetDirty();

  bool result =
    sql_update_->BindPathHash(path_hash) &&
    sql_update_->BindDirent(entry) &&
    sql_update_->Execute();

  sql_update_->Reset();

  return result;
}


/**
 * Sets the last modified time stamp of this catalog to current time.
 * @return true on success, false otherwise
 */
bool WritableCatalog::UpdateLastModified() {
  const time_t now = time(NULL);
  const string sql = "INSERT OR REPLACE INTO properties "
     "(key, value) VALUES ('last_modified', '" + StringifyInt(now) + "');";
  return Sql(database(), sql).Execute();
}


/**
 * Increments the revision of the catalog in the database.
 * @return true on success, false otherwise
 */
bool WritableCatalog::IncrementRevision() {
  const string sql =
    "UPDATE properties SET value=value+1 WHERE key='revision';";
  return Sql(database(), sql).Execute();
}


/**
 * Sets the content hash of the previous catalog revision.
 * @return true on success, false otherwise
 */
bool WritableCatalog::SetPreviousRevision(const hash::Any &hash) {
  const string sql = "INSERT OR REPLACE INTO properties "
    "(key, value) VALUES ('previous_revision', '" + hash.ToString() + "');";
  return Sql(database(), sql).Execute();
}


/**
 * Moves a subtree from this catalog into a just created nested catalog.
 */
bool WritableCatalog::Partition(WritableCatalog *new_nested_catalog) {
  // Create connection between parent and child catalogs
  if (!MakeTransitionPoint(new_nested_catalog->path().ToString())) {
    LogCvmfs(kLogCatalog, kLogStderr,
             "failed to create nested catalog mountpoint in catalog '%s'",
             path().c_str());
    return false;
  }
  if (!new_nested_catalog->MakeNestedRoot()) {
    LogCvmfs(kLogCatalog, kLogStderr, "failed to create nested catalog root "
             "entry in new nested catalog '%s'",
             new_nested_catalog->path().c_str());
    return false;
  }

  // Move the present directory tree into the newly created nested catalog
  // if we hit nested catalog mountpoints on the way, we return them through
  // the passed list
  vector<string> GrandChildMountpoints;
  if (!MoveToNested(new_nested_catalog->path().ToString(),
                    new_nested_catalog, &GrandChildMountpoints))
  {
    LogCvmfs(kLogCatalog, kLogStderr, "failed to move directory structure in "
             "'%s' to new nested catalog", new_nested_catalog->path().c_str());
    return false;
  }

  // Nested catalog mountpoints found in the moved directory structure are now
  // links to nested catalogs of the newly created nested catalog.
  // Move these references into the new nested catalog
  if (!MoveCatalogsToNested(GrandChildMountpoints, new_nested_catalog)) {
    LogCvmfs(kLogCatalog, kLogStderr, "failed to move nested catalog references"
             " into new nested catalog '%s'",
             new_nested_catalog->path().c_str());
    return false;
  }

  return true;
}


bool WritableCatalog::MakeTransitionPoint(const string &mountpoint) {
  // Find the directory entry to edit
  DirectoryEntry transition_entry;
  if (!LookupPath(PathString(mountpoint.data(), mountpoint.length()),
                  &transition_entry))
  {
    return false;
  }

  assert(transition_entry.IsDirectory() &&
         !transition_entry.IsNestedCatalogRoot());

  transition_entry.set_is_nested_catalog_mountpoint(true);
  if (!UpdateEntry(transition_entry, mountpoint))
    return false;

  return true;
}


bool WritableCatalog::MakeNestedRoot() {
  DirectoryEntry root_entry;
  if (!LookupPath(path(), &root_entry)) {
    LogCvmfs(kLogCatalog, kLogStderr, "no root entry found in catalog '%s'",
             path().c_str());
    return false;
  }
  assert(root_entry.IsDirectory() && !root_entry.IsNestedCatalogMountpoint());

  root_entry.set_is_nested_catalog_root(true);
  if (!UpdateEntry(root_entry, path().ToString()))
    return false;

  return true;
}


bool WritableCatalog::MoveToNestedRecursively(
       const string directory,
       WritableCatalog *new_nested_catalog,
       vector<string> *grand_child_mountpoints)
{
  // After creating a new nested catalog we have move all elements
  // now contained by the new one.  List and move them recursively.
  DirectoryEntryList listing;
  if (!ListingPath(PathString(directory.data(), directory.length()),
                              &listing))
  {
    return false;
  }

  // Go through the listing
  string full_path;
  for (DirectoryEntryList::const_iterator i = listing.begin(),
       iEnd = listing.end(); i != iEnd; ++i)
  {
    full_path = directory + "/";
    full_path.append(i->name().GetChars(), i->name().GetLength());

    // The entries are first inserted into the new catalog
    if (!new_nested_catalog->AddEntry(*i, full_path)) {
      return false;
    }

    // Then we check if we have some special cases:
    if (i->IsNestedCatalogMountpoint()) {
      grand_child_mountpoints->push_back(full_path);
    } else if (i->IsDirectory()) {
      // Recurse deeper into the directory tree
      if (!MoveToNestedRecursively(full_path, new_nested_catalog,
                                   grand_child_mountpoints))
      {
        return false;
      }
    }

    // Remove the entry from the current catalog
    if (!RemoveEntry(full_path)) {
      return false;
    }
  }

  return true;
}


bool WritableCatalog::MoveCatalogsToNested(
       const vector<string> &nested_catalogs,
       WritableCatalog *new_nested_catalog)
{
  for (vector<string>::const_iterator i = nested_catalogs.begin(),
       iEnd = nested_catalogs.end(); i != iEnd; ++i)
  {
    // TODO: only fitting nested catalogs
    Catalog *attached_reference = NULL;
    if (!RemoveNestedCatalog(*i, &attached_reference))
      return false;

    // TODO: big bug: with HASH
    if (!new_nested_catalog->InsertNestedCatalog(*i, attached_reference,
                                                 hash::Any(hash::kSha1)))
    {
      return false;
    }
  }

  return true;
}


/**
 * Insert a nested catalog reference into this catalog.
 * The attached catalog object of this mountpoint can be specified (optional)
 * This way, the in-memory representation of the catalog tree is updated, too
 * @param mountpoint the path to the catalog to add a reference to
 * @param attached_reference can contain a reference to the attached catalog
 *        object of mountpoint
 * @param content_hash can be set to safe a content hash together with the
 *        reference
 * @return true on success, false otherwise
 */
bool WritableCatalog::InsertNestedCatalog(const string &mountpoint,
                                          Catalog *attached_reference,
                                          const hash::Any content_hash)
{
  const string sha1_string = (!content_hash.IsNull()) ?
                             content_hash.ToString() : "";

  Sql stmt(database(),
    "INSERT INTO nested_catalogs (path, sha1) VALUES (:p, :sha1);");
  bool successful =
    stmt.BindText(1, mountpoint) &&
    stmt.BindText(2, sha1_string) &&
    stmt.Execute();

  // If a reference of the in-memory object of the newly referenced
  // catalog was passed, we add this to our own children
  if (successful && attached_reference != NULL)
    AddChild(attached_reference);

  return successful;
}


/**
 * Remove a nested catalog reference from the database.
 * If the catalog 'mountpoint' is currently attached as a child, it will be
 * removed, too (but not detached).
 * @param[in] mountpoint the mountpoint of the nested catalog to dereference in
              the database
 * @param[out] attached_reference is set to the object of the attached child or
 *             to NULL
 * @return true on success, false otherwise
 */
bool WritableCatalog::RemoveNestedCatalog(const string &mountpoint,
                                          Catalog **attached_reference)
{
  Sql stmt(database(),
           "DELETE FROM nested_catalogs WHERE path = :p;");
  bool successful =
    stmt.BindText(1, mountpoint) &&
  stmt.Execute();

  // If the reference was successfully deleted, we also have to check whether
  // there is also an attached reference in our in-memory data.
  // In this case we remove the child and return it through **attached_reference
  if (successful) {
    Catalog *child = FindChild(PathString(mountpoint.data(),
                                          mountpoint.length()));
    if (child != NULL)
      RemoveChild(child);
    if (attached_reference != NULL)
      *attached_reference = child;
  }

  return successful;
}


/**
 * Updates the link to a nested catalog in the database.
 * @param path the path of the nested catalog to update
 * @param hash the hash to set the given nested catalog link to
 * @return true on success, false otherwise
 */
bool WritableCatalog::UpdateNestedCatalog(const string &path,
                                          const hash::Any &hash)
{
  const string sha1_str = hash.ToString();
  const string sql = "UPDATE nested_catalogs SET sha1 = :sha1 "
    "WHERE path = :path;";
  Sql stmt(database(), sql);

  stmt.BindText(1, sha1_str);
  stmt.BindText(2, path);

  return stmt.Execute();
}


bool WritableCatalog::MergeIntoParent() {
  assert(!IsRoot());

  WritableCatalog *parent = GetWritableParent();

  // Copy all DirectoryEntries to the parent catalog
  if (!CopyToParent()) {
    LogCvmfs(kLogCatalog, kLogStderr, "failed to copy directory entries from "
             "'%s' to parent '%s'",
             this->path().c_str(), parent->path().c_str());
    return false;
  }

  // Copy the nested catalog references
  if (!CopyCatalogsToParent()) {
    LogCvmfs(kLogCatalog, kLogStderr, "failed to merge nested catalog references"
             " from '%s' to parent '%s'",
             this->path().c_str(), parent->path().c_str());
    return false;
  }

  // Remove the nested catalog reference for this nested catalog.
  // From now on this catalog will be dangling!
  if (!parent->RemoveNestedCatalog(this->path().ToString(), NULL)) {
    LogCvmfs(kLogCatalog, kLogStderr, "failed to remove nested catalog reference"
             "'%s', in parent catalog '%s'",
             this->path().c_str(), parent->path().c_str());
    return false;
  }

  return true;
}


bool WritableCatalog::CopyCatalogsToParent() {
  WritableCatalog *parent = GetWritableParent();

  // Obtain a list of all nested catalog references
  NestedCatalogList nested_catalog_references = ListNestedCatalogs();

  // Go through the list and update the databases
  // simultaneously we are checking if the referenced catalogs are currently
  // attached and update the in-memory data structures as well
  for (NestedCatalogList::const_iterator i = nested_catalog_references.begin(),
       iEnd = nested_catalog_references.end(); i != iEnd; ++i)
  {
    Catalog *child = FindChild(i->path);
    parent->InsertNestedCatalog(i->path.ToString(), child, i->hash);
  }

  return true;
}

bool WritableCatalog::CopyToParent() {
  // We could simply copy all entries from this database to the 'other' database
  // BUT: 1. this would create collisions in hardlink group IDs.
  //         therefor we first update all hardlink group IDs to fit behind the
  //         ones in the 'other' database
  //      2. the root entry of the nested catalog is present twice:
  //         1. in the parent directory (as mount point) and
  //         2. in the nested catalog (as root entry)
  //         therefore we delete the mount point from the parent before merging

  WritableCatalog *parent = GetWritableParent();

  // Update hardlink group IDs in this nested catalog.
  // To avoid collisions we add the maximal present hardlink group ID in parent
  // to all hardlink group IDs in the nested catalog.
  // (CAUTION: hardlink group ID is saved in the inode field --> legacy :-) )
  const uint64_t offset = static_cast<uint64_t>(parent->GetMaxLinkId()) << 32;
  const string update_link_ids =
    "UPDATE catalog SET hardlinks = hardlinks + " + StringifyInt(offset) +
    " WHERE hardlinks > 1 << 32;";

  Sql sql_update_link_ids(database(), update_link_ids);
  if (!sql_update_link_ids.Execute()) {
    LogCvmfs(kLogCatalog, kLogStderr,
             "failed to harmonize the hardlink group IDs in '%s'",
             this->path().c_str());
    return false;
  }

  // Remove the nested catalog mount point.
  // It will be replaced with the nested catalog root entry when copying.
  if (!parent->RemoveEntry(this->path().ToString())) {
    LogCvmfs(kLogCatalog, kLogStderr,
             "failed to remove mount point '%s' of nested catalog to be merged",
             this->path().c_str());
    return false;
  }

  // Now copy all DirectoryEntries to the 'other' catalog.
  // There will be no data collisions, as we resolved them beforehand
  if (dirty_)
    Commit();
  if (parent->dirty_)
    parent->Commit();
  Sql sql_attach(database(), "ATTACH '" + parent->database_path() +
                             "' AS other;");
  if (!sql_attach.Execute()) {
    LogCvmfs(kLogCatalog, kLogStderr,
             "failed to attach catalog '%s' in nested path '%s' (%d)",
             parent->database_path().c_str(), this->path().c_str(),
             sql_attach.GetLastError());
    return false;
  }
  if (!Sql(database(), "INSERT INTO other.catalog "
                       "SELECT * FROM main.catalog;").Execute())
  {
    LogCvmfs(kLogCatalog, kLogStderr, "failed to copy DirectoryEntries from "
             "catalog '%s' to catalog '%s'",
             this->path().c_str(), parent->path().c_str());
    return false;
  }
  if (!Sql(database(), "DETACH other;").Execute()) {
    LogCvmfs(kLogCatalog, kLogStderr,
             "failed to detach database of catalog '%s' from catalog '%s'",
             parent->path().c_str(), this->path().c_str());
    return false;
  }
  parent->SetDirty();

  // Change the just copied nested catalog root to an ordinary directory
  // (the nested catalog is merged into it's parent)
  DirectoryEntry old_root_entry;
  if (!parent->LookupPath(this->path(), &old_root_entry)) {
    LogCvmfs(kLogCatalog, kLogStderr, "root entry of removed nested catalog '%s'"
             " not found in parent catalog '%s'",
             this->path().c_str(), parent->path().c_str());
    return false;
  }

  assert(old_root_entry.IsDirectory() && old_root_entry.IsNestedCatalogRoot() &&
         !old_root_entry.IsNestedCatalogMountpoint());

  // Remove the nested catalog root mark
  old_root_entry.set_is_nested_catalog_root(false);
  if (!parent->UpdateEntry(old_root_entry, this->path().ToString())) {
    LogCvmfs(kLogCatalog, kLogStderr,
             "unable to remove the 'nested catalog root' mark from '%s'",
             this->path().c_str());
    return false;
  }

  return true;
}

}  // namespace catalog
