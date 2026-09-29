import { StreamingUSDRenderer } from 'lightusd-js/streaming.js';
import { renderHttpUSD } from 'lightusd-js/http-asset-resolver.js';
import { ASSETS, CATEGORIES } from '../usd-assets-manifest.js';
import { Report } from '../app-report.js';

const GITHUB_RAW = 'https://raw.githubusercontent.com/usd-wg/assets/1b91f3c464891af259d51d9ee9ee9e6c357f7079/';

function assetUrl(a) { return GITHUB_RAW + a.repoPath + '/' + a.filename; }
function thumbnailUrl(a) { return GITHUB_RAW + a.repoPath + '/thumbnails/' + a.id + '.png'; }
function baseUrl(a) { return GITHUB_RAW + a.repoPath + '/'; }

function escapeHTML(v) {
  return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;');
}

function fmtMB(b) { return (b / 1048576).toFixed(1) + ' MB'; }

function terseName(s) {
  return String(s || '')
    .replace(/[_-]/g, ' ')
    .replace(/\b[a-z]/g, (c) => c.toUpperCase());
}

// ── App State ──
const state = {
  assets: ASSETS,
  activeCategory: 'all',
  searchQuery: '',
  selectedId: null,
  loading: false,
  fps: 0,
};

let renderer = null;

// ── DOM refs ──
const $ = (id) => document.getElementById(id);

// ── Build Shell ──
const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell assets-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>USD Assets Browser</h1>
      <p>Browse and preview curated USD assets from the
        <a href="https://github.com/usd-wg/assets" target="_blank" rel="noopener">usd-wg/assets</a> corpus.
        Assets are fetched from GitHub's raw CDN and rendered via LightUSD's HTTP asset resolver.</p>
    </div>
  </header>
  <main class="assets-main">
    <aside class="assets-catalog">
      <div class="assets-toolbar">
        <div class="assets-categories" id="category-bar"></div>
        <div class="assets-search-row">
          <input id="asset-search" type="text" spellcheck="false" placeholder="Filter assets by name or tag…" />
          <span class="assets-count" id="asset-count"></span>
        </div>
      </div>
      <div class="assets-grid" id="asset-grid"></div>
      <div class="assets-status-bar" id="catalog-status"></div>
    </aside>
    <section class="assets-viewport-wrap">
      <div class="assets-canvas-wrap">
        <canvas id="asset-canvas"></canvas>
        <div class="assets-drop-hint" id="drop-hint">Drop USDA, USDC, USD, or USDZ</div>
        <div class="assets-viewer-overlay" id="viewer-overlay">
          <div class="assets-viewer-placeholder" id="viewer-placeholder">
            <svg width="64" height="64" viewBox="0 0 64 64" fill="none">
              <rect x="8" y="12" width="48" height="40" rx="4" stroke="#474751" stroke-width="2" fill="none"/>
              <path d="M8 44l14-12 10 8 12-10 12 10v4H8z" fill="#26262c" stroke="#474751" stroke-width="1"/>
              <circle cx="22" cy="24" r="4" fill="#26262c" stroke="#474751" stroke-width="1"/>
            </svg>
            <p>Select an asset from the catalog to preview</p>
          </div>
        </div>
      </div>
      <div class="assets-status" id="asset-status">Select an asset</div>
      <div class="assets-stats" id="asset-stats">
        <div class="assets-stats-inner">
          <div class="asset-stat"><span class="stat-label">Meshes</span><span class="stat-value" id="stat-meshes">—</span></div>
          <div class="asset-stat"><span class="stat-label">Materials</span><span class="stat-value" id="stat-materials">—</span></div>
          <div class="asset-stat"><span class="stat-label">Textures</span><span class="stat-value" id="stat-textures">—</span></div>
          <div class="asset-stat"><span class="stat-label">Input</span><span class="stat-value" id="stat-input">—</span></div>
          <div class="asset-stat"><span class="stat-label">Fetched</span><span class="stat-value" id="stat-fetched">—</span></div>
          <div class="asset-stat"><span class="stat-label">Load time</span><span class="stat-value" id="stat-loadtime">—</span></div>
          <div class="asset-stat"><span class="stat-label">FPS</span><span class="stat-value" id="stat-fps">—</span></div>
          <div class="asset-stat"><span class="stat-label">Backend</span><span class="stat-value" id="stat-backend">—</span></div>
        </div>
      </div>
      <div class="assets-warning" id="rate-warning" hidden>
        <strong>GitHub rate limit reached.</strong>
        Unauthenticated requests are limited to 60/hr. Switch assets carefully.
      </div>
    </section>
  </main>
  <input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden>
