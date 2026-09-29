import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import { isNextScene, nextCountsFromScene } from 'lightusd-next-demo-utils';
import { buildNextDemoScene } from '../next-scene.js';
import { showLoader, hideLoader } from '../lightusd-loader.js';

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(i) { return document.getElementById(i); }

// ── Shell ──
document.getElementById('demo-root').innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>Backend Comparison</h1>
      <p>Compare deprecated legacy and next rendering backends side by side with synchronized cameras.</p>
    </div>
    <div class="demo-actions">
      <select id="sample-select"></select>
      <button id="load-btn" class="primary">Load</button>
      <button id="fit-btn">Fit Both</button>
    </div>
  </header>
  <main class="assets-main" style="grid-template-columns:minmax(0,1fr) minmax(280px,360px)">
    <div style="display:grid;grid-template-columns:1fr 1px 1fr;min-height:0;overflow:hidden">
      <div id="legacy-wrap" style="display:flex;flex-direction:column;min-width:0;overflow:hidden">
        <div style="flex:0 0 auto;padding:6px 10px;background:rgba(56,189,248,.08);border-bottom:1px solid var(--line);display:flex;align-items:center;gap:6px;font-size:.82rem;font-weight:600;color:#38bdf8">
          Legacy Backend (deprecated)<div id="legacy-loading" class="loading-dot" style="width:6px;height:6px;border-radius:50%;background:var(--ok);display:inline-block;margin-left:auto"></div>
        </div>
        <div id="legacy-vp" style="flex:1;min-height:0;position:relative;background:#080809"></div>
        <div style="flex:0 0 auto;padding:5px 10px;border-top:1px solid var(--line);font-size:.74rem;color:var(--dim);background:var(--panel);display:flex;gap:12px;flex-wrap:wrap">
          <span id="legacy-stats" style="color:var(--dim)">—</span>
          <span style="flex:1"></span>
          <span id="legacy-fps" style="color:var(--ok);font-variant-numeric:tabular-nums">—</span>
        </div>
      </div>
      <div style="background:var(--line);width:1px;height:100%"></div>
      <div id="next-wrap" style="display:flex;flex-direction:column;min-width:0;overflow:hidden">
        <div style="flex:0 0 auto;padding:6px 10px;background:rgba(167,139,250,.08);border-bottom:1px solid var(--line);display:flex;align-items:center;gap:6px;font-size:.82rem;font-weight:600;color:#a78bfa">
          Next Backend<div id="next-loading" class="loading-dot" style="width:6px;height:6px;border-radius:50%;background:var(--ok);display:inline-block;margin-left:auto"></div>
        </div>
        <div id="next-vp" style="flex:1;min-height:0;position:relative;background:#080809"></div>
        <div style="flex:0 0 auto;padding:5px 10px;border-top:1px solid var(--line);font-size:.74rem;color:var(--dim);background:var(--panel);display:flex;gap:12px;flex-wrap:wrap">
          <span id="next-stats" style="color:var(--dim)">—</span>
          <span style="flex:1"></span>
          <span id="next-fps" style="color:var(--ok);font-variant-numeric:tabular-nums">—</span>
        </div>
      </div>
    </div>
    <aside id="diff-panel" style="display:flex;flex-direction:column;overflow-y:auto;padding:12px;background:var(--panel);border-left:1px solid var(--line);font-size:.8rem;gap:6px">
      <div style="color:var(--dim);text-align:center;padding:40px 0;font-size:.82rem">Load a scene to compare backends.</div>
    </aside>
  </main>
