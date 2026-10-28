#include "tinyvdb_tetmesh.h"

#include <math.h>
#include <float.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_checked.h"
#include "tinyvdb_tree_internal.h"
#include "tinyvdb_tet_graded_internal.h"
#include "tinyvdb_tet_graded.h"

/* Lattice vertices use doubled integer coordinates: cube corners have all
 * coordinates even, cube centers all odd. Axis ("long") edges join vertices
 * two units apart along an axis (length h); diagonal ("short") edges join a
 * corner and a center one unit apart on every axis (length h * sqrt(3)/2).
 * Every lattice tetrahedron has one long center-center edge and one long
 * corner-corner edge; it is generated once, from its center-center edge and
 * two consecutive corners of the square ring around that edge. */

#define TET_ALPHA_LONG 0.24999
#define TET_ALPHA_SHORT 0.41189

typedef struct {
  int nx, ny, nz;      /* cells per axis */
  size_t corners;      /* (nx+1)(ny+1)(nz+1) */
  size_t vertices;     /* corners + nx*ny*nz */
  double origin[3], h;
} tet_lattice;

static tvdb_status_t tet_error(tvdb_error_t *err, tvdb_status_t st,
                               const char *msg) {
  if (err) {
    memset(err, 0, sizeof(*err));
    err->status = st;
    strncpy(err->message, msg, sizeof(err->message) - 1);
  }
  return st;
}

static void tet_coords(const tet_lattice *L, size_t id, int64_t c[3]) {
  if (id < L->corners) {
    const size_t sx = (size_t)L->nx + 1, sy = (size_t)L->ny + 1;
    c[0] = 2 * (int64_t)(id % sx);
    c[1] = 2 * (int64_t)((id / sx) % sy);
    c[2] = 2 * (int64_t)(id / (sx * sy));
    return;
  }
  id -= L->corners;
  c[0] = 2 * (int64_t)(id % (size_t)L->nx) + 1;
  c[1] = 2 * (int64_t)((id / (size_t)L->nx) % (size_t)L->ny) + 1;
  c[2] = 2 * (int64_t)(id / ((size_t)L->nx * (size_t)L->ny)) + 1;
}
static void tet_position(const tet_lattice *L, size_t id, double p[3]) {
  int64_t c[3];
  tet_coords(L, id, c);
  for (int a = 0; a < 3; ++a) p[a] = L->origin[a] + 0.5 * L->h * (double)c[a];
}

/* Per-range sampler state. The pool hands a slice of indices to whichever
 * worker gets there first, so a sampler that caches lookups (the level-set
 * tree accessor) cannot keep that cache in the shared `user` pointer: each
 * slice opens its own state and closes it when the slice is done. */
typedef struct {
  void *(*begin)(void *user);
  void (*end)(void *user, void *state);
} tet_sample_scope;
typedef struct {
  const tet_lattice *L;
  tvdb_sdf_sample_fn sample;
  void *user;
  const tet_sample_scope *scope;
  float *value;
  atomic_int bad;
  atomic_int oom;
} tet_sample_job;
static void tet_sample_range(size_t begin, size_t end, void *user) {
  tet_sample_job *job = (tet_sample_job *)user;
  void *state = job->user;
  int scoped = 0;
  if (job->scope) {
    if (!(state = job->scope->begin(job->user))) {
      atomic_store_explicit(&job->oom, 1, memory_order_relaxed);
      return;
    }
    scoped = 1;
  }
  for (size_t id = begin; id < end; ++id) {
    double p[3];
    float v = 0;
    tet_position(job->L, id, p);
    if (!job->sample(p, &v, state) || !isfinite(v))
      atomic_store_explicit(&job->bad, 1, memory_order_relaxed);
    job->value[id] = v;
  }
  if (scoped) job->scope->end(job->user, state);
}

/* Output assembly: lattice vertices and cut points get output indices on
 * first use; cut points are keyed by their (sorted) edge. */
typedef struct {
  uint64_t a, b; /* edge vertex ids, a < b */
  int32_t index;
} tet_cut_slot;
typedef struct {
  uint64_t a, b; /* lattice vertex (a == b), or sorted cut edge */
} tet_vertex_key;
typedef struct {
  const tet_lattice *L;
  const float *value;
  const double *pos; /* 3 per lattice vertex (warped) */
  float iso;
  int32_t *vertex_out; /* per lattice vertex, -1 unused */
  /* A z-slab builder stores only its corner and center map ranges. */
  int compact_vertex_out;
  size_t corner_map_base, corner_map_count, center_map_base, center_map_count;
  int collect_keys;
  tet_vertex_key *keys;
  size_t key_capacity;
  tet_cut_slot *cuts;
  size_t cut_capacity, cut_count;
  double *positions; /* 3 per output vertex */
  size_t vertex_count, vertex_capacity;
  int32_t *tets;
  size_t tet_count, tet_capacity, max_tets;
  atomic_size_t *shared_tet_count; /* optional aggregate budget for slab builders */
  double min_volume;
  tvdb_error_t *err;
  tvdb_status_t st;
} tet_builder;

typedef struct {
  uint64_t k0, k1; /* total order for the diagonal rule */
  int32_t index;
} tet_point;

