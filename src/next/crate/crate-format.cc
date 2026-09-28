// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Crate Format Implementation

#include "crate-format.hh"
#include "safe-arithmetic.hh"

// Include LZ4 from existing LightUSD
#include "../../lz4/lz4.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

namespace lightusd {
namespace next {

const char* CrateTypeIdName(CrateTypeId id) {
  switch (id) {
    case CrateTypeId::Invalid: return "Invalid";
    case CrateTypeId::Bool: return "Bool";
    case CrateTypeId::UChar: return "UChar";
    case CrateTypeId::Int: return "Int";
    case CrateTypeId::UInt: return "UInt";
    case CrateTypeId::Int64: return "Int64";
    case CrateTypeId::UInt64: return "UInt64";
    case CrateTypeId::Half: return "Half";
    case CrateTypeId::Float: return "Float";
    case CrateTypeId::Double: return "Double";
    case CrateTypeId::String: return "String";
    case CrateTypeId::Token: return "Token";
    case CrateTypeId::AssetPath: return "AssetPath";
    case CrateTypeId::Matrix2d: return "Matrix2d";
    case CrateTypeId::Matrix3d: return "Matrix3d";
    case CrateTypeId::Matrix4d: return "Matrix4d";
    case CrateTypeId::Quatd: return "Quatd";
    case CrateTypeId::Quatf: return "Quatf";
    case CrateTypeId::Quath: return "Quath";
    case CrateTypeId::Vec2d: return "Vec2d";
    case CrateTypeId::Vec2f: return "Vec2f";
    case CrateTypeId::Vec2h: return "Vec2h";
    case CrateTypeId::Vec2i: return "Vec2i";
    case CrateTypeId::Vec3d: return "Vec3d";
    case CrateTypeId::Vec3f: return "Vec3f";
    case CrateTypeId::Vec3h: return "Vec3h";
    case CrateTypeId::Vec3i: return "Vec3i";
    case CrateTypeId::Vec4d: return "Vec4d";
    case CrateTypeId::Vec4f: return "Vec4f";
    case CrateTypeId::Vec4h: return "Vec4h";
    case CrateTypeId::Vec4i: return "Vec4i";
    case CrateTypeId::Dictionary: return "Dictionary";
    case CrateTypeId::TokenListOp: return "TokenListOp";
    case CrateTypeId::StringListOp: return "StringListOp";
    case CrateTypeId::PathListOp: return "PathListOp";
    case CrateTypeId::ReferenceListOp: return "ReferenceListOp";
    case CrateTypeId::IntListOp: return "IntListOp";
    case CrateTypeId::Int64ListOp: return "Int64ListOp";
    case CrateTypeId::UIntListOp: return "UIntListOp";
    case CrateTypeId::UInt64ListOp: return "UInt64ListOp";
    case CrateTypeId::PathVector: return "PathVector";
    case CrateTypeId::TokenVector: return "TokenVector";
    case CrateTypeId::Specifier: return "Specifier";
    case CrateTypeId::Permission: return "Permission";
    case CrateTypeId::Variability: return "Variability";
    case CrateTypeId::VariantSelectionMap: return "VariantSelectionMap";
    case CrateTypeId::TimeSamples: return "TimeSamples";
    case CrateTypeId::Payload: return "Payload";
    case CrateTypeId::DoubleVector: return "DoubleVector";
    case CrateTypeId::LayerOffsetVector: return "LayerOffsetVector";
    case CrateTypeId::StringVector: return "StringVector";
    case CrateTypeId::ValueBlock: return "ValueBlock";
    case CrateTypeId::Value: return "Value";
    case CrateTypeId::UnregisteredValue: return "UnregisteredValue";
    case CrateTypeId::UnregisteredValueListOp: return "UnregisteredValueListOp";
    case CrateTypeId::PayloadListOp: return "PayloadListOp";
    case CrateTypeId::TimeCode: return "TimeCode";
    case CrateTypeId::PathExpression: return "PathExpression";
    case CrateTypeId::Spline: return "Spline";
    case CrateTypeId::Relocates: return "Relocates";
  }
  return "Unknown";
}

size_t CrateTypeIdSize(CrateTypeId id) {
  switch (id) {
    case CrateTypeId::Bool: return 1;
    case CrateTypeId::UChar: return 1;
    case CrateTypeId::Int: return 4;
    case CrateTypeId::UInt: return 4;
    case CrateTypeId::Int64: return 8;
    case CrateTypeId::UInt64: return 8;
    case CrateTypeId::Half: return 2;
    case CrateTypeId::Float: return 4;
    case CrateTypeId::Double: return 8;
    case CrateTypeId::Matrix2d: return 4 * 8;   // 4 doubles
    case CrateTypeId::Matrix3d: return 9 * 8;   // 9 doubles
    case CrateTypeId::Matrix4d: return 16 * 8;  // 16 doubles
    case CrateTypeId::Quatd: return 4 * 8;      // 4 doubles
    case CrateTypeId::Quatf: return 4 * 4;      // 4 floats
    case CrateTypeId::Quath: return 4 * 2;      // 4 halfs
    case CrateTypeId::Vec2d: return 2 * 8;
    case CrateTypeId::Vec2f: return 2 * 4;
    case CrateTypeId::Vec2h: return 2 * 2;
    case CrateTypeId::Vec2i: return 2 * 4;
    case CrateTypeId::Vec3d: return 3 * 8;
    case CrateTypeId::Vec3f: return 3 * 4;
    case CrateTypeId::Vec3h: return 3 * 2;
    case CrateTypeId::Vec3i: return 3 * 4;
    case CrateTypeId::Vec4d: return 4 * 8;
    case CrateTypeId::Vec4f: return 4 * 4;
    case CrateTypeId::Vec4h: return 4 * 2;
    case CrateTypeId::Vec4i: return 4 * 4;
    case CrateTypeId::TimeCode: return 8;       // double

    // Variable-size types
    case CrateTypeId::String:
    case CrateTypeId::Token:
    case CrateTypeId::AssetPath:
    case CrateTypeId::Dictionary:
    case CrateTypeId::TimeSamples:
    default:
      return 0;
  }
}

DecompressResult DecompressLZ4(const uint8_t* src, size_t src_size,
                                size_t uncompressed_size) {
  DecompressResult result;

  if (!src || src_size == 0) {
    result.error = "Empty input";
    return result;
  }

  if (uncompressed_size == 0) {
    result.success = true;
    return result;
  }

  // Sanity check - don't allocate absurd amounts
  if (uncompressed_size > 1024 * 1024 * 1024) {  // 1GB limit
    result.error = "Uncompressed size too large";
    return result;
  }

  result.data.resize(uncompressed_size);

  int decoded = LZ4_decompress_safe(
      reinterpret_cast<const char*>(src),
      reinterpret_cast<char*>(result.data.data()),
      static_cast<int>(src_size),
      static_cast<int>(uncompressed_size));

  if (decoded < 0) {
    result.error = "LZ4 decompression failed";
    result.data.clear();
    return result;
  }

  if (static_cast<size_t>(decoded) != uncompressed_size) {
    result.error = "LZ4 decompression size mismatch";
    result.data.clear();
    return result;
  }

  result.success = true;
  return result;
}

CompressResult CompressLZ4(const uint8_t* src, size_t src_size) {
  CompressResult result;
  if (!src || src_size == 0) {
    result.success = true;
    return result;
  }

  int bound = LZ4_compressBound(static_cast<int>(src_size));
  if (bound <= 0) {
    result.error = "LZ4_compressBound failed";
    return result;
  }

  result.data.resize(static_cast<size_t>(bound));
  int compressed = LZ4_compress_default(
      reinterpret_cast<const char*>(src),
      reinterpret_cast<char*>(result.data.data()),
      static_cast<int>(src_size),
      bound);

  if (compressed <= 0) {
    result.error = "LZ4 compression failed";
    result.data.clear();
    return result;
  }

  result.data.resize(static_cast<size_t>(compressed));
  result.success = true;
  return result;
}

// ============================================================
// TfFastCompression wrappers (pxrUSD crate LZ4 format)
// ============================================================
//
// pxrUSD wraps raw LZ4 blocks with a n_chunks prefix byte:
//   n_chunks == 0: single LZ4 block follows
//   n_chunks  > 0: n_chunks blocks, each preceded by i32 chunk_size
//
// For data < 2GB, n_chunks is always 0 (single block).

/// Maximum input size for a single LZ4 chunk (~2GB - 32MB safety margin)
static constexpr size_t kMaxLZ4ChunkSize = 0x7E000000;

CompressResult CompressCrateBlob(const uint8_t* src, size_t src_size) {
  CompressResult result;
  if (!src || src_size == 0) {
    result.success = true;
    result.data.push_back(0);  // n_chunks = 0 (empty)
    return result;
  }

  // Single chunk for data < 2GB
  size_t n_chunks = (src_size <= kMaxLZ4ChunkSize) ? 0
                    : (src_size + kMaxLZ4ChunkSize - 1) / kMaxLZ4ChunkSize;

  // Pre-allocate: 1 byte n_chunks + per-chunk overhead
  size_t total_bound = 1;
  if (n_chunks == 0) {
    int bound = LZ4_compressBound(static_cast<int>(src_size));
    if (bound <= 0) {
      result.error = "LZ4_compressBound failed";
      return result;
    }
    total_bound += static_cast<size_t>(bound);
  } else {
    for (size_t i = 0; i < n_chunks; i++) {
      size_t chunk_sz = std::min(kMaxLZ4ChunkSize, src_size - i * kMaxLZ4ChunkSize);
      int bound = LZ4_compressBound(static_cast<int>(chunk_sz));
      if (bound <= 0) {
        result.error = "LZ4_compressBound failed for chunk";
        return result;
      }
      total_bound += static_cast<size_t>(bound) + sizeof(int32_t);
    }
  }

  result.data.resize(total_bound);
  size_t out_pos = 0;

  if (n_chunks > 255) {
    result.error = "Too many chunks (>255)";
    return result;
  }
  result.data[out_pos++] = static_cast<uint8_t>(n_chunks);

  if (n_chunks == 0) {
    // Single chunk
    int compressed = LZ4_compress_default(
        reinterpret_cast<const char*>(src),
        reinterpret_cast<char*>(result.data.data() + out_pos),
        static_cast<int>(src_size),
        static_cast<int>(total_bound - out_pos));

    if (compressed <= 0) {
      result.error = "LZ4 compression failed";
      result.data.clear();
      return result;
    }
    out_pos += static_cast<size_t>(compressed);
  } else {
    // Multi-chunk
    size_t in_offset = 0;
    for (size_t ci = 0; ci < n_chunks; ci++) {
      size_t chunk_sz = std::min(kMaxLZ4ChunkSize, src_size - in_offset);
      int32_t chunk_size_i32 = static_cast<int32_t>(chunk_sz);
      std::memcpy(result.data.data() + out_pos, &chunk_size_i32, sizeof(int32_t));
      out_pos += sizeof(int32_t);

      int compressed = LZ4_compress_default(
          reinterpret_cast<const char*>(src + in_offset),
          reinterpret_cast<char*>(result.data.data() + out_pos),
          static_cast<int>(chunk_sz),
          static_cast<int>(total_bound - out_pos));

      if (compressed <= 0) {
        result.error = "LZ4 compression failed for chunk";
        result.data.clear();
        return result;
      }
      out_pos += static_cast<size_t>(compressed);
      in_offset += chunk_sz;
    }
  }

  result.data.resize(out_pos);
  result.success = true;
  return result;
}

DecompressResult DecompressCrateBlob(const uint8_t* src, size_t src_size,
                                     size_t uncompressed_size) {
  DecompressResult result;
  if (!src || src_size == 0) {
    result.success = (uncompressed_size == 0);
    return result;
  }

  if (src_size < 1) {
    result.error = "Crate blob too small (missing n_chunks)";
    return result;
  }

  uint8_t n_chunks = src[0];
  const uint8_t* data_start = src + 1;
  size_t data_size = src_size - 1;

  if (uncompressed_size == 0) {
    result.success = true;
    return result;
  }

  if (uncompressed_size > 1024 * 1024 * 1024) {
    result.error = "Uncompressed size too large";
    return result;
  }

  // LZ4 takes srcSize as `int`. A blob larger than INT_MAX would pass a
  // NEGATIVE srcSize, making LZ4 compute an end pointer BEHIND src and read
  // out of bounds. kMaxLZ4ChunkSize is the same bound the compress side uses.
  if (data_size > kMaxLZ4ChunkSize) {
    result.error = "Compressed crate blob too large";
    return result;
  }

  result.data.resize(uncompressed_size);
  size_t out_pos = 0;

  if (n_chunks == 0) {
    // Single chunk
    int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data_start),
        reinterpret_cast<char*>(result.data.data()),
        static_cast<int>(data_size),
        static_cast<int>(uncompressed_size));

