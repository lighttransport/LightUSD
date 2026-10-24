#pragma once
/* Volume access for renderers and USD (UsdVol) consumers: grid facts (class,
 * value type, transform, bounds) from a decoded grid or from a header-only
 * read (tvdb_read_grid_header), typed dense extraction with a voxel budget,
 * and nearest / trilinear sampling of the sparse tree in world or index space.
 *
 * Every value type the reader decodes is supported: bool and mask (0 / 1),
 * int32, int64, float (and half files, stored as float), double, vec3i,
 * vec3f and vec3d (three components). Values are converted to float. Stored
 * values are used as they are: inactive voxels and tiles keep their values,
 * and coordinates outside the tree read the background, as OpenVDB's tree
 * accessors do. Index coordinates name voxel centers (OpenVDB convention);
 * world = index_to_world * (i, j, k, 1). */
#include <stddef.h>
#include <stdint.h>

#include "tinyvdb_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TVDB_GRID_CLASS_UNKNOWN = 0,
  TVDB_GRID_CLASS_LEVEL_SET,  /* "level set" */
  TVDB_GRID_CLASS_FOG_VOLUME, /* "fog volume" */
  TVDB_GRID_CLASS_STAGGERED   /* "staggered" */
} tvdb_grid_class_t;

typedef struct {
  const char *name;              /* borrowed from the grid */
  tvdb_value_type_t value_type;  /* of the voxels (half files read as float) */
  int components;                /* 1, or 3 for vec3 types */
  int is_mask;                   /* OpenVDB MaskGrid */
  int saved_as_half;             /* stored as half in the file */
  tvdb_grid_class_t grid_class;  /* from the "class" metadata */
  int has_transform;             /* index_to_world is valid (linear map) */
  double index_to_world[4][4];
  int has_tree;                  /* tree decoded: bounds below are exact */
  int has_bounds;                /* any active voxel or tile (or file bbox) */
  int32_t index_min[3], index_max[3]; /* active index box, inclusive */
  double world_min[3], world_max[3];  /* the index box's voxel-corner box in world */
  uint64_t active_voxels;        /* active voxels, tiles counted by volume */
  float background[3];           /* background value (components used) */
} tvdb_grid_info_t;

/* Facts about a grid. Without a decoded tree (tvdb_read_grid_header), the
 * bounds and voxel count come from the file's "file_bbox_min/max" and
 * "file_voxel_count" metadata when present (has_tree == 0). */
tvdb_status_t tvdb_grid_get_info(const tvdb_grid_t *grid, tvdb_grid_info_t *out,
                                 tvdb_error_t *err);
tvdb_grid_class_t tvdb_grid_class_from_string(const char *text);
const char *tvdb_grid_class_string(tvdb_grid_class_t c); /* OpenVDB spelling */

/* ---- Dense extraction ---------------------------------------------------- */
typedef struct {
  int use_window;            /* else the active index box */
  int32_t window_min[3];     /* inclusive index box */
  int32_t window_max[3];
  size_t max_voxels;         /* output cell budget; 0: no limit */
  int stride;                /* index voxels per output cell per axis; 0: smallest fitting the budget */
  int inactive_as_background;/* inactive voxels and tiles read the background */
} tvdb_dense_volume_options_t;

typedef struct {
  int32_t dims[3];           /* output cells per axis */
  int32_t index_min[3];      /* index of output cell (0,0,0)'s first voxel */
  int stride;                /* index voxels per output cell per axis */
  int components;            /* 1 or 3 */
  float *data;               /* dims[0]*dims[1]*dims[2]*components, x fastest, interleaved */
  /* Output cell (u, v, w) center in world: cell_to_world * (u, v, w, 1). */
  double cell_to_world[4][4];
  float background[3];
} tvdb_dense_volume_t;

/* Cells average their stride^3 voxels (box filter; stride 1 copies). The
 * window is clipped to nothing and returns an empty volume when the grid has
 * no active voxels. Replaces out (initialized or zeroed) on success. */
tvdb_status_t tvdb_grid_to_dense_volume(const tvdb_grid_t *grid,
                                        const tvdb_dense_volume_options_t *options,
                                        tvdb_dense_volume_t *out, tvdb_error_t *err);
void tvdb_dense_volume_free(tvdb_dense_volume_t *volume);

/* ---- Sparse sampling ----------------------------------------------------- */
typedef struct tvdb_volume_sampler tvdb_volume_sampler_t;
typedef enum {
  TVDB_VOLUME_FILTER_NEAREST = 0,
  TVDB_VOLUME_FILTER_TRILINEAR = 1
} tvdb_volume_filter_t;

/* The grid must outlive the sampler and stay unchanged. Thread-safe reads. */
tvdb_status_t tvdb_volume_sampler_create(const tvdb_grid_t *grid,
                                         tvdb_volume_sampler_t **out, tvdb_error_t *err);
void tvdb_volume_sampler_destroy(tvdb_volume_sampler_t *sampler);
int tvdb_volume_sampler_components(const tvdb_volume_sampler_t *sampler);
/* `count` points (3 doubles each) to components floats each. World space
 * needs an invertible linear transform. */
tvdb_status_t tvdb_volume_sample_world(const tvdb_volume_sampler_t *sampler,
                                       const double *points, size_t count,
                                       tvdb_volume_filter_t filter, float *values,
                                       tvdb_error_t *err);
tvdb_status_t tvdb_volume_sample_index(const tvdb_volume_sampler_t *sampler,
                                       const double *points, size_t count,
                                       tvdb_volume_filter_t filter, float *values,
                                       tvdb_error_t *err);
/* One voxel's stored value and activity. */
void tvdb_volume_sampler_get(const tvdb_volume_sampler_t *sampler, const int32_t ijk[3],
                             float *value, int *active);

#ifdef __cplusplus
}
#endif
