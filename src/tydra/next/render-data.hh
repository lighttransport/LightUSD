// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Render Data Structures
//
// Design goals:
// - Minimal intermediate copies
// - Chunked arrays for WASM heap efficiency
// - GPU-friendly data layout
// - Runtime type dispatch (no templates in API)

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <utility>

#include "chunked-array.hh"

namespace lightusd {
namespace tydra {
namespace next {

//
// Forward declarations
//
class RenderScene;
struct RenderMesh;
struct RenderMaterial;
struct RenderTexture;

//
// Enums
//

enum class ColorSpace : uint8_t {
  sRGB = 0,
  Linear,
  Raw,
  ACEScg,
  Rec709,
  Rec2020,
  DisplayP3,
  Unknown
};

enum class WrapMode : uint8_t {
  Repeat = 0,
  Clamp,
  Mirror,
  Black
};

enum class ComponentType : uint8_t {
  UInt8 = 0,
  Int8,
  UInt16,
  Int16,
  UInt32,
  Int32,
  Float16,
  Float32,
  Float64
};

enum class VertexFormat : uint8_t {
  Float = 0,
  Vec2,
  Vec3,
  Vec4,
  Int,
  IVec2,
  IVec3,
  IVec4,
  UInt,
  UVec2,
  UVec3,
  UVec4,
  Matrix33,
  Matrix44
};

enum class Interpolation : uint8_t {
  Constant = 0,   // One value for entire prim
  Uniform,        // One value per face
  Vertex,         // One value per vertex (shared)
  FaceVarying,    // One value per face-vertex
  Varying         // Bilinear interpolation
};

enum class NodeType : uint8_t {
  Xform = 0,
  Mesh,
  Points,
  PointInstancer,
  Camera,
  PointLight,
  DirectionalLight,
  SpotLight,
  RectLight,
  DiskLight,
  DomeLight,
  SphereLight,
  Skeleton,
  Curves
};

//
// Curve enums (UsdGeomBasisCurves / UsdGeomNurbsCurves)
//

enum class CurveType : uint8_t {
  Linear = 0,  // Polyline segments between control points
  Cubic        // Cubic spans (see CurveBasis)
};

enum class CurveBasis : uint8_t {
  Bezier = 0,
  BSpline,
  CatmullRom
};

enum class CurveWrap : uint8_t {
  Nonperiodic = 0,
  Periodic,   // Curve closes back onto its first point
  Pinned      // bspline/catmullRom interpolate their end control points
};

enum class LightType : uint8_t {
  Point = 0,
  Directional,
  Spot,
  Rect,
  Disk,
  Dome,
  Sphere,
  Cylinder,
  Geometry
};

enum class CameraType : uint8_t {
  Perspective = 0,
  Orthographic
};

//
// Basic math types (GPU-aligned)
//

struct alignas(16) Float3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float _pad = 0.0f;

  Float3() = default;
  Float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct Float2 {
  float x = 0.0f, y = 0.0f;

  Float2() = default;
  Float2(float x_, float y_) : x(x_), y(y_) {}
};

struct alignas(16) Float4 {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;

  Float4() = default;
  Float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct alignas(64) Matrix4 {
  float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

  static Matrix4 Identity() { return Matrix4(); }
  float& operator()(int row, int col) { return m[row * 4 + col]; }
  float operator()(int row, int col) const { return m[row * 4 + col]; }
};

// UsdSkel's bind/rest transforms are matrix4d authored data. Retain these as
// doubles in RenderSkeleton so the public skeleton query does not silently
// round them through the renderer's float-precision transform type.
struct Matrix4d {
  double m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

  static Matrix4d Identity() { return Matrix4d(); }
};

//
// Vertex Attribute
//
// Stores raw vertex data with format and interpolation info
//
struct VertexAttribute {
  std::string name;
  VertexFormat format = VertexFormat::Vec3;
  Interpolation interpolation = Interpolation::Vertex;

  // Raw data storage (format-dependent)
  FloatChunked float_data;     // For Float, Vec2, Vec3, Vec4
  Int32Chunked int_data;       // For Int, IVec2, IVec3, IVec4
  UInt32Chunked uint_data;     // For UInt, UVec2, UVec3, UVec4

  // Optional index buffer (for indexed attributes)
  UInt32Chunked indices;

  bool has_indices() const { return !indices.empty(); }

  size_t element_count() const {
    switch (format) {
      case VertexFormat::Float:
        return float_data.size();
      case VertexFormat::Int:
        return int_data.size();
      case VertexFormat::UInt:
        return uint_data.size();
      case VertexFormat::Vec2:
        return float_data.size() / 2;
      case VertexFormat::IVec2:
        return int_data.size() / 2;
      case VertexFormat::UVec2:
        return uint_data.size() / 2;
      case VertexFormat::Vec3:
        return float_data.size() / 3;
      case VertexFormat::IVec3:
        return int_data.size() / 3;
      case VertexFormat::UVec3:
        return uint_data.size() / 3;
      case VertexFormat::Vec4:
        return float_data.size() / 4;
      case VertexFormat::IVec4:
        return int_data.size() / 4;
      case VertexFormat::UVec4:
        return uint_data.size() / 4;
      case VertexFormat::Matrix33:
        return float_data.size() / 9;
      case VertexFormat::Matrix44:
        return float_data.size() / 16;
    }
    return 0;
  }

  size_t memory_usage() const {
    return float_data.memory_usage() + int_data.memory_usage() +
           uint_data.memory_usage() + indices.memory_usage();
  }
};

//
// RenderMesh - GPU-ready mesh data
//
struct RenderMesh {
  RenderMesh();
  ~RenderMesh();
  RenderMesh(const RenderMesh&);
  RenderMesh& operator=(const RenderMesh&);
  RenderMesh(RenderMesh&&) noexcept;
  RenderMesh& operator=(RenderMesh&&) noexcept;

  std::string name;
  std::string prim_path;
  // True for an extent-derived low-cost stand-in emitted by a streaming sink
  // policy instead of the authored geometry payload.
  bool is_proxy = false;

