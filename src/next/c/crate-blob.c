/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * crate-blob.c -- C11 LZ4 + pxrUSD crate blob framing.
 *
 * The TfFastCompression layout pxrUSD uses for crate (USDC) compressed
 * sections, and the raw LZ4 block wrappers. This is the untrusted-data
 * decompression path: every length is checked before it is used and every
 * destination is bounded by a caller-provided capacity. The C++ wrappers in
 * crate-format.cc own the std::vector and forward here, so the algorithms live
 * once and are reusable from C.
 *
 * pxrUSD blob framing:
 *   n_chunks == 0: one raw LZ4 block follows
 *   n_chunks  > 0: n_chunks blocks, each preceded by an i32 chunk size
 * Data below ~2GB always uses a single block.
 */

#include "lightusd-core-c.h"

#include <string.h>

#include "lz4/lz4.h"

/* Maximum input size for a single LZ4 chunk (~2GB - 32MB safety margin). */
#define LU_MAX_LZ4_CHUNK_SIZE ((size_t)0x7E000000)

static void set_error(const char **out_error, const char *msg) {
  if (out_error != NULL) *out_error = msg;
}

int lu_lz4_decompress_upto(const uint8_t *src, size_t src_size, uint8_t *dst,
                           size_t dst_cap, size_t *out_size,
                           const char **out_error) {
  if (out_error != NULL) *out_error = NULL;
  if (out_size != NULL) *out_size = 0;
  if (src == NULL || src_size == 0) {
    set_error(out_error, "Empty input");
    return 0;
  }
  if (dst_cap == 0) return 1;
  if (dst_cap > (size_t)1024 * 1024 * 1024) {
    set_error(out_error, "Uncompressed size too large");
    return 0;
  }
  if (dst == NULL) {
    set_error(out_error, "Empty output");
    return 0;
  }
  /* LZ4 takes srcSize as int; reject anything that would go negative. */
  if (src_size > LU_MAX_LZ4_CHUNK_SIZE) {
    set_error(out_error, "Compressed crate blob too large");
    return 0;
  }
  const int decoded = LZ4_decompress_safe((const char *)src, (char *)dst,
                                          (int)src_size, (int)dst_cap);
  if (decoded < 0) {
    set_error(out_error, "LZ4 decompression failed");
    return 0;
  }
  if (out_size != NULL) *out_size = (size_t)decoded;
  return 1;
}

int lu_lz4_decompress(const uint8_t *src, size_t src_size, uint8_t *dst,
                      size_t dst_size, size_t *out_size,
                      const char **out_error) {
  size_t decoded = 0;
  if (!lu_lz4_decompress_upto(src, src_size, dst, dst_size, &decoded,
                              out_error)) {
    return 0;
  }
  if (decoded != dst_size) {
    set_error(out_error, "LZ4 decompression size mismatch");
    return 0;
  }
  if (out_size != NULL) *out_size = dst_size;
  return 1;
}

int lu_lz4_compress_bound(size_t src_size, size_t *out_bound) {
  if (out_bound == NULL) return 0;
  if (src_size > LU_MAX_LZ4_CHUNK_SIZE) return 0;
  const int bound = LZ4_compressBound((int)src_size);
  if (bound <= 0) return 0;
  *out_bound = (size_t)bound;
  return 1;
}

int lu_lz4_compress(const uint8_t *src, size_t src_size, uint8_t *dst,
                    size_t dst_cap, size_t *out_size,
                    const char **out_error) {
  if (out_error != NULL) *out_error = NULL;
  if (out_size != NULL) *out_size = 0;
  if (src == NULL || src_size == 0) return 1; /* empty -> empty, success */
  if (dst == NULL || src_size > LU_MAX_LZ4_CHUNK_SIZE) {
    set_error(out_error, "LZ4_compressBound failed");
    return 0;
  }
  if (dst_cap > (size_t)0x7FFFFFFF) dst_cap = (size_t)0x7FFFFFFF;
  const int compressed = LZ4_compress_default((const char *)src, (char *)dst,
                                              (int)src_size, (int)dst_cap);
  if (compressed <= 0) {
    set_error(out_error, "LZ4 compression failed");
    return 0;
  }
  if (out_size != NULL) *out_size = (size_t)compressed;
  return 1;
}

