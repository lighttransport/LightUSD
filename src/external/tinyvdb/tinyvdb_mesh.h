#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "tvdb_memory.h"

// Data types
typedef struct {
  float x, y, z;
} tvdb_vec3f;

typedef struct {
  uint32_t v0, v1, v2;
} tvdb_triangle;

typedef struct {
  tvdb_vec3f* vertices;
  size_t vertex_count;
  size_t vertex_capacity;
  tvdb_triangle* faces;
  size_t face_count;
  size_t face_capacity;
} tvdb_triangle_mesh;

typedef struct {
  int nx, ny, nz;
  float ox, oy, oz;
  float voxel_size;
  float* data;
} tvdb_dense_grid;

// ---- Bounding-volume hierarchy over a mesh's triangles ----------------------
//
// Exposed so the GPU backend can walk the *same* tree the CPU query uses. The
// traversal is only fast if both sides prune on the same bounds and select
// triangles by the same rule, and the selection rule is a tie-break: a triangle
// on a strictly smaller squared distance wins, and an exact tie goes to the lower
// face index. Any reimplementation that visits candidates in a different order
// without that tie-break disagrees at the equidistant voxels, changing both the
// distance and the sign. Sharing the tree removes the whole class of divergence
// rather than re-deriving it.
typedef struct {
  float    bmin[3];
  float    bmax[3];
  int32_t  left;    /* first child; -1 for a leaf */
  int32_t  right;   /* second child; -1 for a leaf */
  int32_t  start;   /* first primitive (leaf only) */
  int32_t  count;   /* primitive count (leaf only) */
} tvdb_mesh_bvh_node_t;   /* 40 bytes; asserted, and uploaded verbatim as std430 */

typedef struct tvdb_mesh_bvh tvdb_mesh_bvh_t;

// Build over every face of `mesh`. Returns false if the tree cannot be built,
// in which case the caller should fall back to an exhaustive scan.
bool tvdb_mesh_bvh_build(const tvdb_triangle_mesh* mesh, tvdb_mesh_bvh_t** out);
void tvdb_mesh_bvh_destroy(tvdb_mesh_bvh_t* bvh);
int32_t tvdb_mesh_bvh_node_count(const tvdb_mesh_bvh_t* bvh);
int32_t tvdb_mesh_bvh_prim_count(const tvdb_mesh_bvh_t* bvh);
// Deepest leaf, 0 for a single node. A depth-first traversal with an explicit
// stack needs at most depth+1 entries, so this is what bounds a GPU-side stack.
int32_t tvdb_mesh_bvh_max_depth(const tvdb_mesh_bvh_t* bvh);
const tvdb_mesh_bvh_node_t* tvdb_mesh_bvh_nodes(const tvdb_mesh_bvh_t* bvh);
// Permutation of face indices, indexed by a leaf's `start`.
const int32_t* tvdb_mesh_bvh_prims(const tvdb_mesh_bvh_t* bvh);

// API
// Constructor: grid need not be initialized; release any previous data first.
// Failure leaves the handle unchanged. Checked replacement and reusable mesh
// acceleration are available in tinyvdb_mesh_conversion.h.
bool tvdb_mesh_to_sdf(const tvdb_triangle_mesh* mesh, float voxel_size, float band_width, tvdb_dense_grid* grid, tvdb_arena_allocator_t* arena);
// Appends to an initialized owning mesh, with the legacy MC table winding
// (toward the negative phase). The checked API offers outward winding.
bool tvdb_sdf_to_mesh(const tvdb_dense_grid* grid, float isovalue, tvdb_triangle_mesh* mesh, tvdb_arena_allocator_t* arena);

// Marching-cubes lookup tables (edge: 256 ints; triangle: flat 256*16 ints).
const int* tvdb_mc_edge_table(void);
const int* tvdb_mc_tri_table_flat(void);
// Appends an outward oriented reconstruction for a negative-inside SDF.
// Uses classic MC; ambiguous cases do not guarantee a manifold result.
bool tvdb_make_manifold(const tvdb_triangle_mesh* input, double resolution, double isovalue, tvdb_triangle_mesh* output, tvdb_arena_allocator_t* arena);

typedef enum {
  TVDB_SIGN_FLOOD_FILL = 0,
  TVDB_SIGN_SWEEP = 1,
} tvdb_sign_method;

bool tvdb_mesh_to_sdf_vdb(const tvdb_triangle_mesh* mesh, float voxel_size, float band_width, tvdb_dense_grid* grid, tvdb_sign_method sign_method, tvdb_arena_allocator_t* arena);
bool tvdb_make_manifold_vdb(const tvdb_triangle_mesh* input, double resolution, double isovalue, tvdb_triangle_mesh* output, tvdb_sign_method sign_method, tvdb_arena_allocator_t* arena);

// Keep original memory management for compatibility
void tvdb_triangle_mesh_init(tvdb_triangle_mesh* mesh);
void tvdb_triangle_mesh_free(tvdb_triangle_mesh* mesh);
void tvdb_dense_grid_init(tvdb_dense_grid* grid, int nx, int ny, int nz);
void tvdb_dense_grid_init_uninit(tvdb_dense_grid* grid, int nx, int ny, int nz);
void tvdb_dense_grid_free(tvdb_dense_grid* grid);

// Arena-based init
void tvdb_triangle_mesh_init_arena(tvdb_triangle_mesh* mesh, tvdb_arena_allocator_t* arena);
void tvdb_dense_grid_init_arena(tvdb_dense_grid* grid, int nx, int ny, int nz, tvdb_arena_allocator_t* arena);

#ifdef __cplusplus
}
#endif
