/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * decimal.c -- C11 decimal integer parsing for the ASCII parser.
 *
 * The pure-digit decimal helpers from src/next/parser/value-parser-numeric.hh.
 * They reject/saturate exactly as the C++ originals: NUL-terminated input, a
 * leading sign (optional '+'), and saturation (or overflow failure for the
 * checked variants) rather than undefined behaviour. The float-token coercion
 * that uses fast_float stays in C++.
 */

#include "lightusd-core-c.h"

int64_t lu_decimal_to_i64(const char *s) {
  if (s == NULL) return 0;
  int neg = 0;
  if (*s == '+' || *s == '-') {
    neg = (*s == '-');
    ++s;
  }
  const uint64_t lim =
      neg ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9') {
    const uint64_t d = (uint64_t)(*s - '0');
    if (v > (lim - d) / 10u) {
      v = lim;
      break;
    }
    v = v * 10u + d;
    ++s;
  }
  return neg ? (int64_t)(0u - v) : (int64_t)v;
}

int32_t lu_decimal_to_i32(const char *s) {
  const int64_t v = lu_decimal_to_i64(s);
  if (v > INT32_MAX) return INT32_MAX;
  if (v < INT32_MIN) return INT32_MIN;
  return (int32_t)v;
}

uint64_t lu_decimal_to_u64(const char *s) {
  if (s == NULL) return 0;
  if (*s == '+') ++s;
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9') {
    const uint64_t d = (uint64_t)(*s - '0');
    if (v > (UINT64_MAX - d) / 10u) {
      v = UINT64_MAX;
      break;
    }
    v = v * 10u + d;
    ++s;
  }
  return v;
}

uint32_t lu_decimal_to_u32(const char *s) {
  const uint64_t v = lu_decimal_to_u64(s);
  return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

int lu_decimal_to_i64_checked(const char *s, int64_t *out) {
  if (s == NULL || out == NULL) return 0;
  int neg = 0;
  if (*s == '+' || *s == '-') {
    neg = (*s == '-');
    ++s;
  }
  const uint64_t lim =
      neg ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9') {
    const uint64_t d = (uint64_t)(*s - '0');
    if (v > (lim - d) / 10u) return 0; /* would overflow */
    v = v * 10u + d;
    ++s;
  }
  if (neg) {
    /* -(2^63) cannot be produced by negating an int64; build it directly. */
    *out = (v == (uint64_t)INT64_MAX + 1u) ? INT64_MIN : -(int64_t)v;
  } else {
    *out = (int64_t)v;
  }
  return 1;
}

int lu_decimal_to_u64_checked(const char *s, uint64_t *out) {
  if (s == NULL || out == NULL) return 0;
  if (*s == '+') ++s;
  uint64_t v = 0;
  while (*s >= '0' && *s <= '9') {
    const uint64_t d = (uint64_t)(*s - '0');
    if (v > (UINT64_MAX - d) / 10u) return 0;
    v = v * 10u + d;
    ++s;
  }
  *out = v;
  return 1;
}

int lu_is_decimal_int_token(const char *s, size_t len, int allow_neg) {
  if (s == NULL) return 0;
  size_t i = 0;
  if (i < len && (s[i] == '+' || (allow_neg && s[i] == '-'))) ++i;
  if (i >= len) return 0;
  for (; i < len; ++i) {
    if (s[i] < '0' || s[i] > '9') return 0;
  }
  return 1;
}
