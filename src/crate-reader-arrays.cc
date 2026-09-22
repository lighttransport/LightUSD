// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-2022 Syoyo Fujita.
// Copyright 2023-Present Light Transport Entertainment Inc.
//
// Numeric array reading functions for CrateReader (int/float/double/half arrays)
//

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "crate-reader.hh"
#include "safe-arithmetic.hh"

#ifdef __wasi__
#else
#include <thread>
#endif

#include <algorithm>
#include <unordered_set>
#include <stack>

#include "crate-format.hh"
#include "parser-timing.hh"
#include "crate-pprint.hh"
#include "integerCoding.h"
#include "lz4-compression.hh"
#include "memory-budget.hh"
#include "path-util.hh"
#include "pprint-meta.hh"
#include "core/prim-spec.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"
#include "tiny-format.hh"
#include "str-util.hh"

//
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "nonstd/expected.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//

#include "common-macros.inc"

namespace lightusd {
namespace crate {

#define kTag "[Crate]"

#define CHECK_MEMORY_USAGE(__nbytes) \
  MEMORY_BUDGET_CHECK((*memory_manager_), (__nbytes), kTag)

#define REDUCE_MEMORY_USAGE(__nbytes) \
  memory_manager_->Release(__nbytes)

#define VERSION_LESS_THAN_0_8_0(__version) ((_version[0] == 0) && (_version[1] < 7))

template <class Int>
bool CrateReader::ReadCompressedInts(Int *out,
                                     size_t num_ints) {
  if (num_ints > _config.maxInts) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("# of ints {} too large. maxInts is set to {}", num_ints, _config.maxInts));
  }

  using Compressor =
      typename std::conditional<sizeof(Int) == 4, Usd_IntegerCompression,
                                Usd_IntegerCompression64>::type;

  size_t compBufferSize = Compressor::GetCompressedBufferSize(num_ints);

  uint64_t compSize;
  if (!sr()->read8(&compSize)) {
    return false;
  }

  if (compSize > compBufferSize) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "compSize exceeds compBufferSize (corrupted USDC).");
  }

  if (!sr()->can_read(compSize)) {
    PUSH_ERROR("Truncated compressed integer payload.");
    return false;
  }

  if (compSize < 4) {
    // Too small
    return false;
  }

  if (!ReserveDecompressionBuffers(compBufferSize, 0)) {
    return false;
  }

  if (!sr()->read_exact(size_t(compSize), size_t(compSize),
                reinterpret_cast<uint8_t *>(decomp_comp_buffer().data()))) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to read compressedInts.");
  }

  // Get working space size for decompression
  size_t workingSpaceSize = Compressor::GetDecompressionWorkingSpaceSize(num_ints);

  if (!ReserveDecompressionBuffers(compBufferSize, workingSpaceSize)) {
    return false;
  }

  bool ret = Compressor::DecompressFromBuffer(
      decomp_comp_buffer().data(), size_t(compSize), out, num_ints, &_err,
      decomp_working_buffer().data());

  return ret;
}

template <typename T>
bool CrateReader::ReadIntArray(bool is_compressed, std::vector<T> *d) {

  size_t length{0}; // uncompressed array elements.
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to read the number of array elements.");
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to read the number of array elements.");
      return false;
    }

#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif

    DCOUT("array.len = " << n);
    length = size_t(n);
  }

  DCOUT("array.len = " << length);
  if (length == 0) {
    d->clear();
    return true;
  }

  if (length > _config.maxArrayElements) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too large array elements.");
  }

  size_t byte_count;
  if (!safe::mul(sizeof(T), length, &byte_count)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow in array size computation");
  }
  CHECK_MEMORY_USAGE(byte_count);

  if ((!is_compressed || length < crate::kMinCompressedArraySize) &&
      !CheckArrayRead(length, sizeof(T))) {
    return false;
  }

  d->resize(length);

  if (!is_compressed) {

    if (!sr()->read_exact(byte_count, byte_count,
                   reinterpret_cast<uint8_t *>(d->data()))) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to read integer array data.");
    }

    return true;

  } else {

    if (length < crate::kMinCompressedArraySize) {
      size_t sz = sizeof(T) * length;
      // Not stored in compressed for smaller data
      if (!sr()->read_exact(sz, sz, reinterpret_cast<uint8_t *>(d->data()))) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to read uncompressed integer array data.");
      }
      return true;
    }

    return ReadCompressedInts(d->data(), d->size());
  }
}

