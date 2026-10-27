/* SPDX-License-Identifier: Apache-2.0 */
/* Stuffing output must stay byte-identical as the pool size changes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyvdb_tetmesh.h"

static int sphere(const double p[3], float *value, void *user) {
  (void)user;
  *value = (float)(sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 0.75);
  return 1;
}

static int same_mesh(const tvdb_tet_mesh *a, const tvdb_tet_mesh *b) {
  if (a->vertex_count != b->vertex_count || a->tet_count != b->tet_count ||
      a->boundary_count != b->boundary_count)
    return 0;
  for (size_t i = 0; i < a->vertex_count; ++i)
    for (size_t c = 0; c < 3; ++c)
      if (memcmp(a->positions + 3 * i + c, b->positions + 3 * i + c,
                 sizeof(float)) != 0)
        return 0;
  for (size_t i = 0; i < 4 * a->tet_count; ++i)
    if (a->tets[i] != b->tets[i]) return 0;
  for (size_t i = 0; i < 3 * a->boundary_count; ++i)
    if (a->boundary[i] != b->boundary[i]) return 0;
  return 1;
}

static int check_levelset_workers(const char *path) {
  tvdb_file_t file = {0};
  tvdb_error_t err = {0};
  tvdb_tet_mesh reference = {0};
  tvdb_thread_pool_t *pool = NULL;
  tvdb_stuffing_options_t options = {0};
  int result = 0, opened = 0;
  if (tvdb_file_open(&file, path, NULL, &err) != TVDB_OK) goto done;
  opened = 1;
  if (tvdb_read_grid(&file, 0, &err) != TVDB_OK) goto done;
  options.spacing = 0.15;
  const size_t worker_counts[] = {1, 2, 4, 8};
  for (size_t i = 0; i < sizeof(worker_counts) / sizeof(worker_counts[0]); ++i) {
    tvdb_tet_mesh mesh = {0};
    if (tvdb_thread_pool_create(worker_counts[i], &pool, &err) != TVDB_OK ||
        tvdb_level_set_to_tet_mesh(&file.grids[0], &options, pool, &mesh, &err) !=
            TVDB_OK) {
      fprintf(stderr, "%s worker count %zu: %s\n", path, worker_counts[i],
              err.message);
      tvdb_tet_mesh_free(&mesh);
      goto done;
    }
    tvdb_thread_pool_destroy(pool);
    pool = NULL;
    if (!mesh.vertex_count || !mesh.tet_count || !mesh.boundary_count ||
        (i && !same_mesh(&reference, &mesh))) {
      fprintf(stderr, "%s tet output differs with %zu workers\n", path,
              worker_counts[i]);
      tvdb_tet_mesh_free(&mesh);
      goto done;
    }
    if (!i) reference = mesh;
    else tvdb_tet_mesh_free(&mesh);
  }
  fprintf(stderr, "PASS: %s level-set output matches across 1/2/4/8 workers\n", path);
  result = 1;
done:
  tvdb_thread_pool_destroy(pool);
  tvdb_tet_mesh_free(&reference);
  if (opened) tvdb_file_close(&file);
  return result;
}

static int check_aggregate_tet_budget(void) {
  const double lo[3] = {-1.0, -1.0, -1.0};
  const double hi[3] = {1.0, 1.0, 1.0};
  const size_t worker_counts[] = {1, 4};
  tvdb_stuffing_options_t options = {0};
  options.spacing = 0.1;
  options.max_tets = 1;
  for (size_t i = 0; i < sizeof(worker_counts) / sizeof(worker_counts[0]); ++i) {
    tvdb_thread_pool_t *pool = NULL;
    tvdb_tet_mesh mesh = {0};
    tvdb_error_t err = {0};
    mesh.positions = (float *)malloc(3 * sizeof(float));
    if (!mesh.positions) return 0;
    mesh.positions[0] = 123.0f;
    mesh.vertex_count = 1;
    float *original = mesh.positions;
    if (tvdb_thread_pool_create(worker_counts[i], &pool, &err) != TVDB_OK) {
      tvdb_tet_mesh_free(&mesh);
      return 0;
    }
    const tvdb_status_t status = tvdb_sdf_to_tet_mesh(
        sphere, NULL, lo, hi, &options, pool, &mesh, &err);
    tvdb_thread_pool_destroy(pool);
    if (status != TVDB_ERROR_INVALID_ARGUMENT || err.status != status ||
        mesh.positions != original || mesh.vertex_count != 1 ||
        mesh.positions[0] != 123.0f) {
      fprintf(stderr, "tet budget behavior changed with %zu workers\n",
              worker_counts[i]);
      tvdb_tet_mesh_free(&mesh);
      return 0;
    }
    tvdb_tet_mesh_free(&mesh);
  }
  return 1;
}

int main(void) {
  const double lo[3] = {-1.0, -1.0, -1.0};
  const double hi[3] = {1.0, 1.0, 1.0};
  const size_t worker_counts[] = {1, 2, 4, 8};
  tvdb_stuffing_options_t options = {0};
  tvdb_tet_mesh reference = {0};
  options.spacing = 0.1;
  for (size_t i = 0; i < sizeof(worker_counts) / sizeof(worker_counts[0]); ++i) {
    tvdb_thread_pool_t *pool = NULL;
    tvdb_tet_mesh mesh = {0};
    tvdb_error_t err = {0};
    if (tvdb_thread_pool_create(worker_counts[i], &pool, &err) != TVDB_OK ||
        tvdb_sdf_to_tet_mesh(sphere, NULL, lo, hi, &options, pool, &mesh, &err) !=
            TVDB_OK) {
      fprintf(stderr, "worker count %zu: %s\n", worker_counts[i], err.message);
      tvdb_tet_mesh_free(&mesh);
      tvdb_thread_pool_destroy(pool);
      tvdb_tet_mesh_free(&reference);
      return 1;
    }
    tvdb_thread_pool_destroy(pool);
    if (!mesh.vertex_count || !mesh.tet_count || !mesh.boundary_count ||
        (i && !same_mesh(&reference, &mesh))) {
      fprintf(stderr, "tet output differs with %zu workers\n", worker_counts[i]);
      tvdb_tet_mesh_free(&mesh);
      tvdb_tet_mesh_free(&reference);
      return 1;
    }
    if (!i) reference = mesh;
    else tvdb_tet_mesh_free(&mesh);
  }
  fprintf(stderr, "PASS: %zu vertices, %zu tets, %zu boundary triangles across 1/2/4/8 workers\n",
          reference.vertex_count, reference.tet_count, reference.boundary_count);
  tvdb_tet_mesh_free(&reference);
  if (!check_aggregate_tet_budget()) return 1;
  const char *data_dir = getenv("TINYVDBIO_DATA_DIR");
  if (data_dir && *data_dir) {
    char path[4096];
    const int n = snprintf(path, sizeof(path), "%s/icosahedron.vdb", data_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) {
      fprintf(stderr, "TINYVDBIO_DATA_DIR path is too long\n");
      return 1;
    }
    if (!check_levelset_workers(path)) return 1;
  }
  return 0;
}
