/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * identifier.c -- C11 AOUSD identifier / path validation.
 *
 * UTF-8 decoding plus the AOUSD (Core Spec section 8) identifier, namespaced
 * property and textual path grammar. Pure, allocation-free, and the single
 * implementation shared by the C++ header (src/next/prim/identifier.hh), the
 * lexer and any C consumer. Character classes come from the shared XID module
 * (src/unicode-xid-c.c).
 */

#include "lightusd-core-c.h"

#include "unicode-xid-c.h"

int lu_utf8_decode(const char *data, size_t size, size_t offset,
                   uint32_t *codepoint, size_t *width) {
  if (data == NULL || codepoint == NULL || width == NULL || offset >= size) {
    return 0;
  }
  const uint8_t c0 = (uint8_t)data[offset];
  if (c0 <= 0x7f) {
    *codepoint = c0;
    *width = 1;
    return 1;
  }
  size_t n = 0;
  uint32_t cp = 0;
  uint32_t minimum = 0;
  if (c0 >= 0xc2 && c0 <= 0xdf) {
    n = 2;
    cp = c0 & 0x1fu;
    minimum = 0x80u;
  } else if (c0 >= 0xe0 && c0 <= 0xef) {
    n = 3;
    cp = c0 & 0x0fu;
    minimum = 0x800u;
  } else if (c0 >= 0xf0 && c0 <= 0xf4) {
    n = 4;
    cp = c0 & 0x07u;
    minimum = 0x10000u;
  } else {
    return 0;
  }
  if (offset + n > size) return 0;
  for (size_t i = 1; i < n; ++i) {
    const uint8_t cx = (uint8_t)data[offset + i];
    if ((cx & 0xc0u) != 0x80u) return 0;
    cp = (cp << 6) | (cx & 0x3fu);
  }
  if (cp < minimum || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
    return 0;
  }
  *codepoint = cp;
  *width = n;
  return 1;
}

int lu_is_identifier_start(uint32_t codepoint) {
  return codepoint == (uint32_t)'_' || unicode_xid_is_start(codepoint);
}

int lu_is_identifier_continue(uint32_t codepoint) {
  return unicode_xid_is_continue(codepoint);
}

/* --- internal grammar (mirrors identifier.hh's identifier_detail) --- */

static int parse_identifier(const char *data, size_t size, size_t *pos) {
  if (pos == NULL || *pos >= size) return 0;
  uint32_t cp = 0;
  size_t width = 0;
  if (!lu_utf8_decode(data, size, *pos, &cp, &width) ||
      !lu_is_identifier_start(cp)) {
    return 0;
  }
  *pos += width;
  while (*pos < size) {
    const size_t before = *pos;
    if (!lu_utf8_decode(data, size, *pos, &cp, &width) ||
        !lu_is_identifier_continue(cp)) {
      *pos = before;
      break;
    }
    *pos += width;
  }
  return 1;
}

static void skip_spaces(const char *data, size_t size, size_t *pos) {
  while (*pos < size && (data[*pos] == ' ' || data[*pos] == '\t')) ++*pos;
}

static int parse_variant_selection(const char *data, size_t size, size_t *pos) {
  if (*pos >= size || data[*pos] != '{') return 0;
  ++*pos;
  skip_spaces(data, size, pos);
  if (!parse_identifier(data, size, pos)) return 0;
  skip_spaces(data, size, pos);
  if (*pos >= size || data[*pos] != '=') return 0;
  ++*pos;
  skip_spaces(data, size, pos);
  if (*pos < size && data[*pos] != '}') {
    int saw = 0;
    if (data[*pos] == '.') {
      ++*pos;
      saw = 1;
    }
    while (*pos < size && data[*pos] != '}') {
      const char c = data[*pos];
      if (c == ' ' || c == '\t') break;
      if (c == '|' || c == '-') {
        ++*pos;
        saw = 1;
        continue;
      }
      uint32_t cp = 0;
      size_t width = 0;
      if (!lu_utf8_decode(data, size, *pos, &cp, &width) ||
          !lu_is_identifier_continue(cp)) {
        return 0;
      }
      *pos += width;
      saw = 1;
    }
    if (!saw) return 0;
  }
  skip_spaces(data, size, pos);
  if (*pos >= size || data[*pos] != '}') return 0;
  ++*pos;
  return 1;
}

static int parse_property_name(const char *data, size_t size, size_t *pos) {
  if (!parse_identifier(data, size, pos)) return 0;
  while (*pos < size && data[*pos] == ':') {
    ++*pos;
    if (!parse_identifier(data, size, pos)) return 0;
  }
  return 1;
}

static int parse_path_elements(const char *data, size_t size, size_t *pos,
                               int prim_required, int at_most_one_property) {
  int have_prim = 0;
  if (*pos < size && data[*pos] != '.') {
    if (!parse_identifier(data, size, pos)) return 0;
    have_prim = 1;
    while (*pos < size) {
      if (data[*pos] == '/') {
        ++*pos;
        if (!parse_identifier(data, size, pos)) return 0;
        continue;
      }
      if (data[*pos] == '{') {
        do {
          if (!parse_variant_selection(data, size, pos)) return 0;
        } while (*pos < size && data[*pos] == '{');
        /* AOUSD also permits a prim element immediately after a selection. */
        if (*pos < size && data[*pos] != '.') {
          const size_t save = *pos;
          if (parse_identifier(data, size, pos)) continue;
          *pos = save;
        }
      }
      break;
    }
  }
  if (prim_required && !have_prim) return 0;
  {
    int have_property = 0;
    size_t property_count = 0;
    while (*pos < size && data[*pos] == '.') {
      if (at_most_one_property && property_count != 0) return 0;
      ++*pos;
      if (!parse_property_name(data, size, pos)) return 0;
      have_property = 1;
      ++property_count;
    }
    return have_prim || have_property;
  }
}

static int memeq(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) return 0;
  }
  return 1;
}

int lu_is_valid_identifier(const char *data, size_t size) {
  size_t pos = 0;
  return parse_identifier(data, size, &pos) && pos == size;
}

int lu_is_valid_namespaced_identifier(const char *data, size_t size) {
  size_t pos = 0;
  return parse_property_name(data, size, &pos) && pos == size;
}

int lu_is_valid_path_string(const char *data, size_t size) {
  if (data == NULL || size == 0) return 0;
  size_t pos = 0;
  if ((size == 1 && data[0] == '.') || (size == 2 && data[0] == '.' && data[1] == '.') ||
      (size == 1 && data[0] == '/')) {
    return 1;
  }
  int prim_required = 0;
  if (data[0] == '/') {
    pos = 1;
    prim_required = 1;
  } else if (size >= 2 && data[0] == '.' && data[1] == '.') {
    pos = 2;
    if (pos == size) return 1;
    /* The normative examples require repeated parent traversal ("../..",
     * "../../Sibling") even though the compact PEG spells out only one "..". */
    while (pos + 3 <= size && memeq(data + pos, "/..", 3) &&
           (pos + 3 == size || data[pos + 3] == '/')) {
      pos += 3;
      if (pos == size) return 1;
    }
    if (data[pos] != '/') return 0;
    ++pos;
  }
  if (pos >= size) return 0;
  /* A property is always the terminal element (AOUSD L3449-3450, OpenUSD
   * SdfPath): the PEG's `PathElements <- PrimElements (PropElements)*` would
   * admit `a.b.c` for relative paths (verif/lean LEAN-203), so allow at most
   * one property in every form. */
  if (!parse_path_elements(data, size, &pos, prim_required, 1)) {
    return 0;
  }
  return pos == size;
}
