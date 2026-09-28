import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';

export function getBackendFromURL(params = new URLSearchParams(window.location.search), fallback = 'next') {
  const backend = params.get('backend');
  return (backend === 'next' || backend === 'auto' || backend === 'legacy')
    ? backend
    : fallback;
}

export const LOADER_BACKEND_CHOICES = ['legacy', 'next', 'auto'];

export function normalizeBackend(value, fallback = 'next') {
  return LOADER_BACKEND_CHOICES.includes(value) ? value : fallback;
}

/**
 * Rewrite ?backend= in the page URL and reload.
 *
 * A reload (rather than a live re-parse) is the correct backend switch: the
 * loader picks its WASM module at init() time (backend=next selects the
 * next-only module), so switching in place would keep running on the module
 * chosen for the previous backend.
 */
export function setBackendAndReload(backend) {
  const value = normalizeBackend(backend);
  const url = new URL(window.location.href);
  url.searchParams.set('backend', value);
  window.location.href = url.toString();
}

/**
 * Mount a small "Backend" <select> reflecting the current URL backend into
 * `container` (prepended). Switching rewrites ?backend= and reloads the page.
 * For lil-gui pages prefer a gui dropdown wired to setBackendAndReload().
 */
export function mountBackendSelector(container, options = {}) {
  if (!container) return null;
  const current = options.current || getBackendFromURL();
  const wrap = document.createElement('label');
  wrap.style.cssText = options.style ||
    'display:inline-flex;gap:6px;align-items:center;font-size:13px;margin:2px 0';
  const caption = document.createElement('span');
  caption.textContent = options.label || 'Backend';
  const select = document.createElement('select');
  for (const value of LOADER_BACKEND_CHOICES) {
    const opt = document.createElement('option');
    opt.value = value;
    opt.textContent = value;
    if (value === current) opt.selected = true;
    select.appendChild(opt);
  }
  select.addEventListener('change', () => setBackendAndReload(select.value));
  wrap.appendChild(caption);
  wrap.appendChild(select);
  if (options.append) container.appendChild(wrap);
  else container.prepend(wrap);
  return select;
}

/**
 * Read a USD asset URI from URL query parameters.
 *
 * Checks the shared aliases (?uri= / ?url= / ?src= / ?model=) plus any
 * page-specific extra aliases (e.g. ['usd']). Returns null when absent.
 */
export function getAssetUriFromURL(params = new URLSearchParams(window.location.search), extraKeys = []) {
  for (const key of ['uri', 'url', 'src', 'model', ...extraKeys]) {
    const value = params.get(key);
    if (value) return value;
  }
  return null;
}

/**
 * Friendly filename derived from a URI/URL path (query/hash stripped,
 * percent-decoded last path segment).
 */
export function basenameFromUri(uri, fallback = 'scene.usd') {
  const clean = String(uri || '').split(/[?#]/)[0];
  return decodeURIComponent(clean.slice(clean.lastIndexOf('/') + 1)) || fallback;
}

export function makeStaticNextParseOptions(options = {}) {
  const backend = options.backend || 'next';
  return {
    ...options,
    backend,
    materialDedup: false,
    mergeMeshes: false,
    mergeMeshesBakeTransform: false,
    flattenRenderTree: false
  };
}

/**
 * Apply skinning-related load options to a LightUSDLoader instance.
 */
export function applySkinningLoaderOptions(loader, options = {}) {
  if (!loader) return;

  const enableBoneReduction = !!options.enableBoneReduction;
  const targetBoneCount = Number.isFinite(options.targetBoneCount)
    ? options.targetBoneCount
    : 4;
  const roundBoneCount = !!options.roundBoneCount;
  const logger = options.logger || console;

  loader.setEnableBoneReduction(enableBoneReduction);
  loader.setRoundBoneCount(roundBoneCount);

  if (enableBoneReduction) {
    loader.setTargetBoneCount(targetBoneCount);
    logger.log(`Bone reduction enabled: ${targetBoneCount} bones per vertex`);
  }
}

/**
 * Create and initialize a LightUSDLoader with common app defaults.
 */
export async function createConfiguredLightUSDLoader(options = {}) {
  const initOptions = options.initOptions || {
    useZstdCompressedWasm: false,
    useMemory64: false,
    // next is the default product; pass backend: 'legacy' for the legacy
    // compatibility module during the transition period.
    backend: options.backend || 'next'
  };
  const skinningOptions = options.skinningOptions || {};

  const loader = new LightUSDLoader();
  await loader.init(initOptions);
  applySkinningLoaderOptions(loader, skinningOptions);
  return loader;
}

/**
 * Promise wrapper around LightUSDLoader.load().
 */
export async function loadUSDSceneFromURL(loader, url, options = {}) {
  return new Promise((resolve, reject) => {
    loader.load(url, resolve, null, reject, options);
  });
}

/**
 * Promise wrapper around LightUSDLoader.parse().
 */
export async function parseUSDSceneFromArrayBuffer(loader, arrayBuffer, filename, options = {}) {
  return new Promise((resolve, reject) => {
    loader.parse(new Uint8Array(arrayBuffer), filename, resolve, reject, options);
  });
}
