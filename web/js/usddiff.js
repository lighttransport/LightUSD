// usddiff — LightUSD WASM diff visualizer
//
// Pick two USD files (.usd/.usda/.usdc/.usdz) and view a structured diff:
//   - value-level reasons (value / type / variability / connections / meta:*, ...)
//   - ULP-tolerant float compare (configurable: ULPs, eps, compare-metadata)
//   - stage (layer) metadata diffs, and a navigable per-prim/-property report
//
// Files are diffed pre-composition (as Layers), mirroring `lusddiff`. Pure WASM
// by default; an optional panel can drive the diff through a running MCP server
// (mcp_server --port ...) for big-diff navigation (diff_tree / diff_paths).

import { loadWasm } from './src/usdzconvert.js';
import { McpFetchClient, toBase64 } from './src/mcp-fetch-client.js';
import { basenameFromUri, mountBackendSelector } from './src/lightusd/LoaderConfigUtils.js';

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

const container = document.createElement('div');
container.style.cssText = 'max-width:1000px;margin:28px auto;padding:20px;font-family:system-ui,sans-serif;color:#eee';
container.innerHTML = `
  <h1 style="margin:0 0 6px">LightUSD — usddiff</h1>
  <p style="color:#aaa;margin:0 0 16px">
    Compare two USD files (<b>.usd / .usda / .usdc / .usdz</b>) at the Layer / Prim /
    Attribute level, with value-level reasons and ULP-tolerant float comparison.
  </p>

  <div style="display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:12px">
    <div id="dropLeft" class="drop">
      <div style="margin-bottom:8px;color:#bbb">Left (file 1)</div>
      <label class="btn">Choose file<input id="leftInput" type="file"
        accept=".usd,.usda,.usdc,.usdz" style="display:none"></label>
      <div id="leftName" style="color:#999;margin-top:8px;font-size:13px">(none)</div>
    </div>
    <div id="dropRight" class="drop">
      <div style="margin-bottom:8px;color:#bbb">Right (file 2)</div>
      <label class="btn">Choose file<input id="rightInput" type="file"
        accept=".usd,.usda,.usdc,.usdz" style="display:none"></label>
      <div id="rightName" style="color:#999;margin-top:8px;font-size:13px">(none)</div>
    </div>
  </div>

  <div class="bar">
    <span class="lbl">ULPs</span>
    <input id="optUlps" type="number" min="0" step="1" value="1" title="0 = bitwise-exact" style="width:64px">
    <span class="lbl">eps</span>
    <input id="optEps" type="text" placeholder="off" title="absolute float epsilon (optional)" style="width:80px">
    <label class="chk"><input id="optMeta" type="checkbox" checked> compare metadata</label>
    <button id="btnDiff" class="btn primary" disabled>Compare</button>
    <span id="status" style="color:#aaa;margin-left:6px"></span>
  </div>

  <div id="loadStats" style="display:none;color:#889;font-family:ui-monospace,monospace;font-size:12px;margin:-2px 0 10px"></div>

  <div class="bar">
    <span class="lbl">View</span>
    <span id="tabs">
      <button class="tab active" data-view="report">Report</button><button class="tab" data-view="text">Text</button><button class="tab" data-view="json">JSON</button>
    </span>
    <span id="filterWrap" style="margin-left:auto;display:flex;gap:8px;align-items:center">
      <input id="filterPath" type="text" placeholder="filter path…" style="width:160px">
      <input id="filterReason" type="text" placeholder="filter reason…" style="width:140px">
    </span>
  </div>

  <details id="mcpPanel" class="bar" style="display:block">
    <summary style="cursor:pointer;color:#bbb">MCP server (optional) — drive the diff through a running mcp_server</summary>
    <div style="display:flex;gap:8px;align-items:center;margin-top:8px;flex-wrap:wrap">
      <input id="mcpUrl" type="text" value="http://localhost:8085/mcp" style="width:280px">
      <button id="btnConnect" class="btn">Connect</button>
      <label class="chk"><input id="useMcp" type="checkbox" disabled> use MCP for diff</label>
      <span id="mcpStatus" style="color:#aaa">not connected</span>
    </div>
  </details>

  <div id="report" style="margin-top:12px"></div>
  <pre id="raw" style="display:none;background:#111;padding:12px;border-radius:6px;
       max-height:62vh;overflow:auto;font-size:12.5px;white-space:pre-wrap;line-height:1.5;margin-top:12px"></pre>
`;
document.body.style.background = '#0d0d1a';
document.body.appendChild(container);

