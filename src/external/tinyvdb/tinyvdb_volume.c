#include "tinyvdb_volume.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_checked.h"
#include "tinyvdb_tree_internal.h"

static tvdb_status_t vol_error(tvdb_error_t *err, tvdb_status_t st, const char *msg) {
  if (err) {
    memset(err, 0, sizeof(*err));
    err->status = st;
    err->grid_index = -1;
    strncpy(err->message, msg, sizeof(err->message) - 1);
  }
  return st;
}

static int vol_components(tvdb_value_type_t t) {
  return t == TVDB_VALUE_VEC3I || t == TVDB_VALUE_VEC3F || t == TVDB_VALUE_VEC3D ? 3 : 1;
}
static int vol_supported(tvdb_value_type_t t) {
  switch (t) {
    case TVDB_VALUE_BOOL: case TVDB_VALUE_INT32: case TVDB_VALUE_INT64:
    case TVDB_VALUE_FLOAT: case TVDB_VALUE_DOUBLE: case TVDB_VALUE_VEC3I:
    case TVDB_VALUE_VEC3F: case TVDB_VALUE_VEC3D:
      return 1;
    default:
      return 0;
  }
}
/* Stored bytes of type t to floats (unaligned-safe). */
static void vol_from_bytes(tvdb_value_type_t t, const uint8_t *b, float out[3]) {
  switch (t) {
    case TVDB_VALUE_BOOL: out[0] = b[0] ? 1.0f : 0.0f; return;
    case TVDB_VALUE_INT32: { int32_t v; memcpy(&v, b, 4); out[0] = (float)v; return; }
    case TVDB_VALUE_INT64: { int64_t v; memcpy(&v, b, 8); out[0] = (float)v; return; }
    case TVDB_VALUE_FLOAT: memcpy(out, b, 4); return;
    case TVDB_VALUE_DOUBLE: { double v; memcpy(&v, b, 8); out[0] = (float)v; return; }
    case TVDB_VALUE_VEC3I: {
      int32_t v[3]; memcpy(v, b, 12);
      for (int k = 0; k < 3; ++k) out[k] = (float)v[k];
      return;
    }
    case TVDB_VALUE_VEC3F: memcpy(out, b, 12); return;
    case TVDB_VALUE_VEC3D: {
      double v[3]; memcpy(v, b, 24);
      for (int k = 0; k < 3; ++k) out[k] = (float)v[k];
      return;
    }
    default: out[0] = out[1] = out[2] = 0; return;
  }
}
static void vol_from_value(const tvdb_value_t *v, float out[3]) {
  out[0] = out[1] = out[2] = 0;
  switch (v->type) {
    case TVDB_VALUE_BOOL: out[0] = v->u.b ? 1.0f : 0.0f; return;
    case TVDB_VALUE_INT32: out[0] = (float)v->u.i32; return;
    case TVDB_VALUE_INT64: out[0] = (float)v->u.i64; return;
    case TVDB_VALUE_FLOAT: out[0] = v->u.f; return;
    case TVDB_VALUE_DOUBLE: out[0] = (float)v->u.d; return;
    case TVDB_VALUE_VEC3I: for (int k = 0; k < 3; ++k) out[k] = (float)v->u.vec3i[k]; return;
    case TVDB_VALUE_VEC3F: for (int k = 0; k < 3; ++k) out[k] = v->u.vec3f[k]; return;
    case TVDB_VALUE_VEC3D: for (int k = 0; k < 3; ++k) out[k] = (float)v->u.vec3d[k]; return;
    default: return;
  }
}

tvdb_grid_class_t tvdb_grid_class_from_string(const char *s) {
  if (!s) return TVDB_GRID_CLASS_UNKNOWN;
  if (!strcmp(s, "level set")) return TVDB_GRID_CLASS_LEVEL_SET;
  if (!strcmp(s, "fog volume")) return TVDB_GRID_CLASS_FOG_VOLUME;
  if (!strcmp(s, "staggered")) return TVDB_GRID_CLASS_STAGGERED;
  return TVDB_GRID_CLASS_UNKNOWN;
}
const char *tvdb_grid_class_string(tvdb_grid_class_t c) {
  switch (c) {
    case TVDB_GRID_CLASS_LEVEL_SET: return "level set";
    case TVDB_GRID_CLASS_FOG_VOLUME: return "fog volume";
    case TVDB_GRID_CLASS_STAGGERED: return "staggered";
    default: return "unknown";
  }
}

