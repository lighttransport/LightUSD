#include "tinyvdb_sdf_tree.h"
#include "tinyvdb_checked.h"
#include "tinyvdb_tree_internal.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float *data;
    int32_t origin[3];
    uint32_t neighbor[6];
    float boundary[6], change;
    size_t offset;
    unsigned bad;
} sdf_leaf;
struct tvdb_sdf_tree {
    tvdb_grid_t *grid;
    tvdb_allocator_t allocator;
    tvdb_thread_pool_t *pool;
    int owns_pool, L, dim, levels;
    float h, background, origin[3];
    size_t nleaves, nactive, nwords, metadata_bytes, scratch_bytes;
    sdf_leaf *leaves;
    uint64_t *masks;
    uint16_t *ranks;
    float *values[2];
    uint8_t *flags;
    uint32_t *order[4], *groups[4], ngroups[4];
    uint32_t *leaf_map;
    size_t leaf_capacity;
    size_t *root_map, root_capacity;
    uint8_t *seen;
    int64_t span[TVDB_MAX_TREE_DEPTH];
};
static tvdb_status_t sdf_error(tvdb_error_t *err, tvdb_status_t st, const char *msg) {
    if (err) {
        memset(err, 0, sizeof(*err));
        err->status = st;
        snprintf(err->message, sizeof(err->message), "%s", msg);
    }
    return st;
}
static void *sdf_default_malloc(size_t n, void *user) {
    (void)user;
    return malloc(n);
}
static void sdf_default_free(void *p, size_t n, void *user) {
    (void)n;
    (void)user;
    free(p);
}
static void *sdf_alloc(tvdb_sdf_tree_t *p, size_t n, int scratch) {
    if (!n)
        return NULL;
    size_t *total = scratch ? &p->scratch_bytes : &p->metadata_bytes;
    if (n > SIZE_MAX - *total)
        return NULL;
    void *v = p->allocator.malloc_fn(n, p->allocator.user_ctx);
    if (v) {
        memset(v, 0, n);
        *total += n;
    }
    return v;
}
static void sdf_free(tvdb_sdf_tree_t *p, void *v, size_t n, int scratch) {
    if (!v)
        return;
    p->allocator.free_fn(v, n, p->allocator.user_ctx);
    if (scratch)
        p->scratch_bytes -= n;
    else
        p->metadata_bytes -= n;
}
static unsigned sdf_pop(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_popcountll(x);
#else
    x = x - ((x >> 1) & UINT64_C(0x5555555555555555));
    x = (x & UINT64_C(0x3333333333333333)) + ((x >> 2) & UINT64_C(0x3333333333333333));
    return (unsigned)((((x + (x >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f)) *
                       UINT64_C(0x0101010101010101)) >>
                      56);
#endif
}
static unsigned sdf_first(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(x);
#else
    unsigned bit = 0;
    while (!(x & 1)) {
        ++bit;
        x >>= 1;
    }
    return bit;
#endif
}
static unsigned sdf_last(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return 63u - (unsigned)__builtin_clzll(x);
#else
    unsigned bit = 0;
    while (x >> 1) {
        ++bit;
        x >>= 1;
    }
    return bit;
#endif
}
static uint64_t sdf_word(const uint8_t *bits, size_t bytes, size_t word) {
    size_t at = word * 8, take = bytes - at;
    if (take > 8)
        take = 8;
    uint64_t v = 0;
    for (size_t i = 0; i < take; ++i)
        v |= (uint64_t)bits[at + i] << (i * 8);
    return v;
}
static uint64_t sdf_hash(int32_t x, int32_t y, int32_t z) {
    uint64_t h = (uint32_t)x * UINT64_C(73856093) ^ (uint32_t)y * UINT64_C(19349663) ^
                 (uint32_t)z * UINT64_C(83492791);
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    return h ^ (h >> 33);
}
static int64_t sdf_floor(int64_t x, int64_t span) {
    return x >= 0 ? x / span : -1 - ((-1 - x) / span);
}
static int sdf_mask_valid(const tvdb_nodemask_t *m, int L) {
    size_t n = (size_t)1 << (3 * L), bytes = (n + 7) / 8;
    return m->log2dim == L && m->bitsize == (int32_t)n && m->bits.num_bits == n &&
           m->bits.num_bytes >= bytes && m->bits.data &&
           (!(n % 8) || !(m->bits.data[bytes - 1] & (uint8_t)~((1u << (n % 8)) - 1)));
}
static size_t sdf_mask_rank(const tvdb_nodemask_t *m, size_t bit) {
    size_t n = (m->bits.num_bits + 7) / 8, word = bit / 64, rank = 0;
    for (size_t i = 0; i < word; ++i)
        rank += sdf_pop(sdf_word(m->bits.data, n, i));
    unsigned b = (unsigned)(bit % 64);
    if (b)
        rank += sdf_pop(sdf_word(m->bits.data, n, word) & ((UINT64_C(1) << b) - 1));
    return rank;
}
static int32_t sdf_root_coord(const tvdb_root_node_t *r, size_t item, int axis) {
    return item < r->num_children ? r->child_origins[3 * item + axis]
                                  : r->tile_origins[3 * (item - r->num_children) + axis];
}
static tvdb_status_t sdf_root_index(tvdb_sdf_tree_t *p, tvdb_error_t *err) {
    const tvdb_root_node_t *r = &p->grid->tree.nodes[0].u.root;
    if ((r->num_children && (!r->child_origins || !r->child_indices)) ||
        (r->num_tiles && (!r->tile_origins || !r->tile_values || !r->tile_active)))
        return sdf_error(err, TVDB_ERROR_INVALID_DATA, "invalid SDF root arrays");
    size_t count = (size_t)r->num_children + r->num_tiles, bytes;
    if (r->num_children >= p->grid->tree.num_nodes || count < (size_t)r->num_children ||
        count > SIZE_MAX / (3 * sizeof(int32_t)) ||
        !tvdb_hash_capacity_scaled(count, 4, 3, &p->root_capacity) ||
        !tvdb_size_mul(p->root_capacity, sizeof(size_t), &bytes))
        return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF root index overflow");
    p->root_map = sdf_alloc(p, bytes, 0);
    if (!p->root_map)
        return sdf_error(err, TVDB_ERROR_OUT_OF_MEMORY, "SDF root index allocation failed");
    for (size_t i = 0; i < count; ++i) {
        int32_t c[3];
        for (int a = 0; a < 3; ++a) {
            c[a] = sdf_root_coord(r, i, a);
            if ((int64_t)c[a] % p->span[1])
                return sdf_error(err, TVDB_ERROR_INVALID_DATA, "unaligned SDF root entry");
        }
        size_t at = (size_t)sdf_hash(c[0], c[1], c[2]) & (p->root_capacity - 1);
        while (p->root_map[at]) {
            size_t j = p->root_map[at] - 1;
            if (c[0] == sdf_root_coord(r, j, 0) && c[1] == sdf_root_coord(r, j, 1) &&
                c[2] == sdf_root_coord(r, j, 2))
                return sdf_error(err, TVDB_ERROR_INVALID_DATA, "duplicate SDF root entry");
            at = (at + 1) & (p->root_capacity - 1);
        }
        p->root_map[at] = i + 1;
    }
    return TVDB_OK;
}
static tvdb_status_t sdf_walk(tvdb_sdf_tree_t *p, size_t index, int level, const int32_t origin[3],
                              size_t *next, tvdb_error_t *err) {
    tvdb_tree_t *t = &p->grid->tree;
    if (index >= t->num_nodes || level >= p->levels || p->seen[index])
        return sdf_error(err, TVDB_ERROR_INVALID_DATA, "invalid or shared SDF tree node");
    p->seen[index] = 1;
    tvdb_tree_node_t *node = &t->nodes[index];
    if (node->type != t->layout.levels[level].node_type)
        return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF node/layout mismatch");
    if (node->type == TVDB_NODE_ROOT) {
        const tvdb_root_node_t *r = &node->u.root;
        for (size_t i = r->num_children; i > 0; --i) {
            tvdb_status_t st = sdf_walk(p, r->child_indices[i - 1], level + 1,
                                        r->child_origins + 3 * (i - 1), next, err);
            if (st != TVDB_OK)
                return st;
        }
    } else if (node->type == TVDB_NODE_INTERNAL) {
        tvdb_internal_node_t *in = &node->u.internal;
        int L = t->layout.levels[level].log2dim;
        size_t slots = (size_t)1 << (3 * L), bytes = (slots + 7) / 8, words = (slots + 63) / 64,
               c = in->num_children;
        if (!sdf_mask_valid(&in->child_mask, L) || !sdf_mask_valid(&in->value_mask, L) ||
            !in->values || in->values_size < slots * sizeof(float) || (c && !in->child_indices))
            return sdf_error(err, TVDB_ERROR_INVALID_DATA, "invalid SDF internal node");
        size_t mask_children = 0;
        for (size_t w = 0; w < words; ++w)
            mask_children += sdf_pop(sdf_word(in->child_mask.bits.data, bytes, w));
        if (mask_children != c)
            return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF child mask/count mismatch");
        for (size_t w = words; w > 0; --w) {
            uint64_t bits = sdf_word(in->child_mask.bits.data, bytes, w - 1);
            while (bits) {
                unsigned bit = sdf_last(bits);
                bits &= ~(UINT64_C(1) << bit);
                size_t slot = 64 * (w - 1) + bit;
                if (!c || slot >= slots)
                    return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF child mask/count mismatch");
                int32_t child[3];
                for (int a = 0; a < 3; ++a) {
                    int digit = (int)((slot >> ((2 - a) * L)) & ((1u << L) - 1));
                    int64_t coord = (int64_t)origin[a] + digit * p->span[level + 1];
                    if (coord < INT32_MIN || coord > INT32_MAX)
                        return sdf_error(err, TVDB_ERROR_INVALID_DATA,
                                         "SDF child coordinate overflow");
                    child[a] = (int32_t)coord;
                }
                tvdb_status_t st = sdf_walk(p, in->child_indices[--c], level + 1, child, next, err);
                if (st != TVDB_OK)
                    return st;
            }
        }
        if (c)
            return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF child mask/count mismatch");
    } else {
        tvdb_leaf_node_t *leaf = &node->u.leaf;
        size_t slots = (size_t)1 << (3 * p->L), bytes = (slots + 7) / 8;
        if (*next >= p->nleaves || !sdf_mask_valid(&leaf->value_mask, p->L) || !leaf->data ||
            leaf->num_voxels != slots || leaf->data_size < slots * sizeof(float))
            return sdf_error(err, TVDB_ERROR_INVALID_DATA, "invalid SDF leaf");
        size_t li = (*next)++;
        sdf_leaf *e = p->leaves + li;
        e->data = (float *)leaf->data;
        e->offset = p->nactive;
        for (int a = 0; a < 3; ++a) {
            if ((int64_t)origin[a] % p->dim || (int64_t)origin[a] + p->dim - 1 > INT32_MAX)
                return sdf_error(err, TVDB_ERROR_INVALID_DATA, "unaligned SDF leaf");
            e->origin[a] = origin[a];
        }
        unsigned rank = 0;
        for (size_t w = 0; w < p->nwords; ++w) {
            uint64_t bits = sdf_word(leaf->value_mask.bits.data, bytes, w);
            p->masks[li * p->nwords + w] = bits;
            p->ranks[li * p->nwords + w] = (uint16_t)rank;
            rank += sdf_pop(bits);
        }
        if (rank > SIZE_MAX - p->nactive)
            return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF active count overflow");
        p->nactive += rank;
    }
    return TVDB_OK;
}
static uint32_t sdf_find_leaf(const tvdb_sdf_tree_t *p, const int32_t c[3]) {
    size_t at = (size_t)sdf_hash(c[0], c[1], c[2]) & (p->leaf_capacity - 1);
    while (p->leaf_map[at]) {
        uint32_t i = p->leaf_map[at] - 1;
        const sdf_leaf *leaf = p->leaves + i;
        if (c[0] == leaf->origin[0] && c[1] == leaf->origin[1] && c[2] == leaf->origin[2])
            return i;
        at = (at + 1) & (p->leaf_capacity - 1);
    }
    return UINT32_MAX;
}
static float sdf_probe_tile(const tvdb_sdf_tree_t *p, const int32_t c[3]) {
    const tvdb_tree_t *t = &p->grid->tree;
    const tvdb_root_node_t *r = &t->nodes[0].u.root;
    int32_t origin[3];
    for (int a = 0; a < 3; ++a)
        origin[a] = (int32_t)(sdf_floor(c[a], p->span[1]) * p->span[1]);
    size_t at = (size_t)sdf_hash(origin[0], origin[1], origin[2]) & (p->root_capacity - 1),
           item = SIZE_MAX;
    while (p->root_map[at]) {
        size_t i = p->root_map[at] - 1;
        if (origin[0] == sdf_root_coord(r, i, 0) && origin[1] == sdf_root_coord(r, i, 1) &&
            origin[2] == sdf_root_coord(r, i, 2)) {
            item = i;
            break;
        }
        at = (at + 1) & (p->root_capacity - 1);
    }
    if (item == SIZE_MAX)
        return p->background;
    if (item >= r->num_children)
        return r->tile_values[item - r->num_children].type == TVDB_VALUE_FLOAT
                   ? r->tile_values[item - r->num_children].u.f
                   : NAN;
    size_t node = r->child_indices[item];
    for (int level = 1; level < p->levels - 1; ++level) {
        const tvdb_internal_node_t *in = &t->nodes[node].u.internal;
        int L = t->layout.levels[level].log2dim;
        size_t slot = 0;
        for (int a = 0; a < 3; ++a) {
            size_t digit = (size_t)(((int64_t)c[a] - origin[a]) / p->span[level + 1]);
            slot = (slot << L) | digit;
            origin[a] = (int32_t)((int64_t)origin[a] + (int64_t)digit * p->span[level + 1]);
        }
        if (!tvdb_nodemask_is_on(&in->child_mask, (int32_t)slot)) {
            float v;
            memcpy(&v, in->values + slot * sizeof(float), sizeof(v));
            return v;
        }
        node = in->child_indices[sdf_mask_rank(&in->child_mask, slot)];
    }
    return NAN; /* A missing leaf cannot terminate at an existing leaf. */
}
static tvdb_status_t sdf_geometry(tvdb_sdf_tree_t *p, tvdb_error_t *err) {
    const tvdb_transform_t *t = &p->grid->transform;
    double h = 1;
    if (t->type == TVDB_TRANSFORM_UNIFORM_SCALE ||
        t->type == TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE)
        h = t->voxel_size[0];
    else if (t->type == TVDB_TRANSFORM_SCALE || t->type == TVDB_TRANSFORM_SCALE_TRANSLATE) {
        h = t->voxel_size[0];
        if (h != t->voxel_size[1] || h != t->voxel_size[2])
            return sdf_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
                             "SDF tree requires isotropic spacing");
    } else if (t->type == TVDB_TRANSFORM_AFFINE) {
        double lengths[3] = {0};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                lengths[a] += t->matrix[b][a] * t->matrix[b][a];
        h = sqrt(lengths[0]);
        for (int a = 0; a < 3; ++a) {
            if (!isfinite(lengths[a]) || fabs(lengths[a] - lengths[0]) > 1e-12 * lengths[0])
                return sdf_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
                                 "SDF tree requires an isotropic affine transform");
            for (int b = a + 1; b < 3; ++b) {
                double dot = 0;
                for (int k = 0; k < 3; ++k)
                    dot += t->matrix[k][a] * t->matrix[k][b];
                if (fabs(dot) > 1e-12 * lengths[0])
                    return sdf_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
                                     "SDF tree does not support shear");
            }
        }
    } else if (t->type != TVDB_TRANSFORM_TRANSLATION)
        return sdf_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM, "unknown SDF transform");
    if (!isfinite(h) || h <= 0 || h > FLT_MAX || (float)h <= 0)
        return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid SDF voxel size");
    p->h = (float)h;
    for (int a = 0; a < 3; ++a) {
        double v = t->type == TVDB_TRANSFORM_AFFINE
                       ? t->matrix[a][3]
                       : (t->type == TVDB_TRANSFORM_UNIFORM_SCALE || t->type == TVDB_TRANSFORM_SCALE
                              ? 0
                              : t->translation[a]);
        if (!isfinite(v) || fabs(v) > FLT_MAX)
            return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid SDF origin");
        p->origin[a] = (float)v;
    }
    return TVDB_OK;
}
void tvdb_sdf_tree_destroy(tvdb_sdf_tree_t *p) {
    if (!p)
        return;
    if (p->owns_pool)
        tvdb_thread_pool_destroy(p->pool);
    sdf_free(p, p->seen, p->grid->tree.num_nodes, 0);
    sdf_free(p, p->root_map, p->root_capacity * sizeof(size_t), 0);
    sdf_free(p, p->leaf_map, p->leaf_capacity * sizeof(uint32_t), 0);
    for (int i = 0; i < 4; ++i) {
        sdf_free(p, p->order[i], p->nleaves * sizeof(uint32_t), 0);
        sdf_free(p, p->groups[i], (p->nleaves + 1) * sizeof(uint32_t), 0);
    }
    sdf_free(p, p->flags, (p->nactive + 3) / 4, 1);
    for (int i = 0; i < 2; ++i)
        sdf_free(p, p->values[i], p->nactive * sizeof(float), 1);
    sdf_free(p, p->ranks, p->nleaves * p->nwords * sizeof(uint16_t), 0);
    sdf_free(p, p->masks, p->nleaves * p->nwords * sizeof(uint64_t), 0);
    sdf_free(p, p->leaves, p->nleaves * sizeof(sdf_leaf), 0);
    tvdb_allocator_t a = p->allocator;
    a.free_fn(p, sizeof(*p), a.user_ctx);
}
void tvdb_sdf_tree_info(const tvdb_sdf_tree_t *p, tvdb_sdf_tree_info_t *out) {
    if (p && out) {
        out->leaves = p->nleaves;
        out->active_voxels = p->nactive;
        out->metadata_bytes = p->metadata_bytes;
        out->scratch_bytes = p->scratch_bytes;
        out->workers = tvdb_thread_pool_size(p->pool);
    }
}
tvdb_status_t tvdb_sdf_tree_create(tvdb_grid_t *grid, tvdb_thread_pool_t *pool,
                                   const tvdb_allocator_t *allocator, tvdb_sdf_tree_t **out,
                                   tvdb_error_t *err) {
    if (!out)
        return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "null SDF workspace output");
    *out = NULL;
    if (!grid || !grid->tree.nodes || !grid->tree.num_nodes || grid->tree.num_nodes > UINT32_MAX ||
        grid->tree.layout.num_levels < 2 || grid->tree.layout.num_levels > TVDB_MAX_TREE_DEPTH)
        return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid SDF tree");
    tvdb_allocator_t a = {sdf_default_malloc, NULL, sdf_default_free, NULL};
    if (allocator)
        a = *allocator;
    if (!a.malloc_fn || !a.free_fn)
        return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid SDF workspace allocator");
    tvdb_sdf_tree_t *p = a.malloc_fn(sizeof(*p), a.user_ctx);
    if (!p)
        return sdf_error(err, TVDB_ERROR_OUT_OF_MEMORY, "SDF workspace allocation failed");
    memset(p, 0, sizeof(*p));
    p->grid = grid;
    p->allocator = a;
    p->metadata_bytes = sizeof(*p);
    p->levels = grid->tree.layout.num_levels;
    tvdb_status_t st = sdf_geometry(p, err);
    if (st != TVDB_OK)
        goto fail;
    p->L = grid->tree.layout.levels[p->levels - 1].log2dim;
    if (p->L < 0 || p->L > 5) {
        st = sdf_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE, "SDF leaf dimension must be 1..32");
        goto fail;
    }
    int sum = 0;
    for (int level = p->levels - 1; level >= 0; --level) {
        const tvdb_node_info_t *info = &grid->tree.layout.levels[level];
        tvdb_node_type_t wanted = level == 0               ? TVDB_NODE_ROOT
                                  : level == p->levels - 1 ? TVDB_NODE_LEAF
                                                           : TVDB_NODE_INTERNAL;
        if (info->node_type != wanted || info->value_type != TVDB_VALUE_FLOAT ||
            info->log2dim < 0 || info->log2dim > 5) {
            st =
                sdf_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE, "unsupported SDF float hierarchy");
            goto fail;
        }
        if (level > 0)
            sum += info->log2dim;
        if (sum > 31) {
            st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF hierarchy coordinate overflow");
            goto fail;
        }
        p->span[level] = INT64_C(1) << sum;
    }
    if (grid->tree.nodes[0].type != TVDB_NODE_ROOT ||
        grid->tree.nodes[0].u.root.background.type != TVDB_VALUE_FLOAT ||
        !isfinite(grid->tree.nodes[0].u.root.background.u.f)) {
        st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "invalid SDF root background");
        goto fail;
    }
    p->background = grid->tree.nodes[0].u.root.background.u.f;
    p->dim = 1 << p->L;
    p->nwords = (((size_t)1 << (3 * p->L)) + 63) / 64;
    for (size_t i = 0; i < grid->tree.num_nodes; ++i)
        if (grid->tree.nodes[i].type == TVDB_NODE_LEAF)
            ++p->nleaves;
    if (p->nleaves >= UINT32_MAX) {
        st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF leaf count overflow");
        goto fail;
    }
    size_t bytes, words;
    if (!tvdb_size_mul(p->nleaves, p->nwords, &words) ||
        !tvdb_size_mul(words, sizeof(uint64_t), &bytes)) {
        st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF mask allocation overflow");
        goto fail;
    }
    if (words) {
        p->masks = sdf_alloc(p, bytes, 0);
        if (!p->masks)
            goto oom;
        p->ranks = sdf_alloc(p, words * sizeof(uint16_t), 0);
        if (!p->ranks)
            goto oom;
    }
    if (!tvdb_size_mul(p->nleaves, sizeof(sdf_leaf), &bytes)) {
        st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF leaf allocation overflow");
        goto fail;
    }
    if (bytes) {
        p->leaves = sdf_alloc(p, bytes, 0);
        if (!p->leaves)
            goto oom;
    }
    p->seen = sdf_alloc(p, grid->tree.num_nodes, 0);
    if (!p->seen)
        goto oom;
    st = sdf_root_index(p, err);
    if (st != TVDB_OK)
        goto fail;
    size_t next = 0;
    int32_t zero[3] = {0};
    st = sdf_walk(p, 0, 0, zero, &next, err);
    if (st != TVDB_OK)
        goto fail;
    if (next != p->nleaves) {
        st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "unreachable SDF leaves");
        goto fail;
    }
    for (size_t i = 0; i < grid->tree.num_nodes; ++i)
        if (!p->seen[i]) {
            st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "unreachable SDF node");
            goto fail;
        }
    sdf_free(p, p->seen, grid->tree.num_nodes, 0);
    p->seen = NULL;
    if (!tvdb_hash_capacity_scaled(p->nleaves, 4, 3, &p->leaf_capacity) ||
        !tvdb_size_mul(p->leaf_capacity, sizeof(uint32_t), &bytes)) {
        st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF leaf index overflow");
        goto fail;
    }
    p->leaf_map = sdf_alloc(p, bytes, 0);
    if (!p->leaf_map)
        goto oom;
    for (size_t i = 0; i < p->nleaves; ++i) {
        sdf_leaf *leaf = p->leaves + i;
        size_t at = (size_t)sdf_hash(leaf->origin[0], leaf->origin[1], leaf->origin[2]) &
                    (p->leaf_capacity - 1);
        while (p->leaf_map[at]) {
            uint32_t j = p->leaf_map[at] - 1;
            if (!memcmp(leaf->origin, p->leaves[j].origin, sizeof(leaf->origin))) {
                st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "duplicate SDF leaf origin");
                goto fail;
            }
            at = (at + 1) & (p->leaf_capacity - 1);
        }
        p->leaf_map[at] = (uint32_t)i + 1;
    }
    for (size_t i = 0; i < p->nleaves; ++i)
        for (int n = 0; n < 6; ++n) {
            sdf_leaf *leaf = p->leaves + i;
            int32_t c[3];
            memcpy(c, leaf->origin, sizeof(c));
            int axis = n / 2;
            int64_t v = (int64_t)c[axis] + ((n & 1) ? p->dim : -p->dim);
            leaf->neighbor[n] = UINT32_MAX;
            leaf->boundary[n] = p->background;
            if (v >= INT32_MIN && v <= INT32_MAX) {
                c[axis] = (int32_t)v;
                leaf->neighbor[n] = sdf_find_leaf(p, c);
                if (leaf->neighbor[n] == UINT32_MAX)
                    leaf->boundary[n] = sdf_probe_tile(p, c);
            }
            if (!isfinite(leaf->boundary[n])) {
                st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "nonfinite SDF boundary tile");
                goto fail;
            }
        }
    sdf_free(p, p->root_map, p->root_capacity * sizeof(size_t), 0);
    p->root_map = NULL;
    p->root_capacity = 0;
    /* Neighbor pointers replace the temporary spatial hash during processing. */
    sdf_free(p, p->leaf_map, p->leaf_capacity * sizeof(uint32_t), 0);
    p->leaf_map = NULL;
    p->leaf_capacity = 0;
    if (!tvdb_size_mul(p->nactive, sizeof(float), &bytes)) {
        st = sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF value allocation overflow");
        goto fail;
    }
    if (bytes) {
        p->values[0] = sdf_alloc(p, bytes, 1);
        if (!p->values[0])
            goto oom;
    }
    p->pool = pool;
    if (!pool) {
        size_t threads = tvdb_thread_default_count();
        /* Sparse wavefronts have short planes; oversized teams spend more time
         * waking and waiting than computing. Larger teams remain opt-in via
         * a borrowed pool, useful for large filter workloads. */
        if (threads > 8)
            threads = 8;
        if (threads > p->nleaves)
            threads = p->nleaves;
        if (!threads)
            threads = 1;
        st = tvdb_thread_pool_create(threads, &p->pool, err);
        if (st != TVDB_OK)
            goto fail;
        p->owns_pool = 1;
    }
    *out = p;
    return TVDB_OK;
