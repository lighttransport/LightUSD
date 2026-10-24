#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_checked.h"
#include "tinyvdb_tree_internal.h"

tvdb_status_t tvdb_tree_error(tvdb_error_t* err, tvdb_status_t st,
                              const char* msg) {
  if (err) {
    memset(err, 0, sizeof(*err));
    err->status = st;
    snprintf(err->message, sizeof(err->message), "%s", msg);
  }
  return st;
}
static unsigned tree_pop(unsigned x) {
  return (unsigned)__builtin_popcount(x);
}
size_t tvdb_tree_mask_rank(const tvdb_nodemask_t* m, size_t bit) {
  size_t n = 0, i;
  for (i = 0; i < bit / 8; ++i) n += tree_pop(m->bits.data[i]);
  if (bit % 8) n += tree_pop(m->bits.data[i] & ((1u << (bit % 8)) - 1));
  return n;
}
static int mask_on(const tvdb_nodemask_t* m, size_t bit) {
  return (m->bits.data[bit / 8] >> (bit % 8)) & 1;
}
static void mask_set(tvdb_nodemask_t* m, size_t bit, int on) {
  unsigned char b = (unsigned char)(1u << (bit % 8));
  if (on)
    m->bits.data[bit / 8] |= b;
  else
    m->bits.data[bit / 8] &= (unsigned char)~b;
}
static int mask_valid(const tvdb_nodemask_t* m, int L) {
  size_t n = (size_t)1 << (3 * L), bytes = (n + 7) / 8;
  return m->log2dim == L && m->bitsize == (int32_t)n && m->bits.num_bits == n &&
         m->bits.num_bytes >= bytes && m->bits.data &&
         (!(n % 8) ||
          !(m->bits.data[bytes - 1] & (unsigned char)~((1u << (n % 8)) - 1)));
}
static uint64_t root_hash(const int32_t c[3]) {
  uint64_t v = (uint32_t)c[0] * UINT64_C(73856093) ^
               (uint32_t)c[1] * UINT64_C(19349663) ^
               (uint32_t)c[2] * UINT64_C(83492791);
  v ^= v >> 33;
  v *= UINT64_C(0xff51afd7ed558ccd);
  return v ^ (v >> 33);
}
static const int32_t* root_coord(const tvdb_root_node_t* r, size_t item) {
  return item < r->num_children
             ? r->child_origins + 3 * item
             : r->tile_origins + 3 * (item - r->num_children);
}
static int tree_region(tvdb_tree_index* p, const int32_t origin[3],
                       int64_t span, size_t count) {
  if (count > SIZE_MAX - p->active_voxels) return 0;
  p->active_voxels += count;
  for (int a = 0; a < 3; ++a) {
    int64_t end = (int64_t)origin[a] + span;
    if (!p->has_bbox || origin[a] < p->bbox_min[a]) p->bbox_min[a] = origin[a];
    if (!p->has_bbox || end > p->bbox_max[a]) p->bbox_max[a] = end;
  }
  p->has_bbox = 1;
  return 1;
}
static int tree_tile(tvdb_tree_index* p, const int32_t origin[3],
                     int64_t span) {
  size_t count;
  if ((uint64_t)span > SIZE_MAX ||
      !tvdb_size_mul((size_t)span, (size_t)span, &count) ||
      !tvdb_size_mul(count, (size_t)span, &count) ||
      p->active_tiles == SIZE_MAX || !tree_region(p, origin, span, count))
    return 0;
  ++p->active_tiles;
  return 1;
}
typedef struct {
  tvdb_tree_index* p;
  unsigned char* seen;
  size_t used;
} tree_walk;
static tvdb_status_t validate_node(tree_walk* w, size_t idx, int level,
                                   const int32_t origin[3], tvdb_error_t* err) {
  tvdb_tree_index* p = w->p;
  const tvdb_tree_t* t = &p->grid->tree;
  if (idx >= t->num_nodes || w->seen[idx] || level >= p->levels)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "invalid, cyclic or shared tree child");
  const tvdb_tree_node_t* n = &t->nodes[idx];
  tvdb_node_type_t want =
      level == p->levels - 1 ? TVDB_NODE_LEAF : TVDB_NODE_INTERNAL;
  if (n->type != want || n->level != level)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "tree node level/type mismatch");
  w->seen[idx] = 1;
  p->order[w->used++] = idx;
  memcpy(p->origins[idx], origin, 3 * sizeof(int32_t));
  int L = t->layout.levels[level].log2dim;
  size_t slots = (size_t)1 << (3 * L), bytes;
  if (!tvdb_size_mul(slots,
                     tvdb_value_type_size(t->layout.levels[level].value_type),
                     &bytes))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "tree buffer size overflow");
  for (int a = 0; a < 3; ++a)
    if ((int64_t)origin[a] + p->span[level] > (int64_t)INT32_MAX + 1)
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "tree extent outside int32 coordinates");
  if (want == TVDB_NODE_LEAF) {
    const tvdb_leaf_node_t* lf = &n->u.leaf;
    if (!mask_valid(&lf->value_mask, L) || !lf->data || lf->data_size < bytes ||
        lf->num_voxels != slots)
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "invalid tree leaf buffer or mask");
    int dim = 1 << L;
    for (size_t s = 0; s < slots; ++s)
      if (mask_on(&lf->value_mask, s)) {
        int32_t c[3];
        for (int a = 0; a < 3; ++a)
          c[a] = (int32_t)((int64_t)origin[a] +
                           ((s >> ((2 - a) * L)) & (dim - 1)));
        if (!tree_region(p, c, 1, 1))
          return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                                 "active voxel count overflow");
      }
  } else {
    const tvdb_internal_node_t* in = &n->u.internal;
    if (!mask_valid(&in->child_mask, L) || !mask_valid(&in->value_mask, L) ||
        !in->values || in->values_size < bytes ||
        (in->num_children && !in->child_indices) ||
        tvdb_tree_mask_rank(&in->child_mask, slots) != in->num_children)
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "invalid internal arrays or masks");
    size_t child = 0;
    int dim = 1 << L;
    for (size_t s = 0; s < slots; ++s) {
      int32_t c[3];
      for (int a = 0; a < 3; ++a)
        c[a] = (int32_t)((int64_t)origin[a] +
                         (int64_t)((s >> ((2 - a) * L)) & (dim - 1)) *
                             p->span[level + 1]);
      if (mask_on(&in->child_mask, s)) {
        if (mask_on(&in->value_mask, s))
          return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                                 "overlapping child and value masks");
        tvdb_status_t st =
            validate_node(w, in->child_indices[child++], level + 1, c, err);
        if (st != TVDB_OK) return st;
      } else if (mask_on(&in->value_mask, s) &&
                 !tree_tile(p, c, p->span[level + 1]))
        return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                               "active tile count overflow");
    }
  }
  return TVDB_OK;
}
void tvdb_tree_index_destroy(tvdb_tree_index* p) {
  free(p->origins);
  free(p->order);
  free(p->root_map);
  memset(p, 0, sizeof(*p));
}
tvdb_status_t tvdb_tree_index_create(const tvdb_grid_t* g, int float_only,
                                     tvdb_tree_index* p, tvdb_error_t* err) {
  if (!p)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL tree index");
  memset(p, 0, sizeof(*p));
  if (!g) return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL grid");
  const tvdb_tree_t* t = &g->tree;
  if (g->metadata.count > g->metadata.capacity ||
      (g->metadata.count && !g->metadata.entries))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "invalid metadata array");
  p->grid = g;
  p->levels = t->layout.num_levels;
  if (p->levels < 2 || p->levels > TVDB_MAX_TREE_DEPTH || !t->nodes ||
      !t->num_nodes || t->num_nodes > t->nodes_capacity ||
      t->nodes[0].type != TVDB_NODE_ROOT || t->nodes[0].level != 0 ||
      t->layout.levels[0].node_type != TVDB_NODE_ROOT)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "invalid tree root/layout");
  tvdb_value_type_t vt = t->layout.levels[p->levels - 1].value_type;
  if ((float_only && vt != TVDB_VALUE_FLOAT) || !tvdb_value_type_size(vt) ||
      (float_only && (t->is_point_data_grid || t->is_point_index_grid)))
    return tvdb_tree_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
                           "expected a float tree");
  int sum = 0;
  for (int l = p->levels - 1; l >= 1; --l) {
    int L = t->layout.levels[l].log2dim;
    if (L < 0 || L > 5 || sum + L > 31 ||
        t->layout.levels[l].value_type != vt ||
        t->layout.levels[l].node_type !=
            (l == p->levels - 1 ? TVDB_NODE_LEAF : TVDB_NODE_INTERNAL))
      return tvdb_tree_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
                             "unsupported tree dimensions/type");
    sum += L;
    p->span[l] = INT64_C(1) << sum;
    p->span_shift[l] = sum;
  }
  const tvdb_root_node_t* r = &t->nodes[0].u.root;
  if ((r->num_children && (!r->child_origins || !r->child_indices)) ||
      (r->num_tiles &&
       (!r->tile_origins || !r->tile_values || !r->tile_active)) ||
      r->background.type != vt)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "invalid root arrays/background");
  size_t count = (size_t)r->num_children + r->num_tiles, bytes;
  if (count < r->num_children ||
      !tvdb_hash_capacity_scaled(count, 4, 3, &p->root_capacity) ||
      !tvdb_size_mul(t->num_nodes, sizeof(*p->origins), &bytes))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                           "tree index size overflow");
  p->origins = calloc(1, bytes);
  if (!tvdb_size_mul(t->num_nodes, sizeof(size_t), &bytes)) goto overflow;
  p->order = malloc(bytes);
  if (!tvdb_size_mul(p->root_capacity, sizeof(size_t), &bytes)) goto overflow;
  p->root_map = calloc(1, bytes);
  unsigned char* seen = calloc(t->num_nodes, 1);
  if (!p->origins || !p->order || !p->root_map || !seen) {
    free(seen);
    tvdb_tree_index_destroy(p);
    return tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                           "tree index allocation failed");
  }
  tree_walk w = {p, seen, 1};
  seen[0] = 1;
  p->order[0] = 0;
  tvdb_status_t st = TVDB_OK;
  for (size_t item = 0; item < count; ++item) {
    const int32_t* c = root_coord(r, item);
    for (int a = 0; a < 3; ++a)
      if ((int64_t)c[a] % p->span[1] ||
          (int64_t)c[a] + p->span[1] > (int64_t)INT32_MAX + 1) {
        st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "misaligned/out-of-range root origin");
        goto done;
      }
    size_t h = (size_t)root_hash(c) & (p->root_capacity - 1);
    while (p->root_map[h]) {
      if (!memcmp(c, root_coord(r, p->root_map[h] - 1), 3 * sizeof(int32_t))) {
        st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "duplicate root origin");
        goto done;
      }
      h = (h + 1) & (p->root_capacity - 1);
    }
    p->root_map[h] = item + 1;
    if (item < r->num_children) {
      st = validate_node(&w, r->child_indices[item], 1, c, err);
      if (st != TVDB_OK) goto done;
    } else {
      size_t tile = item - r->num_children;
      if (r->tile_values[tile].type != vt ||
          (r->tile_active[tile] && !tree_tile(p, c, p->span[1]))) {
        st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                             "invalid root tile/count overflow");
        goto done;
      }
    }
  }
  if (w.used != t->num_nodes)
    st =
        tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA, "unreachable tree nodes");
