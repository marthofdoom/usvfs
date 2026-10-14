/*
Userspace Virtual Filesystem

This file is part of usvfs.

usvfs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

usvfs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with usvfs. If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

// Pure logic and data structures of the metadata cache (metacache.cpp): path
// normalization, mask matching, directory record packing and the LRU store with
// its invalidation rules. No Windows or ntdll dependencies, so it can be unit
// tested.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace usvfs::metacache::logic
{

// ---------------------------------------------------------------------------
// paths

// upper-casing of non-ASCII characters (RtlUpcaseUnicodeChar in the DLL); when
// not set they are compared exactly, which can only cause misses
inline wchar_t (*g_upcaseHook)(wchar_t) = nullptr;

inline wchar_t foldChar(wchar_t c)
{
  if (c >= L'a' && c <= L'z')
    return static_cast<wchar_t>(c - L'a' + L'A');
  if (c == L'/')
    return L'\\';
  if (c >= 0x80 && g_upcaseHook)
    return g_upcaseHook(c);
  return c;
}

// upper case, '/' -> '\\', no trailing backslash (except for a bare root)
inline std::wstring normalizePath(std::wstring_view path)
{
  std::wstring out;
  out.reserve(path.size());
  for (wchar_t c : path)
    out.push_back(foldChar(c));
  while (out.size() > 1 && out.back() == L'\\' && !(out.size() >= 2 && out[out.size() - 2] == L':'))
    out.pop_back();
  return out;
}

// parent directory of a normalized path; empty if there is none
inline std::wstring parentPath(const std::wstring& normalized)
{
  const size_t pos = normalized.find_last_of(L'\\');
  if (pos == std::wstring::npos || pos == 0)
    return std::wstring();
  return normalized.substr(0, pos);
}

// true if `normalized` is `prefix` itself or lies below it
inline bool isUnderPrefix(const std::wstring& normalized, const std::wstring& prefix)
{
  if (prefix.empty() || normalized.size() < prefix.size())
    return false;
  if (normalized.compare(0, prefix.size(), prefix) != 0)
    return false;
  return normalized.size() == prefix.size() || normalized[prefix.size()] == L'\\' ||
         prefix.back() == L'\\';
}

// ---------------------------------------------------------------------------
// masks

// "" and "*" mean "everything" and are stored as "*". ("*.*" is deliberately
// NOT folded into it: at the NT level it requires a dot, and implementations
// differ in how they treat it.)
inline std::wstring normalizeMask(std::wstring_view mask)
{
  if (mask.empty() || mask == L"*")
    return L"*";
  std::wstring out;
  out.reserve(mask.size());
  for (wchar_t c : mask)
    out.push_back(foldChar(c));
  return out;
}

// DOS wildcards (< > ") have subtle semantics; those masks are not cached
inline bool isCacheableMask(std::wstring_view mask)
{
  return mask.find_first_of(L"<>\"") == std::wstring_view::npos;
}

inline bool isLiteralMask(std::wstring_view mask)
{
  return mask.find_first_of(L"*?") == std::wstring_view::npos;
}

// '*' matches any run of characters, '?' exactly one; case-insensitive (ASCII)
inline bool maskMatches(std::wstring_view pattern, std::wstring_view name)
{
  size_t p = 0, n = 0;
  size_t starP = std::wstring_view::npos, starN = 0;
  while (n < name.size()) {
    if (p < pattern.size() && pattern[p] == L'*') {
      starP = p++;
      starN = n;
    } else if (p < pattern.size() &&
               (pattern[p] == L'?' || foldChar(pattern[p]) == foldChar(name[n]))) {
      ++p;
      ++n;
    } else if (starP != std::wstring_view::npos) {
      p = starP + 1;
      n = ++starN;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == L'*')
    ++p;
  return p == pattern.size();
}

// ---------------------------------------------------------------------------
// directory record packing

constexpr size_t kRecordAlign = 8;

inline size_t alignedSize(size_t rawLength)
{
  return (rawLength + kRecordAlign - 1) & ~(kRecordAlign - 1);
}

// Number of records starting at `first` that fit in `bufferSize` bytes when
// packed with 8 byte alignment (the last one needs no padding). 0 means not
// even the first fits (STATUS_BUFFER_OVERFLOW).
inline size_t countFitting(const std::vector<size_t>& rawLengths, size_t first,
                           size_t bufferSize, bool singleEntry)
{
  size_t offset = 0;
  size_t count  = 0;
  for (size_t i = first; i < rawLengths.size(); ++i) {
    if (offset + rawLengths[i] > bufferSize)
      break;
    ++count;
    offset += alignedSize(rawLengths[i]);
    if (singleEntry)
      break;
  }
  return count;
}

// size of the data written for `count` records starting at `first`
// (information value of the call)
inline size_t packedSize(const std::vector<size_t>& rawLengths, size_t first, size_t count)
{
  if (count == 0)
    return 0;
  size_t offset = 0;
  for (size_t i = first; i + 1 < first + count; ++i)
    offset += alignedSize(rawLengths[i]);
  return offset + rawLengths[first + count - 1];
}

// ---------------------------------------------------------------------------
// store

struct Attr
{
  int32_t status   = 0;
  bool full        = false;  // allocation size / end of file are valid
  int64_t creation = 0;
  int64_t access   = 0;
  int64_t write    = 0;
  int64_t change   = 0;
  int64_t allocation = 0;
  int64_t endOfFile  = 0;
  uint32_t attributes = 0;
};

struct Listing
{
  std::vector<std::string> records;  // raw records, NextEntryOffset zeroed
  std::vector<size_t> rawLengths;
  std::vector<std::wstring> names;
  size_t bytes = 0;

  void add(std::string record, std::wstring name)
  {
    rawLengths.push_back(record.size());
    bytes += record.size() + name.size() * sizeof(wchar_t) + 64;
    records.push_back(std::move(record));
    names.push_back(std::move(name));
  }
};

struct StoreStats
{
  uint64_t attrHits = 0, attrNegativeHits = 0, attrMisses = 0;
  uint64_t listingHits = 0, listingMisses = 0, listingDerived = 0;
  uint64_t invalidations = 0, evictions = 0;
};

class Store
{
public:
  explicit Store(uint64_t capBytes) : m_cap(capBytes) {}

  // ---- attributes (keys are normalized paths) ----

  // a `full` entry satisfies both kinds of queries, a basic one only basic
  bool lookupAttr(const std::wstring& path, bool needFull, Attr& out)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_attrs.find(path);
    if (it == m_attrs.end() || (needFull && !it->second.attr.full &&
                                it->second.attr.status >= 0)) {
      ++m_stats.attrMisses;
      return false;
    }
    touch(it->second.lru);
    out = it->second.attr;
    if (out.status < 0) {
      ++m_stats.attrNegativeHits;
    } else {
      ++m_stats.attrHits;
    }
    return true;
  }

  bool hasListing(const std::wstring& dir, int cls, const std::wstring& mask)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto d = m_dirs.find(dir);
    if (d == m_dirs.end())
      return false;
    for (const ListSlot& slot : d->second) {
      if (slot.cls == cls && slot.mask == mask)
        return true;
    }
    return false;
  }

  bool hasAttr(const std::wstring& path)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_attrs.find(path) != m_attrs.end();
  }

  void putAttr(const std::wstring& path, const Attr& attr)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isBlocked(path))
      return;
    auto it = m_attrs.find(path);
    if (it != m_attrs.end()) {
      // keep a full entry rather than replacing it by a basic one
      if (it->second.attr.full && !attr.full && attr.status >= 0 &&
          it->second.attr.status >= 0) {
        touch(it->second.lru);
        return;
      }
      eraseAttr(it);
    }
    insertAttr(path, attr);
    evict();
  }

  // ---- directory listings ----

  std::shared_ptr<const Listing> lookupListing(const std::wstring& dir, int cls,
                                               const std::wstring& mask)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto d = m_dirs.find(dir);
    if (d != m_dirs.end()) {
      for (ListSlot& slot : d->second) {
        if (slot.cls == cls && slot.mask == mask) {
          touch(slot.lru);
          ++m_stats.listingHits;
          return slot.listing;
        }
      }
    }
    ++m_stats.listingMisses;
    return nullptr;
  }

  void putListing(const std::wstring& dir, int cls, const std::wstring& mask,
                  std::shared_ptr<const Listing> listing, bool derived = false)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isBlocked(dir) || m_volatileDirs.count(dir))
      return;
    std::vector<ListSlot>& slots = m_dirs[dir];
    for (ListSlot& slot : slots) {
      if (slot.cls == cls && slot.mask == mask)
        return;  // somebody was faster
    }
    ListSlot slot;
    slot.cls     = cls;
    slot.mask    = mask;
    slot.listing = std::move(listing);
    slot.lru     = addLru(Node{false, dir, cls, mask, slot.listing->bytes + dir.size() * 2 + 64});
    m_bytes += slot.lru->bytes;
    if (derived)
      ++m_stats.listingDerived;
    slots.push_back(std::move(slot));
    evict();
  }

  // ---- invalidation ----

  // a path was (or is about to be) written/created/deleted/renamed: forget
  // what we know about it and its parent directory, and never cache either
  // again
  void invalidatePath(const std::wstring& path)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stats.invalidations;
    markVolatile(path);
  }

  // a directory was renamed/removed: everything below it is stale too. Above
  // the size threshold the whole cache is dropped instead of scanning.
  void invalidateTree(const std::wstring& path)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stats.invalidations;
    markVolatile(path);

    if (m_attrs.size() + m_dirs.size() > kTreeScanLimit) {
      clearLocked();
      return;
    }
    for (auto it = m_attrs.begin(); it != m_attrs.end();) {
      if (isUnderPrefix(it->first, path)) {
        m_bytes -= std::min(m_bytes, it->second.lru->bytes);
        m_lru.erase(it->second.lru);
        it = m_attrs.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = m_dirs.begin(); it != m_dirs.end();) {
      if (isUnderPrefix(it->first, path)) {
        for (ListSlot& slot : it->second) {
          m_bytes -= std::min(m_bytes, slot.lru->bytes);
          m_lru.erase(slot.lru);
        }
        it = m_dirs.erase(it);
      } else {
        ++it;
      }
    }
  }

  // true if nothing under this path may be cached (volatile or excluded)
  bool isBlockedPath(const std::wstring& path)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_overflowed || isBlocked(path) || m_volatileDirs.count(path) != 0;
  }

  bool overflowed() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_overflowed;
  }

  bool isVolatile(const std::wstring& path)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return isBlocked(path) || m_volatilePaths.count(path) != 0 ||
           m_volatileDirs.count(path) != 0;
  }

  // nothing at or below this path is ever cached (e.g. the overwrite folder)
  void addNoCachePrefix(const std::wstring& prefix)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_noCache.begin(), m_noCache.end(), prefix) == m_noCache.end())
      m_noCache.push_back(prefix);
  }

  void clear()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    clearLocked();
  }

  // ---- introspection ----

  uint64_t bytes() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes;
  }

  bool nearlyFull() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes > m_cap - m_cap / 10;
  }

  size_t attrCount() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_attrs.size();
  }

  size_t listingCount() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t n = 0;
    for (const auto& d : m_dirs)
      n += d.second.size();
    return n;
  }

  StoreStats stats() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
  }

private:
  static constexpr size_t kTreeScanLimit = 200000;
  static constexpr size_t kMaxVolatile   = 200000;

  struct Node
  {
    bool isAttr = true;
    std::wstring key;  // path (attr) or directory (listing)
    int cls = 0;
    std::wstring mask;
    size_t bytes = 0;
  };
  using Lru = std::list<Node>;

  struct AttrSlot
  {
    Attr attr;
    Lru::iterator lru;
  };

  struct ListSlot
  {
    int cls = 0;
    std::wstring mask;
    std::shared_ptr<const Listing> listing;
    Lru::iterator lru;
  };

  Lru::iterator addLru(Node node)
  {
    m_lru.push_front(std::move(node));
    return m_lru.begin();
  }

  void touch(Lru::iterator it) { m_lru.splice(m_lru.begin(), m_lru, it); }

  bool isBlocked(const std::wstring& path) const
  {
    if (m_overflowed)
      return true;
    for (const std::wstring& prefix : m_noCache) {
      if (isUnderPrefix(path, prefix))
        return true;
    }
    return m_volatilePaths.count(path) != 0;
  }

  void insertAttr(const std::wstring& path, const Attr& attr)
  {
    AttrSlot slot;
    slot.attr = attr;
    slot.lru  = addLru(Node{true, path, 0, std::wstring(), path.size() * 2 + 160});
    m_bytes += slot.lru->bytes;
    m_attrs.emplace(path, std::move(slot));
  }

  void eraseAttr(std::unordered_map<std::wstring, AttrSlot>::iterator it)
  {
    m_bytes -= std::min(m_bytes, it->second.lru->bytes);
    m_lru.erase(it->second.lru);
    m_attrs.erase(it);
  }

  void eraseDir(const std::wstring& dir)
  {
    auto d = m_dirs.find(dir);
    if (d == m_dirs.end())
      return;
    for (ListSlot& slot : d->second) {
      m_bytes -= std::min(m_bytes, slot.lru->bytes);
      m_lru.erase(slot.lru);
    }
    m_dirs.erase(d);
  }

  void markVolatile(const std::wstring& path)
  {
    auto a = m_attrs.find(path);
    if (a != m_attrs.end())
      eraseAttr(a);
    eraseDir(path);

    const std::wstring parent = parentPath(path);
    if (!parent.empty()) {
      eraseDir(parent);
      // a directory's own times/size change when an entry is added or removed
      auto pa = m_attrs.find(parent);
      if (pa != m_attrs.end())
        eraseAttr(pa);
      m_volatileDirs.insert(parent);
      m_volatilePaths.insert(parent);
    }
    m_volatilePaths.insert(path);

    // The volatile sets cannot be forgotten without caching written paths again,
    // so when they grow too large caching is switched off for good.
    if (m_volatilePaths.size() + m_volatileDirs.size() > kMaxVolatile) {
      clearLocked();
      m_overflowed = true;
    }
  }

  void evict()
  {
    while (m_bytes > m_cap && !m_lru.empty()) {
      Node& victim = m_lru.back();
      ++m_stats.evictions;
      if (victim.isAttr) {
        auto a = m_attrs.find(victim.key);
        m_bytes -= std::min(m_bytes, victim.bytes);
        if (a != m_attrs.end())
          m_attrs.erase(a);
        m_lru.pop_back();
      } else {
        auto d = m_dirs.find(victim.key);
        m_bytes -= std::min(m_bytes, victim.bytes);
        if (d != m_dirs.end()) {
          auto& slots = d->second;
          slots.erase(std::remove_if(slots.begin(), slots.end(),
                                     [&](const ListSlot& s) {
                                       return s.cls == victim.cls && s.mask == victim.mask;
                                     }),
                      slots.end());
          if (slots.empty())
            m_dirs.erase(d);
        }
        m_lru.pop_back();
      }
    }
  }

  void clearLocked()
  {
    m_attrs.clear();
    m_dirs.clear();
    m_lru.clear();
    m_bytes = 0;
  }

  mutable std::mutex m_mutex;
  uint64_t m_cap;
  uint64_t m_bytes = 0;
  Lru m_lru;  // front = most recently used
  std::unordered_map<std::wstring, AttrSlot> m_attrs;
  std::unordered_map<std::wstring, std::vector<ListSlot>> m_dirs;
  std::unordered_set<std::wstring> m_volatilePaths;
  std::unordered_set<std::wstring> m_volatileDirs;
  std::vector<std::wstring> m_noCache;
  bool m_overflowed = false;
  StoreStats m_stats;
};

}  // namespace usvfs::metacache::logic
