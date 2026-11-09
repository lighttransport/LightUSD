import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { OBJLoader } from 'three/examples/jsm/loaders/OBJLoader.js';
import { STLLoader } from 'three/examples/jsm/loaders/STLLoader.js';
import { weldMeshGeometry } from './src/mjcf-mesh-weld.js';
import * as BufferGeometryUtils from 'three/examples/jsm/utils/BufferGeometryUtils.js';
import GUI from 'three/examples/jsm/libs/lil-gui.module.min.js';
import URDFLoader from 'urdf-loader';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import {
  basenameFromUri,
  createConfiguredLightUSDLoader,
  getAssetUriFromURL,
  getBackendFromURL,
  makeStaticNextParseOptions,
  parseUSDSceneFromArrayBuffer
} from 'lightusd/LoaderConfigUtils.js';
import {
  buildNextThreeNode,
  isNextScene,
  nextCountsFromScene
} from 'lightusd/NextRenderSceneUtils.js';

const state = {
  robot: null,
  usdObject: null,
  usdRestObject: null,
  usdArticulation: null,
  // True when the USD view is a conversion of the current source robot (vs an
  // imported USD). Lets the "Export upAxis" toggle auto-reconvert.
  usdIsConverted: false,
  sourceGhost: null,
  usdGhost: null,
  usdLinkBindings: [],
  sourceRestLinkMatrices: new Map(),
  inputText: '',
  inputName: '',
  inputFormat: '',
  usdName: '',
  latestUSDBytes: null,
  latestUSDFormat: '',
  exportPayload: null,
  assetFiles: new Map(),
  objectUrls: new Map(),
  meshCache: new Map(),
  nativeMeshBuffers: new Map(),
  generatedTextureAssets: new Map(),
  lightusdLoader: null,
  nativeExporter: null,
  joints: {},
  jointValues: {},
  jointControls: new Map(),
  collisionMeshes: [],
  visualMeshes: [],
  muscleObjects: [],
  usdMuscleObjects: [],
  muscleLineData: null,
  sensorObjects: [],
  usdSensorObjects: [],
  sensorVisualData: null,
  sensorPreviewCameras: [],
  sensorPreviewIndex: 0,
  settings: {
    upAxis: 'Z',
    showVisuals: true,
    showCollisions: false,
    ignoreJointLimits: false,
    hideFixedJoints: true,
    animateJoints: false,
    animationSpeed: 0.8,
    autocenter: true,
    packageRoot: '',
    ghostUSDInSource: false,
    ghostSourceInUSD: false,
    ghostOpacity: 0.28,
    showAxisHelper: true,
    showLinkLines: false,
    showJointArrows: false,
    showLinkNames: false,
    showJointNames: false,
    labelScale: 1.0,
    jointsCollapsed: false,
    applyHomePose: false,
    showMuscles: true,
    showTendons: true,
    showSensors: true,
    renderSensorView: true,
    backend: getBackendFromURL(new URLSearchParams(window.location.search), 'legacy'),
    // Per-MuJoCo-geom-group visibility for the source view (groups 0-5), mirroring
    // the MuJoCo viewer's group toggles. Default: 0-2 shown, 3-5 hidden (the usual
    // visual/auxiliary split). Geoms with no group fall back to showVisuals/
    // showCollisions (e.g. URDF / converted-USD meshes carry no MuJoCo group).
    geomGroups: { 0: true, 1: true, 2: true, 3: false, 4: false, 5: false }
  }
};

const USD_MESH_EXTENSIONS = new Set(['.usd', '.usda', '.usdc', '.usdz']);
const SOURCE_EXTENSIONS = new Set(['.urdf', '.xml']);

const loadStats = {
  current: null,
  fps: 0,
  frames: 0,
  fpsLast: 0
};

function createViewScene() {
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x202124);
  const root = new THREE.Group();
  const ghostRoot = new THREE.Group();
  const debugRoot = new THREE.Group();
  const axisHelper = new THREE.AxesHelper(0.35);
  axisHelper.name = 'WorldAxesHelper';
  axisHelper.renderOrder = 30;
  axisHelper.visible = state.settings.showAxisHelper;
  scene.add(root);
  scene.add(ghostRoot);
  scene.add(debugRoot);
  scene.add(axisHelper);
  scene.add(new THREE.HemisphereLight(0xddeeff, 0x303030, 1.6));
  const dirLight = new THREE.DirectionalLight(0xffffff, 2.4);
  dirLight.position.set(4, 6, 3);
  dirLight.castShadow = true;
  scene.add(dirLight);
  scene.add(new THREE.GridHelper(20, 40, 0x586069, 0x33383d));
  return { scene, root, ghostRoot, debugRoot, axisHelper };
}

const sourceView = createViewScene();
const usdView = createViewScene();

const camera = new THREE.PerspectiveCamera(50, window.innerWidth / window.innerHeight, 0.01, 2000);
camera.position.set(4, 3, 6);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(window.devicePixelRatio);
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
document.body.appendChild(renderer.domElement);

const sensorPreviewPanel = document.createElement('div');
sensorPreviewPanel.style.cssText = [
  'position:fixed', 'right:12px', 'bottom:12px', 'z-index:8',
  'background:rgba(18,20,24,0.86)', 'border:1px solid rgba(255,255,255,0.16)',
  'border-radius:6px', 'padding:8px', 'color:#e7e9ee',
  'font:12px system-ui,sans-serif', 'display:none',
  'box-shadow:0 8px 24px rgba(0,0,0,0.28)'
].join(';');
const sensorPreviewLabel = document.createElement('div');
sensorPreviewLabel.style.cssText = 'margin-bottom:6px;max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap';
const sensorPreviewCanvas = document.createElement('canvas');
sensorPreviewCanvas.width = 256;
sensorPreviewCanvas.height = 160;
sensorPreviewCanvas.style.cssText = 'display:block;width:256px;height:160px;background:#111;border-radius:4px;cursor:pointer';
sensorPreviewPanel.append(sensorPreviewLabel, sensorPreviewCanvas);
document.body.appendChild(sensorPreviewPanel);
sensorPreviewPanel.addEventListener('click', () => {
  if (state.sensorPreviewCameras.length < 2) return;
  state.sensorPreviewIndex = (state.sensorPreviewIndex + 1) % state.sensorPreviewCameras.length;
  updateSensorPreviewPanel();
});

const controls = new OrbitControls(camera, renderer.domElement);
controls.target.set(0, 0.6, 0);
controls.update();

const robotGroup = sourceView.root;
const usdGroup = usdView.root;

const collisionMaterial = new THREE.MeshStandardMaterial({
  color: 0xffd23f,
  emissive: 0x332300,
  roughness: 0.58,
  metalness: 0.0,
  transparent: true,
  opacity: 0.42,
  depthWrite: false,
  polygonOffset: true,
  polygonOffsetFactor: -1,
  polygonOffsetUnits: -1
});

const sourceLinkLineMaterial = new THREE.LineBasicMaterial({ color: 0x4fc3ff, transparent: true, opacity: 0.9 });
const usdLinkLineMaterial = new THREE.LineBasicMaterial({ color: 0xffb24d, transparent: true, opacity: 0.9 });
const sourceJointMaterial = new THREE.MeshStandardMaterial({
  color: 0x26aee6,
  emissive: 0x062536,
  roughness: 0.54,
  metalness: 0.02,
  depthTest: false
});
const usdJointMaterial = new THREE.MeshStandardMaterial({
  color: 0xffb13b,
  emissive: 0x3a1f02,
  roughness: 0.54,
  metalness: 0.02,
  depthTest: false
});
const jointArrowShaftGeometry = new THREE.CylinderGeometry(0.012, 0.012, 0.078, 24, 1);
const jointArrowHeadGeometry = new THREE.ConeGeometry(0.027, 0.04, 24, 1);
const sourceLabelColor = '#8be9ff';
const usdLabelColor = '#ffd08a';

const statusEl = document.getElementById('status');
const panelEl = document.getElementById('panel');
const jointHeaderEl = document.getElementById('jointHeader');
const jointFoldIconEl = document.getElementById('jointFoldIcon');
const jointControlsEl = document.getElementById('jointControls');
const sourceLabelEl = document.getElementById('sourceLabel');
const usdLabelEl = document.getElementById('usdLabel');
const sourceExportButton = document.getElementById('exportSource');
const convertToUSDButton = document.getElementById('convertToUSD');
const convertToSourceButton = document.getElementById('convertToSource');
const fitViewButton = document.getElementById('fitView');
const backendSelect = document.getElementById('backendSelect');
const exportButtons = [
  document.getElementById('exportUSDA'),
  document.getElementById('exportUSDC'),
  document.getElementById('exportUSDZ')
];
const DEFAULT_USDC_EXPORT_LIMIT_MB = 2048;
const DEFAULT_MEM_EXPORT_LIMIT_MB = 4096;
const DEFAULT_SAMPLE_URDF = `<?xml version="1.0"?>
<robot name="lightusd_sample">
  <link name="base">
    <visual>
      <geometry><box size="1 1 0.3"/></geometry>
      <material name="blue"><color rgba="0.2 0.45 0.9 1"/></material>
    </visual>
    <collision><geometry><box size="1 1 0.3"/></geometry></collision>
    <inertial><mass value="1"/><inertia ixx="1" ixy="0" ixz="0" iyy="1" iyz="0" izz="1"/></inertial>
  </link>
  <link name="arm">
    <visual><origin xyz="0 0 0.6"/><geometry><cylinder radius="0.12" length="1.2"/></geometry></visual>
  </link>
  <joint name="shoulder" type="revolute">
    <parent link="base"/><child link="arm"/><origin xyz="0 0 0.15"/>
    <axis xyz="0 1 0"/><limit lower="-1.2" upper="1.2" effort="10" velocity="2"/>
  </joint>
</robot>`;

function parsePositiveInt(value) {
  const n = Number(value);
  return Number.isInteger(n) && n > 0 ? n : 0;
}

function resolveUSDCExportCapsFromRuntime() {
  const search = (typeof window !== 'undefined' && window.location?.search) ? window.location.search : '';
  const params = new URLSearchParams(search || '');

  const queryUsdc = parsePositiveInt(params.get('maxUsdcMb'));
  const queryMem = parsePositiveInt(params.get('maxMemMb'));
  const globalUsdc = parsePositiveInt(
    typeof window !== 'undefined' ? window.__LIGHTUSD_MAX_USDC_MB : undefined
  );
  const globalMem = parsePositiveInt(
    typeof window !== 'undefined' ? window.__LIGHTUSD_MAX_MEM_MB : undefined
  );

  return {
    maxUsdcMb: queryUsdc || globalUsdc || DEFAULT_USDC_EXPORT_LIMIT_MB,
    maxMemMb: queryMem || globalMem || DEFAULT_MEM_EXPORT_LIMIT_MB,
  };
}

