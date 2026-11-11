// USD Export Demo — LightUSD WASM
// Demonstrates: load USD, generate sample scene, export as USDA/USDC/USDZ
// Textures encoded via browser Canvas API (PNG/JPEG); lightusd image-writer for EXR only.

// ---- UI Setup ----
const container = document.createElement('div');
container.style.cssText = 'max-width:720px;margin:40px auto;padding:20px;';
container.innerHTML = `
  <h1 style="margin:0 0 8px">LightUSD USD Export Demo</h1>
  <p style="color:#aaa;margin:0 0 20px">Load or generate a USD scene, then export as USDA / USDC / USDZ.</p>

  <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px">
    <button id="btnGenerate">Generate Sample Scene</button>
    <label style="display:inline-flex;align-items:center;gap:4px;cursor:pointer;
           background:#2d2d5e;border:1px solid #555;border-radius:4px;padding:6px 12px;color:#eee">
      Load USD File
      <input type="file" id="fileInput" accept=".usd,.usda,.usdc,.usdz" style="display:none">
    </label>
  </div>

  <div id="exportButtons" style="display:none;margin-bottom:16px;display:flex;gap:8px;flex-wrap:wrap">
    <button id="btnUSDA">Export USDA</button>
    <button id="btnUSDC">Export USDC</button>
    <button id="btnUSDZ">Export USDZ</button>
  </div>

  <div id="preview" style="display:none;margin-bottom:16px">
    <h3 style="margin:0 0 4px">Texture Preview</h3>
    <canvas id="previewCanvas" width="256" height="256"
            style="border:1px solid #444;image-rendering:pixelated"></canvas>
  </div>

  <pre id="log" style="background:#111;padding:12px;border-radius:4px;
       max-height:300px;overflow:auto;font-size:13px;white-space:pre-wrap"></pre>
`;
document.body.appendChild(container);

const btnGenerate = document.getElementById('btnGenerate');
const btnUSDA = document.getElementById('btnUSDA');
const btnUSDC = document.getElementById('btnUSDC');
const btnUSDZ = document.getElementById('btnUSDZ');
const fileInput = document.getElementById('fileInput');
const exportDiv = document.getElementById('exportButtons');
const previewDiv = document.getElementById('preview');
const previewCanvas = document.getElementById('previewCanvas');
const logEl = document.getElementById('log');

// Style buttons
document.querySelectorAll('button').forEach(b => {
  b.style.cssText = 'background:#2d2d5e;color:#eee;border:1px solid #555;border-radius:4px;padding:8px 16px;cursor:pointer;font-size:14px';
});

