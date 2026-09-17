import * as THREE from 'three';
import { Timer } from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils, TextureLoadingManager } from 'lightusd/LightUSDLoaderUtils.js';
import { setLightUSD as setMaterialXLightUSD } from 'lightusd/LightUSDMaterialX.js';
import { extractSkinnedMeshData } from 'lightusd/USDSceneSkinningData.js';
import { buildSkeletonDataFromUSD } from 'lightusd/USDSkeletonData.js';
import { applyUSDSceneSkinningPipeline } from 'lightusd/USDSceneSkinningPipeline.js';
import { extractUSDSceneAnimations } from 'lightusd/USDSceneAnimationPipeline.js';
import { buildNodeIndexMap } from 'lightusd/USDAnimationConverter.js';
import { getUSDSceneMetadata } from 'lightusd/USDSceneMetadata.js';
import {
  AssetBudget, FileAssetResolver, HttpAssetResolver, chooseRootFile, composeLayer,
  formatBytes, MAX_ASSET_BYTES
} from '../online-usd-viewer.js';
import { applyUSDMaterialFeatures } from '../usd-material-features.js';

// Three.js r183 provides the ACES 1.x-era filmic approximation but not the
// ACES 2 rendering transform. Reuse the compact WebGL approximation from the
// OpenChess demo: a modern shoulder plus highlight chroma compression.
const ACES2_APPROX_GLSL = `
vec3 CustomToneMapping( vec3 color ) {
  color = max( color * toneMappingExposure, vec3( 0.0 ) );
  float peak = max( max( color.r, color.g ), color.b );
  float chromaCompression = 1.0 / ( 1.0 + 0.18 * peak * peak );
  float luminance = dot( color, vec3( 0.2126, 0.7152, 0.0722 ) );
  color = mix( vec3( luminance ), color, chromaCompression );
  color = ( color * ( 2.51 * color + 0.03 ) ) /
          ( color * ( 2.43 * color + 0.59 ) + 0.14 );
  return clamp( color, 0.0, 1.0 );
}`;
THREE.ShaderChunk.tonemapping_pars_fragment = THREE.ShaderChunk.tonemapping_pars_fragment
  .replace('vec3 CustomToneMapping( vec3 color ) { return color; }', ACES2_APPROX_GLSL);

const TONE_MAPS = {
  aces2: THREE.CustomToneMapping,
  aces1: THREE.ACESFilmicToneMapping,
  srgb: THREE.NoToneMapping,
  raw: THREE.NoToneMapping
};