// Backend switch reloads the page (the WASM module is chosen at startup);
// mounted into the options bar next to ULPs/eps.
mountBackendSelector(container.querySelector('.bar'), { append: true });

const styleEl = document.createElement('style');
styleEl.textContent = `
  .btn{display:inline-flex;align-items:center;gap:4px;cursor:pointer;background:#2d2d5e;
    color:#eee;border:1px solid #555;border-radius:5px;padding:7px 14px;font-size:14px;margin:2px}
  .btn.primary{background:#3a5edb;border-color:#4a6ef0}
  .btn:disabled{opacity:.5;cursor:not-allowed}
  .drop{border:2px dashed #555;border-radius:8px;padding:16px;text-align:center;background:#1a1a2e}
  .drop.active{border-color:#4a6ef0;background:#22223e}
  .bar{display:flex;gap:10px;align-items:center;margin-bottom:10px;font-size:14px;flex-wrap:wrap}
  .lbl{color:#bbb}
  .chk{display:inline-flex;gap:5px;align-items:center;color:#ccc}
  input[type=text],input[type=number]{background:#15152a;color:#eee;border:1px solid #555;border-radius:4px;padding:5px 7px;font-size:13px}
  .tab{cursor:pointer;background:#1a1a2e;color:#ccc;border:1px solid #555;padding:6px 12px;font-size:13px;margin:0}
  .tab:first-child{border-radius:5px 0 0 5px}
  .tab:last-child{border-radius:0 5px 5px 0}
  .tab.active{background:#3a5edb;border-color:#4a6ef0;color:#fff}
  .add{color:#7ee787} .del{color:#ff7b72} .mod{color:#d2a8ff}
  #raw .add{color:#7ee787} #raw .del{color:#ff7b72} #raw .mod{color:#d2a8ff}
  .card{background:#15152a;border:1px solid #2a2a44;border-radius:6px;padding:10px 12px;margin-bottom:8px}
  .chip{display:inline-block;background:#2a2a44;color:#cbd;border:1px solid #444;border-radius:10px;
    padding:1px 9px;margin:2px 4px 2px 0;font-size:12px;cursor:pointer}
  .chip b{color:#fff}
  .ppath{font-family:ui-monospace,monospace;font-size:12.5px;word-break:break-all}
  .vrow{font-family:ui-monospace,monospace;font-size:12px;white-space:pre-wrap;word-break:break-all;margin:2px 0}
  .counts b{color:#fff}
  .muted{color:#888}
  details.tree>summary{cursor:pointer}
`;
document.head.appendChild(styleEl);

