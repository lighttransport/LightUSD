// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// LightUSD Crate Dump Library
//
// Library for dumping low-level USDC Crate file structure in YAML or JSON format
// for efficient debugging and investigation.
//

#pragma once

#include <string>

namespace lightusd {
namespace crate {

enum class OutputFormat {
  YAML,
  JSON
};

struct DumpOptions {
  OutputFormat format = OutputFormat::YAML;
  bool show_bootstrap = true;
  bool show_toc = true;
  bool show_tokens = true;
  bool show_strings = true;
  bool show_fields = true;
  bool show_fieldsets = true;
  bool show_paths = true;
  bool show_specs = true;
  bool show_hex = false;
  int max_tokens = -1;  // -1 = unlimited
  int max_strings = -1;
  int max_fields = -1;
  int max_fieldsets = -1;
  int max_paths = -1;
  int max_specs = -1;
  std::string token_filter;  // substring filter for tokens/fields
  std::string path_filter;   // substring filter for paths/spec paths
};

///
/// Dump USDC crate file structure to stdout
///
/// @param[in] filename - Path to USDC file
/// @param[in] opts - Dump options
/// @param[out] err - Error message (if any)
/// @return true on success, false on error
///
bool DumpCrate(const std::string& filename, const DumpOptions& opts, std::string* err);

}  // namespace crate
}  // namespace lightusd
