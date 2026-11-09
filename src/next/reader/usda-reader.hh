// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA Reader
// High-level interface for reading USDA files

#pragma once

#include "../stage/stage.hh"
#include "../parser/parse-options.hh"
#include <string>

namespace lightusd {
namespace next {

/// Options for loading USDA files
struct LoadOptions {
  /// Parse options passed to the parser
  ParseOptions parse_options;

  /// Resolve asset references
  bool resolve_assets = false;

  /// Base directory for resolving relative paths
  std::string base_dir;
};

/// Load result containing the stage and any messages
struct LoadResult {
  bool success = false;
  Stage stage;
  std::vector<ParseError> errors;
  std::vector<std::string> warnings;
  std::string error_summary;
};

/// Load a USDA file from disk
LoadResult LoadUSDAFromFile(const char* filename, const LoadOptions& options = {});
LoadResult LoadUSDAFromFile(const std::string& filename, const LoadOptions& options = {});

/// Load a USDA from a string
LoadResult LoadUSDAFromString(const char* data, size_t length, const LoadOptions& options = {});
LoadResult LoadUSDAFromString(const std::string& data, const LoadOptions& options = {});

/// Load a USDA from an owned string buffer adopted by move. Prefer on
/// memory-constrained targets (WASM) when the caller can give up ownership of
/// the input bytes and lazy USDA arrays are enabled.
LoadResult LoadUSDAFromStringOwned(std::string&& data,
                                   const LoadOptions& options = {});

/// Quick check if a file appears to be USDA format (checks magic bytes)

}  // namespace next
}  // namespace lightusd
