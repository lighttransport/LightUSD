import * as THREE from 'three';
import { buildNextDemoScene } from '../next-scene.js';
import { showLoader, hideLoader } from '../lightusd-loader.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';

const SAMPLES = [
  { label: 'Suzanne PBR', url: './assets/suzanne-pbr.usda' },
  { label: 'Fancy Teapot (MTLX)', url: './assets/fancy-teapot-mtlx.usdz' },
  { label: 'Multi-clip skeleton', url: './assets/multi-clip-skeleton.usda' },
  { label: 'Robot Arm (Physics)', url: './assets/physics-robot-arm.usda' },
  { label: 'Sphere (GitHub)', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/sphere.usda' },
];

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(id) { return document.getElementById(id); }

// ── Shell ──
const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>USD Inspector</h1>
      <p>Explore the structure of a USD scene: prim hierarchy, stage metadata,
        mesh properties, and material parameters.</p>
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
    <aside class="inspector-panel">
      <div class="inspector-tabs">
        <div class="inspector-tab active" data-tab="tree">Tree</div>
        <div class="inspector-tab" data-tab="detail">Detail</div>
        <div class="inspector-tab" data-tab="stage">Stage</div>
      </div>
      <div class="inspector-content" id="tab-tree">
        <div id="tree-root" class="empty-state">Load a scene to inspect.</div>
      </div>
      <div class="inspector-content" id="tab-detail" hidden>
        <div id="detail-root" class="empty-state">Select a prim in the tree.</div>
      </div>
      <div class="inspector-content" id="tab-stage" hidden>
        <div id="stage-root" class="empty-state">Load a scene to see stage metadata.</div>
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
viewport.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.dampingFactor = 0.08;
controls.target.set(0, 0.5, 0);

const world = new THREE.Group();
scene.add(world);
scene.add(new THREE.HemisphereLight(0xdde8f6, 0x24272c, 1.2));
const keyLight = new THREE.DirectionalLight(0xffffff, 2);
keyLight.position.set(4, 6, 5);
keyLight.castShadow = true;
scene.add(keyLight);
scene.add(new THREE.GridHelper(8, 20, 0x44444a, 0x26262b));

let loader = null;
let nativeScene = null; // the parsed USD scene (for getMaterialWithFormat etc.)
let selectedPath = null;
let highlightMesh = null;

// ── Inspector state ──
let treeData = []; // flat { path, name, type, children: [...] }

// ── Tab switching ──

document.querySelectorAll('.inspector-tab').forEach((tab) => {
  tab.addEventListener('click', () => {
    document.querySelectorAll('.inspector-tab').forEach((t) => t.classList.remove('active'));
    tab.classList.add('active');
    document.querySelectorAll('.inspector-content').forEach((c) => c.hidden = true);
    const content = $id('tab-' + tab.dataset.tab);
    if (content) content.hidden = false;
  });
});

// ── Helpers ──

function setStatus(s) { $id('status').textContent = s; }

function nodeTypeBadge(nt) {
  const t = String(nt || 'unknown').toLowerCase();
  if (t === 'mesh') return '<span class="type-badge badge-mesh">mesh</span>';
  if (t === 'xform' || t === 'scope') return '<span class="type-badge badge-xform">xform</span>';
  return '<span class="type-badge badge-unknown">' + escapeHTML(nt || '?') + '</span>';
}

// ── Build tree from getRootNode() ──

function buildTreeFromUSD() {
  if (!nativeScene) return;
  const rootNode = nativeScene.getDefaultRootNode ? nativeScene.getDefaultRootNode() : null;
  if (!rootNode) { $id('tree-root').innerHTML = '<div class="empty-state">No scene tree available.</div>'; return; }
  treeData = [];
  renderTreeNode(rootNode, treeData, 0);
  renderTreeUI();
  renderStageMeta();
}

function renderTreeNode(usdNode, out, depth) {
  const entry = {
    path: usdNode.primPath || usdNode.absPath || '',
    name: usdNode.primName || usdNode.displayName || '(unnamed)',
    type: usdNode.nodeType || 'unknown',
    nodeType: usdNode.nodeType,
    nodeCategory: usdNode.nodeCategory,
    materialId: usdNode.materialId,
    contentId: usdNode.contentId,
    localMatrix: usdNode.localMatrix,
    worldMatrix: usdNode.worldMatrix,
    visible: usdNode.visible !== false,
    depth,
    children: [],
    threeChildren: usdNode.children || [],
  };
  out.push(entry);
  if (usdNode.children) {
    for (const child of usdNode.children) {
      renderTreeNode(child, entry.children, depth + 1);
    }
  }
}

function renderTreeUI() {
  const container = $id('tree-root');
  if (treeData.length === 0) {
    container.innerHTML = '<div class="empty-state">Empty scene.</div>';
    return;
  }
  container.innerHTML = '<div class="tree-children">' +
    treeData.map((n) => renderTreeItem(n)).join('') +
  '</div>';

  // Add click handlers for tree rows
  container.querySelectorAll('.tree-row').forEach((row) => {
    row.addEventListener('click', (e) => {
      e.stopPropagation();
      const path = row.dataset.path;
      selectTreeItem(path, row);
    });
  });
}

function renderTreeItem(node, expanded) {
  const hasChildren = node.children && node.children.length > 0;
  const arrow = hasChildren
    ? `<span class="arrow" data-toggle>${expanded !== false ? '▾' : '▸'}</span>`
    : '<span class="arrow"></span>';
  const badge = nodeTypeBadge(node.type);
  return `<div>
    <div class="tree-row${selectedPath === node.path ? ' selected' : ''}" data-path="${escapeHTML(node.path)}">
      ${arrow}
      ${badge}
      <span class="prim-name">${escapeHTML(node.name)}</span>
    </div>
    ${hasChildren ? `<div class="tree-children"${expanded === false ? ' style="display:none"' : ''}>
      ${node.children.map((c) => renderTreeItem(c, expanded)).join('')}
    </div>` : ''}
  </div>`;
}

function selectTreeItem(path, rowEl) {
  selectedPath = path;
  document.querySelectorAll('.tree-row').forEach((r) => r.classList.remove('selected'));
  if (rowEl) rowEl.classList.add('selected');
  showDetailForPath(path);
  highlightPrimInViewport(path);
}

// ── Detail panel ──

function showDetailForPath(path) {
  const node = findNodeByPath(treeData, path);
  const container = $id('detail-root');
  if (!node) {
    container.innerHTML = '<div class="empty-state">Prim not found: ' + escapeHTML(path) + '</div>';
    return;
  }

  let html = '<div class="detail-section"><h3>Prim</h3><table class="detail-table">';
  html += `<tr><td>Name</td><td>${escapeHTML(node.name)}</td></tr>`;
  html += `<tr><td>Path</td><td>${escapeHTML(node.path)}</td></tr>`;
  html += `<tr><td>Type</td><td>${escapeHTML(node.type || '—')}</td></tr>`;
  if (node.nodeCategory) html += `<tr><td>Category</td><td>${escapeHTML(node.nodeCategory)}</td></tr>`;
  html += `<tr><td>Visible</td><td>${node.visible ? 'Yes' : 'No'}</td></tr>`;
  html += '</table></div>';

  // Transform
  if (node.localMatrix) {
    html += '<div class="detail-section"><h3>Transform (local)</h3><table class="detail-table">';
    const m = node.localMatrix;
    for (let r = 0; r < 4; r++) {
      html += `<tr><td>Row ${r}</td><td>${m.slice(r * 4, r * 4 + 4).map((v) => v.toFixed(4)).join(', ')}</td></tr>`;
    }
    html += '</table></div>';
  }

  // Material binding
  if (node.materialId >= 0) {
    html += '<div class="detail-section"><h3>Material</h3>';
    try {
      const matData = nativeScene.getMaterialWithFormat
        ? nativeScene.getMaterialWithFormat(node.materialId, 'json')
        : null;
      if (matData && matData.data) {
        const parsed = JSON.parse(matData.data);
        html += '<table class="detail-table">';
        for (const [key, val] of Object.entries(parsed)) {
          if (key === 'textureMetadata' || key === 'materialXJson' || typeof val === 'object') continue;
          html += `<tr><td>${escapeHTML(key)}</td><td>${escapeHTML(JSON.stringify(val))}</td></tr>`;
        }
        html += '</table>';
      } else {
        html += `<p style="color:var(--dim)">Material ID: ${node.materialId}</p>`;
      }
    } catch { html += `<p style="color:var(--danger)">Error reading material</p>`; }
    html += '</div>';
  }

  // Children
  if (node.children && node.children.length > 0) {
    html += '<div class="detail-section"><h3>Children</h3>';
    html += '<div style="color:var(--muted);font-size:.78rem">' + node.children.length + ' prim(s)</div>';
    html += '</div>';
  }

  container.innerHTML = html;
}

function findNodeByPath(nodes, path) {
  for (const n of nodes) {
    if (n.path === path) return n;
    if (n.children) {
      const found = findNodeByPath(n.children, path);
      if (found) return found;
    }
  }
  return null;
}

// ── Highlight prim in viewport ──

function highlightPrimInViewport(path) {
  if (highlightMesh) { scene.remove(highlightMesh); highlightMesh.geometry?.dispose?.(); highlightMesh = null; }
  if (!path) return;
  world.traverse((obj) => {
    if (obj.userData?.primPath === path || obj.name === path || obj.name === path.split('/').pop()) {
      const box = new THREE.Box3().setFromObject(obj);
      if (box.isEmpty()) return;
      const center = box.getCenter(new THREE.Vector3());
      const size = box.getSize(new THREE.Vector3());
      const maxDim = Math.max(size.x, size.y, size.z, 0.01);
      const geo = new THREE.SphereGeometry(maxDim * 0.06, 16, 16);
      const mat = new THREE.MeshBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.6, depthTest: false });
      highlightMesh = new THREE.Mesh(geo, mat);
      highlightMesh.position.copy(center);
      scene.add(highlightMesh);
    }
  });
}

