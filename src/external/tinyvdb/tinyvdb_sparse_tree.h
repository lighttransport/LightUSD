#pragma once

// Bridge between the OpenVDB-style sparse tree loaded by tinyvdb_io and the
// dense / sparse op surface in this library. Lets you:
//   - visit every active leaf of a loaded grid without materializing
//   - count active voxels
//   - extract the active voxels + values into a flat tvdb_sparse_grid
//   - materialize a sub-region into a tvdb_dense_grid for use with the dense ops
//
// Currently float-typed leaves only (TVDB_VALUE_FLOAT). Other value types are
// silently skipped by visit_leaves_float — caller can check the layout.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyvdb_io.h"
#include "tinyvdb_mesh.h"   // tvdb_dense_grid
#include "tinyvdb_sparse.h" // tvdb_sparse_grid

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t origin[3];                  // world voxel coord of leaf's (0,0,0) corner
    int32_t log2dim;                    // leaf is dim^3 voxels with dim = 1<<log2dim
    const float *data;                  // dim^3 floats; OpenVDB layout (i,j,k)->((i<<2L)|(j<<L)|k)
    const tvdb_nodemask_t *value_mask;  // active mask, size dim^3 bits
} tvdb_leaf_view_t;

// Per-leaf callback. Return non-zero to stop iteration early.
typedef int (*tvdb_leaf_visit_fn)(const tvdb_leaf_view_t *leaf, void *user);

// Visit every float-typed leaf in the grid's tree. Returns the number of
// leaves visited.
size_t tvdb_grid_visit_leaves_float(const tvdb_grid_t *grid,
                                    tvdb_leaf_visit_fn cb, void *user);

// Visit every leaf regardless of value type. `origin`, `log2dim` and
// `value_mask` in the leaf view are valid for all types; `data` points at the
// leaf's raw byte buffer (typed as const float* in the view for ABI reasons —
// reinterpret it using the grid's leaf value_type and tvdb_value_type_size).
// Returns the number of leaves visited.
size_t tvdb_grid_visit_leaves(const tvdb_grid_t *grid,
                              tvdb_leaf_visit_fn cb, void *user);

// Count active voxels, including the represented volume of active tiles.
// Invalid trees or counts that overflow size_t return zero.
size_t tvdb_grid_active_voxel_count(const tvdb_grid_t *grid);

// Exact voxel-index AABB of active voxels and tiles, with exclusive maximum.
// Returns false for an empty/invalid tree or an unrepresentable int32 maximum;
// output bounds are preserved on failure.
bool tvdb_grid_active_bbox(const tvdb_grid_t *grid,
                           int32_t out_min[3], int32_t out_max[3]);

// Extract active voxels into a flat (coords[], values[]) sparse_grid. The
// sparse grid's coords are in OpenVDB world-voxel-index space; voxel_size and
// origin are taken from `grid`'s transform for downstream world-space ops.
// Only positive axis-aligned uniform scale is representable. Active tiles
// require explicit expansion and are rejected. Output must be initialized and
// owning; failure preserves it. Use tinyvdb_tree.h _ex APIs for error details.
bool tvdb_grid_to_sparse(const tvdb_grid_t *grid, tvdb_sparse_grid *out);

// Materialize a voxel-index AABB of the grid into a dense grid. bbox_max is
// exclusive (i.e. dims = max - min). Inactive cells get `background`.
// Output's voxel_size and origin are set so that voxel (0,0,0) of the dense
// grid corresponds to bbox_min in world voxel space.
bool tvdb_grid_materialize_dense(const tvdb_grid_t *grid,
                                 const int32_t bbox_min[3],
                                 const int32_t bbox_max[3],
                                 float background,
                                 tvdb_dense_grid *out);

// Read the float background value (root tile fill). Returns 0 for non-float grids.
float tvdb_grid_float_background(const tvdb_grid_t *grid);

// Topology-preserving voxel-value update. For each (coord, value) in `sg`,
// finds the leaf containing the coordinate and writes the value (also
// marking the value_mask bit). Coordinates that fall outside any active
// leaf are skipped; if `out_skipped` is non-NULL, the count of skipped
// coords is written. Returns the number of voxels successfully updated.
//
// This pairs naturally with topology-preserving ops (dilate_active /
// erode_active / leaf-stamp filters): run the op, update_from_sparse,
// then save the file with tvdb_file_save.
size_t tvdb_grid_update_from_sparse(tvdb_grid_t *grid,
                                    const tvdb_sparse_grid *sg,
                                    size_t *out_skipped);

