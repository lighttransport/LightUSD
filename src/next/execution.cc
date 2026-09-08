// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "execution.hh"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace lightusd {
namespace next {

struct TaskArena::Impl {
  explicit Impl(size_t requested)
      : max_threads(std::min<size_t>(
            std::max<size_t>(1, requested),
            static_cast<size_t>(kMaxExecutionThreads))) {
#if defined(LIGHTUSD_ENABLE_THREAD)
    workers.reserve(max_threads - 1);
    for (size_t i = 1; i < max_threads; ++i) {
      workers.emplace_back([this]() { WorkerLoop(); });
    }
#endif
  }

  ~Impl() {
#if defined(LIGHTUSD_ENABLE_THREAD)
    {
      std::lock_guard<std::mutex> lock(mu);
      stopping = true;
      ++generation;
    }
    work_cv.notify_all();
    for (std::thread& worker : workers) worker.join();
#endif
  }

  void WorkerLoop() {
    size_t observed_generation = 0;
    for (;;) {
      std::unique_lock<std::mutex> lock(mu);
      work_cv.wait(lock, [&]() {
        return stopping || generation != observed_generation;
      });
      if (stopping) return;
      observed_generation = generation;
      lock.unlock();
      RunItems();
      lock.lock();
      if (--active_workers == 0) done_cv.notify_one();
    }
  }

  void RunItems() {
    for (;;) {
      const size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= count) return;
      task(context, index);
    }
  }

  void Run(size_t item_count, void* task_context, TaskFn fn) {
    if (item_count == 0) return;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (!workers.empty() && item_count > 1) {
      std::unique_lock<std::mutex> run_lock(run_mu);
      {
        std::lock_guard<std::mutex> lock(mu);
        count = item_count;
        context = task_context;
        task = fn;
        next.store(0, std::memory_order_relaxed);
        active_workers = workers.size();
        ++generation;
      }
      work_cv.notify_all();
      RunItems();
      std::unique_lock<std::mutex> lock(mu);
      done_cv.wait(lock, [&]() { return active_workers == 0; });
      task = nullptr;
      context = nullptr;
      return;
    }
#endif
    for (size_t i = 0; i < item_count; ++i) fn(task_context, i);
  }

  size_t max_threads = 1;
  std::mutex run_mu;
  std::mutex mu;
  std::condition_variable work_cv;
  std::condition_variable done_cv;
  bool stopping = false;
  size_t generation = 0;
  size_t count = 0;
  size_t active_workers = 0;
  std::atomic<size_t> next{0};
  void* context = nullptr;
  TaskFn task = nullptr;
  std::vector<std::thread> workers;
};

TaskArena::TaskArena(size_t max_threads) : impl_(new Impl(max_threads)) {}
TaskArena::~TaskArena() = default;

void TaskArena::Run(size_t count, void* context, TaskFn task) {
  impl_->Run(count, context, task);
}

size_t TaskArena::max_threads() const { return impl_->max_threads; }

}  // namespace next
}  // namespace lightusd
