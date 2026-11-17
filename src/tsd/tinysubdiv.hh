// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// tinysubdiv (tsd): clean-room, dependency-free subdivision surface library.
//
// - Uniform refinement of polygonal meshes: Catmull-Clark, Loop, Bilinear.
// - USD/AOUSD subdivision semantics: interpolateBoundary,
//   faceVaryingLinearInterpolation (all 6 modes), semi-sharp creases
//   (uniform and Chaikin creasing), corners, face holes,
//   triangleSubdivisionRule.
// - C-ish C++: POD input views, no exceptions, no RTTI.
// - Hardened: full input validation, overflow-checked sizing, per-level
//   vertex/face/corner caps. Non-manifold input is rejected (divergence from
//   OpenSubdiv, which tolerates it).
// - Numerically verified against OpenSubdiv (tests/feat/subdiv/).
//
// Notes on primvar handling:
// - "vertex"/"varying" primvars refine through VertexPrimvarView.
// - "faceVarying" primvars refine through FVarChannelView.
// - "constant" primvars are unaffected by refinement.
// - "uniform" (per-face) primvars are NOT refined here: replicate them
//   through RefinedMesh::face_source on the caller side.
//
// Scheme "none" is not a refinement: callers skip subdivision entirely
// (Refine() returns Result::UnsupportedScheme for Scheme::None).

#ifndef LIGHTUSD_TSD_TINYSUBDIV_HH_
#define LIGHTUSD_TSD_TINYSUBDIV_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {
namespace tsd {

// USD `subdivisionScheme`
enum class Scheme : uint8_t {
  CatmullClark = 0,  // "catmullClark" (USD default)
  Loop,              // "loop" (input must be all triangles)
  Bilinear,          // "bilinear"
  None,              // "none" (not subdividable)
};

// USD `interpolateBoundary`
enum class BoundaryInterpolation : uint8_t {
  EdgeAndCorner = 0,  // "edgeAndCorner" (USD default)
  EdgeOnly,           // "edgeOnly"
  None,               // "none"
};

// USD `faceVaryingLinearInterpolation`
enum class FVarLinearInterpolation : uint8_t {
  CornersPlus1 = 0,  // "cornersPlus1" (USD default)
  None,              // "none" (smooth everywhere)
  CornersOnly,       // "cornersOnly"
  CornersPlus2,      // "cornersPlus2"
  Boundaries,        // "boundaries"
  All,               // "all" (fully linear)
};

// OpenSubdiv-compatible crease decay rule.
enum class CreasingMethod : uint8_t {
  Uniform = 0,  // "uniform" (USD default): child sharpness = max(s - 1, 0)
  Chaikin,      // "chaikin"
};

// USD `triangleSubdivisionRule` (Catmull-Clark only)
enum class TriangleSubdivision : uint8_t {
  CatmullClark = 0,  // "catmullClark" (USD default)
  Smooth,            // "smooth"
};

enum class Result : uint8_t {
  Success = 0,
  InvalidArgument,    // null/zero required input, bad stride, bad level
  InvalidTopology,    // out-of-range index, count mismatch, non-manifold,
                      // non-triangle input for Loop
  UnsupportedScheme,  // Scheme::None
  LimitExceeded,      // refinement would exceed max_vertices/max_faces/
                      // max_face_vertex_indices or 32-bit index space
  OutOfMemory,
};

const char *to_string(Result result);

// Sharpness at or above this value is "infinitely sharp" (never decays).
// Matches OpenSubdiv's Sdc::Crease::SHARPNESS_INFINITE clamp of 10.
constexpr float kInfiniteSharpness = 10.0f;

// Hard cap on uniform refinement depth.
constexpr int32_t kMaxLevel = 10;

// Optional threading hook; null => serial. Kernels are scatter-free per
// output element, so results are bit-identical serial vs parallel.
typedef void (*ParallelForFn)(void *user, uint32_t count,
                              void (*body)(void *body_user, uint32_t index),
                              void *body_user);

struct Options {
  Scheme scheme = Scheme::CatmullClark;
  BoundaryInterpolation boundary = BoundaryInterpolation::EdgeAndCorner;
  CreasingMethod creasing = CreasingMethod::Uniform;
  TriangleSubdivision triangle_subdivision = TriangleSubdivision::CatmullClark;

  int32_t level = 1;  // 0..kMaxLevel; 0 = validate + passthrough copy