  // Topology
  UInt32Chunked face_vertex_counts;   // Number of verts per face
  UInt32Chunked face_vertex_indices;  // Vertex indices

  // For pre-tessellated subdivision surfaces, maps each refined face to its
  // level-0 face. GeomSubset membership remains authored in level-0 face
  // space, so render consumers must use this ancestry when batching triangles.
  std::vector<uint32_t> subdivision_face_source;

  // Primary vertex data (always present for valid mesh)
  FloatChunked points;  // xyz interleaved, size = num_points * 3

  // Optional computed/loaded attributes
  FloatChunked normals;           // xyz, may be per-vertex or per-face-vertex
  FloatChunked tangents;          // xyzw (w = handedness sign)
  FloatChunked texcoords_0;       // Primary UV (st), xy interleaved
  FloatChunked texcoords_1;       // Secondary UV
  FloatChunked colors;            // Vertex colors (rgb or rgba)
  FloatChunked opacities;         // displayOpacity (1 float per element)

  // Authored primvar names of the two UV sets (e.g. "st", "UVMap"). Empty when
  // the set is absent. A texture names the set it samples via
  // RenderTexture::uv_primvar, so a consumer matches that against these to pick
  // the slot.
  std::string texcoords_0_name;
  std::string texcoords_1_name;

  // Interpolation modes for attributes
  Interpolation normals_interp = Interpolation::Vertex;
  Interpolation tangents_interp = Interpolation::Vertex;
  Interpolation texcoords_0_interp = Interpolation::Vertex;
  Interpolation texcoords_1_interp = Interpolation::Vertex;
  Interpolation colors_interp = Interpolation::Vertex;
  Interpolation opacities_interp = Interpolation::Vertex;

  // Additional primvars (custom attributes)
  std::vector<VertexAttribute> primvars;

  // Material assignment
  int32_t material_id = -1;  // -1 = no material

  // Per-face material (GeomSubset)
  // Maps face range [start, start+count) to material_id. When the mesh is
  // triangulated (face_triangle_offsets non-empty), the ranges are in
  // TRIANGLE space (indexing triangulated_indices/3); otherwise they are
  // authored polygon-face ranges.
  struct MaterialSubset {
    uint32_t face_start;
    uint32_t face_count;
    int32_t material_id;
  };
  std::vector<MaterialSubset> material_subsets;

  // Prefix sums of triangles emitted per authored face (size = nfaces + 1);
  // filled by triangulation. Holes/degenerate faces contribute 0 triangles.
  std::vector<uint32_t> face_triangle_offsets;

  // Faces dropped by topology sanitization: authored face numbering (and any
  // GeomSubset indices referring to it) no longer aligns when > 0.
  uint32_t sanitize_dropped_faces = 0;

  // When sanitize_dropped_faces > 0: maps each AUTHORED face index to its
  // post-sanitize face index (-1 = the face was dropped). Empty when
  // sanitization kept the authored face numbering intact.
  std::vector<int32_t> sanitize_face_remap;

  // Skinning data
  struct SkinBinding {
    UInt16Chunked joint_indices;
    FloatChunked joint_weights;
    uint32_t influences_per_vertex = 0;
    int32_t skeleton_id = -1;
    std::string skeleton_path;    // bound Skeleton prim (resolves skeleton_id)
    Matrix4 geom_bind_transform;
    // Mesh-local `skel:joints` order: when non-empty, joint_indices index
    // into this list until the converter remaps them to skeleton joint
    // order (done in the skeleton-resolve pass).
    std::vector<std::string> mesh_joint_order;
  };
  std::shared_ptr<SkinBinding> skin;

  // Blend shapes
  struct BlendShape {
    struct Inbetween {
      std::string name;
      FloatChunked point_offsets;
      float weight = 0.0f;
    };
    std::string name;
    FloatChunked point_offsets;   // xyz deltas
    FloatChunked normal_offsets;  // xyz deltas (optional)
    // Sparse target: point_offsets[k] applies to point point_indices[k].
    // Empty = dense (offsets parallel to points).
    std::vector<uint32_t> point_indices;
    float weight = 0.0f;
    std::vector<Inbetween> inbetweens;
  };
  std::vector<BlendShape> blend_shapes;

  // holeIndices: faces excluded from rendering (skipped at triangulation;
  // kept in the topology so uniform/faceVarying primvar alignment holds).
  std::vector<uint32_t> hole_faces;

  // Release the chunk-allocation slack of every chunked member (exact-size
  // tail chunks). Called once after conversion: scenes with thousands of
  // small meshes otherwise pay the 64KB minimum chunk per non-empty array.
  void compact();

  // True when any chunked member failed a (nothrow) chunk allocation during
  // conversion — the mesh is incomplete and must be dropped with an error
  // instead of silently rendering truncated data.
  bool has_alloc_failure() const;

  // Authored winding: `orientation = "leftHanded"`. Triangulation emits
  // reversed (rightHanded) winding for these meshes so consumers can treat
  // triangulated_indices as CCW uniformly; computed normals follow.
  bool left_handed = false;

  // Authored `doubleSided`. false (the USD default) = back-face cull.
  bool double_sided = false;

  // Triangulated data (computed on demand)
  UInt32Chunked triangulated_indices;
  // Per triangulated CORNER, the original face-vertex (corner) index into the
  // authored faceVarying arrays, so a consumer can index faceVarying primvars
  // (uv/normals) against the triangulated topology. Parallel to
  // triangulated_indices (same length). Empty until TriangulateMesh runs.
  UInt32Chunked triangulated_face_vertex_indices;
  bool is_triangulated = false;

  // Bounding box
  Float3 bbox_min;
  Float3 bbox_max;
  bool has_bbox = false;

  // Helpers
  size_t point_count() const { return points.size() / 3; }
  size_t face_count() const { return face_vertex_counts.size(); }
  bool has_normals() const { return !normals.empty(); }
  bool has_tangents() const { return !tangents.empty(); }
  bool has_texcoords() const { return !texcoords_0.empty(); }
  bool has_colors() const { return !colors.empty(); }
  bool has_opacities() const { return !opacities.empty(); }
  bool has_skin() const { return skin != nullptr; }
  bool has_blend_shapes() const { return !blend_shapes.empty(); }