    if (decoded < 0) {
      result.error = "LZ4 decompression failed";
      result.data.clear();
      return result;
    }
    out_pos = static_cast<size_t>(decoded);
  } else {
    // Multi-chunk
    size_t in_pos = 0;
    for (uint8_t ci = 0; ci < n_chunks; ci++) {
      if (in_pos + sizeof(int32_t) > data_size) {
        result.error = "Truncated crate blob (chunk header)";
        result.data.clear();
        return result;
      }

      int32_t chunk_size;
      std::memcpy(&chunk_size, data_start + in_pos, sizeof(int32_t));
      in_pos += sizeof(int32_t);

      if (chunk_size <= 0 || static_cast<size_t>(chunk_size) > data_size - in_pos) {
        result.error = "Invalid chunk size in crate blob";
        result.data.clear();
        return result;
      }

      size_t remaining = uncompressed_size - out_pos;
      int decoded = LZ4_decompress_safe(
          reinterpret_cast<const char*>(data_start + in_pos),
          reinterpret_cast<char*>(result.data.data() + out_pos),
          static_cast<int>(chunk_size),
          static_cast<int>(remaining));

      if (decoded < 0) {
        result.error = "LZ4 decompression failed for chunk";
        result.data.clear();
        return result;
      }
      out_pos += static_cast<size_t>(decoded);
      in_pos += static_cast<size_t>(chunk_size);
    }
  }

