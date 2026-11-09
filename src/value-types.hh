// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// Type-erasure technique for Value, a Value class which can represent USD's
/// mandatory and frequently used types(e.g. `float3`, `token`, `asset`) and its
/// array and compound-types(1D/2D array, dictionary). Neigher std::any nor
/// std::variant is applicable for such usecases, so write our own.
///
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>

#include "compiler-features.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// TODO(syoyo): Use C++17 std::optional when compiled with C++-17 compiler

// clang and gcc
#if defined(__EXCEPTIONS) || defined(__cpp_exceptions)

#ifdef nsel_CONFIG_NO_EXCEPTIONS
#undef nsel_CONFIG_NO_EXCEPTIONS
#endif
#ifdef nssv_CONFIG_NO_EXCEPTIONS
#undef nssv_CONFIG_NO_EXCEPTIONS
#endif

#define nsel_CONFIG_NO_EXCEPTIONS 0
#define nssv_CONFIG_NO_EXCEPTIONS 0
#else
// -fno-exceptions
#if !defined(nsel_CONFIG_NO_EXCEPTIONS)
#define nsel_CONFIG_NO_EXCEPTIONS 1
#endif

#define nssv_CONFIG_NO_EXCEPTIONS 1
#endif
//#include "../../src/nonstd/expected.hpp"
#include "nonstd/optional.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "token-type.hh"
#include "typed-array-core.hh"

// forward decls defined below or in lower-level typed-array headers.
namespace lightusd {
template <typename T>
class ChunkedTypedArray;
namespace value {
class any_value;
}
}  // namespace lightusd

namespace lightusd {

namespace value {

// Identifier is the one used in USDA(Ascii)
// See: https://graphics.pixar.com/usd/release/api/_usd__page__datatypes.html
// NOTE: There are some LightUSD specific types(e.g., short, char)
constexpr auto kToken = "token";
constexpr auto kString = "string";
constexpr auto kPath =
    "Path";  // generic(usually prim) path. internal representation.
constexpr auto kAssetPath = "asset";  // `asset` in USDA
constexpr auto kDictionary = "dictionary";
constexpr auto kTimeCode = "timecode";
constexpr auto kPathExpression = "pathExpression";  // `pathExpression` in USDA (SdfPathExpression)

constexpr auto kBool = "bool";
constexpr auto kChar = "char";
constexpr auto kChar2 = "char2";
constexpr auto kChar3 = "char3";
constexpr auto kChar4 = "char4";
constexpr auto kUChar = "uchar";
constexpr auto kUChar2 = "uchar2";
constexpr auto kUChar3 = "uchar3";
constexpr auto kUChar4 = "uchar4";
constexpr auto kHalf = "half";
constexpr auto kInt = "int";
constexpr auto kUInt = "uint";
constexpr auto kInt64 = "int64";
constexpr auto kUInt64 = "uint64";

constexpr auto kShort = "short";
constexpr auto kShort2 = "short2";
constexpr auto kShort3 = "short3";
constexpr auto kShort4 = "short4";

constexpr auto kUShort = "ushort";
constexpr auto kUShort2 = "ushort2";
constexpr auto kUShort3 = "ushort3";
constexpr auto kUShort4 = "ushort4";

constexpr auto kInt2 = "int2";
constexpr auto kInt3 = "int3";
constexpr auto kInt4 = "int4";

constexpr auto kUInt2 = "uint2";
constexpr auto kUInt3 = "uint3";
constexpr auto kUInt4 = "uint4";

constexpr auto kHalf2 = "half2";
constexpr auto kHalf3 = "half3";
constexpr auto kHalf4 = "half4";

// Seems primarily used in usdSkel.
// float precision matrix is not directly used in XformOp
constexpr auto kMatrix2f = "matrix2f";
constexpr auto kMatrix3f = "matrix3f";
constexpr auto kMatrix4f = "matrix4f";

constexpr auto kMatrix2d = "matrix2d";
constexpr auto kMatrix3d = "matrix3d";
constexpr auto kMatrix4d = "matrix4d";

constexpr auto kFloat = "float";
constexpr auto kFloat2 = "float2";
constexpr auto kFloat3 = "float3";
constexpr auto kFloat4 = "float4";

constexpr auto kDouble = "double";
constexpr auto kDouble2 = "double2";
constexpr auto kDouble3 = "double3";
constexpr auto kDouble4 = "double4";

constexpr auto kQuath = "quath";
constexpr auto kQuatf = "quatf";
constexpr auto kQuatd = "quatd";

constexpr auto kVector3h = "vector3h";
constexpr auto kVector3f = "vector3f";
constexpr auto kVector3d = "vector3d";

constexpr auto kVector4h = "vector4h";
constexpr auto kVector4f = "vector4f";
constexpr auto kVector4d = "vector4d";

constexpr auto kPoint3h = "point3h";
constexpr auto kPoint3f = "point3f";
constexpr auto kPoint3d = "point3d";

constexpr auto kNormal3h = "normal3h";
constexpr auto kNormal3f = "normal3f";
constexpr auto kNormal3d = "normal3d";

constexpr auto kColor3h = "color3h";
constexpr auto kColor3f = "color3f";
constexpr auto kColor3d = "color3d";
constexpr auto kColor4h = "color4h";
constexpr auto kColor4f = "color4f";
constexpr auto kColor4d = "color4d";

constexpr auto kFrame4d = "frame4d";

constexpr auto kTexCoord2h = "texCoord2h";
constexpr auto kTexCoord2f = "texCoord2f";
constexpr auto kTexCoord2d = "texCoord2d";

constexpr auto kTexCoord3h = "texCoord3h";
constexpr auto kTexCoord3f = "texCoord3f";
constexpr auto kTexCoord3d = "texCoord3d";

constexpr auto kTexCoord4h = "texCoord4h";
constexpr auto kTexCoord4f = "texCoord4f";
constexpr auto kTexCoord4d = "texCoord4d";

constexpr auto kRelationship = "rel";

inline std::string Add1DArraySuffix(const std::string &c) { return c + "[]"; }

using token = lightusd::Token;

// single or triple-quoted('"""' or ''') string
struct StringData {

  StringData() = default;
  StringData(const std::string &v) : value(v) {}
  StringData(std::string &&v) : value(std::move(v)) {}
  StringData &operator=(const std::string &v) {
    value = v;
    return (*this);
  }
  StringData &operator=(std::string &&v) {
    value = std::move(v);
    return (*this);
  }

  std::string value;
  bool is_triple_quoted{false};
  bool single_quote{false};  // true for ', false for "

  // For prim metadata comment: track whether parsed with "comment =" prefix
  // When true, pprint outputs "comment = ...", otherwise just the string
  bool has_comment_prefix{false};

  // optional(for USDA)
  int line_row{0};
  int line_col{0};
};

// SdfAssetPath
class AssetPath {
 public:
  AssetPath() = default;
  AssetPath(const std::string &a) : asset_path_(a) {}
  AssetPath(std::string &&a) : asset_path_(std::move(a)) {}
  AssetPath(const std::string &a, const std::string &r)
      : asset_path_(a), resolved_path_(r) {}
  AssetPath(std::string &&a, std::string &&r)
      : asset_path_(std::move(a)), resolved_path_(std::move(r)) {}

  bool Resolve() {
    // TODO;
    return false;
  }

  const std::string &GetAssetPath() const LIGHTUSD_LIFETIMEBOUND {
    return asset_path_;
  }

  const std::string &GetResolvedPath() const LIGHTUSD_LIFETIMEBOUND {
    return resolved_path_;
  }

 private:
  std::string asset_path_;
  std::string resolved_path_;
};

class TimeCode {
 public:
  TimeCode(const double d) : time_(d) {}

  static constexpr double Default() {
    // Return qNaN. same in pxrUSD
    return std::numeric_limits<double>::quiet_NaN();
  }

  // TODO: Deprecate `Get` and use `get`
  double Get(bool *is_default_timecode) {
    if (is_default_timecode) {
      (*is_default_timecode) = is_default();
    }
    return time_;
  }

  double get(bool *is_default_timecode) {
    if (is_default_timecode) {
      (*is_default_timecode) = is_default();
    }
    return time_;
  }

  bool is_default() {
    // TODO: Bitwise comparison
    return std::isnan(time_);
  }

 private:
  double time_{std::numeric_limits<double>::quiet_NaN()};
};

static_assert(sizeof(TimeCode) == 8, "Size of TimeCode must be 8.");

// SdfPathExpression value type.
//
// Stores the expression as its canonical text form (matching OpenUSD's
// `SdfPathExpression::GetText()`), which is also how it is serialized in Crate
// (a plain UTF-8 string). Parsing into a matchable representation and the
// membership evaluation live in `core/path-expression.{hh,cc}` so that this
// foundational header stays light. Storing the verbatim text gives loss-free
// round-trip.
class PathExpression {
 public:
  PathExpression() = default;
  explicit PathExpression(const std::string &text) : text_(text) {}
  explicit PathExpression(std::string &&text) : text_(std::move(text)) {}

  const std::string &GetText() const LIGHTUSD_LIFETIMEBOUND { return text_; }
  void SetText(const std::string &text) { text_ = text; }

  bool empty() const { return text_.empty(); }

  bool operator==(const PathExpression &rhs) const { return text_ == rhs.text_; }
  bool operator!=(const PathExpression &rhs) const { return !(*this == rhs); }

 private:
  std::string text_;
};

//
// Type ID for TypeTraits<T>::type_id.
//
// These type IDs are internally used and can be changed arbitrary.
//
// These ID assignment won't affect Crate binary serialization.
// (See `crate-format.hh` for Type ID used in Crate binary)
//
// TODO(syoyo): Support 3D and 4D?
constexpr uint32_t TYPE_ID_1D_ARRAY_BIT = 1 << 20;  // 1024
// constexpr uint32_t TYPE_ID_2D_ARRAY_BIT = 1 << 21;  // 2048
//  constexpr uint32_t TYPE_ID_3D_ARRAY_BIT = 1 << 22;
//  constexpr uint32_t TYPE_ID_4D_ARRAY_BIT = 1 << 23;
constexpr uint32_t TYPE_ID_TERMINATOR_BIT = 1 << 24;

enum TypeId {
  TYPE_ID_INVALID,  // = 0
  TYPE_ID_NULL,
  TYPE_ID_VOID,
  TYPE_ID_MONOSTATE,
  TYPE_ID_VALUEBLOCK,  // Value block. `None` in ascii.
  TYPE_ID_ANIMATION_BLOCK,  // Animation block. `AnimationBlock` in ascii (Crate type 60).

  // -- begin value type
  TYPE_ID_VALUE_BEGIN,

  TYPE_ID_TOKEN,
  TYPE_ID_STRING,
  TYPE_ID_STRING_DATA,  // String for primvar and metadata. Includes multi-line
                        // string

  TYPE_ID_BOOL,

  TYPE_ID_CHAR,
  TYPE_ID_CHAR2,
  TYPE_ID_CHAR3,
  TYPE_ID_CHAR4,

  // TYPE_ID_INT8,
  TYPE_ID_HALF,
  TYPE_ID_INT32,
  TYPE_ID_INT64,

  TYPE_ID_HALF2,
  TYPE_ID_HALF3,
  TYPE_ID_HALF4,

  TYPE_ID_INT2,  // int32 x 2
  TYPE_ID_INT3,
  TYPE_ID_INT4,

  TYPE_ID_UCHAR,  // uint8
  TYPE_ID_UCHAR2,
  TYPE_ID_UCHAR3,
  TYPE_ID_UCHAR4,

  TYPE_ID_UINT32,
  TYPE_ID_UINT64,

  TYPE_ID_SHORT,
  TYPE_ID_SHORT2,
  TYPE_ID_SHORT3,
  TYPE_ID_SHORT4,

  TYPE_ID_USHORT,
  TYPE_ID_USHORT2,
  TYPE_ID_USHORT3,
  TYPE_ID_USHORT4,

  TYPE_ID_UINT2,
  TYPE_ID_UINT3,
  TYPE_ID_UINT4,

  TYPE_ID_FLOAT,
  TYPE_ID_FLOAT2,
  TYPE_ID_FLOAT3,
  TYPE_ID_FLOAT4,

  TYPE_ID_DOUBLE,
  TYPE_ID_DOUBLE2,
  TYPE_ID_DOUBLE3,
  TYPE_ID_DOUBLE4,

  TYPE_ID_QUATH,
  TYPE_ID_QUATF,
  TYPE_ID_QUATD,

  TYPE_ID_MATRIX2F,
  TYPE_ID_MATRIX3F,
  TYPE_ID_MATRIX4F,

  TYPE_ID_MATRIX2D,
  TYPE_ID_MATRIX3D,
  TYPE_ID_MATRIX4D,

  TYPE_ID_COLOR3H,
  TYPE_ID_COLOR3F,
  TYPE_ID_COLOR3D,

  TYPE_ID_COLOR4H,
  TYPE_ID_COLOR4F,
  TYPE_ID_COLOR4D,

  TYPE_ID_POINT3H,
  TYPE_ID_POINT3F,
  TYPE_ID_POINT3D,

  TYPE_ID_NORMAL3H,
  TYPE_ID_NORMAL3F,
  TYPE_ID_NORMAL3D,

  TYPE_ID_VECTOR3H,
  TYPE_ID_VECTOR3F,
  TYPE_ID_VECTOR3D,

  TYPE_ID_FRAME4D,

  TYPE_ID_TEXCOORD2H,
  TYPE_ID_TEXCOORD2F,
  TYPE_ID_TEXCOORD2D,

