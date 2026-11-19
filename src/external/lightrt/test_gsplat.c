/*
 * test_gsplat.c — LightRT 3D Gaussian splat primitive (lightusd-only test).
 *
 *  1. BVH trace == brute force over all splats (bitwise).
 *  2. Chunked scene == single-chunk scene (bitwise).
 *  3. Vulkan / HIP (when built and a device exists): single vs chunked under a
 *     small device budget are bitwise equal on the same device; versus the CPU
 *     the hit rate and mean alpha agree statistically (fp rounding of the hit
 *     position decorrelates individual stochastic decisions); a budget smaller
 *     than one chunk is rejected with LRT_RESULT_BUILD_LIMIT.
 *  4. Motion blur (3 keys, translating + scaling splats): at times 0 and 1 the
 *     scene equals a static scene of the first/last key (bitwise); at 0.25 it
 *     equals a static scene of the linearly interpolated records; GPU traces
 *     with per-ray times agree with the CPU statistically.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lightrt_c_gsplat.h"
#ifdef HAVE_VULKAN
#include "lightrt_c_vk.h"
#endif
#ifdef HAVE_HIP
#include "lightrt_c_hip.h"
#endif

static int g_fail = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);               \
            fprintf(stderr, "\n");                      \
            g_fail = 1;                                 \
        }                                               \
    } while (0)

static uint32_t rng_state = 12345u;
static float frand(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)(rng_state >> 8) * (1.f / 16777216.f);
}

#define NSPLAT 6000
#define NRAY 40000

static lrt_gsplat splats[NSPLAT];
static lrt_ray rays[NRAY];
static lrt_hit ha[NRAY], hb[NRAY], hc[NRAY], hd[NRAY];

static void make_data(void) {
    size_t m = 0;
    const float linv[9] = {1.f / 1.5f, 0, 0, 0.2f, 1, 0, 0, 0, 1.f / 0.8f};
    while (m < NSPLAT) {
        float mu[3] = {frand() * 8 - 4, frand() * 8 - 4, frand() * 8 - 4};
        float q[4] = {frand() - .5f, frand() - .5f, frand() - .5f, frand() - .5f};
        float sc[3] = {0.02f + 0.15f * frand(), 0.02f + 0.15f * frand(),
                       0.005f + 0.1f * frand()};
        float op = 0.05f + 0.95f * frand();
        if (lrt_gsplat_make(mu, q, sc, op, (m & 1) ? linv : NULL,
                            (m & 1) ? 1.9f : 1.f, (uint32_t)m, &splats[m]))
            ++m;
    }
    for (int i = 0; i < NRAY; ++i) {
        lrt_ray *r = &rays[i];
        r->org[0] = frand() * 2 - 1;
        r->org[1] = frand() * 2 - 1;
        r->org[2] = -10.f;
        float d[3] = {frand() * 0.6f - 0.3f, frand() * 0.6f - 0.3f, 1.f};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; ++k) r->dir[k] = d[k] / l;
        r->tmin = 0.f;
        r->tmax = 1e30f;
    }
}

/* Brute-force reference with the same arithmetic as gs_trace_chunk. */
static void brute(const lrt_ray *r, uint32_t seed, lrt_hit *h) {
    float best = r->tmax;
    h->prim_id = LRT_TRI_NO_HIT;
    h->t = h->u = h->v = 0.f;
    const float *o = r->org, *d = r->dir;
    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    for (int i = 0; i < NSPLAT; ++i) {
        const lrt_gsplat *g = &splats[i];
        const float oc[3] = {o[0] - g->mu[0], o[1] - g->mu[1], o[2] - g->mu[2]};
        const float b = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        const float cc = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] -
                         g->radius * g->radius;
        if (b * b - dd * cc < 0.f) continue;
        const float *A = g->a;
        const float og[3] = {A[0] * oc[0] + A[1] * oc[1] + A[2] * oc[2],
                             A[3] * oc[0] + A[4] * oc[1] + A[5] * oc[2],
                             A[6] * oc[0] + A[7] * oc[1] + A[8] * oc[2]};
        const float dg[3] = {A[0] * d[0] + A[1] * d[1] + A[2] * d[2],
                             A[3] * d[0] + A[4] * d[1] + A[5] * d[2],
                             A[6] * d[0] + A[7] * d[1] + A[8] * d[2]};
        const float t = -(og[0] * dg[0] + og[1] * dg[1] + og[2] * dg[2]) /
                        (dg[0] * dg[0] + dg[1] * dg[1] + dg[2] * dg[2]);
        if (!(t >= r->tmin && t <= best)) continue;
        const float e[3] = {og[0] + dg[0] * t, og[1] + dg[1] * t, og[2] + dg[2] * t};
        const float alpha =
            g->opacity * expf(-0.5f * (e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
        const float x[3] = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t};
        if (lrt_gsplat_hash(x, seed) > alpha) continue;
        best = t;
        h->t = t;
        h->u = alpha;
        h->prim_id = g->id;
    }
}

