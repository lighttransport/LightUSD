#pragma once

#include "../stage/stage.hh"
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <functional>
#include <cstdint>

namespace lightusd {
namespace next {

/// Result of a USDZ write operation.
struct USDZWriteResult {
  bool success = false;
  std::string error;
  size_t bytes_written = 0;
};

struct USDZWriteOptions {
  /// Refuse archives larger than this many bytes (0 = unlimited). Checks are
  /// made before each ZIP entry or central-directory append.
  uint64_t max_file_size_bytes = 0;

  /// Maximum estimated working set in bytes (0 = unlimited). Accounts for
  /// retained USDC/assets plus ZIP output and directory records.
  uint64_t max_memory_bytes = 0;
};

using USDZWriteSink = std::function<bool(const uint8_t*, size_t)>;

/// Stream a USDC root and package assets to a caller-owned sink. The sink is
/// called synchronously and must consume each span before returning.
USDZWriteResult WriteUSDZFromUSDCAndAssetsToSink(
    const uint8_t* usdc_data, size_t usdc_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const USDZWriteSink& sink, const USDZWriteOptions& options = {});

/// Write a Stage to a USDZ file.
/// The stage is first converted to USDC, then packed into a ZIP archive.
USDZWriteResult WriteUSDZToFile(const std::string& filename, const Stage& stage,
                                const USDZWriteOptions& options = {});

/// Write a Stage to a USDZ buffer.
/// The stage is first converted to USDC, then packed into a ZIP archive.
USDZWriteResult WriteUSDZToMemory(std::vector<uint8_t>& buffer, const Stage& stage,
                                  const USDZWriteOptions& options = {});

/// Write USDC data directly as a USDZ archive (skips Stage serialization).
USDZWriteResult WriteUSDZFromUSDCToFile(const std::string& filename,
                                         const uint8_t* usdc_data, size_t usdc_size,
                                         const USDZWriteOptions& options = {});
USDZWriteResult WriteUSDZFromUSDCToMemory(std::vector<uint8_t>& buffer,
                                           const uint8_t* usdc_data, size_t usdc_size,
                                           const USDZWriteOptions& options = {});

/// Write a USDC root plus additional package assets into a USDZ archive.
/// Asset names must be relative normalized package paths and may not replace
/// root.usdc. Entries are stored uncompressed and aligned to 64 bytes.
USDZWriteResult WriteUSDZFromUSDCAndAssetsToMemory(
    std::vector<uint8_t>& buffer, const uint8_t* usdc_data, size_t usdc_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const USDZWriteOptions& options = {});

/// Write a USDA root plus additional package assets into a USDZ archive.
USDZWriteResult WriteUSDZFromUSDAAndAssetsToMemory(
    std::vector<uint8_t>& buffer, const uint8_t* usda_data, size_t usda_size,
    const std::map<std::string, std::vector<uint8_t>>& assets,
    const USDZWriteOptions& options = {});

} // namespace next
} // namespace lightusd
