// SPDX-License-Identifier: Apache 2.0
// Copyright 2019 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertinament Inc.

#include <algorithm>
#include <array>
#include <atomic>
//#include <cassert>
#include <cctype>  // std::tolower
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

#include "usdLux.hh"

#ifndef __wasi__
#include <thread>
#endif

#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "image-loader.hh"
#include "integerCoding.h"
#include "io-util.hh"
#include "lz4-compression.hh"
#include "zstd-compression.hh"
#include "security-policy.hh"
#include "safe-arithmetic.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "string-pool.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "layer.hh"
#include "usda-reader.hh"
#include "usda-writer.hh"
#include "usdc-reader.hh"
#include "usdc-writer.hh"
#include "mmap-array-ref.hh"
#include "pprinter.hh"
#include "value-pprint.hh"


#include "common-macros.inc"

namespace lightusd {

namespace {

// Compute max bytes from a memory-limit-in-MB setting using uint64_t to avoid
// overflow on 32-bit platforms (where size_t is 32-bit). Returns SIZE_MAX when
// the result exceeds the addressable range (clamped rather than wrapped).
inline size_t MaxMemoryBytes(uint64_t limit_mb) {
  constexpr uint64_t kBytesPerMiB = uint64_t(1024) * uint64_t(1024);
#if SIZE_MAX < UINT64_MAX
  constexpr uint64_t kMaxBytes =
      uint64_t((std::numeric_limits<size_t>::max)());
#else
  constexpr uint64_t kMaxBytes =
      (std::numeric_limits<uint64_t>::max)();
#endif
  if (limit_mb > (kMaxBytes / kBytesPerMiB)) {
    return (std::numeric_limits<size_t>::max)();
  }
  return static_cast<size_t>(limit_mb * kBytesPerMiB);
}

inline uint64_t EffectiveMemoryLimitMb(const USDLoadOptions &options) {
  return static_cast<uint64_t>(std::max<int32_t>(0, options.max_memory_limit_in_mb));
}

FILE *ProfileOutput() {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  FILE *output = stderr;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  return output;
}

constexpr uint32_t kMaxZstdNestingDepth = 2;

std::string GetLayerBaseDirForAssetName(const std::string &asset_name) {
  std::string basedir;
  if (asset_name.empty()) {
    return basedir;
  }

  basedir = io::GetBaseDir(asset_name);
  if (basedir.empty()) {
    basedir = ".";
  }
  return basedir;
}

bool AdoptStageMMapFileForZeroCopy(Stage *stage, io::MMapFileHandle *handle,
                                   std::string *err) {
  if (!stage || !handle) {
    if (err) {
      (*err) += "Internal error: invalid Stage or mmap handle for zero-copy ownership.\n";
    }
    return false;
  }

  if (!stage->adopt_mmap_file(std::move(*handle))) {
    if (err) {
      (*err) += "Failed to attach mmap file lifetime to Stage for zero-copy arrays.\n";
    }
    return false;
  }
  return true;
}

bool AdoptStageBufferForZeroCopy(Stage *stage, std::vector<uint8_t> *data,
                                 std::string *err) {
  if (!stage || !data) {
    if (err) {
      (*err) += "Internal error: invalid Stage or buffer for zero-copy ownership.\n";
    }
    return false;
  }

  if (!stage->adopt_mmap_buffer(std::move(*data))) {
    if (err) {
      (*err) += "Failed to attach file buffer lifetime to Stage for zero-copy arrays.\n";
    }
    return false;
  }
  return true;
}
}

// Global flag to control DCOUT output. Defaults to false to suppress flood of output.
// Set to true via LIGHTUSD_ENABLE_DCOUT environment variable.
bool g_enable_dcout_output = false;

// constexpr auto kTagUSDA = "[USDA]";
// constexpr auto kTagUSDC = "[USDC]";
// constexpr auto kTagUSDZ = "[USDZ]";

// For PUSH_ERROR_AND_RETURN
#define PushError(s) \
  if (err) {         \
    (*err) += s;     \
  }
//#define PushWarn(s) if (warn) { (*warn) += s; }

// Helper function to format magic header bytes for error messages
static std::string FormatMagicHeader(const uint8_t *addr, const size_t length, size_t max_bytes = 16) {
  if (!addr || length == 0) {
    return "(empty)";
  }

  std::string result = "0x";
  size_t bytes_to_show = std::min(length, max_bytes);

  for (size_t i = 0; i < bytes_to_show; i++) {
    result += fmt::hex(addr[i], 2);
    if (i < bytes_to_show - 1) {
      result += " ";
    }
  }

  if (length > max_bytes) {
    result += "...";
  }

  return result;
}

static bool IsSafeUSDZAssetPath(const std::string &path) {
  return security_policy::IsSafeRelativeAssetPath(path);
}

bool LoadUSDCFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (stage == nullptr) {
    if (err) {
      (*err) = "null pointer for `stage` argument.\n";
    }
    return false;
  }

  bool swap_endian = false;  // @FIXME

  size_t max_length = MaxMemoryBytes(EffectiveMemoryLimitMb(options));

  DCOUT("Max length = " << max_length);

  if (length > max_length) {
    if (err) {
      (*err) += "USDC data [" + filename +
                "] is too large(size = " + std::to_string(length) +
                ", which exceeds memory limit " + std::to_string(max_length) +
                ".\n";
    }

    return false;
  }

  StreamReader sr(addr, length, swap_endian);

  usdc::USDCReaderConfig config;
  config.numThreads = options.num_threads;
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  config.strict_shader_type_check = options.strict_shader_type_check;
  config.kMaxAllowedMemoryInMB = size_t(EffectiveMemoryLimitMb(options));
  config.mmap_zero_copy = options.mmap_zero_copy;
  usdc::USDCReader reader(&sr, config);

  if (options.progress_callback) {
    reader.SetProgressCallback(options.progress_callback, options.progress_userptr);
  }

  if (!reader.ReadUSDC()) {
    if (warn) {
      (*warn) += reader.GetWarning();
    }

    if (err) {
      (*err) += reader.GetError();
    }
    return false;
  }

  DCOUT("Loaded USDC file.");

  // Reconstruct `Stage`(scene) object
  {
    stage->clear_mmap_data();
    if (!reader.ReconstructStage(stage)) {
      DCOUT("Failed to reconstruct Stage from Crate.");
      if (warn) {
        (*warn) += reader.GetWarning();
      }

      if (err) {
        (*err) += reader.GetError();
      }
      return false;
    }

    // Set mmap data source on Stage for zero-copy array access
    if (options.mmap_zero_copy) {
      stage->set_mmap_source(MMapDataSource(addr, length));
    }
  }

  if (warn) {
    (*warn) += reader.GetWarning();
  }

  // Reconstruction may produce a usable Stage together with diagnostics.
  // Preserve the historical permissive behavior unless strict loading was
  // explicitly requested.
  const std::string reader_error = reader.GetError();
  if (!reader_error.empty()) {
    DCOUT(reader_error);
    if (err) {
      (*err) += reader_error;
    }
    if (options.strict_loading) {
      return false;
    }
  }

  DCOUT("Reconstructed Stage from USDC file.");

  return true;
}

bool LoadUSDCFromFile(const std::string &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);

  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;

    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false;
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDCFromMemory(handle.addr, size_t(handle.size), filepath, stage, warn,
                              err, options);
    bool keep_mmap = false;
    if (ret && options.mmap_zero_copy && stage && stage->has_mmap_zero_copy()) {
      keep_mmap = AdoptStageMMapFileForZeroCopy(stage, &handle, err);
      if (!keep_mmap) {
        ret = false;
      }
    }

    if (!keep_mmap) {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;

  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = MaxMemoryBytes(EffectiveMemoryLimitMb(options));
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      if (err) {
        (*err) += "File not found or failed to read : \"" + filepath + "\"\n";
      }

      return false;
    }

    DCOUT("File size: " + std::to_string(data.size()) + " bytes.");

    if (data.size() < (11 * 8)) {
      // ???
      if (err) {
        (*err) += "File size too short. Looks like this file is not a USDC : \"" +
                  filepath + "\"\n";
      }
      return false;
    }

    bool ret = LoadUSDCFromMemory(data.data(), data.size(), filepath, stage, warn,
                                  err, options);
    if (ret && options.mmap_zero_copy && stage && stage->has_mmap_zero_copy()) {
      if (!AdoptStageBufferForZeroCopy(stage, &data, err)) {
        return false;
      }
    }

    return ret;
  }
}

