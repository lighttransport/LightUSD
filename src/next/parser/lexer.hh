// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA Lexer
// Simple tokenizer for USDA ASCII format

#pragma once

#include <string>
#include <cstdint>

namespace lightusd {
namespace next {

/// Token types
enum class TokenType : uint8_t {
  Invalid,
  Eof,           // End of input

  // Literals
  Identifier,    // foo, bar_baz, _underscore
  String,        // "hello world"
  Number,        // 123, 3.14, -1.5e10
  PathRef,       // </World/Cube>

  // Keywords
  Def,           // def
  Over,          // over
  Class,         // class
  True,          // true
  False,         // false
  None,          // None

  // Symbols
  OpenParen,     // (
  CloseParen,    // )
  OpenBracket,   // [
  CloseBracket,  // ]
  OpenBrace,     // {
  CloseBrace,    // }
  Equals,        // =
  Colon,         // :
  Dot,           // .
  Comma,         // ,
  Semicolon,     // ;
  At,            // @

  // Special
  TimeSamples,   // timeSamples
  Custom,        // custom
  Uniform,       // uniform
  Varying,       // varying
  Prepend,       // prepend
  Append,        // append
  Delete,        // delete
  Add,           // add
  Reorder,       // reorder
  Rel,           // rel
};

/// Token with source location
struct Token {
  TokenType type = TokenType::Invalid;
  std::string value;     // The actual text content
  size_t line = 0;       // 1-based line number
  size_t column = 0;     // 1-based column number

  bool is(TokenType t) const { return type == t; }
  bool is_literal() const {
    return type == TokenType::Identifier ||
           type == TokenType::String ||
           type == TokenType::Number ||
           type == TokenType::PathRef;
  }
};

/// Lexer - tokenizes USDA input
class Lexer {
public:
  /// Construct from input data
  Lexer(const char* data, size_t length);

  /// Get current position info
  size_t line() const { return line_; }
  size_t column() const { return column_; }
  size_t position() const { return pos_; }

  /// Reset the lexer to a previously-saved byte position.  The caller must
  /// ensure `pos` was obtained from `position()` before any tokens were
  /// consumed past that point; the line/column counters are reset to
  /// reflect the new position (they are recomputed on the next peek/next).
  void set_position(size_t pos) {
    pos_ = pos;
    has_current_ = false;  // invalidate cached token
    error_.clear();
    fatal_ = false;
    position_reset_ = true;
    // line_/column_ will be recomputed by the next peek/next scan.  For a
    // short metadata string (typical caller) the cost is negligible.
    line_ = 1;
    column_ = 1;
  }

  /// Peek at current token without consuming
  const Token& peek();

  /// Get current token and advance
  Token next();

  /// Advance past the current token without returning it. Equivalent to
  /// calling next() and discarding the result, minus the Token (and value
  /// string) copy — use for the peek-then-discard pattern. (next() itself must
  /// keep copying: callers hold peek() references across next().)
  void consume();

  /// Check if at end of input
  bool at_end() const { return pos_ >= length_; }

  /// Expect a specific token type (returns false if not matched)
  bool expect(TokenType type);

  /// Expect and consume, storing value
  bool expect(TokenType type, std::string& out_value);

  /// Skip to end of current line
  void skip_line();

  /// Capture and consume a complete bracketed literal without tokenizing every
  /// element. The returned span points into the lexer's input and includes the
  /// outer '[' and ']'. When `out_simple` is non-null it reports whether the
  /// array contains ONLY "plain" bytes (no comment `#`, string/asset quote, or
  /// nested `[` ) — i.e. all commas/parens are pure structural separators, which
  /// lets a numeric array be safely split at separator boundaries for parallel
  /// parsing. When `out_commas` is non-null it receives the number of commas
  /// seen in the SIMD-scanned (plain) bytes; trustworthy ONLY when the array is
  /// simple, where it predicts the scalar count (scalars = commas + 1, or
  /// commas with a trailing comma) so the parser can pre-size its output.
  bool capture_bracketed_literal(const char** out_data, size_t* out_len,
                                 bool* out_simple = nullptr,
                                 size_t* out_commas = nullptr);

  /// Capture and consume one complete prim block
  ///   def|over|class [Type] "name" [(meta)] { ...body... }
  /// starting at the current (peeked) specifier token, without tokenizing its
  /// contents (SIMD brace/paren matching; strings/assets/comments skipped). On
  /// success the whole block is consumed and the returned span (a slice of the
  /// input, from the specifier through the closing '}') can be re-parsed
  /// independently — the basis of the parallel prim-subtree parse.
  /// `out_line`/`out_column` receive the source location of the specifier so
  /// a sub-parser reports file-absolute line numbers.
  ///
  /// If more than `max_bytes` would be scanned, the lexer state is fully
  /// restored and false is returned with *out_too_big = true (the caller
  /// parses the prim inline and its CHILDREN get their own capture attempts).
  /// Blocks smaller than `min_bytes`, blocks containing a C-style block
  /// comment, and malformed (unterminated) blocks are restored likewise
  /// (*out_too_big = false), leaving them to the inline parser. The capture
  /// is only a boundary guess: whoever re-parses the span must verify it
  /// parses cleanly as whole prim blocks.
  bool capture_prim_block(size_t min_bytes, size_t max_bytes,
                          const char** out_block, size_t* out_len,
                          size_t* out_line, size_t* out_column,
                          bool* out_too_big);

