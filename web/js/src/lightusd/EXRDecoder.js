// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment, Inc.
//
// EXR Decoder with Three.js primary + LightUSD fallback
//
// Uses Three.js EXRLoader by default (faster, supports more compression types)
// Falls back to LightUSD for unsupported formats (deep EXR, tiled, etc.)

import { EXRLoader } from 'three/examples/jsm/loaders/EXRLoader.js';
import { HalfFloatType, FloatType } from 'three';

function withToHalfFloatRangeWarningSuppressed(fn) {
  const originalWarn = console.warn;
  console.warn = (...args) => {
    if (typeof args[0] === 'string' &&
        args[0].startsWith('THREE.DataUtils.toHalfFloat(): Value out of range.')) {
      return;
    }
    originalWarn.apply(console, args);
  };
  try {
    return fn();
  } finally {
    console.warn = originalWarn;
  }
}

/**
 * EXR decode result
 * @typedef {Object} EXRDecodeResult
 * @property {boolean} success - Whether decoding succeeded
 * @property {Float32Array|Uint16Array} [data] - Pixel data (RGBA)
 * @property {number} [width] - Image width
 * @property {number} [height] - Image height
 * @property {number} [channels] - Number of channels (always 4 for RGBA)
 * @property {string} [format] - Pixel format ('float32' or 'float16')
 * @property {string} [decoder] - Which decoder was used ('threejs' or 'lightusd')
 * @property {string} [error] - Error message if failed
 */

/**
 * EXR Decoder options
 * @typedef {Object} EXRDecodeOptions
 * @property {string} [outputFormat='float16'] - Output format: 'float32' or 'float16'
 * @property {boolean} [preferThreeJS=true] - Whether to try Three.js first
 * @property {boolean} [verbose=false] - Log decoder selection
 */

/**
 * Decode EXR image data
 *
 * @param {Uint8Array|ArrayBuffer} data - EXR file data
 * @param {Object} [lightusd] - LightUSD WASM module (required for fallback)
 * @param {EXRDecodeOptions} [options] - Decode options
 * @returns {EXRDecodeResult} Decode result
 */
export function decodeEXR(data, lightusd = null, options = {}) {
  const {
    outputFormat = 'float16',
    preferThreeJS = true,
    verbose = false,
  } = options;

  // Ensure we have ArrayBuffer
  const buffer = data instanceof ArrayBuffer
    ? data
    : data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);

  // Try Three.js EXRLoader first (if preferred)
  if (preferThreeJS) {
    try {
      const result = decodeWithThreeJS(buffer, outputFormat);
      if (result.success) {
        if (verbose) console.log('EXR decoded with Three.js EXRLoader');
        return result;
      }
    } catch (err) {
      if (verbose) console.log('Three.js EXRLoader failed:', err.message);
    }
  }

  // Fallback to LightUSD
  if (lightusd && typeof lightusd.decodeEXR === 'function') {
    try {
      const result = decodeWithLightUSD(buffer, lightusd, outputFormat);
      if (result.success) {
        if (verbose) console.log('EXR decoded with LightUSD (fallback)');
        return result;
      } else if (verbose) {
        console.log('LightUSD decodeEXR failed:', result.error);
      }
    } catch (err) {
      if (verbose) console.log('LightUSD decodeEXR error:', err.message);
    }
  }

  // Try Three.js again if we didn't prefer it initially
  if (!preferThreeJS) {
    try {
      const result = decodeWithThreeJS(buffer, outputFormat);
      if (result.success) {
        if (verbose) console.log('EXR decoded with Three.js EXRLoader');
        return result;
      }
    } catch (err) {
      if (verbose) console.log('Three.js EXRLoader failed:', err.message);
    }
  }

  return {
    success: false,
    error: 'Failed to decode EXR with both Three.js and LightUSD',
    decoder: 'none',
  };
}

/**
 * Decode EXR using Three.js EXRLoader
 * @param {ArrayBuffer} buffer - EXR data
 * @param {string} outputFormat - 'float32' or 'float16'
 * @returns {EXRDecodeResult}
 */
