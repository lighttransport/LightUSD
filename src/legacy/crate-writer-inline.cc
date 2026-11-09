// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// CrateWriter::TryInlineValue - split out of crate-writer-values.cc.
// Decides whether a CrateValue can be inlined directly into its ValueRep (small
// PODs, string/token/asset indices, empty arrays) rather than written out-of-line.
// Self-contained: its own CANNOT_INLINE / TRY_INLINE_EMPTY_ARRAY macros, no
// dependency on WriteValueData/ConvertValueToCrateValue. Isolated so the ~1450-line
// WriteValueData TU compiles on its own (faster single-TU rebuilds).
#include "crate-writer.hh"

#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>

// math::is_close — used for exact (eps == 0) floating-point comparison without
// tripping -Wfloat-equal. is_close(a, b, 0) computes fabs(a - b) <= 0, which is
// bit-exact equality for finite values; a non-zero eps would be WRONG here (it
// would inline values that are merely close, losing precision on write).
#include "math-util.inc"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wexceptions"
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif

namespace lightusd {
namespace experimental {

namespace {

template <typename T>
bool EncodeInt8Exact(T v, int8_t *out) {
  const T lo = static_cast<T>(std::numeric_limits<int8_t>::lowest());
  const T hi = static_cast<T>(std::numeric_limits<int8_t>::max());
  if (!(lo <= v && v <= hi)) {
    return false;
  }
  int8_t i = static_cast<int8_t>(v);
  // The int8 round-trip must be exact (lossless inline). For floating-point T
  // use math::is_close with eps 0 (bit-exact, no -Wfloat-equal); integer T uses
  // a plain integer compare (no float warning, exact by construction).
  if constexpr (std::is_floating_point_v<T>) {
    if (!math::is_close(static_cast<T>(i), v, static_cast<T>(0))) {
      return false;
    }
  } else {
    if (static_cast<T>(i) != v) {
      return false;
    }
  }
  *out = i;
  return true;
}

template <typename Vec, size_t N>
bool EncodeVecInlineInt8(const Vec& vec, uint32_t *packed) {
  int8_t ivec[N];
  for (size_t i = 0; i < N; ++i) {
    if (!EncodeInt8Exact(vec[i], &ivec[i])) {
      return false;
    }
  }
  *packed = 0;
  memcpy(packed, ivec, sizeof(ivec));
  return true;
}

template <typename Vec, size_t N>
bool TryInlineVecInt8(const Vec& vec, crate::CrateDataTypeId type_id,
                      crate::ValueRep* rep) {
  uint32_t packed = 0;
  if (!EncodeVecInlineInt8<Vec, N>(vec, &packed)) {
    return false;
  }
  rep->SetType(static_cast<int32_t>(type_id));
  rep->SetIsInlined();
  rep->SetPayload(static_cast<uint64_t>(packed));
  return true;
}

template <typename Vec, size_t N>
bool EncodeHalfVecInlineInt8(const Vec& vec, uint32_t *packed) {
  int8_t ivec[N];
  for (size_t i = 0; i < N; ++i) {
    const float f = value::half_to_float(vec[i]);
    if (!EncodeInt8Exact(f, &ivec[i])) {
      return false;
    }
  }
  *packed = 0;
  memcpy(packed, ivec, sizeof(ivec));
  return true;
}

template <typename Vec, size_t N>
bool TryInlineHalfVecInt8(const Vec& vec, crate::CrateDataTypeId type_id,
                          crate::ValueRep* rep) {
  uint32_t packed = 0;
  if (!EncodeHalfVecInlineInt8<Vec, N>(vec, &packed)) {
    return false;
  }
  rep->SetType(static_cast<int32_t>(type_id));
  rep->SetIsInlined();
  rep->SetPayload(static_cast<uint64_t>(packed));
  return true;
}

template <typename Matrix, size_t N>
bool EncodeMatrixInlineDiagonalInt8(const Matrix& mat, uint32_t *packed) {
  int8_t diag[N];
  for (size_t r = 0; r < N; ++r) {
    for (size_t c = 0; c < N; ++c) {
      if (r == c) {
        if (!EncodeInt8Exact(mat.m[r][c], &diag[r])) {
          return false;
        }
      } else if (!math::is_close(mat.m[r][c], 0.0, 0.0)) {
        // Off-diagonal must be exactly 0 for the diagonal-only inline encoding.
        return false;
      }
    }
  }
  *packed = 0;
  memcpy(packed, diag, sizeof(diag));
  return true;
}

template <typename Matrix, size_t N>
bool TryInlineMatrixDiagonalInt8(const Matrix& mat,
                                 crate::CrateDataTypeId type_id,
                                 crate::ValueRep* rep) {
  uint32_t packed = 0;
  if (!EncodeMatrixInlineDiagonalInt8<Matrix, N>(mat, &packed)) {
    return false;
  }
  rep->SetType(static_cast<int32_t>(type_id));
  rep->SetIsInlined();
  rep->SetPayload(static_cast<uint64_t>(packed));
  return true;
}

}  // namespace

bool CrateWriter::TryInlineValue(const crate::CrateValue& value, crate::ValueRep* rep) {
  // Direct-interning sink: historical behavior (intern into writer tables).
  struct DirectSink final : public InternSink {
    CrateWriter* w;
    explicit DirectSink(CrateWriter* writer) : w(writer) {}
    uint32_t InternToken(const std::string& s) override {
      return w->GetOrCreateToken(s).value;
    }
    uint32_t InternString(const std::string& s) override {
      return w->GetOrCreateString(s).value;
    }
  } sink(this);
  return TryInlineValue(value, rep, sink);
}

bool CrateWriter::TryInlineValue(const crate::CrateValue& value,
                                 crate::ValueRep* rep, InternSink& sink) {
  // Phase 1: String/Token/AssetPath values
  // Strings and tokens are always inlined as indices in USDC format
  //
  // NOTE: interning MUST go through `sink` — see the InternSink contract in
  // crate-writer.hh (a recording sink runs this concurrently in pass A of the
  // two-pass Finalize; any other writer-state mutation here breaks it).

  // Try to get as token
  if (auto* token_val = value.as<value::token>()) {
    const uint32_t idx = sink.InternToken(token_val->str());
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_TOKEN));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(idx));
    return true;
  }

  // Try to get as string
  if (auto* str_val = value.as<std::string>()) {
    const uint32_t idx = sink.InternString(*str_val);
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_STRING));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(idx));
    return true;
  }

  // Try to get as AssetPath
  if (auto* asset_val = value.as<value::AssetPath>()) {
    // Inlined AssetPath is stored as a TokenIndex per OpenUSD's crate
    // format (matches CrateReader at crate-reader-values.cc which reads
    // it via GetToken). Storing as StringIndex caused the reader to read
    // a token at the wrong index and surface garbage like ";-)".
    const uint32_t idx = sink.InternToken(asset_val->GetAssetPath());
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_ASSET_PATH));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(idx));
    return true;
  }

  // NOTE: SdfPathExpression is intentionally NOT inlined. OpenUSD has no
  // GetUninlinedValue<SdfPathExpression> specialization, so it cannot decode an
  // inlined PathExpression (it would read the payload bits as a struct -> empty
  // expression). OpenUSD always stores it non-inlined as a StringIndex at an
  // offset; we match that in PackValue/PackValueData.

  // Basic scalar types

  // Try to get as int32
  if (auto* int_val = value.as<int32_t>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_INT));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(*int_val));
    return true;
  }

  // Try to get as uint32
  if (auto* uint_val = value.as<uint32_t>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_UINT));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(*uint_val));
    return true;
  }

  // Try to get as Specifier enum
  if (auto* spec_val = value.as<Specifier>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_SPECIFIER));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(*spec_val));
    return true;
  }

  // Try to get as Permission enum
  if (auto* perm_val = value.as<Permission>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_PERMISSION));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(*perm_val));
    return true;
  }

  // Try to get as Variability enum
  if (auto* var_val = value.as<Variability>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_VARIABILITY));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(*var_val));
    return true;
  }

  // OpenUSD inlines int64/uint64 only when exactly representable by signed or
  // unsigned 32-bit integer payloads, respectively.
  if (auto* int64_val = value.as<int64_t>()) {
    if (*int64_val >= int64_t((std::numeric_limits<int32_t>::min)()) &&
        *int64_val <= int64_t((std::numeric_limits<int32_t>::max)())) {
      int32_t rep32 = static_cast<int32_t>(*int64_val);
      uint32_t packed = 0;
      memcpy(&packed, &rep32, sizeof(rep32));
      rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_INT64));
      rep->SetIsInlined();
      rep->SetPayload(static_cast<uint64_t>(packed));
      return true;
    }
    return false;
  }

  if (auto* uint64_val = value.as<uint64_t>()) {
    if (*uint64_val <= uint64_t((std::numeric_limits<uint32_t>::max)())) {
      uint32_t rep32 = static_cast<uint32_t>(*uint64_val);
      rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_UINT64));
      rep->SetIsInlined();
      rep->SetPayload(static_cast<uint64_t>(rep32));
      return true;
    }
    return false;
  }

  // Try to get as float
  if (auto* float_val = value.as<float>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_FLOAT));
    rep->SetIsInlined();
    // Copy float bits to uint32, then to uint64
    uint32_t float_bits;
    memcpy(&float_bits, float_val, sizeof(float));
    rep->SetPayload(static_cast<uint64_t>(float_bits));
    return true;
  }

  // OpenUSD inlines doubles that are exactly representable as float by storing
  // the float bits in the 32-bit payload.
  if (auto* double_val = value.as<double>()) {
    float f = static_cast<float>(*double_val);
    // Inline only doubles that round-trip through float EXACTLY (no precision
    // loss), matching OpenUSD. Compare the float-roundtrip against the original
    // with a bit-exact memcmp: it avoids -Wfloat-equal AND, unlike is_close with
    // eps 0, treats +-inf as equal (is_close(inf,inf,0) is false because
    // inf-inf is NaN). `f` preserves the sign of a zero double, so signed zeros
    // also compare bit-equal here, exactly as the original `==` did.
    const double roundtrip = static_cast<double>(f);
    if (std::memcmp(&roundtrip, double_val, sizeof(double)) == 0) {
      uint32_t float_bits = 0;
      memcpy(&float_bits, &f, sizeof(float));
      rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_DOUBLE));
      rep->SetIsInlined();
      rep->SetPayload(static_cast<uint64_t>(float_bits));
      return true;
    }
    return false;
  }

  // Try to get as half
  if (auto* half_val = value.as<value::half>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_HALF));
    rep->SetIsInlined();
    // half is 16 bits, fits easily in payload
    uint16_t half_bits = half_val->value;
    rep->SetPayload(static_cast<uint64_t>(half_bits));
    return true;
  }

  // Try to get as bool
  if (auto* bool_val = value.as<bool>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_BOOL));
    rep->SetIsInlined();
    rep->SetPayload(*bool_val ? 1 : 0);
    return true;
  }

  // Try to get as ValueBlock (None)
  if (value.as<value::ValueBlock>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_VALUE_BLOCK));
    rep->SetIsInlined();
    rep->SetPayload(0);  // ValueBlock has no payload data
    return true;
  }

  // Try to get as AnimationBlock (SdfAnimationBlock, Crate type 60). Like
  // ValueBlock, it is an inline type tag with no payload and requires no crate
  // version bump (OpenUSD does not bump for it).
  if (value.as<value::AnimationBlock>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_ANIMATION_BLOCK));
    rep->SetIsInlined();
    rep->SetPayload(0);  // AnimationBlock has no payload data
    return true;
  }

  // Try to get as uchar
  if (auto* uchar_val = value.as<uint8_t>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_UCHAR));
    rep->SetIsInlined();
    rep->SetPayload(static_cast<uint64_t>(*uchar_val));
    return true;
  }

  // Phase 1: Vector/matrix types. OpenUSD inlines Vec2h as raw half bits,
  // other small vectors when all components are exactly int8, and square
  // matrices when only the diagonal is non-zero and diagonal values are int8.

  // Vec2h - half2 (4 bytes = sizeof(uint32_t)): "always inlined" in Pixar's
  // crate format - raw half bit patterns stored directly via memcpy, NOT int8.
  if (auto* vec2h_val = value.as<value::half2>()) {
    rep->SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2H));
    rep->SetIsInlined();
    uint32_t packed = 0;
    uint16_t data[2] = {(*vec2h_val)[0].value, (*vec2h_val)[1].value};
    memcpy(&packed, data, sizeof(data));
    rep->SetPayload(static_cast<uint64_t>(packed));
    return true;
  }

  // Types that cannot be inlined by OpenUSD's special-case encoders.
