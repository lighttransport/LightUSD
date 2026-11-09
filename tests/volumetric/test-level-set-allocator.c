/* SPDX-License-Identifier: Apache-2.0 */
/* Allocation-failure sweep through the complete CPU SDF construction closure. */
#include "tinyvdb_sparse_tools.h"
#include "tinyvdb_sparse_tree.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
/* Spelled out: MSVC's C headers do not declare max_align_t. */
typedef union { size_t bytes; long long ll; long double ld; void *p; } Header;
typedef struct { size_t used, peak, limit, calls, fail_at; } Budget;
static void *allocate(size_t n, void *ctx) {
  Budget *b = ctx;
  if (++b->calls == b->fail_at || n > SIZE_MAX - sizeof(Header)) return NULL;
  n += sizeof(Header);
  if (n > b->limit - b->used) return NULL;
  Header *h = malloc(n);
  if (!h) return NULL;
  h->bytes = n; b->used += n;
  if (b->used > b->peak) b->peak = b->used;
  return h + 1;
}
static void release(void *p, size_t n, void *ctx) {
  (void)n;
  if (p) { Header *h = (Header *)p - 1; ((Budget *)ctx)->used -= h->bytes; free(h); }
}
static void *resize(void *p, size_t old, size_t n, void *ctx) {
  (void)old;
  if (!p) return allocate(n, ctx);
  Header *h = (Header *)p - 1;
  size_t previous = h->bytes - sizeof(Header);
  void *q = allocate(n, ctx);
  if (q) { memcpy(q, p, n < previous ? n : previous); release(p, 0, ctx); }
  return q;
}
static tvdb_vec3f vertices[] = {{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
static tvdb_triangle faces[] = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
static tvdb_triangle_mesh mesh = {vertices,4,4,faces,4,4};
static tvdb_mesh_level_set_options_t options = {
 .voxel_size=.25f, .exterior_width=.5f, .interior_width=.5f,
 .max_voxels=100000, .grid_name="allocator-test"};
static void serialize(tvdb_grid_t *g, uint8_t **bytes, size_t *size) {
 tvdb_file_t f; tvdb_error_t e;
 tvdb_file_init_for_write(&f, NULL); f.grids = g; f.num_grids = 1;
 CHECK(tvdb_write_to_memory(&f, TVDB_COMPRESS_ZIP | TVDB_COMPRESS_ACTIVE_MASK,
                           0, bytes, size, &e) == TVDB_OK);
}
static void compare(tvdb_grid_t *a, tvdb_grid_t *b) {
 uint8_t *x=NULL,*y=NULL; size_t nx=0,ny=0;
 serialize(a,&x,&nx); serialize(b,&y,&ny);
 CHECK(nx==ny && memcmp(x,y,nx)==0); free(x);free(y);
}
typedef struct { tvdb_grid_t *baseline; } ConcurrentJob;
static void concurrent_build(size_t begin, size_t end, void *ctx) {
 ConcurrentJob *job = ctx;
 tvdb_thread_pool_t *serial = NULL; tvdb_error_t err;
 CHECK(tvdb_thread_pool_create(1, &serial, &err) == TVDB_OK);
 for (size_t i = begin; i < end; ++i) {
  Budget b = {0}; b.limit = i == 0 ? 1 : SIZE_MAX;
  tvdb_allocator_t a = {allocate, resize, release, &b};
  tvdb_grid_t output = {0};
  tvdb_status_t st = tvdb_mesh_to_level_set_with_allocator(
      &mesh, &options, serial, &output, NULL, &err, &a);
  if (i == 0) CHECK(st != TVDB_OK);
  else { CHECK(st == TVDB_OK); compare(job->baseline, &output); }
  tvdb_grid_destroy_owned(&output); CHECK(b.used == 0);
 }
 tvdb_thread_pool_destroy(serial);
}
int main(void) {
 tvdb_error_t err; tvdb_grid_t baseline={0}, output={0};
 tvdb_thread_pool_t *pool=NULL;
 CHECK(tvdb_thread_pool_create(1,&pool,&err)==TVDB_OK);
 CHECK(tvdb_mesh_to_level_set(&mesh,&options,pool,&baseline,NULL,&err)==TVDB_OK);
 tvdb_triangle_mesh oversized = mesh;
 oversized.vertex_count = SIZE_MAX;
 tvdb_tree_node_t *original_nodes = baseline.tree.nodes;
 CHECK(tvdb_mesh_to_level_set(&oversized,&options,pool,&baseline,NULL,&err)!=TVDB_OK);
 CHECK(baseline.tree.nodes==original_nodes);
 oversized = mesh; oversized.face_count = SIZE_MAX;
 CHECK(tvdb_mesh_to_level_set(&oversized,&options,pool,&baseline,NULL,&err)!=TVDB_OK);
 CHECK(baseline.tree.nodes==original_nodes);
 Budget b={0}; b.limit=SIZE_MAX;
 tvdb_allocator_t a={allocate,resize,release,&b};
 CHECK(tvdb_mesh_to_level_set_with_allocator(&mesh,&options,pool,&output,NULL,&err,&a)==TVDB_OK);
 CHECK(b.used>0 && b.peak>b.used); compare(&baseline,&output);
 size_t calls=b.calls, peak=b.peak;
 tvdb_grid_destroy_owned(&output); CHECK(b.used==0);
 // Every failed allocation preserves both the existing grid and the report.
 for (size_t n=1;n<=calls;++n) {
  memset(&b,0,sizeof(b)); b.limit=SIZE_MAX; b.fail_at=n;
  tvdb_mesh_level_set_result_t result={123,456,789,42}, previous=result;
  tvdb_tree_node_t *original=baseline.tree.nodes;
  CHECK(tvdb_mesh_to_level_set_with_allocator(&mesh,&options,pool,&baseline,&result,&err,&a)!=TVDB_OK);
  CHECK(b.used==0 && baseline.tree.nodes==original);
  CHECK(memcmp(&result,&previous,sizeof(result))==0);
 }
 memset(&b,0,sizeof(b)); b.limit=peak-1;
 CHECK(tvdb_mesh_to_level_set_with_allocator(&mesh,&options,pool,&output,NULL,&err,&a)!=TVDB_OK);
 CHECK(b.used==0 && b.peak<=b.limit);
 memset(&b,0,sizeof(b)); b.limit=peak;
 CHECK(tvdb_mesh_to_level_set_with_allocator(&mesh,&options,pool,&output,NULL,&err,&a)==TVDB_OK);
 compare(&baseline,&output); tvdb_grid_destroy_owned(&output); CHECK(b.used==0);
 tvdb_thread_pool_destroy(pool);
 CHECK(tvdb_thread_pool_create(2,&pool,&err)==TVDB_OK);
 memset(&b,0,sizeof(b)); b.limit=SIZE_MAX;
 CHECK(tvdb_mesh_to_level_set_with_allocator(&mesh,&options,pool,&output,NULL,&err,&a)==TVDB_OK);
 compare(&baseline,&output); tvdb_grid_destroy_owned(&output); CHECK(b.used==0);
 ConcurrentJob job = {&baseline};
 CHECK(tvdb_thread_pool_for(pool, 0, 2, 1, concurrent_build, &job, &err) == TVDB_OK);
 tvdb_grid_destroy_owned(&baseline); tvdb_thread_pool_destroy(pool);
 printf("CPU SDF allocator: swept %zu failures, peak %zu bytes; identity preserved\n",calls,peak);
 return 0;
}