static int vol_has_tree(const tvdb_grid_t *g) {
  return g->tree.nodes && g->tree.num_nodes && g->tree.layout.num_levels >= 2;
}

/* World box of the voxel-corner box around [lo, hi] (inclusive indices). */
static void vol_world_box(const double m[4][4], const int32_t lo[3], const int32_t hi[3],
                          double wlo[3], double whi[3]) {
  for (int a = 0; a < 3; ++a) {
    wlo[a] = INFINITY;
    whi[a] = -INFINITY;
  }
  for (int c = 0; c < 8; ++c) {
    const double q[3] = {(c & 1) ? hi[0] + 0.5 : lo[0] - 0.5, (c & 2) ? hi[1] + 0.5 : lo[1] - 0.5,
                         (c & 4) ? hi[2] + 0.5 : lo[2] - 0.5};
    for (int a = 0; a < 3; ++a) {
      const double w = m[a][3] + m[a][0] * q[0] + m[a][1] * q[1] + m[a][2] * q[2];
      if (w < wlo[a]) wlo[a] = w;
      if (w > whi[a]) whi[a] = w;
    }
  }
}

tvdb_status_t tvdb_grid_get_info(const tvdb_grid_t *g, tvdb_grid_info_t *out,
                                 tvdb_error_t *err) {
  if (!g || !out) return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid grid info arguments");
  memset(out, 0, sizeof(*out));
  const int levels = g->tree.layout.num_levels;
  if (levels < 1)
    return vol_error(err, TVDB_ERROR_INVALID_DATA, "grid has no layout (read its header first)");
  out->name = g->descriptor.grid_name;
  out->value_type = g->tree.layout.levels[levels - 1].value_type;
  out->components = vol_components(out->value_type);
  out->is_mask = g->tree.is_mask_grid ||
                 (g->descriptor.grid_type && !strncmp(g->descriptor.grid_type, "Tree_mask_", 10));
  out->saved_as_half = g->descriptor.save_float_as_half;
  const tvdb_meta_entry_t *cls = tvdb_grid_find_metadata(g, "class");
  if (cls && cls->value.type == TVDB_VALUE_STRING)
    out->grid_class = tvdb_grid_class_from_string(cls->value.u.s.str);
  out->has_transform = tvdb_tree_matrix(&g->transform, out->index_to_world);
  if (vol_has_tree(g)) {
    tvdb_tree_index ix;
    tvdb_status_t st = tvdb_tree_index_create(g, 0, &ix, err);
    if (st != TVDB_OK) return st;
    out->has_tree = 1;
    out->has_bounds = ix.has_bbox;
    for (int a = 0; a < 3 && ix.has_bbox; ++a) {
      out->index_min[a] = (int32_t)ix.bbox_min[a];
      out->index_max[a] = (int32_t)(ix.bbox_max[a] - 1); /* the index's max is exclusive */
    }
    out->active_voxels = ix.active_voxels;
    vol_from_value(&g->tree.nodes[0].u.root.background, out->background);
    tvdb_tree_index_destroy(&ix);
  } else {
    const tvdb_meta_entry_t *lo = tvdb_grid_find_metadata(g, "file_bbox_min");
    const tvdb_meta_entry_t *hi = tvdb_grid_find_metadata(g, "file_bbox_max");
    const tvdb_meta_entry_t *n = tvdb_grid_find_metadata(g, "file_voxel_count");
    if (lo && hi && lo->value.type == TVDB_VALUE_VEC3I && hi->value.type == TVDB_VALUE_VEC3I &&
        lo->value.u.vec3i[0] <= hi->value.u.vec3i[0] && lo->value.u.vec3i[1] <= hi->value.u.vec3i[1] &&
        lo->value.u.vec3i[2] <= hi->value.u.vec3i[2]) {
      out->has_bounds = 1;
      memcpy(out->index_min, lo->value.u.vec3i, sizeof(out->index_min));
      memcpy(out->index_max, hi->value.u.vec3i, sizeof(out->index_max));
    }
    if (n && n->value.type == TVDB_VALUE_INT64 && n->value.u.i64 > 0)
      out->active_voxels = (uint64_t)n->value.u.i64;
  }
  if (out->has_bounds && out->has_transform)
    vol_world_box(out->index_to_world, out->index_min, out->index_max, out->world_min, out->world_max);
  return TVDB_OK;
}