static int same(const lrt_hit *a, const lrt_hit *b, int n, const char *what) {
    int diff = 0;
    for (int i = 0; i < n; ++i)
        if (a[i].prim_id != b[i].prim_id ||
            (a[i].prim_id != LRT_TRI_NO_HIT && a[i].t != b[i].t))
            ++diff;
    CHECK(diff == 0, "%s: %d of %d rays differ", what, diff, n);
    return diff == 0;
}

static void stats(const lrt_hit *h, int n, double *rate, double *alpha) {
    int c = 0;
    double a = 0;
    for (int i = 0; i < n; ++i)
        if (h[i].prim_id != LRT_TRI_NO_HIT) {
            ++c;
            a += h[i].u;
        }
    *rate = (double)c / n;
    *alpha = c ? a / c : 0;
}

static void compare_gpu(const char *name, const lrt_hit *gpu) {
    double rc, ac, rg, ag;
    stats(ha, NRAY, &rc, &ac);
    stats(gpu, NRAY, &rg, &ag);
    /* Binomial sigma of the hit rate ~ sqrt(p(1-p)/N) ~ 0.0025; allow 5 sigma. */
    printf("%s: hit rate %.4f (cpu %.4f), mean alpha %.4f (cpu %.4f)\n", name,
           rg, rc, ag, ac);
    CHECK(fabs(rg - rc) < 0.0125, "%s hit rate %.4f vs cpu %.4f", name, rg, rc);
    CHECK(fabs(ag - ac) < 0.02, "%s mean alpha %.4f vs cpu %.4f", name, ag, ac);
    /* Rays whose decision matched must agree on t. */
    int same_prim = 0, tbad = 0;
    for (int i = 0; i < NRAY; ++i)
        if (gpu[i].prim_id == ha[i].prim_id && ha[i].prim_id != LRT_TRI_NO_HIT) {
            ++same_prim;
            if (fabsf(gpu[i].t - ha[i].t) > 1e-3f * (1.f + ha[i].t)) ++tbad;
        }
    CHECK(tbad == 0, "%s: %d rays with t mismatch", name, tbad);
    CHECK(same_prim > NRAY / 20, "%s: only %d rays share the cpu hit", name,
          same_prim);
}

#define NKEY 3
static lrt_gsplat mkeys[NKEY * NSPLAT];
static lrt_gsplat mtmp[NSPLAT];
static float mtimes[NRAY];

