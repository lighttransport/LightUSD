// usdzconvert — LightUSD WASM
// Upload a folder or multiple files (USD + textures), convert to a USDZ with
// options (resize / re-encode textures), and download the result.
// Also includes a standalone texture channel-repack tool.
//
// NOTE: fpnge uses x86 SIMD and is not compiled for WASM, so PNG re-encoding
// here uses the portable `fpng` encoder. The native `lusdzconvert` CLI uses
// fpnge.

import {
  loadWasm,
  isImageName,
  parseByteSize,
  wasmHeapByteLength,
} from './src/usdzconvert.js';

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

const container = document.createElement('div');
container.style.cssText = 'max-width:820px;margin:32px auto;padding:20px;font-family:system-ui,sans-serif;color:#eee';
container.innerHTML = `
  <h1 style="margin:0 0 6px">LightUSD — usdzconvert</h1>
  <p style="color:#aaa;margin:0 0 18px">
    Drop or pick a <b>folder</b> (USD + textures), multiple files, or a single <b>.usdz</b>
    (its textures are unpacked and repacked — passthrough by default), set options,
    <b>Convert</b> to validate, then <b>Download</b>.
  </p>

  <div id="drop" style="border:2px dashed #555;border-radius:8px;padding:22px;text-align:center;margin-bottom:14px;background:#1a1a2e">
    <div style="margin-bottom:10px;color:#bbb">Drag &amp; drop a folder or files here, or:</div>
    <label class="btn">Choose Folder<input id="folderInput" type="file" webkitdirectory multiple style="display:none"></label>
    <label class="btn">Choose Files<input id="filesInput" type="file" multiple
       accept=".usd,.usda,.usdc,.usdz,.png,.jpg,.jpeg,.exr" style="display:none"></label>
    <button id="btnClear" class="btn" disabled title="Remove all uploaded files and start over">Clear</button>
  </div>

  <div id="fileList" style="display:none;background:#111;border-radius:6px;padding:10px;margin-bottom:14px;font-size:13px;max-height:160px;overflow:auto"></div>

  <fieldset style="border:1px solid #444;border-radius:6px;padding:12px;margin-bottom:14px">
    <legend style="color:#bbb">Options</legend>
    <div style="display:grid;grid-template-columns:auto 1fr;gap:8px 12px;align-items:center;font-size:14px">
      <label>Root USD file</label>
      <select id="rootSelect" style="padding:4px"></select>

      <label>Max texture size (px)</label>
      <input id="maxSize" type="number" min="0" step="64" value="0" style="padding:4px;width:120px" title="0 = do not resize">

      <label>Resize colorspace</label>
      <select id="resizeColorspace" style="padding:4px;width:160px"
        title="How to resample when resizing. Auto = per-texture from UsdUVTexture sourceColorSpace (sRGB color resampled in linear light, data maps in gamma space). Applies to WASM-processed textures (incl. EXR).">
        <option value="linear">Linear (default, fast canvas path)</option>
        <option value="auto">Auto — per-texture sRGB-aware (WASM)</option>
        <option value="srgb">sRGB — force linear-light (WASM)</option>
      </select>

      <label>UDIM baking</label>
      <select id="udimBake"><option value="off">Off</option><option value="grid">Grid stitch</option><option value="dense">Dense packing</option></select>
      <label>UDIM tile limit</label><input id="udimMaxTiles" type="number" min="1" max="8999" value="100">
      <label>Atlas edge limit</label><input id="udimMaxAtlasSize" type="number" min="1" max="32768" value="8192">
      <label>Bake memory (MiB)</label><input id="udimMemoryMiB" type="number" min="1" max="2047" value="512">
      <label>Crossing faces</label><select id="udimCrossTile"><option value="reject">Reject</option><option value="split">Split</option></select>
      <label>Dense gutter (px)</label><input id="udimDensePadding" type="number" min="0" max="1024" value="2">
      <label>Subdivision level</label><input id="udimSubdivisionLevel" type="number" min="1" max="8" value="2">
      <label>Texture format</label>
      <select id="textureFormat" style="padding:4px;width:120px"
        title="Keep = preserve source format (incl. EXR). EXR keeps HDR; PNG/JPEG tone-map EXR to LDR.">
        <option value="keep">Keep</option>
        <option value="png">PNG</option>
        <option value="jpeg">JPEG</option>
        <option value="exr">EXR (HDR)</option>
      </select>

      <label>USDZ root layer</label>
      <select id="rootLayerFormat" style="padding:4px;width:120px">
        <option value="usdc">USDC</option>
        <option value="usda">USDA</option>
      </select>

      <label>Flatten pipeline</label>
      <select id="pipeline" style="padding:4px;width:160px"
        title="Legacy is the stable in-memory path. Stream keeps textures lazy. Next is the experimental low-memory flattener. Next-only rewrites one root layer with asset passthrough using the smaller WASM module.">
        <option value="legacy">Legacy</option>
        <option value="next">Next (low memory)</option>
        <option value="next-only">Next-only (single layer; no flatten)</option>
        <option value="stream">Stream</option>
        <option value="stream-next">Stream + Next</option>
      </select>

      <label>Variant overrides</label>
      <input id="variantSelections" type="text" placeholder="set=value, lod=high"
             style="padding:4px;width:200px"
             title="Variant set overrides for next flatten. Comma-separated set=value pairs.">

      <label>Flatten stage</label>
      <input id="flatten" type="checkbox" checked style="justify-self:start">

      <label>ARKit compatible</label>
      <input id="arkitCompatible" type="checkbox" style="justify-self:start">

      <label>Include unused textures</label>
      <input id="includeUnusedTextures" type="checkbox" style="justify-self:start"
             title="For stream-next/next-capable conversion paths, also convert and package image files that are not referenced by the composed root.">

      <label>Target total texture size</label>
      <input id="targetSize" type="text" placeholder="e.g. 100MB (blank = off)" style="padding:4px;width:200px"
             title="Shrink all textures so their total fits this size">

      <label>Fit strategy</label>
      <div style="display:flex;gap:14px;align-items:center">
        <label style="display:inline-flex;gap:4px"><input type="radio" name="fitStrategy" value="size" checked> Texture size</label>
        <label style="display:inline-flex;gap:4px"><input type="radio" name="fitStrategy" value="quality"> JPEG quality</label>
      </div>

      <label>Re-encode textures</label>
      <input id="reencode" type="checkbox" style="justify-self:start" title="Off = passthrough (copy textures unchanged into the repacked USDZ)">

      <label>JPEG quality</label>
      <input id="jpegQuality" type="number" min="1" max="100" value="90" style="padding:4px;width:120px">

      <label>Max USDC size (MB)</label>
      <input id="maxUsdcMb" type="number" min="0" step="64" value="0" style="padding:4px;width:120px"
             title="0 = conservative default (~100 MB). Raise for large scenes whose flattened USDC root exceeds it. 2048 (2 GB) is the cross-browser-safe ceiling (Firefox/Safari ArrayBuffer limit + wasm32 2 GB heap); Chrome allows up to ~4096.">

      <label>Max WASM heap (MB)</label>
      <input id="maxWasmHeapMb" type="number" min="0" step="64" value="1024" style="padding:4px;width:120px"
             title="0 = off. Browser conversion uses this cap to avoid WASM heap growth aborts; large folder conversions auto-switch to the streaming path before bulk texture assets enter WASM.">
    </div>
    <p style="color:#888;font-size:12px;margin:10px 0 0">
      Set a <b>target total texture size</b> to auto-fit all textures to a budget — choose the lever:
      reduce <b>texture size</b> (keeps PNG) or lower <b>JPEG quality</b> (transcodes to JPG).
      Without a target, browser-supported textures are resized/re-encoded through the canvas API;
      other formats are routed to LightUSD WASM. <b>EXR</b> textures (HDR, allowed in recent USDZ)
      keep their format by default and can be resized; choose PNG/JPEG to tone-map them to LDR.
      EXR is encoded as <b>fp16</b> (half) — EXR→EXR stays half end-to-end (decode → resize → encode,
      no fp32 widening), so HDR re-encode/resize uses about half the memory.
      <b>Resize colorspace</b> controls how WASM-processed textures (incl. EXR) are resampled:
      <i>Auto</i> reads each UsdUVTexture's <code>sourceColorSpace</code> (sRGB color → linear-light,
      data maps → gamma space); <i>sRGB</i> forces linear-light for all; <i>Linear</i> keeps gamma space.
    </p>
  </fieldset>

  <fieldset style="border:1px solid #444;border-radius:6px;padding:12px;margin-bottom:14px">
    <legend style="color:#bbb">Output name</legend>
    <div style="display:grid;grid-template-columns:auto 1fr;gap:8px 12px;align-items:center;font-size:14px">
      <label title="Appended to the source name (before .usdz). Ignored when a custom filename is set.">Filename suffix</label>
      <input id="nameSuffix" type="text" placeholder="e.g. _opt (blank = none)" style="padding:4px;width:240px">

      <label title="Overrides the whole output filename. Leave blank to use source name + suffix.">Custom filename</label>
      <input id="nameCustom" type="text" placeholder="auto: <source><suffix>.usdz" style="padding:4px;width:240px">
    </div>
    <p style="color:#888;font-size:12px;margin:8px 0 0">
      <span id="namePreview" style="color:#9bb">output: —</span>
    </p>
  </fieldset>

  <div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
    <button id="btnConvert" class="btn primary" disabled>Convert</button>
    <button id="btnDownload" class="btn" disabled>Download USDZ</button>
    <span id="status" style="color:#aaa;font-size:13px"></span>
  </div>

  <div id="progressPanel" style="display:none;background:#111;border:1px solid #30304a;border-radius:6px;padding:12px;margin:12px 0 8px">
    <div style="display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:8px">
      <div>
        <div id="progressStage" style="font-size:13px;color:#d7ddff">Preparing conversion...</div>
        <div id="progressDetails" style="font-size:12px;color:#8f94aa;margin-top:2px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:650px"></div>
      </div>
      <div id="progressPercent" style="font-variant-numeric:tabular-nums;color:#d7ddff;font-size:13px">0%</div>
    </div>
    <div style="height:8px;background:#222338;border-radius:999px;overflow:hidden">
      <div id="progressBar" style="height:100%;width:0%;background:#4a6ef0;transition:width 120ms linear"></div>
    </div>
    <div id="textureProgress" style="display:none;margin-top:10px">
      <div style="display:flex;justify-content:space-between;font-size:12px;color:#8f94aa;margin-bottom:5px">
        <span id="textureProgressLabel">Textures</span>
        <span id="textureProgressCount" style="font-variant-numeric:tabular-nums">0 / 0</span>
      </div>
      <div style="height:5px;background:#222338;border-radius:999px;overflow:hidden">
        <div id="textureProgressBar" style="height:100%;width:0%;background:#28b8a8;transition:width 120ms linear"></div>
      </div>
    </div>
  </div>

  <details style="margin-top:18px">
    <summary style="cursor:pointer;color:#bbb">Texture repack tool (merge channels, e.g. R=gloss, G=roughness)</summary>
    <div style="border:1px solid #444;border-radius:6px;padding:12px;margin-top:8px">
      <div id="repackSlots"></div>
      <div style="display:flex;gap:8px;align-items:center;margin-top:8px">
        <label style="font-size:14px">Output channels</label>
        <input id="repackChannels" type="number" min="1" max="4" value="3" style="padding:4px;width:70px">
        <button id="btnRepack" class="btn">Repack &amp; Download PNG</button>
      </div>
    </div>
  </details>

  <pre id="log" style="background:#111;padding:12px;border-radius:6px;margin-top:16px;
       max-height:260px;overflow:auto;font-size:12px;white-space:pre-wrap"></pre>
`;
document.body.style.background = '#0d0d1a';
document.body.appendChild(container);

