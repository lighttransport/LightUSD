/*
 * lightrt_c_gsplat.h — 3D Gaussian splat primitive for LightRT (lightusd fork).
 *
 * Ray tracing of 3D Gaussians with the Blender 5.3 Cycles splat kernel
 * (Apache-2.0, Blender Foundation: intern/cycles/kernel/geom/gsplat_intersect.h):
 *
 *   - bounding sphere r = sqrt(2 ln(opacity / 0.01)) * max(scale)
 *   - peak response along the ray in the unit Gaussian frame A = S^-1 R^T L^-1:
 *       t* = -dot(A(o - mu), A d) / dot(A d, A d)
 *   - alpha = opacity * exp(-0.5 |A(o + t* d - mu)|^2)
 *   - stochastic opaque hit: accepted when hash(o + t* d, seed) <= alpha, so a
 *     plain closest-hit traversal integrates transmittance by Monte Carlo.
 *
 * The closest accepted hit is a per-primitive independent test, so a scene may
 * be split into chunks (each with its own BVH) and traced chunk by chunk with
 * tmax clamped to the best hit so far; the result equals a single-BVH trace.
 * The GPU backends (lrt_vk_gsplat_trace, lrt_hip_gsplat_trace) use this to stay
 * under a caller-given device-memory budget.
 *
 * The CPU, Vulkan and HIP kernels share the same hash, so a ray traced on any
 * backend with the same seed takes the same stochastic decision (up to fp
 * rounding of the hit position).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTRT_C_GSPLAT_H
#define LIGHTRT_C_GSPLAT_H

#include <stddef.h>
#include <stdint.h>

#include "lightrt_c.h"     /* lrt_result */
#include "lightrt_c_tri.h" /* lrt_ray, lrt_hit */

#ifdef __cplusplus
extern "C" {
#endif

/* One world-space splat, 64 bytes (GPU layout, 16 x 32-bit words).
 * a = row-major A = S^-1 R^T L^-1 (world -> unit Gaussian frame). radius = world
 * bounding-sphere radius; radius <= 0 never hits. id is reported as prim_id. */
typedef struct lrt_gsplat {
    float mu[3];
    float opacity;
    float a[9];
    float radius;
    uint32_t id;
    uint32_t pad;
} lrt_gsplat;

/* Fill *out from local parameters. mu_world = world center; q = (w,x,y,z),
 * normalized internally; scale = linear standard deviations (all > 0);
 * opacity in [0,1]; linv = row-major inverse of the object's linear part
 * (column-vector convention), NULL = identity; l_frobenius = Frobenius norm of
 * the linear part (1 for identity). Returns 0 when the splat can be culled
 * (degenerate scale or opacity < 0.01). */
int lrt_gsplat_make(const float mu_world[3], const float q[4],
                    const float scale[3], float opacity, const float *linv,
                    float l_frobenius, uint32_t id, lrt_gsplat *out);

/* Stateless uniform hash of a position to [0,1). */
float lrt_gsplat_hash(const float p[3], uint32_t seed);

typedef struct lrt_gsplat_scene lrt_gsplat_scene;

typedef struct lrt_gsplat_build_options {
    /* Max splats per chunk (each chunk gets its own BVH). 0 = one chunk. GPU
     * backends need chunk bytes (~80 B/splat) to fit their memory budget;
     * lrt_gsplat_chunk_splats_for_budget() computes a value. */
    size_t max_chunk_splats;
    uint32_t leaf_size; /* splats per leaf, 1..8; 0 = 4 */
} lrt_gsplat_build_options;

/* Device bytes per splat including BVH nodes (64 B splat + <=32 B nodes). */
#define LRT_GSPLAT_DEVICE_BYTES_PER_SPLAT 96u

/* Largest chunk that, with a ray tile of `ray_tile` rays, fits `budget_bytes`. */
size_t lrt_gsplat_chunk_splats_for_budget(uint64_t budget_bytes,
                                          uint32_t ray_tile);

/* Build from n splats (copied). Spatially coherent chunks via median splits.
 * Returns NULL on error (err set). */
lrt_gsplat_scene *lrt_gsplat_scene_build(const lrt_gsplat *splats, size_t n,
                                         const lrt_gsplat_build_options *opts,
                                         lrt_result *err);
void lrt_gsplat_scene_free(lrt_gsplat_scene *s);

size_t lrt_gsplat_scene_size(const lrt_gsplat_scene *s);
size_t lrt_gsplat_scene_chunk_count(const lrt_gsplat_scene *s);
/* World bounds of the visible support (center +- radius). */
void lrt_gsplat_scene_bounds(const lrt_gsplat_scene *s, float lo[3],
                             float hi[3]);

/* Raw GPU image of chunk k: nodes = 8 words each (lo.xyz, hi.xyz as float,
 * then first, count; count == 0 => inner node, children first and first + 1),
 * splats = lrt_gsplat in leaf order. Root is node 0. */
void lrt_gsplat_scene_chunk(const lrt_gsplat_scene *s, size_t k,
                            const uint32_t **nodes, uint32_t *node_count,
                            const lrt_gsplat **splats, uint32_t *splat_count);

/* Closest stochastically accepted splat in [ray->tmin, ray->tmax]. Writes
 * hit->t and hit->prim_id (= splat id; LRT_TRI_NO_HIT on miss); u = alpha at
 * the hit, v = 0. Returns 1 on hit. Thread-safe. */
int lrt_gsplat_intersect1(const lrt_gsplat_scene *s, const lrt_ray *ray,
                          uint32_t seed, lrt_hit *hit);

/* n rays, ray i uses seed seed_base + i. Multithreaded. Returns hit count. */
int lrt_gsplat_intersect1N(const lrt_gsplat_scene *s, const lrt_ray *rays,
                           uint32_t n, uint32_t seed_base, lrt_hit *out);

#ifdef __cplusplus
}
#endif

#endif /* LIGHTRT_C_GSPLAT_H */
