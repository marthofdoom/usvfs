# usvfs fork handoff (Skyrim/LoreRim under Proton on ullr)

## Purpose
Fork of MO2's usvfs (Omni-guides/usvfs lineage, remote `origin` = marthofdoom/usvfs, `omni` = Omni-guides).
Goal: make Skyrim SE launch/load faster under Wine/Proton on bcachefs by caching the file-API demand
inside the injected usvfs DLL (see `/mnt/gaming/modlists/Projects/Linux-Storage-DLL/{README.md,Docs/}`).
`harness` adds a trace + replay tool so layers can be A/B'd in ~1-2 min without the game.

## Branches / tags
- `async-read-prefetch`: the cache layers (owned by another agent; do not commit there from here).
- `harness`: based on async-read-prefetch + trace/replay (this branch). Merge later; shared-file edits kept small.
- tag `v0.5.6.1-readcache.1`: earlier readcache release. Other tags: v0.5.7.x, wine-shortname-*.
- CI: `.github/workflows/build.yml` (windows-2022, MSBuild, `vsbuild/usvfs.sln`); triggers on master,
  `async-read-prefetch*`, `harness`. Check runs with `gh run list -R marthofdoom/usvfs --branch harness`
  (plain `gh run list` in this dir points at the wrong remote). You cannot compile locally.

## Layers (env toggles read once at DLL init; full list in the headers)
- L1-L3 `src/usvfs_dll/readcache.{h,cpp}` (+ `readcache_logic.h`): in-process whole-file cache for small
  NtReadFile on read-only sync disk handles. `USVFS_READCACHE=0` off, `_MB`, `_MAXFILE_MB`, `_ADMIT_READS`,
  `_ASYNC` (background fill), `_SPLIT` (+`_SPLIT_MIN_KB/_CHUNK_KB`), `_RA` (+`_RA_MB`), `_THREADS`.
  Also serves file-information snapshots (tryServeInfo) and tracks file position (probation handles).
- L4 `src/usvfs_dll/metacache.{h,cpp}` (+ `metacache_logic.h`): metadata/directory cache and virtual-tree
  prewarm. `USVFS_METACACHE=1|0` (default on only in SkyrimSE.exe!), `_MB`, `_PREWARM`, `_PREWARM_THREADS`,
  `_PREWARM_CLASSES`. Replay processes must set USVFS_METACACHE=1 explicitly to test it.
- Exact L1/L2/L3 boundaries: read the headers; my example configs in harness.md are a guess, verify them.
- Trace (harness): `src/usvfs_dll/trace.{h,cpp}`, `trace_format.h`. `USVFS_TRACE=<win path>`,
  `USVFS_TRACE_CRC=1`. Init logs one line to the usvfs log ("trace enabled/disabled: reason").

## Hook structure
- `hooks/ntdll.cpp`: bodies renamed `impl_Nt*` (NtCreateFile/OpenFile/ReadFile/QueryInformationFile/
  SetInformationFile/QueryDirectoryFile(Ex)/QueryAttributes/QueryFullAttributes/Close); declared in
  `hooks/ntdll_impl.h`. The installed `hook_Nt*` wrappers live in `hooks/ntdll_traced.cpp` (trace::Scope +
  record). Any change to hook logic goes in the `impl_*` bodies.
- `hooks/kernel32.cpp`: GetFileAttributesW/ExW, FindFirstFileExW, GetPrivateProfileString/Section A/W,
  DeleteFileW, MoveFileW/ExW, WritePrivateProfileString A/W, SetCurrentDirectoryW are `impl_*`
  (`kernel32_impl.h`), wrapped in `kernel32_traced.cpp`. `hook_FindNextFileW`/`hook_FindClose` exist only for
  tracing and are installed by `HookManager::initHooks` only when `trace::enabled()`.
- `trace::Scope`: inert unless tracing is on; skips if any hook group is active on the thread (our workers,
  nested calls), thread_local depth guards nesting; kernel32 wrappers call keepLastError().
  App memory is read only through `readcache::guardedCopy`. Recorders catch all exceptions.
- `hookmanager.cpp`: trace::init() before readcache::init(); trace::shutdown() after readcache::shutdown().
  The writer thread flushes every 250 ms (a killed game loses <=0.25 s).

## Trace format (`trace_format.h`, unit-tested in `test/shared_test/trace_format_test.cpp`)
Header (40 B) + blocks `[u32 size][records of one thread]`; record = op, presence mask, varints; strings via
StringDef records. Ops: ntdll file calls, kernel32 GetAttr/GetAttrEx/FindFirst/FindNext/FindClose/ProfString/
ProfSection, SetCwd (first record = cwd at init), WriteMarker (writes, never replayed).
Field meaning per op is documented at the top of the file. Timestamps = QPC at hook entry.

