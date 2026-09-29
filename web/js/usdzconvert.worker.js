import {
  loadWasm,
  isImageName,
  isUsdName,
  convertFolderToUSDZ,
  convertSourceToUSDZStreaming,
} from './src/usdzconvert.js';
import { createBrowserTextureProcessor } from './src/texture-processor-browser.mjs';

let native = null;
let lastDebugPostMs = 0;

function post(type, payload = {}, transfer) {
  self.postMessage({ type, ...payload }, transfer || []);
}

function log(message) {
  post('log', { message });
}

function progress(info) {
  post('progress', { info });
}

async function ensureWasm(pipeline) {
  if (native) return native;
  progress({ stage: 'wasm', current: 0, total: 1, message: 'Loading converter module' });
  const importer = pipeline === 'next-only'
    ? () => import('./src/lightusd/lightusd_next.js')
    : () => import('./src/lightusd/lightusd.js');
  native = await loadWasm(importer, {
    onLightUSDDebug(event) {
      const now = performance.now();
      if (now - lastDebugPostMs < 120) return;
      lastDebugPostMs = now;
      const phase = event && event.phase ? String(event.phase) : 'native';
      const detail = event && event.detail ? String(event.detail) : '';
      progress({
        stage: 'flatten',
        message: detail ? `${phase}: ${detail}` : phase,
      });
    },
  });
  progress({ stage: 'wasm', current: 1, total: 1, message: 'Converter module ready' });
  return native;
}

function workerTextureConcurrency(requested) {
  const cores = self.navigator && self.navigator.hardwareConcurrency
    ? self.navigator.hardwareConcurrency : 4;
  const guessed = Math.max(1, Math.min(8, cores - 1 || 1));
  return Math.max(1, Math.min(8, requested || guessed));
}

function maybeAttachTextureProcessor(opts, needsTextureWork, colorspaceAware, textureConcurrency) {
  if (!needsTextureWork || colorspaceAware) return null;
  if (typeof OffscreenCanvas === 'undefined' ||
      typeof OffscreenCanvas.prototype.convertToBlob !== 'function') {
    log('Worker texture codec: OffscreenCanvas unavailable; using WASM image path.');
    return null;
  }
  const pool = createBrowserTextureProcessor({
    concurrency: workerTextureConcurrency(textureConcurrency),
  });
  opts.textureProcessor = pool.processor;
  opts.textureConcurrency = pool.concurrency;
  log(`Worker texture codec: OffscreenCanvas (${pool.concurrency} concurrent job(s))`);
  return pool;
}

async function readUploadedFiles(files) {
  const assetMap = new Map();
  let readCount = 0;
  let readBytes = 0;
  progress({ stage: 'preparing', current: 0, total: files.length, message: 'Reading selected files' });
  for (const entry of files) {
    const bytes = new Uint8Array(await entry.file.arrayBuffer());
    assetMap.set(entry.path, bytes);
    readCount++;
    readBytes += bytes.length;
    progress({
      stage: 'preparing',
      current: readCount,
      total: files.length,
      message: `${entry.path} (${(readBytes / 1048576).toFixed(1)} MiB read)`,
    });
  }
  return assetMap;
}

function streamingSourceFromFiles(files) {
  const fileByPath = new Map(files.map(({ path, file }) => [path, file]));
  const keys = [...fileByPath.keys()];
  const totals = {
    images: keys.filter(isImageName).length,
    usd: keys.filter((key) => isUsdName(key) && !/\.usdz$/i.test(key)).length,
    other: keys.filter((key) => !isImageName(key) && !isUsdName(key)).length,
  };
  const counts = { images: 0, usd: 0, other: 0 };
  let readBytes = 0;
  return {
    keys,
    fetch: async (key) => {
      const file = fileByPath.get(key);
      if (!file) throw new Error(`Missing uploaded file: ${key}`);
      const stage = isImageName(key) ? 'textures' : isUsdName(key) ? 'layers' : 'assets';
      const bucket = isImageName(key) ? 'images' : isUsdName(key) ? 'usd' : 'other';
      progress({
        stage,
        current: counts[bucket],
        total: totals[bucket],
        message: `Reading ${key}`,
        path: key,
      });
      const bytes = new Uint8Array(await file.arrayBuffer());
      counts[bucket]++;
      readBytes += bytes.length;
      progress({
        stage,
        current: counts[bucket],
        total: totals[bucket],
        message: `${key} (${(readBytes / 1048576).toFixed(1)} MiB read)`,
        path: key,
      });
      return bytes;
    },
  };
}

async function runConversion(data) {
  await ensureWasm(data.opts && data.opts.pipeline);
  const opts = {
    ...data.opts,
    log,
    progress,
  };
  const texturePool = maybeAttachTextureProcessor(
    opts,
    !!data.needsTextureWork,
    !!data.colorspaceAware,
    data.textureConcurrency);

  progress({ stage: 'preparing', current: 1, total: 1, message: opts.rootPath });
  let result;
  if (opts.pipeline === 'stream' || opts.pipeline === 'stream-next') {
    if ((opts.targetTextureBytes || 0) > 0) {
      throw new Error('Streaming conversion does not support target total texture size yet.');
    }
    result = await convertSourceToUSDZStreaming(native, streamingSourceFromFiles(data.files), {
      ...opts,
      pipeline: opts.pipeline === 'stream-next' ? 'next' : undefined,
      nextPreloadUsdLayers: opts.pipeline === 'stream-next',
    });
  } else {
    result = await convertFolderToUSDZ(native, await readUploadedFiles(data.files), opts);
  }

  const textureStats = texturePool ? texturePool.stats() : null;
  native = null;
  post('complete', {
    usdz: result.usdz,
    stats: result.stats,
    textureStats,
  }, [result.usdz.buffer]);
}

self.onmessage = (event) => {
  const data = event.data || {};
  if (data.type === 'dispose') {
    native = null;
    self.close();
    return;
  }
  if (data.type !== 'convert') return;
  runConversion(data).catch((err) => {
    native = null;
    post('error', {
      message: err && err.message ? err.message : String(err),
      stack: err && err.stack ? err.stack : '',
    });
  });
};
