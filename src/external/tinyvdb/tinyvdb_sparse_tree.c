#include "tinyvdb_sparse_tree.h"
#include "tinyvdb_checked.h"
#include "tinyvdb_tree_internal.h"
#include "tinyvdb_sdf_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Checked topology is built before callbacks, so allocation or structural
// errors cannot be mistaken for a successfully visited partial tree.
static int leaf_level_of(const tvdb_grid_t *g) {
    return g && g->tree.layout.num_levels >= 2 && g->tree.layout.num_levels <= TVDB_MAX_TREE_DEPTH
        ? g->tree.layout.num_levels-1 : -1;
}
static bool grid_is_float(const tvdb_grid_t *g) {
    int l=leaf_level_of(g);return l>=0 && g->tree.layout.levels[l].value_type==TVDB_VALUE_FLOAT;
}
static int leaf_log2dim_of(const tvdb_grid_t *g) {
    int l=leaf_level_of(g);return l>=0?g->tree.layout.levels[l].log2dim:0;
}
static size_t checked_visit(const tvdb_grid_t *g,int floats,tvdb_leaf_visit_fn cb,void *user) {
    if(!cb)return 0;
    tvdb_tree_index p;
    if(tvdb_tree_index_create(g,floats,&p,NULL)!=TVDB_OK)return 0;
    size_t count=0;
    /* Reverse sibling order retains the legacy depth-first leaf order. */
    for(size_t at=g->tree.num_nodes;at>1;--at) {
        size_t i=p.order[at-1];const tvdb_tree_node_t *n=g->tree.nodes+i;
        if(n->type!=TVDB_NODE_LEAF)continue;
        tvdb_leaf_view_t v;
        memcpy(v.origin,p.origins[i],sizeof(v.origin));
        v.log2dim=n->u.leaf.value_mask.log2dim;
        v.data=(const float*)n->u.leaf.data;v.value_mask=&n->u.leaf.value_mask;
        ++count;if(cb(&v,user))break;
    }
    tvdb_tree_index_destroy(&p);return count;
}
size_t tvdb_grid_visit_leaves_float(const tvdb_grid_t *g,tvdb_leaf_visit_fn cb,void *user) {
    return checked_visit(g,1,cb,user);
}
size_t tvdb_grid_visit_leaves(const tvdb_grid_t *g,tvdb_leaf_visit_fn cb,void *user) {
    return checked_visit(g,0,cb,user);
}
size_t tvdb_grid_active_voxel_count(const tvdb_grid_t *g) {
    tvdb_tree_index p;
    if(tvdb_tree_index_create(g,0,&p,NULL)!=TVDB_OK)return 0;
    size_t count=p.active_voxels;tvdb_tree_index_destroy(&p);return count;
}
bool tvdb_grid_active_bbox(const tvdb_grid_t *g,int32_t min[3],int32_t max[3]) {
    if(!min || !max)return false;
    tvdb_tree_index p;
    if(tvdb_tree_index_create(g,0,&p,NULL)!=TVDB_OK)return false;
    bool ok=p.has_bbox!=0;
    for(int a=0;a<3;++a)if(p.bbox_max[a]>INT32_MAX)ok=false;
    if(ok)for(int a=0;a<3;++a){min[a]=(int32_t)p.bbox_min[a];max[a]=(int32_t)p.bbox_max[a];}
    tvdb_tree_index_destroy(&p);return ok;
}
bool tvdb_grid_to_sparse(const tvdb_grid_t *g,tvdb_sparse_grid *out) {
    return tvdb_grid_to_sparse_ex(g,out,NULL)==TVDB_OK;
}
float tvdb_grid_float_background(const tvdb_grid_t *g) {
    return grid_is_float(g) && g->tree.nodes && g->tree.num_nodes &&
        g->tree.nodes[0].type==TVDB_NODE_ROOT && g->tree.nodes[0].u.root.background.type==TVDB_VALUE_FLOAT
        ? g->tree.nodes[0].u.root.background.u.f : 0;
}
bool tvdb_grid_materialize_dense(const tvdb_grid_t *g,const int32_t min[3],
    const int32_t max[3],float background,tvdb_dense_grid *out) {
    if(!out)return false;
    tvdb_dense_grid tmp={0};
    if(tvdb_grid_materialize_dense_ex(g,min,max,background,&tmp,NULL)!=TVDB_OK)return false;
    /* Legacy constructor accepts an uninitialized output. */
    *out=tmp;return true;
}

typedef struct { uint64_t lkey; uint32_t idx_plus_one; } leaf_hash_entry_t;
static uint64_t pack_leaf_key(int32_t lx, int32_t ly, int32_t lz) {
    uint64_t ux = (uint64_t)((int64_t)lx + (1LL << 20)) & ((1ULL << 21) - 1);
    uint64_t uy = (uint64_t)((int64_t)ly + (1LL << 20)) & ((1ULL << 21) - 1);
    uint64_t uz = (uint64_t)((int64_t)lz + (1LL << 20)) & ((1ULL << 21) - 1);
    return (ux << 42) | (uy << 21) | uz;
}

static uint64_t mix64_(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33; return k;
}

/* Smallest power of two >= v, or 0 if that does not exist. The old form,
   `while (p < v) p <<= 1`, overflows to 0 once v exceeds SIZE_MAX/2 and then
   spins forever, because 0 < v stays true. Every caller here feeds it
   `count * 2 + 16` with a count that came from a sparse structure, so the bound
   is not enforced by anything upstream. Returns 0 rather than aborting so the
   callers keep their existing "allocation failed" path. */
static size_t pow2_(size_t count, size_t factor) {
    size_t cap;
    return tvdb_hash_capacity(count, factor, &cap) ? cap : 0;
}