const styleEl = document.createElement('style');
styleEl.textContent = `
  .btn{display:inline-flex;align-items:center;gap:4px;cursor:pointer;background:#2d2d5e;
    color:#eee;border:1px solid #555;border-radius:5px;padding:7px 14px;font-size:14px;margin:2px}
  .btn.primary{background:#3a5edb;border-color:#4a6ef0}
  .btn:disabled{opacity:.5;cursor:not-allowed}
  #drop.active{border-color:#4a6ef0;background:#22223e}
`;
document.head.appendChild(styleEl);

const els = {
  drop: document.getElementById('drop'),
  folderInput: document.getElementById('folderInput'),
  filesInput: document.getElementById('filesInput'),
  btnClear: document.getElementById('btnClear'),
  fileList: document.getElementById('fileList'),
  rootSelect: document.getElementById('rootSelect'),
  maxSize: document.getElementById('maxSize'),
  resizeColorspace: document.getElementById('resizeColorspace'),
  textureFormat: document.getElementById('textureFormat'),
  rootLayerFormat: document.getElementById('rootLayerFormat'),
  pipeline: document.getElementById('pipeline'),
  variantSelections: document.getElementById('variantSelections'),
  flatten: document.getElementById('flatten'),
  arkitCompatible: document.getElementById('arkitCompatible'),
  includeUnusedTextures: document.getElementById('includeUnusedTextures'),
  reencode: document.getElementById('reencode'),
  jpegQuality: document.getElementById('jpegQuality'),
  maxUsdcMb: document.getElementById('maxUsdcMb'),
  maxWasmHeapMb: document.getElementById('maxWasmHeapMb'),
  nameSuffix: document.getElementById('nameSuffix'),
  nameCustom: document.getElementById('nameCustom'),
  namePreview: document.getElementById('namePreview'),
  btnConvert: document.getElementById('btnConvert'),
  btnDownload: document.getElementById('btnDownload'),
  status: document.getElementById('status'),
  progressPanel: document.getElementById('progressPanel'),
  progressStage: document.getElementById('progressStage'),
  progressDetails: document.getElementById('progressDetails'),
  progressPercent: document.getElementById('progressPercent'),
  progressBar: document.getElementById('progressBar'),
  textureProgress: document.getElementById('textureProgress'),
  textureProgressLabel: document.getElementById('textureProgressLabel'),
  textureProgressCount: document.getElementById('textureProgressCount'),
  textureProgressBar: document.getElementById('textureProgressBar'),
  repackSlots: document.getElementById('repackSlots'),
  repackChannels: document.getElementById('repackChannels'),
  btnRepack: document.getElementById('btnRepack'),
  log: document.getElementById('log'),
};

