import * as THREE from 'three';
import { buildNextDemoScene } from '../next-scene.js';
import { showLoader, hideLoader } from '../lightusd-loader.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import { parseUSDZEntries } from 'lightusd-js/src/usdzconvert.js';

const SAMPLES = [
  { label: 'Suzanne PBR', url: './assets/suzanne-pbr.usda' },
  { label: 'Fancy Teapot (MTLX)', url: './assets/fancy-teapot-mtlx.usdz' },
  { label: 'Damaged Helmet', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz' },
  { label: 'Multi-clip skeleton', url: './assets/multi-clip-skeleton.usda' },
  { label: 'Sphere (simple)', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/sphere.usda' },
];

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(i) { return document.getElementById(i); }
function fmtBytes(b) {
  if (!b) return '—';
  if (b > 1048576) return (b / 1048576).toFixed(2) + ' MB';
  if (b > 1024) return (b / 1024).toFixed(1) + ' KB';
  return b + ' B';
}

// ── Shell ──
document.getElementById('demo-root').innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>USDZ Packager</h1>
      <p>Load a USD scene, inspect its embedded assets, and export as USDA, USDC, or USDZ.</p>
    </div>
    <div class="demo-actions">
      <select id="sample-select">
        ${SAMPLES.map((s, i) => `<option value="${i}"${i===0?' selected':''}>${escapeHTML(s.label)}</option>`).join('')}
      </select>
      <button id="load-btn" type="button">Load</button>
      <button id="fit-btn" type="button">Fit</button>
    </div>
  </header>
  <main class="assets-main" style="grid-template-columns:minmax(0,1fr) minmax(320px,420px)">
    <section class="assets-viewport-wrap">
      <div class="assets-canvas-wrap">
        <canvas id="pack-canvas"></canvas>
        <div class="assets-viewer-overlay" id="viewer-overlay">
          <div class="assets-viewer-placeholder" id="viewer-placeholder">
            <p>Select a sample and click Load.</p>
          </div>
        </div>
      </div>
      <div class="assets-status" id="pack-status">Ready.</div>
    </section>
    <aside class="viz-panel" style="overflow-y:auto;display:flex;flex-direction:column;gap:10px;padding:12px;background:var(--panel);border-left:1px solid var(--line)">
      <div class="viz-section">
        <h3 style="margin:0 0 6px;color:var(--muted);font-size:.72rem;font-weight:700;text-transform:uppercase;letter-spacing:.04em">Scene</h3>
        <div class="info-grid" style="display:grid;grid-template-columns:auto 1fr;gap:3px 12px;font-size:.78rem">
          <span style="color:var(--dim)">Meshes</span><span id="s-meshes" style="color:var(--text);font-variant-numeric:tabular-nums">—</span>
          <span style="color:var(--dim)">Materials</span><span id="s-mats" style="color:var(--text)">—</span>
          <span style="color:var(--dim)">Textures</span><span id="s-tex" style="color:var(--text)">—</span>
          <span style="color:var(--dim)">Triangles</span><span id="s-tris" style="color:var(--text)">—</span>
        </div>
      </div>
      <div class="viz-section">
        <h3 style="margin:0 0 6px;color:var(--muted);font-size:.72rem;font-weight:700;text-transform:uppercase;letter-spacing:.04em">Export</h3>
        <div style="display:flex;gap:6px;flex-wrap:wrap">
          <button id="export-usda" class="export-btn" style="flex:1;padding:7px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);cursor:pointer;font-size:.78rem;font-weight:600">USDA</button>
          <button id="export-usdc" class="export-btn" style="flex:1;padding:7px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);cursor:pointer;font-size:.78rem;font-weight:600">USDC</button>
          <button id="export-usdz" class="export-btn" style="flex:1;padding:7px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);cursor:pointer;font-size:.78rem;font-weight:600">USDZ</button>
        </div>
        <div style="margin-top:4px;font-size:.74rem;color:var(--dim)" id="export-sizes">No export yet.</div>
      </div>
      <div class="viz-section">
        <h3 style="margin:0 0 6px;color:var(--muted);font-size:.72rem;font-weight:700;text-transform:uppercase;letter-spacing:.04em">Textures</h3>
        <div id="texture-list" style="font-size:.76rem;color:var(--dim)">Load a scene to see textures.</div>
      </div>
      <div class="viz-section">
        <h3 style="margin:0 0 6px;color:var(--muted);font-size:.72rem;font-weight:700;text-transform:uppercase;letter-spacing:.04em">USDZ Contents</h3>
        <div id="usdz-contents" style="font-size:.76rem;color:var(--dim)">Export to USDZ to see contents.</div>
      </div>
    </aside>
  </main>
  <input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden>
</div>`;

// ── Three.js ──
const viewport = $id('pack-canvas').parentElement;
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
$id('pack-canvas').replaceWith(renderer.domElement);
renderer.domElement.id = 'pack-canvas';

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
scene.add(new THREE.GridHelper(10, 20, 0x44444a, 0x26262b));

let loader = null;
let nativeScene = null;
let currentBytes = null;
let currentFilename = '';

function setStatus(s) { $id('pack-status').textContent = s; }

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

// ── Export helpers ──

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = filename;
  document.body.appendChild(a); a.click(); a.remove();
  URL.revokeObjectURL(url);
}

let lastExportSizes = {};

async function exportFormat(kind) {
  if (!nativeScene || !currentBytes) { setStatus('No scene loaded.'); return; }
  const Exporter = loader.native_?.NextUSDZConverterNative;
  if (typeof Exporter !== 'function') { setStatus('Next exporter is unavailable.'); return; }
  const exporter = new Exporter();
  try {
    if (!exporter.loadFromBinary(currentBytes, currentFilename || 'scene.usd')) {
      throw new Error(exporter.error?.() || 'Could not prepare scene for export.');
    }
    if (kind === 'usda') {
      const data = exporter.exportAsUSDA();
      if (!data) throw new Error(exporter.error?.() || 'USDA export failed');
      const blob = new Blob([data], { type: 'text/plain' });
      lastExportSizes.usda = blob.size;
      downloadBlob(blob, 'export.usda');
    } else if (kind === 'usdc') {
      const bytes = new Uint8Array(exporter.exportAsUSDC());
      if (!bytes.length) throw new Error(exporter.error?.() || 'USDC export failed');
      lastExportSizes.usdc = bytes.length;
      downloadBlob(new Blob([bytes], { type: 'application/octet-stream' }), 'export.usdc');
    } else if (kind === 'usdz') {
      const bytes = new Uint8Array(exporter.exportAsUSDZ());
      if (!bytes.length) throw new Error(exporter.error?.() || 'USDZ export failed');
      lastExportSizes.usdz = bytes.length;
      downloadBlob(new Blob([bytes], { type: 'model/vnd.usdz+zip' }), 'export.usdz');
      showUSDZContents(bytes);
    }
    updateExportSizes();
    setStatus(`Exported ${kind.toUpperCase()} (${fmtBytes(lastExportSizes[kind])}).`);
  } catch (e) {
    setStatus(`Export failed: ${e.message}`);
  } finally {
    exporter.delete?.();
  }
}

function updateExportSizes() {
  const parts = [];
  if (lastExportSizes.usda) parts.push(`USDA ${fmtBytes(lastExportSizes.usda)}`);
  if (lastExportSizes.usdc) parts.push(`USDC ${fmtBytes(lastExportSizes.usdc)}`);
  if (lastExportSizes.usdz) parts.push(`USDZ ${fmtBytes(lastExportSizes.usdz)}`);
  $id('export-sizes').textContent = parts.length ? parts.join(' · ') : 'No export yet.';
}

function showUSDZContents(usdzBytes) {
  const el = $id('usdz-contents');
  try {
    const entries = parseUSDZEntries(usdzBytes);
    if (!entries || entries.length === 0) {
      el.innerHTML = '<span style="color:var(--dim)">No entries found.</span>';
      return;
    }
    let html = `<div style="font-size:.76rem;color:var(--dim);margin-bottom:4px">${entries.length} entries, ${fmtBytes(usdzBytes.length)} total</div>`;
    // Sort: root USD first, then textures by size desc
    entries.sort((a, b) => {
      const aIsUsd = /\.usd[ac]?$/i.test(a.name);
      const bIsUsd = /\.usd[ac]?$/i.test(b.name);
      if (aIsUsd && !bIsUsd) return -1;
      if (!aIsUsd && bIsUsd) return 1;
      return b.data.length - a.data.length;
    });
    for (const entry of entries) {
      const isTex = /\.(png|jpg|jpeg|webp|exr|hdr)$/i.test(entry.name);
      const icon = /\.usd[ac]?$/i.test(entry.name) ? '📦' : isTex ? '🖼' : '📄';
      const path = entry.name.split('/').filter(Boolean).join(' › ');
      html += `<div style="display:flex;align-items:center;gap:4px;padding:1px 0"><span style="flex:0 0 auto">${icon}</span><span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--text)">${escapeHTML(path)}</span><span style="flex:0 0 auto;color:var(--dim);font-size:.7rem">${fmtBytes(entry.data.length)}</span></div>`;
    }
    el.innerHTML = html;
  } catch {
    el.textContent = 'Could not parse USDZ contents.';
  }
}

