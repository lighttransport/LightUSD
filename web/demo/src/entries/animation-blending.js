import * as THREE from 'three';
import { buildNextDemoScene } from '../next-scene.js';
import { showLoader, hideLoader } from '../lightusd-loader.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import { getUSDSceneMetadata } from 'lightusd/USDSceneMetadata.js';
import { extractSkinnedMeshData } from 'lightusd/USDSceneSkinningData.js';
import { buildSkeletonDataFromUSD } from 'lightusd/USDSkeletonData.js';
import { applyUSDSceneSkinningPipeline } from 'lightusd/USDSceneSkinningPipeline.js';
import { extractUSDSceneAnimations } from 'lightusd/USDSceneAnimationPipeline.js';
import { buildNodeIndexMap } from 'lightusd/USDAnimationConverter.js';

const SAMPLES = [
  { label: 'Multi-clip skeleton', url: './assets/multi-clip-skeleton.usda' },
  { label: 'Cesium Man', url: 'https://raw.githubusercontent.com/usd-wg/assets/refs/heads/main/test_assets/USDZ/CesiumMan/CesiumMan.usdz' },
  { label: 'Brain Stem', url: 'https://raw.githubusercontent.com/usd-wg/assets/refs/heads/main/test_assets/USDZ/BrainStem/BrainStem.usdz' },
];

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(i) { return document.getElementById(i); }

// ── Shell ──
document.getElementById('demo-root').innerHTML = `
<div class="demo-shell" style="grid-template-rows:auto 1fr">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>Animation Blending</h1>
      <p>Crossfade and blend between USD skeletal animation clips with per-clip weights.</p>
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
      <div id="status" class="status">Initializing...</div>
    </section>
    <aside class="blend-panel" id="blend-panel">
      <p id="blend-placeholder" style="color:var(--dim);font-size:.85rem">Load a multi-clip asset to blend animations.</p>
    </aside>
  </main>
  <input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden>
</div>`;

// ── Three.js ──
const viewport = $id('viewport');
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x0e0e10);

const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 200);
camera.position.set(2, 1.5, 3);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.0;
renderer.shadowMap.enabled = true;
viewport.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.dampingFactor = 0.08;

const world = new THREE.Group();
scene.add(world);
scene.add(new THREE.HemisphereLight(0xdde8f6, 0x24272c, 1.2));
const keyLight = new THREE.DirectionalLight(0xffffff, 2);
keyLight.position.set(3, 5, 4);
keyLight.castShadow = true;
scene.add(keyLight);
scene.add(new THREE.GridHelper(8, 20, 0x44444a, 0x26262b));

// ── Animation state ──
let loader = null;
let mixer = null;
let actions = [];
let skeletonHelpers = [];
let clips = [];
let duration = 0;
let playing = true;
let currentTime = 0;

// ── Blend state ──
const blend = {};

function setStatus(s) { $id('status').textContent = s; }

// ── Seeks all actions to the same time while keeping their weights ──
function seekAll(time) {
  if (!mixer || actions.length === 0) return;
  for (const a of actions) {
    a.time = Math.max(0, Math.min(time, a.getClip().duration));
    a.play();
  }
  currentTime = time;
}

// ── Updates timeline display ──
function updateTimelineDisplay() {
  const el = document.querySelector('.time-label');
  if (el) {
    const m = Math.floor(currentTime / 60);
    const s = currentTime % 60;
    const dm = Math.floor(duration / 60);
    const ds = duration % 60;
    el.textContent = `${m}:${s.toFixed(2).padStart(5, '0')} / ${dm}:${ds.toFixed(2).padStart(5, '0')}`;
  }
  const fill = document.querySelector('.fill');
  const scrub = document.querySelector('.scrubber');
  if (fill && scrub && duration > 0) {
    const pct = Math.min(100, (currentTime / duration) * 100);
    fill.style.width = pct + '%';
    scrub.style.left = pct + '%';
  }
}

