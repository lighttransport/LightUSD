# WebGPU MaterialX implementation status

This is an experimental renderer, **not the completed full reference
renderer**. The original complete plan remains in the untracked repository-root
`webgpu-mtlx.md`. Existing WebGPU/Three.js demos are unchanged.

## Running

From `web/js`:

```sh
npm install
node scripts/prepare-webgpu-mtlx.mjs
npm run dev:webgpu-mtlx
```

On Windows PowerShell, use `npm.cmd` if execution policy blocks `npm.ps1`.
The isolated Vite configuration permits synthetic scenes without WASM. For the
ShaderBall geometry diagnostic, also run the standard
`web/demo/scripts/prepare-local-lightusd.sh` in an Emscripten environment.
This produces the combined and next-only modules in ignored output locations.

Asset setup pins USD assets to `3b75c2dad6a494897557dcca0098257bcf42a8c6` and
MaterialX 1.39.5 to `7b64921ef1d42f2d57871e9d2c43dc11f041f26b`. Downloads live
in `.cache/lightusd-verification` at the repository root. Existing checkouts
with a different revision or local edits are rejected without modification.
Retain upstream license/attribution files. `USD_WG_ASSETS_DIR` and
`MATERIALX_DIR` can override the respective local server roots.

## Implemented

- Typed numeric graph-to-WGSL compilation, named outputs, nested graph-defined
  NodeDefs, inheritance, interface binding, shared expressions and diagnostics.
  XML import rejects DTD/entity declarations. The URL loader resolves bounded,
  same-origin whole-document includes and source-relative images. Arbitrary
  source nodes and units remain unsupported. Authored sRGB/ACEScg color literals
  and images convert to the current linear Rec.709 working space.
- Scalar/vector value emitters and an explicitly approximate Standard Surface /
  OpenPBR terminal mapping. The UI inventories 807 upstream NodeDefs, but this
  count is **not a supported-node count**; individual overloads remain unverified.
- Image nodes backed by caller-decoded RGBA float resources in both modes:
  closest/linear filtering; constant/clamp/periodic/mirror addressing; float,
  vector and color outputs; default colors; linear-light area-filtered mip chains.
  The image-node checker demo requires no downloaded textures.
- Worker-built median triangle BVH, stackless compute traversal, per-hit UV/value
  evaluation, GGX/diffuse RGB path preview with progressive accumulation and a
  12-bounce limit. The preview uses a procedural sky and directional light.
- Raw WebGPU raster preview, traced hard shadows, adjustable internal resolution
  and an optional 30-fps resolution controller. Lighting remains approximate.
- Camera controls, pause/resume, accumulation invalidation, PNG and uncompressed
  float32 linear RGB EXR export. Exposure affects display only. EXR metadata
  explicitly records that the image is not a validated reference render.
- ShaderBall composition and authored-camera geometry diagnostic, selecting the
  triangulated variant (51,008 triangles in the pinned asset). It explicitly
  overrides materials and, by default, lighting. An optional checkbox imports
  the five authored RectLights for path modes. Asset provenance is retained across layer
  loading to resolve relative references inside variants.
- Procedural synthetic sphere scenes and small MaterialX arithmetic/stripe
  fixtures, deterministic numeric GPU tests, scene validation and EXR roundtrip.

## Not implemented

Full MaterialX coverage; general BSDF/EDF/VDF closure composition/layering;
MaterialX subsurface_bsdf albedo/radius conversion; hair and curves;
UDIM; bump and authored BSDF shading normals; Catmull-Clark displacement refinement;
faithful authored ShaderBall material graphs and full-resolution map storage;
broad independent physical reference-image validation; complete ACEScg graph color management.
`setMode('reference')` fails rather than substituting the RGB preview. Explicit
unsupported surface inputs fail compilation. Transmissive materials select the
resumable transport path instead of silently becoming opaque in path-preview.

## Experimental reference-transport work

`path-physical` runs RGB transport; `path-spectral` samples one wavelength over
360–830 nm and accumulates CIE XYZ. Both retain path state across four-event
dispatches, without a bounce cutoff. Roulette begins after five scattering
events. They are not enabled under a misleading `reference` alias.

Implemented native nodes: `dielectric_bsdf` (R/T/RT, anisotropic GGX, smooth and
rough refraction, exact Fresnel), `conductor_bsdf` (complex Fresnel), uncompensated
`oren_nayar_diffuse_bsdf`, `uniform_edf`, and `surface`. Native closures currently
use geometric normals and a generated tangent frame. Standard Surface/OpenPBR
remain approximate mappings. BSDF add, mix, and scalar/color weighting preserve
up to eight lobes with mixture evaluation and sampling PDFs. BSDF-over-VDF layer
attaches an interior after surface composition. Active transmissive lobes must
agree on interface IOR. Realtime shading still uses a primary-lobe approximation.
Thin film, sheen, coat, BSDF-over-BSDF layering and multiple-scattering microfacet
compensation remain missing.

