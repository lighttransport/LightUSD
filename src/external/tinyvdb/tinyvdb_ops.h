#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "tinyvdb_mesh.h"
#include "tinyvdb_io.h"

// Dense outputs must be allocated with matching dimensions. Stencils and
// advection require positive finite input spacing; invalid input leaves outputs
// unchanged. Same-handle in-place stencils/advection use scratch storage and
// preserve the output on scratch allocation failure. Pointwise CSG/composites
// support exact in-place buffers without allocation.
// Dense vector grid (3-component)
typedef struct {
  int nx, ny, nz;
  float ox, oy, oz;
  float voxel_size;
  float* data; // nx * ny * nz * 3, interleaved
} tvdb_dense_vec_grid;

// Phase 1: Morphology
void tvdb_dilate(tvdb_dense_grid* grid, int iterations);
void tvdb_erode(tvdb_dense_grid* grid, int iterations);
void tvdb_open(tvdb_dense_grid* grid, int iterations);
void tvdb_close(tvdb_dense_grid* grid, int iterations);

// Phase 1: Filtering
void tvdb_gaussian_filter(tvdb_dense_grid* grid, int width, int iterations);
void tvdb_mean_filter(tvdb_dense_grid* grid, int width, int iterations);
void tvdb_laplacian_filter(tvdb_dense_grid* grid, int iterations);

// Phase 1: CSG
void tvdb_csg_union(const tvdb_dense_grid* a, const tvdb_dense_grid* b, tvdb_dense_grid* result);
void tvdb_csg_intersection(const tvdb_dense_grid* a, const tvdb_dense_grid* b, tvdb_dense_grid* result);
void tvdb_csg_difference(const tvdb_dense_grid* a, const tvdb_dense_grid* b, tvdb_dense_grid* result);

// Phase 1: Measurement
float tvdb_surface_area(const tvdb_dense_grid* grid);
float tvdb_volume(const tvdb_dense_grid* grid);

// Phase 2: Differential operators
void tvdb_gradient(const tvdb_dense_grid* scalar, tvdb_dense_vec_grid* grad);
void tvdb_divergence(const tvdb_dense_vec_grid* vec, tvdb_dense_grid* div);
void tvdb_laplacian(const tvdb_dense_grid* scalar, tvdb_dense_grid* laplacian);
void tvdb_curl(const tvdb_dense_vec_grid* vec, tvdb_dense_vec_grid* curl);

// Phase 2: Finite differences
float tvdb_central_diff_x(const tvdb_dense_grid* g, int ix, int iy, int iz);
float tvdb_central_diff_y(const tvdb_dense_grid* g, int ix, int iy, int iz);
float tvdb_central_diff_z(const tvdb_dense_grid* g, int ix, int iy, int iz);

// Vector-grid operators (parallels OpenVDB GridOperators). `out` must be
// pre-allocated with the same dims as the input.
//   magnitude:     per-voxel |v| of a vector grid -> scalar grid.
//   normalize_vec: per-voxel v/|v| (zero where |v| == 0) -> vector grid.
//   cpt:           closest-point transform of an SDF -> vector grid of world
//                  positions, cpt(p) = p - sdf(p) * grad(sdf)(p).
void tvdb_magnitude(const tvdb_dense_vec_grid* vec, tvdb_dense_grid* out);
void tvdb_normalize_vec(const tvdb_dense_vec_grid* vec, tvdb_dense_vec_grid* out);
void tvdb_cpt(const tvdb_dense_grid* sdf, tvdb_dense_vec_grid* out);

// Per-voxel composite of two same-shape grids (parallels OpenVDB Composite).
// `result` is pre-allocated and may alias `a` or `b`.
void tvdb_comp_max(const tvdb_dense_grid* a, const tvdb_dense_grid* b, tvdb_dense_grid* result);
void tvdb_comp_min(const tvdb_dense_grid* a, const tvdb_dense_grid* b, tvdb_dense_grid* result);
void tvdb_comp_sum(const tvdb_dense_grid* a, const tvdb_dense_grid* b, tvdb_dense_grid* result);
void tvdb_comp_mult(const tvdb_dense_grid* a, const tvdb_dense_grid* b, tvdb_dense_grid* result);

