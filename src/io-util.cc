// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
#include "core/native-io.hh"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <io.h>
#include <windows.h>  // include API for expanding a file path

#ifndef LIGHTUSD_MMAP_SUPPORTED
#define LIGHTUSD_MMAP_SUPPORTED (1)
#endif

#ifdef _MSC_VER
#undef NOMINMAX
#endif

#undef WIN32_LEAN_AND_MEAN

#if defined(__GLIBCXX__)  // mingw

#include <fcntl.h>  // _O_RDONLY

#include <ext/stdio_filebuf.h>  // fstream (all sorts of IO stuff) + stdio_filebuf (=streambuf)

#endif  // __GLIBCXX__

#else  // !_WIN32

#if defined(LIGHTUSD_BUILD_IOS) || defined(TARGET_OS_IPHONE) || \
    defined(TARGET_IPHONE_SIMULATOR) || defined(__ANDROID__) || \
    defined(__EMSCRIPTEN__) || defined(__wasi__)

// non posix

// TODO: Add mmmap or similar feature support to these system.

#else

// Assume Posix
#include <sys/mman.h>
#include <sys/stat.h>

#ifndef LIGHTUSD_MMAP_SUPPORTED
#define LIGHTUSD_MMAP_SUPPORTED (1)
#endif

#endif

#endif  // _WIN32

#ifndef LIGHTUSD_MMAP_SUPPORTED
#define LIGHTUSD_MMAP_SUPPORTED (0)
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#if !defined(__wasi__)
#include "external/filesystem/include/ghc/filesystem.hpp"
#endif

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "io-util.hh"
#include "str-util.hh"

namespace lightusd {
namespace io {

bool OpenInputFile(std::ifstream *file, const std::string &filepath,
                   std::ios_base::openmode mode) {
  if (!::lightusd::NativeIOEnabled()) return false;

  if (!file) return false;
#ifdef _WIN32
#if defined(__GLIBCXX__)
  // libstdc++ MinGW needs the UTF-16 Win32 path for non-ASCII filenames.
  file->open(UTF8ToWchar(filepath).c_str(), mode);
#elif defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  // LLVM-MinGW libc++ accepts the wchar_t overload, but not a
  // ghc::filesystem::path passed directly to std::ifstream.
  file->open(UTF8ToWchar(filepath).c_str(), mode);
#else
  file->open(filepath.c_str(), mode);
#endif
#else
  file->open(filepath.c_str(), mode);
#endif
  return static_cast<bool>(*file);
}

bool OpenOutputFile(std::ofstream *file, const std::string &filepath,
                    std::ios_base::openmode mode) {
  if (!::lightusd::NativeIOEnabled()) return false;

  if (!file) return false;
#ifdef _WIN32
#if defined(__GLIBCXX__) || defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  file->open(UTF8ToWchar(filepath).c_str(), mode);
#else
  file->open(filepath.c_str(), mode);
#endif
#else
  file->open(filepath.c_str(), mode);
#endif
  return static_cast<bool>(*file);
}

#if defined(_WIN32)
namespace {

// from llama.cpp ----
// MIT license
[[maybe_unused]] std::string GetErrorMessageWin32(DWORD error_code) {
  std::string ret;
  LPSTR lpMsgBuf = nullptr;
  DWORD bufLen = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&lpMsgBuf), 0, nullptr);
  if (!bufLen) {
    ret = "Win32 error code: " + std::to_string(error_code);
  } else {
    ret = lpMsgBuf;
    LocalFree(lpMsgBuf);
  }

  return ret;
}

static std::string llama_format_win_err(DWORD err) {
  LPSTR buf;
  size_t size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buf), 0, nullptr);
  if (!size) {
    return "FormatMessageA failed";
  }
  std::string ret(buf, size);
  LocalFree(buf);
  return ret;
}
// ----

}  // namespace
#endif

#ifdef LIGHTUSD_ANDROID_LOAD_FROM_ASSETS
AAssetManager *asset_manager = nullptr;
#endif

bool IsMMapSupported() {
  if (!::lightusd::NativeIOEnabled()) return false;

#if LIGHTUSD_MMAP_SUPPORTED
  return true;
#else
  return false;
#endif
}


#if defined(_WIN32)
static bool MMapFileImplWin32(HANDLE hFile, MMapFileHandle *handle, bool writable, std::string *err) {

  uint64_t size{0};
  {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hFile, &sz)) {
      if (err) {
        (*err) +=
            "GetFileSizeEx failed: " + llama_format_win_err(GetLastError());
      }
      return false;
    }

    size = static_cast<uint64_t>(sz.QuadPart);
  }

  HANDLE hMapping = CreateFileMapping(
      hFile, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
  if (hMapping == nullptr) {
    if (err) {
      (*err) +=
          "CreateFileMapping failed: " + llama_format_win_err(GetLastError());
    }
    return false;
  }
  void *addr = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
  DWORD lastError = GetLastError();
  CloseHandle(hMapping);
  if (!addr) {
    if (err) {
      (*err) += "MapViewOfFile failed: " + llama_format_win_err(lastError);
    }
    return false;
  }

  handle->addr = reinterpret_cast<uint8_t *>(addr);
  handle->size = size;
  handle->writable = writable;

  return true;

}
#endif

