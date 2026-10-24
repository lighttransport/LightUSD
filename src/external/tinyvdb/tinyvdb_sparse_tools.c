#include "tinyvdb_sparse_tools.h"

#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_checked.h"
#include "tinyvdb_mesh_conversion.h"
#include "tinyvdb_tree_internal.h"
#include "tinyvdb_winding.h"

/* Complete coordinate keys; no 21-bit packing or bounding-box strides. */
typedef struct {
  int32_t c[3];
  union {
    float value;
    uint32_t id;
  };
  unsigned char active;
  /* Corner values from the surface scan; the mesher reuses these instead of
     re-querying the tree for every cell. */
  float corners[8];
} tool_voxel;
typedef struct {
  tool_voxel* v;
  size_t count, capacity, *slots, hash_capacity, limit, scans;
  tvdb_error_t* err;
} tool_map;
static size_t tool_limit(size_t n) { return n ? n : 1000000; }
static uint64_t tool_hash(const int32_t c[3]) {
  uint64_t h = (uint32_t)c[0] * UINT64_C(73856093) ^
               (uint32_t)c[1] * UINT64_C(19349663) ^
               (uint32_t)c[2] * UINT64_C(83492791);
  h ^= h >> 33;
  h *= UINT64_C(0xff51afd7ed558ccd);
  return h ^ (h >> 33);
}
static void tool_map_free(tool_map* m) {
  free(m->v);
  free(m->slots);
  memset(m, 0, sizeof(*m));
}
static size_t tool_find(const tool_map* m, const int32_t c[3]) {
  if (!m->hash_capacity) return SIZE_MAX;
  size_t s = (size_t)tool_hash(c) & (m->hash_capacity - 1);
  while (m->slots[s]) {
    size_t i = m->slots[s] - 1;
    if (!memcmp(c, m->v[i].c, sizeof(m->v[i].c))) return i;
    s = (s + 1) & (m->hash_capacity - 1);
  }
  return SIZE_MAX;
}
static tvdb_status_t tool_insert(tool_map* m, const int32_t c[3],
                                 size_t* index) {
  size_t i = tool_find(m, c);
  if (i != SIZE_MAX) {
    if (index) *index = i;
    return TVDB_OK;
  }
  if (m->count >= m->limit)
    return tvdb_tree_error(m->err, TVDB_ERROR_INVALID_ARGUMENT,
                           "sparse coordinate budget exceeded");
  if (m->count == m->capacity) {
    size_t n = m->capacity ? m->capacity : 32, bytes;
    if (m->capacity) {
      if (n > SIZE_MAX / 2) goto overflow;
      n *= 2;
    }
    if (n > m->limit) n = m->limit;
    if (!tvdb_size_mul(n, sizeof(tool_voxel), &bytes)) goto overflow;
    void* p = realloc(m->v, bytes);
    if (!p) goto oom;
    m->v = p;
    m->capacity = n;
  }
  if (!m->hash_capacity || m->count >= m->hash_capacity / 2) {
    size_t n, bytes;
    /* Size for four times the count, so the table doubles and the next
     * rehash waits for twice as many coordinates (amortized O(1) inserts).
     * Sizing for 4/3 of the count put the half-load trigger right behind
     * the new size and rehashed on almost every insert. */
    if (!tvdb_hash_capacity_scaled(m->count + 1, 4, 1, &n) ||
        !tvdb_size_mul(n, sizeof(size_t), &bytes))
      goto overflow;
    size_t* slots = calloc(1, bytes);
    if (!slots) goto oom;
    for (size_t j = 0; j < m->count; ++j) {
      size_t s = (size_t)tool_hash(m->v[j].c) & (n - 1);
      while (slots[s]) s = (s + 1) & (n - 1);
      slots[s] = j + 1;
    }
    free(m->slots);
    m->slots = slots;
    m->hash_capacity = n;
  }
  i = m->count++;
  memset(m->v + i, 0, sizeof(*m->v));
  memcpy(m->v[i].c, c, 3 * sizeof(int32_t));
  size_t s = (size_t)tool_hash(c) & (m->hash_capacity - 1);
  while (m->slots[s]) s = (s + 1) & (m->hash_capacity - 1);
  m->slots[s] = i + 1;
  if (index) *index = i;
  return TVDB_OK;
overflow:
  return tvdb_tree_error(m->err, TVDB_ERROR_INVALID_ARGUMENT,
                         "sparse allocation overflow");
oom:
  return tvdb_tree_error(m->err, TVDB_ERROR_OUT_OF_MEMORY,
                         "sparse workspace allocation failed");
}
static tvdb_status_t tool_scan(tool_map* m) {
  if (m->scans / 32 >= m->limit || m->scans == SIZE_MAX)
    return tvdb_tree_error(m->err, TVDB_ERROR_INVALID_ARGUMENT,
                           "sparse scan budget exceeded");
  ++m->scans;
  return TVDB_OK;
}
static int tool_mask(const tvdb_nodemask_t* m, size_t i) {
  return (m->bits.data[i / 8] >> (i % 8)) & 1;
}
static int tool_output(const tvdb_grid_t* g, const tvdb_grid_t* out) {
  return g && out && g != out &&
         (!out->tree.nodes || out->tree.nodes != g->tree.nodes);
}
static tvdb_status_t tool_validate(const tvdb_grid_t* g, tvdb_tree_index* p,
                                   tvdb_error_t* err) {
  tvdb_tree_diagnostics_t d;
  tvdb_status_t st =
      tvdb_grid_diagnose(g, TVDB_TREE_DIAG_GENERIC, 0, 0, &d, err);
  if (st != TVDB_OK) return st;
  if (!d.valid)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "nonfinite values or invalid affine transform");
  return tvdb_tree_index_create(g, 1, p, err);
}
static int tool_inverse(const double m[4][4], double inv[4][4]) {
  double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (!isfinite(d) || !d) return 0;
  memset(inv, 0, 16 * sizeof(double));
  inv[3][3] = 1;
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) {
      int r0 = (b + 1) % 3, r1 = (b + 2) % 3, c0 = (a + 1) % 3,
          c1 = (a + 2) % 3;
      inv[a][b] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / d;
    }
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) inv[a][3] -= inv[a][b] * m[b][3];
  for (int a = 0; a < 4; ++a)
    for (int b = 0; b < 4; ++b)
      if (!isfinite(inv[a][b])) return 0;
  return 1;
}
static void tool_transform(const double m[4][4], const double c[3],
                           double v[3]) {
  for (int a = 0; a < 3; ++a)
    v[a] = m[a][3] + m[a][0] * c[0] + m[a][1] * c[1] + m[a][2] * c[2];
}
static int tool_spacing(const double m[4][4], double h[3], int isotropic) {
  for (int a = 0; a < 3; ++a) {
    h[a] = hypot(hypot(m[0][a], m[1][a]), m[2][a]);
    if (!isfinite(h[a]) || h[a] <= 0 ||
        (isotropic && fabs(h[a] - h[0]) > 1e-12 * h[0]))
      return 0;
    for (int b = 0; b < a; ++b) {
      double dot = 0;
      for (int r = 0; r < 3; ++r) dot += (m[r][a] / h[a]) * (m[r][b] / h[b]);
      if (!isfinite(dot) || fabs(dot) > 1e-12) return 0;
    }
  }
  return 1;
}
typedef tvdb_status_t (*tool_region_fn)(const int32_t c[3], int64_t span,
                                        float value, int active, void* user);
