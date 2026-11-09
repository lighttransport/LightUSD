// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment Inc.
//
// Lock-free task queue for multi-threaded task execution
//
// Implemented as a bounded multi-producer / multi-consumer (MPMC) queue using
// Dmitry Vyukov's per-cell sequence-number algorithm. Each cell carries an
// atomic sequence counter that is used to hand a slot from producer to consumer:
//
//   * a producer writes the payload and only THEN publishes the cell by storing
//     `pos + 1` into the cell's sequence with release ordering;
//   * a consumer reads the cell's sequence with acquire ordering and only reads
//     the payload once it observes that published value.
//
// This release/acquire pairing on the per-cell sequence establishes a
// happens-before edge between the payload write and the payload read, so the
// payload itself is never accessed concurrently. The previous design advanced
// the shared write position *before* writing the payload, which let a consumer
// claim and read a slot before the producer had written it (a real data race
// that could silently drop a task). See doc/datarace.md, item #1.
//
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <cstdint>
#include <cstddef>

namespace lightusd {

namespace detail {
// Round `n` up to the next power of two (>= 1). Used to size the ring so the
// cell index is a cheap `pos & (cap-1)` mask and the free-running position
// counter wraps cleanly (a plain `pos % cap` would have an index discontinuity
// when `pos` wraps SIZE_MAX unless `cap` is a power of two).
inline size_t NextPow2(size_t n) {
  if (n < 2) return 1;
  n--;
  for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
    n |= n >> shift;
  }
  return n + 1;
}
}  // namespace detail

// C function pointer task type
typedef void (*TaskFuncPtr)(void* user_data);

// Task item for C function pointer version
struct TaskItem {
  TaskFuncPtr func;
  void* user_data;

  TaskItem() : func(nullptr), user_data(nullptr) {}
  TaskItem(TaskFuncPtr f, void* d) : func(f), user_data(d) {}
};

// Task item for std::function version
struct TaskItemFunc {
  std::function<void()> func;

  TaskItemFunc() : func(nullptr) {}
  explicit TaskItemFunc(std::function<void()> f) : func(std::move(f)) {}
};

