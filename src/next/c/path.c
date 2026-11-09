/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * path.c -- C11 USD path string algorithms.
 *
 * The string decomposition behind next::Path (src/next/prim/path.cc): parent,
 * name, property name, prim path and element splitting, over counted buffers.
 * The C++ Path class owns the std::string and forwards here. Every function
 * writes at most `cap` bytes and sets *out_len to the full length; it returns 0
 * (with *out_len = needed) when the output does not fit, so callers can resize.
 */

#include "lightusd-core-c.h"

#include <string.h>

int lu_path_is_absolute(const char *p, size_t n) {
  return n != 0 && p != NULL && p[0] == '/';
}

int lu_path_is_root(const char *p, size_t n) {
  return n == 1 && p != NULL && p[0] == '/';
}

int lu_path_has_property(const char *p, size_t n) {
  if (p == NULL) return 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '.') return 1;
  }
  return 0;
}

/* First '.' or n. */
static size_t first_dot(const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '.') return i;
  }
  return n;
}

/* Last '/' in [0,len) or (size_t)-1. */
static size_t last_slash(const char *p, size_t len) {
  for (size_t i = len; i > 0; i--) {
    if (p[i - 1] == '/') return i - 1;
  }
  return (size_t)-1;
}

static int emit(const char *src, size_t len, char *out, size_t cap,
                size_t *out_len) {
  if (out_len != NULL) *out_len = len;
  if (out == NULL || cap < len) return 0;
  if (len != 0) memcpy(out, src, len);
  return 1;
}

int lu_path_parent(const char *p, size_t n, char *out, size_t cap,
                   size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (p == NULL || n == 0 || lu_path_is_root(p, n)) return 0;

  const size_t dot = first_dot(p, n);
  if (dot < n) {
    /* Property paths descend from their owning prim: /A/B.prop -> /A/B. */
    if (dot == 0) return 0;
    return emit(p, dot, out, cap, out_len);
  }
  const size_t slash = last_slash(p, n);
  if (slash == (size_t)-1) return 0; /* no parent */
  if (slash == 0) return emit("/", 1, out, cap, out_len);
  return emit(p, slash, out, cap, out_len);
}

int lu_path_name(const char *p, size_t n, char *out, size_t cap,
                 size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (p == NULL || n == 0) return emit("", 0, out, cap, out_len);
  const size_t prim_len = first_dot(p, n);
  const size_t slash = last_slash(p, prim_len);
  if (slash == (size_t)-1) return emit(p, prim_len, out, cap, out_len);
  return emit(p + slash + 1, prim_len - slash - 1, out, cap, out_len);
}

int lu_path_property_name(const char *p, size_t n, char *out, size_t cap,
                          size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (p == NULL) return emit("", 0, out, cap, out_len);
  const size_t dot = first_dot(p, n);
  if (dot == n) return emit("", 0, out, cap, out_len);
  return emit(p + dot + 1, n - dot - 1, out, cap, out_len);
}

int lu_path_prim_path(const char *p, size_t n, char *out, size_t cap,
                      size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (p == NULL) return emit("", 0, out, cap, out_len);
  return emit(p, first_dot(p, n), out, cap, out_len);
}

/* Number of '/'-separated elements in a prim path (the path must already be a
 * prim path, i.e. have no property). */
int lu_path_element_count(const char *prim, size_t n, size_t *out) {
  if (out == NULL) return 0;
  if (prim == NULL || n == 0) {
    *out = 0;
    return 1;
  }
  size_t start = (prim[0] == '/') ? 1 : 0;
  size_t count = 0;
  while (start < n) {
    size_t end = start;
    while (end < n && prim[end] != '/') ++end;
    /* Every '/' run produces a segment, empty ones included (matching the C++
     * find('/') walk), so "//" is one empty element and "/A//B" has a middle
     * empty element. A trailing '/' adds no element. */
    ++count;
    if (end >= n) break;
    start = end + 1;
  }
  *out = count;
  return 1;
}

int lu_path_element(const char *prim, size_t n, size_t index, char *out,
                    size_t cap, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (prim == NULL || n == 0) return emit("", 0, out, cap, out_len);
  size_t start = (prim[0] == '/') ? 1 : 0;
  size_t seen = 0;
  while (start < n) {
    size_t end = start;
    while (end < n && prim[end] != '/') ++end;
    if (seen == index) return emit(prim + start, end - start, out, cap, out_len);
    ++seen;
    if (end >= n) break;
    start = end + 1;
  }
  return emit("", 0, out, cap, out_len);
}

int lu_path_make_relative_to(const char *p, size_t n, const char *base,
                             size_t bn, char *out, size_t cap,
                             size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if ((p == NULL && n) || (base == NULL && bn)) return 0;
  const int p_abs = lu_path_is_absolute(p, n);
  const int base_abs = lu_path_is_absolute(base, bn);
  if (!p_abs || !base_abs) return emit(p, n, out, cap, out_len);
  if (bn > n || memcmp(p, base, bn) != 0) return emit(p, n, out, cap, out_len);
  if (n == bn) return emit(".", 1, out, cap, out_len);
  /* The base must match on a path-component boundary: "/A" is not a prefix
   * of "/AB". A base ending in '/' is already a boundary. */
  if (base[bn - 1] != '/' && p[bn] != '/') {
    return emit(p, n, out, cap, out_len);
  }
  size_t offset = bn;
  if (p[offset] == '/') offset++;
  if (offset >= n) return emit(".", 1, out, cap, out_len);
  return emit(p + offset, n - offset, out, cap, out_len);
}