const $ = id => document.getElementById(id);
const els = {
  dropLeft: $('dropLeft'), dropRight: $('dropRight'),
  leftInput: $('leftInput'), rightInput: $('rightInput'),
  leftName: $('leftName'), rightName: $('rightName'),
  btnDiff: $('btnDiff'), status: $('status'),
  optUlps: $('optUlps'), optEps: $('optEps'), optMeta: $('optMeta'),
  tabs: $('tabs'), filterPath: $('filterPath'), filterReason: $('filterReason'),
  report: $('report'), raw: $('raw'), loadStats: $('loadStats'),
  mcpUrl: $('mcpUrl'), btnConnect: $('btnConnect'), useMcp: $('useMcp'), mcpStatus: $('mcpStatus'),
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let native = null;
let mcp = null;            // McpFetchClient when connected
const files = { left: null, right: null };
let view = 'report';
let lastJson = null;       // parsed WASM JSON (pure-WASM mode)
let lastText = '';         // WASM text
let lastMode = 'wasm';     // 'wasm' | 'mcp'

function setStatus(s) { els.status.textContent = s; }

async function ensureWasm() {
  if (native) return native;
  setStatus('Loading WASM…');
  // backend=next / wasm=next load the next-only module (its usddiff diffs
  // next::Layer with the same options/result contract).
  const params = new URLSearchParams(window.location.search);
  const useNext = params.get('backend') === 'next' || params.get('wasm') === 'next';
  const glue = useNext ? './src/lightusd/lightusd.js' : './src/lightusd/lightusd.js';
  native = await loadWasm(() => import(/* @vite-ignore */ new URL(glue, import.meta.url).href));
  setStatus('');
  return native;
}

function refresh() {
  els.leftName.textContent = files.left ? files.left.name : '(none)';
  els.rightName.textContent = files.right ? files.right.name : '(none)';
  els.btnDiff.disabled = !(files.left && files.right);
}
const USD_EXT_RE = /\.(usd|usda|usdc|usdz)$/i;
async function setFile(slot, file) {
  if (!file) { files[slot] = null; refresh(); return; }
  if (!USD_EXT_RE.test(file.name)) {
    setStatus(`Unsupported file "${file.name}" — pick a .usd / .usda / .usdc / .usdz file.`);
    return;
  }
  const t0 = performance.now();
  files[slot] = { name: file.name, data: new Uint8Array(await file.arrayBuffer()), readMs: performance.now() - t0 };
  refresh();
}
// Reset the input value after each pick so re-selecting the same file works.
els.leftInput.addEventListener('change', e => { setFile('left', e.target.files[0]); e.target.value = ''; });
els.rightInput.addEventListener('change', e => { setFile('right', e.target.files[0]); e.target.value = ''; });
function wireDrop(el, slot) {
  ['dragenter', 'dragover'].forEach(ev => el.addEventListener(ev, e => { e.preventDefault(); el.classList.add('active'); }));
  ['dragleave', 'drop'].forEach(ev => el.addEventListener(ev, e => { e.preventDefault(); el.classList.remove('active'); }));
  el.addEventListener('drop', e => { const f = e.dataTransfer.files && e.dataTransfer.files[0]; if (f) setFile(slot, f); });
}
wireDrop(els.dropLeft, 'left');
wireDrop(els.dropRight, 'right');

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function escapeHtml(s) {
  return String(s).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
}

// Center two long values on the first differing byte (JS port of
// tydra::CenterValuePairForDiff) so a shared prefix doesn't hide the change.
function centerPair(lhs, rhs, window = 160) {
  lhs = lhs || ''; rhs = rhs || '';
  if (lhs.length <= window && rhs.length <= window) return [lhs, rhs];
  let d = 0; const n = Math.min(lhs.length, rhs.length);
  while (d < n && lhs[d] === rhs[d]) d++;
  const lead = Math.floor(window / 3);
  const start = d > lead ? d - lead : 0;
  const clip = s => (start > 0 ? '…' : '') + s.slice(start, start + window) + (start + window < s.length ? '…' : '');
  return [clip(lhs), clip(rhs)];
}

function reasonOptions() {
  const o = { compareMetadata: !!els.optMeta.checked };
  const u = parseInt(els.optUlps.value, 10);
  if (!isNaN(u) && u >= 0) o.ulps = u;
  const e = els.optEps.value.trim();
  if (e !== '' && !isNaN(Number(e))) o.eps = Number(e);
  return o;
}

function reasonChips(tally, onClick) {
  const keys = Object.keys(tally).sort((a, b) => tally[b] - tally[a] || (a < b ? -1 : 1));
  return keys.map(k =>
    `<span class="chip" data-reason="${escapeHtml(k)}">${escapeHtml(k)} <b>${tally[k]}</b></span>`).join('');
}

function setView(v) {
  view = v;
  [...els.tabs.querySelectorAll('.tab')].forEach(t => t.classList.toggle('active', t.dataset.view === v));
  els.filterPath.parentElement.style.visibility = (v === 'report') ? 'visible' : 'hidden';
  render();
}
els.tabs.addEventListener('click', e => { const t = e.target.closest('.tab'); if (t) setView(t.dataset.view); });
els.filterPath.addEventListener('input', () => { if (view === 'report') render(); });
els.filterReason.addEventListener('input', () => { if (view === 'report') render(); });

// ---------------------------------------------------------------------------
// Render dispatch
// ---------------------------------------------------------------------------

function render() {
  const showReport = (view === 'report');
  els.report.style.display = showReport ? 'block' : 'none';
  els.raw.style.display = showReport ? 'none' : 'block';
  if (lastMode === 'mcp') { renderMcpView(); return; }
  if (view === 'text') { renderTextRaw(lastText || 'No differences found.\n'); return; }
  if (view === 'json') { els.raw.textContent = lastJson ? JSON.stringify(lastJson, null, 2) : '(no diff)'; return; }
  renderReportFromJson(lastJson);
}

function renderTextRaw(text) {
  els.raw.innerHTML = String(text).split('\n').map(line => {
    const esc = escapeHtml(line);
    if (/^\+/.test(line)) return `<span class="add">${esc}</span>`;
    if (/^- /.test(line) || /^-\//.test(line)) return `<span class="del">${esc}</span>`;
    if (/^~/.test(line)) return `<span class="mod">${esc}</span>`;
    return esc;
  }).join('\n');
}

// ---- WASM report (full JSON) ----

function computeSummary(json) {
  const s = { prims: { added: 0, deleted: 0, modified: 0 }, props: { added: 0, deleted: 0, modified: 0 }, reasons: {} };
  const acc = (det) => (det || []).forEach(m => (m.reasons || []).forEach(r => { s.reasons[r] = (s.reasons[r] || 0) + 1; }));
  const ps = json.primspec_diffs || {};
  for (const k in ps) { const d = ps[k]; s.prims.added += (d.added || []).length; s.prims.deleted += (d.deleted || []).length; s.prims.modified += (d.modified || []).length; acc(d.modified_details); }
  const pp = json.property_diffs || {};
  for (const k in pp) { const d = pp[k]; s.props.added += (d.added || []).length; s.props.deleted += (d.deleted || []).length; s.props.modified += (d.modified || []).length; acc(d.modified_details); }
  return s;
}

function joinPath(parent, child) { return (!parent || parent === '/') ? '/' + child : parent + '/' + child; }

function renderReportFromJson(json) {
  if (!json) { els.report.innerHTML = '<div class="muted">No diff yet.</div>'; return; }
  const fp = els.filterPath.value.trim();
  const fr = els.filterReason.value.trim();
  const matchReason = rs => !fr || (rs || []).some(r => r.includes(fr));
  const s = computeSummary(json);
  const cmp = json.comparison || {};
  let html = '';

  // Summary
  html += `<div class="card"><div class="counts" style="margin-bottom:6px">`
    + `<span class="muted">${escapeHtml(cmp.left || 'left')} → ${escapeHtml(cmp.right || 'right')}</span> &nbsp; `
    + `prims <b class="add">+${s.prims.added}</b> <b class="del">-${s.prims.deleted}</b> <b class="mod">~${s.prims.modified}</b> &nbsp; `
    + `props <b class="add">+${s.props.added}</b> <b class="del">-${s.props.deleted}</b> <b class="mod">~${s.props.modified}</b></div>`
    + `<div>${reasonChips(s.reasons)}</div></div>`;

  // Stage metadata
  const lm = json.layer_meta_diff;
  if (lm && lm.details && lm.details.length) {
    html += `<div class="card"><div class="mod" style="margin-bottom:4px">Stage metadata</div>`;
    lm.details.forEach(d => {
      html += `<div class="vrow"><b>${escapeHtml(d.name)}</b>: <span class="del">${escapeHtml(d.left)}</span> → <span class="add">${escapeHtml(d.right)}</span></div>`;
    });
    html += `</div>`;
  }

  // Per-path: primspec changes (added/deleted/modified children) then property changes.
  const ps = json.primspec_diffs || {};
  const pp = json.property_diffs || {};
  const paths = Array.from(new Set([...Object.keys(ps), ...Object.keys(pp)])).sort();
  let shown = 0; const CAP = 800;
  let body = '';
  for (const path of paths) {
    if (fp && !path.includes(fp)) {
      // still allow child paths whose joined name contains fp — cheap: skip only if neither path nor its children match
    }
    const d = ps[path] || {}, p = pp[path] || {};
    const rows = [];
    (d.deleted || []).forEach(n => { const fpth = joinPath(path, n); if ((!fp || fpth.includes(fp)) && !fr) rows.push(`<div class="vrow del">- ${escapeHtml(fpth)} (prim)</div>`); });
    (d.added || []).forEach(n => { const fpth = joinPath(path, n); if ((!fp || fpth.includes(fp)) && !fr) rows.push(`<div class="vrow add">+ ${escapeHtml(fpth)} (prim)</div>`); });
    (d.modified_details || []).forEach(m => {
      const fpth = joinPath(path, m.name);
      if ((fp && !fpth.includes(fp)) || !matchReason(m.reasons)) return;
      rows.push(`<div class="vrow mod">~ ${escapeHtml(fpth)} (prim) ${(m.reasons || []).map(r => `<span class="chip">${escapeHtml(r)}</span>`).join('')}</div>`);
    });
    (p.deleted || []).forEach(n => { if ((!fp || (path + '.' + n).includes(fp)) && !fr) rows.push(`<div class="vrow del">- ${escapeHtml(path + '.' + n)}</div>`); });
    (p.added || []).forEach(n => { if ((!fp || (path + '.' + n).includes(fp)) && !fr) rows.push(`<div class="vrow add">+ ${escapeHtml(path + '.' + n)}</div>`); });
    (p.modified_details || []).forEach(m => {
      const full = path + '.' + m.name;
      if ((fp && !full.includes(fp)) || !matchReason(m.reasons)) return;
      const [l, r] = centerPair(m.left, m.right);
      rows.push(`<div class="vrow mod">~ ${escapeHtml(full)} ${(m.reasons || []).map(x => `<span class="chip">${escapeHtml(x)}</span>`).join('')}</div>`
        + `<div class="vrow"><span class="del">- ${escapeHtml(l)}</span></div>`
        + `<div class="vrow"><span class="add">+ ${escapeHtml(r)}</span></div>`);
    });
    if (rows.length) {
      if (shown >= CAP) { body += `<div class="muted">… filtered view truncated; refine the filter.</div>`; break; }
      shown++;
      body += `<div class="card"><div class="ppath muted" style="margin-bottom:4px">${escapeHtml(path)}</div>${rows.join('')}</div>`;
    }
  }
  html += body || `<div class="muted">No changes match the current filter.</div>`;
  els.report.innerHTML = html;
  wireChips();
}

function wireChips() {
  els.report.querySelectorAll('.chip[data-reason]').forEach(c => {
    c.addEventListener('click', () => { els.filterReason.value = c.dataset.reason; if (view === 'report') render(); });
  });
}

// ---------------------------------------------------------------------------
// MCP mode
// ---------------------------------------------------------------------------

let mcpSummary = null;

els.btnConnect.addEventListener('click', async () => {
  try {
    els.btnConnect.disabled = true;
    els.mcpStatus.textContent = 'connecting…';
    mcp = new McpFetchClient(els.mcpUrl.value.trim());
    const info = await mcp.connect();
    const tools = await mcp.listTools();
    const hasDiff = tools.some(t => t.name === 'diff_open');
    els.mcpStatus.textContent = `connected: ${(info.serverInfo && info.serverInfo.name) || 'server'}` + (hasDiff ? '' : ' (no diff tools!)');
    els.useMcp.disabled = !hasDiff;
    els.useMcp.checked = hasDiff;
  } catch (err) {
    mcp = null;
    els.useMcp.disabled = true; els.useMcp.checked = false;
    els.mcpStatus.textContent = 'connect failed: ' + (err && err.message ? err.message : err);
  } finally {
    els.btnConnect.disabled = false;
  }
});

async function runMcpDiff(opts) {
  const args = {
    left: { data: toBase64(files.left.data), name: files.left.name },
    right: { data: toBase64(files.right.data), name: files.right.name },
    compareMetadata: opts.compareMetadata,
  };
  if (opts.ulps !== undefined) args.ulps = opts.ulps;
  if (opts.eps !== undefined) args.eps = opts.eps;
  mcpSummary = await mcp.callTool('diff_open', args);
  lastMode = 'mcp';
}

async function renderMcpView() {
  if (view === 'json') { els.raw.textContent = JSON.stringify(mcpSummary, null, 2); return; }
  if (view === 'text') {
    const t = await mcp.callTool('diff_text', {});
    renderTextRaw((t && t.text) || 'No differences found.\n');
    return;
  }
  // Report: summary chips + subtree tree + filtered paths (lazy via MCP tools).
  const s = mcpSummary || {};
  const tally = {}; (s.reasons || []).forEach(r => { tally[r.reason] = r.count; });
  let html = `<div class="card"><div class="counts" style="margin-bottom:6px">`
    + `<span class="muted">${escapeHtml(s.left || 'left')} → ${escapeHtml(s.right || 'right')} · via MCP</span> &nbsp; `
    + `prims <b class="add">+${(s.prims || {}).added || 0}</b> <b class="del">-${(s.prims || {}).deleted || 0}</b> <b class="mod">~${(s.prims || {}).modified || 0}</b> &nbsp; `
    + `props <b class="add">+${(s.properties || {}).added || 0}</b> <b class="del">-${(s.properties || {}).deleted || 0}</b> <b class="mod">~${(s.properties || {}).modified || 0}</b></div>`
    + `<div>${reasonChips(tally)}</div></div>`;
  html += `<div class="card"><div class="mod" id="treeHdr" style="margin-bottom:4px">Subtree overview (diff_tree)</div><div id="treeBody" class="muted">loading…</div></div>`;
  html += `<div class="card"><div class="mod" style="margin-bottom:4px">Paths (diff_paths)</div><div id="pathsBody" class="muted">use the filters above…</div></div>`;
  els.report.innerHTML = html;
  wireChips();

  // Tree
  try {
    const tree = await mcp.callTool('diff_tree', { path: '/', depth: 2 });
    $('treeBody').innerHTML = (tree.nodes || []).map(n =>
      `<div class="vrow"><b>${n.changes}</b> <span class="ppath" style="cursor:pointer" data-path="${escapeHtml(n.path)}">${escapeHtml(n.path)}</span></div>`).join('') || '<span class="muted">no changes</span>';
    $('treeBody').querySelectorAll('[data-path]').forEach(e =>
      e.addEventListener('click', () => { els.filterPath.value = e.dataset.path; refreshMcpPaths(); }));
  } catch (e) { $('treeBody').textContent = 'tree error: ' + e.message; }

  await refreshMcpPaths();
}

async function refreshMcpPaths() {
  const body = $('pathsBody'); if (!body) return;
  try {
    const r = await mcp.callTool('diff_paths', {
      reason: els.filterReason.value.trim() || undefined,
      path_substr: els.filterPath.value.trim() || undefined,
      limit: 200,
    });
    const rows = (r.paths || []).map(p =>
      `<div class="vrow ${p.kind.includes('added') ? 'add' : p.kind.includes('deleted') ? 'del' : 'mod'}">`
      + `${escapeHtml(p.kind)} <span class="ppath">${escapeHtml(p.path)}</span> `
      + `${(p.reasons || []).map(x => `<span class="chip">${escapeHtml(x)}</span>`).join('')}</div>`).join('');
    body.innerHTML = `<div class="muted" style="margin-bottom:4px">${r.total} match(es)${r.total > 200 ? ', showing 200' : ''}</div>` + (rows || '<span class="muted">none</span>');
  } catch (e) { body.textContent = 'paths error: ' + e.message; }
}
// Re-query MCP paths when filters change (debounced-ish).
let mcpFilterTimer = null;
function scheduleMcpPaths() { if (lastMode !== 'mcp' || view !== 'report') return; clearTimeout(mcpFilterTimer); mcpFilterTimer = setTimeout(refreshMcpPaths, 250); }
els.filterPath.addEventListener('input', scheduleMcpPaths);
els.filterReason.addEventListener('input', scheduleMcpPaths);

// ---------------------------------------------------------------------------
// Compare
// ---------------------------------------------------------------------------

els.btnDiff.addEventListener('click', async () => {
  try {
    els.btnDiff.disabled = true;
    const opts = reasonOptions();
    setStatus('Comparing…');

    if (mcp && els.useMcp.checked) {
      await runMcpDiff(opts);
      setStatus(mcpSummary && mcpSummary.hasDiffs === false ? 'No differences.' : 'Differences found (MCP).');
      render();
      return;
    }

    await ensureWasm();
    const res = native.usddiff({
      left: { data: files.left.data, name: files.left.name },
      right: { data: files.right.data, name: files.right.name },
      format: 'both',
      ...opts,
    });
    if (!res || !res.success) {
      els.report.innerHTML = ''; els.raw.style.display = 'block'; els.report.style.display = 'none';
      els.raw.textContent = 'Error: ' + (res && res.error ? res.error : 'usddiff failed');
      setStatus('Failed.');
      return;
    }
    if (res.warn) console.warn('usddiff warning:', res.warn);
    lastMode = 'wasm';
    lastText = res.text || '';
    try { lastJson = res.json ? JSON.parse(res.json) : null; } catch (_) { lastJson = null; }
    render();
    setStatus(res.hasDiffs ? 'Differences found.' : 'No differences found.');
  } catch (err) {
    els.raw.style.display = 'block'; els.report.style.display = 'none';
    els.raw.textContent = 'Error: ' + (err && err.message ? err.message : err);
    setStatus('Failed.');
  } finally {
    els.btnDiff.disabled = !(files.left && files.right);
  }
});

setStatus('Pick two USD files to compare.');
render();

// URL-driven compare: ?left=<url>&right=<url> fetches both files and runs
// the diff automatically (used by the browser regression suite).
(async () => {
  const params = new URLSearchParams(window.location.search);
  const leftUrl = params.get('left');
  const rightUrl = params.get('right');
  if (!leftUrl || !rightUrl) return;
  const fetchSide = async (slot, url) => {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`fetch ${url}: HTTP ${res.status}`);
    const name = decodeURIComponent(url.split('?')[0].split('#')[0].split('/').pop() || slot);
    files[slot] = { name, data: new Uint8Array(await res.arrayBuffer()), readMs: 0 };
  };
  try {
    setStatus('Fetching files from URL…');
    await Promise.all([fetchSide('left', leftUrl), fetchSide('right', rightUrl)]);
    refresh();
    els.btnDiff.click();
  } catch (err) {
    setStatus('Failed to load URL inputs: ' + (err && err.message ? err.message : err));
  }
})();
