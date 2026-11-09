///
/// @file usda-writer.hh
/// @brief USDA (USD ASCII) writer interface
///
/// Production-ready writer for exporting USD scenes to ASCII format.
/// Supports full round-trip preservation of USD data structures.
///
#pragma once

#include "lightusd.hh"

namespace lightusd {
namespace usda {

///
/// Save scene as USDA(ASCII)
///
/// @param[in] filename USDA filename(UTF-8). WideChar(Unicode) represented as std::string is supported on Windows.
///                     If filename ends with ".zst", zstd compression is automatically enabled.
/// @param[in] stage Stage(scene graph).
/// @param[out] warn Warning message
/// @param[out] err Error message
/// @param[in] options Write options (optional). Includes zstd compression settings.
///
/// @return true upon success.
///
bool SaveAsUSDA(const std::string &filename, const Stage &stage, std::string *warn, std::string *err,
                const USDWriteOptions &options = USDWriteOptions());

#if defined(_WIN32)
// WideChar(UNICODE) filename version.
bool SaveAsUSDA(const std::wstring &filename, const Stage &stage, std::string *warn, std::string *err,
                const USDWriteOptions &options = USDWriteOptions());
#endif

///
/// Export stage as USDA string.
///
/// @param[in] stage Stage(scene graph).
/// @param[out] output USDA string output
/// @param[out] warn Warning message
/// @param[out] err Error message
///
/// @return true upon success.
///
bool ExportToUSDAString(const Stage &stage, std::string *output, std::string *warn, std::string *err);

} // namespace usda
} // namespace lightusd