/* Disjoint constant regions, including every stored inactive leaf sample. */
static tvdb_status_t tool_regions(const tvdb_tree_index* p, tool_region_fn fn,
                                  void* user) {
  const tvdb_tree_t* t = &p->grid->tree;
  const tvdb_root_node_t* r = &t->nodes[0].u.root;
  tvdb_status_t st;
  for (size_t i = 0; i < r->num_tiles; ++i) {
    st = fn(r->tile_origins + 3 * i, p->span[1], r->tile_values[i].u.f,
            r->tile_active[i], user);
    if (st != TVDB_OK) return st;
  }
  for (size_t i = 1; i < t->num_nodes; ++i) {
    const tvdb_tree_node_t* n = t->nodes + i;
    int L = t->layout.levels[n->level].log2dim;
    size_t slots = (size_t)1 << (3 * L);
    int leaf = n->type == TVDB_NODE_LEAF;
    int64_t span = leaf ? 1 : p->span[n->level + 1];
    const tvdb_nodemask_t* mask =
        leaf ? &n->u.leaf.value_mask : &n->u.internal.value_mask;
    const float* values =
        (const float*)(leaf ? n->u.leaf.data : n->u.internal.values);
    for (size_t s = 0; s < slots; ++s) {
      if (!leaf && tool_mask(&n->u.internal.child_mask, s)) continue;
      int32_t c[3];
      for (int a = 0; a < 3; ++a)
        c[a] =
            (int32_t)((int64_t)p->origins[i][a] +
                      (int64_t)((s >> ((2 - a) * L)) & ((1 << L) - 1)) * span);
      st = fn(c, span, values[s], tool_mask(mask, s), user);
      if (st != TVDB_OK) return st;
    }
  }
  return TVDB_OK;
}
static tvdb_status_t tool_build(const tvdb_grid_t* source, const tool_map* m,
                                float bg, const tvdb_transform_t* transform,
                                tvdb_grid_t* out, tvdb_error_t* err) {
  tvdb_sparse_grid sg = {0};
  tvdb_grid_t tmpl = {0}, generated = {0}, attrs = {0};
  tvdb_status_t st = TVDB_OK;
  tmpl.descriptor.grid_type = (char*)"Tree_float_5_4_3";
  tmpl.transform = *transform;
  tmpl.tree.layout.num_levels = 4;
  const int dims[4] = {0, 5, 4, 3};
  for (int l = 0; l < 4; ++l) {
    tmpl.tree.layout.levels[l].log2dim = dims[l];
    tmpl.tree.layout.levels[l].value_type = TVDB_VALUE_FLOAT;
    tmpl.tree.layout.levels[l].node_type = l == 0   ? TVDB_NODE_ROOT
                                           : l == 3 ? TVDB_NODE_LEAF
                                                    : TVDB_NODE_INTERNAL;
  }
  if (m->count && !tvdb_sparse_grid_reserve(&sg, m->count)) goto oom;
  sg.count = m->count;
  for (size_t i = 0; i < m->count; ++i) {
    sg.coords[i] = (tvdb_vec3i){m->v[i].c[0], m->v[i].c[1], m->v[i].c[2]};
    sg.values[i] = m->v[i].value;
  }
  if (!tvdb_grid_from_sparse_using_template(
          &tmpl, &sg, source->descriptor.grid_name, bg, &generated))
    goto oom;
  {
    const int nlevels = generated.tree.layout.num_levels;
    int64_t level_span[TVDB_MAX_TREE_DEPTH];  // voxels/axis covered by a node at level l
    for (int l = 0; l < nlevels; ++l) {
      int64_t sh = 0;
      for (int k = l; k < nlevels; ++k)
        sh += generated.tree.layout.levels[k].log2dim;
      level_span[l] = (int64_t)1 << sh;
    }
    #define TVDB_MASK_ON(m_, bit_)                                      \
      ((int)(((m_).bits.data[(bit_) / 8] >> ((bit_) % 8)) & 1u))
    for (size_t j = 0; j < m->count; ++j) {
      if (m->v[j].active) continue;
      size_t node = 0;
      for (;;) {
        tvdb_tree_node_t* n = generated.tree.nodes + node;
        const int L = generated.tree.layout.levels[n->level].log2dim;
        if (n->type == TVDB_NODE_LEAF) {
          size_t s = 0;
          for (int a = 0; a < 3; ++a)
            s = (s << L) | (size_t)(m->v[j].c[a] - n->origin[a]);
          if (s < (size_t)1 << (3 * L))
            n->u.leaf.value_mask.bits.data[s / 8] &=
                (unsigned char)~(1u << (s % 8));
          break;
        }
        const int64_t cspan = level_span[n->level + 1];
        size_t s = 0;
        int hit = 1;
        for (int a = 0; a < 3; ++a) {
          int64_t d = (int64_t)m->v[j].c[a] - n->origin[a];
          if (d < 0 || d >= ((int64_t)1 << L) * cspan) hit = 0;
          s = (s << L) | (size_t)(d / cspan);
        }
        if (n->type == TVDB_NODE_ROOT) {
          tvdb_root_node_t* r = &n->u.root;
          int matched = 0;
          for (size_t c = 0; c < r->num_children; ++c) {
            int chit = 1;
            int32_t o[3];
            memcpy(o, (const int32_t (*)[3])r->child_origins + c, sizeof o);
            for (int a = 0; a < 3; ++a)
              if (m->v[j].c[a] < o[a] ||
                  m->v[j].c[a] >= o[a] + (int32_t)cspan)
                chit = 0;
            if (chit) { node = r->child_indices[c]; matched = 1; break; }
          }
          if (!matched) break;
          continue;
        }
        if (!hit) break;
        const tvdb_internal_node_t* in = &n->u.internal;
        if (!TVDB_MASK_ON(in->child_mask, s)) {
          /* Value stored at this internal node: clear our bit there. */
          in->value_mask.bits.data[s / 8] &= (unsigned char)~(1u << (s % 8));
          break;
        }
        node = in->child_indices[tvdb_tree_mask_rank(&in->child_mask, s)];
      }
    }
    #undef tvdb_level_span
    #undef TVDB_MASK_ON
  }
  st = tvdb_tree_copy_attributes(source, &attrs, err);
  if (st != TVDB_OK) goto done;
  attrs.transform = *transform;
  for (size_t i = 0; i < attrs.metadata.count;) {
    tvdb_meta_entry_t* e = attrs.metadata.entries + i;
    const char* name = e->name;
    if (name &&
        (!strcmp(name, "file_bbox_min") || !strcmp(name, "file_bbox_max") ||
         !strcmp(name, "file_voxel_count") ||
         !strcmp(name, "file_mem_bytes"))) {
      free(e->name);
      free(e->type_name);
      free(e->raw_data);
      if (e->value.type == TVDB_VALUE_STRING) free(e->value.u.s.str);
      memmove(e, e + 1, (attrs.metadata.count - i - 1) * sizeof(*e));
      --attrs.metadata.count;
    } else
      ++i;
  }
  free(attrs.descriptor.grid_type);
  attrs.descriptor.grid_type = generated.descriptor.grid_type;
  generated.descriptor.grid_type = NULL;
  attrs.tree = generated.tree;
  memset(&generated.tree, 0, sizeof(generated.tree));
  *out = attrs;
  memset(&attrs, 0, sizeof(attrs));
  goto done;
oom:
  st = tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                       "sparse tree construction failed");
