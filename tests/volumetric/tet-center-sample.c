/* Failed interior probes must fail the operation without replacing output. */
#include "tinyvdb_tetmesh.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int sample(const double p[3], float *value, void *user) {
  int mode = *(const int *)user;
  for (int i = 0; i < 3; ++i) {
    if (p[i] * 2 != floor(p[i] * 2)) {
      *value = NAN;
      return mode != 0;
    }
  }
  *value = 0;
  return 1;
}
int main(void) {
  const double lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
  const size_t worker_counts[] = {1, 4};
  tvdb_stuffing_options_t options = {0};
  tvdb_error_t error;
  options.spacing = 1;
  for (size_t w = 0; w < sizeof(worker_counts) / sizeof(worker_counts[0]); ++w) {
    tvdb_thread_pool_t *pool = NULL;
    CHECK(tvdb_thread_pool_create(worker_counts[w], &pool, &error) == TVDB_OK);
    for (int mode = 0; mode < 2; ++mode) {
      tvdb_tet_mesh mesh = {0};
      mesh.positions = (float *)malloc(3 * sizeof(float));
      CHECK(mesh.positions);
      mesh.positions[0] = 123;
      mesh.vertex_count = 1;
      float *original = mesh.positions;
      CHECK(tvdb_sdf_to_tet_mesh(sample, &mode, lo, hi, &options, pool, &mesh,
                                 &error) == TVDB_ERROR_INVALID_DATA);
      CHECK(error.status == TVDB_ERROR_INVALID_DATA);
      CHECK(mesh.positions == original && mesh.vertex_count == 1 &&
            mesh.positions[0] == 123);
      tvdb_tet_mesh_free(&mesh);
    }
    tvdb_thread_pool_destroy(pool);
  }
  puts("tetrahedron center sample failure: ok");
  return 0;
}