</div>`;

// ── Samples ──
const SAMPLES = [
  { label: 'Suzanne PBR', url: './assets/suzanne-pbr.usda' },
  { label: 'Fancy Teapot (MTLX)', url: './assets/fancy-teapot-mtlx.usdz' },
  { label: 'Sphere', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/sphere.usda' },
  { label: 'Damaged Helmet', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz' },
  { label: 'Multi-clip skeleton', url: './assets/multi-clip-skeleton.usda' },
  { label: 'Robot Arm (physics)', url: './assets/physics-robot-arm.usda' },
];
$id('sample-select').innerHTML = SAMPLES.map((s, i) => `<option value="${i}">${escapeHTML(s.label)}</option>`).join('');

// ── Viewer setup ──
function viewer(container) {
  const s = new THREE.Scene(); s.background = new THREE.Color(0x0e0e10);
  const c = new THREE.PerspectiveCamera(45, 1, 0.01, 200); c.position.set(3, 2.5, 4);
  const r = new THREE.WebGLRenderer({ antialias: true });
  r.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  r.outputColorSpace = THREE.SRGBColorSpace; r.toneMapping = THREE.ACESFilmicToneMapping;
  r.toneMappingExposure = 1.0; r.shadowMap.enabled = true;
  container.appendChild(r.domElement);
  const o = new OrbitControls(c, r.domElement); o.enableDamping = true; o.dampingFactor = 0.08;
  const w = new THREE.Group(); s.add(w);
  s.add(new THREE.HemisphereLight(0xdde8f6, 0x24272c, 1.2));
  const kl = new THREE.DirectionalLight(0xffffff, 2); kl.position.set(4, 6, 5); kl.castShadow = true; s.add(kl);
  s.add(new THREE.GridHelper(10, 20, 0x44444a, 0x26262b));
  let fpsF = 0, fpsL = performance.now(), fpsV = 0;
  return { scene: s, camera: c, renderer: r, controls: o, world: w, statsEl: null, data: null,
    fps() { return fpsV; },
    tick(now) { fpsF++; if (now - fpsL >= 500) { fpsV = Math.round((fpsF * 1000) / (now - fpsL)); fpsF = 0; fpsL = now; } },
  };
}
const vLegacy = viewer($id('legacy-vp'));
const vNext = viewer($id('next-vp'));

// Synchronize cameras with recursion guard
let syncing = false;
function syncCameras(source, target) {
  if (syncing) return;
  syncing = true;
  target.camera.position.copy(source.camera.position);
  target.camera.quaternion.copy(source.camera.quaternion);
  target.controls.target.copy(source.controls.target);
  target.camera.updateProjectionMatrix();
  target.controls.update();
  syncing = false;
}
vLegacy.controls.addEventListener('change', () => syncCameras(vLegacy, vNext));
vNext.controls.addEventListener('change', () => syncCameras(vNext, vLegacy));

// ── Loading ──
let loader = null;

async function loadScene(url, label) {
  await ensureLoader();

  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const data = new Uint8Array(await resp.arrayBuffer());
  const filename = url.split('/').pop() || 'scene.usd';

  // ---- Legacy ----
  $id('legacy-loading').style.background = '#f59e0b';
  const legacyScene = await new Promise((resolve, reject) => {
    loader.parse(data, filename, resolve, reject, { backend: 'legacy', maxMemoryLimitMB: 512 });
  });
  const defaultMat = LightUSDLoaderUtils.createDefaultMaterial();
  vLegacy.legacyNode = await LightUSDLoaderUtils.buildThreeNode(legacyScene.getDefaultRootNode(), defaultMat, legacyScene, {
    preferredMaterialType: 'usdpreviewsurface', textureCache: new Map(),
  });
  vLegacy.world.clear();
  vLegacy.world.add(vLegacy.legacyNode);
  vLegacy.data = legacyScene;
  let lm = legacyScene.numMeshes ? legacyScene.numMeshes() : 0;
  let lma = legacyScene.numMaterials ? legacyScene.numMaterials() : 0;
  let lt = legacyScene.numTextures ? legacyScene.numTextures() : 0;
  vLegacy.statsEl = document.createElement('span');
  $id('legacy-stats').textContent = `${lm} meshes, ${lma} mats, ${lt} tex`;
  $id('legacy-loading').style.background = '#75d4a5';

  // ---- Next ----
  $id('next-loading').style.background = '#f59e0b';
  let nextScene;
  try {
    nextScene = await new Promise((resolve, reject) => {
      loader.parse(data, filename, resolve, reject, { backend: 'next', maxMemoryLimitMB: 512 });
    });
  } catch (e) {
    $id('next-stats').textContent = `Next backend unavailable: ${e.message}`;
    $id('next-loading').style.background = '#f08a8a';
    $id('diff-panel').innerHTML = `<div style="color:var(--dim);padding:20px 0;font-size:.82rem">Next backend not available in this build. The next WASM module (lightusd_next.wasm) may not be present.</div>`;
    $id('next-stats').style.color = '#f08a8a';
    fitBoth();
    return;
  }
  if (isNextScene(nextScene)) {
    const built = await buildNextDemoScene(nextScene, { sourceUrl: url });
    vNext.world.clear();
    vNext.world.add(built.node);
    vNext.data = nextScene;
    const nc = nextCountsFromScene(nextScene);
    vNext.counts = nc;
    $id('next-stats').textContent = `${nc.meshes} meshes, ${nc.materials} mats, ${nc.textures} tex`;
  } else {
    $id('next-stats').textContent = 'Next backend returned non-next scene';
    $id('next-loading').style.background = '#f08a8a';
  }
  $id('next-loading').style.background = '#75d4a5';

  // Diff panel
  const diffEl = $id('diff-panel');
  const legacyMem = legacyScene.getSceneMetadata ? legacyScene.getSceneMetadata() : {};
  let html = '<div style="font-weight:700;color:var(--accent);margin-bottom:4px;font-size:.82rem">Backend Comparison</div>';
  html += '<table style="width:100%;border-collapse:collapse;font-size:.76rem">';
  html += '<tr style="color:var(--dim);border-bottom:1px solid var(--line)"><td style="padding:4px 6px">Metric</td><td style="padding:4px 6px;text-align:center;color:#38bdf8;font-weight:600">Legacy</td><td style="padding:4px 6px;text-align:center;color:#a78bfa;font-weight:600">Next</td></tr>';
  const rows = [
    ['Meshes', lm, vNext.counts?.meshes ?? '—'],
    ['Materials', lma, vNext.counts?.materials ?? '—'],
    ['Textures', lt, vNext.counts?.textures ?? '—'],
    ['Up Axis', legacyScene.getUpAxis?.() || 'Y', 'Y'],
  ];
  for (const [label, a, b] of rows) {
    html += `<tr style="border-bottom:1px solid var(--line)"><td style="padding:3px 6px;color:var(--dim)">${label}</td><td style="padding:3px 6px;text-align:center;color:var(--text)">${a}</td><td style="padding:3px 6px;text-align:center;color:var(--text)">${b}</td></tr>`;
  }
  html += '</table>';
  html += `<div style="margin-top:8px;padding:8px;border-radius:4px;background:var(--panel-2);border:1px solid var(--line);font-size:.76rem;color:var(--muted)">`;
  html += `<strong style="color:var(--text)">Key differences:</strong><br>`;
  html += `• Legacy builds full Three.js scene graph from USD data<br>`;
  html += `• Next uses incremental flatten + native render scene conversion<br>`;
  html += `• Legacy supports all material types; next has faster loading<br>`;
  html += `• Cameras are synchronized — orbit to compare identical views`;
  html += '</div>';
  diffEl.innerHTML = html;

  fitBoth();
}

function fitBoth() {
  for (const v of [vLegacy, vNext]) {
    const box = new THREE.Box3().setFromObject(v.world);
    if (box.isEmpty()) continue;
    const center = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3());
    const maxDim = Math.max(size.x, size.y, size.z, 0.1);
    const dist = maxDim * 2.5;
    v.controls.target.copy(center);
    v.camera.position.copy(center).add(new THREE.Vector3(1, 0.6, 1).normalize().multiplyScalar(dist));
    v.camera.near = Math.max(0.001, dist / 100);
    v.camera.far = dist * 100;
    v.camera.updateProjectionMatrix();
    v.controls.update();
  }
}

// ── UI ──
$id('load-btn').addEventListener('click', async () => {
  const idx = Number($id('sample-select').value);
  const s = SAMPLES[idx];
  try { await loadScene(s.url, s.label); } catch (e) { $id('legacy-stats').textContent = 'Error: ' + e.message; }
});
$id('fit-btn').addEventListener('click', fitBoth);

// ── Animation ──
let lastTime = performance.now();
function anim(now) {
  requestAnimationFrame(anim);
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;
  for (const v of [vLegacy, vNext]) {
    v.tick(now);
    v.controls.update();
    v.renderer.render(v.scene, v.camera);
  }
  $id('legacy-fps').textContent = vLegacy.fps() + ' FPS';
  $id('next-fps').textContent = vNext.fps() + ' FPS';
}

function onResize() {
  for (const [v, id] of [[vLegacy, 'legacy-vp'], [vNext, 'next-vp']]) {
    const rect = $id(id).getBoundingClientRect();
    const w = Math.max(1, rect.width); const h = Math.max(1, rect.height);
    v.camera.aspect = w / h; v.camera.updateProjectionMatrix();
    v.renderer.setSize(w, h, false);
  }
}
window.addEventListener('resize', onResize);

// ── Main ──
ensureLoader().then(() => loadScene(SAMPLES[0].url, SAMPLES[0].label)).catch(console.error);
requestAnimationFrame(anim);

async function ensureLoader() {
  if (loader) return loader;
  loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512 });
  showLoader('Loading LightUSD WASM...', $id('legacy-wrap'));
  try {
    await loader.init({ useZstdCompressedWasm: false, useMemory64: false, backend: 'legacy' });
  } finally { hideLoader(); }
  LightUSDLoaderUtils.setLightUSD(loader.native_);
  return loader;
}
