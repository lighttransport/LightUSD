#pragma once

// Grid statistics and diagnostics (parallels OpenVDB Statistics / Histogram /
// Diagnostics). All operate on the dense grid representation (tvdb_dense_grid)
// over every voxel. Grids with non-positive dimensions, a NULL data pointer, or
// a voxel count whose float storage size overflows size_t are rejected.

#include <stdbool.h>
#include <stddef.h>
#include "tinyvdb_mesh.h"  // tvdb_dense_grid

#ifdef __cplusplus
extern "C" {
#endif

// Per-voxel value statistics over the whole grid.
typedef struct {
  double min, max, mean, stddev, sum;
  size_t count;
} tvdb_grid_stats_t;

// Compute min/max/mean/stddev/sum over all voxels. Returns false on a
// NULL/empty grid (out is zeroed in that case).
bool tvdb_grid_statistics(const tvdb_dense_grid* grid, tvdb_grid_stats_t* out);

// Histogram of voxel values into `nbins` uniform bins spanning
// [range_min, range_max]; values below/above (including +-infinity) clamp into
// the first/last bin, and NaN voxels are not counted. The range must be finite
// with range_max > range_min. `out_counts` must hold `nbins` entries. Returns
// false on bad args (out_counts is then left unchanged).
bool tvdb_grid_histogram(const tvdb_dense_grid* grid, double range_min,
                         double range_max, int nbins, size_t* out_counts);

// Level-set health check (parallels OpenVDB Diagnostics::checkLevelSet): a valid
// narrow-band SDF has |grad| == 1. Over interior (non-boundary) voxels with
// |value| <= band_world (band_world <= 0 = all interior voxels), measures the
// central-difference gradient magnitude. The grid's voxel size must be
// positive and finite.
typedef struct {
  double mean_grad_mag;    // average |grad| over the band
  double max_grad_error;   // max | |grad| - 1 |
  double bad_fraction;     // fraction of band voxels with | |grad|-1 | > tol
  size_t band_count;       // number of band voxels evaluated
} tvdb_level_set_check_t;

bool tvdb_check_level_set(const tvdb_dense_grid* grid, double band_world,
                          double tol, tvdb_level_set_check_t* out);

// Fog-volume check (parallels OpenVDB Diagnostics::checkFogVolume): a valid fog
// volume has all values in [0, 1]. Reports the value range and whether every
// voxel lies within [0-eps, 1+eps]. Any non-finite voxel makes the volume
// invalid; NaN voxels are excluded from the reported min/max (which are NaN
// only if every voxel is NaN). Returns false on a NULL/empty grid.
bool tvdb_check_fog_volume(const tvdb_dense_grid* grid, double eps,
                           int* out_valid, double* out_min, double* out_max);

#ifdef __cplusplus
}
#endif
