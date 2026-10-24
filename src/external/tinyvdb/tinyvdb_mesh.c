#include "tinyvdb_mesh.h"
#include "tvdb_memory.h"
#include "tinyvdb_mesh_conversion.h"
#include "tinyvdb_checked.h"
#include <float.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

#define TVDB_MAX_GRID_DIM 2048

static const int MC_EDGE_TABLE[256] = {
  0x0  , 0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c,
  0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
  0x190, 0x99 , 0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c,
  0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
  0x230, 0x339, 0x33 , 0x13a, 0x636, 0x73f, 0x435, 0x53c,
  0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
  0x3a0, 0x2a9, 0x1a3, 0xaa , 0x7a6, 0x6af, 0x5a5, 0x4ac,
  0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
  0x460, 0x569, 0x663, 0x76a, 0x66 , 0x16f, 0x265, 0x36c,
  0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
  0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff , 0x3f5, 0x2fc,
  0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
  0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55 , 0x15c,
  0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
  0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc ,
  0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
  0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc,
  0xcc , 0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
  0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c,
  0x15c, 0x55 , 0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
  0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc,
  0x2fc, 0x3f5, 0xff , 0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
  0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c,
  0x36c, 0x265, 0x16f, 0x66 , 0x76a, 0x663, 0x569, 0x460,
  0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac,
  0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa , 0x1a3, 0x2a9, 0x3a0,
  0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c,
  0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33 , 0x339, 0x230,
  0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c,
  0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99 , 0x190,
  0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c,
  0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0
};

#include "tinyvdb_mc_tri_table.h"  // canonical 256-row MC triangle table

static void* arena_alloc_wrapper(tvdb_arena_allocator_t* arena, size_t size) {
    if (!arena) return malloc(size);
    return tvdb_arena_alloc(arena, size);
}