  size_t memory_usage() const;
};

//
// RenderPoints - GPU-ready point cloud data for UsdGeomPoints
//
struct RenderPoints {
  std::string name;
  std::string prim_path;

  FloatChunked points;  // xyz interleaved, size = point_count * 3
  FloatChunked normals; // optional per-point disk/oval normals
  FloatChunked widths;  // optional per-point or constant authored width
  FloatChunked colors;  // optional rgb displayColor data
  Interpolation colors_interp = Interpolation::Vertex;
  FloatChunked opacities;  // optional scalar displayOpacity data
  Interpolation opacities_interp = Interpolation::Vertex;

  int32_t material_id = -1;

  Float3 bbox_min;
  Float3 bbox_max;
  bool has_bbox = false;

  size_t point_count() const { return points.size() / 3; }
  bool has_widths() const { return !widths.empty(); }
  bool has_colors() const { return !colors.empty(); }
  bool has_opacities() const { return !opacities.empty(); }
  void compact();
  bool has_alloc_failure() const;
  size_t memory_usage() const;
};

//
// RenderCurves - curve prim data for BasisCurves, NurbsCurves and HermiteCurves.
//
// Carries both the authored CONTROL data (control points, widths, colors,
// topology) and a render-ready TESSELLATED polyline representation produced
// by the converter (CurvesConfig::tessellation_segments samples per span;
// linear curves pass through unchanged).
//
struct RenderCurves {
  std::string name;
  std::string prim_path;

  //
  // Control (authored) data
  //
  std::vector<uint32_t> curve_vertex_counts;  // control points per curve
  FloatChunked points;   // control points, xyz interleaved
  FloatChunked widths;   // authored widths (element count per widths_interp)
  Interpolation widths_interp = Interpolation::Constant;
  FloatChunked colors;   // displayColor, rgb interleaved
  Interpolation colors_interp = Interpolation::Constant;
  FloatChunked opacities;  // displayOpacity, scalar
  Interpolation opacities_interp = Interpolation::Constant;

  CurveType type = CurveType::Cubic;
  CurveBasis basis = CurveBasis::Bezier;  // cubic BasisCurves only
  CurveWrap wrap = CurveWrap::Nonperiodic;
  bool is_nurbs = false;  // true = NurbsCurves (order/knots evaluated)
  bool is_hermite = false;  // true = HermiteCurves (point/tangent pairs)

  //
  // Tessellated polylines (render-ready output)
  //
  // Each curve becomes one polyline; periodic curves are closed by
  // duplicating the first tessellated point at the end.
  std::vector<uint32_t> tessellated_vertex_counts;  // points per polyline
  FloatChunked tessellated_points;  // xyz interleaved
  // Per-tessellated-point widths (linearly interpolated along the curve).
  // Empty when widths are absent or constant (use widths[0] instead).
  FloatChunked tessellated_widths;
  // Per-tessellated-point display colors. Empty when displayColor is absent.
  FloatChunked tessellated_colors;
  // Per-tessellated-point display opacity. Empty when absent or constant.
  FloatChunked tessellated_opacities;

  int32_t material_id = -1;

  Float3 bbox_min;
  Float3 bbox_max;
  bool has_bbox = false;

  size_t curve_count() const { return curve_vertex_counts.size(); }
  size_t control_point_count() const { return points.size() / 3; }
  size_t tessellated_point_count() const {
    return tessellated_points.size() / 3;
  }
  bool has_widths() const { return !widths.empty(); }
  bool has_colors() const { return !colors.empty(); }
  bool has_opacities() const { return !opacities.empty(); }
  void compact();
  bool has_alloc_failure() const;
  size_t memory_usage() const;
};

//
// RenderPointInstancer - render-ready instance arrays for UsdGeomPointInstancer
//
struct RenderPointInstancer {
  struct CompactInstance {
    float position[3];
    uint32_t packed_orientation;  // four signed normalized 8-bit components
    uint16_t scale[3];            // IEEE 754 binary16
    uint16_t flags;               // bit 0: visible and active
    int32_t prototype_index;
    uint32_t source_index;
  };
  static_assert(sizeof(CompactInstance) == 32,
                "Compact PointInstancer record must remain 32 bytes");

  std::string name;
  std::string prim_path;

  std::vector<std::string> prototype_paths;
  std::vector<int32_t> prototype_node_ids;     // One entry per prototype path.
  std::vector<uint32_t> prototype_mesh_offsets; // CSR offsets into prototype_mesh_ids.
  std::vector<int32_t> prototype_mesh_ids;     // Flattened mesh ids per prototype.
  std::vector<Matrix4> prototype_mesh_transforms; // Prototype-root-relative mesh transforms.
  std::vector<int32_t> proto_indices;
  std::vector<float> positions;       // xyz, size = instance_count * 3
  std::vector<float> orientations;    // quatf real,imaginary xyz, size = instance_count * 4
  std::vector<float> scales;          // xyz, size = instance_count * 3
  std::vector<float> velocities;      // xyz, size = instance_count * 3
  std::vector<float> angular_velocities; // xyz, size = instance_count * 3
  std::vector<int64_t> ids;
  std::vector<int64_t> invisible_ids;
  std::vector<int64_t> inactive_ids;
  std::vector<Matrix4> transforms;
  std::vector<uint8_t> instance_visible;  // 1 = visible/active, 0 = hidden
  std::vector<CompactInstance> compact_instances;
  uint32_t draw_start = 0;
  uint32_t draw_count = 0;

  bool valid = false;
  std::string validation_error;

