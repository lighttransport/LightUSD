// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-present Light Transport Entertainment, Inc.
//
// Streaming (scanline-at-a-time) PNG codec built on miniz's streaming zlib.
//
// Motivation: re-encoding / resizing / colorspace-converting a large texture
// through a whole-image decode->process->encode path materializes the full
// W*H*channels buffer (hundreds of MB for big textures) plus a full-image
// encoder buffer. The reader/writer here process ONE scanline at a time, so a
// transform pipeline (decode -> [resize] -> [colorspace] -> encode) peaks at a
// few scanlines + the compressed output instead of the whole image.
//
// Supported: 8-bit gray/gray+alpha/RGB/RGBA and palette, plus 1/2/4/8-bit
// gray/palette, non-interlaced (the same subset the previous in-place
// transcoder handled). Anything else makes Open() return false so the caller
// can fall back to a whole-image codec.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lightusd {
namespace imageio {

struct PngImageInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 0;   // 1/2/4/8 (supported subset)
  uint8_t color_type = 0;  // 0 gray, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA
  uint8_t channels = 0;    // samples per pixel (derived from color_type)
  uint32_t bytes_per_pixel = 0;  // for PNG filtering; >= 1 (1 for sub-byte)
  uint32_t row_bytes = 0;  // unfiltered scanline size (excludes filter byte)
};

// Streaming PNG decoder: parse chunks, inflate IDAT, unfilter — yielding one
// raw (unfiltered, as-stored) scanline at a time. Samples/indices are returned
// verbatim (no palette expansion); call ExpandsPalette()/needs of the consumer
// are handled by the caller.
class PngScanlineReader {
 public:
  PngScanlineReader();
  ~PngScanlineReader();
  PngScanlineReader(const PngScanlineReader&) = delete;
  PngScanlineReader& operator=(const PngScanlineReader&) = delete;

  // Parse the PNG header + chunk table (no pixel work yet). `data` must remain
  // valid for the lifetime of this reader (chunk/IDAT spans point into it).
  // Returns false for non-PNG or unsupported PNGs (caller falls back).
  bool Open(const uint8_t* data, size_t size);
  bool Open(const uint8_t* data, size_t size, size_t max_chunk_metadata_bytes);

  const PngImageInfo& info() const { return info_; }

  // Fill `row` (info().row_bytes bytes) with the next unfiltered scanline.
  // Returns false at end-of-image or on error (see ok()).
  bool NextRow(uint8_t* row);

  bool ok() const { return ok_; }
  uint32_t rows_done() const { return rows_done_; }

  // Original chunks (start ptr + total length incl. len/type/crc) in file
  // order, for callers that re-emit unchanged ancillary/palette chunks
  // (passthrough).
  const std::vector<std::pair<const uint8_t*, size_t>>& chunks() const {
    return chunks_;
  }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  PngImageInfo info_;
  std::vector<std::pair<const uint8_t*, size_t>> chunks_;
  bool ok_ = false;
  uint32_t rows_done_ = 0;
};

// Streaming PNG encoder: accept raw scanlines, pick the best PNG filter per row
// (libpng min-sum-of-absolute-values heuristic), and deflate incrementally.
class PngScanlineWriter {
 public:
  PngScanlineWriter();
  ~PngScanlineWriter();
  PngScanlineWriter(const PngScanlineWriter&) = delete;
  PngScanlineWriter& operator=(const PngScanlineWriter&) = delete;

  // Passthrough mode: copy `templ`'s chunks verbatim on Finish(), replacing the
  // IDAT run with the freshly deflated rows. Produces byte-equivalent metadata
  // to the input (preserves palette, ancillary chunks, bit depth). `templ` must
  // outlive Finish().
  //
  // deflate_level defaults to 6 (zlib default): level 9 is several times
  // slower for a few percent smaller output, and dominated texture re-encode
  // wall time on large scenes (~2 MP/s per 2048^2 texture).
  bool BeginPassthrough(const PngScanlineReader& templ, int deflate_level = 6);
  // Maximum encoded PNG bytes, including metadata. Set before Begin().
  void SetOutputLimit(size_t max_bytes);

  // Fresh mode: synthesize IHDR for a (possibly transformed) image; Finish()
  // emits signature + IHDR + IDAT + IEND only (no palette/ancillary).
  bool Begin(const PngImageInfo& info, int deflate_level = 6);

  // Filter + deflate one scanline (info().row_bytes bytes).
  bool WriteRow(const uint8_t* row);

  // Flush deflate and assemble the final PNG into `out`.
  bool Finish(std::vector<uint8_t>& out);

  bool ok() const { return ok_; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool ok_ = false;
};

enum class ColorspaceXform { SrgbToLinear, LinearToSrgb };

// Streaming PNG -> colorspace-converted PNG (sRGB<->linear) via a per-scanline
// LUT on the color channels (alpha preserved): 256-entry for 8-bit, 65536-entry
// for 16-bit (big-endian samples). Output drops the source's ancillary chunks
// (so no stale color-profile tag remains). Returns false (caller falls back)
// for palette / sub-byte / non-PNG inputs.
bool ConvertColorspacePNG(const uint8_t* data, size_t size, ColorspaceXform xf,
                          std::vector<uint8_t>& out);

// Streaming PNG -> resized PNG. Decodes scanlines, resizes via
// stb_image_resize2 driven by its input/output scanline callbacks, and
// re-encodes — so peak memory is ~the resampler's row window + the compressed
// output, not the full decoded image plus a full-image resize buffer. `srgb`
// selects sRGB-aware filtering (else linear), matching tydra::ResizeImage's two
// paths. Palette (8-bit) is expanded to RGB / RGBA (+tRNS) and the output is
// RGB/RGBA accordingly. Returns false (caller falls back to a whole-image
// resize) for unsupported inputs (sub-byte / 16-bit / non-PNG). out_w/out_h
// must be > 0.
bool ResizePNG(const uint8_t* data, size_t size, uint32_t out_w, uint32_t out_h,
               bool srgb, std::vector<uint8_t>& out);

// Convenience: streaming PNG -> PNG re-encode (filter-optimize + recompress),
// preserving color type / bit depth / palette / ancillary chunks. Peak memory
// is ~a few scanlines + the compressed output. Returns false (caller falls back
// to a whole-image codec) for unsupported PNGs. Byte-equivalent to the previous
// web/binding-legacy.cc transcodePNGStreaming().
bool TranscodePNG(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

// Bounded overloads retain the original entry points for native consumers.
bool TranscodePNG(const uint8_t*, size_t, std::vector<uint8_t>&,
                  size_t max_output_bytes);
bool ResizePNG(const uint8_t*, size_t, uint32_t, uint32_t, bool,
               std::vector<uint8_t>&, size_t max_output_bytes);
bool ConvertColorspacePNG(const uint8_t*, size_t, ColorspaceXform,
                          std::vector<uint8_t>&, size_t max_output_bytes);

}  // namespace imageio
}  // namespace lightusd