  TYPE_ID_TEXCOORD3H,
  TYPE_ID_TEXCOORD3F,
  TYPE_ID_TEXCOORD3D,

  TYPE_ID_EXTENT,  // float3[2]

  TYPE_ID_TIMECODE,

  // TYPE_ID_ASSET,
  TYPE_ID_ASSET_PATH,

  TYPE_ID_PATH_EXPRESSION,  // SdfPathExpression

  TYPE_ID_DICT,        // Generic dict type. TODO: remove?
  TYPE_ID_CUSTOMDATA,  // similar to `dictionary`, but limited types are allowed
                       // to use. for metadatum(e.g. `customData` in Prim Meta)

  TYPE_ID_VALUE_END,

  // -- end value type

  TYPE_ID_LAYER_OFFSET,
  TYPE_ID_PAYLOAD,

  // Types in prim-types.hh
  TYPE_ID_REFERENCE,
  TYPE_ID_SPECIFIER,
  TYPE_ID_PERMISSION,
  TYPE_ID_VARIABILITY,
  TYPE_ID_LIST_OP_TOKEN,
  TYPE_ID_LIST_OP_STRING,
  TYPE_ID_LIST_OP_PATH,
  TYPE_ID_LIST_OP_REFERENCE,
  TYPE_ID_LIST_OP_INT,
  TYPE_ID_LIST_OP_INT64,
  TYPE_ID_LIST_OP_UINT,
  TYPE_ID_LIST_OP_UINT64,
  TYPE_ID_LIST_OP_PAYLOAD,

  TYPE_ID_PATH,
  TYPE_ID_PATH_VECTOR,
  TYPE_ID_RELOCATES,  // SdfRelocates: std::vector<std::pair<Path, Path>> (Crate type 58)
  TYPE_ID_TOKEN_VECTOR,
  TYPE_ID_RELATIONSHIP,

  // -- end of base type for Property.

  TYPE_ID_TIMESAMPLES,
  TYPE_ID_SPLINE_DATA,  // primvar::PrimVar::SplineData carrier (Crate type 59)
  TYPE_ID_ARRAY_EDIT,  // value::ArrayEdit carrier (VtArrayEdit, crate >= 0.14.0)
  TYPE_ID_VARIANT_SELECION_MAP,


  // Types in crate-format.hh
  TYPE_ID_CRATE_BEGIN = 256,
  TYPE_ID_CRATE_VALUE,
  TYPE_ID_CRATE_UNREGISTERED_VALUE,
  TYPE_ID_CRATE_LIST_OP_UNREGISTERED_VALUE,
  TYPE_ID_CRATE_END,

  // Types for Model and GPrim
  TYPE_ID_MODEL_BEGIN = (1 << 10),
  TYPE_ID_MODEL,  // internally used class
  // TYPE_ID_GROUP,
  TYPE_ID_SCOPE,
  TYPE_ID_GPRIM,
  TYPE_ID_GEOM_XFORM,
  TYPE_ID_GEOM_MESH,
  TYPE_ID_GEOM_BASIS_CURVES,
  TYPE_ID_GEOM_NURBS_CURVES,
  TYPE_ID_GEOM_SPHERE,
  TYPE_ID_GEOM_CUBE,
  TYPE_ID_GEOM_CYLINDER,
  TYPE_ID_GEOM_CONE,
  TYPE_ID_GEOM_CAPSULE,
  TYPE_ID_GEOM_POINTS,
  TYPE_ID_GEOM_GEOMSUBSET,
  TYPE_ID_GEOM_POINT_INSTANCER,
  TYPE_ID_GEOM_CAMERA,
  TYPE_ID_GEOM_PLANE,
  TYPE_ID_GEOM_CYLINDER_1,
  TYPE_ID_GEOM_CAPSULE_1,
  TYPE_ID_GEOM_TET_MESH,
  TYPE_ID_GEOM_NURBS_PATCH,
  TYPE_ID_GEOM_HERMITE_CURVES,
  TYPE_ID_GEOM_END,

  // Types for usdLux
  TYPE_ID_LUX_BEGIN = (1 << 10) + (1 << 9),
  TYPE_ID_LUX_SPHERE,
  TYPE_ID_LUX_DOME,
  TYPE_ID_LUX_CYLINDER,
  TYPE_ID_LUX_DISK,
  TYPE_ID_LUX_RECT,
  TYPE_ID_LUX_DISTANT,
  TYPE_ID_LUX_GEOMETRY,
  TYPE_ID_LUX_PORTAL,
  TYPE_ID_LUX_PLUGIN,
  TYPE_ID_LUX_DOME_1,
  TYPE_ID_LUX_LIGHT_FILTER,
  TYPE_ID_LUX_PLUGIN_LIGHT_FILTER,
  TYPE_ID_LUX_END,

  // Types for usdShader
  TYPE_ID_SHADER_BEGIN = 1 << 11,
  TYPE_ID_SHADER,
  TYPE_ID_MATERIAL,
  TYPE_ID_NODEGRAPH,
  TYPE_ID_SHADER_END,

  // Types for usdImaging and usdMtlx
  // See <pxrUSD>/pxr/usdImaging/usdImaging/tokens.h
  TYPE_ID_IMAGING_BEGIN = (1 << 11) + (1 << 10),
  TYPE_ID_IMAGING_SHADER_NODE,
  TYPE_ID_IMAGING_PREVIEWSURFACE,
  TYPE_ID_IMAGING_UVTEXTURE,
  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT,
  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT2,
  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT3,
  TYPE_ID_IMAGING_PRIMVAR_READER_FLOAT4,
  TYPE_ID_IMAGING_PRIMVAR_READER_INT,
  TYPE_ID_IMAGING_PRIMVAR_READER_STRING,
  TYPE_ID_IMAGING_PRIMVAR_READER_NORMAL, // float3
  TYPE_ID_IMAGING_PRIMVAR_READER_POINT, // float3
  TYPE_ID_IMAGING_PRIMVAR_READER_VECTOR, // float3
  TYPE_ID_IMAGING_PRIMVAR_READER_MATRIX, // float3
  TYPE_ID_IMAGING_TRANSFORM_2D,

  TYPE_ID_IMAGING_MTLX_PREVIEWSURFACE,
  TYPE_ID_IMAGING_MTLX_STANDARDSURFACE,
  TYPE_ID_IMAGING_MTLX_OPENPBRSURFACE,
  TYPE_ID_IMAGING_MTLX_UNIFORMEDF,
  TYPE_ID_IMAGING_MTLX_CONICALEDF,
  TYPE_ID_IMAGING_MTLX_MEASUREDEDF,
  TYPE_ID_IMAGING_MTLX_LIGHT,
  TYPE_ID_IMAGING_OPENPBR_SURFACE,
  TYPE_ID_IMAGING_SOURCE_COLORSPACE,

  TYPE_ID_IMAGING_END,

  // Types for usdVol
  TYPE_ID_VOL_BEGIN = 1 << 12,
  TYPE_ID_VOLUME,
  TYPE_ID_FIELD_ASSET,
  TYPE_ID_OPENVDB_ASSET,
  TYPE_ID_FIELD3D_ASSET,
  // UsdVol field base schemas: VolumeFieldBase is the current Xformable root
  // of field prims, FieldBase its deprecated spelling; VolumeFieldAsset is the
  // file-backed field base that FieldAsset (deprecated) shares.
  TYPE_ID_VOLUME_FIELD_BASE,
  TYPE_ID_FIELD_BASE,
  TYPE_ID_VOLUME_FIELD_ASSET,
  TYPE_ID_VOL_END,

  // Types for usdRender
  TYPE_ID_RENDER_BEGIN = (1 << 12) + 16,
  TYPE_ID_RENDER_SETTINGS,
  TYPE_ID_RENDER_PRODUCT,
  TYPE_ID_RENDER_VAR,
  TYPE_ID_RENDER_END,

  // Types for usdProc
  TYPE_ID_PROC_BEGIN = (1 << 12) + 32,
  TYPE_ID_GENERATIVE_PROCEDURAL,
  TYPE_ID_PROC_END,

  // Types for usdSkel
  TYPE_ID_SKEL_BEGIN = 1 << 13,
  TYPE_ID_SKEL_ROOT,
  TYPE_ID_SKELETON,
  TYPE_ID_SKELANIMATION,
  TYPE_ID_BLENDSHAPE,
  TYPE_ID_SKEL_END,

  // Types for UsdPhysics + mjcPhysics
  TYPE_ID_PHYSICS_BEGIN = (1 << 13) + 16,
  TYPE_ID_PHYSICS_JOINT,
  TYPE_ID_PHYSICS_SCENE,
  TYPE_ID_PHYSICS_REVOLUTE_JOINT,
  TYPE_ID_PHYSICS_PRISMATIC_JOINT,
  TYPE_ID_PHYSICS_SPHERICAL_JOINT,
  TYPE_ID_PHYSICS_FIXED_JOINT,
  TYPE_ID_PHYSICS_DISTANCE_JOINT,
  TYPE_ID_MJC_ACTUATOR,
  TYPE_ID_MJC_TENDON,
  TYPE_ID_MJC_KEYFRAME,
  TYPE_ID_MJC_SENSOR,
  TYPE_ID_PHYSICS_COLLISION_GROUP,
  TYPE_ID_NEWTON_ACTUATOR,
  TYPE_ID_PHYSICS_END,

  // Types for AR/Interactive schemas (Apple Preliminary_*)
  TYPE_ID_AR_BEGIN = (1 << 13) + 32,
  TYPE_ID_PRELIMINARY_GRAVITATIONAL_FORCE,
  TYPE_ID_PRELIMINARY_INFINITE_COLLIDER_PLANE,
  TYPE_ID_PRELIMINARY_REFERENCE_IMAGE,
  TYPE_ID_PRELIMINARY_BEHAVIOR,
  TYPE_ID_PRELIMINARY_TRIGGER,
  TYPE_ID_PRELIMINARY_ACTION,
  TYPE_ID_PRELIMINARY_TEXT,
  TYPE_ID_AR_END,

  // Types for usdMedia
  TYPE_ID_MEDIA_BEGIN = (1 << 13) + 48,
  TYPE_ID_SPATIAL_AUDIO,
  TYPE_ID_MEDIA_END,

  TYPE_ID_MODEL_END,


  // Types for API
  TYPE_ID_API_BEGIN = 1 << 14,
  TYPE_ID_COLLECTION,
  TYPE_ID_COLLECTION_INSTANCE,
  TYPE_ID_MATERIAL_BINDING,
  TYPE_ID_MATERIALX_CONFIG_API,
  TYPE_ID_COLOR_SPACE_API,
  TYPE_ID_API_END,

  // Base ID for user data type(less than `TYPE_ID_1D_ARRAY_BIT-1`)
  TYPE_ID_USER_BEGIN = 1 << 16,

  TYPE_ID_ALL = (TYPE_ID_TERMINATOR_BIT - 1)  // terminator.
};

//static_assert(TYPE_ID_TYPED_ARRAY_TIMESAMPLE_VALUE < 256, "internal error.");
static_assert(TYPE_ID_API_END <= 65535, "Non user-defined TYPE_ID must be less than 16bit");


struct timecode {
  double value;
};

struct half {
  uint16_t value;

  // Bitwise comparison (matches OpenUSD GfHalf semantics).
  bool operator==(const half &rhs) const { return value == rhs.value; }
  bool operator!=(const half &rhs) const { return value != rhs.value; }
};

using half2 = std::array<half, 2>;
using half3 = std::array<half, 3>;
using half4 = std::array<half, 4>;

///
/// Convert half-precision float to single-precision float.
/// Uses portable bit manipulation that works on both little-endian and big-endian.
///
float half_to_float(value::half h);

half float_to_half_full(float f);

half operator+(const half &a, const half &b);
half operator-(const half &a, const half &b);
half operator*(const half &a, const half &b);
half operator/(const half &a, const half &b);

half& operator+=(half &a, const half &b);
half& operator-=(half &a, const half &b);
half& operator*=(half &a, const half &b);
half& operator/=(half &a, const half &b);

half operator+(const half &a, float b);
half operator-(const half &a, float b);
half operator*(const half &a, float b);
half operator/(const half &a, float b);

half operator+(float a, const half &b);
half operator-(float a, const half &b);
half operator*(float a, const half &b);
half operator/(float a, const half &b);

using char2 = std::array<char, 2>;
using char3 = std::array<char, 3>;
using char4 = std::array<char, 4>;

using uchar2 = std::array<uint8_t, 2>;
using uchar3 = std::array<uint8_t, 3>;
using uchar4 = std::array<uint8_t, 4>;

using short2 = std::array<int16_t, 2>;
using short3 = std::array<int16_t, 3>;
using short4 = std::array<int16_t, 4>;

using ushort2 = std::array<uint16_t, 2>;
using ushort3 = std::array<uint16_t, 3>;
using ushort4 = std::array<uint16_t, 4>;

using int2 = std::array<int32_t, 2>;
using int3 = std::array<int32_t, 3>;
using int4 = std::array<int32_t, 4>;

using uint2 = std::array<uint32_t, 2>;
using uint3 = std::array<uint32_t, 3>;
using uint4 = std::array<uint32_t, 4>;

using float2 = std::array<float, 2>;
using float3 = std::array<float, 3>;
using float4 = std::array<float, 4>;

using double2 = std::array<double, 2>;
using double3 = std::array<double, 3>;
using double4 = std::array<double, 4>;

//
// Matrix is represented as row-major order as done in pxrUSD.
// m[i][j] is read as: i'th row, j'th column
// memory layout is same both for column-major and row-major.
// (e.g. m[3][0], m[3][1], m[3][2] or a[13], a[14], a[15] are translation components for 4x4 matrix)
//

struct matrix2d;
struct matrix3d;
struct matrix4d;

struct matrix2f {
  // Default constructor - makes struct trivial
  matrix2f() = default;

