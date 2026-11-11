import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';

// Compatibility entry points for old demo URLs/imports. Only core is shipped.
export function getBackendFromURL(_params, _fallback = 'core') { return 'core'; }
export const LOADER_BACKEND_CHOICES = ['core'];
export function normalizeBackend(_value, _fallback = 'core') { return 'core'; }
export function setBackendAndReload(_backend) {
  const url = new URL(window.location.href);
  url.searchParams.set('backend','core');
  window.location.href = url.toString();
}
// Retain the helper for demo imports without presenting an unavailable choice.
export function mountBackendSelector(_container, _options = {}) { return null; }

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

export function makeStaticParseOptions(options = {}) {
  const backend = options.backend || 'core';
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
    // core is the default product; pass backend: 'legacy' for the legacy
    // compatibility module during the transition period.
    backend: options.backend || 'core'
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
