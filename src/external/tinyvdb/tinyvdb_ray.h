#pragma once

// Ray operations on dense grids: voxel DDA traversal, uniform sampling along
// rays, and SDF-segment extraction. Plus batched marching-cubes wrapper.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tinyvdb_mesh.h"   // tvdb_dense_grid, tvdb_vec3f, tvdb_triangle_mesh

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int x, y, z;
} tvdb_vec3i;

typedef struct {
  tvdb_vec3f origin;
  tvdb_vec3f dir;
  float tmin;
  float tmax;
} tvdb_ray;

// Enumerate voxel indices the ray traverses across the grid. If `out` is NULL,
// only counts; otherwise writes up to `cap` voxels and returns the number
// written (which may be less than the actual count if `cap` is exceeded).
// Every voxel whose interior the [tmin, tmax] segment passes through is
// reported, including the voxel containing the segment end point. Returns 0
// for invalid grids (non-positive dimensions or voxel size) and for rays with
// non-finite origin/direction or NaN tmin/tmax.
size_t tvdb_voxels_along_ray_dense(const tvdb_dense_grid* g,
                                   const tvdb_ray* ray,
                                   tvdb_vec3i* out_voxels,
                                   size_t cap);

// Generate `n_samples` uniformly spaced points along the ray within
// [tmin, tmax]. out_points and/or out_t may be NULL to skip that output.
void tvdb_uniform_ray_samples(const tvdb_ray* ray,
                              size_t n_samples,
                              tvdb_vec3f* out_points,
                              float* out_t);

// Find SDF-zero crossings along the ray and emit them as t-pair entries
// (entry, exit) for each contiguous "inside" run (sample f < isovalue).
// A sample exactly equal to the isovalue (or NaN) counts as outside; a
// crossing between samples of strictly opposite sign is linearly
// interpolated, otherwise it is placed at the sample lying on the isovalue.
// out_t_pairs is filled with up to `cap` pairs (2 floats each).
// Returns the number of pairs written (or required, if out is NULL).
size_t tvdb_segments_along_ray(const tvdb_dense_grid* g,
                               const tvdb_ray* ray,
                               float isovalue,
                               size_t step_count,
                               float* out_t_pairs,
                               size_t cap);

// Run marching cubes on each grid in the batch; meshes[i] receives the
// triangle mesh for grids[i]. Each output mesh must be initialized first
// (the function will populate vertices and faces). Uses the same arena for
// all grids (or NULL for malloc).
bool tvdb_marching_cubes_batch(const tvdb_dense_grid* grids,
                               size_t n_grids,
                               float isovalue,
                               tvdb_triangle_mesh* meshes,
                               tvdb_arena_allocator_t* arena);

#ifdef __cplusplus
}
#endif