  if (out_pos > uncompressed_size) {
    result.error = "LZ4 decompression overflow";
    result.data.clear();
    return result;
  }

  // Trim to actual decompressed size
  result.data.resize(out_pos);
  result.success = true;
  return result;
}

DecompressResult DecompressCrateBlobWithCapacityHint(
    const uint8_t* src, size_t src_size, size_t uncompressed_size_limit,
    size_t initial_capacity) {
  DecompressResult result;
  if (!src || src_size == 0) {
    result.success = (uncompressed_size_limit == 0);
    return result;
  }

  if (src_size < 1) {
    result.error = "Crate blob too small (missing n_chunks)";
    return result;
  }

  if (uncompressed_size_limit == 0) {
    result.success = true;
    return result;
  }

  if (uncompressed_size_limit > 1024 * 1024 * 1024) {
    result.error = "Uncompressed size too large";
    return result;
  }

  const uint8_t n_chunks = src[0];
  if (n_chunks != 0) {
    return DecompressCrateBlob(src, src_size, uncompressed_size_limit);
  }

  const uint8_t* data_start = src + 1;
  const size_t data_size = src_size - 1;
  if (data_size > kMaxLZ4ChunkSize) {  // see DecompressCrateBlob
    result.error = "Compressed crate blob too large";
    return result;
  }

  // Bound the doubling retry below by what LZ4 can physically produce: its
  // maximum expansion ratio is 255:1. Without this a deliberately-corrupt blob
  // (which never decodes successfully) drives the loop all the way to the 1 GiB
  // limit, costing ~2 GiB of resize+decode work PER compressed array.
  const size_t kMaxLZ4Ratio = 255;
  size_t ratio_limit = (data_size > (std::numeric_limits<size_t>::max)() /
                                        kMaxLZ4Ratio)
                           ? (std::numeric_limits<size_t>::max)()
                           : data_size * kMaxLZ4Ratio;
  if (ratio_limit < uncompressed_size_limit) {
    uncompressed_size_limit = ratio_limit;
  }
  if (uncompressed_size_limit == 0) {
    result.success = true;
    return result;
  }

  size_t capacity =
      std::min(uncompressed_size_limit,
               (std::max)(size_t(1), initial_capacity));

  for (;;) {
    result.data.resize(capacity);
    const int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data_start),
        reinterpret_cast<char*>(result.data.data()),
        static_cast<int>(data_size), static_cast<int>(capacity));

    if (decoded >= 0) {
      result.data.resize(static_cast<size_t>(decoded));
      result.success = true;
      return result;
    }

    if (capacity >= uncompressed_size_limit) {
      result.data.clear();
      result.error = "LZ4 decompression failed";
      return result;
    }
    const size_t next_capacity =
        (capacity > uncompressed_size_limit / 2)
            ? uncompressed_size_limit
            : capacity * 2;
    capacity = (std::max)(next_capacity, capacity + size_t(1));
  }
}

