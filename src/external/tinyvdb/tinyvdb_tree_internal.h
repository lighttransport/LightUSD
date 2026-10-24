#pragma once
#include "tinyvdb_tree.h"

/* Validated topology, derived origins (serialized nodes may omit origins),
 * preorder and a full-int32 root lookup. All allocations are operation-local.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const tvdb_grid_t* grid;
  int levels;
  int64_t span[TVDB_MAX_TREE_DEPTH];
  int span_shift[TVDB_MAX_TREE_DEPTH];  // log2(span[l]) == trailing zero bits
  int32_t (*origins)[3];
  size_t *order, *root_map, root_capacity;
  size_t active_voxels, active_tiles;
  int has_bbox;
  int64_t bbox_min[3], bbox_max[3];
} tvdb_tree_index;
tvdb_status_t tvdb_tree_index_create(const tvdb_grid_t*, int float_only,
                                     tvdb_tree_index*, tvdb_error_t*);
void tvdb_tree_index_destroy(tvdb_tree_index*);
float tvdb_tree_get(const tvdb_tree_index*, const int32_t coord[3],
                    int* active);
size_t tvdb_tree_mask_rank(const tvdb_nodemask_t*, size_t bit);
tvdb_status_t tvdb_tree_error(tvdb_error_t*, tvdb_status_t, const char*);
int tvdb_tree_matrix(const tvdb_transform_t*, double matrix[4][4]);
int tvdb_tree_dense_geometry(const tvdb_transform_t*, float*, float origin[3]);
tvdb_status_t tvdb_tree_copy_attributes(const tvdb_grid_t*, tvdb_grid_t*,
                                        tvdb_error_t*);

#ifdef __cplusplus
}
#endif