/* Bound for the framed blob: the n_chunks byte plus every chunk's max size. */
int lu_crate_blob_compress_bound(size_t src_size, size_t *out_bound) {
  if (out_bound == NULL) return 0;
  size_t total = 1; /* n_chunks byte */
  if (src_size == 0) {
    *out_bound = total;
    return 1;
  }
  if (src_size <= LU_MAX_LZ4_CHUNK_SIZE) {
    int bound = LZ4_compressBound((int)src_size);
    if (bound <= 0) return 0;
    *out_bound = total + (size_t)bound;
    return 1;
  }
  const size_t n_chunks =
      src_size / LU_MAX_LZ4_CHUNK_SIZE +
      (src_size % LU_MAX_LZ4_CHUNK_SIZE != 0);
  if (n_chunks > 255) return 0;
  for (size_t i = 0; i < n_chunks; i++) {
    const size_t chunk_sz = src_size - i * LU_MAX_LZ4_CHUNK_SIZE <
                                    LU_MAX_LZ4_CHUNK_SIZE
                                ? src_size - i * LU_MAX_LZ4_CHUNK_SIZE
                                : LU_MAX_LZ4_CHUNK_SIZE;
    const int bound = LZ4_compressBound((int)chunk_sz);
    if (bound <= 0) return 0;
    if ((size_t)bound > (size_t)-1 - sizeof(int32_t) ||
        total > (size_t)-1 - (size_t)bound - sizeof(int32_t)) return 0;
    total += (size_t)bound + sizeof(int32_t);
  }
  *out_bound = total;
  return 1;
}

int lu_crate_blob_compress(const uint8_t *src, size_t src_size, uint8_t *dst,
                           size_t dst_cap, size_t *out_size,
                           const char **out_error) {
  if (out_error != NULL) *out_error = NULL;
  if (out_size != NULL) *out_size = 0;
  if (dst == NULL || dst_cap < 1) {
    set_error(out_error, "LZ4_compressBound failed");
    return 0;
  }
  if (src == NULL || src_size == 0) {
    dst[0] = 0; /* n_chunks = 0 (empty) */
    if (out_size != NULL) *out_size = 1;
    return 1;
  }

  const size_t n_chunks = (src_size <= LU_MAX_LZ4_CHUNK_SIZE)
                              ? 0
                              : src_size / LU_MAX_LZ4_CHUNK_SIZE +
                                    (src_size % LU_MAX_LZ4_CHUNK_SIZE != 0);
  if (n_chunks > 255) {
    set_error(out_error, "Too many chunks (>255)");
    return 0;
  }

  size_t out_pos = 0;
  dst[out_pos++] = (uint8_t)n_chunks;

  if (n_chunks == 0) {
    if (dst_cap - out_pos > (size_t)0x7FFFFFFF) {
      /* leave as is; LZ4 takes an int capacity */
    }
    const int cap = (dst_cap - out_pos) > (size_t)0x7FFFFFFF
                        ? 0x7FFFFFFF
                        : (int)(dst_cap - out_pos);
    const int compressed = LZ4_compress_default(
        (const char *)src, (char *)(dst + out_pos), (int)src_size, cap);
    if (compressed <= 0) {
      set_error(out_error, "LZ4 compression failed");
      return 0;
    }
    out_pos += (size_t)compressed;
  } else {
    size_t in_offset = 0;
    for (size_t ci = 0; ci < n_chunks; ci++) {
      const size_t chunk_sz = (src_size - in_offset) < LU_MAX_LZ4_CHUNK_SIZE
                                  ? (src_size - in_offset)
                                  : LU_MAX_LZ4_CHUNK_SIZE;
      if (dst_cap - out_pos < sizeof(int32_t)) {
        set_error(out_error, "LZ4 compression failed for chunk");
        return 0;
      }
      const int32_t chunk_size_i32 = (int32_t)chunk_sz;
      memcpy(dst + out_pos, &chunk_size_i32, sizeof(int32_t));
      out_pos += sizeof(int32_t);
      const size_t remaining = dst_cap - out_pos;
      const int cap = remaining > (size_t)0x7FFFFFFF ? 0x7FFFFFFF : (int)remaining;
      const int compressed = LZ4_compress_default(
          (const char *)(src + in_offset), (char *)(dst + out_pos),
          (int)chunk_sz, cap);
      if (compressed <= 0) {
        set_error(out_error, "LZ4 compression failed for chunk");
        return 0;
      }
      out_pos += (size_t)compressed;
      in_offset += chunk_sz;
    }
  }

  if (out_size != NULL) *out_size = out_pos;
  return 1;
}