static void motion_test(void) {
    /* Key k: translated by k*0.08 along x, scaled by (1 + 0.15k). */
    for (int k = 0; k < NKEY; ++k)
        for (int i = 0; i < NSPLAT; ++i) {
            lrt_gsplat g = splats[i];
            g.mu[0] += 0.08f * (float)k;
            for (int j = 0; j < 9; ++j) g.a[j] /= 1.0f + 0.15f * (float)k;
            g.radius *= 1.0f + 0.15f * (float)k;
            mkeys[k * NSPLAT + i] = g;
        }
    lrt_result err;
    lrt_gsplat_build_options bo;
    memset(&bo, 0, sizeof(bo));
    bo.max_chunk_splats = 1500;
    lrt_gsplat_scene *ms = lrt_gsplat_scene_build_motion(mkeys, NSPLAT, NKEY, &bo, &err);
    CHECK(ms && lrt_gsplat_scene_key_count(ms) == NKEY, "motion build");
    if (!ms) return;
    const float probe[3] = {0.0f, 1.0f, 0.25f};
    for (int pi = 0; pi < 3; ++pi) {
        const float tm = probe[pi];
        const float f = tm * (NKEY - 1);
        int step = (int)f;
        if (step > NKEY - 2) step = NKEY - 2;
        const float t = f - (float)step, u = 1.0f - t;
        for (int i = 0; i < NSPLAT; ++i) {
            const lrt_gsplat *a = &mkeys[step * NSPLAT + i], *b = &mkeys[(step + 1) * NSPLAT + i];
            lrt_gsplat g = *a;
            for (int k = 0; k < 3; ++k) g.mu[k] = a->mu[k] * u + b->mu[k] * t;
            for (int k = 0; k < 9; ++k) g.a[k] = a->a[k] * u + b->a[k] * t;
            g.opacity = a->opacity * u + b->opacity * t;
            g.radius = a->radius * u + b->radius * t;
            mtmp[i] = g;
        }
        lrt_gsplat_scene *st = lrt_gsplat_scene_build(mtmp, NSPLAT, &bo, &err);
        for (int i = 0; i < NRAY; ++i) mtimes[i] = tm;
        lrt_gsplat_intersect1N_time(ms, rays, mtimes, NRAY, 99u, ha);
        lrt_gsplat_intersect1N(st, rays, NRAY, 99u, hb);
        char what[64];
        snprintf(what, sizeof(what), "motion t=%.2f vs static interpolated", tm);
        same(ha, hb, NRAY, what);
        lrt_gsplat_scene_free(st);
    }
    /* Motion must change the image: t=0 vs t=1 differ. */
    for (int i = 0; i < NRAY; ++i) mtimes[i] = 0.0f;
    lrt_gsplat_intersect1N_time(ms, rays, mtimes, NRAY, 99u, ha);
    for (int i = 0; i < NRAY; ++i) mtimes[i] = 1.0f;
    lrt_gsplat_intersect1N_time(ms, rays, mtimes, NRAY, 99u, hb);
    int diff = 0;
    for (int i = 0; i < NRAY; ++i) diff += ha[i].prim_id != hb[i].prim_id;
    CHECK(diff > NRAY / 20, "motion has no effect (%d rays differ)", diff);
    /* Per-ray stratified times; GPU vs CPU. */
    for (int i = 0; i < NRAY; ++i) mtimes[i] = (float)((i * 7919) % 1000) / 999.0f;
    lrt_gsplat_intersect1N_time(ms, rays, mtimes, NRAY, 5u, ha);
#ifdef HAVE_VULKAN
    {
        lrt_vk_engine_options vo;
        memset(&vo, 0, sizeof(vo));
        vo.device_index = -1;
        vo.prefer_discrete = 1;
        lrt_vk_engine *vk = lrt_vk_engine_create(&vo, NULL);
        if (vk) {
            CHECK(lrt_vk_gsplat_trace_time(vk, ms, rays, mtimes, NRAY, 5u, 0, hc, &err) >= 0,
                  "vk motion trace: %s", lrt_vk_engine_last_error(vk));
            compare_gpu("vulkan motion", hc);
            lrt_vk_engine_destroy(vk);
        }
    }
#endif
#ifdef HAVE_HIP
    {
        lrt_hip_engine *hip = lrt_hip_engine_create(NULL, NULL);
        if (hip) {
            CHECK(lrt_hip_gsplat_trace_time(hip, ms, rays, mtimes, NRAY, 5u, 0, hc, &err) >= 0,
                  "hip motion trace: %s", lrt_hip_engine_last_error(hip));
            compare_gpu("hip motion", hc);
            lrt_hip_engine_destroy(hip);
        }
    }
#endif
    lrt_gsplat_scene_free(ms);
}

