/*
 * lightrt_c_gsplat.c — CPU build + trace for the 3D Gaussian splat primitive.
 * Kernel math ported from Blender 5.3 Cycles (Apache-2.0); see the header.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lightrt_c_gsplat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__STDC_NO_THREADS__) && \
    (!defined(__has_include) || __has_include(<threads.h>))
#define LRT_GS_HAS_THREADS 1
#include <stdatomic.h>
#include <threads.h>
#else
#define LRT_GS_HAS_THREADS 0
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define GS_ALPHA_MIN 0.01f
#define GS_STACK 64

typedef struct gs_node {
    float lo[3], hi[3];
    uint32_t first, count; /* count == 0: inner (children first, first+1) */
} gs_node;

typedef struct gs_chunk {
    gs_node *nodes;
    uint32_t node_count;
    lrt_gsplat *splats;
    uint32_t splat_count;
} gs_chunk;

struct lrt_gsplat_scene {
    gs_chunk *chunks;
    size_t chunk_count;
    size_t size;
    uint32_t nkeys; /* motion keys per splat (1 = static) */
    float lo[3], hi[3];
};

/* ------------------------------------------------------------------------- */
int lrt_gsplat_make(const float mu[3], const float q[4], const float scale[3],
                    float opacity, const float *linv, float l_frobenius,
                    uint32_t id, lrt_gsplat *out) {
    static const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (!(scale[0] > 0.f && scale[1] > 0.f && scale[2] > 0.f)) return 0;
    if (!(opacity >= GS_ALPHA_MIN)) return 0;
    float m = scale[0];
    if (scale[1] > m) m = scale[1];
    if (scale[2] > m) m = scale[2];
    const float r = sqrtf(2.f * logf(opacity / GS_ALPHA_MIN)) * m;
    if (!(r > 0.f)) return 0;
    if (!linv) linv = ident;
    /* Rotation rows (normalizes q like Cycles' quaternion_to_rotation). */
    const float w = q ? q[0] : 1.f, x = q ? q[1] : 0.f, y = q ? q[2] : 0.f,
                z = q ? q[3] : 0.f;
    const float n = w * w + x * x + y * y + z * z;
    const float s = n > 0.f ? 2.f / n : 0.f;
    const float xx = x * x * s, yy = y * y * s, zz = z * z * s;
    const float xy = x * y * s, xz = x * z * s, yz = y * z * s;
    const float wx = w * x * s, wy = w * y * s, wz = w * z * s;
    const float R[9] = {1.f - (yy + zz), xy - wz, xz + wy,
                        xy + wz, 1.f - (xx + zz), yz - wx,
                        xz - wy, yz + wx, 1.f - (xx + yy)};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float acc = 0.f;
            for (int k = 0; k < 3; ++k) acc += R[k * 3 + i] * linv[k * 3 + j];
            out->a[i * 3 + j] = acc / scale[i];
        }
    out->mu[0] = mu[0];
    out->mu[1] = mu[1];
    out->mu[2] = mu[2];
    out->opacity = opacity;
    out->radius = r * l_frobenius;
    out->id = id;
    out->pad = 0;
    return 1;
}

float lrt_gsplat_hash(const float p[3], uint32_t seed) {
    uint32_t h = 0x9e3779b9u ^ (seed * 0x85ebca6bu);
    for (int i = 0; i < 3; ++i) {
        uint32_t b;
        memcpy(&b, &p[i], 4);
        h ^= b + 0x7f4a7c15u + (h << 6) + (h >> 2);
        h = h * 747796405u + 2891336453u;
        h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
        h = (h >> 22u) ^ h;
    }
    return (float)(h >> 8) * (1.f / 16777216.f);
}

/* ------------------------------------------------------------------------- */
size_t lrt_gsplat_chunk_splats_for_budget(uint64_t budget, uint32_t ray_tile) {
    const uint64_t ray_bytes = (uint64_t)ray_tile * (sizeof(lrt_ray) + sizeof(lrt_hit));
    if (budget <= ray_bytes + (1u << 20)) return 1u << 16;
    uint64_t n = (budget - ray_bytes - (1u << 20)) / LRT_GSPLAT_DEVICE_BYTES_PER_SPLAT;
    if (n < (1u << 16)) n = 1u << 16;
    if (n > 0x7fffffffu) n = 0x7fffffffu;
    return (size_t)n;
}

static float gs_center(const lrt_gsplat *g, int axis) { return g->mu[axis]; }

