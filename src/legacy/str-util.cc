// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment, Inc.
#include "str-util.hh"

#include "unicode-xid.hh"
#include "common-macros.inc"
#include "value-types.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "external/fast_float/include/fast_float/fast_float.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <cstring>
#include <cstdint>
#include <cmath>
#include <array>
#include <cstdlib>
#include <limits>

// Disable dragonbox's internal asserts for hardened builds
// We handle all special cases (inf, nan, zero) before calling dragonbox
#ifndef NDEBUG
#define LIGHTUSD_DRAGONBOX_NDEBUG_DEFINED
#define NDEBUG
#endif
#include "external/dragonbox/dragonbox_to_chars.h"
#ifdef LIGHTUSD_DRAGONBOX_NDEBUG_DEFINED
#undef NDEBUG
#undef LIGHTUSD_DRAGONBOX_NDEBUG_DEFINED
#endif

// Note: dragonbox_binary16.hh exists for direct fp16 conversion but needs work
// Currently using half→float→dragonbox for correct output

#ifdef __SSE2__
#include <emmintrin.h>
#endif

namespace lightusd {

namespace {

void appendHexEscape(unsigned char value, std::string *out) {
  static constexpr char kHex[] = "0123456789abcdef";
  *out += "\\x";
  *out += kHex[(value >> 4) & 0x0f];
  *out += kHex[value & 0x0f];
}

}  // namespace

std::string buildEscapedAndQuotedStringForUSDA(const std::string &str) {
  // Rule for triple quote string:
  //
  // if str contains newline
  //   if str contains """ and '''
  //      use quote """ and escape " to \\", no escape for '''
  //   elif str contains """ only
  //      use quote ''' and no escape for """
  //   elif str contains ''' only
  //      use quote """ and no escape for '''
  //   else
  //      use quote """
  //
  // Rule for single quote string
  //   if str contains " and '
  //      use quote " and escape " to \\", no escape for '
  //   elif str contains " only
  //      use quote ' and no escape for "
  //   elif str contains ' only
  //      use quote " and no escape for '
  //   else
  //      use quote "

  bool has_newline = hasNewline(str);

  std::string delim;
  bool escape_delimiter = false;

  if (has_newline) {
    bool has_triple_single_quoted_string = hasTripleQuotes(str, false);
    bool has_triple_double_quoted_string = hasTripleQuotes(str, true);

    delim = "\"\"\"";
    if (has_triple_single_quoted_string && has_triple_double_quoted_string) {
      escape_delimiter = true;
    } else if (has_triple_double_quoted_string) {
      delim = "'''";
    }
  } else {
    // single quote string.
    bool has_single_quote = hasQuotes(str, false);
    bool has_double_quote = hasQuotes(str, true);

    delim = "\"";
    if (has_single_quote && has_double_quote) {
      escape_delimiter = true;
    } else if (has_double_quote) {
      delim = "'";
    }
  }

  // Escape the original text in one pass. Escaping quotes first and then
  // escaping backslashes cannot distinguish an inserted quote escape from a
  // literal backslash followed by a quote, and either loses or doubles one of
  // them on roundtrip.
  std::string escaped;
  escaped.reserve(str.size() + 8);
  for (char c : str) {
    const unsigned char value = static_cast<unsigned char>(c);
    if (c == '\\') {
      escaped += "\\\\";
    } else if (c == '\t') {
      escaped += "\\t";
    } else if (c == '\r') {
      escaped += "\\r";
    } else if ((value < 0x20 && c != '\n') || value == 0x7f) {
      appendHexEscape(value, &escaped);
    } else if (escape_delimiter && c == delim[0]) {
      escaped += '\\';
      escaped += c;
    } else {
      escaped += c;
    }
  }

  return quote(escaped, delim);
}

std::string escapeControlSequence(const std::string &str) {
  std::string s;
  s.reserve(str.size());

  for (size_t i = 0; i < str.size(); i++) {
    const char c = str[i];
    const unsigned char value = static_cast<unsigned char>(c);
    if (c == '\t') {
      s += "\\t";
    } else if (c == '\n') {
      s += "\\n";
    } else if (c == '\r') {
      s += "\\r";
    } else if (c == '\\') {
      // Escape backslash as \\ always; the following quote (if any) will be
      // handled by its own case below, producing \\\" for a literal \ followed
      // by ".  The old code emitted just \ (un-escaped) before \", which
      // silently lost the backslash on roundtrip.
      s += "\\\\";
    } else if (value < 0x20 || value == 0x7f) {
      appendHexEscape(value, &s);
    } else {
      s += c;
    }
  }

  return s;
}

std::string unescapeControlSequence(const std::string &str) {
  std::string s;

  if (str.size() < 2) {
    s = str;
  } else {

    for (size_t i = 0; i < str.size(); i++) {
      if (str[i] == '\\') {
        if (i + 1 < str.size()) {
          if (str[i + 1] == 'a') {
            s += '\a';
            i++;
          } else if (str[i + 1] == 'b') {
            s += '\b';
            i++;
          } else if (str[i + 1] == 't') {
            s += '\t';
            i++;
          } else if (str[i + 1] == 'v') {
            s += '\v';
            i++;
          } else if (str[i + 1] == 'f') {
            s += '\f';
            i++;
          } else if (str[i + 1] == 'n') {
            s += '\n';
            i++;
          } else if (str[i + 1] == 'r') {
            s += '\r';
            i++;
          } else if (str[i + 1] == '\\') {
            s += "\\";
            i++;
          } else if (str[i + 1] == 'x') {
            // Hex escape: \xNN
            if (i + 3 < str.size()) {
              char h1 = str[i + 2];
              char h2 = str[i + 3];
              auto hex_digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
              };
              int d1 = hex_digit(h1);
              int d2 = hex_digit(h2);
              if (d1 >= 0 && d2 >= 0) {
                s += static_cast<char>((d1 << 4) | d2);
                i += 3;
              } else {
                // Invalid hex, keep backslash
                s += str[i];
              }
            } else {
              // Not enough characters for \xNN
              s += str[i];
            }
          } else {
            // ignore backslash
          }
        } else {
          // ignore backslash
        }
      } else {
        s += str[i];
      }
    }
  }

  return s;
}

bool hasQuotes(const std::string &str, bool is_double_quote) {
  for (size_t i = 0; i < str.size(); i++) {
    if (is_double_quote) {
      if (str[i] == '"') {
        return true;
      }
    } else {
      if (str[i] == '\'') {
        return true;
      }
    }
  }

  return false;
}

bool hasTripleQuotes(const std::string &str, bool is_double_quote) {
  for (size_t i = 0; i < str.size(); i++) {
    if (i + 3 < str.size()) {
      if (is_double_quote) {
        if ((str[i + 0] == '"') && (str[i + 1] == '"') && (str[i + 2] == '"')) {
          return true;
        }
      } else {
        if ((str[i + 0] == '\'') && (str[i + 1] == '\'') &&
            (str[i + 2] == '\'')) {
          return true;
        }
      }
    }
  }

  return false;
}

bool hasEscapedTripleQuotes(const std::string &str, bool is_double_quote,
                            size_t *n) {
  size_t count = 0;

  for (size_t i = 0; i < str.size(); i++) {
    if (str[i] == '\\') {
      if (i + 3 < str.size()) {
        if (is_double_quote) {
          if ((str[i + 1] == '"') && (str[i + 2] == '"') &&
              (str[i + 3] == '"')) {
            if (!n) {  // early exit
              return true;
            }

            count++;
            i += 3;
          }
        } else {
          if ((str[i + 1] == '\'') && (str[i + 2] == '\'') &&
              (str[i + 3] == '\'')) {
            if (!n) {  // early exit
              return true;
            }
            count++;
            i += 3;
          }
        }
      }
    }
  }

  if (n) {
    (*n) = count;
  }

  return count > 0;
}

std::string escapeSingleQuote(const std::string &str,
                              const bool is_double_quote) {
  std::string s;

  if (is_double_quote) {
    for (size_t i = 0; i < str.size(); i++) {
      if (str[i] == '"') {
        s += "\\\"";
      } else {
        s += str[i];
      }
    }
  } else {
    for (size_t i = 0; i < str.size(); i++) {
      if (str[i] == '\'') {
        s += "\\'";
      } else {
        s += str[i];
      }
    }
  }

  return s;
}