oom:
    st = sdf_error(err, TVDB_ERROR_OUT_OF_MEMORY, "SDF workspace allocation failed");
fail:
    tvdb_sdf_tree_destroy(p);
    return st;
}

/* Mask ranks map the tree's local slots to compact active-value storage. */
static int sdf_active(const tvdb_sdf_tree_t *p, size_t leaf, size_t slot) {
    return (int)((p->masks[leaf * p->nwords + slot / 64] >> (slot % 64)) & 1);
}
static size_t sdf_index(const tvdb_sdf_tree_t *p, size_t leaf, size_t slot) {
    size_t w = leaf * p->nwords + slot / 64;
    unsigned b = (unsigned)(slot % 64);
    uint64_t before = b ? p->masks[w] & ((UINT64_C(1) << b) - 1) : 0;
    return p->leaves[leaf].offset + p->ranks[w] + sdf_pop(before);
}
static float sdf_neighbor(const tvdb_sdf_tree_t *p, const float *values, size_t li, size_t slot,
                          int direction, int domain) {
    int axis = direction / 2, shift = (2 - axis) * p->L;
    size_t step = (size_t)1 << shift;
    int digit = (int)((slot >> shift) & (p->dim - 1));
    if ((direction & 1) ? digit + 1 < p->dim : digit > 0)
        slot = (direction & 1) ? slot + step : slot - step;
    else {
        uint32_t next = p->leaves[li].neighbor[direction];
        if (next == UINT32_MAX)
            return domain ? INFINITY : p->leaves[li].boundary[direction];
        li = next;
        slot = (direction & 1) ? slot - (p->dim - 1) * step : slot + (p->dim - 1) * step;
    }
    if (sdf_active(p, li, slot))
        return values[sdf_index(p, li, slot)];
    return domain ? INFINITY : p->leaves[li].data[slot];
}
static void sdf_pack(size_t begin, size_t end, void *user) {
    tvdb_sdf_tree_t *p = user;
    for (size_t li = begin; li < end; ++li) {
        sdf_leaf *e = p->leaves + li;
        size_t at = e->offset;
        e->bad = 0;
        for (size_t w = 0; w < p->nwords; ++w) {
            uint64_t bits = p->masks[li * p->nwords + w];
            while (bits) {
                unsigned b = sdf_first(bits);
                bits &= bits - 1;
                float v = e->data[w * 64 + b];
                if (!isfinite(v))
                    e->bad = 1;
                p->values[0][at++] = v;
            }
        }
    }
}
static int sdf_bad(const tvdb_sdf_tree_t *p) {
    for (size_t i = 0; i < p->nleaves; ++i)
        if (p->leaves[i].bad)
            return 1;
    return 0;
}
static tvdb_status_t sdf_prepare(tvdb_sdf_tree_t *p, tvdb_error_t *err) {
    tvdb_status_t st = tvdb_thread_pool_for(p->pool, 0, p->nleaves, 1, sdf_pack, p, err);
    if (st != TVDB_OK)
        return st;
    return sdf_bad(p) ? sdf_error(err, TVDB_ERROR_INVALID_DATA, "nonfinite active SDF value")
                      : TVDB_OK;
}
typedef struct {
    tvdb_sdf_tree_t *p;
    const float *input;
    float *output;
    tvdb_sdf_filter_t kind;
    int axis;
} sdf_filter_job;
static void sdf_filter_range(size_t begin, size_t end, void *user) {
    sdf_filter_job *j = user;
    tvdb_sdf_tree_t *p = j->p;
    for (size_t li = begin; li < end; ++li) {
        sdf_leaf *e = p->leaves + li;
        size_t at = e->offset;
        e->bad = 0;
        for (size_t w = 0; w < p->nwords; ++w) {
            uint64_t bits = p->masks[li * p->nwords + w];
            while (bits) {
                unsigned b = sdf_first(bits);
                bits &= bits - 1;
                size_t slot = w * 64 + b;
                double v = j->input[at];
                if (j->axis >= 0) {
                    float a = sdf_neighbor(p, j->input, li, slot, j->axis * 2, 0),
                          c = sdf_neighbor(p, j->input, li, slot, j->axis * 2 + 1, 0);
                    if (!isfinite(a) || !isfinite(c))
                        e->bad = 1;
                    v = j->kind == TVDB_SDF_FILTER_MEAN ? ((double)a + v + c) / 3
                                                        : ((double)a + 2 * v + c) / 4;
                } else {
                    double sum = 0;
                    for (int n = 0; n < 6; ++n) {
                        float a = sdf_neighbor(p, j->input, li, slot, n, 0);
                        if (!isfinite(a))
                            e->bad = 1;
                        if (j->kind == TVDB_SDF_FILTER_DILATE) {
                            if (a < v)
                                v = a;
                        } else if (j->kind == TVDB_SDF_FILTER_ERODE) {
                            if (a > v)
                                v = a;
                        } else
                            sum += a;
                    }
                    if (j->kind == TVDB_SDF_FILTER_LAPLACIAN)
                        v = sum / 6;
                }
                if (!isfinite(v) || fabs(v) > FLT_MAX) {
                    e->bad = 1;
                    j->output[at++] = 0;
                } else
                    j->output[at++] = (float)v;
            }
        }
    }
}
typedef struct {
    tvdb_sdf_tree_t *p;
    const float *values;
    int swept;
} sdf_commit_job;
static unsigned sdf_flag(const tvdb_sdf_tree_t *p, size_t at) {
    return (p->flags[at / 4] >> ((at % 4) * 2)) & 3;
}
static void sdf_commit(size_t begin, size_t end, void *user) {
    sdf_commit_job *j = user;
    tvdb_sdf_tree_t *p = j->p;
    for (size_t li = begin; li < end; ++li) {
        sdf_leaf *e = p->leaves + li;
        size_t at = e->offset;
        for (size_t w = 0; w < p->nwords; ++w) {
            uint64_t bits = p->masks[li * p->nwords + w];
            while (bits) {
                unsigned b = sdf_first(bits);
                bits &= bits - 1;
                float v = j->values[at];
                if (!j->swept)
                    e->data[w * 64 + b] = v;
                else {
                    unsigned f = sdf_flag(p, at);
                    if (!(f & 1) && isfinite(v))
                        e->data[w * 64 + b] = (f & 2) ? v : -v;
                }
                ++at;
            }
        }
    }
}
static tvdb_status_t sdf_filter_values(tvdb_sdf_tree_t *p, tvdb_sdf_filter_t kind, int iterations,
                                   float **result, tvdb_error_t *err) {
    if (!p || kind < TVDB_SDF_FILTER_MEAN || kind > TVDB_SDF_FILTER_ERODE || iterations < 0)
        return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid SDF filter arguments");
    tvdb_status_t st = sdf_prepare(p, err);
    *result = p->values[0];
    if (st != TVDB_OK || !iterations || !p->nactive)
        return st;
    if (!p->values[1]) {
        p->values[1] = sdf_alloc(p, p->nactive * sizeof(float), 1);
        if (!p->values[1])
            return sdf_error(err, TVDB_ERROR_OUT_OF_MEMORY, "SDF filter buffer allocation failed");
    }
    float *input = p->values[0], *output = p->values[1];
    int passes = kind == TVDB_SDF_FILTER_MEAN || kind == TVDB_SDF_FILTER_GAUSSIAN ? 3 : 1;
    for (int i = 0; i < iterations; ++i)
        for (int a = 0; a < passes; ++a) {
            sdf_filter_job job = {p, input, output, kind, passes == 3 ? a : -1};
            st = tvdb_thread_pool_for(p->pool, 0, p->nleaves, 1, sdf_filter_range, &job, err);
            if (st != TVDB_OK)
                return st;
            if (sdf_bad(p))
                return sdf_error(err, TVDB_ERROR_INVALID_DATA,
                                 "nonfinite SDF filter boundary or result");
            float *tmp = input;
            input = output;
            output = tmp;
        }
    *result=input;return TVDB_OK;
}
tvdb_status_t tvdb_sdf_tree_filter(tvdb_sdf_tree_t *p,tvdb_sdf_filter_t kind,int iterations,
                                 tvdb_error_t *err) {
    float *result=NULL;
    tvdb_status_t st=sdf_filter_values(p,kind,iterations,&result,err);
    if(st!=TVDB_OK || !iterations || !p->nactive)return st;
    sdf_commit_job job={p,result,0};
    return tvdb_thread_pool_for(p->pool,0,p->nleaves,1,sdf_commit,&job,err);
}
tvdb_status_t tvdb_sdf_tree_filter_to_sparse(tvdb_sdf_tree_t *p,tvdb_sdf_filter_t kind,
    int iterations,tvdb_sparse_grid *out,tvdb_error_t *err) {
    if(!p || !out)return sdf_error(err,TVDB_ERROR_INVALID_ARGUMENT,"invalid filter output");
    tvdb_sparse_grid d={0};float origin[3];
    if(!tvdb_tree_dense_geometry(&p->grid->transform,&d.voxel_size,origin))
        return sdf_error(err,TVDB_ERROR_UNSUPPORTED_TRANSFORM,"flat output cannot represent this transform");
    for(size_t i=0;i<p->grid->tree.num_nodes;++i) {
        const tvdb_tree_node_t *n=p->grid->tree.nodes+i;
        if(n->type==TVDB_NODE_ROOT) {
            for(size_t t=0;t<n->u.root.num_tiles;++t)if(n->u.root.tile_active[t])
                return sdf_error(err,TVDB_ERROR_UNIMPLEMENTED,"flat output requires active-tile expansion");
        } else if(n->type==TVDB_NODE_INTERNAL && tvdb_nodemask_count_on(&n->u.internal.value_mask))
            return sdf_error(err,TVDB_ERROR_UNIMPLEMENTED,"flat output requires active-tile expansion");
    }
    float *result=NULL;
    tvdb_status_t st=sdf_filter_values(p,kind,iterations,&result,err);
    if(st!=TVDB_OK)return st;
    d.ox=origin[0];d.oy=origin[1];d.oz=origin[2];
    if(!tvdb_sparse_grid_reserve(&d,p->nactive))
        return sdf_error(err,TVDB_ERROR_OUT_OF_MEMORY,"filter output allocation failed");
    for(size_t li=0;li<p->nleaves;++li)for(size_t w=0;w<p->nwords;++w) {
        uint64_t bits=p->masks[li*p->nwords+w];
        while(bits) {
            unsigned b=sdf_first(bits);bits&=bits-1;
            size_t slot=w*64+b;
            d.coords[d.count].x=(int32_t)((int64_t)p->leaves[li].origin[0]+((slot>>(2*p->L))&(p->dim-1)));
            d.coords[d.count].y=(int32_t)((int64_t)p->leaves[li].origin[1]+((slot>>p->L)&(p->dim-1)));
            d.coords[d.count].z=(int32_t)((int64_t)p->leaves[li].origin[2]+(slot&(p->dim-1)));
            d.values[d.count++]=result[sdf_index(p,li,slot)];
        }
    }
    tvdb_sparse_grid_free(out);*out=d;
    return TVDB_OK;
}
typedef struct {
    tvdb_sdf_tree_t *p;
    float distance;
} sdf_offset_job;
static void sdf_offset_range(size_t begin, size_t end, void *user) {
    sdf_offset_job *j = user;
    tvdb_sdf_tree_t *p = j->p;
    for (size_t li = begin; li < end; ++li) {
        sdf_leaf *e = p->leaves + li;
        size_t count =
            li + 1 < p->nleaves ? p->leaves[li + 1].offset - e->offset : p->nactive - e->offset;
        e->bad = 0;
        for (size_t i = e->offset; i < e->offset + count; ++i) {
            double v = (double)p->values[0][i] + j->distance;
            if (!isfinite(v) || fabs(v) > FLT_MAX)
                e->bad = 1;
            else
                p->values[0][i] = (float)v;
        }
    }
}
tvdb_status_t tvdb_sdf_tree_offset(tvdb_sdf_tree_t *p, float distance, tvdb_error_t *err) {
    if (!p || !isfinite(distance))
        return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid SDF offset");
    tvdb_status_t st = sdf_prepare(p, err);
    if (st != TVDB_OK)
        return st;
    sdf_offset_job job = {p, distance};
    st = tvdb_thread_pool_for(p->pool, 0, p->nleaves, 1, sdf_offset_range, &job, err);
    if (st != TVDB_OK)
        return st;
    if (sdf_bad(p))
        return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF offset overflow");
    sdf_commit_job commit = {p, p->values[0], 0};
    return tvdb_thread_pool_for(p->pool, 0, p->nleaves, 1, sdf_commit, &commit, err);
}

