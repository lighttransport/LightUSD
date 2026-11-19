// SPDX-License-Identifier: Apache-2.0
// Minimal POSIX shims so the C++ tests/benches build with MSVC. Include
// instead of <unistd.h>/<dirent.h>/<sys/resource.h> when _WIN32 is defined.
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <psapi.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <filesystem>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

#define popen _popen
#define pclose _pclose
#define getpid _getpid
#define unlink _unlink
#define rmdir _rmdir
#define getcwd _getcwd
#define chdir _chdir
#define mkdir(path, mode) _mkdir(path)

inline void lusd_compat_mkdirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] == '/' || path[i] == '\\') _mkdir(path.substr(0, i).c_str());
  }
}

// In-place mkdtemp; creates missing parent directories (e.g. "/tmp/").
inline char* mkdtemp(char* tmpl) {
  size_t n = strlen(tmpl);
  if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) return nullptr;
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  lusd_compat_mkdirs(tmpl);
  for (int attempt = 0; attempt < 100; ++attempt) {
    unsigned r = (unsigned)GetTickCount() * 2654435761u + (unsigned)attempt * 40503u +
                 (unsigned)GetCurrentProcessId();
    for (int i = 0; i < 6; ++i) {
      tmpl[n - 6 + i] = kChars[r % (sizeof(kChars) - 1)];
      r = r * 1103515245u + 12345u;
    }
    if (_mkdir(tmpl) == 0) return tmpl;
  }
  return nullptr;
}

// <dirent.h> subset.
struct dirent { char d_name[MAX_PATH]; };
struct LusdDir_ {
  HANDLE h = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAA fd;
  bool first = true;
  dirent ent;
};
typedef LusdDir_ DIR;
inline DIR* opendir(const char* path) {
  std::string pat = std::string(path) + "\\*";
  DIR* d = new DIR;
  d->h = FindFirstFileA(pat.c_str(), &d->fd);
  if (d->h == INVALID_HANDLE_VALUE) { delete d; return nullptr; }
  return d;
}
inline dirent* readdir(DIR* d) {
  if (!d->first && !FindNextFileA(d->h, &d->fd)) return nullptr;
  d->first = false;
  strncpy_s(d->ent.d_name, d->fd.cFileName, _TRUNCATE);
  return &d->ent;
}
inline int closedir(DIR* d) { FindClose(d->h); delete d; return 0; }

#define _SC_PAGESIZE 30
inline long sysconf(int name) {
  if (name == _SC_PAGESIZE) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwPageSize;
  }
  return -1;
}

// <sys/resource.h> subset: only ru_maxrss (kilobytes) is provided.
#define RUSAGE_SELF 0
struct rusage { long ru_maxrss; };
inline int getrusage(int, rusage* ru) {
  PROCESS_MEMORY_COUNTERS pmc;
  ru->ru_maxrss = 0;
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
  ru->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024);
  return 0;
}

// "mkdir -p a b c" without a shell. Returns 0 on success.
inline int lusd_mkdir_p(const std::string& spaced_paths) {
  size_t i = 0;
  while (i < spaced_paths.size()) {
    size_t j = spaced_paths.find(' ', i);
    if (j == std::string::npos) j = spaced_paths.size();
    if (j > i) {
      std::string p = spaced_paths.substr(i, j - i);
      lusd_compat_mkdirs(p + "/");
    }
    i = j + 1;
  }
  return 0;
}

// Forward-slash form of a path (no-op on POSIX), for path comparisons.
inline std::string lusd_slashes(std::string p) {
  for (char& c : p) if (c == '\\') c = '/';
  return p;
}
// Real absolute path for a POSIX-style "/tmp/..." literal: Windows resolves a
// bare "/tmp" against the current drive, and identifiers must then agree.
inline std::string lusd_abs(const std::string& p) {
  return std::filesystem::absolute(p).generic_string();
}
#else
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
inline std::string lusd_slashes(std::string p) { return p; }
inline std::string lusd_abs(const std::string& p) { return p; }
inline int lusd_mkdir_p(const std::string& spaced_paths) {
  return std::system(("mkdir -p " + spaced_paths).c_str());
}
#endif