bool MMapFile(const std::string &filepath, MMapFileHandle *handle, bool writable, std::string *err) {
  if (!::lightusd::NativeIOAvailable(err)) return false;


#if LIGHTUSD_MMAP_SUPPORTED
#if defined(_WIN32)
  //int fd = open(filepath.c_str(), writable ? O_RDWR : O_RDONLY);

  std::wstring unicode_filepath = UTF8ToWchar(filepath);
  HANDLE hFile = CreateFileW(unicode_filepath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    if (err) {
      (*err) += "Failed to open file.";
    }
    return false;
  }

  if (!MMapFileImplWin32(hFile, handle, writable, err)) {
    return false;
  }

  handle->hFile = hFile;
  handle->filename = filepath;
  handle->unicode_filename = unicode_filepath;

  return true;

#else   // !WIN32
  // POSIX: open with O_NOFOLLOW to reject symlinks (symlink-following
  // could be used to read sensitive files via crafted USD paths).
  int fd = ::open(filepath.c_str(),
                  writable ? (O_RDWR | O_NOFOLLOW) : (O_RDONLY | O_NOFOLLOW));
  if (fd < 0) {
    if (err) {
      (*err) += "open failed.";
    }
    return false;
  }

  size_t size = size_t(::lseek(fd, 0, SEEK_END));
  ::lseek(fd, 0, SEEK_SET);

  if (size == 0) {
    if (err) {
      (*err) += "File size is zero.";
    }
    ::close(fd);
    return false;
  }

  int flags = MAP_PRIVATE;  // delayed access
  void *addr =
      mmap(nullptr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, flags,
           fd, 0);
  if (addr == MAP_FAILED) {
    if (err) {
      (*err) += "mmap failed.";
    }
    ::close(fd);
    return false;
  }

  handle->addr = reinterpret_cast<uint8_t *>(addr);
  handle->size = uint64_t(size);
  handle->writable = writable;
  handle->filename = filepath;
  ::close(fd);

  return true;
#endif  // !WIN32
#else   // !LIGHTUSD_MMAP_SUPPORTED
  (void)filepath;
  (void)handle;
  (void)writable;
  (void)err;
  return false;
#endif
}

#if defined(_WIN32)
bool MMapFile(const std::wstring &unicode_filepath, MMapFileHandle *handle, bool writable, std::string *err) {
  if (!::lightusd::NativeIOAvailable(err)) return false;


#if LIGHTUSD_MMAP_SUPPORTED
  //int fd = open(filepath.c_str(), writable ? O_RDWR : O_RDONLY);

  HANDLE hFile = CreateFileW(unicode_filepath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    if (err) {
      (*err) += "Failed to open file.";
    }
    return false;
  }

  if (!MMapFileImplWin32(hFile, handle, writable, err)) {
    CloseHandle(hFile);
    return false;
  }

  handle->hFile = hFile;
  handle->filename = WcharToUTF8(unicode_filepath);
  handle->unicode_filename = unicode_filepath;

  return true;

#else
  (void)unicode_filepath;
  (void)handle;
  (void)writable;
  return false;
#endif
}
#endif

bool UnmapFile(const MMapFileHandle &handle, std::string *err) {
#if LIGHTUSD_MMAP_SUPPORTED
#if defined(_WIN32)
  bool result = true;

  if (handle.addr && handle.size) {
    if (!UnmapViewOfFile(handle.addr)) {
      if (err) {
        (*err) += "warning: UnmapViewOfFile failed: " +
                  llama_format_win_err(GetLastError());
      }

      // may be ok for now.
      // result = false;
    }
  } else {
    // arg is invalid
    result = false;
  }

  if (handle.hFile != nullptr) {
    if (!CloseHandle(handle.hFile)) {
      if (err) {
        (*err) += "CloseHandle failed: " +
                  llama_format_win_err(GetLastError());
      }

      result =false;
    }
  }

  return result;
#else  // !WIN32
  if (handle.addr && handle.size) {
    int ret = munmap(reinterpret_cast<void *>(handle.addr), size_t(handle.size));
    if (ret != 0) { // 0 = success
      if (err) {
        (*err) += "warning: munmap failed.";
      }
    }
    // ignore return code for now
    return true;
  }
  return false;
#endif
#else  // !LIGHTUSD_MMAP_SUPPORTED
  (void)handle;
  (void)err;
  return false;
#endif
}

// Full desktop platforms (POSIX + Windows) that have a real filesystem to glob
// against. Restricted/sandboxed targets do only string (env/tilde) expansion.
#if !defined(__wasi__) && !defined(LIGHTUSD_BUILD_IOS) &&      \
    !defined(TARGET_OS_IPHONE) && !defined(TARGET_IPHONE_SIMULATOR) && \
    !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__OpenBSD__)
#define LIGHTUSD_HAVE_GLOB_FS 1
#else
#define LIGHTUSD_HAVE_GLOB_FS 0
#endif

