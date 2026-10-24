/*
 * tinyvdb_to_nanovdb.c — convert a loaded VDB grid (5_4_3 tree of float,
 * double, int32, int64, vec3f, vec3d or mask values) into an in-memory
 * NanoVDB grid byte buffer (version 32.9 layout).
 *
 * Layout produced, breadth first:
 *
 *   [GridData 672][TreeData 64][RootData][RootTile * (uppers + root tiles)]
 *   [Upper * U][Lower * L][Leaf * F]
 *
 * Node sizes and field offsets per value type come from PNanoVDB.h's
 * pnanovdb_grid_type_constants table, so they match the reader GPU code
 * uses. The map is the grid's full affine index-to-world transform; the
 * class comes from the "class" metadata; node bounding boxes are the exact
 * active boxes. Leaf min/max/average/deviation are filled for float and
 * double grids (the grid flags claim bounding boxes only). The checksum is
 * "empty" (0xFFFFFFFFFFFFFFFF), which NanoVDB treats as unchecked.
 */

#include "tinyvdb_io.h"

#define PNANOVDB_C
#include "third_party/nanovdb/PNanoVDB.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls matching tinyvdb_nanovdb.h. Defined here. */
tvdb_status_t tvdb_grid_to_nanovdb_float(const tvdb_grid_t *grid,
                                          uint8_t **out_data,
                                          size_t *out_size,
                                          tvdb_error_t *err);
tvdb_status_t tvdb_grid_to_nanovdb(const tvdb_grid_t *grid, uint8_t **out_data,
                                   size_t *out_size, tvdb_error_t *err);

#define NV_GRID_BYTES        672u
#define NV_TREE_BYTES         64u
#define NV_ROOT_FLOAT_BYTES   64u
#define NV_TILE_FLOAT_BYTES   32u
#define NV_UPPER_FLOAT_BYTES  270400u
#define NV_LOWER_FLOAT_BYTES   33856u
#define NV_LEAF_FLOAT_BYTES     2144u

/* Field offsets within each NanoVDB node (FloatGrid). */
#define NV_UPPER_BBOX_MIN_OFF      0u
#define NV_UPPER_BBOX_MAX_OFF     12u
#define NV_UPPER_FLAGS_OFF        24u
#define NV_UPPER_VALUE_MASK_OFF   32u
#define NV_UPPER_CHILD_MASK_OFF 4128u
#define NV_UPPER_MIN_OFF        8224u
#define NV_UPPER_TABLE_OFF      8256u

#define NV_LOWER_BBOX_MIN_OFF      0u
#define NV_LOWER_BBOX_MAX_OFF     12u
#define NV_LOWER_FLAGS_OFF        24u
#define NV_LOWER_VALUE_MASK_OFF   32u
#define NV_LOWER_CHILD_MASK_OFF  544u
#define NV_LOWER_MIN_OFF        1056u
#define NV_LOWER_TABLE_OFF      1088u

#define NV_LEAF_BBOX_MIN_OFF       0u
#define NV_LEAF_BBOX_DIF_FLAGS_OFF 12u
#define NV_LEAF_VALUE_MASK_OFF    16u
#define NV_LEAF_MIN_OFF           80u
#define NV_LEAF_MAX_OFF           84u
#define NV_LEAF_AVE_OFF           88u
#define NV_LEAF_STDDEV_OFF        92u
#define NV_LEAF_TABLE_OFF         96u

#define NV_TREE_OFF_LEAF_OFF       0u
#define NV_TREE_OFF_LOWER_OFF      8u
#define NV_TREE_OFF_UPPER_OFF     16u
#define NV_TREE_OFF_ROOT_OFF      24u
#define NV_TREE_NODE_COUNT_LEAF   32u
#define NV_TREE_NODE_COUNT_LOWER  36u
#define NV_TREE_NODE_COUNT_UPPER  40u
#define NV_TREE_TILE_COUNT_LOWER  44u
#define NV_TREE_TILE_COUNT_UPPER  48u
#define NV_TREE_TILE_COUNT_ROOT   52u
#define NV_TREE_VOXEL_COUNT       56u

#define NV_ROOT_BBOX_MIN_OFF       0u
#define NV_ROOT_BBOX_MAX_OFF      12u
#define NV_ROOT_TABLE_SIZE_OFF    24u
#define NV_ROOT_BG_OFF            28u  /* float */
#define NV_ROOT_MIN_OFF           32u
#define NV_ROOT_MAX_OFF           36u

#define NV_TILE_KEY_OFF            0u
#define NV_TILE_CHILD_OFF          8u
#define NV_TILE_STATE_OFF         16u
#define NV_TILE_VALUE_OFF         20u

