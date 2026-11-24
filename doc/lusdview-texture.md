# lusdview texture compression formats

This document summarizes the practical compressed texture targets for desktop
OpenGL and Vulkan on NVIDIA GeForce RTX 50-series and AMD Radeon RX 9070 XT
class hardware.

## Practical format availability

| Format family | RTX 5070 Ti OpenGL | RX 9070 XT OpenGL | Vulkan on both GPUs |
|---|---|---|---|
| BC1 / DXT1 | Yes | Yes | Yes |
| BC2 / DXT3 | Yes | Yes | Yes |
| BC3 / DXT5 | Yes | Yes | Yes |
| BC4 / RGTC1 | Yes | Yes | Yes |
| BC5 / RGTC2 | Yes | Yes | Yes |
| BC6H / BPTC float HDR | Yes | Yes | Yes |
| BC7 / BPTC | Yes | Yes | Yes |
| ETC2 / EAC | Not portable; usually unavailable on desktop GL | Driver-dependent | Optional |
| ASTC LDR/HDR | Not portable on desktop GL | Driver/extension-dependent | Optional |

The local test system has an RTX 5060 Ti rather than an RTX 5070 Ti. Its
NVIDIA Vulkan driver and the RX 9070 XT RADV driver both report
`textureCompressionBC = true`, while ETC2 and ASTC feature bits are false.
Therefore BC1 through BC7 are the reliable native Vulkan formats on this
machine. Format enums existing in Vulkan do not imply that a device supports
sampling them.

## OpenGL baselines

| OpenGL baseline | Core compressed formats |
|---|---|
| OpenGL 3.3 | RGTC: BC4 and BC5 |
| OpenGL 3.3 plus common extensions | BC1 through BC3 through S3TC extensions |
| OpenGL 4.2 | BPTC: BC6H and BC7 |
| OpenGL 4.3 | ETC2/EAC |
| OpenGL 4.4–4.6 | No major new universal desktop block-compression family; ASTC remains extension-dependent |