</div>`;

// ── Init ──
init();

async function init() {
  renderer = new StreamingUSDRenderer($('asset-canvas'));
  showLoader("Loading LightUSD WASM...", document.getElementById("viewport"));
    try {
      await renderer.init()
    } finally {
      hideLoader();
    };

  startFpsMeter((fps) => { state.fps = fps; updateStats({ fps }); });

  renderer.onMemory((m) => {
    const el = $('asset-stats');
    if (!el) return;
    const heap = m.heapReserved || 0;
    const ratio = m.inputBytes ? (heap / m.inputBytes).toFixed(2) : '—';
    el.dataset.heap = fmtMB(heap);
    el.dataset.ratio = ratio;
  });

  renderer.setClearColor([0.12, 0.12, 0.14, 1.0]);

  setupFilterBar();
  setupSearch();
  setupFileDrop();
  renderCatalog();

  // Auto-load from ?uri= or ?asset= param
  const params = new URLSearchParams(location.search);
  const assetId = params.get('asset') || params.get('id');
  if (assetId) {
    const match = state.assets.find((a) => a.id === assetId);
    if (match) {
      selectAsset(match);
      scrollToAsset(match.id);
    }
  }
}

// ── FPS ──
function startFpsMeter(onFps) {
  let frames = 0;
  let last = performance.now();
  const tick = (now) => {
    frames++;
    if (now - last >= 500) {
      onFps(Math.round((frames * 1000) / (now - last)));
      frames = 0;
      last = now;
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

// ── Filter / Search ──
function setupFilterBar() {
  const bar = $('category-bar');
  bar.innerHTML = '<button class="cat-btn active" data-cat="all">All</button>' +
    CATEGORIES.map((c) =>
      `<button class="cat-btn" data-cat="${c.id}">${escapeHTML(c.label)}</button>`).join('');
  bar.addEventListener('click', (e) => {
    const btn = e.target.closest('.cat-btn');
    if (!btn) return;
    bar.querySelectorAll('.cat-btn').forEach((b) => b.classList.remove('active'));
    btn.classList.add('active');
    state.activeCategory = btn.dataset.cat;
    renderCatalog();
  });
}

function setupSearch() {
  const input = $('asset-search');
  let timer = null;
  input.addEventListener('input', () => {
    clearTimeout(timer);
    timer = setTimeout(() => {
      state.searchQuery = input.value.trim().toLowerCase();
      renderCatalog();
    }, 200);
  });
}

function setupFileDrop() {
  const dropHint = $('drop-hint');
  const fileInput = $('file-input');
  const canvasWrap = $('asset-canvas').parentElement;

  $('file-input').addEventListener('change', () => {
    const file = fileInput.files?.[0];
    if (file) loadLocalFile(file);
    fileInput.value = '';
  });

  canvasWrap.addEventListener('dragenter', (e) => { e.preventDefault(); dropHint.classList.add('active'); });
  canvasWrap.addEventListener('dragover', (e) => { e.preventDefault(); dropHint.classList.add('active'); });
  canvasWrap.addEventListener('dragleave', () => { dropHint.classList.remove('active'); });
  canvasWrap.addEventListener('drop', (e) => {
    e.preventDefault();
    dropHint.classList.remove('active');
    const file = e.dataTransfer?.files?.[0];
    if (file) loadLocalFile(file);
  });
}

// ── Catalog Rendering ──
function filteredAssets() {
  return state.assets.filter((a) => {
    if (state.activeCategory !== 'all' && a.category !== state.activeCategory) return false;
    if (state.searchQuery) {
      const q = state.searchQuery;
      const text = (a.name + ' ' + a.id + ' ' + a.category + ' ' + (a.tags || []).join(' ') + ' ' + (a.description || '')).toLowerCase();
      if (!text.includes(q)) return false;
    }
    return true;
  });
}

function renderCatalog() {
  const grid = $('asset-grid');
  const results = filteredAssets();
  $('asset-count').textContent = `${results.length} / ${state.assets.length}`;

  if (results.length === 0) {
    grid.innerHTML = `<div class="assets-empty">No assets match your filter.</div>`;
    return;
  }

  grid.innerHTML = results.map((a) => {
    const selected = state.selectedId === a.id ? ' selected' : '';
    const catLabel = CATEGORIES.find((c) => c.id === a.category)?.label || terseName(a.category);
    return `
      <article class="asset-card${selected}" data-asset-id="${a.id}">
        <div class="asset-thumb">
          <img src="${thumbnailUrl(a)}" alt="${escapeHTML(a.name)}" loading="lazy"
               onerror="this.style.display='none';this.nextElementSibling.style.display='flex'" />
          <div class="asset-thumb-placeholder"><span>${escapeHTML(a.name.charAt(0).toUpperCase())}</span></div>
        </div>
        <div class="asset-info">
          <h3 class="asset-name">${escapeHTML(a.name)}</h3>
          <span class="asset-cat">${escapeHTML(catLabel)}</span>
          <p class="asset-desc">${escapeHTML(a.description || '')}</p>
        </div>
      </article>
    `;
  }).join('');

  grid.querySelectorAll('.asset-card').forEach((card) => {
    card.addEventListener('click', () => {
      const id = card.dataset.assetId;
      const asset = state.assets.find((a) => a.id === id);
      if (asset) selectAsset(asset);
    });
  });
}

function scrollToAsset(id) {
  const card = document.querySelector(`.asset-card[data-asset-id="${id}"]`);
  if (card) card.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

// ── Asset Selection / Loading ──
async function selectAsset(asset) {
  if (state.loading) return;
  if (state.selectedId === asset.id) return;

  state.selectedId = asset.id;
  state.loading = true;

  document.querySelectorAll('.asset-card').forEach((c) => c.classList.remove('selected'));
  const card = document.querySelector(`.asset-card[data-asset-id="${asset.id}"]`);
  if (card) card.classList.add('selected');

  const status = $('asset-status');
  const overlay = $('viewer-overlay');
  const placeholder = $('viewer-placeholder');
  const warning = $('rate-warning');

  warning.hidden = true;
  overlay.style.display = 'flex';
  placeholder.style.display = 'flex';
  placeholder.querySelector('p').textContent = `Loading ${asset.name}...`;

  status.textContent = `Fetching ${asset.filename} from GitHub...`;

  try {
    const url = assetUrl(asset);
    const base = baseUrl(asset);
    const resp = await fetch(url, { cache: 'no-store' });

    if (resp.status === 403 || resp.status === 429) {
      warning.hidden = false;
      throw new Error(`GitHub rate limit (HTTP ${resp.status}). Try again later.`);
    }
    if (!resp.ok && resp.status !== 206) {
      throw new Error(`HTTP ${resp.status} fetching ${asset.filename}`);
    }

    const rootBytes = new Uint8Array(await resp.arrayBuffer());
    const filename = asset.filename;
    const label = asset.name;

    const loadStart = performance.now();
    status.textContent = `Composing ${label}...`;
    const loaded = await renderHttpUSD({
      renderer,
      rootBytes,
      filename,
      baseUrl: base,
      label,
      backend: 'next',
      onStatus: (msg) => { status.textContent = msg; },
    });
    const elapsed = performance.now() - loadStart;

    overlay.style.display = 'none';
    status.textContent = `${label}: ${loaded.result.meshes} meshes, ${loaded.result.textures} textures, ${loaded.result.materials} materials — ` +
      `${loaded.backend} backend — ${elapsed.toFixed(0)} ms`;

    updateStats({
      meshes: loaded.result.meshes,
      materials: loaded.result.materials,
      textures: loaded.result.textures,
      input: fmtMB(rootBytes.byteLength),
      fetched: fmtMB(loaded.fetchedBytes),
      loadtime: elapsed.toFixed(0) + ' ms',
      backend: loaded.backend + ' / ' + loaded.shadeLabel,
    });

    if (loaded.fetchLog && loaded.fetchLog.length > 1) {
      const tex = loaded.fetchLog.filter((f) => f.ok && f.path !== filename).length;
      if (tex > 0) status.textContent += ` (${tex} texture(s) resolved over HTTP)`;
    }
  } catch (e) {
    console.error('Failed to load', asset.id, e);
    status.textContent = `Failed: ${e.message}`;
    placeholder.style.display = 'flex';
    placeholder.querySelector('p').textContent = `Failed to load ${asset.name}: ${e.message}`;
    state.selectedId = null;
    document.querySelectorAll('.asset-card').forEach((c) => c.classList.remove('selected'));
    updateStats({});
    Report.err(e, `Loading ${asset.name}`).action('Dismiss', () => Report.dismiss());
  } finally {
    state.loading = false;
  }
}

async function loadLocalFile(file) {
  if (state.loading) return;
  if (!/\.(usd|usda|usdc|usdz)$/i.test(file.name)) {
    $('asset-status').textContent = 'Drop or select a USD file (.usd, .usda, .usdc, .usdz).';
    return;
  }

  state.loading = true;
  document.querySelectorAll('.asset-card').forEach((c) => c.classList.remove('selected'));
  const status = $('asset-status');
  const overlay = $('viewer-overlay');
  const placeholder = $('viewer-placeholder');

  overlay.style.display = 'flex';
  placeholder.style.display = 'flex';
  placeholder.querySelector('p').textContent = `Loading ${file.name}...`;

  status.textContent = `Reading ${file.name}...`;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const loadStart = performance.now();
    status.textContent = `Composing ${file.name}...`;
    const loaded = await renderHttpUSD({
      renderer,
      rootBytes: bytes,
      filename: file.name,
      baseUrl: '',
      label: file.name,
      backend: 'next',
      onStatus: (msg) => { status.textContent = msg; },
    });
    const elapsed = performance.now() - loadStart;
    overlay.style.display = 'none';
    status.textContent = `${file.name}: ${loaded.result.meshes} meshes, ${loaded.result.textures} textures — ${elapsed.toFixed(0)} ms`;
    updateStats({
      meshes: loaded.result.meshes,
      materials: loaded.result.materials,
      textures: loaded.result.textures,
      input: fmtMB(bytes.byteLength),
      fetched: fmtMB(loaded.fetchedBytes),
      loadtime: elapsed.toFixed(0) + ' ms',
      backend: loaded.backend + ' / ' + loaded.shadeLabel,
    });
    state.selectedId = '__local__';
  } catch (e) {
    status.textContent = `Failed: ${e.message}`;
    placeholder.querySelector('p').textContent = `Error: ${e.message}`;
  } finally {
    state.loading = false;
  }
}

// ── Stats ──
function updateStats(values = {}) {
  const set = (id, v) => { const el = $(id); if (el) el.textContent = v || '—'; };
  set('stat-meshes', values.meshes != null ? values.meshes : '—');
  set('stat-materials', values.materials != null ? values.materials : '—');
  set('stat-textures', values.textures != null ? values.textures : '—');
  set('stat-input', values.input || '—');
  set('stat-fetched', values.fetched || '—');
  set('stat-loadtime', values.loadtime || '—');
  set('stat-fps', values.fps != null ? values.fps : (state.fps || '—'));
  set('stat-backend', values.backend || '—');
}

// ── Expose for debugging ──
window.__usdAssetsBrowser = { state, renderer, ASSETS };
import { showLoader, hideLoader } from "../lightusd-loader.js";
