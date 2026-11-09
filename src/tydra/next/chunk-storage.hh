// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include "../../next/c/chunks.h"

namespace lightusd {
namespace tydra {
namespace next {

/// Thread-safe accounting object owned by one conversion. Chunk storage keeps
/// it alive for as long as the resulting scene retains any accounted arrays.
class ChunkAllocationBudget {
 public:
  explicit ChunkAllocationBudget(size_t cap = (std::numeric_limits<size_t>::max)());
  ~ChunkAllocationBudget();
  ChunkAllocationBudget(const ChunkAllocationBudget&) = delete;
  ChunkAllocationBudget& operator=(const ChunkAllocationBudget&) = delete;
  bool TryAdd(size_t bytes) { return lu_budget_add(budget_, bytes) != 0; }
  void Sub(size_t bytes) { lu_budget_sub(budget_, bytes); }
  size_t Cap() const { return lu_budget_limit(budget_); }
  size_t Tracked() const { return lu_budget_current(budget_); }
  size_t PeakTracked() const { return lu_budget_peak(budget_); }
  lu_budget* core() const { return budget_; }
 private:
  lu_budget* budget_;
};

/// Installs an allocation budget for arrays constructed on the current thread.
/// The captured shared object, rather than this thread-local scope, is retained
/// by their storage and is safe to charge from worker threads.
class ScopedChunkAllocationBudget {
 public:
  explicit ScopedChunkAllocationBudget(
      std::shared_ptr<ChunkAllocationBudget> budget);
  ~ScopedChunkAllocationBudget();
  ScopedChunkAllocationBudget(const ScopedChunkAllocationBudget&) = delete;
  ScopedChunkAllocationBudget& operator=(const ScopedChunkAllocationBudget&) =
      delete;

 private:
  std::shared_ptr<ChunkAllocationBudget> previous_;
};

bool ProbeAlloc(size_t bytes);

namespace detail {

// One compiled implementation for all POD chunk arrays. Layout is explicit;
// ownership, allocation and bulk copying never instantiate per element type.
// Sharing is thread-compatible, like the typed facade: mutation of the same
// instance must be externally synchronized.
class ChunkStorage {
 public:
  ChunkStorage(size_t element, size_t alignment, size_t chunk) {
    if (!lu_chunks_init(&state_, element, alignment, chunk)) std::abort();
  }
  ~ChunkStorage() { lu_chunks_destroy(&state_); }
  ChunkStorage(ChunkStorage&& other) noexcept : ChunkStorage(other.state_.element_size, other.state_.alignment, other.state_.chunk_elements) { lu_chunks_move(&state_, &other.state_); }
  ChunkStorage& operator=(ChunkStorage&& other) noexcept { lu_chunks_move(&state_, &other.state_); return *this; }
  ChunkStorage(const ChunkStorage&) = delete;
  ChunkStorage& operator=(const ChunkStorage&) = delete;
  void share_from(const ChunkStorage& other) { lu_chunks_share(&state_, &other.state_); }
  bool is_shared() const { return lu_chunks_shared(&state_) != 0; }
  bool reserve(size_t count);
  bool reserve_exact(size_t count);
  bool resize(size_t count) {
    if ((state_.maybe_shared || count > capacity()) && !reserve(count)) return false;
    state_.size = count; return true;
  }
  bool resize_exact(size_t count) {
    if ((state_.maybe_shared || count > capacity()) && !reserve_exact(count)) return false;
    state_.size = count; return true;
  }
  bool append(const void* data, size_t count);
  void shrink_to_fit() { lu_chunks_compact(&state_); }
  bool copy_to(void* dest, size_t count) const { return lu_chunks_copy(&state_, dest, count) != 0; }
  void clear() { state_.size = 0; }
  size_t size() const { return state_.size; }
  void set_size_unchecked(size_t count) { state_.size = count; }
  size_t capacity() const { return state_.table && state_.table->count ? (state_.table->count - 1) * state_.chunk_elements + state_.table->tail : 0; }
  size_t chunk_count() const { return state_.table ? state_.table->count : 0; }
  size_t chunk_size(size_t index) const {
    if (index >= chunk_count() || index * state_.chunk_elements >= size()) return 0;
    const size_t remaining = size() - index * state_.chunk_elements;
    return remaining < state_.chunk_elements ? remaining : state_.chunk_elements;
  }
  size_t allocated_bytes() const { return capacity() * state_.element_size; }
  bool alloc_failed() const { return state_.failed != 0; }
  bool make_unique() { return lu_chunks_unique(&state_) != 0; }
  const void* chunk_data(size_t index) const { return state_.table->chunks[index]; }
  void* mutable_chunk_data(size_t index) { if (!make_unique()) std::abort(); return state_.table->chunks[index]; }
 private:
  mutable lu_chunks state_{};
};

}  // namespace detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
