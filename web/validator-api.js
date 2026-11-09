// SPDX-License-Identifier: Apache-2.0
(() => {
  const encoder = new TextEncoder(), decoder = new TextDecoder();
  let busy = false;
  const withBytes = (bytes, call) => {
    if (bytes.buffer === Module.HEAPU8.buffer) bytes = bytes.slice();
    const ptr = Module['_lu_feature_alloc'](Math.max(bytes.length, 1));
    if (!ptr) throw new RangeError('validator allocation failed');
    try { Module.HEAPU8.set(bytes, Number(ptr)); return call(ptr); }
    finally { Module['_lu_feature_free'](ptr); }
  };
  Module['runValidation'] = (bytes, filename, options, checker, snapshot = null) => {
    if (busy) throw new Error('validator is already running');
    if (!ArrayBuffer.isView(bytes))
      throw new TypeError('validation: expected byte view');
    if (typeof filename !== 'string' || typeof options !== 'string')
      throw new TypeError('validation: expected filename and options strings');
    const source = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const name = encoder.encode(filename), config = encoder.encode(options);
    if (source.length > 0x40000000 || name.length > 65536 || config.length > 0x1000000)
      throw new RangeError('validation input exceeds limits');
    busy = true;
    try {
      if (snapshot) {
        const meta = encoder.encode(JSON.stringify(snapshot.metadata));
        let charge = meta.length;
        for (const asset of snapshot.metadata.assets) charge += asset.size + encoder.encode(asset.name).length;
        if (charge > 0x20000000) throw new RangeError('validation asset snapshot exceeds 512 MiB');
        if (withBytes(meta, p => Module['_lu_validator_begin'](p, meta.length)) !== 0)
          throw new RangeError('validation snapshot rejected');
        for (const asset of snapshot.metadata.assets) {
          const key = encoder.encode(asset.name), data = snapshot.read(asset.name);
          if (!data || data.length !== asset.size) throw new Error('validation asset changed');
          const status = withBytes(key, k => withBytes(data, p => Module['_lu_validator_asset'](k, key.length, p, data.length)));
          if (status !== 0) throw new RangeError('validation asset budget exceeded');
        }
      }
      return withBytes(source, d => withBytes(name, n => withBytes(config, c => {
        const result = Module['_lu_validator_check'](d, source.length, n, name.length, c, config.length, checker ? 1 : 0);
        if (!result) throw new RangeError('validation failed to allocate result');
        try {
          const start = Number(result), end = Module.HEAPU8.indexOf(0, start);
          if (end < start) throw new Error('invalid validation result');
          return decoder.decode(Module.HEAPU8.subarray(start, end));
        } finally { Module['_lu_feature_free'](result); }
      })));
    } finally { Module['_lu_validator_clear'](); busy = false; }
  };
  Module['checkUSD'] = (bytes, filename, options = {}) =>
    JSON.parse(Module['runValidation'](bytes, filename, JSON.stringify(options), true));
  Module['validateFromBinary'] = (bytes, filename, options = '{}') =>
    Module['runValidation'](bytes, filename, options, false);
})();