// ── Scene loading ──

async function loadScene(url, label) {
  await ensureLoader();
  setStatus(`Fetching ${label}...`);
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const data = new Uint8Array(await resp.arrayBuffer());
  currentBytes = data;
  currentFilename = url.split('/').pop() || 'scene.usd';

  setStatus(`Parsing ${label}...`);
  nativeScene = await new Promise((resolve, reject) => {
    loader.parse(data, currentFilename, resolve, reject, {
      backend: 'next', maxMemoryLimitMB: 512,
    });
  });

  setStatus(`Building scene...`);
  world.clear();
  const { node: threeNode } = await buildNextDemoScene(nativeScene, { sourceUrl: url });
  world.add(threeNode);

  let tris = 0;
  world.traverse((obj) => {
    if (obj.isMesh && obj.geometry) {
      const idx = obj.geometry.index;
      tris += idx ? idx.count / 3 : obj.geometry.attributes.position.count / 3;
    }
  });

  $id('s-meshes').textContent = String(nativeScene.numMeshes ? nativeScene.numMeshes() : 0);
  $id('s-mats').textContent = String(nativeScene.numMaterials ? nativeScene.numMaterials() : 0);
  $id('s-tex').textContent = String(nativeScene.numTextures ? nativeScene.numTextures() : 0);
  $id('s-tris').textContent = Math.round(tris).toLocaleString();

  showTextures();
  $id('viewer-overlay').style.display = 'none';
  fitCamera();
  setStatus(`Loaded ${label}.`);
}

