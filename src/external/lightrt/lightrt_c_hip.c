/*
 * lightrt_c_hip.c — HIP/ROCm backend for the LightRT triangle kernel (Path A).
 *
 * Mirrors lightrt_c_vk.c's lrt_vk_trace_scene: serialize the CPU-built scene with
 * lrt_tri_scene_save_to_memory(), upload the node/block regions verbatim, and run
 * a HIP kernel (trace_bvh_hip.h, a port of trace_bvh.comp) that walks the same
 * memory image. HIP + hiprtc are loaded at runtime via hipew (no SDK link).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lightrt_c_hip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hipew.h"
#include "hip/trace_bvh_hip.h"
#include "hip/trace_gsplat_hip.h"
#include "lightrt_c_gsplat.h"

/* On-disk / in-memory LRTS header produced by lrt_tri_scene_save_to_memory().
 * Layout-identical to the replica used by lightrt_c_vk.c. */
typedef struct hip_lrts_header {
    char magic[4];
    uint32_t version;
    uint32_t endian;
    uint32_t flags;
    uint32_t layout;
    uint32_t prim_kind;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t root;
    uint32_t node_stride;
    uint32_t block_stride;
    uint32_t reserved0;
    float root_lo[3];
    float root_hi[3];
    uint64_t node_offset;
    uint64_t block_offset;
    uint64_t file_size;
} hip_lrts_header;

struct lrt_hip_engine {
    int device;
    hipModule_t module;
    hipFunction_t kernel;
    hipModule_t gsplat_module; /* lazily compiled trace_gsplat_hip.h */
    hipFunction_t gsplat_kernel;
    char device_name[256];
    char err[512];
};

/* trace_stack_for: matches lightrt_c_vk.c. The HIP kernel uses a fixed 256-entry
 * stack, so any bucket <= 256 is fine; deeper trees are rejected. */
static uint32_t hip_trace_stack_for(uint32_t max_depth, uint32_t w) {
    uint32_t need = max_depth * (w - 1u) + w + 1u;
    static const uint32_t buckets[] = {32u, 64u, 128u, 256u};
    for (int i = 0; i < 4; i++)
        if (need <= buckets[i]) return buckets[i];
    return 0;
}

static void hip_set_err(lrt_hip_engine *e, const char *msg) {
    if (!e) return;
    snprintf(e->err, sizeof(e->err), "%s", msg ? msg : "");
}

static int hip_ok(hipError_t r) { return r == hipSuccess; }

