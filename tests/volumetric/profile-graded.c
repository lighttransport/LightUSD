/* Reproducible CPU graded-stuffing profile, including a compact output hash. */
#include "tinyvdb_tet_graded.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int sphere(const double p[3], float *value, void *user) {
  (void)user;
  *value = (float)(sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]) - 1);
  return 1;
}
static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  for (size_t i = 0; i < size; ++i)
    hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
  return hash;
}
int main(int argc, char **argv) {
  const double lo[] = {-1,-1,-1}, hi[] = {1,1,1};
  tvdb_graded_stuffing_options_t options = {0}; tvdb_tet_mesh mesh = {0}; tvdb_error_t err;
  options.fine_spacing = argc > 1 ? strtod(argv[1], NULL) : 0.2;
  options.coarse_spacing = 0.8; options.grading = 0.8; options.warp_alpha = 0.2;
  tvdb_status_t st = argc > 2 && atoi(argv[2])
      ? tvdb_sdf_to_tet_mesh_graded_guarded(sphere, NULL, lo, hi, &options, &mesh, &err)
      : tvdb_sdf_to_tet_mesh_graded(sphere, NULL, lo, hi, &options, &mesh, &err);
  if (st != TVDB_OK) {
    fprintf(stderr, "graded mesh: %s\n", err.message); return 1;
  }
  uint64_t hash = hash_bytes(UINT64_C(14695981039346656037), mesh.positions,
                             3 * mesh.vertex_count * sizeof(float));
  hash = hash_bytes(hash, mesh.tets, 4 * mesh.tet_count * sizeof(int32_t));
  hash = hash_bytes(hash, mesh.boundary, 3 * mesh.boundary_count * sizeof(int32_t));
  printf("vertices %zu tets %zu boundary %zu hash %016llx\n", mesh.vertex_count,
         mesh.tet_count, mesh.boundary_count, (unsigned long long)hash);
  tvdb_tet_mesh_free(&mesh); return 0;
}