  // Per-level caps, checked with 64-bit arithmetic before allocating base or
  // generated child buffers.
  // `max_face_vertex_indices` caps face-corner-proportional topology/fvar
  // buffers, and defaults to the corner count of 16M quads.
  uint32_t max_vertices = 16u << 20;             // 16M
  uint32_t max_faces = 16u << 20;                // 16M
  uint32_t max_face_vertex_indices = 64u << 20;  // 64M

  // Filter hole-tagged faces out of the final output.
  bool remove_holes = true;

  ParallelForFn parallel_for = nullptr;
  void *parallel_for_user = nullptr;
};

// Non-owning view of the input mesh. All arrays are caller-owned and must
// stay valid for the duration of Refine().
struct MeshView {
  const float *points = nullptr;  // xyz interleaved, num_points * 3 floats
  uint32_t num_points = 0;

  const uint32_t *face_vertex_counts = nullptr;  // each in [3, 256]
  uint32_t num_faces = 0;

  const uint32_t *face_vertex_indices = nullptr;
  uint32_t num_face_vertex_indices = 0;  // == sum(face_vertex_counts)

  // USD subdiv tags. Optional: count 0 => absent.
  // (int32_t to match USD authoring; negative values are rejected.)
  const int32_t *corner_indices = nullptr;
  uint32_t num_corners = 0;
  const float *corner_sharpnesses = nullptr;  // length num_corners

  // Crease vertex chains: creaseIndices split into runs of creaseLengths[i]
  // (each >= 2). Sharpness count is either num_crease_lengths (per-crease)
  // or sum(crease_lengths[i] - 1) (per-edge).
  const int32_t *crease_indices = nullptr;
  uint32_t num_crease_indices = 0;
  const int32_t *crease_lengths = nullptr;
  uint32_t num_crease_lengths = 0;
  const float *crease_sharpnesses = nullptr;
  uint32_t num_crease_sharpnesses = 0;

  const int32_t *hole_indices = nullptr;  // base-face ids
  uint32_t num_holes = 0;
};

// One faceVarying primvar channel (e.g. UVs).
struct FVarChannelView {
  const float *values = nullptr;  // num_values * stride floats
  uint32_t num_values = 0;
  // Per-face-corner index into values (length num_face_vertex_indices),
  // or null for identity (then num_values == num_face_vertex_indices).
  const uint32_t *indices = nullptr;
  uint32_t stride = 0;  // 1..4
  FVarLinearInterpolation interpolation = FVarLinearInterpolation::CornersPlus1;
};

// One "vertex" or "varying" primvar (per-point values).
struct VertexPrimvarView {
  const float *values = nullptr;  // num_points * stride floats
  uint32_t stride = 0;            // 1..4
  // true => "varying" (linear interpolation); false => "vertex"
  // (smooth, scheme weights).
  bool varying = false;
};

// Refinement output. Vertex order is tsd-canonical:
// [child-of-vertex][child-of-edge][child-of-face] per level.
struct RefinedMesh {
  std::vector<float> points;  // xyz interleaved
  std::vector<uint32_t> face_vertex_counts;
  std::vector<uint32_t> face_vertex_indices;

  // Base (level-0) face id for each refined face. Use to replicate
  // "uniform" primvars and remap per-face data (e.g. GeomSubset indices).
  std::vector<uint32_t> face_source;

  // Per channel, expanded per-face-corner:
  // fvar[c].size() == face_vertex_indices.size() * stride(c).
  std::vector<std::vector<float>> fvar;

