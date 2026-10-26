/* CUDA and HIP compute (tinyvdb_gpu_compute.h): the runtime driver loaders,
 * shared with tinyvdb_gpu.c, a lean device handle, and the sparse level-set
 * and isosurface-stuffing kernels. The host keeps the sparse bookkeeping, so
 * results follow the CPU functions; see the header for the precision notes. */
#include "tinyvdb_gpu_compute.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "tinyvdb_gpu_compute_internal.h"
#include "tinyvdb_nanovdb_device.h" /* sampler body (host test compiles it too) */
#include "tinyvdb_tree_internal.h"
#include "tinyvdb_winding.h"
/* Provided by tinyvdb_to_nanovdb.c (the nanovdb module): the level-set sampler
 * reads a NanoVDB buffer, so this module needs the converter. Declared here
 * rather than by including tinyvdb_nanovdb.h, which would pull the whole
 * NanoVDB surface into the vendored GPU module. */
struct tvdb_grid;
tvdb_status_t tvdb_grid_to_nanovdb(const struct tvdb_grid* grid, uint8_t** out_data,
                                   size_t* out_size, tvdb_error_t* err);

/* Generated at configure time from the header above: the same text as a
 * string for NVRTC/hiprtc. */
#include "tinyvdb_nanovdb_device_src.inc"

static void tvdb_compute_error(tvdb_error_t* err, tvdb_status_t st, const char* msg) {
  if (!err) return;
  err->status = st;
  err->byte_offset = 0;
  err->grid_index = -1;
  snprintf(err->message, sizeof(err->message), "%s", msg ? msg : "");
}

/* ---- Runtime loading ---------------------------------------------------- */
static void* tvdb_dyn_open(const char* name) {
#if defined(_WIN32)
  return (void*)LoadLibraryA(name);
#else
  return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void* tvdb_dyn_sym(void* lib, const char* name) {
#if defined(_WIN32)
  return (void*)GetProcAddress((HMODULE)lib, name);
#else
  return dlsym(lib, name);
#endif
}
static void tvdb_dyn_close(void* lib) {
  if (!lib) return;
#if defined(_WIN32)
  FreeLibrary((HMODULE)lib);
#else
  dlclose(lib);
#endif
}
static void* tvdb_load_first_library(const char* const* names, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    void* lib = tvdb_dyn_open(names[i]);
    if (lib) return lib;
  }
  return NULL;
}

int tvdb_gpu_driver_load_cuda(tvdb_cuda_table* cu) {
  memset(cu, 0, sizeof(*cu));
#if defined(_WIN32)
  const char* cuda_names[] = {"nvcuda.dll"};
  const char* nvrtc_names[] = {"nvrtc64_130_0.dll", "nvrtc64_120_0.dll", "nvrtc64_112_0.dll", "nvrtc64_102_0.dll"};
#elif defined(__APPLE__)
  const char* cuda_names[] = {"libcuda.dylib"};
  const char* nvrtc_names[] = {"libnvrtc.dylib"};
#else
  const char* cuda_names[] = {"libcuda.so.1", "libcuda.so"};
  const char* nvrtc_names[] = {"libnvrtc.so.13", "libnvrtc.so.12", "libnvrtc.so.11.2", "libnvrtc.so"};
#endif
  cu->libcuda = tvdb_load_first_library(cuda_names, sizeof(cuda_names) / sizeof(cuda_names[0]));
  if (!cu->libcuda) return 0;
  cu->libnvrtc = tvdb_load_first_library(nvrtc_names, sizeof(nvrtc_names) / sizeof(nvrtc_names[0]));
  if (!cu->libnvrtc) {
    tvdb_dyn_close(cu->libcuda);
    memset(cu, 0, sizeof(*cu));
    return 0;
  }
#define TVDB_CUDA_SYM(field, name) do { \
  cu->field = (void*)tvdb_dyn_sym(cu->libcuda, name); \
  if (!cu->field) goto fail; \
} while (0)
#define TVDB_NVRTC_SYM(field, name) do { \
  cu->field = (void*)tvdb_dyn_sym(cu->libnvrtc, name); \
  if (!cu->field) goto fail; \
} while (0)
  TVDB_CUDA_SYM(cuInit, "cuInit");
  TVDB_CUDA_SYM(cuDeviceGetCount, "cuDeviceGetCount");
  TVDB_CUDA_SYM(cuDeviceGet, "cuDeviceGet");
  TVDB_CUDA_SYM(cuDeviceGetName, "cuDeviceGetName");
  TVDB_CUDA_SYM(cuCtxCreate, "cuCtxCreate_v2");
  TVDB_CUDA_SYM(cuCtxDestroy, "cuCtxDestroy_v2");
  TVDB_CUDA_SYM(cuCtxSynchronize, "cuCtxSynchronize");
  TVDB_CUDA_SYM(cuMemAlloc, "cuMemAlloc_v2");
  TVDB_CUDA_SYM(cuMemFree, "cuMemFree_v2");
  TVDB_CUDA_SYM(cuMemcpyHtoD, "cuMemcpyHtoD_v2");
  TVDB_CUDA_SYM(cuMemcpyDtoH, "cuMemcpyDtoH_v2");
  TVDB_CUDA_SYM(cuMemsetD32, "cuMemsetD32_v2");
  TVDB_CUDA_SYM(cuModuleLoadData, "cuModuleLoadData");
  TVDB_CUDA_SYM(cuModuleUnload, "cuModuleUnload");
  TVDB_CUDA_SYM(cuModuleGetFunction, "cuModuleGetFunction");
  TVDB_CUDA_SYM(cuLaunchKernel, "cuLaunchKernel");
  cu->cuGetErrorString = (void*)tvdb_dyn_sym(cu->libcuda, "cuGetErrorString");
  cu->cuDeviceGetUuid = (void*)tvdb_dyn_sym(cu->libcuda, "cuDeviceGetUuid");
  // External-memory interop (optional; absent on very old drivers).
  cu->cuImportExternalMemory = (void*)tvdb_dyn_sym(cu->libcuda, "cuImportExternalMemory");
  cu->cuExternalMemoryGetMappedBuffer = (void*)tvdb_dyn_sym(cu->libcuda, "cuExternalMemoryGetMappedBuffer");
  cu->cuDestroyExternalMemory = (void*)tvdb_dyn_sym(cu->libcuda, "cuDestroyExternalMemory");
  TVDB_NVRTC_SYM(nvrtcCreateProgram, "nvrtcCreateProgram");
  TVDB_NVRTC_SYM(nvrtcCompileProgram, "nvrtcCompileProgram");
  TVDB_NVRTC_SYM(nvrtcGetPTXSize, "nvrtcGetPTXSize");
  TVDB_NVRTC_SYM(nvrtcGetPTX, "nvrtcGetPTX");
  TVDB_NVRTC_SYM(nvrtcGetProgramLogSize, "nvrtcGetProgramLogSize");
  TVDB_NVRTC_SYM(nvrtcGetProgramLog, "nvrtcGetProgramLog");
  TVDB_NVRTC_SYM(nvrtcDestroyProgram, "nvrtcDestroyProgram");
  cu->nvrtcGetErrorString = (void*)tvdb_dyn_sym(cu->libnvrtc, "nvrtcGetErrorString");
