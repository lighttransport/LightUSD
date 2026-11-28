// SPDX-License-Identifier: Apache-2.0
// Copy-in bridge for the standalone Tydra render-conversion feature. The
// feature owns its own heap and C++ objects; only bytes cross the boundary.
(() => {
  const decoder = new TextDecoder();
  const kMaxInputBytes = 256 * 1024 * 1024;
  const copyIn = (bytes) => {
    if (!ArrayBuffer.isView(bytes))
      throw new TypeError('expected byte view');
    let source = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (source.length > kMaxInputBytes) throw new RangeError('input exceeds 256 MiB');
    if (source.buffer === Module.HEAPU8.buffer) source = source.slice();
    const ptr = Module['_lu_feature_alloc'](Math.max(source.length, 1));
    if (!ptr) throw new RangeError('input allocation failed');
    Module.HEAPU8.set(source, Number(ptr));
    return {ptr, length: source.length};
  };
  const copyOut = (ptr) => {
    if (!ptr) return null;
    const start = Number(ptr);
    const end = Module.HEAPU8.indexOf(0, start);
    if (end < start) throw new Error('invalid result');
    return decoder.decode(Module.HEAPU8.subarray(start, end));
  };
  const memoryMB = (options) => {
    const value = options?.maxMemoryMB == null ? 512 : Number(options.maxMemoryMB);
    if (!Number.isFinite(value) || value < 8 || value > 4096)
      throw new RangeError('maxMemoryMB out of range');
    return value | 0;
  };

  Module['convertToRenderScene'] = (bytes, options = {}) => {
    const input = copyIn(bytes);
    let output = 0;
    try {
      output = Module['_lu_tydra_convert'](input.ptr, input.length, memoryMB(options));
      if (!output) throw new Error('convertToRenderScene: conversion rejected input');
      return JSON.parse(copyOut(output));
    } finally {
      if (output) Module['_lu_feature_free'](output);
      Module['_lu_feature_free'](input.ptr);
    }
  };
})();
