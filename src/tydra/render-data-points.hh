// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-enums.hh"
#include "render-data-math.hh"
#include "chunked-array.hh"
#include <string>
#include <vector>

namespace lightusd { namespace tydra {
struct RenderMesh;
struct RenderMaterial;

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
  // Authored curve normals evaluated with the curve basis; normalize at use.
  FloatChunked tessellated_normals;
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

}} // namespace lightusd::tydra
