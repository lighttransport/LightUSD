/* SPDX-License-Identifier: Apache-2.0 */
/* Level-set stuffing workload for instruction-count comparisons. */
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
  char *end = NULL, *worker_end = NULL;
  size_t workers = 1;
  int result = 1, open = 0;
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "usage: %s input.vdb spacing [workers]\n", argv[0]);
    return 2;
  }
  options.spacing = strtod(argv[2], &end);
  if (!end || end == argv[2] || *end || !isfinite(options.spacing) || options.spacing <= 0)
    return 2;
  if (argc == 4) {
    const unsigned long parsed = strtoul(argv[3], &worker_end, 10);
    if (!worker_end || worker_end == argv[3] || *worker_end || !parsed || parsed > 256)
      return 2;
    workers = (size_t)parsed;
  }
  if (tvdb_file_open(&file, argv[1], NULL, &err) != TVDB_OK) goto done;
  open = 1;
  if (tvdb_read_grid(&file, 0, &err) != TVDB_OK ||
      tvdb_thread_pool_create(workers, &pool, &err) != TVDB_OK ||
      tvdb_level_set_to_tet_mesh(&file.grids[0], &options, pool, &mesh, &err) != TVDB_OK)
    goto done;
  fprintf(stderr, "workers=%zu vertices=%zu tets=%zu boundary=%zu\n", workers,
          mesh.vertex_count,
          mesh.tet_count, mesh.boundary_count);
  result = 0;
done:
  if (result) fprintf(stderr, "tinyvdb: %s\n", err.message);
  tvdb_tet_mesh_free(&mesh);
  tvdb_thread_pool_destroy(pool);
  if (open) tvdb_file_close(&file);
  return result;
}
