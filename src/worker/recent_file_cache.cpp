#include "recent_file_cache.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <uv.h>

#include "../../helper/common.h"
#include "../../log.h"

using std::chrono::minutes;
using std::chrono::steady_clock;
using std::chrono::time_point;
using std::endl;
using std::move;
using std::ostream;
using std::ostringstream;
using std::pair;
using std::queue;
using std::shared_ptr;
using std::static_pointer_cast;
using std::string;
using std::vector;

shared_ptr<StatResult> StatResult::at(string &&path, bool file_hint, bool directory_hint)
{
  struct stat path_stat
  {};

  if (lstat(path.c_str(), &path_stat) != 0) {
    errno_t stat_errno = errno;

    // Ignore lstat() errors on entries that:
    // (a) we aren't allowed to see
    // (b) are at paths with too many symlinks or looping symlinks
    // (c) have names that are too long
    // (d) have a path component that is (no longer) a directory
    // Log any other errno that we see.
    if (stat_errno != ENOENT && stat_errno != EACCES && stat_errno != ELOOP && stat_errno != ENAMETOOLONG
      && stat_errno != ENOTDIR) {
      LOGGER << "lstat(" << path << ") failed: " << strerror(stat_errno) << "." << endl;
    }

    EntryKind guessed_kind = KIND_UNKNOWN;
    if (file_hint && !directory_hint) guessed_kind = KIND_FILE;
    if (!file_hint && directory_hint) guessed_kind = KIND_DIRECTORY;

    return shared_ptr<StatResult>(new AbsentEntry(move(path), guessed_kind));
  }

  // Derive the entry kind from the lstat() results.
  EntryKind entry_kind = KIND_UNKNOWN;
  if ((path_stat.st_mode & S_IFREG) != 0) {
    entry_kind = KIND_FILE;
  }
  if ((path_stat.st_mode & S_IFDIR) != 0) {
    entry_kind = KIND_DIRECTORY;
  }

  return shared_ptr<StatResult>(new PresentEntry(move(path), entry_kind, path_stat.st_ino, path_stat.st_size));
}

bool StatResult::has_changed_from(const StatResult &other) const
{
  return entry_kind != other.entry_kind || path != other.path;
}

bool StatResult::could_be_rename_of(const StatResult &other) const
{
  return !kinds_are_different(entry_kind, other.entry_kind);
}

bool StatResult::update_for_rename(const std::string &from_dir_path, const std::string &to_dir_path)
{
  if (path.size() > from_dir_path.size() && path.rfind(from_dir_path, 0) == 0) {
    path = to_dir_path + path.substr(from_dir_path.size());
    return true;
  }

  return false;
}

const string &StatResult::get_path() const
{
  return path;
}

EntryKind StatResult::get_entry_kind() const
{
  return entry_kind;
}

ostream &operator<<(ostream &out, const StatResult &result)
{
  out << result.to_string(true);
  return out;
}

PresentEntry::PresentEntry(std::string &&path, EntryKind entry_kind, ino_t inode, off_t size) :
  StatResult(move(path), entry_kind),
  inode{inode},
  size{size},
  last_seen{steady_clock::now()}
{
  //
}

bool PresentEntry::is_present() const
{
  return true;
}

bool PresentEntry::has_changed_from(const StatResult &other) const
{
  if (StatResult::has_changed_from(other)) return true;
  if (other.is_absent()) return true;

  const auto &casted = static_cast<const PresentEntry &>(other);  // NOLINT
  return inode != casted.get_inode() || get_path() != casted.get_path();
}

bool PresentEntry::could_be_rename_of(const StatResult &other) const
{
  if (!StatResult::could_be_rename_of(other)) return false;
  if (other.is_absent()) return false;

  const auto &casted = static_cast<const PresentEntry &>(other);  // NOLINT
  return inode == casted.get_inode() && !kinds_are_different(get_entry_kind(), casted.get_entry_kind());
}

ino_t PresentEntry::get_inode() const
{
  return inode;
}

off_t PresentEntry::get_size() const
{
  return size;
}

const time_point<steady_clock> &PresentEntry::get_last_seen() const
{
  return last_seen;
}

string PresentEntry::to_string(bool verbose) const
{
  ostringstream result;

  result << "[present " << get_entry_kind();
  if (verbose) result << " (" << get_path() << ")";
  result << " inode=" << inode << " size=" << size << "]";

  return result.str();
}

bool AbsentEntry::is_present() const
{
  return false;
}

bool AbsentEntry::has_changed_from(const StatResult &other) const
{
  if (StatResult::has_changed_from(other)) return true;
  if (other.is_present()) return true;

  return false;
}

bool AbsentEntry::could_be_rename_of(const StatResult & /*other*/) const
{
  return false;
}

string AbsentEntry::to_string(bool verbose) const
{
  ostringstream result;

  result << "[absent " << get_entry_kind();
  if (verbose) result << " (" << get_path() << ")";
  result << "]";

  return result.str();
}

RecentFileCache::RecentFileCache(size_t maximum_size) : maximum_size{maximum_size}
{
  //
}

shared_ptr<StatResult> RecentFileCache::current_at_path(const string &path, bool file_hint, bool directory_hint)
{
  auto maybe_pending = pending.find(path);
  if (maybe_pending != pending.end()) {
    return maybe_pending->second;
  }

  shared_ptr<StatResult> stat_result = StatResult::at(string(path), file_hint, directory_hint);
  if (stat_result->is_present()) {
    pending.emplace(path, static_pointer_cast<PresentEntry>(stat_result));
  }
  return stat_result;
}

