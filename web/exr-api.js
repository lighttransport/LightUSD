// SPDX-License-Identifier: Apache-2.0
(() => {
  let busy = false;
  Module['codec'] = (op, bytes, width = 0, height = 0, channels = 0, bits = 0, budget = 0x20000000) => {
    if (busy) throw new Error('EXR codec is already running');
    if (!ArrayBuffer.isView(bytes)) throw new TypeError('EXR requires a byte view');
    let source = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (!Number.isSafeInteger(budget) || budget <= 0 || budget > 0x20000000 || source.length > 0x10000000)
      throw new RangeError('EXR input or budget exceeds limits');
    for (const value of [op, width, height, channels, bits])
      if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw new TypeError('Invalid EXR dimensions');
    if (source.buffer === Module.HEAPU8.buffer) source = source.slice();
    // The transfer and owned result copy are part of the operation budget.
    const remaining = Math.floor((budget - source.length) / 2);
    if (remaining <= 0) throw new RangeError('EXR transfer budget exceeded');
    busy = true;
    let ptr;
    try {
      ptr = Module['_lu_feature_alloc'](Math.max(1, source.length));
      if (!ptr) throw new RangeError('EXR input allocation failed');
      Module.HEAPU8.set(source, Number(ptr));
      const status = Module['_lu_exr_run'](op, ptr, source.length, width, height, channels, bits, remaining);
      if (status !== 0) throw new Error(`EXR codec failed (${status})`);
      const meta = field => Module['_lu_exr_meta'](field);
      const size = meta(4), offset = Number(Module['_lu_exr_data']());
      if (size > remaining || (size && (!offset || offset > Module.HEAPU8.length - size)))
        throw new RangeError('Invalid EXR result');
      return {width: meta(0), height: meta(1), channels: meta(2), bits: meta(3),
        data: Module.HEAPU8.slice(offset, offset + size)};
    } finally {
      Module['_lu_exr_clear']();
      if (ptr) Module['_lu_feature_free'](ptr);
      busy = false;
    }
  };
  Module['info'] = bytes => Module['codec'](0, bytes);
  Module['decode'] = bytes => Module['codec'](1, bytes);
  Module['decodeEXR'] = (bytes, format = 'float32') => {
    if (format !== 'float32' && format !== 'float16') throw new TypeError('Invalid EXR output format');
    try {
      const result = Module['codec'](format === 'float16' ? 3 : 4, bytes);
      return {...result, success: true, pixelFormat: format,
        data: format === 'float16' ? new Uint16Array(result.data.buffer)
          : new Float32Array(result.data.buffer)};
    } catch (error) { return {success: false, error: error.message}; }
  };
  Module['encode'] = (bytes, width, height, channels, bits = 8) =>
    Module['codec'](2, bytes, width, height, channels, bits).data;
})();
