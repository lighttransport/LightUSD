// SPDX-License-Identifier: Apache-2.0
// USD file-format detection (magic bytes / extension / file header), extracted
// from the lightusd-next facade load path.
#pragma once
#include <cstdint>
#include <string>

namespace lightusd {
namespace next {

enum class FileFormat {
  Unknown,
  USDA,
  USDC,
  USDZ
};

FileFormat DetectFormat(const uint8_t* data, size_t size);
FileFormat DetectFormatFromExtension(const std::string& filename);
FileFormat DetectFormatFromFileHeader(const std::string& filename, std::string* err);

}  // namespace next
}  // namespace lightusd