lrt_hip_engine *lrt_hip_engine_create(const lrt_hip_engine_options *opts,
                                      lrt_result *err) {
    if (hipewInit(HIPEW_INIT_HIP | HIPEW_INIT_HIPRTC) != HIPEW_SUCCESS) {
        if (err) *err = LRT_RESULT_UNSUPPORTED;
        return NULL;
    }

    lrt_hip_engine *e = (lrt_hip_engine *)calloc(1, sizeof(lrt_hip_engine));
    if (!e) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }

    if (!hip_ok(hipInit(0))) {
        hip_set_err(e, "hipInit failed");
        goto fail;
    }
    int count = 0;
    if (!hip_ok(hipGetDeviceCount(&count)) || count <= 0) {
        hip_set_err(e, "no HIP device");
        goto fail;
    }
    int idx = (opts && opts->device_index >= 0) ? opts->device_index : 0;
    if (idx >= count) idx = 0;
    if (!hip_ok(hipSetDevice(idx))) {
        hip_set_err(e, "hipSetDevice failed");
        goto fail;
    }
    e->device = idx;

    hipDevice_t dev = 0;
    if (hip_ok(hipDeviceGet(&dev, idx)) &&
        hip_ok(hipDeviceGetName(e->device_name, (int)sizeof(e->device_name),
                                dev))) {
        /* device_name populated */
    } else {
        snprintf(e->device_name, sizeof(e->device_name), "HIP device %d", idx);
    }

    /* Compile the trace kernel with hiprtc. No --offload-arch is passed: hiprtc
     * targets the current device by default. */
    hiprtcProgram prog = NULL;
    if (hiprtcCreateProgram(&prog, k_lrt_hip_trace_bvh_src,
                            "lrt_hip_trace_bvh.hip", 0, NULL, NULL) !=
        HIPRTC_SUCCESS) {
        hip_set_err(e, "hiprtcCreateProgram failed");
        goto fail;
    }
    hiprtcResult cres = hiprtcCompileProgram(prog, 0, NULL);
    if (cres != HIPRTC_SUCCESS) {
        size_t log_n = 0;
        hiprtcGetProgramLogSize(prog, &log_n);
        char *log = (log_n > 1) ? (char *)malloc(log_n) : NULL;
        if (log) {
            hiprtcGetProgramLog(prog, log);
            snprintf(e->err, sizeof(e->err), "hiprtc compile failed: %s", log);
            free(log);
        } else {
            hip_set_err(e, "hiprtc compile failed");
        }
        hiprtcDestroyProgram(&prog);
        goto fail;
    }
    size_t code_n = 0;
    if (hiprtcGetCodeSize(prog, &code_n) != HIPRTC_SUCCESS || code_n == 0) {
        hip_set_err(e, "hiprtcGetCodeSize failed");
        hiprtcDestroyProgram(&prog);
        goto fail;
    }
    char *code = (char *)malloc(code_n);
    if (!code) {
        hiprtcDestroyProgram(&prog);
        hip_set_err(e, "out of memory (code object)");
        goto fail;
    }
    if (hiprtcGetCode(prog, code) != HIPRTC_SUCCESS) {
        free(code);
        hiprtcDestroyProgram(&prog);
        hip_set_err(e, "hiprtcGetCode failed");
        goto fail;
    }
    hiprtcDestroyProgram(&prog);

    if (!hip_ok(hipModuleLoadData(&e->module, code))) {
        free(code);
        hip_set_err(e, "hipModuleLoadData failed");
        goto fail;
    }
    free(code);
    if (!hip_ok(hipModuleGetFunction(&e->kernel, e->module,
                                     "lrt_hip_trace_bvh"))) {
        hip_set_err(e, "hipModuleGetFunction failed");
        goto fail;
    }

    if (err) *err = LRT_RESULT_OK;
    return e;

fail:
    if (err) *err = LRT_RESULT_UNSUPPORTED;
    /* Surface the reason to stderr before freeing — engine-create returns NULL
     * (like the Vulkan backend) so the detailed string would otherwise be lost. */
    if (e->err[0]) fprintf(stderr, "lightrt-hip: %s\n", e->err);
    if (e->module) hipModuleUnload(e->module);
    free(e);
    return NULL;
}

void lrt_hip_engine_destroy(lrt_hip_engine *e) {
    if (!e) return;
    if (e->gsplat_module) hipModuleUnload(e->gsplat_module);
    if (e->module) hipModuleUnload(e->module);
    free(e);
}

uint32_t lrt_hip_engine_caps(const lrt_hip_engine *e) {
    return e ? (LRT_HIP_CAP_COMPUTE | LRT_HIP_CAP_POINTS) : 0u;
}

const char *lrt_hip_engine_device_name(const lrt_hip_engine *e) {
    return e ? e->device_name : "";
}

const char *lrt_hip_engine_last_error(const lrt_hip_engine *e) {
    return e ? e->err : "";
}