namespace {

static std::string GetFileExtension(const std::string &filename) {
  if (filename.find_last_of('.') != std::string::npos)
    return filename.substr(filename.find_last_of('.') + 1);
  return "";
}

static std::string str_tolower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace

namespace {

struct USDZAssetInfo {
  std::string filename;
  size_t byte_begin;
  size_t byte_end;
};

bool ParseUSDZHeader(const uint8_t *addr, const size_t length,
                     std::vector<USDZAssetInfo> *assets, std::string *warn,
                     std::string *err) {
  (void)warn;

  if (!addr) {
    if (err) {
      (*err) += "null for `addr` argument.\n";
    }
    return false;
  }

  if (length < 30) {  // minimum ZIP local-file header
    if (err) {
      (*err) += "File size too short. Looks like this file is not a USDZ\n";
    }
    return false;
  }

  size_t offset = 0;
  std::unordered_set<std::string> entry_names;
  while ((offset + 30) <= length) {
    //
    // PK zip format:
    // https://users.cs.jmu.edu/buchhofp/forensics/formats/pkzip.html
    //
    std::vector<char> local_header(30);
    memcpy(local_header.data(), addr + offset, 30);

    // Check signagure(first 4 bytes)
    // Must be \x50\x4b\x03\x04
    if ((local_header[0] == 0x50) && (local_header[1] == 0x4b) &&
        (local_header[2] == 0x03) && (local_header[3] == 0x04)) {
      // ok

      // TODO: Check other header info(version, flags, crc32)
    } else {
      if (offset == 0) {
        // Invalid header found.
        if (err) {
          (*err) += "PKZIP header not found.\n";
        }
        return false;
      } else {
        // not a local(global?) header
        // Maybe near to the end of file.
        break;
      }
    }

    offset += 30;

    // read in the variable name
    uint16_t name_len;
    memcpy(&name_len, &local_header[26], sizeof(uint16_t));
    {
      size_t next_offset;
      if (!safe::add(offset, size_t(name_len), &next_offset) ||
          next_offset > length) {
        if (err) {
          (*err) += "Invalid ZIP data\n";
        }
        return false;
      }
    }

    std::string varname(name_len, ' ');
    memcpy(&varname[0], addr + offset, name_len);

    if (!IsSafeUSDZAssetPath(varname)) {
      if (err) {
        (*err) += "Unsafe USDZ entry path: `" + varname + "`\n";
      }
      return false;
    }
    if (!entry_names.insert(varname).second) {
      if (err) {
        (*err) += "Duplicate USDZ entry path: `" + varname + "`\n";
      }
      return false;
    }

    if (!safe::add(offset, size_t(name_len), &offset)) {
      if (err) {
        (*err) += "Integer overflow in ZIP entry name length.\n";
      }
      return false;
    }

    // read in the extra field
    uint16_t extra_field_len;
    memcpy(&extra_field_len, &local_header[28], sizeof(uint16_t));
    if (extra_field_len > 0) {
      size_t next_offset;
      if (!safe::add(offset, size_t(extra_field_len), &next_offset) ||
          next_offset > length) {
        if (err) {
          (*err) += "Invalid extra field length in ZIP data\n";
        }
        return false;
      }
    }

    if (!safe::add(offset, size_t(extra_field_len), &offset)) {
      if (err) {
        (*err) += "Integer overflow in ZIP extra field length.\n";
      }
      return false;
    }

    // In strict USDZ, data must be aligned at a 64-byte boundary. Keep
    // ValidateUSDZ() strict, but allow the loader to read unaligned stored ZIP
    // packages in the wild; byte ranges into the mmapped/archive buffer do not
    // require alignment for parsing.
    if ((offset % 64) != 0) {
      if (warn) {
        (*warn) += "USDZ entry '" + varname +
                   "' data offset is not 64-byte aligned: " +
                   std::to_string(offset) + ".\n";
      }
    }

    uint16_t compr_method;
    memcpy(&compr_method, &local_header[8], sizeof(compr_method));
    uint32_t compr_bytes;
    memcpy(&compr_bytes, &local_header[18], sizeof(compr_bytes));
    uint32_t uncompr_bytes;
    memcpy(&uncompr_bytes, &local_header[22], sizeof(uncompr_bytes));

    // USDZ only supports uncompressed ZIP
    if (compr_method != 0) {
      if (err) {
        (*err) += "Compressed ZIP is not supported for USDZ\n";
      }
      return false;
    }
    if (compr_bytes != uncompr_bytes) {
      if (err) {
        (*err) += "Stored USDZ entry has mismatched compressed and "
                  "uncompressed sizes\n";
      }
      return false;
    }

    if (offset > length || uncompr_bytes > (length - offset)) {
      if (!assets) {
        // Detection mode (IsUSDZ / IsUSD): only a prefix of the file was read
        // (IsUSDZ reads ~256 bytes), so the first entry's data legitimately
        // extends past `length`. The local header has already been validated
        // (PK signature, uncompressed, 64-byte aligned) — that is sufficient
        // to classify the file as USDZ. Full validation (which needs every
        // entry's bytes present) runs when the whole file is parsed with a
        // non-null `assets`. Without this, IsUSDZ(filename) wrongly returns
        // false for any USDZ whose root layer exceeds the prefix size — i.e.
        // essentially all real packages.
        return true;
      }
      if (err) {
        (*err) += "Invalid uncompressed size in ZIP data\n";
      }
      return false;
    }

    if (assets) {
      USDZAssetInfo info;
      DCOUT("USDZasset[" << assets->size() << "] " << varname << ", byte_begin " << offset << ", length " << uncompr_bytes << "\n");
      info.filename = varname;
      info.byte_begin = offset;
      {
        size_t end_offset;
        if (!safe::add(offset, size_t(uncompr_bytes), &end_offset)) {
          if (err) {
            (*err) += "Integer overflow in ZIP uncompressed size.\n";
          }
          return false;
        }
        info.byte_end = end_offset;
      }

      assets->push_back(info);
    }

    if (!safe::add(offset, size_t(uncompr_bytes), &offset)) {
      if (err) {
        (*err) += "Integer overflow in ZIP uncompressed size.\n";
      }
      return false;
    }
  }

  if (assets && offset < length) {
    uint32_t signature = 0;
    if (length - offset >= sizeof(signature)) {
      memcpy(&signature, addr + offset, sizeof(signature));
    }
    constexpr uint32_t kCentralDirectoryHeader = 0x02014b50u;
    constexpr uint32_t kEndOfCentralDirectory = 0x06054b50u;
    if (signature != kCentralDirectoryHeader &&
        signature != kEndOfCentralDirectory) {
      if (err) {
        (*err) += "Invalid or truncated data after USDZ entries\n";
      }
      return false;
    }
  }

  return true;
}

enum class USDZRootFormat { Invalid, USDA, USDC };

USDZRootFormat GetUSDZRootFormat(const std::string &filename,
                                 const uint8_t *data, size_t size) {
  const std::string ext = str_tolower(GetFileExtension(filename));
  const bool is_usdc = size >= 8 && std::memcmp(data, "PXR-USDC", 8) == 0;
  const bool is_usda = size >= 5 && std::memcmp(data, "#usda", 5) == 0;
  if ((ext == "usdc" || ext == "usd") && is_usdc) {
    return USDZRootFormat::USDC;
  }
  if ((ext == "usda" || ext == "usd") && is_usda) {
    return USDZRootFormat::USDA;
  }
  return USDZRootFormat::Invalid;
}

USDZRootFormat GetUSDZRootFormat(const uint8_t *addr, size_t length,
                                 const std::vector<USDZAssetInfo> &assets) {
  if (assets.empty()) return USDZRootFormat::Invalid;

  const USDZAssetInfo &root = assets.front();
  if (root.byte_begin > root.byte_end || root.byte_end > length) {
    return USDZRootFormat::Invalid;
  }

  const uint8_t *root_data = addr + root.byte_begin;
  const size_t root_size = root.byte_end - root.byte_begin;
  return GetUSDZRootFormat(root.filename, root_data, root_size);
}

}  // namespace

bool LoadUSDZFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  std::vector<USDZAssetInfo> assets;
  if (!ParseUSDZHeader(addr, length, &assets, warn, err)) {
    return false;
  }

#ifdef LIGHTUSD_LOCAL_DEBUG_PRINT
  for (size_t i = 0; i < assets.size(); i++) {
    DCOUT("[" << i << "] " << assets[i].filename << " : byte range ("
              << assets[i].byte_begin << ", " << assets[i].byte_end << ")");
  }
#endif

  // USDZ root-layer selection is positional: the first archive entry is the
  // root. Do not prefer a later USDC merely because it is binary. Accept the
  // neutral .usd suffix by sniffing its payload.
  const USDZRootFormat root_format = GetUSDZRootFormat(addr, length, assets);
  const int32_t usdc_index = root_format == USDZRootFormat::USDC ? 0 : -1;
  const int32_t usda_index = root_format == USDZRootFormat::USDA ? 0 : -1;

  if ((usdc_index == -1) && (usda_index == -1)) {
    if (err) {
      (*err) += "USDZ first entry is not a valid USD root layer\n";
    }
    return false;
  }

  if (usdc_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usdc_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usdc_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usdc_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usdc_addr = addr + start_addr_offset;
    bool ret = LoadUSDCFromMemory(usdc_addr, usdc_size, filename, stage, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        if (!err->empty() && err->back() != '\n') {
          (*err) += "\n";
        }
        (*err) += "Failed to load USDC: [" + filename + "].\n";
      }

      return false;
    }
  } else if (usda_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usda_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usda_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usda_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usda_addr = addr + start_addr_offset;
    bool ret = LoadUSDAFromMemory(usda_addr, usda_size, filename, stage, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        (*err) += "Failed to load USDA: [" + filename + "].\n";
      }

      return false;
    }
  }


  return true;
}

bool LoadUSDZFromFile(const std::string &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  // <filename, byte_begin, byte_end>
  std::vector<std::tuple<std::string, size_t, size_t>> assets;

  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);


  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;

    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false;
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDZFromMemory(handle.addr, size_t(handle.size), filepath, stage, warn,
                              err, options);
    bool keep_mmap = false;
    if (ret && options.mmap_zero_copy && stage && stage->has_mmap_zero_copy()) {
      keep_mmap = AdoptStageMMapFileForZeroCopy(stage, &handle, err);
      if (!keep_mmap) {
        ret = false;
      }
    }

    if (!keep_mmap) {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;
  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = MaxMemoryBytes(EffectiveMemoryLimitMb(options));
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      return false;
    }

    if (data.size() < (11 * 8) + 30) {  // 88 for USDC header, 30 for ZIP header
      // ???
      if (err) {
        (*err) += "File size too short. Looks like this file is not a USDZ : \"" +
                  filepath + "\"\n";
      }
      return false;
    }

    bool ret = LoadUSDZFromMemory(data.data(), data.size(), filepath, stage, warn,
                                  err, options);
    if (ret && options.mmap_zero_copy && stage && stage->has_mmap_zero_copy()) {
      if (!AdoptStageBufferForZeroCopy(stage, &data, err)) {
        return false;
      }
    }

    return ret;
  }
}

