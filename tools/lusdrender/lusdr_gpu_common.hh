// SPDX-License-Identifier: Apache-2.0
// lusdrender — shared helpers for the LightRT GPU backends (-vk / -vkr / -hip).
//
// The Vulkan and HIP drivers differ only in how they create their engine and
// dispatch the trace; the geometry flatten + BVH build, primary-ray generation
// (with Halton AA), and CPU shading + image write are identical. These helpers
// hold that common code so the per-backend drivers stay thin.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "lightrt_c_tri.h"  // lrt_tri_scene, lrt_ray, lrt_hit
#include "lusdr_context.hh"  // Vec3, Options, CameraFrame, RTPreviewStats

namespace lusdr {

// Flattened indexed geometry + per-triangle shading data shared by the GPU
// backends. `scene` is a BVH4 lrt_tri_scene built from flat_verts/flat_idx;
// the caller owns it and must lrt_tri_scene_free() it.
struct GpuTriScene {
  std::vector<float> flat_verts;    // 3*nverts unique positions (indexed input)
  std::vector<float> flat_attrs;    // 10*nverts: pos,nrm,uv,uv1 for shaded Vulkan RT
  std::vector<uint32_t> flat_idx;   // 3*ntris vertex ids
  uint32_t ntris = 0;
  std::vector<Vec3> base_colors;    // per-triangle base color
  std::vector<uint32_t> material_ids;  // source mesh/material per triangle
  std::vector<float> tri_uvs;          // six floats per triangle
  std::vector<uint32_t> flat_ptex;     // PtexTriRef per triangle when any mesh binds Ptex
  std::vector<Vec3> normals;        // per-triangle flat face normal (fallback)
  std::vector<Vec3> vn0, vn1, vn2;  // per-triangle vertex normals (smooth shading)
  bool has_vertex_normals{false};   // false => the vn streams are omitted
  lrt_tri_scene *scene = nullptr;
};

// Flatten `geos` (with per-mesh `base_colors`) into a single indexed mesh and
// optionally build the CPU BVH4 scene. Returns false (with a stderr diagnostic)
// when there is no geometry or the requested BVH build fails. When
// build_cpu_scene is false, `out->scene` stays null; this is used by the Vulkan
// hardware ray-query path, which builds a Vulkan AS directly from flat_verts.
bool BuildGpuTriScene(const std::vector<Vec3> &base_colors,
                      const std::vector<RTPreviewStats::MeshGeometry> &geos,
                      int threads, bool build_cpu_scene, GpuTriScene *out,
                      lrt_tri_quality quality = LRT_TRI_BUILD_DEFAULT);

// Build one bounded, locally-indexed geometry batch. Fully consumed source
// meshes are released in-place, so Vulkan/HIP chunk callers do not retain the
// original position/index arrays alongside the accumulated shading data.
bool BuildGpuTriChunk(const std::vector<Vec3> &base_colors,
                      std::vector<RTPreviewStats::MeshGeometry> &geos,
                      size_t limit, size_t *mesh_index, size_t *tri_index,
                      std::vector<Vec3> *chunk_colors,
                      std::vector<RTPreviewStats::MeshGeometry> *chunk_geos,
                      size_t *chunk_triangles);

// Build the CPU LightRT BVH for an already-flattened GpuTriScene. Used by the
// compute backends and as a fallback when Vulkan ray query is unavailable/fails.
bool BuildGpuCpuScene(int threads, GpuTriScene *out,
                      lrt_tri_quality quality = LRT_TRI_BUILD_DEFAULT);

// Append per-triangle smooth-normal metadata from a chunk, promoting the
// destination to zero-filled streams only when some chunk actually needs it.
// prior_triangles is explicit: colors may already contain the current chunk.
void AppendGpuSmoothNormals(const GpuTriScene &src, GpuTriScene *dst,
                            size_t prior_triangles);

// Validate a GPU trace dispatch dimensions before allocating rays or passing the
// count to LightRT's GPU APIs, which all take a uint32_t ray count.
bool ValidateGpuFrameSize(int w, int h, int spp, const char *backend,
                          size_t *nrays);

// GPU hit/material IDs are 32-bit. Reject an aggregate scene that would make
// chunk hit offsets wrap before any backend allocation or trace dispatch.
bool ValidateGpuTriangleCount(size_t ntris, const char *backend);

// --- True two-level (instanced) GPU scene -----------------------------------
//
// For the -vkr two-level path: prototype geometry stored ONCE (object/prototype
// space) plus a list of placements (per-instance transform + prototype id). The
// GPU builds one BLAS per prototype and one TLAS instance per placement; the hit
// id decodes as instance = prim_id / stride, prototypeLocalTri = prim_id % stride.
struct GpuInstProto {
  std::vector<float> verts;          // 3*nverts prototype-local positions
  std::vector<uint32_t> idx;         // 3*ntris vertex ids
  uint32_t ntris = 0;
  std::vector<Vec3> normals;         // per-tri flat face normal (prototype space)
  std::vector<Vec3> vn0, vn1, vn2;   // per-tri vertex normals (prototype space)
  Vec3 base_color{0.5f, 0.5f, 0.5f};
  // Ptex base-colour lane: per-triangle face reference (parallel to idx/3) and
  // the resolved lazy texture id. Empty/-1 when the prototype's material does
  // not bind a Ptex base colour. The instanced path uses mip 0; the chunked and
  // descriptor paths retain their per-face mip/footprint logic.
  std::vector<uint32_t> ptex_tri;
  int32_t ptex_tex_id{-1};
};
struct GpuInstPlacement {
  float o2w[12];  // object->world 3x4 row-major (world = M*[p;1])
  float n2w[9];   // normal matrix: inverse-transpose of o2w's upper 3x3, row-major
  uint32_t proto = 0;
};
struct GpuInstancedScene {
  std::vector<GpuInstProto> protos;
  std::vector<GpuInstPlacement> insts;
  uint32_t stride = 0;  // = max prototype ntris (the prim_id stride); set by build
};

// Build the two-level GPU acceleration structure (lrt_vk_rtx_scene_build_instanced)
// from `scene`, trace `camera`'s primary rays, decode the instanced hits, and write
// the image. Sets scene.stride from the build. Returns false (leaving the scene
// untouched) when ray query is unavailable or the build fails, so the caller can
// fall back to the flat path. Defined only under HAVE_VULKAN.
bool RunVulkanLightRTInstanced(
    const Options &opt, GpuInstancedScene &scene,
    const std::vector<lusdr::Texture> &textures, const CameraFrame &camera,
    int height);

// One traced primary-ray sample already decoded to two-level scene coordinates:
// `inst` = placement index, `local` = prototype-local triangle, (u,v) = hit
// barycentrics. `valid` is false for a miss. The narrow (32-bit prim_id) and wide
// (separate instanceId/prim) traces both decode into this, so the shader below is
// independent of the hit wire format.
struct InstSampleHit {
  uint32_t inst = 0;
  uint32_t local = 0;
  float u = 0.0f, v = 0.0f, t = 0.0f;
  bool valid = false;
};

// Shade the decoded `hits` for the two-level scene: shade each sample's prototype
// triangle and transform its object-space normal by the instance's normal matrix.
// Same lighting model + spp averaging as ShadeAndWriteImage. `hits` is indexed
// identically to `rays` ((y*w+x)*spp + s). Returns false on write failure.
bool ShadeAndWriteImageInstanced(
    const Options &opt, const GpuInstancedScene &s,
    const std::vector<lusdr::Texture> &textures,
    const std::vector<lrt_ray> &rays, const std::vector<InstSampleHit> &hits,
    int w, int h, int spp);

// Generate w*h*spp primary rays for `camera`, ray index = (y*w + x)*spp + s,
// each sample offset by a Halton(2,3) sub-pixel jitter for anti-aliasing.
void GenerateCameraRays(const CameraFrame &camera, int w, int h, int spp,
                        std::vector<lrt_ray> *rays);

// Shade the traced `hits` (smooth normal + key light + camera headlight +
// ambient), average the spp samples per pixel, and write the image to
// opt.output. Returns false on image-write failure.
using GpuTraceContinuation = std::function<bool(
    const std::vector<lrt_ray>&, std::vector<lrt_hit>*)>;
using GpuTrianglePositions = std::function<bool(uint32_t, Vec3*)>;
bool ShadeAndWriteImage(const Options &opt, const GpuTriScene &s,
                        const std::vector<lrt_ray> &rays,
                        const std::vector<lrt_hit> &hits, int w, int h, int spp,
                        const std::vector<ResolvedMat> *materials = nullptr,
                        const std::vector<Texture> *textures = nullptr,
                        const GpuTraceContinuation& trace_continuation = {},
                        const GpuTrianglePositions& triangle_positions = {});

// Shade native Gaussian ellipse hits directly from chunk-local metadata. This
// avoids flattening all splat colors/normals into a second scene-wide
// GpuTriScene after the chunked Vulkan trace.
bool ShadeAndWriteGaussianImage(
    const Options &opt, const std::vector<EllipseSceneChunk> &chunks,
    const std::vector<lrt_ray> &rays, const std::vector<lrt_hit> &hits, int w,
    int h, int spp);

}  // namespace lusdr