// Simple glob matcher (`*` = any run, `?` = one char). Classic linear
// two-pointer algorithm with a single backtrack point; O(len(pattern) +
// len(name)) and NO recursion, so it cannot blow up on adversarial input.
bool SimpleGlobMatch(const std::string &pattern, const std::string &name) {
  const size_t P = pattern.size(), S = name.size();
  size_t p = 0, s = 0;
  size_t star = std::string::npos;  // last '*' position in pattern
  size_t ss = 0;                    // name position when '*' was seen
  while (s < S) {
    if (p < P && (pattern[p] == '?' || pattern[p] == name[s])) {
      ++p;
      ++s;
    } else if (p < P && pattern[p] == '*') {
      star = p++;
      ss = s;
    } else if (star != std::string::npos) {
      // Backtrack: let the last '*' consume one more character.
      p = star + 1;
      s = ++ss;
    } else {
      return false;
    }
  }
  while (p < P && pattern[p] == '*') ++p;  // trailing stars match empty
  return p == P;
}

namespace {

// POSIX-style leading '~' (-> HOME) plus $VAR / ${VAR} caller-variable
// substitution. Pure string transform — no filesystem, no shell, no brace or
// glob expansion — so it is safe on any input (this replaces the useful,
// deterministic part of the old wordexp() call). Undefined variables expand to
// empty (matching shell default).
[[maybe_unused]] std::string ExpandEnvAndTilde(
    const std::string &in, const std::map<std::string, std::string> &variables) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  if (!in.empty() && in[0] == '~' && (in.size() == 1 || in[1] == '/')) {
    const auto home = variables.find("HOME");
    if (home != variables.end()) out += home->second;
    i = 1;  // skip the '~'; the following '/...' (if any) is copied below
  }
  for (; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '$' && (i + 1) < in.size()) {
      size_t j = i + 1;
      std::string name;
      const bool braced = (in[j] == '{');
      if (braced) {
        ++j;
        while (j < in.size() && in[j] != '}') name += in[j++];
        if (j < in.size()) ++j;  // consume '}'
      } else {
        while (j < in.size() &&
               (std::isalnum(static_cast<unsigned char>(in[j])) ||
                in[j] == '_')) {
          name += in[j++];
        }
      }
      if (!name.empty()) {
        const auto val = variables.find(name);
        if (val != variables.end()) out += val->second;
        i = j - 1;  // -1: the for-loop's ++i re-advances
        continue;
      }
    }
    out += c;
  }
  return out;
}

}  // namespace

#if LIGHTUSD_HAVE_GLOB_FS
namespace {

inline bool HasGlobWildcard(const std::string &s) {
  return s.find_first_of("*?") != std::string::npos;
}

}  // namespace
#endif

std::vector<std::string> SimpleGlob(const std::string &pattern,
                                    size_t max_results) {
  if (!::lightusd::NativeIOEnabled()) return {};

  std::vector<std::string> results;
  if (max_results == 0) return results;

  // Reject parent-directory references (`..`) to prevent directory traversal.
  // (The only wildcard chars are '*' and '?', and `..` is never a valid glob.)
  if (pattern.find("..") != std::string::npos) {
    return results;
  }

#if LIGHTUSD_HAVE_GLOB_FS
  namespace fs = ghc::filesystem;

  // No wildcard: resolve to the literal path iff it exists.
  if (!HasGlobWildcard(pattern)) {
    std::error_code ec;
    if (fs::exists(fs::path(pattern), ec)) results.push_back(pattern);
    return results;
  }

  const bool absolute = !pattern.empty() && pattern[0] == '/';

  // Split into '/'-separated components (empty components from '//' skipped).
  std::vector<std::string> comps;
  {
    std::string cur;
    for (char c : pattern) {
      if (c == '/') {
        if (!cur.empty()) comps.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) comps.push_back(cur);
  }
  if (comps.empty()) return results;

  auto join = [](const std::string &dir, const std::string &name) {
    if (dir == "/") return std::string("/") + name;
    if (dir == ".") return name;  // relative: no "./" prefix
    return dir + "/" + name;
  };

  std::vector<std::string> cands;
  cands.push_back(absolute ? std::string("/") : std::string("."));

  for (size_t ci = 0; ci < comps.size(); ++ci) {
    const std::string &comp = comps[ci];
    std::vector<std::string> next;
    if (!HasGlobWildcard(comp)) {
      // Literal component: append without a filesystem probe (a later wildcard
      // component's directory listing, or the final existence filter, validates
      // it). '.' and '..' pass through unchanged.
      for (const std::string &cand : cands) {
        next.push_back(join(cand, comp));
        if (next.size() >= max_results) break;
      }
    } else {
      // Wildcard component: list each candidate directory and keep matches.
      for (const std::string &cand : cands) {
        std::error_code ec;
        fs::directory_iterator it(fs::path(cand), ec);
        if (ec) continue;
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {
          if (ec) break;
          const std::string name = it->path().filename().string();
          if (SimpleGlobMatch(comp, name)) {
            next.push_back(join(cand, name));
            if (next.size() >= max_results) break;
          }
        }
        if (next.size() >= max_results) break;
      }
    }
    if (next.empty()) return results;  // nothing matched: no results
    cands.swap(next);
  }

  // Keep only paths that actually exist (validates trailing literal components).
  for (const std::string &c : cands) {
    std::error_code ec;
    if (fs::exists(fs::path(c), ec)) {
      results.push_back(c);
      if (results.size() >= max_results) break;
    }
  }
  return results;
#else
  (void)pattern;
  (void)max_results;
  return results;  // no filesystem globbing on this platform
#endif
}

namespace {
std::string ExpandPath(const std::string &_filepath,
                       const std::map<std::string, std::string> *variables) {
  std::string filepath = _filepath;
  if (filepath.size() > 2048) {
    // file path too large.
    // TODO: Report warn.
    filepath.resize(2048);
  }
  if (filepath.empty()) return std::string();

  std::string expanded = variables ? ExpandEnvAndTilde(filepath, *variables) : filepath;
#ifdef _WIN32
  if (variables) {
    for (const auto &entry : *variables) {
      const std::string key = "%" + entry.first + "%";
      size_t pos = 0;
      while ((pos = expanded.find(key, pos)) != std::string::npos) {
        expanded.replace(pos, key.size(), entry.second);
        pos += entry.second.size();
      }
    }
  }
#endif

  // Step 2: simple glob. Only '*' and '?' are wildcards (no brace expansion,
  // no '**' recursion), so this is bounded by the actual directory sizes and
  // cannot exhibit the wordexp() brace-expansion DoS. A path without wildcards
  // is returned as-is. When wildcards match, return the first match (this
  // mirrors the old wordexp() "use we_wordv[0]" behavior); otherwise return the
  // env-expanded path unchanged.
#if LIGHTUSD_HAVE_GLOB_FS
  if (HasGlobWildcard(expanded)) {
    std::vector<std::string> matches = SimpleGlob(expanded, /* max */ 64);
    // Route through the NRVO variable (a second named return defeats copy
    // elision and trips clang's -Werror,-Wnrvo).
    if (!matches.empty()) expanded = matches[0];
  }
#endif
  return std::string(expanded);
}
}  // namespace

std::string ExpandFilePath(const std::string &filepath, void *) {
  return ExpandPath(filepath, nullptr);
}
std::string ExpandFilePathWithVariables(
    const std::string &filepath, const std::map<std::string, std::string> &variables) {
  return ExpandPath(filepath, &variables);
}


#ifdef _WIN32
std::wstring UTF8ToWchar(const std::string &str) {
  int wstr_size =
      MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), nullptr, 0);
  std::wstring wstr(size_t(wstr_size), 0);
  MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), &wstr[0],
                      int(wstr.size()));
  return wstr;
}

