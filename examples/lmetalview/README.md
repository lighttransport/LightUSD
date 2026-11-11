# lmetalview

A native macOS **Swift + Metal** USD viewer: the Quick Look end of the LightUSD
examples. Drag a `.usd` / `.usda` / `.usdc` / `.usdz` file onto the window (or
pass it on the command line) and get a shaded, framed preview with a timeline.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLIGHTUSD_BUILD_EXAMPLES=ON \
      -DLIGHTUSD_BUILD_LMETALVIEW=ON
cmake --build build -j16

open build/src/lmetalview_example/lmetalview.app --args model.usdz
# or, with a file argument:
build/src/lmetalview_example/lmetalview.app/Contents/MacOS/lmetalview model.usdz
```

The bundle is `lmetalview.app`; Finder's *Open With* lists it for USD files.
`LIGHTUSD_BUILD_LMETALVIEW` defaults to `LIGHTUSD_BUILD_EXAMPLES` and
is only available in the core product. The Swift + Metal app is macOS only;
on Windows the option builds just the portable core (`lmetalview_core`) that
[`ld3d12view`](../ld3d12view/README.md) renders with Direct3D 12.

Headless mode (used by CTest) renders one frame without a window:

```sh
build/src/lmetalview_example/lmetalview.app/Contents/MacOS/lmetalview \
  --headless --size 640x480 --screenshot out.png models/cube-previewsurface.usdc
```

## Controls

| Input | Action |
| --- | --- |
| Left drag | Orbit (Maya-style, shared with lusdview) |
| Right / middle drag | Pan |
| Wheel / pinch | Dolly |
| Double-click, `F` | Frame all |
| `W` / `G` / `S` | Wireframe / ground grid / shadows |
| `[` `]` | Exposure −/+ |
| Space | Play / pause the timeline |
| `L` | Toggle looping |
| `,` / `.` | Halve / double playback speed |
| `0` | Reset playback speed to 1× |
| ← → | Step one frame |
| `1`–`7` | Camera presets (iso, front, back, right, left, top, bottom) |
| `⌘O` / `⌘R` | Open / reload |
| `⌘S` | Save a PNG screenshot |
| View ▸ Use Authored Camera | Switch to the first `Camera` prim |

## What it is built on

| Layer | Source | Notes |
| --- | --- | --- |
| Composition + conversion | `lightusd_core` + `tydra` through the C API (`lightusd-c.h`, `lightusd-render-c.h`) | The core product is linked |
| Preview scene ABI | `examples/lmetalview/core/lmv.h` | Flat meshes, draws, packed OpenPBR materials, decoded images, lights, cameras |
| Texture pixels | `stb_image`, vendored tinyexr (EXR), texpipe/texcomp + zstd (KTX2), `lightusd::USDZReader` | tydra records texture metadata only; the viewer decodes preview-sized pixels itself |
| Ptex | `src/ptx-loader.cc` + a bounded preview atlas (`core/lmv-ptex.*`) | one atlas sampled by authored face index + face-local UV |
| Skinning / morph | `lightusd_skel_sample_*` + GPU palette in `lmv_scene.cc` | 4 influences per vertex plus blend-shape deltas; the vertex shader poses the bind-pose geometry every frame |
| Camera | `examples/common/light3d/camera-nav.hh` | The same `OrbitCamera` lusdview uses (moved there so both examples share it) |
| Renderer | Swift + MSL, compiled at startup with `MTLDevice.makeLibrary(source:)` | No offline Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`) is required |

`SceneGPU` is built on a background queue and handed to the render thread as a
fully independent Metal scene, so scrubbing the timeline never races a draw.
The composed stage is retained. A time change first tries
Playback runs on a wall-clock anchor at the authored frame rate, with looping
and 0.125×–8× speed control.

`lmv_scene_update_animation`, which re-evaluates node transforms, light
transforms and intensity/color/exposure, PointInstancer instance transforms,
visibility, skeleton palettes and blend-shape weights in place; only the small
palette/weight/light/draw buffers are replaced, never the geometry. An animated authored camera is re-applied to
the camera controller while "Use Authored Camera" is active. Channels the
fast path does not own — animated material inputs, geometry/topology, other
camera/light parameters, and animated transforms/visibility on Points/Curves —
force a tydra re-conversion (`lmv_scene_set_time`) without re-parsing or
re-composing the file, and decoded textures are cached across conversions.