function decodeWithThreeJS(buffer, outputFormat) {
  const loader = new EXRLoader();

  // Set output type based on requested format
  loader.setDataType(outputFormat === 'float32' ? FloatType : HalfFloatType);

  try {
    const texture = outputFormat === 'float32'
      ? loader.parse(buffer)
      : withToHalfFloatRangeWarningSuppressed(() => loader.parse(buffer));

    if (!texture || !texture.data) {
      return {
        success: false,
        error: 'Three.js EXRLoader returned empty result',
        decoder: 'threejs',
      };
    }

    // Determine actual format from texture type
    const isFloat16 = texture.type === HalfFloatType;
    const actualFormat = isFloat16 ? 'float16' : 'float32';

    return {
      success: true,
      data: texture.data,
      width: texture.width,
      height: texture.height,
      channels: 4,
      format: actualFormat,
      decoder: 'threejs',
    };
  } catch (err) {
    return {
      success: false,
      error: err.message || 'Unknown Three.js EXRLoader error',
      decoder: 'threejs',
    };
  }
}

/**
 * Decode EXR using LightUSD
 * @param {ArrayBuffer} buffer - EXR data
 * @param {Object} lightusd - LightUSD WASM module
 * @param {string} outputFormat - 'float32' or 'float16'
 * @returns {EXRDecodeResult}
 */
function decodeWithLightUSD(buffer, lightusd, outputFormat) {
  const uint8Array = new Uint8Array(buffer);
  const result = lightusd.decodeEXR(uint8Array, outputFormat);

  if (!result.success) {
    return {
      success: false,
      error: result.error || 'LightUSD decodeEXR failed',
      decoder: 'lightusd',
    };
  }

  return {
    success: true,
    data: result.data,
    width: result.width,
    height: result.height,
    channels: result.channels,
    format: result.pixelFormat || outputFormat,
    decoder: 'lightusd',
  };
}

/**
 * Check which decoder can handle the given EXR data
 * @param {Uint8Array|ArrayBuffer} data - EXR file data
 * @param {Object} [lightusd] - LightUSD WASM module
 * @returns {{threejs: boolean, lightusd: boolean}} Decoder support
 */
export function checkEXRSupport(data, lightusd = null) {
  const buffer = data instanceof ArrayBuffer
    ? data
    : data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);

  const support = {
    threejs: false,
    lightusd: false,
  };

  // Check Three.js
  try {
    const loader = new EXRLoader();
    const texture = loader.parse(buffer);
    support.threejs = !!(texture && texture.data);
  } catch (err) {
    support.threejs = false;
  }

  // Check LightUSD
  if (lightusd && typeof lightusd.decodeEXR === 'function') {
    try {
      const uint8Array = new Uint8Array(buffer);
      const result = lightusd.decodeEXR(uint8Array, 'float32');
      support.lightusd = result.success;
    } catch (err) {
      support.lightusd = false;
    }
  }

  return support;
}

/**
 * Async version of decodeEXR for use with dynamic module loading
 * @param {Uint8Array|ArrayBuffer} data - EXR file data
 * @param {EXRDecodeOptions} [options] - Decode options
 * @returns {Promise<EXRDecodeResult>}
 */
export async function decodeEXRAsync(data, options = {}) {
  // Try Three.js first (no async needed)
  const buffer = data instanceof ArrayBuffer
    ? data
    : data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);

  try {
    const result = decodeWithThreeJS(buffer, options.outputFormat || 'float16');
    if (result.success) {
      return result;
    }
  } catch (err) {
    // Fall through to LightUSD
  }

  // Load LightUSD dynamically for fallback
  try {
    // The WASM glue is a generated, optional artifact. Keep Vite from trying to
    // resolve it while transforming demos that only use the Three.js decoder.
    // The browser resolves this URL only if the fallback is actually needed.
    const lightusdModuleUrl = new URL('./lightusd_exr.js', import.meta.url).href;
    const createLightUSD = (await import(/* @vite-ignore */ lightusdModuleUrl)).default;
    const lightusd = await createLightUSD();
    return decodeEXR(data, lightusd, options);
  } catch (err) {
    return {
      success: false,
      error: `Failed to load LightUSD for fallback: ${err.message}`,
      decoder: 'none',
    };
  }
}

export default {
  decodeEXR,
  decodeEXRAsync,
  checkEXRSupport,
};