done:
  tvdb_sparse_grid_free(&sg);
  tvdb_grid_destroy_owned(&generated);
  tvdb_grid_destroy_owned(&attrs);
  return st;
}
static float tool_get64(const tvdb_tree_index* p, const int64_t c[3],
                        int* active) {
  int32_t q[3];
  for (int a = 0; a < 3; ++a)
    if (c[a] < INT32_MIN || c[a] > INT32_MAX) {
      if (active) *active = 0;
      return p->grid->tree.nodes[0].u.root.background.u.f;
    } else
      q[a] = (int32_t)c[a];
  return tvdb_tree_get(p, q, active);
}
static double tool_sample(const tvdb_tree_index* p, const double c[3],
                          int linear, int* active) {
  double bg = p->grid->tree.nodes[0].u.root.background.u.f;
  *active = 0;
  for (int a = 0; a < 3; ++a)
    if (!isfinite(c[a]) || c[a] < (double)INT32_MIN - 1 ||
        c[a] > (double)INT32_MAX + 1)
      return bg;
  int64_t base[3];
  double f[3];
  for (int a = 0; a < 3; ++a) {
    base[a] = (int64_t)floor(c[a] + (linear ? 0 : 0.5));
    f[a] = c[a] - base[a];
  }
  if (!linear) return tool_get64(p, base, active);
  double value = 0;
  for (int i = 0; i < 8; ++i) {
    int64_t q[3];
    double w = 1;
    for (int a = 0; a < 3; ++a) {
      int b = (i >> a) & 1;
      q[a] = base[a] + b;
      w *= b ? f[a] : 1 - f[a];
    }
    if (w == 0) continue;
    int on;
    value += w * tool_get64(p, q, &on);
    *active |= on;
  }
  return value;
}
typedef struct {
  tool_map* m;
  double forward[4][4];
  float bg;
  int linear;
} tool_resample;
static tvdb_status_t tool_resample_region(const int32_t c[3], int64_t span,
                                          float v, int active, void* user) {
  tool_resample* r = user;
  if (!active && !memcmp(&v, &r->bg, sizeof(v))) return TVDB_OK;
  double lo[3] = {INFINITY, INFINITY, INFINITY},
         hi[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (int i = 0; i < 8; ++i) {
    double p[3], q[3];
    for (int a = 0; a < 3; ++a)
      p[a] = (double)c[a] + (((i >> a) & 1)
                                 ? (double)span - 1 + (r->linear ? 1 : 0.5)
                                 : -(r->linear ? 1 : 0.5));
    tool_transform(r->forward, p, q);
    for (int a = 0; a < 3; ++a) {
      if (!isfinite(q[a]))
        return tvdb_tree_error(r->m->err, TVDB_ERROR_INVALID_ARGUMENT,
                               "resampled extent overflow");
      lo[a] = fmin(lo[a], q[a]);
      hi[a] = fmax(hi[a], q[a]);
    }
  }
  int64_t begin[3], end[3];
  for (int a = 0; a < 3; ++a) {
    lo[a] = ceil(lo[a]);
    hi[a] = floor(hi[a]);
    if (lo[a] > hi[a]) return TVDB_OK;
    if (lo[a] < INT32_MIN || hi[a] > INT32_MAX)
      return tvdb_tree_error(r->m->err, TVDB_ERROR_INVALID_ARGUMENT,
                             "resampled support outside int32");
    begin[a] = (int64_t)ceil(lo[a]);
    end[a] = (int64_t)floor(hi[a]);
  }
  for (int64_t x = begin[0]; x <= end[0]; ++x)
    for (int64_t y = begin[1]; y <= end[1]; ++y)
      for (int64_t z = begin[2]; z <= end[2]; ++z) {
        int32_t q[3] = {(int32_t)x, (int32_t)y, (int32_t)z};
        tvdb_status_t st = tool_scan(r->m);
        if (st == TVDB_OK) st = tool_insert(r->m, q, NULL);
        if (st != TVDB_OK) return st;
      }
  return TVDB_OK;
}
tvdb_status_t tvdb_grid_resample(const tvdb_grid_t* source,
                                 const tvdb_transform_t* target,
                                 tvdb_tree_sampler_t sampler, size_t budget,
                                 tvdb_grid_t* out, tvdb_error_t* err) {
  double sm[4][4], tm[4][4], si[4][4], ti[4][4];
  if (!tool_output(source, out) || !target ||
      (sampler != TVDB_SAMPLE_NEAREST && sampler != TVDB_SAMPLE_LINEAR) ||
      !tvdb_tree_matrix(target, tm) || !tool_inverse(tm, ti))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid resample arguments");
  tvdb_tree_index p = {0};
  tvdb_grid_t generated = {0}, pruned = {0};
  tool_map m = {0};
  m.limit = tool_limit(budget);
  m.err = err;
  tvdb_status_t st = tool_validate(source, &p, err);
  if (st != TVDB_OK) goto done;
  if (!tvdb_tree_matrix(&source->transform, sm) || !tool_inverse(sm, si)) {
    st = tvdb_tree_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
                         "cannot invert source transform");
    goto done;
  }
  tool_resample r = {0};
  r.m = &m;
  r.bg = source->tree.nodes[0].u.root.background.u.f;
  r.linear = sampler == TVDB_SAMPLE_LINEAR;
  for (int a = 0; a < 4; ++a)
    for (int b = 0; b < 4; ++b)
      for (int k = 0; k < 4; ++k) r.forward[a][b] += ti[a][k] * sm[k][b];
  st = tool_regions(&p, tool_resample_region, &r);
  if (st != TVDB_OK) goto done;
  for (size_t i = 0; i < m.count; ++i) {
    double c[3] = {m.v[i].c[0], m.v[i].c[1], m.v[i].c[2]}, w[3], q[3];
    int active;
    tool_transform(tm, c, w);
    tool_transform(si, w, q);
    for (int a = 0; a < 3; ++a)
      if (!isfinite(w[a]) || !isfinite(q[a])) {
        st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "resample coordinate overflow");
        goto done;
      }
    double value = tool_sample(&p, q, r.linear, &active);
    if (!isfinite(value) || fabs(value) > FLT_MAX) {
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "resample value overflow");
      goto done;
    }
    m.v[i].value = (float)value;
    m.v[i].active = (unsigned char)active;
  }
  st = tool_build(source, &m, r.bg, target, &generated, err);
  if (st == TVDB_OK) st = tvdb_grid_prune(&generated, &pruned, err);
  if (st == TVDB_OK) {
    tvdb_grid_destroy_owned(out);
    *out = pruned;
    memset(&pruned, 0, sizeof(pruned));
  }
done:
  tool_map_free(&m);
  tvdb_tree_index_destroy(&p);
  tvdb_grid_destroy_owned(&generated);
  tvdb_grid_destroy_owned(&pruned);
  return st;
}

static const int tool_corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0},
                                       {0, 1, 0}, {0, 0, 1}, {1, 0, 1},
                                       {1, 1, 1}, {0, 1, 1}};
static const int tool_edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0},
                                      {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                      {0, 4}, {1, 5}, {2, 6}, {3, 7}};
