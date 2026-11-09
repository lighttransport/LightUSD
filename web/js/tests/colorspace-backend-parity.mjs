// Color-management parity across the legacy and next WASM backends.

import assert from 'node:assert/strict';

import { loadWasm } from '../src/usdzconvert.js';
import { transformLinearColor } from '../src/lightusd/ColorSpaceUtils.js';

const encoder = new TextEncoder();
const legacy = await loadWasm(() => import(
  new URL('../src/lightusd/lightusd.js', import.meta.url).href));
const next = await loadWasm(() => import(
  new URL('../src/lightusd/lightusd_next.js', import.meta.url).href), {
  locateFile: (file) => new URL('../src/lightusd/' + file,
    import.meta.url).href
});

const COLOR_USDA = `#usda 1.0
(
    defaultPrim = "World"
    renderSettingsPrimPath = "/World/Settings"
)
def Xform "World"
{
    def RenderSettings "Settings"
    {
        uniform token renderingColorSpace = "lin_ap1_scene"
    }
    def Mesh "Triangle" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Mat>
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.25, 0.5, 0.75) (
                colorSpace = "srgb_rec709_scene"
            )
            token outputs:surface
        }
    }
}
`;

const MTLX_USDA = `#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def Mesh "Triangle" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Mat>
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
    def Material "Mat" (
        prepend apiSchemas = ["MaterialXConfigAPI"]
    )
    {
        string config:mtlx:version = "1.39"
        string config:mtlx:namespace = "mtlx"
        string config:mtlx:colorspace = "lin_ap1_scene"
        string config:mtlx:sourceUri = "lookdev/materials.mtlx"
        token outputs:mtlx:surface.connect = </World/Mat/OpenPBR.outputs:out>
        def Shader "OpenPBR"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            color3f inputs:base_color.connect = </World/Mat/Graph.outputs:base>
            float inputs:base_weight = 1
            token outputs:out
        }
        def NodeGraph "Graph"
        {
            color3f outputs:base.connect = </World/Mat/Graph/Constant.outputs:out>
            def Shader "Constant"
            {
                uniform token info:id = "ND_constant_color3"
                color3f inputs:value = (0.25, 0.5, 0.75) (
                    colorSpace = "srgb_rec709_scene"
                )
                color3f outputs:out
            }
        }
    }
}
`;

// Remove only the value's explicit metadata. The document-level
// MaterialXConfigAPI colorspace must then become the source opinion.
const MTLX_CONFIG_DEFAULT_USDA = MTLX_USDA.replace(
  `color3f inputs:value = (0.25, 0.5, 0.75) (
                    colorSpace = "srgb_rec709_scene"
                )`,
  'color3f inputs:value = (0.25, 0.5, 0.75)');
const MTLX_INHERITED_API_USDA = MTLX_CONFIG_DEFAULT_USDA
  .replace('prepend apiSchemas = ["MaterialXConfigAPI"]',
    'prepend apiSchemas = ["MaterialXConfigAPI", "ColorSpaceAPI"]')
  .replace('string config:mtlx:version = "1.39"',
    'uniform token colorSpace:name = "lin_rec709_scene"\n' +
    '        string config:mtlx:version = "1.39"');

const CUSTOM_TEXTURE_USDA = `#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World" (
    prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio_ap0"]
)
{
    uniform token colorSpaceDefinition:studio_ap0:name = "studio_ap0"
    float2 colorSpaceDefinition:studio_ap0:redChroma = (0.7348552434, 0.2642253252)
    float2 colorSpaceDefinition:studio_ap0:greenChroma = (-0.0061709125, 1.0113149590)
    float2 colorSpaceDefinition:studio_ap0:blueChroma = (0.0159675593, -0.0642355031)
    float2 colorSpaceDefinition:studio_ap0:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:studio_ap0:gamma = 1
    float colorSpaceDefinition:studio_ap0:linearBias = 0
    def Mesh "Triangle" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Mat>
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Mat/Tex.outputs:rgb>
            token outputs:surface
        }
        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @missing-custom.png@ (
                colorSpace = "studio_ap0"
            )
            token inputs:sourceColorSpace = "sRGB"
            float3 outputs:rgb
        }
    }
}
`;