static int tet_grow(void **ptr, size_t *capacity, size_t need, size_t elem) {
  if (need <= *capacity) return 1;
  size_t n = *capacity ? *capacity : 64, bytes;
  while (n < need) {
    if (n > SIZE_MAX / 2) return 0;
    n *= 2;
  }
  if (!tvdb_size_mul(n, elem, &bytes)) return 0;
  void *p = realloc(*ptr, bytes);
  if (!p) return 0;
  *ptr = p;
  *capacity = n;
  return 1;
}
static int32_t tet_add_vertex(tet_builder *B, const double p[3], uint64_t a,
                              uint64_t b) {
  if (B->vertex_count >= (size_t)INT32_MAX ||
      !tet_grow((void **)&B->positions, &B->vertex_capacity, B->vertex_count + 1,
                3 * sizeof(double)) ||
      (B->collect_keys &&
       !tet_grow((void **)&B->keys, &B->key_capacity, B->vertex_count + 1,
                 sizeof(tet_vertex_key)))) {
    B->st = tet_error(B->err, TVDB_ERROR_OUT_OF_MEMORY, "tet vertex allocation failed");
    return -1;
  }
  memcpy(B->positions + 3 * B->vertex_count, p, 3 * sizeof(double));
  if (B->collect_keys) B->keys[B->vertex_count] = (tet_vertex_key){a, b};
  return (int32_t)B->vertex_count++;
}
static int32_t *tet_vertex_map_slot(tet_builder *B, size_t id) {
  if (!B->compact_vertex_out) return B->vertex_out + id;
  if (id < B->L->corners) {
    if (id < B->corner_map_base ||
        id - B->corner_map_base >= B->corner_map_count)
      return NULL;
    return B->vertex_out + (id - B->corner_map_base);
  }
  if (id < B->center_map_base) return NULL;
  const size_t center_offset = id - B->center_map_base;
  if (center_offset >= B->center_map_count) return NULL;
  const size_t slot = B->corner_map_count + center_offset;
  return B->vertex_out + slot;
}
static tet_point tet_lattice_point(tet_builder *B, size_t id) {
  tet_point q = {id, id, -1};
  int32_t *slot = tet_vertex_map_slot(B, id);
  if (!slot) {
    B->st = tet_error(B->err, TVDB_ERROR_INVALID_DATA,
                      "tet slab lattice index out of range");
    return q;
  }
  q.index = *slot;
  if (q.index < 0) {
    q.index = tet_add_vertex(B, B->pos + 3 * id, (uint64_t)id, (uint64_t)id);
    *slot = q.index;
  }
  return q;
}
static uint64_t tet_hash2(uint64_t a, uint64_t b) {
  uint64_t h = a * UINT64_C(0x9E3779B97F4A7C15) ^ (b + UINT64_C(0x632BE59BD9B4E019));
  h ^= h >> 31;
  h *= UINT64_C(0xff51afd7ed558ccd);
  return h ^ (h >> 33);
}
static int tet_cut_rehash(tet_builder *B) {
  size_t n = B->cut_capacity ? 2 * B->cut_capacity : 1024, bytes;
  if (!tvdb_size_mul(n, sizeof(tet_cut_slot), &bytes)) return 0;
  tet_cut_slot *slots = (tet_cut_slot *)malloc(bytes);
  if (!slots) return 0;
  for (size_t i = 0; i < n; ++i) slots[i].index = -1;
  for (size_t i = 0; i < B->cut_capacity; ++i) {
    if (B->cuts[i].index < 0) continue;
    size_t s = (size_t)tet_hash2(B->cuts[i].a, B->cuts[i].b) & (n - 1);
    while (slots[s].index >= 0) s = (s + 1) & (n - 1);
    slots[s] = B->cuts[i];
  }
  free(B->cuts);
  B->cuts = slots;
  B->cut_capacity = n;
  return 1;
}
/* The cut point on lattice edge (u, w), whose endpoints straddle iso. */
static tet_point tet_cut_point(tet_builder *B, size_t u, size_t w) {
  const uint64_t a = u < w ? u : w, b = u < w ? w : u;
  tet_point q = {a, b, -1};
  if (B->cut_count + 1 > B->cut_capacity / 2 && !tet_cut_rehash(B)) {
    B->st = tet_error(B->err, TVDB_ERROR_OUT_OF_MEMORY, "tet cut allocation failed");
    return q;
  }
  size_t s = (size_t)tet_hash2(a, b) & (B->cut_capacity - 1);
  while (B->cuts[s].index >= 0) {
    if (B->cuts[s].a == a && B->cuts[s].b == b) {
      q.index = B->cuts[s].index;
      return q;
    }
    s = (s + 1) & (B->cut_capacity - 1);
  }
  const double fa = (double)B->value[a] - B->iso, fb = (double)B->value[b] - B->iso;
  const double t = fa / (fa - fb);
  double p[3];
  for (int k = 0; k < 3; ++k)
    p[k] = B->pos[3 * a + k] + t * (B->pos[3 * b + k] - B->pos[3 * a + k]);
  q.index = tet_add_vertex(B, p, a, b);
  if (q.index < 0) return q;
  B->cuts[s].a = a;
  B->cuts[s].b = b;
  B->cuts[s].index = q.index;
  ++B->cut_count;
  return q;
}
static int tet_less(const tet_point *x, const tet_point *y) {
  return x->k0 < y->k0 || (x->k0 == y->k0 && x->k1 < y->k1);
}
static double tet_volume6(const double *p0, const double *p1, const double *p2,
                          const double *p3) {
  const double a[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
  const double b[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
  const double c[3] = {p3[0] - p0[0], p3[1] - p0[1], p3[2] - p0[2]};
  return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
         a[2] * (b[0] * c[1] - b[1] * c[0]);
}
/* The public mesh stores floats. A valid double-precision tet can overflow,
 * collapse, or invert when converted, especially at large world offsets. */
static tvdb_status_t tet_validate_float_output(const tet_builder *B, tvdb_tet_mesh *out,
                                                tvdb_error_t *err) {
  for (size_t i = 0; i < 3 * B->vertex_count; ++i) {
    double x = B->positions[i];
    if (!isfinite(x) || fabs(x) > FLT_MAX)
      return tet_error(err, TVDB_ERROR_INVALID_DATA, "tet vertex exceeds float range");
    out->positions[i] = (float)x;
  }
  for (size_t i = 0; i < B->tet_count; ++i) {
    double p[4][3];
    for (size_t j = 0; j < 4; ++j) {
      size_t vertex = (size_t)B->tets[4*i+j];
      for (size_t k = 0; k < 3; ++k) p[j][k] = out->positions[3*vertex+k];
    }
    double volume = tet_volume6(p[0], p[1], p[2], p[3]);
    if (!isfinite(volume) || !(volume > 0))
      return tet_error(err, TVDB_ERROR_INVALID_DATA, "tet collapses or inverts at float precision");
  }
  return TVDB_OK;
}
static void tet_emit(tet_builder *B, tet_point p0, tet_point p1, tet_point p2,
                     tet_point p3) {
  if (B->st != TVDB_OK || p0.index < 0 || p1.index < 0 || p2.index < 0 ||
      p3.index < 0)
    return;
  int32_t v[4] = {p0.index, p1.index, p2.index, p3.index};
  const double vol = tet_volume6(B->positions + 3 * (size_t)v[0],
                                 B->positions + 3 * (size_t)v[1],
                                 B->positions + 3 * (size_t)v[2],
                                 B->positions + 3 * (size_t)v[3]);
  if (!(fabs(vol) > B->min_volume)) return; /* degenerate sliver */
  if (vol < 0) {
    const int32_t t = v[2];
    v[2] = v[3];
    v[3] = t;
  }
  if (B->shared_tet_count) {
    size_t total = atomic_load_explicit(B->shared_tet_count, memory_order_relaxed);
    for (;;) {
      if (total >= B->max_tets) {
        B->st = tet_error(B->err, TVDB_ERROR_INVALID_ARGUMENT, "tet budget exceeded");
        return;
      }
      if (atomic_compare_exchange_weak_explicit(B->shared_tet_count, &total, total + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed))
        break;
    }
  } else if (B->tet_count >= B->max_tets) {
    B->st = tet_error(B->err, TVDB_ERROR_INVALID_ARGUMENT, "tet budget exceeded");
    return;
  }
  if (!tet_grow((void **)&B->tets, &B->tet_capacity, B->tet_count + 1,
                4 * sizeof(int32_t))) {
    B->st = tet_error(B->err, TVDB_ERROR_OUT_OF_MEMORY, "tet allocation failed");
    return;
  }
  memcpy(B->tets + 4 * B->tet_count++, v, sizeof(v));
}
/* Prism P0 P1 P2 (bottom), P3 P4 P5 (top), vertical edges Pi-P(i+3). */
static void tet_prism(tet_builder *B, const tet_point in[6]) {
  int m = 0;
  for (int i = 1; i < 6; ++i)
    if (tet_less(in + i, in + m)) m = i;
  tet_point s[6], P[6];
  for (int i = 0; i < 6; ++i) s[i] = in[m >= 3 ? (i + 3) % 6 : i];
  m %= 3;
  for (int i = 0; i < 3; ++i) {
    P[i] = s[(i + m) % 3];
    P[i + 3] = s[(i + m) % 3 + 3];
  }
  /* Quadrilateral P1 P2 P5 P4: diagonal through its minimum vertex. */
  const tet_point *a = tet_less(P + 1, P + 5) ? P + 1 : P + 5;
  const tet_point *b = tet_less(P + 2, P + 4) ? P + 2 : P + 4;
  if (tet_less(a, b)) {
    tet_emit(B, P[0], P[1], P[2], P[5]);
    tet_emit(B, P[0], P[1], P[5], P[4]);
    tet_emit(B, P[0], P[4], P[5], P[3]);
  } else {
    tet_emit(B, P[0], P[1], P[2], P[4]);
    tet_emit(B, P[0], P[4], P[2], P[5]);
    tet_emit(B, P[0], P[4], P[5], P[3]);
  }
}
/* Pyramid with apex A and base q0 q1 q2 q3 in cyclic order. */
static void tet_pyramid(tet_builder *B, tet_point A, const tet_point q[4]) {
  int m = 0;
  for (int i = 1; i < 4; ++i)
    if (tet_less(q + i, q + m)) m = i;
  if (m % 2 == 0) {
    tet_emit(B, A, q[0], q[1], q[2]);
    tet_emit(B, A, q[0], q[2], q[3]);
  } else {
    tet_emit(B, A, q[1], q[2], q[3]);
    tet_emit(B, A, q[1], q[3], q[0]);
  }
}
static void tet_stencil(tet_builder *B, const size_t id[4],
                        tvdb_sdf_sample_fn sample, void *user) {
  size_t M[4], Z[4], P[4];
  int nm = 0, nz = 0, np = 0;
  for (int i = 0; i < 4; ++i) {
    const float f = B->value[id[i]] - B->iso;
    if (f < 0) M[nm++] = id[i];
    else if (f > 0) P[np++] = id[i];
    else Z[nz++] = id[i];
  }
  if (nm == 0) {
    if (nz < 4) return;
    /* Four surface vertices: keep the tetrahedron if its center is inside. */
    double c[3] = {0, 0, 0};
    for (int i = 0; i < 4; ++i)
      for (int k = 0; k < 3; ++k) c[k] += 0.25 * B->pos[3 * id[i] + k];
    float f = 0;
    if (!sample(c, &f, user) || !isfinite(f)) {
      B->st = tet_error(B->err, TVDB_ERROR_INVALID_DATA, "stuffing sample failed");
      return;
    }
    if (!(f < B->iso)) return;
  }
  if (np == 0) {
    tet_emit(B, tet_lattice_point(B, id[0]), tet_lattice_point(B, id[1]),
             tet_lattice_point(B, id[2]), tet_lattice_point(B, id[3]));
    return;
  }
  if (nm == 1) {
    const tet_point m = tet_lattice_point(B, M[0]);
    if (np == 3)
      tet_emit(B, m, tet_cut_point(B, M[0], P[0]), tet_cut_point(B, M[0], P[1]),
               tet_cut_point(B, M[0], P[2]));
    else if (np == 2)
      tet_emit(B, m, tet_lattice_point(B, Z[0]), tet_cut_point(B, M[0], P[0]),
               tet_cut_point(B, M[0], P[1]));
    else
      tet_emit(B, m, tet_lattice_point(B, Z[0]), tet_lattice_point(B, Z[1]),
               tet_cut_point(B, M[0], P[0]));
    return;
  }
  if (nm == 2 && np == 2) {
    const tet_point prism[6] = {
        tet_lattice_point(B, M[0]), tet_cut_point(B, M[0], P[0]),
        tet_cut_point(B, M[0], P[1]), tet_lattice_point(B, M[1]),
        tet_cut_point(B, M[1], P[0]), tet_cut_point(B, M[1], P[1])};
    tet_prism(B, prism);
    return;
  }
  if (nm == 2) { /* np == 1, nz == 1 */
    const tet_point base[4] = {tet_lattice_point(B, M[0]),
                               tet_cut_point(B, M[0], P[0]),
                               tet_cut_point(B, M[1], P[0]),
                               tet_lattice_point(B, M[1])};
    tet_pyramid(B, tet_lattice_point(B, Z[0]), base);
    return;
  }
  /* nm == 3, np == 1 */
  const tet_point prism[6] = {
      tet_lattice_point(B, M[0]), tet_lattice_point(B, M[1]),
      tet_lattice_point(B, M[2]), tet_cut_point(B, M[0], P[0]),
      tet_cut_point(B, M[1], P[0]), tet_cut_point(B, M[2], P[0])};
  tet_prism(B, prism);
}

void tvdb_tet_mesh_free(tvdb_tet_mesh *mesh) {
  if (!mesh) return;
  free(mesh->positions);
  free(mesh->tets);
  free(mesh->boundary);
  memset(mesh, 0, sizeof(*mesh));
}

/* Boundary faces: those used by exactly one tetrahedron, oriented as that
 * tetrahedron sees them. Faces are bucketed by their smallest vertex (output
 * vertices are numbered in first-use order, so buckets are local), each
 * bucket is sorted by its other two vertices and singletons are kept. The
 * result is in sorted-triple order. Linear memory: 16 bytes per face.
 *
 * The counting sort runs on the pool in four passes:
 *   1. every slice counts its own tets into a private per-bucket table;
 *   2. a serial prefix sum turns the per-bucket totals into bucket starts and
 *      each slice's table into its write cursor (slice order inside a bucket
 *      is tet order, so every face lands where the serial scatter put it);
 *   3. the slices scatter their faces;
 *   4. bucket ranges are sorted and their singletons emitted in parallel, with
 *      the per-bucket output offsets coming from one serial prefix sum.
 * The output is therefore identical for any worker count. */
typedef struct {
  int32_t s1, s2;   /* the other two vertices, ascending */
  uint64_t face;    /* 4 * tet + local face */
} tet_face_entry;
static int tet_face_entry_less(const tet_face_entry *a, const tet_face_entry *b) {
  return a->s1 < b->s1 || (a->s1 == b->s1 && a->s2 < b->s2);
}
static int tet_face_entry_compare(const void *pa, const void *pb) {
  const tet_face_entry *a = (const tet_face_entry *)pa, *b = (const tet_face_entry *)pb;
  return tet_face_entry_less(a, b) ? -1 : tet_face_entry_less(b, a);
}
static const int tet_faces[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
static void tet_sorted_face(const int32_t *tet, int f, int32_t s[3]) {
  for (int k = 0; k < 3; ++k) s[k] = tet[tet_faces[f][k]];
  for (int i = 0; i < 2; ++i)
    for (int j = i + 1; j < 3; ++j)
      if (s[j] < s[i]) {
        const int32_t x = s[i];
        s[i] = s[j];
        s[j] = x;
      }
}
/* Single-slice fallback: the fused sort-and-emit loop, which never needs
 * the per-bucket singleton counts that the parallel emit pass does. */
static tvdb_status_t tet_boundary_serial(const int32_t *tets, size_t tet_count,
                                         size_t vertex_count, int32_t **out,
                                         size_t *out_count, tvdb_error_t *err) {
  size_t faces, bytes, start_bytes;
  if (!tvdb_size_mul(tet_count, 4, &faces) ||
      !tvdb_size_mul(faces, sizeof(tet_face_entry), &bytes) ||
      vertex_count == SIZE_MAX || !tvdb_size_mul(vertex_count + 1, sizeof(size_t), &start_bytes))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet face overflow");
  size_t *start = (size_t *)calloc(1, start_bytes);
  tet_face_entry *entry = (tet_face_entry *)malloc(bytes ? bytes : 1);
  int32_t *list = NULL;
  if (!start || !entry) {
    free(start);
    free(entry);
    return tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet face allocation failed");
  }
  for (size_t t = 0; t < tet_count; ++t)
    for (int f = 0; f < 4; ++f) {
      int32_t s[3];
      tet_sorted_face(tets + 4 * t, f, s);
      ++start[(size_t)s[0] + 1];
    }
  for (size_t v = 0; v < vertex_count; ++v) start[v + 1] += start[v];
  /* Scatter with start[v] as the cursor; afterwards start[v] is bucket v's
   * end, i.e. bucket v spans [v ? start[v - 1] : 0, start[v]). */
  for (size_t t = 0; t < tet_count; ++t)
    for (int f = 0; f < 4; ++f) {
      int32_t s[3];
      tet_sorted_face(tets + 4 * t, f, s);
      tet_face_entry *e = entry + start[s[0]]++;
      e->s1 = s[1];
      e->s2 = s[2];
      e->face = 4 * (uint64_t)t + (uint64_t)f;
    }
  size_t count = 0, capacity = 0;
  for (size_t v = 0, begin = 0; v < vertex_count; begin = start[v++]) {
    tet_face_entry *b = entry + begin;
    const size_t k = start[v] - begin;
    if (k > 24) {
      qsort(b, k, sizeof(*b), tet_face_entry_compare);
    } else {
      for (size_t i = 1; i < k; ++i)
        for (size_t j = i; j > 0 && tet_face_entry_less(b + j, b + j - 1); --j) {
          const tet_face_entry x = b[j];
          b[j] = b[j - 1];
          b[j - 1] = x;
        }
    }
    for (size_t i = 0; i < k;) {
      size_t j = i + 1;
      while (j < k && b[j].s1 == b[i].s1 && b[j].s2 == b[i].s2) ++j;
      if (j == i + 1) {
        if (count == capacity) {
          const size_t n = capacity ? 2 * capacity : 1024;
          size_t lb;
          int32_t *p = tvdb_size_mul(n, 3 * sizeof(int32_t), &lb) ? (int32_t *)realloc(list, lb) : NULL;
          if (!p) {
            free(start);
            free(entry);
            free(list);
            return tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet boundary allocation failed");
          }
          list = p;
          capacity = n;
        }
        const int32_t *tet = tets + 4 * (size_t)(b[i].face / 4);
        const int f = (int)(b[i].face % 4);
        for (int c = 0; c < 3; ++c) list[3 * count + c] = tet[tet_faces[f][c]];
        ++count;
      }
      i = j;
    }
  }
  free(start);
  free(entry);
  *out = list;
  *out_count = count;
  return TVDB_OK;
}

typedef struct {
  const int32_t *tets;
  tet_face_entry *entry;
  uint32_t *cursors; /* slices * vertex_count */
  size_t tet_count, vertex_count, slice_tets;
} tet_boundary_job;
typedef struct {
  const int32_t *tets;
  tet_face_entry *entry;
  const size_t *start;
  size_t *counts; /* singletons per bucket, then their output offsets */
  int32_t *list;
} tet_boundary_bucket_job;
static void tet_boundary_count_range(size_t begin, size_t end, void *user) {
  const tet_boundary_job *job = (const tet_boundary_job *)user;
  for (size_t c = begin; c < end; ++c) {
    uint32_t *cursors = job->cursors + c * job->vertex_count;
    const size_t t0 = c * job->slice_tets;
    size_t t1 = t0 + job->slice_tets;
    if (t1 > job->tet_count) t1 = job->tet_count;
    for (size_t t = t0; t < t1; ++t)
      for (int f = 0; f < 4; ++f) {
        int32_t s[3];
        tet_sorted_face(job->tets + 4 * t, f, s);
        ++cursors[(size_t)s[0]];
      }
  }
}
static void tet_boundary_scatter_range(size_t begin, size_t end, void *user) {
  const tet_boundary_job *job = (const tet_boundary_job *)user;
  for (size_t c = begin; c < end; ++c) {
    uint32_t *cursors = job->cursors + c * job->vertex_count;
    const size_t t0 = c * job->slice_tets;
    size_t t1 = t0 + job->slice_tets;
    if (t1 > job->tet_count) t1 = job->tet_count;
    for (size_t t = t0; t < t1; ++t)
      for (int f = 0; f < 4; ++f) {
        int32_t s[3];
        tet_sorted_face(job->tets + 4 * t, f, s);
        tet_face_entry *e = job->entry + cursors[(size_t)s[0]]++;
        e->s1 = s[1];
        e->s2 = s[2];
        e->face = 4 * (uint64_t)t + (uint64_t)f;
      }
  }
}
/* Sort one bucket and record how many singleton faces it holds. */
static void tet_boundary_bucket_sort(tet_face_entry *b, size_t k) {
  if (k > 24) {
    qsort(b, k, sizeof(*b), tet_face_entry_compare);
  } else {
    for (size_t i = 1; i < k; ++i)
      for (size_t j = i; j > 0 && tet_face_entry_less(b + j, b + j - 1); --j) {
        const tet_face_entry x = b[j];
        b[j] = b[j - 1];
        b[j - 1] = x;
      }
  }
}
static void tet_boundary_tally_range(size_t begin, size_t end, void *user) {
  const tet_boundary_bucket_job *job = (const tet_boundary_bucket_job *)user;
  for (size_t v = begin; v < end; ++v) {
    tet_face_entry *b = job->entry + job->start[v];
    const size_t k = job->start[v + 1] - job->start[v];
    tet_boundary_bucket_sort(b, k);
    size_t singles = 0;
    for (size_t i = 0; i < k;) {
      size_t j = i + 1;
      while (j < k && b[j].s1 == b[i].s1 && b[j].s2 == b[i].s2) ++j;
      if (j == i + 1) ++singles;
      i = j;
    }
    job->counts[v] = singles;
  }
}
static void tet_boundary_emit_range(size_t begin, size_t end, void *user) {
  const tet_boundary_bucket_job *job = (const tet_boundary_bucket_job *)user;
  for (size_t v = begin; v < end; ++v) {
    const tet_face_entry *b = job->entry + job->start[v];
    const size_t k = job->start[v + 1] - job->start[v];
    size_t out = job->counts[v];
    for (size_t i = 0; i < k;) {
      size_t j = i + 1;
      while (j < k && b[j].s1 == b[i].s1 && b[j].s2 == b[i].s2) ++j;
      if (j == i + 1) {
        const int32_t *tet = job->tets + 4 * (size_t)(b[i].face / 4);
        const int f = (int)(b[i].face % 4);
        for (int c = 0; c < 3; ++c) job->list[3 * out + c] = tet[tet_faces[f][c]];
        ++out;
      }
      i = j;
    }
    job->counts[v] = out;
  }
}
static tvdb_status_t tet_boundary(const int32_t *tets, size_t tet_count, size_t vertex_count,
                                  tvdb_thread_pool_t *pool, int32_t **out,
                                  size_t *out_count, tvdb_error_t *err) {
  size_t faces, bytes, start_bytes, cursor_cells, cursors_bytes;
  if (!tvdb_size_mul(tet_count, 4, &faces) ||
      !tvdb_size_mul(faces, sizeof(tet_face_entry), &bytes) ||
      vertex_count == SIZE_MAX || !tvdb_size_mul(vertex_count + 1, sizeof(size_t), &start_bytes))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet face overflow");
  /* Faces land in per-bucket uint32 cursors, so the total must fit one. */
  if (faces > UINT32_MAX)
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet face overflow");
  size_t slices = pool ? tvdb_thread_pool_size(pool) : 1;
  if (slices > 8) slices = 8; /* one cursor table per slice: 4 bytes a vertex */
  if (slices < 1) slices = 1;
  if (tet_count < 4096) slices = 1;
  const size_t slice_tets = slices > 1 ? (tet_count + slices - 1) / slices : tet_count;
  const size_t used = slice_tets ? (tet_count + slice_tets - 1) / slice_tets : 1;
  if (used == 1) return tet_boundary_serial(tets, tet_count, vertex_count, out, out_count, err);
  if (!tvdb_size_mul(used, vertex_count, &cursor_cells) ||
      !tvdb_size_mul(cursor_cells, sizeof(uint32_t), &cursors_bytes))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet face overflow");
  size_t *start = (size_t *)calloc(1, start_bytes);
  uint32_t *cursors = (uint32_t *)calloc(1, cursors_bytes ? cursors_bytes : 1);
  tet_face_entry *entry = (tet_face_entry *)malloc(bytes ? bytes : 1);
  size_t *singles = (size_t *)calloc(1, start_bytes);
  int32_t *list = NULL;
  tvdb_status_t st = TVDB_OK;
  if (!start || !cursors || !entry || !singles) {
    st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet face allocation failed");
    goto done;
  }
  tet_boundary_job job = {.tets = tets,
                          .entry = entry,
                          .cursors = cursors,
                          .tet_count = tet_count,
                          .vertex_count = vertex_count,
                          .slice_tets = slice_tets};
  st = tvdb_thread_pool_for(pool, 0, used, 1, tet_boundary_count_range, &job, err);
  if (st != TVDB_OK) goto done;
  /* Bucket starts, then each slice's cursor: slice c writes at the bucket
   * start plus the faces earlier slices claimed there. */
  for (size_t v = 0; v < vertex_count; ++v) {
    size_t total = 0;
    for (size_t c = 0; c < used; ++c) total += cursors[c * vertex_count + v];
    start[v + 1] = start[v] + total;
  }
  for (size_t v = 0; v < vertex_count; ++v) {
    size_t at = start[v];
    for (size_t c = 0; c < used; ++c) {
      const size_t n = cursors[c * vertex_count + v];
      cursors[c * vertex_count + v] = (uint32_t)at;
      at += n;
    }
  }
  st = tvdb_thread_pool_for(pool, 0, used, 1, tet_boundary_scatter_range, &job, err);
  if (st != TVDB_OK) goto done;
  /* Sort each bucket once (the scatter left every slice's faces in bucket
   * order), then count its singletons. */
  tet_boundary_bucket_job buckets = {
      .tets = tets, .entry = entry, .start = start, .counts = singles, .list = NULL};
  {
    const size_t vslice = (vertex_count + used - 1) / used;
    st = tvdb_thread_pool_for(pool, 0, vertex_count, vslice, tet_boundary_tally_range,
                              &buckets, err);
    if (st != TVDB_OK) goto done;
  }
  size_t boundary = 0;
  for (size_t v = 0; v < vertex_count; ++v) {
    const size_t n = singles[v];
    singles[v] = boundary;
    boundary += n;
  }
  if (boundary) {
    size_t list_bytes;
    if (!tvdb_size_mul(boundary, 3 * sizeof(int32_t), &list_bytes)) {
      st = tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet face overflow");
      goto done;
    }
    list = (int32_t *)malloc(list_bytes);
    if (!list) {
      st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet boundary allocation failed");
      goto done;
    }
  }
  buckets.list = list;
  {
    const size_t vslice = (vertex_count + used - 1) / used;
    st = tvdb_thread_pool_for(pool, 0, vertex_count, vslice, tet_boundary_emit_range,
                              &buckets, err);
    if (st != TVDB_OK) goto done;
  }
  *out = list;
  *out_count = boundary;
  list = NULL;
done:
  free(start);
  free(cursors);
  free(entry);
  free(singles);
  free(list);
  return st;
}

typedef struct {
  tet_builder builder;
  tvdb_status_t status;
  tvdb_error_t err;
} tet_stuff_part;
typedef struct {
  const tet_lattice *L;
  const float *value;
  const double *pos;
  float iso;
  size_t max_tets, part_count;
  double min_volume;
  tvdb_sdf_sample_fn sample;
  void *user;
  const tet_sample_scope *scope;
  atomic_size_t total_tets;
  tet_stuff_part *parts;
} tet_stuff_job;

static void tet_stuff_cells(tet_builder *B, int k_begin, int k_end,
                            tvdb_sdf_sample_fn sample, void *user) {
  const tet_lattice *L = B->L;
  const size_t corner_stride[3] = {1, (size_t)L->nx + 1,
                                  ((size_t)L->nx + 1) * ((size_t)L->ny + 1)};
  const size_t center_stride[3] = {1, (size_t)L->nx, (size_t)L->nx * (size_t)L->ny};
  const int cell_dims[3] = {L->nx, L->ny, L->nz};
  for (int k = k_begin; k < k_end && B->st == TVDB_OK; ++k)
    for (int j = 0; j < L->ny && B->st == TVDB_OK; ++j)
      for (int i = 0; i < L->nx && B->st == TVDB_OK; ++i) {
        const int c[3] = {i, j, k};
        const size_t center = L->corners + (size_t)i + center_stride[1] * (size_t)j +
                              center_stride[2] * (size_t)k;
        const size_t corner_base = (size_t)i + corner_stride[1] * (size_t)j +
                                   corner_stride[2] * (size_t)k;
        for (int a = 0; a < 3 && B->st == TVDB_OK; ++a) {
          if (c[a] + 1 >= cell_dims[a]) continue;
          const size_t other = center + center_stride[a];
          const int b = (a + 1) % 3, cc = (a + 2) % 3;
          static const int ring[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
          size_t r[4];
          for (int q = 0; q < 4; ++q) {
            r[q] = corner_base + corner_stride[a] +
                   (ring[q][0] > 0 ? corner_stride[b] : 0) +
                   (ring[q][1] > 0 ? corner_stride[cc] : 0);
          }
          /* Most of the lattice is outside: skip cells whose six vertices
           * are all positive, since none of their tetrahedra emit. */
          if (B->value[center] - B->iso > 0 && B->value[other] - B->iso > 0 &&
              B->value[r[0]] - B->iso > 0 && B->value[r[1]] - B->iso > 0 &&
              B->value[r[2]] - B->iso > 0 && B->value[r[3]] - B->iso > 0)
            continue;
          for (int q = 0; q < 4 && B->st == TVDB_OK; ++q) {
            const size_t ids[4] = {center, other, r[q], r[(q + 1) % 4]};
            tet_stencil(B, ids, sample, user);
          }
        }
      }
}

typedef struct {
  const tet_lattice *L;
  float *value;
  double *pos;
  float iso;
  double alpha_long, alpha_short;
  int center;
  int ext[3];
  size_t stride[3];
  size_t base;
  int rank, axis, other, derived;
} tet_warp_job;

static void tet_warp_job_init(tet_warp_job *job, const tet_lattice *L,
                              float *value, double *pos, float iso,
                              double alpha_long, double alpha_short,
                              int center) {
  memset(job, 0, sizeof(*job));
  job->L = L;
  job->value = value;
  job->pos = pos;
  job->iso = iso;
  job->alpha_long = alpha_long;
  job->alpha_short = alpha_short;
  job->center = center;
  job->ext[0] = center ? L->nx : L->nx + 1;
  job->ext[1] = center ? L->ny : L->ny + 1;
  job->ext[2] = center ? L->nz : L->nz + 1;
  if (center) {
    job->base = L->corners;
    job->stride[0] = 1;
    job->stride[1] = (size_t)L->nx;
    job->stride[2] = (size_t)L->nx * (size_t)L->ny;
  } else {
    job->stride[0] = 1;
    job->stride[1] = (size_t)L->nx + 1;
    job->stride[2] = ((size_t)L->nx + 1) * ((size_t)L->ny + 1);
  }
}

static void tet_warp_vertex(tet_warp_job *job, size_t v, const int at[3]) {
  const tet_lattice *L = job->L;
  const int cell_dims[3] = {L->nx, L->ny, L->nz};
  const double fv = (double)job->value[v] - job->iso;
  const double len_long = L->h, len_short = L->h * 0.86602540378443864676;
  /* A sign change across an edge needs the two values to straddle the
   * isovalue, and a 1-Lipschitz sampler changes by at most sqrt(3) spacings
   * over the longest edge, so a farther vertex cannot warp. */
  if (fabs(fv) > 1.7320508075688772 * len_long) return;
  double best = INFINITY, best_t = 0;
  size_t best_w = SIZE_MAX;
  for (int e = 0; e < 14; ++e) {
    size_t w;
    double alpha, len;
    if (e < 6) {
      /* Same-kind neighbor one cell along axis e / 2. */
      const int axis = e / 2, step = (e & 1) ? -1 : 1, q = at[axis] + step;
      if (q < 0 || q >= job->ext[axis]) continue;
      int c[3] = {at[0], at[1], at[2]};
      c[axis] = q;
      w = job->base + (size_t)c[0] * job->stride[0] +
          (size_t)c[1] * job->stride[1] + (size_t)c[2] * job->stride[2];
      alpha = job->alpha_long;
      len = len_long;
    } else {
      /* Other-kind neighbor half a cell along every axis. */
      const int sd = e - 6;
      int c[3], ok = 1;
      for (int a2 = 0; a2 < 3; ++a2) {
        const int plus = (sd >> a2) & 1;
        /* corner i -> centers i-1 / i; center i -> corners i / i+1 */
        c[a2] = job->center ? at[a2] + plus : at[a2] - 1 + plus;
        const int neighbor_ext = cell_dims[a2] + job->center;
        if (c[a2] < 0 || c[a2] >= neighbor_ext) ok = 0;
      }
      if (!ok) continue;
      w = job->center ? (size_t)c[0] + ((size_t)L->nx + 1) * (size_t)c[1] +
                            ((size_t)L->nx + 1) * ((size_t)L->ny + 1) * (size_t)c[2]
                      : L->corners + (size_t)c[0] + (size_t)L->nx * (size_t)c[1] +
                            (size_t)L->nx * (size_t)L->ny * (size_t)c[2];
      alpha = job->alpha_short;
      len = len_short;
    }
    const double fw = (double)job->value[w] - job->iso;
    if (!((fv < 0 && fw > 0) || (fv > 0 && fw < 0))) continue;
    const double t = fv / (fv - fw);
    if (t < alpha && t * len < best) {
      best = t * len;
      best_t = t;
      best_w = w;
    }
  }
  if (best_w == SIZE_MAX) return;
  double pw[3];
  tet_position(L, best_w, pw);
  for (int c = 0; c < 3; ++c)
    job->pos[3 * v + c] += best_t * (pw[c] - job->pos[3 * v + c]);
  job->value[v] = job->iso;
}

static void tet_warp_range(size_t begin, size_t end, void *user) {
  tet_warp_job *job = (tet_warp_job *)user;
  int at[3] = {0, 0, 0};
  for (size_t q = begin; q < end; ++q) {
    at[job->axis] = (int)q;
    const int r = job->rank - (int)q;
    int lo = r - (job->ext[job->derived] - 1);
    int hi = r;
    if (lo < 0) lo = 0;
    if (hi >= job->ext[job->other]) hi = job->ext[job->other] - 1;
    for (int b = lo; b <= hi; ++b) {
      at[job->other] = b;
      at[job->derived] = r - b;
      const size_t v = job->base + (size_t)at[0] * job->stride[0] +
                       (size_t)at[1] * job->stride[1] +
                       (size_t)at[2] * job->stride[2];
      tet_warp_vertex(job, v, at);
    }
  }
}

static tvdb_status_t tet_warp_parallel(const tet_lattice *L, float *value,
                                       double *pos, float iso,
                                       double alpha_long, double alpha_short,
                                       tvdb_thread_pool_t *pool,
                                       tvdb_error_t *err) {
  for (int center = 0; center < 2; ++center) {
    tet_warp_job job;
    tet_warp_job_init(&job, L, value, pos, iso, alpha_long, alpha_short, center);
    job.axis = 0;
    for (int a = 1; a < 3; ++a)
      if (job.ext[a] > job.ext[job.axis]) job.axis = a;
    job.other = (job.axis + 1) % 3;
    job.derived = (job.axis + 2) % 3;
    const int max_rank = job.ext[0] + job.ext[1] + job.ext[2] - 3;
    /* Same-kind axis neighbors differ in coordinate sum by one. Therefore a
     * wave has no same-kind edges, and every earlier neighbor has completed
     * before the next wave starts. The two phases preserve the original
     * corners-before-centers dependency: corners see unmodified centers, then
     * centers see the completed corner phase. */
    for (job.rank = 0; job.rank <= max_rank; ++job.rank) {
      const tvdb_status_t st = tvdb_thread_pool_for(
          pool, 0, (size_t)job.ext[job.axis], 1, tet_warp_range, &job, err);
      if (st != TVDB_OK) return st;
    }
  }
  return TVDB_OK;
}

static void tet_stuff_part_range(size_t begin, size_t end, void *user) {
  tet_stuff_job *job = (tet_stuff_job *)user;
  const tet_lattice *L = job->L;
  for (size_t index = begin; index < end; ++index) {
    tet_stuff_part *part = &job->parts[index];
    tet_builder *B = &part->builder;
    const int k0 = (int)((size_t)L->nz * index / job->part_count);
    const int k1 = (int)((size_t)L->nz * (index + 1) / job->part_count);
    const int center_k1 = k1 < L->nz ? k1 + 1 : k1;
    size_t corner_plane, center_plane, corner_count, center_count, map_count, map_bytes;
    if (!tvdb_size_mul((size_t)L->nx + 1, (size_t)L->ny + 1, &corner_plane) ||
        !tvdb_size_mul((size_t)L->nx, (size_t)L->ny, &center_plane) ||
        !tvdb_size_mul((size_t)(k1 - k0) + 1, corner_plane, &corner_count) ||
        !tvdb_size_mul((size_t)(center_k1 - k0), center_plane, &center_count) ||
        corner_count > SIZE_MAX - center_count) {
      part->status = tet_error(&part->err, TVDB_ERROR_INVALID_ARGUMENT,
                               "tet slab map size overflow");
      return;
    }
    map_count = corner_count + center_count;
    if (!tvdb_size_mul(map_count, sizeof(int32_t), &map_bytes)) {
      part->status = tet_error(&part->err, TVDB_ERROR_INVALID_ARGUMENT,
                               "tet slab map size overflow");
      return;
    }
    B->L = L;
    B->value = job->value;
    B->pos = job->pos;
    B->iso = job->iso;
    B->vertex_out = (int32_t *)malloc(map_bytes ? map_bytes : 1);
    B->compact_vertex_out = 1;
    B->corner_map_base = (size_t)k0 * corner_plane;
    B->corner_map_count = corner_count;
    B->center_map_base = L->corners + (size_t)k0 * center_plane;
    B->center_map_count = center_count;
    B->collect_keys = 1;
    B->max_tets = job->max_tets;
    B->shared_tet_count = &job->total_tets;
    B->min_volume = job->min_volume;
    B->err = &part->err;
    B->st = TVDB_OK;
    if (!B->vertex_out) {
      part->status = tet_error(&part->err, TVDB_ERROR_OUT_OF_MEMORY,
                               "tet slab map allocation failed");
      return;
    }
    for (size_t i = 0; i < map_count; ++i) B->vertex_out[i] = -1;
    void *stencil_user = job->user;
    int scoped = 0;
    if (job->scope) {
      if (!(stencil_user = job->scope->begin(job->user))) {
        B->st = tet_error(B->err, TVDB_ERROR_OUT_OF_MEMORY,
                          "stuffing sampler allocation failed");
      } else {
        scoped = 1;
      }
    }
    if (B->st == TVDB_OK)
      tet_stuff_cells(B, k0, k1, job->sample, stencil_user);
    if (scoped) job->scope->end(job->user, stencil_user);
    part->status = B->st;
  }
}

static void tet_stuff_parts_free(tet_stuff_part *parts, size_t count) {
  if (!parts) return;
  for (size_t i = 0; i < count; ++i) {
    tet_builder *B = &parts[i].builder;
    free(B->vertex_out);
    free(B->cuts);
    free(B->positions);
    free(B->keys);
    free(B->tets);
  }
  free(parts);
}

static tvdb_status_t tet_stuff_parts_merge(tet_builder *B,
                                           tet_stuff_part *parts,
                                           size_t count,
                                           tvdb_error_t *err) {
  for (size_t p = 0; p < count; ++p) {
    tet_builder *src = &parts[p].builder;
    size_t remap_bytes;
    if (!tvdb_size_mul(src->vertex_count, sizeof(int32_t), &remap_bytes))
      return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet remap size overflow");
    int32_t *remap = src->vertex_count ? (int32_t *)malloc(remap_bytes) : NULL;
    if (src->vertex_count && !remap)
      return tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet remap allocation failed");
    for (size_t i = 0; i < src->vertex_count; ++i) {
      const tet_vertex_key key = src->keys[i];
      tet_point point;
      if (key.a > (uint64_t)SIZE_MAX || key.b > (uint64_t)SIZE_MAX) {
        free(remap);
        return tet_error(err, TVDB_ERROR_INVALID_DATA, "tet remap key out of range");
      }
      if (key.a == key.b)
        point = tet_lattice_point(B, (size_t)key.a);
      else
        point = tet_cut_point(B, (size_t)key.a, (size_t)key.b);
      if (point.index < 0) {
        const tvdb_status_t st = B->st;
        free(remap);
        return tet_error(err, st, "tet merge failed");
      }
      remap[i] = point.index;
    }
    if (B->tet_count > B->max_tets ||
        src->tet_count > B->max_tets - B->tet_count) {
      free(remap);
      return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet budget exceeded");
    }
    if (!tet_grow((void **)&B->tets, &B->tet_capacity,
                  B->tet_count + src->tet_count, 4 * sizeof(int32_t))) {
      free(remap);
      return tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet allocation failed");
    }
    for (size_t t = 0; t < src->tet_count; ++t) {
      int32_t *dst = B->tets + 4 * (B->tet_count + t);
      for (size_t v = 0; v < 4; ++v) {
        const int32_t local = src->tets[4 * t + v];
        if (local < 0 || (size_t)local >= src->vertex_count) {
          free(remap);
          return tet_error(err, TVDB_ERROR_INVALID_DATA, "tet local index out of range");
        }
        dst[v] = remap[(size_t)local];
      }
    }
    B->tet_count += src->tet_count;
    free(remap);
    free(src->vertex_out);
    src->vertex_out = NULL;
    free(src->cuts);
    src->cuts = NULL;
    free(src->positions);
    src->positions = NULL;
    free(src->keys);
    src->keys = NULL;
    free(src->tets);
    src->tets = NULL;
  }
  return TVDB_OK;
}

static tvdb_status_t tet_sdf_to_tet_mesh(tvdb_sdf_sample_fn sample, void *user,
                                         const tet_sample_scope *scope,
                                         const double bbox_min[3],
                                         const double bbox_max[3],
                                         const tvdb_stuffing_options_t *options,
                                         tvdb_thread_pool_t *pool, tvdb_tet_mesh *out,
                                         tvdb_error_t *err) {
  if (!sample || !bbox_min || !bbox_max || !options || !out ||
      !isfinite(options->spacing) || options->spacing <= 0 ||
      !isfinite(options->isovalue) || !isfinite(options->alpha_long) ||
      !isfinite(options->alpha_short) || options->alpha_long < 0 ||
      options->alpha_long >= 0.5 || options->alpha_short < 0 ||
      options->alpha_short >= 0.5)
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid stuffing arguments");
  tet_lattice L;
  memset(&L, 0, sizeof(L));
  L.h = options->spacing;
  int *dims[3] = {&L.nx, &L.ny, &L.nz};
  for (int a = 0; a < 3; ++a) {
    if (!isfinite(bbox_min[a]) || !isfinite(bbox_max[a]) || bbox_max[a] < bbox_min[a])
      return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid stuffing box");
    L.origin[a] = bbox_min[a] - L.h;
    const double cells = ceil((bbox_max[a] + L.h - L.origin[a]) / L.h);
    if (!isfinite(cells) || cells < 1 || cells > 1 << 20)
      return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "stuffing lattice too large");
    *dims[a] = (int)cells;
  }
  const size_t max_vertices =
      options->max_lattice_vertices ? options->max_lattice_vertices : 8000000;
  size_t corners, centers, n;
  if (!tvdb_size_mul((size_t)L.nx + 1, (size_t)L.ny + 1, &corners) ||
      !tvdb_size_mul(corners, (size_t)L.nz + 1, &corners) ||
      !tvdb_size_mul((size_t)L.nx, (size_t)L.ny, &centers) ||
      !tvdb_size_mul(centers, (size_t)L.nz, &centers) || corners > SIZE_MAX - centers ||
      (n = corners + centers) > max_vertices || n > (size_t)INT32_MAX)
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                     "stuffing lattice exceeds the vertex budget");
  size_t value_bytes, position_bytes, vertex_out_bytes;
  if (!tvdb_size_mul(n, sizeof(float), &value_bytes) ||
      !tvdb_size_mul(n, 3 * sizeof(double), &position_bytes) ||
      !tvdb_size_mul(n, sizeof(int32_t), &vertex_out_bytes))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                     "stuffing lattice allocation size overflow");
  L.corners = corners;
  L.vertices = n;
  const double alpha_long = options->alpha_long ? options->alpha_long : TET_ALPHA_LONG;
  const double alpha_short = options->alpha_short ? options->alpha_short : TET_ALPHA_SHORT;

  tvdb_status_t st = TVDB_OK;
  tvdb_thread_pool_t *owned = NULL;
  tet_stuff_part *parts = NULL;
  size_t part_count = 0;
  float *value = (float *)malloc(value_bytes);
  double *pos = (double *)malloc(position_bytes);
  int32_t *vertex_out = (int32_t *)malloc(vertex_out_bytes);
  tet_builder B;
  memset(&B, 0, sizeof(B));
  if (!value || !pos || !vertex_out) {
    st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "stuffing allocation failed");
    goto done;
  }
  if (!pool) {
    size_t workers = n < 32768 ? 1 : tvdb_thread_default_count();
    if (workers > 8) workers = 8;
    st = tvdb_thread_pool_create(workers ? workers : 1, &owned, err);
    if (st != TVDB_OK) goto done;
    pool = owned;
  }
  for (size_t id = 0; id < n; ++id) {
    tet_position(&L, id, pos + 3 * id);
    vertex_out[id] = -1;
  }
  if (options->batch_sample) {
    /* Bounded batches keep a device's staging buffers small. */
    const size_t chunk = (size_t)1 << 22;
    for (size_t begin = 0; begin < n && st == TVDB_OK; begin += chunk) {
      const size_t count = n - begin < chunk ? n - begin : chunk;
      st = options->batch_sample(options->batch_user, pos + 3 * begin, count,
                                 value + begin, err);
      for (size_t i = 0; i < count && st == TVDB_OK; ++i)
        if (!isfinite(value[begin + i]))
          st = tet_error(err, TVDB_ERROR_INVALID_DATA, "stuffing sample failed");
    }
    if (st != TVDB_OK) goto done;
  } else {
    tet_sample_job job = {.L = &L,
                          .sample = sample,
                          .user = user,
                          .scope = scope,
                          .value = value};
    atomic_init(&job.bad, 0);
    atomic_init(&job.oom, 0);
    st = tvdb_thread_pool_for(pool, 0, n, 4096, tet_sample_range, &job, err);
    if (st == TVDB_OK && atomic_load_explicit(&job.oom, memory_order_relaxed))
      st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "stuffing sampler allocation failed");
    if (st == TVDB_OK && atomic_load_explicit(&job.bad, memory_order_relaxed))
      st = tet_error(err, TVDB_ERROR_INVALID_DATA, "stuffing sample failed");
    if (st != TVDB_OK) goto done;
  }
  /* A warped vertex takes the isovalue, so later neighbors can make different
   * cut-point choices. Keep the original flat serial traversal for one worker.
   * With multiple workers, exact-order wavefronts complete every predecessor
   * before a vertex while allowing independent vertices to run concurrently. */
  if (tvdb_thread_pool_size(pool) > 1) {
    st = tet_warp_parallel(&L, value, pos, options->isovalue, alpha_long,
                           alpha_short, pool, err);
    if (st != TVDB_OK) goto done;
  } else {
    for (int center = 0; center < 2; ++center) {
      tet_warp_job job;
      tet_warp_job_init(&job, &L, value, pos, options->isovalue,
                        alpha_long, alpha_short, center);
      for (int k = 0; k < job.ext[2]; ++k)
        for (int j = 0; j < job.ext[1]; ++j)
          for (int i = 0; i < job.ext[0]; ++i) {
            const int at[3] = {i, j, k};
            const size_t v = job.base + (size_t)i * job.stride[0] +
                             (size_t)j * job.stride[1] +
                             (size_t)k * job.stride[2];
            tet_warp_vertex(&job, v, at);
          }
    }
  }
  B.L = &L;
  B.value = value;
  B.pos = pos;
  B.iso = options->isovalue;
  B.vertex_out = vertex_out;
  B.max_tets = options->max_tets ? options->max_tets : 4000000;
  B.min_volume = 6.0 * 1e-12 * L.h * L.h * L.h;
  B.err = err;
  B.st = TVDB_OK;
  size_t workers = tvdb_thread_pool_size(pool);
  if (workers > 8) workers = 8;
  if (workers > (size_t)L.nz) workers = (size_t)L.nz;
  if (workers > 1) {
    size_t part_bytes;
    if (!tvdb_size_mul(workers, sizeof(*parts), &part_bytes)) {
      st = tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet slab table size overflow");
      goto done;
    }
    parts = (tet_stuff_part *)calloc(1, part_bytes);
    if (!parts) {
      st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet slab table allocation failed");
      goto done;
    }
    part_count = workers;
    tet_stuff_job job = {.L = &L,
                         .value = value,
                         .pos = pos,
                         .iso = options->isovalue,
                         .max_tets = B.max_tets,
                         .part_count = part_count,
                         .min_volume = B.min_volume,
                         .sample = sample,
                         .user = user,
                         .scope = scope,
                         .parts = parts};
    atomic_init(&job.total_tets, 0);
    st = tvdb_thread_pool_for(pool, 0, part_count, 1, tet_stuff_part_range,
                              &job, err);
    if (st != TVDB_OK) goto done;
    for (size_t i = 0; i < part_count; ++i) {
      if (parts[i].status != TVDB_OK) {
        st = tet_error(err, parts[i].status, parts[i].err.message);
        goto done;
      }
    }
    st = tet_stuff_parts_merge(&B, parts, part_count, err);
  } else {
    /* One scope state covers the serial loop's rare interior probes. */
    void *stencil_user = user;
    int stencil_scoped = 0;
    if (scope) {
      if (!(stencil_user = scope->begin(user))) {
        st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                       "stuffing sampler allocation failed");
        goto done;
      }
      stencil_scoped = 1;
    }
    tet_stuff_cells(&B, 0, L.nz, sample, stencil_user);
    if (stencil_scoped) scope->end(user, stencil_user);
    st = B.st;
  }
  if (st != TVDB_OK) goto done;
  {
    tvdb_tet_mesh result;
    memset(&result, 0, sizeof(result));
    result.vertex_count = B.vertex_count;
    result.tet_count = B.tet_count;
    size_t output_bytes;
    if (!tvdb_size_mul(B.vertex_count, 3 * sizeof(float), &output_bytes)) {
      st = tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet output size overflow");
      goto done;
    }
    if (B.vertex_count &&
        !(result.positions = (float *)malloc(output_bytes))) {
      st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet output allocation failed");
      goto done;
    }
    st = tet_validate_float_output(&B, &result, err);
    if (st != TVDB_OK) { tvdb_tet_mesh_free(&result); goto done; }
    result.tets = B.tets;
    B.tets = NULL;
    st = tet_boundary(result.tets, result.tet_count, result.vertex_count, pool,
                      &result.boundary, &result.boundary_count, err);
    if (st != TVDB_OK) {
      tvdb_tet_mesh_free(&result);
      goto done;
    }
    tvdb_tet_mesh_free(out);
    *out = result;
  }
