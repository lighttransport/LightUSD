import * as THREE from 'three';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';
import {
  addColorCalibrationScene,
  comparePixelSamples,
  readRenderTargetPixels
} from './src/lightusd/ColorCalibrationTestKit.js';
import {
  installTextureColorTransform,
  textureColorTransform,
  transformLinearColor
} from './src/lightusd/ColorSpaceUtils.js';
import { loadWasm } from './src/usdzconvert.js';
import { RenderSceneAdapter } from './src/lightusd/LightUSDLoader.js';
import { buildThreeNode, createMaterial } from './src/lightusd/RenderSceneUtils.js';

const view = document.querySelector('#view');
const status = document.querySelector('#status');
const report = document.querySelector('#report');
const width = 960, height = 640;
const renderer = new THREE.WebGLRenderer({ antialias: false,
  preserveDrawingBuffer: true });
renderer.setSize(width, height, false);
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.NoToneMapping;
view.appendChild(renderer.domElement);

const scene = new THREE.Scene();
scene.background = new THREE.Color(0.01, 0.01, 0.01);
const environmentGenerator = new THREE.PMREMGenerator(renderer);
scene.environment = environmentGenerator.fromScene(new RoomEnvironment()).texture;
const camera = new THREE.OrthographicCamera(-2.2, 2.2, 1.47, -1.47, 0.01, 20);
camera.position.z = 5;
const kit = addColorCalibrationScene(scene, {
  chart: { patchSize: 0.38, gap: 0.035 }, ballsY: -1.05
});
scene.add(new THREE.HemisphereLight(0xffffff, 0x333333, 2.0));

const gl = renderer.getContext();
const floatSupported = renderer.capabilities.isWebGL2 &&
  !!gl.getExtension('EXT_color_buffer_float');
const target = new THREE.WebGLRenderTarget(width, height, {
  type: floatSupported ? THREE.FloatType : THREE.UnsignedByteType,
  format: THREE.RGBAFormat,
  depthBuffer: true
});
target.texture.colorSpace = THREE.NoColorSpace;

renderer.setRenderTarget(target);
renderer.render(scene, camera);
const points = kit.samples.map((sample) => {
  const p = sample.object.getWorldPosition(new THREE.Vector3()).project(camera);
  return { x: (p.x * 0.5 + 0.5) * width,
    y: (p.y * 0.5 + 0.5) * height };
});
const actual = readRenderTargetPixels(renderer, target, points);
const expected = kit.samples.map((sample) => sample.expectedLinear);
const tolerance = floatSupported ? 0.006 : 2.5 / 255;
const comparison = comparePixelSamples(actual, expected, tolerance);

// Render a real 8-bit texture through the same shader hook used by the next
// USD loader. This validates texture fetch -> AP0/AP1 gamut conversion ->
// float pixel readback without requiring a wide-gamut monitor.
function renderWideGamutTexture(sourceColorSpace, bytes, resolved = null) {
  const texture = new THREE.DataTexture(new Uint8Array([...bytes, 255]),
    1, 1, THREE.RGBAFormat, THREE.UnsignedByteType);
  const transform = textureColorTransform(
    sourceColorSpace, 'lin_rec709_scene', resolved);
  texture.colorSpace = transform.colorRole === 'color'
    ? THREE.SRGBColorSpace : THREE.NoColorSpace;
  texture.magFilter = THREE.NearestFilter;
  texture.minFilter = THREE.NearestFilter;
  texture.needsUpdate = true;
  const material = new THREE.MeshBasicMaterial({ map: texture,
    toneMapped: false });
  installTextureColorTransform(material, 'map', transform);
  const textureScene = new THREE.Scene();
  textureScene.add(new THREE.Mesh(new THREE.PlaneGeometry(2, 2), material));
  const textureCamera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.01, 2);
  textureCamera.position.z = 1;
  const textureTarget = new THREE.WebGLRenderTarget(8, 8, {
    type: floatSupported ? THREE.FloatType : THREE.UnsignedByteType,
    format: THREE.RGBAFormat,
    depthBuffer: false
  });
  textureTarget.texture.colorSpace = THREE.NoColorSpace;
  renderer.setRenderTarget(textureTarget);
  renderer.render(textureScene, textureCamera);
  const sampled = readRenderTargetPixels(renderer, textureTarget,
    [{ x: 4, y: 4 }])[0].slice(0, 3);
  const authored = bytes.map((value) => value / 255);
  const decoded = authored.map((value) => {
    if (transform.gamma === 1) return value;
    const magnitude = Math.abs(value);
    const bias = transform.linearBias || 0;
    if (bias > 0) {
      const k0 = bias / (transform.gamma - 1);
      const phi = (bias / Math.exp(Math.log(transform.gamma * bias /
        (transform.gamma + transform.gamma * bias - 1 - bias)) *
        transform.gamma)) / (transform.gamma - 1);
      return Math.sign(value) * (magnitude < k0 ? magnitude / phi :
        Math.pow((magnitude + bias) / (1 + bias), transform.gamma));
    }
    return Math.sign(value) * Math.pow(magnitude, transform.gamma);
  });
  const m = transform.matrix;
  let expectedLinear = [
    m[0] * decoded[0] + m[1] * decoded[1] + m[2] * decoded[2],
    m[3] * decoded[0] + m[4] * decoded[1] + m[5] * decoded[2],
    m[6] * decoded[0] + m[7] * decoded[1] + m[8] * decoded[2]
  ];
  if (!floatSupported) {
    expectedLinear = expectedLinear.map((value) =>
      Math.round(Math.max(0, Math.min(1, value)) * 255) / 255);
  }
  textureTarget.dispose();
  material.dispose();
  texture.dispose();
  return { sourceColorSpace, authored, actual: sampled,
    expected: expectedLinear };
}