function multiply3(matrix, color) {
  return [
    matrix[0] * color[0] + matrix[1] * color[1] + matrix[2] * color[2],
    matrix[3] * color[0] + matrix[4] * color[1] + matrix[5] * color[2],
    matrix[6] * color[0] + matrix[7] * color[1] + matrix[8] * color[2]
  ];
}

function assertNear(actual, expected, tolerance, label) {
  assert.equal(actual.length, expected.length, `${label}: channel count`);
  for (let i = 0; i < expected.length; ++i) {
    assert(Math.abs(actual[i] - expected[i]) <= tolerance,
      `${label}[${i}]: ${actual[i]} != ${expected[i]} (tol ${tolerance})`);
  }
}

function loadLegacy(usda, name) {
  const loader = new legacy.LightUSDLoaderNative();
  assert(loader.loadFromBinary(encoder.encode(usda), name), loader.error());
  return loader;
}

function loadNext(usda) {
  const stream = new next.RenderStream();
  const result = stream.begin(encoder.encode(usda));
  assert(result?.success, result?.error || stream.error());
  return stream;
}

// Both converters store constants in the selected working space. Applying the
// exported display matrix must recover the channel-distinct linear Rec.709
// values. A neutral gray would not catch a transposed or channel-swizzled
// working-space matrix.
const legacyColor = loadLegacy(COLOR_USDA, 'colorspace-legacy.usda');
let legacyMeta;
let legacyWorking;
try {
  legacyMeta = legacyColor.getSceneMetadata();
  const result = legacyColor.getMaterialWithFormat(0, 'json');
  assert.equal(result.error, undefined, result.error);
  legacyWorking = JSON.parse(result.data).surfaceShader.diffuseColor;
} finally {
  legacyColor.delete();
}

const nextColor = loadNext(COLOR_USDA);
let nextMeta;
let nextWorking;
try {
  nextMeta = nextColor.getSceneMetadata();
  const mesh = nextColor.getMesh(0);
  assert(!mesh.error, mesh.error);
  nextWorking = Array.from(mesh.material.baseColor);
} finally {
  nextColor.end();
  nextColor.delete();
}

assert.equal(legacyMeta.renderSettingsPrimPath, '/World/Settings');
assert.equal(nextMeta.renderSettingsPrimPath, '/World/Settings');
assert.equal(legacyMeta.workingColorSpace, 'lin_ap1_scene');
assert.equal(nextMeta.workingColorSpace, 'lin_ap1_scene');
const expectedDisplay = [0.05087609, 0.21404114, 0.52252155];
const legacyDisplay = multiply3(Array.from(legacyMeta.workingToDisplayLinear),
  legacyWorking);
const nextDisplay = multiply3(Array.from(nextMeta.workingToDisplayLinear),
  nextWorking);
assertNear(legacyDisplay, expectedDisplay, 3e-4, 'legacy display-linear');
assertNear(nextDisplay, expectedDisplay, 3e-4, 'next display-linear');
assertNear(nextDisplay, legacyDisplay, 3e-5, 'backend parity');

// The next binding exports the already-resolved transform because custom
// ColorSpaceDefinitionAPI properties are stage-local and unavailable once the
// RenderScene is handed to Three.js.
const nextCustomTexture = loadNext(CUSTOM_TEXTURE_USDA);
try {
  const mesh = nextCustomTexture.getMesh(0);
  assert(!mesh.error, mesh.error);
  const meta = mesh.material.textureMetadata.baseColor;
  assert.equal(meta.sourceColorSpace, 'studio_ap0');
  assert.equal(meta.colorTransformValid, true);
  assert.equal(meta.colorTransformBypass, false);
  assert.equal(meta.sourceColorIsData, false);
  assert.equal(meta.sourceGamma, 1);
  assert.equal(meta.sourceLinearBias, 0);
  assertNear(Array.from(meta.sourceToDisplayLinear), [
    2.521686, -1.134130, -0.387556,
    -0.276480, 1.372719, -0.096239,
    -0.015378, -0.152975, 1.168353
  ], 3e-4, 'next custom texture transform');
} finally {
  nextCustomTexture.end();
  nextCustomTexture.delete();
}

