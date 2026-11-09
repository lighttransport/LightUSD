// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment Inc.
//
// Parallel pretty-printing for Prim and PrimSpec
//
#include "prim-pprint-parallel.hh"
#include "prim-pprint.hh"
#include "stream-writer.hh"
#include <sstream>

namespace lightusd {
namespace prim {

#if defined(LIGHTUSD_ENABLE_THREAD)

// Worker function for printing Prims
static void print_prim_worker(void* user_data) {
  PrintPrimTask* task = static_cast<PrintPrimTask*>(user_data);
  if (task && task->prim && task->output) {
    *(task->output) = print_prim(*(task->prim), task->indent);
  }
}

// Worker function for printing PrimSpecs
static void print_primspec_worker(void* user_data) {
  PrintPrimSpecTask* task = static_cast<PrintPrimSpecTask*>(user_data);
  if (task && task->primspec && task->output) {
    *(task->output) = print_primspec(*(task->primspec), task->indent);
  }
}

std::string print_prims_parallel(
    const std::vector<const Prim*>& prims,
    uint32_t indent,
    const ParallelPrintConfig& config) {

  // Check if parallel printing is worth it
  if (!config.enabled || prims.size() < config.min_prims_for_parallel) {
    // Fall back to sequential printing
    std::stringstream ss;
    for (size_t i = 0; i < prims.size(); i++) {
      if (prims[i]) {
        ss << print_prim(*prims[i], indent);
        if (i != (prims.size() - 1)) {
          ss << "\n";
        }
      }
    }
    return ss.str();
  }

  // Prepare output buffers
  std::vector<std::string> outputs(prims.size());
  std::vector<PrintPrimTask> tasks(prims.size());

  // Initialize tasks
  for (size_t i = 0; i < prims.size(); i++) {
    tasks[i] = PrintPrimTask(prims[i], indent, i, &outputs[i]);
  }

  // Create task queue
  TaskQueue queue(config.task_queue_capacity);
  std::atomic<size_t> completed_tasks(0);
  std::atomic<bool> producer_done(false);

  // Launch worker threads
  std::vector<std::thread> workers;
  workers.reserve(config.num_threads);

  for (size_t t = 0; t < config.num_threads; t++) {
    workers.emplace_back([&queue, &completed_tasks, &producer_done]() {
      TaskItem task;
      while (!producer_done.load(std::memory_order_acquire) || !queue.Empty()) {
        if (queue.Pop(task)) {
          if (task.func) {
            task.func(task.user_data);
            completed_tasks.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // Producer: push all tasks
  for (size_t i = 0; i < tasks.size(); i++) {
    while (!queue.Push(print_prim_worker, &tasks[i])) {
      std::this_thread::yield();
    }
  }

  producer_done.store(true, std::memory_order_release);

  // Wait for all workers to finish
  for (auto& worker : workers) {
    worker.join();
  }

  // Concatenate results in original order
  std::stringstream ss;
  for (size_t i = 0; i < outputs.size(); i++) {
    ss << outputs[i];
    if (i != (outputs.size() - 1)) {
      ss << "\n";
    }
  }

  return ss.str();
}

std::string print_primspecs_parallel(
    const std::vector<const PrimSpec*>& primspecs,
    uint32_t indent,
    const ParallelPrintConfig& config) {

  // Check if parallel printing is worth it
  if (!config.enabled || primspecs.size() < config.min_prims_for_parallel) {
    // Fall back to sequential printing
    std::stringstream ss;
    for (size_t i = 0; i < primspecs.size(); i++) {
      if (primspecs[i]) {
        ss << print_primspec(*primspecs[i], indent);
        if (i != (primspecs.size() - 1)) {
          ss << "\n";
        }
      }
    }
    return ss.str();
  }

  // Prepare output buffers
  std::vector<std::string> outputs(primspecs.size());
  std::vector<PrintPrimSpecTask> tasks(primspecs.size());

  // Initialize tasks
  for (size_t i = 0; i < primspecs.size(); i++) {
    tasks[i] = PrintPrimSpecTask(primspecs[i], indent, i, &outputs[i]);
  }

  // Create task queue
  TaskQueue queue(config.task_queue_capacity);
  std::atomic<size_t> completed_tasks(0);
  std::atomic<bool> producer_done(false);

  // Launch worker threads
  std::vector<std::thread> workers;
  workers.reserve(config.num_threads);

  for (size_t t = 0; t < config.num_threads; t++) {
    workers.emplace_back([&queue, &completed_tasks, &producer_done]() {
      TaskItem task;
      while (!producer_done.load(std::memory_order_acquire) || !queue.Empty()) {
        if (queue.Pop(task)) {
          if (task.func) {
            task.func(task.user_data);
            completed_tasks.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // Producer: push all tasks
  for (size_t i = 0; i < tasks.size(); i++) {
    while (!queue.Push(print_primspec_worker, &tasks[i])) {
      std::this_thread::yield();
    }
  }

  producer_done.store(true, std::memory_order_release);

  // Wait for all workers to finish
  for (auto& worker : workers) {
    worker.join();
  }

  // Concatenate results in original order
  std::stringstream ss;
  for (size_t i = 0; i < outputs.size(); i++) {
    ss << outputs[i];
    if (i != (outputs.size() - 1)) {
      ss << "\n";
    }
  }

  return ss.str();
}

// ============================================================================
// ChunkedStreamWriter Template Implementations (Zero-Copy Parallel Printing)
// ============================================================================

///
/// Task data for printing a Prim to ChunkedStreamWriter
///
template <size_t ChunkSize, size_t Alignment>
struct PrintPrimChunkedTask {
  const Prim* prim;
  uint32_t indent;
  size_t index;  // Original index for ordering
  ChunkedStreamWriter<ChunkSize, Alignment>* output;  // Output writer

  PrintPrimChunkedTask() : prim(nullptr), indent(0), index(0), output(nullptr) {}
  PrintPrimChunkedTask(const Prim* p, uint32_t i, size_t idx, ChunkedStreamWriter<ChunkSize, Alignment>* out)
    : prim(p), indent(i), index(idx), output(out) {}
};

///
/// Task data for printing a PrimSpec to ChunkedStreamWriter
///
template <size_t ChunkSize, size_t Alignment>
struct PrintPrimSpecChunkedTask {
  const PrimSpec* primspec;
  uint32_t indent;
  size_t index;  // Original index for ordering
  ChunkedStreamWriter<ChunkSize, Alignment>* output;  // Output writer

  PrintPrimSpecChunkedTask() : primspec(nullptr), indent(0), index(0), output(nullptr) {}
  PrintPrimSpecChunkedTask(const PrimSpec* ps, uint32_t i, size_t idx, ChunkedStreamWriter<ChunkSize, Alignment>* out)
    : primspec(ps), indent(i), index(idx), output(out) {}
};

// Worker function for printing Prims to ChunkedStreamWriter
template <size_t ChunkSize, size_t Alignment>
static void print_prim_chunked_worker(void* user_data) {
  auto* task = static_cast<PrintPrimChunkedTask<ChunkSize, Alignment>*>(user_data);
  if (task && task->prim && task->output) {
    print_prim(*(task->output), *(task->prim), task->indent);
  }
}

// Worker function for printing PrimSpecs to ChunkedStreamWriter
template <size_t ChunkSize, size_t Alignment>
static void print_primspec_chunked_worker(void* user_data) {
  auto* task = static_cast<PrintPrimSpecChunkedTask<ChunkSize, Alignment>*>(user_data);
  if (task && task->primspec && task->output) {
    print_primspec(*(task->output), *(task->primspec), task->indent);
  }
}

template <size_t ChunkSize, size_t Alignment>
void print_prims_parallel(
    ChunkedStreamWriter<ChunkSize, Alignment>& writer,
    const std::vector<const Prim*>& prims,
    uint32_t indent,
    const ParallelPrintConfig& config) {

  // Check if parallel printing is worth it
  if (!config.enabled || prims.size() < config.min_prims_for_parallel) {
    // Fall back to sequential printing
    for (size_t i = 0; i < prims.size(); i++) {
      if (prims[i]) {
        print_prim(writer, *prims[i], indent);
        if (i != (prims.size() - 1)) {
          writer.write("\n");
        }
      }
    }
    return;
  }

  // Prepare output buffers - each thread gets its own ChunkedStreamWriter
  std::vector<ChunkedStreamWriter<ChunkSize, Alignment>> outputs(prims.size());
  std::vector<PrintPrimChunkedTask<ChunkSize, Alignment>> tasks(prims.size());

  // Initialize tasks
  for (size_t i = 0; i < prims.size(); i++) {
    tasks[i] = PrintPrimChunkedTask<ChunkSize, Alignment>(prims[i], indent, i, &outputs[i]);
  }

  // Create task queue
  TaskQueue queue(config.task_queue_capacity);
  std::atomic<size_t> completed_tasks(0);
  std::atomic<bool> producer_done(false);

  // Launch worker threads
  std::vector<std::thread> workers;
  workers.reserve(config.num_threads);

  for (size_t t = 0; t < config.num_threads; t++) {
    workers.emplace_back([&queue, &completed_tasks, &producer_done]() {
      TaskItem task;
      while (!producer_done.load(std::memory_order_acquire) || !queue.Empty()) {
        if (queue.Pop(task)) {
          if (task.func) {
            task.func(task.user_data);
            completed_tasks.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // Producer: push all tasks
  for (size_t i = 0; i < tasks.size(); i++) {
    while (!queue.Push(print_prim_chunked_worker<ChunkSize, Alignment>, &tasks[i])) {
      std::this_thread::yield();
    }
  }

  producer_done.store(true, std::memory_order_release);

  // Wait for all workers to finish
  for (auto& worker : workers) {
    worker.join();
  }

  // OPTIMIZATION: Use zero-copy concat to merge results in original order
  for (size_t i = 0; i < outputs.size(); i++) {
    if (!outputs[i].empty()) {
      writer.concat(std::move(outputs[i]));
      if (i != (outputs.size() - 1)) {
        writer.write("\n");
      }
    }
  }
}

template <size_t ChunkSize, size_t Alignment>
void print_primspecs_parallel(
    ChunkedStreamWriter<ChunkSize, Alignment>& writer,
    const std::vector<const PrimSpec*>& primspecs,
    uint32_t indent,
    const ParallelPrintConfig& config) {

  // Check if parallel printing is worth it
  if (!config.enabled || primspecs.size() < config.min_prims_for_parallel) {
    // Fall back to sequential printing
    for (size_t i = 0; i < primspecs.size(); i++) {
      if (primspecs[i]) {
        print_primspec(writer, *primspecs[i], indent);
        if (i != (primspecs.size() - 1)) {
          writer.write("\n");
        }
      }
    }
    return;
  }

  // Prepare output buffers - each thread gets its own ChunkedStreamWriter
  std::vector<ChunkedStreamWriter<ChunkSize, Alignment>> outputs(primspecs.size());
  std::vector<PrintPrimSpecChunkedTask<ChunkSize, Alignment>> tasks(primspecs.size());

  // Initialize tasks
  for (size_t i = 0; i < primspecs.size(); i++) {
    tasks[i] = PrintPrimSpecChunkedTask<ChunkSize, Alignment>(primspecs[i], indent, i, &outputs[i]);
  }

  // Create task queue
  TaskQueue queue(config.task_queue_capacity);
  std::atomic<size_t> completed_tasks(0);
  std::atomic<bool> producer_done(false);

  // Launch worker threads
  std::vector<std::thread> workers;
  workers.reserve(config.num_threads);

  for (size_t t = 0; t < config.num_threads; t++) {
    workers.emplace_back([&queue, &completed_tasks, &producer_done]() {
      TaskItem task;
      while (!producer_done.load(std::memory_order_acquire) || !queue.Empty()) {
        if (queue.Pop(task)) {
          if (task.func) {
            task.func(task.user_data);
            completed_tasks.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // Producer: push all tasks
  for (size_t i = 0; i < tasks.size(); i++) {
    while (!queue.Push(print_primspec_chunked_worker<ChunkSize, Alignment>, &tasks[i])) {
      std::this_thread::yield();
    }
  }

  producer_done.store(true, std::memory_order_release);

  // Wait for all workers to finish
  for (auto& worker : workers) {
    worker.join();
  }

  // OPTIMIZATION: Use zero-copy concat to merge results in original order
  for (size_t i = 0; i < outputs.size(); i++) {
    if (!outputs[i].empty()) {
      writer.concat(std::move(outputs[i]));
      if (i != (outputs.size() - 1)) {
        writer.write("\n");
      }
    }
  }
}

// Explicit template instantiations for common parameters
template void print_prims_parallel<4096, 16>(
    ChunkedStreamWriter<4096, 16>& writer,
    const std::vector<const Prim*>& prims,
    uint32_t indent,
    const ParallelPrintConfig& config);

template void print_primspecs_parallel<4096, 16>(
    ChunkedStreamWriter<4096, 16>& writer,
    const std::vector<const PrimSpec*>& primspecs,
    uint32_t indent,
    const ParallelPrintConfig& config);

#endif  // LIGHTUSD_ENABLE_THREAD

}  // namespace prim
}  // namespace lightusd
