// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Process-wide memory budget with a tracking pool allocator.
//
// The point of this header is graceful failure: an app sets a cap, routes its
// bulk containers through PoolAlloc, and guards each expensive phase with
// WouldExceed(). Allocations past the cap throw std::bad_alloc mid-flight,
// which the caller catches and turns into a clean abort or a degraded result —
// instead of being OOM-killed by the kernel with no diagnostics.
//
// Originally written for tools/lusdrender (-maxMem); lifted here so
// examples/lusdquicklook can share it. Header-only, depends only on std plus
// resource-budget.hh.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "tydra/next/chunked-array.hh"
#include "tydra/next/resource-budget.hh"

namespace lightusd {
namespace tydra {
namespace next {

class MemBudget {
 public:
  static constexpr size_t kDefaultCapBytes = size_t(32) << 30;  // 32 GiB

  static MemBudget &Get() {
    static MemBudget inst;
    return inst;
  }

  // cap_override_gib <= 0 -> the shared 32 GiB-target host policy.
  void Init(double cap_override_gib) {
    if (cap_override_gib > 0.0) {
      // Plain `double` here, not `long double`: MSVC's `long double` is
      // bit-identical to `double` (no 80-bit extended precision like
      // GCC/Clang/x86 get), and double's ~15-17 significant decimal digits
      // are already far more than enough to place `requested` correctly on
      // either side of `max_size` (~1.8e19, i.e. SIZE_MAX on 64-bit) -- no
      // realistic --max-mem/--max-vram GiB value comes close to that
      // boundary, so no compiler-specific extra precision is needed here.
      const double requested =
          cap_override_gib * static_cast<double>(size_t(1) << 30);
      const double max_size =
          static_cast<double>((std::numeric_limits<size_t>::max)());
      cap_ = requested >= max_size
                 ? (std::numeric_limits<size_t>::max)()
                 : static_cast<size_t>(requested);
    } else {
      size_t avail = AvailableSystemMemory();
      const uint64_t target_capacity =
          avail ? std::min<uint64_t>(avail, lightusd::tydra::next::GiB(32))
                : lightusd::tydra::next::GiB(32);
      cap_ = static_cast<size_t>(
          lightusd::tydra::next::ComputeResourceBudget(target_capacity, 0)
              .host_limit);
    }
    base_.store(0);
    tracked_.store(0);
    peak_tracked_.store(0);
  }

  // Set an explicit byte cap, clamped to what the machine can actually back.
  // Convenience for callers whose flag is in MB rather than GiB (a previewer
  // wants a 512 MB budget, not 0.5 GiB of floating point).
  void InitBytes(uint64_t cap_bytes) {
    uint64_t avail = AvailableSystemMemory();
    if (avail) {
      const uint64_t host_limit =
          lightusd::tydra::next::ComputeResourceBudget(avail, 0).host_limit;
      if (host_limit) cap_bytes = std::min<uint64_t>(cap_bytes, host_limit);
    }
#if SIZE_MAX < UINT64_MAX
    cap_ = cap_bytes > static_cast<uint64_t>(SIZE_MAX)
               ? SIZE_MAX
               : static_cast<size_t>(cap_bytes);
#else
    cap_ = static_cast<size_t>(cap_bytes);
#endif
    base_.store(0);
    tracked_.store(0);
    peak_tracked_.store(0);
  }

  size_t Cap() const { return cap_; }
  size_t Tracked() const { return tracked_.load(std::memory_order_relaxed); }
  size_t PeakTracked() const {
    return peak_tracked_.load(std::memory_order_relaxed);
  }

  // Snapshot the non-tracked RSS (everything except our render buffers), so the
  // pool allocator can bound OUR allocations to cap_ - base_ with pure atomics
  // (no /proc read per allocation). Call at the start of a streaming phase.
  void SnapshotBase() {
    size_t rss = ProcessRSS();
    size_t tr = tracked_.load(std::memory_order_relaxed);
    base_.store(rss > tr ? rss - tr : 0, std::memory_order_relaxed);
  }