typedef struct {
  const tvdb_tree_index* p;
  tool_map* cells;
  float iso;
  int bg_below;
} tool_surface;
static int tool_cube(const tvdb_tree_index* p, const int64_t c[3], float iso,
                     float values[8]) {
  int cube = 0;
  for (int i = 0; i < 8; ++i) {
    int64_t q[3];
    for (int a = 0; a < 3; ++a) q[a] = c[a] + tool_corners[i][a];
    float v = tool_get64(p, q, NULL);
    if (values) values[i] = v;
    if (v < iso) cube |= 1 << i;
  }
  return cube;
}
static tvdb_status_t tool_cell(tool_surface* s, const int64_t c[3]) {
  tvdb_status_t st = tool_scan(s->cells);
  if (st != TVDB_OK) return st;
  float corners[8];
  int cube = tool_cube(s->p, c, s->iso, corners);
  if (cube == 0 || cube == 255) return TVDB_OK;
  int32_t q[3];
  for (int a = 0; a < 3; ++a) {
    if (c[a] < INT32_MIN || c[a] > INT32_MAX)
      return tvdb_tree_error(s->cells->err, TVDB_ERROR_INVALID_ARGUMENT,
                             "surface cell outside int32");
    q[a] = (int32_t)c[a];
  }
  size_t i;
  st = tool_insert(s->cells, q, &i);
  if (st == TVDB_OK) {
    s->cells->v[i].active = (unsigned char)cube;
    memcpy(s->cells->v[i].corners, corners, sizeof corners);
  }
  return st;
}
static tvdb_status_t tool_surface_region(const int32_t c[3], int64_t span,
                                         float value, int active, void* user) {
  (void)active;
  tool_surface* s = user;
  if ((value < s->iso) == s->bg_below) return TVDB_OK;
  if (span == 1) {
    for (int i = 0; i < 8; ++i) {
      int64_t q[3];
      for (int a = 0; a < 3; ++a) q[a] = (int64_t)c[a] - ((i >> a) & 1);
      tvdb_status_t st = tool_cell(s, q);
      if (st != TVDB_OK) return st;
    }
  } else {
    /* Constant-region interiors cannot cross; visit only six boundary slabs. */
    for (int a = 0; a < 3; ++a)
      for (int side = 0; side < 2; ++side) {
        int b = (a + 1) % 3, d = (a + 2) % 3;
        for (int64_t u = -1; u < span; ++u)
          for (int64_t v = -1; v < span; ++v) {
            int64_t q[3];
            q[a] = (int64_t)c[a] + (side ? span - 1 : -1);
            q[b] = (int64_t)c[b] + u;
            q[d] = (int64_t)c[d] + v;
            tvdb_status_t st = tool_cell(s, q);
            if (st != TVDB_OK) return st;
          }
      }
  }
  return TVDB_OK;
}
static int tool_compare_voxel(const void* a, const void* b) {
  const tool_voxel *x = a, *y = b;
  for (int i = 0; i < 3; ++i)
    if (x->c[i] != y->c[i]) return x->c[i] < y->c[i] ? -1 : 1;
  return 0;
}
static tvdb_status_t tool_surface_cells(const tvdb_tree_index* p, float iso,
                                        tool_map* cells) {
  tool_surface s = {p, cells, iso,
                    p->grid->tree.nodes[0].u.root.background.u.f < iso};
  tvdb_status_t st = tool_regions(p, tool_surface_region, &s);
  if (st == TVDB_OK && cells->count > 1)
    qsort(cells->v, cells->count, sizeof(tool_voxel), tool_compare_voxel);
  return st;
}
static tvdb_status_t tool_grow(void** ptr, size_t* capacity, size_t count,
                               size_t width, tvdb_error_t* err) {
  if (count <= *capacity) return TVDB_OK;
  size_t n = *capacity ? *capacity : 32, bytes;
  while (n < count) {
    if (n > SIZE_MAX / 2)
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                             "mesh capacity overflow");
    n *= 2;
  }
  if (!tvdb_size_mul(n, width, &bytes))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "mesh size overflow");
  void* p = realloc(*ptr, bytes);
  if (!p)
    return tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                           "mesh allocation failed");
  *ptr = p;
  *capacity = n;
  return TVDB_OK;
}
typedef struct {
  int64_t c[3];
  size_t vertex;
} tool_cluster;
static int tool_compare_cluster(const void* a, const void* b) {
  const tool_cluster *x = a, *y = b;
  for (int i = 0; i < 3; ++i)
    if (x->c[i] != y->c[i]) return x->c[i] < y->c[i] ? -1 : 1;
  return x->vertex < y->vertex ? -1 : x->vertex != y->vertex;
}
static tvdb_status_t tool_adapt(tvdb_triangle_mesh* mesh, double tolerance,
                                tvdb_error_t* err) {
  if (!tolerance || !mesh->vertex_count) return TVDB_OK;
  size_t bytes;
  if (!tvdb_size_mul(mesh->vertex_count, sizeof(tool_cluster), &bytes))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "cluster size overflow");
  tool_cluster* clusters = malloc(bytes);
  uint32_t* remap = NULL;
  tvdb_vec3f* vertices = NULL;
  tvdb_status_t st = TVDB_OK;
  if (!tvdb_size_mul(mesh->vertex_count, sizeof(uint32_t), &bytes)) {
    st = TVDB_ERROR_INVALID_ARGUMENT;
    goto done;
  }
  remap = malloc(bytes);
  if (!tvdb_size_mul(mesh->vertex_count, sizeof(tvdb_vec3f), &bytes)) {
    st = TVDB_ERROR_INVALID_ARGUMENT;
    goto done;
  }
  vertices = malloc(bytes);
  if (!clusters || !remap || !vertices) {
    st = tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                         "adaptive clustering allocation failed");
    goto done;
  }
  double step = tolerance / sqrt(3.0);
  if (!isfinite(step) || step <= 0) {
    st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                         "invalid cluster scale");
    goto done;
  }
  for (size_t i = 0; i < mesh->vertex_count; ++i) {
    double v[3] = {mesh->vertices[i].x, mesh->vertices[i].y,
                   mesh->vertices[i].z};
    clusters[i].vertex = i;
    for (int a = 0; a < 3; ++a) {
      double q = floor(v[a] / step);
      if (!isfinite(q) || q < -9223372036854775808.0 ||
          q >= 9223372036854775808.0) {
        st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                             "cluster coordinate overflow");
        goto done;
      }
      clusters[i].c[a] = (int64_t)q;
    }
  }
  qsort(clusters, mesh->vertex_count, sizeof(tool_cluster),
        tool_compare_cluster);
  size_t used = 0;
  for (size_t i = 0; i < mesh->vertex_count;) {
    size_t end = i + 1;
    while (end < mesh->vertex_count &&
           !memcmp(clusters[i].c, clusters[end].c, sizeof(clusters[i].c)))
      ++end;
    double sum[3] = {0};
    for (size_t j = i; j < end; ++j) {
      tvdb_vec3f v = mesh->vertices[clusters[j].vertex];
      sum[0] += v.x;
      sum[1] += v.y;
      sum[2] += v.z;
    }
    tvdb_vec3f avg = {(float)(sum[0] / (end - i)), (float)(sum[1] / (end - i)),
                      (float)(sum[2] / (end - i))};
    int safe = 1;
    for (size_t j = i; j < end; ++j) {
      tvdb_vec3f v = mesh->vertices[clusters[j].vertex];
      if (hypot(hypot((double)v.x - avg.x, (double)v.y - avg.y),
                (double)v.z - avg.z) > tolerance)
        safe = 0;
    }
    if (safe) {
      vertices[used] = avg;
      for (size_t j = i; j < end; ++j)
        remap[clusters[j].vertex] = (uint32_t)used;
      ++used;
    } else
      for (size_t j = i; j < end; ++j) {
        size_t old = clusters[j].vertex;
        vertices[used] = mesh->vertices[old];
        remap[old] = (uint32_t)used++;
      }
    i = end;
  }
  size_t faces = 0;
  for (size_t i = 0; i < mesh->face_count; ++i) {
    tvdb_triangle f = mesh->faces[i];
    f.v0 = remap[f.v0];
    f.v1 = remap[f.v1];
    f.v2 = remap[f.v2];
    if (f.v0 != f.v1 && f.v1 != f.v2 && f.v0 != f.v2) mesh->faces[faces++] = f;
  }
  memcpy(mesh->vertices, vertices, used * sizeof(tvdb_vec3f));
  mesh->vertex_count = used;
  mesh->face_count = faces;
done:
  free(clusters);
  free(remap);
  free(vertices);
  return st;
}
static tvdb_status_t tool_mesh(const tvdb_tree_index* p, const tool_map* cells,
                               float iso, double adapt,
                               tvdb_triangle_mesh* mesh, tvdb_error_t* err) {
  tool_map edges[3] = {{0}};
  double matrix[4][4];
  tvdb_tree_matrix(&p->grid->transform, matrix);
  tvdb_status_t st = TVDB_OK;
  size_t edge_limit;
  if (!tvdb_size_mul(cells->limit, 12, &edge_limit))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "edge budget overflow");
  for (int a = 0; a < 3; ++a) {
    edges[a].limit = edge_limit;
    edges[a].err = err;
  }
  double det = matrix[0][0] *
                   (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) -
               matrix[0][1] *
                   (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0]) +
               matrix[0][2] *
                   (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
  const int *table = tvdb_mc_tri_table_flat(),
            *edge_table = tvdb_mc_edge_table();
  for (size_t i = 0; i < cells->count; ++i) {
    int64_t c[3] = {cells->v[i].c[0], cells->v[i].c[1], cells->v[i].c[2]};
    float values[8];
    memcpy(values, cells->v[i].corners, sizeof values);
    int cube = cells->v[i].active;
    uint32_t vertex[12] = {0};
    for (int e = 0; e < 12; ++e)
      if (edge_table[cube] & (1 << e)) {
        int a = tool_edges[e][0], b = tool_edges[e][1], axis = 0;
        int32_t key[3];
        for (int k = 0; k < 3; ++k) {
          int64_t q = c[k] + (tool_corners[a][k] < tool_corners[b][k]
                                  ? tool_corners[a][k]
                                  : tool_corners[b][k]);
          if (q < INT32_MIN || q > INT32_MAX) {
            st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                                 "surface edge outside int32");
            goto done;
          }
          key[k] = (int32_t)q;
          if (tool_corners[a][k] != tool_corners[b][k]) axis = k;
        }
        size_t at = tool_find(edges + axis, key);
        if (at == SIZE_MAX) {
          if (mesh->vertex_count >= UINT32_MAX) {
            st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                                 "mesh index overflow");
            goto done;
          }
          st = tool_insert(edges + axis, key, &at);
          if (st != TVDB_OK) goto done;
          /* Store vertex IDs in the hash slot, not a float value. */
          st = tool_grow((void**)&mesh->vertices, &mesh->vertex_capacity,
                         mesh->vertex_count + 1, sizeof(tvdb_vec3f), err);
          if (st != TVDB_OK) goto done;
          double t = ((double)iso - values[a]) /
                     ((double)values[b] - values[a]),
                 q[3], w[3];
          t = fmax(0, fmin(1, t));
          for (int k = 0; k < 3; ++k)
            q[k] = c[k] + tool_corners[a][k] +
                   t * (tool_corners[b][k] - tool_corners[a][k]);
          tool_transform(matrix, q, w);
          for (int k = 0; k < 3; ++k)
            if (!isfinite(w[k]) || fabs(w[k]) > FLT_MAX) {
              st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                                   "mesh world coordinate overflow");
              goto done;
            }
          edges[axis].v[at].id = (uint32_t)mesh->vertex_count;
          mesh->vertices[mesh->vertex_count++] =
              (tvdb_vec3f){(float)w[0], (float)w[1], (float)w[2]};
        }
        vertex[e] = edges[axis].v[at].id;
      }
    const int* tri = table + 16 * cube;
    for (int t = 0; t < 16 && tri[t] >= 0; t += 3) {
      st = tool_grow((void**)&mesh->faces, &mesh->face_capacity,
                     mesh->face_count + 1, sizeof(tvdb_triangle), err);
      if (st != TVDB_OK) goto done;
      int flip = det > 0;
      mesh->faces[mesh->face_count++] =
          (tvdb_triangle){vertex[tri[t]], vertex[tri[t + (flip ? 2 : 1)]],
                          vertex[tri[t + (flip ? 1 : 2)]]};
    }
  }
  st = tool_adapt(mesh, adapt, err);