#define NV_GRID_OFF_MAGIC          0u
#define NV_GRID_OFF_CHECKSUM       8u
#define NV_GRID_OFF_VERSION       16u
#define NV_GRID_OFF_FLAGS         20u
#define NV_GRID_OFF_GRID_INDEX    24u
#define NV_GRID_OFF_GRID_COUNT    28u
#define NV_GRID_OFF_GRID_SIZE     32u
#define NV_GRID_OFF_GRID_NAME     40u
#define NV_GRID_OFF_MAP          296u
#define NV_GRID_OFF_WORLD_BBOX   560u
#define NV_GRID_OFF_VOXEL_SIZE   608u
#define NV_GRID_OFF_GRID_CLASS   632u
#define NV_GRID_OFF_GRID_TYPE    636u
#define NV_GRID_OFF_BLIND_OFF    640u
#define NV_GRID_OFF_BLIND_COUNT  648u

#define NV_MAGIC_NUMBER 0x304244566f6e614eULL  /* "NanoVDB0" */
#define NV_GRID_TYPE_FLOAT 1u

/* version (Major,Minor,Patch) packed (Major<<21 | Minor<<10 | Patch). */
#define NV_VERSION_MAJOR 32u
#define NV_VERSION_MINOR 9u
#define NV_VERSION_PATCH 1u

static uint32_t nv__make_version(void) {
    return (NV_VERSION_MAJOR << 21) | (NV_VERSION_MINOR << 10) | NV_VERSION_PATCH;
}

static void nv__write_u32(uint8_t *p, size_t off, uint32_t v) {
    memcpy(p + off, &v, 4);
}
static void nv__write_u64(uint8_t *p, size_t off, uint64_t v) {
    memcpy(p + off, &v, 8);
}
static void nv__write_i32(uint8_t *p, size_t off, int32_t v) {
    memcpy(p + off, &v, 4);
}
static void nv__write_i64(uint8_t *p, size_t off, int64_t v) {
    memcpy(p + off, &v, 8);
}
static void nv__write_f32(uint8_t *p, size_t off, float v) {
    memcpy(p + off, &v, 4);
}
static void nv__write_f64(uint8_t *p, size_t off, double v) {
    memcpy(p + off, &v, 8);
}