// Build a fresh tvdb_grid_t from a tvdb_sparse_grid using a template
// grid's descriptor / transform / tree layout. The output owns all its
// memory (allocated via malloc); free with tvdb_grid_destroy_owned.
//
// `tmpl` provides: descriptor.grid_type (e.g. "Tree_float_5_4_3"),
// transform, and tree.layout (level structure + log2dims).
//
// `grid_name` becomes the new grid's name (descriptor.grid_name).
//
// `background` is the float background fill for inactive voxels and
// internal-node tiles. The voxel size and origin in `sg` are not used —
// the template's transform is preserved.
//
// Duplicate coordinates are allowed; the first occurrence in `sg` wins (this
// also applies to the typed and vec3 variants below).
//
// Currently supports only Tree_float_5_4_3 (two internal levels above leaf,
// float value type). Returns false otherwise.
//
// Pairs with tvdb_file_save: after building, append the grid to a
// tvdb_file_t and call tvdb_file_save.
bool tvdb_grid_from_sparse_using_template(const tvdb_grid_t *tmpl,
                                          const tvdb_sparse_grid *sg,
                                          const char *grid_name,
                                          float background,
                                          tvdb_grid_t *out);

// Generic typed-value builder. `value_type` selects element size and the
// expected leaf-level type on the template; `values` is a packed array of
// `count` elements, each `tvdb_value_type_size(value_type)` bytes wide.
// `bg_bytes` is one element's worth of background fill (must not be NULL).
// Currently supports 4-level layouts (`num_levels == 4`, the standard
// Tree_*_5_4_3 hierarchy) only.
bool tvdb_grid_from_sparse_typed_using_template(const tvdb_grid_t *tmpl,
                                                const tvdb_vec3i *coords,
                                                const void *values,
                                                size_t count,
                                                tvdb_value_type_t value_type,
                                                const void *bg_bytes,
                                                const char *grid_name,
                                                tvdb_grid_t *out);

// Vec3 (Tree_vec3s_5_4_3) variant. `coords` and `values` describe `count`
// active voxels; `values` is `count*3` floats laid out (vx,vy,vz) per voxel.
// Template grid must be a vec3 grid (leaf level value_type == VEC3F);
// otherwise returns false. `background` is 3 floats (may be NULL → zero).
bool tvdb_grid_from_sparse_vec3_using_template(const tvdb_grid_t *tmpl,
                                               const tvdb_vec3i *coords,
                                               const float *values,
                                               size_t count,
                                               const char *grid_name,
                                               const float background[3],
                                               tvdb_grid_t *out);

// Topology-extending merge: rebuild grid as `existing` ∪ `sg` (sg wins on
// overlap, new leaves are created where needed). Among duplicate coordinates
// inside `sg`, the first occurrence wins. Output ownership matches
// tvdb_grid_from_sparse_using_template. Float layouts only.
bool tvdb_grid_extend_from_sparse(const tvdb_grid_t *existing,
                                  const tvdb_sparse_grid *sg,
                                  const char *grid_name,
                                  float background,
                                  tvdb_grid_t *out);

// Free a grid produced by tvdb_grid_from_sparse_using_template.
void tvdb_grid_destroy_owned(tvdb_grid_t *grid);

// Tree dilate / erode. Computes seven-point min/max using a compact active
// workspace; inactive voxels, signed tiles and background stay fixed during
// every iteration. No dense intermediate. Output is a flat sparse grid
// containing all originally-active voxels with updated values
// (topology-preserving). Zero iterations extracts unchanged active values.
// Positive axis-aligned uniform scale is required; active tiles are rejected.
// Output must be initialized and owning. Failure preserves source and output.
// For topology-growing dilation use tvdb_grid_dilate_topology.
bool tvdb_grid_dilate_active(const tvdb_grid_t *grid, int iterations,
                             tvdb_sparse_grid *out);
bool tvdb_grid_erode_active(const tvdb_grid_t *grid, int iterations,
                            tvdb_sparse_grid *out);

// Topology-growing dilate: extracts active voxels into a sparse grid, then
// expands the active set by `iterations` voxels (6-connected) using the
// tree's float background as the fill value for missing neighbors. Output
// `count` is generally larger than the input active count.
bool tvdb_grid_dilate_topology(const tvdb_grid_t *grid, int iterations,
                               tvdb_sparse_grid *out);

// Topology-shrinking erode: voxels whose 6-neighborhood is not fully active
// are dropped. Output `count` is generally smaller than the input active count.
bool tvdb_grid_erode_topology(const tvdb_grid_t *grid, int iterations,
                              tvdb_sparse_grid *out);

// Tree-aware CSG requires identical complete transforms and flat-compatible
// uniform scale. All three operations use the union of active voxel sets,
// sampling each input's own stored inactive leaf/tile/background values where
// that input is inactive. Active tiles are rejected; failure preserves output.
bool tvdb_grid_csg_union(const tvdb_grid_t *a, const tvdb_grid_t *b,
                         tvdb_sparse_grid *out);
bool tvdb_grid_csg_intersection(const tvdb_grid_t *a, const tvdb_grid_t *b,
                                tvdb_sparse_grid *out);
bool tvdb_grid_csg_difference(const tvdb_grid_t *a, const tvdb_grid_t *b,
                              tvdb_sparse_grid *out);

#ifdef __cplusplus
}
#endif