## Materials

Tydra resolves `UsdPreviewSurface`, OpenPBR and MaterialX
(`standard_surface`, `open_pbr_surface`, preview-surface, and material-local
node graphs with constant/arithmetic/mix/image nodes) into the canonical packed
OpenPBR block (`src/tydra/openpbr-params.hh`). The Metal shader evaluates an
**OpenPBR-lite** response:

| Feature | Supported |
| --- | --- |
| Base color / weight, metallic, specular weight/color/roughness/IOR | yes |
| Coat (weight, color, roughness, IOR) | yes |
| Sheen / fuzz (weight, color, roughness) | yes |
| Emission, opacity (opaque / mask / blend), normal maps | yes |
| Image-based lighting (authored dome texture, SH9 diffuse + roughness mips) | yes |
| Directional shadow map (3×3 PCF, slope-scaled bias) | yes |
| Transmission (refracted environment, tint), subsurface (wrap + translucency), thin film (Airy), anisotropy (anisotropic GGX) | approximated (no volumetric absorption/scattering) |
| UsdSkel skinning + blend shapes | yes, CPU-baked per evaluated time |
| Native instances + PointInstancer | yes (native instances via `preserve_native_instances`; point-instance draws with per-instance transforms/material overrides) |
| UV1 | yes, per-texture UV-set selection |
| Working-space colour management | material/light constants + IBL/background use `working_to_display_linear` |
| UsdGeomPoints / curves / Gaussian particles | yes (splats, screen-space ribbons, Gaussian splats with SH DC colour) |
| Shadows | up to four directional/spot casters in a 2×2 atlas |
| UDIM tiles | yes, as a per-texture `texture2d_array` with a 10×10 tile LUT |
| Ptex (`.ptx`) | preview atlas (bounded face resolution, no page streaming or mips) |
| KTX2 (uni / BC / ETC2 / EAC / ASTC LDR, Zstd supercompression) | yes; BC1/BC3/BC5/BC6H/BC7 upload as block-compressed Metal textures, the rest decode to RGBA8 |
| EXR | yes for uncompressed/ZIP/PIZ/B44 files; DWA-compressed EXRs are not supported by the vendored TinyEXR backend |

Texture slots evaluated per material: base color, metallic, roughness, normal,
emission, opacity. Each slot keeps its authored UV offset/scale/rotation, wrap
mode, channel selection and value scale/bias, and sRGB decode is applied only to
color slots.

## Deliberate limitations

* `UsdGeomPoints` and curves are supported as splats/screen-space ribbons, but
  their transforms and visibility are baked at conversion time; animating them
  (or an ancestor) falls back to re-conversion.
* Skinning is 4-influence linear blend skinning on the GPU. Extra influences are
  truncated and renormalized; there is no dual-quaternion skinning. Blend-shape
  normal deltas and in-between targets are ignored; positional targets (dense
  and sparse) are applied before LBS.
* Camera depth of field is not rendered yet (authored exposure and f-stop are
  carried through; only exposure is applied).
* UsdVol volumes are not ray-marched (ParticleFields are rendered as splats).
* KTX2 ETC2/EAC/ASTC (and the private `uni` carrier) are decoded to RGBA8;
  only BC1/BC3/BC5/BC6H/BC7 stay block-compressed (Metal on macOS has no
  ETC/ASTC decode for these formats).
* Transparency is sorted per draw, not per triangle; there is no OIT/depth
  peeling.
* UDIM tiles must exist on disk / in the `.usdz` and share a resolution; missing
  tiles render magenta.
* Ptex uses one bounded preview atlas (default 64-texel faces, 4k atlas
  ceiling) with no mip chain, page streaming or adjacent-face filtering.
  Constant-face and faceIndex fixtures are tested; the non-constant face codec
  path is not.
