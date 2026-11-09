// SPDX-License-Identifier: MIT
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace lightusd {

///
/// Buffer class to replace std::vector<uint8_t> with manual memory management
/// and configurable alignment.
///
/// @tparam Alignment Memory alignment in bytes (default: 16)
///
template <size_t Alignment = 16>
class Buffer {
 public:
  Buffer() : data_(nullptr), size_(0), capacity_(0) {}

  ~Buffer() { free_memory(); }

  // Copy constructor
  Buffer(const Buffer& other) : data_(nullptr), size_(0), capacity_(0) {
    if (other.size_ > 0) {
      resize(other.size_);
      std::memcpy(data_, other.data_, other.size_);
    }
  }

  // Move constructor
  Buffer(Buffer&& other) noexcept
      : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }

  // Copy assignment
  Buffer& operator=(const Buffer& other) {
    if (this != &other) {
      if (other.size_ > 0) {
        resize(other.size_);
        std::memcpy(data_, other.data_, other.size_);
      } else {
        clear();
      }
    }
    return *this;
  }

  // Move assignment
  Buffer& operator=(Buffer&& other) noexcept {
    if (this != &other) {
      free_memory();
      data_ = other.data_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }

  ///
  /// Check if buffer is empty
  ///
  bool empty() const { return size_ == 0; }

  ///
  /// Get current size in bytes
  ///
  size_t size() const { return size_; }

  ///
  /// Get current capacity in bytes
  ///
  size_t capacity() const { return capacity_; }

  ///
  /// Resize buffer to new_size bytes
  /// If new_size > capacity, reallocates memory
  /// If new_size < size, shrinks size but keeps capacity
  ///
  void resize(size_t new_size) {
    if (new_size > capacity_) {
      reallocate(new_size);
    }
    size_ = new_size;
  }

  ///
  /// Shrink capacity to match current size
  ///
  void shrink_to_fit() {
    if (size_ < capacity_) {
      if (size_ == 0) {
        free_memory();
      } else {
        reallocate(size_);
      }
    }
  }

  ///
  /// Clear buffer (sets size to 0, keeps capacity)
  ///
  void clear() { size_ = 0; }

  ///
  /// Get raw pointer to data
  ///
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }

  ///
  /// Reserve capacity without changing size
  ///
  void reserve(size_t new_capacity) {
    if (new_capacity > capacity_) {
      reallocate(new_capacity);
    }
  }

  ///
  /// Array access operator
  ///
  uint8_t& operator[](size_t index) { return data_[index]; }
  const uint8_t& operator[](size_t index) const { return data_[index]; }

  ///
  /// Push back a single byte
  ///
  void push_back(uint8_t value) {
    size_t old_size = size_;
    resize(size_ + 1);
    data_[old_size] = value;
  }

  ///
  /// Iterator support for range-based for loops
  ///
  uint8_t* begin() { return data_; }
  const uint8_t* begin() const { return data_; }
  uint8_t* end() { return data_ + size_; }
  const uint8_t* end() const { return data_ + size_; }

 private:
  void free_memory() {
    if (data_) {
#ifdef _WIN32
      _aligned_free(data_);
#else
      free(data_);
#endif
      data_ = nullptr;
      capacity_ = 0;
      size_ = 0;
    }
  }

  void reallocate(size_t new_capacity) {
    uint8_t* new_data = nullptr;

#ifdef _WIN32
    new_data = static_cast<uint8_t*>(_aligned_malloc(new_capacity, Alignment));
#else
    if (posix_memalign(reinterpret_cast<void**>(&new_data), Alignment,
                       new_capacity) != 0) {
      new_data = nullptr;
    }
#endif

    if (!new_data) {
      // Allocation failed. Keep existing buffer.
      return;
    }

    if (data_ && size_ > 0) {
      std::memcpy(new_data, data_, size_);
    }

    free_memory();
    data_ = new_data;
    capacity_ = new_capacity;
  }

  uint8_t* data_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
};

///
/// ChunkedBuffer class that allocates memory in fixed-size chunks instead of
/// one contiguous block. This reduces memory fragmentation and reallocation
/// overhead for large buffers.
///
/// @tparam ChunkSize Size of each chunk in bytes (default: 4096 = 4KB)
/// @tparam Alignment Memory alignment in bytes (default: 16)
///
template <size_t ChunkSize = 4096, size_t Alignment = 16>
class ChunkedBuffer {
 public:
  ///
  /// Iterator class for ChunkedBuffer
  ///
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = uint8_t;
    using difference_type = std::ptrdiff_t;
    using pointer = uint8_t*;
    using reference = uint8_t&;

    Iterator() : buffer_(nullptr), index_(0) {}

    Iterator(ChunkedBuffer* buffer, size_t index)
        : buffer_(buffer), index_(index) {}