function setStatus(text) {
  statusEl.textContent = text;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return '--';
  if (bytes === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = Math.abs(bytes);
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${bytes < 0 ? '-' : ''}${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}

function formatMs(ms) {
  return Number.isFinite(ms) ? `${Math.round(ms)} ms` : '--';
}

function jsHeapBytes() {
  return performance.memory ? performance.memory.usedJSHeapSize : 0;
}

function wasmHeapBytes() {
  try {
    return state.lightusdLoader?.native_?.HEAPU8?.buffer?.byteLength || 0;
  } catch (_) {
    return 0;
  }
}

function captureMemoryStats() {
  return {
    jsHeap: jsHeapBytes(),
    wasmHeap: wasmHeapBytes()
  };
}

function updateLoadStatsPanel(stats = loadStats.current) {
  if (!stats) return;
  const setText = (id, text) => {
    const el = document.getElementById(id);
    if (el) el.textContent = text;
  };
  const jsDelta = (stats.memoryAfter?.jsHeap || 0) - (stats.memoryBefore?.jsHeap || 0);
  const wasmDelta = (stats.memoryAfter?.wasmHeap || 0) - (stats.memoryBefore?.wasmHeap || 0);
  setText('currentFile', stats.filename || '-');
  setText('backendName', stats.backend || state.settings.backend);
  setText('fpsValue', loadStats.fps ? String(loadStats.fps) : '--');
  setText('fileSize', formatBytes(stats.fileSize));
  setText('readTime', formatMs(stats.readMs));
  setText('parseTime', formatMs(stats.parseMs));
  setText('processTime', formatMs(stats.processMs));
  setText('totalTime', formatMs(stats.totalMs));
  setText('jsHeapStat', `${formatBytes(stats.memoryAfter?.jsHeap || 0)} (${jsDelta >= 0 ? '+' : ''}${formatBytes(jsDelta)})`);
  setText('wasmHeapStat', `${formatBytes(stats.memoryAfter?.wasmHeap || 0)} (${wasmDelta >= 0 ? '+' : ''}${formatBytes(wasmDelta)})`);
}

function beginLoadStats(filename, fileSize = 0) {
  const memory = captureMemoryStats();
  const stats = {
    filename,
    backend: state.settings.backend,
    fileSize,
    startTime: performance.now(),
    readMs: NaN,
    parseMs: NaN,
    processMs: NaN,
    totalMs: NaN,
    memoryBefore: memory,
    memoryAfter: memory
  };
  loadStats.current = stats;
  updateLoadStatsPanel(stats);
  return stats;
}

function finishLoadStats(stats) {
  stats.totalMs = performance.now() - stats.startTime;
  stats.memoryAfter = captureMemoryStats();
  updateLoadStatsPanel(stats);
}

function urlParam(params) {
  return getAssetUriFromURL(params) || '';
}

function basenameFromUrl(url) {
  return basenameFromUri(url);
}

function isNextSceneObject(object) {
  return !!object?.userData && Number.isFinite(object.userData.usdMeshCount);
}

function countMeshes(root) {
  let count = 0;
  root?.traverse((obj) => {
    if (obj.isMesh) count++;
  });
  return count;
}

function updateButtonStates() {
  const hasSource = Boolean(state.robot);
  const hasUSD = Boolean(state.usdObject || state.latestUSDBytes);
  sourceExportButton.disabled = !hasSource;
  convertToUSDButton.disabled = !hasSource;
  convertToSourceButton.disabled = !hasUSD;
  fitViewButton.disabled = !(hasSource || hasUSD);
  for (const button of exportButtons) button.disabled = !(hasSource || hasUSD);
}

function disposeObject(root) {
  root.traverse((obj) => {
    if (obj.geometry) obj.geometry.dispose();
    if (obj.material && obj.material !== collisionMaterial) {
      const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
      for (const mat of mats) {
        // Label sprite materials/textures are shared and cached across frames;
        // disposeLabelSpriteCache() owns their lifetime, skip them here.
        if (mat.userData?.debugLabelMaterial) continue;
        if (mat.map?.userData?.debugLabelTexture) mat.map.dispose();
        mat.dispose?.();
      }
    }
  });
}

function clearObjectFromGroup(group, object) {
  if (!object) return;
  group.remove(object);
  disposeObject(object);
}

function clearGhosts() {
  clearObjectFromGroup(sourceView.ghostRoot, state.usdGhost);
  clearObjectFromGroup(usdView.ghostRoot, state.sourceGhost);
  state.usdGhost = null;
  state.sourceGhost = null;
}

function clearDebugRoot(root) {
  while (root.children.length) {
    const child = root.children.pop();
    disposeObject(child);
  }
}

function clearDebugVisualizations() {
  clearDebugRoot(sourceView.debugRoot);
  clearDebugRoot(usdView.debugRoot);
}

function clearRobot() {
  if (state.robot) {
    clearObjectFromGroup(robotGroup, state.robot);
  }
  state.robot = null;
  state.inputText = '';
  state.inputName = '';
  state.inputFormat = '';
  state.exportPayload = null;
  state.sourceRestLinkMatrices.clear();
  state.usdLinkBindings = [];
  state.nativeMeshBuffers.clear();
  state.generatedTextureAssets.clear();
  state.joints = {};
  state.jointValues = {};
  state.collisionMeshes = [];
  state.visualMeshes = [];
  for (const obj of state.muscleObjects) clearObjectFromGroup(robotGroup, obj);
  state.muscleObjects = [];
  state.muscleLineData = null;
  for (const obj of state.sensorObjects) clearObjectFromGroup(robotGroup, obj);
  state.sensorObjects = [];
  state.sensorVisualData = null;
  state.sensorPreviewCameras = [];
  state.sensorPreviewIndex = 0;
  updateSensorPreviewPanel();
  jointControlsEl.innerHTML = '';
  updateButtonStates();
  clearGhosts();
  clearDebugVisualizations();
  disposeLabelSpriteCache();
  updateLabels();
}

function clearUSD() {
  clearObjectFromGroup(usdGroup, state.usdObject);
  if (state.usdRestObject && state.usdRestObject !== state.usdObject) {
    disposeObject(state.usdRestObject);
  }
  for (const obj of state.usdMuscleObjects) clearObjectFromGroup(usdGroup, obj);
  state.usdMuscleObjects = [];
  for (const obj of state.usdSensorObjects) clearObjectFromGroup(usdGroup, obj);
  state.usdSensorObjects = [];
  state.usdObject = null;
  state.usdRestObject = null;
  state.usdArticulation = null;
  state.usdIsConverted = false;
  state.usdName = '';
  state.latestUSDBytes = null;
  state.latestUSDFormat = '';
  state.usdLinkBindings = [];
  updateButtonStates();
  clearGhosts();
  clearDebugVisualizations();
  disposeLabelSpriteCache();
  updateLabels();
}

function rebuildAssetFiles(files) {
  for (const url of state.objectUrls.values()) URL.revokeObjectURL(url);
  state.assetFiles.clear();
  state.objectUrls.clear();
  state.meshCache.clear();

  for (const file of files) {
    rememberAssetFile(file);
  }
  setStatus(`${files.length} asset files indexed.`);
}

function normalizePath(path) {
  return path.replace(/\\/g, '/').replace(/^\.?\//, '');
}

function dirname(path) {
  const normalized = normalizePath(path || '');
  const slash = normalized.lastIndexOf('/');
  return slash >= 0 ? normalized.slice(0, slash) : '';
}

function joinPath(base, path) {
  const cleanPath = normalizePath(path || '');
  if (!base) return cleanPath;
  return normalizePath(`${base}/${cleanPath}`);
}

function rememberAssetFile(file) {
  const rel = file.webkitRelativePath || file.name;
  state.assetFiles.set(rel, file);
  state.assetFiles.set(file.name, file);
}

function resolveAssetEntry(assetPath) {
  const raw = normalizePath(assetPath || '');
  const withoutPackage = raw.replace(/^package:\/\//, '');
  const withoutRoot = state.settings.packageRoot
    ? withoutPackage.replace(new RegExp(`^${state.settings.packageRoot.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}/`), '')
    : withoutPackage;
  const candidates = [
    raw,
    withoutPackage,
    withoutRoot,
    withoutPackage.split('/').slice(1).join('/'),
    withoutPackage.split('/').pop()
  ].filter(Boolean);

  for (const candidate of candidates) {
    if (state.assetFiles.has(candidate)) {
      return { rel: candidate, file: state.assetFiles.get(candidate) };
    }
  }
  for (const [rel, file] of state.assetFiles) {
    if (rel.endsWith(withoutPackage) || rel.endsWith(withoutRoot)) return { rel, file };
  }
  return null;
}

function resolveAssetFile(assetPath) {
  return resolveAssetEntry(assetPath)?.file || null;
}

function objectURLForFile(file) {
  if (!state.objectUrls.has(file)) {
    state.objectUrls.set(file, URL.createObjectURL(file));
  }
  return state.objectUrls.get(file);
}

function extension(path) {
  const clean = (path || '').split('?')[0].split('#')[0];
  const dot = clean.lastIndexOf('.');
  return dot >= 0 ? clean.slice(dot).toLowerCase() : '';
}

async function ensureTinyLoader() {
  if (!state.lightusdLoader) {
    setStatus('Loading LightUSD WASM...');
    state.lightusdLoader = await createConfiguredLightUSDLoader({
      initOptions: {
        useZstdCompressedWasm: false,
        useMemory64: false,
        backend: state.settings.backend
      }
    });
    LightUSDLoaderUtils.setLightUSD(state.lightusdLoader.native_);
  }
  return state.lightusdLoader;
}

async function loadUSDMeshFromFile(file) {
  const loader = await ensureTinyLoader();
  const sceneData = await parseUSDSceneFromArrayBuffer(loader, await file.arrayBuffer(), file.name);
  const rootNode = sceneData.getDefaultRootNode();
  const defaultMaterial = LightUSDLoaderUtils.createDefaultMaterial();
  return LightUSDLoaderUtils.buildThreeNode(rootNode, defaultMaterial, sceneData, { overrideMaterial: false });
}

async function loadUSDObjectFromBytes(bytes, filename) {
  const loader = await ensureTinyLoader();
  const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const parseStart = performance.now();
  const sceneData = await parseUSDSceneFromArrayBuffer(
    loader,
    data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
    filename,
    // Decode in-package textures (e.g. an embedded-texture USDZ) in the native
    // loader so the converted-USD view shows materials instead of a flat color.
    makeStaticNextParseOptions({
      backend: state.settings.backend,
      loadTextureInNative: true
    })
  );
  const stats = loadStats.current;
  if (stats) {
    stats.parseMs = performance.now() - parseStart;
    updateLoadStatsPanel(stats);
  }
  if (isNextScene(sceneData)) {
    const built = buildNextThreeNode(sceneData, {
      skipTextures: false,
      lazyTextures: false
    });
    const object = built.node;
    object.name = filename.replace(/\.[^.]+$/, '') || 'usd_scene';
    const counts = nextCountsFromScene(sceneData);
    object.userData.usdMeshCount = counts.meshes || 0;
    object.userData.usdMaterialCount = counts.materials || 0;
    object.userData.usdTextureCount = counts.textures || 0;
    return object;
  }
  const rootNode = sceneData.getDefaultRootNode();
  const defaultMaterial = LightUSDLoaderUtils.createDefaultMaterial();
  const object = await LightUSDLoaderUtils.buildThreeNode(rootNode, defaultMaterial, sceneData, { overrideMaterial: false });
  object.name = filename.replace(/\.[^.]+$/, '') || 'usd_scene';
  return object;
}

async function extractUSDPhysicsJSONFromBytes(bytes, filename) {
  const native = await ensureNativeExporter();
  const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (!native.loadFromBinary(data, filename || 'scene.usd')) {
    throw new Error(native.error() || 'Failed to load USD for physics extraction.');
  }
  const jsonText = native.extractPhysicsSceneJSON();
  if (!jsonText) throw new Error(native.error() || 'USD Physics extraction failed.');
  return JSON.parse(jsonText);
}

async function loadUSDFile(file, existingStats = null) {
  clearUSD();
  const stats = existingStats || beginLoadStats(file.name, file.size || 0);
  let bytes;
  if (existingStats?.bytes) {
    bytes = existingStats.bytes;
  } else {
    const readStart = performance.now();
    bytes = new Uint8Array(await file.arrayBuffer());
    stats.readMs = performance.now() - readStart;
  }
  stats.fileSize = bytes.byteLength;
  updateLoadStatsPanel(stats);
  const processStart = performance.now();
  const object = await loadUSDObjectFromBytes(bytes, file.name);
  stats.processMs = Math.max(0, performance.now() - processStart - (stats.parseMs || 0));
  state.usdObject = object;
  state.usdRestObject = object;
  state.usdName = file.name;
  state.latestUSDBytes = bytes;
  state.latestUSDFormat = extension(file.name).slice(1) || 'usd';
  usdGroup.add(object);
  applySceneOrientation();
  let updatedInfoFromPhysics = false;
  try {
    const extracted = await extractUSDPhysicsJSONFromBytes(bytes, file.name);
    annotateUSDRenderableClasses(object, extracted);
    const model = usdPhysicsToSourceModel(extracted);
    if (model.links.length && model.joints.length) {
      const articulated = buildArticulatedRobotFromUSD(model, object, {
        name: object.name,
        resetMeshLists: false,
        inferMissingJointFrames: true
      });
      usdGroup.remove(object);
      state.usdObject = articulated;
      state.usdArticulation = articulated;
      usdGroup.add(articulated);
      rebuildJointControls();
      updateRobotInfo({
        name: model.name,
        links: new Map(model.links.map((link) => [link.name, link])),
        joints: model.joints,
        meshes: (extracted.prims || []).filter((prim) => prim.geometry).length
      });
      updatedInfoFromPhysics = true;
    }
  } catch (err) {
    console.warn('USD Physics extraction skipped:', err);
  }
  if (!updatedInfoFromPhysics) {
    updateRobotInfo({
      name: object.name || file.name,
      links: new Map(),
      joints: [],
      meshes: object.userData.usdMeshCount ?? countMeshes(object)
    });
  }
  updateButtonStates();
  rebuildGhosts();
  updateLabels();
  applyVisibility();
  if (state.settings.autocenter) fitCamera(currentFitObjects());
  finishLoadStats(stats);
  setStatus(`Loaded USD ${file.name} (${isNextSceneObject(object) ? 'next' : 'legacy'} backend)`);
}

async function loadOBJMeshFromFile(file) {
  const url = objectURLForFile(file);
  const loader = new OBJLoader();
  return loader.loadAsync(url);
}

async function loadSTLMeshFromFile(file) {
  // Weld the STL soup and shade it like MuJoCo (per-vertex normals with
  // sharp edges kept); matches the native and CLI converters.
  const geometry = weldMeshGeometry(new STLLoader().parse(await file.arrayBuffer()),
    { mode: 'stl' });
  const mesh = new THREE.Mesh(
    geometry,
    new THREE.MeshStandardMaterial({ color: 0xb8c0c8, roughness: 0.62, metalness: 0.05 })
  );
  mesh.name = file.name.replace(/\.[^.]+$/, '');
  const group = new THREE.Group();
  group.name = mesh.name;
  group.add(mesh);
  return group;
}

// Apply a MuJoCo geom/material rgba to a visual mesh: tint the (cloned)
// material and, when alpha < 1, render it transparent — so e.g. ms_human_700's
// semi-transparent leg bones show the muscles behind them, matching the MuJoCo
// viewer. Materials are already cloned per-geom (cloneRenderableObject), but we
// clone again defensively before mutating.
function applyRgbaToMesh(mesh, info) {
  if (!mesh?.isMesh || !mesh.material || !info?.rgba) return;
  const rgba = info.rgba;
  const emission = info.emission || 0;
  const apply = (src) => {
    const mat = src.clone ? src.clone() : src;
    if (mat.color && rgba.length >= 3) mat.color.setRGB(rgba[0], rgba[1], rgba[2]);
    // MuJoCo material `emission` makes the geom self-lit (e.g. the bright
    // "green pulleys"); map it to an emissive tint so it reads at full color.
    const emissive = emission > 0 && mat.emissive;
    if (emissive) {
      mat.emissive.setRGB(rgba[0] * emission, rgba[1] * emission, rgba[2] * emission);
    }
    // Honor alpha<1 as transparency — EXCEPT for self-lit (emissive) geoms,
    // which MuJoCo draws as a solid bright color; many overlapping low-alpha
    // cylinders (e.g. the pulley grid) otherwise blend into muddy artifacts.
    const alpha = rgba.length >= 4 ? rgba[3] : 1;
    if (alpha < 1 && !emissive) {
      mat.transparent = true;
      mat.opacity = alpha;
      mat.depthWrite = false;
    }
    return mat;
  };
  mesh.material = Array.isArray(mesh.material) ? mesh.material.map(apply) : apply(mesh.material);
}

const _rgbCss = (c) => `rgb(${Math.round((c[0] || 0) * 255)},${Math.round((c[1] || 0) * 255)},${Math.round((c[2] || 0) * 255)})`;

function makeCanvas2D(size) {
  const canvas = (typeof OffscreenCanvas !== 'undefined')
    ? new OffscreenCanvas(size, size)
    : Object.assign(document.createElement('canvas'), { width: size, height: size });
  return { canvas, ctx: canvas.getContext('2d') };
}

function drawCheckerTexture(ctx, def, size = 256, repeat = [1, 1]) {
  ctx.fillStyle = _rgbCss(def.rgb1);
  ctx.fillRect(0, 0, size, size);
  ctx.fillStyle = _rgbCss(def.rgb2);
  const repeatX = Math.max(1, Math.round(Number(repeat[0]) || 1));
  const repeatY = Math.max(1, Math.round(Number(repeat[1]) || 1));
  const cellsX = repeatX * 2;
  const cellsY = repeatY * 2;
  const cellW = size / cellsX;
  const cellH = size / cellsY;
  for (let y = 0; y < cellsY; y++) {
    for (let x = 0; x < cellsX; x++) {
      if (((x + y) & 1) === 0) ctx.fillRect(x * cellW, y * cellH, cellW + 0.5, cellH + 0.5);
    }
  }
}

function drawGradientTexture(ctx, def, size = 256) {
  const g = ctx.createLinearGradient(0, 0, 0, size);
  g.addColorStop(0, _rgbCss(def.rgb1));
  g.addColorStop(1, _rgbCss(def.rgb2));
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, size, size);
}

// MuJoCo builtin "checker": a 2x2 checkerboard of rgb1/rgb2 (material texrepeat
// tiles it). Returns a THREE.CanvasTexture.
function makeCheckerTexture(def) {
  const { canvas, ctx } = makeCanvas2D(256);
  drawCheckerTexture(ctx, def);
  return new THREE.CanvasTexture(canvas);
}

// MuJoCo builtin "gradient": vertical rgb1 (top) -> rgb2 (bottom).
function makeGradientTexture(def) {
  const { canvas, ctx } = makeCanvas2D(256);
  drawGradientTexture(ctx, def);
  return new THREE.CanvasTexture(canvas);
}

async function canvasPNGBytes(canvas) {
  const blob = canvas.convertToBlob
    ? await canvas.convertToBlob({ type: 'image/png' })
    : await new Promise((resolve, reject) => {
      canvas.toBlob((b) => (b ? resolve(b) : reject(new Error('canvas PNG encode failed'))), 'image/png');
    });
  return new Uint8Array(await blob.arrayBuffer());
}

async function generatedMujocoTextureAsset(textureName, def, texrepeat = [1, 1]) {
  if (!def || (def.builtin !== 'checker' && def.builtin !== 'gradient')) return '';
  const safeName = sanitizeUSDIdentifier(textureName || def.builtin, 'texture');
  const repeatX = Math.max(1, Math.round(Number(texrepeat[0]) || 1));
  const repeatY = Math.max(1, Math.round(Number(texrepeat[1]) || 1));
  const path = `textures/mjcf_builtin_${safeName}_${repeatX}x${repeatY}.png`;
  if (state.generatedTextureAssets.has(path)) return path;
  const size = 256;
  const canvas = Object.assign(document.createElement('canvas'), { width: size, height: size });
  const ctx = canvas.getContext('2d');
  if (def.builtin === 'checker') drawCheckerTexture(ctx, def, size, texrepeat);
  else drawGradientTexture(ctx, def, size);
  state.generatedTextureAssets.set(path, await canvasPNGBytes(canvas));
  return path;
}

function makeMissingMesh(path) {
  const group = new THREE.Group();
  group.name = `missing_${path || 'mesh'}`;
  const mesh = new THREE.Mesh(
    new THREE.BoxGeometry(0.08, 0.08, 0.08),
    new THREE.MeshStandardMaterial({ color: 0xd84a4a, roughness: 0.7 })
  );
  group.add(mesh);
  return group;
}

async function loadURDFMesh(path) {
  const file = resolveAssetFile(path);
  if (!file) {
    setStatus(`Missing mesh asset: ${path}`);
    return makeMissingMesh(path);
  }

  const ext = extension(file.name || path);
  if (ext === '.obj') return loadOBJMeshFromFile(file);
  if (ext === '.stl') return loadSTLMeshFromFile(file);
  if (USD_MESH_EXTENSIONS.has(ext)) return loadUSDMeshFromFile(file);

  setStatus(`Unsupported mesh extension ${ext || '(none)'} for ${path}`);
  return makeMissingMesh(path);
}

async function loadMeshObject(path) {
  const entry = resolveAssetEntry(path);
  if (!entry) {
    setStatus(`Missing mesh asset: ${path}`);
    return makeMissingMesh(path);
  }

  if (!state.meshCache.has(entry.rel)) {
    const ext = extension(entry.file.name || path);
    let promise = null;
    if (ext === '.obj') {
      promise = loadOBJMeshFromFile(entry.file);
    } else if (ext === '.stl') {
      promise = loadSTLMeshFromFile(entry.file);
    } else if (USD_MESH_EXTENSIONS.has(ext)) {
      promise = loadUSDMeshFromFile(entry.file);
    } else {
      setStatus(`Unsupported mesh extension ${ext || '(none)'} for ${path}`);
      promise = Promise.resolve(makeMissingMesh(path));
    }
    state.meshCache.set(entry.rel, promise);
  }

  return cloneRenderableObject(await state.meshCache.get(entry.rel));
}

function cloneRenderableObject(source) {
  const savedUserData = [];
  source.traverse((obj) => {
    savedUserData.push([obj, obj.userData]);
    obj.userData = {};
  });
  let clone = null;
  try {
    clone = source.clone(true);
  } finally {
    for (const [obj, userData] of savedUserData) obj.userData = userData;
  }
  clone.traverse((obj) => {
    if (!obj.isMesh) return;
    if (obj.geometry) obj.geometry = obj.geometry.clone();
    if (Array.isArray(obj.material)) {
      obj.material = obj.material.map((mat) => mat.clone?.() || mat);
    } else if (obj.material) {
      obj.material = obj.material.clone?.() || obj.material;
    }
  });
  return clone;
}

function makeGhostObject(source, color) {
  const ghost = cloneRenderableObject(source);
  const ghostMaterial = new THREE.MeshStandardMaterial({
    color,
    roughness: 0.8,
    metalness: 0.0,
    transparent: true,
    opacity: state.settings.ghostOpacity,
    depthWrite: false
  });
  ghost.traverse((obj) => {
    if (!obj.isMesh) return;
    obj.material = ghostMaterial.clone();
  });
  return ghost;
}

function setGhostOpacity(root, opacity) {
  root?.traverse((obj) => {
    if (obj.isMesh && obj.material) {
      const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
      for (const mat of mats) {
        mat.opacity = opacity;
        mat.transparent = true;
        mat.depthWrite = false;
      }
    }
  });
}

function syncObjectTransforms(source, target) {
  if (!source || !target) return;
  const sourceObjects = [];
  const targetObjects = [];
  source.traverse((obj) => sourceObjects.push(obj));
  target.traverse((obj) => targetObjects.push(obj));
  const count = Math.min(sourceObjects.length, targetObjects.length);
  for (let i = 0; i < count; i++) {
    targetObjects[i].position.copy(sourceObjects[i].position);
    targetObjects[i].quaternion.copy(sourceObjects[i].quaternion);
    targetObjects[i].scale.copy(sourceObjects[i].scale);
  }
}

function rebuildGhosts() {
  clearGhosts();
  if (state.settings.ghostUSDInSource && state.usdObject) {
    state.usdGhost = makeGhostObject(state.usdObject, 0x70c8ff);
    sourceView.ghostRoot.add(state.usdGhost);
  }
  if (state.settings.ghostSourceInUSD && state.robot) {
    state.sourceGhost = makeGhostObject(state.robot, 0xffb35c);
    usdView.ghostRoot.add(state.sourceGhost);
  }
  applySceneOrientation();
}

function syncGhosts() {
  syncObjectTransforms(state.usdObject, state.usdGhost);
  syncObjectTransforms(state.robot, state.sourceGhost);
}

function isCollisionObject(object) {
  // Prefer the explicit classification flag when present (set by the MJCF/URDF
  // parser and the converted-USD preview). The name/path heuristic is only a
  // fallback for imported USD without the flag — otherwise geoms legitimately
  // named "*_collision" but tagged group=0 (visual), e.g. iit_softfoot, would be
  // wrongly hidden.
  if (typeof object.userData?.urdfCollision === 'boolean') {
    return object.userData.urdfCollision;
  }
  const marker = [
    object.name,
    object.userData?.nodeCategory,
    object.userData?.['primMeta.absPath']
  ].filter(Boolean).join(' ').toLowerCase();
  return marker.includes('collision') || marker.includes('collider');
}

function pathInSetOrUnder(path, paths) {
  if (!path || !paths) return false;
  if (paths.has(path)) return true;
  for (const candidate of paths) {
    if (path.startsWith(`${candidate}/`)) return true;
  }
  return false;
}

function usdGeometryClassification(extracted) {
  const visualPaths = new Set();
  const collisionPaths = new Set();
  for (const prim of extracted?.prims || []) {
    if (!prim.geometry) continue;
    const group = Number(prim.properties?.['mjc:group']);
    // Three independent collider signals, in priority order:
    //   1. `UsdPhysicsCollisionAPI` applied (canonical UsdPhysics)
    //   2. `mjc:group >= 3` (menagerie convention: groups 3-5 are collision)
    //   3. `purpose == "guide"` (surfaced by the patched
    //      `AppendPhysicsPrimJson` in `web/binding.cc`; used by the
    //      mujoco-usd-converter to hide colliders from default renders)
    // Any of these flips the prim into the collision bucket.
    if (hasApi(prim, 'PhysicsCollisionAPI')
        || (Number.isFinite(group) && group >= 3)
        || prim.purpose === 'guide') {
      collisionPaths.add(prim.path);
    } else {
      visualPaths.add(prim.path);
    }
  }
  return { visualPaths, collisionPaths };
}

function annotateUSDRenderableClasses(root, extracted) {
  const { visualPaths, collisionPaths } = usdGeometryClassification(extracted);
  root?.traverse((obj) => {
    if (!obj.isMesh) return;
    const path = usdPathForObject(obj);
    if (pathInSetOrUnder(path, collisionPaths)) {
      obj.userData.urdfCollision = true;
    } else if (pathInSetOrUnder(path, visualPaths)) {
      obj.userData.urdfCollision = false;
    }
  });
}

function sanitizeUSDIdentifier(name, fallback = 'link') {
  let out = String(name || '').replace(/[^A-Za-z0-9_]/g, '_');
  if (!out) out = fallback;
  if (!/^[A-Za-z_]/.test(out)) out = `_${out}`;
  return out;
}

function findObjectByName(root, name) {
  let found = null;
  root?.traverse((obj) => {
    if (!found && obj.name === name) found = obj;
  });
  return found;
}

function sourceLinkObject(linkName) {
  return state.robot?.links?.[linkName] || null;
}

function captureSourceRestLinkMatrices() {
  state.sourceRestLinkMatrices.clear();
  if (!state.robot?.links) return;
  state.robot.updateWorldMatrix(true, true);
  for (const [name, link] of Object.entries(state.robot.links)) {
    link.updateWorldMatrix(true, false);
    state.sourceRestLinkMatrices.set(name, link.matrixWorld.clone());
  }
}

function bindConvertedUSDLinksToSource() {
  state.usdLinkBindings = [];
  if (!state.robot?.links || !state.usdObject || !state.exportPayload?.links) return;

  state.robot.updateWorldMatrix(true, true);
  state.usdObject.updateWorldMatrix(true, true);
  for (const link of state.exportPayload.links) {
    const linkName = link.name;
    const sourceLink = sourceLinkObject(linkName);
    const usdLink = findObjectByName(state.usdObject, sanitizeUSDIdentifier(linkName, 'link'));
    if (!sourceLink || !usdLink) continue;
    const sourceRestWorld = state.sourceRestLinkMatrices.get(linkName) || sourceLink.matrixWorld.clone();
    state.usdLinkBindings.push({
      linkName,
      sourceLink,
      usdLink,
      sourceRestWorld: sourceRestWorld.clone(),
      inverseSourceRestWorld: sourceRestWorld.clone().invert(),
      usdRestWorld: usdLink.matrixWorld.clone()
    });
  }
}

function syncConvertedUSDToSourcePose() {
  if (!state.usdLinkBindings.length) return;
  state.robot?.updateWorldMatrix(true, true);
  state.usdObject?.updateWorldMatrix(true, true);
  for (const binding of state.usdLinkBindings) {
    binding.sourceLink.updateWorldMatrix(true, false);
    const delta = new THREE.Matrix4()
      .copy(binding.sourceLink.matrixWorld)
      .multiply(binding.inverseSourceRestWorld);
    const desiredWorld = new THREE.Matrix4().copy(delta).multiply(binding.usdRestWorld);
    const parentInverse = new THREE.Matrix4();
    if (binding.usdLink.parent) {
      binding.usdLink.parent.updateWorldMatrix(true, false);
      parentInverse.copy(binding.usdLink.parent.matrixWorld).invert();
    } else {
      parentInverse.identity();
    }
    const local = parentInverse.multiply(desiredWorld);
    local.decompose(binding.usdLink.position, binding.usdLink.quaternion, binding.usdLink.scale);
    binding.usdLink.updateWorldMatrix(false, true);
  }
}

function worldPositionOf(object) {
  object.updateWorldMatrix(true, false);
  return new THREE.Vector3().setFromMatrixPosition(object.matrixWorld);
}

function objectWorldCenter(object) {
  const box = new THREE.Box3().setFromObject(object);
  if (!box.isEmpty()) return box.getCenter(new THREE.Vector3());
  return worldPositionOf(object);
}

function makeLinkDebugEntry(object) {
  return {
    origin: worldPositionOf(object),
    center: objectWorldCenter(object),
    object
  };
}

function sourceLinkDebugEntries() {
  const entries = new Map();
  if (!state.robot?.links) return entries;
  state.robot.updateWorldMatrix(true, true);
  for (const [name, link] of Object.entries(state.robot.links)) {
    entries.set(name, makeLinkDebugEntry(link));
  }
  return entries;
}

function usdLinkDebugEntries() {
  if (state.usdLinkBindings.length) {
    const entries = new Map();
    state.usdObject?.updateWorldMatrix(true, true);
    for (const binding of state.usdLinkBindings) {
      entries.set(binding.linkName, makeLinkDebugEntry(binding.usdLink));
    }
    return entries;
  }

  const entries = new Map();
  const linksScope = findObjectByName(state.usdObject, 'Links');
  if (linksScope) {
    linksScope.updateWorldMatrix(true, true);
    for (const child of linksScope.children) {
      entries.set(child.name, makeLinkDebugEntry(child));
    }
  } else if (state.usdArticulation?.links) {
    state.usdArticulation.updateWorldMatrix(true, true);
    for (const [name, link] of Object.entries(state.usdArticulation.links)) {
      entries.set(name, makeLinkDebugEntry(link));
    }
  }
  return entries;
}

function jointPosition(joint, linkEntries) {
  if (joint?.pivot) return worldPositionOf(joint.pivot);
  if (joint?.isObject3D) return worldPositionOf(joint);
  if (joint?.child && linkEntries.has(joint.child)) return linkEntries.get(joint.child).origin.clone();
  return null;
}

function axisVector(axis) {
  if (axis?.isVector3) return axis.clone();
  if (Array.isArray(axis)) return new THREE.Vector3(axis[0] || 0, axis[1] || 0, axis[2] || 0);
  if (axis && typeof axis === 'object') return new THREE.Vector3(axis.x || 0, axis.y || 0, axis.z || 0);
  return new THREE.Vector3(1, 0, 0);
}

function jointWorldDirection(joint, linkEntries) {
  const direction = axisVector(joint?.axis);
  if (direction.lengthSq() < 1.0e-12) direction.set(1, 0, 0);
  direction.normalize();

  const frame = joint?.pivot || (joint?.isObject3D ? joint : linkEntries.get(joint?.child)?.object);
  if (frame?.isObject3D) {
    const worldQuat = new THREE.Quaternion();
    frame.getWorldQuaternion(worldQuat);
    direction.applyQuaternion(worldQuat).normalize();
  }
  return direction;
}

function debugVisualizationEnabled() {
  return state.settings.showLinkLines
    || state.settings.showJointArrows
    || state.settings.showLinkNames
    || state.settings.showJointNames;
}

// Rasterizing label text and uploading a CanvasTexture is expensive, and the
// skeleton debug overlay is rebuilt every frame (positions follow the joints).
// Label text/color never changes between frames, so cache the texture+material
// per (text, color): frames reuse the GPU texture and only the cheap Sprite
// wrapper + its position are recreated. Cleared on robot/USD swap (see
// disposeLabelSpriteCache) since link/joint names may then differ.
const labelSpriteCache = new Map();
const LABEL_CACHE_LIMIT = 512;
const LABEL_MAX_CHARS = 160;
const LABEL_MAX_CANVAS_WIDTH = 2048;
const LABEL_HEIGHT_RATIO = 0.035;
const LABEL_MIN_HEIGHT = 0.024;

function labelTextForSprite(text) {
  const raw = String(text ?? '');
  return raw.length > LABEL_MAX_CHARS
    ? `${raw.slice(0, LABEL_MAX_CHARS - 3)}...`
    : raw;
}

function disposeLabelSpriteAssets(assets) {
  assets.material.map?.dispose();
  assets.material.dispose();
}

function labelSpriteAssets(text, color) {
  const displayText = labelTextForSprite(text);
  const key = JSON.stringify([color, displayText]);
  const cached = labelSpriteCache.get(key);
  if (cached) {
    labelSpriteCache.delete(key);
    labelSpriteCache.set(key, cached);
    return cached;
  }

  const canvas = document.createElement('canvas');
  const ctx = canvas.getContext('2d');
  const fontSize = 36;
  const paddingX = 14;
  const paddingY = 8;
  ctx.font = `600 ${fontSize}px system-ui, sans-serif`;
  const metrics = ctx.measureText(displayText);
  canvas.width = Math.min(
    LABEL_MAX_CANVAS_WIDTH,
    Math.max(1, Math.ceil(metrics.width + paddingX * 2))
  );
  canvas.height = fontSize + paddingY * 2;

  ctx.font = `600 ${fontSize}px system-ui, sans-serif`;
  ctx.textBaseline = 'middle';
  ctx.fillStyle = 'rgba(12, 16, 22, 0.72)';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.strokeStyle = 'rgba(255, 255, 255, 0.22)';
  ctx.strokeRect(0.5, 0.5, canvas.width - 1, canvas.height - 1);
  ctx.fillStyle = color;
  ctx.fillText(displayText, paddingX, canvas.height * 0.5, canvas.width - paddingX * 2);

  const texture = new THREE.CanvasTexture(canvas);
  texture.userData.debugLabelTexture = true;
  const material = new THREE.SpriteMaterial({
    map: texture,
    transparent: true,
    depthTest: false,
    depthWrite: false
  });
  const cacheable = labelSpriteCache.size < LABEL_CACHE_LIMIT;
  if (cacheable) {
    // Shared+cached: disposeObject must not free this on the per-frame rebuild.
    material.userData.debugLabelMaterial = true;
  }
  const assets = { material, aspect: canvas.width / canvas.height };
  if (cacheable) labelSpriteCache.set(key, assets);
  return assets;
}

function objectLabelHeight(object) {
  if (!object) return LABEL_MIN_HEIGHT * state.settings.labelScale;
  const box = new THREE.Box3().setFromObject(object);
  if (box.isEmpty()) return LABEL_MIN_HEIGHT * state.settings.labelScale;
  const size = box.getSize(new THREE.Vector3());
  const diagonal = size.length();
  const base = Math.max(LABEL_MIN_HEIGHT, diagonal * LABEL_HEIGHT_RATIO);
  return base * state.settings.labelScale;
}

function makeTextSprite(text, color, height) {
  const { material, aspect } = labelSpriteAssets(text, color);
  const sprite = new THREE.Sprite(material);
  sprite.scale.set(height * aspect, height, 1);
  sprite.renderOrder = 20;
  return sprite;
}

function disposeLabelSpriteCache() {
  for (const assets of labelSpriteCache.values()) disposeLabelSpriteAssets(assets);
  labelSpriteCache.clear();
}

function addLabel(root, text, position, color, labelHeight, offsetFactor = 1.8) {
  if (!text || !position) return;
  const height = Number.isFinite(labelHeight) && labelHeight > 0
    ? labelHeight
    : LABEL_MIN_HEIGHT * state.settings.labelScale;
  const label = makeTextSprite(text, color, height);
  label.position.copy(position);
  label.position.y += height * offsetFactor;
  root.add(label);
}

function jointsByParentLink(joints) {
  const byParent = new Map();
  for (const joint of joints) {
    if (!joint.parent) continue;
    if (!byParent.has(joint.parent)) byParent.set(joint.parent, []);
    byParent.get(joint.parent).push(joint);
  }
  return byParent;
}

function makeLinkSegments(linkEntries, joints) {
  const segments = [];
  const byParent = jointsByParentLink(joints);
  for (const joint of joints) {
    if (!joint.child) continue;
    const start = jointPosition(joint, linkEntries);
    if (!start) continue;
    const nextJoints = byParent.get(joint.child) || [];
    if (nextJoints.length) {
      for (const nextJoint of nextJoints) {
        const end = jointPosition(nextJoint, linkEntries);
        if (end && start.distanceToSquared(end) > 1.0e-10) {
          segments.push({ linkName: joint.child, start, end });
        }
      }
      continue;
    }

    const entry = linkEntries.get(joint.child);
    const end = entry?.center || entry?.origin;
    if (end && start.distanceToSquared(end) > 1.0e-10) {
      segments.push({ linkName: joint.child, start, end });
    }
  }
  return segments;
}

function makeJointArrow(position, direction, material) {
  const arrow = new THREE.Group();
  arrow.position.copy(position);
  arrow.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), direction.clone().normalize());
  arrow.renderOrder = 10;

  const shaft = new THREE.Mesh(jointArrowShaftGeometry.clone(), material.clone());
  shaft.position.y = 0.039;
  shaft.renderOrder = 10;
  arrow.add(shaft);

  const head = new THREE.Mesh(jointArrowHeadGeometry.clone(), material.clone());
  head.position.y = 0.098;
  head.renderOrder = 10;
  arrow.add(head);
  return arrow;
}

function buildSkeletonDebug(root, targetObject, linkEntries, joints, lineMaterial, jointMaterial, labelColor) {
  clearDebugRoot(root);
  if (!debugVisualizationEnabled()) return;

  const segments = makeLinkSegments(linkEntries, joints);
  const labelHeight = objectLabelHeight(targetObject);

  if (state.settings.showLinkLines) {
    const points = [];
    for (const segment of segments) {
      points.push(segment.start.x, segment.start.y, segment.start.z, segment.end.x, segment.end.y, segment.end.z);
    }
    if (points.length) {
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute('position', new THREE.Float32BufferAttribute(points, 3));
      root.add(new THREE.LineSegments(geometry, lineMaterial.clone()));
    }
  }

  if (state.settings.showLinkNames) {
    for (const segment of segments) {
      const midpoint = new THREE.Vector3().addVectors(segment.start, segment.end).multiplyScalar(0.5);
      addLabel(root, segment.linkName, midpoint, labelColor, labelHeight, 1.6);
    }
  }

  if (state.settings.showJointArrows) {
    for (const joint of joints) {
      const type = joint.jointType || joint.type || 'fixed';
      if (type === 'fixed') continue;
      const pos = jointPosition(joint, linkEntries);
      if (!pos) continue;
      root.add(makeJointArrow(pos, jointWorldDirection(joint, linkEntries), jointMaterial));
    }
  }

  if (state.settings.showJointNames) {
    for (const joint of joints) {
      const pos = jointPosition(joint, linkEntries);
      addLabel(root, joint.name || joint.jointName || 'joint', pos, labelColor, labelHeight, 2.4);
    }
  }
}

function updateSkeletonDebugVisualizations() {
  if (!debugVisualizationEnabled()) {
    clearDebugVisualizations();
    return;
  }
  const joints = Object.values(state.joints || {});
  buildSkeletonDebug(sourceView.debugRoot, state.robot, sourceLinkDebugEntries(), joints, sourceLinkLineMaterial, sourceJointMaterial, sourceLabelColor);
  buildSkeletonDebug(usdView.debugRoot, state.usdObject || state.usdArticulation, usdLinkDebugEntries(), joints, usdLinkLineMaterial, usdJointMaterial, usdLabelColor);
}

async function parseURDFWithMeshes(urdfText, filename) {
  const doc = parseXMLDocument(urdfText);
  if (doc.documentElement?.localName !== 'robot') {
    throw new Error(`Expected URDF <robot> root, got <${doc.documentElement?.localName || 'unknown'}>.`);
  }

  const manager = new THREE.LoadingManager();
  manager.setURLModifier((url) => {
    const file = resolveAssetFile(url);
    return file ? objectURLForFile(file) : url;
  });

  const loader = new URDFLoader(manager);
  loader.parseCollision = true;
  loader.packages = (pkg) => pkg;
  loader.loadMeshCb = (path, _manager, done) => {
    loadURDFMesh(path)
      .then((obj) => done(obj))
      .catch((err) => {
        console.error(err);
        done(makeMissingMesh(path));
      });
  };

  const robot = loader.parse(urdfText);
  robot.name = robot.name || filename.replace(/\.[^.]+$/, '');
  return robot;
}

function parseXMLDocument(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  const parseError = doc.querySelector('parsererror');
  if (parseError) {
    throw new Error(parseError.textContent?.trim() || 'XML parse failed.');
  }
  return doc;
}

function detectInputFormat(text) {
  const rootName = parseXMLDocument(text).documentElement?.localName || '';
  if (rootName === 'robot') return 'urdf';
  if (rootName === 'mujoco') return 'mjcf';
  throw new Error(`Unsupported XML root <${rootName || 'unknown'}>. Expected <robot> or <mujoco>.`);
}

function decodeXML(text) {
  return text
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&');
}

function parseAttributes(text = '') {
  const attrs = {};
  const re = /([A-Za-z_:][-A-Za-z0-9_:.]*)\s*=\s*(?:"([^"]*)"|'([^']*)')/g;
  let match = null;
  while ((match = re.exec(text))) {
    attrs[match[1]] = decodeXML(match[2] ?? match[3] ?? '');
  }
  return attrs;
}

function stripMujocoDocumentRoot(xml) {
  const withoutDecl = xml.replace(/<\?xml[\s\S]*?\?>/i, '');
  const open = withoutDecl.search(/<mujoco\b[^>]*>/i);
  const close = withoutDecl.lastIndexOf('</mujoco>');
  if (open < 0 || close < 0) return withoutDecl;
  const openEnd = withoutDecl.indexOf('>', open);
  return withoutDecl.slice(openEnd + 1, close);
}

async function expandMujocoIncludes(xml, baseDir = '', seen = new Set()) {
  const includeRe = /<include\b([^>]*?)\/\s*>/gi;
  let result = '';
  let lastIndex = 0;
  let match = null;
  while ((match = includeRe.exec(xml))) {
    result += xml.slice(lastIndex, match.index);
    lastIndex = includeRe.lastIndex;

    const attrs = parseAttributes(match[1] || '');
    if (!attrs.file) continue;

    const includePath = joinPath(baseDir, attrs.file);
    const entry = resolveAssetEntry(includePath) || resolveAssetEntry(attrs.file);
    if (!entry) {
      throw new Error(`MJCF include not found: ${attrs.file}. Select the model folder with Assets first.`);
    }
    if (seen.has(entry.rel)) {
      throw new Error(`Recursive MJCF include: ${entry.rel}`);
    }

    seen.add(entry.rel);
    const childXML = stripMujocoDocumentRoot(await entry.file.text());
    result += await expandMujocoIncludes(childXML, dirname(entry.rel), seen);
    seen.delete(entry.rel);
  }
  result += xml.slice(lastIndex);
  return result;
}

function parseNumbers(text, fallback = []) {
  if (!text) return fallback;
  const values = text.trim().split(/\s+/).map(Number).filter(Number.isFinite);
  return values.length ? values : fallback;
}

// Attributes whose value is a name or a reference to one (prefixed on <attach>).
const ATTACH_REF_ATTRS = new Set([
  'name', 'class', 'childclass', 'material', 'mesh', 'hfield', 'texture',
  'joint', 'joint1', 'joint2', 'site', 'site1', 'site2', 'refsite', 'sidesite',
  'tendon', 'tendon1', 'tendon2', 'body', 'body1', 'body2', 'geom', 'geom1',
  'geom2', 'objname']);

function prefixAttachSubtree(node, prefix, radToDeg) {
  if (node.nodeType !== 1) return;
  for (const attr of Array.from(node.attributes)) {
    if (ATTACH_REF_ATTRS.has(attr.name) && attr.value) {
      node.setAttribute(attr.name, prefix + attr.value);
    }
  }
  if (radToDeg) {
    // Child authored angles in radians; the merged model is parsed in degrees.
    const k = 180 / Math.PI;
    const e = node.getAttribute('euler');
    if (e) { const v = parseNumbers(e, []); if (v.length === 3) node.setAttribute('euler', v.map((x) => x * k).join(' ')); }
    const aa = node.getAttribute('axisangle');
    if (aa) { const v = parseNumbers(aa, []); if (v.length === 4) { v[3] *= k; node.setAttribute('axisangle', v.join(' ')); } }
  }
  for (const c of childElements(node)) prefixAttachSubtree(c, prefix, radToDeg);
}

function findBodyByNameDeep(node, name) {
  for (const b of childElements(node, 'body')) {
    if (b.getAttribute('name') === name) return b;
    const r = findBodyByNameDeep(b, name);
    if (r) return r;
  }
  return null;
}

