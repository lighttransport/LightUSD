#pragma once
#include "tinyvdb_ops.h"
#include "tinyvdb_tree.h"
#include "tinyvdb_thread.h"
#ifdef __cplusplus
extern "C" {
#endif

/* All constructors require distinct initialized owning outputs, use libc,
 * and preserve source/output/report on failure. Float trees only. Budgets of
 * zero select 1,000,000 coordinates/cells; budget exhaustion is an error.
 * Index samples are at integer coordinates mapped by the complete transform.
 * No dense bounding-box array is allocated. */
typedef enum {
  TVDB_SAMPLE_NEAREST = 0,
  TVDB_SAMPLE_LINEAR = 1
} tvdb_tree_sampler_t;
/* Scalar values (including stored inactive values) are sampled in world space.
 * An output sample is active if any nonzero-weight input tap is active.
 * Nonbackground inactive values are retained and exactly pruned. Output uses
 * the standard float 5/4/3 hierarchy. Level sets are not redistanced here. */
tvdb_status_t tvdb_grid_resample(const tvdb_grid_t* source,
                                 const tvdb_transform_t* target,
                                 tvdb_tree_sampler_t sampler, size_t max_voxels,
                                 tvdb_grid_t* out, tvdb_error_t* err);

/* Indexed classic MC over stored negative regions and their boundaries,
 * including inactive tiles; active state does not clip the surface. Replaces
 * an owning mesh, with outward winding for values below isovalue. All affine
 * transforms, including reflections, are supported. adaptivity is a maximum
 * world-space vertex displacement (0 disables clustering); shared global
 * clusters remove faces with repeated indices. Classic ambiguities/clustering
 * do not guarantee manifoldness or preserve topology. max_cells also bounds
 * boundary scan work (32 times this budget), so huge tile faces can fail
 * explicitly. */
tvdb_status_t tvdb_grid_volume_to_mesh(const tvdb_grid_t* source,
                                       float isovalue, double adaptivity,
                                       size_t max_cells,
                                       tvdb_triangle_mesh* out,
                                       tvdb_error_t* err);

typedef struct {
  size_t surface_cells, candidate_voxels, active_voxels;
  double max_distance;
} tvdb_level_set_rebuild_result_t;
/* Reconstruct the piecewise-linear isosurface, then closest-triangle world
 * distances using a BVH. Expands/trims topology and fills inactive signs.
 * Closed, resolved narrow-band inputs with positive exterior background only;
 * affine columns must be orthogonal and have equal lengths. Widths are positive
 * world distances, inside_width is a positive magnitude. This geometric
 * rebuild has no iterative convergence budget and cannot repair unresolved or
 * open surfaces. Tracking is a rebuild after edits; it does not advect a grid.
 * Active values satisfy -inside_width < value < outside_width. */
tvdb_status_t tvdb_grid_level_set_rebuild(
    const tvdb_grid_t* source, float isovalue, float outside_width,
    float inside_width, size_t max_voxels, tvdb_grid_t* out,
    tvdb_level_set_rebuild_result_t* result, tvdb_error_t* err);
typedef enum {
  TVDB_LEVEL_SET_SIGN_WINDING_FAST = 0, /* fast winding number (default) */
  TVDB_LEVEL_SET_SIGN_WINDING_EXACT = 1 /* exact winding number, O(faces) */
} tvdb_level_set_sign_t;
/* Batch replacement for tvdb_mesh_to_level_set's per-voxel pass: for `count`
 * voxels (3 index coordinates each; world position index * voxel_size),
 * write the closest-face distance and the generalized winding number. A NaN
 * distance hands that voxel back to the CPU. The mesh is the one passed to
 * tvdb_mesh_to_level_set. tvdb_gpu_mesh_to_level_set installs a CUDA/HIP one. */
typedef tvdb_status_t (*tvdb_mesh_level_set_eval_fn)(void* user, const int32_t* ijk,
                                                     size_t count, double voxel_size,
                                                     float* distance, double* winding,
                                                     tvdb_error_t* err);
typedef struct {
  float voxel_size;      /* world units; index i has its center at i * size */
  float exterior_width;  /* positive world distance kept outside */
  float interior_width;  /* positive world distance kept inside */
  tvdb_level_set_sign_t sign;
  size_t max_voxels;     /* candidate budget; zero selects 1,000,000 */
  const char* grid_name; /* NULL selects "surface" */
  tvdb_mesh_level_set_eval_fn evaluate; /* NULL: CPU distances and windings */
  void* evaluate_user;
} tvdb_mesh_level_set_options_t;
typedef struct {
  size_t seed_voxels, candidate_voxels, active_voxels;
  double max_distance;
} tvdb_mesh_level_set_result_t;
/* Narrow-band level set of a triangle mesh, without a dense bounding-box
 * array. Voxels near every face are seeded by sampling the faces at half the
 * voxel size, then dilated to cover the band. Unsigned distances are exact
 * closest-face distances (BVH); the sign is inside where the generalized
 * winding number's magnitude exceeds 0.5, so open, self-intersecting,
 * inconsistently oriented and inside-out meshes still get a consistent sign. The result is a standard float
 * 5/4/3 tree with a uniform-scale transform, class "level set", background
 * exterior_width, interior tiles flood-filled to -interior_width and pruned.
 * Active values satisfy -interior_width < value < exterior_width. Output is
 * independent of the pool's worker count; NULL pool selects up to eight
 * workers for large jobs. The mesh must have finite vertices and in-range
 * faces. Replaces an initialized owning out; failure preserves out. */
tvdb_status_t tvdb_mesh_to_level_set(const tvdb_triangle_mesh* mesh,
                                     const tvdb_mesh_level_set_options_t* options,
                                     tvdb_thread_pool_t* pool, tvdb_grid_t* out,
                                     tvdb_mesh_level_set_result_t* result,
                                     tvdb_error_t* err);
tvdb_status_t tvdb_grid_level_set_track(const tvdb_grid_t* source,
                                        float outside_width, float inside_width,
                                        size_t max_voxels, tvdb_grid_t* out,
                                        tvdb_level_set_rebuild_result_t* result,
                                        tvdb_error_t* err);

typedef enum {
  TVDB_BOUNDARY_DIRICHLET = 0,
  TVDB_BOUNDARY_NEUMANN = 1
} tvdb_boundary_kind_t;
/* Called once per missing-neighbor face. Dirichlet value is at that neighbor's
 * center; Neumann value is outward k*du/dn at the half-cell face. Return zero
 * to fail. NULL callback selects zero Dirichlet. Coordinates use int64 so
 * neighbors outside int32 remain representable. */
typedef int (*tvdb_sparse_boundary_fn)(const int32_t coord[3],
                                       const int64_t neighbor[3],
                                       tvdb_boundary_kind_t* kind,
                                       double* value, void* user);
/* Positive finite face conductivity, called once per undirected active edge
 * and once per boundary face; the same coefficient is used by both rows. */
typedef double (*tvdb_sparse_coefficient_fn)(const int32_t coord[3],
                                             const int64_t neighbor[3],
                                             void* user);
typedef struct {
  int max_iterations;
  double tolerance; /* relative L2 residual; includes returned float rounding */
  size_t max_voxels;
  tvdb_sparse_boundary_fn boundary;
  tvdb_sparse_coefficient_fn coefficient;
  void* user;
} tvdb_sparse_poisson_options_t;
/* Solve -div(k grad u)=rhs on active topology with Jacobi-preconditioned CG in
 * double. Orthogonal affine axes may have different spacing; shear is rejected.
 * Active tiles expand within max_voxels. Each pure-Neumann connected component
 * requires compatible summed RHS/flux (rounding tolerance 1e-10 of L1
 * magnitude) and gets zero-mean u; disconnected components are handled
 * independently. A zero RHS needs no iterations. Iteration exhaustion commits a
 * finite iterate with converged=false; invalid/OOM/breakdown preserves outputs.
 * Inactive output values/background are zero, independent of boundary
 * callbacks. */
tvdb_status_t tvdb_grid_solve_poisson(
    const tvdb_grid_t* rhs, const tvdb_sparse_poisson_options_t* options,
    tvdb_grid_t* out, tvdb_poisson_result_t* result, tvdb_error_t* err);
#ifdef __cplusplus
}
#endif
