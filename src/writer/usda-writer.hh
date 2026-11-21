// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - USDA Writer
// Export Stage/Layer to USDA ASCII format

#pragma once

#include "stage/stage.hh"
#include "stream-writer.hh"
#include <string>
#include <ostream>

namespace lightusd {

/// Options for USDA output
struct USDAWriteOptions {
  /// Indent string (default 4 spaces)
  std::string indent = "    ";

  /// Float precision (digits after decimal)
  int float_precision = 6;

  /// Double precision (digits after decimal)
  int double_precision = 15;

  /// Maximum array elements emitted before truncating with `, ...`
  /// (0 = unlimited / faithful, round-trippable output). A non-zero value
  /// produces a non-parseable *preview* and should only be used for debugging.
  size_t max_elements_per_line = 0;

  /// Emit the legacy `custom` qualifier on attributes that carry it. OFF by
  /// default: `custom` is a deprecated USD qualifier, so lightusd omits it.
  /// Enable (e.g. under `--openusd-compat`) to byte-match pxr/usdcat output.
  bool emit_custom = false;

  /// Write compact format (less whitespace)
  bool compact = false;

  /// Include comments (doc metadata)
  bool include_comments = true;

  /// Emit a prim's properties sorted by NAME (alphabetical, like pxr usdcat),
  /// rather than in internal name_id (interning/parse) order. Default ON so the
  /// serialized output never depends on the order layers were parsed -- a
  /// prerequisite for parallel composition to be byte-identical to a serial
  /// build (composition interns property names from referenced layers in a
  /// parse-order-dependent way). Set false to preserve raw authored property-child order.
  bool sort_properties = true;

  /// Export time samples as separate lines
  bool expand_time_samples = true;

  /// Number of threads for subtree serialization (only effective when the next
  /// module is built with LIGHTUSD_ENABLE_THREAD). 1 = serial (default, so all
  /// existing callers are unchanged); <= 0 = auto (hardware_concurrency); > 1 =
  /// that many. Output is byte-for-byte identical regardless of thread count.
  int num_threads = 1;

  /// Composed-stage semantics (pxr UsdStage::Flatten parity), for writing a
  /// stage produced by COMPOSITION (pcp::ComposeStageFromFile): consumed
  /// composition directives (layer subLayers/relocates) are omitted and
  /// string list-op prim metadata (apiSchemas) is emitted as its composed
  /// EXPLICIT list instead of the residual authored edit. Leave false for
  /// stages that came from a plain load (LoadUSD) and for Layer writes —
  /// there the directives are not consumed and authored list-op fidelity
  /// must round-trip.
  bool composed_stage_output = false;

  /// Opt-in skeleton/build/format/output timing through the library logger.
  bool enable_timing = false;
};

/// Result of write operation
struct USDAWriteResult {
  bool success = false;
  std::string error;
  size_t bytes_written = 0;
};

/// Write Stage to USDA string
std::string WriteUSDAToString(const Stage& stage,
                               const USDAWriteOptions& options = {});

/// Write Stage to a StreamWriter (stdio-independent core; supply any backend —
/// stdio, file, std::string, or a host-provided WASM/WASI block sink).
USDAWriteResult WriteUSDA(StreamWriter& os, const Stage& stage,
                          const USDAWriteOptions& options = {});

/// Write Stage to a std::ostream (compatibility wrapper over the StreamWriter core).
USDAWriteResult WriteUSDA(std::ostream& os, const Stage& stage,
                           const USDAWriteOptions& options = {});

/// Write Stage to file
USDAWriteResult WriteUSDAToFile(const std::string& filename, const Stage& stage,
                                 const USDAWriteOptions& options = {});
USDAWriteResult WriteUSDAToFile(const char* filename, const Stage& stage,
                                 const USDAWriteOptions& options = {});

/// Write Layer to USDA string (for individual layers)
std::string WriteLayerToString(const Layer& layer,
                                const USDAWriteOptions& options = {});

/// Write Layer to a StreamWriter (stdio-independent core).
USDAWriteResult WriteLayer(StreamWriter& os, const Layer& layer,
                           const USDAWriteOptions& options = {});

/// Write Layer to a std::ostream (compatibility wrapper).
USDAWriteResult WriteLayer(std::ostream& os, const Layer& layer,
                            const USDAWriteOptions& options = {});

/// Write Layer to file
USDAWriteResult WriteLayerToFile(const std::string& filename, const Layer& layer,
                                  const USDAWriteOptions& options = {});

}  // namespace lightusd
