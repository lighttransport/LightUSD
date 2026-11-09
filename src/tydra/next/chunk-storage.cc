// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "chunk-storage.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace lightusd {
namespace tydra {
namespace next {
namespace {
thread_local std::shared_ptr<ChunkAllocationBudget> current_budget;
}

ScopedChunkAllocationBudget::ScopedChunkAllocationBudget(
    std::shared_ptr<ChunkAllocationBudget> budget)
    : previous_(std::move(current_budget)) {
  current_budget = std::move(budget);
}

ScopedChunkAllocationBudget::~ScopedChunkAllocationBudget() {
  current_budget = std::move(previous_);
}

bool ProbeAlloc(size_t bytes) {
  if (!bytes) return true;
  if (bytes > size_t((std::numeric_limits<std::ptrdiff_t>::max)())) return false;
  void* p = std::malloc(bytes);
  if (!p) return false;
  std::free(p);
  return true;
}

ChunkAllocationBudget::ChunkAllocationBudget(size_t cap) : budget_(lu_budget_create(cap)) {
  if (!budget_) std::abort();
}
ChunkAllocationBudget::~ChunkAllocationBudget() { lu_budget_release(budget_); }
namespace detail {
bool ChunkStorage::reserve(size_t count) {
  return lu_chunks_reserve(&state_, count, 0, current_budget ? current_budget->core() : nullptr) != 0;
}
bool ChunkStorage::reserve_exact(size_t count) {
  return lu_chunks_reserve(&state_, count, 1, current_budget ? current_budget->core() : nullptr) != 0;
}
bool ChunkStorage::append(const void* data, size_t count) {
  return lu_chunks_append(&state_, data, count, current_budget ? current_budget->core() : nullptr) != 0;
}
} // namespace detail
} // namespace next
} // namespace tydra
} // namespace lightusd