done:
  tvdb_thread_pool_destroy(owned);
  tet_stuff_parts_free(parts, part_count);
  free(value);
  free(pos);
  free(vertex_out);
  free(B.cuts);
  free(B.positions);
  free(B.keys);
  free(B.tets);
  return st;
}
/* Conservative edge warp: reject any snap that would invert or collapse an
 * incident background tetrahedron. The adjacency is local to this prototype. */
static tvdb_status_t graded_warp(double *pos, float *value, float iso,
                                 size_t n, const uint32_t *tets, size_t nt,
                                 double alpha, tvdb_error_t *err) {
  size_t bytes, incidences, *offset = NULL, *cursor = NULL, *adj = NULL;
  tvdb_status_t st = TVDB_OK;
  if (!alpha) return TVDB_OK;
  if (n == SIZE_MAX || !tvdb_size_mul(n+1, sizeof(size_t), &bytes) ||
      !tvdb_size_mul(nt, 4, &incidences))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "graded adjacency overflow");
  offset = (size_t *)calloc(1, bytes);
  if (!tvdb_size_mul(n, sizeof(size_t), &bytes) || !(cursor = (size_t *)malloc(bytes ? bytes : 1)) ||
      !tvdb_size_mul(incidences, sizeof(size_t), &bytes) ||
      !(adj = (size_t *)malloc(bytes ? bytes : 1)) || !offset) {
    st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "graded adjacency allocation failed"); goto done;
  }
  for (size_t i = 0; i < incidences; ++i) ++offset[(size_t)tets[i]+1];
  for (size_t i = 0; i < n; ++i) { offset[i+1] += offset[i]; cursor[i] = offset[i]; }
  for (size_t i = 0; i < incidences; ++i) adj[cursor[tets[i]]++] = i/4;
  for (size_t v = 0; v < n; ++v) {
    double fv = (double)value[v] - iso, best = INFINITY, best_t = 0;
    size_t best_w = SIZE_MAX;
    for (size_t i = offset[v]; i < offset[v+1]; ++i) {
      size_t t = adj[i];
      for (size_t j = 0; j < 4; ++j) {
        size_t w = tets[4*t+j]; double fw = (double)value[w] - iso;
        if (!((fv < 0 && fw > 0) || (fv > 0 && fw < 0))) continue;
        double f = fv / (fv-fw);
        double dx = pos[3*v]-pos[3*w], dy = pos[3*v+1]-pos[3*w+1], dz = pos[3*v+2]-pos[3*w+2];
        double distance = f * hypot(hypot(dx,dy),dz);
        if (f < alpha && (distance < best || (distance == best && w < best_w))) {
          best = distance; best_t = f; best_w = w;
        }
      }
    }
    if (best_w == SIZE_MAX) continue;
    double candidate[3]; int valid = 1;
    for (size_t d = 0; d < 3; ++d)
      candidate[d] = pos[3*v+d] + best_t*(pos[3*best_w+d]-pos[3*v+d]);
    for (size_t i = offset[v]; i < offset[v+1] && valid; ++i) {
      size_t t = adj[i]; const double *before[4], *after[4];
      for (size_t j = 0; j < 4; ++j) {
        size_t w = tets[4*t+j]; before[j] = pos+3*w; after[j] = w == v ? candidate : before[j];
      }
      double a = tet_volume6(before[0], before[1], before[2], before[3]);
      double b = tet_volume6(after[0], after[1], after[2], after[3]);
      if (!isfinite(a) || !isfinite(b) || a == 0 || b == 0 ||
          (a > 0) != (b > 0) || fabs(b) <= fabs(a)*1e-8) valid = 0;
    }
    if (valid) { memcpy(pos+3*v, candidate, sizeof(candidate)); value[v] = iso; }
  }