  /// Override the 1-based source location the lexer reports for the FIRST
  /// input byte. Used by sub-parsers running on a slice of a larger file so
  /// their diagnostics carry file-absolute line numbers.
  void set_source_location(size_t line, size_t column) {
    line_ = line;
    column_ = column;
  }

  /// Complete scanning state (position, location, peeked token, error state)
  /// for an exact rewind by restore_state(). Unlike set_position(), a
  /// restore keeps line numbering intact.
  struct SavedState {
    size_t pos = 0;
    size_t line = 1;
    size_t column = 1;
    Token current;
    bool has_current = false;
    std::string error;
    bool fatal = false;
    size_t token_start = 0;
  };
  SavedState save_state() const {
    SavedState s;
    s.pos = pos_;
    s.line = line_;
    s.column = column_;
    s.current = current_;
    s.has_current = has_current_;
    s.error = error_;
    s.fatal = fatal_;
    s.token_start = token_start_;
    return s;
  }
  void restore_state(const SavedState& s) {
    pos_ = s.pos;
    line_ = s.line;
    column_ = s.column;
    current_ = s.current;
    has_current_ = s.has_current;
    error_ = s.error;
    fatal_ = s.fatal;
    token_start_ = s.token_start;
  }

  /// True once set_position() has rewound the lexer (which also resets the
  /// line counter). The parallel prim parse treats such a sub-parse as
  /// unsafe to merge (its later line numbers would differ from a serial
  /// parse of the whole file) and falls back to the serial parser.
  bool position_reset() const { return position_reset_; }

  /// Get error message if in error state
  const std::string& error() const { return error_; }

  /// Check if lexer is in error state
  bool has_error() const { return !error_.empty(); }

  /// Set error message
  void set_error(const std::string& msg);

  /// Fatal lexical malformation (unterminated string / oversized token):
  /// unlike the recoverable `expect()` mismatch errors, these mean the token
  /// stream itself is broken and the parse must fail even if the token-level
  /// grammar happens to recover.
  bool has_fatal_error() const { return fatal_; }
  void set_fatal_error(const std::string& msg);

  /// Byte offset (into the input buffer) where the current token — the one
  /// returned by the latest peek()/next() — starts, AFTER leading whitespace
  /// and comments. Used to capture the raw source text of skipped values.
  size_t token_start() const { return token_start_; }

  /// Raw input buffer (for raw-text slices via token_start()/position()).
  const char* input_data() const { return data_; }

  /// Maximum accepted length for a single token (string literal, identifier,
  /// number, path/asset reference). Longer tokens are a fatal lex error.
  static constexpr size_t kMaxTokenLength = 16u * 1024u * 1024u;  // 16MB

  /// Worker-thread hint for the parallel large-array parse path, forwarded from
  /// ParseOptions::num_threads (0 = auto, 1 = serial, >1 = that many). Carried on
  /// the lexer because the stateless value-parser array helpers receive only the
  /// lexer; replaces the former LIGHTUSD_NEXT_NUM_THREADS env read.
  int num_threads = 0;

  /// Reject invalid AOUSD paths and malformed Unicode immediately.
  bool strict_aousd_conformance = false;

private:
  const char* data_;
  size_t length_;
  size_t pos_ = 0;
  size_t line_ = 1;
  size_t column_ = 1;

  Token current_;
  bool has_current_ = false;
  std::string error_;
  bool fatal_ = false;
  bool position_reset_ = false;
  size_t token_start_ = 0;

  // Per-byte primitives, inline: they sit in every scanning loop.
  void advance() {
    if (pos_ < length_) {
      if (data_[pos_] == '\n') {
        line_++;
        column_ = 1;
      } else {
        column_++;
      }
      pos_++;
    }
  }
  // Skip a quoted string / asset literal starting at the current opening
  // delimiter (raw byte skipping, no token production; same termination rules
  // as capture_bracketed_literal). Used by capture_prim_block.
  void skip_quoted_raw();
  void skip_asset_raw();
  char current_char() const {
    return (pos_ < length_) ? data_[pos_] : '\0';
  }
  char peek_char(size_t offset = 1) const {
    if (offset > static_cast<size_t>(-1) - pos_) return '\0';
    const size_t idx = pos_ + offset;
    return (idx < length_) ? data_[idx] : '\0';
  }
  void skip_whitespace();
  void skip_comment();

  Token scan_token();
  Token scan_identifier();
  Token scan_number();
  Token scan_string();
  Token scan_path_ref();
  Token scan_asset_ref();

  Token make_token(TokenType type, size_t start_line, size_t start_col);
  Token make_token(TokenType type, std::string value, size_t start_line, size_t start_col);
};

/// Get string name for token type (for debugging)
const char* TokenTypeName(TokenType type);

}  // namespace next
}  // namespace lightusd