// MJCF <asset><model name file> + <attach model body prefix>: graft the child
// model's named body subtree at the attach point, prefixing every name +
// reference, and merge the child's defaults/assets/tendons/actuators/etc.
// Mirrors the C++ ExpandAttachments. Used by iit_softfoot/scene.xml.
async function expandMujocoAttachments(root, baseDir) {
  const models = new Map();
  for (const m of root.querySelectorAll('asset model')) {
    const name = m.getAttribute('name');
    const file = m.getAttribute('file');
    if (name && file) models.set(name, file);
  }
  const attaches = Array.from(root.querySelectorAll('attach'));
  if (!attaches.length) return;

  const compilerEl = firstChildElement(root, 'compiler');
  const parentRadian = (compilerEl?.getAttribute('angle') || 'degree').toLowerCase() === 'radian';
  const ownerDoc = root.ownerDocument;

  for (const attach of attaches) {
    const modelName = attach.getAttribute('model');
    const bodyName = attach.getAttribute('body') || '';
    const prefix = attach.getAttribute('prefix') || '';
    const file = models.get(modelName);
    if (!file) { console.warn(`MJCF <attach>: unknown model "${modelName}"`); continue; }
    const childPath = joinPath(baseDir, file);
    const entry = resolveAssetEntry(childPath) || resolveAssetEntry(file);
    if (!entry) { console.warn(`MJCF <attach>: model file not found: ${file}`); continue; }

    const childDir = dirname(entry.rel);
    const childExpanded = await expandMujocoIncludes(await entry.file.text(), childDir);
    const childRoot = parseXMLDocument(childExpanded).documentElement;
    const cc = firstChildElement(childRoot, 'compiler');
    const childMeshdir = cc?.getAttribute('meshdir') || cc?.getAttribute('assetdir') || '';
    const childRadian = (cc?.getAttribute('angle') || 'degree').toLowerCase() === 'radian';
    const radToDeg = childRadian && !parentRadian;

    // Rewrite child mesh/hfield file paths so they resolve against the merged
    // model's asset folder (the child's dir + its meshdir).
    for (const tag of ['mesh', 'hfield', 'skin']) {
      for (const a of childRoot.querySelectorAll(`asset ${tag}`)) {
        const f = a.getAttribute('file');
        if (f) a.setAttribute('file', joinPath(joinPath(childDir, childMeshdir), f));
      }
    }

    prefixAttachSubtree(childRoot, prefix, radToDeg);

    let found = null;
    for (const wb of childRoot.querySelectorAll('worldbody')) {
      found = findBodyByNameDeep(wb, prefix + bodyName);
      if (found) break;
    }
    if (!found) { console.warn(`MJCF <attach>: body "${bodyName}" not found in "${modelName}"`); continue; }

    attach.parentNode.insertBefore(ownerDoc.importNode(found, true), attach);
    attach.parentNode.removeChild(attach);

    for (const sec of ['default', 'asset', 'tendon', 'actuator', 'sensor', 'equality', 'contact', 'keyframe']) {
      const childSec = firstChildElement(childRoot, sec);
      if (!childSec) continue;
      let mainSec = firstChildElement(root, sec);
      if (!mainSec) { mainSec = ownerDoc.createElement(sec); root.appendChild(mainSec); }
      for (const c of childElements(childSec)) {
        if (c.localName === 'model') continue;
        mainSec.appendChild(ownerDoc.importNode(c, true));
      }
    }
  }
}

function parseURDFMetadata(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  const robotEl = doc.querySelector('robot');
  const links = new Map();
  const joints = [];

  for (const linkEl of doc.querySelectorAll('link')) {
    const name = linkEl.getAttribute('name') || `link_${links.size}`;
    const inertialEl = linkEl.querySelector(':scope > inertial');
    const inertial = {};
    if (inertialEl) {
      const massEl = inertialEl.querySelector(':scope > mass');
      const originEl = inertialEl.querySelector(':scope > origin');
      const inertiaEl = inertialEl.querySelector(':scope > inertia');
      if (massEl) inertial.mass = Number(massEl.getAttribute('value')) || 0;
      if (originEl) inertial.centerOfMass = parseNumbers(originEl.getAttribute('xyz'), [0, 0, 0]);
      if (inertiaEl) {
        inertial.diagonalInertia = [
          Number(inertiaEl.getAttribute('ixx')) || 0,
          Number(inertiaEl.getAttribute('iyy')) || 0,
          Number(inertiaEl.getAttribute('izz')) || 0
        ];
      }
    }
    links.set(name, { name, inertial });
  }

  for (const jointEl of doc.querySelectorAll('joint')) {
    const parent = jointEl.querySelector(':scope > parent')?.getAttribute('link') || '';
    const child = jointEl.querySelector(':scope > child')?.getAttribute('link') || '';
    const axis = parseNumbers(jointEl.querySelector(':scope > axis')?.getAttribute('xyz'), [1, 0, 0]);
    const origin = parseNumbers(jointEl.querySelector(':scope > origin')?.getAttribute('xyz'), [0, 0, 0]);
    const rpy = parseNumbers(jointEl.querySelector(':scope > origin')?.getAttribute('rpy'), [0, 0, 0]);
    const originQuat = new THREE.Quaternion().setFromEuler(new THREE.Euler(rpy[0] || 0, rpy[1] || 0, rpy[2] || 0, 'XYZ'));
    const limitEl = jointEl.querySelector(':scope > limit');
    const dynamicsEl = jointEl.querySelector(':scope > dynamics');
    const mimicEl = jointEl.querySelector(':scope > mimic');
    let mimic = null;
    if (mimicEl?.getAttribute('joint')) {
      mimic = {
        joint: mimicEl.getAttribute('joint'),
        multiplier: numberAttr(mimicEl, 'multiplier', 1),
        offset: numberAttr(mimicEl, 'offset', 0)
      };
    } else if (mimicEl) {
      const mimicName = jointEl.getAttribute('name') || '?';
      console.warn(`URDF joint "${mimicName}" has a <mimic> with no joint target; mimic coupling is not exported to USD.`);
      setStatus('Note: malformed URDF <mimic> joint coupling is not exported.');
    }
    joints.push({
      name: jointEl.getAttribute('name') || `joint_${joints.length}`,
      type: jointEl.getAttribute('type') || 'fixed',
      parent,
      child,
      axis,
      axisToken: axisToToken(axis),
      origin,
      originMatrix: originToUSDMatrix(origin, rpy),
      localPos0: origin,
      localPos1: [0, 0, 0],
      localRot0: quaternionToUSDArray(originQuat),
      localRot1: [1, 0, 0, 0],
      limit: limitEl ? {
        lower: Number(limitEl.getAttribute('lower')),
        upper: Number(limitEl.getAttribute('upper')),
        effort: Number(limitEl.getAttribute('effort')),
        velocity: Number(limitEl.getAttribute('velocity'))
      } : {},
      dynamics: dynamicsEl ? {
        damping: Number(dynamicsEl.getAttribute('damping')),
        friction: Number(dynamicsEl.getAttribute('friction'))
      } : {},
      ...(mimic ? { mimic } : {})
    });
  }

  return {
    name: robotEl?.getAttribute('name') || '',
    links,
    joints
  };
}

function axisToToken(axis) {
  const abs = axis.map((v) => Math.abs(v));
  const max = Math.max(abs[0] || 0, abs[1] || 0, abs[2] || 0);
  if (max === abs[1]) return 'Y';
  if (max === abs[2]) return 'Z';
  return 'X';
}

function originToUSDMatrix(xyz, rpy) {
  const euler = new THREE.Euler(rpy[0] || 0, rpy[1] || 0, rpy[2] || 0, 'XYZ');
  const matrix = new THREE.Matrix4().compose(
    new THREE.Vector3(xyz[0] || 0, xyz[1] || 0, xyz[2] || 0),
    new THREE.Quaternion().setFromEuler(euler),
    new THREE.Vector3(1, 1, 1)
  );
  return matrixToUSDArray(matrix);
}

function childElements(node, name = null) {
  return Array.from(node?.children || []).filter((child) => !name || child.localName === name);
}

function firstChildElement(node, name) {
  return childElements(node, name)[0] || null;
}

function numberAttr(nodeOrAttrs, name, fallback = undefined) {
  const raw = typeof nodeOrAttrs?.getAttribute === 'function'
    ? nodeOrAttrs.getAttribute(name)
    : nodeOrAttrs?.[name];
  const value = Number(raw);
  return Number.isFinite(value) ? value : fallback;
}

function attrsFromElement(el) {
  const attrs = {};
  for (const attr of Array.from(el?.attributes || [])) attrs[attr.name] = attr.value;
  return attrs;
}

// MuJoCo <compiler> context for angle units + euler sequence. Set per-parse in
// parseMJCFWithMeshes; defaults match MuJoCo (degrees, "xyz").
let mjcfPoseCtx = { toRad: Math.PI / 180, eulerseq: 'xyz' };
// <asset><hfield> name -> { nrow, ncol, size, data } for the current model,
// populated (async, PNG-decoded) by parseMJCFWithMeshes.
let mjcfHFields = new Map();

function eulerQuatFromSeq(angles, seq, toRad) {
  const axisFor = (c) => (c === 'x' ? new THREE.Vector3(1, 0, 0)
    : c === 'y' ? new THREE.Vector3(0, 1, 0) : new THREE.Vector3(0, 0, 1));
  const q = new THREE.Quaternion();
  for (let i = 0; i < seq.length && i < angles.length; i++) {
    const c = seq[i];
    const lower = c.toLowerCase();
    const qi = new THREE.Quaternion().setFromAxisAngle(axisFor(lower), (angles[i] || 0) * toRad);
    if (c === lower) q.multiply(qi);   // lowercase = intrinsic (moving axes)
    else q.premultiply(qi);            // uppercase = extrinsic (fixed axes)
  }
  return q;
}

// Resolve any MuJoCo orientation specifier (quat/axisangle/euler/xyaxes/zaxis).
function orientationQuat(attrs, ctx) {
  const toRad = ctx.toRad;
  if (attrs.quat) {
    const q = parseNumbers(attrs.quat, [1, 0, 0, 0]);
    return new THREE.Quaternion(q[1] || 0, q[2] || 0, q[3] || 0, q[0] ?? 1).normalize();
  }
  if (attrs.axisangle) {
    const a = parseNumbers(attrs.axisangle, [0, 0, 1, 0]);
    const axis = new THREE.Vector3(a[0] || 0, a[1] || 0, a[2] || 0);
    if (axis.lengthSq() < 1e-12) axis.set(0, 0, 1);
    return new THREE.Quaternion().setFromAxisAngle(axis.normalize(), (a[3] || 0) * toRad);
  }
  if (attrs.euler) {
    return eulerQuatFromSeq(parseNumbers(attrs.euler, [0, 0, 0]), ctx.eulerseq, toRad);
  }
  if (attrs.xyaxes) {
    const v = parseNumbers(attrs.xyaxes, [1, 0, 0, 0, 1, 0]);
    const x = new THREE.Vector3(v[0], v[1], v[2]);
    if (x.lengthSq() < 1e-12) x.set(1, 0, 0);
    x.normalize();
    const y = new THREE.Vector3(v[3], v[4], v[5]);
    y.sub(x.clone().multiplyScalar(x.dot(y)));   // Gram-Schmidt against x
    if (y.lengthSq() < 1e-12) y.crossVectors(new THREE.Vector3(0, 0, 1), x);
    y.normalize();
    const z = new THREE.Vector3().crossVectors(x, y);
    return new THREE.Quaternion().setFromRotationMatrix(new THREE.Matrix4().makeBasis(x, y, z));
  }
  if (attrs.zaxis) {
    const v = parseNumbers(attrs.zaxis, [0, 0, 1]);
    const z = new THREE.Vector3(v[0] || 0, v[1] || 0, v[2] || 0);
    if (z.lengthSq() < 1e-12) z.set(0, 0, 1);
    return new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 0, 1), z.normalize());
  }
  return new THREE.Quaternion();
}

function matrixFromPoseAttrs(attrs = {}) {
  const pos = parseNumbers(attrs.pos, [0, 0, 0]);
  const translation = new THREE.Vector3(pos[0] || 0, pos[1] || 0, pos[2] || 0);
  const quat = orientationQuat(attrs, mjcfPoseCtx);
  return new THREE.Matrix4().compose(translation, quat, new THREE.Vector3(1, 1, 1));
}

// A MuJoCo 3 <frame> is a pure coordinate transform applied to its children (not
// a body). Dissolve each frame by composing its transform into every child's
// pose and lifting the children to the frame's parent, so downstream traversal
// never sees a <frame>. Needs mjcfPoseCtx set (uses the compiler angle units).
function flattenMujocoFrames(parent) {
  for (const c of Array.from(childElements(parent))) flattenMujocoFrames(c);
  for (const frame of Array.from(childElements(parent, 'frame'))) {
    const fm = matrixFromPoseAttrs(attrsFromElement(frame));
    const fcc = frame.getAttribute('childclass');
    const pos = new THREE.Vector3(), quat = new THREE.Quaternion(), scl = new THREE.Vector3();
    for (const gc of Array.from(childElements(frame))) {
      const cm = new THREE.Matrix4().multiplyMatrices(fm, matrixFromPoseAttrs(attrsFromElement(gc)));
      cm.decompose(pos, quat, scl);
      gc.setAttribute('pos', `${pos.x} ${pos.y} ${pos.z}`);
      gc.setAttribute('quat', `${quat.w} ${quat.x} ${quat.y} ${quat.z}`);  // MuJoCo wxyz
      for (const a of ['euler', 'axisangle', 'xyaxes', 'zaxis']) gc.removeAttribute(a);
      const ft = parseNumbers(gc.getAttribute('fromto'), []);
      if (ft.length === 6) {
        const p0 = new THREE.Vector3(ft[0], ft[1], ft[2]).applyMatrix4(fm);
        const p1 = new THREE.Vector3(ft[3], ft[4], ft[5]).applyMatrix4(fm);
        gc.setAttribute('fromto', `${p0.x} ${p0.y} ${p0.z} ${p1.x} ${p1.y} ${p1.z}`);
      }
      if (fcc && !gc.getAttribute('childclass') && !gc.getAttribute('class')) gc.setAttribute('childclass', fcc);
      parent.insertBefore(gc, frame);  // DOM move
    }
    parent.removeChild(frame);
  }
}

function decomposeMatrix(matrix) {
  const position = new THREE.Vector3();
  const quaternion = new THREE.Quaternion();
  const scale = new THREE.Vector3();
  matrix.decompose(position, quaternion, scale);
  return { position, quaternion, scale };
}

function quaternionToUSDArray(quaternion) {
  return [quaternion.w, quaternion.x, quaternion.y, quaternion.z];
}

function transformPointArray(matrix, values = [0, 0, 0]) {
  const point = vectorFromArray(values);
  point.applyMatrix4(matrix);
  return point.toArray();
}

function applyMatrixToObject(object, matrix) {
  matrix.decompose(object.position, object.quaternion, object.scale);
}

function collectMujocoAssets(root) {
  const compiler = firstChildElement(root, 'compiler');
  // <compiler meshdir> wins; assetdir is the shared meshdir/texturedir default.
  const meshDir = compiler?.getAttribute('meshdir') || compiler?.getAttribute('assetdir') || '';
  const meshes = new Map();
  for (const asset of childElements(root, 'asset')) {
    for (const mesh of childElements(asset, 'mesh')) {
      const file = mesh.getAttribute('file') || '';
      if (!file) continue;
      const name = mesh.getAttribute('name') || file.split('/').pop().replace(/\.[^.]+$/, '');
      meshes.set(name, {
        path: joinPath(meshDir, file),
        scale: parseNumbers(mesh.getAttribute('scale'), [1, 1, 1]),
        refpos: parseNumbers(mesh.getAttribute('refpos'), [0, 0, 0]),
        refquat: parseNumbers(mesh.getAttribute('refquat'), [1, 0, 0, 0])
      });
    }
  }
  return meshes;
}

function collectMujocoDefaults(root) {
  const defaults = {
    geom: new Map(),
    joint: new Map()
  };

  function mergeAttrs(base, attrs) {
    return { ...(base || {}), ...(attrs || {}) };
  }

  function visitDefault(defaultNode, inherited = { geom: {}, joint: {} }) {
    const className = defaultNode.getAttribute('class') || '';
    const next = {
      geom: mergeAttrs(inherited.geom, attrsFromElement(firstChildElement(defaultNode, 'geom'))),
      joint: mergeAttrs(inherited.joint, attrsFromElement(firstChildElement(defaultNode, 'joint')))
    };

    if (className) {
      defaults.geom.set(className, next.geom);
      defaults.joint.set(className, next.joint);
    } else {
      defaults.geom.set('', next.geom);
      defaults.joint.set('', next.joint);
    }

    for (const child of childElements(defaultNode, 'default')) {
      visitDefault(child, next);
    }
  }

  for (const defaultNode of childElements(root, 'default')) {
    visitDefault(defaultNode);
  }
  return defaults;
}

function resolveMujocoAttrs(node, defaults, kind, inheritedClass = '') {
  const attrs = attrsFromElement(node);
  const className = attrs.class || inheritedClass || '';
  return {
    ...(defaults?.[kind]?.get('') || {}),
    ...(className ? defaults?.[kind]?.get(className) || {} : {}),
    ...attrs,
    class: className || attrs.class
  };
}

function makeGeometryPayload(mesh, matrix, name) {
  const geom = mesh.geometry;
  const pos = geom?.getAttribute('position');
  if (!pos || pos.count < 3) return null;
  const normal = geom.getAttribute('normal');
  const uv = geom.getAttribute('uv');
  const index = geom.getIndex();
  return {
    name,
    matrix: matrixToUSDArray(matrix),
    geometry: {
      positions: Array.from(pos.array),
      normals: normal ? Array.from(normal.array) : [],
      uvs: uv ? Array.from(uv.array) : [],
      indices: index ? Array.from(index.array) : []
    }
  };
}

function collectMeshPayloads(root, baseMatrix, fallbackName) {
  const payloads = [];
  root.updateMatrixWorld(true);
  let index = 0;
  root.traverse((obj) => {
    if (!obj.isMesh) return;
    const matrix = new THREE.Matrix4().copy(baseMatrix).multiply(obj.matrixWorld);
    const payload = makeGeometryPayload(obj, matrix, obj.name || `${fallbackName}_${index}`);
    if (payload) {
      payload.name = payloads.length ? `${payload.name}_${payloads.length}` : payload.name;
      payloads.push(payload);
    }
    index++;
  });
  return payloads;
}

function asFloat32Array(values) {
  return values instanceof Float32Array ? values : new Float32Array(values || []);
}

function asInt32Array(values) {
  return values instanceof Int32Array ? values : new Int32Array(values || []);
}

function asUint32Array(values) {
  return values instanceof Uint32Array ? values : new Uint32Array(values || []);
}

function nativeMeshBufferForMesh(mesh, meshRef) {
  if (state.nativeMeshBuffers.has(meshRef)) return meshRef;
  const geom = mesh.geometry;
  const pos = geom?.getAttribute('position');
  if (!pos || pos.count < 3) return null;
  const normal = geom.getAttribute('normal');
  const uv = geom.getAttribute('uv');
  const index = geom.getIndex();
  let indices = null;
  if (index) {
    indices = asInt32Array(index.array);
  } else {
    indices = new Int32Array(pos.count);
    for (let i = 0; i < indices.length; i++) indices[i] = i;
  }
  state.nativeMeshBuffers.set(meshRef, {
    positions: asFloat32Array(pos.array),
    normals: normal ? asFloat32Array(normal.array) : new Float32Array(),
    uvs: uv ? asFloat32Array(uv.array) : new Float32Array(),
    indices
  });
  return meshRef;
}

function collectNativeMeshRefPayloads(root, baseMatrix, fallbackName, meshRefPrefix) {
  const payloads = [];
  root.updateMatrixWorld(true);
  let index = 0;
  root.traverse((obj) => {
    if (!obj.isMesh) return;
    const meshRef = sanitizeUSDIdentifier(`${meshRefPrefix}_${obj.name || index}`, 'mesh');
    if (!nativeMeshBufferForMesh(obj, meshRef)) return;
    const matrix = new THREE.Matrix4().copy(baseMatrix).multiply(obj.matrixWorld);
    payloads.push({
      name: payloads.length ? `${fallbackName}_${payloads.length}` : fallbackName,
      matrix: matrixToUSDArray(matrix),
      meshRef
    });
    index++;
  });
  return payloads;
}

function primitiveObjectFromGeometry(geometry, name) {
  const mesh = new THREE.Mesh(
    geometry,
    new THREE.MeshStandardMaterial({ color: 0x9fb6c7, roughness: 0.68, metalness: 0.02 })
  );
  mesh.name = name;
  const group = new THREE.Group();
  group.name = name;
  group.add(mesh);
  return group;
}

// Merge every sub-mesh of `object` (e.g. an OBJ split into many `o`/`g`
// objects, as in ms_human_700's Rib1L.obj) into a single mesh. A MuJoCo
// <mesh> is one mesh regardless of how the source file is grouped, matching
// the native loader / JS CLI and MuJoCo semantics; keeps the demo's source
// view and exported USD mesh counts consistent with the CLI.
function flattenToSingleMesh(object, name) {
  object.updateMatrixWorld(true);
  const positions = [];
  const normals = [];
  const uvs = [];
  const indices = [];
  let base = 0;
  let hasNormals = true;
  let hasUVs = true;
  const v = new THREE.Vector3();
  const nrm = new THREE.Vector3();
  let meshCount = 0;
  object.traverse((obj) => {
    if (!obj.isMesh || !obj.geometry) return;
    meshCount++;
    const g = obj.geometry;
    const pos = g.getAttribute('position');
    if (!pos) return;
    const nAttr = g.getAttribute('normal');
    const uvAttr = g.getAttribute('uv');
    const idx = g.getIndex();
    const m = obj.matrixWorld;
    const normalMat = new THREE.Matrix3().getNormalMatrix(m);
    for (let i = 0; i < pos.count; i++) {
      v.set(pos.getX(i), pos.getY(i), pos.getZ(i)).applyMatrix4(m);
      positions.push(v.x, v.y, v.z);
      if (nAttr) {
        nrm.set(nAttr.getX(i), nAttr.getY(i), nAttr.getZ(i)).applyMatrix3(normalMat).normalize();
        normals.push(nrm.x, nrm.y, nrm.z);
      } else {
        hasNormals = false;
      }
      if (uvAttr) uvs.push(uvAttr.getX(i), uvAttr.getY(i)); else hasUVs = false;
    }
    if (idx) {
      for (let i = 0; i < idx.count; i++) indices.push(base + idx.getX(i));
    } else {
      for (let i = 0; i < pos.count; i++) indices.push(base + i);
    }
    base += pos.count;
  });
  if (meshCount <= 1) return object;  // nothing to merge
  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  if (hasNormals && normals.length) geom.setAttribute('normal', new THREE.Float32BufferAttribute(normals, 3));
  if (hasUVs && uvs.length) geom.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
  geom.setIndex(indices);
  return primitiveObjectFromGeometry(geom, name);
}

function mujocoJointType(type) {
  if (type === 'hinge') return 'revolute';
  if (type === 'slide') return 'prismatic';
  if (type === 'free') return 'floating';
  // MuJoCo ball joint (3-DOF rotation) -> USD PhysicsSphericalJoint. Best-effort:
  // the joint round-trips structurally; the single-slider preview leaves it at
  // rest (see setJointValue) rather than collapsing it to a rigid weld.
  if (type === 'ball') return 'spherical';
  return 'fixed';
}

function parseMujocoInertial(bodyNode) {
  const inertialNode = firstChildElement(bodyNode, 'inertial');
  if (!inertialNode) return {};
  const full = parseNumbers(inertialNode.getAttribute('fullinertia'), []);
  const inertial = {
    mass: numberAttr(inertialNode, 'mass', 0),
    centerOfMass: parseNumbers(inertialNode.getAttribute('pos'), [0, 0, 0])
  };
  if (full.length >= 3) {
    inertial.diagonalInertia = [full[0], full[1], full[2]];
    // fullinertia is "Ixx Iyy Izz Ixy Ixz Iyz"; USD physics:diagonalInertia +
    // physics:principalAxes would be needed to represent a non-diagonal tensor.
    // We only export the diagonal, so flag when off-diagonal terms are dropped.
    if (full.length >= 6 && full.slice(3, 6).some((v) => Math.abs(v) > 1e-9)) {
      const name = bodyNode.getAttribute?.('name') || '?';
      console.warn(
        `MJCF body "${name}" has a non-diagonal fullinertia; off-diagonal terms `
        + `(Ixy/Ixz/Iyz) are dropped — only the diagonal is exported.`
      );
    }
  } else {
    inertial.diagonalInertia = parseNumbers(inertialNode.getAttribute('diaginertia'), []);
  }
  return inertial;
}

// MuJoCo capsule/cylinder fromto="x1 y1 z1 x2 y2 z2": the geom spans p1->p2 with
// radius=size[0]; pos/quat are ignored when fromto is present. Returns the local
// center, segment length, and the rotation aligning the +Y axis (three.js
// cylinder axis) to the segment direction.
function mujocoFromto(attrs) {
  const ft = parseNumbers(attrs?.fromto, []);
  if (ft.length < 6) return null;
  const p1 = new THREE.Vector3(ft[0], ft[1], ft[2]);
  const p2 = new THREE.Vector3(ft[3], ft[4], ft[5]);
  const dir = p2.clone().sub(p1);
  const length = dir.length() || 1e-6;
  const ndir = dir.clone().normalize();
  return {
    center: p1.clone().add(p2).multiplyScalar(0.5),
    length,
    dir: ndir,
    quatY: new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 1, 0), ndir),
    quatZ: new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 0, 1), ndir)
  };
}

// MuJoCo mesh refpos/refquat define the mesh's reference frame: vertices are
// translated by -refpos and rotated by the conjugate of refquat before the geom
// places them. Returns the corresponding local matrix (identity by default).
function mujocoMeshRefMatrix(meshAsset) {
  const rq = meshAsset?.refquat || [1, 0, 0, 0];
  const rp = meshAsset?.refpos || [0, 0, 0];
  const q = new THREE.Quaternion(rq[1] || 0, rq[2] || 0, rq[3] || 0, rq[0] ?? 1).normalize().conjugate();
  return new THREE.Matrix4().makeRotationFromQuaternion(q)
    .multiply(new THREE.Matrix4().makeTranslation(-(rp[0] || 0), -(rp[1] || 0), -(rp[2] || 0)));
}

function shapePayloadForMujocoGeom(geomAttrs, originMatrix, fallbackName) {
  const attrs = geomAttrs || {};
  const geomType = attrs.type || (attrs.mesh ? 'mesh' : 'sphere');
  if (geomType === 'mesh') return null;

  const size = parseNumbers(attrs.size, []);
  if (geomType === 'box') {
    const matrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(new THREE.Matrix4().makeScale(size[0] || 1, size[1] || 1, size[2] || 1));
    return [{ name: fallbackName, matrix: matrixToUSDArray(matrix), shape: { type: 'box' } }];
  }

  if (geomType === 'sphere') {
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(originMatrix),
      shape: { type: 'sphere', radius: size[0] || 0.5 }
    }];
  }

  if (geomType === 'cylinder' || geomType === 'capsule') {
    // fromto: bake the segment midpoint + Z->direction into the matrix and use
    // the segment length as height (pos/quat ignored when fromto is present).
    const ft = mujocoFromto(attrs);
    const matrix = ft
      ? new THREE.Matrix4().copy(originMatrix)
          .multiply(new THREE.Matrix4().compose(ft.center, ft.quatZ, new THREE.Vector3(1, 1, 1)))
      : originMatrix;
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(matrix),
      shape: {
        type: geomType,
        radius: size[0] || 0.5,
        height: ft ? ft.length : (size[1] ? size[1] * 2 : 1),
        axis: 'Z'
      }
    }];
  }

  if (geomType === 'plane') {
    return [{
      name: fallbackName,
      matrix: matrixToUSDArray(originMatrix),
      shape: {
        type: 'plane',
        width: (size[0] && size[0] > 0 ? size[0] : 1) * 2,
        length: (size[1] && size[1] > 0 ? size[1] : 1) * 2,
        axis: 'Z'
      }
    }];
  }

  return null;
}

// Tessellate a heightfield's top surface into a THREE.BufferGeometry, matching
// the CLI's buildHFieldGeometry / C++ TessellateHField exactly: x in [-rx,rx]
// over ncol, y in [-ry,ry] over nrow, z = normalized*elevation_z, with smooth
// per-vertex normals from the (edge-correct) height gradient.
function buildHFieldGeometry(hf) {
  const nr = hf.nrow, nc = hf.ncol;
  const rx = hf.size[0], ry = hf.size[1], ez = hf.size[2] ?? 1;
  const dx = (2 * rx) / (nc - 1), dy = (2 * ry) / (nr - 1);
  const H = (r, c) => {
    r = Math.max(0, Math.min(nr - 1, r));
    c = Math.max(0, Math.min(nc - 1, c));
    return hf.data[r * nc + c] * ez;
  };
  const positions = new Float32Array(nr * nc * 3);
  const normals = new Float32Array(nr * nc * 3);
  for (let r = 0; r < nr; r++) {
    for (let c = 0; c < nc; c++) {
      const i = (r * nc + c) * 3;
      positions[i] = -rx + c * dx;
      positions[i + 1] = -ry + r * dy;
      positions[i + 2] = H(r, c);
      const cl = Math.max(0, c - 1), cr = Math.min(nc - 1, c + 1);
      const rb = Math.max(0, r - 1), rt = Math.min(nr - 1, r + 1);
      const hx = (H(r, cr) - H(r, cl)) / ((cr - cl) * dx);
      const hy = (H(rt, c) - H(rb, c)) / ((rt - rb) * dy);
      let nx = -hx, ny = -hy, nz = 1;
      const len = Math.hypot(nx, ny, nz) || 1;
      normals[i] = nx / len; normals[i + 1] = ny / len; normals[i + 2] = nz / len;
    }
  }
  const indices = new Uint32Array((nr - 1) * (nc - 1) * 6);
  let k = 0;
  for (let r = 0; r < nr - 1; r++) {
    for (let c = 0; c < nc - 1; c++) {
      const v00 = r * nc + c, v01 = r * nc + (c + 1);
      const v10 = (r + 1) * nc + c, v11 = (r + 1) * nc + (c + 1);
      indices[k++] = v00; indices[k++] = v01; indices[k++] = v11;
      indices[k++] = v00; indices[k++] = v11; indices[k++] = v10;
    }
  }
  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geom.setAttribute('normal', new THREE.BufferAttribute(normals, 3));
  geom.setIndex(new THREE.BufferAttribute(indices, 1));
  return geom;
}

