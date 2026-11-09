import { StreamingUSDRenderer } from 'lightusd-js/streaming.js';
import { renderHttpUSD } from 'lightusd-js/http-asset-resolver.js';

const SAMPLES = [
  { label: 'Normals Texture', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usda' },
  { label: 'MaterialX Textured', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/MaterialXTest/basicTextured_flatten.usda' },
  { label: 'Damaged Helmet', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz' },
  { label: 'Cesium Man', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/USDZ/CesiumMan/CesiumMan.usdz' },
  { label: 'Sphere (simple)', url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/sphere.usda' },
];

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(id) { return document.getElementById(id); }
function fmtMB(b) { return (b / 1048576).toFixed(1) + ' MB'; }
function fmtTime(ms) { return ms.toFixed(0) + ' ms'; }

// ── Shell ──

const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>Streaming Loading Viz</h1>
      <p>Visualizes the WASM heap timeline and HTTP fetch waterfall during
        USD asset loading and composition.</p>
    </div>
    <div class="demo-actions">
      <select id="sample-select">
        ${SAMPLES.map((s, i) => `<option value="${i}"${i===0?' selected':''}>${escapeHTML(s.label)}</option>`).join('')}
      </select>
      <button id="load-btn" type="button">Load</button>
      <button id="fit-btn" type="button">Fit</button>
    </div>
  </header>
  <main class="assets-main" style="grid-template-columns:minmax(0,1fr) minmax(360px,480px)">
    <section class="assets-viewport-wrap">
      <div class="assets-canvas-wrap">
        <canvas id="viz-canvas"></canvas>
        <div class="assets-viewer-overlay" id="viewer-overlay">
          <div class="assets-viewer-placeholder" id="viewer-placeholder">
            <p>Select a sample and click Load.</p>
          </div>
        </div>
      </div>
      <div class="assets-status" id="viz-status">Ready.</div>
    </section>
    <aside class="viz-panel">
      <div class="viz-section">
        <h3>WASM Heap Timeline</h3>
        <canvas id="heap-chart" class="viz-canvas"></canvas>
      </div>
      <div class="viz-section">
        <h3>Summary</h3>
        <div class="viz-summary" id="summary-grid">
          <div class="viz-stat"><div class="val" id="s-peak">—</div><div class="lbl">Peak Heap</div></div>
          <div class="viz-stat"><div class="val" id="s-input">—</div><div class="lbl">Input Size</div></div>
          <div class="viz-stat"><div class="val" id="s-ratio">—</div><div class="lbl">Heap / Input</div></div>
          <div class="viz-stat"><div class="val" id="s-fetches">—</div><div class="lbl">HTTP Fetches</div></div>
          <div class="viz-stat"><div class="val" id="s-fetched">—</div><div class="lbl">Fetched Bytes</div></div>
          <div class="viz-stat"><div class="val" id="s-loadtime">—</div><div class="lbl">Load Time</div></div>
        </div>
      </div>
      <div class="viz-section">
        <h3>Phases</h3>
        <table class="viz-table" id="phases-table">
          <thead><tr><th>Phase</th><th>Heap</th><th>Buffers</th></tr></thead>
          <tbody id="phases-body"></tbody>
        </table>
      </div>
      <div class="viz-section">
        <h3>Fetch Waterfall</h3>
        <canvas id="fetch-chart" class="viz-canvas"></canvas>
        <table class="viz-table" id="fetch-table">
          <thead><tr><th>#</th><th>Path</th><th>Size</th><th>Status</th></tr></thead>
          <tbody id="fetch-body"></tbody>
        </table>
      </div>
    </aside>
  </main>
</div>`;

// ── Canvas drawing helpers ──

function drawHeapChart(canvas, phases, peakMB) {
  const ctx = canvas.getContext('2d');
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  ctx.scale(dpr, dpr);
  const w = rect.width;
  const h = rect.height;
  ctx.clearRect(0, 0, w, h);

  if (!phases || phases.length === 0) {
    ctx.fillStyle = '#474751';
    ctx.font = '12px system-ui';
    ctx.textAlign = 'center';
    ctx.fillText('No data', w / 2, h / 2);
    return;
  }

  const pad = { t: 16, r: 10, b: 22, l: 48 };
  const cw = w - pad.l - pad.r;
  const ch = h - pad.t - pad.b;

  // Find max
  const maxVal = Math.max(1, ...phases.map((p) => p.heapReserved));
  const barW = Math.max(4, Math.min(32, cw / phases.length - 2));

  // Grid lines
  ctx.strokeStyle = '#202025';
  ctx.lineWidth = 0.5;
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + ch - (ch * i) / 4;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
  }

  // Y-axis labels
  ctx.fillStyle = '#71717a';
  ctx.font = '9px system-ui';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + ch - (ch * i) / 4;
    ctx.fillText(fmtMB((maxVal * i) / 4), pad.l - 4, y);
  }

  // Bars
  for (let i = 0; i < phases.length; i++) {
    const p = phases[i];
    const x = pad.l + (cw * i) / phases.length + (cw / phases.length - barW) / 2;
    const barH = (p.heapReserved / maxVal) * ch;
    const y = pad.t + ch - barH;

    // Color by phase type
    const label = (p.label || '').toLowerCase();
    let color;
    if (label.includes('reset') || label.includes('free')) color = '#34d399';
    else if (label.includes('texture')) color = '#38bdf8';
    else if (label.includes('mesh')) color = '#a78bfa';
    else if (label.includes('peak')) color = '#f59e0b';
    else color = '#6366f1';

    ctx.fillStyle = color;
    ctx.fillRect(x, y, barW, barH);

    // Render buffers overlay
    if (p.renderBuffers > 0) {
      const bufH = (p.renderBuffers / maxVal) * ch;
      ctx.fillStyle = 'rgba(255,255,255,0.12)';
      ctx.fillRect(x, pad.t + ch - bufH, barW, bufH);
    }

    // X-axis label
    ctx.fillStyle = '#71717a';
    ctx.font = '8px system-ui';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    const shortLabel = p.label.length > 12 ? p.label.slice(0, 11) + '…' : p.label;
    ctx.fillText(shortLabel, x + barW / 2, pad.t + ch + 4);
  }
}

function drawFetchChart(canvas, fetchLog) {
  const ctx = canvas.getContext('2d');
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  canvas.width = rect.width * dpr;
  canvas.height = rect.height * dpr;
  ctx.scale(dpr, dpr);
  const w = rect.width;
  const h = rect.height;
  ctx.clearRect(0, 0, w, h);

  if (!fetchLog || fetchLog.length === 0) {
    ctx.fillStyle = '#474751';
    ctx.font = '12px system-ui';
    ctx.textAlign = 'center';
    ctx.fillText('No fetches', w / 2, h / 2);
    return;
  }

  const ok = fetchLog.filter((f) => f.ok);
  const maxBytes = Math.max(1, ...fetchLog.map((f) => f.bytes || 0));
  const pad = { t: 10, r: 10, b: 10, l: 10 };
  const cw = w - pad.l - pad.r;
  const rowH = Math.max(10, Math.min(20, (h - pad.t - pad.b) / fetchLog.length));
  const barMaxW = cw * 0.65;

  for (let i = 0; i < fetchLog.length; i++) {
    const f = fetchLog[i];
    const y = pad.t + i * rowH;
    const barW = f.bytes ? (f.bytes / maxBytes) * barMaxW : 0;

    ctx.fillStyle = f.ok ? '#38bdf8' : '#f08a8a';
    ctx.fillRect(pad.l, y + 1, Math.max(2, barW), rowH - 2);

    ctx.fillStyle = '#71717a';
    ctx.font = '8px system-ui';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    const path = (f.path || f.url || '?').split('/').pop() || '?';
    ctx.fillText(path, pad.l + barW + 4, y + rowH / 2);
  }
}

function renderPhasesTable(phases) {
  const body = $id('phases-body');
  if (!phases || phases.length === 0) {
    body.innerHTML = '<tr><td class="muted" colspan="3">No phase data.</td></tr>';
    return;
  }
  body.innerHTML = phases.map((p) =>
    `<tr><td>${escapeHTML(p.label)}</td><td>${fmtMB(p.heapReserved)}</td><td>${fmtMB(p.renderBuffers || 0)}</td></tr>`
  ).join('');
}

function renderFetchTable(fetchLog) {
  const body = $id('fetch-body');
  if (!fetchLog || fetchLog.length === 0) {
    body.innerHTML = '<tr><td class="muted" colspan="4">No fetch data.</td></tr>';
    return;
  }
  body.innerHTML = fetchLog.map((f, i) =>
    `<tr>
      <td>${i + 1}</td>
      <td style="max-width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${escapeHTML(f.path || f.url || '')}</td>
      <td>${f.bytes ? fmtMB(f.bytes) : '—'}</td>
      <td class="${f.ok ? 'ok' : 'err'}">${f.ok ? 'OK' : 'ERR'}</td>
    </tr>`
  ).join('');
}

function updateSummary(mem, loaded) {
  const peak = mem?.peakReserved || 0;
  const input = mem?.input || 0;
  const ratio = input > 0 ? (peak / input).toFixed(2) : '—';
  $id('s-peak').textContent = fmtMB(peak);
  $id('s-input').textContent = fmtMB(input);
  $id('s-ratio').textContent = ratio + '×';
  $id('s-fetches').textContent = loaded?.fetches ?? '—';
  $id('s-fetched').textContent = loaded?.fetchedBytes ? fmtMB(loaded.fetchedBytes) : '—';
  $id('s-loadtime').textContent = loaded?.loadMs ? fmtTime(loaded.loadMs) : '—';
}

// ── Loading ──

async function loadSample(index) {
  const sample = SAMPLES[index];
  if (!sample) return;

  const status = $id('viz-status');
  const overlay = $id('viewer-overlay');
  const placeholder = $id('viewer-placeholder');

  overlay.style.display = 'flex';
  placeholder.style.display = 'flex';
  placeholder.querySelector('p').textContent = `Loading ${sample.label}...`;
  status.textContent = 'Fetching...';

  try {
    const baseUrl = sample.url.slice(0, sample.url.lastIndexOf('/') + 1);
    const resp = await fetch(sample.url, { cache: 'no-store' });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const rootBytes = new Uint8Array(await resp.arrayBuffer());
    const filename = sample.url.split('/').pop() || 'scene.usd';

    const renderer = new StreamingUSDRenderer($id('viz-canvas'));
    showLoader("Loading LightUSD WASM...", document.getElementById("viewport"));
    try {
      await renderer.init()
    } finally {
      hideLoader();
    };
    renderer.setClearColor([0.12, 0.12, 0.14, 1.0]);
    // Reset on subsequent loads
    window.__vizRenderer = renderer;

    status.textContent = 'Loading and composing...';
    const loaded = await renderHttpUSD({
      renderer,
      rootBytes,
      filename,
      baseUrl,
      label: sample.label,
      backend: 'next',
      onStatus: (msg) => { status.textContent = msg; },
    });

    overlay.style.display = 'none';
    status.textContent = `${sample.label}: ${loaded.result.meshes} meshes, ${loaded.result.textures} textures (${loaded.backend})`;

    // Draw visualizations
    const mem = loaded.result.memory;
    drawHeapChart($id('heap-chart'), mem?.phases, mem?.summary?.peakHeapMB || 0);
    renderPhasesTable(mem?.phases);
    drawFetchChart($id('fetch-chart'), loaded.fetchLog);
    renderFetchTable(loaded.fetchLog);
    updateSummary(mem, loaded);

    $id('fit-btn').onclick = () => renderer.frameCamera();
  } catch (e) {
    console.error(e);
    status.textContent = `Error: ${e.message}`;
    placeholder.querySelector('p').textContent = `Failed: ${e.message}`;
  }
}

// ── Init ──

$id('load-btn').addEventListener('click', () => {
  loadSample(Number($id('sample-select').value));
});

// Auto-load
const params = new URLSearchParams(location.search);
const idx = params.has('sample') ? Math.min(Number(params.get('sample')), SAMPLES.length - 1) : 0;
loadSample(idx).catch((e) => { console.error(e); $id('viz-status').textContent = 'Error: ' + e.message; });
import { showLoader, hideLoader } from "../lightusd-loader.js";
