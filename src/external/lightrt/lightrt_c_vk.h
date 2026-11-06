/*
 * lightrt_c_vk.h — Vulkan GPU interop for the LightRT C11 triangle kernel.
 *
 * Two directions, both built on Vulkan compute (the loader, lightrt_vkew, opens
 * libvulkan at runtime — there is NO Vulkan SDK header or link-time dependency):
 *
 *   Path A  CPU build -> GPU trace : lrt_vk_trace_scene() uploads a scene built
 *           by lrt_tri_scene_build() and traverses it with a compute shader that
 *           mirrors the scalar CPU kernel, returning identical hits (within fp
 *           tolerance).
 *
 *   Path B  GPU build -> CPU trace : lrt_vk_build_scene() runs the LBVH front
 *           end (centroids + Morton codes) on the GPU, finishes the hierarchy on
 *           the CPU, and returns a heap lrt_tri_scene traversable with the normal
 *           lrt_tri_intersect1() / etc.
 *
 * Everything degrades gracefully: if no Vulkan loader/device is available,
 * lrt_vk_engine_create() returns NULL and the caller falls back to the CPU
 * kernel. An engine is created once and may be reused across calls; a single
 * engine is NOT safe to use from multiple threads concurrently.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTRT_C_VK_H
#define LIGHTRT_C_VK_H

#include <stddef.h>
#include <stdint.h>

#include "lightrt_c.h"     /* lrt_result */
#include "lightrt_c_tri.h" /* lrt_tri_scene, lrt_ray, lrt_hit, lrt_tri_layout */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lrt_vk_engine lrt_vk_engine;

/* Capability flags reported by lrt_vk_engine_caps(). */
typedef enum lrt_vk_caps {
    LRT_VK_CAP_COMPUTE        = 1u << 0, /* always set when an engine exists      */
    LRT_VK_CAP_BUFFER_ADDRESS = 1u << 1, /* bufferDeviceAddress usable            */
    LRT_VK_CAP_ACCEL_STRUCT   = 1u << 2, /* VK_KHR_acceleration_structure loaded  */
    LRT_VK_CAP_RAY_QUERY      = 1u << 3  /* VK_KHR_ray_query usable in compute     */
} lrt_vk_caps;

typedef struct lrt_vk_engine_options {
    int device_index;     /* explicit physical-device index, or -1 = auto-pick   */
    int prefer_discrete;  /* when auto-picking, prefer a discrete GPU            */
    int want_ray_tracing; /* try to enable AS + ray_query extensions if present  */
    uint32_t flags;       /* reserved; pass 0                                    */
} lrt_vk_engine_options;

/* Create an engine (calls vkewInit internally). Returns NULL and, when err is
 * non-NULL, stores the reason, on any failure: no libvulkan, no instance, no
 * compute-capable device. opts may be NULL for defaults (auto device, no RT). */
lrt_vk_engine *lrt_vk_engine_create(const lrt_vk_engine_options *opts,
                                    lrt_result *err);
void lrt_vk_engine_destroy(lrt_vk_engine *e);

/* Bitmask of lrt_vk_caps for the created device. */
uint32_t lrt_vk_engine_caps(const lrt_vk_engine *e);

/* Selected physical-device name (e.g. "NVIDIA GeForce RTX 3070"). */
const char *lrt_vk_engine_device_name(const lrt_vk_engine *e);

/* Largest DEVICE_LOCAL memory heap (VRAM) in bytes, queried with a throwaway
 * Vulkan instance (no engine needed). prefer_discrete!=0 favors a discrete GPU.
 * Returns 0 if Vulkan or a suitable device is unavailable. Lets callers size GPU
 * memory budgets before building anything. */
uint64_t lrt_vk_device_local_bytes(int prefer_discrete);

/* Human-readable message for the last failed call on this engine. */
const char *lrt_vk_engine_last_error(const lrt_vk_engine *e);

/* --- Path A: trace a CPU-built scene on the GPU ---------------------------
 *
 * Uploads s (via its position-independent LRTS serialization) and traverses n
 * rays with a compute shader. Writes n hits to out. Returns the number of rays
 * that hit geometry, or -1 on error (err set). Plain triangle and serialized
 * point/ellipse scenes with BVH4/BVH8 layout are supported; quantized/curve/
 * user scenes are rejected. Results match lrt_tri_intersect1 within fp
 * tolerance. */
