// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate Reader array unpackers

#include "crate-reader-internal.hh"

#include "crate-data-source.hh"
#include "lazy-array.hh"
#include "safe-arithmetic.hh"

#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace lightusd {
namespace next {

bool CrateReader::Impl::UnpackArray(ValueRep rep, Value& out) {
  CrateTypeId type_id = rep.type_id();

  // Fast path: keep numeric POD arrays as lazy references into the retained
  // source buffer instead of decoding them now. They are materialized on first
  // access and can be written back via verbatim byte pass-through. Token/string
  // and unsupported array types fall through to eager decode below.
  if (options_.lazy_arrays && source_ &&
      CrateArrayTypeCanBeLazy(type_id, rep.is_compressed())) {
    LazyArrayRef lr;
    // Pass the configured cap, not SIZE_MAX: otherwise a lazy Value could
    // advertise an element count (via Value::array_size()) that eager decode
    // would have rejected outright, and a consumer sizing a buffer from it
    // allocates on that number long before materialization refuses the array.
    if (ProbeArrayBlock(source_, rep,
                        static_cast<size_t>(options_.max_array_elements),
                        &lr) &&
        lr.element_count <=
            static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) &&
        (rep.payload() == 0 || lr.block_len > 0)) {
      lr.max_elements = options_.max_array_elements;
      out = Value::MakeLazyArray(lr);
      return true;
    }
    // Malformed header: fall through to eager decode, which reports the error.
  }

  // The representation's compressed bit is authoritative. The AOUSD advice
  // not to compress small arrays constrains writers, not the reader layout.
  // payload()==0 (non-inlined) denotes an empty array (pxrUSD convention); no
  // data block to seek to. Otherwise seek to the block and read the count.
  uint64_t count = 0;
  if (rep.payload() != 0) {
    if (!SeekToPayload(reader(), rep)) return false;
    // u32 count for crate < 0.7.0, u64 for >= 0.7.0 (pxr crateFile.cpp
    // _ReadUncompressedArray); element data immediately follows the count.
    if (!ReadCrateArrayCount(*reader(), version_, &count)) return false;
  }

  // Bound the file-controlled element count before any allocation.
  if (count > options_.max_array_elements) {
    AddWarning("Array element count exceeds max_array_elements limit");
    return false;
  }

  const bool compressed = rep.is_compressed();

  // Guard the element allocation BEFORE allocating any std::vector(count). The
  // per-type cases below allocate count*elem bytes first and only then call
  // read_raw() (whose has_elements() check would catch an over-long uncompressed
  // run) -- so a malformed count (e.g. 1e9 matrix4d = 128 GB) would OOM before
  // that check. Bound it here against the file-size-relative cap. For an
  // uncompressed array the element bytes must physically be present in the file;
  // for a compressed array the file-relative cap still limits the decoded size.
  {
    const uint64_t stride = CrateArrayElemStride(type_id);
    const uint64_t elem_bytes = stride ? stride : 1;
    if (!compressed && stride > 0 && !reader()->has_elements(
            static_cast<size_t>(count), static_cast<size_t>(stride))) {
      AddWarning("Array element count exceeds available file bytes");
      return false;
    }
    if (elem_bytes != 0 &&
        count > (std::numeric_limits<uint64_t>::max)() / elem_bytes) {
      AddWarning("Array payload byte size overflow");
      return false;
    }
    if (!CheckByteAllocation(count * elem_bytes, "Array payload")) {
      return false;
    }
  }

  // Decode a compressed integer array ([u64 compSize][LZ4(delta) blob]) in place.
  auto read_compressed_u32_n = [&](uint32_t* dst, size_t n) -> bool {
    uint64_t comp_size;
    if (!reader()->read_u64(comp_size)) return false;
    if (comp_size >
            static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
        static_cast<size_t>(comp_size) > reader()->remaining()) {
      return false;
    }
    size_t prefixed_size = 0;
    if (!safe::add(size_t{8}, static_cast<size_t>(comp_size), &prefixed_size)) return false;
    std::vector<uint8_t> with_prefix(prefixed_size);
    std::memcpy(with_prefix.data(), &comp_size, 8);
    if (comp_size > 0) {
      if (!reader()->read(with_prefix.data() + 8, static_cast<size_t>(comp_size))) return false;
    }
    DecompressResult dr = DecompressCompressedU32(
        with_prefix.data(), with_prefix.size(), dst, n);
    if (!dr.success) {
      AddWarning("Failed to decompress integer array: " + dr.error);
      return false;
    }
    return true;
  };
  auto read_compressed_u32 = [&](uint32_t* dst) -> bool {
    return read_compressed_u32_n(dst, static_cast<size_t>(count));
  };

