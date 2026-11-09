// SPDX-License-Identifier: Apache-2.0
// lusdview - CPU linear-blend skinning + blendshape deformation.
//
// Tydra bakes a rest-pose mesh plus separate skeleton/animation data; it does
// not deform geometry for skinning, and it does not emit animated blendshape
// weights. This module fills both gaps on the CPU: given a RenderScene already
// converted at time `timecode` (so node transforms/value-clips are resolved)
// and the source Stage, it deforms each skinned / blendshaped RenderMesh's
// points in place so the existing pack + upload path renders the posed mesh.
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gpu_scene.hh"
#include "stage.hh"
#include "tydra/render-data.hh"
#include "value-types.hh"

namespace lusdview {

// In-between blendshape samples for one BlendShape: (weight, offsets) ascending
// by weight; offsets are parallel to the BlendShape's pointIndices (USD inbetween
// semantics, same indexing as the primary `offsets`).
using InbetweenSamples =
    std::vector<std::pair<float, std::vector<lightusd::legacy::value::vector3f>>>;

// Collect in-between samples for every BlendShape in the stage, keyed by the
// BlendShape's prim name (== morph target name == SkelAnimation weight key). The
// tydra converter does not carry in-betweens, so they are read here from the
// `inbetweens:*` attributes (vector3f[] value + a `weight` attr-meta).
std::map<std::string, InbetweenSamples> CollectBlendShapeInbetweens(
    const lightusd::legacy::Stage& stage);

// True if any mesh in `render` carries skeletal skinning data or blendshape
// targets (i.e. would deform over time). Cheap topology check.
bool SceneHasDeformation(const lightusd::legacy::tydra::RenderScene& render);
bool SceneHasSkeletalSkinning(const lightusd::legacy::tydra::RenderScene& render);
bool SceneHasBlendShapes(const lightusd::legacy::tydra::RenderScene& render);
bool SceneHasNonSkeletalAnimation(const lightusd::legacy::tydra::RenderScene& render);
int MaxSkinInfluenceCount(const lightusd::legacy::tydra::RenderScene& render);

// Per-skeleton skinning matrices:
// skinMat[j] = inverse(bind[j]) * posedWorld[j] (USD row-vector convention).
bool BuildSkinningMatrices(const lightusd::legacy::tydra::RenderScene& render,
                           int skelId, double timecode,
                           std::vector<lightusd::legacy::value::matrix4d>* skinOut);

// Per-skeleton animated joint world matrices in skeleton/model space. This is
// the same posed hierarchy used to build skinning matrices, but without the
// inverse-bind step, for drawing joint overlays/debug bones.
bool BuildSkeletonJointWorlds(const lightusd::legacy::tydra::RenderScene& render,
                              int skelId, double timecode,
                              std::vector<lightusd::legacy::value::matrix4d>* worldOut);

// Per-mesh composed skinning matrices (geomBind * skinMat * skeletonWorld *
// inverse(meshWorld), USD row-vector convention -- exactly what
// ApplySkinningToVertices consumes), for mesh `dm` at `timecode`. `skinCache`
// memoizes BuildSkinningMatrices per skeleton id across calls sharing one
// cache (e.g. looping a scene's meshes for one frame); pass a fresh/empty map
// if that reuse isn't wanted. False (composed left empty) if `dm` isn't
// skinned or its skeleton has no matrices at `timecode`.
bool BuildComposedSkinningMatrices(
    const lightusd::legacy::tydra::RenderScene& render, const DrawMeshCPU& dm,
    double timecode,
    std::unordered_map<int, std::vector<lightusd::legacy::value::matrix4d>>* skinCache,
    std::vector<lightusd::legacy::value::matrix4d>* composed);

// Linear-blend-skin `dm`'s vertices (rest pose, read from *verts on input) in
// place using `mats` (one composed matrix per joint, BuildComposedSkinningMatrices'
// output; indexed by dm.jointIdx's absolute bone-matrix rows via dm.skinMatrixBase).
// No-op (returns false) if `dm` carries no skin attributes matching *verts's size.
bool ApplySkinningToVertices(const DrawMeshCPU& dm,
                             const std::vector<lightusd::legacy::value::matrix4d>& mats,
                             std::vector<DrawVertex>* verts);

// Pack the per-frame GPU bone texture from `render` into `frame`, using the
// per-mesh skin layout stored in `draw`. Also updates skinned mesh AABBs in
// `draw`; scene bounds stay stable during playback to avoid grid/helper scale
// wobble.
//
// The raster path morphs in the GPU vertex shader (BuildMorphChannelWeights +
// the renderer's morph buffers), so no morphed vertices are produced here -- but
// the BOUNDS have to see the morph anyway, or a morph-only mesh keeps its rest box
// and the grid / depth ramp / auto-fit sit where the mesh no longer is. Pass
// `stage` (and any manual weight overrides) to include it; without a stage the
// bounds are skin-only, as they used to be.
bool BuildGpuSkinningFrame(
    const lightusd::legacy::tydra::RenderScene& render, DrawScene* draw, double timecode,
    SkinningFrameCPU* frame, bool updateSkinnedHelpers,
    const lightusd::legacy::Stage* stage = nullptr,
    const std::unordered_map<std::string, float>* blendOverride = nullptr);

struct RtSkinnedMeshUpload {
  int meshIndex{-1};
  std::vector<DrawVertex> vertices;
};

// Deterministic parallel-for over [0, n): contiguous ranges, one per worker,
// no shared accumulation -- each index runs exactly the serial loop body, so
// the result is BIT-IDENTICAL to serial regardless of thread count. That is
// the contract that lets the per-frame RT deform be threaded without touching
// any of the byte-parity oracles (refit-vs-rebuild, gpu-vs-cpu-bake).
//
// grainMin is the MINIMUM indices per worker, set from the measured cost of
// the loop body. Measured 2026-07-16 (Ryzen/Linux, medians over 120 poses --
// single-shot timings of this path swing 2-4x, do not trust them): the
// 102k-vert LBS deform runs 3.2 ms serial vs 1.67 ms with 7 workers at grain
// 16384, byte-identical; at 1M verts threading is ~3x. Thread wake latency is
// real (~0.3 ms plus outliers) but amortizes once a worker carries a few tens
// of thousands of deform-weight indices; use larger grains for cheaper loop
// bodies (see RecomputeSmoothNormals).
template <typename F>
inline void DeformParallelFor(size_t n, size_t grainMin, F&& fn) {
  const unsigned hw = std::thread::hardware_concurrency();
  const size_t maxWorkers = grainMin ? (n + grainMin - 1) / grainMin : 1;
  const size_t workers = std::min<size_t>(hw ? hw : 1, maxWorkers);
  if (workers <= 1 || n == 0) {
    if (n) fn(size_t(0), n);
    return;
  }
  std::vector<std::thread> ts;
  ts.reserve(workers);
  const size_t chunk = (n + workers - 1) / workers;
  for (size_t w = 0; w < workers; ++w) {
    const size_t b = w * chunk;
    const size_t e = std::min(n, b + chunk);
    if (b >= e) break;
    ts.emplace_back([&fn, b, e]() { fn(b, e); });
  }
  for (std::thread& t : ts) t.join();
}

// Ray-query RT cannot use the raster vertex shader's morph/skinning path: the
// BLAS is built from actual vertex buffers. Build per-mesh posed DrawVertex
// buffers from the retained rest DrawScene so the renderer can update the VBO and
// rebuild the acceleration structure without re-running Tydra conversion.
// `skipMeshes` (optional) excludes meshes the GPU compute-skinning path already
// handled this frame (see BuildRtGpuSkinUpdates).
bool BuildRtSkinnedMeshVertices(
    const lightusd::legacy::Stage& stage,
    const lightusd::legacy::tydra::RenderScene& render, DrawScene* draw,
    double timecode,
    const std::unordered_map<std::string, float>* blendOverride,
    bool updateSkinnedHelpers,
    std::vector<RtSkinnedMeshUpload>* outUploads,
    const std::unordered_set<int>* skipMeshes = nullptr);

// One GPU-compute-skinnable mesh's per-frame inputs: the composed skinning
// matrices (geomBind * skinMat * skeletonWorld * inverse(meshWorld), 16 floats
// each, row-major, row-vector p*M — exactly what ApplySkinningToVertices uses)
// plus a conservative posed object-space bound (union of the per-joint
// transformed rest prototype box; the skinned mesh is a convex combination of
// per-joint transforms, so it is contained in that union).
struct RtGpuSkinUpdate {
  int meshIndex{-1};
  int matrixBase{0};  // absolute joint id - matrixBase indexes mats
  int jointCount{0};
  std::vector<float> mats;  // 16 floats per joint
  float aabbMin[3]{0, 0, 0};
  float aabbMax[3]{0, 0, 0};
};

// Partition per-frame RT skinning between the GPU compute path and the CPU
// path: emit composed matrices + conservative posed bounds for every
// GPU-ELIGIBLE mesh — pure <= 4-influence skeletal skinning, no morphs, no
// displacement bake. Positions and normals are skinned in the compute shader
// (normals via the weighted joint matrices, the raster deform.glsl
// convention); the CPU path instead regenerates smooth normals on the posed
// surface, so GPU output is close but not bit-identical on smooth-shaded
// meshes. Handled meshes are recorded so BuildRtSkinnedMeshVertices can skip
// them; their dm/scene bounds are updated from the conservative bound.
// Ineligible meshes are left for the CPU path.
bool BuildRtGpuSkinUpdates(const lightusd::legacy::tydra::RenderScene& render,
                           DrawScene* draw, double timecode,
                           std::vector<RtGpuSkinUpdate>* outUpdates,
                           std::unordered_set<int>* outHandled);

// Update each draw mesh's world transform to its value at `timecode`, evaluated
// from the Stage's xform hierarchy. For scenes whose node transforms animate
// alongside GPU skeletal skinning (e.g. a moving SkelRoot), this poses node
// motion without a geometry re-pack. Returns true if any world changed; the
// renderer must be told separately (Renderer::updateMeshWorld).
bool UpdateAnimatedMeshWorlds(const lightusd::legacy::Stage& stage, DrawScene* draw,
                              double timecode);

// Deform every skinned / blendshaped mesh in `render` to its pose at
// `timecode`: applies animated blendshape offsets (weights read from the
// Stage's SkelAnimation prims) then linear-blend skinning (joint poses from
// `render.animations`). Mesh `points` are overwritten with the posed result and
// `normals` are cleared so the packer regenerates them from the posed geometry.
// No-op for meshes without skinning/blendshapes. `stage` must be the source of
// `render`.
//
// `blendOverride` (optional, by BlendShape name) supplies manual weights from the
// blend editor that replace the animated weight -- this is the ray-traced /
// CPU-skinned path's equivalent of BuildGpuSkinningFrame's override. In-between
// shapes are honored (the baked offset is interpolated through them).
void DeformSkinnedMeshes(
    const lightusd::legacy::Stage& stage, lightusd::legacy::tydra::RenderScene& render,
    double timecode,
    const std::unordered_map<std::string, float>* blendOverride = nullptr);

// Compute per-mesh GPU-morph channel coefficients for every blendshaped mesh at
// `timecode` (animated weights overlaid by `blendOverride`). Reproduces
// ApplyMorphTarget's piecewise-lerp exactly. Output: (meshIndex, coeffs) per mesh
// with morph channels; the renderer uploads `coeffs` via updateMorphWeights so the
// vertex shader applies the morph (no CPU vertex morph / VBO re-upload).
void BuildMorphChannelWeights(
    const lightusd::legacy::Stage& stage, const DrawScene& draw, double timecode,
    const std::unordered_map<std::string, float>* blendOverride,
    std::vector<std::pair<int, std::vector<float>>>* out);

}  // namespace lusdview
