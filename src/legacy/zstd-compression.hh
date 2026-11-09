// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Zstd compression wrapper for LightUSD
// Provides file-level zstd compression/decompression for USD files

#pragma once

#ifndef ZSTD_COMPRESSION_HH_
#define ZSTD_COMPRESSION_HH_

#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {

///
/// Zstd compression utility class.
/// Provides static methods for compressing and decompressing data using zstd.
///
class ZstdCompression {
 public:
  ///
  /// Check if data starts with zstd magic number (0x28 0xB5 0x2F 0xFD)
  /// @param[in] data Pointer to the data to check
  /// @param[in] length Length of the data in bytes
  /// @return true if data starts with zstd magic number
  ///
  static bool IsZstdCompressed(const uint8_t *data, size_t length);

  ///
  /// Get the decompressed size of zstd-compressed data.
  /// This can be used to check memory budget before decompression.
  /// @param[in] compressed Pointer to compressed data
  /// @param[in] compressedSize Size of compressed data in bytes
  /// @param[out] err Error message if size cannot be determined
  /// @return Decompressed size in bytes, or 0 on error
  ///
  static size_t GetDecompressedSize(const uint8_t *compressed,
                                     size_t compressedSize, std::string *err);

  ///
  /// Decompress zstd-compressed data.
  /// @param[in] compressed Pointer to compressed data
  /// @param[in] compressedSize Size of compressed data in bytes
  /// @param[out] output Vector to store decompressed data
  /// @param[out] err Error message on failure
  /// @param[in] maxDecompressedSize Maximum allowed decompressed size in bytes.
  ///             0 means no limit (default). Use GetDecompressedSize() to check
  ///             the expected size before calling Decompress.
  /// @return true on success, false on failure
  ///
  static bool Decompress(const uint8_t *compressed, size_t compressedSize,
                         std::vector<uint8_t> *output, std::string *err,
                         size_t maxDecompressedSize = 0);

  ///
  /// Compress data using zstd.
  /// @param[in] input Pointer to input data
  /// @param[in] inputSize Size of input data in bytes
  /// @param[out] output Vector to store compressed data
  /// @param[in] compressionLevel Compression level (1-22, default 5)
  /// @param[out] err Error message on failure
  /// @return true on success, false on failure
  ///
  static bool Compress(const uint8_t *input, size_t inputSize,
                       std::vector<uint8_t> *output, int compressionLevel,
                       std::string *err);

  ///
  /// Get the maximum compressed size for a given input size.
  /// @param[in] inputSize Size of input data in bytes
  /// @return Maximum possible compressed size
  ///
  static size_t GetCompressBound(size_t inputSize);

  /// Default compression level (5 is a good balance of speed and ratio)
  static constexpr int kDefaultCompressionLevel = 5;

  /// Zstd magic number bytes
  static constexpr uint8_t kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
};

}  // namespace lightusd

#else  // !LIGHTUSD_WITH_ZSTD_COMPRESSION

// Stub implementation when zstd compression is disabled
namespace lightusd {

class ZstdCompression {
 public:
  static bool IsZstdCompressed(const uint8_t *, size_t) { return false; }
  static size_t GetDecompressedSize(const uint8_t *, size_t, std::string *err) {
    if (err) *err = "Zstd compression support not enabled";
    return 0;
  }
  static bool Decompress(const uint8_t *, size_t, std::vector<uint8_t> *,
                         std::string *err, size_t = 0) {
    if (err) *err = "Zstd compression support not enabled";
    return false;
  }
  static bool Compress(const uint8_t *, size_t, std::vector<uint8_t> *, int,
                       std::string *err) {
    if (err) *err = "Zstd compression support not enabled";
    return false;
  }
  static size_t GetCompressBound(size_t) { return 0; }
  static constexpr int kDefaultCompressionLevel = 5;
};

}  // namespace lightusd

#endif  // LIGHTUSD_WITH_ZSTD_COMPRESSION

#endif  // ZSTD_COMPRESSION_HH_
