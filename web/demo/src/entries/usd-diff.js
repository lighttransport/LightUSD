import * as THREE from 'three';
import { buildNextDemoScene } from '../next-scene.js';
import { showLoader, hideLoader } from '../lightusd-loader.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(i) { return document.getElementById(i); }

// ── Shell ──
document.getElementById('demo-root').innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>USD Diff</h1>
      <p>Compare two USD files side by side. Load each file, then view the differences.</p>
    </div>
  </header>
  <main style="display:grid;grid-template-rows:auto 1fr;min-height:0;overflow:hidden">
    <div style="display:flex;gap:10px;padding:8px 14px;border-bottom:1px solid var(--line);flex-wrap:wrap;align-items:center;background:var(--panel)">
      <span style="color:var(--muted);font-size:.78rem;font-weight:600">Compare:</span>
      <select id="compare-pair" style="padding:5px 8px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);font-size:.8rem">
        <option value="">— manual select —</option>
      </select>
      <button id="load-pair-btn" style="padding:5px 12px;border:1px solid var(--line-strong);border-radius:4px;background:var(--accent);color:#121214;cursor:pointer;font-size:.78rem;font-weight:600">Load Pair</button>
      <span style="width:1px;height:20px;background:var(--line);margin:0 4px"></span>
      <span style="color:var(--dim);font-size:.74rem;font-weight:600">A:</span>
      <select id="sample-a" style="padding:5px 8px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);font-size:.8rem"></select>
      <button id="load-a-btn" style="padding:5px 8px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);cursor:pointer;font-size:.76rem">A</button>
      <span style="color:var(--dim);font-size:.74rem;font-weight:600">B:</span>
      <select id="sample-b" style="padding:5px 8px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);font-size:.8rem"></select>
      <button id="load-b-btn" style="padding:5px 8px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);cursor:pointer;font-size:.76rem">B</button>
      <span style="flex:1"></span>
      <span id="diff-status" style="color:var(--dim);font-size:.8rem">Load two scenes to compare.</span>
    </div>
    <div style="display:grid;grid-template-columns:minmax(0,1fr) minmax(240px,320px);min-height:0;overflow:hidden">
      <div style="display:grid;grid-template-columns:1fr 1px 1fr;min-height:0;overflow:hidden">
        <div style="display:flex;flex-direction:column;min-width:0;overflow:hidden">
          <div style="flex:0 0 auto;padding:6px 10px;background:rgba(56,189,248,.08);border-bottom:1px solid var(--line);font-size:.82rem;font-weight:600;color:#38bdf8">Scene A</div>
          <div id="viewport-a" style="flex:1;min-height:0;position:relative;background:#080809"></div>
          <div id="stats-a" style="flex:0 0 auto;padding:6px 10px;border-top:1px solid var(--line);font-size:.76rem;color:var(--dim);background:var(--panel)">Not loaded.</div>
        </div>
        <div style="background:var(--line);width:1px;height:100%"></div>
        <div style="display:flex;flex-direction:column;min-width:0;overflow:hidden">
          <div style="flex:0 0 auto;padding:6px 10px;background:rgba(167,139,250,.08);border-bottom:1px solid var(--line);font-size:.82rem;font-weight:600;color:#a78bfa">Scene B</div>
          <div id="viewport-b" style="flex:1;min-height:0;position:relative;background:#080809"></div>
          <div id="stats-b" style="flex:0 0 auto;padding:6px 10px;border-top:1px solid var(--line);font-size:.76rem;color:var(--dim);background:var(--panel)">Not loaded.</div>
        </div>
      </div>
      <aside id="diff-panel" style="display:flex;flex-direction:column;overflow-y:auto;padding:10px 12px;background:var(--panel);border-left:1px solid var(--line);font-size:.8rem;gap:8px">
        <div style="color:var(--dim);font-size:.82rem;text-align:center;padding:40px 0">Load both A and B to see differences.</div>
      </aside>
    </div>
  </main>
