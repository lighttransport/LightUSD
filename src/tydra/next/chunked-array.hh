// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <type_traits>
#include <vector>
#include "chunk-storage.hh"

namespace lightusd {
namespace tydra {
namespace next {

constexpr size_t kDefaultChunkSize = 64 * 1024;

// Typed indexing only. Ownership, allocation, COW, compaction and bulk copies
// are compiled once in ChunkStorage, independent of the element type.
template <typename T, size_t ChunkBytes = kDefaultChunkSize>
class ChunkedArray {
 public:
  static constexpr size_t kElementsPerChunk = ChunkBytes / sizeof(T);
  static_assert(kElementsPerChunk > 0, "Element size too large for chunk");
  static_assert(std::is_trivial<T>::value,
                "ChunkedArray requires POD elements without constructors");

  ChunkedArray() : storage_(sizeof(T), alignof(T), kElementsPerChunk) {}
  ChunkedArray(ChunkedArray&&) noexcept = default;
  ChunkedArray& operator=(ChunkedArray&&) noexcept = default;
  ChunkedArray(const ChunkedArray&) = delete;
  ChunkedArray& operator=(const ChunkedArray&) = delete;

  void share_from(const ChunkedArray& other) {
    if (this == &other) return;
    storage_.share_from(other.storage_);
    maybe_shared_ = true;
    other.maybe_shared_ = true;
  }
  bool is_shared() const { return storage_.is_shared(); }
  bool reserve(size_t n) {
    if (!storage_.reserve(n)) return false;
    maybe_shared_ = false;
    return true;
  }
  bool resize(size_t n) {
    if (!storage_.resize(n)) return false;
    maybe_shared_ = false;
    return true;
  }
  bool resize(size_t n, const T& value) {
    // Copy before growth: value may refer to the old tail allocation.
    const T fill = value;
    const size_t previous = size();
    if (!resize(n)) return false;
    size_t i = previous;
    while (i < n) {
      const size_t offset = i % kElementsPerChunk;
      const size_t space = kElementsPerChunk - offset;
      const size_t count = n - i < space ? n - i : space;
      T* dest = const_cast<T*>(chunk_data(i / kElementsPerChunk)) + offset;
      for (size_t j = 0; j < count; ++j) dest[j] = fill;
      i += count;
    }
    return true;
  }
  size_t push_back(const T& value) {
    const T copy = value;
    const size_t index = size();
    if (index == (std::numeric_limits<size_t>::max)()) {
      storage_.reserve(index);
      return index;
    }
    if (!resize(index + 1)) return (std::numeric_limits<size_t>::max)();
    // resize already established bounds and uniqueness.
    const_cast<T*>(chunk_data(index / kElementsPerChunk))
        [index % kElementsPerChunk] = copy;
    return index;
  }
  bool append(const T* data, size_t count) {
    if (!count) return true;
    const size_t previous = size();
    const size_t offset = previous % kElementsPerChunk;
    if (!maybe_shared_ && data && count <= capacity() - previous &&
        count <= kElementsPerChunk - offset) {
      T* dest = const_cast<T*>(static_cast<const T*>(
          storage_.chunk_data(previous / kElementsPerChunk))) + offset;
      std::memmove(dest, data, count * sizeof(T));
      storage_.set_size_unchecked(previous + count);
      return true;
    }
    if (!storage_.append(data, count)) return false;
    if (count) maybe_shared_ = false;
    return true;
  }
  bool alloc_failed() const { return storage_.alloc_failed(); }
  void clear() { storage_.clear(); }
  void shrink_to_fit() { storage_.shrink_to_fit(); }

  // Reads never detach. Mutation is explicit so inspecting a shared array
  // cannot allocate a second geometry buffer or introduce COW work in a loop.
  T& mutable_at(size_t index) {
    check_index(index);
    require_unique();
    return const_cast<T*>(static_cast<const T*>(storage_.chunk_data(index / kElementsPerChunk)))
        [index % kElementsPerChunk];
  }
  const T& operator[](size_t index) const {
    return static_cast<const T*>(storage_.chunk_data(index / kElementsPerChunk))
        [index % kElementsPerChunk];
  }
  const T& at(size_t index) const { check_index(index); return (*this)[index]; }
  template <size_t N>
  void read_n(size_t base, T* out) const {
    for (size_t i = 0; i < N; ++i) out[i] = (*this)[base + i];
  }
  void read2(size_t base, T* out) const { read_n<2>(base, out); }
  void read3(size_t base, T* out) const { read_n<3>(base, out); }
  const T& front() const { return (*this)[0]; }
  const T& back() const { return (*this)[size() - 1]; }
  size_t size() const { return storage_.size(); }
  bool empty() const { return size() == 0; }
  size_t capacity() const { return storage_.capacity(); }
  size_t chunk_count() const { return storage_.chunk_count(); }
  size_t chunk_size(size_t index) const { return storage_.chunk_size(index); }
  bool is_contiguous() const { return size() <= kElementsPerChunk; }
  size_t memory_usage() const { return sizeof(*this) + storage_.allocated_bytes(); }
  T* mutable_chunk_data(size_t index) {
    require_unique();
    return const_cast<T*>(static_cast<const T*>(storage_.chunk_data(index)));
  }
  const T* chunk_data(size_t index) const {
    return static_cast<const T*>(storage_.chunk_data(index));
  }
  bool copy_to(T* dest) const { return storage_.copy_to(dest, size()); }
  bool copy_to(T* dest, size_t count) const { return storage_.copy_to(dest, count); }
  std::vector<T> flatten() const {
    std::vector<T> result(size());
    copy_to(result.data());
    return result;
  }