function log(msg) {
  const ts = new Date().toLocaleTimeString();
  logEl.textContent += `[${ts}] ${msg}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

// ---- Texture Generation via Canvas API ----

function generateCheckerboardRGBA(size, tileSize) {
  const data = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const checker = ((Math.floor(x / tileSize) + Math.floor(y / tileSize)) % 2 === 0);
      const idx = (y * size + x) * 4;
      // Use warm tones: cream and teal
      if (checker) {
        data[idx] = 240; data[idx + 1] = 228; data[idx + 2] = 200; // cream
      } else {
        data[idx] = 40; data[idx + 1] = 120; data[idx + 2] = 130; // teal
      }
      data[idx + 3] = 255;
    }
  }
  return data;
}

async function encodeRGBAAsPNG(rgbaData, width, height) {
  const canvas = document.getElementById('texCanvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  const imgData = new ImageData(new Uint8ClampedArray(rgbaData.buffer), width, height);
  ctx.putImageData(imgData, 0, 0);
  const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/png'));
  return new Uint8Array(await blob.arrayBuffer());
}

// ---- Three.js Texture Extraction (for loaded scenes) ----

async function extractTextureAsPNG(threeTexture) {
  if (!threeTexture || !threeTexture.image) return null;
  const img = threeTexture.image;
  const canvas = document.createElement('canvas');
  canvas.width = img.width || img.naturalWidth || 256;
  canvas.height = img.height || img.naturalHeight || 256;
  const ctx = canvas.getContext('2d');
  if (img instanceof ImageBitmap || img instanceof HTMLImageElement || img instanceof HTMLCanvasElement) {
    ctx.drawImage(img, 0, 0);
  } else if (img.data) {
    // Raw pixel data (e.g. from DataTexture)
    const imgData = new ImageData(new Uint8ClampedArray(img.data), canvas.width, canvas.height);
    ctx.putImageData(imgData, 0, 0);
  }
  const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/png'));
  return new Uint8Array(await blob.arrayBuffer());
}

// ---- WASM Module ----

let native = null;
let usd = null;
let sceneReady = false;

async function initWasm() {
  log('Loading LightUSD WASM module...');
  try {
    const mod = await import('./src/lightusd/lightusd.js');
    native = await mod.default();
    log('WASM module loaded.');
  } catch (e) {
    log('ERROR: Failed to load WASM module: ' + e.message);
    throw e;
  }
}

function showExportButtons() {
  exportDiv.style.display = 'flex';
}

// ---- Generate Sample Scene ----

btnGenerate.addEventListener('click', async () => {
  if (!native) await initWasm();

  log('Generating checkerboard texture via Canvas...');
  const texSize = 256;
  const rgba = generateCheckerboardRGBA(texSize, 32);

  // Show preview
  const pCtx = previewCanvas.getContext('2d');
  const imgData = new ImageData(new Uint8ClampedArray(rgba.buffer), texSize, texSize);
  pCtx.putImageData(imgData, 0, 0);
  previewDiv.style.display = 'block';

  // Encode as PNG using browser Canvas
  const pngBytes = await encodeRGBAAsPNG(rgba, texSize, texSize);
  log(`Texture encoded as PNG: ${pngBytes.length} bytes`);

  // Create native instance
  if (usd) usd.delete();
  usd = new native.USDZConverterNative();

  // Pass texture to WASM asset cache
  // The core converter copies the typed bytes into its asset cache.
  usd.setAsset('textures/checkerboard.png', pngBytes);
  log('Texture set in asset cache.');

  // Build sample scene in WASM
  const ok = usd.createSampleScene();
  if (!ok) {
    log('ERROR: createSampleScene failed: ' + usd.error());
    return;
  }

  sceneReady = true;
  showExportButtons();
  log('Sample scene created (textured quad with UsdPreviewSurface material).');
});

// ---- Load USD File ----

fileInput.addEventListener('change', async (e) => {
  const file = e.target.files[0];
  if (!file) return;

  if (!native) await initWasm();

  log(`Loading file: ${file.name} (${(file.size / 1024).toFixed(1)} KB)...`);

  const fileData = new Uint8Array(await file.arrayBuffer());

  if (usd) usd.delete();
  usd = new native.USDZConverterNative();

  const ok = usd.loadFromBinary(fileData, file.name);
  if (!ok) {
    log('ERROR loading file: ' + usd.error());
    return;
  }

  sceneReady = true;
  showExportButtons();
  log(`File loaded successfully. You can now export.`);

  if (usd.warn()) {
    log('WARN: ' + usd.warn());
  }
});

// ---- Export Handlers ----

btnUSDA.addEventListener('click', () => {
  if (!sceneReady || !usd) { log('No scene loaded.'); return; }

  log('Exporting as USDA...');
  const usda = usd.exportAsUSDA();
  if (!usda || usda.length === 0) {
    log('ERROR: USDA export failed: ' + usd.error());
    return;
  }

  log(`USDA exported: ${usda.length} chars`);
  const blob = new Blob([usda], { type: 'text/plain' });
  downloadBlob(blob, 'export.usda');
  log('Download triggered: export.usda');
});

btnUSDC.addEventListener('click', () => {
  if (!sceneReady || !usd) { log('No scene loaded.'); return; }

  log('Exporting as USDC...');
  const data = usd.exportAsUSDC();
  if (!data) {
    log('ERROR: USDC export failed: ' + usd.error());
    return;
  }

  // typed_memory_view returns a view into WASM memory - copy it
  const bytes = new Uint8Array(data);
  log(`USDC exported: ${bytes.length} bytes`);
  const blob = new Blob([bytes], { type: 'application/octet-stream' });
  downloadBlob(blob, 'export.usdc');
  log('Download triggered: export.usdc');
});

btnUSDZ.addEventListener('click', () => {
  if (!sceneReady || !usd) { log('No scene loaded.'); return; }

  log('Exporting as USDZ (with packed textures)...');
  const data = usd.exportAsUSDZ();
  if (!data) {
    log('ERROR: USDZ export failed: ' + usd.error());
    return;
  }

  // typed_memory_view returns a view into WASM memory - copy it
  const bytes = new Uint8Array(data);
  log(`USDZ exported: ${bytes.length} bytes`);
  const blob = new Blob([bytes], { type: 'model/vnd.usdz+zip' });
  downloadBlob(blob, 'export.usdz');
  log('Download triggered: export.usdz');
});

// ---- Init ----
log('LightUSD USD Export Demo ready.');
log('Click "Generate Sample Scene" or load a USD file to start.');
