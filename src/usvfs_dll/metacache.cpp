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
#include "metacache.h"
#include "metacache_logic.h"
#include "readcache.h"
#include "readcache_logic.h"
#include "hookcallcontext.h"
#include "hookcontext.h"
#include "redirectiontree.h"
#include "trace.h"

#include <stringcast.h>

#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

// Directory enumeration
// ---------------------
// usvfs merges directory listings itself (hook_NtQueryDirectoryFile and
// addNtSearchData in hooks/ntdll.cpp). Instead of duplicating that logic, only
// the *real* NtQueryDirectoryFile calls it makes are replaced by queryDirectory():
//  * the first call on a handle looks up a stored listing for {directory path,
//    information class, mask}; a hit makes the handle a "replay" cursor that
//    answers every call from memory, packing whole records into the caller's
//    buffer exactly like the system does (alignment, single entry, buffer
//    overflow, no more files);
//  * a miss records the raw records returned by the real calls and publishes
//    the listing when the enumeration ran to its end (an abandoned enumeration
//    is never published).
//  * a literal or simple wildcard mask can be derived from an existing "*"
//    listing of the same directory and class.
// The directory is identified by the path the handle was opened with, i.e. the
// virtual path; this is stable as long as the redirection tree is.

namespace usvfs::metacache
{

std::atomic<bool> g_enabled{false};

namespace
{

namespace chr = std::chrono;
using logic::Listing;

constexpr NTSTATUS kStatusObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034L);
constexpr NTSTATUS kStatusObjectPathNotFound = static_cast<NTSTATUS>(0xC000003AL);
constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

// ---- directory record layout ------------------------------------------------

bool classSupported(FILE_INFORMATION_CLASS c)
{
  switch (c) {
  case FileDirectoryInformation:
  case FileFullDirectoryInformation:
  case FileBothDirectoryInformation:
  case FileNamesInformation:
  case FileIdBothDirectoryInformation:
  case FileIdFullDirectoryInformation:
    return true;
  default:
    return false;
  }
}

// offsets of FileNameLength / FileName in the record of a class
void classLayout(FILE_INFORMATION_CLASS c, size_t& lengthOffset, size_t& nameOffset)
{
  switch (c) {
  case FileDirectoryInformation:
    lengthOffset = offsetof(FILE_DIRECTORY_INFORMATION, FileNameLength);
    nameOffset   = offsetof(FILE_DIRECTORY_INFORMATION, FileName);
    break;
  case FileFullDirectoryInformation:
    lengthOffset = offsetof(FILE_FULL_DIR_INFORMATION, FileNameLength);
    nameOffset   = offsetof(FILE_FULL_DIR_INFORMATION, FileName);
    break;
  case FileBothDirectoryInformation:
    lengthOffset = offsetof(FILE_BOTH_DIR_INFORMATION, FileNameLength);
    nameOffset   = offsetof(FILE_BOTH_DIR_INFORMATION, FileName);
    break;
  case FileNamesInformation:
    lengthOffset = offsetof(FILE_NAMES_INFORMATION, FileNameLength);
    nameOffset   = offsetof(FILE_NAMES_INFORMATION, FileName);
    break;
  case FileIdBothDirectoryInformation:
    lengthOffset = offsetof(FILE_ID_BOTH_DIR_INFORMATION, FileNameLength);
    nameOffset   = offsetof(FILE_ID_BOTH_DIR_INFORMATION, FileName);
    break;
  default:  // FileIdFullDirectoryInformation
    lengthOffset = offsetof(FILE_ID_FULL_DIR_INFORMATION, FileNameLength);
    nameOffset   = offsetof(FILE_ID_FULL_DIR_INFORMATION, FileName);
    break;
  }
}

// splits the data returned by one call into records; false if it doesn't parse
bool parseRecords(const unsigned char* data, size_t information,
                  FILE_INFORMATION_CLASS cls, Listing& out)
{
  size_t lengthOffset = 0, nameOffset = 0;
  classLayout(cls, lengthOffset, nameOffset);

  size_t off = 0;
  while (off < information) {
    if (off + nameOffset > information)
      return false;
    uint32_t next = 0, nameLength = 0;
    std::memcpy(&next, data + off, sizeof(next));
    std::memcpy(&nameLength, data + off + lengthOffset, sizeof(nameLength));
    const size_t raw = nameOffset + nameLength;
    if (off + raw > information)
      return false;

    std::string record(reinterpret_cast<const char*>(data + off), raw);
    std::memset(record.data(), 0, sizeof(uint32_t));  // NextEntryOffset

    std::wstring name(reinterpret_cast<const wchar_t*>(data + off + nameOffset),
                      nameLength / sizeof(wchar_t));
    out.add(std::move(record), std::move(name));

    if (next == 0)
      return true;
    if (next < raw)
      return false;
    off += next;
  }
  return true;
}

// ---- state ------------------------------------------------------------------

struct Cursor
{
  enum Mode { kReal, kReplay, kRecord } mode = kReal;
  FILE_INFORMATION_CLASS cls = FileDirectoryInformation;
  std::wstring dir;
  std::wstring mask;
  std::shared_ptr<const Listing> listing;  // replay
  size_t next = 0;
  uint64_t calls = 0;
  std::shared_ptr<Listing> building;  // record
  uint64_t generation = 0;            // store generation when the recording began
  bool buildOk = true;
};

// small pool of background threads with hooks blocked; the pre-warm uses it and
// other prefetching could be queued here as well
class BgPool
{
public:
  bool start(int threads)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started)
      return !m_threads.empty();
    m_started = true;
    try {
      for (int i = 0; i < threads; ++i) {
        m_threads.emplace_back([this] { run(); });
        m_alive.fetch_add(1);
      }
    } catch (...) {
    }
    return !m_threads.empty();
  }

  void submit(std::function<void()> task)
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_queue.push_back(std::move(task));
    }
    m_cv.notify_one();
  }

  // asks the threads to finish; waits a bounded time, detaches stragglers
  void stop()
  {
    m_stop.store(true);
    { std::lock_guard<std::mutex> lock(m_mutex); }
    m_cv.notify_all();
    const auto deadline = chr::steady_clock::now() + chr::seconds(2);
    while (m_alive.load() > 0 && chr::steady_clock::now() < deadline)
      Sleep(2);
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool clean = m_alive.load() == 0;
    for (std::thread& t : m_threads) {
      if (!t.joinable())
        continue;
      if (clean) {
        t.join();
      } else {
        t.detach();
      }
    }
    m_threads.clear();
  }

  bool stopping() const { return m_stop.load(std::memory_order_relaxed); }

private:
  void run()
  {
    HookCallContext blockHooks(MutExHookGroup::ALL_GROUPS);
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_stop.load() || !m_queue.empty(); });
        if (m_stop.load())
          break;
        task = std::move(m_queue.front());
        m_queue.pop_front();
      }
      try {
        task();
      } catch (...) {
      }
    }
    m_alive.fetch_sub(1);
  }

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<std::function<void()>> m_queue;
  std::vector<std::thread> m_threads;
  bool m_started = false;
  std::atomic<bool> m_stop{false};
  std::atomic<int> m_alive{0};
};

uint64_t nowUs()
{
  static const LONGLONG freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? f.QuadPart : 1;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<uint64_t>(c.QuadPart * 1000000 / freq);
}

wchar_t upcaseNt(wchar_t c)
{
  using Fn = WCHAR(NTAPI*)(WCHAR);
  static const Fn fn = reinterpret_cast<Fn>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlUpcaseUnicodeChar"));
  return fn ? static_cast<wchar_t>(fn(c)) : c;
}

struct State
{
  explicit State(uint64_t cap) : store(cap) {}

  logic::Store store;
  BgPool pool;

  std::mutex cursorsLock;
  std::unordered_map<HANDLE, std::shared_ptr<Cursor>> cursors;

  std::atomic<uint64_t> dirReplayed{0};   // enumerations answered from memory
  std::atomic<uint64_t> dirReplayCalls{0};
  std::atomic<uint64_t> dirRecorded{0};   // enumerations that filled a listing
  std::atomic<uint64_t> dirRealOnly{0};   // enumerations that could not be cached
  std::atomic<uint64_t> dirClassCount[40]{};

  // pre-warm
  bool prewarm = false;
  int prewarmThreads = 4;
  std::vector<int> prewarmClasses{FileBothDirectoryInformation};  // the game only uses class 3
  std::atomic<int64_t> prewarmPending{0};
  std::atomic<uint64_t> prewarmDirs{0};
  std::atomic<uint64_t> prewarmFiles{0};
  std::atomic<uint64_t> prewarmListings{0};
  std::atomic<uint64_t> prewarmAttrStored{0};
  std::atomic<uint64_t> prewarmListingBytes{0};
  std::atomic<int> diagDirLookups{0};
  std::atomic<int> diagPrewarmStores{0};
  std::atomic<int> diagAttrMisses{0};
  std::atomic<int> diagEnumRejects{0};
  std::atomic<uint64_t> prewarmLockWaitUs{0};  // time spent waiting for the context lock
  std::atomic<uint64_t> prewarmLockWaits{0};
  std::atomic<int> prewarmLimit{4};            // concurrently working pre-warm jobs
  std::atomic<int> prewarmRunning{0};
  std::mutex gateMutex;
  std::mutex realDirsMutex;
  std::unordered_set<std::string> realDirs;  // real directories queued for pre-listing
  std::condition_variable gateCv;
  std::atomic<uint64_t> prewarmStart{0};
  std::atomic<bool> prewarmDone{false};
  std::thread frontThread;
  std::atomic<bool> frontRunning{false};
  bool frontPrewarm = false;  // USVFS_METACACHE_FRONT_PREWARM (game process only)
  int frontPrewarmSec = 0;    // optional time budget of the front pre-warm (0 = none)
  int frontPrewarmThreads = 4;
  std::atomic<uint64_t> frontAttrSynth{0};  // attr front entries synthesized by the main pre-warm
  std::atomic<uint64_t> frontDirsDone{0};
  std::atomic<uint64_t> frontListsDone{0};
  std::atomic<uint64_t> frontStartTick{0};
  std::atomic<bool> prewarmCapped{false};

  std::atomic<uint64_t> tick{0};
  std::atomic<uint64_t> lastLogTick{0};
  std::atomic<uint64_t> lastActivity{0};
};

// intentionally leaked (detached threads may outlive static destruction)
State* g = nullptr;

