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

constexpr int kMaxExecutionThreads = 16;

inline int ClampExecutionThreads(int requested) {
  return requested > kMaxExecutionThreads ? kMaxExecutionThreads : requested;
}

enum class CallbackConcurrency : uint8_t {
  Serialized = 0,
  Concurrent
};

// Common execution policy for next and tydra/next operations. A value of -1
// leaves an older API's thread-count field authoritative during the migration;
// 0 selects a bounded hardware-derived count, and 1 forces serial execution.
struct ExecutionOptions {
  int max_threads = -1;
  size_t max_in_flight_bytes = 0;
  CallbackConcurrency callback_concurrency = CallbackConcurrency::Serialized;
};

// Reusable bounded worker arena. Run() is synchronous and preserves ownership
// of task state in the caller; one arena amortizes thread creation across all
// phases of a top-level operation.
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
