// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Zstd compression wrapper implementation for LightUSD

#include "zstd-compression.hh"

#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION

#include <cstring>
#include <limits>

// Use amalgamated single-file zstd
#include "external/zstd.h"

namespace lightusd {

bool ZstdCompression::IsZstdCompressed(const uint8_t *data, size_t length) {
  if (!data || length < 4) {
    return false;
  }
  return (data[0] == kZstdMagic[0] && data[1] == kZstdMagic[1] &&
          data[2] == kZstdMagic[2] && data[3] == kZstdMagic[3]);
}

size_t ZstdCompression::GetDecompressedSize(const uint8_t *compressed,
                                             size_t compressedSize,
                                             std::string *err) {
  if (!compressed || compressedSize == 0) {
    if (err) *err = "Invalid compressed data";
    return 0;
  }

  unsigned long long frameContentSize =
      ZSTD_getFrameContentSize(compressed, compressedSize);

  if (frameContentSize == ZSTD_CONTENTSIZE_ERROR) {
    if (err) *err = "Not a valid zstd compressed frame";
    return 0;
  }

  if (frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
    if (err) *err = "Zstd frame does not contain content size";
    return 0;
  }

  // Check for size overflow
  if (frameContentSize > (std::numeric_limits<size_t>::max)()) {
    if (err) *err = "Decompressed size exceeds size_t range";
    return 0;
  }

  return static_cast<size_t>(frameContentSize);
}

bool ZstdCompression::Decompress(const uint8_t *compressed,
                                  size_t compressedSize,
                                  std::vector<uint8_t> *output,
                                  std::string *err,
                                  size_t maxDecompressedSize) {
  if (!compressed || compressedSize == 0) {
    if (err) *err = "Invalid compressed data";
    return false;
  }

  if (!output) {
    if (err) *err = "Output buffer is null";
    return false;
  }

  // Get decompressed size first
  size_t decompressedSize = GetDecompressedSize(compressed, compressedSize, err);
  if (decompressedSize == 0) {
    // err already set by GetDecompressedSize
    return false;
  }

  // Enforce caller-specified memory budget ceiling
  if (maxDecompressedSize > 0 && decompressedSize > maxDecompressedSize) {
    if (err) {
      *err = "Decompressed size (" + std::to_string(decompressedSize) +
             " bytes) exceeds maximum allowed (" +
             std::to_string(maxDecompressedSize) + " bytes)";
    }
    return false;
  }

  // Allocate output buffer
  // Note: LightUSD has exceptions disabled, so we check capacity instead
  output->clear();
  if (decompressedSize > output->max_size()) {
    if (err) *err = "Decompressed size exceeds maximum allocatable size";
    return false;
  }
  output->resize(decompressedSize);
  if (output->size() != decompressedSize) {
    if (err) *err = "Failed to allocate memory for decompression";
    return false;
  }

  // Decompress
  size_t result =
      ZSTD_decompress(output->data(), decompressedSize, compressed, compressedSize);

  if (ZSTD_isError(result)) {
    if (err) {
      *err = "Zstd decompression failed: ";
      *err += ZSTD_getErrorName(result);
    }
    output->clear();
    return false;
  }

  // Verify decompressed size matches expected
  if (result != decompressedSize) {
    if (err) {
      *err = "Decompressed size mismatch: expected " +
             std::to_string(decompressedSize) + ", got " + std::to_string(result);
    }
    output->clear();
    return false;
  }

  return true;
}

bool ZstdCompression::Compress(const uint8_t *input, size_t inputSize,
                                std::vector<uint8_t> *output,
                                int compressionLevel, std::string *err) {
  if (!input || inputSize == 0) {
    if (err) *err = "Invalid input data";
    return false;
  }

  if (!output) {
    if (err) *err = "Output buffer is null";
    return false;
  }

  // Clamp compression level to valid range
  if (compressionLevel < 1) compressionLevel = 1;
  if (compressionLevel > ZSTD_maxCLevel()) compressionLevel = ZSTD_maxCLevel();

  // Get maximum compressed size
  size_t maxCompressedSize = ZSTD_compressBound(inputSize);

  // Allocate output buffer
  // Note: LightUSD has exceptions disabled, so we check capacity instead
  output->clear();
  if (maxCompressedSize > output->max_size()) {
    if (err) *err = "Compressed buffer size exceeds maximum allocatable size";
    return false;
  }
  output->resize(maxCompressedSize);
  if (output->size() != maxCompressedSize) {
    if (err) *err = "Failed to allocate memory for compression";
    return false;
  }

  // Compress
  size_t result =
      ZSTD_compress(output->data(), maxCompressedSize, input, inputSize,
                    compressionLevel);

  if (ZSTD_isError(result)) {
    if (err) {
      *err = "Zstd compression failed: ";
      *err += ZSTD_getErrorName(result);
    }
    output->clear();
    return false;
  }

  // Shrink to actual compressed size
  output->resize(result);

  return true;
}

size_t ZstdCompression::GetCompressBound(size_t inputSize) {
  return ZSTD_compressBound(inputSize);
}

}  // namespace lightusd

#endif  // LIGHTUSD_WITH_ZSTD_COMPRESSION