  size_t instance_count() const {
    return proto_indices.empty() ? compact_instances.size()
                                 : proto_indices.size();
  }
  size_t visible_instance_count() const;
  bool has_valid_draw_range(size_t total_draw_count) const;
  bool has_orientations() const { return !orientations.empty(); }
  bool has_scales() const { return !scales.empty(); }
  bool has_velocities() const { return !velocities.empty(); }
  bool has_angular_velocities() const { return !angular_velocities.empty(); }
  bool has_ids() const { return !ids.empty(); }
  size_t prototype_count() const { return prototype_paths.size(); }
  size_t prototype_mesh_count(size_t prototype_index) const;
  bool has_valid_prototype_mesh_bindings() const;
  size_t memory_usage() const;
};

//
// RenderPointInstanceDraw - one visible PointInstancer instance mesh draw.
//
struct RenderPointInstanceDraw {
  int32_t point_instancer_id = -1;
  uint32_t instance_index = 0;
  uint32_t prototype_index = 0;
  int32_t mesh_id = -1;
  int32_t material_id = -1;
  int32_t expanded_mesh_id = -1;  // Optional duplicated mesh generated from this draw.
  Matrix4 transform;
};

struct RenderPointInstanceDrawView {
  const RenderPointInstanceDraw* draw = nullptr;
  const RenderPointInstancer* instancer = nullptr;
  const RenderMesh* mesh = nullptr;
  const RenderMesh* expanded_mesh = nullptr;
  const RenderMaterial* material = nullptr;

  bool valid() const { return draw && instancer && mesh; }
};

struct RenderPointInstanceDrawRange {
  const RenderPointInstanceDraw* data = nullptr;
  size_t size = 0;

  bool empty() const { return size == 0; }
  const RenderPointInstanceDraw* begin() const { return data; }
  const RenderPointInstanceDraw* end() const { return data ? data + size : nullptr; }
  const RenderPointInstanceDraw& operator[](size_t i) const { return data[i]; }
};

//
// Shader Parameter - value or texture reference
//
struct ShaderParam {
  // If texture_id >= 0, use texture; otherwise use value
  int32_t texture_id = -1;

  // Scalar/vector value (when texture_id < 0)
  Float4 value = {0, 0, 0, 1};

  bool is_texture() const { return texture_id >= 0; }
  bool is_value() const { return texture_id < 0; }

  float as_float() const { return value.x; }
  Float3 as_float3() const { return Float3(value.x, value.y, value.z); }
  Float4 as_float4() const { return value; }
};

//
// PreviewSurface shader parameters
//
struct PreviewSurfaceShader {
  ShaderParam diffuse_color = {-1, {0.18f, 0.18f, 0.18f, 1.0f}};
  ShaderParam emissive_color = {-1, {0, 0, 0, 1}};
  // UsdPreviewSurface spec fallback is (0,0,0) — only meaningful when
  // useSpecularWorkflow is on.
  ShaderParam specular_color = {-1, {0, 0, 0, 1}};

  ShaderParam metallic = {-1, {0, 0, 0, 0}};
  ShaderParam roughness = {-1, {0.5f, 0, 0, 0}};
  ShaderParam clearcoat = {-1, {0, 0, 0, 0}};
  ShaderParam clearcoat_roughness = {-1, {0.01f, 0, 0, 0}};

  ShaderParam opacity = {-1, {1, 0, 0, 0}};
  ShaderParam opacity_threshold = {-1, {0, 0, 0, 0}};
  ShaderParam ior = {-1, {1.5f, 0, 0, 0}};

  ShaderParam normal = {-1, {0, 0, 1, 0}};
  ShaderParam displacement = {-1, {0, 0, 0, 0}};
  ShaderParam occlusion = {-1, {1, 0, 0, 0}};

  bool use_specular_workflow = false;
};

//
// OpenPBR Surface shader (MaterialX)
//
struct OpenPBRSurfaceShader {
  // Base
  ShaderParam base_weight = {-1, {1, 0, 0, 0}};
  ShaderParam base_color = {-1, {0.8f, 0.8f, 0.8f, 1}};
  ShaderParam base_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam base_diffuse_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam base_metalness = {-1, {0, 0, 0, 0}};

  // Specular
  ShaderParam specular_weight = {-1, {1, 0, 0, 0}};
  ShaderParam specular_color = {-1, {1, 1, 1, 1}};
  ShaderParam specular_roughness = {-1, {0.3f, 0, 0, 0}};
  ShaderParam specular_ior = {-1, {1.5f, 0, 0, 0}};
  ShaderParam specular_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam specular_roughness_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam specular_rotation = {-1, {0, 0, 0, 0}};

  // Transmission
  ShaderParam transmission_weight = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_color = {-1, {1, 1, 1, 1}};
  ShaderParam transmission_depth = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_scatter = {-1, {0, 0, 0, 1}};
  ShaderParam transmission_scatter_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_dispersion = {-1, {0, 0, 0, 0}};
  ShaderParam transmission_dispersion_scale = {-1, {0, 0, 0, 0}};

  // Subsurface
  ShaderParam subsurface_weight = {-1, {0, 0, 0, 0}};
  ShaderParam subsurface_color = {-1, {0.8f, 0.8f, 0.8f, 1}};
  ShaderParam subsurface_radius = {-1, {1, 1, 1, 0}};
  ShaderParam subsurface_scale = {-1, {1, 0, 0, 0}};
  ShaderParam subsurface_anisotropy = {-1, {0, 0, 0, 0}};

  // Coat
  ShaderParam coat_weight = {-1, {0, 0, 0, 0}};
  ShaderParam coat_color = {-1, {1, 1, 1, 1}};
  ShaderParam coat_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam coat_rotation = {-1, {0, 0, 0, 0}};
  ShaderParam coat_ior = {-1, {1.5f, 0, 0, 0}};
  ShaderParam coat_affect_color = {-1, {0, 0, 0, 0}};
  ShaderParam coat_affect_roughness = {-1, {0, 0, 0, 0}};
  ShaderParam coat_anisotropy = {-1, {0, 0, 0, 0}};
  ShaderParam coat_roughness_anisotropy = {-1, {0, 0, 0, 0}};
  // OpenPBR's independently authored coat-layer normal. Keep this separate
  // from `normal`: a missing coat normal falls back to the surface normal in
  // consumers, but an authored map must retain its own image/UV descriptor.
  ShaderParam coat_normal = {-1, {0, 0, 1, 0}};