std::string WcharToUTF8(const std::wstring &wstr) {
  int str_size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), int(wstr.size()),
                                     nullptr, 0, nullptr, nullptr);
  std::string str(size_t(str_size), 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), int(wstr.size()), &str[0],
                      int(str.size()), nullptr, nullptr);
  return str;
}
#endif

bool ReadWholeFile(std::vector<uint8_t> *out, std::string *err,
                   const std::string &filepath, size_t filesize_max,
                   void *userdata) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

  (void)userdata;

#ifdef LIGHTUSD_ANDROID_LOAD_FROM_ASSETS
  if (lightusd::io::asset_manager) {
    AAsset *asset = AAssetManager_open(asset_manager, filepath.c_str(),
                                       AASSET_MODE_STREAMING);
    if (!asset) {
      if (err) {
        (*err) += "File open error(from AssestManager) : " + filepath + "\n";
      }
      return false;
    }
    off_t len = AAsset_getLength(asset);
    if (len <= 0) {
      if (err) {
        (*err) += "Invalid file size : " + filepath +
                  " (does the path point to a directory?)";
      }
      return false;
    }
    size_t size = size_t(len);

    if (size >= filesize_max) {
      (*err) += "File size exceeds filesize_max : " + filepath +
                " (filesize_max " + std::to_string(filesize_max) + ")";

      return false;
    }

    out->resize(size);
    AAsset_read(asset, reinterpret_cast<char *>(&out->at(0)), size);
    AAsset_close(asset);
    return true;
  } else {
    if (err) {
      (*err) += "No asset manager specified : " + filepath + "\n";
    }
    return false;
  }

#else
#ifdef _WIN32
#if defined(__GLIBCXX__)  // mingw
  int file_descriptor =
      _wopen(UTF8ToWchar(filepath).c_str(), _O_RDONLY | _O_BINARY);
  __gnu_cxx::stdio_filebuf<char> wfile_buf(file_descriptor, std::ios_base::in);
  std::istream f(&wfile_buf);
#elif defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  // For libcxx, assume _LIBCPP_HAS_OPEN_WITH_WCHAR is defined to accept
  // `wchar_t *`
  std::ifstream f(UTF8ToWchar(filepath).c_str(), std::ifstream::binary);