  // Iterator support
  class iterator {
   public:
    using value_type = T;
    using pointer = T*;
    using reference = T&;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::random_access_iterator_tag;

    iterator(ChunkedArray* arr, size_t idx) : arr_(arr), idx_(idx) {}

    reference operator*() { return arr_->mutable_at(idx_); }
    pointer operator->() { return &arr_->mutable_at(idx_); }

    iterator& operator++() { ++idx_; return *this; }
    iterator operator++(int) { iterator tmp = *this; ++idx_; return tmp; }
    iterator& operator--() { --idx_; return *this; }
    iterator operator--(int) { iterator tmp = *this; --idx_; return tmp; }

    iterator& operator+=(difference_type n) { idx_ += n; return *this; }
    iterator& operator-=(difference_type n) { idx_ -= n; return *this; }
    iterator operator+(difference_type n) const { return iterator(arr_, idx_ + n); }
    iterator operator-(difference_type n) const { return iterator(arr_, idx_ - n); }
    difference_type operator-(const iterator& other) const {
      return static_cast<difference_type>(idx_) - static_cast<difference_type>(other.idx_);
    }

    bool operator==(const iterator& other) const { return idx_ == other.idx_; }
    bool operator!=(const iterator& other) const { return idx_ != other.idx_; }
    bool operator<(const iterator& other) const { return idx_ < other.idx_; }
    bool operator>(const iterator& other) const { return idx_ > other.idx_; }
    bool operator<=(const iterator& other) const { return idx_ <= other.idx_; }
    bool operator>=(const iterator& other) const { return idx_ >= other.idx_; }

   private:
    ChunkedArray* arr_;
    size_t idx_;
  };

  class const_iterator {
   public:
    using value_type = T;
    using pointer = const T*;
    using reference = const T&;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() : arr_(nullptr), idx_(0) {}
    const_iterator(const ChunkedArray* arr, size_t idx) : arr_(arr), idx_(idx) {}

    reference operator*() const { return (*arr_)[idx_]; }
    pointer operator->() const { return &(*arr_)[idx_]; }

    const_iterator& operator++() { ++idx_; return *this; }
    const_iterator operator++(int) { const_iterator tmp = *this; ++idx_; return tmp; }

    bool operator==(const const_iterator& other) const {
      return arr_ == other.arr_ && idx_ == other.idx_;
    }
    bool operator!=(const const_iterator& other) const { return !(*this == other); }

   private:
    const ChunkedArray* arr_;
    size_t idx_;
  };

  iterator mutable_begin() { return iterator(this, 0); }
  iterator mutable_end() { return iterator(this, size()); }
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, size()); }
  const_iterator cbegin() const { return const_iterator(this, 0); }
  const_iterator cend() const { return const_iterator(this, size()); }

 private:
  void require_unique() {
    if (!maybe_shared_) return;
    if (!storage_.make_unique()) {
      std::fputs("ChunkedArray: unable to detach shared storage for write\n", stderr);
      std::abort();
    }
    maybe_shared_ = false;
  }
  void check_index(size_t index) const {
    if (index >= size()) {
      std::fprintf(stderr, "ChunkedArray::at: index %zu out of range (size %zu)\n",
                   index, size());
      std::abort();
    }
  }
  detail::ChunkStorage storage_;
  // Keep the typed indexing fast-path flag outside the compiled storage
  // operations. They cannot set it, so loops over unique arrays can hoist it.
  mutable bool maybe_shared_ = false;
};

// Type aliases for common vertex data
using FloatChunked = ChunkedArray<float>;
using DoubleChunked = ChunkedArray<double>;
using Int32Chunked = ChunkedArray<int32_t>;
using UInt32Chunked = ChunkedArray<uint32_t>;
using UInt16Chunked = ChunkedArray<uint16_t>;
using UInt8Chunked = ChunkedArray<uint8_t>;

// Vec3 chunk (for positions, normals)
struct alignas(16) Vec3f {
  float x, y, z;
  float _pad;  // Padding for alignment
};
using Vec3fChunked = ChunkedArray<Vec3f>;

// Vec2 chunk (for UVs)
struct Vec2f {
  float u, v;
};
using Vec2fChunked = ChunkedArray<Vec2f>;

// Vec4 chunk (for colors, tangents)
struct alignas(16) Vec4f {
  float x, y, z, w;
};
using Vec4fChunked = ChunkedArray<Vec4f>;

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
