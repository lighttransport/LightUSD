// SPDX-License-Identifier: Apache-2.0
#include "core/native-io.hh"
#include "format-detect.hh"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace lightusd {

FileFormat DetectFormat(const uint8_t* data, size_t size) {
  if (size < 8) return FileFormat::Unknown;

  // USDC (Crate) magic: "PXR-USDC"
  if (std::memcmp(data, "PXR-USDC", 8) == 0) {
    return FileFormat::USDC;
  }

  // USDZ (ZIP archive) magic: "PK\x03\x04"
  if (data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04) {
    return FileFormat::USDZ;
  }

  // USDA is text - check for ASCII/UTF-8
  // Look for #usda header or common patterns
  bool is_text = true;
  for (size_t i = 0; i < std::min(size, size_t(1024)); ++i) {
    uint8_t c = data[i];
    if (c < 0x09 || (c > 0x0D && c < 0x20 && c != 0x1B)) {
      is_text = false;
      break;
    }
  }

  if (is_text) {
    return FileFormat::USDA;
  }

  return FileFormat::Unknown;
}

FileFormat DetectFormatFromExtension(const std::string& filename) {
  size_t dot = filename.rfind('.');
  if (dot == std::string::npos) return FileFormat::Unknown;

  std::string ext = filename.substr(dot);
  // Convert to lowercase
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c += 32;
  }

  if (ext == ".usda") return FileFormat::USDA;
  if (ext == ".usdc") return FileFormat::USDC;
  if (ext == ".usdz") return FileFormat::USDZ;
  if (ext == ".usd") return FileFormat::Unknown;  // Need content detection

  return FileFormat::Unknown;
}

FileFormat DetectFormatFromFileHeader(const std::string& filename, std::string* err) {
  if (!::lightusd::NativeIOAvailable(err)) return FileFormat::Unknown;

  std::ifstream ifs(filename, std::ios::binary);
  if (!ifs) {
    if (err) *err = "Failed to open file for format detection: " + filename;
    return FileFormat::Unknown;
  }

  uint8_t header[4096] = {};
  ifs.read(reinterpret_cast<char*>(header), sizeof(header));
  const std::streamsize nread = ifs.gcount();
  if (nread <= 0) {
    if (err) *err = "Failed to read file header: " + filename;
    return FileFormat::Unknown;
  }
  return DetectFormat(header, static_cast<size_t>(nread));
}

}  // namespace lightusd
