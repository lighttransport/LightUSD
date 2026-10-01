#!/usr/bin/env node
// usdzconvert CLI — LightUSD WASM
//
// Convert a USD file (plus its textures) or a whole folder into a USDZ, with
// optional texture resize / re-encode. Also a standalone channel-repack mode.
//
// Usage:
//   vite-node cli/usdzconvert.js <input-dir|input.usd> [options]
//   vite-node cli/usdzconvert.js --repack out.png -packR a.png:0 -packG b.png:0 [options]

import { udimOptions } from '../src/udim-bake.js';
import fs from 'node:fs';
import path from 'node:path';
import { convertFolderToUSDZ, convertSourceToUSDZStreaming, loadWasm, parseByteSize } from '../src/usdzconvert.js';

// Load the Emscripten glue directly (no three.js / vite-node dependency) so the
// CLI runs with plain `node`. LIGHTUSD_WASM64=1 selects the 64-bit (8 GB) glue
// — used as a fallback for scenes that overflow the wasm32 2 GB ceiling. Falls
// back to the 32-bit glue if the 64-bit build is not present.
function selectWasmGlue() {
  if (process.env.LIGHTUSD_WASM64 === '1') {
    const url64 = new URL('../src/lightusd/lightusd_64.js', import.meta.url);
    if (fs.existsSync(url64)) return url64.href;
    console.error('[usdzconvert] LIGHTUSD_WASM64=1 but lightusd_64.js not found; using wasm32.');
  }
  return new URL('../src/lightusd/lightusd.js', import.meta.url).href;
}
const wasmGlue = selectWasmGlue();

function printHelp() {
  console.log(`
usdzconvert CLI — LightUSD WASM

Usage:
  vite-node cli/usdzconvert.js <input-dir|input.usd> [options]
  vite-node cli/usdzconvert.js --repack <out.png> -packR <src> [-packG <src> ...] [options]

Convert options:
  -o, --output <file>      Output .usdz path (default: <root>.usdz)
  --root <relpath>         Root USD layer within the input dir (default: auto)
  --resize <N>             Cap each texture's longest edge to N pixels
  --resize-colorspace <s>  How to resample when resizing:
                           'auto'   = per-texture from UsdUVTexture sourceColorSpace
                                      (sRGB textures in linear light, data maps
                                      gamma-space) — correct without guessing;
                           'srgb'   = force linear-light for ALL resized textures;
                           'linear' = force gamma-space for all (default).
  --bake-udim <mode>       off, grid, or dense (default: off).
  --udim-max-tiles <n>     Maximum tiles per atlas (default: 100; excess fails).
  --udim-max-atlas-size <n> Maximum atlas edge (default: 8192).
  --udim-memory-budget <size> Working-memory cap (default: 512 MiB).
  --udim-cross-tile <mode> reject or split (default: reject).
  --udim-dense-padding <n> Gutter pixels (default: 2).
  --udim-subdivision-level <n> Refinement level (default: 2).
  --texture-format <fmt>   Texture output: keep, png, jpeg, exr (default: keep).
                           'keep' preserves each source format (EXR stays EXR,
                           resize-only — HDR is retained). 'exr' forces EXR (fp16)
                           output for all textures (allowed in recent USDZ);
                           'png'/'jpeg' tone-map EXR to LDR.
  --root-layer-format <fmt> USDZ root layer: usdc, usda (default: usdc)
  --arkit-compatible       Force ARKit-friendly flattened USDC package metadata
  --no-flatten             Accepted for parity; JS/WASM export still flattens
  --jpeg-quality <1-100>   JPEG quality when re-encoding (default 90)
  --max-usdc-mb <N>        Raise the USDC root-layer write size cap to N MB
                           (0 = keep the conservative ~100 MB WASM default).
                           Needed for very large scenes.
  --max-mem-mb <N>         Raise the USDC writer memory cap to N MB (0 = default)
  --max-wasm-heap-mb <N>   Best-effort cap for bulk WASM asset registration
                           (0 = off). Use stream/stream-next for large folders.
  --url-list <file.json>   Streaming source as URLs: [{key,url}...], [url...],
                           or {baseUrl, files}. Implies network fetch per asset.
  --pipeline <legacy|next|stream|stream-next> Flatten pipeline (default: legacy).
                           'stream': lazy folder/url-list source — USD layers
                           only in memory; textures fetched->processed->
                           zip-appended one at a time (lowest peak RSS;
                           requires -o and flatten). 'next' uses the
                           experimental low-memory lazy-ValueRep path for a single
                           .usdz with a top-level USDC root; falls back to legacy
                           otherwise. 'stream-next': the stream source combined
                           with the next multi-asset compose+flatten in wasm
                           (USDC layers only; supports texture rename remaps
                           for --texture-format png/jpeg/exr); falls back to
                           the legacy stream compose when the input doesn't
                           qualify. Also via LIGHTUSD_PIPELINE env.
  --include-unused-textures
                           With --pipeline stream-next, convert/package texture
                           files from the input folder even when the next-composed
                           root does not reference them. Default is to skip
                           unreferenced textures for lower memory and smaller
                           USDZ output.
  --variant <set=value>    Override a variant set during next flattening.
                           Repeat for multiple sets. Example: --variant lod=high
  --stream-textures        Re-encode/resize textures one at a time and repack in
                           JS, so decoded images never accumulate in the WASM
                           heap. This is the DEFAULT for a single .usdz with
                           --texture-format keep + re-encode/resize (it keeps
                           large texture-heavy scenes under the wasm32
                           2 GB ceiling). The flag forces it; otherwise it is
                           auto-enabled and falls back for nested roots / non-keep
                           formats.
  --no-stream-textures     Force the in-heap batch texture path (higher memory).
  --stream-write           (--pipeline next, DEFAULT) Stream the flattened root
                           crate straight into the .usdz instead of buffering it,
                           so the output crate never materializes in the WASM heap
                           or in JS. Byte-identical output; roughly halves peak
                           RSS on large scenes. Engages when writing to a file
                           (-o); falls back to buffering otherwise.
  --no-stream-write        Force the buffered root path (LIGHTUSD_STREAM_WRITE=0).
  --png-encoder <fpnge|fpng|auto>  PNG encoder hint (WASM always uses fpng)
  --texture-codec <auto|best|wasm|js>
                           Texture pipeline: auto (default), best, wasm, or js.
                           auto keeps the low-memory WASM path for small/medium
                           jobs and uses a small Node worker pool for large PNG
                           resize-to-PNG jobs. js runs PNG via worker_threads
                           + pngjs/node zlib in parallel; non-PNG falls back
                           to wasm. best uses a rough RSS estimate and
                           --texture-memory-budget to choose wasm/js jobs.
  --texture-jobs <N|best>  Worker threads for --texture-codec js.
                           best estimates a safe count from the memory budget.
  --texture-memory-budget <size>
                           Best-effort RSS budget for auto/best texture codec
                           selection (e.g. 1GB, 900MB). 0 = no explicit cap.
  --no-reencode            Copy unmodified textures through unchanged
  --optimize-materials <mode>
                           Material optimization: off, dedupe, preview, atlas.
                           preview canonicalizes supported UsdPreviewSurface graphs;
                           atlas applies preview dedupe without atlas images yet.
  --material-atlas-size <N>       Max generated atlas edge (default 4096)
  --material-atlas-tile-size <N>  Tile edge for atlas mode (default 512)
  --material-atlas-padding <N>    Gutter padding pixels for atlas mode (default 2)
  --material-atlas-min-group-size <N>
                           Minimum compatible materials before atlas generation
  -v, --verbose            Verbose logging
  -h, --help               Show this help

Fit textures to a total size budget:
  --target-size <size>     Shrink all textures so their total fits <size> (e.g. 100MB)
  --fit-strategy <size|quality>  Lever: reduce dimensions (size) or transcode to
                           JPEG + lower quality (quality). Default: size
  --fit-min-size <N>       Smallest longest-edge for the size search (default 64)
  --fit-min-quality <N>    Lowest JPEG quality for the quality search (default 30)

Repack mode (merge channels into one image, e.g. R=gloss, G=roughness):
  --repack <out.png>       Enable repack mode; write packed image here
  -packR/-packG/-packB/-packA <src>
                           Channel source: 'file.png:CH' (CH 0..3) or 'const:VALUE'
  --pack-channels <1-4>    Output channel count (default: from -pack* flags)

Examples:
  vite-node cli/usdzconvert.js ./model_folder -o model.usdz --resize 1024 -v
  vite-node cli/usdzconvert.js ./model/scene.usda -o out.usdz
  vite-node cli/usdzconvert.js --repack orm.png -packR ao.png:0 -packG rough.png:0 -packB metal.png:0 --pack-channels 3
`);
}

