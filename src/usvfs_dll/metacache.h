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
#include <string>
#include <string_view>
#include <ntdll_declarations.h>

// In-process file metadata cache: results of NtQueryAttributesFile /
// NtQueryFullAttributesFile (including "not found") and the raw record lists of
// directory enumerations are remembered. Assumption: the files are not changed
// behind our back by other processes (the modlist is frozen while the game
// runs). Everything this process writes through usvfs's hooks is invalidated and
// excluded from caching.
//
// Configuration (environment, read once in init()):
//   USVFS_METACACHE=1|0                force on/off (default: on only in SkyrimSE.exe)
//   USVFS_METACACHE_MB=128             memory cap
//   USVFS_METACACHE_FRONT=1|0          front cache for GetFileAttributesW/ExW keyed
//                                      by the virtual path (default: on with the
//                                      metacache)
//   USVFS_METACACHE_PREWARM=1|0        walk the virtual tree in the background
//                                      (default: only in SkyrimSE.exe)
//   USVFS_METACACHE_PREWARM_THREADS=4
//   USVFS_METACACHE_PREWARM_CLASSES=3,37   directory information classes to
//                                      pre-list (FILE_INFORMATION_CLASS values)

namespace usvfs::metacache
{

extern std::atomic<bool> g_enabled;

inline bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void init();

// starts the pre-warm (if configured); call after the hooks are installed
void startPrewarm();

// stops background threads, logs final statistics; not for DllMain
void shutdown();

// cache key for an NT path ("\??\C:\x"); empty if the path should not be cached
std::wstring makeKey(std::wstring_view ntPath);

// attribute cache. Return true if answered from the cache (status is the
// result to return).
// generation counter to hand to the store*() calls (read it before asking the
// system; results from before an invalidation are then dropped)
uint64_t generation();

bool lookupBasic(const std::wstring& key, PFILE_BASIC_INFORMATION out, NTSTATUS& status);
bool lookupFull(const std::wstring& key, PFILE_NETWORK_OPEN_INFORMATION out,
                NTSTATUS& status);
void storeBasic(const std::wstring& key, NTSTATUS status,
                const FILE_BASIC_INFORMATION* info, uint64_t generation);
void storeFull(const std::wstring& key, NTSTATUS status,
               const FILE_NETWORK_OPEN_INFORMATION* info, uint64_t generation);

// replacement for the real NtQueryDirectoryFile (synchronous calls without
// event/apc, RestartScan FALSE) used by usvfs's directory merging. `dirPath` is
// the path the handle was opened with, empty if unknown.
//
// `resolveContent` maps the path above to the directory whose content the
// handle really lists (after usvfs's redirection); nothing is cached for
// excluded or written directories, on either side.
NTSTATUS queryDirectory(const std::wstring& dirPath, HANDLE handle,
                        PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length,
                        FILE_INFORMATION_CLASS infoClass, BOOLEAN singleEntry,
                        PUNICODE_STRING mask,
                        std::wstring (*resolveContent)(const std::wstring&));

// forget the enumeration state of a handle; call before closing it
void onClose(HANDLE handle);

// Front cache: the final results of GetFileAttributesW / GetFileAttributesExW
// (GetFileExInfoStandard only) keyed by the virtual path as the application passed
// it. Valid while the store generation is unchanged, so every invalidation
// (noteWrite, noteDosPath, noteSetInformation) and every change of the usvfs
// mappings (noteMappingChange) drops all entries.
struct FrontTicket
{
  std::wstring key;  // empty: not cacheable
  uint64_t gen = 0;
};

struct FrontResult
{
  bool ok = false;
  DWORD err = 0;   // GetLastError of the call
  DWORD attr = 0;  // GetFileAttributesW result (ok)
  WIN32_FILE_ATTRIBUTE_DATA data = {};  // GetFileAttributesExW (ok)
};

// true if answered (`out` is filled); on a miss `ticket` is for frontStore()
bool frontLookup(bool ex, const wchar_t* name, FrontResult& out, FrontTicket& ticket);
// `reroutedName`: the real path the call was redirected to (nothing is cached
// if that is excluded or written). `canonicalName`: the absolute path the call
// resolved the name to; must match the ticket key (cwd changed meanwhile otherwise)
void frontStore(const FrontTicket& ticket, bool ex, const FrontResult& result,
                const wchar_t* reroutedName, const wchar_t* canonicalName);

// the usvfs mappings (redirection tree, delete / fake-directory trackers) changed
void noteMappingChange();

// invalidation
bool isWriteIntent(ACCESS_MASK desiredAccess, ULONG createDisposition);
void noteWrite(std::wstring_view ntPath);

// like noteWrite, for a Win32 path as seen by the kernel32 hooks. `tree`: the
// path may be a directory whose content goes along (remove, rename). Relative
// paths clear the cache.
void noteDosPath(const wchar_t* dosPath, bool tree);
bool isMutatingClass(FILE_INFORMATION_CLASS infoClass);
void noteSetInformation(std::wstring_view handlePath, std::wstring_view rootPath,
                        FILE_INFORMATION_CLASS infoClass, PVOID info, ULONG length);

}  // namespace usvfs::metacache
