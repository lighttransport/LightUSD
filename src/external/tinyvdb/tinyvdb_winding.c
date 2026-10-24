#include "tinyvdb_winding.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_checked.h"

/* Exact solid angles: Van Oosterom and Strackee 1983,
 *   tan(Omega/2) = a.(b x c) / (|a||b||c| + (a.b)|c| + (b.c)|a| + (c.a)|b|)
 * with a, b, c the triangle vertices relative to the query point.
 * Fast path: Barnes-Hut over an AABB tree whose nodes aggregate the
 * area-weighted normal (dipole) of their faces; a node whose box is at
 * least TVDB_WN_FAR diagonals away contributes its dipole term instead of
 * its faces. */

/* A node is far when its box is at least TVDB_WN_FAR box diagonals away. */
#define TVDB_WN_FAR 2.0
#define TVDB_WN_LEAF 8
#define TVDB_WN_STACK 128

typedef struct {
  double lo[3], hi[3];
  double c[3]; /* area-weighted centroid */
  double n[3]; /* sum of area-weighted normals */
  int32_t first, count, left, right;
} wn_node;

struct tvdb_winding {
  double *v;      /* 9 per face */
  double *center; /* 3 per face */
  double *normal; /* 3 per face, area-weighted */
  double *area;
  int32_t *order;
  wn_node *nodes;
  size_t faces, node_count;
  tvdb_winding_kind_t kind;
};

static tvdb_status_t wn_error(tvdb_error_t *err, tvdb_status_t st,
                              const char *msg) {
  if (err) {
    memset(err, 0, sizeof(*err));
    err->status = st;
    strncpy(err->message, msg, sizeof(err->message) - 1);
  }
  return st;
}

static double wn_solid_angle(const double *v, const double p[3]) {
  double a[3], b[3], c[3];
  for (int k = 0; k < 3; ++k) {
    a[k] = v[k] - p[k];
    b[k] = v[3 + k] - p[k];
    c[k] = v[6 + k] - p[k];
  }
  const double na = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  const double nb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  const double nc = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
  const double num = a[0] * (b[1] * c[2] - b[2] * c[1]) +
                     a[1] * (b[2] * c[0] - b[0] * c[2]) +
                     a[2] * (b[0] * c[1] - b[1] * c[0]);
  const double den = na * nb * nc + (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) * nc +
                     (b[0] * c[0] + b[1] * c[1] + b[2] * c[2]) * na +
                     (c[0] * a[0] + c[1] * a[1] + c[2] * a[2]) * nb;
  return 2.0 * atan2(num, den);
}

/* Split keys carry their face, so sorting needs no shared state and
 * concurrent builds are safe. */
typedef struct {
  double key;
  int32_t face;
} wn_key;
static int wn_compare(const void *pa, const void *pb) {
  const wn_key *a = (const wn_key *)pa, *b = (const wn_key *)pb;
  if (a->key < b->key) return -1;
  if (a->key > b->key) return 1;
  return (a->face > b->face) - (a->face < b->face); /* deterministic ties */
}

static void wn_fill_node(tvdb_winding_t *w, wn_node *n) {
  for (int k = 0; k < 3; ++k) {
    n->lo[k] = INFINITY;
    n->hi[k] = -INFINITY;
    n->c[k] = n->n[k] = 0;
  }
  double area = 0;
  for (int32_t i = 0; i < n->count; ++i) {
    const size_t f = (size_t)w->order[n->first + i];
    for (int vtx = 0; vtx < 3; ++vtx)
      for (int k = 0; k < 3; ++k) {
        const double x = w->v[9 * f + 3 * vtx + k];
        if (x < n->lo[k]) n->lo[k] = x;
        if (x > n->hi[k]) n->hi[k] = x;
      }
    for (int k = 0; k < 3; ++k) {
      n->c[k] += w->area[f] * w->center[3 * f + k];
      n->n[k] += w->normal[3 * f + k];
    }
    area += w->area[f];
  }
  for (int k = 0; k < 3; ++k)
    n->c[k] = area > 0 ? n->c[k] / area : 0.5 * (n->lo[k] + n->hi[k]);
}

