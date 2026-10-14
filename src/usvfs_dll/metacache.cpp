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

#include <stringcast.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
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
  std::vector<int> prewarmClasses{FileBothDirectoryInformation,
                                  FileIdBothDirectoryInformation};
  std::atomic<int64_t> prewarmPending{0};
  std::atomic<uint64_t> prewarmDirs{0};
  std::atomic<uint64_t> prewarmFiles{0};
  std::atomic<uint64_t> prewarmListings{0};
  std::atomic<uint64_t> prewarmStart{0};
  std::atomic<bool> prewarmDone{false};
  std::atomic<bool> prewarmCapped{false};

  std::atomic<uint64_t> tick{0};
  std::atomic<uint64_t> lastLogTick{0};
  std::atomic<uint64_t> lastActivity{0};
};

// intentionally leaked (detached threads may outlive static destruction)
State* g = nullptr;

uint64_t activity()
{
  const logic::StoreStats s = g->store.stats();
  return s.attrHits + s.attrMisses + s.attrNegativeHits + s.invalidations +
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

  logger->info(
      "{}: attr hits={} negativeHits={} misses={} | dir enumerations replayed={} "
      "(calls {}) recorded={} uncached={} listingsDerived={} classes[{} ] | "
      "invalidations={} evictions={} | entries attr={} listings={} memory={}B",
      prefix, s.attrHits, s.attrNegativeHits, s.attrMisses, ld(g->dirReplayed),
      ld(g->dirReplayCalls), ld(g->dirRecorded), ld(g->dirRealOnly), s.listingDerived,
      classes, s.invalidations, s.evictions, g->store.attrCount(),
      g->store.listingCount(), g->store.bytes());
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
    const std::string& rec = l.records[c.next + i];
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
  std::string link;  // real target, empty if none
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

void prewarmAttr(const std::wstring& realNtPath)
{
  const std::wstring key = makeKey(realNtPath);
  if (key.empty() || g->store.hasAttr(key))
    return;

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
  if (cacheableStatus(st))
    g->store.putAttr(key, toAttr(st, &info));
  g->prewarmFiles.fetch_add(1, std::memory_order_relaxed);
}

void prewarmListing(const std::wstring& virtualKey, const std::wstring& realNtPath, int cls)
{
  if (g->store.hasListing(virtualKey, cls, L"*"))
    return;
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
    g->store.putListing(virtualKey, cls, L"*", listing);
    g->prewarmListings.fetch_add(1, std::memory_order_relaxed);
  }
}

bool listChildren(const std::string& vpath, std::vector<Child>& out)
{
  using NodePtr = RedirectionTree::NodePtrT;

  HookContext::ConstPtr context = HookContext::readAccess("metacache prewarm");
  const RedirectionTreeContainer& table = context->redirectionTable();

  std::vector<NodePtr> nodes;
  if (vpath.empty()) {
    nodes = table->find("*");
  } else {
    NodePtr node = table->findNode(boost::filesystem::path(vpath));
    if (node.get() == nullptr)
      return false;
    nodes = node->find("*");
  }

  for (const NodePtr& n : nodes) {
    Child c;
    c.name  = n->name();
    c.dir   = n->isDirectory();
    c.dummy = n->hasFlag(shared::FLAG_DUMMY);
    c.link  = n->data().linkTarget.c_str();
    if (n->hasFlag(shared::FLAG_CREATETARGET) && !c.link.empty()) {
      // the overwrite folder: never cache anything in it
      g->store.addNoCachePrefix(makeKey(toNtPath(c.link)));
    }
    out.push_back(std::move(c));
  }
  return true;
}

void prewarmDirectory(const std::string& vpath, const std::string& link);

void finishPrewarmJob()
{
  if (g->prewarmPending.fetch_sub(1) == 1 && !g->prewarmDone.exchange(true)) {
    const uint64_t ms = GetTickCount64() - g->prewarmStart.load();
    if (auto logger = spdlog::get("usvfs")) {
      logger->info(
          "metacache: pre-warm finished in {} ms: {} directories, {} files, {} listings{}",
          ms, g->prewarmDirs.load(), g->prewarmFiles.load(), g->prewarmListings.load(),
          g->prewarmCapped.load() ? " (stopped at the memory cap)" : "");
    }
  }
}