done:
  free(offset); free(cursor); free(adj); return st;
}
/* Exercise the shared cut/stencil code on a graded background; not public. */
tvdb_status_t tvdb_tet_graded_stencil(const tvdb_tet_refine_graph *graph,
                                     tvdb_sdf_sample_fn sample, void *user,
                                     float iso, double alpha, size_t max_tets,
                                     tvdb_tet_mesh *out, tvdb_error_t *err) {
  tvdb_status_t st = TVDB_OK;
  tet_builder B;
  size_t n, nt, bytes;
  float *value = NULL;
  int32_t *vertex_out = NULL;
  double *pos = NULL;
  const uint32_t *tets;
  memset(&B, 0, sizeof(B));
  if (!graph || !sample || !out || !isfinite(iso) || !isfinite(alpha) || alpha < 0 || alpha >= 0.5 || !max_tets)
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid graded stencil arguments");
  n = tvdb_tet_refine_vertex_count(graph); nt = tvdb_tet_refine_tet_count(graph);
  tets = tvdb_tet_refine_tets(graph);
  if (!tvdb_size_mul(n, sizeof(float), &bytes) || n > INT32_MAX)
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "graded vertex budget exceeded");
  value = (float *)malloc(bytes ? bytes : 1);
  if (!tvdb_size_mul(n, sizeof(int32_t), &bytes) ||
      !(vertex_out = (int32_t *)malloc(bytes ? bytes : 1)) || !value) {
    st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "graded stencil allocation failed");
    goto done_graded;
  }
  if (!tvdb_size_mul(n, 3*sizeof(double), &bytes) || !(pos = (double *)malloc(bytes ? bytes : 1))) {
    st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "graded position allocation failed"); goto done_graded;
  }
  if (n) memcpy(pos, tvdb_tet_refine_positions(graph), bytes);
  for (size_t i = 0; i < n; ++i) {
    value[i] = NAN; vertex_out[i] = -1;
    if (!sample(pos + 3*i, &value[i], user) || !isfinite(value[i])) {
      st = tet_error(err, TVDB_ERROR_INVALID_DATA, "graded sample failed"); goto done_graded;
    }
  }
  st = graded_warp(pos, value, iso, n, tets, nt, alpha, err);
  if (st != TVDB_OK) goto done_graded;
  B.value = value; B.pos = pos; B.iso = iso; B.vertex_out = vertex_out;
  B.max_tets = max_tets; B.err = err; B.st = TVDB_OK;
  for (size_t i = 0; i < nt && B.st == TVDB_OK; ++i) {
    size_t id[4] = {tets[4*i], tets[4*i+1], tets[4*i+2], tets[4*i+3]};
    tet_stencil(&B, id, sample, user);
  }
  st = B.st;
  if (st != TVDB_OK) goto done_graded;
  {
    tvdb_tet_mesh result = {0};
    result.vertex_count = B.vertex_count; result.tet_count = B.tet_count;
    if (!tvdb_size_mul(B.vertex_count, 3*sizeof(float), &bytes) ||
        (B.vertex_count && !(result.positions = (float *)malloc(bytes)))) {
      st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "graded output allocation failed");
      goto done_graded;
    }
    st = tet_validate_float_output(&B, &result, err);
    if (st != TVDB_OK) { tvdb_tet_mesh_free(&result); goto done_graded; }
    result.tets = B.tets; B.tets = NULL;
    st = tet_boundary(result.tets, result.tet_count, result.vertex_count, NULL,
                      &result.boundary, &result.boundary_count, err);
    if (st != TVDB_OK) { tvdb_tet_mesh_free(&result); goto done_graded; }
    tvdb_tet_mesh_free(out); *out = result;
  }