done:
  for (int a = 0; a < 3; ++a) tool_map_free(edges + a);
  return st;
}
tvdb_status_t tvdb_grid_volume_to_mesh(const tvdb_grid_t* source, float iso,
                                       double adapt, size_t budget,
                                       tvdb_triangle_mesh* out,
                                       tvdb_error_t* err) {
  if (!source || !out || !isfinite(iso) || !isfinite(adapt) || adapt < 0 ||
      out->vertex_count > out->vertex_capacity ||
      out->face_count > out->face_capacity ||
      (out->vertex_capacity && !out->vertices) ||
      (out->face_capacity && !out->faces))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid sparse meshing arguments");
  size_t vb, fb;
  if (!tvdb_size_mul(out->vertex_capacity, sizeof(tvdb_vec3f), &vb) ||
      !tvdb_size_mul(out->face_capacity, sizeof(tvdb_triangle), &fb) ||
      tvdb_buffers_overlap(out->vertices, vb, out->faces, fb))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid overlapping mesh storage");
  tvdb_tree_index p = {0};
  tool_map cells = {0};
  cells.limit = tool_limit(budget);
  cells.err = err;
  tvdb_triangle_mesh mesh = {0};
  tvdb_status_t st = tool_validate(source, &p, err);
  if (st != TVDB_OK) goto done;
  for (size_t i = 1; i < source->tree.num_nodes; ++i) {
    const tvdb_tree_node_t* n = source->tree.nodes + i;
    const void* data = n->type == TVDB_NODE_LEAF ? (void*)n->u.leaf.data
                                                 : (void*)n->u.internal.values;
    size_t bytes = n->type == TVDB_NODE_LEAF ? n->u.leaf.data_size
                                             : n->u.internal.values_size;
    if (tvdb_buffers_overlap(data, bytes, out->vertices, vb) ||
        tvdb_buffers_overlap(data, bytes, out->faces, fb)) {
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "mesh output overlaps source storage");
      goto done;
    }
  }
  st = tool_surface_cells(&p, iso, &cells);
  if (st == TVDB_OK) st = tool_mesh(&p, &cells, iso, adapt, &mesh, err);
  if (st == TVDB_OK) {
    tvdb_triangle_mesh_free(out);
    *out = mesh;
    memset(&mesh, 0, sizeof(mesh));
  }
done:
  tool_map_free(&cells);
  tvdb_tree_index_destroy(&p);
  tvdb_triangle_mesh_free(&mesh);
  return st;
}
/* Set (or add) string metadata `key`. */
static tvdb_status_t tool_set_string(tvdb_grid_t* g, const char* key,
                                     const char* text, tvdb_error_t* err) {
  size_t i = 0;
  for (; i < g->metadata.count; ++i)
    if (g->metadata.entries[i].name &&
        !strcmp(g->metadata.entries[i].name, key))
      break;
  char *name = malloc(strlen(key) + 1), *type = malloc(7),
       *value = malloc(strlen(text) + 1);
  if (!name || !type || !value) {
    free(name);
    free(type);
    free(value);
    return tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                           "metadata allocation failed");
  }
  memcpy(name, key, strlen(key) + 1);
  memcpy(type, "string", 7);
  memcpy(value, text, strlen(text) + 1);
  if (i == g->metadata.count) {
    size_t bytes;
    if (i == SIZE_MAX ||
        !tvdb_size_mul(i + 1, sizeof(tvdb_meta_entry_t), &bytes)) {
      free(name);
      free(type);
      free(value);
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                             "metadata size overflow");
    }
    void* p = realloc(g->metadata.entries, bytes);
    if (!p) {
      free(name);
      free(type);
      free(value);
      return tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                             "metadata allocation failed");
    }
    g->metadata.entries = p;
    g->metadata.count = g->metadata.capacity = i + 1;
    memset(g->metadata.entries + i, 0, sizeof(tvdb_meta_entry_t));
  }
  tvdb_meta_entry_t* e = g->metadata.entries + i;
  free(e->name);
  free(e->type_name);
  free(e->raw_data);
  if (e->value.type == TVDB_VALUE_STRING) free(e->value.u.s.str);
  memset(e, 0, sizeof(*e));
  e->name = name;
  e->type_name = type;
  e->value.type = TVDB_VALUE_STRING;
  e->value.u.s.str = value;
  e->value.u.s.len = strlen(text);
  return TVDB_OK;
}
tvdb_status_t tvdb_grid_level_set_rebuild(
    const tvdb_grid_t* source, float iso, float outside, float inside,
    size_t budget, tvdb_grid_t* out, tvdb_level_set_rebuild_result_t* result,
    tvdb_error_t* err) {
  if (!tool_output(source, out) || !isfinite(iso) || !isfinite(outside) ||
      !isfinite(inside) || outside <= 0 || inside <= 0)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid rebuild arguments");
  tvdb_tree_index p = {0};
  tool_map cells = {0}, voxels = {0}, band = {0};
  tvdb_triangle_mesh mesh = {0};
  tvdb_mesh_sdf_t* query = NULL;
  tvdb_grid_t generated = {0}, filled = {0}, pruned = {0};
  tvdb_level_set_rebuild_result_t report = {0};
  tvdb_status_t st = tool_validate(source, &p, err);
  cells.limit = voxels.limit = band.limit = tool_limit(budget);
  cells.err = voxels.err = band.err = err;
  if (st != TVDB_OK) goto done;
  double matrix[4][4], h[3];
  tvdb_tree_matrix(&source->transform, matrix);
  if (!tool_spacing(matrix, h, 1)) {
    st = tvdb_tree_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
                         "rebuild requires isotropic orthogonal axes");
    goto done;
  }
  if (source->tree.nodes[0].u.root.background.u.f <= iso) {
    st = tvdb_tree_error(
        err, TVDB_ERROR_INVALID_DATA,
        "rebuild requires positive exterior relative to isovalue");
    goto done;
  }
  st = tool_surface_cells(&p, iso, &cells);
  if (st != TVDB_OK) goto done;
  report.surface_cells = cells.count;
  if (cells.count) {
    st = tool_mesh(&p, &cells, iso, 0, &mesh, err);
    if (st != TVDB_OK) goto done;
    st = tvdb_mesh_sdf_create(&mesh, &query, err);
    if (st != TVDB_OK) goto done;
    for (size_t i = 0; i < cells.count; ++i)
      for (int k = 0; k < 8; ++k) {
        int32_t c[3];
        for (int a = 0; a < 3; ++a) {
          int64_t q = (int64_t)cells.v[i].c[a] + tool_corners[k][a];
          if (q > INT32_MAX) {
            st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                                 "rebuild coordinate overflow");
            goto done;
          }
          c[a] = (int32_t)q;
        }
        st = tool_insert(&voxels, c, NULL);
        if (st != TVDB_OK) goto done;
      }
    double layers = ceil(sqrt(3.0) * fmax(outside, inside) / h[0]) + 2;
    if (!isfinite(layers) || layers > voxels.limit) {
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "rebuild band exceeds budget");
      goto done;
    }
    size_t begin = 0;
    for (size_t layer = 0; layer < (size_t)layers; ++layer) {
      size_t end = voxels.count;
      for (size_t i = begin; i < end; ++i) {
        int32_t base[3];
        memcpy(base, voxels.v[i].c, sizeof(base));
        for (int a = 0; a < 3; ++a)
          for (int dir = -1; dir <= 1; dir += 2) {
            int32_t c[3];
            memcpy(c, base, sizeof(c));
            int64_t q = (int64_t)c[a] + dir;
            if (q < INT32_MIN || q > INT32_MAX) {
              st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                                   "rebuild support outside int32");
              goto done;
            }
            c[a] = (int32_t)q;
            st = tool_insert(&voxels, c, NULL);
            if (st != TVDB_OK) goto done;
          }
      }
      begin = end;
      if (begin == voxels.count) break;
    }
    report.candidate_voxels = voxels.count;
    for (size_t i = 0; i < voxels.count; ++i) {
      double c[3] = {voxels.v[i].c[0], voxels.v[i].c[1], voxels.v[i].c[2]},
             w[3], distance;
      tool_transform(matrix, c, w);
      st = tvdb_mesh_sdf_distance(query, w, &distance, err);
      if (st != TVDB_OK) goto done;
      int negative = tvdb_tree_get(&p, voxels.v[i].c, NULL) < iso;
      float d = (float)distance, width = negative ? inside : outside;
      if (d < width) {
        size_t j;
        st = tool_insert(&band, voxels.v[i].c, &j);
        if (st != TVDB_OK) goto done;
        band.v[j].value = negative ? -d : d;
        band.v[j].active = 1;
        report.max_distance = fmax(report.max_distance, d);
      }
    }
  }
  report.active_voxels = band.count;
  st = tool_build(source, &band, outside, &source->transform, &generated, err);
  if (st == TVDB_OK) st = tool_set_string(&generated, "class", "level set", err);
  if (st == TVDB_OK)
    st =
        tvdb_grid_signed_flood_fill(&generated, outside, -inside, &filled, err);
  if (st == TVDB_OK) st = tvdb_grid_prune(&filled, &pruned, err);
  if (st == TVDB_OK) {
    tvdb_grid_destroy_owned(out);
    *out = pruned;
    memset(&pruned, 0, sizeof(pruned));
    if (result) *result = report;
  }