std::string narrow(const std::wstring& w)
{
  try {
    return shared::string_cast<std::string>(w.c_str(), shared::CodePage::UTF8);
  } catch (const std::exception&) {
    return std::string("?");
  }
}

// the first few lookups and stores are logged so that keys of the game's queries
// and of the pre-warm can be compared
template <typename F>
void diag(std::atomic<int>& counter, F&& f)
{
  if (counter.fetch_add(1, std::memory_order_relaxed) < 20) {
    if (auto logger = spdlog::get("usvfs"))
      f(*logger);
  }
}

struct FrontEntry
{
  uint64_t gen = 0;
  FrontResult result;
};

// 128-bit hash of a front key: the entry stores 16 bytes instead of the path
// string (a collision of two different live paths is ~2^-100 per pair)
struct FrontKey
{
  uint64_t a = 0, b = 0;
  bool operator==(const FrontKey& o) const { return a == o.a && b == o.b; }
};
struct FrontKeyHash
{
  size_t operator()(const FrontKey& k) const { return static_cast<size_t>(k.a); }
};
FrontKey makeFrontKey(const std::wstring& s)
{
  uint64_t a = 1469598103934665603ull, b = 0x9E3779B97F4A7C15ull;
  for (wchar_t c : s) {
    a = (a ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    b = (b + static_cast<uint16_t>(c) + 0x632BE59BD9B4E019ull) * 0xFF51AFD7ED558CCDull;
    b ^= b >> 29;
  }
  return FrontKey{a, b ^ (static_cast<uint64_t>(s.size()) << 48)};
}

struct FrontState
{
  std::shared_mutex mutex;
  std::unordered_map<FrontKey, FrontEntry, FrontKeyHash> entries;  // hash of key + kind char
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> misses{0};
  // diagnostics: misses of a key seen before (bloom-ish, 2 hashes) vs first time
  std::atomic<uint64_t> missesFirst{0};
  std::atomic<uint64_t> missesRepeat{0};
  static constexpr size_t kSeenBits = size_t(1) << 23;
  std::unique_ptr<std::atomic<uint64_t>[]> seen{new std::atomic<uint64_t>[kSeenBits / 64]()};
  size_t maxEntries = 100000;  // set from the memory cap in init()
};

FrontState* gFront = nullptr;  // allocated in init() when enabled, never freed

// ---- enumeration front cache ----

// WIN32_FIND_DATAW records without the 592-byte fixed size: a small header and
// the two names at their real length
class PackedFinds
{
public:
  size_t size() const { return m_offs.size(); }
  bool empty() const { return m_offs.empty(); }
  size_t capacityBytes() const { return m_blob.capacity() + m_offs.capacity() * sizeof(uint32_t) + m_byHash.size() * 48; }

  void push_back(const WIN32_FIND_DATAW& fd)
  {
    Head h;
    h.attrs = fd.dwFileAttributes;
    h.res0  = fd.dwReserved0;
    h.res1  = fd.dwReserved1;
    h.ct    = fd.ftCreationTime;
    h.at    = fd.ftLastAccessTime;
    h.wt    = fd.ftLastWriteTime;
    h.sizeHigh = fd.nFileSizeHigh;
    h.sizeLow  = fd.nFileSizeLow;
    h.nl = static_cast<uint16_t>(wcsnlen(fd.cFileName, 260));
    h.al = static_cast<uint16_t>(wcsnlen(fd.cAlternateFileName, 14));
    const uint32_t idx = static_cast<uint32_t>(m_offs.size());
    m_byHash.emplace(logic::Listing::hashFolded(std::wstring_view(fd.cFileName, h.nl), true), idx);
    if (h.al != 0)
      m_byHash.emplace(logic::Listing::hashFolded(std::wstring_view(fd.cAlternateFileName, h.al), true), idx);
    m_offs.push_back(static_cast<uint32_t>(m_blob.size()));
    m_blob.append(reinterpret_cast<const char*>(&h), sizeof(h));
    m_blob.append(reinterpret_cast<const char*>(fd.cFileName), h.nl * sizeof(wchar_t));
    m_blob.append(reinterpret_cast<const char*>(fd.cAlternateFileName), h.al * sizeof(wchar_t));
  }

  WIN32_FIND_DATAW operator[](size_t i) const
  {
    Head h;
    std::memcpy(&h, m_blob.data() + m_offs[i], sizeof(h));
    WIN32_FIND_DATAW fd;
    std::memset(&fd, 0, sizeof(fd));
    fd.dwFileAttributes = h.attrs;
    fd.dwReserved0      = h.res0;
    fd.dwReserved1      = h.res1;
    fd.ftCreationTime   = h.ct;
    fd.ftLastAccessTime = h.at;
    fd.ftLastWriteTime  = h.wt;
    fd.nFileSizeHigh    = h.sizeHigh;
    fd.nFileSizeLow     = h.sizeLow;
    const char* p = m_blob.data() + m_offs[i] + sizeof(h);
    std::memcpy(fd.cFileName, p, h.nl * sizeof(wchar_t));
    std::memcpy(fd.cAlternateFileName, p + h.nl * sizeof(wchar_t), h.al * sizeof(wchar_t));
    return fd;
  }

  // indices (ascending) of the entries whose long or short name equals
  // `foldedName` (folded with foldChar); compares on the packed names
  std::vector<uint32_t> findLiteral(const std::wstring& foldedName) const
  {
    std::vector<uint32_t> out;
    auto range = m_byHash.equal_range(logic::Listing::hashFolded(foldedName, false));
    for (auto it = range.first; it != range.second; ++it) {
      const uint32_t i = it->second;
      Head h;
      std::memcpy(&h, m_blob.data() + m_offs[i], sizeof(h));
      const char* p = m_blob.data() + m_offs[i] + sizeof(h);
      if (nameEquals(p, h.nl, foldedName) ||
          (h.al != 0 && nameEquals(p + h.nl * sizeof(wchar_t), h.al, foldedName))) {
        if (out.empty() || out.back() != i)
          out.push_back(i);
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  void shrink_to_fit()
  {
    m_blob.shrink_to_fit();
    m_offs.shrink_to_fit();
  }

private:
  static bool nameEquals(const char* raw, size_t n, const std::wstring& folded)
  {
    if (n != folded.size())
      return false;
    for (size_t k = 0; k < n; ++k) {
      wchar_t c;
      std::memcpy(&c, raw + k * sizeof(wchar_t), sizeof(c));
      if (logic::foldChar(c) != folded[k])
        return false;
    }
    return true;
  }

  struct Head
  {
    DWORD attrs, res0, res1, sizeHigh, sizeLow;
    FILETIME ct, at, wt;
    uint16_t nl, al;
  };
  std::string m_blob;
  std::vector<uint32_t> m_offs;
  std::unordered_multimap<uint64_t, uint32_t> m_byHash;  // folded long/short name -> index
};

struct EnumListing
{
  bool failed = false;
  DWORD err   = 0;
  PackedFinds entries;
  size_t bytes() const { return sizeof(*this) + entries.capacityBytes() + 128; }
};

struct EnumSearch
{
  // replay
  std::shared_ptr<const EnumListing> replay;
  size_t next = 0;
  // recording
  std::wstring key;
  std::wstring dirNt;
  uint64_t gen = 0;
  std::shared_ptr<EnumListing> building;
};

struct EnumState
{
  std::mutex mutex;
  struct Stored
  {
    uint64_t gen = 0;
    std::shared_ptr<const EnumListing> listing;
  };
  std::unordered_map<std::wstring, Stored> listings;
  std::unordered_map<HANDLE, std::shared_ptr<EnumSearch>> searches;
  size_t bytes    = 0;
  size_t maxBytes = 0;
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> misses{0};
  std::atomic<uint64_t> records{0};
  // diagnostics: rejections by reason (see enumFirst), key kinds, unfinished searches
  std::atomic<uint64_t> rejects[64]{};
  std::atomic<uint64_t> keyedWild{0};
  std::atomic<uint64_t> keyedLiteral{0};
  std::atomic<uint64_t> unfinished{0};
  std::atomic<uint64_t> derived{0};
  static constexpr size_t kMaxEntries = 50000;  // per listing
};

EnumState* gEnum = nullptr;  // allocated in init() when enabled, never freed

std::wstring frontCwd(const wchar_t* name)
{
  std::wstring cwd;
  if (logic::frontIsDriveAbs(name) || name[0] == 0)
    return cwd;
  wchar_t buf[MAX_PATH + 1];
  const DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
  if (n > 0 && n < MAX_PATH)
    cwd.assign(buf, n);
  return cwd;
}

uint64_t activity()
{
  const logic::StoreStats s = g->store.stats();
  return (gEnum ? gEnum->hits.load(std::memory_order_relaxed) +
                       gEnum->misses.load(std::memory_order_relaxed)
                 : 0) +
         (gFront ? gFront->hits.load(std::memory_order_relaxed) +
                       gFront->misses.load(std::memory_order_relaxed)
                 : 0) +
         s.attrHits + s.attrMisses + s.attrNegativeHits + s.invalidations +
         g->dirReplayed.load(std::memory_order_relaxed) +
         g->dirRecorded.load(std::memory_order_relaxed) +
         g->prewarmFiles.load(std::memory_order_relaxed);
}

void logSummary(const char* prefix)
{
  auto logger = spdlog::get("usvfs");
  if (!logger)
    return;
  const logic::StoreStats s = g->store.stats();
  auto ld = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };

  std::string classes;
  for (int i = 0; i < 40; ++i) {
    if (ld(g->dirClassCount[i]) != 0)
      classes += std::format(" {}={}", i, ld(g->dirClassCount[i]));
  }

  {
    uint64_t ab = 0, lb = 0, le = 0;
    g->store.breakdown(ab, lb, le);
    const uint64_t ac = g->store.attrCount();
    logger->info("{}: store memory={}B: attr {}B for {} entries ({} B/entry), listings {}B for {} "
                 "listings / {} entries ({} B/entry)",
                 prefix, g->store.bytes(), ab, ac, ac ? ab / ac : 0, lb, g->store.listingCount(), le,
                 le ? lb / le : 0);
  }
  if (gFront) {
    logger->info("{}: attr front misses first-time={} repeat={}", prefix,
                 ld(gFront->missesFirst), ld(gFront->missesRepeat));
  }
  if (gEnum) {
    std::string rj;
    for (int i = 0; i < 64; ++i) {
      if (ld(gEnum->rejects[i]) != 0)
        rj += std::format(" {}={}", i, ld(gEnum->rejects[i]));
    }
    logger->info("{}: enum front rejections by reason[{} ] (10 empty,11 level,12 flags,13 drive-rel,"
                 "14 bare-mask-no-cwd,15 mask empty/long,16 mask bad char,20+k dir key reject k "
                 "(2 empty,3 UNC/device/rooted,4 drive-rel,5 rel-no-cwd,6 bad segment,7 bad char/wildcard,"
                 "8 trailing sep),30 no data,31 filter,32 searchOp) keyed wildcard={} literal={} "
                 "searches closed unfinished={} derived-from-star hits={}",
                 prefix, rj, ld(gEnum->keyedWild), ld(gEnum->keyedLiteral), ld(gEnum->unfinished),
                 ld(gEnum->derived));
  }
  logger->info(
      "{}: attr hits={} negativeHits={} misses={} | dir enumerations replayed={} "
      "(calls {}) recorded={} uncached={} listingsDerived={} classes[{} ] | "
      "invalidations={} evictions={} | entries attr={} listings={} memory={}B{} | front hits={} misses={} | enum front hits={} misses={} records={} memory={}B",
      prefix, s.attrHits, s.attrNegativeHits, s.attrMisses, ld(g->dirReplayed),
      ld(g->dirReplayCalls), ld(g->dirRecorded), ld(g->dirRealOnly), s.listingDerived,
      classes, s.invalidations, s.evictions, g->store.attrCount(),
      g->store.listingCount(), g->store.bytes(),
      g->store.overflowed() ? " (switched off: too many written paths)" : "",
      gFront ? gFront->hits.load(std::memory_order_relaxed) : 0,
      gFront ? gFront->misses.load(std::memory_order_relaxed) : 0,
      gEnum ? gEnum->hits.load(std::memory_order_relaxed) : 0,
      gEnum ? gEnum->misses.load(std::memory_order_relaxed) : 0,
      gEnum ? gEnum->records.load(std::memory_order_relaxed) : 0,
      gEnum ? gEnum->bytes : 0);
}

void maybeLog()
{
  const uint64_t now = GetTickCount64();
  uint64_t last      = g->lastLogTick.load(std::memory_order_relaxed);
  if (now - last < 30000)
    return;
  const uint64_t act = activity();
  if (act == g->lastActivity.load(std::memory_order_relaxed)) {
    g->lastLogTick.store(now, std::memory_order_relaxed);
    return;
  }
  if (!g->lastLogTick.compare_exchange_strong(last, now))
    return;
  g->lastActivity.store(act, std::memory_order_relaxed);
  try {
    logSummary("metacache");
  } catch (...) {
  }
}

inline void tick()
{
  if ((g->tick.fetch_add(1, std::memory_order_relaxed) & 0xFFF) == 0)
    maybeLog();
}

bool cacheableStatus(NTSTATUS s)
{
  return s == STATUS_SUCCESS || s == kStatusObjectNameNotFound ||
         s == kStatusObjectPathNotFound;
}

// ---- attribute conversion -----------------------------------------------------

logic::Attr toAttr(NTSTATUS status, const FILE_BASIC_INFORMATION* b)
{
  logic::Attr a;
  a.status = status;
  if (status == STATUS_SUCCESS && b) {
    a.full       = false;
    a.creation   = b->CreationTime.QuadPart;
    a.access     = b->LastAccessTime.QuadPart;
    a.write      = b->LastWriteTime.QuadPart;
    a.change     = b->ChangeTime.QuadPart;
    a.attributes = b->FileAttributes;
  }
  return a;
}

logic::Attr toAttr(NTSTATUS status, const FILE_NETWORK_OPEN_INFORMATION* n)
{
  logic::Attr a;
  a.status = status;
  if (status == STATUS_SUCCESS && n) {
    a.full       = true;
    a.creation   = n->CreationTime.QuadPart;
    a.access     = n->LastAccessTime.QuadPart;
    a.write      = n->LastWriteTime.QuadPart;
    a.change     = n->ChangeTime.QuadPart;
    a.allocation = n->AllocationSize.QuadPart;
    a.endOfFile  = n->EndOfFile.QuadPart;
    a.attributes = n->FileAttributes;
  }
  return a;
}

// ---- directory enumeration ----------------------------------------------------

std::shared_ptr<Cursor> findCursor(HANDLE h)
{
  std::lock_guard<std::mutex> lock(g->cursorsLock);
  auto it = g->cursors.find(h);
  return it == g->cursors.end() ? nullptr : it->second;
}

NTSTATUS realQuery(HANDLE h, PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length,
                   FILE_INFORMATION_CLASS cls, BOOLEAN single, PUNICODE_STRING mask)
{
  return ::NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, iosb, buffer, length,
                                cls, single, mask, FALSE);
}

// listing for a mask derived from the complete "*" listing of the directory
std::shared_ptr<const Listing> deriveListing(const std::wstring& dir, int cls,
                                             const std::wstring& mask)
{
  if (mask == L"*" || mask == L"*.*" || (!mask.empty() && mask.back() == L'.'))
    return nullptr;
  std::shared_ptr<const Listing> all = g->store.lookupListing(dir, cls, L"*");
  if (!all)
    return nullptr;

  auto derived = std::make_shared<Listing>();
  if (logic::isLiteralMask(mask)) {
    // a single file: by far the most common query. Not stored (there is one per
    // file); the name index makes it cheap to derive again.
    for (uint32_t i : all->findLiteral(mask))
      derived->add(all->records[i], all->names[i]);
    g->store.noteDerived();
    return derived;
  }

  for (size_t i = 0; i < all->records.size(); ++i) {
    if (logic::maskMatches(mask, all->names[i]))
      derived->add(all->records[i], all->names[i]);
  }
  g->store.putListing(dir, cls, mask, derived, true);
  return derived;
}

NTSTATUS replay(Cursor& c, PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length,
                BOOLEAN single)
{
  g->dirReplayCalls.fetch_add(1, std::memory_order_relaxed);
  const Listing& l = *c.listing;
  ++c.calls;

  auto finish = [&](NTSTATUS status, ULONG_PTR info) -> NTSTATUS {
    IO_STATUS_BLOCK local;
    std::memset(&local, 0, sizeof(local));
    local.Status      = status;
    local.Information = info;
    if (!readcache::guardedCopy(iosb, &local, sizeof(local)))
      return STATUS_ACCESS_VIOLATION;
    return status;
  };

  size_t lengthOffset = 0, nameOffset = 0;
  classLayout(c.cls, lengthOffset, nameOffset);
  if (length < nameOffset)
    return finish(kStatusInfoLengthMismatch, 0);

  if (c.next >= l.records.size()) {
    // first call without any match: no such file, afterwards: no more files
    return finish(c.calls == 1 ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES, 0);
  }

  const size_t n = logic::countFitting(l.rawLengths, c.next, length, single != FALSE);
  if (n == 0)
    return finish(STATUS_BUFFER_OVERFLOW, 0);

  const size_t info = logic::packedSize(l.rawLengths, c.next, n);
  std::string out(info, '\0');
  size_t off = 0;
  for (size_t i = 0; i < n; ++i) {
    const std::string_view rec = l.records[c.next + i];
    std::memcpy(out.data() + off, rec.data(), rec.size());
    const uint32_t next = (i + 1 < n) ? static_cast<uint32_t>(logic::alignedSize(rec.size())) : 0;
    std::memcpy(out.data() + off, &next, sizeof(next));
    off += logic::alignedSize(rec.size());
  }

  if (!readcache::guardedCopy(buffer, out.data(), out.size()))
    return finish(STATUS_ACCESS_VIOLATION, 0);

  c.next += n;
  return finish(STATUS_SUCCESS, info);
}

// ---- pre-warm ------------------------------------------------------------------

struct Child
{
  std::string name;
  bool dir   = false;
  bool dummy = false;
  bool file  = false;  // see listChildren
  std::string link;    // real target, empty if none
};

std::wstring toNtPath(const std::string& utf8Path)
{
  std::wstring w = shared::string_cast<std::wstring>(utf8Path, shared::CodePage::UTF8);
  std::replace(w.begin(), w.end(), L'/', L'\\');
  return LR"(\??\)" + w;
}

HANDLE openDirectory(const std::wstring& ntPath)
{
  UNICODE_STRING name;
  name.Buffer        = const_cast<PWSTR>(ntPath.c_str());
  name.Length        = static_cast<USHORT>(ntPath.size() * sizeof(WCHAR));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attrs;
  std::memset(&attrs, 0, sizeof(attrs));
  attrs.Length     = sizeof(attrs);
  attrs.ObjectName = &name;
  attrs.Attributes = 0x40;  // OBJ_CASE_INSENSITIVE

  HANDLE h = nullptr;
  IO_STATUS_BLOCK iosb;
  std::memset(&iosb, 0, sizeof(iosb));
  const NTSTATUS st = ::NtCreateFile(
      &h, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attrs, &iosb,
      nullptr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      0x00000001 /* FILE_OPEN */,
      FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT,
      nullptr, 0);
  return (st < 0) ? nullptr : h;
}

// Stores the real file's attributes in the store; `out` (optional) receives them
// when the file exists (also if they were already stored: it is queried again).
bool prewarmAttr(const std::wstring& realNtPath, logic::Attr* out = nullptr)
{
  const std::wstring key = makeKey(realNtPath);
  if (key.empty() || (!out && g->store.hasAttr(key)))
    return false;

  UNICODE_STRING name;
  name.Buffer        = const_cast<PWSTR>(realNtPath.c_str());
  name.Length        = static_cast<USHORT>(realNtPath.size() * sizeof(WCHAR));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attrs;
  std::memset(&attrs, 0, sizeof(attrs));
  attrs.Length     = sizeof(attrs);
  attrs.ObjectName = &name;
  attrs.Attributes = 0x40;

  FILE_NETWORK_OPEN_INFORMATION info;
  std::memset(&info, 0, sizeof(info));
  const NTSTATUS st = ::NtQueryFullAttributesFile(&attrs, &info);
  bool ok = false;
  if (cacheableStatus(st)) {
    const logic::Attr a = toAttr(st, &info);
    if (!g->store.hasAttr(key)) {
      g->store.putAttr(key, a);
      g->prewarmAttrStored.fetch_add(1, std::memory_order_relaxed);
    }
    if (out && st == STATUS_SUCCESS) {
      *out = a;
      ok   = true;
    }
  }
  g->prewarmFiles.fetch_add(1, std::memory_order_relaxed);
  return ok;
}

// Attr front entries for virtual files, synthesized from the real target's
// attributes (what hook_GetFileAttributesW/ExW would return for the virtual
// path: the real call's result). Built per directory, inserted in one batch.
struct FrontBatch
{
  std::vector<std::pair<FrontKey, FrontResult>> items;
};

void frontBatchAdd(FrontBatch& batch, const std::string& vpath, const logic::Attr& a)
{
  std::wstring w = shared::string_cast<std::wstring>(vpath, shared::CodePage::UTF8);
  std::replace(w.begin(), w.end(), L'/', L'\\');
  std::wstring key = logic::frontKey(w, std::wstring_view());
  if (key.empty() || key.size() < 3 || key[1] != L':')
    return;
  const std::wstring virtualNt = makeKey(std::wstring(LR"(\??\)") + key);
  if (virtualNt.empty() || g->store.isVolatile(virtualNt))
    return;
  FrontResult r;
  r.ok   = true;
  r.err  = 0;
  r.attr = a.attributes;
  WIN32_FILE_ATTRIBUTE_DATA& d = r.data;
  d.dwFileAttributes = a.attributes;
  auto ft = [](int64_t v, FILETIME& f) {
    f.dwLowDateTime  = static_cast<DWORD>(static_cast<uint64_t>(v) & 0xFFFFFFFFu);
    f.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(v) >> 32);
  };
  ft(a.creation, d.ftCreationTime);
  ft(a.access, d.ftLastAccessTime);
  ft(a.write, d.ftLastWriteTime);
  d.nFileSizeLow  = static_cast<DWORD>(static_cast<uint64_t>(a.endOfFile) & 0xFFFFFFFFu);
  d.nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(a.endOfFile) >> 32);
  std::wstring k1 = key, k2 = key;
  k1.push_back(L'\x01');
  k2.push_back(L'\x02');
  batch.items.emplace_back(makeFrontKey(k1), r);
  batch.items.emplace_back(makeFrontKey(k2), r);
}