  // Sheen
  ShaderParam sheen_weight = {-1, {0, 0, 0, 0}};
  ShaderParam sheen_color = {-1, {1, 1, 1, 1}};
  ShaderParam sheen_roughness = {-1, {0.3f, 0, 0, 0}};
  ShaderParam fuzz_weight = {-1, {0, 0, 0, 0}};
  ShaderParam fuzz_color = {-1, {1, 1, 1, 1}};
  ShaderParam fuzz_roughness = {-1, {0.5f, 0, 0, 0}};
  bool fuzz_authored = false;

  // Thin-film / iridescence.
  ShaderParam thin_film_weight = {-1, {0, 0, 0, 0}};
  ShaderParam thin_film_thickness = {-1, {0, 0, 0, 0}};
  ShaderParam thin_film_ior = {-1, {1.5f, 0, 0, 0}};

  // Emission
  ShaderParam emission_luminance = {-1, {0, 0, 0, 0}};
  ShaderParam emission_color = {-1, {1, 1, 1, 1}};

  // Geometry
  ShaderParam opacity = {-1, {1, 0, 0, 0}};
  ShaderParam thin_walled = {-1, {0, 0, 0, 0}};
  ShaderParam normal = {-1, {0, 0, 1, 0}};
  ShaderParam tangent = {-1, {1, 0, 0, 0}};
  ShaderParam coat_tangent = {-1, {1, 0, 0, 0}};
  // Scalar metadata carried by common MaterialX utility nodes feeding the
  // geometry inputs. Renderers without a programmable MaterialX path can
  // still reproduce normal-map strength and retain tangent-frame intent.
  float normal_map_scale = 1.0f;
  float coat_normal_map_scale = 1.0f;
  float tangent_rotation = 0.0f;
  float coat_tangent_rotation = 0.0f;
  // Height/displacement output carried by MaterialX standard_surface graphs.
  // OpenPBR itself does not define surface displacement, but retaining it here
  // lets render consumers use the same geometry path as UsdPreviewSurface.
  ShaderParam displacement = {-1, {0, 0, 0, 0}};

  // MaterialX node graph as JSON (optional)
  std::string nodegraph_json;
};

//
// RenderMaterial
//
enum class MaterialDiagnosticKind : uint8_t {
  UnsupportedShader = 0,
  UnsupportedMaterialXNode,
  DegradedMaterial
};

struct MaterialDiagnostic {
  MaterialDiagnosticKind kind = MaterialDiagnosticKind::UnsupportedShader;
  std::string material_path;
  std::string node_path;
  std::string shader_id;
  std::string message;
};

// Authored shader inputs retained when the surface terminal cannot be fully
// evaluated. These values are intentionally neutral to current real-time
// shading, but remain available to future evaluators and diagnostics.
struct RetainedMaterialParam {
  std::string shader;
  std::string name;
  ShaderParam value;
};

struct RenderMaterial {
  RenderMaterial();
  ~RenderMaterial();
  RenderMaterial(const RenderMaterial&);
  RenderMaterial& operator=(const RenderMaterial&);
  RenderMaterial(RenderMaterial&&) noexcept;
  RenderMaterial& operator=(RenderMaterial&&) noexcept;

  std::string name;
  std::string prim_path;

  struct MaterialXConfig {
    bool authored = false;
    std::string version;
    std::string name_space;
    std::string colorspace;
    std::string source_uri;
  };

  // Shader type
  enum class ShaderType : uint8_t {
    None = 0,
    PreviewSurface,
    OpenPBR
  };
  ShaderType shader_type = ShaderType::None;

  // The material had no fully convertible surface shader and carries a
  // degraded PreviewSurface instead of being dropped (see ConvertMaterial).
  // Recognizable authored constants and texture inputs are retained when
  // possible. Conversion still SUCCEEDS, so consumers that report load
  // degradation must look here rather than relying on the return value.
  bool default_fallback = false;
  std::vector<MaterialDiagnostic> diagnostics;
  std::vector<RetainedMaterialParam> retained_params;

  // Shader data (one of these based on shader_type)
  std::shared_ptr<PreviewSurfaceShader> preview_surface;
  std::shared_ptr<OpenPBRSurfaceShader> openpbr;
  // Authored utility graph feeding a PreviewSurface terminal. Kept as a
  // separate compatibility payload because the GPU-facing parameters above
  // intentionally contain only the resolved shader values.
  std::string preview_surface_nodegraph_json;

  // Double-sided
  bool double_sided = false;

  // Alpha mode
  enum class AlphaMode : uint8_t {
    Opaque = 0,
    Mask,
    Blend
  };
  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;

  // Material displacement/volume terminals (outputs:displacement /
  // outputs:volume connections). Terminal paths remain available for geometry
  // consumers; MaterialX volume topology is retained separately below.
  bool has_displacement = false;
  std::string displacement_shader_path;
  bool has_volume = false;
  std::string volume_shader_path;
  // Retained MaterialX graph for the volume terminal. Kept separate from the
  // surface graph because a material may author both terminals.
  std::string volume_nodegraph_json;

  MaterialXConfig mtlx_config;
};

//
// RenderTexture
//
struct RenderTexture {
  std::string name;
  std::string prim_path;
  std::string asset_path;  // Original USD asset path

  // Legacy-safe GPU-compressed companion named by the `inputs:file` attribute's
  // `customData = { asset ktx2 = @foo.ktx2@ }` hint (see doc/texcomp.md). Empty
  // when unauthored. `asset_path` still points at the plain image, so unaware
  // consumers are unaffected; a consumer that can upload GPU blocks loads this
  // instead.
  std::string ktx2_hint;

  // UV transform
  Float2 offset = {0, 0};
  Float2 scale = {1, 1};
  float rotation = 0.0f;  // Radians

  // Legacy UVTexture record fields. Preserve whether a UsdTransform2d node was
  // authored separately from the effective identity transform, and retain
  // its legacy-facing degree/scale/translation components for C adapters.
  bool has_transform2d = false;
  float tx_rotation = 0.0f;  // Degrees, as authored by UsdTransform2d
  Float2 tx_scale = {1, 1};
  Float2 tx_translation = {0, 0};