///
/// Lock-free (bounded MPMC) task queue for C function pointers.
///
/// Example:
///   TaskQueue queue(1024);
///   queue.Push(my_task_func, my_data);
///   TaskItem task;
///   if (queue.Pop(task)) {
///     task.func(task.user_data);
///   }
///
class TaskQueue {
 public:
  // `capacity` is rounded up to the next power of two (>= 1) so the ring can be
  // indexed with a bitmask; a non-pow2 request just yields a slightly larger
  // ring. Capacity() returns the rounded value.
  explicit TaskQueue(size_t capacity = 1024)
      : _capacity(detail::NextPow2(capacity)),
        _mask(_capacity - 1),
        _cells(new Cell[_capacity]),
        _write_pos(0),
        _read_pos(0) {
    // Each cell starts "free for position i".
    for (size_t i = 0; i < _capacity; i++) {
      _cells[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  ~TaskQueue() = default;

  // Disable copy
  TaskQueue(const TaskQueue&) = delete;
  TaskQueue& operator=(const TaskQueue&) = delete;

  ///
  /// Push a task to the queue
  /// @param[in] func Task function pointer
  /// @param[in] user_data User data to pass to the task
  /// @return true on success, false if queue is full
  ///
  bool Push(TaskFuncPtr func, void* user_data) {
    if (!func) {
      return false;
    }

    Cell* cell;
    size_t pos = _write_pos.load(std::memory_order_relaxed);
    for (;;) {
      cell = &_cells[pos & _mask];
      size_t seq = cell->sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
      if (diff == 0) {
        // Cell is free; try to claim this position.
        if (_write_pos.compare_exchange_weak(pos, pos + 1,
                                             std::memory_order_relaxed)) {
          break;
        }
        // CAS failed: `pos` was reloaded by compare_exchange; retry.
      } else if (diff < 0) {
        // Cell still holds an unconsumed payload -> queue is full.
        return false;
      } else {
        // Another producer claimed `pos`; reload and retry.
        pos = _write_pos.load(std::memory_order_relaxed);
      }
    }

    // Write the payload, then publish the cell (release) so a consumer that
    // observes the new sequence with acquire also observes the payload.
    cell->data = TaskItem(func, user_data);
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  ///
  /// Pop a task from the queue
  /// @param[out] task Retrieved task item
  /// @return true if a task was retrieved, false if queue is empty
  ///
  bool Pop(TaskItem& task) {
    Cell* cell;
    size_t pos = _read_pos.load(std::memory_order_relaxed);
    for (;;) {
      cell = &_cells[pos & _mask];
      size_t seq = cell->sequence.load(std::memory_order_acquire);
      intptr_t diff =
          static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
      if (diff == 0) {
        // Cell holds a published payload; try to claim it.
        if (_read_pos.compare_exchange_weak(pos, pos + 1,
                                            std::memory_order_relaxed)) {
          break;
        }
        // CAS failed: `pos` was reloaded; retry.
      } else if (diff < 0) {
        // Cell not yet published (empty, or producer mid-publish).
        return false;
      } else {
        // Another consumer claimed `pos`; reload and retry.
        pos = _read_pos.load(std::memory_order_relaxed);
      }
    }

    // Acquire on the sequence above synchronizes-with the producer's release,
    // so this payload read is race-free.
    task = cell->data;
    // Recycle the cell for the producer that will reach it `_capacity` later.
    cell->sequence.store(pos + _capacity, std::memory_order_release);
    return true;
  }

  ///
  /// Get current queue size (approximate under concurrent access)
  ///
  size_t Size() const {
    size_t w = _write_pos.load(std::memory_order_acquire);
    size_t r = _read_pos.load(std::memory_order_acquire);
    return (w >= r) ? (w - r) : 0;
  }

  ///
  /// Check if queue is empty
  ///
  bool Empty() const {
    return Size() == 0;
  }

  ///
  /// Get queue capacity
  ///
  size_t Capacity() const {
    return _capacity;
  }

  ///
  /// Clear all pending tasks. Not safe to call concurrently with Push/Pop.
  ///
  void Clear() {
    TaskItem tmp;
    while (Pop(tmp)) {
      // drain
    }
  }

 private:
  struct Cell {
    TaskItem data;
    std::atomic<size_t> sequence;
    Cell() : data(), sequence(0) {}
  };

  const size_t _capacity;  // power of two
  const size_t _mask;      // _capacity - 1
  std::unique_ptr<Cell[]> _cells;
  std::atomic<size_t> _write_pos;
  std::atomic<size_t> _read_pos;
};

///
/// Lock-free (bounded MPMC) task queue for std::function tasks.
///
/// Example:
///   TaskQueueFunc queue(1024);
///   queue.Push([]() { std::cout << "Hello\n"; });
///   TaskItemFunc task;
///   if (queue.Pop(task)) {
///     task.func();
///   }
///
class TaskQueueFunc {
 public:
  // `capacity` is rounded up to the next power of two (>= 1); see TaskQueue.
  explicit TaskQueueFunc(size_t capacity = 1024)
      : _capacity(detail::NextPow2(capacity)),
        _mask(_capacity - 1),
        _cells(new Cell[_capacity]),
        _write_pos(0),
        _read_pos(0) {
    for (size_t i = 0; i < _capacity; i++) {
      _cells[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  ~TaskQueueFunc() = default;

  // Disable copy
  TaskQueueFunc(const TaskQueueFunc&) = delete;
  TaskQueueFunc& operator=(const TaskQueueFunc&) = delete;

  ///
  /// Push a task to the queue
  /// @param[in] func Task function (lambda, function object, etc.)
  /// @return true on success, false if queue is full
  ///
  bool Push(std::function<void()> func) {
    if (!func) {
      return false;
    }

    Cell* cell;
    size_t pos = _write_pos.load(std::memory_order_relaxed);
    for (;;) {
      cell = &_cells[pos & _mask];
      size_t seq = cell->sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
      if (diff == 0) {
        if (_write_pos.compare_exchange_weak(pos, pos + 1,
                                             std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = _write_pos.load(std::memory_order_relaxed);
      }
    }

    cell->data = TaskItemFunc(std::move(func));
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  ///
  /// Pop a task from the queue
  /// @param[out] task Retrieved task item
  /// @return true if a task was retrieved, false if queue is empty
  ///
  bool Pop(TaskItemFunc& task) {
    Cell* cell;
    size_t pos = _read_pos.load(std::memory_order_relaxed);
    for (;;) {
      cell = &_cells[pos & _mask];
      size_t seq = cell->sequence.load(std::memory_order_acquire);
      intptr_t diff =
          static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
      if (diff == 0) {
        if (_read_pos.compare_exchange_weak(pos, pos + 1,
                                            std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = _read_pos.load(std::memory_order_relaxed);
      }
    }

    task = std::move(cell->data);
    cell->sequence.store(pos + _capacity, std::memory_order_release);
    return true;
  }

  ///
  /// Get current queue size (approximate under concurrent access)
  ///
  size_t Size() const {
    size_t w = _write_pos.load(std::memory_order_acquire);
    size_t r = _read_pos.load(std::memory_order_acquire);
    return (w >= r) ? (w - r) : 0;
  }

  ///
  /// Check if queue is empty
  ///
  bool Empty() const {
    return Size() == 0;
  }

  ///
  /// Get queue capacity
  ///
  size_t Capacity() const {
    return _capacity;
  }

  ///
  /// Clear all pending tasks. Not safe to call concurrently with Push/Pop.
  ///
  void Clear() {
    TaskItemFunc tmp;
    while (Pop(tmp)) {
      // drain
    }
  }

 private:
  struct Cell {
    TaskItemFunc data;
    std::atomic<size_t> sequence;
    Cell() : data(), sequence(0) {}
  };

  const size_t _capacity;  // power of two
  const size_t _mask;      // _capacity - 1
  std::unique_ptr<Cell[]> _cells;
  std::atomic<size_t> _write_pos;
  std::atomic<size_t> _read_pos;
};

}  // namespace lightusd
