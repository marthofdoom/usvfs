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
#include "readcache.h"
#include "readcache_logic.h"
#include "hookcallcontext.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <unordered_map>

// Read cache
// ==========
//
// Skyrim (under Wine) issues tens of thousands of small NtReadFile calls per
// second that are all served from the OS page cache, so the cost is almost
// entirely the syscall. We answer those from an in-process copy of the file.
//
// Layer 1 (always on unless USVFS_READCACHE=0): the first NtReadFile on an
// unknown handle classifies the handle (see logic::evaluateEligibility). An
// eligible handle starts on probation: its reads pass through while the small
// reads of the file are counted (per CacheEntry, so reopens accumulate). Once
// USVFS_READCACHE_ADMIT_READS have been seen the file is admitted: its
// CacheEntry holds the whole file, loaded synchronously with the real NtReadFile. Entries are shared between handles
// by {file id, size, write time, change time}. All eligible
// handles keep a "shadow" file pointer, because served reads do not move the
// real one; the position queries/updates are patched accordingly and any read
// we cannot serve first writes the shadow position back to the real pointer.
//
// Layer 2 (USVFS_READCACHE_ASYNC=1, default off): instead of filling in the
// calling thread, the fill is queued to a small worker pool. Workers read the
// file in 1 MiB chunks through their own handle and publish progress in
// CacheEntry::loadedBytes; readers wait only for the range they need.
//   The worker handle is opened *by name*: the NT name of the caller's handle
//   (NtQueryObject/ObjectNameInformation) is captured at classification time,
//   and after opening the file id, size and times are compared with the cache
//   key, so a renamed/replaced file fails the fill instead of caching wrong
//   data. (FILE_OPEN_BY_FILE_ID was not used: it needs a volume handle and its
//   behaviour under Wine is unverified. DuplicateHandle would share the file
//   pointer and is not an option.) If the name cannot be queried the fill
//   falls back to the synchronous path.
//
// Correctness rules worth remembering:
//  * the key has no volume serial: querying it is far too expensive under Wine
//    (mountmgr round trip per handle). A collision would need equal inode, size,
//    write time and change time (ns precision) on two different filesystems;
//  * the state of a handle is erased in onClose() *before* the real close:
//    handle values get reused;
//  * a handle is classified exactly once; negative results are stored too;
//  * served data is a snapshot of the file at the time of the fill. Handles
//    are only eligible if they cannot write, but another handle writing to the
//    same file would not be noticed by already classified handles.

namespace usvfs::readcache
{

std::atomic<bool> g_enabled{false};

namespace
{

using logic::Eligibility;
namespace chr = std::chrono;

constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
constexpr NTSTATUS kStatusBufferTooSmall     = static_cast<NTSTATUS>(0xC0000023L);

constexpr uint64_t kSyncChunk  = 8ull << 20;  // layer 1 fill chunk
constexpr uint64_t kAsyncChunk = 1ull << 20;  // layer 2 fill chunk
constexpr auto kWaitLimit      = chr::seconds(30);
constexpr uint64_t kLogIntervalMs = 30000;

// ---------------------------------------------------------------------------
// guarded memory access (no C++ objects with destructors in these functions)

thread_local bool t_inProtectedCopy = false;
thread_local uint64_t t_syncFillUs = 0;  // sync fill time on this thread, see classify()

bool sehCopy(void* dst, const void* src, size_t n)
{
  bool ok = true;
  t_inProtectedCopy = true;
  __try {
    std::memcpy(dst, src, n);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                      GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    ok = false;
  }
  t_inProtectedCopy = false;
  return ok;
}

bool sehStoreStatus(PIO_STATUS_BLOCK iosb, NTSTATUS status, ULONG_PTR info)
{
  IO_STATUS_BLOCK local;
  std::memset(&local, 0, sizeof(local));
  local.Status      = status;
  local.Information = info;
  return sehCopy(iosb, &local, sizeof(local));
}

bool sehStoreInt64(void* dst, int64_t value)
{
  return sehCopy(dst, &value, sizeof(value));
}

// ---------------------------------------------------------------------------
// data structures

struct Key
{
  int64_t fileId  = 0;
  uint64_t size   = 0;
  int64_t written = 0;
  int64_t changed = 0;

  bool operator==(const Key& o) const
  {
    return fileId == o.fileId && size == o.size &&
           written == o.written && changed == o.changed;
  }
};

struct KeyHash
{
  size_t operator()(const Key& k) const
  {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
      h ^= v;
      h *= 1099511628211ull;
      h ^= h >> 29;
    };
    mix(static_cast<uint64_t>(k.fileId));
    mix(k.size);
    mix(static_cast<uint64_t>(k.written));
    mix(static_cast<uint64_t>(k.changed));
    return static_cast<size_t>(h);
  }
};

enum EntryState : int
{
  kEmpty = 0,  // nobody started loading
  kLoading,    // being loaded (sync filler holds `m`, or queued/running async)
  kReady,
  kFailed
};

struct CacheEntry
{
  Key key;
  uint64_t size = 0;
  std::unique_ptr<uint8_t[]> data;  // null while not resident

  // Readers hold dataLock shared while they look at `data`; eviction frees the
  // buffer under an exclusive try_lock and skips entries that are busy.
  std::shared_mutex dataLock;
  std::atomic<uint64_t> lastUse{0};     // tick of the last serve/fill (approximate LRU)
  std::atomic<uint32_t> smallReads{0};  // small reads seen on any handle of this file
  std::atomic<bool> everFilled{false};  // distinguishes a refill from the first fill
  bool counted = false;                 // data is included in the store's total; store mutex

  std::atomic<uint64_t> loadedBytes{0};
  std::atomic<int> state{kEmpty};

  std::mutex m;  // guards state transitions, pairs with cv
  std::condition_variable cv;

  std::wstring ntPath;  // layer 2: name used by the worker to reopen the file
};

enum HandleMode : int
{
  kHandleNone = 0,   // not classified yet
  kHandleIneligible, // pure passthrough
  kHandleProbation,  // eligible; reads pass through (real pointer is authoritative)
  kHandleServed      // admitted; reads are served from memory, shadowPos is authoritative
};

struct HandleState
{
  std::mutex m;
  std::atomic<int> mode{kHandleNone};
  uint64_t shadowPos = 0;  // guarded by m
  std::shared_ptr<CacheEntry> entry;