static bool grid_dilate_erode_active(const tvdb_grid_t *g,int iterations,int dilate,
                                     tvdb_sparse_grid *out) {
    if(!g || !out || iterations<0)return false;
    tvdb_tree_index index;
    if(tvdb_tree_index_create(g,1,&index,NULL)!=TVDB_OK)return false;
    float vs,origin[3];
    int valid=!index.active_tiles && tvdb_tree_dense_geometry(&g->transform,&vs,origin);
    tvdb_tree_index_destroy(&index);
    if(!valid)return false;
    tvdb_sdf_tree_t *workspace=NULL;
    /* Construction and noncommitting filtering never write to the source. */
    if(tvdb_sdf_tree_create((tvdb_grid_t*)g,NULL,NULL,&workspace,NULL)!=TVDB_OK)return false;
    tvdb_status_t status=tvdb_sdf_tree_filter_to_sparse(workspace,
        dilate?TVDB_SDF_FILTER_DILATE:TVDB_SDF_FILTER_ERODE,iterations,out,NULL);
    tvdb_sdf_tree_destroy(workspace);return status==TVDB_OK;
}
bool tvdb_grid_dilate_active(const tvdb_grid_t *g,int iterations,tvdb_sparse_grid *out) {
    return grid_dilate_erode_active(g,iterations,1,out);
}
bool tvdb_grid_erode_active(const tvdb_grid_t *g,int iterations,tvdb_sparse_grid *out) {
    return grid_dilate_erode_active(g,iterations,0,out);
}

// ----- topology-growing dilate + tree-aware CSG -----

bool tvdb_grid_dilate_topology(const tvdb_grid_t *grid, int iterations,
                               tvdb_sparse_grid *out) {
    if (!grid || !out) return false;
    tvdb_sparse_grid sg; tvdb_sparse_grid_init(&sg);
    if (!tvdb_grid_to_sparse(grid, &sg)) {
        tvdb_sparse_grid_free(&sg);
        return false;
    }
    float bg = tvdb_grid_float_background(grid);
    bool ok = tvdb_dilate_sparse(&sg, bg, iterations, out);
    tvdb_sparse_grid_free(&sg);
    return ok;
}

bool tvdb_grid_erode_topology(const tvdb_grid_t *grid, int iterations,
                              tvdb_sparse_grid *out) {
    if (!grid || !out) return false;
    tvdb_sparse_grid sg; tvdb_sparse_grid_init(&sg);
    if (!tvdb_grid_to_sparse(grid, &sg)) {
        tvdb_sparse_grid_free(&sg);
        return false;
    }
    bool ok = tvdb_erode_sparse(&sg, iterations, out);
    tvdb_sparse_grid_free(&sg);
    return ok;
}

static bool tree_csg_dispatch(const tvdb_grid_t *a,const tvdb_grid_t *b,
                              tvdb_sparse_grid *out,int op) {
    if(!a || !b || !out)return false;
    double ma[4][4],mb[4][4];
    if(!tvdb_tree_matrix(&a->transform,ma) || !tvdb_tree_matrix(&b->transform,mb))return false;
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)if(ma[i][j]!=mb[i][j])return false;
    tvdb_sparse_grid sa={0},sb={0},tmp={0};tvdb_tree_index ia={0},ib={0};
    bool ok=false;
    if(!tvdb_grid_to_sparse(a,&sa) || !tvdb_grid_to_sparse(b,&sb) ||
       tvdb_tree_index_create(a,1,&ia,NULL)!=TVDB_OK ||
       tvdb_tree_index_create(b,1,&ib,NULL)!=TVDB_OK ||
       !tvdb_csg_union_sparse(&sa,&sb,0,&tmp))goto done;
    for(size_t i=0;i<tmp.count;++i) {
        int32_t c[3]={tmp.coords[i].x,tmp.coords[i].y,tmp.coords[i].z};
        float x=tvdb_tree_get(&ia,c,NULL),y=tvdb_tree_get(&ib,c,NULL);
        if(!isfinite(x) || !isfinite(y))goto done;
        tmp.values[i]=op==0?fminf(x,y):op==1?fmaxf(x,y):fmaxf(x,-y);
    }
    tvdb_sparse_grid_free(out);*out=tmp;memset(&tmp,0,sizeof(tmp));ok=true;
done:
    tvdb_sparse_grid_free(&sa);tvdb_sparse_grid_free(&sb);tvdb_sparse_grid_free(&tmp);
    tvdb_tree_index_destroy(&ia);tvdb_tree_index_destroy(&ib);return ok;
}

bool tvdb_grid_csg_union(const tvdb_grid_t *a, const tvdb_grid_t *b,
                         tvdb_sparse_grid *out) {
    return tree_csg_dispatch(a, b, out, 0);
}
bool tvdb_grid_csg_intersection(const tvdb_grid_t *a, const tvdb_grid_t *b,
                                tvdb_sparse_grid *out) {
    return tree_csg_dispatch(a, b, out, 1);
}
bool tvdb_grid_csg_difference(const tvdb_grid_t *a, const tvdb_grid_t *b,
                              tvdb_sparse_grid *out) {
    return tree_csg_dispatch(a, b, out, 2);
}

// ----- update_from_sparse: topology-preserving voxel-value write -----

typedef struct mutable_leaf {
    int32_t  lcoord[3];   // leaf-coord (origin >> log2dim)
    int32_t  log2dim;
    float   *data;        // dim^3 floats (writable)
    tvdb_nodemask_t *value_mask; // writable
} mutable_leaf_t;

typedef struct mutable_leaf_collect {
    mutable_leaf_t *entries;
    size_t          count;
    size_t          capacity;
    int             log2dim;
} mutable_leaf_collect_t;

// ----- from-sparse builder (topology construction) -----

// Allocator that wraps the standard libc allocator. Required because
// tvdb__tree_destroy / nodemask_destroy / etc call alloc->free_fn(ptr, size,
// user_ctx) without a NULL check, so the fn pointers must be valid.
static void *s_libc_malloc(size_t size, void *ctx) { (void)ctx; return malloc(size); }
static void *s_libc_realloc(void *ptr, size_t old_size, size_t new_size, void *ctx) {
    (void)ctx; (void)old_size; return realloc(ptr, new_size);
}
static void s_libc_free(void *ptr, size_t size, void *ctx) {
    (void)ctx; (void)size; free(ptr);
}
static tvdb_allocator_t s_owned_alloc = {
    s_libc_malloc, s_libc_realloc, s_libc_free, NULL
};

/* Carve `size` bytes (16-aligned) from the tree's chunked pool. Returns NULL
 * on OOM. Chunks are never moved, so pointers carved earlier stay valid. */