#undef TVDB_CUDA_SYM
#undef TVDB_NVRTC_SYM
  return 1;
fail:
  tvdb_dyn_close(cu->libnvrtc);
  tvdb_dyn_close(cu->libcuda);
  memset(cu, 0, sizeof(*cu));
  return 0;
}


/* libamdhip64 and libhiprtc into the CUDA table (see tvdb_cuda_table). ROCm
 * does not register its library directory with the loader on every install,
 * so the usual locations are tried after the plain names. */
static void* tvdb_load_rocm_library(const char* const* names, size_t count) {
  void* lib = tvdb_load_first_library(names, count);
#if !defined(_WIN32)
  if (lib) return lib;
  const char* env = getenv("ROCM_PATH");
  const char* dirs[] = {env, "/opt/rocm/lib", "/opt/rocm/core/lib"};
  for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]) && !lib; ++d) {
    if (!dirs[d] || !dirs[d][0]) continue;
    for (size_t i = 0; i < count && !lib; ++i) {
      char path[1024];
      snprintf(path, sizeof(path), d == 0 ? "%s/lib/%s" : "%s/%s", dirs[d], names[i]);
      lib = tvdb_dyn_open(path);
    }
  }
#endif
  return lib;
}

/* ROCm must stay loaded once loaded: unloading libamdhip64 drops comgr's
 * LLVM command-line options while libLLVM keeps them registered, so a second
 * load crashes inside LLVM. The handles are cached for the process and HIP
 * tables never close them (tvdb_gpu_driver_close). A racing first load just
 * takes one more dlopen reference. */
static void* tvdb_hip_lib_cache;
static void* tvdb_hiprtc_lib_cache;

void tvdb_gpu_driver_close(tvdb_cuda_table* cu) {
  if (!cu->hip) {
    tvdb_dyn_close(cu->libnvrtc);
    tvdb_dyn_close(cu->libcuda);
  }
  memset(cu, 0, sizeof(*cu));
}

int tvdb_gpu_driver_load_hip(tvdb_cuda_table* cu) {
#if defined(_WIN32)
  const char* hip_names[] = {"amdhip64_7.dll", "amdhip64_6.dll", "amdhip64.dll"};
  const char* rtc_names[] = {"hiprtc0700.dll", "hiprtc0605.dll", "hiprtc0604.dll", "hiprtc.dll"};
#else
  const char* hip_names[] = {"libamdhip64.so.7", "libamdhip64.so.6", "libamdhip64.so"};
  const char* rtc_names[] = {"libhiprtc.so.7", "libhiprtc.so.6", "libhiprtc.so"};
#endif
  memset(cu, 0, sizeof(*cu));
  if (!tvdb_hip_lib_cache)
    tvdb_hip_lib_cache = tvdb_load_rocm_library(hip_names, sizeof(hip_names) / sizeof(hip_names[0]));
  if (!tvdb_hip_lib_cache) return 0;
  if (!tvdb_hiprtc_lib_cache)
    tvdb_hiprtc_lib_cache = tvdb_load_rocm_library(rtc_names, sizeof(rtc_names) / sizeof(rtc_names[0]));
  if (!tvdb_hiprtc_lib_cache) return 0;
  cu->libcuda = tvdb_hip_lib_cache;
  cu->libnvrtc = tvdb_hiprtc_lib_cache;
  cu->hip = 1;
#define TVDB_HIP_SYM(field, name) do { \
  cu->field = (void*)tvdb_dyn_sym(cu->libcuda, name); \
  if (!cu->field) goto fail; \
} while (0)
#define TVDB_HIPRTC_SYM(field, name) do { \
  cu->field = (void*)tvdb_dyn_sym(cu->libnvrtc, name); \
  if (!cu->field) goto fail; \
} while (0)
  TVDB_HIP_SYM(cuInit, "hipInit");
  TVDB_HIP_SYM(cuDeviceGetCount, "hipGetDeviceCount");
  TVDB_HIP_SYM(cuDeviceGet, "hipDeviceGet");
  TVDB_HIP_SYM(cuDeviceGetName, "hipDeviceGetName");
  TVDB_HIP_SYM(cuCtxCreate, "hipCtxCreate");
  TVDB_HIP_SYM(cuCtxDestroy, "hipCtxDestroy");
  TVDB_HIP_SYM(cuCtxSynchronize, "hipDeviceSynchronize");
  TVDB_HIP_SYM(cuMemAlloc, "hipMalloc");
  TVDB_HIP_SYM(cuMemFree, "hipFree");
  TVDB_HIP_SYM(cuMemcpyHtoD, "hipMemcpyHtoD");
  TVDB_HIP_SYM(cuMemcpyDtoH, "hipMemcpyDtoH");
  TVDB_HIP_SYM(cuMemsetD32, "hipMemsetD32");
  TVDB_HIP_SYM(cuModuleLoadData, "hipModuleLoadData");
  TVDB_HIP_SYM(cuModuleUnload, "hipModuleUnload");
  TVDB_HIP_SYM(cuModuleGetFunction, "hipModuleGetFunction");
  TVDB_HIP_SYM(cuLaunchKernel, "hipModuleLaunchKernel");
  cu->cuGetErrorString = (void*)tvdb_dyn_sym(cu->libcuda, "hipDrvGetErrorString");
  cu->cuDeviceGetUuid = (void*)tvdb_dyn_sym(cu->libcuda, "hipDeviceGetUuid");
  /* No external-memory import: tvdb_gpu_buffer_import stays CUDA only. */
  TVDB_HIPRTC_SYM(nvrtcCreateProgram, "hiprtcCreateProgram");
  TVDB_HIPRTC_SYM(nvrtcCompileProgram, "hiprtcCompileProgram");
  TVDB_HIPRTC_SYM(nvrtcGetPTXSize, "hiprtcGetCodeSize");
  TVDB_HIPRTC_SYM(nvrtcGetPTX, "hiprtcGetCode");
  TVDB_HIPRTC_SYM(nvrtcGetProgramLogSize, "hiprtcGetProgramLogSize");
  TVDB_HIPRTC_SYM(nvrtcGetProgramLog, "hiprtcGetProgramLog");
  TVDB_HIPRTC_SYM(nvrtcDestroyProgram, "hiprtcDestroyProgram");
  cu->nvrtcGetErrorString = (void*)tvdb_dyn_sym(cu->libnvrtc, "hiprtcGetErrorString");
#undef TVDB_HIP_SYM
#undef TVDB_HIPRTC_SYM
  return 1;
fail:
  memset(cu, 0, sizeof(*cu));
  return 0;
}


