/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * crate-delta-encode.c -- C11 pxrUSD crate delta encoders.
 *
 * The uint32/int32 delta encoders and the most-common-delta selector from
 * src/next/crate/crate-format.cc. The selector has two modes exactly as the
 * C++ original: short arrays sort a copy of the deltas, long ones tally the
 * common [-32768, 32767] range in a flat table and the rest in an
 * open-addressing table (large FIELDSETS/PATHS tables have millions of distinct
 * jumps, where a node-based map is allocation-bound). The chosen delta is the
 * most frequent; ties go to the smallest delta.
 *
 * Output is byte-identical to the C++ encoders; the C++ wrappers own the
 * std::vector and forward.
 */

#include "lightusd-core-c.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* DeltaHistogram                                                      */
/* ------------------------------------------------------------------ */

#define DH_SMALL_MIN (-32768)
#define DH_SMALL_SPAN ((size_t)65536)
#define DH_EMPTY ((int64_t)(-9223372036854775807LL - 1))

typedef struct {
  int sorted_mode;
  /* sorted mode */
  int32_t *few;
  size_t nfew;
  size_t few_cap;
  /* flat + hash mode */
  uint32_t *small; /* DH_SMALL_SPAN counters */
  size_t lo;
  size_t hi;
  int64_t *large_keys;
  size_t *large_counts;
  size_t large_cap;
  size_t large_used;
  int oom;
} delta_hist;

static size_t dh_hash(int32_t d) {
  uint64_t x = (uint32_t)d;
  x *= 0x9E3779B97F4A7C15ull;
  return (size_t)(x ^ (x >> 29));
}

static void dh_insert_large(delta_hist *h, int32_t d, size_t n) {
  const size_t mask = h->large_cap - 1;
  for (size_t i = dh_hash(d) & mask;; i = (i + 1) & mask) {
    if (h->large_keys[i] == d) {
      h->large_counts[i] += n;
      return;
    }
    if (h->large_keys[i] == DH_EMPTY) {
      h->large_keys[i] = d;
      h->large_counts[i] = n;
      ++h->large_used;
      return;
    }
  }
}

static void dh_add_large(delta_hist *h, int32_t d) {
  if ((h->large_used + 1) * 2 > h->large_cap) {
    int64_t *old_keys = h->large_keys;
    size_t *old_counts = h->large_counts;
    const size_t old_cap = h->large_cap;
    const size_t new_cap = old_cap == 0 ? 1024 : old_cap * 2;
    h->large_keys = (int64_t *)malloc(new_cap * sizeof(int64_t));
    h->large_counts = (size_t *)malloc(new_cap * sizeof(size_t));
    if (h->large_keys == NULL || h->large_counts == NULL) {
      free(h->large_keys);
      free(h->large_counts);
      h->large_keys = old_keys;
      h->large_counts = old_counts;
      h->oom = 1;
      return;
    }
    for (size_t i = 0; i < new_cap; i++) h->large_keys[i] = DH_EMPTY;
    h->large_cap = new_cap;
    h->large_used = 0;
    for (size_t i = 0; i < old_cap; i++) {
      if (old_keys[i] != DH_EMPTY) {
        dh_insert_large(h, (int32_t)old_keys[i], old_counts[i]);
      }
    }
    free(old_keys);
    free(old_counts);
  }
  dh_insert_large(h, d, 1);
}

static int dh_init(delta_hist *h, size_t expected) {
  memset(h, 0, sizeof(*h));
  h->sorted_mode = expected < 2048;
  h->lo = DH_SMALL_SPAN;
  h->hi = 0;
  if (h->sorted_mode) {
    h->few_cap = expected > 16 ? expected : 16;
    h->few = (int32_t *)malloc(h->few_cap * sizeof(int32_t));
    if (h->few == NULL) return 0;
  } else {
    h->small = (uint32_t *)calloc(DH_SMALL_SPAN, sizeof(uint32_t));
    h->large_cap = 1024;
    h->large_keys = (int64_t *)malloc(h->large_cap * sizeof(int64_t));
    h->large_counts = (size_t *)malloc(h->large_cap * sizeof(size_t));
    if (h->small == NULL || h->large_keys == NULL || h->large_counts == NULL) {
      free(h->small);
      free(h->large_keys);
      free(h->large_counts);
      h->small = NULL;
      h->large_keys = NULL;
      h->large_counts = NULL;
      return 0;
    }
    for (size_t i = 0; i < h->large_cap; i++) h->large_keys[i] = DH_EMPTY;
  }
  return 1;
}

static void dh_free(delta_hist *h) {
  free(h->few);
  free(h->small);
  free(h->large_keys);
  free(h->large_counts);
  memset(h, 0, sizeof(*h));
}

