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
- Hair utility nodes `deon_hair_absorption_from_melanin`,
  `chiang_hair_absorption_from_color`, and `chiang_hair_roughness` preserve the
  pinned MaterialX formulas and named roughness outputs. The resulting hair
  transport remains an explicitly bounded approximation.
- `conical_edf` preserves its authored color, normal, inner/outer angles, and
  smooth angular falloff through surface, path, area-light, and raster emission.
- `generalized_schlick_edf` preserves its base EDF and directional `color0`,
  `color90`, and exponent controls through the same emission transport.
- MaterialX `light`, `point_light`, `directional_light`, and `spot_light`
  constructors compile as typed light-shader values. Point, directional, and
  spot results retain their authored position/direction, color, intensity,
  decay, and cone-angle controls for scene-light integration; the generic
  `light` node applies nonnegative intensity and exposure in linear radiance
  space. The `volume` and `volumematerial` constructors preserve their VDF/EDF
  chain; homogeneous volume emission
  contributes along traveled segments, while spatially varying emission still
  fails explicitly until its estimator is implemented. Volume-only USD
  materials receive a unit-IOR transmissive boundary so their mesh interior
  enters the authored medium without inventing surface emission.
- `measured_edf` accepts bounded LM-63 IES files from source-aware USD asset
  resolution. The parser retains bounded vertical and horizontal candela grids;
  the shader bilinearly interpolates both angles in a deterministic tangent
  frame around the authored EDF axis. Malformed, oversized, empty, or
  ambiguous profiles fail with diagnostics; inline `TILT=INCLUDE` tables and
  bounded external tilt files are applied relative to the photometric asset.
- `UsdPreviewSurface` now preserves `useSpecularWorkflow` and `specularColor`
  in the compiled lobe, including spectral conversion of authored color.
- Standard Surface and OpenPBR `specular_color` now tint the IOR-derived or
  metallic Fresnel color instead of being accepted and ignored.
- `generalized_schlick_bsdf` now carries the pinned `color82` control and
  Hoffman Schlick correction through RGB and spectral lobe evaluation. Its
  supported authored path is static GGX reflection; transmission,
  retroreflection, and custom distribution fail with structured diagnostics
  instead of being silently ignored. Authored normal and tangent vectors
  propagate to the enclosing surface frame.
- `translucent_bsdf` uses a bounded opposite-hemisphere diffuse transmission
  lobe and preserves its authored color and weight.
- Direct `sheen_bsdf` nodes use the pinned Imageworks sheen NDF/BRDF with
  bounded cosine sampling, or the pinned Zeltner LTC equations with matched
  transformed-cosine sampling and PDF evaluation.
- The pinned float, color3, and color4 `blur` overloads follow the MaterialX
  1.39.5 standard library implementation, which is currently a documented
  pass-through for box and gaussian filter types.
- `blackbody` matches the pinned GLSL Kelvin-to-xy-to-linear-Rec.709
  approximation, defaults to 5000 K, clamps temperature to 800–25000 K, and
  retains HDR RGB values. This is chromaticity with normalized Y, not spectral
  Planck radiance. `plus`/`minus` use `fg`, `bg`, and `mix` compositing ports.
  `bump`/`bump3` evaluate connected height graphs at four central-difference
  offsets. The direct emitter matches the pinned `NG_bump_vector3` composition
  (heighttonormal followed by normalmap), including its UV-space 1/16 scale.
  World derivatives offset position-dependent height graphs. Constant heights
  leave normals unchanged. `heighttonormal` separately emits encoded tangent-space
  normals with the pinned library's 1/16 scale and authored texcoord Jacobian.
- `colorcorrect` applies hue, luminance saturation, signed gamma, lift, gain,
  contrast, then exposure in pinned-library order; color4 alpha is unchanged.
  `switch` selects among ten typed value inputs
  using the authored index and preserves explicit defaults.
- PBR utility nodes `artistic_ior`, `roughness_anisotropy`, and
  `glossiness_anisotropy` preserve the pinned MaterialX conversion formulas,
  including multi-output artistic conductor IOR/extinction results.
  Numeric checks cover the square-before-clamp rule and 1e-8 roughness floor,
  anisotropy bounds, and unclamped edge-color extrapolation. Identity normal
  transforms preserve vector magnitude, including zero; they do not normalize.
- Transform aliases `transformnormal`, `transformpoint`, and `transformvector`
  accept world-space identity aliases and reject unsupported non-world
  conversions; `trianglewave` has the library's period 1 and peak 0.5.
- Image nodes backed by caller-decoded RGBA float resources in both modes:
  closest/linear filtering; constant/clamp/periodic/mirror addressing; float,
  vector and color outputs; default colors; linear-light area-filtered mip chains.
  `UsdUVTexture` graphs additionally support static `st`, `fallback`, `scale`,
  `bias`, and named RGB/channel outputs through the same bounded sampler.