/* ---- Devices ------------------------------------------------------------ */
static void tvdb_cuda_set_error(tvdb_gpu_compute_t* ctx, tvdb_error_t* err, const char* label,
                                CUresult r) {
  char msg[512];
  const char* text = NULL;
  if (ctx && ctx->cuda.cuGetErrorString) ctx->cuda.cuGetErrorString(r, &text);
  if (text) snprintf(msg, sizeof(msg), "%s failed: %s (%d)", label, text, r);
  else snprintf(msg, sizeof(msg), "%s failed: %d", label, r);
  tvdb_compute_error(err, TVDB_ERROR_IO, msg);
}
static int tvdb_cuda_ok(tvdb_gpu_compute_t* ctx, tvdb_error_t* err, const char* label, CUresult r) {
  if (r == CUDA_SUCCESS) return 1;
  tvdb_cuda_set_error(ctx, err, label, r);
  return 0;
}
static tvdb_status_t tvdb_cuda_alloc_copy_in(tvdb_gpu_compute_t* ctx, CUdeviceptr* dst,
                                             const void* src, size_t size, tvdb_error_t* err) {
  if (!tvdb_cuda_ok(ctx, err, "cuMemAlloc", ctx->cuda.cuMemAlloc(dst, size ? size : 4)))
    return err ? err->status : TVDB_ERROR_IO;
  if (src && size &&
      !tvdb_cuda_ok(ctx, err, "cuMemcpyHtoD", ctx->cuda.cuMemcpyHtoD(*dst, src, size))) {
    ctx->cuda.cuMemFree(*dst);
    *dst = 0;
    return err ? err->status : TVDB_ERROR_IO;
  }
  return TVDB_OK;
}

static tvdb_status_t tvdb_compute_create_on(int hip, uint32_t device_index, tvdb_gpu_compute_t** out,
                                            tvdb_error_t* err) {
  tvdb_gpu_compute_t* ctx = (tvdb_gpu_compute_t*)calloc(1, sizeof(*ctx));
  if (!ctx) {
    tvdb_compute_error(err, TVDB_ERROR_OUT_OF_MEMORY, "GPU device allocation failed");
    return TVDB_ERROR_OUT_OF_MEMORY;
  }
  if (!(hip ? tvdb_gpu_driver_load_hip(&ctx->cuda) : tvdb_gpu_driver_load_cuda(&ctx->cuda))) {
    free(ctx);
    tvdb_compute_error(err, TVDB_ERROR_UNIMPLEMENTED,
                       hip ? "HIP runtime and hiprtc libraries not found"
                           : "CUDA driver and NVRTC runtime libraries not found");
    return TVDB_ERROR_UNIMPLEMENTED;
  }
  int count = 0;
  if (!tvdb_cuda_ok(ctx, err, "cuInit", ctx->cuda.cuInit(0)) ||
      !tvdb_cuda_ok(ctx, err, "cuDeviceGetCount", ctx->cuda.cuDeviceGetCount(&count)))
    goto fail;
  if (count <= 0 || device_index >= (uint32_t)count) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                       hip ? "requested HIP device not found" : "requested CUDA device not found");
    goto fail;
  }
  if (!tvdb_cuda_ok(ctx, err, "cuDeviceGet", ctx->cuda.cuDeviceGet(&ctx->cu_device, (int)device_index)))
    goto fail;
  ctx->cuda.cuDeviceGetName(ctx->device_name, (int)sizeof(ctx->device_name), ctx->cu_device);
  if (!ctx->device_name[0])
    snprintf(ctx->device_name, sizeof(ctx->device_name), "%s device %u", hip ? "HIP" : "CUDA",
             device_index);
  if (!tvdb_cuda_ok(ctx, err, "cuCtxCreate", ctx->cuda.cuCtxCreate(&ctx->cu_ctx, 0, ctx->cu_device)))
    goto fail;
  *out = ctx;
  return TVDB_OK;
fail:
  tvdb_gpu_compute_destroy(ctx);
  return err ? err->status : TVDB_ERROR_IO;
}

tvdb_status_t tvdb_gpu_compute_create(tvdb_gpu_backend_t backend, uint32_t device_index,
                                      tvdb_gpu_compute_t** out, tvdb_error_t* err) {
  if (!out) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL output device");
    return TVDB_ERROR_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (backend == TVDB_GPU_BACKEND_CUDA || backend == TVDB_GPU_BACKEND_HIP)
    return tvdb_compute_create_on(backend == TVDB_GPU_BACKEND_HIP, device_index, out, err);
  if (backend != TVDB_GPU_BACKEND_AUTO) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "GPU compute needs CUDA, HIP or AUTO");
    return TVDB_ERROR_INVALID_ARGUMENT;
  }
  tvdb_status_t st = tvdb_compute_create_on(0, device_index, out, err);
  return st == TVDB_OK ? st : tvdb_compute_create_on(1, device_index, out, err);
}

void tvdb_gpu_compute_destroy(tvdb_gpu_compute_t* ctx) {
  if (!ctx || ctx->borrowed) return;
  if (ctx->vol_module && ctx->cuda.cuModuleUnload) ctx->cuda.cuModuleUnload(ctx->vol_module);
  if (ctx->cu_ctx && ctx->cuda.cuCtxDestroy) ctx->cuda.cuCtxDestroy(ctx->cu_ctx);
  tvdb_gpu_driver_close(&ctx->cuda);
  free(ctx);
}

tvdb_gpu_backend_t tvdb_gpu_compute_backend(const tvdb_gpu_compute_t* ctx) {
  return !ctx ? TVDB_GPU_BACKEND_AUTO : ctx->cuda.hip ? TVDB_GPU_BACKEND_HIP : TVDB_GPU_BACKEND_CUDA;
}
const char* tvdb_gpu_compute_device_name(const tvdb_gpu_compute_t* ctx) {
  return ctx ? ctx->device_name : "";
}

