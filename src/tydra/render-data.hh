// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file render-data.hh
/// @brief Tydra render-friendly data structures and conversion utilities
///
/// The Tydra framework converts USD scene graphs into render-friendly
/// data structures optimized for graphics APIs like OpenGL/WebGL, Vulkan,
/// and raytracing engines. This header defines the core RenderScene and
/// related data structures.
///
/// Key features:
/// - Flattened scene representation with pre-computed transforms
/// - GPU-friendly data layout (vertex buffers, index buffers)
/// - Material and texture management
/// - Animation support
/// - Multiple rendering backend support (WebGL, Vulkan, raytracing)
///
/// Main classes:
/// - RenderScene: Top-level render scene container
/// - RenderMesh: GPU-ready mesh data with materials
/// - RenderMaterial: Processed material definitions
/// - RenderTexture: Texture resources and samplers
/// - RenderCamera: Camera parameters for rendering
/// - RenderLight: Light definitions for shading
///
/// Memory optimization:
/// - Define TYDRA_USE_INDEX to use array indices instead of values in
///   DefaultPackedVertexData, reducing memory usage by ~55% per vertex
/// - When TYDRA_USE_INDEX is defined, attributes are stored as uint32_t
///   indices into shared attribute arrays instead of duplicate values
/// - Use index value ~0u (UINT32_MAX) to indicate missing attributes
///
/// Epsilon-based comparison:
/// - DefaultPackedVertexDataCompare and DefaultPackedVertexDataEqualEps
///   provide floating-point comparison with configurable epsilon values
/// - Default epsilon: 1e-6f for positions, 1e-3f for other attributes
/// - Supports both indexed and direct value modes
/// - Use these comparators for robust vertex deduplication with floating-point data
///
/// Spatial hashing optimization:
/// - VertexSpatialHashGrid provides O(1) average-case vertex similarity search
/// - Uses Morton code ordering for cache-friendly traversal
/// - BuildIndicesWithSpatialHash() offers optimized vertex deduplication
/// - Particularly beneficial for large meshes (>10K vertices)
/// - Automatic subdivision for large cells maintains performance
///
/// The conversion process:
/// ```cpp
/// lightusd::tydra::RenderScene renderScene;
/// lightusd::tydra::RenderSceneConverter converter;
/// bool success = converter.ConvertToRenderScene(stage, &renderScene);
/// ```
///
#pragma once
#define LIGHTUSD_TYDRA_RENDER_DATA_HH_

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#include "asset-resolution.hh"
#include "nonstd/expected.hpp"
#include "chunked-typed-array.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "value-types.hh"

// tydra
#include "common-types.hh"
#include "scene-access.hh"
#include "variant-support.hh"
#include "spatial-hashes.hh"
#include "render-data-pprint.hh"
#include "shape-to-mesh.hh"

// Extracted headers
#include "render-data-shader.hh"

namespace lightusd {

// forward decl
class Stage;
class Prim;
struct Material;
struct GeomMesh;
struct Xform;
struct AssetInfo;
class Path;
struct UsdPreviewSurface;
struct UsdUVTexture;

template <typename T>
struct UsdPrimvarReader;

using UsdPrimvarReader_int = UsdPrimvarReader<int>;
using UsdPrimvarReader_float = UsdPrimvarReader<float>;
using UsdPrimvarReader_float3 = UsdPrimvarReader<value::float3>;
using UsdPrimvarReader_float3 = UsdPrimvarReader<value::float3>;
using UsdPrimvarReader_string = UsdPrimvarReader<std::string>;
using UsdPrimvarReader_matrix4d = UsdPrimvarReader<value::matrix4d>;

namespace tydra {

///
/// Progress callback function type for RenderSceneConverter.
/// @param[in] progress Progress value between 0.0 and 1.0
/// @param[in] userptr User-provided pointer for custom data
/// @return true to continue conversion, false to cancel
///
using ProgressCallback = std::function<bool(float progress, void *userptr)>;

///
/// Detailed progress information for fine-grained progress reporting.
/// Contains counts for meshes, materials, textures and the current processing stage.
///
struct DetailedProgressInfo {
  enum class Stage {
    Idle,
    CountingPrims,      // Counting prims before conversion
    ConvertingXforms,   // Converting xform nodes
    ConvertingMeshes,   // Converting meshes
    ConvertingMaterials,// Converting materials
    ConvertingTextures, // Loading textures
    BuildingHierarchy,  // Building node hierarchy
    ExtractingAnimations,// Extracting animations
    MergingMeshes,      // Merging meshes (optional)
    Complete
  };

  Stage stage{Stage::Idle};
  float progress{0.0f};           // 0.0 to 1.0 overall progress

  // Mesh progress
  size_t meshes_processed{0};
  size_t meshes_total{0};
  std::string current_mesh_name;

  // Material progress
  size_t materials_processed{0};
  size_t materials_total{0};
  std::string current_material_name;

  // Texture progress
  size_t textures_processed{0};
  size_t textures_total{0};
  std::string current_texture_name;

  // Generic progress message
  std::string message;

  const char* GetStageName() const {
    switch (stage) {
      case Stage::Idle: return "idle";
      case Stage::CountingPrims: return "counting";
      case Stage::ConvertingXforms: return "xforms";
      case Stage::ConvertingMeshes: return "meshes";
      case Stage::ConvertingMaterials: return "materials";
      case Stage::ConvertingTextures: return "textures";
      case Stage::BuildingHierarchy: return "hierarchy";
      case Stage::ExtractingAnimations: return "animations";
      case Stage::MergingMeshes: return "merging";
      case Stage::Complete: return "complete";
    }
    return "unknown";
  }
};

///
/// Detailed progress callback function type for RenderSceneConverter.
/// Provides more granular progress information including mesh/material counts.
/// @param[in] info Detailed progress information
/// @param[in] userptr User-provided pointer for custom data
/// @return true to continue conversion, false to cancel
///
using DetailedProgressCallback = std::function<bool(const DetailedProgressInfo &info, void *userptr)>;

// Conditional typedef for ChunkedVectorArray based on TYDRA_USE_CHUNKED_ARRAY
#ifdef TYDRA_USE_CHUNKED_ARRAY
template<typename T>
using ChunkedVectorArray = lightusd::ChunkedTypedArray<T>;
#else
template<typename T>
using ChunkedVectorArray = std::vector<T>;
#endif

// GLSL like data types
using vec2 = value::float2;
using vec3 = value::float3;
using vec4 = value::float4;
using quat = value::float4; // (x, y, z, w)
using mat2 = value::matrix2f;
using mat3 = value::matrix3f;
using mat4 = value::matrix4f;
using dmat4 = value::matrix4d;

///
/// Bidirectional mapping between strings and numeric IDs.
/// Useful for converting between human-readable names and efficient
/// numeric identifiers in render data structures.
///
struct StringAndIdMap {
  using IdToStringMap = std::unordered_map<uint64_t, std::string>;
  using StringToIdMap =
      std::unordered_map<std::string, uint64_t, FNV1StringHash>;
  using id_const_iterator = IdToStringMap::const_iterator;
  using string_const_iterator = StringToIdMap::const_iterator;

  void add(uint64_t key, const std::string &val) {
    _i_to_s[key] = val;
    _s_to_i[val] = key;
  }

  void add(const std::string &key, uint64_t val) {
    _s_to_i[key] = val;
    _i_to_s[val] = key;
  }

  bool empty() const { return _i_to_s.empty(); }

  size_t count(uint64_t i) const { return _i_to_s.count(i); }

  size_t count(const std::string &s) const { return _s_to_i.count(s); }

  std::string at(uint64_t i) const { return _i_to_s.at(i); }

  uint64_t at(const std::string &s) const { return _s_to_i.at(s); }

  id_const_iterator find(uint64_t key) const {
    return _i_to_s.find(key);
  }

  string_const_iterator find(const std::string &key) const {
    return _s_to_i.find(key);
  }

  string_const_iterator s_begin() const {
    return _s_to_i.begin();
  }

  string_const_iterator s_end() const {
    return _s_to_i.end();
  }

  id_const_iterator i_begin() const {
    return _i_to_s.begin();
  }

  id_const_iterator i_end() const {
    return _i_to_s.end();
  }

  size_t size() const {
    // size should be same, but just in case.
    if (_i_to_s.size() == _s_to_i.size()) {
      return _i_to_s.size();
    }

    return 0;
  }