/* ---- Dense extraction ---------------------------------------------------- */
/* Default scratch_bytes: the sums and counts of one slab of output layers. */
#define VOL_SLAB_BYTES ((size_t)64 << 20)

typedef struct {
  const tvdb_grid_t *g;
  const tvdb_tree_index *ix;
  tvdb_value_type_t vt;
  size_t vsize;
  int comps, stride, inactive_bg;
  int magnitude;          /* vector grids: one channel, the length */
  int empty;              /* no active voxels and no window */
  int64_t lo[3], hi[3];   /* window, inclusive */
  int32_t dims[3];
  size_t cells;
  size_t scratch_bytes;
  float bg[3];
  double m[4][4];         /* index to world */
  int64_t zlo, zhi;       /* index z range painted this pass, inclusive */
  int32_t w0;             /* stride > 1: first output layer of the slab */
  float *out;             /* stride 1: values */
  double *sum;            /* stride > 1: comps per slab cell */
  uint64_t *count;        /* stride > 1: covered voxels per slab cell */
} vol_paint;

static int vol_out_comps(const vol_paint *P) { return P->magnitude ? 1 : P->comps; }

static float vol_length(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static size_t vol_cell(const vol_paint *P, int64_t x, int64_t y, int64_t z) {
  return (size_t)((x - P->lo[0]) / P->stride) +
         (size_t)P->dims[0] * ((size_t)((y - P->lo[1]) / P->stride) +
                               (size_t)P->dims[1] * (size_t)((z - P->lo[2]) / P->stride));
}

/* Value v over the index box [b0, b1) (clipped to the window and the pass). */
static void vol_paint_box(vol_paint *P, const int64_t b0[3], const int64_t b1[3], const float v[3]) {
  int64_t c0[3], c1[3];
  for (int a = 0; a < 3; ++a) {
    c0[a] = b0[a] > P->lo[a] ? b0[a] : P->lo[a];
    c1[a] = b1[a] < P->hi[a] + 1 ? b1[a] : P->hi[a] + 1;
  }
  if (c0[2] < P->zlo) c0[2] = P->zlo;
  if (c1[2] > P->zhi + 1) c1[2] = P->zhi + 1;
  for (int a = 0; a < 3; ++a)
    if (c0[a] >= c1[a]) return;
  const int C = P->comps;
  if (P->stride == 1) {
    const int OC = vol_out_comps(P);
    float o[3] = {v[0], v[1], v[2]};
    if (P->magnitude) o[0] = vol_length(v);
    for (int64_t z = c0[2]; z < c1[2]; ++z)
      for (int64_t y = c0[1]; y < c1[1]; ++y) {
        float *row = P->out + OC * vol_cell(P, c0[0], y, z);
        for (int64_t x = c0[0]; x < c1[0]; ++x, row += OC)
          for (int k = 0; k < OC; ++k) row[k] = o[k];
      }
    return;
  }
  /* Per output cell, the overlap volume. */
  const int64_t s = P->stride;
  int64_t u0[3], u1[3];
  for (int a = 0; a < 3; ++a) {
    u0[a] = (c0[a] - P->lo[a]) / s;
    u1[a] = (c1[a] - 1 - P->lo[a]) / s;
  }
  for (int64_t w = u0[2]; w <= u1[2]; ++w) {
    const int64_t zl = P->lo[2] + w * s, zo = (c1[2] < zl + s ? c1[2] : zl + s) - (c0[2] > zl ? c0[2] : zl);
    for (int64_t v2 = u0[1]; v2 <= u1[1]; ++v2) {
      const int64_t yl = P->lo[1] + v2 * s, yo = (c1[1] < yl + s ? c1[1] : yl + s) - (c0[1] > yl ? c0[1] : yl);
      for (int64_t u = u0[0]; u <= u1[0]; ++u) {
        const int64_t xl = P->lo[0] + u * s, xo = (c1[0] < xl + s ? c1[0] : xl + s) - (c0[0] > xl ? c0[0] : xl);
        const uint64_t n = (uint64_t)(xo * yo * zo);
        const size_t cell =
            (size_t)u + (size_t)P->dims[0] * ((size_t)v2 + (size_t)P->dims[1] * (size_t)(w - P->w0));
        P->count[cell] += n;
        for (int k = 0; k < C; ++k) P->sum[(size_t)C * cell + (size_t)k] += (double)v[k] * (double)n;
      }
    }
  }
}

static void vol_paint_node(vol_paint *P, size_t node) {
  const tvdb_tree_t *t = &P->g->tree;
  const tvdb_tree_node_t *n = &t->nodes[node];
  const int l = n->level, L = t->layout.levels[l].log2dim;
  const int64_t span = P->ix->span[l];
  const int32_t *o = P->ix->origins[node];
  for (int a = 0; a < 3; ++a)
    if ((int64_t)o[a] > P->hi[a] || (int64_t)o[a] + span <= P->lo[a]) return;
  if ((int64_t)o[2] > P->zhi || (int64_t)o[2] + span <= P->zlo) return;
  const int64_t sub = l + 1 < P->ix->levels ? P->ix->span[l + 1] : 1;
  const size_t dim = (size_t)1 << L;
  float v[3];
  if (n->type == TVDB_NODE_LEAF) {
    const tvdb_leaf_node_t *leaf = &n->u.leaf;
    for (size_t i = 0; i < dim; ++i)
      for (size_t j = 0; j < dim; ++j)
        for (size_t k = 0; k < dim; ++k) {
          const size_t slot = (i << (2 * L)) | (j << L) | k;
          const int on = (leaf->value_mask.bits.data[slot / 8] >> (slot % 8)) & 1;
          if (P->inactive_bg && !on) continue;
          const int64_t b0[3] = {o[0] + (int64_t)i, o[1] + (int64_t)j, o[2] + (int64_t)k};
          const int64_t b1[3] = {b0[0] + 1, b0[1] + 1, b0[2] + 1};
          vol_from_bytes(P->vt, leaf->data + slot * P->vsize, v);
          vol_paint_box(P, b0, b1, v);
        }
    return;
  }
  const tvdb_internal_node_t *in = &n->u.internal;
  size_t child = 0;
  for (size_t slot = 0; slot < dim * dim * dim; ++slot) {
    const int has_child = (in->child_mask.bits.data[slot / 8] >> (slot % 8)) & 1;
    if (has_child) {
      vol_paint_node(P, in->child_indices[child++]);
      continue;
    }
    const int on = (in->value_mask.bits.data[slot / 8] >> (slot % 8)) & 1;
    if (P->inactive_bg && !on) continue;
    const size_t i = slot >> (2 * L), j = (slot >> L) & (dim - 1), k = slot & (dim - 1);
    const int64_t b0[3] = {o[0] + (int64_t)i * sub, o[1] + (int64_t)j * sub, o[2] + (int64_t)k * sub};
    const int64_t b1[3] = {b0[0] + sub, b0[1] + sub, b0[2] + sub};
    vol_from_bytes(P->vt, in->values + slot * P->vsize, v);
    vol_paint_box(P, b0, b1, v);
  }
}

/* Root: children and tiles. Uncovered space keeps the background. */
static void vol_paint_root(vol_paint *P) {
  const tvdb_root_node_t *r = &P->g->tree.nodes[0].u.root;
  float v[3];
  for (uint32_t i = 0; i < r->num_children; ++i) vol_paint_node(P, r->child_indices[i]);
  for (uint32_t i = 0; i < r->num_tiles; ++i) {
    if (P->inactive_bg && !r->tile_active[i]) continue;
    const int64_t b0[3] = {r->tile_origins[3 * i], r->tile_origins[3 * i + 1], r->tile_origins[3 * i + 2]};
    const int64_t b1[3] = {b0[0] + P->ix->span[1], b0[1] + P->ix->span[1], b0[2] + P->ix->span[1]};
    vol_from_value(&r->tile_values[i], v);
    vol_paint_box(P, b0, b1, v);
  }
}

/* Window, stride and dims for the options. On TVDB_OK, *ix is live (the
 * caller destroys it) and P refers to it. */
static tvdb_status_t vol_plan(const tvdb_grid_t *g, const tvdb_dense_volume_options_t *options,
                              tvdb_tree_index *ix, vol_paint *P, tvdb_error_t *err) {
  tvdb_dense_volume_options_t opt;
  memset(&opt, 0, sizeof(opt));
  if (options) opt = *options;
  if (!g || opt.stride < 0)
    return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid dense volume arguments");
  if (!vol_has_tree(g))
    return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "grid tree not decoded (tvdb_read_grid)");
  if (g->tree.is_point_data_grid || g->tree.is_point_index_grid)
    return vol_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE, "point grids have no voxel values");
  tvdb_status_t st = tvdb_tree_index_create(g, 0, ix, err);
  if (st != TVDB_OK) return st;
  memset(P, 0, sizeof(*P));
  P->g = g;
  P->ix = ix;
  P->vt = g->tree.layout.levels[ix->levels - 1].value_type;
  if (!vol_supported(P->vt)) {
    tvdb_tree_index_destroy(ix);
    return vol_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE, "unsupported voxel value type");
  }
  P->vsize = tvdb_value_type_size(P->vt);
  P->comps = vol_components(P->vt);
  P->magnitude = opt.vector_magnitude && P->comps == 3;
  P->inactive_bg = opt.inactive_as_background;
  P->scratch_bytes = opt.scratch_bytes ? opt.scratch_bytes : VOL_SLAB_BYTES;
  P->stride = 1;
  vol_from_value(&g->tree.nodes[0].u.root.background, P->bg);
  if (!tvdb_tree_matrix(&g->transform, P->m)) {
    tvdb_tree_index_destroy(ix);
    return vol_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM, "grid transform is not linear");
  }
  if (opt.use_window) {
    for (int a = 0; a < 3; ++a) {
      if (opt.window_min[a] > opt.window_max[a]) {
        tvdb_tree_index_destroy(ix);
        return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "empty dense volume window");
      }
      P->lo[a] = opt.window_min[a];
      P->hi[a] = opt.window_max[a];
    }
  } else if (ix->has_bbox) {
    for (int a = 0; a < 3; ++a) {
      P->lo[a] = ix->bbox_min[a];
      P->hi[a] = ix->bbox_max[a] - 1; /* exclusive in the index */
    }
  } else {
    P->empty = 1; /* nothing active: an empty volume */
    return TVDB_OK;
  }
  /* Stride: the given one, or the smallest whose cells fit the budget. */
  int64_t ext[3];
  for (int a = 0; a < 3; ++a) ext[a] = P->hi[a] - P->lo[a] + 1;
  int s = opt.stride ? opt.stride : 1;
  for (;;) {
    const double cells = ceil((double)ext[0] / s) * ceil((double)ext[1] / s) * ceil((double)ext[2] / s);
    if (!opt.max_voxels || cells <= (double)opt.max_voxels) break;
    if (opt.stride || s >= (1 << 20)) {
      tvdb_tree_index_destroy(ix);
      return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "dense volume exceeds the voxel budget");
    }
    ++s;
  }
  P->stride = s;
  size_t cells = 1, bytes;
  for (int a = 0; a < 3; ++a) {
    const int64_t d = (ext[a] + s - 1) / s;
    if (d > INT32_MAX || !tvdb_size_mul(cells, (size_t)d, &cells)) {
      tvdb_tree_index_destroy(ix);
      return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "dense volume too large");
    }
    P->dims[a] = (int32_t)d;
  }
  if (!tvdb_size_mul(cells, (size_t)vol_out_comps(P) * sizeof(float), &bytes)) {
    tvdb_tree_index_destroy(ix);
    return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "dense volume too large");
  }
  P->cells = cells;
  return TVDB_OK;
}