int lrt_vk_trace_scene(lrt_vk_engine *e, const lrt_tri_scene *s,
                       const lrt_ray *rays, uint32_t n, lrt_hit *out,
                       lrt_result *err);

/* --- Path B: build a BVH with a GPU front end, return a CPU scene ----------
 *
 * vertices = 9*ntris floats (v0 v1 v2 per triangle). Computes centroids and
 * 30-bit Morton codes on the GPU, then finishes the LBVH (radix sort + collapse
 * + leaf packing) on the CPU and returns a heap scene. layout must be BVH4 or
 * BVH8 (others are rejected). Free the result with lrt_tri_scene_free(). Returns
 * 0 on success, -1 on error (err set). The result is traversable with the normal
 * lrt_tri_* query functions and matches a FAST CPU build. */
int lrt_vk_build_scene(lrt_vk_engine *e, const float *vertices, uint32_t ntris,
                       lrt_tri_layout layout, lrt_tri_scene **out,
                       lrt_result *err);

/* --- Hardware ray tracing (VK_KHR_ray_query) ------------------------------
 *
 * Build a real acceleration structure (BLAS + identity-instance TLAS) on the GPU
 * from the triangle soup (9*ntris floats) and trace n rays against it with a
 * ray_query compute shader. Writes n hits to out; returns the number of rays
 * that hit, or -1 on error. Requires an engine created with want_ray_tracing=1
 * on an RT-capable device (check lrt_vk_engine_caps() & LRT_VK_CAP_RAY_QUERY);
 * otherwise returns -1 with LRT_RESULT_NOT_BUILT.
 *
 * Hits match a Moller-Trumbore CPU trace within fp tolerance: t is in units of
 * |dir|, (u,v) are hit barycentrics, prim_id is the triangle index. Unlike Path
 * A/B this takes raw triangles (the AS is a vendor-opaque blob and cannot be fed
 * back into lrt_tri_scene), so it is a trace-only backend.
 *
 * IMPORTANT: this rebuilds the acceleration structure on every call. It is a
 * convenience for a single batch — do NOT call it once per ray/pixel in a
 * render loop. To trace many batches against the same geometry, build a resident
 * lrt_vk_rtx_scene ONCE (below) and reuse it; that is both correct and orders of
 * magnitude faster. */
int lrt_vk_trace_scene_rtx(lrt_vk_engine *e, const float *vertices, uint32_t ntris,
                           const lrt_ray *rays, uint32_t n, lrt_hit *out,
                           lrt_result *err);

/* As lrt_vk_trace_scene_rtx, but for INDEXED geometry: `vertices` is 3*nverts
 * floats of unique positions and `indices` is 3*ntris vertex ids — so callers
 * with shared/welded vertices need not expand to a 9*ntris soup. prim_id is the
 * triangle index (0..ntris-1). Same per-call AS-rebuild caveat as above. */
int lrt_vk_trace_scene_rtx_indexed(lrt_vk_engine *e, const float *vertices,
                                   uint32_t nverts, const uint32_t *indices,
                                   uint32_t ntris, const lrt_ray *rays, uint32_t n,
                                   lrt_hit *out, lrt_result *err);

/* Resident ray-tracing scene: build the acceleration structure ONCE and trace
 * many ray batches against it. Unlike the one-shot lrt_vk_trace_scene_rtx (which
 * rebuilds the AS per call), the BLAS+TLAS stay device-resident and the trace
 * uses device-local ray/hit buffers (bulk staged), so lrt_vk_rtx_scene_trace
 * measures GPU traversal throughput rather than per-batch build + transfer cost.
 * The engine must outlive the scene.
 *
 * This is the API a renderer should use: build the scene once, then push every
 * frame's rays through lrt_vk_rtx_scene_trace in as few batches as possible
 * (ideally one), rather than calling a one-shot per pixel. */
typedef struct lrt_vk_rtx_scene lrt_vk_rtx_scene;

/* Build from a de-indexed triangle soup: `vertices` = 9*ntris floats (v0 v1 v2
 * per triangle). */
lrt_vk_rtx_scene *lrt_vk_rtx_scene_build(lrt_vk_engine *e, const float *vertices,
                                         uint32_t ntris, lrt_result *err);

