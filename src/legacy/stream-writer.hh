/*
Copyright (c) 2022 - Present Syoyo Fujita.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Syoyo Fujita nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

//
// Simple stream writer for pretty printing. Can be used instead of std::stringstream for better control.
//

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>

#include "buffer-util.hh"
#include "tiny-format.hh"

namespace lightusd {

namespace {

static inline void swap2(unsigned short *val) {
  unsigned short tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[1];
  dst[1] = src[0];
}

static inline void swap4(uint32_t *val) {
  uint32_t tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap4(int *val) {
  int tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap8(uint64_t *val) {
  uint64_t tmp = (*val);
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

static inline void swap8(int64_t *val) {
  int64_t tmp = (*val);
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

} // namespace

///
/// Simple stream writer for pretty printing
///
class StreamWriter {
 public:
  explicit StreamWriter(const size_t max_length = 1024 * 1024 * 10)  // 10MB default
      : max_length_(max_length) {
    buffer_.reserve(1024);  // Initial reserve for performance
  }

  // Write string
  void write(const std::string& str) {
    if (buffer_.size() + str.size() > max_length_) {
      return;  // Silently ignore if exceeds max
    }
    buffer_ += str;
  }

  // Write C-string
  void write(const char* str) {
    if (!str) return;
    size_t len = std::strlen(str);
    if (buffer_.size() + len > max_length_) {
      return;
    }
    buffer_ += str;
  }

  // Write single char
  void write(char c) {
    if (buffer_.size() + 1 > max_length_) {
      return;
    }
    buffer_ += c;
  }

  // Write integer types
  void write(int value) {
    write(std::to_string(value));
  }

  void write(unsigned int value) {
    write(std::to_string(value));
  }

  void write(long value) {
    write(std::to_string(value));
  }

  void write(unsigned long value) {
    write(std::to_string(value));
  }

  void write(long long value) {
    write(std::to_string(value));
  }

  void write(unsigned long long value) {
    write(std::to_string(value));
  }

  // Write floating point
  void write(float value) {
    write(fmt::g(static_cast<double>(value)));
  }

  void write(double value) {
    write(fmt::g(value));
  }

  // Write boolean
  void write(bool value) {
    write(value ? "true" : "false");
  }

  // Convenience operator<<
  template<typename T>
  StreamWriter& operator<<(const T& value) {
    write(value);
    return *this;
  }

  // Get the accumulated string
  const std::string& str() const { return buffer_; }

  // Get C-string
  const char* c_str() const { return buffer_.c_str(); }

  // Clear buffer
  void clear() { buffer_.clear(); }

  // Get current size
  size_t size() const { return buffer_.size(); }

  // Check if empty
  bool empty() const { return buffer_.empty(); }

  // Reserve capacity
  void reserve(size_t capacity) {
    if (capacity <= max_length_) {
      buffer_.reserve(capacity);
    }
  }

 private:
  std::string buffer_;
  const size_t max_length_;
};

///
/// Chunked stream writer for pretty printing using ChunkedBuffer
/// More memory-efficient for very large outputs due to reduced fragmentation
///
template <size_t ChunkSize = 4096, size_t Alignment = 16>
class ChunkedStreamWriter {
 public:
  explicit ChunkedStreamWriter(const size_t max_length = 1024 * 1024 * 100)  // 100MB default
      : max_length_(max_length) {
    // ChunkedBuffer doesn't need initial reserve since it allocates on demand
  }

  // Write string
  void write(const std::string& str) {
    if (current_size_ + str.size() > max_length_) {
      return;  // Silently ignore if exceeds max
    }
    append_bytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
  }

  // Write C-string
  void write(const char* str) {
    if (!str) return;
    size_t len = std::strlen(str);
    if (current_size_ + len > max_length_) {
      return;
    }
    append_bytes(reinterpret_cast<const uint8_t*>(str), len);
  }

  // Write single char
  void write(char c) {
    if (current_size_ + 1 > max_length_) {
      return;
    }
    buffer_.push_back(static_cast<uint8_t>(c));
    current_size_++;
  }

  // Write integer types
  void write(int value) {
    write(std::to_string(value));
  }

  void write(unsigned int value) {
    write(std::to_string(value));
  }

  void write(long value) {
    write(std::to_string(value));
  }

  void write(unsigned long value) {
    write(std::to_string(value));
  }

  void write(long long value) {
    write(std::to_string(value));
  }

  void write(unsigned long long value) {
    write(std::to_string(value));
  }

  // Write floating point
  void write(float value) {
    write(fmt::g(static_cast<double>(value)));
  }

  void write(double value) {
    write(fmt::g(value));
  }

  // Write boolean
  void write(bool value) {
    write(value ? "true" : "false");
  }

  // Convenience operator<<
  template<typename T>
  ChunkedStreamWriter& operator<<(const T& value) {
    write(value);
    return *this;
  }

  // Get the accumulated data as a string (copies data)
  std::string str() const {
    std::string result;
    result.reserve(current_size_);

    for (size_t i = 0; i < buffer_.num_chunks(); ++i) {
      const uint8_t* chunk_data = buffer_.get_chunk(i);
      size_t chunk_size = buffer_.get_chunk_size(i);
      if (chunk_data && chunk_size > 0) {
        result.append(reinterpret_cast<const char*>(chunk_data), chunk_size);
      }
    }

    return result;
  }

  // Get as BufferView (zero-copy access per chunk)
  BufferView get_view() const {
    // Note: BufferView can only view contiguous memory, so this returns
    // view of first chunk. For full access, use get_chunk_view() or str()
    if (buffer_.num_chunks() > 0) {
      return BufferView(buffer_.get_chunk(0), buffer_.get_chunk_size(0));
    }
    return BufferView();
  }

  // Get view of specific chunk
  BufferView get_chunk_view(size_t chunk_idx) const {
    if (chunk_idx < buffer_.num_chunks()) {
      return BufferView(buffer_.get_chunk(chunk_idx),
                       buffer_.get_chunk_size(chunk_idx));
    }
    return BufferView();
  }

  // Get reference to underlying ChunkedBuffer
  const ChunkedBuffer<ChunkSize, Alignment>& buffer() const {
    return buffer_;
  }

  // Convert to contiguous buffer
  Buffer<Alignment> to_contiguous() const {
    return buffer_.template to_contiguous<Alignment>();
  }

  ///
  /// Concatenate another ChunkedStreamWriter to this one by moving chunks
  /// This is very efficient as it just moves chunk pointers without any
  /// data copying or reallocation.
  ///
  /// @param other The writer to concatenate (will be moved from and cleared)
  ///
  void concat(ChunkedStreamWriter&& other) {
    if (other.empty()) {
      return;
    }

    if (current_size_ + other.current_size_ > max_length_) {
      // Would exceed max length, don't concat
      return;
    }

    // Move chunks from other buffer to this buffer
    buffer_.concat(std::move(other.buffer_));

    // Update size
    current_size_ += other.current_size_;

    // Other is now empty (buffer was moved)
    other.current_size_ = 0;
  }

  ///
  /// Concatenate another ChunkedStreamWriter to this one (const version)
  /// This version copies chunks since we can't move from a const reference.
  ///
  /// @param other The writer to concatenate
  ///
  void concat(const ChunkedStreamWriter& other) {
    if (other.empty()) {
      return;
    }

    if (current_size_ + other.current_size_ > max_length_) {
      // Would exceed max length, don't concat
      return;
    }

    // Copy chunks from other buffer to this buffer
    buffer_.concat(other.buffer_);

    // Update size
    current_size_ += other.current_size_;
  }

  // Clear buffer
  void clear() {
    buffer_.clear();
    current_size_ = 0;
  }

  // Get current size
  size_t size() const { return current_size_; }

  // Check if empty
  bool empty() const { return current_size_ == 0; }

  // Get number of chunks
  size_t num_chunks() const { return buffer_.num_chunks(); }

  // Get chunk size
  size_t chunk_size() const { return ChunkSize; }

 private:
  void append_bytes(const uint8_t* data, size_t len) {
    if (len == 0) return;

    size_t old_size = current_size_;
    buffer_.resize(current_size_ + len);

    // Copy bytes efficiently chunk-by-chunk
    // ChunkedBuffer stores data in non-contiguous chunks, so we need to
    // handle chunk boundaries properly for efficient copying
    size_t bytes_copied = 0;
    size_t current_index = old_size;

    while (bytes_copied < len) {
      // Find which chunk current_index is in
      size_t chunk_idx = current_index / ChunkSize;
      size_t offset_in_chunk = current_index % ChunkSize;

      // How many bytes can we copy to this chunk?
      size_t bytes_available_in_chunk = ChunkSize - offset_in_chunk;
      size_t bytes_to_copy = (std::min)(bytes_available_in_chunk, len - bytes_copied);

      // Get pointer to chunk data and copy
      uint8_t* chunk_ptr = buffer_.get_chunk(chunk_idx);
      if (chunk_ptr) {
        std::memcpy(chunk_ptr + offset_in_chunk, data + bytes_copied, bytes_to_copy);
      }

      bytes_copied += bytes_to_copy;
      current_index += bytes_to_copy;
    }

    current_size_ += len;
  }

  ChunkedBuffer<ChunkSize, Alignment> buffer_;
  size_t current_size_{0};
  const size_t max_length_;
};

} // namespace lightusd