void frontBatchCommit(FrontBatch& batch, uint64_t gen)
{
  FrontState* f = gFront;
  if (!f || batch.items.empty())
    return;
  std::unique_lock<std::shared_mutex> lock(f->mutex);
  if (g->store.generation() != gen)
    return;
  if (f->entries.size() * 4 + batch.items.size() * 4 >= f->maxEntries * 3)
    return;  // keep room for what the game fills itself
  for (auto& it : batch.items) {
    FrontEntry& e = f->entries[it.first];
    e.gen    = gen;
    e.result = it.second;
  }
  g->frontAttrSynth.fetch_add(batch.items.size(), std::memory_order_relaxed);
}

void prewarmListing(const std::wstring& virtualKey, const std::wstring& realNtPath, int cls)
{
  // listings are the big items: leave most of the memory to what the game fills
  if (g->store.usedAbove(60) || g->store.hasListing(virtualKey, cls, L"*"))
    return;
  const uint64_t generation = g->store.generation();
  HANDLE h = openDirectory(realNtPath);
  if (!h)
    return;

  auto listing = std::make_shared<Listing>();
  std::vector<unsigned char> buf(64 * 1024);
  bool complete = false;
  for (;;) {
    if (g->pool.stopping())
      break;
    IO_STATUS_BLOCK iosb;
    std::memset(&iosb, 0, sizeof(iosb));
    const NTSTATUS st = ::NtQueryDirectoryFile(
        h, nullptr, nullptr, nullptr, &iosb, buf.data(), static_cast<ULONG>(buf.size()),
        static_cast<FILE_INFORMATION_CLASS>(cls), FALSE, nullptr, FALSE);
    if (st == STATUS_SUCCESS) {
      if (!parseRecords(buf.data(), iosb.Information,
                        static_cast<FILE_INFORMATION_CLASS>(cls), *listing))
        break;
    } else {
      complete = (st == STATUS_NO_MORE_FILES || st == STATUS_NO_SUCH_FILE);
      break;
    }
  }
  ::NtClose(h);
  if (complete) {
    g->store.putListing(virtualKey, cls, L"*", listing, false, generation);
    g->prewarmListings.fetch_add(1, std::memory_order_relaxed);
    g->prewarmListingBytes.fetch_add(listing->bytes, std::memory_order_relaxed);
    diag(g->diagPrewarmStores, [&](spdlog::logger& l) {
      l.info("metacache diag: pre-warm stored listing key='{}' class={} entries={}",
             narrow(virtualKey), cls, listing->records.size());
    });
  }
}

