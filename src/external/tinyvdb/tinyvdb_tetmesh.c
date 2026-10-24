#include "tinyvdb_tetmesh.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_checked.h"
#include "tinyvdb_tree_internal.h"

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

/* Doubled coordinates to a vertex id; SIZE_MAX outside the lattice or for
 * mixed parity. */
static size_t tet_id(const tet_lattice *L, int64_t x, int64_t y, int64_t z) {
  const int odd = (int)(x & 1);
  if ((int)(y & 1) != odd || (int)(z & 1) != odd || x < 0 || y < 0 || z < 0)
    return SIZE_MAX;
  if (!odd) {
    const int64_t i = x / 2, j = y / 2, k = z / 2;
    if (i > L->nx || j > L->ny || k > L->nz) return SIZE_MAX;
    return (size_t)i + (size_t)(L->nx + 1) * ((size_t)j + (size_t)(L->ny + 1) * (size_t)k);
  }
  const int64_t i = (x - 1) / 2, j = (y - 1) / 2, k = (z - 1) / 2;
  if (i >= L->nx || j >= L->ny || k >= L->nz) return SIZE_MAX;
  return L->corners + (size_t)i + (size_t)L->nx * ((size_t)j + (size_t)L->ny * (size_t)k);
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

typedef struct {
  const tet_lattice *L;
  tvdb_sdf_sample_fn sample;
  void *user;
  float *value;
  atomic_int bad;
} tet_sample_job;
static void tet_sample_range(size_t begin, size_t end, void *user) {
  tet_sample_job *job = (tet_sample_job *)user;
  for (size_t id = begin; id < end; ++id) {
    double p[3];
    float v = 0;
    tet_position(job->L, id, p);
    if (!job->sample(p, &v, job->user) || !isfinite(v))
      atomic_store_explicit(&job->bad, 1, memory_order_relaxed);
    job->value[id] = v;
  }
}

/* Output assembly: lattice vertices and cut points get output indices on
 * first use; cut points are keyed by their (sorted) edge. */
typedef struct {
  uint64_t a, b; /* edge vertex ids, a < b */
  int32_t index;
} tet_cut_slot;
typedef struct {
  const tet_lattice *L;
  const float *value;
  const double *pos; /* 3 per lattice vertex (warped) */
  float iso;
  int32_t *vertex_out; /* per lattice vertex, -1 unused */
  tet_cut_slot *cuts;
  size_t cut_capacity, cut_count;
  double *positions; /* 3 per output vertex */
  size_t vertex_count, vertex_capacity;
  int32_t *tets;
  size_t tet_count, tet_capacity, max_tets;
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
static int32_t tet_add_vertex(tet_builder *B, const double p[3]) {
  if (B->vertex_count >= (size_t)INT32_MAX ||
      !tet_grow((void **)&B->positions, &B->vertex_capacity, B->vertex_count + 1,
                3 * sizeof(double))) {
    B->st = tet_error(B->err, TVDB_ERROR_OUT_OF_MEMORY, "tet vertex allocation failed");
    return -1;
  }
  memcpy(B->positions + 3 * B->vertex_count, p, 3 * sizeof(double));
  return (int32_t)B->vertex_count++;
}
static tet_point tet_lattice_point(tet_builder *B, size_t id) {
  tet_point q = {id, id, B->vertex_out[id]};
  if (q.index < 0) {
    q.index = tet_add_vertex(B, B->pos + 3 * id);
    B->vertex_out[id] = q.index;
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
  q.index = tet_add_vertex(B, p);
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
  if (B->tet_count >= B->max_tets) {
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
    if (!sample(c, &f, user) || !(f < B->iso)) return;
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

typedef struct {
  int32_t v[3]; /* sorted */
  int32_t o[3]; /* oriented outward */
  int32_t count;
} tet_face_slot;

void tvdb_tet_mesh_free(tvdb_tet_mesh *mesh) {
  if (!mesh) return;
  free(mesh->positions);
  free(mesh->tets);
  free(mesh->boundary);
  memset(mesh, 0, sizeof(*mesh));
}

/* Faces used by exactly one tetrahedron, oriented outward. */
static tvdb_status_t tet_boundary(const int32_t *tets, size_t tet_count,
                                  int32_t **out, size_t *out_count,
                                  tvdb_error_t *err) {
  size_t n = 1024, bytes;
  while (n < 8 * tet_count) {
    if (n > SIZE_MAX / 2) return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet face overflow");
    n *= 2;
  }
  if (!tvdb_size_mul(n, sizeof(tet_face_slot), &bytes))
    return tet_error(err, TVDB_ERROR_INVALID_ARGUMENT, "tet face overflow");
  tet_face_slot *slots = (tet_face_slot *)calloc(1, bytes);
  int32_t *list = NULL;
  size_t *order = NULL;
  if (!slots) return tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet face allocation failed");
  static const int faces[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
  size_t used = 0;
  for (size_t t = 0; t < tet_count; ++t)
    for (int f = 0; f < 4; ++f) {
      int32_t o[3], s[3];
      for (int k = 0; k < 3; ++k) o[k] = s[k] = tets[4 * t + faces[f][k]];
      for (int i = 0; i < 2; ++i)
        for (int j = i + 1; j < 3; ++j)
          if (s[j] < s[i]) {
            const int32_t x = s[i];
            s[i] = s[j];
            s[j] = x;
          }
      size_t h = (size_t)tet_hash2(((uint64_t)(uint32_t)s[0] << 32) | (uint32_t)s[1],
                                   (uint64_t)(uint32_t)s[2]) & (n - 1);
      while (slots[h].count && memcmp(slots[h].v, s, sizeof(s))) h = (h + 1) & (n - 1);
      if (!slots[h].count) {
        memcpy(slots[h].v, s, sizeof(s));
        memcpy(slots[h].o, o, sizeof(o));
        ++used;
      }
      ++slots[h].count;
    }
  size_t count = 0;
  for (size_t i = 0; i < n; ++i) count += slots[i].count == 1;
  if (count) {
    if (!tvdb_size_mul(count, 3 * sizeof(int32_t), &bytes) ||
        !(list = (int32_t *)malloc(bytes)) ||
        !(order = (size_t *)malloc(count * sizeof(size_t)))) {
      free(slots);
      free(list);
      return tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet boundary allocation failed");
    }
  }
  /* Hash order depends only on the input; the caller sorts the triples. */
  size_t c = 0;
  for (size_t i = 0; i < n; ++i)
    if (slots[i].count == 1) order[c++] = i;
  for (size_t i = 0; i < count; ++i) memcpy(list + 3 * i, slots[order[i]].o, 3 * sizeof(int32_t));
  free(order);
  free(slots);
  (void)used;
  *out = list;
  *out_count = count;
  return TVDB_OK;
}
static int tet_face_compare(const void *pa, const void *pb) {
  const int32_t *a = (const int32_t *)pa, *b = (const int32_t *)pb;
  int32_t sa[3] = {a[0], a[1], a[2]}, sb[3] = {b[0], b[1], b[2]};
  for (int i = 0; i < 2; ++i)
    for (int j = i + 1; j < 3; ++j) {
      if (sa[j] < sa[i]) { const int32_t x = sa[i]; sa[i] = sa[j]; sa[j] = x; }
      if (sb[j] < sb[i]) { const int32_t x = sb[i]; sb[i] = sb[j]; sb[j] = x; }
    }
  for (int k = 0; k < 3; ++k)
    if (sa[k] != sb[k]) return sa[k] < sb[k] ? -1 : 1;
  return 0;
}

tvdb_status_t tvdb_sdf_to_tet_mesh(tvdb_sdf_sample_fn sample, void *user,
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
  L.corners = corners;
  L.vertices = n;
  const double alpha_long = options->alpha_long ? options->alpha_long : TET_ALPHA_LONG;
  const double alpha_short = options->alpha_short ? options->alpha_short : TET_ALPHA_SHORT;

  tvdb_status_t st = TVDB_OK;
  tvdb_thread_pool_t *owned = NULL;
  float *value = (float *)malloc(n * sizeof(float));
  double *pos = (double *)malloc(n * 3 * sizeof(double));
  int32_t *vertex_out = (int32_t *)malloc(n * sizeof(int32_t));
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
  {
    tet_sample_job job = {.L = &L, .sample = sample, .user = user, .value = value};
    atomic_init(&job.bad, 0);
    st = tvdb_thread_pool_for(pool, 0, n, 4096, tet_sample_range, &job, err);
    if (st == TVDB_OK && atomic_load_explicit(&job.bad, memory_order_relaxed))
      st = tet_error(err, TVDB_ERROR_INVALID_DATA, "stuffing sample failed");
    if (st != TVDB_OK) goto done;
  }
  for (size_t id = 0; id < n; ++id) {
    tet_position(&L, id, pos + 3 * id);
    vertex_out[id] = -1;
  }
  /* Warp: snap each vertex to its nearest violating cut point. A cut point
   * can violate only one endpoint (alphas < 1/2), and a warped endpoint takes
   * the isovalue, so later vertices see no cut point on its edges. */
  {
    static const int dirs_long[6][3] = {{2, 0, 0}, {-2, 0, 0}, {0, 2, 0},
                                        {0, -2, 0}, {0, 0, 2}, {0, 0, -2}};
    const float iso = options->isovalue;
    for (size_t v = 0; v < n; ++v) {
      const double fv = (double)value[v] - iso;
      if (fv == 0) continue;
      int64_t c[3];
      tet_coords(&L, v, c);
      double best = INFINITY, best_t = 0;
      size_t best_w = SIZE_MAX;
      for (int e = 0; e < 14; ++e) {
        int64_t d[3];
        double alpha, len;
        if (e < 6) {
          d[0] = dirs_long[e][0];
          d[1] = dirs_long[e][1];
          d[2] = dirs_long[e][2];
          alpha = alpha_long;
          len = L.h;
        } else {
          const int s = e - 6;
          d[0] = (s & 1) ? 1 : -1;
          d[1] = (s & 2) ? 1 : -1;
          d[2] = (s & 4) ? 1 : -1;
          alpha = alpha_short;
          len = L.h * 0.86602540378443864676;
        }
        const size_t w = tet_id(&L, c[0] + d[0], c[1] + d[1], c[2] + d[2]);
        if (w == SIZE_MAX) continue;
        const double fw = (double)value[w] - iso;
        if (!((fv < 0 && fw > 0) || (fv > 0 && fw < 0))) continue;
        const double t = fv / (fv - fw);
        if (t < alpha && t * len < best) {
          best = t * len;
          best_t = t;
          best_w = w;
        }
      }
      if (best_w == SIZE_MAX) continue;
      double pw[3];
      tet_position(&L, best_w, pw);
      for (int k = 0; k < 3; ++k) pos[3 * v + k] += best_t * (pw[k] - pos[3 * v + k]);
      value[v] = iso;
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
  for (int k = 0; k < L.nz && B.st == TVDB_OK; ++k)
    for (int j = 0; j < L.ny && B.st == TVDB_OK; ++j)
      for (int i = 0; i < L.nx && B.st == TVDB_OK; ++i) {
        const int64_t c[3] = {2 * (int64_t)i + 1, 2 * (int64_t)j + 1, 2 * (int64_t)k + 1};
        const size_t center = tet_id(&L, c[0], c[1], c[2]);
        for (int a = 0; a < 3 && B.st == TVDB_OK; ++a) {
          int64_t c2[3] = {c[0], c[1], c[2]};
          c2[a] += 2;
          const size_t other = tet_id(&L, c2[0], c2[1], c2[2]);
          if (other == SIZE_MAX) continue;
          const int b = (a + 1) % 3, cc = (a + 2) % 3;
          static const int ring[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
          size_t r[4];
          for (int q = 0; q < 4; ++q) {
            int64_t p[3] = {c[0], c[1], c[2]};
            p[a] += 1;
            p[b] += ring[q][0];
            p[cc] += ring[q][1];
            r[q] = tet_id(&L, p[0], p[1], p[2]);
          }
          for (int q = 0; q < 4 && B.st == TVDB_OK; ++q) {
            const size_t ids[4] = {center, other, r[q], r[(q + 1) % 4]};
            if (ids[2] == SIZE_MAX || ids[3] == SIZE_MAX) continue;
            tet_stencil(&B, ids, sample, user);
          }
        }
      }
  st = B.st;
  if (st != TVDB_OK) goto done;
  {
    tvdb_tet_mesh result;
    memset(&result, 0, sizeof(result));
    result.vertex_count = B.vertex_count;
    result.tet_count = B.tet_count;
    if (B.vertex_count &&
        !(result.positions = (float *)malloc(B.vertex_count * 3 * sizeof(float)))) {
      st = tet_error(err, TVDB_ERROR_OUT_OF_MEMORY, "tet output allocation failed");
      goto done;
    }
    for (size_t i = 0; i < 3 * B.vertex_count; ++i) result.positions[i] = (float)B.positions[i];
    result.tets = B.tets;
    B.tets = NULL;
    st = tet_boundary(result.tets, result.tet_count, &result.boundary,
                      &result.boundary_count, err);
    if (st != TVDB_OK) {
      tvdb_tet_mesh_free(&result);
      goto done;
    }
    qsort(result.boundary, result.boundary_count, 3 * sizeof(int32_t), tet_face_compare);
    tvdb_tet_mesh_free(out);
    *out = result;
  }
done:
  tvdb_thread_pool_destroy(owned);
  free(value);
  free(pos);
  free(vertex_out);
  free(B.cuts);
  free(B.positions);
  free(B.tets);
  return st;
}

/* ---- Level-set grids ---------------------------------------------------- */
typedef struct {
  tvdb_tree_index index;
  double inverse[3][4]; /* world -> index */
} tet_grid_sampler;
static int tet_grid_sample(const double p[3], float *value, void *user) {
  const tet_grid_sampler *s = (const tet_grid_sampler *)user;
  double q[3];
  for (int a = 0; a < 3; ++a)
    q[a] = s->inverse[a][3] + s->inverse[a][0] * p[0] + s->inverse[a][1] * p[1] +
           s->inverse[a][2] * p[2];
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
    sum += w * (double)tvdb_tree_get(&s->index, at, NULL);
  }
  *value = (float)sum;
  return 1;
}
tvdb_status_t tvdb_level_set_to_tet_mesh(const tvdb_grid_t *grid,
                                         const tvdb_stuffing_options_t *options,
                                         tvdb_thread_pool_t *pool,
                                         tvdb_tet_mesh *out, tvdb_error_t *err) {
  if (!grid || !options || !out)
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
  st = tvdb_sdf_to_tet_mesh(tet_grid_sample, &s, lo, hi, options, pool, out, err);
  tvdb_tree_index_destroy(&s.index);
  return st;
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
      for (int k = 0; k < 3; ++k) p[i][k] = mesh->positions[3 * (size_t)v + k];
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