/* ---- Volumetric kernels ------------------------------------------------- */
static const char kTvdbCudaVolumetricSource[] =
"/* Volumetric kernels shared by CUDA (NVRTC) and HIP (hiprtc), compiled with\n"
"   floating-point contraction off so the arithmetic is the CPU's, operation\n"
"   for operation. */\n"
"typedef struct { float bmin[3]; float bmax[3]; int left, right, start, count; } vol_bvh_node;\n"
"typedef struct { float lo[3], hi[3], c[3], n[3]; int first, count, left, right; } vol_wn_node;\n"
"\n"
"__device__ __forceinline__ float vol_aabb_dsq(const float* lo, const float* hi, const float* p) {\n"
"  float d2 = 0.0f;\n"
"  for (int a = 0; a < 3; ++a) {\n"
"    if (p[a] < lo[a]) { float t = lo[a] - p[a]; d2 += t * t; }\n"
"    else if (p[a] > hi[a]) { float t = p[a] - hi[a]; d2 += t * t; }\n"
"  }\n"
"  return d2;\n"
"}\n"
"__device__ __forceinline__ float vol_dot(const float* a, const float* b) {\n"
"  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];\n"
"}\n"
"/* tri_closest_point_c (tinyvdb_mesh.c) in float; returns 0 where the CPU\n"
"   switches to its double-precision fallback, so the caller hands the voxel\n"
"   back to the CPU. */\n"
"__device__ int vol_tri_closest(const float* p, const float* a, const float* b, const float* c, float* o) {\n"
"  float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};\n"
"  float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};\n"
"  float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};\n"
"  float d1 = vol_dot(ab, ap), d2 = vol_dot(ac, ap);\n"
"  if (!isfinite(d1) || !isfinite(d2)) return 0;\n"
"  if (d1 <= 0.0f && d2 <= 0.0f) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; return 1; }\n"
"  float bp[3] = {p[0] - b[0], p[1] - b[1], p[2] - b[2]};\n"
"  float d3 = vol_dot(ab, bp), d4 = vol_dot(ac, bp);\n"
"  if (!isfinite(d3) || !isfinite(d4)) return 0;\n"
"  if (d3 >= 0.0f && d4 <= d3) { o[0] = b[0]; o[1] = b[1]; o[2] = b[2]; return 1; }\n"
"  float vc = d1 * d4 - d3 * d2;\n"
"  if (!isfinite(vc)) return 0;\n"
"  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {\n"
"    if (d1 == d3) return 0;\n"
"    float v = d1 / (d1 - d3);\n"
"    o[0] = a[0] + ab[0] * v; o[1] = a[1] + ab[1] * v; o[2] = a[2] + ab[2] * v;\n"
"    return 1;\n"
"  }\n"
"  float cp[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};\n"
"  float d5 = vol_dot(ab, cp), d6 = vol_dot(ac, cp);\n"
"  if (!isfinite(d5) || !isfinite(d6)) return 0;\n"
"  if (d6 >= 0.0f && d5 <= d6) { o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; return 1; }\n"
"  float vb = d5 * d2 - d1 * d6;\n"
"  if (!isfinite(vb)) return 0;\n"
"  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {\n"
"    if (d2 == d6) return 0;\n"
"    float w = d2 / (d2 - d6);\n"
"    o[0] = a[0] + ac[0] * w; o[1] = a[1] + ac[1] * w; o[2] = a[2] + ac[2] * w;\n"
"    return 1;\n"
"  }\n"
"  float va = d3 * d6 - d5 * d4;\n"
"  if (!isfinite(va)) return 0;\n"
"  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {\n"
"    if ((d4 - d3) + (d5 - d6) == 0.0f) return 0;\n"
"    float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));\n"
"    o[0] = b[0] + (c[0] - b[0]) * w; o[1] = b[1] + (c[1] - b[1]) * w; o[2] = b[2] + (c[2] - b[2]) * w;\n"
"    return 1;\n"
"  }\n"
"  if (!isfinite(va + vb + vc) || va + vb + vc <= 0.0f) return 0;\n"
"  float denom = 1.0f / (va + vb + vc);\n"
"  float sv = vb * denom, sw = vc * denom;\n"
"  o[0] = (a[0] + ab[0] * sv) + ac[0] * sw;\n"
"  o[1] = (a[1] + ab[1] * sv) + ac[1] * sw;\n"
"  o[2] = (a[2] + ab[2] * sv) + ac[2] * sw;\n"
"  return 1;\n"
"}\n"
"/* Van Oosterom and Strackee solid angle of a triangle seen from p. */\n"
"__device__ float vol_solid_angle(const float* v0, const float* v1, const float* v2, const float* p) {\n"
"  float a[3], b[3], c[3];\n"
"  for (int k = 0; k < 3; ++k) { a[k] = v0[k] - p[k]; b[k] = v1[k] - p[k]; c[k] = v2[k] - p[k]; }\n"
"  float na = sqrtf(vol_dot(a, a)), nb = sqrtf(vol_dot(b, b)), nc = sqrtf(vol_dot(c, c));\n"
"  float num = a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) +\n"
"              a[2] * (b[0] * c[1] - b[1] * c[0]);\n"
"  float den = na * nb * nc + vol_dot(a, b) * nc + vol_dot(b, c) * na + vol_dot(c, a) * nb;\n"
"  return 2.0f * atan2f(num, den);\n"
"}\n"
"#define VOL_STACK 128\n"
"/* One thread per voxel: world point index * h (computed in double and\n"
"   rounded like the CPU), closest-face distance over the shared BVH and the\n"
"   generalized winding number (fast tree, or every face when wn_count is 0).\n"
"   A NaN distance asks the host to redo the voxel. */\n"
"extern \"C\" __global__ void tvdb_vol_mesh_query(\n"
"    const int* ijk, unsigned int count, double h,\n"
"    const float* verts, const unsigned int* faces,\n"
"    const vol_bvh_node* bvh, const int* prims,\n"
"    const vol_wn_node* wn, unsigned int wn_count, const int* wn_order,\n"
"    unsigned int face_count, float far_diagonals,\n"
"    float* out_distance, float* out_winding) {\n"
"  unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;\n"
"  if (i >= count) return;\n"
"  double w[3] = {(double)ijk[3 * i] * h, (double)ijk[3 * i + 1] * h, (double)ijk[3 * i + 2] * h};\n"
"  float p[3] = {(float)w[0], (float)w[1], (float)w[2]};\n"
"  /* Closest face: tvdb_bvh_closest's traversal, pruning and tie-break. */\n"
"  float best = __int_as_float(0x7f800000);\n"
"  int best_face = -1, ok = 1;\n"
"  float best_cp[3] = {0.0f, 0.0f, 0.0f};\n"
"  int stack_node[VOL_STACK];\n"
"  float stack_d[VOL_STACK];\n"
"  int sp = 0;\n"
"  stack_node[sp] = 0; stack_d[sp] = 0.0f; ++sp;\n"
"  while (sp > 0 && ok) {\n"
"    --sp;\n"
"    int ni = stack_node[sp];\n"
"    float nd = stack_d[sp];\n"
"    if (nd > best) continue;\n"
"    const vol_bvh_node* node = &bvh[ni];\n"
"    if (node->left < 0) {\n"
"      for (int k = 0; k < node->count; ++k) {\n"
"        int f = prims[node->start + k];\n"
"        const float* a = &verts[3u * faces[3u * f]];\n"
"        const float* b = &verts[3u * faces[3u * f + 1u]];\n"
"        const float* c = &verts[3u * faces[3u * f + 2u]];\n"
"        float cp[3];\n"
"        if (!vol_tri_closest(p, a, b, c, cp)) { ok = 0; break; }\n"
"        float d[3] = {p[0] - cp[0], p[1] - cp[1], p[2] - cp[2]};\n"
"        float dsq = vol_dot(d, d);\n"
"        if (dsq < best || (dsq == best && f < best_face)) {\n"
"          best = dsq; best_face = f;\n"
"          best_cp[0] = cp[0]; best_cp[1] = cp[1]; best_cp[2] = cp[2];\n"
"        }\n"
"      }\n"
"    } else {\n"
"      int l = node->left, r = node->right;\n"
"      float dl = vol_aabb_dsq(bvh[l].bmin, bvh[l].bmax, p);\n"
"      float dr = vol_aabb_dsq(bvh[r].bmin, bvh[r].bmax, p);\n"
"      if (dl <= dr) {\n"
"        if (dr <= best && sp < VOL_STACK - 2) { stack_node[sp] = r; stack_d[sp] = dr; ++sp; }\n"
"        if (dl <= best && sp < VOL_STACK - 2) { stack_node[sp] = l; stack_d[sp] = dl; ++sp; }\n"
"      } else {\n"
"        if (dl <= best && sp < VOL_STACK - 2) { stack_node[sp] = l; stack_d[sp] = dl; ++sp; }\n"
"        if (dr <= best && sp < VOL_STACK - 2) { stack_node[sp] = r; stack_d[sp] = dr; ++sp; }\n"
"      }\n"
"    }\n"
"  }\n"
"  double dist = hypot(hypot(w[0] - (double)best_cp[0], w[1] - (double)best_cp[1]),\n"
"                      w[2] - (double)best_cp[2]);\n"
"  if (!ok || best_face < 0 || !isfinite(dist)) {\n"
"    out_distance[i] = __int_as_float(0x7fc00000);\n"
"    out_winding[i] = 0.0f;\n"
"    return;\n"
"  }\n"
"  /* Winding number (tvdb_winding_eval). */\n"
"  double sum = 0.0;\n"
"  if (wn_count == 0u) {\n"
"    for (unsigned int f = 0; f < face_count; ++f)\n"
"      sum += (double)vol_solid_angle(&verts[3u * faces[3u * f]], &verts[3u * faces[3u * f + 1u]],\n"
"                                     &verts[3u * faces[3u * f + 2u]], p);\n"
"  } else {\n"
"    int stack[VOL_STACK];\n"
"    int depth = 0;\n"
"    stack[depth++] = 0;\n"
"    while (depth) {\n"
"      const vol_wn_node* n = &wn[stack[--depth]];\n"
"      if (n->left < 0) {\n"
"        for (int k = 0; k < n->count; ++k) {\n"
"          int f = wn_order[n->first + k];\n"
"          sum += (double)vol_solid_angle(&verts[3u * faces[3u * f]], &verts[3u * faces[3u * f + 1u]],\n"
"                                         &verts[3u * faces[3u * f + 2u]], p);\n"
"        }\n"
"        continue;\n"
"      }\n"
"      float d2 = vol_aabb_dsq(n->lo, n->hi, p);\n"
"      float ex = n->hi[0] - n->lo[0], ey = n->hi[1] - n->lo[1], ez = n->hi[2] - n->lo[2];\n"
"      float diag2 = ex * ex + ey * ey + ez * ez;\n"
"      if (d2 > far_diagonals * far_diagonals * diag2) {\n"
"        float r[3] = {n->c[0] - p[0], n->c[1] - p[1], n->c[2] - p[2]};\n"
"        float r2 = vol_dot(r, r);\n"
"        if (r2 > 0.0f) sum += (double)(vol_dot(n->n, r) / (r2 * sqrtf(r2)));\n"
"        continue;\n"
"      }\n"
"      if (depth > VOL_STACK - 2) { out_distance[i] = __int_as_float(0x7fc00000); return; }\n"
"      stack[depth++] = n->right;\n"
"      stack[depth++] = n->left;\n"
"    }\n"
"  }\n"
"  out_distance[i] = (float)dist;\n"
"  out_winding[i] = (float)(sum / 12.566370614359172953850573533118);\n"
"}\n"
"\n"
"/* ---- Level-set sampling from a NanoVDB buffer -------------------------- */\n"
"/* tvdb_nvdb_get (prepended below, from tinyvdb_nanovdb_device.h) walks the\n"
"   grid's own NanoVDB layout: root tiles, upper/lower internal nodes and\n"
"   leaves, in index space. Trilinear samples at world points (tet_grid_sample,\n"
"   in double). */\n"
"extern \"C\" __global__ void tvdb_vol_nanovdb_sample(\n"
"    const double* points, unsigned int count, const double* inverse,\n"
"    const unsigned char* field, unsigned long long field_size, float* out) {\n"
"  unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;\n"
"  if (i >= count) return;\n"
"  const double* p = &points[3u * i];\n"
"  double q[3];\n"
"  for (int a = 0; a < 3; ++a)\n"
"    q[a] = inverse[4 * a + 3] + inverse[4 * a] * p[0] + inverse[4 * a + 1] * p[1] + inverse[4 * a + 2] * p[2];\n"
"  int base[3];\n"
"  double f[3];\n"
"  for (int a = 0; a < 3; ++a) {\n"
"    double fl = floor(q[a]);\n"
"    if (!(fl >= -2147483648.0 && fl < 2147483647.0)) { out[i] = __int_as_float(0x7fc00000); return; }\n"
"    base[a] = (int)fl;\n"
"    f[a] = q[a] - fl;\n"
"  }\n"
"  tvdb_nvdb_grid g;\n"
"  g.base = field;\n"
"  g.size = field_size;\n"
"  double sum = 0.0;\n"
"  for (int c = 0; c < 8; ++c) {\n"
"    int at[3] = {base[0] + (c & 1), base[1] + ((c >> 1) & 1), base[2] + ((c >> 2) & 1)};\n"
"    double w = ((c & 1) ? f[0] : 1.0 - f[0]) * (((c >> 1) & 1) ? f[1] : 1.0 - f[1]) *\n"
"               (((c >> 2) & 1) ? f[2] : 1.0 - f[2]);\n"
"    sum += w * (double)tvdb_nvdb_get(&g, at[0], at[1], at[2]);\n"
"  }\n"
"  out[i] = (float)sum;\n"
"}\n";