  // Copy/move constructors and assignment operators
  matrix2f(const matrix2f&) = default;
  matrix2f(matrix2f&&) = default;
  matrix2f& operator=(const matrix2f&) = default;
  matrix2f& operator=(matrix2f&&) = default;

  void set_row(uint32_t row, float x, float y);
  void set_scale(float sx, float sy);

  static matrix2f identity() {
    matrix2f mat{};
    mat.m[0][0] = 1.0f;
    mat.m[0][1] = 0.0f;
    mat.m[1][0] = 0.0f;
    mat.m[1][1] = 1.0f;
    return mat;
  }

  matrix2f(const matrix2d &rhs);
  matrix2f &operator=(const matrix2d &rhs);

  float m[2][2];
};

struct matrix3f {
  // Default constructor - makes struct trivial
  matrix3f() = default;

  // Copy/move constructors and assignment operators
  matrix3f(const matrix3f&) = default;
  matrix3f(matrix3f&&) = default;
  matrix3f& operator=(const matrix3f&) = default;
  matrix3f& operator=(matrix3f&&) = default;

  void set_row(uint32_t row, float x, float y, float z);
  void set_scale(float sx, float sy, float sz);
  void set_translation(float tx, float ty, float tz);

  static matrix3f identity() {
    matrix3f mat{};
    mat.m[0][0] = 1.0f;
    mat.m[0][1] = 0.0f;
    mat.m[0][2] = 0.0f;
    mat.m[1][0] = 0.0f;
    mat.m[1][1] = 1.0f;
    mat.m[1][2] = 0.0f;
    mat.m[2][0] = 0.0f;
    mat.m[2][1] = 0.0f;
    mat.m[2][2] = 1.0f;
    return mat;
  }

  matrix3f(const matrix3d &rhs);
  matrix3f &operator=(const matrix3d &rhs);

  float m[3][3];
};

struct matrix4f {
  matrix4f() = default;
  matrix4f(const matrix4f&) = default;
  matrix4f(matrix4f&&) = default;
  matrix4f& operator=(const matrix4f&) = default;
  matrix4f& operator=(matrix4f&&) = default;

  void set_row(uint32_t row, float x, float y, float z, float w);
  void set_scale(float sx, float sy, float sz);
  void set_translation(float tx, float ty, float tz);

  static matrix4f identity() {
    matrix4f mat{};
    mat.m[0][0] = 1.0f;
    mat.m[1][1] = 1.0f;
    mat.m[2][2] = 1.0f;
    mat.m[3][3] = 1.0f;
    return mat;
  }

  matrix4f(const matrix4d &rhs);

  matrix4f &operator=(const matrix4d &rhs);

  float m[4][4];
};

struct matrix2d {
  matrix2d() = default;
  matrix2d(const matrix2d&) = default;
  matrix2d(matrix2d&&) = default;
  matrix2d& operator=(const matrix2d&) = default;
  matrix2d& operator=(matrix2d&&) = default;

  void set_row(uint32_t row, double x, double y);
  void set_scale(double sx, double sy);

  static matrix2d identity() {
    matrix2d mat{};
    mat.m[0][0] = 1.0;
    mat.m[1][1] = 1.0;
    return mat;
  }

  matrix2d &operator=(const matrix2f &rhs);

  double m[2][2];
};

struct matrix3d {
  matrix3d() = default;
  matrix3d(const matrix3d&) = default;
  matrix3d(matrix3d&&) = default;
  matrix3d& operator=(const matrix3d&) = default;
  matrix3d& operator=(matrix3d&&) = default;

  void set_row(uint32_t row, double x, double y, double z);
  void set_scale(double sx, double sy, double sz);

  static matrix3d identity() {
    matrix3d mat{};
    mat.m[0][0] = 1.0;
    mat.m[1][1] = 1.0;
    mat.m[2][2] = 1.0;
    return mat;
  }

  matrix3d &operator=(const matrix3f &rhs);

  double m[3][3];
};

struct matrix4d {
  matrix4d() = default;
  matrix4d(const matrix4d&) = default;
  matrix4d(matrix4d&&) = default;
  matrix4d& operator=(const matrix4d&) = default;
  matrix4d& operator=(matrix4d&&) = default;

  void set_row(uint32_t row, double x, double y, double z, double w);
  void set_scale(double sx, double sy, double sz);

  static matrix4d identity() {
    matrix4d mat{};
    mat.m[0][0] = 1.0;
    mat.m[1][1] = 1.0;
    mat.m[2][2] = 1.0;
    mat.m[3][3] = 1.0;
    return mat;
  }

  matrix4d &operator=(const matrix4f &rhs);

  double m[4][4];
};

// = matrix4d
struct frame4d {
  frame4d() {
    m[0][0] = 1.0;
    m[0][1] = 0.0;
    m[0][2] = 0.0;
    m[0][3] = 0.0;

    m[1][0] = 0.0;
    m[1][1] = 1.0;
    m[1][2] = 0.0;
    m[1][3] = 0.0;

    m[2][0] = 0.0;
    m[2][1] = 0.0;
    m[2][2] = 1.0;
    m[2][3] = 0.0;

    m[3][0] = 0.0;
    m[3][1] = 0.0;
    m[3][2] = 0.0;
    m[3][3] = 1.0;
  }
  double m[4][4];
};

// ret = m x n in row-major(n x m in column-major)
// i.e. You can express TRS transform as
//
// p * S * R * T = p'
// p' = Mult(Mult(S, R), T)
//
// you can express world matrix as
//
// node.world = parent.world * node.local
//            = Mult(parent.world, node.local)
// Matrix multiply (implementations in value-types.cc)
matrix2f Mult(const matrix2f &m, const matrix2f &n);
matrix3f Mult(const matrix3f &m, const matrix3f &n);
matrix4f Mult(const matrix4f &m, const matrix4f &n);
matrix2d Mult(const matrix2d &m, const matrix2d &n);
matrix3d Mult(const matrix3d &m, const matrix3d &n);
matrix4d Mult(const matrix4d &m, const matrix4d &n);

// Matrix-vector multiply (implementations in value-types.cc)
// Kept as template since it's used with many type combinations in xform.cc
template <typename MTy, typename VTy, typename MBaseTy, typename VBaseTy, size_t N>
VTy MultV(const MTy &m, const VTy &v) {
  static_assert(std::is_same<MBaseTy, double>::value || std::is_same<MBaseTy, float>::value,
    "Matrix element type must be `float` or `double`");

  typedef typename std::conditional<sizeof(MBaseTy) >= sizeof(VBaseTy), MBaseTy, VBaseTy>::type Ty;

  VTy ret;

  for (size_t j = 0; j < N; j++) {
    Ty value = static_cast<Ty>(0);
    for (size_t i = 0; i < N; i++) {
      value += static_cast<Ty>(m.m[i][j]) * static_cast<Ty>(v[i]);
    }
    ret[j] = static_cast<VBaseTy>(value);
  }

  return ret;
}

// Matrix add (implementations in value-types.cc)
matrix2f MatAdd(const matrix2f &a, const matrix2f &b);
matrix3f MatAdd(const matrix3f &a, const matrix3f &b);
matrix4f MatAdd(const matrix4f &a, const matrix4f &b);
matrix2d MatAdd(const matrix2d &a, const matrix2d &b);
matrix3d MatAdd(const matrix3d &a, const matrix3d &b);
matrix4d MatAdd(const matrix4d &a, const matrix4d &b);

// Matrix subtract (implementations in value-types.cc)
matrix2f MatSub(const matrix2f &a, const matrix2f &b);
matrix3f MatSub(const matrix3f &a, const matrix3f &b);
matrix4f MatSub(const matrix4f &a, const matrix4f &b);
matrix2d MatSub(const matrix2d &a, const matrix2d &b);
matrix3d MatSub(const matrix3d &a, const matrix3d &b);
matrix4d MatSub(const matrix4d &a, const matrix4d &b);

// TODO: division

inline matrix2f operator+(const matrix2f &a, const matrix2f &b) { return MatAdd(a, b); }
inline matrix2f operator-(const matrix2f &a, const matrix2f &b) { return MatSub(a, b); }
inline matrix2f operator*(const matrix2f &a, const matrix2f &b) { return Mult(a, b); }

inline matrix3f operator+(const matrix3f &a, const matrix3f &b) { return MatAdd(a, b); }
inline matrix3f operator-(const matrix3f &a, const matrix3f &b) { return MatSub(a, b); }
inline matrix3f operator*(const matrix3f &a, const matrix3f &b) { return Mult(a, b); }

inline matrix4f operator+(const matrix4f &a, const matrix4f &b) { return MatAdd(a, b); }
inline matrix4f operator-(const matrix4f &a, const matrix4f &b) { return MatSub(a, b); }
inline matrix4f operator*(const matrix4f &a, const matrix4f &b) { return Mult(a, b); }

inline matrix2d operator+(const matrix2d &a, const matrix2d &b) { return MatAdd(a, b); }
inline matrix2d operator-(const matrix2d &a, const matrix2d &b) { return MatSub(a, b); }
inline matrix2d operator*(const matrix2d &a, const matrix2d &b) { return Mult(a, b); }

inline matrix3d operator+(const matrix3d &a, const matrix3d &b) { return MatAdd(a, b); }
inline matrix3d operator-(const matrix3d &a, const matrix3d &b) { return MatSub(a, b); }
inline matrix3d operator*(const matrix3d &a, const matrix3d &b) { return Mult(a, b); }

inline matrix4d operator+(const matrix4d &a, const matrix4d &b) { return MatAdd(a, b); }
inline matrix4d operator-(const matrix4d &a, const matrix4d &b) { return MatSub(a, b); }
inline matrix4d operator*(const matrix4d &a, const matrix4d &b) { return Mult(a, b); }

// Equality operators for matrix types
// Use floating-point aware comparison (suitable for dedup)
// Implementations in value-types.cc
bool operator==(const matrix2f &a, const matrix2f &b);
bool operator==(const matrix3f &a, const matrix3f &b);
bool operator==(const matrix4f &a, const matrix4f &b);
bool operator==(const matrix2d &a, const matrix2d &b);
bool operator==(const matrix3d &a, const matrix3d &b);
bool operator==(const matrix4d &a, const matrix4d &b);

// Quaternion has memory layout of [x, y, z, w] in Crate(Binary)
// and QfQuat class in pxrUSD.
// https://github.com/PixarAnimationStudios/USD/blob/3abc46452b1271df7650e9948fef9f0ce602e3b2/pxr/base/gf/quatf.h#L287
// NOTE: ASCII uses [w, x, y, z] ordering
struct quath {
  half3 imag;
  half real;
  half operator[](size_t idx) const { return *(&imag[0] + idx); }
  half &operator[](size_t idx) { return *(&imag[0] + idx); }
  bool operator==(const quath &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const quath &rhs) const { return !(*this == rhs); }
};

struct quatf {
  float3 imag;
  float real;
  float operator[](size_t idx) const { return *(&imag[0] + idx); }
  float &operator[](size_t idx) { return *(&imag[0] + idx); }
  bool operator==(const quatf &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const quatf &rhs) const { return !(*this == rhs); }
};

struct quatd {
  double3 imag;
  double real;
  double operator[](size_t idx) const { return *(&imag[0] + idx); }
  double &operator[](size_t idx) { return *(&imag[0] + idx); }
  bool operator==(const quatd &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const quatd &rhs) const { return !(*this == rhs); }
};

struct vector3h {
  half x, y, z;

  half operator[](size_t idx) const { return *(&x + idx); }
  half &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const vector3h &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const vector3h &rhs) const { return !(*this == rhs); }
};

struct vector3f {
  float x, y, z;

  float operator[](size_t idx) const { return *(&x + idx); }
  float &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const vector3f &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const vector3f &rhs) const { return !(*this == rhs); }
};

struct vector3d {
  double x, y, z;

  double operator[](size_t idx) const { return *(&x + idx); }
  double &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const vector3d &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const vector3d &rhs) const { return !(*this == rhs); }
};

struct normal3h {
  half x, y, z;

  half operator[](size_t idx) const { return *(&x + idx); }
  half &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const normal3h &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const normal3h &rhs) const { return !(*this == rhs); }
};

struct normal3f {
  float x, y, z;

  float operator[](size_t idx) const { return *(&x + idx); }
  float &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const normal3f &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const normal3f &rhs) const { return !(*this == rhs); }
};

struct normal3d {
  double x, y, z;

  double operator[](size_t idx) const { return *(&x + idx); }
  double &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const normal3d &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const normal3d &rhs) const { return !(*this == rhs); }
};

struct point3h {
  half x, y, z;

  half operator[](size_t idx) const { return *(&x + idx); }
  half &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const point3h &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const point3h &rhs) const { return !(*this == rhs); }
};


struct point3f {
  float x, y, z;

  float operator[](size_t idx) const { return *(&x + idx); }
  float &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const point3f &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const point3f &rhs) const { return !(*this == rhs); }
};


struct point3d {
  double x, y, z;

  double operator[](size_t idx) const { return *(&x + idx); }
  double &operator[](size_t idx) { return *(&x + idx); }
  bool operator==(const point3d &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const point3d &rhs) const { return !(*this == rhs); }
};


struct color3h {
  half r, g, b;