function log(msg) {
  els.log.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
  els.log.scrollTop = els.log.scrollHeight;
}

function setStatus(s) { els.status.textContent = s; }

const PROGRESS_LABELS = {
  preparing: 'Preparing files...',
  wasm: 'Loading LightUSD WASM...',
  layers: 'Reading USD layers...',
  flatten: 'Composing and flattening...',
  textures: 'Processing textures...',
  assets: 'Packaging assets...',
  package: 'Writing USDZ...',
  complete: 'Complete',
};

const PROGRESS_BASE = {
  preparing: 2,
  wasm: 8,
  layers: 18,
  flatten: 38,
  textures: 52,
  assets: 86,
  package: 94,
  complete: 100,
};

const PROGRESS_SPAN = {
  preparing: 5,
  wasm: 8,
  layers: 18,
  flatten: 14,
  textures: 32,
  assets: 6,
  package: 6,
  complete: 0,
};

let visibleProgressPct = 0;
let lastProgressUpdateMs = 0;

function showProgress() {
  visibleProgressPct = 0;
  lastProgressUpdateMs = 0;
  els.progressPanel.style.display = 'block';
  els.progressBar.style.width = '0%';
  els.progressPercent.textContent = '0%';
  els.progressStage.textContent = PROGRESS_LABELS.preparing;
  els.progressDetails.textContent = '';
  els.textureProgress.style.display = 'none';
  els.textureProgressBar.style.width = '0%';
  els.textureProgressCount.textContent = '0 / 0';
}

