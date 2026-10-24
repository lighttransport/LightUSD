// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Crate Format Implementation

#include "crate-format.hh"
#include "../c/lightusd-core-c.h"  // lu_crate_decode_*
#include "safe-arithmetic.hh"

// Include LZ4 from existing LightUSD
#include "../../lz4/lz4.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <functional>
#include <thread>
#endif
#include <cstdint>

namespace lightusd {
namespace next {

const char* CrateTypeIdName(CrateTypeId id) {
  return lu_crate_type_name(static_cast<uint8_t>(id));
}

size_t CrateTypeIdSize(CrateTypeId id) {
  return lu_crate_type_size(static_cast<uint8_t>(id));
}

DecompressResult DecompressLZ4(const uint8_t* src, size_t src_size,
                                size_t uncompressed_size) {
  DecompressResult result;
  result.data.resize(uncompressed_size);
  const char* error = nullptr;
  size_t out_size = 0;
  if (lu_lz4_decompress(src, src_size, result.data.data(), uncompressed_size,
                        &out_size, &error)) {
    result.data.resize(out_size);
    result.success = true;
  } else {
    result.data.clear();
    result.error = error ? error : "LZ4 decompression failed";
  }
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
  size_t bound = 0;
  if (!lu_crate_blob_compress_bound(src_size, &bound)) {
    result.error = "LZ4_compressBound failed";
    return result;
  }
  result.data.resize(bound);
  const char* error = nullptr;
  size_t out_size = 0;
  if (lu_crate_blob_compress(src, src_size, result.data.data(), bound, &out_size,
                             &error)) {
    result.data.resize(out_size);
    result.success = true;
  } else {
    result.data.clear();
    result.error = error ? error : "LZ4 compression failed";
  }
  return result;
}

DecompressResult DecompressCrateBlobUpto(const uint8_t* src, size_t src_size,
                                         size_t capacity) {
  DecompressResult result;
  result.data.resize(capacity);
  const char* error = nullptr;
  size_t out_size = 0;
  if (lu_crate_blob_decompress(src, src_size, result.data.data(), capacity,
                               &out_size, &error)) {
    result.data.resize(out_size);
    result.success = true;
  } else {
    result.data.clear();
    if (error) result.error = error;
  }
  return result;
}

DecompressResult DecompressCrateBlob(const uint8_t* src, size_t src_size,
                                     size_t uncompressed_size) {
  DecompressResult result =
      DecompressCrateBlobUpto(src, src_size, uncompressed_size);
  if (result.success && result.data.size() != uncompressed_size) {
    result.success = false;
    result.data.clear();
    result.error = "LZ4 decompression size mismatch";
  }
  return result;
}

DecompressResult DecompressCrateBlobExact(const uint8_t* src, size_t src_size,
                                          size_t expected_size) {
  return DecompressCrateBlob(src, src_size, expected_size);
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
    return DecompressCrateBlobUpto(src, src_size, uncompressed_size_limit);
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
    const char* decode_error = nullptr;
    size_t decoded = 0;
    if (lu_lz4_decompress_upto(data_start, data_size, result.data.data(),
                               capacity, &decoded, &decode_error)) {
      result.data.resize(decoded);
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
  size_t bound = 0;
  if (!lu_crate_encode_integers_bound(count, &bound)) return result;
  result.resize(bound);
  size_t out_size = 0;
  const char* error = nullptr;
  if (lu_crate_encode_integers(values, count, result.data(), bound, &out_size,
                               &error)) {
    result.resize(out_size);
  } else {
    result.clear();
  }
  return result;
}

// USD Integer compression decoder
// Based on pxrUSD's Usd_IntegerCompression
namespace {

// Decode integers encoded with USD's integer compression
bool DecodeIntegers(const uint8_t* src, size_t src_size,
                    uint32_t* dst, size_t count) {
  return lu_crate_decode_integers(src, src_size, dst, count) != 0;
}

bool DecodeIntegers64(const uint8_t* src, size_t src_size,
                      uint64_t* dst, size_t count) {
  return lu_crate_decode_integers64(src, src_size, dst, count) != 0;
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

#if defined(LIGHTUSD_ENABLE_THREAD)
namespace {

// Parallel twin of lu_crate_encode_delta_u32 for large columns (the crate
// writer's FIELDS / FIELDSETS / SPECS / PATHS integer tables reach 10^8
// entries). Output is byte-identical to the serial encoder:
//  - deltas depend only on the previous value;
//  - the common delta is the most frequent one, ties to the smallest, which
//    depends only on the global counts (per-chunk histograms, merged);
//  - each chunk's value bytes are sized first, so a prefix sum gives every
//    chunk its exact write position; chunks start on multiples of 4 elements,
//    so no two chunks share a 2-bit code byte.
constexpr size_t kParallelDeltaMin = size_t(1) << 22;  // 4M elements

// Open-addressed delta -> count table for deltas outside the flat range
// (index columns have many large jumps; a node map allocates per entry).
struct WideDeltaCounts {
  static constexpr int64_t kEmpty = INT64_MIN;
  std::vector<int64_t> keys;
  std::vector<size_t> counts;
  size_t used = 0;
  static size_t Hash(int32_t d) {
    uint64_t x = static_cast<uint32_t>(d);
    x *= 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>(x ^ (x >> 29));
  }
  void add(int32_t d, size_t n) {
    if (keys.empty() || (used + 1) * 2 > keys.size()) grow();
    const size_t mask = keys.size() - 1;
    for (size_t i = Hash(d) & mask;; i = (i + 1) & mask) {
      if (keys[i] == d) { counts[i] += n; return; }
      if (keys[i] == kEmpty) { keys[i] = d; counts[i] = n; ++used; return; }
    }
  }
  void grow() {
    std::vector<int64_t> ok;
    std::vector<size_t> oc;
    ok.swap(keys);
    oc.swap(counts);
    const size_t cap = ok.empty() ? 1024 : ok.size() * 2;
    keys.assign(cap, kEmpty);
    counts.assign(cap, 0);
    used = 0;
    for (size_t i = 0; i < ok.size(); ++i)
      if (ok[i] != kEmpty) add(static_cast<int32_t>(ok[i]), oc[i]);
  }
};

bool EncodeDeltaU32Parallel(const uint32_t* values, size_t count,
                            std::vector<uint8_t>* out) {
  size_t code_bytes = 0;
  size_t bound = 0;
  if (!lu_crate_delta_code_bytes(count, &code_bytes) ||
      !lu_crate_encode_delta_u32_bound(count, &bound)) {
    return false;
  }
  unsigned hw = std::thread::hardware_concurrency();
  size_t nt = std::min<size_t>(hw ? hw : 1, 8);
  if (nt < 2) return false;
  // Chunk boundaries on multiples of 4 elements.
  std::vector<size_t> bnd(nt + 1);
  for (size_t t = 0; t <= nt; ++t) bnd[t] = ((count * t / nt) + 3) & ~size_t(3);
  bnd[0] = 0;
  bnd[nt] = count;
  for (size_t t = 1; t < nt; ++t) bnd[t] = std::min(bnd[t], count);

  auto delta_at = [values](size_t i) -> int32_t {
    const int32_t prev = i ? static_cast<int32_t>(values[i - 1]) : 0;
    return static_cast<int32_t>(static_cast<int64_t>(values[i]) -
                                static_cast<int64_t>(prev));
  };

  // Pass 1: per-chunk histograms. Deltas in [-32768, 32767] go to a flat
  // table; the rest to a per-chunk unordered map (rare in these columns).
  constexpr int32_t kSmallMin = -32768;
  constexpr size_t kSmallSpan = 65536;
  std::vector<std::vector<uint32_t>> small(nt);
  std::vector<WideDeltaCounts> large(nt);
  auto run = [nt](const std::function<void(size_t)>& fn) {
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (size_t t = 0; t < nt; ++t) pool.emplace_back(fn, t);
    for (std::thread& th : pool) th.join();
  };
  run([&](size_t t) {
    small[t].assign(kSmallSpan, 0);
    for (size_t i = bnd[t]; i < bnd[t + 1]; ++i) {
      const int32_t d = delta_at(i);
      if (d >= kSmallMin && d < kSmallMin + static_cast<int32_t>(kSmallSpan)) {
        ++small[t][static_cast<size_t>(d - kSmallMin)];
      } else {
        large[t].add(d, 1);
      }
    }
  });
  // Merge and pick: most frequent, ties to the smallest delta.
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
  for (size_t k = 0; k < kSmallSpan; ++k) {
    size_t c = 0;
    for (size_t t = 0; t < nt; ++t) c += small[t][k];
    consider(static_cast<int32_t>(k) + kSmallMin, c);
  }
  std::vector<std::vector<uint32_t>>().swap(small);
  {
    WideDeltaCounts merged;
    for (size_t t = 0; t < nt; ++t) {
      const WideDeltaCounts& w = large[t];
      for (size_t i = 0; i < w.keys.size(); ++i)
        if (w.keys[i] != WideDeltaCounts::kEmpty)
          merged.add(static_cast<int32_t>(w.keys[i]), w.counts[i]);
    }
    std::vector<WideDeltaCounts>().swap(large);
    for (size_t i = 0; i < merged.keys.size(); ++i)
      if (merged.keys[i] != WideDeltaCounts::kEmpty)
        consider(static_cast<int32_t>(merged.keys[i]), merged.counts[i]);
  }
  const int32_t common = best;

  // Pass 2: per-chunk value-byte sizes.
  auto value_bytes = [common](int32_t d) -> size_t {
    if (d == common) return 0;
    if (d >= -128 && d <= 127) return 1;
    if (d >= -32768 && d <= 32767) return 2;
    return 4;
  };
  std::vector<size_t> sizes(nt, 0);
  run([&](size_t t) {
    size_t n = 0;
    for (size_t i = bnd[t]; i < bnd[t + 1]; ++i) n += value_bytes(delta_at(i));
    sizes[t] = n;
  });
  std::vector<size_t> vstart(nt + 1, 0);
  const size_t codes_start = sizeof(int32_t);
  vstart[0] = codes_start + code_bytes;
  for (size_t t = 0; t < nt; ++t) vstart[t + 1] = vstart[t] + sizes[t];
  const size_t total = vstart[nt];
  if (total > bound) return false;

  out->assign(total, 0);
  uint8_t* dst = out->data();
  std::memcpy(dst, &common, sizeof(int32_t));
  // Pass 3: write codes and values; chunk t owns code bytes
  // [bnd[t]/4, ceil(bnd[t+1]/4)) and value bytes [vstart[t], vstart[t+1]).
  run([&](size_t t) {
    size_t vpos = vstart[t];
    for (size_t i = bnd[t]; i < bnd[t + 1]; ++i) {
      const int32_t d = delta_at(i);
      uint8_t code;
      if (d == common) {
        code = 0;
      } else if (d >= -128 && d <= 127) {
        code = 1;
        const int8_t v = static_cast<int8_t>(d);
        dst[vpos++] = static_cast<uint8_t>(v);
      } else if (d >= -32768 && d <= 32767) {
        code = 2;
        const int16_t v = static_cast<int16_t>(d);
        std::memcpy(dst + vpos, &v, sizeof(int16_t));
        vpos += sizeof(int16_t);
      } else {
        code = 3;
        std::memcpy(dst + vpos, &d, sizeof(int32_t));
        vpos += sizeof(int32_t);
      }
      dst[codes_start + i / 4] |= static_cast<uint8_t>(code << ((i % 4) * 2));
    }
  });
  return true;
}

}  // namespace
#endif

std::vector<uint8_t> EncodeDeltaU32(const uint32_t* values, size_t count) {
  std::vector<uint8_t> result;
  if (count == 0 || !values) return result;
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (count >= kParallelDeltaMin && EncodeDeltaU32Parallel(values, count, &result))
    return result;
  result.clear();
#endif
  size_t bound = 0;
  if (!lu_crate_encode_delta_u32_bound(count, &bound)) return result;
  result.resize(bound);
  size_t out_size = 0;
  const char* error = nullptr;
  if (lu_crate_encode_delta_u32(values, count, result.data(), bound, &out_size,
                                &error)) {
    result.resize(out_size);
  } else {
    result.clear();
  }
  return result;
}

bool DecodeDeltaU32(const uint8_t* buffer, size_t buffer_size,
                    uint32_t* dst, size_t count) {
  return lu_crate_decode_delta_u32(buffer, buffer_size, dst, count) != 0;
}

std::vector<uint8_t> EncodeDeltaS32(const int32_t* values, size_t count) {
  std::vector<uint8_t> result;
  if (count == 0 || !values) return result;
  size_t bound = 0;
  if (!lu_crate_encode_delta_s32_bound(count, &bound)) return result;
  result.resize(bound);
  size_t out_size = 0;
  const char* error = nullptr;
  if (lu_crate_encode_delta_s32(values, count, result.data(), bound, &out_size,
                                &error)) {
    result.resize(out_size);
  } else {
    result.clear();
  }
  return result;
}

bool DecodeDeltaS32(const uint8_t* buffer, size_t buffer_size,
                    int32_t* dst, size_t count) {
  return lu_crate_decode_delta_s32(buffer, buffer_size, dst, count) != 0;
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
  return lu_crate_decode_delta_u64(buffer, buffer_size, dst, count) != 0;
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

  DecompressResult dr = DecompressCrateBlobUpto(
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

// ---------------------------------------------------------------------------
// Inline method bodies moved out of the header so units that include it do
// not re-parse them.
// ---------------------------------------------------------------------------
std::string CrateSection::name_str() const {
    return std::string(name, strnlen(name, 15));
  }
const CrateSection* CrateTOC::find(const char* name) const {
    for (const auto& s : sections) {
      if (strncmp(s.name, name, 15) == 0) {
        return &s;
      }
    }
    return nullptr;
  }
bool CrateVersion::is_valid() const {
    // Check minimum version.
    // Compare as int: major is uint8_t, so `major < 0` is always false and
    // trips -Wtautological-unsigned-zero-compare under -Weverything. The cast
    // keeps the check meaningful if kCrateMinVersionMajor ever rises above 0.
    if (static_cast<int>(major) < kCrateMinVersionMajor) return false;
    if (major == kCrateMinVersionMajor) {
      if (minor < kCrateMinVersionMinor) return false;
      if (minor == kCrateMinVersionMinor &&
          static_cast<int>(patch) < kCrateMinVersionPatch)
        return false;
    }
    // Check maximum version
    if (major > kCrateMaxVersionMajor) return false;
    if (major == kCrateMaxVersionMajor) {
      if (minor > kCrateMaxVersionMinor) return false;
    }
    return true;
  }
std::string CrateVersion::to_string() const {
    return UIntToStr(major) + "." + UIntToStr(minor) + "." + UIntToStr(patch);
  }
bool CrateVersion::is_pre_070() const { return major == 0 && minor < 7; }
bool TokenIndex::is_valid() const { return value != 0xFFFFFFFF; }
bool StringIndex::is_valid() const { return value != 0xFFFFFFFF; }
bool PathIndex::is_valid() const { return value != 0xFFFFFFFF; }
bool FieldsetIndex::is_valid() const { return value != 0xFFFFFFFF; }

}  // namespace next
}  // namespace lightusd
