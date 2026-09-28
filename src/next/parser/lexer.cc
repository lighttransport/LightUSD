// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Lexer implementation

#include "lexer.hh"
#include "../prim/identifier.hh"
#include "../strfmt.hh"
#include "simd-scan.hh"

#include <cctype>
#include <cstring>
#include <limits>
#include <utility>

namespace lightusd {
namespace next {

namespace {

bool IsIdentifierStart(const char* data, size_t size, size_t pos) {
  uint32_t cp = 0;
  size_t width = 0;
  return identifier_detail::DecodeUtf8(data, size, pos, &cp, &width) &&
         identifier_detail::IsStart(cp);
}

bool IsIdentifierContinueAscii(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsDigit(char c) {
  return std::isdigit(static_cast<unsigned char>(c));
}

bool IsHexDigit(char c) {
  return std::isxdigit(static_cast<unsigned char>(c));
}

// Length of an `inf` / `infinity` / `nan` special-float word (case-insensitive)
// starting at p, or 0 if none. Requires a non-identifier char (or end) after the
// word so e.g. `info` or `index` are not mistaken for `inf`.
size_t MatchFloatSpecial(const char* p, const char* end) {
  auto ieq = [](const char* a, const char* lit, size_t n) {
    for (size_t i = 0; i < n; i++) {
      if (std::tolower(static_cast<unsigned char>(a[i])) != lit[i]) return false;
    }
    return true;
  };
  auto word_boundary = [&](size_t n) {
    return (p + n == end) || !IsIdentifierContinueAscii(p[n]);
  };
  if (p + 8 <= end && ieq(p, "infinity", 8) && word_boundary(8)) return 8;
  if (p + 3 <= end && ieq(p, "inf", 3) && word_boundary(3)) return 3;
  if (p + 3 <= end && ieq(p, "nan", 3) && word_boundary(3)) return 3;
  return 0;
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct Keyword {
  const char* name;
  TokenType type;
};

const Keyword kKeywords[] = {
  {"def", TokenType::Def},
  {"over", TokenType::Over},
  {"class", TokenType::Class},
  {"true", TokenType::True},
  {"True", TokenType::True},
  {"false", TokenType::False},
  {"False", TokenType::False},
  {"None", TokenType::None},
  {"timeSamples", TokenType::TimeSamples},
  {"custom", TokenType::Custom},
  {"uniform", TokenType::Uniform},
  {"varying", TokenType::Varying},
  {"prepend", TokenType::Prepend},
  {"append", TokenType::Append},
  {"delete", TokenType::Delete},
  {"add", TokenType::Add},
  {"reorder", TokenType::Reorder},
  {"rel", TokenType::Rel},
};

// Keyword lookup over the raw identifier bytes: a length + first-byte gate in
// front of the memcmp (comparing a std::string against each `const char*`
// keyword cost a strlen per keyword per identifier on the hot path).
TokenType LookupKeyword(const char* p, size_t n) {
  if (n < 3 || n > 11) return TokenType::Identifier;
  for (const auto& kw : kKeywords) {
    if (kw.name[0] == p[0] && std::strlen(kw.name) == n &&
        std::memcmp(kw.name, p, n) == 0) {
      return kw.type;
    }
  }
  return TokenType::Identifier;
}

// ASCII bytes that continue an identifier, derived once from the same XID
// tables the general path uses (so the fast path cannot disagree with it).
struct AsciiIdentTable {
  bool cont[128];
  AsciiIdentTable() {
    for (uint32_t c = 0; c < 128; ++c) {
      cont[c] = identifier_detail::IsContinue(c);
    }
  }
};
const AsciiIdentTable& GetAsciiIdentTable() {
  static const AsciiIdentTable table;
  return table;
}

}  // anonymous namespace

Lexer::Lexer(const char* data, size_t length)
    : data_(data), length_(length) {}

// advance()/current_char()/peek_char() are defined inline in lexer.hh (they
// sit in every per-byte scanning loop).

void Lexer::skip_whitespace() {
  while (pos_ < length_) {
    char c = current_char();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance();
    } else if (c == '#') {
      skip_comment();
    } else if (c == '/' && peek_char() == '*') {
      // USDA accepts C-style separating comments in addition to `#` line
      // comments. They are whitespace and may occur between a type and name.
      advance();
      advance();
      while (pos_ < length_) {
        if (current_char() == '*' && peek_char() == '/') {
          advance();
          advance();
          break;
        }
        advance();
      }
      if (pos_ >= length_) {
        set_fatal_error("Unterminated block comment");
        return;
      }
    } else {
      break;
    }
  }
}

void Lexer::skip_comment() {
  // Skip to end of line
  while (pos_ < length_ && current_char() != '\n') {
    advance();
  }
}

void Lexer::skip_line() {
  while (pos_ < length_ && current_char() != '\n') {
    advance();
  }
  if (pos_ < length_) {
    advance();  // Skip the newline
  }
}

bool Lexer::capture_bracketed_literal(const char** out_data, size_t* out_len,
                                      bool* out_simple, size_t* out_commas) {
  if (!out_data || !out_len) return false;
  const Token& tok = peek();
  if (tok.type != TokenType::OpenBracket) {
    set_error("Expected OpenBracket, got " + std::string(TokenTypeName(tok.type)));
    return false;
  }

  // `peek()` has scanned the '[' token, so pos_ is just after it.
  const size_t start = pos_ > 0 ? pos_ - 1 : 0;
  has_current_ = false;

  // "Simple" = no comment/string/asset/nested-bracket bytes inside, so every
  // comma/paren is a pure separator (safe to split for parallel numeric parse).
  bool simple = true;
  size_t commas = 0;
  int depth = 1;
  while (pos_ < length_ && depth > 0) {
    // SIMD-skip the "boring" array bytes (digits/commas/whitespace) straight to
    // the next structural byte {[ ] " ' @ #}; only those need per-char handling.
    // Equivalent to the old byte-by-byte advance() loop (same stop positions),
    // just far faster on the big numeric arrays that dominate flattened scenes.
    {
      const char* base = data_ + pos_;
      size_t nl = 0;
      const char* hit =
          simdscan::ScanArrayStructural(base, data_ + length_, &nl, &commas);
      pos_ += static_cast<size_t>(hit - base);
      line_ += nl;  // column_ left approximate inside arrays (error cosmetics)
      if (pos_ >= length_ || depth <= 0) break;
    }
    const char c = current_char();

    if (c == '#') {
      simple = false;  // comment bytes may contain commas/parens/']'
      skip_comment();
      continue;
    }

    if (c == '"' || c == '\'') {
      simple = false;  // string bytes may contain separators
      const char quote = c;
      advance();
      bool triple = false;
      if (current_char() == quote && peek_char() == quote) {
        triple = true;
        advance();
        advance();
      }
      while (pos_ < length_) {
        if (current_char() == '\\') {
          advance();
          if (pos_ < length_) advance();
          continue;
        }
        if (triple) {
          if (current_char() == quote && peek_char() == quote && peek_char(2) == quote) {
            advance();
            advance();
            advance();
            break;
          }
        } else if (current_char() == quote) {
          advance();
          break;
        }
        advance();
      }
      continue;
    }

    if (c == '@') {
      simple = false;  // asset-ref bytes may contain separators
      // Triple-@ form (@@@path@@@, used when the path itself contains '@'):
      // terminated only by an unescaped @@@ run.
      const bool triple = (pos_ + 2 < length_ && data_[pos_ + 1] == '@' &&
                           data_[pos_ + 2] == '@');
      advance();
      if (triple) {
        advance();
        advance();
        while (pos_ < length_) {
          if (current_char() == '\\') {
            advance();
            if (pos_ < length_) advance();
            continue;
          }
          if (current_char() == '@' && pos_ + 2 < length_ &&
              data_[pos_ + 1] == '@' && data_[pos_ + 2] == '@') {
            advance();
            advance();
            advance();
            break;
          }
          advance();
        }
        continue;
      }
      while (pos_ < length_) {
        if (current_char() == '\\') {
          advance();
          if (pos_ < length_) advance();
          continue;
        }
        if (current_char() == '@') {
          advance();
          break;
        }
        advance();
      }
      continue;
    }

    if (c == '[') {
      simple = false;  // nested array: separators are not flat
      depth++;
    } else if (c == ']') {
      depth--;
    }
    advance();
  }

  if (depth != 0) {
    set_error("Unexpected end of input while parsing array literal");
    return false;
  }

  *out_data = data_ + start;
  *out_len = pos_ - start;
  if (out_simple) *out_simple = simple;
  if (out_commas) *out_commas = commas;
  has_current_ = false;
  return true;
}

void Lexer::skip_quoted_raw() {
  const char quote = current_char();
  advance();
  bool triple = false;
  if (current_char() == quote && peek_char() == quote) {
    triple = true;
    advance();
    advance();
  }
  while (pos_ < length_) {
    if (current_char() == '\\') {
      advance();
      if (pos_ < length_) advance();
      continue;
    }
    if (triple) {
      if (current_char() == quote && peek_char() == quote &&
          peek_char(2) == quote) {
        advance();
        advance();
        advance();
        return;
      }
    } else if (current_char() == quote) {
      advance();
      return;
    }
    advance();
  }
}

void Lexer::skip_asset_raw() {
  // Triple-@ form (@@@path@@@, used when the path itself contains '@'):
  // terminated only by an unescaped @@@ run.
  const bool triple = (pos_ + 2 < length_ && data_[pos_ + 1] == '@' &&
                       data_[pos_ + 2] == '@');
  advance();
  if (triple) {
    advance();
    advance();
  }
  while (pos_ < length_) {
    if (current_char() == '\\') {
      advance();
      if (pos_ < length_) advance();
      continue;
    }
    if (triple) {
      if (current_char() == '@' && pos_ + 2 < length_ &&
          data_[pos_ + 1] == '@' && data_[pos_ + 2] == '@') {
        advance();
        advance();
        advance();
        return;
      }
    } else if (current_char() == '@') {
      advance();
      return;
    }
    advance();
  }
}

bool Lexer::capture_prim_block(size_t min_bytes, size_t max_bytes,
                               const char** out_block, size_t* out_len,
                               size_t* out_line, size_t* out_column,
                               bool* out_too_big) {
  if (out_too_big) *out_too_big = false;
  if (!out_block || !out_len) return false;

  const Token& tok = peek();
  if (tok.type != TokenType::Def && tok.type != TokenType::Over &&
      tok.type != TokenType::Class) {
    return false;
  }
  // peek() recorded where the specifier token starts (after whitespace and
  // comments): that is the block start.
  const size_t block_start = token_start_;
  const size_t start_line = tok.line;
  const size_t start_column = tok.column;

  // Full state snapshot for restore on too-big/too-small/malformed.
  const size_t saved_pos = pos_;
  const size_t saved_line = line_;
  const size_t saved_column = column_;
  const Token saved_current = current_;
  const bool saved_has_current = has_current_;

  has_current_ = false;

  // Scan for the body '{' (first open brace at paren depth 0 — metadata dicts
  // live inside the '(...)' block so their braces are at paren depth >= 1),
  // then match braces to the block end. Strings / asset refs / comments are
  // skipped so separator bytes inside them are inert; the SIMD scan jumps over
  // everything else.
  int paren_depth = 0;
  int brace_depth = 0;
  bool body_seen = false;
  bool ok = false;
  bool bail = false;
  const size_t scan_limit =
      (max_bytes > 0 && max_bytes < length_ - block_start)
          ? (block_start + max_bytes)
          : length_;

  while (pos_ < length_) {
    {
      const char* base = data_ + pos_;
      size_t nl = 0;
      const char* hit =
          simdscan::ScanPrimStructural(base, data_ + length_, &nl);
      pos_ += static_cast<size_t>(hit - base);
      line_ += nl;  // column_ approximate inside the block (restored/unused)
      if (pos_ >= length_) break;
    }
    if (pos_ > scan_limit) break;
    const char c = current_char();

    if (c == '#') {
      skip_comment();
      continue;
    }
    if (c == '"' || c == '\'') {
      skip_quoted_raw();
      continue;
    }
    if (c == '@') {
      skip_asset_raw();
      continue;
    }
    if (c == '/') {
      if (peek_char() == '*') {
        // C-style block comment: rare; leave the whole prim to the inline
        // parser rather than teaching every raw scanner about it.
        bail = true;
        break;
      }
      advance();
      continue;
    }

    if (c == '[') {
      // Skip the whole bracketed literal with the ARRAY scanner: numeric
      // tuple arrays are full of parens, and stopping at each one here made
      // the prim scan far slower than the inline parse it replaces.
      advance();  // '['
      int bracket_depth = 1;
      while (pos_ < length_ && bracket_depth > 0) {
        {
          const char* base = data_ + pos_;
          size_t nl = 0;
          size_t commas_unused = 0;
          const char* hit = simdscan::ScanArrayStructural(
              base, data_ + length_, &nl, &commas_unused);
          pos_ += static_cast<size_t>(hit - base);
          line_ += nl;
          if (pos_ >= length_) break;
        }
        if (pos_ > scan_limit) break;
        const char ac = current_char();
        if (ac == '#') {
          skip_comment();
          continue;
        }
        if (ac == '"' || ac == '\'') {
          skip_quoted_raw();
          continue;
        }
        if (ac == '@') {
          skip_asset_raw();
          continue;
        }
        if (ac == '[') {
          bracket_depth++;
        } else if (ac == ']') {
          bracket_depth--;
        }
        advance();
      }
      if (pos_ > scan_limit) break;
      continue;
    }

    if (c == '(') {
      paren_depth++;
    } else if (c == ')') {
      if (paren_depth > 0) paren_depth--;
    } else if (c == '{') {
      if (paren_depth == 0) {
        body_seen = true;
        brace_depth++;
      }
      // braces inside metadata parens (dict values) are balanced within the
      // parens: they cannot open the prim body.
    } else if (c == '}') {
      if (paren_depth == 0 && body_seen) {
        brace_depth--;
        if (brace_depth == 0) {
          advance();  // consume the closing '}'
          ok = true;
          break;
        }
      }
    }
    advance();  // stray ']' (malformed) is skipped like any other byte
  }

  const size_t block_len = ok ? (pos_ - block_start) : 0;
  if (!ok || bail || block_len < min_bytes || pos_ > scan_limit) {
    if (out_too_big && !ok && !bail && pos_ > scan_limit) *out_too_big = true;
    pos_ = saved_pos;
    line_ = saved_line;
    column_ = saved_column;
    current_ = saved_current;
    has_current_ = saved_has_current;
    return false;
  }

  *out_block = data_ + block_start;
  *out_len = block_len;
  if (out_line) *out_line = start_line;
  if (out_column) *out_column = start_column;
  has_current_ = false;
  return true;
}

const Token& Lexer::peek() {
  if (!has_current_) {
    current_ = scan_token();
    has_current_ = true;
  }
  return current_;
}

Token Lexer::next() {
  if (has_current_) {
    has_current_ = false;
    return current_;
  }
  return scan_token();
}

void Lexer::consume() {
  if (has_current_) {
    has_current_ = false;
    return;
  }
  (void)scan_token();
}

bool Lexer::expect(TokenType type) {
  Token tok = next();
  if (tok.type != type) {
    set_error("Expected " + std::string(TokenTypeName(type)) +
              ", got " + std::string(TokenTypeName(tok.type)));
    return false;
  }
  return true;
}

bool Lexer::expect(TokenType type, std::string& out_value) {
  Token tok = next();
  if (tok.type != type) {
    set_error("Expected " + std::string(TokenTypeName(type)) +
              ", got " + std::string(TokenTypeName(tok.type)));
    return false;
  }
  out_value = std::move(tok.value);
  return true;
}

void Lexer::set_error(const std::string& msg) {
  if (error_.empty()) {
    error_ = "Line " + UIntToStr(line_) + ", column " + UIntToStr(column_) + ": " + msg;
  }
}

void Lexer::set_fatal_error(const std::string& msg) {
  fatal_ = true;
  // A fatal lexical malformation must be the surfaced message even when a
  // recoverable expect() mismatch was recorded first.
  error_ = "Line " + UIntToStr(line_) + ", column " + UIntToStr(column_) + ": " + msg;
}

Token Lexer::make_token(TokenType type, size_t start_line, size_t start_col) {
  Token tok;
  tok.type = type;
  tok.line = start_line;
  tok.column = start_col;
  return tok;
}

Token Lexer::make_token(TokenType type, std::string value, size_t start_line, size_t start_col) {
  Token tok;
  tok.type = type;
  tok.value = std::move(value);
  tok.line = start_line;
  tok.column = start_col;
  return tok;
}

Token Lexer::scan_token() {
  skip_whitespace();
  token_start_ = pos_;

  if (at_end()) {
    return make_token(TokenType::Eof, line_, column_);
  }

  size_t start_line = line_;
  size_t start_col = column_;
  char c = current_char();

  // Single-character tokens
  switch (c) {
    case '(': advance(); return make_token(TokenType::OpenParen, start_line, start_col);
    case ')': advance(); return make_token(TokenType::CloseParen, start_line, start_col);
    case '[': advance(); return make_token(TokenType::OpenBracket, start_line, start_col);
    case ']': advance(); return make_token(TokenType::CloseBracket, start_line, start_col);
    case '{': advance(); return make_token(TokenType::OpenBrace, start_line, start_col);
    case '}': advance(); return make_token(TokenType::CloseBrace, start_line, start_col);
    case '=': advance(); return make_token(TokenType::Equals, start_line, start_col);
    case ':': advance(); return make_token(TokenType::Colon, start_line, start_col);
    case '.':
      if (IsDigit(peek_char())) break;  // leading-dot float literal (.5)
      advance();
      return make_token(TokenType::Dot, start_line, start_col);
    case ',': advance(); return make_token(TokenType::Comma, start_line, start_col);
    case ';': advance(); return make_token(TokenType::Semicolon, start_line, start_col);
  }

  // Path reference: </path>
  if (c == '<') {
    return scan_path_ref();
  }

  // Asset reference: @path@
  if (c == '@') {
    return scan_asset_ref();
  }

  // String
  if (c == '"' || c == '\'') {
    return scan_string();
  }

  // Identifier or keyword
  if (IsIdentifierStart(data_, length_, pos_)) {
    return scan_identifier();
  }

  // Number (including negative, and signed inf/nan specials such as `-inf`).
  // Bare `inf`/`nan` start with a letter and are lexed as identifiers above;
  // the value parser accepts those in numeric context (keeping an attribute
  // literally named `inf` safe).
  if (IsDigit(c) || (c == '-' && IsDigit(peek_char())) || (c == '+' && IsDigit(peek_char())) ||
      (c == '.' && IsDigit(peek_char())) ||
      ((c == '-' || c == '+') && peek_char() == '.') ||
      ((c == '-' || c == '+') && MatchFloatSpecial(data_ + pos_ + 1, data_ + length_) > 0)) {
    return scan_number();
  }

  // Unrecognized character
  set_error(std::string("Unexpected character: ") + c);
  advance();
  return make_token(TokenType::Invalid, start_line, start_col);
}

Token Lexer::scan_identifier() {
  size_t start_line = line_;
  size_t start_col = column_;
  size_t start = pos_;
  const AsciiIdentTable& ascii = GetAsciiIdentTable();

  while (pos_ < length_) {
    // ASCII fast path: identifier bytes never include '\n', so the location
    // update is a plain column bump (same result as advance() per byte).
    {
      size_t p = pos_;
      while (p < length_) {
        const unsigned char c = static_cast<unsigned char>(data_[p]);
        if (c >= 0x80 || !ascii.cont[c]) break;
        ++p;
      }
      column_ += p - pos_;
      pos_ = p;
      if (pos_ >= length_ ||
          static_cast<unsigned char>(data_[pos_]) < 0x80) {
        break;  // end of input or a non-identifier ASCII byte
      }
    }
    uint32_t cp = 0;
    size_t width = 0;
    if (!identifier_detail::DecodeUtf8(data_, length_, pos_, &cp, &width)) {
      if (static_cast<unsigned char>(current_char()) >= 0x80) {
        set_fatal_error("Malformed UTF-8 in identifier");
        return make_token(TokenType::Invalid, start_line, start_col);
      }
      break;
    }
    if (!identifier_detail::IsContinue(cp)) break;
    for (size_t i = 0; i < width; ++i) advance();
  }

  if (pos_ - start > kMaxTokenLength) {
    set_fatal_error("Identifier token too large");
    return make_token(TokenType::Invalid, start_line, start_col);
  }

  const TokenType type = LookupKeyword(data_ + start, pos_ - start);
  return make_token(type, std::string(data_ + start, pos_ - start), start_line,
                    start_col);
}

Token Lexer::scan_number() {
  size_t start_line = line_;
  size_t start_col = column_;
  size_t start = pos_;

  // Sign
  if (current_char() == '-' || current_char() == '+') {
    advance();
  }

  // Signed inf / infinity / nan special float literal (e.g. `-inf`). Emit as a
  // Number token; strtof/strtod/fast_float parse these directly.
  if (size_t special = MatchFloatSpecial(data_ + pos_, data_ + length_)) {
    for (size_t i = 0; i < special; i++) advance();
    std::string value(data_ + start, pos_ - start);
    return make_token(TokenType::Number, std::move(value), start_line, start_col);
  }

  // Check for hex
  if (current_char() == '0' && (peek_char() == 'x' || peek_char() == 'X')) {
    advance();  // '0'
    advance();  // 'x'
    while (IsHexDigit(current_char())) {
      advance();
    }
  } else {
    // Integer part
    while (IsDigit(current_char())) {
      advance();
    }

    // Decimal part. pxr's grammar is `[0-9]+\.?[0-9]*|\.[0-9]+`: a leading
    // dot (`.5`) and a trailing dot (`1.`) are both valid float literals.
    if (current_char() == '.') {
      advance();  // '.'
      while (IsDigit(current_char())) {
        advance();
      }
    }

    // Exponent: only consume when followed by digits (`1e` / `1e+` are NOT
    // numbers — consuming a bare exponent silently truncated `1e3`-style
    // typos to `1`; leave the `e` for the parser to reject).
    if (current_char() == 'e' || current_char() == 'E') {
      const size_t exp_pos = pos_;
      advance();
      if (current_char() == '+' || current_char() == '-') {
        advance();
      }
      if (IsDigit(current_char())) {
        while (IsDigit(current_char())) {
          advance();
        }
      } else {
        // rewind: not an exponent
        while (pos_ > exp_pos) { pos_--; if (column_ > 0) column_--; }
      }
    }
  }

  if (pos_ - start > kMaxTokenLength) {
    set_fatal_error("Number token too large");
    return make_token(TokenType::Invalid, start_line, start_col);
  }

  std::string value(data_ + start, pos_ - start);
  return make_token(TokenType::Number, std::move(value), start_line, start_col);
}

Token Lexer::scan_string() {
  size_t start_line = line_;
  size_t start_col = column_;

  const char quote = current_char();
  advance();  // Opening quote

  std::string value;
  bool is_multiline = false;
  bool terminated = false;

  // Check for triple-quoted string
  if (current_char() == quote && peek_char() == quote) {
    is_multiline = true;
    advance();  // Second quote
    advance();  // Third quote
  }

  while (!at_end()) {
    char c = current_char();

    if (value.size() > kMaxTokenLength) {
      set_fatal_error("String literal too large");
      return make_token(TokenType::Invalid, start_line, start_col);
    }

    if (is_multiline) {
      // Check for closing triple quotes
      if (c == quote && peek_char() == quote && peek_char(2) == quote) {
        advance();
        advance();
        advance();
        terminated = true;
        break;
      }
    } else {
      if (c == quote) {
        advance();
        terminated = true;
        break;
      }
      if (c == '\n') {
        set_fatal_error("Unterminated string literal");
        return make_token(TokenType::Invalid, start_line, start_col);
      }
    }

    // Escape sequences
    if (c == '\\' && !at_end()) {
      advance();                  // consume '\'
      char escaped = current_char();
      advance();                  // consume the escape indicator char
      switch (escaped) {
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        case 'r': value += '\r'; break;
        case 'a': value += '\a'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'v': value += '\v'; break;
        case '\\': value += '\\'; break;
        case '"': value += '"'; break;
        case '\'': value += '\''; break;
        case 'x': {
          // \xNN - up to two hex digits
          int v = 0, n = 0;
          while (n < 2 && HexVal(current_char()) >= 0) {
            v = v * 16 + HexVal(current_char());
            advance();
            n++;
          }
          if (n == 0) { value += '\\'; value += 'x'; }
          else value += static_cast<char>(v);
          break;
        }
        // NOTE: `\u`/`\U` are NOT USD escapes (the grammar has only
        // single-char, \xNN hex and octal): pxr treats them as unknown
        // escapes and keeps the character without the backslash.
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
          // Octal \NNN - first digit is `escaped`, up to two more
          int v = escaped - '0', n = 1;
          while (n < 3 && current_char() >= '0' && current_char() <= '7') {
            v = v * 8 + (current_char() - '0');
            advance();
            n++;
          }
          value += static_cast<char>(v & 0xff);
          break;
        }
        default:
          // Unknown escape: pxr drops the backslash and keeps the character.
          value += escaped;
          break;
      }
    } else {
      value += c;
      advance();
    }
  }

