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
ChunkAllocHook alloc_hook = nullptr;
ChunkFreeHook free_hook = nullptr;
}

void SetChunkAllocHooks(ChunkAllocHook alloc, ChunkFreeHook release) {
  // An allocation charge must always have a matching release operation.
  if ((alloc == nullptr) != (release == nullptr)) std::abort();
  alloc_hook = alloc;
  free_hook = release;
}

bool ProbeAlloc(size_t bytes) {
  if (!bytes) return true;
  if (bytes > size_t((std::numeric_limits<std::ptrdiff_t>::max)())) return false;
  void* p = std::malloc(bytes);
  if (!p) return false;
  std::free(p);
  return true;
}

namespace detail {

ChunkStorage::~ChunkStorage() { release(); }

ChunkStorage::ChunkStorage(ChunkStorage&& other) noexcept
    : control_(other.control_), size_(other.size_),
      chunk_elements_(other.chunk_elements_), element_size_(other.element_size_),
      alignment_(other.alignment_), alloc_failed_(other.alloc_failed_),
      maybe_shared_(other.maybe_shared_) {
  other.control_ = nullptr;
  other.size_ = 0;
  other.alloc_failed_ = false;
  other.maybe_shared_ = false;
}

ChunkStorage& ChunkStorage::operator=(ChunkStorage&& other) noexcept {
  if (this == &other) return *this;
  release();
  control_ = other.control_;
  size_ = other.size_;
  chunk_elements_ = other.chunk_elements_;
  element_size_ = other.element_size_;
  alignment_ = other.alignment_;
  alloc_failed_ = other.alloc_failed_;
  maybe_shared_ = other.maybe_shared_;
  other.control_ = nullptr;
  other.size_ = 0;
  other.alloc_failed_ = false;
  other.maybe_shared_ = false;
  return *this;
}

void* ChunkStorage::allocate_chunk(size_t count) const {
  const size_t bytes = count * element_size_;
  if (alloc_hook && !alloc_hook(bytes)) return nullptr;
  void* data = alignment_ > alignof(std::max_align_t)
      ? ::operator new(bytes, std::align_val_t(alignment_), std::nothrow)
      : std::malloc(bytes);
  if (!data && free_hook) free_hook(bytes);
  return data;
}

void ChunkStorage::free_chunk(void* data, size_t count) const {
  if (alignment_ > alignof(std::max_align_t))
    ::operator delete(data, std::align_val_t(alignment_));
  else
    std::free(data);
  if (free_hook) free_hook(count * element_size_);
}

void ChunkStorage::release() {
  if (!control_ ||
      control_->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  for (size_t i = 0; i < control_->count; ++i) {
    free_chunk(control_->chunks[i],
               i + 1 == control_->count ? control_->tail : chunk_elements_);
  }
  std::free(control_->chunks);
  delete control_;
}

void ChunkStorage::share_from(const ChunkStorage& other) {
  if (this == &other) return;
  if (element_size_ != other.element_size_ || alignment_ != other.alignment_ ||
      chunk_elements_ != other.chunk_elements_) std::abort();
  if (other.control_ && other.control_->references.fetch_add(
          1, std::memory_order_relaxed) == (std::numeric_limits<size_t>::max)())
    std::abort();
  release();
  control_ = other.control_;
  size_ = other.size_;
  alloc_failed_ = other.alloc_failed_;
  maybe_shared_ = control_ != nullptr;
  if (control_) other.maybe_shared_ = true;
}

bool ChunkStorage::is_shared() const {
  return maybe_shared_ && control_ &&
      control_->references.load(std::memory_order_acquire) > 1;
}

bool ChunkStorage::make_unique() {
  if (!is_shared()) { maybe_shared_ = false; return true; }
  ChunkStorage copy(element_size_, alignment_, chunk_elements_);
  copy.control_ = new (std::nothrow) Control;
  if (!copy.control_) return fail();
  const size_t count = control_->count;
  if (count) {
    copy.control_->chunks = static_cast<void**>(std::malloc(count * sizeof(void*)));
    if (!copy.control_->chunks) return fail();
  }
  copy.control_->slots = count;
  for (size_t i = 0; i < count; ++i) {
    const size_t n = i + 1 == count ? control_->tail : chunk_elements_;
    void* p = allocate_chunk(n);
    if (!p) return fail();
    std::memcpy(p, control_->chunks[i], n * element_size_);
    copy.control_->chunks[copy.control_->count++] = p;
    copy.control_->tail = n;
  }
  copy.size_ = size_;
  copy.alloc_failed_ = alloc_failed_;
  *this = std::move(copy);
  return true;
}

void ChunkStorage::require_unique() {
  if (!make_unique()) {
    std::fputs("ChunkedArray: unable to detach shared storage for write\n", stderr);
    std::abort();
  }
}

bool ChunkStorage::ensure_capacity(size_t count) {
  if (count <= capacity()) return true;
  const size_t max_elements =
      size_t((std::numeric_limits<std::ptrdiff_t>::max)()) / element_size_;
  if (count >= max_elements) return fail();
  const size_t needed = count / chunk_elements_ + (count % chunk_elements_ != 0);
  // Include rounding to full chunks in the address-space bound.
  if (needed > max_elements / chunk_elements_ ||
      needed > size_t((std::numeric_limits<std::ptrdiff_t>::max)()) /
                   sizeof(void*)) return fail();
  if (!control_) {
    control_ = new (std::nothrow) Control;
    if (!control_) return fail();
  }
  if (needed > control_->slots) {
    void* table = std::realloc(control_->chunks, needed * sizeof(void*));
    if (!table) return fail();
    control_->chunks = static_cast<void**>(table);
    control_->slots = needed;
  }
  if (control_->count && control_->tail < chunk_elements_) {
    void* p = allocate_chunk(chunk_elements_);
    if (!p) return fail();
    const size_t last = control_->count - 1;
    std::memcpy(p, control_->chunks[last], control_->tail * element_size_);
    free_chunk(control_->chunks[last], control_->tail);
    control_->chunks[last] = p;
    control_->tail = chunk_elements_;
  }
  while (control_->count < needed) {
    void* p = allocate_chunk(chunk_elements_);
    if (!p) return fail();
    control_->chunks[control_->count++] = p;
    control_->tail = chunk_elements_;
  }
  return true;
}

bool ChunkStorage::reserve(size_t count) {
  return make_unique() && ensure_capacity(count);
}

bool ChunkStorage::append(const void* data, size_t count) {
  if (!count) return true;
  if (!data || count > size_t((std::numeric_limits<std::ptrdiff_t>::max)()) /
                           element_size_ ||
      count > (std::numeric_limits<size_t>::max)() - size_) return fail();
  // A caller may append a view of an existing chunk. Preserve it across COW
  // detachment or expansion of an exact-sized tail.
  void* saved = nullptr;
  const uintptr_t address = reinterpret_cast<uintptr_t>(data);
  // Full, uniquely owned chunks never move during append. Only a shared
  // backing or an expanding short tail can invalidate a borrowed source.
  // Avoid scanning all previous chunks on the normal append fast path.
  size_t first = chunk_count();
  if (is_shared()) first = 0;
  else if (control_ && control_->count && control_->tail < chunk_elements_ &&
           size_ + count > capacity()) first = control_->count - 1;
  for (size_t i = first; i < chunk_count(); ++i) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(chunk_data(i));
    const size_t bytes = chunk_size(i) * element_size_;
    if (address >= start && address - start < bytes) {
      if (count * element_size_ > bytes - (address - start)) return fail();
      saved = std::malloc(count * element_size_);
      if (!saved) return fail();
      std::memcpy(saved, data, count * element_size_);
      data = saved;
      break;
    }
  }
  if (!reserve(size_ + count)) { std::free(saved); return false; }
  const auto* src = static_cast<const unsigned char*>(data);
  while (count) {
    const size_t offset = size_ % chunk_elements_;
    const size_t space = chunk_elements_ - offset;
    const size_t n = count < space ? count : space;
    auto* dest = static_cast<unsigned char*>(control_->chunks[size_ / chunk_elements_]);
    std::memcpy(dest + offset * element_size_, src, n * element_size_);
    size_ += n;
    src += n * element_size_;
    count -= n;
  }
  std::free(saved);
  return true;
}

size_t ChunkStorage::chunk_size(size_t index) const {
  if (index >= chunk_count()) return 0;
  const size_t start = index * chunk_elements_;
  if (start >= size_) return 0;
  const size_t remaining = size_ - start;
  return remaining < chunk_elements_ ? remaining : chunk_elements_;
}

bool ChunkStorage::copy_to(void* dest, size_t count) const {
  if (count < size_ || (size_ && !dest)) return false;
  auto* out = static_cast<unsigned char*>(dest);
  for (size_t i = 0; i < chunk_count(); ++i) {
    const size_t bytes = chunk_size(i) * element_size_;
    if (!bytes) break;
    std::memcpy(out, chunk_data(i), bytes);
    out += bytes;
  }
  return true;
}

void ChunkStorage::shrink_to_fit() {
  if (!control_ || !make_unique()) return;
  const size_t needed = size_ / chunk_elements_ + (size_ % chunk_elements_ != 0);
  while (control_->count > needed) {
    free_chunk(control_->chunks[control_->count - 1], control_->tail);
    --control_->count;
    control_->tail = control_->count ? chunk_elements_ : 0;
  }
  if (!needed) return;
  const size_t used = size_ - (needed - 1) * chunk_elements_;
  if (used >= control_->tail) return;
  void* p = allocate_chunk(used);
  if (!p) return;  // Optional compaction does not latch allocation failure.
  std::memcpy(p, control_->chunks[needed - 1], used * element_size_);
  free_chunk(control_->chunks[needed - 1], control_->tail);
  control_->chunks[needed - 1] = p;
  control_->tail = used;
}

}  // namespace detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