#else
  // Unknown compiler/runtime
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
#else
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
  if (!f) {
    if (err) {
      (*err) += "File open error : " + filepath + "\n";
    }
    return false;
  }

  // For directory(and pipe?), peek() will fail(Posix gnustl/libc++ only)
  int buf = f.peek();
  (void)buf;
  if (!f) {
    if (err) {
      (*err) +=
          "File read error. Maybe empty file or invalid file : " + filepath +
          "\n";
    }
    return false;
  }

  f.seekg(0, f.end);
  std::streamoff pos = f.tellg();
  f.seekg(0, f.beg);

  if (pos < 0) {
    if (err) {
      (*err) += "Failed to determine file size for : " + filepath + "\n";
    }
    return false;
  }
  size_t sz = static_cast<size_t>(pos);

  if (sz == 0) {
    if (err) {
      (*err) += "File is empty : " + filepath + "\n";
    }
    return false;
  }

  if ((filesize_max > 0) && (sz > filesize_max)) {
    if (err) {
      (*err) += "File size is too large : " + filepath +
                " sz = " + std::to_string(sz) +
                ", allowed max filesize = " + std::to_string(filesize_max) +
                "\n";
    }
    return false;
  }

  out->resize(sz);
  f.read(reinterpret_cast<char *>(&out->at(0)),
         static_cast<std::streamsize>(sz));

  if (!f) {
    // read failure.
    if (err) {
      (*err) += "Failed to read file: " + filepath + "\n";
    }
    return false;
  }

  return true;
#endif
}

bool ReadFileHeader(std::vector<uint8_t> *out, std::string *err,
                    const std::string &filepath, uint32_t max_read_bytes,
                    void *userdata) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

  (void)userdata;

  // Hard limit to 1MB. Clamp to prevent overflow in downstream size_t casts.
  if (max_read_bytes > 1024 * 1024) {
    max_read_bytes = 1024 * 1024;
  }
  // Clamp to at least 1 byte so the zero-byte case is handled downstream.
  if (max_read_bytes < 1) {
    max_read_bytes = 1;
  }

#ifdef LIGHTUSD_ANDROID_LOAD_FROM_ASSETS
  if (lightusd::io::asset_manager) {
    AAsset *asset = AAssetManager_open(asset_manager, filepath.c_str(),
                                       AASSET_MODE_STREAMING);
    if (!asset) {
      if (err) {
        (*err) += "File open error(from AssestManager) : " + filepath + "\n";
      }
      return false;
    }
    off_t len = AAsset_getLength(asset);
    if (len <= 0) {
      if (err) {
        (*err) += "Invalid file size : " + filepath +
                  " (does the path point to a directory?)";
      }
      return false;
    }

    size_t size = size_t(len);

    size = (std::min)(size_t(max_read_bytes), size);
    out->resize(size);
    AAsset_read(asset, reinterpret_cast<char *>(&out->at(0)), size);
    AAsset_close(asset);
    return true;
  } else {
    if (err) {
      (*err) += "No asset manager specified : " + filepath + "\n";
    }
    return false;
  }

#else
#ifdef _WIN32
#if defined(__GLIBCXX__)  // mingw
  int file_descriptor =
      _wopen(UTF8ToWchar(filepath).c_str(), _O_RDONLY | _O_BINARY);
  __gnu_cxx::stdio_filebuf<char> wfile_buf(file_descriptor, std::ios_base::in);
  std::istream f(&wfile_buf);
#elif defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  // For libcxx, assume _LIBCPP_HAS_OPEN_WITH_WCHAR is defined to accept
  // `wchar_t *`
  std::ifstream f(UTF8ToWchar(filepath).c_str(), std::ifstream::binary);
#else
  // Unknown compiler/runtime
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
#else
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
  if (!f) {
    if (err) {
      (*err) += "File does not exit or open error : " + filepath + "\n";
    }
    return false;
  }

  // For directory(and pipe?), peek() will fail(Posix gnustl/libc++ only)
  int buf = f.peek();
  (void)buf;
  if (!f) {
    if (err) {
      (*err) +=
          "File read error. Maybe empty file or invalid file : " + filepath +
          "\n";
    }
    return false;
  }

  // Note: std::ifstream follows symlinks on POSIX. For symlink-attack
  // resistant reads use MMapFile with O_NOFOLLOW instead.
  f.seekg(0, f.end);
  size_t sz = static_cast<size_t>(f.tellg());
  f.seekg(0, f.beg);

  if (int64_t(sz) < 0) {
    if (err) {
      (*err) += "Invalid file size : " + filepath +
                " (does the path point to a directory?)";
    }
    return false;
  } else if (sz == 0) {
    if (err) {
      (*err) += "File is empty : " + filepath + "\n";
    }
    return false;
  } else if (uint64_t(sz) >= uint64_t((std::numeric_limits<int64_t>::max)())) {
    // Posixish environment.
    if (err) {
      (*err) += "Invalid File(Pipe or special device?) : " + filepath + "\n";
    }
    return false;
  }

  sz = (std::min)(size_t(max_read_bytes), sz);

  out->resize(sz);
  f.read(reinterpret_cast<char *>(&out->at(0)),
         static_cast<std::streamsize>(sz));

  if (!f) {
    // read failure.
    if (err) {
      (*err) += "Failed to read file: " + filepath + "\n";
    }
    return false;
  }

  return true;
#endif
}

bool WriteWholeFile(const std::string &filepath, const unsigned char *contents,
                    size_t content_bytes, std::string *err) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

