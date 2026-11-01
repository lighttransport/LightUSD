/* Write public graded-mesh arrays for exact cross-build comparisons. */
#include "tinyvdb_tet_graded.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int field(const double p[3], float *v, void *user) {
  int shape = *(const int *)user;
  double x=p[0]-0.137, y=p[1]+0.091, z=p[2]-0.213;
  if (shape == 0) *v=(float)(hypot(hypot(x,y),z)-1);
  else if (shape == 1) {
    double a=fabs(x)-0.9, b=fabs(y)-0.7, c=fabs(z)-0.6;
    *v=(float)(hypot(hypot(fmax(a,0),fmax(b,0)),fmax(c,0))+fmin(fmax(a,fmax(b,c)),0));
  } else *v=(float)(hypot(hypot(x,y)-0.7,z)-0.25);
  return 1;
}
int main(int argc, char **argv) {
  const double lo[]={-1.5,-1.5,-1.5}, hi[]={1.5,1.5,1.5};
  tvdb_graded_stuffing_options_t options={0}; tvdb_tet_mesh mesh={0}; tvdb_error_t err;
  if (argc!=4) { fprintf(stderr,"usage: graded-identity shape spacing output\n"); return 2; }
  int shape=atoi(argv[1]); if (shape<0 || shape>2) return 2;
  options.fine_spacing=strtod(argv[2],NULL); options.coarse_spacing=0.8;
  options.grading=0.8; options.warp_alpha=0.2;
  if (tvdb_sdf_to_tet_mesh_graded(field,&shape,lo,hi,&options,&mesh,&err)!=TVDB_OK) {
    fprintf(stderr,"graded mesh: %s\n",err.message); return 1;
  }
  FILE *f=fopen(argv[3],"wb"); if (!f) { tvdb_tet_mesh_free(&mesh); return 1; }
  uint64_t counts[]={mesh.vertex_count,mesh.tet_count,mesh.boundary_count};
  int ok=fwrite(counts,sizeof(counts),1,f)==1 &&
    fwrite(mesh.positions,sizeof(float),3*mesh.vertex_count,f)==3*mesh.vertex_count &&
    fwrite(mesh.tets,sizeof(int32_t),4*mesh.tet_count,f)==4*mesh.tet_count &&
    fwrite(mesh.boundary,sizeof(int32_t),3*mesh.boundary_count,f)==3*mesh.boundary_count;
  if (fclose(f)) ok=0;
  printf("shape %d: %zu vertices, %zu tets, %zu boundary triangles\n",shape,mesh.vertex_count,mesh.tet_count,mesh.boundary_count);
  tvdb_tet_mesh_free(&mesh); return ok ? 0 : 1;
}