function updateTextureProgress(current, total, detail) {
  if (!total) {
    els.textureProgress.style.display = 'none';
    return;
  }
  const cur = Math.max(0, Math.min(total, current || 0));
  els.textureProgress.style.display = 'block';
  els.textureProgressLabel.textContent = detail ? `Textures: ${detail}` : 'Textures';
  els.textureProgressCount.textContent = `${cur.toLocaleString()} / ${total.toLocaleString()}`;
  els.textureProgressBar.style.width = `${Math.round((cur / total) * 100)}%`;
}

function updateProgress(info = {}) {
  const stage = info.stage || 'preparing';
  const total = Number(info.total || 0);
  const current = Number(info.current || 0);
  let pct = PROGRESS_BASE[stage] ?? visibleProgressPct;
  if (total > 0 && PROGRESS_SPAN[stage]) {
    pct += Math.max(0, Math.min(1, current / total)) * PROGRESS_SPAN[stage];
  }
  if (stage === 'complete') pct = 100;

  const now = performance.now();
  const rounded = Math.max(visibleProgressPct, Math.min(100, Math.round(pct)));
  if (now - lastProgressUpdateMs < 80 && rounded < 100 && stage !== 'textures') return;
  lastProgressUpdateMs = now;
  visibleProgressPct = rounded;

  els.progressPanel.style.display = 'block';
  els.progressBar.style.width = `${rounded}%`;
  els.progressPercent.textContent = `${rounded}%`;
  els.progressStage.textContent = PROGRESS_LABELS[stage] || stage;
  els.progressDetails.textContent = info.message || info.path || '';
  if (stage === 'textures' || stage === 'udim') updateTextureProgress(current, total, info.path || info.message || '');
}

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

// ---------------------------------------------------------------------------
// State: uploaded files
// ---------------------------------------------------------------------------

let native = null;
let conversionWorker = null;
let uploaded = []; // [{ path, file }]
let lastResult = null; // { usdz: Uint8Array, filename: string } after a successful convert

// Sanitize a user-supplied filename fragment (strip path separators).
function sanitizeName(s) {
  return String(s || '').replace(/[\\/]+/g, '_').trim();
}

// Compute the output filename from the selected source + suffix/custom inputs.
function outputFilename() {
  const custom = sanitizeName(els.nameCustom.value);
  if (custom) return /\.usdz$/i.test(custom) ? custom : custom + '.usdz';
  const src = els.rootSelect.value || 'output';
  const base = src.split('/').pop().replace(/\.(usd|usda|usdc|usdz)$/i, '') || 'output';
  return base + sanitizeName(els.nameSuffix.value) + '.usdz';
}

function refreshNamePreview() {
  els.namePreview.textContent = uploaded.length
    ? `output: ${outputFilename()}`
    : 'output: —';
}

async function ensureWasm() {
  if (native) return native;
  log('Loading LightUSD WASM module...');
  native = await loadWasm(() => import('./src/lightusd/lightusd.js'));
  log('WASM module loaded.');
  return native;
}

function releaseWasmModule(reason = '') {
  if (!native) return;
  native = null;
  if (reason) log(`Released LightUSD WASM module (${reason}).`);
}

function terminateConversionWorker(reason = '') {
  if (!conversionWorker) return;
  conversionWorker.terminate();
  conversionWorker = null;
  if (reason) log(`Stopped conversion worker (${reason}).`);
}