done:
  free(seen);
  if (st != TVDB_OK) tvdb_tree_index_destroy(p);
  return st;
overflow:
  tvdb_tree_index_destroy(p);
  return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                         "tree index size overflow");
}
float tvdb_tree_get(const tvdb_tree_index* p, const int32_t coord[3],
                    int* active) {
  const tvdb_root_node_t* r = &p->grid->tree.nodes[0].u.root;
  int32_t origin[3];
  if (active) *active = 0;
  for (int a = 0; a < 3; ++a) {
    int64_t v = coord[a], span = p->span[1], sh = p->span_shift[1];
    origin[a] = (int32_t)((v >= 0 ? v >> sh : -1 - ((-1 - v) >> sh)) * span);
  }
  size_t h = (size_t)root_hash(origin) & (p->root_capacity - 1),
         item = SIZE_MAX;
  while (p->root_map[h]) {
    size_t i = p->root_map[h] - 1;
    if (!memcmp(origin, root_coord(r, i), sizeof(origin))) {
      item = i;
      break;
    }
    h = (h + 1) & (p->root_capacity - 1);
  }
  if (item == SIZE_MAX) return r->background.u.f;
  if (item >= r->num_children) {
    item -= r->num_children;
    if (active) *active = r->tile_active[item] != 0;
    return r->tile_values[item].u.f;
  }
  size_t node = r->child_indices[item];
  for (int l = 1; l < p->levels; ++l) {
    const tvdb_tree_node_t* n = &p->grid->tree.nodes[node];
    int L = p->grid->tree.layout.levels[l].log2dim;
    size_t slot = 0;
    const int sh = l + 1 == p->levels ? 0 : p->span_shift[l + 1];
    for (int a = 0; a < 3; ++a)
      slot = (slot << L) |
             (size_t)(((int64_t)coord[a] - p->origins[node][a]) >> sh);
    if (n->type == TVDB_NODE_LEAF) {
      float v;
      v = *(const float*)(n->u.leaf.data + slot * 4);
      if (active) *active = mask_on(&n->u.leaf.value_mask, slot);
      return v;
    }
    const tvdb_internal_node_t* in = &n->u.internal;
    if (!mask_on(&in->child_mask, slot)) {
      float v;
      v = *(const float*)(in->values + slot * 4);
      if (active) *active = mask_on(&in->value_mask, slot);
      return v;
    }
    node = in->child_indices[tvdb_tree_mask_rank(&in->child_mask, slot)];
  }
  return NAN;
}
int tvdb_tree_matrix(const tvdb_transform_t* t, double m[4][4]) {
  memset(m, 0, 16 * sizeof(double));
  for (int a = 0; a < 4; ++a) m[a][a] = 1;
  switch (t->type) {
    case TVDB_TRANSFORM_AFFINE:
      memcpy(m, t->matrix, 16 * sizeof(double));
      break;
    case TVDB_TRANSFORM_UNIFORM_SCALE:
    case TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE:
      for (int a = 0; a < 3; ++a) m[a][a] = t->voxel_size[0];
      if (t->type == TVDB_TRANSFORM_UNIFORM_SCALE) break;
      for (int a = 0; a < 3; ++a) m[a][3] = t->translation[a];
      break;
    case TVDB_TRANSFORM_SCALE:
    case TVDB_TRANSFORM_SCALE_TRANSLATE:
      for (int a = 0; a < 3; ++a) m[a][a] = t->voxel_size[a];
      if (t->type == TVDB_TRANSFORM_SCALE) break;
      for (int a = 0; a < 3; ++a) m[a][3] = t->translation[a];
      break;
    case TVDB_TRANSFORM_TRANSLATION:
      for (int a = 0; a < 3; ++a) m[a][3] = t->translation[a];
      break;
    default:
      return 0;
  }
  for (int a = 0; a < 4; ++a)
    for (int b = 0; b < 4; ++b)
      if (!isfinite(m[a][b])) return 0;
  if (m[3][0] || m[3][1] || m[3][2] || m[3][3] != 1) return 0;
  double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  return isfinite(d) && d != 0;
}
int tvdb_tree_dense_geometry(const tvdb_transform_t* t, float* h,
                             float origin[3]) {
  double m[4][4];
  if (!tvdb_tree_matrix(t, m) || m[0][0] <= 0 || m[0][0] > FLT_MAX ||
      (float)m[0][0] <= 0)
    return 0;
  for (int a = 0; a < 3; ++a) {
    if (m[a][a] != m[0][0] || fabs(m[a][3]) > FLT_MAX) return 0;
    for (int b = 0; b < 3; ++b)
      if (a != b && m[a][b] != 0) return 0;
    origin[a] = (float)m[a][3];
  }
  *h = (float)m[0][0];
  return 1;
}

