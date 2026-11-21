// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - USDC Writer
// High-level API for writing USDC (binary) files

#pragma once

#include "stage/stage.hh"
#include "crate/crate-writer.hh"
#include <string>
#include <vector>

namespace lightusd {

/// Options for USDC output
struct USDCWriteOptions {
  /// Crate format options
  CrateWriteOptions crate_options;

  /// Also write USDA alongside (for debugging)
  bool write_usda_debug = false;
};

/// Result of USDC write operation
struct USDCWriteResult {
  bool success = false;
  std::string error;
  size_t bytes_written = 0;

  /// Statistics from crate writer
  size_t token_count = 0;
  size_t path_count = 0;
  size_t spec_count = 0;
};

/// Write Stage to USDC file
USDCWriteResult WriteUSDCToFile(const std::string& filename, const Stage& stage,
                                 const USDCWriteOptions& options = {});
USDCWriteResult WriteUSDCToFile(const char* filename, const Stage& stage,
                                 const USDCWriteOptions& options = {});

/// Write Stage to USDC memory buffer
USDCWriteResult WriteUSDCToMemory(std::vector<uint8_t>& buffer, const Stage& stage,
                                   const USDCWriteOptions& options = {});

/// Write Layer to USDC file
USDCWriteResult WriteLayerToUSDCFile(const std::string& filename, const Layer& layer,
                                      const USDCWriteOptions& options = {});

/// Write Layer to USDC memory buffer
USDCWriteResult WriteLayerToUSDCMemory(std::vector<uint8_t>& buffer, const Layer& layer,
                                        const USDCWriteOptions& options = {});

/// Check if a file path looks like a USDC file (by extension)
bool IsUSDCPath(const std::string& path);
bool IsUSDCPath(const char* path);

}  // namespace lightusd