void prewarmDirectory(const std::string& vpath, const std::string& link)
{
  if (!g->pool.stopping() && !g->prewarmCapped.load()) {
    std::vector<Child> kids;
    if (listChildren(vpath, kids)) {
      g->prewarmDirs.fetch_add(1, std::memory_order_relaxed);

      // listing of this directory: what a game-side enumeration of it sees
      if (!vpath.empty()) {
        const std::wstring virtualKey = makeKey(toNtPath(vpath));
        if (!virtualKey.empty()) {
          for (int cls : g->prewarmClasses)
            prewarmListing(virtualKey, toNtPath(link.empty() ? vpath : link), cls);
        }
      }

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
          prewarmAttr(toNtPath(c.link));
        }
      }
    }
  }
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

  const bool enabled = readcache::logic::parseFlag(env("USVFS_METACACHE", buf, sizeof(buf)), true);
  const uint64_t mb =
      readcache::logic::parseNumber(env("USVFS_METACACHE_MB", buf, sizeof(buf)), 128);
  g = new State(mb << 20);
  g->lastLogTick.store(GetTickCount64());

  const char* pw = env("USVFS_METACACHE_PREWARM", buf, sizeof(buf));
  g->prewarm = pw ? readcache::logic::parseFlag(pw, false) : isGameProcess();
  g->prewarmThreads = static_cast<int>(std::clamp<uint64_t>(
      readcache::logic::parseNumber(env("USVFS_METACACHE_PREWARM_THREADS", buf, sizeof(buf)), 4),
      1, 16));

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

  if (auto logger = spdlog::get("usvfs")) {
    if (enabled) {
      logger->info("metacache: enabled, cap {} MiB, pre-warm {}", mb,
                   g->prewarm ? std::format("on ({} threads)", g->prewarmThreads)
                              : std::string("off"));
    } else {
      logger->info("metacache: disabled by USVFS_METACACHE");
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
  return logic::normalizePath(tmp);
}

bool lookupBasic(const std::wstring& key, PFILE_BASIC_INFORMATION out, NTSTATUS& status)
{
  if (!g || !out)
    return false;
  tick();
  logic::Attr a;
  if (!g->store.lookupAttr(key, false, a))
    return false;
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
  if (!g->store.lookupAttr(key, true, a))
    return false;
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

void storeBasic(const std::wstring& key, NTSTATUS status, const FILE_BASIC_INFORMATION* info)
{
  if (!g || !cacheableStatus(status))
    return;
  g->store.putAttr(key, toAttr(status, info));
}

void storeFull(const std::wstring& key, NTSTATUS status,
               const FILE_NETWORK_OPEN_INFORMATION* info)
{
  if (!g || !cacheableStatus(status))
    return;
  g->store.putAttr(key, toAttr(status, info));
}

NTSTATUS queryDirectory(const std::wstring& dirPath, HANDLE h, PIO_STATUS_BLOCK iosb,
                        PVOID buffer, ULONG length, FILE_INFORMATION_CLASS cls,
                        BOOLEAN single, PUNICODE_STRING mask)
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

    if (c->dir.empty() || !logic::isCacheableMask(maskView) || g->store.isVolatile(c->dir)) {
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
        c->mode     = Cursor::kRecord;
        c->building = std::make_shared<Listing>();
      }
    }

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
      g->store.putListing(c->dir, static_cast<int>(cls), c->mask, c->building);
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

void noteWrite(std::wstring_view ntPath)
{
  if (!g || !enabled())
    return;
  const std::wstring key = makeKey(ntPath);
  if (!key.empty())
    g->store.invalidatePath(key);
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

  // the handle's own path; rename/delete of a directory takes everything below
  // it along, attribute/size changes only concern the file itself
  if (!handlePath.empty()) {
    const std::wstring key = makeKey(handlePath);
    if (!key.empty()) {
      const bool structural = c == 10 || c == 11 || c == 13 || c == 64 || c == 65 || c == 72;
      if (structural) {
        g->store.invalidateTree(key);
      } else {
        g->store.invalidatePath(key);
      }
    }
  }

  if ((c == 10 || c == 11 || c == 65 || c == 72) && info &&
      length >= offsetof(RenameInfo, FileName)) {
    // rename / link: the target appears
    RenameInfo hdr;
    if (!readcache::guardedCopy(&hdr, info, offsetof(RenameInfo, FileName)))
      return;
    const size_t bytes = std::min<size_t>(hdr.FileNameLength, length - offsetof(RenameInfo, FileName));
    std::wstring name(bytes / sizeof(WCHAR), L'\0');
    if (!readcache::guardedCopy(name.data(),
                                static_cast<const char*>(info) + offsetof(RenameInfo, FileName),
                                bytes))
      return;
    std::wstring full = hdr.RootDirectory ? std::wstring(rootPath) + L"\\" + name : name;
    const std::wstring key = makeKey(full);
    if (!key.empty())
      g->store.invalidateTree(key);
  }
}

}  // namespace usvfs::metacache
