// replay.exe: replays a file-API trace recorded by the usvfs DLL (USVFS_TRACE)
// against the current environment, to A/B usvfs cache layers without the game.
// See README of the harness (Linux-Storage-DLL/Tools/harness.md) for usage.
//
//   replay.exe <trace> [--timing asap|original] [--verify] [--threads-as-recorded]
//              [--single-thread] [--allow-writes] [--inherit-env] [--out results.json]
//              [--config NAME KEY=VAL ...]...
//
// The parent process spawns itself once per --config (with that environment, so
// the usvfs DLL injected into the child reads it); each child does one pass.

#include <ntdll_declarations.h>
#include "../usvfs_dll/trace_format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <latch>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace usvfs::trace;

namespace
{

constexpr NTSTATUS kStatusPending = 0x00000103;
constexpr ULONG kObjCaseInsensitive = 0x40;
constexpr ULONG kFileOpen = 1;

struct Options {
  std::wstring tracePath;
  bool timingOriginal = false;
  bool verify = false;
  bool singleThread = false;
  bool allowWrites = false;
  bool inheritEnv = false;
  std::wstring outPath;
  std::wstring childResult;  // set in child mode
};

struct Config {
  std::string name;
  std::vector<std::pair<std::wstring, std::wstring>> env;
};

std::string toUtf8(const std::wstring& w)
{
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring fromUtf8(const std::string& s)
{
  if (s.empty()) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

uint64_t qpcNow()
{
  LARGE_INTEGER v;
  ::QueryPerformanceCounter(&v);
  return (uint64_t)v.QuadPart;
}

uint64_t qpcFreq()
{
  static const uint64_t f = [] {
    LARGE_INTEGER v;
    ::QueryPerformanceFrequency(&v);
    return (uint64_t)v.QuadPart;
  }();
  return f;
}

double ticksToUs(uint64_t ticks) { return (double)ticks * 1e6 / (double)qpcFreq(); }

// ---------------------------------------------------------------------------
// trace loading

struct Trace {
  FileHeader hdr{};
  std::vector<Record> recs;                     // without StringDefs
  std::vector<std::wstring> strings;            // by id
  std::map<uint32_t, std::vector<size_t>> perThread;  // indices into recs, ts ordered
  // handle instance resolution (see resolveHandles)
  std::vector<int> instOf;   // instance of rec.h (-1 = none)
  std::vector<int> rootOf;   // instance of rec.h2
  std::vector<int> ownInst;  // instance a successful Create/Open produced
  int instances = 0;
  uint64_t minTs = 0;
};

bool readFile(const std::wstring& path, std::vector<uint8_t>& out)
{
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size;
  ::GetFileSizeEx(f, &size);
  out.resize((size_t)size.QuadPart);
  size_t done = 0;
  while (done < out.size()) {
    DWORD got = 0;
    DWORD want = (DWORD)std::min<size_t>(out.size() - done, 1u << 28);
    if (!::ReadFile(f, out.data() + done, want, &got, nullptr) || got == 0) break;
    done += got;
  }
  ::CloseHandle(f);
  out.resize(done);
  return true;
}

// A handle value is reused over the trace's lifetime: map every successful
// create to a unique "instance" and give each later use of the value the latest
// instance whose create had returned by then.
void resolveHandles(Trace& t)
{
  struct Created { uint64_t exitTs; size_t rec; };
  std::vector<Created> creates;
  for (size_t i = 0; i < t.recs.size(); ++i) {
    const Record& r = t.recs[i];
    if ((r.op == Op::Create || r.op == Op::Open) && (int32_t)r.status >= 0 && r.h)
      creates.push_back({r.ts + r.dur, i});
  }
  std::stable_sort(creates.begin(), creates.end(),
                   [](const Created& a, const Created& b) { return a.exitTs < b.exitTs; });

  std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, int>>> byValue;
  t.ownInst.assign(t.recs.size(), -1);
  for (size_t k = 0; k < creates.size(); ++k) {
    const Record& r = t.recs[creates[k].rec];
    byValue[r.h].push_back({creates[k].exitTs, (int)k});
    t.ownInst[creates[k].rec] = (int)k;
  }
  t.instances = (int)creates.size();

  auto lookup = [&](uint64_t h, uint64_t at) -> int {
    auto it = byValue.find(h);
    if (it == byValue.end()) return -1;
    auto& v = it->second;
    auto pos = std::upper_bound(v.begin(), v.end(), at,
                                [](uint64_t a, const std::pair<uint64_t, int>& e) { return a < e.first; });
    return pos == v.begin() ? -1 : std::prev(pos)->second;
  };

  t.instOf.assign(t.recs.size(), -1);
  t.rootOf.assign(t.recs.size(), -1);
  for (size_t i = 0; i < t.recs.size(); ++i) {
    const Record& r = t.recs[i];
    if (r.op != Op::Create && r.op != Op::Open && r.h) t.instOf[i] = lookup(r.h, r.ts);
    if (r.h2) t.rootOf[i] = lookup(r.h2, r.ts);
  }
}

bool loadTrace(const std::wstring& path, Trace& t, std::string& err)
{
  std::vector<uint8_t> data;
  if (!readFile(path, data)) { err = "cannot read trace"; return false; }
  if (data.size() < sizeof(FileHeader)) { err = "trace too short"; return false; }
  std::memcpy(&t.hdr, data.data(), sizeof(FileHeader));
  if (std::memcmp(t.hdr.magic, kMagic, sizeof(kMagic)) != 0 || t.hdr.version != kVersion) {
    err = "not a usvfs trace (bad magic/version)";
    return false;
  }

  size_t pos = sizeof(FileHeader);
  while (pos + 4 <= data.size()) {
    uint32_t size;
    std::memcpy(&size, &data[pos], 4);
    pos += 4;
    if (size > data.size() - pos) break;  // truncated tail (process was killed)
    size_t off = 0;
    while (off < size) {
      Record r;
      size_t n = decodeRecord(&data[pos + off], size - off, r);
      if (n == 0) break;
      off += n;
      if (r.op == Op::StringDef) {
        if (t.strings.size() <= r.str) t.strings.resize(r.str + 1);
        t.strings[r.str] = std::move(r.text);
      } else {
        t.recs.push_back(std::move(r));
      }
    }
    pos += size;
  }

  // a thread's blocks may reach the file out of order
  std::stable_sort(t.recs.begin(), t.recs.end(),
                   [](const Record& a, const Record& b) { return a.ts < b.ts; });
  t.minTs = t.recs.empty() ? 0 : t.recs.front().ts;
  for (size_t i = 0; i < t.recs.size(); ++i) t.perThread[t.recs[i].tid].push_back(i);
  resolveHandles(t);
  return true;
}

// ---------------------------------------------------------------------------
// replay pass

struct OpStat {
  uint64_t count = 0, ticks = 0, diffs = 0;
};

struct ThreadStats {
  uint32_t tid = 0;
  uint64_t ops = 0, busyTicks = 0, startTicks = 0, endTicks = 0;
  uint64_t skipped = 0, unmapped = 0, verifyChecked = 0, verifyMismatch = 0;
  OpStat op[kOpCount];
  std::vector<std::string> samples;
};

struct Inst {
  std::atomic<HANDLE> live{nullptr};
  std::atomic<bool> resolved{false};
  bool sync = true;
};

bool isWriteIntent(const Record& r)
{
  constexpr uint64_t writeBits = 0x2 /*FILE_WRITE_DATA*/ | 0x4 /*FILE_APPEND_DATA*/ |
                                 0x10 /*FILE_WRITE_EA*/ | 0x10000 /*DELETE*/ |
                                 0x40000 /*WRITE_DAC*/ | 0x80000 /*WRITE_OWNER*/ |
                                 0x40000000 /*GENERIC_WRITE*/ | 0x10000000 /*GENERIC_ALL*/;
  if (r.a & writeBits) return true;
  if (r.op == Op::Create && r.c != kFileOpen) return true;
  if (r.d & 0x1000 /*FILE_DELETE_ON_CLOSE*/) return true;
  return false;
}

class Pass
{
public:
  Pass(const Trace& t, const Options& o) : m_t(t), m_o(o), m_insts(new Inst[std::max(t.instances, 1)]) {}

  std::vector<ThreadStats> run(uint64_t& wallTicks)
  {
    std::vector<std::vector<size_t>> lists;
    if (m_o.singleThread) {
      std::vector<size_t> all(m_t.recs.size());
      for (size_t i = 0; i < all.size(); ++i) all[i] = i;
      lists.push_back(std::move(all));
    } else {
      for (auto& [tid, idx] : m_t.perThread) lists.push_back(idx);
    }

    std::vector<ThreadStats> stats(lists.size());
    std::vector<size_t> maxBuf(lists.size(), 0);
    for (size_t i = 0; i < lists.size(); ++i) {
      for (size_t idx : lists[i]) {
        const Record& r = m_t.recs[idx];
        maxBuf[i] = std::max<size_t>(maxBuf[i], std::max(r.a, r.b));
        if (r.op == Op::ProfString || r.op == Op::ProfSection)
          maxBuf[i] = std::max<size_t>(maxBuf[i], r.c * sizeof(wchar_t) + 16);
      }
      stats[i].tid = m_o.singleThread ? 0 : m_t.recs[lists[i].front()].tid;
    }

    std::latch ready((ptrdiff_t)lists.size());
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < lists.size(); ++i) {
      threads.emplace_back([&, i] {
        std::vector<uint8_t> buf(maxBuf[i] + 64);  // pre-sized: not part of the timing
        ready.count_down();
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        runThread(lists[i], buf, stats[i]);
      });
    }
    ready.wait();
    m_start = qpcNow();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    wallTicks = qpcNow() - m_start;
    return stats;
  }

private:
  void waitUntil(uint64_t recTs)
  {
    const double scale = (double)qpcFreq() / (double)m_t.hdr.qpcFreq;
    const uint64_t due = m_start + (uint64_t)((double)(recTs - m_t.minTs) * scale);
    for (;;) {
      const uint64_t now = qpcNow();
      if (now >= due) return;
      const double leftMs = ticksToUs(due - now) / 1000.0;
      if (leftMs > 2.0) ::Sleep((DWORD)(leftMs - 1.0));
      else std::this_thread::yield();
    }
  }

  // live handle of an instance, waiting for the thread that opens it; null = skip
  HANDLE resolve(int id, ThreadStats& ts)
  {
    if (id < 0) { ++ts.unmapped; return nullptr; }
    Inst& in = m_insts[id];
    const uint64_t t0 = qpcNow();
    while (!in.resolved.load(std::memory_order_acquire)) {
      if (ticksToUs(qpcNow() - t0) > 30e6) break;  // never opened (30 s)
      std::this_thread::yield();
    }
    HANDLE h = in.live.load(std::memory_order_acquire);
    if (!h) ++ts.skipped;
    return h;
  }

  void note(ThreadStats& ts, const Record& r, size_t idx, NTSTATUS got, const char* what)
  {
    OpStat& s = ts.op[(int)r.op];
    if ((uint32_t)got != r.status) {
      ++s.diffs;
      if (ts.samples.size() < 20) {
        char b[160];
        std::snprintf(b, sizeof(b), "%s rec#%zu tid %u: %s recorded 0x%08x got 0x%08x", opName(r.op),
                      idx, r.tid, what, r.status, (uint32_t)got);
        ts.samples.push_back(b);
      }
    }
  }

  const wchar_t* str(uint32_t id) const
  {
    return id && id < m_t.strings.size() ? m_t.strings[id].c_str() : nullptr;
  }

  static std::string toAnsi(const wchar_t* w)
  {
    const int n = ::WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) ::WideCharToMultiByte(CP_ACP, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
  }

  struct Name {
    UNICODE_STRING us{};
    OBJECT_ATTRIBUTES oa{};
  };

  bool makeName(const Record& r, size_t idx, ThreadStats& ts, Name& n, int rootId)
  {
    HANDLE root = nullptr;
    if (r.h2) {
      root = resolve(rootId, ts);
      if (!root) return false;
    }
    const std::wstring* s = nullptr;
    if (r.str && r.str < m_t.strings.size()) s = &m_t.strings[r.str];
    if (s) {
      n.us.Buffer = const_cast<PWSTR>(s->c_str());
      n.us.Length = (USHORT)(s->size() * sizeof(WCHAR));
      n.us.MaximumLength = n.us.Length + sizeof(WCHAR);
    }
    n.oa.Length = sizeof(n.oa);
    n.oa.RootDirectory = root;
    n.oa.ObjectName = s ? &n.us : nullptr;
    n.oa.Attributes = kObjCaseInsensitive;
    (void)idx;
    return true;
  }

  void runThread(const std::vector<size_t>& list, std::vector<uint8_t>& buf, ThreadStats& ts)
  {
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ts.startTicks = qpcNow();
    for (size_t idx : list) {
      const Record& r = m_t.recs[idx];
      if (m_o.timingOriginal) waitUntil(r.ts);
      exec(r, idx, buf, event, ts);
    }
    ts.endTicks = qpcNow();
    ::CloseHandle(event);
  }

  void exec(const Record& r, size_t idx, std::vector<uint8_t>& buf, HANDLE event, ThreadStats& ts)
  {
    IO_STATUS_BLOCK iosb{};
    NTSTATUS st = 0;
    uint64_t t0 = 0, t1 = 0;
    OpStat& os = ts.op[(int)r.op];

    switch (r.op) {
    case Op::Create:
    case Op::Open: {
      const int own = m_t.ownInst[idx];
      Name n;
      if (!makeName(r, idx, ts, n, m_t.rootOf[idx]) || (!m_o.allowWrites && isWriteIntent(r))) {
        ++ts.skipped;
        if (own >= 0) m_insts[own].resolved.store(true, std::memory_order_release);
        return;
      }
      HANDLE h = nullptr;
      t0 = qpcNow();
      if (r.op == Op::Create)
        st = NtCreateFile(&h, (ACCESS_MASK)r.a, &n.oa, &iosb, nullptr, (ULONG)r.e, (ULONG)r.b,
                          (ULONG)r.c, (ULONG)r.d, nullptr, 0);
      else
        st = NtOpenFile(&h, (ACCESS_MASK)r.a, &n.oa, &iosb, (ULONG)r.b, (ULONG)r.d);
      t1 = qpcNow();
      if (own >= 0) {
        Inst& in = m_insts[own];
        if (st >= 0) {
          in.sync = (r.d & 0x30) != 0;
          in.live.store(h, std::memory_order_release);
        }
        in.resolved.store(true, std::memory_order_release);
      } else if (st >= 0 && h) {
        NtClose(h);  // succeeded now, failed when recorded
      }
      note(ts, r, idx, st, "path");
      break;
    }
    case Op::Read: {
      HANDLE h = resolve(m_t.instOf[idx], ts);
      if (!h) return;
      LARGE_INTEGER off;
      off.QuadPart = r.off;
      const bool sync = m_insts[m_t.instOf[idx]].sync;
      if (!sync) ::ResetEvent(event);
      t0 = qpcNow();
      st = NtReadFile(h, sync ? nullptr : event, nullptr, nullptr, &iosb, buf.data(), (ULONG)r.a,
                      r.b ? &off : nullptr, nullptr);
      if (st == kStatusPending) {
        ::WaitForSingleObject(event, INFINITE);
        st = iosb.Status;
      }
      t1 = qpcNow();
      note(ts, r, idx, st, "read");
      if (m_o.verify && r.hasCrc && st >= 0) {
        ++ts.verifyChecked;
        const uint32_t crc = crc32Update(0, buf.data(), iosb.Information);
        if (crc != r.crc || iosb.Information != r.info) {
          ++ts.verifyMismatch;
          if (ts.samples.size() < 20) {
            char b[200];
            std::snprintf(b, sizeof(b), "verify rec#%zu tid %u: len %llu off %lld recorded crc %08x/%llu got %08x/%llu",
                          idx, r.tid, (unsigned long long)r.a, (long long)r.off, r.crc,
                          (unsigned long long)r.info, crc, (unsigned long long)iosb.Information);
            ts.samples.push_back(b);
          }
        }
      }
      break;
    }
    case Op::QueryInfo: {
      HANDLE h = resolve(m_t.instOf[idx], ts);
      if (!h) return;
      t0 = qpcNow();
      st = NtQueryInformationFile(h, &iosb, buf.data(), (ULONG)r.b, (FILE_INFORMATION_CLASS)r.a);
      t1 = qpcNow();
      note(ts, r, idx, st, "queryinfo");
      break;
    }
    case Op::SetInfo: {
      // only the file position; renames/deletes/etc. must not touch the data
      if ((FILE_INFORMATION_CLASS)r.a != FilePositionInformation) { ++ts.skipped; return; }
      HANDLE h = resolve(m_t.instOf[idx], ts);
      if (!h) return;
      FILE_POSITION_INFORMATION pi;
      pi.CurrentByteOffset.QuadPart = r.off;
      t0 = qpcNow();
      st = NtSetInformationFile(h, &iosb, &pi, sizeof(pi), FilePositionInformation);
      t1 = qpcNow();
      note(ts, r, idx, st, "setpos");
      break;
    }
    case Op::QueryDir:
    case Op::QueryDirEx: {
      HANDLE h = resolve(m_t.instOf[idx], ts);
      if (!h) return;
      UNICODE_STRING mask{};
      PUNICODE_STRING pm = nullptr;
      if (r.str && r.str < m_t.strings.size()) {
        mask.Buffer = const_cast<PWSTR>(m_t.strings[r.str].c_str());
        mask.Length = (USHORT)(m_t.strings[r.str].size() * sizeof(WCHAR));
        mask.MaximumLength = mask.Length + sizeof(WCHAR);
        pm = &mask;
      }
      t0 = qpcNow();
      if (r.op == Op::QueryDir)
        st = NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buf.data(), (ULONG)r.b,
                                  (FILE_INFORMATION_CLASS)r.a, (r.c & 2) != 0, pm, (r.c & 1) != 0);
      else
        st = NtQueryDirectoryFileEx(h, nullptr, nullptr, nullptr, &iosb, buf.data(), (ULONG)r.b,
                                    (FILE_INFORMATION_CLASS)r.a, (ULONG)r.c, pm);
      t1 = qpcNow();
      note(ts, r, idx, st, "querydir");
      break;
    }
    case Op::QueryAttr:
    case Op::QueryFullAttr: {
      Name n;
      if (!makeName(r, idx, ts, n, m_t.rootOf[idx])) { ++ts.skipped; return; }
      FILE_BASIC_INFORMATION bi;
      FILE_NETWORK_OPEN_INFORMATION ni;
      t0 = qpcNow();
      st = r.op == Op::QueryAttr ? NtQueryAttributesFile(&n.oa, &bi) : NtQueryFullAttributesFile(&n.oa, &ni);
      t1 = qpcNow();
      note(ts, r, idx, st, "attr");
      break;
    }
    case Op::Close: {
      const int id = m_t.instOf[idx];
      HANDLE h = resolve(id, ts);
      if (!h) return;
      t0 = qpcNow();
      st = NtClose(h);
      t1 = qpcNow();
      m_insts[id].live.store(nullptr, std::memory_order_release);
      note(ts, r, idx, st, "close");
      break;
    }
    case Op::GetAttr:
    case Op::GetAttrEx:
    case Op::FindFirst: {
      const wchar_t* path = str(r.str);
      if (!path) { ++ts.skipped; return; }
      if (r.op == Op::GetAttr) {
        t0 = qpcNow();
        st = (NTSTATUS)::GetFileAttributesW(path);
        t1 = qpcNow();
        note(ts, r, idx, st, "getattr");
      } else if (r.op == Op::GetAttrEx) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        t0 = qpcNow();
        const BOOL ok = ::GetFileAttributesExW(path, (GET_FILEEX_INFO_LEVELS)r.a, &d);
        t1 = qpcNow();
        note(ts, r, idx, ok ? 1 : 0, "getattrex");
      } else {
        // the app's enumeration is replayed to its end (FindNext/FindClose are not hooked)
        WIN32_FIND_DATAW fd;
        t0 = qpcNow();
        HANDLE fh = ::FindFirstFileExW(path, (FINDEX_INFO_LEVELS)r.a, &fd, (FINDEX_SEARCH_OPS)r.b,
                                       nullptr, (DWORD)r.c);
        if (fh != INVALID_HANDLE_VALUE) {
          while (::FindNextFileW(fh, &fd)) {}
          ::FindClose(fh);
        }
        t1 = qpcNow();
        note(ts, r, idx, fh != INVALID_HANDLE_VALUE ? 1 : 0, "findfirst");
      }
      break;
    }
    case Op::ProfString:
    case Op::ProfSection: {
      const wchar_t* file = str(r.str);
      const wchar_t* app = str((uint32_t)r.a);
      const wchar_t* key = str((uint32_t)r.b);
      if (!file) { ++ts.skipped; return; }
      const DWORD n = (DWORD)r.c;
      DWORD got;
      if (r.d) {  // the app called the ANSI function
        const std::string f = toAnsi(file), a = app ? toAnsi(app) : std::string(), k = key ? toAnsi(key) : std::string();
        t0 = qpcNow();
        got = r.op == Op::ProfString
                  ? ::GetPrivateProfileStringA(app ? a.c_str() : nullptr, key ? k.c_str() : nullptr, "",
                                               (char*)buf.data(), n, f.c_str())
                  : ::GetPrivateProfileSectionA(app ? a.c_str() : nullptr, (char*)buf.data(), n, f.c_str());
      } else {
        t0 = qpcNow();
        got = r.op == Op::ProfString
                  ? ::GetPrivateProfileStringW(app, key, L"", (wchar_t*)buf.data(), n, file)
                  : ::GetPrivateProfileSectionW(app, (wchar_t*)buf.data(), n, file);
      }
      t1 = qpcNow();
      note(ts, r, idx, (NTSTATUS)got, "profile");
      break;
    }
    case Op::WriteMarker:
      ++ts.skipped;  // writes are never replayed
      return;
    default:
      return;
    }
    ++ts.ops;
    ++os.count;
    os.ticks += t1 - t0;
    ts.busyTicks += t1 - t0;
  }