// ── Stage metadata ──

function renderStageMeta() {
  const container = $id('stage-root');
  if (!nativeScene) {
    container.innerHTML = '<div class="empty-state">Load a scene first.</div>';
    return;
  }
  const meta = nativeScene.getSceneMetadata ? nativeScene.getSceneMetadata() : {};
  const upAxis = nativeScene.getUpAxis ? nativeScene.getUpAxis() : '—';
  const stats = nativeScene.getStats ? nativeScene.getStats() : {};

  let html = '<div class="detail-section"><h3>Stage Metadata</h3><div class="metadata-grid">';
  html += `<span class="key">Up Axis</span><span class="val">${escapeHTML(upAxis)}</span>`;
  html += `<span class="key">Meters Per Unit</span><span class="val">${meta.metersPerUnit ?? '—'}</span>`;
  html += `<span class="key">Time Codes / Sec</span><span class="val">${meta.timeCodesPerSecond ?? '—'}</span>`;
  html += `<span class="key">Frames / Sec</span><span class="val">${meta.framesPerSecond ?? '—'}</span>`;
  html += `<span class="key">Start TimeCode</span><span class="val">${meta.startTimeCode ?? '—'}</span>`;
  html += `<span class="key">End TimeCode</span><span class="val">${meta.endTimeCode ?? '—'}</span>`;
  html += '</div></div>';

  // Mesh stats
  const numMeshes = nativeScene.numMeshes ? nativeScene.numMeshes() : 0;
  const numMaterials = nativeScene.numMaterials ? nativeScene.numMaterials() : 0;
  const numLights = nativeScene.numLights ? nativeScene.numLights() : 0;
  const numCameras = nativeScene.numCameras ? nativeScene.numCameras() : 0;

  html += '<div class="detail-section"><h3>Scene Stats</h3><div class="metadata-grid">';
  html += `<span class="key">Meshes</span><span class="val">${numMeshes}</span>`;
  html += `<span class="key">Materials</span><span class="val">${numMaterials}</span>`;
  html += `<span class="key">Lights</span><span class="val">${numLights}</span>`;
  html += `<span class="key">Cameras</span><span class="val">${numCameras}</span>`;
  html += '</div></div>';

  container.innerHTML = html;
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
  nativeScene = await new Promise((resolve, reject) => {
    loader.parse(data, filename, resolve, reject, {
      backend: 'next', maxMemoryLimitMB: 512,
    });
  });

  setStatus(`Building 3D scene...`);
  world.clear();
  if (highlightMesh) { scene.remove(highlightMesh); highlightMesh.geometry?.dispose?.(); highlightMesh = null; }

  const { node: threeNode } = await buildNextDemoScene(nativeScene, { sourceUrl: url });
  world.add(threeNode);

  buildTreeFromUSD();

  // Switch to tree tab
  document.querySelectorAll('.inspector-tab').forEach((t) => t.classList.remove('active'));
  document.querySelector('[data-tab="tree"]')?.classList.add('active');
  document.querySelectorAll('.inspector-content').forEach((c) => c.hidden = true);
  $id('tab-tree').hidden = false;

  fitCamera();
  setStatus(`Loaded ${label}: ${treeData.length} root prim(s)`);
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
  nativeScene = await new Promise((resolve, reject) => {
    loader.parse(data, file.name, resolve, reject, { backend: 'next', maxMemoryLimitMB: 512 });
  });
  world.clear();
  if (highlightMesh) { scene.remove(highlightMesh); highlightMesh.geometry?.dispose?.(); highlightMesh = null; }
  const { node: threeNode } = await buildNextDemoScene(nativeScene);
  world.add(threeNode);
  buildTreeFromUSD();
  document.querySelectorAll('.inspector-tab').forEach((t) => t.classList.remove('active'));
  document.querySelector('[data-tab="tree"]')?.classList.add('active');
  document.querySelectorAll('.inspector-content').forEach((c) => c.hidden = true);
  $id('tab-tree').hidden = false;
  fitCamera();
  setStatus(`Loaded ${file.name}`);
}

// ── UI ──

$id('load-btn').addEventListener('click', () => {
  const idx = Number($id('sample-select').value);
  const s = SAMPLES[idx];
  if (s) loadURL(s.url, s.label);
});
$id('fit-btn').addEventListener('click', fitCamera);

// ── Main loop ──

let lastTime = performance.now();
function anim(now) {
  requestAnimationFrame(anim);
  const dt = Math.min(0.05, (now - lastTime) / 1000);
  lastTime = now;
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

main().catch((e) => { console.error(e); setStatus(`Error: ${e.message}`); });
