// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <cstdint>

#include "nonstd/optional.hpp"

namespace lightusd { namespace legacy {

constexpr size_t kMaxUTF8Codepoint = 0x10ffff;

enum class CharEncoding
{
  None,
  UTF8,
  UTF8BOM, // UTF8 + BOM
  UTF16LE  // UTF16 LE(Windows Unicode)
};

inline const std::string to_lower(const std::string &str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    if ((c >= 'A') && (c <= 'Z')) {
      c += 32;
    }
    return static_cast<char>(c);
  });
  return result;
}

inline bool hasNewline(const std::string &str) {
  for (size_t i = 0; i < str.size(); i++) {
    if ((str[i] == '\r') || (str[i] == '\n')) {
      return true;
    }
  }

  return false;
}

inline bool startsWith(const std::string &str, const std::string &t) {
  return (str.size() >= t.size()) &&
         std::equal(std::begin(t), std::end(t), std::begin(str));
}

inline bool endsWith(const std::string &str, const std::string &suffix) {
  return (str.size() >= suffix.size()) &&
         (str.find(suffix, str.size() - suffix.size()) != std::string::npos);
}

inline std::string removePrefix(const std::string &str,
                                const std::string &prefix) {
  if (startsWith(str, prefix)) {
    return str.substr(prefix.length());
  }
  return str;
}

inline std::string removeSuffix(const std::string &str,
                                const std::string &suffix) {
  if (endsWith(str, suffix)) {
    return str.substr(0, str.length() - suffix.length());
  }
  return str;
}

inline bool contains(const std::string &str, char c) {
  return str.find(c) != std::string::npos;
}

inline bool contains_str(const std::string &str, const std::string &substr) {
  return str.find(substr) != std::string::npos;
}

inline size_t counts(const std::string &str, char c) {
  size_t cnt = 0;
  for (size_t i = 0; i < str.size(); i++) {
    if (str[i] == c) {
      cnt++;
    }
  }
  return cnt;
}

// Remove the beginning and the ending delimiter(s) from input string
// e.g. "mystring" -> mystring
// no error for an input string which does not contain `delim` in both side.
inline std::string unwrap(const std::string &str,
                          const std::string &delim = "\"") {
  size_t n = delim.size();

  if (str.size() < n) {
    return str;
  }

  std::string s = str;

  if (s.size() >= n && s.substr(0, n) == delim) {
    s.erase(0, n);
  }

  if (s.size() >= n && s.substr(s.size() - n) == delim) {
    s.erase(s.size() - n);
  }

  return s;
}

inline std::string unwrap(const std::string &str, const std::string &l,
                          const std::string &r) {
  return removePrefix(removeSuffix(str, r), l);
}

inline std::string quote(const char *s, const std::string &quote_str = "\"") {
  return quote_str + std::string(s) + quote_str;
}

inline std::string quote(const std::string &s,
                         const std::string &quote_str = "\"") {
  return quote_str + s + quote_str;
}

inline std::string wquote(const std::string &s,
                          const std::string &quote_lstr = "\"",
                          const std::string &quote_rstr = "\"") {
  return quote_lstr + s + quote_rstr;
}

// Path quote: wraps string in angle brackets < >
inline std::string pquote(const std::string &s) {
  return wquote(s, "<", ">");
}

inline std::vector<std::string> quote(const std::vector<std::string> &vs,
                                      const std::string &quote_str = "\"") {
  std::vector<std::string> dst;

  for (const auto &item : vs) {
    dst.emplace_back(quote(item, quote_str));
  }

  return dst;
}

// Python like join  ", ".join(v)
template <typename It>
inline std::string join(const std::string &sep, const It &v) {
  std::ostringstream oss;
  if (!v.empty()) {
    typename It::const_iterator it = v.begin();
    oss << *it++;
    for (typename It::const_iterator e = v.end(); it != e; ++it)
      oss << sep << *it;
  }
  return oss.str();
}