static void* tree_malloc(size_t n, void* u) {
  (void)u;
  return malloc(n);
}
static void* tree_realloc(void* p, size_t old, size_t n, void* u) {
  (void)old;
  (void)u;
  return realloc(p, n);
}
static void tree_free(void* p, size_t n, void* u) {
  (void)n;
  (void)u;
  free(p);
}
static tvdb_allocator_t tree_allocator = {tree_malloc, tree_realloc, tree_free,
                                          NULL};
static void* tree_copy(const void* src, size_t n) {
  if (!n) return NULL;
  if (!src) return NULL;
  void* p = malloc(n);
  if (p) memcpy(p, src, n);
  return p;
}
static char* tree_string(const char* s) {
  return s ? tree_copy(s, strlen(s) + 1) : NULL;
}
static int clone_mask(const tvdb_nodemask_t* src, tvdb_nodemask_t* dst) {
  *dst = *src;
  dst->bits.alloc = &tree_allocator;
  dst->bits.data = tree_copy(src->bits.data, src->bits.num_bytes);
  return dst->bits.data != NULL;
}
static int clone_array(const void* src, size_t count, size_t width,
                       void** out) {
  size_t bytes;
  *out = NULL;
  if (!tvdb_size_mul(count, width, &bytes)) return 0;
  if (!bytes) return 1;
  *out = tree_copy(src, bytes);
  return *out != NULL;
}
tvdb_status_t tvdb_tree_copy_attributes(const tvdb_grid_t* s, tvdb_grid_t* d,
                                        tvdb_error_t* err) {
  memset(d, 0, sizeof(*d));
  d->tree.alloc = &tree_allocator;
  d->metadata.alloc = &tree_allocator;
  d->descriptor = s->descriptor;
  d->descriptor.grid_name = d->descriptor.unique_name =
      d->descriptor.grid_type = d->descriptor.instance_parent_name = NULL;
  d->descriptor.grid_byte_offset = d->descriptor.block_byte_offset =
      d->descriptor.end_byte_offset = 0;
#define CLONE_STRING(field)                                    \
  do {                                                         \
    d->descriptor.field = tree_string(s->descriptor.field);    \
    if (s->descriptor.field && !d->descriptor.field) goto oom; \
  } while (0)
  CLONE_STRING(grid_name);
  CLONE_STRING(unique_name);
  CLONE_STRING(grid_type);
  /* The result owns a complete tree, not an instance of another file grid. */
#undef CLONE_STRING
  d->transform = s->transform;
  d->compression_flags = s->compression_flags;
  if (s->metadata.count > s->metadata.capacity ||
      (s->metadata.count && !s->metadata.entries))
    goto invalid;
  if (s->metadata.count) {
    size_t bytes;
    if (!tvdb_size_mul(s->metadata.count, sizeof(tvdb_meta_entry_t), &bytes))
      goto invalid;
    d->metadata.entries = calloc(1, bytes);
    if (!d->metadata.entries) goto oom;
    d->metadata.capacity = s->metadata.count;
    for (size_t i = 0; i < s->metadata.count; ++i) {
      const tvdb_meta_entry_t* a = s->metadata.entries + i;
      tvdb_meta_entry_t* b = d->metadata.entries + i;
      ++d->metadata.count;
      b->name = tree_string(a->name);
      b->type_name = tree_string(a->type_name);
      b->value = a->value;
      if (a->value.type == TVDB_VALUE_STRING) {
        b->value.u.s.str = NULL;
        if (a->value.u.s.len == SIZE_MAX || !a->value.u.s.str) goto invalid;
        b->value.u.s.str = tree_copy(a->value.u.s.str, a->value.u.s.len + 1);
        if (!b->value.u.s.str) goto oom;
      }
      b->raw_data_len = a->raw_data_len;
      if ((a->name && !b->name) || (a->type_name && !b->type_name)) goto oom;
      if (a->raw_data_len && !a->raw_data) goto invalid;
      b->raw_data = tree_copy(a->raw_data, a->raw_data_len);
      if (a->raw_data_len && !b->raw_data) goto oom;
    }
  }
  return TVDB_OK;
invalid:
  tvdb_grid_destroy_owned(d);
  return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                         "invalid metadata/allocation length");
oom:
  tvdb_grid_destroy_owned(d);
  return tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                         "attribute allocation failed");
}
static tvdb_status_t clone_grid(const tvdb_tree_index* p, tvdb_grid_t* d,
                                tvdb_error_t* err) {
  const tvdb_grid_t* s = p->grid;
  tvdb_status_t st = tvdb_tree_copy_attributes(s, d, err);
  if (st != TVDB_OK) return st;
  size_t bytes;
  if (!tvdb_size_mul(s->tree.num_nodes, sizeof(tvdb_tree_node_t), &bytes))
    goto invalid;
  d->tree.nodes = calloc(1, bytes);
  if (!d->tree.nodes) goto oom;
  d->tree.num_nodes = d->tree.nodes_capacity = s->tree.num_nodes;
  d->tree.layout = s->tree.layout;
  for (size_t i = 0; i < s->tree.num_nodes; ++i) {
    const tvdb_tree_node_t* a = s->tree.nodes + i;
    tvdb_tree_node_t* b = d->tree.nodes + i;
    b->type = a->type;
    b->level = a->level;
    memcpy(b->origin, p->origins[i], sizeof(b->origin));
    if (a->type == TVDB_NODE_ROOT) {
      const tvdb_root_node_t* r = &a->u.root;
      tvdb_root_node_t* q = &b->u.root;
      q->background = r->background;
      q->num_children = r->num_children;
      q->num_tiles = r->num_tiles;
      if (!clone_array(r->child_origins, r->num_children, 3 * sizeof(int32_t),
                       (void**)&q->child_origins) ||
          !clone_array(r->child_indices, r->num_children, sizeof(size_t),
                       (void**)&q->child_indices) ||
          !clone_array(r->tile_origins, r->num_tiles, 3 * sizeof(int32_t),
                       (void**)&q->tile_origins) ||
          !clone_array(r->tile_values, r->num_tiles, sizeof(tvdb_value_t),
                       (void**)&q->tile_values) ||
          !clone_array(r->tile_active, r->num_tiles, sizeof(int),
                       (void**)&q->tile_active))
        goto oom;
    } else if (a->type == TVDB_NODE_INTERNAL) {
      const tvdb_internal_node_t* r = &a->u.internal;
      tvdb_internal_node_t* q = &b->u.internal;
      q->num_children = r->num_children;
      q->values_size = r->values_size;
      if (!clone_mask(&r->child_mask, &q->child_mask) ||
          !clone_mask(&r->value_mask, &q->value_mask) ||
          !clone_array(r->values, r->values_size, 1, (void**)&q->values) ||
          !clone_array(r->child_indices, r->num_children, sizeof(size_t),
                       (void**)&q->child_indices))
        goto oom;
    } else {
      const tvdb_leaf_node_t* r = &a->u.leaf;
      tvdb_leaf_node_t* q = &b->u.leaf;
      q->num_voxels = r->num_voxels;
      q->data_size = r->data_size;
      if (!clone_mask(&r->value_mask, &q->value_mask) ||
          !clone_array(r->data, r->data_size, 1, (void**)&q->data))
        goto oom;
    }
  }
  return TVDB_OK;
invalid:
  tvdb_grid_destroy_owned(d);
  return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                         "invalid metadata/allocation length");