int lrt_hip_trace_scene(lrt_hip_engine *e, const lrt_tri_scene *s,
                        const lrt_ray *rays, uint32_t n, lrt_hit *out,
                        lrt_result *err) {
    if (!e || !s || (n && (!rays || !out))) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    if (n == 0) {
        if (err) *err = LRT_RESULT_OK;
        return 0;
    }

    void *blob = NULL;
    size_t blob_n = 0;
    lrt_result sr = lrt_tri_scene_save_to_memory(s, &blob, &blob_n);
    if (sr != LRT_RESULT_OK) {
        hip_set_err(e, "scene not GPU-traceable (quantized/curve/user?)");
        if (err) *err = sr;
        return -1;
    }
    const hip_lrts_header *h = (const hip_lrts_header *)blob;
    if (h->prim_kind != 0u /* TRI */ && h->prim_kind != 6u /* POINT */) {
        free(blob);
        hip_set_err(e, "only triangle and point scenes are GPU-traceable");
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    uint32_t w = h->layout; /* 4 or 8 */

    lrt_tri_stats st;
    lrt_tri_scene_stats(s, &st);
    uint32_t stack = hip_trace_stack_for(st.max_depth, w);
    if (stack == 0) {
        hip_set_err(e, "BVH too deep for the GPU compute stack");
        free(blob);
        if (err) *err = LRT_RESULT_TRAVERSAL_OVERFLOW;
        return -1;
    }

    size_t nodes_bytes = (size_t)h->node_count * h->node_stride;
    size_t blocks_bytes = (size_t)h->block_count * h->block_stride;
    size_t rays_bytes = (size_t)n * sizeof(lrt_ray);
    size_t hits_bytes = (size_t)n * sizeof(lrt_hit);

    hipDeviceptr_t d_nodes = NULL, d_blocks = NULL, d_rays = NULL, d_hits = NULL;
    int failed = 0;
    if (!hip_ok(hipMalloc(&d_nodes, nodes_bytes ? nodes_bytes : 1)) ||
        !hip_ok(hipMalloc(&d_blocks, blocks_bytes ? blocks_bytes : 1)) ||
        !hip_ok(hipMalloc(&d_rays, rays_bytes)) ||
        !hip_ok(hipMalloc(&d_hits, hits_bytes))) {
        hip_set_err(e, "hipMalloc failed");
        failed = 1;
        goto cleanup;
    }
    if (!hip_ok(hipMemcpyHtoD(d_nodes, (const char *)blob + h->node_offset,
                              nodes_bytes)) ||
        !hip_ok(hipMemcpyHtoD(d_blocks, (const char *)blob + h->block_offset,
                              blocks_bytes)) ||
        !hip_ok(hipMemcpyHtoD(d_rays, rays, rays_bytes))) {
        hip_set_err(e, "host->device copy failed");
        failed = 1;
        goto cleanup;
    }

    {
        uint32_t root = h->root;
        uint32_t ray_count = n;
        uint32_t kw = w;
        uint32_t kstack = stack;
        uint32_t prim_kind = h->prim_kind;
        uint32_t point_type = h->reserved0;
        void *params[] = {&d_nodes, &d_blocks, &d_rays, &d_hits,
                          &root,    &ray_count, &kw,     &kstack,
                          &prim_kind, &point_type};
        unsigned int groups = (n + 63u) / 64u;
        if (!hip_ok(hipModuleLaunchKernel(e->kernel, groups, 1, 1, 64, 1, 1, 0,
                                          NULL, params, NULL))) {
            hip_set_err(e, "hipModuleLaunchKernel failed");
            failed = 1;
            goto cleanup;
        }
        if (!hip_ok(hipDeviceSynchronize())) {
            hip_set_err(e, "hipDeviceSynchronize failed");
            failed = 1;
            goto cleanup;
        }
    }

    if (!hip_ok(hipMemcpyDtoH(out, d_hits, hits_bytes))) {
        hip_set_err(e, "device->host copy failed");
        failed = 1;
        goto cleanup;
    }

cleanup:
    if (d_nodes) hipFree(d_nodes);
    if (d_blocks) hipFree(d_blocks);
    if (d_rays) hipFree(d_rays);
    if (d_hits) hipFree(d_hits);
    free(blob);
    if (failed) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    int hits = 0;
    for (uint32_t i = 0; i < n; i++)
        if (out[i].prim_id != LRT_TRI_NO_HIT) hits++;
    if (err) *err = LRT_RESULT_OK;
    return hits;
}

/* ------------------------------------------------------------------------- */
/* 3D Gaussian splats.                                                       */
/* ------------------------------------------------------------------------- */
static int hip_compile_gsplat(lrt_hip_engine *e) {
    if (e->gsplat_kernel) return 1;
    hiprtcProgram prog = NULL;
    if (hiprtcCreateProgram(&prog, k_lrt_hip_trace_gsplat_src,
                            "lrt_hip_trace_gsplat.hip", 0, NULL, NULL) !=
        HIPRTC_SUCCESS) {
        hip_set_err(e, "hiprtcCreateProgram(gsplat) failed");
        return 0;
    }
    if (hiprtcCompileProgram(prog, 0, NULL) != HIPRTC_SUCCESS) {
        size_t log_n = 0;
        hiprtcGetProgramLogSize(prog, &log_n);
        char *log = (log_n > 1) ? (char *)malloc(log_n) : NULL;
        if (log) {
            hiprtcGetProgramLog(prog, log);
            snprintf(e->err, sizeof(e->err), "hiprtc gsplat compile failed: %s", log);
            free(log);
        } else {
            hip_set_err(e, "hiprtc gsplat compile failed");
        }
        hiprtcDestroyProgram(&prog);
        return 0;
    }
    size_t code_n = 0;
    char *code = NULL;
    if (hiprtcGetCodeSize(prog, &code_n) != HIPRTC_SUCCESS || code_n == 0 ||
        !(code = (char *)malloc(code_n)) ||
        hiprtcGetCode(prog, code) != HIPRTC_SUCCESS) {
        free(code);
        hiprtcDestroyProgram(&prog);
        hip_set_err(e, "hiprtcGetCode(gsplat) failed");
        return 0;
    }
    hiprtcDestroyProgram(&prog);
    int ok = hip_ok(hipModuleLoadData(&e->gsplat_module, code));
    free(code);
    if (!ok) {
        e->gsplat_module = NULL;
        hip_set_err(e, "hipModuleLoadData(gsplat) failed");
        return 0;
    }
    if (!hip_ok(hipModuleGetFunction(&e->gsplat_kernel, e->gsplat_module,
                                     "lrt_hip_trace_gsplat"))) {
        e->gsplat_kernel = NULL;
        hip_set_err(e, "hipModuleGetFunction(gsplat) failed");
        return 0;
    }
    return 1;
}

static uint32_t hip_ray_tile_size(void) {
    const char *ev = getenv("LRT_VK_RAY_TILE");
    if (ev) {
        char *end = NULL;
        unsigned long v = strtoul(ev, &end, 10);
        if (end != ev && v > 0) return v > 0xffffffffUL ? 0xffffffffu : (uint32_t)v;
    }
    return 4u << 20;
}

int lrt_hip_gsplat_trace(lrt_hip_engine *e, const struct lrt_gsplat_scene *s,
                         const lrt_ray *rays, uint32_t n, uint32_t seed_base,
                         uint64_t max_device_bytes, lrt_hit *out,
                         lrt_result *err) {
    return lrt_hip_gsplat_trace_time(e, s, rays, NULL, n, seed_base,
                                     max_device_bytes, out, err);
}

int lrt_hip_gsplat_trace_time(lrt_hip_engine *e, const struct lrt_gsplat_scene *s,
                              const lrt_ray *rays, const float *times, uint32_t n,
                              uint32_t seed_base, uint64_t max_device_bytes,
                              lrt_hit *out, lrt_result *err) {
    if (!e || !s || (n && (!rays || !out))) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i].t = out[i].u = out[i].v = 0.0f;
        out[i].prim_id = LRT_TRI_NO_HIT;
    }
    if (n == 0 || lrt_gsplat_scene_size(s) == 0) {
        if (err) *err = LRT_RESULT_OK;
        return 0;
    }
    if (!hip_compile_gsplat(e)) {
        fprintf(stderr, "lightrt-hip: %s\n", e->err);
        if (err) *err = LRT_RESULT_UNSUPPORTED;
        return -1;
    }
    const size_t nchunks = lrt_gsplat_scene_chunk_count(s);
    const uint32_t nkeys = lrt_gsplat_scene_key_count(s);
    uint64_t max_chunk = 0;
    for (size_t k = 0; k < nchunks; k++) {
        const uint32_t *nodes;
        const lrt_gsplat *sp;
        uint32_t nn, ns;
        lrt_gsplat_scene_chunk(s, k, &nodes, &nn, &sp, &ns);
        uint64_t b = (uint64_t)nn * 32u + (uint64_t)ns * nkeys * sizeof(lrt_gsplat);
        if (b > max_chunk) max_chunk = b;
    }
    uint32_t tile = hip_ray_tile_size();
    if (tile > n) tile = n;
    const uint64_t per_ray = sizeof(lrt_ray) + sizeof(lrt_hit) + sizeof(float);
    if (max_device_bytes) {
        if (max_chunk + per_ray * 4096u > max_device_bytes) {
            hip_set_err(e, "gsplat chunk exceeds the device memory budget");
            if (err) *err = LRT_RESULT_BUILD_LIMIT;
            return -1;
        }
        uint64_t fit = (max_device_bytes - max_chunk) / per_ray;
        if (fit < tile) tile = (uint32_t)fit;
    }
    lrt_ray *work = (lrt_ray *)malloc((size_t)tile * sizeof(lrt_ray));
    lrt_hit *th = (lrt_hit *)malloc((size_t)tile * sizeof(lrt_hit));
    float *best = (float *)malloc((size_t)n * sizeof(float));
    hipDeviceptr_t d_rays = NULL, d_hits = NULL, d_nodes = NULL, d_times = NULL;
    int ok = work && th && best &&
             hip_ok(hipMalloc(&d_rays, (size_t)tile * sizeof(lrt_ray))) &&
             hip_ok(hipMalloc(&d_hits, (size_t)tile * sizeof(lrt_hit))) &&
             (!times || nkeys < 2 ||
              hip_ok(hipMalloc(&d_times, (size_t)tile * sizeof(float)))) &&
             /* One allocation holds a chunk's nodes followed by its splats. */
             hip_ok(hipMalloc(&d_nodes, (size_t)max_chunk));
    if (!ok) hip_set_err(e, "hipMalloc(gsplat) failed");
    if (ok)
        for (uint32_t i = 0; i < n; i++) best[i] = rays[i].tmax;
    for (size_t k = 0; ok && k < nchunks; k++) {
        const uint32_t *nodes;
        const lrt_gsplat *sp;
        uint32_t nn, ns;
        lrt_gsplat_scene_chunk(s, k, &nodes, &nn, &sp, &ns);
        if (!nn || !ns) continue;
        const size_t nb = (size_t)nn * 32u, sb = (size_t)ns * nkeys * sizeof(lrt_gsplat);
        hipDeviceptr_t d_sp = (hipDeviceptr_t)((char *)d_nodes + nb);
        if (!hip_ok(hipMemcpyHtoD(d_nodes, nodes, nb)) ||
            !hip_ok(hipMemcpyHtoD(d_sp, sp, sb))) {
            hip_set_err(e, "gsplat host->device copy failed");
            ok = 0;
            break;
        }
        for (uint32_t off = 0; ok && off < n; off += tile) {
            const uint32_t cnt = n - off < tile ? n - off : tile;
            for (uint32_t i = 0; i < cnt; i++) {
                work[i] = rays[off + i];
                work[i].tmax = best[off + i];
            }
            uint32_t ray_count = cnt, sb0 = seed_base + off, node_count = nn;
            uint32_t kkeys = nkeys;
            void *params[] = {&d_nodes, &d_sp, &d_rays, &d_hits,
                              &ray_count, &sb0, &node_count, &d_times, &kkeys};
            ok = hip_ok(hipMemcpyHtoD(d_rays, work, (size_t)cnt * sizeof(lrt_ray))) &&
                 (!d_times || hip_ok(hipMemcpyHtoD(d_times, times + off,
                                                   (size_t)cnt * sizeof(float)))) &&
                 hip_ok(hipModuleLaunchKernel(e->gsplat_kernel, (cnt + 63u) / 64u, 1, 1,
                                              64, 1, 1, 0, NULL, params, NULL)) &&
                 hip_ok(hipDeviceSynchronize()) &&
                 hip_ok(hipMemcpyDtoH(th, d_hits, (size_t)cnt * sizeof(lrt_hit)));
            if (!ok) {
                hip_set_err(e, "gsplat trace launch failed");
                break;
            }
            for (uint32_t i = 0; i < cnt; i++)
                if (th[i].prim_id != LRT_TRI_NO_HIT) {
                    out[off + i] = th[i];
                    best[off + i] = th[i].t;
                }
        }
    }
    if (d_rays) hipFree(d_rays);
    if (d_hits) hipFree(d_hits);
    if (d_nodes) hipFree(d_nodes);
    if (d_times) hipFree(d_times);
    free(work);
    free(th);
    free(best);
    if (!ok) {
        fprintf(stderr, "lightrt-hip: %s\n", e->err);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }
    int hits = 0;
    for (uint32_t i = 0; i < n; i++)
        if (out[i].prim_id != LRT_TRI_NO_HIT) hits++;
    if (err) *err = LRT_RESULT_OK;
    return hits;
}