// To avoid splitting toooo large input text(e.g. few GB).
inline std::vector<std::string> split(
    const std::string &str, const std::string &sep,
    const uint32_t kMaxItems = (std::numeric_limits<int32_t>::max)() / 100) {
  size_t s;
  size_t e = 0;

  size_t count = 0;
  std::vector<std::string> result;

  while ((s = str.find_first_not_of(sep, e)) != std::string::npos) {
    e = str.find(sep, s);
    result.push_back(str.substr(s, e - s));
    if (++count > kMaxItems) {
      break;
    }
  }

  return result;
}

//
// "{name=varname}"
//
// => ["name", "varname"]
//
// "{name=}"
//
// => ["name", ""]
//
// Return false when input string is not a variant element, or `name` and `varname` contains invalid characters.
//
bool tokenize_variantElement(const std::string &elementName, std::array<std::string, 2> *result = nullptr);

bool is_variantElementName(const std::string &elementName);

///
/// Test if str contains " or '
///
/// @param[in] is_double_quote true: find escaped triple double quotes. false find escaped single double quotes.
///
bool hasQuotes(const std::string &str, bool is_double_quote);

///
/// Test if str contains """ or '''
///
/// @param[in] is_double_quote true: find escaped triple double quotes. false find escaped single double quotes.
///
bool hasTripleQuotes(const std::string &str, bool is_double_quote);

///
/// Test if str contains \""" or \'''
///
/// @param[in] is_double_quote true: find escaped triple double quotes. false find escaped single double quotes.
/// @param[out] n The number of escaped triple quotes
///
/// Return true immediately when an escaped triple quotes found when `n` is nullptr.
///
bool hasEscapedTripleQuotes(const std::string &str, bool is_double_quote, size_t *n = nullptr);

std::string escapeSingleQuote(const std::string &str, const bool is_double_quote);

std::string escapeBackslash(const std::string &str, const bool triple_quoted_string = false);

// Unescape backslash('\\' -> '\')
std::string unescapeBackslash(const std::string &str);

std::string escapeControlSequence(const std::string &str);

std::string unescapeControlSequence(const std::string &str);

std::string buildEscapedAndQuotedStringForUSDA(const std::string &str);

///
/// Determine if input UTF-8 string is Unicode Identifier
/// (UAX31 Default Identifier)
///
bool is_valid_utf8_identifier(const std::string &str);

// TfIsValidIdentifier in pxrUSD equivalanet
// Supports UTF-8 identifier(UAX31 Default Identifier. pxrUSD supports UTF8 Identififer from 24.03)
inline bool isValidIdentifier(const std::string &str, bool is_utf8 = true) {

  if (str.empty()) {
    return false;
  }

  if (is_utf8) {
    return is_valid_utf8_identifier(str);
  } else {
    // legacy

    // first char
    // [a-ZA-Z_]
    if ((('a' <= str[0]) && (str[0] <= 'z')) || (('A' <= str[0]) && (str[0] <= 'Z')) || (str[0] == '_')) {
      // ok
    } else {
      return false;
    }

    // remaining chars
    // [a-ZA-Z0-9_]
    for (size_t i = 1; i < str.length(); i++) {
      if ((('a' <= str[i]) && (str[i] <= 'z')) || (('A' <= str[i]) && (str[i] <= 'Z')) || (('0' <= str[i]) && (str[i] <= '9')) || (str[i] == '_')) {
        // ok
      } else {
        return false;
      }
    }
  }

  return true;
}


// TfMakeValidIdentifier in pxrUSD equivalanet
std::string makeIdentifierValid(const std::string &str, bool is_utf8 = true);

///
/// Simply add number suffix to make unique string.
///
/// - plane -> plane1
/// - sphere1 -> sphere11
/// - xform4 -> xform41
///
///
bool makeUniqueName(std::multiset<std::string> &nameSet, const std::string &name, std::string *unique_name);


///
/// Determine if input string is valid UTF-8 string.
///
bool is_valid_utf8(const std::string &str);