#ifdef _WIN32
bool LoadUSDZFromFile(const std::wstring &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  std::string filename = io::WcharToUTF8(_filename);
  return LoadUSDZFromFile(filename, stage, warn, err, options);
}
#endif

bool LoadUSDAFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &base_dir, Stage *stage,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (addr == nullptr) {
    if (err) {
      (*err) = "null pointer for `addr` argument.\n";
    }
    return false;
  }

  if (stage == nullptr) {
    if (err) {
      (*err) = "null pointer for `stage` argument.\n";
    }
    return false;
  }

  lightusd::StreamReader sr(addr, length, /* swap endian */ false);
  lightusd::usda::USDAReader reader(&sr);

  lightusd::usda::USDAReaderConfig config;
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  config.strict_shader_type_check = options.strict_shader_type_check;
  config.allow_unknown_apiSchema = !options.strict_apiSchema_check;
  config.max_memory_limit_in_mb = size_t(EffectiveMemoryLimitMb(options));
  // MaterialX validation options
  config.strict_mtlx_check = options.strict_mtlx_check;
  config.validate_mtlx_info_id = options.validate_mtlx_info_id;
  config.validate_mtlx_connection_types = options.validate_mtlx_connection_types;
  config.validate_mtlx_connection_targets = options.validate_mtlx_connection_targets;
  config.validate_mtlx_duplicate_names = options.validate_mtlx_duplicate_names;
  config.validate_mtlx_index_bounds = options.validate_mtlx_index_bounds;
  config.error_detail = options.error_detail;
  reader.set_reader_config(config);

  if (options.progress_callback) {
    reader.SetProgressCallback(options.progress_callback, options.progress_userptr);
  }

  reader.SetBaseDir(base_dir);
  reader.set_filename(base_dir);  // Pass filename for error context display

  {
    bool ret = reader.Read();

    if (!ret) {
      if (err) {
        (*err) += "Failed to parse USDA\n";
        (*err) += reader.GetError();
      }

      return false;
    }
  }

  {
    bool ret = reader.ReconstructStage();
    if (!ret) {
      if (err) {
        (*err) += "Failed to reconstruct Stage from USDA:\n";
        (*err) += reader.GetError() + "\n";
      }
      return false;
    }
  }

  (*stage) = reader.GetStage();

  if (warn) {
    (*warn) += reader.GetWarning();
  }

  return true;
}

bool LoadUSDAFromFile(const std::string &_filename, Stage *stage,
                      std::string *warn, std::string *err,
                      const USDLoadOptions &options) {
  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(filepath);

  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;

    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false;
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDAFromMemory(handle.addr, size_t(handle.size), base_dir, stage, warn,
                              err, options);

    {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;
  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = MaxMemoryBytes(EffectiveMemoryLimitMb(options));
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      if (err) {
        (*err) += "File not found or failed to read : \"" + filepath + "\"\n";
      }
      return false;
    }

    return LoadUSDAFromMemory(data.data(), data.size(), base_dir, stage, warn,
                              err, options);
  }
}

bool LoadUSDFromFile(const std::string &_filename, Stage *stage,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options) {
  // Ensure common USD strings are interned for deduplication
  PreInternCommonStrings();

  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(filepath);

  if (io::IsMMapSupported()) {
    io::MMapFileHandle handle;

    {
      std::string _err;
      if (!io::MMapFile(filepath, &handle, /* writable */false, &_err)) {
        if (err) {
          (*err) += _err + "\n";
        }
        return false;
      }

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    bool ret = LoadUSDFromMemory(handle.addr, size_t(handle.size), base_dir, stage, warn,
                              err, options);
    bool keep_mmap = false;
    if (ret && options.mmap_zero_copy && stage && stage->has_mmap_zero_copy()) {
      keep_mmap = AdoptStageMMapFileForZeroCopy(stage, &handle, err);
      if (!keep_mmap) {
        ret = false;
      }
    }

    if (!keep_mmap) {
      std::string _err;
      // Ignore unmap result for now.
      io::UnmapFile(handle, &_err);

      if (_err.size()) {
        if (warn) {
          (*warn) += _err + "\n";
        }
      }
    }

    return ret;
  } else {
    std::vector<uint8_t> data;
    size_t max_bytes = MaxMemoryBytes(EffectiveMemoryLimitMb(options));
    if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                           /* userdata */ nullptr)) {
      return false;
    }

    bool ret = LoadUSDFromMemory(data.data(), data.size(), base_dir, stage, warn,
                                 err, options);
    if (ret && options.mmap_zero_copy && stage && stage->has_mmap_zero_copy()) {
      if (!AdoptStageBufferForZeroCopy(stage, &data, err)) {
        return false;
      }
    }

    return ret;
  }
}

static bool LoadUSDFromMemoryImpl(const uint8_t *addr, const size_t length,
                                  const std::string &base_dir, Stage *stage,
                                  std::string *warn, std::string *err,
                                  const USDLoadOptions &options,
                                  uint32_t zstd_depth) {
  // Check for zstd-compressed data first (file-level compression)
  if (IsZstdCompressed(addr, length)) {
    DCOUT("Detected as zstd-compressed USD.");
#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION
    if (zstd_depth >= kMaxZstdNestingDepth) {
      if (err) {
        (*err) += "Nested zstd compression depth exceeded limit.\n";
      }
      return false;
    }

    // Get decompressed size for memory budget check
    std::string zstd_err;
    size_t decompressed_size = ZstdCompression::GetDecompressedSize(addr, length, &zstd_err);
    if (decompressed_size == 0) {
      if (err) {
        (*err) += "Failed to get zstd decompressed size: " + zstd_err + "\n";
      }
      return false;
    }

    // Check against memory budget
    size_t max_length = MaxMemoryBytes(EffectiveMemoryLimitMb(options));
    if (decompressed_size > max_length) {
      if (err) {
        (*err) += "Decompressed USD size (" + std::to_string(decompressed_size) +
                  " bytes) exceeds memory limit (" + std::to_string(max_length) + " bytes)\n";
      }
      return false;
    }

    // Decompress
    std::vector<uint8_t> decompressed_data;
    if (!ZstdCompression::Decompress(addr, length, &decompressed_data, &zstd_err)) {
      if (err) {
        (*err) += "Failed to decompress zstd data: " + zstd_err + "\n";
      }
      return false;
    }

    // The decompressed buffer is local to this stack frame, so zero-copy refs
    // cannot safely outlive this call.
    USDLoadOptions decompressed_options = options;
    decompressed_options.mmap_zero_copy = false;

    // Recursively call with bounded nested zstd depth.
    return LoadUSDFromMemoryImpl(decompressed_data.data(), decompressed_data.size(),
                                 base_dir, stage, warn, err,
                                 decompressed_options, zstd_depth + 1);
#else
    if (err) {
      (*err) += "zstd-compressed USD file detected, but zstd compression support is not enabled. "
                "Rebuild with LIGHTUSD_WITH_ZSTD_COMPRESSION=ON.\n";
    }
    return false;
#endif
  }

  if (IsUSDC(addr, length)) {
    DCOUT("Detected as USDC.");
    return LoadUSDCFromMemory(addr, length, base_dir, stage, warn, err,
                              options);
  } else if (IsUSDA(addr, length)) {
    DCOUT("Detected as USDA.");
    return LoadUSDAFromMemory(addr, length, base_dir, stage, warn, err,
                              options);
  } else if (IsUSDZ(addr, length)) {
    DCOUT("Detected as USDZ.");
    return LoadUSDZFromMemory(addr, length, base_dir, stage, warn, err,
                              options);
  } else {
    if (err) {
      (*err) += "Couldn't determine USD format(USDA/USDC/USDZ). ";
      (*err) += "Found magic header: " + FormatMagicHeader(addr, length, 8) + ", ";
      (*err) += "expected: \"#usda 1.0\" (0x23 75 73 64 61 20 31 2e 30) for USDA, ";
      (*err) += "\"PXR-USDC\" (0x50 58 52 2d 55 53 44 43) for USDC, ";
      (*err) += "or ZIP signature (0x50 4b 03 04) for USDZ.\n";
    }
    return false;
  }
}

bool LoadUSDFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &base_dir, Stage *stage,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options) {
  return LoadUSDFromMemoryImpl(addr, length, base_dir, stage, warn, err, options, 0);
}

bool ReadUSDZAssetInfoFromMemory(const uint8_t *addr, const size_t length, const bool asset_on_memory, USDZAsset *asset,
  std::string *warn, std::string *err) {

  if (!asset) {
    return false;
  }
  asset->asset_map.clear();
  asset->root_asset_name.clear();
  asset->data.clear();
  asset->addr = nullptr;
  asset->size = 0;

  std::vector<USDZAssetInfo> assetInfos;
  if (!ParseUSDZHeader(addr, length, &assetInfos, warn, err)) {
    return false;
  }

  for (size_t i = 0; i < assetInfos.size(); i++) {
    if (assetInfos[i].byte_begin > length) {
      if (err) {
        (*err) += "Invalid byte begin offset in USDZ asset header.";
      }
      return false;
    }
    if (assetInfos[i].byte_end > length) {
      if (err) {
        (*err) += "Invalid byte end offset in USDZ asset header.";
      }
      return false;
    }
    if (!IsSafeUSDZAssetPath(assetInfos[i].filename)) {
      if (err) {
        (*err) += "Unsafe asset path in USDZ header: `" + assetInfos[i].filename + "`\n";
      }
      return false;
    }

    // Assume same filename does not exist.
    asset->asset_map[assetInfos[i].filename] =
        std::make_pair(assetInfos[i].byte_begin, assetInfos[i].byte_end);
    if (i == 0) asset->root_asset_name = assetInfos[i].filename;
  }

  if (asset_on_memory) {
    asset->data.clear();
    asset->addr = addr;
    asset->size = length;
  } else {
    // copy content
    asset->data.resize(length);
    memcpy(asset->data.data(), addr, length);
    asset->addr = nullptr;
    asset->size = 0;
  }

  return true;
}