  half operator[](size_t idx) const { return *(&r + idx); }
  half &operator[](size_t idx) { return *(&r + idx); }
  bool operator==(const color3h &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const color3h &rhs) const { return !(*this == rhs); }
};

struct color3f {
  float r, g, b;

  float operator[](size_t idx) const { return *(&r + idx); }
  float &operator[](size_t idx) { return *(&r + idx); }
  bool operator==(const color3f &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const color3f &rhs) const { return !(*this == rhs); }
};

struct color4h {
  half r, g, b, a;

  half operator[](size_t idx) const { return *(&r + idx); }
  half &operator[](size_t idx) { return *(&r + idx); }
  bool operator==(const color4h &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const color4h &rhs) const { return !(*this == rhs); }
};

struct color4f {
  float r, g, b, a;

  float operator[](size_t idx) const { return *(&r + idx); }
  float &operator[](size_t idx) { return *(&r + idx); }
  bool operator==(const color4f &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const color4f &rhs) const { return !(*this == rhs); }
};

struct color3d {
  double r, g, b;

  double operator[](size_t idx) const { return *(&r + idx); }
  double &operator[](size_t idx) { return *(&r + idx); }
  bool operator==(const color3d &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const color3d &rhs) const { return !(*this == rhs); }
};

struct color4d {
  double r, g, b, a;

  double operator[](size_t idx) const { return *(&r + idx); }
  double &operator[](size_t idx) { return *(&r + idx); }
  bool operator==(const color4d &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const color4d &rhs) const { return !(*this == rhs); }
};

struct texcoord2h {
  half s, t;
  half operator[](size_t idx) const { return *(&s + idx); }
  half &operator[](size_t idx) { return *(&s + idx); }
  bool operator==(const texcoord2h &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const texcoord2h &rhs) const { return !(*this == rhs); }
};

struct texcoord2f {
  float s, t;
  float operator[](size_t idx) const { return *(&s + idx); }
  float &operator[](size_t idx) { return *(&s + idx); }
  bool operator==(const texcoord2f &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const texcoord2f &rhs) const { return !(*this == rhs); }
};

struct texcoord2d {
  double s, t;
  double operator[](size_t idx) const { return *(&s + idx); }
  double &operator[](size_t idx) { return *(&s + idx); }
  bool operator==(const texcoord2d &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const texcoord2d &rhs) const { return !(*this == rhs); }
};

struct texcoord3h {
  half s, t, r;
  half operator[](size_t idx) const { return *(&s + idx); }
  half &operator[](size_t idx) { return *(&s + idx); }
  bool operator==(const texcoord3h &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const texcoord3h &rhs) const { return !(*this == rhs); }
};

struct texcoord3f {
  float s, t, r;
  float operator[](size_t idx) const { return *(&s + idx); }
  float &operator[](size_t idx) { return *(&s + idx); }
  bool operator==(const texcoord3f &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const texcoord3f &rhs) const { return !(*this == rhs); }
};

struct texcoord3d {
  double s, t, r;
  double operator[](size_t idx) const { return *(&s + idx); }
  double &operator[](size_t idx) { return *(&s + idx); }
  bool operator==(const texcoord3d &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const texcoord3d &rhs) const { return !(*this == rhs); }
};



// Attribute value Block(`None`)
struct ValueBlock {};

// Attribute animation Block(`AnimationBlock`). Blocks spline/timeSamples on
// weaker layers while letting the default value resolve (SdfAnimationBlock).
struct AnimationBlock {};

using double2 = std::array<double, 2>;
using double3 = std::array<double, 3>;
using double4 = std::array<double, 4>;

using dict = std::map<std::string, any_value>;

template <class dtype>
struct TypeTraits;

template <typename T>
inline constexpr bool is_binary_serializable_v =
    std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

template <typename T>
using timesample_storage_type_t = typename std::decay<T>::type;

template <typename T>
inline constexpr bool uses_binary_timesample_scalar_storage_v =
    is_binary_serializable_v<timesample_storage_type_t<T>> &&
    !std::is_same_v<timesample_storage_type_t<T>, bool> &&
    // uchar (uint8_t) uses generic value::Value timesample storage, like bool —
    // the binary flat-storage reconstruction path does not handle the 1-byte
    // element type, so route it through the generic path for a correct roundtrip.
    !std::is_same_v<timesample_storage_type_t<T>, uint8_t>;

template <typename T>
inline constexpr bool uses_binary_timesample_array_storage_v =
    uses_binary_timesample_scalar_storage_v<T>;

// import DEFINE_TYPE_TRAIT and DEFINE_ROLE_TYPE_TRAIT
#include "define-type-trait.inc"

// `void` hash no sizeof(void), so define it manually.
template <>
struct TypeTraits<void> {
  using value_type = void;
  using value_underlying_type = void;
  static constexpr uint32_t ndim() { return 0; } /* array dim */
  static constexpr uint32_t size = 0; /* zero for void  */
  static constexpr uint32_t ncomp() { return 0; }
  static constexpr uint32_t type_id() { return TYPE_ID_VOID; }
  static constexpr uint32_t get_type_id() { return TYPE_ID_VOID; }
  static constexpr uint32_t underlying_type_id() { return TYPE_ID_VOID; }
  static std::string type_name() { return "void"; }
  static std::string underlying_type_name() { return "void"; }
  static bool is_role_type() { return false; }
  static bool is_array() { return false; }
};

// Specialization for const char* to support string literals
template <>
struct TypeTraits<const char*> {
  using value_type = const char*;
  using value_underlying_type = const char*;
  static constexpr uint32_t ndim() { return 0; }
  static constexpr uint32_t size = sizeof(const char*);
  static constexpr uint32_t ncomp() { return 1; }
  static constexpr uint32_t type_id() { return TYPE_ID_STRING; }
  static constexpr uint32_t get_type_id() { return TYPE_ID_STRING; }
  static constexpr uint32_t underlying_type_id() { return TYPE_ID_STRING; }
  static std::string type_name() { return kString; }
  static std::string underlying_type_name() { return kString; }
  static bool is_role_type() { return false; }
  static bool is_array() { return false; }
};

DEFINE_TYPE_TRAIT(std::nullptr_t, "null", TYPE_ID_NULL, 1);
// DEFINE_TYPE_TRAIT(void, "void", TYPE_ID_VOID, 1);
DEFINE_TYPE_TRAIT(ValueBlock, "None", TYPE_ID_VALUEBLOCK, 1);
DEFINE_TYPE_TRAIT(AnimationBlock, "AnimationBlock", TYPE_ID_ANIMATION_BLOCK, 1);

DEFINE_TYPE_TRAIT(bool, kBool, TYPE_ID_BOOL, 1);
DEFINE_TYPE_TRAIT(uint8_t, kUChar, TYPE_ID_UCHAR, 1);
DEFINE_TYPE_TRAIT(half, kHalf, TYPE_ID_HALF, 1);

DEFINE_TYPE_TRAIT(int16_t, kShort, TYPE_ID_SHORT, 1);
DEFINE_TYPE_TRAIT(uint16_t, kUShort, TYPE_ID_USHORT, 1);

DEFINE_TYPE_TRAIT(int32_t, kInt, TYPE_ID_INT32, 1);
DEFINE_TYPE_TRAIT(uint32_t, kUInt, TYPE_ID_UINT32, 1);

DEFINE_TYPE_TRAIT(int64_t, kInt64, TYPE_ID_INT64, 1);
DEFINE_TYPE_TRAIT(uint64_t, kUInt64, TYPE_ID_UINT64, 1);

DEFINE_TYPE_TRAIT(char, kChar, TYPE_ID_CHAR, 1);
DEFINE_TYPE_TRAIT(char2, kChar2, TYPE_ID_CHAR2, 2);
DEFINE_TYPE_TRAIT(char3, kChar3, TYPE_ID_CHAR3, 3);
DEFINE_TYPE_TRAIT(char4, kChar4, TYPE_ID_CHAR4, 4);

DEFINE_TYPE_TRAIT(uchar2, kUChar2, TYPE_ID_UCHAR2, 2);
DEFINE_TYPE_TRAIT(uchar3, kUChar3, TYPE_ID_UCHAR3, 3);
DEFINE_TYPE_TRAIT(uchar4, kUChar4, TYPE_ID_UCHAR4, 4);

DEFINE_TYPE_TRAIT(short2, kShort2, TYPE_ID_SHORT2, 2);
DEFINE_TYPE_TRAIT(short3, kShort3, TYPE_ID_SHORT3, 3);
DEFINE_TYPE_TRAIT(short4, kShort4, TYPE_ID_SHORT4, 4);

DEFINE_TYPE_TRAIT(ushort2, kUShort2, TYPE_ID_USHORT2, 2);
DEFINE_TYPE_TRAIT(ushort3, kUShort3, TYPE_ID_USHORT3, 3);
DEFINE_TYPE_TRAIT(ushort4, kUShort4, TYPE_ID_USHORT4, 4);

DEFINE_TYPE_TRAIT(int2, kInt2, TYPE_ID_INT2, 2);
DEFINE_TYPE_TRAIT(int3, kInt3, TYPE_ID_INT3, 3);
DEFINE_TYPE_TRAIT(int4, kInt4, TYPE_ID_INT4, 4);

DEFINE_TYPE_TRAIT(uint2, kUInt2, TYPE_ID_UINT2, 2);
DEFINE_TYPE_TRAIT(uint3, kUInt3, TYPE_ID_UINT3, 3);
DEFINE_TYPE_TRAIT(uint4, kUInt4, TYPE_ID_UINT4, 4);

DEFINE_TYPE_TRAIT(half2, kHalf2, TYPE_ID_HALF2, 2);
DEFINE_TYPE_TRAIT(half3, kHalf3, TYPE_ID_HALF3, 3);
DEFINE_TYPE_TRAIT(half4, kHalf4, TYPE_ID_HALF4, 4);

DEFINE_TYPE_TRAIT(float, kFloat, TYPE_ID_FLOAT, 1);
DEFINE_TYPE_TRAIT(float2, kFloat2, TYPE_ID_FLOAT2, 2);
DEFINE_TYPE_TRAIT(float3, kFloat3, TYPE_ID_FLOAT3, 3);
DEFINE_TYPE_TRAIT(float4, kFloat4, TYPE_ID_FLOAT4, 4);

DEFINE_TYPE_TRAIT(double, kDouble, TYPE_ID_DOUBLE, 1);
DEFINE_TYPE_TRAIT(double2, kDouble2, TYPE_ID_DOUBLE2, 2);
DEFINE_TYPE_TRAIT(double3, kDouble3, TYPE_ID_DOUBLE3, 3);
DEFINE_TYPE_TRAIT(double4, kDouble4, TYPE_ID_DOUBLE4, 4);

DEFINE_TYPE_TRAIT(quath, kQuath, TYPE_ID_QUATH, 1);
DEFINE_TYPE_TRAIT(quatf, kQuatf, TYPE_ID_QUATF, 1);
DEFINE_TYPE_TRAIT(quatd, kQuatd, TYPE_ID_QUATD, 1);

DEFINE_TYPE_TRAIT(matrix2f, kMatrix2f, TYPE_ID_MATRIX2F, 1);
DEFINE_TYPE_TRAIT(matrix3f, kMatrix3f, TYPE_ID_MATRIX3F, 1);
DEFINE_TYPE_TRAIT(matrix4f, kMatrix4f, TYPE_ID_MATRIX4F, 1);

DEFINE_TYPE_TRAIT(matrix2d, kMatrix2d, TYPE_ID_MATRIX2D, 1);
DEFINE_TYPE_TRAIT(matrix3d, kMatrix3d, TYPE_ID_MATRIX3D, 1);
DEFINE_TYPE_TRAIT(matrix4d, kMatrix4d, TYPE_ID_MATRIX4D, 1);

DEFINE_TYPE_TRAIT(timecode, kTimeCode, TYPE_ID_TIMECODE, 1);

//
// Role types
//
DEFINE_ROLE_TYPE_TRAIT(vector3h, kVector3h, TYPE_ID_VECTOR3H, half3);
DEFINE_ROLE_TYPE_TRAIT(vector3f, kVector3f, TYPE_ID_VECTOR3F, float3);
DEFINE_ROLE_TYPE_TRAIT(vector3d, kVector3d, TYPE_ID_VECTOR3D, double3);

DEFINE_ROLE_TYPE_TRAIT(normal3h, kNormal3h, TYPE_ID_NORMAL3H, half3);
DEFINE_ROLE_TYPE_TRAIT(normal3f, kNormal3f, TYPE_ID_NORMAL3F, float3);
DEFINE_ROLE_TYPE_TRAIT(normal3d, kNormal3d, TYPE_ID_NORMAL3D, double3);

DEFINE_ROLE_TYPE_TRAIT(point3h, kPoint3h, TYPE_ID_POINT3H, half3);
DEFINE_ROLE_TYPE_TRAIT(point3f, kPoint3f, TYPE_ID_POINT3F, float3);
DEFINE_ROLE_TYPE_TRAIT(point3d, kPoint3d, TYPE_ID_POINT3D, double3);

DEFINE_ROLE_TYPE_TRAIT(frame4d, kFrame4d, TYPE_ID_FRAME4D, matrix4d);