/* Quickselect (nth_element) on idx[lo,hi) by splat center along axis. */
static void gs_select(uint32_t *idx, size_t lo, size_t hi, size_t nth,
                      const lrt_gsplat *src, int axis) {
    while (hi - lo > 1) {
        /* Pivot must not be the last element (Hoare needs j < hi - 1). */
        const float pivot = gs_center(&src[idx[lo + (hi - lo - 1) / 2]], axis);
        /* Canonical Hoare partition (do-while form keeps j >= lo). */
        long i = (long)lo - 1, j = (long)hi;
        for (;;) {
            do ++i; while (gs_center(&src[idx[i]], axis) < pivot);
            do --j; while (gs_center(&src[idx[j]], axis) > pivot);
            if (i >= j) break;
            uint32_t t = idx[i];
            idx[i] = idx[j];
            idx[j] = t;
        }
        /* [lo, j] <= pivot <= [j+1, hi) */
        if (nth <= (size_t)j) hi = (size_t)j + 1;
        else lo = (size_t)j + 1;
    }
}

static int gs_widest_axis(const uint32_t *idx, size_t b, size_t e,
                          const lrt_gsplat *src) {
    float lo[3] = {INFINITY, INFINITY, INFINITY},
          hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t i = b; i < e; ++i)
        for (int k = 0; k < 3; ++k) {
            const float c = src[idx[i]].mu[k];
            if (c < lo[k]) lo[k] = c;
            if (c > hi[k]) hi[k] = c;
        }
    int axis = 0;
    for (int k = 1; k < 3; ++k)
        if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;
    return axis;
}

/* Median-split BVH over bounding spheres of idx[b,e). keys holds nkeys
 * key-major arrays of ntot splats; src (the middle key) drives the splits and
 * node bounds cover the splat's sphere at every key (linear motion between
 * keys stays inside their union's box). */
static int gs_build_chunk(const lrt_gsplat *src, const lrt_gsplat *keys,
                          size_t ntot, uint32_t nkeys, uint32_t *idx, size_t b,
                          size_t e, uint32_t leaf, gs_chunk *c) {
    const size_t n = e - b;
    c->splat_count = (uint32_t)n;
    c->splats = (lrt_gsplat *)malloc((n ? n : 1) * nkeys * sizeof(lrt_gsplat));
    size_t cap = n / leaf * 2 + 3;
    c->nodes = (gs_node *)malloc(cap * sizeof(gs_node));
    if (!c->splats || !c->nodes) return 0;
    typedef struct { uint32_t node, begin, end; } item;
    item stack[128];
    int sp = 0;
    uint32_t nn = 1;
    stack[sp++] = (item){0, (uint32_t)b, (uint32_t)e};
    while (sp) {
        const item it = stack[--sp];
        gs_node *nd = &c->nodes[it.node];
        for (int k = 0; k < 3; ++k) {
            nd->lo[k] = INFINITY;
            nd->hi[k] = -INFINITY;
        }
        for (uint32_t i = it.begin; i < it.end; ++i)
            for (uint32_t key = 0; key < nkeys; ++key) {
                const lrt_gsplat *g = &keys[(size_t)key * ntot + idx[i]];
                for (int k = 0; k < 3; ++k) {
                    if (g->mu[k] - g->radius < nd->lo[k]) nd->lo[k] = g->mu[k] - g->radius;
                    if (g->mu[k] + g->radius > nd->hi[k]) nd->hi[k] = g->mu[k] + g->radius;
                }
            }
        const uint32_t cnt = it.end - it.begin;
        if (cnt <= leaf || sp + 2 > 128) {
            nd->first = it.begin - (uint32_t)b;
            nd->count = cnt;
            continue;
        }
        const int axis = gs_widest_axis(idx, it.begin, it.end, src);
        const uint32_t mid = it.begin + cnt / 2;
        gs_select(idx, it.begin, it.end, mid, src, axis);
        if (nn + 2 > cap) {
            cap = cap * 2;
            gs_node *g2 = (gs_node *)realloc(c->nodes, cap * sizeof(gs_node));
            if (!g2) return 0;
            c->nodes = g2;
            nd = &c->nodes[it.node];
        }
        nd->first = nn;
        nd->count = 0;
        stack[sp++] = (item){nn, it.begin, mid};
        stack[sp++] = (item){nn + 1, mid, it.end};
        nn += 2;
    }
    c->node_count = nn;
    for (size_t i = 0; i < n; ++i)
        for (uint32_t key = 0; key < nkeys; ++key)
            c->splats[i * nkeys + key] = keys[(size_t)key * ntot + idx[b + i]];
    return 1;
}

