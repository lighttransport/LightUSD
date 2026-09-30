// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate Format Definitions
// Binary format structures for USDC files

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../strfmt.hh"

namespace lightusd {
namespace next {

// ============================================================
// IEEE 754 half (float16) <-> float32 conversion
// ============================================================
//
// Crate stores half/half2/half3/half4/quath as packed uint16 components. The
// next Value model has no 16-bit storage, so half arrays materialize into a
// float buffer (widening is exact) and re-encode by narrowing back to half
// (round-to-nearest-even). Half-origin values round-trip byte-exact: a float
// widened from a half has zero low mantissa bits, so narrowing reproduces it.

inline float HalfToFloat(uint16_t h) {
  uint32_t sign = static_cast<uint32_t>(h >> 15) & 1u;
  uint32_t exp = static_cast<uint32_t>(h >> 10) & 0x1Fu;
  uint32_t mant = static_cast<uint32_t>(h) & 0x3FFu;
  uint32_t f;
  if (exp == 0) {
    if (mant == 0) {
      f = sign << 31;  // +/- zero
    } else {
      // Subnormal half -> normalized float.
      exp = 1;
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3FFu;
      f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
  } else if (exp == 0x1Fu) {
    f = (sign << 31) | (0xFFu << 23) | (mant << 13);  // inf / nan
  } else {
    f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &f, sizeof(out));
  return out;
}

inline uint16_t FloatToHalf(float value) {
  uint32_t f;
  std::memcpy(&f, &value, sizeof(f));
  uint16_t sign = static_cast<uint16_t>((f >> 16) & 0x8000u);
  uint32_t fexp = (f >> 23) & 0xFFu;
  uint32_t mant = f & 0x7FFFFFu;

  if (fexp == 0xFFu) {  // inf / nan
    return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
  }
  int32_t exp = static_cast<int32_t>(fexp) - 127 + 15;
  if (exp >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00u);  // overflow -> inf
  }
  if (exp <= 0) {
    if (exp < -10) return sign;  // underflow -> zero
    mant |= 0x800000u;
    uint32_t shift = static_cast<uint32_t>(14 - exp);
    uint32_t hm = mant >> shift;
    uint32_t rem = mant & ((1u << shift) - 1u);
    uint32_t half = 1u << (shift - 1);
    if (rem > half || (rem == half && (hm & 1u))) hm++;  // round to nearest even
    return static_cast<uint16_t>(sign | hm);
  }
  uint16_t h = static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                     (mant >> 13));
  uint32_t rem = mant & 0x1FFFu;
  if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++;  // round to nearest even
  return h;
}

// ============================================================
// Crate file constants
// ============================================================

/// Magic number at start of USDC files
constexpr char kCrateMagic[8] = {'P', 'X', 'R', '-', 'U', 'S', 'D', 'C'};

/// Minimum supported version
constexpr uint8_t kCrateMinVersionMajor = 0;
constexpr uint8_t kCrateMinVersionMinor = 4;
constexpr uint8_t kCrateMinVersionPatch = 0;

/// Maximum supported version.
/// Versions past 0.9.0 only ADD value types / fields over the structurally
/// stable section layout: 0.10 timecode, 0.11 relocates, 0.12/0.13 splines,
/// 0.14 array edits. Unknown type ids / fields surface as per-value warnings
/// rather than wholesale rejection, so accept the whole window (legacy reads
/// through 0.14 too; pxr 25.x writes 0.11+ when those features are authored).
constexpr uint8_t kCrateMaxVersionMajor = 0;
constexpr uint8_t kCrateMaxVersionMinor = 14;
constexpr uint8_t kCrateMaxVersionPatch = 0;

/// Bootstrap header size
constexpr size_t kCrateBootstrapSize = 88;

// ============================================================
// Crate data types (matches pxrUSD CrateDataTypeId)
// ============================================================

/// Type IDs used in binary format (8-bit value in ValueRep)
enum class CrateTypeId : uint8_t {
  Invalid = 0,

  // Scalar types
  Bool = 1,
  UChar = 2,
  Int = 3,
  UInt = 4,
  Int64 = 5,
  UInt64 = 6,
  Half = 7,
  Float = 8,
  Double = 9,

  // String types
  String = 10,
  Token = 11,
  AssetPath = 12,