done:
  tool_map_free(&cells);
  tool_map_free(&voxels);
  tool_map_free(&band);
  tvdb_tree_index_destroy(&p);
  tvdb_mesh_sdf_destroy(query);
  tvdb_triangle_mesh_free(&mesh);
  tvdb_grid_destroy_owned(&generated);
  tvdb_grid_destroy_owned(&filled);
  tvdb_grid_destroy_owned(&pruned);
  return st;
}
/* ---- Mesh to narrow-band level set ------------------------------------ */
typedef struct {
  const tool_voxel* voxels;
  float* distance;
  double* winding;
  const tvdb_mesh_sdf_t* query;
  const tvdb_winding_t* wn;
  double h;
  atomic_int bad;
} tool_level_set_job;
static void tool_level_set_range(size_t begin, size_t end, void* user) {
  tool_level_set_job* job = (tool_level_set_job*)user;
  for (size_t i = begin; i < end; ++i) {
    const double w[3] = {job->voxels[i].c[0] * job->h,
                         job->voxels[i].c[1] * job->h,
                         job->voxels[i].c[2] * job->h};
    double d = 0;
    if (tvdb_mesh_sdf_distance(job->query, w, &d, NULL) != TVDB_OK ||
        !isfinite(d)) {
      atomic_store_explicit(&job->bad, 1, memory_order_relaxed);
      continue;
    }
    const double wn = tvdb_winding_eval(job->wn, w);
    if (!isfinite(wn)) atomic_store_explicit(&job->bad, 1, memory_order_relaxed);
    job->distance[i] = (float)d;
    job->winding[i] = wn;
  }
}
/* Index of the voxel nearest a world point, or 0 outside int32. */
static int tool_nearest_voxel(const double p[3], double inv_h, int32_t c[3]) {
  for (int a = 0; a < 3; ++a) {
    const double q = floor(p[a] * inv_h + 0.5);
    if (!(q >= INT32_MIN && q <= INT32_MAX)) return 0;
    c[a] = (int32_t)q;
  }
  return 1;
}
tvdb_status_t tvdb_mesh_to_level_set(const tvdb_triangle_mesh* mesh,
                                     const tvdb_mesh_level_set_options_t* options,
                                     tvdb_thread_pool_t* pool, tvdb_grid_t* out,
                                     tvdb_mesh_level_set_result_t* result,
                                     tvdb_error_t* err) {
  if (!mesh || !options || !out || !mesh->faces || !mesh->vertices ||
      !mesh->face_count || !isfinite(options->voxel_size) ||
      options->voxel_size <= 0 || !isfinite(options->exterior_width) ||
      options->exterior_width <= 0 || !isfinite(options->interior_width) ||
      options->interior_width <= 0 ||
      (options->sign != TVDB_LEVEL_SET_SIGN_WINDING_FAST &&
       options->sign != TVDB_LEVEL_SET_SIGN_WINDING_EXACT))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid mesh level set arguments");
  for (size_t i = 0; i < mesh->vertex_count; ++i) {
    const tvdb_vec3f v = mesh->vertices[i];
    if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z))
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                             "mesh level set vertex is not finite");
  }
  for (size_t i = 0; i < mesh->face_count; ++i) {
    const tvdb_triangle f = mesh->faces[i];
    if (f.v0 >= mesh->vertex_count || f.v1 >= mesh->vertex_count ||
        f.v2 >= mesh->vertex_count)
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "mesh level set face index out of range");
  }
  const double h = options->voxel_size, inv_h = 1.0 / h;
  const float outside = options->exterior_width,
              inside = options->interior_width;
  tool_map voxels = {0}, band = {0};
  tvdb_mesh_sdf_t* query = NULL;
  tvdb_winding_t* wn = NULL;
  tvdb_thread_pool_t* owned = NULL;
  float* distance = NULL;
  double* winding = NULL;
  tvdb_grid_t stub = {0}, generated = {0}, filled = {0}, pruned = {0};
  tvdb_mesh_level_set_result_t report = {0};
  tvdb_status_t st = TVDB_OK;
  voxels.limit = band.limit = tool_limit(options->max_voxels);
  voxels.err = band.err = err;

  /* Seeds: samples at most h/2 apart cover every face, so every voxel whose
   * center lies within h * sqrt(3) / 2 of a face is at most one 6-neighbor
   * step from a seed. */
  for (size_t f = 0; f < mesh->face_count && st == TVDB_OK; ++f) {
    const tvdb_vec3f* v[3] = {mesh->vertices + mesh->faces[f].v0,
                              mesh->vertices + mesh->faces[f].v1,
                              mesh->vertices + mesh->faces[f].v2};
    const double p0[3] = {v[0]->x, v[0]->y, v[0]->z};
    const double e1[3] = {v[1]->x - p0[0], v[1]->y - p0[1], v[1]->z - p0[2]};
    const double e2[3] = {v[2]->x - p0[0], v[2]->y - p0[1], v[2]->z - p0[2]};
    const double l = fmax(fmax(sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]),
                               sqrt(e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2])),
                          sqrt((e1[0] - e2[0]) * (e1[0] - e2[0]) +
                               (e1[1] - e2[1]) * (e1[1] - e2[1]) +
                               (e1[2] - e2[2]) * (e1[2] - e2[2])));
    const double steps = ceil(2.0 * l * inv_h);
    if (!isfinite(steps) || steps > 1e6 ||
        (steps + 1) * (steps + 2) / 2 > 32.0 * (double)voxels.limit) {
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "mesh level set face exceeds the voxel budget");
      break;
    }
    const size_t n = steps < 1 ? 1 : (size_t)steps;
    for (size_t i = 0; i <= n && st == TVDB_OK; ++i)
      for (size_t j = 0; i + j <= n; ++j) {
        const double a = (double)i / (double)n, b = (double)j / (double)n;
        const double p[3] = {p0[0] + a * e1[0] + b * e2[0],
                             p0[1] + a * e1[1] + b * e2[1],
                             p0[2] + a * e1[2] + b * e2[2]};
        int32_t c[3];
        if (!tool_nearest_voxel(p, inv_h, c)) {
          st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                               "mesh level set coordinate outside int32");
          break;
        }
        st = tool_insert(&voxels, c, NULL);
        if (st != TVDB_OK) break;
      }
  }
  if (st != TVDB_OK) goto done;
  report.seed_voxels = voxels.count;
  {
    /* Dilate by enough 6-neighbor layers to reach every voxel within the
     * band (Euclidean distance <= L1 distance / sqrt(3)). */
    const double layers =
        ceil(sqrt(3.0) * fmax(outside, inside) * inv_h) + 2;
    if (!isfinite(layers) || layers > (double)voxels.limit) {
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "mesh level set band exceeds the voxel budget");
      goto done;
    }
    size_t begin = 0;
    for (size_t layer = 0; layer < (size_t)layers; ++layer) {
      const size_t end = voxels.count;
      for (size_t i = begin; i < end; ++i) {
        int32_t base[3];
        memcpy(base, voxels.v[i].c, sizeof(base));
        for (int a = 0; a < 3; ++a)
          for (int dir = -1; dir <= 1; dir += 2) {
            int32_t c[3];
            memcpy(c, base, sizeof(c));
            const int64_t q = (int64_t)c[a] + dir;
            if (q < INT32_MIN || q > INT32_MAX) {
              st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                                   "mesh level set coordinate outside int32");
              goto done;
            }
            c[a] = (int32_t)q;
            st = tool_insert(&voxels, c, NULL);
            if (st != TVDB_OK) goto done;
          }
      }
      begin = end;
    }
  }
  report.candidate_voxels = voxels.count;
  {
    size_t fbytes, dbytes;
    if (!tvdb_size_mul(voxels.count, sizeof(float), &fbytes) ||
        !tvdb_size_mul(voxels.count, sizeof(double), &dbytes)) {
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "mesh level set size overflow");
      goto done;
    }
    distance = (float*)malloc(fbytes ? fbytes : 1);
    winding = (double*)malloc(dbytes ? dbytes : 1);
    if (!distance || !winding) {
      st = tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                           "mesh level set allocation failed");
      goto done;
    }
  }
  st = tvdb_mesh_sdf_create(mesh, &query, err);
  if (st != TVDB_OK) goto done;
  st = tvdb_winding_create(mesh,
                           options->sign == TVDB_LEVEL_SET_SIGN_WINDING_EXACT
                               ? TVDB_WINDING_EXACT
                               : TVDB_WINDING_FAST,
                           &wn, err);
  if (st != TVDB_OK) goto done;
  if (!pool) {
    size_t workers = voxels.count < 32768 ? 1 : tvdb_thread_default_count();
    if (workers > 8) workers = 8;
    st = tvdb_thread_pool_create(workers ? workers : 1, &owned, err);
    if (st != TVDB_OK) goto done;
    pool = owned;
  }
  {
    tool_level_set_job job = {.voxels = voxels.v, .distance = distance,
                              .winding = winding, .query = query, .wn = wn,
                              .h = h};
    atomic_init(&job.bad, 0);
    st = tvdb_thread_pool_for(pool, 0, voxels.count, 1024,
                              tool_level_set_range, &job, err);
    if (st == TVDB_OK && atomic_load_explicit(&job.bad, memory_order_relaxed))
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "mesh level set distance arithmetic failed");
    if (st != TVDB_OK) goto done;
  }
  /* Serial, in candidate order: the band is independent of worker count. */
  for (size_t i = 0; i < voxels.count; ++i) {
    /* |w| > 1/2: inside-out closed meshes (winding -1) still read inside. */
    const int negative = fabs(winding[i]) > 0.5;
    const float d = distance[i];
    if (d >= (negative ? inside : outside)) continue;
    size_t j;
    st = tool_insert(&band, voxels.v[i].c, &j);
    if (st != TVDB_OK) goto done;
    /* No negative zero: voxels on the surface read +0 for either sign. */
    band.v[j].value = negative && d > 0 ? -d : d;
    band.v[j].active = 1;
    if (d > report.max_distance) report.max_distance = d;
  }
  report.active_voxels = band.count;
  {
    tvdb_transform_t transform;
    memset(&transform, 0, sizeof(transform));
    transform.type = TVDB_TRANSFORM_UNIFORM_SCALE;
    for (int a = 0; a < 3; ++a)
      transform.scale_values[a] = transform.voxel_size[a] = h;
    stub.descriptor.grid_name =
        (char*)(options->grid_name ? options->grid_name : "surface");
    stub.transform = transform;
    st = tool_build(&stub, &band, outside, &transform, &generated, err);
  }
  if (st == TVDB_OK) st = tool_set_string(&generated, "class", "level set", err);
  /* OpenVDB keeps the grid name as metadata too; its tools display that. */
  if (st == TVDB_OK)
    st = tool_set_string(&generated, "name", stub.descriptor.grid_name, err);
  if (st == TVDB_OK)
    st = tvdb_grid_signed_flood_fill(&generated, outside, -inside, &filled, err);
  if (st == TVDB_OK) st = tvdb_grid_prune(&filled, &pruned, err);
  if (st == TVDB_OK) {
    tvdb_grid_destroy_owned(out);
    *out = pruned;
    memset(&pruned, 0, sizeof(pruned));
    if (result) *result = report;
  }
