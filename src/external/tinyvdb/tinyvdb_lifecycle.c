// Implementations of the lifecycle functions declared in tinyvdb_mesh.h.
// These were declared in the header but never defined elsewhere; consumers
// (Python wrapper, tests) all link against them.

#include "tinyvdb_mesh.h"
#include "tvdb_memory.h"
#include "tinyvdb_checked.h"
#include <stdlib.h>
#include <string.h>

void tvdb_triangle_mesh_init(tvdb_triangle_mesh* mesh) {
  mesh->vertices = NULL;
  mesh->vertex_count = 0;
  mesh->vertex_capacity = 0;
  mesh->faces = NULL;
  mesh->face_count = 0;
  mesh->face_capacity = 0;
}

void tvdb_triangle_mesh_free(tvdb_triangle_mesh* mesh) {
  if (mesh->vertices) free(mesh->vertices);
  if (mesh->faces) free(mesh->faces);
  mesh->vertices = NULL; mesh->vertex_count = 0; mesh->vertex_capacity = 0;
  mesh->faces = NULL;    mesh->face_count = 0;   mesh->face_capacity = 0;
}

void tvdb_triangle_mesh_init_arena(tvdb_triangle_mesh* mesh, tvdb_arena_allocator_t* arena) {
  // Arena-backed; freeing is done by the arena, not us.
  (void)arena;
  mesh->vertices = NULL; mesh->vertex_count = 0; mesh->vertex_capacity = 0;
  mesh->faces = NULL;    mesh->face_count = 0;   mesh->face_capacity = 0;
}

void tvdb_dense_grid_init(tvdb_dense_grid* grid, int nx, int ny, int nz) {
  if (!grid) return;
  grid->nx = nx; grid->ny = ny; grid->nz = nz;
  grid->ox = grid->oy = grid->oz = 0.0f;
  grid->voxel_size = 1.0f;
  size_t bytes;
  if (!tvdb_grid_bytes(nx, ny, nz, sizeof(float), &bytes)) {
    grid->data = NULL; grid->nx = grid->ny = grid->nz = 0; return;
  }
  grid->data = bytes ? (float*)malloc(bytes) : NULL;
  if (grid->data) memset(grid->data, 0, bytes);
}

// Like tvdb_dense_grid_init but leaves the data buffer uninitialized. Use only
// when every voxel will be written before any read (advection scratch buffers,
// BFECC correction fields).
void tvdb_dense_grid_init_uninit(tvdb_dense_grid* grid, int nx, int ny, int nz) {
  if (!grid) return;
  grid->nx = nx; grid->ny = ny; grid->nz = nz;
  grid->ox = grid->oy = grid->oz = 0.0f;
  grid->voxel_size = 1.0f;
  size_t bytes;
  if (!tvdb_grid_bytes(nx, ny, nz, sizeof(float), &bytes)) {
    grid->data = NULL; grid->nx = grid->ny = grid->nz = 0; return;
  }
  grid->data = bytes ? (float*)malloc(bytes) : NULL;
}

void tvdb_dense_grid_free(tvdb_dense_grid* grid) {
  if (grid->data) free(grid->data);
  grid->data = NULL;
  grid->nx = grid->ny = grid->nz = 0;
}

void tvdb_dense_grid_init_arena(tvdb_dense_grid* grid, int nx, int ny, int nz,
                                tvdb_arena_allocator_t* arena) {
  if (!grid) return;
  grid->nx = nx; grid->ny = ny; grid->nz = nz;
  grid->ox = grid->oy = grid->oz = 0.0f;
  grid->voxel_size = 1.0f;
  size_t bytes;
  if (!tvdb_grid_bytes(nx, ny, nz, sizeof(float), &bytes)) {
    grid->data = NULL; grid->nx = grid->ny = grid->nz = 0; return;
  }
  if (bytes == 0) { grid->data = NULL; return; }
  if (arena) {
    grid->data = (float*)tvdb_arena_alloc(arena, bytes);
  } else {
    grid->data = (float*)malloc(bytes);
  }
  if (grid->data) memset(grid->data, 0, bytes);
}