/* Iterative median-split build over w->order (nodes are preallocated: a
 * binary tree over n faces with leaves of at most TVDB_WN_LEAF faces has
 * fewer than 2 * n nodes). */
static int wn_build(tvdb_winding_t *w) {
  size_t cap = 2 * w->faces + 1;
  w->nodes = (wn_node *)calloc(cap, sizeof(wn_node));
  wn_key *keys = (wn_key *)malloc(w->faces * sizeof(wn_key));
  if (!w->nodes || !keys) {
    free(keys);
    return 0;
  }
  int32_t stack[TVDB_WN_STACK];
  int depth = 0;
  w->node_count = 1;
  w->nodes[0].first = 0;
  w->nodes[0].count = (int32_t)w->faces;
  stack[depth++] = 0;
  while (depth) {
    const int32_t id = stack[--depth];
    wn_node *n = w->nodes + id;
    wn_fill_node(w, n);
    n->left = n->right = -1;
    if (n->count <= TVDB_WN_LEAF) continue;
    int axis = 0;
    double ext = n->hi[0] - n->lo[0];
    for (int k = 1; k < 3; ++k)
      if (n->hi[k] - n->lo[k] > ext) {
        ext = n->hi[k] - n->lo[k];
        axis = k;
      }
    for (int32_t i = 0; i < n->count; ++i) {
      const int32_t f = w->order[n->first + i];
      keys[i].key = w->center[3 * (size_t)f + axis];
      keys[i].face = f;
    }
    qsort(keys, (size_t)n->count, sizeof(wn_key), wn_compare);
    for (int32_t i = 0; i < n->count; ++i) w->order[n->first + i] = keys[i].face;
    const int32_t half = n->count / 2;
    if (w->node_count + 2 > cap || depth + 2 > TVDB_WN_STACK) {
      free(keys);
      return 0;
    }
    const int32_t l = (int32_t)w->node_count++, r = (int32_t)w->node_count++;
    n = w->nodes + id;
    n->left = l;
    n->right = r;
    w->nodes[l].first = n->first;
    w->nodes[l].count = half;
    w->nodes[r].first = n->first + half;
    w->nodes[r].count = n->count - half;
    stack[depth++] = r;
    stack[depth++] = l;
  }
  free(keys);
  return 1;
}

void tvdb_winding_destroy(tvdb_winding_t *w) {
  if (!w) return;
  free(w->v);
  free(w->center);
  free(w->normal);
  free(w->area);
  free(w->order);
  free(w->nodes);
  free(w);
}