</div>`;

// ── Scene state ──
const state = {
  a: { scene: null, three: null, stats: null, label: '' },
  b: { scene: null, three: null, stats: null, label: '' },
};

const SAMPLES = [
  { label: 'Suzanne PBR', url: './assets/suzanne-pbr.usda' },
  { label: 'Fancy Teapot (MTLX)', url: './assets/fancy-teapot-mtlx.usdz' },
  { label: 'Multi-clip skeleton', url: './assets/multi-clip-skeleton.usda' },
  { label: 'Sphere (simple)', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/sphere.usda' },
  { label: 'Cube (simple)', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/cube.usda' },
  { label: 'Cone', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/cone.usda' },
  { label: 'Robot Arm (physics)', url: './assets/physics-robot-arm.usda' },
  { label: 'Cesium Man', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/CesiumMan/CesiumMan.usdz' },
  { label: 'Damaged Helmet', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz' },
];

const PAIRS = [
  { label: 'Sphere vs Cube', a: 3, b: 4 },
  { label: 'Cone vs Sphere', a: 5, b: 3 },
  { label: 'Suzanne vs Teapot', a: 0, b: 1 },
  { label: 'Suzanne vs Skeleton', a: 0, b: 2 },
  { label: 'Robot Arm vs Skeleton', a: 6, b: 2 },
  { label: 'Cesium Man vs Helmet', a: 7, b: 8 },
];

function init() {
  const selA = $id('sample-a');
  const selB = $id('sample-b');
  const opts = SAMPLES.map((s, i) => `<option value="${i}">${escapeHTML(s.label)}</option>`).join('');
  selA.innerHTML = opts;
  selB.innerHTML = opts;
  selA.value = '0';
  selB.value = '1';

  const pairSel = $id('compare-pair');
  pairSel.innerHTML = '<option value="">— manual select —</option>' +
    PAIRS.map((p, i) => `<option value="${i}">${escapeHTML(p.label)}</option>`).join('');
}

// ── Three.js setup ──

function createViewer(containerId) {
  const container = $id(containerId);
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
  container.appendChild(renderer.domElement);
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
  return { scene, camera, renderer, controls, world };
}

const viewA = createViewer('viewport-a');
const viewB = createViewer('viewport-b');

let loader = null;

async function ensureLoader() {
  if (loader) return loader;
  loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512 });
    showLoader('Loading LightUSD WASM...', document.getElementById('viewport'));
  await loader.init({ useZstdCompressedWasm: false, useMemory64: false, backend: 'next' });
    hideLoader();
  LightUSDLoaderUtils.setLightUSD(loader.native_);
  return loader;
}

// ── Scene stats extraction ──

function extractStats(sceneData, threeWorld) {
  let tris = 0;
  threeWorld.traverse((obj) => {
    if (obj.isMesh && obj.geometry) {
      const idx = obj.geometry.index;
      tris += idx ? idx.count / 3 : obj.geometry.attributes.position.count / 3;
    }
  });
  return {
    meshes: sceneData.numMeshes ? sceneData.numMeshes() : 0,
    materials: sceneData.numMaterials ? sceneData.numMaterials() : 0,
    textures: sceneData.numTextures ? sceneData.numTextures() : 0,
    lights: sceneData.numLights ? sceneData.numLights() : 0,
    cameras: sceneData.numCameras ? sceneData.numCameras() : 0,
    triangles: Math.round(tris),
    upAxis: sceneData.getUpAxis ? sceneData.getUpAxis() : 'Y',
    metadata: sceneData.getSceneMetadata ? sceneData.getSceneMetadata() : {},
  };
}

// ── Loading ──

async function loadScene(side, url, label) {
  await ensureLoader();
  const key = side === 'a' ? 'a' : 'b';
  const s = state[key];
  const view = key === 'a' ? viewA : viewB;
  const statsEl = $id('stats-' + key);

  statsEl.textContent = `Loading ${label}...`;
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const data = new Uint8Array(await resp.arrayBuffer());
  const filename = url.split('/').pop() || 'scene.usd';

  const sceneData = await new Promise((resolve, reject) => {
    loader.parse(data, filename, resolve, reject, { backend: 'next', maxMemoryLimitMB: 512 });
  });

  view.world.clear();
  const { node: threeNode } = await buildNextDemoScene(sceneData, { sourceUrl: url });
  view.world.add(threeNode);

  s.scene = sceneData;
  s.three = threeNode;
  s.stats = extractStats(sceneData, view.world);
  s.label = label;

  statsEl.textContent = `${label}: ${s.stats.meshes} meshes, ${s.stats.materials} mats, ${s.stats.textures} tex`;

  // Fit camera
  const box = new THREE.Box3().setFromObject(view.world);
  if (!box.isEmpty()) {
    const center = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3());
    const maxDim = Math.max(size.x, size.y, size.z, 0.1);
    const dist = maxDim * 2.5;
    view.controls.target.copy(center);
    view.camera.position.copy(center).add(new THREE.Vector3(1, 0.6, 1).normalize().multiplyScalar(dist));
    view.camera.near = Math.max(0.001, dist / 100);
    view.camera.far = dist * 100;
    view.camera.updateProjectionMatrix();
    view.controls.update();
  }

  if (state.a.stats && state.b.stats) computeDiff();
}

// ── Diff computation ──

function computeDiff() {
  const sa = state.a.stats;
  const sb = state.b.stats;
  const panel = $id('diff-panel');

  const diff = (a, b, label, fmt) => {
    const valA = a ?? 0;
    const valB = b ?? 0;
    const diffVal = valB - valA;
    const sign = diffVal > 0 ? '+' : '';
    const cls = diffVal === 0 ? 'dim' : (diffVal > 0 ? 'green' : 'red');
    const color = cls === 'green' ? '#75d4a5' : cls === 'red' ? '#f08a8a' : 'var(--dim)';
    return `<div style="display:flex;align-items:center;gap:6px;padding:2px 0">
      <span style="flex:1;color:var(--dim);font-size:.76rem">${escapeHTML(label)}</span>
      <span style="color:var(--text);font-size:.8rem;font-variant-numeric:tabular-nums;text-align:right;min-width:6ch">${fmt ? fmt(valA) : valA}</span>
      <span style="color:var(--dim);font-size:.74rem">→</span>
      <span style="color:var(--text);font-size:.8rem;text-align:right;min-width:6ch">${fmt ? fmt(valB) : valB}</span>
      <span style="color:${color};font-size:.78rem;font-weight:600;min-width:5ch;text-align:right">${sign}${fmt ? fmt(Math.abs(diffVal)) : diffVal}</span>
    </div>`;
  };

  const fmtNum = (v) => String(v);
  const fmtStr = (v) => String(v ?? '—');

  let html = '<div style="font-size:.82rem;font-weight:700;color:var(--accent);margin-bottom:4px">Differences</div>';

  html += '<div style="font-size:.72rem;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;margin:8px 0 4px">Stage Metadata</div>';
  html += diff(sa.upAxis, sb.upAxis, 'Up Axis', fmtStr);
  html += diff(sa.metadata?.metersPerUnit, sb.metadata?.metersPerUnit, 'Meters Per Unit', (v) => v?.toFixed(3) ?? '—');
  html += diff(sa.metadata?.timeCodesPerSecond, sb.metadata?.timeCodesPerSecond, 'TimeCodes/s', fmtNum);
  html += diff(sa.metadata?.startTimeCode, sb.metadata?.startTimeCode, 'Start TimeCode', fmtNum);
  html += diff(sa.metadata?.endTimeCode, sb.metadata?.endTimeCode, 'End TimeCode', fmtNum);

  html += '<div style="font-size:.72rem;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;margin:8px 0 4px">Counts</div>';
  html += diff(sa.meshes, sb.meshes, 'Meshes');
  html += diff(sa.materials, sb.materials, 'Materials');
  html += diff(sa.textures, sb.textures, 'Textures');
  html += diff(sa.lights, sb.lights, 'Lights');
  html += diff(sa.cameras, sb.cameras, 'Cameras');
  html += diff(sa.triangles, sb.triangles, 'Triangles');

  // Mesh details if available
  const meshesA = sa.meshes || 0;
  const meshesB = sb.meshes || 0;

  // Material details
  html += '<div style="font-size:.72rem;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;margin:8px 0 4px">Geometry Change</div>';
  if (sa.triangles !== sb.triangles) {
    const ratio = sa.triangles > 0 ? ((sb.triangles / sa.triangles) * 100 - 100).toFixed(1) : '∞';
    const direction = Number(ratio) > 0 ? 'more' : Number(ratio) < 0 ? 'fewer' : 'same';
    html += `<div style="padding:8px;border-radius:4px;background:var(--panel-2);border:1px solid var(--line);margin-top:4px;font-size:.78rem;color:var(--muted)"><strong style="color:var(--text)">${Math.abs(sb.triangles - sa.triangles).toLocaleString()}</strong> ${direction} triangles (${ratio}% change)</div>`;
  }

  if (meshesA !== meshesB) {
    html += `<div style="padding:8px;border-radius:4px;background:var(--panel-2);border:1px solid var(--line);margin-top:4px;font-size:.78rem;color:var(--muted)">Mesh count differs by <strong style="color:var(--text)">${Math.abs(meshesB - meshesA)}</strong></div>`;
  }

  // Overall
  const totalDiff = Math.abs(sa.meshes - sb.meshes) + Math.abs(sa.materials - sb.materials) + Math.abs(sa.textures - sb.textures);
  html += '<div style="margin-top:10px;padding:8px;border-radius:4px;background:var(--panel-3);border:1px solid var(--line);font-size:.78rem;color:var(--muted)">';
  if (totalDiff === 0 && sa.triangles === sb.triangles) {
    html += '<span style="color:var(--ok)">✓ Scenes appear identical.</span>';
  } else {
    html += `<span style="color:var(--accent-warm)">⚠ ${totalDiff} metric difference(s) detected.</span>`;
  }
  html += '</div>';

  panel.innerHTML = html;
  $id('diff-status').textContent = `Comparing ${state.a.label} vs ${state.b.label}`;
}

// ── UI ──

$id('load-a-btn').addEventListener('click', async () => {
  const idx = Number($id('sample-a').value);
  const s = SAMPLES[idx];
  try { await loadScene('a', s.url, s.label); } catch (e) { $id('stats-a').textContent = 'Error: ' + e.message; }
});
$id('load-b-btn').addEventListener('click', async () => {
  const idx = Number($id('sample-b').value);
  const s = SAMPLES[idx];
  try { await loadScene('b', s.url, s.label); } catch (e) { $id('stats-b').textContent = 'Error: ' + e.message; }
});
$id('load-pair-btn').addEventListener('click', async () => {
  const idx = Number($id('compare-pair').value);
  if (isNaN(idx)) return;
  const pair = PAIRS[idx];
  if (!pair) return;
  const sa = SAMPLES[pair.a];
  const sb = SAMPLES[pair.b];
  $id('sample-a').value = String(pair.a);
  $id('sample-b').value = String(pair.b);
  try {
    await Promise.all([
      loadScene('a', sa.url, sa.label),
      loadScene('b', sb.url, sb.label),
    ]);
  } catch (e) {
    $id('diff-status').textContent = 'Error: ' + e.message;
  }
});

// ── Animation loop ──

let lastTime = performance.now();
function anim(now) {
  requestAnimationFrame(anim);
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;
  viewA.controls.update();
  viewA.renderer.render(viewA.scene, viewA.camera);
  viewB.controls.update();
  viewB.renderer.render(viewB.scene, viewB.camera);
}

function onResize() {
  for (const v of [viewA, viewB]) {
    const rect = v.renderer.domElement.parentElement.getBoundingClientRect();
    const w = Math.max(1, rect.width);
    const h = Math.max(1, rect.height);
    v.camera.aspect = w / h;
    v.camera.updateProjectionMatrix();
    v.renderer.setSize(w, h, false);
  }
}
window.addEventListener('resize', onResize);

// ── Main ──

init();
(async () => {
  await ensureLoader();
  await Promise.all([
    loadScene('a', SAMPLES[0].url, SAMPLES[0].label),
    loadScene('b', SAMPLES[1].url, SAMPLES[1].label),
  ]);
})().catch((e) => console.error(e));
requestAnimationFrame(anim);