// ── Rebuilds blend UI from loaded clips ──
function buildBlendUI() {
  const panel = $id('blend-panel');
  panel.innerHTML = '';

  if (clips.length < 2) {
    panel.innerHTML = `<p style="color:var(--dim);font-size:.85rem">Need at least 2 clips for blending (found ${clips.length}). Try the multi-clip skeleton asset.</p>`;
    return;
  }

  // Timeline
  const timelineHtml = `
    <div class="blend-section"><h3>Timeline</h3>
      <div class="timeline-row">
        <div class="track" id="blend-track">
          <div class="bg"><div class="fill" style="width:0%"></div></div>
          <div class="scrubber" style="left:0%"></div>
        </div>
        <span class="time-label">0:00.00 / 0:00.00</span>
      </div>
      <div class="btn-row" style="margin-top:6px">
        <button id="play-btn" class="primary" style="flex:0 0 auto;padding:6px 16px">Pause</button>
        <button id="reset-btn" style="flex:0 0 auto;padding:6px 16px">Reset</button>
        <select id="speed-select" style="flex:0 0 auto;padding:4px;border:1px solid var(--line-strong);border-radius:4px;background:var(--panel-2);color:var(--text);font-size:.78rem">
          <option value="0.25">0.25×</option><option value="0.5">0.5×</option>
          <option value="1" selected>1×</option><option value="2">2×</option><option value="4">4×</option>
        </select>
      </div>
    </div>`;

  // Blending section
  let blendHtml = `<div class="blend-section"><h3>Clip Blending</h3>`;

  // Assign clips to channels
  const channels = [
    { key: 'a', label: 'Clip A', defaultWeight: 0.7, color: '#38bdf8' },
    { key: 'b', label: 'Clip B', defaultWeight: 0.3, color: '#a78bfa' },
  ];

  for (const ch of channels) {
    blend[ch.key] = { weight: ch.defaultWeight, clipIdx: 0, action: null };
    blendHtml += `<div class="clip-row" style="border-left:3px solid ${ch.color}">
      <div class="row-header">
        <span class="clip-name">${ch.label}</span>
        <select class="clip-select" data-channel="${ch.key}">
          ${clips.map((c, i) => `<option value="${i}"${i===0?' selected':''}>${c.name || 'Clip '+i}</option>`).join('')}
        </select>
      </div>
      <div class="weight-control">
        <span style="font-size:.72rem;color:var(--dim)">Weight</span>
        <input type="range" class="weight-slider" data-channel="${ch.key}" min="0" max="1" step="0.01" value="${ch.defaultWeight}">
        <span class="weight-val" id="wval-${ch.key}">${ch.defaultWeight.toFixed(2)}</span>
      </div>
    </div>`;
  }

  blendHtml += `<div class="btn-row">
    <button id="crossfade-btn" class="primary">Crossfade A→B</button>
    <button id="equalize-btn">Equalize (50/50)</button>
  </div></div>`;

  // Info
  const infoHtml = `
    <div class="blend-section"><h3>Info</h3>
      <div class="info-grid">
        <span class="k">Clips</span><span class="v" id="info-clips">${clips.length}</span>
        <span class="k">Duration</span><span class="v" id="info-dur">${duration.toFixed(2)}s</span>
        <span class="k">FPS</span><span class="v" id="info-fps">—</span>
        <span class="k">Blend</span><span class="v" id="info-blend">Manual</span>
      </div>
    </div>`;

  panel.innerHTML = timelineHtml + blendHtml + infoHtml;
  bindBlendUI();
  updateTimelineDisplay();
}

