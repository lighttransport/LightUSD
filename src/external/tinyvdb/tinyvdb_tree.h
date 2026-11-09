#pragma once

/* Checked maintenance of loaded float trees. No bounding-box volume is
 * materialized. Constructors require distinct initialized owning outputs
 * ({0} is valid); failure preserves both source and output. Successful
 * outputs own libc allocations; release with tvdb_grid_destroy_owned(). */
#include "tinyvdb_sparse_tree.h"
#include "tinyvdb_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TVDB_TREE_DIAG_GENERIC = 0,
  TVDB_TREE_DIAG_LEVEL_SET,
  TVDB_TREE_DIAG_FOG
} tvdb_tree_diagnostic_kind_t;
enum {
  TVDB_TREE_DIAG_NONFINITE = 1u,
  TVDB_TREE_DIAG_TRANSFORM = 2u,
  TVDB_TREE_DIAG_BACKGROUND = 4u,
  TVDB_TREE_DIAG_ACTIVE_TILES = 8u,
  TVDB_TREE_DIAG_RANGE = 16u,
  TVDB_TREE_DIAG_INACTIVE = 32u,
  TVDB_TREE_DIAG_GRADIENT = 64u,
  TVDB_TREE_DIAG_CLASS = 128u
};
typedef struct {
  int valid;
  unsigned flags;
  size_t active_voxels, active_tiles, nonfinite_values;
  size_t first_bad_node; /* SIZE_MAX when no issue was found. */
  int64_t first_bad_coord[3];
  int has_active_bbox;
  int64_t active_min[3], active_max[3]; /* Exact, exclusive maximum. */
  tvdb_level_set_check_t gradient;
} tvdb_tree_diagnostics_t;

/* Structural/type/allocation errors return status and preserve *out. Value
 * defects instead produce TVDB_OK with valid=0 and the corresponding flags.
 * Level-set diagnostics expect symmetric bands, uniform scale and string
 * metadata with key "class" and value "level set"; fog expects "fog volume".
 * Generic checks do not require class metadata. tolerance is finite and
 * nonnegative. */
tvdb_status_t tvdb_grid_diagnose(const tvdb_grid_t* grid,
                                 tvdb_tree_diagnostic_kind_t kind,
                                 int check_gradient, double tolerance,
                                 tvdb_tree_diagnostics_t* out,
                                 tvdb_error_t* err);

/* Closed narrow-band level sets only; this is sign propagation, not a
 * watertightness test or distance reconstruction. Widths are world distances:
 * outside_width > 0, inside_width < 0. Active tiles and nonfinite stored
 * values are rejected. All active values and masks are preserved. */
tvdb_status_t tvdb_grid_signed_flood_fill(const tvdb_grid_t* grid,
                                          float outside_width,
                                          float inside_width, tvdb_grid_t* out,
                                          tvdb_error_t* err);

/* Exact pruning: collapse a subtree only if every represented value has the
 * same float bit pattern and active state. Mixed signs/states never collapse.
 * Active tiles may be created; flat extraction rejects their expansion. */
tvdb_status_t tvdb_grid_prune(const tvdb_grid_t* grid, tvdb_grid_t* out,
                              tvdb_error_t* err);

/* Checked bridge variants. Unlike the legacy dense constructor, *out must
 * be initialized and owning; success replaces its prior allocation. */
tvdb_status_t tvdb_grid_to_sparse_ex(const tvdb_grid_t* grid,
                                     tvdb_sparse_grid* out, tvdb_error_t* err);
tvdb_status_t tvdb_grid_materialize_dense_ex(const tvdb_grid_t* grid,
                                             const int32_t bbox_min[3],
                                             const int32_t bbox_max[3],
                                             float background,
                                             tvdb_dense_grid* out,
                                             tvdb_error_t* err);

#ifdef __cplusplus
}
#endif
