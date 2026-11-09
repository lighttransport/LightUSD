import { StreamingUSDRenderer } from 'lightusd-js/streaming.js';
import { renderHttpUSD } from 'lightusd-js/http-asset-resolver.js';

const SAMPLES = [
  {
    label: 'SubLayer + Over',
    desc: 'Two sublayers with overrides that reposition two Suzanne models side by side.',
    url: './assets/usd-composite-sample.usda',
  },
  {
    label: 'References (Suzanne PBR)',
    desc: 'A root Xform references suzanne-pbr.usda with a translate offset.',
    url: './assets/references-001.usda',
  },
  {
    label: 'References (UsdCookie)',
    desc: 'Root prim references UsdCookie.usdz with no override.',
    url: './assets/references-002.usda',
  },
  {
    label: 'References (Texture Cat)',
    desc: 'Root prim references a textured cat plane.',
    url: './assets/references-003.usda',
  },
  {
    label: 'Sphere (no arcs)',
    desc: 'A simple stand-alone USD file with no composition arcs — for comparison.',
    url: 'https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/schemaTests/usdGeom/primitives/sphere.usda',
  },
];

function escapeHTML(v) { return String(v).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'); }
function $id(id) { return document.getElementById(id); }

// ── Parse composition arcs from USDA source text ──

function parseCompositionArcs(text) {
  const result = {
    subLayers: [],
    references: [],
    payloads: [],
    inherits: [],
    specializes: [],
    variantSelections: [],
    overrides: [],
  };

  // subLayers = [ @path@, @path@ ]
  const subLayerMatch = text.match(/subLayers\s*=\s*\[([^\]]*)\]/);
  if (subLayerMatch) {
    const paths = subLayerMatch[1].match(/@([^@]+)@/g) || [];
    for (const p of paths) {
      const clean = p.replace(/^@|@$/g, '').split('@')[0];
      result.subLayers.push({ target: clean });
    }
  }

  // references / payloads / inherits / specializes in prim metadata: prepend references = [ @path@ ]
  const arcPatterns = [
    { key: 'references', re: /references\s*=\s*\[([^\]]*)\]/g },
    { key: 'payloads', re: /payloads\s*=\s*\[([^\]]*)\]/g },
    { key: 'inherits', re: /inherits\s*=\s*\[([^\]]*)\]/g },
    { key: 'specializes', re: /specializes\s*=\s*\[([^\]]*)\]/g },
    // Also match single-reference form: references = @path@
    { key: 'references', re: /references\s*=\s*@([^@]+)@/g },
    { key: 'payloads', re: /payloads\s*=\s*@([^@]+)@/g },
    { key: 'inherits', re: /inherits\s*=\s*@([^@]+)@/g },
    { key: 'specializes', re: /specializes\s*=\s*@([^@]+)@/g },
  ];

  for (const { key, re } of arcPatterns) {
    let m;
    while ((m = re.exec(text)) !== null) {
      const val = m[1].trim();
      if (val.startsWith('@')) {
        const target = val.replace(/^@|@$/g, '').split('@')[0];
        const primPath = extractPrimContext(text, m.index);
        if (!result[key].some((r) => r.target === target && r.prim === primPath)) {
          result[key].push({ target, prim: primPath });
        }
      }
    }
  }

  // overrides: find `over ` blocks
  const overRe = /^over\s+(\S+)/gm;
  let om;
  while ((om = overRe.exec(text)) !== null) {
    result.overrides.push({ prim: om[1] });
  }

  // Variant selections: token variant = "set=selection"
  const varRe = /variantSet\s*=\s*"([^"]+)";\s*variant\s*=\s*"([^"]+)"/g;
  let vm;
  while ((vm = varRe.exec(text)) !== null) {
    result.variantSelections.push({ set: vm[1], variant: vm[2] });
  }

  return result;
}

function extractPrimContext(text, index) {
  // Find the nearest `def ` or `over ` before the arc
  const before = text.slice(0, index);
  const lines = before.split('\n');
  for (let i = lines.length - 1; i >= 0; i--) {
    const m = lines[i].match(/^\s*(?:def|over)\s+(\S+)/);
    if (m) return m[1];
  }
  return '(root)';
}

// ── Shell ──

const root = document.getElementById('demo-root');
root.innerHTML = `
<div class="demo-shell">
  <header class="demo-toolbar">
    <div>
      <a class="demo-back" href="./">Demos</a>
      <h1>Composition Layer Viz</h1>
      <p>Visualizes how USD composition arcs (subLayers, references, payloads,
        inherits, specializes) stack to produce the final scene.</p>
    </div>
    <div class="demo-actions">
      <select id="sample-select">
        ${SAMPLES.map((s, i) => `<option value="${i}"${i===0?' selected':''}>${escapeHTML(s.label)}</option>`).join('')}
      </select>
      <button id="load-btn" type="button">Load</button>
      <button id="fit-btn" type="button">Fit</button>
    </div>
  </header>
  <main class="assets-main" style="grid-template-columns:minmax(320px,420px) minmax(0,1fr)">
    <aside class="assets-catalog" style="border-right:1px solid var(--line);display:flex;flex-direction:column;overflow:hidden">
      <div style="flex:0 0 auto;padding:12px 14px;border-bottom:1px solid var(--line)">
        <div style="display:flex;gap:8px;margin-bottom:8px;flex-wrap:wrap">
          <button class="cat-btn active" data-layer="all">All</button>
          <button class="cat-btn" data-layer="sublayers">SubLayers</button>
          <button class="cat-btn" data-layer="references">References</button>
          <button class="cat-btn" data-layer="overrides">Overrides</button>
        </div>
        <p id="comp-desc" style="margin:0;color:var(--dim);font-size:.78rem;line-height:1.35"></p>
      </div>
      <div id="layer-stack" style="flex:1;overflow-y:auto;padding:12px 14px">
        <div class="empty-state" style="min-height:100px">Load a scene.</div>
      </div>
      <div id="comp-usda" style="flex:0 0 auto;border-top:1px solid var(--line);max-height:30vh;overflow:auto;padding:8px 14px">
        <pre id="usda-source" style="margin:0;color:var(--muted);font-size:.7rem;line-height:1.35;white-space:pre-wrap;word-break:break-word"></pre>
      </div>
    </aside>
    <section class="assets-viewport-wrap">
      <div class="assets-canvas-wrap">
        <canvas id="comp-canvas"></canvas>
        <div class="assets-viewer-overlay" id="viewer-overlay">
          <div class="assets-viewer-placeholder" id="viewer-placeholder">
            <p>Select a sample and click Load.</p>
          </div>
        </div>
      </div>
      <div class="assets-status" id="comp-status">Ready.</div>
      <div class="assets-stats" id="comp-stats">
        <div class="assets-stats-inner">
          <div class="asset-stat"><span class="stat-label">Meshes</span><span class="stat-value" id="stat-meshes">—</span></div>
          <div class="asset-stat"><span class="stat-label">Materials</span><span class="stat-value" id="stat-materials">—</span></div>
          <div class="asset-stat"><span class="stat-label">Arcs found</span><span class="stat-value" id="stat-arcs">—</span></div>
          <div class="asset-stat"><span class="stat-label">Backend</span><span class="stat-value" id="stat-backend">—</span></div>
          <div class="asset-stat"><span class="stat-label">Load time</span><span class="stat-value" id="stat-loadtime">—</span></div>
        </div>
      </div>
    </section>
  </main>
</div>`;

// ── State ──

let renderer = null;
let currentArcs = null;

// ── Layer stack rendering ──

function renderLayerStack(arcs) {
  const container = $id('layer-stack');
  if (!arcs || (!arcs.subLayers.length && !arcs.references.length && !arcs.payloads.length && !arcs.overrides.length)) {
    container.innerHTML = '<div class="empty-state" style="min-height:80px;font-size:.82rem">No composition arcs found in this file.</div>';
    return;
  }

  let html = '<div style="display:flex;flex-direction:column;gap:8px">';

  // SubLayers (strongest at top)
  if (arcs.subLayers.length > 0) {
    for (const sl of arcs.subLayers) {
      html += `<div class="layer-card sublayer">
        <div class="layer-header">
          <span class="layer-badge">subLayer</span>
          <span class="layer-target">${escapeHTML(sl.target)}</span>
        </div>
        <div class="layer-desc">Appended to the root layer stack. Overrides in higher layers take precedence.</div>
      </div>`;
    }
  }

  // Overrides
  if (arcs.overrides.length > 0) {
    for (const ov of arcs.overrides) {
      html += `<div class="layer-card override">
        <div class="layer-header">
          <span class="layer-badge">over</span>
          <span class="layer-target">${escapeHTML(ov.prim)}</span>
        </div>
        <div class="layer-desc">Overrides properties on the existing prim without creating a new def.</div>
      </div>`;
    }
  }

  // References
  if (arcs.references.length > 0) {
    // Group by prim
    const grouped = {};
    for (const ref of arcs.references) {
      const key = ref.prim || '(root)';
      if (!grouped[key]) grouped[key] = [];
      grouped[key].push(ref.target);
    }
    for (const [prim, targets] of Object.entries(grouped)) {
      html += `<div class="layer-card reference">
        <div class="layer-header">
          <span class="layer-badge">reference</span>
          <span class="layer-target" style="color:var(--muted)">on ${escapeHTML(prim)}</span>
        </div>
        <div style="margin-top:4px">${targets.map((t) => `<div class="layer-target" style="padding-left:14px">→ ${escapeHTML(t)}</div>`).join('')}</div>
        <div class="layer-desc">Inserts prims from the referenced file under the target prim.</div>
      </div>`;
    }
  }

  // Payloads
  if (arcs.payloads.length > 0) {
    for (const pl of arcs.payloads) {
      html += `<div class="layer-card payload">
        <div class="layer-header">
          <span class="layer-badge">payload</span>
          <span class="layer-target">${escapeHTML(pl.target)}</span>
        </div>
        <div class="layer-desc">Deferred reference — only loaded when explicitly needed.</div>
      </div>`;
    }
  }

  html += '</div>';
  container.innerHTML = html;
}

// ── Style the layer cards once in CSS, inject via style tag ──

function injectLayerStyles() {
  const style = document.createElement('style');
  style.textContent = `
    .layer-card {
      padding: 10px 12px;
      border-radius: 6px;
      border: 1px solid var(--line);
      background: var(--panel);
      transition: background .12s;
    }
    .layer-card:hover { background: var(--panel-2); }
    .layer-card.sublayer { border-left: 3px solid #38bdf8; }
    .layer-card.reference { border-left: 3px solid #a78bfa; }
    .layer-card.payload { border-left: 3px solid #f59e0b; }
    .layer-card.override { border-left: 3px solid #34d399; }
    .layer-header {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .layer-badge {
      padding: 1px 7px;
      border-radius: 10px;
      font-size: .7rem;
      font-weight: 700;
      text-transform: uppercase;
      white-space: nowrap;
    }
    .sublayer .layer-badge { background: rgba(56,189,248,.15); color: #38bdf8; }
    .reference .layer-badge { background: rgba(167,139,250,.15); color: #a78bfa; }
    .payload .layer-badge { background: rgba(245,158,11,.15); color: #f59e0b; }
    .override .layer-badge { background: rgba(52,211,153,.15); color: #34d399; }
    .layer-target {
      color: var(--text);
      font-size: .82rem;
      font-weight: 600;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .layer-desc {
      margin-top: 4px;
      color: var(--dim);
      font-size: .74rem;
      line-height: 1.3;
    }
  `;
  document.head.appendChild(style);
}
injectLayerStyles();

// ── Stats ──

function updateStats(stats) {
  const set = (id, v) => { const el = $id(id); if (el) el.textContent = v ?? '—'; };
  set('stat-meshes', stats.meshes);
  set('stat-materials', stats.materials);
  set('stat-arcs', stats.arcs);
  set('stat-backend', stats.backend);
  set('stat-loadtime', stats.loadtime);
}

// ── Loading ──

async function loadSample(index) {
  const sample = SAMPLES[index];
  if (!sample) return;

  const status = $id('comp-status');
  const overlay = $id('viewer-overlay');
  const placeholder = $id('viewer-placeholder');
  const usdaPre = $id('usda-source');
  const desc = $id('comp-desc');

  overlay.style.display = 'flex';
  placeholder.style.display = 'flex';
  placeholder.querySelector('p').textContent = `Loading ${sample.label}...`;
  status.textContent = 'Fetching...';

  try {
    // Fetch USDA text for parsing
    const resp = await fetch(sample.url);
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const bytes = await resp.arrayBuffer();
    const text = new TextDecoder('utf-8', { fatal: false }).decode(bytes);
    usdaPre.textContent = text;
    desc.textContent = sample.desc;

    // Parse composition arcs
    currentArcs = parseCompositionArcs(text);
    renderLayerStack(currentArcs);

    const arcCount =
      currentArcs.subLayers.length +
      currentArcs.references.length +
      currentArcs.payloads.length +
      currentArcs.overrides.length;

    // Decide base URL for relative arc resolution
    const urlObj = new URL(sample.url, window.location.href);
    const baseUrl = sample.url.startsWith('http')
      ? sample.url.slice(0, sample.url.lastIndexOf('/') + 1)
      : urlObj.origin + urlObj.pathname.slice(0, urlObj.pathname.lastIndexOf('/') + 1);

    const rootBytes = new Uint8Array(bytes);
    const filename = sample.url.split('/').pop() || 'scene.usd';

    status.textContent = 'Composing scene...';
    const t0 = performance.now();
    const loaded = await renderHttpUSD({
      renderer,
      rootBytes,
      filename,
      baseUrl,
      label: sample.label,
      backend: 'next',
      onStatus: (msg) => { status.textContent = msg; },
    });
    const elapsed = performance.now() - t0;

    overlay.style.display = 'none';
    status.textContent = `${sample.label}: ${loaded.result.meshes} meshes, ${loaded.result.textures} textures — ${loaded.backend} backend`;

    updateStats({
      meshes: loaded.result.meshes,
      materials: loaded.result.materials,
      arcs: arcCount,
      backend: loaded.backend,
      loadtime: elapsed.toFixed(0) + ' ms',
    });
  } catch (e) {
    console.error(e);
    status.textContent = `Error: ${e.message}`;
    placeholder.querySelector('p').textContent = `Failed: ${e.message}`;
  }
}

// ── Init ──

async function init() {
  renderer = new StreamingUSDRenderer($id('comp-canvas'));
  showLoader("Loading LightUSD WASM...", document.getElementById("viewport"));
    try {
      await renderer.init()
    } finally {
      hideLoader();
    };
  renderer.setClearColor([0.12, 0.12, 0.14, 1.0]);

  // Tab-like filter buttons for layer types
  document.querySelectorAll('[data-layer]').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('[data-layer]').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      if (currentArcs) renderLayerStack(currentArcs);
    });
  });

  $id('load-btn').addEventListener('click', () => {
    loadSample(Number($id('sample-select').value));
  });
  $id('fit-btn').addEventListener('click', () => {
    if (renderer) renderer.frameCamera();
  });

  // Auto-load
  const params = new URLSearchParams(location.search);
  const url = params.get('url') || params.get('uri');
  if (url) {
    // Add to samples temporarily
    SAMPLES.push({ label: url.split('/').pop(), desc: 'Custom URL', url });
    await loadSample(SAMPLES.length - 1);
  } else {
    await loadSample(0);
  }
}

init().catch((e) => { console.error(e); $id('comp-status').textContent = 'Error: ' + e.message; });
import { showLoader, hideLoader } from "../lightusd-loader.js";