bool ReadUSDZAssetInfoFromFile(const std::string &_filename, USDZAsset *asset,
  std::string *warn, std::string *err, size_t max_memory_limit_in_mb) {

  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(_filename);

  std::vector<uint8_t> data;
  size_t max_bytes = 1024ull * 1024ull * max_memory_limit_in_mb;
  if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                         /* userdata */ nullptr)) {
    return false;
  }

  return ReadUSDZAssetInfoFromMemory(data.data(), data.size(), /* asset_on_memory */false, asset, warn, err);

}

//
// File type detection
//

bool IsUSDA(const std::string &filename) {
  // TODO: Read first few bytes and check the magic number.
  //
  std::vector<uint8_t> data;
  std::string err;
  // 12 = enough storage for "#usda 1.0"
  if (!io::ReadFileHeader(&data, &err, filename, 12,
                          /* userdata */ nullptr)) {
    // TODO: return `err`
    return false;
  }

  return IsUSDA(data.data(), data.size());
}

bool IsUSDA(const uint8_t *addr, const size_t length) {
  if (length < 9) {
    return false;
  }
  const char header[9 + 1] = "#usda 1.0";

  if (memcmp(header, addr, 9) == 0) {
    return true;
  }

  return false;
}

bool IsUSDC(const std::string &filename) {
  // TODO: Read first few bytes and check the magic number.
  //
  std::vector<uint8_t> data;
  std::string err;
  // 88 bytes should enough
  if (!io::ReadFileHeader(&data, &err, filename, /* header bytes */ 88,
                          /* userdata */ nullptr)) {
    return false;
  }

  return IsUSDC(data.data(), data.size());
}

bool IsUSDC(const uint8_t *addr, const size_t length) {
  // must be 88bytes or more
  if (length < 88) {
    return false;
  }
  const char header[8 + 1] = "PXR-USDC";

  if (memcmp(header, addr, 8) == 0) {
    return true;
  }

  return false;
}

bool IsUSDZ(const std::string &filename) {
  // TODO: Read first few bytes and check the magic number.
  //
  std::vector<uint8_t> data;
  std::string err;
  // 256 bytes may be enough.
  if (!io::ReadFileHeader(&data, &err, filename, 256,
                          /* userdata */ nullptr)) {
    return false;
  }

  return IsUSDZ(data.data(), data.size());
}

bool IsUSDZ(const uint8_t *addr, const size_t length) {
  std::string warn;
  std::string err;

  return ParseUSDZHeader(addr, length, /* [out] assets */ nullptr, &warn, &err);
}

bool IsZstdCompressed(const uint8_t *addr, const size_t length) {
  return ZstdCompression::IsZstdCompressed(addr, length);
}

bool IsUSD(const std::string &filename, std::string *detected_format) {
  if (IsUSDA(filename)) {
    if (detected_format) {
      (*detected_format) = "usda";
    }
    return true;
  }

  if (IsUSDC(filename)) {
    if (detected_format) {
      (*detected_format) = "usdc";
    }
    return true;
  }

  if (IsUSDZ(filename)) {
    if (detected_format) {
      (*detected_format) = "usdz";
    }
    return true;
  }

  return false;
}

bool IsUSD(const uint8_t *addr, const size_t length, std::string *detected_format) {
  if (IsUSDA(addr, length)) {
    if (detected_format) {
      (*detected_format) = "usda";
    }
    return true;
  }

  if (IsUSDC(addr, length)) {
    if (detected_format) {
      (*detected_format) = "usdc";
    }
    return true;
  }

  if (IsUSDZ(addr, length)) {
    if (detected_format) {
      (*detected_format) = "usdz";
    }
    return true;
  }

  return false;
}

bool LoadUSDCLayerFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Layer *layer,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (layer == nullptr) {
    if (err) {
      (*err) = "null pointer for `layer` argument.\n";
    }
    return false;
  }

  bool swap_endian = false;  // @FIXME

  size_t max_length = MaxMemoryBytes(EffectiveMemoryLimitMb(options));

  DCOUT("Max length = " << max_length);

  if (length > max_length) {
    if (err) {
      (*err) += "USDC data [" + filename +
                "] is too large(size = " + std::to_string(length) +
                ", which exceeds memory limit " + std::to_string(max_length) +
                ".\n";
    }

    return false;
  }

  StreamReader sr(addr, length, swap_endian);

  usdc::USDCReaderConfig config;
  config.numThreads = options.num_threads;
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  config.strict_shader_type_check = options.strict_shader_type_check;
  config.allow_unknown_apiSchemas = !options.strict_apiSchema_check;
  usdc::USDCReader reader(&sr, config);

  // LIGHTUSD_CRATE_PROFILE=1: split the layer load into crate parse vs
  // PrimSpec reconstruction (stderr; complements the lusdcat phase marks).
  const bool profile_load = (std::getenv("LIGHTUSD_CRATE_PROFILE") != nullptr);
  const auto load_t0 = std::chrono::steady_clock::now();

  if (!reader.ReadUSDC()) {
    if (warn) {
      (*warn) += reader.GetWarning();
    }

    if (err) {
      (*err) += reader.GetError();
    }
    return false;
  }

  DCOUT("Loaded USDC file.");

  const auto load_t1 = std::chrono::steady_clock::now();

  {
    if (!reader.get_as_layer(layer)) {
      DCOUT("Failed to reconstruct Layer from Crate.");
      if (warn) {
        (*warn) += reader.GetWarning();
      }

      if (err) {
        (*err) += reader.GetError();
      }
      return false;
    }
  }

  if (profile_load) {
    const auto load_t2 = std::chrono::steady_clock::now();
    fprintf(ProfileOutput(),
            "[usdc-layer profile] %s: ReadUSDC %.1fms | ToLayer %.1fms\n",
        filename.c_str(),
        std::chrono::duration<double, std::milli>(load_t1 - load_t0).count(),
        std::chrono::duration<double, std::milli>(load_t2 - load_t1).count());
  }

  if (warn) {
    (*warn) += reader.GetWarning();
  }

  const std::string reader_error = reader.GetError();
  if (!reader_error.empty()) {
    DCOUT(reader_error);
    if (err) {
      (*err) += reader_error;
    }
    if (options.strict_loading) {
      return false;
    }
  }

  DCOUT("Reconstructed Stage from USDC file.");

  return true;
}

bool LoadUSDALayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &asset_name, Layer *dst_layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options) {

  if (!addr) {
    if (err) {
      (*err) += "addr arg is nullptr.\n";
    }
    return false;
  }

  if (length < 9) {
    if (err) {
      (*err) += "Input too short.\n";
    }
    return false;
  }

  if (!dst_layer) {
    if (err) {
      (*err) += "dst_layher arg is nullptr.\n";
    }
    return false;
  }

  lightusd::StreamReader sr(addr, length, /* swap endian */ false);
  lightusd::usda::USDAReader reader(&sr);

  lightusd::usda::USDAReaderConfig config;
  config.max_memory_limit_in_mb = size_t(EffectiveMemoryLimitMb(options));
  config.strict_allowedToken_check = options.strict_allowedToken_check;
  config.strict_shader_type_check = options.strict_shader_type_check;
  // MaterialX validation options
  config.strict_mtlx_check = options.strict_mtlx_check;
  config.validate_mtlx_info_id = options.validate_mtlx_info_id;
  config.validate_mtlx_connection_types = options.validate_mtlx_connection_types;
  config.validate_mtlx_connection_targets = options.validate_mtlx_connection_targets;
  config.validate_mtlx_duplicate_names = options.validate_mtlx_duplicate_names;
  config.validate_mtlx_index_bounds = options.validate_mtlx_index_bounds;
  config.error_detail = options.error_detail;
  reader.set_reader_config(config);
  reader.set_base_dir(GetLayerBaseDirForAssetName(asset_name));
  reader.set_filename(asset_name);
  if (options.progress_callback) {
    reader.SetProgressCallback(options.progress_callback, options.progress_userptr);
  }

  uint32_t load_states = static_cast<uint32_t>(lightusd::LoadState::Toplevel);

  bool as_primspec = true;

  {
    bool ret = reader.read(load_states, as_primspec);

    if (!ret) {
      if (err) {
        (*err) += "Failed to parse USDA: " + asset_name + "\n";
        (*err) += reader.get_error() + "\n";
      }
      return false;
    }
  }

  lightusd::Layer layer;
  bool ret = reader.get_as_layer(&layer);
  if (!ret) {
    if (err) {
      (*err) += reader.get_error();
    }
    return false;
  }

  if (warn) {
    if (reader.get_warning().size()) {
      (*warn) += reader.get_warning();
    }
  }

  (*dst_layer) = std::move(layer);

  return true;
}