bool CrateReader::ReadHalfArray(bool is_compressed,
                                std::vector<value::half> *d) {
  size_t length;
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Failed to read the number of array elements.");
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }

#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif
    length = size_t(n);
  }

  if (length > _config.maxArrayElements) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, fmt::format("Too many array elements {}.", length));
  }

  size_t half_bytes;
  if (!safe::mul(length, sizeof(uint16_t), &half_bytes)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow in half array size computation");
  }
  CHECK_MEMORY_USAGE(half_bytes);

  if ((!is_compressed || length < crate::kMinCompressedArraySize) &&
      !CheckArrayRead(length, sizeof(uint16_t))) {
    return false;
  }

  d->resize(length);

  if (!is_compressed) {


    if (!sr()->read_exact(sizeof(uint16_t) * length, sizeof(uint16_t) * length,
                   reinterpret_cast<uint8_t *>(d->data()))) {
      _err += "Failed to read half array data.\n";
      return false;
    }

    return true;
  } else {

    //
    // compressed data is represented by integers or look-up table.
    //

    if (length < crate::kMinCompressedArraySize) {
      size_t sz = sizeof(uint16_t) * length;
      // Not stored in compressed.
      // reader.ReadContiguous(odata, osize);
      if (!sr()->read_exact(sz, sz, reinterpret_cast<uint8_t *>(d->data()))) {
        _err += "Failed to read uncompressed array data.\n";
        return false;
      }
      return true;
    }

    // Read the code
    char code;
    if (!sr()->read1(&code)) {
      _err += "Failed to read the code.\n";
      return false;
    }

    if (code == 'i') {
      // Compressed integers.
      size_t tmp_bytes = sizeof(int32_t) * length;
      CHECK_MEMORY_USAGE(tmp_bytes);
      std::vector<int32_t> ints;
      ints.resize(length);
      if (!ReadCompressedInts(ints.data(), ints.size())) {
        REDUCE_MEMORY_USAGE(tmp_bytes);
        _err += "Failed to read compressed ints in ReadHalfArray.\n";
        return false;
      }
      for (size_t i = 0; i < length; i++) {
        float f = float(ints[i]);
        value::half h = value::float_to_half_full(f);
        (*d)[i] = h;
      }
      REDUCE_MEMORY_USAGE(tmp_bytes);
    } else if (code == 't') {
      // Lookup table & indexes.
      uint32_t lutSize;
      if (!sr()->read4(&lutSize)) {
        _err += "Failed to read lutSize in ReadHalfArray.\n";
        return false;
      }

      if (lutSize > _config.maxArrayElements) {
        _err += "LUT size too large in ReadHalfArray.\n";
        return false;
      }

      size_t lut_bytes = sizeof(value::half) * lutSize;
      size_t idx_bytes = sizeof(uint32_t) * length;
      CHECK_MEMORY_USAGE(lut_bytes + idx_bytes);

      if (!CheckArrayRead(lutSize, sizeof(value::half))) {
        return false;
      }
      std::vector<value::half> lut;
      lut.resize(lutSize);
      if (!sr()->read_exact(sizeof(value::half) * lutSize, sizeof(value::half) * lutSize,
                     reinterpret_cast<uint8_t *>(lut.data()))) {
        REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
        _err += "Failed to read lut table in ReadHalfArray.\n";
        return false;
      }

      std::vector<uint32_t> indexes;
      indexes.resize(length);
      if (!ReadCompressedInts(indexes.data(), indexes.size())) {
        REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
        _err += "Failed to read lut indices in ReadHalfArray.\n";
        return false;
      }

      auto o = d->data();
      for (auto index : indexes) {
        if (index >= lutSize) {
          REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
          _err += "LUT index out of bounds in ReadHalfArray.\n";
          return false;
        }
        *o++ = lut[index];
      }
      REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
    } else {
      _err += "Invalid code. Data is currupted\n";
      return false;
    }

    return true;
  }

}