shared_ptr<StatResult> RecentFileCache::former_at_path(const string &path, bool file_hint, bool directory_hint)
{
  auto maybe = by_path.find(path);
  if (maybe == by_path.end()) {
    EntryKind kind = KIND_UNKNOWN;
    if (file_hint && !directory_hint) kind = KIND_FILE;
    if (!file_hint && directory_hint) kind = KIND_DIRECTORY;

    return shared_ptr<StatResult>(new AbsentEntry(string(path), kind));
  }

  return maybe->second;
}

void RecentFileCache::evict(const string &path)
{
  auto maybe = by_path.find(path);
  if (maybe != by_path.end()) {
    shared_ptr<PresentEntry> existing = maybe->second;

    auto range = by_timestamp.equal_range(existing->get_last_seen());
    auto to_erase = by_timestamp.end();
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second == existing) {
        to_erase = it;
      }
    }
    if (to_erase != by_timestamp.end()) {
      by_timestamp.erase(to_erase);
    }

    by_path.erase(maybe);
  }
}

void RecentFileCache::evict(const shared_ptr<PresentEntry> &entry)
{
  auto maybe = by_path.find(entry->get_path());
  if (maybe != by_path.end() && maybe->second == entry) {
    evict(entry->get_path());
  }
}

void RecentFileCache::update_for_rename(const string &from_dir_path, const string &to_dir_path)
{
  vector<pair<string, string>> renames;

  for (auto &each : by_path) {
    if (each.second->update_for_rename(from_dir_path, to_dir_path)) {
      renames.emplace_back(each.first, each.second->get_path());
    }
  }

  for (auto &rename : renames) {
    shared_ptr<PresentEntry> p = by_path[rename.first];
    by_path.erase(rename.first);
    by_path.emplace(rename.second, p);
  }
}

void RecentFileCache::apply()
{
  for (auto &pair : pending) {
    shared_ptr<PresentEntry> &present = pair.second;

    // Clear an existing entry at the same path if one exists
    evict(present->get_path());

    // Add the new PresentEntry
    by_path.emplace(present->get_path(), present);
    by_timestamp.emplace(present->get_last_seen(), present);
  }
  pending.clear();
}

void RecentFileCache::prune()
{
  if (by_path.size() <= maximum_size) {
    return;
  }
  size_t to_remove = by_path.size() - maximum_size;

  LOGGER << "Cache currently contains " << plural(by_path.size(), "entry", "entries") << ". Pruning triggered." << endl;

  auto last = by_timestamp.begin();
  for (size_t i = 0; i < to_remove && last != by_timestamp.end(); i++) {
    ++last;
  }

  for (auto it = by_timestamp.begin(); it != last; ++it) {
    shared_ptr<PresentEntry> entry = it->second;
    by_path.erase(entry->get_path());
  }
  by_timestamp.erase(by_timestamp.begin(), last);

  LOGGER << "Pruned " << plural(to_remove, "entry", "entries") << ". " << plural(by_path.size(), "entry", "entries")
         << " remain." << endl;
}

void RecentFileCache::prepopulate(const string &root, size_t max)
{
  size_t count = 0;
  size_t entries = 0;
  size_t bounded_max = max > maximum_size ? maximum_size : max;
  queue<string> next_roots;
  next_roots.push(root);

  while (count < bounded_max && !next_roots.empty()) {
    string current_root(next_roots.front());
    next_roots.pop();

    DIR *dir = opendir(current_root.c_str());
    if (dir == nullptr) {
      errno_t opendir_errno = errno;
      LOGGER << "Unable to open directory " << root << ": " << strerror(opendir_errno) << "." << endl;
      LOGGER << "Incompletely pre-populated cache with " << entries << " entries." << endl;
      return;
    }

    errno = 0;
    dirent *entry = readdir(dir);
    while (entry != nullptr) {
      string entry_name(entry->d_name, entry->d_namlen);

      if (entry_name != "." && entry_name != "..") {
        string entry_path(path_join(current_root, entry_name));

        bool file_hint = (entry->d_type & DT_REG) == DT_REG;
        bool dir_hint = (entry->d_type & DT_DIR) == DT_DIR;

        shared_ptr<StatResult> r = current_at_path(entry_path, file_hint, dir_hint);
        if (r->is_present()) {
          entries++;
          if (r->get_entry_kind() == KIND_DIRECTORY) next_roots.push(entry_path);
        }

        count++;
        if (count >= max) {
          LOGGER << "Incompletely pre-populated cache with " << entries << " entries." << endl;
          return;
        }
      }

      errno = 0;
      entry = readdir(dir);
    }
    errno_t readdir_errno = errno;
    if (readdir_errno != 0) {
      LOGGER << "Unable to read directory entry within " << root << ": " << strerror(readdir_errno) << "." << endl;
    }

    if (closedir(dir) != 0) {
      errno_t closedir_errno = errno;
      LOGGER << "Unable to close directory " << root << ": " << strerror(closedir_errno) << "." << endl;
    }
  }
  apply();

  LOGGER << "Pre-populated cache with " << entries << " entries." << endl;
}

void RecentFileCache::resize(size_t maximum_size)
{
  this->maximum_size = maximum_size;
  prune();
}
