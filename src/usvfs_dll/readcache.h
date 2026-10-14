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

#include <atomic>
#include <ntdll_declarations.h>

// In-process read cache: small NtReadFile calls on synchronous, read-only disk
// handles are answered from a memory copy of the whole file instead of a
// syscall. See readcache.cpp for the design.
//
// Configuration (environment variables, read once in init()):
//   USVFS_READCACHE=0              disable (hooks are plain passthrough)
//   USVFS_READCACHE_MB=1024        LRU budget in MiB
//   USVFS_READCACHE_MAXFILE_MB=64  largest cached file in MiB
//   USVFS_READCACHE_ASYNC=1        fill in background workers (default off)
//   USVFS_READCACHE_THREADS=4      number of background workers

namespace usvfs::readcache
{

extern std::atomic<bool> g_enabled;

// the one branch every hook takes when the cache is off
inline bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

// reads configuration; must be called once before the hooks are installed
void init();

// stops background workers, logs the final statistics and turns the cache
// off. Must NOT be called from DllMain (it waits for the workers).
void shutdown();

enum class Peek { Unknown, Ineligible, Eligible };

// cheap look at the classification of a handle (no reference counting, no
// hook context); used to let ineligible handles bypass everything
Peek peek(HANDLE handle);

// counts a read that was passed to the real function without further ado
void notePassthrough();

// drops the state of a handle; must be called before the handle is closed
void onClose(HANDLE handle);

// true while the calling thread executes a guarded memory copy whose access
// violations are expected and handled (used to keep the crash dump handler quiet)
bool inProtectedCopy();

// replacements for the corresponding ntdll functions; the real functions are
// invoked as necessary (and bypass our hooks when called from inside a hook)
NTSTATUS read(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
              PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
              ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

NTSTATUS queryInformation(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                          PVOID FileInformation, ULONG Length,
                          FILE_INFORMATION_CLASS FileInformationClass);

NTSTATUS setInformation(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                        PVOID FileInformation, ULONG Length,
                        FILE_INFORMATION_CLASS FileInformationClass);

}  // namespace usvfs::readcache