// In-place filters (parallels OpenVDB Filter / LevelSetFilter).
//   median_filter:        (2*radius+1)^3 window median, `iterations` passes.
//   mean_curvature_flow:  level-set smoothing phi += dt*|grad phi|*kappa via
//                         explicit Euler (kappa = mean curvature); keep dt small.
void tvdb_median_filter(tvdb_dense_grid* grid, int radius, int iterations);
void tvdb_mean_curvature_flow(tvdb_dense_grid* grid, float dt, int iterations);

// Signed flood fill (parallels OpenVDB SignedFloodFill). Given a level set whose
// narrow band (|value| < band_world) carries correct signed distances but whose
// far voxels may have the wrong sign, set every far voxel (|value| >=
// band_world) to +band_world if it connects to the grid boundary through far
// voxels (exterior) or -band_world otherwise (interior). In-place; the band is
// left untouched. Use to restore interior signs after an unsigned operation.
void tvdb_signed_flood_fill(tvdb_dense_grid* grid, float band_world);

// Phase 2: Advection
void tvdb_advect_semi_lagrangian(const tvdb_dense_grid* field,
                                 const tvdb_dense_vec_grid* velocity,
                                 float dt,
                                 tvdb_dense_grid* result);

// Higher-order advection (parallels OpenVDB VolumeAdvect). Advect `field`
// through a steady `velocity` field by `dt` using `scheme`:
//   0 RK1, 1 RK2, 2 RK3, 3 RK4  — semi-Lagrangian backtrace of that order;
//   4 MacCormack, 5 BFECC       — second-order error-compensated schemes.
// `clamp` (0/1) limits the MacCormack/BFECC correction to the trilinear-stencil
// value range at the backtrace point, suppressing overshoot (recommended; the
// RK schemes ignore it). `result` is pre-allocated, same shape as `field`.
// `result` may alias `field` or `velocity`; if any scratch allocation fails,
// `result` is left unchanged.
typedef enum {
  TVDB_ADVECT_RK1 = 0,
  TVDB_ADVECT_RK2 = 1,
  TVDB_ADVECT_RK3 = 2,
  TVDB_ADVECT_RK4 = 3,
  TVDB_ADVECT_MACCORMACK = 4,
  TVDB_ADVECT_BFECC = 5
} tvdb_advect_scheme_t;

void tvdb_advect(const tvdb_dense_grid* field, const tvdb_dense_vec_grid* velocity,
                 float dt, int scheme, int clamp, tvdb_dense_grid* result);

// Phase 2: Poisson solver
int tvdb_solve_poisson(const tvdb_dense_grid* rhs,
                       tvdb_dense_grid* x,
                       int max_iters,
                       float tolerance);

// Higher-precision Poisson solve. Identical interface to tvdb_solve_poisson
// but performs all CG iterates and the 7-point Laplacian apply in fp64
// internally (input/output remain fp32). Use when the fp32 path stalls or
// for grids large enough that fp32 rounding limits convergence (≥ 64^3).
// Returns iterations used.
int tvdb_solve_poisson_d(const tvdb_dense_grid* rhs,
                         tvdb_dense_grid* x,
                         int max_iters,
                         double tolerance);

// Legacy Poisson entry points delegate to these checked solves and return zero
// on failure; use _ex to distinguish failure from an already-solved input.
// Checked Poisson solves use edge-clamped boundaries, preserve the initial
// solution mean, and reject an incompatible RHS (nonzero mean beyond rounding).
// Invalid input, OOM, and numerical breakdown leave x unchanged. Budget
// exhaustion returns TVDB_OK with converged=false and the best finite iterate.
// _ex tolerance is relative for fp32 / fp64-internal, absolute for fp64 storage.
// Reports refer to the projected RHS and the actual returned storage precision.
typedef struct {
  int iterations;
  bool converged;
  double initial_residual_norm;
  double final_residual_norm;
} tvdb_poisson_result_t;
tvdb_status_t tvdb_solve_poisson_ex(const tvdb_dense_grid* rhs, tvdb_dense_grid* x,
  int max_iters, float tolerance, tvdb_poisson_result_t* result, tvdb_error_t* err);
tvdb_status_t tvdb_solve_poisson_d_ex(const tvdb_dense_grid* rhs, tvdb_dense_grid* x,
  int max_iters, double tolerance, tvdb_poisson_result_t* result, tvdb_error_t* err);