bool CrateReader::ReadFloatArray(bool is_compressed, std::vector<float> *d) {

  size_t length;
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }

#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif
    length = size_t(n);
  }

  if (length > _config.maxArrayElements) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too many array elements.");
  }

  size_t float_bytes;
  if (!safe::mul(length, sizeof(float), &float_bytes)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow in float array size computation");
  }
  CHECK_MEMORY_USAGE(float_bytes);

  if ((!is_compressed || length < crate::kMinCompressedArraySize) &&
      !CheckArrayRead(length, sizeof(float))) {
    return false;
  }

  d->resize(length);

  if (!is_compressed) {

    if (!sr()->read_exact(sizeof(float) * length, sizeof(float) * length,
                   reinterpret_cast<uint8_t *>(d->data()))) {
      _err += "Failed to read float array data.\n";
      return false;
    }

    return true;
  } else {

    //
    // compressed data is represented by integers or look-up table.
    //

    if (length < crate::kMinCompressedArraySize) {
      size_t sz = sizeof(float) * length;
      // Not stored in compressed.
      // reader.ReadContiguous(odata, osize);
      if (!sr()->read_exact(sz, sz, reinterpret_cast<uint8_t *>(d->data()))) {
        _err += "Failed to read uncompressed array data.\n";
        return false;
      }
      return true;
    }

    // Read the code
    char code;
    if (!sr()->read1(&code)) {
      _err += "Failed to read the code.\n";
      return false;
    }

    if (code == 'i') {
      // Compressed integers.
      size_t tmp_bytes = sizeof(int32_t) * length;
      auto tmp_budget = memory_manager_->ReserveScoped(tmp_bytes);
      if (!tmp_budget.IsReserved()) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Memory budget exceeded for compressed float scratch buffer.");
      }
      std::vector<int32_t> ints;
      ints.resize(length);
      if (!ReadCompressedInts(ints.data(), ints.size())) {
        _err += "Failed to read compressed ints in ReadFloatArray.\n";
        return false;
      }
      for (size_t i = 0; i < length; i++) {
        d->data()[i] = float(ints[i]);
      }
    } else if (code == 't') {
      // Lookup table & indexes.
      uint32_t lutSize;
      if (!sr()->read4(&lutSize)) {
        _err += "Failed to read lutSize in ReadFloatArray.\n";
        return false;
      }

      if (lutSize > _config.maxArrayElements) {
        _err += "LUT size too large in ReadFloatArray.\n";
        return false;
      }

      size_t lut_bytes = sizeof(float) * lutSize;
      size_t idx_bytes = sizeof(uint32_t) * length;
      auto table_budget = memory_manager_->ReserveScoped(lut_bytes + idx_bytes);
      if (!table_budget.IsReserved()) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Memory budget exceeded for compressed float lookup table.");
      }

      if (!CheckArrayRead(lutSize, sizeof(float))) {
        return false;
      }
      std::vector<float> lut;
      lut.resize(lutSize);
      if (!sr()->read_exact(sizeof(float) * lutSize, sizeof(float) * lutSize,
                     reinterpret_cast<uint8_t *>(lut.data()))) {
        _err += "Failed to read lut table in ReadFloatArray.\n";
        return false;
      }

      std::vector<uint32_t> indexes;
      indexes.resize(length);
      if (!ReadCompressedInts(indexes.data(), indexes.size())) {
        _err += "Failed to read lut indices in ReadFloatArray.\n";
        return false;
      }

      auto o = d->data();
      for (auto index : indexes) {
        if (index >= lutSize) {
          _err += "LUT index out of bounds in ReadFloatArray.\n";
          return false;
        }
        *o++ = lut[index];
      }
    } else {
      _err += "Invalid code. Data is currupted\n";
      return false;
    }

    return true;
  }

}