#ifdef _WIN32
#if defined(__GLIBCXX__)  // mingw
  int file_descriptor = _wopen(UTF8ToWchar(filepath).c_str(),
                               _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY);
  __gnu_cxx::stdio_filebuf<char> wfile_buf(
      file_descriptor, std::ios_base::out | std::ios_base::binary);
  std::ostream f(&wfile_buf);
#elif defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  std::ofstream f(UTF8ToWchar(filepath).c_str(), std::ofstream::binary);
#else  // other C++ compiler for win32?
  std::ofstream f(filepath.c_str(), std::ofstream::binary);
#endif
#else
  std::ofstream f(filepath.c_str(), std::ofstream::binary);
#endif
  if (!f) {
    if (err) {
      (*err) += "File open error for writing : " + filepath + "\n";
    }
    return false;
  }

  f.write(reinterpret_cast<const char *>(contents),
          static_cast<std::streamsize>(content_bytes));
  if (!f) {
    if (err) {
      (*err) += "File write error: " + filepath + "\n";
    }
    return false;
  }

  return true;
}

#ifdef _WIN32
bool WriteWholeFile(const std::wstring &filepath, const unsigned char *contents,
                    size_t content_bytes, std::string *err) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

#if defined(__GLIBCXX__)  // mingw
  int file_descriptor =
      _wopen(filepath.c_str(), _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY);
  __gnu_cxx::stdio_filebuf<char> wfile_buf(
      file_descriptor, std::ios_base::out | std::ios_base::binary);
  std::ostream f(&wfile_buf);
#elif defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  // MSVC extension allow wstrng as an argument.
  std::ofstream f(filepath.c_str(), std::ofstream::binary);
#else  // other C++ compiler for win32?
#error "Unsupporte platform"
#endif

  if (!f) {
    if (err) {
      // This would print garbage character...
      // FIXME: First create string in wchar, then convert to wstring?
      (*err) += "File open error for writing : " + WcharToUTF8(filepath) + "\n";
    }
    return false;
  }

  f.write(reinterpret_cast<const char *>(contents),
          static_cast<std::streamsize>(content_bytes));
  if (!f) {
    if (err) {
      (*err) += "File write error: " + WcharToUTF8(filepath) + "\n";
    }
    return false;
  }

  return true;
}
#endif

std::string GetBaseDir(const std::string &filepath) {
  if (filepath.find_last_of("/\\") != std::string::npos)
    return filepath.substr(0, filepath.find_last_of("/\\"));
  return "";
}

std::string GetFileExtension(const std::string &FileName) {
  if (FileName.find_last_of(".") != std::string::npos)
    return FileName.substr(FileName.find_last_of(".") + 1);
  return "";
}

std::string GetBaseFilename(const std::string &filepath) {
  auto idx = filepath.find_last_of("/\\");
  if (idx != std::string::npos) return filepath.substr(idx + 1);
  return filepath;
}

bool IsAbsPath(const std::string &filename) {
  if (filename.size() > 0) {
    if (filename[0] == '/') {
      return true;
    }
  }

  // UNC path?
  if (filename.size() > 2) {
    if ((filename[0] == '\\') && (filename[1] == '\\')) {
      return true;
    }
  }

  // Windows drive path (e.g. C:\, D:/, ...). A drive letter followed by ':'
  // and a separator is absolute; "C:foo" (drive-relative) is not.
  if (filename.size() > 2) {
    const char c = filename[0];
    const bool is_drive_letter =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (is_drive_letter && filename[1] == ':' &&
        (filename[2] == '/' || filename[2] == '\\')) {
      return true;
    }
  }

  return false;
}

std::string JoinPath(const std::string &dir, const std::string &filename) {
  if (dir.empty()) {
    return filename;
  } else {
    // check '/'
    char lastChar = *dir.rbegin();

    // TODO: Support more relative path case.

    std::string basedir;
    if (lastChar != '/') {
      basedir = dir + std::string("/");
    } else {
      basedir = dir;
    }

    if (basedir.size()) {
      if (startsWith(filename, "./")) {
        // strip "./"
        return basedir + removePrefix(filename, "./");
      }
      return basedir + filename;
    } else {
      return filename;
    }
  }
}

std::string NormalizePath(const std::string &path) {
  if (path.empty()) {
    return path;
  }

  const bool absolute = (path[0] == '/');

  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = path.size();
  while (i < n) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) {
      j = n;
    }
    const std::string seg = path.substr(i, j - i);
    i = j + 1;

    if (seg.empty() || seg == ".") {
      // collapse empty (from `//`) and `.`
    } else if (seg == "..") {
      if (!out.empty() && out.back() != "..") {
        out.pop_back();
      } else if (!absolute) {
        // keep leading `..` for relative paths that climb above the anchor
        out.push_back("..");
      }
      // `..` at the root of an absolute path is dropped (matches realpath/pxr).
    } else {
      out.push_back(seg);
    }
  }

  std::string result = absolute ? "/" : std::string();
  for (size_t k = 0; k < out.size(); k++) {
    if (k) {
      result += "/";
    }
    result += out[k];
  }
  if (result.empty()) {
    result = absolute ? "/" : ".";
  }
  return result;
}