int main(void) {
    make_data();
    lrt_result err;
    lrt_gsplat_build_options one, many;
    memset(&one, 0, sizeof(one));
    memset(&many, 0, sizeof(many));
    many.max_chunk_splats = 700;
    lrt_gsplat_scene *s1 = lrt_gsplat_scene_build(splats, NSPLAT, &one, &err);
    lrt_gsplat_scene *s2 = lrt_gsplat_scene_build(splats, NSPLAT, &many, &err);
    CHECK(s1 && s2, "build failed (%d)", (int)err);
    if (!s1 || !s2) return 1;
    CHECK(lrt_gsplat_scene_chunk_count(s1) == 1, "single chunk expected");
    CHECK(lrt_gsplat_scene_chunk_count(s2) >= 9, "chunks: %zu",
          lrt_gsplat_scene_chunk_count(s2));
    CHECK(lrt_gsplat_scene_size(s2) == NSPLAT, "size");

    const uint32_t seed = 77u;
    int hits = lrt_gsplat_intersect1N(s1, rays, NRAY, seed, ha);
    lrt_gsplat_intersect1N(s2, rays, NRAY, seed, hb);
    for (int i = 0; i < 4000; ++i) brute(&rays[i], seed + (uint32_t)i, &hc[i]);
    printf("cpu: %d / %d rays hit, %zu chunks\n", hits, NRAY,
           lrt_gsplat_scene_chunk_count(s2));
    CHECK(hits > NRAY / 10 && hits < NRAY, "implausible hit count %d", hits);
    same(ha, hc, 4000, "bvh vs brute force");
    same(ha, hb, NRAY, "chunked vs single (cpu)");

#ifdef HAVE_VULKAN
    {
        lrt_vk_engine_options vo;
        memset(&vo, 0, sizeof(vo));
        vo.device_index = -1;
        vo.prefer_discrete = 1;
        lrt_vk_engine *vk = lrt_vk_engine_create(&vo, NULL);
        if (!vk) {
            printf("vulkan: no device, skipped\n");
        } else {
            CHECK(lrt_vk_gsplat_trace(vk, s1, rays, NRAY, seed, 0, hc, &err) >= 0,
                  "vk trace: %s", lrt_vk_engine_last_error(vk));
            /* Budget: one chunk (~67 KB) + a 4096-ray tile, forcing tiling. */
            CHECK(lrt_vk_gsplat_trace(vk, s2, rays, NRAY, seed, 300000, hd, &err) >= 0,
                  "vk chunked trace: %s", lrt_vk_engine_last_error(vk));
            same(hc, hd, NRAY, "vk chunked+budget vs single");
            compare_gpu("vulkan", hc);
            CHECK(lrt_vk_gsplat_trace(vk, s1, rays, NRAY, seed, 100000, hd, &err) < 0 &&
                      err == LRT_RESULT_BUILD_LIMIT,
                  "vk: over-budget chunk must be rejected");
            lrt_vk_engine_destroy(vk);
        }
    }
#endif
#ifdef HAVE_HIP
    {
        lrt_hip_engine *hip = lrt_hip_engine_create(NULL, NULL);
        if (!hip) {
            printf("hip: no device, skipped\n");
        } else {
            CHECK(lrt_hip_gsplat_trace(hip, s1, rays, NRAY, seed, 0, hc, &err) >= 0,
                  "hip trace: %s", lrt_hip_engine_last_error(hip));
            CHECK(lrt_hip_gsplat_trace(hip, s2, rays, NRAY, seed, 300000, hd, &err) >= 0,
                  "hip chunked trace: %s", lrt_hip_engine_last_error(hip));
            same(hc, hd, NRAY, "hip chunked+budget vs single");
            compare_gpu("hip", hc);
            CHECK(lrt_hip_gsplat_trace(hip, s1, rays, NRAY, seed, 100000, hd, &err) < 0 &&
                      err == LRT_RESULT_BUILD_LIMIT,
                  "hip: over-budget chunk must be rejected");
            lrt_hip_engine_destroy(hip);
        }
    }
#endif
    lrt_gsplat_scene_free(s1);
    lrt_gsplat_scene_free(s2);
    motion_test();
    printf(g_fail ? "FAILED\n" : "OK\n");
    return g_fail;
}