function refreshFileList() {
  if (!uploaded.length) {
    els.fileList.style.display = 'none';
    els.fileList.innerHTML = '';
    els.rootSelect.innerHTML = '';
    els.btnConvert.disabled = true;
    els.btnClear.disabled = true;
    return;
  }
  els.btnClear.disabled = false;
  els.fileList.style.display = 'block';
  els.fileList.innerHTML = uploaded
    .map(({ path, file }) => `<div>${isImageName(path) ? '🖼️' : '📄'} ${path} <span style="color:#777">(${(file.size / 1024).toFixed(1)} KB)</span></div>`)
    .join('');

  // Populate root USD select.
  const usds = uploaded.filter(u => /\.(usd|usda|usdc|usdz)$/i.test(u.path));
  els.rootSelect.innerHTML = usds.map(u => `<option value="${u.path}">${u.path}</option>`).join('');
  els.btnConvert.disabled = usds.length === 0;
  if (usds.length === 0) {
    setStatus('No USD file found in the upload.');
  } else {
    setStatus(`${uploaded.length} files, ${usds.length} USD layer(s). Ready to convert.`);
  }
  // New input invalidates any prior conversion result.
  invalidateResult();
  refreshNamePreview();
}

function invalidateResult() {
  lastResult = null;
  els.btnDownload.disabled = true;
}

function releasePreviousConversion(reason) {
  invalidateResult();
  terminateConversionWorker(reason);
  releaseWasmModule(reason);
}

// Remove all uploaded files and reset to the initial state. Clearing the file
// inputs' .value lets the user re-pick the same folder/files afterwards.
function clearFiles() {
  uploaded = [];
  releasePreviousConversion('clear');
  els.folderInput.value = '';
  els.filesInput.value = '';
  refreshFileList();
  refreshNamePreview();
  setStatus('Cleared. Upload a folder or files to begin.');
  log('Cleared uploaded files.');
}

async function addFiles(fileEntries) {
  invalidateResult();
  // fileEntries: array of { path, file }
  for (const e of fileEntries) {
    if (!uploaded.some(u => u.path === e.path)) uploaded.push(e);
  }
  refreshFileList();
}

function entriesFromFileList(files) {
  return Array.from(files).map(file => ({
    path: file.webkitRelativePath || file.name,
    file,
  }));
}

els.folderInput.addEventListener('change', e => addFiles(entriesFromFileList(e.target.files)));
els.filesInput.addEventListener('change', e => addFiles(entriesFromFileList(e.target.files)));
els.btnClear.addEventListener('click', clearFiles);

// Output-name controls: live preview; a name change does not invalidate the
// already-converted bytes (it only affects the download filename).
els.nameSuffix.addEventListener('input', refreshNamePreview);
els.nameCustom.addEventListener('input', refreshNamePreview);
els.rootSelect.addEventListener('change', () => { invalidateResult(); refreshNamePreview(); });
let flattenBeforeNextOnly = null;
els.pipeline.addEventListener('change', () => {
  if (els.pipeline.value === 'next-only') {
    flattenBeforeNextOnly = els.flatten.checked;
    els.flatten.checked = false;
    els.flatten.disabled = true;
  } else if (flattenBeforeNextOnly !== null) {
    els.flatten.checked = flattenBeforeNextOnly;
    els.flatten.disabled = false;
    flattenBeforeNextOnly = null;
  }
  invalidateResult();
});

// Drag & drop (supports folders via webkitGetAsEntry).
['dragenter', 'dragover'].forEach(ev =>
  els.drop.addEventListener(ev, e => { e.preventDefault(); els.drop.classList.add('active'); }));
['dragleave', 'drop'].forEach(ev =>
  els.drop.addEventListener(ev, e => { e.preventDefault(); els.drop.classList.remove('active'); }));

els.drop.addEventListener('drop', async e => {
  const items = e.dataTransfer.items;
  const entries = [];
  const walk = async (entry, prefix) => {
    if (entry.isFile) {
      const file = await new Promise((res, rej) => entry.file(res, rej));
      entries.push({ path: prefix + entry.name, file });
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      // readEntries returns at most 100 entries per call; loop until empty.
      let batch;
      do {
        batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        for (const c of batch) await walk(c, prefix + entry.name + '/');
      } while (batch.length > 0);
    }
  };
  if (items && items.length && items[0].webkitGetAsEntry) {
    for (const it of items) {
      const entry = it.webkitGetAsEntry();
      if (entry) await walk(entry, '');
    }
    await addFiles(entries);
  } else {
    await addFiles(entriesFromFileList(e.dataTransfer.files));
  }
});

// ---------------------------------------------------------------------------
// Browser texture helpers
// ---------------------------------------------------------------------------

function browserImageFormat(name) {
  const ext = (name.toLowerCase().split('.').pop() || '');
  if (ext === 'jpg' || ext === 'jpeg') return 'jpeg';
  if (ext === 'png') return 'png';
  return null;
}

function browserTextureConcurrency() {
  const cores = navigator.hardwareConcurrency || 4;
  return Math.max(1, Math.min(8, cores - 1 || 1));
}

function uploadedSizeSum(predicate) {
  return uploaded.reduce((sum, entry) =>
    predicate(entry.path) ? sum + (entry.file ? entry.file.size : 0) : sum, 0);
}

