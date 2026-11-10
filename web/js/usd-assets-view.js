// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-present Light Transport Entertainment, Inc.
//
// Browser view for visual USD asset-corpus captures. The page is intentionally
// URL-driven so Puppeteer can drive it without interacting with in-page controls.

import { StreamingUSDRenderer } from './streaming.js';
import { renderHttpUSD } from './http-asset-resolver.js';
import { getBackendFromURL } from './src/lightusd/LoaderConfigUtils.js';

const DEFAULT_URL = './assets/suzanne.usdc';
const DEFAULT_CAMERA = {
  az: Math.PI,
  el: 0.26,
  fov: 50,
  padding: 1.04,
};
const DEFAULT_CLEAR = [0.28, 0.28, 0.28, 1.0];

function status(msg) {
  const el = document.getElementById('status');
  if (el) el.textContent = msg;
}

function baseOf(url) {
  const i = url.lastIndexOf('/');
  return i >= 0 ? url.slice(0, i + 1) : './';
}

function basename(url) {
  const clean = String(url || '').split(/[?#]/)[0];
  return decodeURIComponent(clean.slice(clean.lastIndexOf('/') + 1)) || 'scene.usd';
}

function urlParam(params) {
  return params.get('uri') || params.get('url') || params.get('src') || params.get('model') || DEFAULT_URL;
}

function jsHeapBytes() {
  return performance.memory ? performance.memory.usedJSHeapSize : 0;
}

function parseJsonParam(params, name) {
  const v = params.get(name);
  if (!v) return null;
  try { return JSON.parse(v); } catch (_) { return null; }
}

function parseCsvNumbers(v) {
  if (!v) return null;
  const nums = v.split(',').map((x) => Number(x.trim()));
  return nums.every((x) => Number.isFinite(x)) ? nums : null;
}

function parseCamera(params) {
  const cam = { ...DEFAULT_CAMERA, ...(parseJsonParam(params, 'camera') || {}) };
  for (const k of ['az', 'el', 'dist', 'fov', 'padding', 'near', 'far']) {
    if (params.has(k)) {
      const n = Number(params.get(k));
      if (Number.isFinite(n)) cam[k] = n;
    }
  }
  const target = parseCsvNumbers(params.get('target'));
  if (target && target.length >= 3) cam.target = target.slice(0, 3);
  return cam;
}

function parseClear(params) {
  const c = parseCsvNumbers(params.get('clear'));
  return c && c.length >= 3 ? [c[0], c[1], c[2], c[3] ?? 1] : DEFAULT_CLEAR;
}

async function settleFrames(n = 3) {
  for (let i = 0; i < n; i++) {
    // eslint-disable-next-line no-await-in-loop
    await new Promise((resolve) => requestAnimationFrame(resolve));
  }
}

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

async function loadPreloads(params) {
  const raw = parseJsonParam(params, 'preload') || [];
  const out = [];
  for (const item of raw) {
    if (!item || !item.key || !item.url) continue;
    // eslint-disable-next-line no-await-in-loop
    const resp = await fetch(item.url, { cache: 'no-store' });
    if (!resp.ok && resp.status !== 206) continue;
    // eslint-disable-next-line no-await-in-loop
    out.push({ key: item.key, url: item.url, bytes: await resp.arrayBuffer() });
  }
  return out;
}

async function main() {
  const params = new URLSearchParams(location.search);
  if (params.get('ui') === '0') document.body.classList.add('hide-ui');

  const url = urlParam(params);
  const baseUrl = params.get('base') || baseOf(url);
  const filename = params.get('name') || basename(url);
  const label = params.get('label') || filename;
  const camera = parseCamera(params);
  const backend = getBackendFromURL(params, 'next');

  window.__usdAssetsViewer = {
    ready: false,
    error: null,
    stats: null,
    url,
    baseUrl,
    label,
    camera,
    backend,
  };

  const canvas = document.getElementById('gl');
  const renderer = new StreamingUSDRenderer(canvas);
  await renderer.init();
  renderer.setClearColor(parseClear(params));
  startFpsMeter((fps) => {
    if (window.__usdAssetsViewer?.stats) {
      window.__usdAssetsViewer.stats.fps = fps;
    }
  });

  try {
    const totalStart = performance.now();
    const jsStart = jsHeapBytes();
    status(`Fetching ${url}`);
    const fetchStart = performance.now();
    const resp = await fetch(url, { cache: 'no-store' });
    if (!resp.ok && resp.status !== 206) throw new Error(`HTTP ${resp.status} ${resp.statusText}`);
    const rootBytes = new Uint8Array(await resp.arrayBuffer());
    const fetchMs = performance.now() - fetchStart;
    const preloadedAssets = await loadPreloads(params);

    status(`Loading ${label}\nbackend=${backend}\n${baseUrl}`);
    const t0 = performance.now();
    const loaded = await renderHttpUSD({
      renderer,
      rootBytes,
      filename,
      baseUrl,
      label,
      backend,
      onStatus: (msg) => status(`${label}\n${msg}`),
      preloadedAssets,
    });
    const processMs = performance.now() - t0;

    const result = loaded.result;
    renderer.frameCamera(camera);
    await settleFrames(4);

    const stats = {
      ...renderer.getRenderStats(),
      label,
      filename,
      url,
      baseUrl,
      backend: loaded.backend,
      requestedBackend: loaded.requestedBackend,
      sourceBytes: rootBytes.byteLength,
      fetchedBytes: loaded.fetchedBytes,
      fetches: loaded.fetches,
      fetchLog: loaded.fetchLog,
      result,
      shadeLabel: loaded.shadeLabel,
      fetchMs: Math.round(fetchMs),
      processMs: Math.round(processMs),
      elapsedMs: Math.round(performance.now() - totalStart),
      jsHeapDelta: jsHeapBytes() - jsStart,
      fps: 0,
    };
    window.__usdAssetsViewer.ready = true;
    window.__usdAssetsViewer.stats = stats;
    window.__usdAssetsViewer.camera = stats.camera;
    status(`${label}: ${stats.meshes} meshes, ${result.textures} textures, ${result.materials} materials ` +
      `(${stats.backend}, ${stats.shadeLabel})\nfetch ${stats.fetchMs} ms, process ${stats.processMs} ms, total ${stats.elapsedMs} ms`);
  } catch (e) {
    const msg = e && e.message ? e.message : String(e);
    window.__usdAssetsViewer.error = msg;
    status(`Error: ${msg}`);
    console.error(e);
  }
}

main().catch((e) => {
  const msg = e && e.message ? e.message : String(e);
  window.__usdAssetsViewer = window.__usdAssetsViewer || {};
  window.__usdAssetsViewer.error = msg;
  status(`Init failed: ${msg}`);
  console.error(e);
});