static void nv__set_bit(uint8_t *mask, size_t bit) {
    mask[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

/* Node-walking state: collected during a DFS pass over the VDB tree. */
typedef struct nv_node_info {
    int32_t origin[3];     /* lower-corner ijk in world index space */
    size_t  vdb_node_idx;  /* index into grid->tree.nodes[] */
    size_t  parent_idx;    /* index into the same per-level array */
    uint32_t parent_slot;  /* slot index within parent's child table */
} nv_node_info_t;

typedef struct nv_build_ctx {
    nv_node_info_t *uppers;  size_t n_upper, cap_upper;
    nv_node_info_t *lowers;  size_t n_lower, cap_lower;
    nv_node_info_t *leaves;  size_t n_leaf,  cap_leaf;
} nv_build_ctx_t;

static int nv__push(nv_node_info_t **arr, size_t *n, size_t *cap,
                    nv_node_info_t info) {
    if (*n >= *cap) {
        size_t nc = (*cap == 0) ? 64 : (*cap * 2);
        nv_node_info_t *p = (nv_node_info_t *)realloc(*arr, nc * sizeof(*p));
        if (!p) return 0;
        *arr = p; *cap = nc;
    }
    (*arr)[(*n)++] = info;
    return 1;
}

/* Decompose VDB internal-node child slot index into (ix,iy,iz) given log2dim. */
static void nv__slot_to_ijk(int32_t s, int log2dim,
                            int32_t *ix, int32_t *iy, int32_t *iz) {
    int32_t mask = (1 << log2dim) - 1;
    *ix = (s >> (2 * log2dim)) & mask;
    *iy = (s >> log2dim) & mask;
    *iz =  s & mask;
}

/* DFS walk: collect uppers/lowers/leaves with origins and parent linkage. */
static int nv__collect(const tvdb_tree_t *tree, nv_build_ctx_t *ctx) {
    if (tree->num_nodes == 0) return 1;

    /* Walk root → upper(level 1) → lower(level 2) → leaf(level 3). */
    const tvdb_tree_node_t *root = &tree->nodes[0];
    if (root->type != TVDB_NODE_ROOT) return 0;
    const tvdb_root_node_t *r = &root->u.root;

    /* Upper (level 1) children of root. */
    for (uint32_t i = 0; i < r->num_children; ++i) {
        nv_node_info_t up;
        up.origin[0] = r->child_origins[3 * i + 0];
        up.origin[1] = r->child_origins[3 * i + 1];
        up.origin[2] = r->child_origins[3 * i + 2];
        up.vdb_node_idx = r->child_indices[i];
        up.parent_idx = 0;
        up.parent_slot = i;  /* root tile index */
        if (!nv__push(&ctx->uppers, &ctx->n_upper, &ctx->cap_upper, up)) return 0;
    }

    /* For each upper, enumerate lowers. */
    for (size_t u = 0; u < ctx->n_upper; ++u) {
        const tvdb_tree_node_t *un = &tree->nodes[ctx->uppers[u].vdb_node_idx];
        if (un->type != TVDB_NODE_INTERNAL) continue;
        const tvdb_internal_node_t *ui = &un->u.internal;
        int32_t log2dim = ui->child_mask.log2dim;  /* 5 */
        int32_t total = 1 << (3 * log2dim);
        size_t c_idx = 0;
        for (int32_t s = 0; s < total && c_idx < ui->num_children; ++s) {
            if (!(ui->child_mask.bits.data[s >> 3] & (1u << (s & 7u)))) continue;
            int32_t ix, iy, iz;
            nv__slot_to_ijk(s, log2dim, &ix, &iy, &iz);
            /* Lower covers 16 * 8 = 128 voxels per axis. */
            int32_t child_dim = 1 << (4 + 3);
            nv_node_info_t lo;
            lo.origin[0] = ctx->uppers[u].origin[0] + ix * child_dim;
            lo.origin[1] = ctx->uppers[u].origin[1] + iy * child_dim;
            lo.origin[2] = ctx->uppers[u].origin[2] + iz * child_dim;
            lo.vdb_node_idx = ui->child_indices[c_idx];
            lo.parent_idx = u;
            lo.parent_slot = (uint32_t)s;
            if (!nv__push(&ctx->lowers, &ctx->n_lower, &ctx->cap_lower, lo)) return 0;
            ++c_idx;
        }
    }

    /* For each lower, enumerate leaves. */
    for (size_t l = 0; l < ctx->n_lower; ++l) {
        const tvdb_tree_node_t *ln = &tree->nodes[ctx->lowers[l].vdb_node_idx];
        if (ln->type != TVDB_NODE_INTERNAL) continue;
        const tvdb_internal_node_t *li = &ln->u.internal;
        int32_t log2dim = li->child_mask.log2dim;  /* 4 */
        int32_t total = 1 << (3 * log2dim);
        size_t c_idx = 0;
        for (int32_t s = 0; s < total && c_idx < li->num_children; ++s) {
            if (!(li->child_mask.bits.data[s >> 3] & (1u << (s & 7u)))) continue;
            int32_t ix, iy, iz;
            nv__slot_to_ijk(s, log2dim, &ix, &iy, &iz);
            int32_t child_dim = 1 << 3;  /* leaf covers 8 voxels per axis */
            nv_node_info_t lf;
            lf.origin[0] = ctx->lowers[l].origin[0] + ix * child_dim;
            lf.origin[1] = ctx->lowers[l].origin[1] + iy * child_dim;
            lf.origin[2] = ctx->lowers[l].origin[2] + iz * child_dim;
            lf.vdb_node_idx = li->child_indices[c_idx];
            lf.parent_idx = l;
            lf.parent_slot = (uint32_t)s;
            if (!nv__push(&ctx->leaves, &ctx->n_leaf, &ctx->cap_leaf, lf)) return 0;
            ++c_idx;
        }
    }
    return 1;
}

/* NanoVDB-style root-tile key from world-space ijk. */
static uint64_t nv__coord_to_key(int32_t x, int32_t y, int32_t z) {
    uint64_t iu = (uint32_t)x >> 12u;
    uint64_t ju = (uint32_t)y >> 12u;
    uint64_t ku = (uint32_t)z >> 12u;
    return ku | (ju << 21) | (iu << 42);
}

/* NanoVDB Upper child slot from world-ijk relative to upper origin (32^3). */
static uint32_t nv__upper_slot(int32_t ix, int32_t iy, int32_t iz) {
    /* upper log2dim=5, so each axis shifts by 5 within upper-local 32^3 */
    return (uint32_t)((ix << 10) | (iy << 5) | iz);
}
static uint32_t nv__lower_slot(int32_t ix, int32_t iy, int32_t iz) {
    return (uint32_t)((ix << 8) | (iy << 4) | iz);
}
static uint32_t nv__leaf_slot(int32_t ix, int32_t iy, int32_t iz) {
    return (uint32_t)((ix << 6) | (iy << 3) | iz);
}


/* Index -> world 4x4 (column vectors), as tvdb_tree_matrix builds it; this
 * library links only the I/O core. 0 for nonlinear or singular maps. */
static int nv__matrix(const tvdb_transform_t *t, double m[4][4]) {
    memset(m, 0, 16 * sizeof(double));
    for (int a = 0; a < 4; ++a) m[a][a] = 1;
    switch (t->type) {
        case TVDB_TRANSFORM_AFFINE:
            memcpy(m, t->matrix, 16 * sizeof(double));
            break;
        case TVDB_TRANSFORM_UNIFORM_SCALE:
        case TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE:
            for (int a = 0; a < 3; ++a) m[a][a] = t->voxel_size[0];
            if (t->type == TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE)
                for (int a = 0; a < 3; ++a) m[a][3] = t->translation[a];
            break;
        case TVDB_TRANSFORM_SCALE:
        case TVDB_TRANSFORM_SCALE_TRANSLATE:
            for (int a = 0; a < 3; ++a) m[a][a] = t->voxel_size[a];
            if (t->type == TVDB_TRANSFORM_SCALE_TRANSLATE)
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
    const double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return isfinite(d) && d != 0;
}

static tvdb_status_t nv__error(tvdb_error_t *err, tvdb_status_t st, const char *msg) {
    if (err) {
        memset(err, 0, sizeof(*err));
        err->status = st;
        err->grid_index = -1;
        snprintf(err->message, sizeof(err->message), "%s", msg);
    }
    return st;
}

/* NanoVDB grid type for a tinyvdb value type (0: not convertible). */
static uint32_t nv__grid_type(const tvdb_grid_t *g) {
    const tvdb_value_type_t vt = g->tree.layout.levels[3].value_type;
    if (g->tree.is_mask_grid) return PNANOVDB_GRID_TYPE_MASK;
    switch (vt) {
        case TVDB_VALUE_FLOAT:  return PNANOVDB_GRID_TYPE_FLOAT;
        case TVDB_VALUE_DOUBLE: return PNANOVDB_GRID_TYPE_DOUBLE;
        case TVDB_VALUE_INT32:  return PNANOVDB_GRID_TYPE_INT32;
        case TVDB_VALUE_INT64:  return PNANOVDB_GRID_TYPE_INT64;
        case TVDB_VALUE_VEC3F:  return PNANOVDB_GRID_TYPE_VEC3F;
        case TVDB_VALUE_VEC3D:  return PNANOVDB_GRID_TYPE_VEC3D;
        default:                return 0;
    }
}

/* Bytes of one stored value in NanoVDB (0 for masks). The tinyvdb in-memory
 * layout of these types is the NanoVDB one (native little-endian). */
static size_t nv__value_bytes(uint32_t gt) {
    switch (gt) {
        case PNANOVDB_GRID_TYPE_FLOAT: case PNANOVDB_GRID_TYPE_INT32: return 4;
        case PNANOVDB_GRID_TYPE_DOUBLE: case PNANOVDB_GRID_TYPE_INT64: return 8;
        case PNANOVDB_GRID_TYPE_VEC3F: return 12;
        case PNANOVDB_GRID_TYPE_VEC3D: return 24;
        default: return 0;
    }
}

static void nv__write_tvdb_value(uint8_t *buf, size_t off, uint32_t gt, const tvdb_value_t *v) {
    switch (gt) {
        case PNANOVDB_GRID_TYPE_FLOAT:  memcpy(buf + off, &v->u.f, 4); break;
        case PNANOVDB_GRID_TYPE_DOUBLE: memcpy(buf + off, &v->u.d, 8); break;
        case PNANOVDB_GRID_TYPE_INT32:  memcpy(buf + off, &v->u.i32, 4); break;
        case PNANOVDB_GRID_TYPE_INT64:  memcpy(buf + off, &v->u.i64, 8); break;
        case PNANOVDB_GRID_TYPE_VEC3F:  memcpy(buf + off, v->u.vec3f, 12); break;
        case PNANOVDB_GRID_TYPE_VEC3D:  memcpy(buf + off, v->u.vec3d, 24); break;
        default: break;
    }
}

typedef struct { int32_t lo[3], hi[3]; int any; } nv_box;
static void nv__box_add(nv_box *b, const int32_t lo[3], const int32_t hi[3]) {
    for (int k = 0; k < 3; ++k) {
        if (!b->any || lo[k] < b->lo[k]) b->lo[k] = lo[k];
        if (!b->any || hi[k] > b->hi[k]) b->hi[k] = hi[k];
    }
    b->any = 1;
}
static void nv__write_box(uint8_t *buf, size_t off, const nv_box *b, const int32_t origin[3]) {
    for (int k = 0; k < 3; ++k) {
        nv__write_i32(buf, off + 4 * (size_t)k, b->any ? b->lo[k] : origin[k]);
        nv__write_i32(buf, off + 12 + 4 * (size_t)k, b->any ? b->hi[k] : origin[k]);
    }
}

typedef struct { uint64_t key; int is_child; size_t index; } nv_root_entry;
static int nv__root_entry_cmp(const void *a, const void *b) {
    const uint64_t x = ((const nv_root_entry *)a)->key, y = ((const nv_root_entry *)b)->key;
    return (x > y) - (x < y);
}

/* An internal node (upper or lower) and its children's boxes. */
static void nv__write_internal(uint8_t *buf, size_t off, const pnanovdb_grid_type_constants_t *C,
                               int upper, uint32_t gt, size_t vbytes, const tvdb_internal_node_t *in,
                               const int32_t origin[3], const nv_node_info_t *children,
                               size_t child_begin, size_t child_end, size_t child_off0,
                               size_t child_size, const nv_box *child_boxes, nv_box *box,
                               uint32_t *active_tiles) {
    const int L = upper ? 5 : 4, sub = upper ? 128 : 8;
    const size_t mask_bytes = upper ? 4096u : 512u;
    const size_t off_value_mask = 32, off_child_mask = 32 + mask_bytes;
    const size_t off_table = upper ? C->upper_off_table : C->lower_off_table;
    const int32_t total = 1 << (3 * L);
    size_t c = child_begin;
    for (int32_t s = 0; s < total; ++s) {
        int32_t ix, iy, iz;
        nv__slot_to_ijk(s, L, &ix, &iy, &iz);
        const size_t entry = off + off_table + (size_t)s * C->table_stride;
        if (in->child_mask.bits.data[s >> 3] & (1u << (s & 7u))) {
            if (c < child_end && children[c].parent_slot == (uint32_t)s) {
                nv__set_bit(buf + off + off_child_mask, (size_t)s);
                nv__write_i64(buf, entry, (int64_t)(child_off0 + c * child_size) - (int64_t)off);
                if (child_boxes[c].any) nv__box_add(box, child_boxes[c].lo, child_boxes[c].hi);
                ++c;
            }
            continue;
        }
        const int on = (in->value_mask.bits.data[s >> 3] >> (s & 7u)) & 1;
        if (on) {
            nv__set_bit(buf + off + off_value_mask, (size_t)s);
            const int32_t lo[3] = {origin[0] + ix * sub, origin[1] + iy * sub, origin[2] + iz * sub};
            const int32_t hi[3] = {lo[0] + sub - 1, lo[1] + sub - 1, lo[2] + sub - 1};
            nv__box_add(box, lo, hi);
            ++*active_tiles;
        }
        if (vbytes && in->values && in->values_size >= (size_t)total * vbytes)
            memcpy(buf + entry, in->values + (size_t)s * vbytes, vbytes);
    }
    (void)gt;
    nv__write_box(buf, off, box, origin);
}

tvdb_status_t tvdb_grid_to_nanovdb(const tvdb_grid_t *grid, uint8_t **out_data,
                                   size_t *out_size, tvdb_error_t *err) {
    if (!grid || !out_data || !out_size)
        return nv__error(err, TVDB_ERROR_INVALID_ARGUMENT, "null arg");
    *out_data = NULL;
    *out_size = 0;
    if (grid->tree.layout.num_levels != 4 || grid->tree.layout.levels[0].log2dim != 0 ||
        grid->tree.layout.levels[1].log2dim != 5 || grid->tree.layout.levels[2].log2dim != 4 ||
        grid->tree.layout.levels[3].log2dim != 3 || !grid->tree.nodes || !grid->tree.num_nodes ||
        grid->tree.is_point_data_grid || grid->tree.is_point_index_grid)
        return nv__error(err, TVDB_ERROR_UNSUPPORTED_VERSION,
                         "tvdb_grid_to_nanovdb: needs a decoded 5_4_3 voxel tree");
    const uint32_t gt = nv__grid_type(grid);
    if (!gt)
        return nv__error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
                         "tvdb_grid_to_nanovdb: float, double, int32, int64, vec3f, vec3d or mask only");
    double m[4][4];
    if (!nv__matrix(&grid->transform, m))
        return nv__error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM, "grid transform is not linear");
    const pnanovdb_grid_type_constants_t *C = &pnanovdb_grid_type_constants[gt];
    const size_t vbytes = nv__value_bytes(gt);

    nv_build_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!nv__collect(&grid->tree, &ctx)) {
        free(ctx.uppers); free(ctx.lowers); free(ctx.leaves);
        return nv__error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM in nv__collect");
    }
    const tvdb_root_node_t *r = &grid->tree.nodes[0].u.root;
    const size_t n_tiles = ctx.n_upper + r->num_tiles;
    const size_t off_grid = 0, off_tree = NV_GRID_BYTES, off_root = off_tree + NV_TREE_BYTES;
    const size_t off_tile0 = off_root + C->root_size;
    const size_t off_up0 = off_tile0 + n_tiles * C->root_tile_size;
    const size_t off_lo0 = off_up0 + ctx.n_upper * C->upper_size;
    const size_t off_lf0 = off_lo0 + ctx.n_lower * C->lower_size;
    const size_t total = off_lf0 + ctx.n_leaf * C->leaf_size;
    const size_t aligned_total = (total + 31u) & ~(size_t)31u;
    uint8_t *buf = (uint8_t *)calloc(1, aligned_total);
    nv_box *leaf_box = (nv_box *)calloc(ctx.n_leaf ? ctx.n_leaf : 1, sizeof(nv_box));
    nv_box *lower_box = (nv_box *)calloc(ctx.n_lower ? ctx.n_lower : 1, sizeof(nv_box));
    nv_box *upper_box = (nv_box *)calloc(ctx.n_upper ? ctx.n_upper : 1, sizeof(nv_box));
    nv_root_entry *entries = (nv_root_entry *)calloc(n_tiles ? n_tiles : 1, sizeof(nv_root_entry));
    if (!buf || !leaf_box || !lower_box || !upper_box || !entries) {
        free(buf); free(leaf_box); free(lower_box); free(upper_box); free(entries);
        free(ctx.uppers); free(ctx.lowers); free(ctx.leaves);
        return nv__error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM allocating output buffer");
    }

    /* Leaves. */
    uint64_t total_active = 0;
    for (size_t lf = 0; lf < ctx.n_leaf; ++lf) {
        const nv_node_info_t *li = &ctx.leaves[lf];
        const size_t off = off_lf0 + lf * C->leaf_size;
        const tvdb_leaf_node_t *vl = &grid->tree.nodes[li->vdb_node_idx].u.leaf;
        if (vl->value_mask.bits.num_bytes < 64u || !vl->data ||
            vl->data_size < 512u * (vbytes ? vbytes : 1u)) {
            free(buf); free(leaf_box); free(lower_box); free(upper_box); free(entries);
            free(ctx.uppers); free(ctx.lowers); free(ctx.leaves);
            return nv__error(err, TVDB_ERROR_INVALID_DATA, "tvdb_grid_to_nanovdb: leaf without 8^3 data");
        }
        memcpy(buf + off + NV_LEAF_VALUE_MASK_OFF, vl->value_mask.bits.data, 64);
        if (vbytes) memcpy(buf + off + C->leaf_off_table, vl->data, 512u * vbytes);
        nv_box *b = &leaf_box[lf];
        double vmin = INFINITY, vmax = -INFINITY, vsum = 0, vsq = 0;
        size_t active = 0;
        for (int32_t n = 0; n < 512; ++n) {
            if (!(vl->value_mask.bits.data[n >> 3] & (1u << (n & 7)))) continue;
            ++active;
            const int32_t c[3] = {li->origin[0] + (n >> 6), li->origin[1] + ((n >> 3) & 7),
                                  li->origin[2] + (n & 7)};
            nv__box_add(b, c, c);
            if (gt == PNANOVDB_GRID_TYPE_FLOAT || gt == PNANOVDB_GRID_TYPE_DOUBLE) {
                double v;
                if (gt == PNANOVDB_GRID_TYPE_FLOAT) { float f; memcpy(&f, vl->data + 4u * (size_t)n, 4); v = f; }
                else memcpy(&v, vl->data + 8u * (size_t)n, 8);
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
                vsum += v;
                vsq += v * v;
            }
        }
        total_active += active;
        /* bbox min, then (max - min) per axis in the low 24 bits and the
           flags byte (bit 0 as written by NanoVDB's builder). */
        const int32_t org[3] = {li->origin[0], li->origin[1], li->origin[2]};
        for (int k = 0; k < 3; ++k) nv__write_i32(buf, off + 4 * (size_t)k, b->any ? b->lo[k] : org[k]);
        uint32_t dif = 0;
        for (int k = 0; k < 3 && b->any; ++k) dif |= (uint32_t)(b->hi[k] - b->lo[k]) << (8 * k);
        nv__write_u32(buf, off + NV_LEAF_BBOX_DIF_FLAGS_OFF, dif | (1u << 24));
        if (active && (gt == PNANOVDB_GRID_TYPE_FLOAT || gt == PNANOVDB_GRID_TYPE_DOUBLE)) {
            const double mean = vsum / (double)active, var = vsq / (double)active - mean * mean;
            const double dev = var > 0 ? sqrt(var) : 0;
            if (gt == PNANOVDB_GRID_TYPE_FLOAT) {
                nv__write_f32(buf, off + C->leaf_off_min, (float)vmin);
                nv__write_f32(buf, off + C->leaf_off_max, (float)vmax);
                nv__write_f32(buf, off + C->leaf_off_ave, (float)mean);
                nv__write_f32(buf, off + C->leaf_off_stddev, (float)dev);
            } else {
                nv__write_f64(buf, off + C->leaf_off_min, vmin);
                nv__write_f64(buf, off + C->leaf_off_max, vmax);
                nv__write_f64(buf, off + C->leaf_off_ave, mean);
                nv__write_f64(buf, off + C->leaf_off_stddev, dev);
            }
        }
    }

    /* Lowers, then uppers (children are contiguous per parent). */
    uint32_t lower_tiles = 0, upper_tiles = 0, root_active_tiles = 0;
    size_t cursor = 0;
    for (size_t l = 0; l < ctx.n_lower; ++l) {
        const size_t begin = cursor;
        while (cursor < ctx.n_leaf && ctx.leaves[cursor].parent_idx == l) ++cursor;
        nv__write_internal(buf, off_lo0 + l * C->lower_size, C, 0, gt, vbytes,
                           &grid->tree.nodes[ctx.lowers[l].vdb_node_idx].u.internal,
                           ctx.lowers[l].origin, ctx.leaves, begin, cursor, off_lf0, C->leaf_size,
                           leaf_box, &lower_box[l], &lower_tiles);
    }
    cursor = 0;
    for (size_t u = 0; u < ctx.n_upper; ++u) {
        const size_t begin = cursor;
        while (cursor < ctx.n_lower && ctx.lowers[cursor].parent_idx == u) ++cursor;
        nv__write_internal(buf, off_up0 + u * C->upper_size, C, 1, gt, vbytes,
                           &grid->tree.nodes[ctx.uppers[u].vdb_node_idx].u.internal,
                           ctx.uppers[u].origin, ctx.lowers, begin, cursor, off_lo0, C->lower_size,
                           lower_box, &upper_box[u], &upper_tiles);
    }

    /* Root table: children and tiles, sorted by key. */
    nv_box root_box;
    memset(&root_box, 0, sizeof(root_box));
    for (size_t u = 0; u < ctx.n_upper; ++u) {
        entries[u].key = nv__coord_to_key(ctx.uppers[u].origin[0], ctx.uppers[u].origin[1],
                                          ctx.uppers[u].origin[2]);
        entries[u].is_child = 1;
        entries[u].index = u;
        if (upper_box[u].any) nv__box_add(&root_box, upper_box[u].lo, upper_box[u].hi);
    }
    for (uint32_t t = 0; t < r->num_tiles; ++t) {
        const int32_t *o = r->tile_origins + 3 * t;
        nv_root_entry *e = &entries[ctx.n_upper + t];
        e->key = nv__coord_to_key(o[0], o[1], o[2]);
        e->index = t;
        if (r->tile_active[t]) {
            const int32_t hi[3] = {o[0] + 4095, o[1] + 4095, o[2] + 4095};
            nv__box_add(&root_box, o, hi);
            ++root_active_tiles;
        }
    }
    qsort(entries, n_tiles, sizeof(*entries), nv__root_entry_cmp);
    for (size_t i = 0; i < n_tiles; ++i) {
        const size_t off = off_tile0 + i * C->root_tile_size;
        nv__write_u64(buf, off + NV_TILE_KEY_OFF, entries[i].key);
        if (entries[i].is_child) {
            const size_t up = off_up0 + entries[i].index * C->upper_size;
            nv__write_i64(buf, off + NV_TILE_CHILD_OFF, (int64_t)up - (int64_t)off_root);
            nv__write_tvdb_value(buf, off + C->root_tile_off_value, gt, &r->background);
        } else {
            nv__write_i64(buf, off + NV_TILE_CHILD_OFF, 0);
            nv__write_u32(buf, off + NV_TILE_STATE_OFF, r->tile_active[entries[i].index] ? 1u : 0u);
            nv__write_tvdb_value(buf, off + C->root_tile_off_value, gt, &r->tile_values[entries[i].index]);
        }
    }
    {
        const int32_t zero[3] = {0, 0, 0};
        nv__write_box(buf, off_root + NV_ROOT_BBOX_MIN_OFF, &root_box, zero);
    }
    nv__write_u32(buf, off_root + NV_ROOT_TABLE_SIZE_OFF, (uint32_t)n_tiles);
    nv__write_tvdb_value(buf, off_root + C->root_off_background, gt, &r->background);

    /* TreeData. */
    nv__write_u64(buf, off_tree + NV_TREE_OFF_LEAF_OFF, ctx.n_leaf ? (uint64_t)(off_lf0 - off_tree) : 0);
    nv__write_u64(buf, off_tree + NV_TREE_OFF_LOWER_OFF, ctx.n_lower ? (uint64_t)(off_lo0 - off_tree) : 0);
    nv__write_u64(buf, off_tree + NV_TREE_OFF_UPPER_OFF, ctx.n_upper ? (uint64_t)(off_up0 - off_tree) : 0);
    nv__write_u64(buf, off_tree + NV_TREE_OFF_ROOT_OFF, NV_TREE_BYTES);
    nv__write_u32(buf, off_tree + NV_TREE_NODE_COUNT_LEAF, (uint32_t)ctx.n_leaf);
    nv__write_u32(buf, off_tree + NV_TREE_NODE_COUNT_LOWER, (uint32_t)ctx.n_lower);
    nv__write_u32(buf, off_tree + NV_TREE_NODE_COUNT_UPPER, (uint32_t)ctx.n_upper);
    nv__write_u32(buf, off_tree + NV_TREE_TILE_COUNT_LOWER, lower_tiles);
    nv__write_u32(buf, off_tree + NV_TREE_TILE_COUNT_UPPER, upper_tiles);
    nv__write_u32(buf, off_tree + NV_TREE_TILE_COUNT_ROOT, root_active_tiles);
    nv__write_u64(buf, off_tree + NV_TREE_VOXEL_COUNT,
                  total_active + (uint64_t)lower_tiles * 512u + (uint64_t)upper_tiles * 128u * 128u * 128u +
                      (uint64_t)root_active_tiles * 4096u * 4096u * 4096u);

    /* GridData. */
    nv__write_u64(buf, off_grid + NV_GRID_OFF_MAGIC, NV_MAGIC_NUMBER);
    nv__write_u64(buf, off_grid + NV_GRID_OFF_CHECKSUM, 0xFFFFFFFFFFFFFFFFull);
    nv__write_u32(buf, off_grid + NV_GRID_OFF_VERSION, nv__make_version());
    /* HasBBox (1 << 1) and IsBreadthFirst (1 << 5). */
    nv__write_u32(buf, off_grid + NV_GRID_OFF_FLAGS, (1u << 1) | (1u << 5));
    nv__write_u32(buf, off_grid + NV_GRID_OFF_GRID_INDEX, 0u);
    nv__write_u32(buf, off_grid + NV_GRID_OFF_GRID_COUNT, 1u);
    nv__write_u64(buf, off_grid + NV_GRID_OFF_GRID_SIZE, (uint64_t)aligned_total);
    {
        const char *name = grid->descriptor.grid_name ? grid->descriptor.grid_name : "";
        size_t nl = strlen(name);
        if (nl > 255) nl = 255;
        memcpy(buf + off_grid + NV_GRID_OFF_GRID_NAME, name, nl);
    }
    {
        /* Map: row-major 3x3 linear part (world = mat * index + vec), its
           inverse, the translation and taper 1, in float and double. */
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
        const size_t map = off_grid + NV_GRID_OFF_MAP;
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                nv__write_f32(buf, map + 4 * (size_t)(3 * a + b), (float)m[a][b]);
                nv__write_f32(buf, map + 36 + 4 * (size_t)(3 * a + b), (float)inv[a][b]);
                nv__write_f64(buf, map + 88 + 8 * (size_t)(3 * a + b), m[a][b]);
                nv__write_f64(buf, map + 160 + 8 * (size_t)(3 * a + b), inv[a][b]);
            }
            nv__write_f32(buf, map + 72 + 4 * (size_t)a, (float)m[a][3]);
            nv__write_f64(buf, map + 232 + 8 * (size_t)a, m[a][3]);
            /* Voxel size: length of each index axis in world space. */
            nv__write_f64(buf, off_grid + NV_GRID_OFF_VOXEL_SIZE + 8 * (size_t)a,
                          sqrt(m[0][a] * m[0][a] + m[1][a] * m[1][a] + m[2][a] * m[2][a]));
        }
        nv__write_f32(buf, map + 84, 1.0f);
        nv__write_f64(buf, map + 256, 1.0);
        /* World box of the index box's voxel corners [min, max + 1]. */
        double wlo[3] = {0, 0, 0}, whi[3] = {0, 0, 0};
        if (root_box.any) {
            for (int a = 0; a < 3; ++a) { wlo[a] = INFINITY; whi[a] = -INFINITY; }
            for (int c = 0; c < 8; ++c) {
                const double q[3] = {(c & 1) ? root_box.hi[0] + 1.0 : (double)root_box.lo[0],
                                     (c & 2) ? root_box.hi[1] + 1.0 : (double)root_box.lo[1],
                                     (c & 4) ? root_box.hi[2] + 1.0 : (double)root_box.lo[2]};
                for (int a = 0; a < 3; ++a) {
                    const double w = m[a][3] + m[a][0] * q[0] + m[a][1] * q[1] + m[a][2] * q[2];
                    if (w < wlo[a]) wlo[a] = w;
                    if (w > whi[a]) whi[a] = w;
                }
            }
        }
        for (int a = 0; a < 3; ++a) {
            nv__write_f64(buf, off_grid + NV_GRID_OFF_WORLD_BBOX + 8 * (size_t)a, wlo[a]);
            nv__write_f64(buf, off_grid + NV_GRID_OFF_WORLD_BBOX + 24 + 8 * (size_t)a, whi[a]);
        }
    }
    {
        const tvdb_meta_entry_t *cls = tvdb_grid_find_metadata(grid, "class");
        uint32_t gc = PNANOVDB_GRID_CLASS_UNKNOWN;
        if (gt == PNANOVDB_GRID_TYPE_MASK) gc = PNANOVDB_GRID_CLASS_TOPOLOGY;
        else if (cls && cls->value.type == TVDB_VALUE_STRING && cls->value.u.s.str) {
            const char *c = cls->value.u.s.str;
            if (!strcmp(c, "level set")) gc = PNANOVDB_GRID_CLASS_LEVEL_SET;
            else if (!strcmp(c, "fog volume")) gc = PNANOVDB_GRID_CLASS_FOG_VOLUME;
            else if (!strcmp(c, "staggered")) gc = PNANOVDB_GRID_CLASS_STAGGERED;
        }
        nv__write_u32(buf, off_grid + NV_GRID_OFF_GRID_CLASS, gc);
    }
    nv__write_u32(buf, off_grid + NV_GRID_OFF_GRID_TYPE, gt);
    nv__write_i64(buf, off_grid + NV_GRID_OFF_BLIND_OFF, 0);
    nv__write_u32(buf, off_grid + NV_GRID_OFF_BLIND_COUNT, 0u);

    free(leaf_box); free(lower_box); free(upper_box); free(entries);
    free(ctx.uppers); free(ctx.lowers); free(ctx.leaves);
    *out_data = buf;
    *out_size = aligned_total;
    return TVDB_OK;
}

tvdb_status_t tvdb_grid_to_nanovdb_float(const tvdb_grid_t *grid, uint8_t **out_data,
                                         size_t *out_size, tvdb_error_t *err) {
    if (!grid || !out_data || !out_size)
        return nv__error(err, TVDB_ERROR_INVALID_ARGUMENT, "null arg");
    if (grid->tree.layout.num_levels != 4 ||
        grid->tree.layout.levels[3].value_type != TVDB_VALUE_FLOAT || grid->tree.is_mask_grid)
        return nv__error(err, TVDB_ERROR_UNSUPPORTED_VERSION,
                         "tvdb_grid_to_nanovdb_float: only Tree_float_5_4_3 supported");
    return tvdb_grid_to_nanovdb(grid, out_data, out_size, err);
}