function shouldAutoStreamForWasmCap(opts, rootPath) {
  const cap = Number(opts.wasmHeapLimitBytes || 0);
  if (!cap || opts.pipeline === 'stream' || opts.pipeline === 'stream-next' ||
      opts.pipeline === 'next-only') return null;
  if (!opts.flatten) return null;
  if (!rootPath || /\.usdz$/i.test(rootPath)) return null;
  if ((opts.targetTextureBytes || 0) > 0) return null;

  const imageBytes = uploadedSizeSum(isImageName);
  const usdBytes = uploadedSizeSum((path) => /\.(usd|usda|usdc)$/i.test(path));
  const otherBytes = uploadedSizeSum((path) => !isImageName(path) && !/\.(usd|usda|usdc|usdz)$/i.test(path));
  const currentHeap = wasmHeapByteLength(native);
  const reserve = Math.max(128 * 1024 * 1024, Math.floor(cap * 0.25));
  const availableBulkBudget = currentHeap > cap ? 0 : Math.max(0, cap - reserve);
  // Bulk conversion mirrors textures/passthrough assets into the WASM resolver
  // cache before export. USD layers also need transient composition memory, so
  // count them twice as a conservative browser-side trigger.
  const estimatedBulkBytes = imageBytes + otherBytes + usdBytes * 2;
  if (estimatedBulkBytes <= availableBulkBudget) return null;
  return {
    pipeline: opts.pipeline === 'next' ? 'stream-next' : 'stream',
    estimatedBulkBytes,
    availableBulkBudget,
    imageBytes,
    usdBytes,
    otherBytes,
  };
}

function parseVariantSelections(text) {
  const out = {};
  for (const item of String(text || '').split(',')) {
    const spec = item.trim();
    if (!spec) continue;
    const eq = spec.indexOf('=');
    if (eq <= 0 || eq === spec.length - 1) continue;
    const key = spec.slice(0, eq).trim();
    const value = spec.slice(eq + 1).trim();
    if (key && value) out[key] = value;
  }
  return out;
}

function encodeCanvas(canvas, mime, quality) {
  return new Promise((resolve, reject) => {
    canvas.toBlob(blob => {
      if (!blob) {
        reject(new Error('Canvas encoding failed.'));
        return;
      }
      blob.arrayBuffer().then(buf => resolve(new Uint8Array(buf)), reject);
    }, mime, mime === 'image/jpeg' ? quality : undefined);
  });
}

async function imageBitmapFromBytes(data, name) {
  const fmt = browserImageFormat(name);
  if (!fmt) return null;
  const mime = fmt === 'jpeg' ? 'image/jpeg' : 'image/png';
  return await createImageBitmap(new Blob([data], { type: mime }));
}

async function imageDataFromSlot(slot) {
  const bitmap = await imageBitmapFromBytes(slot.data, slot.name);
  if (!bitmap) return null;
  const canvas = document.createElement('canvas');
  canvas.width = bitmap.width;
  canvas.height = bitmap.height;
  const ctx = canvas.getContext('2d');
  ctx.drawImage(bitmap, 0, 0);
  bitmap.close?.();
  return {
    width: canvas.width,
    height: canvas.height,
    data: ctx.getImageData(0, 0, canvas.width, canvas.height).data,
  };
}

async function browserRepackChannels(slots, channels) {
  const sources = [];
  for (const slot of slots) {
    if (!slot || !slot.data) {
      sources.push(null);
      continue;
    }
    const image = await imageDataFromSlot(slot);
    if (!image) return null;
    sources.push({ ...image, channel: slot.channel });
  }

  const first = sources.find(Boolean);
  const width = first ? first.width : 1;
  const height = first ? first.height : 1;
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  const out = ctx.createImageData(width, height);

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const dst = (y * width + x) * 4;
      for (let c = 0; c < 4; c++) {
        if (c >= channels) {
          out.data[dst + c] = c === 3 ? 255 : 0;
          continue;
        }
        const slot = slots[c];
        const src = sources[c];
        if (src) {
          const sx = Math.min(src.width - 1, Math.floor(x * src.width / width));
          const sy = Math.min(src.height - 1, Math.floor(y * src.height / height));
          out.data[dst + c] = src.data[(sy * src.width + sx) * 4 + src.channel];
        } else if (slot && slot.const !== undefined) {
          out.data[dst + c] = slot.const;
        } else {
          out.data[dst + c] = c === 3 ? 255 : 0;
        }
      }
    }
  }

  ctx.putImageData(out, 0, 0);
  return {
    data: await encodeCanvas(canvas, 'image/png', 1),
    width,
    height,
    channels,
  };
}

// ---------------------------------------------------------------------------
// Convert
// ---------------------------------------------------------------------------

