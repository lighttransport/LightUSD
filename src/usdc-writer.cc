// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

#include "core/native-io.hh"
#include "usdc-writer.hh"
#include <chrono>
#include <cstdlib>
#include <iostream>

#if !defined(LIGHTUSD_DISABLE_MODULE_USDC_WRITER)

#if defined(_MSC_VER) || defined(__MINGW32__)
#if defined(__clang__)
// No need to define NOMINMAX for llvm-mingw
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>  // include API for expanding a file path

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif


#include <iostream>
#include <cstdio>    // for FILE, fopen, fwrite, fclose
#include <cstring>   // for strerror
#include <cerrno>    // for errno

#include "crate-writer.hh"  // experimental CrateWriter
#include "zstd-compression.hh"

#include "common-macros.inc"

namespace lightusd {
namespace usdc {

namespace {

bool HasZstdExtension(const std::string &filename) {
  if (filename.size() < 4) return false;
  std::string ext = filename.substr(filename.size() - 4);
  return (ext == ".zst" || ext == ".ZST");
}

#ifdef _WIN32
std::wstring UTF8ToWchar(const std::string &str) {
  int wstr_size =
      MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), nullptr, 0);
  std::wstring wstr(size_t(wstr_size), 0);
  MultiByteToWideChar(CP_UTF8, 0, str.data(), int(str.size()), &wstr[0],
                      int(wstr.size()));
  return wstr;
}

#endif

bool WriteAllBytesToFile(FILE *fp, const uint8_t *data, size_t size,
                         std::string *err) {
  if (!fp) {
    if (err) {
      (*err) += "Invalid file handle.\n";
    }
    return false;
  }

  if (size == 0) {
    return true;
  }

  if (!data) {
    if (err) {
      (*err) += "Invalid output data buffer.\n";
    }
    return false;
  }

  size_t written = 0;
  while (written < size) {
    const size_t n = fwrite(data + written, 1, size - written, fp);
    if (n == 0) {
      if (ferror(fp)) {
        if (err) {
          (*err) += "Failed to write data to a file: ";
          (*err) += strerror(errno);
          (*err) += "\n";
        }
      } else if (err) {
        (*err) += "Failed to write data to a file: fwrite made no progress.\n";
      }
      return false;
    }
    written += n;
  }

  return true;
}

}  // namespace

bool SaveAsUSDCToFile(const std::string &filename, const Stage &stage,
                      std::string *warn, std::string *err,
                      const USDWriteOptions &options) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

  std::vector<uint8_t> output;

  if (!SaveAsUSDCToMemory(stage, &output, warn, err, /*max_file_size*/ 0,
                          /*max_memory*/ 0, options.compress_float_arrays,
                          options.profile_crate)) {
    return false;
  }

  // Check if we should use zstd compression
  bool use_compression = options.use_zstd_compression || HasZstdExtension(filename);

  const uint8_t *write_data = output.data();
  size_t write_size = output.size();
  std::vector<uint8_t> compressed;

  if (use_compression) {
#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION
    if (!ZstdCompression::Compress(output.data(), output.size(),
                                   &compressed, options.zstd_compression_level, err)) {
      return false;
    }
    write_data = compressed.data();
    write_size = compressed.size();
    std::cout << "Compressing USDC with zstd (" << output.size() << " -> " << compressed.size() << " bytes)\n";
#else
    if (err) {
      (*err) = "zstd compression requested but LIGHTUSD_WITH_ZSTD_COMPRESSION is not enabled.\n";
    }
    return false;
#endif
  }

#ifdef _WIN32
#if defined(_MSC_VER) || defined(__GLIBCXX__) || defined(__clang__)
  FILE *fp = nullptr;
  errno_t fperr = _wfopen_s(&fp, UTF8ToWchar(filename).c_str(), L"wb");
  if (fperr != 0) {
    if (err) {
      (*err) += "Failed to open file `" + filename + "` to write.\n";
    }
    return false;
  }
#else
  FILE *fp = nullptr;
  errno_t fperr = fopen_s(&fp, filename.c_str(), "wb");
  if (fperr != 0) {
    if (err) {
      (*err) += "Failed to open file `" + filename + "` to write.\n";
    }
    return false;
  }
#endif

