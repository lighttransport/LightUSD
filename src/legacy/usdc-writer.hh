// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file usdc-writer.hh
/// @brief USDC (USD binary/Crate) writer interface
///
/// Work-in-progress writer for exporting USD scenes to binary Crate format.
/// Provides more compact file sizes compared to ASCII format.
///
#pragma once

#include "lightusd.hh"

namespace lightusd {
namespace usdc {

///
/// Save scene as USDC(binary) to a file
///
/// @param[in] filename USDC filename. If filename ends with ".zst", zstd compression is auto-enabled.
/// @param[in] stage Stage
/// @param[out] warn Warning message
/// @param[out] err Error message
/// @param[in] options Write options (optional). Includes zstd compression settings.
///
/// @return true upon success.
///
bool SaveAsUSDCToFile(const std::string &filename, const Stage &stage,
                      std::string *warn, std::string *err,
                      const USDWriteOptions &options = USDWriteOptions());

bool SaveAsUSDCToFile(const std::string &filename, const Layer &layer,
                      std::string *warn, std::string *err,
                      const USDWriteOptions &options = USDWriteOptions());

///
/// Save scene as USDC(binary) to a memory
///
/// @param[in] stage Stage
/// @param[out] output Binary data
/// @param[out] warn Warning message
/// @param[out] err Error message
/// @param[in] max_file_size_bytes Override the writer's max output size limit
///            (0 = keep built-in default; WASM defaults are intentionally small).
/// @param[in] max_memory_bytes Override the writer's max memory estimate limit
///            (0 = keep built-in default).
///
/// @param[in] profile_crate Emit conversion/finalization timings to stderr.
/// @return true upon success.
///
bool SaveAsUSDCToMemory(const Stage &stage, std::vector<uint8_t> *output,
                        std::string *warn, std::string *err,
                        int64_t max_file_size_bytes = 0,
                        int64_t max_memory_bytes = 0,
                        bool compress_float_arrays = false,
                        bool profile_crate = false);

bool SaveAsUSDCToMemory(const Layer &layer, std::vector<uint8_t> *output,
                        std::string *warn, std::string *err,
                        int64_t max_file_size_bytes = 0,
                        int64_t max_memory_bytes = 0,
                        bool compress_float_arrays = false,
                        bool profile_crate = false);

}  // namespace usdc
}  // namespace lightusd