bool USDFileExists(const std::string &fpath) {
  if (!::lightusd::NativeIOEnabled()) return false;

  size_t read_len = 9;  // USD file must be at least 9 bytes or more.

  std::string err;
  std::vector<uint8_t> data;

  if (!ReadFileHeader(&data, &err, fpath, uint32_t(read_len))) {
    return false;
  }

  return true;
}

bool IsUDIMPath(const std::string &path) {
  return SplitUDIMPath(path, nullptr, nullptr);
}

bool SplitUDIMPath(const std::string &path, std::string *pre,
                   std::string *post) {
  std::string tag = "<UDIM>";

  auto rs = std::search(path.begin(), path.end(), tag.begin(), tag.end());
  if (rs == path.end()) {
    return false;
  }

  auto re = std::find_end(path.begin(), path.end(), tag.begin(), tag.end());
  if (re == path.end()) {
    return false;
  }

  // No multiple tags. e.g. diffuse.<UDIM>.<UDIM>.png
  if (rs != re) {
    return false;
  }

  if (pre) {
    (*pre) = std::string(path.begin(), rs);
  }

  if (post) {
    // `post` is everything AFTER the `<UDIM>` tag (the tag itself is excluded).
    // e.g. diffuse.<UDIM>.png => pre="diffuse.", post=".png"
    (*post) = std::string(re + std::ptrdiff_t(tag.size()), path.end());
  }

  return true;
}

bool FileExists(const std::string &filepath, void *userdata) {
  if (!::lightusd::NativeIOEnabled()) return false;

  (void)userdata;

  bool ret{false};
#ifdef LIGHTUSD_ANDROID_LOAD_FROM_ASSETS
  if (asset_manager) {
    AAsset *asset = AAssetManager_open(asset_manager, filepath.c_str(),
                                       AASSET_MODE_STREAMING);
    if (!asset) {
      return false;
    }
    AAsset_close(asset);
    ret = true;
  } else {
    return false;
  }
#else
#ifdef _WIN32
#if defined(_MSC_VER) || defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
  FILE *fp = nullptr;
  errno_t err = _wfopen_s(&fp, UTF8ToWchar(filepath).c_str(), L"rb");
  if (err != 0) {
    return false;
  }
#else
  FILE *fp = nullptr;
  errno_t err = fopen_s(&fp, filepath.c_str(), "rb");
  if (err != 0) {
    return false;
  }
#endif

#else
  FILE *fp = fopen(filepath.c_str(), "rb");
#endif
  if (fp) {
    ret = true;
    fclose(fp);
  } else {
    ret = false;
  }
#endif

  return ret;
}

std::string FindFile(const std::string &filename,
                     const std::vector<std::string> &search_paths) {
  // TODO: Use ghc filesystem?

  if (filename.empty()) {
    return filename;
  }

  // Reject filenames containing parent-directory references before any
  // early return (the OS would resolve the ".." and may find a file,
  // bypassing the security check). Use segment-based rather than substring
  // matching to properly detect path segments like ".." while allowing
  // filenames that merely contain ".." as a substring (e.g., "file..txt").
  {
    size_t i = 0;
    const size_t n = filename.size();
    bool has_parent_ref = false;
    while (i < n) {
      size_t j = filename.find('/', i);
      if (j == std::string::npos) {
        j = n;
      }
      std::string segment = filename.substr(i, j - i);
      if (segment == "..") {
        has_parent_ref = true;
        break;
      }
      i = (j < n) ? j + 1 : j;
    }
    if (has_parent_ref) {
      return std::string();
    }
  }

  // An ABSOLUTE path must be tried as-is, whether or not search paths are set:
  // JoinPath(dir, "/abs/path") produces "<dir>//abs/path", which never exists, so
  // the loop below can only ever miss. Before this, an absolute asset path failed
  // to resolve whenever the resolver had a search path / working directory —
  // which is exactly what an already-anchored texture path looks like.
  if (IsAbsPath(filename)) {
    std::string absPath = io::ExpandFilePath(filename, /* userdata */ nullptr);
    if (io::FileExists(absPath, /* userdata */ nullptr)) {
      return absPath;
    }
  }

  if (search_paths.empty()) {
    std::string absPath = io::ExpandFilePath(filename, /* userdata */ nullptr);
    if (io::FileExists(absPath, /* userdata */ nullptr)) {
      return absPath;
    }
  }

  for (size_t i = 0; i < search_paths.size(); i++) {
    std::string absPath = io::ExpandFilePath(
        io::JoinPath(search_paths[i], filename), /* userdata */ nullptr);
    if (io::FileExists(absPath, /* userdata */ nullptr)) {
      return absPath;
    }
  }

  return std::string();
}