tvdb_status_t tvdb_winding_create(const tvdb_triangle_mesh *mesh,
                                  tvdb_winding_kind_t kind,
                                  tvdb_winding_t **out, tvdb_error_t *err) {
  if (!mesh || !out || !mesh->vertices || !mesh->faces || !mesh->face_count ||
      (kind != TVDB_WINDING_EXACT && kind != TVDB_WINDING_FAST) ||
      mesh->face_count > (size_t)INT32_MAX / 2)
    return wn_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid winding mesh");
  for (size_t i = 0; i < mesh->vertex_count; ++i) {
    const tvdb_vec3f p = mesh->vertices[i];
    if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z))
      return wn_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                      "winding mesh has a nonfinite vertex");
  }
  const size_t nf = mesh->face_count;
  size_t bytes9, bytes3, bytes1;
  if (!tvdb_size_mul(nf, 9 * sizeof(double), &bytes9) ||
      !tvdb_size_mul(nf, 3 * sizeof(double), &bytes3) ||
      !tvdb_size_mul(nf, sizeof(double), &bytes1))
    return wn_error(err, TVDB_ERROR_INVALID_ARGUMENT, "winding size overflow");
  tvdb_winding_t *w = (tvdb_winding_t *)calloc(1, sizeof(*w));
  if (!w) return wn_error(err, TVDB_ERROR_OUT_OF_MEMORY, "winding allocation failed");
  w->faces = nf;
  w->kind = kind;
  w->v = (double *)malloc(bytes9);
  w->center = (double *)malloc(bytes3);
  w->normal = (double *)malloc(bytes3);
  w->area = (double *)malloc(bytes1);
  if (!w->v || !w->center || !w->normal || !w->area) goto oom;
  for (size_t f = 0; f < nf; ++f) {
    const uint32_t idx[3] = {mesh->faces[f].v0, mesh->faces[f].v1,
                             mesh->faces[f].v2};
    for (int vtx = 0; vtx < 3; ++vtx) {
      if (idx[vtx] >= mesh->vertex_count) {
        tvdb_winding_destroy(w);
        return wn_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                        "winding face index out of range");
      }
      const tvdb_vec3f p = mesh->vertices[idx[vtx]];
      w->v[9 * f + 3 * vtx + 0] = p.x;
      w->v[9 * f + 3 * vtx + 1] = p.y;
      w->v[9 * f + 3 * vtx + 2] = p.z;
    }
    const double *v = w->v + 9 * f;
    const double e1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]};
    const double e2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
    const double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                          e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0]};
    w->area[f] = 0.5 * sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
    for (int k = 0; k < 3; ++k) {
      w->center[3 * f + k] = (v[k] + v[3 + k] + v[6 + k]) / 3.0;
      w->normal[3 * f + k] = 0.5 * cr[k];
    }
  }
  if (kind == TVDB_WINDING_FAST) {
    w->order = (int32_t *)malloc(nf * sizeof(int32_t));
    if (!w->order) goto oom;
    for (size_t f = 0; f < nf; ++f) w->order[f] = (int32_t)f;
    if (!wn_build(w)) goto oom;
  }
  *out = w;
  return TVDB_OK;
oom:
  tvdb_winding_destroy(w);
  return wn_error(err, TVDB_ERROR_OUT_OF_MEMORY, "winding allocation failed");
}

static double wn_box_distance(const wn_node *n, const double p[3]) {
  double d2 = 0;
  for (int k = 0; k < 3; ++k) {
    const double d = p[k] < n->lo[k] ? n->lo[k] - p[k]
                     : p[k] > n->hi[k] ? p[k] - n->hi[k]
                                       : 0;
    d2 += d * d;
  }
  return sqrt(d2);
}

double tvdb_winding_eval(const tvdb_winding_t *w, const double p[3]) {
  if (!w || !p || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
    return NAN;
  const double four_pi = 12.566370614359172953850573533118;
  double sum = 0;
  if (w->kind == TVDB_WINDING_EXACT) {
    for (size_t f = 0; f < w->faces; ++f) sum += wn_solid_angle(w->v + 9 * f, p);
    return sum / four_pi;
  }
  int32_t stack[TVDB_WN_STACK];
  int depth = 0;
  stack[depth++] = 0;
  while (depth) {
    const wn_node *n = w->nodes + stack[--depth];
    if (n->left < 0) {
      for (int32_t i = 0; i < n->count; ++i)
        sum += wn_solid_angle(w->v + 9 * (size_t)w->order[n->first + i], p);
      continue;
    }
    const double dist = wn_box_distance(n, p);
    const double ex = n->hi[0] - n->lo[0], ey = n->hi[1] - n->lo[1],
                 ez = n->hi[2] - n->lo[2];
    const double diag = sqrt(ex * ex + ey * ey + ez * ez);
    if (dist > TVDB_WN_FAR * diag) {
      const double r[3] = {n->c[0] - p[0], n->c[1] - p[1], n->c[2] - p[2]};
      const double r2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
      /* Solid angle of the aggregated dipole: (sum a_f n_f) . r / |r|^3. */
      if (r2 > 1e-300)
        sum += (n->n[0] * r[0] + n->n[1] * r[1] + n->n[2] * r[2]) / (r2 * sqrt(r2));
      continue;
    }
    /* Median splits keep the depth at log2(faces), far below the stack. */
    stack[depth++] = n->right;
    stack[depth++] = n->left;
  }
  return sum / four_pi;
}