// USD Integer compression encoder based on pxrUSD's Usd_IntegerCompression
std::vector<uint8_t> EncodeIntegers(const uint32_t* values, size_t count) {
  std::vector<uint8_t> result;
  if (count == 0) return result;

  // Use common=0 with 0 common bytes for simplicity.
  // This works well for small values (typical token indices 0-100).
  uint32_t common = 0;
  result.push_back(0);  // common_bytes = 0

  for (size_t i = 0; i < count;) {
    // Count run of identical values
    size_t run = 1;
    while (i + run < count && values[i + run] == values[i]) ++run;

    uint32_t value = values[i];
    uint8_t num_bytes;

    if (value == common) {
      num_bytes = 0;
    } else if (value <= 0xFF) {
      num_bytes = 1;
    } else if (value <= 0xFFFF) {
      num_bytes = 2;
    } else {
      num_bytes = 3;
    }

    // Encode in chunks of up to 64 (max run-length per code byte)
    size_t remaining = run;
    while (remaining > 0) {
      size_t chunk = (remaining > 64) ? 64 : remaining;
      uint8_t code = static_cast<uint8_t>((chunk - 1) << 2) | num_bytes;
      result.push_back(code);
      if (num_bytes > 0) {
        for (int b = 0; b < num_bytes; ++b) {
          result.push_back(static_cast<uint8_t>(value >> (b * 8)));
        }
      }
      remaining -= chunk;
    }

    i += run;
  }

  return result;
}

// USD Integer compression decoder
// Based on pxrUSD's Usd_IntegerCompression
namespace {

// Decode integers encoded with USD's integer compression
bool DecodeIntegers(const uint8_t* src, size_t src_size,
                    uint32_t* dst, size_t count) {
  if (count == 0) return true;
  if (!src || src_size == 0) return false;

  size_t src_idx = 0;
  size_t dst_idx = 0;

  // Read common prefix
  if (src_idx >= src_size) return false;
  uint32_t common = 0;
  uint8_t common_bytes = src[src_idx++];
  // The prefix is a uint32: more than 4 bytes is malformed input (and i*8
  // would be an out-of-range shift -> UB).
  if (common_bytes > 4) return false;

  for (int i = 0; i < common_bytes && src_idx < src_size; i++) {
    common |= static_cast<uint32_t>(src[src_idx++]) << (i * 8);
  }

  // Decode each integer
  while (dst_idx < count && src_idx < src_size) {
    uint8_t code = src[src_idx++];
    uint8_t num_bytes = code & 0x03;
    uint32_t value = common;

    if (num_bytes > 0) {
      uint32_t suffix = 0;
      for (int i = 0; i < num_bytes && src_idx < src_size; i++) {
        suffix |= static_cast<uint32_t>(src[src_idx++]) << (i * 8);
      }
      // Shift common and add suffix
      value = (common << (num_bytes * 8)) | suffix;
    }

    // Handle run-length encoding
    uint8_t run_length = (code >> 2) + 1;
    for (int i = 0; i < run_length && dst_idx < count; i++) {
      dst[dst_idx++] = value;
    }
  }

  return dst_idx == count;
}

bool DecodeIntegers64(const uint8_t* src, size_t src_size,
                      uint64_t* dst, size_t count) {
  if (count == 0) return true;
  if (!src || src_size == 0) return false;

  // For 64-bit, decode as pairs of 32-bit
  std::vector<uint32_t> lo(count), hi(count);

  size_t half_size = src_size / 2;
  if (!DecodeIntegers(src, half_size, lo.data(), count)) return false;
  if (!DecodeIntegers(src + half_size, src_size - half_size, hi.data(), count)) return false;

  for (size_t i = 0; i < count; i++) {
    dst[i] = (static_cast<uint64_t>(hi[i]) << 32) | lo[i];
  }

  return true;
}

}  // anonymous namespace