///
/// Convert string buffer to list of UTF-8 chars.
/// Example: 'こんにちは' => ['こ', 'ん', 'に', 'ち', 'は']
///
std::vector<std::string> to_utf8_chars(const std::string &str);

///
/// Convert UTF-8 char to codepoint.
/// Return ~0u(0xffffffff) when input `u8char` is not a valid UTF-8 charcter.
///
uint32_t to_utf8_code(const std::string &u8char);

///
/// Convert UTF-8 string to codepoint values.
///
/// Return empty array when input is not a valid UTF-8 string.
///
std::vector<uint32_t> to_codepoints(const std::string &str);

///
/// Convert UTF-8 codepoint to UTF-8 string.
///
inline std::string codepoint_to_utf8(uint32_t code) {
  if (code <= 0x7f) {
    return std::string(1, char(code));
  } else if (code <= 0x7ff) {
    // 11bit: 110y-yyyx 10xx-xxxx
    uint8_t buf[2];
    buf[0] = uint8_t(((code >> 6) & 0x1f) | 0xc0);
    buf[1] = uint8_t(((code >> 0) & 0x3f) | 0x80);
    return std::string(reinterpret_cast<const char *>(&buf[0]), 2);
  } else if (code <= 0xffff) {
    // 16bit: 1110-yyyy 10yx-xxxx 10xx-xxxx
    uint8_t buf[3];
    buf[0] = uint8_t(((code >> 12) & 0x0f) | 0xe0);
    buf[1] = uint8_t(((code >>  6) & 0x3f) | 0x80);
    buf[2] = uint8_t(((code >>  0) & 0x3f) | 0x80);
    return std::string(reinterpret_cast<const char *>(&buf[0]), 3);
  } else if (code <= 0x10ffff) {
    // 21bit: 1111-0yyy 10yy-xxxx 10xx-xxxx 10xx-xxxx
    uint8_t buf[4];
    buf[0] = uint8_t(((code >> 18) & 0x07) | 0xF0);
    buf[1] = uint8_t(((code >> 12) & 0x3F) | 0x80);
    buf[2] = uint8_t(((code >>  6) & 0x3F) | 0x80);
    buf[3] = uint8_t(((code >>  0) & 0x3F) | 0x80);
    return std::string(reinterpret_cast<const char *>(&buf[0]), 4);
  }

  // invalid
  return std::string();
}


//
// float/double to string
// Currently lightusd uses dragonbox algorithm
//
// buffer must be at least 25 bytes.
// filled string is not null-terminated.
// (Use *(dtoa(f, buf)) = '\0' if you want null-terminated string)
//
char *dtoa(float f, char *buf);
char *dtoa(double f, char *buf);




// Simple atof replacement
// Returns qNaN for invalid input.
double atof(const char *s);
double atof(const std::string &s);

///
/// Safe integer/double parse helpers (replacement for std::st*)
///
/// Return nonstd::optional<T> with the parsed value, or nullopt on error.
/// Uses fast_float::from_chars for float/double, manual parsing for int.
/// No exceptions thrown - check .has_value() to detect errors.
///
/// @param[in] s String to parse
/// @return optional<T> with parsed value, or nullopt on invalid input
///
nonstd::optional<int> atoi(const std::string &s);
nonstd::optional<uint32_t> atou(const std::string &s);
nonstd::optional<int64_t> atoll(const std::string &s);
nonstd::optional<uint64_t> atoull(const std::string &s);
nonstd::optional<float> atof_float(const std::string &s);
nonstd::optional<double> atod(const std::string &s);

// char* overloads
nonstd::optional<int> atoi(const char *s);
nonstd::optional<uint32_t> atou(const char *s);
nonstd::optional<int64_t> atoll(const char *s);
nonstd::optional<uint64_t> atoull(const char *s);
nonstd::optional<float> atof_float(const char *s);
nonstd::optional<double> atod(const char *s);

