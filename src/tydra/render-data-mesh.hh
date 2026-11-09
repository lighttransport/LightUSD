// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-enums.hh"
#include "render-data-vertex.hh"
#include "render-data-math.hh"
#include "chunked-array.hh"
#include <string>
#include <vector>
#include <memory>

namespace lightusd { namespace tydra { namespace next {

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

}}} // namespace lightusd::tydra::next
