#pragma once
#include "tinyvdb_sparse_tree.h"
#include "tinyvdb_thread.h"
#include "tinyvdb_sparse.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct tvdb_sdf_tree tvdb_sdf_tree_t;
typedef enum {
    TVDB_SDF_FILTER_MEAN,      /* separable three-point mean (27-point footprint) */
    TVDB_SDF_FILTER_GAUSSIAN,  /* separable [1,2,1]/4 */
    TVDB_SDF_FILTER_LAPLACIAN, /* stable diffusion step: six-neighbor average */
    TVDB_SDF_FILTER_DILATE,    /* seven-point minimum */
    TVDB_SDF_FILTER_ERODE      /* seven-point maximum */
} tvdb_sdf_filter_t;
typedef struct {
    int iterations;
    int converged;
    float max_change; /* May be infinite on the first propagation from unreached values. */
    size_t frozen_voxels, unreached_voxels;
} tvdb_sdf_sweep_result_t;
typedef struct {
    size_t leaves, active_voxels, metadata_bytes, scratch_bytes, workers;
} tvdb_sdf_tree_info_t;

/* Reusable workspace bound to an owning float tree with isotropic voxel size.
 * Internal and leaf dimensions 1..32 and hierarchies up to TVDB_MAX_TREE_DEPTH are supported.
 * A NULL pool creates an owned pool capped at eight workers and the leaf count;
 * otherwise the pool is borrowed.
 * A NULL allocator uses malloc/free. The allocator struct is copied; its
 * callbacks and user_ctx must remain valid until destroy. Create sets *out to
 * NULL on failure.
 *
 * Keep the grid, transform, leaf allocations, masks, tiles and hierarchy alive
 * and unchanged until destroy; leaf values may change between calls. Serialize calls
 * on a workspace and on its grid. Destroy borrowed pools after workspaces. */
tvdb_status_t tvdb_sdf_tree_create(tvdb_grid_t *grid, tvdb_thread_pool_t *pool,
                                   const tvdb_allocator_t *allocator, tvdb_sdf_tree_t **out,
                                   tvdb_error_t *err);
void tvdb_sdf_tree_destroy(tvdb_sdf_tree_t *workspace);
void tvdb_sdf_tree_info(const tvdb_sdf_tree_t *workspace, tvdb_sdf_tree_info_t *out);

/* Update active leaf values only, preserving buffers, topology and tiles.
 * All operations preserve the input on allocation or numeric failure.
 * Filters use stored inactive leaf values and signed tile/background values
 * as fixed boundary data in every pass. Zero iterations is a validated no-op. */
tvdb_status_t tvdb_sdf_tree_filter(tvdb_sdf_tree_t *workspace, tvdb_sdf_filter_t kind,
                                   int iterations, tvdb_error_t *err);
/* Compute the same filter without committing to the source tree. The owning
 * sparse output is replaced only on success. Flat transform restrictions
 * apply; inactive values and signed tiles remain fixed in every pass. */
tvdb_status_t tvdb_sdf_tree_filter_to_sparse(tvdb_sdf_tree_t *workspace,
    tvdb_sdf_filter_t kind, int iterations, tvdb_sparse_grid *out, tvdb_error_t *err);
/* Adds a world-distance offset to active values. This does not grow the band. */
tvdb_status_t tvdb_sdf_tree_offset(tvdb_sdf_tree_t *workspace, float distance, tvdb_error_t *err);

/* Eight-direction block-wavefront Gauss-Seidel fast sweeping, parallel across
 * independent leaves. Frozen |phi| <= frozen_band and signs are preserved.
 * The domain consists of active leaf voxels; inactive voxels/tiles are excluded
 * from the Eikonal stencil. Unseeded components retain their original values
 * and count as unreached, so convergence is false. A zero iteration budget
 * preserves the input. Tolerance is an absolute world-distance change.
 * Scratch is one float per active voxel plus two packed flag bits; filters
 * lazily retain a second float buffer. No bounding-box volume is allocated. */
tvdb_status_t tvdb_sdf_tree_fast_sweep(tvdb_sdf_tree_t *workspace, float frozen_band,
                                       int max_iterations, float tolerance,
                                       tvdb_sdf_sweep_result_t *result, tvdb_error_t *err);
#ifdef __cplusplus
}
#endif