- USD preview `UsdPrimvarReader` resolves standard UV/geometry names with typed
  fallbacks, and `UsdTransform2d` applies scale, degree rotation, and translation.
  Authored UV slots exposed by native meshes are preserved and selected per
  material graph for `texcoord` indices and `uvN`/`uvSetN` readers.
  The image-node checker demo requires no downloaded textures.
- `geomcolor` reads the normalized primary RGBA geometry-color stream in both
  raster and path shading; scene packing, displacement refinement, and authored
  mesh extraction preserve the interpolated channel. Nonzero color indices fail
  explicitly until additional primvar streams are available. One authored
  float/vector2/vector3/vector4/color3/color4 custom geomprop channels are now
  snapshotted from composed mesh primvars, carried through the packed vertex ABI,
  and exposed to matching MaterialX `geompropvalue` readers. Constant, vertex,
  varying, uniform, and already-expanded face-varying streams are accepted when
  they provide one value per emitted render vertex. Common half/double and
  integer scalar/vector aliases are converted to the packed float32 ABI; more
  than eight channels, unexpanded streams, and unsupported types remain
  diagnosed or use the graph fallback.
- Authored tangent streams are decoded from native packed or float formats,
  carried through the WebGPU triangle buffer and displacement bake, and used
  for MaterialX tangent/bitangent inputs with derivative frames as fallback.
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
  authored USD lights for path modes. Asset provenance is retained across layer
  loading to resolve relative references inside variants. Authored distant
  lights preserve their world direction and radiance, including multiple
  nonmatching distant lights, external IES tilt files, and textureless dome lights
  contribute environment radiance, and rect lights are imported as emissive
  geometry. Point and sphere lights use bounded finite-radius emissive geometry
  to participate in path sampling, using the authored position or transform
  translation; realtime shading also evaluates bounded analytic direct
  contributions for point, sphere, spot, rect, disk, and cylinder lights with shadow checks.
  Cylinder lights use a bounded 24-segment side surface and a representative
  center sample; shaped sphere lights use a conical EDF and the same bounded
  emitter geometry. Rect, disk, and cylinder realtime lighting uses center samples while path transport
  preserves the finite emitter meshes and area sampling. The point representation
  approximates an ideal point source.
  Geometry lights duplicate a bounded authored mesh target when the composed
  binding range and relationship target are available; the native light payload
  preserves that target path. Their realtime contribution remains a
  representative center sample.
  A single textured dome is decoded through the authored USD asset resolver and
  sampled as a bounded latlong environment, including packed UDIM atlases;
  multiple same-sized textured domes are combined in linear RGB with their
  authored radiance scales.
- Procedural synthetic sphere scenes and small MaterialX arithmetic/stripe
  fixtures, deterministic numeric GPU tests, scene validation and EXR roundtrip.

## Not implemented

Full MaterialX coverage; remaining EDF/VDF closure semantics and exact layered
transport;
complete authored light/material sync semantics;
MaterialX subsurface_bsdf radius-to-transport conversion and true BSSRDF;
arbitrary authored primvar/shading-property maps beyond the supported geometry
aliases, UV slots, and eight bounded custom geomprop channels;
Catmull-Clark displacement refinement; faithful authored ShaderBall material
graphs and full-resolution map storage;
broad independent physical reference-image validation; complete ACEScg graph color management.
`setMode('reference')` fails rather than substituting the RGB preview. Explicit
unsupported surface inputs fail compilation. Transmissive materials select the
resumable transport path instead of silently becoming opaque in path-preview.

USD `UsdPreviewSurface.inputs:displacement` height inputs are promoted into the
existing displacement shader and bake/refinement path, including connected graph
expressions. Dedicated MaterialX/USD displacement terminals remain supported.

Physical cutout continuation advances past the hit point and preserves
throughput on the accepted coverage branch. Chrome analytic emission tests
verify fractional coverage and six transparent surfaces across dispatches.

Path and raster shading derive tangent frames from world-space geometry and UV
derivatives, including mirrored UV handedness and degenerate-UV fallbacks. Path
transport projects that frame onto the final shading normal before sampling,
so anisotropic lobes retain authored tangent orientation after normal mapping.
Chrome renders normal-as-emission fixtures for standard, rotated and mirrored
UVs in both modes. MikkTSpace matching and arbitrary custom primvar maps remain
separate work.

## Experimental reference-transport work

`path-physical` runs RGB transport; `path-spectral` samples one wavelength over
360–830 nm and accumulates CIE XYZ. Both retain path state across four-event
dispatches, without a bounce cutoff. Roulette begins after five scattering
events. They are not enabled under a misleading `reference` alias.