function bindBlendUI() {
  // Play/Pause
  $id('play-btn').addEventListener('click', () => {
    playing = !playing;
    $id('play-btn').textContent = playing ? 'Pause' : 'Play';
    if (playing) for (const a of actions) { a.paused = false; a.play(); }
    else for (const a of actions) a.paused = true;
  });

  // Reset
  $id('reset-btn').addEventListener('click', () => {
    seekAll(0);
    if (!playing) { playing = true; $id('play-btn').textContent = 'Pause'; for (const a of actions) { a.paused = false; a.play(); } }
  });

  // Speed
  $id('speed-select').addEventListener('change', () => {
    if (mixer) mixer.timeScale = Number($id('speed-select').value);
  });

  // Track scrubbing
  const track = $id('blend-track');
  let scrubbing = false;
  track.addEventListener('mousedown', (e) => { scrubbing = true; scrub(e); });
  window.addEventListener('mousemove', (e) => { if (scrubbing) scrub(e); });
  window.addEventListener('mouseup', () => { scrubbing = false; });
  function scrub(e) {
    if (!mixer || duration <= 0) return;
    const rect = track.getBoundingClientRect();
    const pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    currentTime = pct * duration;
    seekAll(currentTime);
    updateTimelineDisplay();
  }

  // Clip selection
  document.querySelectorAll('.clip-select').forEach((sel) => {
    sel.addEventListener('change', () => {
      const ch = sel.dataset.channel;
      blend[ch].clipIdx = Number(sel.value);
      const idx = blend[ch].clipIdx;
      if (idx >= 0 && idx < actions.length) {
        blend[ch].action = actions[idx];
        actions[idx].weight = blend[ch].weight;
      }
      applyWeights();
    });
  });

  // Weight sliders
  document.querySelectorAll('.weight-slider').forEach((sl) => {
    const ch = sl.dataset.channel;
    sl.addEventListener('input', () => {
      const w = Number(sl.value);
      blend[ch].weight = w;
      $id('wval-' + ch).textContent = w.toFixed(2);
      applyWeights();
    });
  });

  // Crossfade
  $id('crossfade-btn').addEventListener('click', () => {
    // Smooth crossfade: animate A→0, B→1 over 1 second
    const dur = 1000;
    const startA = blend.a.weight;
    const startB = blend.b.weight;
    const t0 = performance.now();
    function step(now) {
      const t = Math.min(1, (now - t0) / dur);
      const ease = t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t; // ease-in-out
      const wA = startA * (1 - ease);
      const wB = startB + (1 - startB) * ease;
      blend.a.weight = Math.min(1, Math.max(0, wA));
      blend.b.weight = Math.min(1, Math.max(0, wB));
      document.querySelectorAll('.weight-slider').forEach((sl) => {
        const ch = sl.dataset.channel;
        if (ch === 'a') sl.value = String(Math.round(blend.a.weight * 100) / 100);
        if (ch === 'b') sl.value = String(Math.round(blend.b.weight * 100) / 100);
      });
      $id('wval-a').textContent = blend.a.weight.toFixed(2);
      $id('wval-b').textContent = blend.b.weight.toFixed(2);
      applyWeights();
      $id('info-blend').textContent = `Crossfade ${(t * 100).toFixed(0)}%`;
      if (t < 1) requestAnimationFrame(step);
      else $id('info-blend').textContent = 'Crossfade done';
    }
    $id('info-blend').textContent = 'Crossfade...';
    requestAnimationFrame(step);
  });

  // Equalize
  $id('equalize-btn').addEventListener('click', () => {
    blend.a.weight = 0.5;
    blend.b.weight = 0.5;
    document.querySelectorAll('.weight-slider').forEach((sl) => {
      const ch = sl.dataset.channel;
      if (ch === 'a') sl.value = '0.5';
      if (ch === 'b') sl.value = '0.5';
    });
    $id('wval-a').textContent = '0.50';
    $id('wval-b').textContent = '0.50';
    applyWeights();
    $id('info-blend').textContent = 'Equalized';
  });
}

function applyWeights() {
  if (actions.length === 0) return;
  for (const ch of ['a', 'b']) {
    const idx = blend[ch]?.clipIdx;
    if (idx != null && idx >= 0 && idx < actions.length) {
      actions[idx].weight = blend[ch]?.weight || 0;
    }
  }
  // If any channel weight is 0, stop that action; if > 0, ensure play
  for (let i = 0; i < actions.length; i++) {
    let totalWeight = 0;
    for (const ch of ['a', 'b']) {
      if (blend[ch]?.clipIdx === i) totalWeight += blend[ch]?.weight || 0;
    }
    if (totalWeight <= 0 && false) {
      // Don't stop — keep timeline synced; weight=0 effectively mutes
    }
  }
}

