#include "tinyvdb_tet_graded_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int sphere(const double p[3], float *v, void *user) {
  (void)user; *v = (float)(hypot(hypot(p[0],p[1]),p[2]) - 1); return 1;
}
typedef struct { int32_t a, b; int sign; } edge;
static int cmp(const void *x, const void *y) {
  const edge *a = (const edge *)x, *b = (const edge *)y;
  if (a->a != b->a) return a->a < b->a ? -1 : 1;
  return (a->b > b->b) - (a->b < b->b);
}
static int closed(const tvdb_tet_mesh *m) {
  size_t n = 3*m->boundary_count; edge *e = (edge *)malloc(n*sizeof(*e));
  if (!e) return 0;
  for (size_t i=0; i<m->boundary_count; ++i) for (size_t j=0; j<3; ++j) {
    int32_t a=m->boundary[3*i+j], b=m->boundary[3*i+(j+1)%3];
    e[3*i+j] = a < b ? (edge){a,b,1} : (edge){b,a,-1};
  }
  qsort(e,n,sizeof(*e),cmp);
  for (size_t i=0; i<n; i+=2) {
    if (i+1 >= n || cmp(e+i,e+i+1) || e[i].sign == e[i+1].sign ||
        (i+2<n && !cmp(e+i,e+i+2))) { free(e); return 0; }
  }
  free(e); return 1;
}
int main(void) {
  const double lo[3]={-2,-2,-2}, hi[3]={2,2,2}; const size_t cells[3]={2,2,2};
  tvdb_tet_refine_graph *g = NULL; tvdb_error_t err; tvdb_tet_mesh m={0}, raw={0};
  tvdb_tet_mesh_quality_t q, raw_q;
  CHECK(tvdb_tet_refine_box(lo,hi,cells,100000,500000,&g,&err)==TVDB_OK);
  CHECK(tvdb_tet_refine_adapt_sdf(g,sphere,NULL,0,0.5,2.5,0.8,&err)==TVDB_OK);
  CHECK(tvdb_tet_graded_stencil(g,sphere,NULL,0,0,500000,&raw,&err)==TVDB_OK);
  CHECK(closed(&raw) && tvdb_tet_mesh_quality(&raw,&raw_q,&err)==TVDB_OK && !raw_q.inverted);
  CHECK(tvdb_tet_graded_stencil(g,sphere,NULL,0,0.2,500000,&m,&err)==TVDB_OK);
  CHECK(m.tet_count>0 && m.boundary_count>0 && closed(&m));
  CHECK(tvdb_tet_mesh_quality(&m,&q,&err)==TVDB_OK && !q.inverted);
  CHECK(q.min_dihedral > raw_q.min_dihedral);
  CHECK(fabs(q.volume-4.1887902047863905)<0.8);
  printf("graded sphere: %zu tets, %zu faces, volume %.6f, dihedral %.3f..%.3f\n",
         m.tet_count,m.boundary_count,q.volume,q.min_dihedral,q.max_dihedral);
  {
    float *saved = m.positions; size_t count = m.tet_count;
    CHECK(tvdb_tet_graded_stencil(g,sphere,NULL,0,0.5,500000,&m,&err)==TVDB_ERROR_INVALID_ARGUMENT);
    CHECK(m.positions == saved && m.tet_count == count);
  }
  tvdb_tet_mesh_free(&raw); tvdb_tet_mesh_free(&m); tvdb_tet_refine_destroy(g); return 0;
}
