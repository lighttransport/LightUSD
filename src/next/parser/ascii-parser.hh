// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII Parser
// High-level parser for USD ASCII format files

#pragma once

#include "../stage/stage.hh"
#include "parse-options.hh"
#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace lightusd {
namespace next {

class Lexer;
struct ArrayEditData;

/// Parse a VtArrayEdit value `edit [ <op>; ... ]` from `lexer` (positioned at
/// the `edit` keyword). Literals are validated against `elem_type`; returns
/// the canonical one-line spelling and, when `out_edit` is non-null, the
/// structured op list.
bool ParseArrayEditText(Lexer& lexer, TypeId elem_type, std::string* canonical,
                        ArrayEditData* out_edit, std::string* err);

/// Options for parsing USDA files
// (moved below includes)

/// Error information from parsing

/// USDA ASCII Parser
/// Parses USD ASCII format files into a Stage
class AsciiParser {
public:
  /// Construct with options
  explicit AsciiParser(const ParseOptions& options = {});

  /// Destructor
  ~AsciiParser();

  /// No copy
  AsciiParser(const AsciiParser&) = delete;
  AsciiParser& operator=(const AsciiParser&) = delete;

  /// Move is allowed
  AsciiParser(AsciiParser&&) noexcept;
  AsciiParser& operator=(AsciiParser&&) noexcept;

  // ============================================================
  // Parsing
  // ============================================================

  /// Parse from string data
  bool Parse(const char* data, size_t length);

  /// Parse from an owned string buffer. When USDA lazy arrays are enabled, the
  /// parser adopts this buffer as the retained source for lazy array slices,
  /// avoiding a second in-heap copy on memory-constrained targets such as WASM.
  bool ParseOwned(std::string&& data);

  /// Parse from a file
  bool ParseFile(const char* filename);

  // ============================================================
  // Results
  // ============================================================

  /// Get the parsed stage (moves ownership to caller)
  Stage TakeStage();

  /// Get parse errors
  const std::vector<ParseError>& GetErrors() const;

  /// Check if parsing succeeded
  bool HasErrors() const;

  /// Get warning messages
  const std::vector<std::string>& GetWarnings() const;

  /// True when the last parse completed on the batched/parallel fast path
  /// (ParseOptions::async_arrays / parallel_prims) rather than the serial
  /// parser. Diagnostics/testing only: results are identical either way.
  bool UsedFastPath() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace next
}  // namespace lightusd