// Decode an image File to a grayscale (luminance) pixel array via the browser's
// image pipeline (createImageBitmap + canvas), for <asset><hfield file=.png>.
async function decodeImageToGray(file) {
  const bitmap = await createImageBitmap(file);
  const w = bitmap.width, h = bitmap.height;
  const canvas = (typeof OffscreenCanvas !== 'undefined')
    ? new OffscreenCanvas(w, h)
    : Object.assign(document.createElement('canvas'), { width: w, height: h });
  const ctx = canvas.getContext('2d');
  ctx.drawImage(bitmap, 0, 0);
  const px = ctx.getImageData(0, 0, w, h).data;  // RGBA, rows top-to-bottom
  bitmap.close?.();
  const gray = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i++) {
    gray[i] = Math.round(0.299 * px[4 * i] + 0.587 * px[4 * i + 1] + 0.114 * px[4 * i + 2]);
  }
  return { width: w, height: h, gray };
}

// Collect <asset><hfield> (name -> { nrow, ncol, size, data[0..1] }). File-based
// fields decode a PNG (rows reversed: MuJoCo row 0 = -y edge); inline fields use
// nrow/ncol/elevation. Async because the browser image decode is async.
async function collectMujocoHFields(root) {
  const compiler = firstChildElement(root, 'compiler');
  const meshDir = compiler?.getAttribute('meshdir') || compiler?.getAttribute('assetdir') || '';
  const hfields = new Map();
  for (const asset of childElements(root, 'asset')) {
    for (const hf of childElements(asset, 'hfield')) {
      const file = hf.getAttribute('file') || '';
      const name = hf.getAttribute('name') || (file ? file.split('/').pop().replace(/\.[^.]+$/, '') : '');
      if (!name) continue;
      const size = parseNumbers(hf.getAttribute('size'), [1, 1, 1, 0.1]);
      let nrow = 0, ncol = 0, data = null;
      if (file) {
        const entry = resolveAssetEntry(joinPath(meshDir, file)) || resolveAssetEntry(file);
        if (entry) {
          try {
            const png = await decodeImageToGray(entry.file);
            ncol = png.width; nrow = png.height;
            data = new Float32Array(nrow * ncol);
            for (let r = 0; r < nrow; r++) {
              for (let c = 0; c < ncol; c++) {
                data[r * ncol + c] = png.gray[c + (nrow - 1 - r) * ncol];
              }
            }
          } catch (err) {
            console.warn(`MJCF hfield decode failed for ${file}:`, err);
          }
        }
      } else {
        nrow = Math.trunc(Number(hf.getAttribute('nrow')) || 0);
        ncol = Math.trunc(Number(hf.getAttribute('ncol')) || 0);
        const elev = parseNumbers(hf.getAttribute('elevation'), []);
        if (nrow > 0 && ncol > 0 && elev.length === nrow * ncol) data = Float32Array.from(elev);
      }
      if (nrow > 1 && ncol > 1 && data) {
        let emin = Infinity, emax = -Infinity;
        for (const v of data) { if (v < emin) emin = v; if (v > emax) emax = v; }
        const range = emax - emin;
        for (let i = 0; i < data.length; i++) { data[i] -= emin; if (range > 1e-9) data[i] /= range; }
        hfields.set(name, { nrow, ncol, size, data });
      }
    }
  }
  return hfields;
}

async function loadMujocoGeomObject(geomAttrs, meshAssets, fallbackName) {
  const attrs = geomAttrs || {};
  const geomType = attrs.type || (attrs.mesh ? 'mesh' : 'sphere');
  if (geomType === 'hfield') {
    const hf = mjcfHFields.get(attrs.hfield);
    if (!hf) return makeMissingMesh(attrs.hfield || fallbackName);
    return primitiveObjectFromGeometry(buildHFieldGeometry(hf), fallbackName);
  }
  if (geomType === 'mesh') {
    const meshAsset = meshAssets.get(attrs.mesh);
    if (!meshAsset) return makeMissingMesh(attrs.mesh || fallbackName);
    // A MuJoCo <mesh> is a single mesh: merge multi-object source files so the
    // source view's mesh count matches the exported USD and the CLI.
    return flattenToSingleMesh(await loadMeshObject(meshAsset.path), attrs.mesh || fallbackName);
  }

  const size = parseNumbers(attrs.size, []);
  if (geomType === 'box') {
    return primitiveObjectFromGeometry(
      new THREE.BoxGeometry((size[0] || 1) * 2, (size[1] || 1) * 2, (size[2] || 1) * 2),
      fallbackName
    );
  }
  if (geomType === 'sphere') {
    return primitiveObjectFromGeometry(new THREE.SphereGeometry(size[0] || 0.5, 24, 12), fallbackName);
  }
  if (geomType === 'cylinder' || geomType === 'capsule') {
    const radius = size[0] || 0.5;
    const ft = mujocoFromto(attrs);
    const length = ft ? ft.length : (size[1] ? size[1] * 2 : 1);
    return primitiveObjectFromGeometry(new THREE.CylinderGeometry(radius, radius, length, 24, 1), fallbackName);
  }
  if (geomType === 'plane') {
    return primitiveObjectFromGeometry(
      new THREE.PlaneGeometry((size[0] && size[0] > 0 ? size[0] : 1) * 2, (size[1] && size[1] > 0 ? size[1] : 1) * 2),
      fallbackName
    );
  }

  return makeMissingMesh(fallbackName);
}

async function mujocoGeomPayloads(geomAttrs, meshAssets, fallbackName, baseMatrix = new THREE.Matrix4()) {
  const attrs = geomAttrs || {};
  const geomType = attrs.type || (attrs.mesh ? 'mesh' : 'sphere');
  const originMatrix = new THREE.Matrix4().copy(baseMatrix).multiply(matrixFromPoseAttrs(attrs));
  if (geomType === 'mesh') {
    const meshAsset = meshAssets.get(attrs.mesh);
    if (!meshAsset) return [];
    // Merge multi-object source meshes into one (MuJoCo <mesh> == one mesh).
    const object = flattenToSingleMesh(await loadMeshObject(meshAsset.path), attrs.mesh || fallbackName);
    const scale = parseNumbers(attrs.scale, meshAsset.scale || [1, 1, 1]);
    const meshMatrix = new THREE.Matrix4()
      .copy(originMatrix)
      .multiply(mujocoMeshRefMatrix(meshAsset))
      .multiply(new THREE.Matrix4().makeScale(scale[0] || 1, scale[1] || 1, scale[2] || 1));
    return collectNativeMeshRefPayloads(object, meshMatrix, fallbackName, meshAsset.path || attrs.mesh || fallbackName);
  }

  const object = await loadMujocoGeomObject(attrs, meshAssets, fallbackName);
  const yToZ = geomType === 'cylinder' || geomType === 'capsule'
    ? new THREE.Matrix4().makeRotationX(Math.PI / 2)
    : new THREE.Matrix4();
  return collectMeshPayloads(object, new THREE.Matrix4().copy(originMatrix).multiply(yToZ), fallbackName);
}

function classifyMujocoGeom(geomAttrs) {
  const attrs = geomAttrs || {};
  // MuJoCo visibility is by geom group: 0-2 are the default-visible set, 3-5 are
  // hidden/auxiliary (collision). The resolved group (explicit on the geom, else
  // from its class) is authoritative and wins over the class NAME — e.g.
  // iit_softfoot tags class="collision" meshes with group="0" to show them, and
  // shadow_dexee's group-5 CollisionGeom capsules must stay hidden.
  const g = Number(attrs.group);
  if (Number.isFinite(g)) return g < 3;
  // No group => MuJoCo default group 0 (visible); use the class name only as a
  // hint to keep ungrouped collision-class geoms hidden.
  const className = String(attrs.class || '').toLowerCase();
  if (className.includes('collision') || className.includes('collider')) return false;
  return true;
}

// A geom's MuJoCo display group (0-5), resolved from its own/class `group`.
// Ungrouped geoms default to 0 (visible) or 3 (collision-class / non-visual).
function mujocoGeomGroup(geomAttrs, isVisual) {
  const g = Number((geomAttrs || {}).group);
  if (Number.isFinite(g)) return Math.max(0, Math.min(5, Math.trunc(g)));
  return isVisual ? 0 : 3;
}

// True when a geom actually collides (MuJoCo default contype=conaffinity=1;
// only a pure decoration sets BOTH to 0, e.g. iit_softfoot's group-4
// virtual_pulley cylinders). Used to keep the red collision overlay limited to
// real colliders while non-collider auxiliary geoms keep their own rgba.
function isMujocoCollider(geomAttrs) {
  const attrs = geomAttrs || {};
  const ct = attrs.contype !== undefined ? Number(attrs.contype) : 1;
  const ca = attrs.conaffinity !== undefined ? Number(attrs.conaffinity) : 1;
  return ct !== 0 || ca !== 0;
}

function applyMujocoObjectDisplayTransform(object, geomAttrs, meshAssets) {
  const attrs = geomAttrs || {};
  const geomType = attrs.type || (attrs.mesh ? 'mesh' : 'sphere');
  // fromto fully specifies a capsule/cylinder's placement (pos/quat ignored):
  // center it on the segment midpoint and align the cylinder's +Y axis to it.
  if (geomType === 'cylinder' || geomType === 'capsule') {
    const ft = mujocoFromto(attrs);
    if (ft) {
      applyMatrixToObject(object, new THREE.Matrix4().compose(ft.center, ft.quatY, new THREE.Vector3(1, 1, 1)));
      return;
    }
  }
  const matrix = matrixFromPoseAttrs(attrs);
  if (geomType === 'mesh') {
    const meshAsset = meshAssets.get(attrs.mesh);
    const scale = parseNumbers(attrs.scale, meshAsset?.scale || [1, 1, 1]);
    // Apply the mesh reference frame (refpos/refquat) before scale, so e.g.
    // shadow_dexee's finger meshes are oriented correctly.
    matrix
      .multiply(mujocoMeshRefMatrix(meshAsset))
      .multiply(new THREE.Matrix4().makeScale(scale[0] || 1, scale[1] || 1, scale[2] || 1));
  } else if (geomType === 'cylinder' || geomType === 'capsule') {
    matrix.multiply(new THREE.Matrix4().makeRotationX(Math.PI / 2));
  }
  applyMatrixToObject(object, matrix);
}

function buildMujocoActuators(root) {
  const actuators = [];
  for (const actuatorRoot of childElements(root, 'actuator')) {
    for (const actNode of childElements(actuatorRoot)) {
      const attrs = attrsFromElement(actNode);
      const joint = attrs.joint || '';
      const name = attrs.name || `${actNode.localName || 'actuator'}_${joint || actuators.length}`;
      if (!joint) {
        if (attrs.tendon || attrs.site || attrs.body || ['muscle', 'general', 'adhesion',
            'cylinder', 'intvelocity', 'damper', 'plugin'].includes(actNode.localName)) {
          continue;
        }
        console.warn(`MJCF actuator "${name}" has no joint target; it is not converted to USD.`);
        setStatus('Note: MJCF non-joint actuators are not converted.');
        continue;
      }
      const act = { name, joint, control: 'pd' };
      const kp = numberAttr(attrs, 'kp');
      if (Number.isFinite(kp)) {
        act.kp = kp;
      } else {
        const gain = parseNumbers(attrs.gainprm, []);
        if (gain.length) act.kp = gain[0];
      }
      const kd = numberAttr(attrs, 'kv');
      if (Number.isFinite(kd)) {
        act.kd = kd;
      } else {
        const bias = parseNumbers(attrs.biasprm, []);
        if (bias.length >= 3) act.kd = Math.abs(bias[2]);
      }
      const forceRange = parseNumbers(attrs.forcerange, []);
      if (forceRange.length >= 2) {
        act.maxEffort = Math.max(Math.abs(forceRange[0]), Math.abs(forceRange[1]));
      }
      const ctrlRange = parseNumbers(attrs.ctrlrange, []);
      if (actNode.localName === 'motor' && ctrlRange.length >= 2) {
        act.constEffort = Math.max(Math.abs(ctrlRange[0]), Math.abs(ctrlRange[1]));
      }
      const delay = numberAttr(attrs, 'delay');
      if (Number.isFinite(delay)) act.delaySteps = Math.max(1, Math.round(delay));
      actuators.push(act);
    }
  }
  return actuators;
}

function parseMujocoTendons(root) {
  const tendonRoot = firstChildElement(root, 'tendon');
  if (!tendonRoot) return [];
  const out = [];
  for (const node of childElements(tendonRoot)) {
    if (node.localName !== 'fixed' && node.localName !== 'spatial') continue;
    const range = parseNumbers(node.getAttribute('range'), []);
    const frcRange = parseNumbers(node.getAttribute('actuatorfrcrange'), []);
    const tendon = {
      name: node.getAttribute('name') || `tendon_${out.length}`,
      type: node.localName
    };
    if (node.localName === 'fixed') {
      const joints = [];
      for (const j of childElements(node, 'joint')) {
        const joint = j.getAttribute('joint');
        if (!joint) continue;
        joints.push({ joint, coef: numberAttr(j, 'coef', 1) });
      }
      if (!joints.length) continue;
      tendon.joints = joints;
    } else {
      const path = [];
      const sideSites = [];
      for (const w of childElements(node)) {
        if (w.localName === 'site' && w.getAttribute('site')) {
          path.push({ site: w.getAttribute('site') });
        } else if (w.localName === 'geom') {
          const wp = { geom: w.getAttribute('geom') || '' };
          if (w.getAttribute('sidesite')) {
            wp.sidesite = w.getAttribute('sidesite');
            sideSites.push({ site: w.getAttribute('sidesite') });
          }
          path.push(wp);
        } else if (w.localName === 'pulley') {
          const divisor = numberAttr(w, 'divisor');
          if (Number.isFinite(divisor)) {
            if (!tendon.routeDivisors) tendon.routeDivisors = [];
            tendon.routeDivisors.push(divisor);
          }
        }
      }
      if (!path.length) continue;
      tendon.path = path;
      if (sideSites.length) tendon.sideSites = sideSites;
    }

    const setNum = (attr, key = attr) => {
      const value = numberAttr(node, attr);
      if (Number.isFinite(value)) tendon[key] = value;
    };
    setNum('stiffness');
    setNum('damping');
    setNum('armature');
    setNum('frictionloss');
    setNum('margin');
    setNum('width');
    if (node.getAttribute('group') !== null) tendon.group = Math.round(numberAttr(node, 'group'));
    if (node.getAttribute('limited') !== null) tendon.limited = node.getAttribute('limited');
    if (node.getAttribute('actuatorfrclimited') !== null) tendon.actuatorfrclimited = node.getAttribute('actuatorfrclimited');
    if (range.length >= 2) tendon.range = [range[0], range[1]];
    if (frcRange.length >= 2) tendon.actuatorfrcrange = [frcRange[0], frcRange[1]];
    const rgba = parseNumbers(node.getAttribute('rgba'), []);
    if (rgba.length >= 4) tendon.rgba = rgba.slice(0, 4);
    for (const key of ['springlength', 'solreflimit', 'solimplimit', 'solreffriction', 'solimpfriction']) {
      const values = parseNumbers(node.getAttribute(key), []);
      if (values.length) tendon[key] = values;
    }
    out.push(tendon);
  }
  return out;
}

function parseMujocoMJCActuators(root) {
  const actRoot = firstChildElement(root, 'actuator');
  if (!actRoot) return [];
  const out = [];
  for (const node of childElements(actRoot)) {
    const type = node.localName;
    const tendon = node.getAttribute('tendon') || '';
    const site = node.getAttribute('site') || '';
    const joint = node.getAttribute('joint') || '';
    const body = node.getAttribute('body') || '';
    const isMjc = type === 'muscle' || type === 'general' || type === 'adhesion'
      || type === 'cylinder' || type === 'intvelocity' || type === 'damper'
      || type === 'plugin' || tendon || site || body;
    if (!isMjc) continue;
    const a = {
      name: node.getAttribute('name') || `${type}_${joint || tendon || site || body}`,
      actuatorType: type
    };
    if (joint) a.targetJoint = joint;
    if (tendon) a.targetTendon = tendon;
    if (site) a.targetSite = site;
    if (body) a.targetBody = body;
    for (const [attr, key] of [
      ['gainprm', 'gainPrm'], ['biasprm', 'biasPrm'],
      ['dynprm', 'dynPrm'], ['gear', 'gear']
    ]) {
      const values = parseNumbers(node.getAttribute(attr), []);
      if (values.length) a[key] = values;
    }
    if (!a.gainPrm && node.getAttribute('gain') !== null) a.gainPrm = [numberAttr(node, 'gain')];
    for (const [attr, key] of [
      ['lengthrange', 'lengthRange'], ['ctrlrange', 'ctrlRange'],
      ['forcerange', 'forceRange'], ['actrange', 'actRange']
    ]) {
      const values = parseNumbers(node.getAttribute(attr), []);
      if (values.length >= 2) a[key] = [values[0], values[1]];
    }
    for (const [attr, key] of [
      ['gaintype', 'gainType'], ['biastype', 'biasType'], ['dyntype', 'dynType'],
      ['ctrllimited', 'ctrlLimited'], ['forcelimited', 'forceLimited'],
      ['actlimited', 'actLimited'], ['plugin', 'plugin'], ['instance', 'instance']
    ]) {
      if (node.getAttribute(attr) !== null) a[key] = node.getAttribute(attr);
    }
    for (const [attr, key] of [
      ['group', 'group'], ['actdim', 'actDim'], ['cranklength', 'crankLength'],
      ['inheritrange', 'inheritRange']
    ]) {
      const value = numberAttr(node, attr);
      if (Number.isFinite(value)) a[key] = value;
    }
    for (const [attr, key] of [['jointinparent', 'jointInParent'], ['actearly', 'actEarly']]) {
      if (node.getAttribute(attr) !== null) a[key] = mjcBoolAttr(node.getAttribute(attr));
    }
    if (node.getAttribute('refsite') !== null) a.refSite = node.getAttribute('refsite');
    if (node.getAttribute('slidersite') !== null) a.sliderSite = node.getAttribute('slidersite');
    out.push(a);
  }
  return out;
}

// Compute merged line-segment data for MuJoCo tendon primitives. Spatial
// tendons route through <site>s / geom sidesites; fixed tendons route through
// their referenced joint positions. All segments merge into one buffer so large
// muscle models stay cheap.
// `sitesByName` maps site name -> { pos, matrix } in model space.
// Returns { positions, colors, tendonCount } (model-space) or null. The data is
// reused to draw tendons on BOTH the MJCF source and the converted-USD
// view, which share the same model coordinate space.
function computeMuscleLineData(root, sitesByName, jointPositionsByName = new Map()) {
  // Tendon default rgba/width from <default><tendon ...> (collectMujocoDefaults
  // only tracks geom/joint, so read the tendon default directly here). MuJoCo's
  // tendon `width` is the rendered tube RADIUS.
  let defRgba = [0.95, 0.3, 0.3, 1];
  let defWidth = 0.003;
  const rootDefault = firstChildElement(root, 'default');
  const defTendon = rootDefault ? firstChildElement(rootDefault, 'tendon') : null;
  if (defTendon?.getAttribute('rgba')) {
    defRgba = parseNumbers(defTendon.getAttribute('rgba'), defRgba);
  }
  if (defTendon?.getAttribute('width')) {
    defWidth = Number(defTendon.getAttribute('width')) || defWidth;
  }

  // Per-segment parallel arrays: positions (6 each: a,b), rgba (4 each), radii.
  const positions = [];
  const rgba = [];
  const radii = [];
  let tendonCount = 0;
  const pushSegments = (pts, col, width) => {
    if (pts.length < 2) return false;
    for (let i = 0; i + 1 < pts.length; i++) {
      positions.push(pts[i].x, pts[i].y, pts[i].z,
                     pts[i + 1].x, pts[i + 1].y, pts[i + 1].z);
      rgba.push(col[0], col[1], col[2], col[3] ?? 1);
      radii.push(width);
    }
    return true;
  };
  for (const tendonRoot of childElements(root, 'tendon')) {
    for (const spatial of childElements(tendonRoot, 'spatial')) {
      const col = spatial.getAttribute('rgba')
        ? parseNumbers(spatial.getAttribute('rgba'), defRgba) : defRgba;
      const width = spatial.getAttribute('width')
        ? (Number(spatial.getAttribute('width')) || defWidth) : defWidth;
      // Resolve the ordered waypoints: <site> by name, and <geom sidesite=...>
      // wraps approximated by their sidesite (the via point on the wrap surface).
      const pts = [];
      for (const child of childElements(spatial)) {
        let ref = null;
        if (child.localName === 'site') ref = child.getAttribute('site');
        else if (child.localName === 'geom') ref = child.getAttribute('sidesite');
        if (!ref) continue;
        const site = sitesByName.get(ref);
        if (site) pts.push(site.pos);
      }
      if (pushSegments(pts, col, width)) tendonCount++;
    }
    for (const fixed of childElements(tendonRoot, 'fixed')) {
      const col = fixed.getAttribute('rgba')
        ? parseNumbers(fixed.getAttribute('rgba'), defRgba) : [0.25, 0.85, 1.0, 1];
      const width = fixed.getAttribute('width')
        ? (Number(fixed.getAttribute('width')) || defWidth) : defWidth;
      const pts = [];
      for (const child of childElements(fixed, 'joint')) {
        const ref = child.getAttribute('joint');
        const pos = ref ? jointPositionsByName.get(ref) : null;
        if (pos) pts.push(pos);
      }
      if (pts.length === 1) {
        pts.push(pts[0].clone().add(new THREE.Vector3(0, 0, Math.max(width * 8, 0.04))));
      }
      if (pushSegments(pts, col, width)) tendonCount++;
    }
  }
  if (!positions.length) return null;
  return { positions, rgba, radii, tendonCount };
}

const MUSCLE_TUBE_AXIS = new THREE.Vector3(0, 1, 0);

// Build a THREE.Group of muscle tubes from computeMuscleLineData() data: each
// spatial-tendon segment becomes a small cylinder (radius = tendon width),
// merged into a single mesh (one draw call) with per-vertex RGBA so a tendon's
// alpha<1 renders transparent. Built fresh per view (source + USD) since a
// BufferGeometry can't be shared across two scenes with independent disposal.
function makeMuscleGroup(data) {
  if (!data || !data.radii.length) return null;
  const a = new THREE.Vector3();
  const b = new THREE.Vector3();
  const dir = new THREE.Vector3();
  const mid = new THREE.Vector3();
  const q = new THREE.Quaternion();
  const unitScale = new THREE.Vector3(1, 1, 1);
  const xform = new THREE.Matrix4();
  const segGeos = [];
  let anyTransparent = false;
  for (let s = 0; s < data.radii.length; s++) {
    a.set(data.positions[6 * s], data.positions[6 * s + 1], data.positions[6 * s + 2]);
    b.set(data.positions[6 * s + 3], data.positions[6 * s + 4], data.positions[6 * s + 5]);
    dir.subVectors(b, a);
    const len = dir.length();
    if (len < 1e-9) continue;
    const r = data.radii[s];
    const geo = new THREE.CylinderGeometry(r, r, len, 6, 1, false);
    q.setFromUnitVectors(MUSCLE_TUBE_AXIS, dir.divideScalar(len));
    mid.addVectors(a, b).multiplyScalar(0.5);
    geo.applyMatrix4(xform.compose(mid, q, unitScale));
    const cr = data.rgba[4 * s], cg = data.rgba[4 * s + 1];
    const cb = data.rgba[4 * s + 2], ca = data.rgba[4 * s + 3];
    if (ca < 1) anyTransparent = true;
    const n = geo.attributes.position.count;
    const colArr = new Float32Array(n * 4);
    for (let v = 0; v < n; v++) {
      colArr[4 * v] = cr; colArr[4 * v + 1] = cg;
      colArr[4 * v + 2] = cb; colArr[4 * v + 3] = ca;
    }
    geo.setAttribute('color', new THREE.BufferAttribute(colArr, 4));
    segGeos.push(geo);
  }
  if (!segGeos.length) return null;
  const merged = BufferGeometryUtils.mergeGeometries(segGeos, false);
  for (const g of segGeos) g.dispose();
  const material = new THREE.MeshStandardMaterial({
    vertexColors: true,
    roughness: 0.5,
    metalness: 0.0,
    transparent: anyTransparent,
    depthWrite: !anyTransparent
  });
  const mesh = new THREE.Mesh(merged, material);
  mesh.name = 'mjcf_tendons';
  const muscleGroup = new THREE.Group();
  muscleGroup.name = 'tendons';
  muscleGroup.add(mesh);
  muscleGroup.userData.tendonCount = data.tendonCount;
  return muscleGroup;
}

function makeSensorVisualData(sensors, sitesByName, bodiesByName, cameras) {
  const cameraByName = new Map((cameras || []).map((cam) => [cam.name, {
    matrix: matrixFromUSDArray(cam.matrix),
    fovy: Number.isFinite(cam.fovy) ? cam.fovy : 45
  }]));
  const resolveFrame = (type, name) => {
    if (!name) return null;
    if (type === 'camera' && cameraByName.has(name)) return cameraByName.get(name);
    if (type === 'site' && sitesByName.has(name)) return { matrix: sitesByName.get(name).matrix, fovy: 45 };
    if ((type === 'body' || type === 'xbody') && bodiesByName.has(name)) return { matrix: bodiesByName.get(name), fovy: 45 };
    return null;
  };

  const entries = [];
  for (const sensor of sensors || []) {
    const frame = resolveFrame(sensor.objtype, sensor.objname);
    if (!frame) continue;
    const canRender = ['camera', 'site', 'body', 'xbody'].includes(sensor.objtype)
      && ['rangefinder', 'camprojection', 'framepos', 'framequat', 'framexaxis',
          'frameyaxis', 'framezaxis', 'user'].includes(sensor.type);
    entries.push({
      name: sensor.name || `${sensor.type}_${entries.length}`,
      type: sensor.type || 'sensor',
      objtype: sensor.objtype || '',
      objname: sensor.objname || '',
      matrix: frame.matrix.clone(),
      fovy: frame.fovy || 45,
      canRender
    });
  }
  return entries.length ? entries : null;
}

function makeSensorHelperGroup(sensorData, { makePreviewCameras = false } = {}) {
  if (!sensorData?.length) return null;
  const group = new THREE.Group();
  group.name = 'mjc_sensors';
  const markerGeo = new THREE.SphereGeometry(0.018, 12, 8);
  const markerMat = new THREE.MeshStandardMaterial({
    color: 0x4ade80,
    emissive: 0x082a14,
    roughness: 0.45,
    depthTest: false
  });
  const rayMat = new THREE.LineBasicMaterial({ color: 0x4ade80, transparent: true, opacity: 0.9, depthTest: false });
  for (const entry of sensorData) {
    const marker = new THREE.Mesh(markerGeo, markerMat);
    marker.name = `sensor_${entry.name}`;
    applyMatrixToObject(marker, entry.matrix);
    marker.renderOrder = 40;
    group.add(marker);

    const localRay = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(0, 0, 0),
      new THREE.Vector3(0, 0, -0.25)
    ]);
    const ray = new THREE.Line(localRay, rayMat);
    ray.name = `sensor_ray_${entry.name}`;
    applyMatrixToObject(ray, entry.matrix);
    ray.renderOrder = 40;
    group.add(ray);

    if (entry.canRender) {
      const cam = new THREE.PerspectiveCamera(entry.fovy, 1, 0.01, 50);
      cam.name = `sensor_camera_${entry.name}`;
      applyMatrixToObject(cam, entry.matrix);
      group.add(cam);
      const helper = new THREE.CameraHelper(cam);
      helper.name = `sensor_frustum_${entry.name}`;
      helper.visible = state.settings.showSensors;
      group.add(helper);
      if (makePreviewCameras) {
        state.sensorPreviewCameras.push({ name: entry.name, type: entry.type, camera: cam });
      }
    }
  }
  group.traverse((obj) => { obj.visible = state.settings.showSensors; });
  return group;
}

let sensorRenderTarget = null;
function updateSensorPreviewPanel() {
  const active = state.sensorPreviewCameras[state.sensorPreviewIndex] || state.sensorPreviewCameras[0] || null;
  const visible = Boolean(active && state.settings.showSensors && state.settings.renderSensorView);
  sensorPreviewPanel.style.display = visible ? 'block' : 'none';
  if (active) {
    const suffix = state.sensorPreviewCameras.length > 1
      ? ` (${state.sensorPreviewIndex + 1}/${state.sensorPreviewCameras.length})`
      : '';
    sensorPreviewLabel.textContent = `${active.type}: ${active.name}${suffix}`;
  }
}

function renderSensorPreview() {
  const active = state.sensorPreviewCameras[state.sensorPreviewIndex] || state.sensorPreviewCameras[0] || null;
  if (!active || !state.settings.showSensors || !state.settings.renderSensorView) return;
  const w = sensorPreviewCanvas.width;
  const h = sensorPreviewCanvas.height;
  if (!sensorRenderTarget || sensorRenderTarget.width !== w || sensorRenderTarget.height !== h) {
    sensorRenderTarget?.dispose();
    sensorRenderTarget = new THREE.WebGLRenderTarget(w, h);
  }
  active.camera.aspect = w / h;
  active.camera.updateProjectionMatrix();
  sourceView.scene.updateMatrixWorld(true);
  renderer.setRenderTarget(sensorRenderTarget);
  renderer.setScissorTest(false);
  renderer.setViewport(0, 0, w, h);
  renderer.render(sourceView.scene, active.camera);
  const pixels = new Uint8Array(w * h * 4);
  renderer.readRenderTargetPixels(sensorRenderTarget, 0, 0, w, h, pixels);
  renderer.setRenderTarget(null);
  const ctx = sensorPreviewCanvas.getContext('2d');
  const image = ctx.createImageData(w, h);
  for (let y = 0; y < h; y++) {
    const src = (h - 1 - y) * w * 4;
    const dst = y * w * 4;
    image.data.set(pixels.subarray(src, src + w * 4), dst);
  }
  ctx.putImageData(image, 0, 0);
}