  bool served() const { return mode.load(std::memory_order_acquire) == kHandleServed; }
};

struct BucketStats
{
  std::atomic<uint64_t> admitted{0};
  std::atomic<uint64_t> fills{0};  // including refills
  std::atomic<uint64_t> fillBytes{0};
  std::atomic<uint64_t> served{0};
  std::atomic<uint64_t> bytesServed{0};
  std::atomic<uint64_t> probationEnd{0};  // handles closed/left on probation
};

struct Stats
{
  std::atomic<uint64_t> classified{0};
  std::atomic<uint64_t> eligible{0};
  std::atomic<uint64_t> ineligible{0};
  std::atomic<uint64_t> byReason[static_cast<int>(Eligibility::Count)]{};
  BucketStats bucket[logic::kSizeBuckets];
  std::atomic<uint64_t> fills{0};
  std::atomic<uint64_t> fillBytes{0};
  std::atomic<uint64_t> evictions{0};      // entries dropped
  std::atomic<uint64_t> dataEvictions{0};  // buffers freed (entry kept for its handles)
  std::atomic<uint64_t> refills{0};        // fills of an entry that had been evicted
  std::atomic<uint64_t> passthroughs{0};
  std::atomic<uint64_t> failedClassifications{0};
  std::atomic<uint64_t> classifyUs{0};  // classification time excluding sync fills
  std::atomic<uint64_t> fillUs{0};      // sync fills and async fills on workers
};

// cache entries with a byte budget on the resident data. Eviction works on the
// data buffers (entries stay alive for the handles that reference them) and
// picks the least recently used ones; it runs only when over budget and then
// evicts down to logic::evictionTarget() so that it is not repeated for every
// fill.
class Store
{
public:
  // returns the entry for the key, creating an empty one if necessary
  std::shared_ptr<CacheEntry> acquire(const Key& key)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto found = m_map.find(key);
    if (found != m_map.end())
      return found->second;

    auto entry  = std::make_shared<CacheEntry>();
    entry->key  = key;
    entry->size = key.size;
    if (entry->size == 0)
      entry->state.store(kReady);  // nothing to load
    m_map.emplace(key, entry);
    m_entriesMirror.store(m_map.size(), std::memory_order_relaxed);
    return entry;
  }

  // allocates the buffer of an entry that has none and counts it against the
  // budget, evicting other entries if necessary. May throw std::bad_alloc.
  void allocate(CacheEntry& e, uint64_t budget, Stats& stats)
  {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[static_cast<size_t>(e.size)]);

    std::lock_guard<std::mutex> lock(m_mutex);
    e.data = std::move(buf);
    if (!e.counted) {
      e.counted = true;
      m_used += e.size;
    }
    evict(budget, stats);
    m_usedMirror.store(m_used, std::memory_order_relaxed);
  }

  // removes a failed entry so that later opens try again
  void discard(const std::shared_ptr<CacheEntry>& entry)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_map.find(entry->key);
    if (found != m_map.end() && found->second == entry) {
      if (entry->counted) {
        entry->counted = false;
        m_used -= std::min(m_used, entry->size);
      }
      m_map.erase(found);
      m_usedMirror.store(m_used, std::memory_order_relaxed);
      m_entriesMirror.store(m_map.size(), std::memory_order_relaxed);
    }
  }

  uint64_t used() const { return m_usedMirror.load(std::memory_order_relaxed); }
  uint64_t entries() const { return m_entriesMirror.load(std::memory_order_relaxed); }

private:
  void evict(uint64_t budget, Stats& stats)
  {
    if (m_used <= budget)
      return;

    struct Cand
    {
      uint64_t lastUse;
      CacheEntry* entry;
    };
    std::vector<Cand> cands;
    for (auto& kv : m_map) {
      CacheEntry* e = kv.second.get();
      if (e->counted)
        cands.push_back({e->lastUse.load(std::memory_order_relaxed), e});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.lastUse < b.lastUse; });

    std::vector<logic::EvictCandidate> info;
    info.reserve(cands.size());
    for (const Cand& c : cands) {
      // entries being filled can't be evicted
      info.push_back({c.entry->size,
                      c.entry->state.load(std::memory_order_acquire) != kReady});
    }

    for (size_t idx : logic::selectEvictions(info, m_used, logic::evictionTarget(budget))) {
      CacheEntry* e = cands[idx].entry;
      if (!e->dataLock.try_lock())
        continue;  // a reader is copying, leave it
      if (e->state.load(std::memory_order_acquire) == kReady) {
        e->data.reset();
        e->loadedBytes.store(0, std::memory_order_release);
        e->state.store(kEmpty, std::memory_order_release);
        e->counted = false;
        m_used -= std::min(m_used, e->size);
        stats.dataEvictions.fetch_add(1, std::memory_order_relaxed);
      }
      e->dataLock.unlock();
    }

    // forget entries nobody references any more and that hold no data
    for (auto it = m_map.begin(); it != m_map.end();) {
      if (!it->second->counted && it->second.use_count() == 1 &&
          it->second->state.load(std::memory_order_acquire) != kLoading) {
        it = m_map.erase(it);
        stats.evictions.fetch_add(1, std::memory_order_relaxed);
      } else {
        ++it;
      }
    }
    m_entriesMirror.store(m_map.size(), std::memory_order_relaxed);
  }

  std::mutex m_mutex;
  std::unordered_map<Key, std::shared_ptr<CacheEntry>, KeyHash> m_map;
  uint64_t m_used = 0;
  std::atomic<uint64_t> m_usedMirror{0};
  std::atomic<uint64_t> m_entriesMirror{0};
};

struct Pool
{
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::shared_ptr<CacheEntry>> queue;
  std::vector<std::thread> threads;
  bool started = false;
  std::atomic<int> alive{0};
};

struct State
{
  logic::Limits limits;
  uint32_t admitReads = 8;
  bool async = false;
  int threads = 4;

  std::shared_mutex handlesLock;  // SRWLOCK based on MSVC
  std::unordered_map<HANDLE, std::shared_ptr<HandleState>> handles;

  Store store;
  Stats stats;
  Pool pool;

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> lastLogTick{0};
  std::atomic<uint64_t> lastLogActivity{0};
};

// intentionally leaked: detached workers may outlive the static destructors
State* g = nullptr;

// ---------------------------------------------------------------------------
// statistics logging (off the hot path: only every 4096th event looks at the
// clock)

uint64_t servedTotal()
{
  uint64_t n = 0;
  for (const BucketStats& b : g->stats.bucket)
    n += b.served.load(std::memory_order_relaxed);
  return n;
}

uint64_t bytesServedTotal()
{
  uint64_t n = 0;
  for (const BucketStats& b : g->stats.bucket)
    n += b.bytesServed.load(std::memory_order_relaxed);
  return n;
}

uint64_t activity()
{
  const Stats& s = g->stats;
  return s.classified.load(std::memory_order_relaxed) +
         servedTotal() +
         s.passthroughs.load(std::memory_order_relaxed) +
         s.fills.load(std::memory_order_relaxed);
}