  // Sparse UDIM identity. Tile IDs are 1001..1100 and index `udim_textures`;
  // image_id remains the lowest available tile for simple renderers.
  bool is_udim = false;
  int32_t udim_texture_id = -1;
  // UV remap for a combined UDIM atlas. Sparse textures retain identity.
  Float2 udim_uv_scale = {1, 1};
  Float2 udim_uv_offset = {0, 0};

  // UV set sampled by this texture (UsdPrimvarReader varname on the st
  // chain); empty = the default uv primvar ("st").
  std::string uv_primvar;

  // Sampling
  // Effective UsdUVTexture default: unauthored/useMetadata wrap is Clamp
  // (matches legacy/pxr; the converter always assigns via ParseWrapMode).
  WrapMode wrap_s = WrapMode::Clamp;
  WrapMode wrap_t = WrapMode::Clamp;

  // Bias/scale for texture values
  Float4 bias = {0, 0, 0, 0};
  Float4 scale_value = {1, 1, 1, 1};

  // Image reference
  int32_t image_id = -1;

  // Authored colorspace: colorSpace asset metadata on inputs:file when
  // present, else inputs:sourceColorSpace ("auto"/"sRGB"/"raw"/...). Web
  // consumers decode in the browser and need the authored intent.
  std::string source_color_space = "auto";

  // Canonical working space selected from RenderSettings. Texture consumers
  // can use this with source_color_space to perform the decode/gamut step
  // before shading. Data/raw textures intentionally leave this as "data".
  std::string target_color_space = "lin_rec709_scene";

  // Resolved source -> display-linear Rec.709 transform. Unlike the token
  // alone, this preserves custom ColorSpaceDefinitionAPI chromaticities for
  // native and Web texture consumers after the Stage has gone away.
  bool color_transform_valid = false;
  bool color_transform_bypass = true;
  bool source_color_is_data = false;
  float source_gamma = 1.0f;
  float source_linear_bias = 0.0f;
  float source_to_display_linear[9] = {
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f};

  // Which channel to use (for single-channel textures)
  enum class Channel : uint8_t { R = 0, G, B, A, RGB, RGBA };
  Channel output_channel = Channel::RGBA;
};

struct RenderUDIMTexture {
  struct Tile {
    uint32_t udim = 1001;
    int32_t image_id = -1;
  };
  std::string prim_name;
  std::string abs_path;
  std::string display_name;
  std::string asset_identifier;
  std::vector<Tile> tiles;
};

//
// TextureImage - actual image data
//
struct TextureImage {
  std::string name;
  std::string resolved_path;  // Filesystem path
  std::string asset_identifier;  // Authored asset path, when available

  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t channels = 4;
  uint8_t mip_levels = 1;

  ComponentType component_type = ComponentType::UInt8;
  ColorSpace color_space = ColorSpace::sRGB;

  // Raw image data (may be empty if not loaded)
  UInt8Chunked data;

  bool is_loaded() const { return !data.empty(); }
  size_t memory_usage() const { return data.memory_usage(); }
};

//
// RenderLight
//
struct RenderLight {
  std::string name;
  std::string prim_path;
  LightType type = LightType::Point;

  // Common properties
  Float3 color = {1, 1, 1};
  float intensity = 1.0f;
  float exposure = 0.0f;
  bool normalize = false;
  bool enable_color_temperature = false;
  float color_temperature = 6500.0f;
  float diffuse = 1.0f;
  float specular = 1.0f;
  float shaping_cone_angle = 90.0f;
  float shaping_focus = 0.0f;
  Float3 shaping_focus_tint = {0, 0, 0};  // color3f per UsdLux ShapingAPI
  float shaping_cone_softness = 0.0f;
  std::string shaping_ies_file;
  // Authored texture asset on RectLight. Dome textures use the resolved image
  // table and params.dome.texture_id instead.
  std::string texture_file;
  float shaping_ies_angle_scale = 0.0f;
  bool shaping_ies_normalize = false;
  std::vector<std::string> light_link_targets;
  std::vector<std::string> shadow_link_targets;
  std::vector<std::string> filter_targets;

  // Light/shadow linking resolved to RenderScene mesh indices (CollectionAPI
  // collection:lightLink / collection:shadowLink relationship form). When the
  // collection is unauthored the light links everything and *_links_all stays
  // true. Relationship and membershipExpression collection forms are both
  // resolved by the converter.
  bool light_links_all = true;
  std::vector<int32_t> light_link_mesh_indices;
  bool shadow_links_all = true;
  std::vector<int32_t> shadow_link_mesh_indices;

  // DomeLight inputs:texture:format token (matches UsdLux).
  enum class DomeTextureFormat : uint8_t {
    Automatic = 0,
    Latlong,
    MirroredBall,
    Angular,
  };

  // Transform
  Matrix4 transform;

  // Preserve the SphereLight radius when shaping makes its render type Spot.
  float spot_source_radius = 0.5f;
  float guide_radius = 1.0e5f;

  // Type-specific properties
  union {
    struct { float radius; } sphere;
    struct { float width, height; } rect;
    struct { float radius; } disk;
    struct { float angle; } spot;  // Cone angle in radians
    struct { int32_t texture_id; DomeTextureFormat texture_format; } dome;
    struct { float radius, length; } cylinder;
    struct { float angle; } distant;  // Angular size in degrees
  } params = {};

  // Shadow
  bool enable_shadow = true;
  Float3 shadow_color = {0, 0, 0};
  float shadow_distance = -1.0f;
  float shadow_falloff = -1.0f;
  float shadow_falloff_gamma = 1.0f;
};

//
// RenderCamera
//
struct RenderBackPlate {
  std::string instance_name;
  std::string image;
  std::string alpha_image;
  std::string depth_image;
  float depth_min_offset = 0.0f;
  float depth_normalizing_factor = 1.0f;
  float depth_camera_space_offset = 0.0f;
  Float2 scale_tweak = {1.0f, 1.0f};
  Float3 rotate_xyz_tweak = {0.0f, 0.0f, 0.0f};
  Float3 translate_tweak = {0.0f, 0.0f, 0.0f};
  Float3 luma_gain = {1.0f, 1.0f, 1.0f};
  Float3 luma_lift = {0.0f, 0.0f, 0.0f};
  Float3 luma_gamma = {1.0f, 1.0f, 1.0f};
  std::string plate_visibility = "solo";
};

struct RenderCamera {
  std::string name;
  std::string prim_path;
  CameraType type = CameraType::Perspective;