function showTextures() {
  const el = $id('texture-list');
  const n = nativeScene.numTextures ? nativeScene.numTextures() : 0;
  if (n === 0) {
    el.innerHTML = '<span style="color:var(--dim)">No textures.</span>';
    return;
  }
  let html = '';
  for (let i = 0; i < n; i++) {
    try {
      const tex = nativeScene.getTexture(i);
      if (!tex) continue;
      const name = tex.name || tex.uri || `Texture ${i}`;
      const w = tex.width || '?';
      const h = tex.height || '?';
      html += `<div style="display:flex;align-items:center;gap:4px;padding:1px 0"><span style="color:var(--text);flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${escapeHTML(name)}</span><span style="color:var(--dim);font-size:.7rem">${w}×${h}</span></div>`;
    } catch {}
  }
  el.innerHTML = html || '<span style="color:var(--dim)">Could not list textures.</span>';
}

// ── File drop ──

$id('file-input').addEventListener('change', () => {
  const file = $id('file-input').files?.[0];
  if (file) loadLocalFile(file);
  $id('file-input').value = '';
});
viewport.addEventListener('dragover', (e) => e.preventDefault());
viewport.addEventListener('drop', async (e) => {
  e.preventDefault();
  const file = e.dataTransfer?.files?.[0];
  if (file) loadLocalFile(file);
});

async function loadLocalFile(file) {
  if (!/\.(usd|usda|usdc|usdz)$/i.test(file.name)) return;
  await ensureLoader();
  currentBytes = new Uint8Array(await file.arrayBuffer());
  currentFilename = file.name;
  nativeScene = await new Promise((resolve, reject) => {
    loader.parse(currentBytes, currentFilename, resolve, reject, { backend: 'next', maxMemoryLimitMB: 512 });
  });
  world.clear();
  const { node: threeNode } = await buildNextDemoScene(nativeScene);
  world.add(threeNode);
  let tris = 0;
  world.traverse((obj) => { if (obj.isMesh && obj.geometry) { const idx = obj.geometry.index; tris += idx ? idx.count / 3 : obj.geometry.attributes.position.count / 3; } });
  $id('s-meshes').textContent = String(nativeScene.numMeshes ? nativeScene.numMeshes() : 0);
  $id('s-mats').textContent = String(nativeScene.numMaterials ? nativeScene.numMaterials() : 0);
  $id('s-tex').textContent = String(nativeScene.numTextures ? nativeScene.numTextures() : 0);
  $id('s-tris').textContent = Math.round(tris).toLocaleString();
  showTextures();
  $id('viewer-overlay').style.display = 'none';
  fitCamera();
  setStatus(`Loaded ${file.name}`);
}

// ── UI ──

$id('load-btn').addEventListener('click', () => {
  const idx = Number($id('sample-select').value);
  loadScene(SAMPLES[idx].url, SAMPLES[idx].label).catch((e) => { console.error(e); setStatus('Error: ' + e.message); });
});
$id('fit-btn').addEventListener('click', fitCamera);
$id('export-usda').addEventListener('click', () => exportFormat('usda'));
$id('export-usdc').addEventListener('click', () => exportFormat('usdc'));
$id('export-usdz').addEventListener('click', () => exportFormat('usdz'));

// ── Main ──

async function main() {
  await ensureLoader();
  const params = new URLSearchParams(location.search);
  const url = params.get('url') || params.get('uri');
  if (url) {
    await loadScene(url, url.split('/').pop());
  } else {
    await loadScene(SAMPLES[0].url, SAMPLES[0].label);
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
  const parent = renderer.domElement.parentElement;
  const rect = parent.getBoundingClientRect();
  camera.aspect = Math.max(1, rect.width) / Math.max(1, rect.height);
  camera.updateProjectionMatrix();
  renderer.setSize(Math.max(1, rect.width), Math.max(1, rect.height), false);
}
window.addEventListener('resize', onResize);

main().catch((e) => { console.error(e); setStatus('Error: ' + e.message); });