function runConversionWorker(payload) {
  terminateConversionWorker();
  conversionWorker = new Worker(new URL('./usdzconvert.worker.js', import.meta.url), { type: 'module' });
  return new Promise((resolve, reject) => {
    const worker = conversionWorker;
    worker.onmessage = (event) => {
      const data = event.data || {};
      if (data.type === 'log') {
        log(data.message);
      } else if (data.type === 'progress') {
        updateProgress(data.info || {});
      } else if (data.type === 'complete') {
        if (conversionWorker === worker) conversionWorker = null;
        worker.terminate();
        resolve(data);
      } else if (data.type === 'error') {
        if (conversionWorker === worker) conversionWorker = null;
        worker.terminate();
        const err = new Error(data.message || 'Conversion worker failed');
        if (data.stack) err.stack = data.stack;
        reject(err);
      }
    };
    worker.onerror = (event) => {
      if (conversionWorker === worker) conversionWorker = null;
      worker.terminate();
      reject(new Error(event.message || 'Conversion worker failed'));
    };
    worker.postMessage({ type: 'convert', ...payload });
  });
}

els.btnConvert.addEventListener('click', async () => {
  try {
    els.btnConvert.disabled = true;
    releasePreviousConversion('new conversion');
    showProgress();

    const rootPath = els.rootSelect.value;
    const fitStrategy = (document.querySelector('input[name="fitStrategy"]:checked') || {}).value || 'size';
    const targetTextureBytes = parseByteSize(document.getElementById('targetSize').value);
    const maxTextureSize = parseInt(els.maxSize.value, 10) || 0;
    const textureFormat = els.textureFormat.value;
    const reencode = els.reencode.checked;
    const maxWasmHeapMb = parseInt(els.maxWasmHeapMb.value, 10) || 0;
    const needsTextureWork = maxTextureSize > 0 || reencode ||
      String(textureFormat).toLowerCase() !== 'keep' || targetTextureBytes > 0;
    const opts = {
      rootPath,
      udimBake: document.getElementById('udimBake').value,
      udimMaxTiles: Number(document.getElementById('udimMaxTiles').value),
      udimMaxAtlasSize: Number(document.getElementById('udimMaxAtlasSize').value),
      udimMemoryBudgetBytes: Number(document.getElementById('udimMemoryMiB').value) * 1048576,
      udimCrossTile: document.getElementById('udimCrossTile').value,
      udimDensePadding: Number(document.getElementById('udimDensePadding').value),
      udimSubdivisionLevel: Number(document.getElementById('udimSubdivisionLevel').value),
      maxTextureSize,
      resizeColorspace: els.resizeColorspace.value,
      targetTextureBytes,
      fitStrategy,
      reencode,
      textureFormat,
      rootLayerFormat: els.rootLayerFormat.value,
      pipeline: els.pipeline.value,
      variantSelections: parseVariantSelections(els.variantSelections.value),
      flatten: els.flatten.checked,
      arkitCompatible: els.arkitCompatible.checked,
      includeUnusedTextures: els.includeUnusedTextures.checked,
      jpegQuality: parseInt(els.jpegQuality.value, 10) || 90,
      // Raise the USDC writer's file-size AND working-memory caps together: a
      // raised file cap with the conservative default memory cap forces a slow
      // low-memory writer path. On wasm32 the heap is bounded at 2 GB.
      maxUsdcMb: parseInt(els.maxUsdcMb.value, 10) || 0,
      maxMemMb: parseInt(els.maxUsdcMb.value, 10) || 0,
      wasmHeapLimitBytes: maxWasmHeapMb > 0 ? maxWasmHeapMb * 1024 * 1024 : 0,
    };
    const colorspaceAware = opts.resizeColorspace === 'auto' ||
                            opts.resizeColorspace === 'srgb';
    if (targetTextureBytes > 0) {
      log(`Fitting textures to ${(targetTextureBytes / 1048576).toFixed(1)} MB via "${fitStrategy}" strategy...`);
    }
    const autoStream = shouldAutoStreamForWasmCap(opts, rootPath);
    if (autoStream) {
      log(`WASM heap cap: estimated bulk asset load ` +
          `${(autoStream.estimatedBulkBytes / 1048576).toFixed(1)} MiB exceeds ` +
          `${(autoStream.availableBulkBudget / 1048576).toFixed(1)} MiB budget; ` +
          `using ${autoStream.pipeline} pipeline.`);
      opts.pipeline = autoStream.pipeline;
      updateProgress({
        stage: 'preparing',
        current: 1,
        total: 1,
        message: `Auto-switched to ${autoStream.pipeline} for WASM heap cap`,
      });
    }

    setStatus('Converting...');
    updateProgress({ stage: 'preparing', current: 1, total: 1, message: rootPath });
    const result = await runConversionWorker({
      files: uploaded.map(({ path, file }) => ({ path, file })),
      opts,
      needsTextureWork,
      colorspaceAware,
      textureConcurrency: browserTextureConcurrency(),
    });
    const usdz = result.usdz instanceof Uint8Array ? result.usdz : new Uint8Array(result.usdz);
    const stats = result.stats || {};
    log(`Converted OK. textures: ${stats.textures || 0}, resized: ${stats.resized || 0}, ` +
        `reencoded: ${stats.reencoded || 0}, audio: ${stats.audio || 0}, ` +
        `other assets: ${stats.otherAssets || 0}. USDZ: ${usdz.length} bytes`);
    if (result.textureStats) {
      const tex = result.textureStats;
      log(`Worker texture time: decode ${tex.decodeMs.toFixed(1)} ms, ` +
          `raster ${tex.rasterMs.toFixed(1)} ms, encode ${tex.encodeMs.toFixed(1)} ms; ` +
          `processed ${tex.processed}, skipped ${tex.skipped}`);
    }

    lastResult = { usdz, filename: outputFilename() };
    els.btnDownload.disabled = false;
    refreshNamePreview();
    updateProgress({ stage: 'complete', message: `${usdz.length.toLocaleString()} bytes` });
    setStatus(`✓ Converted — ${usdz.length.toLocaleString()} bytes. Ready to download.`);
  } catch (err) {
    terminateConversionWorker('failed conversion');
    log('ERROR: ' + (err && err.message ? err.message : err));
    els.progressStage.textContent = 'Conversion failed';
    els.progressDetails.textContent = err && err.message ? err.message : String(err);
    setStatus('✗ Conversion failed — see log.');
  } finally {
    els.btnConvert.disabled = !uploaded.some(u => /\.(usd|usda|usdc|usdz)$/i.test(u.path));
  }
});

