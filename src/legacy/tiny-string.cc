// SPDX-License-Identifier: MIT
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "tiny-string.hh"
#include <cmath>
#include "value-types.hh"  // For complete matrix type definitions

#if defined(LIGHTUSD_USE_THREAD)
#include <thread>
#include <atomic>
#include <mutex>
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/fast_float/include/fast_float/fast_float.h"

#define nssv_CONFIG_USR_SV_OPERATOR  0

// TODO(syoyo): Use C++17 std::string_view when compiled with C++-17 compiler

// clang and gcc
#if defined(__EXCEPTIONS) || defined(__cpp_exceptions)

#ifdef nsel_CONFIG_NO_EXCEPTIONS
#undef nsel_CONFIG_NO_EXCEPTIONS
#endif
#ifdef nssv_CONFIG_NO_EXCEPTIONS
#undef nssv_CONFIG_NO_EXCEPTIONS
#endif

#define nsel_CONFIG_NO_EXCEPTIONS 0
#define nssv_CONFIG_NO_EXCEPTIONS 0
#else
// -fno-exceptions
#if !defined(nsel_CONFIG_NO_EXCEPTIONS)
#define nsel_CONFIG_NO_EXCEPTIONS 1
#endif

#define nssv_CONFIG_NO_EXCEPTIONS 1
#endif
#include "nonstd/string_view.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {

namespace str {

struct Lexer {

  void skip_whitespaces() {

    while (!eof()) {

      char s = *curr;
      if ((s == ' ') || (s == '\t') || (s == '\f') || (s == '\n') || (s == '\r') || (s == '\v')) {
        curr++;
      }
      break;
    }

  }

  bool skip_until_delim_or_close_paren(const char delim, const char close_paren) {

    while (!eof()) {

      char s = *curr;
      if ((s == delim) || (s == close_paren)) {
        return true;
      }

      curr++;
    }

    return false;
  }

  bool char1(char *result) {
    if (eof()) {
      return false;
    }
    *result = *curr;
    curr++;

    return true;
  }

  bool look_char1(char *result) {
    if (eof()) {
      return false;
    }
    *result = *curr;

    return true;
  }

  bool consume_char1() {
    if (eof()) {
      return false;
    }
    curr++;

    return true;
  }

  inline bool eof() const {
    return (curr >= p_end);
  }

  inline bool unwind_char1() {
    if (curr <= p_begin) {
      return false;
    }

    curr--;
    return true;
  }

  bool lex_float(uint16_t &len, bool &truncated) {

    // truncate too large fp string
    // (e.g. "0.100000010000000100000010000..."
    constexpr size_t n_trunc_chars = 256; // 65535 at max.

    size_t n = 0;
    bool has_sign = false;
    bool has_exponential = false;
    bool has_dot = false;

    // oneOf [0-9, eE, -+]
    while (!eof() || (n < n_trunc_chars)) {
      char c;
      look_char1(&c);
      if ((c == '-') || (c == '+')) {
        if (has_sign) {
          return false;
        }
        has_sign = true;
      } else if (c == '.') {
        if (has_dot) {
          return false;
        }
        has_dot = true;
      } else if ((c == 'e') || (c == 'E')) {
        if (has_exponential) {
          return false;
        }
        has_exponential = true;
      } else if ((c >= '0') && (c <= '9')) {
      } else {
        break;
      }

      consume_char1();
      n++;
    }

    if (n == 0) {
      len = 0;
      return false;
    }

    truncated = (n >= n_trunc_chars);

    len = uint16_t(n);
    return true;
  }

  void push_error(const std::string &msg) {
    err_ += msg + "\n";
  }

  std::string get_error() const {
    return err_;
  }

  const char *p_begin{nullptr};
  const char *p_end{nullptr};

  const char *curr{nullptr};