  // Matrix types
  Matrix2d = 13,
  Matrix3d = 14,
  Matrix4d = 15,

  // Quaternion types
  Quatd = 16,
  Quatf = 17,
  Quath = 18,

  // Vector types (2-component)
  Vec2d = 19,
  Vec2f = 20,
  Vec2h = 21,
  Vec2i = 22,

  // Vector types (3-component)
  Vec3d = 23,
  Vec3f = 24,
  Vec3h = 25,
  Vec3i = 26,

  // Vector types (4-component)
  Vec4d = 27,
  Vec4f = 28,
  Vec4h = 29,
  Vec4i = 30,

  // Special types
  Dictionary = 31,
  TokenListOp = 32,
  StringListOp = 33,
  PathListOp = 34,
  ReferenceListOp = 35,
  IntListOp = 36,
  Int64ListOp = 37,
  UIntListOp = 38,
  UInt64ListOp = 39,

  PathVector = 40,
  TokenVector = 41,

  Specifier = 42,
  Permission = 43,
  Variability = 44,

  VariantSelectionMap = 45,
  TimeSamples = 46,
  Payload = 47,
  DoubleVector = 48,
  LayerOffsetVector = 49,
  StringVector = 50,
  ValueBlock = 51,
  Value = 52,
  UnregisteredValue = 53,
  UnregisteredValueListOp = 54,
  PayloadListOp = 55,
  TimeCode = 56,
  PathExpression = 57,  // SdfPathExpression (crate >= 0.10.0)
  Relocates = 58,       // SdfRelocates (crate >= 0.11.0):
                        //   [u64 count][(u32 srcPathIdx, u32 dstPathIdx)*]
  Spline = 59,          // TsSpline (crate >= 0.12.0)
  // 60 = AnimationBlock (SdfAnimationBlock) -- reserved, not yet implemented
};

/// Convert CrateTypeId to string name
const char* CrateTypeIdName(CrateTypeId id);

/// Get the base element size for a crate type (0 for variable-size types)
size_t CrateTypeIdSize(CrateTypeId id);

// ============================================================
// ValueRep - 64-bit value representation
// ============================================================

/// Represents a value in the binary format
/// Layout:
///   Bit 63: IsArray (1 = array, 0 = scalar)
///   Bit 62: IsInlined (1 = value in payload, 0 = offset to data)
///   Bit 61: IsCompressed (1 = compressed data)
///   Bits 55-48: Type ID (CrateTypeId)
///   Bits 47-0: Payload (value or file offset)
class ValueRep {
public:
  ValueRep() : data_(0) {}
  explicit ValueRep(uint64_t raw) : data_(raw) {}

  /// Get raw data
  uint64_t raw() const { return data_; }

  /// Check flags
  bool is_array() const { return (data_ >> 63) & 1; }
  bool is_inlined() const { return (data_ >> 62) & 1; }
  bool is_compressed() const { return (data_ >> 61) & 1; }
  /// VtArrayEdit flag (crate >= 0.14). The type byte holds the ELEMENT type
  /// and is_array() is false, so an unhandled array-edit rep looks like a
  /// plain scalar whose payload points at the edit tuple — it must be
  /// detected explicitly or it decodes to garbage.
  bool is_array_edit() const { return (data_ >> 60) & 1; }

  /// Get type ID
  CrateTypeId type_id() const {
    return static_cast<CrateTypeId>((data_ >> 48) & 0xFF);
  }

  /// Get payload (48 bits)
  uint64_t payload() const { return data_ & 0xFFFFFFFFFFFFULL; }

  /// Get payload as signed offset
  int64_t payload_as_offset() const {
    // Sign-extend 48-bit value
    uint64_t val = payload();
    if (val & 0x800000000000ULL) {
      return static_cast<int64_t>(val | 0xFFFF000000000000ULL);
    }
    return static_cast<int64_t>(val);
  }