DecompressResult DecompressIntegers(const uint8_t* src, size_t src_size,
                                     size_t num_integers, bool is_64bit) {
  DecompressResult result;

  if (num_integers == 0) {
    result.success = true;
    return result;
  }

  if (!src || src_size == 0) {
    result.error = "Empty input";
    return result;
  }

  if (is_64bit) {
    size_t nbytes;
    if (!safe::mul(num_integers, sizeof(uint64_t), &nbytes)) {
      result.error = "Integer overflow in decompressed buffer size (64-bit)";
      return result;
    }
    result.data.resize(nbytes);
    if (!DecodeIntegers64(src, src_size,
                          reinterpret_cast<uint64_t*>(result.data.data()),
                          num_integers)) {
      result.error = "Integer decompression failed";
      result.data.clear();
      return result;
    }
  } else {
    size_t nbytes;
    if (!safe::mul(num_integers, sizeof(uint32_t), &nbytes)) {
      result.error = "Integer overflow in decompressed buffer size (32-bit)";
      return result;
    }
    result.data.resize(nbytes);
    if (!DecodeIntegers(src, src_size,
                        reinterpret_cast<uint32_t*>(result.data.data()),
                        num_integers)) {
      result.error = "Integer decompression failed";
      result.data.clear();
      return result;
    }
  }

  result.success = true;
  return result;
}

// ============================================================
// pxrUSD delta-coded integer compression
// ============================================================

namespace {

bool ComputeDeltaCodeBytes(size_t count, size_t* out) {
  if (!out ||
      count > ((std::numeric_limits<size_t>::max)() - size_t{7}) / size_t{2}) {
    return false;
  }
  *out = (count * size_t{2} + size_t{7}) / size_t{8};
  return true;
}

}  // namespace

namespace {

// Most frequent delta, ties broken toward the smallest value -- exactly what a
// std::map<int32_t, count> scan in ascending key order picks (the former
// implementation, which cost O(n log k) with a node allocation per distinct
// delta: ~5 s for the ~30M-entry FIELDSETS table of a large
// crate). Short inputs sort a copy; long ones tally deltas in
// [-32768, 32767] (the overwhelmingly common case) in a flat table, scanning
// only the touched range, and the rest in a hash map.
class DeltaHistogram {
 public:
  explicit DeltaHistogram(size_t expected) : sorted_mode_(expected < 2048) {
    if (sorted_mode_) {
      few_.reserve(expected);
    } else {
      small_.assign(kSmallSpan, 0);
    }
  }
  void add(int32_t d) {
    if (sorted_mode_) {
      few_.push_back(d);
    } else if (d >= kSmallMin &&
               d < kSmallMin + static_cast<int32_t>(kSmallSpan)) {
      const size_t slot = static_cast<size_t>(d - kSmallMin);
      ++small_[slot];
      lo_ = std::min(lo_, slot);
      hi_ = std::max(hi_, slot);
    } else {
      add_large(d);
    }
  }
  int32_t most_common() {
    int32_t best = 0;
    size_t best_count = 0;
    bool have = false;
    auto consider = [&](int32_t d, size_t c) {
      if (c == 0) return;
      if (!have || c > best_count || (c == best_count && d < best)) {
        best = d;
        best_count = c;
        have = true;
      }
    };
    if (sorted_mode_) {
      std::sort(few_.begin(), few_.end());
      for (size_t i = 0; i < few_.size();) {
        size_t j = i + 1;
        while (j < few_.size() && few_[j] == few_[i]) ++j;
        consider(few_[i], j - i);
        i = j;
      }
      return best;
    }
    for (size_t i = lo_; i <= hi_ && i < kSmallSpan; ++i) {
      consider(static_cast<int32_t>(i) + kSmallMin, small_[i]);
    }
    for (const LargeSlot& e : large_) {
      if (e.key != kEmpty) consider(static_cast<int32_t>(e.key), e.count);
    }
    return best;
  }

 private:
  static constexpr int32_t kSmallMin = -32768;
  static constexpr size_t kSmallSpan = 65536;
  bool sorted_mode_;
  std::vector<int32_t> few_;
  std::vector<uint32_t> small_;
  size_t lo_ = kSmallSpan;
  size_t hi_ = 0;

  // Open-addressing (linear probe) tally for deltas outside the flat range:
  // FIELDSETS/PATHS tables have millions of distinct large jumps, where a
  // node-based map is dominated by allocation.
  static constexpr int64_t kEmpty = INT64_MIN;
  struct LargeSlot {
    int64_t key;
    size_t count;
  };
  std::vector<LargeSlot> large_;
  size_t large_used_ = 0;
  static size_t HashDelta(int32_t d) {
    uint64_t x = static_cast<uint32_t>(d);
    x *= 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>(x ^ (x >> 29));
  }
  void add_large(int32_t d) {
    if ((large_used_ + 1) * 2 > large_.size()) {
      std::vector<LargeSlot> old;
      old.swap(large_);
      large_.assign(old.empty() ? 1024 : old.size() * 2, LargeSlot{kEmpty, 0});
      large_used_ = 0;
      for (const LargeSlot& e : old) {
        if (e.key != kEmpty) insert_large(static_cast<int32_t>(e.key), e.count);
      }
    }
    insert_large(d, 1);
  }
  void insert_large(int32_t d, size_t n) {
    const size_t mask = large_.size() - 1;
    for (size_t i = HashDelta(d) & mask;; i = (i + 1) & mask) {
      LargeSlot& e = large_[i];
      if (e.key == d) {
        e.count += n;
        return;
      }
      if (e.key == kEmpty) {
        e.key = d;
        e.count = n;
        ++large_used_;
        return;
      }
    }
  }
};

}  // namespace

