/* Verify oversized lattice allocations are rejected before sampling on 32-bit. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tinyvdb_tetmesh.h"

static int sample(const double p[3], float *value, void *user) {
  (void)p;
  (void)value;
  (void)user;
  return 0; /* Must not be called for the oversized lattice. */
}

int main(void) {
#if SIZE_MAX > UINT32_MAX
  puts("SKIP: requires a 32-bit size_t");
  return 77;
#else
  const double lo[3] = {0, 0, 0}, hi[3] = {600, 600, 600};
  tvdb_stuffing_options_t options = {0};
  tvdb_tet_mesh mesh = {0};
  tvdb_error_t error = {0};
  options.spacing = 1;
  options.max_lattice_vertices = SIZE_MAX;
  tvdb_status_t st = tvdb_sdf_to_tet_mesh(sample, NULL, lo, hi, &options,
                                          NULL, &mesh, &error);
  if (st != TVDB_ERROR_INVALID_ARGUMENT ||
      !strstr(error.message, "allocation size overflow") || mesh.positions || mesh.tets) {
    fprintf(stderr, "unexpected result: %d (%s)\n", (int)st, error.message);
    tvdb_tet_mesh_free(&mesh);
    return 1;
  }
  puts("PASS: oversized stuffing allocation rejected before sampling");
  return 0;
#endif
}