/* The layout of a planned extraction, without data. */
static void vol_layout(const vol_paint *P, tvdb_dense_volume_t *out) {
  memset(out, 0, sizeof(*out));
  out->components = vol_out_comps(P);
  memcpy(out->background, P->bg, sizeof(P->bg));
  out->stride = P->stride;
  if (P->empty) {
    memcpy(out->cell_to_world, P->m, sizeof(P->m));
    return;
  }
  const int s = P->stride;
  for (int a = 0; a < 3; ++a) {
    out->dims[a] = P->dims[a];
    out->index_min[a] = (int32_t)P->lo[a];
  }
  /* cell (u, v, w) -> index center lo + s * u + (s - 1) / 2 -> world. */
  const double off = 0.5 * (double)(s - 1);
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) out->cell_to_world[a][b] = P->m[a][b] * (double)s;
    out->cell_to_world[a][3] = P->m[a][3] + P->m[a][0] * ((double)P->lo[0] + off) +
                               P->m[a][1] * ((double)P->lo[1] + off) +
                               P->m[a][2] * ((double)P->lo[2] + off);
  }
  out->cell_to_world[3][3] = 1;
}

/* Paints a planned extraction into data (P->cells * out comps floats). */
static tvdb_status_t vol_fill(vol_paint *P, float *data, tvdb_error_t *err) {
  if (P->empty) return TVDB_OK;
  const int C = P->comps, OC = vol_out_comps(P);
  if (P->stride == 1) {
    float o[3] = {P->bg[0], P->bg[1], P->bg[2]};
    if (P->magnitude) o[0] = vol_length(P->bg);
    for (size_t i = 0; i < P->cells; ++i)
      for (int k = 0; k < OC; ++k) data[(size_t)OC * i + (size_t)k] = o[k];
    P->out = data;
    P->zlo = P->lo[2];
    P->zhi = P->hi[2];
    vol_paint_root(P);
    return TVDB_OK;
  }
  /* Slabs of whole output layers; cells sum their voxels in double. */
  const int64_t s = P->stride;
  const size_t layer = (size_t)P->dims[0] * (size_t)P->dims[1]; /* <= cells */
  size_t per_layer, sum_bytes, count_bytes;
  if (!tvdb_size_mul(layer, (size_t)C * sizeof(double) + sizeof(uint64_t), &per_layer))
    return vol_error(err, TVDB_ERROR_OUT_OF_MEMORY, "dense volume scratch too large");
  size_t layers = per_layer ? P->scratch_bytes / per_layer : 1;
  if (layers < 1) layers = 1;
  if (layers > (size_t)P->dims[2]) layers = (size_t)P->dims[2];
  if (!tvdb_size_mul(layers * layer, (size_t)C * sizeof(double), &sum_bytes) ||
      !tvdb_size_mul(layers * layer, sizeof(uint64_t), &count_bytes))
    return vol_error(err, TVDB_ERROR_OUT_OF_MEMORY, "dense volume scratch too large");
  P->sum = (double *)malloc(sum_bytes);
  P->count = (uint64_t *)malloc(count_bytes);
  if (!P->sum || !P->count) {
    free(P->sum);
    free(P->count);
    P->sum = NULL;
    P->count = NULL;
    return vol_error(err, TVDB_ERROR_OUT_OF_MEMORY, "dense volume allocation failed");
  }
  for (int32_t w0 = 0; w0 < P->dims[2]; w0 += (int32_t)layers) {
    const int32_t nl = (int32_t)((size_t)(P->dims[2] - w0) < layers ? (size_t)(P->dims[2] - w0) : layers);
    memset(P->sum, 0, (size_t)nl * layer * (size_t)C * sizeof(double));
    memset(P->count, 0, (size_t)nl * layer * sizeof(uint64_t));
    P->w0 = w0;
    P->zlo = P->lo[2] + (int64_t)w0 * s;
    P->zhi = P->lo[2] + (int64_t)(w0 + nl) * s - 1;
    if (P->zhi > P->hi[2]) P->zhi = P->hi[2];
    vol_paint_root(P);
    for (int32_t w = w0; w < w0 + nl; ++w)
      for (int32_t v2 = 0; v2 < P->dims[1]; ++v2)
        for (int32_t u = 0; u < P->dims[0]; ++u) {
          /* Voxels of this cell inside the window. */
          uint64_t n = 1;
          const int32_t c[3] = {u, v2, w};
          for (int a = 0; a < 3; ++a) {
            const int64_t l0 = P->lo[a] + (int64_t)c[a] * s;
            const int64_t l1 = l0 + s - 1 < P->hi[a] ? l0 + s - 1 : P->hi[a];
            n *= (uint64_t)(l1 - l0 + 1);
          }
          const size_t sc = (size_t)u + (size_t)P->dims[0] * ((size_t)v2 + (size_t)P->dims[1] * (size_t)(w - w0));
          const size_t cell = (size_t)u + (size_t)P->dims[0] * ((size_t)v2 + (size_t)P->dims[1] * (size_t)w);
          const double missing = (double)(n - P->count[sc]);
          float val[3] = {0, 0, 0};
          for (int k = 0; k < C; ++k)
            val[k] = (float)((P->sum[(size_t)C * sc + (size_t)k] + missing * P->bg[k]) / (double)n);
          if (P->magnitude) {
            data[cell] = vol_length(val);
          } else {
            for (int k = 0; k < C; ++k) data[(size_t)C * cell + (size_t)k] = val[k];
          }
        }
  }
  free(P->sum);
  free(P->count);
  P->sum = NULL;
  P->count = NULL;
  return TVDB_OK;
}