DEFINE_ROLE_TYPE_TRAIT(color3h, kColor3h, TYPE_ID_COLOR3H, half3);
DEFINE_ROLE_TYPE_TRAIT(color4h, kColor4h, TYPE_ID_COLOR4H, half4);
DEFINE_ROLE_TYPE_TRAIT(color3f, kColor3f, TYPE_ID_COLOR3F, float3);
DEFINE_ROLE_TYPE_TRAIT(color4f, kColor4f, TYPE_ID_COLOR4F, float4);
DEFINE_ROLE_TYPE_TRAIT(color3d, kColor3d, TYPE_ID_COLOR3D, double3);
DEFINE_ROLE_TYPE_TRAIT(color4d, kColor4d, TYPE_ID_COLOR4D, double4);

DEFINE_ROLE_TYPE_TRAIT(texcoord2h, kTexCoord2h, TYPE_ID_TEXCOORD2H, half2);
DEFINE_ROLE_TYPE_TRAIT(texcoord2f, kTexCoord2f, TYPE_ID_TEXCOORD2F, float2);
DEFINE_ROLE_TYPE_TRAIT(texcoord2d, kTexCoord2d, TYPE_ID_TEXCOORD2D, double2);

DEFINE_ROLE_TYPE_TRAIT(texcoord3h, kTexCoord3h, TYPE_ID_TEXCOORD3H, half3);
DEFINE_ROLE_TYPE_TRAIT(texcoord3f, kTexCoord3f, TYPE_ID_TEXCOORD3F, float3);
DEFINE_ROLE_TYPE_TRAIT(texcoord3d, kTexCoord3d, TYPE_ID_TEXCOORD3D, double3);

//
//
//

DEFINE_TYPE_TRAIT(token, kToken, TYPE_ID_TOKEN, 1);
DEFINE_TYPE_TRAIT(std::string, kString, TYPE_ID_STRING, 1);
DEFINE_TYPE_TRAIT(StringData, kString, TYPE_ID_STRING_DATA, 1);
DEFINE_TYPE_TRAIT(dict, kDictionary, TYPE_ID_DICT, 1);

DEFINE_TYPE_TRAIT(AssetPath, kAssetPath, TYPE_ID_ASSET_PATH, 1);
DEFINE_TYPE_TRAIT(PathExpression, kPathExpression, TYPE_ID_PATH_EXPRESSION, 1);

//
// Other types(e.g. TYPE_ID_REFERENCE) are defined in corresponding header
// files(e.g. core/ headers, `crate-format.hh`(Data types used in Crate data))
//
// token[] and TimeSamples traits are defined later in this file (after
// TimeSamples struct is available from timesamples.hh)
//

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