Attach `document.spectra` curves as sorted `[wavelengthNm,value]` pairs, keyed by
`base_color`, `transmission_color`, `emission_color`, `ior`, `conductor_ior` or
`extinction`. Scalar IOR curves must cover 360–830 nm. RGB uplift is an explicit
assumption using PBRT v3 numerical bases, with illuminant white normalized to
CIE Y=1; measured data avoids this ambiguity. The CIE table is attributed under
CC-BY-SA-4.0 and the PBRT numerical tables retain their BSD license notice.

`document.mediumOutput` selects an `anisotropic_vdf` graph for the closed mesh
interior. Homogeneous absorption/scattering uses free-flight sampling and HG
phase sampling. Spatially varying coefficients require spectral mode and an
explicit conservative `document.mediumMajorant`; delta tracking checks for bound
violations at sampled events. This supports geometric random walks, not the
MaterialX subsurface albedo/radius parameterization. Cameras are assumed to start
in vacuum; the stack supports three nested interiors and fails on overflow or
mismatched boundaries. Volume direct-light sampling is not implemented.

Scene `lighting.environment` is an optional constant RGB radiance; optional
`lighting.directional` contains `direction` and `radiance`. Otherwise the original
procedural lights are used. The physical paths sample the environment and all
triangles (area-weighted), with BSDF/light MIS. Realtime does not reproduce the
new transmission and volume effects.

`document.displacementOutput` selects float normal displacement or a world-space
vector3 offset. `scene.displacementRefinement` (0–5, budget-limited) performs
linear triangle refinement. GPU graph evaluation and normal recomputation happen
before worker BVH construction; raster and paths share the displaced triangles.
Material changes that affect displacement rebake the source mesh. This is not
Catmull-Clark subdivision or a verified displacement-convergence implementation.

Float captures return per-pixel `sampleCounts` and, for resumable modes,
`variance` of the mean (NaN for fewer than two samples). Spectral float captures
and EXRs contain XYZ, not RGB mislabeled as XYZ. Invalid transport prevents further
rendering/capture until reset. No denoising or radiance clamping is applied.

The current render color convention is linear RGB without a validated ACEScg
pipeline. Do not use these outputs as ground truth or claim the full plan done.

### Decoded image resource API and limits

Attach `document.images[filename] = {width, height, data, colorspace}` before
loading a scene or replacing a material. `data` is unpremultiplied RGBA float
data with row zero at v=0. Colorspace is `lin_rec709` (default), `srgb_texture`
(RGB decoding only; alpha unchanged), `acescg` (or `ACEScg`), or explicitly
untransformed `raw` data. ACEScg conversion uses the pinned MaterialX cmlib matrix
and does not clamp out-of-gamut values.
The renderer packs resources into a storage buffer with a 64 MiB total mip
budget, validates finite float32 values, and diagnoses missing files.
The optional `loadMaterialXResources(url, options)` API and URL control fetch
EXR/PNG/JPEG resources. It resolves file prefixes and source layers, uses streaming
byte budgets, preflights EXR dimensions before decoder allocation, and rejects
include cycles, cross-origin dependencies and redirects by default. EXR support
is single-part scanline; browser bitmap decoding supplies other image formats.
Caller-provided decoded data remains supported. Other color spaces fail.

ShaderBall's 7500x7500 ground map exceeds the current full-resolution float-buffer
budget. It is rejected, not silently downsampled. Native material serialization
is explicitly recorded as lossy and is not treated as an authored graph import.

Raster mip selection uses the base mesh UV derivatives, so transformed or
procedural UV graph derivatives are approximate. Path preview currently uses
level zero: ray differentials/cones remain outstanding. Cubic filtering,
connected filename/sampler inputs, layers, image sequences, UDIM and full
MaterialX colorspace inheritance are not implemented. These images do not
complete authored ShaderBall material support.

## Verification

```sh
npm run test:webgpu-mtlx
node tests/webgpu-mtlx-chrome.mjs --hardware --shaderball --performance
node tests/webgpu-mtlx-chrome.mjs --hardware --shaderball --authored-lights
```

`CHROME_PATH` overrides the Chrome executable. Without `--hardware`, the focused
harness requests SwiftShader; software runs do not satisfy the hardware gate.
Screenshots and JSON reports go to `web/js/.regression/webgpu-mtlx`.