std::string escapeBackslash(const std::string &str,
                            const bool triple_quoted_string) {
  if (triple_quoted_string) {
    std::string s;

    // Do not escape \""" or \'''

    for (size_t i = 0; i < str.size(); i++) {
      if (str[i] == '\\') {
        if (i + 3 < str.size()) {
          if ((str[i + 1] == '\'') && (str[i + 2] == '\'') &&
              (str[i + 3] == '\'')) {
            s += "\\'''";
            i += 3;
          } else if ((str[i + 1] == '"') && (str[i + 2] == '"') &&
                     (str[i + 3] == '"')) {
            s += "\\\"\"\"";
            i += 3;
          } else {
            s += "\\\\";
          }
        } else {
          s += "\\\\";
        }
      } else {
        s += str[i];
      }
    }

    return s;
  } else {
    const std::string bs = "\\";
    const std::string bs_escaped = "\\\\";

    std::string s = str;

    std::string::size_type pos = 0;
    while ((pos = s.find(bs, pos)) != std::string::npos) {
      s.replace(pos, bs.length(), bs_escaped);
      pos += bs_escaped.length();
    }

    return s;
  }
}

std::string unescapeBackslash(const std::string &str) {
  std::string s = str;

  std::string bs = "\\\\";
  std::string bs_unescaped = "\\";

  std::string::size_type pos = 0;
  while ((pos = s.find(bs, pos)) != std::string::npos) {
    s.replace(pos, bs.length(), bs_unescaped);
    pos += bs_unescaped.length();
  }

  return s;
}

bool tokenize_variantElement(const std::string &elementName,
                             std::array<std::string, 2> *result) {
  std::vector<std::string> toks;

  // Ensure ElementPath is quoted with '{' and '}'
  if (startsWith(elementName, "{") && endsWith(elementName, "}")) {
    // ok
  } else {
    return false;
  }

  // Remove variant quotation
  std::string name = unwrap(elementName, "{", "}");

  toks = split(name, "=");
  if (toks.size() == 1) {
    if (result) {
      // ensure '=' and newline does not exist.
      if (counts(toks[0], '=') || hasNewline(toks[0])) {
        return false;
      }

      (*result)[0] = toks[0];
      (*result)[1] = std::string();
    }
    return true;
  } else if (toks.size() == 2) {
    if (result) {
      // ensure '=' and newline does not exist.
      if (counts(toks[0], '=') || hasNewline(toks[0])) {
        return false;
      }

      if (counts(toks[1], '=') || hasNewline(toks[1])) {
        return false;
      }

      (*result)[0] = toks[0];
      (*result)[1] = toks[1];
    }
    return true;
  } else {
    return false;
  }
}

bool is_variantElementName(const std::string &name) {
  return tokenize_variantElement(name);
}

///
/// Simply add number suffix to make unique string.
///
/// - plane -> plane1
/// - sphere1 -> sphere11
/// - xform4 -> xform41
///
///
bool makeUniqueName(std::multiset<std::string> &nameSet,
                    const std::string &name, std::string *unique_name) {
  if (!unique_name) {
    return false;
  }

  if (nameSet.count(name) == 0) {
    (*unique_name) = name;
    return true;
  }

  // Simply add number

  const size_t kMaxLoop = 1024;  // to avoid infinite loop.

  std::string new_name = name;

  size_t cnt = 0;
  while (cnt < kMaxLoop) {
    size_t i = nameSet.count(new_name);
    if (i == 0) {
      // This should not happen though.
      return false;
    }

    new_name += std::to_string(i);

    if (nameSet.count(new_name) == 0) {
      (*unique_name) = new_name;
      return true;
    }

    cnt++;
  }

  return false;
}

namespace detail {

inline uint32_t utf8_len(const unsigned char c) {
      if (c <= 127) {
        // ascii
        return 1;
      } else if ((c & 0xE0) == 0xC0) {
        return 2;
      } else if ((c & 0xF0) == 0xE0) {
        return 3;
      } else if ((c & 0xF8) == 0xF0) {
        return 4;
      }

      // invalid
      return 0;
}

inline std::string extract_utf8_char(const std::string &str, uint32_t start_i,
                                     int &len) {
  len = 0;

  if ((start_i + 1) > str.size()) {
    len = 0;
    return std::string();
  }

  unsigned char c = static_cast<unsigned char>(str[start_i]);

  if (c <= 127) {
    // ascii
    len = 1;
    return str.substr(start_i, 1);
  } else if ((c & 0xE0) == 0xC0) {
    if ((start_i + 2) > str.size()) {
      len = 0;
      return std::string();
    }
    len = 2;
    return str.substr(start_i, 2);
  } else if ((c & 0xF0) == 0xE0) {
    if ((start_i + 3) > str.size()) {
      len = 0;
      return std::string();
    }
    len = 3;
    return str.substr(start_i, 3);
  } else if ((c & 0xF8) == 0xF0) {
    if ((start_i + 4) > str.size()) {
      len = 0;
      return std::string();
    }
    len = 4;
    return str.substr(start_i, 4);
  } else {
    // invalid utf8
    len = 0;
    return std::string();
  }
}

inline uint32_t to_codepoint(const char *s, uint32_t &char_len) {
  if (!s) {
    char_len = 0;
    return ~0u;
  }

  char_len = detail::utf8_len(static_cast<unsigned char>(s[0]));
  if (char_len == 0) {
    return ~0u;
  }

  uint32_t code = 0;
  if (char_len == 1) {
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    if (s0 > 0x7f) {
      return ~0u;
    }
    code = uint32_t(s0) & 0x7f;
  } else if (char_len == 2) {
    // 11bit: 110y-yyyx 10xx-xxxx
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    unsigned char s1 = static_cast<unsigned char>(s[1]);

    if (((s0 & 0xe0) == 0xc0) && ((s1 & 0xc0) == 0x80)) {
      code = (uint32_t(s0 & 0x1f) << 6) | (s1 & 0x3f);
    } else {
      return ~0u;
    }
  } else if (char_len == 3) {
    // 16bit: 1110-yyyy 10yx-xxxx 10xx-xxxx
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    unsigned char s1 = static_cast<unsigned char>(s[1]);
    unsigned char s2 = static_cast<unsigned char>(s[2]);
    if (((s0 & 0xf0) == 0xe0) && ((s1 & 0xc0) == 0x80) &&
        ((s2 & 0xc0) == 0x80)) {
      code =
          (uint32_t(s0 & 0xf) << 12) | (uint32_t(s1 & 0x3f) << 6) | (s2 & 0x3f);
    } else {
      return ~0u;
    }
  } else if (char_len == 4) {
    // 21bit: 1111-0yyy 10yy-xxxx 10xx-xxxx 10xx-xxxx
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    unsigned char s1 = static_cast<unsigned char>(s[1]);
    unsigned char s2 = static_cast<unsigned char>(s[2]);
    unsigned char s3 = static_cast<unsigned char>(s[3]);
    if (((s0 & 0xf8) == 0xf0) && ((s1 & 0xc0) == 0x80) &&
        ((s2 & 0xc0) == 0x80) && ((s3 & 0xc0) == 0x80)) {
      code = (uint32_t(s0 & 0x7) << 18) | (uint32_t(s1 & 0x3f) << 12) |
             (uint32_t(s2 & 0x3f) << 6) | uint32_t(s3 & 0x3f);
    } else {
      return ~0u;
    }
  } else {
    // ???
    char_len = 0;
    return ~0u;
  }

  return code;
}

}  // namespace detail

std::vector<std::string> to_utf8_chars(const std::string &str) {
  std::vector<std::string> utf8_chars;
  size_t sz = str.size();

  for (size_t i = 0; i < sz;) {
    int len = 0;
    std::string s = detail::extract_utf8_char(str, uint32_t(i), len);
    if (len == 0) {
      // invalid char
      //return std::vector<std::string>();
      utf8_chars = std::vector<std::string>();
      break;
    }

    i += uint64_t(len);
    utf8_chars.push_back(s);
  }

  return utf8_chars;
}

