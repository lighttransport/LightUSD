// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - hand freed heap pages back to the OS now.
//
// Allocator purges and page advice are optional application capabilities.
// The portable default is a no-op; native hooks live in host-services.cc.

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include "core/host-services.hh"
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace lightusd {

// Optional allocator/page hints. The portable default performs no host calls.
inline void ReturnFreeMemoryToOS(
    const std::shared_ptr<const HostServices>& services = {}) {
  if (services && services->trim_memory) services->trim_memory();
}
inline void AdviseHugePages(const void* data, size_t bytes,
    const std::shared_ptr<const HostServices>& services = {}) {
  if (services && services->advise_memory)
    services->advise_memory(const_cast<void*>(data), bytes, MemoryAdvice::HugePages);
}
inline void PopulatePagesParallel(void* data, size_t bytes,
    const std::shared_ptr<const HostServices>& services = {}) {
  if (services && services->advise_memory)
    services->advise_memory(data, bytes, MemoryAdvice::PopulateWrite);
}

class MemoryTrimmer {
 public:
  explicit MemoryTrimmer(std::shared_ptr<const HostServices> services = {})
      : services_(std::move(services)) {}
  MemoryTrimmer(const MemoryTrimmer &) = delete;
  MemoryTrimmer &operator=(const MemoryTrimmer &) = delete;
  ~MemoryTrimmer() { Stop(); }

  void Request() {
    if (!services_ || !services_->trim_memory) return;
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lk(mu_);
    if (!thread_.joinable())
      thread_ = std::thread([this] { Loop(); });
    pending_ = true;
    cv_.notify_one();
#else
    ReturnFreeMemoryToOS(services_);
#endif
  }

  // Finishes a pending trim and joins the thread.
  void Stop() {
#if defined(LIGHTUSD_ENABLE_THREAD)
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
      cv_.notify_one();
    }
    if (thread_.joinable()) thread_.join();
#endif
  }

 private:
  std::shared_ptr<const HostServices> services_;
#if defined(LIGHTUSD_ENABLE_THREAD)
  void Loop() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
      cv_.wait(lk, [this] { return pending_ || stop_; });
      if (pending_) {
        pending_ = false;
        lk.unlock();
        ReturnFreeMemoryToOS(services_);
        lk.lock();
        continue;
      }
      if (stop_) return;
    }
  }
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool pending_ = false;
  bool stop_ = false;
#endif
};

}  // namespace lightusd
