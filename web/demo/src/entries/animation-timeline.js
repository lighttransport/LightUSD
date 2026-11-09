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
  { label: 'Animated Box', url: 'https://raw.githubusercontent.com/usd-wg/assets/refs/heads/main/test_assets/USDZ/BoxAnimated/BoxAnimated.usdz' },
];

function escapeHTML(v) {
  return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
}

function fmt(s, d) { return Number(s).toFixed(d); }

// ── Shell ──
const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell" style="grid-template-rows:auto 1fr auto">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>Animation Timeline</h1>
      <p>Scrub through USD skeletal and node animations. Load a multi-clip
        asset to switch between animation clips.</p>
    </div>
    <div class="demo-actions">
      <select id="sample-select">
        ${SAMPLES.map((s, i) => `<option value="${i}"${i===0?' selected':''}>${escapeHTML(s.label)}</option>`).join('')}
      </select>
      <button id="load-btn" type="button">Load</button>
      <button id="fit-btn" type="button">Fit</button>
    </div>
  </header>
  <main class="demo-main" style="grid-template-rows:1fr">
    <section class="viewport-wrap" style="position:relative">
      <div id="viewport" class="viewport"></div>
      <div id="status" class="status">Initializing...</div>
      <div class="timeline-bar" id="timeline-bar">
        <button id="play-btn" class="active" title="Play/Pause">Pause</button>
        <button id="loop-btn" class="active" title="Loop">Loop</button>
        <div class="timeline-track" id="timeline-track">
          <div class="timeline-bg"><div class="timeline-fill" id="timeline-fill"></div></div>
          <div class="timeline-scrubber" id="timeline-scrubber" style="left:0"></div>
        </div>
        <div class="timeline-time" id="time-display">0:00.000 / 0:00.000</div>
        <div class="timeline-info">
          <span class="label">Speed</span>
          <select id="speed-select">
            <option value="0.25">0.25×</option>
            <option value="0.5">0.5×</option>
            <option value="1" selected>1×</option>
            <option value="2">2×</option>
            <option value="4">4×</option>
          </select>
        </div>
        <div class="timeline-info">
          <span class="label" id="clip-label">Clip</span>
          <select id="clip-select"></select>
        </div>
      </div>
    </section>
  </main>
  <input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden>
