// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lightusd {

// A retained library handle. Implementations own their loader lifetime.
class HostLibrary {
 public:
  virtual ~HostLibrary();
  virtual void* Symbol(const char* name) const = 0;
};

struct HostFaultCounters {
  unsigned long minor = 0;
  unsigned long major = 0;
};

enum class MemoryAdvice { HugePages, PopulateWrite };

// Optional capabilities supplied by the application, not discovered by parsers.
// Empty callbacks mean unavailable. Callbacks may be called concurrently and
// must not throw. Retain the provider for as long as its consumers are alive.
struct HostServices {
  std::function<std::string()> current_directory;
  std::function<std::string()> temporary_directory;
  std::function<uint64_t()> resident_memory_bytes;
  std::function<uint64_t()> available_memory_bytes;
  std::function<HostFaultCounters()> fault_counters;
  std::function<void()> trim_memory;
  std::function<void(void*, size_t, MemoryAdvice)> advise_memory;
  std::function<std::shared_ptr<HostLibrary>(const std::string&, std::string*)>
      open_library;
};

struct NativeHostServicesOptions {
  // False requires an absolute library filename. True explicitly authorizes
  // the OS loader's library search rules; never enable for untrusted names.
  bool allow_library_search = false;
};

// Explicit opt-in to platform services. Does not probe or load anything until
// a callback is invoked. Returns null when native host services were not built.
std::shared_ptr<const HostServices> CreateNativeHostServices(
    const NativeHostServicesOptions& options = {}, std::string* error = nullptr);

}  // namespace lightusd