bool LoadUSDZLayerFromMemory(const uint8_t *addr, const size_t length,
                        const std::string &filename, Layer *layer,
                        std::string *warn, std::string *err,
                        const USDLoadOptions &options) {
  if (layer == nullptr) {
    if (err) {
      (*err) = "null pointer for `layer` argument.\n";
    }
    return false;
  }

  std::vector<USDZAssetInfo> assets;
  if (!ParseUSDZHeader(addr, length, &assets, warn, err)) {
    return false;
  }

#ifdef LIGHTUSD_LOCAL_DEBUG_PRINT
  for (size_t i = 0; i < assets.size(); i++) {
    DCOUT("[" << i << "] " << assets[i].filename << " : byte range ("
              << assets[i].byte_begin << ", " << assets[i].byte_end << ")");
  }
#endif

  // Layer loading follows the same USDZ first-entry root rule as Stage loading.
  const USDZRootFormat root_format = GetUSDZRootFormat(addr, length, assets);
  const int32_t usdc_index = root_format == USDZRootFormat::USDC ? 0 : -1;
  const int32_t usda_index = root_format == USDZRootFormat::USDA ? 0 : -1;

  if ((usdc_index == -1) && (usda_index == -1)) {
    if (err) {
      (*err) += "USDZ first entry is not a valid USD root layer\n";
    }
    return false;
  }

  if (usdc_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usdc_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usdc_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usdc_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDC data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usdc_addr = addr + start_addr_offset;
    bool ret = LoadUSDCLayerFromMemory(usdc_addr, usdc_size, filename, layer, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        if (!err->empty() && err->back() != '\n') {
          (*err) += "\n";
        }
        (*err) += "Failed to load USDC: [" + filename + "].\n";
      }

      return false;
    }
  } else if (usda_index >= 0) {
    const size_t start_addr_offset = assets[size_t(usda_index)].byte_begin;
    const size_t end_addr_offset = assets[size_t(usda_index)].byte_end;
    if (end_addr_offset < start_addr_offset) {
      if (err) {
        (*err) +=
            "Invalid start/end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }
    const size_t usda_size = end_addr_offset - start_addr_offset;

    if (start_addr_offset > length) {
      if (err) {
        (*err) += "Invalid start offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    if (end_addr_offset > length) {
      if (err) {
        (*err) += "Invalid end offset to USDA data: [" + filename + "].\n";
      }
      return false;
    }

    const uint8_t *usda_addr = addr + start_addr_offset;
    bool ret = LoadUSDALayerFromMemory(usda_addr, usda_size, filename, layer, warn,
                                  err, options);

    if (!ret) {
      if (err) {
        (*err) += "Failed to load USDA: [" + filename + "].\n";
      }

      return false;
    }
  }

  return true;
}


// Copy asset-resolver state to the complete PrimSpec tree without consuming the
// process stack on deeply nested or variant-heavy input.
static void PropagateAssetResolverState(
    PrimSpec &root, const std::string &cwp,
    const std::vector<std::string> &search_paths) {
  DCOUT("current_working_path: " << cwp);
  DCOUT("search_paths: " << search_paths);

  std::vector<PrimSpec *> stack;
  stack.push_back(&root);
  while (!stack.empty()) {
    PrimSpec *ps = stack.back();
    stack.pop_back();
    ps->set_asset_resolution_state(cwp, search_paths);

    for (auto &child : ps->children()) stack.push_back(&child);
    // Variant content carries the same anchor as the owning PrimSpec.
    for (auto &variant_set_item : ps->variantSets()) {
      for (auto &variant_item : variant_set_item.second.variantSet) {
        stack.push_back(&variant_item.second);
      }
    }
  }
}

bool LoadLayerFromMemory(const uint8_t *addr, const size_t length,
                       const std::string &asset_name, Layer *layer,
                       std::string *warn, std::string *err,
                       const USDLoadOptions &options) {

  bool ret{false};

  if (IsUSDC(addr, length)) {
    DCOUT("Detected as USDC.");
    ret = LoadUSDCLayerFromMemory(addr, length, asset_name, layer, warn, err,
                              options);
  } else if (IsUSDA(addr, length)) {
    DCOUT("Detected as USDA.");
    ret = LoadUSDALayerFromMemory(addr, length, asset_name, layer, warn, err,
                              options);
  } else if (IsUSDZ(addr, length)) {
    DCOUT("Detected as USDZ.");
    return LoadUSDZLayerFromMemory(addr, length, asset_name, layer, warn, err,
                              options);
  } else {
    if (err) {
      (*err) += "Couldn't determine USD format(USDA/USDC/USDZ). ";
      (*err) += "Found magic header: " + FormatMagicHeader(addr, length, 8) + ", ";
      (*err) += "expected: \"#usda 1.0\" (0x23 75 73 64 61 20 31 2e 30) for USDA, ";
      (*err) += "\"PXR-USDC\" (0x50 58 52 2d 55 53 44 43) for USDC, ";
      (*err) += "or ZIP signature (0x50 4b 03 04) for USDZ.\n";
    }
    return false;
  }

  if (ret) {
    std::vector<std::string> search_paths; // empty
    std::string basedir = GetLayerBaseDirForAssetName(asset_name);
    layer->set_asset_resolution_state(basedir, search_paths,
                                      /* userdata */ nullptr);
    // Save current working path to each PrimSpec in the layer
    // for the subsequent composition operation.
    for (auto &root_ps : layer->primspecs()) {
      PropagateAssetResolverState(root_ps.second, basedir, search_paths);
    }
  }

  return ret;
}

bool LoadLayerFromFile(const std::string &_filename, Layer *stage,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options) {

  if (_filename.empty()) {
    PUSH_ERROR_AND_RETURN("Input filename is empty.");
  }

  // TODO: Use AssetResolutionResolver.
  std::string filepath = io::ExpandFilePath(_filename, /* userdata */ nullptr);
  std::string base_dir = io::GetBaseDir(filepath);

  std::vector<uint8_t> data;
  size_t max_bytes = MaxMemoryBytes(EffectiveMemoryLimitMb(options));
  if (!io::ReadWholeFile(&data, err, filepath, max_bytes,
                         /* userdata */ nullptr)) {
    return false;
  }

  return LoadLayerFromMemory(data.data(), data.size(), filepath, stage, warn, err,
                           options);
}

bool LoadLayerFromAsset(AssetResolutionResolver &resolver, const std::string &resolved_asset_name, Layer *layer,
                     std::string *warn, std::string *err,
                     const USDLoadOptions &options) {

  if (resolved_asset_name.empty()) {
    PUSH_ERROR_AND_RETURN("Input asset name is empty.");
  }

  resolver.set_max_asset_bytes_in_mb(options.max_allowed_asset_size_in_mb);

  Asset asset;
  if (!resolver.open_asset(resolved_asset_name, resolved_asset_name, &asset, warn, err)) {
    PUSH_ERROR_AND_RETURN(fmt::format("Failed to open asset `{}`.", resolved_asset_name));
  }

  return LoadLayerFromMemory(asset.data(), asset.size(), resolved_asset_name, layer, warn, err,
                           options);
}

int USDZResolveAsset(const char *asset_name, const std::vector<std::string> &search_paths, std::string *resolved_asset_name, std::string *err, void *userdata) {

  DCOUT("Resolve asset: " << asset_name);

  if (!userdata) {
    if (err) {
      (*err) += "`userdata` must be non-null.\n";
    }
    return -2;
  }

  if (!asset_name) {
    if (err) {
      (*err) += "`asset_name` must be non-null.\n";
    }
    return -2;
  }

  if (!resolved_asset_name) {
    if (err) {
      (*err) += "`resolved_asset_name` must be non-null.\n";
    }
    return -2;
  }

  std::string asset_path = asset_name;

  // Remove relative path prefix './'
  if (lightusd::startsWith(asset_path, "./")) {
    asset_path = lightusd::removePrefix(asset_path, "./");
  }

  if (!IsSafeUSDZAssetPath(asset_path)) {
    if (err) {
      (*err) += "Unsafe asset path: `" + asset_path + "`\n";
    }
    return -2;
  }

  // Not used
  (void)search_paths;

  const USDZAsset *passet = reinterpret_cast<const USDZAsset *>(userdata);

  if (passet->asset_map.count(asset_path)) {
    DCOUT("Resolved asset: " << asset_name << " as " << asset_path);
    (*resolved_asset_name) = asset_path;
    return 0;
  }

  return -1; // not found
}

int USDZSizeAsset(const char *resolved_asset_name, uint64_t *nbytes, std::string *err, void *userdata) {

  if (!userdata) {
    if (err) {
      (*err) += "`userdata` must be non-null.\n";
    }
    return -2;
  }

  if (!resolved_asset_name) {
    if (err) {
      (*err) += "`resolved_asset_name` must be non-null.\n";
    }
    return -2;
  }

  if (!nbytes) {
    if (err) {
      (*err) += "`nbytes` must be non-null.\n";
    }
    return -2;
  }

  const USDZAsset *passet = reinterpret_cast<const USDZAsset *>(userdata);

  if (!passet->asset_map.count(resolved_asset_name)) {
    if (err) {
      (*err) += "resolved_asset_name `" + std::string(resolved_asset_name) + "` not found in USDZAsset.\n";
    }
    return -1;
  }

  std::pair<size_t, size_t> byte_range = passet->asset_map.at(resolved_asset_name);

  if (byte_range.first >= byte_range.second) {
    if (err) {
      (*err) += "Invalid USDZAsset byte range.\n";
    }
    return -2;
  }

  (*nbytes) = byte_range.second - byte_range.first;

  return 0;
}

int USDZReadAsset(const char *resolved_asset_name, uint64_t req_bytes, uint8_t *out_buf, uint64_t *nbytes, std::string *err, void *userdata) {
  if (!userdata) {
    if (err) {
      (*err) += "`userdata` must be non-null.\n";
    }
    return -1;
  }

  if (!resolved_asset_name) {
    if (err) {
      (*err) += "`resolved_asset_name` must be non-null.\n";
    }
    return -2;
  }

  if (!out_buf) {
    if (err) {
      (*err) += "`out_buf` must be non-null.\n";
    }
    return -2;
  }

  if (!nbytes) {
    if (err) {
      (*err) += "`nbytes` must be non-null.\n";
    }
    return -2;
  }

  const USDZAsset *passet = reinterpret_cast<const USDZAsset *>(userdata);

  if (!passet->asset_map.count(resolved_asset_name)) {
    if (err) {
      (*err) += "resolved_asset_name `" + std::string(resolved_asset_name) + "` not found in USDZAsset.\n";
    }
    return -1;
  }

  std::pair<size_t, size_t> byte_range = passet->asset_map.at(resolved_asset_name);

  if (byte_range.first >= byte_range.second) {
    if (err) {
      (*err) += "Invalid USDZAsset byte range.\n";
    }
    return -2;
  }

  size_t sz = byte_range.second - byte_range.first;

  if (sz > req_bytes) {
    if (err) {
      (*err) += "USDZAsset " + std::string(resolved_asset_name) + "'s size exceeds requested bytes.\n";
    }
    return -2;
  }

  const uint8_t *src = nullptr;
  size_t src_size = 0;
  if (!passet->data.empty()) {
    src = passet->data.data();
    src_size = passet->data.size();
  } else if (passet->addr && passet->size > 0) {
    src = passet->addr;
    src_size = passet->size;
  } else {
    if (err) {
      (*err) += "USDZAsset has no backing data.\n";
    }
    return -2;
  }

  if (byte_range.first + sz > src_size) {
    if (err) {
      (*err) += "Invalid USDZAsset size: " + std::string(resolved_asset_name) + "\n";
    }
    return -2;
  }

  memcpy(out_buf, src + byte_range.first, sz);
  (*nbytes) = sz;

  return 0;
}

bool SetupUSDZAssetResolution(
  AssetResolutionResolver &resolver,
  const USDZAsset *pusdzAsset)
{
  // https://openusd.org/release/spec_usdz.html

  if (!pusdzAsset) {
    return false;
  }
  // TODO: Validate Asset data.

  AssetResolutionHandler handler;
  handler.resolve_fun = USDZResolveAsset;
  handler.size_fun = USDZSizeAsset;
  handler.read_fun = USDZReadAsset;
  handler.write_fun = nullptr;
  handler.userdata = reinterpret_cast<void *>(const_cast<USDZAsset *>(pusdzAsset));

  resolver.register_asset_resolution_handler("png", handler);
  resolver.register_asset_resolution_handler("PNG", handler);
  resolver.register_asset_resolution_handler("JPG", handler);
  resolver.register_asset_resolution_handler("jpg", handler);
  resolver.register_asset_resolution_handler("jpeg", handler);
  resolver.register_asset_resolution_handler("JPEG", handler);
  resolver.register_asset_resolution_handler("exr", handler);
  resolver.register_asset_resolution_handler("EXR", handler);
  // HDR (Radiance HDR format) - commonly used for environment maps
  resolver.register_asset_resolution_handler("hdr", handler);
  resolver.register_asset_resolution_handler("HDR", handler);
  resolver.register_asset_resolution_handler("avif", handler);
  resolver.register_asset_resolution_handler("AVIF", handler);
  resolver.register_asset_resolution_handler("m4a", handler);
  resolver.register_asset_resolution_handler("M4A", handler);
  resolver.register_asset_resolution_handler("mp3", handler);
  resolver.register_asset_resolution_handler("MP3", handler);
  resolver.register_asset_resolution_handler("wav", handler);
  resolver.register_asset_resolution_handler("WAV", handler);
  resolver.register_asset_resolution_handler("usd", handler);
  resolver.register_asset_resolution_handler("USD", handler);
  resolver.register_asset_resolution_handler("usda", handler);
  resolver.register_asset_resolution_handler("USDA", handler);
  resolver.register_asset_resolution_handler("usdc", handler);
  resolver.register_asset_resolution_handler("USDC", handler);

  return true;
}

// ============================================================================
// USDZ Writer (AOUSD Core Spec Section 17)
// ============================================================================

namespace {

// CRC32 lookup table (IEEE 802.3 polynomial).
// Thread-safe one-time init via a function-local static (C++11 magic statics),
// so concurrent USDZ writes don't race on a hand-rolled init flag.
static const std::array<uint32_t, 256> &Usdz_CRC32Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int j = 0; j < 8; j++) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  return table;
}

static uint32_t ComputeCRC32(const uint8_t *data, size_t len) {
  const std::array<uint32_t, 256> &usdz_crc32_table = Usdz_CRC32Table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc = usdz_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

// USDZ requires all file data to be aligned at 64-byte boundaries.
// ZIP local file header: 30 bytes fixed + filename_len + extra_field_len
// We use extra_field padding to achieve alignment.
constexpr size_t kUSDZAlignment = 64;

// ZIP local file header fixed size
constexpr size_t kZipLocalHeaderSize = 30;

// Allowed file extensions per AOUSD Core Spec 17.2
bool IsAllowedUSDZExtension(const std::string &filename) {
  std::string ext;
  auto dot = filename.rfind('.');
  if (dot != std::string::npos) {
    ext = filename.substr(dot + 1);
  }
  // Convert to lowercase
  std::string lower_ext;
  for (char c : ext) {
    lower_ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return (lower_ext == "usd" || lower_ext == "usda" || lower_ext == "usdc" ||
          lower_ext == "png" || lower_ext == "jpg" || lower_ext == "jpeg" ||
          lower_ext == "exr" || lower_ext == "avif" ||
          lower_ext == "m4a" || lower_ext == "mp3" || lower_ext == "wav");
}

bool IsSafeUSDZEntryName(const std::string &name, std::string *reason) {
  if (name.empty()) {
    if (reason) *reason = "entry name is empty";
    return false;
  }
  if (name.size() > UINT16_MAX) {
    if (reason) *reason = "entry name exceeds ZIP filename length limit";
    return false;
  }
  if (name[0] == '/' || name[0] == '\\') {
    if (reason) *reason = "entry name must be relative";
    return false;
  }
  if (name.find('\0') != std::string::npos) {
    if (reason) *reason = "entry name contains NUL byte";
    return false;
  }
  if (name.find('\\') != std::string::npos) {
    if (reason) *reason = "entry name contains backslash";
    return false;
  }
  if (name.find(':') != std::string::npos) {
    if (reason) *reason = "entry name contains drive/URI separator";
    return false;
  }

  size_t pos = 0;
  while (pos <= name.size()) {
    size_t next = name.find('/', pos);
    std::string segment = (next == std::string::npos)
                              ? name.substr(pos)
                              : name.substr(pos, next - pos);
    if (segment.empty() || segment == "." || segment == "..") {
      if (reason) *reason = "entry name contains empty or dot segment";
      return false;
    }
    if (next == std::string::npos) break;
    pos = next + 1;
  }

  return true;
}

// Write a single entry to a USDZ archive buffer with 64-byte alignment.
// Returns false on error.
bool WriteUSDZEntry(std::vector<uint8_t> &buf,
                    std::vector<std::tuple<std::string, uint32_t, uint32_t, size_t>> &central_dir_entries,
                    const std::string &name, const uint8_t *data, size_t data_size,
                    std::string *err) {
  std::string reason;
  if (!IsSafeUSDZEntryName(name, &reason)) {
    if (err) {
      (*err) += "Unsafe USDZ entry name '" + name + "': " + reason + "\n";
    }
    return false;
  }
  if (data_size > UINT32_MAX) {
    if (err) {
      (*err) += "USDZ entry '" + name + "' exceeds ZIP32 size limit\n";
    }
    return false;
  }
  if (buf.size() > UINT32_MAX) {
    if (err) {
      (*err) += "USDZ archive exceeds ZIP32 offset limit before entry '" +
                name + "'\n";
    }
    return false;
  }

  // Calculate padding needed for 64-byte alignment of file data
  size_t header_size = kZipLocalHeaderSize + name.size();
  size_t padding = 0;
  size_t remainder = (buf.size() + header_size) % kUSDZAlignment;
  if (remainder != 0) {
    padding = kUSDZAlignment - remainder;
  }

  size_t local_header_offset = buf.size();

  // CRC32
  uint32_t crc = 0;
  if (data && data_size > 0) {
    crc = ComputeCRC32(data, data_size);
  }

  // Build local file header
  uint8_t header[kZipLocalHeaderSize];
  memset(header, 0, sizeof(header));

  // Signature: PK\003\004
  header[0] = 0x50; header[1] = 0x4b; header[2] = 0x03; header[3] = 0x04;
  // Version needed: 2.0
  header[4] = 20; header[5] = 0;
  // General purpose bit flag: 0
  // Compression method: 0 (stored/uncompressed)
  // Last mod time/date: 0
  // CRC-32
  memcpy(&header[14], &crc, 4);
  // Compressed size == uncompressed size (stored)
  uint32_t sz32 = static_cast<uint32_t>(data_size);
  memcpy(&header[18], &sz32, 4);
  memcpy(&header[22], &sz32, 4);
  // Filename length
  uint16_t name_len = static_cast<uint16_t>(name.size());
  memcpy(&header[26], &name_len, 2);
  // Extra field length (for alignment padding)
  uint16_t extra_len = static_cast<uint16_t>(padding);
  memcpy(&header[28], &extra_len, 2);

  // Write header
  buf.insert(buf.end(), header, header + kZipLocalHeaderSize);
  // Write filename
  buf.insert(buf.end(), name.begin(), name.end());
  // Write padding (extra field)
  if (padding > 0) {
    buf.insert(buf.end(), padding, 0);
  }

  // Verify alignment
  if ((buf.size() % kUSDZAlignment) != 0) {
    if (err) {
      (*err) += "Internal error: USDZ alignment failed for entry '" + name + "'\n";
    }
    return false;
  }

  // Write file data
  if (data && data_size > 0) {
    buf.insert(buf.end(), data, data + data_size);
  }
  if (buf.size() > UINT32_MAX) {
    if (err) {
      (*err) += "USDZ archive exceeds ZIP32 size limit after entry '" + name +
                "'\n";
    }
    return false;
  }

  // Record for central directory
  central_dir_entries.emplace_back(name, crc, sz32, local_header_offset);

  return true;
}

// Write the central directory and end-of-central-directory record.
bool WriteUSDZCentralDirectory(
    std::vector<uint8_t> &buf,
    const std::vector<std::tuple<std::string, uint32_t, uint32_t, size_t>> &entries,
    std::string *err) {
  if (entries.size() > UINT16_MAX) {
    if (err) {
      (*err) += "USDZ archive has too many entries for ZIP32\n";
    }
    return false;
  }
  if (buf.size() > UINT32_MAX) {
    if (err) {
      (*err) += "USDZ central directory offset exceeds ZIP32 limit\n";
    }
    return false;
  }
  size_t cd_offset = buf.size();

  for (const auto &entry : entries) {
    const auto &name = std::get<0>(entry);
    uint32_t crc = std::get<1>(entry);
    uint32_t size = std::get<2>(entry);
    size_t local_offset_size = std::get<3>(entry);
    if (name.size() > UINT16_MAX || local_offset_size > UINT32_MAX) {
      if (err) {
        (*err) += "USDZ central directory entry exceeds ZIP32 limits\n";
      }
      return false;
    }
    uint32_t local_offset = static_cast<uint32_t>(local_offset_size);

    uint8_t cdr[46];
    memset(cdr, 0, sizeof(cdr));

    // Signature: PK\001\002
    cdr[0] = 0x50; cdr[1] = 0x4b; cdr[2] = 0x01; cdr[3] = 0x02;
    // Version made by: 2.0
    cdr[4] = 20; cdr[5] = 0;
    // Version needed: 2.0
    cdr[6] = 20; cdr[7] = 0;
    // CRC-32
    memcpy(&cdr[16], &crc, 4);
    // Compressed size
    memcpy(&cdr[20], &size, 4);
    // Uncompressed size
    memcpy(&cdr[24], &size, 4);
    // Filename length
    uint16_t name_len = static_cast<uint16_t>(name.size());
    memcpy(&cdr[28], &name_len, 2);
    // Relative offset of local header
    memcpy(&cdr[42], &local_offset, 4);

    buf.insert(buf.end(), cdr, cdr + 46);
    buf.insert(buf.end(), name.begin(), name.end());
  }

  size_t cd_size = buf.size() - cd_offset;
  if (cd_size > UINT32_MAX || buf.size() > UINT32_MAX) {
    if (err) {
      (*err) += "USDZ central directory exceeds ZIP32 limits\n";
    }
    return false;
  }

  // End of central directory record
  uint8_t eocd[22];
  memset(eocd, 0, sizeof(eocd));
  // Signature: PK\005\006
  eocd[0] = 0x50; eocd[1] = 0x4b; eocd[2] = 0x05; eocd[3] = 0x06;
  // Number of entries on this disk
  uint16_t num_entries = static_cast<uint16_t>(entries.size());
  memcpy(&eocd[8], &num_entries, 2);
  // Total number of entries
  memcpy(&eocd[10], &num_entries, 2);
  // Size of central directory
  uint32_t cd_size32 = static_cast<uint32_t>(cd_size);
  memcpy(&eocd[12], &cd_size32, 4);
  // Offset of start of central directory
  uint32_t cd_offset32 = static_cast<uint32_t>(cd_offset);
  memcpy(&eocd[16], &cd_offset32, 4);

  buf.insert(buf.end(), eocd, eocd + 22);
  return true;
}

bool SerializeUSDZRootLayer(const Stage &stage, const USDZWriteOptions &options,
                            std::vector<uint8_t> *root_data,
                            std::string *root_name,
                            std::string *warn, std::string *err) {
  if (!root_data || !root_name) {
    if (err) { (*err) += "`root_data` or `root_name` is nullptr.\n"; }
    return false;
  }

  switch (options.root_layer_format) {
    case USDZRootLayerFormat::USDC: {
      if (!usdc::SaveAsUSDCToMemory(stage, root_data, warn, err,
                                    options.max_file_size_bytes,
                                    options.max_memory_bytes)) {
        if (err) { (*err) += "Failed to serialize root layer as USDC.\n"; }
        return false;
      }
      if (root_data->empty()) {
        if (err) { (*err) += "USDC serialization produced empty data.\n"; }
        return false;
      }
      *root_name = "root.usdc";
      return true;
    }
    case USDZRootLayerFormat::USDA: {
      std::string usda_text;
      if (!usda::ExportToUSDAString(stage, &usda_text, warn, err)) {
        if (err) { (*err) += "Failed to serialize root layer as USDA.\n"; }
        return false;
      }
      if (usda_text.empty()) {
        if (err) { (*err) += "USDA serialization produced empty data.\n"; }
        return false;
      }
      root_data->assign(usda_text.begin(), usda_text.end());
      *root_name = "root.usda";
      return true;
    }
  }

  if (err) { (*err) += "Invalid USDZ root layer format.\n"; }
  return false;
}

bool SerializeUSDZRootLayer(const Layer &layer, const USDZWriteOptions &options,
                            std::vector<uint8_t> *root_data,
                            std::string *root_name,
                            std::string *warn, std::string *err) {
  if (!root_data || !root_name) {
    if (err) { (*err) += "`root_data` or `root_name` is nullptr.\n"; }
    return false;
  }

  switch (options.root_layer_format) {
    case USDZRootLayerFormat::USDC: {
      if (!usdc::SaveAsUSDCToMemory(layer, root_data, warn, err,
                                    options.max_file_size_bytes,
                                    options.max_memory_bytes)) {
        if (err) { (*err) += "Failed to serialize root layer as USDC.\n"; }
        return false;
      }
      if (root_data->empty()) {
        if (err) { (*err) += "USDC serialization produced empty data.\n"; }
        return false;
      }
      *root_name = "root.usdc";
      return true;
    }
    case USDZRootLayerFormat::USDA: {
      const std::string usda_text = lightusd::print_layer(layer, 0);
      if (usda_text.empty()) {
        if (err) { (*err) += "USDA serialization produced empty data.\n"; }
        return false;
      }
      root_data->assign(usda_text.begin(), usda_text.end());
      *root_name = "root.usda";
      return true;
    }
  }

  if (err) { (*err) += "Invalid USDZ root layer format.\n"; }
  return false;
}

}  // namespace

bool SaveAsUSDZToMemory(const Stage &stage,
                        const std::map<std::string, std::vector<uint8_t>> &assets,
                        std::vector<uint8_t> *output,
                        const USDZWriteOptions &options,
                        std::string *warn, std::string *err) {
  if (!output) {
    if (err) { (*err) += "`output` is nullptr.\n"; }
    return false;
  }

  // Step 1: Serialize the root layer to memory.
  std::vector<uint8_t> root_data;
  std::string root_name;
  if (!SerializeUSDZRootLayer(stage, options, &root_data, &root_name, warn, err)) {
    return false;
  }

  // Step 2: Build USDZ archive
  std::vector<uint8_t> buf;
  buf.reserve(root_data.size() + 4096);  // rough estimate

  // entries: (name, crc32, size, local_header_offset)
  std::vector<std::tuple<std::string, uint32_t, uint32_t, size_t>> central_dir_entries;

  // Root layer must be the first entry (AOUSD Core Spec 17.2)
  if (!WriteUSDZEntry(buf, central_dir_entries, root_name,
                       root_data.data(), root_data.size(), err)) {
    return false;
  }

  // Step 3: Add additional assets
  for (const auto &asset : assets) {
    std::string reason;
    if (!IsSafeUSDZEntryName(asset.first, &reason)) {
      if (err) {
        (*err) += "Unsafe USDZ asset entry name '" + asset.first +
                  "': " + reason + "\n";
      }
      return false;
    }
    if (!IsAllowedUSDZExtension(asset.first)) {
      if (warn) {
        (*warn) += "Skipping asset with disallowed extension: " + asset.first + "\n";
      }
      continue;
    }

    if (!WriteUSDZEntry(buf, central_dir_entries, asset.first,
                         asset.second.data(), asset.second.size(), err)) {
      return false;
    }
  }

  // Step 4: Write central directory (must be at the end with no padding)
  if (!WriteUSDZCentralDirectory(buf, central_dir_entries, err)) {
    return false;
  }

  *output = std::move(buf);
  return true;
}

bool SaveAsUSDZToMemory(const Layer &layer,
                        const std::map<std::string, std::vector<uint8_t>> &assets,
                        std::vector<uint8_t> *output,
                        const USDZWriteOptions &options,
                        std::string *warn, std::string *err) {
  if (!output) {
    if (err) { (*err) += "`output` is nullptr.\n"; }
    return false;
  }

  std::vector<uint8_t> root_data;
  std::string root_name;
  if (!SerializeUSDZRootLayer(layer, options, &root_data, &root_name, warn, err)) {
    return false;
  }

  std::vector<uint8_t> buf;
  buf.reserve(root_data.size() + 4096);

  std::vector<std::tuple<std::string, uint32_t, uint32_t, size_t>> central_dir_entries;

  if (!WriteUSDZEntry(buf, central_dir_entries, root_name,
                       root_data.data(), root_data.size(), err)) {
    return false;
  }

  for (const auto &asset : assets) {
    std::string reason;
    if (!IsSafeUSDZEntryName(asset.first, &reason)) {
      if (err) {
        (*err) += "Unsafe USDZ asset entry name '" + asset.first +
                  "': " + reason + "\n";
      }
      return false;
    }
    if (!IsAllowedUSDZExtension(asset.first)) {
      if (warn) {
        (*warn) += "Skipping asset with disallowed extension: " + asset.first + "\n";
      }
      continue;
    }

    if (!WriteUSDZEntry(buf, central_dir_entries, asset.first,
                         asset.second.data(), asset.second.size(), err)) {
      return false;
    }
  }

  if (!WriteUSDZCentralDirectory(buf, central_dir_entries, err)) {
    return false;
  }

  *output = std::move(buf);
  return true;
}

bool SaveAsUSDZToMemory(const Stage &stage,
                        const std::map<std::string, std::vector<uint8_t>> &assets,
                        std::vector<uint8_t> *output,
                        std::string *warn, std::string *err) {
  return SaveAsUSDZToMemory(stage, assets, output, USDZWriteOptions{}, warn, err);
}

bool SaveAsUSDZToFile(const std::string &filename, const Stage &stage,
                      const std::map<std::string, std::vector<uint8_t>> &assets,
                      const USDZWriteOptions &options,
                      std::string *warn, std::string *err) {
  std::vector<uint8_t> usdz_data;

  if (!SaveAsUSDZToMemory(stage, assets, &usdz_data, options, warn, err)) {
    return false;
  }

  std::ofstream ofs(filename, std::ios::binary);
  if (!ofs) {
    if (err) {
      (*err) += "Failed to open file for writing: " + filename + "\n";
    }
    return false;
  }

  ofs.write(reinterpret_cast<const char *>(usdz_data.data()),
            static_cast<std::streamsize>(usdz_data.size()));
  if (!ofs) {
    if (err) {
      (*err) += "Failed to write USDZ data to file: " + filename + "\n";
    }
    return false;
  }

  return true;
}

bool SaveAsUSDZToFile(const std::string &filename, const Layer &layer,
                      const std::map<std::string, std::vector<uint8_t>> &assets,
                      const USDZWriteOptions &options,
                      std::string *warn, std::string *err) {
  std::vector<uint8_t> usdz_data;

  if (!SaveAsUSDZToMemory(layer, assets, &usdz_data, options, warn, err)) {
    return false;
  }

  std::ofstream ofs(filename, std::ios::binary);
  if (!ofs) {
    if (err) {
      (*err) += "Failed to open file for writing: " + filename + "\n";
    }
    return false;
  }

  ofs.write(reinterpret_cast<const char *>(usdz_data.data()),
            static_cast<std::streamsize>(usdz_data.size()));
  if (!ofs) {
    if (err) {
      (*err) += "Failed to write USDZ data to file: " + filename + "\n";
    }
    return false;
  }

  return true;
}

bool SaveAsUSDZToFile(const std::string &filename, const Stage &stage,
                      const std::map<std::string, std::vector<uint8_t>> &assets,
                      std::string *warn, std::string *err) {
  return SaveAsUSDZToFile(filename, stage, assets, USDZWriteOptions{}, warn, err);
}

// ============================================================================
// USDZ Validator (AOUSD Core Spec Section 17)
// ============================================================================

bool ValidateUSDZ(const uint8_t *addr, size_t length,
                  std::string *warn, std::string *err) {
  if (!addr || length < 4) {
    if (err) { (*err) += "Input data is null or too short.\n"; }
    return false;
  }

  // Check ZIP magic
  if (addr[0] != 0x50 || addr[1] != 0x4b || addr[2] != 0x03 || addr[3] != 0x04) {
    if (err) { (*err) += "Not a valid ZIP file (bad magic).\n"; }
    return false;
  }

  bool valid = true;
  size_t offset = 0;
  size_t entry_index = 0;
  bool found_usd_root = false;
  std::unordered_set<std::string> entry_names;

  while ((offset + kZipLocalHeaderSize) <= length) {
    // Check for local file header signature
    if (addr[offset] != 0x50 || addr[offset + 1] != 0x4b ||
        addr[offset + 2] != 0x03 || addr[offset + 3] != 0x04) {
      break;  // No more local headers
    }

    uint8_t local_header[30];
    memcpy(local_header, addr + offset, 30);
    offset += 30;

    // Compression method must be 0 (stored)
    uint16_t compr_method;
    memcpy(&compr_method, &local_header[8], 2);
    if (compr_method != 0) {
      if (err) {
        (*err) += "Entry " + std::to_string(entry_index) +
                  ": compression method must be 0 (stored), got " +
                  std::to_string(compr_method) + ".\n";
      }
      valid = false;
    }

    // Compressed size must equal uncompressed size (stored method)
    uint32_t compr_size;
    memcpy(&compr_size, &local_header[18], 4);
    uint32_t uncompr_size_hdr;
    memcpy(&uncompr_size_hdr, &local_header[22], 4);
    if (compr_size != uncompr_size_hdr) {
      if (err) {
        (*err) += "Entry " + std::to_string(entry_index) +
                  ": compressed size (" + std::to_string(compr_size) +
                  ") != uncompressed size (" + std::to_string(uncompr_size_hdr) +
                  ") for stored method.\n";
      }
      valid = false;
    }

    // General purpose bit flag must be 0 (no encryption, no data descriptor)
    uint16_t gp_flag;
    memcpy(&gp_flag, &local_header[6], 2);
    if (gp_flag != 0) {
      if (warn) {
        (*warn) += "Entry " + std::to_string(entry_index) +
                   ": general purpose bit flag is " + std::to_string(gp_flag) +
                   " (expected 0 for USDZ).\n";
      }
    }

    // Version needed to extract must be <= 20 (2.0, no ZIP64)
    uint16_t version_needed;
    memcpy(&version_needed, &local_header[4], 2);
    if (version_needed > 20) {
      if (warn) {
        (*warn) += "Entry " + std::to_string(entry_index) +
                   ": version needed " + std::to_string(version_needed) +
                   " > 20 (ZIP64 features not allowed in USDZ).\n";
      }
    }

    // Filename
    uint16_t name_len;
    memcpy(&name_len, &local_header[26], 2);
    if (offset + name_len > length) {
      if (err) { (*err) += "Truncated filename.\n"; }
      return false;
    }

    std::string name(reinterpret_cast<const char *>(addr + offset), name_len);
    offset += name_len;

    if (!IsSafeUSDZAssetPath(name)) {
      if (err) {
        (*err) += "Entry '" + name + "': unsafe USDZ entry path.\n";
      }
      valid = false;
    }
    if (!entry_names.insert(name).second) {
      if (err) {
        (*err) += "Entry '" + name + "': duplicate USDZ entry path.\n";
      }
      valid = false;
    }

    // Extra field
    uint16_t extra_len;
    memcpy(&extra_len, &local_header[28], 2);
    if (offset + extra_len > length) {
      if (err) { (*err) += "Truncated extra field.\n"; }
      return false;
    }
    offset += extra_len;

    // 64-byte alignment check
    if ((offset % kUSDZAlignment) != 0) {
      if (err) {
        (*err) += "Entry '" + name + "': data offset " +
                  std::to_string(offset) + " is not 64-byte aligned.\n";
      }
      valid = false;
    }

    // Check allowed extensions
    if (!IsAllowedUSDZExtension(name)) {
      if (warn) {
        (*warn) += "Entry '" + name +
                   "': file extension is not in the USDZ allowed list.\n";
      }
    }

    // The first entry must have an exact USD extension and matching file
    // magic. In particular, do not accept names such as root.usda.exe or a
    // USDC payload renamed to .usda.
    if (entry_index == 0) {
      if (uncompr_size_hdr <= length - offset &&
          GetUSDZRootFormat(name, addr + offset, uncompr_size_hdr) !=
              USDZRootFormat::Invalid) {
        found_usd_root = true;
      } else {
        if (err) {
          (*err) += "First entry must be a USD root layer whose extension "
                    "matches its content, got '" + name + "'.\n";
        }
        valid = false;
      }
    }

    // Validate file data CRC32
    uint32_t uncompr_size;
    memcpy(&uncompr_size, &local_header[22], 4);

    uint32_t header_crc;
    memcpy(&header_crc, &local_header[14], 4);

    if (uncompr_size > 0 && uncompr_size <= length - offset) {
      uint32_t actual_crc = ComputeCRC32(addr + offset, uncompr_size);
      if (header_crc != 0 && actual_crc != header_crc) {
        if (err) {
          (*err) += "Entry '" + name + "': CRC32 mismatch (header=" +
                    std::to_string(header_crc) + ", actual=" +
                    std::to_string(actual_crc) + ").\n";
        }
        valid = false;
      }
    } else if (uncompr_size > 0 && uncompr_size > length - offset) {
      if (err) {
        (*err) += "Entry '" + name + "': data extends beyond file end.\n";
      }
      return false;
    }

    offset += uncompr_size;

    entry_index++;
  }

  if (entry_index == 0) {
    if (err) { (*err) += "No entries found in ZIP archive.\n"; }
    return false;
  }

  if (!found_usd_root) {
    if (err) { (*err) += "No USD root layer found as first entry.\n"; }
    valid = false;
  }

  // Check that end-of-central-directory exists somewhere after the local headers
  // A minimal check: look for EOCD signature near the end
  bool found_eocd = false;
  if (length >= 22) {
    const size_t eocd_lower = (length > 65557) ? (length - 65557) : 0;
    for (size_t i = length - 22;; i--) {
      if (addr[i] == 0x50 && addr[i + 1] == 0x4b &&
          addr[i + 2] == 0x05 && addr[i + 3] == 0x06) {
        found_eocd = true;
        // Per spec: EOCD must be at the very end (no trailing data after comment)
        uint16_t comment_len;
        memcpy(&comment_len, addr + i + 20, 2);
        if (i + 22 + comment_len != length) {
          if (warn) {
            (*warn) += "End of central directory is not at the exact end of file.\n";
          }
        }
        break;
      }
      if (i == eocd_lower) {
        break;
      }
    }
  }

  if (!found_eocd) {
    if (err) { (*err) += "End of central directory record not found.\n"; }
    valid = false;
  } else {
    // Validate central directory entry count matches local headers
    const size_t eocd_lower = (length > 65557) ? (length - 65557) : 0;
    for (size_t i = length - 22;; i--) {
      if (addr[i] == 0x50 && addr[i + 1] == 0x4b &&
          addr[i + 2] == 0x05 && addr[i + 3] == 0x06) {
        uint16_t cd_entry_count;
        memcpy(&cd_entry_count, addr + i + 10, 2);
        if (cd_entry_count != static_cast<uint16_t>(entry_index)) {
          if (warn) {
            (*warn) += "Central directory entry count (" +
                       std::to_string(cd_entry_count) +
                       ") does not match local header count (" +
                       std::to_string(entry_index) + ").\n";
          }
        }
        break;
      }
      if (i == eocd_lower) {
        break;
      }
    }
  }

  return valid;
}

}  // namespace lightusd