#define CANNOT_INLINE(Type) \
  if (value.as<Type>()) { return false; }

  if (auto* vec2f_val = value.as<value::float2>()) {
    return TryInlineVecInt8<value::float2, 2>(
        *vec2f_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2F, rep);
  }
  if (auto* vec2d_val = value.as<value::double2>()) {
    return TryInlineVecInt8<value::double2, 2>(
        *vec2d_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2D, rep);
  }
  if (auto* vec2i_val = value.as<value::int2>()) {
    return TryInlineVecInt8<value::int2, 2>(
        *vec2i_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2I, rep);
  }

  // Vec3h/Vec4h: inline only if each component is exactly int8.
  if (auto* vec3h_val = value.as<value::half3>()) {
    return TryInlineHalfVecInt8<value::half3, 3>(
        *vec3h_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3H, rep);
  }

  if (auto* vec3f_val = value.as<value::float3>()) {
    return TryInlineVecInt8<value::float3, 3>(
        *vec3f_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3F, rep);
  }
  if (auto* vec3d_val = value.as<value::double3>()) {
    return TryInlineVecInt8<value::double3, 3>(
        *vec3d_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3D, rep);
  }
  if (auto* vec3i_val = value.as<value::int3>()) {
    return TryInlineVecInt8<value::int3, 3>(
        *vec3i_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3I, rep);
  }
  if (auto* vec4h_val = value.as<value::half4>()) {
    return TryInlineHalfVecInt8<value::half4, 4>(
        *vec4h_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4H, rep);
  }
  if (auto* vec4f_val = value.as<value::float4>()) {
    return TryInlineVecInt8<value::float4, 4>(
        *vec4f_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4F, rep);
  }
  if (auto* vec4d_val = value.as<value::double4>()) {
    return TryInlineVecInt8<value::double4, 4>(
        *vec4d_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4D, rep);
  }
  if (auto* vec4i_val = value.as<value::int4>()) {
    return TryInlineVecInt8<value::int4, 4>(
        *vec4i_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4I, rep);
  }
  if (auto* mat2d_val = value.as<value::matrix2d>()) {
    return TryInlineMatrixDiagonalInt8<value::matrix2d, 2>(
        *mat2d_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_MATRIX2D, rep);
  }
  if (auto* mat3d_val = value.as<value::matrix3d>()) {
    return TryInlineMatrixDiagonalInt8<value::matrix3d, 3>(
        *mat3d_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_MATRIX3D, rep);
  }
  if (auto* mat4d_val = value.as<value::matrix4d>()) {
    return TryInlineMatrixDiagonalInt8<value::matrix4d, 4>(
        *mat4d_val, crate::CrateDataTypeId::CRATE_DATA_TYPE_MATRIX4D, rep);
  }
  CANNOT_INLINE(value::quath)    // 8 bytes
  CANNOT_INLINE(value::quatf)    // 16 bytes
  CANNOT_INLINE(value::quatd)    // 32 bytes