* EXR decoding covers the codecs the vendored TinyEXR C backend implements
  (none/RLE/ZIP/ZIPS/PIZ/PXR24/B44/B44A). DWA/DWAB and HTJ2K/ZSTD EXRs are not
  decoded and the material falls back to its constant inputs.
* Shadows use one 2×2 atlas for up to four directional/spot casters. There are
  no cascades, contact shadows or per-light shadow resolutions.
* MaterialX node graphs are evaluated on the CPU by Tydra and reduced to
  PBR parameters; arbitrary graph evaluation on the GPU is out of scope (the
  lusdview lightrt/MaterialX path covers that case).
* The in-place path covers node transforms, light transforms plus
  intensity/color/exposure, PointInstancer positions/orientations/scales,
  authored-camera transforms plus focal length/apertures/clipping/focus/
  exposure/f-stop, visibility, PointInstancer `invisibleIds`, prototype-index
  switching and instance-count growth, skeleton poses and blend shapes. Scenes
  that animate material inputs (including texture paths), geometry attributes/
  topology, or other camera/light parameters are detected at load and always
  use the fallback re-conversion. For node-only scenes that fallback runs
  whenever nothing moved; rigged/morphed/visibility/instancer scenes stay on
  the in-place path so loop seams and visibility holds do not re-convert.
  Hidden-at-load PointInstancer instances and growth slots get kept-but-hidden
  draws, so animated `invisibleIds`/instance counts can reveal them without
  re-conversion. Prototype switching requires a single-mesh prototype;
  multi-mesh prototype changes fall back to re-conversion.

## Tests

| Test | What it covers |
| --- | --- |
| `lmetalview_core_test` | ABI constants, mesh/material/texture/animation plumbing, retained-stage time scrubbing, in-place node/skin/morph updates, shared camera, UDIM/Ptex/EXR/KTX2 fixtures |
| `lmetalview-headless-*` | MSL compilation, offscreen render and PNG output for smoke, texture-format, instancing and non-mesh fixtures (skips with 77 when no Metal device) |
| `lmetalview-headless-animated-fast` | Loads a skinned/blend-shape asset at frame 1 and updates to frame 2 through the in-place GPU playback path |
| `lmetalview-headless-animated-light` | Updates an animated distant light through the in-place playback path |
| `lmetalview-headless-animated-pointinstancer` | Updates animated PointInstancer positions/orientations/scales in place |
| `lmetalview-headless-animated-camera` | Updates an animated authored camera in place |
| `lmetalview-headless-animated-visibility` | Updates time-sampled mesh visibility in place |
| `lmetalview-headless-animated-material` | Detects animated material inputs and falls back to re-conversion |
| `lmetalview-headless-animated-instance-visibility` | Updates animated PointInstancer `invisibleIds` in place |
| `lmetalview-headless-animated-instance-reveal` | Reveals an instance that was invisible at load |
| `lmetalview-headless-animated-proto-index` | Switches PointInstancer prototypes in place |
| `lmetalview-headless-animated-instance-count` | Grows and shrinks the PointInstancer instance count |

Registered by CTest when the core tests are enabled. The headless test is the
guard against shader regressions, because the MSL is compiled at runtime.

Format fixtures live in `tests/`: `udim-texture.usda` + four PNG tiles,
`ptex-texture.usda` + a synthetic constant-face `.ptx`, `exr-texture.usda` +
a small ZIP-compressed EXR, and `animated-xform.usda` for the timeline.

## Relationship to lusdview / lusdrender

The C++ core (`core/`) is also the scene source for the Windows viewer
[`ld3d12view`](../ld3d12view/README.md). When changing `lmv.h`, note that a
scene keeps the load options and `lmv_scene_set_time()` calls the progress
callback again, so `progress_user` must stay valid until `lmv_scene_free()`.

This example intentionally does **not** reuse lusdview's `core_scene_loader`
(~10k lines coupled to the GPU ray tracer's `DrawScene`) or its metal-cpp/GLFW
backend. It shares the pieces that are genuinely reusable — the orbit camera,
`light3d` math, the `tydra` OpenPBR packing, and the core C API — and keeps the
Swift/Metal side small enough to read in one sitting.