uint32_t to_utf8_code(const std::string &s) {
  if (s.empty() || (s.size() > 4)) {
    return ~0u;  // invalid
  }

  // TODO: endianness.
  uint32_t code = 0;
  if (s.size() == 1) {
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    if (s0 > 0x7f) {
      return ~0u;
    }
    code = uint32_t(s0) & 0x7f;
  } else if (s.size() == 2) {
    // 11bit: 110y-yyyx 10xx-xxxx
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    unsigned char s1 = static_cast<unsigned char>(s[1]);

    if (((s0 & 0xe0) == 0xc0) && ((s1 & 0xc0) == 0x80)) {
      code = (uint32_t(s0 & 0x1f) << 6) | (s1 & 0x3f);
    } else {
      return ~0u;
    }
  } else if (s.size() == 3) {
    // 16bit: 1110-yyyy 10yx-xxxx 10xx-xxxx
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    unsigned char s1 = static_cast<unsigned char>(s[1]);
    unsigned char s2 = static_cast<unsigned char>(s[2]);
    if (((s0 & 0xf0) == 0xe0) && ((s1 & 0xc0) == 0x80) &&
        ((s2 & 0xc0) == 0x80)) {
      code =
          (uint32_t(s0 & 0xf) << 12) | (uint32_t(s1 & 0x3f) << 6) | (s2 & 0x3f);
    } else {
      return ~0u;
    }
  } else {
    // 21bit: 1111-0yyy 10yy-xxxx 10xx-xxxx 10xx-xxxx
    unsigned char s0 = static_cast<unsigned char>(s[0]);
    unsigned char s1 = static_cast<unsigned char>(s[1]);
    unsigned char s2 = static_cast<unsigned char>(s[2]);
    unsigned char s3 = static_cast<unsigned char>(s[3]);
    if (((s0 & 0xf8) == 0xf0) && ((s1 & 0xc0) == 0x80) &&
        ((s2 & 0xc0) == 0x80) && ((s3 & 0xc0) == 0x80)) {
      code = (uint32_t(s0 & 0x7) << 18) | (uint32_t(s1 & 0x3f) << 12) |
             (uint32_t(s2 & 0x3f) << 6) | uint32_t(s3 & 0x3f);
    } else {
      return ~0u;
    }
  }

  return code;
}



bool is_valid_utf8(const std::string &str) {
  // TODO: Consider UTF-BOM?
  for (size_t i = 0; i < str.size();) {
    uint32_t len = detail::utf8_len(*reinterpret_cast<const unsigned char *>(&str[i]));
    if (len == 0) {
      return false;
    }
    i += len;
  }
  return true;
}

std::vector<uint32_t> to_codepoints(const std::string &str) {

  std::vector<uint32_t> cps;

  for (size_t i = 0; i < str.size(); ) {
    uint32_t char_len;
    uint32_t cp = detail::to_codepoint(str.c_str() + i, char_len);

    if ((cp > kMaxUTF8Codepoint) || (char_len == 0)) {
      cps = std::vector<uint32_t>();
      break;
      //return std::vector<uint32_t>();
    }

    cps.push_back(cp);

    i += char_len;
  }

  return cps;
}

bool is_valid_utf8_identifier(const std::string &str) {
  // First convert to codepoint values.
  std::vector<uint32_t> codepoints = to_codepoints(str);

  if (codepoints.empty()) {
    return false;
  }

  // (XID_Start|_) (XID_Continue|_)+

  if ((codepoints[0] != '_') && !unicode_xid::is_xid_start(codepoints[0])) {
    return false;
  }

  for (size_t i = 1; i < codepoints.size(); i++) {
    if ((codepoints[i] != '_') && !unicode_xid::is_xid_continue(codepoints[i])) {
      return false;
    }
  }

  return true;
}

std::string makeIdentifierValid(const std::string &str, bool is_utf8) {
  // TODO: utf8 support
  (void)is_utf8;

  std::string s;

  if (str.empty()) {
    // return '_'
    s =  "_";
  } else {

    // first char
    // [a-ZA-Z_]
    if ((('a' <= str[0]) && (str[0] <= 'z')) || (('A' <= str[0]) && (str[0] <= 'Z')) || (str[0] == '_')) {
      s.push_back(str[0]);
    } else {
      s.push_back('_');
    }

    // remain chars
    // [a-ZA-Z0-9_]
    for (size_t i = 1; i < str.length(); i++) {
      if ((('a' <= str[i]) && (str[i] <= 'z')) || (('A' <= str[i]) && (str[i] <= 'Z')) || (('0' <= str[i]) && (str[i] <= '9')) || (str[i] == '_')) {
        s.push_back(str[i]);
      } else {
        s.push_back('_');
      }
    }
  }

  return s;
}

double atof(const char *p) {
  // TODO: Use from_chars
  return std::atof(p);
}

double atof(const std::string &s) {
  return atof(s.c_str());
}

//
// Safe integer/double parse helpers (replacement for std::st*)
// No exceptions - returns nullopt on error
//

nonstd::optional<int> atoi(const std::string &s) {
  return atoi(s.c_str());
}

namespace {

inline bool IsParseSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
         c == '\f' || c == '\v';
}

nonstd::optional<uint64_t> ParseUnsignedMagnitude(const char *s, bool *negative, const char **end) {
  if (!s) {
    return nonstd::nullopt;
  }

  while (IsParseSpace(*s)) {
    ++s;
  }

  if (*s == '\0') {
    return nonstd::nullopt;
  }

  bool is_negative = false;

  if (*s == '-') {
    is_negative = true;
    ++s;
  } else if (*s == '+') {
    ++s;
  }

  if (*s == '\0') {
    return nonstd::nullopt;
  }

  uint64_t result = 0;
  const char *p = s;
  const uint64_t positive_limit = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
  const uint64_t negative_limit = positive_limit + 1u;
  const uint64_t limit = is_negative ? negative_limit : positive_limit;

  while (*p >= '0' && *p <= '9') {
    const uint64_t digit = static_cast<uint64_t>(*p - '0');

    if (result > limit / 10u || (result == limit / 10u && digit > limit % 10u)) {
      return nonstd::nullopt;
    }
    result *= 10;
    result += digit;
    ++p;
  }

  if (p == s) {
    return nonstd::nullopt;
  }

  *negative = is_negative;
  *end = p;
  return result;
}

nonstd::optional<uint64_t> ParseUnsignedInteger(const char *s,
                                                uint64_t limit,
                                                const char **end) {
  if (!s) {
    return nonstd::nullopt;
  }

  while (IsParseSpace(*s)) {
    ++s;
  }

  if (*s == '\0') {
    return nonstd::nullopt;
  }

  if (*s == '+') {
    ++s;
  } else if (*s == '-') {
    return nonstd::nullopt;
  }

  if (*s == '\0') {
    return nonstd::nullopt;
  }

  uint64_t result = 0;
  const char *p = s;
  while (*p >= '0' && *p <= '9') {
    const uint64_t digit = static_cast<uint64_t>(*p - '0');
    if (result > limit / 10u || (result == limit / 10u && digit > limit % 10u)) {
      return nonstd::nullopt;
    }
    result *= 10u;
    result += digit;
    ++p;
  }

  if (p == s) {
    return nonstd::nullopt;
  }

  *end = p;
  return result;
}

inline bool HasTrailingNonSpace(const char *p) {
  while (*p != '\0') {
    if (!IsParseSpace(*p)) {
      return true;
    }
    ++p;
  }
  return false;
}

}  // namespace

nonstd::optional<int> atoi(const char *s) {
  bool negative = false;
  const char *end = nullptr;
  nonstd::optional<uint64_t> magnitude = ParseUnsignedMagnitude(s, &negative, &end);
  if (!magnitude.has_value() || HasTrailingNonSpace(end)) {
    return nonstd::nullopt;
  }

  if (negative) {
    if (magnitude.value() > static_cast<uint64_t>(std::numeric_limits<int>::max()) + 1u) {
      return nonstd::nullopt;
    }
    int64_t val = -static_cast<int64_t>(magnitude.value());
    if (val < std::numeric_limits<int>::min()) {
      return nonstd::nullopt;
    }
    return static_cast<int>(val);
  } else {
    if (magnitude.value() > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
      return nonstd::nullopt;
    }
    return static_cast<int>(magnitude.value());
  }
}