void tvdb_dense_volume_free(tvdb_dense_volume_t *v) {
  if (!v) return;
  free(v->data);
  memset(v, 0, sizeof(*v));
}

tvdb_status_t tvdb_grid_dense_volume_layout(const tvdb_grid_t *g,
                                            const tvdb_dense_volume_options_t *options,
                                            tvdb_dense_volume_t *out, tvdb_error_t *err) {
  if (!out) return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid dense volume arguments");
  tvdb_tree_index ix;
  vol_paint P;
  const tvdb_status_t st = vol_plan(g, options, &ix, &P, err);
  if (st != TVDB_OK) return st;
  tvdb_tree_index_destroy(&ix);
  vol_layout(&P, out);
  return TVDB_OK;
}

tvdb_status_t tvdb_grid_to_dense_volume_buffer(const tvdb_grid_t *g,
                                               const tvdb_dense_volume_options_t *options,
                                               float *data, size_t count,
                                               tvdb_dense_volume_t *layout, tvdb_error_t *err) {
  tvdb_tree_index ix;
  vol_paint P;
  tvdb_status_t st = vol_plan(g, options, &ix, &P, err);
  if (st != TVDB_OK) return st;
  const size_t need = P.cells * (size_t)vol_out_comps(&P); /* checked by vol_plan */
  if (count < need || (need && !data)) {
    tvdb_tree_index_destroy(&ix);
    return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "dense volume buffer too small");
  }
  st = vol_fill(&P, data, err);
  tvdb_tree_index_destroy(&ix);
  if (st == TVDB_OK && layout) vol_layout(&P, layout);
  return st;
}