// Fast Sweeping: solve the Eikonal equation |∇φ|=1 on a dense grid to
// redistance an SDF away from its zero-crossing band. Voxels with
// |grid->data[i]| <= frozen_band are treated as boundary conditions (kept
// fixed); all other voxels are recomputed from the upwind 3D quadratic
// (Zhao 2005) using `voxel_size` as the metric. The solver runs eight
// directional sweeps per iteration and stops when no update exceeds
// `tol` (in world units) or after `max_iters` iterations. Sign of each
// non-frozen voxel is preserved from its initial value (positive vs negative
// half-space) so an outside-positive / inside-negative SDF is reproduced.
// Returns the number of iterations executed.
int tvdb_fast_sweeping(tvdb_dense_grid* grid, float frozen_band,
                       int max_iters, float tol);

// Memory management
void tvdb_dense_vec_grid_init(tvdb_dense_vec_grid* grid, int nx, int ny, int nz);
void tvdb_dense_vec_grid_free(tvdb_dense_vec_grid* grid);

// =============================================================================
// fp64 dense grid (`tvdb_dense_grid_d`). Mirrors `tvdb_dense_grid` but uses
// double for storage and world-space metrics. A subset of ops is provided
// where fp64 accuracy is most valuable (Poisson, Eikonal, large-grid CSG,
// volume/surface integrals). For ops not in this subset, convert to fp32
// via `tvdb_dense_grid_d_to_f` and use the fp32 path.
// =============================================================================

typedef struct {
  int nx, ny, nz;
  double ox, oy, oz;
  double voxel_size;
  double* data;
} tvdb_dense_grid_d;

void tvdb_dense_grid_d_init(tvdb_dense_grid_d* grid, int nx, int ny, int nz);
void tvdb_dense_grid_d_free(tvdb_dense_grid_d* grid);

// fp32 <-> fp64 deep-copy converters. `out` must be uninitialized; on
// return it owns its data and the caller frees with the matching free fn.
// Voxel-size and origin are widened/narrowed accordingly.
void tvdb_dense_grid_f_to_d(const tvdb_dense_grid* in, tvdb_dense_grid_d* out);
void tvdb_dense_grid_d_to_f(const tvdb_dense_grid_d* in, tvdb_dense_grid* out);

// Sampling
double tvdb_sample_trilinear_dense_d(const tvdb_dense_grid_d* g,
                                     double wx, double wy, double wz);

// Differential
void tvdb_laplacian_d(const tvdb_dense_grid_d* scalar, tvdb_dense_grid_d* laplacian);

// CSG
void tvdb_csg_union_d(const tvdb_dense_grid_d* a, const tvdb_dense_grid_d* b, tvdb_dense_grid_d* result);
void tvdb_csg_intersection_d(const tvdb_dense_grid_d* a, const tvdb_dense_grid_d* b, tvdb_dense_grid_d* result);
void tvdb_csg_difference_d(const tvdb_dense_grid_d* a, const tvdb_dense_grid_d* b, tvdb_dense_grid_d* result);

// Measurement (zero-crossing 6-edge area; volume of `value < 0` half-space).
// Like the fp32 versions, return 0 for an invalid grid, including a
// non-positive or non-finite voxel_size.
double tvdb_surface_area_d(const tvdb_dense_grid_d* grid);
double tvdb_volume_d(const tvdb_dense_grid_d* grid);

// FastSweeping (full fp64 path)
int tvdb_fast_sweeping_d(tvdb_dense_grid_d* grid, double frozen_band,
                         int max_iters, double tol);

// Poisson solver: fp64 input/output, fp64 internals.
// (Distinct from `tvdb_solve_poisson_d` which takes fp32 in/out with fp64
// internals.) Returns iterations used.
int tvdb_solve_poisson_dd(const tvdb_dense_grid_d* rhs,
                          tvdb_dense_grid_d* x,
                          int max_iters,
                          double tolerance);

tvdb_status_t tvdb_solve_poisson_dd_ex(const tvdb_dense_grid_d* rhs, tvdb_dense_grid_d* x,
  int max_iters, double tolerance, tvdb_poisson_result_t* result, tvdb_error_t* err);

#ifdef __cplusplus
}
#endif
