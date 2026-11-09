/* SPDX-License-Identifier: Apache-2.0 */
/* Small reproducible CUDA level-set benchmark. Profile externally with nsys.
 * Usage: profile-gpu-level-set [resolution 16..128] [fast|exact] [output.vdb]
 * Candidate count is capped at two million; no large assets are loaded.
 * GPU timings include transfer and host sparse construction, after warm-up. */
#define _POSIX_C_SOURCE 200809L
#include "tinyvdb_gpu_compute.h"
#include "tinyvdb_sparse_tree.h"
#include "tinyvdb_volume.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif
static double seconds(void) {
#if defined(_WIN32)
 LARGE_INTEGER count, frequency;
 QueryPerformanceCounter(&count); QueryPerformanceFrequency(&frequency);
 return (double)count.QuadPart / (double)frequency.QuadPart;
#else
 struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
 return t.tv_sec + t.tv_nsec*1e-9;
#endif
}
#define REQUIRE(expr) do { if (!(expr)) { fprintf(stderr,"%s: %s\n",#expr,error.message); return 1; } } while (0)
static unsigned ring(unsigned i,unsigned j) { return 2+(i-1)*48+j%48; }
int main(int argc,char **argv) {
 char *end=NULL; long resolution=argc>1?strtol(argv[1],&end,10):32;
 if(argc>4 || resolution<16 || resolution>128 || (argc>1 && (!end || *end)) ||
    (argc>2 && strcmp(argv[2],"fast") && strcmp(argv[2],"exact"))) {
  fprintf(stderr,"usage: %s [16..128] [fast|exact] [output.vdb]\n",argv[0]); return 2;
 }
 tvdb_error_t error={0};
 tvdb_vec3f vertices[1106];tvdb_triangle faces[2208];size_t nv=2,nf=0;
 vertices[0]=(tvdb_vec3f){0,0,1};vertices[1]=(tvdb_vec3f){0,0,-1};
 const double pi=3.14159265358979323846;
 for(unsigned i=1;i<24;++i) for(unsigned j=0;j<48;++j) {
  double t=pi*i/24,p=2*pi*j/48;
  vertices[nv++]=(tvdb_vec3f){(float)(sin(t)*cos(p)),(float)(sin(t)*sin(p)),(float)cos(t)};
 }
 for(unsigned j=0;j<48;++j) {
  faces[nf++]=(tvdb_triangle){0,ring(1,j),ring(1,j+1)};
  faces[nf++]=(tvdb_triangle){1,ring(23,j+1),ring(23,j)};
 }
 for(unsigned i=1;i<23;++i) for(unsigned j=0;j<48;++j) {
  faces[nf++]=(tvdb_triangle){ring(i,j),ring(i+1,j),ring(i+1,j+1)};
  faces[nf++]=(tvdb_triangle){ring(i,j),ring(i+1,j+1),ring(i,j+1)};
 }
 tvdb_triangle_mesh mesh={vertices,nv,nv,faces,nf,nf};
 tvdb_mesh_level_set_options_t options={0};
 options.voxel_size=(float)(2.0/resolution);
 options.exterior_width=options.interior_width=3*options.voxel_size;
 options.max_voxels=2000000; options.grid_name="sdf";
 options.sign=argc>2 && !strcmp(argv[2],"exact")?TVDB_LEVEL_SET_SIGN_WINDING_EXACT:TVDB_LEVEL_SET_SIGN_WINDING_FAST;
 tvdb_gpu_compute_t *device=NULL;tvdb_thread_pool_t *pool=NULL;
 REQUIRE(tvdb_thread_pool_create(1,&pool,&error)==TVDB_OK);
 REQUIRE(tvdb_gpu_compute_create(TVDB_GPU_BACKEND_CUDA,0,&device,&error)==TVDB_OK);
 printf("device=%s resolution=%ld sign=%s vertices=%zu faces=%zu\n",
        tvdb_gpu_compute_device_name(device),resolution,options.sign?"exact":"fast",nv,nf);
 // Compile and warm up outside the measured repeats, using a coarse grid.
 tvdb_grid_t warm={0};tvdb_mesh_level_set_options_t coarse=options;coarse.voxel_size=.25f;
 coarse.exterior_width=coarse.interior_width=.75f;
 REQUIRE(tvdb_gpu_compute_mesh_to_level_set(device,&mesh,&coarse,pool,&warm,NULL,&error)==TVDB_OK);
 tvdb_grid_destroy_owned(&warm);
 tvdb_grid_t cpu={0},gpu={0};tvdb_mesh_level_set_result_t cr={0},gr={0};
 double start=seconds();
 REQUIRE(tvdb_mesh_to_level_set(&mesh,&options,pool,&cpu,&cr,&error)==TVDB_OK);
 printf("cpu_seconds=%.6f candidates=%zu active=%zu\n",seconds()-start,cr.candidate_voxels,cr.active_voxels);
 for(int repeat=0;repeat<3;++repeat) {
  tvdb_grid_destroy_owned(&gpu);
  start=seconds();
  REQUIRE(tvdb_gpu_compute_mesh_to_level_set(device,&mesh,&options,pool,&gpu,&gr,&error)==TVDB_OK);
  printf("cuda_repeat=%d seconds=%.6f candidates=%zu active=%zu\n",repeat,seconds()-start,gr.candidate_voxels,gr.active_voxels);
  REQUIRE(gr.candidate_voxels==cr.candidate_voxels && gr.active_voxels==cr.active_voxels);
 }
 double points[4096*3];float cv[4096],gv[4096];size_t at=0;
 for(int z=0;z<16;++z)for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
  points[at++]=-.125*15+.25*x;points[at++]=-.125*15+.25*y;points[at++]=-.125*15+.25*z;
 }
 tvdb_volume_sampler_t *sampler=NULL;
 REQUIRE(tvdb_volume_sampler_create(&cpu,&sampler,&error)==TVDB_OK);
 REQUIRE(tvdb_volume_sample_world(sampler,points,4096,TVDB_VOLUME_FILTER_TRILINEAR,cv,&error)==TVDB_OK);
 REQUIRE(tvdb_gpu_compute_level_set_sample(device,&gpu,points,4096,gv,&error)==TVDB_OK);
 double maximum=0;
 for(int k=0;k<4096;++k) { double difference=fabs((double)cv[k]-gv[k]);REQUIRE(isfinite(gv[k]));if(difference>maximum)maximum=difference; }
 printf("samples=4096 max_cpu_cuda_difference=%.9g\n",maximum);REQUIRE(maximum<1e-5);
 if(argc>3) {
  tvdb_file_t file;tvdb_file_init_for_write(&file,NULL);file.grids=&gpu;file.num_grids=1;
  uint8_t *data=NULL;size_t size=0;
  REQUIRE(tvdb_write_to_memory(&file,TVDB_COMPRESS_ZIP,0,&data,&size,&error)==TVDB_OK);
  FILE *f=fopen(argv[3],"wb");REQUIRE(f && fwrite(data,1,size,f)==size);REQUIRE(fclose(f)==0);free(data);
 }
 tvdb_volume_sampler_destroy(sampler);tvdb_grid_destroy_owned(&cpu);tvdb_grid_destroy_owned(&gpu);
 tvdb_thread_pool_destroy(pool);tvdb_gpu_compute_destroy(device);return 0;
}