  const Trace& m_t;
  const Options& m_o;
  std::unique_ptr<Inst[]> m_insts;
  uint64_t m_start = 0;
};

// child: one pass, results as simple "key value" lines for the parent
int runChild(const Options& o)
{
  Trace t;
  std::string err;
  if (!loadTrace(o.tracePath, t, err)) {
    std::fprintf(stderr, "replay: %s\n", err.c_str());
    return 2;
  }

  if (t.recs.empty()) {
    std::fprintf(stderr, "replay: trace holds no records\n");
    return 2;
  }

  Pass pass(t, o);
  uint64_t wall = 0;
  std::vector<ThreadStats> stats = pass.run(wall);

  OpStat total[kOpCount];
  uint64_t recordedTicks[kOpCount] = {};
  uint64_t recordedCount[kOpCount] = {};
  uint64_t skipped = 0, unmapped = 0, vc = 0, vm = 0;
  for (auto& s : stats) {
    skipped += s.skipped;
    unmapped += s.unmapped;
    vc += s.verifyChecked;
    vm += s.verifyMismatch;
    for (int i = 0; i < kOpCount; ++i) {
      total[i].count += s.op[i].count;
      total[i].ticks += s.op[i].ticks;
      total[i].diffs += s.op[i].diffs;
    }
  }
  const double recScale = 1e6 / (double)t.hdr.qpcFreq;
  for (auto& r : t.recs) {
    recordedTicks[(int)r.op] += r.dur;
    ++recordedCount[(int)r.op];
  }

  std::ofstream f(o.childResult, std::ios::binary | std::ios::trunc);
  if (!f) { std::fprintf(stderr, "replay: cannot write result file\n"); return 3; }
  f << "wall_us " << (uint64_t)ticksToUs(wall) << "\n";
  f << "recorded_ops " << t.recs.size() << "\n";
  f << "skipped " << skipped << "\n";
  f << "unmapped " << unmapped << "\n";
  f << "verify_checked " << vc << "\n";
  f << "verify_mismatches " << vm << "\n";
  uint64_t diffs = 0;
  for (int i = 2; i < kOpCount; ++i) {
    diffs += total[i].diffs;
    f << "op " << opName((Op)i) << " " << total[i].count << " " << (uint64_t)ticksToUs(total[i].ticks)
      << " " << total[i].diffs << " " << recordedCount[i] << " " << (uint64_t)((double)recordedTicks[i] * recScale)
      << "\n";
  }
  f << "status_diffs " << diffs << "\n";
  for (auto& s : stats)
    f << "thread " << s.tid << " " << s.ops << " " << (uint64_t)ticksToUs(s.busyTicks) << " "
      << (uint64_t)ticksToUs(s.endTicks - s.startTicks) << "\n";
  for (auto& s : stats)
    for (auto& x : s.samples) f << "sample " << x << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
// parent

struct OpResult { std::string name; uint64_t count = 0, us = 0, diffs = 0, recCount = 0, recUs = 0; };
struct ThreadResult { uint64_t tid = 0, ops = 0, busyUs = 0, spanUs = 0; };

struct Result {
  Config cfg;
  DWORD exitCode = 0;
  double processMs = 0;
  std::map<std::string, uint64_t> scalars;
  std::vector<OpResult> ops;
  std::vector<ThreadResult> threads;
  std::vector<std::string> samples;
};

void parseResult(const std::wstring& path, Result& r)
{
  std::ifstream f(path, std::ios::binary);
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string key;
    ss >> key;
    if (key == "op") {
      OpResult o;
      ss >> o.name >> o.count >> o.us >> o.diffs >> o.recCount >> o.recUs;
      r.ops.push_back(o);
    } else if (key == "thread") {
      ThreadResult t;
      ss >> t.tid >> t.ops >> t.busyUs >> t.spanUs;
      r.threads.push_back(t);
    } else if (key == "sample") {
      r.samples.push_back(line.size() > 7 ? line.substr(7) : "");
    } else {
      uint64_t v = 0;
      ss >> v;
      r.scalars[key] = v;
    }
  }
}

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring selfPath()
{
  wchar_t buf[MAX_PATH * 4];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
  return std::wstring(buf, n);
}

