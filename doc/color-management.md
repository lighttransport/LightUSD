# Color management and rendering regression

LightUSD resolves USD color-space opinions in both the legacy Tydra converter
and the core/Tydra pair. The core implementation is the reference path;
the legacy implementation shares the same dependency-free transfer-function
and RGB-primary math.

## Resolution rules

An authored color value uses this precedence:

1. the property's `colorSpace` metadata;
2. the nearest ancestor with `ColorSpaceAPI` and `colorSpace:name`;
3. for MaterialX color inputs and image nodes, the enclosing Material's
   `MaterialXConfigAPI.config:mtlx:colorspace` document working space;
4. `lin_rec709_scene`.

The MaterialX document fallback applies only to color-typed values and color
texture parameters. Scalar/vector data maps (roughness, normals, masks, and
similar inputs) remain raw and are not reinterpreted through the document
working space. A property-level opinion always overrides the document default.

Constant MaterialX graphs convert each color-typed leaf into the document
working space before evaluating arithmetic, mix, HSV, and other utility nodes.
The graph result is converted once from the document space to the selected
rendering working space. This permits differently tagged inputs in one graph
without mixing encoded or mismatched-primary values.

The rendering working space uses an explicit converter/`RenderStream` override,
then stage metadata `renderSettingsPrimPath`, then `lin_rec709_scene`. The
selected `RenderSettings.renderingColorSpace` must identify a linear space.
Unknown or nonlinear working spaces produce a warning and deterministic linear
Rec.709 fallback.

Built-in OpenUSD names include linear AP0, AP1, Rec.709, P3-D65, Rec.2020,
Adobe RGB, CIE XYZ D65, sRGB/gamma-encoded Rec.709, AP1, P3-D65 and Adobe RGB,
plus data/raw identities. Common MaterialX and legacy aliases such as `acescg`,
`srgb_texture`, `lin_srgb`, and `sRGB` are canonicalized. Custom definitions
use the current multiple-apply form:

```usda
def Scope "World" (
    prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio"]
)
{
    uniform token colorSpaceDefinition:studio:name = "studio_linear"
    float2 colorSpaceDefinition:studio:redChroma = (0.64, 0.33)
    float2 colorSpaceDefinition:studio:greenChroma = (0.30, 0.60)
    float2 colorSpaceDefinition:studio:blueChroma = (0.15, 0.06)
    float2 colorSpaceDefinition:studio:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:studio:gamma = 1
    float colorSpaceDefinition:studio:linearBias = 0
}
```

The old non-instance API/property spelling remains readable for compatibility.
The USD validator checks duplicate, incomplete, invalid, and unknown
definitions and validates `renderingColorSpace`.

Texture cache identity includes the resolved asset path and effective color
transform. Reusing one image as color and data, or using scoped definitions
with the same token name, therefore retains separate converted image entries.

## Material and MaterialX behavior

Preview Surface and OpenPBR constant color inputs are transformed to the
selected linear working space during Tydra conversion. The render scene exports
the working-space token and a row-major `workingToDisplayLinear` matrix. Native,
Three.js, and WASM consumers apply that matrix before linear Rec.709/sRGB output.
Data parameters and alpha are never color transformed.

`MaterialXConfigAPI` fields (`version`, `namespace`, `colorspace`, and
`sourceUri`) are retained by legacy and next WASM material records. Its
`colorspace` is also evaluated as the MaterialX document-level fallback for
untagged color3/color4 constants and image nodes. MaterialX nodegraph
serialization in both backends retains effective per-input `colorspace`
metadata, including an inherited `ColorSpaceAPI` opinion. Constant evaluation
traces through NodeGraph outputs and utility nodes to the connected value input,
so that source opinion also drives the numeric transform instead of being lost
at the surface-shader terminal. Texture records retain their authored
source-space role and the resolved transfer/matrix so shader-side texture
decoding can distinguish color textures from raw/data maps even when the source
is a stage-local `ColorSpaceDefinitionAPI` name.