std::vector<uint8_t> EncodeDeltaU32(const uint32_t* values, size_t count) {
  std::vector<uint8_t> result;
  if (count == 0) return result;
  size_t codes_bytes = 0;
  if (!values || !ComputeDeltaCodeBytes(count, &codes_bytes)) return result;

  // Compute deltas
  std::vector<int32_t> deltas(count);
  int32_t prev = 0;
  for (size_t i = 0; i < count; i++) {
    deltas[i] = static_cast<int32_t>(static_cast<int64_t>(values[i]) - static_cast<int64_t>(prev));
    prev = static_cast<int32_t>(values[i]);
  }

  // Find most common delta
  DeltaHistogram freq(count);
  for (size_t i = 0; i < count; i++) freq.add(deltas[i]);
  const int32_t common_delta = freq.most_common();

  // Write common delta
  result.resize(sizeof(int32_t));
  std::memcpy(result.data(), &common_delta, sizeof(int32_t));

  // Codes: 2 bits per value, packed LSB-first into bytes
  size_t codes_start = result.size();
  result.resize(codes_start + codes_bytes, 0);

  // Variable-length integer data appended at end
  std::vector<uint8_t> vints;

  for (size_t i = 0; i < count; i++) {
    int32_t d = deltas[i];
    uint8_t code;
    if (d == common_delta) {
      code = 0;
    } else if (d >= INT8_MIN && d <= INT8_MAX) {
      code = 1;
      int8_t v = static_cast<int8_t>(d);
      vints.push_back(*reinterpret_cast<const uint8_t*>(&v));
    } else if (d >= INT16_MIN && d <= INT16_MAX) {
      code = 2;
      int16_t v = static_cast<int16_t>(d);
      size_t pos = vints.size();
      vints.resize(pos + sizeof(int16_t));
      std::memcpy(vints.data() + pos, &v, sizeof(int16_t));
    } else {
      code = 3;
      size_t pos = vints.size();
      vints.resize(pos + sizeof(int32_t));
      std::memcpy(vints.data() + pos, &d, sizeof(int32_t));
    }

    // Pack code LSB-first: code for value[i] at bits [(i%4)*2, (i%4)*2+1]
    result[codes_start + i / 4] |= (code << ((i % 4) * 2));
  }

  // Append variable-length integers
  result.insert(result.end(), vints.begin(), vints.end());
  return result;
}

bool DecodeDeltaU32(const uint8_t* buffer, size_t buffer_size,
                    uint32_t* dst, size_t count) {
  if (count == 0) return true;
  if (!buffer || !dst || buffer_size < sizeof(int32_t)) return false;

  // Read common delta
  int32_t common_delta;
  std::memcpy(&common_delta, buffer, sizeof(int32_t));

  size_t codes_bytes = 0;
  if (!ComputeDeltaCodeBytes(count, &codes_bytes)) return false;
  size_t codes_start = sizeof(int32_t);
  size_t vints_start = 0;
  if (!safe::add(codes_start, codes_bytes, &vints_start)) return false;

  if (buffer_size < vints_start) return false;

  int32_t prev = 0;
  size_t vints_pos = vints_start;

  for (size_t i = 0; i < count; i++) {
    uint8_t code_byte = buffer[codes_start + i / 4];
    uint8_t code = (code_byte >> ((i % 4) * 2)) & 3;

    int32_t delta = 0;
    switch (code) {
      case 0:
        delta = common_delta;
        break;
      case 1: {
        if (sizeof(int8_t) > buffer_size - vints_pos) return false;
        int8_t v;
        std::memcpy(&v, buffer + vints_pos, sizeof(int8_t));
        delta = static_cast<int32_t>(v);
        vints_pos += sizeof(int8_t);
        break;
      }
      case 2: {
        if (sizeof(int16_t) > buffer_size - vints_pos) return false;
        int16_t v;
        std::memcpy(&v, buffer + vints_pos, sizeof(int16_t));
        delta = static_cast<int32_t>(v);
        vints_pos += sizeof(int16_t);
        break;
      }
      case 3: {
        if (sizeof(int32_t) > buffer_size - vints_pos) return false;
        std::memcpy(&delta, buffer + vints_pos, sizeof(int32_t));
        vints_pos += sizeof(int32_t);
        break;
      }
    }

    // Use unsigned wrapping arithmetic (int32 values stored as uint32
    // need wrapping, e.g., -2 as uint32 = 0xFFFFFFFE).
    uint32_t prev_u = static_cast<uint32_t>(prev);
    uint32_t delta_u = static_cast<uint32_t>(delta);
    dst[i] = prev_u + delta_u;
    prev = static_cast<int32_t>(dst[i]);
  }

  return true;
}