/* Build from INDEXED geometry: `vertices` = 3*nverts floats of unique positions,
 * `indices` = 3*ntris vertex ids (uint32). Equivalent to de-indexing into a soup
 * and calling lrt_vk_rtx_scene_build, but it uploads the shared vertices once and
 * lets the driver index them — cheaper for welded meshes. prim_id stays the
 * triangle index (0..ntris-1). `indices` must be non-NULL. */
lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_indexed(lrt_vk_engine *e,
                                                 const float *vertices,
                                                 uint32_t nverts,
                                                 const uint32_t *indices,
                                                 uint32_t ntris, lrt_result *err);

/* --- True two-level (instanced) scene -------------------------------------
 *
 * Build a genuine two-level acceleration structure: one BLAS per PROTOTYPE
 * (geometry stored on the device ONCE) and one TLAS instance per PLACEMENT (the
 * same prototype BLAS referenced under a per-instance transform). This is the
 * memory-sharing path the flat builders above cannot express — N copies of a
 * prototype cost one BLAS, not N. It reuses the SAME trace pipeline/shader as
 * the flat builders (no shader change): the shader already recovers the hit id
 * as instanceId*tri_chunk + primitiveIndex, so here tri_chunk is set to the max
 * prototype triangle count and the returned lrt_hit.prim_id decodes as:
 *     instance       = prim_id / (*out_tri_stride)
 *     prototypeLocalTri = prim_id % (*out_tri_stride)
 * The caller maps `instance` -> prototype (via its own instance list) and shades
 * the prototype-local triangle, transforming object-space attributes by that
 * instance's transform.
 *
 * Each prototype builds as a SINGLE BLAS (this API does no chunk splitting), so a
 * prototype larger than the device's one-BLAS build limit must be pre-split by the
 * caller into several smaller prototypes (each a triangle slice) sharing the same
 * per-instance transform — the encoding is unaffected. Fails (returns NULL,
 * LRT_RESULT_INVALID_ARGUMENT) if ninsts*maxPrototypeTris would overflow the 32-bit
 * prim_id encoding — the caller should fall back to the flat builder. */
typedef struct lrt_vk_proto {
    const float *vertices;   /* 3*nverts floats of unique positions             */
    uint32_t nverts;
    const uint32_t *indices; /* 3*ntris vertex ids (uint32), prototype-local    */
    uint32_t ntris;
} lrt_vk_proto;

typedef struct lrt_vk_instance {
    float transform[12]; /* object->world 3x4 row-major (world = M*[p;1])        */
    uint32_t proto;      /* index into the protos[] array                        */
} lrt_vk_instance;

lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_instanced(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, uint32_t *out_tri_stride,
    lrt_result *err);

/* Wide-id instanced build: same inputs as lrt_vk_rtx_scene_build_instanced, but
 * the hit id is NOT packed into a single 32-bit prim_id. Instead the trace stores
 * the TLAS instanceId and the prototype-local triangle index in SEPARATE 32-bit
 * words (lrt_hit_wide below), so there is no ninsts*maxPrototypeTris product to
 * overflow -- the only remaining ceiling is the device TLAS maxInstanceCount
 * (commonly 2^24). Use this for instanced scenes the narrow builder rejects
 * (Moana-island scale). A scene built this way MUST be traced with
 * lrt_vk_rtx_scene_trace_wide (the narrow trace returns -1 on it). */
lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_instanced_wide(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, lrt_result *err);

/* Multi-TLAS wide instanced build: as lrt_vk_rtx_scene_build_instanced_wide, but
 * splits the instances into ceil(ninsts / ~16M) TLAS slices, each its own TLAS
 * over the SAME shared BLAS set, so a scene with MORE than the device TLAS
 * maxInstanceCount (2^24) instances renders IN FULL (Moana island's ~42.8M
 * instances -> 3 TLASes). Trace with lrt_vk_rtx_scene_trace_wide: it traces the
 * slices sequentially and merges the nearest hit on the host, reporting GLOBAL
 * instanceIds (0..ninsts-1). Costs K sequential dispatches per trace + K TLAS
 * instance buffers of VRAM; the BLAS is stored once regardless of K. */
lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_instanced_multi(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, lrt_result *err);

/* Trace n rays against the resident AS. Returns #rays that hit, or -1 on error.
 * Trace buffers are reused/grown across calls. Rejects (-1) a scene built with
 * the wide builder -- use lrt_vk_rtx_scene_trace_wide for those. */