done_graded:
  free(pos); free(value); free(vertex_out); free(B.cuts); free(B.positions); free(B.tets);
  return st;
}
static tvdb_status_t tet_sdf_to_tet_mesh_graded(tvdb_sdf_sample_fn sample, void *user,
    const double bbox_min[3], const double bbox_max[3],
    const tvdb_graded_stuffing_options_t *options, tvdb_tet_mesh *out,
    int surface_guard, tvdb_error_t *err) {
  double lo[3], hi[3]; size_t cells[3]; tvdb_tet_refine_graph *graph = NULL;
  tvdb_status_t st;
  if (!sample || !bbox_min || !bbox_max || !options || !out ||
      !isfinite(options->fine_spacing) || options->fine_spacing <= 0 ||
      !isfinite(options->coarse_spacing) || options->coarse_spacing < options->fine_spacing ||
      !isfinite(options->grading) || options->grading <= 0 ||
      !isfinite(options->warp_alpha) || options->warp_alpha < 0 || options->warp_alpha >= 0.5 ||
      !isfinite(options->isovalue))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid graded stuffing arguments");
  for (int i = 0; i < 3; ++i) {
    if (!isfinite(bbox_min[i]) || !isfinite(bbox_max[i]) || bbox_max[i] < bbox_min[i])
      return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid graded stuffing box");
    lo[i] = bbox_min[i] - options->coarse_spacing;
    hi[i] = bbox_max[i] + options->coarse_spacing;
    double count = ceil((hi[i] - lo[i]) / options->coarse_spacing);
    if (!isfinite(lo[i]) || !isfinite(hi[i]) || !isfinite(count) || count < 1 || count > (1 << 20))
      return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "graded stuffing box too large");
    cells[i] = (size_t)count;
  }
  size_t max_vertices = options->max_vertices ? options->max_vertices : 8000000;
  size_t max_background = options->max_background_tets ? options->max_background_tets : 8000000;
  size_t max_output = options->max_output_tets ? options->max_output_tets : 4000000;
  st = tvdb_tet_refine_box(lo, hi, cells, max_vertices, max_background, &graph, err);
  if (st == TVDB_OK) {
    if (surface_guard)
      st = tvdb_tet_refine_adapt_sdf_guarded(graph, sample, user, options->isovalue,
                                             options->fine_spacing, options->coarse_spacing,
                                             options->grading, err);
    else
      st = tvdb_tet_refine_adapt_sdf(graph, sample, user, options->isovalue,
                                     options->fine_spacing, options->coarse_spacing,
                                     options->grading, err);
  }
  if (st == TVDB_OK)
    st = tvdb_tet_graded_stencil(graph, sample, user, options->isovalue,
                                  options->warp_alpha, max_output, out, err);
  tvdb_tet_refine_destroy(graph);
  return st;
}
tvdb_status_t tvdb_sdf_to_tet_mesh_graded(tvdb_sdf_sample_fn sample, void *user,
    const double bbox_min[3], const double bbox_max[3],
    const tvdb_graded_stuffing_options_t *options, tvdb_tet_mesh *out, tvdb_error_t *err) {
  return tet_sdf_to_tet_mesh_graded(sample, user, bbox_min, bbox_max, options, out, 0, err);
}
tvdb_status_t tvdb_sdf_to_tet_mesh_graded_guarded(tvdb_sdf_sample_fn sample, void *user,
    const double bbox_min[3], const double bbox_max[3],
    const tvdb_graded_stuffing_options_t *options, tvdb_tet_mesh *out, tvdb_error_t *err) {
  return tet_sdf_to_tet_mesh_graded(sample, user, bbox_min, bbox_max, options, out, 1, err);
}