static void *pool_alloc(tvdb_tree_t *tree, size_t size) {
    size = (size + 15u) & ~(size_t)15u;
    tvdb_pool_chunk_t *c = tree->pool;
    if (!c || c->size - c->used < size) {
        size_t chunk_size = size > 65536 ? size : 65536;
        tvdb_pool_chunk_t *nc =
            (tvdb_pool_chunk_t *)malloc(sizeof(tvdb_pool_chunk_t) + chunk_size);
        if (!nc) return NULL;
        nc->next = tree->pool;
        nc->size = chunk_size;
        nc->used = 0;
        tree->pool = nc;
        c = nc;
    }
    void *ptr = (char *)c + sizeof(tvdb_pool_chunk_t) + c->used;
    c->used += size;
    return ptr;
}

// Per-coord entry collected from input. `val_bytes` holds up to 24 bytes
// (matches the largest tvdb_value_type_t = VEC3D); only `vsize` bytes are
// meaningful per builder invocation.
typedef struct { int32_t lorig[3]; int32_t slot; uint32_t order; uint8_t val_bytes[24]; } tvdb__coord_entry;
// Sort PRIMARILY by leaf origin (not by full coord). Coords with the same
// leaf origin must be contiguous so the per-leaf grouping loop is correct.
static int tvdb__cmp_coord_entry(const void *a, const void *b) {
    const tvdb__coord_entry *A = (const tvdb__coord_entry *)a;
    const tvdb__coord_entry *B = (const tvdb__coord_entry *)b;
    if (A->lorig[0] != B->lorig[0]) return (A->lorig[0] < B->lorig[0]) ? -1 : 1;
    if (A->lorig[1] != B->lorig[1]) return (A->lorig[1] < B->lorig[1]) ? -1 : 1;
    if (A->lorig[2] != B->lorig[2]) return (A->lorig[2] < B->lorig[2]) ? -1 : 1;
    // Within same leaf, sort by slot, then by input position. qsort is not
    // stable, so the input-order tie-break is what makes duplicate coordinates
    // deterministic: the builder keeps the first occurrence of each slot.
    if (A->slot != B->slot) return (A->slot < B->slot) ? -1 : 1;
    return (A->order < B->order) ? -1 : (A->order > B->order);
}