const REMOTE_SAMPLES = [
  ['Choose a usd-assets sample…', ''],
  ['Cesium Man (animated USDZ)', 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/CesiumMan/CesiumMan.usdz'],
  ['Teapot (payload + UsdPreviewSurface)', 'https://usd-assets.needle.tools/full_assets/Teapot/Teapot/Teapot.usd'],
  ['OpenChessSet (MaterialX)', 'https://usd-assets.needle.tools/full_assets/OpenChessSet/chess_set/chess_set.usda'],
  ['Damaged Helmet (GitHub API)', 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz']
];

const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar"><div><a class="demo-back" href="./">Demos</a><h1>Online USD Viewer</h1>
    <p>Drop a USD file or folder. References load first; payloads stay deferred until requested.</p></div>
    <div class="demo-actions"><button id="open-btn">Open USD</button><button id="folder-btn">Open Folder</button><button id="clear-btn">Clear</button><button id="fit-btn">Fit</button></div>
  </header>
  <main class="demo-main"><section class="viewport-wrap"><div id="viewport" class="viewport"></div><div id="drop-zone" class="drop-hint">Drop USD, USDZ, or a folder here</div><div id="status" class="status">Choose a USD file, folder, or URL below.</div><div id="fetch-progress" style="display:none;position:absolute;left:18px;right:18px;bottom:18px;padding:8px 10px;background:rgba(10,12,18,.86);border:1px solid rgba(148,163,184,.25);border-radius:6px"><div id="fetch-progress-label" style="font-size:.76rem;color:#cbd5e1;margin-bottom:5px">Fetching asset…</div><div style="height:5px;background:#263244;border-radius:4px;overflow:hidden"><div id="fetch-progress-fill" style="height:100%;width:0;background:#38bdf8;transition:width .12s ease"></div></div></div>
    <div id="timeline" class="timeline-bar" hidden><button id="play-btn">Play</button><label>Clip <select id="clip-select"></select></label><label>Speed <select id="speed-select"><option>0.25</option><option>0.5</option><option selected>1</option><option>2</option><option>4</option></select>×</label><input id="scrub" type="range" min="0" max="1" step="0.001" value="0"><span class="muted">F current · A all</span></div>
  </section><aside class="info-panel" style="overflow:auto"><h2>Source</h2><select id="sample-select" style="width:100%">${REMOTE_SAMPLES.map(([label, url]) => `<option value="${url}">${label}</option>`).join('')}</select><input id="url-input" placeholder="https://…/scene.usd" style="width:100%;box-sizing:border-box;margin-top:8px"><button id="url-btn" style="margin-top:8px">Load URL</button><p id="source-note" class="muted">GitHub-hosted assets use the GitHub Contents API. Other hosts still need CORS; 403/429 responses are shown here.</p>
    <h2>Shading</h2><select id="material-select" style="width:100%"><option value="auto">Auto</option><option value="usdpreviewsurface">UsdPreviewSurface</option><option value="openpbr">MaterialX / OpenPBR</option></select><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">Environment<select id="environment-select" style="width:100%;margin-top:4px"><option value="goegap">Goegap HDRI (default)</option><option value="sunsky">Synthetic Sun / Sky</option><option value="furnace">White Furnace</option></select></label><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">Rotation <span id="environment-rotation-value">0°</span><input id="environment-rotation" type="range" min="-180" max="180" step="1" value="0" style="width:100%"></label><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">Lighting intensity <span id="environment-intensity-value">1.00</span><input id="environment-intensity" type="range" min="0" max="4" step="0.05" value="1" style="width:100%"></label><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">Background intensity <span id="background-intensity-value">1.00</span><input id="background-intensity" type="range" min="0" max="2" step="0.05" value="1" style="width:100%"></label><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">Background blur <span id="background-blur-value">0.00</span><input id="background-blur" type="range" min="0" max="1" step="0.01" value="0" style="width:100%"></label><label style="display:flex;align-items:center;gap:7px;margin-top:8px;font-size:.8rem;color:#cbd5e1"><input id="background-visible" type="checkbox" checked> Show background</label><label style="display:flex;align-items:center;gap:7px;margin-top:6px;font-size:.8rem;color:#cbd5e1"><input id="ground-visible" type="checkbox"> Show ground plane</label><label style="display:flex;align-items:center;gap:7px;margin-top:6px;font-size:.8rem;color:#cbd5e1"><input id="environment-shadow" type="checkbox" checked> Soft contact shadow</label><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">AOV<select id="aov-select" style="width:100%;margin-top:4px"><option value="beauty">Beauty</option><option value="baseColor">Base Color</option><option value="normal">Normal</option><option value="roughness">Roughness</option><option value="metalness">Metalness</option><option value="occlusion">Ambient Occlusion</option><option value="emission">Emission</option><option value="opacity">Opacity</option></select></label><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">Display transform<select id="tone-map-select" style="width:100%;margin-top:4px"><option value="aces2">ACES 2.0 (approx)</option><option value="aces1">ACES 1.x Filmic</option><option value="srgb">sRGB</option><option value="raw">RAW (linear)</option></select></label><label style="display:block;margin-top:8px;font-size:.8rem;color:#cbd5e1">Exposure <span id="exposure-value">0.0 EV</span><input id="exposure" type="range" min="-4" max="4" step="0.1" value="0" style="width:100%"></label>
    <button id="payload-btn" hidden style="width:100%;margin-top:10px">Load deferred payload</button>
    <h2>Scene</h2><dl><dt>Source</dt><dd id="stat-source">—</dd><dt>Budget</dt><dd id="stat-budget">—</dd><dt>Meshes</dt><dd id="stat-meshes">—</dd><dt>Materials</dt><dd id="stat-materials">—</dd><dt>Textures</dt><dd id="stat-textures">—</dd><dt>Animations</dt><dd id="stat-anims">—</dd><dt>Playback FPS</dt><dd id="stat-fps">—</dd><dt>Composition</dt><dd id="stat-compose">—</dd></dl>
  </aside></main><input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden><input id="folder-input" type="file" webkitdirectory directory multiple hidden>
</div>`;

const $ = (id) => document.getElementById(id);
const viewport = $('viewport');
const scene = new THREE.Scene(); scene.background = new THREE.Color(0x0e0e10);
const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 200); camera.position.set(3, 2.5, 4);
const renderer = new THREE.WebGLRenderer({ antialias: true }); renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); renderer.outputColorSpace = THREE.SRGBColorSpace; renderer.toneMapping = THREE.CustomToneMapping; renderer.shadowMap.enabled = true; renderer.shadowMap.type = THREE.PCFShadowMap; viewport.appendChild(renderer.domElement);
const pmremGenerator = new THREE.PMREMGenerator(renderer);
const controls = new OrbitControls(camera, renderer.domElement); controls.enableDamping = true;
// Keep helper lights restrained: the PMREM environment is the primary source
// for PreviewSurface specular. Strong unfiltered lights wash out roughness-mip
// differences and make rough surfaces look polished.
// PMREM already supplies diffuse ambient light. Keep only a very faint fill;
// a stronger hemisphere light flattens curved surfaces and normal-map detail.
scene.add(new THREE.HemisphereLight(0xdde8f6, 0x24272c, 0.05)); const key = new THREE.DirectionalLight(0xffffff, 1.1); key.position.set(4, 6, 5); key.castShadow = true; key.shadow.mapSize.set(2048, 2048); key.shadow.bias = -0.00015; key.shadow.normalBias = 0.025; key.shadow.radius = 5; key.shadow.blurSamples = 16; scene.add(key); scene.add(key.target);
const grid = new THREE.GridHelper(10, 20, 0x44444a, 0x26262b); grid.visible = false; scene.add(grid);
const groundPlane = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshStandardMaterial({ color: 0x30343b, metalness: 0, roughness: 0.92 })); groundPlane.name = 'ViewerGroundPlane'; groundPlane.rotation.x = -Math.PI / 2; groundPlane.receiveShadow = true; groundPlane.visible = false; groundPlane.renderOrder = -1; scene.add(groundPlane);
const world = new THREE.Group(); scene.add(world);

const state = { loader: null, record: null, resolver: null, budget: null, sourceName: '', material: 'auto', environmentPreset: 'goegap', environmentRotation: 0, environmentIntensity: 1, backgroundIntensity: 1, backgroundBlur: 0, backgroundVisible: true, groundVisible: false, environmentShadow: true, aov: 'beauty', toneMap: 'aces2', exposureEV: 0, generation: 0, mixer: null, actions: [], clips: [], playing: false, duration: 0, animationFPS: 24, playbackSpeed: 1, textureManager: null, domeEnvironment: null, activeEnvironment: null, animationBounds: null, framingBounds: null, loadProgressTimer: 0, loadProgressInterval: 0 };
function status(message) { $('status').textContent = message; }
function beginDelayedLoadProgress(label) {
  endDelayedLoadProgress(false);
  const started = performance.now();
  const update = () => {
    const elapsed = Math.floor((performance.now() - started) / 1000);
    fetchProgress({ assetPath: elapsed >= 3 ? `${label} (${elapsed}s)` : label, loaded: 0, total: 0 });
  };
  update();
  state.loadProgressInterval = window.setInterval(update, 1000);
}
function endDelayedLoadProgress(hide = true) {
  clearTimeout(state.loadProgressTimer); clearInterval(state.loadProgressInterval);
  state.loadProgressTimer = 0; state.loadProgressInterval = 0;
  if (hide) fetchProgress(null);
}
function fetchProgress(info) {
  const box = $('fetch-progress'); const fill = $('fetch-progress-fill'); const label = $('fetch-progress-label');
  if (!info) { box.style.display = 'none'; return; }
  box.style.display = 'block';
  const loaded = Number(info.loaded) || 0; const total = Number(info.total) || 0;
  fill.style.width = total > 0 ? `${Math.min(100, (loaded / total) * 100)}%` : '35%';
  fill.style.opacity = total > 0 ? '1' : '.65';
  label.textContent = total > 0
    ? `Fetching ${info.assetPath || 'asset'} — ${formatBytes(loaded)} / ${formatBytes(total)}`
    : `Fetching ${info.assetPath || 'asset'} — ${formatBytes(loaded)}`;
}
function stat(id, value) { $(id).textContent = value; }
function updateStats(layer, composition = 'references') {
  stat('stat-source', state.sourceName || '—'); stat('stat-budget', state.budget?.label() || '—'); stat('stat-compose', composition);
  stat('stat-meshes', layer?.numMeshes?.() ?? '—'); stat('stat-materials', layer?.numMaterials?.() ?? '—'); stat('stat-textures', layer?.numImages?.() ?? '—'); stat('stat-anims', state.clips.length || 0);
  $('payload-btn').hidden = !state.record?.hasPayload || composition === 'payload';
}
async function ensureLoader() {
  if (state.loader) return state.loader;
  status('Initializing LightUSD…'); state.loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512 });
  await state.loader.init({ backend: 'legacy', useZstdCompressedWasm: false, useMemory64: false });
  LightUSDLoaderUtils.setLightUSD(state.loader.native_); setMaterialXLightUSD(state.loader.native_); state.loader.setMaxMemoryLimitMB?.(512); return state.loader;
}
function clearWorld() {
  endDelayedLoadProgress();
  state.textureManager?.abort?.(); state.textureManager?.reset?.(); state.textureManager = null;
  state.mixer?.stopAllAction?.(); state.mixer = null; state.actions = []; state.clips = []; state.duration = 0; state.playing = false; state.animationBounds = null; state.framingBounds = null; $('timeline').hidden = true; $('play-btn').textContent = 'Play'; $('scrub').value = '0';
  while (world.children.length) { const item = world.children.pop(); item.traverse?.((o) => { o.geometry?.dispose?.(); const ms = Array.isArray(o.material) ? o.material : [o.material]; ms.forEach((m) => m?.dispose?.()); }); }
}
function disposeRecord(record) {
  record?.layer?.delete?.();
  record?.composer?.setLayer?.(null);
}
function resetViewer() {
  state.generation++;
  clearWorld();
  disposeRecord(state.record);
  state.resolver?.clearCache?.();
  state.record = null; state.resolver = null; state.budget = null; state.sourceName = '';
  state.animationFPS = 24; state.playbackSpeed = 1;
  $('speed-select').value = '1'; $('sample-select').value = ''; $('url-input').value = '';
  for (const id of ['stat-source', 'stat-budget', 'stat-meshes', 'stat-materials', 'stat-textures', 'stat-anims', 'stat-fps', 'stat-compose']) stat(id, '—');
  $('payload-btn').hidden = true;
  fetchProgress(null);
  status('Scene cleared. Choose a USD file, folder, or URL.');
}
function currentSceneBounds() {
  world.updateMatrixWorld(true);
  return new THREE.Box3().setFromObject(world, true);
}
function animatedSceneBounds() {
  if (!state.mixer || !state.actions.length) return currentSceneBounds();
  if (state.animationBounds) return state.animationBounds.clone();
  const snapshots = state.actions.map((action) => ({
    time: action.time, enabled: action.enabled, paused: action.paused,
    weight: action.getEffectiveWeight(), running: action.isRunning()
  }));
  const result = new THREE.Box3();
  state.actions.forEach((action) => action.stop());
  state.actions.forEach((action) => {
    const duration = Math.max(action.getClip().duration, 0);
    action.reset().play(); action.enabled = true; action.paused = false; action.setEffectiveWeight(1);
    const samples = Math.max(2, Math.min(24, Math.ceil(duration) + 1));
    for (let index = 0; index < samples; index++) {
      action.time = duration * index / (samples - 1);
      state.mixer.update(0);
      result.union(currentSceneBounds());
    }
    action.stop();
  });
  state.actions.forEach((action, index) => {
    const saved = snapshots[index];
    action.reset(); action.enabled = saved.enabled; action.paused = saved.paused;
    action.setEffectiveWeight(saved.weight); action.time = saved.time;
    if (saved.running) action.play();
  });
  state.mixer.update(0);
  state.animationBounds = result.clone();
  return result;
}
function updateShadowFrustum(box, center, radius) {
  const rotation = THREE.MathUtils.degToRad(state.environmentRotation);
  const direction = new THREE.Vector3(0.55, 0.9, 0.45)
    .applyAxisAngle(new THREE.Vector3(0, 1, 0), rotation).normalize();
  key.target.position.copy(center); key.position.copy(center).addScaledVector(direction, radius * 4);
  key.target.updateMatrixWorld(true); key.updateMatrixWorld(true);
  key.shadow.camera.position.copy(key.position);
  key.shadow.camera.lookAt(center);
  key.shadow.camera.updateMatrixWorld(true);
  const lightBox = new THREE.Box3();
  for (const x of [box.min.x, box.max.x]) for (const y of [box.min.y, box.max.y]) for (const z of [box.min.z, box.max.z]) {
    lightBox.expandByPoint(new THREE.Vector3(x, y, z).applyMatrix4(key.shadow.camera.matrixWorldInverse));
  }
  const pad = Math.max(radius * 0.08, 0.01);
  Object.assign(key.shadow.camera, {
    left: lightBox.min.x - pad, right: lightBox.max.x + pad,
    bottom: lightBox.min.y - pad, top: lightBox.max.y + pad,
    near: Math.max(0.01, -lightBox.max.z - pad),
    far: Math.max(0.02, -lightBox.min.z + pad)
  });
  key.shadow.camera.updateProjectionMatrix(); key.shadow.needsUpdate = true;
}
function fitScene(mode = 'current') {
  const box = mode === 'all' ? animatedSceneBounds() : currentSceneBounds();
  if (box.isEmpty()) return;
  state.framingBounds = box.clone();
  const size = box.getSize(new THREE.Vector3()); const center = box.getCenter(new THREE.Vector3()); const radius = Math.max(size.length() * 0.5, 0.1); const floorY = box.min.y - radius * 0.01; grid.position.y = floorY; groundPlane.position.set(center.x, floorY - radius * 0.002, center.z); groundPlane.scale.setScalar(radius * 8); camera.near = Math.max(radius / 100, 0.001); camera.far = radius * 100; const fitDistance = radius / Math.sin(THREE.MathUtils.degToRad(camera.fov * 0.5)) * 1.12; const viewDirection = new THREE.Vector3(1.4, .9, 1.6).normalize(); camera.position.copy(center).addScaledVector(viewDirection, fitDistance); camera.lookAt(center); controls.target.copy(center); controls.update(); updateEnvironmentDisplay();
}
function updateEnvironmentDisplay() {
  const environment = state.activeEnvironment;
  const rotation = THREE.MathUtils.degToRad(state.environmentRotation);
  scene.environmentRotation.set(0, rotation, 0);
  scene.backgroundRotation.set(0, rotation, 0);
  scene.background = state.backgroundVisible ? (environment?.sourceTexture || new THREE.Color(0x0e0e10)) : new THREE.Color(0x0e0e10);
  scene.backgroundBlurriness = state.backgroundBlur;
  scene.backgroundIntensity = state.backgroundIntensity;
  grid.visible = state.groundVisible;
  groundPlane.visible = state.groundVisible;
  groundPlane.receiveShadow = state.environmentShadow && state.aov === 'beauty';
  // Keep the shadow camera stable over the full motion range even when the
  // view itself is fitted tightly to the current pose.
  const lightBounds = state.animationBounds?.clone()
    || state.framingBounds?.clone() || currentSceneBounds();
  if (!lightBounds.isEmpty()) {
    const center = lightBounds.getCenter(new THREE.Vector3());
    const radius = Math.max(lightBounds.getSize(new THREE.Vector3()).length() * 0.5, 0.1);
    updateShadowFrustum(lightBounds, center, radius);
  }
  key.intensity = 1.1 * state.environmentIntensity;
  world.traverse((object) => {
    if (object.isMesh) { object.castShadow = true; object.receiveShadow = true; }
    const materials = Array.isArray(object.material) ? object.material : [object.material];
    materials.forEach((material) => { if (material && 'envMapIntensity' in material) material.envMapIntensity = (environment?.intensity ?? 1) * state.environmentIntensity; });
  });
}
function activateEnvironment(environment) {
  state.activeEnvironment = environment;
  scene.environment = environment?.texture || null;
  world.traverse((object) => {
    const materials = Array.isArray(object.material) ? object.material : [object.material];
    materials.forEach((material) => {
      if (!material) return;
      if ('envMap' in material) material.envMap = scene.environment;
      if ('envMapIntensity' in material) material.envMapIntensity = (environment?.intensity ?? 1) * state.environmentIntensity;
      material.needsUpdate = true;
    });
  });
  updateEnvironmentDisplay();
}
function srgbByte(value) {
  const encoded = value <= 0.0031308 ? value * 12.92 : 1.055 * Math.pow(value, 1 / 2.4) - 0.055;
  return Math.round(Math.max(0, Math.min(1, encoded)) * 255);
}
function makeSyntheticSunSky() {
  const width = 256; const height = 128; const data = new Uint8Array(width * height * 4);
  const sun = new THREE.Vector3(-0.35, 0.78, 0.48).normalize();
  for (let y = 0; y < height; y++) {
    // Three's equirectangular background/PMREM paths address DataTexture rows
    // bottom-to-top. Generate in that storage order so sky remains above the
    // horizon in both paths without relying on DataTexture.flipY.
    const elevation = Math.PI * (y / (height - 1) - 0.5);
    for (let x = 0; x < width; x++) {
      const azimuth = (x / width - 0.5) * Math.PI * 2;
      const direction = new THREE.Vector3(Math.cos(elevation) * Math.sin(azimuth), Math.sin(elevation), Math.cos(elevation) * Math.cos(azimuth));
      const sky = Math.max(0, direction.y);
      const horizon = Math.pow(1 - Math.abs(direction.y), 2);
      let color = direction.y < 0 ? new THREE.Color(0.025, 0.03, 0.04) : new THREE.Color(0.08 + sky * 0.28 + horizon * 0.12, 0.16 + sky * 0.38 + horizon * 0.16, 0.28 + sky * 0.5 + horizon * 0.2);
      const sunAmount = Math.max(0, direction.dot(sun));
      const glow = Math.pow(sunAmount, 180) * 6 + Math.pow(sunAmount, 18) * 0.35;
      color = new THREE.Color(color.r + glow, color.g + glow * 0.82, color.b + glow * 0.45);
      const offset = (y * width + x) * 4;
      data[offset] = srgbByte(color.r); data[offset + 1] = srgbByte(color.g); data[offset + 2] = srgbByte(color.b); data[offset + 3] = 255;
    }
  }
  const sourceTexture = new THREE.DataTexture(data, width, height, THREE.RGBAFormat, THREE.UnsignedByteType);
  sourceTexture.colorSpace = THREE.SRGBColorSpace; sourceTexture.mapping = THREE.EquirectangularReflectionMapping; sourceTexture.needsUpdate = true;
  return sourceTexture;
}
function makeWhiteFurnace() {
  const sourceTexture = new THREE.DataTexture(new Uint8Array(32 * 16 * 4).fill(255), 32, 16, THREE.RGBAFormat, THREE.UnsignedByteType);
  sourceTexture.colorSpace = THREE.SRGBColorSpace; sourceTexture.mapping = THREE.EquirectangularReflectionMapping; sourceTexture.needsUpdate = true;
  return sourceTexture;
}
const environmentCache = new Map();
const environmentPromises = new Map();
async function loadEnvironmentPreset(key) {
  if (environmentCache.has(key)) return environmentCache.get(key);
  if (environmentPromises.has(key)) return environmentPromises.get(key);
  const promise = (async () => {
    let sourceTexture;
    if (key === 'sunsky') sourceTexture = makeSyntheticSunSky();
    else if (key === 'furnace') sourceTexture = makeWhiteFurnace();
    else {
      sourceTexture = await new HDRLoader().loadAsync('./assets/textures/goegap_1k.hdr');
      sourceTexture.mapping = THREE.EquirectangularReflectionMapping;
    }
    const environment = { sourceTexture, texture: pmremGenerator.fromEquirectangular(sourceTexture).texture, intensity: key === 'furnace' ? 0.75 : 1.0 };
    environmentCache.set(key, environment);
    return environment;
  })();
  environmentPromises.set(key, promise);
  try { return await promise; } finally { environmentPromises.delete(key); }
}
async function selectEnvironment(key) {
  state.environmentPreset = key;
  try {
    const environment = await loadEnvironmentPreset(key);
    if (!state.domeEnvironment && state.environmentPreset === key) activateEnvironment(environment);
  } catch (error) {
    console.warn(`Environment '${key}' unavailable:`, error);
  }
}
async function applyDomeEnvironment(layer) {
  state.domeEnvironment?.sourceTexture?.dispose?.();
  state.domeEnvironment?.texture?.dispose?.();
  state.domeEnvironment = null;
  if (!layer?.numLights || layer.numLights() === 0) {
    await selectEnvironment(state.environmentPreset);
    return;
  }
  try {
    const dome = await LightUSDLoaderUtils.loadDomeLightFromUSD(layer, pmremGenerator);
    if (!dome?.texture) {
      await selectEnvironment(state.environmentPreset);
      return;
    }
    state.domeEnvironment = dome;
    activateEnvironment({ ...dome, intensity: dome.intensity || 1 });
    status(`Using DomeLight environment${dome.name ? `: ${dome.name}` : ''}…`);
  } catch (error) {
    console.warn('DomeLight environment skipped:', error);
    await selectEnvironment(state.environmentPreset);
  }
}
function textureMimeType(uri, bytes) {
  const ext = String(uri || '').split(/[?#]/)[0].toLowerCase().split('.').pop();
  if (ext === 'jpg' || ext === 'jpeg') return 'image/jpeg';
  if (ext === 'png') return 'image/png';
  if (ext === 'webp') return 'image/webp';
  if (bytes?.[0] === 0xff && bytes?.[1] === 0xd8) return 'image/jpeg';
  if (bytes?.[0] === 0x89 && bytes?.[1] === 0x50) return 'image/png';
  if (bytes?.[0] === 0x52 && bytes?.[1] === 0x49 && bytes?.[2] === 0x46 && bytes?.[3] === 0x46) return 'image/webp';
  return '';
}
async function decodeBrowserTexture(textureId, layer, mapProperty) {
  const texture = layer.getTexture(textureId); const imageId = texture.textureImageId;
  const image = layer.getImageCopy(imageId); const uri = image.uri || '';
  const encoded = state.resolver?.getAsset(uri);
  const bytes = encoded ? new Uint8Array(encoded) : (!image.decoded && image.data ? new Uint8Array(image.data) : null);
  const mime = textureMimeType(uri, bytes);
  if (!bytes || !mime) return LightUSDLoaderUtils.getTextureFromUSD(layer, textureId, mapProperty);
  // Match the working web/js lazy path: let Three.js own image decoding and
  // upload through TextureLoader rather than uploading ImageBitmap directly.
  const blobUrl = URL.createObjectURL(new Blob([bytes.slice()], { type: mime }));
  try {
    const result = await new THREE.TextureLoader().loadAsync(blobUrl);
    // Keep the browser-decoded texture.  The optional RGBA readback/compression
    // path is useful for offline pipelines, but doing a canvas readback here
    // blocks the main thread for every large map and defeats lazy loading.
    return LightUSDLoaderUtils.applyTextureSampler(result, texture);
  } finally {
    URL.revokeObjectURL(blobUrl);
  }
}
function aovFragment(mode) {
  if (mode === 'baseColor') return 'vec4 lightusdAov = vec4( diffuseColor.rgb, 1.0 );';
  if (mode === 'normal') return 'vec4 lightusdAov = vec4( normalize( normal ) * 0.5 + 0.5, 1.0 );';
  if (mode === 'roughness') return 'vec4 lightusdAov = vec4( vec3( roughnessFactor ), 1.0 );';
  if (mode === 'metalness') return 'vec4 lightusdAov = vec4( vec3( metalnessFactor ), 1.0 );';
  if (mode === 'emission') return 'vec4 lightusdAov = vec4( totalEmissiveRadiance, 1.0 );';
  if (mode === 'opacity') return 'vec4 lightusdAov = vec4( vec3( diffuseColor.a ), 1.0 );';
  if (mode === 'occlusion') return `
float lightusdAovAO = 1.0;
#ifdef USE_AOMAP
  lightusdAovAO = ambientOcclusion;
#endif
vec4 lightusdAov = vec4( vec3( lightusdAovAO ), 1.0 );`;
  return '';
}
function installAOVHook(material) {
  if (!material || material.userData.lightusdAovHookInstalled) return;
  material.userData.lightusdAovHookInstalled = true;
  const previousCompile = material.onBeforeCompile?.bind(material);
  const previousCacheKey = material.customProgramCacheKey?.bind(material);
  material.onBeforeCompile = (shader, activeRenderer) => {
    if (previousCompile) previousCompile(shader, activeRenderer);
    const fragment = aovFragment(material.userData.lightusdAovMode);
    if (!fragment) return;
    shader.fragmentShader = shader.fragmentShader.replace(
      '#include <opaque_fragment>',
      `${fragment}\n${THREE.ShaderChunk.opaque_fragment}\ngl_FragColor = lightusdAov;`);
  };
  material.customProgramCacheKey = () => {
    const prior = previousCacheKey ? previousCacheKey() : '';
    return `${prior}|lightusd-aov:${material.userData.lightusdAovMode || 'beauty'}`;
  };
}
function updateMaterialDisplay(material) {
  if (!material) return;
  material.userData.lightusdAovMode = state.aov;
  installAOVHook(material);
  material.needsUpdate = true;
}
function updateAllMaterialDisplays() {
  world.traverse((object) => {
    const materials = Array.isArray(object.material) ? object.material : [object.material];
    materials.forEach(updateMaterialDisplay);
  });
}
function applyDisplayTransform() {
  renderer.outputColorSpace = state.toneMap === 'raw'
    ? THREE.LinearSRGBColorSpace : THREE.SRGBColorSpace;
  renderer.toneMapping = state.aov === 'beauty'
    ? TONE_MAPS[state.toneMap] : THREE.NoToneMapping;
  renderer.toneMappingExposure = Math.pow(2, state.exposureEV);
  updateAllMaterialDisplays();
}
function applyMaterialFeatures(material, mapProperty = '') {
  if (!material) return;
  applyUSDMaterialFeatures(material, mapProperty);
  updateMaterialDisplay(material);
}
async function buildScene(layer) {
  clearWorld(); state.clips = [];
  const metadata = getUSDSceneMetadata(layer);
  world.rotation.set(metadata.fileUpAxis === 'Z' ? -Math.PI / 2 : 0, 0, 0);
  world.updateMatrixWorld(true);
  state.animationFPS = metadata.framesPerSecond;
  stat('stat-fps', `${state.animationFPS} fps`);
  await applyDomeEnvironment(layer);
  const material = LightUSDLoaderUtils.createDefaultMaterial();
  const textureLoadingManager = new TextureLoadingManager();
  state.textureManager = textureLoadingManager;
  const rootNode = await LightUSDLoaderUtils.buildThreeNode(layer.getDefaultRootNode(), material, layer, { preferredMaterialType: state.material, envMap: scene.environment, computeMissingTangents: true, textureCache: new Map(), textureLoadingManager, onProgress: (info) => status(info.message || 'Building scene…') });
  world.add(rootNode);
  rootNode.traverse((object) => {
    const materials = Array.isArray(object.material) ? object.material : [object.material];
    materials.forEach((item) => applyMaterialFeatures(item));
  });
  textureLoadingManager.startLoading({
    concurrency: 4,
    yieldInterval: 16,
    onProgress: (info) => status(info.total ? `Decoding textures ${info.loaded + info.failed}/${info.total}…` : 'Building scene…'),
    loadTexture: (textureId, usdScene, mapProperty) => decodeBrowserTexture(textureId, usdScene, mapProperty),
    onTextureLoaded: (material, mapProperty) => applyMaterialFeatures(material, mapProperty)
  }).then((textureStatus) => {
    if (state.textureManager !== textureLoadingManager) return;
    if (textureStatus.failed) console.warn(`Skipped ${textureStatus.failed} texture(s).`);
    state.textureManager = null;
    fetchProgress(null);
  });
  try {
    const skin = extractSkinnedMeshData(layer, { logger: console, verbose: false }); const skel = buildSkeletonDataFromUSD(layer, { logger: console, hasSkinnedMeshData: skin.hasSkinnedMeshData });
    const nodeIndexMap = buildNodeIndexMap(rootNode);
    applyUSDSceneSkinningPipeline({ threeNode: rootNode, characterGroup: world, helperScene: scene, skeletonDataArray: skel.skeletonDataArray, allSkinnedMeshUSDData: skin.allSkinnedMeshUSDData, skinnedMeshDataByName: skin.skinnedMeshDataByName, usdScene: layer, showMesh: true, textureLoadingManager, logger: console });
    const anim = extractUSDSceneAnimations(layer, { boneMaps: skel.boneMaps, nodeIndexMap, timeCodesPerSecond: metadata.timeCodesPerSecond, logger: console }); state.clips = [...anim.usdAnimations, ...anim.usdNodeAnimations];
  } catch (error) { console.warn('Animation extraction skipped:', error); }
  if (state.clips.length) {
    state.mixer = new THREE.AnimationMixer(world); state.mixer.timeScale = state.animationFPS * state.playbackSpeed; state.actions = state.clips.map((clip) => state.mixer.clipAction(clip)); state.duration = Math.max(...state.clips.map((clip) => clip.duration), 0); $('clip-select').innerHTML = state.clips.map((clip, i) => `<option value="${i}">${clip.name || `Clip ${i}`}</option>`).join(''); $('timeline').hidden = false; state.actions[0]?.play(); state.playing = true; $('play-btn').textContent = 'Pause';
  }
  if (state.actions.length) animatedSceneBounds();
  fitScene('current'); updateStats(layer, state.record?.hasPayload ? 'references (payload deferred)' : 'references');
}
async function loadBytes(bytes, filename, resolver, budget, label) {
  const token = ++state.generation; state.budget = budget; state.resolver = resolver; state.sourceName = label || filename;
  beginDelayedLoadProgress(`Loading ${filename}`);
  try {
    // Paint the indeterminate indicator before entering a synchronous WASM
    // parse so long files never look like a frozen page.
    await new Promise((resolve) => requestAnimationFrame(() => resolve()));
    status(`Parsing ${filename}…`); await ensureLoader();
    const record = await composeLayer({ loader: state.loader, bytes, filename, resolver, composePayload: false, onStatus: status });
    if (token !== state.generation) { disposeRecord(record); resolver.clearCache?.(); return; }
    disposeRecord(state.record); state.record = record; status(`Building ${filename}…`); await buildScene(record.layer);
    if (token !== state.generation) return;
    status(`Loaded ${filename} — references ready.`);
  } finally {
    endDelayedLoadProgress();
  }
}
async function loadFile(file, files = [file]) {
  try { const budget = new AssetBudget(MAX_ASSET_BYTES); if (files.length > 1) for (const item of files) budget.claim(item.webkitRelativePath || item.name, item.size); const rootFile = files.length > 1 ? chooseRootFile(files) : file; if (files.length === 1) budget.claim(rootFile.name, rootFile.size); fetchProgress({ assetPath: rootFile.name, loaded: 0, total: rootFile.size }); const bytes = new Uint8Array(await rootFile.arrayBuffer()); fetchProgress({ assetPath: rootFile.name, loaded: bytes.byteLength, total: bytes.byteLength, done: true }); const resolver = new FileAssetResolver(files, budget, fetchProgress); await loadBytes(bytes, rootFile.name, resolver, budget, rootFile.webkitRelativePath || rootFile.name); }
  catch (error) { console.error(error); status(`Failed: ${error.message}`); }
}
async function loadURL(url) {
  try { const budget = new AssetBudget(MAX_ASSET_BYTES); const clean = new URL(url, document.baseURI); const resolver = new HttpAssetResolver(new URL('.', clean).href, budget, status, fetchProgress); const [, bytes] = await resolver.resolveAsync(clean.href); await loadBytes(bytes, clean.pathname.split('/').pop() || 'scene.usd', resolver, budget, clean.href); }
  catch (error) { console.error(error); status(`Failed: ${error.message}`); }
}
async function loadPayload() {
  if (!state.record?.hasPayload) return; try { status('Loading deferred payload…'); await state.record.composer.progressiveComposition({ composePayload: true }); await buildScene(state.record.layer); status('Loaded references and payload.'); } catch (error) { status(`Payload failed: ${error.message}`); }
}

$('open-btn').onclick = () => $('file-input').click(); $('folder-btn').onclick = () => $('folder-input').click(); $('clear-btn').onclick = resetViewer; $('fit-btn').onclick = () => fitScene('current'); $('payload-btn').onclick = loadPayload;
$('sample-select').onchange = (event) => { $('url-input').value = event.target.value; }; $('url-btn').onclick = () => { if ($('url-input').value.trim()) loadURL($('url-input').value.trim()); }; $('material-select').onchange = async (event) => { state.material = event.target.value; if (state.record) await buildScene(state.record.layer); }; $('environment-select').onchange = (event) => selectEnvironment(event.target.value);
$('environment-rotation').oninput = (event) => { state.environmentRotation = Number(event.target.value); $('environment-rotation-value').textContent = `${state.environmentRotation.toFixed(0)}°`; updateEnvironmentDisplay(); };
$('environment-intensity').oninput = (event) => { state.environmentIntensity = Number(event.target.value); $('environment-intensity-value').textContent = state.environmentIntensity.toFixed(2); updateEnvironmentDisplay(); };
$('background-intensity').oninput = (event) => { state.backgroundIntensity = Number(event.target.value); $('background-intensity-value').textContent = state.backgroundIntensity.toFixed(2); updateEnvironmentDisplay(); };
$('background-blur').oninput = (event) => { state.backgroundBlur = Number(event.target.value); $('background-blur-value').textContent = state.backgroundBlur.toFixed(2); updateEnvironmentDisplay(); };
$('background-visible').onchange = (event) => { state.backgroundVisible = event.target.checked; updateEnvironmentDisplay(); };
$('ground-visible').onchange = (event) => { state.groundVisible = event.target.checked; updateEnvironmentDisplay(); };
$('environment-shadow').onchange = (event) => { state.environmentShadow = event.target.checked; updateEnvironmentDisplay(); };
$('aov-select').onchange = (event) => { state.aov = event.target.value; applyDisplayTransform(); updateEnvironmentDisplay(); };
$('tone-map-select').onchange = (event) => { state.toneMap = event.target.value; applyDisplayTransform(); };
$('exposure').oninput = (event) => { state.exposureEV = Number(event.target.value); $('exposure-value').textContent = `${state.exposureEV.toFixed(1)} EV`; applyDisplayTransform(); };
$('file-input').onchange = () => { const f = $('file-input').files?.[0]; if (f) loadFile(f); $('file-input').value = ''; }; $('folder-input').onchange = () => { const files = [...($('folder-input').files || [])]; if (files.length) loadFile(chooseRootFile(files), files); $('folder-input').value = ''; };
const drop = $('drop-zone'); viewport.ondragover = (event) => { event.preventDefault(); drop.classList.add('active'); }; viewport.ondragleave = () => drop.classList.remove('active'); viewport.ondrop = (event) => { event.preventDefault(); drop.classList.remove('active'); const files = [...(event.dataTransfer?.files || [])]; if (files.length) loadFile(chooseRootFile(files), files); };
window.addEventListener('keydown', (event) => {
  if (event.ctrlKey || event.metaKey || event.altKey || event.repeat) return;
  const target = event.target;
  if (target instanceof HTMLInputElement || target instanceof HTMLSelectElement || target instanceof HTMLTextAreaElement) return;
  if (event.key.toLowerCase() === 'f') { event.preventDefault(); fitScene('current'); }
  if (event.key.toLowerCase() === 'a') { event.preventDefault(); fitScene('all'); }
});
$('play-btn').onclick = () => { state.playing = !state.playing; $('play-btn').textContent = state.playing ? 'Pause' : 'Play'; if (state.playing) state.actions.forEach((action) => action.play()); }; $('speed-select').onchange = (event) => { state.playbackSpeed = Number(event.target.value); if (state.mixer) state.mixer.timeScale = state.animationFPS * state.playbackSpeed; }; $('clip-select').onchange = (event) => { state.actions.forEach((action, i) => action.reset().stop()); state.actions[Number(event.target.value)]?.play(); }; $('scrub').oninput = (event) => {
  const time = Number(event.target.value) * state.duration;
  state.actions.forEach((action) => {
    const clipDuration = action.getClip?.().duration || state.duration;
    const lastSampleTime = Math.max(0, clipDuration - 1e-6);
    action.time = Math.min(time, lastSampleTime);
  });
  // Assigning AnimationAction.time alone does not update bound object values.
  // A zero-delta mixer evaluation applies the requested pose while paused
  // without advancing the USD time-code timeline.
  state.mixer?.update(0);
};

window.__usdViewer = { loadFile, loadURL, loadFolder: (files) => loadFile(chooseRootFile(files), files), state, world };
function resizeRenderer() {
  const width = Math.max(1, Math.floor(viewport.clientWidth || viewport.getBoundingClientRect().width));
  const height = Math.max(1, Math.floor(viewport.clientHeight || viewport.getBoundingClientRect().height));
  const pixelRatio = Math.min(window.devicePixelRatio || 1, 2);
  const targetWidth = Math.floor(width * pixelRatio);
  const targetHeight = Math.floor(height * pixelRatio);
  if (renderer.domElement.width !== targetWidth || renderer.domElement.height !== targetHeight) {
    renderer.setSize(width, height, false);
    renderer.setViewport(0, 0, width, height);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
  }
}
window.addEventListener('resize', resizeRenderer);
const resizeObserver = new ResizeObserver(resizeRenderer); resizeObserver.observe(viewport);
const timer = new Timer(); timer.connect(document);
function animate(timestamp) { requestAnimationFrame(animate); timer.update(timestamp); const dt = Math.min(timer.getDelta(), 0.05); if (state.playing) { state.mixer?.update(dt); if (state.duration) $('scrub').value = String((state.actions[0]?.time || 0) / state.duration); } controls.update(); resizeRenderer(); renderer.render(scene, camera); }
void selectEnvironment('goegap');
applyDisplayTransform();
animate();
