/* A double-precision tetrahedron must remain valid in the float output mesh. */
#include "tinyvdb_tet_graded.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
typedef struct { double center, radius; } sphere_data;
static int sphere(const double p[3], float *value, void *user) {
  const sphere_data *s = (const sphere_data *)user;
  double x = p[0] - s->center;
  *value = (float)(sqrt(x*x + p[1]*p[1] + p[2]*p[2]) - s->radius);
  return 1;
}
static int check_case(double center, double radius) {
  sphere_data s = {center, radius};
  double lo[3] = {center-radius, -radius, -radius};
  double hi[3] = {center+radius, radius, radius};
  tvdb_stuffing_options_t uniform = {0};
  tvdb_graded_stuffing_options_t graded = {0};
  tvdb_tet_mesh mesh = {0}; tvdb_error_t err;
  mesh.positions = (float *)malloc(3 * sizeof(float)); CHECK(mesh.positions);
  mesh.vertex_count = 1; mesh.positions[0] = 123;
  float *saved = mesh.positions;
  uniform.spacing = radius * 0.5;
  graded.fine_spacing = radius * 0.5; graded.coarse_spacing = radius;
  graded.grading = 0.8; graded.warp_alpha = 0.2;
  CHECK(tvdb_sdf_to_tet_mesh(sphere, &s, lo, hi, &uniform, NULL, &mesh, &err) == TVDB_ERROR_INVALID_DATA);
  CHECK(strstr(err.message, center > 1e20 ? "float range" : "float precision"));
  CHECK(mesh.positions == saved && mesh.vertex_count == 1 && mesh.positions[0] == 123);
  CHECK(tvdb_sdf_to_tet_mesh_graded(sphere, &s, lo, hi, &graded, &mesh, &err) == TVDB_ERROR_INVALID_DATA);
  CHECK(strstr(err.message, center > 1e20 ? "float range" : "float precision"));
  CHECK(mesh.positions == saved && mesh.vertex_count == 1 && mesh.positions[0] == 123);
  tvdb_tet_mesh_free(&mesh);
  return 0;
}
int main(void) {
  CHECK(check_case(1e40, 1e37) == 0); /* double coordinates exceed FLT_MAX */
  CHECK(check_case(1e10, 1) == 0);    /* float ULP exceeds the sphere's diameter */
  puts("tetrahedron float output range: ok");
  return 0;
}