static char *xstrdup_(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static bool nodemask_alloc_owned(tvdb_nodemask_t *m, int log2dim) {
    m->log2dim = log2dim;
    m->bitsize = 1 << (3 * log2dim);
    m->bits.num_bits = (size_t)m->bitsize;
    m->bits.num_bytes = ((size_t)m->bitsize + 7) / 8;
    m->bits.alloc = &s_owned_alloc;
    m->bits.data = (uint8_t *)calloc(m->bits.num_bytes, 1);
    return m->bits.data != NULL;
}

static inline void nm_set(tvdb_nodemask_t *m, int32_t i) {
    m->bits.data[i >> 3] |= (uint8_t)(1u << (i & 7));
}

// Cumulative voxel-extent of all levels descending from level `lv` (inclusive).
// Equals 2^(sum of log2dims from lv to leaf).
static int level_voxel_span(const tvdb_grid_layout_t *layout, int lv) {
    int sum = 0;
    for (int i = lv; i < layout->num_levels; ++i) sum += layout->levels[i].log2dim;
    return 1 << sum;
}

// Compute slot index in a parent at level `lv` for a child whose origin lies
// at `(child_origin)`. The slot uses OpenVDB packing:
//   slot = (sx << 2*L) | (sy << L) | sz, where L = log2dim of `lv`.
static int32_t slot_in_parent(const tvdb_grid_layout_t *layout, int lv,
                              const int32_t parent_origin[3],
                              const int32_t child_origin[3]) {
    int L = layout->levels[lv].log2dim;
    int child_span = level_voxel_span(layout, lv + 1);
    int dim_mask = (1 << L) - 1;
    int sx = ((child_origin[0] - parent_origin[0]) / child_span) & dim_mask;
    int sy = ((child_origin[1] - parent_origin[1]) / child_span) & dim_mask;
    int sz = ((child_origin[2] - parent_origin[2]) / child_span) & dim_mask;
    return (sx << (2 * L)) | (sy << L) | sz;
}

typedef struct {
    int32_t origin[3];
    size_t  node_idx;       // index into tree.nodes[]
} grouping_entry_t;

// Comparator for grouping_entry_t: lexicographic on origin.
static int cmp_origin(const void *a, const void *b) {
    const grouping_entry_t *A = (const grouping_entry_t *)a;
    const grouping_entry_t *B = (const grouping_entry_t *)b;
    if (A->origin[0] != B->origin[0]) return (A->origin[0] < B->origin[0]) ? -1 : 1;
    if (A->origin[1] != B->origin[1]) return (A->origin[1] < B->origin[1]) ? -1 : 1;
    if (A->origin[2] != B->origin[2]) return (A->origin[2] < B->origin[2]) ? -1 : 1;
    return 0;
}

// Add a new node to tree.nodes[]; returns index. Allocates nodes[] via realloc.
static size_t append_node(tvdb_tree_t *tree, tvdb_node_type_t type, int level,
                          const int32_t origin[3]) {
    if (tree->num_nodes >= tree->nodes_capacity) {
        size_t new_cap = tree->nodes_capacity ? tree->nodes_capacity * 2 : 64;
        tvdb_tree_node_t *nn = (tvdb_tree_node_t *)realloc(
            tree->nodes, new_cap * sizeof(tvdb_tree_node_t));
        if (!nn) return (size_t)-1;
        memset(nn + tree->nodes_capacity, 0,
               (new_cap - tree->nodes_capacity) * sizeof(tvdb_tree_node_t));
        tree->nodes = nn;
        tree->nodes_capacity = new_cap;
    }
    size_t idx = tree->num_nodes++;
    memset(&tree->nodes[idx], 0, sizeof(tvdb_tree_node_t));
    tree->nodes[idx].type = type;
    tree->nodes[idx].level = level;
    tree->nodes[idx].origin[0] = origin[0];
    tree->nodes[idx].origin[1] = origin[1];
    tree->nodes[idx].origin[2] = origin[2];
    return idx;
}

typedef struct { int32_t slot; size_t idx; } slot_pair_t;

// Comparator for slot_pair_t: ascending slot.
static int cmp_slot_pair(const void* a, const void* b) {
    const slot_pair_t* x = (const slot_pair_t*)a;
    const slot_pair_t* y = (const slot_pair_t*)b;
    if (x->slot != y->slot) return x->slot < y->slot ? -1 : 1;
    return 0;
}

// Build internal nodes at level `lv` from an unsorted children list.
// Groups children by parent origin via hash table, regardless of input order.
// Returns a list of created internal-node entries (one per unique parent).
static bool build_parent_level(tvdb_tree_t *tree, int parent_lv,
                               int child_lv,
                               const grouping_entry_t *children,
                               size_t n_children,
                               int vsize, const void *bg_bytes,
                               grouping_entry_t **out_parents,
                               size_t *out_n_parents) {
    (void)child_lv;
    if (n_children == 0) {
        *out_parents = NULL; *out_n_parents = 0; return true;
    }
    // Parent voxel extent = covers (1 << log2dim) children of voxel-extent
    // child_span. parent_voxel_span = level_voxel_span(parent_lv).
    int parent_span = level_voxel_span(&tree->layout, parent_lv);
    int parent_log2dim = tree->layout.levels[parent_lv].log2dim;
    int parent_bitsize = 1 << (3 * parent_log2dim);

    // Floor-divide helper for parent origin computation.
    #define PORIGIN(c) ((int32_t)((int64_t)(c) - \
        (((int64_t)(c) % parent_span + parent_span) % parent_span)))

    // Hash table: (parent_origin) -> index into local `pgroup` array.
    // Key = pack 3x21-bit signed-shifted origins into uint64.
    size_t htbl_cap = pow2_(n_children, 2);
    size_t htbl_mask = htbl_cap - 1;
    typedef struct { uint64_t key; uint32_t group_plus_one; } htbl_entry_t;
    htbl_entry_t *htbl = (htbl_entry_t *)calloc(htbl_cap, sizeof(htbl_entry_t));
    /* cap is checked separately: calloc(0, ..) is permitted to return non-NULL,
       which would sail past the null test and leave htbl_mask = SIZE_MAX. */
    if (!htbl || !htbl_cap) return false;

    typedef struct {
        int32_t origin[3];
        size_t  *child_idx;     // node indices of children in this group
        size_t   n_child;
        size_t   child_cap;
    } pgroup_t;
    pgroup_t *pg = NULL;
    size_t n_pg = 0, pg_cap = 0;

    for (size_t i = 0; i < n_children; ++i) {
        int32_t porig[3] = {
            PORIGIN(children[i].origin[0]),
            PORIGIN(children[i].origin[1]),
            PORIGIN(children[i].origin[2])
        };
        uint64_t key = pack_leaf_key(porig[0], porig[1], porig[2]);
        size_t h = (size_t)(mix64_(key) & htbl_mask);
        size_t group_idx = (size_t)-1;
        while (htbl[h].group_plus_one) {
            size_t gi = htbl[h].group_plus_one - 1;
            if (pg[gi].origin[0] == porig[0] &&
                pg[gi].origin[1] == porig[1] &&
                pg[gi].origin[2] == porig[2]) { group_idx = gi; break; }
            h = (h + 1) & htbl_mask;
        }
        if (group_idx == (size_t)-1) {
            // New parent group.
            if (n_pg == pg_cap) {
                size_t nc = pg_cap ? pg_cap * 2 : 32;
                pgroup_t *np = (pgroup_t *)realloc(pg, nc * sizeof(pgroup_t));
                if (!np) goto fail;
                pg = np; pg_cap = nc;
            }
            pg[n_pg].origin[0] = porig[0];
            pg[n_pg].origin[1] = porig[1];
            pg[n_pg].origin[2] = porig[2];
            pg[n_pg].child_idx = NULL;
            pg[n_pg].n_child = 0;
            pg[n_pg].child_cap = 0;
            group_idx = n_pg++;
            htbl[h].key = key;
            htbl[h].group_plus_one = (uint32_t)(group_idx + 1);
        }
        // Append child node_idx to this group.
        pgroup_t *gp = &pg[group_idx];
        if (gp->n_child == gp->child_cap) {
            size_t nc = gp->child_cap ? gp->child_cap * 2 : 8;
            size_t *na = (size_t *)realloc(gp->child_idx, nc * sizeof(size_t));
            if (!na) goto fail;
            gp->child_idx = na; gp->child_cap = nc;
        }
        gp->child_idx[gp->n_child++] = children[i].node_idx;
    }
    free(htbl); htbl = NULL;

    // Build internal nodes from groups.
    grouping_entry_t *parents = (grouping_entry_t *)malloc(n_pg * sizeof(grouping_entry_t));
    if (!parents) goto fail;

    for (size_t g = 0; g < n_pg; ++g) {
        pgroup_t *gp = &pg[g];
        size_t pidx = append_node(tree, TVDB_NODE_INTERNAL, parent_lv, gp->origin);
        if (pidx == (size_t)-1) { free(parents); goto fail; }
        tvdb_internal_node_t *in = &tree->nodes[pidx].u.internal;
        if (!nodemask_alloc_owned(&in->child_mask, parent_log2dim) ||
            !nodemask_alloc_owned(&in->value_mask, parent_log2dim)) {
            free(parents); goto fail;
        }
        in->num_children = gp->n_child;
        in->child_indices = (size_t *)malloc(gp->n_child * sizeof(size_t));
        if (!in->child_indices) { free(parents); goto fail; }
        in->values_size = (size_t)parent_bitsize * (size_t)vsize;
        in->values = (uint8_t *)pool_alloc(tree, in->values_size);
        if (!in->values) { free(parents); goto fail; }
        tree->nodes[pidx].pooled = 1;
        for (int k = 0; k < parent_bitsize; ++k) {
            memcpy(in->values + (size_t)k * vsize, bg_bytes, (size_t)vsize);
        }
        // Compute slots, sort by slot, populate child_mask + child_indices.
        slot_pair_t *pairs = (slot_pair_t *)malloc(gp->n_child * sizeof(slot_pair_t));
        if (!pairs) { free(parents); goto fail; }
        for (size_t k = 0; k < gp->n_child; ++k) {
            // Need each child's origin. Look it up via tree.nodes.
            size_t cni = gp->child_idx[k];
            int32_t corigin[3] = {
                tree->nodes[cni].origin[0],
                tree->nodes[cni].origin[1],
                tree->nodes[cni].origin[2]
            };
            pairs[k].slot = slot_in_parent(&tree->layout, parent_lv,
                                           gp->origin, corigin);
            pairs[k].idx = cni;
        }
        qsort(pairs, gp->n_child, sizeof(*pairs), cmp_slot_pair);
        for (size_t k = 0; k < gp->n_child; ++k) {
            nm_set(&in->child_mask, pairs[k].slot);
            in->child_indices[k] = pairs[k].idx;
        }
        free(pairs);
        parents[g].origin[0] = gp->origin[0];
        parents[g].origin[1] = gp->origin[1];
        parents[g].origin[2] = gp->origin[2];
        parents[g].node_idx = pidx;
    }

    for (size_t i = 0; i < n_pg; ++i) free(pg[i].child_idx);
    free(pg);
    *out_parents = parents;
    *out_n_parents = n_pg;
    #undef PORIGIN
    return true;