  IdToStringMap _i_to_s;  // index -> string
  StringToIdMap _s_to_i;  // string -> index
};

// timeSamples in USD
// TODO: AttributeBlock support
template <typename T>
struct AnimationSample {
  float t{0.0};  // time is represented as float
  T value;
};

enum class VertexVariability {
  Constant,  // one value for all geometric elements
  Uniform,   // one value for each geometric elements(e.g. `face`, `UV patch`)
  Varying,   // per-vertex for each geometric elements. Bilinear interpolation.
  Vertex,  // Equvalent to `Varying` for Polygon mesh. The basis function of the
           // surface is used for the interpolation(Curves, Subdivision Surface,
           // etc).
  FaceVarying,  // per-Vertex per face. Bilinear interpolation.
  Indexed,      // Dedicated index buffer provided(unflattened Indexed Primvar).
};

//std::string to_string(VertexVariability variability);

enum class NodeType {
  Xform,
  Mesh,  // Polygon mesh
  Camera,
  SkelRoot, // UsdSkelRoot: encapsulation prim for skinned subtree
  Skeleton, // UsdSkeleton: joint hierarchy with bind/rest transforms
  PointLight,       // SphereLight in USD
  DirectionalLight, // DistantLight in USD
  EnvmapLight,      // DomeLight in USD
  RectLight,
  DiskLight,
  CylinderLight,
  GeometryLight,
  Volume,  // UsdVol Volume (OpenVDB / Field3D)
};

// High-level categorization of USD Prim types
enum class NodeCategory {
  Group,     // Organizational: Xform, Scope, Model
  Geom,      // Geometry: Mesh, Points, Curves, etc.
  Light,     // Lights: RectLight, DomeLight, SphereLight, etc.
  Camera,    // Camera
  Material,  // Material, Shader, NodeGraph
  Skeleton,  // SkelRoot, Skeleton, SkelAnimation
};

enum class ComponentType {
  UInt8,
  Int8,
  UInt16,
  Int16,
  UInt32,
  Int32,
  Half,
  Float,
  Double,
};

// GPU block-compressed texture format for a TextureImage whose `buffer_id`
// holds compressed block bytes rather than uncompressed texels (see
// TextureImage::blockFormat). `None` = uncompressed (the default; texels are
// described by ComponentType/channels as before). `UNI` is the private tinyexr
// transcodable intermediate: its payload is valid ASTC 4x4 and
// can be transcoded per device to BC7/BC1/ASTC/ETC2 (texcomp tc_uni_transcode_*)
// or decoded to RGBA8 (tc_uni_decompress_rgba8). Populated when a KTX2 asset is
// loaded in "keep compressed" mode; consumers (lusdview GPU upload) map it to a
// GL/VK compressed format, or fall back to CPU-decoding it.
enum class TextureBlockFormat {
  None = 0,
  BC1,
  BC3,
  BC5,
  BC6H,   // HDR
  BC7,
  ETC2_RGB,
  ETC2_RGBA,
  EAC_R11,
  EAC_RG11,
  ASTC_4x4,
  UNI,    // private tinyexr uni transcodable intermediate
};


// glTF-like BufferData
struct BufferData {
  ComponentType componentType{ComponentType::UInt8};
  //uint8_t count{1};           // # of components. up to 256
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<uint8_t> data;  // binary data. size is dividable by sizeof(componentType)
#else
  std::vector<uint8_t> data;  // binary data. size is dividable by sizeof(componentType)
#endif

  // TODO: Stride?
};

// Move raw bytes into a BufferData's storage. In the default (std::vector)
// configuration this steals `src`'s buffer in O(1) with no copy; the chunked
// storage build cannot adopt a std::vector's buffer, so it falls back to an
// element-wise copy. `src` is left empty either way.
inline void SetBufferDataBytes(BufferData &dst, std::vector<uint8_t> &&src) {
#ifdef TYDRA_USE_CHUNKED_ARRAY
  dst.data.resize(src.size());
  for (size_t i = 0; i < src.size(); i++) {
    dst.data[i] = src[i];
  }
  src.clear();
  src.shrink_to_fit();
#else
  dst.data = std::move(src);
#endif
}

// Compound of ComponentType x component
enum class VertexAttributeFormat {
  Bool,     // bool(1 byte)
  Char,     // int8
  Char2,    // int8x2
  Char3,    // int8x3
  Char4,    // int8x4
  Byte,     // uint8
  Byte2,    // uint8x2
  Byte3,    // uint8x3
  Byte4,    // uint8x4
  Short,    // int16
  Short2,   // int16x2
  Short3,   // int16x2
  Short4,   // int16x2
  Ushort,   // uint16
  Ushort2,  // uint16x2
  Ushort3,  // uint16x2
  Ushort4,  // uint16x2
  Half,     // half
  Half2,    // half2
  Half3,    // half3
  Half4,    // half4
  Float,    // float
  Vec2,     // float2
  Vec3,     // float3
  Vec4,     // float4
  Int,      // int
  Ivec2,    // int2
  Ivec3,    // int3
  Ivec4,    // int4
  Uint,     // uint
  Uvec2,    // uint2
  Uvec3,    // uint3
  Uvec4,    // uint4
  Double,   // double
  Dvec2,    // double2
  Dvec3,    // double3
  Dvec4,    // double4
  Mat2,     // float 2x2
  Mat3,     // float 3x3
  Mat4,     // float 4x4
  Dmat2,    // double 2x2
  Dmat3,    // double 3x3
  Dmat4,    // double 4x4
};

static size_t VertexAttributeFormatSize(VertexAttributeFormat f) {
  size_t elemsize{0};

  switch (f) {
    case VertexAttributeFormat::Bool: {
      elemsize = 1;
      break;
    }
    case VertexAttributeFormat::Char: {
      elemsize = 1;
      break;
    }
    case VertexAttributeFormat::Char2: {
      elemsize = 2;
      break;
    }
    case VertexAttributeFormat::Char3: {
      elemsize = 3;
      break;
    }
    case VertexAttributeFormat::Char4: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Byte: {
      elemsize = 1;
      break;
    }
    case VertexAttributeFormat::Byte2: {
      elemsize = 2;
      break;
    }
    case VertexAttributeFormat::Byte3: {
      elemsize = 3;
      break;
    }
    case VertexAttributeFormat::Byte4: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Short: {
      elemsize = 2;
      break;
    }
    case VertexAttributeFormat::Short2: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Short3: {
      elemsize = 6;
      break;
    }
    case VertexAttributeFormat::Short4: {
      elemsize = 8;
      break;
    }
    case VertexAttributeFormat::Ushort: {
      elemsize = 2;
      break;
    }
    case VertexAttributeFormat::Ushort2: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Ushort3: {
      elemsize = 6;
      break;
    }
    case VertexAttributeFormat::Ushort4: {
      elemsize = 8;
      break;
    }
    case VertexAttributeFormat::Half: {
      elemsize = 2;
      break;
    }
    case VertexAttributeFormat::Half2: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Half3: {
      elemsize = 6;
      break;
    }
    case VertexAttributeFormat::Half4: {
      elemsize = 8;
      break;
    }
    case VertexAttributeFormat::Mat2: {
      elemsize = 4 * 4;
      break;
    }
    case VertexAttributeFormat::Mat3: {
      elemsize = 4 * 9;
      break;
    }
    case VertexAttributeFormat::Mat4: {
      elemsize = 4 * 16;
      break;
    }
    case VertexAttributeFormat::Dmat2: {
      elemsize = 8 * 4;
      break;
    }
    case VertexAttributeFormat::Dmat3: {
      elemsize = 8 * 9;
      break;
    }
    case VertexAttributeFormat::Dmat4: {
      elemsize = 8 * 16;
      break;
    }
    case VertexAttributeFormat::Float: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Vec2: {
      elemsize = sizeof(float) * 2;
      break;
    }
    case VertexAttributeFormat::Vec3: {
      elemsize = sizeof(float) * 3;
      break;
    }
    case VertexAttributeFormat::Vec4: {
      elemsize = sizeof(float) * 4;
      break;
    }
    case VertexAttributeFormat::Int: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Ivec2: {
      elemsize = sizeof(int) * 2;
      break;
    }
    case VertexAttributeFormat::Ivec3: {
      elemsize = sizeof(int) * 3;
      break;
    }
    case VertexAttributeFormat::Ivec4: {
      elemsize = sizeof(int) * 4;
      break;
    }
    case VertexAttributeFormat::Uint: {
      elemsize = 4;
      break;
    }
    case VertexAttributeFormat::Uvec2: {
      elemsize = sizeof(uint32_t) * 2;
      break;
    }
    case VertexAttributeFormat::Uvec3: {
      elemsize = sizeof(uint32_t) * 3;
      break;
    }
    case VertexAttributeFormat::Uvec4: {
      elemsize = sizeof(uint32_t) * 4;
      break;
    }
    case VertexAttributeFormat::Double: {
      elemsize = sizeof(double);
      break;
    }
    case VertexAttributeFormat::Dvec2: {
      elemsize = sizeof(double) * 2;
      break;
    }
    case VertexAttributeFormat::Dvec3: {
      elemsize = sizeof(double) * 3;
      break;
    }
    case VertexAttributeFormat::Dvec4: {
      elemsize = sizeof(double) * 4;
      break;
    }
  }

  return elemsize;
}


///
/// Vertex attribute array. Stores raw vertex attribute data.
///
/// arrayLength = elementSize * vertexCount
/// arrayBytes = formatSize * elementSize * vertexCount
///
/// Example:
///    positions(float3, elementSize=1, n=2): [1.0, 1.1, 1.2,  0.4, 0.3, 0.2]
///    skinWeights(float, elementSize=4, n=2): [1.0, 1.0, 1.0, 1.0,  0.5, 0.5,
///    0.5, 0.5]
///
struct VertexAttribute {
  std::string name;  // Attribute(primvar) name. Optional. Can be empty.
  VertexAttributeFormat format{VertexAttributeFormat::Vec3};
  uint32_t elementSize{1};  // `elementSize` in USD terminology(i.e. # of
                            // samples per vertex data)
  uint32_t stride{0};  //  We don't support packed(interleaved) vertex data, so
                       //  stride is usually sizeof(VertexAttributeFormat) *
                       //  elementSize. 0 = tightly packed.
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<uint8_t> data;  // raw binary data(TODO: Use Buffer ID?)
#else
  std::vector<uint8_t> data;  // raw binary data(TODO: Use Buffer ID?)
#endif
  std::vector<uint32_t>
      indices;  // Dedicated Index buffer. Set when variability == Indexed.
                // empty = Use externally provided vertex index buffer
  VertexVariability variability{VertexVariability::Vertex};
  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid

  //
  // Returns the number of vertex items(excludes `elementSize`).
  //
  // We use compound type for the format.
  // For example, this returns 1 when the buffer is
  // composed of 3 floats and `format` is float3(in any elementSize >= 1).
  size_t vertex_count() const {
    if (stride != 0) {
      // TODO: return 0 when (data.size() % stride) != 0?
      return data.size() / stride;
    }

    size_t itemSize = stride_bytes();

    if ((data.size() % itemSize) != 0) {
      // data size mismatch
      return 0;
    }

    return data.size() / itemSize;
  }

  inline bool empty() const {
    return data.empty();
  }

  size_t num_bytes() const { return data.size(); }

  const void *buffer() const {
    return reinterpret_cast<const void *>(data.data());
  }

  void set_buffer(const uint8_t *addr, size_t n) {
    data.resize(n);
    memcpy(data.data(), addr, n);
  }

  const std::vector<uint8_t> &get_data() const { return data; }

  std::vector<uint8_t> &get_data() { return data; }

  //
  // Bytes for each vertex item.
  // Returns `formatSize * elementSize` when `stride` is 0.
  // Returns `stride` when `stride` is not zero.
  //
  size_t stride_bytes() const {
    if (stride != 0) {
      return stride;
    }

    return element_size() * VertexAttributeFormatSize(format);
  }

  size_t element_size() const { return elementSize; }

  size_t format_size() const { return VertexAttributeFormatSize(format); }

  bool is_constant() const {
    return (variability == VertexVariability::Constant);
  }

  bool is_uniform() const {
    return (variability == VertexVariability::Uniform);
  }

  // includes 'varying'
  bool is_vertex() const {
    return (variability == VertexVariability::Vertex) ||
           (variability == VertexVariability::Varying);
  }

  bool is_facevarying() const {
    return (variability == VertexVariability::FaceVarying);
  }

  bool is_indexed() const { return variability == VertexVariability::Indexed; }
};



//
// Convert PrimVar(type-erased value) at specified time to VertexAttribute
//
// Input Primvar's name, variability(interpolation) and elementSize are
// preserved. Use Primvar's underlying type to set the type of VertexAttribute.
// (example: 'color3f'(underlying type 'float3') -> Vec3)
//
// @param[in] pvar GeomPrimvar.
// @param[out] dst Output VertexAttribute.
// @param[out] err Error messsage. can be nullptr
// @param[in] t timecode
// @param[in] tinterp Interpolation for timesamples
//
// @return true upon success.
//
bool ToVertexAttribute(const GeomPrimvar &pvar, VertexAttribute &dst,
                       std::string *err,
                       const double t = value::TimeCode::Default(),
                       const value::TimeSampleInterpolationType tinterp =
                           value::TimeSampleInterpolationType::Linear);

enum class ColorSpace {
  sRGB,
  Lin_sRGB,     // Linear sRGB(D65)
  Rec709,
  Lin_Rec709,   // Linear Rec.709 - same primaries as sRGB but linear (MaterialX: lin_rec709)
  g22_Rec709,   // Gamma 2.2 Rec.709 (MaterialX: g22_rec709)
  g18_Rec709,   // Gamma 1.8 Rec.709 (MaterialX: g18_rec709)
  sRGB_Texture, // sRGB for textures (MaterialX: srgb_texture)
  Raw,          // Raw(physical quantity) value(e.g. normal maps, ao maps)
  Lin_ACEScg,   // ACES CG colorspace(AP1. D50)
  ACES2065_1,   // ACES 2065-1 (AP0. D60)
  Lin_Rec2020,  // Linear Rec.2020/Rec.2100
  OCIO,
  Lin_DisplayP3,   // colorSpace 'lin_displayp3'
  sRGB_DisplayP3,  // colorSpace 'srgb_displayp3'
  Custom,          // TODO: Custom colorspace
  Unknown,         // Unknown color space.
};


// Infer colorspace from token value.
bool InferColorSpace(const value::token &tok, ColorSpace *result);

struct TextureImage {
  std::string asset_identifier;  // (resolved) filename or asset identifier.

  ComponentType texelComponentType{
      ComponentType::UInt8};  // texel bit depth of `buffer_id`
  ComponentType assetTexelComponentType{
      ComponentType::UInt8};  // texel bit depth of UsdUVTexture asset

  ColorSpace colorSpace{ColorSpace::sRGB};  // color space of texel data.
  ColorSpace usdColorSpace{
      ColorSpace::sRGB};  // original color space info in UsdUVTexture(asset meta or sourceColorSpace attrib)

  // Resolved authored source -> display-linear Rec.709 transform. This keeps
  // ColorSpaceDefinitionAPI data available to lazy native/Web texture users,
  // which otherwise only see the legacy ColorSpace enum after conversion.
  std::string sourceColorSpaceName{"auto"};
  bool colorTransformValid{false};
  bool colorTransformApplied{false};
  bool colorTransformBypass{true};
  bool sourceColorIsData{false};
  float sourceGamma{1.0f};
  float sourceLinearBias{0.0f};
  float sourceToDisplayLinear[9] = {
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f};

  int32_t width{-1};
  int32_t height{-1};
  int32_t channels{-1};  // e.g. 3 for RGB.
  int32_t miplevel{0};

  int64_t buffer_id{-1};  // index to buffer_id(texel data)

  bool decoded{false}; // true if texture data(buffer_id) is decoded. false if buffer_id contains raw image data(e.g. JPEG data)

  // GPU block-compressed representation. When blockFormat != None, `buffer_id`
  // holds compressed block bytes (a level-0 block stream; optional precomputed
  // mip levels may follow depending on the loader) instead of uncompressed
  // texels, `decoded` is false, and `channels`/`texelComponentType` describe the
  // decoded form. `blockWidth`/`blockHeight` are the codec block footprint
  // (e.g. 4x4). Default None = ordinary uncompressed image (unchanged behavior).
  TextureBlockFormat blockFormat{TextureBlockFormat::None};
  int32_t blockWidth{0};
  int32_t blockHeight{0};

  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid
};

struct Cubemap
{
  // face id mapping(based on OpenGL)
  // https://www.khronos.org/opengl/wiki/Cubemap_Texture
  //
  // 0: +X (right)
  // 1: -X (left)
  // 2: +Y (top)
  // 3: -Y (bottom)
  // 4: +Z (back)
  // 5: -Z (front)

  // LoD of cubemap
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<std::array<TextureImage, 6>> faces_lod;
#else
  std::vector<std::array<TextureImage, 6>> faces_lod;
#endif
};

// Envmap lightsource
struct EnvmapLight
{
  enum class Coordinate {
    LatLong,  // "latlong"
    Angular,  // "angular"
    // MirroredBall, // TODO: "mirroredBall"
    Cubemap,  // LightUSD Tydra specific.
  };

  std::string element_name;
  std::string abs_path;
  std::string display_name;

  double guideRadius{1.0e5};
  std::string asset_name; // 'inputs:texture:file'

#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<TextureImage> texture_lod;
#else
  std::vector<TextureImage> texture_lod;
#endif

  // Utility
  bool to_cubemap(Cubemap &cubemap);

};

// glTF-lie animation data

// TOOD: Implement Animation sample resampler.

// In USD, timeSamples are linearly interpolated by default.
template <typename T>
struct AnimationSampler {
  nonstd::optional<T> static_value; // value at static time('default' time) if exist
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<AnimationSample<T>> samples;
#else
  std::vector<AnimationSample<T>> samples;
#endif

  // No cubicSpline in USD
  enum class Interpolation {
    Linear,
    Step,  // Held in USD
  };

