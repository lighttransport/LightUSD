/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 * Allocation-free USDA array grammar shared by counting and materialization.
 */
#include "lightusd-core-c.h"

typedef struct array_cursor {
  lu_view input;
  size_t pos;
} array_cursor;

static void skip_space(array_cursor *s) {
  while (s->pos < s->input.len) {
    const char c = s->input.data[s->pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++s->pos;
    } else if (c == '#') {
      while (s->pos < s->input.len && s->input.data[s->pos] != '\n')
        ++s->pos;
    } else {
      break;
    }
  }
}

static int consume(array_cursor *s, char c) {
  skip_space(s);
  if (s->pos == s->input.len || s->input.data[s->pos] != c) return 0;
  ++s->pos;
  return 1;
}

int lu_usda_walk_array(lu_view input, unsigned rows, unsigned columns,
                       size_t max_elements, size_t max_scalars,
                       lu_usda_read_values read_values, void *user,
                       size_t *out_count) {
  array_cursor s = {input, 0};
  size_t count = 0;
  unsigned r;
  const unsigned lanes = (rows ? rows : 1) * columns;
  if (out_count) *out_count = 0;
  if (!input.data || !read_values || columns < 1 || columns > 4 ||
      (rows && (rows < 2 || rows > 4 || rows != columns))) return 0;
  if (!consume(&s, '[')) return 0;
  skip_space(&s);
  while (s.pos < input.len && input.data[s.pos] != ']') {
    /* Check before invoking callbacks: even a partial element cannot grow
     * the caller's output beyond its scalar budget. Division avoids overflow. */
    if (count >= max_elements || count >= max_scalars / lanes) return 0;
    if (columns > 1 && !consume(&s, '(')) return 0;
    for (r = 0; r < (rows ? rows : 1); ++r) {
      if (rows && !consume(&s, '(')) return 0;
      {
        lu_view remaining;
        size_t consumed, values = 0;
        size_t limit = columns;
        if (columns == 1) {
          limit = max_elements - count;
          if (limit > max_scalars - count) limit = max_scalars - count;
        }
        skip_space(&s);
        remaining.data = input.data + s.pos;
        remaining.len = input.len - s.pos;
        consumed = read_values(user, remaining, limit, &values);
        if (!consumed || consumed > remaining.len || !values || values > limit ||
            (columns > 1 && values != columns)) return 0;
        s.pos += consumed;
        if (columns == 1) count += values - 1;
      }
      if (rows && !consume(&s, ')')) return 0;
      if (r + 1 < rows && !consume(&s, ',')) return 0;
    }
    if (columns > 1 && !consume(&s, ')')) return 0;
    ++count;
    if (!consume(&s, ',')) break;
    skip_space(&s);  /* A trailing comma before ']' is valid USDA. */
  }
  if (!consume(&s, ']')) return 0;
  skip_space(&s);
  if (s.pos != input.len) return 0;
  if (out_count) *out_count = count;
  return 1;
}