fail:
    if (htbl) free(htbl);
    if (pg) { for (size_t i = 0; i < n_pg; ++i) free(pg[i].child_idx); free(pg); }
    #undef PORIGIN
    return false;
}

// Public typed-builder entry point. See header for documentation.
bool tvdb_grid_from_sparse_typed_using_template(const tvdb_grid_t *tmpl,
                                              const tvdb_vec3i *coords,
                                              const void *values,
                                              size_t count,
                                              tvdb_value_type_t value_type,
                                              const void *bg_bytes,
                                              const char *grid_name,
                                              tvdb_grid_t *out) {
    if (!tmpl || !out || tmpl == out) return false;
    int leaf_lv = tmpl->tree.layout.num_levels - 1;
    if (leaf_lv < 0) return false;
    if (tmpl->tree.layout.num_levels != 4) return false;
    if (tmpl->tree.layout.levels[leaf_lv].value_type != value_type) return false;
    int vsize = (int)tvdb_value_type_size(value_type);
    if (vsize <= 0 || vsize > 24) return false;
    if (!bg_bytes || (count > 0 && (!coords || !values))) return false;

    const int dims[4] = {0,5,4,3};
    for(int l=0;l<4;++l) {
        tvdb_node_type_t expected=l==0?TVDB_NODE_ROOT:l==3?TVDB_NODE_LEAF:TVDB_NODE_INTERNAL;
        if(tmpl->tree.layout.levels[l].log2dim!=dims[l] ||
           tmpl->tree.layout.levels[l].value_type!=value_type ||
           tmpl->tree.layout.levels[l].node_type!=expected)return false;
    }
    size_t coord_bytes,value_bytes;
    if(!tvdb_size_mul(count,sizeof(tvdb__coord_entry),&coord_bytes) ||
       !tvdb_size_mul(count,(size_t)vsize,&value_bytes))return false;

    memset(out, 0, sizeof(*out));
    int leaf_log2dim = tmpl->tree.layout.levels[leaf_lv].log2dim;
    int leaf_dim = 1 << leaf_log2dim;
    int leaf_dim_mask = leaf_dim - 1;
    int leaf_bitsize = 1 << (3 * leaf_log2dim);

    // Descriptor: pick grid_type string from value_type (so the writer +
    // a fresh reader agree on element width). The template's grid_type
    // is only used as a fallback for FLOAT, since older callers (and the
    // float-only entry point) inherit the template string verbatim.
    out->descriptor.grid_name = xstrdup_(grid_name ? grid_name : "");
    const char *type_str = NULL;
    switch (value_type) {
        case TVDB_VALUE_FLOAT:
            type_str = (tmpl->descriptor.grid_type && tmpl->descriptor.grid_type[0])
                       ? tmpl->descriptor.grid_type : "Tree_float_5_4_3";
            break;
        case TVDB_VALUE_DOUBLE: type_str = "Tree_double_5_4_3"; break;
        case TVDB_VALUE_INT32:  type_str = "Tree_int32_5_4_3";  break;
        case TVDB_VALUE_INT64:  type_str = "Tree_int64_5_4_3";  break;
        case TVDB_VALUE_BOOL:   type_str = "Tree_bool_5_4_3";   break;
        case TVDB_VALUE_VEC3F:
            type_str = (tmpl->descriptor.grid_type && tmpl->descriptor.grid_type[0])
                       ? tmpl->descriptor.grid_type : "Tree_vec3s_5_4_3";
            break;
        case TVDB_VALUE_VEC3D:  type_str = "Tree_vec3d_5_4_3";  break;
        case TVDB_VALUE_VEC3I:  type_str = "Tree_vec3i_5_4_3";  break;
        default:                type_str = "Tree_float_5_4_3";  break;
    }
    out->descriptor.grid_type = xstrdup_(type_str);
    if(!out->descriptor.grid_name || !out->descriptor.grid_type) { tvdb_grid_destroy_owned(out);return false; }

    // Transform: deep-copy (no pointers in tvdb_transform_t).
    out->transform = tmpl->transform;

    // Tree skeleton.
    out->tree.alloc = &s_owned_alloc;
    out->tree.layout = tmpl->tree.layout;
    out->tree.num_nodes = 0;
    out->tree.nodes_capacity = 0;
    out->tree.nodes = NULL;
    out->tree.is_point_data_grid = 0;
    out->tree.is_point_index_grid = 0;

    // Step 1: group sparse coords by leaf origin.
    // Create one (origin, slot, value-bytes) entry per coord, sort by leaf origin.
    const uint8_t *vbytes = (const uint8_t *)values;
    tvdb__coord_entry *ce = NULL;
    if (count > 0) {
        ce = (tvdb__coord_entry *)malloc(coord_bytes);
        if (!ce) { tvdb_grid_destroy_owned(out); return false; }
    }
    for (size_t ii = 0; ii < count; ++ii) {
        int32_t cx = coords[ii].x, cy = coords[ii].y, cz = coords[ii].z;
        int32_t lx = (int32_t)((int64_t)cx - (((int64_t)cx % leaf_dim + leaf_dim) % leaf_dim));
        int32_t ly = (int32_t)((int64_t)cy - (((int64_t)cy % leaf_dim + leaf_dim) % leaf_dim));
        int32_t lz = (int32_t)((int64_t)cz - (((int64_t)cz % leaf_dim + leaf_dim) % leaf_dim));
        ce[ii].lorig[0] = lx; ce[ii].lorig[1] = ly; ce[ii].lorig[2] = lz;
        int slx = cx & leaf_dim_mask;
        int sly = cy & leaf_dim_mask;
        int slz = cz & leaf_dim_mask;
        ce[ii].slot = (slx << (2 * leaf_log2dim)) | (sly << leaf_log2dim) | slz;
        ce[ii].order = ii;
        memcpy(ce[ii].val_bytes, vbytes + ii * (size_t)vsize, (size_t)vsize);
    }
    if (count > 1) qsort(ce, count, sizeof(tvdb__coord_entry), tvdb__cmp_coord_entry);

    // Step 2: walk sorted ce[], emit a leaf node per unique leaf origin.
    grouping_entry_t *leaves = NULL;
    size_t leaf_cap = 0, n_leaves = 0;

    size_t i = 0;
    while (i < count) {
        int32_t lorig[3] = { ce[i].lorig[0], ce[i].lorig[1], ce[i].lorig[2] };
        size_t leaf_node_idx = append_node(&out->tree, TVDB_NODE_LEAF, leaf_lv, lorig);
        if (leaf_node_idx == (size_t)-1) { free(ce); free(leaves); tvdb_grid_destroy_owned(out); return false; }
        tvdb_leaf_node_t *leaf = &out->tree.nodes[leaf_node_idx].u.leaf;
        if (!nodemask_alloc_owned(&leaf->value_mask, leaf_log2dim)) {
            free(ce); free(leaves); tvdb_grid_destroy_owned(out); return false;
        }
        leaf->num_voxels = (uint32_t)leaf_bitsize;
        leaf->data_size = (size_t)leaf_bitsize * (size_t)vsize;
        leaf->data = (uint8_t *)pool_alloc(&out->tree, leaf->data_size);
        if (!leaf->data) { free(ce); free(leaves); tvdb_grid_destroy_owned(out); return false; }
        out->tree.nodes[leaf_node_idx].pooled = 1;
        // Fill all voxels with background (inactive default).
        for (int k = 0; k < leaf_bitsize; ++k) {
            memcpy(leaf->data + (size_t)k * (size_t)vsize, bg_bytes, (size_t)vsize);
        }
        // Write active voxels in this group. Duplicates of a coordinate are
        // adjacent and ordered by input position; only the first is written.
        size_t group_start = i;
        while (i < count &&
               ce[i].lorig[0] == lorig[0] &&
               ce[i].lorig[1] == lorig[1] &&
               ce[i].lorig[2] == lorig[2]) {
            int32_t slot = ce[i].slot;
            if (i > group_start && ce[i - 1].slot == slot) { ++i; continue; }
            memcpy(leaf->data + (size_t)slot * (size_t)vsize, ce[i].val_bytes, (size_t)vsize);
            nm_set(&leaf->value_mask, slot);
            ++i;
        }
        // Append to leaves grouping list.
        if (n_leaves == leaf_cap) {
            leaf_cap = leaf_cap ? leaf_cap * 2 : 64;
            grouping_entry_t *nl = (grouping_entry_t *)realloc(leaves, leaf_cap * sizeof(grouping_entry_t));
            if (!nl) { free(ce); free(leaves); tvdb_grid_destroy_owned(out); return false; }
            leaves = nl;
        }
        leaves[n_leaves].origin[0] = lorig[0];
        leaves[n_leaves].origin[1] = lorig[1];
        leaves[n_leaves].origin[2] = lorig[2];
        leaves[n_leaves].node_idx = leaf_node_idx;
        ++n_leaves;
    }
    free(ce);

    // Step 3: build L_(N-2) parents from leaves.
    grouping_entry_t *l2_parents = NULL; size_t n_l2 = 0;
    if (!build_parent_level(&out->tree, /*parent_lv=*/leaf_lv - 1,
                             /*child_lv=*/leaf_lv,
                             leaves, n_leaves, vsize, bg_bytes,
                             &l2_parents, &n_l2)) {
        free(leaves); tvdb_grid_destroy_owned(out); return false;
    }
    free(leaves);
    // Re-sort l2_parents by origin to be safe.
    if (n_l2 > 1) qsort(l2_parents, n_l2, sizeof(grouping_entry_t), cmp_origin);

    // Step 4: build L_(N-3) (= level 1 = root's children) from l2_parents.
    grouping_entry_t *l1_parents = NULL; size_t n_l1 = 0;
    if (!build_parent_level(&out->tree, /*parent_lv=*/leaf_lv - 2,
                             /*child_lv=*/leaf_lv - 1,
                             l2_parents, n_l2, vsize, bg_bytes,
                             &l1_parents, &n_l1)) {
        free(l2_parents); tvdb_grid_destroy_owned(out); return false;
    }
    free(l2_parents);
    if (n_l1 > 1) qsort(l1_parents, n_l1, sizeof(grouping_entry_t), cmp_origin);

    // Step 5: build root with l1_parents as children.
    int32_t root_origin[3] = {0, 0, 0};
    size_t root_idx = append_node(&out->tree, TVDB_NODE_ROOT, 0, root_origin);
    if (root_idx == (size_t)-1) { free(l1_parents); tvdb_grid_destroy_owned(out); return false; }
    // Root must be at index 0 conceptually, but tree allows any index.
    // The visit code in this file uses idx 0 as root start, so we want root first.
    // Easiest: swap root to index 0 if not already.
    if (root_idx != 0 && out->tree.num_nodes > 1) {
        tvdb_tree_node_t tmp = out->tree.nodes[0];
        out->tree.nodes[0] = out->tree.nodes[root_idx];
        out->tree.nodes[root_idx] = tmp;
        // Patch any child_indices pointing at the swapped node.
        for (size_t n = 0; n < out->tree.num_nodes; ++n) {
            if (out->tree.nodes[n].type == TVDB_NODE_INTERNAL) {
                tvdb_internal_node_t *in = &out->tree.nodes[n].u.internal;
                for (size_t c = 0; c < in->num_children; ++c) {
                    if (in->child_indices[c] == 0) in->child_indices[c] = root_idx;
                    else if (in->child_indices[c] == root_idx) in->child_indices[c] = 0;
                }
            }
        }
        // Patch l1_parents node_idx references.
        for (size_t k = 0; k < n_l1; ++k) {
            if (l1_parents[k].node_idx == 0) l1_parents[k].node_idx = root_idx;
            else if (l1_parents[k].node_idx == root_idx) l1_parents[k].node_idx = 0;
        }
        root_idx = 0;
    }
    tvdb_root_node_t *root = &out->tree.nodes[root_idx].u.root;
    root->background.type = value_type;
    memset(&root->background.u, 0, sizeof(root->background.u));
    switch (value_type) {
        case TVDB_VALUE_FLOAT:  memcpy(&root->background.u.f,    bg_bytes, sizeof(float));      break;
        case TVDB_VALUE_DOUBLE: memcpy(&root->background.u.d,    bg_bytes, sizeof(double));     break;
        case TVDB_VALUE_INT32:  memcpy(&root->background.u.i32,  bg_bytes, sizeof(int32_t));    break;
        case TVDB_VALUE_INT64:  memcpy(&root->background.u.i64,  bg_bytes, sizeof(int64_t));    break;
        case TVDB_VALUE_VEC3F:  memcpy(root->background.u.vec3f, bg_bytes, 3 * sizeof(float));  break;
        case TVDB_VALUE_VEC3I:  memcpy(root->background.u.vec3i, bg_bytes, 3 * sizeof(int32_t));break;
        case TVDB_VALUE_VEC3D:  memcpy(root->background.u.vec3d, bg_bytes, 3 * sizeof(double)); break;
        default: break;
    }
    root->num_tiles = 0;
    root->tile_origins = NULL;
    root->tile_values = NULL;
    root->tile_active = NULL;
    root->num_children = (uint32_t)n_l1;
    if (n_l1 > 0) {
        root->child_origins = (int32_t *)malloc((size_t)n_l1 * 3 * sizeof(int32_t));
        root->child_indices = (size_t *)malloc((size_t)n_l1 * sizeof(size_t));
        if (!root->child_origins || !root->child_indices) {
            free(l1_parents); tvdb_grid_destroy_owned(out); return false;
        }
        for (size_t k = 0; k < n_l1; ++k) {
            root->child_origins[3*k + 0] = l1_parents[k].origin[0];
            root->child_origins[3*k + 1] = l1_parents[k].origin[1];
            root->child_origins[3*k + 2] = l1_parents[k].origin[2];
            root->child_indices[k] = l1_parents[k].node_idx;
        }
    }
    free(l1_parents);
    return true;
}