OpenGL 3.3 core RGTC formats are specified in the
[OpenGL 3.3 core specification](https://www.opengl.org/registry/doc/glspec33.core.20100311.withchanges.pdf).
BPTC became core in OpenGL 4.2, and ETC2/EAC became standard in OpenGL 4.3
([Khronos OpenGL 4.3 announcement](https://www.khronos.org/news/press/khronos-releases-opengl-4.3-specification-with-major-enhancements)).

OpenGL compression support should still be checked through the context
version and extension string. In particular, S3TC is commonly available on
desktop drivers but is not a portable OpenGL 3.3 core guarantee.

## Vulkan baseline

Vulkan does not guarantee one universal compressed format family on every GPU.
Applications should query:

```cpp
VkPhysicalDeviceFeatures::textureCompressionBC
VkPhysicalDeviceFeatures::textureCompressionETC2
VkPhysicalDeviceFeatures::textureCompressionASTC_LDR
```

Recommended desktop baseline:

1. Target Vulkan 1.0 or newer.
2. Prefer BC1–BC7 when `textureCompressionBC` is enabled.
3. Check individual format properties before creating the image.
4. Fall back to `VK_FORMAT_R8G8B8A8_UNORM` or
   `VK_FORMAT_R8G8B8A8_SRGB`.
5. Use ETC2/EAC or ASTC only when the corresponding feature and format
   properties are reported.

The [Vulkan format specification](https://docs.vulkan.org/spec/latest/chapters/formats.html)
lists BC, ETC2/EAC, and ASTC as separate format families with device-dependent
feature support.

## Recommended lusdview policy

```text
HDR:              BC6H -> RGBA16F/RGBA32F fallback
Color:            BC7  -> BC3 -> BC1 -> RGBA8
Normal:           BC5  -> BC3 -> RGBA8
Single channel:   BC4  -> R8
ASTC/ETC2:        optional mobile/driver-specific paths
```

BC6H is the preferred compact HDR target. BC7 is the preferred high-quality
LDR color target. BC5 is preferred for two-channel normal maps, and BC4 is
preferred for single-channel masks or material data.

## TIFF, BigTIFF, and metadata loading

TinyDNG now supports classic TIFF and BigTIFF metadata and pixel loading,
including 64-bit IFD offsets/counts, `LONG8`/`IFD8`, big-endian files, tiled
images, Deflate-compressed tiles, and multi-IFD files. TIFF metadata queries
use `GetImageInfoFromFile()` and mmap when available; they do not decode pixels
or allocate tile/strip offset tables unnecessarily. `LoadImageFromFile()` also
uses mmap before falling back to the stream reader.

Representative validation:

| Input | Result |
|---|---|
| Island `islandsunEnv.tex` metadata | 16384×8192 RGB, 0.229 ms, ~50 MB process RSS |
| Island `islandsunEnv.tex` full decode | 8192×4096 output, HIP success, 5.33 ms GPU |
| Island `islandsunCam.tex` full decode | 8192×4096 output, HIP success, 5.47 ms GPU |
| Big-endian TIFF | Full decode passed |
| Tiled Deflate TIFF | Full decode passed |
| Multi-IFD TIFF | Full decode passed |

The decoded-image allocation remains proportional to the requested output
image. mmap removes the additional encoded-file heap copy; it does not make a
full pixel decode memory-free.

## GPU benchmark results

The standalone harness is `tools/lusdview-texture-bench`. It supports
`--metadata-only`, `--max-images`, `--mips`, JSON reports, and Vulkan/HIP/CUDA
backends. Reports include source bytes, mapped source bytes, load time, RSS
delta, GPU resize/compression time, total time, and PSNR.

On four `alab` corpus images with three mip levels, BC7/ASTC mip 0 timings
were:

| Backend/device | BC7 | ASTC |
|---|---:|---:|
| Vulkan RX 9070 XT | 2.62–2.76 ms | 1.21–1.23 ms |
| HIP RX 9070 XT | 0.13–1.46 ms | 0.06–0.12 ms |
| CUDA RTX 5060 Ti | 0.17–0.26 ms | 0.05–0.06 ms |

On the real Island EXR (`islandsun.exr`, 14308×7154), mip 0 timings were:

| Backend/device | BC6H | BC7 | ASTC |
|---|---:|---:|---:|
| Vulkan RX 9070 XT | 1706.9 ms | 216.6 ms | 143.4 ms |
| HIP RX 9070 XT | 37.0 ms | 8.24 ms | 3.41 ms |
| CUDA RTX 5060 Ti | 41.0 ms | 11.8 ms | 3.71 ms |

All EXR paths completed three mip levels. BC6H quality was approximately
46.4 dB at mip 0 in the current GPU prototype. The Vulkan BC6H encoder is
feature-complete but remains a quality/performance prototype; HIP and CUDA are
currently substantially faster on this hardware.

## Real-data CTest coverage

The opt-in CTest matrix covers NVIDIA Vulkan, AMD Vulkan, HIP, CUDA, real
PTEX/TEX selection, tiled-TIFF metadata, full TEX decode/compress, and
three-level mip generation. Set `LUSDVIEW_TEXTURE_GPU_HDR_ROOT` directly to an
EXR for the uncapped BC6H tests.

After the fast Vulkan BC6H endpoint/index path was enabled, uncapped
`islandsun.exr` BC6H completed as follows:

| Backend/device | mip 0 | mip 1 | mip 2 |
|---|---:|---:|---:|
| Vulkan RTX 5060 Ti | 21450 ms | 5241 ms | 1292 ms |
| Vulkan RX 9070 XT | 384 ms | 189 ms | 43 ms |

GPU-assisted Vulkan validation passes with Khronos ValidationLayers. The test
self-skips when `VK_LAYER_PATH` is not configured. Query pools are reset on
the command stream, producing no query-reset validation errors.

## lusdview integration

The viewer's `--core` texture finalization can use the same Vulkan compute
processor as the benchmark:

```sh
lusdview --core --backend vk --texture-gpu vulkan \
  --texture-compress bc7 --texture-gpu-device=9070XT scene.usdz
```

GPU resize is used for texture edge/budget reductions and GPU block encoding is
used for BC1/BC3/BC5/BC7/ASTC-compatible requests. Float EXR/HDR textures keep
their linear RGB data, use GPU BC6H for `--texture-compress auto` or `bc`, and
retain a Reinhard-tonemapped RGBA8 representation for BC7/unsupported-device
fallbacks. HDR BC6H mip levels are generated in linear float space before GPU
encoding. CUDA is embedded when the CUDA compiler is available. HIP uses an
isolated hipew runtime loader so it does not collide with lusdview's existing
HIP ray-tracing symbols. Ptex and UDIM paths retain their existing specialized
handling; HDR UDIM tiles retain per-tile float data and Vulkan uploads their
BC6H array when all tile payloads are compatible. If initialization or a
format request fails, the viewer falls back to the existing CPU encoder. The
integrated processor uses a private Vulkan device dispatch table so scene
rendering's volk state is not replaced.