oom:
  tvdb_grid_destroy_owned(d);
  return tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                         "tree clone allocation failed");
}
static void diag_issue(tvdb_tree_diagnostics_t* d, unsigned flag, size_t node,
                       const int32_t c[3]) {
  d->flags |= flag;
  if (d->first_bad_node == SIZE_MAX) {
    d->first_bad_node = node;
    for (int a = 0; a < 3; ++a) d->first_bad_coord[a] = c ? c[a] : 0;
  }
}
static void diag_value(tvdb_tree_diagnostics_t* d, float v, int active,
                       float bg, tvdb_tree_diagnostic_kind_t kind, double tol,
                       size_t node, const int32_t c[3]) {
  if (!isfinite(v)) {
    ++d->nonfinite_values;
    diag_issue(d, TVDB_TREE_DIAG_NONFINITE, node, c);
    return;
  }
  if (kind == TVDB_TREE_DIAG_LEVEL_SET) {
    if (active && fabs((double)v) > bg + tol)
      diag_issue(d, TVDB_TREE_DIAG_RANGE, node, c);
    if (!active && fabs(fabs((double)v) - bg) > tol)
      diag_issue(d, TVDB_TREE_DIAG_INACTIVE, node, c);
  } else if (kind == TVDB_TREE_DIAG_FOG) {
    if (active && (v < -tol || v > 1 + tol))
      diag_issue(d, TVDB_TREE_DIAG_RANGE, node, c);
    if (!active && fabs((double)v) > tol)
      diag_issue(d, TVDB_TREE_DIAG_INACTIVE, node, c);
  }
}
static int uniform_geometry(const tvdb_transform_t* t, double* h) {
  double m[4][4], lengths[3] = {0};
  if (!tvdb_tree_matrix(t, m)) return 0;
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) lengths[a] += m[b][a] * m[b][a];
  if (!isfinite(lengths[0]) || lengths[0] <= 0) return 0;
  for (int a = 0; a < 3; ++a) {
    if (!isfinite(lengths[a]) ||
        fabs(lengths[a] - lengths[0]) > 1e-12 * lengths[0])
      return 0;
    for (int b = a + 1; b < 3; ++b) {
      double dot = 0;
      for (int k = 0; k < 3; ++k) dot += m[k][a] * m[k][b];
      if (!isfinite(dot) || fabs(dot) > 1e-12 * lengths[0]) return 0;
    }
  }
  *h = sqrt(lengths[0]);
  return isfinite(*h);
}
tvdb_status_t tvdb_grid_diagnose(const tvdb_grid_t* g,
                                 tvdb_tree_diagnostic_kind_t kind,
                                 int gradients, double tol,
                                 tvdb_tree_diagnostics_t* out,
                                 tvdb_error_t* err) {
  if (!out || kind < TVDB_TREE_DIAG_GENERIC || kind > TVDB_TREE_DIAG_FOG ||
      !isfinite(tol) || tol < 0 ||
      (gradients && kind != TVDB_TREE_DIAG_LEVEL_SET))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid tree diagnostic arguments");
  tvdb_tree_index p;
  tvdb_status_t st = tvdb_tree_index_create(g, 1, &p, err);
  if (st != TVDB_OK) return st;
  tvdb_tree_diagnostics_t d = {0};
  d.first_bad_node = SIZE_MAX;
  d.active_voxels = p.active_voxels;
  d.active_tiles = p.active_tiles;
  d.has_active_bbox = p.has_bbox;
  memcpy(d.active_min, p.bbox_min, sizeof(d.active_min));
  memcpy(d.active_max, p.bbox_max, sizeof(d.active_max));
  float bg = g->tree.nodes[0].u.root.background.u.f;
  double h = 1, m[4][4];
  int geometry = kind == TVDB_TREE_DIAG_LEVEL_SET
                     ? uniform_geometry(&g->transform, &h)
                     : tvdb_tree_matrix(&g->transform, m);
  if (!geometry) diag_issue(&d, TVDB_TREE_DIAG_TRANSFORM, 0, NULL);
  if (kind == TVDB_TREE_DIAG_LEVEL_SET && (!isfinite(bg) || bg <= 0))
    diag_issue(&d, TVDB_TREE_DIAG_BACKGROUND, 0, NULL);
  if (kind == TVDB_TREE_DIAG_FOG && (!isfinite(bg) || fabs((double)bg) > tol))
    diag_issue(&d, TVDB_TREE_DIAG_BACKGROUND, 0, NULL);
  if (kind != TVDB_TREE_DIAG_GENERIC) {
    const char* want =
        kind == TVDB_TREE_DIAG_LEVEL_SET ? "level set" : "fog volume";
    int found = 0;
    for (size_t i = 0; g->metadata.entries && i < g->metadata.count; ++i) {
      const tvdb_meta_entry_t* e = g->metadata.entries + i;
      if (e->name && !strcmp(e->name, "class") &&
          e->value.type == TVDB_VALUE_STRING && e->value.u.s.str &&
          e->value.u.s.len == strlen(want) &&
          !memcmp(e->value.u.s.str, want, strlen(want)))
        found = 1;
    }
    if (!found) diag_issue(&d, TVDB_TREE_DIAG_CLASS, 0, NULL);
  }
  diag_value(&d, bg, 0, bg, kind, tol, 0, NULL);
  const tvdb_root_node_t* root = &g->tree.nodes[0].u.root;
  for (size_t i = 0; i < root->num_tiles; ++i) {
    const int32_t* c = root->tile_origins + 3 * i;
    if (root->tile_active[i] && kind == TVDB_TREE_DIAG_LEVEL_SET)
      diag_issue(&d, TVDB_TREE_DIAG_ACTIVE_TILES, 0, c);
    diag_value(&d, root->tile_values[i].u.f, root->tile_active[i], bg, kind,
               tol, 0, c);
  }
  double grad_sum = 0;
  size_t bad = 0;
  for (size_t ni = 1; ni < g->tree.num_nodes; ++ni) {
    const tvdb_tree_node_t* n = g->tree.nodes + ni;
    int L = g->tree.layout.levels[n->level].log2dim, dim = 1 << L;
    size_t slots = (size_t)1 << (3 * L);
    int leaf = n->type == TVDB_NODE_LEAF;
    for (size_t s = 0; s < slots; ++s) {
      if (!leaf && mask_on(&n->u.internal.child_mask, s)) continue;
      int active =
          mask_on(leaf ? &n->u.leaf.value_mask : &n->u.internal.value_mask, s);
      float v;
      v = *(const float*)((leaf ? n->u.leaf.data : n->u.internal.values) + s * 4);
      int32_t c[3];
      int64_t step = leaf ? 1 : p.span[n->level + 1];
      for (int a = 0; a < 3; ++a)
        c[a] = (int32_t)((int64_t)p.origins[ni][a] +
                         (int64_t)((s >> ((2 - a) * L)) & (dim - 1)) * step);
      if (!leaf && active && kind == TVDB_TREE_DIAG_LEVEL_SET)
        diag_issue(&d, TVDB_TREE_DIAG_ACTIVE_TILES, ni, c);
      diag_value(&d, v, active, bg, kind, tol, ni, c);
      if (gradients && geometry && leaf && active && isfinite(v) &&
          fabsf(v) < bg) {
        double norm = 0;
        int finite = 1;
        for (int a = 0; a < 3; ++a) {
          int32_t lo[3], hi[3];
          memcpy(lo, c, sizeof(lo));
          memcpy(hi, c, sizeof(hi));
          if (c[a] == INT32_MIN || c[a] == INT32_MAX) {
            finite = 0;
            break;
          }
          --lo[a];
          ++hi[a];
          double dv = ((double)tvdb_tree_get(&p, hi, NULL) -
                       tvdb_tree_get(&p, lo, NULL)) /
                      (2 * h);
          if (!isfinite(dv)) {
            finite = 0;
            break;
          }
          norm += dv * dv;
        }
        if (finite && isfinite(norm)) {
          norm = sqrt(norm);
          double e = fabs(norm - 1);
          grad_sum += norm;
          ++d.gradient.band_count;
          if (e > d.gradient.max_grad_error) d.gradient.max_grad_error = e;
          if (e > tol) {
            ++bad;
            diag_issue(&d, TVDB_TREE_DIAG_GRADIENT, ni, c);
          }
        } else
          diag_issue(&d, TVDB_TREE_DIAG_GRADIENT, ni, c);
      }
    }
  }
  if (d.gradient.band_count) {
    d.gradient.mean_grad_mag = grad_sum / d.gradient.band_count;
    d.gradient.bad_fraction = (double)bad / d.gradient.band_count;
  }
  d.valid = d.flags == 0;
  *out = d;
  tvdb_tree_index_destroy(&p);
  return tvdb_tree_error(err, TVDB_OK, "");
}

