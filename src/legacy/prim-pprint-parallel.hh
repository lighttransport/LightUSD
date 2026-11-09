// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment Inc.
//
// Parallel pretty-printing for Prim and PrimSpec
//
#pragma once

#include <string>
#include <vector>

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#include <atomic>
#include "task-queue.hh"
#endif

#include "core/prim.hh"       // Prim class
#include "core/prim-spec.hh"  // PrimSpec class
#include "stage.hh"
#include "layer.hh"

namespace lightusd {

// Forward declarations
template <size_t ChunkSize, size_t Alignment> class ChunkedStreamWriter;

namespace prim {

#if defined(LIGHTUSD_ENABLE_THREAD)

///
/// Configuration for parallel printing
///
struct ParallelPrintConfig {
  bool enabled = true;              // Enable parallel printing
  size_t num_threads = 0;           // 0 = auto-detect (std::thread::hardware_concurrency())
  size_t min_prims_for_parallel = 4; // Minimum number of prims to use parallel printing
  size_t task_queue_capacity = 1024; // Task queue capacity

  ParallelPrintConfig() {
    // Auto-detect number of threads
    unsigned int hw_threads = std::thread::hardware_concurrency();
    num_threads = (hw_threads > 0) ? hw_threads : 4;
  }
};

///
/// Task data for printing a Prim
///
struct PrintPrimTask {
  const Prim* prim;
  uint32_t indent;
  size_t index;  // Original index for ordering
  std::string* output;  // Output buffer

  PrintPrimTask() : prim(nullptr), indent(0), index(0), output(nullptr) {}
  PrintPrimTask(const Prim* p, uint32_t i, size_t idx, std::string* out)
    : prim(p), indent(i), index(idx), output(out) {}
};

///
/// Task data for printing a PrimSpec
///
struct PrintPrimSpecTask {
  const PrimSpec* primspec;
  uint32_t indent;
  size_t index;  // Original index for ordering
  std::string* output;  // Output buffer

  PrintPrimSpecTask() : primspec(nullptr), indent(0), index(0), output(nullptr) {}
  PrintPrimSpecTask(const PrimSpec* ps, uint32_t i, size_t idx, std::string* out)
    : primspec(ps), indent(i), index(idx), output(out) {}
};

///
/// Print multiple Prims in parallel
///
/// @param[in] prims Vector of Prim pointers to print
/// @param[in] indent Indentation level
/// @param[in] config Parallel printing configuration
/// @return Concatenated string of all printed prims
///
std::string print_prims_parallel(
    const std::vector<const Prim*>& prims,
    uint32_t indent,
    const ParallelPrintConfig& config = ParallelPrintConfig());

///
/// Print multiple PrimSpecs in parallel
///
/// @param[in] primspecs Vector of PrimSpec pointers to print
/// @param[in] indent Indentation level
/// @param[in] config Parallel printing configuration
/// @return Concatenated string of all printed primspecs
///
std::string print_primspecs_parallel(
    const std::vector<const PrimSpec*>& primspecs,
    uint32_t indent,
    const ParallelPrintConfig& config = ParallelPrintConfig());

///
/// Print multiple Prims in parallel using ChunkedStreamWriter for zero-copy concatenation
///
/// @param[out] writer ChunkedStreamWriter to write output
/// @param[in] prims Vector of Prim pointers to print
/// @param[in] indent Indentation level
/// @param[in] config Parallel printing configuration
///
template <size_t ChunkSize = 4096, size_t Alignment = 16>
void print_prims_parallel(
    ChunkedStreamWriter<ChunkSize, Alignment>& writer,
    const std::vector<const Prim*>& prims,
    uint32_t indent,
    const ParallelPrintConfig& config = ParallelPrintConfig());

///
/// Print multiple PrimSpecs in parallel using ChunkedStreamWriter for zero-copy concatenation
///
/// @param[out] writer ChunkedStreamWriter to write output
/// @param[in] primspecs Vector of PrimSpec pointers to print
/// @param[in] indent Indentation level
/// @param[in] config Parallel printing configuration
///
template <size_t ChunkSize = 4096, size_t Alignment = 16>
void print_primspecs_parallel(
    ChunkedStreamWriter<ChunkSize, Alignment>& writer,
    const std::vector<const PrimSpec*>& primspecs,
    uint32_t indent,
    const ParallelPrintConfig& config = ParallelPrintConfig());

#endif  // LIGHTUSD_ENABLE_THREAD

}  // namespace prim
}  // namespace lightusd