const legacyCustomTexture = loadLegacy(
  CUSTOM_TEXTURE_USDA, 'colorspace-custom-texture-legacy.usda');
try {
  const texture = legacyCustomTexture.getTexture(0);
  const image = legacyCustomTexture.getImageCopy(texture.textureImageId);
  assert.equal(image.sourceColorSpaceName, 'studio_ap0');
  assert.equal(image.colorTransformValid, true);
  assert.equal(image.colorTransformApplied, false);
  assert.equal(image.colorTransformBypass, false);
  assert.equal(image.sourceColorIsData, false);
  assert.equal(image.sourceGamma, 1);
  assertNear(Array.from(image.sourceToDisplayLinear), [
    2.521686, -1.134130, -0.387556,
    -0.276480, 1.372719, -0.096239,
    -0.015378, -0.152975, 1.168353
  ], 3e-4, 'legacy custom texture transform');
} finally {
  legacyCustomTexture.delete();
}

// `raw` is a numeric bypass. The working-space values must remain authored in
// both converters even when the selected rendering space is AP1.
const RAW_USDA = COLOR_USDA.replace(
  'colorSpace = "srgb_rec709_scene"', 'colorSpace = "raw"');
const legacyRaw = loadLegacy(RAW_USDA, 'colorspace-raw-legacy.usda');
let legacyRawWorking;
try {
  const result = legacyRaw.getMaterialWithFormat(0, 'json');
  assert.equal(result.error, undefined, result.error);
  legacyRawWorking = JSON.parse(result.data).surfaceShader.diffuseColor;
} finally {
  legacyRaw.delete();
}
const nextRaw = loadNext(RAW_USDA);
let nextRawWorking;
try {
  const mesh = nextRaw.getMesh(0);
  assert(!mesh.error, mesh.error);
  nextRawWorking = Array.from(mesh.material.baseColor);
} finally {
  nextRaw.end();
  nextRaw.delete();
}
const authoredRaw = [0.25, 0.5, 0.75];
const decodedChromatic = [0.05087609, 0.21404114, 0.52252155];
assertNear(legacyRawWorking, authoredRaw, 1e-7, 'legacy raw bypass');
assertNear(nextRawWorking, authoredRaw, 1e-7, 'next raw bypass');

// MaterialXConfig and property-level colorSpace metadata must survive graph
// reconstruction in both backends.
const legacyMtlx = loadLegacy(MTLX_USDA, 'colorspace-mtlx-legacy.usda');
try {
  const direct = legacyMtlx.getMaterialWithFormat(0, 'legacy');
  assert.equal(direct.materialXConfig.authored, true);
  assert.equal(direct.materialXConfig.version, '1.39');
  assert.equal(direct.materialXConfig.namespace, 'mtlx');
  assert.equal(direct.materialXConfig.colorspace, 'lin_ap1_scene');
  assert.equal(direct.materialXConfig.sourceUri, 'lookdev/materials.mtlx');
  const result = legacyMtlx.getMaterialWithFormat(0, 'json');
  assert.equal(result.error, undefined, result.error);
  const material = JSON.parse(result.data);
  assert.equal(material.hasOpenPBR, true);
  assertNear(material.openPBR.base.base_color.value, decodedChromatic, 3e-5,
    'legacy MaterialX nodegraph color transform');
  const input = material.openPBR.nodeGraph.nodegraph.nodes
    .flatMap((node) => node.inputs || [])
    .find((candidate) => candidate.name === 'value');
  assert.equal(input?.colorspace, 'srgb_rec709_scene');
} finally {
  legacyMtlx.delete();
}

