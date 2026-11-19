/*
 * lightrt_c_hip.h — HIP/ROCm GPU interop for the LightRT C11 triangle kernel.
 *
 * Path A (CPU build -> GPU trace): lrt_hip_trace_scene() uploads a scene built by
 * lrt_tri_scene_build() (via its position-independent LRTS serialization) and
 * traverses n rays with a HIP compute kernel that mirrors the scalar CPU kernel,
 * returning identical hits within fp tolerance — the HIP analogue of
 * lrt_vk_trace_scene(). The kernel is compiled at runtime with hiprtc (no ROCm
 * SDK link dependency; libamdhip64 + libhiprtc are opened at runtime via hipew).
 *
 * Everything degrades gracefully: if no HIP runtime / device is available,
 * lrt_hip_engine_create() returns NULL and the caller falls back to the CPU
 * kernel. An engine is created once and may be reused across calls; a single
 * engine is NOT safe to use from multiple threads concurrently.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTRT_C_HIP_H
#define LIGHTRT_C_HIP_H

#include <stddef.h>
#include <stdint.h>

#include "lightrt_c.h"     /* lrt_result */
#include "lightrt_c_tri.h" /* lrt_tri_scene, lrt_ray, lrt_hit */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lrt_hip_engine lrt_hip_engine;

/* Capability flags reported by lrt_hip_engine_caps(). */
typedef enum lrt_hip_caps {
    LRT_HIP_CAP_COMPUTE = 1u << 0, /* always set when an engine exists */
    LRT_HIP_CAP_POINTS = 1u << 1   /* serialized sphere/disc/ellipse leaves */
} lrt_hip_caps;

typedef struct lrt_hip_engine_options {
    int device_index; /* explicit device ordinal, or -1 = device 0 */
    uint32_t flags;   /* reserved; pass 0 */
} lrt_hip_engine_options;

/* Create an engine: opens HIP + hiprtc, selects a device, and compiles the trace
 * kernel. Returns NULL and, when err is non-NULL, stores the reason, on any
 * failure (no libamdhip64/libhiprtc, no device, kernel compile error). opts may
 * be NULL for defaults (device 0). */
lrt_hip_engine *lrt_hip_engine_create(const lrt_hip_engine_options *opts,
                                      lrt_result *err);
void lrt_hip_engine_destroy(lrt_hip_engine *e);

/* Bitmask of lrt_hip_caps for the created device. */
uint32_t lrt_hip_engine_caps(const lrt_hip_engine *e);

/* Selected device name (e.g. "AMD Radeon RX 9070 XT"). */
const char *lrt_hip_engine_device_name(const lrt_hip_engine *e);

/* Human-readable message for the last failed call on this engine. */
const char *lrt_hip_engine_last_error(const lrt_hip_engine *e);

/* --- Path A: trace a CPU-built scene on the GPU ---------------------------
 *
 * Uploads s (via its LRTS serialization) and traverses n rays with a HIP kernel.
 * Writes n hits to out. Returns the number of rays that hit geometry, or -1 on
 * error (err set). Plain triangle and serialized point/ellipse scenes with
 * BVH4/BVH8 layout are supported (quantized/curve/user scenes are rejected).
 * Results match lrt_tri_intersect1 within fp tolerance. */
int lrt_hip_trace_scene(lrt_hip_engine *e, const lrt_tri_scene *s,
                        const lrt_ray *rays, uint32_t n, lrt_hit *out,
                        lrt_result *err);

/* --- 3D Gaussian splats (lightrt_c_gsplat.h) ------------------------------
 * HIP analogue of lrt_vk_gsplat_trace: same semantics, seeds and memory
 * budget (max_device_bytes, 0 = no limit). The splat kernel is compiled with
 * hiprtc on first use. Returns the hit count, or -1 on error. */
struct lrt_gsplat_scene;
int lrt_hip_gsplat_trace(lrt_hip_engine *e, const struct lrt_gsplat_scene *s,
                         const lrt_ray *rays, uint32_t n, uint32_t seed_base,
                         uint64_t max_device_bytes, lrt_hit *out,
                         lrt_result *err);

#ifdef __cplusplus
}
#endif

#endif /* LIGHTRT_C_HIP_H */