  Interpolation interpolation{Interpolation::Linear};
};

///
/// Animation interpolation mode (matches glTF specification)
///
enum class AnimationInterpolation {
  Linear,      ///< LINEAR - linear interpolation (slerp for quaternions)
  Step,        ///< STEP - discrete/stepped interpolation (no interpolation)
  CubicSpline  ///< CUBICSPLINE - cubic spline with in/out tangents
};

/// Tracks the USD origin of an AnimationClip.
enum class AnimationSourceType {
  Unknown,
  XformOp,          ///< From xformOp timeSamples
  SkelAnimation,    ///< From UsdSkelAnimation prim
  BlendShape,       ///< From BlendShape weight animation (future)
};

///
/// Animation channel target type - distinguishes what the channel animates
///
/// USD has two distinct animation systems:
/// - Node animations: xformOp time samples animate scene node transforms
/// - Skeletal animations: SkelAnimation arrays animate skeleton joint transforms
///
/// This enum enables type-safe handling of both animation types in a unified structure.
///
enum class ChannelTargetType {
  SceneNode,      ///< Targets a scene node's transform (from USD xformOp animations)
  SkeletonJoint   ///< Targets a skeleton joint (from USD SkelAnimation)
};

///
/// Animation target property path (matches glTF animation paths)
///
enum class AnimationPath {
  Translation,  ///< Animates position (vec3) - maps to .position in Three.js
  Rotation,     ///< Animates rotation (quat) - maps to .quaternion in Three.js
  Scale,        ///< Animates scale (vec3) - maps to .scale in Three.js
  Weights,      ///< Animates morph target weights (float array)
  CustomProperty ///< Animates arbitrary numeric prim property (e.g. trajectory
                 ///< position or physics data)
};

///
/// Keyframe sampler - stores keyframe times and values in flat arrays
///
/// Matches glTF Animation Sampler structure and Three.js KeyframeTrack format.
/// Values are stored as flat float arrays:
/// - Translation/Scale: [x0,y0,z0, x1,y1,z1, ...] (3 floats per keyframe)
/// - Rotation: [x0,y0,z0,w0, x1,y1,z1,w1, ...] (4 floats per keyframe)
/// - Weights: [w0, w1, w2, ...] (1 float per keyframe per target)
///
/// For CubicSpline interpolation, each keyframe requires 3 values:
/// [in_tangent, value, out_tangent], so array size is times.size() * components * 3
///
struct KeyframeSampler {
  std::vector<float> times;   ///< Keyframe times in seconds (flat array)
  std::vector<float> values;  ///< Keyframe values (flat array)
  AnimationInterpolation interpolation{AnimationInterpolation::Linear};

  /// Check if sampler is empty
  bool empty() const { return times.empty(); }

  /// Get number of keyframes
  size_t num_keyframes() const { return times.size(); }
};

///
/// Animation channel - binds sampler data to a specific target property
///
/// Supports both node transform animations (from USD xformOps) and
/// skeletal joint animations (from USD SkelAnimation). The target_type field
/// determines how to interpret the target identification fields.
///
/// Matches glTF Animation Channel structure. Each channel targets one property
/// of one target, and references a sampler that provides the keyframe data.
///
/// Example usage:
/// ```
/// // Node animation (xformOp)
/// AnimationChannel node_channel;
/// node_channel.target_type = ChannelTargetType::SceneNode;
/// node_channel.path = AnimationPath::Translation;
/// node_channel.target_node = 5;  // Index into RenderScene::nodes
/// node_channel.sampler = 0;
///
/// // Skeletal animation (SkelAnimation)
/// AnimationChannel joint_channel;
/// joint_channel.target_type = ChannelTargetType::SkeletonJoint;
/// joint_channel.path = AnimationPath::Rotation;
/// joint_channel.skeleton_id = 0;  // Index into RenderScene::skeletons
/// joint_channel.joint_id = 12;     // Joint index within skeleton
/// joint_channel.sampler = 1;
/// ```
///
struct AnimationChannel {
  AnimationPath path;         ///< Which property to animate (translation/rotation/scale/weights)
  ChannelTargetType target_type{ChannelTargetType::SceneNode};  ///< Target type (node or joint)

  // Target identification (interpretation depends on target_type)
  int32_t target_node{-1};    ///< SceneNode: index into RenderScene::nodes (-1 = invalid)
                               ///< SkeletonJoint: unused (use skeleton_id + joint_id instead)

  // Skeletal animation fields (only used when target_type == SkeletonJoint)
  int32_t skeleton_id{-1};    ///< Index into RenderScene::skeletons (-1 = invalid)
  int32_t joint_id{-1};       ///< Index within skeleton's joint array (-1 = invalid)

  int32_t sampler{-1};        ///< Index into AnimationClip::samplers (-1 = invalid)

  // For AnimationPath::CustomProperty channels.
  bool is_custom_property{false};    ///< true when `path == AnimationPath::CustomProperty`
  std::string property_name;         ///< The custom property name for this channel.
  std::string target_prim_path;      ///< Optional unresolved/resolved target prim path.

  // For AnimationPath::Weights channels sourced from UsdSkel blendShapeWeights.
  // Values are stored in this target-name order.
  std::vector<std::string> blendshape_target_names;

  /// Check if channel is valid based on its target type
  bool is_valid() const {
    if (sampler < 0) return false;
    if (target_type == ChannelTargetType::SceneNode) {
      return target_node >= 0;
    } else {  // SkeletonJoint
      return skeleton_id >= 0 && joint_id >= 0;
    }
  }

  /// Check if this is a skeletal animation channel
  bool is_skeletal() const {
    return target_type == ChannelTargetType::SkeletonJoint;
  }
};

///
/// Animation clip - collection of animation channels and samplers
///
/// Matches glTF Animation structure and Three.js AnimationClip.
/// An animation clip contains:
/// - samplers: Keyframe data (times and values)
/// - channels: Bindings from samplers to node properties
///
/// This design separates animation data from the scene hierarchy,
/// making it compatible with Three.js/glTF animation systems.
///
/// Example usage:
/// ```
/// AnimationClip clip;
/// clip.name = "Walk";
/// clip.duration = 2.0f;
///
/// // Create sampler for translation
/// KeyframeSampler trans_sampler;
/// trans_sampler.times = {0.0f, 1.0f, 2.0f};
/// trans_sampler.values = {0,0,0,  1,0,0,  0,0,0};  // Flat array: x,y,z for each keyframe
/// clip.samplers.push_back(trans_sampler);
///
/// // Create channel targeting node 5's translation
/// AnimationChannel channel;
/// channel.path = AnimationPath::Translation;
/// channel.target_node = 5;
/// channel.sampler = 0;  // Index into clip.samplers
/// clip.channels.push_back(channel);
/// ```
///
struct AnimationClip {
  std::string name;               ///< Animation name
  std::string prim_name;          ///< Original USD prim name (element name)
  std::string abs_path;           ///< Original USD absolute prim path
  std::string display_name;       ///< USD `displayName` prim meta

  float duration{0.0f};           ///< Animation duration in seconds

  AnimationSourceType source_type{AnimationSourceType::Unknown};

  // Set true when animation originated from USD value clips.
  bool has_value_clip{false};
  // Set true when clip data has been baked into sampler arrays.
  bool value_clip_baked{false};
  // Time range of sampled stage time used to bake this clip.
  float value_clip_start_time{0.0f};
  float value_clip_end_time{0.0f};
  // Configured sample rate used during clip baking (0 if not resampled).
  float value_clip_sample_rate{0.0f};

  // Clip asset path(s) referenced while baking this animation.
  std::vector<std::string> clip_asset_paths;

  int32_t num_animated_joints{0};   ///< Count of unique joints animated
  int32_t num_animated_nodes{0};    ///< Count of unique scene nodes animated

  std::vector<KeyframeSampler> samplers;  ///< Keyframe data
  std::vector<AnimationChannel> channels;  ///< Property bindings

  /// Check if animation is empty
  bool empty() const { return channels.empty(); }

  /// Get number of channels
  size_t num_channels() const { return channels.size(); }

  /// Check if animation contains skeletal animation channels
  bool has_skeletal_animation() const {
    for (const auto& ch : channels) {
      if (ch.is_skeletal()) return true;
    }
    return false;
  }

  /// Check if animation contains scene node animation channels
  bool has_node_animation() const {
    for (const auto& ch : channels) {
      if (!ch.is_skeletal()) return true;
    }
    return false;
  }
};

struct Node {
  std::string prim_name;     // Prim name(element name)
  std::string abs_path;      // Absolute prim path
  std::string display_name;  // `displayName` prim meta

  NodeCategory category{NodeCategory::Group};  // High-level category (Group, Geom, Light, Camera, etc.)
  NodeType nodeType{NodeType::Xform};  // Specific type within the category

  int32_t id{-1};  // Index to node content(e.g. meshes[id] when nodeTypes ==
                   // Mesh). -1 = no corresponding content exists for this node.

#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<Node> children;
#else
  std::vector<Node> children;
#endif

  // Every node have its transform at specified timecode.
  // `resetXform` is encoded in global matrix.
  value::matrix4d local_matrix;
  value::matrix4d global_matrix;  // = local_matrix * parent_matrix (USD use
                                  // row-major(pre-multiply))

  bool has_resetXform{false}; // true: When updating the transform of the node, need to reset parent's matrix to compute global matrix.

  bool is_identity_matrix() { return is_identity(local_matrix); }

  // NOTE: Animation data has been moved to RenderScene::animations (AnimationClip).
  // Animations reference nodes by index (AnimationChannel::target_node) rather than
  // being embedded in the Node structure. This matches glTF/Three.js design and
  // allows animations to be managed independently of the scene hierarchy.

  // Instance support (AOUSD Spec 11.3.3)
  bool is_instance{false};          // True if this node is a USD instance prim
  int32_t prototype_index{-1};      // Index to prototype group (-1 = not an instance)
  int32_t instance_id{-1};          // Index to RenderScene::instances (-1 = not an instance)

  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid
};

/// Instance of geometry with unique transform (AOUSD Spec 11.3.3).
///
/// Used for USD instancing: multiple prims share the same mesh data
/// but have different transforms. The mesh_id references a shared entry
/// in RenderScene::meshes.
struct RenderInstance {
  std::string prim_name;       ///< Instance prim name (element name)
  std::string abs_path;        ///< Absolute prim path of the instance
  std::string display_name;    ///< displayName metadata

