// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Crate Writer Implementation
// Writes spec-compliant USDC with: tokens, strings, paths, fields,
// fieldsets, specs, VALUE data section, time samples, metadata.

#include "crate-writer.hh"
#include "../layer/array-edit.hh"
#include "../parser/lexer.hh"
#include "../parser/value-parser.hh"
#include "crate-data-source.hh"
#include "crate-timing.hh"
#include "variant-holders.hh"
#include "crate-writer-types.hh"
#include "lazy-array.hh"
#include "safe-arithmetic.hh"
#include "stream-writer.hh"
#include "../layer/property-index.hh"
#include "../parser/ascii-parser.hh"
#include "../types/spline.hh"
#include "../types/type-id.hh"
#include "../writer/value-printer.hh"
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <deque>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <cstdio>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif

// LZ4 for compression
#include "lz4/lz4.h"

namespace lightusd {
namespace next {

namespace {

bool ValidateStrictCrateFields(const Layer& layer, std::string* error) {
  for (const PrimSpec& prim : layer.prims()) {
    for (const PropSlot& slot : prim.properties().slots()) {
      const std::string name(GetPropNameTable().get(slot.name_id));
      if (const std::string* spline_text = prim.spline_source(slot.name_id)) {
        // Typed splines encode as Crate type 59; only a spline whose raw
        // text does not parse is unencodable.
        SplineData sd;
        std::string serr;
        if (!ParseSplineText(*spline_text, &sd, &serr)) {
          if (error) *error =
              "Strict AOUSD USDC write cannot encode malformed spline " +
              prim.path().str() + "." + name + ": " + serr;
          return false;
        }
      }
      if (prim.raw_default_source(slot.name_id) &&
          !prim.array_edit(slot.name_id)) {
        // A sparse array edit also carries its canonical text as a raw
        // default source, but it has a real crate encoding (VtArrayEdit rep,
        // crate 0.14) -- only raw text WITHOUT a structured twin is lossy.
        if (error) *error =
            "Strict AOUSD USDC write cannot encode raw unsupported value " +
            prim.path().str() + "." + name;
        return false;
      }
    }
  }
  return true;
}

// File output for the streaming write: appends go through a fixed staging
// buffer (large pass-through blocks bypass it), so a file write never holds
// the whole crate in memory.
class BufferedFileSink {
 public:
  explicit BufferedFileSink(std::ofstream* ofs) : ofs_(ofs) {
    buf_.reserve(kCapacity);
  }
  bool Write(const uint8_t* data, size_t size) {
    if (size == 0) return true;
    if (buf_.size() + size > kCapacity && !Flush()) return false;
    if (size >= kCapacity) {
      ofs_->write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(size));
      return ofs_->good();
    }
    buf_.insert(buf_.end(), data, data + size);
    return true;
  }
  bool Flush() {
    if (!buf_.empty()) {
      ofs_->write(reinterpret_cast<const char*>(buf_.data()),
                  static_cast<std::streamsize>(buf_.size()));
      buf_.clear();
    }
    return ofs_->good();
  }

 private:
  static constexpr size_t kCapacity = size_t(4) << 20;
  std::ofstream* ofs_;
  std::vector<uint8_t> buf_;
};

}  // namespace


#include "crate-writer-impl.inc"

CrateWriter::CrateWriter(const CrateWriteOptions& options)
    : impl_(new Impl(options)) {}

CrateWriter::~CrateWriter() = default;

CrateWriteResult CrateWriter::WriteToFile(const char* filename, const Stage& stage) {
  if (!filename) { CrateWriteResult r; r.error = "Null filename"; return r; }
  const Layer* root_layer = stage.GetRootLayer();
  if (!root_layer) { CrateWriteResult r; r.error = "Stage has no root layer"; return r; }
  return WriteLayerToFile(filename, *root_layer);
}

CrateWriteResult CrateWriter::WriteToFile(const std::string& filename, const Stage& stage) {
  return WriteToFile(filename.c_str(), stage);
}

CrateWriteResult CrateWriter::WriteToMemory(std::vector<uint8_t>& buffer, const Stage& stage) {
  const Layer* root_layer = stage.GetRootLayer();
  if (!root_layer) { CrateWriteResult r; r.error = "Stage has no root layer"; return r; }
  return WriteLayerToMemory(buffer, *root_layer);
}

CrateWriteResult CrateWriter::WriteLayerToFile(const char* filename, const Layer& layer) {
  if (!filename) { CrateWriteResult r; r.error = "Null filename"; return r; }
  // Stream straight to the file (byte-identical to WriteLayerToMemory): the
  // VALUE section is copied block by block from its sources instead of first
  // being staged, with the whole crate, in one output buffer.
  std::ofstream ofs(filename, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!ofs) { CrateWriteResult r; r.error = "Failed to open file"; return r; }
  BufferedFileSink file_sink(&ofs);
  CrateWriteSink sink = [&file_sink](const uint8_t* data, size_t size) {
    return file_sink.Write(data, size);
  };
  CrateWriteResult result = WriteLayerToSink(sink, layer);
  const bool flushed = file_sink.Flush();
  ofs.close();
  if (result.success && (!flushed || ofs.fail())) {
    result.success = false;
    result.error = "Failed to write";
  }
  if (!result.success) {
    // Do not leave a truncated crate behind.
    std::remove(filename);
  }
  return result;
}

CrateWriteResult CrateWriter::WriteLayerToMemory(std::vector<uint8_t>& buffer, const Layer& layer) {
  if (impl_->strict_aousd_conformance()) {
    CrateWriteResult strict_result;
    if (!ValidateStrictCrateFields(layer, &strict_result.error)) {
      return strict_result;
    }
  }
  // Inline-authored variants (VariantSetData without bracketed holder prims)
  // must be materialized into holder prims or the crate drops them.
  if (LayerNeedsVariantHolders(layer)) {
    Layer materialized = MaterializeVariantHolders(layer);
    CrateWriteResult result = impl_->Write(materialized);
    if (result.success) buffer = impl_->take_buffer();
    return result;
  }
  CrateWriteResult result = impl_->Write(layer);
  if (result.success) buffer = impl_->take_buffer();
  return result;
}

CrateWriteResult CrateWriter::WriteLayerToSink(const CrateWriteSink& sink, const Layer& layer) {
  if (impl_->strict_aousd_conformance()) {
    CrateWriteResult strict_result;
    if (!ValidateStrictCrateFields(layer, &strict_result.error)) {
      return strict_result;
    }
  }
  // Impl::Write streams bootstrap/VALUE/structural/TOC to `sink` in file order
  // when a sink is supplied; buffer_ only ever holds the small structural tail.
  if (LayerNeedsVariantHolders(layer)) {
    Layer materialized = MaterializeVariantHolders(layer);
    return impl_->Write(materialized, &sink);
  }
  return impl_->Write(layer, &sink);
}

}  // namespace next
}  // namespace lightusd