tvdb_status_t tvdb_grid_to_dense_volume(const tvdb_grid_t *g,
                                        const tvdb_dense_volume_options_t *options,
                                        tvdb_dense_volume_t *out, tvdb_error_t *err) {
  if (!out) return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid dense volume arguments");
  tvdb_tree_index ix;
  vol_paint P;
  tvdb_status_t st = vol_plan(g, options, &ix, &P, err);
  if (st != TVDB_OK) return st;
  float *data = NULL;
  if (!P.empty) {
    const size_t bytes = P.cells * (size_t)vol_out_comps(&P) * sizeof(float); /* checked */
    if (!(data = (float *)malloc(bytes ? bytes : 1))) {
      tvdb_tree_index_destroy(&ix);
      return vol_error(err, TVDB_ERROR_OUT_OF_MEMORY, "dense volume allocation failed");
    }
    st = vol_fill(&P, data, err);
  }
  tvdb_tree_index_destroy(&ix);
  if (st != TVDB_OK) {
    free(data);
    return st;
  }
  tvdb_dense_volume_free(out);
  vol_layout(&P, out);
  out->data = data;
  return TVDB_OK;
}

/* ---- Sparse sampling ----------------------------------------------------- */
struct tvdb_volume_sampler {
  tvdb_tree_index ix;
  tvdb_value_type_t vt;
  int comps;
  int has_inverse;
  double inverse[3][4];
};

