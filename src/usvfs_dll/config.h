/*
Userspace Virtual Filesystem

This file is part of usvfs.

usvfs is free software:
 * you can redistribute it and/or modify
it under the terms of the GNU General Public
 * License as published by
the Free Software Foundation, either version 3 of the
 * License, or
(at your option) any later version.

usvfs is distributed in the hope
 * that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty
 * of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public
 * License for more details.

You should have received a copy of the GNU General Public
 * License
along with usvfs. If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

// Configuration of the cache layers. Every knob is named after its environment
// variable (USVFS_READCACHE_MB, ...). Sources, highest priority first:
//   1. the environment variable,
//   2. the INI file named by USVFS_CONFIG, section/key derived from the name
//      (USVFS_READCACHE_MB -> [readcache] mb, USVFS_READCACHE -> [readcache] enabled,
//       USVFS_TRACE -> [trace] path, USVFS_CACHE_TOTAL_MB -> [cache] total_mb),
//   3. the built-in default.
// Read once at init (not live). Header-only; all state is process-wide.

#include "config_logic.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace usvfs::config
{

struct Entry
{
  std::string name, value, source;
};

struct State
{
  std::mutex m;
  bool loaded = false;
  logic::ParsedIni file;
  std::string filePath, fileStatus;
  std::vector<Entry> entries;  // in first-use order
  std::set<std::string> used;  // keys looked up (for unknown-key warnings)
};

inline State& state()
{
  static State* s = new State();  // leaked on purpose (late threads)
  return *s;
}

inline void load()
{
  State& s = state();
  std::lock_guard<std::mutex> lock(s.m);
  if (s.loaded)
    return;
  s.loaded = true;
  wchar_t wpath[2048];
  const DWORD n = GetEnvironmentVariableW(L"USVFS_CONFIG", wpath, 2048);
  if (n == 0 || n >= 2048)
    return;
  int len = WideCharToMultiByte(CP_UTF8, 0, wpath, static_cast<int>(n), nullptr, 0,
                                nullptr, nullptr);
  s.filePath.resize(len > 0 ? len : 0);
  if (len > 0)
    WideCharToMultiByte(CP_UTF8, 0, wpath, static_cast<int>(n), s.filePath.data(), len,
                        nullptr, nullptr);
  HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    s.fileStatus = "cannot open (error " + std::to_string(GetLastError()) + ")";
    return;
  }
  std::string text;
  char buf[8192];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0 &&
         text.size() < (4u << 20))
    text.append(buf, got);
  CloseHandle(h);
  s.file       = logic::parseIni(text);
  s.fileStatus = "loaded, " + std::to_string(s.file.values.size()) + " keys";
}

// raw value of a knob; `source` is "env", "file" or "" (not set)
inline bool raw(const char* name, std::string& value, const char*& source)
{
  load();
  {
    std::wstring wname(name, name + strlen(name));
    wchar_t wbuf[2048];
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), wbuf, 2048);
    if (n > 0 && n < 2048) {
      const int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n),
                                          nullptr, 0, nullptr, nullptr);
      if (len > 0) {
        value.resize(len);
        WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n), value.data(), len,
                            nullptr, nullptr);
        source = "env";
        return true;
      }
    }
  }
  State& s              = state();
  const std::string key = logic::keyFromEnvName(name);
  std::lock_guard<std::mutex> lock(s.m);
  s.used.insert(key);
  auto it = s.file.values.find(key);
  if (it != s.file.values.end() && !it->second.empty()) {
    value  = it->second;
    source = "file";
    return true;
  }
  source = "";
  return false;
}

inline void record(const char* name, const std::string& value, const char* source)
{
  State& s = state();
  std::lock_guard<std::mutex> lock(s.m);
  for (Entry& e : s.entries) {
    if (e.name == name) {
      e.value  = value;
      e.source = source;
      return;
    }
  }
  s.entries.push_back({name, value, source});
}

inline uint64_t parseNumberStrict(const std::string& v, uint64_t def, bool& ok)
{
  ok = false;
  if (v.empty())
    return def;
  uint64_t r = 0;
  for (char c : v) {
    if (c < '0' || c > '9')
      return def;
    const uint64_t d = static_cast<uint64_t>(c - '0');
    if (r > (UINT64_MAX - d) / 10)
      return def;
    r = r * 10 + d;
  }
  ok = true;
  return r;
}

// numeric knob, clamped to [lo, hi]; the recorded value is the clamped one
inline uint64_t number(const char* name, uint64_t def, uint64_t lo = 0,
                       uint64_t hi = UINT64_MAX)
{
  std::string v;
  const char* src  = "";
  uint64_t r       = def;
  const char* used = "default";
  if (raw(name, v, src)) {
    bool ok = false;
    r       = parseNumberStrict(v, def, ok);
    if (ok)
      used = src;
  }
  r = std::clamp<uint64_t>(r, lo, hi);
  record(name, std::to_string(r), used);
  return r;
}

inline bool flag(const char* name, bool def)
{
  std::string v;
  const char* src  = "";
  bool r           = def;
  const char* used = "default";
  if (raw(name, v, src)) {
    const std::string l = logic::lower(v);
    r                   = !(l == "0" || l == "false" || l == "off" || l == "no");
    used                = src;
  }
  record(name, r ? "1" : "0", used);
  return r;
}

inline std::string text(const char* name, const std::string& def = std::string())
{
  std::string v;
  const char* src = "";
  if (raw(name, v, src)) {
    record(name, v, src);
    return v;
  }
  record(name, def, "default");
  return def;
}

// true if the knob is set by the environment or the file
inline bool isSet(const char* name)
{
  std::string v;
  const char* src = "";
  return raw(name, v, src);
}

// one block listing every knob that was looked up; call after all init()s
inline void logEffective()
{
  auto logger = spdlog::get("usvfs");
  if (!logger)
    return;
  State& s = state();
  std::lock_guard<std::mutex> lock(s.m);
  std::string out =
      "effective configuration (USVFS_CONFIG=" +
      (s.filePath.empty() ? std::string("<unset>") : s.filePath + ", " + s.fileStatus) +
      ")";
  for (const Entry& e : s.entries)
    out += "\n  " + e.name + " = " + e.value + "  [" + e.source + "]";
  for (const auto& kv : s.file.values) {
    if (!s.used.count(kv.first))
      out += "\n  WARNING: unknown key in config file: " + kv.first;
  }
  for (const std::string& err : s.file.errors)
    out += "\n  WARNING: config file " + err;
  logger->info("{}", out);
}

}  // namespace usvfs::config