Verified on Chrome 152.0.7977.76, NVIDIA Ampere hardware:

- Twenty-seven Node tests pass, including native closure diagnostics, image checks and EXR
  decoding through Three.js independently.
- 37 numeric WGSL cases pass at `1e-5 + 1e-4 * abs(expected)` tolerance.
- Four actual pinned library graph cases pass: scalar-gamma color range, ACEScg
  color transform, channel conversion, and normal-map decoding. These are not
  evidence for all 807 NodeDefs. Normal maps are not yet consumed by native BSDFs.
- URL resource tests cover includes, source-relative EXR paths, inherited image
  colorspace, cycle rejection and cross-origin rejection. ShaderBall's 2048x2048
  neutral EXR decodes successfully.
- Authored RectLights use one-sided emissive geometry, world-space normalized
  area, and intensity/exposure scaling per the
  [UsdLux LightAPI definition](https://openusd.org/dev/user_guides/schemas/usdLux/LightAPI.html).
  Texture, shaping, temperature and nonphysical contribution overrides fail.
  The 192x128 eight-sample ShaderBall light-import smoke test passes; its materials
  remain diagnostic overrides, and the image is visibly noisy at eight samples.
- Ten analytic GPU image sampling cases pass (addressing, edge/default color,
  bilinear/trilinear and explicit mip levels), alongside textured path/raster
  scene loading and material replacement tests.
- Transport checks cover Fresnel/TIR, reflection/refraction reciprocity,
  32,768-sample dielectric energy/PDF consistency, Beer survival and HG mean.
  Spectral checks cover official CIE column sums, GPU XYZ integration, measured
  curves and white normalization. End-to-end constant RGB emitter radiance and
  zero variance, equal-energy spectral emitter Y, and baked displacement pass.
- Independent Lambert-furnace and lossless dielectric-slab scenes run with seeds
  1, 17 and 31337 at 8 and 32 spp. They compare analytic radiance to image means
  with four standard errors plus 1e-4 absolute tolerance. A systematic slab error
  exposed shared-edge intersection cancellation; ray-aligned shear intersection
  resolved it (32-spp means 0.92307/0.92371/0.92273, expected 0.92308).
  WGSL still lacks PBRT's double-precision edge fallback; broader robust-geometry
  coverage remains necessary. `setOptions({seed})` accepts uint32 seeds, resets
  accumulation, and records the seed in linear capture metadata.
- `--reference-images` adds four 192x128 32-spp spectral smoke renders for native
  conductor, rough dielectric, scattering media and displacement. These noisy
  smoke images are not independent full-scene reference comparisons.
- ShaderBall geometry, accumulation/reset, exposure, resize, worker scene loading,
  graph replacement and mode switching pass without GPU or page errors.
- Fixed 1280×720 ShaderBall raster diagnostic: latest 30-second run, 1,543 frames,
  median 19.2 ms, p95 21.7 ms. These measurements cover the diagnostic shaders,
  not the planned complete MaterialX realtime renderer.
- Combined and next-only WASM builds complete with cached Emscripten 4.0.8.
- Node regression profile in WSL: 20 reported passes, zero failures (the native
  validation-parity check is skipped because lusdcat is not built).

Full Windows `npm test` is not green: three existing USD tests construct an
invalid doubled-drive WASM path, and two dataset gates lack MuJoCo Menagerie.
The same Node profile passes under WSL. See `docs/regression.md` for full setup.

The combined WASM loader exposes `getShadingGraphJSON()` for inspection of
composed Shader, Material and NodeGraph properties directly from a loaded layer,
before schema reconstruction or render conversion.
The version-1 snapshot retains property types, connections and supported default
values separately, including connected defaults. `colorMetadataVersion: 1` adds
attribute `colorSpace` metadata and a `colorSpaces` map of authored ColorSpaceAPI
settings, including non-shading ancestors. Unsupported value types are explicit
markers; time-sampled properties are flagged, not evaluated. It is not yet a
complete import format: general metadata, full property-stack asset resolution,
binding resolution and remaining value types still need coverage. ShaderBall
retains this snapshot alongside the explicitly lossy render-material diagnostic.

`materialXFromUSD(snapshot, materialPath, { library, resolveAsset })` translates
the reachable surface graph into a compiler document. It resolves graph outputs
and interface inputs by absolute property path, selects exact library NodeDefs,
and checks typed ports. Missing definitions, unknown inputs, cycles, time samples
and authored displacement/volume terminals fail explicitly. Asset inputs require
a synchronous caller-supplied resolver returning a resource key; no source-layer
anchor is guessed. Color literals and asset resolver requests retain the source
attribute's color space, with attribute overrides preceding inherited prim
settings as described in the [OpenUSD color guide](https://openusd.org/release/user_guides/color_user_guide.html).
Supported canonical names map to the existing Rec.709, sRGB, ACEScg and raw
transforms; custom/unknown spaces fail explicitly. The graph working space is
still Rec.709, not the planned ACEScg working pipeline.

`USDTextureSources` records authored asset keys and their source-layer URLs before
composition remaps prims. Resolution succeeds only when all observed sources
agree on one URL. Missing, ambiguous, package/tiled and cross-origin paths fail.
Resource keys include color space so the same image can be used as color or data
without aliasing. ShaderBall retains the source index for inspection; this is not
a full USD property-stack resolver and can conservatively reject valid assets.
The snapshot's separate `assetPaths` list includes asset opinions on untyped
`over` prims, such as ShaderBall's inherited wall-material texture overrides.
Image-header color interpretation and legacy assets without color metadata still
need work; no color space is inferred from a filename containing `ACEScg`.
`packImages` accepts `maxDimension` and performs bounded area-box downsampling
before generating float mip levels. The renderer exposes this as
`textureMaxDimension` and `textureMaxBytes`, preserving alpha and colorspace
conversion while keeping GPU allocation bounded. EXR source decoding still
preflights pixel count before allocation; true scanline streaming/downsampling is
needed for the 7,500² ShaderBall ground EXR.
The API still does not enable faithful ShaderBall material rendering.

The Chrome ShaderBall gate checks a synthetic native USD material with a nested
graph interface and sRGB attribute metadata against an analytic linear emission
image, using the pinned `ND_surface` and `ND_uniform_edf` definitions. Conflicting
destination metadata must not recolor the connected source.

Standard Surface/OpenPBR terminals now accept the canonical 1.39.5 input spellings,
including Standard Surface's `specular_IOR` and OpenPBR's `specular_ior`, geometry
opacity/thin-wall fields, and common authored defaults. The current terminal is
still an approximate single-lobe mapping. Nonzero or connected subsurface, fuzz,
sheen, coat and thin-film controls fail explicitly; they are not silently erased.

The ShaderBall geometry adapter now retains native material IDs, serialized
material absolute paths, and subset-presence records per mesh binding. It creates
diagnostic slots for all authored material IDs and no longer decides the gold
material from a pathname heuristic. Per-face subset ranges are used when the
native render mesh provides them; the current ShaderBall asset exposes no such
ranges after composition, so binding-level IDs remain the authoritative result.

`loadShaderBallGeometry({ authoredMaterials: true })` loads the pinned
stdlib/pbrlib/bxdf libraries and attempts strict translation of every composed
Material prim through `materialXFromUSD`. Successful documents are retained in
`authored.translatedMaterials` and passed through the real WGSL `compileGraph`
path. Documents accepted by that compiler are retained in
`authored.compiledMaterials`; translation or compile failures are structured in
`authored.translationDiagnostics`. This is inspection-only until texture requests
are decoded into document images and all terminal/closure features are supported.
The optional Chrome `--authored-materials` run currently translates 10 composed
ShaderBall Material prims and attempts all 10 through the real WGSL path. The
seven example materials still report missing `outputs:mtlx:surface` terminals.
The authored inspection downsamples the 7,500² ground EXR into the bounded
image budget instead of rejecting it. It now covers opacity cutouts,
thin-wall continuation, approximate sheen/subsurface/translucent lobes,
interior-preserving closure composition, OpenPBR anisotropy and bounded
generalized-Schlick fallbacks. These mappings are explicitly approximate and do
not claim full MaterialX physical conformance.

Authored-render preflight now rejects oversized EXR maps from their header
before allocation and records a structured diagnostic; remaining maps use
bounded downsampling for the partial authored-material render.

The synthetic coverage set now includes a `hair` scene using the MaterialX
`hair_bsdf` category. Its legacy melanin and explicit-color forms compile to a
bounded fiber lobe with longitudinal/azimuthal roughness and are exercised by
the Chrome hardware reference-image gate. This is a practical approximation,
not a full Marschner/RTRT hair model.

Standard Surface `thin_film_thickness`/`thin_film_IOR` now carry through the
compiled lobe and use a bounded RGB interference Fresnel approximation. The
synthetic `thin-film` scene is included in the hardware reference-image gate;
spectral wavelength-dependent film optics remain future work.

Standard Surface and OpenPBR subsurface weights now compile as an explicit
closure mixture with a broad diffuse scattering lobe; authored subsurface color
and radius inputs are accepted and retained in the graph path. The synthetic
`subsurface` scene is hardware-tested, while true random-walk BSSRDF transport
and radius-dependent exit sampling remain future work.

Authored `subsurface_radius` now also controls the bounded scattering-lobe
roughness through its maximum channel, preserving a visible radius influence
without claiming a true BSSRDF or radius-dependent exit profile.

Authored `standard_surface normal` connections now populate `Material.normal`
and are used by both path and realtime transport, with geometric-normal
fallbacks and geometric offsets retained for robustness. The synthetic
`normalmap` scene is included in the hardware matrix. Bump/displacement and
normal-map texture filtering still need broader authored-asset coverage.

Scalar MaterialX `bump3` and `heighttonormal` nodes now have a bounded
height-to-normal fallback (`mxBumpHeight`) that feeds the same authored normal
path without mutating geometry. The synthetic `bump` scene is hardware-tested;
texture-aware finite-difference bump and true displacement/subdivision remain
separate follow-up work.

The Chrome harness accepts `--reference-samples=N` for bounded focused
diagnostics; the default and pre-merge matrix remain 32 spp.

Regular raster-image resources now honor `allowDownsample` as well as EXR:
oversized PNG/JPEG inputs are reduced into the configured pixel budget before
RGBA conversion, with `resizedFrom` provenance. Default decode still rejects
oversized images. The Chrome gate exercises both branches with a generated PNG.

The authored ShaderBall Chrome integration now reuses the loaded authored scene
for diagnostics instead of decoding the asset graph a second time. The hardware
run completes with seven compiled authored material slots, 51,008 triangles,
and no GPU/page errors; remaining slots stay explicit placeholders when their
composed terminal or texture preflight is unavailable.

Standard Surface and OpenPBR coat controls now compile to a bounded additive
dielectric coat lobe (`coat`, `coat_weight`, color, roughness and IOR aliases).
The synthetic `coat` scene is hardware-tested; exact layered energy
compensation and multiple-scattering coat transport remain future work.

Standard Surface `sheen` and OpenPBR `fuzz` controls now compile to a bounded
diffuse-sheen lobe with authored color and weight aliases. The synthetic
`sheen` scene is hardware-tested; exact Charlie/artist-friendly sheen and
layered energy compensation remain future work.

Standard Surface `sheen_roughness` and OpenPBR `fuzz_roughness` now control the
bounded diffuse-sheen lobe roughness, with clamped fallback behavior. Exact
Charlie/fuzz distributions remain future work.

Standard Surface `thin_walled` now propagates into the compiled dielectric
lobe instead of being silently forced opaque. The synthetic `thin-walled`
scene exercises medium-stack-free glossy transmission in the hardware gate.

Standard Surface/OpenPBR `transmission_depth` and `transmission_scatter` now
survive graph compilation in the lobe and apply bounded Beer-Lambert
attenuation to transmitted paths, including spectral conversion. The synthetic
`transmission-depth` scene is hardware-tested; full distance-aware random-walk
volumes remain a separate model.

`generalized_schlick_bsdf` now preserves authored color-at-normal, color-at-
grazing, roughness and exponent controls in a bounded microfacet lobe rather
than collapsing to the ordinary dielectric Fresnel curve. The synthetic
`generalized-schlick` scene passes the targeted Chrome hardware reference gate;
full measured/angle-dependent Schlick layering remains future work.

OpenPBR `base_weight` now scales the compiled base-color contribution instead
of being accepted and silently ignored. The synthetic `open-pbr-weight` scene
is included in the Chrome reference-image matrix; full OpenPBR energy-balanced
layer semantics remain future work.

Standard Surface `diffuse_roughness` and OpenPBR `base_diffuse_roughness` now
feed the bounded roughness path when no explicit specular roughness is authored.
This is an approximation because the current lobe shares one roughness field;
independent diffuse/specular roughness remains future work.

OpenPBR `thin_film_weight`, `thin_film_thickness`, and `thin_film_ior` now map
to the existing bounded RGB interference Fresnel path. The synthetic
`open-pbr-film` scene is covered by the Chrome matrix; wavelength-dependent
spectral film optics and full layered energy compensation remain future work.

OpenPBR `specular_weight` and Standard Surface `specular` now scale the
terminal microfacet contribution through the bounded lobe weight, while the
diffuse contribution remains separate. The OpenPBR synthetic scene exercises
this path; authored specular-color and full layered energy compensation remain
future work.
