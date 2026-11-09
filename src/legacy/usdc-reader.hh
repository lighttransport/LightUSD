// SPDX-License-Identifier: Apache 2.0
// Copyright 2020-2022 Syoyo Fujita.
// Copyright 2023-Present Light Transport Entertainment Inc.
//
#pragma once

#include <functional>
#include <cstdint>
#include "stream-reader.hh"
#include "lightusd.hh"

namespace lightusd {
namespace usdc {

///
/// Progress callback function type.
/// @param[in] progress Progress value between 0.0 and 1.0
/// @param[in] userptr User-provided pointer for custom data
/// @return true to continue parsing, false to cancel
///
using ProgressCallback = std::function<bool(float progress, void *userptr)>;

struct USDCMemoryUsageReport {
  uint64_t current_usage_bytes{0};
  uint64_t peak_usage_bytes{0};
  uint64_t max_budget_bytes{0};
  uint64_t remaining_budget_bytes{0};
};

///
/// USDC(Crate) reader
///

struct USDCReaderConfig {
  int32_t numThreads = -1; // -1 = use system's # of threads
  uint32_t kMaxPrimNestLevel = 256;
  uint32_t kMaxFieldValuePairs = 4096;
  uint32_t kMaxTokenLength = 4096; // Max length of `token`
  uint32_t kMaxStringLength = 1024*1024*64; // Max length of `string` data
  uint32_t kMaxElementSize = 8192; // Max allowed value for `elementSize`
  size_t kMaxAllowedMemoryInMB = 512; // Max allowed memory usage in [mb]

  bool allow_unknown_prims = true;
  bool allow_unknown_apiSchemas = true;

  bool strict_allowedToken_check = false;

  // Shader input/output type conformance(false: warn, true: error)
  bool strict_shader_type_check = false;

  // Memory optimization: use mmap for uncompressed arrays
  bool use_mmap = false;

  // Memory optimization: mmap zero-copy for uncompressed USDC arrays.
  // When true + mmap, defer uncompressed array reads and record mmap offsets.
  bool mmap_zero_copy = false;

  // Memory optimization: decode fieldsets/properties on demand instead of
  // prebuilding all live fieldsets. Reduces peak memory for large files.
  bool use_lazy_property_construction = true;
};

class USDCReader {
 public:
  USDCReader(StreamReader *sr,
             const USDCReaderConfig &config = USDCReaderConfig());
  ~USDCReader();

  void set_reader_config(const USDCReaderConfig &config);
  const USDCReaderConfig get_reader_config() const;

  ///
  /// Set progress callback for monitoring parsing progress.
  ///
  /// @param[in] callback Function to call during parsing to report progress
  /// @param[in] userptr User-provided pointer for custom data
  ///
  void SetProgressCallback(ProgressCallback callback, void *userptr = nullptr);

  bool ReadUSDC();

  bool ReconstructStage(Stage *stage);

  // For composition.
  bool get_as_layer(Layer *layer);

  // Approximated memory usage in [mb]
  size_t GetMemoryUsage() const;
  USDCMemoryUsageReport GetMemoryUsageReport() const;

  std::string GetError();
  std::string GetWarning();

 private:
  class Impl;
  Impl *impl_{};
};

}  // namespace usdc
}  // namespace lightusd
