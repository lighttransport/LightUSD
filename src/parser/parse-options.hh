// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace lightusd {
namespace next {

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

  /// Inputs below this byte size parse serially (0 leaves the hint unchanged).
  /// Useful when an outer loader reserves parallel parsing for large layers.
  size_t min_parallel_parse_bytes = 0;

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

  /// Opt-in phase diagnostics through the library logger.
  bool enable_timing = false;
};


  // Error information from parsing (lightweight)
  struct ParseError {
    size_t line = 0;
    size_t column = 0;
    std::string message;
  };
}  // namespace next
}  // namespace lightusd

