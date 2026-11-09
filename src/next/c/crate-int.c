/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * crate-int.c -- C11 pxrUSD crate integer decode.
 *
 * The decode side of src/next/crate/crate-format.{hh,cc}: pxrUSD delta-coded
 * integer sections and the common-prefix integer section. This is the
 * security-critical path that turns untrusted crate bytes into integers, so it
 * lives in the C11 core where every bounds check is explicit and the C ABI can
 * reuse it. All functions are allocation-light and never over-read the input.
 *
 * Integer formats (pxrUSD):
 *   - common-prefix: u8 prefix byte count, up to 4-byte common prefix, then
 *     per-element code bytes (2 bits each: 0/1/2/3 suffix widths) with
 *     run-length in the high bits.
 *   - delta: i32/i64 common delta, packed 2-bit code table, then int8/16/32/64
 *     suffixes; values are the signed prefix sum (wrapping).
 */

#include "lightusd-core-c.h"

#include <stdlib.h>
#include <string.h>

static int add_size(size_t a, size_t b, size_t *out) {
  if (a > (size_t)-1 - b) return 0;
  *out = a + b;
  return 1;
}

int lu_crate_delta_code_bytes(size_t count, size_t *out) {
  if (out == NULL) return 0;
  if (count > (((size_t)-1) - (size_t)7) / (size_t)2) return 0;
  *out = (count * (size_t)2 + (size_t)7) / (size_t)8;
  return 1;
}

int lu_crate_decode_integers(const uint8_t *src, size_t src_size, uint32_t *dst,
                             size_t count) {
  if (count == 0) return 1;
  if (src == NULL || dst == NULL || src_size == 0) return 0;

  size_t src_idx = 0;
  size_t dst_idx = 0;

  /* Common prefix. */
  if (src_idx >= src_size) return 0;
  uint32_t common = 0;
  uint8_t common_bytes = src[src_idx++];
  /* The prefix is a uint32: more than 4 bytes is malformed (and i*8 would be an
   * out-of-range shift -> UB). */
  if (common_bytes > 4 || src_size - src_idx < common_bytes) return 0;
  for (int i = 0; i < common_bytes; i++) {
    common |= (uint32_t)src[src_idx++] << (i * 8);
  }

  while (dst_idx < count && src_idx < src_size) {
    uint8_t code = src[src_idx++];
    uint8_t num_bytes = code & 0x03u;
    uint32_t value = common;

    if (num_bytes > 0) {
      uint32_t suffix = 0;
      if (src_size - src_idx < num_bytes) return 0;
      for (int i = 0; i < num_bytes; i++) {
        suffix |= (uint32_t)src[src_idx++] << (i * 8);
      }
      value = (common << (num_bytes * 8)) | suffix;
    }

    uint8_t run_length = (uint8_t)((code >> 2) + 1);
    for (int i = 0; i < run_length && dst_idx < count; i++) {
      dst[dst_idx++] = value;
    }
  }

  return dst_idx == count;
}

int lu_crate_decode_integers64(const uint8_t *src, size_t src_size,
                               uint64_t *dst, size_t count) {
  if (count == 0) return 1;
  if (src == NULL || dst == NULL || src_size == 0) return 0;

  /* For 64-bit, decode as pairs of 32-bit halves. */
  if (count > ((size_t)-1) / sizeof(uint32_t)) return 0;
  uint32_t *lo = (uint32_t *)malloc(count * sizeof(uint32_t));
  uint32_t *hi = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (lo == NULL || hi == NULL) {
    free(lo);
    free(hi);
    return 0;
  }
  const size_t half_size = src_size / 2;
  int ok = lu_crate_decode_integers(src, half_size, lo, count) &&
           lu_crate_decode_integers(src + half_size, src_size - half_size, hi,
                                    count);
  if (ok) {
    for (size_t i = 0; i < count; i++) {
      dst[i] = ((uint64_t)hi[i] << 32) | lo[i];
    }
  }
  free(lo);
  free(hi);
  return ok;
}