bool startsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
  return _wcsnicmp(s.c_str(), prefix, wcslen(prefix)) == 0;
}

std::vector<wchar_t> buildEnv(const Config& c, bool inherit)
{
  std::vector<std::wstring> entries;
  wchar_t* block = ::GetEnvironmentStringsW();
  for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
    std::wstring e = p;
    const size_t eq = e.find(L'=', 1);  // entries like "=C:=C:\" start with '='
    const std::wstring name = e.substr(0, eq);
    bool overridden = false;
    for (auto& kv : c.env) overridden |= _wcsicmp(kv.first.c_str(), name.c_str()) == 0;
    if (overridden) continue;
    if (!inherit && startsWithNoCase(name, L"USVFS_")) continue;
    entries.push_back(e);
  }
  ::FreeEnvironmentStringsW(block);
  for (auto& kv : c.env) entries.push_back(kv.first + L"=" + kv.second);
  std::sort(entries.begin(), entries.end(),
            [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  std::vector<wchar_t> out;
  for (auto& e : entries) {
    out.insert(out.end(), e.begin(), e.end());
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  return out;
}

bool runConfig(const Options& o, const Config& c, size_t index, Result& res)
{
  res.cfg = c;
  const std::wstring resultFile = o.outPath + L".cfg" + std::to_wstring(index) + L".tmp";
  std::wstring cmd = quote(selfPath()) + L" " + quote(o.tracePath) + L" --child " + quote(resultFile) +
                     L" --timing " + (o.timingOriginal ? L"original" : L"asap");
  if (o.verify) cmd += L" --verify";
  if (o.singleThread) cmd += L" --single-thread";
  if (o.allowWrites) cmd += L" --allow-writes";

  std::vector<wchar_t> env = buildEnv(c, o.inheritEnv);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const uint64_t t0 = qpcNow();
  if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                        env.data(), nullptr, &si, &pi)) {
    std::fprintf(stderr, "replay: CreateProcess failed (%lu)\n", ::GetLastError());
    return false;
  }
  ::WaitForSingleObject(pi.hProcess, INFINITE);
  ::GetExitCodeProcess(pi.hProcess, &res.exitCode);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  res.processMs = ticksToUs(qpcNow() - t0) / 1000.0;
  if (res.exitCode == 0) parseResult(resultFile, res);
  ::DeleteFileW(resultFile.c_str());
  return res.exitCode == 0;
}