bool CrateReader::ReadDoubleArray(bool is_compressed, std::vector<double> *d) {

  size_t length;
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }

#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif
    length = size_t(n);
  }

  if (length == 0) {
    d->clear();
    return true;
  }

  if (length > _config.maxArrayElements) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too many array elements.");
  }

  size_t double_bytes;
  if (!safe::mul(length, sizeof(double), &double_bytes)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow in double array size computation");
  }
  CHECK_MEMORY_USAGE(double_bytes);

  if ((!is_compressed || length < crate::kMinCompressedArraySize) &&
      !CheckArrayRead(length, sizeof(double))) {
    return false;
  }

  d->resize(length);

  if (!is_compressed) {

    if (!sr()->read_exact(sizeof(double) * length, sizeof(double) * length,
                   reinterpret_cast<uint8_t *>(d->data()))) {
      _err += "Failed to read double array data.\n";
      return false;
    }

    return true;
  } else {

    //
    // compressed data is represented by integers or look-up table.
    //

    d->resize(length);

    if (length < crate::kMinCompressedArraySize) {
      size_t sz = sizeof(double) * length;
      // Not stored in compressed.
      // reader.ReadContiguous(odata, osize);
      if (!sr()->read_exact(sz, sz, reinterpret_cast<uint8_t *>(d->data()))) {
        _err += "Failed to read uncompressed array data.\n";
        return false;
      }
      return true;
    }

    // Read the code
    char code;
    if (!sr()->read1(&code)) {
      _err += "Failed to read the code.\n";
      return false;
    }

    if (code == 'i') {
      // Compressed integers.
      size_t tmp_bytes = sizeof(int32_t) * length;
      auto tmp_budget = memory_manager_->ReserveScoped(tmp_bytes);
      if (!tmp_budget.IsReserved()) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Memory budget exceeded for compressed double scratch buffer.");
      }
      std::vector<int32_t> ints;
      ints.resize(length);
      if (!ReadCompressedInts(ints.data(), ints.size())) {
        _err += "Failed to read compressed ints in ReadDoubleArray.\n";
        return false;
      }
      std::copy(ints.begin(), ints.end(), d->data());
    } else if (code == 't') {
      // Lookup table & indexes.
      uint32_t lutSize;
      if (!sr()->read4(&lutSize)) {
        _err += "Failed to read lutSize in ReadDoubleArray.\n";
        return false;
      }

      if (lutSize > _config.maxArrayElements) {
        _err += "LUT size too large in ReadDoubleArray.\n";
        return false;
      }

      size_t lut_bytes = sizeof(double) * lutSize;
      size_t idx_bytes = sizeof(uint32_t) * length;
      auto table_budget = memory_manager_->ReserveScoped(lut_bytes + idx_bytes);
      if (!table_budget.IsReserved()) {
        PUSH_ERROR_AND_RETURN_TAG(kTag, "Memory budget exceeded for compressed double lookup table.");
      }

      if (!CheckArrayRead(lutSize, sizeof(double))) {
        return false;
      }
      std::vector<double> lut;
      lut.resize(lutSize);
      if (!sr()->read_exact(sizeof(double) * lutSize, sizeof(double) * lutSize,
                     reinterpret_cast<uint8_t *>(lut.data()))) {
        _err += "Failed to read lut table in ReadDoubleArray.\n";
        return false;
      }

      std::vector<uint32_t> indexes;
      indexes.resize(length);
      if (!ReadCompressedInts(indexes.data(), indexes.size())) {
        _err += "Failed to read lut indices in ReadDoubleArray.\n";
        return false;
      }

      auto o = d->data();
      for (auto index : indexes) {
        if (index >= lutSize) {
          _err += "LUT index out of bounds in ReadDoubleArray.\n";
          return false;
        }
        *o++ = lut[index];
      }
    } else {
      _err += "Invalid code. Data is currupted\n";
      return false;
    }

    return true;
  }
}