  // Transform
  Matrix4 transform;

  // Perspective params
  float focal_length = 50.0f;        // mm
  float horizontal_aperture = 36.0f; // mm
  float vertical_aperture = 24.0f;   // mm
  float horizontal_aperture_offset = 0.0f;
  float vertical_aperture_offset = 0.0f;

  // Orthographic params
  float ortho_width = 10.0f;

  // Clipping
  float near_clip = 0.1f;
  float far_clip = 10000.0f;

  // Depth of field / exposure
  float focus_distance = 5.0f;
  float fstop = 0.0f;           // 0 = no DoF
  float exposure = 0.0f;

  enum class StereoRole : uint8_t { Mono = 0, Left, Right };
  StereoRole stereo_role = StereoRole::Mono;
  std::vector<Float4> clipping_planes;
  std::vector<RenderBackPlate> back_plates;

  // Motion-blur shutter interval (UsdGeomCamera shutter:open/close), in
  // time-code offsets relative to the sample time.
  double shutter_open = 0.0;
  double shutter_close = 0.0;

  // Computed FOV
  float fov_y() const;
  float fov_x() const;
  float aspect_ratio() const;
};

//
// Scene Node (transform hierarchy)
//
struct SceneNode {
  std::string name;
  std::string prim_path;
  std::string prototype_path;
  NodeType type = NodeType::Xform;
  bool has_reset_xform = false;
  bool is_instance = false;

  // Transform
  Matrix4 local_transform;
  Matrix4 world_transform;

  // Reference to data (based on type)
  int32_t data_id = -1;  // Index into appropriate array

  // Hierarchy
  int32_t parent_id = -1;
  std::vector<int32_t> children;

  // Visibility
  bool visible = true;
};

//
// Animation Keyframe
//
struct Keyframe {
  double time;
  Float4 value;  // Supports up to vec4
};

//
// Animation Channel
//
struct AnimationChannel {
  enum class TargetPath : uint8_t {
    Translation = 0,
    Rotation,
    Scale,
    Weights,  // Blend shape weights
    CustomProperty
  };

  TargetPath target_path = TargetPath::Translation;
  int32_t target_node = -1;
  int32_t target_skeleton = -1;
  std::string target_prim_path;
  std::string property_name;
  std::string target_skeleton_path;
  std::vector<std::string> joint_order;
  std::vector<std::string> blend_shape_order;
  // Maps each SkelAnimation joint_order element to a Skeleton::joints index.
  // -1 means the animation joint could not be resolved on the target skeleton.
  std::vector<int32_t> joint_remap;

  // Keyframes (sorted by time)
  std::vector<Keyframe> keyframes;

  // Optional full array payload for UsdSkelAnimation channels. For frame i,
  // slice array_values at:
  //   i * element_count * value_stride
  // with element_count values, each value_stride floats wide.
  // Existing scalar keyframes keep a first-element preview for compatibility.
  std::vector<float> array_values;
  uint32_t element_count = 1;
  uint32_t value_stride = 4;
  bool is_skeletal = false;

  // Interpolation
  enum class Interpolation : uint8_t {
    Step = 0,
    Linear,
    CubicSpline
  };
  Interpolation interpolation = Interpolation::Linear;
};

//
// Animation Clip
//
struct AnimationClip {
  std::string name;
  std::string prim_path;

  double start_time = 0.0;
  double end_time = 0.0;

  std::vector<AnimationChannel> channels;

  // Source value-clip metadata retained after baking for diagnostics and web
  // feature-parity reporting.
  std::vector<std::string> clip_asset_paths;
  bool value_clip_baked = false;
};

//
// Skeleton Joint
//
struct SkeletonJoint {
  std::string name;
  std::string path;
  int32_t parent_id = -1;

  Matrix4d bind_transform;
  Matrix4d rest_transform;

  std::vector<int32_t> children;
};

//
// Skeleton
//
struct Skeleton {
  std::string name;
  std::string display_name;
  std::string prim_path;

  std::vector<SkeletonJoint> joints;
  int32_t root_joint = 0;

