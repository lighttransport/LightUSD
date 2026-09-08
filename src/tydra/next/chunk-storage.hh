// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace lightusd {
namespace tydra {
namespace next {

using ChunkAllocHook = bool (*)(size_t bytes);
using ChunkFreeHook = void (*)(size_t bytes);
// Install/uninstall on the conversion thread, with both hooks set or both null.
void SetChunkAllocHooks(ChunkAllocHook alloc, ChunkFreeHook free_hook);
bool ProbeAlloc(size_t bytes);

namespace detail {

// One compiled implementation for all POD chunk arrays. Layout is explicit;
// ownership, allocation and bulk copying never instantiate per element type.
// Sharing is thread-compatible, like the typed facade: mutation of the same
// instance must be externally synchronized.
class ChunkStorage {
 public:
  ChunkStorage(size_t element_size, size_t alignment, size_t chunk_elements)
      : chunk_elements_(chunk_elements),
        element_size_(static_cast<uint32_t>(element_size)),
        alignment_(static_cast<uint32_t>(alignment)) {
    if (!element_size || element_size > UINT32_MAX || !alignment ||
        alignment > UINT32_MAX || (alignment & (alignment - 1)) ||
        element_size % alignment || !chunk_elements ||
        chunk_elements > size_t(PTRDIFF_MAX) / element_size) std::abort();
  }
  ~ChunkStorage();
  ChunkStorage(ChunkStorage&& other) noexcept;
  ChunkStorage& operator=(ChunkStorage&& other) noexcept;
  ChunkStorage(const ChunkStorage&) = delete;
  ChunkStorage& operator=(const ChunkStorage&) = delete;

  void share_from(const ChunkStorage& other);
  bool is_shared() const;
  bool reserve(size_t count);
  bool resize(size_t count) {
    if ((maybe_shared_ || count > capacity()) && !reserve(count)) return false;
    size_ = count;
    return true;
  }
  bool append(const void* data, size_t count);
  void shrink_to_fit();
  bool copy_to(void* dest, size_t count) const;
  void clear() { size_ = 0; }
  size_t size() const { return size_; }
  // Typed facade only: caller has established uniqueness and capacity.
  void set_size_unchecked(size_t count) { size_ = count; }
  size_t capacity() const {
    return control_ && control_->count
        ? (control_->count - 1) * chunk_elements_ + control_->tail : 0;
  }
  size_t chunk_count() const { return control_ ? control_->count : 0; }
  size_t chunk_size(size_t index) const;
  size_t allocated_bytes() const { return capacity() * element_size_; }
  bool alloc_failed() const { return alloc_failed_; }
  bool make_unique();

  const void* chunk_data(size_t index) const { return control_->chunks[index]; }
  void* mutable_chunk_data(size_t index) {
    if (maybe_shared_) require_unique();
    return control_->chunks[index];
  }

 private:
  struct Control {
    std::atomic<size_t> references{1};
    void** chunks = nullptr;
    size_t count = 0;
    size_t slots = 0;
    size_t tail = 0;
  };
  void release();
  void require_unique();
  bool ensure_capacity(size_t count);
  void* allocate_chunk(size_t count) const;
  void free_chunk(void* data, size_t count) const;
  bool fail() { alloc_failed_ = true; return false; }

  Control* control_ = nullptr;
  size_t size_ = 0;
  size_t chunk_elements_;
  uint32_t element_size_;
  uint32_t alignment_;
  bool alloc_failed_ = false;
  mutable bool maybe_shared_ = false;
};

}  // namespace detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
