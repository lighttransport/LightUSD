#pragma once
/* Generalized winding numbers of triangle meshes (Jacobson, Kavan and
 * Sorkine-Hornung 2013, "Robust Inside-Outside Segmentation using Generalized
 * Winding Numbers"). Ported from lightgeom's lgphys_winding (Apache-2.0).
 *
 * The winding number is about 1 inside a closed, outward-oriented mesh and 0
 * outside. For open, self-intersecting or inconsistently oriented meshes it
 * degrades smoothly, so thresholding at 0.5 is a robust inside test (the sign
 * source of tvdb_mesh_to_level_set). */
#include "tinyvdb_io.h"
#include "tinyvdb_mesh.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct tvdb_winding tvdb_winding_t;
typedef enum {
  TVDB_WINDING_EXACT = 0, /* sum of exact solid angles over every face */
  TVDB_WINDING_FAST = 1   /* AABB tree with a dipole far field */
} tvdb_winding_kind_t;

/* Copies the mesh; the input may change or be freed afterwards. Requires at
 * least one face, face indices below vertex_count and finite vertices. The
 * fast tree splits at the median face centroid, so its depth is
 * ceil(log2(faces / 8)) + 1 at most. Evaluation is thread-safe. */
tvdb_status_t tvdb_winding_create(const tvdb_triangle_mesh *mesh,
                                  tvdb_winding_kind_t kind,
                                  tvdb_winding_t **out, tvdb_error_t *err);
void tvdb_winding_destroy(tvdb_winding_t *winding);
/* Winding number at a finite point; NAN for invalid arguments. */
double tvdb_winding_eval(const tvdb_winding_t *winding, const double p[3]);

/* The fast kind's tree, for device ports that must walk the same nodes: node
 * 0 is the root, a leaf (left < 0) owns faces order[first .. first+count),
 * and a node at least 2 box diagonals (TVDB_WINDING_FAR_DIAGONALS) from the
 * query contributes its dipole n (sum of area-weighted normals) at its
 * area-weighted centroid c. Children are visited left first. Face f's
 * vertices are the mesh's faces[f] vertices. NULL (and 0) for the exact kind. */
#define TVDB_WINDING_FAR_DIAGONALS 2.0
typedef struct {
  double lo[3], hi[3];
  double c[3];
  double n[3];
  int32_t first, count, left, right;
} tvdb_winding_node_t;
const tvdb_winding_node_t *tvdb_winding_nodes(const tvdb_winding_t *winding,
                                              size_t *count);
const int32_t *tvdb_winding_order(const tvdb_winding_t *winding);

#ifdef __cplusplus
}
#endif
