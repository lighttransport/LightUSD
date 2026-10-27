#pragma once
/* Experimental CPU graded tetrahedralization. See the limits below before use. */
#include "tinyvdb_tetmesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  double fine_spacing;   /* positive maximum edge size at the isosurface */
  double coarse_spacing; /* maximum desired edge size away from the surface */
  double grading;        /* positive growth rate per unit SDF distance */
  double warp_alpha;     /* [0, 0.5); zero disables vertex warping */
  float isovalue;
  size_t max_vertices;        /* 0: 8,000,000 background vertices */
  size_t max_background_tets; /* 0: 8,000,000 background tetrahedra */
  size_t max_output_tets;     /* 0: 4,000,000 output tetrahedra */
} tvdb_graded_stuffing_options_t;

/* bbox must contain the entire interior; one coarse spacing of padding is
 * added. sample must be a finite 1-Lipschitz signed-distance field and may be
 * called at edge midpoints, vertices, and tetrahedron centers. This path is
 * experimental: features smaller than the requested resolution may be missed,
 * dihedral bounds are not guaranteed, and output volume is approximate.
 * Execution is serial and deterministic; on failure, out is preserved. */
tvdb_status_t tvdb_sdf_to_tet_mesh_graded(tvdb_sdf_sample_fn sample, void *user,
    const double bbox_min[3], const double bbox_max[3],
    const tvdb_graded_stuffing_options_t *options, tvdb_tet_mesh *out,
    tvdb_error_t *err);

#ifdef __cplusplus
}
#endif