 private:
  std::string err_;
};


struct fp_lex_span
{
  const char *p_begin{nullptr};
  uint16_t length{0};
};

template<size_t N>
struct vec_lex_span
{
  fp_lex_span vspans[N];
};

namespace internal {


} // namespace internal

bool parse_int(const tstring_view &sv, int32_t *ret) {
  const char* str = sv.c_str();
  size_t len = sv.size();

  if (len == 0) {
    return false;
  }

  bool negative = false;
  size_t start = 0;

  if (str[0] == '-') {
    negative = true;
    start = 1;
  } else if (str[0] == '+') {
    start = 1;
  }

  if (start >= len) {
    return false;
  }

  int64_t result = 0;
  for (size_t i = start; i < len; i++) {
    if (str[i] < '0' || str[i] > '9') {
      return false;
    }
    result = result * 10 + (str[i] - '0');

    // Check for overflow
    if (negative && result > static_cast<int64_t>((std::numeric_limits<int32_t>::max)()) + 1) {
      return false;
    }
    if (!negative && result > (std::numeric_limits<int32_t>::max)()) {
      return false;
    }
  }

  *ret = static_cast<int32_t>(negative ? -result : result);
  return true;
}

bool parse_int64(const tstring_view &sv, int64_t *ret) {
  const char* str = sv.c_str();
  size_t len = sv.size();

  if (len == 0) {
    return false;
  }

  bool negative = false;
  size_t start = 0;

  if (str[0] == '-') {
    negative = true;
    start = 1;
  } else if (str[0] == '+') {
    start = 1;
  }

  if (start >= len) {
    return false;
  }

  uint64_t result = 0;
  for (size_t i = start; i < len; i++) {
    if (str[i] < '0' || str[i] > '9') {
      return false;
    }
    result = result * 10ull + uint64_t(str[i] - '0');

    // Check for overflow
    if (negative && result > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) + 1) {
      return false;
    }
    if (!negative && result > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
      return false;
    }
  }

  *ret = negative ? -static_cast<int64_t>(result) : static_cast<int64_t>(result);
  return true;
}

bool parse_uint(const tstring_view &sv, uint32_t *ret) {
  const char* str = sv.c_str();
  size_t len = sv.size();

  if (len == 0) {
    return false;
  }

  size_t start = 0;
  if (str[0] == '+') {
    start = 1;
  }

  if (start >= len) {
    return false;
  }

  uint64_t result = 0;
  for (size_t i = start; i < len; i++) {
    if (str[i] < '0' || str[i] > '9') {
      return false;
    }
    result = result * 10 + uint64_t(str[i] - '0');

    // Check for overflow
    if (result > (std::numeric_limits<uint32_t>::max)()) {
      return false;
    }
  }

  *ret = static_cast<uint32_t>(result);
  return true;
}

bool parse_uint64(const tstring_view &sv, uint64_t *ret) {
  const char* str = sv.c_str();
  size_t len = sv.size();

  if (len == 0) {
    return false;
  }

  size_t start = 0;
  if (str[0] == '+') {
    start = 1;
  }

  if (start >= len) {
    return false;
  }

  uint64_t result = 0;
  for (size_t i = start; i < len; i++) {
    if (str[i] < '0' || str[i] > '9') {
      return false;
    }

    // Check for overflow before multiplication
    if (result > ((std::numeric_limits<uint64_t>::max)() - uint64_t(str[i] - '0')) / 10) {
      return false;
    }

    result = result * 10 + uint64_t(str[i] - '0');
  }

  *ret = result;
  return true;
}

bool parse_float(const tstring_view &sv, float *ret) {
  auto result = fast_float::from_chars(sv.c_str(), sv.c_str() + sv.size(), *ret);
  return result.ec == std::errc{};
}

bool parse_double(const tstring_view &sv, double *ret) {
  auto result = fast_float::from_chars(sv.c_str(), sv.c_str() + sv.size(), *ret);
  return result.ec == std::errc{};
}


// Skip whitespace and '#' line comments
static inline const char* skip_whitespace(const char *p, const char *end) {
  while (p < end) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      p++;
    } else if (*p == '#') {
      // Skip comment to end of line
      while (p < end && *p != '\n' && *p != '\r') {
        p++;
      }
    } else {
      break;
    }
  }
  return p;
}

// Helper: parse a single float/double from pointer, advancing past the number
template<typename T>
static inline bool parse_single(const char **p, const char *end, T *value) {
  const char *start = *p;
  auto result = fast_float::from_chars(start, end, *value);
  if (result.ec != std::errc{} || result.ptr == start) {
    return false;
  }
  *p = result.ptr;
  return true;
}

