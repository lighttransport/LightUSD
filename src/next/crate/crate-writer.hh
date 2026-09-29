// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Crate Writer
// Low-level binary USDC format writer

#pragma once

#include "crate-format.hh"
#include "../layer/layer.hh"
#include "../stage/stage.hh"
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

namespace lightusd {
namespace next {

/// Output sink for streaming crate writes: receives the file bytes in order, in
/// chunks. Returns false to abort the write. See WriteLayerToSink().
using CrateWriteSink = std::function<bool(const uint8_t* data, size_t size)>;

/// Seek-write for a seekable sink: overwrite `size` bytes at absolute output
/// position `pos` (bytes already emitted through the sink). Returns false to
/// abort. Used internally for file output (see WriteLayerToFile).
using CrateWritePatch =
    std::function<bool(uint64_t pos, const uint8_t* data, size_t size)>;

/// Options for crate writing
struct CrateWriteOptions {
  /// Refuse authored fields that cannot be represented in the selected Crate
  /// version instead of silently omitting or approximating them.
  bool strict_aousd_conformance = false;

  /// Version to write (default 0.8.0 for broad compatibility)
  uint8_t version_major = 0;
  uint8_t version_minor = 8;
  uint8_t version_patch = 0;

  /// Compress large arrays with LZ4
  bool compress_arrays = true;

  /// Minimum array size to compress (in bytes)
  size_t compression_threshold = 256;

  /// Write string tokens inline if small enough
  bool inline_small_values = true;

  /// Maximum inline value size (bytes)
  size_t max_inline_size = 8;

  /// Stream the output to a sink instead of materializing the whole crate in a
  /// single buffer. Only the (small) structural sections are staged in memory;
  /// the (large) VALUE section is streamed block-by-block straight from the
  /// retained source buffer. Used by WriteLayerToSink(); see that method.
  bool streaming = false;

  /// Worker count for parallel build/sort paths (1 = serial; <=0 = auto, capped).
  /// Only effective in a LIGHTUSD_ENABLE_THREAD build. Output is a valid
  /// round-trippable crate at any thread count and is byte-identical across thread
  /// counts (the parallel build merges per-prim results in deterministic order).
  int num_threads = 1;

  /// Log per-phase wall times ("[next_crate_write] ...") at INFO through
  /// lightusd::logging.
  bool enable_timing = false;

  /// Consume the input layer's property values while writing: after a prim's
  /// fields are built (its values encoded into the crate's value blocks), the
  /// prim's default values and time samples are released
  /// (PrimSpec::release_value_payloads), so the layer's materialized values
  /// and the staged value blocks never coexist in full -- a peak-RSS cut on
  /// write-and-discard flows (flatten-to-file). The layer stays structurally
  /// valid but value-stripped (AssetPath defaults are kept for post-write
  /// asset collection). Requires the caller's layer to be genuinely mutable;
  /// output bytes are identical to a non-consuming write. Default off.
  bool consume_values = false;

  /// Maximum complete crate size in bytes (0 = unlimited). The writer stops
  /// before growing the output buffer beyond this bound.
  uint64_t max_file_size_bytes = 0;

  /// Maximum estimated writer working set in bytes (0 = unlimited). This is
  /// based on retained Layer data plus Crate table/index overhead; it is a
  /// policy estimate, not an operating-system RSS limit.
  uint64_t max_memory_bytes = 0;
};

/// Result of crate write operation
struct CrateWriteResult {
  bool success = false;
  std::string error;
  size_t bytes_written = 0;

  /// Statistics
  size_t token_count = 0;
  size_t string_count = 0;
  size_t path_count = 0;
  size_t spec_count = 0;
  size_t field_count = 0;

  /// Lazy-array write accounting: arrays copied verbatim from the source crate
  /// (byte pass-through) vs. arrays decoded and re-encoded.
  size_t arrays_passed_through = 0;
  size_t arrays_reencoded = 0;

  /// VALUE blocks elided because an identical-bytes block was already written
  /// (cross-spec content dedup).
  size_t blocks_deduped = 0;
};

/// Crate file writer
/// Writes Stage/Layer to binary USDC format
class CrateWriter {
public:
  explicit CrateWriter(const CrateWriteOptions& options = {});
  ~CrateWriter();

  /// Write Stage to a file
  CrateWriteResult WriteToFile(const char* filename, const Stage& stage);
  CrateWriteResult WriteToFile(const std::string& filename, const Stage& stage);

  /// Write Stage to memory buffer
  CrateWriteResult WriteToMemory(std::vector<uint8_t>& buffer, const Stage& stage);

  /// Write Layer to a file (for individual layer export)
  CrateWriteResult WriteLayerToFile(const char* filename, const Layer& layer);

  /// Write Layer to memory buffer
  CrateWriteResult WriteLayerToMemory(std::vector<uint8_t>& buffer, const Layer& layer);

  /// Write Layer to a streaming sink. The crate is emitted in file order
  /// (bootstrap, VALUE section, structural sections, TOC) without ever holding
  /// the full output in memory: peak working set is the (small) structural
  /// sections plus the retained source buffer, with VALUE bytes streamed
  /// straight from their source. `sink` receives ordered byte chunks and
  /// returns false to abort. Output is byte-identical to WriteLayerToMemory.
  CrateWriteResult WriteLayerToSink(const CrateWriteSink& sink, const Layer& layer);

  /// Like WriteLayerToSink, for a sink that can also seek-write (`patch`):
  /// VALUE blocks are streamed as they are built instead of being staged
  /// until the end, so the value section is never held in memory. Output is
  /// byte-identical to WriteLayerToMemory.
  CrateWriteResult WriteLayerToSeekableSink(const CrateWriteSink& sink,
                                            const CrateWritePatch& patch,
                                            const Layer& layer);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace next
}  // namespace lightusd