  // Per channel, per refined point:
  // vertex_primvars[p].size() == (points.size() / 3) * stride(p).
  std::vector<std::vector<float>> vertex_primvars;
};

// Uniformly refine `options.level` times.
// `fvar_channels`/`vertex_primvars` may be null when their count is 0.
// `err` may be null; on failure it receives a human-readable message.
Result Refine(const MeshView &mesh, const FVarChannelView *fvar_channels,
              uint32_t num_fvar_channels,
              const VertexPrimvarView *vertex_primvars,
              uint32_t num_vertex_primvars, const Options &options,
              RefinedMesh *out, std::string *err);

// Convenience: geometry only.
inline Result Refine(const MeshView &mesh, const Options &options,
                     RefinedMesh *out, std::string *err) {
  return Refine(mesh, nullptr, 0, nullptr, 0, options, out, err);
}

// --- Streaming refinement (memory-bounded) -----------------------------------
//
// RefineStream refines to options.level but never materializes the full output:
// it runs levels 0..N-1 with the bulk machinery, then emits the final level in
// bounded batches to a sink. Peak working memory is the level-(N-1) data plus
// one batch, so the (largest) level-N output is never resident -- this is what
// lets deep refinement fit in wasm32's 2GB. Geometry, "vertex"/"varying"
// primvars and faceVarying (linear per-corner AND smooth seam-split, via a
// per-channel split mesh) all stream.
//
// With StreamOptions::want_normals, closed-form limit normals are emitted
// (catmullClark/loop): exact at vertex-children, a normalized blend of the two
// (or n) parent-vertex limit normals at edge-/face-children. They are bounded
// by the working set (not the full level-N output) and seamless across batches.
//
// Shared final-level vertices are deduplicated only within a batch, so a vertex
// on a batch boundary is emitted by each batch that touches it (the canonical
// global child id is reported in StreamBatch::vertex_source for welding; the
// normal is keyed by that id, so duplicates always shade identically).

struct StreamPrimvar {
  uint32_t stride = 0;
  const float *values = nullptr;  // num_vertices * stride
};

// One batch of refined output. All pointers are owned by the library and valid
// only during the sink callback (reused for the next batch); copy what you need.
struct StreamBatch {
  uint32_t batch_index = 0;

  uint32_t num_vertices = 0;
  const float *positions = nullptr;  // num_vertices * 3
  const float *normals = nullptr;    // num_vertices * 3, or null
  // Canonical global child-vertex id per batch vertex (vertex-child v in
  // [0,V), edge-child V+e, face-child V+E+f at the final level). Lets callers
  // weld batch-duplicated vertices into a single shared mesh.
  const uint32_t *vertex_source = nullptr;  // num_vertices
  uint32_t num_vertex_primvars = 0;
  const StreamPrimvar *vertex_primvars = nullptr;

  uint32_t num_faces = 0;    // output faces in this batch
  uint32_t num_indices = 0;  // == sum(face_vertex_counts), batch-local
  // null when uniform (every face is a triangle if emit_triangles, else the
  // scheme's native arity 4/3).
  const uint32_t *face_vertex_counts = nullptr;
  const uint32_t *indices = nullptr;       // batch-local vertex ids
  const uint32_t *face_source = nullptr;   // per face -> base (level-0) face id

  // Linear faceVarying, per output corner (parallel to `indices`):
  // fvar[c] has num_indices * stride(c) values.
  uint32_t num_fvar = 0;
  const StreamPrimvar *fvar = nullptr;
};

// Return false to abort refinement early (RefineStream then returns Success
// with whatever was already emitted).
typedef bool (*StreamSink)(void *user, const StreamBatch *batch);

struct StreamOptions {
  uint32_t batch_faces = 4096;     // level-(N-1) parent faces per batch
  bool emit_triangles = true;      // quads -> 2 triangles for direct GPU upload
  bool want_normals = false;       // emit closed-form limit normals
  bool dedup_within_batch = true;  // share vertices inside a batch

  // Block mode: bound the WORKING set (not just the output) for very large base
  // meshes. When > 0, base faces are refined in blocks of `block_faces`, each
  // with a halo of `halo_rings` neighbouring base-face rings so owned faces keep
  // full stencil support. Subdivision's support is local: one base-face ring is
  // sufficient at any level (the floor), so `halo_rings == 0` defaults to 2 (one
  // ring of margin) and the halo -- hence the peak working set -- stays bounded
  // independent of both total mesh size AND refinement level. Blocked output
  // duplicates vertices at block borders (vertex_source is block-local);
  // analytic normals keep shading seamless. Geometry, vertex/varying primvars,
  // limit normals, and ALL faceVarying modes (linear and smooth seam-split, via
  // the per-block bulk refine) stream in block mode.
  uint32_t block_faces = 0;
  uint32_t halo_rings = 0;