const nextMtlx = loadNext(MTLX_USDA);
try {
  const mesh = nextMtlx.getMesh(0);
  assert(!mesh.error, mesh.error);
  assert.equal(mesh.material.materialXConfig.authored, true);
  assert.equal(mesh.material.materialXConfig.version, '1.39');
  assert.equal(mesh.material.materialXConfig.namespace, 'mtlx');
  assert.equal(mesh.material.materialXConfig.colorspace, 'lin_ap1_scene');
  assert.equal(mesh.material.materialXConfig.sourceUri,
    'lookdev/materials.mtlx');
  assertNear(Array.from(mesh.material.baseColor), decodedChromatic, 3e-5,
    'next MaterialX nodegraph color transform');
  const graph = JSON.parse(mesh.material.openPBRNodeGraphJson);
  const input = graph.nodegraph.nodes.flatMap((node) => node.inputs || [])
    .find((candidate) => candidate.name === 'value');
  assert.equal(input?.colorspace, 'srgb_rec709_scene');
} finally {
  nextMtlx.end();
  nextMtlx.delete();
}

// With no property metadata, MaterialXConfigAPI's AP1 document space is the
// source. The explicit-sRGB case above also proves that property metadata has
// higher precedence than the document default.
const expectedConfigured = transformLinearColor(
  authoredRaw, 'lin_ap1_scene', 'lin_rec709_scene');
const legacyMtlxConfigured = loadLegacy(
  MTLX_CONFIG_DEFAULT_USDA, 'colorspace-mtlx-config-legacy.usda');
let legacyMtlxConfiguredColor;
try {
  const result = legacyMtlxConfigured.getMaterialWithFormat(0, 'json');
  assert.equal(result.error, undefined, result.error);
  legacyMtlxConfiguredColor =
    JSON.parse(result.data).openPBR.base.base_color.value;
} finally {
  legacyMtlxConfigured.delete();
}
const nextMtlxConfigured = loadNext(MTLX_CONFIG_DEFAULT_USDA);
let nextMtlxConfiguredColor;
try {
  const mesh = nextMtlxConfigured.getMesh(0);
  assert(!mesh.error, mesh.error);
  nextMtlxConfiguredColor = Array.from(mesh.material.baseColor);
} finally {
  nextMtlxConfigured.end();
  nextMtlxConfigured.delete();
}
assertNear(legacyMtlxConfiguredColor, expectedConfigured, 3e-5,
  'legacy MaterialXConfig default transform');
assertNear(nextMtlxConfiguredColor, expectedConfigured, 3e-5,
  'next MaterialXConfig default transform');
assertNear(nextMtlxConfiguredColor, legacyMtlxConfiguredColor, 3e-5,
  'MaterialXConfig backend parity');

const legacyMtlxInherited = loadLegacy(
  MTLX_INHERITED_API_USDA, 'colorspace-mtlx-inherited-legacy.usda');
let legacyMtlxInheritedColor;
try {
  const result = legacyMtlxInherited.getMaterialWithFormat(0, 'json');
  assert.equal(result.error, undefined, result.error);
  legacyMtlxInheritedColor =
    JSON.parse(result.data).openPBR.base.base_color.value;
} finally {
  legacyMtlxInherited.delete();
}
const nextMtlxInherited = loadNext(MTLX_INHERITED_API_USDA);
let nextMtlxInheritedColor;
try {
  const mesh = nextMtlxInherited.getMesh(0);
  assert(!mesh.error, mesh.error);
  nextMtlxInheritedColor = Array.from(mesh.material.baseColor);
} finally {
  nextMtlxInherited.end();
  nextMtlxInherited.delete();
}
assertNear(legacyMtlxInheritedColor, authoredRaw, 3e-5,
  'legacy inherited ColorSpaceAPI precedence');
assertNear(nextMtlxInheritedColor, authoredRaw, 3e-5,
  'next inherited ColorSpaceAPI precedence');

console.log(JSON.stringify({
  pass: true,
  workingColorSpace: 'lin_ap1_scene',
  legacyDisplay,
  nextDisplay,
  rawBypass: authoredRaw,
  materialXConfig: true,
  nodeGraphColorSpace: 'srgb_rec709_scene',
  materialXConfigDefault: expectedConfigured,
  inheritedColorSpaceAPI: authoredRaw
}, null, 2));