typedef struct {
  int32_t c[3];
  float value;
  int active, priority;
  size_t node;
} root_entry;
static int root_entry_cmp(const void* aa, const void* bb) {
  const root_entry *a = aa, *b = bb;
  for (int k = 0; k < 3; ++k)
    if (a->c[k] != b->c[k]) return a->c[k] < b->c[k] ? -1 : 1;
  return a->priority > b->priority ? -1 : a->priority < b->priority;
}
static int same_float(float a, float b) {
  return !memcmp(&a, &b, sizeof(float));
}
static int constructor_valid(const tvdb_grid_t* g, tvdb_grid_t* out) {
  return g && out && g != out &&
         (!g->tree.nodes || out->tree.nodes != g->tree.nodes);
}
/* Called only after structural validation; avoid building a second index. */
static tvdb_status_t finite_tree(const tvdb_grid_t* g, tvdb_error_t* err) {
  const tvdb_root_node_t* r = &g->tree.nodes[0].u.root;
  if (!isfinite(r->background.u.f)) goto nonfinite;
  for (size_t i = 0; i < r->num_tiles; ++i)
    if (!isfinite(r->tile_values[i].u.f)) goto nonfinite;
  for (size_t ni = 1; ni < g->tree.num_nodes; ++ni) {
    const tvdb_tree_node_t* n = g->tree.nodes + ni;
    int leaf = n->type == TVDB_NODE_LEAF;
    int L = g->tree.layout.levels[n->level].log2dim;
    size_t slots = (size_t)1 << (3 * L);
    for (size_t s = 0; s < slots; ++s) {
      if (!leaf && mask_on(&n->u.internal.child_mask, s)) continue;
      float v;
      v = *(const float*)((leaf ? n->u.leaf.data : n->u.internal.values) + s * 4);
      if (!isfinite(v)) goto nonfinite;
    }
  }
  return TVDB_OK;
nonfinite:
  return tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                         "nonfinite stored tree value");
}
/* Internal nodes have sparse child arrays in increasing slot order. Compute
 * first/last represented values without assuming physical node order. */