  // Atomically reserve `bytes` of tracked allocation; false if it would exceed
  // the cap (cap_ - base_). Used by PoolAlloc::allocate.
  bool TryAdd(size_t bytes) {
    size_t base = base_.load(std::memory_order_relaxed);
    size_t limit = cap_ > base ? cap_ - base : 0;
    size_t prev = tracked_.load(std::memory_order_relaxed);
    for (;;) {
      if (bytes > (std::numeric_limits<size_t>::max)() - prev) return false;
      const size_t next = prev + bytes;
      if (cap_ && next > limit) return false;
      if (tracked_.compare_exchange_weak(prev, next,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
        BumpPeak(next);
        return true;
      }
    }
  }
  void Sub(size_t bytes) {
    tracked_.fetch_sub(bytes, std::memory_order_relaxed);
  }

  // Phase guard: would the process RSS plus `extra_estimate` untracked bytes bust
  // the cap right now? `phase`/extra are for the diagnostic message.
  bool WouldExceed(size_t extra_estimate, std::string *why = nullptr) const {
    if (!cap_) return false;
    size_t rss = ProcessRSS();
    if (extra_estimate <= cap_ - std::min(rss, cap_)) return false;
    if (why) {
      *why = "memory cap " + GiB(cap_) + " would be exceeded (current RSS " +
             GiB(rss) + " + estimated " + GiB(extra_estimate) + ")";
    }
    return true;
  }

  // Process RSS in bytes; 0 if unavailable.
  //   wasm:    grown linear-heap size (there is no /proc, and the heap IS the
  //            memory bound that matters on wasm32).
  //   Windows: working set via GetProcessMemoryInfo.
  //   Linux:   /proc/self/statm (pages).
  // NOTE: the /proc branch is guarded on __linux__ rather than used as the
  // fallback for every non-Windows target -- it referenced _SC_PAGESIZE
  // without <unistd.h> being included off-Linux, which broke the emscripten
  // build as soon as a wasm translation unit included this header.
  static size_t ProcessRSS() {
#if defined(__EMSCRIPTEN__)
    return static_cast<size_t>(emscripten_get_heap_size());
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
      return size_t(pmc.WorkingSetSize);
    }
    return 0;
#elif defined(__linux__)
    std::ifstream f("/proc/self/statm");
    if (!f) return 0;
    size_t total_pages = 0, rss_pages = 0;
    f >> total_pages >> rss_pages;
    if (!f) return 0;
    long pg = sysconf(_SC_PAGESIZE);
    return rss_pages * size_t(pg > 0 ? pg : 4096);
#else
    return 0;  // unknown platform: budget guards become inert, never wrong
#endif
  }

  // Available system memory in bytes; 0 if unavailable. Linux: /proc/meminfo
  // MemAvailable. Windows: GlobalMemoryStatusEx ullAvailPhys.
  static size_t AvailableSystemMemory() {
#if defined(__EMSCRIPTEN__)
    return 0;  // no host meminfo; callers fall back to their configured cap
#elif defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) {
      return size_t(st.ullAvailPhys);
    }
    return 0;
#else
    std::ifstream f("/proc/meminfo");
    if (!f) return 0;
    std::string key;
    while (f >> key) {
      if (key == "MemAvailable:") {
        size_t kib = 0;
        f >> kib;
        return kib * size_t(1024);
      }
      std::string rest;
      std::getline(f, rest);
    }
    return 0;
#endif
  }

  static std::string GiB(size_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f GiB",
                  double(bytes) / double(size_t(1) << 30));
    return std::string(buf);
  }

