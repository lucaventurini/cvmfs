/**
 * This file is part of the CernVM file system.
 */

#include "catalog_sql.h"

#include <fcntl.h>
#include <errno.h>

#include <cstdlib>
#include <cstring>
#include <cstdlib>

#include "platform.h"
#include "catalog.h"
#include "logging.h"
#include "util.h"

using namespace std;  // NOLINT

namespace catalog {

Database::Database(const std::string filename, const OpenMode open_mode) {
  int retval;

  filename_ = filename;
  ready_ = false;
  schema_version_ = 0.0;
  sqlite_db_ = NULL;

  int flags = SQLITE_OPEN_NOMUTEX;
  switch (open_mode) {
    case kOpenReadOnly:
      flags |= SQLITE_OPEN_READONLY;
      read_write_ = false;
      break;
    case kOpenReadWrite:
      flags |= SQLITE_OPEN_READWRITE;
      read_write_ = true;
      break;
    default:
      abort();
  }

  // Open database file (depending on the flags read-only or read-write)
  LogCvmfs(kLogCatalog, kLogDebug, "opening database file %s",
           filename_.c_str());
  if (SQLITE_OK != sqlite3_open_v2(filename_.c_str(), &sqlite_db_, flags, NULL))
  {
    LogCvmfs(kLogCatalog, kLogDebug, "cannot open catalog database file %s",
             filename_.c_str());
    return;
  }
  sqlite3_extended_result_codes(sqlite_db_, 1);

  // Read-ahead into file system buffers
  int fd_readahead = open(filename_.c_str(), O_RDONLY);
  if (fd_readahead < 0) {
    LogCvmfs(kLogCatalog, kLogDebug, "failed to open %s for read-ahead (%d)",
             filename_.c_str(), errno);
    goto database_failure;
    return;
  }
  retval = platform_readahead(fd_readahead);
  if (retval != 0) {
    LogCvmfs(kLogCatalog, kLogDebug, "failed to read-ahead %s (%d)",
             filename_.c_str(), errno);
    close(fd_readahead);
    goto database_failure;
  }
  close(fd_readahead);

  {  // Get schema version
    Sql sql_schema(*this, "SELECT value FROM properties WHERE key='schema';");
    if (sql_schema.FetchRow()) {
      schema_version_ = sql_schema.RetrieveDouble(0);
    } else {
      schema_version_ = 1.0;
    }
  }
  LogCvmfs(kLogCatalog, kLogDebug, "open db with schema version %f",
           schema_version_);
  if ((schema_version_ >= 2.0-kSchemaEpsilon) &&
      (schema_version_ < kLatestSupportedSchema-kSchemaEpsilon))
  {
    LogCvmfs(kLogCatalog, kLogDebug, "schema version %f not supported (%s)",
             schema_version_, filename.c_str());
    goto database_failure;
  }

  ready_ = true;
  return;

 database_failure:
  sqlite3_close(sqlite_db_);
  sqlite_db_ = NULL;
}


/**
 * Private constructor.  Used to create a new sqlite database.
 */
Database::Database(sqlite3 *sqlite_db, const float schema, const bool rw) {
  sqlite_db_ = sqlite_db;
  filename_ = "TMP";
  schema_version_ = schema;
  read_write_ = rw;
  ready_ = false;  // Don't close on delete
}


Database::~Database() {
  if (ready_)
    sqlite3_close(sqlite_db_);
}


/**
 * This method creates a new database file and initializes the database schema.
 */
bool Database::Create(const string &filename,
                      const DirectoryEntry &root_entry,
                      const string &root_path)
{
  sqlite3 *sqlite_db;
  SqlDirentInsert *sql_insert;
  Sql *sql_schema;
  int open_flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE |
                   SQLITE_OPEN_CREATE;

  // Path hashes
  hash::Md5 root_path_hash = hash::Md5(hash::AsciiPtr(root_path));
  hash::Md5 root_parent_hash;
  if (root_path == "")
    root_parent_hash = hash::Md5();
  else
    root_parent_hash = hash::Md5(hash::AsciiPtr(GetParentPath(root_path)));

  // Create the new catalog file and open it
  LogCvmfs(kLogCatalog, kLogVerboseMsg, "creating new catalog at '%s'",
           filename.c_str());
  if (sqlite3_open_v2(filename.c_str(), &sqlite_db, open_flags, NULL) !=
      SQLITE_OK)
  {
    LogCvmfs(kLogCatalog, kLogStderr,
             "Cannot create and open catalog database file '%s'",
             filename.c_str());
    return false;
  }
  sqlite3_extended_result_codes(sqlite_db, 1);
  Database database(sqlite_db, kLatestSchema, true);

  bool retval;
  string sql;
  retval = Sql(database,
    "CREATE TABLE catalog "
    "(md5path_1 INTEGER, md5path_2 INTEGER, parent_1 INTEGER, parent_2 INTEGER,"
    " hardlinks INTEGER, hash BLOB, size INTEGER, mode INTEGER, mtime INTEGER,"
    " flags INTEGER, name TEXT, symlink TEXT, uid INTEGER, gid INTEGER, "
    " CONSTRAINT pk_catalog PRIMARY KEY (md5path_1, md5path_2));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(database,
               "CREATE INDEX idx_catalog_parent "
               "ON catalog (parent_1, parent_2);").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(database,
    "CREATE TABLE chunks "
    "(md5path_1 INTEGER, md5path_2 INTEGER, offset INTEGER, size INTEGER, "
    " hash BLOB, "
    " CONSTRAINT pk_chunks PRIMARY KEY (md5path_1, md5path_2, offset, size), "
    " FOREIGN KEY (md5path_1, md5path_2) REFERENCES "
    "   catalog(md5path_1, md5path_2));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(database,
               "CREATE TABLE properties (key TEXT, value TEXT, "
               "CONSTRAINT pk_properties PRIMARY KEY (key));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(database,
               "CREATE TABLE nested_catalogs (path TEXT, sha1 TEXT, "
               "CONSTRAINT pk_nested_catalogs PRIMARY KEY (path));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(database, "INSERT INTO properties "
               "(key, value) VALUES ('revision', 0);").Execute();
  if (!retval)
    goto create_schema_fail;

  sql_schema = new Sql(database, "INSERT INTO properties "
                                 "(key, value) VALUES ('schema', :schema);");
  retval = sql_schema->BindDouble(1, kLatestSchema) && sql_schema->Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(database,
               "CREATE TABLE statistics (counter TEXT, value INTEGER, "
               "CONSTRAINT pk_statistics PRIMARY KEY (counter));").Execute();
  if (!retval)
    goto create_schema_fail;

  retval = Sql(database,
    "INSERT INTO statistics (counter, value) "
    "SELECT 'self_regular', 0 UNION ALL SELECT 'self_symlink', 0 UNION ALL "
    "SELECT 'self_dir', 1 UNION ALL SELECT 'self_nested', 0 UNION ALL "
    "SELECT 'subtree_regular', 0 UNION ALL SELECT 'subtree_symlink', 0 UNION ALL "
    "SELECT 'subtree_dir', 0 UNION ALL SELECT 'subtree_nested', 0;").Execute();
  if (!retval)
    goto create_schema_fail;

  // Insert root entry
  sql_insert = new SqlDirentInsert(database);
  retval = sql_insert->BindPathHash(root_path_hash) &&
           sql_insert->BindParentPathHash(root_parent_hash) &&
           sql_insert->BindDirent(root_entry) &&
           sql_insert->Execute();
  delete sql_insert;
  if (!retval)
    goto create_schema_fail;

  if (root_path != "") {
    retval = Sql(database, "INSERT INTO properties "
      "(key, value) VALUES ('root_prefix', '" + root_path + "');").Execute();
    if (!retval)
      goto create_schema_fail;
  }

  sqlite3_close(sqlite_db);
  return true;

create_schema_fail:
  LogCvmfs(kLogSql, kLogVerboseMsg, "sql failure %s",
           sqlite3_errmsg(sqlite_db));
  sqlite3_close(sqlite_db);
  return false;
}


//------------------------------------------------------------------------------


Sql::Sql(const Database &database, const std::string &statement) {
  Init(database.sqlite_db(), statement);
}


Sql::~Sql() {
  last_error_code_ = sqlite3_finalize(statement_);

  if (!Successful()) {
    LogCvmfs(kLogSql, kLogDebug,
             "failed to finalize statement - error code: %d", last_error_code_);
  }
  LogCvmfs(kLogSql, kLogDebug, "successfully finalized statement");
}


/**
 * Executes the prepared statement.
 * (this method should be used for modifying statements like DELETE or INSERT)
 * @return true on success otherwise false
 */
bool Sql::Execute() {
  last_error_code_ = sqlite3_step(statement_);
  return Successful();
}


/**
 * Execute the prepared statement or fetch its next row.
 * This method is intended to step through the result set.
 * If it returns false this does not neccessarily mean, that the actual
 * statement execution failed, but that no row was fetched.
 * @return true if a new row was fetched otherwise false
 */
bool Sql::FetchRow() {
  last_error_code_ = sqlite3_step(statement_);
  return SQLITE_ROW == last_error_code_;
}


/**
 * Reset a prepared statement to make it reusable.
 * @return true on success otherwise false
 */
bool Sql::Reset() {
  last_error_code_ = sqlite3_reset(statement_);
  return Successful();
}


bool Sql::Init(const sqlite3 *database, const std::string &statement) {
  last_error_code_ = sqlite3_prepare_v2((sqlite3*)database,
                                        statement.c_str(),
                                        -1, // parse until null termination
                                        &statement_,
                                        NULL);

  if (!Successful()) {
    LogCvmfs(kLogSql, kLogDebug, "failed to prepare statement '%s' (%d: %s)",
             statement.c_str(), GetLastError(),
             sqlite3_errmsg((sqlite3*)database));
    return false;
  }

  LogCvmfs(kLogSql, kLogDebug, "successfully prepared statement '%s'",
           statement.c_str());
  return true;
}


//------------------------------------------------------------------------------


unsigned SqlDirent::CreateDatabaseFlags(const DirectoryEntry &entry) const {
  unsigned int database_flags = 0;

  if (entry.IsNestedCatalogRoot())
    database_flags |= kFlagDirNestedRoot;
  else if (entry.IsNestedCatalogMountpoint())
    database_flags |= kFlagDirNestedMountpoint;

  if (entry.IsDirectory())
    database_flags |= kFlagDir;
  else if (entry.IsLink())
    database_flags |= kFlagFile | kFlagLink;
  else
    database_flags |= kFlagFile;

  return database_flags;
}


/**
 * Expands variant symlinks containing $(VARIABLE) string.  Uses the environment
 * variables of the current process (cvmfs2)
 */
void SqlDirent::ExpandSymlink(LinkString *raw_symlink) const {
  const char *c = raw_symlink->GetChars();
  const char *cEnd = c+raw_symlink->GetLength();
  for (; c <= cEnd; ++c) {
    if (*c == '$')
      goto expand_symlink;
  }
  return;

 expand_symlink:
  LinkString result;
  for (; c <= cEnd; ++c) {
    if ((*c == '$') && (c < cEnd-2) && (*(c+1) == '(')) {
      c += 2;
      const char *rpar = c;
      while (rpar <= cEnd) {
        if (*rpar == ')')
          goto expand_symlink_getenv;
        rpar++;
      }
      // right parenthesis missing
      result.Append("$(", 2);
      continue;

     expand_symlink_getenv:
      const unsigned environ_var_length = rpar-c;
      char environ_var[environ_var_length+1];
      environ_var[environ_var_length] = '\0';
      memcpy(environ_var, c, environ_var_length);
      const char *environ_value = getenv(environ_var);  // Don't free!
      if (environ_value)
        result.Append(environ_value, strlen(environ_value));
      c = rpar+1;
      continue;
    }
    result.Append(c, 1);
  }
  raw_symlink->Assign(result);
  return;
}


//------------------------------------------------------------------------------


bool SqlDirentWrite::BindDirentFields(const int hash_idx,
                                      const int hardlinks_idx,
                                      const int size_idx,
                                      const int mode_idx,
                                      const int mtime_idx,
                                      const int flags_idx,
                                      const int name_idx,
                                      const int symlink_idx,
                                      const int uid_idx,
                                      const int gid_idx,
                                      const DirectoryEntry &entry)
{
  return (
    BindSha1Blob(hash_idx, entry.checksum_) &&
    BindInt64(hardlinks_idx, entry.hardlinks_) &&
    BindInt64(size_idx, entry.size_) &&
    BindInt(mode_idx, entry.mode_) &&
    BindInt64(uid_idx, entry.uid_) &&
    BindInt64(gid_idx, entry.gid_) &&
    BindInt64(mtime_idx, entry.mtime_) &&
    BindInt(flags_idx, CreateDatabaseFlags(entry)) &&
    BindText(name_idx, entry.name_.GetChars(), entry.name_.GetLength()) &&
    BindText(symlink_idx, entry.symlink_.GetChars(), entry.symlink_.GetLength())
  );
}


//------------------------------------------------------------------------------


string SqlLookup::GetFieldsToSelect(const Database &database) const {
  if (database.schema_version() < 2.1-Database::kSchemaEpsilon) {
    return "hash, inode, size, mode, mtime, flags, name, symlink, "
        //    0     1      2     3     4      5      6      7
           "md5path_1, md5path_2, parent_1, parent_2, rowid";
        //    8          9           10        11       12
  } else {
    return "hash, hardlinks, size, mode, mtime, flags, name, symlink, "
        //    0        1      2     3     4      5      6      7
           "md5path_1, md5path_2, parent_1, parent_2, rowid, uid, gid";
        //    8          9           10        11       12    13   14
  }
}


hash::Md5 SqlLookup::GetPathHash() const {
  return RetrieveMd5(8, 9);
}


hash::Md5 SqlLookup::GetParentPathHash() const {
  return RetrieveMd5(10, 11);
}


/**
 * This method is a friend of DirectoryEntry.
 */
DirectoryEntry SqlLookup::GetDirent(const Catalog *catalog) const {
  DirectoryEntry result;

  const unsigned database_flags = RetrieveInt(5);
  result.catalog_ = (Catalog*)catalog;
  result.is_nested_catalog_root_ = (database_flags & kFlagDirNestedRoot);
  result.is_nested_catalog_mountpoint_ =
    (database_flags & kFlagDirNestedMountpoint);
  const char *name = reinterpret_cast<const char *>(RetrieveText(6));
  const char *symlink = reinterpret_cast<const char *>(RetrieveText(7));

  // must be set later by a second catalog lookup
  result.parent_inode_ = DirectoryEntry::kInvalidInode;
  result.hardlinks_ = RetrieveInt64(1);
  if (catalog->schema() < 2.1-Database::kSchemaEpsilon) {
    result.inode_ = ((Catalog*)catalog)->GetMangledInode(RetrieveInt64(12), 0);
    result.uid_ = g_uid;
    result.gid_ = g_gid;
  } else {
    const uint32_t hardlink_group =
      DirectoryEntry::Hardlinks2HardlinkGroup(result.hardlinks_);
    result.inode_ = ((Catalog*)catalog)->GetMangledInode(RetrieveInt64(12),
                                                         hardlink_group);
    result.uid_ = RetrieveInt64(13);
    result.gid_ = RetrieveInt64(14);
  }
  result.mode_ = RetrieveInt(3);
  result.size_ = RetrieveInt64(2);
  result.mtime_ = RetrieveInt64(4);
  result.checksum_ = RetrieveSha1Blob(0);
  result.name_.Assign(name, strlen(name));
  result.symlink_.Assign(symlink, strlen(symlink));
  ExpandSymlink(&result.symlink_);

  return result;
}


//------------------------------------------------------------------------------


SqlListing::SqlListing(const Database &database) {
  const string statement =
    "SELECT " + GetFieldsToSelect(database) + " FROM catalog "
    "WHERE (parent_1 = :p_1) AND (parent_2 = :p_2);";
  Init(database.sqlite_db(), statement);
}


bool SqlListing::BindPathHash(const struct hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


//------------------------------------------------------------------------------


SqlLookupPathHash::SqlLookupPathHash(const Database &database) {
  const string statement =
    "SELECT " + GetFieldsToSelect(database) + " FROM catalog "
    "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);";
  Init(database.sqlite_db(), statement);
}

bool SqlLookupPathHash::BindPathHash(const struct hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


//------------------------------------------------------------------------------


SqlLookupInode::SqlLookupInode(const Database &database) {
  const string statement =
    "SELECT " + GetFieldsToSelect(database) + " FROM catalog "
    "WHERE rowid = :rowid;";
  Init(database.sqlite_db(), statement);
}


bool SqlLookupInode::BindRowId(const uint64_t inode) {
  return BindInt64(1, inode);
}


//------------------------------------------------------------------------------


SqlNestedCatalogLookup::SqlNestedCatalogLookup(const Database &database) {
  Init(database.sqlite_db(),
       "SELECT sha1 FROM nested_catalogs WHERE path=:path;");
}


bool SqlNestedCatalogLookup::BindSearchPath(const PathString &path) {
  return BindText(1, path.GetChars(), path.GetLength());
}


hash::Any SqlNestedCatalogLookup::GetContentHash() const {
  const string sha1 = string(reinterpret_cast<const char *>(RetrieveText(0)));
  return (sha1.empty()) ? hash::Any(hash::kSha1) :
                          hash::Any(hash::kSha1, hash::HexPtr(sha1));
}


//------------------------------------------------------------------------------


SqlNestedCatalogListing::SqlNestedCatalogListing(const Database &database) {
  Init(database.sqlite_db(), "SELECT path, sha1 FROM nested_catalogs;");
}


PathString SqlNestedCatalogListing::GetMountpoint() const {
  const char *mountpoint = reinterpret_cast<const char *>(RetrieveText(0));
  return PathString(mountpoint, strlen(mountpoint));
}


hash::Any SqlNestedCatalogListing::GetContentHash() const {
  const string sha1 = string(reinterpret_cast<const char *>(RetrieveText(1)));
  return (sha1.empty()) ? hash::Any(hash::kSha1) :
                          hash::Any(hash::kSha1, hash::HexPtr(sha1));
}


//------------------------------------------------------------------------------


SqlDirentInsert::SqlDirentInsert(const Database &database) {
  const string statement = "INSERT INTO catalog "
    "(md5path_1, md5path_2, parent_1, parent_2, hash, hardlinks, size, mode,"
    //    1           2         3         4       5       6        7     8
    "mtime, flags, name, symlink, uid, gid) "
    // 9,     10    11     12     13   14
    "VALUES (:md5_1, :md5_2, :p_1, :p_2, :hash, :links, :size, :mode, :mtime,"
    " :flags, :name, :symlink, :uid, :gid);";
  Init(database.sqlite_db(), statement);
}


bool SqlDirentInsert::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


bool SqlDirentInsert::BindParentPathHash(const hash::Md5 &hash) {
  return BindMd5(3, 4, hash);
}


bool SqlDirentInsert::BindDirent(const DirectoryEntry &entry) {
  return BindDirentFields(5, 6, 7, 8, 9, 10, 11, 12, 13, 14, entry);
}


//------------------------------------------------------------------------------


SqlDirentUpdate::SqlDirentUpdate(const Database &database) {
  const string statement =
    "UPDATE catalog "
    "SET hash = :hash, size = :size, mode = :mode, mtime = :mtime, "
//            1             2             3               4
    "flags = :flags, name = :name, symlink = :symlink, hardlinks = :hardlinks, "
//          5             6                  7                8
    "uid = :uid, gid = :gid "
//          9           10
    "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);";
//                     11                       12
  Init(database.sqlite_db(), statement);
}


bool SqlDirentUpdate::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(11, 12, hash);
}


bool SqlDirentUpdate::BindDirent(const DirectoryEntry &entry) {
  return BindDirentFields(1, 8, 2, 3, 4, 5, 6, 7, 9, 10, entry);
}


//------------------------------------------------------------------------------


SqlDirentTouch::SqlDirentTouch(const Database &database) {
  Init(database.sqlite_db(),
       "UPDATE catalog SET mtime = :mtime "
       "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);");
}


bool SqlDirentTouch::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(2, 3, hash);
}


bool SqlDirentTouch::BindTimestamp(const time_t timestamp) {
  return BindInt64(1, timestamp);
}


//------------------------------------------------------------------------------


SqlDirentUnlink::SqlDirentUnlink(const Database &database) {
  Init(database.sqlite_db(),
       "DELETE FROM catalog "
       "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);");
}

bool SqlDirentUnlink::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(1, 2, hash);
}