  auto read_compressed_u64_n = [&](uint64_t* dst, size_t n) -> bool {
    uint64_t comp_size;
    if (!reader()->read_u64(comp_size)) return false;
    if (comp_size >
            static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) ||
        static_cast<size_t>(comp_size) > reader()->remaining()) {
      return false;
    }
    size_t prefixed_size = 0;
    if (!safe::add(size_t{8}, static_cast<size_t>(comp_size), &prefixed_size)) return false;
    std::vector<uint8_t> with_prefix(prefixed_size);
    std::memcpy(with_prefix.data(), &comp_size, 8);
    if (comp_size > 0) {
      if (!reader()->read(with_prefix.data() + 8, static_cast<size_t>(comp_size))) return false;
    }
    DecompressResult dr = DecompressCompressedU64(
        with_prefix.data(), with_prefix.size(), dst, n);
    if (!dr.success) {
      AddWarning("Failed to decompress 64-bit integer array: " + dr.error);
      return false;
    }
    return true;
  };
  auto read_compressed_u64 = [&](uint64_t* dst) -> bool {
    return read_compressed_u64_n(dst, static_cast<size_t>(count));
  };

  // Read `count` raw elements of `elem_size` bytes into `dst` (overflow-safe).
  auto read_raw = [&](void* dst, size_t elem_size) -> bool {
    if (count == 0) return true;
    if (!reader()->has_elements(count, elem_size)) return false;
    size_t byte_count;
    if (!safe::mul(static_cast<size_t>(count), elem_size, &byte_count)) return false;
    return reader()->read(dst, byte_count);
  };

  // Decode a compressed floating-point array (pxrUSD format): [i8 code] then
  // 'i' -> compressed int32s cast to T; 't' -> [u32 lutSize][T lut][compressed
  // u32 indices]. Fills `n` scalar elements of T into `dst`.
  auto read_compressed_floating_n = [&](auto* dst, size_t n) -> bool {
    using T = typename std::remove_pointer<decltype(dst)>::type;
    int8_t code = 0;
    if (!reader()->read_i8(code)) return false;
    if (code == 'i') {
      if (sizeof(T) == 4) {
        if (!read_compressed_u32_n(reinterpret_cast<uint32_t*>(dst), n)) return false;
        uint32_t* u32_dst = reinterpret_cast<uint32_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
          dst[i] = static_cast<T>(static_cast<int32_t>(u32_dst[i]));
        }
      } else {
        std::vector<uint32_t> ints(n);
        if (!read_compressed_u32_n(ints.data(), n)) return false;
        for (size_t i = 0; i < n; ++i) {
          dst[i] = static_cast<T>(static_cast<int32_t>(ints[i]));
        }
      }
      return true;
    }
    if (code == 't') {
      uint32_t lut_size = 0;
      if (!reader()->read_u32(lut_size)) return false;
      if (lut_size == 0 || lut_size > options_.max_array_elements) return false;
      // The LUT holds lut_size elements read directly from the file; bound the
      // count against the remaining file before allocating to avoid a
      // malformed-count huge allocation ahead of the read below.
      if (!reader()->has_elements(size_t(lut_size), sizeof(T))) return false;
      std::vector<T> lut(lut_size);
      size_t lut_bytes;
      if (!safe::mul(size_t(lut_size), sizeof(T), &lut_bytes)) return false;
      if (!reader()->read(lut.data(), lut_bytes)) return false;
      std::vector<uint32_t> idxs(n);
      if (!read_compressed_u32_n(idxs.data(), n)) return false;
      for (size_t i = 0; i < n; ++i) {
        if (idxs[i] >= lut_size) return false;
        dst[i] = lut[idxs[i]];
      }
      return true;
    }
    AddWarning("Unknown compressed floating-point array code");
    return false;
  };
  auto read_compressed_floating = [&](auto* dst) -> bool {
    return read_compressed_floating_n(dst, static_cast<size_t>(count));
  };

  // Decode a pxrUSD compressed scalar half array into raw half (uint16) values.
  // Same [i8 code] framing as the floating decoder, but code 'i' reconstructs
  // each value as GfHalf(int) and the 't' lookup table holds 2-byte halfs.
  auto read_compressed_half_n = [&](uint16_t* dst, size_t n) -> bool {
    int8_t code = 0;
    if (!reader()->read_i8(code)) return false;
    if (code == 'i') {
      std::vector<uint32_t> ints(n);
      if (!read_compressed_u32_n(ints.data(), n)) return false;
      for (size_t i = 0; i < n; ++i) {
        dst[i] = FloatToHalf(static_cast<float>(static_cast<int32_t>(ints[i])));
      }
      return true;
    }
    if (code == 't') {
      uint32_t lut_size = 0;
      if (!reader()->read_u32(lut_size)) return false;
      if (lut_size == 0 || lut_size > options_.max_array_elements) return false;
      if (!reader()->has_elements(size_t(lut_size), sizeof(uint16_t))) return false;
      std::vector<uint16_t> lut(lut_size);
      size_t lut_bytes;
      if (!safe::mul(size_t(lut_size), sizeof(uint16_t), &lut_bytes)) return false;
      if (!reader()->read(lut.data(), lut_bytes)) return false;
      std::vector<uint32_t> idxs(n);
      if (!read_compressed_u32_n(idxs.data(), n)) return false;
      for (size_t i = 0; i < n; ++i) {
        if (idxs[i] >= lut_size) return false;
        dst[i] = lut[idxs[i]];
      }
      return true;
    }
    AddWarning("Unknown compressed half-precision array code");
    return false;
  };
  switch (type_id) {
    case CrateTypeId::Float: {
      std::vector<float> data(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_floating(data.data())) return false;
      } else if (!read_raw(data.data(), sizeof(float))) {
        return false;
      }
      out = Value::MakeFloatArray(std::move(data));
      return true;
    }
    case CrateTypeId::Int: {
      std::vector<int32_t> data(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u32(reinterpret_cast<uint32_t*>(data.data()))) return false;
      } else if (!read_raw(data.data(), sizeof(int32_t))) {
        return false;
      }
      out = Value::MakeIntArray(std::move(data));
      return true;
    }
    case CrateTypeId::Vec2f: {
      size_t data_size;
      if (!safe::mul(static_cast<size_t>(count), size_t(2), &data_size)) {
        return false;
      }
      std::vector<float> data(data_size);
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), data_size)) return false;
      } else if (!read_raw(data.data(), 2 * sizeof(float))) {
        return false;
      }
      out = Value::MakeFloat2Array(std::move(data));
      return true;
    }
    case CrateTypeId::Vec3f: {
      size_t data_size;
      if (!safe::mul(static_cast<size_t>(count), size_t(3), &data_size)) {
        return false;
      }
      std::vector<float> data(data_size);
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), data_size)) return false;
      } else if (!read_raw(data.data(), 3 * sizeof(float))) {
        return false;
      }
      out = Value::MakeFloat3Array(std::move(data));
      return true;
    }
    case CrateTypeId::Double: {
      std::vector<double> data(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_floating(data.data())) return false;
      } else if (!read_raw(data.data(), sizeof(double))) {
        return false;
      }
      out = Value::MakeDoubleArray(std::move(data));
      return true;
    }
    case CrateTypeId::Int64: {
      std::vector<int64_t> data(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u64(reinterpret_cast<uint64_t*>(data.data()))) return false;
      } else if (!read_raw(data.data(), sizeof(int64_t))) {
        return false;
      }
      out = Value::MakeInt64Array(std::move(data));
      return true;
    }
    case CrateTypeId::UInt: {
      std::vector<uint32_t> data(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u32(data.data())) return false;
      } else if (!read_raw(data.data(), sizeof(uint32_t))) {
        return false;
      }
      out = Value::MakeUIntArray(std::move(data));
      return true;
    }
    case CrateTypeId::UChar: {
      // uchar[]: tightly packed uint8 on disk (pxr never int-compresses
      // 8-bit arrays); widened into the uint32 array storage in memory.
      // Previously these were dropped as unsupported.
      std::vector<uint32_t> data(static_cast<size_t>(count));
      for (size_t i = 0; i < count; ++i) {
        uint8_t b;
        if (!reader()->read(&b, 1)) return false;
        data[i] = static_cast<uint32_t>(b);
      }
      out = Value::MakeUIntCompArray(std::move(data), TypeId::UChar, 1);
      return true;
    }
    case CrateTypeId::UInt64: {
      std::vector<uint64_t> data(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u64(data.data())) return false;
      } else if (!read_raw(data.data(), sizeof(uint64_t))) {
        return false;
      }
      out = Value::MakeUInt64Array(std::move(data));
      return true;
    }
    case CrateTypeId::Bool: {
      std::vector<bool> out_bool(static_cast<size_t>(count));
      if (compressed) {
        // Bool participates in Crate's compressed-integral array encoding.
        // Decode the integer lanes first, then canonicalize every non-zero
        // value to true in the in-memory byte representation.
        std::vector<uint32_t> lanes(static_cast<size_t>(count));
        if (!read_compressed_u32(lanes.data())) return false;
        for (size_t i = 0; i < lanes.size(); ++i) {
          out_bool[i] = (lanes[i] != 0);
        }
      } else {
        for (size_t i = 0; i < count; ++i) {
          uint8_t b;
          if (!reader()->read(&b, 1)) return false;
          out_bool[i] = (b != 0);
        }
      }
      out = Value::MakeBoolArray(std::move(out_bool));
      return true;
    }
    case CrateTypeId::Token: {
      std::vector<uint32_t> idxs(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u32(idxs.data())) return false;
      } else if (!read_raw(idxs.data(), sizeof(uint32_t))) {
        return false;
      }
      std::vector<std::string> data(static_cast<size_t>(count));
      for (size_t i = 0; i < count; i++) {
        if (idxs[i] >= tokens_.size()) return false;
        data[i] = tokens_.str(idxs[i]);
      }
      out = Value::MakeTokenArray(std::move(data));
      return true;
    }
    // Vector / quaternion / matrix arrays stored as flat float or double
    // buffers (uncompressed in crate). Decoded generically via the element
    // stride so each type doesn't need a near-identical case.
    case CrateTypeId::Quatf: {
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(4), &scalars)) return false;
      std::vector<float> data(scalars);
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), scalars)) return false;
      } else if (!read_raw(data.data(), 16)) {
        return false;
      }
      // Disk is imaginary-first per element; internal is real-first.
      for (size_t e = 0; e < count; ++e) {
        float* q = data.data() + e * 4;
        const float w = q[3];
        q[3] = q[2]; q[2] = q[1]; q[1] = q[0]; q[0] = w;
      }
      out = Value::MakeFloatCompArray(std::move(data),
                                      CrateArrayValueType(type_id), 4);
      return true;
    }
    case CrateTypeId::Vec4f: {
      const uint32_t stride_bytes = CrateArrayElemStride(type_id);  // e.g. 16
      const uint32_t comps = stride_bytes / 4;
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(comps), &scalars)) return false;
      std::vector<float> data(scalars);
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), scalars)) return false;
      } else if (!read_raw(data.data(), stride_bytes)) {
        return false;
      }
      out = Value::MakeFloatCompArray(std::move(data),
                                      CrateArrayValueType(type_id), comps);
      return true;
    }
    case CrateTypeId::Quatd: {
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(4), &scalars)) return false;
      std::vector<double> data(scalars);
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), scalars)) return false;
      } else if (!read_raw(data.data(), 32)) {
        return false;
      }
      for (size_t e = 0; e < count; ++e) {
        double* q = data.data() + e * 4;
        const double w = q[3];
        q[3] = q[2]; q[2] = q[1]; q[1] = q[0]; q[0] = w;
      }
      out = Value::MakeDoubleCompArray(std::move(data),
                                       CrateArrayValueType(type_id), 4);
      return true;
    }
    case CrateTypeId::TimeCode: {
      std::vector<double> data(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_floating(data.data())) return false;
      } else if (!read_raw(data.data(), sizeof(double))) {
        return false;
      }
      out = Value::MakeDoubleCompArray(std::move(data), TypeId::TimeCode, 1);
      return true;
    }
    case CrateTypeId::Vec2i:
    case CrateTypeId::Vec3i:
    case CrateTypeId::Vec4i: {
      const uint32_t stride_bytes = CrateArrayElemStride(type_id);
      const uint32_t comps = stride_bytes / 4;
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(comps), &scalars)) return false;
      std::vector<int32_t> data(scalars);
      if (compressed) {
        if (!read_compressed_u32_n(reinterpret_cast<uint32_t*>(data.data()),
                                   scalars)) return false;
      } else if (!read_raw(data.data(), stride_bytes)) {
        return false;
      }
      out = Value::MakeIntCompArray(std::move(data),
                                    CrateArrayValueType(type_id), comps);
      return true;
    }
    case CrateTypeId::String:
    case CrateTypeId::PathExpression: {
      // uint32 indices into the STRINGS section. SdfPathExpression arrays
      // (crate >= 0.10) store each expression's text the same way.
      std::vector<uint32_t> idxs(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u32(idxs.data())) return false;
      } else if (!read_raw(idxs.data(), sizeof(uint32_t))) {
        return false;
      }
      std::vector<std::string> data(static_cast<size_t>(count));
      for (size_t i = 0; i < count; i++) {
        if (!GetString(idxs[i], data[i])) return false;
      }
      out = Value::MakeStringLikeArray(
          std::move(data), type_id == CrateTypeId::PathExpression
                               ? TypeId::PathExpression
                               : TypeId::String);
      return true;
    }
    case CrateTypeId::AssetPath: {
      // uint32 STRING indices (unlike the scalar form, which uses a token
      // index) — matches pxr's VtArray<SdfAssetPath> layout.
      std::vector<uint32_t> idxs(static_cast<size_t>(count));
      if (compressed) {
        if (!read_compressed_u32(idxs.data())) return false;
      } else if (!read_raw(idxs.data(), sizeof(uint32_t))) {
        return false;
      }
      std::vector<std::string> data(static_cast<size_t>(count));
      for (size_t i = 0; i < count; i++) {
        if (!GetString(idxs[i], data[i])) return false;
      }
      out = Value::MakeStringLikeArray(std::move(data), TypeId::AssetPath);
      return true;
    }
    case CrateTypeId::Vec2d:
    case CrateTypeId::Vec3d:
    case CrateTypeId::Vec4d:
    case CrateTypeId::Matrix2d:
    case CrateTypeId::Matrix3d:
    case CrateTypeId::Matrix4d: {
      const uint32_t stride_bytes = CrateArrayElemStride(type_id);  // e.g. 128
      const uint32_t comps = stride_bytes / 8;
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(comps), &scalars)) return false;
      std::vector<double> data(scalars);
      if (compressed) {
        if (!read_compressed_floating_n(data.data(), scalars)) return false;
      } else if (!read_raw(data.data(), stride_bytes)) {
        return false;
      }
      out = Value::MakeDoubleCompArray(std::move(data),
                                       CrateArrayValueType(type_id), comps);
      return true;
    }
    case CrateTypeId::Quath: {
      size_t scalars;
      if (!safe::mul(static_cast<size_t>(count), size_t(4), &scalars)) return false;
      std::vector<float> data(scalars);
      if (compressed) {
        if (!read_compressed_half_n(reinterpret_cast<uint16_t*>(data.data()), scalars)) return false;
      } else if (!read_raw(data.data(), 8)) {
        return false;
      }
      uint16_t* halfs = reinterpret_cast<uint16_t*>(data.data());
      for (size_t e = 0; e < count; ++e) {
        const uint16_t* q = halfs + e * 4;
        data[e * 4 + 0] = HalfToFloat(q[3]);
        data[e * 4 + 1] = HalfToFloat(q[0]);
        data[e * 4 + 2] = HalfToFloat(q[1]);
        data[e * 4 + 3] = HalfToFloat(q[2]);
      }
      out = Value::MakeFloatCompArray(std::move(data),
                                      CrateArrayValueType(type_id), 4);
      return true;
    }
    case CrateTypeId::Half:
    case CrateTypeId::Vec2h:
    case CrateTypeId::Vec3h:
    case CrateTypeId::Vec4h: {
      const uint32_t comps = CrateArrayElemStride(type_id) / 2;  // 2 bytes/half
      size_t scalars;
      if (comps == 0 || !safe::mul(static_cast<size_t>(count), size_t(comps), &scalars)) return false;
      std::vector<float> data(scalars);
      if (compressed) {
        if (!read_compressed_half_n(reinterpret_cast<uint16_t*>(data.data()), scalars)) return false;
      } else if (!read_raw(data.data(), comps * 2)) {
        return false;
      }
      uint16_t* halfs = reinterpret_cast<uint16_t*>(data.data());
      for (size_t i = scalars; i > 0; --i) {
        data[i - 1] = HalfToFloat(halfs[i - 1]);
      }
      out = Value::MakeFloatCompArray(std::move(data),
                                      CrateArrayValueType(type_id), comps);
      return true;
    }
    default:
      AddWarning(std::string("Unsupported array type: ") + CrateTypeIdName(type_id));
      return false;
  }
}

}  // namespace next
}  // namespace lightusd