// 1D Array
template <typename T>
struct TypeTraits<std::vector<T>> {
  using value_type = std::vector<T>;
  static constexpr uint32_t ndim() { return 1; } /* array dim */
  static constexpr uint32_t ncomp() { return TypeTraits<T>::ncomp(); }
  // Return the size of base type
  static constexpr size_t size() { return TypeTraits<T>::size(); }
  static constexpr uint32_t type_id() { return
      TypeTraits<T>::type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static constexpr uint32_t get_type_id() {
      return TypeTraits<T>::type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static constexpr uint32_t underlying_type_id() {
      return TypeTraits<T>::underlying_type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static std::string type_name() { return TypeTraits<T>::type_name() + "[]"; }
  static std::string underlying_type_name() {
    return TypeTraits<T>::underlying_type_name() + "[]";
  }
  static constexpr bool is_role_type() { return TypeTraits<T>::is_role_type(); }
  static constexpr bool is_array() { return true; }
};

template <typename T>
struct TypeTraits<TypedArray<T>> {
  using value_type = TypedArray<T>;
  static constexpr uint32_t ndim() { return 1; } /* array dim */
  static constexpr uint32_t ncomp() { return TypeTraits<T>::ncomp(); }
  // Return the size of base type
  static constexpr size_t size() { return TypeTraits<T>::size(); }
  static constexpr uint32_t type_id() { return
      TypeTraits<T>::type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static constexpr uint32_t get_type_id() {
      return TypeTraits<T>::type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static constexpr uint32_t underlying_type_id() {
      return TypeTraits<T>::underlying_type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static std::string type_name() { return TypeTraits<T>::type_name() + "[]"; }
  static std::string underlying_type_name() {
    return TypeTraits<T>::underlying_type_name() + "[]";
  }
  static constexpr bool is_role_type() { return TypeTraits<T>::is_role_type(); }
  static constexpr bool is_array() { return true; }
};

template <typename T>
struct TypeTraits<ChunkedTypedArray<T>> {
  using value_type = TypedArray<T>;
  static constexpr uint32_t ndim() { return 1; } /* array dim */
  static constexpr uint32_t ncomp() { return TypeTraits<T>::ncomp(); }
  // Return the size of base type
  static constexpr size_t size() { return TypeTraits<T>::size(); }
  static constexpr uint32_t type_id() { return
      TypeTraits<T>::type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static constexpr uint32_t get_type_id() {
      return TypeTraits<T>::type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static constexpr uint32_t underlying_type_id() {
      return TypeTraits<T>::underlying_type_id() | TYPE_ID_1D_ARRAY_BIT; }
  static std::string type_name() { return TypeTraits<T>::type_name() + "[]"; }
  static std::string underlying_type_name() {
    return TypeTraits<T>::underlying_type_name() + "[]";
  }
  static constexpr bool is_role_type() { return TypeTraits<T>::is_role_type(); }
  static constexpr bool is_array() { return true; }
};



// Lookup TypeTraits<T>::type_name from type_id
// Return nullopt when the input is invalid type id.
nonstd::optional<std::string> TryGetTypeName(uint32_t tyid);

// Return error string when the input is invalid type id
std::string GetTypeName(uint32_t tyid);

// Lookup TypeTraits<T>::type_id from string
// Return nullopt when the input is invalid type name.
nonstd::optional<uint32_t> TryGetTypeId(const std::string &tyname);

// Return TYPE_ID_INVALID when the input is invalid type name
uint32_t GetTypeId(const std::string &tyname);

// For Role type.
// Get underlying type name(e.g. return type "float4" for role type "color4f"),
// or return nullopt/invalid string for invalid input type id. For non-Role
// type, the behavior is same with TryGetTypeName/GetTypeName(i.e, return
// "float4" for type `float4`)
nonstd::optional<std::string> TryGetUnderlyingTypeName(uint32_t tyid);
std::string GetUnderlyingTypeName(uint32_t tyid);

// Get underlying type id(e.g. return type "float4" for role type "color4f"), or
// return nullopt/TYPE_ID_INVALID for invalid input type name For non-Role type,
// the behavior is same with TryGetTypeId/GetTypeId(i.e, return `float4` for
// name "float4")
nonstd::optional<uint32_t> TryGetUnderlyingTypeId(const std::string &tyname);
uint32_t GetUnderlyingTypeId(const std::string &tyname);

constexpr inline uint32_t GetUnderlyingTypeId(const uint32_t tyid) {
  const bool is_array = (tyid & TYPE_ID_1D_ARRAY_BIT) != 0;
  const uint32_t base_type_id = tyid & (~TYPE_ID_1D_ARRAY_BIT);

  uint32_t underlying = base_type_id;
  switch (base_type_id) {
    case TYPE_ID_POINT3H: underlying = TYPE_ID_HALF3; break;
    case TYPE_ID_POINT3F: underlying = TYPE_ID_FLOAT3; break;
    case TYPE_ID_POINT3D: underlying = TYPE_ID_DOUBLE3; break;
    case TYPE_ID_VECTOR3H: underlying = TYPE_ID_HALF3; break;
    case TYPE_ID_VECTOR3F: underlying = TYPE_ID_FLOAT3; break;
    case TYPE_ID_VECTOR3D: underlying = TYPE_ID_DOUBLE3; break;
    case TYPE_ID_NORMAL3H: underlying = TYPE_ID_HALF3; break;
    case TYPE_ID_NORMAL3F: underlying = TYPE_ID_FLOAT3; break;
    case TYPE_ID_NORMAL3D: underlying = TYPE_ID_DOUBLE3; break;
    case TYPE_ID_COLOR3H: underlying = TYPE_ID_HALF3; break;
    case TYPE_ID_COLOR3F: underlying = TYPE_ID_FLOAT3; break;
    case TYPE_ID_COLOR3D: underlying = TYPE_ID_DOUBLE3; break;
    case TYPE_ID_COLOR4H: underlying = TYPE_ID_HALF4; break;
    case TYPE_ID_COLOR4F: underlying = TYPE_ID_FLOAT4; break;
    case TYPE_ID_COLOR4D: underlying = TYPE_ID_DOUBLE4; break;
    case TYPE_ID_TEXCOORD2H: underlying = TYPE_ID_HALF2; break;
    case TYPE_ID_TEXCOORD2F: underlying = TYPE_ID_FLOAT2; break;
    case TYPE_ID_TEXCOORD2D: underlying = TYPE_ID_DOUBLE2; break;
    case TYPE_ID_TEXCOORD3H: underlying = TYPE_ID_HALF3; break;
    case TYPE_ID_TEXCOORD3F: underlying = TYPE_ID_FLOAT3; break;
    case TYPE_ID_TEXCOORD3D: underlying = TYPE_ID_DOUBLE3; break;
    case TYPE_ID_FRAME4D: underlying = TYPE_ID_MATRIX4D; break;
    default: break;
  }

  return is_array ? (underlying | TYPE_ID_1D_ARRAY_BIT) : underlying;
}

constexpr inline bool UsesBinaryTimesampleStorageType(const uint32_t tyid) {
  switch (GetUnderlyingTypeId(tyid) & (~TYPE_ID_1D_ARRAY_BIT)) {
    case TYPE_ID_BOOL:
      return false;
    case TYPE_ID_CHAR:
    case TYPE_ID_CHAR2:
    case TYPE_ID_CHAR3:
    case TYPE_ID_CHAR4:
    case TYPE_ID_UCHAR:
    case TYPE_ID_UCHAR2:
    case TYPE_ID_UCHAR3:
    case TYPE_ID_UCHAR4:
    case TYPE_ID_SHORT:
    case TYPE_ID_SHORT2:
    case TYPE_ID_SHORT3:
    case TYPE_ID_SHORT4:
    case TYPE_ID_USHORT:
    case TYPE_ID_USHORT2:
    case TYPE_ID_USHORT3:
    case TYPE_ID_USHORT4:
    case TYPE_ID_INT32:
    case TYPE_ID_INT2:
    case TYPE_ID_INT3:
    case TYPE_ID_INT4:
    case TYPE_ID_UINT32:
    case TYPE_ID_UINT2:
    case TYPE_ID_UINT3:
    case TYPE_ID_UINT4:
    case TYPE_ID_INT64:
    case TYPE_ID_UINT64:
    case TYPE_ID_HALF:
    case TYPE_ID_HALF2:
    case TYPE_ID_HALF3:
    case TYPE_ID_HALF4:
    case TYPE_ID_FLOAT:
    case TYPE_ID_FLOAT2:
    case TYPE_ID_FLOAT3:
    case TYPE_ID_FLOAT4:
    case TYPE_ID_DOUBLE:
    case TYPE_ID_DOUBLE2:
    case TYPE_ID_DOUBLE3:
    case TYPE_ID_DOUBLE4:
    case TYPE_ID_QUATH:
    case TYPE_ID_QUATF:
    case TYPE_ID_QUATD:
    case TYPE_ID_MATRIX2F:
    case TYPE_ID_MATRIX3F:
    case TYPE_ID_MATRIX4F:
    case TYPE_ID_MATRIX2D:
    case TYPE_ID_MATRIX3D:
    case TYPE_ID_MATRIX4D:
      return true;
    default:
      return false;
  }
}

constexpr inline bool IsBinarySerializableType(const uint32_t tyid) {
  return UsesBinaryTimesampleStorageType(tyid);
}

/// @brief Check if given typeName string is a role-type(e.g. "vector3f")
/// @param[in] tyname typeName string
/// @return true if a type is role-type.
bool IsRoleType(const std::string &tyname);

/// @brief Check if given type_id is a role-type(e.g. "vector3f")
/// @param[in] tyid type id(value::TYPE_ID_***)
/// @return true if a type is role-type.
bool IsRoleType(const uint32_t tyid);

bool IsReasonableValueVectorSize(size_t size);

}  // namespace value
}  // namespace lightusd

namespace lightusd {
namespace value {

///
/// any_value — Purpose-built type-erased container for USD value types.
///
/// Replaces linb::any with a design tailored for LightUSD:
/// - 16-byte SBO buffer: scalars/float3 and smaller stay inline; everything
///   bigger (strings, vectors, matrices) lives in a refcounted heap holder
///   shared copy-on-write between copies — sizeof(any_value) = 24, and
///   copying a Value is O(1) for all heap-stored payloads
/// - A single shared per-type descriptor pointer (TypeDesc*): destroy/copy/move
///   ops plus type-id/underlying-id/name/sizeof/is_inline. The descriptor is one
///   static instance per stored type (vague linkage), so each any_value holds only
///   { 16-byte buffer + one pointer } = 24 bytes,
///   and copy/move just propagate the pointer instead of seven fields.
///
class any_value {
 public:
  any_value() noexcept = default;
  ~any_value() { destroy(); }

  any_value(const any_value& other) : desc_(other.desc_) {
    if (desc_) {
      desc_->ops.copy(&storage_, &other.storage_);
    }
  }

  any_value(any_value&& other) noexcept : desc_(other.desc_) {
    if (desc_) {
      desc_->ops.move(&storage_, &other.storage_);
      other.desc_ = nullptr;
    }
  }

  any_value& operator=(const any_value& other) {
    if (this != &other) {
      any_value tmp(other);
      swap(tmp);
    }
    return *this;
  }

  any_value& operator=(any_value&& other) noexcept {
    if (this != &other) {
      destroy();
      desc_ = other.desc_;
      if (desc_) {
        desc_->ops.move(&storage_, &other.storage_);
        other.desc_ = nullptr;
      }
    }
    return *this;
  }

  // Typed constructor
  template <typename T,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<T>::type, any_value>::value>::type>
  any_value(T&& val) {
    construct(std::forward<T>(val));
  }

  // Typed assignment
  template <typename T,
            typename = typename std::enable_if<
                !std::is_same<typename std::decay<T>::type, any_value>::value>::type>
  any_value& operator=(T&& val) {
    any_value tmp(std::forward<T>(val));
    swap(tmp);
    return *this;
  }

  // Type queries
  uint32_t type_id() const noexcept { return desc_ ? desc_->type_id : 0; }
  uint32_t underlying_type_id() const noexcept {
    return desc_ ? desc_->underlying_type_id : 0;
  }
  bool empty() const noexcept { return desc_ == nullptr; }

  const std::string type_name() const noexcept {
    if (!desc_) return TypeTraits<void>::type_name();
    return desc_->type_name_fn();
  }

  const std::string underlying_type_name() const noexcept {
    if (!desc_) return TypeTraits<void>::underlying_type_name();
    return desc_->underlying_type_name_fn();
  }

  void clear() noexcept {
    destroy();
    desc_ = nullptr;
  }

  void swap(any_value& other) noexcept {
    // Simple swap via moves through a temporary
    any_value tmp(std::move(other));
    other = std::move(*this);
    *this = std::move(tmp);
  }

  /// Reinterpret as a different type without copying (for role type casting).
  /// Caller must ensure NewType has identical memory layout to the current type.
  /// The descriptor swap is sound because layout-compatible types (role <->
  /// underlying, guaranteed by DEFINE_ROLE_TYPE_TRAIT's size/align static_asserts)
  /// have byte-identical destroy/copy/move ops and identical is_inline.
  template <typename NewType>
  void unsafe_reinterpret_as() noexcept {
    if (!empty()) {
      desc_ = desc_for<NewType>();
    }
  }

  /// Raw pointer to stored value (no type check). Const version.
  template <typename T>
  const T* cast() const noexcept {
    if (desc_ && desc_->is_inline) {
      return reinterpret_cast<const T*>(&storage_);
    }
    const SharedHolder<T>* h =
        *reinterpret_cast<const SharedHolder<T>* const*>(&storage_);
    return h ? &h->value : nullptr;
  }

  /// Raw pointer to stored value (no type check). Mutable version.
  /// Copy-on-write: heap-stored payloads are shared between copies, so
  /// mutable access detaches (clones) when the payload is shared.
  template <typename T>
  T* cast() {
    if (desc_ && desc_->is_inline) {
      return reinterpret_cast<T*>(&storage_);
    }
    if (desc_) {
      desc_->ops.detach(&storage_);
    }
    SharedHolder<T>* h = *reinterpret_cast<SharedHolder<T>**>(&storage_);
    return h ? &h->value : nullptr;
  }

 private:
  static constexpr size_t kBufferSize = 16;
  static constexpr size_t kBufferAlign = 8;

  using name_fn_t = std::string (*)();

  // Refcounted holder for heap-stored payloads. Copies of an any_value share
  // one holder (copy-on-write); mutable access detaches when shared. The
  // refcount is atomic so const copies/destroys are thread-safe; mutating the
  // SAME any_value from multiple threads is not (and never was) supported.
  //
  // Layout note: role <-> underlying reinterpretation (unsafe_reinterpret_as /
  // any_value_raw_cast) relies on SharedHolder<Role> and SharedHolder<Under>
  // being layout-identical, which holds because the wrapped types are
  // static_asserted to have identical size/alignment.
  template <typename T>
  struct SharedHolder {
    std::atomic<uint32_t> refs{1};
    T value;

    SharedHolder(const T& v) : value(v) {}
    SharedHolder(T&& v) noexcept : value(std::move(v)) {}
  };

  // Per-type descriptor: a single shared static instance per stored type (vague
  // linkage), pointed to by every any_value of that type. Holds the type-erased
  // ops plus the metadata that used to be duplicated in every instance.
  struct TypeDesc {
    struct {
      void (*destroy)(void* storage) noexcept;
      void (*copy)(void* dst, const void* src);
      void (*move)(void* dst, void* src) noexcept;
      // Clone the payload when shared (copy-on-write detach). No-op for
      // inline storage and for uniquely-owned holders.
      void (*detach)(void* storage);
    } ops;
    name_fn_t type_name_fn;
    name_fn_t underlying_type_name_fn;
    uint32_t type_id;
    uint32_t underlying_type_id;
    uint32_t sizeof_stored;  // sizeof(T) of the stored type
    bool is_inline;
  };

  // Inline storage ops (T fits in buffer)
  template <typename T>
  struct InlineOps {
    static void destroy(void* s) noexcept {
      static_cast<T*>(s)->~T();
    }
    static void copy(void* d, const void* s) {
      new (d) T(*static_cast<const T*>(s));
    }
    static void move(void* d, void* s) noexcept {
      new (d) T(std::move(*static_cast<T*>(s)));
      static_cast<T*>(s)->~T();
    }
    static void detach(void*) {}  // inline storage is never shared
  };

  // Heap storage ops (T too large for buffer): refcounted copy-on-write.
  template <typename T>
  struct HeapOps {
    using Holder = SharedHolder<T>;
    static void destroy(void* s) noexcept {
      Holder* h = *static_cast<Holder**>(s);
      if (h && h->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete h;
      }
    }
    static void copy(void* d, const void* s) {
      Holder* h = *static_cast<Holder* const*>(s);
      if (h) {
        h->refs.fetch_add(1, std::memory_order_relaxed);
      }
      *static_cast<Holder**>(d) = h;
    }
    static void move(void* d, void* s) noexcept {
      *static_cast<Holder**>(d) = *static_cast<Holder**>(s);
      *static_cast<Holder**>(s) = nullptr;
    }
    static void detach(void* s) {
      Holder* h = *static_cast<Holder**>(s);
      if (h && h->refs.load(std::memory_order_acquire) > 1) {
        Holder* fresh = new Holder(h->value);
        if (h->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          // Raced with the last other owner releasing; we own the original
          // again, but `fresh` is already built — keep it, drop the original.
          delete h;
        }
        *static_cast<Holder**>(s) = fresh;
      }
    }
  };

  template <typename T>
  struct is_vector_like : std::false_type {};
  template <typename U, typename A>
  struct is_vector_like<std::vector<U, A>> : std::true_type {};

  template <typename T>
  static constexpr bool fits_inline() {
    // Arrays (std::vector control blocks) FIT the buffer but their inline
    // copy op deep-copies the elements; route them through the refcounted
    // heap path instead so any_value copies share the (potentially huge)
    // element storage copy-on-write.
    return sizeof(T) <= kBufferSize &&
           alignof(T) <= kBufferAlign &&
           std::is_nothrow_move_constructible<T>::value &&
           !is_vector_like<T>::value;
  }

  // The single shared descriptor for stored type T.
  template <typename T>
  static const TypeDesc* desc_for() {
    using DecayT = typename std::decay<T>::type;
    using OpsType = typename std::conditional<fits_inline<DecayT>(),
                                               InlineOps<DecayT>,
                                               HeapOps<DecayT>>::type;
    static const TypeDesc desc = {
        {OpsType::destroy, OpsType::copy, OpsType::move, OpsType::detach},
        &TypeTraits<DecayT>::type_name,
        &TypeTraits<DecayT>::underlying_type_name,
        TypeTraits<DecayT>::type_id(),
        TypeTraits<DecayT>::underlying_type_id(),
        static_cast<uint32_t>(sizeof(DecayT)),
        fits_inline<DecayT>(),
    };
    return &desc;
  }

  template <typename ValueType, typename T>
  typename std::enable_if<fits_inline<T>()>::type
  do_construct(ValueType&& val) {
    new (&storage_) T(std::forward<ValueType>(val));
  }

  template <typename ValueType, typename T>
  typename std::enable_if<!fits_inline<T>()>::type
  do_construct(ValueType&& val) {
    *reinterpret_cast<SharedHolder<T>**>(&storage_) =
        new SharedHolder<T>(std::forward<ValueType>(val));
  }

  template <typename ValueType>
  void construct(ValueType&& val) {
    using T = typename std::decay<ValueType>::type;
    static_assert(std::is_copy_constructible<T>::value,
                  "T shall satisfy the CopyConstructible requirements.");
    desc_ = desc_for<T>();
    do_construct<ValueType, T>(std::forward<ValueType>(val));
  }

  void destroy() noexcept {
    if (desc_) {
      desc_->ops.destroy(&storage_);
    }
  }

  typename std::aligned_storage<kBufferSize, kBufferAlign>::type storage_{};
  const TypeDesc* desc_ = nullptr;

 public:
  /// Return sizeof(T) of the stored type.
  /// This is the shallow size of the concrete object, not including heap allocations.
  size_t sizeof_stored() const { return desc_ ? desc_->sizeof_stored : 0; }
};

// Type-checked cast (returns nullptr on type mismatch)
template <typename T>
inline const T* any_value_cast(const any_value* av) noexcept {
  using DecayT = typename std::decay<T>::type;
  if (av && av->type_id() == TypeTraits<DecayT>::type_id()) {
    return av->cast<T>();
  }
  return nullptr;
}

template <typename T>
inline T* any_value_cast(any_value* av) noexcept {
  using DecayT = typename std::decay<T>::type;
  if (av && av->type_id() == TypeTraits<DecayT>::type_id()) {
    return av->cast<T>();
  }
  return nullptr;
}

// Internal force-cast (no type-id check) — equivalent to linb::cast. Confined to
// `detail` so it cannot be reached without intent: it is only sound when the
// requested T is layout-compatible with the stored type (e.g. role <-> underlying,
// which the DEFINE_ROLE_TYPE_TRAIT static_asserts now guarantee). Every caller
// must first gate it with an underlying_type_id() equality check (see Value::as<T>).
namespace detail {
template <typename T>
inline const T* any_value_raw_cast(const any_value* av) noexcept {
  return av->cast<T>();
}

template <typename T>
inline T* any_value_raw_cast(any_value* av) noexcept {
  return av->cast<T>();
}
}  // namespace detail

///
/// Generic Value class using any_value
///
class Value {
 public:
  Value() {
    //LIGHTUSD_LOG_I("Value default constructor called");
  }

  // Copy constructor
  Value(const Value& rhs) : v_(rhs.v_) {
    //LIGHTUSD_LOG_I("Value copy constructor called");
  }

  // Move constructor
  Value(Value&& rhs) noexcept : v_(std::move(rhs.v_)) {
    //LIGHTUSD_LOG_I("Value move constructor called");
  }

  template <class T>
  Value(const T &v) : v_(v) {
  }

  // Perfect-forwarding constructor. Use std::forward (NOT std::move): since
  // `T&&` is a forwarding reference, an unconditional std::move here would steal
  // from a non-const lvalue, e.g. `std::vector<uint8_t> a{...}; Value(a);` would
  // silently empty `a`. std::forward moves only genuine rvalues; lvalues copy.
  // (Callers that intend a move pass `Value(std::move(x))`.)
  template <class T>
  Value(T &&v) noexcept : v_(std::forward<T>(v)) {
  }

  // template <class T>
  // Value(T &&v) : v_(v) {}

  const std::string type_name() const { return v_.type_name(); }
  const std::string underlying_type_name() const {
    return v_.underlying_type_name();
  }

  uint32_t type_id() const { return v_.type_id(); }
  uint32_t underlying_type_id() const { return v_.underlying_type_id(); }

  //
  // Cast value to given type.
  //
  // when `strict_cast` is false(default behavior), it supports casting type among role type and underlying type.
  // (e.g. "float3" -> "color3f", "color3f" -> "vector3f", "normal3f[]" -> "float3[]")
  //
  // Return nullptr when type conversion failed.
  template <class T>
  const T *as(bool strict_cast = false) const {
    if (TypeTraits<T>::type_id() == v_.type_id()) {
      return any_value_cast<const T>(&v_);
    } else if (!strict_cast) {
      if (TypeTraits<T>::is_array() && (v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are array type
        if ((TypeTraits<T>::underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT)) == (v_.underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT))) {
          return detail::any_value_raw_cast<const T>(&v_);
        }
      } else if (!TypeTraits<T>::is_array() && !(v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are scalar type.
        if (TypeTraits<T>::underlying_type_id() == v_.underlying_type_id()) {
          return detail::any_value_raw_cast<const T>(&v_);
        }
      }
    }

    return nullptr;
  }

  // Non const version of `as`.
  //
  // Return nullptr when type conversion failed.
  template <class T>
  T *as(bool strict_cast = false) {
    if (TypeTraits<T>::type_id() == v_.type_id()) {
      return any_value_cast<T>(&v_);
    } else if (!strict_cast) {
      if (TypeTraits<T>::is_array() && (v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are array type
        if ((TypeTraits<T>::underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT)) == (v_.underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT))) {
          return detail::any_value_raw_cast<T>(&v_);
        }
      } else if (!TypeTraits<T>::is_array() && !(v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are scalar type.
        if (TypeTraits<T>::underlying_type_id() == v_.underlying_type_id()) {
          return detail::any_value_raw_cast<T>(&v_);
        }
      }
    }

    return nullptr;
  }

  //
  // Get TypedArrayView to the underlying data.
  //
  // Returns a view over array data if the value contains an array type that's compatible
  // with the requested element type T. For non-array types, returns an empty view.
  //
  // The view provides zero-copy access to the underlying data with type safety validation.
  //
  // Template parameter T: the desired element type for the view
  // Parameter strict_cast: if true, requires exact type match; if false, allows compatible type casting
  //
  // Returns: TypedArrayView<T> - may be empty if type conversion is not possible
  //
  template <class T>
  TypedArrayView<const T> as_view(bool strict_cast = false) const {
    // Check if this is an array type
    if (!(v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) {
      // Not an array type - return empty view
      return TypedArrayView<const T>();
    }

    // For arrays, we need to check if we can safely view the underlying data as T
    uint32_t underlying_type_id = v_.underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT);
    uint32_t target_type_id = TypeTraits<T>::underlying_type_id();

    if (strict_cast) {
      // Strict cast - requires exact underlying type match
      if (underlying_type_id != target_type_id) {
        return TypedArrayView<const T>();
      }
    } else {
      // Non-strict cast - allow compatible types (same underlying type)
      if (underlying_type_id != target_type_id) {
        return TypedArrayView<const T>();
      }
    }

    // Try to get the array data and create a view
    return create_array_view_helper<T>(strict_cast);
  }

  //
  // Non-const version of as_view() for mutable access
  //
  template <class T>
  TypedArrayView<T> as_view(bool strict_cast = false) {
    // Check if this is an array type
    if (!(v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) {
      // Not an array type - return empty view
      return TypedArrayView<T>();
    }

    // For arrays, we need to check if we can safely view the underlying data as T
    uint32_t underlying_type_id = v_.underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT);
    uint32_t target_type_id = TypeTraits<T>::underlying_type_id();

    if (strict_cast) {
      // Strict cast - requires exact underlying type match
      if (underlying_type_id != target_type_id) {
        return TypedArrayView<T>();
      }
    } else {
      // Non-strict cast - allow compatible types (same underlying type)
      if (underlying_type_id != target_type_id) {
        return TypedArrayView<T>();
      }
    }

    // Try to get the array data and create a view
    return create_array_view_helper_mutable<T>(strict_cast);
  }




  // Helper to check vector size bounds
  template <typename T>
  static bool check_vector_size(const std::vector<T>& vec) {
    return IsReasonableValueVectorSize(vec.size());
  }

  template <typename T>
  static bool check_vector_size(const T&) {
    // Non-vector type, always OK
    return true;
  }

  // Type-safe way to get concrete value (const version — copies).
  template <class T>
  nonstd::optional<T> get_value(bool strict_cast = false) const {
    if (TypeTraits<T>::type_id() == v_.type_id()) {
      const T *pv = any_value_cast<const T>(&v_);
      if (!pv) {
        return nonstd::nullopt;
      }
      return *pv;
    } else if (!strict_cast) {

      if (TypeTraits<T>::is_array() && (v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are array type
        if ((TypeTraits<T>::underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT)) == (v_.underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT))) {
          const T* pv = detail::any_value_raw_cast<const T>(&v_);
          if (pv) {
            if (!check_vector_size(*pv)) {
              return nonstd::nullopt;
            }
          }
          return *pv;
        }
      } else if (!TypeTraits<T>::is_array() && !(v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are scalar type.
        if (TypeTraits<T>::underlying_type_id() == v_.underlying_type_id()) {
          return *detail::any_value_raw_cast<const T>(&v_);
        }
      }
    }
    return nonstd::nullopt;
  }

  // Type-safe way to extract concrete value (mutable version — moves, leaves Value empty).
  template <class T>
  nonstd::optional<T> take_value(bool strict_cast = false) {
    if (TypeTraits<T>::type_id() == v_.type_id()) {
      T *pv = any_value_cast<T>(&v_);
      if (!pv) {
        return nonstd::nullopt;
      }
      return std::move(*pv);
    } else if (!strict_cast) {

      if (TypeTraits<T>::is_array() && (v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are array type
        if ((TypeTraits<T>::underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT)) == (v_.underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT))) {
          T* pv = detail::any_value_raw_cast<T>(&v_);
          if (pv) {
            if (!check_vector_size(*pv)) {
              return nonstd::nullopt;
            }
          }
          return std::move(*pv);
        }
      } else if (!TypeTraits<T>::is_array() && !(v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT)) { // both are scalar type.
        if (TypeTraits<T>::underlying_type_id() == v_.underlying_type_id()) {
          return std::move(*detail::any_value_raw_cast<T>(&v_));
        }
      }
    }
    return nonstd::nullopt;
  }


  // Copy assignment operator
  Value& operator=(const Value& rhs) {
    //LIGHTUSD_LOG_I("Value copy assignment operator called");
    if (this != &rhs) {
      v_ = rhs.v_;
    }
    return *this;
  }

  // Move assignment operator
  Value& operator=(Value&& rhs) noexcept {
    //LIGHTUSD_LOG_I("Value move assignment operator called");
    if (this != &rhs) {
      v_ = std::move(rhs.v_);
    }
    return *this;
  }

  template <class T>
  Value &operator=(const T &v) {
    v_ = v;
    return (*this);
  }


  const any_value &get_raw() const LIGHTUSD_LIFETIMEBOUND { return v_; }
  any_value &get_raw_mutable() LIGHTUSD_LIFETIMEBOUND { return v_; }

  bool is_array() const { return (v_.type_id() & value::TYPE_ID_1D_ARRAY_BIT); }

  // return 0 for non array type.
  // This method is primaliry for Primvar types(`float[]`, `color3f[]`, ...)
  // It does not report non-Primvar types(e.g. `Reference`, `Xform`, `GeomMesh`,
  // ...)
  size_t array_size() const;

  bool is_empty() const { return v_.empty() || v_.type_id() == value::TYPE_ID_NULL; }

  bool is_none() const { return v_.type_id() == value::TYPE_ID_VALUEBLOCK; }

  size_t estimate_memory_usage() const;

  /// Estimate actual (size-based) memory usage.
  size_t estimate_actual_usage() const;

  /// Return sizeof(T) of the stored concrete type.
  /// This is the shallow struct size, not including heap allocations.
  size_t sizeof_stored() const { return v_.sizeof_stored(); }

 private:
  any_value v_;

  // Helper methods for as_view() implementation
  template <class T>
  TypedArrayView<const T> create_array_view_helper(bool strict_cast) const {
    // Try common array types that could contain T elements

    // Direct type match - try std::vector<T>
    if (auto* vec = as<std::vector<T>>(strict_cast)) {
      return TypedArrayView<const T>(*vec);
    }

    // Try related types based on underlying type compatibility
    if (!strict_cast) {
      // Handle role type conversions (e.g., float3 <-> vector3f <-> normal3f)
      uint32_t target_underlying_id = TypeTraits<T>::underlying_type_id();

      switch (target_underlying_id) {
        case TYPE_ID_FLOAT: {
          if (auto* vec = as<std::vector<float>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_DOUBLE: {
          if (auto* vec = as<std::vector<double>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_FLOAT3: {
          // Try float3, vector3f, normal3f, color3f, point3f
          if (auto* vec = as<std::vector<float3>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<vector3f>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<normal3f>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<color3f>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<point3f>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_FLOAT2: {
          // Try float2, texcoord2f
          if (auto* vec = as<std::vector<float2>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<texcoord2f>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_FLOAT4: {
          // Try float4, color4f
          if (auto* vec = as<std::vector<float4>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<color4f>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_INT32: {
          if (auto* vec = as<std::vector<int32_t>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_UINT32: {
          if (auto* vec = as<std::vector<uint32_t>>(false)) {
            return TypedArrayView<const T>(reinterpret_cast<const T*>(vec->data()), vec->size());
          }
          break;
        }
        default:
          break;
      }
    }

    // No compatible type found
    return TypedArrayView<const T>();
  }

  template <class T>
  TypedArrayView<T> create_array_view_helper_mutable(bool strict_cast) {
    // Try common array types that could contain T elements

    // Direct type match - try std::vector<T>
    if (auto* vec = as<std::vector<T>>(strict_cast)) {
      return TypedArrayView<T>(*vec);
    }

    // Try related types based on underlying type compatibility
    if (!strict_cast) {
      // Handle role type conversions (e.g., float3 <-> vector3f <-> normal3f)
      uint32_t target_underlying_id = TypeTraits<T>::underlying_type_id();

      switch (target_underlying_id) {
        case TYPE_ID_FLOAT: {
          if (auto* vec = as<std::vector<float>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_DOUBLE: {
          if (auto* vec = as<std::vector<double>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_FLOAT3: {
          // Try float3, vector3f, normal3f, color3f, point3f
          if (auto* vec = as<std::vector<float3>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<vector3f>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<normal3f>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<color3f>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<point3f>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_FLOAT2: {
          // Try float2, texcoord2f
          if (auto* vec = as<std::vector<float2>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<texcoord2f>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_FLOAT4: {
          // Try float4, color4f
          if (auto* vec = as<std::vector<float4>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          if (auto* vec = as<std::vector<color4f>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_INT32: {
          if (auto* vec = as<std::vector<int32_t>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          break;
        }
        case TYPE_ID_UINT32: {
          if (auto* vec = as<std::vector<uint32_t>>(false)) {
            return TypedArrayView<T>(reinterpret_cast<T*>(vec->data()), vec->size());
          }
          break;
        }
        default:
          break;
      }
    }

    // No compatible type found
    return TypedArrayView<T>();
  }

};

///
/// ValueView - A compact view to typed data
///
/// ValueView provides a lightweight, non-owning view over typed data with
/// inline type information. The view stores a pointer, type_id, and flags
/// in a compact representation for efficient memory usage.
///
/// Memory layout:
/// - pointer: sizeof(void*) bytes (4 on 32-bit, 8 on 64-bit)
/// - type_id: 4 bytes (32-bit)
/// - flags: 1 byte (bit 0: is_vector, bit 1: is_typed_array)
/// - padding: 3 bytes
/// Total: 12 bytes on 32-bit, 16 bytes on 64-bit
///
class ValueView {
 public:
  // Flags for storage type
  enum StorageFlags : uint8_t {
    FLAG_NONE = 0x00,
    FLAG_IS_VECTOR = 0x01,        // std::vector storage
    FLAG_IS_TYPED_ARRAY = 0x02,   // TypedArray storage
  };

  // Default constructor - creates an invalid view
  ValueView() noexcept
    : ptr_(nullptr),
      type_id_(static_cast<uint32_t>(TYPE_ID_INVALID)),
      flags_(FLAG_NONE) {
    std::memset(padding_, 0, sizeof(padding_));
  }

  // Constructor from const Value reference
  explicit ValueView(const Value& value) noexcept
    : ptr_(&value), type_id_(value.type_id()), flags_(FLAG_NONE) {
    std::memset(padding_, 0, sizeof(padding_));
    // Detect storage type based on type_id
    if (type_id_ & TYPE_ID_1D_ARRAY_BIT) {
      // For now, we'll mark as vector by default
      // In practice, you'd check the actual storage type
      flags_ = FLAG_IS_VECTOR;
    }
  }

  // Constructor from const Value pointer
  explicit ValueView(const Value* value) noexcept
    : ptr_(value),
      type_id_(value ? value->type_id()
                     : static_cast<uint32_t>(TYPE_ID_INVALID)),
      flags_(FLAG_NONE) {
    std::memset(padding_, 0, sizeof(padding_));
    if (value && (type_id_ & TYPE_ID_1D_ARRAY_BIT)) {
      flags_ = FLAG_IS_VECTOR;
    }
  }

  // Direct construction from concrete type pointer
  template <typename T>
  explicit ValueView(const T* ptr) noexcept
    : ptr_(static_cast<const void*>(ptr)),
      type_id_(TypeTraits<T>::type_id()),
      flags_(FLAG_NONE) {
    std::memset(padding_, 0, sizeof(padding_));
    // No need to detect storage type for non-container types
  }

  // Specialization for std::vector
  template <typename ElementType>
  explicit ValueView(const std::vector<ElementType>* ptr) noexcept
    : ptr_(static_cast<const void*>(ptr)),
      type_id_(TypeTraits<std::vector<ElementType>>::type_id()),
      flags_(FLAG_IS_VECTOR) {
    std::memset(padding_, 0, sizeof(padding_));
  }

  // Specialization for TypedArray
  template <typename ElementType>
  explicit ValueView(const TypedArray<ElementType>* ptr) noexcept
    : ptr_(static_cast<const void*>(ptr)),
      type_id_(TypeTraits<TypedArray<ElementType>>::type_id()),
      flags_(FLAG_IS_TYPED_ARRAY) {
    std::memset(padding_, 0, sizeof(padding_));
  }

  // Copy constructor
  ValueView(const ValueView& other) noexcept = default;

  // Assignment operator
  ValueView& operator=(const ValueView& other) noexcept = default;

  // Check if the view is valid (points to data)
  bool valid() const noexcept { return ptr_ != nullptr; }

  // Explicit bool conversion for validity check
  explicit operator bool() const noexcept { return valid(); }

  // Get the type name of the underlying value
  const std::string type_name() const {
    // In production, use GetTypeName(type_id_)
    // For testing, return a simple placeholder
    return "type_" + std::to_string(type_id_);
  }

  const std::string underlying_type_name() const {
    // In production, use GetUnderlyingTypeName(type_id_)
    // For testing, return a simple placeholder
    return "underlying_type_" + std::to_string(underlying_type_id());
  }

  // Get type IDs
  uint32_t type_id() const noexcept {
    return type_id_;
  }

  uint32_t underlying_type_id() const noexcept {
    if (type_id_ & TYPE_ID_1D_ARRAY_BIT) {
      return type_id_ & (~TYPE_ID_1D_ARRAY_BIT);
    }
    // Map role types to their underlying types
    switch (type_id_) {
      // Point types -> underlying types
      case TYPE_ID_POINT3H: return TYPE_ID_HALF3;
      case TYPE_ID_POINT3F: return TYPE_ID_FLOAT3;
      case TYPE_ID_POINT3D: return TYPE_ID_DOUBLE3;
      // Vector types -> underlying types
      case TYPE_ID_VECTOR3H: return TYPE_ID_HALF3;
      case TYPE_ID_VECTOR3F: return TYPE_ID_FLOAT3;
      case TYPE_ID_VECTOR3D: return TYPE_ID_DOUBLE3;
      // Normal types -> underlying types
      case TYPE_ID_NORMAL3H: return TYPE_ID_HALF3;
      case TYPE_ID_NORMAL3F: return TYPE_ID_FLOAT3;
      case TYPE_ID_NORMAL3D: return TYPE_ID_DOUBLE3;
      // Color types -> underlying types
      case TYPE_ID_COLOR3H: return TYPE_ID_HALF3;
      case TYPE_ID_COLOR3F: return TYPE_ID_FLOAT3;
      case TYPE_ID_COLOR3D: return TYPE_ID_DOUBLE3;
      case TYPE_ID_COLOR4H: return TYPE_ID_HALF4;
      case TYPE_ID_COLOR4F: return TYPE_ID_FLOAT4;
      case TYPE_ID_COLOR4D: return TYPE_ID_DOUBLE4;
      // Texcoord types -> underlying types
      case TYPE_ID_TEXCOORD2H: return TYPE_ID_HALF2;
      case TYPE_ID_TEXCOORD2F: return TYPE_ID_FLOAT2;
      case TYPE_ID_TEXCOORD2D: return TYPE_ID_DOUBLE2;
      case TYPE_ID_TEXCOORD3H: return TYPE_ID_HALF3;
      case TYPE_ID_TEXCOORD3F: return TYPE_ID_FLOAT3;
      case TYPE_ID_TEXCOORD3D: return TYPE_ID_DOUBLE3;
      // Frame type
      case TYPE_ID_FRAME4D: return TYPE_ID_MATRIX4D;
      // Non-role types return themselves
      default: return type_id_;
    }
  }

  // Direct view method - get typed pointer to data
  template <typename T>
  const T* view() const noexcept {
    if (!ptr_) return nullptr;

    // Check if type IDs match exactly
    if (TypeTraits<T>::type_id() == type_id_) {
      return static_cast<const T*>(ptr_);
    }

    // Check for compatible underlying types (for role types)
    // If the requested type's ID matches our underlying type ID
    if (TypeTraits<T>::type_id() == underlying_type_id()) {
      return static_cast<const T*>(ptr_);
    }

    // Also check the reverse: if our type ID matches the requested type's underlying ID
    if (TypeTraits<T>::underlying_type_id() == type_id_) {
      return static_cast<const T*>(ptr_);
    }

    // Check if both have same underlying type (e.g., point3f and normal3f both -> float3)
    if (TypeTraits<T>::underlying_type_id() == underlying_type_id()) {
      return static_cast<const T*>(ptr_);
    }

    return nullptr;
  }

  // Legacy as() method for backward compatibility with Value interface
  template <class T>
  const T* as(bool strict_cast = false) const {
    if (!ptr_) return nullptr;

    if (ptr_ == &value_placeholder_) {
      // If we're pointing to a Value object, delegate to it
      return static_cast<const Value*>(ptr_)->as<T>(strict_cast);
    }

    // Otherwise use direct view
    if (strict_cast) {
      if (TypeTraits<T>::type_id() == type_id_) {
        return static_cast<const T*>(ptr_);
      }
      return nullptr;
    } else {
      return view<T>();
    }
  }

  // Get TypedArrayView to the underlying data
  template <class T>
  TypedArrayView<const T> as_view(bool /*strict_cast*/ = false) const {
    if (!ptr_) return TypedArrayView<const T>();

    // Check if this is an array type
    if (!(type_id_ & TYPE_ID_1D_ARRAY_BIT)) {
      return TypedArrayView<const T>();
    }

    // For std::vector
    if (flags_ & FLAG_IS_VECTOR) {
      auto vec_ptr = view<std::vector<T>>();
      if (vec_ptr) {
        return TypedArrayView<const T>(vec_ptr->data(), vec_ptr->size());
      }
    }

    // For TypedArray
    if (flags_ & FLAG_IS_TYPED_ARRAY) {
      auto arr_ptr = view<TypedArray<T>>();
      if (arr_ptr) {
        return TypedArrayView<const T>(arr_ptr->data(), arr_ptr->size());
      }
    }

    return TypedArrayView<const T>();
  }

  // Check if the value is None (ValueBlock)
  bool is_none() const noexcept {
    return type_id_ == TYPE_ID_VALUEBLOCK;
  }

  // Check storage flags
  bool is_vector() const noexcept { return flags_ & FLAG_IS_VECTOR; }
  bool is_typed_array() const noexcept { return flags_ & FLAG_IS_TYPED_ARRAY; }

  // Get the underlying pointer (const access only)
  const void* get() const noexcept { return ptr_; }

  // Equality comparison - views are equal if they point to the same data with same type
  bool operator==(const ValueView& other) const noexcept {
    return ptr_ == other.ptr_ && type_id_ == other.type_id_;
  }

  bool operator!=(const ValueView& other) const noexcept {
    return !(*this == other);
  }

  // Reset the view
  void reset() noexcept {
    ptr_ = nullptr;
    type_id_ = static_cast<uint32_t>(TYPE_ID_INVALID);
    flags_ = FLAG_NONE;
  }

  // Reset with new data
  template <typename T>
  void reset(const T* ptr) noexcept {
    *this = ValueView(ptr);
  }

  // Size check - ensure we have the expected compact size for the platform
  // 12 bytes on 32-bit (4+4+1+3), 16 bytes on 64-bit (8+4+1+3)
  static_assert(sizeof(void*) == 4 || sizeof(void*) == 8,
                "Expecting 32-bit or 64-bit pointer");

 private:
  const void* ptr_;        // sizeof(void*) bytes: Pointer to data
  uint32_t type_id_;       // 4 bytes: Type identifier
  uint8_t flags_;          // 1 byte: Storage flags
  uint8_t padding_[3];     // 3 bytes: Padding

  // Placeholder for Value compatibility
  static Value value_placeholder_;
};

// TimeSample interpolation type.
//
// Held = something like numpy.digitize(right=False)
// https://numpy.org/doc/stable/reference/generated/numpy.digitize.html
//
// Returns `values[i-1]` for `times[i-1] <= t < times[i]`
//
// Linear = linear interpolation
//
// example:
// { 0 : 0.0
//   10 : 1.0
// }
//
// - Held
//   - time 5 = returns 0.0
//   - time 9.99 = returns 0.0
//   - time 10 = returns 1.0
// - Linear
//   - time 5 = returns 0.5
//   - time 9.99 = nearly 1.0
//   - time 10 = 1.0
//
enum class TimeSampleInterpolationType {
  Held,  // something like nearest-neighbor.
  Linear,
};

//
// Supported type for `Linear` interpolation
//
// half, float, double, TimeCode(double)
// matrix2d, matrix3d, matrix4d,
// float2h, float3h, float4h
// float2f, float3f, float4f
// float2d, float3d, float4d
// quath, quatf, quatd
// (use slerp for quaternion type)

bool IsLerpSupportedType(uint32_t tyid);

template<class T>
struct LerpTraits
{
  static constexpr bool supported() {
    return false;
  }
};

///
/// @param[in] dt interpolator [0.0, 1.0)
///
bool Lerp(const value::Value &a, const value::Value &b, double dt,
          value::Value *dst);



// Handy, but may not be efficient for large time samples(e.g. 1M samples or
// more)
//
// For the runtime speed, with "-O2 -g" optimization, adding 10M `double`
// samples to linb::any takes roughly 1.8 ms on Threadripper 1950X, whereas
// simple vector<double> push_back takes 390 us(roughly x4 times faster). (Build
// benchmarks to see the numbers on your CPU)
//
// We assume having large time samples is rare situlation, and above benchmark
// speed is acceptable in general  usecases.
//
// TimeSamples struct has been moved to timesamples.hh



///
/// Cast input value's type to Role type
/// Return true: cast success.
///
bool RoleTypeCast(const uint32_t roleTyId, value::Value &inout);

///
/// Upcast value to specified type(e.g. `half` -> `float`)
/// Return true: Upcast success.
///
bool UpcastType(const std::string &toType, value::Value &inout);

///
/// Register known primitive attribute type names into a set-like container.
/// Works with std::set<std::string>, std::unordered_set<std::string>, etc.
///
/// @param d Container to populate (cleared first).
/// @param include_variant_set If true, include "variantSet" entry.
///
template <typename SetType>
inline void RegisterPrimAttrTypes(SetType &d, bool include_variant_set = false) {
  d.clear();

  d.insert(kBool);

  d.insert(kChar);
  d.insert(kChar2);
  d.insert(kChar3);
  d.insert(kChar4);

  d.insert(kUChar);
  d.insert(kUChar2);
  d.insert(kUChar3);
  d.insert(kUChar4);

  d.insert(kShort);
  d.insert(kShort2);
  d.insert(kShort3);
  d.insert(kShort4);

  d.insert(kUShort);
  d.insert(kUShort2);
  d.insert(kUShort3);
  d.insert(kUShort4);

  d.insert(kInt64);

  d.insert(kInt);
  d.insert(kInt2);
  d.insert(kInt3);
  d.insert(kInt4);

  d.insert(kUInt64);

  d.insert(kUInt);
  d.insert(kUInt2);
  d.insert(kUInt3);
  d.insert(kUInt4);

  d.insert(kFloat);
  d.insert(kFloat2);
  d.insert(kFloat3);
  d.insert(kFloat4);

  d.insert(kDouble);
  d.insert(kDouble2);
  d.insert(kDouble3);
  d.insert(kDouble4);

  d.insert(kHalf);
  d.insert(kHalf2);
  d.insert(kHalf3);
  d.insert(kHalf4);

  d.insert(kQuath);
  d.insert(kQuatf);
  d.insert(kQuatd);

  // AOUSD Core Spec 6.2: timecode scalar type
  d.insert(kTimeCode);

  // AOUSD Core Spec 6.5: All semantic aliases (role types)
  // normal3{h,f,d}
  d.insert(kNormal3h);
  d.insert(kNormal3f);
  d.insert(kNormal3d);
  // point3{h,f,d}
  d.insert(kPoint3h);
  d.insert(kPoint3f);
  d.insert(kPoint3d);
  // vector3{h,f,d}
  d.insert(kVector3h);
  d.insert(kVector3f);
  d.insert(kVector3d);
  d.insert(kVector4f);
  d.insert(kVector4d);
  // color{3,4}{h,f,d}
  d.insert(kColor3h);
  d.insert(kColor3f);
  d.insert(kColor3d);
  d.insert(kColor4h);
  d.insert(kColor4f);
  d.insert(kColor4d);
  // texCoord{2,3}{h,f,d} + texCoord4{h,f,d}
  d.insert(kTexCoord2h);
  d.insert(kTexCoord2f);
  d.insert(kTexCoord2d);
  d.insert(kTexCoord3h);
  d.insert(kTexCoord3f);
  d.insert(kTexCoord3d);
  d.insert(kTexCoord4h);
  d.insert(kTexCoord4f);
  d.insert(kTexCoord4d);
  // frame4d
  d.insert(kFrame4d);

  d.insert(kMatrix2f);
  d.insert(kMatrix3f);
  d.insert(kMatrix4f);

  d.insert(kMatrix2d);
  d.insert(kMatrix3d);
  d.insert(kMatrix4d);

  d.insert(kToken);
  d.insert(kString);

  d.insert(kRelationship);
  d.insert(kAssetPath);
  d.insert(kPathExpression);

  d.insert(kDictionary);

  if (include_variant_set) {
    d.insert("variantSet");
  }
}

// token[] trait. std::vector<token> needs an explicit specialization to
// override the generic TypeTraits<std::vector<T>>.
//
// NOTE: The TimeSamples TypeTrait used to live here behind `#include
// "timesamples.hh"`. It now lives at the end of timesamples.hh instead, so that
// value-types.hh does NOT have to pull in the (heavy) timesamples.hh. This
// breaks a costly include cycle: timesamples.hh already includes value-types.hh,
// so every value-types.hh consumer was transitively parsing all of
// timesamples.hh (and the TypedTimeSamples<> machinery).
#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(std::vector<token>, "token[]", TYPE_ID_TOKEN_VECTOR, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

static_assert(sizeof(quath) == 8, "sizeof(quath) must be 8");
static_assert(sizeof(quatf) == 16, "sizeof(quatf) must be 16");
static_assert(sizeof(quatd) == 32, "sizeof(quatd) must be 32");
static_assert(sizeof(half) == 2, "sizeof(half) must be 2");
static_assert(sizeof(half2) == 4, "sizeof(half2) must be 4");
static_assert(sizeof(half3) == 6, "sizeof(half3) must be 6");
static_assert(sizeof(half4) == 8, "sizeof(half4) must be 8");
static_assert(sizeof(float3) == 12, "sizeof(float3) must be 12");
static_assert(sizeof(color3f) == 12, "sizeof(color3f) must be 12");
static_assert(sizeof(color4f) == 16, "sizeof(color4f) must be 16");

}  // namespace value

}  // namespace lightusd
