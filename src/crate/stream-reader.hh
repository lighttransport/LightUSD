// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Binary Stream Reader
// Simple binary file reading interface

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

/// Binary stream reader for USDC files
/// Wraps a memory buffer with position tracking and endian-aware reading
class StreamReader {
public:
  /// Construct from memory buffer (does not take ownership)
  StreamReader(const uint8_t* data, size_t size)
      : data_(data), size_(size), pos_(0) {}

  /// Construct from vector (does not take ownership)
  explicit StreamReader(const std::vector<uint8_t>& buffer)
      : data_(buffer.data()), size_(buffer.size()), pos_(0) {}

  // ============================================================
  // Position control
  // ============================================================

  /// Get current position
  size_t position() const { return pos_; }

  /// Get total size
  size_t size() const { return size_; }

  /// Get remaining bytes
  size_t remaining() const { return (pos_ < size_) ? (size_ - pos_) : 0; }

  /// Check if at end
  bool at_end() const { return pos_ >= size_; }

  /// Seek to absolute position
  bool seek(size_t pos) {
    if (pos > size_) return false;
    pos_ = pos;
    return true;
  }

  /// Skip bytes forward
  bool skip(size_t count) {
    // Overflow-safe: pos_ <= size_ invariant holds, so size_ - pos_ is the
    // exact remaining byte count and cannot wrap (unlike pos_ + count).
    if (count > size_ - pos_) return false;
    pos_ += count;
    return true;
  }

  /// Align position to boundary
  bool align(size_t alignment) {
    if (alignment == 0) return false;
    // Overflow-safe round-up: compute padding from the current offset and
    // reject if it would pass end-of-buffer (pos_ + pad cannot wrap this way).
    size_t rem = pos_ % alignment;
    size_t pad = rem ? (alignment - rem) : 0;
    if (pad > size_ - pos_) return false;
    return seek(pos_ + pad);
  }

  // ============================================================
  // Raw reading
  // ============================================================

  /// Read raw bytes into buffer
  bool read(void* dst, size_t count) {
    if (count > size_ - pos_) return false;  // overflow-safe (pos_ <= size_)
    if (count == 0) return true;  // memcpy with a null src/dst is UB even at 0
    std::memcpy(dst, data_ + pos_, count);
    pos_ += count;
    return true;
  }

  /// Read raw bytes into vector
  bool read(std::vector<uint8_t>& dst, size_t count) {
    // Check availability BEFORE resizing so a bogus count can't trigger a huge
    // allocation; overflow-safe form (pos_ <= size_ invariant).
    if (count > size_ - pos_) return false;
    dst.resize(count);
    if (count == 0) return true;  // dst.data() may be null for an empty vector
    std::memcpy(dst.data(), data_ + pos_, count);
    pos_ += count;
    return true;
  }

  /// Get pointer to current position (for zero-copy access)
  const uint8_t* current() const {
    return (pos_ < size_) ? (data_ + pos_) : nullptr;
  }

  /// Get pointer at absolute position
  const uint8_t* at(size_t pos) const {
    return (pos < size_) ? (data_ + pos) : nullptr;
  }

  // ============================================================
  // Typed reading (little-endian)
  // ============================================================

  bool read_u8(uint8_t& v) { return read(&v, 1); }
  bool read_i8(int8_t& v) { return read(&v, 1); }

  bool read_u16(uint16_t& v) {
    if (!read(&v, 2)) return false;
    // Assume little-endian (USDC is LE)
    return true;
  }

  bool read_i16(int16_t& v) {
    uint16_t u;
    if (!read_u16(u)) return false;
    v = static_cast<int16_t>(u);
    return true;
  }

  bool read_u32(uint32_t& v) {
    if (!read(&v, 4)) return false;
    return true;
  }

  bool read_i32(int32_t& v) {
    uint32_t u;
    if (!read_u32(u)) return false;
    v = static_cast<int32_t>(u);
    return true;
  }

  bool read_u64(uint64_t& v) {
    if (!read(&v, 8)) return false;
    return true;
  }

  bool read_i64(int64_t& v) {
    uint64_t u;
    if (!read_u64(u)) return false;
    v = static_cast<int64_t>(u);
    return true;
  }

  bool read_f32(float& v) {
    uint32_t u;
    if (!read_u32(u)) return false;
    std::memcpy(&v, &u, 4);
    return true;
  }

  bool read_f64(double& v) {
    uint64_t u;
    if (!read_u64(u)) return false;
    std::memcpy(&v, &u, 8);
    return true;
  }

  // ============================================================
  // String reading
  // ============================================================

  /// Read null-terminated string
  bool read_cstring(std::string& s) {
    s.clear();
    while (pos_ < size_) {
      char c = static_cast<char>(data_[pos_++]);
      if (c == '\0') return true;
      s += c;
    }
    return false;  // No null terminator found
  }

  /// Read fixed-length string (may not be null-terminated)
  bool read_fixed_string(std::string& s, size_t len) {
    if (len > size_ - pos_) return false;  // overflow-safe (pos_ <= size_)
    s.assign(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    // Remove trailing nulls
    while (!s.empty() && s.back() == '\0') {
      s.pop_back();
    }
    return true;
  }

  // ============================================================
  // Array reading
  // ============================================================

  /// True iff `count` elements of `elem_size` bytes are available from pos_,
  /// computed without overflow (count * elem_size can wrap otherwise).
  bool has_elements(size_t count, size_t elem_size) const {
    return elem_size != 0 && count <= (size_ - pos_) / elem_size;
  }

  /// Read array of uint32_t
  bool read_u32_array(std::vector<uint32_t>& arr, size_t count) {
    size_t bytes = 0;
    if (!array_byte_count(count, sizeof(uint32_t), &bytes)) return false;
    arr.resize(count);
    return read(arr.data(), bytes);
  }

  /// Read array of int32_t
  bool read_i32_array(std::vector<int32_t>& arr, size_t count) {
    size_t bytes = 0;
    if (!array_byte_count(count, sizeof(int32_t), &bytes)) return false;
    arr.resize(count);
    return read(arr.data(), bytes);
  }

  /// Read array of float
  bool read_f32_array(std::vector<float>& arr, size_t count) {
    size_t bytes = 0;
    if (!array_byte_count(count, sizeof(float), &bytes)) return false;
    arr.resize(count);
    return read(arr.data(), bytes);
  }

  /// Read array of double
  bool read_f64_array(std::vector<double>& arr, size_t count) {
    size_t bytes = 0;
    if (!array_byte_count(count, sizeof(double), &bytes)) return false;
    arr.resize(count);
    return read(arr.data(), bytes);
  }

private:
  bool array_byte_count(size_t count, size_t elem_size, size_t* bytes) const {
    if (!bytes || !has_elements(count, elem_size)) return false;
    *bytes = count * elem_size;
    return true;
  }

  const uint8_t* data_;
  size_t size_;
  size_t pos_;
};

}  // namespace next
}  // namespace lightusd