  int32_t prototype_index{-1}; ///< Index to prototype group
  int32_t mesh_id{-1};         ///< Index to RenderScene::meshes (shared)
  int32_t material_id{-1};     ///< Material index (-1 = use mesh default)
  bool has_display_color{false};
  std::array<float, 3> display_color{{1.0f, 1.0f, 1.0f}};
  bool has_display_opacity{false};
  float display_opacity{1.0f};

  value::matrix4d local_matrix;   ///< Instance local transform
  value::matrix4d global_matrix;  ///< Instance world transform

  bool visible{true};
};

// BlendShape shape target.

struct InbetweenShapeTarget {
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<vec3> pointOffsets;
  ChunkedVectorArray<vec3> normalOffsets;
#else
  std::vector<vec3> pointOffsets;
  std::vector<vec3> normalOffsets;
#endif
  float weight{0.5f};  // TODO: Init with invalid weight?
};

struct ShapeTarget {
  std::string prim_name;     // Prim name
  std::string abs_path;      // Absolute prim path
  std::string display_name;  // `displayName` prim meta

  std::vector<uint32_t> pointIndices; // Keep int array as std::vector
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<vec3> pointOffsets;
  ChunkedVectorArray<vec3> normalOffsets;
#else
  std::vector<vec3> pointOffsets;
  std::vector<vec3> normalOffsets;
#endif

  // key = weight
  std::unordered_map<float, InbetweenShapeTarget> inbetweens;
};

struct JointAndWeight {
  value::matrix4d geomBindTransform{
      value::matrix4d::identity()};  // matrix4d primvars:skel:geomBindTransform
  bool hasGeomBindTransform{false};  // true if geomBindTransform was explicitly authored in USD

  //
  // NOTE: variability of jointIndices and jointWeights are 'vertex'
  // NOTE: Values in jointIndices and jointWeights will be reordered when `MeshConverterConfig::build_vertex_indices` is set true.
  //
  std::vector<int> jointIndices;  // int[] primvars:skel:jointIndices - Keep int array as std::vector

  // NOTE: weight is converted from USD as-is. not normalized.
#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<float> jointWeights;  // float[] primvars:skel:jointWeight;
#else
  std::vector<float> jointWeights;  // float[] primvars:skel:jointWeight;
#endif

  int elementSize{1}; // # of weights per vertex
};

struct MaterialPath {
  std::string material_path;           // USD Material Prim path.
  std::string backface_material_path;  // USD Material Prim path.

  // Default RenderMaterial Id to assign when
  // material_path/backface_material_path is empty. -1 = no material will be
  // assigned.
  int default_material_id{-1};
  int default_backface_material_id{-1};

  // primvar name used for texcoords when default RenderMaterial is used.
  // Currently we don't support different texcoord for each frontface and
  // backface material.
  std::string default_texcoords_primvar_name{"st"};
};

// GeomSubset whose familyName is 'materialBind'.
// For per-face material mapping.
struct MaterialSubset {
  std::string prim_name;     // Prim name in Stage
  std::string abs_path;      // Absolute Prim path in Stage
  std::string display_name;  // `displayName` Prim meta
  int64_t prim_index{-1};    // Prim index in Stage

  // Index to RenderScene::materials
  int material_id{-1};
  int backface_material_id{-1};

  // USD GeomSubset.indices. Index to a facet, i.e. index to GeomMesh.faceVertexCounts[], in USD GeomSubset
  std::vector<int> usdIndices;

  // Triangulated indices. Filled when `MeshConverterConfig::triangualte` is true
  std::vector<int> triangulatedIndices;

  const std::vector<int> &indices() const {
    return triangulatedIndices.size() ? triangulatedIndices : usdIndices;
  }

};

// Currently normals and texcoords are converted as facevarying attribute.
struct RenderMesh {

  std::string prim_name;     // Prim name
  std::string abs_path;      // Absolute Prim path in Stage
  std::string display_name;  // `displayName` Prim metadataum

  // true: all vertex attributes are 'vertex'-varying. i.e, an App can simply use faceVertexIndices to draw mesh.
  // false: some vertex attributes are 'facevarying'-varying. An app need to decompose 'points' and 'vertex'-varying attribute to 'facevarying' variability to draw a mesh.
  bool is_single_indexable{false};

  //VertexArrayType vertexArrayType{VertexArrayType::Facevarying};

#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<vec3> points;  // varying is always 'vertex'.
#else
  std::vector<vec3> points;  // varying is always 'vertex'.
#endif

  ///
  /// Initialized with USD faceVertexIndices/faceVertexCounts in GeomMesh.
  /// When the mesh is triangulated, these attribute does not change.
  ///
  /// But will be modified when `MeshConverterCondig::build_vertex_indices` is set to true
  /// (To make vertex attributes of the mesh single-indexable)
  ///
  ///
  std::vector<uint32_t> usdFaceVertexIndices;
  std::vector<uint32_t> usdFaceVertexCounts;

  ///
  /// Triangulated faceVertexIndices, faceVerteCounts and auxiality state
  /// required to triangulate primvars in the app.
  ///
  /// trinangulated*** variables will be empty when the mesh is not
  /// triangulated.
  ///
  /// Topology could be changed(modified) when `MeshConverterCondig::build_vertex_indices` is set to true.
  ///
  std::vector<uint32_t> triangulatedFaceVertexIndices;
  std::vector<uint32_t> triangulatedFaceVertexCounts;

  std::vector<uint32_t>
      triangulatedToOrigFaceVertexIndexMap;  // used for rearrange facevertex
                                             // attrib
  std::vector<uint32_t>
      triangulatedFaceCounts;  // used for rearrange face indices(e.g GeomSubset
                               // indices)

  const std::vector<uint32_t> &faceVertexIndices() const {
    return is_triangulated() ? triangulatedFaceVertexIndices : usdFaceVertexIndices;
  }

  const std::vector<uint32_t> &faceVertexCounts() const {
    return is_triangulated() ? triangulatedFaceVertexCounts : usdFaceVertexCounts;
  }

  bool is_triangulated() const {
    return triangulatedFaceVertexIndices.size() && triangulatedFaceVertexCounts.size();
  }

  /// Free triangulation intermediate data that is only needed during
  /// ConvertMesh. After calling this, triangulatedToOrigFaceVertexIndexMap
  /// and triangulatedFaceCounts will be empty.
  void free_triangulation_intermediates() {
    { std::vector<uint32_t> tmp; triangulatedToOrigFaceVertexIndexMap.swap(tmp); }
    { std::vector<uint32_t> tmp; triangulatedFaceCounts.swap(tmp); }
  }

  // `normals` or `primvar:normals`. Empty when no normals exist in the
  // GeomMesh.
  VertexAttribute normals;

  // key = slot ID. Usually 0 = primary
  std::unordered_map<uint32_t, VertexAttribute> texcoords;
  StringAndIdMap texcoordSlotIdMap;  // st primvarname to slotID map

  //
  // tangents and binormals(single-frame only)
  //
  // When `normals`(or `normals` primvar) is not present in the GeomMesh,
  // tangents and normals are not computed.
  //
  // When `normals` is supplied, but neither `tangents` nor `binormals` are
  // supplied in primvars, Tydra computes it based on:
  // https://learnopengl.com/Advanced-Lighting/Normal-Mapping (when
  // MeshConverterConfig::compute_tangents_and_binormals is set to `true`)
  //
  // For UsdPreviewSurface, geom primvar name of `tangents` and `binormals` are
  // read from Material's inputs::frame:tangentsPrimvarName(default "tangents"),
  // inputs::frame::binormalsPrimvarName(default "binormals")
  // https://learnopengl.com/Advanced-Lighting/Normal-Mapping
  //
  VertexAttribute tangents;
  VertexAttribute binormals;

  //
  // Lazy tangent computation support.
  // When tangent_computation_deferred is true, tangents/binormals were NOT
  // computed during ConvertMesh. Call ComputeDeferredTangents() to compute
  // them on demand (e.g. at first getTangents() in WASM bindings).
  //
  bool tangent_computation_deferred{false};

  bool doubleSided{false};  // false = backface-cull.
  value::color3f displayColor{
      0.18f, 0.18f,
      0.18f};  // displayColor primvar(The number of array elements = 1) in USD.
               // default is set to the same in UsdPreviewSurface::diffuseColor
  // True when a constant (single-element) primvars:displayColor was authored on
  // the prim and stored in `displayColor` above (vs the 0.18 default). Lets a
  // consumer distinguish an authored constant color from the fallback.
  bool has_authored_displayColor{false};
  float displayOpacity{
      1.0};  // displayOpacity primvar(The number of array elements = 1) in USD
  bool is_rightHanded{true};  // orientation attribute in USD.

  VertexAttribute
      vertex_colors;  // vertex color(displayColor primvar in USD). vec3.
  VertexAttribute
      vertex_opacities;  // opacity(alpha) component of vertex
                         // color(displayOpacity primvar in USD). float

  // For vertex skinning
  JointAndWeight joint_and_weights;
  int skel_id{-1}; // index to RenderScene::skeletons

  // BlendShapes
  // key = USD BlendShape prim name.
  std::map<std::string, ShapeTarget> targets;

