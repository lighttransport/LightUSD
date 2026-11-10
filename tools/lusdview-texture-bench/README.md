# lusdview GPU texture benchmark

This is a standalone, headless texture-processing benchmark. It does not link
the lusdview renderer and is safe to build while renderer work is in progress.
It links the core image/Ptex providers and works with `LIGHTUSD_BUILD_LEGACY=OFF`.
Native TIFF/DNG decoding is enabled by `LIGHTUSD_WITH_TIFF` (default ON);
16-bit integer RGB/RGBA inputs normalize to RGBA8 for the LDR benchmarks.

```sh
cmake -S . -B build_ninja -G Ninja \
  -DLIGHTUSD_BUILD_TEXTURE_GPU_BENCH=ON \
  -DLIGHTUSD_WITH_TEXTOOLS=ON
cmake --build build_ninja --target lusdview_texture_gpu_bench

build_ninja/lusdview_texture_gpu_bench \
  --backend vulkan --root /path/to/texture-corpus \
  --report texture-vulkan.json
```

The input corpus is external and must be supplied at runtime. A single image
file may also be passed as `--root` for bounded probes. Missing hardware,
drivers, runtimes, or corpus data return CTest-style skip code `77`. LDR and
EXR/HDR images are accepted. HDR images are Reinhard-tonemapped to RGBA8 for
BC1/BC3/BC5/BC7 and ASTC; BC6H preserves the HDR float data. Vulkan measures
resize plus BC1/BC3/BC5/BC7 and has a fast unsigned BC6H mode-11 compute
encoder. The BC6H prototype uses nearest source sampling and a single
endpoint-line search; the vendored CPU encoder remains the higher-quality
reference. ASTC uses a Vulkan 4x3-weight interpolated block path with a
deliberately small endpoint/weight search; it favors throughput over the
quality of the vendored CPU encoder.

Use `--synthetic --allow-software` for a small smoke run on llvmpipe. `--filter
bilinear` selects the fast filter; Mitchell is the default and matches the
viewer’s general-purpose CPU filter semantics more closely. The HIP adapter
uses native ROCm kernels for resize and BC1/BC3/BC5/BC7 compression. It selects
the first HIP device and supports BC6H mode-11 HDR encoding; use
`--backend hip` to benchmark it. Set `LD_LIBRARY_PATH` to the ROCm library
directory when the HIP runtime is not on the system loader path; hipew also
searches `HIP_PATH`, `ROCM_PATH`, and versioned `/opt/rocm/core-*/lib` trees.
The viewer build puts the HIP adapter in a load-on-demand module, so the main
`lusdview` executable has no link-time ROCm dependency. The CUDA adapter uses
native CUDA kernels for resize and BC1/BC3/BC5/BC7 compression.
Its default architecture is `120` for Blackwell-class GPUs and can be changed
with `-DLUSDVIEW_TEXTURE_CUDA_ARCHITECTURES=...`; select it with
`--backend cuda`. CUDA also supports the BC6H mode-11 HDR path.

Use `--mips N` to benchmark and validate N output mip levels; `--mips 0`
generates the complete chain down to 1x1. LDR and HIP BC6H HDR levels are chained from the
previous GPU-generated level. Each level is reported separately (`bc7@mip0`,
`bc7@mip1`, …) and GPU time is averaged over the requested iterations.

Use `--metadata-only --root FILE` to query TIFF/BigTIFF dimensions without
decoding pixels. This is useful for large tiled TIFF/TEX files and reports
metadata time separately. For corpus runs, pass a directory with
`--max-images N`; JSON reports include `source_bytes`, `mapped_source_bytes`,
`load_ms`, and `rss_delta_bytes` alongside GPU timings.

Opt-in real-corpus CTest checks are registered as
`lusdview-texture-vulkan-nvidia-real`, `lusdview-texture-vulkan-nvidia-formats-real`,
`lusdview-texture-vulkan-amd-real`,
`lusdview-texture-vulkan-amd-formats-real`,
`lusdview-texture-hip-real`, and `lusdview-texture-cuda-real` when those
backends are available.
Set `LUSDVIEW_TEXTURE_GPU_ROOT` to an external file or corpus directory:

```sh
LUSDVIEW_TEXTURE_GPU_ROOT=/mnt/disk1/data/island \
  ctest --test-dir build_ninja -R lusdview-texture-vulkan-nvidia-real -V
```

They skip with CTest code 77 when the external corpus is absent.
HDR tests use the same matrix with `bc6h,bc7,astc`; point
`LUSDVIEW_TEXTURE_GPU_ROOT` at an EXR, or set
`LUSDVIEW_TEXTURE_GPU_HDR_ROOT` separately, then select them with
`-R 'lusdview-texture-.*-hdr-real'`.
Directory scans are sorted by path before `--max-images` is applied, and
AppleDouble sidecars (`._*`) are excluded.
Set `LUSDVIEW_TEXTURE_GPU_MIN_PSNR` to enforce a minimum decoded quality in
real-data tests; the default is `0` (report-only).
Full-resolution BC6H tests require `LUSDVIEW_TEXTURE_GPU_HDR_ROOT` to point
directly at an EXR and are named
`lusdview-texture-vulkan-{nvidia,amd}-bc6h-full-real`.