// TypedArray version with mmap support for float arrays
bool CrateReader::ReadFloatArrayTyped(bool is_compressed, TypedArray<float> *d) {
  size_t length;
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif
    length = size_t(n);
  }

  if (length > _config.maxArrayElements) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too many array elements.");
  }

  size_t float_bytes;
  if (!safe::mul(length, sizeof(float), &float_bytes)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow in float array size computation");
  }
  CHECK_MEMORY_USAGE(float_bytes);

  if ((!is_compressed || length < crate::kMinCompressedArraySize) &&
      !CheckArrayRead(length, sizeof(float))) {
    return false;
  }

  d->resize(length);

  if (!is_compressed) {
    if (!sr()->read_exact(sizeof(float) * length, sizeof(float) * length,
                   reinterpret_cast<uint8_t *>(d->data()))) {
      _err += "Failed to read float array data.\n";
      return false;
    }
    return true;
  } else {
    // Handle compressed data
    if (length < crate::kMinCompressedArraySize) {
      size_t sz = sizeof(float) * length;
      if (!sr()->read_exact(sz, sz, reinterpret_cast<uint8_t *>(d->data()))) {
        _err += "Failed to read uncompressed array data.\n";
        return false;
      }
      return true;
    }

    char code;
    if (!sr()->read1(&code)) {
      _err += "Failed to read the code.\n";
      return false;
    }

    if (code == 'i') {
      size_t tmp_bytes = sizeof(int32_t) * length;
      CHECK_MEMORY_USAGE(tmp_bytes);
      std::vector<int32_t> ints;
      ints.resize(length);
      if (!ReadCompressedInts(ints.data(), ints.size())) {
        REDUCE_MEMORY_USAGE(tmp_bytes);
        _err += "Failed to read compressed ints in ReadFloatArrayTyped.\n";
        return false;
      }
      for (size_t i = 0; i < length; i++) {
        d->data()[i] = float(ints[i]);
      }
      REDUCE_MEMORY_USAGE(tmp_bytes);
    } else if (code == 't') {
      uint32_t lutSize;
      if (!sr()->read4(&lutSize)) {
        _err += "Failed to read lutSize in ReadFloatArrayTyped.\n";
        return false;
      }

      if (lutSize > _config.maxArrayElements) {
        _err += "LUT size too large in ReadFloatArrayTyped.\n";
        return false;
      }

      size_t lut_bytes = sizeof(float) * lutSize;
      size_t idx_bytes = sizeof(uint32_t) * length;
      CHECK_MEMORY_USAGE(lut_bytes + idx_bytes);

      if (!CheckArrayRead(lutSize, sizeof(float))) {
        return false;
      }
      std::vector<float> lut;
      lut.resize(lutSize);
      if (!sr()->read_exact(sizeof(float) * lutSize, sizeof(float) * lutSize,
                     reinterpret_cast<uint8_t *>(lut.data()))) {
        REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
        _err += "Failed to read lut table in ReadFloatArrayTyped.\n";
        return false;
      }

      std::vector<uint32_t> indexes;
      indexes.resize(length);
      if (!ReadCompressedInts(indexes.data(), indexes.size())) {
        REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
        _err += "Failed to read lut indices in ReadFloatArrayTyped.\n";
        return false;
      }

      auto o = d->data();
      for (auto index : indexes) {
        if (index >= lutSize) {
          REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
          _err += "LUT index out of bounds in ReadFloatArrayTyped.\n";
          return false;
        }
        *o++ = lut[index];
      }
      REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
    } else {
      _err += "Invalid code. Data is corrupted\n";
      return false;
    }
    return true;
  }
}