std::vector<uint8_t> EncodeDeltaS32(const int32_t* values, size_t count) {
  std::vector<uint8_t> result;
  if (count == 0) return result;
  size_t codes_bytes = 0;
  if (!values || !ComputeDeltaCodeBytes(count, &codes_bytes)) return result;

  // Compute deltas (promote to int64_t to avoid signed overflow UB)
  std::vector<int64_t> deltas(count);
  int32_t prev = 0;
  for (size_t i = 0; i < count; i++) {
    deltas[i] =
        static_cast<int64_t>(values[i]) - static_cast<int64_t>(prev);
    prev = values[i];
  }

  // The shared/common delta header is only int32; wider deltas use code 3.
  DeltaHistogram freq(count);
  for (int64_t delta : deltas) {
    if (delta >= INT32_MIN && delta <= INT32_MAX) {
      freq.add(static_cast<int32_t>(delta));
    }
  }
  const int32_t common_delta = freq.most_common();

  // Write common delta
  result.resize(sizeof(int32_t));
  std::memcpy(result.data(), &common_delta, sizeof(int32_t));

  size_t codes_start = result.size();
  result.resize(codes_start + codes_bytes, 0);

  std::vector<uint8_t> vints;

  // For i32: small = int16_t, medium = int32_t, full = int64_t
  for (size_t i = 0; i < count; i++) {
    const int64_t d = deltas[i];
    uint8_t code;
    if (d == common_delta) {
      code = 0;
    } else if (d >= INT16_MIN && d <= INT16_MAX) {
      code = 1;
      int16_t v = static_cast<int16_t>(d);
      size_t pos = vints.size();
      vints.resize(pos + sizeof(int16_t));
      std::memcpy(vints.data() + pos, &v, sizeof(int16_t));
    } else if (d >= INT32_MIN && d <= INT32_MAX) {
      code = 2;
      const int32_t v = static_cast<int32_t>(d);
      size_t pos = vints.size();
      vints.resize(pos + sizeof(int32_t));
      std::memcpy(vints.data() + pos, &v, sizeof(int32_t));
    } else {
      code = 3;
      size_t pos = vints.size();
      vints.resize(pos + sizeof(int64_t));
      std::memcpy(vints.data() + pos, &d, sizeof(int64_t));
    }

    result[codes_start + i / 4] |= (code << ((i % 4) * 2));
  }

  result.insert(result.end(), vints.begin(), vints.end());
  return result;
}

bool DecodeDeltaS32(const uint8_t* buffer, size_t buffer_size,
                    int32_t* dst, size_t count) {
  if (count == 0) return true;
  if (!buffer || !dst || buffer_size < sizeof(int32_t)) return false;

  int32_t common_delta;
  std::memcpy(&common_delta, buffer, sizeof(int32_t));

  size_t codes_bytes = 0;
  if (!ComputeDeltaCodeBytes(count, &codes_bytes)) return false;
  size_t codes_start = sizeof(int32_t);
  size_t vints_start = 0;
  if (!safe::add(codes_start, codes_bytes, &vints_start)) return false;

  if (buffer_size < vints_start) return false;

  int32_t prev = 0;
  size_t vints_pos = vints_start;

  for (size_t i = 0; i < count; i++) {
    uint8_t code_byte = buffer[codes_start + i / 4];
    uint8_t code = (code_byte >> ((i % 4) * 2)) & 3;

    int64_t delta;
    switch (code) {
      case 0:
        delta = common_delta;
        break;
      case 1: {
        if (sizeof(int16_t) > buffer_size - vints_pos) return false;
        int16_t v;
        std::memcpy(&v, buffer + vints_pos, sizeof(int16_t));
        delta = static_cast<int32_t>(v);
        vints_pos += sizeof(int16_t);
        break;
      }
      case 2: {
        if (sizeof(int32_t) > buffer_size - vints_pos) return false;
        int32_t v;
        std::memcpy(&v, buffer + vints_pos, sizeof(int32_t));
        delta = v;
        vints_pos += sizeof(int32_t);
        break;
      }
      case 3: {
        if (sizeof(int64_t) > buffer_size - vints_pos) return false;
        int64_t v;
        std::memcpy(&v, buffer + vints_pos, sizeof(int64_t));
        delta = v;
        vints_pos += sizeof(int64_t);
        break;
      }
      default:
        return false;
    }

    const int64_t min_delta =
        static_cast<int64_t>(INT32_MIN) - static_cast<int64_t>(prev);
    const int64_t max_delta =
        static_cast<int64_t>(INT32_MAX) - static_cast<int64_t>(prev);
    if (delta < min_delta || delta > max_delta) return false;
    const int64_t acc = static_cast<int64_t>(prev) + delta;
    dst[i] = static_cast<int32_t>(acc);
    prev = dst[i];
  }

  return true;
}

// ============================================================
// pxrUSD compressed integer section I/O
// ============================================================

CompressResult WriteCompressedU32(const uint32_t* values, size_t count) {
  CompressResult result;

  // Delta-encode
  std::vector<uint8_t> delta_encoded = EncodeDeltaU32(values, count);

  // LZ4 compress with n_chunks prefix
  CompressResult cr = CompressCrateBlob(delta_encoded.data(), delta_encoded.size());
  if (!cr.success) {
    result.error = cr.error;
    return result;
  }

  result.data = std::move(cr.data);
  result.success = true;
  return result;
}

