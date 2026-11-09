// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Light Transport Entertainment Inc.
//
// Minimal fixed-layout spinlock + scoped guard.
//
// Used where a lock is needed on rarely-contended paths (diagnostics,
// cold caches) and where std::mutex is unsuitable: the class layout must not
// depend on build-private defines like LIGHTUSD_ENABLE_THREAD, and the header
// must compile in single-threaded WASM/WASI builds (std::atomic_flag does;
// <mutex> may not).

#pragma once

#include <atomic>

// ThreadSanitizer schedules tight atomic spin loops pathologically (a
// contended busy-wait can livelock for minutes), so yield to the OS
// scheduler under TSan. Plain builds keep the pure busy-wait: critical
// sections here are a few instructions and contention is rare.
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#include <sched.h>
#define LIGHTUSD_SPIN_YIELD() sched_yield()
#else
#define LIGHTUSD_SPIN_YIELD() ((void)0)
#endif

namespace lightusd {

struct SpinMutex {
  void lock() {
    while (_flag.test_and_set(std::memory_order_acquire)) {
      LIGHTUSD_SPIN_YIELD();
    }
  }
  void unlock() { _flag.clear(std::memory_order_release); }

 private:
  std::atomic_flag _flag = ATOMIC_FLAG_INIT;
};

template <typename M>
struct ScopedSpinLock {
  explicit ScopedSpinLock(M &m) : _m(m) { _m.lock(); }
  ~ScopedSpinLock() { _m.unlock(); }
  ScopedSpinLock(const ScopedSpinLock &) = delete;
  ScopedSpinLock &operator=(const ScopedSpinLock &) = delete;

 private:
  M &_m;
};

}  // namespace lightusd
