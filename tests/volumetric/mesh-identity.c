/* SPDX-License-Identifier: Apache-2.0 */
/* Serialize mesher and stuffing output for byte-for-byte revision comparisons.
 * Build this source against each revision's own headers and libraries. */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_io.h"
#include "tinyvdb_sparse_tools.h"
#include "tinyvdb_tetmesh.h"

_Static_assert(sizeof(tvdb_vec3f) == 3 * sizeof(float), "padded vertex");
_Static_assert(sizeof(tvdb_triangle) == 3 * sizeof(uint32_t), "padded triangle");

static int write_array(FILE *f, const void *data, size_t size, size_t count) {
  uint64_t n = (uint64_t)count;
  return fwrite(&n, sizeof(n), 1, f) == 1 &&
         (!count || fwrite(data, size, count, f) == count);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s input.vdb spacing output.bin\n", argv[0]);
    return 2;
  }
  char *end = NULL;
  double spacing = strtod(argv[2], &end);
  if (!end || end == argv[2] || *end || !isfinite(spacing) || !(spacing > 0)) {
    fprintf(stderr, "invalid spacing\n");
    return 2;
  }
  tvdb_file_t file = {0};
  tvdb_error_t err = {0};
  tvdb_triangle_mesh surface = {0};
  tvdb_tet_mesh tets = {0};
  tvdb_thread_pool_t *pool = NULL;
  tvdb_stuffing_options_t options = {0};
  options.spacing = spacing;
  int result = 1;
  if (tvdb_file_open(&file, argv[1], NULL, &err) != TVDB_OK) goto fail_open;
  if (tvdb_read_grid(&file, 0, &err) != TVDB_OK) goto fail;
  if (tvdb_thread_pool_create(2, &pool, &err) != TVDB_OK) goto fail;
  if (tvdb_grid_volume_to_mesh_pooled(&file.grids[0], 0, 0, (size_t)1 << 26,
                                      pool, &surface, &err) != TVDB_OK) goto fail;
  if (tvdb_level_set_to_tet_mesh(&file.grids[0], &options, pool, &tets, &err) != TVDB_OK)
    goto fail;
  if (!surface.vertex_count || !surface.face_count || !tets.vertex_count ||
      !tets.tet_count || !tets.boundary_count) {
    fprintf(stderr, "empty output; choose a suitable level set and spacing\n");
    goto done;
  }
  FILE *out = fopen(argv[3], "wb");
  if (!out) { perror(argv[3]); goto done; }
  int ok = write_array(out, surface.vertices, sizeof(*surface.vertices), surface.vertex_count) &&
           write_array(out, surface.faces, sizeof(*surface.faces), surface.face_count) &&
           write_array(out, tets.positions, 3 * sizeof(*tets.positions), tets.vertex_count) &&
           write_array(out, tets.tets, 4 * sizeof(*tets.tets), tets.tet_count) &&
           write_array(out, tets.boundary, 3 * sizeof(*tets.boundary), tets.boundary_count);
  if (fclose(out) != 0) ok = 0;
  if (!ok) { fprintf(stderr, "failed to write output\n"); goto done; }
  fprintf(stderr, "surface %zu vertices %zu triangles; tets %zu vertices %zu tets %zu boundary triangles\n",
          surface.vertex_count, surface.face_count, tets.vertex_count, tets.tet_count,
          tets.boundary_count);
  result = 0;
  goto done;
fail:
  fprintf(stderr, "tinyvdb: %s\n", err.message);
done:
  free(surface.vertices);
  free(surface.faces);
  tvdb_tet_mesh_free(&tets);
  if (pool) tvdb_thread_pool_destroy(pool);
  tvdb_file_close(&file);
  return result;
fail_open:
  fprintf(stderr, "tinyvdb: %s\n", err.message);
  return 1;
}
