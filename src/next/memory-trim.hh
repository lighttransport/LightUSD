// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - hand freed heap pages back to the OS now.
//
// Allocators keep freed pages for a while (jemalloc decays dirty pages over
// ~10 s, mimalloc purges after a delay). A phase that frees gigabytes in the
// middle of a memory peak (e.g. source layers released during the opinion
// fill) only lowers the process RSS once those pages are returned, so it asks
// for that explicitly. Whichever allocator is linked answers through a weak
// symbol; with none of them this is a no-op. Never changes any output.

#pragma once

#include <cstddef>
#include <cstdint>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#if defined(__linux__) && defined(__GNUC__)
extern "C" int mallctl(const char *, void *, size_t *, void *, size_t)
    __attribute__((weak));
extern "C" void mi_collect(bool) __attribute__((weak));
#endif

namespace lightusd {
namespace next {

// Synchronous. A jemalloc purge walks every arena: hundreds of milliseconds
// after a large free, so prefer MemoryTrimmer off the critical path.
inline void ReturnFreeMemoryToOS() {
#if defined(__linux__) && defined(__GNUC__)
  if (mallctl) {
    // "arena.<MALLCTL_ARENAS_ALL>.purge"
    (void)mallctl("arena.4096.purge", nullptr, nullptr, nullptr, 0);
  } else if (mi_collect) {
    mi_collect(true);
  }
#endif
}

// Ask for transparent huge pages on the whole 2 MiB pages inside
// [data, data + bytes) (Linux; a no-op elsewhere or when THP is off). For a
// large, long-lived, densely filled allocation.
inline void AdviseHugePages(const void *data, size_t bytes) {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  constexpr uintptr_t kHuge = uintptr_t(2) << 20;
  const uintptr_t begin = (reinterpret_cast<uintptr_t>(data) + kHuge - 1) & ~(kHuge - 1);
  const uintptr_t end = (reinterpret_cast<uintptr_t>(data) + bytes) & ~(kHuge - 1);
  if (data && end > begin)
    (void)::madvise(reinterpret_cast<void *>(begin), end - begin, MADV_HUGEPAGE);
#else
  (void)data;
  (void)bytes;
#endif
}

// Fault in the whole pages inside [data, data + bytes) from several threads
// (Linux MADV_POPULATE_WRITE; a no-op elsewhere, without threads, or on a
// kernel that lacks it). For a large fresh allocation that one thread is about
// to construct or fill: the fill then runs at memory bandwidth instead of
// taking every first-touch fault serially. No byte is changed.
inline void PopulatePagesParallel(void *data, size_t bytes) {
#if defined(LIGHTUSD_ENABLE_THREAD) && defined(__linux__) && defined(MADV_POPULATE_WRITE)
  constexpr uintptr_t kPage = 4096;
  const uintptr_t lo = (reinterpret_cast<uintptr_t>(data) + kPage - 1) & ~(kPage - 1);
  const uintptr_t hi = (reinterpret_cast<uintptr_t>(data) + bytes) & ~(kPage - 1);
  if (!data || hi <= lo) return;
  const unsigned hw = std::thread::hardware_concurrency();
  const uintptr_t pages = (hi - lo) / kPage;
  const uintptr_t T = pages < 64 ? 1 : (hw == 0 ? 1 : hw < 16 ? hw : 16);
  std::thread pool[16];
  for (uintptr_t t = 0; t < T; ++t) {
    const uintptr_t a = lo + pages * t / T * kPage;
    const uintptr_t b = t + 1 == T ? hi : lo + pages * (t + 1) / T * kPage;
    if (b > a)
      pool[t] = std::thread([a, b] {
        (void)::madvise(reinterpret_cast<void *>(a), b - a, MADV_POPULATE_WRITE);
      });
  }
  for (std::thread &th : pool)
    if (th.joinable()) th.join();
#else
  (void)data;
  (void)bytes;
#endif
}

// Runs ReturnFreeMemoryToOS on its own thread when asked; requests made while
// one runs collapse into one more. Synchronous without thread support.
class MemoryTrimmer {
 public:
  MemoryTrimmer() = default;
  MemoryTrimmer(const MemoryTrimmer &) = delete;
  MemoryTrimmer &operator=(const MemoryTrimmer &) = delete;
  ~MemoryTrimmer() { Stop(); }

  void Request() {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::lock_guard<std::mutex> lk(mu_);
    if (!thread_.joinable())
      thread_ = std::thread([this] { Loop(); });
    pending_ = true;
    cv_.notify_one();
#else
    ReturnFreeMemoryToOS();
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
#if defined(LIGHTUSD_ENABLE_THREAD)
  void Loop() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
      cv_.wait(lk, [this] { return pending_ || stop_; });
      if (pending_) {
        pending_ = false;
        lk.unlock();
        ReturnFreeMemoryToOS();
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

}  // namespace next
}  // namespace lightusd