For `UsdUVTexture`, `colorSpace` metadata on `inputs:file` takes precedence
over `inputs:sourceColorSpace`. Native next and legacy sampling support the
built-in sRGB/gamma transfer curves and AP0, AP1, Rec.709, P3-D65, and
Rec.2020 primaries. The Three.js loaders use the texture's sRGB flag when
appropriate and inject the remaining transfer/gamut operation into the map or
emissive-map fragment shader, avoiding an 8-bit canvas conversion and its
wide-gamut clipping. Custom definitions use that same path in next and legacy;
eager legacy image conversion marks the transform as already applied to avoid
double conversion. Texture caches include the resolved transform coefficients,
so identically named custom definitions in different scopes cannot alias.

## Regression commands

Build native and both WASM backends first, then run:

```bash
# C++ conversion and schema/validation coverage
build_ninja/unit-test-lightusd tydra_renderscene_rendering_colorspace_test
build_ninja/unit-test-lightusd tydra_renderscene_custom_texture_colorspace_test
ctest --test-dir build-core --output-on-failure

# Native core/legacy render, PNG readback, expected constant pixel and texture
# checks (sRGB/raw, gamma 2.2, Rec.709/AP1, AP1/AP0, pixel-exact
# built-in/custom AP1 and piecewise-sRGB parity, plus MaterialXConfig constant
# and texture NodeGraph fallback versus explicit metadata in both backends)
python3 tools/lusdrender/check_colorspace_regression.py \
  build_ninja/tools/lusdrender/lusdrender . \
  --output /tmp/lightusd-colorspace-native

# Numeric AP0/Macbeth references plus legacy/core WASM parity, raw bypass,
# channel-distinct transforms, and MaterialX graph/config metadata
cd web/js
npm run test:colorspace-regression -- --wasm

# Browser Three.js float render-target readback (Macbeth plus
# AP0/AP1/gamma/custom-definition textures) and screenshot. This also loads an
# untagged MaterialXConfig material through the actual legacy and next WASM
# backends, adapts both material records to Three.js, and compares their pixels
# against the expected transform and each other. The batch runner starts and
# stops its own loopback Vite server when no URL is supplied.
npm run test:colorspace-render

# The two WASM checks are also CTest-registered in combined browser builds.
ctest --test-dir ../build_ninja -R 'wasm-regression-colorspace' \
  --output-on-failure
```

The browser batch writes a PNG screenshot and JSON pixel report under
`artifacts/colorspace` by default. Set `LIGHTUSD_COLORSPACE_URL` to use an
already-running server, or `LIGHTUSD_COLORSPACE_PORT` to select the self-hosted
port. `LIGHTUSD_COLORSPACE_OUTPUT` changes the output directory and
`LIGHTUSD_COLORSPACE_SERVER_TIMEOUT_MS` controls first-build startup timeout.
Float render-target readback is preferred; RGBA8 has a quantization-aware
tolerance. A physical Display-P3/HDR check is reported as skipped if the browser
and hardware do not advertise it. Transform correctness is still checked at the
pixel-value level, so wide-gamut display hardware is not required.

The Three.js calibration helpers are exported from
`web/js/src/lightusd/ColorCalibrationTestKit.js`:

- `createMacbethColorChart()` creates 24 emissive/reference patches;
- `createGrayAndChromeBalls()` creates an 18% gray ball and chrome mirror ball;
- `addColorCalibrationScene()` installs both in an existing Three.js scene;
- `readRenderTargetPixels()` and `comparePixelSamples()` support batch checks.

## Reference provenance

The 24 linear ACES2065-1 Macbeth values come from the local OpenColorIO test
checkout:

`~/work/OpenColorIO/tests/cpu/ops/fixedfunction/FixedFunctionOpCPU_tests.cpp`

Color-space names and MaterialX graph conventions were cross-checked against:

- `/mnt/disk1/work/MaterialX/resources/Materials/TestSuite/stdlib/color_management/native_color_management.mtlx`
- `/mnt/disk1/work/MaterialX/libraries/cmlib/cmlib_ng.mtlx`
- the local OpenUSD NanoColor implementation at
  `~/work/USD/pxr/base/gf/nc/nanocolor.c`

No external network reference is required for the checked-in values.