DecompressResult DecompressCompressedU32(const uint8_t* data, size_t data_size,
                                          uint32_t* dst, size_t count) {
  DecompressResult result;
  if (!data || data_size == 0 || count == 0) {
    result.success = true;
    return result;
  }

  if (data_size < 8) {
    result.error = "Compressed integer buffer too small for size prefix";
    return result;
  }

  // Read u64 compressed_size
  uint64_t comp_size;
  std::memcpy(&comp_size, data, 8);

  if (comp_size == 0 || comp_size > data_size - 8) {
    result.error = "Invalid compressed size in integer data";
    return result;
  }

  // Estimate max delta-encoded size.
  size_t code_bits = 0;
  size_t code_bytes = 0;
  size_t value_bytes = 0;
  size_t hint_value_bytes = 0;
  size_t initial_delta_size = 0;
  size_t max_delta_size = 0;
  if (!safe::mul(count, size_t(2), &code_bits) ||
      !safe::add(code_bits, size_t(7), &code_bits) ||
      !safe::mul(count, sizeof(int32_t), &value_bytes) ||
      !safe::mul(count, size_t(2), &hint_value_bytes) ||
      !safe::add(sizeof(int32_t), code_bits / 8, &code_bytes) ||
      !safe::add(code_bytes, hint_value_bytes, &initial_delta_size) ||
      !safe::add(code_bytes, value_bytes, &max_delta_size)) {
    result.error = "Compressed integer decoded size overflow";
    return result;
  }
  initial_delta_size = std::min(initial_delta_size, max_delta_size);

  // Decompress the n_chunks LZ4 blob
  DecompressResult dr = DecompressCrateBlobWithCapacityHint(
      data + 8, static_cast<size_t>(comp_size), max_delta_size,
      initial_delta_size);
  if (!dr.success) {
    result.error = "Failed to decompress compressed integers: " + dr.error;
    return result;
  }

  // Delta-decode
  if (!DecodeDeltaU32(dr.data.data(), dr.data.size(), dst, count)) {
    result.error = "Failed to delta-decode integers";
    return result;
  }

  result.success = true;
  return result;
}

bool DecodeDeltaU64(const uint8_t* buffer, size_t buffer_size, uint64_t* dst,
                    size_t count) {
  if (count == 0) return true;
  if (!buffer || !dst || buffer_size < sizeof(int64_t)) return false;

  // Common delta is a full 64-bit value; code widths widen relative to the
  // 32-bit variant: code 1 = int16, code 2 = int32, code 3 = int64.
  int64_t common_delta;
  std::memcpy(&common_delta, buffer, sizeof(int64_t));

  size_t codes_bytes = 0;
  if (!ComputeDeltaCodeBytes(count, &codes_bytes)) return false;
  const size_t codes_start = sizeof(int64_t);
  size_t vints_start = 0;
  if (!safe::add(codes_start, codes_bytes, &vints_start)) return false;
  if (buffer_size < vints_start) return false;

  uint64_t prev = 0;
  size_t vints_pos = vints_start;

  for (size_t i = 0; i < count; i++) {
    const uint8_t code_byte = buffer[codes_start + i / 4];
    const uint8_t code = (code_byte >> ((i % 4) * 2)) & 3;

    int64_t delta = 0;
    switch (code) {
      case 0:
        delta = common_delta;
        break;
      case 1: {
        if (sizeof(int16_t) > buffer_size - vints_pos) return false;
        int16_t v;
        std::memcpy(&v, buffer + vints_pos, sizeof(int16_t));
        delta = static_cast<int64_t>(v);
        vints_pos += sizeof(int16_t);
        break;
      }
      case 2: {
        if (sizeof(int32_t) > buffer_size - vints_pos) return false;
        int32_t v;
        std::memcpy(&v, buffer + vints_pos, sizeof(int32_t));
        delta = static_cast<int64_t>(v);
        vints_pos += sizeof(int32_t);
        break;
      }
      case 3: {
        if (sizeof(int64_t) > buffer_size - vints_pos) return false;
        std::memcpy(&delta, buffer + vints_pos, sizeof(int64_t));
        vints_pos += sizeof(int64_t);
        break;
      }
    }

    // Unsigned wrapping arithmetic (signed values stored as their two's
    // complement bit pattern).
    const uint64_t delta_u = static_cast<uint64_t>(delta);
    dst[i] = prev + delta_u;
    prev = dst[i];
  }

  return true;
}

DecompressResult DecompressCompressedU64(const uint8_t* data, size_t data_size,
                                         uint64_t* dst, size_t count) {
  DecompressResult result;
  if (!data || data_size == 0 || count == 0) {
    result.success = true;
    return result;
  }

  if (data_size < 8) {
    result.error = "Compressed integer buffer too small for size prefix";
    return result;
  }

  uint64_t comp_size;
  std::memcpy(&comp_size, data, 8);

  if (comp_size == 0 || comp_size > data_size - 8) {
    result.error = "Invalid compressed size in integer data";
    return result;
  }

  // Worst-case delta-coded size: int64 common value + 2-bit codes + up to one
  // full int64 per element.
  size_t code_bits = 0;
  size_t code_bytes = 0;
  size_t value_bytes = 0;
  size_t max_delta_size = 0;
  if (!safe::mul(count, size_t(2), &code_bits) ||
      !safe::add(code_bits, size_t(7), &code_bits) ||
      !safe::mul(count, sizeof(int64_t), &value_bytes) ||
      !safe::add(sizeof(int64_t), code_bits / 8, &code_bytes) ||
      !safe::add(code_bytes, value_bytes, &max_delta_size)) {
    result.error = "Compressed 64-bit integer decoded size overflow";
    return result;
  }

  DecompressResult dr = DecompressCrateBlob(
      data + 8, static_cast<size_t>(comp_size), max_delta_size);
  if (!dr.success) {
    result.error = "Failed to decompress compressed integers: " + dr.error;
    return result;
  }

  if (!DecodeDeltaU64(dr.data.data(), dr.data.size(), dst, count)) {
    result.error = "Failed to delta-decode 64-bit integers";
    return result;
  }

  result.success = true;
  return result;
}

}  // namespace next
}  // namespace lightusd
