#pragma once
/* CUDA driver API / NVRTC and HIP / hiprtc, declared locally (no SDK headers)
 * and loaded at runtime. Shared by tinyvdb_gpu_compute.c and tinyvdb_gpu.c;
 * not a public header. */
#include <stddef.h>
#include <stdint.h>

#include "tinyvdb_gpu_compute.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st* CUcontext;
typedef struct CUmod_st* CUmodule;
typedef struct CUfunc_st* CUfunction;
typedef uint64_t CUdeviceptr;
typedef struct CUextMemory_st* CUexternalMemory;
typedef struct {
  unsigned int type;
  union { int fd; struct { void* handle; const void* name; } win32; const void* nvSciBufObject; } handle;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_HANDLE_DESC;
typedef struct {
  unsigned long long offset;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_BUFFER_DESC;
#define CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD 1
#define CUDA_EXTERNAL_MEMORY_DEDICATED 0x1u
typedef int nvrtcResult;
typedef struct _nvrtcProgram* nvrtcProgram;

#define CUDA_SUCCESS 0
#define NVRTC_SUCCESS 0

/* The CUDA driver API and NVRTC, or their HIP and hiprtc counterparts: the
 * HIP entry points used here have the same C ABI (int status, 64-bit device
 * pointers, opaque handles), so a HIP context fills this table with them and
 * runs the CUDA code paths unchanged. `hip` records which runtime it holds;
 * nvrtcGetPTX/nvrtcGetPTXSize then hold hiprtcGetCode/hiprtcGetCodeSize. */
typedef struct {
  void* libcuda;
  void* libnvrtc;
  int hip;
  CUresult (*cuInit)(unsigned int);
  CUresult (*cuDeviceGetCount)(int*);
  CUresult (*cuDeviceGet)(CUdevice*, int);
  CUresult (*cuDeviceGetName)(char*, int, CUdevice);
  CUresult (*cuDeviceGetUuid)(uint8_t*, CUdevice);  // optional; CUuuid is 16 bytes
  CUresult (*cuCtxCreate)(CUcontext*, unsigned int, CUdevice);
  CUresult (*cuCtxDestroy)(CUcontext);
  CUresult (*cuCtxSynchronize)(void);
  CUresult (*cuMemAlloc)(CUdeviceptr*, size_t);
  CUresult (*cuMemFree)(CUdeviceptr);
  CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void*, size_t);
  CUresult (*cuMemcpyDtoH)(void*, CUdeviceptr, size_t);
  CUresult (*cuMemsetD32)(CUdeviceptr, unsigned int, size_t);
  CUresult (*cuModuleLoadData)(CUmodule*, const void*);
  CUresult (*cuModuleUnload)(CUmodule);
  CUresult (*cuModuleGetFunction)(CUfunction*, CUmodule, const char*);
  CUresult (*cuLaunchKernel)(CUfunction, unsigned int, unsigned int, unsigned int,
                             unsigned int, unsigned int, unsigned int,
                             unsigned int, void*, void**, void**);
  CUresult (*cuGetErrorString)(CUresult, const char**);
  CUresult (*cuImportExternalMemory)(CUexternalMemory*, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC*);
  CUresult (*cuExternalMemoryGetMappedBuffer)(CUdeviceptr*, CUexternalMemory, const CUDA_EXTERNAL_MEMORY_BUFFER_DESC*);
  CUresult (*cuDestroyExternalMemory)(CUexternalMemory);
  nvrtcResult (*nvrtcCreateProgram)(nvrtcProgram*, const char*, const char*, int,
                                    const char* const*, const char* const*);
  nvrtcResult (*nvrtcCompileProgram)(nvrtcProgram, int, const char* const*);
  nvrtcResult (*nvrtcGetPTXSize)(nvrtcProgram, size_t*);
  nvrtcResult (*nvrtcGetPTX)(nvrtcProgram, char*);
  nvrtcResult (*nvrtcGetProgramLogSize)(nvrtcProgram, size_t*);
  nvrtcResult (*nvrtcGetProgramLog)(nvrtcProgram, char*);
  nvrtcResult (*nvrtcDestroyProgram)(nvrtcProgram*);
  const char* (*nvrtcGetErrorString)(nvrtcResult);
} tvdb_cuda_table;

/* Fill `cu` with the CUDA driver and NVRTC, or with HIP and hiprtc (which
 * stay loaded for the process: unloading ROCm is unsafe). 0 when the
 * libraries or a required entry point are missing; `cu` is then zeroed. */
int tvdb_gpu_driver_load_cuda(tvdb_cuda_table* cu);
int tvdb_gpu_driver_load_hip(tvdb_cuda_table* cu);
/* Release a loaded table (HIP libraries are kept) and zero it. */
void tvdb_gpu_driver_close(tvdb_cuda_table* cu);

/* A CUDA/HIP device: a context on it and the lazily compiled volumetric
 * module. tvdb_gpu_context_t lends its own fields through one of these
 * (borrowed = 1: nothing is released). */
struct tvdb_gpu_compute {
  tvdb_cuda_table cuda;
  CUcontext cu_ctx;
  CUdevice cu_device;
  CUmodule vol_module;
  char device_name[128];
  int borrowed;
};

#ifdef __cplusplus
}
#endif