/* Kernels compile into their own module on first use: the main module's
 * compile cost is unchanged for applications that never call these, and
 * contraction is off here so device arithmetic follows the CPU's. */
static tvdb_status_t tvdb_vol_get_module(tvdb_gpu_compute_t* ctx, CUmodule* module,
                                         tvdb_error_t* err) {
  if (ctx->vol_module) {
    *module = ctx->vol_module;
    return TVDB_OK;
  }
  nvrtcProgram prog = NULL;
  /* The sampler walks NanoVDB's own layout; its accessor is a header so the
   * host test can compile the same code (see tinyvdb_nanovdb_device.h). */
  const size_t acc_len = sizeof(tvdb_nanovdb_device_src) - 1;
  const size_t src_len = strlen(kTvdbCudaVolumetricSource);
  char* source = (char*)malloc(acc_len + src_len + 1);
  if (!source) {
    tvdb_compute_error(err, TVDB_ERROR_OUT_OF_MEMORY, "volumetric kernel source allocation failed");
    return TVDB_ERROR_OUT_OF_MEMORY;
  }
  memcpy(source, tvdb_nanovdb_device_src, acc_len);
  memcpy(source + acc_len, kTvdbCudaVolumetricSource, src_len + 1);
  const nvrtcResult created =
      ctx->cuda.nvrtcCreateProgram(&prog, source, "tinyvdb_gpu_volumetric.cu", 0, NULL, NULL);
  free(source);
  if (created != NVRTC_SUCCESS) {
    tvdb_compute_error(err, TVDB_ERROR_IO, "volumetric kernel program creation failed");
    return TVDB_ERROR_IO;
  }
  const char* cuda_opts[] = {"--std=c++11", "--fmad=false"};
  const char* hip_opts[] = {"-std=c++14", "-ffp-contract=off", "-O3"};
  nvrtcResult nr = ctx->cuda.hip ? ctx->cuda.nvrtcCompileProgram(prog, 3, hip_opts)
                                 : ctx->cuda.nvrtcCompileProgram(prog, 2, cuda_opts);
  if (nr != NVRTC_SUCCESS) {
    size_t log_size = 0;
    ctx->cuda.nvrtcGetProgramLogSize(prog, &log_size);
    char* log = (char*)calloc(log_size ? log_size : 1, 1);
    if (log) ctx->cuda.nvrtcGetProgramLog(prog, log);
    tvdb_compute_error(err, TVDB_ERROR_IO, log && log[0] ? log : "volumetric kernel compile failed");
    free(log);
    ctx->cuda.nvrtcDestroyProgram(&prog);
    return TVDB_ERROR_IO;
  }
  size_t size = 0;
  char* code = NULL;
  if (ctx->cuda.nvrtcGetPTXSize(prog, &size) != NVRTC_SUCCESS || !size ||
      !(code = (char*)malloc(size)) || ctx->cuda.nvrtcGetPTX(prog, code) != NVRTC_SUCCESS) {
    free(code);
    ctx->cuda.nvrtcDestroyProgram(&prog);
    tvdb_compute_error(err, TVDB_ERROR_IO, "volumetric kernel code retrieval failed");
    return TVDB_ERROR_IO;
  }
  ctx->cuda.nvrtcDestroyProgram(&prog);
  CUresult cr = ctx->cuda.cuModuleLoadData(&ctx->vol_module, code);
  free(code);
  if (!tvdb_cuda_ok(ctx, err, "cuModuleLoadData", cr)) return err ? err->status : TVDB_ERROR_IO;
  *module = ctx->vol_module;
  return TVDB_OK;
}