  // Index to RenderScene::materials
  int material_id{-1};  // Material applied to whole faces in the mesh. per-face
                        // material by GeomSubset is stored in
                        // `material_subsetMap`
  int backface_material_id{
      -1};  // Backface material. Look up `rel material:binding:<BACKFACENAME>`
            // in GeomMesh. BACKFACENAME is a user-supplied setting. Default =
            // MaterialConverterConfig::default_backface_material_purpose_name

  // Key = GeomSubset name
  std::map<std::string, MaterialSubset>
      material_subsetMap;  // GeomSubset whose famiyName is 'materialBind'

  // If you want to access user-defined primvars or custom property,
  // Plese look into corresponding Prim( stage::find_prim_at_path(abs_path) )

  //
  // Area light properties (MeshLightAPI)
  // When is_area_light = true, this mesh emits light.
  //
  // Renderer integration guide:
  //   1. Calculate effective light color: light_color * light_intensity * pow(2, light_exposure)
  //   2. Apply materialSyncMode:
  //      - "materialGlowTintsLight" (default): material.emissiveColor tints the light color
  //           finalEmission = effectiveLightColor * material.emissiveColor
  //      - "independent": material emission and light are independent
  //           finalEmission = effectiveLightColor + material.emissiveColor
  //      - "noMaterialResponse": material doesn't respond to light (only emits)
  //           finalEmission = effectiveLightColor
  //   3. If light_normalize = true, divide by surface area for energy conservation
  //
  bool is_area_light{false};  // true if MeshLightAPI is applied
  std::array<float, 3> light_color{{1.0f, 1.0f, 1.0f}};  // inputs:color (linear RGB)
  float light_intensity{1.0f};  // inputs:intensity
  float light_exposure{0.0f};  // inputs:exposure (optional, in EV)
  bool light_normalize{false};  // inputs:normalize - divide by surface area if true
  std::string light_material_sync_mode;  // inputs:materialSyncMode
                                         // "materialGlowTintsLight" (default), "independent", or "noMaterialResponse"

  // Helper: Calculate effective light color with intensity and exposure applied
  inline std::array<float, 3> get_effective_light_color() const {
    float multiplier = light_intensity * std::pow(2.0f, light_exposure);
    return {{
      light_color[0] * multiplier,
      light_color[1] * multiplier,
      light_color[2] * multiplier
    }};
  }

  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid

  ///
  /// Estimate memory usage of this RenderMesh in bytes
  ///
  size_t estimate_memory_usage() const;
};

enum class UVReaderFloatComponentType {
  COMPONENT_FLOAT,
  COMPONENT_FLOAT2,
  COMPONENT_FLOAT3,
  COMPONENT_FLOAT4,
};


// TODO: Deprecate UVReaderFloat.
// float, float2, float3 or float4 only
struct UVReaderFloat {
  UVReaderFloatComponentType componentType{
      UVReaderFloatComponentType::COMPONENT_FLOAT2};
  int64_t mesh_id{-1};   // index to RenderMesh
  int64_t coord_id{-1};  // index to RenderMesh::facevaryingTexcoords

};

struct UVTexture {
  // NOTE: it looks no 'rgba' in UsdUvTexture
  enum class Channel { R, G, B, A, RGB, RGBA };

  std::string prim_name; // element Prim name
  std::string abs_path; // Absolute Prim path
  std::string display_name; // displayName prim metadatum

  // TextureWrap `black` in UsdUVTexture is mapped to `CLAMP_TO_BORDER`(app must
  // set border color to black) default is CLAMP_TO_EDGE and `useMetadata` wrap
  // mode is ignored.
  enum class WrapMode { CLAMP_TO_EDGE, REPEAT, MIRROR, CLAMP_TO_BORDER };

  WrapMode wrapS{WrapMode::CLAMP_TO_EDGE};
  WrapMode wrapT{WrapMode::CLAMP_TO_EDGE};

  // Do CPU texture mapping. For baking texels with transform, texturing in
  // raytracer(bake lighting), etc.
  //
  // This method accounts for `tranform` and `bias/scale`
  //
  // NOTE: for R, G, B channel, The value is replicated to output[0], output[1]
  // and output[2]. For A channel, The value is returned to output[3]
  vec4 fetch_uv(size_t faceId, float varyu, float varyv);

  // `fetch_uv` with user-specified channel. `outputChannel` is ignored.
  vec4 fetch_uv_channel(size_t faceId, float varyu, float varyv,
                        Channel channel);

  // UVW version of `fetch_uv`.
  vec4 fetch_uvw(size_t faceId, float varyu, float varyv, float varyw);
  vec4 fetch_uvw_channel(size_t faceId, float varyu, float varyv, float varyw,
                         Channel channel);

  // Connected output channel(determined by connectionPath in UsdPreviewSurface)
  Channel connectedOutputChannel{Channel::RGB};

  std::set<Channel> authoredOutputChannels; // Authored `output:***` attribute in UsdUVTexture

  // bias and scale for texel value
  vec4 bias{0.0f, 0.0f, 0.0f, 0.0f};
  vec4 scale{1.0f, 1.0f, 1.0f, 1.0f};

  UVReaderFloat uvreader;
  vec4 fallback_uv{0.0f, 0.0f, 0.0f, 0.0f};

  // UsdTransform2d
  // https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_transform
  // = scale * rotate + translation
  bool has_transform2d{false};  // true = `transform`, `tx_rotation`, `tx_scale`
                                // and `tx_translation` are filled;
  mat3 transform{value::matrix3f::identity()};

  // raw transform2d value
  float tx_rotation{0.0f};
  vec2 tx_scale{1.0f, 1.0f};
  vec2 tx_translation{0.0f, 0.0f};

  // UV primvars name(UsdPrimvarReader's inputs:varname)
  std::string varname_uv;

  int64_t texture_image_id{-1};  // Index to TextureImage
  uint64_t handle{0};            // Handle ID for Graphics API. 0 = invalid

  // --- UDIM texture support ---
  // True when this UVTexture originates from a UDIM asset path
  // (e.g. `diffuse.<UDIM>.png`).
  bool is_udim{false};

  // When `is_udim` is true and tiles were combined into a single atlas
  // (`MaterialConverterConfig::combine_udim_tiles`), the referenced mesh UV
  // set must be remapped by `uv' = uv * udim_uv_scale + udim_uv_offset` so
  // that tile (u,v) lands in its atlas cell. The mesh-UV rebake pass consumes
  // these values; `texture_image_id` already points at the combined atlas.
  vec2 udim_uv_scale{1.0f, 1.0f};
  vec2 udim_uv_offset{0.0f, 0.0f};

  // When tiles are kept sparse (combine disabled), index into
  // `RenderScene::udim_textures`. -1 = not a sparse UDIM texture. In this case
  // `texture_image_id` points at a representative tile (lowest UDIM id) for
  // renderers that do not understand UDIM.
  int64_t udim_texture_id{-1};
};

// to_string functions for UVTexture nested types
std::string to_string(UVTexture::WrapMode ty);
std::string to_string(const UVTexture::Channel channel);

struct UDIMTexture {
  enum class Channel { R, G, B, RGB, RGBA };

  std::string prim_name; // element Prim name
  std::string abs_path; // Absolute Prim path
  std::string display_name; // displayName prim metadatum

  // Original UDIM asset path containing the `<UDIM>` token
  // (e.g. `diffuse.<UDIM>.png`).
  std::string asset_identifier;

  // NOTE: for single channel(e.g. R) fetch, Only [0] will be filled for the
  // return value.
  vec4 fetch(size_t faceId, float varyu, float varyv, float varyw = 1.0f,
             Channel channel = Channel::RGB);

  // key = UDIM id(e.g. 1001), value = index into RenderScene::images
  std::unordered_map<uint32_t, int32_t> imageTileIds;
};

// Spectral types, ShaderParam, PreviewSurfaceShader, OpenPBRSurfaceShader,
// MaterialTag, and RenderMaterial are now in render-data-shader.hh

// Simple Camera
//
// https://openusd.org/dev/api/class_usd_geom_camera.html
//
// NOTE: Node's matrix is used for Camera matrix
// NOTE: "Y up" coordinate, right-handed coordinate space in USD.
// NOTE: Unit uses tenths of a scene unit(i.e. [mm] by default).
//       RenderSceneConverter adjusts property value to [mm] accounting for Stage's unitsPerMeter
struct RenderCamera {

  std::string name;  // elementName in USD (e.g. "frontCamera")
  std::string
      abs_path;  // abosolute GeomCamera Prim path in USD (e.g. "/xform/camera")
  std::string display_name;

  float znear{0.1f}; // clippingRange[0]
  float zfar{1000000.0f}; // clippingRange[1]
  float verticalAspectRatio{1.0}; // vertical aspect ratio

  // for Ortho camera
  float xmag{1.0f}; // horizontal maginification
  float ymag{1.0f}; // vertical maginification

  float focalLength{50.0f}; // EFL(Effective Focal Length). [mm]
  float verticalAperture{15.2908f}; // [mm]
  float horizontalAperture{20.965f}; // [mm]
  float horizontalApertureOffset{0.0f}; // [mm]
  float verticalApertureOffset{0.0f}; // [mm]
  float exposure{0.0f};
  float focusDistance{5.0f};
  float fStop{0.0f};
  std::vector<value::float4> clippingPlanes;

  // vertical FOV in radian
  inline float yfov() {
    return 2.0f * std::atan(0.5f * verticalAperture / focalLength);
  }