std::vector<std::string> AssetPathSuffixCandidates(
    const std::string &asset_path) {
  std::vector<std::string> candidates;

  if (asset_path.empty()) {
    return candidates;
  }

  std::string p = asset_path;
  std::replace(p.begin(), p.end(), '\\', '/');

  // Strip a Windows drive prefix(e.g. "F:")
  if ((p.size() >= 2) &&
      (((p[0] >= 'A') && (p[0] <= 'Z')) || ((p[0] >= 'a') && (p[0] <= 'z'))) &&
      (p[1] == ':')) {
    p = p.substr(2);
  }

  // Strip leading '/', './' and '../' runs.
  size_t pos = 0;
  while (pos < p.size()) {
    if (p[pos] == '/') {
      pos++;
    } else if (p.compare(pos, 2, "./") == 0) {
      pos += 2;
    } else if (p.compare(pos, 3, "../") == 0) {
      pos += 3;
    } else {
      break;
    }
  }
  p = p.substr(pos);

  // Emit progressively shorter suffixes, longest first, down to the basename.
  while (p.size()) {
    if (p != asset_path) {
      candidates.push_back(p);
    }

    size_t slash_loc = p.find('/');
    if (slash_loc == std::string::npos) {
      break;
    }
    p = p.substr(slash_loc + 1);
  }

  return candidates;
}

//
// Directory / temp-path utilities.
//
// GetTempDir accepts a caller path/provider, otherwise a portable fallback
// per request; the remaining helpers wrap the vendored ghc::filesystem so that
// callers never need <filesystem>.
//
std::string GetTempDir(const std::string &directory,
    const std::shared_ptr<const HostServices> &services) {
  if (!directory.empty()) return directory;
  if (services && services->temporary_directory) {
    const std::string result = services->temporary_directory();
    if (!result.empty()) return result;
  }
#ifdef _WIN32
  // GetTempPathW writes a path terminated with a backslash. The system
  // temporary directory is the documented default; "." would make callers
  // author relative paths (and is not a temp directory).
  wchar_t buf[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  if (n == 0 || n > MAX_PATH) {
    return ".";
  }
  std::string s = WcharToUTF8(std::wstring(buf, size_t(n)));
  while (!s.empty() && (s.back() == '\\' || s.back() == '/')) {
    s.pop_back();
  }
  return s.empty() ? std::string(".") : s;
#else
  return "/tmp";
#endif
}

// The helpers below instantiate ghc::filesystem templates; suppress
// -Weverything for them the same way the header include is wrapped above (this
// TU is compiled with -Weverything -Werror).
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

std::string GetCurrentDir(const std::shared_ptr<const HostServices> &services) {
  if (services && services->current_directory) {
    const std::string result = services->current_directory();
    if (!result.empty()) return result;
  }
  return ".";
}

bool IsDirectory(const std::string &path) {
  if (!::lightusd::NativeIOEnabled()) return false;

#if defined(__wasi__)
  (void)path;
  return false;
#else
  std::error_code ec;
  return ghc::filesystem::is_directory(ghc::filesystem::path(path), ec);
#endif
}

bool CreateDirectories(const std::string &path) {
  if (!::lightusd::NativeIOEnabled()) return false;

#if defined(__wasi__)
  (void)path;
  return false;
#else
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  ghc::filesystem::path p(path);
  if (ghc::filesystem::is_directory(p, ec)) {
    return true;
  }
  ghc::filesystem::create_directories(p, ec);
  return ghc::filesystem::is_directory(p, ec);
#endif
}

bool RemoveFile(const std::string &path) {
  if (!::lightusd::NativeIOEnabled()) return false;

#if defined(__wasi__)
  (void)path;
  return false;
#else
  std::error_code ec;
  ghc::filesystem::remove(ghc::filesystem::path(path), ec);
  return !ec;
#endif
}

bool RemoveAll(const std::string &path) {
  if (!::lightusd::NativeIOEnabled()) return false;

#if defined(__wasi__)
  (void)path;
  return false;
#else
  std::error_code ec;
  ghc::filesystem::remove_all(ghc::filesystem::path(path), ec);
  return !ec;
#endif
}

std::string AbsPath(const std::string &path) {
  if (!::lightusd::NativeIOEnabled()) return path;

#if defined(__wasi__)
  return path;
#else
  std::error_code ec;
  ghc::filesystem::path p = ghc::filesystem::absolute(ghc::filesystem::path(path), ec);
  if (ec) {
    return path;
  }
  return p.lexically_normal().string();
#endif
}

std::string RelativePath(const std::string &path, const std::string &base) {
  if (!::lightusd::NativeIOEnabled()) return path;

#if defined(__wasi__)
  (void)base;
  return path;
#else
  std::error_code ec;
  ghc::filesystem::path p =
      ghc::filesystem::relative(ghc::filesystem::path(path), ghc::filesystem::path(base), ec);
  if (ec) {
    return std::string();
  }
  return p.generic_string();
#endif
}

std::vector<std::string> ListDir(const std::string &dir, bool recursive) {
  if (!::lightusd::NativeIOEnabled()) return {};

  std::vector<std::string> out;
#if !defined(__wasi__)
  std::error_code ec;
  ghc::filesystem::path base(dir);
  if (recursive) {
    ghc::filesystem::recursive_directory_iterator it(base, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      ghc::filesystem::path rel = ghc::filesystem::relative(it->path(), base, ec);
      if (!ec) {
        out.push_back(rel.generic_string());
      }
    }
  } else {
    ghc::filesystem::directory_iterator it(base, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      out.push_back(it->path().filename().string());
    }
  }
#else
  (void)dir;
  (void)recursive;
#endif
  return out;
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

}  // namespace io
}  // namespace lightusd
