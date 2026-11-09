// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 - Present, Light Transport Entertainment Inc.
//
// Minimal persistent worker pool for legacy tydra conversion phases.
//
// Self-contained on purpose: the legacy lightusd static lib does not link
// tydra_next, so it cannot reuse next::TaskArena. Run() distributes indices
// dynamically through an atomic counter (work stealing at index granularity),
// which keeps all workers busy even when item costs are heavily skewed --
// unlike fixed-size batch waves.
//
// Determinism contract: task execution order across threads is unspecified,
// but each task writes only into caller-owned slots indexed by its argument;
// callers merge slot results in a serial, ordered pass afterwards.

#ifndef LIGHTUSD_TYDRA_TASK_ARENA_HH_
#define LIGHTUSD_TYDRA_TASK_ARENA_HH_

// Worker threads follow the repo-wide LIGHTUSD_ENABLE_THREAD opt-in (default
// OFF so WASM/browser builds need no Emscripten pthreads). Without it, Run()
// executes tasks inline on the calling thread.
#if !defined(LIGHTUSD_ENABLE_THREAD)
#define LIGHTUSD_TYDRA_TASK_ARENA_SERIAL 1
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lightusd {
namespace tydra {

// Suggested upper bound for auto-detected worker counts.
constexpr size_t kMaxTaskArenaThreads = 16;

class TaskArena {
 public:
  // Spawns max_threads-1 background workers; the calling thread also
  // participates in Run(), so max_threads == 1 never spawns a thread and
  // degrades to inline sequential execution.
  explicit TaskArena(size_t max_threads)
      : requested_(max_threads ? max_threads : 1) {
#if !defined(LIGHTUSD_TYDRA_TASK_ARENA_SERIAL)
    const unsigned hw = std::thread::hardware_concurrency();
    size_t n = requested_;
    if (n > kMaxTaskArenaThreads) n = kMaxTaskArenaThreads;
    if (n < 1) n = 1;
    if (hw && n > hw) n = hw;

    stop_ = false;
    if (n > 1) {
      workers_.reserve(n - 1);
      for (size_t i = 1; i < n; i++) {
        workers_.emplace_back([this]() { WorkerLoop(); });
      }
    }
#endif
  }

  ~TaskArena() {
#if !defined(LIGHTUSD_TYDRA_TASK_ARENA_SERIAL)
    {
      std::unique_lock<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_) {
      t.join();
    }
#endif
  }

  TaskArena(const TaskArena &) = delete;
  TaskArena &operator=(const TaskArena &) = delete;

  size_t max_threads() const { return requested_; }

  // Executes task(i) for i in [0, count) using every arena thread plus the
  // calling thread. Synchronous: returns once all tasks have completed.
  void Run(size_t count, const std::function<void(size_t)> &task) {
    if (count == 0) return;

#if defined(LIGHTUSD_TYDRA_TASK_ARENA_SERIAL)
    for (size_t i = 0; i < count; i++) {
      task(i);
    }
#else
    if (workers_.empty()) {
      for (size_t i = 0; i < count; i++) {
        task(i);
      }
      return;
    }

    {
      std::unique_lock<std::mutex> lk(mu_);
      job_ = &task;
      next_index_.store(0, std::memory_order_relaxed);
      total_.store(count, std::memory_order_relaxed);
      generation_++;
    }
    cv_.notify_all();

    // Calling thread participates.
    RunIndices();

    // Wait for workers to drain the range.
    {
      std::unique_lock<std::mutex> lk(mu_);
      done_cv_.wait(lk, [&]() {
        return completed_.load(std::memory_order_acquire) >= total_.load(std::memory_order_acquire);
      });
      completed_.store(0, std::memory_order_relaxed);
      job_ = nullptr;
    }
#endif
  }

 private:
#if defined(LIGHTUSD_TYDRA_TASK_ARENA_SERIAL)
  size_t requested_{1};
#else
  void RunIndices() {
    const std::function<void(size_t)> *fn = job_;
    if (!fn) return;
    for (;;) {
      const size_t i = next_index_.fetch_add(1, std::memory_order_relaxed);
      if (i >= total_.load(std::memory_order_acquire)) break;
      (*fn)(i);
      completed_.fetch_add(1, std::memory_order_release);
    }
  }

  void WorkerLoop() {
    uint64_t seen_generation = 0;
    for (;;) {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&]() { return stop_ || (job_ != nullptr && generation_ != seen_generation); });
      if (stop_) return;
      seen_generation = generation_;
      lk.unlock();
      RunIndices();
      {
        std::unique_lock<std::mutex> lk2(mu_);
        done_cv_.notify_one();
      }
    }
  }

  size_t requested_{1};
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable done_cv_;
  const std::function<void(size_t)> *job_{nullptr};
  std::atomic<uint64_t> next_index_{0};
  std::atomic<size_t> total_{0};
  std::atomic<size_t> completed_{0};
  uint64_t generation_{0};
  bool stop_{false};
#endif
};

}  // namespace tydra
}  // namespace lightusd

#endif  // LIGHTUSD_TYDRA_TASK_ARENA_HH_