static tvdb_status_t tvdb_vol_require_device(const tvdb_gpu_compute_t* ctx, tvdb_error_t* err) {
  if (ctx) return TVDB_OK;
  tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL GPU device");
  return TVDB_ERROR_INVALID_ARGUMENT;
}

/* (Re)size a device staging buffer to at least `bytes`. */
static tvdb_status_t tvdb_vol_reserve(tvdb_gpu_compute_t* ctx, CUdeviceptr* buf, size_t* capacity,
                                      size_t bytes, tvdb_error_t* err) {
  if (bytes <= *capacity && *buf) return TVDB_OK;
  if (*buf) ctx->cuda.cuMemFree(*buf);
  *buf = 0;
  *capacity = 0;
  if (!tvdb_cuda_ok(ctx, err, "cuMemAlloc", ctx->cuda.cuMemAlloc(buf, bytes ? bytes : 4)))
    return err ? err->status : TVDB_ERROR_OUT_OF_MEMORY;
  *capacity = bytes;
  return TVDB_OK;
}

/* ---- Mesh distance + winding evaluator ---------------------------------- */
typedef struct {
  float lo[3], hi[3], c[3], n[3];
  int32_t first, count, left, right;
} tvdb_vol_wn_node;
_Static_assert(sizeof(tvdb_vol_wn_node) == 64, "device winding node is 64 bytes");

typedef struct {
  tvdb_gpu_compute_t* ctx;
  CUfunction fn;
  CUdeviceptr verts, faces, bvh, prims, wn, wn_order;
  unsigned int wn_count, face_count;
  CUdeviceptr ijk, distance, winding;
  size_t ijk_cap, distance_cap, winding_cap;
  float* staging;
  size_t staging_cap;
} tvdb_vol_mesh_eval;

static void tvdb_vol_mesh_eval_free(tvdb_vol_mesh_eval* e) {
  if (!e->ctx) return;
  CUdeviceptr* bufs[] = {&e->verts, &e->faces, &e->bvh, &e->prims, &e->wn, &e->wn_order,
                         &e->ijk, &e->distance, &e->winding};
  for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); ++i)
    if (*bufs[i]) e->ctx->cuda.cuMemFree(*bufs[i]);
  free(e->staging);
  memset(e, 0, sizeof(*e));
}

static tvdb_status_t tvdb_vol_mesh_eval_init(tvdb_gpu_compute_t* ctx, const tvdb_triangle_mesh* mesh,
                                             tvdb_level_set_sign_t sign, tvdb_vol_mesh_eval* e,
                                             tvdb_error_t* err) {
  memset(e, 0, sizeof(*e));
  e->ctx = ctx;
  if (!mesh || !mesh->vertices || !mesh->faces || !mesh->face_count ||
      mesh->vertex_count > UINT32_MAX / 3 || mesh->face_count > UINT32_MAX / 3 ||
      mesh->face_count > (size_t)INT32_MAX) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid or oversized mesh for the GPU");
    return TVDB_ERROR_INVALID_ARGUMENT;
  }
  _Static_assert(sizeof(tvdb_triangle) == 3 * sizeof(uint32_t), "faces upload as uint32 triples");
  _Static_assert(sizeof(tvdb_vec3f) == 3 * sizeof(float), "vertices upload as float triples");
  CUmodule module = NULL;
  tvdb_mesh_bvh_t* bvh = NULL;
  tvdb_winding_t* wn = NULL;
  tvdb_vol_wn_node* nodes = NULL;
  tvdb_status_t st = tvdb_vol_get_module(ctx, &module, err);
  if (st != TVDB_OK) return st;
  if (!tvdb_cuda_ok(ctx, err, "cuModuleGetFunction",
                    ctx->cuda.cuModuleGetFunction(&e->fn, module, "tvdb_vol_mesh_query")))
    return err ? err->status : TVDB_ERROR_IO;
  if (!tvdb_mesh_bvh_build(mesh, &bvh)) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_DATA, "mesh BVH build failed");
    return TVDB_ERROR_INVALID_DATA;
  }
  if (tvdb_mesh_bvh_max_depth(bvh) > 60) {
    tvdb_mesh_bvh_destroy(bvh);
    tvdb_compute_error(err, TVDB_ERROR_UNIMPLEMENTED, "mesh BVH deeper than the device stack");
    return TVDB_ERROR_UNIMPLEMENTED;
  }
  st = tvdb_winding_create(mesh,
                           sign == TVDB_LEVEL_SET_SIGN_WINDING_EXACT ? TVDB_WINDING_EXACT
                                                                     : TVDB_WINDING_FAST,
                           &wn, err);
  if (st != TVDB_OK) goto done;
  size_t node_count = 0;
  const tvdb_winding_node_t* src = tvdb_winding_nodes(wn, &node_count);
  if (node_count) {
    if (node_count > UINT32_MAX ||
        !(nodes = (tvdb_vol_wn_node*)malloc(node_count * sizeof(*nodes)))) {
      st = TVDB_ERROR_OUT_OF_MEMORY;
      tvdb_compute_error(err, st, "winding upload allocation failed");
      goto done;
    }
    for (size_t i = 0; i < node_count; ++i) {
      for (int k = 0; k < 3; ++k) {
        nodes[i].lo[k] = (float)src[i].lo[k];
        nodes[i].hi[k] = (float)src[i].hi[k];
        nodes[i].c[k] = (float)src[i].c[k];
        nodes[i].n[k] = (float)src[i].n[k];
      }
      nodes[i].first = src[i].first;
      nodes[i].count = src[i].count;
      nodes[i].left = src[i].left;
      nodes[i].right = src[i].right;
    }
  }
  e->wn_count = (unsigned int)node_count;
  e->face_count = (unsigned int)mesh->face_count;
  if ((st = tvdb_cuda_alloc_copy_in(ctx, &e->verts, mesh->vertices,
                                    mesh->vertex_count * sizeof(tvdb_vec3f), err)) != TVDB_OK ||
      (st = tvdb_cuda_alloc_copy_in(ctx, &e->faces, mesh->faces,
                                    mesh->face_count * sizeof(tvdb_triangle), err)) != TVDB_OK ||
      (st = tvdb_cuda_alloc_copy_in(ctx, &e->bvh, tvdb_mesh_bvh_nodes(bvh),
                                    (size_t)tvdb_mesh_bvh_node_count(bvh) * sizeof(tvdb_mesh_bvh_node_t),
                                    err)) != TVDB_OK ||
      (st = tvdb_cuda_alloc_copy_in(ctx, &e->prims, tvdb_mesh_bvh_prims(bvh),
                                    (size_t)tvdb_mesh_bvh_prim_count(bvh) * sizeof(int32_t), err)) != TVDB_OK ||
      (st = tvdb_cuda_alloc_copy_in(ctx, &e->wn, nodes, node_count * sizeof(*nodes), err)) != TVDB_OK ||
      (st = tvdb_cuda_alloc_copy_in(ctx, &e->wn_order, tvdb_winding_order(wn),
                                    node_count ? mesh->face_count * sizeof(int32_t) : 0, err)) != TVDB_OK)
    goto done;