static inline bool parse_single_half(const char **p, const char *end,
                                     lightusd::value::half *value) {
  float f;
  if (!parse_single<float>(p, end, &f)) {
    return false;
  }
  *value = lightusd::value::float_to_half_full(f);
  return true;
}

// Scalar array parser: [val, val, ...]
// Works for float, double (via fast_float) and int (via parse_int).
template<typename T, typename ParseFn>
static bool parse_scalar_array_impl(const tstring_view &sv, std::vector<T> *result,
                                    ParseFn parse_fn) {
  if (!result) return false;
  result->clear();
  if (sv.size() == 0) return false;

  const char *p = sv.c_str();
  const char *end = p + sv.size();

  p = skip_whitespace(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  p = skip_whitespace(p, end);
  if (p < end && *p == ']') return true;

  while (p < end) {
    p = skip_whitespace(p, end);
    if (p >= end) break;
    if (*p == ']') break;

    const char *num_start = p;
    while (p < end && *p != ',' && *p != ']' && *p != '#' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
      p++;
    }
    if (p <= num_start) return false;

    T value;
    if (!parse_fn(num_start, p, &value)) return false;
    result->push_back(value);

    p = skip_whitespace(p, end);
    if (p < end && *p == ',') {
      p++;
    } else if (p < end && *p == ']') {
      break;
    }
  }
  return true;
}

bool parse_float_array(const tstring_view &sv, std::vector<float> *result) {
  return parse_scalar_array_impl<float>(sv, result,
    [](const char *start, const char *end, float *val) -> bool {
      auto r = fast_float::from_chars(start, end, *val);
      return r.ec == std::errc{};
    });
}

bool parse_double_array(const tstring_view &sv, std::vector<double> *result) {
  return parse_scalar_array_impl<double>(sv, result,
    [](const char *start, const char *end, double *val) -> bool {
      auto r = fast_float::from_chars(start, end, *val);
      return r.ec == std::errc{};
    });
}

// pxrUSD coerces float literals to integer types in usda text, truncating
// toward zero (`int[] a = [1.5]` -> [1]); out of range AFTER truncation is a
// parse error and NaN/inf fail the range comparisons. `hi_excl` is exclusive
// so the bound is an exactly-representable power of two.
static bool coerce_float_to_i64(const tstring_view &sv, double lo,
                                double hi_excl, int64_t *out) {
  double d = 0.0;
  if (!parse_double(sv, &d)) {
    return false;
  }
  d = std::trunc(d);
  if (!(d >= lo && d < hi_excl)) {
    return false;
  }
  *out = static_cast<int64_t>(d);
  return true;
}

static bool coerce_float_to_u64(const tstring_view &sv, double hi_excl,
                                uint64_t *out) {
  double d = 0.0;
  if (!parse_double(sv, &d)) {
    return false;
  }
  d = std::trunc(d);
  if (!(d >= 0.0 && d < hi_excl)) {
    return false;
  }
  *out = static_cast<uint64_t>(d);
  return true;
}

bool parse_int_array(const tstring_view &sv, std::vector<int32_t> *result) {
  return parse_scalar_array_impl<int32_t>(sv, result,
    [](const char *start, const char *end, int32_t *val) -> bool {
      tstring_view num_view(start, size_t(end - start));
      if (parse_int(num_view, val)) return true;
      int64_t wide = 0;
      if (!coerce_float_to_i64(num_view, -2147483648.0, 2147483648.0, &wide)) {
        return false;
      }
      *val = static_cast<int32_t>(wide);
      return true;
    });
}

bool parse_uint_array(const tstring_view &sv, std::vector<uint32_t> *result) {
  return parse_scalar_array_impl<uint32_t>(sv, result,
    [](const char *start, const char *end, uint32_t *val) -> bool {
      tstring_view num_view(start, size_t(end - start));
      if (parse_uint(num_view, val)) return true;
      uint64_t wide = 0;
      if (!coerce_float_to_u64(num_view, 4294967296.0, &wide)) {
        return false;
      }
      *val = static_cast<uint32_t>(wide);
      return true;
    });
}

bool parse_int64_array(const tstring_view &sv, std::vector<int64_t> *result) {
  return parse_scalar_array_impl<int64_t>(sv, result,
    [](const char *start, const char *end, int64_t *val) -> bool {
      tstring_view num_view(start, size_t(end - start));
      if (parse_int64(num_view, val)) return true;
      return coerce_float_to_i64(num_view, -9223372036854775808.0,
                                 9223372036854775808.0, val);
    });
}

bool parse_uint64_array(const tstring_view &sv, std::vector<uint64_t> *result) {
  return parse_scalar_array_impl<uint64_t>(sv, result,
    [](const char *start, const char *end, uint64_t *val) -> bool {
      tstring_view num_view(start, size_t(end - start));
      if (parse_uint64(num_view, val)) return true;
      return coerce_float_to_u64(num_view, 18446744073709551616.0, val);
    });
}

bool parse_half_array(const tstring_view &sv, std::vector<lightusd::value::half> *result) {
  return parse_scalar_array_impl<lightusd::value::half>(sv, result,
    [](const char *start, const char *end, lightusd::value::half *val) -> bool {
      float f;
      auto r = fast_float::from_chars(start, end, f);
      if (r.ec != std::errc{}) return false;
      *val = lightusd::value::float_to_half_full(f);
      return true;
    });
}

namespace {

constexpr size_t kMaxStringArrayElements = 1024ull * 1024ull * 128ull;
constexpr size_t kMaxStringLiteralLen = 64ull * 1024ull * 1024ull;

struct ParsedStringLiteral {
  std::string value;
  bool is_triple_quoted{false};
  bool single_quote{false};
};

struct ParsedTokenLiteral {
  const char *start{nullptr};
  const char *end{nullptr};
  std::string unescaped;
  bool use_unescaped{false};
};

static inline int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void assign_unescaped_control_range(const char *start, const char *end,
                                           std::string *out) {
  out->clear();
  out->reserve(size_t(end - start));

  for (const char *p = start; p < end; p++) {
    if (*p != '\\') {
      out->push_back(*p);
      continue;
    }

    if (p + 1 >= end) {
      continue;
    }

    const char esc = *(p + 1);
    if (esc == 'a') {
      out->push_back('\a');
      p++;
    } else if (esc == 'b') {
      out->push_back('\b');
      p++;
    } else if (esc == 't') {
      out->push_back('\t');
      p++;
    } else if (esc == 'v') {
      out->push_back('\v');
      p++;
    } else if (esc == 'f') {
      out->push_back('\f');
      p++;
    } else if (esc == 'n') {
      out->push_back('\n');
      p++;
    } else if (esc == 'r') {
      out->push_back('\r');
      p++;
    } else if (esc == '\\') {
      out->push_back('\\');
      p++;
    } else if (esc == 'x') {
      if (p + 3 < end) {
        const int d1 = hex_digit(*(p + 2));
        const int d2 = hex_digit(*(p + 3));
        if (d1 >= 0 && d2 >= 0) {
          out->push_back(static_cast<char>((d1 << 4) | d2));
          p += 3;
        } else {
          out->push_back('\\');
        }
      } else {
        out->push_back('\\');
      }
    } else {
      // Match unescapeControlSequence: unknown escapes drop the backslash.
    }
  }
}

static bool has_backslash(const char *start, const char *end) {
  for (const char *p = start; p < end; p++) {
    if (*p == '\\') return true;
  }
  return false;
}

static void assign_string_range(const char *start, const char *end,
                                bool unescape, std::string *out) {
  if (unescape && has_backslash(start, end)) {
    assign_unescaped_control_range(start, end, out);
  } else {
    out->assign(start, size_t(end - start));
  }
}

static bool parse_quoted_string_literal(const char **p, const char *end,
                                        bool unescape_regular,
                                        ParsedStringLiteral *out) {
  if (!p || !out) return false;
  if (*p >= end) return false;

  const char quote = **p;
  if (quote != '"' && quote != '\'') return false;
  const bool single_quote = (quote == '\'');
  (*p)++;

  const bool is_triple =
      ((*p + 1) < end && (*p)[0] == quote && (*p)[1] == quote);
  if (is_triple) {
    *p += 2;
    const char *content_start = *p;

    while (*p < end) {
      if (size_t(*p - content_start) > kMaxStringLiteralLen) return false;

      if (**p == '\\') {
        if ((*p + 3) < end && (*p)[1] == quote && (*p)[2] == quote &&
            (*p)[3] == quote) {
          *p += 4;
          continue;
        }
        (*p)++;
        continue;
      }

      if ((*p + 2) < end && (*p)[0] == quote && (*p)[1] == quote &&
          (*p)[2] == quote) {
        const char *content_end = *p;
        assign_string_range(content_start, content_end, true, &out->value);
        out->is_triple_quoted = true;
        out->single_quote = single_quote;
        *p += 3;
        return true;
      }

      (*p)++;
    }
    return false;
  }

  const char *content_start = *p;
  while (*p < end) {
    if (size_t(*p - content_start) > kMaxStringLiteralLen) return false;

    const char c = **p;
    if (c == '\n' || c == '\r') return false;

    // Consume the escape pair atomically: \" and \' must not terminate
    // the literal, and the second char of \\ must not arm a fresh escape
    // (`"a\\"` is the token `a\`, not an unterminated literal).
    if (unescape_regular && c == '\\' && (*p + 1) < end) {
      *p += 2;
      continue;
    }

    if (c == quote) {
      const char *content_end = *p;
      assign_string_range(content_start, content_end, unescape_regular,
                          &out->value);
      out->is_triple_quoted = false;
      out->single_quote = false;
      (*p)++;
      return true;
    }

    (*p)++;
  }

  return false;
}

static bool parse_token_literal(const char **p, const char *end,
                                ParsedTokenLiteral *out) {
  if (!p || !out) return false;
  if (*p >= end) return false;

  const char quote = **p;
  if (quote != '"' && quote != '\'') return false;
  (*p)++;

  const bool is_triple =
      ((*p + 1) < end && (*p)[0] == quote && (*p)[1] == quote);
  if (is_triple) {
    *p += 2;
    const char *content_start = *p;

    while (*p < end) {
      if (size_t(*p - content_start) > kMaxStringLiteralLen) return false;

      if (**p == '\\') {
        if ((*p + 3) < end && (*p)[1] == quote && (*p)[2] == quote &&
            (*p)[3] == quote) {
          *p += 4;
          continue;
        }
        (*p)++;
        continue;
      }

      if ((*p + 2) < end && (*p)[0] == quote && (*p)[1] == quote &&
          (*p)[2] == quote) {
        const char *content_end = *p;
        if (has_backslash(content_start, content_end)) {
          assign_unescaped_control_range(content_start, content_end,
                                         &out->unescaped);
          out->use_unescaped = true;
        } else {
          out->start = content_start;
          out->end = content_end;
        }
        *p += 3;
        return true;
      }

      (*p)++;
    }
    return false;
  }

  const char *content_start = *p;
  bool saw_escape = false;
  while (*p < end) {
    if (size_t(*p - content_start) > kMaxStringLiteralLen) return false;

    const char c = **p;
    if (c == '\n' || c == '\r') return false;

    // Escape pair (\" \' \\ \t ...): consume atomically so an escaped
    // quote does not terminate the token literal. Previously this loop had
    // NO escape handling and `token[] a = ["mix\"match"]` failed to parse.
    if (c == '\\' && (*p + 1) < end) {
      saw_escape = true;
      *p += 2;
      continue;
    }

    if (c == quote) {
      if (saw_escape) {
        assign_unescaped_control_range(content_start, *p, &out->unescaped);
        out->use_unescaped = true;
      } else {
        out->start = content_start;
        out->end = *p;
      }
      (*p)++;
      return true;
    }

    (*p)++;
  }

  return false;
}

template<typename PushFn>
static bool parse_quoted_array_impl(const tstring_view &sv,
                                    bool unescape_regular,
                                    PushFn push_value) {
  if (sv.size() == 0) return false;

  const char *p = sv.c_str();
  const char *end = p + sv.size();

  p = skip_whitespace(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  p = skip_whitespace(p, end);
  if (p < end && *p == ']') return true;

  size_t count = 0;
  while (p < end) {
    p = skip_whitespace(p, end);
    if (p >= end) return false;
    if (*p == ']') return true;

    ParsedStringLiteral parsed;
    if (!parse_quoted_string_literal(&p, end, unescape_regular, &parsed)) {
      return false;
    }

    if (count >= kMaxStringArrayElements) return false;
    push_value(std::move(parsed));
    count++;

    p = skip_whitespace(p, end);
    if (p < end && *p == ',') {
      p++;
      p = skip_whitespace(p, end);
      if (p < end && *p == ']') return true;
      continue;
    }
    if (p < end && *p == ']') return true;
    return false;
  }

  return false;
}

}  // namespace

bool parse_token_array(const tstring_view &sv, std::vector<lightusd::value::token> *result) {
  if (!result) return false;
  result->clear();
  if (sv.size() == 0) return false;

  const char *p = sv.c_str();
  const char *end = p + sv.size();

  p = skip_whitespace(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  p = skip_whitespace(p, end);
  if (p < end && *p == ']') return true;

  size_t count = 0;
  while (p < end) {
    p = skip_whitespace(p, end);
    if (p >= end) return false;
    if (*p == ']') return true;

    ParsedTokenLiteral parsed;
    if (!parse_token_literal(&p, end, &parsed)) {
      return false;
    }

    if (count >= kMaxStringArrayElements) return false;
    if (parsed.use_unescaped) {
      result->emplace_back(std::move(parsed.unescaped));
    } else {
      result->emplace_back(parsed.start, size_t(parsed.end - parsed.start));
    }
    count++;

    p = skip_whitespace(p, end);
    if (p < end && *p == ',') {
      p++;
      p = skip_whitespace(p, end);
      if (p < end && *p == ']') return true;
      continue;
    }
    if (p < end && *p == ']') return true;
    return false;
  }

  return false;
}

bool parse_string_array(const tstring_view &sv, std::vector<lightusd::value::StringData> *result) {
  if (!result) return false;
  result->clear();
  return parse_quoted_array_impl(sv, true, [&](ParsedStringLiteral parsed) {
    lightusd::value::StringData sdata;
    sdata.value = std::move(parsed.value);
    sdata.is_triple_quoted = parsed.is_triple_quoted;
    sdata.single_quote = parsed.single_quote;
    result->push_back(std::move(sdata));
  });
}

bool parse_std_string_array(const tstring_view &sv, std::vector<std::string> *result) {
  if (!result) return false;
  result->clear();
  return parse_quoted_array_impl(sv, true, [&](ParsedStringLiteral parsed) {
    result->push_back(std::move(parsed.value));
  });
}

// Tuple array parser: [(v0, v1, ...), (v0, v1, ...), ...]
// VecT must support operator[] for element access.
template<typename VecT, size_t N, typename ParseFn>
static bool parse_tuple_array_impl(const tstring_view &sv, std::vector<VecT> *result,
                                   ParseFn parse_fn) {
  if (!result) return false;
  result->clear();
  if (sv.size() == 0) return false;

  const char *p = sv.c_str();
  const char *end = p + sv.size();

  p = skip_whitespace(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  p = skip_whitespace(p, end);
  if (p < end && *p == ']') return true;

  while (p < end) {
    p = skip_whitespace(p, end);
    if (p >= end) break;
    if (*p == ']') break;

    if (*p != '(') return false;
    p++;

    VecT vec{};
    for (size_t i = 0; i < N; i++) {
      p = skip_whitespace(p, end);
      if (!parse_fn(&p, end, &vec[i])) return false;
      p = skip_whitespace(p, end);
      if (i < N - 1) {
        if (p >= end || *p != ',') return false;
        p++;
      }
    }

    p = skip_whitespace(p, end);
    if (p >= end || *p != ')') return false;
    p++;

    result->push_back(vec);

    p = skip_whitespace(p, end);
    if (p < end && *p == ',') p++;
  }
  return true;
}

template<typename QuatT, typename ScalarT, typename ParseFn, typename AssignFn>
static bool parse_quat_array_impl(const tstring_view &sv, std::vector<QuatT> *result,
                                  ParseFn parse_fn, AssignFn assign_fn) {
  if (!result) return false;
  result->clear();
  if (sv.size() == 0) return false;

  const char *p = sv.c_str();
  const char *end = p + sv.size();

  p = skip_whitespace(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  p = skip_whitespace(p, end);
  if (p < end && *p == ']') return true;

  while (p < end) {
    p = skip_whitespace(p, end);
    if (p >= end) break;
    if (*p == ']') break;

    if (*p != '(') return false;
    p++;

    ScalarT values[4]{};
    for (size_t i = 0; i < 4; i++) {
      p = skip_whitespace(p, end);
      if (!parse_fn(&p, end, &values[i])) return false;
      p = skip_whitespace(p, end);
      if (i < 3) {
        if (p >= end || *p != ',') return false;
        p++;
      }
    }

    p = skip_whitespace(p, end);
    if (p >= end || *p != ')') return false;
    p++;

    QuatT quat{};
    assign_fn(values, &quat);
    result->push_back(quat);

    p = skip_whitespace(p, end);
    if (p < end && *p == ',') p++;
  }
  return true;
}

bool parse_half2_array(const tstring_view &sv, std::vector<lightusd::value::half2> *result) {
  return parse_tuple_array_impl<lightusd::value::half2, 2>(sv, result, parse_single_half);
}
bool parse_half3_array(const tstring_view &sv, std::vector<lightusd::value::half3> *result) {
  return parse_tuple_array_impl<lightusd::value::half3, 3>(sv, result, parse_single_half);
}
bool parse_half4_array(const tstring_view &sv, std::vector<lightusd::value::half4> *result) {
  return parse_tuple_array_impl<lightusd::value::half4, 4>(sv, result, parse_single_half);
}
bool parse_float2_array(const tstring_view &sv, std::vector<lightusd::value::float2> *result) {
  return parse_tuple_array_impl<lightusd::value::float2, 2>(sv, result, parse_single<float>);
}
bool parse_float3_array(const tstring_view &sv, std::vector<lightusd::value::float3> *result) {
  return parse_tuple_array_impl<lightusd::value::float3, 3>(sv, result, parse_single<float>);
}
bool parse_float4_array(const tstring_view &sv, std::vector<lightusd::value::float4> *result) {
  return parse_tuple_array_impl<lightusd::value::float4, 4>(sv, result, parse_single<float>);
}
bool parse_point3f_array(const tstring_view &sv, std::vector<lightusd::value::point3f> *result) {
  return parse_tuple_array_impl<lightusd::value::point3f, 3>(sv, result, parse_single<float>);
}
bool parse_normal3f_array(const tstring_view &sv, std::vector<lightusd::value::normal3f> *result) {
  return parse_tuple_array_impl<lightusd::value::normal3f, 3>(sv, result, parse_single<float>);
}
bool parse_double2_array(const tstring_view &sv, std::vector<lightusd::value::double2> *result) {
  return parse_tuple_array_impl<lightusd::value::double2, 2>(sv, result, parse_single<double>);
}
bool parse_double3_array(const tstring_view &sv, std::vector<lightusd::value::double3> *result) {
  return parse_tuple_array_impl<lightusd::value::double3, 3>(sv, result, parse_single<double>);
}
bool parse_double4_array(const tstring_view &sv, std::vector<lightusd::value::double4> *result) {
  return parse_tuple_array_impl<lightusd::value::double4, 4>(sv, result, parse_single<double>);
}
bool parse_quath_array(const tstring_view &sv, std::vector<lightusd::value::quath> *result) {
  return parse_quat_array_impl<lightusd::value::quath, float>(
    sv, result, parse_single<float>,
    [](const float values[4], lightusd::value::quath *quat) {
      quat->real = lightusd::value::float_to_half_full(values[0]);
      quat->imag[0] = lightusd::value::float_to_half_full(values[1]);
      quat->imag[1] = lightusd::value::float_to_half_full(values[2]);
      quat->imag[2] = lightusd::value::float_to_half_full(values[3]);
    });
}
bool parse_quatf_array(const tstring_view &sv, std::vector<lightusd::value::quatf> *result) {
  return parse_quat_array_impl<lightusd::value::quatf, float>(
    sv, result, parse_single<float>,
    [](const float values[4], lightusd::value::quatf *quat) {
      quat->real = values[0];
      quat->imag[0] = values[1];
      quat->imag[1] = values[2];
      quat->imag[2] = values[3];
    });
}
bool parse_quatd_array(const tstring_view &sv, std::vector<lightusd::value::quatd> *result) {
  return parse_quat_array_impl<lightusd::value::quatd, double>(
    sv, result, parse_single<double>,
    [](const double values[4], lightusd::value::quatd *quat) {
      quat->real = values[0];
      quat->imag[0] = values[1];
      quat->imag[1] = values[2];
      quat->imag[2] = values[3];
    });
}

// Matrix array parser: [((r00, r01, ...), (r10, r11, ...), ...), ...]
// MatT must have mat.m[row][col] access.
template<typename MatT, size_t N, typename ParseFn>
static bool parse_matrix_array_impl(const tstring_view &sv, std::vector<MatT> *result,
                                    ParseFn parse_fn) {
  if (!result) return false;
  result->clear();
  if (sv.size() == 0) return false;

  const char *p = sv.c_str();
  const char *end = p + sv.size();

  p = skip_whitespace(p, end);
  if (p >= end || *p != '[') return false;
  p++;

  p = skip_whitespace(p, end);
  if (p < end && *p == ']') return true;

  while (p < end) {
    p = skip_whitespace(p, end);
    if (p >= end) break;
    if (*p == ']') break;

    if (*p != '(') return false;
    p++;

    MatT mat{};
    for (size_t i = 0; i < N; i++) {
      p = skip_whitespace(p, end);
      if (p >= end || *p != '(') return false;
      p++;

      for (size_t j = 0; j < N; j++) {
        p = skip_whitespace(p, end);
        if (!parse_fn(&p, end, &mat.m[i][j])) return false;
        p = skip_whitespace(p, end);
        if (j < N - 1) {
          if (p >= end || *p != ',') return false;
          p++;
        }
      }

      p = skip_whitespace(p, end);
      if (p >= end || *p != ')') return false;
      p++;

      p = skip_whitespace(p, end);
      if (i < N - 1 && p < end && *p == ',') p++;
    }

    p = skip_whitespace(p, end);
    if (p >= end || *p != ')') return false;
    p++;

    result->push_back(mat);

    p = skip_whitespace(p, end);
    if (p < end && *p == ',') p++;
  }
  return true;
}

bool parse_matrix2f_array(const tstring_view &sv, std::vector<lightusd::value::matrix2f> *result) {
  return parse_matrix_array_impl<lightusd::value::matrix2f, 2>(sv, result, parse_single<float>);
}
bool parse_matrix3f_array(const tstring_view &sv, std::vector<lightusd::value::matrix3f> *result) {
  return parse_matrix_array_impl<lightusd::value::matrix3f, 3>(sv, result, parse_single<float>);
}
bool parse_matrix4f_array(const tstring_view &sv, std::vector<lightusd::value::matrix4f> *result) {
  return parse_matrix_array_impl<lightusd::value::matrix4f, 4>(sv, result, parse_single<float>);
}
bool parse_matrix2d_array(const tstring_view &sv, std::vector<lightusd::value::matrix2d> *result) {
  return parse_matrix_array_impl<lightusd::value::matrix2d, 2>(sv, result, parse_single<double>);
}
bool parse_matrix3d_array(const tstring_view &sv, std::vector<lightusd::value::matrix3d> *result) {
  return parse_matrix_array_impl<lightusd::value::matrix3d, 3>(sv, result, parse_single<double>);
}
bool parse_matrix4d_array(const tstring_view &sv, std::vector<lightusd::value::matrix4d> *result) {
  return parse_matrix_array_impl<lightusd::value::matrix4d, 4>(sv, result, parse_single<double>);
}

}


} // namespace lightusd
