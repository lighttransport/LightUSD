#pragma once

// CUDA and HIP compute without the rest of the GPU backend: a device handle on
// the runtime-loaded CUDA driver API + NVRTC or HIP + hiprtc (no SDK headers
// or link libraries), and the sparse level-set and isosurface-stuffing
// operations that run on it. Needs only the CPU modules it accelerates
// (tinyvdb_sparse_tools, tinyvdb_tetmesh, tinyvdb_winding, tinyvdb_mesh,
// tinyvdb_tree), so applications that vendor those can add this one file.
// tinyvdb_gpu.h offers the same operations on a tvdb_gpu_context_t.

#include <stddef.h>
#include <stdint.h>

#include "tinyvdb_io.h"           // tvdb_status_t, tvdb_error_t, tvdb_grid_t
#include "tinyvdb_sparse_tools.h" // tvdb_mesh_level_set_options_t
#include "tinyvdb_tetmesh.h"      // tvdb_tet_mesh, tvdb_stuffing_options_t
#include "tinyvdb_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TVDB_GPU_BACKEND_AUTO = 0,
  TVDB_GPU_BACKEND_VULKAN = 1,
  TVDB_GPU_BACKEND_CUDA = 2,
  // AMD ROCm through the HIP driver API (libamdhip64) and hiprtc, loaded at
  // runtime like CUDA. tvdb_gpu_context_t HIP contexts run the CUDA backend's
  // kernels and code paths: the kernel source is compiled by hiprtc instead of
  // NVRTC, so every operation documented for CUDA also runs on HIP, except
  // external-memory interop (tvdb_gpu_buffer_import), which is CUDA only.
  TVDB_GPU_BACKEND_HIP = 3
} tvdb_gpu_backend_t;

typedef struct tvdb_gpu_compute tvdb_gpu_compute_t;

// backend: TVDB_GPU_BACKEND_CUDA, TVDB_GPU_BACKEND_HIP, or AUTO (CUDA, then
// HIP). Returns TVDB_ERROR_UNIMPLEMENTED when the runtime libraries are
// missing and TVDB_ERROR_INVALID_ARGUMENT for a missing device or a Vulkan
// request. HIP libraries are searched on the loader path, then
// $ROCM_PATH/lib, /opt/rocm/lib and /opt/rocm/core/lib, and stay loaded for
// the process once loaded. Calls on one device must be serialized.
tvdb_status_t tvdb_gpu_compute_create(tvdb_gpu_backend_t backend, uint32_t device_index,
                                      tvdb_gpu_compute_t** out, tvdb_error_t* err);
void tvdb_gpu_compute_destroy(tvdb_gpu_compute_t* device);
tvdb_gpu_backend_t tvdb_gpu_compute_backend(const tvdb_gpu_compute_t* device);
const char* tvdb_gpu_compute_device_name(const tvdb_gpu_compute_t* device);

// ---- Sparse level sets and tetrahedral meshes ------------------------------
//
// GPU versions of tvdb_mesh_to_level_set, trilinear level-set sampling and
// tvdb_level_set_to_tet_mesh. The device does the per-point work: closest-face
// distance and winding number per candidate voxel, and the stuffing lattice's
// samples. The host keeps the sparse bookkeeping (seeding, dilation, band
// selection, tree build, flood fill, prune, stuffing assembly), so outputs
// follow the CPU functions' rules, deterministically and independent of the
// pool's workers. The first call on a device compiles the kernels (NVRTC or
// hiprtc; under a second).
//
// Precision. Kernels compile with floating-point contraction off.
//  * Distances repeat the CPU's float closest-point search on the same BVH
//    with the same tie-break, then its double distance; they match the CPU to
//    rounding (contraction on the CPU side is the only difference). A voxel
//    where the CPU's closest-point code would switch to its double fallback
//    is recomputed on the host.
//  * Winding numbers walk the CPU's tree in float (the CPU uses double), with
//    the same far-field rule. Only the |w| > 1/2 sign threshold uses them, so
//    signs agree except at voxels on the surface itself, where |w| is 1/2 and
//    the distance is zero to rounding.
//  * Lattice samples are the CPU's double trilinear interpolation, operation
//    for operation, so stuffing meshes match the CPU's.
tvdb_status_t tvdb_gpu_compute_mesh_to_level_set(tvdb_gpu_compute_t* device,
                                                 const tvdb_triangle_mesh* mesh,
                                                 const tvdb_mesh_level_set_options_t* options,
                                                 tvdb_thread_pool_t* pool, tvdb_grid_t* out,
                                                 tvdb_mesh_level_set_result_t* result,
                                                 tvdb_error_t* err);
// Trilinear samples of a float tree's stored values at world points (3
// doubles each), inactive tiles included and background outside, as
// tvdb_level_set_to_tet_mesh samples them.
tvdb_status_t tvdb_gpu_compute_level_set_sample(tvdb_gpu_compute_t* device,
                                                const tvdb_grid_t* grid, const double* points,
                                                size_t count, float* values, tvdb_error_t* err);
tvdb_status_t tvdb_gpu_compute_level_set_to_tet_mesh(tvdb_gpu_compute_t* device,
                                                     const tvdb_grid_t* level_set,
                                                     const tvdb_stuffing_options_t* options,
                                                     tvdb_thread_pool_t* pool, tvdb_tet_mesh* out,
                                                     tvdb_error_t* err);

#ifdef __cplusplus
}
#endif
