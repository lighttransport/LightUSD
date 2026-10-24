#pragma once
#include "tinyvdb_mesh.h"
#include "tinyvdb_thread.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct tvdb_mesh_sdf tvdb_mesh_sdf_t;
typedef struct {
    size_t faces, acceleration_bytes;
} tvdb_mesh_sdf_info_t;
/* Reusable closest-triangle acceleration. Borrows the mesh and its arrays;
 * keep their pointers, counts and geometry fixed until destroy. Concurrent
 * generation into distinct outputs is supported with a shared borrowed pool.
 * Sign uses the closest face normal; this is not a watertight inside test for
 * open/inconsistently oriented meshes. Degenerate faces have zero normals. */
tvdb_status_t tvdb_mesh_sdf_create(const tvdb_triangle_mesh *mesh, tvdb_mesh_sdf_t **out,
                                   tvdb_error_t *err);
void tvdb_mesh_sdf_destroy(tvdb_mesh_sdf_t *workspace);
void tvdb_mesh_sdf_info(const tvdb_mesh_sdf_t *workspace, tvdb_mesh_sdf_info_t *out);
/* Unsigned closest-triangle distance at a finite world point. Output is
 * preserved on failure; the workspace continues borrowing the mesh. BVH
 * traversal rounds query positions to float; returned distance uses double. */
tvdb_status_t tvdb_mesh_sdf_distance(const tvdb_mesh_sdf_t *workspace,
    const double point[3], double *distance, tvdb_error_t *err);
/* Generate a padded, cell-centered dense lattice with values clamped to band.
 * Positive finite spacing/band required; each dimension is capped at 2048.
 * out must be an initialized owning grid, and must not overlap the input mesh.
 * Success replaces out (freeing its old heap data). Failure preserves out and
 * restores the arena offset. With an arena, old output storage belongs to that
 * same arena; its old allocation remains retained until the caller resets it.
 * Temporary normals/BVH and counting/edge caches always use the heap.
 * NULL pool selects up to eight workers, with serial execution for small jobs. */
tvdb_status_t tvdb_mesh_sdf_generate(const tvdb_mesh_sdf_t *workspace, float voxel_size,
                                     float band_width, tvdb_thread_pool_t *pool,
                                     tvdb_dense_grid *out, tvdb_arena_allocator_t *arena,
                                     tvdb_error_t *err);
tvdb_status_t tvdb_mesh_to_sdf_ex(const tvdb_triangle_mesh *mesh, float voxel_size,
                                  float band_width, tvdb_thread_pool_t *pool, tvdb_dense_grid *out,
                                  tvdb_arena_allocator_t *arena, tvdb_error_t *err);

typedef enum {
    TVDB_MESH_WINDING_TABLE = 0,  /* legacy table order, toward the negative phase */
    TVDB_MESH_WINDING_OUTWARD = 1 /* outward for negative-inside SDFs */
} tvdb_mesh_winding_t;

/* Indexed marching cubes, appending to an initialized mesh. Stable raster/edge
 * order and shared-edge deduplication are independent of worker count. Finite
 * grid values, origins, isovalue and positive spacing are required. Degenerate
 * triangles from exact-isovalue samples follow the classic MC table.
 * Counting is parallel; deterministic shared-edge emission is serial. Exact
 * output sizing avoids growth copies. Scratch chooses the smaller of a rolling
 * two-plane cache (~20*nx*ny bytes) and a pre-sized surface-edge hash. No scratch
 * is allocated per volume cell. Failures preserve mesh and arena offset.
 * Output buffers must not overlap the input grid or each other. */
tvdb_status_t tvdb_sdf_to_mesh_ex(const tvdb_dense_grid *grid, float isovalue,
                                  tvdb_mesh_winding_t winding, tvdb_thread_pool_t *pool,
                                  tvdb_triangle_mesh *mesh, tvdb_arena_allocator_t *arena,
                                  tvdb_error_t *err);
#ifdef __cplusplus
}
#endif
