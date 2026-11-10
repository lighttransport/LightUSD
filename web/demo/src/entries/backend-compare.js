import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
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
      <h1>Core Roundtrip Comparison</h1>
      <p>Compare the source scene with a USDC roundtrip using synchronized cameras.</p>
    </div>
    <div class="demo-actions">
      <select id="sample-select"></select>
      <button id="load-btn" class="primary">Load</button>
      <button id="fit-btn">Fit Both</button>
    </div>
  </header>
  <main class="assets-main" style="grid-template-columns:minmax(0,1fr) minmax(280px,360px)">
    <div style="display:grid;grid-template-columns:1fr 1px 1fr;min-height:0;overflow:hidden">
      <div id="source-wrap" style="display:flex;flex-direction:column;min-width:0;overflow:hidden">
        <div style="flex:0 0 auto;padding:6px 10px;background:rgba(56,189,248,.08);border-bottom:1px solid var(--line);display:flex;align-items:center;gap:6px;font-size:.82rem;font-weight:600;color:#38bdf8">
          Source<div id="source-loading" class="loading-dot" style="width:6px;height:6px;border-radius:50%;background:var(--ok);display:inline-block;margin-left:auto"></div>
        </div>
        <div id="source-vp" style="flex:1;min-height:0;position:relative;background:#080809"></div>
        <div style="flex:0 0 auto;padding:5px 10px;border-top:1px solid var(--line);font-size:.74rem;color:var(--dim);background:var(--panel);display:flex;gap:12px;flex-wrap:wrap">
          <span id="source-stats" style="color:var(--dim)">—</span>
          <span style="flex:1"></span>
          <span id="source-fps" style="color:var(--ok);font-variant-numeric:tabular-nums">—</span>
        </div>
      </div>
      <div style="background:var(--line);width:1px;height:100%"></div>
      <div id="roundtrip-wrap" style="display:flex;flex-direction:column;min-width:0;overflow:hidden">
        <div style="flex:0 0 auto;padding:6px 10px;background:rgba(167,139,250,.08);border-bottom:1px solid var(--line);display:flex;align-items:center;gap:6px;font-size:.82rem;font-weight:600;color:#a78bfa">
          USDC Roundtrip<div id="roundtrip-loading" class="loading-dot" style="width:6px;height:6px;border-radius:50%;background:var(--ok);display:inline-block;margin-left:auto"></div>
        </div>
        <div id="roundtrip-vp" style="flex:1;min-height:0;position:relative;background:#080809"></div>
        <div style="flex:0 0 auto;padding:5px 10px;border-top:1px solid var(--line);font-size:.74rem;color:var(--dim);background:var(--panel);display:flex;gap:12px;flex-wrap:wrap">
          <span id="roundtrip-stats" style="color:var(--dim)">—</span>
          <span style="flex:1"></span>
          <span id="roundtrip-fps" style="color:var(--ok);font-variant-numeric:tabular-nums">—</span>
        </div>
      </div>
    </div>
    <aside id="diff-panel" style="display:flex;flex-direction:column;overflow-y:auto;padding:12px;background:var(--panel);border-left:1px solid var(--line);font-size:.8rem;gap:6px">
      <div style="color:var(--dim);text-align:center;padding:40px 0;font-size:.82rem">Load a scene to compare its roundtrip.</div>
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
const vSource = viewer($id('source-vp'));
const vRoundtrip = viewer($id('roundtrip-vp'));

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
vSource.controls.addEventListener('change', () => syncCameras(vSource, vRoundtrip));
vRoundtrip.controls.addEventListener('change', () => syncCameras(vRoundtrip, vSource));

// ── Loading ──
let loader = null;

async function loadScene(url, label) {
  await ensureLoader();

  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
  const data = new Uint8Array(await resp.arrayBuffer());
  const filename = url.split('/').pop() || 'scene.usd';

  const parse = (bytes, name) => new Promise((resolve, reject) => {
    loader.parse(bytes, name, resolve, reject, { backend: 'next', maxMemoryLimitMB: 512 });
  });
  const converter = new loader.native_.NextUSDZConverterNative();
  let roundtrip;
  try {
    if (!converter.loadFromBinary(data, filename)) throw new Error(converter.error());
    // Keep packaged assets while exercising the USDC writer and reader.
    roundtrip = converter.exportAsUSDZWithOptions({rootLayerFormat: 'usdc'});
    if (!roundtrip?.length) throw new Error(converter.error());
  } finally { converter.delete(); }

  const counts = [];
  for (const [view, id, bytes, name] of [
    [vSource, 'source', data, filename],
    [vRoundtrip, 'roundtrip', roundtrip, 'roundtrip.usdz'],
  ]) {
    $id(`${id}-loading`).style.background = '#f59e0b';
    const scene = await parse(bytes, name);
    if (!isNextScene(scene)) throw new Error('Core loader returned an incompatible scene');
    try {
      const built = await buildNextDemoScene(scene, {sourceUrl: url});
      for (const child of [...view.world.children]) {
        child.traverse(object => {
          object.geometry?.dispose?.();
          for (const material of [].concat(object.material || [])) material.dispose?.();
        });
      }
      view.world.clear();
      view.data?.delete?.();
      view.world.add(built.node);
      view.data = scene;
      const count = nextCountsFromScene(scene);
      counts.push(count);
      $id(`${id}-stats`).textContent = `${count.meshes} meshes, ${count.materials} mats, ${count.textures} tex`;
      $id(`${id}-loading`).style.background = '#75d4a5';
    } catch (error) { scene.delete?.(); throw error; }
  }
  const rows = ['meshes', 'materials', 'textures'].map(key =>
    `<tr><td>${key}</td><td>${counts[0][key]}</td><td>${counts[1][key]}</td></tr>`).join('');
  $id('diff-panel').innerHTML = `<strong>Core USDC roundtrip</strong>
    <table><thead><tr><th>Metric</th><th>Source</th><th>Roundtrip</th></tr></thead><tbody>${rows}</tbody></table>
    <p>The right scene is written to a USDC-rooted USDZ archive and loaded again.
    Both views use the core reader and renderer. Orbit to compare identical views.</p>`;

  fitBoth();
}

function fitBoth() {
  for (const v of [vSource, vRoundtrip]) {
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
  try { await loadScene(s.url, s.label); } catch (e) { $id('source-stats').textContent = 'Error: ' + e.message; }
});
$id('fit-btn').addEventListener('click', fitBoth);

// ── Animation ──
let lastTime = performance.now();
function anim(now) {
  requestAnimationFrame(anim);
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;
  for (const v of [vSource, vRoundtrip]) {
    v.tick(now);
    v.controls.update();
    v.renderer.render(v.scene, v.camera);
  }
  $id('source-fps').textContent = vSource.fps() + ' FPS';
  $id('roundtrip-fps').textContent = vRoundtrip.fps() + ' FPS';
}

function onResize() {
  for (const [v, id] of [[vSource, 'source-vp'], [vRoundtrip, 'roundtrip-vp']]) {
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
  showLoader('Loading LightUSD WASM...', $id('source-wrap'));
  try {
    await loader.init({ useZstdCompressedWasm: false, useMemory64: false, backend: 'next' });
  } finally { hideLoader(); }
  return loader;
}