tvdb_status_t tvdb_volume_sampler_create(const tvdb_grid_t *g, tvdb_volume_sampler_t **out,
                                         tvdb_error_t *err) {
  if (!g || !out) return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid sampler arguments");
  *out = NULL;
  if (!vol_has_tree(g))
    return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "grid tree not decoded (tvdb_read_grid)");
  if (g->tree.is_point_data_grid || g->tree.is_point_index_grid)
    return vol_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE, "point grids have no voxel values");
  tvdb_volume_sampler_t *s = (tvdb_volume_sampler_t *)calloc(1, sizeof(*s));
  if (!s) return vol_error(err, TVDB_ERROR_OUT_OF_MEMORY, "sampler allocation failed");
  tvdb_status_t st = tvdb_tree_index_create(g, 0, &s->ix, err);
  if (st != TVDB_OK) {
    free(s);
    return st;
  }
  s->vt = g->tree.layout.levels[s->ix.levels - 1].value_type;
  if (!vol_supported(s->vt)) {
    tvdb_volume_sampler_destroy(s);
    return vol_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE, "unsupported voxel value type");
  }
  s->comps = vol_components(s->vt);
  double m[4][4];
  if (tvdb_tree_matrix(&g->transform, m)) {
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
      for (int b = 0; b < 3; ++b) s->inverse[a][b] = inv[a][b];
      s->inverse[a][3] = -(inv[a][0] * m[0][3] + inv[a][1] * m[1][3] + inv[a][2] * m[2][3]);
    }
    s->has_inverse = 1;
  }
  *out = s;
  return TVDB_OK;
}