function parseArgs() {
  const args = process.argv.slice(2);
  const o = {
    input: null, output: null, root: null, resize: 0, jpegQuality: 90,
    pngEncoder: 'auto', reencode: true, verbose: false, resizeColorspace: '',
    textureFormat: 'keep', rootLayerFormat: 'usdc', arkitCompatible: false,
    flatten: true,
    targetSize: 0, fitStrategy: 'size', fitMinSize: 64, fitMinQuality: 30,
    maxUsdcMb: 0, maxMemMb: 0,
    wasmHeapLimitBytes: process.env.LIGHTUSD_MAX_WASM_HEAP ?
      parseByteSize(process.env.LIGHTUSD_MAX_WASM_HEAP) : 0,
    pipeline: process.env.LIGHTUSD_PIPELINE || 'legacy',
    // undefined => auto (stream textures for a single .usdz keep-format re-encode,
    // the low-memory default); true => force; false => force the in-heap path.
    streamTextures: undefined,
    // Stream the flattened root crate straight into the .usdz (next pipeline only)
    // instead of buffering it — keeps the output crate out of the WASM heap and JS.
    // Default ON for --pipeline next when writing to a file; LIGHTUSD_STREAM_WRITE=0
    // (or --no-stream-write) disables it.
    streamWrite: process.env.LIGHTUSD_STREAM_WRITE !== '0',
    repack: null, packChannels: 0, pack: { R: null, G: null, B: null, A: null },
    // 'auto' (default): choose low-memory WASM or a small JS worker pool for
    // large PNG resize jobs. 'wasm': single-threaded convertImage. 'js': PNG
    // work runs on a worker_threads pool; non-PNG still falls back to WASM.
    textureCodec: process.env.LIGHTUSD_TEXTURE_CODEC || 'auto',
    textureJobs: 0,  // 0 = cpu count - 1; 'best' = estimate from budget
    textureMemoryBudget: process.env.LIGHTUSD_TEXTURE_MEMORY_BUDGET ?
      parseByteSize(process.env.LIGHTUSD_TEXTURE_MEMORY_BUDGET) : 0,
    optimizeMaterials: 'off',
    materialAtlasSize: 4096,
    materialAtlasTileSize: 512,
    materialAtlasPadding: 2,
    materialAtlasMinGroupSize: 2,
    optimizeGeometry: 'off',
    includeUnusedTextures: false,
    meshMergeMaxInputFaces: 2048,
    meshMergeMaxInputPoints: 4096,
    meshMergeMaxAggregateFaces: 65536,
    meshMergeMinGroupSize: 2,
    variantSelections: {},
  };
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '-h' || a === '--help') { printHelp(); process.exit(0); }
    else if (a === '-o' || a === '--output') o.output = args[++i];
    else if (a === '--root') o.root = args[++i];
    else if (a === '--resize') o.resize = parseInt(args[++i], 10) || 0;
    else if (a === '--resize-colorspace') o.resizeColorspace = args[++i];
    else if (a === '--bake-udim') o.udimBake = args[++i];
    else if (a === '--udim-cross-tile') o.udimCrossTile = args[++i];
    else if (a === '--udim-max-tiles') o.udimMaxTiles = Number(args[++i]);
    else if (a === '--udim-max-atlas-size') o.udimMaxAtlasSize = Number(args[++i]);
    else if (a === '--udim-memory-budget') o.udimMemoryBudgetBytes = parseByteSize(args[++i]);
    else if (a === '--udim-dense-padding') o.udimDensePadding = Number(args[++i]);
    else if (a === '--udim-subdivision-level') o.udimSubdivisionLevel = Number(args[++i]);
    else if (a === '--texture-format') o.textureFormat = args[++i];
    else if (a === '--root-layer-format') o.rootLayerFormat = args[++i];
    else if (a === '--arkit-compatible') o.arkitCompatible = true;
    else if (a === '--no-flatten') o.flatten = false;
    else if (a === '--jpeg-quality') o.jpegQuality = parseInt(args[++i], 10) || 90;
    else if (a === '--png-encoder') o.pngEncoder = args[++i];
    else if (a === '--no-reencode') o.reencode = false;
    else if (a === '--target-size') o.targetSize = parseByteSize(args[++i]);
    else if (a === '--fit-strategy') o.fitStrategy = args[++i];
    else if (a === '--fit-min-size') o.fitMinSize = parseInt(args[++i], 10) || 64;
    else if (a === '--fit-min-quality') o.fitMinQuality = parseInt(args[++i], 10) || 30;
    else if (a === '--max-usdc-mb') o.maxUsdcMb = parseInt(args[++i], 10) || 0;
    else if (a === '--max-mem-mb') o.maxMemMb = parseInt(args[++i], 10) || 0;
    else if (a === '--max-wasm-heap-mb') o.wasmHeapLimitBytes = (parseInt(args[++i], 10) || 0) * 1024 * 1024;
    else if (a === '--pipeline') o.pipeline = args[++i];
    else if (a === '--stream-textures') o.streamTextures = true;
    else if (a === '--no-stream-textures') o.streamTextures = false;
    else if (a === '--stream-write') o.streamWrite = true;
    else if (a === '--no-stream-write') o.streamWrite = false;
    else if (a === '--texture-codec') o.textureCodec = args[++i];
    else if (a === '--texture-jobs') {
      const v = args[++i];
      o.textureJobs = String(v).toLowerCase() === 'best' ? 'best' : (parseInt(v, 10) || 0);
    }
    else if (a === '--texture-memory-budget') o.textureMemoryBudget = parseByteSize(args[++i]);
    else if (a === '--include-unused-textures') o.includeUnusedTextures = true;
    else if (a === '--optimize-materials') o.optimizeMaterials = args[++i];
    else if (a === '--material-atlas-size') o.materialAtlasSize = parseInt(args[++i], 10) || 4096;
    else if (a === '--material-atlas-tile-size') o.materialAtlasTileSize = parseInt(args[++i], 10) || 512;
    else if (a === '--material-atlas-padding') o.materialAtlasPadding = parseInt(args[++i], 10) || 0;
    else if (a === '--material-atlas-min-group-size') o.materialAtlasMinGroupSize = parseInt(args[++i], 10) || 2;
    else if (a === '--optimize-geometry' || a === '--optimize-meshes') o.optimizeGeometry = args[++i];
    else if (a === '--mesh-merge-max-input-faces') o.meshMergeMaxInputFaces = parseInt(args[++i], 10) || 2048;
    else if (a === '--mesh-merge-max-input-points') o.meshMergeMaxInputPoints = parseInt(args[++i], 10) || 4096;
    else if (a === '--mesh-merge-max-aggregate-faces') o.meshMergeMaxAggregateFaces = parseInt(args[++i], 10) || 65536;
    else if (a === '--mesh-merge-min-group-size') o.meshMergeMinGroupSize = parseInt(args[++i], 10) || 2;
    else if (a === '--variant' || a === '--variant-selection') {
      const spec = args[++i] || '';
      const eq = spec.indexOf('=');
      if (eq <= 0 || eq === spec.length - 1) {
        console.error('--variant expects set=value');
        process.exit(1);
      }
      o.variantSelections[spec.slice(0, eq)] = spec.slice(eq + 1);
    }
    else if (a === '--url-list') o.urlList = args[++i];
    else if (a === '-v' || a === '--verbose') o.verbose = true;
    else if (a === '--repack') o.repack = args[++i];
    else if (a === '--pack-channels') o.packChannels = parseInt(args[++i], 10) || 0;
    else if (a === '-packR') o.pack.R = args[++i];
    else if (a === '-packG') o.pack.G = args[++i];
    else if (a === '-packB') o.pack.B = args[++i];
    else if (a === '-packA') o.pack.A = args[++i];
    else if (!o.input) o.input = a;
    else { console.error('Unknown argument: ' + a); process.exit(1); }
  }
  return o;
}