  /// Create from components
  static ValueRep Make(CrateTypeId type, uint64_t payload,
                       bool is_array = false, bool is_inlined = false,
                       bool is_compressed = false,
                       bool is_array_edit = false) {
    uint64_t data = (payload & 0xFFFFFFFFFFFFULL) |
                    (static_cast<uint64_t>(type) << 48) |
                    (static_cast<uint64_t>(is_array_edit) << 60) |
                    (static_cast<uint64_t>(is_compressed) << 61) |
                    (static_cast<uint64_t>(is_inlined) << 62) |
                    (static_cast<uint64_t>(is_array) << 63);
    return ValueRep(data);
  }

private:
  uint64_t data_;
};

// ============================================================
// Section and TOC structures
// ============================================================

/// Section entry in the table of contents
struct CrateSection {
  char name[16];      // Null-terminated section name
  int64_t start;      // File offset
  int64_t size;       // Section size in bytes

  std::string name_str() const {
    return std::string(name, strnlen(name, 15));
  }
};

/// Table of contents
struct CrateTOC {
  std::vector<CrateSection> sections;

  /// Find section by name
  const CrateSection* find(const char* name) const {
    for (const auto& s : sections) {
      if (strncmp(s.name, name, 15) == 0) {
        return &s;
      }
    }
    return nullptr;
  }
};

/// File version
struct CrateVersion {
  uint8_t major;
  uint8_t minor;
  uint8_t patch;

  bool is_valid() const {
    // Check minimum version.
    // Compare as int: major is uint8_t, so `major < 0` is always false and
    // trips -Wtautological-unsigned-zero-compare under -Weverything. The cast
    // keeps the check meaningful if kCrateMinVersionMajor ever rises above 0.
    if (static_cast<int>(major) < kCrateMinVersionMajor) return false;
    if (major == kCrateMinVersionMajor) {
      if (minor < kCrateMinVersionMinor) return false;
      if (minor == kCrateMinVersionMinor &&
          static_cast<int>(patch) < kCrateMinVersionPatch)
        return false;
    }
    // Check maximum version
    if (major > kCrateMaxVersionMajor) return false;
    if (major == kCrateMaxVersionMajor) {
      if (minor > kCrateMaxVersionMinor) return false;
    }
    return true;
  }

  std::string to_string() const {
    return UIntToStr(major) + "." + UIntToStr(minor) + "." + UIntToStr(patch);
  }

