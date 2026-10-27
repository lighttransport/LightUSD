#pragma once
/* Tetrahedral meshes of the interior of a signed distance field by isosurface
 * stuffing (Labelle and Shewchuk 2007, "Isosurface Stuffing: Fast Tetrahedral
 * Meshes with Good Dihedral Angles").
 *
 * A body-centered cubic lattice of spacing h (cube corners and cube centers)
 * covers the region. Values are sampled at lattice vertices; a vertex too
 * close to a cut point (where the field crosses the isovalue on one of its
 * edges: closer than alpha_long * h along an axis edge, alpha_short * h *
 * sqrt(3)/2 along a diagonal edge) is warped onto that cut point. Each lattice
 * tetrahedron then becomes zero, one or several output tetrahedra by its
 * vertex signs. Wedge and pyramid pieces are split by the minimum-key diagonal
 * rule (Dompierre et al. 1999), which makes neighbors agree on shared
 * quadrilaterals; this replaces the paper's parity stencils, so its exact
 * dihedral bounds (10.7 to 164.8 degrees for the default alphas) are not
 * guaranteed for those pieces. The boundary is watertight and every output
 * tetrahedron is positively oriented. Lattice sampling and boundary
 * extraction run on the task pool; output is deterministic and independent of
 * the worker count. Values are negative inside. */
#include <stdint.h>

#include "tinyvdb_io.h"
#include "tinyvdb_thread.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float* positions;     /* 3 per vertex */
  size_t vertex_count;
  int32_t* tets;        /* 4 per tetrahedron, positive orientation */
  size_t tet_count;
  int32_t* boundary;    /* 3 per boundary triangle, oriented outward */
  size_t boundary_count;
} tvdb_tet_mesh;
void tvdb_tet_mesh_free(tvdb_tet_mesh* mesh);

/* Batch sampler for the lattice pass: values at `count` world points (3
 * doubles each). The per-point sampler still serves the few extra samples
 * stuffing needs. tvdb_gpu_level_set_to_tet_mesh installs a CUDA/HIP one. */
typedef tvdb_status_t (*tvdb_sdf_batch_sample_fn)(void* user, const double* points,
                                                  size_t count, float* values,
                                                  tvdb_error_t* err);
typedef struct {
  double spacing;      /* lattice spacing h, world units (> 0) */
  double alpha_long;   /* 0 selects 0.24999 */
  double alpha_short;  /* 0 selects 0.41189 */
  float isovalue;
  size_t max_lattice_vertices; /* 0 selects 8,000,000 */
  size_t max_tets;             /* 0 selects 4,000,000 */
  tvdb_sdf_batch_sample_fn batch_sample; /* NULL: the per-point sampler */
  void* batch_user;
} tvdb_stuffing_options_t;

/* Field sampler for tvdb_sdf_to_tet_mesh; must be thread-safe and return a
 * finite value at every lattice vertex and any tetrahedron center queried
 * inside [bbox_min, bbox_max] padded by one spacing. Return nonzero on success.
 * The field must be a signed distance
 * (1-Lipschitz) function: undirected distances in world units, negative
 * inside. The warp pass relies on that to skip vertices that no lattice edge
 * can cross. */
typedef int (*tvdb_sdf_sample_fn)(const double p[3], float* value, void* user);

/* Interior of an analytic or caller-provided field over a world box that must
 * contain the whole interior. Replaces out (an initialized, possibly empty
 * mesh) on success; failure preserves it. */
tvdb_status_t tvdb_sdf_to_tet_mesh(tvdb_sdf_sample_fn sample, void* user,
                                   const double bbox_min[3],
                                   const double bbox_max[3],
                                   const tvdb_stuffing_options_t* options,
                                   tvdb_thread_pool_t* pool, tvdb_tet_mesh* out,
                                   tvdb_error_t* err);
/* Interior of a float level-set grid: trilinear samples of stored values
 * (including inactive tiles, so flood-filled interiors work) over the
 * bounding box of its active voxels. Any invertible affine transform. */
tvdb_status_t tvdb_level_set_to_tet_mesh(const tvdb_grid_t* level_set,
                                         const tvdb_stuffing_options_t* options,
                                         tvdb_thread_pool_t* pool,
                                         tvdb_tet_mesh* out, tvdb_error_t* err);

typedef struct {
  double min_dihedral, max_dihedral; /* degrees */
  double volume, min_volume;
  size_t inverted;                   /* tetrahedra with volume <= 0 */
} tvdb_tet_mesh_quality_t;
tvdb_status_t tvdb_tet_mesh_quality(const tvdb_tet_mesh* mesh,
                                    tvdb_tet_mesh_quality_t* out,
                                    tvdb_error_t* err);

#ifdef __cplusplus
}
#endif
