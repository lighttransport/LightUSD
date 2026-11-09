#pragma once

// Lightweight sparse representation: parallel arrays of (i,j,k) integer
// coordinates and float values. Compact, GPU-friendly layout that mirrors
// the fvdb "active_grid_coords + JaggedTensor" model on CPU. Works alongside
// the existing dense grid type and provides materializers between the two.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "tinyvdb_mesh.h"   // tvdb_dense_grid
#include "tinyvdb_ray.h"    // tvdb_vec3i
#include "tvdb_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  tvdb_vec3i* coords;     // length `count`
  float* values;          // length `count`
  size_t count;
  size_t capacity;
  // Common origin / voxel_size shared by all active voxels.
  float voxel_size;
  float ox, oy, oz;
} tvdb_sparse_grid;

// Outputs for materializers, CSG, morphology, and convolution must be initialized
// owning containers (init/reserve), including empty containers. Same-handle
// in-place output is supported; distinct containers may not share backing arrays.
// Failure leaves the prior output unchanged. Full int32 coordinates are compared
// for equality; duplicate lookups use the first input occurrence.
// Lifecycle.
void tvdb_sparse_grid_init(tvdb_sparse_grid* sg);
void tvdb_sparse_grid_free(tvdb_sparse_grid* sg);

// Reserve capacity (grows arrays); ignored if already large enough.
bool tvdb_sparse_grid_reserve(tvdb_sparse_grid* sg, size_t capacity);

// Materialization: dense -> sparse, keeping voxels where |value - background|
// > tolerance. Result voxels are stored in scan order.
bool tvdb_dense_to_sparse(const tvdb_dense_grid* dense,
                          float background,
                          float tolerance,
                          tvdb_sparse_grid* out);

// Materialization: sparse -> dense. Caller provides target dimensions and
// origin via the (already-allocated) `out` grid. Inactive voxels are filled
// with `background`.
bool tvdb_sparse_to_dense(const tvdb_sparse_grid* sparse,
                          float background,
                          tvdb_dense_grid* out);

// Active-voxel enumeration. Same as dense_to_sparse with tolerance=0.
bool tvdb_active_grid_coords(const tvdb_dense_grid* dense,
                             float background,
                             tvdb_sparse_grid* out);

// Sparse SDF CSG (input grids must share origin/voxel_size). Output spans
// the union of input coordinate sets.
bool tvdb_csg_union_sparse(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
                           float background, tvdb_sparse_grid* out);
bool tvdb_csg_intersection_sparse(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
                                  float background, tvdb_sparse_grid* out);
bool tvdb_csg_difference_sparse(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
                                float background, tvdb_sparse_grid* out);

// Sparse morphology by single-voxel radius. dilate adds 6-connected
// neighbors of each active voxel (taking the min over (self, neighbors));
// erode removes voxels whose 6-neighborhood is not fully active.
// `background` is used for missing-neighbor lookups in dilate.
bool tvdb_dilate_sparse(const tvdb_sparse_grid* in,
                        float background, int iterations,
                        tvdb_sparse_grid* out);
bool tvdb_erode_sparse(const tvdb_sparse_grid* in, int iterations,
                       tvdb_sparse_grid* out);

// 3D convolution on a sparse grid. Output topology equals input topology
// (same-topology mode). Kernel is dense kx*ky*kz floats laid out as
// kernel[((kz_idx) * ky + ky_idx) * kx + kx_idx]. Anchor is at
// (kx/2, ky/2, kz/2) (matches numpy/scipy convention for both odd and
// even kernel sizes). Out-of-active-set taps contribute `pad_value`.
//
// out->coords is a copy of in->coords. out->values[i] =
//   sum over (di, dj, dk) of  K[di,dj,dk] * V(coord_i + (di-ax, dj-ay, dk-az))
// where V() returns pad_value for missing input.
bool tvdb_sparse_conv3d(const tvdb_sparse_grid* in,
                        const float* kernel,
                        int kx, int ky, int kz,
                        float pad_value,
                        tvdb_sparse_grid* out);

// Multi-channel 3D convolution. Input has c_in channels per voxel; output
// has c_out channels per voxel. Values arrays are interleaved per voxel
// (length count * c_{in,out}, channel-fastest layout).
//
// Kernel layout: kernel[((((dk * ky) + dj) * kx + di) * c_out + co) * c_in + ci]
// — i.e. for each spatial offset (di, dj, dk), a c_out × c_in matrix
// stored row-major (output channel outer, input channel inner).
//
// Output topology = input topology. Out-of-active-set taps contribute
// pad_value for every channel.
//
// `in_values` is in->count * c_in floats; `out_values` is allocated and
// returned via *out_values_out (caller frees with free()) of length
// out->count * c_out.
bool tvdb_sparse_conv3d_mc(const tvdb_sparse_grid* in,
                           const float* in_values, int c_in,
                           const float* kernel, int kx, int ky, int kz,
                           int c_out,
                           float pad_value,
                           tvdb_sparse_grid* out,
                           float** out_values_mc);

#ifdef __cplusplus
}
#endif