    reference operator*() { return (*buffer_)[index_]; }

    pointer operator->() { return &(*buffer_)[index_]; }

    Iterator& operator++() {
      ++index_;
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      ++index_;
      return tmp;
    }

    bool operator==(const Iterator& other) const {
      return buffer_ == other.buffer_ && index_ == other.index_;
    }

    bool operator!=(const Iterator& other) const { return !(*this == other); }

   private:
    ChunkedBuffer* buffer_;
    size_t index_;
  };

  ///
  /// Const iterator class for ChunkedBuffer
  ///
  class ConstIterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = uint8_t;
    using difference_type = std::ptrdiff_t;
    using pointer = const uint8_t*;
    using reference = const uint8_t&;

    ConstIterator() : buffer_(nullptr), index_(0) {}

    ConstIterator(const ChunkedBuffer* buffer, size_t index)
        : buffer_(buffer), index_(index) {}

    reference operator*() const { return (*buffer_)[index_]; }

    pointer operator->() const { return &(*buffer_)[index_]; }

    ConstIterator& operator++() {
      ++index_;
      return *this;
    }

    ConstIterator operator++(int) {
      ConstIterator tmp = *this;
      ++index_;
      return tmp;
    }

    bool operator==(const ConstIterator& other) const {
      return buffer_ == other.buffer_ && index_ == other.index_;
    }

    bool operator!=(const ConstIterator& other) const {
      return !(*this == other);
    }

   private:
    const ChunkedBuffer* buffer_;
    size_t index_;
  };

 private:
  struct Chunk {
    uint8_t* data;
    size_t size;  // Actual used size in this chunk

    Chunk() : data(nullptr), size(0) {}

    ~Chunk() { free_chunk(); }

    // Move constructor
    Chunk(Chunk&& other) noexcept : data(other.data), size(other.size) {
      other.data = nullptr;
      other.size = 0;
    }

    // Move assignment
    Chunk& operator=(Chunk&& other) noexcept {
      if (this != &other) {
        free_chunk();
        data = other.data;
        size = other.size;
        other.data = nullptr;
        other.size = 0;
      }
      return *this;
    }

    // Disable copy
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    void allocate() {
      if (data) return;

#ifdef _WIN32
      data = static_cast<uint8_t*>(_aligned_malloc(ChunkSize, Alignment));
#else
      if (posix_memalign(reinterpret_cast<void**>(&data), Alignment,
                         ChunkSize) != 0) {
        data = nullptr;
      }
#endif
    }

    void free_chunk() {
      if (data) {
#ifdef _WIN32
        _aligned_free(data);
#else
        free(data);
#endif
        data = nullptr;
        size = 0;
      }
    }
  };

 public:
  ChunkedBuffer() : total_size_(0) {}

  ~ChunkedBuffer() { clear(); }

  // Copy constructor
  ChunkedBuffer(const ChunkedBuffer& other) : total_size_(0) {
    if (other.total_size_ > 0) {
      resize(other.total_size_);
      // Copy chunk by chunk
      for (size_t i = 0; i < chunks_.size() && i < other.chunks_.size(); ++i) {
        std::memcpy(chunks_[i].data, other.chunks_[i].data,
                    other.chunks_[i].size);
      }
    }
  }

  // Move constructor
  ChunkedBuffer(ChunkedBuffer&& other) noexcept
      : chunks_(std::move(other.chunks_)), total_size_(other.total_size_) {
    other.total_size_ = 0;
  }

  // Copy assignment
  ChunkedBuffer& operator=(const ChunkedBuffer& other) {
    if (this != &other) {
      clear();
      if (other.total_size_ > 0) {
        resize(other.total_size_);
        for (size_t i = 0; i < chunks_.size() && i < other.chunks_.size();
             ++i) {
          std::memcpy(chunks_[i].data, other.chunks_[i].data,
                      other.chunks_[i].size);
        }
      }
    }
    return *this;
  }

  // Move assignment
  ChunkedBuffer& operator=(ChunkedBuffer&& other) noexcept {
    if (this != &other) {
      clear();
      chunks_ = std::move(other.chunks_);
      total_size_ = other.total_size_;
      other.total_size_ = 0;
    }
    return *this;
  }

  ///
  /// Check if buffer is empty
  ///
  bool empty() const { return total_size_ == 0; }

  ///
  /// Get current size in bytes
  ///
  size_t size() const { return total_size_; }

  ///
  /// Get chunk size
  ///
  size_t chunk_size() const { return ChunkSize; }

  ///
  /// Get number of chunks
  ///
  size_t num_chunks() const { return chunks_.size(); }

  ///
  /// Resize buffer to new_size bytes
  ///
  void resize(size_t new_size) {
    if (new_size == 0) {
      clear();
      return;
    }

    size_t required_chunks = (new_size + ChunkSize - 1) / ChunkSize;

    // Add chunks if needed
    while (chunks_.size() < required_chunks) {
      chunks_.emplace_back();
      chunks_.back().allocate();
      if (!chunks_.back().data) {
        // Allocation failed
        return;
      }
    }

    // Remove excess chunks if shrinking
    while (chunks_.size() > required_chunks) {
      chunks_.pop_back();
    }

    // Update chunk sizes
    for (size_t i = 0; i < chunks_.size(); ++i) {
      if (i < chunks_.size() - 1) {
        chunks_[i].size = ChunkSize;
      } else {
        // Last chunk might be partial
        size_t remainder = new_size % ChunkSize;
        chunks_[i].size = (remainder == 0) ? ChunkSize : remainder;
      }
    }

    total_size_ = new_size;
  }

  ///
  /// Clear buffer (removes all chunks)
  ///
  void clear() {
    chunks_.clear();
    total_size_ = 0;
  }

  ///
  /// Array access operator
  /// Note: This involves chunk lookup and is slower than contiguous Buffer
  ///
  uint8_t& operator[](size_t index) {
    // Find the chunk containing this index
    size_t current_pos = 0;
    for (size_t i = 0; i < chunks_.size(); ++i) {
      if (current_pos + chunks_[i].size > index) {
        // Found the chunk
        return chunks_[i].data[index - current_pos];
      }
      current_pos += chunks_[i].size;
    }
    // Should never reach here if index is valid
    return chunks_.back().data[0];  // Fallback
  }

  const uint8_t& operator[](size_t index) const {
    // Find the chunk containing this index
    size_t current_pos = 0;
    for (size_t i = 0; i < chunks_.size(); ++i) {
      if (current_pos + chunks_[i].size > index) {
        // Found the chunk
        return chunks_[i].data[index - current_pos];
      }
      current_pos += chunks_[i].size;
    }
    // Should never reach here if index is valid
    return chunks_.back().data[0];  // Fallback
  }

  ///
  /// Push back a single byte
  ///
  void push_back(uint8_t value) {
    size_t old_size = total_size_;
    resize(total_size_ + 1);
    (*this)[old_size] = value;
  }

  ///
  /// Get pointer to a specific chunk
  ///
  uint8_t* get_chunk(size_t chunk_idx) {
    if (chunk_idx < chunks_.size()) {
      return chunks_[chunk_idx].data;
    }
    return nullptr;
  }

  const uint8_t* get_chunk(size_t chunk_idx) const {
    if (chunk_idx < chunks_.size()) {
      return chunks_[chunk_idx].data;
    }
    return nullptr;
  }

  ///
  /// Get size of a specific chunk
  ///
  size_t get_chunk_size(size_t chunk_idx) const {
    if (chunk_idx < chunks_.size()) {
      return chunks_[chunk_idx].size;
    }
    return 0;
  }

  ///
  /// Copy data to a contiguous buffer (for compatibility)
  ///
  template <size_t TargetAlignment = 16>
  Buffer<TargetAlignment> to_contiguous() const {
    Buffer<TargetAlignment> result;
    result.resize(total_size_);

    size_t offset = 0;
    for (const auto& chunk : chunks_) {
      std::memcpy(result.data() + offset, chunk.data, chunk.size);
      offset += chunk.size;
    }

    return result;
  }

  ///
  /// Concatenate another ChunkedBuffer to this one by moving chunks
  /// This is very efficient as it minimizes data copying. Only the data needed
  /// to fill a partial last chunk (if any) is copied; all other chunks are moved.
  ///
  /// @param other The buffer to concatenate (will be moved from and cleared)
  ///
  void concat(ChunkedBuffer&& other) {
    if (other.empty()) {
      return;
    }

    // Check if our last chunk is partial and can be filled
    if (!chunks_.empty()) {
      size_t last_chunk_idx = chunks_.size() - 1;
      size_t last_chunk_free = ChunkSize - chunks_[last_chunk_idx].size;

      if (last_chunk_free > 0 && !other.chunks_.empty()) {
        // Fill our last chunk with data from other's first chunk
        size_t to_copy =
            std::min(last_chunk_free, other.chunks_[0].size);

        std::memcpy(chunks_[last_chunk_idx].data +
                        chunks_[last_chunk_idx].size,
                    other.chunks_[0].data, to_copy);

        chunks_[last_chunk_idx].size += to_copy;
        total_size_ += to_copy;

        // If we consumed all of other's first chunk, remove it
        if (to_copy == other.chunks_[0].size) {
          other.chunks_.erase(other.chunks_.begin());
          other.total_size_ -= to_copy;
        } else {
          // Partial consumption - shift remaining data in first chunk
          std::memmove(other.chunks_[0].data,
                       other.chunks_[0].data + to_copy,
                       other.chunks_[0].size - to_copy);
          other.chunks_[0].size -= to_copy;
          other.total_size_ -= to_copy;
        }
      }
    }

    // Move all remaining chunks from other to this buffer
    for (auto& chunk : other.chunks_) {
      chunks_.push_back(std::move(chunk));
    }

    // Update total size
    total_size_ += other.total_size_;

    // Clear the other buffer (chunks were moved)
    other.chunks_.clear();
    other.total_size_ = 0;
  }

  ///
  /// Concatenate another ChunkedBuffer to this one (const version)
  /// This version copies chunks since we can't move from a const reference.
  /// Fills partial last chunk if possible to maintain chunk size invariant.
  ///
  /// @param other The buffer to concatenate
  ///
  void concat(const ChunkedBuffer& other) {
    if (other.empty()) {
      return;
    }

    size_t other_offset = 0;  // Track how much of other we've consumed

    // Check if our last chunk is partial and can be filled
    if (!chunks_.empty()) {
      size_t last_chunk_idx = chunks_.size() - 1;
      size_t last_chunk_free = ChunkSize - chunks_[last_chunk_idx].size;

      if (last_chunk_free > 0 && !other.chunks_.empty()) {
        // Fill our last chunk with data from other's first chunk
        size_t to_copy =
            std::min(last_chunk_free, other.chunks_[0].size);

        std::memcpy(chunks_[last_chunk_idx].data +
                        chunks_[last_chunk_idx].size,
                    other.chunks_[0].data, to_copy);

        chunks_[last_chunk_idx].size += to_copy;
        total_size_ += to_copy;
        other_offset = to_copy;
      }
    }

    // Copy remaining chunks from other
    for (size_t i = 0; i < other.chunks_.size(); ++i) {
      size_t chunk_start = 0;
      size_t chunk_len = other.chunks_[i].size;

      // Handle partial first chunk if we consumed part of it
      if (i == 0 && other_offset > 0) {
        if (other_offset >= chunk_len) {
          continue;  // Already fully consumed
        }
        chunk_start = other_offset;
        chunk_len -= other_offset;
      }

      // Allocate new chunk
      chunks_.emplace_back();
      chunks_.back().allocate();
      if (!chunks_.back().data) {
        // Allocation failed
        return;
      }

      chunks_.back().size = chunk_len;
      std::memcpy(chunks_.back().data,
                  other.chunks_[i].data + chunk_start, chunk_len);
      total_size_ += chunk_len;
    }
  }

  ///
  /// Iterator support for range-based for loops
  ///
  Iterator begin() { return Iterator(this, 0); }
  ConstIterator begin() const { return ConstIterator(this, 0); }
  Iterator end() { return Iterator(this, total_size_); }
  ConstIterator end() const { return ConstIterator(this, total_size_); }

 private:
  std::vector<Chunk> chunks_;
  size_t total_size_;
};