  // Block mode only: stream each block's final level recursively instead of
  // materializing its full level-N refinement, so a block's peak is bounded by
  // its level-(N-1) state plus one batch. This is an OPT-IN niche trade: it saves
  // only ~10% peak heap (a block's resident state is dominated by its level-(N-1)
  // topology, which both paths hold; recursion just avoids the level-N output
  // arrays, small because the submesh is halo-dominated) and costs ~1.5-2x time
  // (the per-element streamed final level is slower than bulk's vectorized
  // refine). Worth it only when you are right at a hard memory ceiling and will
  // trade time for that last sliver. Output is bit-identical to the non-recursive
  // block path. Ignored when `want_normals` is set on a smooth scheme (exact
  // limit normals need the materialized level-N mesh, so that path stays bulk).
  bool recursive_blocks = false;
};

// Streaming refinement. `fvar_channels`/`vertex_primvars` may be null when
// their count is 0. faceVarying is emitted per-corner (StreamBatch::fvar,
// parallel to indices). ALL fvar modes stream: linear channels (the "all" mode,
// or any mode under bilinear) refine per corner; smooth seam-split channels
// carry a per-channel split mesh through the levels. (Block mode, block_faces >
// 0, instead refines each block with the bulk Refine.)
Result RefineStream(const MeshView &mesh, const FVarChannelView *fvar_channels,
                    uint32_t num_fvar_channels,
                    const VertexPrimvarView *vertex_primvars,
                    uint32_t num_vertex_primvars, const Options &options,
                    const StreamOptions &stream_options, StreamSink sink,
                    void *sink_user, std::string *err);

// Snap refined points to closed-form limit positions, honoring the same
// boundary/crease/corner state at the final level. (Milestone M5)
Result SnapToLimit(const MeshView &base_mesh, const Options &options,
                   RefinedMesh *inout, std::string *err);

// Closed-form limit normals per refined point (xyz interleaved).
// (Milestone M5)
Result ComputeLimitNormals(const MeshView &base_mesh, const Options &options,
                           const RefinedMesh &refined,
                           std::vector<float> *out_normals, std::string *err);

// ---------------------------------------------------------------------------
// Adaptive (selective) bilinear refinement.
//
// Quadtree-style refinement driven by a caller predicate: each round the
// caller sees the faces created in the previous round (the "frontier") and
// marks which of them to split; marked faces are replaced by their level-1
// bilinear children (computed with Refine, Scheme::Bilinear, on the marked
// subset only). Unmarked frontier faces are final. The result is the set of
// leaf faces; neighbouring leaves may differ in level (T-junctions), which is
// intended: the output is a partition of the base faces, not a watertight
// surface. Typical use is adaptive partitioning of a 2D parameter domain
// (positions are the parameters, z = 0), e.g. branch-and-bound over cells.
//
// Children of an axis-aligned rectangle are its four quadrants; with dyadic
// base coordinates the child coordinates are exact in binary32 down to
// kMaxAdaptiveLevel levels.
// ---------------------------------------------------------------------------

constexpr int32_t kMaxAdaptiveLevel = 20;

struct AdaptiveMesh {
  std::vector<float> points;  // xyz interleaved (unreferenced points removed)
  std::vector<uint32_t> face_vertex_counts;
  std::vector<uint32_t> face_vertex_indices;
  std::vector<uint32_t> face_offsets;  // per face: start in face_vertex_indices
  std::vector<uint32_t> face_source;   // per leaf face -> base face id
  std::vector<uint8_t> face_level;     // per leaf face: refinement depth
  // Per leaf face: index of its parent within the frontier of the round that
  // split it (i.e. relative to first_face of that round); UINT32_MAX for
  // base faces. Lets a mark callback carry per-face data to the children.
  std::vector<uint32_t> face_parent;
};

// Called once per round with the frontier faces [first_face, first_face +
// num_faces) of `mesh` (face_offsets is valid). Set refine[i] != 0 to split
// face first_face + i. Return false to abort (RefineAdaptive then returns
// Result::InvalidArgument with `mesh` left as of this round).
typedef bool (*AdaptiveMarkFn)(void *user, int32_t level,
                               const AdaptiveMesh &mesh, uint32_t first_face,
                               uint32_t num_faces, uint8_t *refine);

struct AdaptiveOptions {
  int32_t max_level = 8;          // 0..kMaxAdaptiveLevel
  uint32_t max_faces = 1u << 22;  // leaf-count cap -> Result::LimitExceeded
  ParallelForFn parallel_for = nullptr;
  void *parallel_for_user = nullptr;
};

Result RefineAdaptive(const MeshView &base_mesh, const AdaptiveOptions &options,
                      AdaptiveMarkFn mark, void *mark_user, AdaptiveMesh *out,
                      std::string *err);

}  // namespace tsd
}  // namespace lightusd

#endif  // LIGHTUSD_TSD_TINYSUBDIV_HH_