  // Animation reference
  int32_t animation_id = -1;
  std::string animation_source_path;
};

//
// USD Physics annotations extracted from next::Stage.
// These are descriptive data for render/tool consumers; tydra-next does not
// simulate physics.
//
struct PhysicsProperty {
  std::string name;
  std::string value;
};

struct PhysicsSceneAnnotation {
  std::string prim_path;
  Float3 gravity_direction{0.0f, -1.0f, 0.0f};
  float gravity_magnitude = 9.81f;
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsRigidBodyAnnotation {
  std::string prim_path;
  bool rigid_body_enabled = true;
  bool kinematic_enabled = false;
  std::string simulation_owner;
  Float3 velocity{0.0f, 0.0f, 0.0f};
  Float3 angular_velocity{0.0f, 0.0f, 0.0f};
  bool starts_asleep = false;
  bool has_mass = false;
  float mass = 0.0f;
  float density = 0.0f;
  Float3 center_of_mass{0.0f, 0.0f, 0.0f};
  Float3 diagonal_inertia{0.0f, 0.0f, 0.0f};
  Float4 principal_axes{0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsColliderAnnotation {
  std::string prim_path;
  bool collision_enabled = true;
  std::string simulation_owner;
  bool has_mesh_collision = false;
  std::string approximation = "none";
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsJointAnnotation {
  std::string prim_path;
  std::string type_name;
  std::string body0;
  std::string body1;
  bool has_body0 = false;
  bool has_body1 = false;
  Float3 local_pos0{0.0f, 0.0f, 0.0f};
  Float3 local_pos1{0.0f, 0.0f, 0.0f};
  Float4 local_rot0{1.0f, 0.0f, 0.0f, 0.0f};
  Float4 local_rot1{1.0f, 0.0f, 0.0f, 0.0f};
  Float3 axis{1.0f, 0.0f, 0.0f};
  float lower_limit = 0.0f;
  float upper_limit = 0.0f;
  float cone_angle0_limit = -1.0f;
  float cone_angle1_limit = -1.0f;
  float min_distance = -1.0f;
  float max_distance = -1.0f;
  bool collision_enabled = false;
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsMaterialAnnotation {
  std::string prim_path;
  float static_friction = 0.0f;
  float dynamic_friction = 0.0f;
  float restitution = 0.0f;
  float density = 0.0f;
  std::vector<PhysicsProperty> extension_properties;
};

struct PhysicsFilteredPairsAnnotation {
  std::string prim_path;
  std::vector<std::string> filtered_pair_paths;
};

struct PhysicsAnnotations {
  std::vector<PhysicsSceneAnnotation> scenes;
  std::vector<PhysicsRigidBodyAnnotation> rigid_bodies;
  std::vector<PhysicsColliderAnnotation> colliders;
  std::vector<PhysicsJointAnnotation> joints;
  std::vector<PhysicsMaterialAnnotation> materials;
  std::vector<PhysicsFilteredPairsAnnotation> filtered_pairs;
  std::vector<std::string> articulation_roots;
};

struct UnsupportedRenderable {
  std::string prim_path;
  std::string type_name;
  std::string reason;
};

enum class ConversionDisposition : uint8_t {
  PreservedUnsupported,
  Approximated,
  Failed
};

struct ConversionDiagnostic {
  ConversionDisposition disposition = ConversionDisposition::Failed;
  std::string prim_path;
  std::string message;
};

//
// RenderScene - top-level container
//
class RenderScene {
 public:
  RenderScene();
  ~RenderScene();

  // Copies share chunk-backed geometry copy-on-write. Catalog vectors are
  // copied, while optional skin/shader records remain shared and immutable;
  // transactional updates replace whole affected resources.
  RenderScene(RenderScene&&) noexcept;
  RenderScene& operator=(RenderScene&&) noexcept;
  RenderScene(const RenderScene&);
  RenderScene& operator=(const RenderScene&);

  // Scene metadata
  std::string name;
  std::string default_prim;
  float meters_per_unit = 1.0f;
  enum class UpAxis : uint8_t { Y = 0, Z } up_axis = UpAxis::Y;

  // OpenUSD color-management state. Shader color constants in this scene are
  // expressed in working_color_space. The row-major matrix maps linear values
  // from that space into Three.js/native display-linear Rec.709.
  std::string render_settings_path;
  std::string working_color_space = "lin_rec709_scene";
  float working_to_display_linear[9] = {
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f};

  // Time range
  double start_time = 0.0;
  double end_time = 0.0;
  double frames_per_second = 24.0;

  // Scene data
  std::vector<SceneNode> nodes;
  std::vector<RenderMesh> meshes;
  std::vector<RenderPoints> points;
  std::vector<RenderCurves> curves;
  std::vector<RenderPointInstancer> point_instancers;
  std::vector<RenderPointInstanceDraw> point_instance_draws;
  std::vector<RenderMaterial> materials;
  std::vector<RenderTexture> textures;
  std::vector<RenderUDIMTexture> udim_textures;
  std::vector<TextureImage> images;
  std::vector<RenderLight> lights;
  std::vector<RenderCamera> cameras;
  std::vector<AnimationClip> animations;
  std::vector<Skeleton> skeletons;
  std::vector<UnsupportedRenderable> unsupported_renderables;
  // Degradation is separate from success: unsupported data remains in the
  // source Stage, approximations have render output, and failures omit output.
  std::vector<ConversionDiagnostic> conversion_diagnostics;
  PhysicsAnnotations physics;

  // Root nodes
  std::vector<int32_t> root_nodes;

  // Lookup by path
  std::unordered_map<std::string, int32_t> node_by_path;
  std::unordered_map<std::string, int32_t> mesh_by_path;
  std::unordered_map<std::string, int32_t> points_by_path;
  std::unordered_map<std::string, int32_t> curves_by_path;
  std::unordered_map<std::string, int32_t> point_instancer_by_path;
  std::unordered_map<std::string, int32_t> material_by_path;

  // Memory usage
  size_t memory_usage() const;

  // Bounds-checked accessors
  const RenderMesh* get_mesh(int32_t mesh_id) const;
  const RenderPoints* get_points(int32_t points_id) const;
  const RenderCurves* get_curves(int32_t curves_id) const;
  const RenderMaterial* get_material(int32_t material_id) const;
  const RenderPointInstancer* get_point_instancer(int32_t instancer_id) const;
  const RenderPointInstanceDraw* get_point_instance_draw(size_t draw_id) const;
  RenderPointInstanceDrawView get_point_instance_draw_view(size_t draw_id) const;
  RenderPointInstanceDrawRange get_point_instancer_draws(int32_t instancer_id) const;
  const SceneNode* get_node(int32_t node_id) const;
  bool has_valid_point_instance_draw_ranges() const;

  // Stats
  struct Stats {
    size_t node_count;
    size_t mesh_count;
    size_t points_count;
    size_t point_cloud_point_count;
    size_t curves_count;                    // RenderCurves records
    size_t curve_count;                     // individual curves (all records)
    size_t curve_tessellated_point_count;   // total tessellated polyline points
    size_t point_instancer_count;
    size_t point_instance_count;
    size_t visible_point_instance_count;
    size_t point_instance_draw_count;
    size_t material_count;
    size_t texture_count;
    size_t image_count;
    size_t light_count;
    size_t camera_count;
    size_t animation_count;
    size_t skeleton_count;
    size_t total_vertices;
    size_t total_triangles;
    size_t memory_bytes;
  };
  Stats get_stats() const;
};

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
