import * as THREE from 'three';
import { buildNextDemoScene } from '../next-scene.js';
import { showLoader, hideLoader } from '../lightusd-loader.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { GUI } from 'lil-gui';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';

const SAMPLE_ASSETS = [
  { label: 'Suzanne PBR', url: './assets/suzanne-pbr.usda' },
  { label: 'Fancy Teapot (MTLX)', url: './assets/fancy-teapot-mtlx.usdz' },
  { label: 'Shader Ball', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/full_assets/StandardShaderBall/standard_shader_ball_scene.usda' },
];

function escapeHTML(v) {
  return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
}

// ── Shell ──
const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>Material Editor</h1>
      <p>Load a USD scene and edit PBR material parameters in real time.
        Changes apply instantly to Three.js MeshPhysicalMaterial.</p>
    </div>
    <div class="demo-actions">
      <select id="asset-select">
        ${SAMPLE_ASSETS.map((a, i) => `<option value="${i}"${i === 0 ? ' selected' : ''}>${escapeHTML(a.label)}</option>`).join('')}
      </select>
      <button id="load-btn" type="button">Load</button>
      <button id="fit-btn" type="button">Fit</button>
    </div>
  </header>
  <main class="demo-main">
    <section class="viewport-wrap">
      <div id="viewport" class="viewport"></div>
      <div id="drop-hint" class="drop-hint active" style="display:none">Drop USDA, USDC, USD, or USDZ</div>
      <div id="status" class="status">Initializing...</div>
    </section>
    <aside class="info-panel" style="overflow-y:auto">
      <h2>Material Properties</h2>
      <div id="gui-container" class="gui-container"></div>
      <h2>Scene</h2>
      <dl id="scene-stats">
        <dt>Meshes</dt><dd id="s-meshes">—</dd>
        <dt>Materials</dt><dd id="s-materials">—</dd>
        <dt>Textures</dt><dd id="s-textures">—</dd>
      </dl>
      <h2>Notes</h2>
      <div id="notes">
        <p>Select a material folder to expand it. Each PBR parameter
          (<em>color, metalness, roughness, clearcoat, ior</em>, etc.)
          can be edited with sliders or color pickers.</p>
        <p>Parameters are read from the USD material and mapped to
          Three.js MeshPhysicalMaterial properties.</p>
      </div>
    </aside>
  </main>
  <input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden>
</div>`;

// ── DOM ──
const $ = (id) => document.getElementById(id);
const viewport = $('viewport');
const statusEl = $('status');
const guiContainer = $('gui-container');
const meshesEl = $('s-meshes');
const matsEl = $('s-materials');
const texEl = $('s-textures');
const dropHint = $('drop-hint');
const fileInput = $('file-input');

function setStatus(s) { statusEl.textContent = s; }

// ── Three.js ──
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x0e0e10);

const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 200);
camera.position.set(3, 2.5, 4);

const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.0;
renderer.shadowMap.enabled = true;
viewport.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.dampingFactor = 0.08;
controls.target.set(0, 0, 0);

const pmremGenerator = new THREE.PMREMGenerator(renderer);

let envMap = null;
let world = new THREE.Group();
world.name = 'SceneRoot';
scene.add(world);

const ambLight = new THREE.AmbientLight(0xffffff, 0.35);
const keyLight = new THREE.DirectionalLight(0xffffff, 2.0);
keyLight.position.set(4, 6, 5);
keyLight.castShadow = true;
keyLight.shadow.mapSize.set(1024, 1024);
const fillLight = new THREE.DirectionalLight(0x9fb7ff, 0.5);
fillLight.position.set(-4, 3, -3);
scene.add(ambLight, keyLight, fillLight);

const grid = new THREE.GridHelper(10, 20, 0x44444a, 0x26262b);
grid.position.y = -0.01;
scene.add(grid);

let gui = null;

// ── Loaders ──
let loader = null;

async function ensureLoader() {
  if (loader) return loader;
  setStatus('Initializing LightUSD WASM...');
  loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512 });
    showLoader('Loading LightUSD WASM...', document.getElementById('viewport'));
  await loader.init({ useZstdCompressedWasm: false, useMemory64: false, backend: 'next' });
    hideLoader();
  LightUSDLoaderUtils.setLightUSD(loader.native_);
  return loader;
}

async function loadEnv() {
  try {
    const tex = await new HDRLoader().loadAsync('./assets/textures/goegap_1k.hdr');
    tex.mapping = THREE.EquirectangularReflectionMapping;
    envMap = pmremGenerator.fromEquirectangular(tex).texture;
    tex.dispose();
  } catch { /* no env */ }
}

// ── Material Editor ──

function getMaterialParams(material) {
  const c = material.color;
  return {
    color: [c.r, c.g, c.b],
    metalness: material.metalness ?? 0,
    roughness: material.roughness ?? 0.5,
    clearcoat: material.clearcoat ?? 0,
    clearcoatRoughness: material.clearcoatRoughness ?? 0,
    ior: material.ior ?? 1.5,
    opacity: material.opacity ?? 1,
    emissive: [material.emissive.r, material.emissive.g, material.emissive.b],
    emissiveIntensity: material.emissiveIntensity ?? 1,
  };
}

function setMaterialParam(material, key, value) {
  switch (key) {
    case 'color': material.color.setRGB(value[0], value[1], value[2]); break;
    case 'emissive': material.emissive.setRGB(value[0], value[1], value[2]); break;
    case 'metalness': material.metalness = value; break;
    case 'roughness': material.roughness = value; break;
    case 'clearcoat': material.clearcoat = value; break;
    case 'clearcoatRoughness': material.clearcoatRoughness = value; break;
    case 'ior': material.ior = value; break;
    case 'opacity': material.opacity = value; material.transparent = value < 1; break;
    case 'emissiveIntensity': material.emissiveIntensity = value; break;
  }
  material.needsUpdate = true;
}

function buildGUI(materials) {
  if (gui) { gui.destroy(); gui = null; }
  guiContainer.innerHTML = '';
  if (materials.length === 0) {
    guiContainer.innerHTML = '<p style="color:var(--dim);font-size:0.85rem">No materials found.</p>';
    return;
  }

  gui = new GUI({ container: guiContainer, width: 320 });

  // Global folder
  const global = gui.addFolder('Global');
  global.add({ fit: () => fitCamera() }, 'fit').name('Fit Scene');
  global.add(renderer, 'toneMappingExposure', 0.1, 4, 0.01).name('Exposure').onChange(() => renderer.toneMappingExposure = renderer.toneMappingExposure);
  global.open();

  for (let i = 0; i < materials.length; i++) {
    const mat = materials[i];
    const label = mat.name || `Material ${i}`;
    const folder = gui.addFolder(label);
    const params = getMaterialParams(mat);

    folder.addColor(params, 'color').name('Base Color').onChange((v) => setMaterialParam(mat, 'color', v));
    folder.add(params, 'metalness', 0, 1, 0.01).name('Metalness').onChange((v) => setMaterialParam(mat, 'metalness', v));
    folder.add(params, 'roughness', 0, 1, 0.01).name('Roughness').onChange((v) => setMaterialParam(mat, 'roughness', v));
    folder.add(params, 'clearcoat', 0, 1, 0.01).name('Clearcoat').onChange((v) => setMaterialParam(mat, 'clearcoat', v));
    folder.add(params, 'clearcoatRoughness', 0, 1, 0.01).name('Clearcoat Roughness').onChange((v) => setMaterialParam(mat, 'clearcoatRoughness', v));
    folder.add(params, 'ior', 1, 2.5, 0.01).name('IOR').onChange((v) => setMaterialParam(mat, 'ior', v));
    folder.add(params, 'opacity', 0, 1, 0.01).name('Opacity').onChange((v) => setMaterialParam(mat, 'opacity', v));
    folder.addColor(params, 'emissive').name('Emissive').onChange((v) => setMaterialParam(mat, 'emissive', v));
    folder.add(params, 'emissiveIntensity', 0, 10, 0.1).name('Emissive Intensity').onChange((v) => setMaterialParam(mat, 'emissiveIntensity', v));
    folder.open();
  }
}

function collectMaterials() {
  const seen = new Set();
  const unique = [];
  world.traverse((obj) => {
    if (!obj.isMesh || !obj.material) return;
    const list = Array.isArray(obj.material) ? obj.material : [obj.material];
    for (const mat of list) {
      if (mat && !seen.has(mat.uuid)) {
        seen.add(mat.uuid);
        unique.push(mat);
      }
    }
  });
  return unique;
}

function rebuildGUI() {
  const mats = collectMaterials();
  meshesEl.textContent = String(collectMeshes().length);
  matsEl.textContent = String(mats.length);
  buildGUI(mats);
}

function collectMeshes() {
  const meshes = [];
  world.traverse((obj) => { if (obj.isMesh) meshes.push(obj); });
  return meshes;
}

// ── USD Loading ──

async function loadUSD(url, label) {
  setStatus(`Fetching ${label}...`);
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const data = new Uint8Array(await resp.arrayBuffer());
  const filename = url.split('/').pop() || 'scene.usd';

  setStatus(`Parsing ${label}...`);
  const usd = await new Promise((resolve, reject) => {
    loader.parse(data, filename, resolve, reject, {
      backend: 'next',
      maxMemoryLimitMB: 512,
      preferredMaterialType: 'usdpreviewsurface',
    });
  });

  setStatus(`Building scene...`);
  const { node: threeNode } = await buildNextDemoScene(usd, { envMap, sourceUrl: url });

  // Replace world
  world.clear();
  world.add(threeNode);
  scene.environment = envMap;

  rebuildGUI();
  texEl.textContent = String(usd.numTextures ? usd.numTextures() : 0);
  fitCamera();
  setStatus(`Loaded ${label} — ${collectMeshes().length} meshes, ${collectMaterials().length} materials`);
}

function fitCamera() {
  const box = new THREE.Box3().setFromObject(world);
  if (box.isEmpty()) return;
  const center = box.getCenter(new THREE.Vector3());
  const size = box.getSize(new THREE.Vector3());
  const maxDim = Math.max(size.x, size.y, size.z, 0.1);
  const dist = maxDim * 2.2;
  controls.target.copy(center);
  camera.position.copy(center).add(new THREE.Vector3(0.8, 0.55, 1).normalize().multiplyScalar(dist));
  camera.near = Math.max(0.001, dist / 100);
  camera.far = dist * 100;
  camera.updateProjectionMatrix();
  controls.update();
}

// ── File drop ──

function setupFileDrop() {
  fileInput.addEventListener('change', () => {
    const file = fileInput.files?.[0];
    if (file) loadLocalFile(file);
    fileInput.value = '';
  });
  viewport.addEventListener('dragenter', (e) => { e.preventDefault(); dropHint.style.display = 'flex'; });
  viewport.addEventListener('dragover', (e) => { e.preventDefault(); dropHint.style.display = 'flex'; });
  viewport.addEventListener('dragleave', () => { dropHint.style.display = 'none'; });
  viewport.addEventListener('drop', async (e) => {
    e.preventDefault();
    dropHint.style.display = 'none';
    const file = e.dataTransfer?.files?.[0];
    if (file) loadLocalFile(file);
  });
}

async function loadLocalFile(file) {
  if (!/\.(usd|usda|usdc|usdz)$/i.test(file.name)) return;
  const data = new Uint8Array(await file.arrayBuffer());
  setStatus(`Reading ${file.name}...`);
  const usd = await new Promise((resolve, reject) => {
    loader.parse(data, file.name, resolve, reject, {
      backend: 'next',
      maxMemoryLimitMB: 512,
      preferredMaterialType: 'usdpreviewsurface',
    });
  });
  const { node: threeNode } = await buildNextDemoScene(usd, { envMap });
  world.clear();
  world.add(threeNode);
  scene.environment = envMap;
  rebuildGUI();
  fitCamera();
  setStatus(`Loaded ${file.name}`);
}

// ── UI ──

$('load-btn').addEventListener('click', () => {
  const idx = Number($('asset-select').value);
  const asset = SAMPLE_ASSETS[idx];
  if (asset) loadUSD(asset.url, asset.label);
});
$('fit-btn').addEventListener('click', fitCamera);

// ── Main ──

async function main() {
  setupFileDrop();
  await loadEnv();
  await ensureLoader();
  scene.environment = envMap;
  setStatus('Select an asset and click Load, or drag-and-drop a USD file.');

  // Auto-load first asset
  const params = new URLSearchParams(location.search);
  const urlParam = params.get('url') || params.get('uri');
  if (urlParam) {
    await loadUSD(urlParam, urlParam.split('/').pop());
  } else {
    const first = SAMPLE_ASSETS[0];
    await loadUSD(first.url, first.label);
  }

  let lastTime = performance.now();
  function anim(now) {
    requestAnimationFrame(anim);
    const dt = Math.min(0.05, (now - lastTime) / 1000);
    lastTime = now;
    controls.update();
    renderer.render(scene, camera);
  }
  requestAnimationFrame(anim);
}

function onResize() {
  const rect = viewport.getBoundingClientRect();
  const w = Math.max(1, rect.width);
  const h = Math.max(1, rect.height);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
  renderer.setSize(w, h, false);
}
window.addEventListener('resize', onResize);

main().catch((e) => { console.error(e); setStatus(`Error: ${e.message}`); });