void logSummary(const char* prefix)
{
  auto logger = spdlog::get("usvfs");
  if (!logger)
    return;

  const Stats& s = g->stats;
  auto ld = [](const std::atomic<uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
  };

  std::string reasons;
  for (int i = 1; i < static_cast<int>(Eligibility::Count); ++i) {
    const uint64_t n = ld(s.byReason[i]);
    if (n == 0)
      continue;
    if (!reasons.empty())
      reasons += ' ';
    reasons += logic::eligibilityName(static_cast<Eligibility>(i));
    reasons += '=';
    reasons += std::to_string(n);
  }

  logger->info(
      "{}: handles classified={} eligible={} ineligible={} [{}] "
      "reads served={} bytes={} passthrough={} fills={} refills={} fillBytes={} "
      "dataEvictions={} entryEvictions={} entries={} resident={}B classifyFailures={} classifyUs={} (avg {}) "
      "fillUs={} (avg {})",
      prefix, ld(s.classified), ld(s.eligible), ld(s.ineligible), reasons,
      servedTotal(), bytesServedTotal(), ld(s.passthroughs), ld(s.fills),
      ld(s.refills), ld(s.fillBytes), ld(s.dataEvictions), ld(s.evictions),
      g->store.entries(), g->store.used(),
      ld(s.failedClassifications), ld(s.classifyUs),
      ld(s.classified) ? ld(s.classifyUs) / ld(s.classified) : 0, ld(s.fillUs),
      ld(s.fills) ? ld(s.fillUs) / ld(s.fills) : 0);

  std::string buckets;
  for (size_t i = 0; i < logic::kSizeBuckets; ++i) {
    const BucketStats& b = s.bucket[i];
    buckets += std::format(
        " [{}: admitted={} fills={} fillBytes={} served={} bytes={} probationEnd={}]",
        logic::sizeBucketName(i), ld(b.admitted), ld(b.fills), ld(b.fillBytes),
        ld(b.served), ld(b.bytesServed), ld(b.probationEnd));
  }
  logger->info("{} by file size:{}", prefix, buckets);
}

void maybeLog()
{
  const uint64_t now  = GetTickCount64();
  uint64_t last       = g->lastLogTick.load(std::memory_order_relaxed);
  if (now - last < kLogIntervalMs)
    return;

  const uint64_t act = activity();
  if (act == g->lastLogActivity.load(std::memory_order_relaxed)) {
    // nothing happened, don't spam; look again later
    g->lastLogTick.store(now, std::memory_order_relaxed);
    return;
  }
  if (!g->lastLogTick.compare_exchange_strong(last, now))
    return;  // another thread logs
  g->lastLogActivity.store(act, std::memory_order_relaxed);
  try {
    logSummary("readcache");
  } catch (...) {
  }
}

inline void countEvent(const std::atomic<uint64_t>& counter)
{
  if ((counter.load(std::memory_order_relaxed) & 0xFFF) == 0)
    maybeLog();
}

// ---------------------------------------------------------------------------
// thin wrappers around the real ntdll functions (callers are either inside
// one of our hooks, where the hook barrier makes calls to the same function
// bypass the hook, or on threads that run with all usvfs hook groups blocked)

NTSTATUS setRealPos(HANDLE h, uint64_t pos)
{
  FILE_POSITION_INFORMATION info;
  info.CurrentByteOffset.QuadPart = static_cast<LONGLONG>(pos);
  IO_STATUS_BLOCK iosb;
  std::memset(&iosb, 0, sizeof(iosb));
  return ::NtSetInformationFile(h, &iosb, &info, sizeof(info),
                                FilePositionInformation);
}

bool getRealPos(HANDLE h, uint64_t& pos)
{
  FILE_POSITION_INFORMATION info;
  std::memset(&info, 0, sizeof(info));
  IO_STATUS_BLOCK iosb;
  std::memset(&iosb, 0, sizeof(iosb));
  const NTSTATUS st = ::NtQueryInformationFile(h, &iosb, &info, sizeof(info),
                                               FilePositionInformation);
  if (st < 0 || info.CurrentByteOffset.QuadPart < 0)
    return false;
  pos = static_cast<uint64_t>(info.CurrentByteOffset.QuadPart);
  return true;
}

