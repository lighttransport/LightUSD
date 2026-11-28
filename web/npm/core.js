// SPDX-License-Identifier: Apache-2.0
// Three.js-free entry point.
//
// `lightusd` (index.js) also exposes the three.js-based loaders, material
// converters and scene helpers. Import `lightusd/core` when you only need the
// WASM core module and its independently linked features: nothing in this
// module reaches for `three`, so it can be used in Node, workers and
// non-rendering tools without installing a renderer.
export { default as createLightUSD } from './lightusd.js';
export { default as createLightUSD64 } from './lightusd_64.js';

// The feature names accepted by module.loadFeature(). Keep in sync with
// web/core-api.js.
export const FEATURE_NAMES = Object.freeze([
  'geometry', 'raytrace', 'validator', 'exr', 'tydra'
]);
