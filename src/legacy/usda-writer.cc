// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// USDA(Ascii) writer
//

#include "usda-writer.hh"

#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_WRITER)

#include <fstream>
#include <iostream>
#include <sstream>

#include "value-pprint.hh"
#include "lightusd.hh"
#include "io-util.hh"
#include "str-util.hh"
#include "zstd-compression.hh"

namespace lightusd {
namespace usda {

namespace {

// Check if filename ends with ".zst" extension (case-insensitive)
bool HasZstdExtension(const std::string &filename) {
  if (filename.size() < 4) return false;
  std::string ext = filename.substr(filename.size() - 4);
  return (ext == ".zst" || ext == ".ZST");
}

#if defined(_WIN32)
bool HasZstdExtension(const std::wstring &filename) {
  if (filename.size() < 4) return false;
  std::wstring ext = filename.substr(filename.size() - 4);
  return (ext == L".zst" || ext == L".ZST");
}
#endif

}  // namespace

bool ExportToUSDAString(const Stage &stage, std::string *output, std::string *warn, std::string *err) {
  (void)warn;
  (void)err;

  if (!output) {
    if (err) {
      (*err) = "output parameter is null.\n";
    }
    return false;
  }

  *output = stage.ExportToString();
  return true;
}

bool SaveAsUSDA(const std::string &filename, const Stage &stage,
                std::string *warn, std::string *err,
                const USDWriteOptions &options) {

  (void)warn;

  // Export stage to string
  std::string s = stage.ExportToString();

  // Check if we should use zstd compression
  bool use_compression = options.use_zstd_compression || HasZstdExtension(filename);

  if (use_compression) {
#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION
    // Compress the data
    std::vector<uint8_t> compressed;
    if (!ZstdCompression::Compress(reinterpret_cast<const uint8_t*>(s.data()), s.size(),
                                   &compressed, options.zstd_compression_level, err)) {
      return false;
    }

    if (!io::WriteWholeFile(filename, compressed.data(), compressed.size(), err)) {
      return false;
    }

    std::cout << "Wrote zstd-compressed USDA to [" << filename << "] ("
              << s.size() << " -> " << compressed.size() << " bytes)\n";
#else
    if (err) {
      (*err) = "zstd compression requested but LIGHTUSD_WITH_ZSTD_COMPRESSION is not enabled.\n";
    }
    return false;
#endif
  } else {
    if (!io::WriteWholeFile(filename, reinterpret_cast<const unsigned char *>(s.data()), s.size(), err)) {
      return false;
    }

    std::cout << "Wrote USDA to [" << filename << "]\n";
  }

  return true;
}

#if defined(_WIN32)
bool SaveAsUSDA(const std::wstring &filename, const Stage &stage,
                std::string *warn, std::string *err,
                const USDWriteOptions &options) {

  (void)warn;

  // Export stage to string
  std::string s = stage.ExportToString();

  // Check if we should use zstd compression
  bool use_compression = options.use_zstd_compression || HasZstdExtension(filename);

  if (use_compression) {
#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION
    // Compress the data
    std::vector<uint8_t> compressed;
    if (!ZstdCompression::Compress(reinterpret_cast<const uint8_t*>(s.data()), s.size(),
                                   &compressed, options.zstd_compression_level, err)) {
      return false;
    }

    if (!io::WriteWholeFile(filename, compressed.data(), compressed.size(), err)) {
      return false;
    }

    std::wcout << L"Wrote zstd-compressed USDA to [" << filename << L"] ("
               << s.size() << L" -> " << compressed.size() << L" bytes)\n";
#else
    if (err) {
      (*err) = "zstd compression requested but LIGHTUSD_WITH_ZSTD_COMPRESSION is not enabled.\n";
    }
    return false;
#endif
  } else {
    if (!io::WriteWholeFile(filename, reinterpret_cast<const unsigned char *>(s.data()), s.size(), err)) {
      return false;
    }

    std::wcout << L"Wrote USDA to [" << filename << L"]\n";
  }

  return true;
}
#endif

} // namespace usda
}  // namespace lightusd


#else

namespace lightusd {
namespace usda {

bool ExportToUSDAString(const Stage &stage, std::string *output, std::string *warn, std::string *err) {
  (void)stage;
  (void)output;
  (void)warn;

  if (err) {
    (*err) = "USDA Writer feature is disabled in this build.\n";
  }
  return false;
}

bool SaveAsUSDA(const std::string &filename, const Stage &stage, std::string *warn, std::string *err,
                const USDWriteOptions &options) {
  (void)filename;
  (void)stage;
  (void)warn;
  (void)options;

  if (err) {
    (*err) = "USDA Writer feature is disabled in this build.\n";
  }
  return false;
}

#if defined(_WIN32)
bool SaveAsUSDA(const std::wstring &filename, const Stage &stage, std::string *warn, std::string *err,
                const USDWriteOptions &options) {
  (void)filename;
  (void)stage;
  (void)warn;
  (void)options;

  if (err) {
    (*err) = "USDA Writer feature is disabled in this build.\n";
  }
  return false;
}
#endif

} // namespace usda
}  // namespace lightusd
#endif