static void node_ends(const tvdb_tree_node_t* n, int L, const float (*ends)[2],
                      float result[2]) {
  size_t slots = (size_t)1 << (3 * L);
  if (n->type == TVDB_NODE_LEAF) {
    memcpy(result, n->u.leaf.data, 4);
    memcpy(result + 1, n->u.leaf.data + (slots - 1) * 4, 4);
  } else {
    const tvdb_internal_node_t* in = &n->u.internal;
    if (mask_on(&in->child_mask, 0))
      result[0] = ends[in->child_indices[0]][0];
    else
      memcpy(result, in->values, 4);
    if (mask_on(&in->child_mask, slots - 1))
      result[1] = ends[in->child_indices[in->num_children - 1]][1];
    else
      memcpy(result + 1, in->values + (slots - 1) * 4, 4);
  }
}
tvdb_status_t tvdb_grid_signed_flood_fill(const tvdb_grid_t* g, float outside,
                                          float inside, tvdb_grid_t* out,
                                          tvdb_error_t* err) {
  if (!constructor_valid(g, out) || !isfinite(outside) || !isfinite(inside) ||
      outside <= 0 || inside >= 0)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid flood-fill source/output/widths");
  tvdb_tree_index p;
  tvdb_status_t st = tvdb_tree_index_create(g, 1, &p, err);
  if (st != TVDB_OK) return st;
  tvdb_grid_t d = {0};
  float (*ends)[2] = NULL;
  root_entry *children = NULL, *tiles = NULL;
  if (p.active_tiles) {
    st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                         "signed flood fill rejects active tiles");
    goto done;
  }
  st = finite_tree(g, err);
  if (st != TVDB_OK) goto done;
  st = clone_grid(&p, &d, err);
  if (st != TVDB_OK) goto done;
  size_t bytes;
  if (!tvdb_size_mul(g->tree.num_nodes, sizeof(*ends), &bytes)) goto overflow;
  ends = calloc(1, bytes);
  if (!ends) goto oom;
  for (size_t at = g->tree.num_nodes; at > 1; --at) {
    size_t ni = p.order[at - 1];
    tvdb_tree_node_t* n = d.tree.nodes + ni;
    int L = d.tree.layout.levels[n->level].log2dim, dim = 1 << L;
    size_t slots = (size_t)1 << (3 * L);
    int leaf = n->type == TVDB_NODE_LEAF;
    tvdb_nodemask_t* mask =
        leaf ? &n->u.leaf.value_mask : &n->u.internal.child_mask;
    float* v = (float*)(leaf ? n->u.leaf.data : n->u.internal.values);
    size_t first = 0;
    while (first < slots && !mask_on(mask, first)) ++first;
    if (first == slots) {
      float fill = v[0] < 0 ? inside : outside;
      for (size_t s = 0; s < slots; ++s) v[s] = fill;
    } else {
      size_t rank = 0;
      /* Byte-level cumulative popcount of the child mask: turns every
         O(bit/8) mask_rank scan below into an O(1) lookup. */
      unsigned short ranktab[64];
      const size_t nbytes = slots / 8;
      const int fast_rank = nbytes <= sizeof(ranktab);
      if (fast_rank) {
        unsigned short crun = 0;
        for (size_t bi = 0; bi < nbytes; ++bi) {
          ranktab[bi] = crun;
          crun = (unsigned short)(crun + __builtin_popcount(mask->bits.data[bi]));
        }
      }
      #define MASK_RANK_FAST(m, bit) \
        (fast_rank ? ((size_t)ranktab[(bit) / 8] + \
         (size_t)__builtin_popcount((m)->bits.data[(bit) / 8] & ((1u << ((bit) % 8)) - 1))) \
                   : tvdb_tree_mask_rank(m, bit))
      int xinside =
          leaf ? v[first] < 0 : ends[n->u.internal.child_indices[0]][0] < 0;
      for (int x = 0; x < dim; ++x) {
        size_t x00 = (size_t)x << (2 * L);
        if (mask_on(mask, x00))
          xinside = leaf ? v[x00] < 0
                         : ends[n->u.internal.child_indices[MASK_RANK_FAST(mask, x00)]][1] < 0;
        int yinside = xinside;
        for (int y = 0; y < dim; ++y) {
          size_t xy0 = x00 + ((size_t)y << L);
          if (mask_on(mask, xy0))
            yinside =
                leaf ? v[xy0] < 0
                     : ends[n->u.internal.child_indices[MASK_RANK_FAST(mask, xy0)]][1] < 0;
          int zinside = yinside;
          for (int z = 0; z < dim; ++z) {
            size_t s = xy0 + (size_t)z;
            if (mask_on(mask, s)) {
              zinside = leaf ? v[s] < 0
                             : ends[n->u.internal.child_indices[rank++]][1] < 0;
            } else
              v[s] = zinside ? inside : outside;
          }
        }
      }
      #undef MASK_RANK_FAST
    }
    node_ends(n, L, (const float (*)[2])ends, ends[ni]);
  }
  tvdb_root_node_t* r = &d.tree.nodes[0].u.root;
  if (!tvdb_size_mul(r->num_children, sizeof(*children), &bytes)) goto overflow;
  if (bytes) {
    children = malloc(bytes);
    if (!children) goto oom;
  }
  for (size_t i = 0; i < r->num_children; ++i) {
    memset(children + i, 0, sizeof(*children));
    memcpy(children[i].c, r->child_origins + 3 * i, sizeof(children[i].c));
    children[i].node = r->child_indices[i];
  }
  if (r->num_children > 1)
    qsort(children, r->num_children, sizeof(*children), root_entry_cmp);
  size_t total = r->num_tiles;
  for (size_t i = 1; i < r->num_children; ++i) {
    root_entry *a = children + i - 1, *b = children + i;
    int64_t dz = (int64_t)b->c[2] - a->c[2];
    if (a->c[0] != b->c[0] || a->c[1] != b->c[1] || dz <= p.span[1] ||
        ends[a->node][1] >= 0 || ends[b->node][0] >= 0)
      continue;
    size_t gaps = (size_t)((dz >> p.span_shift[1]) - 1);
    if (gaps > UINT32_MAX - total) goto overflow;
    total += gaps;
  }
  if (!tvdb_size_mul(total, sizeof(*tiles), &bytes)) goto overflow;
  if (bytes) {
    tiles = malloc(bytes);
    if (!tiles) goto oom;
  }
  size_t used = 0;
  for (size_t i = 0; i < r->num_tiles; ++i) {
    root_entry* e = tiles + used++;
    memset(e, 0, sizeof(*e));
    memcpy(e->c, r->tile_origins + 3 * i, sizeof(e->c));
    e->value = r->tile_values[i].u.f < 0 ? inside : outside;
  }
  for (size_t i = 1; i < r->num_children; ++i) {
    root_entry *a = children + i - 1, *b = children + i;
    if (a->c[0] != b->c[0] || a->c[1] != b->c[1] || ends[a->node][1] >= 0 ||
        ends[b->node][0] >= 0)
      continue;
    for (int64_t z = (int64_t)a->c[2] + p.span[1]; z < b->c[2];
         z += p.span[1]) {
      root_entry* e = tiles + used++;
      memset(e, 0, sizeof(*e));
      e->c[0] = a->c[0];
      e->c[1] = a->c[1];
      e->c[2] = (int32_t)z;
      e->value = inside;
      e->priority = 1;
    }
  }
  if (used > 1) qsort(tiles, used, sizeof(*tiles), root_entry_cmp);
  size_t unique = 0;
  for (size_t i = 0; i < used; ++i)
    if (!unique || memcmp(tiles[i].c, tiles[unique - 1].c, sizeof(tiles[i].c)))
      tiles[unique++] = tiles[i];
  free(r->tile_origins);
  free(r->tile_values);
  free(r->tile_active);
  r->tile_origins = NULL;
  r->tile_values = NULL;
  r->tile_active = NULL;
  r->num_tiles = (uint32_t)unique;
  if (unique) {
    r->tile_origins = malloc(unique * 3 * sizeof(int32_t));
    r->tile_values = calloc(unique, sizeof(tvdb_value_t));
    r->tile_active = calloc(unique, sizeof(int));
    if (!r->tile_origins || !r->tile_values || !r->tile_active) goto oom;
    for (size_t i = 0; i < unique; ++i) {
      memcpy(r->tile_origins + 3 * i, tiles[i].c, sizeof(tiles[i].c));
      r->tile_values[i].type = TVDB_VALUE_FLOAT;
      r->tile_values[i].u.f = tiles[i].value;
    }
  }
  r->background.u.f = outside;
  tvdb_grid_destroy_owned(out);
  *out = d;
  memset(&d, 0, sizeof(d));
  st = tvdb_tree_error(err, TVDB_OK, "");
  goto done;
overflow:
  st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                       "flood-fill allocation size overflow");
  goto done;
oom:
  st = tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                       "flood-fill allocation failed");