bool listChildren(const std::string& vpath, std::vector<Child>& out)
{
  using NodePtr = RedirectionTree::NodePtrT;

  const uint64_t waitStart = nowUs();
  HookContext::ConstPtr context = HookContext::readAccess("metacache prewarm");
  const uint64_t waited = nowUs() - waitStart;
  g->prewarmLockWaitUs.fetch_add(waited, std::memory_order_relaxed);
  const uint64_t waits = g->prewarmLockWaits.fetch_add(1, std::memory_order_relaxed) + 1;
  // contention with the game threads: back off to two concurrent jobs
  if (waits >= 200 && g->prewarmLimit.load() > 2 &&
      g->prewarmLockWaitUs.load() / waits > 1000) {
    g->prewarmLimit.store(2);
    if (auto logger = spdlog::get("usvfs"))
      logger->info("metacache: pre-warm limited to 2 jobs (avg lock wait {} us)",
                   g->prewarmLockWaitUs.load() / waits);
  }
  const RedirectionTreeContainer& table = context->redirectionTable();

  // NOTE: DirectoryTree::find("*") returns all descendants, not just the
  // children, so the child map is walked directly.
  auto collect = [&](const RedirectionTree& tree) {
    for (auto it = tree.filesBegin(); it != tree.filesEnd(); ++it) {
      const NodePtr& n = it->second;
      Child c;
      c.name  = n->name();
      c.dir   = n->isDirectory();
      c.dummy = n->hasFlag(shared::FLAG_DUMMY);
      c.link  = n->data().linkTarget.c_str();
      // File nodes carry FLAG_DIRECTORY as well (usvfsVirtualLinkDirectoryStatic
      // passes `true` as the flags of addFile), so isDirectory() is no use. A
      // directory link ends in a separator or is a dummy; a file link does not.
      const bool linkEndsInSeparator =
          !c.link.empty() && (c.link.back() == '\\' || c.link.back() == '/');
      c.file = !c.dummy && !c.link.empty() && !linkEndsInSeparator;
      c.dir  = !c.file;
      if (n->hasFlag(shared::FLAG_CREATETARGET) && !c.link.empty()) {
        // the overwrite folder: never cache anything in it
        g->store.addNoCachePrefix(makeKey(toNtPath(c.link)));
      }
      out.push_back(std::move(c));
    }
  };

  if (vpath.empty()) {
    collect(*table.get());
  } else {
    NodePtr node = table->findNode(boost::filesystem::path(vpath));
    if (node.get() == nullptr)
      return false;
    collect(*node);
  }
  return true;
}

void prewarmDirectory(const std::string& vpath, const std::string& link);
void prewarmRealDirectory(const std::string& realDir);


// Enumeration front pre-warm (the attribute front is synthesized during the main
// pre-warm walk, see frontBatchAdd). Runs the real (hooked) FindFirstFileExW "*"
// enumeration of every virtual directory, which records and publishes the
// listing: exact usvfs merge semantics. N worker threads (dedicated threads, not
// BgPool workers, which block every hook group: the hooks must behave as for the
// game or nothing would be recorded), BELOW_NORMAL, never traced. Directories
// are visited by logic::frontPrewarmRank (Data and its children first, then
// breadth-first). Stops when the enum cache is 75% full, when the generation
// moves, at shutdown, or at the optional time budget.
struct FrontJob
{
  std::mutex m;
  std::condition_variable cv;
  std::multimap<std::pair<int, uint64_t>, std::string> queue;  // (rank, seq) -> vpath
  uint64_t seq    = 0;
  int busy        = 0;  // workers holding a directory
  int alive       = 0;  // workers not yet finished
  bool stop       = false;
  const char* why = "done";
  uint64_t gen0   = 0;
};

void frontJobStop(FrontJob& job, const char* why)
{
  std::lock_guard<std::mutex> lock(job.m);
  if (!job.stop) {
    job.stop = true;
    job.why  = why;
  }
  job.cv.notify_all();
}