Implemented native nodes: `dielectric_bsdf` (R/T/RT, anisotropic GGX, smooth and
rough refraction, exact Fresnel), `conductor_bsdf` (complex Fresnel), uncompensated
`oren_nayar_diffuse_bsdf`, bounded `burley_diffuse_bsdf`, bounded
`chiang_hair_bsdf`, homogeneous `absorption_vdf`, `uniform_edf`, and `surface`.
Native closures use the enclosing authored/geometric normal and its preserved
tangent frame. Standard Surface/OpenPBR
remain approximate mappings. BSDF add, mix, select, switch, and scalar/color
weighting preserve up to sixteen lobes with mixture evaluation and sampling
PDFs; bounded closure expression nodes retain nested add/mix structure and
BSDF-over-BSDF layers use directional top-transmission weighting during
evaluation. Add/mix reject ambiguous combinations that carry two separate
interiors. BSDF-over-VDF layer attaches an interior after surface composition.
VDF add, mix, select, switch and scalar/color weighting compose bounded medium
coefficients. Active transmissive lobes must agree on interface IOR. Realtime
shading still uses a primary-lobe approximation.
Thin film, sheen, coat and multiple-scattering microfacet compensation remain
approximate. Layered closure sampling still uses the bounded lobe mixture and
does not yet provide full recursive interface continuation or exact layered
energy compensation.

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
violations at sampled events. Both homogeneous and delta-tracked scattering
events estimate directional-light radiance with HG phase evaluation and
first-boundary shadow transmittance, accepting a boundary only when its
material matches the active medium. This supports geometric random walks, not the MaterialX subsurface
albedo/radius parameterization. Cameras are assumed to start in vacuum; the
stack supports eight nested interiors and fails on overflow or mismatched
boundaries. Uniform-sphere environment direct sampling is included at medium
events; multiple-importance weighting between volume light strategies remains
future work.

Scene `lighting.environment` is an optional constant RGB radiance; optional
`lighting.directional` contains `direction` and `radiance`. Otherwise the original
procedural lights are used. The physical paths sample the environment and all
triangles (area-weighted), with BSDF/light MIS. Realtime does not reproduce the
new transmission and volume effects.

`document.displacementOutput` selects a MaterialX displacement shader, a float
normal displacement, or a world-space vector3 offset. USD Material terminals
with `outputs:displacement` or `outputs:mtlx:displacement` now translate the
typed displacement constructor into that path. `scene.displacementRefinement` (0–5, budget-limited) performs
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
budget, validates finite float32 values, and diagnoses missing files. Renderer
image storage defaults to `textureStorage: 'half'`, packing RGBA mip texels into
two 16-bit-float pairs and unpacking them in WGSL; `'float'` remains available
when full float32 texture precision is required. Changing texture storage or
texture budget options after a scene is loaded rebuilds the scene resources.
The optional `loadMaterialXResources(url, options)` API and URL control fetch
EXR/PNG/JPEG resources for image, tiledimage, gltf_image, gltf_normalmap,
gltf_colorimage, gltf_iridescence_thickness, gltf_anisotropy_image, UsdUVTexture,
latlongimage, and triplanarprojection nodes, including bounded
`<UDIM>`/`<UVTILE>` atlases.
It resolves file prefixes and source layers, uses streaming
byte budgets, preflights EXR dimensions before decoder allocation, and rejects
include cycles, cross-origin dependencies and redirects by default. EXR support
is single-part scanline; browser bitmap decoding supplies other image formats.
Caller-provided decoded data remains supported. Other color spaces fail.

ShaderBall's 7500x7500 ground map exceeds the current full-resolution float-buffer
budget. It is decoded through the bounded EXR reduction path and retained at the
configured pixel budget; native material serialization
is explicitly recorded as lossy and is not treated as an authored graph import.

Raster mip selection uses the base mesh UV derivatives, so transformed or
procedural UV graph derivatives are approximate. Direction-based `latlongimage`
lookups now use the same bounded shading footprint for mip selection; this is
an angular approximation because the context does not carry full direction
derivatives. Path tracing carries a bounded ray-cone footprint through secondary
bounces, while full ray differentials remain outstanding. Connected
filename/sampler inputs and full MaterialX colorspace inheritance are not
implemented. Caller-provided decoded image resources may also expose bounded
same-size `layers`; static `layer` selectors choose a packed layer. These images do not
complete authored ShaderBall material support.

`setOptions({time, frame})` supplies animation values to raster, preview, and
physical shading contexts. Changing either value resets accumulated path
samples so animated graph evaluation cannot reuse samples from another time.
Caller-provided decoded image resources may use `frames: [...]`; the resource
loader also expands bounded `####`, `<FRAME>`, and `%0Nd` filename patterns
using an authored integer `framerange`. Image nodes select those frames from
`frame`, `framerange`, and `frameoffset`, with bounded `constant`, `cycle`, and
`mirror` end actions.

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

- Forty-nine Node tests pass, including native closure diagnostics, image checks and EXR
  decoding through Three.js independently.