  /// Crate files older than 0.7.0 store array element counts as uint32 (pxr
  /// crateFile.cpp _Write/_ReadUncompressedArray:
  /// `(ver < CrateFile::Version(0,7,0)) ? Read<uint32_t>() : Read<uint64_t>()`).
  bool is_pre_070() const { return major == 0 && minor < 7; }
};

// ============================================================
// Index types (for type safety)
// ============================================================

/// Token index
struct TokenIndex {
  uint32_t value = 0;
  bool is_valid() const { return value != 0xFFFFFFFF; }
};

/// String index
struct StringIndex {
  uint32_t value = 0;
  bool is_valid() const { return value != 0xFFFFFFFF; }
};

/// Path index
struct PathIndex {
  uint32_t value = 0;
  bool is_valid() const { return value != 0xFFFFFFFF; }
};

/// Fieldset index
struct FieldsetIndex {
  uint32_t value = 0;
  bool is_valid() const { return value != 0xFFFFFFFF; }
};

// ============================================================
// Field and Spec structures
// ============================================================

/// Field definition (property name + value)
struct CrateField {
  TokenIndex token_index;   // Index into tokens table
  ValueRep value_rep;       // Value representation
};

/// Spec types
enum class SpecType : uint32_t {
  Unknown = 0,
  Attribute = 1,
  Connection = 2,
  Expression = 3,
  Mapper = 4,
  MapperArg = 5,
  Prim = 6,
  PseudoRoot = 7,
  Relationship = 8,
  RelationshipTarget = 9,
  Variant = 10,
  VariantSet = 11,
};

/// Map a raw file-supplied uint32 onto SpecType. Values outside the
/// enumerator range become Unknown rather than an out-of-range enum that any
/// downstream table lookup or exhaustive switch would treat as live.
inline SpecType ToSpecType(uint32_t v) {
  return (v <= static_cast<uint32_t>(SpecType::VariantSet))
             ? static_cast<SpecType>(v)
             : SpecType::Unknown;
}

/// Spec definition (prim or property)
struct CrateSpec {
  PathIndex path_index;       // Index into paths table
  FieldsetIndex fieldset_index;  // Index into fieldsets table
  SpecType spec_type;
};

// ============================================================
// Compression helpers
// ============================================================

/// LZ4 decompression result
struct DecompressResult {
  bool success = false;
  std::vector<uint8_t> data;
  std::string error;
};

/// Decompress LZ4 data
DecompressResult DecompressLZ4(const uint8_t* src, size_t src_size,
                                size_t uncompressed_size);

/// Decompress USD integer-encoded data
DecompressResult DecompressIntegers(const uint8_t* src, size_t src_size,
                                     size_t num_integers, bool is_64bit = false);

/// LZ4 compression result
struct CompressResult {
  bool success = false;
  std::vector<uint8_t> data;
  std::string error;
};

/// Compress data with LZ4
CompressResult CompressLZ4(const uint8_t* src, size_t src_size);

/// Encode integers using USD's integer compression (inverse of DecompressIntegers)
/// Returns the compressed byte representation.
std::vector<uint8_t> EncodeIntegers(const uint32_t* values, size_t count);

/// LZ4 compression/decompression with USD's TfFastCompression wrapper.
///
/// pxrUSD wraps raw LZ4 block data with a n_chunks prefix byte:
///   - n_chunks == 0: single LZ4 block follows
///   - n_chunks > 0: n_chunks blocks, each preceded by a i32 chunk_size
///
/// This is used for TOKENS and FIELDS structural sections.
///
/// Compress with CompressCrateBlob (adds prefix).
/// Decompress with DecompressCrateBlob (strips prefix).
CompressResult CompressCrateBlob(const uint8_t* src, size_t src_size);
DecompressResult DecompressCrateBlob(const uint8_t* src, size_t src_size,
                                     size_t uncompressed_size);
DecompressResult DecompressCrateBlobWithCapacityHint(
    const uint8_t* src, size_t src_size, size_t uncompressed_size_limit,
    size_t initial_capacity);

// ============================================================
// pxrUSD delta-coded integer compression
// ============================================================
///
/// pxrUSD's integer coding uses delta encoding with a common delta value
/// and 2-bit-per-value code map, followed by variable-length integer data.
///
/// For u32: delta = i32, code 1 = int8_t, code 2 = int16_t, code 3 = int32_t
/// For i32: delta = i32, code 1 = int16_t, code 2 = int32_t, code 3 = int64_t

/// Encode uint32_t values using pxrUSD delta-coding format.
/// Returns the raw (uncompressed) delta-coded byte sequence.
std::vector<uint8_t> EncodeDeltaU32(const uint32_t* values, size_t count);

/// Decode uint32_t values from pxrUSD delta-coded format.
/// buffer points to the raw (already decompressed) delta-coded data.
bool DecodeDeltaU32(const uint8_t* buffer, size_t buffer_size,
                    uint32_t* dst, size_t count);

/// Encode int32_t values using pxrUSD delta-coding format.
std::vector<uint8_t> EncodeDeltaS32(const int32_t* values, size_t count);

/// Decode int32_t values from pxrUSD delta-coded format.
bool DecodeDeltaS32(const uint8_t* buffer, size_t buffer_size,
                    int32_t* dst, size_t count);

// ============================================================
// pxrUSD compressed integer section I/O
// ============================================================
///
/// These wrap delta-coded integers with n_chunks-prefixed LZ4 compression
/// and a u64 size prefix, as used by pxrUSD's read_compressed_ints.

/// Serialize (delta-encode + LZ4-compress + write u64 size + data).
CompressResult WriteCompressedU32(const uint32_t* values, size_t count);

/// Deserialize (read u64 size + LZ4-decompress + delta-decode).
DecompressResult DecompressCompressedU32(const uint8_t* data, size_t data_size,
                                         uint32_t* dst, size_t count);

/// Decode uint64_t values from pxrUSD delta-coded format (64-bit variant of
/// DecodeDeltaU32: int64 common value, int16/int32/int64 code widths).
bool DecodeDeltaU64(const uint8_t* buffer, size_t buffer_size,
                    uint64_t* dst, size_t count);

/// Deserialize a pxrUSD compressed 64-bit integer section
/// (read u64 size + LZ4-decompress + 64-bit delta-decode).
DecompressResult DecompressCompressedU64(const uint8_t* data, size_t data_size,
                                         uint64_t* dst, size_t count);

}  // namespace next
}  // namespace lightusd