int lrt_vk_rtx_scene_trace(lrt_vk_engine *e, lrt_vk_rtx_scene *s,
                           const lrt_ray *rays, uint32_t n, lrt_hit *out,
                           lrt_result *err);

/* Descriptor-backed MaterialX/OpenPBR preview and production path tracing.
 * The resident TLAS in `s` must describe the same indexed triangles. Vertices
 * are interleaved pos.xyz,nrm.xyz,uv.xy (8 floats). Graph blocks use the
 * canonical lusdview PackMaterialXGraphRuntime layout. Textures are packed
 * RGBA8 uint32 texels with 8-int descriptors: offset,width,height,wrapS,wrapT,
 * srgb,0,0. Materials use the canonical packed OpenPBR block whose stride is
 * supplied in `material_stride_floats`. Lights use 16 floats:
 * position.xyz,type,direction.xyz,radius,radiance.xyz,width,height,length,
 * distant-angle,environment-texture-id.
 * Output is display-referred RGBA float. */
typedef struct lrt_vk_material_path_desc {
    const float *vertices;
    uint32_t nverts;
    /* Floats per vertex in `vertices`: pos3, nrm3, uv2 and, when the stride is
     * 10, a second uv set (uv2). 0 keeps the historical 8-float layout. */
    uint32_t vertex_stride_floats;
    const uint32_t *indices;
    const uint32_t *triangle_materials;
    uint32_t ntris;
    const float *materials;
    uint32_t nmaterials;
    uint32_t material_stride_floats;
    const float *graphs;
    uint32_t graph_stride_floats;
    const uint32_t *texels;
    uint32_t ntexels;
    const int32_t *texture_descs;
    uint32_t ntextures;
    const float *lights;
    uint32_t nlights;
    float inv_view_proj[16];
    float camera[3];
    float clear_color[3];
    float exposure;
    float scene_time;
    float scene_frame;
    uint32_t width, height;
    uint32_t samples, max_depth, rr_depth, seed;
    /* Diagnostic AOV written instead of shaded radiance (0 = colour):
     * 1 = material id, 2 = first texture id of the material, 3 = UV. */
    uint32_t debug_aov;
    /* Display transform: 0 = filmic beauty curve, 1 = plain sRGB encode
     * (usdrecord's colorCorrectionMode=sRGB). */
    uint32_t display_transform;
} lrt_vk_material_path_desc;

/* Packed light ABI consumed by trace_materialx_path.comp. */
#define LRT_VK_MATERIAL_PATH_LIGHT_FLOATS 16u

int lrt_vk_rtx_scene_render_materialx_path(
    lrt_vk_engine *e, lrt_vk_rtx_scene *s,
    const lrt_vk_material_path_desc *desc, float *out_rgba,
    lrt_result *err);

/* Offline, bounded GPU residency: share every prototype BLAS, retain an owned
 * host copy of placements, and build/trace one <=1M-instance TLAS at a time.
 * All placements participate in nearest-hit merging. Repeated wide traces are
 * supported, rebuilding the slices each time. LRT_VK_TLAS_SLICE can override the
 * slice size for diagnostics. Use trace_wide even for a small scene. */
lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_instanced_streamed(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, lrt_result *err);

/* Wide-id hit: like lrt_hit but the single 32-bit prim_id is replaced by the two
 * fields the wide trace stores separately. `inst` is the TLAS instance (placement)
 * id 0..ninsts-1; `local` is the prototype-local triangle index. A miss sets
 * inst == LRT_TRI_NO_HIT (as lrt_hit.prim_id does). The caller maps inst ->
 * prototype via its own instance list and shades prototype triangle `local`. */
typedef struct lrt_hit_wide {
    float t, u, v;
    uint32_t inst;
    uint32_t local;
} lrt_hit_wide;

/* As lrt_vk_rtx_scene_trace, for a scene built with lrt_vk_rtx_scene_build_instanced_wide.
 * Writes n lrt_hit_wide. Rejects (-1) a narrow scene. */
int lrt_vk_rtx_scene_trace_wide(lrt_vk_engine *e, lrt_vk_rtx_scene *s,
                                const lrt_ray *rays, uint32_t n, lrt_hit_wide *out,
                                lrt_result *err);

/* Number of TLAS slices the scene was built with (1 for a single-TLAS scene, >1
 * for a multi-TLAS scene). 0 if s is NULL. Informational (logging / tests). */
uint32_t lrt_vk_rtx_scene_ntlas(const lrt_vk_rtx_scene *s);