// MuJoCo boolean/flag attr: <flag x="enable|disable">, <compiler x="true|false">.
function mjcBoolAttr(v) { return v === 'true' || v === 'enable' || v === '1'; }

// MJCF <light>/<camera> -> exportPayload lights/cameras with a baked world
// matrix (-> UsdLux + GeomCamera in the converted USD).
function collectMujocoLightsCameras(worldbody) {
  const lights = [];
  const cameras = [];
  const visit = (node, world) => {
    for (const l of childElements(node, 'light')) {
      const m = new THREE.Matrix4().copy(world).multiply(matrixFromPoseAttrs(attrsFromElement(l)));
      const directional = mjcBoolAttr(l.getAttribute('directional') || 'false');
      const light = {
        name: l.getAttribute('name') || 'light',
        type: l.getAttribute('type') || (directional ? 'directional' : 'spot'),
        matrix: matrixToUSDArray(m),
        dir: parseNumbers(l.getAttribute('dir'), [0, 0, -1]).slice(0, 3)
      };
      const diffuse = parseNumbers(l.getAttribute('diffuse'), []);
      if (diffuse.length >= 3) light.color = diffuse.slice(0, 3);
      if (l.getAttribute('castshadow') !== null) light.castshadow = mjcBoolAttr(l.getAttribute('castshadow'));
      if (l.getAttribute('cutoff') !== null) light.cutoff = Number(l.getAttribute('cutoff'));
      lights.push(light);
    }
    for (const c of childElements(node, 'camera')) {
      const m = new THREE.Matrix4().copy(world).multiply(matrixFromPoseAttrs(attrsFromElement(c)));
      const cam = { name: c.getAttribute('name') || 'camera', matrix: matrixToUSDArray(m) };
      if (c.getAttribute('fovy') !== null) cam.fovy = Number(c.getAttribute('fovy'));
      if (c.getAttribute('projection') === 'orthographic'
          || (c.getAttribute('orthographic') !== null && mjcBoolAttr(c.getAttribute('orthographic')))) {
        cam.orthographic = true;
      }
      cameras.push(cam);
    }
    for (const b of childElements(node, 'body')) {
      visit(b, new THREE.Matrix4().copy(world).multiply(matrixFromPoseAttrs(attrsFromElement(b))));
    }
  };
  visit(worldbody, new THREE.Matrix4());
  return { lights, cameras };
}

// MJCF <option>/<option><flag>/<compiler> -> exportPayload.mjcScene, consumed by
// the C++ converter's ApplyMjcSceneOptions -> MjcSceneAPI (so the converted USD
// carries the full simulation options, not just timestep).
function parseMujocoSceneOptions(root) {
  const ms = {};
  const option = firstChildElement(root, 'option');
  if (option) {
    const opt = {};
    const num = (k) => { const v = option.getAttribute(k); if (v !== null) opt[k] = Number(v); };
    const inum = (k) => { const v = option.getAttribute(k); if (v !== null) opt[k] = Math.round(Number(v)); };
    const tok = (k) => { const v = option.getAttribute(k); if (v !== null) opt[k] = v; };
    const vec = (k) => { const v = parseNumbers(option.getAttribute(k), []); if (v.length) opt[k] = v; };
    ['timestep', 'impratio', 'density', 'viscosity', 'o_margin', 'tolerance',
     'ls_tolerance', 'noslip_tolerance', 'ccd_tolerance'].forEach(num);
    ['iterations', 'ls_iterations', 'noslip_iterations', 'ccd_iterations',
     'sdf_iterations', 'sdf_initpoints'].forEach(inum);
    ['integrator', 'cone', 'jacobian', 'solver'].forEach(tok);
    ['wind', 'magnetic', 'o_solref', 'o_solimp', 'o_friction'].forEach(vec);
    if (Object.keys(opt).length) ms.option = opt;
    const flag = firstChildElement(option, 'flag');
    if (flag) {
      const fl = {};
      ['constraint', 'equality', 'frictionloss', 'limit', 'contact', 'gravity',
       'clampctrl', 'warmstart', 'filterparent', 'actuation', 'refsafe', 'sensor',
       'midphase', 'nativeccd', 'eulerdamp', 'autoreset', 'island', 'override',
       'energy', 'fwdinv', 'invdiscrete', 'multiccd'].forEach((k) => {
        const v = flag.getAttribute(k); if (v !== null) fl[k] = mjcBoolAttr(v);
      });
      if (Object.keys(fl).length) ms.flag = fl;
    }
  }
  const compiler = firstChildElement(root, 'compiler');
  if (compiler) {
    const comp = {};
    const num = (k) => { const v = compiler.getAttribute(k); if (v !== null) comp[k] = Number(v); };
    const tok = (k) => { const v = compiler.getAttribute(k); if (v !== null) comp[k] = v; };
    const boolean = (k) => { const v = compiler.getAttribute(k); if (v !== null) comp[k] = mjcBoolAttr(v); };
    boolean('autolimits'); num('boundmass'); num('boundinertia'); num('settotalmass');
    boolean('usethread'); boolean('balanceinertia'); tok('angle'); boolean('fitaabb');
    boolean('fusestatic'); tok('inertiafromgeom'); boolean('alignfree'); boolean('saveinertial');
    const igr = parseNumbers(compiler.getAttribute('inertiagrouprange'), []);
    if (igr.length >= 2) {
      comp.inertiagrouprange_min = Math.round(igr[0]);
      comp.inertiagrouprange_max = Math.round(igr[1]);
    }
    if (Object.keys(comp).length) ms.compiler = comp;
  }
  return ms;
}

async function parseMJCFWithMeshes(xmlText, filename, baseDir = '') {
  const expanded = await expandMujocoIncludes(xmlText, baseDir);
  const doc = parseXMLDocument(expanded);
  const root = doc.documentElement;
  if (root.localName !== 'mujoco') {
    throw new Error(`Expected MJCF <mujoco> root, got <${root.localName || 'unknown'}>.`);
  }

  // Graft <attach> sub-models into the tree before any semantic parsing.
  await expandMujocoAttachments(root, baseDir);
  // Decode <asset><hfield> (PNG/inline) for <geom type="hfield"> tessellation.
  mjcfHFields = await collectMujocoHFields(root);

  // Honor <compiler angle="..." eulerseq="..."> for all orientation specifiers.
  const compilerEl = firstChildElement(root, 'compiler');
  const angleAttr = (compilerEl?.getAttribute('angle') || 'degree').toLowerCase();
  mjcfPoseCtx = {
    toRad: angleAttr === 'radian' ? 1 : Math.PI / 180,
    eulerseq: compilerEl?.getAttribute('eulerseq') || 'xyz'
  };

  // Dissolve <frame> grouping transforms (needs the angle ctx above).
  for (const worldbody of childElements(root, 'worldbody')) flattenMujocoFrames(worldbody);

  const meshAssets = collectMujocoAssets(root);
  const defaults = collectMujocoDefaults(root);
  // <asset><material name rgba emission>: name -> {rgba, emission}, to color
  // geoms that reference a material rather than a direct rgba (e.g. ms_human_700
  // bones, iit_softfoot's emissive "green pulleys").
  // <asset><texture> name -> definition (builtin checker/gradient or file).
  const textureDefs = new Map();
  for (const t of root.querySelectorAll('asset texture')) {
    const name = t.getAttribute('name') || t.getAttribute('file');
    if (!name) continue;
    textureDefs.set(name, {
      type: t.getAttribute('type') || '2d',
      builtin: t.getAttribute('builtin') || 'none',
      file: t.getAttribute('file') || '',
      rgb1: parseNumbers(t.getAttribute('rgb1'), [0.8, 0.8, 0.8]),
      rgb2: parseNumbers(t.getAttribute('rgb2'), [0.5, 0.5, 0.5]),
      markrgb: parseNumbers(t.getAttribute('markrgb'), [0, 0, 0]),
      mark: t.getAttribute('mark') || 'none',
      width: Number(t.getAttribute('width')) || 0,
      height: Number(t.getAttribute('height')) || 0
    });
  }
  const materialDefs = new Map();
  for (const m of root.querySelectorAll('asset material')) {
    const name = m.getAttribute('name');
    if (!name) continue;
    const rgba = parseNumbers(m.getAttribute('rgba'), []);
    const emission = Number(m.getAttribute('emission'));
    materialDefs.set(name, {
      rgba: rgba.length >= 3 ? rgba : null,
      emission: Number.isFinite(emission) ? emission : 0,
      texture: m.getAttribute('texture') || '',
      texrepeat: parseNumbers(m.getAttribute('texrepeat'), [1, 1])
    });
  }
  // A geom's effective display material {rgba, emission, texDef, texrepeat}: its
  // own `rgba` (no emission/texture), else its <material> (which may carry a
  // texture), else null (keep the loader's neutral default).
  const effectiveGeomRgba = (attrs) => {
    const direct = parseNumbers(attrs.rgba, []);
    if (direct.length >= 3) return { rgba: direct, emission: 0 };
    if (attrs.material && materialDefs.has(attrs.material)) {
      const d = materialDefs.get(attrs.material);
      const tex = d.texture && textureDefs.get(d.texture);
      if (d.rgba || tex) return { ...d, texDef: tex || null };
    }
    return null;
  };
  // Build (and cache) a THREE.Texture for a <texture> def: builtin checker/
  // gradient via canvas, or a file texture loaded from the asset folder.
  const textureCache = new Map();
  const getMujocoTexture = async (def) => {
    if (!def) return null;
    const key = def.file || `${def.builtin}:${def.rgb1.join()}:${def.rgb2.join()}`;
    if (textureCache.has(key)) return textureCache.get(key);
    let tex = null;
    if (def.file) {
      const file = resolveAssetFile(def.file);
      if (file) {
        try { tex = await new THREE.TextureLoader().loadAsync(objectURLForFile(file)); }
        catch (e) { console.warn(`MJCF texture load failed: ${def.file}`, e); }
      }
    } else if (def.builtin === 'checker') {
      tex = makeCheckerTexture(def);
    } else if (def.builtin === 'gradient') {
      tex = makeGradientTexture(def);
    }
    if (tex) { tex.wrapS = THREE.RepeatWrapping; tex.wrapT = THREE.RepeatWrapping; }
    textureCache.set(key, tex);
    return tex;
  };
  const actuators = buildMujocoActuators(root);
  const group = new THREE.Group();
  group.name = root.getAttribute('model') || filename.replace(/\.[^.]+$/, '') || 'mujoco_scene';
  group.links = {};
  group.joints = {};
  group.setJointValue = (jointName, value) => {
    const joint = group.joints[jointName];
    if (!joint?.pivot) return;
    // MuJoCo joint coordinate is qpos; the geometric displacement relative to
    // the XML rest configuration is (qpos - ref). At qpos == ref the body is in
    // its authored pose (zero displacement).
    const disp = value - (joint.refRad || 0);
    // Spherical (ball) and floating (free) joints are multi-DOF; a single slider
    // value can't drive them meaningfully, so leave them at their rest pose
    // instead of misapplying it as a 1-DOF hinge rotation.
    if (joint.jointType === 'spherical' || joint.jointType === 'floating') {
      return;
    }
    if (joint.jointType === 'prismatic') {
      const offset = new THREE.Vector3(...joint.axis)
        .normalize()
        .applyQuaternion(joint.originQuat)
        .multiplyScalar(disp);
      joint.pivot.position.copy(joint.origin).add(offset);
    } else {
      joint.pivot.quaternion.copy(joint.originQuat)
        .multiply(new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(...joint.axis).normalize(), disp));
    }
  };

  const links = [];
  const joints = [];
  let visualCount = 0;
  let collisionCount = 0;
  // Site/body/joint maps in model space for tendon routing and sensor frames.
  const sitesByName = new Map();
  const bodiesByName = new Map();
  const jointPositionsByName = new Map();
  const sitesPayload = [];
  state.muscleObjects = [];

  // Bake one <geom> into a link's THREE object + its payload. Shared by the
  // per-body traversal and the worldbody-level (static) geom collection.
  async function processGeomForLink(geomNode, linkObject, linkPayload, linkName, childClass, bodyWorldMatrix, geomIndex) {
    const geomAttrs = resolveMujocoAttrs(geomNode, defaults, 'geom', childClass);
    const geomName = geomAttrs.name || geomAttrs.mesh || `${linkName}_geom_${geomIndex}`;
    const isVisual = classifyMujocoGeom(geomAttrs);
    let payloads = null;
    if (!isVisual) {
      payloads = shapePayloadForMujocoGeom(
        geomAttrs,
        new THREE.Matrix4().copy(bodyWorldMatrix).multiply(matrixFromPoseAttrs(geomAttrs)),
        geomName
      );
    }
    if (!payloads) payloads = await mujocoGeomPayloads(geomAttrs, meshAssets, geomName, bodyWorldMatrix);

    const object = await loadMujocoGeomObject(geomAttrs, meshAssets, geomName);
    object.name = geomName;
    applyMujocoObjectDisplayTransform(object, geomAttrs, meshAssets);
    linkObject.add(object);
    const geomRgba = effectiveGeomRgba(geomAttrs);
    const geomGroup = mujocoGeomGroup(geomAttrs, isVisual);
    const geomIsCollider = isMujocoCollider(geomAttrs);
    object.traverse((obj) => {
      if (!obj.isMesh) return;
      obj.userData.urdfOwnerLink = linkObject;
      obj.userData.urdfOwnerLinkName = linkName;
      obj.userData.urdfCollision = !isVisual;
      obj.userData.mujocoGroup = geomGroup;
      if (isVisual) {
        if (geomRgba) applyRgbaToMesh(obj, geomRgba);
        state.visualMeshes.push(obj);
      } else if (!geomIsCollider) {
        if (geomRgba) applyRgbaToMesh(obj, geomRgba);
        state.collisionMeshes.push(obj);
      } else {
        obj.userData.originalMaterial = obj.material;
        obj.material = collisionMaterial;
        state.collisionMeshes.push(obj);
      }
    });

    // Apply a material's texture (builtin checker/gradient or file) to the
    // visual meshes — e.g. the iconic checkered ground plane, or robot skins.
    if (isVisual && geomRgba?.texDef) {
      const tex = await getMujocoTexture(geomRgba.texDef);
      if (tex) {
        const rep = geomRgba.texrepeat || [1, 1];
        object.traverse((obj) => {
          if (!obj.isMesh || !obj.material) return;
          const mat = obj.material.clone ? obj.material.clone() : obj.material;
          const t = tex.clone();
          t.needsUpdate = true;
          t.wrapS = THREE.RepeatWrapping;
          t.wrapT = THREE.RepeatWrapping;
          t.repeat.set(rep[0] || 1, rep[1] || 1);
          mat.map = t;
          if (mat.color) mat.color.setRGB(1, 1, 1);  // show the texture untinted
          mat.needsUpdate = true;
          obj.material = mat;
        });
      }
    }

    if (isVisual) {
      if (geomAttrs.material) {
        for (const p of payloads) p.material = geomAttrs.material;
      }
      linkPayload.visuals.push(...payloads);
      visualCount += payloads.length;
    } else {
      for (const payload of payloads) {
        payload.approximation = payload.approximation || 'convexHull';
      }
      linkPayload.collisions.push(...payloads);
      collisionCount += payloads.length;
    }
  }

  async function visitBody(
    bodyNode,
    parentObject,
    parentName = '',
    parentWorldMatrix = new THREE.Matrix4(),
    inheritedChildClass = ''
  ) {
    const bodyAttrs = attrsFromElement(bodyNode);
    const childClass = bodyAttrs.childclass || inheritedChildClass;
    const bodyLocalMatrix = matrixFromPoseAttrs(bodyAttrs);
    const bodyWorldMatrix = new THREE.Matrix4().copy(parentWorldMatrix).multiply(bodyLocalMatrix);
    const linkName = bodyAttrs.name || `body_${links.length}`;
    bodiesByName.set(linkName, bodyWorldMatrix.clone());
    // <freejoint>/<joint type="free"> -> 6-DOF floating base (not joined to its
    // parent); marked so the converter emits a free articulation root.
    const hasFree = !!firstChildElement(bodyNode, 'freejoint')
      || childElements(bodyNode, 'joint').some((j) => j.getAttribute('type') === 'free');
    const linkPayload = {
      name: linkName,
      inertial: parseMujocoInertial(bodyNode),
      visuals: [],
      collisions: []
    };
    if (hasFree) linkPayload.floating = true;
    if (bodyAttrs.mocap !== undefined && mjcBoolAttr(bodyAttrs.mocap)) {
      linkPayload.mocap = true;
    }

    const pivot = new THREE.Group();
    pivot.name = `${linkName}_joint`;
    applyMatrixToObject(pivot, bodyLocalMatrix);
    parentObject.add(pivot);

    const linkObject = new THREE.Group();
    linkObject.name = linkName;
    pivot.add(linkObject);
    group.links[linkName] = linkObject;

    if (parentName && !hasFree) {
      const jointNodes = childElements(bodyNode, 'joint');
      const jointNode = jointNodes[0] || null;
      // MuJoCo allows several <joint> per body (a composite/universal joint).
      // USD physics joints are pairwise, so we represent the first and warn that
      // the rest are dropped rather than silently losing DOFs.
      if (jointNodes.length > 1) {
        console.warn(
          `MJCF body "${linkName}" has ${jointNodes.length} joints; only the first `
          + `("${jointNodes[0].getAttribute('name') || '?'}") is converted — the `
          + `remaining ${jointNodes.length - 1} DOF(s) are dropped.`
        );
        setStatus(`Note: body "${linkName}" has ${jointNodes.length} joints; extras dropped.`);
      }
      const jointAttrs = jointNode ? resolveMujocoAttrs(jointNode, defaults, 'joint', childClass) : {};
      const axis = parseNumbers(jointAttrs.axis, [0, 0, 1]);
      const range = parseNumbers(jointAttrs.range, []);
      const jointName = jointAttrs.name || `${parentName}_to_${linkName}${jointNode ? '' : '_fixed'}`;
      const rawType = jointNode ? (jointAttrs.type || 'hinge') : 'fixed';
      const type = jointNode ? mujocoJointType(jointAttrs.type || 'hinge') : 'fixed';
      // Per-joint DOF count and reference offset for keyframe (qpos) mapping.
      // ball=4 (quat), free=7, hinge/slide=1, fixed/weld=0. `ref` is the qpos at
      // which the joint matches its XML configuration; rotation = qpos - ref.
      const dofCount = rawType === 'ball' ? 4 : rawType === 'free' ? 7 : rawType === 'fixed' ? 0 : 1;
      const refVal = numberAttr(jointAttrs, 'ref');
      const refRad = Number.isFinite(refVal)
        ? (rawType === 'slide' ? refVal : refVal * mjcfPoseCtx.toRad)
        : 0;
      const bodyPose = decomposeMatrix(bodyLocalMatrix);
      const jointPos = parseNumbers(jointAttrs.pos, [0, 0, 0]);
      const localPos0 = transformPointArray(bodyLocalMatrix, jointPos);
      const localPos1 = jointPos;
      const jointWorldPos = vectorFromArray(jointPos).applyMatrix4(bodyWorldMatrix);
      jointPositionsByName.set(jointName, jointWorldPos);
      const localRot0 = quaternionToUSDArray(bodyPose.quaternion);
      const localRot1 = [1, 0, 0, 0];
      const jointInfo = {
        name: jointName,
        type,
        rawType,
        dofCount,
        refRad,
        parent: parentName,
        child: linkName,
        axis,
        axisToken: axisToToken(axis),
        origin: localPos0,
        originMatrix: matrixToUSDArray(matrixFromPoseAttrs(bodyAttrs)),
        localPos0,
        localPos1,
        localRot0,
        localRot1,
        // Range in joint-coordinate units (radians for hinge). MuJoCo `range` is
        // in the model's angle unit, so convert degrees -> radians; slide is
        // meters. The USD converter expects revolute limits in radians.
        limit: range.length >= 2
          ? (rawType === 'slide'
              ? { lower: range[0], upper: range[1] }
              : { lower: range[0] * mjcfPoseCtx.toRad, upper: range[1] * mjcfPoseCtx.toRad })
          : {},
        uiLimit: range.length >= 2
          ? (rawType === 'slide'
              ? { lower: range[0], upper: range[1] }
              : { lower: range[0] * mjcfPoseCtx.toRad, upper: range[1] * mjcfPoseCtx.toRad })
          : null,
        dynamics: {
          damping: numberAttr(jointAttrs, 'damping'),
          friction: numberAttr(jointAttrs, 'frictionloss')
        }
      };
      joints.push(jointInfo);
      group.joints[jointName] = {
        ...jointInfo,
        jointType: type,
        pivot,
        origin: pivot.position.clone(),
        originQuat: pivot.quaternion.clone()
      };
    }

    let geomIndex = 0;
    for (const geomNode of childElements(bodyNode, 'geom')) {
      await processGeomForLink(geomNode, linkObject, linkPayload, linkName, childClass, bodyWorldMatrix, geomIndex);
      geomIndex++;
    }

    links.push(linkPayload);

    // Collect <site> world positions for muscle/spatial-tendon routing. Sites
    // are body-local frames; bake to model space via the accumulated
    // bodyWorldMatrix (matching how the merged muscle lines are positioned).
    for (const siteNode of childElements(bodyNode, 'site')) {
      const siteAttrs = attrsFromElement(siteNode);
      const siteName = siteAttrs.name;
      if (!siteName) continue;
      const siteLocal = matrixFromPoseAttrs(siteAttrs);
      const siteWorld = new THREE.Matrix4().copy(bodyWorldMatrix).multiply(siteLocal);
      sitesByName.set(siteName, {
        pos: new THREE.Vector3().setFromMatrixPosition(siteWorld),
        matrix: siteWorld.clone()
      });
      const sitePayload = { name: siteName, matrix: matrixToUSDArray(siteWorld) };
      if (siteAttrs.group !== undefined) sitePayload.group = parseInt(siteAttrs.group, 10) || 0;
      const size = parseNumbers(siteAttrs.size, []);
      if (size.length) sitePayload.size = size[0];
      sitesPayload.push(sitePayload);
    }

    for (const childBody of childElements(bodyNode, 'body')) {
      await visitBody(childBody, linkObject, linkName, bodyWorldMatrix, childClass);
    }
  }

  for (const worldbody of childElements(root, 'worldbody')) {
    for (const bodyNode of childElements(worldbody, 'body')) {
      await visitBody(bodyNode, group);
    }
  }

  // World-fixed geoms directly under <worldbody> (floor/ground/obstacles) belong
  // to no body; collect them onto a single static "world" link group so scenes
  // show their ground. Mirrors the CLI's AddWorldbodyGeomsLink.
  const worldGroup = new THREE.Group();
  worldGroup.name = 'world';
  const worldLinkPayload = { name: 'world', static: true, inertial: { mass: 0 }, visuals: [], collisions: [] };
  let worldGeomIndex = 0;
  for (const worldbody of childElements(root, 'worldbody')) {
    for (const geomNode of childElements(worldbody, 'geom')) {
      await processGeomForLink(geomNode, worldGroup, worldLinkPayload, 'world', '', new THREE.Matrix4(), worldGeomIndex);
      worldGeomIndex++;
    }
  }
  if (worldGroup.children.length) group.add(worldGroup);
  if (worldLinkPayload.visuals.length || worldLinkPayload.collisions.length) links.push(worldLinkPayload);

  // Muscle / spatial-tendon visualization (e.g. ms_human_700, iit_softfoot).
  // Drawn as polylines through the routing sites, in model space, added to the
  // model root so it rides the same up-axis orientation as the body meshes. The
  // line data is stashed in state so the converted-USD view can draw the same
  // muscles (it shares the model coordinate space).
  let muscleTendonCount = 0;
  const muscleData = computeMuscleLineData(root, sitesByName, jointPositionsByName);
  state.muscleLineData = muscleData;
  const muscleGroup = makeMuscleGroup(muscleData);
  if (muscleGroup) {
    muscleGroup.visible = state.settings.showMuscles && state.settings.showTendons;
    group.add(muscleGroup);
    state.muscleObjects.push(muscleGroup);
    muscleTendonCount = muscleGroup.userData.tendonCount || 0;
  }

  // Parse the model's "home" keyframe (qpos) so the robot can be posed in its
  // natural configuration instead of the MuJoCo zero-pose. qpos layout: a
  // floating base contributes 7 leading values (pos3 + quat4), then one value
  // per single-DOF joint in body-tree (DFS) order — matching how `joints` is
  // built above. qpos angles are radians regardless of <compiler angle>.
  const keyEl = root.querySelector('keyframe key');
  if (keyEl?.getAttribute('qpos')) {
    const hasFree = !!root.querySelector('worldbody freejoint, worldbody joint[type="free"]');
    group.homeKeyframe = {
      qpos: parseNumbers(keyEl.getAttribute('qpos'), []),
      baseDofs: hasFree ? 7 : 0,
      // Joints in body-tree (DFS) order = MuJoCo qpos order. Each descriptor
      // carries its DOF count so multi-DOF joints (ball=4) advance the cursor
      // correctly and don't shift every subsequent joint's value.
      jointOrder: joints.map((j) => ({
        name: j.name, rawType: j.rawType, dofCount: j.dofCount, refRad: j.refRad
      }))
    };
  }

  const mjcSceneOptions = parseMujocoSceneOptions(root);
  const tendonsPayload = parseMujocoTendons(root);
  const mjcActuatorsPayload = parseMujocoMJCActuators(root);
  // <texture name file=...> -> file path, and built-in checker/gradient textures
  // -> generated PNG paths. A material's texture then becomes a UsdUVTexture in
  // the converted USD.
  const texFiles = new Map();
  for (const t of root.querySelectorAll('asset texture')) {
    const tn = t.getAttribute('name'); const tf = t.getAttribute('file');
    if (tn && tf) texFiles.set(tn, resolveAssetEntry(tf)?.rel || tf);
  }
  const materialsPayload = [];
  for (const m of root.querySelectorAll('asset material')) {
    if (!m.getAttribute('name')) continue;
    const mat = { name: m.getAttribute('name') };
    const rgba = parseNumbers(m.getAttribute('rgba'), []);
    if (rgba.length >= 4) mat.rgba = rgba.slice(0, 4);
    for (const k of ['metallic', 'roughness', 'specular', 'emission', 'reflectance']) {
      const v = m.getAttribute(k);
      if (v !== null) mat[k] = Number(v);
    }
    const texRef = m.getAttribute('texture');
    const texrepeat = parseNumbers(m.getAttribute('texrepeat'), [1, 1]);
    if (texRef && texFiles.has(texRef)) mat.texture = texFiles.get(texRef);
    else if (texRef && textureDefs.has(texRef)) {
      const generated = await generatedMujocoTextureAsset(texRef, textureDefs.get(texRef), texrepeat);
      if (generated) mat.texture = generated;
    }
    materialsPayload.push(mat);
  }
  const sensorsPayload = [];
  const sensorRootEl = firstChildElement(root, 'sensor');
  if (sensorRootEl) {
    for (const node of childElements(sensorRootEl)) {
      const type = node.localName;
      if (!type) continue;
      const s = { type, name: node.getAttribute('name') || `${type}_${sensorsPayload.length}` };
      const targetAliases = ['site', 'joint', 'actuator', 'tendon', 'body', 'xbody',
        'geom', 'camera', 'light', 'sensor', 'numeric', 'text', 'tuple', 'key',
        'plugin'];
      if (node.getAttribute('objtype') !== null) {
        s.objtype = node.getAttribute('objtype');
        if (node.getAttribute('objname') !== null) s.objname = node.getAttribute('objname');
      } else {
        for (const k of targetAliases) {
          if (node.getAttribute(k) !== null) { s.objtype = k; s.objname = node.getAttribute(k); break; }
        }
      }
      if (node.getAttribute('reftype') !== null) {
        s.reftype = node.getAttribute('reftype');
        if (node.getAttribute('refname') !== null) s.refname = node.getAttribute('refname');
      } else {
        for (const k of targetAliases) {
          const refKey = `ref${k}`;
          const camelRefKey = `ref${k[0].toUpperCase()}${k.slice(1)}`;
          if (node.getAttribute(refKey) !== null) {
            s.reftype = k;
            s.refname = node.getAttribute(refKey);
            break;
          }
          if (node.getAttribute(camelRefKey) !== null) {
            s.reftype = k;
            s.refname = node.getAttribute(camelRefKey);
            break;
          }
        }
      }
      if (node.getAttribute('group') !== null) s.group = Math.round(Number(node.getAttribute('group')));
      if (node.getAttribute('cutoff') !== null) s.cutoff = Number(node.getAttribute('cutoff'));
      if (node.getAttribute('noise') !== null) s.noise = Number(node.getAttribute('noise'));
      const user = parseNumbers(node.getAttribute('user'), []);
      if (user.length) s.user = user;
      sensorsPayload.push(s);
    }
  }
  const contactPairsPayload = [];
  for (const node of root.querySelectorAll('contact pair')) {
    if (!node.getAttribute('geom1') || !node.getAttribute('geom2')) continue;
    const pr = { name: node.getAttribute('name') || `pair_${contactPairsPayload.length}`,
                 geom1: node.getAttribute('geom1'), geom2: node.getAttribute('geom2') };
    if (node.getAttribute('condim') !== null) pr.condim = Math.round(Number(node.getAttribute('condim')));
    for (const k of ['margin', 'gap']) {
      if (node.getAttribute(k) !== null) pr[k] = Number(node.getAttribute(k));
    }
    for (const k of ['friction', 'solref', 'solimp']) {
      const a = parseNumbers(node.getAttribute(k), []);
      if (a.length) pr[k] = a;
    }
    contactPairsPayload.push(pr);
  }
  const customPayload = {};
  const customRootEl = firstChildElement(root, 'custom');
  if (customRootEl) {
    const numeric = [];
    for (const n of childElements(customRootEl, 'numeric')) {
      if (!n.getAttribute('name')) continue;
      numeric.push({ name: n.getAttribute('name'), data: parseNumbers(n.getAttribute('data'), []) });
    }
    const text = [];
    for (const t of childElements(customRootEl, 'text')) {
      if (!t.getAttribute('name')) continue;
      text.push({ name: t.getAttribute('name'), data: t.getAttribute('data') || '' });
    }
    if (numeric.length) customPayload.numeric = numeric;
    if (text.length) customPayload.text = text;
  }
  // <extension><plugin plugin="..."><instance name="..."><config key= value=>
  // -> plugin-instance declarations (engine plugins, e.g. shadow_dexee PID).
  const pluginsPayload = [];
  const extensionEl = firstChildElement(root, 'extension');
  if (extensionEl) {
    for (const pluginNode of childElements(extensionEl, 'plugin')) {
      const pluginId = pluginNode.getAttribute('plugin') || '';
      for (const inst of childElements(pluginNode, 'instance')) {
        const instName = inst.getAttribute('name') || '';
        if (!instName) continue;
        const p = { instance: instName };
        if (pluginId) p.plugin = pluginId;
        const config = {};
        for (const cfg of childElements(inst, 'config')) {
          const key = cfg.getAttribute('key') || '';
          if (!key) continue;
          config[key] = cfg.getAttribute('value') || '';
        }
        if (Object.keys(config).length) p.config = config;
        pluginsPayload.push(p);
      }
    }
  }
  // MuJoCo merges every <worldbody> block (the local one plus any pulled in via
  // <include>); collect lights/cameras from them all, not just the first.
  const lightsPayload = [];
  const camerasPayload = [];
  for (const worldbodyEl of childElements(root, 'worldbody')) {
    const lc = collectMujocoLightsCameras(worldbodyEl);
    lightsPayload.push(...lc.lights);
    camerasPayload.push(...lc.cameras);
  }
  state.sensorPreviewCameras = [];
  const sensorVisualData = makeSensorVisualData(sensorsPayload, sitesByName, bodiesByName, camerasPayload);
  state.sensorVisualData = sensorVisualData;
  const sensorHelpers = makeSensorHelperGroup(sensorVisualData, { makePreviewCameras: true });
  if (sensorHelpers) {
    sensorHelpers.visible = state.settings.showSensors;
    group.add(sensorHelpers);
    state.sensorObjects.push(sensorHelpers);
  }
  updateSensorPreviewPanel();
  // All <keyframe><key> -> payload keyframes (-> MjcKeyframe prims). Separate
  // from group.homeKeyframe above, which is only used to pose the source view.
  const keyframesPayload = [];
  for (const key of root.querySelectorAll('keyframe key')) {
    const k = { name: key.getAttribute('name') || `key_${keyframesPayload.length}` };
    for (const a of ['qpos', 'qvel', 'act', 'ctrl', 'mpos', 'mquat']) {
      const v = parseNumbers(key.getAttribute(a), []);
      if (v.length) k[a] = v;
    }
    keyframesPayload.push(k);
  }
  state.exportPayload = {
    name: group.name,
    upAxis: state.settings.upAxis,
    gravity: state.settings.upAxis === 'Z' ? [0, 0, -1] : [0, -1, 0],
    timestep: numberAttr(firstChildElement(root, 'option'), 'timestep'),
    links,
    joints,
    actuators,
    ...(Object.keys(mjcSceneOptions).length ? { mjcScene: mjcSceneOptions } : {}),
    ...(sitesPayload.length ? { sites: sitesPayload } : {}),
    ...(tendonsPayload.length ? { tendons: tendonsPayload } : {}),
    ...(mjcActuatorsPayload.length ? { mjcActuators: mjcActuatorsPayload } : {}),
    ...(keyframesPayload.length ? { keyframes: keyframesPayload } : {}),
    ...(lightsPayload.length ? { lights: lightsPayload } : {}),
    ...(camerasPayload.length ? { cameras: camerasPayload } : {}),
    ...(materialsPayload.length ? { materials: materialsPayload } : {}),
    ...(sensorsPayload.length ? { sensors: sensorsPayload } : {}),
    ...(contactPairsPayload.length ? { contactPairs: contactPairsPayload } : {}),
    ...(Object.keys(customPayload).length ? { custom: customPayload } : {}),
    ...(pluginsPayload.length ? { plugins: pluginsPayload } : {})
  };
  group.userData.stats = {
    links: links.length,
    joints: joints.length,
    visuals: visualCount,
    collisions: collisionCount,
    actuators: actuators.length,
    sites: sitesByName.size,
    muscles: muscleTendonCount
  };
  return group;
}