nonstd::optional<uint32_t> atou(const std::string &s) {
  return atou(s.c_str());
}

nonstd::optional<uint32_t> atou(const char *s) {
  const char *end = nullptr;
  nonstd::optional<uint64_t> value = ParseUnsignedInteger(
      s, static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()), &end);
  if (!value.has_value() || HasTrailingNonSpace(end)) {
    return nonstd::nullopt;
  }
  return static_cast<uint32_t>(value.value());
}

nonstd::optional<int64_t> atoll(const std::string &s) {
  return atoll(s.c_str());
}

nonstd::optional<int64_t> atoll(const char *s) {
  bool negative = false;
  const char *end = nullptr;
  nonstd::optional<uint64_t> magnitude = ParseUnsignedMagnitude(s, &negative, &end);
  if (!magnitude.has_value() || HasTrailingNonSpace(end)) {
    return nonstd::nullopt;
  }

  if (negative) {
    const uint64_t min_magnitude = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) + 1u;
    if (magnitude.value() == min_magnitude) {
      return (std::numeric_limits<int64_t>::min)();
    }
    return -static_cast<int64_t>(magnitude.value());
  }
  return static_cast<int64_t>(magnitude.value());
}

nonstd::optional<uint64_t> atoull(const std::string &s) {
  return atoull(s.c_str());
}

nonstd::optional<uint64_t> atoull(const char *s) {
  const char *end = nullptr;
  nonstd::optional<uint64_t> value =
      ParseUnsignedInteger(s, (std::numeric_limits<uint64_t>::max)(), &end);
  if (!value.has_value() || HasTrailingNonSpace(end)) {
    return nonstd::nullopt;
  }
  return value.value();
}

nonstd::optional<float> atof_float(const std::string &s) {
  return atof_float(s.c_str());
}

nonstd::optional<float> atof_float(const char *s) {
  if (!s) {
    return nonstd::nullopt;
  }
  while (IsParseSpace(*s)) {
    ++s;
  }
  float result = 0.0f;
  const char *end = s + strlen(s);
  auto r = fast_float::from_chars(s, end, result);
  if (r.ec == std::errc{} && std::isfinite(result) &&
      !HasTrailingNonSpace(r.ptr)) {
    return result;
  }
  return nonstd::nullopt;
}

nonstd::optional<double> atod(const std::string &s) {
  return atod(s.c_str());
}

nonstd::optional<double> atod(const char *s) {
  if (!s) {
    return nonstd::nullopt;
  }
  while (IsParseSpace(*s)) {
    ++s;
  }
  double result = 0.0;
  const char *end = s + strlen(s);
  auto r = fast_float::from_chars(s, end, result);
  if (r.ec == std::errc{} && std::isfinite(result) &&
      !HasTrailingNonSpace(r.ptr)) {
    return result;
  }
  return nonstd::nullopt;
}

/*
   base64.cpp and base64.h

   Copyright (C) 2004-2008 René Nyffenegger

   This source code is provided 'as-is', without any express or implied
   warranty. In no event will the author be held liable for any damages
   arising from the use of this software.

   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it
   freely, subject to the following restrictions:

   1. The origin of this source code must not be misrepresented; you must not
      claim that you wrote the original source code. If you use this source code
      in a product, an acknowledgment in the product documentation would be
      appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
      misrepresented as being the original source code.

   3. This notice may not be removed or altered from any source distribution.

   René Nyffenegger rene.nyffenegger@adp-gmbh.ch

*/

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wconversion"
#endif

#ifdef __SSE2__
#else
static inline bool is_base64(unsigned char c) {
  return (isalnum(c) || (c == '+') || (c == '/'));
}
#endif

#ifdef __SSE2__
#else
// Fallback implementation (original)
static std::string base64_encode_scalar(unsigned char const *bytes_to_encode,
                                       unsigned int in_len) {
  std::string ret;
  int i = 0;
  int j = 0;
  unsigned char char_array_3[3];
  unsigned char char_array_4[4];

  const char *base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";

  while (in_len--) {
    char_array_3[i++] = *(bytes_to_encode++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] =
          ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] =
          ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for (i = 0; (i < 4); i++) ret += base64_chars[char_array_4[i]];
      i = 0;
    }
  }

  if (i) {
    for (j = i; j < 3; j++) char_array_3[j] = '\0';

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] =
        ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] =
        ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

    for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];

    while ((i++ < 3)) ret += '=';
  }

  return ret;
}
#endif

// SSE2-optimized base64 encode implementation
#ifdef __SSE2__
static std::string base64_encode_sse(unsigned char const *bytes_to_encode, unsigned int in_len) {
  if (in_len == 0) return std::string();

  const char base64_chars[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'
  };

  // Calculate output size
  const size_t output_len = ((in_len + 2) / 3) * 4;
  std::string result;
  result.reserve(output_len);

  size_t input_pos = 0;

  // Process 12 bytes at a time using SSE2 (produces 16 base64 characters)
  while (input_pos + 12 <= in_len) {
    // Load 12 input bytes (will process as 4 groups of 3 bytes each)
    alignas(16) uint8_t input_block[16] = {0};

    // Copy 12 bytes, leaving last 4 bytes as zero padding
    for (int i = 0; i < 12; i++) {
      input_block[i] = bytes_to_encode[input_pos + i];
    }

    // Load input data into SSE register (currently unused but reserved for future vectorization)
    (void)_mm_load_si128(reinterpret_cast<const __m128i*>(input_block));

    // Process 4 groups of 3 bytes each
    alignas(16) uint8_t output_indices[16];

    for (int group = 0; group < 4; group++) {
      int base_idx = group * 3;

      // Extract 3 bytes for this group
      uint8_t b0 = input_block[base_idx];
      uint8_t b1 = input_block[base_idx + 1];
      uint8_t b2 = input_block[base_idx + 2];

      // Convert 3 bytes to 4 base64 indices
      output_indices[group * 4] = (b0 >> 2) & 0x3F;
      output_indices[group * 4 + 1] = ((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F);
      output_indices[group * 4 + 2] = ((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03);
      output_indices[group * 4 + 3] = b2 & 0x3F;
    }

    // Convert indices to base64 characters using table lookup
    for (int i = 0; i < 16; i++) {
      result.push_back(base64_chars[output_indices[i]]);
    }

    input_pos += 12;
  }

  // Handle remaining bytes with scalar code
  while (input_pos + 3 <= in_len) {
    uint8_t b0 = bytes_to_encode[input_pos];
    uint8_t b1 = bytes_to_encode[input_pos + 1];
    uint8_t b2 = bytes_to_encode[input_pos + 2];

    result.push_back(base64_chars[(b0 >> 2) & 0x3F]);
    result.push_back(base64_chars[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)]);
    result.push_back(base64_chars[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)]);
    result.push_back(base64_chars[b2 & 0x3F]);

    input_pos += 3;
  }

  // Handle final 1-2 bytes if present
  if (input_pos < in_len) {
    uint8_t b0 = bytes_to_encode[input_pos];
    uint8_t b1 = (input_pos + 1 < in_len) ? bytes_to_encode[input_pos + 1] : 0;

    result.push_back(base64_chars[(b0 >> 2) & 0x3F]);
    result.push_back(base64_chars[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)]);

    if (input_pos + 1 < in_len) {
      result.push_back(base64_chars[((b1 & 0x0F) << 2)]);
    } else {
      result.push_back('=');
    }
    result.push_back('=');
  }

  return result;
}
#endif // __SSE2__

std::string base64_encode(unsigned char const *bytes_to_encode,
                          unsigned int in_len) {
#ifdef __SSE2__
  // Use SSE2 optimized version if available
  return base64_encode_sse(bytes_to_encode, in_len);
#else
  // Use scalar fallback implementation
  return base64_encode_scalar(bytes_to_encode, in_len);
#endif
}