int lu_crate_blob_decompress(const uint8_t *src, size_t src_size, uint8_t *dst,
                             size_t dst_size, size_t *out_size,
                             const char **out_error) {
  return lu_crate_blob_decompress_bounded(
      src, src_size, dst, dst_size, (size_t)1024 * 1024 * 1024, out_size,
      out_error);
}

int lu_crate_blob_decompress_bounded(
    const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size,
    size_t max_output_size, size_t *out_size, const char **out_error) {
  if (out_error != NULL) *out_error = NULL;
  if (out_size != NULL) *out_size = 0;
  if (src == NULL || src_size == 0) return 1; /* nothing to do */
  if (src_size < 1) {
    set_error(out_error, "Crate blob too small (missing n_chunks)");
    return 0;
  }
  if (dst_size == 0) return 1;
  if (dst_size > max_output_size) {
    set_error(out_error, "Uncompressed size too large");
    return 0;
  }
  if (dst == NULL) {
    set_error(out_error, "Empty output");
    return 0;
  }

  const uint8_t n_chunks = src[0];
  const uint8_t *data_start = src + 1;
  const size_t data_size = src_size - 1;

  /* LZ4 takes each block's size as int. A multi-block frame may be larger. */
  if (n_chunks == 0 && data_size > LU_MAX_LZ4_CHUNK_SIZE) {
    set_error(out_error, "Compressed crate blob too large");
    return 0;
  }

  size_t out_pos = 0;
  if (n_chunks == 0) {
    const int decoded = LZ4_decompress_safe((const char *)data_start,
                                            (char *)dst, (int)data_size,
                                            (int)(dst_size > (size_t)0x7FFFFFFF
                                                      ? (size_t)0x7FFFFFFF
                                                      : dst_size));
    if (decoded < 0) {
      set_error(out_error, "LZ4 decompression failed");
      return 0;
    }
    out_pos = (size_t)decoded;
  } else {
    size_t in_pos = 0;
    for (uint8_t ci = 0; ci < n_chunks; ci++) {
      if (in_pos + sizeof(int32_t) > data_size) {
        set_error(out_error, "Truncated crate blob (chunk header)");
        return 0;
      }
      int32_t chunk_size;
      memcpy(&chunk_size, data_start + in_pos, sizeof(int32_t));
      in_pos += sizeof(int32_t);
      if (chunk_size <= 0 ||
          (size_t)chunk_size > data_size - in_pos) {
        set_error(out_error, "Invalid chunk size in crate blob");
        return 0;
      }
      const size_t remaining = dst_size - out_pos;
      const int decoded = LZ4_decompress_safe(
          (const char *)(data_start + in_pos), (char *)(dst + out_pos),
          (int)chunk_size, (int)(remaining > (size_t)0x7FFFFFFF
                                     ? (size_t)0x7FFFFFFF
                                     : remaining));
      if (decoded < 0) {
        set_error(out_error, "LZ4 decompression failed for chunk");
        return 0;
      }
      out_pos += (size_t)decoded;
      in_pos += (size_t)chunk_size;
    }
  }

  if (out_pos > dst_size) {
    set_error(out_error, "LZ4 decompression overflow");
    return 0;
  }
  if (out_size != NULL) *out_size = out_pos;
  return 1;
}