int lu_crate_decode_delta_u32(const uint8_t *buffer, size_t buffer_size,
                              uint32_t *dst, size_t count) {
  if (count == 0) return 1;
  if (buffer == NULL || dst == NULL || buffer_size < sizeof(int32_t)) return 0;

  int32_t common_delta;
  memcpy(&common_delta, buffer, sizeof(int32_t));

  size_t codes_bytes = 0;
  if (!lu_crate_delta_code_bytes(count, &codes_bytes)) return 0;
  const size_t codes_start = sizeof(int32_t);
  size_t vints_start = 0;
  if (!add_size(codes_start, codes_bytes, &vints_start)) return 0;
  if (buffer_size < vints_start) return 0;

  int32_t prev = 0;
  size_t vints_pos = vints_start;

  for (size_t i = 0; i < count; i++) {
    const uint8_t code_byte = buffer[codes_start + i / 4];
    const uint8_t code = (uint8_t)((code_byte >> ((i % 4) * 2)) & 3u);

    int32_t delta = 0;
    switch (code) {
      case 0:
        delta = common_delta;
        break;
      case 1: {
        if (sizeof(int8_t) > buffer_size - vints_pos) return 0;
        int8_t v;
        memcpy(&v, buffer + vints_pos, sizeof(int8_t));
        delta = (int32_t)v;
        vints_pos += sizeof(int8_t);
        break;
      }
      case 2: {
        if (sizeof(int16_t) > buffer_size - vints_pos) return 0;
        int16_t v;
        memcpy(&v, buffer + vints_pos, sizeof(int16_t));
        delta = (int32_t)v;
        vints_pos += sizeof(int16_t);
        break;
      }
      default: {
        if (sizeof(int32_t) > buffer_size - vints_pos) return 0;
        memcpy(&delta, buffer + vints_pos, sizeof(int32_t));
        vints_pos += sizeof(int32_t);
        break;
      }
    }

    /* Unsigned wrapping arithmetic (int32 values stored as uint32 need
     * wrapping, e.g. -2 as uint32 = 0xFFFFFFFE). */
    dst[i] = (uint32_t)prev + (uint32_t)delta;
    prev = (int32_t)dst[i];
  }
  return 1;
}

int lu_crate_decode_delta_s32(const uint8_t *buffer, size_t buffer_size,
                              int32_t *dst, size_t count) {
  if (count == 0) return 1;
  if (buffer == NULL || dst == NULL || buffer_size < sizeof(int32_t)) return 0;

  int32_t common_delta;
  memcpy(&common_delta, buffer, sizeof(int32_t));

  size_t codes_bytes = 0;
  if (!lu_crate_delta_code_bytes(count, &codes_bytes)) return 0;
  const size_t codes_start = sizeof(int32_t);
  size_t vints_start = 0;
  if (!add_size(codes_start, codes_bytes, &vints_start)) return 0;
  if (buffer_size < vints_start) return 0;

  int32_t prev = 0;
  size_t vints_pos = vints_start;

  for (size_t i = 0; i < count; i++) {
    const uint8_t code_byte = buffer[codes_start + i / 4];
    const uint8_t code = (uint8_t)((code_byte >> ((i % 4) * 2)) & 3u);

    int64_t delta;
    switch (code) {
      case 0:
        delta = common_delta;
        break;
      case 1: {
        if (sizeof(int16_t) > buffer_size - vints_pos) return 0;
        int16_t v;
        memcpy(&v, buffer + vints_pos, sizeof(int16_t));
        delta = (int32_t)v;
        vints_pos += sizeof(int16_t);
        break;
      }
      case 2: {
        if (sizeof(int32_t) > buffer_size - vints_pos) return 0;
        int32_t v;
        memcpy(&v, buffer + vints_pos, sizeof(int32_t));
        delta = v;
        vints_pos += sizeof(int32_t);
        break;
      }
      default: {
        if (sizeof(int64_t) > buffer_size - vints_pos) return 0;
        int64_t v;
        memcpy(&v, buffer + vints_pos, sizeof(int64_t));
        delta = v;
        vints_pos += sizeof(int64_t);
        break;
      }
    }

    const int64_t min_delta = (int64_t)(-2147483647 - 1) - (int64_t)prev;
    const int64_t max_delta = (int64_t)2147483647 - (int64_t)prev;
    if (delta < min_delta || delta > max_delta) return 0;
    const int64_t acc = (int64_t)prev + delta;
    dst[i] = (int32_t)acc;
    prev = dst[i];
  }
  return 1;
}