tvdb_status_t tvdb_sdf_to_tet_mesh(tvdb_sdf_sample_fn sample, void *user,
                                   const double bbox_min[3],
                                   const double bbox_max[3],
                                   const tvdb_stuffing_options_t *options,
                                   tvdb_thread_pool_t *pool, tvdb_tet_mesh *out,
                                   tvdb_error_t *err) {
  return tet_sdf_to_tet_mesh(sample, user, NULL, bbox_min, bbox_max, options, pool,
                             out, err);
}

/* ---- Level-set grids ---------------------------------------------------- */
/* One lookup cache per sample range: the pool hands each 4096-vertex slice to
 * whichever worker gets there first, so the slice's state is allocated on
 * entry and released on exit. Slices run for ~0.1 ms, making the allocation
 * noise next to the sampling it caches. */
typedef struct {
  const double (*inverse)[4]; /* world -> index */
  tvdb_tree_accessor acc;
} tet_sample_state;
typedef struct {
  tvdb_tree_index index;
  double inverse[3][4]; /* world -> index */
} tet_grid_sampler;
static void *tet_sampler_begin(void *user) {
  tet_grid_sampler *s = (tet_grid_sampler *)user;
  tet_sample_state *st = (tet_sample_state *)malloc(sizeof(*st));
  if (!st) return NULL;
  st->inverse = s->inverse;
  tvdb_tree_accessor_init(&st->acc, &s->index);
  return st;
}
static void tet_sampler_end(void *user, void *state) {
  (void)user;
  free(state);
}
static int tet_grid_sample(const double p[3], float *value, void *user) {
  tet_sample_state *st = (tet_sample_state *)user;
  const double(*inverse)[4] = st->inverse;
  double q[3];
  for (int a = 0; a < 3; ++a)
    q[a] = inverse[a][3] + inverse[a][0] * p[0] + inverse[a][1] * p[1] +
           inverse[a][2] * p[2];
  int32_t base[3];
  double f[3];
  for (int a = 0; a < 3; ++a) {
    const double fl = floor(q[a]);
    if (!(fl >= INT32_MIN && fl < INT32_MAX)) return 0;
    base[a] = (int32_t)fl;
    f[a] = q[a] - fl;
  }
  double sum = 0;
  for (int c = 0; c < 8; ++c) {
    const int32_t at[3] = {base[0] + (c & 1), base[1] + ((c >> 1) & 1),
                           base[2] + ((c >> 2) & 1)};
    const double w = ((c & 1) ? f[0] : 1 - f[0]) * (((c >> 1) & 1) ? f[1] : 1 - f[1]) *
                     (((c >> 2) & 1) ? f[2] : 1 - f[2]);
    sum += w * (double)tvdb_tree_accessor_get(&st->acc, at, NULL);
  }
  *value = (float)sum;
  return 1;
}
static tvdb_status_t tet_grid_to_mesh(const tvdb_grid_t *grid,
                                     const tvdb_stuffing_options_t *options,
                                     const tvdb_graded_stuffing_options_t *graded,
                                     int surface_guard,
                                     tvdb_thread_pool_t *pool,
                                     tvdb_tet_mesh *out, tvdb_error_t *err) {
  if (!grid || (!options && !graded) || !out)
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid stuffing arguments");
  tet_grid_sampler s;
  memset(&s, 0, sizeof(s));
  tvdb_status_t st = tvdb_tree_index_create(grid, 1, &s.index, err);
  if (st != TVDB_OK) return st;
  double m[4][4];
  if (!tvdb_tree_matrix(&grid->transform, m)) {
    tvdb_tree_index_destroy(&s.index);
    return tet_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM, "level set transform not invertible");
  }
  /* Invert the 3x3 linear part. */
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  double inv[3][3];
  inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
  inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
  inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
  inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
  inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
  inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
  inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
  inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
  inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) s.inverse[a][b] = inv[a][b];
    s.inverse[a][3] = -(inv[a][0] * m[0][3] + inv[a][1] * m[1][3] + inv[a][2] * m[2][3]);
  }
  if (!s.index.has_bbox) {
    tvdb_tree_index_destroy(&s.index);
    tvdb_tet_mesh_free(out);
    return TVDB_OK; /* nothing active: empty interior */
  }
  /* World box of the active voxels (index box corners, one voxel padded). */
  double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (int c = 0; c < 8; ++c) {
    const double q[3] = {(c & 1) ? (double)s.index.bbox_max[0] + 1 : (double)s.index.bbox_min[0] - 1,
                         (c & 2) ? (double)s.index.bbox_max[1] + 1 : (double)s.index.bbox_min[1] - 1,
                         (c & 4) ? (double)s.index.bbox_max[2] + 1 : (double)s.index.bbox_min[2] - 1};
    for (int a = 0; a < 3; ++a) {
      const double w = m[a][3] + m[a][0] * q[0] + m[a][1] * q[1] + m[a][2] * q[2];
      if (w < lo[a]) lo[a] = w;
      if (w > hi[a]) hi[a] = w;
    }
  }
  /* The sampler keeps its tree accessor per range (see tet_sample_scope). */
  if (graded) {
    tet_sample_state state;
    state.inverse = s.inverse;
    tvdb_tree_accessor_init(&state.acc, &s.index);
    st = tet_sdf_to_tet_mesh_graded(tet_grid_sample, &state, lo, hi, graded, out,
                                    surface_guard, err);
  } else {
    const tet_sample_scope scope = {tet_sampler_begin, tet_sampler_end};
    st = tet_sdf_to_tet_mesh(tet_grid_sample, &s, &scope, lo, hi, options, pool, out, err);
  }
  tvdb_tree_index_destroy(&s.index);
  return st;
}
tvdb_status_t tvdb_level_set_to_tet_mesh(const tvdb_grid_t *grid,
                                         const tvdb_stuffing_options_t *options,
                                         tvdb_thread_pool_t *pool,
                                         tvdb_tet_mesh *out, tvdb_error_t *err) {
  return tet_grid_to_mesh(grid, options, NULL, 0, pool, out, err);
}
tvdb_status_t tvdb_level_set_to_tet_mesh_graded(const tvdb_grid_t *grid,
    const tvdb_graded_stuffing_options_t *options, tvdb_tet_mesh *out, tvdb_error_t *err) {
  return tet_grid_to_mesh(grid, NULL, options, 0, NULL, out, err);
}
tvdb_status_t tvdb_level_set_to_tet_mesh_graded_guarded(const tvdb_grid_t *grid,
    const tvdb_graded_stuffing_options_t *options, tvdb_tet_mesh *out, tvdb_error_t *err) {
  return tet_grid_to_mesh(grid, NULL, options, 1, NULL, out, err);
}

