import * as THREE from 'three';
import { buildNextDemoScene } from '../next-scene.js';
import { showLoader, hideLoader } from '../lightusd-loader.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';

const SAMPLES = [
  { label: 'Suzanne PBR', url: './assets/suzanne-pbr.usda' },
  { label: 'Fancy Teapot (MTLX)', url: './assets/fancy-teapot-mtlx.usdz' },
  { label: 'Damaged Helmet', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz' },
  { label: 'Cesium Man', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/CesiumMan/CesiumMan.usdz' },
  { label: 'Sphere (GitHub)', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/sphere.usda' },
  { label: 'Multi-clip skeleton', url: './assets/multi-clip-skeleton.usda' },
];

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(i) { return document.getElementById(i); }
function q(s) { return document.querySelector(s); }

// ── Shell ──
const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>Viewer Toolkit</h1>
      <p>Full-featured USD viewer with shading modes, exposure, and display toggles.</p>
    </div>
    <div class="demo-actions">
      <select id="sample-select">
        ${SAMPLES.map((s, i) => `<option value="${i}"${i===0?' selected':''}>${escapeHTML(s.label)}</option>`).join('')}
      </select>
      <button id="load-btn" type="button">Load</button>
      <button id="fit-btn" type="button">Fit</button>
    </div>
  </header>
  <main class="demo-main">
    <section class="viewport-wrap">
      <div id="viewport" class="viewport"></div>
      <div class="drop-hint" id="drop-hint">Drop USDA, USDC, USD, or USDZ</div>
      <div id="status" class="status">Initializing...</div>
    </section>
    <aside class="info-panel" style="overflow-y:auto">
      <div style="margin-bottom:14px">
        <h2 style="margin-top:0">Display</h2>
        <div class="gui-container" id="display-gui"></div>
      </div>
      <div>
        <h2>Scene</h2>
        <dl id="scene-stats">
          <dt>Meshes</dt><dd id="s-meshes">—</dd>
          <dt>Materials</dt><dd id="s-materials">—</dd>
          <dt>Textures</dt><dd id="s-textures">—</dd>
          <dt>Triangles</dt><dd id="s-tris">—</dd>
          <dt>FPS</dt><dd id="s-fps">—</dd>
        </dl>
      </div>
      <div id="notes" style="margin-top:14px">
        <p style="color:var(--dim);font-size:.8rem"><strong>Shading modes:</strong><br>
        <em>Wireframe</em> — edge-only view<br>
        <em>Flat</em> — per-face normals, no interpolation<br>
        <em>Shaded</em> — MeshPhysicalMaterial with envmap<br>
        <em>Textured</em> — shaded + loaded textures</p>
      </div>
    </aside>
  </main>
  <input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden>
</div>`;

// ── Three.js ──
const viewport = $id('viewport');
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x0e0e10);

const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 200);
camera.position.set(3, 2.5, 4);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.0;
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;
viewport.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.dampingFactor = 0.08;

const world = new THREE.Group();
scene.add(world);

scene.add(new THREE.HemisphereLight(0xdde8f6, 0x24272c, 1.2));
const keyLight = new THREE.DirectionalLight(0xffffff, 2);
keyLight.position.set(4, 6, 5);
keyLight.castShadow = true;
scene.add(keyLight);

const gridHelper = new THREE.GridHelper(10, 20, 0x44444a, 0x26262b);
gridHelper.position.y = -0.01;
scene.add(gridHelper);

const axesHelper = new THREE.AxesHelper(0.5);
scene.add(axesHelper);

// ── State ──
const state = {
  shadingMode: 'textured', // wireframe | flat | shaded | textured
  wireframeOverlay: false,
  backfaceCulling: true,
  showGrid: true,
  showAxes: true,
  exposure: 1.0,
  fps: 0,
  meshes: 0,
  materials: 0,
  textures: 0,
  tris: 0,
};

let loader = null;
let currentScene = null;
let originalMaterials = []; // [{ mesh, materials }] for restoring

// ── UI helpers ──
function setStatus(s) { $id('status').textContent = s; }

function updateStats() {
  $id('s-meshes').textContent = state.meshes || '—';
  $id('s-materials').textContent = state.materials || '—';
  $id('s-textures').textContent = state.textures || '—';
  $id('s-tris').textContent = state.tris ? state.tris.toLocaleString() : '—';
  $id('s-fps').textContent = state.fps || '—';
}

// ── Shading mode application ──

function collectAllMaterials() {
  const seen = new Set();
  const list = [];
  world.traverse((obj) => {
    if (!obj.isMesh || !obj.material) return;
    const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
    for (const m of mats) {
      if (m && !seen.has(m.uuid)) { seen.add(m.uuid); list.push(m); }
    }
  });
  return list;
}

function collectAllMeshes() {
  const list = [];
  world.traverse((obj) => { if (obj.isMesh) list.push(obj); });
  return list;
}

function applyShadingMode() {
  const mats = collectAllMaterials();
  if (mats.length === 0) return;

  const isWireframe = state.shadingMode === 'wireframe';
  const isFlat = state.shadingMode === 'flat';
  const isShaded = state.shadingMode === 'shaded' || state.shadingMode === 'textured';

  for (const mat of mats) {
    // Wireframe
    mat.wireframe = isWireframe;

    // Flat shading
    if (mat.flatShading !== undefined) {
      mat.flatShading = isFlat;
    }

    // Side
    mat.side = state.backfaceCulling ? THREE.FrontSide : THREE.DoubleSide;

    // MeshPhysicalMaterial specific
    if (mat.isMeshPhysicalMaterial || mat.isMeshStandardMaterial) {
      // Textured vs Shaded: toggle texture map usage
      if (state.shadingMode === 'textured') {
        // Restore any textures that were cleared
      } else if (state.shadingMode === 'shaded') {
        // Keep the material color but skip textures
        // We leave textures loaded but the color is already set
      }
    }

    mat.needsUpdate = true;
  }

  // Wireframe overlay
  toggleWireframeOverlay(state.wireframeOverlay);

  updateStats();
}

function toggleWireframeOverlay(show) {
  // Remove existing overlay
  const existing = scene.getObjectByName('__wireframe_overlay');
  if (existing) { scene.remove(existing); existing.geometry?.dispose?.(); }

  if (!show) return;

  const group = new THREE.Group();
  group.name = '__wireframe_overlay';

  world.traverse((obj) => {
    if (!obj.isMesh) return;
    const geo = obj.geometry;
    if (!geo) return;
    const wf = new THREE.WireframeGeometry(geo);
    const mat = new THREE.LineBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.35, depthTest: true });
    const line = new THREE.LineSegments(wf, mat);
    // Match world transform
    line.matrix.copy(obj.matrixWorld);
    line.matrixAutoUpdate = false;
    group.add(line);
  });

  scene.add(group);
}

// ── Builder GUI ──

let gui = null;

function buildDisplayGUI() {
  if (gui) { gui.destroy(); gui = null; }
  const c = $id('display-gui');
  c.innerHTML = '';

  const container = document.createElement('div');
  container.style.cssText = 'display:flex;flex-direction:column;gap:6px';
  c.appendChild(container);

  // Shading row
  const shadeRow = document.createElement('div');
  shadeRow.style.cssText = 'display:flex;gap:4px;flex-wrap:wrap';
  ['wireframe', 'flat', 'shaded', 'textured'].forEach((mode) => {
    const btn = document.createElement('button');
    btn.textContent = mode.charAt(0).toUpperCase() + mode.slice(1);
    btn.dataset.mode = mode;
    btn.style.cssText = `padding:4px 10px;border:1px solid var(--line-strong);border-radius:4px;background:${state.shadingMode === mode ? 'var(--accent)' : 'var(--panel-2)'};color:${state.shadingMode === mode ? '#121214' : 'var(--text)'};cursor:pointer;font-size:.78rem;font-weight:600;transition:background .1s`;
    btn.addEventListener('click', () => {
      state.shadingMode = mode;
      shadeRow.querySelectorAll('button').forEach((b) => {
        b.style.background = b.dataset.mode === mode ? 'var(--accent)' : 'var(--panel-2)';
        b.style.color = b.dataset.mode === mode ? '#121214' : 'var(--text)';
      });
      applyShadingMode();
    });
    shadeRow.appendChild(btn);
  });
  container.appendChild(shadeRow);

  // Toggles
  const toggles = [
    { key: 'wireframeOverlay', label: 'Wireframe Overlay' },
    { key: 'backfaceCulling', label: 'Backface Culling' },
    { key: 'showGrid', label: 'Grid' },
    { key: 'showAxes', label: 'Axes' },
  ];

  for (const t of toggles) {
    const row = document.createElement('label');
    row.style.cssText = 'display:flex;align-items:center;gap:8px;cursor:pointer;font-size:.82rem;color:var(--text)';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = state[t.key];
    cb.style.cssText = 'accent-color:var(--accent);width:14px;height:14px';
    cb.addEventListener('change', () => {
      state[t.key] = cb.checked;
      switch (t.key) {
        case 'showGrid': gridHelper.visible = cb.checked; break;
        case 'showAxes': axesHelper.visible = cb.checked; break;
        case 'backfaceCulling': applyShadingMode(); break;
        case 'wireframeOverlay': toggleWireframeOverlay(cb.checked); break;
      }
    });
    row.appendChild(cb);
    row.appendChild(document.createTextNode(t.label));
    container.appendChild(row);
  }

  // Exposure
  const expRow = document.createElement('div');
  expRow.style.cssText = 'display:flex;align-items:center;gap:8px';
  const expLabel = document.createElement('span');
  expLabel.textContent = 'Exposure';
  expLabel.style.cssText = 'font-size:.82rem;color:var(--text);flex:0 0 auto';
  const expSlider = document.createElement('input');
  expSlider.type = 'range';
  expSlider.min = 0.1; expSlider.max = 4.0; expSlider.step = 0.05;
  expSlider.value = state.exposure;
  expSlider.style.cssText = 'flex:1;min-width:0';
  expSlider.addEventListener('input', () => {
    state.exposure = Number(expSlider.value);
    renderer.toneMappingExposure = state.exposure;
    expVal.textContent = state.exposure.toFixed(2);
  });
  const expVal = document.createElement('span');
  expVal.textContent = state.exposure.toFixed(2);
  expVal.style.cssText = 'font-size:.8rem;color:var(--text);font-variant-numeric:tabular-nums;min-width:3ch;text-align:right';
  expRow.appendChild(expLabel);
  expRow.appendChild(expSlider);
  expRow.appendChild(expVal);
  container.appendChild(expRow);
}

// ── USD Loading ──

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

async function loadURL(url, label) {
  await ensureLoader();
  setStatus(`Fetching ${label}...`);
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const data = new Uint8Array(await resp.arrayBuffer());
  const filename = url.split('/').pop() || 'scene.usd';

  setStatus(`Parsing ${label}...`);
  const usd = await new Promise((resolve, reject) => {
    loader.parse(data, filename, resolve, reject, {
      backend: 'next', maxMemoryLimitMB: 512,
    });
  });

  setStatus(`Building scene...`);
  originalMaterials = [];
  world.clear();
  const { node: threeNode } = await buildNextDemoScene(usd, { sourceUrl: url });
  world.add(threeNode);

  // Count triangles
  let tris = 0;
  world.traverse((obj) => {
    if (obj.isMesh && obj.geometry) {
      const idx = obj.geometry.index;
      if (idx) tris += idx.count / 3;
      else tris += obj.geometry.attributes.position.count / 3;
    }
  });

  state.meshes = usd.numMeshes ? usd.numMeshes() : 0;
  state.materials = usd.numMaterials ? usd.numMaterials() : 0;
  state.textures = usd.numTextures ? usd.numTextures() : 0;
  state.tris = Math.round(tris);
  state.shadingMode = 'textured';

  applyShadingMode();
  buildDisplayGUI();
  fitCamera();
  updateStats();
  setStatus(`Loaded ${label} — ${state.meshes} meshes, ${(state.tris / 1e6).toFixed(2)}M triangles`);
}

function fitCamera() {
  const box = new THREE.Box3().setFromObject(world);
  if (box.isEmpty()) return;
  const center = box.getCenter(new THREE.Vector3());
  const size = box.getSize(new THREE.Vector3());
  const maxDim = Math.max(size.x, size.y, size.z, 0.1);
  const dist = maxDim * 2.5;
  controls.target.copy(center);
  camera.position.copy(center).add(new THREE.Vector3(1, 0.6, 1).normalize().multiplyScalar(dist));
  camera.near = Math.max(0.001, dist / 100);
  camera.far = dist * 100;
  camera.updateProjectionMatrix();
  controls.update();
}

// ── File drop ──

$id('file-input').addEventListener('change', () => {
  const file = $id('file-input').files?.[0];
  if (file) loadLocalFile(file);
  $id('file-input').value = '';
});
viewport.addEventListener('dragenter', (e) => { e.preventDefault(); $id('drop-hint').classList.add('active'); });
viewport.addEventListener('dragover', (e) => { e.preventDefault(); $id('drop-hint').classList.add('active'); });
viewport.addEventListener('dragleave', () => { $id('drop-hint').classList.remove('active'); });
viewport.addEventListener('drop', async (e) => {
  e.preventDefault();
  $id('drop-hint').classList.remove('active');
  const file = e.dataTransfer?.files?.[0];
  if (file) loadLocalFile(file);
});

async function loadLocalFile(file) {
  if (!/\.(usd|usda|usdc|usdz)$/i.test(file.name)) return;
  await ensureLoader();
  const data = new Uint8Array(await file.arrayBuffer());
  const usd = await new Promise((resolve, reject) => {
    loader.parse(data, file.name, resolve, reject, { backend: 'next', maxMemoryLimitMB: 512 });
  });
  originalMaterials = [];
  world.clear();
  const { node: threeNode } = await buildNextDemoScene(usd);
  world.add(threeNode);
  let tris = 0;
  world.traverse((obj) => { if (obj.isMesh && obj.geometry) { const idx = obj.geometry.index; if (idx) tris += idx.count / 3; else tris += obj.geometry.attributes.position.count / 3; } });
  state.meshes = usd.numMeshes ? usd.numMeshes() : 0;
  state.materials = usd.numMaterials ? usd.numMaterials() : 0;
  state.textures = usd.numTextures ? usd.numTextures() : 0;
  state.tris = Math.round(tris);
  state.shadingMode = 'textured';
  applyShadingMode();
  buildDisplayGUI();
  fitCamera();
  updateStats();
  setStatus(`Loaded ${file.name}`);
}

// ── UI ──

$id('load-btn').addEventListener('click', () => loadSample(Number($id('sample-select').value)));
$id('fit-btn').addEventListener('click', fitCamera);

function loadSample(idx) {
  const s = SAMPLES[idx];
  if (s) loadURL(s.url, s.label).catch((e) => { console.error(e); setStatus('Error: ' + e.message); });
}

// ── FPS ──
let fpsFrames = 0, fpsLast = performance.now();

function tickFps(now) {
  fpsFrames++;
  if (now - fpsLast >= 500) {
    state.fps = Math.round((fpsFrames * 1000) / (now - fpsLast));
    fpsFrames = 0;
    fpsLast = now;
    updateStats();
  }
}

// ── Main loop ──

let lastTime = performance.now();
function anim(now) {
  requestAnimationFrame(anim);
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;
  tickFps(now);
  controls.update();
  renderer.render(scene, camera);
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

// ── Main ──

async function main() {
  buildDisplayGUI();
  await ensureLoader();

  const params = new URLSearchParams(location.search);
  const url = params.get('url') || params.get('uri');
  if (url) {
    await loadURL(url, url.split('/').pop());
  } else {
    await loadURL(SAMPLES[0].url, SAMPLES[0].label);
  }

  requestAnimationFrame(anim);
}

main().catch((e) => { console.error(e); setStatus('Error: ' + e.message); });