// Topology-extending merge: build a new owned grid that contains every
// active voxel in `existing` plus every (coord, value) in `sg`. Where the
// two overlap, `sg`'s value wins. New leaves are created where `sg`
// references coordinates outside `existing`'s active set. Output ownership
// matches tvdb_grid_from_sparse_using_template (free with
// tvdb_grid_destroy_owned).
bool tvdb_grid_extend_from_sparse(const tvdb_grid_t *existing,
                                  const tvdb_sparse_grid *sg,
                                  const char *grid_name,
                                  float background,
                                  tvdb_grid_t *out) {
    if (!existing || !sg || !out) return false;
    if (!grid_is_float(existing)) return false;
    if (existing->tree.layout.num_levels != 4) return false;

    // Pull existing active voxels into a flat sparse grid.
    tvdb_sparse_grid old_sg; tvdb_sparse_grid_init(&old_sg);
    if (!tvdb_grid_to_sparse(existing, &old_sg)) {
        tvdb_sparse_grid_free(&old_sg);
        return false;
    }
    // Build a hash table over sg's coords for O(1) override lookup.
    size_t cap = pow2_(sg->count, 2);
    typedef struct { uint64_t key; uint32_t idx_plus_one; } merge_he_t;
    merge_he_t *htbl = (merge_he_t *)calloc(cap, sizeof(merge_he_t));
    if (!htbl || !cap) { tvdb_sparse_grid_free(&old_sg); return false; }
    size_t mask = cap - 1;
    for (size_t i = 0; i < sg->count; ++i) {
        uint64_t key = pack_leaf_key(sg->coords[i].x, sg->coords[i].y, sg->coords[i].z);
        size_t h = (size_t)(mix64_(key) & mask);
        while (htbl[h].idx_plus_one) {
            size_t pi = htbl[h].idx_plus_one - 1;
            if (sg->coords[pi].x == sg->coords[i].x &&
                sg->coords[pi].y == sg->coords[i].y &&
                sg->coords[pi].z == sg->coords[i].z) {
                // Duplicate inside sg: this table only marks coordinates
                // as overridden; the builder keeps sg's first occurrence.
                break;
            }
            h = (h + 1) & mask;
        }
        htbl[h].key = key;
        htbl[h].idx_plus_one = (uint32_t)(i + 1);
    }

    // Output capacity upper bound = old_sg.count + sg->count.
    tvdb_sparse_grid merged; tvdb_sparse_grid_init(&merged);
    if (!tvdb_sparse_grid_reserve(&merged, old_sg.count + sg->count)) {
        free(htbl); tvdb_sparse_grid_free(&old_sg); return false;
    }
    merged.voxel_size = old_sg.voxel_size;
    merged.ox = old_sg.ox; merged.oy = old_sg.oy; merged.oz = old_sg.oz;

    // Walk old_sg; if a coord is overridden by sg, skip it (sg writes its own
    // entry below). Otherwise emit it.
    for (size_t i = 0; i < old_sg.count; ++i) {
        uint64_t key = pack_leaf_key(old_sg.coords[i].x, old_sg.coords[i].y, old_sg.coords[i].z);
        size_t h = (size_t)(mix64_(key) & mask);
        int overridden = 0;
        while (htbl[h].idx_plus_one) {
            size_t pi = htbl[h].idx_plus_one - 1;
            if (sg->coords[pi].x == old_sg.coords[i].x &&
                sg->coords[pi].y == old_sg.coords[i].y &&
                sg->coords[pi].z == old_sg.coords[i].z) { overridden = 1; break; }
            h = (h + 1) & mask;
        }
        if (overridden) continue;
        merged.coords[merged.count] = old_sg.coords[i];
        merged.values[merged.count] = old_sg.values[i];
        ++merged.count;
    }
    // Append every coord/value from sg (these win on overlap; new leaves elsewhere).
    for (size_t i = 0; i < sg->count; ++i) {
        merged.coords[merged.count] = sg->coords[i];
        merged.values[merged.count] = sg->values[i];
        ++merged.count;
    }

    free(htbl);
    tvdb_sparse_grid_free(&old_sg);

    bool ok = tvdb_grid_from_sparse_using_template(existing, &merged, grid_name,
                                                   background, out);
    tvdb_sparse_grid_free(&merged);
    return ok;
}