static void dh_add(delta_hist *h, int32_t d) {
  if (h->sorted_mode) {
    if (h->nfew == h->few_cap) {
      const size_t cap = h->few_cap * 2;
      int32_t *grown = (int32_t *)realloc(h->few, cap * sizeof(int32_t));
      if (grown == NULL) {
        h->oom = 1;
        return;
      }
      h->few = grown;
      h->few_cap = cap;
    }
    h->few[h->nfew++] = d;
  } else if (d >= DH_SMALL_MIN && d < DH_SMALL_MIN + (int32_t)DH_SMALL_SPAN) {
    const size_t slot = (size_t)(d - DH_SMALL_MIN);
    ++h->small[slot];
    if (slot < h->lo) h->lo = slot;
    if (slot > h->hi) h->hi = slot;
  } else {
    dh_add_large(h, d);
  }
}

static int32_t dh_most_common(delta_hist *h) {
  int32_t best = 0;
  size_t best_count = 0;
  int have = 0;

  if (h->sorted_mode) {
    /* Insertion sort keeps the result identical to a stable ascending order but
     * avoids <stdlib.h> qsort's function-pointer overhead for short arrays. */
    for (size_t i = 1; i < h->nfew; i++) {
      const int32_t key = h->few[i];
      size_t j = i;
      while (j > 0 && h->few[j - 1] > key) {
        h->few[j] = h->few[j - 1];
        --j;
      }
      h->few[j] = key;
    }
    for (size_t i = 0; i < h->nfew;) {
      size_t j = i + 1;
      while (j < h->nfew && h->few[j] == h->few[i]) ++j;
      const size_t c = j - i;
      if (c != 0 && (!have || c > best_count || (c == best_count && h->few[i] < best))) {
        best = h->few[i];
        best_count = c;
        have = 1;
      }
      i = j;
    }
    return best;
  }

  for (size_t i = h->lo; i <= h->hi && i < DH_SMALL_SPAN; ++i) {
    const size_t c = h->small[i];
    if (c == 0) continue;
    const int32_t d = (int32_t)i + DH_SMALL_MIN;
    if (!have || c > best_count || (c == best_count && d < best)) {
      best = d;
      best_count = c;
      have = 1;
    }
  }
  for (size_t i = 0; i < h->large_cap; i++) {
    if (h->large_keys[i] == DH_EMPTY) continue;
    const size_t c = h->large_counts[i];
    if (c == 0) continue;
    const int32_t d = (int32_t)h->large_keys[i];
    if (!have || c > best_count || (c == best_count && d < best)) {
      best = d;
      best_count = c;
      have = 1;
    }
  }
  return best;
}

/* ------------------------------------------------------------------ */
/* Encoders                                                            */
/* ------------------------------------------------------------------ */

int lu_crate_encode_delta_u32_bound(size_t count, size_t *out_bound) {
  if (out_bound == NULL) return 0;
  size_t code_bytes = 0;
  if (!lu_crate_delta_code_bytes(count, &code_bytes)) return 0;
  /* common i32 + code table + up to one i32 per element */
  if (count > ((size_t)-1 - 4 - code_bytes) / 4) return 0;
  *out_bound = 4 + code_bytes + count * 4;
  return 1;
}

int lu_crate_encode_delta_s32_bound(size_t count, size_t *out_bound) {
  if (out_bound == NULL) return 0;
  size_t code_bytes = 0;
  if (!lu_crate_delta_code_bytes(count, &code_bytes)) return 0;
  /* common i32 + code table + up to one i64 per element */
  if (count > ((size_t)-1 - 4 - code_bytes) / 8) return 0;
  *out_bound = 4 + code_bytes + count * 8;
  return 1;
}

