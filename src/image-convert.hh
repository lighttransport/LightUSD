// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "image-types.hh"
#include "nonstd/expected.hpp"
namespace lightusd::image {
struct ConvertOptions {
  int max_size{0}, width{0}, height{0};
  std::string format{"png"}, png_encoder{"auto"};
  int jpeg_quality{90};
  bool low_memory{false};
  std::string colorspace, resize_colorspace;
  size_t memory_budget_bytes{size_t(512) << 20};
};
struct ConvertResult {
  std::vector<uint8_t> data;
  int width{0}, height{0};
  bool resized{false};
};
// Common native/WASM conversion. Input, decoder scratch, resize and encoded
// output are checked against a finite working-memory budget.
nonstd::expected<ConvertResult, std::string> ConvertImage(
    const uint8_t* bytes, size_t size, const ConvertOptions& options,
    const std::string& name = "memory");
// Preserve HDR/half for EXR and uint16 for PNG; prepare PNG/JPEG LDR output
// using the converter's ACES fitted tonemap and linear alpha convention.
nonstd::expected<Image, std::string> PrepareImageForEncoding(
    Image image, const std::string& format, size_t memory_budget_bytes);
}  // namespace lightusd::image