lrt_gsplat_scene *lrt_gsplat_scene_build(const lrt_gsplat *splats, size_t n,
                                         const lrt_gsplat_build_options *opts,
                                         lrt_result *err) {
    return lrt_gsplat_scene_build_motion(splats, n, 1, opts, err);
}

lrt_gsplat_scene *lrt_gsplat_scene_build_motion(
    const lrt_gsplat *keys, size_t n, uint32_t nkeys,
    const lrt_gsplat_build_options *opts, lrt_result *err) {
    if ((!keys && n) || n > 0xfffffffeu || nkeys < 1 || nkeys > 64) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return NULL;
    }
    uint32_t leaf = opts && opts->leaf_size ? opts->leaf_size : 4u;
    if (leaf > 8u) leaf = 8u;
    size_t cap = opts && opts->max_chunk_splats ? opts->max_chunk_splats : n;
    if (cap < 1) cap = 1;
    const lrt_gsplat *splats = keys + (size_t)(nkeys / 2) * n; /* split key */
    lrt_gsplat_scene *s = (lrt_gsplat_scene *)calloc(1, sizeof(*s));
    if (s) s->nkeys = nkeys;
    uint32_t *idx = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    if (!s || !idx) goto oom;
    for (int k = 0; k < 3; ++k) {
        s->lo[k] = INFINITY;
        s->hi[k] = -INFINITY;
    }
    /* Drop never-hit splats up front: zero radius at every key, or a
     * non-finite center at any key. */
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) {
        int any = 0, finite = 1;
        for (uint32_t key = 0; key < nkeys; ++key) {
            const lrt_gsplat *g = &keys[(size_t)key * n + i];
            if (g->radius > 0.f) any = 1;
            if (!isfinite(g->mu[0]) || !isfinite(g->mu[1]) || !isfinite(g->mu[2]))
                finite = 0;
        }
        if (!any || !finite) continue;
        idx[m++] = (uint32_t)i;
        for (uint32_t key = 0; key < nkeys; ++key) {
            const lrt_gsplat *g = &keys[(size_t)key * n + i];
            for (int k = 0; k < 3; ++k) {
                if (g->mu[k] - g->radius < s->lo[k]) s->lo[k] = g->mu[k] - g->radius;
                if (g->mu[k] + g->radius > s->hi[k]) s->hi[k] = g->mu[k] + g->radius;
            }
        }
    }
    s->size = m;
    /* Coarse median splits into spatially coherent chunks of <= cap splats. */
    size_t nchunks = m ? (m + cap - 1) / cap : 1;
    size_t pow2 = 1;
    while (pow2 < nchunks) pow2 <<= 1;
    s->chunks = (gs_chunk *)calloc(pow2, sizeof(gs_chunk));
    if (!s->chunks) goto oom;
    {
        typedef struct { size_t b, e, parts; } seg;
        seg *q = (seg *)malloc(pow2 * 2 * sizeof(seg));
        if (!q) goto oom;
        size_t qn = 0, out = 0;
        q[qn++] = (seg){0, m, pow2};
        while (qn) {
            seg sg = q[--qn];
            if (sg.parts <= 1 || sg.e - sg.b <= cap) {
                if (!gs_build_chunk(splats, keys, n, nkeys, idx, sg.b, sg.e, leaf,
                                    &s->chunks[out++])) {
                    free(q);
                    s->chunk_count = out;
                    goto oom;
                }
                continue;
            }
            const int axis = gs_widest_axis(idx, sg.b, sg.e, splats);
            const size_t mid = sg.b + (sg.e - sg.b) / 2;
            gs_select(idx, sg.b, sg.e, mid, splats, axis);
            q[qn++] = (seg){mid, sg.e, sg.parts / 2};
            q[qn++] = (seg){sg.b, mid, sg.parts / 2};
        }
        free(q);
        s->chunk_count = out;
    }
    free(idx);
    if (err) *err = LRT_RESULT_OK;
    return s;
oom:
    free(idx);
    lrt_gsplat_scene_free(s);
    if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
    return NULL;
}

void lrt_gsplat_scene_free(lrt_gsplat_scene *s) {
    if (!s) return;
    for (size_t i = 0; i < s->chunk_count; ++i) {
        free(s->chunks[i].nodes);
        free(s->chunks[i].splats);
    }
    free(s->chunks);
    free(s);
}