void frontPrewarmWorker(std::shared_ptr<FrontJob> job)
{
  trace::setInternalThread(true);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  auto enumBytes = [&]() -> size_t {
    if (!gEnum)
      return 0;
    std::lock_guard<std::mutex> lock(gEnum->mutex);
    return gEnum->bytes;
  };
  try {
    uint32_t n = 0;
    for (;;) {
      std::string vpath;
      {
        std::unique_lock<std::mutex> lock(job->m);
        for (;;) {
          if (job->stop)
            goto out;
          if (!job->queue.empty()) {
            auto it = job->queue.begin();
            vpath   = std::move(it->second);
            job->queue.erase(it);
            ++job->busy;
            break;
          }
          if (job->busy == 0) {  // nothing queued, nobody can add more
            job->cv.notify_all();
            goto out;
          }
          job->cv.wait_for(lock, chr::milliseconds(100));
        }
      }
      bool failed = false;
      try {
        if (g->pool.stopping()) {
          frontJobStop(*job, "shutdown");
        } else if (g->store.generation() != job->gen0) {
          frontJobStop(*job, "generation moved");
        } else if (gEnum && enumBytes() * 4 >= gEnum->maxBytes * 3) {
          frontJobStop(*job, "cache budget");
        } else if (g->frontPrewarmSec > 0 &&
                   GetTickCount64() - g->frontStartTick.load() >
                       static_cast<uint64_t>(g->frontPrewarmSec) * 1000) {
          frontJobStop(*job, "time budget");
        } else {
          std::vector<Child> kids;
          if (listChildren(vpath, kids)) {
            {
              std::lock_guard<std::mutex> lock(job->m);
              for (const Child& c : kids) {
                if (!c.dir)
                  continue;
                std::string child = vpath.empty() ? c.name : vpath + "/" + c.name;
                const int rank    = logic::frontPrewarmRank(child);
                job->queue.emplace(std::make_pair(rank, job->seq++), std::move(child));
              }
              job->cv.notify_all();
            }
            if (gEnum && !vpath.empty() && !kids.empty()) {
              std::wstring winDir =
                  shared::string_cast<std::wstring>(vpath, shared::CodePage::UTF8);
              std::replace(winDir.begin(), winDir.end(), L'/', L'\\');
              for (int level = 0; level <= 1; ++level) {
                WIN32_FIND_DATAW fd;
                const std::wstring pat = winDir + L"\\*";
                HANDLE h = ::FindFirstFileExW(pat.c_str(), static_cast<FINDEX_INFO_LEVELS>(level),
                                              &fd, FindExSearchNameMatch, nullptr, 0);
                if (h != INVALID_HANDLE_VALUE) {
                  while (::FindNextFileW(h, &fd) && !g->pool.stopping()) {
                  }
                  ::FindClose(h);
                  g->frontListsDone.fetch_add(1, std::memory_order_relaxed);
                }
              }
              g->frontDirsDone.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      } catch (...) {
        failed = true;
      }
      {
        std::lock_guard<std::mutex> lock(job->m);
        --job->busy;
        job->cv.notify_all();
      }
      if (failed)
        frontJobStop(*job, "exception");
      if ((++n & 3) == 0)
        Sleep(0);  // yield: every hooked search takes the context write lock
    }
  } catch (...) {
    frontJobStop(*job, "exception");
  }
out:
  std::lock_guard<std::mutex> lock(job->m);
  --job->alive;
  job->cv.notify_all();
}

void frontLogProgress(const char* what, uint64_t sinceMs, size_t frontNow, uint64_t recordsGained)
{
  if (auto logger = spdlog::get("usvfs"))
    logger->info("metacache: front pre-warm {} after {} ms: {} directories, {} searches, "
                 "attr entries synthesized {} (front now {}), enum listings recorded +{}",
                 what, sinceMs, g->frontDirsDone.load(), g->frontListsDone.load(),
                 g->frontAttrSynth.load(), frontNow, recordsGained);
}

void frontPrewarmJob()
{
  trace::setInternalThread(true);
  const uint64_t start = GetTickCount64();
  g->frontStartTick.store(start);
  auto job  = std::make_shared<FrontJob>();
  job->gen0 = g->store.generation();
  job->queue.emplace(std::make_pair(std::make_pair(-1, job->seq++), std::string()));
  const uint64_t recordsBefore = gEnum ? gEnum->records.load() : 0;
  auto frontFill = [&]() -> size_t {
    if (!gFront)
      return 0;
    std::shared_lock<std::shared_mutex> lock(gFront->mutex);
    return gFront->entries.size();
  };
  if (auto logger = spdlog::get("usvfs"))
    logger->info("metacache: front pre-warm start: {} threads, attr entries synthesized so far {}, "
                 "front entries {}",
                 g->frontPrewarmThreads, g->frontAttrSynth.load(), frontFill());
  int started = 0;
  for (int i = 0; i < g->frontPrewarmThreads; ++i) {
    try {
      {
        std::lock_guard<std::mutex> lock(job->m);
        ++job->alive;
      }
      std::thread([job] { frontPrewarmWorker(job); }).detach();
      ++started;
    } catch (...) {
      std::lock_guard<std::mutex> lock(job->m);
      --job->alive;
    }
  }
  uint64_t lastLog = start;
  {
    std::unique_lock<std::mutex> lock(job->m);
    while (job->alive > 0) {
      job->cv.wait_for(lock, chr::milliseconds(200));
      if (g->pool.stopping() && !job->stop) {
        job->stop = true;
        job->why  = "shutdown";
      }
      const uint64_t now = GetTickCount64();
      if (now - lastLog >= 5000) {
        lastLog = now;
        lock.unlock();
        frontLogProgress("progress", now - start, frontFill(),
                         (gEnum ? gEnum->records.load() : 0) - recordsBefore);
        lock.lock();
      }
    }
  }
  (void)started;
  frontLogProgress(job->why, GetTickCount64() - start, frontFill(),
                   (gEnum ? gEnum->records.load() : 0) - recordsBefore);
  g->frontRunning.store(false);
}

void finishPrewarmJob()
{
  if (g->prewarmPending.fetch_sub(1) == 1 && !g->prewarmDone.exchange(true)) {
    const uint64_t ms = GetTickCount64() - g->prewarmStart.load();
    if (g->frontPrewarm && gEnum && !g->pool.stopping())
    {
      try {
        g->frontRunning.store(true);
        g->frontThread = std::thread([] { frontPrewarmJob(); });
      } catch (...) {
        g->frontRunning.store(false);
      }
    }
    if (auto logger = spdlog::get("usvfs")) {
      logger->info(
          "metacache: pre-warm finished in {} ms: {} directories, {} files, {} listings, "
          "stored {} attribute entries and {} listings ({} listing bytes), cache now {} B, "
          "context lock wait {} us in {} acquisitions{}",
          ms, g->prewarmDirs.load(), g->prewarmFiles.load(), g->prewarmListings.load(),
          g->prewarmAttrStored.load(), g->prewarmListings.load(), g->prewarmListingBytes.load(),
          g->store.bytes(), g->prewarmLockWaitUs.load(), g->prewarmLockWaits.load(),
          g->prewarmCapped.load() ? " (stopped at the memory cap)" : "");
    }
  }
}

void prewarmRealDirectory(const std::string& realDir)
{
  {
    std::unique_lock<std::mutex> lock(g->gateMutex);
    while (g->prewarmRunning.load() >= g->prewarmLimit.load() && !g->pool.stopping())
      g->gateCv.wait_for(lock, chr::milliseconds(100));
    g->prewarmRunning.fetch_add(1);
  }

  if (!g->pool.stopping() && !g->prewarmCapped.load() && !g->store.nearlyFull()) {
    const std::wstring nt  = toNtPath(realDir);
    const std::wstring key = makeKey(nt);
    if (!key.empty()) {
      for (int cls : g->prewarmClasses)
        prewarmListing(key, nt, cls);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g->gateMutex);
    g->prewarmRunning.fetch_sub(1);
  }
  g->gateCv.notify_one();
  finishPrewarmJob();
}

void prewarmDirectory(const std::string& vpath, const std::string& link)
{
  // at most prewarmLimit jobs work at the same time
  {
    std::unique_lock<std::mutex> lock(g->gateMutex);
    while (g->prewarmRunning.load() >= g->prewarmLimit.load() && !g->pool.stopping())
      g->gateCv.wait_for(lock, chr::milliseconds(100));
    g->prewarmRunning.fetch_add(1);
  }

  if (!g->pool.stopping() && !g->prewarmCapped.load()) {
    std::vector<Child> kids;
    if (listChildren(vpath, kids)) {
      g->prewarmDirs.fetch_add(1, std::memory_order_relaxed);

      // listing of this directory: what a game-side enumeration of it sees
      if (!vpath.empty() && !kids.empty()) {
        std::string realDir = link.empty() ? vpath : link;
        while (realDir.size() > 3 && (realDir.back() == '\\' || realDir.back() == '/'))
          realDir.pop_back();
        const std::wstring virtualKey = makeKey(toNtPath(vpath));
        if (!virtualKey.empty()) {
          for (int cls : g->prewarmClasses)
            prewarmListing(virtualKey, toNtPath(realDir), cls);
        }
      }

      FrontBatch batch;
      const uint64_t frontGen = g->store.generation();
      for (const Child& c : kids) {
        if (g->pool.stopping())
          break;
        if (g->store.nearlyFull()) {
          g->prewarmCapped.store(true);
          break;
        }
        const std::string childPath = vpath.empty() ? c.name : vpath + "/" + c.name;
        if (c.dir) {
          g->prewarmPending.fetch_add(1);
          g->pool.submit([childPath, link = c.link] {
            prewarmDirectory(childPath, link);
          });
        } else if (!c.dummy && !c.link.empty()) {
          if (gFront && g->frontPrewarm) {
            logic::Attr a;
            if (prewarmAttr(toNtPath(c.link), &a) && !g->store.isVolatile(makeKey(toNtPath(c.link))))
              frontBatchAdd(batch, childPath, a);
          } else {
            prewarmAttr(toNtPath(c.link));
          }

          // The game looks files up one by one in the real (mod) directory by
          // literal name; those queries are derived from a complete listing of
          // it, so list each real directory once.
          const size_t slash = c.link.find_last_of("/\\");
          if (slash != std::string::npos && slash > 0) {
            const std::string realDir = c.link.substr(0, slash);
            bool fresh = false;
            {
              std::lock_guard<std::mutex> lock(g->realDirsMutex);
              fresh = g->realDirs.insert(realDir).second;
            }
            if (fresh) {
              g->prewarmPending.fetch_add(1);
              g->pool.submit([realDir] { prewarmRealDirectory(realDir); });
            }
          }
        }
      }
      frontBatchCommit(batch, frontGen);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g->gateMutex);
    g->prewarmRunning.fetch_sub(1);
  }
  g->gateCv.notify_one();
  finishPrewarmJob();
}

bool isGameProcess()
{
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  if (n == 0 || n >= MAX_PATH * 2)
    return false;
  std::wstring_view p(path, n);
  const size_t slash = p.find_last_of(L"\\/");
  if (slash != std::wstring_view::npos)
    p.remove_prefix(slash + 1);
  return logic::normalizePath(p) == L"SKYRIMSE.EXE";
}

// The overwrite folder (create target) receives everything the game writes:
// never cache anything in it. It is found along the path of the game directory.
void registerOverwrite()
{
  try {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2)
      return;
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
      return;
    dir.resize(slash);

    HookContext::ConstPtr context = HookContext::readAccess("metacache overwrite");
    const RedirectionTreeContainer& table = context->redirectionTable();

    for (const std::wstring& candidate : {dir + L"\\Data", dir}) {
      std::string utf8 = shared::string_cast<std::string>(candidate, shared::CodePage::UTF8);
      std::replace(utf8.begin(), utf8.end(), '\\', '/');
      table->visitPath(boost::filesystem::path(utf8), [&](const RedirectionTree::NodePtrT& node) {
        if (node->hasFlag(shared::FLAG_CREATETARGET)) {
          const std::string link = node->data().linkTarget.c_str();
          if (!link.empty())
            g->store.addNoCachePrefix(makeKey(toNtPath(link)));
        }
      });
    }
  } catch (const std::exception&) {
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// public interface

void init()
{
  auto env = [](const char* name, char* buf, DWORD size) -> const char* {
    const DWORD n = GetEnvironmentVariableA(name, buf, size);
    return (n > 0 && n < size) ? buf : nullptr;
  };
  char buf[128];

  logic::g_upcaseHook = upcaseNt;

  // other processes (MO2's tools, the editors) may change files behind our back
  // while the game process is the one that is known to run on a frozen modlist
  const char* forced = env("USVFS_METACACHE", buf, sizeof(buf));
  const bool enabled =
      forced ? readcache::logic::parseFlag(forced, false) : isGameProcess();
  MEMORYSTATUSEX memStatus{};
  memStatus.dwLength = sizeof(memStatus);
  const uint64_t ramBytes = GlobalMemoryStatusEx(&memStatus) ? memStatus.ullTotalPhys : (16ull << 30);
  const uint64_t mb       = readcache::logic::parseNumber(
      env("USVFS_METACACHE_MB", buf, sizeof(buf)), readcache::logic::scaledDefaultMb(ramBytes, 64));
  g = new State(mb << 20);
  g->lastLogTick.store(GetTickCount64());
  if (enabled)
    registerOverwrite();
  const char* fr = env("USVFS_METACACHE_FRONT", buf, sizeof(buf));
  if (enabled && (fr ? readcache::logic::parseFlag(fr, true) : true))
  {
    gFront = new FrontState();
    // ~150 bytes per entry (hashed key), at most a quarter of the cap
    gFront->maxEntries = static_cast<size_t>(std::clamp<uint64_t>((mb << 20) / 4 / 150, 1000, 1000000));
  }

  const char* pw = env("USVFS_METACACHE_PREWARM", buf, sizeof(buf));
  g->prewarm = pw ? readcache::logic::parseFlag(pw, false) : isGameProcess();
  g->prewarmThreads = static_cast<int>(std::clamp<uint64_t>(
      readcache::logic::parseNumber(env("USVFS_METACACHE_PREWARM_THREADS", buf, sizeof(buf)), 4),
      1, 16));
  g->prewarmLimit.store(g->prewarmThreads);

  if (const char* cl = env("USVFS_METACACHE_PREWARM_CLASSES", buf, sizeof(buf))) {
    std::vector<int> classes;
    std::string tok;
    for (const char* c = cl;; ++c) {
      if (*c == ',' || *c == 0) {
        const uint64_t v = readcache::logic::parseNumber(tok.c_str(), 0);
        if (v != 0 && classSupported(static_cast<FILE_INFORMATION_CLASS>(v)))
          classes.push_back(static_cast<int>(v));
        tok.clear();
        if (*c == 0)
          break;
      } else {
        tok.push_back(*c);
      }
    }
    g->prewarmClasses = classes;
  }

  if (enabled && g->prewarm && isGameProcess()) {
    const char* fp = env("USVFS_METACACHE_FRONT_PREWARM", buf, sizeof(buf));
    g->frontPrewarm = fp ? readcache::logic::parseFlag(fp, true) : true;
    g->frontPrewarmSec = static_cast<int>(readcache::logic::parseNumber(
        env("USVFS_METACACHE_FRONT_PREWARM_SEC", buf, sizeof(buf)), 0));
    g->frontPrewarmThreads = static_cast<int>(std::clamp<uint64_t>(
        readcache::logic::parseNumber(env("USVFS_METACACHE_FRONT_PREWARM_THREADS", buf, sizeof(buf)), 4),
        1, 16));
  }

  const char* fe = env("USVFS_METACACHE_FRONT_ENUM", buf, sizeof(buf));
  if (enabled && (fe ? readcache::logic::parseFlag(fe, true) : true)) {
    gEnum           = new EnumState();
    gEnum->maxBytes = static_cast<size_t>(mb << 20) / 2;
  }

  if (auto logger = spdlog::get("usvfs")) {
    if (enabled) {
      logger->info("metacache: enabled, RAM {} MiB, cap {} MiB, front {}, enum front {}, pre-warm {}", ramBytes >> 20, mb,
                   gFront ? "on" : "off", gEnum ? "on" : "off",
                   g->prewarm ? std::format("on ({} threads)", g->prewarmThreads)
                              : std::string("off"));
    } else {
      logger->info("metacache: off (default outside the game process; USVFS_METACACHE=1 forces it)");
    }
  }
  g_enabled.store(enabled, std::memory_order_release);
}

void startPrewarm()
{
  if (!g || !enabled() || !g->prewarm)
    return;
  if (!g->pool.start(g->prewarmThreads))
    return;
  g->prewarmStart.store(GetTickCount64());
  g->prewarmPending.store(1);
  g->pool.submit([] { prewarmDirectory(std::string(), std::string()); });
}

void shutdown()
{
  if (!g)
    return;
  g_enabled.store(false, std::memory_order_release);
  g->pool.stop();
  if (g->frontThread.joinable()) {
    const uint64_t deadline = GetTickCount64() + 2000;
    while (g->frontRunning.load() && GetTickCount64() < deadline)
      Sleep(2);
    if (g->frontRunning.load()) {
      frontLogProgress("still running at shutdown", GetTickCount64() - g->frontStartTick.load(),
                       0, 0);
      g->frontThread.detach();
    }
    else
      g->frontThread.join();
  }
  try {
    logSummary("metacache final");
  } catch (...) {
  }
}

std::wstring makeKey(std::wstring_view ntPath)
{
  if (ntPath.size() < 7)
    return std::wstring();
  std::wstring tmp(ntPath);
  if (tmp.compare(0, 4, LR"(\\?\)") == 0)
    tmp[1] = L'?';
  if (tmp.compare(0, 4, LR"(\??\)") != 0)
    return std::wstring();
  // a trailing separator changes what the system does with the name
  if (tmp.back() == L'\\' || tmp.back() == L'/')
    return std::wstring();
  return logic::normalizePath(tmp);
}

bool frontLookup(bool ex, const wchar_t* name, FrontResult& out, FrontTicket& ticket)
{
  ticket.key.clear();
  FrontState* f = gFront;
  if (!f || !g || !enabled() || !name)
    return false;
  std::wstring key = logic::frontKey(name, frontCwd(name));
  if (key.empty())
    return false;
  key.push_back(ex ? L'\x01' : L'\x02');
  const uint64_t gen = g->store.generation();
  tick();
  {
    std::shared_lock<std::shared_mutex> lock(f->mutex);
    auto it = f->entries.find(makeFrontKey(key));
    if (it != f->entries.end() && it->second.gen == gen) {
      out = it->second.result;
      f->hits.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  f->misses.fetch_add(1, std::memory_order_relaxed);
  {
    const size_t h1 = std::hash<std::wstring>()(key);
    const size_t h2 = (h1 * 0x9E3779B97F4A7C15ull) >> 17 ^ (h1 >> 7);
    bool repeat = true;
    for (size_t h : {h1, h2}) {
      const size_t bit = h & (FrontState::kSeenBits - 1);
      const uint64_t m = uint64_t(1) << (bit & 63);
      if (!(f->seen[bit >> 6].fetch_or(m, std::memory_order_relaxed) & m))
        repeat = false;
    }
    (repeat ? f->missesRepeat : f->missesFirst).fetch_add(1, std::memory_order_relaxed);
  }
  ticket.key = std::move(key);
  ticket.gen = gen;
  return false;
}

void frontStore(const FrontTicket& ticket, bool ex, const FrontResult& result,
                const wchar_t* reroutedName, const wchar_t* canonicalName)
{
  FrontState* f = gFront;
  if (!f || !g || !enabled() || ticket.key.empty())
    return;
  if (!result.ok && result.err != ERROR_FILE_NOT_FOUND && result.err != ERROR_PATH_NOT_FOUND)
    return;
  (void)ex;
  // the key was built with the cwd at lookup time, the call resolved the name
  // with the cwd at its own time: they must agree
  if (!canonicalName ||
      logic::frontKey(canonicalName, std::wstring_view()) !=
          std::wstring_view(ticket.key).substr(0, ticket.key.size() - 1))
    return;
  if (ticket.key.size() < 3 || ticket.key[1] != L':')
    return;
  // virtual path (without the kind char) and the real path it resolved to must
  // not be written/excluded
  const std::wstring virtualNt =
      makeKey(std::wstring(LR"(\??\)") + std::wstring(ticket.key, 0, ticket.key.size() - 1));
  if (virtualNt.empty() || g->store.isVolatile(virtualNt))
    return;
  if (reroutedName && *reroutedName) {
    const std::wstring_view rr(reroutedName);
    if (!logic::frontIsDriveAbs(rr))
      return;
    const std::wstring realNt = makeKey(std::wstring(LR"(\??\)") + std::wstring(rr));
    if (realNt.empty() || g->store.isVolatile(realNt))
      return;
  }
  std::unique_lock<std::shared_mutex> lock(f->mutex);
  if (g->store.generation() != ticket.gen)
    return;  // something was invalidated meanwhile
  if (f->entries.size() >= f->maxEntries)
    f->entries.clear();
  FrontEntry& e = f->entries[makeFrontKey(ticket.key)];
  e.gen    = ticket.gen;
  e.result = result;
}

bool enumFrontEnabled()
{
  return gEnum != nullptr && enabled();
}

bool enumFrontActive()
{
  return gEnum != nullptr;
}

static void enumPublish(const std::wstring& key, const std::wstring& dirNt, uint64_t gen,
                        std::shared_ptr<EnumListing> listing)
{
  EnumState* e = gEnum;
  if (!e || !g || !enabled() || dirNt.empty() || g->store.isBlockedPath(dirNt))
    return;
  std::lock_guard<std::mutex> lock(e->mutex);
  if (g->store.generation() != gen)
    return;
  const size_t add = listing->bytes() + key.size() * sizeof(wchar_t) + 96;
  if (e->bytes + add > e->maxBytes) {
    e->listings.clear();
    e->bytes = 0;
    if (add > e->maxBytes)
      return;
  }
  auto it = e->listings.find(key);
  if (it != e->listings.end())
    e->bytes -= std::min(e->bytes, it->second.listing->bytes() + key.size() * sizeof(wchar_t) + 96);
  e->listings[key] = EnumState::Stored{gen, std::move(listing)};
  e->bytes += add;
  e->records.fetch_add(1, std::memory_order_relaxed);
}

bool enumFirst(const wchar_t* name, int infoLevel, int searchOp, void* filter, DWORD flags,
               void* data, HANDLE& res, DWORD& err, EnumTicket& ticket)
{
  ticket.key.clear();
  EnumState* e = gEnum;
  if (!e || !g || !enabled() || !name)
    return false;
  auto reject = [&](int why) {
    e->rejects[why & 63].fetch_add(1, std::memory_order_relaxed);
    diag(g->diagEnumRejects, [&](spdlog::logger& l) {
      l.info("metacache diag: enum front rejected reason={} name='{}' level={} op={} filter={} flags={}",
             why, narrow(name), infoLevel, searchOp, filter != nullptr, flags);
    });
    return false;
  };
  if (!data)
    return reject(30);
  if (filter != nullptr)
    return reject(31);
  if (searchOp != 0)
    return reject(32);
  std::wstring dirKey;
  int why = 0;
  std::wstring key =
      logic::frontEnumKey(name, frontCwd(name), infoLevel, flags, &dirKey, &why);
  if (key.empty())
    return reject(why);
  std::wstring_view maskView;
  {
    const wchar_t* sep = wcsrchr(name, L'\\');
    const wchar_t* sep2 = wcsrchr(name, L'/');
    if (sep2 > sep)
      sep = sep2;
    maskView = sep ? std::wstring_view(sep + 1) : std::wstring_view(name);
    (logic::isLiteralMask(maskView) ? e->keyedLiteral : e->keyedWild)
        .fetch_add(1, std::memory_order_relaxed);
  }
  const uint64_t gen = g->store.generation();
  tick();
  std::shared_ptr<const EnumListing> star;
  {
    std::lock_guard<std::mutex> lock(e->mutex);
    auto it = e->listings.find(key);
    if (it != e->listings.end() && it->second.gen == gen) {
      const std::shared_ptr<const EnumListing>& l = it->second.listing;
      if (l->failed) {
        res = INVALID_HANDLE_VALUE;
        err = l->err;
        e->hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      WIN32_FIND_DATAW first;
      if (!l->entries.empty() && ((first = l->entries[0]), true) &&
          readcache::guardedCopy(data, &first, sizeof(WIN32_FIND_DATAW))) {
        auto s    = std::make_shared<EnumSearch>();
        s->replay = l;
        s->next   = 1;
        res       = s.get();
        e->searches[res] = std::move(s);
        err              = 0;
        e->hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
    if (logic::isDerivableMask(maskView)) {
      auto st = e->listings.find(logic::frontEnumStarKey(key));
      if (st != e->listings.end() && st->second.gen == gen)
        star = st->second.listing;
    }
  }
  // not recorded for this literal mask: derive it from the "*" listing of the same
  // directory, level and flags (existence checks); scanned without the lock
  if (star) {
    if (star->failed) {
      res = INVALID_HANDLE_VALUE;
      err = star->err;
      e->hits.fetch_add(1, std::memory_order_relaxed);
      e->derived.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    const std::vector<uint32_t> idx = star->entries.findLiteral(logic::normalizeMask(maskView));
    if (idx.empty()) {
      res = INVALID_HANDLE_VALUE;
      err = ERROR_FILE_NOT_FOUND;
      e->hits.fetch_add(1, std::memory_order_relaxed);
      e->derived.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    auto l = std::make_shared<EnumListing>();
    for (uint32_t i : idx)
      l->entries.push_back(star->entries[i]);
    const WIN32_FIND_DATAW first = l->entries[0];
    if (readcache::guardedCopy(data, &first, sizeof(WIN32_FIND_DATAW))) {
      auto s    = std::make_shared<EnumSearch>();
      s->replay = l;
      s->next   = 1;
      res       = s.get();
      {
        std::lock_guard<std::mutex> lock(e->mutex);
        e->searches[res] = std::move(s);
      }
      err = 0;
      e->hits.fetch_add(1, std::memory_order_relaxed);
      e->derived.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  e->misses.fetch_add(1, std::memory_order_relaxed);
  ticket.key   = std::move(key);
  ticket.dirNt = makeKey(std::wstring(LR"(\??\)") + dirKey);
  ticket.gen   = gen;
  return false;
}

void enumRecordFirst(const EnumTicket& ticket, const wchar_t* canonicalName, int infoLevel,
                     DWORD flags, HANDLE res, DWORD err, const WIN32_FIND_DATAW* first)
{
  EnumState* e = gEnum;
  if (!e || !g || !enabled() || ticket.key.empty() || ticket.dirNt.empty() || !canonicalName)
    return;
  // the key used the cwd at lookup time, the call resolved the name by itself
  if (logic::frontEnumKey(canonicalName, std::wstring_view(), infoLevel, flags) != ticket.key)
    return;
  if (res == INVALID_HANDLE_VALUE) {
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
      return;
    auto l    = std::make_shared<EnumListing>();
    l->failed = true;
    l->err    = err;
    enumPublish(ticket.key, ticket.dirNt, ticket.gen, std::move(l));
    return;
  }
  if (!res || !first)
    return;
  auto s      = std::make_shared<EnumSearch>();
  s->key      = ticket.key;
  s->dirNt    = ticket.dirNt;
  s->gen      = ticket.gen;
  s->building = std::make_shared<EnumListing>();
  WIN32_FIND_DATAW fd;
  if (!readcache::guardedCopy(&fd, first, sizeof(fd)))
    return;
  s->building->entries.push_back(fd);
  std::lock_guard<std::mutex> lock(e->mutex);
  e->searches[res] = std::move(s);
}

bool enumNext(HANDLE h, WIN32_FIND_DATAW* data, BOOL& result, DWORD& err)
{
  EnumState* e = gEnum;
  if (!e || !data)
    return false;
  std::lock_guard<std::mutex> lock(e->mutex);
  auto it = e->searches.find(h);
  if (it == e->searches.end() || !it->second->replay)
    return false;
  EnumSearch& s = *it->second;
  if (s.next < s.replay->entries.size()) {
    const WIN32_FIND_DATAW cur = s.replay->entries[s.next];
    if (!readcache::guardedCopy(data, &cur, sizeof(WIN32_FIND_DATAW))) {
      result = FALSE;
      err    = ERROR_INVALID_PARAMETER;
      return true;
    }
    ++s.next;
    result = TRUE;
    err    = 0;
  } else {
    result = FALSE;
    err    = ERROR_NO_MORE_FILES;
  }
  return true;
}

void enumAfterNext(HANDLE h, BOOL result, DWORD err, const WIN32_FIND_DATAW* data)
{
  EnumState* e = gEnum;
  if (!e)
    return;
  std::shared_ptr<EnumSearch> done;
  {
    std::lock_guard<std::mutex> lock(e->mutex);
    auto it = e->searches.find(h);
    if (it == e->searches.end() || it->second->replay || !it->second->building)
      return;
    EnumSearch& s = *it->second;
    if (result) {
      WIN32_FIND_DATAW fd;
      if (!data || s.building->entries.size() >= EnumState::kMaxEntries ||
          !readcache::guardedCopy(&fd, data, sizeof(fd))) {
        e->searches.erase(it);  // abandoned
        return;
      }
      s.building->entries.push_back(fd);
      return;
    }
    if (err == ERROR_NO_MORE_FILES)
      done = it->second;
    e->searches.erase(it);
  }
  if (done) {
    done->building->entries.shrink_to_fit();
    enumPublish(done->key, done->dirNt, done->gen, done->building);
  }
}

bool enumClose(HANDLE h)
{
  EnumState* e = gEnum;
  if (!e)
    return false;
  std::lock_guard<std::mutex> lock(e->mutex);
  auto it = e->searches.find(h);
  if (it == e->searches.end())
    return false;
  const bool synthetic = static_cast<bool>(it->second->replay);
  if (!synthetic && it->second->building)
    e->unfinished.fetch_add(1, std::memory_order_relaxed);
  e->searches.erase(it);
  return synthetic;
}

void noteMappingChange()
{
  if (g && enabled())
    g->store.bumpGeneration();
}

bool lookupBasic(const std::wstring& key, PFILE_BASIC_INFORMATION out, NTSTATUS& status)
{
  if (!g || !out)
    return false;
  tick();
  logic::Attr a;
  if (!g->store.lookupAttr(key, false, a)) {
    diag(g->diagAttrMisses, [&](spdlog::logger& l) {
      l.info("metacache diag: attribute miss key='{}'", narrow(key));
    });
    return false;
  }
  status = a.status;
  if (status == STATUS_SUCCESS) {
    FILE_BASIC_INFORMATION b;
    b.CreationTime.QuadPart   = a.creation;
    b.LastAccessTime.QuadPart = a.access;
    b.LastWriteTime.QuadPart  = a.write;
    b.ChangeTime.QuadPart     = a.change;
    b.FileAttributes          = a.attributes;
    if (!readcache::guardedCopy(out, &b, sizeof(b)))
      status = STATUS_ACCESS_VIOLATION;
  }
  return true;
}

bool lookupFull(const std::wstring& key, PFILE_NETWORK_OPEN_INFORMATION out, NTSTATUS& status)
{
  if (!g || !out)
    return false;
  tick();
  logic::Attr a;
  if (!g->store.lookupAttr(key, true, a)) {
    diag(g->diagAttrMisses, [&](spdlog::logger& l) {
      l.info("metacache diag: attribute miss key='{}' (full)", narrow(key));
    });
    return false;
  }
  status = a.status;
  if (status == STATUS_SUCCESS) {
    FILE_NETWORK_OPEN_INFORMATION n;
    n.CreationTime.QuadPart   = a.creation;
    n.LastAccessTime.QuadPart = a.access;
    n.LastWriteTime.QuadPart  = a.write;
    n.ChangeTime.QuadPart     = a.change;
    n.AllocationSize.QuadPart = a.allocation;
    n.EndOfFile.QuadPart      = a.endOfFile;
    n.FileAttributes          = a.attributes;
    if (!readcache::guardedCopy(out, &n, sizeof(n)))
      status = STATUS_ACCESS_VIOLATION;
  }
  return true;
}

uint64_t generation()
{
  return g ? g->store.generation() : 0;
}

void storeBasic(const std::wstring& key, NTSTATUS status,
                const FILE_BASIC_INFORMATION* info, uint64_t gen)
{
  if (!g || !cacheableStatus(status))
    return;
  FILE_BASIC_INFORMATION copy;
  std::memset(&copy, 0, sizeof(copy));
  if (status == STATUS_SUCCESS &&
      (!info || !readcache::guardedCopy(&copy, info, sizeof(copy))))
    return;
  g->store.putAttr(key, toAttr(status, &copy), gen);
}

void storeFull(const std::wstring& key, NTSTATUS status,
               const FILE_NETWORK_OPEN_INFORMATION* info, uint64_t gen)
{
  if (!g || !cacheableStatus(status))
    return;
  FILE_NETWORK_OPEN_INFORMATION copy;
  std::memset(&copy, 0, sizeof(copy));
  if (status == STATUS_SUCCESS &&
      (!info || !readcache::guardedCopy(&copy, info, sizeof(copy))))
    return;
  g->store.putAttr(key, toAttr(status, &copy), gen);
}

NTSTATUS queryDirectory(const std::wstring& dirPath, HANDLE h, PIO_STATUS_BLOCK iosb,
                        PVOID buffer, ULONG length, FILE_INFORMATION_CLASS cls,
                        BOOLEAN single, PUNICODE_STRING mask,
                        std::wstring (*resolveContent)(const std::wstring&))
{
  if (!g || !enabled() || h == nullptr || h == INVALID_HANDLE_VALUE ||
      iosb == nullptr || buffer == nullptr || !classSupported(cls))
    return realQuery(h, iosb, buffer, length, cls, single, mask);

  std::shared_ptr<Cursor> c = findCursor(h);
  if (!c) {
    c = std::make_shared<Cursor>();
    c->cls = cls;

    const std::wstring_view maskView =
        mask && mask->Buffer ? std::wstring_view(mask->Buffer, mask->Length / sizeof(WCHAR))
                             : std::wstring_view();
    c->mask = logic::normalizeMask(maskView);
    c->dir  = makeKey(dirPath);

    if (static_cast<int>(cls) < 40)
      g->dirClassCount[static_cast<int>(cls)].fetch_add(1, std::memory_order_relaxed);

    // the directory that is really listed must be cacheable as well
    bool contentOk = true;
    if (!c->dir.empty() && resolveContent) {
      const std::wstring content = makeKey(resolveContent(dirPath));
      contentOk = !content.empty() && !g->store.isBlockedPath(content);
    }

    if (c->dir.empty() || !contentOk || !logic::isCacheableMask(maskView) ||
        g->store.isBlockedPath(c->dir)) {
      c->mode = Cursor::kReal;
      g->dirRealOnly.fetch_add(1, std::memory_order_relaxed);
    } else {
      std::shared_ptr<const Listing> l =
          g->store.lookupListing(c->dir, static_cast<int>(cls), c->mask);
      if (!l)
        l = deriveListing(c->dir, static_cast<int>(cls), c->mask);
      if (l) {
        c->mode    = Cursor::kReplay;
        c->listing = std::move(l);
        g->dirReplayed.fetch_add(1, std::memory_order_relaxed);
      } else {
        c->mode       = Cursor::kRecord;
        c->building   = std::make_shared<Listing>();
        c->generation = g->store.generation();
      }
    }

    diag(g->diagDirLookups, [&](spdlog::logger& l) {
      l.info("metacache diag: directory query key='{}' class={} mask='{}' -> {}",
             narrow(c->dir), static_cast<int>(cls), narrow(c->mask),
             c->mode == Cursor::kReplay ? "replay"
                                        : c->mode == Cursor::kRecord ? "record" : "real");
    });

    std::lock_guard<std::mutex> lock(g->cursorsLock);
    g->cursors[h] = c;
  }

  tick();

  if (c->mode == Cursor::kReplay && c->cls == cls)
    return replay(*c, iosb, buffer, length, single);

  const NTSTATUS res = realQuery(h, iosb, buffer, length, cls, single, mask);
  ++c->calls;

  if (c->mode == Cursor::kRecord) {
    if (cls != c->cls) {
      c->mode = Cursor::kReal;  // information class changed mid-enumeration
      g->dirRealOnly.fetch_add(1, std::memory_order_relaxed);
    } else if (res == STATUS_SUCCESS) {
      if (!parseRecords(static_cast<const unsigned char*>(buffer), iosb->Information, cls,
                        *c->building))
        c->mode = Cursor::kReal;
    } else if (res == STATUS_NO_MORE_FILES || res == STATUS_NO_SUCH_FILE) {
      g->store.putListing(c->dir, static_cast<int>(cls), c->mask, c->building, false,
                          c->generation);
      g->dirRecorded.fetch_add(1, std::memory_order_relaxed);
      c->building.reset();
      c->mode = Cursor::kReal;
    } else {
      c->mode = Cursor::kReal;  // error or overflow: don't trust the record
    }
    if (c->mode == Cursor::kReal)
      c->building.reset();
  }
  return res;
}

void onClose(HANDLE handle)
{
  if (!g || !enabled())
    return;
  std::lock_guard<std::mutex> lock(g->cursorsLock);
  if (!g->cursors.empty())
    g->cursors.erase(handle);
}

bool isWriteIntent(ACCESS_MASK access, ULONG disposition)
{
  constexpr ACCESS_MASK kWriteBits =
      FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
      DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
  return disposition != 1 /* FILE_OPEN */ || (access & kWriteBits) != 0;
}

// Which names are files we cache? Device paths, pipes, mailslots and other
// non-file namespaces are none of our business and must not disturb the cache.
// Result: 0 = ignore, 1 = a drive/UNC file path, 2 = unresolved relative name.
static int classifyWriteName(std::wstring_view p)
{
  auto startsWith = [&](std::wstring_view prefix) {
    return p.size() >= prefix.size() &&
           _wcsnicmp(p.data(), prefix.data(), prefix.size()) == 0;
  };

  if (startsWith(LR"(\Device\)") || startsWith(LR"(\??\pipe\)") ||
      startsWith(LR"(\??\mailslot\)") || startsWith(LR"(\\.\)"))
    return 0;

  std::wstring_view rest;
  if (startsWith(LR"(\??\)") || startsWith(LR"(\\?\)")) {
    rest = p.substr(4);
  } else if (!p.empty() && p[0] == L'\\') {
    return 0;  // some other NT namespace
  } else {
    // a drive path without prefix is fine, anything else is a relative name
    if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'))
      return 1;
    return p.empty() ? 0 : 2;
  }

  if (rest.size() >= 2 && rest[1] == L':')
    return 1;  // \??\C:\...
  if (rest.size() >= 4 && _wcsnicmp(rest.data(), L"UNC\\", 4) == 0)
    return 1;
  return 0;  // \??\ConDrv, hid#..., Volume{...}, etc.
}

void noteWrite(std::wstring_view ntPath)
{
  if (!g || !enabled())
    return;
  while (ntPath.size() > 1 && (ntPath.back() == L'\\' || ntPath.back() == L'/'))
    ntPath.remove_suffix(1);

  switch (classifyWriteName(ntPath)) {
  case 0:
    return;
  case 2:
    g->store.clear();  // an unresolved relative name: can't tell what it touches
    return;
  default:
    break;
  }

  std::wstring full(ntPath);
  if (full.size() >= 3 && full[1] == L':')
    full = LR"(\??\)" + full;  // plain drive path
  const std::wstring key = makeKey(full);
  if (key.empty()) {
    g->store.clear();
  } else {
    g->store.invalidatePath(key);
  }
}

bool isMutatingClass(FILE_INFORMATION_CLASS cls)
{
  switch (static_cast<int>(cls)) {
  case 4:   // FileBasicInformation
  case 10:  // FileRenameInformation
  case 11:  // FileLinkInformation
  case 13:  // FileDispositionInformation
  case 19:  // FileAllocationInformation
  case 20:  // FileEndOfFileInformation
  case 40:  // FileShortNameInformation
  case 64:  // FileDispositionInformationEx
  case 65:  // FileRenameInformationEx
  case 72:  // FileLinkInformationEx
    return true;
  default:
    return false;
  }
}

namespace
{
struct RenameInfo
{
  union {
    BOOLEAN ReplaceIfExists;
    ULONG Flags;
  };
  HANDLE RootDirectory;
  ULONG FileNameLength;
  WCHAR FileName[1];
};
}  // namespace

void noteSetInformation(std::wstring_view handlePath, std::wstring_view rootPath,
                        FILE_INFORMATION_CLASS cls, PVOID info, ULONG length)
{
  if (!g || !enabled())
    return;

  const int c = static_cast<int>(cls);
  const bool structural = c == 10 || c == 11 || c == 13 || c == 64 || c == 65 || c == 72;

  // the handle's own path; rename/delete of a directory takes everything below
  // it along, attribute/size changes only concern the file itself
  const std::wstring key = makeKey(handlePath);
  if (key.empty()) {
    // we don't know what this handle is: forget everything
    g->store.clear();
  } else if (structural) {
    g->store.invalidateTree(key);
  } else {
    g->store.invalidatePath(key);
  }

  if ((c == 10 || c == 11 || c == 65 || c == 72)) {
    // rename / link: the target appears
    if (!info || length < offsetof(RenameInfo, FileName)) {
      g->store.clear();
      return;
    }
    RenameInfo hdr;
    if (!readcache::guardedCopy(&hdr, info, offsetof(RenameInfo, FileName))) {
      g->store.clear();
      return;
    }
    const size_t bytes = std::min<size_t>(hdr.FileNameLength, length - offsetof(RenameInfo, FileName));
    std::wstring name(bytes / sizeof(WCHAR), L'\0');
    if (!readcache::guardedCopy(name.data(),
                                static_cast<const char*>(info) + offsetof(RenameInfo, FileName),
                                bytes)) {
      g->store.clear();
      return;
    }
    std::wstring full;
    if (hdr.RootDirectory) {
      if (rootPath.empty()) {
        g->store.clear();  // relative to a handle we know nothing about
        return;
      }
      full = std::wstring(rootPath) + L"\\" + name;
    } else {
      full = name;
    }
    const std::wstring targetKey = makeKey(full);
    if (targetKey.empty()) {
      g->store.clear();  // a bare relative name or an unusual prefix
    } else {
      g->store.invalidateTree(targetKey);
    }
  }
}

void noteDosPath(const wchar_t* dosPath, bool tree)
{
  if (!g || !enabled() || !dosPath || !*dosPath)
    return;
  std::wstring nt;
  std::wstring_view p(dosPath);
  while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
    p.remove_suffix(1);
  if (p.size() >= 4 && p.compare(0, 4, LR"(\\?\)") == 0) {
    nt = std::wstring(p);
    nt[1] = L'?';
  } else if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) {
    nt = LR"(\??\)" + std::wstring(p);
  } else {
    g->store.clear();  // relative or unusual: can't tell which entries it touches
    return;
  }
  const std::wstring key = makeKey(nt);
  if (key.empty()) {
    g->store.clear();
    return;
  }
  if (tree) {
    g->store.invalidateTree(key);
  } else {
    g->store.invalidatePath(key);
  }
}

}  // namespace usvfs::metacache