done:
  free(ends);
  free(children);
  free(tiles);
  tvdb_grid_destroy_owned(&d);
  tvdb_tree_index_destroy(&p);
  return st;
}
static void free_node(tvdb_tree_node_t* n) {
  if (n->type == TVDB_NODE_LEAF) {
    free(n->u.leaf.value_mask.bits.data);
    free(n->u.leaf.data);
  } else if (n->type == TVDB_NODE_INTERNAL) {
    free(n->u.internal.child_mask.bits.data);
    free(n->u.internal.value_mask.bits.data);
    free(n->u.internal.values);
    free(n->u.internal.child_indices);
  }
  memset(n, 0, sizeof(*n));
}
tvdb_status_t tvdb_grid_prune(const tvdb_grid_t* g, tvdb_grid_t* out,
                              tvdb_error_t* err) {
  if (!constructor_valid(g, out))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "prune requires distinct owning output");
  tvdb_tree_index p;
  tvdb_status_t st = tvdb_tree_index_create(g, 1, &p, err);
  if (st != TVDB_OK) return st;
  tvdb_grid_t d = {0};
  unsigned char *uniform = NULL, *active = NULL, *keep = NULL;
  float* value = NULL;
  size_t* map = NULL;
  tvdb_tree_node_t* nodes = NULL;
  size_t bytes;
  st = finite_tree(g, err);
  if (st != TVDB_OK) goto done;
  st = clone_grid(&p, &d, err);
  if (st != TVDB_OK) goto done;
  size_t count = g->tree.num_nodes;
  uniform = calloc(count, 1);
  active = calloc(count, 1);
  keep = calloc(count, 1);
  if (!tvdb_size_mul(count, sizeof(float), &bytes)) goto overflow;
  value = malloc(bytes);
  if (!tvdb_size_mul(count, sizeof(size_t), &bytes)) goto overflow;
  map = malloc(bytes);
  if (!uniform || !active || !keep || !value || !map) goto oom;
  for (size_t at = count; at > 1; --at) {
    size_t ni = p.order[at - 1];
    tvdb_tree_node_t* n = d.tree.nodes + ni;
    int L = d.tree.layout.levels[n->level].log2dim;
    size_t slots = (size_t)1 << (3 * L), child = 0, newchild = 0;
    int leaf = n->type == TVDB_NODE_LEAF;
    uniform[ni] = 1;
    value[ni] = 0;
    active[ni] = 0;
    for (size_t s = 0; s < slots; ++s) {
      float v;
      int on;
      if (!leaf && mask_on(&n->u.internal.child_mask, s)) {
        size_t ci = n->u.internal.child_indices[child++];
        if (uniform[ci]) {
          v = value[ci];
          on = active[ci];
          mask_set(&n->u.internal.child_mask, s, 0);
          mask_set(&n->u.internal.value_mask, s, on);
          memcpy(n->u.internal.values + s * 4, &v, 4);
        } else {
          n->u.internal.child_indices[newchild++] = ci;
          uniform[ni] = 0;
          continue;
        }
      } else {
        v = *(const float*)((leaf ? n->u.leaf.data : n->u.internal.values) + s * 4);
        on = mask_on(leaf ? &n->u.leaf.value_mask : &n->u.internal.value_mask,
                     s);
      }
      if (!s) {
        value[ni] = v;
        active[ni] = (unsigned char)on;
      } else if (!same_float(v, value[ni]) || on != active[ni])
        uniform[ni] = 0;
    }
    if (!leaf) n->u.internal.num_children = newchild;
  }
  tvdb_root_node_t* r = &d.tree.nodes[0].u.root;
  size_t total = r->num_tiles;
  for (size_t i = 0; i < r->num_children; ++i)
    if (uniform[r->child_indices[i]]) ++total;
  if (total > UINT32_MAX || !tvdb_size_mul(total, 3 * sizeof(int32_t), &bytes))
    goto overflow;
  int32_t* origins = bytes ? malloc(bytes) : NULL;
  tvdb_value_t* values = total ? calloc(total, sizeof(tvdb_value_t)) : NULL;
  int* states = total ? malloc(total * sizeof(int)) : NULL;
  if (total && (!origins || !values || !states)) {
    free(origins);
    free(values);
    free(states);
    goto oom;
  }
  size_t nt = 0, nc = 0;
  for (size_t i = 0; i < r->num_tiles; ++i) {
    if (!r->tile_active[i] &&
        same_float(r->tile_values[i].u.f, r->background.u.f))
      continue;
    memcpy(origins + nt * 3, r->tile_origins + i * 3, 3 * sizeof(int32_t));
    values[nt] = r->tile_values[i];
    states[nt++] = r->tile_active[i];
  }
  for (size_t i = 0; i < r->num_children; ++i) {
    size_t ci = r->child_indices[i];
    if (uniform[ci]) {
      if (!active[ci] && same_float(value[ci], r->background.u.f)) continue;
      memcpy(origins + nt * 3, r->child_origins + i * 3, 3 * sizeof(int32_t));
      values[nt].type = TVDB_VALUE_FLOAT;
      values[nt].u.f = value[ci];
      states[nt++] = active[ci];
    } else {
      r->child_indices[nc] = ci;
      memmove(r->child_origins + nc * 3, r->child_origins + i * 3,
              3 * sizeof(int32_t));
      ++nc;
    }
  }
  free(r->tile_origins);
  free(r->tile_values);
  free(r->tile_active);
  r->tile_origins = origins;
  r->tile_values = values;
  r->tile_active = states;
  r->num_tiles = (uint32_t)nt;
  r->num_children = (uint32_t)nc;
  keep[0] = 1;
  for (size_t i = 0; i < nc; ++i) keep[r->child_indices[i]] = 1;
  size_t retained = 1;
  for (size_t at = 1; at < count; ++at) {
    size_t ni = p.order[at];
    if (!keep[ni]) continue;
    ++retained;
    if (d.tree.nodes[ni].type == TVDB_NODE_INTERNAL) {
      tvdb_internal_node_t* in = &d.tree.nodes[ni].u.internal;
      for (size_t j = 0; j < in->num_children; ++j)
        keep[in->child_indices[j]] = 1;
    }
  }
  /* Release or shrink the retained child tables too, so pruning does not
     leave their former allocations behind in an otherwise compact tree. */
  for (size_t ni = 1; ni < count; ++ni)
    if (keep[ni] && d.tree.nodes[ni].type == TVDB_NODE_INTERNAL) {
      tvdb_internal_node_t* in = &d.tree.nodes[ni].u.internal;
      if (in->num_children == g->tree.nodes[ni].u.internal.num_children)
        continue;
      if (!in->num_children) {
        free(in->child_indices);
        in->child_indices = NULL;
      } else {
        size_t* q =
            realloc(in->child_indices, in->num_children * sizeof(size_t));
        if (!q) goto oom;
        in->child_indices = q;
      }
    }
  if (nc != g->tree.nodes[0].u.root.num_children) {
    if (!nc) {
      free(r->child_indices);
      free(r->child_origins);
      r->child_indices = NULL;
      r->child_origins = NULL;
    } else {
      size_t* ci = realloc(r->child_indices, nc * sizeof(size_t));
      if (!ci) goto oom;
      r->child_indices = ci;
      int32_t* co = realloc(r->child_origins, nc * 3 * sizeof(int32_t));
      if (!co) goto oom;
      r->child_origins = co;
    }
  }
  if (nt != total) {
    if (!nt) {
      free(r->tile_origins);
      free(r->tile_values);
      free(r->tile_active);
      r->tile_origins = NULL;
      r->tile_values = NULL;
      r->tile_active = NULL;
    } else {
      int32_t* co = realloc(r->tile_origins, nt * 3 * sizeof(int32_t));
      if (!co) goto oom;
      r->tile_origins = co;
      tvdb_value_t* v = realloc(r->tile_values, nt * sizeof(tvdb_value_t));
      if (!v) goto oom;
      r->tile_values = v;
      int* on = realloc(r->tile_active, nt * sizeof(int));
      if (!on) goto oom;
      r->tile_active = on;
    }
  }
  if (!tvdb_size_mul(retained, sizeof(*nodes), &bytes)) goto overflow;
  nodes = malloc(bytes);
  if (!nodes) goto oom;
  size_t used = 0;
  for (size_t i = 0; i < count; ++i)
    if (keep[i]) {
      map[i] = used;
      nodes[used++] = d.tree.nodes[i];
    }
  for (size_t i = 0; i < used; ++i) {
    size_t* children;
    size_t nchildren;
    if (nodes[i].type == TVDB_NODE_ROOT) {
      children = nodes[i].u.root.child_indices;
      nchildren = nodes[i].u.root.num_children;
    } else if (nodes[i].type == TVDB_NODE_INTERNAL) {
      children = nodes[i].u.internal.child_indices;
      nchildren = nodes[i].u.internal.num_children;
    } else
      continue;
    for (size_t j = 0; j < nchildren; ++j) children[j] = map[children[j]];
  }
  for (size_t i = 1; i < count; ++i)
    if (!keep[i]) free_node(d.tree.nodes + i);
  free(d.tree.nodes);
  d.tree.nodes = nodes;
  nodes = NULL;
  d.tree.num_nodes = d.tree.nodes_capacity = retained;
  tvdb_grid_destroy_owned(out);
  *out = d;
  memset(&d, 0, sizeof(d));
  st = tvdb_tree_error(err, TVDB_OK, "");
  goto done;
