#include "tinyvdb_tet_graded_internal.h"
#include "tinyvdb_tet_graded.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int sphere(const double p[3], float *v, void *user) {
  (void)user; *v = (float)(hypot(hypot(p[0],p[1]),p[2]) - 1); return 1;
}
static int shape(const double p[3], float *v, void *user) {
  int mode = *(const int *)user;
  double x = p[0]-0.137, y = p[1]+0.091, z = p[2]-0.213;
  if (mode == 0) *v = (float)(hypot(hypot(x,y),z)-1);
  else if (mode == 1) {
    double a = fabs(x)-0.9, b = fabs(y)-0.7, c = fabs(z)-0.6;
    *v = (float)(hypot(hypot(fmax(a,0),fmax(b,0)),fmax(c,0)) +
                 fmin(fmax(a,fmax(b,c)),0));
  } else *v = (float)(hypot(hypot(x,y)-0.7,z)-0.25);
  return 1;
}
static int small_sphere(const double p[3], float *v, void *user) {
  (void)user;
  *v = (float)(hypot(hypot(p[0]-0.61,p[1]-0.77),p[2]+0.52)-0.05);
  return 1;
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
static int euler(const tvdb_tet_mesh *m) {
  char *used = (char *)calloc(m->vertex_count ? m->vertex_count : 1, 1);
  size_t vertices = 0;
  if (!used) return -999;
  for (size_t i = 0; i < 3*m->boundary_count; ++i) {
    int32_t v = m->boundary[i];
    if (v < 0 || (size_t)v >= m->vertex_count) { free(used); return -999; }
    if (!used[v]) { used[v] = 1; ++vertices; }
  }
  free(used);
  return (int)vertices - (int)(3*m->boundary_count/2) + (int)m->boundary_count;
}
int main(void) {
  const double lo[3]={-2,-2,-2}, hi[3]={2,2,2}; const size_t cells[3]={2,2,2};
  tvdb_tet_refine_graph *g = NULL; tvdb_error_t err; tvdb_tet_mesh m={0}, raw={0};
  tvdb_tet_mesh_quality_t q, raw_q;
  double torus_error = 0;
  CHECK(tvdb_tet_refine_box(lo,hi,cells,100000,500000,&g,&err)==TVDB_OK);
  CHECK(tvdb_tet_refine_adapt_sdf(g,sphere,NULL,0,0.5,2.5,0.8,&err)==TVDB_OK);
  CHECK(tvdb_tet_graded_stencil(g,sphere,NULL,0,0,500000,&raw,&err)==TVDB_OK);
  CHECK(closed(&raw) && tvdb_tet_mesh_quality(&raw,&raw_q,&err)==TVDB_OK && !raw_q.inverted);
  CHECK(tvdb_tet_graded_stencil(g,sphere,NULL,0,0.2,500000,&m,&err)==TVDB_OK);
  CHECK(m.tet_count>0 && m.boundary_count>0 && closed(&m));
  CHECK(tvdb_tet_mesh_quality(&m,&q,&err)==TVDB_OK && !q.inverted);
  CHECK(q.min_dihedral > 10 && q.max_dihedral < 170);
  CHECK(fabs(q.volume-4.1887902047863905)<0.8);
  printf("graded sphere: %zu tets, %zu faces, volume %.6f, dihedral %.3f..%.3f\n",
         m.tet_count,m.boundary_count,q.volume,q.min_dihedral,q.max_dihedral);
  {
    float *saved = m.positions; size_t count = m.tet_count;
    CHECK(tvdb_tet_graded_stencil(g,sphere,NULL,0,0.5,500000,&m,&err)==TVDB_ERROR_INVALID_ARGUMENT);
    CHECK(m.positions == saved && m.tet_count == count);
  }
  tvdb_tet_mesh_free(&raw); tvdb_tet_mesh_free(&m); tvdb_tet_refine_destroy(g);
  for (int mode = 0; mode < 3; ++mode) {
    const double expected[] = {4.1887902047863905, 3.024, 0.8635903850953188};
    const double tolerance[] = {0.25, 0.25, 0.30};
    const int expected_euler[] = {2,2,0};
    size_t near_n = 0, far_n = 0; double near_sum = 0, far_sum = 0;
    g = NULL;
    CHECK(tvdb_tet_refine_box(lo,hi,cells,200000,600000,&g,&err)==TVDB_OK);
    CHECK(tvdb_tet_refine_adapt_sdf(g,shape,&mode,0,0.4,2.5,0.8,&err)==TVDB_OK);
    const double *p = tvdb_tet_refine_positions(g);
    const uint32_t *t = tvdb_tet_refine_tets(g);
    for (size_t i = 0; i < tvdb_tet_refine_tet_count(g); ++i)
      for (int a = 0; a < 4; ++a) for (int b = a+1; b < 4; ++b) {
        const double *x = p+3*t[4*i+a], *y = p+3*t[4*i+b]; double mid[3]; float f;
        for (int d = 0; d < 3; ++d) mid[d] = 0.5*(x[d]+y[d]);
        CHECK(shape(mid,&f,&mode));
        double length = hypot(hypot(x[0]-y[0],x[1]-y[1]),x[2]-y[2]);
        if (fabs(f) < 0.3) { near_sum += length; ++near_n; }
        if (fabs(f) > 1.4) { far_sum += length; ++far_n; }
      }
    CHECK(near_n && far_n && near_sum/near_n < 0.6*far_sum/far_n);
    CHECK(tvdb_tet_graded_stencil(g,shape,&mode,0,0.2,600000,&m,&err)==TVDB_OK);
    CHECK(m.tet_count && closed(&m) && euler(&m)==expected_euler[mode]);
    CHECK(tvdb_tet_mesh_quality(&m,&q,&err)==TVDB_OK && !q.inverted);
    CHECK(fabs(q.volume-expected[mode]) < tolerance[mode]*expected[mode]);
    if (mode == 2) torus_error = fabs(q.volume-expected[mode]);
    CHECK(q.min_dihedral > 5 && q.max_dihedral < 170);
    CHECK(tvdb_tet_graded_stencil(g,shape,&mode,0,0.2,600000,&raw,&err)==TVDB_OK);
    CHECK(raw.vertex_count == m.vertex_count && raw.tet_count == m.tet_count &&
          raw.boundary_count == m.boundary_count);
    CHECK(!memcmp(raw.positions,m.positions,3*m.vertex_count*sizeof(float)) &&
          !memcmp(raw.tets,m.tets,4*m.tet_count*sizeof(int32_t)) &&
          !memcmp(raw.boundary,m.boundary,3*m.boundary_count*sizeof(int32_t)));
    printf("graded shape %d: near %.3f far %.3f, volume %.6f, angles %.3f..%.3f\n",
           mode, near_sum/near_n, far_sum/far_n, q.volume, q.min_dihedral, q.max_dihedral);
    tvdb_tet_mesh_free(&raw); tvdb_tet_mesh_free(&m); tvdb_tet_refine_destroy(g);
  }
  {
    const double fine_sizes[] = {0.3, 0.2, 0.15};
    const double exact_volume = 0.8635903850953188;
    int torus = 2;
    for (size_t i = 0; i < 3; ++i) {
      g = NULL;
      CHECK(tvdb_tet_refine_box(lo,hi,cells,200000,600000,&g,&err)==TVDB_OK);
      CHECK(tvdb_tet_refine_adapt_sdf(g,shape,&torus,0,fine_sizes[i],2.5,0.8,&err)==TVDB_OK);
      CHECK(tvdb_tet_graded_stencil(g,shape,&torus,0,0.2,600000,&m,&err)==TVDB_OK);
      CHECK(m.tet_count && closed(&m) && euler(&m)==0);
      CHECK(tvdb_tet_mesh_quality(&m,&q,&err)==TVDB_OK && !q.inverted);
      CHECK(fabs(q.volume-exact_volume) < torus_error);
      torus_error = fabs(q.volume-exact_volume);
      printf("torus fine %.2f: %zu tets, volume %.6f\n",fine_sizes[i],m.tet_count,q.volume);
      tvdb_tet_mesh_free(&m); tvdb_tet_refine_destroy(g);
    }
    CHECK(torus_error < 0.03*exact_volume);
  }
  {
    const double exact_volume = 0.0005235987755982988;
    g = NULL;
    CHECK(tvdb_tet_refine_box(lo,hi,cells,200000,600000,&g,&err)==TVDB_OK);
    CHECK(tvdb_tet_refine_adapt_sdf(g,small_sphere,NULL,0,0.05,2.5,0.8,&err)==TVDB_OK);
    CHECK(tvdb_tet_graded_stencil(g,small_sphere,NULL,0,0.2,600000,&m,&err)==TVDB_OK);
    CHECK(m.tet_count && closed(&m) && euler(&m)==2);
    CHECK(tvdb_tet_mesh_quality(&m,&q,&err)==TVDB_OK && !q.inverted);
    CHECK(fabs(q.volume-exact_volume) < 0.3*exact_volume);
    tvdb_tet_mesh_free(&m); tvdb_tet_refine_destroy(g);
  }
  {
    const double min_box[] = {-1,-1,-1}, max_box[] = {1,1,1};
    tvdb_graded_stuffing_options_t options = {0};
    options.fine_spacing = 0.4; options.coarse_spacing = 1.0;
    options.grading = 0.8; options.warp_alpha = 0.2;
    CHECK(tvdb_sdf_to_tet_mesh_graded(sphere,NULL,min_box,max_box,&options,&m,&err)==TVDB_OK);
    CHECK(m.tet_count && closed(&m) && euler(&m)==2);
    CHECK(tvdb_tet_mesh_quality(&m,&q,&err)==TVDB_OK && !q.inverted &&
          fabs(q.volume-4.1887902047863905)<0.5);
    float *saved = m.positions; size_t count = m.tet_count;
    options.max_background_tets = 10;
    CHECK(tvdb_sdf_to_tet_mesh_graded(sphere,NULL,min_box,max_box,&options,&m,&err)!=TVDB_OK);
    CHECK(m.positions == saved && m.tet_count == count);
    options.max_background_tets = 0; options.grading = 0;
    CHECK(tvdb_sdf_to_tet_mesh_graded(sphere,NULL,min_box,max_box,&options,&m,&err)==TVDB_ERROR_INVALID_ARGUMENT);
    CHECK(m.positions == saved && m.tet_count == count);
    tvdb_tet_mesh_free(&m);
  }
  return 0;
}
