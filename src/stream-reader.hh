/*
Copyright (c) 2019 - 2020, Syoyo Fujita.
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
// Simple byte stream reader. Consider endianness when reading 2, 4, 8 bytes data.
//

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

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
/// Simple stream reader
///
class StreamReader {
 public:
  explicit StreamReader(const uint8_t *binary, const uint64_t length,
                        const bool swap_endian)
      : binary_(binary), length_(length), swap_endian_(swap_endian), idx_(0) {
    (void)pad_;
  }

  bool seek_set(const uint64_t offset) const {
    if (offset > length_) {
      return false;
    }

    idx_ = offset;
    return true;
  }

  bool seek_from_current(const int64_t offset) const {
    if (offset >= 0) {
      const uint64_t distance = static_cast<uint64_t>(offset);
      if (distance > length_ - idx_) {
        return false;
      }
      idx_ += distance;
    } else {
      // Avoid negating INT64_MIN in signed arithmetic.
      const uint64_t distance = static_cast<uint64_t>(-(offset + 1)) + 1;
      if (distance > idx_) {
        return false;
      }
      idx_ -= distance;
    }
    return true;
  }

  // Check both stream bounds and host addressability without adding offsets.
  bool can_read(const uint64_t n) const {
    const uint64_t max_size = (std::numeric_limits<size_t>::max)();
    if (idx_ > length_ || n > length_ - idx_ || n > max_size - idx_) {
      return false;
    }
    // `idx_ <= max_size` only carries meaning where size_t is narrower than
    // uint64_t. On 64-bit hosts max_size is UINT64_MAX, so the term is
    // tautological and trips -Wtautological-type-limit-compare under
    // -Weverything. The test must be a preprocessor one: clang diagnoses
    // tautological comparisons while building the AST, before discarded
    // `if constexpr` branches are dropped, so `if constexpr` still warns.
    //
    // The `n <= max_size - idx_` term above is NOT tautological on any host --
    // it is what stops idx_ + n wrapping size_t -- so it is always evaluated.
#if SIZE_MAX < UINT64_MAX
    if (idx_ > max_size) {
      return false;
    }
#endif
    return true;
  }

  // All-or-nothing read: failure leaves the cursor and destination unchanged.
  bool read_exact(const uint64_t n, const uint64_t dst_len,
                  uint8_t *dst) const {
    if (n == 0) {
      return true;
    }
    if (!binary_ || !dst || n > dst_len || !can_read(n)) {
      return false;
    }
    memcpy(dst, binary_ + static_cast<size_t>(idx_), static_cast<size_t>(n));
    idx_ += n;
    return true;
  }

  uint64_t read(const uint64_t n, const uint64_t dst_len, uint8_t *dst) const {
    // 0-byte read is a no-op success. Return a truthy value (1) so callers
    // that check `if (!read(...))` don't treat it as failure.
    if (n == 0) {
      return 1;
    }

    if (idx_ > length_) {
      return 0;
    }

    uint64_t len = n;
    if (len > length_ - idx_) {
      len = length_ - uint64_t(idx_);
    }

    if (len > 0) {
      if (!binary_ || !dst || dst_len < len || !can_read(len)) {
        // dst does not have enough space. return 0 for a while.
        return 0;
      }

      size_t nbytes = static_cast<size_t>(len);

      memcpy(dst, &binary_[idx_], nbytes);
      idx_ += nbytes;
      return nbytes;

    } else {
      return 0;
    }
  }

  bool read1(uint8_t *ret) const {
    if (!binary_ || !ret || !can_read(1)) {
      return false;
    }

    const uint8_t val = binary_[idx_];

    (*ret) = val;
    idx_ += 1;

    return true;
  }

  bool read_bool(bool *ret) const {
    if (!binary_ || !ret || !can_read(1)) {
      return false;
    }

    const char val = static_cast<const char>(binary_[idx_]);

    (*ret) = bool(val);
    idx_ += 1;

    return true;
  }

  bool read1(char *ret) const {
    if (!binary_ || !ret || !can_read(1)) {
      return false;
    }

    const char val = static_cast<const char>(binary_[idx_]);

    (*ret) = val;
    idx_ += 1;

    return true;
  }

  bool read2(unsigned short *ret) const {
    if (!binary_ || !ret || !can_read(2)) {
      return false;
    }

    unsigned short val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap2(&val);
    }

    (*ret) = val;
    idx_ += 2;

    return true;
  }

  bool read4(uint32_t *ret) const {
    if (!binary_ || !ret || !can_read(4)) {
      return false;
    }

    uint32_t val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap4(&val);
    }

    (*ret) = val;
    idx_ += 4;

    return true;
  }

  bool read4(int *ret) const {
    if (!binary_ || !ret || !can_read(4)) {
      return false;
    }

    int val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap4(&val);
    }

    (*ret) = val;
    idx_ += 4;

    return true;
  }

  bool read8(uint64_t *ret) const {
    if (!binary_ || !ret || !can_read(8)) {
      return false;
    }

    uint64_t val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap8(&val);
    }

    (*ret) = val;
    idx_ += 8;

    return true;
  }

  bool read8(int64_t *ret) const {
    if (!binary_ || !ret || !can_read(8)) {
      return false;
    }

    int64_t val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap8(&val);
    }

    (*ret) = val;
    idx_ += 8;

    return true;
  }

  bool read_float(float *ret) const {
    if (!ret) {
      return false;
    }

    uint32_t bits = 0;
    if (!read4(&bits)) {
      return false;
    }

    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    (*ret) = value;

    return true;
  }

  bool read_double(double *ret) const {
    if (!ret) {
      return false;
    }

    uint64_t bits = 0;
    if (!read8(&bits)) {
      return false;
    }

    double value{};
    std::memcpy(&value, &bits, sizeof(value));
    (*ret) = value;

    return true;
  }


  uint64_t tell() const { return uint64_t(idx_); }
  bool eof() const { return idx_ >= length_; }

  bool is_nullchar() const {
    if (idx_ < length_) {
      return binary_[idx_] == '\0';
    }

    // TODO: report true when eof()?
    return false;
  }

  const uint8_t *data() const { return binary_; }

  bool swap_endian() const { return swap_endian_; }

  uint64_t size() const { return length_; }

 private:
  const uint8_t *binary_;
  const uint64_t length_;
  bool swap_endian_;
  char pad_[7];
  mutable uint64_t idx_;
};

} // namespace lightusd