// SSE2-optimized base64 decode implementation
#ifdef __SSE2__
static std::string base64_decode_sse(std::string const &encoded_string) {
  const size_t input_len = encoded_string.size();
  if (input_len == 0) return std::string();

  // Lookup table for base64 decoding (256 entries, -1 for invalid chars)
  static const int8_t decode_table[256] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-2,-1,-1,
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1
  };

  // Calculate output size (remove padding)
  size_t padding = 0;
  if (input_len >= 1 && encoded_string[input_len - 1] == '=') padding++;
  if (input_len >= 2 && encoded_string[input_len - 2] == '=') padding++;

  const size_t output_len = (input_len * 3) / 4 - padding;
  std::string result;
  result.reserve(output_len);

  const uint8_t* input = reinterpret_cast<const uint8_t*>(encoded_string.data());
  size_t input_pos = 0;

  // Process 16 bytes at a time using SSE2
  while (input_pos + 16 <= input_len) {
    // Load 16 input bytes
    __m128i input_chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + input_pos));

    // Decode using lookup table (split into two 8-byte chunks for table lookup)
    alignas(16) uint8_t input_bytes[16];
    _mm_store_si128(reinterpret_cast<__m128i*>(input_bytes), input_chunk);

    alignas(16) int8_t decoded[16];
    bool valid = true;

    for (int i = 0; i < 16; i++) {
      decoded[i] = decode_table[input_bytes[i]];
      if (decoded[i] < 0 && input_bytes[i] != '=') {
        valid = false;
        break;
      }
    }

    if (!valid) break; // Fall back to scalar processing for invalid chars

    // Pack groups of 4 decoded bytes into 3 output bytes
    for (int group = 0; group < 4; group++) {
      if (input_pos + group * 4 + 3 >= input_len) break;

      int base_idx = group * 4;
      if (decoded[base_idx] >= 0 && decoded[base_idx + 1] >= 0 &&
          decoded[base_idx + 2] >= 0 && decoded[base_idx + 3] >= 0) {

        uint32_t combined = (static_cast<uint32_t>(decoded[base_idx]) << 18) |
                           (static_cast<uint32_t>(decoded[base_idx + 1]) << 12) |
                           (static_cast<uint32_t>(decoded[base_idx + 2]) << 6) |
                           static_cast<uint32_t>(decoded[base_idx + 3]);

        result.push_back(static_cast<char>((combined >> 16) & 0xFF));
        result.push_back(static_cast<char>((combined >> 8) & 0xFF));
        result.push_back(static_cast<char>(combined & 0xFF));
      }
    }

    input_pos += 16;
  }

  // Process remaining bytes with scalar code
  while (input_pos + 4 <= input_len) {
    uint8_t a = input[input_pos];
    uint8_t b = input[input_pos + 1];
    uint8_t c = input[input_pos + 2];
    uint8_t d = input[input_pos + 3];

    if (a == '=' || b == '=') break;

    int8_t da = decode_table[a];
    int8_t db = decode_table[b];
    int8_t dc = decode_table[c];
    int8_t dd = decode_table[d];

    if (da < 0 || db < 0) break;

    uint32_t combined = (static_cast<uint32_t>(da) << 18) |
                       (static_cast<uint32_t>(db) << 12);

    result.push_back(static_cast<char>((combined >> 16) & 0xFF));

    if (c != '=' && dc >= 0) {
      combined |= static_cast<uint32_t>(dc) << 6;
      result.push_back(static_cast<char>((combined >> 8) & 0xFF));

      if (d != '=' && dd >= 0) {
        combined |= static_cast<uint32_t>(dd);
        result.push_back(static_cast<char>(combined & 0xFF));
      }
    }

    input_pos += 4;
  }

  return result;
}
#endif // __SSE2__