bool tvdb_grid_from_sparse_using_template(const tvdb_grid_t *tmpl,
                                          const tvdb_sparse_grid *sg,
                                          const char *grid_name,
                                          float background,
                                          tvdb_grid_t *out) {
    if (!sg) return false;
    return tvdb_grid_from_sparse_typed_using_template(tmpl, sg->coords, sg->values,
                                             sg->count, TVDB_VALUE_FLOAT,
                                             &background, grid_name, out);
}

bool tvdb_grid_from_sparse_vec3_using_template(const tvdb_grid_t *tmpl,
                                               const tvdb_vec3i *coords,
                                               const float *values,
                                               size_t count,
                                               const char *grid_name,
                                               const float background[3],
                                               tvdb_grid_t *out) {
    float bg[3] = {0.0f, 0.0f, 0.0f};
    if (background) { bg[0] = background[0]; bg[1] = background[1]; bg[2] = background[2]; }
    return tvdb_grid_from_sparse_typed_using_template(tmpl, coords, values, count,
                                             TVDB_VALUE_VEC3F, bg, grid_name, out);
}

void tvdb_grid_destroy_owned(tvdb_grid_t *grid) {
    tvdb_grid_destroy(grid, NULL);
}

size_t tvdb_grid_update_from_sparse(tvdb_grid_t *grid,
                                    const tvdb_sparse_grid *sg,
                                    size_t *out_skipped) {
    if (out_skipped) *out_skipped = 0;
    if (!grid || !sg || sg->count == 0) return 0;
    size_t input_bytes;
    if (!sg->coords || !sg->values || !tvdb_size_mul(sg->count, sizeof(tvdb_vec3i), &input_bytes)) return 0;
    for (size_t i = 0; i < sg->count; ++i) if (!isfinite(sg->values[i])) return 0;
    tvdb_tree_index index;
    if (tvdb_tree_index_create(grid, 1, &index, NULL) != TVDB_OK) return 0;
    mutable_leaf_collect_t leaves; memset(&leaves, 0, sizeof(leaves));
    for (size_t i = 1; i < grid->tree.num_nodes; ++i)
        leaves.count += grid->tree.nodes[i].type == TVDB_NODE_LEAF;
    size_t bytes;
    if (leaves.count > UINT32_MAX || !tvdb_size_mul(leaves.count, sizeof(mutable_leaf_t), &bytes) ||
        (bytes && !(leaves.entries = malloc(bytes)))) {
        tvdb_tree_index_destroy(&index); return 0;
    }
    size_t k = 0;
    for (size_t i = 1; i < grid->tree.num_nodes; ++i) {
        tvdb_tree_node_t *n = grid->tree.nodes + i;
        if (n->type != TVDB_NODE_LEAF) continue;
        mutable_leaf_t *e = leaves.entries + k++;
        int L = n->u.leaf.value_mask.log2dim;
        e->log2dim = L; leaves.log2dim = L;
        for (int a = 0; a < 3; ++a) e->lcoord[a] = index.origins[i][a] >> L;
        e->data = (float *)n->u.leaf.data; e->value_mask = &n->u.leaf.value_mask;
    }
    tvdb_tree_index_destroy(&index);
    if (leaves.count == 0) {
        free(leaves.entries);
        if (out_skipped) *out_skipped = sg->count;
        return 0;
    }

    int L = leaves.log2dim;
    int32_t dim_mask = (1 << L) - 1;

    // Build hash table on leaf-coords.
    size_t cap = pow2_(leaves.count, 2);
    leaf_hash_entry_t *htbl = (leaf_hash_entry_t *)calloc(cap, sizeof(leaf_hash_entry_t));
    if (!htbl || !cap) { free(htbl); free(leaves.entries); return 0; }
    size_t mask = cap - 1;
    for (size_t i = 0; i < leaves.count; ++i) {
        uint64_t key = pack_leaf_key(leaves.entries[i].lcoord[0],
                                     leaves.entries[i].lcoord[1],
                                     leaves.entries[i].lcoord[2]);
        size_t h = (size_t)(mix64_(key) & mask);
        while (htbl[h].idx_plus_one) h = (h + 1) & mask;
        htbl[h].lkey = key;
        htbl[h].idx_plus_one = (uint32_t)(i + 1);
    }

    size_t updated = 0, skipped = 0;
    for (size_t i = 0; i < sg->count; ++i) {
        int32_t cx = sg->coords[i].x, cy = sg->coords[i].y, cz = sg->coords[i].z;
        int32_t lcx = cx >> L, lcy = cy >> L, lcz = cz >> L;
        // Hash lookup: linear-probe until idx matches or empty.
        uint64_t key = pack_leaf_key(lcx, lcy, lcz);
        size_t h = (size_t)(mix64_(key) & mask);
        int found = -1;
        while (htbl[h].idx_plus_one) {
            size_t idx = htbl[h].idx_plus_one - 1;
            if (leaves.entries[idx].lcoord[0] == lcx &&
                leaves.entries[idx].lcoord[1] == lcy &&
                leaves.entries[idx].lcoord[2] == lcz) {
                found = (int)idx;
                break;
            }
            h = (h + 1) & mask;
        }
        if (found < 0) { ++skipped; continue; }
        int32_t lx = cx & dim_mask;
        int32_t ly = cy & dim_mask;
        int32_t lz = cz & dim_mask;
        int32_t slot = (lx << (2 * L)) | (ly << L) | lz;
        leaves.entries[found].data[slot] = sg->values[i];
        tvdb_nodemask_set_on(leaves.entries[found].value_mask, slot);
        ++updated;
    }

    free(htbl);
    free(leaves.entries);
    if (out_skipped) *out_skipped = skipped;
    return updated;
}