- 37 numeric WGSL cases pass at `1e-5 + 1e-4 * abs(expected)` tolerance.
- Four actual pinned library graph cases pass: scalar-gamma color range, ACEScg
  color transform, channel conversion, and normal-map decoding. These are not
  evidence for all 807 NodeDefs. Connected normal/normalmap outputs now feed
  native BSDF frames, with geometric-hemisphere correction during transport.
- The value-graph compiler also covers typed `distance`, `reflect`, and
  `refract` nodes, preserving vector port validation and WGSL-native semantics.
- Fresnel and facing-ratio value nodes use normalized direction/normal inputs,
  Schlick-compatible scalar Fresnel, and bounded exponents.
- `luminance` uses explicit linear Rec.709 coefficients, while `average`
  handles color/vector widths with generated typed dot products.
- `rgbtohsv` and `hsvtorgb` are available as bounded color3 graph operations
  for authored procedural color networks.
- Common angle, exponential, and hyperbolic math nodes (`atan`, `log10`,
  `exp2`, `exp10`, `log2`, `sinh`/`cosh`/`tanh`, `radians`, `degrees`)
  map to WGSL with explicit conversion constants.
- `acescg_to_lin_rec709` is a graph operation using the pinned MaterialX
  white-adapted matrix, matching the library validation fixture.
- The inverse `lin_rec709_to_acescg` operation is available with the derived
  inverse matrix for round-trip authored color graphs.
- Piecewise signed `lin_rec709_to_srgb` and `srgb_to_lin_rec709` transfer nodes
  match the resource decoder thresholds.
- The pinned cmlib gamma transforms cover 1.8/2.2 Rec.709, display Rec.709
  gamma 2.4, sRGB texture decoding, and gamma-encoded AP1 to linear Rec.709;
  color4 variants preserve alpha without applying color transforms to it.
- Adobe RGB and Display P3 cmlib transforms use the pinned 3x3 matrices for
  both linear and transfer-encoded inputs, with color4 alpha preserved.
- Matrix utilities cover typed `transpose`, `determinant`, `invertmatrix`, and
  `creatematrix` operations for the supported 3x3/4x4 overloads.
- `roughness_dual` follows the pbrlib dual roughness convention, including the
  negative-secondary fallback and squared/clamped outputs.
- The NPR `gooch_shade` graph is available with world-space normal, view, and
  authored light direction inputs.
- The procedural `cloverleaf` mask follows the pinned four-circle construction
  and defaults to the current UV stream.
- The procedural `hexagon` mask follows the pinned signed-distance construction
  with centered coordinates and a bounded radius.
- Tiled cloverleaf and hexagon masks apply UV tiling, offset, size, and staggered
  row controls with typed color3 outputs.
- Typed `select` branches enforce boolean conditions and matching output types,
  preserving MaterialX conditional graph semantics.
- Deterministic bounded `noise2d` and `noise3d` value nodes are available for
  procedural UV/position graphs. Their authored scale, amplitude and pivot
  controls are applied; unsupported multi-octave controls fail explicitly.
- Bounded `fractal2d` and `fractal3d` fBm nodes now support authored amplitude,
  octave, lacunarity, diminish, and coordinate inputs with an eight-octave cap.
- Bounded `worleynoise2d`/`worleynoise3d` nodes provide deterministic distance,
  solid-style, and hash channels over a fixed 3x3/3x3x3 neighborhood.
- `latlongimage` maps a normalized view direction to periodic longitude and
  clamped latitude, with authored rotation, typed fallback color, and bounded
  static or connected layer selection for decoded layered environment images.
- `splitlr` and `splittb` provide typed left/right and top/bottom UV mattes with
  authored centers and value branches.
- `ramp` and `ramp_gradient` support static color4 control points, standard/
  radial/circular/box coordinate shapes, and linear/smooth/step interpolation.
- `ramplr` and `ramptb` provide typed left/right and top/bottom UV-axis ramps.
- `checkerboard` applies authored UV tiling/offset and alternates two color3
  inputs with deterministic cell parity.
- `circle` and `line` provide bounded scalar UV masks using analytic distance
  tests for circles and rounded line segments.
- `hextiledimage` color3/color4 overloads implement the pinned three-sample
  hexagonal tile transform, randomized rotation/scale/offset ranges,
  luminance-weighted falloff, derivative-based mip selection, and bounded UDIM
  atlas sampling.
- `hextilednormalmap` applies the same tile transform to tangent-space normal
  samples, including green-channel flipping, tangent rotation, strength,
  gradient-based normal blending, and bounded UDIM atlas sampling.
- `flake2d` and `flake3d` expose the four MaterialX outputs (`id`, `rand`,
  `presence`, and `flakenormal`) from a bounded 3×3×3 priority search and GGX
  flake-normal construction.