done:
  free(nodes);
  tvdb_winding_destroy(wn);
  tvdb_mesh_bvh_destroy(bvh);
  if (st != TVDB_OK) tvdb_vol_mesh_eval_free(e);
  return st;
}

static tvdb_status_t tvdb_vol_mesh_eval_run(void* user, const int32_t* ijk, size_t count,
                                            double h, float* distance, double* winding,
                                            tvdb_error_t* err) {
  tvdb_vol_mesh_eval* e = (tvdb_vol_mesh_eval*)user;
  tvdb_gpu_compute_t* ctx = e->ctx;
  if (!count) return TVDB_OK;
  if (count > UINT32_MAX / 3) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "GPU level set batch too large");
    return TVDB_ERROR_INVALID_ARGUMENT;
  }
  tvdb_status_t st;
  if ((st = tvdb_vol_reserve(ctx, &e->ijk, &e->ijk_cap, count * 3 * sizeof(int32_t), err)) != TVDB_OK ||
      (st = tvdb_vol_reserve(ctx, &e->distance, &e->distance_cap, count * sizeof(float), err)) != TVDB_OK ||
      (st = tvdb_vol_reserve(ctx, &e->winding, &e->winding_cap, count * sizeof(float), err)) != TVDB_OK)
    return st;
  if (count > e->staging_cap) {
    float* s = (float*)realloc(e->staging, count * sizeof(float));
    if (!s) {
      tvdb_compute_error(err, TVDB_ERROR_OUT_OF_MEMORY, "GPU level set staging allocation failed");
      return TVDB_ERROR_OUT_OF_MEMORY;
    }
    e->staging = s;
    e->staging_cap = count;
  }
  if (!tvdb_cuda_ok(ctx, err, "cuMemcpyHtoD",
                    ctx->cuda.cuMemcpyHtoD(e->ijk, ijk, count * 3 * sizeof(int32_t))))
    return err ? err->status : TVDB_ERROR_IO;
  unsigned int n = (unsigned int)count;
  float far_diagonals = (float)TVDB_WINDING_FAR_DIAGONALS;
  void* args[] = {&e->ijk, &n, &h, &e->verts, &e->faces, &e->bvh, &e->prims, &e->wn,
                  &e->wn_count, &e->wn_order, &e->face_count, &far_diagonals,
                  &e->distance, &e->winding};
  const unsigned int block = 128, grid = (n + block - 1u) / block;
  if (!tvdb_cuda_ok(ctx, err, "cuLaunchKernel",
                    ctx->cuda.cuLaunchKernel(e->fn, grid, 1, 1, block, 1, 1, 0, NULL, args, NULL)) ||
      !tvdb_cuda_ok(ctx, err, "cuCtxSynchronize", ctx->cuda.cuCtxSynchronize()) ||
      !tvdb_cuda_ok(ctx, err, "cuMemcpyDtoH",
                    ctx->cuda.cuMemcpyDtoH(distance, e->distance, count * sizeof(float))) ||
      !tvdb_cuda_ok(ctx, err, "cuMemcpyDtoH",
                    ctx->cuda.cuMemcpyDtoH(e->staging, e->winding, count * sizeof(float))))
    return err ? err->status : TVDB_ERROR_IO;
  for (size_t i = 0; i < count; ++i) winding[i] = (double)e->staging[i];
  return TVDB_OK;
}

tvdb_status_t tvdb_gpu_compute_mesh_to_level_set(tvdb_gpu_compute_t* ctx, const tvdb_triangle_mesh* mesh,
                                         const tvdb_mesh_level_set_options_t* options,
                                         tvdb_thread_pool_t* pool, tvdb_grid_t* out,
                                         tvdb_mesh_level_set_result_t* result, tvdb_error_t* err) {
  tvdb_status_t st = tvdb_vol_require_device(ctx, err);
  if (st != TVDB_OK) return st;
  if (!options) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL level set options");
    return TVDB_ERROR_INVALID_ARGUMENT;
  }
  tvdb_vol_mesh_eval e;
  if ((st = tvdb_vol_mesh_eval_init(ctx, mesh, options->sign, &e, err)) != TVDB_OK) return st;
  tvdb_mesh_level_set_options_t opt = *options;
  opt.evaluate = tvdb_vol_mesh_eval_run;
  opt.evaluate_user = &e;
  st = tvdb_mesh_to_level_set(mesh, &opt, pool, out, result, err);
  tvdb_vol_mesh_eval_free(&e);
  return st;
}

/* ---- Level-set tree sampler --------------------------------------------- */
static uint32_t tvdb_vol_root_hash(int32_t x, int32_t y, int32_t z) {
  uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  return h;
}

typedef struct {
  tvdb_gpu_compute_t* ctx;
  CUfunction fn;
  CUdeviceptr field, inverse; /* NanoVDB grid bytes, world -> index map */
  tvdb_nvdb_u64 field_size;
  CUdeviceptr points, out;
  size_t points_cap, out_cap;
} tvdb_vol_tree;

static void tvdb_vol_tree_free(tvdb_vol_tree* t) {
  if (!t->ctx) return;
  CUdeviceptr* bufs[] = {&t->field, &t->inverse, &t->points, &t->out};
  for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); ++i)
    if (*bufs[i]) t->ctx->cuda.cuMemFree(*bufs[i]);
  memset(t, 0, sizeof(*t));
}

