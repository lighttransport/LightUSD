// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string_view>

namespace lightusd {
namespace next {

// OpenUSD-compatible dictionary order for element/property names. ASCII case
// and leading-zero differences defer to the end. Numeric runs are compared by
// significant length and bytes, so their size is not bounded by an integer type.
// This is the compatibility order: unlike the literal Core spec, digits precede
// underscores. UTF-8 bytes compare unsigned, preserving valid scalar order.
inline bool DictionaryLess(std::string_view left, std::string_view right) {
  if (left == right) return false;
  const auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
  const auto letter = [](unsigned char c) {
    c = static_cast<unsigned char>(c & ~0x20u);
    return c >= 'A' && c <= 'Z';
  };
  size_t l = 0, r = 0;
  for (;;) {
    while (l < left.size() && r < right.size() && left[l] == right[r]) {
      ++l;
      ++r;
    }
    if (l == left.size() || r == right.size()) break;
    const auto a = static_cast<unsigned char>(left[l]);
    const auto b = static_cast<unsigned char>(right[r]);
    if (a < 0x80 && b < 0x80 && a >= 0x40 && b >= 0x40 &&
        (a & ~0x20u) != (b & ~0x20u))
      return ((a + 5u) & 31u) < ((b + 5u) & 31u);
    if (digit(a) && digit(b)) {
      size_t lstart = l, rstart = r, lend = l, rend = r;
      while (lstart && digit(static_cast<unsigned char>(left[lstart - 1]))) --lstart;
      while (rstart && digit(static_cast<unsigned char>(right[rstart - 1]))) --rstart;
      while (lend < left.size() && digit(static_cast<unsigned char>(left[lend]))) ++lend;
      while (rend < right.size() && digit(static_cast<unsigned char>(right[rend]))) ++rend;
      while (lstart < lend && left[lstart] == '0') ++lstart;
      while (rstart < rend && right[rstart] == '0') ++rstart;
      if (lend - lstart != rend - rstart) return lend - lstart < rend - rstart;
      const int numeric = left.substr(lstart, lend - lstart).compare(
          right.substr(rstart, rend - rstart));
      if (numeric) return numeric < 0;
      l = lend;
      r = rend;
      continue;
    }
    if (digit(a) || digit(b)) {
      if (l == 0) return a < b;
      // A digit run ending at this mismatch sorts before a continuation of
      // that run, even when extra zeros leave its numeric value unchanged.
      return digit(static_cast<unsigned char>(left[l - 1])) ? digit(b) : digit(a);
    }
    if (!letter(a) || !letter(b)) return a < b;
    // ASCII letters differing only in case: defer the tie-break.
    ++l;
    ++r;
  }
  if (l != left.size() || r != right.size()) return l == left.size();
  // Equal primary keys. The first original byte difference determines case or
  // leading-zero order; a missing byte is the terminating zero for this tie.
  size_t first = 0;
  while (first < left.size() && first < right.size() && left[first] == right[first]) ++first;
  const auto a = first < left.size() ? static_cast<unsigned char>(left[first]) : 0u;
  const auto b = first < right.size() ? static_cast<unsigned char>(right[first]) : 0u;
  return b == '0' || (a != '0' && a < b);
}

}  // namespace next
}  // namespace lightusd