int lu_crate_decode_delta_u64(const uint8_t *buffer, size_t buffer_size,
                              uint64_t *dst, size_t count) {
  if (count == 0) return 1;
  if (buffer == NULL || dst == NULL || buffer_size < sizeof(int64_t)) return 0;

  /* Common delta is a full 64-bit value; code widths widen relative to the
   * 32-bit variant: code 1 = int16, code 2 = int32, code 3 = int64. */
  int64_t common_delta;
  memcpy(&common_delta, buffer, sizeof(int64_t));

  size_t codes_bytes = 0;
  if (!lu_crate_delta_code_bytes(count, &codes_bytes)) return 0;
  const size_t codes_start = sizeof(int64_t);
  size_t vints_start = 0;
  if (!add_size(codes_start, codes_bytes, &vints_start)) return 0;
  if (buffer_size < vints_start) return 0;

  uint64_t prev = 0;
  size_t vints_pos = vints_start;

  for (size_t i = 0; i < count; i++) {
    const uint8_t code_byte = buffer[codes_start + i / 4];
    const uint8_t code = (uint8_t)((code_byte >> ((i % 4) * 2)) & 3u);

    int64_t delta = 0;
    switch (code) {
      case 0:
        delta = common_delta;
        break;
      case 1: {
        if (sizeof(int16_t) > buffer_size - vints_pos) return 0;
        int16_t v;
        memcpy(&v, buffer + vints_pos, sizeof(int16_t));
        delta = (int64_t)v;
        vints_pos += sizeof(int16_t);
        break;
      }
      case 2: {
        if (sizeof(int32_t) > buffer_size - vints_pos) return 0;
        int32_t v;
        memcpy(&v, buffer + vints_pos, sizeof(int32_t));
        delta = (int64_t)v;
        vints_pos += sizeof(int32_t);
        break;
      }
      default: {
        if (sizeof(int64_t) > buffer_size - vints_pos) return 0;
        memcpy(&delta, buffer + vints_pos, sizeof(int64_t));
        vints_pos += sizeof(int64_t);
        break;
      }
    }

    /* Unsigned wrapping arithmetic (signed values stored as two's complement). */
    dst[i] = prev + (uint64_t)delta;
    prev = dst[i];
  }
  return 1;
}

int lu_crate_encode_integers_bound(size_t count, size_t *out_bound) {
  if (out_bound == NULL) return 0;
  if (count > ((size_t)-1 - 1) / 4) return 0;
  *out_bound = 1 + count * 4; /* common byte + (code + up to 3 value bytes) each */
  return 1;
}

int lu_crate_encode_integers(const uint32_t *values, size_t count, uint8_t *dst,
                             size_t dst_cap, size_t *out_size,
                             const char **out_error) {
  if (out_error != NULL) *out_error = NULL;
  if (out_size != NULL) *out_size = 0;
  if (count == 0) return 1;
  if (values == NULL || dst == NULL) {
    if (out_error != NULL) *out_error = "Invalid argument";
    return 0;
  }
  size_t cap = 0;
  if (!lu_crate_encode_integers_bound(count, &cap)) {
    if (out_error != NULL) *out_error = "Integer section size overflow";
    return 0;
  }
  if (dst_cap < cap) {
    if (out_error != NULL) *out_error = "Output buffer too small";
    return 0;
  }

  /* Common = 0 with 0 common bytes: best for small values (token indices). */
  const uint32_t common = 0;
  size_t pos = 0;
  dst[pos++] = 0; /* common_bytes = 0 */

  for (size_t i = 0; i < count;) {
    size_t run = 1;
    while (i + run < count && values[i + run] == values[i]) ++run;

    const uint32_t value = values[i];
    uint8_t num_bytes;
    if (value == common) {
      num_bytes = 0;
    } else if (value <= 0xFFu) {
      num_bytes = 1;
    } else if (value <= 0xFFFFu) {
      num_bytes = 2;
    } else {
      num_bytes = 3;
    }

    size_t remaining = run;
    while (remaining > 0) {
      const size_t chunk = (remaining > 64) ? 64 : remaining;
      const uint8_t code = (uint8_t)(((chunk - 1) << 2) | num_bytes);
      dst[pos++] = code;
      if (num_bytes > 0) {
        for (int b = 0; b < num_bytes; ++b) {
          dst[pos++] = (uint8_t)(value >> (b * 8));
        }
      }
      remaining -= chunk;
    }
    i += run;
  }

  if (out_size != NULL) *out_size = pos;
  return 1;
}