done:
  tool_map_free(&voxels);
  tool_map_free(&band);
  tvdb_mesh_sdf_destroy(query);
  tvdb_winding_destroy(wn);
  tvdb_thread_pool_destroy(owned);
  free(distance);
  free(winding);
  tvdb_grid_destroy_owned(&generated);
  tvdb_grid_destroy_owned(&filled);
  tvdb_grid_destroy_owned(&pruned);
  return st;
}
tvdb_status_t tvdb_grid_level_set_track(const tvdb_grid_t* source,
                                        float outside, float inside,
                                        size_t budget, tvdb_grid_t* out,
                                        tvdb_level_set_rebuild_result_t* result,
                                        tvdb_error_t* err) {
  return tvdb_grid_level_set_rebuild(source, 0, outside, inside, budget, out,
                                     result, err);
}

static tvdb_status_t tool_active_region(const int32_t c[3], int64_t span,
                                        float value, int active, void* user) {
  tool_map* m = user;
  if (!active) return TVDB_OK;
  for (int64_t x = 0; x < span; ++x)
    for (int64_t y = 0; y < span; ++y)
      for (int64_t z = 0; z < span; ++z) {
        int32_t q[3] = {(int32_t)((int64_t)c[0] + x),
                        (int32_t)((int64_t)c[1] + y),
                        (int32_t)((int64_t)c[2] + z)};
        size_t i;
        tvdb_status_t st = tool_insert(m, q, &i);
        if (st != TVDB_OK) return st;
        m->v[i].value = value;
        m->v[i].active = 1;
      }
  return TVDB_OK;
}
static size_t tool_root(size_t* parent, size_t i) {
  size_t r = i;
  while (parent[r] != r) r = parent[r];
  while (parent[i] != i) {
    size_t next = parent[i];
    parent[i] = r;
    i = next;
  }
  return r;
}
typedef struct {
  size_t n, *parent, *component_size;
  uint32_t* neighbors;
  unsigned char* anchored;
  double *weight, *diagonal, *b, *x, *r, *z, *direction, *ap, *sums;
} tool_pcg;
static void tool_pcg_free(tool_pcg* p) {
  free(p->neighbors);
  free(p->parent);
  free(p->component_size);
  free(p->anchored);
  free(p->weight);
  free(p->diagonal);
  free(p->b);
  free(p->x);
  free(p->r);
  free(p->z);
  free(p->direction);
  free(p->ap);
  free(p->sums);
  memset(p, 0, sizeof(*p));
}
static tvdb_status_t tool_pcg_alloc(tool_pcg* p, size_t n, tvdb_error_t* err) {
  p->n = n;
  if (!n) return TVDB_OK;
  if (n > SIZE_MAX / 6 || n > UINT32_MAX) goto overflow;
  size_t bytes, six;
  if (!tvdb_size_mul(n, 6, &six) || !tvdb_size_mul(six, sizeof(uint32_t), &bytes))
    goto overflow;
  p->neighbors = malloc(bytes);
  if (!p->neighbors) goto oom;
  for (size_t i = 0; i < six; ++i) p->neighbors[i] = UINT32_MAX;
  if (!tvdb_size_mul(six, sizeof(double), &bytes)) goto overflow;
  p->weight = calloc(1, bytes);
  if (!p->weight) goto oom;
  if (!tvdb_size_mul(n, sizeof(size_t), &bytes)) goto overflow;
  p->parent = malloc(bytes);
  p->component_size = calloc(1, bytes);
  p->anchored = calloc(n, 1);
  if (!p->parent || !p->component_size || !p->anchored) goto oom;
  for (size_t i = 0; i < n; ++i) p->parent[i] = i;
  if (!tvdb_size_mul(n, sizeof(double), &bytes)) goto overflow;
#define TOOL_VECTOR(field)       \
  do {                           \
    p->field = calloc(1, bytes); \
    if (!p->field) goto oom;     \
  } while (0)
  TOOL_VECTOR(diagonal);
  TOOL_VECTOR(b);
  TOOL_VECTOR(x);
  TOOL_VECTOR(r);
  TOOL_VECTOR(z);
  TOOL_VECTOR(direction);
  TOOL_VECTOR(ap);
  TOOL_VECTOR(sums);
#undef TOOL_VECTOR
  return TVDB_OK;
overflow:
  return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                         "PDE allocation overflow");
