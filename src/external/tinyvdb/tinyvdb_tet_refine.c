/* Internal building block for conforming graded tetrahedral backgrounds. */
#include "tinyvdb_tet_refine_internal.h"
#include "tinyvdb_checked.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NONE SIZE_MAX

typedef struct { size_t tet, next, generation; } incidence_t;
struct tvdb_tet_refine_graph {
    double *positions;
    uint32_t *tets;
    size_t *heads, *generations;
    incidence_t *incidence;
    size_t nv, nt, ni, max_vertices, max_tets;
    size_t cv, ct, ci;
};

static tvdb_status_t fail(tvdb_error_t *err, tvdb_status_t status, const char *message) {
    if (err) {
        memset(err, 0, sizeof(*err));
        err->status = status;
        strncpy(err->message, message, sizeof(err->message) - 1);
    }
    return status;
}
static int reserve(void **ptr, size_t *capacity, size_t count, size_t element) {
    size_t n = *capacity ? *capacity : 16, bytes;
    void *p;
    if (count <= *capacity) return 1;
    while (n < count) {
        if (n > SIZE_MAX / 2) { n = count; break; }
        n *= 2;
    }
    if (!tvdb_size_mul(n, element, &bytes)) return 0;
    p = realloc(*ptr, bytes);
    if (!p) return 0;
    *ptr = p; *capacity = n;
    return 1;
}
/* Position and head arrays share a logical capacity but are grown together. */
static int vertices_reserve(tvdb_tet_refine_graph *g, size_t count) {
    size_t cap = g->cv, hcap = g->cv;
    if (!reserve((void **)&g->positions, &cap, count, 3 * sizeof(double))) return 0;
    if (!reserve((void **)&g->heads, &hcap, count, sizeof(size_t))) return 0;
    g->cv = cap;
    return 1;
}
static int tets_reserve(tvdb_tet_refine_graph *g, size_t count) {
    size_t cap = g->ct, gcap = g->ct;
    if (!reserve((void **)&g->tets, &cap, count, 4 * sizeof(uint32_t))) return 0;
    if (!reserve((void **)&g->generations, &gcap, count, sizeof(size_t))) return 0;
    g->ct = cap;
    return 1;
}
static void index_tet(tvdb_tet_refine_graph *g, size_t t) {
    unsigned i;
    for (i = 0; i < 4; ++i) {
        uint32_t v = g->tets[4 * t + i];
        incidence_t *entry = &g->incidence[g->ni];
        entry->tet = t; entry->generation = g->generations[t];
        entry->next = g->heads[v]; g->heads[v] = g->ni++;
    }
}
void tvdb_tet_refine_destroy(tvdb_tet_refine_graph *g) {
    if (!g) return;
    free(g->positions); free(g->tets); free(g->heads);
    free(g->generations); free(g->incidence); free(g);
}
tvdb_status_t tvdb_tet_refine_create(const double *positions, size_t nv,
        const uint32_t *tets, size_t nt, size_t max_vertices, size_t max_tets,
        tvdb_tet_refine_graph **out, tvdb_error_t *err) {
    tvdb_tet_refine_graph *g;
    size_t i, j, k, nodes;
    if (!out) return fail(err, TVDB_ERROR_INVALID_ARGUMENT, "Missing output");
    *out = NULL;
    if ((!positions && nv) || (!tets && nt) || nv > max_vertices || nt > max_tets ||
        max_vertices > UINT32_MAX || nv > SIZE_MAX / 3 || nt > SIZE_MAX / 4 ||
        !tvdb_size_mul(nt, 4, &nodes))
        return fail(err, TVDB_ERROR_INVALID_ARGUMENT, "Invalid mesh size or limits");
    for (i = 0; i < nv * 3; ++i)
        if (!isfinite(positions[i])) return fail(err, TVDB_ERROR_INVALID_DATA, "Non-finite position");
    for (i = 0; i < nt; ++i) for (j = 0; j < 4; ++j) {
        if (tets[4*i+j] >= nv) return fail(err, TVDB_ERROR_INVALID_DATA, "Vertex index out of range");
        for (k = 0; k < j; ++k) if (tets[4*i+j] == tets[4*i+k])
            return fail(err, TVDB_ERROR_INVALID_DATA, "Repeated tetrahedron vertex");
    }
    g = (tvdb_tet_refine_graph *)calloc(1, sizeof(*g));
    if (!g) return fail(err, TVDB_ERROR_OUT_OF_MEMORY, "Allocation failed");
    if (!vertices_reserve(g, nv) || !tets_reserve(g, nt) ||
        !reserve((void **)&g->incidence, &g->ci, nodes, sizeof(incidence_t))) {
        tvdb_tet_refine_destroy(g); return fail(err, TVDB_ERROR_OUT_OF_MEMORY, "Allocation failed");
    }
    if (nv) memcpy(g->positions, positions, nv * 3 * sizeof(double));
    if (nt) memcpy(g->tets, tets, nt * 4 * sizeof(uint32_t));
    for (i = 0; i < nv; ++i) g->heads[i] = NONE;
    for (i = 0; i < nt; ++i) { g->generations[i] = 0; index_tet(g, i); }
    g->nv = nv; g->nt = nt; g->max_vertices = max_vertices; g->max_tets = max_tets;
    *out = g; return TVDB_OK;
}
static int compare_size(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return (x > y) - (x < y);
}
static tvdb_status_t split_impl(tvdb_tet_refine_graph *g, uint32_t a,
        uint32_t b, uint32_t *midpoint, size_t **changed, size_t *changed_count,
        tvdb_error_t *err) {
    size_t node, count = 0, i, j, old_nt, extra, *parents = NULL;
    uint32_t m;
    if (!g || !midpoint || a == b || a >= g->nv || b >= g->nv)
        return fail(err, TVDB_ERROR_INVALID_ARGUMENT, "Invalid edge");
    {
        int same_a = 1, same_b = 1;
        for (j = 0; j < 3; ++j) {
            double x = g->positions[3*(size_t)a+j], y = g->positions[3*(size_t)b+j];
            double m = x * 0.5 + y * 0.5;
            same_a &= m == x; same_b &= m == y;
        }
        if (same_a || same_b)
            return fail(err, TVDB_ERROR_INVALID_DATA, "Edge midpoint is not representable");
    }
    for (node = g->heads[a]; node != NONE; node = g->incidence[node].next) {
        const incidence_t *e = &g->incidence[node];
        if (e->generation != g->generations[e->tet]) continue;
        for (j = 0; j < 4; ++j) if (g->tets[4*e->tet+j] == b) { ++count; break; }
    }
    if (!count) return fail(err, TVDB_ERROR_INVALID_ARGUMENT, "Edge is not in the mesh");
    if (g->nv == g->max_vertices || count > g->max_tets - g->nt ||
        !tvdb_size_mul(count, 8, &extra) || extra > SIZE_MAX - g->ni ||
        count > SIZE_MAX / sizeof(size_t))
        return fail(err, TVDB_ERROR_INVALID_DATA, "Refinement limit exceeded");
    parents = (size_t *)malloc(count * sizeof(size_t));
    if (!parents) return fail(err, TVDB_ERROR_OUT_OF_MEMORY, "Allocation failed");
    i = 0;
    for (node = g->heads[a]; node != NONE; node = g->incidence[node].next) {
        const incidence_t *e = &g->incidence[node];
        if (e->generation != g->generations[e->tet]) continue;
        for (j = 0; j < 4; ++j) if (g->tets[4*e->tet+j] == b) { parents[i++] = e->tet; break; }
    }
    qsort(parents, count, sizeof(size_t), compare_size);
    for (i = 0; i < count; ++i) if (g->generations[parents[i]] == SIZE_MAX) {
        free(parents); return fail(err, TVDB_ERROR_INVALID_DATA, "Generation overflow");
    }
    if (!vertices_reserve(g, g->nv + 1) || !tets_reserve(g, g->nt + count) ||
        !reserve((void **)&g->incidence, &g->ci, g->ni + extra, sizeof(incidence_t))) {
        free(parents); return fail(err, TVDB_ERROR_OUT_OF_MEMORY, "Allocation failed");
    }
    m = (uint32_t)g->nv; g->heads[m] = NONE;
    for (j = 0; j < 3; ++j)
        g->positions[3*(size_t)m+j] = g->positions[3*(size_t)a+j] * 0.5 + g->positions[3*(size_t)b+j] * 0.5;
    ++g->nv; old_nt = g->nt; g->nt += count;
    for (i = 0; i < count; ++i) {
        size_t p = parents[i], child = old_nt + i;
        memcpy(&g->tets[4*child], &g->tets[4*p], 4 * sizeof(uint32_t));
        for (j = 0; j < 4; ++j) {
            if (g->tets[4*p+j] == a) g->tets[4*p+j] = m;
            if (g->tets[4*child+j] == b) g->tets[4*child+j] = m;
        }
        ++g->generations[p]; g->generations[child] = 0;
        index_tet(g, p); index_tet(g, child);
    }
    if (changed) { *changed = parents; *changed_count = count; }
    else free(parents);
    *midpoint = m; return TVDB_OK;
}
tvdb_status_t tvdb_tet_refine_split(tvdb_tet_refine_graph *g, uint32_t a,
        uint32_t b, uint32_t *midpoint, tvdb_error_t *err) {
    return split_impl(g, a, b, midpoint, NULL, NULL, err);
}
tvdb_status_t tvdb_tet_refine_adapt(tvdb_tet_refine_graph *g,
        tvdb_tet_refine_size_fn size, void *user, tvdb_error_t *err) {
    size_t *queue = NULL, cap = 0, head = 0, tail = 0, i;
    tvdb_status_t st = TVDB_OK;
    if (!g || !size) return fail(err, TVDB_ERROR_INVALID_ARGUMENT, "Missing refinement graph or size callback");
    if (!reserve((void **)&queue, &cap, g->nt, sizeof(size_t)))
        return fail(err, TVDB_ERROR_OUT_OF_MEMORY, "Refinement queue allocation failed");
    for (i = 0; i < g->nt; ++i) queue[tail++] = i;
    while (head < tail) {
        size_t t = queue[head++];
        uint32_t a = 0, b = 0;
        double longest = 0;
        for (unsigned x = 0; x < 4; ++x) for (unsigned y = x + 1; y < 4; ++y) {
            uint32_t u = g->tets[4*t+x], v = g->tets[4*t+y];
            double midpoint[3], delta[3], desired = NAN, length;
            for (unsigned d = 0; d < 3; ++d) {
                double p = g->positions[3*(size_t)u+d], q = g->positions[3*(size_t)v+d];
                midpoint[d] = 0.5*p + 0.5*q; delta[d] = p-q;
            }
            length = hypot(hypot(delta[0], delta[1]), delta[2]);
            if (!isfinite(length) || !size(midpoint, &desired, user) ||
                !isfinite(desired) || desired <= 0) {
                st = fail(err, TVDB_ERROR_INVALID_DATA, "Invalid refinement size or edge length");
                goto done;
            }
            if (length > desired && length > longest) { longest = length; a = u; b = v; }
        }
        if (longest > 0) {
            size_t old_nt = g->nt, *parents = NULL, count = 0, extra;
            uint32_t midpoint;
            st = split_impl(g, a, b, &midpoint, &parents, &count, err);
            if (st != TVDB_OK) goto done;
            if (!tvdb_size_mul(count, 2, &extra) || extra > SIZE_MAX - tail ||
                !reserve((void **)&queue, &cap, tail + extra, sizeof(size_t))) {
                free(parents);
                st = fail(err, TVDB_ERROR_OUT_OF_MEMORY, "Refinement queue allocation failed");
                goto done;
            }
            for (i = 0; i < count; ++i) {
                queue[tail++] = parents[i]; queue[tail++] = old_nt + i;
            }
            free(parents);
        }
    }
done:
    free(queue); return st;
}
const double *tvdb_tet_refine_positions(const tvdb_tet_refine_graph *g) { return g ? g->positions : NULL; }
const uint32_t *tvdb_tet_refine_tets(const tvdb_tet_refine_graph *g) { return g ? g->tets : NULL; }
size_t tvdb_tet_refine_vertex_count(const tvdb_tet_refine_graph *g) { return g ? g->nv : 0; }
size_t tvdb_tet_refine_tet_count(const tvdb_tet_refine_graph *g) { return g ? g->nt : 0; }
