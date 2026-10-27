/* SPDX-License-Identifier: Apache-2.0 */
/* One-worker level-set stuffing workload for instruction-count comparisons. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "tinyvdb_io.h"
#include "tinyvdb_tetmesh.h"

int main(int argc, char **argv) {
  tvdb_file_t file = {0};
  tvdb_error_t err = {0};
  tvdb_tet_mesh mesh = {0};
  tvdb_thread_pool_t *pool = NULL;
  tvdb_stuffing_options_t options = {0};
  char *end = NULL;
  int result = 1, open = 0;
  if (argc != 3) {
    fprintf(stderr, "usage: %s input.vdb spacing\n", argv[0]);
    return 2;
  }
  options.spacing = strtod(argv[2], &end);
  if (!end || end == argv[2] || *end || !isfinite(options.spacing) || options.spacing <= 0)
    return 2;
  if (tvdb_file_open(&file, argv[1], NULL, &err) != TVDB_OK) goto done;
  open = 1;
  if (tvdb_read_grid(&file, 0, &err) != TVDB_OK ||
      tvdb_thread_pool_create(1, &pool, &err) != TVDB_OK ||
      tvdb_level_set_to_tet_mesh(&file.grids[0], &options, pool, &mesh, &err) != TVDB_OK)
    goto done;
  fprintf(stderr, "vertices=%zu tets=%zu boundary=%zu\n", mesh.vertex_count,
          mesh.tet_count, mesh.boundary_count);
  result = 0;
done:
  if (result) fprintf(stderr, "tinyvdb: %s\n", err.message);
  tvdb_tet_mesh_free(&mesh);
  tvdb_thread_pool_destroy(pool);
  if (open) tvdb_file_close(&file);
  return result;
}