// C-compatible math helpers
static inline float dot_c(tvdb_vec3f a, tvdb_vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline tvdb_vec3f sub_c(tvdb_vec3f a, tvdb_vec3f b) { return (tvdb_vec3f){a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline tvdb_vec3f add_c(tvdb_vec3f a, tvdb_vec3f b) { return (tvdb_vec3f){a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline tvdb_vec3f mul_c(tvdb_vec3f a, float s) { return (tvdb_vec3f){a.x * s, a.y * s, a.z * s}; }
static float dist_sq_c(tvdb_vec3f a, tvdb_vec3f b) { tvdb_vec3f d = sub_c(a, b); return dot_c(d, d); }
static float point_triangle_dist_sq_c(tvdb_vec3f p, tvdb_vec3f a, tvdb_vec3f b, tvdb_vec3f c) {
  tvdb_vec3f ab = sub_c(b, a), ac = sub_c(c, a), ap = sub_c(p, a);
  float d1 = dot_c(ab, ap), d2 = dot_c(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f) return dist_sq_c(p, a);
  tvdb_vec3f bp = sub_c(p, b);
  float d3 = dot_c(ab, bp), d4 = dot_c(ac, bp);
  if (d3 >= 0.0f && d4 <= d3) return dist_sq_c(p, b);
  float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
    float v = d1 / (d1 - d3);
    return dist_sq_c(p, add_c(a, mul_c(ab, v)));
  }
  tvdb_vec3f cp = sub_c(p, c);
  float d5 = dot_c(ab, cp), d6 = dot_c(ac, cp);
  if (d6 >= 0.0f && d5 <= d6) return dist_sq_c(p, c);
  float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
    float w = d2 / (d2 - d6);
    return dist_sq_c(p, add_c(a, mul_c(ac, w)));
  }
  float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
    float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return dist_sq_c(p, add_c(b, mul_c(sub_c(c, b), w)));
  }
  float denom = 1.0f / (va + vb + vc);
  return dist_sq_c(p, add_c(add_c(a, mul_c(ab, vb * denom)), mul_c(ac, vc * denom)));
}

// VoxelPos, VoxelIdx, EdgeKey
static tvdb_vec3f voxel_pos_c(const tvdb_dense_grid* grid, int ix, int iy, int iz) {
    return (tvdb_vec3f){grid->ox + (ix + 0.5f) * grid->voxel_size,
                        grid->oy + (iy + 0.5f) * grid->voxel_size,
                        grid->oz + (iz + 0.5f) * grid->voxel_size};
}

static uint64_t voxel_idx_c(int nx, int ny, int ix, int iy, int iz) {
    return (uint64_t)ix + (uint64_t)iy * nx + (uint64_t)iz * nx * ny;
}

// -------------------------------------------------------------------------
// SDF -> mesh (marching cubes)
// -------------------------------------------------------------------------

// Marching-cubes corner-of-cube offsets and edge endpoint table.
static const int MC_CORNER_OFFSETS[8][3] = {
    {0,0,0},{1,0,0},{1,1,0},{0,1,0},
    {0,0,1},{1,0,1},{1,1,1},{0,1,1}
};

// MC edge -> (corner_a, corner_b) using the canonical lookup-table indexing.
static const int MC_EDGE_VERTS[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7}
};

const int* tvdb_mc_edge_table(void) { return MC_EDGE_TABLE; }
const int* tvdb_mc_tri_table_flat(void) { return (const int*)MC_TRI_TABLE; }

static tvdb_status_t mesh_validate(const tvdb_triangle_mesh *, tvdb_error_t *);

// -------------------------------------------------------------------------
// Mesh -> SDF (closest-triangle, signed via face normal)
// -------------------------------------------------------------------------
//
// For each voxel, compute distance to nearest triangle; sign comes from
// dot((voxel - closest_point), triangle_normal). This is the classic
// "pseudo-normal-free" approach: simple, robust for moderately well-formed
// closed meshes, but can have sign artifacts at sharp edges/vertices.
//
// Distance is clamped to ±band_width.

/* Double fallback for degenerate edges and overflow in float predicates. */
static double mesh_dot3d(const double *a, const double *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static tvdb_vec3f mesh_triangle_double(tvdb_vec3f p, tvdb_vec3f a, tvdb_vec3f b, tvdb_vec3f c) {
    double pa[3] = {(double)p.x - a.x, (double)p.y - a.y, (double)p.z - a.z};
    double ab[3] = {(double)b.x - a.x, (double)b.y - a.y, (double)b.z - a.z},
           ac[3] = {(double)c.x - a.x, (double)c.y - a.y, (double)c.z - a.z};
    double normal[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                        ab[0] * ac[1] - ab[1] * ac[0]};
    if (!mesh_dot3d(normal, normal)) {
        const tvdb_vec3f v[3] = {a, b, c};
        double best = INFINITY;
        tvdb_vec3f result = a;
        for (int e = 0; e < 3; ++e) {
            tvdb_vec3f x = v[e], y = v[(e + 1) % 3];
            double d[3] = {(double)y.x - x.x, (double)y.y - x.y, (double)y.z - x.z};
            double q[3] = {(double)p.x - x.x, (double)p.y - x.y, (double)p.z - x.z},
                   length = mesh_dot3d(d, d);
            double t = length ? mesh_dot3d(q, d) / length : 0;
            if (t < 0)
                t = 0;
            if (t > 1)
                t = 1;
            double r[3] = {x.x + t * d[0], x.y + t * d[1], x.z + t * d[2]};
            double delta[3] = {(double)p.x - r[0], (double)p.y - r[1], (double)p.z - r[2]},
                   distance = mesh_dot3d(delta, delta);
            if (distance < best) {
                best = distance;
                result = (tvdb_vec3f){(float)r[0], (float)r[1], (float)r[2]};
            }
        }
        return result;
    }
    double d1 = mesh_dot3d(ab, pa), d2 = mesh_dot3d(ac, pa);
    if (d1 <= 0 && d2 <= 0)
        return a;
    double pb[3] = {(double)p.x - b.x, (double)p.y - b.y, (double)p.z - b.z};
    double d3 = mesh_dot3d(ab, pb), d4 = mesh_dot3d(ac, pb);
    if (d3 >= 0 && d4 <= d3)
        return b;
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        double t = d1 / (d1 - d3);
        return (tvdb_vec3f){(float)(a.x + t * ab[0]), (float)(a.y + t * ab[1]),
                            (float)(a.z + t * ab[2])};
    }
    double pc[3] = {(double)p.x - c.x, (double)p.y - c.y, (double)p.z - c.z};
    double d5 = mesh_dot3d(ab, pc), d6 = mesh_dot3d(ac, pc);
    if (d6 >= 0 && d5 <= d6)
        return c;
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        double t = d2 / (d2 - d6);
        return (tvdb_vec3f){(float)(a.x + t * ac[0]), (float)(a.y + t * ac[1]),
                            (float)(a.z + t * ac[2])};
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return (tvdb_vec3f){(float)(b.x + t * ((double)c.x - b.x)),
                            (float)(b.y + t * ((double)c.y - b.y)),
                            (float)(b.z + t * ((double)c.z - b.z))};
    }
    double v = vb / (va + vb + vc), w = vc / (va + vb + vc);
    return (tvdb_vec3f){(float)(a.x + v * ab[0] + w * ac[0]), (float)(a.y + v * ab[1] + w * ac[1]),
                        (float)(a.z + v * ab[2] + w * ac[2])};
}
static tvdb_vec3f tri_closest_point_c(tvdb_vec3f p, tvdb_vec3f a, tvdb_vec3f b, tvdb_vec3f c) {
    // Same case analysis as point_triangle_dist_sq_c, but returning the point.
    tvdb_vec3f ab = sub_c(b, a), ac = sub_c(c, a), ap = sub_c(p, a);
    float d1 = dot_c(ab, ap), d2 = dot_c(ac, ap);
    if (!isfinite(d1) || !isfinite(d2))
        return mesh_triangle_double(p, a, b, c);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return a;
    tvdb_vec3f bp = sub_c(p, b);
    float d3 = dot_c(ab, bp), d4 = dot_c(ac, bp);
    if (!isfinite(d3) || !isfinite(d4))
        return mesh_triangle_double(p, a, b, c);
    if (d3 >= 0.0f && d4 <= d3)
        return b;
    float vc = d1 * d4 - d3 * d2;
    if (!isfinite(vc))
        return mesh_triangle_double(p, a, b, c);
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        if (d1 == d3)
            return mesh_triangle_double(p, a, b, c);
        float v = d1 / (d1 - d3);
        return add_c(a, mul_c(ab, v));
    }
    tvdb_vec3f cp = sub_c(p, c);
    float d5 = dot_c(ab, cp), d6 = dot_c(ac, cp);
    if (!isfinite(d5) || !isfinite(d6))
        return mesh_triangle_double(p, a, b, c);
    if (d6 >= 0.0f && d5 <= d6)
        return c;
    float vb = d5 * d2 - d1 * d6;
    if (!isfinite(vb))
        return mesh_triangle_double(p, a, b, c);
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        if (d2 == d6)
            return mesh_triangle_double(p, a, b, c);
        float w = d2 / (d2 - d6);
        return add_c(a, mul_c(ac, w));
    }
    float va = d3 * d6 - d5 * d4;
    if (!isfinite(va))
        return mesh_triangle_double(p, a, b, c);
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        if ((d4 - d3) + (d5 - d6) == 0)
            return mesh_triangle_double(p, a, b, c);
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return add_c(b, mul_c(sub_c(c, b), w));
    }
    if (!isfinite(va + vb + vc) || va + vb + vc <= 0)
        return mesh_triangle_double(p, a, b, c);
    float denom = 1.0f / (va + vb + vc);
    return add_c(add_c(a, mul_c(ab, vb * denom)), mul_c(ac, vc * denom));
}