const ap1Builtin = textureColorTransform('lin_ap1_scene');
const textureSamples = [
  renderWideGamutTexture('lin_ap1_scene', [82, 118, 104]),
  renderWideGamutTexture('lin_ap0_scene', [48, 52, 80]),
  renderWideGamutTexture('g22_rec709_scene', [128, 64, 192]),
  renderWideGamutTexture('studio_ap1', [82, 118, 104], {
    colorTransformValid: true,
    colorTransformBypass: false,
    sourceColorIsData: false,
    sourceGamma: 1,
    sourceLinearBias: 0,
    sourceToDisplayLinear: ap1Builtin.matrix
  }),
  renderWideGamutTexture('studio_piecewise', [128, 64, 192], {
    colorTransformValid: true,
    colorTransformBypass: false,
    sourceColorIsData: false,
    sourceGamma: 2.2,
    sourceLinearBias: 0.05,
    sourceToDisplayLinear: [1, 0, 0, 0, 1, 0, 0, 0, 1]
  })
];
const textureComparison = comparePixelSamples(
  textureSamples.map((sample) => sample.actual),
  textureSamples.map((sample) => sample.expected), tolerance);
const customDefinitionComparison = comparePixelSamples(
  [textureSamples[3].actual], [textureSamples[0].actual], tolerance);

// Exercise the core scene adapter and direct stream material paths, followed by
// their production Three.js material adapters and float render-target
// readback. The source value is untagged MaterialX/AP1; MaterialXConfigAPI is
// therefore the only source-space opinion, while RenderSettings keeps the
// intermediate material values in AP1 until each adapter applies the exported
// working-to-display matrix.
const BACKEND_MTLX_USDA = `#usda 1.0
(
  defaultPrim = "World"
  renderSettingsPrimPath = "/World/Settings"
)
def Xform "World" {
  def RenderSettings "Settings" {
    uniform token renderingColorSpace = "lin_ap1_scene"
  }
  def Mesh "Triangle" (
    prepend apiSchemas = ["MaterialBindingAPI"]
  ) {
    rel material:binding = </World/Mat>
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
  }
  def Material "Mat" (
    prepend apiSchemas = ["MaterialXConfigAPI"]
  ) {
    string config:mtlx:colorspace = "lin_ap1_scene"
    token outputs:mtlx:surface.connect = </World/Mat/OpenPBR.outputs:out>
    def Shader "OpenPBR" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:base_color.connect = </World/Mat/Graph.outputs:base>
      token outputs:out
    }
    def NodeGraph "Graph" {
      color3f outputs:base.connect = </World/Mat/Graph/Constant.outputs:out>
      def Shader "Constant" {
        uniform token info:id = "ND_constant_color3"
        color3f inputs:value = (0.25, 0.5, 0.75)
        color3f outputs:out
      }
    }
  }
}`;

