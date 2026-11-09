/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * strfmt.c -- C11 integer formatting.
 *
 * Locale-independent, libc-free decimal integer formatting (no std::to_string,
 * no snprintf). Output is byte-identical to `ss << v` under the classic locale:
 * plain decimal, leading '-' for negatives, no '+'/padding/grouping. This is
 * the C core of src/next/strfmt.hh; the C++ wrappers forward here so the digit
 * loop lives once and is available to C consumers.
 */

#include "lightusd-core-c.h"

size_t lu_uint_to(char *buf, uint64_t v) {
  char tmp[20];
  char *p = tmp + sizeof(tmp);
  do {
    *--p = (char)('0' + (v % 10));
    v /= 10;
  } while (v);
  const size_t n = (size_t)(tmp + sizeof(tmp) - p);
  for (size_t i = 0; i < n; ++i) buf[i] = p[i];
  return n;
}

size_t lu_int_to(char *buf, int64_t v) {
  if (v < 0) {
    buf[0] = '-';
    /* Negate in unsigned space so INT64_MIN is handled without UB. */
    return 1 + lu_uint_to(buf + 1, ~(uint64_t)v + 1u);
  }
  return lu_uint_to(buf, (uint64_t)v);
}