// Pose a parsed MJCF robot to the MuJoCo default qpos0 = ref (XML rest pose).
// Slider value shows ref; geometric displacement (qpos - ref) is zero.
function poseToRest(robot) {
  if (!robot?.joints) return;
  for (const [name, joint] of Object.entries(robot.joints)) {
    const ref = joint.refRad || 0;
    state.jointValues[name] = ref;
    if (joint.rawType === 'ball') {
      if (joint.pivot && joint.originQuat) joint.pivot.quaternion.copy(joint.originQuat);
    } else if (typeof robot.setJointValue === 'function') {
      robot.setJointValue(name, ref);   // displacement = ref - ref = 0
    }
  }
}

// Pose to the model's "home" keyframe. qpos angles are radians; setJointValue
// applies the (qpos - ref) displacement, so the slider value shown is the raw
// qpos coordinate (matching the MuJoCo viewer). DOF-count aware: ball=4 (quat),
// free=7, hinge/slide=1, so multi-DOF joints don't shift later joints' values.
function poseFromKeyframe(robot) {
  const kf = robot?.homeKeyframe;
  if (!kf) { poseToRest(robot); return; }
  const qpos = kf.qpos;
  let cursor = kf.baseDofs;   // skip the floating-base DOFs (pos3 + quat4)
  for (const j of kf.jointOrder) {
    const dof = j.dofCount;
    if (dof === 0) continue;                 // fixed/welded body: no qpos
    if (cursor + dof > qpos.length) break;
    if (j.rawType === 'ball') {
      const jc = robot.joints[j.name];
      if (jc?.pivot) {
        const bq = new THREE.Quaternion(qpos[cursor + 1], qpos[cursor + 2], qpos[cursor + 3], qpos[cursor]).normalize();
        jc.pivot.quaternion.copy(jc.originQuat).multiply(bq);
      }
    } else if (j.rawType !== 'free' && typeof robot.setJointValue === 'function') {
      const qv = qpos[cursor];               // joint coordinate (qpos)
      state.jointValues[j.name] = qv;
      robot.setJointValue(j.name, qv);        // displacement = qv - ref
    }
    cursor += dof;
  }
}

// Apply the active pose preset (home keyframe vs. qpos0=ref) and resync views.
function applyPosePreset() {
  const robot = state.robot;
  if (!robot?.joints) return;
  if (state.settings.applyHomePose && robot.homeKeyframe) poseFromKeyframe(robot);
  else poseToRest(robot);
  syncUSDArticulationToJointValues();
  rebuildJointControls();
  syncConvertedUSDToSourcePose();
  syncGhosts();
}

function syncUSDArticulationToJointValues() {
  if (!state.usdArticulation?.setJointValue || state.usdArticulation === state.robot) return;
  for (const [name, value] of Object.entries(state.jointValues)) {
    state.usdArticulation.setJointValue(name, value);
  }
}

function classifyRobotMeshes(robot) {
  state.visualMeshes = [];
  state.collisionMeshes = [];

  const linkEntries = Object.entries(robot.links || {});
  const linkSet = new Set(linkEntries.map(([, link]) => link));
  const linkNames = new Map(linkEntries.map(([name, link]) => [link, name]));
  robot.traverse((obj) => {
    if (!obj.isMesh) return;
    let cursor = obj;
    let ownerLink = null;
    let isCollision = false;
    // Walk up to the owning link. Check `robot` itself too: urdf-loader makes
    // the ROOT link node the URDFRobot root (robot.links[root] === robot), so
    // stopping at `cursor !== robot` would orphan the root link's meshes (they
    // then get dropped from the export payload).
    while (cursor) {
      const marker = `${cursor.type || ''} ${cursor.name || ''}`.toLowerCase();
      if (cursor.isURDFCollider || cursor.isURDFCollision || marker.includes('collision') || marker.includes('collider')) {
        isCollision = true;
      }
      if (linkSet.has(cursor)) {
        ownerLink = cursor;
        break;
      }
      if (cursor === robot) break;
      cursor = cursor.parent;
    }
    obj.userData.urdfOwnerLink = ownerLink;
    obj.userData.urdfOwnerLinkName = ownerLink ? linkNames.get(ownerLink) : '';
    obj.userData.urdfCollision = isCollision;
    if (isCollision) {
      obj.userData.originalMaterial = obj.material;
      obj.material = collisionMaterial;
      state.collisionMeshes.push(obj);
    } else {
      state.visualMeshes.push(obj);
    }
  });

  applyVisibility();
}

// A geom mesh's visibility: visual MuJoCo-tagged source geoms follow their
// group toggle. Collision meshes follow the coarse collision toggle so enabling
// "Collision meshes" reliably shows colliders even when MuJoCo groups 3-5 are
// hidden by default.
function geomMeshVisible(mesh, isVisualList) {
  const g = mesh.userData?.mujocoGroup;
  if (isVisualList) {
    if (g !== undefined && g !== null && state.settings.geomGroups[g] === false) return false;
    return state.settings.showVisuals;
  }
  return state.settings.showCollisions;
}

function applyVisibility() {
  for (const mesh of state.visualMeshes) mesh.visible = geomMeshVisible(mesh, true);
  for (const mesh of state.collisionMeshes) mesh.visible = geomMeshVisible(mesh, false);
  applyMuscleVisibility();
  applyRenderableVisibility(state.usdObject);
  applyRenderableVisibility(state.usdGhost);
  applyRenderableVisibility(state.sourceGhost);
}

function applyMuscleVisibility() {
  const tendonVisible = state.settings.showMuscles && state.settings.showTendons;
  for (const obj of state.muscleObjects) obj.visible = tendonVisible;
  for (const obj of state.usdMuscleObjects) obj.visible = tendonVisible;
  for (const obj of state.sensorObjects) obj.visible = state.settings.showSensors;
  for (const obj of state.usdSensorObjects) obj.visible = state.settings.showSensors;
  updateSensorPreviewPanel();
}

function currentFitObjects() {
  return [state.robot, state.usdObject].filter(Boolean);
}

function applyRenderableVisibility(root) {
  root?.traverse((obj) => {
    if (!obj.isMesh) return;
    obj.visible = isCollisionObject(obj)
      ? state.settings.showCollisions
      : state.settings.showVisuals;
  });
}

function fitCamera(rootOrObjects) {
  const objects = Array.isArray(rootOrObjects) ? rootOrObjects : [rootOrObjects].filter(Boolean);
  const box = new THREE.Box3();
  for (const object of objects) {
    if (object) { object.updateWorldMatrix(true, true); box.expandByObject(object); }
  }
  if (box.isEmpty()) return;
  const size = box.getSize(new THREE.Vector3());
  const center = box.getCenter(new THREE.Vector3());
  const radius = Math.max(size.length() * 0.5, 1e-3);
  // Distance so the bounding sphere fits BOTH the vertical and horizontal
  // frustum of a single split view (each view is ~half the window width, so the
  // horizontal FOV is the tighter constraint), plus a safety margin.
  const fovV = (camera.fov * Math.PI) / 180;
  const aspect = camera.aspect || 1;
  const fovH = 2 * Math.atan(Math.tan(fovV / 2) * aspect);
  const dist = (radius / Math.sin(Math.max(Math.min(fovV, fovH) / 2, 1e-3))) * 1.15;
  const dir = new THREE.Vector3(1.1, 0.75, 1.35).normalize();
  controls.target.copy(center);
  camera.position.copy(center).add(dir.multiplyScalar(dist));
  camera.near = Math.max(dist / 1000, 1e-4);
  camera.far = dist * 100;
  camera.updateProjectionMatrix();
  controls.update();
}

function fitCurrentView() {
  fitCamera(currentFitObjects());
}

function applySceneOrientation() {
  // The source robot is built directly from URDF/MJCF, which are always Z-up,
  // so it always needs the -90deg X display rotation to stand upright in
  // three.js (Y-up).
  for (const root of [sourceView.root, sourceView.ghostRoot]) {
    root.rotation.set(0, 0, 0);
    root.rotation.x = -Math.PI / 2;
  }
  // The USD view shows the actual loaded USD (converted or imported). A Z-up USD
  // needs the same -90deg X display rotation; a Y-up USD already carries the
  // converter's /World rotateX (applied by the loader and preserved by
  // buildArticulatedRobotFromUSD), so it is already upright.
  for (const root of [usdView.root, usdView.ghostRoot]) {
    root.rotation.set(0, 0, 0);
    if (state.settings.upAxis === 'Z') {
      root.rotation.x = -Math.PI / 2;
    }
  }
}

function applyAxisHelperVisibility() {
  sourceView.axisHelper.visible = state.settings.showAxisHelper;
  usdView.axisHelper.visible = state.settings.showAxisHelper;
}

function buildGUI() {
  const gui = new GUI({ title: 'Robot Controls' });
  gui.add(state.settings, 'upAxis', ['Z', 'Y']).name('Export upAxis').onChange(() => {
    // If a converted USD is on screen, re-export with the new up axis so the
    // USD view stays in sync (convertSourceToUSD re-applies orientation + fit).
    if (state.robot && state.usdIsConverted) {
      convertSourceToUSD(state.latestUSDFormat || 'usdc').catch((err) => {
        console.error(err);
        setStatus(`Convert to USD failed: ${err.message}`);
      });
      return;
    }
    applySceneOrientation();
    if ((state.robot || state.usdObject) && state.settings.autocenter) fitCamera(currentFitObjects());
  });
  gui.add(state.settings, 'showVisuals').name('Visual meshes').onChange(applyVisibility);
  gui.add(state.settings, 'showCollisions').name('Collision meshes').onChange(applyVisibility);
  gui.add(state.settings, 'showMuscles').name('Muscles').onChange(applyMuscleVisibility);
  gui.add(state.settings, 'showTendons').name('Tendons').onChange(applyMuscleVisibility);
  gui.add(state.settings, 'showSensors').name('Sensors').onChange(applyMuscleVisibility);
  gui.add(state.settings, 'renderSensorView').name('Sensor view').onChange(updateSensorPreviewPanel);
  // Per-MuJoCo-geom-group visibility (mirrors the MuJoCo viewer's group toggles).
  // Applies to MJCF source geoms tagged with a group; e.g. enable Group 4 to see
  // iit_softfoot's virtual-pulley tubes. Spatial tendons (the yellow streamlines)
  // are a separate entity controlled by 'Muscles', not by group.
  const groupFolder = gui.addFolder('Geom groups (source)');
  for (let g = 0; g <= 5; g++) {
    groupFolder.add(state.settings.geomGroups, String(g)).name(`Group ${g}`).onChange(applyVisibility);
  }
  groupFolder.close();
  gui.add(state.settings, 'ignoreJointLimits').name('Ignore limits').onChange(rebuildJointControls);
  gui.add(state.settings, 'hideFixedJoints').name('Hide fixed joints').onChange(rebuildJointControls);
  gui.add(state.settings, 'applyHomePose').name('Home pose').onChange(() => {
    applyPosePreset();
    if (state.robot && state.settings.autocenter) fitCamera(currentFitObjects());
  });
  gui.add(state.settings, 'animateJoints').name('Animate joints');
  gui.add(state.settings, 'animationSpeed', 0.1, 4.0, 0.1).name('Animation speed');
  gui.add(state.settings, 'autocenter').name('Autocenter');
  gui.add(state.settings, 'showAxisHelper').name('Axis helper').onChange(applyAxisHelperVisibility);
  gui.add(state.settings, 'showLinkLines').name('Link lines').onChange(updateSkeletonDebugVisualizations);
  gui.add(state.settings, 'showJointArrows').name('Joint arrows').onChange(updateSkeletonDebugVisualizations);
  gui.add(state.settings, 'showLinkNames').name('Link names').onChange(updateSkeletonDebugVisualizations);
  gui.add(state.settings, 'showJointNames').name('Joint names').onChange(updateSkeletonDebugVisualizations);
  gui.add(state.settings, 'labelScale', 0.25, 6.0, 0.05).name('Label scale').onChange(updateSkeletonDebugVisualizations);
  gui.add(state.settings, 'ghostUSDInSource').name('USD ghost on left').onChange(rebuildGhosts);
  gui.add(state.settings, 'ghostSourceInUSD').name('Source ghost on right').onChange(rebuildGhosts);
  gui.add(state.settings, 'ghostOpacity', 0.05, 0.75, 0.01).name('Ghost opacity').onChange(() => {
    setGhostOpacity(state.usdGhost, state.settings.ghostOpacity);
    setGhostOpacity(state.sourceGhost, state.settings.ghostOpacity);
  });
}

function setJointValue(jointName, value) {
  const joint = state.joints[jointName];
  if (!joint) return;
  state.jointValues[jointName] = value;
  const controls = state.jointControls.get(jointName);
  if (controls) {
    controls.number.value = value.toFixed(3);
    controls.range.value = String(value);
  }
  if (state.robot?.setJointValue) {
    state.robot.setJointValue(jointName, value);
  } else if (joint.setJointValue) {
    joint.setJointValue(value);
  }
  if (state.usdArticulation?.setJointValue && state.usdArticulation !== state.robot) {
    state.usdArticulation.setJointValue(jointName, value);
  }
  syncConvertedUSDToSourcePose();
  syncGhosts();
}

function jointLimits(joint) {
  const type = joint.jointType || joint.type || 'fixed';
  const limit = joint.uiLimit || joint.limit || {};
  if (type === 'continuous') return [-Math.PI, Math.PI, 0.01];
  if (type === 'prismatic') {
    return state.settings.ignoreJointLimits
      ? [-1, 1, 0.001]
      : [Number.isFinite(limit.lower) ? limit.lower : -1, Number.isFinite(limit.upper) ? limit.upper : 1, 0.001];
  }
  return state.settings.ignoreJointLimits
    ? [-Math.PI, Math.PI, 0.01]
    : [Number.isFinite(limit.lower) ? limit.lower : -Math.PI, Number.isFinite(limit.upper) ? limit.upper : Math.PI, 0.01];
}

function rebuildJointControls() {
  jointControlsEl.innerHTML = '';
  state.jointControls.clear();
  const jointSource = state.robot || state.usdArticulation;
  if (!jointSource) return;
  state.joints = jointSource.joints || {};
  const jointEntries = Object.entries(state.joints);
  for (const [name, joint] of jointEntries) {
    const type = joint.jointType || joint.type || 'fixed';
    if (state.settings.hideFixedJoints && type === 'fixed') continue;

    const [min, max, step] = jointLimits(joint);
    const value = state.jointValues[name] ?? 0;
    const row = document.createElement('div');
    row.className = 'joint-row';
    row.innerHTML = `
      <div>
        <div class="joint-name" title="${escapeXML(name)}">${escapeXML(name)}</div>
        <div class="joint-type">${escapeXML(type)}</div>
      </div>
      <input type="number" min="${min}" max="${max}" step="${step}" value="${value.toFixed(3)}">
      <input type="range" min="${min}" max="${max}" step="${step}" value="${value}">
    `;
    const number = row.querySelector('input[type="number"]');
    const range = row.querySelector('input[type="range"]');
    const onInput = (event) => {
      const next = Number(event.target.value);
      if (!Number.isFinite(next)) return;
      number.value = next.toFixed(3);
      range.value = String(next);
      setJointValue(name, next);
    };
    number.addEventListener('input', onInput);
    range.addEventListener('input', onInput);
    state.jointControls.set(name, { number, range });
    jointControlsEl.appendChild(row);
  }
}

function updateRobotInfo(metadata) {
  document.getElementById('robotName').textContent = metadata.name || state.robot?.name || '-';
  document.getElementById('linkCount').textContent = String(metadata.links?.size || Object.keys(state.robot?.links || state.usdArticulation?.links || {}).length);
  document.getElementById('jointCount').textContent = String(metadata.joints?.length || Object.keys(state.robot?.joints || state.usdArticulation?.joints || {}).length);
  document.getElementById('meshCount').textContent = String(metadata.meshes ?? (state.visualMeshes.length + state.collisionMeshes.length));
}

function updateLabels() {
  const sourceName = state.inputName || 'URDF/MJCF';
  const sourceFormat = state.inputFormat ? state.inputFormat.toUpperCase() : 'URDF/MJCF';
  sourceLabelEl.textContent = `${sourceFormat}: ${sourceName}`;
  usdLabelEl.textContent = state.usdName ? `USD: ${state.usdName}` : 'USD';
}

function updateJointPanelFold() {
  panelEl.classList.toggle('joints-collapsed', state.settings.jointsCollapsed);
  jointHeaderEl.setAttribute('aria-expanded', String(!state.settings.jointsCollapsed));
  jointFoldIconEl.textContent = state.settings.jointsCollapsed ? '▸' : '▾';
}

function toggleJointPanelFold() {
  state.settings.jointsCollapsed = !state.settings.jointsCollapsed;
  updateJointPanelFold();
}

async function loadRobotFile(file, existingStats = null) {
  clearRobot();
  rememberAssetFile(file);
  const stats = existingStats || beginLoadStats(file.name, file.size || 0);
  if (existingStats?.text !== undefined) {
    state.inputText = existingStats.text;
  } else {
    const readStart = performance.now();
    state.inputText = await file.text();
    stats.readMs = performance.now() - readStart;
  }
  stats.fileSize = state.inputText.length;
  state.inputName = file.name;
  const parseStart = performance.now();
  state.inputFormat = detectInputFormat(state.inputText);
  stats.parseMs = performance.now() - parseStart;
  setStatus(`Parsing ${state.inputFormat.toUpperCase()} ${file.name}...`);
  const baseDir = dirname(file.webkitRelativePath || file.name);
  const processStart = performance.now();
  const robot = state.inputFormat === 'mjcf'
    ? await parseMJCFWithMeshes(state.inputText, file.name, baseDir)
    : await parseURDFWithMeshes(state.inputText, file.name);
  stats.processMs = performance.now() - processStart;
  state.robot = robot;
  robotGroup.add(robot);
  applySceneOrientation();
  if (state.inputFormat === 'urdf') classifyRobotMeshes(robot);
  else applyVisibility();
  rebuildJointControls();
  const metadata = state.inputFormat === 'mjcf'
    ? {
      name: robot.name,
      links: new Map(Object.keys(robot.links || {}).map((name) => [name, { name }])),
      joints: Object.values(robot.joints || {})
    }
    : parseURDFMetadata(state.inputText);
  updateRobotInfo(metadata);
  captureSourceRestLinkMatrices();
  // Apply the active pose preset: MuJoCo default qpos0 = ref (the viewer's load
  // state), or the home keyframe when the "Home pose" toggle is on. Rest capture
  // above stays at the XML rest, keeping the USD binding reference clean.
  if (state.inputFormat === 'mjcf') applyPosePreset();
  updateButtonStates();
  rebuildGhosts();
  updateLabels();
  if (state.settings.autocenter) fitCamera(currentFitObjects());
  finishLoadStats(stats);
  setStatus(`Loaded ${state.inputFormat.toUpperCase()} ${file.name}`);
}

function matrixToUSDArray(matrix) {
  const e = matrix.elements;
  return [
    e[0], e[1], e[2], e[3],
    e[4], e[5], e[6], e[7],
    e[8], e[9], e[10], e[11],
    e[12], e[13], e[14], e[15]
  ];
}

function geometryPayload(mesh, ownerLink) {
  const geom = mesh.geometry;
  const pos = geom.getAttribute('position');
  if (!pos || pos.count < 3) return null;

  const normal = geom.getAttribute('normal');
  const uv = geom.getAttribute('uv');
  const index = geom.getIndex();
  const rel = new THREE.Matrix4()
    .copy(ownerLink.matrixWorld)
    .invert()
    .multiply(mesh.matrixWorld);

  return {
    name: mesh.name || mesh.parent?.name || 'mesh',
    matrix: matrixToUSDArray(rel),
    geometry: {
      positions: Array.from(pos.array),
      normals: normal ? Array.from(normal.array) : [],
      uvs: uv ? Array.from(uv.array) : [],
      indices: index ? Array.from(index.array) : []
    }
  };
}

function buildExportPayload() {
  if (!state.robot) throw new Error('No URDF robot loaded.');
  state.robot.updateWorldMatrix(true, true);
  const metadata = parseURDFMetadata(state.inputText);

  const linkPayloads = new Map();
  for (const [name, link] of Object.entries(state.robot.links || {})) {
    const info = metadata.links.get(name) || { name, inertial: {} };
    linkPayloads.set(name, {
      name,
      inertial: info.inertial || {},
      visuals: [],
      collisions: []
    });
    link.updateWorldMatrix(true, true);
  }

  for (const mesh of [...state.visualMeshes, ...state.collisionMeshes]) {
    const ownerLink = mesh.userData.urdfOwnerLink;
    if (!ownerLink) continue;
    const linkName = mesh.userData.urdfOwnerLinkName || ownerLink.name;
    const linkPayload = linkPayloads.get(linkName);
    if (!linkPayload) continue;
    const payload = geometryPayload(mesh, ownerLink);
    if (!payload) continue;
    if (mesh.userData.urdfCollision) {
      // Match the URDF→USD writer convention (convexHull) so re-export
      // round-trips don't silently regress to triangle-soup collisions.
      payload.approximation = payload.approximation || 'convexHull';
      linkPayload.collisions.push(payload);
    } else {
      linkPayload.visuals.push(payload);
    }
  }

  return {
    name: metadata.name || state.robot.name || 'Robot',
    upAxis: state.settings.upAxis,
    gravity: state.settings.upAxis === 'Z' ? [0, 0, -1] : [0, -1, 0],
    links: Array.from(linkPayloads.values()),
    joints: metadata.joints
  };
}

function buildCurrentExportPayload() {
  if (state.inputFormat === 'mjcf') {
    if (!state.exportPayload) throw new Error('No MJCF export payload is available.');
    return {
      ...state.exportPayload,
      upAxis: state.settings.upAxis,
      gravity: state.settings.upAxis === 'Z' ? [0, 0, -1] : [0, -1, 0]
    };
  }
  return buildExportPayload();
}

const DEG_TO_RAD = Math.PI / 180;

function basenameFromPath(primPath) {
  return String(primPath || '').split('/').filter(Boolean).pop() || '';
}

function parentLinkPath(primPath, linkPaths) {
  let best = null;
  for (const linkPath of linkPaths) {
    if (primPath === linkPath || primPath.startsWith(`${linkPath}/`)) {
      if (!best || linkPath.length > best.length) best = linkPath;
    }
  }
  return best;
}

function hasApi(prim, apiName) {
  return (prim.apiSchemas || []).some((api) => api === apiName || api.startsWith(`${apiName}:`));
}

function firstRelTarget(prim, name) {
  const rel = prim.relationships?.[name];
  if (Array.isArray(rel)) return rel[0] || '';
  return '';
}

function matrixTranslation(matrix) {
  if (!Array.isArray(matrix) || matrix.length < 15) return [0, 0, 0];
  return [matrix[12] || 0, matrix[13] || 0, matrix[14] || 0];
}

function matrixScale(matrix) {
  if (!Array.isArray(matrix) || matrix.length < 11) return [1, 1, 1];
  return [
    Math.hypot(matrix[0] || 0, matrix[1] || 0, matrix[2] || 0) || 1,
    Math.hypot(matrix[4] || 0, matrix[5] || 0, matrix[6] || 0) || 1,
    Math.hypot(matrix[8] || 0, matrix[9] || 0, matrix[10] || 0) || 1
  ];
}

function geometryToUrdf(geometry = {}, matrix) {
  if (geometry.type === 'box' || geometry.type === 'cube') {
    const scale = matrixScale(matrix);
    // GeomCube emits a scalar size (USD schema); legacy form emits a vec3.
    const base = Array.isArray(geometry.size)
      ? geometry.size
      : (typeof geometry.size === 'number'
          ? [geometry.size, geometry.size, geometry.size]
          : [2, 2, 2]);
    return { type: 'box', size: base.map((v, i) => Number(v || 1) * scale[i]) };
  }
  if (geometry.type === 'sphere') return { type: 'sphere', radius: Number(geometry.radius || 0) };
  if (geometry.type === 'cylinder' || geometry.type === 'capsule') {
    return {
      type: geometry.type,
      radius: Number(geometry.radius || 0),
      length: Number(geometry.length || geometry.height || 0)
    };
  }
  if (geometry.type === 'plane') {
    return { type: 'box', size: [Number(geometry.width || 1), Number(geometry.length || 1), 0.001] };
  }
  return { type: 'mesh', filename: `extracted:${geometry.source || 'mesh'}` };
}

