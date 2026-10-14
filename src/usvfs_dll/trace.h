#pragma once

#include <atomic>
#include <ntdll_declarations.h>

// Application file-API trace for the replay harness (replay.exe). Records what
// the APPLICATION asked of ntdll, at hook entry, regardless of what our caches
// serve. Format: trace_format.h.
//
// Configuration (environment variables, read once in init()):
//   USVFS_TRACE=<windows path>   enable and write the trace there (if another
//                                process holds the file, "<path>.<pid>" is used)
//   USVFS_TRACE_CRC=1            also record the crc32 of data returned by reads
//
// Cost when off: one relaxed atomic load per hooked call.

namespace usvfs::trace
{

extern std::atomic<bool> g_enabled;

inline bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

// reads configuration and opens the output; must be called before the hooks
// are installed (the output file is opened with the real, unhooked functions)
void init();

// stops the writer thread and flushes. Must NOT be called from DllMain.
void shutdown();

// One per hook invocation (declare it first thing in the hook). Inert unless
// tracing is on and the call comes from the application itself, i.e. not from
// inside another hook or from one of our own worker threads. Also preserves
// GetLastError across the recording.
class Scope
{
public:
  Scope()
  {
    if (enabled())
      begin();
  }
  ~Scope()
  {
    if (m_on)
      end();
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  bool on() const { return m_on; }
  uint64_t startTicks() const { return m_start; }

private:
  void begin();
  void end();

  bool m_on = false;
  uint64_t m_start = 0;
  unsigned long m_err = 0;
};

// each is called after the real work, only if scope.on()
void recCreate(const Scope& s, bool isOpen, PHANDLE FileHandle, POBJECT_ATTRIBUTES oa,
               ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options,
               ULONG fileAttributes, NTSTATUS status);
void recRead(const Scope& s, HANDLE handle, HANDLE event, PIO_STATUS_BLOCK iosb,
             PVOID buffer, ULONG length, PLARGE_INTEGER byteOffset, NTSTATUS status);
void recQueryInfo(const Scope& s, HANDLE handle, PIO_STATUS_BLOCK iosb, ULONG length,
                  FILE_INFORMATION_CLASS cls, NTSTATUS status);
void recSetInfo(const Scope& s, HANDLE handle, PIO_STATUS_BLOCK iosb, PVOID info,
                ULONG length, FILE_INFORMATION_CLASS cls, NTSTATUS status);
void recQueryDir(const Scope& s, bool ex, HANDLE handle, PIO_STATUS_BLOCK iosb,
                 ULONG length, FILE_INFORMATION_CLASS cls, ULONG flags,
                 PUNICODE_STRING mask, NTSTATUS status);
void recQueryAttr(const Scope& s, bool full, POBJECT_ATTRIBUTES oa, NTSTATUS status);
void recClose(const Scope& s, HANDLE handle, NTSTATUS status);

}  // namespace usvfs::trace