async function createBackendThreeMaterials() {
  const bytes = new TextEncoder().encode(BACKEND_MTLX_USDA);
  const next = await loadWasm(() => import(/* @vite-ignore */
    new URL('./src/lightusd/lightusd.js', import.meta.url).href));
  const adapter = await RenderSceneAdapter.create(next, bytes, 'colorspace-browser.usda');
  let adapterMaterial;
  try {
    const {node} = buildThreeNode(adapter, {skipTextures:true});
    node.traverse(object => {
      object.geometry?.dispose();
      if (!object.isMesh) return;
      const materials = Array.isArray(object.material) ? object.material : [object.material];
      for (const material of materials) {
        if (!adapterMaterial) adapterMaterial = material.clone();
        material.dispose();
      }
    });
    if (!adapterMaterial) throw new Error('Core adapter produced no material');
  } finally {adapter.delete();}

  const stream = new next.RenderStream();
  let nextMaterial;
  try {
    const begun = stream.begin(bytes);
    if (!begun?.success) throw new Error(begun?.error || stream.error());
    const mesh = stream.getMesh(0);
    if (mesh.error) throw new Error(mesh.error);
    nextMaterial = createMaterial({
      material: mesh.material,
      texturePaths: {}
    }, {}, null, true);
  } finally {
    stream.end();
    stream.delete();
  }
  return { adapter: adapterMaterial, stream: nextMaterial };
}

function renderBackendMaterialPixels(materials) {
  const backendScene = new THREE.Scene();
  const geometry = new THREE.PlaneGeometry(0.8, 0.8);
  const backendNames = ['adapter', 'stream'];
  const previewMaterials = [];
  for (let i = 0; i < backendNames.length; ++i) {
    const source = materials[backendNames[i]];
    const preview = new THREE.MeshBasicMaterial({
      color: source.color.clone(), toneMapped: false
    });
    previewMaterials.push(preview);
    const mesh = new THREE.Mesh(geometry, preview);
    mesh.position.x = i === 0 ? -0.5 : 0.5;
    backendScene.add(mesh);
  }
  const backendCamera = new THREE.OrthographicCamera(-1, 1, 0.5, -0.5,
    0.01, 2);
  backendCamera.position.z = 1;
  const backendTarget = new THREE.WebGLRenderTarget(16, 8, {
    type: floatSupported ? THREE.FloatType : THREE.UnsignedByteType,
    format: THREE.RGBAFormat,
    depthBuffer: false
  });
  backendTarget.texture.colorSpace = THREE.NoColorSpace;
  renderer.setRenderTarget(backendTarget);
  renderer.render(backendScene, backendCamera);
  const pixels = readRenderTargetPixels(renderer, backendTarget,
    [{ x: 4, y: 4 }, { x: 12, y: 4 }]).map((pixel) => pixel.slice(0, 3));
  backendTarget.dispose();
  geometry.dispose();
  for (const material of previewMaterials) material.dispose();
  return pixels;
}

async function finishRegression() {
const backendMaterials = await createBackendThreeMaterials();
const backendPixels = renderBackendMaterialPixels(backendMaterials);
const backendExpectedLinear = transformLinearColor(
  [0.25, 0.5, 0.75], 'lin_ap1_scene', 'lin_rec709_scene');
const backendExpected = floatSupported ? backendExpectedLinear :
  backendExpectedLinear.map((value) =>
    Math.round(Math.max(0, Math.min(1, value)) * 255) / 255);
const backendComparison = comparePixelSamples(backendPixels,
  [backendExpected, backendExpected], tolerance);
const backendParityComparison = comparePixelSamples(
  [backendPixels[0]], [backendPixels[1]], tolerance);
backendMaterials.adapter.dispose();
backendMaterials.stream.dispose();
renderer.setRenderTarget(null);
renderer.render(scene, camera);

const displayP3Supported = matchMedia('(color-gamut: p3)').matches &&
  CSS.supports('color', 'color(display-p3 1 0 0)');
const result = {
  pass: comparison.pass && textureComparison.pass &&
    customDefinitionComparison.pass && backendComparison.pass &&
    backendParityComparison.pass,
  readback: floatSupported ? 'float32' : 'rgba8',
  tolerance,
  patches: actual.length,
  failures: comparison.failures,
  textureFailures: textureComparison.failures,
  customDefinitionFailures: customDefinitionComparison.failures,
  backendFailures: backendComparison.failures,
  backendParityFailures: backendParityComparison.failures,
  backendPixels: {
    adapter: backendPixels[0], stream: backendPixels[1],
    expected: backendExpected
  },
  textureSamples,
  displayP3: displayP3Supported ? 'available-not-required' : 'skipped-no-hardware',
  actual,
  expected
};
window.__colorRegression = result;
document.documentElement.dataset.regressionReady = 'true';
status.textContent = result.pass ? 'PASS' : 'FAIL';
status.className = result.pass ? 'pass' : 'fail';
report.textContent = JSON.stringify(result, null, 2);

}
finishRegression().catch(error => {
  window.__colorRegression = {pass:false,error:String(error)};
  document.documentElement.dataset.regressionReady = 'true';
  status.textContent = 'FAIL'; status.className = 'fail';
  report.textContent = String(error.stack || error);
  console.error(error);
});