// ── Load USD ──

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
    loader.parse(data, filename, resolve, reject, { backend: 'next', maxMemoryLimitMB: 512 });
  });

  setStatus(`Building scene...`);
  if (mixer) { mixer.stopAllAction(); mixer = null; }
  actions = [];
  for (const h of skeletonHelpers) scene.remove(h);
  skeletonHelpers = [];
  world.clear();

  const metadata = getUSDSceneMetadata(usd);
  const fps = metadata.timeCodesPerSecond || 24;
  const built = await buildNextDemoScene(usd, { sourceUrl: url });
  const threeNode = built.node;
  world.add(threeNode);

  const skinData = extractSkinnedMeshData(usd, { logger: console, verbose: false });
  const skelData = buildSkeletonDataFromUSD(usd, { logger: console, hasSkinnedMeshData: skinData.hasSkinnedMeshData });
  const nodeIndexMap = built.nodeIndexMap || buildNodeIndexMap(threeNode);
  const r = applyUSDSceneSkinningPipeline({
    threeNode, characterGroup: world, helperScene: scene,
    skeletonDataArray: skelData.skeletonDataArray,
    allSkinnedMeshUSDData: skinData.allSkinnedMeshUSDData,
    skinnedMeshDataByName: skinData.skinnedMeshDataByName,
    usdScene: usd, showMesh: true, showSkeleton: true, useWASMBoneTexture: false, logger: console,
  });
  skeletonHelpers = r.skeletonHelpers || [];
  for (const h of skeletonHelpers) scene.add(h);

  const animData = extractUSDSceneAnimations(usd, {
    boneMaps: skelData.boneMaps, nodeIndexMap, timeCodesPerSecond: fps, logger: console,
  });
  clips = [...animData.usdAnimations, ...animData.usdNodeAnimations];
  if (clips.length === 0) { setStatus('No animation clips.'); return; }

  mixer = new THREE.AnimationMixer(world);
  mixer.timeScale = fps;
  actions = clips.map((c) => { const a = mixer.clipAction(c); a.play(); return a; });
  duration = clips.reduce((m, c) => Math.max(m, c.duration), 0);

  buildBlendUI();
  fitCamera();
  applyWeights();
  setStatus(`${label}: ${clips.length} clip(s), ${fps} fps`);
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
viewport.addEventListener('dragover', (e) => e.preventDefault());
viewport.addEventListener('drop', async (e) => {
  e.preventDefault();
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
  if (mixer) { mixer.stopAllAction(); mixer = null; }
  actions = [];
  for (const h of skeletonHelpers) scene.remove(h);
  skeletonHelpers = [];
  world.clear();
  const metadata = getUSDSceneMetadata(usd);
  const fps = metadata.timeCodesPerSecond || 24;
  const built = await buildNextDemoScene(usd);
  const threeNode = built.node;
  world.add(threeNode);
  const skinData = extractSkinnedMeshData(usd, { logger: console, verbose: false });
  const skelData = buildSkeletonDataFromUSD(usd, { logger: console, hasSkinnedMeshData: skinData.hasSkinnedMeshData });
  const nodeIndexMap = built.nodeIndexMap || buildNodeIndexMap(threeNode);
  const r = applyUSDSceneSkinningPipeline({ threeNode, characterGroup: world, helperScene: scene, skeletonDataArray: skelData.skeletonDataArray, allSkinnedMeshUSDData: skinData.allSkinnedMeshUSDData, skinnedMeshDataByName: skinData.skinnedMeshDataByName, usdScene: usd, showMesh: true, showSkeleton: true, useWASMBoneTexture: false, logger: console });
  skeletonHelpers = r.skeletonHelpers || [];
  for (const h of skeletonHelpers) scene.add(h);
  const animData = extractUSDSceneAnimations(usd, { boneMaps: skelData.boneMaps, nodeIndexMap, timeCodesPerSecond: fps, logger: console });
  clips = [...animData.usdAnimations, ...animData.usdNodeAnimations];
  if (clips.length === 0) { fitCamera(); setStatus('No animations.'); return; }
  mixer = new THREE.AnimationMixer(world);
  mixer.timeScale = fps;
  actions = clips.map((c) => { const a = mixer.clipAction(c); a.play(); return a; });
  duration = clips.reduce((m, c) => Math.max(m, c.duration), 0);
  buildBlendUI();
  fitCamera();
  applyWeights();
  setStatus(`Loaded ${file.name}`);
}

// ── UI ──
$id('load-btn').addEventListener('click', () => {
  const idx = Number($id('sample-select').value);
  loadURL(SAMPLES[idx].url, SAMPLES[idx].label).catch((e) => { console.error(e); setStatus('Error: ' + e.message); });
});
$id('fit-btn').addEventListener('click', fitCamera);

// ── Animation loop ──
let lastTime = performance.now();

function anim(now) {
  requestAnimationFrame(anim);
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;
  if (mixer && playing) {
    mixer.update(dt);
    // Collect current time from the weighted average of action times
    let weightedTime = 0;
    let totalWeight = 0;
    for (let i = 0; i < actions.length; i++) {
      let w = 0;
      for (const ch of ['a', 'b']) { if (blend[ch]?.clipIdx === i) w += blend[ch]?.weight || 0; }
      weightedTime += actions[i].time * w;
      totalWeight += w;
    }
    currentTime = totalWeight > 0 ? weightedTime / totalWeight : (actions[0]?.time || 0);
    updateTimelineDisplay();
  }
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