overflow:
  st = tvdb_tree_error(err, TVDB_ERROR_INVALID_DATA,
                       "prune allocation size overflow");
  goto done;
oom:
  st =
      tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY, "prune allocation failed");
done:
  free(nodes);
  free(uniform);
  free(active);
  free(keep);
  free(value);
  free(map);
  tvdb_grid_destroy_owned(&d);
  tvdb_tree_index_destroy(&p);
  return st;
}

tvdb_status_t tvdb_grid_to_sparse_ex(const tvdb_grid_t* g,
                                     tvdb_sparse_grid* out, tvdb_error_t* err) {
  if (!out)
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "NULL sparse output");
  tvdb_tree_index p;
  tvdb_status_t st = tvdb_tree_index_create(g, 1, &p, err);
  if (st != TVDB_OK) return st;
  tvdb_sparse_grid d = {0};
  float origin[3];
  if (p.active_tiles) {
    st = tvdb_tree_error(
        err, TVDB_ERROR_UNIMPLEMENTED,
        "flat extraction requires explicit active-tile expansion");
    goto done;
  }
  if (!tvdb_tree_dense_geometry(&g->transform, &d.voxel_size, origin)) {
    st = tvdb_tree_error(
        err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
        "flat grids require positive axis-aligned uniform scale");
    goto done;
  }
  d.ox = origin[0];
  d.oy = origin[1];
  d.oz = origin[2];
  if (!tvdb_sparse_grid_reserve(&d, p.active_voxels)) {
    st = tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                         "sparse extraction allocation failed");
    goto done;
  }
  for (size_t at = 1; at < g->tree.num_nodes; ++at) {
    size_t ni = p.order[at];
    const tvdb_tree_node_t* n = g->tree.nodes + ni;
    if (n->type != TVDB_NODE_LEAF) continue;
    int L = g->tree.layout.levels[n->level].log2dim, dim = 1 << L;
    size_t slots = (size_t)1 << (3 * L);
    for (size_t s = 0; s < slots; ++s)
      if (mask_on(&n->u.leaf.value_mask, s)) {
        tvdb_vec3i* c = d.coords + d.count;
        c->x =
            (int32_t)((int64_t)p.origins[ni][0] + ((s >> (2 * L)) & (dim - 1)));
        c->y = (int32_t)((int64_t)p.origins[ni][1] + ((s >> L) & (dim - 1)));
        c->z = (int32_t)((int64_t)p.origins[ni][2] + (s & (dim - 1)));
        memcpy(d.values + d.count, n->u.leaf.data + s * 4, 4);
        ++d.count;
      }
  }
  tvdb_sparse_grid_free(out);
  *out = d;
  memset(&d, 0, sizeof(d));
  st = tvdb_tree_error(err, TVDB_OK, "");
done:
  tvdb_sparse_grid_free(&d);
  tvdb_tree_index_destroy(&p);
  return st;
}
static void materialize_region(tvdb_dense_grid* d, const int32_t min[3],
                               const int32_t max[3], const int32_t origin[3],
                               int64_t span, float value) {
  int64_t lo[3], hi[3];
  for (int a = 0; a < 3; ++a) {
    lo[a] = origin[a] > min[a] ? origin[a] : min[a];
    int64_t end = (int64_t)origin[a] + span;
    hi[a] = end < max[a] ? end : max[a];
  }
  for (int a = 0; a < 3; ++a)
    if (lo[a] >= hi[a]) return;
  for (int64_t z = lo[2]; z < hi[2]; ++z)
    for (int64_t y = lo[1]; y < hi[1]; ++y)
      for (int64_t x = lo[0]; x < hi[0]; ++x) {
        size_t s =
            ((size_t)(z - min[2]) * d->ny + (size_t)(y - min[1])) * d->nx +
            (size_t)(x - min[0]);
        d->data[s] = value;
      }
}
tvdb_status_t tvdb_grid_materialize_dense_ex(
    const tvdb_grid_t* g, const int32_t min[3], const int32_t max[3],
    float background, tvdb_dense_grid* out, tvdb_error_t* err) {
  if (!min || !max || !out || !isfinite(background))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "invalid materialization arguments");
  int dims[3];
  size_t bytes;
  for (int a = 0; a < 3; ++a) {
    int64_t delta = (int64_t)max[a] - min[a];
    if (delta <= 0 || delta > INT_MAX)
      return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                             "invalid/overflowing dense bounds");
    dims[a] = (int)delta;
  }
  if (!tvdb_grid_bytes(dims[0], dims[1], dims[2], 4, &bytes))
    return tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "dense allocation size overflow");
  tvdb_tree_index p;
  tvdb_status_t st = tvdb_tree_index_create(g, 1, &p, err);
  if (st != TVDB_OK) return st;
  tvdb_dense_grid d = {0};
  d.nx = dims[0];
  d.ny = dims[1];
  d.nz = dims[2];
  float origin[3];
  if (!tvdb_tree_dense_geometry(&g->transform, &d.voxel_size, origin)) {
    st = tvdb_tree_error(
        err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
        "dense grids require positive axis-aligned uniform scale");
    goto done;
  }
  float* coords[3] = {&d.ox, &d.oy, &d.oz};
  for (int a = 0; a < 3; ++a) {
    double v = (double)origin[a] + (double)min[a] * d.voxel_size;
    if (!isfinite(v) || fabs(v) > FLT_MAX) {
      st = tvdb_tree_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                           "dense origin overflow");
      goto done;
    }
    *coords[a] = (float)v;
  }
  d.data = malloc(bytes);
  if (!d.data) {
    st = tvdb_tree_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                         "dense materialization allocation failed");
    goto done;
  }
  for (size_t i = 0; i < bytes / 4; ++i) d.data[i] = background;
  const tvdb_root_node_t* r = &g->tree.nodes[0].u.root;
  for (size_t i = 0; i < r->num_tiles; ++i)
    if (r->tile_active[i])
      materialize_region(&d, min, max, r->tile_origins + 3 * i, p.span[1],
                         r->tile_values[i].u.f);
  for (size_t ni = 1; ni < g->tree.num_nodes; ++ni) {
    const tvdb_tree_node_t* n = g->tree.nodes + ni;
    int L = g->tree.layout.levels[n->level].log2dim, dim = 1 << L;
    int leaf = n->type == TVDB_NODE_LEAF;
    size_t slots = (size_t)1 << (3 * L);
    for (size_t s = 0; s < slots; ++s) {
      if (!leaf && mask_on(&n->u.internal.child_mask, s)) continue;
      if (!mask_on(leaf ? &n->u.leaf.value_mask : &n->u.internal.value_mask, s))
        continue;
      int64_t step = leaf ? 1 : p.span[n->level + 1];
      int32_t c[3];
      float v;
      for (int a = 0; a < 3; ++a)
        c[a] = (int32_t)((int64_t)p.origins[ni][a] +
                         (int64_t)((s >> ((2 - a) * L)) & (dim - 1)) * step);
      v = *(const float*)((leaf ? n->u.leaf.data : n->u.internal.values) + s * 4);
      materialize_region(&d, min, max, c, step, v);
    }
  }
  tvdb_dense_grid_free(out);
  *out = d;
  memset(&d, 0, sizeof(d));
  st = tvdb_tree_error(err, TVDB_OK, "");
done:
  tvdb_dense_grid_free(&d);
  tvdb_tree_index_destroy(&p);
  return st;
}