  // horizontal FOV in radian
  float xfov() {
    return 2.0f * std::atan(0.5f * horizontalAperture / focalLength);
  }

  GeomCamera::Projection projection{GeomCamera::Projection::Perspective};
  GeomCamera::StereoRole stereoRole{GeomCamera::StereoRole::Mono};

  double shutterOpen{0.0};
  double shutterClose{0.0};

};

// Light source for rendering
struct RenderLight
{
  std::string name;       // elementName in USD (e.g. "sunLight")
  std::string abs_path;   // absolute Prim path in USD (e.g. "/scene/lights/sun")
  std::string display_name;

  enum class Type {
    Point,        ///< SphereLight with small radius
    Sphere,       ///< SphereLight
    Disk,         ///< DiskLight
    Rect,         ///< RectLight
    Cylinder,     ///< CylinderLight
    Distant,      ///< DistantLight (directional)
    Dome,         ///< DomeLight (environment)
    Geometry,     ///< GeometryLight
    Portal,       ///< PortalLight
  };

  enum class DomeTextureFormat {
    Automatic,
    Latlong,
    MirroredBall,
    Angular
  };

  Type type{Type::Point};

  // Common light properties (LightAPI)
  vec3 color{1.0f, 1.0f, 1.0f};       ///< Light color (linear RGB)
  float intensity{1.0f};              ///< Light intensity multiplier
  float exposure{0.0f};               ///< Exposure value (EV)
  float diffuse{1.0f};                ///< Diffuse contribution multiplier
  float specular{1.0f};               ///< Specular contribution multiplier
  bool normalize{false};              ///< Normalize by surface area

  // Color temperature
  bool enableColorTemperature{false}; ///< Use color temperature instead of color
  float colorTemperature{6500.0f};    ///< Color temperature in Kelvin

  // Transform (world space)
  mat4 transform;                     ///< World transformation matrix
  vec3 position{0.0f, 0.0f, 0.0f};    ///< World position
  vec3 direction{0.0f, -1.0f, 0.0f};  ///< Light direction (for distant/spot)

  // Type-specific parameters
  float radius{0.5f};                 ///< Sphere/Disk radius
  float width{1.0f};                  ///< RectLight width
  float height{1.0f};                 ///< RectLight height
  float length{1.0f};                 ///< CylinderLight length
  float angle{0.53f};                 ///< DistantLight angle (degrees)
  std::string textureFile;            ///< Texture for RectLight/DomeLight

  // Shaping properties (ShapingAPI)
  float shapingConeAngle{90.0f};      ///< Cone angle (degrees)
  float shapingConeSoftness{0.0f};    ///< Cone edge softness
  float shapingFocus{0.0f};           ///< Focus adjustment
  vec3 shapingFocusTint{0.0f, 0.0f, 0.0f}; ///< Focus tint color
  std::string shapingIesFile;         ///< IES profile file path
  float shapingIesAngleScale{0.0f};   ///< IES angle scale
  bool shapingIesNormalize{false};    ///< Normalize IES profile

  // Shadow properties (ShadowAPI)
  bool shadowEnable{true};            ///< Enable shadows
  vec3 shadowColor{0.0f, 0.0f, 0.0f}; ///< Shadow color
  float shadowDistance{-1.0f};        ///< Shadow distance (-1 = infinite)
  float shadowFalloff{-1.0f};         ///< Shadow falloff (-1 = no falloff)
  float shadowFalloffGamma{1.0f};     ///< Shadow falloff gamma

  // DomeLight specific
  DomeTextureFormat domeTextureFormat{DomeTextureFormat::Automatic};
  float guideRadius{1.0e5f};          ///< Radius for visualization
  int32_t envmap_texture_id{-1};      ///< Index to textures for environment map

  // GeometryLight (mesh lights with MeshLightAPI)
  int32_t geometry_mesh_id{-1};       ///< Index to meshes array for geometry lights
  std::string geometry_target_path;   ///< Authored GeometryLight relationship target
  std::string material_sync_mode;     ///< MeshLightAPI materialSyncMode

  // LTE SpectralAPI: Spectral emission support
  // Only exported if has_data() returns true
  nonstd::optional<SpectralEmission> spd_emission;  ///< wavelength:emission

  uint64_t handle{0};  // Handle ID for Graphics API. 0 = invalid

  // Light linking (LightAPI applies CollectionAPI:lightLink / :shadowLink).
  // Resolved against scene geometry by ResolveLightLinking(). When a link
  // collection is unauthored the light affects all geometry (the `*_links_all`
  // default); otherwise `*_link_mesh_indices` lists the affected RenderMesh
  // indices in `RenderScene::meshes`.
  bool light_links_all{true};                  ///< true => illuminates all geometry
  std::vector<int> light_link_mesh_indices;    ///< affected meshes when !light_links_all
  bool shadow_links_all{true};                 ///< true => casts shadows from all geometry
  std::vector<int> shadow_link_mesh_indices;   ///< shadow-casting meshes when !shadow_links_all

  /// Check if light has spectral emission data
  bool hasSpectralEmission() const {
    return spd_emission.has_value() && spd_emission->has_data();
  }
};

struct SceneMetadata
{
  std::string copyright;
  std::string comment;

  std::string upAxis{"Y"}; // "X", "Y" or "Z"
  nonstd::optional<double> startTimeCode;
  nonstd::optional<double> endTimeCode;
  double framesPerSecond{24.0};
  double timeCodesPerSecond{24.0};
  double metersPerUnit{1.0}; // default [m]

  bool autoPlay{true};

  std::string renderSettingsPrimPath;
  std::string workingColorSpace{"lin_rec709_scene"};
  // Row-major working-linear RGB to linear Rec.709.
  std::array<float, 9> workingToDisplayLinear{{
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f}};

  // If you want to lookup more thing on USD Stage Metadata, Use Stage::metas()
};

///
/// One field (e.g. "density") of a UsdVol Volume, decoded into a DENSE float
/// voxel grid. The voxel data lives in RenderScene::buffers[buffer_id]
/// (componentType Float, length dim[0]*dim[1]*dim[2]*components, x-contiguous:
/// index = c + components*(x + dim[0]*(y + dim[1]*z))).
///
struct RenderVolumeField {
  std::string field_name;        // "density", "temperature", ...
  std::string field_data_type;   // logical type ("float", "vec3f", ...)
  // UsdVol fieldClass: "levelSet", "fogVolume", "staggered" or "unknown".
  // The authored OpenVDBAsset.fieldClass when present, else the grid's class
  // metadata.
  std::string field_class = "unknown";
  int32_t components = 1;        // 1 scalar, or 3 interleaved vec3 components
  int32_t dim[3] = {0, 0, 0};    // voxel dimensions (x, y, z)
  int32_t origin[3] = {0, 0, 0}; // index-space origin (min voxel coord)
  float voxel_size[3] = {1.0f, 1.0f, 1.0f};
  float world_translation[3] = {0.0f, 0.0f, 0.0f};
  float background = 0.0f;
  int64_t buffer_id = -1;        // index into RenderScene::buffers (dense floats)
  // Object-space (pre prim-xform) AABB derived from origin/dim/voxel_size +
  // world_translation. The prim's world matrix (RenderVolume::world_matrix)
  // maps this box into world space.
  float bounds_min[3] = {0.0f, 0.0f, 0.0f};
  float bounds_max[3] = {0.0f, 0.0f, 0.0f};
};

///
/// Read a volume field's dense voxels as one scalar per cell into `out`
/// (magnitude for 3-component vector fields). Returns false when the buffer
/// is missing, not float-typed, or smaller than the field claims.
///
inline bool ReadVolumeFieldScalars(const BufferData &buffer,
                                   const RenderVolumeField &field,
                                   std::vector<float> *out) {
  if (!out || buffer.componentType != ComponentType::Float) return false;
  const size_t cells = size_t(field.dim[0]) * size_t(field.dim[1]) *
                       size_t(field.dim[2]);
  const size_t components = field.components > 0 ? size_t(field.components) : 1;
  if (cells == 0 || buffer.data.size() < cells * components * sizeof(float))
    return false;
  out->resize(cells);
  const uint8_t *bytes = buffer.data.data();
  if (bytes) {  // contiguous fast path
    const float *src = reinterpret_cast<const float *>(bytes);
    if (components == 1) {
      std::memcpy(out->data(), src, cells * sizeof(float));
    } else {
      for (size_t i = 0; i < cells; ++i) {
        const float *v = src + components * i;
        float sum = 0.0f;
        for (size_t c = 0; c < components; ++c) sum += v[c] * v[c];
        (*out)[i] = std::sqrt(sum);
      }
    }
    return true;
  }
  // Chunked storage: read each float through the array's element access.
  const size_t count = cells * components;
  std::vector<float> scalars(count);
  for (size_t i = 0; i < count; ++i) {
    uint8_t raw[sizeof(float)];
    for (size_t b = 0; b < sizeof(float); ++b) {
      raw[b] = buffer.data[i * sizeof(float) + b];
    }
    std::memcpy(&scalars[i], raw, sizeof(float));
  }
  if (components == 1) {
    *out = std::move(scalars);
  } else {
    for (size_t i = 0; i < cells; ++i) {
      float sum = 0.0f;
      for (size_t c = 0; c < components; ++c) {
        const float v = scalars[components * i + c];
        sum += v * v;
      }
      (*out)[i] = std::sqrt(sum);
    }
  }
  return true;
}

///
/// A UsdVol Volume prim converted for rendering. Carries its own world matrix
/// (renderers may iterate RenderScene::volumes directly) plus simple
/// emission/absorption shading parameters. (Full volume-material shading is a
/// TODO; "simple volume shading" per the initial scope.)
///
struct RenderVolume {
  std::string prim_name;
  std::string abs_path;
  std::string display_name;

