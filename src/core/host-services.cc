// SPDX-License-Identifier: Apache-2.0
#include "host-services.hh"

namespace lightusd { HostLibrary::~HostLibrary() = default; }

#if defined(LIGHTUSD_NATIVE_HOST_SERVICES) && LIGHTUSD_NATIVE_HOST_SERVICES && \
    !defined(__EMSCRIPTEN__) && !defined(__wasi__) && !defined(LIGHTUSD_NO_NATIVE_IO) && \
    (defined(_WIN32) || defined(__unix__) || defined(__APPLE__))
#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#endif
#if defined(__linux__) && defined(__GNUC__)
extern "C" int mallctl(const char*, void*, size_t*, void*, size_t)
    __attribute__((weak));
extern "C" void mi_collect(bool) __attribute__((weak));
#endif

namespace lightusd {
namespace {
bool AbsoluteLibraryPath(const std::string& path) {
#ifdef _WIN32
  return path.size() >= 3 &&
      ((path[0] == '\\' && path[1] == '\\') ||
       (path[1] == ':' && (path[2] == '/' || path[2] == '\\')));
#else
  return !path.empty() && path[0] == '/';
#endif
}
#ifdef _WIN32
std::string Utf8Path(const wchar_t* value, DWORD count) {
  if (!count || count > DWORD((std::numeric_limits<int>::max)())) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
      int(count), nullptr, 0, nullptr, nullptr);
  if (!bytes) return {};
  std::string result(size_t(bytes), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, int(count),
      &result[0], bytes, nullptr, nullptr)) return {};
  return result;
}
#endif
class NativeLibrary final : public HostLibrary {
 public:
#ifdef _WIN32
  explicit NativeLibrary(HMODULE handle) : handle_(handle) {}
  ~NativeLibrary() override { FreeLibrary(handle_); }
  void* Symbol(const char* name) const override {
    return name ? reinterpret_cast<void*>(GetProcAddress(handle_, name)) : nullptr;
  }
 private:
  HMODULE handle_;
#else
  explicit NativeLibrary(void* handle) : handle_(handle) {}
  ~NativeLibrary() override { dlclose(handle_); }
  void* Symbol(const char* name) const override {
    return name ? dlsym(handle_, name) : nullptr;
  }
 private:
  void* handle_;
#endif
};
uint64_t ResidentMemory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return static_cast<uint64_t>(counters.WorkingSetSize);
#elif defined(__linux__)
  std::ifstream input("/proc/self/statm");
  uint64_t total = 0, resident = 0;
  const long page = sysconf(_SC_PAGESIZE);
  if (input >> total >> resident && page > 0 &&
      resident <= (std::numeric_limits<uint64_t>::max)() / uint64_t(page))
    return resident * uint64_t(page);
#endif
  return 0;
}
uint64_t AvailableMemory() {
#ifdef _WIN32
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) return memory.ullAvailPhys;
#elif defined(__linux__)
  std::ifstream input("/proc/meminfo");
  std::string key, rest;
  while (input >> key) {
    if (key == "MemAvailable:") {
      uint64_t kib = 0;
      if (input >> kib && kib <= (std::numeric_limits<uint64_t>::max)() / 1024)
        return kib * 1024;
      return 0;
    }
    std::getline(input, rest);
  }
#endif
  return 0;
}
}  // namespace
std::shared_ptr<const HostServices> CreateNativeHostServices(
    const NativeHostServicesOptions& options, std::string* error) {
  if (error) error->clear();
  auto services = std::make_shared<HostServices>();
  services->current_directory = [] {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetCurrentDirectoryW(DWORD(buffer.size()), buffer.data());
    return size && size < buffer.size() ? Utf8Path(buffer.data(), size) : std::string();
#else
    std::vector<char> buffer(4096);
    return getcwd(buffer.data(), buffer.size()) ? std::string(buffer.data()) : std::string();
#endif
  };
  services->temporary_directory = [] {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetTempPathW(DWORD(buffer.size()), buffer.data());
    return size && size < buffer.size() ? Utf8Path(buffer.data(), size) : std::string();
#else
    return std::string("/tmp");
#endif
  };
  services->resident_memory_bytes = ResidentMemory;
  services->available_memory_bytes = AvailableMemory;
  services->fault_counters = [] {
    HostFaultCounters result;
#ifndef _WIN32
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
      result.minor = static_cast<unsigned long>(usage.ru_minflt);
      result.major = static_cast<unsigned long>(usage.ru_majflt);
    }
#endif
    return result;
  };
  services->trim_memory = [] {
#if defined(__linux__) && defined(__GNUC__)
    if (mallctl) (void)mallctl("arena.4096.purge", nullptr, nullptr, nullptr, 0);
    else if (mi_collect) mi_collect(true);
#endif
  };
  services->advise_memory = [](void* data, size_t bytes, MemoryAdvice advice) {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    const long page = sysconf(_SC_PAGESIZE);
    const uintptr_t alignment = advice == MemoryAdvice::HugePages
        ? uintptr_t(2) << 20 : uintptr_t(page > 0 ? page : 4096);
    const uintptr_t address = reinterpret_cast<uintptr_t>(data);
    if (!data || bytes > (std::numeric_limits<uintptr_t>::max)() - address ||
        address > (std::numeric_limits<uintptr_t>::max)() - (alignment - 1)) return;
    const uintptr_t begin = (address + alignment - 1) / alignment * alignment;
    const uintptr_t end = (address + bytes) / alignment * alignment;
    if (end <= begin) return;
    if (advice == MemoryAdvice::HugePages)
      (void)madvise(reinterpret_cast<void*>(begin), end - begin, MADV_HUGEPAGE);
#ifdef MADV_POPULATE_WRITE
    else (void)madvise(reinterpret_cast<void*>(begin), end - begin, MADV_POPULATE_WRITE);
#endif
#else
    (void)data; (void)bytes; (void)advice;
#endif
  };
  services->open_library = [options](const std::string& path, std::string* why)
      -> std::shared_ptr<HostLibrary> {
    if (why) why->clear();
    if (path.empty() || path.find('\0') != std::string::npos ||
        (!options.allow_library_search && !AbsoluteLibraryPath(path))) {
      if (why) *why = "native library loading requires an absolute filename";
      return {};
    }
#ifdef _WIN32
    if (path.size() > size_t((std::numeric_limits<int>::max)())) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path.data(), int(path.size()), nullptr, 0);
    if (!count) return {};
    std::wstring wide(size_t(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                           int(path.size()), &wide[0], count)) return {};
    HMODULE handle = options.allow_library_search
        ? LoadLibraryW(wide.c_str())
        : LoadLibraryExW(wide.c_str(), nullptr,
              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle) {
      if (why) *why = "native library could not be loaded";
      return {};
    }
    return std::make_shared<NativeLibrary>(handle);
  };
  return services;
}
}  // namespace lightusd
#else
namespace lightusd {
std::shared_ptr<const HostServices> CreateNativeHostServices(
    const NativeHostServicesOptions&, std::string* error) {
  if (error) *error = "native host services were not built";
  return {};
}
}  // namespace lightusd
#endif