// Recursively read a directory into a Map<relPath, Uint8Array>.
// fs.writeFileSync caps a single write at 2^31-1 bytes; a passthrough .usdz of
// a multi-GB scene exceeds that. Write in <2 GiB chunks instead.
function writeFileChunked(path, bytes) {
  const CHUNK = 1 << 30;  // 1 GiB
  const fd = fs.openSync(path, 'w');
  try {
    for (let pos = 0; pos < bytes.length; pos += CHUNK) {
      const n = Math.min(CHUNK, bytes.length - pos);
      fs.writeSync(fd, bytes, pos, n, pos);
    }
  } finally {
    fs.closeSync(fd);
  }
}

// Lazy streaming sources for --pipeline stream: keys are listed up front,
// bytes are fetched on demand (fs read or HTTP), never all-at-once.
function folderSource(dir) {
  const keys = [];
  const walk = (cur, prefix) => {
    for (const entry of fs.readdirSync(cur, { withFileTypes: true })) {
      const full = path.join(cur, entry.name);
      const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(full, rel);
      else if (entry.isFile()) keys.push(rel);
    }
  };
  walk(dir, '');
  return {
    keys,
    fetch: async (key, {maxBytes} = {}) => {
      const file = path.join(dir, key);
      if (maxBytes !== undefined && (await fs.promises.stat(file)).size > maxBytes)
        throw new Error(`UDIM bake: encoded tile exceeds memory limit: ${key}`);
      return new Uint8Array(await fs.promises.readFile(file));
    },
    fetchSync: (key) => new Uint8Array(fs.readFileSync(path.join(dir, key))),
  };
}