std::string base64_encode(unsigned char const *bytes_to_encode,
                          unsigned int in_len);
std::string base64_decode(std::string const &encoded_string);

///
/// Fast floating-point to string conversion using Dragonbox algorithm
/// - Shortest representation that round-trips correctly
/// - Human-readable format for typical ranges: [1e-4, 1e16) for double, [1e-4, 1e7) for float
/// - Scientific notation for extreme values
/// - Fast path optimization for 1.0 and -1.0 (23x faster)
///
std::string dtos(float v);
std::string dtos(double v);

///
/// Buffer-based version for efficient printing (avoids std::string construction)
/// Writes the string representation to buffer and returns the length
/// Buffer must be at least DTOS_MAX_CHARS_FLOAT/DOUBLE bytes
///
constexpr size_t DTOS_MAX_CHARS_FLOAT = 24;
constexpr size_t DTOS_MAX_CHARS_DOUBLE = 32;

size_t dtos(float v, char* buffer);
size_t dtos(double v, char* buffer);

///
/// Opt-in: format floats/doubles like OpenUSD's usdcat instead of lightusd's
/// default (shortest & robust). Default false = existing behavior.
///
/// OpenUSD (pxr_double_conversion ToShortest/ToShortestSingle) uses fixed
/// notation for decimal exponents in [-6, 15) and scientific outside, with NO
/// `+` sign on positive exponents and NO zero-padding (e.g. `0.0000019073486`,
/// `1e-7`, `1.234e15`). lightusd's default switches to scientific below 1e-4 and
/// pads the exponent to two digits with a sign (`1.9073486e-06`, `e+15`). Both
/// produce the same shortest round-trip digits; only the notation differs.
///
/// Plain global (not thread_local) so parallel-print workers observe it.
///
void SetUSDFloatFormat(bool enable);
bool GetUSDFloatFormat();

// Forward declarations for types (actual definitions in value-types.hh)
namespace value {
struct half;
}  // namespace value (forward declaration only)

///
/// Half-precision float to string conversion using direct dragonbox algorithm
/// Produces shortest representation that uniquely identifies the half value
/// Maximum output length: 16 characters (including null terminator)
///
constexpr size_t DTOS_MAX_CHARS_HALF = 16;

std::string dtos(value::half v);
size_t dtos(value::half v, char* buffer);

// Continue forward declarations
namespace value {
struct matrix2d;
struct matrix3d;
struct matrix4d;

// Vector type aliases (must match value-types.hh)
using float2 = std::array<float, 2>;
using float3 = std::array<float, 3>;
using float4 = std::array<float, 4>;
using double2 = std::array<double, 2>;
using double3 = std::array<double, 3>;
using double4 = std::array<double, 4>;
using half2 = std::array<half, 2>;
using half3 = std::array<half, 3>;
using half4 = std::array<half, 4>;
}  // namespace value

///
/// Maximum buffer sizes for printing vector/matrix types
/// Format: (v0, v1, ..., vN) with separators
///
constexpr size_t PRINT_FLOAT2_MAX_CHARS = 2 * DTOS_MAX_CHARS_FLOAT + 6;   // (f, f)
constexpr size_t PRINT_FLOAT3_MAX_CHARS = 3 * DTOS_MAX_CHARS_FLOAT + 8;   // (f, f, f)
constexpr size_t PRINT_FLOAT4_MAX_CHARS = 4 * DTOS_MAX_CHARS_FLOAT + 10;  // (f, f, f, f)

constexpr size_t PRINT_DOUBLE2_MAX_CHARS = 2 * DTOS_MAX_CHARS_DOUBLE + 6;
constexpr size_t PRINT_DOUBLE3_MAX_CHARS = 3 * DTOS_MAX_CHARS_DOUBLE + 8;
constexpr size_t PRINT_DOUBLE4_MAX_CHARS = 4 * DTOS_MAX_CHARS_DOUBLE + 10;

