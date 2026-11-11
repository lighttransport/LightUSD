// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - Prim Printer
// Pretty-print Stage and UsdPrim for debugging

#pragma once

#include "stage/stage.hh"
#include "stream-writer.hh"
#include <string>

namespace lightusd {

/// Options for prim printing
struct PrimPrintOptions {
  /// Indent string (default 4 spaces)
  std::string indent = "    ";

  /// Maximum depth to print (-1 = unlimited)
  int max_depth = -1;

  /// Print property values
  bool print_values = true;

  /// Print metadata
  bool print_metadata = true;

  /// Print relationships
  bool print_relationships = true;

  /// Maximum properties per prim (0 = unlimited)
  size_t max_properties = 0;

  /// Emit the deprecated `custom` qualifier (OFF by default; see USDAWriteOptions).
  bool emit_custom = false;
};

/// Print a single UsdPrim to string
std::string PrintPrim(const UsdPrim& prim, const PrimPrintOptions& opts = {});

/// Print a single UsdPrim to a StreamWriter sink
void PrintPrim(StreamWriter& os, const UsdPrim& prim, const PrimPrintOptions& opts = {});

/// Print entire Stage to string
std::string PrintStage(const Stage& stage, const PrimPrintOptions& opts = {});

/// Print entire Stage to a StreamWriter sink
void PrintStage(StreamWriter& os, const Stage& stage, const PrimPrintOptions& opts = {});

/// Print a PrimSpec directly (lower-level)
std::string PrintPrimSpec(const PrimSpec& spec, const PrimPrintOptions& opts = {});
void PrintPrimSpec(StreamWriter& os, const PrimSpec& spec, int depth, const PrimPrintOptions& opts);

/// Print a Layer to string
std::string PrintLayer(const Layer& layer, const PrimPrintOptions& opts = {});
void PrintLayer(StreamWriter& os, const Layer& layer, const PrimPrintOptions& opts = {});

}  // namespace lightusd