// --url-list file: JSON, either [{key, url}, ...], ["url", ...] (key = path
// part after the last common base), or { baseUrl, files: ["rel", ...] }.
function urlListSource(file) {
  const spec = JSON.parse(fs.readFileSync(file, 'utf8'));
  let entries;
  if (Array.isArray(spec)) {
    entries = spec.map((e) => (typeof e === 'string')
      ? { key: decodeURIComponent(new URL(e).pathname.replace(/^\//, '')), url: e }
      : e);
  } else if (spec && spec.baseUrl && Array.isArray(spec.files)) {
    const base = spec.baseUrl.replace(/\/?$/, '/');
    entries = spec.files.map((rel) => ({ key: rel, url: new URL(rel, base).href }));
  } else {
    throw new Error('--url-list must be [{key,url}...], [url...], or {baseUrl, files}');
  }
  const byKey = new Map(entries.map((e) => [e.key, e.url]));
  return {
    keys: [...byKey.keys()],
    fetch: async (key, {maxBytes} = {}) => {
      const res = await fetch(byKey.get(key));
      if (!res.ok) throw new Error(`fetch ${byKey.get(key)}: HTTP ${res.status}`);
      if (maxBytes === undefined) return new Uint8Array(await res.arrayBuffer());
      const length = Number(res.headers.get('content-length'));
      if (length > maxBytes) {
        await res.body?.cancel();
        throw new Error(`UDIM bake: encoded tile exceeds memory limit: ${key}`);
      }
      const reader = res.body.getReader(), chunks = [];
      let size = 0;
      try {
        for (;;) {
          const {value, done} = await reader.read();
          if (done) break;
          if (value.length > maxBytes - size) {
            await reader.cancel();
            throw new Error(`UDIM bake: encoded tile exceeds memory limit: ${key}`);
          }
          size += value.length; chunks.push(value);
        }
      } finally {reader.releaseLock();}
      const bytes = new Uint8Array(size);
      let offset = 0;
      for (const chunk of chunks) {bytes.set(chunk, offset); offset += chunk.length;}
      return bytes;
    },
  };
}

function isImagePathForCodec(pathname) {
  return /\.(png|jpe?g|webp|exr|hdr|tiff?|bmp|gif)$/i.test(String(pathname || ''));
}

function defaultTextureJobs() {
  return Math.max(1, ((globalThis.navigator && navigator.hardwareConcurrency) || 4) - 1);
}

function estimateTextureRssMiB(imageCount, jobs, codec) {
  const count = Math.max(0, imageCount | 0);
  // Observed on large 512px PNG resize jobs:
  //   wasm: ~0.5 GiB, js/2: ~0.8 GiB, js/4: ~1.2 GiB.
  // The root flatten cost varies per scene, so keep the estimate deliberately
  // conservative rather than trying to predict exact texture dimensions.
  const base = 384 + Math.min(192, count * 0.4);
  if (codec === 'wasm') return base;
  return base + Math.max(1, jobs | 0) * 192;
}

function chooseBestTextureJobs(imageCount, budgetBytes) {
  if (!budgetBytes || budgetBytes <= 0) return 4;
  const budgetMiB = budgetBytes / 1048576;
  const candidates = [8, 6, 4, 3, 2, 1];
  for (const jobs of candidates) {
    if (estimateTextureRssMiB(imageCount, jobs, 'js') <= budgetMiB) {
      return jobs;
    }
  }
  return 0;
}

function chooseTextureCodec(o, keys = []) {
  const requested = String(o.textureCodec || 'auto').toLowerCase();
  const textureFormat = String(o.textureFormat || 'keep').toLowerCase();
  const imageCount = Array.isArray(keys) ? keys.filter(isImagePathForCodec).length : 0;
  const wantsResizeToPng = textureFormat === 'png' && (o.resize || 0) > 0;
  const budgetBytes = o.textureMemoryBudget || 0;

  if (requested === 'wasm') {
    return {
      codec: 'wasm',
      jobs: 0,
      estimatedRssMiB: estimateTextureRssMiB(imageCount, 0, 'wasm'),
      reason: 'wasm'
    };
  }
  if (requested === 'js') {
    const explicitJobs = o.textureJobs === 'best'
      ? chooseBestTextureJobs(imageCount, budgetBytes)
      : (o.textureJobs || defaultTextureJobs());
    return {
      codec: explicitJobs > 0 ? 'js' : 'wasm',
      jobs: explicitJobs,
      estimatedRssMiB: explicitJobs > 0
        ? estimateTextureRssMiB(imageCount, explicitJobs, 'js')
        : estimateTextureRssMiB(imageCount, 0, 'wasm'),
      reason: explicitJobs > 0
        ? `js${o.textureJobs === 'best' ? ' best-guess' : ''}: ${imageCount} image assets`
        : `budget too small for js; wasm fallback (${imageCount} image assets)`
    };
  }

  if (requested === 'best') {
    if (!wantsResizeToPng || imageCount < 64) {
      return {
        codec: 'wasm',
        jobs: 0,
        estimatedRssMiB: estimateTextureRssMiB(imageCount, 0, 'wasm'),
        reason: `best: low-memory wasm (${imageCount} image assets)`
      };
    }
    const jobs = chooseBestTextureJobs(imageCount, budgetBytes || 1024 * 1024 * 1024);
    if (jobs > 0) {
      return {
        codec: 'js',
        jobs,
        estimatedRssMiB: estimateTextureRssMiB(imageCount, jobs, 'js'),
        reason: `best: ${imageCount} image assets, png resize, budget ` +
          `${Math.round((budgetBytes || 1024 * 1024 * 1024) / 1048576)} MiB`
      };
    }
    return {
      codec: 'wasm',
      jobs: 0,
      estimatedRssMiB: estimateTextureRssMiB(imageCount, 0, 'wasm'),
      reason: `best: budget favors wasm (${imageCount} image assets)`
    };
  }

  if (requested !== 'auto') {
    return { codec: requested, jobs: 0, reason: requested };
  }

  if (wantsResizeToPng && imageCount >= 300) {
    const jobs = o.textureJobs === 'best'
      ? chooseBestTextureJobs(imageCount, budgetBytes || 1024 * 1024 * 1024)
      : budgetBytes > 0
      ? chooseBestTextureJobs(imageCount, budgetBytes)
      : (o.textureJobs || 4);
    if (jobs <= 0) {
      return {
        codec: 'wasm',
        jobs: 0,
        estimatedRssMiB: estimateTextureRssMiB(imageCount, 0, 'wasm'),
        reason: `auto: budget favors wasm (${imageCount} image assets)`
      };
    }
    return {
      codec: 'js',
      jobs,
      estimatedRssMiB: estimateTextureRssMiB(imageCount, jobs, 'js'),
      reason: `auto: ${imageCount} image assets, png resize` +
        (budgetBytes > 0 ? `, budget ${Math.round(budgetBytes / 1048576)} MiB` : '')
    };
  }

  return {
    codec: 'wasm',
    jobs: 0,
    estimatedRssMiB: estimateTextureRssMiB(imageCount, 0, 'wasm'),
    reason: `auto: low-memory wasm (${imageCount} image assets)`
  };
}

function validateTextureCodecSelection(o, selected) {
  if (selected.codec !== 'wasm' && selected.codec !== 'js') {
    console.error(`--texture-codec must be auto, best, wasm or js (got '${o.textureCodec}')`);
    process.exit(1);
  }
}

async function runStreamingConvert(native, o) {
  const log = o.verbose ? (m) => console.log(m) : () => {};
  const source = o.urlList ? urlListSource(o.urlList) : folderSource(o.input);
  if (!o.output) { console.error('--pipeline stream requires -o <output.usdz>'); process.exit(1); }

  let streamFd = null, streamBytes = 0;
  const ensureFd = () => { if (streamFd === null) streamFd = fs.openSync(o.output, 'w'); };
  const zipSink = {
    write: (chunk) => { ensureFd(); fs.writeSync(streamFd, chunk, 0, chunk.length, streamBytes); streamBytes += chunk.length; },
    patch: (pos, chunk) => { ensureFd(); fs.writeSync(streamFd, chunk, 0, chunk.length, pos); },
  };

  let texturePool = null;
  const selectedTextureCodec = chooseTextureCodec(o, source.keys || []);
  validateTextureCodecSelection(o, selectedTextureCodec);
  if (o.verbose && o.textureCodec === 'auto') {
    log(`texture codec: ${selectedTextureCodec.codec} (${selectedTextureCodec.reason}; ` +
      `estimated RSS ${Math.round(selectedTextureCodec.estimatedRssMiB || 0)} MiB)`);
  } else if (o.verbose && o.textureCodec === 'best') {
    log(`texture codec: ${selectedTextureCodec.codec} (${selectedTextureCodec.reason}; ` +
      `estimated RSS ${Math.round(selectedTextureCodec.estimatedRssMiB || 0)} MiB)`);
  }
  if (selectedTextureCodec.codec === 'js') {
    const { createNodeTextureProcessor } = await import('../src/texture-processor-node.mjs');
    texturePool = createNodeTextureProcessor({
      concurrency: selectedTextureCodec.jobs || 0,
      memoryBudgetBytes: o.textureMemoryBudget || 0,
    });
    log(`texture codec: js (${texturePool.concurrency} worker(s))`);
  }

  try {
    const { stats } = await convertSourceToUSDZStreaming(native, source, {
      rootPath: o.root || undefined,
      udimBake: o.udimBake, udimCrossTile: o.udimCrossTile,
      udimMaxTiles: o.udimMaxTiles, udimMaxAtlasSize: o.udimMaxAtlasSize,
      udimMemoryBudgetBytes: o.udimMemoryBudgetBytes, udimDensePadding: o.udimDensePadding,
      udimSubdivisionLevel: o.udimSubdivisionLevel,
      maxTextureSize: o.resize,
      resizeColorspace: o.resizeColorspace,
      reencode: o.reencode,
      textureFormat: o.textureFormat,
      jpegQuality: o.jpegQuality,
      pngEncoder: o.pngEncoder,
      maxUsdcMb: o.maxUsdcMb,
      maxMemMb: o.maxMemMb,
      wasmHeapLimitBytes: o.wasmHeapLimitBytes,
      pipeline: o.pipeline === 'stream-next' ? 'next' : undefined,
      streamWrite: o.streamWrite,
      optimizeMaterials: o.optimizeMaterials,
      materialAtlasSize: o.materialAtlasSize,
      materialAtlasTileSize: o.materialAtlasTileSize,
      materialAtlasPadding: o.materialAtlasPadding,
      materialAtlasMinGroupSize: o.materialAtlasMinGroupSize,
      optimizeGeometry: o.optimizeGeometry,
      includeUnusedTextures: o.includeUnusedTextures,
      variantSelections: o.variantSelections,
      meshMergeMaxInputFaces: o.meshMergeMaxInputFaces,
      meshMergeMaxInputPoints: o.meshMergeMaxInputPoints,
      meshMergeMaxAggregateFaces: o.meshMergeMaxAggregateFaces,
      meshMergeMinGroupSize: o.meshMergeMinGroupSize,
      textureProcessor: texturePool ? texturePool.processor : undefined,
      textureConcurrency: texturePool ? texturePool.concurrency : 4,
      zipSink,
      log,
    });
    if (streamFd !== null) fs.closeSync(streamFd);
    console.log(`Wrote ${o.output} (${streamBytes} bytes, streamed) — root: ${stats.rootPath}, ` +
      `textures: ${stats.textures}, resized: ${stats.resized}, reencoded: ${stats.reencoded}, ` +
      `audio: ${stats.audio}, other assets: ${stats.otherAssets}`);
  } finally {
    if (texturePool) texturePool.destroy();
  }
}

function readDirToMap(dir) {
  const map = new Map();
  const walk = (cur, prefix) => {
    for (const entry of fs.readdirSync(cur, { withFileTypes: true })) {
      const full = path.join(cur, entry.name);
      const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(full, rel);
      else if (entry.isFile()) map.set(rel, fs.readFileSync(full));
    }
  };
  walk(dir, '');
  return map;
}

function parseChannelSrc(s) {
  if (s.startsWith('const:')) return { const: parseInt(s.slice(6), 10) & 0xff };
  const m = s.match(/^(.*):([0-3])$/);
  if (m) return { file: m[1], channel: parseInt(m[2], 10) };
  return { file: s, channel: 0 };
}

async function runRepack(native, o) {
  const slots = ['R', 'G', 'B', 'A'];
  const args = { channels: o.packChannels, format: 'png', pngEncoder: o.pngEncoder };
  let inferred = 0;
  for (let i = 0; i < 4; i++) {
    const spec = o.pack[slots[i]];
    if (!spec) continue;
    inferred = Math.max(inferred, i + 1);
    const parsed = parseChannelSrc(spec);
    const key = slots[i].toLowerCase();
    if (parsed.file !== undefined) {
      if (!fs.existsSync(parsed.file)) { console.error('File not found: ' + parsed.file); process.exit(1); }
      args[key] = { data: fs.readFileSync(parsed.file), channel: parsed.channel };
    } else {
      args[key] = { const: parsed.const };
    }
  }
  if (!args.channels || args.channels < 1) args.channels = inferred || 4;

  const res = native.repackChannels(args);
  if (!res || !res.success) { console.error('Repack failed:', res && res.error); process.exit(1); }
  fs.writeFileSync(o.repack, new Uint8Array(res.data));
  console.log(`Wrote ${o.repack} (${res.width}x${res.height}x${res.channels}, ${res.data.length} bytes)`);
}

async function main() {
  const o = parseArgs();
  udimOptions(o);

  const native = await loadWasm(() => import(wasmGlue));
  if (o.verbose) console.log('WASM module loaded.');
  o.textureCodec = String(o.textureCodec || 'auto').toLowerCase();
  if (!['keep', 'png', 'jpeg', 'jpg', 'exr'].includes(String(o.textureFormat).toLowerCase())) {
    console.error('Invalid --texture-format. Expected keep, png, jpeg, or exr.');
    process.exit(1);
  }
  if (!['usdc', 'usda'].includes(String(o.rootLayerFormat).toLowerCase())) {
    console.error('Invalid --root-layer-format. Expected usdc or usda.');
    process.exit(1);
  }
  o.optimizeMaterials = String(o.optimizeMaterials || 'off').toLowerCase();
  if (!['off', 'none', 'dedupe', 'dedup', 'preview', 'previewsurface', 'usdpreviewsurface', 'atlas'].includes(o.optimizeMaterials)) {
    console.error('Invalid --optimize-materials. Expected off, dedupe, preview, or atlas.');
    process.exit(1);
  }
  if (o.optimizeMaterials === 'none') o.optimizeMaterials = 'off';
  if (o.optimizeMaterials === 'dedup') o.optimizeMaterials = 'dedupe';
  if (o.optimizeMaterials === 'previewsurface' || o.optimizeMaterials === 'usdpreviewsurface') {
    o.optimizeMaterials = 'preview';
  }
  if (o.materialAtlasSize < 1 || o.materialAtlasSize > 32768) {
    console.error('--material-atlas-size must be 1-32768.');
    process.exit(1);
  }
  if (o.materialAtlasTileSize < 1 || o.materialAtlasTileSize > o.materialAtlasSize) {
    console.error('--material-atlas-tile-size must be positive and <= --material-atlas-size.');
    process.exit(1);
  }
  if (o.materialAtlasPadding < 0 || o.materialAtlasPadding > 1024) {
    console.error('--material-atlas-padding must be 0-1024.');
    process.exit(1);
  }
  if (o.materialAtlasMinGroupSize < 1) {
    console.error('--material-atlas-min-group-size must be positive.');
    process.exit(1);
  }
  if (o.optimizeMaterials !== 'off' && o.flatten === false) {
    console.warn('WARN: material optimization requires flattened output; ignoring --no-flatten.');
    o.flatten = true;
  }
  o.optimizeGeometry = String(o.optimizeGeometry || 'off').toLowerCase();
  if (!['off', 'none', 'mergemeshes', 'merge', 'meshmerge'].includes(o.optimizeGeometry)) {
    console.error('Invalid --optimize-geometry. Expected off or mergeMeshes.');
    process.exit(1);
  }
  if (o.optimizeGeometry === 'none') o.optimizeGeometry = 'off';
  if (o.optimizeGeometry === 'merge' || o.optimizeGeometry === 'meshmerge') o.optimizeGeometry = 'mergemeshes';
  if (o.meshMergeMaxInputFaces < 1) {
    console.error('--mesh-merge-max-input-faces must be positive.');
    process.exit(1);
  }
  if (o.meshMergeMaxInputPoints < 1) {
    console.error('--mesh-merge-max-input-points must be positive.');
    process.exit(1);
  }
  if (o.meshMergeMaxAggregateFaces < 1) {
    console.error('--mesh-merge-max-aggregate-faces must be positive.');
    process.exit(1);
  }
  if (o.meshMergeMinGroupSize < 2) {
    console.error('--mesh-merge-min-group-size must be >= 2.');
    process.exit(1);
  }
  if (o.optimizeGeometry !== 'off' && o.flatten === false) {
    console.warn('WARN: geometry optimization requires flattened output; ignoring --no-flatten.');
    o.flatten = true;
  }

  if (o.repack) {
    await runRepack(native, o);
    return;
  }

  if (!o.input && !o.urlList) { console.error('Error: input directory or USD file (or --url-list) required.'); printHelp(); process.exit(1); }
  if (o.input && !fs.existsSync(o.input)) { console.error('Not found: ' + o.input); process.exit(1); }

  // --pipeline stream: lazy source (folder walk or URL list); USD layers only
  // go into memory for composition, textures are fetched -> processed ->
  // appended to the output zip one at a time. Lowest peak RSS for
  // texture-heavy scene folders. Requires flatten; root must be a bare USD.
  if (o.pipeline === 'stream' || o.pipeline === 'stream-next') {
    await runStreamingConvert(native, o);
    return;
  }

  // Build the asset map and determine the root USD's relative path.
  let assetMap, rootRel;
  const st = fs.statSync(o.input);
  if (st.isDirectory()) {
    assetMap = readDirToMap(o.input);
    rootRel = o.root || null;
  } else if (/\.usdz$/i.test(o.input)) {
    // A .usdz is self-contained; read only the archive and let the converter
    // unpack it to repack its internal textures (avoids slurping siblings).
    const base = path.basename(o.input);
    assetMap = new Map([[base, fs.readFileSync(o.input)]]);
    rootRel = o.root || base;
  } else {
    // Single file: read its whole directory so sibling textures resolve.
    const dir = path.dirname(o.input);
    assetMap = readDirToMap(dir);
    rootRel = o.root || path.basename(o.input);
  }

  const log = o.verbose ? (m) => console.log(m) : () => {};

  // When an explicit -o path is given, offer a file-backed zip sink so the
  // texture-streaming path can write the archive straight to disk (one entry at
  // a time) instead of building the whole USDZ in memory — the lowest-peak
  // legacy path for texture-heavy scenes. Opened lazily on first write, so if a
  // non-streaming path runs the sink is simply never used.
  // Patch-capable (seekable) sink: write() appends at the running offset; patch()
  // rewrites already-written bytes at an absolute position (used to backfill a
  // streamed root entry's local-header CRC/size). All writes use an explicit
  // position so append and patch never depend on the fd's implicit cursor.
  let streamFd = null, streamBytes = 0;
  const ensureFd = () => { if (streamFd === null) streamFd = fs.openSync(o.output, 'w'); };
  const zipSink = o.output
    ? {
        write: (chunk) => {
          ensureFd();
          fs.writeSync(streamFd, chunk, 0, chunk.length, streamBytes);
          streamBytes += chunk.length;
        },
        patch: (pos, chunk) => {
          ensureFd();
          fs.writeSync(streamFd, chunk, 0, chunk.length, pos);
        },
      }
    : undefined;

  // --texture-codec js: PNG decode/resize/encode on a worker_threads pool
  // (pngjs + node:zlib), parallel across textures. Non-PNG inputs fall back
  // to the WASM convertImage path inside convertFolderToUSDZ.
  let texturePool = null;
  const selectedTextureCodec = chooseTextureCodec(o, assetMap ? [...assetMap.keys()] : []);
  validateTextureCodecSelection(o, selectedTextureCodec);
  if (o.verbose && o.textureCodec === 'auto') {
    log(`texture codec: ${selectedTextureCodec.codec} (${selectedTextureCodec.reason}; ` +
      `estimated RSS ${Math.round(selectedTextureCodec.estimatedRssMiB || 0)} MiB)`);
  } else if (o.verbose && o.textureCodec === 'best') {
    log(`texture codec: ${selectedTextureCodec.codec} (${selectedTextureCodec.reason}; ` +
      `estimated RSS ${Math.round(selectedTextureCodec.estimatedRssMiB || 0)} MiB)`);
  }
  if (selectedTextureCodec.codec === 'js') {
    const { createNodeTextureProcessor } = await import('../src/texture-processor-node.mjs');
    texturePool = createNodeTextureProcessor({
      concurrency: selectedTextureCodec.jobs || 0,
      memoryBudgetBytes: o.textureMemoryBudget || 0,
    });
    log(`texture codec: js (${texturePool.concurrency} worker(s))`);
  }

  let convertResult;
  try {
    convertResult = await convertFolderToUSDZ(native, assetMap, {
      rootPath: rootRel,
      udimBake: o.udimBake, udimCrossTile: o.udimCrossTile,
      udimMaxTiles: o.udimMaxTiles, udimMaxAtlasSize: o.udimMaxAtlasSize,
      udimMemoryBudgetBytes: o.udimMemoryBudgetBytes, udimDensePadding: o.udimDensePadding,
      udimSubdivisionLevel: o.udimSubdivisionLevel,
      maxTextureSize: o.resize,
      resizeColorspace: o.resizeColorspace,
      targetTextureBytes: o.targetSize,
      fitStrategy: o.fitStrategy,
      fitMinTextureSize: o.fitMinSize,
      fitMinQuality: o.fitMinQuality,
      reencode: o.reencode,
      textureFormat: o.textureFormat,
      rootLayerFormat: o.rootLayerFormat,
      arkitCompatible: o.arkitCompatible,
      flatten: o.flatten,
      pngEncoder: o.pngEncoder,
      jpegQuality: o.jpegQuality,
      maxUsdcMb: o.maxUsdcMb,
      maxMemMb: o.maxMemMb,
      wasmHeapLimitBytes: o.wasmHeapLimitBytes,
      pipeline: o.pipeline,
      streamTextures: o.streamTextures,
      streamWrite: o.streamWrite,
      optimizeMaterials: o.optimizeMaterials,
      materialAtlasSize: o.materialAtlasSize,
      materialAtlasTileSize: o.materialAtlasTileSize,
      materialAtlasPadding: o.materialAtlasPadding,
      materialAtlasMinGroupSize: o.materialAtlasMinGroupSize,
      optimizeGeometry: o.optimizeGeometry,
      includeUnusedTextures: o.includeUnusedTextures,
      variantSelections: o.variantSelections,
      meshMergeMaxInputFaces: o.meshMergeMaxInputFaces,
      meshMergeMaxInputPoints: o.meshMergeMaxInputPoints,
      meshMergeMaxAggregateFaces: o.meshMergeMaxAggregateFaces,
      meshMergeMinGroupSize: o.meshMergeMinGroupSize,
      textureProcessor: texturePool ? texturePool.processor : undefined,
      textureConcurrency: texturePool ? texturePool.concurrency : 0,
      zipSink,
      log,
    });
  } finally {
    if (texturePool) texturePool.destroy();
  }
  const { usdz, streamedToSink, stats } = convertResult;

  const statLine = `root: ${stats.rootPath}, textures: ${stats.textures}, ` +
    `resized: ${stats.resized}, reencoded: ${stats.reencoded}, ` +
    `audio: ${stats.audio || 0}, other assets: ${stats.otherAssets || 0}`;

  if (streamedToSink && streamFd !== null) {
    fs.closeSync(streamFd);
    console.log(`Wrote ${o.output} (${streamBytes} bytes, streamed) — ${statLine}`);
  } else {
    if (streamFd !== null) fs.closeSync(streamFd);  // sink opened but path bailed
    const outPath = o.output ||
      `${stats.rootPath.split('/').pop().replace(/\.(usd|usda|usdc|usdz)$/i, '')}.usdz`;
    writeFileChunked(outPath, usdz);
    console.log(`Wrote ${outPath} (${usdz.length} bytes) — ${statLine}`);
  }
}

main().catch(err => { console.error('Error:', err); process.exit(1); });