size_t lrt_gsplat_scene_size(const lrt_gsplat_scene *s) { return s ? s->size : 0; }
uint32_t lrt_gsplat_scene_key_count(const lrt_gsplat_scene *s) {
    return s ? s->nkeys : 1u;
}
size_t lrt_gsplat_scene_chunk_count(const lrt_gsplat_scene *s) {
    return s ? s->chunk_count : 0;
}
void lrt_gsplat_scene_bounds(const lrt_gsplat_scene *s, float lo[3], float hi[3]) {
    for (int k = 0; k < 3; ++k) {
        lo[k] = s ? s->lo[k] : 0.f;
        hi[k] = s ? s->hi[k] : 0.f;
    }
}

void lrt_gsplat_scene_chunk(const lrt_gsplat_scene *s, size_t k,
                            const uint32_t **nodes, uint32_t *node_count,
                            const lrt_gsplat **splats, uint32_t *splat_count) {
    const gs_chunk *c = &s->chunks[k];
    *nodes = (const uint32_t *)c->nodes;
    *node_count = c->node_count;
    *splats = c->splats;
    *splat_count = c->splat_count;
}

/* ------------------------------------------------------------------------- */
static int gs_box(const gs_node *nd, const float o[3], const float inv[3],
                  float tmin, float tmax, float *tnear) {
    float t0 = tmin, t1 = tmax;
    for (int k = 0; k < 3; ++k) {
        float a = (nd->lo[k] - o[k]) * inv[k], b = (nd->hi[k] - o[k]) * inv[k];
        if (a > b) {
            float t = a;
            a = b;
            b = t;
        }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
    }
    *tnear = t0;
    return t0 <= t1;
}

/* Trace one chunk, tightening *best_t; returns 1 if a closer hit was found. */
/* Splat i of chunk c at shutter time in [0,1]: linear interpolation between
 * the two neighbouring motion keys (Cycles motion_gsplat interpolates its
 * attributes linearly per step as well). */
static const lrt_gsplat *gs_at_time(const gs_chunk *c, uint32_t nkeys,
                                    uint32_t i, float time, lrt_gsplat *tmp) {
    const lrt_gsplat *k0 = &c->splats[(size_t)i * nkeys];
    if (nkeys == 1) return k0;
    float f = time * (float)(nkeys - 1);
    if (!(f > 0.f)) f = 0.f;
    if (f > (float)(nkeys - 1)) f = (float)(nkeys - 1);
    uint32_t step = (uint32_t)f;
    if (step > nkeys - 2) step = nkeys - 2;
    const float t = f - (float)step, u = 1.f - t;
    const lrt_gsplat *a = k0 + step, *b = k0 + step + 1;
    for (int k = 0; k < 3; ++k) tmp->mu[k] = a->mu[k] * u + b->mu[k] * t;
    for (int k = 0; k < 9; ++k) tmp->a[k] = a->a[k] * u + b->a[k] * t;
    tmp->opacity = a->opacity * u + b->opacity * t;
    tmp->radius = a->radius * u + b->radius * t;
    tmp->id = a->id;
    tmp->pad = 0;
    return tmp;
}