function usdPhysicsToUrdf(extracted) {
  const prims = extracted.prims || [];
  const linkPrims = prims.filter((prim) => hasApi(prim, 'PhysicsRigidBodyAPI'));
  const linkPaths = new Set(linkPrims.map((prim) => prim.path));
  const links = linkPrims.map((prim) => ({
    name: prim.name || basenameFromPath(prim.path),
    path: prim.path,
    inertial: {
      mass: Number(prim.properties?.['physics:mass'] || 0),
      centerOfMass: prim.properties?.['physics:centerOfMass'] || [0, 0, 0],
      diagonalInertia: prim.properties?.['physics:diagonalInertia'] || [0, 0, 0]
    },
    visuals: [],
    collisions: []
  }));
  const linksByPath = new Map(links.map((link) => [link.path, link]));

  for (const prim of prims) {
    if (!prim.geometry) continue;
    const linkPath = parentLinkPath(prim.path, linkPaths);
    if (!linkPath) continue;
    const link = linksByPath.get(linkPath);
    const item = {
      name: prim.name || basenameFromPath(prim.path),
      origin: matrixTranslation(prim.matrix),
      geometry: geometryToUrdf(prim.geometry, prim.matrix)
    };
    if (hasApi(prim, 'PhysicsCollisionAPI')) link.collisions.push(item);
    else link.visuals.push(item);
  }

  const joints = prims
    .filter((prim) => /^Physics(?:Revolute|Prismatic|Spherical|Fixed|Joint)/.test(prim.type))
    .map((prim) => {
      const type = prim.type === 'PhysicsRevoluteJoint'
        ? 'revolute'
        : prim.type === 'PhysicsPrismaticJoint'
          ? 'prismatic'
          : prim.type === 'PhysicsSphericalJoint'
            ? 'spherical'
            : 'fixed';
      const parentPath = firstRelTarget(prim, 'physics:body0');
      const childPath = firstRelTarget(prim, 'physics:body1');
      const axisToken = prim.properties?.['physics:axis'] || 'X';
      const lower = prim.properties?.['physics:lowerLimit'];
      const upper = prim.properties?.['physics:upperLimit'];
      return {
        name: prim.name || basenameFromPath(prim.path),
        type,
        parent: linksByPath.get(parentPath)?.name || basenameFromPath(parentPath),
        child: linksByPath.get(childPath)?.name || basenameFromPath(childPath),
        origin: prim.properties?.['physics:localPos0'] || [0, 0, 0],
        axis: axisToken === 'Y' ? [0, 1, 0] : axisToken === 'Z' ? [0, 0, 1] : [1, 0, 0],
        limit: lower !== undefined || upper !== undefined
          ? {
            lower: type === 'revolute' ? Number(lower || 0) * DEG_TO_RAD : Number(lower || 0),
            upper: type === 'revolute' ? Number(upper || 0) * DEG_TO_RAD : Number(upper || 0)
          }
          : null,
        dynamics: jointDynamicsFromUsdPrim(prim, type)
      };
    });

  return {
    name: extracted.name || 'ConvertedFromUSDPhysics',
    upAxis: extracted.upAxis || 'Y',
    links,
    joints
  };
}

// Collect joint dynamics from a USD prim, accepting either the
// legacy MJC namespace (mjc:*) or the PhysX/Newton equivalents
// (physxJoint:*, physxLimit:*) — the first authored source wins.
// Mirrors ref/newton/newton/_src/usd/schemas.py SchemaResolverPhysx +
// ref/genesis/genesis/options/morphs.py revolute_joint_*_attr_candidates.
function jointDynamicsFromUsdPrim(prim, jointType) {
  const props = prim.properties || {};
  const limitNs = jointType === 'prismatic'
    ? 'physxLimit:linear:' : 'physxLimit:angular:';
  function pickNumber(...keys) {
    for (const k of keys) {
      const v = props[k];
      if (typeof v === 'number' && Number.isFinite(v)) return v;
    }
    return undefined;
  }
  return {
    damping: pickNumber('mjc:damping', limitNs + 'damping'),
    friction: pickNumber('mjc:frictionloss',
                          'physxJoint:jointFriction'),
    stiffness: pickNumber('mjc:stiffness', limitNs + 'stiffness'),
    armature: pickNumber('mjc:armature', 'physxJoint:armature')
  };
}

function usdPhysicsToSourceModel(extracted) {
  const prims = extracted.prims || [];
  const linkPrims = prims.filter((prim) => hasApi(prim, 'PhysicsRigidBodyAPI'));
  const links = linkPrims.map((prim) => ({
    name: prim.name || basenameFromPath(prim.path),
    path: prim.path,
    inertial: {
      mass: Number(prim.properties?.['physics:mass'] || 0),
      centerOfMass: prim.properties?.['physics:centerOfMass'] || [0, 0, 0],
      diagonalInertia: prim.properties?.['physics:diagonalInertia'] || [0, 0, 0]
    }
  }));
  const linksByPath = new Map(links.map((link) => [link.path, link]));

  const joints = prims
    .filter((prim) => /^Physics(?:Revolute|Prismatic|Spherical|Fixed|Joint)/.test(prim.type))
    .map((prim) => {
      const type = prim.type === 'PhysicsRevoluteJoint'
        ? 'revolute'
        : prim.type === 'PhysicsPrismaticJoint'
          ? 'prismatic'
          : prim.type === 'PhysicsSphericalJoint'
            ? 'spherical'
            : 'fixed';
      const parentPath = firstRelTarget(prim, 'physics:body0');
      const childPath = firstRelTarget(prim, 'physics:body1');
      const axisToken = prim.properties?.['physics:axis'] || 'X';
      const lower = prim.properties?.['physics:lowerLimit'];
      const upper = prim.properties?.['physics:upperLimit'];
      const ref = Number(prim.properties?.['mjc:ref']);
      return {
        name: prim.name || basenameFromPath(prim.path),
        type,
        parent: linksByPath.get(parentPath)?.name || basenameFromPath(parentPath),
        child: linksByPath.get(childPath)?.name || basenameFromPath(childPath),
        parentPath,
        childPath,
        axis: axisToken === 'Y' ? [0, 1, 0] : axisToken === 'Z' ? [0, 0, 1] : [1, 0, 0],
        axisToken,
        origin: prim.properties?.['physics:localPos0'] || [0, 0, 0],
        localPos0: prim.properties?.['physics:localPos0'],
        localPos1: prim.properties?.['physics:localPos1'],
        localRot0: prim.properties?.['physics:localRot0'],
        localRot1: prim.properties?.['physics:localRot1'],
        refRad: Number.isFinite(ref)
          ? (type === 'revolute' ? ref * DEG_TO_RAD : ref)
          : 0,
        limit: lower !== undefined || upper !== undefined
          ? {
            lower: type === 'revolute' ? Number(lower || 0) * DEG_TO_RAD : Number(lower || 0),
            upper: type === 'revolute' ? Number(upper || 0) * DEG_TO_RAD : Number(upper || 0)
          }
          : {},
        dynamics: jointDynamicsFromUsdPrim(prim, type)
      };
    });

  // Three independent collider signals; see `usdGeometryClassification`
  // above for rationale and ordering.
  const collisionPaths = new Set(
    prims
      .filter((prim) => prim.geometry
          && (hasApi(prim, 'PhysicsCollisionAPI')
              || Number(prim.properties?.['mjc:group']) >= 3
              || prim.purpose === 'guide'))
      .map((prim) => prim.path)
  );

  return {
    name: extracted.name || basenameFromPath(state.usdName) || 'ConvertedFromUSDPhysics',
    upAxis: extracted.upAxis || 'Y',
    links,
    joints,
    collisionPaths
  };
}

function fmtNumber(value) {
  if (!Number.isFinite(value)) return '0';
  return Number(value).toPrecision(9).replace(/\.?0+$/u, '');
}

function vec(value, fallback = [0, 0, 0]) {
  const a = Array.isArray(value) ? value : fallback;
  return [a[0] || 0, a[1] || 0, a[2] || 0].map(fmtNumber).join(' ');
}

function quatAttr(quaternion) {
  if (!quaternion) return '';
  const q = quaternion.clone().normalize();
  if (Math.abs(q.x) < 1e-9 && Math.abs(q.y) < 1e-9 && Math.abs(q.z) < 1e-9 && Math.abs(q.w - 1) < 1e-9) return '';
  return ` quat="${fmtNumber(q.w)} ${fmtNumber(q.x)} ${fmtNumber(q.y)} ${fmtNumber(q.z)}"`;
}

function mjcfBodyPoseFromJoint(joint) {
  const bodyMatrix = jointFrameMatrix(joint, 0).multiply(jointFrameMatrix(joint, 1).invert());
  return decomposeMatrix(bodyMatrix);
}

function mjcfJointPosAttr(joint) {
  const pos = joint.localPos1 || [0, 0, 0];
  return vectorFromArray(pos).lengthSq() > 1e-18 ? ` pos="${vec(pos)}"` : '';
}

