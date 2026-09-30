// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate Reader
// Binary USD file parser

#pragma once

#include "crate-format.hh"
#include "crate-limits.hh"
#include "../stage/stage.hh"
#include <functional>
#include <string>
#include <vector>
#include <memory>

#include "../../security-policy.hh"

namespace lightusd {
namespace next {

/// Options for reading crate files. The structural table-count limits
/// (max_tokens ... max_path_depth) come from CrateLimits.
struct CrateReadOptions : CrateLimits {
  /// Fail closed when any field/value must be ignored or approximated. This
  /// turns reader warnings into errors for AOUSD conformance-sensitive loads.
  bool strict_aousd_conformance = false;

  /// Optional progress callback. `phase` is a stable short string, `current`
  /// and `total` are phase-local counts when available. Return false to cancel.
  std::function<bool(const char* phase, size_t current, size_t total)>
      progress_callback;

  /// Maximum number of elements in a single value array
  /// (mirrors pxrUSD/legacy core's 1<<30 cap; guards against a malformed
  /// count triggering an enormous allocation).
  size_t max_array_elements = 16 * 1024 * 1024;

  /// Cumulative allocation budget for the read and later lazy materializations
  /// (bytes, 0 disables the explicit budget; file-relative guards remain).
  /// Temporary decode buffers count too. Deferred copies of a value share the
  /// retained source's budget, which outlives the reader and is not refunded.
  size_t max_memory = security_policy::kDefaultInputLimitBytes;

  /// Keep numeric POD arrays as lazy references into the retained source buffer
  /// instead of eagerly decoding them (low-memory load/compose/write path).
  /// Set false to force eager decode (e.g. for A/B memory comparisons).
  bool lazy_arrays = true;

  /// Log per-phase wall times ("[next_crate_read] ...", "[next_crate_stage]
  /// ...") at INFO through lightusd::logging.
  bool enable_timing = false;

  /// Worker-thread hint for the parallel stage build (0 = auto min(hw, 8),
  /// 1 = serial, >1 = fixed, clamped). Output is identical at every count;
  /// small crates always build serially. Ignored without LIGHTUSD_ENABLE_THREAD.
  int num_threads = 0;

  /// When reading from a file path, memory-map the crate read-only instead of
  /// copying it into an owned heap buffer (Phase 8.3). Falls back to the owned
  /// path automatically when mmap is unavailable (non-posix / WASM) or the
  /// mapping fails. The mapping stays alive as long as any lazy value (or the
  /// reader) references it. Set false to force the owned-buffer path.
  ///
  /// SET THIS FALSE FOR UNTRUSTED INPUT ON A SHARED FILESYSTEM. A mapping is
  /// sized once, at open, and every bounds check in the reader is against that
  /// size. If another process truncates the file while the mapping is alive,
  /// touching a page past the new EOF raises SIGBUS -- a hard crash that no
  /// bounds check can prevent and that this reader cannot turn into a clean
  /// error. Because lazy array values keep the mapping alive, the window lasts
  /// well beyond the load itself. The owned-buffer path reads the bytes once
  /// up front and is immune. A truncation noticed during the load is reported
  /// as a warning (see CrateDataSource::MappedFileShrank), but that is
  /// after-the-fact detection, not protection.
  bool use_mmap = true;
};

/// Error from crate reading
struct CrateError {
  size_t offset = 0;    // File offset where error occurred
  std::string message;
};

/// Result of reading a crate file
struct CrateReadResult {
  bool success = false;
  Stage stage;
  std::vector<CrateError> errors;
  std::vector<std::string> warnings;
  CrateVersion version;
  bool source_was_mmap = false;
};

/// USDC Crate file reader
class CrateReader {
public:
  explicit CrateReader(const CrateReadOptions& options = {});
  ~CrateReader();

  /// No copy
  CrateReader(const CrateReader&) = delete;
  CrateReader& operator=(const CrateReader&) = delete;

  /// Move allowed
  CrateReader(CrateReader&&) noexcept;
  CrateReader& operator=(CrateReader&&) noexcept;

  /// Read from memory buffer (copies the input into a retained buffer).
  CrateReadResult Read(const uint8_t* data, size_t size);

  /// Read from an owned buffer, adopted by move (single in-heap copy). Prefer
  /// this on memory-constrained targets (e.g. WASM) when the caller can give up
  /// ownership of the input bytes.
  CrateReadResult ReadOwned(std::string&& owned);

  /// Read from file
  CrateReadResult ReadFile(const char* filename);

  // ============================================================
  // Low-level access (for debugging/testing)
  // ============================================================

  /// Get tokens table
  // Materialized copy of the token table (diagnostics only; the reader stores
  // tokens pooled, see TokenPool in the .cc).
  std::vector<std::string> tokens() const;

  /// Get paths table (materialized copy; the reader stores paths pooled)
  std::vector<std::string> paths() const;

  /// Get fields table
  const std::vector<CrateField>& fields() const;

  /// Get specs table
  const std::vector<CrateSpec>& specs() const;

  /// Get fieldset-indices table (flattened; 0xFFFFFFFF terminates each set)
  const std::vector<uint32_t>& fieldset_indices() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Check if data looks like a USDC file (check magic bytes)
bool IsUSDCData(const uint8_t* data, size_t size);

/// Check if file is a USDC file
bool IsUSDCFile(const char* filename);

}  // namespace next
}  // namespace lightusd