// attributes of a file handle needed for classification
struct FileAttrs
{
  int64_t fileId       = 0;
  int64_t written      = 0;
  int64_t changed      = 0;
  int64_t endOfFile    = 0;
  ULONG fileAttributes = 0;
  int64_t position     = 0;
  ULONG mode           = 0;
  ACCESS_MASK access   = 0;
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

// set once the system says FileStatInformation does not exist
std::atomic<bool> g_noStatInfo{false};

// FileAllInformation fallback. Its name at the end is variable-length;
// STATUS_BUFFER_OVERFLOW still fills in the fixed part; if a stack buffer is too
// small anyway we retry with a buffer that fits any path. Some implementations
// leave Mode/AccessFlags zeroed, so ask for them individually in that case.
NTSTATUS queryAllFallback(HANDLE h, FileAttrs& out)
{
  auto extract = [&out](const FILE_ALL_INFORMATION& all) {
    out.fileId         = all.InternalInformation.IndexNumber.QuadPart;
    out.written        = all.BasicInformation.LastWriteTime.QuadPart;
    out.changed        = all.BasicInformation.ChangeTime.QuadPart;
    out.endOfFile      = all.StandardInformation.EndOfFile.QuadPart;
    out.fileAttributes = all.BasicInformation.FileAttributes;
    if (all.StandardInformation.Directory)
      out.fileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    out.position = all.PositionInformation.CurrentByteOffset.QuadPart;
    out.mode     = all.ModeInformation.Mode;
    out.access   = all.AccessInformation.AccessFlags;
  };

  struct
  {
    FILE_ALL_INFORMATION info;
    WCHAR extra[1024];
  } stackBuf;
  std::memset(&stackBuf, 0, sizeof(stackBuf));

  IO_STATUS_BLOCK iosb;
  std::memset(&iosb, 0, sizeof(iosb));
  NTSTATUS st = ::NtQueryInformationFile(h, &iosb, &stackBuf, sizeof(stackBuf),
                                         FileAllInformation);
  if (st == STATUS_SUCCESS || st == STATUS_BUFFER_OVERFLOW) {
    extract(stackBuf.info);
    st = STATUS_SUCCESS;
  } else if (st == kStatusInfoLengthMismatch || st == kStatusBufferTooSmall) {
    std::vector<unsigned char> big(sizeof(FILE_ALL_INFORMATION) + 70000);
    std::memset(&iosb, 0, sizeof(iosb));
    st = ::NtQueryInformationFile(h, &iosb, big.data(),
                                  static_cast<ULONG>(big.size()),
                                  FileAllInformation);
    if (st == STATUS_SUCCESS || st == STATUS_BUFFER_OVERFLOW) {
      FILE_ALL_INFORMATION copy;
      std::memcpy(&copy, big.data(), sizeof(copy));
      extract(copy);
      st = STATUS_SUCCESS;
    }
  }
  if (st != STATUS_SUCCESS)
    return st;

  if (out.mode == 0) {
    FILE_MODE_INFORMATION mi;
    std::memset(&mi, 0, sizeof(mi));
    std::memset(&iosb, 0, sizeof(iosb));
    if (::NtQueryInformationFile(h, &iosb, &mi, sizeof(mi), FileModeInformation) >= 0)
      out.mode = mi.Mode;
  }
  if (out.access == 0) {
    FILE_ACCESS_INFORMATION ai;
    std::memset(&ai, 0, sizeof(ai));
    std::memset(&iosb, 0, sizeof(iosb));
    if (::NtQueryInformationFile(h, &iosb, &ai, sizeof(ai), FileAccessInformation) >= 0)
      out.access = ai.AccessFlags;
  }
  return STATUS_SUCCESS;
}

// Preferred path: FileStatInformation + position + mode + access. Under Wine
// the first two are plain fstat/lseek and the other two one wineserver call
// each, while FileAllInformation needs a server round trip to build the name
// and reports Mode/AccessFlags as 0. (EffectiveAccess of the stat information
// is not trustworthy under Wine, so FileAccessInformation is used instead.)
NTSTATUS queryAttrs(HANDLE h, FileAttrs& out)
{
  IO_STATUS_BLOCK iosb;
  if (!g_noStatInfo.load(std::memory_order_relaxed)) {
    USVFS_FILE_STAT_INFORMATION stat;
    std::memset(&stat, 0, sizeof(stat));
    std::memset(&iosb, 0, sizeof(iosb));
    const NTSTATUS st = ::NtQueryInformationFile(h, &iosb, &stat, sizeof(stat),
                                                 FileStatInformation);
    if (st == STATUS_SUCCESS) {
      FILE_POSITION_INFORMATION pos;
      FILE_MODE_INFORMATION mode;
      FILE_ACCESS_INFORMATION access;
      std::memset(&pos, 0, sizeof(pos));
      std::memset(&mode, 0, sizeof(mode));
      std::memset(&access, 0, sizeof(access));

      std::memset(&iosb, 0, sizeof(iosb));
      NTSTATUS st2 = ::NtQueryInformationFile(h, &iosb, &pos, sizeof(pos),
                                              FilePositionInformation);
      if (st2 >= 0) {
        std::memset(&iosb, 0, sizeof(iosb));
        st2 = ::NtQueryInformationFile(h, &iosb, &mode, sizeof(mode),
                                       FileModeInformation);
      }
      if (st2 >= 0) {
        std::memset(&iosb, 0, sizeof(iosb));
        st2 = ::NtQueryInformationFile(h, &iosb, &access, sizeof(access),
                                       FileAccessInformation);
      }
      if (st2 < 0)
        return st2;

      out.fileId         = stat.FileId.QuadPart;
      out.written        = stat.LastWriteTime.QuadPart;
      out.changed        = stat.ChangeTime.QuadPart;
      out.endOfFile      = stat.EndOfFile.QuadPart;
      out.fileAttributes = stat.FileAttributes;
      out.position       = pos.CurrentByteOffset.QuadPart;
      out.mode           = mode.Mode;
      out.access         = access.AccessFlags;
      return STATUS_SUCCESS;
    }
    // 0xC0000003 invalid info class, 0xC0000002 not implemented,
    // 0xC00000BB not supported
    if (st == static_cast<NTSTATUS>(0xC0000003L) ||
        st == static_cast<NTSTATUS>(0xC0000002L) ||
        st == static_cast<NTSTATUS>(0xC00000BBL))
      g_noStatInfo.store(true, std::memory_order_relaxed);
  }
  return queryAllFallback(h, out);
}

// ---------------------------------------------------------------------------
// filling

bool fillFromHandle(HANDLE h, CacheEntry& e, uint64_t chunk, bool progressive)
{
  uint64_t off = 0;
  while (off < e.size) {
    if (g->stop.load(std::memory_order_relaxed))
      return false;

    const ULONG want = static_cast<ULONG>(std::min<uint64_t>(chunk, e.size - off));
    LARGE_INTEGER bo;
    bo.QuadPart = static_cast<LONGLONG>(off);
    IO_STATUS_BLOCK iosb;
    std::memset(&iosb, 0, sizeof(iosb));
    const NTSTATUS st = ::NtReadFile(h, nullptr, nullptr, nullptr, &iosb,
                                     e.data.get() + off, want, &bo, nullptr);
    if (st != STATUS_SUCCESS || iosb.Information != want)
      return false;

    off += want;
    if (progressive) {
      e.loadedBytes.store(off, std::memory_order_release);
      { std::lock_guard<std::mutex> lock(e.m); }  // no lost wakeup
      e.cv.notify_all();
    }
  }
  e.loadedBytes.store(e.size, std::memory_order_release);
  return true;
}

void touch(CacheEntry& e)
{
  const uint64_t now = GetTickCount64();
  if (e.lastUse.load(std::memory_order_relaxed) != now)
    e.lastUse.store(now, std::memory_order_relaxed);
}

void countFill(CacheEntry& e)
{
  if (e.everFilled.exchange(true)) {
    g->stats.refills.fetch_add(1, std::memory_order_relaxed);
  } else {
    g->stats.fills.fetch_add(1, std::memory_order_relaxed);
  }
  g->stats.fillBytes.fetch_add(e.size, std::memory_order_relaxed);
  BucketStats& b = g->stats.bucket[logic::sizeBucket(e.size)];
  b.fills.fetch_add(1, std::memory_order_relaxed);
  b.fillBytes.fetch_add(e.size, std::memory_order_relaxed);
  touch(e);
}

void finishEntry(CacheEntry& e, bool ok)
{
  {
    std::lock_guard<std::mutex> lock(e.m);
    e.state.store(ok ? kReady : kFailed, std::memory_order_release);
  }
  e.cv.notify_all();
  if (ok)
    countFill(e);
}

// layer 1: load the whole file through the caller's handle. The real file
// pointer ends up at an unspecified place; the caller restores it on failure.
bool fillSync(HANDLE h, const std::shared_ptr<CacheEntry>& e)
{
  std::unique_lock<std::mutex> lock(e->m);
  const int state = e->state.load(std::memory_order_acquire);
  if (state == kReady)
    return true;
  if (state == kFailed)
    return false;
  if (state == kLoading) {
    // an async fill is in flight (the other entry users will wait for it)
    return true;
  }

  e->state.store(kLoading, std::memory_order_release);
  const uint64_t t0 = nowUs();
  bool ok = false;
  try {
    g->store.allocate(*e, g->limits.budgetBytes, g->stats);
    ok = fillFromHandle(h, *e, kSyncChunk, false);
  } catch (...) {
    ok = false;
  }
  const uint64_t dt = nowUs() - t0;
  g->stats.fillUs.fetch_add(dt, std::memory_order_relaxed);
  t_syncFillUs += dt;
  e->state.store(ok ? kReady : kFailed, std::memory_order_release);
  lock.unlock();
  e->cv.notify_all();
  if (ok) {
    countFill(*e);
  } else {
    g->store.discard(e);
  }
  return ok;
}

// ----- layer 2: worker pool -------------------------------------------------

// waits until [0, need) of the entry is loaded; false if the fill failed
bool waitLoaded(CacheEntry& e, uint64_t need)
{
  if (e.loadedBytes.load(std::memory_order_acquire) >= need)
    return true;

  std::unique_lock<std::mutex> lock(e.m);
  const auto deadline = chr::steady_clock::now() + kWaitLimit;
  for (;;) {
    if (e.loadedBytes.load(std::memory_order_acquire) >= need)
      return true;
    if (e.state.load(std::memory_order_acquire) == kFailed)
      return false;
    if (g->stop.load(std::memory_order_relaxed))
      return false;
    if (chr::steady_clock::now() >= deadline)
      return false;
    e.cv.wait_for(lock, chr::milliseconds(50));
  }
}

bool queryObjectName(HANDLE h, std::wstring& out)
{
  std::vector<unsigned char> buf(2048);
  for (int attempt = 0; attempt < 2; ++attempt) {
    ULONG needed = 0;
    const NTSTATUS st = ::NtQueryObject(h, ObjectNameInformationClass,
                                        buf.data(), static_cast<ULONG>(buf.size()),
                                        &needed);
    if (st == STATUS_SUCCESS) {
      auto* name = reinterpret_cast<OBJECT_NAME_INFORMATION*>(buf.data());
      if (!name->Name.Buffer || name->Name.Length == 0)
        return false;
      out.assign(name->Name.Buffer, name->Name.Length / sizeof(WCHAR));
      return true;
    }
    if ((st == kStatusInfoLengthMismatch || st == kStatusBufferTooSmall ||
         st == STATUS_BUFFER_OVERFLOW) &&
        needed > buf.size()) {
      buf.resize(needed);
      continue;
    }
    return false;
  }
  return false;
}

bool sameFile(const FileAttrs& a, const Key& k)
{
  return a.fileId == k.fileId && a.endOfFile == static_cast<LONGLONG>(k.size) &&
         a.written == k.written && a.changed == k.changed;
}

bool fillAsync(const std::shared_ptr<CacheEntry>& e)
{
  UNICODE_STRING name;
  name.Buffer        = const_cast<PWSTR>(e->ntPath.c_str());
  name.Length        = static_cast<USHORT>(e->ntPath.size() * sizeof(WCHAR));
  name.MaximumLength = name.Length;

  OBJECT_ATTRIBUTES attrs;
  std::memset(&attrs, 0, sizeof(attrs));
  attrs.Length     = sizeof(attrs);
  attrs.ObjectName = &name;
  attrs.Attributes = 0x00000040;  // OBJ_CASE_INSENSITIVE

  HANDLE file = nullptr;
  IO_STATUS_BLOCK iosb;
  std::memset(&iosb, 0, sizeof(iosb));
  const NTSTATUS st = ::NtCreateFile(
      &file, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attrs, &iosb,
      nullptr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      0x00000001 /* FILE_OPEN */,
      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr, 0);
  if (st < 0 || !file)
    return false;

  bool ok = false;
  FileAttrs info;
  if (queryAttrs(file, info) == STATUS_SUCCESS && sameFile(info, e->key)) {
    const uint64_t t0 = nowUs();
    ok = fillFromHandle(file, *e, kAsyncChunk, true);
    g->stats.fillUs.fetch_add(nowUs() - t0, std::memory_order_relaxed);
  }

  ::NtClose(file);
  return ok;
}

void workerMain()
{
  // keep every usvfs hook out of the way on this thread
  HookCallContext blockHooks(MutExHookGroup::ALL_GROUPS);

  Pool& pool = g->pool;
  for (;;) {
    std::shared_ptr<CacheEntry> job;
    {
      std::unique_lock<std::mutex> lock(pool.m);
      pool.cv.wait(lock, [&] {
        return g->stop.load(std::memory_order_relaxed) || !pool.queue.empty();
      });
      if (g->stop.load(std::memory_order_relaxed))
        break;
      job = std::move(pool.queue.front());
      pool.queue.pop_front();
    }

    bool ok = false;
    try {
      ok = fillAsync(job);
    } catch (...) {
      ok = false;
    }
    finishEntry(*job, ok);
    if (!ok)
      g->store.discard(job);
  }
  pool.alive.fetch_sub(1);
}

// queues the load of an entry; false if that is not possible (caller falls
// back to the synchronous fill)
bool queueFill(HANDLE h, const std::shared_ptr<CacheEntry>& e)
{
  {
    std::lock_guard<std::mutex> lock(e->m);
    if (e->state.load(std::memory_order_acquire) != kEmpty)
      return true;  // somebody else already took care of it
    if (!queryObjectName(h, e->ntPath))
      return false;
    try {
      g->store.allocate(*e, g->limits.budgetBytes, g->stats);
    } catch (...) {
      return false;
    }
    e->state.store(kLoading, std::memory_order_release);
  }

  Pool& pool = g->pool;
  {
    std::lock_guard<std::mutex> lock(pool.m);
    if (!pool.started) {
      pool.started = true;
      try {
        for (int i = 0; i < g->threads; ++i) {
          pool.threads.emplace_back(workerMain);
          pool.alive.fetch_add(1);
        }
      } catch (...) {
        // run with however many threads we got
      }
      if (pool.threads.empty()) {
        pool.started = false;
        std::lock_guard<std::mutex> elock(e->m);
        e->state.store(kEmpty, std::memory_order_release);
        return false;
      }
    }
    pool.queue.push_back(e);
  }
  pool.cv.notify_one();
  return true;
}

// ---------------------------------------------------------------------------
// classification

// returns Eligible and fills the handle state, or the reason for rejection
Eligibility classifyImpl(HandleState& hs, HANDLE h, bool eventOrApc, ULONG length)
{
  logic::HandleAttrs a;
  a.hasEventOrApc    = eventOrApc;
  a.firstReadLength  = length;

  auto early = [&](Eligibility r) {
    return r == Eligibility::EventOrApc || r == Eligibility::QueryFailed ||
           r == Eligibility::NotDisk;
  };

  if (eventOrApc)
    return Eligibility::EventOrApc;

  IO_STATUS_BLOCK iosb;
  std::memset(&iosb, 0, sizeof(iosb));

  FILE_FS_DEVICE_INFORMATION dev;
  std::memset(&dev, 0, sizeof(dev));
  NTSTATUS st = ::NtQueryVolumeInformationFile(h, &iosb, &dev, sizeof(dev),
                                               FileFsDeviceInformation);
  if (st < 0) {
    a.queriesOk = false;
  } else {
    a.deviceType = dev.DeviceType;
  }
  if (Eligibility r = logic::evaluateEligibility(a, g->limits); early(r))
    return r;

  FileAttrs info;
  st = queryAttrs(h, info);
  if (st != STATUS_SUCCESS) {
    g->stats.failedClassifications.fetch_add(1, std::memory_order_relaxed);
    if (g->stats.failedClassifications.load(std::memory_order_relaxed) <= 8) {
      if (auto logger = spdlog::get("usvfs")) {
        logger->warn("readcache: attribute query failed with {:#x}",
                     static_cast<uint32_t>(st));
      }
    }
    return Eligibility::QueryFailed;
  }

  a.directory = (info.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  a.mode      = info.mode;
  a.access    = info.access;
  a.endOfFile = info.endOfFile < 0 ? UINT64_MAX
                                   : static_cast<uint64_t>(info.endOfFile);

  if (Eligibility r = logic::evaluateEligibility(a, g->limits);
      r != Eligibility::Eligible)
    return r;

  Key key;
  key.fileId  = info.fileId;
  key.size    = a.endOfFile;
  key.written = info.written;
  key.changed = info.changed;

  // no data is loaded yet: the handle starts on probation (see read())
  hs.entry = g->store.acquire(key);
  return Eligibility::Eligible;
}

void classify(HandleState& hs, HANDLE h, bool eventOrApc, ULONG length)
{
  Eligibility r = Eligibility::QueryFailed;
  const uint64_t t0 = nowUs();
  const uint64_t fill0 = t_syncFillUs;
  try {
    r = classifyImpl(hs, h, eventOrApc, length);
  } catch (...) {
    r = Eligibility::QueryFailed;
    g->stats.failedClassifications.fetch_add(1, std::memory_order_relaxed);
  }

  Stats& s = g->stats;
  {
    const uint64_t total = nowUs() - t0;
    const uint64_t fill  = t_syncFillUs - fill0;
    s.classifyUs.fetch_add(total > fill ? total - fill : 0, std::memory_order_relaxed);
  }
  s.classified.fetch_add(1, std::memory_order_relaxed);
  if (r == Eligibility::Eligible) {
    s.eligible.fetch_add(1, std::memory_order_relaxed);
  } else {
    s.ineligible.fetch_add(1, std::memory_order_relaxed);
    s.byReason[static_cast<int>(r)].fetch_add(1, std::memory_order_relaxed);
  }
  hs.mode.store(r == Eligibility::Eligible ? kHandleProbation : kHandleIneligible,
                std::memory_order_release);
  countEvent(s.classified);
}

// ---------------------------------------------------------------------------
// handle map

std::shared_ptr<HandleState> lookup(HANDLE h)
{
  std::shared_lock<std::shared_mutex> lock(g->handlesLock);
  auto it = g->handles.find(h);
  return it == g->handles.end() ? nullptr : it->second;
}

// returns true if `fresh` was inserted, otherwise `existing` is set
bool insertIfAbsent(HANDLE h, const std::shared_ptr<HandleState>& fresh,
                    std::shared_ptr<HandleState>& existing)
{
  std::unique_lock<std::shared_mutex> lock(g->handlesLock);
  auto res = g->handles.try_emplace(h, fresh);
  if (!res.second)
    existing = res.first->second;
  return res.second;
}

// ---------------------------------------------------------------------------
// reading

NTSTATUS realRead(HANDLE h, HANDLE ev, PIO_APC_ROUTINE apc, PVOID ctx,
                  PIO_STATUS_BLOCK iosb, PVOID buf, ULONG len,
                  PLARGE_INTEGER off, PULONG key)
{
  notePassthrough();
  return ::NtReadFile(h, ev, apc, ctx, iosb, buf, len, off, key);
}

// moves a handle from probation to served mode: loads the file (or queues
// it) and takes over the file pointer. Caller holds hs.m. On failure the handle
// becomes ineligible and the real pointer is as it was.
bool admit(HandleState& hs, HANDLE h)
{
  CacheEntry& e = *hs.entry;
  uint64_t pos  = 0;
  bool ok       = getRealPos(h, pos);

  if (ok) {
    bool loaded = false;
    if (g->async && e.size > 0 && !g->stop.load(std::memory_order_relaxed))
      loaded = queueFill(h, hs.entry);
    ok = loaded || fillSync(h, hs.entry);
    if (!ok)
      setRealPos(h, pos);  // the sync fill moved it
  }

  if (!ok) {
    hs.mode.store(kHandleIneligible, std::memory_order_release);
    hs.entry.reset();
    g->stats.byReason[static_cast<int>(Eligibility::FillFailed)].fetch_add(
        1, std::memory_order_relaxed);
    return false;
  }

  hs.shadowPos = pos;
  g->stats.bucket[logic::sizeBucket(e.size)].admitted.fetch_add(
      1, std::memory_order_relaxed);
  hs.mode.store(kHandleServed, std::memory_order_release);
  return true;
}

// a request on an eligible handle that we don't serve: the real file pointer
// has to match what the caller believes it to be. Caller holds hs.m.
NTSTATUS syncedRead(HandleState& hs, HANDLE h, HANDLE ev, PIO_APC_ROUTINE apc,
                    PVOID ctx, PIO_STATUS_BLOCK iosb, PVOID buf, ULONG len,
                    PLARGE_INTEGER off, PULONG key)
{
  if (setRealPos(h, hs.shadowPos) < 0)
    hs.mode.store(kHandleIneligible, std::memory_order_release);

  const NTSTATUS res = realRead(h, ev, apc, ctx, iosb, buf, len, off, key);

  uint64_t pos = 0;
  if (getRealPos(h, pos)) {
    hs.shadowPos = pos;
  } else {
    hs.mode.store(kHandleIneligible, std::memory_order_release);
  }
  return res;
}

}  // namespace

// ---------------------------------------------------------------------------
// public interface

Peek peek(HANDLE h, ULONG length)
{
  if (!g)
    return Peek::Unknown;
  std::shared_lock<std::shared_mutex> lock(g->handlesLock);
  auto it = g->handles.find(h);
  if (it == g->handles.end())
    return Peek::Unknown;
  const HandleState& hs = *it->second;
  switch (hs.mode.load(std::memory_order_acquire)) {
  case kHandleServed:
    return Peek::Serve;
  case kHandleProbation: {
    if (!logic::isSmallRead(length))
      return Peek::Passthrough;
    CacheEntry& e = *hs.entry;
    const uint32_t count = e.smallReads.fetch_add(1, std::memory_order_relaxed) + 1;
    const int state      = e.state.load(std::memory_order_relaxed);
    const bool resident  = state == kReady || state == kLoading;
    return logic::shouldAdmit(count, g->admitReads, resident) ? Peek::Serve
                                                              : Peek::Passthrough;
  }
  case kHandleIneligible:
    return Peek::Passthrough;
  default:
    return Peek::Unknown;
  }
}

void notePassthrough()
{
  // per-thread count, flushed in batches so the hot path has no atomic RMW
  thread_local uint32_t pending = 0;
  if (++pending < 1024)
    return;
  g->stats.passthroughs.fetch_add(pending, std::memory_order_relaxed);
  pending = 0;
  maybeLog();
}

bool inProtectedCopy()
{
  return t_inProtectedCopy;
}

void init()
{
  auto env = [](const char* name, char* buf, DWORD size) -> const char* {
    const DWORD n = GetEnvironmentVariableA(name, buf, size);
    return (n > 0 && n < size) ? buf : nullptr;
  };

  char buf[64];
  g = new State();

  const bool enabled = logic::parseFlag(env("USVFS_READCACHE", buf, sizeof(buf)), true);
  g->limits.budgetBytes =
      logic::parseNumber(env("USVFS_READCACHE_MB", buf, sizeof(buf)), 1024) << 20;
  g->limits.maxFileBytes =
      logic::parseNumber(env("USVFS_READCACHE_MAXFILE_MB", buf, sizeof(buf)), 64) << 20;
  g->admitReads = static_cast<uint32_t>(std::min<uint64_t>(
      logic::parseNumber(env("USVFS_READCACHE_ADMIT_READS", buf, sizeof(buf)), 8),
      1u << 20));
  g->async =
      logic::parseFlag(env("USVFS_READCACHE_ASYNC", buf, sizeof(buf)), false);
  g->threads = static_cast<int>(std::clamp<uint64_t>(
      logic::parseNumber(env("USVFS_READCACHE_THREADS", buf, sizeof(buf)), 4), 1, 16));
  g->lastLogTick.store(GetTickCount64());

  if (auto logger = spdlog::get("usvfs")) {
    if (enabled) {
      logger->info("readcache: enabled, budget {} MiB, max file {} MiB, admit after {} small reads, {}",
                   g->limits.budgetBytes >> 20, g->limits.maxFileBytes >> 20,
                   g->admitReads,
                   g->async ? std::format("async fill with {} threads", g->threads)
                            : std::string("synchronous fill"));
    } else {
      logger->info("readcache: disabled by USVFS_READCACHE");
    }
  }

  g_enabled.store(enabled, std::memory_order_release);
}

void shutdown()
{
  if (!g)
    return;

  g_enabled.store(false, std::memory_order_release);
  g->stop.store(true);
  { std::lock_guard<std::mutex> lock(g->pool.m); }  // no lost wakeup
  g->pool.cv.notify_all();

  // This runs from HookManager's destructor, i.e. outside of DllMain and the
  // loader lock, so waiting for the workers is fine. They finish at most one
  // chunk each. Give them a bounded time; if one is stuck in a slow read we
  // detach it instead of hanging process exit (State is never freed, so a late
  // worker doesn't touch freed memory).
  const auto deadline = chr::steady_clock::now() + chr::seconds(2);
  while (g->pool.alive.load() > 0 && chr::steady_clock::now() < deadline)
    Sleep(2);

  {
    std::lock_guard<std::mutex> lock(g->pool.m);
    const bool clean = g->pool.alive.load() == 0;
    for (std::thread& t : g->pool.threads) {
      if (!t.joinable())
        continue;
      if (clean) {
        t.join();
      } else {
        t.detach();
      }
    }
    g->pool.threads.clear();
  }

  {
    std::unique_lock<std::shared_mutex> lock(g->handlesLock);
    for (auto& kv : g->handles) {
      const HandleState& hs = *kv.second;
      if (hs.mode.load(std::memory_order_acquire) == kHandleProbation && hs.entry) {
        g->stats.bucket[logic::sizeBucket(hs.entry->size)].probationEnd.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
    g->handles.clear();
  }

  try {
    logSummary("readcache final");
  } catch (...) {
  }
}

void onClose(HANDLE handle)
{
  if (!g || !enabled())
    return;

  {
    std::shared_lock<std::shared_mutex> lock(g->handlesLock);
    if (g->handles.empty() || g->handles.find(handle) == g->handles.end())
      return;
  }
  std::unique_lock<std::shared_mutex> lock(g->handlesLock);
  auto it = g->handles.find(handle);
  if (it == g->handles.end())
    return;
  const HandleState& hs = *it->second;
  if (hs.mode.load(std::memory_order_acquire) == kHandleProbation && hs.entry) {
    g->stats.bucket[logic::sizeBucket(hs.entry->size)].probationEnd.fetch_add(
        1, std::memory_order_relaxed);
  }
  g->handles.erase(it);
}

NTSTATUS read(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
              PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
              ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key)
{
  auto passthrough = [&]() {
    return realRead(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
                    Buffer, Length, ByteOffset, Key);
  };

  if (!enabled() || FileHandle == nullptr || FileHandle == INVALID_HANDLE_VALUE)
    return ::NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
                        Buffer, Length, ByteOffset, Key);

  std::shared_ptr<HandleState> hs = lookup(FileHandle);
  std::unique_lock<std::mutex> lock;
  bool justClassified = false;

  if (!hs) {
    auto fresh = std::make_shared<HandleState>();
    std::unique_lock<std::mutex> freshLock(fresh->m);
    if (insertIfAbsent(FileHandle, fresh, hs)) {
      hs   = fresh;
      lock = std::move(freshLock);
      classify(*hs, FileHandle, Event != nullptr || ApcRoutine != nullptr, Length);
      justClassified = true;
    } else {
      freshLock.unlock();
    }
  }

  if (!lock.owns_lock()) {
    // fast path for handles we decided not to touch
    if (hs->mode.load(std::memory_order_acquire) == kHandleIneligible)
      return passthrough();
    lock = std::unique_lock<std::mutex>(hs->m);
  }

  if (hs->mode.load(std::memory_order_acquire) == kHandleIneligible) {
    lock.unlock();
    return passthrough();
  }

  if (hs->mode.load(std::memory_order_acquire) == kHandleProbation) {
    // the real file pointer is authoritative on probation
    if (Event != nullptr || ApcRoutine != nullptr || Key != nullptr ||
        IoStatusBlock == nullptr) {
      lock.unlock();
      return passthrough();
    }
    if (justClassified) {
      // peek() counts the reads of every later call
      bool admitNow = false;
      if (logic::isSmallRead(Length)) {
        CacheEntry& pe      = *hs->entry;
        const uint32_t cnt  = pe.smallReads.fetch_add(1, std::memory_order_relaxed) + 1;
        const int pstate    = pe.state.load(std::memory_order_relaxed);
        admitNow = logic::shouldAdmit(cnt, g->admitReads,
                                      pstate == kReady || pstate == kLoading);
      }
      if (!admitNow) {
        lock.unlock();
        return passthrough();
      }
    }
    if (!admit(*hs, FileHandle)) {
      lock.unlock();
      return passthrough();
    }
  }

  // ---- admitted handle, hs->m held ----

  CacheEntry& e = *hs->entry;

  if (Event != nullptr || ApcRoutine != nullptr || IoStatusBlock == nullptr)
    return syncedRead(*hs, FileHandle, Event, ApcRoutine, ApcContext,
                      IoStatusBlock, Buffer, Length, ByteOffset, Key);

  const logic::ReadPlan plan = logic::planRead(
      e.size, hs->shadowPos, ByteOffset != nullptr,
      ByteOffset != nullptr ? ByteOffset->QuadPart : 0, Length, Key != nullptr);

  switch (plan.action) {
  case logic::ReadAction::Passthrough:
    return syncedRead(*hs, FileHandle, Event, ApcRoutine, ApcContext,
                      IoStatusBlock, Buffer, Length, ByteOffset, Key);

  case logic::ReadAction::EndOfFile:
    if (!sehStoreStatus(IoStatusBlock, STATUS_END_OF_FILE, 0))
      return STATUS_ACCESS_VIOLATION;
    return STATUS_END_OF_FILE;

  case logic::ReadAction::Copy:
    break;
  }

  // The buffer may have been evicted since the last read (the entry stays with
  // the handle): refill it. While dataLock is held shared the buffer can't go.
  std::shared_lock<std::shared_mutex> dataLock(e.dataLock);
  bool loaded = false;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (e.state.load(std::memory_order_acquire) == kEmpty) {
      dataLock.unlock();
      bool refilled = false;
      if (g->async && !g->stop.load(std::memory_order_relaxed))
        refilled = queueFill(FileHandle, hs->entry);
      if (!refilled)
        refilled = fillSync(FileHandle, hs->entry);
      dataLock.lock();
      if (!refilled)
        break;
      continue;
    }
    // layer 2: the range may still be loading
    loaded = waitLoaded(e, plan.offset + plan.count);
    break;
  }

  if (!loaded) {
    // the fill failed or timed out: continue with the real file
    dataLock.unlock();
    const uint64_t shadow = hs->shadowPos;
    hs->mode.store(kHandleIneligible, std::memory_order_release);
    hs->entry.reset();
    g->stats.byReason[static_cast<int>(Eligibility::FillFailed)].fetch_add(
        1, std::memory_order_relaxed);
    setRealPos(FileHandle, shadow);
    lock.unlock();
    return passthrough();
  }

  if (!sehCopy(Buffer, e.data.get() + plan.offset, static_cast<size_t>(plan.count)) ||
      !sehStoreStatus(IoStatusBlock, STATUS_SUCCESS, static_cast<ULONG_PTR>(plan.count))) {
    return STATUS_ACCESS_VIOLATION;
  }
  touch(e);

  hs->shadowPos = plan.newPos;
  BucketStats& bs = g->stats.bucket[logic::sizeBucket(e.size)];
  bs.bytesServed.fetch_add(plan.count, std::memory_order_relaxed);
  bs.served.fetch_add(1, std::memory_order_relaxed);
  countEvent(bs.served);
  return STATUS_SUCCESS;
}

NTSTATUS queryInformation(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                          PVOID FileInformation, ULONG Length,
                          FILE_INFORMATION_CLASS FileInformationClass)
{
  if (!g || !enabled() ||
      (FileInformationClass != FilePositionInformation &&
       FileInformationClass != FileAllInformation))
    return ::NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation,
                                    Length, FileInformationClass);