- `grid` and `crosshatch` provide typed color3 UV patterns with tiling, offset,
  thickness, and staggered-cell controls.
- `tiledcircles` provides deterministic cell-local circle masks with authored
  tiling, offset, diameter, and staggering.
- `randomfloat` produces deterministic bounded values from authored input and
  integer seed hashes, interpolated over the authored min/max range.
- `randomcolor` derives deterministic hue, saturation, and brightness channels
  from the authored input/seed and converts them through the bounded HSV path.
- `viewdirection` now resolves to the world-space outgoing direction in path
  and raster shading contexts, with explicit stable fallbacks for displacement
  and standalone validation contexts.
- `unifiednoise2d`/`unifiednoise3d` dispatch among Perlin-like, cell, Worley,
  and fractal helpers with authored frequency, range, clamp, and octave controls.
- `cellnoise2d` and `cellnoise3d` provide non-interpolated integer-cell hash
  values for procedural masks.
- Standard compositing `burn`, `dodge`, and `overlay` support float/color3/
  color4 inputs with authored mix amounts; `disjointover` supports alpha-aware
  color4 compositing with bounded coverage and denominator handling.
- Porter-Duff `in`, `mask`, `matte`, `out`, and `over` support color4 inputs
  with explicit premultiplied-channel and alpha equations. `inside` and
  `outside` apply typed scalar masks to float/color3/color4 values.
- Common color utility nodes `premult`, `unpremult`, `contrast`, and
  `hsvadjust` now compile with typed ports and bounded alpha/value handling.
  `hsvadjust` follows the MaterialX `amount` vector3 contract and preserves
  color4 alpha; unpremultiplication clamps its divisor before WGSL selection.
- `normalize` uses finite fallback directions for vector2, vector3 and vector4
  inputs, preventing zero or non-finite authored values from propagating NaNs.
- `reorder` supports statically authored channel permutations with the same
  typed bounds checks as `swizzle`.
- `saturate` performs the specified luma interpolation with authored
  coefficients and preserves color4 alpha.
- Supplemental `safepower` preserves the sign of negative bases while applying
  the exponent to the absolute value.
- Supplemental `place2d` applies pivot, inverse scale, degree rotation and UV
  offset transforms with typed vector2 inputs, defaulting to current UVs.
- `rotate2d` now defaults its omitted rotation amount to zero degrees.
- Core `clamp`/`smoothstep` bounds, `mix` amount, and `invert` amount now use
  their MaterialX defaults when omitted, including typed vector fallbacks.
- Supplemental `ramp4` evaluates typed four-corner values with bilinear UV
  interpolation and defaults its coordinate input to current UVs.
- `screen` and `difference` compositing nodes compile for scalar, vector, and
  color values using explicit component-wise arithmetic.
- Boolean `and`, `or`, `xor`, and `not` nodes now compile with strict boolean
  ports for graph control expressions.
- `tiledimage` accepts the validated single-tile resource path, applies static
  `uvtiling` multipliers and `uvoffset` subtraction; paired static
  `realworldimagesize`/`realworldtilesize` values add their UV ratio, and that
  effective scale is also included in derivative-based mip LOD selection;
  bounded UDIM/UVTILE atlas expansion is supported for static filename inputs.
- `gltf_image` supports the MaterialX glTF image resource path, factor
  modulation, and the authored pivot/scale/rotate/offset UV transform.
- `gltf_normalmap` samples a bounded glTF normal image and applies it through
  the authored tangent basis with the same UV transform controls.
- `gltf_colorimage` preserves the glTF color and geometry modulation inputs and
  exposes authored RGB and alpha outputs.
- `gltf_iridescence_thickness` and `gltf_anisotropy_image` preserve their
  authored channel extraction and utility output mappings.
- Image nodes now support MaterialX `filtertype="cubic"` through a bounded
  16-tap cubic sampler with trilinear mip selection.