/* World -> index: the inverse of the grid's affine map, computed exactly as
 * tvdb_level_set_to_tet_mesh does so device samples match its CPU ones. */
static int tvdb_vol_inverse(const tvdb_grid_t* grid, double inverse[3][4]) {
  double m[4][4];
  if (!tvdb_tree_matrix(&grid->transform, m)) return 0;
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  double inv[3][3];
  inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
  inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
  inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
  inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
  inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
  inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
  inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
  inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
  inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) inverse[a][b] = inv[a][b];
    inverse[a][3] = -(inv[a][0] * m[0][3] + inv[a][1] * m[1][3] + inv[a][2] * m[2][3]);
  }
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 4; ++b)
      if (!isfinite(inverse[a][b])) return 0;
  return 1;
}

/* Flatten a float tree for vol_tree_get: every node's values (internal tiles
 * and leaf voxels) and, for internal nodes, a per-slot child table. */
static tvdb_status_t tvdb_vol_tree_init(tvdb_gpu_compute_t* ctx, const tvdb_grid_t* grid,
                                        tvdb_vol_tree* t, tvdb_error_t* err) {
  memset(t, 0, sizeof(*t));
  t->ctx = ctx;
  double inverse[3][4];
  if (!tvdb_vol_inverse(grid, inverse)) {
    tvdb_compute_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM,
                       "level set transform not invertible");
    return TVDB_ERROR_UNSUPPORTED_TRANSFORM;
  }
  /* The sampler reads NanoVDB's own layout, so one conversion replaces the
   * per-node flattening: the buffer is the grid, and root tiles, internal
   * tiles and the background come along unchanged. */
  uint8_t* bytes = NULL;
  size_t size = 0;
  tvdb_status_t st = tvdb_grid_to_nanovdb(grid, &bytes, &size, err);
  if (st != TVDB_OK) return st;
  if (!bytes || size < TVDB_NVDB_GRID_BYTES + TVDB_NVDB_TREE_ROOT_OFF + 8u ||
      size > (size_t)0x7fffffffu) {
    free(bytes);
    tvdb_compute_error(err, TVDB_ERROR_INVALID_DATA, "level set has no NanoVDB representation");
    return TVDB_ERROR_INVALID_DATA;
  }
  CUmodule module = NULL;
  if ((st = tvdb_vol_get_module(ctx, &module, err)) != TVDB_OK) {
    free(bytes);
    return st;
  }
  if (!tvdb_cuda_ok(ctx, err, "cuModuleGetFunction",
                    ctx->cuda.cuModuleGetFunction(&t->fn, module,
                                                  "tvdb_vol_nanovdb_sample"))) {
    free(bytes);
    st = err ? err->status : TVDB_ERROR_IO;
    return st;
  }
  t->field_size = (tvdb_nvdb_u64)size;
  st = tvdb_cuda_alloc_copy_in(ctx, &t->field, bytes, size, err);
  free(bytes);
  if (st == TVDB_OK)
    st = tvdb_cuda_alloc_copy_in(ctx, &t->inverse, inverse, sizeof(inverse), err);
  if (st != TVDB_OK) tvdb_vol_tree_free(t);
  return st;
}

static tvdb_status_t tvdb_vol_tree_run(void* user, const double* points, size_t count,
                                       float* values, tvdb_error_t* err) {
  tvdb_vol_tree* t = (tvdb_vol_tree*)user;
  tvdb_gpu_compute_t* ctx = t->ctx;
  const size_t chunk = (size_t)1 << 22;
  for (size_t begin = 0; begin < count; begin += chunk) {
    const size_t n = count - begin < chunk ? count - begin : chunk;
    tvdb_status_t st;
    if ((st = tvdb_vol_reserve(ctx, &t->points, &t->points_cap, n * 3 * sizeof(double), err)) != TVDB_OK ||
        (st = tvdb_vol_reserve(ctx, &t->out, &t->out_cap, n * sizeof(float), err)) != TVDB_OK)
      return st;
    if (!tvdb_cuda_ok(ctx, err, "cuMemcpyHtoD",
                      ctx->cuda.cuMemcpyHtoD(t->points, points + 3 * begin, n * 3 * sizeof(double))))
      return err ? err->status : TVDB_ERROR_IO;
    unsigned int un = (unsigned int)n;
    tvdb_nvdb_u64 field_size = t->field_size;
    void* args[] = {&t->points, &un, &t->inverse, &t->field, &field_size, &t->out};
    const unsigned int block = 256, grid = (un + block - 1u) / block;
    if (!tvdb_cuda_ok(ctx, err, "cuLaunchKernel",
                      ctx->cuda.cuLaunchKernel(t->fn, grid, 1, 1, block, 1, 1, 0, NULL, args, NULL)) ||
        !tvdb_cuda_ok(ctx, err, "cuCtxSynchronize", ctx->cuda.cuCtxSynchronize()) ||
        !tvdb_cuda_ok(ctx, err, "cuMemcpyDtoH",
                      ctx->cuda.cuMemcpyDtoH(values + begin, t->out, n * sizeof(float))))
      return err ? err->status : TVDB_ERROR_IO;
  }
  return TVDB_OK;
}

tvdb_status_t tvdb_gpu_compute_level_set_sample(tvdb_gpu_compute_t* ctx, const tvdb_grid_t* grid,
                                        const double* points, size_t count, float* values,
                                        tvdb_error_t* err) {
  tvdb_status_t st = tvdb_vol_require_device(ctx, err);
  if (st != TVDB_OK) return st;
  if (!grid || (count && (!points || !values))) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid level set sample arguments");
    return TVDB_ERROR_INVALID_ARGUMENT;
  }
  tvdb_vol_tree t;
  if ((st = tvdb_vol_tree_init(ctx, grid, &t, err)) != TVDB_OK) return st;
  st = tvdb_vol_tree_run(&t, points, count, values, err);
  tvdb_vol_tree_free(&t);
  return st;
}

tvdb_status_t tvdb_gpu_compute_level_set_to_tet_mesh(tvdb_gpu_compute_t* ctx, const tvdb_grid_t* level_set,
                                             const tvdb_stuffing_options_t* options,
                                             tvdb_thread_pool_t* pool, tvdb_tet_mesh* out,
                                             tvdb_error_t* err) {
  tvdb_status_t st = tvdb_vol_require_device(ctx, err);
  if (st != TVDB_OK) return st;
  if (!level_set || !options || !out) {
    tvdb_compute_error(err, TVDB_ERROR_INVALID_ARGUMENT, "invalid stuffing arguments");
    return TVDB_ERROR_INVALID_ARGUMENT;
  }
  tvdb_vol_tree t;
  if ((st = tvdb_vol_tree_init(ctx, level_set, &t, err)) != TVDB_OK) return st;
  tvdb_stuffing_options_t opt = *options;
  opt.batch_sample = tvdb_vol_tree_run;
  opt.batch_user = &t;
  st = tvdb_level_set_to_tet_mesh(level_set, &opt, pool, out, err);
  tvdb_vol_tree_free(&t);
  return st;
}
