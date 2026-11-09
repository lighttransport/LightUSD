/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * lexer.c -- C11 USDA lexical primitives.
 *
 * The byte-classification and keyword/number scanning core of
 * src/next/parser/lexer.cc: identifier-start (UTF-8 XID + '_'), ASCII digit/
 * hex classification, the inf/infinity/nan special-float word matcher, the
 * keyword table lookup, and the numeric-literal length scanner. The stateful
 * Lexer class (position/line/column, token construction) stays C++.
 */

#include "lightusd-core-c.h"

#include <ctype.h>
#include <string.h>

int lu_lex_is_identifier_start(const char *data, size_t size, size_t pos) {
  uint32_t cp = 0;
  size_t width = 0;
  return lu_utf8_decode(data, size, pos, &cp, &width) &&
         lu_is_identifier_start(cp);
}

int lu_lex_is_identifier_continue_ascii(char c) {
  return isalnum((unsigned char)c) != 0 || c == '_';
}

int lu_lex_is_digit(char c) { return isdigit((unsigned char)c) != 0; }

int lu_lex_is_hex_digit(char c) { return isxdigit((unsigned char)c) != 0; }

int lu_lex_hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Length of an `inf` / `infinity` / `nan` special-float word (case-insensitive)
 * starting at p, or 0 if none. Requires a non-identifier char (or end) after
 * the word so e.g. `info` / `index` are not mistaken for `inf`. */
size_t lu_lex_match_float_special(const char *p, const char *end) {
  size_t i;
  if (end - p >= 8) {
    static const char kInfinity[] = "infinity";
    for (i = 0; i < 8; i++)
      if (tolower((unsigned char)p[i]) != kInfinity[i]) break;
    if (i == 8 && ((p + 8 == end) || !lu_lex_is_identifier_continue_ascii(p[8])))
      return 8;
  }
  if (end - p >= 3) {
    if (tolower((unsigned char)p[0]) == 'i' &&
        tolower((unsigned char)p[1]) == 'n' &&
        tolower((unsigned char)p[2]) == 'f' &&
        ((p + 3 == end) || !lu_lex_is_identifier_continue_ascii(p[3])))
      return 3;
    if (tolower((unsigned char)p[0]) == 'n' &&
        tolower((unsigned char)p[1]) == 'a' &&
        tolower((unsigned char)p[2]) == 'n' &&
        ((p + 3 == end) || !lu_lex_is_identifier_continue_ascii(p[3])))
      return 3;
  }
  return 0;
}

typedef struct lu_lex_keyword {
  const char *name;
  uint8_t type;
} lu_lex_keyword;

static const lu_lex_keyword lu_lex_keywords[] = {
    {"def", LU_TOK_DEF},
    {"over", LU_TOK_OVER},
    {"class", LU_TOK_CLASS},
    {"true", LU_TOK_TRUE},
    {"True", LU_TOK_TRUE},
    {"false", LU_TOK_FALSE},
    {"False", LU_TOK_FALSE},
    {"None", LU_TOK_NONE},
    {"timeSamples", LU_TOK_TIMESAMPLES},
    {"custom", LU_TOK_CUSTOM},
    {"uniform", LU_TOK_UNIFORM},
    {"varying", LU_TOK_VARYING},
    {"prepend", LU_TOK_PREPEND},
    {"append", LU_TOK_APPEND},
    {"delete", LU_TOK_DELETE},
    {"add", LU_TOK_ADD},
    {"reorder", LU_TOK_REORDER},
    {"rel", LU_TOK_REL},
};

uint8_t lu_lex_lookup_keyword(const char *p, size_t n) {
  size_t k;
  if (n < 3 || n > 11) return LU_TOK_IDENTIFIER;
  for (k = 0; k < sizeof(lu_lex_keywords) / sizeof(lu_lex_keywords[0]); ++k) {
    const lu_lex_keyword *kw = &lu_lex_keywords[k];
    if (kw->name[0] == p[0] && strlen(kw->name) == n &&
        memcmp(kw->name, p, n) == 0) {
      return kw->type;
    }
  }
  return LU_TOK_IDENTIFIER;
}

/* Consume one numeric literal from [p, end) and return its byte length. Mirrors
 * next::Lexer::scan_number's position progression: optional sign, a signed
 * inf/infinity/nan word, a hex literal, or a decimal integer/float with an
 * optional (only-if-followed-by-digits) exponent. */
size_t lu_lex_scan_number(const char *p, const char *end) {
  const char *start = p;

  if (p < end && (*p == '-' || *p == '+')) p++;

  {
    const size_t special = lu_lex_match_float_special(p, end);
    if (special) return (size_t)(p - start) + special;
  }

  if (p < end && p[0] == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) {
    p += 2;
    while (p < end && lu_lex_is_hex_digit(*p)) p++;
  } else {
    while (p < end && lu_lex_is_digit(*p)) p++;
    if (p < end && *p == '.') {
      p++;
      while (p < end && lu_lex_is_digit(*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      const char *exp_pos = p;
      p++;
      if (p < end && (*p == '+' || *p == '-')) p++;
      if (p < end && lu_lex_is_digit(*p)) {
        while (p < end && lu_lex_is_digit(*p)) p++;
      } else {
        p = exp_pos; /* rewind: not an exponent */
      }
    }
  }

  return (size_t)(p - start);
}