///
/// BufferView class for non-owning span/view access to buffer data.
/// Similar to std::span but compatible with C++14.
///
class BufferView {
 public:
  BufferView() : data_(nullptr), size_(0) {}

  BufferView(uint8_t* data, size_t size) : data_(data), size_(size) {}

  BufferView(const uint8_t* data, size_t size)
      : data_(const_cast<uint8_t*>(data)), size_(size) {}

  template <size_t Alignment>
  BufferView(Buffer<Alignment>& buffer)
      : data_(buffer.data()), size_(buffer.size()) {}

  template <size_t Alignment>
  BufferView(const Buffer<Alignment>& buffer)
      : data_(const_cast<uint8_t*>(buffer.data())), size_(buffer.size()) {}

  ///
  /// Get size in bytes
  ///
  size_t size() const { return size_; }

  ///
  /// Check if empty
  ///
  bool empty() const { return size_ == 0; }

  ///
  /// Get raw pointer to data
  ///
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }

  ///
  /// Array access operator
  ///
  uint8_t& operator[](size_t index) { return data_[index]; }
  const uint8_t& operator[](size_t index) const { return data_[index]; }

  ///
  /// Create a subview
  ///
  BufferView subview(size_t offset, size_t count) const {
    if (offset >= size_) {
      return BufferView();
    }
    size_t actual_count = (offset + count > size_) ? (size_ - offset) : count;
    return BufferView(data_ + offset, actual_count);
  }

  ///
  /// Iterator support
  ///
  uint8_t* begin() { return data_; }
  const uint8_t* begin() const { return data_; }
  uint8_t* end() { return data_ + size_; }
  const uint8_t* end() const { return data_ + size_; }

 private:
  uint8_t* data_;
  size_t size_;
};

}  // namespace lightusd
