// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII Parser
// High-level parser for USD ASCII format files

#pragma once

#include "../stage/stage.hh"
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
struct ParseOptions {
  /// Enforce AOUSD Core grammar and fail rather than accepting constructs that
  /// next cannot interpret conformantly. The default keeps legacy permissive
  /// ingestion, but lossy constructs are still preserved and diagnosed.
  bool strict_aousd_conformance = false;

  /// Allow unknown/unrecognized prim types (stored as generic prims)
  bool allow_unknown_types = true;

  /// Maximum file size to parse (0 = no limit)
  size_t max_file_size = 0;

  /// Enable USDA array lazy-materialization mode.
  ///
  /// When true, numeric/textual array literals may be kept as lazy references
  /// against a retained input-buffer copy. Values are parsed and decoded on first
  /// materialization through a lazy `Value::materialize()` path.
  bool enable_usda_lazy_arrays = false;

  /// Maximum elements accepted for USDA array lazy-materialization (hard cap per
  /// array). Arrays larger than this are parsed eagerly. 0 means no cap.
  size_t max_usda_lazy_array_elements = (static_cast<size_t>(1) << 30);

  /// Maximum nesting depth for prims
  size_t max_depth = 256;

  /// Worker-thread hint for the parallel large-array parse path (when built with
  /// LIGHTUSD_ENABLE_THREAD). 0 = auto (min(hardware_concurrency, 8)); 1 =
  /// serial; >1 = that many workers. Replaces the former LIGHTUSD_NEXT_NUM_THREADS
  /// env read so the library takes no implicit process-environment input.
  int num_threads = 0;

  /// Parse captured simple numeric arrays (attribute defaults AND timeSample
  /// values) on the parser worker pool, batched, while the main thread keeps
  /// lexing; payloads are filled in place and joined before finalize.
  /// Requires LIGHTUSD_ENABLE_THREAD and more than one parse thread; inactive
  /// with enable_usda_lazy_arrays or a progress_callback. The result is
  /// identical to the synchronous parse: any failure re-runs the parse
  /// serially, so errors and warnings are always the serial parser's.
  bool async_arrays = true;

  /// Parse mid-size prim subtrees on the parser worker pool: the main thread
  /// captures each prim block (SIMD brace matching) and workers parse blocks
  /// into layer fragments that are spliced back (authored order and serial
  /// prim order preserved) before finalize. Same conditions and the same
  /// serial-fallback guarantee as async_arrays.
  bool parallel_prims = true;

  /// Optional coarse parse progress callback. Reports bootstrap, after each
  /// completed prim (including nested prims), and completion. Returning false
  /// cancels before the parsed Stage is published.
  std::function<bool(const char* phase, size_t current, size_t total)>
      progress_callback;
};

/// Error information from parsing
struct ParseError {
  size_t line = 0;
  size_t column = 0;
  std::string message;
};

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