// TypedArray version with mmap support for float arrays
bool CrateReader::ReadFloat2ArrayTyped(TypedArray<value::float2> *d) {
  size_t length;
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif
    length = size_t(n);
  }

  if (length > _config.maxArrayElements) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too many array elements.");
  }

  CHECK_MEMORY_USAGE(length * sizeof(value::float2));

  if (!CheckArrayRead(length, sizeof(value::float2))) {
    return false;
  }

  d->resize(length);

  if (!sr()->read_exact(sizeof(value::float2) * length, sizeof(value::float2) * length,
                 reinterpret_cast<uint8_t *>(d->data()))) {
    _err += "Failed to read float2 array data.\n";
    return false;
  }
  return true;
}

// TypedArray version with mmap support for double arrays
bool CrateReader::ReadDoubleArrayTyped(bool is_compressed, TypedArray<double> *d) {
  size_t length;
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif
    length = size_t(n);
  }

  if (length == 0) {
    d->clear();
    return true;
  }

  if (length > _config.maxArrayElements) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too many array elements.");
  }

  size_t double_bytes;
  if (!safe::mul(length, sizeof(double), &double_bytes)) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Integer overflow in double array size computation");
  }
  CHECK_MEMORY_USAGE(double_bytes);

  if ((!is_compressed || length < crate::kMinCompressedArraySize) &&
      !CheckArrayRead(length, sizeof(double))) {
    return false;
  }

  d->resize(length);

  if (!is_compressed) {
    if (!sr()->read_exact(sizeof(double) * length, sizeof(double) * length,
                   reinterpret_cast<uint8_t *>(d->data()))) {
      _err += "Failed to read double array data.\n";
      return false;
    }
    return true;
  } else {
    // Handle compressed data
    if (length < crate::kMinCompressedArraySize) {
      size_t sz = sizeof(double) * length;
      if (!sr()->read_exact(sz, sz, reinterpret_cast<uint8_t *>(d->data()))) {
        _err += "Failed to read uncompressed array data.\n";
        return false;
      }
      return true;
    }

    char code;
    if (!sr()->read1(&code)) {
      _err += "Failed to read the code.\n";
      return false;
    }

    if (code == 'i') {
      size_t tmp_bytes = sizeof(int64_t) * length;
      CHECK_MEMORY_USAGE(tmp_bytes);
      std::vector<int64_t> ints;
      ints.resize(length);
      if (!ReadCompressedInts(ints.data(), ints.size())) {
        REDUCE_MEMORY_USAGE(tmp_bytes);
        _err += "Failed to read compressed ints in ReadDoubleArrayTyped.\n";
        return false;
      }
      std::transform(ints.begin(), ints.end(), d->data(),
                     [](int64_t v) { return static_cast<double>(v); });
      REDUCE_MEMORY_USAGE(tmp_bytes);
    } else if (code == 't') {
      uint32_t lutSize;
      if (!sr()->read4(&lutSize)) {
        _err += "Failed to read lutSize in ReadDoubleArrayTyped.\n";
        return false;
      }

      if (lutSize > _config.maxArrayElements) {
        _err += "LUT size too large in ReadDoubleArrayTyped.\n";
        return false;
      }

      size_t lut_bytes = sizeof(double) * lutSize;
      size_t idx_bytes = sizeof(uint32_t) * length;
      CHECK_MEMORY_USAGE(lut_bytes + idx_bytes);

      if (!CheckArrayRead(lutSize, sizeof(double))) {
        return false;
      }
      std::vector<double> lut;
      lut.resize(lutSize);
      if (!sr()->read_exact(sizeof(double) * lutSize, sizeof(double) * lutSize,
                     reinterpret_cast<uint8_t *>(lut.data()))) {
        REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
        _err += "Failed to read lut table in ReadDoubleArrayTyped.\n";
        return false;
      }

      std::vector<uint32_t> indexes;
      indexes.resize(length);
      if (!ReadCompressedInts(indexes.data(), indexes.size())) {
        REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
        _err += "Failed to read lut indices in ReadDoubleArrayTyped.\n";
        return false;
      }

      auto o = d->data();
      for (auto index : indexes) {
        if (index >= lutSize) {
          REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
          _err += "LUT index out of bounds in ReadDoubleArrayTyped.\n";
          return false;
        }
        *o++ = lut[index];
      }
      REDUCE_MEMORY_USAGE(lut_bytes + idx_bytes);
    } else {
      _err += "Invalid code. Data is corrupted\n";
      return false;
    }
    return true;
  }
}