int lu_crate_encode_delta_u32(const uint32_t *values, size_t count, uint8_t *dst,
                              size_t dst_cap, size_t *out_size,
                              const char **out_error) {
  if (out_error != NULL) *out_error = NULL;
  if (out_size != NULL) *out_size = 0;
  if (count == 0) return 1;
  if (values == NULL || dst == NULL) {
    if (out_error != NULL) *out_error = "Invalid argument";
    return 0;
  }
  size_t bound = 0;
  if (!lu_crate_encode_delta_u32_bound(count, &bound) || dst_cap < bound) {
    if (out_error != NULL) *out_error = "Output buffer too small";
    return 0;
  }
  size_t code_bytes = 0;
  if (!lu_crate_delta_code_bytes(count, &code_bytes)) {
    if (out_error != NULL) *out_error = "Integer section size overflow";
    return 0;
  }

  int32_t *deltas = (int32_t *)malloc(count * sizeof(int32_t));
  if (deltas == NULL) {
    if (out_error != NULL) *out_error = "Out of memory";
    return 0;
  }
  int32_t prev = 0;
  for (size_t i = 0; i < count; i++) {
    deltas[i] = (int32_t)((int64_t)values[i] - (int64_t)prev);
    prev = (int32_t)values[i];
  }

  delta_hist hist;
  if (!dh_init(&hist, count)) {
    free(deltas);
    if (out_error != NULL) *out_error = "Out of memory";
    return 0;
  }
  for (size_t i = 0; i < count; i++) dh_add(&hist, deltas[i]);
  const int32_t common_delta = dh_most_common(&hist);
  if (hist.oom) {
    dh_free(&hist);
    free(deltas);
    if (out_error != NULL) *out_error = "Out of memory";
    return 0;
  }
  dh_free(&hist);

  memcpy(dst, &common_delta, sizeof(int32_t));
  const size_t codes_start = sizeof(int32_t);
  memset(dst + codes_start, 0, code_bytes);
  size_t vpos = codes_start + code_bytes;

  for (size_t i = 0; i < count; i++) {
    const int32_t d = deltas[i];
    uint8_t code;
    if (d == common_delta) {
      code = 0;
    } else if (d >= -128 && d <= 127) {
      code = 1;
      const int8_t v = (int8_t)d;
      dst[vpos++] = (uint8_t)v;
    } else if (d >= -32768 && d <= 32767) {
      code = 2;
      const int16_t v = (int16_t)d;
      memcpy(dst + vpos, &v, sizeof(int16_t));
      vpos += sizeof(int16_t);
    } else {
      code = 3;
      memcpy(dst + vpos, &d, sizeof(int32_t));
      vpos += sizeof(int32_t);
    }
    dst[codes_start + i / 4] |= (uint8_t)(code << ((i % 4) * 2));
  }

  free(deltas);
  if (out_size != NULL) *out_size = vpos;
  return 1;
}

int lu_crate_encode_delta_s32(const int32_t *values, size_t count, uint8_t *dst,
                              size_t dst_cap, size_t *out_size,
                              const char **out_error) {
  if (out_error != NULL) *out_error = NULL;
  if (out_size != NULL) *out_size = 0;
  if (count == 0) return 1;
  if (values == NULL || dst == NULL) {
    if (out_error != NULL) *out_error = "Invalid argument";
    return 0;
  }
  size_t bound = 0;
  if (!lu_crate_encode_delta_s32_bound(count, &bound) || dst_cap < bound) {
    if (out_error != NULL) *out_error = "Output buffer too small";
    return 0;
  }
  size_t code_bytes = 0;
  if (!lu_crate_delta_code_bytes(count, &code_bytes)) {
    if (out_error != NULL) *out_error = "Integer section size overflow";
    return 0;
  }

  int64_t *deltas = (int64_t *)malloc(count * sizeof(int64_t));
  if (deltas == NULL) {
    if (out_error != NULL) *out_error = "Out of memory";
    return 0;
  }
  int32_t prev = 0;
  for (size_t i = 0; i < count; i++) {
    deltas[i] = (int64_t)values[i] - (int64_t)prev;
    prev = values[i];
  }

  /* The common-delta header is int32; wider deltas use code 3. */
  delta_hist hist;
  if (!dh_init(&hist, count)) {
    free(deltas);
    if (out_error != NULL) *out_error = "Out of memory";
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    const int64_t d = deltas[i];
    if (d >= -2147483647LL - 1 && d <= 2147483647LL) {
      dh_add(&hist, (int32_t)d);
    }
  }
  const int32_t common_delta = dh_most_common(&hist);
  if (hist.oom) {
    dh_free(&hist);
    free(deltas);
    if (out_error != NULL) *out_error = "Out of memory";
    return 0;
  }
  dh_free(&hist);

  memcpy(dst, &common_delta, sizeof(int32_t));
  const size_t codes_start = sizeof(int32_t);
  memset(dst + codes_start, 0, code_bytes);
  size_t vpos = codes_start + code_bytes;

  for (size_t i = 0; i < count; i++) {
    const int64_t d = deltas[i];
    uint8_t code;
    if (d == (int64_t)common_delta) {
      code = 0;
    } else if (d >= -32768 && d <= 32767) {
      code = 1;
      const int16_t v = (int16_t)d;
      memcpy(dst + vpos, &v, sizeof(int16_t));
      vpos += sizeof(int16_t);
    } else if (d >= -2147483647LL - 1 && d <= 2147483647LL) {
      code = 2;
      const int32_t v = (int32_t)d;
      memcpy(dst + vpos, &v, sizeof(int32_t));
      vpos += sizeof(int32_t);
    } else {
      code = 3;
      memcpy(dst + vpos, &d, sizeof(int64_t));
      vpos += sizeof(int64_t);
    }
    dst[codes_start + i / 4] |= (uint8_t)(code << ((i % 4) * 2));
  }

  free(deltas);
  if (out_size != NULL) *out_size = vpos;
  return 1;
}
