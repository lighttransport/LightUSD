/* SPDX-License-Identifier: Apache-2.0 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "next/c/lightusd-core-c.h"
#include <assert.h>
#include <string.h>

static size_t read_digit(void *user, lu_view input, size_t limit, size_t *count) {
  size_t *calls = (size_t *)user;
  size_t pos = 0;
  *count = 0;
  while (pos < input.len && input.data[pos] >= '0' && input.data[pos] <= '9') {
    ++*calls;
    ++*count;
    ++pos;
    if (*count == limit || pos + 1 >= input.len || input.data[pos] != ',' ||
        input.data[pos + 1] < '0' || input.data[pos + 1] > '9') break;
    ++pos;
  }
  return pos;
}
static size_t bad_reader(void *user, lu_view input, size_t limit, size_t *count) {
  (void)user;
  (void)limit;
  *count = 1;
  return input.len + 1;
}
static void check(const char *s, unsigned rows, unsigned cols, int valid,
                  size_t expected) {
  size_t count = 99, calls = 0;
  lu_view v = {s, strlen(s)};
  assert(lu_usda_walk_array(v, rows, cols, 10, 100,
                            read_digit, &calls, &count) == valid);
  assert(count == (valid ? expected : 0));
}
int main(void) {
  size_t count = 99, calls = 0;
  const char tuple[] = "[(1,2),(3,4)]";
  const lu_view view = {tuple, sizeof(tuple) - 1};
  check("[]", 0, 1, 1, 0);
  check("[1, # comment\n 2,] # tail", 0, 1, 1, 2);
  check("[(1,2), (3,4),]", 0, 2, 1, 2);
  check("[((1,2),(3,4)),]", 2, 2, 1, 1);
  check("[((1,2,3),(4,5,6),(7,8,9))]", 3, 3, 1, 1);
  check("[1 2]", 0, 1, 0, 0);
  check("[1,,]", 0, 1, 0, 0);
  check("[(1,2,)]", 0, 2, 0, 0);
  check("[(1)]", 0, 2, 0, 0);
  check("[((1,2),(3,4))]x", 2, 2, 0, 0);
  check("[((1,2),(3,4)]", 2, 2, 0, 0);
  check("[]", 0, 0, 0, 0);
  check("[]", 3, 2, 0, 0);
  check("[]", 5, 5, 0, 0);
  assert(!lu_usda_walk_array(view, 0, 2, 10, 3, read_digit, &calls, &count));
  assert(calls == 2 && count == 0);  /* No partial second element. */
  calls = 0;
  assert(!lu_usda_walk_array(view, 0, 2, 0, SIZE_MAX, read_digit, &calls, &count));
  assert(calls == 0);
  assert(!lu_usda_walk_array(view, 0, 2, SIZE_MAX, SIZE_MAX,
                             bad_reader, NULL, &count));
  assert(!lu_usda_walk_array((lu_view){NULL, 0}, 0, 1, 10, 10,
                             read_digit, &calls, &count));
  /* Every truncated prefix stops inside its counted span. */
  for (size_t n = 0; n < view.len; ++n) {
    assert(!lu_usda_walk_array((lu_view){tuple, n}, 0, 2, 10, 100,
                               read_digit, &calls, &count));
  }
  for (size_t n = 0; n <= 8; ++n) {
    const char *word = "infinity";
    const size_t matched = lu_lex_match_float_special(word, word + n);
    assert(matched == ((n == 3 || n == 8) ? n : 0));
  }
  {
    const char sign[] = "+";
    assert(lu_lex_scan_number(sign, sign + 1) == 1);
  }
  return 0;
}