function escapeXML(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/"/g, '&quot;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

function geometryXML(geometry = {}) {
  if (geometry.type === 'box' || geometry.type === 'cube') {
    // GeomCube emits a scalar size (USD schema); legacy form emits a vec3.
    const size = Array.isArray(geometry.size)
      ? geometry.size
      : (typeof geometry.size === 'number'
          ? [geometry.size, geometry.size, geometry.size]
          : [1, 1, 1]);
    return `<box size="${vec(size, [1, 1, 1])}"/>`;
  }
  if (geometry.type === 'sphere') return `<sphere radius="${fmtNumber(geometry.radius || 0)}"/>`;
  if (geometry.type === 'cylinder') {
    return `<cylinder radius="${fmtNumber(geometry.radius || 0)}" length="${fmtNumber(geometry.length || 0)}"/>`;
  }
  return `<mesh filename="${escapeXML(geometry.filename || 'extracted:mesh.obj')}"/>`;
}

function urdfToXML(robot) {
  const lines = [`<robot name="${escapeXML(robot.name)}">`];
  for (const link of robot.links) {
    lines.push(`  <link name="${escapeXML(link.name)}">`);
    if (link.inertial && link.inertial.mass > 0) {
      const i = link.inertial.diagonalInertia || [0, 0, 0];
      lines.push('    <inertial>');
      lines.push(`      <origin xyz="${vec(link.inertial.centerOfMass)}" rpy="0 0 0"/>`);
      lines.push(`      <mass value="${fmtNumber(link.inertial.mass)}"/>`);
      lines.push(`      <inertia ixx="${fmtNumber(i[0] || 0)}" ixy="0" ixz="0" iyy="${fmtNumber(i[1] || 0)}" iyz="0" izz="${fmtNumber(i[2] || 0)}"/>`);
      lines.push('    </inertial>');
    }
    for (const visual of link.visuals) {
      lines.push(`    <visual name="${escapeXML(visual.name)}">`);
      lines.push(`      <origin xyz="${vec(visual.origin)}" rpy="0 0 0"/>`);
      lines.push(`      <geometry>${geometryXML(visual.geometry)}</geometry>`);
      lines.push('    </visual>');
    }
    for (const collision of link.collisions) {
      lines.push(`    <collision name="${escapeXML(collision.name)}">`);
      lines.push(`      <origin xyz="${vec(collision.origin)}" rpy="0 0 0"/>`);
      lines.push(`      <geometry>${geometryXML(collision.geometry)}</geometry>`);
      lines.push('    </collision>');
    }
    lines.push('  </link>');
  }
  for (const joint of robot.joints) {
    lines.push(`  <joint name="${escapeXML(joint.name)}" type="${joint.type}">`);
    lines.push(`    <parent link="${escapeXML(joint.parent)}"/>`);
    lines.push(`    <child link="${escapeXML(joint.child)}"/>`);
    lines.push(`    <origin xyz="${vec(joint.origin)}" rpy="0 0 0"/>`);
    lines.push(`    <axis xyz="${vec(joint.axis, [1, 0, 0])}"/>`);
    if (joint.limit) {
      lines.push(`    <limit lower="${fmtNumber(joint.limit.lower)}" upper="${fmtNumber(joint.limit.upper)}" effort="0" velocity="0"/>`);
    }
    if (joint.dynamics?.damping !== undefined || joint.dynamics?.friction !== undefined) {
      lines.push(`    <dynamics damping="${fmtNumber(joint.dynamics.damping || 0)}" friction="${fmtNumber(joint.dynamics.friction || 0)}"/>`);
    }
    lines.push('  </joint>');
  }
  lines.push('</robot>');
  return `${lines.join('\n')}\n`;
}

function mjcfFromSourceModel(model) {
  const childJoints = new Map();
  const childLinks = new Set();
  for (const joint of model.joints || []) {
    childLinks.add(joint.child);
    if (!childJoints.has(joint.parent)) childJoints.set(joint.parent, []);
    childJoints.get(joint.parent).push(joint);
  }

  const linksByName = new Map((model.links || []).map((link) => [link.name, link]));
  const roots = (model.links || []).filter((link) => !childLinks.has(link.name));
  const axis = (value) => vec(value, [1, 0, 0]);

  const lines = [
    `<mujoco model="${escapeXML(model.name || 'ConvertedFromUSD')}">`,
    '  <worldbody>'
  ];
  const mjcfJointType = (joint) => joint.type === 'prismatic'
    ? 'slide'
    : joint.type === 'fixed'
      ? 'fixed'
      : joint.type === 'spherical'
        ? 'ball'
        : 'hinge';

  function writeBody(link, indent) {
    const pad = ' '.repeat(indent);
    lines.push(`${pad}<body name="${escapeXML(link.name)}">`);
    for (const joint of childJoints.get(link.name) || []) {
      const child = linksByName.get(joint.child);
      if (!child) continue;
      const jointType = mjcfJointType(joint);
      const range = joint.limit && Number.isFinite(joint.limit.lower) && Number.isFinite(joint.limit.upper)
        ? ` range="${fmtNumber(joint.limit.lower)} ${fmtNumber(joint.limit.upper)}"`
        : '';
      const bodyPose = mjcfBodyPoseFromJoint(joint);
      lines.push(`${pad}  <body name="${escapeXML(child.name)}" pos="${vec(bodyPose.position.toArray())}"${quatAttr(bodyPose.quaternion)}>`);
      if (jointType !== 'fixed') {
        lines.push(`${pad}    <joint name="${escapeXML(joint.name)}" type="${jointType}" axis="${axis(joint.axis)}"${mjcfJointPosAttr(joint)}${range}/>`);
      }
      lines.push(`${pad}    <!-- Mesh geometry is displayed from the loaded USD scene in the web demo. -->`);
      for (const grandChild of childJoints.get(child.name) || []) {
        const grandLink = linksByName.get(grandChild.child);
        if (grandLink) writeBodyFromJoint(child, grandChild, grandLink, indent + 4);
      }
      lines.push(`${pad}  </body>`);
    }
    lines.push(`${pad}</body>`);
  }

  function writeBodyFromJoint(parent, joint, link, indent) {
    const pad = ' '.repeat(indent);
    const jointType = mjcfJointType(joint);
    const range = joint.limit && Number.isFinite(joint.limit.lower) && Number.isFinite(joint.limit.upper)
      ? ` range="${fmtNumber(joint.limit.lower)} ${fmtNumber(joint.limit.upper)}"`
      : '';
    const bodyPose = mjcfBodyPoseFromJoint(joint);
    lines.push(`${pad}<body name="${escapeXML(link.name)}" pos="${vec(bodyPose.position.toArray())}"${quatAttr(bodyPose.quaternion)}>`);
    if (jointType !== 'fixed') {
      lines.push(`${pad}  <joint name="${escapeXML(joint.name)}" type="${jointType}" axis="${axis(joint.axis)}"${mjcfJointPosAttr(joint)}${range}/>`);
    }
    lines.push(`${pad}  <!-- Mesh geometry is displayed from the loaded USD scene in the web demo. -->`);
    for (const childJoint of childJoints.get(link.name) || []) {
      const childLink = linksByName.get(childJoint.child);
      if (childLink) writeBodyFromJoint(link, childJoint, childLink, indent + 2);
    }
    lines.push(`${pad}</body>`);
  }

  for (const root of roots) {
    writeBody(root, 4);
  }
  lines.push('  </worldbody>');
  lines.push('</mujoco>');
  return `${lines.join('\n')}\n`;
}

function findObjectsByUSDPath(root) {
  const byPath = new Map();
  root?.traverse((obj) => {
    const path = usdPathForObject(obj);
    if (path && !byPath.has(path)) byPath.set(path, obj);
  });
  return byPath;
}

function usdPathForObject(obj) {
  let cursor = obj;
  while (cursor) {
    const path = cursor.userData?.['primMeta.absPath'];
    if (path) {
      if (cursor === obj) return path;
      const name = sanitizeUSDIdentifier(obj.name || 'mesh', 'mesh');
      return `${path}/${name}`;
    }
    cursor = cursor.parent;
  }
  return '';
}

function bestOwnerLinkPath(path, linkPaths) {
  let best = '';
  for (const linkPath of linkPaths) {
    if (path === linkPath || path.startsWith(`${linkPath}/`)) {
      if (linkPath.length > best.length) best = linkPath;
    }
  }
  return best;
}

function cloneMeshForSource(mesh, ownerLink, linkName, isCollision) {
  const clone = new THREE.Mesh();
  clone.name = mesh.name;
  clone.renderOrder = mesh.renderOrder;
  if (mesh.geometry) clone.geometry = mesh.geometry.clone();
  if (Array.isArray(mesh.material)) {
    clone.material = mesh.material.map((mat) => mat.clone?.() || mat);
  } else if (mesh.material) {
    clone.material = mesh.material.clone?.() || mesh.material;
  }
  clone.userData = {
    ...mesh.userData,
    urdfOwnerLink: ownerLink,
    urdfOwnerLinkName: linkName,
    urdfCollision: isCollision
  };
  if (isCollision) {
    clone.userData.originalMaterial = clone.material;
    clone.material = collisionMaterial.clone();
  }
  return clone;
}

function matrixInSceneRoot(object, sceneRoot) {
  object.updateWorldMatrix(true, false);
  const parent = sceneRoot?.parent;
  if (!parent) return object.matrixWorld.clone();
  parent.updateWorldMatrix(true, false);
  return parent.matrixWorld.clone().invert().multiply(object.matrixWorld);
}

function matrixInUSDScene(object) {
  return matrixInSceneRoot(object, state.usdObject);
}

function vectorFromArray(values, fallback = [0, 0, 0]) {
  const v = Array.isArray(values) ? values : fallback;
  return new THREE.Vector3(v[0] || 0, v[1] || 0, v[2] || 0);
}

function quaternionFromUSDValue(value) {
  if (Array.isArray(value)) {
    if (value.length >= 4) return new THREE.Quaternion(value[1] || 0, value[2] || 0, value[3] || 0, value[0] ?? 1).normalize();
    if (value.length === 3) return new THREE.Quaternion(value[0] || 0, value[1] || 0, value[2] || 0, 1).normalize();
  }
  if (value && typeof value === 'object') {
    return new THREE.Quaternion(value.x || 0, value.y || 0, value.z || 0, value.w ?? value.real ?? 1).normalize();
  }
  return new THREE.Quaternion();
}

function jointFrameMatrix(joint, side) {
  const suffix = side === 1 ? '1' : '0';
  const posFallback = side === 1 ? [0, 0, 0] : joint?.origin;
  return new THREE.Matrix4().compose(
    vectorFromArray(joint?.[`localPos${suffix}`], posFallback),
    quaternionFromUSDValue(joint?.[`localRot${suffix}`]),
    new THREE.Vector3(1, 1, 1)
  );
}

function buildRestWorldByLinkPath(model, usdObjectsByPath, parentJointByChild, childJointsByParent, sceneRoot) {
  const restWorldByLinkPath = new Map();
  const rootLinks = (model.links || []).filter((link) => !parentJointByChild.has(link.path));
  const visit = (linkPath, restWorld) => {
    restWorldByLinkPath.set(linkPath, restWorld.clone());
    for (const joint of childJointsByParent.get(linkPath) || []) {
      const parentJointFrame = jointFrameMatrix(joint, 0);
      const childJointFrameInverse = jointFrameMatrix(joint, 1).invert();
      const childRest = restWorld.clone().multiply(parentJointFrame).multiply(childJointFrameInverse);
      visit(joint.childPath, childRest);
    }
  };

  for (const link of rootLinks) {
    const object = usdObjectsByPath.get(link.path);
    const rootRest = object ? matrixInSceneRoot(object, sceneRoot) : new THREE.Matrix4();
    visit(link.path, rootRest);
  }

  for (const link of model.links || []) {
    if (!restWorldByLinkPath.has(link.path)) {
      const object = usdObjectsByPath.get(link.path);
      restWorldByLinkPath.set(link.path, object ? matrixInSceneRoot(object, sceneRoot) : new THREE.Matrix4());
    }
  }

  return restWorldByLinkPath;
}

function meshRestWorldByLinkPath(sourceObject, linkPaths, collisionPaths) {
  const restByLinkPath = new Map();
  sourceObject?.updateWorldMatrix(true, true);
  sourceObject?.traverse((obj) => {
    if (!obj.isMesh) return;
    const path = usdPathForObject(obj);
    if (pathInSetOrUnder(path, collisionPaths)) return;
    const linkPath = bestOwnerLinkPath(path, linkPaths);
    if (!linkPath || restByLinkPath.has(linkPath)) return;
    restByLinkPath.set(linkPath, matrixInSceneRoot(obj, sourceObject));
  });
  return restByLinkPath;
}

function articulationModelFromSourcePayload(payload, sourceObject) {
  const linksScope = findObjectByName(sourceObject, 'Links');
  const pathByLinkName = new Map();
  if (linksScope) {
    for (const child of linksScope.children) {
      const path = usdPathForObject(child);
      if (path) pathByLinkName.set(child.name, path);
    }
  }
  const linkPath = (name) => {
    const usdName = sanitizeUSDIdentifier(name, 'link');
    return pathByLinkName.get(usdName) || `/World/Links/${usdName}`;
  };
  const links = (payload?.links || []).map((link) => ({
    name: link.name,
    path: linkPath(link.name),
    inertial: link.inertial || {}
  }));
  const pathByName = new Map(links.map((link) => [link.name, link.path]));
  const collisionPaths = new Set();
  for (const link of payload?.links || []) {
    const base = pathByName.get(link.name);
    if (!base) continue;
    for (const collision of link.collisions || []) {
      collisionPaths.add(`${base}/${sanitizeUSDIdentifier(collision.name, 'collision')}`);
    }
  }
  const joints = (payload?.joints || [])
    .map((joint) => ({
      ...joint,
      type: joint.type || joint.jointType || 'fixed',
      parentPath: pathByName.get(joint.parent) || '',
      childPath: pathByName.get(joint.child) || '',
      axis: joint.axis || [1, 0, 0],
      origin: joint.origin || joint.localPos0 || [0, 0, 0],
      localPos0: joint.localPos0,
      localPos1: joint.localPos1,
      localRot0: joint.localRot0,
      localRot1: joint.localRot1,
      refRad: joint.refRad || 0
    }))
    .filter((joint) => joint.parentPath && joint.childPath);
  return {
    name: payload?.name || 'ConvertedFromSource',
    upAxis: payload?.upAxis || state.settings.upAxis || 'Z',
    links,
    joints,
    collisionPaths
  };
}

function sourceRestWorldByLinkPath(model) {
  const rest = new Map();
  if (!state.robot?.links) return rest;
  state.robot.updateWorldMatrix(true, true);
  for (const link of model.links || []) {
    const sourceLink = state.robot.links[link.name];
    if (!sourceLink) continue;
    rest.set(link.path, matrixInSceneRoot(sourceLink, state.robot));
  }
  return rest;
}

function jointHasAuthoredRotations(joint) {
  return Array.isArray(joint?.localRot0) && Array.isArray(joint?.localRot1);
}

function modelNeedsJointFrameInference(model) {
  return (model.joints || []).some((joint) => !jointHasAuthoredRotations(joint));
}

function inferMissingJointFrames(model, restWorldByLinkPath) {
  for (const joint of model.joints || []) {
    if (jointHasAuthoredRotations(joint)) continue;
    const parentRest = restWorldByLinkPath.get(joint.parentPath);
    const childRest = restWorldByLinkPath.get(joint.childPath);
    if (!parentRest || !childRest) continue;
    const relative = parentRest.clone().invert().multiply(childRest);
    if (!Array.isArray(joint.localPos0)) {
      const { position } = decomposeMatrix(relative);
      joint.origin = position.toArray();
      joint.localPos0 = position.toArray();
    }
    if (!Array.isArray(joint.localPos1)) joint.localPos1 = [0, 0, 0];

    const local0Translation = new THREE.Matrix4().makeTranslation(...joint.localPos0);
    const local1Translation = new THREE.Matrix4().makeTranslation(...joint.localPos1);
    const rotationOnly = local0Translation.clone().invert().multiply(relative).multiply(local1Translation);
    const { quaternion } = decomposeMatrix(rotationOnly);
    joint.localRot0 = quaternionToUSDArray(quaternion);
    joint.localRot1 = [1, 0, 0, 0];
  }
}

function buildArticulatedRobotFromUSD(model, sourceObject, options = {}) {
  if (!sourceObject) throw new Error('No USD view is loaded.');
  const group = new THREE.Group();
  group.name = options.name || `${model.name || usdBaseName()}_mjcf`;
  group.links = {};
  group.joints = {};

  const usdObjectsByPath = findObjectsByUSDPath(sourceObject);
  const linkPaths = new Set((model.links || []).map((link) => link.path));
  const linksByPath = new Map((model.links || []).map((link) => [link.path, link]));
  const parentJointByChild = new Map((model.joints || []).map((joint) => [joint.childPath, joint]));
  const childJointsByParent = new Map();
  for (const joint of model.joints || []) {
    if (!childJointsByParent.has(joint.parentPath)) childJointsByParent.set(joint.parentPath, []);
    childJointsByParent.get(joint.parentPath).push(joint);
  }

  sourceObject.updateWorldMatrix(true, true);
  const restWorldByLinkPath = buildRestWorldByLinkPath(
    model,
    usdObjectsByPath,
    parentJointByChild,
    childJointsByParent,
    sourceObject
  );
  if (options.restWorldByLinkPath instanceof Map) {
    for (const [path, matrix] of options.restWorldByLinkPath) {
      restWorldByLinkPath.set(path, matrix.clone());
    }
  }
  if (options.inferMissingJointFrames && modelNeedsJointFrameInference(model)) {
    const meshRestFrames = meshRestWorldByLinkPath(sourceObject, linkPaths, model.collisionPaths);
    for (const [path, matrix] of meshRestFrames) {
      restWorldByLinkPath.set(path, matrix.clone());
    }
    inferMissingJointFrames(model, restWorldByLinkPath);
  }

  const nodeByLinkPath = new Map();
  for (const link of model.links || []) {
    const pivot = new THREE.Group();
    pivot.name = `${link.name}_joint`;
    const linkObject = new THREE.Group();
    linkObject.name = link.name;
    linkObject.userData['primMeta.absPath'] = link.path;
    pivot.add(linkObject);
    group.links[link.name] = linkObject;
    nodeByLinkPath.set(link.path, { link, pivot, linkObject });
  }

  const rootLinks = (model.links || []).filter((link) => !parentJointByChild.has(link.path));
  const attachLink = (linkPath, parentObject = group) => {
    const node = nodeByLinkPath.get(linkPath);
    if (!node) return;
    const { link, pivot, linkObject } = node;
    const joint = parentJointByChild.get(linkPath);
    if (joint) {
      applyMatrixToObject(pivot, jointFrameMatrix(joint, 0));
      applyMatrixToObject(linkObject, jointFrameMatrix(joint, 1).invert());
    } else {
      const rootRest = restWorldByLinkPath.get(linkPath) || new THREE.Matrix4();
      applyMatrixToObject(pivot, rootRest);
      linkObject.position.set(0, 0, 0);
      linkObject.quaternion.identity();
      linkObject.scale.set(1, 1, 1);
    }
    parentObject.add(pivot);

    if (joint) {
      group.joints[joint.name] = {
        ...joint,
        jointType: joint.type,
        pivot,
        origin: pivot.position.clone(),
        originQuat: pivot.quaternion.clone()
      };
    }

    for (const childJoint of childJointsByParent.get(linkPath) || []) {
      attachLink(childJoint.childPath, linkObject);
    }
  };
  for (const link of rootLinks) attachLink(link.path, group);

  // Localize each mesh against the SAME rest world used to place its link node
  // (the joint chain above), not the loaded USD's link-Xform world. The link
  // node sits at `restWorldByLinkPath`, so a mesh placed with
  //   meshLocal = inverse(restWorld) * meshWorld(loaded)
  // lands back at meshWorld(loaded) in the rest pose; the rig reproduces the
  // loaded USD exactly, then articulates from there.
  //
  // This is robust to where the writer parks the body-world transform:
  //   - baked into the link Xform (link world == restWorld): equivalent to the
  //     old loaded-Xform math, and
  //   - baked into the geoms with the link Xform left at identity (the current
  //     lightusd URDF/MJCF converter, MuJoCo-style): the old code used the
  //     identity link Xform, so meshLocal == full kinematic world, which the
  //     joint-positioned link node then double-applied, exploding the rig.
  // Falls back to the loaded link world, then identity, when a link has no
  // rest-world entry.
  const linkInverseWorld = new Map();
  for (const link of model.links || []) {
    let baseWorld = restWorldByLinkPath.get(link.path);
    if (!baseWorld) {
      const linkObj = usdObjectsByPath.get(link.path);
      baseWorld = linkObj ? matrixInSceneRoot(linkObj, sourceObject) : new THREE.Matrix4();
    }
    linkInverseWorld.set(link.path, baseWorld.clone().invert());
  }

  if (options.resetMeshLists) {
    state.visualMeshes = [];
    state.collisionMeshes = [];
  }
  sourceObject.traverse((obj) => {
    if (!obj.isMesh) return;
    const path = usdPathForObject(obj);
    const linkPath = bestOwnerLinkPath(path, linkPaths);
    if (!linkPath) return;
    const node = nodeByLinkPath.get(linkPath);
    const link = linksByPath.get(linkPath);
    if (!node || !link) return;
    const isCollision = pathInSetOrUnder(path, model.collisionPaths) || isCollisionObject(obj);
    const clone = cloneMeshForSource(obj, node.linkObject, link.name, isCollision);
    const local = linkInverseWorld.get(linkPath).clone().multiply(matrixInSceneRoot(obj, sourceObject));
    applyMatrixToObject(clone, local);
    node.linkObject.add(clone);
    if (options.resetMeshLists) {
      if (isCollision) state.collisionMeshes.push(clone);
      else state.visualMeshes.push(clone);
    }
  });

  group.setJointValue = (jointName, value) => {
    const joint = group.joints[jointName];
    if (!joint?.pivot) return;
    const displacement = value - (joint.refRad || 0);
    if (joint.jointType === 'prismatic') {
      const offset = axisVector(joint.axis).normalize().applyQuaternion(joint.originQuat).multiplyScalar(displacement);
      joint.pivot.position.copy(joint.origin).add(offset);
    } else if (joint.jointType !== 'fixed' && joint.jointType !== 'spherical') {
      // Spherical (3-DOF) joints can't be driven by a single scalar; leave at rest.
      joint.pivot.quaternion.copy(joint.originQuat)
        .multiply(new THREE.Quaternion().setFromAxisAngle(axisVector(joint.axis).normalize(), displacement));
    }
  };

  return group;
}

function buildGeneratedSourceFromUSD(model) {
  return buildArticulatedRobotFromUSD(model, state.usdRestObject || state.usdObject, {
    name: `${model.name || usdBaseName()}_mjcf`,
    resetMeshLists: true,
    inferMissingJointFrames: true
  });
}

async function ensureNativeExporter() {
  const loader = await ensureTinyLoader();
  if (!state.nativeExporter) {
    const Exporter = loader.native_.LightUSDLoaderNative ||
      loader.native_.NextUSDZConverterNative;
    if (typeof Exporter !== 'function') {
      throw new Error('The selected LightUSD backend does not provide the URDF/MJCF exporter.');
    }
    state.nativeExporter = new Exporter();
    const { maxUsdcMb, maxMemMb } = resolveUSDCExportCapsFromRuntime();
    // Raise the USDC writer's conservative WASM size caps so mesh-dense scenes
    // (e.g. robot_soccer_kit ~104MB, apptronik_apollo ~111MB) can export past
    // the 100MB default.
    state.nativeExporter.setUSDCExportLimitMB?.(maxUsdcMb, maxMemMb);
  }
  return state.nativeExporter;
}

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

function sourceBaseName() {
  return `${state.inputName || state.robot?.name || 'robot'}`
    .replace(/\.(urdf|xml)$/i, '')
    .replace(/[^A-Za-z0-9_.-]+/g, '_');
}

function usdBaseName() {
  const name = state.usdName || state.exportPayload?.name || state.robot?.name || 'robot';
  return `${name}`.replace(/\.(usd|usda|usdc|usdz)$/i, '').replace(/[^A-Za-z0-9_.-]+/g, '_');
}

function bytesFromNativeView(view) {
  if (!view) return null;
  return new Uint8Array(view);
}

function convertedUSDPreviewMaterial() {
  return new THREE.MeshStandardMaterial({
    color: 0x9aa5ad,
    roughness: 0.62,
    metalness: 0.0
  });
}

function shapeGeometryFromPayload(shape = {}) {
  if (shape.type === 'box') return new THREE.BoxGeometry(2, 2, 2);
  if (shape.type === 'sphere') return new THREE.SphereGeometry(shape.radius || 0.5, 24, 12);
  if (shape.type === 'cylinder') {
    const geometry = new THREE.CylinderGeometry(shape.radius || 0.5, shape.radius || 0.5, shape.height || 1, 24, 1);
    geometry.rotateX(Math.PI / 2);
    return geometry;
  }
  if (shape.type === 'capsule') {
    const radius = shape.radius || 0.5;
    const height = Math.max(shape.height || 1, 0.001);
    const geometry = typeof THREE.CapsuleGeometry === 'function'
      ? new THREE.CapsuleGeometry(radius, height, 12, 24)
      : new THREE.CylinderGeometry(radius, radius, height, 24, 1);
    geometry.rotateX(Math.PI / 2);
    return geometry;
  }
  if (shape.type === 'plane') {
    return new THREE.PlaneGeometry(shape.width || 1, shape.length || 1);
  }
  return null;
}

function matrixFromUSDArray(values) {
  const matrix = new THREE.Matrix4();
  if (Array.isArray(values) && values.length === 16) {
    matrix.fromArray(values);
  }
  return matrix;
}

function geometryFromPayloadItem(item, geometryCache) {
  const meshRef = item?.meshRef;
  if (meshRef && geometryCache.has(meshRef)) return geometryCache.get(meshRef);

  if (item?.shape) {
    const key = `shape:${JSON.stringify(item.shape)}`;
    if (geometryCache.has(key)) return geometryCache.get(key);
    const geometry = shapeGeometryFromPayload(item.shape);
    if (geometry) {
      geometry.computeBoundingSphere();
      geometryCache.set(key, geometry);
    }
    return geometry;
  }

  const buffer = meshRef ? state.nativeMeshBuffers.get(meshRef) : item?.geometry;
  const positions = buffer?.positions;
  if (!positions || positions.length < 9) return null;

  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute('position', new THREE.BufferAttribute(asFloat32Array(positions), 3));
  if (buffer.normals?.length === positions.length) {
    geometry.setAttribute('normal', new THREE.BufferAttribute(asFloat32Array(buffer.normals), 3));
  } else {
    geometry.computeVertexNormals();
  }
  if (buffer.uvs?.length === (positions.length / 3) * 2) {
    geometry.setAttribute('uv', new THREE.BufferAttribute(asFloat32Array(buffer.uvs), 2));
  }
  if (buffer.indices?.length) {
    geometry.setIndex(new THREE.BufferAttribute(asUint32Array(buffer.indices), 1));
  }
  geometry.computeBoundingSphere();
  if (meshRef) geometryCache.set(meshRef, geometry);
  return geometry;
}

function addPayloadMeshesToLink(linkGroup, items, geometryCache, material, isCollision, inverseSourceRest) {
  for (const item of items || []) {
    const geometry = geometryFromPayloadItem(item, geometryCache);
    if (!geometry) continue;
    const mesh = new THREE.Mesh(geometry, material.clone());
    mesh.name = sanitizeUSDIdentifier(item.name || (isCollision ? 'collision' : 'visual'), isCollision ? 'collision' : 'visual');
    const linkPath = linkGroup.userData?.['primMeta.absPath'] || `/World/Links/${linkGroup.name}`;
    mesh.userData['primMeta.absPath'] = `${linkPath}/${mesh.name}`;
    mesh.userData.urdfCollision = isCollision;
    let matrix = matrixFromUSDArray(item.matrix);
    if (state.inputFormat === 'mjcf' && inverseSourceRest) {
      matrix = new THREE.Matrix4().copy(inverseSourceRest).multiply(matrix);
    }
    applyMatrixToObject(mesh, matrix);
    if (isCollision) mesh.renderOrder = 8;
    linkGroup.add(mesh);
  }
}

function buildConvertedUSDPreviewFromPayload(payload) {
  const root = new THREE.Group();
  root.name = usdBaseName();
  const linksScope = new THREE.Group();
  linksScope.name = 'Links';
  root.add(linksScope);

  const geometryCache = new Map();
  const visualMaterial = convertedUSDPreviewMaterial();

  const sourceRestByLink = state.sourceRestLinkMatrices;
  for (const link of payload.links || []) {
    const linkName = link.name || 'link';
    const linkGroup = new THREE.Group();
    linkGroup.name = sanitizeUSDIdentifier(linkName, 'link');
    linkGroup.userData['primMeta.absPath'] = `/World/Links/${linkGroup.name}`;

    const sourceRest = sourceRestByLink.get(linkName);
    const inverseSourceRest = sourceRest ? sourceRest.clone().invert() : null;
    if (sourceRest) applyMatrixToObject(linkGroup, sourceRest);

    addPayloadMeshesToLink(linkGroup, link.visuals, geometryCache, visualMaterial, false, inverseSourceRest);
    addPayloadMeshesToLink(linkGroup, link.collisions, geometryCache, collisionMaterial, true, inverseSourceRest);

    linksScope.add(linkGroup);
  }

  return root;
}

function registerNativeMeshBuffer(native, item, collision) {
  const buffer = state.nativeMeshBuffers.get(item.meshRef);
  if (!buffer) throw new Error(`Missing native mesh buffer for ${item.meshRef}`);
  const fn = collision ? native.setCollisionMesh : native.setVisualMesh;
  if (typeof fn !== 'function') {
    throw new Error('LightUSD WASM does not expose setVisualMesh/setCollisionMesh.');
  }
  const ok = fn.call(native, item.meshRef, buffer.positions, buffer.normals, buffer.uvs, buffer.indices);
  if (!ok) throw new Error(native.error() || `Failed to register mesh buffer ${item.meshRef}`);
}

function registerNativeMeshBuffers(native, payload) {
  const used = new Set();
  const register = (item, collision) => {
    if (!item?.meshRef || used.has(item.meshRef)) return;
    registerNativeMeshBuffer(native, item, collision);
    used.add(item.meshRef);
  };

  native.clearURDFMeshBuffers?.();
  for (const link of payload.links || []) {
    for (const visual of link.visuals || []) register(visual, false);
    for (const collision of link.collisions || []) register(collision, true);
  }
}

function exportNativeStage(native, format, payload = null) {
  if (format === 'usda') {
    const text = native.exportAsUSDA();
    if (!text) throw new Error(native.error());
    return {
      bytes: new TextEncoder().encode(text),
      blob: new Blob([text], { type: 'text/plain' }),
      filename: `${usdBaseName()}.usda`,
      payload
    };
  }
  if (format === 'usdc') {
    const bytes = bytesFromNativeView(native.exportAsUSDC());
    if (!bytes) throw new Error(native.error());
    return {
      bytes,
      blob: new Blob([bytes], { type: 'application/octet-stream' }),
      filename: `${usdBaseName()}.usdc`,
      payload
    };
  }
  const bytes = bytesFromNativeView(native.exportAsUSDZ());
  if (!bytes) throw new Error(native.error());
  return {
    bytes,
    blob: new Blob([bytes], { type: 'model/vnd.usdz+zip' }),
    filename: `${usdBaseName()}.usdz`,
    payload
  };
}

// Feed referenced texture bytes into the WASM asset cache so exportAsUSDZ()
// embeds them into the package (the archive name matches the relative
// inputs:file reference the converter authored). usda/usdc don't need this —
// they reference the texture by the same relative path on disk.
async function registerTextureAssets(native, payload) {
  if (!native.setAsset) return;
  for (const m of payload.materials || []) {
    if (!m.texture) continue;
    const generated = state.generatedTextureAssets.get(m.texture);
    if (generated) {
      native.setAsset(m.texture, generated);
      continue;
    }
    const file = resolveAssetEntry(m.texture)?.file;
    if (!file) continue;
    try {
      native.setAsset(m.texture, new Uint8Array(await file.arrayBuffer()));
    } catch (err) {
      console.warn(`texture not embedded into usdz: ${m.texture}: ${err.message}`);
    }
  }
}

async function createUSDBytesFromSource(format, { autoEmbedTextures = false } = {}) {
  const native = await ensureNativeExporter();
  const payload = buildCurrentExportPayload();
  registerNativeMeshBuffers(native, payload);
  // For the on-screen comparison view, upgrade a textured scene to an embedded
  // USDZ so the loaded converted-USD carries the texture bytes (a usdc would
  // only reference a relative path the browser can't fetch). Explicit exports
  // keep the format the user asked for.
  if (autoEmbedTextures && format !== 'usdz' &&
      (payload.materials || []).some((m) => m.texture)) {
    format = 'usdz';
  }
  if (format === 'usdz') await registerTextureAssets(native, payload);
  setStatus(`Creating USD Physics + MuJoCo stage for ${payload.name}...`);
  const ok = native.createURDFPhysicsScene(JSON.stringify(payload));
  if (!ok) throw new Error(native.error());
  return exportNativeStage(native, format, payload);
}

async function createUSDBytesFromImportedUSD(format) {
  const native = await ensureNativeExporter();
  if (!state.latestUSDBytes) throw new Error('No imported or converted USD is loaded.');
  if (!native.loadFromBinary(state.latestUSDBytes, state.usdName || `scene.${state.latestUSDFormat || 'usd'}`)) {
    throw new Error(native.error() || 'Failed to load current USD for export.');
  }
  return exportNativeStage(native, format);
}

async function createUSDBytes(format) {
  return state.robot ? createUSDBytesFromSource(format) : createUSDBytesFromImportedUSD(format);
}

function exportSourceXML() {
  if (!state.inputText) throw new Error('No URDF/MJCF source is loaded.');
  const ext = state.inputFormat === 'urdf' ? 'urdf' : 'xml';
  const type = state.inputFormat === 'urdf' ? 'application/xml' : 'text/xml';
  downloadBlob(new Blob([state.inputText], { type }), `${sourceBaseName()}.${ext}`);
  setStatus(`Exported ${state.inputFormat.toUpperCase()} XML.`);
}

async function convertSourceToUSD(format = 'usdc') {
  const result = await createUSDBytesFromSource(format, { autoEmbedTextures: true });
  clearUSD();
  const object = await loadUSDObjectFromBytes(result.bytes, result.filename);
  const resultFormat = extension(result.filename).slice(1) || format;
  state.usdObject = object;
  state.usdRestObject = object;
  state.usdName = result.filename;
  state.latestUSDBytes = result.bytes;
  state.latestUSDFormat = resultFormat;
  usdGroup.add(object);
  applySceneOrientation();
  // Articulate from the *exported USD's* own physics, exactly like the import
  // path. This replaces the former payload-based preview that was positioned
  // from the source's world-space link matrices + a world-delta source sync:
  // that path dropped links on URDF input (the inverseSourceRest correction was
  // MJCF-only) and stopped following joints once the source and USD views no
  // longer shared a frame. Driving the USD's own joints is robust to both.
  let didArticulate = false;
  try {
    const extracted = await extractUSDPhysicsJSONFromBytes(result.bytes, result.filename);
    annotateUSDRenderableClasses(object, extracted);
    const model = usdPhysicsToSourceModel(extracted);
    if (model.links.length && model.joints.length) {
      const articulatedObject = buildArticulatedRobotFromUSD(model, object, {
        name: object.name,
        resetMeshLists: false,
        inferMissingJointFrames: true
      });
      usdGroup.remove(object);
      state.usdObject = articulatedObject;
      state.usdArticulation = articulatedObject;
      usdGroup.add(articulatedObject);
      // Match the USD articulation to the source's current joint pose.
      for (const [name, value] of Object.entries(state.jointValues)) {
        articulatedObject.setJointValue?.(name, value);
      }
      didArticulate = true;
    }
  } catch (err) {
    console.warn('USD Physics articulation skipped:', err);
  }
  if (!didArticulate && state.exportPayload?.joints?.length) {
    const model = articulationModelFromSourcePayload(state.exportPayload, object);
    if (model.links.length && model.joints.length) {
      const articulatedObject = buildArticulatedRobotFromUSD(model, object, {
        name: object.name,
        resetMeshLists: false,
        inferMissingJointFrames: false,
        restWorldByLinkPath: sourceRestWorldByLinkPath(model)
      });
      usdGroup.remove(object);
      state.usdObject = articulatedObject;
      state.usdArticulation = articulatedObject;
      usdGroup.add(articulatedObject);
      syncUSDArticulationToJointValues();
    }
  }
  // Draw the muscle/spatial-tendon polylines on the USD view too. The converted
  // USD carries the muscle topology (MjcTendon + MjcSite prims), but the site
  // markers are guide-purpose (not rendered); reuse the model-space line data
  // computed at MJCF-parse time (both views share the model coordinate space).
  if (state.muscleLineData) {
    const usdMuscle = makeMuscleGroup(state.muscleLineData);
    if (usdMuscle) {
      usdMuscle.visible = state.settings.showMuscles && state.settings.showTendons;
      usdGroup.add(usdMuscle);
      state.usdMuscleObjects.push(usdMuscle);
    }
  }
  if (state.sensorVisualData) {
    const usdSensors = makeSensorHelperGroup(state.sensorVisualData);
    if (usdSensors) {
      usdSensors.visible = state.settings.showSensors;
      usdGroup.add(usdSensors);
      state.usdSensorObjects.push(usdSensors);
    }
  }
  state.usdIsConverted = true;
  updateButtonStates();
  rebuildGhosts();
  updateLabels();
  applyVisibility();
  if (state.settings.autocenter) fitCamera(currentFitObjects());
  setStatus(`Converted ${state.inputFormat.toUpperCase()} to ${resultFormat.toUpperCase()} for comparison.`);
}

async function convertUSDToSource() {
  if (!state.latestUSDBytes) throw new Error('No USD is loaded.');
  const native = await ensureNativeExporter();
  if (!native.loadFromBinary(state.latestUSDBytes, state.usdName || `scene.${state.latestUSDFormat || 'usd'}`)) {
    throw new Error(native.error() || 'Failed to load USD for conversion.');
  }
  const jsonText = native.extractPhysicsSceneJSON();
  if (!jsonText) throw new Error(native.error() || 'USD Physics extraction failed.');
  const sourceModel = usdPhysicsToSourceModel(JSON.parse(jsonText));
  clearRobot();
  const robot = buildGeneratedSourceFromUSD(sourceModel);
  state.exportPayload = {
    ...sourceModel,
    links: sourceModel.links.map((link) => ({
      ...link,
      visuals: link.visuals || [],
      collisions: link.collisions || []
    }))
  };
  state.inputText = mjcfFromSourceModel(sourceModel);
  state.inputName = `${usdBaseName()}_from_usd.xml`;
  state.inputFormat = 'mjcf';
  state.robot = robot;
  robotGroup.add(robot);
  applySceneOrientation();
  applyVisibility();
  rebuildJointControls();
  updateRobotInfo({
    name: sourceModel.name,
    links: new Map(sourceModel.links.map((link) => [link.name, link])),
    joints: sourceModel.joints
  });
  captureSourceRestLinkMatrices();
  bindConvertedUSDLinksToSource();
  syncConvertedUSDToSourcePose();
  updateButtonStates();
  rebuildGhosts();
  updateLabels();
  if (state.settings.autocenter) fitCamera(currentFitObjects());
  setStatus(`Converted USD to generated MJCF view: ${sourceModel.links.length} links, ${sourceModel.joints.length} joints.`);
}

async function exportRobot(format) {
  try {
    const result = await createUSDBytes(format);
    downloadBlob(result.blob, result.filename);
    const native = await ensureNativeExporter();
    const warn = native.warn?.();
    setStatus(warn ? `Exported ${format.toUpperCase()} with warnings.` : `Exported ${format.toUpperCase()}.`);
    if (warn) console.warn(warn);
  } catch (err) {
    console.error(err);
    setStatus(`Export failed: ${err.message}`);
  }
}

async function loadInputFile(file, existingStats = null) {
  const ext = extension(file?.name || '');
  if (SOURCE_EXTENSIONS.has(ext)) {
    await loadRobotFile(file, existingStats);
  } else if (USD_MESH_EXTENSIONS.has(ext)) {
    await loadUSDFile(file, existingStats);
  } else {
    throw new Error(`Unsupported file extension ${ext || '(none)'}. Expected URDF, XML, or USD.`);
  }
}

async function loadURL(url) {
  const filename = basenameFromUrl(url);
  const ext = extension(filename);
  if (!SOURCE_EXTENSIONS.has(ext) && !USD_MESH_EXTENSIONS.has(ext)) {
    throw new Error(`Unsupported URL extension ${ext || '(none)'}. Expected URDF, XML, or USD.`);
  }
  const stats = beginLoadStats(filename, 0);
  setStatus(`Fetching ${filename}...`);
  const fetchStart = performance.now();
  const response = await fetch(url, { cache: 'no-store' });
  if (!response.ok && response.status !== 206) {
    throw new Error(`HTTP ${response.status} ${response.statusText}`);
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  stats.readMs = performance.now() - fetchStart;
  stats.fileSize = bytes.byteLength;
  updateLoadStatsPanel(stats);
  const file = new File([bytes], filename, { type: SOURCE_EXTENSIONS.has(ext) ? 'text/xml' : 'application/octet-stream' });
  if (SOURCE_EXTENSIONS.has(ext)) {
    stats.text = new TextDecoder().decode(bytes);
  } else {
    stats.bytes = bytes;
  }
  try {
    await loadInputFile(file, stats);
  } finally {
    delete stats.text;
    delete stats.bytes;
  }
}

function setupDragAndDrop() {
  const onEnter = (event) => {
    event.preventDefault();
    document.body.classList.add('drag-over');
  };
  const onLeave = (event) => {
    event.preventDefault();
    if (event.target === document.body || event.target === renderer.domElement) {
      document.body.classList.remove('drag-over');
    }
  };
  window.addEventListener('dragover', onEnter);
  window.addEventListener('dragleave', onLeave);
  window.addEventListener('drop', (event) => {
    event.preventDefault();
    document.body.classList.remove('drag-over');
    const file = event.dataTransfer?.files?.[0];
    if (!file) return;
    loadInputFile(file).catch((err) => {
      console.error(err);
      setStatus(`Drop load failed: ${err.message}`);
    });
  });
}

function setupURLAutoload() {
  const params = new URLSearchParams(window.location.search);
  const url = urlParam(params);
  window.renderComplete = false;
  window.renderError = null;
  const load = url
    ? loadURL(url)
    : loadRobotFile(new File([DEFAULT_SAMPLE_URDF], 'sample.urdf', {
        type: 'text/xml'
      }));
  load.then(() => {
    window.renderComplete = true;
  }).catch((err) => {
    console.error(err);
    setStatus(`${url ? 'URL' : 'Sample'} load failed: ${err.message}`);
    window.renderError = err?.message || String(err);
    window.renderComplete = true;
  });
}

document.getElementById('urdfInput').addEventListener('change', (event) => {
  const file = event.target.files?.[0];
  if (file) loadInputFile(file).catch((err) => {
    console.error(err);
    setStatus(`Load failed: ${err.message}`);
  });
  event.target.value = '';
});

document.getElementById('assetInput').addEventListener('change', (event) => {
  rebuildAssetFiles(Array.from(event.target.files || []));
});

document.getElementById('usdInput').addEventListener('change', (event) => {
  const file = event.target.files?.[0];
  if (file) loadInputFile(file).catch((err) => {
    console.error(err);
    setStatus(`USD load failed: ${err.message}`);
  });
  event.target.value = '';
});

sourceExportButton.addEventListener('click', () => {
  try {
    exportSourceXML();
  } catch (err) {
    console.error(err);
    setStatus(`Source export failed: ${err.message}`);
  }
});

convertToUSDButton.addEventListener('click', () => {
  convertSourceToUSD('usdc').catch((err) => {
    console.error(err);
    setStatus(`Convert to USD failed: ${err.message}`);
  });
});

convertToSourceButton.addEventListener('click', () => {
  convertUSDToSource().catch((err) => {
    console.error(err);
    setStatus(`Convert to MJCF failed: ${err.message}`);
  });
});

document.getElementById('exportUSDA').addEventListener('click', () => exportRobot('usda'));
document.getElementById('exportUSDC').addEventListener('click', () => exportRobot('usdc'));
document.getElementById('exportUSDZ').addEventListener('click', () => exportRobot('usdz'));
fitViewButton.addEventListener('click', fitCurrentView);
jointHeaderEl.addEventListener('click', toggleJointPanelFold);

// Collapse/expand the whole Robot/Joints panel (foldable header).
const panelHeaderEl = document.getElementById('panelHeader');
const togglePanelFold = () => panelEl.classList.toggle('collapsed');
panelHeaderEl?.addEventListener('click', togglePanelFold);
panelHeaderEl?.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); togglePanelFold(); }
});

// Diagnostics hook for headless drivers: count effectively-visible meshes per
// view so a "blank render" (model loaded but nothing actually drawn) can be
// detected instead of silently passing.
window.__fitView = () => fitCamera(currentFitObjects());
window.__viewerStats = () => {
  const effVisible = (o) => { for (let c = o; c; c = c.parent) { if (!c.visible) return false; } return true; };
  const countVisible = (root) => {
    let n = 0;
    root.traverse((o) => { if (o.isMesh && effVisible(o)) n++; });
    return n;
  };
  return {
    links: Object.keys(state.robot?.links || state.usdArticulation?.links || {}).length,
    sourceVisibleMeshes: countVisible(robotGroup),
    usdVisibleMeshes: countVisible(usdGroup),
    tendonSegments: state.muscleLineData?.radii?.length || 0,
    sensors: state.sensorVisualData?.length || 0,
    sensorPreviewCameras: state.sensorPreviewCameras.length,
    payloadSites: state.exportPayload?.sites?.length || 0,
    payloadTendons: state.exportPayload?.tendons?.length || 0,
    payloadMjcActuators: state.exportPayload?.mjcActuators?.length || 0,
    payloadSensors: state.exportPayload?.sensors?.length || 0
  };
};

window.addEventListener('keydown', (event) => {
  if (event.key.toLowerCase() !== 'f' || event.altKey || event.ctrlKey || event.metaKey) return;
  const tagName = event.target?.tagName?.toLowerCase();
  if (tagName === 'input' || tagName === 'textarea' || tagName === 'select' || event.target?.isContentEditable) return;
  event.preventDefault();
  fitCurrentView();
});

window.addEventListener('resize', () => {
  camera.aspect = Math.max(window.innerWidth * 0.5, 1) / Math.max(window.innerHeight, 1);
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
});

const animationStartTime = performance.now();
function renderSplitView() {
  const width = renderer.domElement.clientWidth;
  const height = renderer.domElement.clientHeight;
  const leftWidth = Math.floor(width * 0.5);
  const rightWidth = width - leftWidth;

  camera.aspect = Math.max(leftWidth, 1) / Math.max(height, 1);
  camera.updateProjectionMatrix();
  renderer.setScissorTest(true);
  renderer.clear();

  renderer.setViewport(0, 0, leftWidth, height);
  renderer.setScissor(0, 0, leftWidth, height);
  renderer.render(sourceView.scene, camera);

  renderer.setViewport(leftWidth, 0, rightWidth, height);
  renderer.setScissor(leftWidth, 0, rightWidth, height);
  renderer.render(usdView.scene, camera);

  renderer.setScissorTest(false);
  renderSensorPreview();
}

function animate() {
  requestAnimationFrame(animate);
  const now = performance.now();
  if (!loadStats.fpsLast) loadStats.fpsLast = now;
  loadStats.frames++;
  if (now - loadStats.fpsLast >= 500) {
    loadStats.fps = Math.round((loadStats.frames * 1000) / (now - loadStats.fpsLast));
    loadStats.frames = 0;
    loadStats.fpsLast = now;
    updateLoadStatsPanel();
  }
  const elapsed = (performance.now() - animationStartTime) / 1000;
  if ((state.robot || state.usdArticulation) && state.settings.animateJoints) {
    for (const [name, joint] of Object.entries(state.joints)) {
      const type = joint.jointType || joint.type || 'fixed';
      if (type === 'fixed') continue;
      const [min, max] = jointLimits(joint);
      const mid = (min + max) * 0.5;
      const amp = (max - min) * 0.45;
      if (Number.isFinite(mid) && Number.isFinite(amp)) {
        setJointValue(name, mid + Math.sin(elapsed * state.settings.animationSpeed) * amp);
      }
    }
  }
  controls.update();
  syncConvertedUSDToSourcePose();
  syncGhosts();
  updateSkeletonDebugVisualizations();
  renderSplitView();
}

buildGUI();
if (backendSelect) {
  backendSelect.value = state.settings.backend;
  backendSelect.addEventListener('change', () => {
    state.nativeExporter?.delete?.();
    state.lightusdLoader?.dispose?.();
    state.nativeExporter = null;
    state.lightusdLoader = null;
    state.settings.backend = backendSelect.value;
    if (loadStats.current) loadStats.current.backend = state.settings.backend;
    updateLoadStatsPanel();
    setStatus(`USD backend set to ${state.settings.backend}.`);
  });
}
updateButtonStates();
updateLabels();
updateJointPanelFold();
updateLoadStatsPanel(beginLoadStats('-', 0));
setupDragAndDrop();
setupURLAutoload();
animate();