  if (!terminated) {
    // Reached EOF inside the string (single- or triple-quoted): the token
    // stream is broken; silently returning a String here made truncated files
    // "parse" successfully.
    set_fatal_error("Unterminated string literal (EOF inside string)");
    return make_token(TokenType::Invalid, start_line, start_col);
  }

  return make_token(TokenType::String, std::move(value), start_line, start_col);
}

Token Lexer::scan_path_ref() {
  size_t start_line = line_;
  size_t start_col = column_;

  advance();  // '<'

  std::string value;
  while (!at_end() && current_char() != '>') {
    if (value.size() > kMaxTokenLength) {
      set_fatal_error("Path reference token too large");
      return make_token(TokenType::Invalid, start_line, start_col);
    }
    value += current_char();
    advance();
  }

  if (current_char() == '>') {
    advance();
  } else {
    set_fatal_error("Unterminated path reference");
    return make_token(TokenType::Invalid, start_line, start_col);
  }

  // `<>` is the empty path, which AOUSD authors for relocate-to-none and to
  // clear reference/payload targets; only non-empty text must be a path.
  if (strict_aousd_conformance && !value.empty() && !IsValidPathString(value)) {
    set_fatal_error("Invalid AOUSD path reference: <" + value + ">");
    return make_token(TokenType::Invalid, start_line, start_col);
  }
  return make_token(TokenType::PathRef, std::move(value), start_line, start_col);
}

Token Lexer::scan_asset_ref() {
  size_t start_line = line_;
  size_t start_col = column_;

  advance();  // '@'

  // Check for @@@ (triple at)
  bool is_triple = false;
  if (current_char() == '@' && peek_char() == '@') {
    is_triple = true;
    advance();
    advance();
  }

  std::string value;
  bool terminated = false;
  while (!at_end()) {
    char c = current_char();

    if (value.size() > kMaxTokenLength) {
      set_fatal_error("Asset reference token too large");
      return make_token(TokenType::Invalid, start_line, start_col);
    }

    if (is_triple) {
      // Inside @@@...@@@, `\@@@` is an ESCAPED literal `@@@` (SdfAssetPath
      // escaping) — consume it without terminating, else the tail desyncs
      // the token stream.
      if (c == '\\' && peek_char() == '@' && peek_char(2) == '@' &&
          peek_char(3) == '@') {
        value += "@@@";
        advance();
        advance();
        advance();
        advance();
        continue;
      }
      if (c == '@' && peek_char() == '@' && peek_char(2) == '@') {
        advance();
        advance();
        advance();
        terminated = true;
        break;
      }
    } else {
      if (c == '@') {
        advance();
        terminated = true;
        break;
      }
    }

    value += c;
    advance();
  }

  if (!terminated) {
    set_fatal_error("Unterminated asset reference (EOF inside '@' literal)");
    return make_token(TokenType::Invalid, start_line, start_col);
  }

  // Return as a string token (asset references are essentially strings)
  return make_token(TokenType::String, std::move(value), start_line, start_col);
}

const char* TokenTypeName(TokenType type) {
  switch (type) {
    case TokenType::Invalid: return "Invalid";
    case TokenType::Eof: return "EOF";
    case TokenType::Identifier: return "Identifier";
    case TokenType::String: return "String";
    case TokenType::Number: return "Number";
    case TokenType::PathRef: return "PathRef";
    case TokenType::Def: return "def";
    case TokenType::Over: return "over";
    case TokenType::Class: return "class";
    case TokenType::True: return "true";
    case TokenType::False: return "false";
    case TokenType::None: return "None";
    case TokenType::OpenParen: return "(";
    case TokenType::CloseParen: return ")";
    case TokenType::OpenBracket: return "[";
    case TokenType::CloseBracket: return "]";
    case TokenType::OpenBrace: return "{";
    case TokenType::CloseBrace: return "}";
    case TokenType::Equals: return "=";
    case TokenType::Colon: return ":";
    case TokenType::Dot: return ".";
    case TokenType::Comma: return ",";
    case TokenType::Semicolon: return ";";
    case TokenType::At: return "@";
    case TokenType::TimeSamples: return "timeSamples";
    case TokenType::Custom: return "custom";
    case TokenType::Uniform: return "uniform";
    case TokenType::Varying: return "varying";
    case TokenType::Prepend: return "prepend";
    case TokenType::Append: return "append";
    case TokenType::Delete: return "delete";
    case TokenType::Add: return "add";
    case TokenType::Reorder: return "reorder";
    case TokenType::Rel: return "rel";
  }
  return "Unknown";
}

}  // namespace next
}  // namespace lightusd