// Fallback implementation (original)
std::string base64_decode(std::string const &encoded_string) {
#ifdef __SSE2__
  // Use SSE2 optimized version if available
  return base64_decode_sse(encoded_string);
#else
  // Original scalar implementation
  int in_len = static_cast<int>(encoded_string.size());
  int i = 0;
  int j = 0;
  int in_ = 0;
  unsigned char char_array_4[4], char_array_3[3];
  std::string ret;

  const std::string base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";

  while (in_len-- && (encoded_string[in_] != '=') &&
         is_base64(encoded_string[in_])) {
    char_array_4[i++] = encoded_string[in_];
    in_++;
    if (i == 4) {
      for (i = 0; i < 4; i++)
        char_array_4[i] =
            static_cast<unsigned char>(base64_chars.find(char_array_4[i]));

      char_array_3[0] =
          (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
      char_array_3[1] =
          ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
      char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

      for (i = 0; (i < 3); i++) ret += char_array_3[i];
      i = 0;
    }
  }

  if (i) {
    for (j = i; j < 4; j++) char_array_4[j] = 0;

    for (j = 0; j < 4; j++)
      char_array_4[j] =
          static_cast<unsigned char>(base64_chars.find(char_array_4[j]));

    char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
    char_array_3[1] =
        ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
    char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

    for (j = 0; (j < i - 1); j++) ret += char_array_3[j];
  }

  return ret;
#endif // __SSE2__
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

/*
   -- end base64.cpp and base64.h
*/

// ============================================================================
// dtoa_dragonbox - Fast floating-point to string conversion
// Based on Dragonbox algorithm by Junekey Jeon
// ============================================================================

namespace {

// Opt-in OpenUSD-compatible float notation (see SetUSDFloatFormat). Read by the
// dragonbox formatter below. Plain global so parallel-print workers observe it.
bool g_usd_float_format = false;

// Helper functions for dragonbox formatting
template <typename T>
inline int count_digits(T n) {
  int count = 1;
  for (;;) {
    if (n < 10) return count;
    if (n < 100) return count + 1;
    if (n < 1000) return count + 2;
    if (n < 10000) return count + 3;
    n /= 10000u;
    count += 4;
  }
}

inline auto digits2(size_t value) -> const char* {
  alignas(2) static const char data[] =
      "0001020304050607080910111213141516171819"
      "2021222324252627282930313233343536373839"
      "4041424344454647484950515253545556575859"
      "6061626364656667686970717273747576777879"
      "8081828384858687888990919293949596979899";
  return &data[value * 2];
}

inline void write2digits(char* out, size_t value) {
  *out++ = static_cast<char>('0' + value / 10);
  *out = static_cast<char>('0' + value % 10);
}

char* write_exponent(int exp, char* out) {
  if (exp < 0) {
    *out++ = '-';
    exp = -exp;
  } else if (!g_usd_float_format) {
    // OpenUSD omits the '+' on positive exponents; the default emits it.
    *out++ = '+';
  }
  auto uexp = static_cast<uint32_t>(exp);

  if (g_usd_float_format) {
    // OpenUSD: minimal digits, no zero padding (e.g. `e-7`, `e15`).
    char tmp[8];
    int n = 0;
    if (uexp == 0) {
      tmp[n++] = '0';
    }
    while (uexp) {
      tmp[n++] = static_cast<char>('0' + (uexp % 10u));
      uexp /= 10u;
    }
    for (int i = n - 1; i >= 0; --i) *out++ = tmp[i];
    return out;
  }

  // Default: zero-pad the exponent to (at least) two digits.
  if (uexp >= 100u) {
    const char* top = digits2(uexp / 100);
    if (uexp >= 1000u) *out++ = top[0];
    *out++ = static_cast<char>(top[1]);
    uexp %= 100;
  }
  const char* d = digits2(uexp);
  *out++ = static_cast<char>(d[0]);
  *out++ = static_cast<char>(d[1]);
  return out;
}

inline char* fill_n(char* p, int n, char c) {
  for (int i = 0; i < n; i++, p++) {
    *p = c;
  }
  return p;
}

inline void format_decimal_impl(char* out, uint64_t value, uint32_t size) {
  unsigned n = size;
  while (value >= 100) {
    n -= 2;
    write2digits(out + n, static_cast<unsigned>(value % 100));
    value /= 100;
  }
  if (value >= 10) {
    n -= 2;
    write2digits(out + n, static_cast<unsigned>(value));
  } else {
    out[--n] = static_cast<char>('0' + value);
  }
}

inline char* format_decimal(char* out, uint64_t value, uint32_t num_digits) {
  format_decimal_impl(out, value, num_digits);
  return out + num_digits;
}

inline char* write_significand_e(char* out, uint64_t significand,
                                 int significand_size, int exponent) {
  out = format_decimal(out, significand, static_cast<uint32_t>(significand_size));
  return fill_n(out, exponent, '0');
}

inline char* write_significand(char* out, uint64_t significand,
                               int significand_size, int integral_size,
                               char decimal_point) {
  if (!decimal_point) return format_decimal(out, significand, static_cast<uint32_t>(significand_size));
  out += significand_size + 1;
  char* end = out;
  int floating_size = significand_size - integral_size;
  for (int i = floating_size / 2; i > 0; --i) {
    out -= 2;
    write2digits(out, static_cast<std::size_t>(significand % 100));
    significand /= 100;
  }
  if (floating_size % 2 != 0) {
    *--out = static_cast<char>('0' + significand % 10);
    significand /= 10;
  }
  *--out = decimal_point;
  format_decimal(out - integral_size, significand, static_cast<uint32_t>(integral_size));
  return end;
}

// Template implementation for both float and double
// max_digits: maximum significant digits (9 for float, 17 for double)
// exp_upper: threshold for switching to exponential notation
// Returns nullptr on invalid input (null buffer)
template<typename Float>
char* dtoa_dragonbox_impl_t(const Float f, char* buf, int max_digits, int exp_upper) {
  // Null buffer check - return nullptr for invalid input
  if (!buf) {
    return nullptr;
  }

  // Handle special cases first (dragonbox requires finite, non-zero values)
  if (std::isnan(f)) {
    std::memcpy(buf, "nan", 3);
    return buf + 3;
  }
  if (std::isinf(f)) {
    if (std::signbit(f)) {
      std::memcpy(buf, "-inf", 4);
      return buf + 4;
    } else {
      std::memcpy(buf, "inf", 3);
      return buf + 3;
    }
  }

  bool is_negative = std::signbit(f);

  // Handle zero specially (use fpclassify to avoid float comparison warning)
  if (std::fpclassify(f) == FP_ZERO) {
    // Preserve authored IEEE-754 signed zero independently of the optional
    // OpenUSD notation mode. Both spellings are valid USD, and `-0` is the
    // shortest spelling that retains the sign on parse.
    if (std::signbit(f)) {
      *buf++ = '-';
    }
    *buf++ = '0';
    return buf;
  }

  // Final safety guard: ensure value is finite before calling dragonbox
  // This should never trigger given the checks above, but provides defense in depth
  if (!std::isfinite(f)) {
    // Fallback for any unexpected non-finite value
    std::memcpy(buf, "0", 1);
    return buf + 1;
  }

  // Use dragonbox directly on the original type (float or double)
  auto ret = jkj::dragonbox::to_decimal(f);

  // print human-readable float for the value in range [1e-exp_lower, 1e+exp_upper]
  // OpenUSD-compat (pxr ToShortest/ToShortestSingle): fixed for decimal
  // exponents in [-6, 15) regardless of float vs double, scientific outside.
  int exp_lower = -4;
  if (g_usd_float_format) {
    exp_lower = -6;
    exp_upper = 15;
  }
  char exp_char = 'e';
  char zero_char = '0';

  auto significand = ret.significand;
  int significand_size = count_digits(significand);
  int exponent = ret.exponent;

  // Limit to max_digits significant digits (round half to even)
  if (significand_size > max_digits) {
    int digits_to_remove = significand_size - max_digits;
    uint64_t divisor = 1;
    for (int i = 0; i < digits_to_remove; i++) {
      divisor *= 10;
    }
    uint64_t remainder = significand % divisor;
    significand /= divisor;
    exponent += digits_to_remove;

    // Round half to even
    uint64_t half = divisor / 2;
    if (remainder > half || (remainder == half && (significand & 1))) {
      significand++;
      // Check if rounding caused overflow (e.g., 999 -> 1000)
      if (count_digits(significand) > max_digits) {
        significand /= 10;
        exponent++;
      }
    }
    significand_size = count_digits(significand);
  }

  size_t size = size_t(significand_size) + (is_negative ? 1u : 0u);
  (void)size;  // Used for tracking output size, may be used in future optimizations

  int output_exp = exponent + significand_size - 1;
  bool use_exp_format = (output_exp < exp_lower) || (output_exp >= exp_upper);

  char decimal_point = '.';
  if (use_exp_format) {
    int num_zeros = 0;
    if (significand_size == 1) {
      decimal_point = '\0';
    }
    auto abs_output_exp = output_exp >= 0 ? output_exp : -output_exp;
    int exp_digits = 2;
    if (abs_output_exp >= 100) exp_digits = abs_output_exp >= 1000 ? 4 : 3;

    size += (decimal_point ? 1u : 0u) + 2u + size_t(exp_digits);

    if (is_negative) {
      *buf++ = '-';
    }

    buf = write_significand(buf, significand, significand_size, 1, decimal_point);

    if (num_zeros > 0) buf = fill_n(buf, num_zeros, zero_char);
    *buf++ = exp_char;
    return write_exponent(output_exp, buf);
  }

  int exp = exponent + significand_size;
  if (exponent >= 0) {
    size += static_cast<size_t>(exponent);

    if (is_negative) {
      *buf++ = '-';
    }

    return write_significand_e(buf, significand, significand_size,
                               exponent);

  } else if (exp > 0) {
    size += 1;
    if (is_negative) {
      *buf++ = '-';
    }

    return write_significand(buf, significand, significand_size, exp,
                             decimal_point);
  }

  int num_zeros = -exp;
  bool pointy = num_zeros != 0 || significand_size != 0;
  size += 1u + (pointy ? 1u : 0u) + size_t(num_zeros);

  if (is_negative) {
    *buf++ = '-';
  }

  *buf++ = zero_char;

  if (!pointy) return buf;
  *buf++ = decimal_point;
  buf = fill_n(buf, num_zeros, zero_char);

  return format_decimal(buf, significand, static_cast<uint32_t>(significand_size));
}

// Double precision: max 17 significant digits (std::numeric_limits<double>::max_digits10)
// Returns nullptr on invalid input (null buffer)
char* dtoa_dragonbox_impl(const double f, char* buf, int exp_upper = 16) {
  // Null buffer check
  if (!buf) {
    return nullptr;
  }

  // Fast path for common values 1.0 and -1.0 (bitwise comparison)
  // IEEE 754 double precision: 1.0 = 0x3FF0000000000000, -1.0 = 0xBFF0000000000000
  uint64_t bits;
  std::memcpy(&bits, &f, sizeof(double));

  if (bits == 0x3FF0000000000000ULL) {
    // Exactly 1.0
    *buf++ = '1';
    return buf;
  }
  if (bits == 0xBFF0000000000000ULL) {
    // Exactly -1.0
    *buf++ = '-';
    *buf++ = '1';
    return buf;
  }

  constexpr int kMaxDigits10Double = 17;  // std::numeric_limits<double>::max_digits10
  return dtoa_dragonbox_impl_t(f, buf, kMaxDigits10Double, exp_upper);
}

// Single precision: max 9 significant digits (std::numeric_limits<float>::max_digits10)
// Returns nullptr on invalid input (null buffer)
char* dtoa_dragonbox_impl(const float f, char* buf) {
  // Null buffer check
  if (!buf) {
    return nullptr;
  }

  // Fast path for common values 1.0f and -1.0f (bitwise comparison)
  // IEEE 754 single precision: 1.0f = 0x3F800000, -1.0f = 0xBF800000
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(float));

  if (bits == 0x3F800000U) {
    // Exactly 1.0f
    *buf++ = '1';
    return buf;
  }
  if (bits == 0xBF800000U) {
    // Exactly -1.0f
    *buf++ = '-';
    *buf++ = '1';
    return buf;
  }

  constexpr int kMaxDigits10Float = 9;  // std::numeric_limits<float>::max_digits10
  constexpr int kExpUpperFloat = 7;     // Use exponential notation for values >= 1e7
  return dtoa_dragonbox_impl_t(f, buf, kMaxDigits10Float, kExpUpperFloat);
}

} // anonymous namespace

void SetUSDFloatFormat(bool enable) { g_usd_float_format = enable; }
bool GetUSDFloatFormat() { return g_usd_float_format; }

// Public API for dtos - std::string version
std::string dtos(float v) {
  constexpr size_t kBufferSize = 24; // DTOA_DRAGONBOX_BUFFER_SIZE_FLOAT
  char buffer[kBufferSize];
  char* end = dtoa_dragonbox_impl(v, buffer);
  return std::string(buffer, end);
}

std::string dtos(double v) {
  constexpr size_t kBufferSize = 32; // DTOA_DRAGONBOX_BUFFER_SIZE_DOUBLE
  char buffer[kBufferSize];
  char* end = dtoa_dragonbox_impl(v, buffer);
  return std::string(buffer, end);
}

// Public API for dtos - buffer version (efficient, no std::string construction)
// Returns 0 if buffer is null
size_t dtos(float v, char* buffer) {
  char* end = dtoa_dragonbox_impl(v, buffer);
  if (!end) {
    return 0;  // Invalid input (null buffer)
  }
  return static_cast<size_t>(end - buffer);
}

// Returns 0 if buffer is null
size_t dtos(double v, char* buffer) {
  char* end = dtoa_dragonbox_impl(v, buffer);
  if (!end) {
    return 0;  // Invalid input (null buffer)
  }
  return static_cast<size_t>(end - buffer);
}

// ============================================================================
// Half-precision (fp16) to string conversion
// Produces shortest decimal representation that round-trips through fp16.
// Half-precision has ~3.31 decimal digits of precision (11-bit mantissa).
// We find the shortest decimal that, when parsed back, gives the same fp16.
// ============================================================================

namespace {

// Round a decimal mantissa to N significant digits
// Returns the rounded value and adjusts exponent if needed
uint64_t round_to_digits(uint64_t mantissa, int num_digits, int& exp_adjust) {
  exp_adjust = 0;
  if (mantissa == 0) return 0;

  // Count digits in mantissa
  uint64_t temp = mantissa;
  int digits = 0;
  while (temp > 0) {
    digits++;
    temp /= 10;
  }

  if (digits <= num_digits) return mantissa;

  // Need to round off (digits - num_digits) digits
  int to_remove = digits - num_digits;
  exp_adjust = to_remove;

  // Compute divisor and remainder for rounding
  uint64_t divisor = 1;
  for (int i = 0; i < to_remove; i++) divisor *= 10;

  uint64_t result = mantissa / divisor;
  uint64_t remainder = mantissa % divisor;

  // Round half-even (banker's rounding)
  uint64_t half = divisor / 2;
  if (remainder > half || (remainder == half && (result & 1))) {
    result++;
    // Check for overflow (e.g., 999 -> 1000)
    temp = result;
    int new_digits = 0;
    while (temp > 0) {
      new_digits++;
      temp /= 10;
    }
    if (new_digits > num_digits) {
      result /= 10;
      exp_adjust++;
    }
  }

  return result;
}

// Parse a decimal string back to float (simplified parser)
float parse_decimal(uint64_t mantissa, int exponent) {
  double result = static_cast<double>(mantissa);
  if (exponent > 0) {
    for (int i = 0; i < exponent; i++) result *= 10.0;
  } else if (exponent < 0) {
    for (int i = 0; i < -exponent; i++) result /= 10.0;
  }
  return static_cast<float>(result);
}

// Format mantissa with exponent into buffer
// Returns number of chars written
size_t format_decimal(uint64_t mantissa, int exponent, char* buffer) {
  if (mantissa == 0) {
    buffer[0] = '0';
    return 1;
  }

  char* p = buffer;

  // Convert mantissa to string (reversed)
  char digits[20];
  int num_digits = 0;
  uint64_t temp = mantissa;
  while (temp > 0) {
    digits[num_digits++] = '0' + static_cast<char>(temp % 10);
    temp /= 10;
  }

  // Total exponent adjustment for decimal point position
  int total_exp = exponent + num_digits - 1;

  // Decide on format: fixed or scientific
  // For values in range [0.0001, 10000), use fixed notation
  // Otherwise use scientific

  if (total_exp >= -4 && total_exp < 5) {
    // Fixed notation
    if (total_exp >= 0) {
      // Value >= 1
      int int_digits = total_exp + 1;
      int frac_digits = num_digits - int_digits;

      // Write integer part
      for (int i = num_digits - 1; i >= num_digits - int_digits && i >= 0; i--) {
        *p++ = digits[i];
      }
      // Pad with zeros if needed
      for (int i = 0; i < int_digits - num_digits; i++) {
        *p++ = '0';
      }

      // Write fractional part if any
      if (frac_digits > 0) {
        *p++ = '.';
        for (int i = num_digits - int_digits - 1; i >= 0; i--) {
          *p++ = digits[i];
        }
      }
    } else {
      // Value < 1
      *p++ = '0';
      *p++ = '.';
      // Leading zeros
      for (int i = 0; i < -total_exp - 1; i++) {
        *p++ = '0';
      }
      // Digits
      for (int i = num_digits - 1; i >= 0; i--) {
        *p++ = digits[i];
      }
    }
  } else {
    // Scientific notation (but USD prefers fixed, so we'll extend fixed range)
    // For half-precision, values are in range [~6e-8, 65504]
    // Let's use fixed notation for most cases

    if (total_exp >= 0) {
      // Large value - still use fixed
      int int_digits = total_exp + 1;

      for (int i = num_digits - 1; i >= 0; i--) {
        *p++ = digits[i];
      }
      for (int i = 0; i < int_digits - num_digits; i++) {
        *p++ = '0';
      }
    } else {
      // Small value
      *p++ = '0';
      *p++ = '.';
      for (int i = 0; i < -total_exp - 1; i++) {
        *p++ = '0';
      }
      for (int i = num_digits - 1; i >= 0; i--) {
        *p++ = digits[i];
      }
    }
  }

  return static_cast<size_t>(p - buffer);
}

} // anonymous namespace

// Public API for dtos - half-precision version (std::string)
std::string dtos(value::half h) {
  float f = value::half_to_float(h);

  // Handle special cases
  if (std::isnan(f)) return "nan";
  if (std::isinf(f)) return f > 0 ? "inf" : "-inf";
  if (f == 0.0f) return h.value & 0x8000 ? "-0" : "0";

  bool negative = f < 0;
  if (negative) f = -f;

  // Get dragonbox result for float
  char float_buf[32];
  size_t float_len = dtos(f, float_buf);
  float_buf[float_len] = '\0';

  // Parse the dragonbox output to get mantissa and exponent
  uint64_t mantissa = 0;
  int exponent = 0;
  bool in_frac = false;
  int frac_digits = 0;
  bool in_exp = false;
  bool exp_neg = false;
  int exp_val = 0;

  for (size_t i = 0; i < float_len; i++) {
    char c = float_buf[i];
    if (c == '.') {
      in_frac = true;
    } else if (c == 'e' || c == 'E') {
      in_exp = true;
    } else if (in_exp) {
      if (c == '-') {
        exp_neg = true;
      } else if (c == '+') {
        // skip
      } else if (c >= '0' && c <= '9') {
        exp_val = exp_val * 10 + (c - '0');
      }
    } else if (c >= '0' && c <= '9') {
      mantissa = mantissa * 10 + static_cast<uint64_t>(c - '0');
      if (in_frac) frac_digits++;
    }
  }

  exponent = (exp_neg ? -exp_val : exp_val) - frac_digits;

  // Now try to find shortest representation that round-trips
  // Start with 4 digits (minimum for half precision) and go up to 7
  char result_buf[32];
  size_t result_len = 0;

  for (int precision = 4; precision <= 7; precision++) {
    int exp_adjust = 0;
    uint64_t rounded = round_to_digits(mantissa, precision, exp_adjust);
    int new_exp = exponent + exp_adjust;

    // Strip trailing zeros from mantissa
    while (rounded > 0 && rounded % 10 == 0) {
      rounded /= 10;
      new_exp++;
    }

    // Format and parse back
    char test_buf[32];
    size_t test_len = format_decimal(rounded, new_exp, test_buf);
    test_buf[test_len] = '\0';

    // Parse back to float, then convert to half
    float parsed_f = parse_decimal(rounded, new_exp);
    value::half parsed_h = value::float_to_half_full(parsed_f);

    // Check if round-trip succeeds (ignoring sign bit which we handle separately)
    uint16_t original_bits = h.value & 0x7FFF;
    uint16_t parsed_bits = parsed_h.value & 0x7FFF;

    if (original_bits == parsed_bits) {
      // Found shortest representation
      if (negative) {
        result_buf[0] = '-';
        std::memcpy(result_buf + 1, test_buf, test_len);
        result_len = test_len + 1;
      } else {
        std::memcpy(result_buf, test_buf, test_len);
        result_len = test_len;
      }
      break;
    }
  }

  // If no short representation found, use full float precision
  if (result_len == 0) {
    if (negative) {
      result_buf[0] = '-';
      std::memcpy(result_buf + 1, float_buf, float_len);
      result_len = float_len + 1;
    } else {
      std::memcpy(result_buf, float_buf, float_len);
      result_len = float_len;
    }
  }

  return std::string(result_buf, result_len);
}

// Public API for dtos - half-precision buffer version
size_t dtos(value::half h, char* buffer) {
  std::string s = dtos(h);
  std::memcpy(buffer, s.c_str(), s.size());
  return s.size();
}

// ============================================================================
// Vector and Matrix Printing Functions
// ============================================================================

// Helper template for printing vector types
template<typename T, size_t N>
size_t print_vector_impl(const std::array<T, N>& v, char* buffer) {
  char* p = buffer;
  *p++ = '(';

  for (size_t i = 0; i < N; i++) {
    if (i > 0) {
      *p++ = ',';
      *p++ = ' ';
    }
    size_t len = dtos(static_cast<T>(v[i]), p);
    p += len;
  }

  *p++ = ')';
  return static_cast<size_t>(p - buffer);
}

// Float vectors
size_t print_float2(const value::float2& v, char* buffer) {
  return print_vector_impl(v, buffer);
}

size_t print_float3(const value::float3& v, char* buffer) {
  return print_vector_impl(v, buffer);
}

size_t print_float4(const value::float4& v, char* buffer) {
  return print_vector_impl(v, buffer);
}

// Double vectors
size_t print_double2(const value::double2& v, char* buffer) {
  return print_vector_impl(v, buffer);
}

size_t print_double3(const value::double3& v, char* buffer) {
  return print_vector_impl(v, buffer);
}

size_t print_double4(const value::double4& v, char* buffer) {
  return print_vector_impl(v, buffer);
}

// Half vectors (convert to float for printing)
// Note: These functions are only available when linking with value-types

// Matrix printing: ( (row0), (row1), ... ) - with spaces for USD compatibility
size_t print_matrix2d(const value::matrix2d& m, char* buffer) {
  char* p = buffer;
  *p++ = '(';
  *p++ = ' ';  // Space after opening paren for USD compatibility

  for (size_t row = 0; row < 2; row++) {
    if (row > 0) {
      *p++ = ',';
      *p++ = ' ';
    }
    *p++ = '(';
    for (size_t col = 0; col < 2; col++) {
      if (col > 0) {
        *p++ = ',';
        *p++ = ' ';
      }
      size_t len = dtos(m.m[row][col], p);
      p += len;
    }
    *p++ = ')';
  }

  *p++ = ' ';  // Space before closing paren for USD compatibility
  *p++ = ')';
  return static_cast<size_t>(p - buffer);
}

size_t print_matrix3d(const value::matrix3d& m, char* buffer) {
  char* p = buffer;
  *p++ = '(';
  *p++ = ' ';  // Space after opening paren for USD compatibility

  for (size_t row = 0; row < 3; row++) {
    if (row > 0) {
      *p++ = ',';
      *p++ = ' ';
    }
    *p++ = '(';
    for (size_t col = 0; col < 3; col++) {
      if (col > 0) {
        *p++ = ',';
        *p++ = ' ';
      }
      size_t len = dtos(m.m[row][col], p);
      p += len;
    }
    *p++ = ')';
  }

  *p++ = ' ';  // Space before closing paren for USD compatibility
  *p++ = ')';
  return static_cast<size_t>(p - buffer);
}

size_t print_matrix4d(const value::matrix4d& m, char* buffer) {
  char* p = buffer;
  *p++ = '(';
  *p++ = ' ';  // Space after opening paren for USD compatibility

  for (size_t row = 0; row < 4; row++) {
    if (row > 0) {
      *p++ = ',';
      *p++ = ' ';
    }
    *p++ = '(';
    for (size_t col = 0; col < 4; col++) {
      if (col > 0) {
        *p++ = ',';
        *p++ = ' ';
      }
      size_t len = dtos(m.m[row][col], p);
      p += len;
    }
    *p++ = ')';
  }

  *p++ = ' ';  // Space before closing paren for USD compatibility
  *p++ = ')';
  return static_cast<size_t>(p - buffer);
}

bool GlobMatch(const std::string &pattern, const std::string &str) {
  // Simple glob matching with * (any chars) and ? (single char)
  // Uses dynamic programming approach

  size_t p = 0;  // pattern index
  size_t s = 0;  // string index
  size_t starIdx = std::string::npos;
  size_t matchIdx = 0;

  while (s < str.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == str[s])) {
      // Current characters match, or pattern has ?
      p++;
      s++;
    } else if (p < pattern.size() && pattern[p] == '*') {
      // Star found, remember position
      starIdx = p;
      matchIdx = s;
      p++;
    } else if (starIdx != std::string::npos) {
      // Mismatch, but we have a previous star
      // Backtrack: try matching one more character with the star
      p = starIdx + 1;
      matchIdx++;
      s = matchIdx;
    } else {
      // No match and no star to backtrack
      return false;
    }
  }

  // Check remaining pattern characters (should all be *)
  while (p < pattern.size() && pattern[p] == '*') {
    p++;
  }

  return p == pattern.size();
}