void lrt_vk_rtx_scene_free(lrt_vk_engine *e, lrt_vk_rtx_scene *s);

/* --- Analytic-primitive GPU shading (spheres + boxes) ---------------------
 *
 * Scope: GPU *shading evaluation* only — no triangle BVH, no MaterialX node
 * graph. A compute shader casts one primary ray per pixel against a small set
 * of analytic spheres / axis-aligned boxes (linear scan), forward-shades the
 * closest hit with an OpenPBR-core layered BSDF (Lambert diffuse + GGX specular
 * with metallic/dielectric Schlick Fresnel), one directional "sun" via a hard
 * shadow ray, plus a 2-colour hemisphere environment (ambient + background),
 * and writes a linear RGBA image. The intent is to evaluate constant OpenPBR
 * parameters (e.g. baked per-object from a MaterialX graph on the CPU) on the
 * GPU; the math mirrors the CPU reference in examples/vk_shade so the two agree
 * within fp tolerance.
 *
 * Each primitive is 16 tightly-packed floats (no struct padding): center.xyz,
 * radius (sphere), half.xyz (box half-extents), type (0=sphere,1=box),
 * base_color.xyz, metalness, roughness, specular_ior, emission, opacity. */
typedef struct lrt_vk_shade_prim {
    float center[3];
    float radius;        /* sphere radius (boxes ignore)                  */
    float half_extent[3];/* box half-extents (spheres ignore)            */
    float type;          /* 0.0 = sphere, 1.0 = box                       */
    float base_color[3];
    float metalness;
    float roughness;
    float specular_ior;
    float emission;      /* emissive weight; radiance = base_color*emission */
    float opacity;       /* reserved (currently unused by the shader)     */
} lrt_vk_shade_prim;

/* Camera + lighting description. Direction vectors should be normalized;
 * aspect = width/height (the host fills it in when 0). */
typedef struct lrt_vk_shade_desc {
    uint32_t width, height;
    uint32_t spp;          /* anti-alias samples (deterministic jitter)    */
    float cam_origin[3];
    float tan_half_fov;    /* tan(0.5 * vertical fov)                      */
    float cam_forward[3];  /* normalized view direction                   */
    float aspect;          /* width/height; 0 -> host computes            */
    float cam_right[3];
    float cam_up[3];
    float sun_dir[3];      /* direction TOWARD the sun (normalized)       */
    float sun_radiance[3]; /* zero disables the sun                       */
    float env_top[3];      /* sky-dome colour at +Y                       */
    float env_bottom[3];   /* ground colour at -Y                         */
} lrt_vk_shade_desc;

/* Shade `nprims` analytic primitives into out_rgba (width*height*4 floats,
 * linear, caller-allocated). Returns 0 on success, -1 on error (err set).
 *
 * This one-shot form (re)allocates and uploads every buffer per call. For
 * repeated rendering of the same scene (animation, an orbit, parameter sweeps)
 * prefer the resident API below, which uploads the primitives once and reuses
 * the device buffers + descriptor set across frames. */
int lrt_vk_shade_analytic(lrt_vk_engine *e, const lrt_vk_shade_prim *prims,
                          uint32_t nprims, const lrt_vk_shade_desc *desc,
                          float *out_rgba, lrt_result *err);

/* Resident analytic-shading scene: upload the primitives ONCE and render many
 * frames against them. The primitive buffer and descriptor set are reused; the
 * output buffer is reused and only grown when a larger frame is requested. The
 * engine must outlive the scene. */
typedef struct lrt_vk_shade_scene lrt_vk_shade_scene;

lrt_vk_shade_scene *lrt_vk_shade_scene_build(lrt_vk_engine *e,
                                             const lrt_vk_shade_prim *prims,
                                             uint32_t nprims, lrt_result *err);

/* Render one frame of a resident scene into out_rgba (width*height*4 floats).
 * Uploads only the small per-frame description; reuses everything else. */
int lrt_vk_shade_scene_render(lrt_vk_engine *e, lrt_vk_shade_scene *s,
                              const lrt_vk_shade_desc *desc, float *out_rgba,
                              lrt_result *err);

void lrt_vk_shade_scene_free(lrt_vk_engine *e, lrt_vk_shade_scene *s);

#ifdef __cplusplus
}
#endif

#endif /* LIGHTRT_C_VK_H */