static tvdb_vec3f cross_c(tvdb_vec3f a, tvdb_vec3f b) {
    return (tvdb_vec3f){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static tvdb_vec3f normalize_c(tvdb_vec3f v) {
    float L = sqrtf(dot_c(v, v));
    if (L < 1e-30f) return (tvdb_vec3f){0, 0, 0};
    return mul_c(v, 1.0f / L);
}

// -------------------------------------------------------------------------
// BVH over mesh triangles
// -------------------------------------------------------------------------
//
// tvdb_mesh_to_sdf evaluates every voxel against every triangle, which is
// O(voxels * faces): a 64^3 grid over a marching-cubes sphere took 63
// seconds. A bounding volume hierarchy over the triangle set makes the query
// near-O(log n) on average and is the single largest win available here.
//
// Two properties are preserved exactly so results stay bit-identical:
//
//  * The brute-force loop accepts a triangle only on a *strictly* smaller
//    squared distance (`dsq < best_dsq`), so when two faces are exactly
//    equidistant the lower face index wins. The traversal below uses the same
//    strict test plus an explicit tie-break on the lower face index, so the
//    chosen triangle -- and hence both the distance and the sign -- matches
//    the brute-force result exactly.
//  * Node bounds are only ever used to *prune*. A node is skipped solely when
//    its lower bound already exceeds the current best, so a loose bound can
//    cost time but can never change the answer.

/* The node type is public (tinyvdb_mesh.h) because the GPU backend uploads these
 * verbatim and walks them in a shader. One definition, so the two cannot drift. */
_Static_assert(sizeof(tvdb_mesh_bvh_node_t) == 40,
               "BVH node must stay 40 bytes: it is uploaded as a std430 struct");
typedef tvdb_mesh_bvh_node_t tvdb_bvh_node_t;

typedef struct {
    int32_t *prim;              /* face indices, partitioned by the build */
    int32_t prim_count;
    tvdb_bvh_node_t *nodes;
    int32_t node_count;
    int32_t node_capacity;
    int32_t max_depth;
    /* Six floats per face, interleaved as {bmin.x,bmin.y,bmin.z,bmax.x,bmax.y,
       bmax.z}. Kept in one array rather than two so a face's box is 24 contiguous
       bytes and normally one cache line: the traversal reads it for every leaf
       triangle it tests, and the split layout cost two lines per face. */
    float *tri_box;
} tvdb_bvh_t;

struct tvdb_mesh_bvh { tvdb_bvh_t b; };

static void tvdb_bvh_free(tvdb_bvh_t *b) {
    if (!b) return;
    free(b->prim);
    free(b->nodes);
    free(b->tri_box);
    memset(b, 0, sizeof(*b));
}

static int32_t tvdb_bvh_new_node(tvdb_bvh_t *b) {
    if (b->node_count >= b->node_capacity) {
        if (b->node_capacity == INT32_MAX) return -1;
        int32_t new_cap = !b->node_capacity ? 256 : b->node_capacity > INT32_MAX/2 ? INT32_MAX : b->node_capacity*2;
        if ((size_t)new_cap > SIZE_MAX/sizeof(tvdb_bvh_node_t)) return -1;
        tvdb_bvh_node_t *n = (tvdb_bvh_node_t *)realloc(
            b->nodes, (size_t)new_cap * sizeof(*n));
        if (!n) return -1;
        b->nodes = n;
        b->node_capacity = new_cap;
    }
    int32_t idx = b->node_count++;
    tvdb_bvh_node_t *nd = &b->nodes[idx];
    nd->left = -1; nd->right = -1; nd->start = 0; nd->count = 0;
    return idx;
}

static inline float tvdb_aabb_dist_sq(const float *bmin, const float *bmax,
                                     const tvdb_vec3f *p) {
    const float v[3] = { p->x, p->y, p->z };
    float d2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
        if (v[a] < bmin[a]) { float t = bmin[a] - v[a]; d2 += t * t; }
        else if (v[a] > bmax[a]) { float t = v[a] - bmax[a]; d2 += t * t; }
    }
    return d2;
}

/* Split [lo,hi) of b->prim at the median centroid on the widest axis.
 *
 * A binned surface-area heuristic and a Morton-ordered midpoint split were
 * both implemented and measured; neither beat this. SAH scored planes against
 * triangle AABB centroids, and for a level-set shell every centroid sits on the
 * same thin surface, so the bins carry no information. Morton ordering groups
 * space well but its balanced midpoint split produces long, thin boxes across
 * the shell. The median split wins because the resulting node boxes stay
 * compact even though the ordering ignores that structure.
 *
 * Returns a split point strictly inside the range. */
static int32_t tvdb_bvh_split(tvdb_bvh_t *b, int32_t lo, int32_t hi) {
    float cmin[3] = { INFINITY, INFINITY, INFINITY };
    float cmax[3] = { -INFINITY, -INFINITY, -INFINITY };
    for (int32_t i = lo; i < hi; ++i) {
        int32_t f = b->prim[i];
        for (int a = 0; a < 3; ++a) {
            float c = 0.5f * (b->tri_box[(size_t)f * 6 + a] +
                              b->tri_box[(size_t)f * 6 + 3 + a]);
            if (c < cmin[a]) cmin[a] = c;
            if (c > cmax[a]) cmax[a] = c;
        }
    }
    int axis = 0;
    float widest = cmax[0] - cmin[0];
    for (int a = 1; a < 3; ++a) {
        float e = cmax[a] - cmin[a];
        if (e > widest) { widest = e; axis = a; }
    }
    if (!(widest > 0.0f)) return lo + 1;   /* all centroids coincide */

    int32_t mid = lo + (hi - lo) / 2;
    int32_t mf = b->prim[mid];
    float pivot = 0.5f * (b->tri_box[(size_t)mf * 6 + axis] +
                          b->tri_box[(size_t)mf * 6 + 3 + axis]);
    int32_t i = lo, j = hi - 1;
    for (;;) {
        while (i < hi) {
            int32_t f = b->prim[i];
            float c = 0.5f * (b->tri_box[(size_t)f * 6 + axis] +
                              b->tri_box[(size_t)f * 6 + 3 + axis]);
            if (!(c < pivot)) break;
            i++;
        }
        while (j > lo) {
            int32_t f = b->prim[j];
            float c = 0.5f * (b->tri_box[(size_t)f * 6 + axis] +
                              b->tri_box[(size_t)f * 6 + 3 + axis]);
            if (!(c > pivot)) break;
            j--;
        }
        if (i >= j) break;
        int32_t t = b->prim[i]; b->prim[i] = b->prim[j]; b->prim[j] = t;
        i++; j--;
    }
    int32_t split = i;
    if (split <= lo) split = lo + 1;
    if (split >= hi) split = hi - 1;
    return split;
}

/* Recursively build a node covering [lo,hi). Returns its index, or -1 on OOM.
 * Node indices are stable because the array only grows and existing entries
 * are never moved. */
static int32_t tvdb_bvh_build(tvdb_bvh_t *b, int32_t lo, int32_t hi, int depth) {
    int32_t self = tvdb_bvh_new_node(b);
    if (self < 0) return -1;

    float nmin[3] = { INFINITY, INFINITY, INFINITY };
    float nmax[3] = { -INFINITY, -INFINITY, -INFINITY };
    for (int32_t i = lo; i < hi; ++i) {
        int32_t f = b->prim[i];
        for (int a = 0; a < 3; ++a) {
            float l0 = b->tri_box[(size_t)f * 6 + a];
            float h0 = b->tri_box[(size_t)f * 6 + 3 + a];
            if (l0 < nmin[a]) nmin[a] = l0;
            if (h0 > nmax[a]) nmax[a] = h0;
        }
    }
    for (int a = 0; a < 3; ++a) {
        b->nodes[self].bmin[a] = nmin[a];
        b->nodes[self].bmax[a] = nmax[a];
    }

    int32_t n = hi - lo;
    /* Small leaves, or exhausted depth, become leaves. The depth cap also
     * bounds the traversal stack in tvdb_bvh_closest to 64+1 entries. */
    if (n <= 4 || depth >= 64) {
        b->nodes[self].start = lo;
        b->nodes[self].count = n;
        if (depth > b->max_depth) b->max_depth = depth;
        return self;
    }

    int32_t split = tvdb_bvh_split(b, lo, hi);
    int32_t l = tvdb_bvh_build(b, lo, split, depth + 1);
    if (l < 0) return -1;
    int32_t r = tvdb_bvh_build(b, split, hi, depth + 1);
    if (r < 0) return -1;
    b->nodes[self].left = l;
    b->nodes[self].right = r;
    return self;
}

/* Build the hierarchy over all `nf` faces of `mesh`. Returns 0 on success. */
static int tvdb_bvh_init(tvdb_bvh_t *b, const tvdb_triangle_mesh *mesh, size_t nf) {
    memset(b, 0, sizeof(*b));
    if (nf == 0) return -1;
    if (nf > (size_t)INT32_MAX || nf > SIZE_MAX/(6*sizeof(float))) return -1;
    b->prim_count = (int32_t)nf;

    b->prim = (int32_t *)malloc(nf * sizeof(int32_t));
    b->tri_box = (float *)malloc(nf * 6 * sizeof(float));
    if (!b->prim || !b->tri_box) { tvdb_bvh_free(b); return -1; }

    for (size_t f = 0; f < nf; ++f) {
        b->prim[f] = (int32_t)f;
        const tvdb_triangle *face = &mesh->faces[f];
        float lo[3], hi[3];
        /* Derive the bounds from the three corners the distance routine will
         * actually use, so a degenerate face (repeated index, NaN-free) still
         * produces a well-defined box. */
        const uint32_t vi[3] = { face->v0, face->v1, face->v2 };
        for (int a = 0; a < 3; ++a) { lo[a] = INFINITY; hi[a] = -INFINITY; }
        for (int c = 0; c < 3; ++c) {
            const tvdb_vec3f *v = &mesh->vertices[vi[c]];
            float xyz[3] = { v->x, v->y, v->z };
            for (int a = 0; a < 3; ++a) {
                if (xyz[a] < lo[a]) lo[a] = xyz[a];
                if (xyz[a] > hi[a]) hi[a] = xyz[a];
            }
        }
        for (int a = 0; a < 3; ++a) {
            b->tri_box[f * 6 + a]     = lo[a];
            b->tri_box[f * 6 + 3 + a] = hi[a];
        }
    }

    int32_t root = tvdb_bvh_build(b, 0, (int32_t)nf, 0);
    if (root < 0) { tvdb_bvh_free(b); return -1; }
    return 0;
}

/* ---- public BVH accessors -------------------------------------------------
 *
 * Heap-wrapped so a caller can hold one for the lifetime of a GPU job and free
 * it deterministically. CPU voxelization can retain the same acceleration
 * in a tvdb_mesh_sdf_t workspace across repeated lattice generations. */
bool tvdb_mesh_bvh_build(const tvdb_triangle_mesh* mesh, tvdb_mesh_bvh_t** out) {
    if (!out) return false;
    *out = NULL;
    if (mesh_validate(mesh, NULL) != TVDB_OK) return false;
    tvdb_mesh_bvh_t* h = (tvdb_mesh_bvh_t*)calloc(1, sizeof(*h));
    if (!h) return false;
    if (tvdb_bvh_init(&h->b, mesh, mesh->face_count) != 0) { free(h); return false; }
    *out = h;
    return true;
}

void tvdb_mesh_bvh_destroy(tvdb_mesh_bvh_t* h) {
    if (!h) return;
    tvdb_bvh_free(&h->b);
    free(h);
}

int32_t     tvdb_mesh_bvh_node_count(const tvdb_mesh_bvh_t* h) { return h ? h->b.node_count : 0; }
int32_t     tvdb_mesh_bvh_prim_count(const tvdb_mesh_bvh_t* h) { return h ? h->b.prim_count : 0; }
int32_t     tvdb_mesh_bvh_max_depth(const tvdb_mesh_bvh_t* h) { return h ? h->b.max_depth : 0; }
const tvdb_mesh_bvh_node_t* tvdb_mesh_bvh_nodes(const tvdb_mesh_bvh_t* h) { return h ? h->b.nodes : NULL; }
const int32_t* tvdb_mesh_bvh_prims(const tvdb_mesh_bvh_t* h) { return h ? h->b.prim : NULL; }

/* Closest face to `p`, by squared distance. On return `*out_cp` is the
 * closest point and `*out_n` that face's normal, mirroring the brute-force
 * loop's initialization exactly: if no face is ever accepted (which happens
 * when every candidate distance is NaN, e.g. a mesh of zero-area triangles),
 * both stay {0,0,0} and the caller produces the same signed value it always
 * did. Recomputing the closest point from `best_face` would instead yield NaN
 * and flip the sign at those voxels.
 *
 * `seed_dsq`/`seed_face` start the search from an already-known candidate; pass
 * INFINITY / -1 for a cold search. `*out_face` receives the winning face so the
 * caller can seed the next query with it.
 *
 * Seeding is exact, not a heuristic, and it is the *pair* that matters:
 *   - Both pruning tests are strict `>`, so a candidate is dropped only when
 *     something is provably farther than the current best. A tighter bound can
 *     only prune more, never wrongly.
 *   - The acceptance test is `dsq < best_dsq || (dsq == best_dsq && f < best_face)`,
 *     i.e. the lowest face index wins an exact tie -- which is the rule the
 *     brute-force loop applies. It is applied per candidate, not once at the end,
 *     so starting from a different candidate yields the same winner.
 * The one thing that must stay consistent is best_dsq, best_face and
 * (best_cp, best_n): the caller derives the signed distance from best_n and the
 * distance from best_dsq, so reusing the previous voxel's best_dsq with this
 * voxel's geometry yields a wrong magnitude. The seed is therefore fully
 * re-measured at this voxel's position and becomes a genuine candidate, exactly
 * as if it had been examined first; a later face that beats it overwrites all four
 * together. Since the acceptance rule is order-independent, examining the seed
 * first changes nothing about the winner. */
static float tvdb_bvh_closest(const tvdb_bvh_t *b,
                              const tvdb_triangle_mesh *mesh,
                              const tvdb_vec3f *tri_n,
                              tvdb_vec3f p,
                              float seed_dsq, int32_t seed_face,
                              tvdb_vec3f *out_cp, tvdb_vec3f *out_n,
                              int32_t *out_face) {
    float best_dsq = seed_dsq;
    int32_t best_face = seed_face;
    tvdb_vec3f best_cp = { 0, 0, 0 };
    tvdb_vec3f best_n  = { 0, 0, 0 };
    /* Materialize the seed as a real candidate at *this* voxel's position, not
     * just as a pruning bound. When the seed is in fact the winner -- the common
     * case once a row has converged on one face -- nothing is ever accepted, so
     * without this best_cp/best_n would stay {0,0,0} and the caller's sign (which
     * comes from best_n) would flip. If a later face wins, the acceptance test
     * overwrites all three. */
    if (seed_face >= 0 && seed_face < (int32_t)b->prim_count) {
        const tvdb_triangle *sf = &mesh->faces[seed_face];
        best_cp = tri_closest_point_c(p, mesh->vertices[sf->v0],
                                      mesh->vertices[sf->v1], mesh->vertices[sf->v2]);
        best_n = tri_n[seed_face];
        /* Re-measure the seed at *this* position. seed_dsq is the previous voxel's
         * distance to this face, which is not an answer for this voxel: if the
         * seed goes on to win -- the common case once a row has settled -- nothing
         * else is ever accepted and a stale best_dsq would be returned as the
         * voxel's distance. Recomputing is also the better pruning bound, since
         * it is this voxel's true distance rather than a neighbour's. */
        tvdb_vec3f sd = sub_c(p, best_cp);
        best_dsq = dot_c(sd, sd);
    }

    /* Explicit stack of (node index, lower-bound distance).
     *
     * A binary DFS pops one node and pushes at most two, so the stack can
     * never hold more than tree_depth + 1 entries. The builder caps depth at
     * 64, so 128 is a comfortable margin and the `sp < 126` guards below are
     * unreachable in practice -- but they are written to leave the traversal
     * safe (never out of bounds) rather than to silently corrupt results. */
    int32_t stack_node[128];
    float  stack_d[128];
    int sp = 0;

    stack_node[sp] = 0;
    stack_d[sp] = 0.0f;
    sp++;

    while (sp > 0) {
        sp--;
        int32_t ni = stack_node[sp];
        float nd = stack_d[sp];
        /* Prune only when the subtree provably cannot match the current best.
         *
         * The test must be strict `>`, not `>=`. The brute-force original keeps
         * the *lowest* face index among exact ties, so a triangle sitting at
         * exactly best_dsq can still improve the result by having a smaller
         * index. Pruning on equality would silently drop those. (The bound is
         * inclusive of the node's own surface, so equality is reachable.) */
        if (nd > best_dsq) continue;
        const tvdb_bvh_node_t *node = &b->nodes[ni];

        if (node->left < 0) {
            for (int32_t k = 0; k < node->count; ++k) {
                int32_t f = b->prim[node->start + k];
                /* Reject on the triangle's own box before touching the mesh.
                 *
                 * The closest point on a triangle is always inside the triangle's
                 * AABB, so a box strictly farther than the current best cannot
                 * improve the result. That replaces a ~50-flop closest-point
                 * case analysis (plus three scattered vertex loads) with a 6-flop
                 * squared-distance test, and the leaf triangles are the ones the
                 * node-level pruning cannot help with -- a node box is the union
                 * of its children, so a node the query is near often holds
                 * triangles that are nowhere near it.
                 *
                 * Strict `>` for the same reason as the node test above: a
                 * triangle at exactly best_dsq can still win the index tie-break. */
                const float* tb = &b->tri_box[(size_t)f * 6];
                if (tvdb_aabb_dist_sq(tb, tb + 3, &p) > best_dsq) continue;
                const tvdb_triangle *face = &mesh->faces[f];
                tvdb_vec3f a = mesh->vertices[face->v0];
                tvdb_vec3f bv = mesh->vertices[face->v1];
                tvdb_vec3f c = mesh->vertices[face->v2];
                tvdb_vec3f cp = tri_closest_point_c(p, a, bv, c);
                tvdb_vec3f d = sub_c(p, cp);
                float dsq = dot_c(d, d);
                /* Strict `<` mirrors the original loop; the index tie-break
                 * reproduces "lowest face index wins" for exact ties. */
                if (dsq < best_dsq || (dsq == best_dsq && f < best_face)) {
                    best_dsq = dsq;
                    best_face = f;
                    best_cp = cp;
                    best_n = tri_n[f];
                }
            }
        } else {
            int32_t l = node->left, r = node->right;
            float dl = tvdb_aabb_dist_sq(b->nodes[l].bmin, b->nodes[l].bmax, &p);
            float dr = tvdb_aabb_dist_sq(b->nodes[r].bmin, b->nodes[r].bmax, &p);
            /* Same inclusive test as the pop: an equidistant subtree may hold
             * a lower-indexed triangle. Push the farther child first so the
             * nearer one is examined next. */
            if (dl <= dr) {
                if (dr <= best_dsq && sp < 126) {
                    stack_node[sp] = r; stack_d[sp] = dr; sp++;
                }
                if (dl <= best_dsq && sp < 126) {
                    stack_node[sp] = l; stack_d[sp] = dl; sp++;
                }
            } else {
                if (dl <= best_dsq && sp < 126) {
                    stack_node[sp] = l; stack_d[sp] = dl; sp++;
                }
                if (dr <= best_dsq && sp < 126) {
                    stack_node[sp] = r; stack_d[sp] = dr; sp++;
                }
            }
        }
    }
    *out_cp = best_cp;
    *out_n = best_n;
    if (out_face) *out_face = best_face;
    return best_dsq;
}

#include "tinyvdb_mesh_conversion.inl"

// -------------------------------------------------------------------------
// Mesh -> SDF -> Mesh (remeshing for manifold-ness)
// -------------------------------------------------------------------------

bool tvdb_make_manifold(const tvdb_triangle_mesh* input, double resolution,
                        double isovalue, tvdb_triangle_mesh* output,
                        tvdb_arena_allocator_t* arena) {
    if (!input || !output || !isfinite(resolution) || resolution <= 0.0 ||
        resolution > FLT_MAX / 4.0 || (float)resolution <= 0 ||
        !isfinite(isovalue) || fabs(isovalue) > FLT_MAX) return false;

    tvdb_dense_grid grid;
    grid.data = NULL;
    float band = (float)(resolution * 4.0);
    /* The intermediate lattice is temporary, including for arena outputs. */
    if (!tvdb_mesh_to_sdf(input, (float)resolution, band, &grid, NULL)) return false;
    bool ok = tvdb_sdf_to_mesh_ex(&grid, (float)isovalue, TVDB_MESH_WINDING_OUTWARD, NULL, output, arena, NULL) == TVDB_OK;
    tvdb_dense_grid_free(&grid);
    return ok;
}

// -------------------------------------------------------------------------
// _vdb variants (sign_method is currently advisory; the implementation uses
// closest-triangle pseudo-normal sign regardless).
// -------------------------------------------------------------------------

bool tvdb_mesh_to_sdf_vdb(const tvdb_triangle_mesh* mesh, float voxel_size,
                          float band_width, tvdb_dense_grid* grid,
                          tvdb_sign_method sign_method,
                          tvdb_arena_allocator_t* arena) {
    (void)sign_method;
    return tvdb_mesh_to_sdf(mesh, voxel_size, band_width, grid, arena);
}

bool tvdb_make_manifold_vdb(const tvdb_triangle_mesh* input, double resolution,
                            double isovalue, tvdb_triangle_mesh* output,
                            tvdb_sign_method sign_method,
                            tvdb_arena_allocator_t* arena) {
    (void)sign_method;
    return tvdb_make_manifold(input, resolution, isovalue, output, arena);
}