/* Only occupied leaf planes are stored: distant sparse islands cost O(leaves).
 * Face-adjacent leaves differ in plane key, so each plane can run concurrently. */
typedef struct {
    int64_t key;
    uint32_t leaf;
} sdf_plane;
static int sdf_plane_compare(const void *a, const void *b) {
    const sdf_plane *x = a, *y = b;
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    return x->leaf < y->leaf ? -1 : x->leaf > y->leaf;
}
static tvdb_status_t sdf_planes(tvdb_sdf_tree_t *p, tvdb_error_t *err) {
    size_t bytes;
    if (!tvdb_size_mul(p->nleaves, sizeof(sdf_plane), &bytes))
        return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF plane allocation overflow");
    sdf_plane *planes = sdf_alloc(p, bytes, 0);
    if (bytes && !planes)
        return sdf_error(err, TVDB_ERROR_OUT_OF_MEMORY, "SDF plane allocation failed");
    tvdb_status_t st = TVDB_OK;
    for (int d = 0; d < 4; ++d) {
        if (p->ngroups[d])
            continue;
        if (!p->order[d])
            p->order[d] = sdf_alloc(p, p->nleaves * sizeof(uint32_t), 0);
        if (!p->groups[d])
            p->groups[d] = sdf_alloc(p, (p->nleaves + 1) * sizeof(uint32_t), 0);
        if (!p->order[d] || !p->groups[d]) {
            st = sdf_error(err, TVDB_ERROR_OUT_OF_MEMORY, "SDF plane order allocation failed");
            break;
        }
        for (size_t i = 0; i < p->nleaves; ++i) {
            const int32_t *c = p->leaves[i].origin;
            planes[i].leaf = (uint32_t)i;
            planes[i].key = (int64_t)c[0] / p->dim + ((d & 1) ? -1 : 1) * (int64_t)c[1] / p->dim +
                            ((d & 2) ? -1 : 1) * (int64_t)c[2] / p->dim;
        }
        qsort(planes, p->nleaves, sizeof(*planes), sdf_plane_compare);
        uint32_t groups = 0;
        for (size_t i = 0; i < p->nleaves; ++i) {
            p->order[d][i] = planes[i].leaf;
            if (!i || planes[i].key != planes[i - 1].key)
                p->groups[d][groups++] = (uint32_t)i;
        }
        p->groups[d][groups] = (uint32_t)p->nleaves;
        p->ngroups[d] = groups;
    }
    sdf_free(p, planes, bytes, 0);
    return st;
}
static double sdf_eikonal(double a, double b, double c, double h) {
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        double t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    if (!isfinite(a))
        return INFINITY;
    double x = a + h;
    if (x > b) {
        double ab = a - b, disc = 2 * h * h - ab * ab;
        if (disc < 0)
            disc = 0;
        x = (a + b + sqrt(disc)) / 2;
        if (x > c) {
            double ac = a - c, bc = b - c;
            disc = 3 * h * h - ab * ab - ac * ac - bc * bc;
            if (disc < 0)
                disc = 0;
            x = (a + b + c + sqrt(disc)) / 3;
        }
    }
    return x;
}
typedef struct {
    tvdb_sdf_tree_t *p;
    uint32_t *order;
    int sx, sy, sz;
} sdf_sweep_job;
static void sdf_sweep_range(size_t begin, size_t end, void *user) {
    sdf_sweep_job *j = user;
    tvdb_sdf_tree_t *p = j->p;
    for (size_t pos = begin; pos < end; ++pos) {
        size_t li = j->order[pos];
        sdf_leaf *e = p->leaves + li;
        int x0 = j->sx > 0 ? 0 : p->dim - 1, y0 = j->sy > 0 ? 0 : p->dim - 1;
        for (int x = x0; x >= 0 && x < p->dim; x += j->sx)
            for (int y = y0; y >= 0 && y < p->dim; y += j->sy) {
                size_t base = ((size_t)x << (2 * p->L)) | ((size_t)y << p->L);
                uint64_t bits = (p->masks[li * p->nwords + base / 64] >> (base % 64)) &
                                ((UINT64_C(1) << p->dim) - 1);
                while (bits) {
                    unsigned z = j->sz > 0 ? sdf_first(bits) : sdf_last(bits);
                    bits &= ~(UINT64_C(1) << z);
                    size_t slot = base + z, at = sdf_index(p, li, slot);
                    if (sdf_flag(p, at) & 1)
                        continue;
                    double a = fminf(sdf_neighbor(p, p->values[0], li, slot, 0, 1),
                                     sdf_neighbor(p, p->values[0], li, slot, 1, 1));
                    double b = fminf(sdf_neighbor(p, p->values[0], li, slot, 2, 1),
                                     sdf_neighbor(p, p->values[0], li, slot, 3, 1));
                    double c = fminf(sdf_neighbor(p, p->values[0], li, slot, 4, 1),
                                     sdf_neighbor(p, p->values[0], li, slot, 5, 1));
                    double v = sdf_eikonal(a, b, c, p->h);
                    float old = p->values[0][at];
                    if (isfinite(v) && v > FLT_MAX) {
                        e->bad = 1;
                        continue;
                    }
                    if (v < old) {
                        float next = (float)v, delta = old - next;
                        if (delta > e->change)
                            e->change = delta;
                        p->values[0][at] = next;
                    }
                }
            }
    }
}
tvdb_status_t tvdb_sdf_tree_fast_sweep(tvdb_sdf_tree_t *p, float band, int max_iters, float tol,
                                       tvdb_sdf_sweep_result_t *result, tvdb_error_t *err) {
    if (!p || !isfinite(band) || band < 0 || max_iters < 0 || !isfinite(tol) || tol < 0)
        return sdf_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid SDF fast-sweeping arguments");
    tvdb_status_t st = sdf_prepare(p, err);
    if (st != TVDB_OK)
        return st;
    tvdb_sdf_sweep_result_t r = {0};
    r.converged = !p->nactive;
    if (!p->flags && p->nactive) {
        p->flags = sdf_alloc(p, (p->nactive + 3) / 4, 1);
        if (!p->flags)
            return sdf_error(err, TVDB_ERROR_OUT_OF_MEMORY, "SDF flag allocation failed");
    }
    /* Assemble complete bytes serially; packed flags can straddle leaf ranges. */
    for (size_t i = 0; i < p->nactive; i += 4) {
        uint8_t flags = 0;
        size_t take = p->nactive - i;
        if (take > 4)
            take = 4;
        for (size_t b = 0; b < take; ++b) {
            float v = p->values[0][i + b], av = fabsf(v);
            unsigned f = v >= 0 ? 2 : 0;
            if (av <= band) {
                f |= 1;
                ++r.frozen_voxels;
                p->values[0][i + b] = av;
            } else
                p->values[0][i + b] = INFINITY;
            flags |= (uint8_t)(f << (2 * b));
        }
        p->flags[i / 4] = flags;
    }
    r.unreached_voxels = p->nactive - r.frozen_voxels;
    if (!r.unreached_voxels)
        r.converged = 1;
    if (!max_iters || !r.frozen_voxels || !r.unreached_voxels) {
        if (result)
            *result = r;
        return TVDB_OK;
    }
    int ready = 1;
    for (int d = 0; d < 4; ++d)
        if (!p->ngroups[d])
            ready = 0;
    if (!ready) {
        st = sdf_planes(p, err);
        if (st != TVDB_OK)
            return st;
    }
    const int dirs[8][3] = {{1, 1, 1},  {-1, 1, 1},  {1, -1, 1},  {-1, -1, 1},
                            {1, 1, -1}, {-1, 1, -1}, {1, -1, -1}, {-1, -1, -1}};
    for (int iter = 0; iter < max_iters; ++iter) {
        for (size_t i = 0; i < p->nleaves; ++i) {
            p->leaves[i].change = 0;
            p->leaves[i].bad = 0;
        }
        for (int d = 0; d < 8; ++d) {
            int sx = dirs[d][0], sy = dirs[d][1], sz = dirs[d][2];
            int o = (sx * sy < 0 ? 1 : 0) + (sx * sz < 0 ? 2 : 0);
            sdf_sweep_job job = {p, p->order[o], sx, sy, sz};
            uint32_t n = p->ngroups[o];
            for (uint32_t step = 0; step < n; ++step) {
                uint32_t g = sx > 0 ? step : n - 1 - step;
                st = tvdb_thread_pool_for(p->pool, p->groups[o][g], p->groups[o][g + 1], 1,
                                          sdf_sweep_range, &job, err);
                if (st != TVDB_OK)
                    return st;
            }
        }
        if (sdf_bad(p))
            return sdf_error(err, TVDB_ERROR_INVALID_DATA, "SDF fast-sweeping distance overflow");
        r.iterations = iter + 1;
        r.max_change = 0;
        for (size_t i = 0; i < p->nleaves; ++i)
            if (p->leaves[i].change > r.max_change)
                r.max_change = p->leaves[i].change;
        if (r.max_change <= tol)
            break;
    }
    r.unreached_voxels = 0;
    for (size_t i = 0; i < p->nactive; ++i)
        if (!isfinite(p->values[0][i]))
            ++r.unreached_voxels;
    r.converged = r.max_change <= tol && !r.unreached_voxels;
    sdf_commit_job job = {p, p->values[0], 1};
    st = tvdb_thread_pool_for(p->pool, 0, p->nleaves, 1, sdf_commit, &job, err);
    if (st == TVDB_OK && result)
        *result = r;
    return st;
}