## replay.exe (`src/replay/main.cpp`, `vsbuild/replay.vcxproj`)
Run as an MO2 executable (inside the VFS). Docs: `/mnt/gaming/modlists/Projects/Linux-Storage-DLL/Tools/harness.md`
and `Tools/harness-cold.sh DIR...` (posix_fadvise DONTNEED for cold runs).
`replay_x64.exe <trace> [--timing asap|original] [--verify] [--single-thread] [--allow-writes] [--inherit-env]
 [--from S --to S --limit-seconds N] [--out FILE] [--config NAME KEY=VAL ...]...`
- Parent only orchestrates: spawns itself per config (env block = inherited minus USVFS_* plus overrides, short
  command line), each child maps+indexes the trace and replays one pass with lazy per-thread decode.
- Outputs: `FILE` (JSON), `FILE.txt` (table incl. skipped/unmapped, per-op count/ms vs recorded, p50/p95/max
  latency per op, busiest threads), `FILE.log` (progress every 5 s, CreateProcess errors, first 20 failing
  paths). Look at the .log first when something dies.
- Handles: reused handle values are resolved by time (instance = latest create returned before the use);
  find handles separately. Window mode re-opens handles open at window start (file positions restart at 0).
- Safety: opens with write/delete/non-FILE_OPEN intent and non-position SetInformation are skipped.

## Known fidelity limits
- Only hooked ntdll/kernel32 calls: no NtWriteFile, mmap/sections, kernel32-only paths. Launch via
  GetFileAttributes*/FindFirst storm is replayed through the same hooks; ntdll calls made inside kernel32
  hooks are not recorded (they are part of the kernel32 call).
- Replay runs against current file contents; status diffs appear if the mod list changed or the cwd differs
  (cwd is now restored from the trace; verify the diff count dropped).
- Trace reading is via a mapped view (not hooked reads) so it does not pollute the read cache.
- Async-handle reads wait on an event; completion order is not reproduced. `--timing original` can fall
  behind but never runs ahead.
- Observed (not a harness bug): GetFileAttributesExW costs ~80 us/call even with metacache hits (295k calls,
  23.8 s baseline vs 22.8 s cached): time is inside usvfs's own kernel32 hook before our cache. Not fixed.

## Wine/Proton facts that bit us
- Wine reports FILE_DEVICE_DISK_FILE_SYSTEM; classification of "disk" handles must not assume FILE_DEVICE_DISK.
- FileAllInformation under Wine returns Mode/Access = 0; don't trust them for eligibility.
- FileFsVolumeInformation is answered via mountmgr (slow); don't call it per file.
- Steam launch options strip backslashes: set `USVFS_TRACE=Z:\...` paths carefully (test that the file appears;
  forward slashes or an MO2-side env may be needed).
- usvfs redirects newly created files to `overwrite`; tools run from MO2 write results there unless the path is
  outside the virtual tree (use Z:\mnt\...\logs\).
- usvfs file nodes can carry FLAG_DIRECTORY wrongly; the metacache has to tolerate it.
- `HookCallContext`/HookStack uses `std::bitset<12>`; `MutExHookGroup::LAST` (== NO_GROUP, 12) is out of
  range: probing it throws std::out_of_range (this crashed the game once with tracing on). Loop `< LAST`.

## Deploy
- Download CI artifacts: `gh run download <id> -R marthofdoom/usvfs -n usvfs-release_x64` (DLL/bins in
  `bin/`, libs in `lib/`) and `-n replay_x64` (`replay_x64.exe`). Run id from `gh run list -R ... --branch harness`.
- Install: copy `usvfs_x64.dll` (and proxy exe if changed) over `/mnt/gaming/modlists/LoreRim/usvfs_x64.dll`.
  Keep backups there: `usvfs_x64.dll.stock-0.5.6.1`, `.omni-0.5.6.1` exist. Logs: `LoreRim/logs/usvfs-*.log`.
- Capture: Steam launch option `USVFS_TRACE=Z:\mnt\gaming\modlists\Projects\Linux-Storage-DLL\logs\runN.trc
  USVFS_TRACE_CRC=1 %command%`. run7 (625 MB, whole launch+load) exists in that logs dir.
  First 120 s replay of run7: baseline 103.0 s, L1-4noRA 85.5 s (before the fidelity fixes below).

## Open items
- Last commit added: cwd record+restore, FindNext/FindClose call-for-call, latency histogram, failing-path
  samples. CI is green but NOT run under Wine: re-run the replay and check status diffs (was 23,147) and that
  FindFirst replay time is near recorded (was 57-72 s vs 12 s).
- Old traces (run7) lack SetCwd/FindNext records: capture a new trace to benefit.
- harness.md example config table (L1..L4 knob sets) is unverified against the headers.
- Possible next work: make the GetFileAttributesExW kernel32 hook path cheap (cache before group/lock work);
  replay file-position fixup for window starts; merge harness into async-read-prefetch once stable.
