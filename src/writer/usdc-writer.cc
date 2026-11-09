// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Writer Implementation

#include "usdc-writer.hh"
#include "usda-writer.hh"
#include <algorithm>
#include <cctype>

namespace lightusd {
namespace next {

namespace {

// Convert CrateWriteResult to USDCWriteResult
USDCWriteResult ConvertResult(const CrateWriteResult& crate_result) {
  USDCWriteResult result;
  result.success = crate_result.success;
  result.error = crate_result.error;
  result.bytes_written = crate_result.bytes_written;
  result.token_count = crate_result.token_count;
  result.path_count = crate_result.path_count;
  result.spec_count = crate_result.spec_count;
  return result;
}

// Get file extension (lowercase)
std::string GetExtension(const std::string& path) {
  size_t dot_pos = path.rfind('.');
  if (dot_pos == std::string::npos || dot_pos == path.size() - 1) {
    return "";
  }
  std::string ext = path.substr(dot_pos + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return ext;
}

// Replace extension
std::string ReplaceExtension(const std::string& path, const std::string& new_ext) {
  size_t dot_pos = path.rfind('.');
  if (dot_pos == std::string::npos) {
    return path + "." + new_ext;
  }
  return path.substr(0, dot_pos + 1) + new_ext;
}

}  // anonymous namespace

USDCWriteResult WriteUSDCToFile(const std::string& filename, const Stage& stage,
                                 const USDCWriteOptions& options) {
  return WriteUSDCToFile(filename.c_str(), stage, options);
}

USDCWriteResult WriteUSDCToFile(const char* filename, const Stage& stage,
                                 const USDCWriteOptions& options) {
  USDCWriteResult result;

  if (!filename) {
    result.error = "Null filename";
    return result;
  }

  CrateWriter writer(options.crate_options);
  CrateWriteResult crate_result = writer.WriteToFile(filename, stage);
  result = ConvertResult(crate_result);

  // Optionally write USDA debug file
  if (result.success && options.write_usda_debug) {
    std::string usda_path = ReplaceExtension(filename, "usda");
    WriteUSDAToFile(usda_path, stage);
  }

  return result;
}

USDCWriteResult WriteUSDCToMemory(std::vector<uint8_t>& buffer, const Stage& stage,
                                   const USDCWriteOptions& options) {
  CrateWriter writer(options.crate_options);
  CrateWriteResult crate_result = writer.WriteToMemory(buffer, stage);
  return ConvertResult(crate_result);
}

USDCWriteResult WriteLayerToUSDCFile(const std::string& filename, const Layer& layer,
                                      const USDCWriteOptions& options) {
  USDCWriteResult result;

  CrateWriter writer(options.crate_options);
  CrateWriteResult crate_result = writer.WriteLayerToFile(filename.c_str(), layer);
  result = ConvertResult(crate_result);

  // Optionally write USDA debug file
  if (result.success && options.write_usda_debug) {
    std::string usda_path = ReplaceExtension(filename, "usda");
    WriteLayerToFile(usda_path, layer);
  }

  return result;
}

USDCWriteResult WriteLayerToUSDCMemory(std::vector<uint8_t>& buffer, const Layer& layer,
                                        const USDCWriteOptions& options) {
  CrateWriter writer(options.crate_options);
  CrateWriteResult crate_result = writer.WriteLayerToMemory(buffer, layer);
  return ConvertResult(crate_result);
}

bool IsUSDCPath(const std::string& path) {
  std::string ext = GetExtension(path);
  return ext == "usdc" || ext == "usd";  // .usd can be either, but often binary
}

bool IsUSDCPath(const char* path) {
  if (!path) return false;
  return IsUSDCPath(std::string(path));
}

}  // namespace next
}  // namespace lightusd