  std::shared_ptr<HandleState> hs = lookup(FileHandle);
  if (!hs || !hs->served() ||
      FileInformation == nullptr || IoStatusBlock == nullptr)
    return ::NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation,
                                    Length, FileInformationClass);

  if (FileInformationClass == FilePositionInformation) {
    if (Length < sizeof(FILE_POSITION_INFORMATION))
      return ::NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation,
                                      Length, FileInformationClass);
    std::lock_guard<std::mutex> lock(hs->m);
    if (hs->served()) {
      if (!sehStoreInt64(FileInformation, static_cast<int64_t>(hs->shadowPos)) ||
          !sehStoreStatus(IoStatusBlock, STATUS_SUCCESS,
                          sizeof(FILE_POSITION_INFORMATION)))
        return STATUS_ACCESS_VIOLATION;
      return STATUS_SUCCESS;
    }
    // demoted in the meantime: the real pointer is correct again
    return ::NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation,
                                    Length, FileInformationClass);
  }

  // FileAllInformation: let the system fill it, then correct the position
  const NTSTATUS res = ::NtQueryInformationFile(
      FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
  if ((res == STATUS_SUCCESS || res == STATUS_BUFFER_OVERFLOW) &&
      Length >= offsetof(FILE_ALL_INFORMATION, ModeInformation)) {
    std::lock_guard<std::mutex> lock(hs->m);
    if (hs->served()) {
      auto* all = static_cast<FILE_ALL_INFORMATION*>(FileInformation);
      sehStoreInt64(&all->PositionInformation.CurrentByteOffset,
                    static_cast<int64_t>(hs->shadowPos));
    }
  }
  return res;
}