//------------------------------------------------------------------------------


SqlIncLinkcount::SqlIncLinkcount(const Database &database) {
  const string statememt =
    "UPDATE catalog SET inode="
    "CASE (hardlinks << 32) >> 32 WHEN 2 THEN 0 ELSE hardlinks+1*(:delta) END "
    "WHERE hardlinks = (SELECT hardlinks from catalog "
    "WHERE md5path_1 = :md5_1 AND md5path_2 = :md5_2);";
  Init(database.sqlite_db(), statememt);
}


bool SqlIncLinkcount::BindPathHash(const hash::Md5 &hash) {
  return BindMd5(2, 3, hash);
}


bool SqlIncLinkcount::BindDelta(const int delta) {
  return BindInt(1, delta);
}


//------------------------------------------------------------------------------


SqlMaxHardlinkGroup::SqlMaxHardlinkGroup(const Database &database) {
  Init(database.sqlite_db(), "SELECT max(hardlinks) FROM catalog;");
}

uint32_t SqlMaxHardlinkGroup::GetMaxGroupId() const {
  return RetrieveInt64(0) >> 32;
}


//------------------------------------------------------------------------------


SqlGetCounter::SqlGetCounter(const Database &database) {
  Init(database.sqlite_db(),
       "SELECT value from statistics WHERE counter = :counter;");
}


bool SqlGetCounter::BindCounter(const std::string &counter) {
  return BindText(1, counter);
}


uint64_t SqlGetCounter::GetCounter() const {
  return RetrieveInt64(0);
}


//------------------------------------------------------------------------------


SqlSetCounter::SqlSetCounter(const Database &database) {
  Init(database.sqlite_db(),
       "UPDATE statistics SET value=:val WHERE counter=:counter;");
}


bool SqlSetCounter::BindCounter(const std::string &counter) {
  return BindText(2, counter);
}


bool SqlSetCounter::BindValue(const uint64_t value) {
  return BindInt64(1, value);
}

} // namespace catalog
