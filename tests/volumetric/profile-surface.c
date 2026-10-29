/* SPDX-License-Identifier: Apache-2.0 */
/* One-worker surface meshing workload for instruction-count comparisons. */
#include <stdio.h>
#include <stdlib.h>

#include "tinyvdb_io.h"
#include "tinyvdb_sparse_tools.h"

int main(int argc, char **argv) {
  tvdb_file_t file = {0};
  tvdb_error_t err = {0};
  tvdb_triangle_mesh mesh = {0};
  tvdb_thread_pool_t *pool = NULL;
  int result = 1, open = 0;
  if (argc != 2) {
    fprintf(stderr, "usage: %s input.vdb\n", argv[0]);
    return 2;
  }
  if (tvdb_file_open(&file, argv[1], NULL, &err) != TVDB_OK) goto done;
  open = 1;
  if (tvdb_read_grid(&file, 0, &err) != TVDB_OK ||
      tvdb_thread_pool_create(1, &pool, &err) != TVDB_OK ||
      tvdb_grid_volume_to_mesh_pooled(&file.grids[0], 0, 0, (size_t)1 << 26,
                                      pool, &mesh, &err) != TVDB_OK)
    goto done;
  fprintf(stderr, "vertices=%zu faces=%zu\n", mesh.vertex_count, mesh.face_count);
  result = 0;
done:
  if (result) fprintf(stderr, "tinyvdb: %s\n", err.message);
  free(mesh.vertices);
  free(mesh.faces);
  tvdb_thread_pool_destroy(pool);
  if (open) tvdb_file_close(&file);
  return result;
}