oom:
  return tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                         "PDE workspace allocation failed");
}
static void tool_project(tool_pcg* p, double* v) {
  memset(p->sums, 0, p->n * sizeof(double));
  for (size_t i = 0; i < p->n; ++i)
    if (!p->anchored[p->parent[i]]) p->sums[p->parent[i]] += v[i];
  for (size_t i = 0; i < p->n; ++i)
    if (!p->anchored[p->parent[i]])
      v[i] -= p->sums[p->parent[i]] / p->component_size[p->parent[i]];
}
static void tool_apply(const tool_pcg* p, const double* x, double* out) {
  for (size_t i = 0; i < p->n; ++i) {
    double v = p->diagonal[i] * x[i];
    for (int d = 0; d < 6; ++d)
      if (p->neighbors[6 * i + d] != UINT32_MAX)
        v -= p->weight[6 * i + d] * x[p->neighbors[6 * i + d]];
    out[i] = v;
  }
}
static double tool_dot(size_t n, const double* a, const double* b) {
  double s = 0, correction = 0;
  for (size_t i = 0; i < n; ++i) {
    double value = a[i] * b[i] - correction, t = s + value;
    correction = (t - s) - value;
    s = t;
  }
  return s;
}
static double tool_norm(size_t n, const double* x) {
  double s = 0;
  for (size_t i = 0; i < n; ++i) s += x[i] * x[i];
  return sqrt(s);
}
static tvdb_status_t tool_assemble(tool_map* m, const double h[3],
                                   const tvdb_sparse_poisson_options_t* opts,
                                   tool_pcg* p, tvdb_error_t* err) {
  for (size_t i = 0; i < m->count; ++i) {
    p->b[i] = m->v[i].value;
    for (int axis = 0; axis < 3; ++axis)
      for (int side = 0; side < 2; ++side) {
        int d = 2 * axis + side;
        int64_t neighbor[3] = {m->v[i].c[0], m->v[i].c[1], m->v[i].c[2]};
        neighbor[axis] += side ? 1 : -1;
        size_t j = SIZE_MAX;
        if (neighbor[axis] >= INT32_MIN && neighbor[axis] <= INT32_MAX) {
          int32_t q[3] = {(int32_t)neighbor[0], (int32_t)neighbor[1],
                          (int32_t)neighbor[2]};
          j = tool_find(m, q);
        }
        if (j != SIZE_MAX && j < i) continue;
        double k = opts->coefficient
                       ? opts->coefficient(m->v[i].c, neighbor, opts->user)
                       : 1;
        double w = k / h[axis] / h[axis];
        if (!isfinite(k) || k <= 0 || !isfinite(w) || w <= 0)
          return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                                 "invalid face conductivity");
        if (j != SIZE_MAX) {
          p->neighbors[6 * i + d] = j;
          p->neighbors[6 * j + (d ^ 1)] = i;
          p->weight[6 * i + d] = p->weight[6 * j + (d ^ 1)] = w;
          p->diagonal[i] += w;
          p->diagonal[j] += w;
          size_t a = tool_root(p->parent, i), b = tool_root(p->parent, j);
          if (a != b) p->parent[b] = a;
        } else {
          tvdb_boundary_kind_t kind = TVDB_BOUNDARY_DIRICHLET;
          double value = 0;
          if (opts->boundary &&
              !opts->boundary(m->v[i].c, neighbor, &kind, &value, opts->user))
            return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                                   "boundary callback failed");
          if (!isfinite(value) || (kind != TVDB_BOUNDARY_DIRICHLET &&
                                   kind != TVDB_BOUNDARY_NEUMANN))
            return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                                   "invalid boundary condition");
          if (kind == TVDB_BOUNDARY_DIRICHLET) {
            p->diagonal[i] += w;
            p->b[i] += w * value;
            p->anchored[i] = 1;
          } else
            p->b[i] += value / h[axis];
        }
      }
  }
  for (size_t i = 0; i < p->n; ++i) p->parent[i] = tool_root(p->parent, i);
  for (size_t i = 0; i < p->n; ++i) {
    size_t c = p->parent[i];
    ++p->component_size[c];
    if (p->anchored[i]) p->anchored[c] = 1;
  }
  /* Component flags must be read only at their roots after this pass. */
  for (size_t i = 0; i < p->n; ++i) {
    if (!isfinite(p->diagonal[i]) || !isfinite(p->b[i]))
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "PDE assembly overflow");
    p->sums[p->parent[i]] += p->b[i];
    p->ap[p->parent[i]] += fabs(p->b[i]);
  }
  for (size_t i = 0; i < p->n; ++i)
    if (p->parent[i] == i && !p->anchored[i]) {
      if (!isfinite(p->sums[i]) || !isfinite(p->ap[i]) ||
          fabs(p->sums[i]) > 1e-10 * p->ap[i])
        return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                               "incompatible RHS/flux on a Neumann component");
    }
  if (p->n) tool_project(p, p->b);
  return TVDB_OK;
}
tvdb_status_t tvdb_grid_solve_poisson(const tvdb_grid_t* source,
                                      const tvdb_sparse_poisson_options_t* opts,
                                      tvdb_grid_t* out,
                                      tvdb_poisson_result_t* result,
                                      tvdb_error_t* err) {
  if (!tool_output(source, out) || !opts || opts->max_iterations < 0 ||
      !isfinite(opts->tolerance) || opts->tolerance < 0)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid sparse PDE arguments");
  tvdb_tree_index index = {0};
  tool_map m = {0};
  tool_pcg p = {0};
  tvdb_grid_t generated = {0};
  tvdb_poisson_result_t report = {0};
  m.limit = tool_limit(opts->max_voxels);
  m.err = err;
  tvdb_status_t st = tool_validate(source, &index, err);
  if (st != TVDB_OK) goto done;
  if (index.active_voxels > m.limit) {
    st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                         "PDE active topology exceeds budget");
    goto done;
  }
  double matrix[4][4], h[3];
  tvdb_tree_matrix(&source->transform, matrix);
  if (!tool_spacing(matrix, h, 0)) {
    st = tvdb_tree_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
                         "PDE requires orthogonal axes");
    goto done;
  }
  st = tool_regions(&index, tool_active_region, &m);
  if (st != TVDB_OK) goto done;
  st = tool_pcg_alloc(&p, m.count, err);
  if (st != TVDB_OK) goto done;
  st = tool_assemble(&m, h, opts, &p, err);
  if (st != TVDB_OK) goto done;
  report.initial_residual_norm = tool_norm(p.n, p.b);
  if (!isfinite(report.initial_residual_norm)) goto breakdown;
  double threshold = opts->tolerance * report.initial_residual_norm;
  if (!isfinite(threshold)) goto breakdown;
  if (p.n) {
    memcpy(p.r, p.b, p.n * sizeof(double));
    for (size_t i = 0; i < p.n; ++i)
      p.z[i] = p.diagonal[i] ? p.r[i] / p.diagonal[i] : 0;
    tool_project(&p, p.z);
    memcpy(p.direction, p.z, p.n * sizeof(double));
  }
  double rz = tool_dot(p.n, p.r, p.z), residual = report.initial_residual_norm;
  for (int it = 0; it < opts->max_iterations && residual > threshold; ++it) {
    tool_apply(&p, p.direction, p.ap);
    double denom = tool_dot(p.n, p.direction, p.ap);
    if (!isfinite(rz) || rz <= 0 || !isfinite(denom) || denom <= 0)
      goto breakdown;
    double alpha = rz / denom;
    if (!isfinite(alpha)) goto breakdown;
    for (size_t i = 0; i < p.n; ++i) {
      p.x[i] += alpha * p.direction[i];
      p.r[i] -= alpha * p.ap[i];
    }
    tool_project(&p, p.x);
    tool_project(&p, p.r);
    ++report.iterations;
    residual = tool_norm(p.n, p.r);
    if (!isfinite(residual)) goto breakdown;
    if (residual <= threshold) break;
    for (size_t i = 0; i < p.n; ++i)
      p.z[i] = p.diagonal[i] ? p.r[i] / p.diagonal[i] : 0;
    tool_project(&p, p.z);
    double next = tool_dot(p.n, p.r, p.z), beta = next / rz;
    if (!isfinite(next) || next <= 0 || !isfinite(beta)) goto breakdown;
    for (size_t i = 0; i < p.n; ++i)
      p.direction[i] = p.z[i] + beta * p.direction[i];
    tool_project(&p, p.direction);
    rz = next;
  }
  for (size_t i = 0; i < p.n; ++i) {
    if (!isfinite(p.x[i]) || fabs(p.x[i]) > FLT_MAX) goto breakdown;
    m.v[i].value = (float)p.x[i];
    p.z[i] = m.v[i].value;
  }
  tool_apply(&p, p.z, p.ap);
  for (size_t i = 0; i < p.n; ++i) p.r[i] = p.b[i] - p.ap[i];
  report.final_residual_norm = tool_norm(p.n, p.r);
  if (!isfinite(report.final_residual_norm)) goto breakdown;
  report.converged = report.final_residual_norm <= threshold;
  st = tool_build(source, &m, 0, &source->transform, &generated, err);
  if (st == TVDB_OK) st = tool_set_string(&generated, "class", "unknown", err);
  if (st == TVDB_OK) {
    tvdb_grid_destroy_owned(out);
    *out = generated;
    memset(&generated, 0, sizeof(generated));
    if (result) *result = report;
  }
  goto done;
breakdown:
  st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                       "sparse PCG numerical breakdown");
done:
  tool_map_free(&m);
  tool_pcg_free(&p);
  tvdb_tree_index_destroy(&index);
  tvdb_grid_destroy_owned(&generated);
  return st;
}
