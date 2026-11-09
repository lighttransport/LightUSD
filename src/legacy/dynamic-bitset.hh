// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
#pragma once

#include <vector>
#include <cstdint>
#include <cstring>

namespace lightusd {

class DynamicBitset {
 public:
  DynamicBitset() = default;

  explicit DynamicBitset(size_t num_bits, bool init_value = false) {
    resize(num_bits, init_value);
  }

  void resize(size_t num_bits, bool init_value = false) {
    size_t old_size = _num_bits;
    _num_bits = num_bits;
    size_t num_blocks = (num_bits + 63) / 64;

    uint64_t fill_value = init_value ? ~uint64_t(0) : uint64_t(0);
    _blocks.resize(num_blocks, fill_value);

    if (init_value && num_bits > 0) {
      size_t last_block_bits = num_bits % 64;
      if (last_block_bits != 0) {
        _blocks.back() &= ((uint64_t(1) << last_block_bits) - 1);
      }
    }

    if (num_bits < old_size && !init_value) {
      for (size_t i = num_bits; i < old_size && i < _blocks.size() * 64; ++i) {
        set(i, false);
      }
    }
  }

  void set(size_t index, bool value = true) {
    if (index >= _num_bits) return;
    size_t block_idx = index / 64;
    size_t bit_idx = index % 64;

    if (value) {
      _blocks[block_idx] |= (uint64_t(1) << bit_idx);
    } else {
      _blocks[block_idx] &= ~(uint64_t(1) << bit_idx);
    }
  }

  bool get(size_t index) const {
    if (index >= _num_bits) return false;
    size_t block_idx = index / 64;
    size_t bit_idx = index % 64;
    return (_blocks[block_idx] & (uint64_t(1) << bit_idx)) != 0;
  }

  bool operator[](size_t index) const {
    return get(index);
  }

  void clear() {
    _blocks.clear();
    _num_bits = 0;
  }

  size_t size() const {
    return _num_bits;
  }

  bool empty() const {
    return _num_bits == 0;
  }

  void reserve(size_t num_bits) {
    size_t num_blocks = (num_bits + 63) / 64;
    _blocks.reserve(num_blocks);
  }

  size_t memory_usage() const {
    return sizeof(DynamicBitset) + _blocks.capacity() * sizeof(uint64_t);
  }

  void set_all(bool value = true) {
    uint64_t fill_value = value ? ~uint64_t(0) : uint64_t(0);
    for (size_t i = 0; i < _blocks.size(); ++i) {
      _blocks[i] = fill_value;
    }

    if (value && _num_bits > 0) {
      size_t last_block_bits = _num_bits % 64;
      if (last_block_bits != 0 && !_blocks.empty()) {
        _blocks.back() &= ((uint64_t(1) << last_block_bits) - 1);
      }
    }
  }

  void flip(size_t index) {
    if (index >= _num_bits) return;
    size_t block_idx = index / 64;
    size_t bit_idx = index % 64;
    _blocks[block_idx] ^= (uint64_t(1) << bit_idx);
  }

  size_t count() const {
    size_t total = 0;
    for (size_t i = 0; i < _blocks.size(); ++i) {
      total += popcount(_blocks[i]);
    }

    if (_num_bits > 0) {
      size_t last_block_bits = _num_bits % 64;
      if (last_block_bits != 0 && !_blocks.empty()) {
        uint64_t mask = (uint64_t(1) << last_block_bits) - 1;
        size_t last_count = popcount(_blocks.back() & mask);
        total -= popcount(_blocks.back());
        total += last_count;
      }
    }

    return total;
  }

  bool any() const {
    for (size_t i = 0; i < _blocks.size(); ++i) {
      if (_blocks[i] != 0) return true;
    }
    return false;
  }

  bool none() const {
    return !any();
  }

  bool all() const {
    if (_num_bits == 0) return true;

    for (size_t i = 0; i < _blocks.size() - 1; ++i) {
      if (_blocks[i] != ~uint64_t(0)) return false;
    }

    if (!_blocks.empty()) {
      size_t last_block_bits = _num_bits % 64;
      if (last_block_bits == 0) {
        return _blocks.back() == ~uint64_t(0);
      } else {
        uint64_t mask = (uint64_t(1) << last_block_bits) - 1;
        return (_blocks.back() & mask) == mask;
      }
    }

    return true;
  }

 private:
  static size_t popcount(uint64_t x) {
    // Use portable implementation to avoid signedness conversion issues
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (x * 0x0101010101010101ULL) >> 56;
  }

  std::vector<uint64_t> _blocks;
  size_t _num_bits{0};
};

}  // namespace lightusd