#else
  FILE *fp = fopen(filename.c_str(), "wb");
  if (fp == nullptr) {
    if (err) {
      (*err) += "Failed to open file `" + filename + "` to write.\n";
    }
    return false;
  }
#endif

  if (!WriteAllBytesToFile(fp, write_data, write_size, err)) {
    fclose(fp);
    return false;
  }

  fclose(fp);
  return true;
}

bool SaveAsUSDCToFile(const std::string &filename, const Layer &layer,
                      std::string *warn, std::string *err,
                      const USDWriteOptions &options) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

  std::vector<uint8_t> output;
  if (!SaveAsUSDCToMemory(layer, &output, warn, err, /*max_file_size*/ 0,
                          /*max_memory*/ 0, options.compress_float_arrays,
                          options.profile_crate)) {
    return false;
  }

  bool use_compression = options.use_zstd_compression || HasZstdExtension(filename);
  const uint8_t *write_data = output.data();
  size_t write_size = output.size();
  std::vector<uint8_t> compressed;

  if (use_compression) {
#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION
    if (!ZstdCompression::Compress(output.data(), output.size(),
                                   &compressed, options.zstd_compression_level, err)) {
      return false;
    }
    write_data = compressed.data();
    write_size = compressed.size();
#else
    std::vector<uint8_t> compressed;
    (void)compressed;
    if (err) {
      (*err) = "zstd compression requested but LIGHTUSD_WITH_ZSTD_COMPRESSION is not enabled.\n";
    }
    return false;
#endif
  }

  FILE *fp = fopen(filename.c_str(), "wb");
  if (!fp) {
    if (err) {
      (*err) += "Failed to open file: " + filename + "\n";
    }
    return false;
  }

  if (!WriteAllBytesToFile(fp, write_data, write_size, err)) {
    fclose(fp);
    return false;
  }

  fclose(fp);
  return true;
}

bool SaveAsUSDCToMemory(const Stage &stage, std::vector<uint8_t> *output,
                        std::string *warn, std::string *err,
                        int64_t max_file_size_bytes,
                        int64_t max_memory_bytes,
                        bool compress_float_arrays,
                        bool profile_crate) {
  (void)warn;

  if (!output) {
    if (err) {
      (*err) += "Output buffer is null.\n";
    }
    return false;
  }

  // Write directly to memory via MemoryOutputStream
  auto mem_stream = std::unique_ptr<experimental::MemoryOutputStream>(
      new experimental::MemoryOutputStream());
  auto* mem_ptr = mem_stream.get();

  experimental::CrateWriter writer(
      std::unique_ptr<experimental::IOutputStream>(std::move(mem_stream)));

  experimental::CrateWriter::Options opts;
  opts.version_major = 0;
  opts.version_minor = 8;
  opts.version_patch = 0;
  opts.enable_compression = true;
  opts.enable_deduplication = true;
  opts.enable_float_array_compression = compress_float_arrays;
  opts.profile_crate = profile_crate;
  // Optional caller overrides for the (intentionally small on WASM) resource
  // limits; 0 keeps the built-in default.
  if (max_file_size_bytes > 0) opts.max_file_size_bytes = max_file_size_bytes;
  if (max_memory_bytes > 0) opts.max_memory_bytes = max_memory_bytes;
  writer.SetOptions(opts);

  std::string open_err;
  if (!writer.Open(&open_err)) {
    if (err) {
      (*err) += "Failed to open CrateWriter: " + open_err + "\n";
    }
    return false;
  }

  const bool flatten_timing = profile_crate;
  auto phase_start = std::chrono::steady_clock::now();
  auto report_phase = [&](const char* name) {
    if (!flatten_timing) return;
    auto now = std::chrono::steady_clock::now();
    std::cerr << "[lightusd] usdc-write phase " << name << ": "
              << std::chrono::duration<double>(now - phase_start).count()
              << " s\n";
    phase_start = now;
  };

  std::string convert_err;
  if (!writer.ConvertStageToSpecs(stage, &convert_err)) {
    if (err) {
      (*err) += "Failed to convert Stage to USDC: " + convert_err + "\n";
    }
    return false;
  }
  report_phase("ConvertStageToSpecs");

  std::string finalize_err;
  if (!writer.Finalize(&finalize_err)) {
    if (err) {
      (*err) += "Failed to finalize USDC: " + finalize_err + "\n";
    }
    return false;
  }
  report_phase("Finalize");

  writer.Close();

  *output = mem_ptr->TakeBuffer();
  report_phase("Close+TakeBuffer");
  return true;
}