std::string jsonEscape(const std::string& s)
{
  std::string o;
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
    else if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", ch); o += b; }
    else o += (char)ch;
  }
  return o;
}

std::string writeJson(const Options& o, const std::vector<Result>& results)
{
  std::ostringstream j;
  j << "{\n  \"trace\": \"" << jsonEscape(toUtf8(o.tracePath)) << "\",\n  \"timing\": \""
    << (o.timingOriginal ? "original" : "asap") << "\",\n  \"verify\": " << (o.verify ? "true" : "false")
    << ",\n  \"configs\": [\n";
  for (size_t i = 0; i < results.size(); ++i) {
    const Result& r = results[i];
    auto sc = [&](const char* k) { auto it = r.scalars.find(k); return it == r.scalars.end() ? 0ull : (unsigned long long)it->second; };
    j << "    {\n      \"name\": \"" << jsonEscape(r.cfg.name) << "\",\n      \"env\": {";
    for (size_t k = 0; k < r.cfg.env.size(); ++k)
      j << (k ? ", " : "") << "\"" << jsonEscape(toUtf8(r.cfg.env[k].first)) << "\": \""
        << jsonEscape(toUtf8(r.cfg.env[k].second)) << "\"";
    j << "},\n      \"exit_code\": " << r.exitCode << ",\n      \"process_ms\": " << r.processMs
      << ",\n      \"wall_us\": " << sc("wall_us") << ",\n      \"recorded_ops\": " << sc("recorded_ops")
      << ",\n      \"skipped\": " << sc("skipped") << ",\n      \"unmapped\": " << sc("unmapped")
      << ",\n      \"verify_checked\": " << sc("verify_checked")
      << ",\n      \"verify_mismatches\": " << sc("verify_mismatches")
      << ",\n      \"status_diffs\": " << sc("status_diffs") << ",\n      \"ops\": {";
    for (size_t k = 0; k < r.ops.size(); ++k) {
      const OpResult& x = r.ops[k];
      j << (k ? ",\n        " : "\n        ") << "\"" << x.name << "\": {\"count\": " << x.count
        << ", \"total_us\": " << x.us << ", \"status_diffs\": " << x.diffs
        << ", \"recorded_count\": " << x.recCount << ", \"recorded_us\": " << x.recUs << "}";
    }
    j << "\n      },\n      \"threads\": [";
    for (size_t k = 0; k < r.threads.size(); ++k) {
      const ThreadResult& t = r.threads[k];
      j << (k ? ", " : "") << "{\"tid\": " << t.tid << ", \"ops\": " << t.ops << ", \"busy_us\": " << t.busyUs
        << ", \"span_us\": " << t.spanUs << "}";
    }
    j << "],\n      \"samples\": [";
    for (size_t k = 0; k < r.samples.size(); ++k) j << (k ? ", " : "") << "\"" << jsonEscape(r.samples[k]) << "\"";
    j << "]\n    }" << (i + 1 < results.size() ? "," : "") << "\n";
  }
  j << "  ]\n}\n";
  return j.str();
}

