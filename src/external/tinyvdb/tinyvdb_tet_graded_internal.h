#pragma once
/* Experimental integration; not part of tinyvdb's public API. */
#include "tinyvdb_tetmesh.h"
#include "tinyvdb_tet_refine_internal.h"

tvdb_status_t tvdb_tet_graded_stencil(const tvdb_tet_refine_graph *graph,
                                      tvdb_sdf_sample_fn sample, void *user,
                                      float isovalue, double alpha, size_t max_tets,
                                      tvdb_tet_mesh *out, tvdb_error_t *err);