NTSTATUS setInformation(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                        PVOID FileInformation, ULONG Length,
                        FILE_INFORMATION_CLASS FileInformationClass)
{
  if (!g || !enabled() || FileInformationClass != FilePositionInformation)
    return ::NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation,
                                  Length, FileInformationClass);

  std::shared_ptr<HandleState> hs = lookup(FileHandle);
  if (!hs || !hs->served() ||
      FileInformation == nullptr || Length < sizeof(FILE_POSITION_INFORMATION))
    return ::NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation,
                                  Length, FileInformationClass);

  std::lock_guard<std::mutex> lock(hs->m);

  // read the requested position before the call so the buffer is only
  // touched through the guarded copy
  int64_t requested = 0;
  const bool readable = sehCopy(&requested, FileInformation, sizeof(requested));

  const NTSTATUS res = ::NtSetInformationFile(FileHandle, IoStatusBlock,
                                              FileInformation, Length,
                                              FileInformationClass);
  if (res >= 0 && hs->served()) {
    if (readable && requested >= 0) {
      hs->shadowPos = static_cast<uint64_t>(requested);
    } else {
      // can't tell where the pointer went; hand the handle back to the system
      uint64_t pos = 0;
      if (getRealPos(FileHandle, pos)) {
        hs->shadowPos = pos;
      } else {
        hs->mode.store(kHandleIneligible, std::memory_order_release);
      }
    }
  }
  return res;
}

}  // namespace usvfs::readcache