// Template implementation for TypedArray version with mmap support for integer arrays
template <typename T>
bool CrateReader::ReadIntArrayTyped(bool is_compressed, TypedArray<T> *d) {
  size_t length{0};
  // < ver 0.7.0  use 32bit
  if (VERSION_LESS_THAN_0_8_0(_version)) {
      uint32_t shapesize; // not used
      if (!sr()->read4(&shapesize)) {
        PUSH_ERROR("Failed to read the number of array elements.");
        return false;
      }
    uint32_t n;
    if (!sr()->read4(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
    length = size_t(n);
  } else {
    uint64_t n;
    if (!sr()->read8(&n)) {
      _err += "Failed to read the number of array elements.\n";
      return false;
    }
#if SIZE_MAX < UINT64_MAX
    if (n > static_cast<uint64_t>(SIZE_MAX)) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array element count exceeds addressable memory");
    }
#endif
    length = size_t(n);
  }

  if (length == 0) {
    d->clear();
    return true;
  }

  if (length > _config.maxInts) {
    PUSH_ERROR_AND_RETURN_TAG(kTag, "Too many int elements.");
  }

  CHECK_MEMORY_USAGE(length * sizeof(T));

  if (!is_compressed && !CheckArrayRead(length, sizeof(T))) {
    return false;
  }

  d->resize(length);

  if (!is_compressed) {
    if (!sr()->read_exact(sizeof(T) * length, sizeof(T) * length,
                   reinterpret_cast<uint8_t *>(d->data()))) {
      _err += "Failed to read int array data.\n";
      return false;
    }
    return true;
  } else {
    // Handle compressed data
    if (!ReadCompressedInts(d->data(), length)) {
      _err += "Failed to read compressed int array.\n";
      return false;
    }
    return true;
  }
}

// Explicit template instantiations
template bool CrateReader::ReadCompressedInts<int32_t>(int32_t*, size_t);
template bool CrateReader::ReadCompressedInts<uint32_t>(uint32_t*, size_t);
template bool CrateReader::ReadCompressedInts<int64_t>(int64_t*, size_t);
template bool CrateReader::ReadCompressedInts<uint64_t>(uint64_t*, size_t);
template bool CrateReader::ReadIntArray<int32_t>(bool, std::vector<int32_t>*);
template bool CrateReader::ReadIntArray<uint32_t>(bool, std::vector<uint32_t>*);
template bool CrateReader::ReadIntArray<int64_t>(bool, std::vector<int64_t>*);
template bool CrateReader::ReadIntArray<uint64_t>(bool, std::vector<uint64_t>*);
template bool CrateReader::ReadIntArrayTyped<int32_t>(bool, TypedArray<int32_t>*);
template bool CrateReader::ReadIntArrayTyped<uint32_t>(bool, TypedArray<uint32_t>*);
template bool CrateReader::ReadIntArrayTyped<int64_t>(bool, TypedArray<int64_t>*);
template bool CrateReader::ReadIntArrayTyped<uint64_t>(bool, TypedArray<uint64_t>*);

} // namespace crate
} // namespace lightusd
