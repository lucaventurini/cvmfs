/**
 * This file is part of the CernVM File System.
 *
 * Implements a socket interface to cvmfs.  This way commands can be send
 * to cvmfs.  When cvmfs is running, the socket
 * /var/cache/cvmfs2/$INSTANCE/cvmfs_io
 * is available for command input and reply messages, resp.
 *
 * Cvmfs comes with the cvmfs_talk script, that handles writing and reading the
 * socket.
 *
 * The talk module runs in a separate thread.
 */

#include "cvmfs_config.h"
#include "talk.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include <cassert>
#include <cstdlib>

#include <string>
#include <vector>

#include "platform.h"
#include "tracer.h"
#include "lru.h"
#include "cvmfs.h"
#include "util.h"
#include "logging.h"
#include "download.h"
#include "sqlite3-duplex.h"

using namespace std;  // NOLINT

namespace talk {

const unsigned kMaxCommandSize = 512;

string *cachedir_ = NULL;  /**< Stores the cache directory from cvmfs.
                                Pipe files will be created here. */
string *socket_path_ = NULL;  /**< $cache_dir/cvmfs_io */
int socket_fd_;
pthread_t thread_talk_;
bool spawned_;


static void Answer(const int con_fd, const string &msg) {
  (void)send(con_fd, &msg[0], msg.length(), MSG_NOSIGNAL);
}


static void AnswerStringList(const int con_fd, const vector<string> &list) {
  string list_str;
  for (unsigned i = 0; i < list.size(); ++i) {
    list_str += list[i] + "\n";
  }
  Answer(con_fd, list_str);
}


static void *MainTalk(void *data __attribute__((unused))) {
  LogCvmfs(kLogTalk, kLogDebug, "talk thread started");

  struct sockaddr_un remote;
  socklen_t socket_size = sizeof(remote);
  int con_fd = -1;
  while (true) {
    if (con_fd > 0) {
      shutdown(con_fd, SHUT_RDWR);
      close(con_fd);
    }
    if ((con_fd = accept(socket_fd_, (struct sockaddr *)&remote, &socket_size))
         < 0)
    {
      break;
    }

    char buf[kMaxCommandSize];
    if (recv(con_fd, buf, sizeof(buf), 0) > 0) {
      const string line = string(buf);

      if (line == "flush") {
        tracer::Flush();
        Answer(con_fd, "OK\n");
      } else if (line == "cache size") {
        if (lru::GetCapacity() == 0) {
          Answer(con_fd, "Cache is unmanaged\n");
        } else {
          uint64_t size_unpinned = lru::GetSize();
          uint64_t size_pinned = lru::GetSizePinned();
          const string size_str = "Current cache size is " +
            StringifyInt(size_unpinned / (1024*1024)) + "MB (" +
            StringifyInt(size_unpinned) + " Bytes),pinned: " +
            StringifyInt(size_pinned / (1024*1024)) + "MB (" +
            StringifyInt(size_pinned) + " Bytes)\n";
          Answer(con_fd, size_str);
        }
      } else if (line == "cache list") {
        if (lru::GetCapacity() == 0) {
          Answer(con_fd, "Cache is unmanaged\n");
        } else {
          vector<string> ls = lru::List();
          AnswerStringList(con_fd, ls);
        }
      } else if (line == "cache list pinned") {
        if (lru::GetCapacity() == 0) {
          Answer(con_fd, "Cache is unmanaged\n");
        } else {
          vector<string> ls_pinned = lru::ListPinned();
          AnswerStringList(con_fd, ls_pinned);
        }
      } else if (line == "cache list catalogs") {
        if (lru::GetCapacity() == 0) {
          Answer(con_fd, "Cache is unmanaged\n");
        } else {
          vector<string> ls_catalogs = lru::ListCatalogs();
          AnswerStringList(con_fd, ls_catalogs);
        }
      } else if (line.substr(0, 7) == "cleanup") {
        if (lru::GetCapacity() == 0) {
          Answer(con_fd, "Cache is unmanaged\n");
        } else {
          if (line.length() < 9) {
            Answer(con_fd, "Usage: cleanup <MB>\n");
          } else {
            const uint64_t size = String2Uint64(line.substr(8))*1024*1024;
            if (lru::Cleanup(size)) {
              Answer(con_fd, "OK\n");
            } else {
              Answer(con_fd, "Not fully cleaned "
                     "(there might be pinned chunks)\n");
            }
          }
        }
      } else if (line.substr(0, 10) == "clear file") {
        if (lru::GetCapacity() == 0) {
          Answer(con_fd, "Cache is unmanaged\n");
        } else {
          if (line.length() < 12) {
            Answer(con_fd, "Usage: clear file <path>\n");
          } else {
            const string path = line.substr(11);
            int retval = cvmfs::clear_file(path);
            switch (retval) {
              case 0:
                Answer(con_fd, "OK\n");
                break;
              case -ENOENT:
                Answer(con_fd, "No such file\n");
                break;
              case -EINVAL:
                Answer(con_fd, "Not a regular file\n");
                break;
              default:
                const string error_str = "Unknown error (" +
                                         StringifyInt(retval) + ")\n";
                Answer(con_fd, error_str);
                break;
            }
          }
        }
      } else if (line == "mountpoint") {
        Answer(con_fd, cvmfs::mountpoint + "\n");
      } else if (line == "remount") {
        // TODO: implement this!!
        int result = -1;
        //               int result = cvmfs::remount()
        if (result < 0) {
          Answer(con_fd, "Failed\n");
        }/* else if (result == 0) {
          Answer(con_fd, "Catalog up to date\n");
        } else if (result == 2) {
          Answer(con_fd, "Already draining out caches\n");
        } else {
          ostringstream str_max_cache_timeout;
          str_max_cache_timeout << "Remounting, draining out kernel caches for "
          << cvmfs::max_cache_timeout
          << " seconds..." << endl;
          Answer(con_fd, str_max_cache_timeout.str());
          sleep(cvmfs::max_cache_timeout);
        }
      } */
      // TODO
      /*else if (line == "revision") {
       catalog::lock();
       const uint64_t revision = catalog::get_revision();
       catalog::unlock();
       ostringstream revision_str;
       revision_str << revision;
       Answer(con_fd, revision_str.str() + "\n"); */
      } else if (line == "max ttl info") {
        const unsigned max_ttl = cvmfs::get_max_ttl();
        if (max_ttl == 0) {
          Answer(con_fd, "unset\n");
        } else {
          const string max_ttl_str = StringifyInt(cvmfs::get_max_ttl()) +
                                     " minutes\n";
          Answer(con_fd, max_ttl_str);
        }
      } else if (line.substr(0, 11) == "max ttl set") {
        if (line.length() < 13) {
          Answer(con_fd, "Usage: max ttl set <minutes>\n");
        } else {
          const unsigned max_ttl = String2Uint64(line.substr(12));
          cvmfs::set_max_ttl(max_ttl);
          Answer(con_fd, "OK\n");
        }
      } else if (line == "host info") {
        vector<string> host_chain;
        vector<int> rtt;
        unsigned active_host;

        download::GetHostInfo(&host_chain, &rtt, &active_host);
        string host_str;
        for (unsigned i = 0; i < host_chain.size(); ++i) {
          host_str += "  [" + StringifyInt(i) + "] " + host_chain[i] + " (";
          if (rtt[i] == -1)
            host_str += "unprobed";
          else if (rtt[i] == -2)
            host_str += "host down";
          else
            host_str += StringifyInt(rtt[i]) + " ms";
          host_str += ")\n";
        }
        host_str += "Active host " + StringifyInt(active_host) + ": " +
                    host_chain[active_host] + "\n";
        Answer(con_fd, host_str);
      } else if (line == "host probe") {
        download::ProbeHosts();
        Answer(con_fd, "OK\n");
      } else if (line == "host switch") {
        download::SwitchHost();
        Answer(con_fd, "OK\n");
      } else if (line.substr(0, 8) == "host set") {
        if (line.length() < 10) {
          Answer(con_fd, "Usage: host set <host list>\n");
        } else {
          const string hosts = line.substr(9);
          download::SetHostChain(hosts);
          Answer(con_fd, "OK\n");
        }
      } else if (line == "proxy info") {
        vector< vector<string> > proxy_chain;
        unsigned active_group;
        download::GetProxyInfo(&proxy_chain, &active_group);

        string proxy_str;
        if (proxy_chain.size()) {
          proxy_str += "Load-balance groups:\n";
          for (unsigned i = 0; i < proxy_chain.size(); ++i) {
            proxy_str += "[" + StringifyInt(i) + "] " +
                         JoinStrings(proxy_chain[i], ", ") + "\n";
          }
          proxy_str += "Active proxy: [" + StringifyInt(active_group) + "] " +
                       proxy_chain[active_group][0] + "\n";
        } else {
          proxy_str = "No proxies defined\n";
        }

        Answer(con_fd, proxy_str);
      } else if (line == "proxy rebalance") {
        download::RebalanceProxies();
        Answer(con_fd, "OK\n");
      } else if (line == "proxy group switch") {
        download::SwitchProxyGroup();
        Answer(con_fd, "OK\n");
      } else if (line.substr(0, 9) == "proxy set") {
        if (line.length() < 11) {
          Answer(con_fd, "Usage: proxy set <proxy list>\n");
        } else {
          const string proxies = line.substr(10);
          download::SetProxyChain(proxies);
          Answer(con_fd, "OK\n");
        }
      } else if (line == "timeout info") {
        unsigned timeout;
        unsigned timeout_direct;
        download::GetTimeout(&timeout, &timeout_direct);
        string timeout_str =  "Timeout with proxy: ";
        if (timeout)
          timeout_str += StringifyInt(timeout) + "s\n";
        else
          timeout_str += "no timeout\n";
        timeout_str += "Timeout without proxy: ";
        if (timeout_direct)
          timeout_str += StringifyInt(timeout_direct) + "s\n";
        else
          timeout_str += "no timeout\n";
        Answer(con_fd, timeout_str);
      } else if (line.substr(0, 11) == "timeout set") {
        if (line.length() < 13) {
          Answer(con_fd, "Usage: timeout set <proxy> <direct>\n");
        } else {
          uint64_t timeout;
          uint64_t timeout_direct;
          String2Uint64Pair(line.substr(12), &timeout, &timeout_direct);
          download::SetTimeout(timeout, timeout_direct);
          Answer(con_fd, "OK\n");
        }
      /*}
        TODO  else if (line == "open catalogs") {
       vector<string> prefix;
       vector<time_t> last_modified, expires;
       vector<unsigned int> inode_offsets;
       cvmfs::info_loaded_catalogs(&prefix, &last_modified, &expires, &inode_offsets);
       string result = "Prefix | Last Modified | Expires | inode offset\n";
       for (unsigned i = 0; i < prefix.size(); ++i) {
       result += ((prefix[i] == "") ? "/" : prefix[i]) + " | ";
       result += ((last_modified[i] == 0) ? "n/a" : localtime_ascii(last_modified[i], true)) + " | ";
       result += (expires[i] == 0) ? "n/a" : localtime_ascii(expires[i], true) + " | ";
       result += " " + inode_offsets[i];
       result += "\n";
       }

       Answer(con_fd, result);
       TODO
       } else if (line == "sqlite memory") {
       ostringstream result;
       int current = 0;
       int highwater = 0;
       int ncache = 0;
       int pcache = 0;
       int acache = 0;
       int cache_inserts = 0;
       int cache_replaces = 0;
       int cache_cleans = 0;
       int cache_hits = 0;
       int cache_misses = 0;
       int cert_hits = 0;
       int cert_misses = 0;

       catalog::lock();
       lru::lock();

       result << "File catalog memcache " << cvmfs::catalog_cache_memusage_bytes()/1024 << " KB" << endl;
       cvmfs::catalog_cache_memusage_slots(&pcache, &ncache, &acache,
       &cache_inserts, &cache_replaces, &cache_cleans, &cache_hits, &cache_misses,
       &cert_hits, &cert_misses);
       result << "File catalog memcache slots "
       << pcache << " positive, " << ncache << " negative / " << acache << " slots, "
       << cache_inserts << " inserts, " << cache_replaces << " replaces (not measured), " << cache_cleans << " cleans, "
       << cache_hits << " hits, " << cache_misses << " misses" << endl
       << "certificate disk cache hits/misses " << cert_hits << "/" << cert_misses << endl;


       sqlite3_status(SQLITE_STATUS_MALLOC_COUNT, &current, &highwater, 0);
       result << "Number of allocations " << current << endl;

       sqlite3_status(SQLITE_STATUS_MEMORY_USED, &current, &highwater, 0);
       result << "General purpose allocator " << current/1024 << " KB / "
       << highwater/1024 << " KB" << endl;

       sqlite3_status(SQLITE_STATUS_MALLOC_SIZE, &current, &highwater, 0);
       result << "Largest malloc " << highwater << " Bytes" << endl;

       sqlite3_status(SQLITE_STATUS_PAGECACHE_USED, &current, &highwater, 0);
       result << "Page cache allocations " << current << " / " << highwater << endl;

       sqlite3_status(SQLITE_STATUS_PAGECACHE_OVERFLOW, &current, &highwater, 0);
       result << "Page cache overflows " << current/1024 << " KB / " << highwater/1024 << " KB" << endl;

       sqlite3_status(SQLITE_STATUS_PAGECACHE_SIZE, &current, &highwater, 0);
       result << "Largest page cache allocation " << highwater << " Bytes" << endl;

       sqlite3_status(SQLITE_STATUS_SCRATCH_USED, &current, &highwater, 0);
       result << "Scratch allocations " << current << " / " << highwater << endl;

       sqlite3_status(SQLITE_STATUS_SCRATCH_OVERFLOW, &current, &highwater, 0);
       result << "Scratch overflows " << current << " / " << highwater << endl;

       sqlite3_status(SQLITE_STATUS_SCRATCH_SIZE, &current, &highwater, 0);
       result << "Largest scratch allocation " << highwater/1024 << " KB" << endl;

       result << catalog::get_db_memory_usage();
       result << lru::get_memory_usage();

       lru::unlock();
       catalog::unlock();

       Answer(con_fd, result.str());
       }
       } else if (line == "catalog tree") {
       Answer(con_fd, catalog_tree::show_tree()); */
      } else if (line == "pid") {
        const string pid_str = StringifyInt(cvmfs::pid) + "\n";
        Answer(con_fd, pid_str);
      } else if (line == "version") {
        Answer(con_fd, string(VERSION) + "\n");
      } else if (line == "version patchlevel") {
        Answer(con_fd, string(CVMFS_PATCH_LEVEL) + "\n");
      } else {
        Answer(con_fd, "What?\n");
      }
    }
  }

  return NULL;
}


/**
 * Init the socket.
 */
bool Init(const string &cachedir) {
  spawned_ = false;
  cachedir_ = new string(cachedir);

  struct sockaddr_un sock_addr;
  socket_path_ = new string(cachedir + "/cvmfs_io");
  if (socket_path_->length() >= sizeof(sock_addr.sun_path))
    return false;

  if ((socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    return false;

#ifndef __APPLE__
  // fchmod on a socket is not allowed under Mac OS X
  // using default here... (0770 in this case)
  if (fchmod(socket_fd_, 0660) != 0)
    return false;
#endif

  sock_addr.sun_family = AF_UNIX;
  strncpy(sock_addr.sun_path, socket_path_->c_str(),
          sizeof(sock_addr.sun_path));

  if (bind(socket_fd_, (struct sockaddr *)&sock_addr,
           sizeof(sock_addr.sun_family) + sizeof(sock_addr.sun_path)) < 0)
  {
    if ((errno == EADDRINUSE) && (unlink(socket_path_->c_str()) == 0)) {
      // Second try, perhaps the file was left over
      if (bind(socket_fd_, (struct sockaddr *)&sock_addr,
               sizeof(sock_addr.sun_family) + sizeof(sock_addr.sun_path)) < 0)
      {
        return false;
      }
      LogCvmfs(kLogTalk, kLogSyslog,
               "There was already a cvmfs_io file in cache directory.  "
               "Did we have a crash shutdown?");
    } else {
      return false;
    }
  }

  if (listen(socket_fd_, 1) < -1)
    return false;

  LogCvmfs(kLogTalk, kLogDebug, "socket created at %s", socket_path_->c_str());

  return true;
}


/**
 * Spawns the socket-dealing thread
 */
void Spawn() {
  int result;
  result = pthread_create(&thread_talk_, NULL, MainTalk, NULL);
  assert(result == 0);
  spawned_ = true;
}


/**
 * Terminates command-listener thread.  Removes socket.
 */
void Fini() {
  int result;
  result = unlink(socket_path_->c_str());
  if (result != 0) {
    LogCvmfs(kLogTalk, kLogSyslog,
             "Could not remove cvmfs_io socket from cache directory.");
  }

  delete cachedir_;
  delete socket_path_;
  cachedir_ = NULL;
  socket_path_ = NULL;

  shutdown(socket_fd_, SHUT_RDWR);
  close(socket_fd_);
  if (spawned_) pthread_join(thread_talk_, NULL);
  LogCvmfs(kLogTalk, kLogDebug, "talk thread stopped");
}

}  // namespace talk