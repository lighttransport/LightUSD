#pragma once
/* Internal conforming edge-star refinement for a graded stuffing background.
 * Not a public mesh API: the graded sampler and stuffing path are not yet
 * connected. Each split bisects every tetrahedron incident to the edge. */
#include <stddef.h>
#include <stdint.h>
#include "tinyvdb_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tvdb_tet_refine_graph tvdb_tet_refine_graph;

/* Copies input positions and tetrahedra. max_vertices/max_tets are hard caps.
 * On failure *out is NULL. Inputs must describe valid, nondegenerate index
 * tuples; positions must be finite. */
tvdb_status_t tvdb_tet_refine_create(const double *positions, size_t nvertices,
                                     const uint32_t *tets, size_t ntets,
                                     size_t max_vertices, size_t max_tets,
                                     tvdb_tet_refine_graph **out, tvdb_error_t *err);
/* On failure the graph is disposable but must not be reused. The new vertex
 * is the midpoint; children replace parents in place and append in increasing
 * parent-index order. No existing vertex IDs change. */
tvdb_status_t tvdb_tet_refine_split(tvdb_tet_refine_graph *graph, uint32_t a,
                                    uint32_t b, uint32_t *midpoint, tvdb_error_t *err);
/* Desired maximum edge length at a midpoint. Return nonzero and set a finite,
 * positive size; the callback must be deterministic during one refinement. */
typedef int (*tvdb_tet_refine_size_fn)(const double point[3], double *size, void *user);
/* Refine all edges longer than the midpoint size. The edge star is split as a
 * unit; changed tetrahedra are revisited until the criterion holds everywhere.
 * Graph limits bound the operation. On failure the graph must be discarded. */
tvdb_status_t tvdb_tet_refine_adapt(tvdb_tet_refine_graph *graph,
                                    tvdb_tet_refine_size_fn size, void *user,
                                    tvdb_error_t *err);
const double *tvdb_tet_refine_positions(const tvdb_tet_refine_graph *graph);
const uint32_t *tvdb_tet_refine_tets(const tvdb_tet_refine_graph *graph);
size_t tvdb_tet_refine_vertex_count(const tvdb_tet_refine_graph *graph);
size_t tvdb_tet_refine_tet_count(const tvdb_tet_refine_graph *graph);
void tvdb_tet_refine_destroy(tvdb_tet_refine_graph *graph);
#ifdef __cplusplus
}
#endif