 private:
  MemBudget() = default;
  void BumpPeak(size_t v) {
    size_t p = peak_tracked_.load(std::memory_order_relaxed);
    while (v > p &&
           !peak_tracked_.compare_exchange_weak(p, v, std::memory_order_relaxed)) {
    }
  }
  size_t cap_{kDefaultCapBytes};
  std::atomic<size_t> base_{0};
  std::atomic<size_t> tracked_{0};
  std::atomic<size_t> peak_tracked_{0};
};

// Thread-safe size-bucketed free-list pool. Cuts malloc/free traffic for the
// render buffers (which are freed + reallocated across animation frames / re-
// renders) and recycles whole bucket-sized blocks so reuse is always safe.
// Oversize requests bypass the pool. A retained-free-bytes cap prevents hoarding.
class MemPool {
 public:
  static MemPool &Get() {
    static MemPool p;
    return p;
  }
  void *Alloc(size_t bytes) {
    int b = Bucket(bytes);
    if (b < 0) return std::malloc(bytes);  // oversize: exact size
    {
      std::lock_guard<std::mutex> lk(mu_[b]);
      if (!free_[b].empty()) {
        void *p = free_[b].back();
        free_[b].pop_back();
        pooled_.fetch_sub(BucketBytes(b), std::memory_order_relaxed);
        return p;
      }
    }
    return std::malloc(BucketBytes(b));  // full bucket size -> safe to recycle
  }
  void Free(void *p, size_t bytes) {
    if (!p) return;
    int b = Bucket(bytes);
    if (b >= 0) {
      std::lock_guard<std::mutex> lk(mu_[b]);
      const size_t bucket_bytes = BucketBytes(b);
      const size_t pooled = pooled_.load(std::memory_order_relaxed);
      if (pooled <= kMaxPooled &&
          bucket_bytes <= kMaxPooled - pooled) {
        free_[b].push_back(p);
        pooled_.fetch_add(bucket_bytes, std::memory_order_relaxed);
        return;
      }
    }
    std::free(p);
  }

 private:
  static constexpr int kMinShift = 6;   // 64 B
  static constexpr int kMaxShift = 20;  // 1 MiB
  static constexpr int kNumBuckets = kMaxShift - kMinShift + 1;
  static constexpr size_t kMaxPooled = size_t(256) << 20;  // retain <=256 MiB
  static int Bucket(size_t bytes) {
    if (bytes == 0) bytes = 1;
    if (bytes > (size_t(1) << kMaxShift)) return -1;
    int s = kMinShift;
    while ((size_t(1) << s) < bytes) ++s;
    if (s > kMaxShift) return -1;
    return s - kMinShift;
  }
  static size_t BucketBytes(int b) { return size_t(1) << (b + kMinShift); }
  std::mutex mu_[kNumBuckets];
  std::vector<void *> free_[kNumBuckets];
  std::atomic<size_t> pooled_{0};
};

// PoolAlloc reports exhaustion by throwing std::bad_alloc, so it only exists in
// exception-enabled builds. MemBudget/MemPool above are throw-free, which lets
// -fno-exceptions translation units (the wasm converter) include this header
// for the WouldExceed() phase guards and degrade gracefully instead.
#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)

// Allocator that routes std::vector storage through MemPool and accounts every
// byte into MemBudget — so the triangle buffers are tracked precisely and a
// stream that would bust the cap throws std::bad_alloc mid-flight (caught by the
// stream workers -> clean abort) instead of OOM-killing the process.
template <class T>
struct PoolAlloc {
  using value_type = T;
  PoolAlloc() noexcept = default;
  template <class U>
  PoolAlloc(const PoolAlloc<U> &) noexcept {}
  T *allocate(std::size_t n) {
    if (n > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) {
      throw std::bad_alloc();
    }
    std::size_t bytes = n * sizeof(T);
    if (!MemBudget::Get().TryAdd(bytes)) throw std::bad_alloc();
    void *p = MemPool::Get().Alloc(bytes);
    if (!p) {
      MemBudget::Get().Sub(bytes);
      throw std::bad_alloc();
    }
    return static_cast<T *>(p);
  }
  void deallocate(T *p, std::size_t n) noexcept {
    if (n > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) return;
    std::size_t bytes = n * sizeof(T);
    MemPool::Get().Free(p, bytes);
    MemBudget::Get().Sub(bytes);
  }
  template <class U>
  bool operator==(const PoolAlloc<U> &) const noexcept {
    return true;
  }
  template <class U>
  bool operator!=(const PoolAlloc<U> &) const noexcept {
    return false;
  }
};

#endif  // exceptions enabled

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