static int gs_trace_chunk(const gs_chunk *c, uint32_t nkeys, float time,
                          const lrt_ray *r, uint32_t seed, float *best_t,
                          lrt_hit *hit) {
    if (!c->node_count || !c->splat_count) return 0;
    const float *o = r->org, *d = r->dir;
    float inv[3];
    for (int k = 0; k < 3; ++k) {
        float v = 1.f / d[k];
        if (!(v >= -1e18f && v <= 1e18f)) v = (d[k] < 0.f) ? -1e18f : 1e18f;
        inv[k] = v;
    }
    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const float tmin = r->tmin;
    float best = *best_t;
    int found = 0;
    uint32_t stack[GS_STACK];
    int sp = 0;
    float tn;
    if (!gs_box(&c->nodes[0], o, inv, tmin, best, &tn)) return 0;
    stack[sp++] = 0;
    while (sp) {
        const gs_node *nd = &c->nodes[stack[--sp]];
        if (nd->count == 0) {
            float ta, tb;
            const int ha = gs_box(&c->nodes[nd->first], o, inv, tmin, best, &ta);
            const int hb = gs_box(&c->nodes[nd->first + 1], o, inv, tmin, best, &tb);
            if (ha && hb && sp + 2 <= GS_STACK) {
                if (ta <= tb) {
                    stack[sp++] = nd->first + 1;
                    stack[sp++] = nd->first;
                } else {
                    stack[sp++] = nd->first;
                    stack[sp++] = nd->first + 1;
                }
            } else if (ha && sp < GS_STACK) {
                stack[sp++] = nd->first;
            } else if (hb && sp < GS_STACK) {
                stack[sp++] = nd->first + 1;
            }
            continue;
        }
        for (uint32_t i = nd->first; i < nd->first + nd->count; ++i) {
            lrt_gsplat tmp;
            const lrt_gsplat *g = gs_at_time(c, nkeys, i, time, &tmp);
            if (!(g->radius > 0.f)) continue;
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
            if (!(t >= tmin && t <= best)) continue;
            const float e[3] = {og[0] + dg[0] * t, og[1] + dg[1] * t, og[2] + dg[2] * t};
            const float alpha =
                g->opacity * expf(-0.5f * (e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
            const float x[3] = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t};
            if (lrt_gsplat_hash(x, seed) > alpha) continue;
            best = t;
            hit->t = t;
            hit->u = alpha;
            hit->v = 0.f;
            hit->prim_id = g->id;
            found = 1;
        }
    }
    *best_t = best;
    return found;
}

int lrt_gsplat_intersect1(const lrt_gsplat_scene *s, const lrt_ray *ray,
                          uint32_t seed, lrt_hit *hit) {
    return lrt_gsplat_intersect1_time(s, ray, 0.5f, seed, hit);
}

int lrt_gsplat_intersect1_time(const lrt_gsplat_scene *s, const lrt_ray *ray,
                               float time, uint32_t seed, lrt_hit *hit) {
    hit->t = 0.f;
    hit->u = hit->v = 0.f;
    hit->prim_id = LRT_TRI_NO_HIT;
    if (!s) return 0;
    float best = ray->tmax;
    int found = 0;
    for (size_t k = 0; k < s->chunk_count; ++k)
        found |= gs_trace_chunk(&s->chunks[k], s->nkeys, time, ray, seed, &best,
                                hit);
    return found;
}

typedef struct gs_job {
    const lrt_gsplat_scene *s;
    const lrt_ray *rays;
    const float *times;
    lrt_hit *out;
    uint32_t n, seed_base;
#if LRT_GS_HAS_THREADS
    atomic_uint next;
    atomic_int hits;
#else
    uint32_t next;
    int hits;
#endif
} gs_job;

static int gs_worker(void *p) {
    gs_job *j = (gs_job *)p;
    int local = 0;
    for (;;) {
#if LRT_GS_HAS_THREADS
        uint32_t b = atomic_fetch_add(&j->next, 256u);
#else
        uint32_t b = j->next;
        j->next += 256u;
#endif
        if (b >= j->n) break;
        uint32_t e = b + 256u < j->n ? b + 256u : j->n;
        for (uint32_t i = b; i < e; ++i)
            local += lrt_gsplat_intersect1_time(
                j->s, &j->rays[i], j->times ? j->times[i] : 0.5f,
                j->seed_base + i, &j->out[i]);
    }
#if LRT_GS_HAS_THREADS
    atomic_fetch_add(&j->hits, local);
#else
    j->hits += local;
#endif
    return 0;
}

static unsigned gs_threads(void) {
    const char *ev = getenv("LRT_GSPLAT_THREADS");
    if (ev && atoi(ev) > 0) return (unsigned)atoi(ev);
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
#else
    long v = sysconf(_SC_NPROCESSORS_ONLN);
    return v > 0 ? (unsigned)v : 1u;
#endif
}

int lrt_gsplat_intersect1N(const lrt_gsplat_scene *s, const lrt_ray *rays,
                           uint32_t n, uint32_t seed_base, lrt_hit *out) {
    return lrt_gsplat_intersect1N_time(s, rays, NULL, n, seed_base, out);
}

int lrt_gsplat_intersect1N_time(const lrt_gsplat_scene *s, const lrt_ray *rays,
                                const float *times, uint32_t n,
                                uint32_t seed_base, lrt_hit *out) {
    gs_job job;
    memset(&job, 0, sizeof(job));
    job.s = s;
    job.rays = rays;
    job.times = times;
    job.out = out;
    job.n = n;
    job.seed_base = seed_base;
#if LRT_GS_HAS_THREADS
    atomic_init(&job.next, 0u);
    atomic_init(&job.hits, 0);
    unsigned nt = gs_threads();
    if (nt > 128) nt = 128;
    if (nt > n / 256u + 1u) nt = n / 256u + 1u;
    thrd_t tids[128];
    unsigned spawned = 0;
    for (unsigned i = 0; i + 1 < nt; ++i) {
        if (thrd_create(&tids[i], gs_worker, &job) != thrd_success) break;
        ++spawned;
    }
    gs_worker(&job);
    for (unsigned i = 0; i < spawned; ++i) thrd_join(tids[i], NULL);
    return atomic_load(&job.hits);
#else
    gs_worker(&job);
    return job.hits;
#endif
}
