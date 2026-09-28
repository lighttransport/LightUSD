// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace lightusd {
namespace next {

// Upper bound for any next/tydra worker pool (auto = hardware_concurrency,
// clamped here). Was 16, which idled half of a 32-thread workstation: with a
// scalable allocator the pcp opinion fill still gains going 16 -> 32 threads
// on Island. The bound stays because per-worker state is not free -- each pcp
// warm worker is a seeded Impl copying the layer-stack table, and the serial
// structure/merge passes cap the useful parallelism (Amdahl) -- so 64 covers
// current 32-64 thread machines without letting a 128+ thread host fan out
// hundreds of mostly-idle workers.
constexpr int kMaxExecutionThreads = 64;

#if defined(LIGHTUSD_ENABLE_THREAD)
constexpr bool kExecutionThreadsEnabled = true;
#else
constexpr bool kExecutionThreadsEnabled = false;
#endif

inline int ClampExecutionThreads(int requested) {
  return requested > kMaxExecutionThreads ? kMaxExecutionThreads : requested;
}

enum class CallbackConcurrency : uint8_t {
  Serialized = 0,
  Concurrent
};

// Common execution policy for next and tydra/next operations. 0 selects a
// bounded hardware-derived count, 1 forces serial execution, and >1 requests a
// fixed count (clamped to kMaxExecutionThreads).
struct ExecutionOptions {
  int max_threads = 0;
  size_t max_in_flight_bytes = 0;
  CallbackConcurrency callback_concurrency = CallbackConcurrency::Serialized;
};

// Reusable bounded worker arena. Run() is synchronous and preserves ownership
// of task state in the caller; one arena amortizes thread creation across all
// phases of a top-level operation. In a thread-disabled build this is a small
// serial executor: it does not instantiate std::thread/mutex/condition_variable
// and max_threads() reports the actual value 1.
class TaskArena {
 public:
  using TaskFn = void (*)(void* context, size_t index);
  explicit TaskArena(size_t max_threads);
  ~TaskArena();
  TaskArena(const TaskArena&) = delete;
  TaskArena& operator=(const TaskArena&) = delete;

  void Run(size_t count, void* context, TaskFn task);

  template <typename Fn>
  void Run(size_t count, Fn&& task) {
    using Task = typename std::decay<Fn>::type;
    Task local(std::forward<Fn>(task));
    Run(count, &local, [](void* context, size_t index) {
      (*static_cast<Task*>(context))(index);
    });
  }
  size_t max_threads() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace next
}  // namespace lightusd