#undef CANNOT_INLINE

  // Empty dictionaries are inlined with payload=0, matching OpenUSD's
  // VtDictionary path. Non-empty dictionaries remain out-of-line.
  if (auto* dict_val = value.as<value::dict>()) {
    if (dict_val->empty()) {
      rep->SetType(static_cast<int32_t>(
          crate::CrateDataTypeId::CRATE_DATA_TYPE_DICTIONARY));
      rep->SetIsInlined();
      rep->SetPayload(0);
      return true;
    }
    return false;
  }
  if (auto* custom_data = value.as<CustomDataType>()) {
    if (custom_data->empty()) {
      rep->SetType(static_cast<int32_t>(
          crate::CrateDataTypeId::CRATE_DATA_TYPE_DICTIONARY));
      rep->SetIsInlined();
      rep->SetPayload(0);
      return true;
    }
    return false;
  }

  // Phase 2: ListOps, Reference, and Payload are NEVER inlined - always
  // out-of-line storage.
  if (value.as<ListOp<value::token>>() ||
      value.as<ListOp<std::string>>() ||
      value.as<ListOp<Path>>() ||
      value.as<ListOp<Reference>>() ||
      value.as<ListOp<Payload>>()) {
    return false;
  }

  if (value.as<Reference>() || value.as<Payload>() || value.as<VariantSelectionMap>()) {
    return false;
  }

  // Phase 1: Empty arrays are inlined with payload=0
  // Non-empty arrays require out-of-line storage with size prefix + data
  //
  // IMPORTANT: The Crate reader expects empty arrays to have payload=0.
  // When the reader sees payload=0 for an array type, it creates an empty array
  // without seeking to any offset.

  // Helper macro to check if array is empty and inline it