bool SaveAsUSDCToMemory(const Layer &layer, std::vector<uint8_t> *output,
                        std::string *warn, std::string *err,
                        int64_t max_file_size_bytes,
                        int64_t max_memory_bytes,
                        bool compress_float_arrays,
                        bool profile_crate) {
  (void)warn;

  if (!output) {
    if (err) {
      (*err) += "Output buffer is null.\n";
    }
    return false;
  }

  auto mem_stream = std::unique_ptr<experimental::MemoryOutputStream>(
      new experimental::MemoryOutputStream());
  auto* mem_ptr = mem_stream.get();

  experimental::CrateWriter writer(
      std::unique_ptr<experimental::IOutputStream>(std::move(mem_stream)));

  experimental::CrateWriter::Options opts;
  opts.version_major = 0;
  opts.version_minor = 8;
  opts.version_patch = 0;
  opts.enable_compression = true;
  opts.enable_deduplication = true;
  opts.enable_float_array_compression = compress_float_arrays;
  opts.profile_crate = profile_crate;
  if (max_file_size_bytes > 0) opts.max_file_size_bytes = max_file_size_bytes;
  if (max_memory_bytes > 0) opts.max_memory_bytes = max_memory_bytes;
  writer.SetOptions(opts);

  std::string open_err;
  if (!writer.Open(&open_err)) {
    if (err) {
      (*err) += "Failed to open CrateWriter: " + open_err + "\n";
    }
    return false;
  }

  std::string convert_err;
  if (!writer.ConvertLayerToSpecs(layer, &convert_err)) {
    if (err) {
      (*err) += "Failed to convert Layer to USDC: " + convert_err + "\n";
    }
    return false;
  }

  std::string finalize_err;
  if (!writer.Finalize(&finalize_err)) {
    if (err) {
      (*err) += "Failed to finalize USDC: " + finalize_err + "\n";
    }
    return false;
  }

  writer.Close();

  *output = mem_ptr->TakeBuffer();
  return true;
}

}  // namespace usdc
}  // namespace lightusd

#else

namespace lightusd {
namespace usdc {

bool SaveAsUSDCToFile(const std::string &filename, const Stage &stage,
                      std::string *warn, std::string *err,
                      const USDWriteOptions &options) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

  (void)filename;
  (void)stage;
  (void)warn;
  (void)options;

  if (err) {
    (*err) = "USDC writer feature is disabled in this build.\n";
  }

  return false;
}

bool SaveAsUSDCToFile(const std::string &filename, const Layer &layer,
                      std::string *warn, std::string *err,
                      const USDWriteOptions &options) {
  if (!::lightusd::NativeIOAvailable(err)) return false;

  (void)filename;
  (void)layer;
  (void)warn;
  (void)options;

  if (err) {
    (*err) = "USDC writer feature is disabled in this build.\n";
  }

  return false;
}

bool SaveAsUSDCToMemory(const Stage &stage, std::vector<uint8_t> *output,
                        std::string *warn, std::string *err,
                        int64_t max_file_size_bytes,
                        int64_t max_memory_bytes,
                        bool compress_float_arrays,
                        bool profile_crate) {
  (void)compress_float_arrays;
  (void)profile_crate;
  (void)stage;
  (void)output;
  (void)warn;
  (void)max_file_size_bytes;
  (void)max_memory_bytes;

  if (err) {
    (*err) = "USDC writer feature is disabled in this build.\n";
  }

  return false;
}

bool SaveAsUSDCToMemory(const Layer &layer, std::vector<uint8_t> *output,
                        std::string *warn, std::string *err,
                        int64_t max_file_size_bytes,
                        int64_t max_memory_bytes,
                        bool compress_float_arrays,
                        bool profile_crate) {
  (void)compress_float_arrays;
  (void)profile_crate;
  (void)layer;
  (void)output;
  (void)warn;
  (void)max_file_size_bytes;
  (void)max_memory_bytes;

  if (err) {
    (*err) = "USDC writer feature is disabled in this build.\n";
  }

  return false;
}

}  // namespace usdc
}  // namespace lightusd

#endif