constexpr size_t PRINT_HALF2_MAX_CHARS = 2 * DTOS_MAX_CHARS_FLOAT + 6;
constexpr size_t PRINT_HALF3_MAX_CHARS = 3 * DTOS_MAX_CHARS_FLOAT + 8;
constexpr size_t PRINT_HALF4_MAX_CHARS = 4 * DTOS_MAX_CHARS_FLOAT + 10;

// Matrix: ((row0), (row1), ...) format
constexpr size_t PRINT_MATRIX2D_MAX_CHARS = 2 * (2 * DTOS_MAX_CHARS_DOUBLE + 6) + 6;   // 2 rows of double2
constexpr size_t PRINT_MATRIX3D_MAX_CHARS = 3 * (3 * DTOS_MAX_CHARS_DOUBLE + 8) + 8;   // 3 rows of double3
constexpr size_t PRINT_MATRIX4D_MAX_CHARS = 4 * (4 * DTOS_MAX_CHARS_DOUBLE + 10) + 10; // 4 rows of double4

///
/// Efficient vector/matrix printing functions
/// Returns number of characters written to buffer
/// Buffer must be at least PRINT_***_MAX_CHARS bytes
///
size_t print_float2(const value::float2& v, char* buffer);
size_t print_float3(const value::float3& v, char* buffer);
size_t print_float4(const value::float4& v, char* buffer);

size_t print_double2(const value::double2& v, char* buffer);
size_t print_double3(const value::double3& v, char* buffer);
size_t print_double4(const value::double4& v, char* buffer);

// Half precision functions - only available when linking with value-types

size_t print_matrix2d(const value::matrix2d& m, char* buffer);
size_t print_matrix3d(const value::matrix3d& m, char* buffer);
size_t print_matrix4d(const value::matrix4d& m, char* buffer);

/// Simple glob pattern matching.
/// Supports * (match any characters) and ? (match single character).
///
/// @param[in] pattern Glob pattern
/// @param[in] str String to match
/// @return true if str matches pattern
///
bool GlobMatch(const std::string &pattern, const std::string &str);

///
/// Glob pattern matching for paths with ** support.
/// ** matches zero or more path segments (including /)
/// * matches any characters except /
/// ? matches single character except /
///
/// @param[in] pattern Glob pattern (e.g., "/Suzanne/**", "/**/Mesh")
/// @param[in] path Path string to match
/// @return true if path matches pattern
///
bool GlobMatchPath(const std::string &pattern, const std::string &path);

///
/// Substitute expression variables in a string.
///
/// Per AOUSD Core Spec 9.5, asset paths may contain `${VAR}` references
/// that are resolved from the layer's `expressionVariables` dictionary.
///
/// Example: "${SHOT_DIR}/geo.usd" with {"SHOT_DIR": "/shots/001"} -> "/shots/001/geo.usd"
///
/// @param[in] input String potentially containing ${VAR} references
/// @param[in] variables Map of variable names to values
/// @return String with all ${VAR} references substituted
///
template <typename Map>
inline std::string SubstituteExpressionVariables(
    const std::string &input,
    const Map &variables) {
  if (input.find("${") == std::string::npos) {
    return input;  // fast path: no variables to substitute
  }

  std::string result;
  result.reserve(input.size());
  size_t i = 0;

  while (i < input.size()) {
    if (i + 1 < input.size() && input[i] == '$' && input[i + 1] == '{') {
      // Find closing brace
      size_t close = input.find('}', i + 2);
      if (close != std::string::npos) {
        std::string varname = input.substr(i + 2, close - i - 2);
        auto it = variables.find(varname);
        if (it != variables.end()) {
          result += it->second;
        } else {
          // Unknown variable: keep original text
          result += input.substr(i, close - i + 1);
        }
        i = close + 1;
      } else {
        // No closing brace: keep as-is
        result += input[i];
        i++;
      }
    } else {
      result += input[i];
      i++;
    }
  }

  return result;
}

}  // namespace legacy
}  // namespace lightusd