  value::matrix4d world_matrix{value::matrix4d::identity()};

  std::vector<RenderVolumeField> fields;  // density, temperature, ...

  // Simple shading params.
  float density_scale = 1.0f;
  float albedo[3] = {0.5f, 0.5f, 0.5f};
  float emission_color[3] = {0.0f, 0.0f, 0.0f};
  float emission_scale = 0.0f;

  int material_id = -1;  // optional source material index
  uint64_t handle = 0;   // graphics API handle. 0 = invalid

  // Convenience: index of the "density" field, or first field, or -1.
  int density_field_index() const {
    for (size_t i = 0; i < fields.size(); i++) {
      if (fields[i].field_name == "density") return int(i);
    }
    return fields.empty() ? -1 : 0;
  }
};

// Simple glTF-like Scene Graph
class RenderScene {
 public:
  std::string usd_filename;

  SceneMetadata meta;

  uint32_t default_root_node{0}; // index to `nodes`.

#ifdef TYDRA_USE_CHUNKED_ARRAY
  ChunkedVectorArray<Node> nodes;
  ChunkedVectorArray<TextureImage> images;
  ChunkedVectorArray<RenderMaterial> materials;
  ChunkedVectorArray<RenderCamera> cameras;
  ChunkedVectorArray<RenderLight> lights;
  ChunkedVectorArray<UVTexture> textures;
  ChunkedVectorArray<UDIMTexture> udim_textures;
  ChunkedVectorArray<RenderMesh> meshes;
  ChunkedVectorArray<AnimationClip> animations;  ///< Animation clips (glTF/Three.js compatible)
  ChunkedVectorArray<SkelHierarchy> skeletons;
  ChunkedVectorArray<BufferData>
      buffers;  // Various data storage(e.g. texel/image data).
  ChunkedVectorArray<RenderInstance> instances;  ///< USD instancing (Spec 11.3.3)
  ChunkedVectorArray<RenderVolume> volumes;      ///< UsdVol volumes (OpenVDB)
#else
  std::vector<Node> nodes;
  std::vector<TextureImage> images;
  std::vector<RenderMaterial> materials;
  std::vector<RenderCamera> cameras;
  std::vector<RenderLight> lights;
  std::vector<UVTexture> textures;
  std::vector<UDIMTexture> udim_textures;
  std::vector<RenderMesh> meshes;
  std::vector<AnimationClip> animations;  ///< Animation clips (glTF/Three.js compatible)
  std::vector<SkelHierarchy> skeletons;
  std::vector<BufferData>
      buffers;  // Various data storage(e.g. texel/image data).
  std::vector<RenderInstance> instances;  ///< USD instancing (Spec 11.3.3)
  std::vector<RenderVolume> volumes;      ///< UsdVol volumes (OpenVDB)
#endif

  ///
  /// Estimate total memory usage of this RenderScene in bytes
  ///
  size_t estimate_memory_usage() const;

  // Variant support (inspired by glTF KHR_materials_variants)
  // Allows runtime switching between different material/geometry/property options
  // See variant-support.hh for detailed API
  std::vector<VariantGroup> variant_groups;  // Variant definitions and metadata
  std::vector<VariantSelection>
      active_selections;  // Current active variant selections
  std::map<std::string, int32_t>
      variant_group_map;  // prim_path -> variant_groups index for fast lookup

  // Get variant manager for querying and modifying variants
  // Note: This should be populated by RenderSceneConverter
  // (Currently stored as variant_groups/active_selections/variant_group_map above)

};

///
/// Resolve UsdLux light linking for every light in `scene`.
///
/// For each RenderLight, looks up its prim in `stage` and resolves the
/// `collection:lightLink` / `collection:shadowLink` collections (CollectionAPI
/// applied by LightAPI) against the scene's meshes. When a link collection is
/// authored, the light's `light_links_all` / `shadow_links_all` is cleared and
/// the corresponding `*_link_mesh_indices` is filled with the affected mesh
/// indices; otherwise the light affects all geometry (the default).
///
/// Both relationship-mode (includes/excludes) and expression-mode
/// (membershipExpression) collections are supported.
///
/// @return number of lights whose linking was resolved (had an authored
///         lightLink/shadowLink collection).
///
size_t ResolveLightLinking(const Stage &stage, RenderScene *scene);

/// Phase tag delivered alongside streamed elements during progressive
/// (streaming) conversion, so a consumer knows what arrives when.
/// See `RenderSceneSink` and `RenderSceneConverter::ConvertToRenderSceneStreaming`.
///
enum class StreamPhase {
  MaterialsAndMeshes,  ///< images, buffers, textures, materials, meshes (meshes are LOCAL space)
  Hierarchy,           ///< nodes (with world matrices), lights, cameras
  Animations,          ///< skeletons, animation clips
  Instances,           ///< render instances
  Complete             ///< conversion finished; RenderScene fully populated
};

///
/// Push "sink" for streaming/progressive scene conversion.
///
/// Set only the callbacks you care about; an unset slot is a no-op that means
/// "continue". Every callback returns bool: `true` to continue, `false` to
/// cancel conversion (matches the existing progress-callback convention).
///
/// `index` is the element's final index in the corresponding RenderScene array;
/// `abs_path` is the USD absolute prim path (empty where not applicable).
///
/// The element passed to a callback is a `const&` still owned by the converter
/// (it has not yet been moved into the RenderScene). COPY out what you need
/// (e.g. enqueue a copy for GPU upload on another thread); do not move from it.
///
/// All callbacks fire synchronously on the converting thread. If conversion runs
/// on a worker thread the sink must be thread-safe (enqueue-only); do no GPU work
/// inside it.
///
/// Ordering: on_phase(MaterialsAndMeshes) then, per prim in traversal order, that
/// prim's newly-produced images -> buffers -> textures -> udim_textures, then
/// on_material, then on_mesh (re-used/cached materials are not re-emitted). A
/// streamed mesh is in LOCAL space; its final world placement is only known once
/// the Hierarchy phase delivers the node tree (on_root_node, with world
/// matrices). Then Animations, then Instances, then on_phase(Complete) +
/// on_complete (once).
///
struct RenderSceneSink {
  // --- Phase: MaterialsAndMeshes ---
  std::function<bool(const TextureImage &, size_t index, void *)> on_image;
  std::function<bool(const BufferData &, size_t index, void *)> on_buffer;
  std::function<bool(const UVTexture &, size_t index, const std::string &abs_path, void *)>
      on_texture;
  std::function<bool(const UDIMTexture &, size_t index, void *)> on_udim_texture;
  std::function<bool(const RenderMaterial &, size_t index, const std::string &abs_path, void *)>
      on_material;
  /// Mesh is delivered in LOCAL space (no final world matrix). See on_root_node.
  std::function<bool(const RenderMesh &, size_t index, const std::string &abs_path, void *)>
      on_mesh;

  // --- Phase: Hierarchy ---
  std::function<bool(const RenderLight &, size_t index, const std::string &abs_path, void *)>
      on_light;
  std::function<bool(const RenderCamera &, size_t index, const std::string &abs_path, void *)>
      on_camera;
  /// Delivered after the node tree is built. Each Node carries local/world
  /// matrices; node.id indexes meshes/lights/cameras per its type.
  std::function<bool(const Node &root, size_t index, void *)> on_root_node;

  // --- Phase: Animations ---
  std::function<bool(const SkelHierarchy &, size_t index, const std::string &abs_path, void *)>
      on_skeleton;
  std::function<bool(const AnimationClip &, size_t index, const std::string &abs_path, void *)>
      on_animation;

  // --- Phase: Instances ---
  std::function<bool(const RenderInstance &, size_t index, const std::string &abs_path, void *)>
      on_instance;

  // --- Phase boundaries / completion ---
  std::function<bool(StreamPhase, void *)> on_phase;        ///< start of each phase
  std::function<bool(const RenderScene &, void *)> on_complete;  ///< once, at the end

  void *userdata{nullptr};
};

///

/// Dump RenderScene to string (for debugging)
///
/// Supported formats:
/// - "yaml" (default) - Human-readable YAML format with metadata header
/// - "json" - Machine-readable JSON format with metadata header
/// - "kdl"  - Original KDL format (https://kdl.dev/)
///
/// Both YAML and JSON formats include:
/// - Metadata section (format_version, generator, source_file, scene settings)
/// - Summary section (counts of nodes, meshes, materials, etc.)
/// - Full scene data (nodes, meshes, skeletons, animations, cameras, materials, textures, images, buffers)
///
std::string DumpRenderScene(const RenderScene &scene,
                            const std::string &format = "yaml");

}  // namespace tydra
}  // namespace lightusd

// Configs, vertex dedup, TextureImageLoader, RenderSceneConverterEnv,
// and RenderSceneConverter are now in render-data-converter.hh
#include "render-data-converter.hh"