std::string table(const std::vector<Result>& results)
{
  std::string out;
  char b[256];
  std::snprintf(b, sizeof(b), "%-18s %10s %9s %9s %9s %9s %5s\n", "config", "wall_ms", "ops", "skipped",
                "mismatch", "stat_diff", "exit");
  out += b;
  for (const Result& r : results) {
    auto sc = [&](const char* k) { auto it = r.scalars.find(k); return it == r.scalars.end() ? 0ull : (unsigned long long)it->second; };
    uint64_t ops = 0;
    for (auto& x : r.ops) ops += x.count;
    std::snprintf(b, sizeof(b), "%-18.18s %10.1f %9llu %9llu %9llu %9llu %5lu\n", r.cfg.name.c_str(),
                  sc("wall_us") / 1000.0, (unsigned long long)ops, sc("skipped"), sc("verify_mismatches"),
                  sc("status_diffs"), (unsigned long)r.exitCode);
    out += b;
  }
  // per op type: count and total time (ms) per config, plus what the game saw
  out += "\nper op type: count / total ms   (recorded = in the original run, with its caches)\n";
  std::snprintf(b, sizeof(b), "%-27s %20s", "op", "recorded");
  out += b;
  for (const Result& r : results) {
    std::snprintf(b, sizeof(b), " %20.20s", r.cfg.name.c_str());
    out += b;
  }
  out += "\n";
  std::vector<std::string> names;
  for (const Result& r : results)
    for (auto& x : r.ops)
      if (std::find(names.begin(), names.end(), x.name) == names.end()) names.push_back(x.name);
  for (const std::string& n : names) {
    const OpResult* first = nullptr;
    for (const Result& r : results)
      for (auto& x : r.ops)
        if (x.name == n && !first) first = &x;
    if (!first || (first->recCount == 0 && first->count == 0)) continue;
    std::snprintf(b, sizeof(b), "%-27s %9llu/%9.1f", n.c_str(), (unsigned long long)first->recCount, first->recUs / 1000.0);
    out += b;
    for (const Result& r : results) {
      const OpResult* x = nullptr;
      for (auto& y : r.ops) if (y.name == n) x = &y;
      std::snprintf(b, sizeof(b), " %9llu/%9.1f", x ? (unsigned long long)x->count : 0ull, x ? x->us / 1000.0 : 0.0);
      out += b;
    }
    out += "\n";
  }
  out += "\nbusiest threads per config (busy ms in replayed calls / thread span ms):\n";
  for (const Result& r : results) {
    std::vector<ThreadResult> t = r.threads;
    std::sort(t.begin(), t.end(), [](auto& a, auto& b2) { return a.busyUs > b2.busyUs; });
    out += "  " + r.cfg.name + ":";
    for (size_t i = 0; i < t.size() && i < 4; ++i) {
      std::snprintf(b, sizeof(b), " [tid %llu: %.1f/%.1f]", (unsigned long long)t[i].tid, t[i].busyUs / 1000.0,
                    t[i].spanUs / 1000.0);
      out += b;
    }
    out += "\n";
  }
  for (const Result& r : results) {
    if (r.exitCode != 0) out += "config " + r.cfg.name + ": child failed, exit code " + std::to_string(r.exitCode) + "\n";
    for (auto& s : r.samples) out += "  [" + r.cfg.name + "] " + s + "\n";
  }
  return out;
}