- `triplanarprojection` now resolves three image resources, projects them on
  the X/Y/Z planes, and blends the samples by normalized authored normals;
  each plane enforces decoded-resource colorspace compatibility, and its
  projection path derives per-plane mip LOD from the projected world-position
  derivatives. Degenerate normals use the shared safe-normal fallback instead
  of producing zero weights.
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
and checks typed ports. Standard `surfacematerial` wrappers are unpacked into
their surface and displacement terminals. Missing definitions, unknown inputs, cycles, time samples
and authored volume terminals now preserve their typed volume constructor,
including homogeneous EDF emission. Spatially varying volume emission remains
an explicit unsupported diagnostic. Asset inputs require
a synchronous caller-supplied resolver returning a resource key; no source-layer
anchor is guessed. Color literals and asset resolver requests retain the source
attribute's color space, with attribute overrides preceding inherited prim
settings as described in the [OpenUSD color guide](https://openusd.org/release/user_guides/color_user_guide.html).
Material prims may expose the translated surface through either
`outputs:mtlx:surface` or the generic `outputs:surface` terminal; both still
resolve through the exact MaterialX NodeDef path.
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
conversion while keeping GPU allocation bounded. Oversized uncompressed
single-part scanline EXRs now stream their offset-table rows directly into the
bounded area-box result, avoiding a full-resolution float allocation. Other EXR
compression modes still use the third-party decoder and therefore require a
future streaming decoder for the 7,500² ShaderBall ground map.
`loadMaterialXResources` exposes the same behavior through its explicit
`allowDownsample` option; the default remains fail-closed at `maxPixels`.
Authored EXR aliases (`lin_ap1_scene`, `srgb_rec709_scene`, `lin_rec709_scene`,
and `data`) are normalized before packing, while unknown metadata remains an
explicit error.
The graph/compiler comparison uses the same normalized form, so a MaterialX
alias and a decoded-resource canonical name do not spuriously disagree.
The API still does not enable faithful ShaderBall material rendering.

The Chrome ShaderBall gate checks a synthetic native USD material with a nested
graph interface and sRGB attribute metadata against an analytic linear emission
image, using the pinned `ND_surface` and `ND_uniform_edf` definitions. Conflicting
destination metadata must not recolor the connected source.

Standard Surface/OpenPBR terminals now accept the canonical 1.39.5 input spellings,
including Standard Surface's `specular_IOR` and OpenPBR's `specular_ior`, geometry
opacity/thin-wall fields, and common authored defaults. The current terminal is
still an approximate multi-lobe mapping. Subsurface, fuzz, sheen, coat and
thin-film controls are carried through bounded approximations; exact layered
energy compensation and reference closure semantics remain incomplete.

`UsdPreviewSurface` maps diffuse/metallic/roughness/IOR, emissive, occlusion,
opacity mode, normal, and bounded clearcoat inputs into the shared material
closure path. Authored `normal` vectors are decoded from USD's signed tangent
space convention through the generated shading frame; omitted normals use the
geometric frame. Nonzero displacement remains an explicit diagnostic.

The stdlib `surface_unlit` shader now preserves authored emission, bounded
transmission, and opacity without introducing direct-light shading.

The ShaderBall geometry adapter now retains native material IDs, serialized
material absolute paths, and subset-presence records per mesh binding. It creates
diagnostic slots for all authored material IDs and no longer decides the gold
material from a pathname heuristic. Per-face subset ranges are expanded to
per-triangle material IDs when the native render mesh provides them; invalid or
overlapping ranges are rejected. If composition exposes no ranges, binding-level
IDs remain the authoritative result.

`loadShaderBallGeometry({ authoredMaterials: true })` loads every `.mtlx` file
listed by the pinned 1.39.5 catalog (with bounded, same-origin validation) and
attempts strict translation of every composed Material prim through
`materialXFromUSD`. Successful documents are retained in
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

Realtime scene loads no longer wait for the physical path pipeline to compile;
that pipeline is compiled lazily and awaited only when physical or spectral
transport is selected. Live material replacement uses the same demand-driven
path, avoiding concurrent physical compilations while preserving path tracing.
Scene loads remain mode-independent: a caller may switch from the previous
physical mode to realtime immediately without inheriting an eager physical
compile from the prior scene.

MaterialX unit annotations are now validated against the supported vocabulary
(`degree`, `radian`, metric length/time units including the standard library's
`micron` spelling, `nanometer`, and related unitless forms) instead of being
rejected wholesale. Percent literals normalize to
unitless factors, radian annotations convert at angle-bearing nodes, and metric
thin-film thickness converts to nanometers before spectral Fresnel evaluation.
Other length/time values remain in the authored convention consumed by each
node implementation; unknown unit names still fail explicitly.

Path shading now supplies an isotropic UV ray-footprint estimate from camera
pixel size, hit distance, and triangle UV/world scale. Image nodes can select a
bounded mip level during path tracing instead of always sampling level zero;
the estimate is conservative and does not replace true ray differentials.
The Chrome harness keeps the image-backed physical reference case behind
`--include-image-path` while its path convergence is investigated; realtime
image validation remains part of the default gate.

Shading normals from normal/normalmap nodes are now clamped to the geometric
hemisphere before path and raster transport, preventing perturbed frames from
launching rays through the wrong side of a surface.

Physical paths also have a post-Russian-roulette depth guard at 64 events. It
terminates only pathological trajectories (and exposes a `depthTerminated`
counter); it does not truncate a dispatch or hide invalid transport errors.

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

The bounded subsurface lobe now has a distinct diffusion-profile evaluation and
cosine sampling branch, carrying authored radius and bounded anisotropy into
its profile instead of reusing the ordinary diffuse closure. True random-walk
BSSRDF transport, surface-distance coupling, and multiple scattering remain
future work.

OpenPBR and Standard Surface `subsurface_anisotropy` inputs now reach that
bounded profile and are clamped to the same finite range as direct
`subsurface_bsdf` anisotropy. This preserves the authored directional bias;
true anisotropic random-walk BSSRDF transport remains future work.

OpenPBR `subsurface_scale` now multiplies the authored radius channels before
they reach the bounded profile. Negative scales are clamped to zero and the
native lobe keeps its finite minimum radius.

The profile uses a bounded rational falloff to keep WGSL evaluation finite and
portable across realtime and path pipelines; it is not a measured diffusion
profile.

Authored `standard_surface normal` connections now populate `Material.normal`
and are used by both path and realtime transport, with geometric-normal
fallbacks and geometric offsets retained for robustness. The synthetic
`normalmap` scene is included in the hardware matrix. Bump/displacement and
normal-map texture filtering still need broader authored-asset coverage.

OpenPBR `geometry_normal` is now accepted as an alias for the authored normal
frame and is exercised by the synthetic `open-pbr-normal` scene. Tangent-space
normal-map filtering and geometric displacement remain separate limitations.

`normalmap` now accepts both MaterialX `color3` and `vector3` inputs, matching
the common image-node color representation without changing tangent-space
normal semantics.

Layered image nodes now accept connected integer layer selectors and clamp them
to the decoded layer range. Authoring a layer selector against a flat image is
reported as a resource error instead of being ignored.

The synthetic `normalmap-image` fixture now exercises a raw-color image node
feeding `normalmap`, including packed-resource binding and linear filtering;
authored texture mip residency and UDIM streaming remain future work; USD and
MaterialX UDIM atlases are decoded eagerly under bounded tile and byte limits,
and hex-tiled nodes sample those packed atlases. Its
focused spectral Chrome run currently stalls before a sample is reported, so
it is intentionally not in the default reference matrix until image-backed
spectral dispatch diagnostics are fixed. The fixture is available through the
focused realtime Chrome mode for raster-resource validation.

The synthetic `ops` fixture now chains `rotate2d`, `extract`, `remap`,
`ifgreater`, and `combine3` into a surface color, adding graph-operation
coverage beyond the earlier arithmetic/wave scene.

The `ops-advanced` fixture extends this with typed vector conversion/swizzling,
component separation, power, dot/cross products, and equality selection.

`geompropvalue` now resolves standard static geometry-property tokens
(`st`/`uv`, `P`, `N`, `T`, and `B`) against the shading context with strict
output typing. `color`/`displayColor` resolve the interpolated geometry color,
and `opacity`/`displayOpacity` resolve its authored alpha channel. Unknown properties use the authored `default` value, preserving
MaterialX fallback behavior; dynamic selectors remain explicit diagnostics
because the current scene context has no arbitrary primvar map.

The standard `geompropvalueuniform` alias uses the same typed geometry lookup
and fallback behavior. `facingratio` follows the pinned NPR graph's
view-direction, face-forward, and invert controls; the separate
`facing_ratio` utility retains its exponent-based semantics.

`rotate3d` now emits a normalized-axis Rodrigues rotation with MaterialX's
degree-valued amount, including a stable fallback for degenerate axes.

The `layered` fixture now compiles and raster-tests an Oren–Nayar top lobe over
an anisotropic VDF interior, exercising BSDF-over-medium attachment in a scene.
Nested BSDF-over-BSDF graphs are preserved in a bounded closure expression tree;
the transport validation shader compiles a two-level Oren-Nayar/sheen/Burley
layer graph to guard the recursive representation.

The `edf` fixture covers a `uniform_edf` connected to a surface terminal,
including emission-only material compilation in the realtime target.

The `native-film` fixture exercises thin-film thickness/IOR propagation on a
direct `dielectric_bsdf`, including film Fresnel evaluation and sampling.
Its focused Chrome pipeline run remains pending while the native-film shader
compile stall is isolated.

The same thin-film controls now propagate through direct `conductor_bsdf`
closures, modulating complex-conductor Fresnel in evaluation and delta
sampling.

The synthetic displacement fixture now requests two levels of bounded linear
triangle refinement before the GPU displacement bake, exercising interior
height samples instead of only moving the original triangle corners.

Displacement bake normals now accumulate across matching material/UV vertices,
preserving seams while avoiding a flat-normal assignment on refined surfaces.

Realtime raster shading now honors compiled opacity with a zero-opacity discard
and bounded color scaling. The synthetic `opacity` scene exercises this path;
full sorted transparency and blend compositing remain future work.

Opacity now scales both environment and direct-light raster contributions, so
partial-opacity materials do not retain an opaque direct-light term.

The compute preview's realtime branch applies the same opacity factor to
emission and direct/environment lighting; physical path modes still require
stochastic transparency and sorted compositing for full transmission parity.

The physical preview now uses stochastic alpha continuation for partial
opacity: rejected hits advance along the current ray, while surviving hits
retain unbiased throughput. This is alpha cutout/coverage handling, not sorted
transparency or volumetric transmission.

Realtime raster lighting now uses the authored `Material.normal` for BRDF and
environment terms while retaining the geometric normal for offsets and shadow
queries. The normal-map hardware fixture exercises this parity path.

Realtime raster transmission now blends a bounded environment-refraction term
using the compiled IOR, transmission weight, and transmission color. This is
an approximation without screen-space thickness, local refraction rays, or
volume transport.


MaterialX bump derivatives now reevaluate connected image/procedural graphs in
isolated shading contexts. Offsets update UV and local planar position, not
curvature or arbitrary geometry properties. Nesting is limited to two derivative
nodes and graph expansion to 32768 expressions. Finite differences are not exact
analytic derivatives; discontinuities and extreme UV scales need broader tests.
The synthetic bump scene uses a varying sinusoidal height instead of a constant.
`--bump-only` verifies six analytic image chains (bump and
image -> heighttonormal -> normalmap, each with three UV orientations) in both
physical and raster modes. Chrome 152/NVIDIA Ampere passed these checks, plus
164 numeric cases and ten pinned-library cases; focused Node tests pass 85/85.
The latest comprehensive Chrome run failed with a 120-second physical-render
timeout and no reported GPU validation error. Focused passes do not supersede
that failure or establish reference readiness.

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

Standard Surface and OpenPBR coat controls now compile to a bounded dielectric
layer over the base closure, blended by `coat`/`coat_weight` and carrying color,
roughness and IOR aliases. The synthetic `coat` scene is hardware-tested; exact
layered energy compensation and multiple-scattering coat transport remain
future work.

Standard Surface `base` now scales the authored `base_color` contribution;
`base=0` therefore removes the diffuse base while preserving the other authored
lobes.

Standard Surface `sheen` and OpenPBR `fuzz` controls now compile to the same
bounded Imageworks sheen lobe used by direct `sheen_bsdf`, with authored color
and weight aliases. The synthetic `sheen` scene is hardware-tested; exact
Charlie/artist-friendly sheen and layered energy compensation remain future
work.

Standard Surface `sheen_roughness` and OpenPBR `fuzz_roughness` now control the
bounded Imageworks sheen lobe roughness, with clamped fallback behavior. Exact
Charlie/fuzz distributions remain future work.

Standard Surface `thin_walled` now propagates into the compiled dielectric
lobe instead of being silently forced opaque. The synthetic `thin-walled`
scene exercises medium-stack-free glossy transmission in the hardware gate.

Standard Surface/OpenPBR `transmission_depth` and `transmission_scatter` now
survive graph compilation in the lobe and apply bounded Beer-Lambert
attenuation to transmitted paths, including spectral conversion. The synthetic
`transmission-depth` scene is hardware-tested; full distance-aware random-walk
volumes remain a separate model.

`subsurface_bsdf` now accepts the MaterialX `albedo` alias as well as `color`,
and applies a nonnegative scalar `scale` to all three authored radius channels
before evaluating the bounded same-surface-point profile. Authored normals,
tangents, radius channels and anisotropy are preserved. This remains an
approximation; it does not perform cross-surface random-walk BSSRDF transport.

VDF `add` and `mix` now compose absorption, scattering, emission and
scattering-weighted anisotropy into a bounded `Medium`. Heterogeneous spatial
composition and distance-aware random-walk volume semantics remain separate
limitations.

`generalized_schlick_bsdf` now preserves authored color-at-normal, color-at-
82, color-at-grazing, roughness and exponent controls in a bounded microfacet
lobe rather than collapsing to the ordinary dielectric Fresnel curve. Its
current implementation accepts GGX reflection only and diagnoses authored
transmission, retroreflection, and custom-distribution variants. Authored
normal and tangent vectors propagate to the enclosing surface frame. The synthetic
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
to the bounded interference Fresnel path. RGB renders retain the fixed
three-channel approximation; spectral renders evaluate the film phase at the
sampled hero wavelength. The synthetic `open-pbr-film` scene is covered by
the Chrome matrix; full layered energy compensation remains future work.

OpenPBR `specular_weight` and Standard Surface `specular` now scale the
terminal microfacet contribution through the bounded lobe weight, while the
diffuse contribution remains separate. The OpenPBR synthetic scene exercises
this path; authored specular-color and full layered energy compensation remain
future work.

The same weight now controls the opaque specular sampling probability and PDF,
avoiding zero-contribution specular samples when the authored weight is low.

Authored Standard Surface/OpenPBR `specular_color` now tints the kind-0
microfacet response through the existing lobe storage, without changing the
WGSL layout. This is an RGB tint approximation; measured spectral and layered
specular semantics remain future work.