tvdb_status_t tvdb_tet_mesh_quality(const tvdb_tet_mesh *mesh,
                                    tvdb_tet_mesh_quality_t *out,
                                    tvdb_error_t *err) {
  if (!mesh || !out || (mesh->tet_count && (!mesh->tets || !mesh->positions)))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid tet mesh");
  tvdb_tet_mesh_quality_t q = {180.0, 0.0, 0.0, INFINITY, 0};
  static const int opp[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
  for (size_t t = 0; t < mesh->tet_count; ++t) {
    double p[4][3];
    for (int i = 0; i < 4; ++i) {
      const int32_t v = mesh->tets[4 * t + i];
      if (v < 0 || (size_t)v >= mesh->vertex_count)
        return tet_error(err, TVDB_ERROR_INVALID_DATA, "tet index out of range");
      for (int k = 0; k < 3; ++k) {
        p[i][k] = mesh->positions[3 * (size_t)v + k];
        if (!isfinite(p[i][k]))
          return tet_error(err, TVDB_ERROR_INVALID_DATA,
                           "non-finite tetrahedron vertex");
      }
    }
    const double vol = tet_volume6(p[0], p[1], p[2], p[3]) / 6.0;
    q.volume += vol;
    if (vol < q.min_volume) q.min_volume = vol;
    if (!(vol > 0)) {
      ++q.inverted;
      continue;
    }
    /* Outward normal of the face opposite each vertex. */
    double n[4][3];
    for (int i = 0; i < 4; ++i) {
      const int a = (i + 1) % 4, b = (i + 2) % 4, c = (i + 3) % 4;
      const double e1[3] = {p[b][0] - p[a][0], p[b][1] - p[a][1], p[b][2] - p[a][2]};
      const double e2[3] = {p[c][0] - p[a][0], p[c][1] - p[a][1], p[c][2] - p[a][2]};
      double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
      const double d[3] = {p[i][0] - p[a][0], p[i][1] - p[a][1], p[i][2] - p[a][2]};
      if (cr[0] * d[0] + cr[1] * d[1] + cr[2] * d[2] > 0)
        for (int k = 0; k < 3; ++k) cr[k] = -cr[k];
      const double len = sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
      for (int k = 0; k < 3; ++k) n[i][k] = len > 0 ? cr[k] / len : 0;
    }
    for (int e = 0; e < 6; ++e) {
      const double *a = n[opp[e][0]], *b = n[opp[e][1]];
      double d = -(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
      d = d > 1 ? 1 : d < -1 ? -1 : d;
      const double angle = acos(d) * 57.295779513082320876798;
      if (angle < q.min_dihedral) q.min_dihedral = angle;
      if (angle > q.max_dihedral) q.max_dihedral = angle;
    }
  }
  if (!mesh->tet_count) q.min_volume = 0;
  *out = q;
  return TVDB_OK;
}

tvdb_status_t tvdb_tet_mesh_surface_distance_bound(
    const tvdb_tet_mesh *mesh, tvdb_sdf_sample_fn sample, void *user,
    float isovalue, double *out_bound, tvdb_error_t *err) {
  if (!mesh || !sample || !out_bound || !isfinite(isovalue) ||
      !mesh->boundary_count || !mesh->boundary || !mesh->positions)
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                     "invalid boundary distance bound arguments");
  if (mesh->vertex_count > SIZE_MAX / (3 * sizeof(float)) ||
      mesh->boundary_count > SIZE_MAX / (3 * sizeof(int32_t)))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                     "boundary distance bound mesh is too large");

  double bound = 0.0;
  for (size_t face = 0; face < mesh->boundary_count; ++face) {
    double p[3][3];
    for (int i = 0; i < 3; ++i) {
      const int32_t index = mesh->boundary[3 * face + (size_t)i];
      if (index < 0 || (size_t)index >= mesh->vertex_count)
        return tet_error(err, TVDB_ERROR_INVALID_DATA,
                         "boundary index out of range");
      for (int axis = 0; axis < 3; ++axis) {
        p[i][axis] = mesh->positions[3 * (size_t)index + (size_t)axis];
        if (!isfinite(p[i][axis]))
          return tet_error(err, TVDB_ERROR_INVALID_DATA,
                           "non-finite boundary vertex");
      }
    }

    double midpoint[3][3];
    for (int edge = 0; edge < 3; ++edge) {
      const int next = (edge + 1) % 3;
      for (int axis = 0; axis < 3; ++axis)
        midpoint[edge][axis] = 0.5 * (p[edge][axis] + p[next][axis]);
    }
    const double subtets[4][3][3] = {
        {{p[0][0],p[0][1],p[0][2]}, {midpoint[0][0],midpoint[0][1],midpoint[0][2]},
         {midpoint[2][0],midpoint[2][1],midpoint[2][2]}},
        {{midpoint[0][0],midpoint[0][1],midpoint[0][2]}, {p[1][0],p[1][1],p[1][2]},
         {midpoint[1][0],midpoint[1][1],midpoint[1][2]}},
        {{midpoint[2][0],midpoint[2][1],midpoint[2][2]},
         {midpoint[1][0],midpoint[1][1],midpoint[1][2]}, {p[2][0],p[2][1],p[2][2]}},
        {{midpoint[0][0],midpoint[0][1],midpoint[0][2]},
         {midpoint[1][0],midpoint[1][1],midpoint[1][2]},
         {midpoint[2][0],midpoint[2][1],midpoint[2][2]}}};
    for (int sub = 0; sub < 4; ++sub) {
      double center[3] = {0.0, 0.0, 0.0};
      for (int vertex = 0; vertex < 3; ++vertex)
        for (int axis = 0; axis < 3; ++axis)
          center[axis] += subtets[sub][vertex][axis] / 3.0;

      float value = 0.0f;
      if (!sample(center, &value, user) || !isfinite(value))
        return tet_error(err, TVDB_ERROR_INVALID_DATA,
                         "invalid boundary centroid sample");

      double radius = 0.0;
      for (int vertex = 0; vertex < 3; ++vertex) {
        const double dx = subtets[sub][vertex][0] - center[0];
        const double dy = subtets[sub][vertex][1] - center[1];
        const double dz = subtets[sub][vertex][2] - center[2];
        const double distance = hypot(hypot(dx, dy), dz);
        if (!isfinite(distance))
          return tet_error(err, TVDB_ERROR_INVALID_DATA,
                           "invalid boundary triangle radius");
        if (distance > radius) radius = distance;
      }
      const double sub_bound = fabs((double)value - (double)isovalue) + radius;
      if (!isfinite(sub_bound))
        return tet_error(err, TVDB_ERROR_INVALID_DATA,
                         "invalid boundary distance bound");
      if (sub_bound > bound) bound = sub_bound;
    }
  }

  *out_bound = bound;
  return TVDB_OK;
}