void usage()
{
  std::fputs(
      "usage: replay.exe <trace> [--timing asap|original] [--verify] [--threads-as-recorded]\n"
      "                  [--single-thread] [--allow-writes] [--inherit-env] [--out results.json]\n"
      "                  [--config NAME KEY=VAL ...]...\n",
      stderr);
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
  Options o;
  std::vector<Config> configs;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : std::wstring(); };
    if (a == L"--timing") o.timingOriginal = next() == L"original";
    else if (a == L"--verify") o.verify = true;
    else if (a == L"--threads-as-recorded") o.singleThread = false;
    else if (a == L"--single-thread") o.singleThread = true;
    else if (a == L"--allow-writes") o.allowWrites = true;
    else if (a == L"--inherit-env") o.inheritEnv = true;
    else if (a == L"--out") o.outPath = next();
    else if (a == L"--child") o.childResult = next();
    else if (a == L"--config") {
      Config c;
      c.name = toUtf8(next());
      while (i + 1 < argc && std::wstring(argv[i + 1]).rfind(L"--", 0) != 0) {
        const std::wstring kv = argv[++i];
        const size_t eq = kv.find(L'=');
        if (eq == std::wstring::npos || eq == 0) { std::fprintf(stderr, "replay: bad KEY=VAL '%s'\n", toUtf8(kv).c_str()); return 1; }
        c.env.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
      }
      configs.push_back(std::move(c));
    } else if (!a.empty() && a[0] != L'-' && o.tracePath.empty()) o.tracePath = a;
    else { usage(); return 1; }
  }
  if (o.tracePath.empty()) { usage(); return 1; }

  if (!o.childResult.empty()) return runChild(o);

  if (o.outPath.empty()) o.outPath = o.tracePath + L".results.json";
  if (configs.empty()) configs.push_back(Config{"default", {}});

  std::vector<Result> results(configs.size());
  for (size_t i = 0; i < configs.size(); ++i) {
    std::printf("running config %s ...\n", configs[i].name.c_str());
    std::fflush(stdout);
    runConfig(o, configs[i], i, results[i]);
  }

  const std::string json = writeJson(o, results);
  const std::string tab = table(results);
  { std::ofstream f(o.outPath, std::ios::binary | std::ios::trunc); f << json; }
  { std::ofstream f(o.outPath + L".txt", std::ios::binary | std::ios::trunc); f << tab; }
  std::fputs(tab.c_str(), stdout);
  std::printf("\nresults: %s\n", toUtf8(o.outPath).c_str());
  for (auto& r : results) if (r.exitCode != 0) return 4;
  return 0;
}