void tvdb_volume_sampler_destroy(tvdb_volume_sampler_t *s) {
  if (!s) return;
  tvdb_tree_index_destroy(&s->ix);
  free(s);
}

int tvdb_volume_sampler_components(const tvdb_volume_sampler_t *s) { return s ? s->comps : 0; }

void tvdb_volume_sampler_get(const tvdb_volume_sampler_t *s, const int32_t ijk[3], float *value,
                             int *active) {
  const uint8_t *bytes;
  const tvdb_value_t *v;
  float tmp[3];
  tvdb_tree_lookup(&s->ix, ijk, &bytes, &v, active);
  if (bytes) vol_from_bytes(s->vt, bytes, tmp);
  else vol_from_value(v, tmp);
  for (int k = 0; k < s->comps; ++k) value[k] = tmp[k];
}

static int vol_index_ok(double q) { return q >= -2147483648.0 && q < 2147483647.0; }

static tvdb_status_t vol_sample(const tvdb_volume_sampler_t *s, const double *points, size_t count,
                                tvdb_volume_filter_t filter, int world, float *values,
                                tvdb_error_t *err) {
  if (!s || (count && (!points || !values)) ||
      (filter != TVDB_VOLUME_FILTER_NEAREST && filter != TVDB_VOLUME_FILTER_TRILINEAR))
    return vol_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid sample arguments");
  if (world && !s->has_inverse)
    return vol_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM, "grid transform is not invertible");
  const int C = s->comps;
  for (size_t i = 0; i < count; ++i) {
    const double *p = points + 3 * i;
    float *o = values + (size_t)C * i;
    double q[3];
    for (int a = 0; a < 3; ++a)
      q[a] = world ? s->inverse[a][3] + s->inverse[a][0] * p[0] + s->inverse[a][1] * p[1] +
                         s->inverse[a][2] * p[2]
                   : p[a];
    int ok = 1;
    for (int a = 0; a < 3; ++a) ok = ok && isfinite(q[a]) && vol_index_ok(floor(q[a]));
    if (!ok) {
      for (int k = 0; k < C; ++k) o[k] = NAN;
      continue;
    }
    if (filter == TVDB_VOLUME_FILTER_NEAREST) {
      const int32_t c[3] = {(int32_t)floor(q[0] + 0.5), (int32_t)floor(q[1] + 0.5),
                            (int32_t)floor(q[2] + 0.5)};
      tvdb_volume_sampler_get(s, c, o, NULL);
      continue;
    }
    int32_t base[3];
    double f[3];
    for (int a = 0; a < 3; ++a) {
      const double fl = floor(q[a]);
      base[a] = (int32_t)fl;
      f[a] = q[a] - fl;
    }
    double sum[3] = {0, 0, 0};
    for (int c = 0; c < 8; ++c) {
      const int32_t at[3] = {base[0] + (c & 1), base[1] + ((c >> 1) & 1), base[2] + ((c >> 2) & 1)};
      const double w = ((c & 1) ? f[0] : 1 - f[0]) * (((c >> 1) & 1) ? f[1] : 1 - f[1]) *
                       (((c >> 2) & 1) ? f[2] : 1 - f[2]);
      float v[3];
      tvdb_volume_sampler_get(s, at, v, NULL);
      for (int k = 0; k < C; ++k) sum[k] += w * (double)v[k];
    }
    for (int k = 0; k < C; ++k) o[k] = (float)sum[k];
  }
  return TVDB_OK;
}

tvdb_status_t tvdb_volume_sample_world(const tvdb_volume_sampler_t *s, const double *points,
                                       size_t count, tvdb_volume_filter_t filter, float *values,
                                       tvdb_error_t *err) {
  return vol_sample(s, points, count, filter, 1, values, err);
}
tvdb_status_t tvdb_volume_sample_index(const tvdb_volume_sampler_t *s, const double *points,
                                       size_t count, tvdb_volume_filter_t filter, float *values,
                                       tvdb_error_t *err) {
  return vol_sample(s, points, count, filter, 0, values, err);
}