#define TRY_INLINE_EMPTY_ARRAY(VecType, CrateTypeId) \
  if (auto* arr = value.as<VecType>()) { \
    if (arr->empty()) { \
      rep->SetType(static_cast<int32_t>(CrateTypeId)); \
      rep->SetIsArray(); \
      rep->SetPayload(0); /* Empty array marker */ \
      return true; \
    } \
    return false; /* Non-empty array needs out-of-line storage */ \
  }

  // Check all array types - inline if empty, otherwise out-of-line
  TRY_INLINE_EMPTY_ARRAY(std::vector<bool>, crate::CrateDataTypeId::CRATE_DATA_TYPE_BOOL)
  TRY_INLINE_EMPTY_ARRAY(std::vector<uint8_t>, crate::CrateDataTypeId::CRATE_DATA_TYPE_UCHAR)
  TRY_INLINE_EMPTY_ARRAY(std::vector<int32_t>, crate::CrateDataTypeId::CRATE_DATA_TYPE_INT)
  TRY_INLINE_EMPTY_ARRAY(std::vector<uint32_t>, crate::CrateDataTypeId::CRATE_DATA_TYPE_UINT)
  TRY_INLINE_EMPTY_ARRAY(std::vector<int64_t>, crate::CrateDataTypeId::CRATE_DATA_TYPE_INT64)
  TRY_INLINE_EMPTY_ARRAY(std::vector<uint64_t>, crate::CrateDataTypeId::CRATE_DATA_TYPE_UINT64)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::half>, crate::CrateDataTypeId::CRATE_DATA_TYPE_HALF)
  TRY_INLINE_EMPTY_ARRAY(std::vector<float>, crate::CrateDataTypeId::CRATE_DATA_TYPE_FLOAT)
  TRY_INLINE_EMPTY_ARRAY(std::vector<double>, crate::CrateDataTypeId::CRATE_DATA_TYPE_DOUBLE)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::half2>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2H)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::half3>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3H)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::half4>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4H)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::float2>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2F)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::float3>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3F)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::float4>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4F)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::double2>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2D)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::double3>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3D)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::double4>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4D)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::int2>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC2I)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::int3>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC3I)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::int4>, crate::CrateDataTypeId::CRATE_DATA_TYPE_VEC4I)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::matrix2d>, crate::CrateDataTypeId::CRATE_DATA_TYPE_MATRIX2D)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::matrix3d>, crate::CrateDataTypeId::CRATE_DATA_TYPE_MATRIX3D)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::matrix4d>, crate::CrateDataTypeId::CRATE_DATA_TYPE_MATRIX4D)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::quath>, crate::CrateDataTypeId::CRATE_DATA_TYPE_QUATH)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::quatf>, crate::CrateDataTypeId::CRATE_DATA_TYPE_QUATF)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::quatd>, crate::CrateDataTypeId::CRATE_DATA_TYPE_QUATD)
  TRY_INLINE_EMPTY_ARRAY(std::vector<std::string>, crate::CrateDataTypeId::CRATE_DATA_TYPE_STRING)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::token>, crate::CrateDataTypeId::CRATE_DATA_TYPE_TOKEN)
  TRY_INLINE_EMPTY_ARRAY(std::vector<value::AssetPath>, crate::CrateDataTypeId::CRATE_DATA_TYPE_ASSET_PATH)

#undef TRY_INLINE_EMPTY_ARRAY

  // Cannot inline - need out-of-line storage
  return false;
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