bool GlobMatchPath(const std::string &pattern, const std::string &path) {
  // Glob matching for paths with ** support for recursive matching
  // ** matches zero or more path segments (including /)
  // * matches any characters except /
  // ? matches single character except /

  size_t p = 0;
  size_t s = 0;
  size_t starStarIdx = std::string::npos;
  size_t starStarMatchIdx = 0;
  size_t starIdx = std::string::npos;
  size_t matchIdx = 0;

  while (s < path.size()) {
    // Check for **
    if (p + 1 < pattern.size() && pattern[p] == '*' && pattern[p + 1] == '*') {
      starStarIdx = p;
      starStarMatchIdx = s;
      p += 2;
      // Skip trailing / after **
      if (p < pattern.size() && pattern[p] == '/') {
        p++;
      }
      continue;
    }

    if (p < pattern.size() && pattern[p] == '*' &&
        (p + 1 >= pattern.size() || pattern[p + 1] != '*')) {
      // Single * - matches any except /
      starIdx = p;
      matchIdx = s;
      p++;
    } else if (p < pattern.size() &&
               ((pattern[p] == '?' && path[s] != '/') || pattern[p] == path[s])) {
      p++;
      s++;
    } else if (starIdx != std::string::npos && path[s] != '/') {
      // Backtrack to single star
      p = starIdx + 1;
      matchIdx++;
      s = matchIdx;
    } else if (starStarIdx != std::string::npos) {
      // Backtrack to double star
      p = starStarIdx + 2;
      if (p < pattern.size() && pattern[p] == '/') {
        p++;
      }
      starStarMatchIdx++;
      s = starStarMatchIdx;
      starIdx = std::string::npos;
    } else {
      return false;
    }
  }

  // Check remaining pattern
  while (p < pattern.size()) {
    if (pattern[p] == '*') {
      p++;
    } else if (p + 1 < pattern.size() && pattern[p] == '*' && pattern[p + 1] == '*') {
      p += 2;
    } else {
      break;
    }
  }

  return p == pattern.size();
}

char *dtoa(float f, char *buf) {
  // Use dragonbox for float-to-string conversion
  size_t len = dtos(f, buf);
  return buf + len;
}

char *dtoa(double d, char *buf) {
  // Use dragonbox for double-to-string conversion
  size_t len = dtos(d, buf);
  return buf + len;
}

}  // namespace lightusd