</div>`;

// ── DOM ──
const $ = (id) => document.getElementById(id);
const viewport = $('viewport');
const statusEl = $('status');
const playBtn = $('play-btn');
const loopBtn = $('loop-btn');
const timelineTrack = $('timeline-track');
const timelineFill = $('timeline-fill');
const timelineScrubber = $('timeline-scrubber');
const timeDisplay = $('time-display');
const speedSelect = $('speed-select');
const clipSelect = $('clip-select');
const clipLabel = $('clip-label');
const fileInput = $('file-input');

function setStatus(s) { statusEl.textContent = s; }

// ── Three.js ──
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

let loader = null;
let mixer = null;
let actions = [];
let skeletonHelpers = [];
let duration = 0;
let playing = true;
let looping = true;
let isScrubbing = false;

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

function clearScene() {
  if (mixer) { mixer.stopAllAction(); mixer = null; }
  actions = [];
  for (const h of skeletonHelpers) scene.remove(h);
  skeletonHelpers = [];
  world.clear();
  duration = 0;
}

function updateTimeline(time, dur) {
  const pct = dur > 0 ? (time / dur) * 100 : 0;
  timelineFill.style.width = Math.min(pct, 100) + '%';
  timelineScrubber.style.left = Math.min(pct, 100) + '%';

  const totalSec = dur;
  const curSec = time;
  const fmtTime = (t) => {
    const m = Math.floor(t / 60);
    const s = t % 60;
    return `${m}:${s.toFixed(3).padStart(6, '0')}`;
  };
  timeDisplay.textContent = `${fmtTime(curSec)} / ${fmtTime(totalSec)}`;
}

function seekAll(time) {
  if (!mixer) return;
  for (const a of actions) {
    a.time = Math.max(0, Math.min(time, a.getClip().duration));
    a.play();
  }
}

// ── Load USD ──

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
      backend: 'next',
      maxMemoryLimitMB: 512,
    });
  });

  setStatus(`Building scene...`);
  clearScene();

  const metadata = getUSDSceneMetadata(usd);
  const fps = metadata.timeCodesPerSecond || 24;
  const built = await buildNextDemoScene(usd, { sourceUrl: url });
  const threeNode = built.node;
  world.add(threeNode);

  setStatus(`Extracting skeleton/animation...`);
  const skinData = extractSkinnedMeshData(usd, { logger: console, verbose: false });
  const skelData = buildSkeletonDataFromUSD(usd, {
    logger: console,
    hasSkinnedMeshData: skinData.hasSkinnedMeshData,
  });
  const nodeIndexMap = built.nodeIndexMap || buildNodeIndexMap(threeNode);
  const skinResult = applyUSDSceneSkinningPipeline({
    threeNode, characterGroup: world, helperScene: scene,
    skeletonDataArray: skelData.skeletonDataArray,
    allSkinnedMeshUSDData: skinData.allSkinnedMeshUSDData,
    skinnedMeshDataByName: skinData.skinnedMeshDataByName,
    usdScene: usd, showMesh: true, showSkeleton: true, useWASMBoneTexture: false,
    logger: console,
  });
  skeletonHelpers = skinResult.skeletonHelpers || [];
  for (const h of skeletonHelpers) scene.add(h);

  const animData = extractUSDSceneAnimations(usd, {
    boneMaps: skelData.boneMaps, nodeIndexMap,
    timeCodesPerSecond: fps, logger: console,
  });

  const clips = [...animData.usdAnimations, ...animData.usdNodeAnimations];
  if (clips.length === 0) {
    setStatus(`No animations in ${label}`);
    fitCamera();
    return;
  }

  mixer = new THREE.AnimationMixer(world);
  mixer.timeScale = fps;
  actions = clips.map((clip) => {
    const a = mixer.clipAction(clip);
    a.play();
    return a;
  });

  // Calculate total duration across all clips
  duration = clips.reduce((max, c) => Math.max(max, c.duration), 0);

  // Populate clip selector
  clipSelect.innerHTML = clips.map((c, i) =>
    `<option value="${i}">${c.name || `Clip ${i}`} (${c.duration.toFixed(2)}s)</option>`
  ).join('');
  clipLabel.textContent = clips.length > 1 ? 'Clip' : 'Clips: ' + clips.length;

  playing = true;
  playBtn.textContent = 'Pause';
  playBtn.classList.add('active');

  fitCamera();
  updateTimeline(0, duration);
  setStatus(`${label}: ${clips.length} clip(s), ${fps} fps`);
}

// ── File drop ──

fileInput.addEventListener('change', () => {
  const file = fileInput.files?.[0];
  if (file) loadLocalFile(file);
  fileInput.value = '';
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
  // Reuse the same loading pipeline
  clearScene();
  const metadata = getUSDSceneMetadata(usd);
  const fps = metadata.timeCodesPerSecond || 24;
  const built = await buildNextDemoScene(usd);
  const threeNode = built.node;
  world.add(threeNode);
  const skinData = extractSkinnedMeshData(usd, { logger: console, verbose: false });
  const skelData = buildSkeletonDataFromUSD(usd, { logger: console, hasSkinnedMeshData: skinData.hasSkinnedMeshData });
  const nodeIndexMap = built.nodeIndexMap || buildNodeIndexMap(threeNode);
  const skinResult = applyUSDSceneSkinningPipeline({
    threeNode, characterGroup: world, helperScene: scene,
    skeletonDataArray: skelData.skeletonDataArray,
    allSkinnedMeshUSDData: skinData.allSkinnedMeshUSDData,
    skinnedMeshDataByName: skinData.skinnedMeshDataByName,
    usdScene: usd, showMesh: true, showSkeleton: true, useWASMBoneTexture: false, logger: console,
  });
  skeletonHelpers = skinResult.skeletonHelpers || [];
  for (const h of skeletonHelpers) scene.add(h);
  const animData = extractUSDSceneAnimations(usd, { boneMaps: skelData.boneMaps, nodeIndexMap, timeCodesPerSecond: fps, logger: console });
  const clips = [...animData.usdAnimations, ...animData.usdNodeAnimations];
  if (clips.length === 0) { fitCamera(); setStatus(`No animations in ${file.name}`); return; }
  mixer = new THREE.AnimationMixer(world);
  mixer.timeScale = fps;
  actions = clips.map((clip) => { const a = mixer.clipAction(clip); a.play(); return a; });
  duration = clips.reduce((max, c) => Math.max(max, c.duration), 0);
  clipSelect.innerHTML = clips.map((c, i) => `<option value="${i}">${c.name || `Clip ${i}`} (${c.duration.toFixed(2)}s)</option>`).join('');
  clipLabel.textContent = clips.length > 1 ? 'Clip' : 'Clips: ' + clips.length;
  playing = true;
  playBtn.textContent = 'Pause';
  playBtn.classList.add('active');
  fitCamera();
  updateTimeline(0, duration);
  setStatus(`${file.name}: ${clips.length} clip(s)`);
}

// ── Timeline scrubbing ──

timelineTrack.addEventListener('mousedown', (e) => { isScrubbing = true; scrub(e); });
window.addEventListener('mousemove', (e) => { if (isScrubbing) scrub(e); });
window.addEventListener('mouseup', () => { isScrubbing = false; });

function scrub(e) {
  if (!mixer || duration <= 0) return;
  const rect = timelineTrack.getBoundingClientRect();
  const pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
  const time = pct * duration;
  seekAll(time);
  updateTimeline(time, duration);
}

// ── UI ──

playBtn.addEventListener('click', () => {
  playing = !playing;
  playBtn.textContent = playing ? 'Pause' : 'Play';
  playBtn.classList.toggle('active', playing);
  if (playing) {
    for (const a of actions) a.play();
  } else {
    for (const a of actions) a.paused = true;
  }
});

loopBtn.addEventListener('click', () => {
  looping = !looping;
  loopBtn.classList.toggle('active', looping);
  if (mixer) mixer.setTime(0);
  for (const a of actions) a.loop = looping ? THREE.LoopRepeat : THREE.LoopOnce;
  if (!playing) { playing = true; playBtn.textContent = 'Pause'; playBtn.classList.add('active'); for (const a of actions) { a.paused = false; a.play(); } }
});

speedSelect.addEventListener('change', () => {
  if (mixer) mixer.timeScale = Number(speedSelect.value) * (mixer.timeScale > 0 ? 1 : -1);
});

clipSelect.addEventListener('change', () => {
  // Show only the selected clip's duration range by resetting all actions
  // to the selected clip's range
  const idx = Number(clipSelect.value);
  if (idx >= 0 && idx < actions.length) {
    const clip = actions[idx].getClip();
    duration = clip.duration;
    seekAll(0);
    updateTimeline(0, duration);
  }
});

$('load-btn').addEventListener('click', () => {
  const idx = Number($('sample-select').value);
  const s = SAMPLES[idx];
  if (s) loadURL(s.url, s.label);
});
$('fit-btn').addEventListener('click', fitCamera);

// ── Animation loop ──

let lastTime = performance.now();

function anim(now) {
  requestAnimationFrame(anim);
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;

  if (mixer && playing && !isScrubbing) {
    mixer.update(dt);
    let curTime = 0;
    for (const a of actions) curTime = Math.max(curTime, a.time);
    // Check if all actions finished (LoopOnce)
    if (!looping) {
      let allDone = true;
      for (const a of actions) { if (!a.paused) allDone = false; }
      if (allDone) { playing = false; playBtn.textContent = 'Play'; playBtn.classList.remove('active'); }
    }
    updateTimeline(curTime, duration);
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

  // Auto-load from URL param or default
  const params = new URLSearchParams(location.search);
  const url = params.get('url') || params.get('uri');
  if (url) {
    await loadURL(url, url.split('/').pop());
  } else {
    const first = SAMPLES[0];
    await loadURL(first.url, first.label);
  }

  if (!mixer) setStatus('No animated asset loaded.');
  requestAnimationFrame(anim);
}

main().catch((e) => { console.error(e); setStatus(`Error: ${e.message}`); });