els.btnDownload.addEventListener('click', () => {
  if (!lastResult) {
    setStatus('Nothing to download — convert first.');
    return;
  }
  const filename = outputFilename(); // re-read in case the name was edited
  downloadBlob(new Blob([lastResult.usdz], { type: 'model/vnd.usdz+zip' }), filename);
  log(`Downloaded ${filename} (${lastResult.usdz.length} bytes)`);
  setStatus(`Downloaded ${filename}.`);
});

// ---------------------------------------------------------------------------
// Repack tool
// ---------------------------------------------------------------------------

const REPACK_CHANS = ['R', 'G', 'B', 'A'];
els.repackSlots.innerHTML = REPACK_CHANS.map((c, i) => `
  <div style="display:flex;gap:8px;align-items:center;margin:4px 0;font-size:14px">
    <b style="width:18px">${c}</b>
    <label class="btn" style="padding:4px 10px">file<input type="file" data-slot="${i}" accept=".png,.jpg,.jpeg" style="display:none"></label>
    <span data-slotname="${i}" style="color:#999;flex:1">(none)</span>
    <label>src ch</label>
    <select data-slotch="${i}" style="padding:3px"><option>0</option><option>1</option><option>2</option><option>3</option></select>
    <label>or const</label>
    <input type="number" data-slotconst="${i}" min="0" max="255" placeholder="-" style="padding:3px;width:70px">
  </div>`).join('');

const repackFiles = [null, null, null, null];
els.repackSlots.querySelectorAll('input[type=file]').forEach(inp => {
  inp.addEventListener('change', async e => {
    const i = +inp.dataset.slot;
    const f = e.target.files[0];
    repackFiles[i] = f ? { name: f.name, data: new Uint8Array(await f.arrayBuffer()) } : null;
    els.repackSlots.querySelector(`[data-slotname="${i}"]`).textContent = f ? f.name : '(none)';
  });
});

els.btnRepack.addEventListener('click', async () => {
  try {
    const channels = parseInt(els.repackChannels.value, 10) || 3;
    const slots = [];
    const args = { channels, format: 'png' };
    for (let i = 0; i < 4; i++) {
      const constEl = els.repackSlots.querySelector(`[data-slotconst="${i}"]`);
      const chEl = els.repackSlots.querySelector(`[data-slotch="${i}"]`);
      const key = REPACK_CHANS[i].toLowerCase();
      if (repackFiles[i]) {
        const channel = parseInt(chEl.value, 10) || 0;
        args[key] = { data: repackFiles[i].data, channel };
        slots[i] = { ...repackFiles[i], channel };
      } else if (constEl.value !== '') {
        const value = parseInt(constEl.value, 10) || 0;
        args[key] = { const: value };
        slots[i] = { const: value };
      } else {
        slots[i] = null;
      }
    }
    log('Repacking channels...');
    let res = null;
    try {
      res = await browserRepackChannels(slots, channels);
    } catch (err) {
      log(`  browser repack failed (${err && err.message ? err.message : err})`);
    }
    if (res) {
      log(`Packed ${res.width}x${res.height}x${res.channels}, ${res.data.length} bytes [browser]`);
      downloadBlob(new Blob([res.data], { type: 'image/png' }), 'packed.png');
      return;
    }

    log('Browser repack unavailable for these inputs; using LightUSD WASM.');
    await ensureWasm();
    res = native.repackChannels(args);
    if (!res.success) throw new Error(res.error);
    log(`Packed ${res.width}x${res.height}x${res.channels}, ${res.data.length} bytes`);
    downloadBlob(new Blob([new Uint8Array(res.data)], { type: 'image/png' }), 'packed.png');
  } catch (err) {
    log('Repack ERROR: ' + (err && err.message ? err.message : err));
  }
});

log('usdzconvert ready. Upload a folder or files to begin.');
