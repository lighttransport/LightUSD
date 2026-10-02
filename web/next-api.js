// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Internal post-JS bridge for next's counted C dispatch. Object IDs are
// uint32 on both wasm32 and memory64; no native pointer is exposed to JS.
{
  const live = new WeakMap();
  function defineClass(name, kind) {
    const Native = class {
      constructor() {
        const handle = Module['_lightusd_next_create'](kind) >>> 0;
        if (!handle) throw new Error('Unable to allocate LightUSD object');
        live.set(this, {handle, busy: 0, kind, progressCallback: null,
          sourceURI: '',
          progress: {progress: 0, stage: 'idle', currentOperation: '',
            cancelRequested: false, errorMessage: '', bytesProcessed: 0,
            totalBytes: 0, percentage: 0, meshesProcessed: 0, meshesTotal: 0,
            currentMeshName: '', materialsProcessed: 0, materialsTotal: 0,
            tydraStage: ''}, parsing: false, cancelRequested: false,
          wasCancelled: false,
          streamBuffers: new Map()});
      }
      delete() {
        const state = live.get(this);
        if (!state || !state.handle) throw new TypeError('LightUSD object already deleted');
        if (state.busy) throw new TypeError('Cannot delete an active LightUSD object');
        Module['_lightusd_next_destroy'](state.handle);
        state.handle = 0;
        state.assetStore = null;
        state.streamBuffers?.clear();
      }
      isDeleted() { return !live.get(this)?.handle; }
    };
    Object.defineProperty(Native, 'name', {value: name});
    Module[name] = Native;
  }
  const diffEncoder = new TextEncoder();
  Module['usddiff'] = options => {
    if (options == null) return {success: false, error: 'usddiff: missing options'};
    if (options.left == null || options.right == null) {
      return {success: false, error: "usddiff: 'left' and 'right' are required"};
    }
    const view = value => {
      if (value == null) return new Uint8Array(0);
      if (!ArrayBuffer.isView(value)) throw new TypeError('usddiff: expected byte views');
      let bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      if (bytes.length > 0x40000000) throw new RangeError('usddiff: input exceeds 1 GiB');
      if (bytes.buffer === Module.HEAPU8.buffer) bytes = bytes.slice();
      return bytes;
    };
    const left = view(options.left.data), right = view(options.right.data);
    const leftName = diffEncoder.encode(options.left.name ?? 'left');
    const rightName = diffEncoder.encode(options.right.name ?? 'right');
    const format = options.format ?? 'text';
    const formatId = format === 'text' ? 0 : format === 'json' ? 1
      : format === 'both' ? 2 : 3;
    const total = 24 + left.length + leftName.length + right.length + rightName.length;
    if (total > 0xffffffff) throw new RangeError('usddiff: input too large');
    let input = 0, output = 0;
    try {
      input = Module['_lightusd_next_alloc'](total);
      if (!input) throw new RangeError('usddiff: allocation failed');
      const base = Number(input);
      const optionsView = new DataView(Module.HEAPU8.buffer, base, 24);
      optionsView.setUint32(0, 24, true);
      optionsView.setInt32(4, options.ulps == null ? -1 : Number(options.ulps) | 0, true);
      optionsView.setFloat64(8, options.eps == null ? -1 : Number(options.eps), true);
      optionsView.setUint8(16, options.compareMetadata == null ? 1 : !!options.compareMetadata);
      optionsView.setUint8(17, options.fuzzyAssetPaths == null ? 1 : !!options.fuzzyAssetPaths);
      optionsView.setUint8(18, formatId);
      let offset = 24;
      const offsets = [];
      for (const bytes of [left, leftName, right, rightName]) {
        offsets.push(offset);
        Module.HEAPU8.set(bytes, base + offset);
        offset += bytes.length;
      }
      const pointer = offset => typeof input === 'bigint' ? input + BigInt(offset)
                                                          : input + offset;
      const call = p => Module['_lightusd_next_diff_json'](
        p(0), p(offsets[0]), left.length, p(offsets[1]), leftName.length,
        p(offsets[2]), right.length, p(offsets[3]), rightName.length);
      try { output = call(pointer); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        output = call(offset => BigInt(pointer(offset)));
      }
      if (!output) throw new RangeError('usddiff: result allocation failed');
      return JSON.parse(UTF8ToString(Number(output)));
    } finally {
      if (output) Module['_lightusd_next_free'](output);
      if (input) Module['_lightusd_next_free'](input);
    }
  };
  const validateEncoder = new TextEncoder();
  const validateBytes = (bytes, filename, options, checker = false, assetHandle = 0) => {
    if (!ArrayBuffer.isView(bytes)) throw new TypeError('validateFromBinary: expected byte view');
    if (typeof filename !== 'string' || typeof options !== 'string') {
      throw new TypeError('validateFromBinary: expected filename and options strings');
    }
    let source = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (source.length > 0x40000000) throw new RangeError('validateFromBinary: input exceeds 1 GiB');
    if (source.buffer === Module.HEAPU8.buffer) source = source.slice();
    const name = validateEncoder.encode(filename);
    if (checker && name.length > 65536) throw new RangeError('checkUSD: filename exceeds 64 KiB');
    const config = validateEncoder.encode(options);
    if (checker && config.length > 0x1000000) throw new RangeError('checkUSD: options exceed 16 MiB');
    const total = source.length + name.length + config.length;
    if (total > 0xffffffff) throw new RangeError('validateFromBinary: input too large');
    let input = 0, output = 0;
    try {
      input = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!input) throw new RangeError('validateFromBinary: allocation failed');
      const base = Number(input);
      Module.HEAPU8.set(source, base);
      Module.HEAPU8.set(name, base + source.length);
      Module.HEAPU8.set(config, base + source.length + name.length);
      const pointer = offset => typeof input === 'bigint' ? input + BigInt(offset)
                                                          : input + offset;
      const call = (data, namePtr, configPtr) => checker
        ? Module['_lightusd_next_check_json'](data, source.length, namePtr, name.length, configPtr, config.length, assetHandle)
        : Module['_lightusd_next_validate_json'](data, source.length, namePtr, name.length, configPtr, config.length);
      try { output = call(input, pointer(source.length), pointer(source.length + name.length)); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        output = call(BigInt(input), BigInt(pointer(source.length)),
          BigInt(pointer(source.length + name.length)));
      }
      if (!output) throw new RangeError('validateFromBinary: validation allocation failed');
      return UTF8ToString(Number(output));
    } finally {
      if (output) Module['_lightusd_next_free'](output);
      if (input) Module['_lightusd_next_free'](input);
    }
  };
  Module['validateFromBinary'] = (bytes, filename, options) => validateBytes(bytes, filename, options);
  // Standalone lusdchecker engine. No implicit filesystem/network access.
  Module['checkUSD'] = (bytes, filename, options = {}, assetStore = null) => {
    if (!options || typeof options !== 'object' || Array.isArray(options))
      throw new TypeError('checkUSD: options must be an object');
    const store = assetStore === null ? null : live.get(assetStore);
    if (assetStore !== null && (!store?.handle || store.kind !== 5))
      throw new TypeError('checkUSD: assetStore must be a live NextAssetStore');
    return JSON.parse(validateBytes(bytes, filename, JSON.stringify(options), true, store?.handle || 0));
  };
  defineClass('NextUSDZConverterNative', 1);
  defineClass('NextAssetStore', 5);
  defineClass('LayerDocument', 6);

  const layerEncoder = new TextEncoder();
  const layerDecoder = new TextDecoder();
  const layerState = self => {
    const state = live.get(self);
    if (!state?.handle || state.kind !== 6) throw new TypeError('Invalid LayerDocument receiver');
    return state;
  };
  const layerCall = (symbol, args, pointerIndexes = []) => {
    try { return Module[symbol](...args); }
    catch (error) {
      if (!(error instanceof TypeError) || !pointerIndexes.length) throw error;
      const wide = args.slice();
      for (const index of pointerIndexes) wide[index] = BigInt(wide[index]);
      return Module[symbol](...wide);
    }
  };
  const layerError = state => {
    const size = Module['_lightusd_next_layer_error_size'](state.handle);
    if (size <= 0) return '';
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('LayerDocument: error allocation failed');
      const copied = layerCall('_lightusd_next_layer_error_copy',
        [state.handle, ptr, size], [1]);
      if (copied !== size) throw new RangeError('LayerDocument: error copy failed');
      return layerDecoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally { if (ptr) Module['_lightusd_next_free'](ptr); }
  };
  const layerBytes = (value, label) => {
    if (!ArrayBuffer.isView(value)) throw new TypeError(label + ': expected byte view');
    if (value.byteLength > 0x20000000) throw new RangeError(label + ': input exceeds 512 MiB');
    if (value.buffer === Module.HEAPU8.buffer) {
      return {heapOffset: value.byteOffset, length: value.byteLength, bytes: null};
    }
    return {heapOffset: -1, length: value.byteLength,
      bytes: new Uint8Array(value.buffer, value.byteOffset, value.byteLength)};
  };
  const layerStringsCall = (self, symbol, values, suffix = []) => {
    const state = layerState(self);
    if (values.some(value => typeof value !== 'string' || value.includes('\0'))) {
      throw new TypeError(symbol + ': expected NUL-free strings');
    }
    const encoded = values.map(value => layerEncoder.encode(value));
    const total = encoded.reduce((sum, bytes) => sum + bytes.length, 0);
    if (total > 0x20000000) throw new RangeError(symbol + ': inputs exceed 512 MiB');
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError(symbol + ': allocation failed');
      let offset = 0;
      const args = [state.handle], pointerIndexes = [];
      for (const bytes of encoded) {
        pointerIndexes.push(args.length);
        args.push(typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset);
        args.push(bytes.length);
        Module.HEAPU8.set(bytes, Number(ptr) + offset);
        offset += bytes.length;
      }
      args.push(...suffix);
      const status = layerCall(symbol, args, pointerIndexes);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  };
  Object.defineProperty(Module.LayerDocument.prototype, 'load', {value: function(value) {
    const state = layerState(this);
    if (arguments.length !== 1) throw new TypeError('load: expected one byte view');
    const input = layerBytes(value, 'load');
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(input.length, 1));
      if (!ptr) throw new RangeError('load: allocation failed');
      if (input.heapOffset >= 0) {
        Module.HEAPU8.copyWithin(Number(ptr), input.heapOffset,
          input.heapOffset + input.length);
      } else {
        Module.HEAPU8.set(input.bytes, Number(ptr));
      }
      const status = layerCall('_lightusd_next_layer_load',
        [state.handle, ptr, input.length], [1]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true, primCount: Module['_lightusd_next_layer_prim_count'](state.handle)};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'loadJSON', {value: function(value) {
    const state = layerState(this);
    if (arguments.length !== 1 || typeof value !== 'string') {
      throw new TypeError('loadJSON: expected one JSON string');
    }
    if (value.includes('\0')) throw new TypeError('loadJSON: input must be NUL-free');
    const input = layerEncoder.encode(value);
    if (!input.length || input.length > 0x20000000) {
      throw new RangeError('loadJSON: input must be between 1 byte and 512 MiB');
    }
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](input.length);
      if (!ptr) throw new RangeError('loadJSON: allocation failed');
      Module.HEAPU8.set(input, Number(ptr));
      const status = layerCall('_lightusd_next_layer_load_json',
        [state.handle, ptr, input.length], [1]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true, primCount: Module['_lightusd_next_layer_prim_count'](state.handle)};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'loadWithProgress', {value: function(value, callback) {
    const state = layerState(this);
    if (arguments.length !== 2 || typeof callback !== 'function') {
      throw new TypeError('loadWithProgress: expected a byte view and callback');
    }
    const input = layerBytes(value, 'loadWithProgress');
    let ptr = 0;
    ++state.busy;
    const previous = Module.__lightusdNextLayerProgressCallback;
    let callbackError = null;
    Module.__lightusdNextLayerProgressCallback = event => {
      try { return callback(event) !== false; }
      catch (error) { callbackError = error; return false; }
    };
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(input.length, 1));
      if (!ptr) throw new RangeError('loadWithProgress: allocation failed');
      if (input.heapOffset >= 0) {
        Module.HEAPU8.copyWithin(Number(ptr), input.heapOffset,
          input.heapOffset + input.length);
      } else {
        Module.HEAPU8.set(input.bytes, Number(ptr));
      }
      const status = layerCall('_lightusd_next_layer_load_with_progress',
        [state.handle, ptr, input.length], [1]);
      if (callbackError) throw callbackError;
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true, primCount: Module['_lightusd_next_layer_prim_count'](state.handle)};
    } finally {
      Module.__lightusdNextLayerProgressCallback = previous;
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getMhProfileJSON', {value: function() {
    const state = layerState(this);
    if (arguments.length !== 0) throw new TypeError('getMhProfileJSON: wrong argument count');
    ++state.busy;
    let ptr = 0;
    try {
      const size = Module['_lightusd_next_layer_mh_profile_json_size'](state.handle);
      if (size < 0) throw new RangeError(layerError(state) || 'getMhProfileJSON: query failed');
      if (size > 0x20000000) throw new RangeError('getMhProfileJSON: output exceeds 512 MiB');
      ptr = Module['_lightusd_next_alloc'](Math.max(size, 1));
      if (!ptr) throw new RangeError('getMhProfileJSON: allocation failed');
      let copied;
      try { copied = Module['_lightusd_next_layer_mh_profile_json_copy'](state.handle, ptr, size); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        copied = Module['_lightusd_next_layer_mh_profile_json_copy'](state.handle, BigInt(ptr), size);
      }
      if (copied !== size) throw new RangeError('getMhProfileJSON: result changed during copy');
      return layerDecoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getShadingGraphJSON', {value: function() {
    const state = layerState(this);
    if (arguments.length !== 0) throw new TypeError('getShadingGraphJSON: wrong argument count');
    ++state.busy;
    let ptr = 0;
    try {
      const size = Module['_lightusd_next_layer_shading_graph_json_size'](state.handle);
      if (size < 0) throw new RangeError(layerError(state) || 'getShadingGraphJSON: query failed');
      if (size === 0) return '';
      if (size > 0x20000000) throw new RangeError('getShadingGraphJSON: output exceeds 512 MiB');
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getShadingGraphJSON: allocation failed');
      let copied;
      try { copied = Module['_lightusd_next_layer_shading_graph_json_copy'](state.handle, ptr, size); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        copied = Module['_lightusd_next_layer_shading_graph_json_copy'](state.handle, BigInt(ptr), size);
      }
      if (copied !== size) throw new RangeError('getShadingGraphJSON: result changed during copy');
      return layerDecoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'definePrim', {value: function(path, typeName = '') {
    if (arguments.length < 1 || arguments.length > 2) throw new TypeError('definePrim: expected path and optional type');
    return layerStringsCall(this, '_lightusd_next_layer_define_prim',
      [path, typeName]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'removePrim', {value: function(path) {
    if (arguments.length !== 1) throw new TypeError('removePrim: expected path');
    return layerStringsCall(this, '_lightusd_next_layer_remove_prim', [path]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'removeAttribute', {value: function(path, name) {
    if (arguments.length !== 2) throw new TypeError('removeAttribute: expected path and name');
    return layerStringsCall(this, '_lightusd_next_layer_remove_attribute', [path, name]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'setStringAttribute', {value: function(path, name, value) {
    if (arguments.length !== 3) throw new TypeError('setStringAttribute: expected path, name and value');
    return layerStringsCall(this, '_lightusd_next_layer_set_string', [path, name, value]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'setNumberAttribute', {value: function(path, name, value) {
    if (arguments.length !== 3 || typeof value !== 'number' || !Number.isFinite(value)) {
      throw new TypeError('setNumberAttribute: expected path, name and finite number');
    }
    return layerStringsCall(this, '_lightusd_next_layer_set_number', [path, name], [value]);
  }});
  const attributeMetadataKinds = new Map([
    ['interpolation', 1], ['colorSpace', 1], ['displayName', 2],
    ['displayGroup', 2], ['doc', 2], ['hidden', 0],
    ['elementSize', 3], ['weight', 4]
  ]);
  Object.defineProperty(Module.LayerDocument.prototype, 'setAttributeMetadata', {value: function(path, name, key, value) {
    const state = layerState(this);
    if (arguments.length !== 4 || typeof path !== 'string' || typeof name !== 'string' ||
        typeof key !== 'string' || path.includes('\0') || name.includes('\0') ||
        key.includes('\0') || !attributeMetadataKinds.has(key)) {
      throw new TypeError('setAttributeMetadata: unsupported key or wrong argument count');
    }
    const kind = attributeMetadataKinds.get(key);
    let maximumBytes = (path.length + name.length + key.length) * 3;
    if (kind === 1 || kind === 2) {
      if (typeof value !== 'string' || value.includes('\0')) {
        throw new TypeError('setAttributeMetadata: expected NUL-free string');
      }
      maximumBytes += value.length * 3;
    } else if (kind === 0 && typeof value !== 'boolean') {
      throw new TypeError('setAttributeMetadata: expected boolean');
    } else if (kind === 3 && (!Number.isInteger(value) || value < -0x80000000 || value > 0x7fffffff)) {
      throw new TypeError('setAttributeMetadata: expected signed 32-bit integer');
    } else if (kind === 4 && (typeof value !== 'number' || !Number.isFinite(value))) {
      throw new TypeError('setAttributeMetadata: expected finite number');
    }
    if (maximumBytes > 0x20000000) throw new RangeError('setAttributeMetadata: inputs exceed 512 MiB');
    let payload = new Uint8Array(0), number = 0, integer = 0;
    if (kind === 0) {
      if (typeof value !== 'boolean') throw new TypeError('setAttributeMetadata: expected boolean');
      payload = new Uint8Array([value ? 1 : 0]);
    } else if (kind === 1 || kind === 2) {
      if (typeof value !== 'string' || value.includes('\0')) {
        throw new TypeError('setAttributeMetadata: expected NUL-free string');
      }
      payload = layerEncoder.encode(value);
    } else if (kind === 3) {
      if (!Number.isInteger(value) || value < -0x80000000 || value > 0x7fffffff) {
        throw new TypeError('setAttributeMetadata: expected signed 32-bit integer');
      }
      integer = value;
    } else {
      if (typeof value !== 'number' || !Number.isFinite(value)) {
        throw new TypeError('setAttributeMetadata: expected finite number');
      }
      number = value;
    }
    const pathBytes = layerEncoder.encode(path), nameBytes = layerEncoder.encode(name),
      keyBytes = layerEncoder.encode(key);
    const total = pathBytes.length + nameBytes.length + keyBytes.length + payload.length;
    if (total > 0x20000000) throw new RangeError('setAttributeMetadata: inputs exceed 512 MiB');
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('setAttributeMetadata: allocation failed');
      const base = Number(ptr);
      Module.HEAPU8.set(pathBytes, base);
      Module.HEAPU8.set(nameBytes, base + pathBytes.length);
      Module.HEAPU8.set(keyBytes, base + pathBytes.length + nameBytes.length);
      Module.HEAPU8.set(payload, base + pathBytes.length + nameBytes.length + keyBytes.length);
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const status = layerCall('_lightusd_next_layer_set_attribute_metadata',
        [state.handle, pointer(0), pathBytes.length,
          pointer(pathBytes.length), nameBytes.length,
          pointer(pathBytes.length + nameBytes.length), keyBytes.length, kind,
          pointer(pathBytes.length + nameBytes.length + keyBytes.length),
          payload.length, number, integer], [1, 3, 5, 8]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getAttributeMetadata', {value: function(path, name, key) {
    const state = layerState(this);
    if (arguments.length !== 3 || typeof path !== 'string' || typeof name !== 'string' ||
        typeof key !== 'string' || path.includes('\0') || name.includes('\0') ||
        key.includes('\0') || !attributeMetadataKinds.has(key)) {
      throw new TypeError('getAttributeMetadata: unsupported key or wrong argument count');
    }
    const pathBytes = layerEncoder.encode(path), nameBytes = layerEncoder.encode(name),
      keyBytes = layerEncoder.encode(key);
    const inputSize = pathBytes.length + nameBytes.length + keyBytes.length;
    if (inputSize > 0x20000000) throw new RangeError('getAttributeMetadata: inputs exceed 512 MiB');
    let input = 0, info = 0;
    ++state.busy;
    try {
      input = Module['_lightusd_next_alloc'](Math.max(inputSize, 1));
      info = Module['_lightusd_next_alloc'](1);
      if (!input || !info) throw new RangeError('getAttributeMetadata: allocation failed');
      Module.HEAPU8.set(pathBytes, Number(input));
      Module.HEAPU8.set(nameBytes, Number(input) + pathBytes.length);
      Module.HEAPU8.set(keyBytes, Number(input) + pathBytes.length + nameBytes.length);
      const pointer = (ptr, offset = 0) => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const call = (out, cap) => layerCall('_lightusd_next_layer_get_attribute_metadata',
        [state.handle, input, pathBytes.length, pointer(input, pathBytes.length),
          nameBytes.length, pointer(input, pathBytes.length + nameBytes.length),
          keyBytes.length, info, out, cap], [1, 3, 5, 7, 8]);
      const required = call(0, 0);
      if (required < 0) return {success: false, error: layerError(state)};
      if (required > 0x20000000) throw new RangeError('getAttributeMetadata: result exceeds 512 MiB');
      const kind = Module.HEAPU8[Number(info)];
      if (kind !== attributeMetadataKinds.get(key)) {
        throw new RangeError('getAttributeMetadata: native type does not match the key');
      }
      let output = 0;
      try {
        output = Module['_lightusd_next_alloc'](Math.max(required, 1));
        if (!output) throw new RangeError('getAttributeMetadata: result allocation failed');
        const copied = call(output, required);
        if (copied !== required) throw new RangeError('getAttributeMetadata: value changed during copy');
        let value;
        if (kind === 0) {
          if (required !== 1 || Module.HEAPU8[Number(output)] > 1) throw new RangeError('getAttributeMetadata: invalid boolean');
          value = Module.HEAPU8[Number(output)] !== 0;
        } else if (kind === 1 || kind === 2) {
          value = layerDecoder.decode(Module.HEAPU8.subarray(Number(output), Number(output) + required));
        } else if (kind === 3) {
          if (required !== 4) throw new RangeError('getAttributeMetadata: invalid integer size');
          value = new DataView(Module.HEAPU8.buffer, Number(output), 4).getInt32(0, true);
        } else {
          if (required !== 8) throw new RangeError('getAttributeMetadata: invalid double size');
          value = new DataView(Module.HEAPU8.buffer, Number(output), 8).getFloat64(0, true);
        }
        return {success: true, value};
      } finally { if (output) Module['_lightusd_next_free'](output); }
    } finally {
      if (info) Module['_lightusd_next_free'](info);
      if (input) Module['_lightusd_next_free'](input);
      --state.busy;
    }
  }});
  const stageMetadataKinds = new Map([
    ['defaultPrim', 1], ['upAxis', 1], ['colorManagementSystem', 1],
    ['doc', 0], ['comment', 0], ['colorConfiguration', 2],
    ['metersPerUnit', 3], ['timeCodesPerSecond', 3], ['startTimeCode', 3],
    ['endTimeCode', 3], ['framesPerSecond', 3], ['kilogramsPerUnit', 3]
  ]);
  Object.defineProperty(Module.LayerDocument.prototype, 'setStageMetadata', {value: function(key, value) {
    const state = layerState(this);
    if (arguments.length !== 2 || typeof key !== 'string' || key.includes('\0') ||
        !stageMetadataKinds.has(key)) {
      throw new TypeError('setStageMetadata: unsupported key or wrong argument count');
    }
    const kind = stageMetadataKinds.get(key);
    const numeric = kind === 3;
    if (numeric ? (typeof value !== 'number' || !Number.isFinite(value))
                : (typeof value !== 'string' || value.includes('\0'))) {
      throw new TypeError('setStageMetadata: value type does not match metadata key');
    }
    const keyBytes = layerEncoder.encode(key);
    const textBytes = numeric ? new Uint8Array(0) : layerEncoder.encode(value);
    if (keyBytes.length + textBytes.length > 0x20000000) {
      throw new RangeError('setStageMetadata: input exceeds 512 MiB');
    }
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(keyBytes.length + textBytes.length, 1));
      if (!ptr) throw new RangeError('setStageMetadata: allocation failed');
      Module.HEAPU8.set(keyBytes, Number(ptr));
      Module.HEAPU8.set(textBytes, Number(ptr) + keyBytes.length);
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const args = [state.handle, pointer(0), keyBytes.length, kind,
        pointer(keyBytes.length), textBytes.length, numeric ? value : 0];
      const status = layerCall('_lightusd_next_layer_set_stage_metadata', args, [1, 4]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  const primMetadataKinds = new Map([
    ['active', 'bool'], ['hidden', 'bool'], ['instanceable', 'bool'],
    ['kind', 'token'], ['doc', 'string'], ['comment', 'string'],
    ['displayName', 'string'], ['apiSchemas', 'tokenArray']
  ]);
  Object.defineProperty(Module.LayerDocument.prototype, 'setPrimMetadata', {value: function(path, key, value) {
    const state = layerState(this);
    if (arguments.length !== 3 || typeof path !== 'string' ||
        typeof key !== 'string' || path.includes('\0') || key.includes('\0') ||
        !primMetadataKinds.has(key)) {
      throw new TypeError('setPrimMetadata: unsupported key or wrong argument count');
    }
    const valueKind = primMetadataKinds.get(key);
    let kind, payload, count = 1;
    if (valueKind === 'bool') {
      if (typeof value !== 'boolean') throw new TypeError('setPrimMetadata: expected boolean value');
      kind = 0;
      payload = new Uint8Array([value ? 1 : 0]);
    } else if (valueKind === 'token' || valueKind === 'string') {
      if (typeof value !== 'string' || value.includes('\0')) {
        throw new TypeError('setPrimMetadata: expected NUL-free string value');
      }
      kind = valueKind === 'token' ? 1 : 2;
      payload = layerEncoder.encode(value);
    } else {
      if (!Array.isArray(value)) {
        throw new TypeError('setPrimMetadata: apiSchemas expects an array of tokens');
      }
      if (value.length > 65536) {
        throw new RangeError('setPrimMetadata: token count exceeds 65536');
      }
      let maximumBytes = value.length * 4;
      for (const item of value) {
        if (typeof item !== 'string' || item.includes('\0')) {
          throw new TypeError('setPrimMetadata: apiSchemas expects NUL-free tokens');
        }
        maximumBytes += item.length * 3;
        if (maximumBytes > 0x20000000) {
          throw new RangeError('setPrimMetadata: token array exceeds 512 MiB');
        }
      }
      if (maximumBytes > 0x20000000) {
        throw new RangeError('setPrimMetadata: token array exceeds 512 MiB');
      }
      kind = 3;
      count = value.length;
      const encoded = value.map(item => layerEncoder.encode(item));
      const packedSize = encoded.reduce((sum, bytes) => sum + 4 + bytes.length, 0);
      if (packedSize > 0x20000000) throw new RangeError('setPrimMetadata: token array exceeds 512 MiB');
      payload = new Uint8Array(packedSize);
      const view = new DataView(payload.buffer);
      let offset = 0;
      for (const bytes of encoded) {
        view.setUint32(offset, bytes.length, true);
        offset += 4;
        payload.set(bytes, offset);
        offset += bytes.length;
      }
    }
    const pathBytes = layerEncoder.encode(path), keyBytes = layerEncoder.encode(key);
    const total = pathBytes.length + keyBytes.length + payload.length;
    if (total > 0x20000000) throw new RangeError('setPrimMetadata: inputs exceed 512 MiB');
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('setPrimMetadata: allocation failed');
      const base = Number(ptr);
      Module.HEAPU8.set(pathBytes, base);
      Module.HEAPU8.set(keyBytes, base + pathBytes.length);
      Module.HEAPU8.set(payload, base + pathBytes.length + keyBytes.length);
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const args = [state.handle, pointer(0), pathBytes.length,
        pointer(pathBytes.length), keyBytes.length, kind,
        pointer(pathBytes.length + keyBytes.length), payload.length, count];
      const status = layerCall('_lightusd_next_layer_set_prim_metadata',
        args, [1, 3, 6]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getPrimMetadata', {value: function(path, key) {
    const state = layerState(this);
    if (arguments.length !== 2 || typeof path !== 'string' ||
        typeof key !== 'string' || path.includes('\0') || key.includes('\0') ||
        !primMetadataKinds.has(key)) {
      throw new TypeError('getPrimMetadata: unsupported key or wrong argument count');
    }
    const pathBytes = layerEncoder.encode(path), keyBytes = layerEncoder.encode(key);
    if (pathBytes.length + keyBytes.length > 0x20000000) {
      throw new RangeError('getPrimMetadata: inputs exceed 512 MiB');
    }
    let input = 0, info = 0;
    ++state.busy;
    try {
      input = Module['_lightusd_next_alloc'](Math.max(pathBytes.length + keyBytes.length, 1));
      info = Module['_lightusd_next_alloc'](8);
      if (!input || !info) throw new RangeError('getPrimMetadata: allocation failed');
      Module.HEAPU8.set(pathBytes, Number(input));
      Module.HEAPU8.set(keyBytes, Number(input) + pathBytes.length);
      const pointer = (ptr, offset = 0) => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const call = (out, cap) => layerCall('_lightusd_next_layer_get_prim_metadata',
        [state.handle, input, pathBytes.length, pointer(input, pathBytes.length),
          keyBytes.length, info, pointer(info, 4), out, cap], [1, 3, 5, 6, 7]);
      const authored = layerCall('_lightusd_next_layer_prim_metadata_is_authored',
        [state.handle, input, pathBytes.length, pointer(input, pathBytes.length),
          keyBytes.length], [1, 3]);
      if (authored < 0) return {success: false, error: layerError(state)};
      if (authored === 0) return {success: true, authored: false};
      const required = call(0, 0);
      if (required < 0) return {success: false, error: layerError(state)};
      if (required > 0x20000000) throw new RangeError('getPrimMetadata: result exceeds 512 MiB');
      const kind = Module.HEAPU8[Number(info)];
      const count = new DataView(Module.HEAPU8.buffer, Number(info), 8).getUint32(4, true);
      if (kind > 3 || count > 65536) throw new RangeError('getPrimMetadata: invalid native result');
      let output = 0;
      try {
        output = Module['_lightusd_next_alloc'](Math.max(required, 1));
        if (!output) throw new RangeError('getPrimMetadata: result allocation failed');
        const copied = call(output, required);
        if (copied !== required) throw new RangeError('getPrimMetadata: value changed during copy');
        const bytes = Module.HEAPU8.subarray(Number(output), Number(output) + required);
        let value;
        if (kind === 0) {
          if (required !== 1 || count !== 1 || bytes[0] > 1) throw new RangeError('getPrimMetadata: invalid boolean');
          value = bytes[0] !== 0;
        } else if (kind === 1 || kind === 2) {
          if (count !== 1) throw new RangeError('getPrimMetadata: invalid scalar count');
          value = layerDecoder.decode(bytes);
        } else {
          value = [];
          let offset = 0;
          const view = new DataView(Module.HEAPU8.buffer, Number(output), required);
          for (let i = 0; i < count; ++i) {
            if (offset + 4 > required) throw new RangeError('getPrimMetadata: truncated token array');
            const length = view.getUint32(offset, true);
            offset += 4;
            if (length > required - offset) throw new RangeError('getPrimMetadata: invalid token length');
            value.push(layerDecoder.decode(Module.HEAPU8.subarray(Number(output) + offset,
              Number(output) + offset + length)));
            offset += length;
          }
          if (offset !== required) throw new RangeError('getPrimMetadata: trailing token bytes');
        }
        return {success: true, authored: true, value};
      } finally { if (output) Module['_lightusd_next_free'](output); }
    } finally {
      if (info) Module['_lightusd_next_free'](info);
      if (input) Module['_lightusd_next_free'](input);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getStageMetadata', {value: function(key) {
    const state = layerState(this);
    if (arguments.length !== 1 || typeof key !== 'string' || key.includes('\0') ||
        !stageMetadataKinds.has(key)) {
      throw new TypeError('getStageMetadata: unsupported key or wrong argument count');
    }
    const keyBytes = layerEncoder.encode(key);
    let input = 0;
    ++state.busy;
    try {
      input = Module['_lightusd_next_alloc'](Math.max(keyBytes.length, 1));
      if (!input) throw new RangeError('getStageMetadata: allocation failed');
      Module.HEAPU8.set(keyBytes, Number(input));
      const keyPtr = input;
      const call = (symbol, args, pointers) => {
        try { return layerCall(symbol, args, pointers); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return layerCall(symbol, args.map((value, index) =>
            pointers.includes(index) ? BigInt(value) : value));
        }
      };
      const authored = call('_lightusd_next_layer_stage_metadata_is_authored',
        [state.handle, keyPtr, keyBytes.length], [1]);
      if (authored < 0) return {success: false, error: layerError(state)};
      const kind = stageMetadataKinds.get(key);
      if (kind === 3) {
        let output = 0;
        try {
          output = Module['_lightusd_next_alloc'](8);
          if (!output) throw new RangeError('getStageMetadata: result allocation failed');
          const status = call('_lightusd_next_layer_get_stage_metadata_number',
            [state.handle, keyPtr, keyBytes.length, output], [1, 3]);
          if (status !== 0) return {success: false, error: layerError(state)};
          return {success: true, authored: authored !== 0,
            value: new DataView(Module.HEAPU8.buffer,
              Number(output), 8).getFloat64(0, true)};
        } finally { if (output) Module['_lightusd_next_free'](output); }
      }
      const query = call('_lightusd_next_layer_get_stage_metadata_string',
        [state.handle, keyPtr, keyBytes.length, 0, 0], [1, 3]);
      if (query < 0) return {success: false, error: layerError(state)};
      if (query > 0x20000000) throw new RangeError('getStageMetadata: value exceeds 512 MiB');
      let output = 0;
      try {
        output = Module['_lightusd_next_alloc'](Math.max(query, 1));
        if (!output) throw new RangeError('getStageMetadata: result allocation failed');
        const copied = call('_lightusd_next_layer_get_stage_metadata_string',
          [state.handle, keyPtr, keyBytes.length, output, query], [1, 3]);
        if (copied !== query) throw new RangeError('getStageMetadata: value changed during copy');
        return {success: true, authored: authored !== 0, value: layerDecoder.decode(
          Module.HEAPU8.subarray(Number(output), Number(output) + query))};
      } finally { if (output) Module['_lightusd_next_free'](output); }
    } finally {
      if (input) Module['_lightusd_next_free'](input);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'setRelationshipTargets', {value: function(path, name, targets) {
    const state = layerState(this);
    if (arguments.length !== 3 || typeof path !== 'string' ||
        typeof name !== 'string' || !Array.isArray(targets) ||
        path.includes('\0') || name.includes('\0')) {
      throw new TypeError('setRelationshipTargets: expected path, name, and NUL-free target strings');
    }
    if (targets.length > 65536) {
      throw new RangeError('setRelationshipTargets: target count exceeds 65536');
    }
    let maxEncodedBytes = (path.length + name.length) * 3 + targets.length * 4;
    for (const target of targets) {
      if (typeof target !== 'string' || target.includes('\0')) {
        throw new TypeError('setRelationshipTargets: expected path, name, and NUL-free target strings');
      }
      maxEncodedBytes += target.length * 3;
      if (maxEncodedBytes > 0x20000000) {
        throw new RangeError('setRelationshipTargets: inputs exceed 512 MiB');
      }
    }
    if (maxEncodedBytes > 0x20000000) {
      throw new RangeError('setRelationshipTargets: inputs exceed 512 MiB');
    }
    const pathBytes = layerEncoder.encode(path);
    const nameBytes = layerEncoder.encode(name);
    const targetBytes = targets.map(target => layerEncoder.encode(target));
    const packedSize = targetBytes.reduce((sum, bytes) => sum + 4 + bytes.length, 0);
    const total = pathBytes.length + nameBytes.length + packedSize;
    if (total > 0x20000000) {
      throw new RangeError('setRelationshipTargets: inputs exceed 512 MiB');
    }
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('setRelationshipTargets: allocation failed');
      const base = Number(ptr);
      Module.HEAPU8.set(pathBytes, base);
      Module.HEAPU8.set(nameBytes, base + pathBytes.length);
      let offset = base + pathBytes.length + nameBytes.length;
      for (const bytes of targetBytes) {
        new DataView(Module.HEAPU8.buffer).setUint32(offset, bytes.length, true);
        offset += 4;
        Module.HEAPU8.set(bytes, offset);
        offset += bytes.length;
      }
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const args = [state.handle, pointer(0), pathBytes.length,
        pointer(pathBytes.length), nameBytes.length,
        pointer(pathBytes.length + nameBytes.length), packedSize, targets.length];
      const status = layerCall('_lightusd_next_layer_set_relationship_targets',
        args, [1, 3, 5]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getRelationshipTargets', {value: function(path, name) {
    const state = layerState(this);
    if (arguments.length !== 2 || typeof path !== 'string' || typeof name !== 'string' ||
        path.includes('\0') || name.includes('\0')) {
      throw new TypeError('getRelationshipTargets: expected NUL-free path and name');
    }
    const pathBytes = layerEncoder.encode(path), nameBytes = layerEncoder.encode(name);
    let input = 0;
    ++state.busy;
    try {
      input = Module['_lightusd_next_alloc'](Math.max(pathBytes.length + nameBytes.length, 1));
      if (!input) throw new RangeError('getRelationshipTargets: allocation failed');
      const base = Number(input);
      Module.HEAPU8.set(pathBytes, base);
      Module.HEAPU8.set(nameBytes, base + pathBytes.length);
      const pointer = offset => typeof input === 'bigint' ? input + BigInt(offset) : input + offset;
      const args = [state.handle, pointer(0), pathBytes.length,
        pointer(pathBytes.length), nameBytes.length];
      let count;
      try { count = layerCall('_lightusd_next_layer_relationship_target_count', args, [1, 3]); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        count = layerCall('_lightusd_next_layer_relationship_target_count',
          args.map((value, index) => index === 1 || index === 3 ? BigInt(value) : value));
      }
      if (count < 0) return {success: false, error: layerError(state)};
      let total = 0;
      const targets = [];
      for (let index = 0; index < count; ++index) {
        const queryArgs = [...args, index, 0, 0];
        let size;
        try { size = layerCall('_lightusd_next_layer_relationship_target_copy', queryArgs, [1, 3, 6]); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          size = layerCall('_lightusd_next_layer_relationship_target_copy',
            queryArgs.map((value, i) => i === 1 || i === 3 || i === 6 ? BigInt(value) : value));
        }
        if (size < 0 || total > 0x20000000 - size) {
          throw new RangeError('getRelationshipTargets: invalid or oversized target payload');
        }
        total += size;
        let output = 0;
        try {
          output = Module['_lightusd_next_alloc'](Math.max(size, 1));
          if (!output) throw new RangeError('getRelationshipTargets: target allocation failed');
          const copyArgs = [...args, index, output, size];
          let copied;
          try { copied = layerCall('_lightusd_next_layer_relationship_target_copy', copyArgs, [1, 3, 6]); }
          catch (error) {
            if (!(error instanceof TypeError)) throw error;
            copied = layerCall('_lightusd_next_layer_relationship_target_copy',
              copyArgs.map((value, i) => i === 1 || i === 3 || i === 6 ? BigInt(value) : value));
          }
          if (copied !== size) throw new RangeError('getRelationshipTargets: target changed');
          targets.push(layerDecoder.decode(Module.HEAPU8.subarray(Number(output), Number(output) + size)));
        } finally { if (output) Module['_lightusd_next_free'](output); }
      }
      return {success: true, targets};
    } finally {
      if (input) Module['_lightusd_next_free'](input);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'removeRelationship', {value: function(path, name) {
    if (arguments.length !== 2) throw new TypeError('removeRelationship: expected path and name');
    return layerStringsCall(this, '_lightusd_next_layer_remove_relationship', [path, name]);
  }});
  const layerPodTypes = {
    bool: {array: Uint8Array, components: 1, kind: 'bool'},
    int: {array: Int32Array, components: 1, kind: 'number'},
    uint: {array: Uint32Array, components: 1, kind: 'number'},
    int64: {array: BigInt64Array, components: 1, kind: 'bigint'},
    uint64: {array: BigUint64Array, components: 1, kind: 'bigint'},
    float: {array: Float32Array, components: 1, kind: 'number'},
    double: {array: Float64Array, components: 1, kind: 'number'},
    half: {array: Float32Array, components: 1, kind: 'number', half: true},
    half2: {array: Float32Array, components: 2, kind: 'number', half: true},
    half3: {array: Float32Array, components: 3, kind: 'number', half: true},
    half4: {array: Float32Array, components: 4, kind: 'number', half: true},
    int2: {array: Int32Array, components: 2, kind: 'number'},
    int3: {array: Int32Array, components: 3, kind: 'number'},
    int4: {array: Int32Array, components: 4, kind: 'number'},
    uint2: {array: Uint32Array, components: 2, kind: 'number'},
    uint3: {array: Uint32Array, components: 3, kind: 'number'},
    uint4: {array: Uint32Array, components: 4, kind: 'number'},
    float2: {array: Float32Array, components: 2, kind: 'number'},
    float3: {array: Float32Array, components: 3, kind: 'number'},
    float4: {array: Float32Array, components: 4, kind: 'number'},
    double2: {array: Float64Array, components: 2, kind: 'number'},
    double3: {array: Float64Array, components: 3, kind: 'number'},
    double4: {array: Float64Array, components: 4, kind: 'number'},
    point3f: {array: Float32Array, components: 3, kind: 'number'},
    point3d: {array: Float64Array, components: 3, kind: 'number'},
    vector3f: {array: Float32Array, components: 3, kind: 'number'},
    vector3d: {array: Float64Array, components: 3, kind: 'number'},
    normal3f: {array: Float32Array, components: 3, kind: 'number'},
    normal3d: {array: Float64Array, components: 3, kind: 'number'},
    color3f: {array: Float32Array, components: 3, kind: 'number'},
    color3d: {array: Float64Array, components: 3, kind: 'number'},
    color4f: {array: Float32Array, components: 4, kind: 'number'},
    color4d: {array: Float64Array, components: 4, kind: 'number'},
    quatf: {array: Float32Array, components: 4, kind: 'number'},
    quatd: {array: Float64Array, components: 4, kind: 'number'},
    quath: {array: Float32Array, components: 4, kind: 'number', half: true},
    point3h: {array: Float32Array, components: 3, kind: 'number', half: true},
    vector3h: {array: Float32Array, components: 3, kind: 'number', half: true},
    normal3h: {array: Float32Array, components: 3, kind: 'number', half: true},
    color3h: {array: Float32Array, components: 3, kind: 'number', half: true},
    color4h: {array: Float32Array, components: 4, kind: 'number', half: true},
    texCoord2h: {array: Float32Array, components: 2, kind: 'number', half: true},
    texCoord3h: {array: Float32Array, components: 3, kind: 'number', half: true},
    matrix2f: {array: Float32Array, components: 4, kind: 'number'},
    matrix2d: {array: Float64Array, components: 4, kind: 'number'},
    matrix3f: {array: Float32Array, components: 9, kind: 'number'},
    matrix4f: {array: Float32Array, components: 16, kind: 'number'},
    matrix3d: {array: Float64Array, components: 9, kind: 'number'},
    matrix4d: {array: Float64Array, components: 16, kind: 'number'},
    texCoord2f: {array: Float32Array, components: 2, kind: 'number'},
    texCoord2d: {array: Float64Array, components: 2, kind: 'number'},
    texCoord3f: {array: Float32Array, components: 3, kind: 'number'},
    texCoord3d: {array: Float64Array, components: 3, kind: 'number'}
  };
  const float32ToHalfBits = value => {
    const f32 = new Float32Array([value]);
    const bits = new Uint32Array(f32.buffer)[0];
    const sign = (bits >>> 16) & 0x8000;
    const exponent = (bits >>> 23) & 0xff;
    const mantissa = bits & 0x7fffff;
    if (exponent === 0) return sign;
    const halfExponent = exponent - 127 + 15;
    if (halfExponent >= 31) throw new RangeError('setAttribute: half value is out of range');
    if (halfExponent <= 0) {
      if (halfExponent < -10) return sign;
      const significand = mantissa | 0x800000;
      const shift = 14 - halfExponent;
      const truncated = significand >>> shift;
      const remainder = significand & ((1 << shift) - 1);
      const halfway = 1 << (shift - 1);
      const roundUp = remainder > halfway ||
        (remainder === halfway && (truncated & 1));
      return sign | (truncated + (roundUp ? 1 : 0));
    }
    let rounded = mantissa >>> 13;
    const remainder = mantissa & 0x1fff;
    if (remainder > 0x1000 || (remainder === 0x1000 && (rounded & 1))) ++rounded;
    let adjustedExponent = halfExponent;
    if (rounded === 0x400) { rounded = 0; ++adjustedExponent; }
    if (adjustedExponent >= 31) throw new RangeError('setAttribute: half value is out of range');
    return sign | (adjustedExponent << 10) | rounded;
  };
  const layerTypedValue = (type, value, requestedArray) => {
    if (type === 'string' || type === 'token' || type === 'asset') {
      const isArray = requestedArray === undefined
        ? Array.isArray(value) : requestedArray;
      if (typeof isArray !== 'boolean') throw new TypeError('setAttribute: isArray must be boolean');
      const values = isArray ? value : [value];
      if (!Array.isArray(values)) {
        throw new TypeError('setAttribute: string-family arrays require an array');
      }
      const encoded = values.map(item => {
        if (typeof item !== 'string' || item.includes('\0')) {
          throw new TypeError('setAttribute: string-family values must be NUL-free strings');
        }
        const bytes = layerEncoder.encode(item);
        if (bytes.length > 0xffffffff) throw new RangeError('setAttribute: string value is too large');
        return bytes;
      });
      if (!isArray) return {bytes: encoded[0], count: 1, isArray: false, heapOffset: -1};
      const totalBytes = encoded.reduce((sum, bytes) => sum + 4 + bytes.length, 0);
      if (totalBytes > 0x20000000) throw new RangeError('setAttribute: data exceeds 512 MiB');
      const bytes = new Uint8Array(totalBytes), view = new DataView(bytes.buffer);
      let offset = 0;
      for (const item of encoded) {
        view.setUint32(offset, item.length, true);
        offset += 4;
        bytes.set(item, offset);
        offset += item.length;
      }
      return {bytes, count: encoded.length, isArray: true, heapOffset: -1};
    }
    const spec = layerPodTypes[type];
    if (!spec) throw new TypeError('setAttribute: unsupported type ' + type);
    const isView = ArrayBuffer.isView(value) && !(value instanceof DataView);
    let isArray = requestedArray;
    if (isArray === undefined) {
      isArray = isView || (Array.isArray(value) &&
        !(spec.components > 1 && value.length === spec.components));
    }
    if (typeof isArray !== 'boolean') throw new TypeError('setAttribute: isArray must be boolean');

    let typed;
    if (isView) {
      if (!(value instanceof spec.array)) {
        throw new TypeError('setAttribute: ' + type + ' requires ' + spec.array.name);
      }
      typed = value;
      if (type === 'bool') {
        for (const item of typed) if (item !== 0 && item !== 1) {
          throw new TypeError('setAttribute: bool buffers contain only 0 or 1');
        }
      }
    } else {
      const items = Array.isArray(value) ? value : [value];
      if (type === 'bool') {
        for (const item of items) if (typeof item !== 'boolean') {
          throw new TypeError('setAttribute: bool values must be booleans');
        }
        typed = new Uint8Array(items.map(item => item ? 1 : 0));
      } else if (spec.kind === 'bigint') {
        const min = type === 'int64' ? -(1n << 63n) : 0n;
        const max = type === 'int64' ? (1n << 63n) - 1n : (1n << 64n) - 1n;
        const big = items.map(item => {
          if (typeof item === 'bigint') return item;
          if (typeof item !== 'number' || !Number.isSafeInteger(item)) {
            throw new TypeError('setAttribute: 64-bit integer values require BigInt or safe integers');
          }
          return BigInt(item);
        });
        for (const item of big) if (item < min || item > max) {
          throw new RangeError('setAttribute: 64-bit integer is out of range');
        }
        typed = new spec.array(big);
      } else {
        for (const item of items) {
          if (typeof item !== 'number' || !Number.isFinite(item)) {
            throw new TypeError('setAttribute: numeric values must be finite numbers');
          }
          if ((type === 'int' || type.startsWith('int')) &&
              (!Number.isInteger(item) || item < -2147483648 || item > 2147483647)) {
            throw new RangeError('setAttribute: signed 32-bit integer is out of range');
          }
          if ((type === 'uint' || type.startsWith('uint')) &&
              (!Number.isInteger(item) || item < 0 || item > 4294967295)) {
            throw new RangeError('setAttribute: unsigned 32-bit integer is out of range');
          }
          if (spec.array === Float32Array) {
            if (Math.abs(item) > 3.4028234663852886e38) {
              throw new RangeError('setAttribute: float32 value is out of range');
            }
          }
          if (spec.half && Math.abs(item) > 65504) {
            throw new RangeError('setAttribute: half value is out of range');
          }
        }
        typed = new spec.array(items);
      }
    }

    if (typed.length % spec.components !== 0) {
      throw new TypeError('setAttribute: value length does not match type components');
    }
    const count = typed.length / spec.components;
    if (!isArray && count !== 1) {
      throw new TypeError('setAttribute: scalar value must match one type element');
    }
    if (spec.half && !isArray) {
      const packed = new Uint16Array(spec.components);
      for (let i = 0; i < packed.length; ++i) {
        const component = typed[i];
        if (!Number.isFinite(component) || Math.abs(component) > 65504) {
          throw new RangeError('setAttribute: half value is out of range');
        }
        packed[i] = float32ToHalfBits(component);
      }
      return {bytes: new Uint8Array(packed.buffer), count: 1, isArray: false,
        heapOffset: -1};
    }
    return {bytes: new Uint8Array(typed.buffer, typed.byteOffset, typed.byteLength),
      count, isArray, heapOffset: typed.buffer === Module.HEAPU8.buffer
        ? typed.byteOffset : -1};
  };
  Object.defineProperty(Module.LayerDocument.prototype, 'setAttribute', {value: function(path, name, type, value, isArray, options) {
    if (arguments.length < 4 || arguments.length > 6) {
      throw new TypeError('setAttribute: expected path, name, type, value, optional isArray and flags');
    }
    if (typeof type !== 'string') throw new TypeError('setAttribute: type must be a string');
    const state = layerState(this);
    if (typeof path !== 'string' || typeof name !== 'string' ||
        path.includes('\0') || name.includes('\0')) {
      throw new TypeError('setAttribute: path and name must be NUL-free strings');
    }
    const pathBytes = layerEncoder.encode(path), nameBytes = layerEncoder.encode(name);
    const typeBytes = layerEncoder.encode(type);
    const typed = layerTypedValue(type, value, isArray);
    let flags = typed.isArray ? 1 : 0;
    if (options !== undefined) {
      if (!options || typeof options !== 'object' || Array.isArray(options) ||
          Object.keys(options).some(key => !['uniform', 'custom'].includes(key)) ||
          (options.uniform !== undefined && typeof options.uniform !== 'boolean') ||
          (options.custom !== undefined && typeof options.custom !== 'boolean'))
        throw new TypeError('setAttribute: flags require boolean uniform/custom options');
      flags |= 0x80 | (options.uniform ? 4 : 0) | (options.custom ? 2 : 0);
    }

    const stringBytes = pathBytes.length + nameBytes.length + typeBytes.length;
    const dataPadding = (8 - (stringBytes & 7)) & 7;
    const total = stringBytes + dataPadding + typed.bytes.length;
    if (total > 0x20000000) throw new RangeError('setAttribute: data exceeds 512 MiB');
    let ptr = 0;
    ++state.busy;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('setAttribute: allocation failed');
      const base = Number(ptr);
      const offsets = [];
      let offset = 0;
      for (const bytes of [pathBytes, nameBytes, typeBytes]) {
        offsets.push(offset);
        Module.HEAPU8.set(bytes, base + offset);
        offset += bytes.length;
      }
      const dataOffset = offset + dataPadding;
      if (typed.heapOffset >= 0) {
        Module.HEAPU8.copyWithin(base + dataOffset, typed.heapOffset,
          typed.heapOffset + typed.bytes.length);
      } else {
        Module.HEAPU8.set(typed.bytes, base + dataOffset);
      }
      const pointer = at => typeof ptr === 'bigint' ? ptr + BigInt(at) : ptr + at;
      const args = [state.handle, pointer(offsets[0]), pathBytes.length,
        pointer(offsets[1]), nameBytes.length, pointer(offsets[2]), typeBytes.length,
        flags, pointer(dataOffset), typed.bytes.length, typed.count];
      const status = layerCall('_lightusd_next_layer_set_typed', args, [1, 3, 5, 8]);
      return status === 0 ? {success: true} : {success: false, error: layerError(state)};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'primCount', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('primCount: wrong argument count');
    const count = Module['_lightusd_next_layer_prim_count'](state.handle);
    if (count < 0) throw new RangeError('primCount: invalid receiver');
    return count;
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'loaded', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('loaded: wrong argument count');
    return Module['_lightusd_next_layer_loaded'](state.handle) === 1;
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'error', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('error: wrong argument count');
    return layerError(state);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportUSDA', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('exportUSDA: wrong argument count');
    ++state.busy;
    try {
      const size = Module['_lightusd_next_layer_export_usda_size'](state.handle);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'USDA output exceeds 512 MiB limit'};
      const data = Module['_lightusd_next_layer_export_usda_data'](state.handle);
      if (!data && size) throw new RangeError('exportUSDA: retained output is missing');
      // Decode directly from the retained C output into JS string ownership;
      // do not stage another output-sized buffer in either WASM or JS memory.
      const view = Module.HEAPU8.subarray(Number(data), Number(data) + size);
      return {success: true, text: layerDecoder.decode(view)};
    } finally {
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportJSON', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('exportJSON: wrong argument count');
    ++state.busy;
    try {
      const size = Module['_lightusd_next_layer_export_json_size'](state.handle);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'Layer JSON output exceeds 512 MiB limit'};
      const data = Module['_lightusd_next_layer_export_json_data'](state.handle);
      if (!data && size) throw new RangeError('exportJSON: retained output is missing');
      return {success: true, text: layerDecoder.decode(
        Module.HEAPU8.subarray(Number(data), Number(data) + size))};
    } finally {
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportJSONWithOptions', {value: function(embedBuffers, arrayMode) {
    if (arguments.length !== 2 || typeof embedBuffers !== 'boolean' ||
        typeof arrayMode !== 'string') {
      throw new TypeError('exportJSONWithOptions: expected a boolean and an array mode string');
    }
    // Layer PrimSpec JSON uses canonical USD value text; unlike the Stage and
    // geometry exporters it does not populate buffer/accessor tables. The
    // legacy layer serializer therefore emits the same document for both modes.
    return this.exportJSON();
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'validateLoadedLayer', {value: function(options) {
    const state = layerState(this);
    if (arguments.length !== 1 || typeof options !== 'string') {
      throw new TypeError('validateLoadedLayer: expected one options JSON string');
    }
    if (Module['_lightusd_next_layer_loaded'](state.handle) !== 1) {
      return JSON.stringify({parse_ok: false, ok: false,
        error: 'No Layer is loaded. Use loadAsLayerFromBinary first.'});
    }
    // Export the current authored state, then validate directly from the
    // document's retained C bytes. Only the small filename/options block is
    // staged in WASM; do not decode USDA to a JS string and re-encode/copy the
    // entire layer merely to call the same synchronous C validator.
    const size = Module['_lightusd_next_layer_export_usda_size'](state.handle);
    if (size < 0) {
      return JSON.stringify({parse_ok: false, ok: false,
        error: layerError(state) || 'Layer export failed'});
    }
    if (!Number.isSafeInteger(size) || size > 0x20000000) {
      throw new RangeError('validateLoadedLayer: USDA exceeds 512 MiB limit');
    }
    const data = Module['_lightusd_next_layer_export_usda_data'](state.handle);
    if (!data && size) throw new RangeError('validateLoadedLayer: retained USDA is missing');
    const filename = validateEncoder.encode('<layer-document>.usda');
    const config = validateEncoder.encode(options);
    const total = filename.length + config.length;
    let input = 0, output = 0;
    ++state.busy;
    try {
      input = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!input) throw new RangeError('validateLoadedLayer: allocation failed');
      const base = Number(input);
      Module.HEAPU8.set(filename, base);
      Module.HEAPU8.set(config, base + filename.length);
      const pointer = offset => typeof input === 'bigint' ? input + BigInt(offset)
                                                          : input + offset;
      const call = (source, filenamePtr, configPtr) => Module['_lightusd_next_validate_json'](
        source, size, filenamePtr, filename.length, configPtr, config.length);
      try {
        output = call(data, pointer(0), pointer(filename.length));
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        output = call(BigInt(data), BigInt(pointer(0)), BigInt(pointer(filename.length)));
      }
      if (!output) throw new RangeError('validateLoadedLayer: validation allocation failed');
      return UTF8ToString(Number(output));
    } finally {
      if (output) Module['_lightusd_next_free'](output);
      if (input) Module['_lightusd_next_free'](input);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportUSDC', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('exportUSDC: wrong argument count');
    ++state.busy;
    try {
      const size = Module['_lightusd_next_layer_export_usdc_size'](state.handle);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'USDC output exceeds 512 MiB limit'};
      const data = Module['_lightusd_next_layer_export_usdc_data'](state.handle);
      if (!data && size) throw new RangeError('exportUSDC: retained output is missing');
      const output = new Uint8Array(size);
      output.set(Module.HEAPU8.subarray(Number(data), Number(data) + size));
      return {success: true, data: output};
    } finally {
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportUSDZ', {value: function(options = {}, assetStore = null) {
    const state = layerState(this);
    if (arguments.length > 2) throw new TypeError('exportUSDZ: expected options and optional NextAssetStore');
    if (options === null) options = {};
    if (typeof options !== 'object' || ArrayBuffer.isView(options)) {
      throw new TypeError('exportUSDZ: options must be an object');
    }
    const root = options.rootLayerFormat === undefined ? 'usda' : options.rootLayerFormat;
    if (root !== 'usda' && root !== 'usdc') {
      throw new TypeError('exportUSDZ: rootLayerFormat must be usda or usdc');
    }
    if (Module['_lightusd_next_layer_loaded'](state.handle) !== 1) {
      return {success: false, error: layerError(state) || 'No layer loaded'};
    }
    let assetState = null;
    if (assetStore !== null) {
      assetState = live.get(assetStore);
      if (!assetState?.handle || assetState.kind !== 5) {
        throw new TypeError('exportUSDZ: assetStore must be a live NextAssetStore');
      }
    }
    ++state.busy;
    if (assetState) ++assetState.busy;
    let ptr = 0;
    try {
      const size = Module['_lightusd_next_layer_export_usdz'](
        state.handle, assetState ? assetState.handle : 0, root === 'usdc' ? 1 : 0);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'USDZ output exceeds 512 MiB limit'};
      const output = new Uint8Array(size);
      if (size) {
        ptr = Module['_lightusd_next_alloc'](size);
        if (!ptr) throw new RangeError('exportUSDZ: output allocation failed');
        const copied = layerCall('_lightusd_next_layer_export_usdz_copy',
          [state.handle, ptr, size], [1]);
        if (copied !== size) throw new RangeError('exportUSDZ: retained output changed');
        output.set(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
      }
      return {success: true, data: output};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      if (assetState) --assetState.busy;
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportUSDCToBuffer', {value: function(buffer, options) {
    const state = layerState(this);
    if (arguments.length !== 2) throw new TypeError('exportUSDCToBuffer: expected buffer and options');
    // Preserve legacy validation order: loaded layer, output kind, then capacity.
    if (Module['_lightusd_next_layer_loaded'](state.handle) !== 1) {
      return {success: false, size: 0, error: layerError(state) || 'No layer loaded'};
    }
    if (buffer === null || buffer === undefined) {
      return {success: false, size: 0, error: 'USDC export output buffer is null.'};
    }
    if (!(buffer instanceof Uint8Array)) {
      return {success: false, size: 0, error: 'USDC export output must be a Uint8Array.'};
    }
    if (buffer.byteLength === 0) {
      return {success: false, size: 0, error: 'USDC export output buffer is empty.'};
    }
    const exported = this.exportUSDC();
    if (!exported.success) return {success: false, size: 0, error: exported.error};
    if (exported.data.byteLength > buffer.byteLength) {
      return {success: false, size: 0, error: 'USDC export output buffer too small.'};
    }
    buffer.set(exported.data, 0);
    return {success: true, size: exported.data.byteLength, warn: ''};
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'end', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('end: wrong argument count');
    if (state.busy) throw new TypeError('Cannot end an active LayerDocument operation');
    Module['_lightusd_next_layer_end'](state.handle);
  }});

  const assetStoreEncoder = new TextEncoder();
  const assetStoreDecoder = new TextDecoder();
  const assetStoreStringResult = (state, symbol, value) => {
    const input = assetStoreEncoder.encode(value);
    let inputPtr = 0, outputPtr = 0;
    try {
      inputPtr = Module['_lightusd_next_alloc'](Math.max(input.length, 1));
      if (!inputPtr) throw new RangeError('asset cache allocation failed');
      Module.HEAPU8.set(input, Number(inputPtr));
      const copy = (ptr, cap) => Module[symbol](state.handle, inputPtr,
        input.length, ptr, cap);
      let size;
      try { size = copy(0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        size = Module[symbol](state.handle, BigInt(inputPtr), input.length, 0n, 0);
      }
      if (size < 0) throw new RangeError('asset cache query failed');
      if (!size) return '';
      outputPtr = Module['_lightusd_next_alloc'](size);
      if (!outputPtr) throw new RangeError('asset cache allocation failed');
      let copied;
      try { copied = copy(outputPtr, size); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        copied = Module[symbol](state.handle, BigInt(inputPtr), input.length,
          BigInt(outputPtr), size);
      }
      if (copied !== size) throw new RangeError('asset cache result changed');
      return assetStoreDecoder.decode(Module.HEAPU8.subarray(
        Number(outputPtr), Number(outputPtr) + size));
    } finally {
      if (outputPtr) Module['_lightusd_next_free'](outputPtr);
      if (inputPtr) Module['_lightusd_next_free'](inputPtr);
    }
  };
  const kMaxAssetStoreAggregateBytes = 0x20000000;
  const assetIdentifierSizePlan = state => {
    const count = Module['_lightusd_next_asset_store_identifier_count'](state.handle);
    if (!Number.isSafeInteger(count) || count < 0 || count > 65536) {
      throw new RangeError('assetIdentifiers: invalid or excessive identifier count');
    }
    let estimate = count * 128;
    const sizes = [];
    for (let index = 0; index < count; ++index) {
      let size;
      try { size = Module['_lightusd_next_asset_store_identifier_copy'](state.handle, index, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        size = Module['_lightusd_next_asset_store_identifier_copy'](state.handle, index, 0n, 0);
      }
      const charge = size * 2 + 64;
      if (!Number.isSafeInteger(size) || size < 0 || !Number.isSafeInteger(charge) ||
          estimate > kMaxAssetStoreAggregateBytes - charge) {
        throw new RangeError('assetIdentifiers: aggregate exceeds 512 MiB limit');
      }
      estimate += charge;
      sizes.push(size);
    }
    return sizes;
  };
  const assetStoreStringSize = (state, symbol, value, label) => {
    const input = assetStoreEncoder.encode(value);
    if (input.length > kMaxAssetStoreAggregateBytes) {
      throw new RangeError(label + ': input exceeds 512 MiB limit');
    }
    let inputPtr = 0;
    try {
      inputPtr = Module['_lightusd_next_alloc'](Math.max(input.length, 1));
      if (!inputPtr) throw new RangeError(label + ': allocation failed');
      Module.HEAPU8.set(input, Number(inputPtr));
      let size;
      try { size = Module[symbol](state.handle, inputPtr, input.length, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        size = Module[symbol](state.handle, BigInt(inputPtr), input.length, 0n, 0);
      }
      if (!Number.isSafeInteger(size) || size < 0 || size > kMaxAssetStoreAggregateBytes) {
        throw new RangeError(label + ': invalid or oversized string');
      }
      return size;
    } finally { if (inputPtr) Module['_lightusd_next_free'](inputPtr); }
  };
  const assetStoreOutputString = (state, symbol, args, label) => {
    let size;
    try { size = Module[symbol](state.handle, ...args, 0, 0); }
    catch (error) {
      if (!(error instanceof TypeError)) throw error;
      size = Module[symbol](state.handle, ...args, 0n, 0);
    }
    if (!Number.isSafeInteger(size) || size < 0 || size > 0x20000000) {
      throw new RangeError(label + ': invalid or oversized string');
    }
    if (!size) return '';
    const ptr = Module['_lightusd_next_alloc'](size);
    if (!ptr) throw new RangeError(label + ': allocation failed');
    try {
      let copied;
      try { copied = Module[symbol](state.handle, ...args, ptr, size); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        copied = Module[symbol](state.handle, ...args, BigInt(ptr), size);
      }
      if (copied !== size) throw new RangeError(label + ': payload changed during copy');
      return assetStoreDecoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally { Module['_lightusd_next_free'](ptr); }
  };
  const assetStoreSetPath = (state, symbol, path, label) => {
    if (typeof path !== 'string') throw new TypeError(label + ': expected a path string');
    const bytes = assetStoreEncoder.encode(path);
    const ptr = Module['_lightusd_next_alloc'](Math.max(bytes.length, 1));
    if (!ptr) throw new RangeError(label + ': allocation failed');
    Module.HEAPU8.set(bytes, Number(ptr));
    try {
      let result;
      try { result = Module[symbol](state.handle, ptr, bytes.length); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        result = Module[symbol](state.handle, BigInt(ptr), bytes.length);
      }
      if (result !== 0) throw new RangeError(label + ': update failed');
    } finally { Module['_lightusd_next_free'](ptr); }
  };
  const assetStoreBytes = (value, label) => {
    if (!ArrayBuffer.isView(value)) throw new TypeError(label + ': expected byte view');
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  };
  Object.defineProperty(Module.NextAssetStore.prototype, 'registerMemoryAsset', {value: function(identifier, bytes) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length === 1) { bytes = identifier; identifier = ''; }
    else if (arguments.length !== 2) throw new TypeError('registerMemoryAsset: expected bytes or identifier and bytes');
    if (identifier !== null && typeof identifier !== 'string') {
      throw new TypeError('registerMemoryAsset: identifier must be a string or null');
    }
    const idBytes = assetStoreEncoder.encode(identifier || '');
    const payload = assetStoreBytes(bytes, 'registerMemoryAsset');
    if (payload.byteLength > 0x40000000) throw new RangeError('registerMemoryAsset: payload exceeds 1 GiB');
    const outputCap = Math.max(idBytes.length, 64);
    const total = idBytes.length + payload.length + outputCap;
    if (total > 0xffffffff) throw new RangeError('registerMemoryAsset: input too large');
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('registerMemoryAsset: allocation failed');
      const base = Number(ptr);
      const payloadOffset = base + idBytes.length;
      const outputOffset = payloadOffset + payload.length;
      Module.HEAPU8.set(idBytes, base);
      Module.HEAPU8.set(payload, payloadOffset);
      const p = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const call = (handle, idPtr, dataPtr, outPtr) => Module['_lightusd_next_asset_store_register'](
        handle, idPtr, idBytes.length, dataPtr, payload.length, outPtr, outputCap);
      let idSize;
      try { idSize = call(state.handle, ptr, p(idBytes.length), p(idBytes.length + payload.length)); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        idSize = call(state.handle, BigInt(ptr), p(idBytes.length),
          p(idBytes.length + payload.length));
      }
      if (idSize === -4) throw new RangeError('registerMemoryAsset: metadata limit exceeded');
      if (idSize < 0) throw new RangeError('registerMemoryAsset: aggregate cache limit exceeded');
      return assetStoreDecoder.decode(Module.HEAPU8.subarray(outputOffset, outputOffset + idSize));
    } finally { if (ptr) Module['_lightusd_next_free'](ptr); --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setAssetFromRawPointer', {value: function(identifier, pointer, byteLength) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 3 || (identifier !== null && typeof identifier !== 'string') ||
        !Number.isSafeInteger(byteLength) || byteLength < 1 || byteLength > 0x40000000) {
      throw new TypeError('setAssetFromRawPointer: expected identifier, heap pointer and byte length');
    }
    const heapLength = Module.HEAPU8.byteLength;
    let offset;
    if (typeof pointer === 'bigint') {
      if (pointer < 0n || pointer > BigInt(heapLength)) throw new RangeError('setAssetFromRawPointer: pointer outside WASM heap');
      offset = Number(pointer);
    } else {
      if (!Number.isSafeInteger(pointer) || pointer < 0 || pointer > heapLength) {
        throw new RangeError('setAssetFromRawPointer: pointer outside WASM heap');
      }
      offset = pointer;
    }
    if (offset === 0 || byteLength > heapLength - offset) {
      throw new RangeError('setAssetFromRawPointer: byte span outside WASM heap');
    }
    const id = assetStoreEncoder.encode(identifier || '');
    ++state.busy;
    let keyPtr = 0;
    try {
      keyPtr = Module['_lightusd_next_alloc'](Math.max(id.length, 1));
      if (!keyPtr) throw new RangeError('setAssetFromRawPointer: allocation failed');
      if (id.length) Module.HEAPU8.set(id, Number(keyPtr));
      const rawPtr = typeof keyPtr === 'bigint' ? BigInt(offset) : offset;
      const invoke = (handle, key) => Module['_lightusd_next_asset_store_set_raw'](
        handle, key, id.length, rawPtr, byteLength);
      let status;
      try { status = invoke(state.handle, keyPtr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = invoke(state.handle, BigInt(keyPtr));
      }
      if (status === -4) throw new RangeError('setAssetFromRawPointer: metadata limit exceeded');
      if (status < 0) throw new TypeError('setAssetFromRawPointer: invalid argument or cache limit exceeded');
      return status === 1;
    } finally { if (keyPtr) Module['_lightusd_next_free'](keyPtr); --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'unregisterMemoryAsset', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier) {
      throw new TypeError('unregisterMemoryAsset: expected a non-empty identifier');
    }
    const bytes = assetStoreEncoder.encode(identifier);
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](bytes.length);
      if (!ptr) throw new RangeError('unregisterMemoryAsset: allocation failed');
      Module.HEAPU8.set(bytes, Number(ptr));
      const invoke = handle => Module['_lightusd_next_asset_store_unregister'](handle, ptr, bytes.length);
      let status;
      try { status = invoke(state.handle); }
      catch (error) { if (!(error instanceof TypeError)) throw error; status = Module['_lightusd_next_asset_store_unregister'](state.handle, BigInt(ptr), bytes.length); }
      if (status < 0) throw new RangeError('unregisterMemoryAsset: operation failed');
      return status === 1;
    } finally { if (ptr) Module['_lightusd_next_free'](ptr); --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'readMemoryAsset', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier) {
      throw new TypeError('readMemoryAsset: expected a non-empty identifier');
    }
    const name = assetStoreEncoder.encode(identifier);
    ++state.busy;
    let namePtr = 0, dataPtr = 0;
    try {
      namePtr = Module['_lightusd_next_alloc'](name.length);
      if (!namePtr) throw new RangeError('readMemoryAsset: allocation failed');
      Module.HEAPU8.set(name, Number(namePtr));
      const call = (out, cap) => Module['_lightusd_next_asset_store_read'](
        state.handle, namePtr, name.length, out, cap);
      let size;
      try { size = call(0, 0); }
      catch (error) { if (!(error instanceof TypeError)) throw error;
        size = Module['_lightusd_next_asset_store_read'](state.handle, BigInt(namePtr), name.length, 0n, 0); }
      if (size === -2) return null;
      if (size < 0) throw new RangeError('readMemoryAsset: read failed');
      if (size === 0) return new Uint8Array(0);
      dataPtr = Module['_lightusd_next_alloc'](size);
      if (!dataPtr) throw new RangeError('readMemoryAsset: allocation failed');
      try { if (call(dataPtr, size) !== size) throw new RangeError('readMemoryAsset: read changed'); }
      catch (error) { if (!(error instanceof TypeError)) throw error;
        if (Module['_lightusd_next_asset_store_read'](state.handle, BigInt(namePtr), name.length, BigInt(dataPtr), size) !== size) throw new RangeError('readMemoryAsset: read changed'); }
      return new Uint8Array(Module.HEAPU8.buffer, Number(dataPtr), size).slice();
    } finally {
      if (dataPtr) Module['_lightusd_next_free'](dataPtr);
      if (namePtr) Module['_lightusd_next_free'](namePtr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetCacheDataAsMemoryView', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string')
      throw new TypeError('getAssetCacheDataAsMemoryView: expected an identifier');
    if (!identifier) return undefined;
    const name = assetStoreEncoder.encode(identifier);
    let namePtr = 0, infoPtr = 0;
    try {
      namePtr = Module['_lightusd_next_alloc'](name.length);
      infoPtr = Module['_lightusd_next_alloc'](16);
      if (!namePtr || !infoPtr) throw new RangeError('getAssetCacheDataAsMemoryView: allocation failed');
      Module.HEAPU8.set(name, Number(namePtr));
      const infoOffset = Number(infoPtr);
      Module.HEAPU8.fill(0, infoOffset, infoOffset + 16);
      const call = (idPointer, infoPointer) => Module['_lightusd_next_asset_store_view'](
        state.handle, idPointer, name.length, infoPointer,
        typeof infoPointer === 'bigint' ? infoPointer + 8n : infoPointer + 8);
      let status;
      try { status = call(namePtr, infoPtr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = call(BigInt(namePtr), BigInt(infoPtr));
      }
      if (status === 0) return undefined;
      if (status < 0) throw new RangeError('getAssetCacheDataAsMemoryView: view query failed');
      const view = new DataView(Module.HEAPU8.buffer, infoOffset, 16);
      const dataPointer = typeof infoPtr === 'bigint'
        ? view.getBigUint64(0, true) : BigInt(view.getUint32(0, true));
      const size = view.getUint32(8, true);
      if (size && dataPointer === 0n)
        throw new RangeError('getAssetCacheDataAsMemoryView: invalid payload pointer');
      return new Uint8Array(Module.HEAPU8.buffer, Number(dataPointer), size);
    } finally {
      if (infoPtr) Module['_lightusd_next_free'](infoPtr);
      if (namePtr) Module['_lightusd_next_free'](namePtr);
    }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAsset', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string')
      throw new TypeError('getAsset: expected an identifier');
    const data = identifier ? this.readMemoryAsset(identifier) : null;
    if (data === null) return {};
    return {name: identifier, data, sha256: this.getAssetHash(identifier),
      uuid: this.getAssetUUID(identifier)};
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setAlias', {value: function(authored, resolved) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof authored !== 'string' || !authored ||
        typeof resolved !== 'string' || !resolved) throw new TypeError('setAlias: expected two non-empty identifiers');
    const a = assetStoreEncoder.encode(authored), r = assetStoreEncoder.encode(resolved);
    const total = a.length + r.length;
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](total);
      if (!ptr) throw new RangeError('setAlias: allocation failed');
      const base = Number(ptr); Module.HEAPU8.set(a, base); Module.HEAPU8.set(r, base + a.length);
      const invoke = (handle, p) => Module['_lightusd_next_asset_store_set_alias'](
        handle, p, a.length, typeof p === 'bigint' ? p + BigInt(a.length) : p + a.length, r.length);
      let status;
      try { status = invoke(state.handle, ptr); }
      catch (error) { if (!(error instanceof TypeError)) throw error; status = invoke(state.handle, BigInt(ptr)); }
      if (status === -2) throw new RangeError('setAlias: metadata limit exceeded');
      if (status !== 0) throw new RangeError('setAlias: alias rejected');
    } finally { if (ptr) Module['_lightusd_next_free'](ptr); --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'assetIdentifiers', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('assetIdentifiers: wrong argument count');
    const sizes = assetIdentifierSizePlan(state);
    return sizes.map((size, index) => assetStoreOutputString(state,
      '_lightusd_next_asset_store_identifier_copy', [index], 'assetIdentifiers'));
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetUUID', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier)
      throw new TypeError('getAssetUUID: expected a non-empty identifier');
    return assetStoreStringResult(state, '_lightusd_next_asset_store_uuid', identifier);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getStreamingAssetUUID', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier)
      throw new TypeError('getStreamingAssetUUID: expected a non-empty asset identifier');
    return assetStoreStringResult(state, '_lightusd_next_asset_store_streaming_uuid', identifier);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'findAssetByUUID', {value: function(uuid) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof uuid !== 'string' || !uuid)
      throw new TypeError('findAssetByUUID: expected a non-empty UUID');
    return assetStoreStringResult(state, '_lightusd_next_asset_store_find_uuid', uuid);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetByUUID', {value: function(uuid) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof uuid !== 'string')
      throw new TypeError('getAssetByUUID: expected a UUID');
    const identifier = uuid ? assetStoreStringResult(state,
      '_lightusd_next_asset_store_find_uuid', uuid) : '';
    if (!identifier) return {error: 'Asset not found with UUID: ' + uuid};
    return this.getAsset(identifier);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetHash', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier)
      throw new TypeError('getAssetHash: expected a non-empty identifier');
    return assetStoreStringResult(state, '_lightusd_next_asset_store_hash', identifier);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'verifyAssetHash', {value: function(identifier, hash) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof identifier !== 'string' || !identifier ||
        typeof hash !== 'string' || !hash)
      throw new TypeError('verifyAssetHash: expected an identifier and hash');
    const idBytes = assetStoreEncoder.encode(identifier);
    const hashBytes = assetStoreEncoder.encode(hash);
    const total = idBytes.length + hashBytes.length;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](total);
      if (!ptr) throw new RangeError('verifyAssetHash: allocation failed');
      const base = Number(ptr), hashOffset = base + idBytes.length;
      Module.HEAPU8.set(idBytes, base);
      Module.HEAPU8.set(hashBytes, hashOffset);
      const invoke = (basePtr, hashPtr) => Module['_lightusd_next_asset_store_verify_hash'](
        state.handle, basePtr, idBytes.length, hashPtr, hashBytes.length);
      let status;
      try { status = invoke(ptr, typeof ptr === 'bigint' ? ptr + BigInt(idBytes.length) : ptr + idBytes.length); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = invoke(BigInt(ptr), BigInt(ptr) + BigInt(idBytes.length));
      }
      if (status < 0) throw new RangeError('verifyAssetHash: operation failed');
      return status === 1;
    } finally { if (ptr) Module['_lightusd_next_free'](ptr); }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAllAssetUUIDs', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAllAssetUUIDs: wrong argument count');
    const identifiers = this.assetIdentifiers();
    let estimate = identifiers.length * 256;
    for (const identifier of identifiers) {
      const identifierCharge = assetStoreEncoder.encode(identifier).length * 2 + 64;
      const uuidBytes = assetStoreStringSize(state,
        '_lightusd_next_asset_store_uuid', identifier, 'getAllAssetUUIDs');
      const uuidCharge = uuidBytes * 2 + 64;
      if (!Number.isSafeInteger(identifierCharge) || !Number.isSafeInteger(uuidCharge) ||
          estimate > kMaxAssetStoreAggregateBytes - identifierCharge ||
          estimate + identifierCharge > kMaxAssetStoreAggregateBytes - uuidCharge) {
        throw new RangeError('getAllAssetUUIDs: aggregate exceeds 512 MiB limit');
      }
      estimate += identifierCharge + uuidCharge;
    }
    const result = {};
    for (const identifier of identifiers) Object.defineProperty(result,
      identifier, {value: assetStoreStringResult(state,
        '_lightusd_next_asset_store_uuid', identifier), enumerable: true,
        configurable: true, writable: true});
    return result;
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'deleteAssetByUUID', {value: function(uuid) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof uuid !== 'string' || !uuid)
      throw new TypeError('deleteAssetByUUID: expected a non-empty UUID');
    const bytes = assetStoreEncoder.encode(uuid);
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](bytes.length);
      if (!ptr) throw new RangeError('deleteAssetByUUID: allocation failed');
      Module.HEAPU8.set(bytes, Number(ptr));
      let status;
      try { status = Module['_lightusd_next_asset_store_delete_uuid'](state.handle, ptr, bytes.length); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_asset_store_delete_uuid'](state.handle, BigInt(ptr), bytes.length);
      }
      if (status < 0) throw new RangeError('deleteAssetByUUID: operation failed');
      return status === 1;
    } finally { if (ptr) Module['_lightusd_next_free'](ptr); }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'assetExists', {value: function(nameOrUuid) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof nameOrUuid !== 'string' || !nameOrUuid)
      throw new TypeError('assetExists: expected a non-empty name or UUID');
    return !!this.getAssetUUID(nameOrUuid) || !!this.findAssetByUUID(nameOrUuid);
  }});
  // Legacy size_t asset getters (count and cache bytes) return Numbers on
  // wasm32 and BigInts on memory64; the uint64 exports return BigInts on both.
  let memory64Build;
  const legacyCacheSize = value => {
    if (memory64Build === undefined) {
      const probe = Module['_lightusd_next_alloc'](1);
      memory64Build = typeof probe === 'bigint';
      if (probe) Module['_lightusd_next_free'](probe);
    }
    return memory64Build ? BigInt(value) : Number(value);
  };
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetCount', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetCount: wrong argument count');
    return legacyCacheSize(Module['_lightusd_next_asset_store_identifier_count'](state.handle));
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setBaseWorkingPath', {value: function(path) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('setBaseWorkingPath: expected one path');
    ++state.busy;
    try { assetStoreSetPath(state, '_lightusd_next_asset_store_set_base_path', path, 'setBaseWorkingPath'); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getBaseWorkingPath', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getBaseWorkingPath: wrong argument count');
    ++state.busy;
    try { return assetStoreOutputString(state, '_lightusd_next_asset_store_base_path', [], 'getBaseWorkingPath'); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'addAssetSearchPath', {value: function(path) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('addAssetSearchPath: expected one path');
    ++state.busy;
    try { assetStoreSetPath(state, '_lightusd_next_asset_store_add_search_path', path, 'addAssetSearchPath'); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'clearAssetSearchPaths', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('clearAssetSearchPaths: wrong argument count');
    ++state.busy;
    try {
      if (Module['_lightusd_next_asset_store_clear_search_paths'](state.handle) !== 0)
        throw new RangeError('clearAssetSearchPaths: update failed');
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setAllowParentRelativeAssetPaths', {value: function(allow) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof allow !== 'boolean')
      throw new TypeError('setAllowParentRelativeAssetPaths: expected one boolean');
    ++state.busy;
    try {
      if (Module['_lightusd_next_asset_store_set_allow_parent_paths'](state.handle, allow ? 1 : 0) !== 0)
        throw new RangeError('setAllowParentRelativeAssetPaths: update failed');
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAllowParentRelativeAssetPaths', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAllowParentRelativeAssetPaths: wrong argument count');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_asset_store_get_allow_parent_paths'](state.handle);
      if (value !== 0 && value !== 1) throw new RangeError('getAllowParentRelativeAssetPaths: query failed');
      return value === 1;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetSearchPaths', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetSearchPaths: wrong argument count');
    ++state.busy;
    try {
      const count = Module['_lightusd_next_asset_store_search_path_count'](state.handle);
      if (!Number.isInteger(count) || count < 0 || count > 65536)
        throw new RangeError('getAssetSearchPaths: invalid or excessive path count');
      const sizes = [];
      let total = count * 32;
      for (let i = 0; i < count; ++i) {
        let size;
        try { size = Module['_lightusd_next_asset_store_search_path_copy'](state.handle, i, 0, 0); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          size = Module['_lightusd_next_asset_store_search_path_copy'](state.handle, i, 0n, 0);
        }
        if (!Number.isSafeInteger(size) || size < 0) throw new RangeError('getAssetSearchPaths: invalid path size');
        total += size;
        if (!Number.isSafeInteger(total) || total > 0x20000000)
          throw new RangeError('getAssetSearchPaths: aggregate exceeds 512 MiB limit');
        sizes.push(size);
      }
      return sizes.map((size, i) => assetStoreOutputString(state,
        '_lightusd_next_asset_store_search_path_copy', [i], 'getAssetSearchPaths'));
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'hasAsset', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('hasAsset: wrong argument count');
    return this.assetExists(name);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setAsset', {value: function(name, bytes) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof name !== 'string' || !name)
      throw new TypeError('setAsset: expected a non-empty name and byte view');
    const overwritten = this.assetIdentifiers().includes(name);
    this.registerMemoryAsset(name, bytes);
    return overwritten;
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'deleteAssetByName', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('deleteAssetByName: wrong argument count');
    return this.unregisterMemoryAsset(name);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'deleteAsset', {value: function(nameOrUuid) {
    if (arguments.length !== 1 || typeof nameOrUuid !== 'string' || !nameOrUuid)
      throw new TypeError('deleteAsset: expected a non-empty name or UUID');
    if (this.assetIdentifiers().includes(nameOrUuid)) return this.deleteAssetByName(nameOrUuid);
    return this.deleteAssetByUUID(nameOrUuid);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'clearAssets', {value: function() {
    if (arguments.length) throw new TypeError('clearAssets: wrong argument count');
    return this.clear();
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setMemoryLimitBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit < 1 || limit > 0x40000000) {
      throw new TypeError('setMemoryLimitBytes: expected 1 byte through 1 GiB');
    }
    const status = Module['_lightusd_next_asset_store_set_memory_limit'](state.handle, limit);
    if (status !== 0) throw new RangeError('setMemoryLimitBytes: limit below current use');
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setMetadataLimitBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 ||
        !(typeof limit === 'bigint' ? limit >= 1n && limit <= 0x40000000n :
          Number.isSafeInteger(limit) && limit >= 1 && limit <= 0x40000000)) {
      throw new TypeError('setMetadataLimitBytes: expected 1 byte through 1 GiB');
    }
    const value = BigInt(limit);
    if (Module['_lightusd_next_asset_store_set_metadata_limit'](state.handle, value) !== 0) {
      throw new RangeError('setMetadataLimitBytes: limit below current metadata use');
    }
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'metadataBytesUsed', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('metadataBytesUsed: wrong argument count');
    return Module['_lightusd_next_asset_store_metadata_bytes'](state.handle);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'metadataLimitBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('metadataLimitBytes: wrong argument count');
    return Module['_lightusd_next_asset_store_metadata_limit'](state.handle);
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'memoryStats', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('memoryStats: wrong argument count');
    return {assetCount: Module['_lightusd_next_asset_store_identifier_count'](state.handle),
      bytesUsed: Module['_lightusd_next_asset_store_memory_bytes'](state.handle),
      limitBytes: Module['_lightusd_next_asset_store_memory_limit'](state.handle)};
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetCacheSizeBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetCacheSizeBytes: wrong argument count');
    return legacyCacheSize(Module['_lightusd_next_asset_store_cache_bytes'](state.handle));
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'getAssetCacheMaxSizeBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetCacheMaxSizeBytes: wrong argument count');
    return legacyCacheSize(Module['_lightusd_next_asset_store_cache_max_bytes'](state.handle));
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'setAssetCacheMaxSizeBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 ||
        !(typeof limit === 'bigint' ? limit >= 0n && limit <= 0xffffffffffffffffn :
          Number.isSafeInteger(limit) && limit >= 0))
      throw new TypeError('setAssetCacheMaxSizeBytes: expected a uint64 byte limit');
    if (Module['_lightusd_next_asset_store_set_cache_max_bytes'](state.handle,
      BigInt(limit)) !== 0) throw new RangeError('setAssetCacheMaxSizeBytes: update failed');
  }});
  Object.defineProperty(Module.NextAssetStore.prototype, 'clear', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('clear: wrong argument count');
    if (Module['_lightusd_next_asset_store_clear'](state.handle) !== 0) throw new RangeError('clear: failed');
  }});
  const converterEncoder = new TextEncoder();
  const converterDecoder = new TextDecoder();
  function converterState(self) {
    const state = live.get(self);
    if (!state?.handle || state.kind !== 1) throw new TypeError('Invalid LightUSD receiver');
    return state;
  }
  function converterByteView(value, method, copyHeap = true) {
    if (!ArrayBuffer.isView(value)) throw new TypeError(method + ': expected byte view');
    let bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    if (bytes.length > (1 << 30)) throw new RangeError(method + ': data exceeds 1 GiB');
    if (copyHeap && bytes.buffer === Module.HEAPU8.buffer) bytes = bytes.slice();
    return bytes;
  }
  function converterSetBytes(self, kind, name, data, method) {
    const state = converterState(self);
    const nameBytes = converterEncoder.encode(name);
    const heapSource = (kind === 0 || kind === 1) && data.buffer === Module.HEAPU8.buffer;
    const heapSourceOffset = heapSource ? data.byteOffset : 0;
    const heapSourceLength = data.byteLength;
    if (nameBytes.length > (1 << 20)) throw new RangeError(method + ': name exceeds 1 MiB');
    if (data.length > (1 << 30)) throw new RangeError(method + ': data exceeds 1 GiB');
    if (kind === 0 || kind === 2) {
      const allowed = Module['_lightusd_next_converter_root_preflight'](
        state.handle, data.length);
      if (allowed < 0) return false;
    }
    if (kind === 1) {
      let keyBlock = 0;
      ++state.busy;
      try {
        keyBlock = Module['_lightusd_next_alloc'](Math.max(nameBytes.length, 1));
        if (!keyBlock) throw new RangeError(method + ': name allocation failed');
        Module.HEAPU8.set(nameBytes, Number(keyBlock));
        const call = ptr => Module['_lightusd_next_converter_asset_preflight'](
          state.handle, ptr, nameBytes.length, data.length);
        let allowed;
        try { allowed = call(keyBlock); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          allowed = call(BigInt(keyBlock));
        }
        if (allowed < 0) return false;
      } finally {
        if (keyBlock) Module['_lightusd_next_free'](keyBlock);
        --state.busy;
      }
    }
    const total = Math.max(nameBytes.length + data.length, 1);
    let block = 0;
    ++state.busy;
    try {
      block = Module['_lightusd_next_alloc'](total);
      if (!block) throw new RangeError(method + ': allocation failed');
      const base = Number(block);
      if (heapSource) {
        data = new Uint8Array(Module.HEAPU8.buffer, heapSourceOffset, heapSourceLength);
      }
      Module.HEAPU8.set(nameBytes, base);
      Module.HEAPU8.set(data, base + nameBytes.length);
      const pointer = offset => typeof block === 'bigint' ? block + BigInt(offset)
                                                           : block + offset;
      const call = p => Module['_lightusd_next_converter_set_bytes'](
        state.handle, kind, p(0), nameBytes.length, p(nameBytes.length), data.length);
      let result;
      try { result = call(pointer); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        result = call(offset => BigInt(pointer(offset)));
      }
      if (result < 0) throw new RangeError(method + ': invalid input or handle');
      return result === 1;
    } finally {
      if (block) Module['_lightusd_next_free'](block);
      --state.busy;
    }
  }
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'loadFromBinary', {value: function(bytes, filename) {
    converterState(this);
    if (arguments.length !== 2) throw new TypeError('loadFromBinary: wrong argument count');
    if (typeof filename !== 'string') throw new TypeError('loadFromBinary: expected filename');
    return converterSetBytes(this, 0, filename,
      converterByteView(bytes, 'loadFromBinary', false), 'loadFromBinary');
  }});
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'createSampleScene', {
    value: function() {
      converterState(this);
      if (arguments.length !== 0) throw new TypeError('createSampleScene: wrong argument count');
      const scene = `#usda 1.0
(
    defaultPrim = "root"
    upAxis = "Y"
)
def Xform "root"
{
    def Mesh "quad"
    {
        point3f[] points = [(-0.5, 0, -0.5), (0.5, 0, -0.5), (0.5, 0, 0.5), (-0.5, 0, 0.5)]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)] (
            interpolation = "vertex"
        )
        int[] faceVertexCounts = [3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </root/mat>
    }
    def Material "mat"
    {
        token outputs:surface.connect = </root/mat/PBRShader.outputs:surface>
        def Shader "PBRShader"
        {
            uniform token info:id = "UsdPreviewSurface"
            float inputs:metallic = 0
            float inputs:roughness = 0.5
            color3f inputs:diffuseColor.connect = </root/mat/diffuseTexture.outputs:rgb>
            token outputs:surface
        }
        def Shader "stReader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "st"
            float2 outputs:result
        }
        def Shader "diffuseTexture"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @textures/checkerboard.png@
            float2 inputs:st.connect = </root/mat/stReader.outputs:result>
            float3 outputs:rgb
        }
    }
}
`;
      return this.loadFromBinary(converterEncoder.encode(scene), 'sample-scene.usda');
    }
  });
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'setAsset', {value: function(name, bytes) {
    converterState(this);
    if (arguments.length !== 2) throw new TypeError('setAsset: wrong argument count');
    if (typeof name !== 'string') throw new TypeError('setAsset: expected name');
    return converterSetBytes(this, 1, name,
      converterByteView(bytes, 'setAsset', false), 'setAsset');
  }});
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'setMaxAssetBytes', {value: function(limit) {
    const state = converterState(this);
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
      throw new RangeError('setMaxAssetBytes: expected an integer from 1 through 1 GiB');
    }
    const status = Module['_lightusd_next_converter_control'](state.handle, 2, limit, 0);
    if (status !== 0) return {success: false, error: this.error()};
    return {success: true};
  }});
  for (const [name, field] of [['assetBytes', 0], ['maxAssetBytes', 1]]) {
    Object.defineProperty(Module.NextUSDZConverterNative.prototype, name, {value: function() {
      const state = converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      const value = Module['_lightusd_next_converter_control'](state.handle, 3, field, 0);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    }});
  }
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'setMaxMeshBytes', {value: function(limit) {
    const state = converterState(this);
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
      throw new RangeError('setMaxMeshBytes: expected an integer from 1 through 1 GiB');
    }
    const status = Module['_lightusd_next_converter_control'](state.handle, 4, limit, 0);
    return status === 0 ? {success: true} : {success: false, error: this.error()};
  }});
  for (const [name, field] of [['meshBytes', 0], ['maxMeshBytes', 1]]) {
    Object.defineProperty(Module.NextUSDZConverterNative.prototype, name, {value: function() {
      const state = converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      const value = Module['_lightusd_next_converter_control'](state.handle, 5, field, 0);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    }});
  }
  Object.defineProperty(Module.NextUSDZConverterNative.prototype,
    'setMaxRetainedPayloadBytes', {value: function(limit) {
      const state = converterState(this);
      if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
        throw new RangeError('setMaxRetainedPayloadBytes: expected an integer from 1 through 1 GiB');
      }
      const status = Module['_lightusd_next_converter_control'](state.handle, 6, limit, 0);
      return status === 0 ? {success: true} : {success: false, error: this.error()};
    }});
  for (const [name, field] of [['retainedPayloadBytes', 0],
                                ['maxRetainedPayloadBytes', 1]]) {
    Object.defineProperty(Module.NextUSDZConverterNative.prototype, name, {value: function() {
      const state = converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      const value = Module['_lightusd_next_converter_control'](state.handle, 7, field, 0);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    }});
  }
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'createURDFPhysicsScene', {value: function(json) {
    converterState(this);
    if (arguments.length !== 1) throw new TypeError('createURDFPhysicsScene: wrong argument count');
    if (typeof json !== 'string') throw new TypeError('createURDFPhysicsScene: expected string');
    return converterSetBytes(this, 2, '', converterEncoder.encode(json), 'createURDFPhysicsScene');
  }});
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'clearURDFMeshBuffers', {value: function() {
    const state = converterState(this);
    if (arguments.length !== 0) throw new TypeError('clearURDFMeshBuffers: wrong argument count');
    if (Module['_lightusd_next_converter_control'](state.handle, 0, 0, 0) !== 0) {
      throw new TypeError('Invalid LightUSD receiver');
    }
  }});
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'setUSDCExportLimitMB', {value: function(fileMB, memoryMB) {
    const state = converterState(this);
    if (arguments.length !== 2) throw new TypeError('setUSDCExportLimitMB: wrong argument count');
    if (typeof fileMB !== 'number' || typeof memoryMB !== 'number') {
      throw new TypeError('setUSDCExportLimitMB: expected numbers');
    }
    const status = Module['_lightusd_next_converter_control'](
      state.handle, 1, fileMB | 0, memoryMB | 0);
    if (status !== 0) {
      throw new TypeError('Invalid LightUSD receiver');
    }
  }});
  function converterSetMesh(self, kind, name, positions, normals, uvs, indices, method) {
    const state = converterState(self);
    if (typeof name !== 'string') throw new TypeError(method + ': expected name');
    const typed = (value, type, label, optional) => {
      if (optional && value == null) return new type(0);
      if (!(value instanceof type)) throw new TypeError(method + ': expected ' + label);
      return value;
    };
    if (indices != null && !(indices instanceof Uint32Array) &&
        !(indices instanceof Int32Array)) {
      throw new TypeError(method + ': expected indices');
    }
    const meshIndices = indices == null ? new Uint32Array(0)
      : indices;
    const arrays = [typed(positions, Float32Array, 'positions', false),
      typed(normals, Float32Array, 'normals', true),
      typed(uvs, Float32Array, 'uvs', true),
      meshIndices];
    const nameBytes = converterEncoder.encode(name);
    if (nameBytes.length > (1 << 20)) throw new RangeError(method + ': name exceeds 1 MiB');
    let preflightBlock = 0;
    ++state.busy;
    let preflight;
    try {
      preflightBlock = Module['_lightusd_next_alloc'](Math.max(nameBytes.length, 1));
      if (!preflightBlock) throw new RangeError(method + ': allocation failed');
      const preBase = Number(preflightBlock);
      Module.HEAPU8.set(nameBytes, preBase);
      const prePointer = typeof preflightBlock === 'bigint' ? preflightBlock : preBase;
      preflight = Module['_lightusd_next_converter_mesh_preflight'](
        state.handle, prePointer, nameBytes.length,
        arrays[0].length, arrays[1].length, arrays[2].length, arrays[3].length);
      if (preflight < 0) throw new RangeError(method + ': invalid input or handle');
      if (preflight === 0) return false;
    } finally {
      if (preflightBlock) Module['_lightusd_next_free'](preflightBlock);
      --state.busy;
    }
    const offsets = [];
    const sourceOffsets = arrays.map(array => array.buffer === Module.HEAPU8.buffer
      ? array.byteOffset : -1);
    const sourceLengths = arrays.map(array => array.length);
    let total = 0;
    for (const array of arrays) {
      offsets.push(total);
      total += array.byteLength;
      if (total > (1 << 30)) throw new RangeError(method + ': mesh exceeds 1 GiB');
    }
    const nameOffset = total;
    total += nameBytes.length;
    if (total > (1 << 30)) throw new RangeError(method + ': mesh exceeds 1 GiB');
    let block = 0;
    ++state.busy;
    try {
      block = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!block) throw new RangeError(method + ': allocation failed');
      const base = Number(block);
      for (let i = 0; i < arrays.length; ++i) {
        let source = arrays[i];
        if (sourceOffsets[i] >= 0) {
          source = new source.constructor(Module.HEAPU8.buffer,
            sourceOffsets[i], sourceLengths[i]);
        }
        Module.HEAPU8.set(new Uint8Array(source.buffer,
          source.byteOffset, source.byteLength), base + offsets[i]);
      }
      Module.HEAPU8.set(nameBytes, base + nameOffset);
      const pointer = offset => typeof block === 'bigint' ? block + BigInt(offset)
                                                           : block + offset;
      const call = p => Module['_lightusd_next_converter_set_mesh'](
        state.handle, kind, p(nameOffset), nameBytes.length,
        p(offsets[0]), arrays[0].length, p(offsets[1]), arrays[1].length,
        p(offsets[2]), arrays[2].length, p(offsets[3]), arrays[3].length);
      let result;
      try { result = call(pointer); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        result = call(offset => BigInt(pointer(offset)));
      }
      if (result < 0) throw new RangeError(method + ': invalid input or handle');
      return result === 1;
    } finally {
      if (block) Module['_lightusd_next_free'](block);
      --state.busy;
    }
  }
  for (const [name, kind] of [['setVisualMesh', 0], ['setCollisionMesh', 1]]) {
    Object.defineProperty(Module.NextUSDZConverterNative.prototype, name, {value: function(meshName, positions, normals, uvs, indices) {
      converterState(this);
      if (arguments.length !== 5) throw new TypeError(name + ': wrong argument count');
      return converterSetMesh(this, kind, meshName, positions, normals, uvs, indices, name);
    }});
  }
  function converterExport(self, kind, method, rootFormat, remapBytes = null) {
    const state = converterState(self);
    let remapPtr = 0;
    ++state.busy;
    try {
      let status;
      if (remapBytes) {
        remapPtr = Module['_lightusd_next_alloc'](Math.max(remapBytes.length, 1));
        if (!remapPtr) throw new RangeError(method + ': remap allocation failed');
        Module.HEAPU8.set(remapBytes, Number(remapPtr));
        const call = ptr => Module['_lightusd_next_converter_export_with_remap'](
          state.handle, ptr, remapBytes.length, rootFormat);
        try { status = call(remapPtr); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          status = call(BigInt(remapPtr));
        }
      } else {
        status = rootFormat == null
          ? Module['_lightusd_next_converter_export'](state.handle, kind)
          : Module['_lightusd_next_converter_export_with_options'](
              state.handle, kind, rootFormat);
      }
      if (status < 0) throw new RangeError(method + ': invalid handle');
      if (status === 0) return kind <= 1 ? '' : null;
      const copy = (ptr, cap) => {
        try { return Module['_lightusd_next_converter_export_buffer'](state.handle, ptr, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_converter_export_buffer'](state.handle, BigInt(ptr), cap);
        }
      };
      const size = copy(0, 0);
      if (size < 0) throw new RangeError(method + ': output query failed');
      if (!size) return kind <= 1 ? '' : new Uint8Array(0);
      const pointer = Module['_lightusd_next_converter_export_data'](state.handle);
      if (!pointer) throw new RangeError(method + ': output data unavailable');
      const start = Number(pointer);
      const end = start + size;
      if (!Number.isSafeInteger(start) || end > Module.HEAPU8.byteLength) {
        throw new RangeError(method + ': output span is outside WASM memory');
      }
      const bytes = Module.HEAPU8.slice(start, end);
      return kind <= 1 ? converterDecoder.decode(bytes) : bytes;
    } finally {
      if (remapPtr) Module['_lightusd_next_free'](remapPtr);
      --state.busy;
    }
  }
  function converterRemapBytes(remap, method) {
    if (remap == null || typeof remap !== 'object' || Array.isArray(remap)) {
      throw new TypeError(method + ': expected an asset path remap object');
    }
    const normalized = {};
    for (const key of Object.keys(remap)) {
      if (!key || typeof remap[key] !== 'string' || !remap[key]) {
        throw new TypeError(method + ': remap keys and values must be non-empty paths');
      }
      normalized[key] = remap[key];
    }
    const bytes = converterEncoder.encode(JSON.stringify(normalized));
    if (bytes.length > 0x20000000) throw new RangeError(method + ': remap exceeds 512 MiB');
    return bytes;
  }
  for (const [name, kind] of [['extractPhysicsSceneJSON', 0], ['exportAsUSDA', 1],
                              ['exportAsUSDC', 2], ['exportAsUSDZ', 3]]) {
    Object.defineProperty(Module.NextUSDZConverterNative.prototype, name, {value: function() {
      converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      return converterExport(this, kind, name);
    }});
  }
  Object.defineProperty(Module.NextUSDZConverterNative.prototype,
    'exportAsUSDZWithRemap', {value: function(remap) {
      converterState(this);
      if (arguments.length !== 1) throw new TypeError('exportAsUSDZWithRemap: expected one remap object');
      return converterExport(this, 3, 'exportAsUSDZWithRemap', 0,
        converterRemapBytes(remap, 'exportAsUSDZWithRemap'));
    }});
  Object.defineProperty(Module.NextUSDZConverterNative.prototype,
    'exportAsUSDZWithOptions', {value: function(remapOrOptions = {}, options) {
      converterState(this);
      if (arguments.length > 2) {
        throw new TypeError('exportAsUSDZWithOptions: expected an options object');
      }
      const hasRemap = arguments.length === 2;
      const exportOptions = hasRemap ? options : remapOrOptions;
      if (exportOptions == null || typeof exportOptions !== 'object' ||
          Array.isArray(exportOptions)) {
        throw new TypeError('exportAsUSDZWithOptions: expected an options object');
      }
      const format = exportOptions.rootLayerFormat == null ? 'usdc' : exportOptions.rootLayerFormat;
      if (format !== 'usdc' && format !== 'usda') {
        throw new TypeError('exportAsUSDZWithOptions: rootLayerFormat must be usdc or usda');
      }
      return converterExport(this, 3, 'exportAsUSDZWithOptions', format === 'usda' ? 1 : 0,
        hasRemap ? converterRemapBytes(remapOrOptions, 'exportAsUSDZWithOptions') : null);
    }});
  function converterString(self, kind, method) {
    const state = live.get(self);
    if (!state?.handle || state.kind !== 1) throw new TypeError('Invalid LightUSD receiver');
    let buffer = 0;
    try {
      const call = (ptr, cap) => {
        try { return Module['_lightusd_next_converter_string'](state.handle, kind, ptr, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_converter_string'](state.handle, kind, BigInt(ptr), cap);
        }
      };
      const size = call(0, 0);
      if (size < 0) throw new RangeError(method + ': string query failed');
      if (!size) return '';
      buffer = Module['_lightusd_next_alloc'](size);
      if (!buffer) throw new RangeError(method + ': allocation failed');
      if (call(buffer, size) !== size) throw new RangeError(method + ': string changed');
      return converterDecoder.decode(Module.HEAPU8.subarray(Number(buffer), Number(buffer) + size));
    } finally { if (buffer) Module['_lightusd_next_free'](buffer); }
  }
  for (const [name, kind] of [['error', 0], ['warn', 1]]) {
    Object.defineProperty(Module.NextUSDZConverterNative.prototype, name, {value: function() {
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      return converterString(this, kind, name);
    }});
  }
  for (const [name, apply] of [['describeUDIM', 0], ['applyUDIM', 1]]) {
    Object.defineProperty(Module.NextUSDZConverterNative.prototype, name, {value: function(request) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 1) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== apply) throw new TypeError(name + ': wrong argument count');
      const bytes = converterEncoder.encode(apply ? JSON.stringify(request) : '');
      if (bytes.length > 4 * 1024 * 1024) throw new RangeError(name + ': edit plans exceed 4 MiB');
      let input = 0, output = 0;
      ++state.busy;
      try {
        input = Module['_lightusd_next_alloc'](Math.max(1, bytes.length));
        if (!input) throw new RangeError(name + ': allocation failed');
        Module.HEAPU8.set(bytes, Number(input));
        const size = Module['_lightusd_next_converter_udim'](state.handle, apply, input, bytes.length);
        if (size < 0) throw new RangeError(name + ': layer query failed');
        output = Module['_lightusd_next_alloc'](Math.max(1, size));
        if (!output) throw new RangeError(name + ': allocation failed');
        if (Module['_lightusd_next_converter_udim_copy'](state.handle, output, size) !== size) throw new RangeError(name + ': output changed');
        return JSON.parse(converterDecoder.decode(Module.HEAPU8.subarray(Number(output), Number(output) + size)));
      } finally {
        if (output) Module['_lightusd_next_free'](output);
        if (input) Module['_lightusd_next_free'](input);
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.NextUSDZConverterNative.prototype, 'rewriteRoot', {value: function(
      bytes, filename, options) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 1) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 3) throw new TypeError('rewriteRoot: wrong argument count');
    if (!ArrayBuffer.isView(bytes)) throw new TypeError('rewriteRoot: expected byte view');
    if (typeof filename !== 'string') throw new TypeError('rewriteRoot: expected filename');
    let source = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (source.length > 0x40000000) throw new RangeError('rewriteRoot: input exceeds 1 GiB');
    if (source.buffer === Module.HEAPU8.buffer) source = source.slice();
    const format = String(options?.rootLayerFormat ?? 'usdc').toLowerCase() === 'usda' ? 1 : 0;
    const maxMemory = Number(options?.maxMemory ?? 0);
    if (!Number.isSafeInteger(maxMemory) || maxMemory < 0) {
      throw new RangeError('rewriteRoot: invalid maxMemory');
    }
    const total = 64 + source.length;
    if (total > 0xffffffff) throw new RangeError('rewriteRoot: input too large');
    let input = 0, output = 0;
    ++state.busy;
    try {
      input = Module['_lightusd_next_alloc'](total);
      if (!input) throw new RangeError('rewriteRoot: allocation failed');
      const base = Number(input);
      const pod = new DataView(Module.HEAPU8.buffer, base, 64);
      pod.setUint32(0, 24, true);
      pod.setUint32(4, format, true);
      pod.setFloat64(8, maxMemory, true);
      pod.setUint32(16, options?.usdaLazy == null ? 1 : !!options.usdaLazy, true);
      pod.setUint32(24, 40, true);
      Module.HEAPU8.set(source, base + 64);
      const pointer = offset => typeof input === 'bigint' ? input + BigInt(offset)
                                                           : input + offset;
      const call = p => Module['_lightusd_next_converter_rewrite'](
        state.handle, p(64), source.length, p(0), p(24));
      let status;
      try { status = call(pointer); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = call(offset => BigInt(pointer(offset)));
      }
      if (status < 0) throw new RangeError('rewriteRoot: invalid input or handle');
      if (status === 0) return {success: false, error: this.error()};
      const info = new DataView(Module.HEAPU8.buffer, Number(input) + 24, 40);
      const size = info.getUint32(8, true);
      const result = {
        success: true, sourcePath: filename,
        rootName: format ? 'root.usda' : 'root.usdc',
        rootLayerFormat: format ? 'usda' : 'usdc', size
      };
      if (!format) {
        result.tokenCount = info.getFloat64(16, true);
        result.pathCount = info.getFloat64(24, true);
        result.specCount = info.getFloat64(32, true);
      }
      if (size) {
        output = Module['_lightusd_next_alloc'](size);
        if (!output) throw new RangeError('rewriteRoot: output allocation failed');
        const copy = ptr => Module['_lightusd_next_converter_rewrite_buffer'](
          state.handle, ptr, size);
        let copied;
        try { copied = copy(output); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          copied = copy(BigInt(output));
        }
        if (copied !== size) throw new RangeError('rewriteRoot: output changed');
        result.data = Module.HEAPU8.slice(Number(output), Number(output) + size);
      } else result.data = new Uint8Array(0);
      return result;
    } finally {
      if (output) Module['_lightusd_next_free'](output);
      if (input) Module['_lightusd_next_free'](input);
      --state.busy;
    }
  }});
  defineClass('SubdivStreamer', 2);
  defineClass('NextFlattenSession', 3);
  defineClass('RenderStream', 4);

  Module['MeshoptSimplifier'] = class MeshoptSimplifier {
    #deleted = false;
    delete() {
      if (this.#deleted) throw new TypeError('MeshoptSimplifier already deleted');
      this.#deleted = true;
    }
    isDeleted() { return this.#deleted; }
    simplify(positions, indices, normals, uvs, locks, targetCount, targetError, options) {
      if (this.#deleted) throw new TypeError('MeshoptSimplifier already deleted');
      if (arguments.length !== 8) throw new TypeError('simplify: wrong argument count');
      const typed = (value, type, name, optional) => {
        if (optional && value == null) return new type(0);
        if (!(value instanceof type)) throw new TypeError('simplify: expected ' + name);
        return value.buffer === Module.HEAPU8.buffer ? value.slice() : value;
      };
      const arrays = [typed(positions, Float32Array, 'positions', false),
        typed(indices, Uint32Array, 'indices', false),
        typed(normals, Float32Array, 'normals', true),
        typed(uvs, Float32Array, 'uvs', true),
        typed(locks, Uint8Array, 'locks', true)];
      if (!Number.isSafeInteger(targetCount) || targetCount < 0 ||
          !Number.isFinite(targetError) || !Number.isSafeInteger(options)) {
        return undefined;
      }
      const offsets = [];
      let total = 0;
      for (const array of arrays) {
        offsets.push(total);
        total += array.byteLength;
        if (total > (1 << 30)) throw new RangeError('simplify: input exceeds 1 GiB');
      }
      const outputOffset = (total + 3) & ~3;
      const errorOffset = outputOffset + arrays[1].byteLength;
      total = errorOffset + 4;
      if (total > (1 << 30)) throw new RangeError('simplify: buffers exceed 1 GiB');
      let block = 0;
      try {
        block = Module['_lightusd_next_alloc'](total);
        if (!block) throw new RangeError('simplify: allocation failed');
        const base = Number(block);
        for (let i = 0; i < arrays.length; ++i) {
          Module.HEAPU8.set(new Uint8Array(arrays[i].buffer,
            arrays[i].byteOffset, arrays[i].byteLength), base + offsets[i]);
        }
        const pointer = offset => typeof block === 'bigint' ? block + BigInt(offset)
                                                             : block + offset;
        const call = p => Module['_lightusd_meshopt_simplify'](
          p(offsets[0]), arrays[0].length, p(offsets[1]), arrays[1].length,
          p(offsets[2]), arrays[2].length, p(offsets[3]), arrays[3].length,
          p(offsets[4]), arrays[4].length, targetCount, targetError,
          options >>> 0, p(outputOffset), arrays[1].length, p(errorOffset));
        let count;
        try { count = call(pointer); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          count = call(offset => BigInt(pointer(offset)));
        }
        if (count < 0) return undefined;
        const resultError = new DataView(Module.HEAPU8.buffer).getFloat32(
          base + errorOffset, true);
        const output = new Uint32Array(Module.HEAPU8.buffer,
          base + outputOffset, count).slice();
        return {indices: output, error: resultError,
          sourceVertexCount: arrays[0].length / 3, vertexCacheOptimized: true};
      } finally { if (block) Module['_lightusd_next_free'](block); }
    }
  };

  function lrtArray(value, type, name) {
    if (!(value instanceof type)) throw new TypeError(name + ': expected ' + type.name);
    return value.buffer === Module.HEAPU8.buffer ? value.slice() : value;
  }
  function withLrtBuffers(inputs, outputSizes, callback) {
    const inputOffsets = [], outputOffsets = [];
    let total = 0;
    for (const source of inputs) {
      total = Math.ceil(total / 4) * 4;
      inputOffsets.push(total);
      total += source.byteLength;
      if (total > (1 << 30)) throw new RangeError('LightRT buffers exceed 1 GiB');
    }
    for (const size of outputSizes) {
      total = Math.ceil(total / 4) * 4;
      outputOffsets.push(total);
      total += size;
      if (total > (1 << 30)) throw new RangeError('LightRT buffers exceed 1 GiB');
    }
    let block = 0;
    try {
      block = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!block) throw new RangeError('LightRT allocation failed');
      const base = Number(block);
      for (let i = 0; i < inputs.length; ++i) {
        const source = inputs[i];
        Module.HEAPU8.set(new Uint8Array(source.buffer,
          source.byteOffset, source.byteLength), base + inputOffsets[i]);
      }
      const pointer = offset => typeof block === 'bigint' ? block + BigInt(offset)
                                                           : block + offset;
      const call = fn => {
        try { return fn(pointer); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return fn(offset => BigInt(pointer(offset)));
        }
      };
      return callback({base, inputOffsets, outputOffsets, call});
    } finally { if (block) Module['_lightusd_next_free'](block); }
  }
  Module['LightRTPathTracer'] = class LightRTPathTracer {
    #handle;
    constructor() {
      this.#handle = Module['_lightusd_lrt_create']() >>> 0;
      if (!this.#handle) throw new RangeError('LightRT allocation failed');
    }
    #active() {
      if (!this.#handle) throw new TypeError('LightRTPathTracer already deleted');
      return this.#handle;
    }
    delete() {
      Module['_lightusd_lrt_destroy'](this.#active());
      this.#handle = 0;
    }
    isDeleted() { return !this.#handle; }
    clear() {
      if (arguments.length !== 0) throw new TypeError('clear: wrong argument count');
      if (Module['_lightusd_lrt_clear'](this.#active()) !== 0) {
        throw new TypeError('Invalid LightRT handle');
      }
    }
    build(positions, normals, colors, vertexParams, materialIds, materials) {
      const handle = this.#active();
      if (arguments.length !== 6) throw new TypeError('build: wrong argument count');
      const inputs = [lrtArray(positions, Float32Array, 'build'),
        lrtArray(normals, Float32Array, 'build'),
        lrtArray(colors, Float32Array, 'build'),
        lrtArray(vertexParams, Float32Array, 'build'),
        lrtArray(materialIds, Int32Array, 'build'),
        lrtArray(materials, Float32Array, 'build')];
      return withLrtBuffers(inputs, [], ({inputOffsets: o, call}) => {
        const status = call(p => Module['_lightusd_lrt_build'](
          handle, p(o[0]), inputs[0].length, p(o[1]), inputs[1].length,
          p(o[2]), inputs[2].length, p(o[3]), inputs[3].length,
          p(o[4]), inputs[4].length, p(o[5]), inputs[5].length));
        if (status < 0) throw new RangeError('build: invalid input or handle');
        return status === 1;
      });
    }
    error() {
      const handle = this.#active();
      if (arguments.length !== 0) throw new TypeError('error: wrong argument count');
      let buffer = 0;
      try {
        const query = (ptr, cap) => {
          try { return Module['_lightusd_lrt_error'](handle, ptr, cap); }
          catch (error) {
            if (!(error instanceof TypeError)) throw error;
            return Module['_lightusd_lrt_error'](handle, BigInt(ptr), cap);
          }
        };
        const size = query(0, 0);
        if (size < 0) throw new TypeError('Invalid LightRT handle');
        if (!size) return '';
        buffer = Module['_lightusd_next_alloc'](size);
        if (!buffer) throw new RangeError('error: allocation failed');
        if (query(buffer, size) !== size) throw new RangeError('error: text changed');
        return new TextDecoder().decode(Module.HEAPU8.subarray(
          Number(buffer), Number(buffer) + size));
      } finally { if (buffer) Module['_lightusd_next_free'](buffer); }
    }
    triangleCount() {
      if (arguments.length !== 0) throw new TypeError('triangleCount: wrong argument count');
      const count = Module['_lightusd_lrt_triangle_count'](this.#active());
      if (count < 0) throw new TypeError('Invalid LightRT handle');
      return count;
    }
    trace(inverse, camera, width, height, sampleStart, sampleCount, maxBounces, exposure) {
      const handle = this.#active();
      if (arguments.length !== 8) throw new TypeError('trace: wrong argument count');
      const inputs = [lrtArray(inverse, Float32Array, 'trace'),
        lrtArray(camera, Float32Array, 'trace')];
      if (width < 1 || height < 1 || width > 4096 || height > 4096 ||
          !Number.isInteger(width) || !Number.isInteger(height) ||
          inputs[0].length !== 16 || inputs[1].length !== 3) return undefined;
      const count = width * height * 4;
      return withLrtBuffers(inputs, [count * 4],
        ({base, inputOffsets: i, outputOffsets: o, call}) => {
          const status = call(p => Module['_lightusd_lrt_trace'](
            handle, p(i[0]), inputs[0].length, p(i[1]), inputs[1].length,
            width, height, sampleStart, sampleCount, maxBounces, exposure,
            p(o[0]), count));
          if (status < 0) throw new TypeError('Invalid LightRT handle');
          return status === 1
            ? new Float32Array(Module.HEAPU8.buffer, base + o[0], count).slice()
            : undefined;
        });
    }
    occluded(origins, directions, maxDistance = 1e30) {
      const handle = this.#active();
      if (arguments.length < 2 || arguments.length > 3) throw new TypeError('occluded: wrong argument count');
      const inputs = [lrtArray(origins, Float32Array, 'occluded'),
        lrtArray(directions, Float32Array, 'occluded')];
      if (!inputs[0].length || inputs[0].length !== inputs[1].length ||
          inputs[0].length % 3 || inputs[0].length > (1 << 26)) return undefined;
      const count = inputs[0].length / 3;
      return withLrtBuffers(inputs, [count],
        ({base, inputOffsets: i, outputOffsets: o, call}) => {
          const status = call(p => Module['_lightusd_lrt_occluded'](
            handle, p(i[0]), inputs[0].length, p(i[1]), inputs[1].length,
            maxDistance, p(o[0]), count));
          if (status < 0) throw new TypeError('Invalid LightRT handle');
          return status === 1
            ? new Uint8Array(Module.HEAPU8.buffer, base + o[0], count).slice()
            : undefined;
        });
    }
    raycast(origins, directions, maxDistance = 1e30) {
      const handle = this.#active();
      if (arguments.length < 2 || arguments.length > 3) throw new TypeError('raycast: wrong argument count');
      const inputs = [lrtArray(origins, Float32Array, 'raycast'),
        lrtArray(directions, Float32Array, 'raycast')];
      if (!inputs[0].length || inputs[0].length !== inputs[1].length ||
          inputs[0].length % 3 || inputs[0].length > (1 << 26)) return undefined;
      const count = inputs[0].length / 3;
      return withLrtBuffers(inputs, [count * 4, count * 4, count * 12],
        ({base, inputOffsets: i, outputOffsets: o, call}) => {
          const status = call(p => Module['_lightusd_lrt_raycast'](
            handle, p(i[0]), inputs[0].length, p(i[1]), inputs[1].length,
            maxDistance, p(o[0]), p(o[1]), p(o[2]), count));
          if (status < 0) throw new TypeError('Invalid LightRT handle');
          if (status !== 1) return undefined;
          const heap = Module.HEAPU8.buffer;
          return {
            distance: new Float32Array(heap, base + o[0], count).slice(),
            triangle: new Int32Array(heap, base + o[1], count).slice(),
            barycentrics: new Float32Array(heap, base + o[2], count * 3).slice()
          };
        });
    }
    webGPUScene() {
      const handle = this.#active();
      if (arguments.length !== 0) throw new TypeError('webGPUScene: wrong argument count');
      const info = withLrtBuffers([], [48], ({base, outputOffsets: o, call}) => {
        new DataView(Module.HEAPU8.buffer, base + o[0], 48).setUint32(0, 48, true);
        const status = call(p => Module['_lightusd_lrt_scene_info_get'](handle, p(o[0])));
        if (status < 0) throw new TypeError('Invalid LightRT handle');
        if (status !== 1) return undefined;
        const view = new DataView(Module.HEAPU8.buffer, base + o[0], 48);
        return {root: view.getUint32(4, true), nodeCount: view.getUint32(8, true),
          blockCount: view.getUint32(12, true), width: view.getUint32(16, true),
          lengths: Array.from({length: 7}, (_, index) => view.getUint32(20 + index * 4, true))};
      });
      if (!info) return undefined;
      const fields = ['nodes', 'blocks', 'normals', 'colors', 'vertexParams', 'materialIds', 'materials'];
      return withLrtBuffers([], info.lengths.map(length => length * 4),
        ({base, outputOffsets: offsets, call}) => {
        const result = {root: info.root, nodeCount: info.nodeCount,
          blockCount: info.blockCount, width: info.width};
        for (let kind = 0; kind < 7; ++kind) {
          const size = info.lengths[kind] * 4;
          const copied = call(p => Module['_lightusd_lrt_scene_buffer'](
            handle, kind, p(offsets[kind]), size));
          if (copied !== size) throw new RangeError('webGPUScene: buffer changed');
          const bytes = Module.HEAPU8.slice(base + offsets[kind], base + offsets[kind] + size);
          result[fields[kind]] = kind < 2 ? new Uint32Array(bytes.buffer)
            : kind === 5 ? new Int32Array(bytes.buffer) : new Float32Array(bytes.buffer);
        }
        return result;
      });
    }
  };

  const subdivCallbacks = new Map();
  let nextSubdivCallbackId = 1;
  Module['__lightusdNextSubdivEmit'] =
    (id, pos, posCount, nrm, nrmCount, idx, idxCount, fsrc, faceCount,
     uv, uvCount, numVertices, numFaces, batchIndex) => {
      const entry = subdivCallbacks.get(id);
      if (!entry) return 0;
      try {
        const floats = (ptr, count) => new Float32Array(Module.HEAPU8.buffer, ptr, count);
        const ints = (ptr, count) => new Uint32Array(Module.HEAPU8.buffer, ptr, count);
        entry.callback(floats(pos, posCount), nrmCount ? floats(nrm, nrmCount) : null,
          ints(idx, idxCount), ints(fsrc, faceCount),
          uvCount ? floats(uv, uvCount) : null, numVertices, numFaces, batchIndex);
        return 1;
      } catch (error) {
        entry.error = error;
        entry.errorSet = true;
        return 0;
      }
    };
  Object.defineProperty(Module.SubdivStreamer.prototype, 'heapBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 2) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('heapBytes: wrong argument count');
    return Module.HEAPU8.length;
  }});
  Object.defineProperty(Module.SubdivStreamer.prototype, 'refineStream', {value: function(
      points, fvc, fvi, uvValues, uvIndices, uvInterp, scheme, boundary,
      level, batchFaces, blockFaces, haloRings, wantNormals, onBatch) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 2) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 14) throw new TypeError('refineStream: wrong argument count');
    if (typeof onBatch !== 'function') throw new TypeError('refineStream: expected callback');
    const input = (value, type, name) => {
      if (value == null && (name === 'uvValues' || name === 'uvIndices')) return new type(0);
      if (!(value instanceof type)) throw new TypeError(`refineStream: ${name} must be ${type.name}`);
      return value.buffer === Module.HEAPU8.buffer ? value.slice() : value;
    };
    const arrays = [input(points, Float32Array, 'points'),
      input(fvc, Uint32Array, 'fvc'), input(fvi, Uint32Array, 'fvi'),
      input(uvValues, Float32Array, 'uvValues'),
      input(uvIndices, Uint32Array, 'uvIndices')];
    const offsets = [];
    let total = 36;
    for (const array of arrays) {
      offsets.push(total);
      total += array.byteLength;
      if (total > 0x40000000) throw new RangeError('refineStream: input exceeds 1 GiB');
    }
    let callbackId = 0, block = 0;
    do { callbackId = nextSubdivCallbackId++ >>> 0; }
    while (callbackId === 0 || subdivCallbacks.has(callbackId));
    const entry = {callback: onBatch, error: null, errorSet: false};
    subdivCallbacks.set(callbackId, entry);
    ++state.busy;
    try {
      block = Module['_lightusd_next_alloc'](total);
      if (!block) throw new RangeError('refineStream: allocation failed');
      const base = Number(block);
      const options = new DataView(Module.HEAPU8.buffer, base, 36);
      for (const [i, value] of [36, uvInterp, scheme, boundary, level,
        batchFaces, blockFaces, haloRings, !!wantNormals].entries()) {
        options.setInt32(i * 4, Number(value) | 0, true);
      }
      for (let i = 0; i < arrays.length; ++i) {
        Module.HEAPU8.set(new Uint8Array(arrays[i].buffer,
          arrays[i].byteOffset, arrays[i].byteLength), base + offsets[i]);
      }
      const pointer = offset => typeof block === 'bigint' ? block + BigInt(offset)
                                                           : block + offset;
      const call = p => Module['_lightusd_next_subdiv_refine'](
        state.handle, p(offsets[0]), arrays[0].length, p(offsets[1]), arrays[1].length,
        p(offsets[2]), arrays[2].length, arrays[3].length ? p(offsets[3]) : p(0) - p(0),
        arrays[3].length, arrays[4].length ? p(offsets[4]) : p(0) - p(0),
        arrays[4].length, p(0), callbackId);
      let result;
      try { result = call(pointer); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        result = call(offset => BigInt(pointer(offset)));
      }
      if (entry.errorSet) throw entry.error;
      if (result === -1) throw new RangeError('refineStream: invalid input or handle');
      if (result === 0) return '';
      let length;
      try { length = Module['_lightusd_next_subdiv_error'](state.handle, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        length = Module['_lightusd_next_subdiv_error'](state.handle, 0n, 0);
      }
      if (length < 0) throw new RangeError('refineStream: error query failed');
      const message = Module['_lightusd_next_alloc'](length + 1);
      if (!message) throw new RangeError('refineStream: error allocation failed');
      try {
        const read = ptr => Module['_lightusd_next_subdiv_error'](state.handle, ptr, length);
        let size;
        try { size = read(message); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          size = read(BigInt(message));
        }
        if (size !== length) throw new RangeError('refineStream: error changed');
        return new TextDecoder().decode(Module.HEAPU8.subarray(Number(message), Number(message) + length));
      } finally { Module['_lightusd_next_free'](message); }
    } finally {
      if (block) Module['_lightusd_next_free'](block);
      subdivCallbacks.delete(callbackId);
      --state.busy;
    }
  }});

  // Integer scene counts do not need an emval argument/result round trip.
  // Keep the public methods and receiver checks identical while using the
  // typed C boundary underneath.
  const renderCounts = [
    ['meshCount', 0], ['numMeshes', 0],
    ['nodeCount', 1], ['numNodes', 1],
    ['lightCount', 2], ['numLights', 2],
    ['pointsCount', 3], ['numPoints', 3],
    ['curvesCount', 4], ['numCurves', 4],
    ['cameraCount', 5], ['numCameras', 5],
    ['pointInstancerCount', 6], ['numPointInstancers', 6],
    ['pointInstanceDrawCount', 7], ['numPointInstanceDraws', 7],
    ['skeletonCount', 8], ['numSkeletons', 8],
    ['unsupportedRenderableCount', 9], ['numUnsupportedRenderables', 9],
    ['numAnimations', 10],
    ['numImages', 11], ['numMaterials', 12], ['numTextures', 13],
    ['numRootNodes', 14], ['getDefaultRootNodeId', 15],
  ];
  for (const [name, kind] of renderCounts) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function() {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) {
        throw new TypeError('Invalid LightUSD receiver');
      }
      if (arguments.length !== 0) {
        throw new TypeError(name + ': wrong argument count');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_count'](state.handle, kind);
      } finally {
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'getMemoryStats', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getMemoryStats: wrong argument count');
    const counts = kind => Module['_lightusd_next_render_count'](state.handle, kind);
    const numMeshes = counts(0), numNodes = counts(1), numLights = counts(2);
    const numImages = counts(11), numMaterials = counts(12), numTextures = counts(13);
    const values = [numMeshes, numNodes, numLights, numImages, numMaterials, numTextures];
    if (values.some(value => !Number.isSafeInteger(value) || value < 0)) {
      throw new RangeError('getMemoryStats: invalid scene counts');
    }
    const bufferKinds = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    let numBuffers = 0, bufferMemoryBytes = 0;
    const querySize = (meshId, kind) => {
      try {
        return Module['_lightusd_next_render_mesh_buffer'](
          state.handle, meshId, kind, 0, 0);
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module['_lightusd_next_render_mesh_buffer'](
          state.handle, meshId, kind, BigInt(0), 0);
      }
    };
    ++state.busy;
    try {
      for (let meshId = 0; meshId < numMeshes; ++meshId) {
        for (const kind of bufferKinds) {
          const size = querySize(meshId, kind);
          if (!Number.isSafeInteger(size) || size < 0) {
            throw new RangeError('getMemoryStats: invalid mesh buffer size');
          }
          if (size > 0) ++numBuffers;
          bufferMemoryBytes += size;
          if (!Number.isSafeInteger(bufferMemoryBytes)) {
            throw new RangeError('getMemoryStats: buffer size overflow');
          }
        }
      }
    } finally { --state.busy; }
    let assetCacheCount = 0, assetCacheSizeBytes = 0, assetCacheMaxBytes = 0;
    if (state.assetStore) {
      const storeState = live.get(state.assetStore);
      if (storeState?.handle && storeState.kind === 5) {
        const stats = state.assetStore.memoryStats();
        assetCacheCount = stats.assetCount;
        assetCacheSizeBytes = stats.bytesUsed;
        assetCacheMaxBytes = stats.limitBytes;
      }
    }
    return {numMeshes, numMaterials, numTextures, numImages, numBuffers, numNodes,
      numLights, bufferMemoryBytes, bufferMemoryMB: bufferMemoryBytes / (1024 * 1024),
      assetCacheCount, assetCacheSizeBytes, assetCacheMaxBytes,
      // The next renderer does not maintain the legacy reordered-mesh cache.
      reorderedMeshCacheCount: 0};
  }});
  const imageEncoder = new TextEncoder();
  Object.defineProperty(Module.RenderStream.prototype, 'encodeImageNative', {
    value: function(pixels, width, height, channels, format) {
      const name = 'encodeImageNative';
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 5) throw new TypeError(name + ': wrong argument count');
      const integer = value => {
        if (typeof value !== 'number' || !Number.isFinite(value) ||
            value < -2147483648 || value > 2147483647) {
          throw new TypeError(name + ': expected numeric dimensions');
        }
        return Math.trunc(value);
      };
      const w = integer(width), h = integer(height), c = integer(channels);
      const source = converterByteView(pixels, name);
      if (source.length > 256 * 1024 * 1024) {
        throw new RangeError(name + ': input exceeds 256 MiB');
      }
      let formatBytes;
      if (typeof format === 'string') formatBytes = imageEncoder.encode(format);
      else if (ArrayBuffer.isView(format)) formatBytes = converterByteView(format, name);
      else throw new TypeError(name + ': expected format string or byte view');
      const total = source.length + formatBytes.length;
      if (total > 0xffffffff) throw new RangeError(name + ': input too large');
      ++state.busy;
      let input = 0, output = 0;
      let pendingExr = false;
      try {
        input = Module['_lightusd_next_alloc'](Math.max(total, 1));
        if (!input) throw new RangeError(name + ': input allocation failed');
        const base = Number(input);
        Module.HEAPU8.set(source, base);
        Module.HEAPU8.set(formatBytes, base + source.length);
        const pointer = offset => typeof input === 'bigint' ? input + BigInt(offset)
                                                             : input + offset;
        const nullPointer = typeof input === 'bigint' ? 0n : 0;
        const encode = (out, cap) => Module['_lightusd_next_encode_image'](
          pointer(0), source.length, w, h, c,
          pointer(source.length), formatBytes.length, out, cap);
        const size = encode(nullPointer, 0);
        if (size === 2) return {success: false, error: 'Invalid image dimensions.'};
        if (size === -2 || size === 0) return null;
        if (size < 0) throw new RangeError(name + ': image encoding failed');
        const isExr = formatBytes.length === 3 &&
          formatBytes[0] === 101 && formatBytes[1] === 120 && formatBytes[2] === 114;
        if (isExr) {
          pendingExr = true;
          const encoded = Module['_lightusd_next_encoded_image_data']();
          const encodedOffset = Number(encoded);
          if (!Number.isSafeInteger(encodedOffset) || encodedOffset <= 0) {
            throw new RangeError(name + ': EXR result unavailable');
          }
          return Module.HEAPU8.slice(encodedOffset, encodedOffset + size);
        }
        output = Module['_lightusd_next_alloc'](size);
        if (!output) throw new RangeError(name + ': output allocation failed');
        const write = out => Module['_lightusd_next_encode_image'](
          pointer(0), source.length, w, h, c,
          pointer(source.length), formatBytes.length, out, size);
        let written;
        try { written = write(output); }
        catch (error) {
          if (!(error instanceof TypeError) || typeof output === 'bigint') throw error;
          written = write(BigInt(output));
        }
        if (written !== size) throw new RangeError(name + ': output changed');
        return Module.HEAPU8.slice(Number(output), Number(output) + size);
      } finally {
        if (pendingExr) Module['_lightusd_next_encoded_image_release']();
        if (output) Module['_lightusd_next_free'](output);
        if (input) Module['_lightusd_next_free'](input);
        --state.busy;
      }
    }
  });
  const renderFlags = [
    ['setMaterialDedup', 0], ['setMeshMerge', 1],
    ['setMeshMergeBakeTransform', 2], ['setFlattenRenderTree', 3],
    ['setMeshOnly', 4], ['setComputeTangents', 5],
    ['setBuildVertexIndices', 6], ['setEnableComposition', 7],
    ['setEnableValueClips', 8], ['setLoadTextureInNative', 9],
    ['setCombineUDIMTiles', 10],
    ['setDeferTangentComputation', 5, true]
  ];
  for (const [name, kind, inverted = false] of renderFlags) {
    const getterName = name.replace(/^set/, 'get');
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(enabled) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 1) throw new TypeError(name + ': wrong argument count');
      if (typeof enabled !== 'boolean') throw new TypeError(name + ': expected boolean');
      ++state.busy;
      try {
        const value = inverted ? !enabled : enabled;
        if (Module['_lightusd_next_render_set_flag'](state.handle, kind, value ? 1 : 0) !== 0) {
          throw new TypeError('Invalid LightUSD receiver');
        }
      } finally { --state.busy; }
    }});
    Object.defineProperty(Module.RenderStream.prototype, getterName, {value: function() {
      if (arguments.length !== 0) throw new TypeError(getterName + ': wrong argument count');
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      ++state.busy;
      try {
        const value = Module['_lightusd_next_render_flag'](state.handle, kind);
        if (value !== 0 && value !== 1) throw new RangeError(getterName + ': query failed');
        return inverted ? value === 0 : value === 1;
      } finally { --state.busy; }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'setSphereSubdivisions', {value: function(value) {
    if (arguments.length !== 1 || !Number.isInteger(value)) {
      throw new TypeError('setSphereSubdivisions: expected one integer');
    }
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_mesh_setting'](state.handle, 0, value) !== 0) {
        throw new RangeError('setSphereSubdivisions: setting rejected');
      }
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getSphereSubdivisions', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getSphereSubdivisions: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_render_mesh_setting'](state.handle, 0);
      if (value < 0) throw new RangeError('getSphereSubdivisions: query failed');
      return value;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setEnableBoneReduction', {value: function(enabled) {
    if (arguments.length !== 1 || typeof enabled !== 'boolean') {
      throw new TypeError('setEnableBoneReduction: expected boolean');
    }
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_mesh_setting'](state.handle, 1, enabled ? 1 : 0) !== 0) {
        throw new RangeError('setEnableBoneReduction: setting rejected');
      }
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getEnableBoneReduction', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getEnableBoneReduction: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_render_mesh_setting'](state.handle, 1);
      if (value < 0) throw new RangeError('getEnableBoneReduction: query failed');
      return value !== 0;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setTargetBoneCount', {value: function(value) {
    if (arguments.length !== 1 || !Number.isInteger(value)) {
      throw new TypeError('setTargetBoneCount: expected one integer');
    }
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_mesh_setting'](state.handle, 2, value) !== 0) {
        throw new RangeError('setTargetBoneCount: setting rejected');
      }
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getTargetBoneCount', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getTargetBoneCount: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_render_mesh_setting'](state.handle, 2);
      if (value < 0) throw new RangeError('getTargetBoneCount: query failed');
      return value;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setRoundBoneCount', {value: function(enabled) {
    if (arguments.length !== 1 || typeof enabled !== 'boolean') {
      throw new TypeError('setRoundBoneCount: expected boolean');
    }
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_mesh_setting'](state.handle, 3, enabled ? 1 : 0) !== 0) {
        throw new RangeError('setRoundBoneCount: setting rejected');
      }
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getRoundBoneCount', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getRoundBoneCount: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_render_mesh_setting'](state.handle, 3);
      if (value < 0) throw new RangeError('getRoundBoneCount: query failed');
      return value !== 0;
    } finally { --state.busy; }
  }});
  function setValueClipSetting(name, field, value) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_value_clip_setting'](
            state.handle, field, value) !== 0) {
        throw new RangeError(name + ': setting rejected');
      }
    } finally { --state.busy; }
  }
  function getValueClipSetting(field) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_render_value_clip_setting'](state.handle, field);
      if (!Number.isFinite(value)) throw new RangeError('value-clip setting query failed');
      return value;
    } finally { --state.busy; }
  }
  Object.defineProperty(Module.RenderStream.prototype, 'setValueClipSampleRate', {value: function(rate) {
    if (arguments.length !== 1 || typeof rate !== 'number' || !Number.isFinite(rate)) {
      throw new TypeError('setValueClipSampleRate: expected one finite number');
    }
    setValueClipSetting.call(this, 'setValueClipSampleRate', 0, rate);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getValueClipSampleRate', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getValueClipSampleRate: wrong argument count');
    return getValueClipSetting.call(this, 0);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setValueClipUseTimeRange', {value: function(enabled) {
    if (arguments.length !== 1 || typeof enabled !== 'boolean') {
      throw new TypeError('setValueClipUseTimeRange: expected boolean');
    }
    setValueClipSetting.call(this, 'setValueClipUseTimeRange', 1, enabled ? 1 : 0);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getValueClipUseTimeRange', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getValueClipUseTimeRange: wrong argument count');
    return getValueClipSetting.call(this, 1) !== 0;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setValueClipTimeRange', {value: function(start, end) {
    if (arguments.length !== 2 || typeof start !== 'number' || typeof end !== 'number' ||
        !Number.isFinite(start) || !Number.isFinite(end)) {
      throw new TypeError('setValueClipTimeRange: expected two finite numbers');
    }
    setValueClipSetting.call(this, 'setValueClipTimeRange', 2, start);
    setValueClipSetting.call(this, 'setValueClipTimeRange', 3, end);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getValueClipStartTime', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getValueClipStartTime: wrong argument count');
    return getValueClipSetting.call(this, 2);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getValueClipEndTime', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getValueClipEndTime: wrong argument count');
    return getValueClipSetting.call(this, 3);
  }});
  const renderTextEncoder = new TextEncoder();
  function withRenderTexts(state, values, call) {
    const encoded = values.map(value => renderTextEncoder.encode(value));
    const total = encoded.reduce((sum, bytes) => sum + bytes.length, 0);
    if (total > 0xffffffff) throw new RangeError('RenderStream text is too large');
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('RenderStream text allocation failed');
      let offset = Number(ptr);
      for (const bytes of encoded) {
        Module.HEAPU8.set(bytes, offset);
        offset += bytes.length;
      }
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset)
                                                    : ptr + offset;
      const offsets = [];
      let cursor = 0;
      for (const bytes of encoded) {
        offsets.push(pointer(cursor));
        cursor += bytes.length;
      }
      let status;
      try { status = call(offsets, encoded.map(bytes => bytes.length)); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = call(offsets.map(p => BigInt(p)), encoded.map(bytes => bytes.length));
      }
      if (status !== 0) throw new TypeError('Invalid LightUSD receiver');
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }
  const renderTextSettings = [
    ['setRenderSettingsPath', 0], ['setTangentMethod', 1]
  ];
  for (const [name, kind] of renderTextSettings) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(value) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 1) throw new TypeError(name + ': wrong argument count');
      if (typeof value !== 'string') throw new TypeError(name + ': expected string');
      withRenderTexts(state, [value], (p, n) =>
        Module['_lightusd_next_render_set_text'](state.handle, kind, p[0], n[0]));
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'setVariantOverride', {value: function(key, selection) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2) throw new TypeError('setVariantOverride: wrong argument count');
    if (typeof key !== 'string' || typeof selection !== 'string') {
      throw new TypeError('setVariantOverride: expected strings');
    }
    withRenderTexts(state, [key, selection], (p, n) =>
      Module['_lightusd_next_render_set_variant_override'](
        state.handle, p[0], n[0], p[1], n[1]));
  }});
  const renderControls = [
    ['clearAssets', 0], ['clearVariantOverrides', 1], ['end', 2], ['reset', 3],
    ['releaseSourceLayer', 4]
  ];
  for (const [name, kind] of renderControls) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function() {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      ++state.busy;
      try {
        if (Module['_lightusd_next_render_control'](state.handle, kind) !== 0) {
          throw new TypeError('Invalid LightUSD receiver');
        }
      } finally { --state.busy; }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'exportUSDCToBuffer', {value: function(buffer, options) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2) throw new TypeError('exportUSDCToBuffer: expected buffer and options');
    if (buffer === null || buffer === undefined) {
      return {success: false, size: 0, error: 'USDC export output buffer is null.'};
    }
    if (!(buffer instanceof Uint8Array)) {
      return {success: false, size: 0, error: 'USDC export output must be a Uint8Array.'};
    }
    if (buffer.byteLength === 0) {
      return {success: false, size: 0, error: 'USDC export output buffer is empty.'};
    }
    void options;
    const wasmBacked = buffer.buffer === Module.HEAPU8.buffer;
    const byteOffset = buffer.byteOffset;
    const byteLength = buffer.byteLength;
    ++state.busy;
    try {
      const status = Module['_lightusd_next_render_export_stage_usdc'](state.handle);
      if (status < 0) throw new TypeError('exportUSDCToBuffer: invalid render stream');
      if (status === 0) return {success: false, size: 0, error: this.error()};
      const size = Module['_lightusd_next_render_stage_usdc_size'](state.handle);
      if (!Number.isSafeInteger(size) || size < 0) {
        throw new RangeError('exportUSDCToBuffer: invalid USDC output size');
      }
      if (size > byteLength) {
        return {success: false, size: 0, error: 'USDC export output buffer too small.'};
      }
      const data = Module['_lightusd_next_render_stage_usdc_data'](state.handle);
      if (!data && size) throw new RangeError('exportUSDCToBuffer: retained output is missing');
      let target = buffer;
      if (wasmBacked) {
        if (byteOffset > Module.HEAPU8.byteLength || byteLength > Module.HEAPU8.byteLength - byteOffset) {
          throw new RangeError('exportUSDCToBuffer: caller buffer was invalidated by memory growth');
        }
        target = new Uint8Array(Module.HEAPU8.buffer, byteOffset, byteLength);
      }
      if (size) target.set(Module.HEAPU8.subarray(Number(data), Number(data) + size), 0);
      return {success: true, size, warn: ''};
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'debugLogMemory', {value: function(label) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof label !== 'string') {
      throw new TypeError('debugLogMemory: expected one string label');
    }
    const heapBytes = Module.HEAPU8.byteLength;
    if (typeof Module.onLightUSDDebug === 'function') {
      const event = {phase: 'manual', detail: label, heapBytes,
        inputBytes: 0, isUsdz: false, materialsCurrent: 0,
        materialsTotal: 0, materialName: ''};
      const notify = Module['__lightusdLoadingCallback'] ||
        ((name, value) => Module[name](value));
      notify('onLightUSDDebug', event);
    }
    return {label, heapBytes};
  }});
  const flattenEncoder = new TextEncoder();
  const flattenDecoder = new TextDecoder();
  function withFlattenBytes(state, label, data, call, allowNegative = false) {
    const name = flattenEncoder.encode(label);
    let bytes = data;
    if (bytes.buffer === Module.HEAPU8.buffer) bytes = bytes.slice();
    const total = name.length + bytes.length;
    if (total > 0xffffffff) throw new RangeError('Flatten input too large');
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('Flatten input allocation failed');
      Module.HEAPU8.set(name, Number(ptr));
      Module.HEAPU8.set(bytes, Number(ptr) + name.length);
      const dataPtr = typeof ptr === 'bigint' ? ptr + BigInt(name.length)
                                                  : ptr + name.length;
      let status;
      try { status = call(ptr, name.length, dataPtr, bytes.length); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = call(BigInt(ptr), name.length, BigInt(dataPtr), bytes.length);
      }
      if (status < 0 && !allowNegative) throw new TypeError('Invalid LightUSD receiver or input');
      return status;
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }
  function flattenAssetRemapJSON(remap, method) {
    if (!remap || typeof remap !== 'object' || Array.isArray(remap)) {
      throw new TypeError(method + ': expected one object mapping asset paths to strings');
    }
    const normalized = Object.create(null);
    for (const key of Object.keys(remap)) {
      const raw = remap[key];
      if (typeof raw === 'string') normalized[key] = raw;
      else if (ArrayBuffer.isView(raw)) {
        try {
          normalized[key] = new TextDecoder('utf-8', {fatal: true}).decode(
            new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength));
        } catch {
          throw new TypeError(method + ': replacement bytes must contain UTF-8');
        }
      } else throw new TypeError(method + ': each replacement must be a string or byte view');
    }
    return flattenEncoder.encode(JSON.stringify(normalized));
  }
  Object.defineProperty(Module.NextFlattenSession.prototype, 'error', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('error: wrong argument count');
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module['_lightusd_next_flatten_error'](state.handle, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_flatten_error'](state.handle, BigInt(p), cap);
        }
      };
      const size = query(0, 0);
      if (size < 0) throw new TypeError('Invalid LightUSD receiver');
      if (size === 0) return '';
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('error: allocation failed');
      if (query(ptr, size) !== size) throw new RangeError('error: text changed');
      return flattenDecoder.decode(new Uint8Array(
        Module.HEAPU8.buffer, Number(ptr), size));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'setMaxInputBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
      throw new RangeError('setMaxInputBytes: expected an integer from 1 through 1 GiB');
    }
    ++state.busy;
    try {
      const status = Module['_lightusd_next_flatten_set_max_input_bytes'](state.handle, limit);
      return status === 0 ? {success: true} : {success: false, error: this.error()};
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'maxInputBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('maxInputBytes: wrong argument count');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_flatten_max_input_bytes'](state.handle);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'setMaxOutputBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
      throw new RangeError('setMaxOutputBytes: expected an integer from 1 through 1 GiB');
    }
    ++state.busy;
    try {
      const status = Module['_lightusd_next_flatten_set_max_output_bytes'](state.handle, limit);
      return status === 0 ? {success: true} : {success: false, error: this.error()};
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'maxOutputBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('maxOutputBytes: wrong argument count');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_flatten_max_output_bytes'](state.handle);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'inputBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('inputBytes: wrong argument count');
    ++state.busy;
    try {
      const value = Module['_lightusd_next_flatten_input_bytes'](state.handle);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'begin', {value: function(root, name, lazyArrays) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 3) throw new TypeError('begin: wrong argument count');
    if (!ArrayBuffer.isView(root)) throw new TypeError('begin: expected byte view');
    if (typeof name !== 'string') throw new TypeError('begin: expected string name');
    const bytes = new Uint8Array(root.buffer, root.byteOffset, root.byteLength);
    if (bytes.length > 0x40000000) throw new RangeError('begin: root exceeds 1 GiB limit');
    if (bytes.length > this.maxInputBytes()) {
      return {success: false, error: 'Root exceeds configured aggregate input byte limit'};
    }
    const status = withFlattenBytes(state, name, bytes, (namePtr, nameSize, dataPtr, dataSize) =>
      Module['_lightusd_next_flatten_begin'](
        state.handle, dataPtr, dataSize, namePtr, nameSize, lazyArrays ? 1 : 0));
    return status ? {success: true, status: 'ready'}
                  : {success: false, error: this.error()};
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'provideLayer', {value: function(key, data) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2) throw new TypeError('provideLayer: wrong argument count');
    if (typeof key !== 'string') throw new TypeError('provideLayer: expected string key');
    if (!ArrayBuffer.isView(data)) throw new TypeError('provideLayer: expected byte view');
    const bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (bytes.length > 0x40000000) throw new RangeError('provideLayer: layer exceeds 1 GiB limit');
    let preflight = -1;
    withFlattenBytes(state, key, new Uint8Array(0), (keyPtr, keySize) => {
      preflight = Module['_lightusd_next_flatten_preflight_layer'](
        state.handle, keyPtr, keySize, bytes.length);
      return 0;
    });
    if (preflight < 0) return {success: false, error: this.error()};
    const status = withFlattenBytes(state, key, bytes, (keyPtr, keySize, dataPtr, dataSize) =>
      Module['_lightusd_next_flatten_provide_layer'](
        state.handle, keyPtr, keySize, dataPtr, dataSize));
    return status ? {success: true} : {success: false, error: this.error()};
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'addSublayer', {value: function(path) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof path !== 'string' || path.length === 0) {
      throw new TypeError('addSublayer: expected one non-empty path');
    }
    if (flattenEncoder.encode(path).length > (1 << 20)) {
      throw new RangeError('addSublayer: path exceeds 1 MiB');
    }
    const status = withFlattenBytes(state, path, new Uint8Array(0),
      (pathPtr, pathSize) => Module['_lightusd_next_flatten_add_sublayer'](
        state.handle, pathPtr, pathSize));
    return status === 1 ? {success: true} : {success: false, error: this.error()};
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'addPrimArc', {value: function(kind, primPath, assetPath, targetPath, listOp = 'explicit') {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    const listOps = {explicit: 0, add: 1, prepend: 2, append: 3, delete: 4, reorder: 5};
    if ((arguments.length !== 4 && arguments.length !== 5) ||
        !Number.isInteger(kind) || kind < 0 || kind > 3 ||
        typeof primPath !== 'string' || typeof assetPath !== 'string' ||
        typeof targetPath !== 'string' || primPath.length === 0 || targetPath.length === 0 ||
        !Object.prototype.hasOwnProperty.call(listOps, listOp)) {
      throw new TypeError('addPrimArc: expected kind, prim path, asset path, target path, and optional list-op');
    }
    const primBytes = flattenEncoder.encode(primPath);
    const assetBytes = flattenEncoder.encode(assetPath);
    const targetBytes = flattenEncoder.encode(targetPath);
    if (primBytes.length > (1 << 20) || assetBytes.length > (1 << 20) ||
        targetBytes.length > (1 << 20)) {
      throw new RangeError('addPrimArc: an arc path exceeds 1 MiB');
    }
    const payload = new Uint8Array(assetBytes.length + targetBytes.length);
    payload.set(assetBytes);
    payload.set(targetBytes, assetBytes.length);
    const status = withFlattenBytes(state, primPath, payload,
      (primPtr, primSize, dataPtr) => {
        const targetPtr = typeof dataPtr === 'bigint'
          ? dataPtr + BigInt(assetBytes.length) : dataPtr + assetBytes.length;
        return Module['_lightusd_next_flatten_add_prim_arc'](
          state.handle, kind, listOps[listOp], primPtr, primSize, dataPtr, assetBytes.length,
          targetPtr, targetBytes.length);
      });
    return status === 1 ? {success: true} : {success: false, error: this.error()};
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'setAssetPathRemap', {value: function(remap) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('setAssetPathRemap: wrong argument count');
    const bytes = flattenAssetRemapJSON(remap, 'setAssetPathRemap');
    const status = withFlattenBytes(state, '', bytes,
      (_base, _nameSize, data, size) =>
        Module['_lightusd_next_flatten_set_asset_path_remap'](
          state.handle, data, size));
    if (status !== 1) throw new RangeError('setAssetPathRemap: ' + this.error());
    return {success: true};
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'remapLayerAssetPaths', {value: function(remap) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('remapLayerAssetPaths: wrong argument count');
    const bytes = flattenAssetRemapJSON(remap, 'remapLayerAssetPaths');
    const count = withFlattenBytes(state, '', bytes,
      (_base, _nameSize, data, size) =>
        Module['_lightusd_next_flatten_remap_layer_asset_paths'](
          state.handle, data, size), true);
    if (count < 0) throw new RangeError('remapLayerAssetPaths: ' + this.error());
    return count;
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'setVariantOverride', {value: function(key, selection) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2) throw new TypeError('setVariantOverride: wrong argument count');
    if (typeof key !== 'string' || typeof selection !== 'string') {
      throw new TypeError('setVariantOverride: expected strings');
    }
    withFlattenBytes(state, key, flattenEncoder.encode(selection),
      (keyPtr, keySize, valuePtr, valueSize) =>
        Module['_lightusd_next_flatten_set_variant'](
          state.handle, keyPtr, keySize, valuePtr, valueSize));
    return {success: true};
  }});
  Object.defineProperty(Module.NextFlattenSession.prototype, 'end', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('end: wrong argument count');
    ++state.busy;
    try {
      if (Module['_lightusd_next_flatten_end'](state.handle) !== 0) {
        throw new TypeError('Invalid LightUSD receiver');
      }
    } finally { --state.busy; }
  }});
  const flattenCallbacks = new Map();
  let nextFlattenCallbackId = 1;
  Module['__lightusdNextFlattenEmit'] = (id, offset, size) => {
    const entry = flattenCallbacks.get(id);
    if (!entry) return 0;
    try {
      return entry.callback(new Uint8Array(Module.HEAPU8.buffer, offset, size)) === false ? 0 : 1;
    } catch (error) {
      entry.error = error;
      entry.errorSet = true;
      return 0;
    }
  };
  Object.defineProperty(Module.NextFlattenSession.prototype, 'step', {value: function(callback) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 3) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('step: wrong argument count');
    if (callback != null && typeof callback !== 'function') {
      throw new TypeError('step: expected callback or null');
    }
    let callbackId = 0;
    let entry;
    if (callback != null) {
      do {
        callbackId = nextFlattenCallbackId++ >>> 0;
      } while (callbackId === 0 || flattenCallbacks.has(callbackId));
      entry = {callback, error: null, errorSet: false};
      flattenCallbacks.set(callbackId, entry);
    }
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](80);
      if (!ptr) throw new RangeError('step: allocation failed');
      new DataView(Module.HEAPU8.buffer).setUint32(Number(ptr), 80, true);
      let result;
      try { result = Module['_lightusd_next_flatten_step'](state.handle, callbackId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        result = Module['_lightusd_next_flatten_step'](state.handle, callbackId, BigInt(ptr));
      }
      if (entry?.errorSet) throw entry.error;
      if (result !== 0) throw new RangeError('step: query failed');
      const info = new DataView(Module.HEAPU8.buffer, Number(ptr), 80);
      const status = info.getInt32(4, true);
      const assetCount = info.getUint32(8, true);
      const compositionErrorCount = info.getUint32(12, true);
      const layerDependencyCount = Module[
        '_lightusd_next_flatten_layer_dependency_count'](state.handle);
      if (layerDependencyCount < 0) throw new RangeError('step: dependency query failed');
      const stats = {
        inputBytes: info.getFloat64(16, true), outputBytes: info.getFloat64(24, true),
        primCount: info.getFloat64(32, true), arraysPassedThrough: info.getFloat64(40, true),
        arraysReencoded: info.getFloat64(48, true), readMs: info.getFloat64(56, true),
        composeMs: info.getFloat64(64, true), writeMs: info.getFloat64(72, true)
      };
      const copy = (kind, index) => {
        let buffer = 0;
        try {
          const query = (p, cap) => {
            try { return Module['_lightusd_next_flatten_step_buffer'](
              state.handle, kind, index, p, cap); }
            catch (error) {
              if (!(error instanceof TypeError)) throw error;
              return Module['_lightusd_next_flatten_step_buffer'](
                state.handle, kind, index, BigInt(p), cap);
            }
          };
          const bytes = query(0, 0);
          if (bytes < 0) throw new RangeError('step: invalid output buffer');
          if (bytes === 0) return new Uint8Array(0);
          buffer = Module['_lightusd_next_alloc'](bytes);
          if (!buffer) throw new RangeError('step: output allocation failed');
          if (query(buffer, bytes) !== bytes) throw new RangeError('step: output changed');
          return new Uint8Array(Module.HEAPU8.buffer, Number(buffer), bytes).slice();
        } finally {
          if (buffer) Module['_lightusd_next_free'](buffer);
        }
      };
      if (status === -1) return {success: false, error: this.error()};
      const copyCompositionErrors = () => {
        const errors = [];
        for (let i = 0; i < compositionErrorCount; ++i) {
          errors.push(flattenDecoder.decode(copy(3, i)));
        }
        return errors;
      };
      const copyLayerDependencies = () => {
        const dependencies = [];
        for (let i = 0; i < layerDependencyCount; ++i) {
          dependencies.push(flattenDecoder.decode(copy(4, i)));
        }
        return dependencies;
      };
      const attachLayerDependencies = result => {
        if (layerDependencyCount) {
          result.layerDependencies = copyLayerDependencies();
          result.layerDependencyCount = layerDependencyCount;
        }
        return result;
      };
      if (status === 0) {
        const failure = {success: false, status: 'error', error: this.error()};
        if (compositionErrorCount) {
          failure.compositionErrors = copyCompositionErrors();
          failure.compositionErrorCount = compositionErrorCount;
        }
        return attachLayerDependencies(failure);
      }
      if (status === 1) return attachLayerDependencies({success: true, status: 'ready'});
      if (status === 2) {
        return attachLayerDependencies({success: true, status: 'need-layer',
          key: flattenDecoder.decode(copy(1, 0))});
      }
      if (status !== 3) throw new RangeError('step: invalid status');
      const output = {success: true, status: 'done'};
      if (!callbackId) output.data = copy(0, 0);
      Object.assign(output, stats);
      output.assetPaths = [];
      for (let i = 0; i < assetCount; ++i) {
        output.assetPaths.push(flattenDecoder.decode(copy(2, i)));
      }
      output.assetPathCount = assetCount;
      if (compositionErrorCount) {
        output.compositionErrors = copyCompositionErrors();
        output.compositionErrorCount = compositionErrorCount;
      }
      return attachLayerDependencies(output);
    } finally {
      Module['_lightusd_next_flatten_release_step'](state.handle);
      if (ptr) Module['_lightusd_next_free'](ptr);
      if (callbackId) flattenCallbacks.delete(callbackId);
      --state.busy;
    }
  }});
  const renderErrorDecoder = new TextDecoder();
  Object.defineProperty(Module.RenderStream.prototype, 'error', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('error: wrong argument count');
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module['_lightusd_next_render_error'](state.handle, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_error'](state.handle, BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new TypeError('Invalid LightUSD receiver');
      if (bytes === 0) return '';
      ptr = Module['_lightusd_next_alloc'](bytes + 1);
      if (!ptr) throw new RangeError('error: allocation failed');
      if (query(ptr, bytes + 1) !== bytes) throw new RangeError('error: text changed');
      return renderErrorDecoder.decode(new Uint8Array(
        Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'warning', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('warning: wrong argument count');
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module['_lightusd_next_render_warning'](state.handle, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_warning'](state.handle, BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new TypeError('Invalid LightUSD receiver');
      if (bytes === 0) return '';
      ptr = Module['_lightusd_next_alloc'](bytes + 1);
      if (!ptr) throw new RangeError('warning: allocation failed');
      if (query(ptr, bytes + 1) !== bytes) throw new RangeError('warning: text changed');
      return renderErrorDecoder.decode(new Uint8Array(
        Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'warn', {value: function() {
    if (arguments.length) throw new TypeError('warn: wrong argument count');
    return this.warning();
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'ok', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('ok: wrong argument count');
    ++state.busy;
    try {
      const loaded = Module['_lightusd_next_render_loaded'](state.handle);
      if (loaded !== 0 && loaded !== 1) throw new RangeError('ok: state query failed');
      return loaded === 1;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'compositionReport', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('compositionReport: wrong argument count');
    ++state.busy;
    let aggregateBytes = 0;
    const query = (field, index, pointer, cap) => {
      try {
        return Module['_lightusd_next_render_composition_record'](
          state.handle, field, index, pointer, cap);
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module['_lightusd_next_render_composition_record'](
          state.handle, field, index, BigInt(pointer), cap);
      }
    };
    const read = (field, index) => {
      let ptr = 0;
      try {
        const size = query(field, index, 0, 0);
        if (size < 0) throw new RangeError('compositionReport: invalid record');
        if (size === 0) return '';
        const estimate = size * 2 + 128;
        if (!Number.isSafeInteger(estimate) || estimate > 0x20000000 ||
            aggregateBytes + estimate > 0x20000000) {
          throw new RangeError('compositionReport: payload exceeds 512 MiB aggregate limit');
        }
        aggregateBytes += estimate;
        preflightRenderAggregate(state, aggregateBytes, 'compositionReport');
        ptr = Module['_lightusd_next_alloc'](size + 1);
        if (!ptr) throw new RangeError('compositionReport: allocation failed');
        if (query(field, index, ptr, size + 1) !== size) {
          throw new RangeError('compositionReport: record changed');
        }
        return renderErrorDecoder.decode(new Uint8Array(
          Module.HEAPU8.buffer, Number(ptr), size));
      } finally {
        if (ptr) Module['_lightusd_next_free'](ptr);
      }
    };
    try {
      const dependencyCount = query(0, 0, 0, 0);
      const issueCount = query(2, 0, 0, 0);
      if (dependencyCount < 0 || issueCount < 0) {
        throw new TypeError('Invalid LightUSD receiver');
      }
      if (dependencyCount > 65536 || issueCount > 65536) {
        throw new RangeError('compositionReport: excessive record count');
      }
      preflightRenderAggregate(state, (dependencyCount + issueCount) * 4096,
        'compositionReport');
      const dependencies = Array.from({length: dependencyCount}, (_, index) =>
        read(1, index));
      const issues = Array.from({length: issueCount}, (_, index) => {
        const code = query(3, index, 0, 0);
        if (code < 0) throw new RangeError('compositionReport: invalid issue code');
        return {code, site: read(4, index), message: read(5, index)};
      });
      return {dependencies, issues};
    } finally {
      --state.busy;
    }
  }});
  function withRenderProgress(state, callback) {
    const property = '__lightusdNextRenderProgressCallback';
    const hadPrevious = Object.prototype.hasOwnProperty.call(Module, property);
    const previous = Module[property];
    const progressCallback = state.progressCallback;
    let callbackError = null;
    Module[property] = event => {
      const current = Number(event.current) || 0;
      const total = Number(event.total) || 0;
      const stage = /convert|render|mesh|material/i.test(event.phase)
        ? 'converting' : 'parsing';
      state.parsing = true;
      state.progress = {...state.progress, progress: total > 0 ? current / total : 0,
        stage, currentOperation: String(event.phase || ''),
        bytesProcessed: current, totalBytes: total,
        percentage: Number(event.percentage) || 0,
        cancelRequested: state.cancelRequested};
      try {
        const keepGoing = !state.cancelRequested &&
          (!progressCallback || progressCallback(event) !== false);
        if (!keepGoing) state.wasCancelled = true;
        state.progress.cancelRequested = state.cancelRequested;
        return keepGoing;
      } catch (error) {
        callbackError = error;
        state.wasCancelled = true;
        return false;
      }
    };
    try {
      const result = callback();
      if (callbackError) throw callbackError;
      return result;
    } finally {
      if (hadPrevious) Module[property] = previous;
      else delete Module[property];
    }
  }
  function renderBeginResult(stream, state, status) {
    if (status < 0) throw new TypeError('Invalid LightUSD receiver');
    state.parsing = false;
    if (!status) {
      const cancelled = state.cancelRequested || state.wasCancelled;
      const error = stream.error();
      state.wasCancelled = cancelled;
      state.progress = {...state.progress,
        stage: cancelled ? 'cancelled' : 'error',
        cancelRequested: state.cancelRequested, errorMessage: error};
      return {success: false, error};
    }
    state.cancelRequested = false;
    state.wasCancelled = false;
    state.progress = {...state.progress, progress: 1, percentage: 100,
      stage: 'complete', cancelRequested: false, errorMessage: ''};
    const count = kind => Module['_lightusd_next_render_count'](state.handle, kind);
    const meshes = count(0), nodes = count(1), lights = count(2);
    const points = count(3), curves = count(4), cameras = count(5);
    const pointInstancers = count(6), pointInstanceDraws = count(7);
    const skeletons = count(8), unsupportedRenderables = count(9);
    const animations = count(10);
    return {success: true, meshCount: meshes,
      points, pointsCount: points, curves, curvesCount: curves,
      nodes, nodeCount: nodes, lights, lightCount: lights,
      cameras, cameraCount: cameras, pointInstancers,
      pointInstancerCount: pointInstancers, skeletons,
      skeletonCount: skeletons, unsupportedRenderables,
      unsupportedRenderableCount: unsupportedRenderables,
      animations, animationCount: animations,
      pointInstanceDraws, pointInstanceDrawCount: pointInstanceDraws};
  }
  function startRenderProgress(state) {
    state.cancelRequested = false;
    state.wasCancelled = false;
    state.parsing = true;
    state.progress = {...state.progress, progress: 0, stage: 'parsing',
      currentOperation: '', cancelRequested: false, errorMessage: '',
      bytesProcessed: 0, totalBytes: 0, percentage: 0};
  }
  function refreshAttachedAssetStore(stream, state) {
    if (state.assetStore) stream.importAssetStore(state.assetStore);
  }
  function beginRenderBytes(stream, state, source, sourceURI = '') {
    state.sourceURI = sourceURI;
    const inputLength = source.byteLength;
    const sourceWasWasmHeap = source.buffer === Module.HEAPU8.buffer;
    const sourceHeapOffset = source.byteOffset;
    refreshAttachedAssetStore(stream, state);
    startRenderProgress(state);
    ++state.busy;
    let ptr = 0;
    try {
      if (inputLength > 0x40000000) {
        const invalid = p => Module['_lightusd_next_render_begin'](
          state.handle, p, 0x40000001);
        try { invalid(0); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          invalid(0n);
        }
        return {success: false, error: stream.error()};
      }
      const preflight = Module['_lightusd_next_render_preflight_input'](
        state.handle, inputLength);
      if (preflight < 0) throw new TypeError('Invalid LightUSD receiver');
      if (preflight === 0) return renderBeginResult(stream, state, 0);
      // Refreshing an attached store may grow memory and detach a borrowed
      // heap view. Rebind its original offset before taking the safe snapshot.
      if (sourceWasWasmHeap) {
        source = new Uint8Array(Module.HEAPU8.buffer, sourceHeapOffset, inputLength).slice();
      }
      ptr = Module['_lightusd_next_alloc'](Math.max(inputLength, 1));
      if (!ptr) throw new RangeError('begin: allocation failed');
      Module.HEAPU8.set(source, Number(ptr));
      let status;
      status = withRenderProgress(state, () => {
        try {
          return Module['_lightusd_next_render_begin'](
            state.handle, ptr, inputLength);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_begin'](
            state.handle, BigInt(ptr), inputLength);
        }
      });
      return renderBeginResult(stream, state, status);
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }
  Object.defineProperty(Module.RenderStream.prototype, 'begin', {value: function(bytes, sourceURI = '') {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length < 1 || arguments.length > 2) throw new TypeError('begin: wrong argument count');
    if (!ArrayBuffer.isView(bytes)) throw new TypeError('begin: expected byte view');
    if (typeof sourceURI !== 'string') throw new TypeError('begin: expected source URI string');
    return beginRenderBytes(this, state,
      new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength), sourceURI);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'beginFromLayerDocument', {value: function(document) {
    const state = live.get(this);
    const documentState = live.get(document);
    if (!state?.handle || state.kind !== 4 || !documentState?.handle ||
        documentState.kind !== 6) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) {
      throw new TypeError('beginFromLayerDocument: expected one LayerDocument');
    }
    if (state.busy || documentState.busy) {
      throw new TypeError('beginFromLayerDocument: object is busy');
    }
    state.sourceURI = '<layer-document>.usda';
    refreshAttachedAssetStore(this, state);
    startRenderProgress(state);
    ++state.busy;
    ++documentState.busy;
    try {
      const status = withRenderProgress(state, () =>
        Module['_lightusd_next_render_begin_layer_document'](
          state.handle, documentState.handle));
      return renderBeginResult(this, state, status);
    } finally {
      --documentState.busy;
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'beginCachedAsset', {value: function(store, identifier) {
    const state = live.get(this), storeState = live.get(store);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || !storeState?.handle || storeState.kind !== 5 ||
        typeof identifier !== 'string' || identifier.length === 0) {
      throw new TypeError('beginCachedAsset: expected a live NextAssetStore and non-empty identifier');
    }
    if (state.busy || storeState.busy) throw new TypeError('beginCachedAsset: object is busy');
    state.sourceURI = identifier;
    const name = assetEncoder.encode(identifier);
    if (name.length === 0 || name.length > 0xffffffff) {
      throw new RangeError('beginCachedAsset: invalid identifier size');
    }
    ++state.busy;
    ++storeState.busy;
    startRenderProgress(state);
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](name.length);
      if (!ptr) throw new RangeError('beginCachedAsset: allocation failed');
      Module.HEAPU8.set(name, Number(ptr));
      let status;
      const call = p => Module['_lightusd_next_render_begin_cached_asset'](
        state.handle, storeState.handle, p, name.length);
      try {
        status = withRenderProgress(state, () => call(ptr));
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = withRenderProgress(state, () => call(BigInt(ptr)));
      }
      state.assetStore = store;
      return renderBeginResult(this, state, status);
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --storeState.busy;
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'beginAsync', {value: function(bytes, sourceURI = '') {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length < 1 || arguments.length > 2) throw new TypeError('beginAsync: wrong argument count');
    if (!ArrayBuffer.isView(bytes)) throw new TypeError('beginAsync: expected byte view');
    if (typeof sourceURI !== 'string') throw new TypeError('beginAsync: expected source URI string');
    if (state.busy) throw new TypeError('beginAsync: object is busy');
    let source = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const inputLength = source.byteLength;
    const sourceWasWasmHeap = source.buffer === Module.HEAPU8.buffer;
    const sourceHeapOffset = source.byteOffset;
    refreshAttachedAssetStore(this, state);
    const preflightSize = inputLength > 0x40000000 ? 0x40000001 : inputLength;
    const preflight = Module['_lightusd_next_render_preflight_input'](
      state.handle, preflightSize);
    if (preflight < 0) throw new TypeError('Invalid LightUSD receiver');
    if (preflight === 0) {
      const result = renderBeginResult(this, state, 0);
      return new Promise(resolve => setTimeout(() => resolve(result), 0));
    }
    if (sourceWasWasmHeap) {
      source = new Uint8Array(Module.HEAPU8.buffer, sourceHeapOffset, inputLength);
    }
    let owned;
    try { owned = source.slice(); }
    catch (_error) { return Promise.reject(new RangeError('beginAsync: input copy failed')); }
    const stream = this;
    return new Promise((resolve, reject) => setTimeout(() => {
      try { resolve(stream.begin(owned, sourceURI)); }
      catch (error) { reject(error); }
    }, 0));
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setProgressCallback', {value: function(callback) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || (callback !== null && typeof callback !== 'function')) {
      throw new TypeError('setProgressCallback: expected a function or null');
    }
    state.progressCallback = callback;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getProgress', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getProgress: wrong argument count');
    return {...state.progress, cancelRequested: state.cancelRequested};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'cancelParsing', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('cancelParsing: wrong argument count');
    if (state.parsing) state.cancelRequested = true;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'isParsingInProgress', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('isParsingInProgress: wrong argument count');
    return state.parsing;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'wasCancelled', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('wasCancelled: wrong argument count');
    return state.wasCancelled;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'resetProgress', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('resetProgress: wrong argument count');
    if (state.parsing || state.busy) throw new TypeError('resetProgress: object is busy');
    state.cancelRequested = false;
    state.wasCancelled = false;
    state.progress = {...state.progress, progress: 0, stage: 'idle',
      currentOperation: '', cancelRequested: false, errorMessage: '',
      bytesProcessed: 0, totalBytes: 0, percentage: 0};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'beginStreamedAsset', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('beginStreamedAsset: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (typeof name !== 'string') throw new TypeError('beginStreamedAsset: expected an asset name');
    state.sourceURI = name;
    refreshAttachedAssetStore(this, state);
    ++state.busy;
    try {
      const status = withRenderProgress(state, () => streamAssetCall(this,
        'beginStreamedAsset', name, undefined,
        '_lightusd_next_render_begin_streamed_asset'));
      return renderBeginResult(this, state, status);
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'importAssetStore', {value: function(store) {
    const state = live.get(this), storeState = live.get(store);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !storeState?.handle || storeState.kind !== 5) {
      throw new TypeError('importAssetStore: expected a live NextAssetStore');
    }
    if (state.busy || storeState.busy) throw new TypeError('importAssetStore: object is busy');
    ++state.busy;
    ++storeState.busy;
    try {
      const status = Module['_lightusd_next_render_import_asset_store'](
        state.handle, storeState.handle);
      if (status < 0) throw new RangeError('importAssetStore: ' + this.error());
      return true;
    } finally {
      --storeState.busy;
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setAssetStore', {value: function(store) {
    const state = live.get(this), storeState = live.get(store);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !storeState?.handle || storeState.kind !== 5) {
      throw new TypeError('setAssetStore: expected a live NextAssetStore');
    }
    this.importAssetStore(store);
    state.assetStore = store;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'detachAssetStore', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('detachAssetStore: wrong argument count');
    if (state.busy) throw new TypeError('detachAssetStore: object is busy');
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_clear_imported_asset_store'](state.handle) < 0) {
        throw new RangeError('detachAssetStore: ' + this.error());
      }
      state.assetStore = null;
      return true;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setMaxInputBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit < 1 || limit > 0x40000000) {
      throw new TypeError('setMaxInputBytes: expected integer from 1 through 1 GiB');
    }
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_max_input_bytes'](state.handle, limit) !== 0) {
        throw new RangeError('setMaxInputBytes: limit rejected');
      }
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'maxInputBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('maxInputBytes: wrong argument count');
    ++state.busy;
    try {
      const limit = Module['_lightusd_next_render_max_input_bytes'](state.handle);
      if (limit < 0) throw new RangeError('maxInputBytes: query failed');
      return limit;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setMaxMemoryLimitMB', {value: function(limit) {
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit < 1 || limit > 8192) {
      throw new TypeError('setMaxMemoryLimitMB: expected integer from 1 through 8192');
    }
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_memory_limit_mb'](state.handle, limit) !== 0) {
        throw new RangeError('setMaxMemoryLimitMB: limit rejected on this platform');
      }
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMaxMemoryLimitMB', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getMaxMemoryLimitMB: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    try {
      const limit = Module['_lightusd_next_render_memory_limit_mb'](state.handle);
      if (limit < 1) throw new RangeError('getMaxMemoryLimitMB: query failed');
      return limit;
    } finally { --state.busy; }
  }});
  const beginEncoder = new TextEncoder();
  Object.defineProperty(Module.RenderStream.prototype, 'beginOwned', {value: function(text) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('beginOwned: wrong argument count');
    if (typeof text !== 'string') throw new TypeError('beginOwned: expected string');
    return beginRenderBytes(this, state, beginEncoder.encode(text));
  }});
  const assetEncoder = new TextEncoder();
  const streamAssetCall = (receiver, label, name, bytes, symbol, rangeOffset = 0) => {
    const state = live.get(receiver);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (typeof name !== 'string') throw new TypeError(label + ': expected string name');
    const nameBytes = assetEncoder.encode(name);
    if (!nameBytes.length) throw new TypeError(label + ': expected non-empty name');
    let data = null;
    const scalarSizeCall = symbol === '_lightusd_next_render_stream_asset_start' ||
      symbol === '_lightusd_next_render_stream_asset_view' ||
      symbol === '_lightusd_next_render_stream_asset_mark_written' ||
      symbol === '_lightusd_next_render_stream_asset_view_at' ||
      symbol === '_lightusd_next_render_stream_asset_mark_range_written';
    if (bytes !== undefined && !scalarSizeCall) {
      if (!ArrayBuffer.isView(bytes)) throw new TypeError(label + ': expected byte view');
      data = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      if (data.byteLength > 0x40000000) throw new RangeError(label + ': chunk exceeds 1 GiB limit');
    }
    ++state.busy;
    let ptr = 0;
    try {
      const total = nameBytes.length + (data?.length || 0);
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError(label + ': allocation failed');
      const heapOffset = Number(ptr);
      Module.HEAPU8.set(nameBytes, heapOffset);
      if (data?.length) Module.HEAPU8.set(data, heapOffset + nameBytes.length);
      const dataOffset = heapOffset + nameBytes.length;
      const call = (handle, namePtr, dataPtr) => {
        if (symbol === '_lightusd_next_render_stream_asset_start') {
          return Module[symbol](handle, namePtr, nameBytes.length, bytes);
        }
        if (symbol === '_lightusd_next_render_stream_asset_append') {
          return Module[symbol](handle, namePtr, nameBytes.length, dataPtr, data.length);
        }
        if (symbol === '_lightusd_next_render_stream_asset_view' ||
            symbol === '_lightusd_next_render_stream_asset_mark_written') {
          return Module[symbol](handle, namePtr, nameBytes.length, bytes);
        }
        if (symbol === '_lightusd_next_render_stream_asset_view_at' ||
            symbol === '_lightusd_next_render_stream_asset_mark_range_written') {
          return Module[symbol](handle, namePtr, nameBytes.length, rangeOffset, bytes);
        }
        return Module[symbol](handle, namePtr, nameBytes.length);
      };
      try { return call(state.handle, ptr, typeof ptr === 'bigint' ? ptr + BigInt(nameBytes.length) : dataOffset); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        const handle = typeof state.handle === 'bigint' ? state.handle : BigInt(state.handle);
        const namePtr = typeof ptr === 'bigint' ? ptr : BigInt(ptr);
        const dataPtr = namePtr + BigInt(nameBytes.length);
        return call(handle, namePtr, dataPtr);
      }
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  };
  const streamAssetStringCall = (receiver, label, name, symbol) => {
    const state = live.get(receiver);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (typeof name !== 'string') throw new TypeError(label + ': expected string name');
    const nameBytes = assetEncoder.encode(name);
    if (!nameBytes.length) throw new TypeError(label + ': expected non-empty name');
    let ptr = 0;
    try {
      const outputCap = 64;
      ptr = Module['_lightusd_next_alloc'](nameBytes.length + outputCap);
      if (!ptr) throw new RangeError(label + ': allocation failed');
      const base = Number(ptr), outOffset = base + nameBytes.length;
      Module.HEAPU8.set(nameBytes, base);
      const outputPointer = typeof ptr === 'bigint'
        ? ptr + BigInt(nameBytes.length) : ptr + nameBytes.length;
      const call = (namePointer, output, cap) => Module[symbol](state.handle,
        namePointer, nameBytes.length, output, cap);
      let size;
      try { size = call(ptr, typeof ptr === 'bigint' ? 0n : 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        size = call(BigInt(ptr), 0n, 0);
      }
      if (size < 0) return '';
      if (!size || size > outputCap) throw new RangeError(label + ': invalid identifier size');
      let copied;
      try { copied = call(ptr, outputPointer, outputCap); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        copied = call(BigInt(ptr), BigInt(outputPointer), outputCap);
      }
      if (copied !== size) throw new RangeError(label + ': identifier changed');
      return assetStoreDecoder.decode(Module.HEAPU8.subarray(outOffset, outOffset + size));
    } finally { if (ptr) Module['_lightusd_next_free'](ptr); }
  };
  Object.defineProperty(Module.RenderStream.prototype, 'startStreamingAsset', {value: function(name, expectedSize) {
    if (arguments.length !== 2 || !Number.isInteger(expectedSize) || expectedSize < 0 || expectedSize > 0x40000000) {
      throw new TypeError('startStreamingAsset: expected a byte count from 0 through 1 GiB');
    }
    const status = streamAssetCall(this, 'startStreamingAsset', name, expectedSize,
      '_lightusd_next_render_stream_asset_start');
    if (status < 0) throw new RangeError('startStreamingAsset: ' + this.error());
    if (status === 0) {
      const state = live.get(this);
      const uuid = this.getStreamingAssetUUID(name);
      if (uuid) {
        for (const [oldUuid, oldInfo] of state.streamBuffers) if (oldInfo.name === name) state.streamBuffers.delete(oldUuid);
        const p = this.streamingAssetProgress(name);
        const view = p?.totalBytes ? this.getStreamingAssetViewAt(name, 0, p.totalBytes) : null;
        state.streamBuffers.set(uuid, {name, ptr: view?.byteOffset ?? 0, size: p?.totalBytes ?? 0});
      }
    }
    return status === 0;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'appendStreamingAsset', {value: function(name, bytes) {
    if (arguments.length !== 2) throw new TypeError('appendStreamingAsset: wrong argument count');
    const status = streamAssetCall(this, 'appendStreamingAsset', name, bytes,
      '_lightusd_next_render_stream_asset_append');
    if (status < 0) throw new RangeError('appendStreamingAsset: invalid stream');
    return status === 1;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'appendAssetChunk', {value: function(name, bytes) {
    if (arguments.length !== 2) throw new TypeError('appendAssetChunk: wrong argument count');
    // Legacy chunks accept JS strings (UTF-8) and ArrayBuffers as well as byte views.
    if (typeof bytes === 'string') bytes = new TextEncoder().encode(bytes);
    else if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
    return this.appendStreamingAsset(name, bytes);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'streamingAssetProgress', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('streamingAssetProgress: wrong argument count');
    const written = streamAssetCall(this, 'streamingAssetProgress', name, undefined,
      '_lightusd_next_render_stream_asset_progress');
    if (written < 0) return null;
    const totalBytes = streamAssetCall(this, 'streamingAssetProgress', name, undefined,
      '_lightusd_next_render_stream_asset_size');
    if (totalBytes < 0 || written > totalBytes) throw new RangeError('streamingAssetProgress: invalid progress');
    return {bytesWritten: written, totalBytes,
      progress: totalBytes === 0 ? 1 : written / totalBytes,
      isComplete: written === totalBytes};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getStreamingAssetUUID', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('getStreamingAssetUUID: wrong argument count');
    return streamAssetStringCall(this, 'getStreamingAssetUUID', name,
      '_lightusd_next_render_stream_asset_uuid');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getStreamingProgress', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('getStreamingProgress: wrong argument count');
    const progress = this.streamingAssetProgress(name);
    if (!progress) return {exists: false};
    return {exists: true, current: progress.bytesWritten, total: progress.totalBytes,
      complete: progress.isComplete, uuid: this.getStreamingAssetUUID(name),
      percentage: progress.progress * 100};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getStreamingAssetView', {value: function(name, byteLength) {
    if (arguments.length < 1 || arguments.length > 2) {
      throw new TypeError('getStreamingAssetView: expected a name and optional byte length');
    }
    const progress = this.streamingAssetProgress(name);
    if (!progress) return null;
    const available = progress.totalBytes - progress.bytesWritten;
    const length = byteLength === undefined ? available : byteLength;
    if (!Number.isInteger(length) || length < 0 || length > available) {
      throw new RangeError('getStreamingAssetView: byte length exceeds remaining stream');
    }
    if (length === 0) return new Uint8Array(0);
    const pointer = streamAssetCall(this, 'getStreamingAssetView', name, length,
      '_lightusd_next_render_stream_asset_view');
    const address = typeof pointer === 'bigint' ? Number(pointer) : pointer;
    if (!pointer || !Number.isSafeInteger(address)) {
      throw new RangeError('getStreamingAssetView: stream view unavailable');
    }
    return new Uint8Array(Module.HEAPU8.buffer, address, length);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'markStreamingAssetBytesWritten', {value: function(name, byteLength) {
    if (arguments.length !== 2 || !Number.isInteger(byteLength) || byteLength < 0 || byteLength > 0xffffffff) {
      throw new TypeError('markStreamingAssetBytesWritten: expected a uint32 byte count');
    }
    const status = streamAssetCall(this, 'markStreamingAssetBytesWritten', name, byteLength,
      '_lightusd_next_render_stream_asset_mark_written');
    if (status < 0) throw new RangeError('markStreamingAssetBytesWritten: invalid stream');
    return status === 1;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getStreamingAssetViewAt', {value: function(name, offset, byteLength) {
    if (arguments.length !== 3 || !Number.isInteger(offset) || offset < 0 || offset > 0xffffffff ||
        !Number.isInteger(byteLength) || byteLength < 0) {
      throw new TypeError('getStreamingAssetViewAt: expected non-negative offset and byte length');
    }
    const progress = this.streamingAssetProgress(name);
    if (!progress) return null;
    if (offset > progress.totalBytes || byteLength > progress.totalBytes - offset) {
      throw new RangeError('getStreamingAssetViewAt: range exceeds streaming asset size');
    }
    if (byteLength === 0) return new Uint8Array(0);
    const pointer = streamAssetCall(this, 'getStreamingAssetViewAt', name,
      byteLength, '_lightusd_next_render_stream_asset_view_at', offset);
    const address = typeof pointer === 'bigint' ? Number(pointer) : pointer;
    if (!pointer || !Number.isSafeInteger(address)) {
      throw new RangeError('getStreamingAssetViewAt: stream view unavailable');
    }
    return new Uint8Array(Module.HEAPU8.buffer, address, byteLength);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'markStreamingAssetRangeWritten', {value: function(name, offset, byteLength) {
    if (arguments.length !== 3 || !Number.isInteger(offset) || offset < 0 || offset > 0xffffffff ||
        !Number.isInteger(byteLength) || byteLength < 0 || byteLength > 0xffffffff) {
      throw new TypeError('markStreamingAssetRangeWritten: expected non-negative offset and byte length');
    }
    const status = streamAssetCall(this, 'markStreamingAssetRangeWritten', name,
      byteLength, '_lightusd_next_render_stream_asset_mark_range_written', offset);
    if (status < 0) throw new RangeError('markStreamingAssetRangeWritten: too many disjoint ranges');
    return status === 1;
  }});
  for (const [name, symbol] of [['finalizeStreamingAsset', '_lightusd_next_render_stream_asset_finalize'],
    ['cancelStreamingAsset', '_lightusd_next_render_stream_asset_cancel']]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(assetName) {
      if (arguments.length !== 1) throw new TypeError(name + ': wrong argument count');
      const status = streamAssetCall(this, name, assetName, undefined, symbol);
      if (status < 0) throw new RangeError(name + ': invalid stream');
      if (status === 1) {
        const state = live.get(this);
        for (const [uuid, mappedName] of state.streamBuffers) {
          if (mappedName.name === assetName) state.streamBuffers.delete(uuid);
        }
      }
      return status === 1;
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'finalizeStreamingAssetToStore', {value: function(name, store) {
    if (arguments.length !== 2 || typeof name !== 'string') {
      throw new TypeError('finalizeStreamingAssetToStore: expected asset name and NextAssetStore');
    }
    const state = live.get(this), storeState = live.get(store);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (!storeState?.handle || storeState.kind !== 5) {
      throw new TypeError('finalizeStreamingAssetToStore: expected a live NextAssetStore');
    }
    if (state.busy || storeState.busy) throw new TypeError('finalizeStreamingAssetToStore: object is busy');
    const nameBytes = assetEncoder.encode(name);
    if (!nameBytes.length) throw new TypeError('finalizeStreamingAssetToStore: expected non-empty name');
    ++state.busy;
    ++storeState.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](nameBytes.length);
      if (!ptr) throw new RangeError('finalizeStreamingAssetToStore: allocation failed');
      Module.HEAPU8.set(nameBytes, Number(ptr));
      const invoke = (render, assetStore, namePointer) =>
        Module['_lightusd_next_render_stream_asset_finalize_to_store'](
          render, assetStore, namePointer, nameBytes.length);
      let status;
      try { status = invoke(state.handle, storeState.handle, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = invoke(BigInt(state.handle), BigInt(storeState.handle), BigInt(ptr));
      }
      if (status < -1) throw new RangeError('finalizeStreamingAssetToStore: ' + this.error());
      if (status < 0) throw new TypeError('Invalid LightUSD receiver');
      if (status === 1) {
        for (const [uuid, info] of state.streamBuffers) {
          if (info.name === name) state.streamBuffers.delete(uuid);
        }
      }
      return status === 1;
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --storeState.busy;
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'allocateZeroCopyBuffer', {value: function(assetName, size, maxBytes = 0) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length < 2 || arguments.length > 3 || typeof assetName !== 'string' ||
        !Number.isSafeInteger(size) || size < 0 || !Number.isSafeInteger(maxBytes) || maxBytes < 0) {
      throw new TypeError('allocateZeroCopyBuffer: expected asset name, non-negative size and optional maximum');
    }
    const cap = maxBytes || (1 << 29);
    if (size === 0) return {success: false, error: 'Size must be greater than 0'};
    if (size > cap) return {success: false, error: 'Buffer size exceeds ' + Math.floor(cap / (1 << 20)) + ' MiB limit'};
    try {
      if (!this.startStreamingAsset(assetName, size)) return {success: false, error: 'Failed to allocate buffer'};
      const uuid = this.getStreamingAssetUUID(assetName);
      const view = this.getStreamingAssetViewAt(assetName, 0, size);
      const info = state.streamBuffers.get(uuid);
      if (info) info.zeroCopy = true;
      return {success: true, uuid, assetName, totalSize: size, bufferPtr: view.byteOffset};
    } catch (error) { return {success: false, error: error.message}; }
  }});
  const zeroCopyInfo = (receiver, uuid) => live.get(receiver)?.streamBuffers.get(uuid);
  Object.defineProperty(Module.RenderStream.prototype, 'getZeroCopyBufferPtr', {value: function(uuid) {
    if (arguments.length !== 1 || typeof uuid !== 'string') throw new TypeError('getZeroCopyBufferPtr: expected UUID');
    return zeroCopyInfo(this, uuid)?.ptr ?? 0;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getZeroCopyBufferPtrAtOffset', {value: function(uuid, offset) {
    if (arguments.length !== 2 || typeof uuid !== 'string' || !Number.isSafeInteger(offset) || offset < 0) throw new TypeError('getZeroCopyBufferPtrAtOffset: expected UUID and non-negative offset');
    const info = zeroCopyInfo(this, uuid);
    if (!info || offset >= info.size) return 0;
    return info.ptr + offset;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'markZeroCopyBytesWritten', {value: function(uuid, count) {
    if (arguments.length !== 2 || typeof uuid !== 'string' || !Number.isSafeInteger(count) || count < 0) throw new TypeError('markZeroCopyBytesWritten: expected UUID and byte count');
    const info = zeroCopyInfo(this, uuid);
    return info === undefined ? false : this.markStreamingAssetBytesWritten(info.name, count);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getZeroCopyProgress', {value: function(uuid) {
    if (arguments.length !== 1 || typeof uuid !== 'string') throw new TypeError('getZeroCopyProgress: expected UUID');
    const info = zeroCopyInfo(this, uuid), p = info === undefined ? null : this.streamingAssetProgress(info.name);
    if (!p) return {exists: false};
    return {exists: true, uuid, assetName: info.name, totalSize: p.totalBytes, bytesWritten: p.bytesWritten,
      progress: p.progress, isComplete: p.isComplete, finalized: false, bufferPtr: this.getZeroCopyBufferPtr(uuid)};
  }});
  for (const [name, target] of [['finalizeZeroCopyBuffer', 'finalizeStreamingAsset'],
    ['cancelZeroCopyBuffer', 'cancelStreamingAsset']]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(uuid) {
      if (arguments.length !== 1 || typeof uuid !== 'string') throw new TypeError(name + ': expected UUID');
      const assetName = zeroCopyInfo(this, uuid)?.name;
      return assetName === undefined ? false : this[target](assetName);
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'isStreamingAssetComplete', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('isStreamingAssetComplete: expected asset name');
    return this.getStreamingProgress(name).complete === true;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getActiveZeroCopyBuffers', {value: function() {
    if (arguments.length) throw new TypeError('getActiveZeroCopyBuffers: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    const result = [];
    // Legacy lists only allocateZeroCopyBuffer transfers, not chunked streams,
    // and its info records carry no `exists` flag.
    for (const uuid of Array.from(state.streamBuffers.keys()).sort()) {
      const {exists, ...info} = this.getZeroCopyProgress(uuid);
      if (!exists) state.streamBuffers.delete(uuid);
      else if (state.streamBuffers.get(uuid).zeroCopy) result.push({uuid, info});
    }
    return result;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMMapZeroCopy', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getMMapZeroCopy: wrong argument count');
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    throw new Error('getMMapZeroCopy: unsupported; next-only WASM assets are not mmap-backed');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setMMapZeroCopy', {value: function(enabled) {
    if (arguments.length !== 1 || typeof enabled !== 'boolean') {
      throw new TypeError('setMMapZeroCopy: expected one boolean');
    }
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    throw new Error('setMMapZeroCopy: unsupported; next-only WASM assets are not mmap-backed');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'provideAsset', {value: function(name, bytes) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2) throw new TypeError('provideAsset: wrong argument count');
    if (typeof name !== 'string') throw new TypeError('provideAsset: expected string');
    if (!ArrayBuffer.isView(bytes)) throw new TypeError('provideAsset: expected byte view');
    const source = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (source.byteLength > 0x40000000) throw new RangeError('provideAsset: input exceeds 1 GiB limit');
    const sourceInHeap = source.buffer === Module.HEAPU8.buffer;
    const sourceHeapOffset = source.byteOffset;
    const nameBytes = assetEncoder.encode(name);
    const total = sourceInHeap ? nameBytes.length : nameBytes.length + source.byteLength;
    if (total > 0xffffffff) throw new RangeError('provideAsset: input too large');
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('provideAsset: allocation failed');
      const offset = Number(ptr);
      Module.HEAPU8.set(nameBytes, offset);
      if (!sourceInHeap) Module.HEAPU8.set(source, offset + nameBytes.length);
      const dataOffset = sourceInHeap ? sourceHeapOffset : offset + nameBytes.length;
      const dataPtr = typeof ptr === 'bigint' ? BigInt(dataOffset) : dataOffset;
      const call = (namePtr, bytesPtr) =>
        Module['_lightusd_next_render_provide_asset'](
          state.handle, namePtr, nameBytes.length, bytesPtr, source.byteLength);
      let status;
      try { status = call(ptr, dataPtr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = call(BigInt(ptr), BigInt(dataPtr));
      }
      if (status !== 0) throw new RangeError('provideAsset: asset rejected');
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'removeAsset', {value: function(name) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('removeAsset: wrong argument count');
    if (typeof name !== 'string') throw new TypeError('removeAsset: expected string');
    const nameBytes = assetEncoder.encode(name);
    if (nameBytes.length === 0) throw new TypeError('removeAsset: expected non-empty name');
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](nameBytes.length);
      if (!ptr) throw new RangeError('removeAsset: allocation failed');
      Module.HEAPU8.set(nameBytes, Number(ptr));
      let result;
      try {
        result = Module['_lightusd_next_render_remove_asset'](
          state.handle, ptr, nameBytes.length);
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        result = Module['_lightusd_next_render_remove_asset'](
          state.handle, BigInt(ptr), nameBytes.length);
      }
      if (result < 0) throw new RangeError('removeAsset: asset removal failed');
      return result === 1;
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'setProvidedAssetByteLimit', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit < 0 || limit > 0xffffffff) {
      throw new TypeError('setProvidedAssetByteLimit: expected uint32 limit');
    }
    ++state.busy;
    try {
      if (Module['_lightusd_next_render_set_provided_asset_byte_limit'](state.handle, limit) !== 0) {
        throw new RangeError('setProvidedAssetByteLimit: limit is below current asset use');
      }
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'providedAssetByteLimit', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('providedAssetByteLimit: wrong argument count');
    ++state.busy;
    try {
      const limit = Module['_lightusd_next_render_provided_asset_byte_limit'](state.handle);
      if (limit < 0) throw new RangeError('providedAssetByteLimit: query failed');
      return limit;
    } finally { --state.busy; }
  }});
  const assetNameDecoder = new TextDecoder();
  Object.defineProperty(Module.RenderStream.prototype, 'providedAssetNames', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('providedAssetNames: wrong argument count');
    ++state.busy;
    const queryName = (assetId, p, cap) => {
      try { return Module['_lightusd_next_render_provided_asset_name'](
        state.handle, assetId, p, cap); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module['_lightusd_next_render_provided_asset_name'](
          state.handle, assetId, typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
      }
    };
    const copyName = (assetId, expectedBytes) => {
      let ptr = 0;
      try {
        const bytes = queryName(assetId, 0, 0);
        if (bytes !== expectedBytes) throw new RangeError('providedAssetNames: asset map changed');
        if (bytes < 0) throw new RangeError('providedAssetNames: invalid asset index');
        if (bytes === 0) return '';
        ptr = Module['_lightusd_next_alloc'](bytes);
        if (!ptr) throw new RangeError('providedAssetNames: allocation failed');
        if (queryName(assetId, ptr, bytes) !== bytes) throw new RangeError('providedAssetNames: asset map changed');
        return assetNameDecoder.decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
      } finally { if (ptr) Module['_lightusd_next_free'](ptr); }
    };
    try {
      const count = Module['_lightusd_next_render_provided_asset_count'](state.handle);
      if (!Number.isSafeInteger(count) || count < 0 || count > 65536) {
        throw new RangeError('providedAssetNames: invalid or excessive asset count');
      }
      const sizes = new Array(count);
      let aggregateBytes = count * 64;
      for (let id = 0; id < count; ++id) {
        const bytes = queryName(id, 0, 0);
        if (!Number.isSafeInteger(bytes) || bytes < 0) {
          throw new RangeError('providedAssetNames: invalid asset name size');
        }
        sizes[id] = bytes;
        aggregateBytes += bytes * 2;
        if (aggregateBytes > 0x20000000) {
          throw new RangeError('providedAssetNames: aggregate names exceed 512 MiB limit');
        }
      }
      preflightRenderAggregate(state, aggregateBytes, 'providedAssetNames');
      return Array.from({length: count}, (_, id) => copyName(id, sizes[id]));
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getProvidedAsset', {value: function(name) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof name !== 'string') {
      throw new TypeError('getProvidedAsset: expected one string name');
    }
    const nameBytes = assetEncoder.encode(name);
    if (nameBytes.length > 0xffffffff) throw new RangeError('getProvidedAsset: name is too long');
    ++state.busy;
    let namePtr = 0;
    let dataPtr = 0;
    try {
      namePtr = Module['_lightusd_next_alloc'](Math.max(nameBytes.length, 1));
      if (!namePtr) throw new RangeError('getProvidedAsset: allocation failed');
      Module.HEAPU8.set(nameBytes, Number(namePtr));
      const query = (outPtr, cap) => {
        try {
          return Module['_lightusd_next_render_provided_asset_bytes'](
            state.handle, namePtr, nameBytes.length, outPtr, cap);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_provided_asset_bytes'](
            state.handle, typeof namePtr === 'bigint' ? namePtr : BigInt(namePtr),
            nameBytes.length, typeof outPtr === 'bigint' ? outPtr : BigInt(outPtr), cap);
        }
      };
      const size = query(0, 0);
      if (size < 0) throw new RangeError('getProvidedAsset: asset not found');
      if (size === 0) return new Uint8Array(0);
      dataPtr = Module['_lightusd_next_alloc'](size);
      if (!dataPtr) throw new RangeError('getProvidedAsset: allocation failed');
      if (query(dataPtr, size) !== size) throw new RangeError('getProvidedAsset: payload changed');
      return Module.HEAPU8.slice(Number(dataPtr), Number(dataPtr) + size);
    } finally {
      if (dataPtr) Module['_lightusd_next_free'](dataPtr);
      if (namePtr) Module['_lightusd_next_free'](namePtr);
      --state.busy;
    }
  }});
  const variantDecoder = new TextDecoder();
  Object.defineProperty(Module.RenderStream.prototype, 'listVariants', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('listVariants: wrong argument count');
    ++state.busy;
    const copyString = (setId, variantId, kind) => {
      let ptr = 0;
      try {
        const query = (p, cap) => {
          try {
            return Module['_lightusd_next_render_variant_string'](
              state.handle, setId, variantId, kind, p, cap);
          } catch (error) {
            if (!(error instanceof TypeError)) throw error;
            return Module['_lightusd_next_render_variant_string'](
              state.handle, setId, variantId, kind,
              typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
          }
        };
        const bytes = query(0, 0);
        if (bytes < 0) throw new RangeError('listVariants: invalid string index');
        if (bytes === 0) return '';
        ptr = Module['_lightusd_next_alloc'](bytes);
        if (!ptr) throw new RangeError('listVariants: allocation failed');
        if (query(ptr, bytes) !== bytes) throw new RangeError('listVariants: string changed');
        return variantDecoder.decode(new Uint8Array(
          Module.HEAPU8.buffer, Number(ptr), bytes));
      } finally {
        if (ptr) Module['_lightusd_next_free'](ptr);
      }
    };
    try {
      const count = Module['_lightusd_next_render_variant_set_count'](state.handle);
      if (count < 0) throw new RangeError('listVariants: count unavailable');
      const result = [];
      for (let setId = 0; setId < count; ++setId) {
        const nameCount = Module['_lightusd_next_render_variant_name_count'](
          state.handle, setId);
        if (nameCount < 0) throw new RangeError('listVariants: invalid set');
        const variants = [];
        for (let nameId = 0; nameId < nameCount; ++nameId) {
          variants.push(copyString(setId, nameId, 3));
        }
        result.push({
          primPath: copyString(setId, 0, 0),
          setName: copyString(setId, 0, 1),
          selected: copyString(setId, 0, 2),
          variants
        });
      }
      return result;
    } finally {
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'hasVariants', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('hasVariants: wrong argument count');
    return this.listVariants().length !== 0;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'lodVariantCount', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('lodVariantCount: wrong argument count');
    let count = 0;
    for (const item of this.listVariants()) {
      if (item.setName === 'LOD') count = Math.max(count, item.variants.length);
    }
    return count;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'extractVariants', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('extractVariants: wrong argument count');
    const prims = new Map();
    for (const item of this.listVariants()) {
      let prim = prims.get(item.primPath);
      if (!prim) {
        prim = {primPath: item.primPath, variantSets: []};
        prims.set(item.primPath, prim);
      }
      prim.variantSets.push({
        name: item.setName,
        selection: item.selected,
        options: item.variants.slice()
      });
    }
    return Array.from(prims.values());
  }});
  function renderLayerAssetPaths(stream, state, kind, label) {
    const count = Module['_lightusd_next_render_layer_asset_count'](
      state.handle, kind);
    if (count < 0) throw new RangeError(`${label}: path count unavailable`);
    const result = [];
    for (let index = 0; index < count; ++index) {
      let ptr = 0;
      try {
        const query = (p, cap) => {
          try {
            return Module['_lightusd_next_render_layer_asset_string'](
              state.handle, kind, index, p, cap);
          } catch (error) {
            if (!(error instanceof TypeError)) throw error;
            return Module['_lightusd_next_render_layer_asset_string'](
              state.handle, kind, index,
              typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
          }
        };
        const bytes = query(0, 0);
        if (bytes < 0) throw new RangeError(`${label}: invalid path index`);
        if (bytes === 0) { result.push(''); continue; }
        ptr = Module['_lightusd_next_alloc'](bytes);
        if (!ptr) throw new RangeError(`${label}: allocation failed`);
        if (query(ptr, bytes) !== bytes) throw new RangeError(`${label}: path changed`);
        result.push(renderErrorDecoder.decode(new Uint8Array(
          Module.HEAPU8.buffer, Number(ptr), bytes)));
      } finally {
        if (ptr) Module['_lightusd_next_free'](ptr);
      }
    }
    return result;
  }
  for (const [name, kind] of [['extractSublayerAssetPaths', 0],
    ['extractReferencesAssetPaths', 1], ['extractPayloadAssetPaths', 2]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function() {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 0) throw new TypeError(`${name}: wrong argument count`);
      ++state.busy;
      try { return renderLayerAssetPaths(this, state, kind, name); }
      finally { --state.busy; }
    }});
  }
  for (const [name, kind] of [['hasSublayers', 0], ['hasReferences', 1],
    ['hasPayload', 2], ['hasInherits', 3], ['hasSpecializes', 4]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function() {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 0) throw new TypeError(`${name}: wrong argument count`);
      const present = Module['_lightusd_next_render_layer_arc_present'](state.handle, kind);
      if (present < 0) throw new RangeError(`${name}: arc state unavailable`);
      return present !== 0;
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'renderStats', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('renderStats: wrong argument count');
    const size = 40 + 7 * 8;
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('renderStats: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_stats_get'](state.handle, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_stats_get'](state.handle, BigInt(p));
      }
      if (status !== 0) throw new RangeError('renderStats: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      const names = ['sourceMeshes', 'sourceMaterials', 'sourceTextures',
        'optimizedMeshes', 'optimizedMaterials', 'optimizedTextures',
        'mergedMeshes', 'mergeGroups', 'skippedMergeMeshes'];
      const out = {};
      names.forEach((name, i) => { out[name] = view.getInt32(4 + i * 4, true); });
      const doubles = ['stageLoadMs', 'inputCopyMs', 'inputBytes',
        'stageMemoryBytes', 'renderSceneMemoryBytes', 'geometryBorrowedBytes',
        'geometryMaterializedBytes'];
      doubles.forEach((name, i) => { out[name] = view.getFloat64(40 + i * 8, true); });
      return out;
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getStats', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getStats: wrong argument count');
    const base = this.renderStats();
    const size = 200;
    ++state.busy;
    let ptr = 0;
    let detail;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getStats: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_stats_detail_get'](state.handle, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_stats_detail_get'](state.handle, BigInt(p));
      }
      if (status !== 0) throw new RangeError('getStats: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      detail = {
        flags: view.getUint32(4, true),
        timings: Array.from({length: 8}, (_, i) => view.getFloat64(8 + i * 8, true)),
        memory: Array.from({length: 6}, (_, i) => view.getFloat64(72 + i * 8, true)),
        cache: Array.from({length: 4}, (_, i) => view.getInt32(120 + i * 4, true)),
        scene: Array.from({length: 15}, (_, i) => view.getInt32(136 + i * 4, true))
      };
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    const s = {
      sourceMeshes: base.sourceMeshes,
      sourceMaterials: base.sourceMaterials,
      sourceTextures: base.sourceTextures,
      optimizedMeshes: base.optimizedMeshes,
      optimizedMaterials: base.optimizedMaterials,
      optimizedTextures: base.optimizedTextures,
      mergedMeshes: base.mergedMeshes,
      mergeGroups: base.mergeGroups,
      skippedMergeMeshes: base.skippedMergeMeshes,
      materialDedup: !!(detail.flags & 1),
      meshMerge: !!(detail.flags & 2),
      meshMergeBakeTransform: !!(detail.flags & 4),
      flattenRenderTree: !!(detail.flags & 8),
      nativeStageLoadMs: base.stageLoadMs,
      nativeInputCopyMs: base.inputCopyMs,
      nativeInputBytes: base.inputBytes,
      nativeCompositionMs: detail.timings[0],
      nativeMeshDiscoveryMs: detail.timings[1],
      nativeOptimizeMs: detail.timings[2],
      nativeMaterialMs: detail.timings[3],
      nativeMaterialIdentityMs: detail.timings[4],
      nativeMaterialConversionMs: detail.timings[5],
      nativeGeometryBuildMs: detail.timings[6],
      nativeMergeAppendMs: detail.timings[7],
      materialIdentityHits: detail.cache[0],
      materialIdentityMisses: detail.cache[1],
      materialGraphCacheHits: detail.cache[2],
      materialGraphCacheMisses: detail.cache[3],
      geometryBorrowedBytes: base.geometryBorrowedBytes,
      geometryMaterializedBytes: base.geometryMaterializedBytes,
      providedAssetBytes: detail.memory[0],
      stageMemoryBytes: base.stageMemoryBytes
    };
    if (detail.flags & 16) {
      s.renderSceneMemoryBytes = base.renderSceneMemoryBytes;
      const memoryNames = ['renderMeshPointsBytes', 'renderMeshNormalsBytes',
        'renderMeshUvBytes', 'renderMeshTopologyBytes', 'renderMeshTriangulationBytes'];
      memoryNames.forEach((name, i) => { s[name] = detail.memory[i + 1]; });
    }
    const sceneNames = ['renderSceneNodes', 'renderSceneMeshes', 'renderScenePoints',
      'renderSceneCurves', 'renderScenePointInstancers', 'renderScenePointInstanceDraws',
      'renderSceneMaterials', 'renderSceneTextures', 'renderSceneImages',
      'renderSceneLights', 'renderSceneCameras', 'renderSceneAnimations',
      'renderSceneSkeletons', 'renderSceneUnsupportedRenderables',
      'renderSceneWarnings'];
    sceneNames.forEach((name, i) => { s[name] = detail.scene[i]; });
    return s;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'animationInfo', {value: function(animationId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof animationId !== 'number') {
      throw new TypeError('animationInfo: expected one numeric animation id');
    }
    const size = 40;
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('animationInfo: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_animation_info_get'](state.handle, animationId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_animation_info_get'](state.handle, animationId, BigInt(p));
      }
      if (status !== 0) throw new RangeError('animationInfo: invalid animation id');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      return {
        id: view.getInt32(4, true),
        startTime: view.getFloat64(8, true), endTime: view.getFloat64(16, true),
        numTracks: view.getInt32(24, true), targetNodeCount: view.getInt32(28, true),
        numClipAssetPaths: view.getInt32(32, true), flags: view.getInt32(36, true),
      };
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'animationClipAssetPath', {value: function(animationId, assetId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof animationId !== 'number' || typeof assetId !== 'number') {
      throw new TypeError('animationClipAssetPath: expected numeric animation and asset ids');
    }
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module['_lightusd_next_render_animation_clip_asset'](state.handle, animationId, assetId, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_animation_clip_asset'](
            state.handle, animationId, assetId,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError('animationClipAssetPath: invalid animation or asset id');
      if (bytes === 0) return '';
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError('animationClipAssetPath: allocation failed');
      query(ptr, bytes);
      return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  const animationInfoDecoder = new TextDecoder();
  const animationInfoPayloadEstimate = (stream, animationId, clip) => {
    if (!Number.isSafeInteger(clip.numClipAssetPaths) || clip.numClipAssetPaths < 0 ||
        clip.numClipAssetPaths > 65536) {
      throw new RangeError('animation info: invalid or excessive clip asset count');
    }
    const state = live.get(stream);
    let total = 512;
    const charge = bytes => {
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('animation info: string size query failed');
      }
      total += bytes * 2 + 64;
      if (total > 0x20000000) {
        throw new RangeError('animation info: returned data exceeds 512 MiB limit');
      }
    };
    const querySize = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    charge(querySize('_lightusd_next_render_resource_name', [8, animationId]));
    for (let assetId = 0; assetId < clip.numClipAssetPaths; ++assetId) {
      const bytes = querySize('_lightusd_next_render_animation_clip_asset',
        [animationId, assetId]);
      if (bytes < 0) throw new RangeError('animation info: clip path query failed');
      charge(bytes);
    }
    return total;
  };
  Object.defineProperty(Module.RenderStream.prototype, 'getAnimationInfo', {value: function(animationId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof animationId !== 'number') {
      throw new TypeError('getAnimationInfo: expected one numeric animation id');
    }
    if (!Number.isInteger(animationId) || animationId < 0 || animationId >= this.numAnimations()) {
      return {};
    }
    const clip = this.animationInfo(animationId);
    animationInfoPayloadEstimate(this, animationId, clip);
    const name = animationInfoDecoder.decode(this.resourceNameBuffer(8, animationId));
    const clipAssetPaths = Array.from({length: clip.numClipAssetPaths},
      (_, i) => this.animationClipAssetPath(animationId, i));
    const skeletal = !!(clip.flags & 1);
    const baked = !!(clip.flags & 4);
    return {
      id: animationId,
      name: name || 'Animation' + animationId,
      // Like legacy, duration is the last key time: keyframe times are
      // absolute, so clips that start after 0 must still reach their end.
      duration: Number.isFinite(clip.endTime) ? Math.max(0, clip.endTime) : 0,
      numTracks: clip.numTracks,
      numSamplers: clip.numTracks,
      numTargetNodes: clip.targetNodeCount,
      has_skeletal_animation: skeletal,
      has_node_animation: !!(clip.flags & 2),
      startTime: clip.startTime,
      endTime: clip.endTime,
      clipAssetPaths,
      numClipAssetPaths: clip.numClipAssetPaths,
      valueClipBaked: baked,
      sourceType: baked ? 'ValueClip' : skeletal ? 'SkelAnimation' : 'XformOp'
    };
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getAllAnimationInfos', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getAllAnimationInfos: wrong argument count');
    const count = this.numAnimations();
    if (!Number.isSafeInteger(count) || count < 0 || count > 65536) {
      throw new RangeError('getAllAnimationInfos: invalid or excessive animation count');
    }
    let aggregateBytes = 0;
    for (let animationId = 0; animationId < count; ++animationId) {
      const clip = this.animationInfo(animationId);
      aggregateBytes += animationInfoPayloadEstimate(this, animationId, clip);
      if (aggregateBytes > 0x20000000) {
        throw new RangeError('getAllAnimationInfos: aggregate returned data exceeds 512 MiB limit');
      }
    }
    preflightRenderAggregate(state, aggregateBytes, 'getAllAnimationInfos');
    return Array.from({length: count}, (_, i) => this.getAnimationInfo(i));
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'animationChannelCount', {value: function(animationId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof animationId !== 'number') {
      throw new TypeError('animationChannelCount: expected one numeric animation id');
    }
    ++state.busy;
    try { return Module['_lightusd_next_render_animation_channel_count'](state.handle, animationId); }
    finally { --state.busy; }
  }});
  const animationChannelFields = [
    ['animationChannelTargetNode', 0], ['animationChannelTargetSkeleton', 1],
    ['animationChannelKeyframeCount', 2], ['animationChannelElementCount', 3],
    ['animationChannelValueStride', 4], ['animationChannelInterpolation', 5],
    ['animationChannelSkeletal', 6], ['animationChannelTargetPath', 7],
    ['animationChannelJointOrderCount', 8], ['animationChannelBlendShapeOrderCount', 9],
    ['animationChannelJointRemapCount', 10]
  ];
  for (const [name, field] of animationChannelFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(animationId, channelId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 2 || typeof animationId !== 'number' || typeof channelId !== 'number') {
        throw new TypeError(name + ': expected numeric animation and channel ids');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_animation_channel_field'](
          state.handle, animationId, channelId, field);
      } finally { --state.busy; }
    }});
  }
  const animationChannelString = (receiver, animationId, channelId, kind, name) => {
    const state = live.get(receiver);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try {
          return Module['_lightusd_next_render_animation_channel_string'](
            state.handle, animationId, channelId, kind, p, cap);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_animation_channel_string'](
            state.handle, animationId, channelId, kind,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError(name + ': invalid animation channel');
      if (bytes === 0) return '';
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError(name + ': allocation failed');
      query(ptr, bytes);
      return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  };
  for (const [name, kind] of [['animationChannelTargetPrimPath', 0],
                               ['animationChannelPropertyName', 1],
                               ['animationChannelTargetSkeletonPath', 2]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(animationId, channelId) {
      if (arguments.length !== 2 || typeof animationId !== 'number' || typeof channelId !== 'number') {
        throw new TypeError(name + ': expected numeric animation and channel ids');
      }
      return animationChannelString(this, animationId, channelId, kind, name);
    }});
  }
  for (const [name, kind] of [['animationChannelJointOrder', 0],
                               ['animationChannelBlendShapeOrder', 1]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(animationId, channelId, orderId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 3 || typeof animationId !== 'number' ||
          typeof channelId !== 'number' || typeof orderId !== 'number') {
        throw new TypeError(name + ': expected numeric animation, channel and order ids');
      }
      ++state.busy;
      let ptr = 0;
      try {
        const query = (p, cap) => {
          try {
            return Module['_lightusd_next_render_animation_channel_order_string'](
              state.handle, animationId, channelId, kind, orderId, p, cap);
          } catch (error) {
            if (!(error instanceof TypeError)) throw error;
            return Module['_lightusd_next_render_animation_channel_order_string'](
              state.handle, animationId, channelId, kind, orderId,
              typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
          }
        };
        const bytes = query(0, 0);
        if (bytes < 0) throw new RangeError(name + ': invalid animation order');
        if (bytes === 0) return '';
        ptr = Module['_lightusd_next_alloc'](bytes);
        if (!ptr) throw new RangeError(name + ': allocation failed');
        query(ptr, bytes);
        return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
      } finally {
        if (ptr) Module['_lightusd_next_free'](ptr);
        --state.busy;
      }
    }});
  }
  const copyRenderBuffer = (receiver, symbol, args, name, ArrayType, invalidMessage) => {
    const state = live.get(receiver);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module[symbol](state.handle, ...args, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module[symbol](state.handle, ...args,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError(name + ': ' + invalidMessage);
      if (!Number.isSafeInteger(bytes) ||
          bytes % ArrayType.BYTES_PER_ELEMENT !== 0) {
        throw new RangeError(name + ': invalid buffer size');
      }
      if (bytes === 0) return new ArrayType(0);
      if (bytes > 0x20000000) {
        throw new RangeError(name + ': buffer exceeds 512 MiB limit');
      }
      const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
      if (!Number.isSafeInteger(remaining) || remaining < 0 || bytes > remaining) {
        throw new RangeError(name + ': buffer exceeds remaining memory limit');
      }
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError(name + ': allocation failed');
      if (query(ptr, bytes) !== bytes) throw new RangeError(name + ': copy failed');
      return new ArrayType(new Uint8Array(
        Module.HEAPU8.buffer, Number(ptr), bytes).slice().buffer);
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  };
  const preflightRenderAggregate = (state, bytes, name) => {
    if (!Number.isSafeInteger(bytes) || bytes < 0) {
      throw new RangeError(name + ': invalid aggregate size');
    }
    const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
    if (!Number.isSafeInteger(remaining) || remaining < 0 || bytes > remaining) {
      throw new RangeError(name + ': aggregate exceeds remaining memory limit');
    }
  };
  const animationChannelBuffer = (receiver, animationId, channelId, kind, name) =>
    copyRenderBuffer(receiver, '_lightusd_next_render_animation_channel_buffer',
      [animationId, channelId, kind], name,
      kind === 0 ? Float64Array : kind === 3 ? Int32Array : Float32Array,
      'invalid animation channel');
  for (const [name, kind] of [['animationKeyframeTimesBuffer', 0],
                               ['animationKeyframeValuesBuffer', 1],
                               ['animationArrayValuesBuffer', 2],
                               ['animationJointRemapBuffer', 3]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(animationId, channelId) {
      if (arguments.length !== 2 || typeof animationId !== 'number' || typeof channelId !== 'number') {
        throw new TypeError(name + ': expected numeric animation and channel ids');
      }
      return animationChannelBuffer(this, animationId, channelId, kind, name);
    }});
  }

  Object.defineProperty(Module.RenderStream.prototype, 'animationArrayView', {value: function(animationId, channelId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof animationId !== 'number' || typeof channelId !== 'number') {
      throw new TypeError('animationArrayView: expected numeric animation and channel ids');
    }
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](24);
      if (!ptr) throw new RangeError('animationArrayView: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, 24, true);
      const query = (out) => Module['_lightusd_next_render_animation_array_view_get'](
        state.handle, animationId, channelId, out);
      let status;
      try { status = query(ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = query(BigInt(p));
      }
      if (status !== 0) throw new RangeError('animationArrayView: invalid animation channel');
      const view = new DataView(Module.HEAPU8.buffer, p, 24);
      const length = view.getUint32(16, true);
      return {ptr: Number(view.getBigUint64(8, true)), length,
        comps: view.getUint32(20, true), dtype: 'f32', byteLength: length * 4};
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});

  const animationPaths = ['Translation', 'Rotation', 'Scale', 'Weights', 'CustomProperty'];
  const animationInterpolations = ['STEP', 'LINEAR', 'CUBICSPLINE'];
  const animationDecoder = new TextDecoder();
  const kMaxAnimationAggregateBytes = 0x20000000;
  const animationPayloadEstimate = (stream, animationId) => {
    const state = live.get(stream);
    const channelCount = stream.animationChannelCount(animationId);
    if (!Number.isSafeInteger(channelCount) || channelCount < 0 || channelCount > 65536) {
      throw new RangeError('animation payload: invalid or excessive channel count');
    }
    let total = channelCount * 256;
    const queryStringBytes = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    const chargeString = bytes => {
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('animation payload: string size query failed');
      }
      // UTF-16 JS storage can require at most twice the UTF-8 byte length;
      // include per-string object/array-slot allowance in the budget too.
      total += bytes * 2 + 64;
      if (total > kMaxAnimationAggregateBytes) {
        throw new RangeError('animation payload: aggregate returned data exceeds 512 MiB limit');
      }
    };
    chargeString(queryStringBytes('_lightusd_next_render_resource_name', [8, animationId]));
    chargeString(queryStringBytes('_lightusd_next_render_resource_path', [8, animationId]));
    for (let channelId = 0; channelId < channelCount; ++channelId) {
      const queryBytes = kind => {
        try {
          return Module['_lightusd_next_render_animation_channel_buffer'](
            state.handle, animationId, channelId, kind, 0, 0);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_animation_channel_buffer'](
            state.handle, animationId, channelId, kind, BigInt(0), 0);
        }
      };
      const byteWeights = [3, 4, 4, 5];
      for (let kind = 0; kind < byteWeights.length; ++kind) {
        const bytes = queryBytes(kind);
        if (!Number.isSafeInteger(bytes) || bytes < 0) {
          throw new RangeError('animation payload: channel buffer size query failed');
        }
        total += bytes * byteWeights[kind];
        if (total > kMaxAnimationAggregateBytes) {
          throw new RangeError('animation payload: aggregate returned data exceeds 512 MiB limit');
        }
      }
      for (let kind = 0; kind < 3; ++kind) {
        chargeString(queryStringBytes(
          '_lightusd_next_render_animation_channel_string',
          [animationId, channelId, kind]));
      }
      const jointOrderCount = stream.animationChannelJointOrderCount(animationId, channelId);
      const blendShapeOrderCount = stream.animationChannelBlendShapeOrderCount(animationId, channelId);
      if (!Number.isSafeInteger(jointOrderCount) || jointOrderCount < 0 ||
          !Number.isSafeInteger(blendShapeOrderCount) || blendShapeOrderCount < 0 ||
          jointOrderCount + blendShapeOrderCount > 65536) {
        throw new RangeError('animation payload: invalid or excessive order count');
      }
      for (const [kind, count] of [[0, jointOrderCount], [1, blendShapeOrderCount]]) {
        for (let orderId = 0; orderId < count; ++orderId) {
          chargeString(queryStringBytes(
            '_lightusd_next_render_animation_channel_order_string',
            [animationId, channelId, kind, orderId]));
        }
      }
    }
    return total;
  };
  const buildAnimation = (stream, animationId, viewMode) => {
    animationPayloadEstimate(stream, animationId);
    const clip = stream.animationInfo(animationId);
    const count = stream.animationChannelCount(animationId);
    const channels = [], samplers = [], tracks = [];
    for (let i = 0; i < count; ++i) {
      const targetPath = stream.animationChannelTargetPath(animationId, i);
      const path = animationPaths[targetPath] || 'Unknown';
      const interpolation = animationInterpolations[stream.animationChannelInterpolation(animationId, i)] || 'LINEAR';
      const isSkeletal = !!stream.animationChannelSkeletal(animationId, i);
      const valueStride = stream.animationChannelValueStride(animationId, i);
      const elementCount = stream.animationChannelElementCount(animationId, i);
      const targetNode = stream.animationChannelTargetNode(animationId, i);
      const targetSkeleton = stream.animationChannelTargetSkeleton(animationId, i);
      const propertyName = stream.animationChannelPropertyName(animationId, i);
      const targetSkeletonPath = stream.animationChannelTargetSkeletonPath(animationId, i);
      const jointRemap = Array.from(stream.animationJointRemapBuffer(animationId, i));
      const times = Array.from(stream.animationKeyframeTimesBuffer(animationId, i), Math.fround);
      const values = Array.from(stream.animationKeyframeValuesBuffer(animationId, i));
      const sampler = {index: i, interpolation, times, values, valueStride, elementCount, isSkeletal};
      const arrayValues = viewMode ? stream.animationArrayView(animationId, i) :
        stream.animationArrayValuesBuffer(animationId, i);
      if (arrayValues.length) {
        sampler.arrayValues = viewMode ? arrayValues : Array.from(arrayValues);
      }
      samplers.push(sampler);
      channels.push({sampler: i, target_node: targetNode,
        target_prim_path: stream.animationChannelTargetPrimPath(animationId, i),
        target_type: isSkeletal ? 'SkelAnimation' : 'SceneNode',
        skeleton_id: targetSkeleton, joint_id: -1, path,
        isCustomProperty: targetPath === 4, propertyName, isSkeletal,
        targetSkeletonPath,
        jointOrder: Array.from({length: stream.animationChannelJointOrderCount(animationId, i)},
          (_, j) => stream.animationChannelJointOrder(animationId, i, j)),
        jointRemap,
        blendShapeOrder: Array.from({length: stream.animationChannelBlendShapeOrderCount(animationId, i)},
          (_, j) => stream.animationChannelBlendShapeOrder(animationId, i, j)),
        valueStride, elementCount});
      if (!viewMode) {
        const type = targetPath === 0 || targetPath === 2
          ? (isSkeletal ? 'vector3Array' : 'vector3')
          : targetPath === 1 ? (isSkeletal ? 'quaternionArray' : 'quaternion')
          : targetPath === 3 && isSkeletal ? 'weightArray' : 'number';
        const track = {sampler: i, target_node: targetNode, path, interpolation,
          times: times.slice(), values: values.slice(), isSkeletal, propertyName,
          targetSkeletonId: targetSkeleton, targetSkeletonPath,
          jointRemap: jointRemap.slice(), valueStride, elementCount, name: path, type};
        if (arrayValues.length) track.arrayValues = Array.from(arrayValues);
        tracks.push(track);
      }
    }
    // Legacy duration: the last key time (keyframe times are absolute).
    const duration = clip.endTime;
    const out = {index: animationId,
      name: animationDecoder.decode(stream.resourceNameBuffer(8, animationId)) || 'Animation' + animationId,
      primPath: animationDecoder.decode(stream.resourcePathBuffer(8, animationId)),
      startTime: clip.startTime, endTime: clip.endTime,
      duration: Number.isFinite(duration) ? Math.max(0, duration) : 0,
      channels, samplers, numChannels: count, numSamplers: count,
      has_skeletal_animation: !!(clip.flags & 1), has_node_animation: count > 0};
    if (!viewMode) out.tracks = tracks;
    return out;
  };
  for (const [name, viewMode] of [['getAnimation', false], ['getAnimationView', true]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(animationId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 1 || typeof animationId !== 'number') {
        throw new TypeError(name + ': expected one numeric animation id');
      }
      if (!Number.isInteger(animationId) || animationId < 0 || animationId >= this.numAnimations()) return {};
      return buildAnimation(this, animationId, viewMode);
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'getAllAnimations', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getAllAnimations: wrong argument count');
    let aggregateBytes = 0;
    for (let animationId = 0; animationId < this.numAnimations(); ++animationId) {
      aggregateBytes += animationPayloadEstimate(this, animationId);
      if (aggregateBytes > kMaxAnimationAggregateBytes) {
        throw new RangeError('getAllAnimations: aggregate returned data exceeds 512 MiB limit');
      }
    }
    preflightRenderAggregate(state, aggregateBytes, 'getAllAnimations');
    return Array.from({length: this.numAnimations()}, (_, i) => this.getAnimation(i));
  }});

  const renderNodeFields = [
    ['nodeType', 0], ['nodeParentId', 1],
    ['nodeDataId', 2], ['nodeVisible', 3],
    ['nodeHasResetXform', 4], ['nodeIsInstance', 5]
  ];
  for (const [name, field] of renderNodeFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(nodeId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) {
        throw new TypeError('Invalid LightUSD receiver');
      }
      if (arguments.length !== 1 || typeof nodeId !== 'number') {
        throw new TypeError(name + ': expected one numeric node id');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_node_field'](state.handle, nodeId, field);
      } finally {
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'nativeInstanceNodeIds', {value: function() {
    if (arguments.length !== 0) throw new TypeError('nativeInstanceNodeIds: wrong argument count');
    const nodeCount = this.nodeCount();
    if (!Number.isSafeInteger(nodeCount) || nodeCount < 0 || nodeCount > 1048576) {
      throw new RangeError('nativeInstanceNodeIds: node count exceeds 1 Mi limit');
    }
    return Array.from(copyRenderBuffer(this,
      '_lightusd_next_render_native_instance_node_ids', [],
      'nativeInstanceNodeIds', Uint32Array, 'invalid render stream'));
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'nodeChildId', {value: function(nodeId, childIndex) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) {
      throw new TypeError('Invalid LightUSD receiver');
    }
    if (arguments.length !== 2 || typeof nodeId !== 'number' ||
        typeof childIndex !== 'number') {
      throw new TypeError('nodeChildId: expected two numeric indices');
    }
    ++state.busy;
    try {
      return Module['_lightusd_next_render_node_child'](state.handle, nodeId, childIndex);
    } finally {
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'nodeChildCount', {value: function(nodeId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof nodeId !== 'number' || !Number.isInteger(nodeId)) {
      throw new TypeError('nodeChildCount: expected one integer node id');
    }
    ++state.busy;
    try { return Module['_lightusd_next_render_node_child_count'](state.handle, nodeId); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'rootNodeId', {value: function(rootIndex) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof rootIndex !== 'number') {
      throw new TypeError('rootNodeId: expected one numeric root index');
    }
    ++state.busy;
    try {
      return Module['_lightusd_next_render_root_node'](state.handle, rootIndex);
    } finally {
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'nodePathBuffer', {value: function(nodeId) {
    if (arguments.length !== 1 || typeof nodeId !== 'number') {
      throw new TypeError('nodePathBuffer: expected one numeric node id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_node_path',
      [nodeId], 'nodePathBuffer', Uint8Array, 'invalid node id');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'nodePrototypePathBuffer', {value: function(nodeId) {
    if (arguments.length !== 1 || typeof nodeId !== 'number' || !Number.isInteger(nodeId)) {
      throw new TypeError('nodePrototypePathBuffer: expected one integer node id');
    }
    return outputString(this, '_lightusd_next_render_node_prototype_path',
      [nodeId], 'nodePrototypePathBuffer');
  }});
  const nodeTransformBuffer = (receiver, nodeId, kind, name) =>
    copyRenderBuffer(receiver, '_lightusd_next_render_node_transform',
      [nodeId, kind], name, Float32Array, 'invalid node id');
  for (const [name, kind] of [['nodeLocalTransformBuffer', 0], ['nodeWorldTransformBuffer', 1]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(nodeId) {
      if (arguments.length !== 1 || typeof nodeId !== 'number') {
        throw new TypeError(name + ': expected one numeric node id');
      }
      return nodeTransformBuffer(this, nodeId, kind, name);
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'resourcePathBuffer', {value: function(kind, resourceId) {
    if (arguments.length !== 2 || typeof kind !== 'number' || typeof resourceId !== 'number') {
      throw new TypeError('resourcePathBuffer: expected numeric kind and resource id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_resource_path',
      [kind, resourceId], 'resourcePathBuffer', Uint8Array, 'invalid resource');
  }});
  const resourceNameBuffer = (receiver, kind, resourceId) =>
    copyRenderBuffer(receiver, '_lightusd_next_render_resource_name',
      [kind, resourceId], 'resourceNameBuffer', Uint8Array, 'invalid resource');
  Object.defineProperty(Module.RenderStream.prototype, 'resourceNameBuffer', {
    value: function(kind, resourceId) {
      if (arguments.length !== 2 || typeof kind !== 'number' || typeof resourceId !== 'number') {
        throw new TypeError('resourceNameBuffer: expected numeric kind and resource id');
      }
      return resourceNameBuffer(this, kind, resourceId);
    }
  });
  // Reconstruct the compatibility object from bounded C/POD queries. Node
  // payloads no longer cross the generic emval dispatcher.
  const nodeTypeNames = [
    'xform', 'mesh', 'points', 'pointInstancer', 'camera',
    'pointLight', 'directionalLight', 'spotLight', 'rectLight',
    'diskLight', 'domeLight', 'sphereLight', 'skeleton', 'curves'
  ];
  const nodeDecoder = new TextDecoder();
  Object.defineProperty(Module.RenderStream.prototype, 'getNode', {value: function(nodeId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof nodeId !== 'number') {
      throw new TypeError('getNode: expected one numeric node id');
    }
    const nodeCount = this.nodeCount();
    if (!Number.isInteger(nodeId) || nodeId < 0 || nodeId >= nodeCount) {
      return {error: 'invalid node index'};
    }
    const typeCode = this.nodeType(nodeId);
    const childCount = this.nodeChildCount(nodeId);
    if (childCount < 0) return {error: 'invalid node index'};
    const queryStringSize = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    let payloadEstimate = 2048 + childCount * 8;
    for (const [symbol, args] of [
      ['_lightusd_next_render_resource_name', [0, nodeId]],
      ['_lightusd_next_render_node_path', [nodeId]],
      ['_lightusd_next_render_node_prototype_path', [nodeId]]
    ]) {
      const bytes = queryStringSize(symbol, args);
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('getNode: string size query failed');
      }
      payloadEstimate += bytes * 2 + 64;
    }
    if (!Number.isSafeInteger(payloadEstimate) || payloadEstimate > 0x20000000) {
      throw new RangeError('getNode: node payload exceeds 512 MiB limit');
    }
    const children = [];
    for (let i = 0; i < childCount; ++i) {
      const child = this.nodeChildId(nodeId, i);
      if (child < 0) return {error: 'invalid child index'};
      children.push(child);
    }
    return {
      index: nodeId,
      name: nodeDecoder.decode(this.resourceNameBuffer(0, nodeId)),
      primPath: nodeDecoder.decode(this.nodePathBuffer(nodeId)),
      type: nodeTypeNames[typeCode] || 'unknown',
      visible: this.nodeVisible(nodeId) !== 0,
      hasResetXform: this.nodeHasResetXform(nodeId) !== 0,
      isInstance: this.nodeIsInstance(nodeId) !== 0,
      prototypePath: this.nodePrototypePathBuffer(nodeId),
      dataId: this.nodeDataId(nodeId),
      parentId: this.nodeParentId(nodeId),
      localMatrix: Array.from(this.nodeLocalTransformBuffer(nodeId)),
      worldMatrix: Array.from(this.nodeWorldTransformBuffer(nodeId)),
      children
    };
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getRootNode', {value: function(rootIndex) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof rootIndex !== 'number' || !Number.isInteger(rootIndex)) {
      throw new TypeError('getRootNode: expected one integer root index');
    }
    const rootId = this.rootNodeId(rootIndex);
    if (rootId < 0) return {};
    const queryStringSize = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    let estimate = 0;
    let visitedCount = 0;
    const activePreflight = new Set();
    const preflight = [{id: rootId, exit: false}];
    const charge = bytes => {
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('getRootNode: invalid hierarchy payload size');
      }
      estimate += bytes;
      if (estimate > 0x20000000) {
        throw new RangeError('getRootNode: hierarchy exceeds 512 MiB limit');
      }
    };
    while (preflight.length) {
      const frame = preflight.pop();
      if (frame.exit) {
        activePreflight.delete(frame.id);
        continue;
      }
      if (activePreflight.has(frame.id)) throw new RangeError('getRootNode: cyclic hierarchy');
      if (++visitedCount > 262144) {
        throw new RangeError('getRootNode: excessive hierarchy node count');
      }
      activePreflight.add(frame.id);
      charge(2048);
      charge(queryStringSize('_lightusd_next_render_resource_name', [0, frame.id]) * 2 + 64);
      charge(queryStringSize('_lightusd_next_render_node_path', [frame.id]) * 2 + 64);
      charge(queryStringSize('_lightusd_next_render_node_prototype_path', [frame.id]) * 2 + 64);
      const childCount = this.nodeChildCount(frame.id);
      if (!Number.isInteger(childCount) || childCount < 0 || childCount > 65536) {
        throw new RangeError('getRootNode: invalid or excessive child count');
      }
      charge(childCount * 8);
      preflight.push({id: frame.id, exit: true});
      for (let i = childCount - 1; i >= 0; --i) {
        const childId = this.nodeChildId(frame.id, i);
        if (!Number.isInteger(childId) || childId < 0) {
          throw new RangeError('getRootNode: invalid child index');
        }
        preflight.push({id: childId, exit: false});
      }
    }
    const active = new Set([rootId]);
    const nodeCategories = [
      'group', 'geom', 'geom', 'geom', 'camera',
      'light', 'light', 'light', 'light', 'light', 'light', 'light',
      'skeleton', 'geom'
    ];
    const makeTree = node => ({
        ...node,
        primName: node.name,
        displayName: node.name,
        absPath: node.primPath,
        nodeType: node.type,
        nodeCategory: nodeCategories[this.nodeType(node.index)] || 'unknown',
        contentId: node.dataId,
        globalMatrix: node.worldMatrix.slice(),
        children: []
      });
    const root = this.getNode(rootId);
    if (root.error) throw new RangeError('getRootNode: invalid hierarchy node');
    const tree = makeTree(root);
    const stack = [{node: root, tree, nextChild: 0}];
    while (stack.length) {
      const frame = stack[stack.length - 1];
      if (frame.nextChild >= frame.node.children.length) {
        active.delete(frame.node.index);
        stack.pop();
        continue;
      }
      const childId = frame.node.children[frame.nextChild++];
      if (active.has(childId)) throw new RangeError('getRootNode: cyclic hierarchy');
      const child = this.getNode(childId);
      if (child.error) throw new RangeError('getRootNode: invalid hierarchy node');
      const childTree = makeTree(child);
      frame.tree.children.push(childTree);
      active.add(childId);
      stack.push({node: child, tree: childTree, nextChild: 0});
    }
    return tree;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getDefaultRootNode', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getDefaultRootNode: wrong argument count');
    return this.getRootNode(0);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'recordPathBuffer', {value: function(kind, recordId) {
    if (arguments.length !== 2 || typeof kind !== 'number' || typeof recordId !== 'number') {
      throw new TypeError('recordPathBuffer: expected numeric kind and record id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_record_path',
      [kind, recordId], 'recordPathBuffer', Uint8Array, 'invalid record');
  }});
  for (const [name, kind] of [['pointsPositionsBuffer', 0], ['pointsWidthsBuffer', 1], ['pointsColorsBuffer', 2]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(pointsId) {
      if (arguments.length !== 1 || typeof pointsId !== 'number') {
        throw new TypeError(name + ': expected one numeric points id');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_points_buffer',
        [pointsId, kind], name, Float32Array, 'invalid points id');
    }});
  }
  const pointsDecoder = new TextDecoder();
  Object.defineProperty(Module.RenderStream.prototype, 'getPoints', {value: function(pointsId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof pointsId !== 'number') {
      throw new TypeError('getPoints: expected one numeric points id');
    }
    if (!Number.isInteger(pointsId) || pointsId < 0 || pointsId >= this.pointsCount()) {
      return {error: 'invalid points index'};
    }
    const size = 40;
    ++state.busy;
    let ptr = 0;
    let info;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getPoints: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_points_info_get'](state.handle, pointsId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_points_info_get'](state.handle, pointsId, BigInt(p));
      }
      if (status !== 0) throw new RangeError('getPoints: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      info = {
        pointCount: view.getInt32(4, true),
        materialId: view.getInt32(8, true),
        hasBounds: view.getInt32(12, true) !== 0,
        bboxMin: Array.from({length: 3}, (_, i) => view.getFloat32(16 + i * 4, true)),
        bboxMax: Array.from({length: 3}, (_, i) => view.getFloat32(28 + i * 4, true))
      };
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    const out = {
      index: pointsId,
      name: pointsDecoder.decode(this.resourceNameBuffer(12, pointsId)),
      primPath: pointsDecoder.decode(this.resourcePathBuffer(12, pointsId)),
      pointCount: info.pointCount,
      materialId: info.materialId,
      points: this.pointsPositionsBuffer(pointsId),
      hasBounds: info.hasBounds
    };
    const widths = this.pointsWidthsBuffer(pointsId);
    const colors = this.pointsColorsBuffer(pointsId);
    if (widths.length) out.widths = widths;
    if (colors.length) out.colors = colors;
    if (info.hasBounds) {
      out.bboxMin = info.bboxMin;
      out.bboxMax = info.bboxMax;
    }
    return out;
  }});
  for (const [name, kind] of [['curvesControlPointsBuffer', 0], ['curvesTessellatedPointsBuffer', 1], ['curvesWidthsBuffer', 2], ['curvesColorsBuffer', 3],
                               ['curvesTessellatedWidthsBuffer', 6], ['curvesTessellatedColorsBuffer', 7],
                               ['curvesOpacitiesBuffer', 8], ['curvesTessellatedOpacitiesBuffer', 9]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(curvesId) {
      if (arguments.length !== 1 || typeof curvesId !== 'number') {
        throw new TypeError(name + ': expected one numeric curves id');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_curves_buffer',
        [curvesId, kind], name, Float32Array, 'invalid curves id');
    }});
  }
  for (const [name, kind] of [['curvesVertexCountsBuffer', 4], ['curvesTessellatedVertexCountsBuffer', 5]]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(curvesId) {
      if (arguments.length !== 1 || typeof curvesId !== 'number') {
        throw new TypeError(name + ': expected one numeric curves id');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_curves_buffer',
        [curvesId, kind], name, Uint32Array, 'invalid curves id');
    }});
  }
  const curvesFields = [
    ['curvesType', 0], ['curvesBasis', 1], ['curvesWrap', 2],
    ['curvesIsNurbs', 3], ['curvesIsHermite', 4],
    ['curvesWidthsInterpolation', 5], ['curvesColorsInterpolation', 6],
    ['curvesOpacitiesInterpolation', 7]
  ];
  for (const [name, field] of curvesFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(curvesId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 1 || typeof curvesId !== 'number') {
        throw new TypeError(name + ': expected one numeric curves id');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_curves_field'](state.handle, curvesId, field);
      } finally { --state.busy; }
    }});
  }
  const curvesDecoder = new TextDecoder();
  const curvesBasisNames = ['bezier', 'bspline', 'catmullRom'];
  const curvesWrapNames = ['nonperiodic', 'periodic', 'pinned'];
  const curvesInterpolationNames = ['constant', 'uniform', 'vertex', 'faceVarying', 'varying'];
  Object.defineProperty(Module.RenderStream.prototype, 'getCurves', {value: function(curvesId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof curvesId !== 'number') {
      throw new TypeError('getCurves: expected one numeric curves id');
    }
    if (!Number.isInteger(curvesId) || curvesId < 0 || curvesId >= this.curvesCount()) {
      return {error: 'invalid curves index'};
    }
    const size = 48;
    ++state.busy;
    let ptr = 0;
    let info;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getCurves: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_curves_info_get'](state.handle, curvesId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_curves_info_get'](state.handle, curvesId, BigInt(p));
      }
      if (status !== 0) throw new RangeError('getCurves: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      info = {
        curveCount: view.getInt32(4, true),
        controlPointCount: view.getInt32(8, true),
        tessellatedPointCount: view.getInt32(12, true),
        materialId: view.getInt32(16, true),
        hasBounds: view.getInt32(20, true) !== 0,
        bboxMin: Array.from({length: 3}, (_, i) => view.getFloat32(24 + i * 4, true)),
        bboxMax: Array.from({length: 3}, (_, i) => view.getFloat32(36 + i * 4, true))
      };
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    const typeCode = this.curvesType(curvesId);
    const basisCode = this.curvesBasis(curvesId);
    const wrapCode = this.curvesWrap(curvesId);
    const out = {
      index: curvesId,
      name: curvesDecoder.decode(this.resourceNameBuffer(13, curvesId)),
      primPath: curvesDecoder.decode(this.resourcePathBuffer(13, curvesId)),
      curveCount: info.curveCount,
      controlPointCount: info.controlPointCount,
      tessellatedPointCount: info.tessellatedPointCount,
      type: typeCode === 0 ? 'linear' : 'cubic',
      typeCode,
      basis: curvesBasisNames[basisCode] || 'bezier',
      basisCode,
      wrap: curvesWrapNames[wrapCode] || 'nonperiodic',
      wrapCode,
      isNurbs: this.curvesIsNurbs(curvesId) !== 0,
      isHermite: this.curvesIsHermite(curvesId) !== 0,
      materialId: info.materialId,
      widthsInterpolation: curvesInterpolationNames[this.curvesWidthsInterpolation(curvesId)] || 'constant',
      colorsInterpolation: curvesInterpolationNames[this.curvesColorsInterpolation(curvesId)] || 'constant',
      opacitiesInterpolation: curvesInterpolationNames[this.curvesOpacitiesInterpolation(curvesId)] || 'constant',
      curveVertexCounts: Array.from(this.curvesVertexCountsBuffer(curvesId)),
      tessellatedVertexCounts: Array.from(this.curvesTessellatedVertexCountsBuffer(curvesId)),
      points: this.curvesControlPointsBuffer(curvesId),
      tessellatedPoints: this.curvesTessellatedPointsBuffer(curvesId),
      hasBounds: info.hasBounds
    };
    for (const [field, bufferName] of [
      ['widths', 'curvesWidthsBuffer'], ['colors', 'curvesColorsBuffer'],
      ['tessellatedWidths', 'curvesTessellatedWidthsBuffer'],
      ['tessellatedColors', 'curvesTessellatedColorsBuffer'],
      ['opacities', 'curvesOpacitiesBuffer'],
      ['tessellatedOpacities', 'curvesTessellatedOpacitiesBuffer']
    ]) {
      const values = this[bufferName](curvesId);
      if (values.length) out[field] = values;
    }
    if (info.hasBounds) {
      out.bboxMin = info.bboxMin;
      out.bboxMax = info.bboxMax;
    }
    return out;
  }});
  const instancerBuffers = [
    ['instancerCompactBuffer', 0, Uint8Array],
    ['instancerPositionsBuffer', 1, Float32Array],
    ['instancerOrientationsBuffer', 2, Float32Array],
    ['instancerScalesBuffer', 3, Float32Array],
    ['instancerPrototypeIndicesBuffer', 4, Int32Array],
    ['instancerVisibilityBuffer', 5, Uint8Array],
    ['instancerPrototypeNodeIdsBuffer', 6, Int32Array],
    ['instancerPrototypeMeshOffsetsBuffer', 7, Uint32Array],
    ['instancerPrototypeMeshIdsBuffer', 8, Int32Array],
    ['instancerPrototypeTransformsBuffer', 9, Float32Array]
  ];
  for (const [name, kind, ArrayType] of instancerBuffers) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(instancerId) {
      if (arguments.length !== 1 || typeof instancerId !== 'number') {
        throw new TypeError(name + ': expected one numeric instancer id');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_instancer_buffer',
        [instancerId, kind], name, ArrayType, 'invalid instancer id');
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'instancerPrototypePath', {value: function(instancerId, prototypeId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof instancerId !== 'number' || typeof prototypeId !== 'number') {
      throw new TypeError('instancerPrototypePath: expected numeric instancer and prototype ids');
    }
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module['_lightusd_next_render_instancer_string'](state.handle, instancerId, prototypeId, 0, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_instancer_string'](
            state.handle, instancerId, prototypeId, 0, typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError('instancerPrototypePath: invalid prototype');
      if (bytes === 0) return '';
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError('instancerPrototypePath: allocation failed');
      query(ptr, bytes);
      return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getPointInstancer', {value: function(instancerId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof instancerId !== 'number') {
      throw new TypeError('getPointInstancer: expected one numeric instancer id');
    }
    if (!Number.isInteger(instancerId) || instancerId < 0 || instancerId >= this.pointInstancerCount()) {
      return {error: 'invalid point instancer index'};
    }
    const size = 48;
    ++state.busy;
    let ptr = 0;
    let info;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getPointInstancer: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_instancer_info_get'](state.handle, instancerId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_instancer_info_get'](state.handle, instancerId, BigInt(p));
      }
      if (status !== 0) throw new RangeError('getPointInstancer: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      info = Array.from({length: 11}, (_, i) => view.getInt32(4 + i * 4, true));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    const out = {
      index: instancerId,
      name: new TextDecoder().decode(this.resourceNameBuffer(10, instancerId)),
      primPath: new TextDecoder().decode(this.resourcePathBuffer(10, instancerId)),
      prototypePaths: Array.from({length: info[2]}, (_, i) => this.instancerPrototypePath(instancerId, i)),
      prototypeNodeIds: Array.from(this.instancerPrototypeNodeIdsBuffer(instancerId)),
      prototypeMeshOffsets: Array.from(this.instancerPrototypeMeshOffsetsBuffer(instancerId)),
      prototypeMeshIds: Array.from(this.instancerPrototypeMeshIdsBuffer(instancerId)),
      drawStart: info[0], drawCount: info[1], protoCount: info[2],
      instanceCount: info[3], visibleInstanceCount: info[4],
      hasTransforms: !!info[5], hasOrientations: !!info[6],
      hasScales: !!info[7], hasVelocities: !!info[8],
      hasAngularVelocities: !!info[9], valid: !!info[10]
    };
    const query = (p, cap) => {
      try { return Module['_lightusd_next_render_instancer_string'](state.handle, instancerId, 0, 1, p, cap); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module['_lightusd_next_render_instancer_string'](
          state.handle, instancerId, 0, 1, typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
      }
    };
    ++state.busy;
    ptr = 0;
    try {
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError('getPointInstancer: validation error query failed');
      if (bytes > 0) {
        ptr = Module['_lightusd_next_alloc'](bytes);
        if (!ptr) throw new RangeError('getPointInstancer: allocation failed');
        query(ptr, bytes);
        out.validationError = new TextDecoder().decode(
          new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
      }
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    return out;
  }});
  for (const [name, symbol, fields] of [
    ['lightField', '_lightusd_next_render_light_field', 5],
    ['cameraField', '_lightusd_next_render_camera_field', 7],
    ['skeletonField', '_lightusd_next_render_skeleton_field', 3]
  ]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(id, field) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 2 || typeof id !== 'number' || typeof field !== 'number' || field < 0 || field >= fields) {
        throw new TypeError(name + ': expected numeric id and field');
      }
      ++state.busy;
      try { return Module[symbol](state.handle, id, field); }
      finally { --state.busy; }
    }});
  }
  for (const [name, symbol] of [
    ['lightTransformBuffer', '_lightusd_next_render_light_transform'],
    ['lightColorBuffer', '_lightusd_next_render_light_color'],
    ['cameraTransformBuffer', '_lightusd_next_render_camera_transform'],
    ['cameraOpticsBuffer', '_lightusd_next_render_camera_optics']
  ]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(id) {
      if (arguments.length !== 1 || typeof id !== 'number') {
        throw new TypeError(name + ': expected one numeric id');
      }
      return copyRenderBuffer(this, symbol, [id], name,
        Float32Array, 'invalid resource id');
    }});
  }
  const lightDecoder = new TextDecoder();
  const lightCopy = (state, lightId, symbol, args, ArrayType) => {
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module[symbol](state.handle, lightId, ...args, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module[symbol](state.handle, lightId, ...args,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError('getLight: resource copy failed');
      if (!Number.isSafeInteger(bytes) || bytes > 0x20000000) {
        throw new RangeError('getLight: string exceeds 512 MiB limit');
      }
      if (bytes === 0) return ArrayType === String ? '' : new ArrayType(0);
      const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
      if (!Number.isSafeInteger(remaining) || remaining < 0 || bytes > remaining) {
        throw new RangeError('getLight: string exceeds remaining memory limit');
      }
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError('getLight: allocation failed');
      if (query(ptr, bytes) !== bytes) throw new RangeError('getLight: resource copy failed');
      const copy = new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes).slice();
      return ArrayType === String ? lightDecoder.decode(copy) : new ArrayType(copy.buffer);
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
    }
  };
  const lightPayloadEstimate = (state, lightId) => {
    const size = 192;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getAllLights: info allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_light_info_get'](state.handle, lightId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_light_info_get'](state.handle, lightId, BigInt(p));
      }
      if (status !== 0) throw new RangeError('getAllLights: info query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      const flags = view.getUint32(8, true);
      const counts = Array.from({length: 5}, (_, i) => view.getInt32(20 + i * 4, true));
      if (counts.some(count => count < 0 || count > 65536)) {
        throw new RangeError('getAllLights: invalid or excessive link count');
      }
      let total = 512;
      const querySize = (symbol, args) => {
        try { return Module[symbol](state.handle, ...args, 0, 0); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module[symbol](state.handle, ...args, BigInt(0), 0);
        }
      };
      const chargeString = (kind, item) => {
        const bytes = querySize('_lightusd_next_render_light_string', [lightId, kind, item]);
        if (!Number.isSafeInteger(bytes) || bytes < 0) {
          throw new RangeError('getAllLights: string size query failed');
        }
        total += bytes * 2 + 64;
      };
      for (const [symbol, args] of [
        ['_lightusd_next_render_resource_name', [5, lightId]],
        ['_lightusd_next_render_resource_path', [5, lightId]]
      ]) {
        const bytes = querySize(symbol, args);
        if (!Number.isSafeInteger(bytes) || bytes < 0) {
          throw new RangeError('getAllLights: resource string size query failed');
        }
        total += bytes * 2 + 64;
      }
      chargeString(0, 0);
      for (let i = 0; i < counts[0]; ++i) chargeString(1, i);
      for (let i = 0; i < counts[1]; ++i) chargeString(2, i);
      for (let i = 0; i < counts[2]; ++i) chargeString(3, i);
      if ((flags & 64) !== 0) chargeString(4, 0);
      for (const kind of [0, 1]) {
        const bytes = querySize('_lightusd_next_render_light_mesh_ids', [lightId, kind]);
        if (!Number.isSafeInteger(bytes) || bytes < 0 || bytes % 4 !== 0) {
          throw new RangeError('getAllLights: mesh link size query failed');
        }
        total += bytes * 3;
      }
      return total;
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
    }
  };
  Object.defineProperty(Module.RenderStream.prototype, 'getLight', {value: function(lightId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof lightId !== 'number') {
      throw new TypeError('getLight: expected one numeric light id');
    }
    if (!Number.isInteger(lightId) || lightId < 0 || lightId >= this.lightCount()) {
      return {error: 'invalid light index'};
    }
    const size = 192;
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getLight: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_light_info_get'](state.handle, lightId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_light_info_get'](state.handle, lightId, BigInt(p));
      }
      if (status !== 0) throw new RangeError('getLight: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      const typeCode = view.getInt32(4, true);
      const flags = view.getUint32(8, true);
      const textureId = view.getInt32(12, true);
      const domeFormat = view.getInt32(16, true);
      const counts = Array.from({length: 5}, (_, i) => view.getInt32(20 + i * 4, true));
      if (counts.some(count => count < 0 || count > 65536)) {
        throw new RangeError('getLight: invalid or excessive link count');
      }
      let payloadEstimate = 512;
      const querySize = (symbol, args) => {
        try { return Module[symbol](state.handle, ...args, 0, 0); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module[symbol](state.handle, ...args, BigInt(0), 0);
        }
      };
      const chargeString = (kind, itemId) => {
        const bytes = querySize('_lightusd_next_render_light_string',
          [lightId, kind, itemId]);
        if (!Number.isSafeInteger(bytes) || bytes < 0) {
          throw new RangeError('getLight: string size query failed');
        }
        payloadEstimate += bytes * 2 + 64;
      };
      for (const [symbol, args] of [
        ['_lightusd_next_render_resource_name', [5, lightId]],
        ['_lightusd_next_render_resource_path', [5, lightId]]
      ]) {
        const bytes = querySize(symbol, args);
        if (!Number.isSafeInteger(bytes) || bytes < 0) {
          throw new RangeError('getLight: resource string size query failed');
        }
        payloadEstimate += bytes * 2 + 64;
      }
      chargeString(0, 0);
      for (let i = 0; i < counts[0]; ++i) chargeString(1, i);
      for (let i = 0; i < counts[1]; ++i) chargeString(2, i);
      for (let i = 0; i < counts[2]; ++i) chargeString(3, i);
      if ((flags & 64) !== 0) chargeString(4, 0);
      for (const kind of [0, 1]) {
        const bytes = querySize('_lightusd_next_render_light_mesh_ids',
          [lightId, kind]);
        if (!Number.isSafeInteger(bytes) || bytes < 0 || bytes % 4 !== 0) {
          throw new RangeError('getLight: mesh link size query failed');
        }
        payloadEstimate += bytes * 3;
      }
      if (payloadEstimate > 0x20000000) {
        throw new RangeError('getLight: returned payload exceeds 512 MiB limit');
      }
      const v = Array.from({length: 22}, (_, i) => view.getFloat32(40 + i * 4, true));
      const transform = Array.from({length: 16}, (_, i) => view.getFloat32(128 + i * 4, true));
      const targetList = (kind, count) => Array.from({length: count}, (_, i) =>
        lightCopy(state, lightId, '_lightusd_next_render_light_string', [kind, i], String));
      const out = {
        index: lightId,
        name: lightDecoder.decode(this.resourceNameBuffer(5, lightId)),
        primPath: lightDecoder.decode(this.resourcePathBuffer(5, lightId)),
        type: ['point', 'directional', 'spot', 'rect', 'disk', 'dome',
          'sphere', 'cylinder', 'geometry'][typeCode] || 'unknown',
        typeCode,
        intensity: v[0], exposure: v[1], normalize: !!(flags & 1),
        enableColorTemperature: !!(flags & 2), colorTemperature: v[2],
        diffuse: v[3], specular: v[4], shapingFocus: v[5],
        shapingFocusTint: v.slice(6, 9), shapingConeSoftness: v[9],
        shapingIesFile: lightCopy(state, lightId,
          '_lightusd_next_render_light_string', [0, 0], String),
        shapingIesAngleScale: v[10], shapingIesNormalize: !!(flags & 4),
        lightLinkTargets: targetList(1, counts[0]),
        shadowLinkTargets: targetList(2, counts[1]),
        filterTargets: targetList(3, counts[2]),
        lightLinksAll: !!(flags & 8),
        lightLinkMeshIndices: Array.from(lightCopy(state, lightId,
          '_lightusd_next_render_light_mesh_ids', [0], Int32Array)),
        shadowLinksAll: !!(flags & 16),
        shadowLinkMeshIndices: Array.from(lightCopy(state, lightId,
          '_lightusd_next_render_light_mesh_ids', [1], Int32Array)),
        enableShadow: !!(flags & 32), color: v.slice(14, 17), transform,
        shadowColor: v.slice(17, 20), shadowDistance: v[11],
        shadowFalloff: v[12], shadowFalloffGamma: v[13]
      };
      if (typeCode === 6 || typeCode === 4) out.radius = v[20];
      else if (typeCode === 3) { out.width = v[20]; out.height = v[21]; }
      else if (typeCode === 2 || typeCode === 1) out.angle = v[20];
      else if (typeCode === 7) { out.radius = v[20]; out.length = v[21]; }
      else if (typeCode === 5) {
        out.textureId = textureId;
        out.envmapTextureId = -1;
        if (flags & 64) out.textureFile = lightCopy(state, lightId,
          '_lightusd_next_render_light_string', [4, 0], String);
        out.domeTextureFormat = ['automatic', 'latlong', 'mirroredBall',
          'angular'][domeFormat] || 'automatic';
      }
      return out;
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getAllLights', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getAllLights: wrong argument count');
    const count = this.lightCount();
    if (!Number.isSafeInteger(count) || count < 0 || count > 65536) {
      throw new RangeError('getAllLights: invalid or excessive light count');
    }
    let aggregateBytes = 0;
    for (let index = 0; index < count; ++index) {
      aggregateBytes += lightPayloadEstimate(state, index);
      if (aggregateBytes > 0x20000000) {
        throw new RangeError('getAllLights: aggregate returned data exceeds 512 MiB limit');
      }
    }
    preflightRenderAggregate(state, aggregateBytes, 'getAllLights');
    return Array.from({length: count}, (_, index) => this.getLight(index));
  }});
  const cameraDecoder = new TextDecoder();
  Object.defineProperty(Module.RenderStream.prototype, 'getCamera', {value: function(cameraId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof cameraId !== 'number') {
      throw new TypeError('getCamera: expected one numeric camera id');
    }
    if (!Number.isInteger(cameraId) || cameraId < 0 || cameraId >= this.cameraCount()) {
      return {error: 'invalid camera index'};
    }
    const size = 88;
    ++state.busy;
    let ptr = 0;
    let info;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getCamera: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_camera_info_get'](state.handle, cameraId, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_camera_info_get'](state.handle, cameraId, BigInt(p));
      }
      if (status !== 0) throw new RangeError('getCamera: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      info = {
        typeCode: view.getInt32(4, true),
        optics: Array.from({length: 14}, (_, i) => view.getFloat32(8 + i * 4, true)),
        stereoRole: view.getInt32(64, true),
        shutterOpen: view.getFloat64(72, true),
        shutterClose: view.getFloat64(80, true)
      };
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    const v = info.optics;
    return {
      index: cameraId,
      name: cameraDecoder.decode(this.resourceNameBuffer(6, cameraId)),
      primPath: cameraDecoder.decode(this.resourcePathBuffer(6, cameraId)),
      type: ['perspective', 'orthographic'][info.typeCode] || 'unknown',
      typeCode: info.typeCode,
      transform: Array.from(this.cameraTransformBuffer(cameraId)),
      focalLength: v[0],
      horizontalAperture: v[1],
      verticalAperture: v[2],
      orthoWidth: v[3],
      nearClip: v[4],
      farClip: v[5],
      focusDistance: v[6],
      fStop: v[7],
      fovX: v[8],
      fovY: v[9],
      aspect: v[10],
      horizontalApertureOffset: v[11],
      verticalApertureOffset: v[12],
      exposure: v[13],
      stereoRole: info.stereoRole,
      shutterOpen: info.shutterOpen,
      shutterClose: info.shutterClose
    };
  }});
  const getImageRecord = (stream, imageId, copyPixels, methodName, argumentCount) => {
    const state = live.get(stream);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (argumentCount !== 1 || typeof imageId !== 'number' || !Number.isInteger(imageId)) {
      throw new TypeError(methodName + ': expected one integer image id');
    }
    if (imageId < 0 || imageId >= stream.numImages()) return {};
    ++state.busy;
    try {
      const field = index => Module['_lightusd_next_render_image_field'](
        state.handle, imageId, index);
      const result = {
        width: field(0), height: field(1), channels: field(2),
        mipLevels: field(3), decoded: field(4) !== 0,
        componentType: field(5), colorSpace: field(6),
        name: nodeDecoder.decode(stream.resourceNameBuffer(4, imageId)),
        uri: nodeDecoder.decode(stream.resourcePathBuffer(4, imageId))
      };
      if (result.decoded) {
        const byteLength = field(7);
        if (byteLength < 0) throw new RangeError(methodName + ': image exceeds 512 MiB limit');
        result.byteLength = byteLength;
        if (copyPixels) {
          const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
          if (!Number.isSafeInteger(remaining) || remaining < 0 || byteLength > remaining) {
            throw new RangeError(methodName + ': image payload exceeds remaining memory limit');
          }
        }
        const ptr = Module['_lightusd_next_render_image_data'](state.handle, imageId);
        if (!copyPixels && !ptr) {
          throw new RangeError(methodName + ': image has no stable borrowed buffer');
        }
        if (ptr) {
          const address = Number(ptr);
          if (!Number.isSafeInteger(address) || address < 0 ||
              address > Module.HEAPU8.byteLength ||
              byteLength > Module.HEAPU8.byteLength - address) {
            throw new RangeError(methodName + ': invalid image buffer span');
          }
          const view = Module.HEAPU8.subarray(address, address + byteLength);
          result.data = copyPixels ? view.slice() : view;
        } else {
          result.data = copyRenderBuffer(stream,
            '_lightusd_next_render_image_buffer', [imageId],
            methodName, Uint8Array, 'image buffer unavailable');
        }
      }
      return result;
    } finally {
      --state.busy;
    }
  };
  Object.defineProperty(Module.RenderStream.prototype, 'getImageCopy', {value: function(imageId) {
    return getImageRecord(this, imageId, true, 'getImageCopy', arguments.length);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getImageView', {value: function(imageId) {
    return getImageRecord(this, imageId, false, 'getImageView', arguments.length);
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getAllImages', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getAllImages: wrong argument count');
    const count = this.numImages();
    if (!Number.isInteger(count) || count < 0 || count > 262144) {
      throw new RangeError('getAllImages: invalid or excessive image count');
    }
    const stringBytes = (symbol, imageId) => {
      try { return Module[symbol](state.handle, 4, imageId, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, 4, imageId, 0n, 0);
      }
    };
    let estimatedBytes = count * 256;
    for (let imageId = 0; imageId < count; ++imageId) {
      const nameBytes = stringBytes('_lightusd_next_render_resource_name', imageId);
      const pathBytes = stringBytes('_lightusd_next_render_resource_path', imageId);
      if (!Number.isSafeInteger(nameBytes) || nameBytes < 0 ||
          !Number.isSafeInteger(pathBytes) || pathBytes < 0) {
        throw new RangeError('getAllImages: invalid image metadata size');
      }
      const stringCharge = (nameBytes + pathBytes) * 2 + 128;
      if (!Number.isSafeInteger(stringCharge) || stringCharge < 0 ||
          estimatedBytes > 0x20000000 - stringCharge) {
        throw new RangeError('getAllImages: aggregate image metadata exceeds 512 MiB limit');
      }
      estimatedBytes += stringCharge;
      if (Module['_lightusd_next_render_image_field'](state.handle, imageId, 4) !== 0) {
        const bytes = Module['_lightusd_next_render_image_field'](state.handle, imageId, 7);
        if (!Number.isSafeInteger(bytes) || bytes < 0 || bytes > 0x20000000 ||
            estimatedBytes > 0x20000000 - bytes) {
          throw new RangeError('getAllImages: aggregate image data exceeds 512 MiB limit');
        }
        estimatedBytes += bytes;
      }
    }
    const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
    if (!Number.isSafeInteger(remaining) || remaining < 0 || estimatedBytes > remaining) {
      throw new RangeError('getAllImages: aggregate exceeds remaining memory limit');
    }
    const result = new Array(count);
    for (let imageId = 0; imageId < count; ++imageId) {
      result[imageId] = getImageRecord(this, imageId, true, 'getAllImages', 1);
    }
    return result;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'extractUnresolvedTexturePaths', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('extractUnresolvedTexturePaths: wrong argument count');
    const count = this.numImages();
    if (!Number.isInteger(count) || count < 0 || count > 262144) {
      throw new RangeError('extractUnresolvedTexturePaths: invalid or excessive image count');
    }
    const unresolved = [];
    let total = count * 16;
    for (let imageId = 0; imageId < count; ++imageId) {
      const loaded = Module['_lightusd_next_render_image_field'](state.handle, imageId, 4);
      if (loaded < 0) throw new RangeError('extractUnresolvedTexturePaths: invalid image record');
      if (loaded !== 0) continue;
      let bytes;
      try { bytes = Module['_lightusd_next_render_image_asset_identifier'](state.handle, imageId, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        bytes = Module['_lightusd_next_render_image_asset_identifier'](state.handle, imageId, 0n, 0);
      }
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('extractUnresolvedTexturePaths: invalid image path size');
      }
      total += bytes;
      if (!Number.isSafeInteger(total) || total > 0x20000000) {
        throw new RangeError('extractUnresolvedTexturePaths: aggregate returned data exceeds 512 MiB limit');
      }
      unresolved.push([imageId, bytes]);
    }
    return unresolved.map(([imageId, bytes]) => {
      const path = copyRenderBuffer(this, '_lightusd_next_render_image_asset_identifier',
        [imageId], 'extractUnresolvedTexturePaths', Uint8Array, 'invalid image path');
      if (path.byteLength !== bytes) throw new RangeError('extractUnresolvedTexturePaths: image path changed during copy');
      return nodeDecoder.decode(path);
    });
  }});
  const sceneStrings = [
    ['sceneName', 0], ['sceneDefaultPrim', 1],
    ['sceneRenderSettingsPath', 2], ['sceneWorkingColorSpace', 3],
    ['sceneUpAxisName', 4], ['sceneComment', 5], ['sceneCopyright', 6]
  ];
  for (const [name, kind] of sceneStrings) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function() {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      ++state.busy;
      let ptr = 0;
      try {
        const query = (p, cap) => {
          try { return Module['_lightusd_next_render_scene_string'](state.handle, kind, p, cap); }
          catch (error) {
            if (!(error instanceof TypeError)) throw error;
            return Module['_lightusd_next_render_scene_string'](
              state.handle, kind, typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
          }
        };
        const bytes = query(0, 0);
        if (bytes < 0) throw new RangeError(name + ': query failed');
        if (bytes === 0) return '';
        ptr = Module['_lightusd_next_alloc'](bytes + 1);
        if (!ptr) throw new RangeError(name + ': allocation failed');
        query(ptr, bytes + 1);
        return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
      } finally {
        if (ptr) Module['_lightusd_next_free'](ptr);
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'getSceneMetadata', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getSceneMetadata: wrong argument count');
    const size = 96;
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getSceneMetadata: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      let status;
      try { status = Module['_lightusd_next_render_scene_metadata_get'](state.handle, ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_next_render_scene_metadata_get'](state.handle, BigInt(p));
      }
      if (status === 1) return {};
      if (status !== 0) throw new RangeError('getSceneMetadata: query failed');
      const view = new DataView(Module.HEAPU8.buffer, p, size);
      const authored = view.getUint32(4, true);
      const numbers = Array.from({length: 6}, (_, i) => view.getFloat64(8 + i * 8, true));
      const matrix = Array.from({length: 9}, (_, i) => view.getFloat32(56 + i * 4, true));
      return {
        upAxis: this.sceneUpAxisName(),
        copyright: this.sceneCopyright(),
        comment: this.sceneComment(),
        metersPerUnit: numbers[0],
        kilogramsPerUnit: numbers[1],
        framesPerSecond: numbers[2],
        timeCodesPerSecond: numbers[3],
        startTimeCode: authored & 1 ? numbers[4] : null,
        endTimeCode: authored & 2 ? numbers[5] : null,
        autoPlay: (authored & 4) ? !!(authored & 8) : true,
        renderSettingsPrimPath: this.sceneRenderSettingsPath(),
        workingColorSpace: this.sceneWorkingColorSpace(),
        workingToDisplayLinear: matrix
      };
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  const unsupportedStrings = [
    ['unsupportedRenderablePath', 0], ['unsupportedRenderableTypeName', 1],
    ['unsupportedRenderableReason', 2]
  ];
  for (const [name, kind] of unsupportedStrings) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(unsupportedId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 1 || typeof unsupportedId !== 'number') {
        throw new TypeError(name + ': expected one numeric unsupported id');
      }
      ++state.busy;
      let ptr = 0;
      try {
        const query = (p, cap) => {
          try {
            return Module['_lightusd_next_render_unsupported_string'](
              state.handle, unsupportedId, kind, p, cap);
          } catch (error) {
            if (!(error instanceof TypeError)) throw error;
            return Module['_lightusd_next_render_unsupported_string'](
              state.handle, unsupportedId, kind,
              typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
          }
        };
        const bytes = query(0, 0);
        if (bytes < 0) throw new RangeError(name + ': invalid unsupported id');
        if (bytes === 0) return '';
        ptr = Module['_lightusd_next_alloc'](bytes + 1);
        if (!ptr) throw new RangeError(name + ': allocation failed');
        query(ptr, bytes + 1);
        return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
      } finally {
        if (ptr) Module['_lightusd_next_free'](ptr);
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'getUnsupportedRenderables', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getUnsupportedRenderables: wrong argument count');
    const count = this.unsupportedRenderableCount();
    const out = [];
    for (let i = 0; i < count; ++i) {
      out.push({
        index: i,
        primPath: this.unsupportedRenderablePath(i),
        type: this.unsupportedRenderableTypeName(i),
        reason: this.unsupportedRenderableReason(i)
      });
    }
    return out;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'pointInstanceDrawField', {value: function(drawId, field) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof drawId !== 'number' || typeof field !== 'number' || field < 0 || field >= 6) {
      throw new TypeError('pointInstanceDrawField: expected numeric draw id and field');
    }
    ++state.busy;
    try { return Module['_lightusd_next_render_point_instance_draw_field'](state.handle, drawId, field); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'pointInstanceDrawTransformBuffer', {value: function(drawId) {
    if (arguments.length !== 1 || typeof drawId !== 'number') {
      throw new TypeError('pointInstanceDrawTransformBuffer: expected one numeric draw id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_point_instance_draw_transform',
      [drawId], 'pointInstanceDrawTransformBuffer', Float32Array, 'invalid draw id');
  }});
  const drawDecoder = new TextDecoder();
  Object.defineProperty(Module.RenderStream.prototype, 'getPointInstanceDraw', {value: function(drawId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof drawId !== 'number') {
      throw new TypeError('getPointInstanceDraw: expected one numeric draw id');
    }
    if (!Number.isInteger(drawId) || drawId < 0 || drawId >= this.pointInstanceDrawCount()) {
      return {error: 'invalid point instance draw index'};
    }
    const pointInstancerId = this.pointInstanceDrawField(drawId, 0);
    const instanceIndex = this.pointInstanceDrawField(drawId, 1);
    const prototypeIndex = this.pointInstanceDrawField(drawId, 2);
    const meshId = this.pointInstanceDrawField(drawId, 3);
    const materialId = this.pointInstanceDrawField(drawId, 4);
    const expandedMeshId = this.pointInstanceDrawField(drawId, 5);
    const out = {
      index: drawId,
      pointInstancerId, instanceIndex, prototypeIndex,
      meshId, materialId, expandedMeshId,
      transform: Array.from(this.pointInstanceDrawTransformBuffer(drawId))
    };
    if (meshId >= 0 && meshId < this.meshCount()) {
      out.meshPath = drawDecoder.decode(this.resourcePathBuffer(1, meshId));
    }
    if (materialId >= 0 && this.materialShaderType(materialId) >= 0) {
      out.materialPath = drawDecoder.decode(this.resourcePathBuffer(2, materialId));
    }
    return out;
  }});
  // Legacy instance queries describe flattened point-instancer mesh draws.
  // Next already stores those draws as bounded records, so expose that same
  // record order through the legacy names.
  Object.defineProperty(Module.RenderStream.prototype, 'numInstances', {value: function() {
    if (arguments.length !== 0) throw new TypeError('numInstances: wrong argument count');
    const count = this.pointInstanceDrawCount();
    if (count < 0 || count > 1_000_000) throw new RangeError('numInstances: count exceeds limit');
    return count;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getInstancesForMesh', {value: function(meshId) {
    if (arguments.length !== 1 || typeof meshId !== 'number') {
      throw new TypeError('getInstancesForMesh: expected one numeric mesh id');
    }
    meshId = Math.trunc(meshId);
    const count = this.pointInstanceDrawCount();
    if (count < 0 || count > 1_000_000) throw new RangeError('getInstancesForMesh: count exceeds limit');
    const matches = [];
    for (let i = 0; i < count; ++i) {
      if (this.pointInstanceDrawField(i, 3) === meshId) matches.push(i);
    }
    return matches;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getInstance', {value: function(instanceId) {
    if (arguments.length !== 1 || typeof instanceId !== 'number') {
      throw new TypeError('getInstance: expected one numeric instance id');
    }
    const count = this.numInstances();
    instanceId = Math.trunc(instanceId);
    if (!Number.isFinite(instanceId) || instanceId < 0 || instanceId >= count) return null;
    const draw = this.getPointInstanceDraw(instanceId);
    const instancer = this.getPointInstancer(draw.pointInstancerId);
    if (instancer.error) throw new RangeError('getInstance: invalid point instancer');
    const index = draw.instanceIndex;
    const path = instancer.primPath;
    const localMatrix = draw.transform.slice();
    let globalMatrix = localMatrix.slice();
    // Next stores the point-instance draw transform in instancer-local space;
    // legacy reports both that matrix and the world-space product.
    for (let nodeId = 0; nodeId < this.nodeCount(); ++nodeId) {
      const node = this.getNode(nodeId);
      if (node.primPath !== path) continue;
      const parent = node.worldMatrix;
      const out=Array(16).fill(0);
      for(let col=0;col<4;++col) for(let row=0;row<4;++row)
        for(let k=0;k<4;++k) out[col*4+row]+=parent[k*4+row]*localMatrix[col*4+k];
      globalMatrix=out;
      break;
    }
    const visibility = this.instancerVisibilityBuffer(draw.pointInstancerId);
    return {
      primName: `${instancer.name}[${index}]`, absPath: `${path}/instance_${index}`,
      displayName: '', prototypeIndex: draw.prototypeIndex, meshId: draw.meshId,
      materialId: draw.materialId, localMatrix,
      globalMatrix,
      visible: visibility.length === 0 || visibility[index] !== 0
    };
  }});
  const skeletonJointBuffers = [
    ['skeletonBindMatricesBuffer', 0, Float64Array],
    ['skeletonRestMatricesBuffer', 1, Float64Array],
    ['skeletonJointParentsBuffer', 2, Int32Array]
  ];
  for (const [name, kind, ArrayType] of skeletonJointBuffers) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(skeletonId) {
      if (arguments.length !== 1 || typeof skeletonId !== 'number') {
        throw new TypeError(name + ': expected one numeric skeleton id');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_skeleton_joint_buffer',
        [skeletonId, kind], name, ArrayType, 'invalid skeleton id');
    }});
  }
  for (const [name, kind] of [
    ['skeletonJointName', 0], ['skeletonJointPath', 1],
    ['skeletonAnimationSourcePath', 2]
  ]) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(skeletonId, jointId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (kind === 2 ? arguments.length !== 1 || typeof skeletonId !== 'number'
                     : arguments.length !== 2 || typeof skeletonId !== 'number' || typeof jointId !== 'number') {
        throw new TypeError(name + (kind === 2 ? ': expected one numeric skeleton id' : ': expected two numeric ids'));
      }
      if (kind === 2) jointId = 0;
      ++state.busy;
      let ptr = 0;
      try {
        const query = (p, cap) => {
          try {
            return Module['_lightusd_next_render_skeleton_joint_string'](
              state.handle, skeletonId, jointId, kind, p, cap);
          } catch (error) {
            if (!(error instanceof TypeError)) throw error;
            return Module['_lightusd_next_render_skeleton_joint_string'](
              state.handle, skeletonId, jointId, kind,
              typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
          }
        };
        const bytes = query(0, 0);
        if (bytes < 0) throw new RangeError(name + ': invalid skeleton or joint id');
        if (bytes === 0) return '';
        ptr = Module['_lightusd_next_alloc'](bytes);
        if (!ptr) throw new RangeError(name + ': allocation failed');
        query(ptr, bytes);
        return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
      } finally {
        if (ptr) Module['_lightusd_next_free'](ptr);
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'skeletonJointChildrenBuffer', {value: function(skeletonId, jointId) {
    if (arguments.length !== 2 || typeof skeletonId !== 'number' || typeof jointId !== 'number') {
      throw new TypeError('skeletonJointChildrenBuffer: expected two numeric ids');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_skeleton_joint_children',
      [skeletonId, jointId], 'skeletonJointChildrenBuffer', Int32Array,
      'invalid skeleton or joint id');
  }});
  const skeletonDecoder = new TextDecoder();
  const kMaxSkeletonAggregateBytes = 0x20000000;
  const skeletonPayloadEstimate = (stream, skeletonId, includeFlatMatrices = false) => {
    const state = live.get(stream);
    const jointCount = stream.skeletonField(skeletonId, 0);
    if (!Number.isSafeInteger(jointCount) || jointCount < 0 || jointCount > 65536) {
      throw new RangeError('skeleton payload: invalid or excessive joint count');
    }
    let total = jointCount * 512;
    const query = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    const chargeString = bytes => {
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('skeleton payload: string size query failed');
      }
      total += bytes * 2 + 64;
      if (total > kMaxSkeletonAggregateBytes) {
        throw new RangeError('skeleton payload: aggregate returned data exceeds 512 MiB limit');
      }
    };
    const matrixWeight = includeFlatMatrices ? 5 : 3;
    for (const [kind, weight] of [[0, matrixWeight], [1, matrixWeight], [2, 3]]) {
      const bytes = query('_lightusd_next_render_skeleton_joint_buffer',
        [skeletonId, kind]);
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('skeleton payload: joint buffer size query failed');
      }
      total += bytes * weight;
      if (total > kMaxSkeletonAggregateBytes) {
        throw new RangeError('skeleton payload: aggregate returned data exceeds 512 MiB limit');
      }
    }
    for (let jointId = 0; jointId < jointCount; ++jointId) {
      chargeString(query('_lightusd_next_render_skeleton_joint_string',
        [skeletonId, jointId, 0]));
      chargeString(query('_lightusd_next_render_skeleton_joint_string',
        [skeletonId, jointId, 1]));
      const childBytes = query('_lightusd_next_render_skeleton_joint_children',
        [skeletonId, jointId]);
      if (!Number.isSafeInteger(childBytes) || childBytes < 0 || childBytes % 4 !== 0) {
        throw new RangeError('skeleton payload: child buffer size query failed');
      }
      total += childBytes * 3;
      if (total > kMaxSkeletonAggregateBytes) {
        throw new RangeError('skeleton payload: aggregate returned data exceeds 512 MiB limit');
      }
    }
    chargeString(query('_lightusd_next_render_skeleton_joint_string',
      [skeletonId, 0, 2]));
    // getSkeleton returns each root name/path twice under compatibility aliases.
    chargeString(query('_lightusd_next_render_resource_name', [7, skeletonId]));
    chargeString(query('_lightusd_next_render_resource_name', [7, skeletonId]));
    chargeString(query('_lightusd_next_render_resource_path', [7, skeletonId]));
    chargeString(query('_lightusd_next_render_resource_path', [7, skeletonId]));
    chargeString(query('_lightusd_next_render_resource_name', [15, skeletonId]));
    return total;
  };
  Object.defineProperty(Module.RenderStream.prototype, 'getSkeleton', {value: function(skeletonId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof skeletonId !== 'number') {
      throw new TypeError('getSkeleton: expected one numeric skeleton id');
    }
    if (!Number.isInteger(skeletonId) || skeletonId < 0 || skeletonId >= this.skeletonCount()) {
      return {error: 'invalid skeleton index'};
    }
    skeletonPayloadEstimate(this, skeletonId);
    const jointCount = this.skeletonField(skeletonId, 0);
    const parents = this.skeletonJointParentsBuffer(skeletonId);
    const bind = this.skeletonBindMatricesBuffer(skeletonId);
    const rest = this.skeletonRestMatricesBuffer(skeletonId);
    const joints = Array.from({length: jointCount}, (_, i) => ({
      index: i,
      name: this.skeletonJointName(skeletonId, i),
      path: this.skeletonJointPath(skeletonId, i),
      parentId: parents[i],
      bindMatrix: Array.from(bind.subarray(i * 16, (i + 1) * 16)),
      restMatrix: Array.from(rest.subarray(i * 16, (i + 1) * 16)),
      children: Array.from(this.skeletonJointChildrenBuffer(skeletonId, i))
    }));
    const rootJoint = this.skeletonField(skeletonId, 1);
    let rootNode;
    if (rootJoint >= 0) {
      const legacyJoints = joints.map(joint => ({
        joint_path: joint.path, joint_name: joint.name, joint_id: joint.index,
        bind_transform: joint.bindMatrix, rest_transform: joint.restMatrix,
        children: []
      }));
      if (rootJoint >= joints.length) {
        throw new RangeError('getSkeleton: invalid root joint');
      }
      for (let i = 0; i < joints.length; ++i) {
        const parentId = joints[i].parentId;
        if (parentId >= 0) {
          if (parentId >= joints.length || parentId === i) {
            throw new RangeError('getSkeleton: invalid joint hierarchy');
          }
          legacyJoints[parentId].children.push(legacyJoints[i]);
        }
      }
      rootNode = legacyJoints[rootJoint];
      const visited = new Set();
      const stack = [rootJoint];
      while (stack.length) {
        const index = stack.pop();
        if (visited.has(index)) throw new RangeError('getSkeleton: cyclic joint hierarchy');
        visited.add(index);
        for (const childId of joints[index].children) stack.push(childId);
      }
    }
    return {
      index: skeletonId,
      name: skeletonDecoder.decode(this.resourceNameBuffer(7, skeletonId)),
      primPath: skeletonDecoder.decode(this.resourcePathBuffer(7, skeletonId)),
      rootJoint,
      jointCount,
      animationId: this.skeletonField(skeletonId, 2),
      animationSourcePath: this.skeletonAnimationSourcePath(skeletonId),
      joints,
      id: skeletonId,
      prim_name: skeletonDecoder.decode(this.resourceNameBuffer(7, skeletonId)),
      abs_path: skeletonDecoder.decode(this.resourcePathBuffer(7, skeletonId)),
      display_name: skeletonDecoder.decode(this.resourceNameBuffer(15, skeletonId)),
      anim_id: this.skeletonField(skeletonId, 2),
      root_node: rootNode
    };
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getSkeletonJointsFlat', {value: function(skeletonId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof skeletonId !== 'number') {
      throw new TypeError('getSkeletonJointsFlat: expected one numeric skeleton id');
    }
    if (Number.isInteger(skeletonId) && skeletonId >= 0 && skeletonId < this.skeletonCount()) {
      skeletonPayloadEstimate(this, skeletonId, true);
    }
    const skeleton = this.getSkeleton(skeletonId);
    if (skeleton.error) return skeleton;
    const jointCount = skeleton.jointCount;
    const jointNames = new Array(jointCount);
    const jointPaths = new Array(jointCount);
    const jointIds = new Array(jointCount);
    const parentIndices = new Array(jointCount);
    const bindMatrices = new Array(jointCount * 16);
    const restMatrices = new Array(jointCount * 16);
    for (let i = 0; i < jointCount; ++i) {
      const joint = skeleton.joints[i];
      jointNames[i] = joint.name;
      jointPaths[i] = joint.path;
      jointIds[i] = joint.index;
      parentIndices[i] = joint.parentId;
      const offset = i * 16;
      for (let e = 0; e < 16; ++e) {
        bindMatrices[offset + e] = joint.bindMatrix[e];
        restMatrices[offset + e] = joint.restMatrix[e];
      }
    }
    return {
      joint_names: jointNames,
      joint_paths: jointPaths,
      joint_ids: jointIds,
      parent_indices: parentIndices,
      bind_matrices: bindMatrices,
      rest_matrices: restMatrices,
      num_joints: jointCount
    };
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getAllSkeletons', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getAllSkeletons: wrong argument count');
    const count = this.skeletonCount();
    if (count < 0) throw new RangeError('getAllSkeletons: invalid count');
    if (count > 65536) throw new RangeError('getAllSkeletons: excessive skeleton count');
    let aggregateBytes = 0;
    for (let skeletonId = 0; skeletonId < count; ++skeletonId) {
      aggregateBytes += skeletonPayloadEstimate(this, skeletonId);
      if (aggregateBytes > kMaxSkeletonAggregateBytes) {
        throw new RangeError('getAllSkeletons: aggregate returned data exceeds 512 MiB limit');
      }
    }
    preflightRenderAggregate(state, aggregateBytes, 'getAllSkeletons');
    return Array.from({length: count}, (_, index) => this.getSkeleton(index));
  }});
  const renderMeshFields = [
    ['meshVertexCount', 0], ['meshFaceCount', 1],
    ['meshMaterialId', 2], ['meshHasNormals', 3], ['meshHasUVs', 4],
    ['meshHasTangents', 5], ['meshHasSecondaryUVs', 6],
    ['meshHasColors', 7], ['meshHasSkin', 8], ['meshHasBounds', 9],
    ['meshSkeletonId', 10]
  ];
  const renderMeshPurposes = ['default', 'render', 'proxy', 'guide'];
  Object.defineProperty(Module.RenderStream.prototype, 'meshPurpose', {value: function(meshId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof meshId !== 'number') {
      throw new TypeError('meshPurpose: expected one numeric mesh id');
    }
    ++state.busy;
    try {
      // Computed UsdGeomImageable purpose; '' for an invalid mesh id.
      return renderMeshPurposes[
        Module['_lightusd_next_render_mesh_field'](state.handle, meshId, 11)] ?? '';
    } finally {
      --state.busy;
    }
  }});
  for (const [name, field] of renderMeshFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(meshId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) {
        throw new TypeError('Invalid LightUSD receiver');
      }
      if (arguments.length !== 1 || typeof meshId !== 'number') {
        throw new TypeError(name + ': expected one numeric mesh id');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_mesh_field'](state.handle, meshId, field);
      } finally {
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'meshPrimvarCount', {value: function(meshId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof meshId !== 'number') {
      throw new TypeError('meshPrimvarCount: expected one numeric mesh id');
    }
    ++state.busy;
    try { return Module['_lightusd_next_render_mesh_primvar_count'](state.handle, meshId); }
    finally { --state.busy; }
  }});
  const meshPrimvarFields = [
    ['meshPrimvarFormat', 0], ['meshPrimvarInterpolation', 1],
    ['meshPrimvarHasIndices', 2], ['meshPrimvarElementCount', 3],
    ['meshPrimvarElementSize', 4]
  ];
  for (const [name, field] of meshPrimvarFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(meshId, primvarId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 2 || typeof meshId !== 'number' || typeof primvarId !== 'number') {
        throw new TypeError(name + ': expected two numeric ids');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_mesh_primvar_field'](
          state.handle, meshId, primvarId, field);
      } finally { --state.busy; }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'meshPrimvarName', {value: function(meshId, primvarId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof meshId !== 'number' || typeof primvarId !== 'number') {
      throw new TypeError('meshPrimvarName: expected two numeric ids');
    }
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try {
          return Module['_lightusd_next_render_mesh_primvar_name'](
            state.handle, meshId, primvarId, p, cap);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_mesh_primvar_name'](
            state.handle, meshId, primvarId,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError('meshPrimvarName: invalid mesh or primvar id');
      if (bytes === 0) return '';
      if (!Number.isSafeInteger(bytes) || bytes > 0x20000000) {
        throw new RangeError('meshPrimvarName: name exceeds 512 MiB limit');
      }
      const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
      if (!Number.isSafeInteger(remaining) || remaining < 0 || bytes > remaining) {
        throw new RangeError('meshPrimvarName: name exceeds remaining memory limit');
      }
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError('meshPrimvarName: allocation failed');
      query(ptr, bytes);
      return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  const meshPrimvarBuffers = [
    ['meshPrimvarBuffer', 0], ['meshPrimvarIndicesBuffer', 1]
  ];
  for (const [name, kind] of meshPrimvarBuffers) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(meshId, primvarId) {
      if (arguments.length !== 2 || typeof meshId !== 'number' || typeof primvarId !== 'number') {
        throw new TypeError(name + ': expected two numeric ids');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_mesh_primvar_buffer',
        [meshId, primvarId, kind], name, Uint8Array, 'invalid mesh or primvar id');
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'getMeshPrimvarsJSON', {value: function(meshId) {
    if (arguments.length !== 1 || typeof meshId !== 'number' || !Number.isInteger(meshId)) {
      throw new TypeError('getMeshPrimvarsJSON: expected one integer mesh id');
    }
    const result = {version: 1, primvars: {}};
    const count = this.meshPrimvarCount(meshId);
    if (count < 0) return JSON.stringify({...result, error: 'invalid mesh id'});
    if (count > 256) return JSON.stringify({...result, error: 'too many mesh primvars'});
    const formats = [
      ['float', 1, 'float'], ['float2', 2, 'float'], ['float3', 3, 'float'],
      ['float4', 4, 'float'], ['int', 1, 'int'], ['int2', 2, 'int'],
      ['int3', 3, 'int'], ['int4', 4, 'int'], ['uint', 1, 'uint'],
      ['uint2', 2, 'uint'], ['uint3', 3, 'uint'], ['uint4', 4, 'uint'],
      ['matrix3', 9, 'float'], ['matrix4', 16, 'float']
    ];
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    const querySize = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, 0n, 0);
      }
    };
    let aggregateBytes = count * 256;
    const pathBytes = querySize('_lightusd_next_render_mesh_view_string', [meshId, 1]);
    if (!Number.isSafeInteger(pathBytes) || pathBytes < 0 ||
        aggregateBytes > 0x20000000 - pathBytes * 2 - 64) {
      throw new RangeError('getMeshPrimvarsJSON: mesh path exceeds aggregate JSON limit');
    }
    aggregateBytes += pathBytes * 2 + 64;
    const descriptors = [];
    for (let id = 0; id < count; ++id) {
      const nameBytes = querySize('_lightusd_next_render_mesh_primvar_name', [meshId, id]);
      if (!Number.isSafeInteger(nameBytes) || nameBytes < 0 ||
          aggregateBytes > 0x20000000 - nameBytes * 2 - 64) {
        throw new RangeError('getMeshPrimvarsJSON: aggregate primvar names exceed 512 MiB');
      }
      aggregateBytes += nameBytes * 2 + 64;
      if (nameBytes === 0) continue;
      const format = this.meshPrimvarFormat(meshId, id);
      const info = formats[format];
      if (!info) {
        descriptors.push({id, nameBytes, unsupported: true});
        continue;
      }
      const [baseType, components, scalarType] = info;
      const rawBytes = querySize('_lightusd_next_render_mesh_primvar_buffer',
        [meshId, id, 0]);
      const hasIndices = this.meshPrimvarHasIndices(meshId, id) !== 0;
      const indexBytes = hasIndices
        ? querySize('_lightusd_next_render_mesh_primvar_buffer', [meshId, id, 1]) : 0;
      const elementSize = this.meshPrimvarElementSize(meshId, id);
      const groupScalars = components * elementSize;
      if (!Number.isSafeInteger(rawBytes) || rawBytes < 0 || rawBytes % 4 !== 0 ||
          !Number.isSafeInteger(indexBytes) || indexBytes < 0 || indexBytes % 4 !== 0 ||
          rawBytes > 0x20000000 || indexBytes > 0x20000000 ||
          !Number.isInteger(elementSize) || elementSize < 1 ||
          !Number.isSafeInteger(groupScalars) ||
          aggregateBytes > 0x20000000 - rawBytes - indexBytes) {
        throw new RangeError('getMeshPrimvarsJSON: aggregate primvar data exceeds 512 MiB');
      }
      aggregateBytes += rawBytes + indexBytes;
      const scalarCount = rawBytes / 4;
      if (scalarCount % groupScalars !== 0) {
        throw new RangeError('getMeshPrimvarsJSON: malformed primvar component count');
      }
      const indexCount = indexBytes / 4;
      const elementCount = indexCount || scalarCount / groupScalars;
      const expandedScalars = elementCount * groupScalars;
      // Charge 32 bytes per output scalar for the JS number, nested-array slot,
      // and its worst-case JSON decimal representation.
      if (!Number.isSafeInteger(expandedScalars) || expandedScalars > 0x20000000 / 32 ||
          aggregateBytes > 0x20000000 - expandedScalars * 32) {
        throw new RangeError('getMeshPrimvarsJSON: expanded primvar exceeds 512 MiB');
      }
      aggregateBytes += expandedScalars * 32;
      descriptors.push({id, nameBytes, baseType, components, scalarType,
        rawBytes, indexBytes, elementSize, groupScalars, scalarCount, elementCount});
    }
    result.primPath = this.meshViewString(meshId, 1);
    for (const descriptor of descriptors) {
      const {id, nameBytes, baseType, components, scalarType, rawBytes,
        indexBytes: indexByteLength, elementSize, groupScalars,
        scalarCount, elementCount} = descriptor;
      const name = this.meshPrimvarName(meshId, id);
      if (new TextEncoder().encode(name).byteLength !== nameBytes) {
        throw new RangeError('getMeshPrimvarsJSON: primvar name changed after preflight');
      }
      if (descriptor.unsupported) {
        result.primvars[name] = {name, error: 'unsupported primvar format'};
        continue;
      }
      const raw = this.meshPrimvarBuffer(meshId, id);
      const indexBytes = indexByteLength
        ? this.meshPrimvarIndicesBuffer(meshId, id) : new Uint8Array(0);
      if (raw.byteLength !== rawBytes || indexBytes.byteLength !== indexByteLength) {
        throw new RangeError('getMeshPrimvarsJSON: primvar payload changed after preflight');
      }
      const readScalar = offset => {
        const view = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
        return scalarType === 'float' ? view.getFloat32(offset, true)
          : scalarType === 'int' ? view.getInt32(offset, true) : view.getUint32(offset, true);
      };
      const indices = new Uint32Array(indexBytes.buffer, indexBytes.byteOffset,
        indexBytes.byteLength / 4);
      const values = new Array(elementCount);
      for (let element = 0; element < elementCount; ++element) {
        const sourceElement = indices.length ? indices[element] : element;
        if (sourceElement >= scalarCount / groupScalars) {
          throw new RangeError('getMeshPrimvarsJSON: primvar index is out of range');
        }
        const vectors = new Array(elementSize);
        for (let group = 0; group < elementSize; ++group) {
          if (components === 1) {
            vectors[group] = readScalar((sourceElement * groupScalars + group) * 4);
          } else {
            const tuple = new Array(components);
            for (let component = 0; component < components; ++component) {
              tuple[component] = readScalar(
                (sourceElement * groupScalars + group * components + component) * 4);
            }
            vectors[group] = tuple;
          }
        }
        values[element] = elementSize === 1 ? vectors[0] : vectors;
      }
      const interpolation = ['constant', 'uniform', 'vertex', 'faceVarying', 'varying'][
        this.meshPrimvarInterpolation(meshId, id)] || 'unknown';
      result.primvars[name] = {
        name, type: baseType + '[]', interpolation,
        elementSize,
        value: {type: baseType + '[]', value: values}
      };
    }
    const json = JSON.stringify(result);
    if (json.length > 0x20000000) {
      throw new RangeError('getMeshPrimvarsJSON: JSON output exceeds 512 MiB');
    }
    return json;
  }});
  const renderMeshBuffers = [
    ['meshPointsBuffer', 0, Float32Array],
    ['meshIndicesBuffer', 1, Uint32Array],
    ['meshNormalsBuffer', 2, Float32Array],
    ['meshUVBuffer', 3, Float32Array],
    ['meshTangentsBuffer', 4, Float32Array],
    ['meshColorsBuffer', 5, Float32Array],
    ['meshOpacitiesBuffer', 6, Float32Array],
    ['meshJointIndicesBuffer', 7, Uint16Array],
    ['meshJointWeightsBuffer', 8, Float32Array],
    ['meshSecondaryUVBuffer', 9, Float32Array]
  ];
  for (const [name, kind, ArrayType] of renderMeshBuffers) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(meshId) {
      if (arguments.length !== 1 || typeof meshId !== 'number') {
        throw new TypeError(name + ': expected one numeric mesh id');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_mesh_buffer',
          [meshId, kind], name, ArrayType, 'invalid mesh id or kind');
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'computeMeshTangents', {
    value: function(meshId) {
      if (arguments.length !== 1 || typeof meshId !== 'number' ||
          !Number.isInteger(meshId)) {
        throw new TypeError('computeMeshTangents: expected one integer mesh id');
      }
      // Like legacy, a request makes deferred tangents available for a
      // normal-mapped mesh and succeeds for every valid mesh id.
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      const status = Module['_lightusd_next_render_request_mesh_tangents'](state.handle, meshId);
      if (status < 0) throw new TypeError('Invalid LightUSD receiver');
      return status === 1;
    }
  });
  Object.defineProperty(Module.RenderStream.prototype, 'generateBoneTexture', {
    value: function(meshId, maxInfluences = 0) {
      const name = 'generateBoneTexture';
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length < 1 || arguments.length > 2 ||
          typeof meshId !== 'number' || !Number.isFinite(meshId) ||
          typeof maxInfluences !== 'number' || !Number.isFinite(maxInfluences)) {
        throw new TypeError(name + ': expected mesh id and optional influence count');
      }
      const id = Math.trunc(meshId), requested = Math.trunc(maxInfluences);
      if (id < 0 || id >= this.meshCount()) return {error: 'Invalid mesh ID or scene not loaded'};
      if (this.meshHasSkin(id) !== 1) return {error: 'Mesh has no skinning data'};

      const view = this.getMeshGeometryView(id);
      if (view.error) return {error: view.error};
      const elementSize = view.elementSize;
      if (elementSize <= 0) return {error: 'Invalid skinning data (elementSize <= 0)'};
      const jointIndices = this.meshJointIndicesBuffer(id);
      const jointWeights = this.meshJointWeightsBuffer(id);
      if (jointWeights.length < jointIndices.length) {
        return {error: 'Invalid skinning data (joint weight count mismatch)'};
      }

      const vertexCount = Math.floor(jointIndices.length / elementSize);
      const standards = [4, 8, 16, 32, 48, 64, 80, 96, 128];
      const influences = requested > 0 ? requested : elementSize;
      const maxInfl = standards.find(value => influences <= value) ?? 128;
      const texelsPerVertex = Math.ceil(maxInfl / 2);
      const maxTextureTexels = 4096 * 4096;
      if (vertexCount > maxTextureTexels / texelsPerVertex) {
        return {error: 'Bone texture dimensions too large'};
      }
      const totalTexels = vertexCount * texelsPerVertex;
      let textureWidth = 1;
      while (textureWidth * textureWidth < totalTexels && textureWidth < 4096) {
        textureWidth *= 2;
      }
      const textureHeight = Math.ceil(totalTexels / textureWidth);
      if (textureWidth > 4096 || textureHeight > 4096 || totalTexels > (1 << 30)) {
        return {error: 'Bone texture dimensions too large'};
      }
      const textureBytes = textureWidth * textureHeight * 16;
      const vertexOffsetBytes = vertexCount * 4;
      if (!Number.isSafeInteger(textureBytes) || !Number.isSafeInteger(vertexOffsetBytes) ||
          textureBytes + vertexOffsetBytes > 256 * 1024 * 1024) {
        return {error: 'Bone texture output exceeds 256 MiB limit'};
      }
      let textureData, vertexOffsets;
      try {
        textureData = new Float32Array(textureWidth * textureHeight * 4);
        vertexOffsets = new Float32Array(vertexCount);
      } catch (_error) {
        return {error: 'Bone texture allocation failed'};
      }

      for (let vertex = 0; vertex < vertexCount; ++vertex) {
        const influencesForVertex = [];
        const base = vertex * elementSize;
        for (let i = 0; i < elementSize && i < maxInfl; ++i) {
          const index = base + i;
          const weight = jointWeights[index];
          if (weight > 0) influencesForVertex.push([jointIndices[index], weight]);
        }
        influencesForVertex.sort((a, b) => b[1] - a[1]);
        const texelBase = vertex * texelsPerVertex * 4;
        for (let texel = 0; texel < texelsPerVertex; ++texel) {
          for (let pair = 0; pair < 2; ++pair) {
            const influence = influencesForVertex[texel * 2 + pair];
            const offset = texelBase + texel * 4 + pair * 2;
            textureData[offset] = influence ? influence[0] : -1;
            textureData[offset + 1] = influence ? influence[1] : 0;
          }
        }
        vertexOffsets[vertex] = vertex * texelsPerVertex;
      }
      return {
        textureData, textureWidth, textureHeight, texelsPerVertex,
        maxInfluences: maxInfl, vertexCount, originalElementSize: elementSize,
        vertexOffsets
      };
    }
  });
  Object.defineProperty(Module.RenderStream.prototype, 'meshViewString', {value: function(meshId, kind) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof meshId !== 'number' || typeof kind !== 'number') {
      throw new TypeError('meshViewString: expected numeric mesh id and kind');
    }
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try {
          return Module['_lightusd_next_render_mesh_view_string'](state.handle, meshId, kind, p, cap);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_mesh_view_string'](state.handle, meshId, kind,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError('meshViewString: invalid mesh id or kind');
      if (bytes === 0) return '';
      const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
      if (!Number.isSafeInteger(remaining) || remaining < 0 || bytes > remaining) {
        throw new RangeError('meshViewString: buffer exceeds remaining memory limit');
      }
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError('meshViewString: allocation failed');
      query(ptr, bytes);
      return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMeshGeometryView', {value: function(meshId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof meshId !== 'number') {
      throw new TypeError('getMeshGeometryView: expected one numeric mesh id');
    }
    if (!Number.isInteger(meshId) || meshId < 0 || meshId >= this.numMeshes()) {
      return {error: 'invalid mesh index'};
    }
    ++state.busy;
    let ptr = 0;
    let result;
    try {
      ptr = Module['_lightusd_next_alloc'](496);
      if (!ptr) throw new RangeError('getMeshGeometryView: allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, 496, true);
      const query = (out) => Module['_lightusd_next_render_mesh_view_get'](state.handle, meshId, out);
      let status;
      try { status = query(ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = query(BigInt(p));
      }
      if (status !== 0) {
        return {error: status === -2
          ? (this.meshViewString(meshId, 4) || 'mesh build failed')
          : 'invalid mesh index'};
      }
      const view = new DataView(Module.HEAPU8.buffer, p, 496);
      const flags = view.getUint32(4, true);
      const descriptors = [
        ['points', 'f32', 3, 4], ['indices', 'u32', 1, 4],
        ['normals', 'f32', 3, 4], ['uv0', 'f32', 2, 4],
        ['tangents', 'f32', 4, 4], ['jointIndices', 'u16', 1, 2],
        ['jointWeights', 'f32', 1, 4]
      ];
      const matrix = (offset) => Array.from({length: 16}, (_, i) =>
        view.getFloat64(offset + i * 8, true));
      result = {
        vertexCount: Math.floor(view.getUint32(464, true) / 3),
        doubleSided: !!(flags & 1),
        materialId: view.getInt32(8, true),
        skel_id: view.getInt32(12, true),
        elementSize: view.getInt32(16, true),
        hasGeomBindTransform: !!(flags & 2),
        localMatrix: matrix(24), worldMatrix: matrix(152),
        geomBindTransform: flags & 2 ? matrix(280) : undefined
      };
      descriptors.forEach(([name, dtype, comps, bytesPerElement], i) => {
        const length = view.getUint32(464 + i * 4, true);
        if (name !== 'points' && length === 0) return;
        result[name] = {ptr: Number(view.getBigUint64(408 + i * 8, true)),
          length, comps, dtype, byteLength: length * bytesPerElement};
      });
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    result.primName = this.meshViewString(meshId, 0);
    result.primPath = this.meshViewString(meshId, 1);
    if (result.hasGeomBindTransform) result.skeletonPath = this.meshViewString(meshId, 2);
    if (result.tangents) result.tangentMethod = this.meshViewString(meshId, 3);
    return result;
  }});
  const outputPod = (receiver, symbol, args, size, label) => {
    const state = live.get(receiver);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError(label + ': allocation failed');
      const p = Number(ptr);
      new DataView(Module.HEAPU8.buffer).setUint32(p, size, true);
      const query = (out) => Module[symbol](state.handle, ...args, out);
      let status;
      try { status = query(ptr); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = query(BigInt(p));
      }
      if (status !== 0) throw new RangeError(label + ': invalid material or texture');
      return new Uint8Array(Module.HEAPU8.buffer, p, size).slice();
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  };
  const outputString = (receiver, symbol, args, label) => {
    const state = live.get(receiver);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module[symbol](state.handle, ...args, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module[symbol](state.handle, ...args,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const bytes = query(0, 0);
      if (bytes < 0) throw new RangeError(label + ': invalid material or string kind');
      if (!Number.isSafeInteger(bytes) || bytes > 0x20000000) {
        throw new RangeError(label + ': string exceeds 512 MiB limit');
      }
      if (bytes === 0) return '';
      const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
      if (!Number.isSafeInteger(remaining) || remaining < 0 || bytes > remaining) {
        throw new RangeError(label + ': string exceeds remaining memory limit');
      }
      ptr = Module['_lightusd_next_alloc'](bytes);
      if (!ptr) throw new RangeError(label + ': allocation failed');
      query(ptr, bytes);
      return new TextDecoder().decode(new Uint8Array(Module.HEAPU8.buffer, Number(ptr), bytes));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  };
  Object.defineProperty(Module.RenderStream.prototype, 'outputMaterialInfo', {value: function(materialId) {
    if (arguments.length !== 1 || typeof materialId !== 'number') {
      throw new TypeError('outputMaterialInfo: expected one numeric material id');
    }
    const bytes = outputPod(this, '_lightusd_next_render_output_material_info_get',
      [materialId], 176, 'outputMaterialInfo');
    const view = new DataView(bytes.buffer);
    return {id: view.getInt32(4, true), flags: view.getUint32(8, true),
      value: new Float32Array(bytes.buffer, 16, 40)};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'outputMaterialString', {value: function(materialId, kind) {
    if (arguments.length !== 2 || typeof materialId !== 'number' || typeof kind !== 'number') {
      throw new TypeError('outputMaterialString: expected numeric material id and kind');
    }
    return outputString(this, '_lightusd_next_render_output_material_string',
      [materialId, kind], 'outputMaterialString');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMaterialWithFormat', {value: function(materialId, format) {
    if (arguments.length !== 2 || typeof materialId !== 'number' ||
        !Number.isInteger(materialId) || typeof format !== 'string') {
      throw new TypeError('getMaterialWithFormat: expected integer material id and format string');
    }
    const formatCode = format === 'json' ? 0 : format === 'xml' ? 1 :
      (format === '' || format === 'legacy') ? 2 : 255;
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    const status = Module['_lightusd_next_render_material_format_status'](
      state.handle, materialId, formatCode);
    if (status < 0) throw new RangeError('getMaterialWithFormat: invalid render stream');
    const data = outputString(this, '_lightusd_next_render_material_format_string',
      [materialId, formatCode], 'getMaterialWithFormat');
    return status === 1 ? (formatCode === 2 ? JSON.parse(data) : {data, format})
      : {error: data};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getLightWithFormat', {value: function(lightId, format) {
    if (arguments.length !== 2 || typeof lightId !== 'number' ||
        !Number.isInteger(lightId) ||
        (typeof format !== 'string' &&
          !(ArrayBuffer.isView(format) && format.BYTES_PER_ELEMENT === 1))) {
      throw new TypeError('getLightWithFormat: expected integer light id and format string');
    }
    const formatName = typeof format === 'string' ? format : new TextDecoder().decode(format);
    const formatCode = formatName === 'json' ? 0 : formatName === 'xml' ? 1 : 255;
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    const status = Module['_lightusd_next_render_light_format_status'](
      state.handle, lightId, formatCode);
    if (status < 0) throw new RangeError('getLightWithFormat: invalid render stream');
    const data = outputString(this, '_lightusd_next_render_light_format_string',
      [lightId, formatCode], 'getLightWithFormat');
    return status === 1 ? {data, format: formatName} : {error: data};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMaterial', {value: function(materialId) {
    if (arguments.length !== 1 || typeof materialId !== 'number' ||
        !Number.isInteger(materialId)) {
      throw new TypeError('getMaterial: expected one integer material id');
    }
    return this.getMaterialWithFormat(materialId, 'json');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'outputTextureMeta', {value: function(materialId, slot) {
    if (arguments.length !== 2 || typeof materialId !== 'number' || typeof slot !== 'number') {
      throw new TypeError('outputTextureMeta: expected numeric material id and slot');
    }
    const bytes = outputPod(this, '_lightusd_next_render_output_texture_meta_get',
      [materialId, slot], 52, 'outputTextureMeta');
    const view = new DataView(bytes.buffer);
    return {flags: view.getUint32(4, true), sourceGamma: view.getFloat32(8, true),
      sourceLinearBias: view.getFloat32(12, true),
      sourceToDisplayLinear: Array.from(new Float32Array(bytes.buffer, 16, 9))};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'outputTextureString', {value: function(materialId, slot, kind) {
    if (arguments.length !== 3 || typeof materialId !== 'number' ||
        typeof slot !== 'number' || typeof kind !== 'number') {
      throw new TypeError('outputTextureString: expected numeric material id, slot and kind');
    }
    return outputString(this, '_lightusd_next_render_output_texture_string',
      [materialId, slot, kind], 'outputTextureString');
  }});
  const outputTextureSlots = [
    ['baseColorTexture', 'baseColor'], ['normalTexture', 'normal'],
    ['roughnessTexture', 'roughness'], ['metallicTexture', 'metallic'],
    ['occlusionTexture', 'occlusion'], ['emissiveTexture', 'emissive'],
    ['opacityTexture', 'opacity']
  ];
  Object.defineProperty(Module.RenderStream.prototype, 'getOutputMaterial', {value: function(materialId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof materialId !== 'number') {
      throw new TypeError('getOutputMaterial: expected one numeric material id');
    }
    if (!Number.isInteger(materialId) || materialId < 0) return {};
    const info = this.outputMaterialInfo(materialId);
    const v = info.value;
    const out = {id: info.id,
      key: this.outputMaterialString(materialId, 0),
      primPath: this.outputMaterialString(materialId, 1),
      baseColor: Array.from(v.subarray(0, 3)), metallic: v[3],
      roughness: v[4], opacity: v[5], occlusion: v[6],
      emissive: Array.from(v.subarray(7, 10))};
    if (info.flags & 1) {
      out.shaderType = this.outputMaterialString(materialId, 2);
      out.workingColorSpace = this.outputMaterialString(materialId, 3);
      out.workingToDisplayLinear = Array.from(v.subarray(11, 20));
      out.materialXConfig = {authored: !!(info.flags & 2),
        version: this.outputMaterialString(materialId, 6),
        namespace: this.outputMaterialString(materialId, 7),
        colorspace: this.outputMaterialString(materialId, 8),
        sourceUri: this.outputMaterialString(materialId, 9)};
      out.materialXJson = this.outputMaterialString(materialId, 4);
      const nodegraph = this.outputMaterialString(materialId, 5);
      if (nodegraph) out.openPBRNodeGraphJson = nodegraph;
      const volumeNodegraph = this.outputMaterialString(materialId, 10);
      if (volumeNodegraph) out.volumeNodeGraphJson = volumeNodegraph;
      const previewSurfaceNodegraph = this.outputMaterialString(materialId, 11);
      if (previewSurfaceNodegraph) {
        out.previewSurfaceNodeGraphJson = previewSurfaceNodegraph;
      }
    }
    if (info.flags & 4) {
      out.hair = {model: 'chiang_hair_bsdf',
        tintR: Array.from(v.subarray(20, 23)),
        tintTT: Array.from(v.subarray(23, 26)),
        tintTRT: Array.from(v.subarray(26, 29)),
        roughnessR: Array.from(v.subarray(29, 31)),
        roughnessTT: Array.from(v.subarray(31, 33)),
        roughnessTRT: Array.from(v.subarray(33, 35)),
        absorptionCoefficient: Array.from(v.subarray(35, 38)),
        ior: v[38], cuticleAngle: v[39]};
    }
    if (v[10] > 0) out.opacityThreshold = v[10];
    const metadata = {};
    outputTextureSlots.forEach(([field, key], slot) => {
      const texturePath = this.outputTextureString(materialId, slot, 0);
      if (texturePath) out[field] = texturePath;
      const meta = this.outputTextureMeta(materialId, slot);
      if (!(meta.flags & 1)) return;
      metadata[key] = {path: this.outputTextureString(materialId, slot, 1),
        sourceColorSpace: this.outputTextureString(materialId, slot, 2),
        wrapS: this.outputTextureString(materialId, slot, 3),
        wrapT: this.outputTextureString(materialId, slot, 4),
        isUdim: !!(meta.flags & 2), colorTransformValid: !!(meta.flags & 4),
        colorTransformBypass: !!(meta.flags & 8), sourceColorIsData: !!(meta.flags & 16),
        sourceGamma: meta.sourceGamma, sourceLinearBias: meta.sourceLinearBias,
        sourceToDisplayLinear: meta.sourceToDisplayLinear};
    });
    out.textureMetadata = metadata;
    return out;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMeshSubsetOutput', {value: function(meshId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof meshId !== 'number') {
      throw new TypeError('getMeshSubsetOutput: expected one numeric mesh id');
    }
    ++state.busy;
    let ptr = 0;
    let bytes;
    try {
      const query = (p, cap) => {
        try { return Module['_lightusd_next_render_mesh_subset_output'](state.handle, meshId, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_mesh_subset_output'](state.handle, meshId,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const size = query(0, 0);
      if (size < 0) throw new RangeError('getMeshSubsetOutput: invalid mesh id');
      if (size === 0) return null;
      const rawEstimate = size * 2 + 128;
      if (!Number.isSafeInteger(size) || size < 0 || rawEstimate > 0x20000000) {
        throw new RangeError('getMeshSubsetOutput: payload exceeds 512 MiB aggregate limit');
      }
      preflightRenderAggregate(state, rawEstimate, 'getMeshSubsetOutput');
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('getMeshSubsetOutput: allocation failed');
      if (query(ptr, size) !== size) throw new RangeError('getMeshSubsetOutput: payload changed');
      const view = new DataView(Module.HEAPU8.buffer, Number(ptr), size);
      const materialCount = view.getInt32(0, true);
      const groupCount = view.getInt32(4, true);
      if (materialCount < 0 || groupCount < 0 || materialCount > 65536 ||
          groupCount > 65536 || size !== 8 + materialCount * 4 + groupCount * 12) {
        throw new RangeError('getMeshSubsetOutput: invalid or excessive payload');
      }
      const recordEstimate = rawEstimate + materialCount * 4096 + groupCount * 128;
      if (recordEstimate > 0x20000000) {
        throw new RangeError('getMeshSubsetOutput: aggregate records exceed 512 MiB limit');
      }
      preflightRenderAggregate(state, recordEstimate, 'getMeshSubsetOutput');
      bytes = new Uint8Array(Module.HEAPU8.buffer, Number(ptr), size).slice();
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
    const view = new DataView(bytes.buffer);
    const materialCount = view.getInt32(0, true);
    const groupCount = view.getInt32(4, true);
    if (materialCount < 0 || groupCount < 0 || materialCount > 65536 ||
        groupCount > 65536 ||
        bytes.length !== 8 + materialCount * 4 + groupCount * 12) {
      throw new RangeError('getMeshSubsetOutput: invalid payload');
    }
    const materials = Array.from({length: materialCount}, (_, i) =>
      this.getOutputMaterial(view.getInt32(8 + i * 4, true)));
    const submeshes = Array.from({length: groupCount}, (_, i) => {
      const base = 8 + materialCount * 4 + i * 12;
      return {start: view.getInt32(base, true), count: view.getInt32(base + 4, true),
        materialIndex: view.getInt32(base + 8, true)};
    });
    return {materials, submeshes};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'meshBlendShapeInfo', {value: function(meshId, shapeId, inbetweenId = -1) {
    const bytes = outputPod(this, '_lightusd_next_render_mesh_blend_shape_info_get',
      [meshId, shapeId, inbetweenId], 16, 'meshBlendShapeInfo');
    const view = new DataView(bytes.buffer);
    return {weight: view.getFloat32(4, true),
      inbetweenCount: view.getUint32(8, true), flags: view.getUint32(12, true)};
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'meshBlendShapeName', {value: function(meshId, shapeId, inbetweenId = -1) {
    return outputString(this, '_lightusd_next_render_mesh_blend_shape_name',
      [meshId, shapeId, inbetweenId], 'meshBlendShapeName');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'meshBlendShapeOffsets', {value: function(meshId, shapeId, inbetweenId = -1, kind = 0) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    ++state.busy;
    let ptr = 0;
    try {
      const query = (p, cap) => {
        try { return Module['_lightusd_next_render_mesh_blend_shape_offsets'](
          state.handle, meshId, shapeId, inbetweenId, kind, p, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_mesh_blend_shape_offsets'](
            state.handle, meshId, shapeId, inbetweenId, kind,
            typeof p === 'bigint' ? Number(p) : BigInt(p), cap);
        }
      };
      const size = query(0, 0);
      if (size < 0 || size % 4) throw new RangeError('meshBlendShapeOffsets: invalid shape');
      if (size === 0) return [];
      const aggregateEstimate = size * 2 + 128;
      if (!Number.isSafeInteger(aggregateEstimate) || aggregateEstimate > 0x20000000) {
        throw new RangeError('meshBlendShapeOffsets: payload exceeds 512 MiB aggregate limit');
      }
      preflightRenderAggregate(state, aggregateEstimate, 'meshBlendShapeOffsets');
      ptr = Module['_lightusd_next_alloc'](size);
      if (!ptr) throw new RangeError('meshBlendShapeOffsets: allocation failed');
      if (query(ptr, size) !== size) throw new RangeError('meshBlendShapeOffsets: payload changed');
      const copy = new Uint8Array(Module.HEAPU8.buffer, Number(ptr), size).slice();
      return Array.from(new Float32Array(copy.buffer));
    } finally {
      if (ptr) Module['_lightusd_next_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMeshBlendShapes', {value: function(meshId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof meshId !== 'number') {
      throw new TypeError('getMeshBlendShapes: expected one numeric mesh id');
    }
    ++state.busy;
    let count;
    try { count = Module['_lightusd_next_render_mesh_blend_shape_count'](state.handle, meshId); }
    finally { --state.busy; }
    if (count < 0) throw new RangeError('getMeshBlendShapes: invalid mesh id');
    if (count === 0) return undefined;
    if (!Number.isSafeInteger(count) || count > 65536) {
      throw new RangeError('getMeshBlendShapes: excessive shape count');
    }
    const plans = [];
    let aggregateBytes = count * 4096;
    const addPayload = (size, label) => {
      if (!Number.isSafeInteger(size) || size < 0 || size > 0x20000000) {
        throw new RangeError('getMeshBlendShapes: invalid ' + label + ' size');
      }
      aggregateBytes += size * 3 + 128;
      if (!Number.isSafeInteger(aggregateBytes) || aggregateBytes > 0x20000000) {
        throw new RangeError('getMeshBlendShapes: aggregate exceeds 512 MiB limit');
      }
    };
    const querySize = (symbol, args, label) => {
      const query = pointer => {
        try { return Module[symbol](state.handle, ...args, pointer, 0); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module[symbol](state.handle, ...args,
            typeof pointer === 'bigint' ? Number(pointer) : BigInt(pointer), 0);
        }
      };
      const size = query(0);
      if (!Number.isSafeInteger(size) || size < 0) {
        throw new RangeError('getMeshBlendShapes: invalid ' + label + ' query');
      }
      return size;
    };
    let recordCount = 0;
    for (let shapeId = 0; shapeId < count; ++shapeId) {
      const info = this.meshBlendShapeInfo(meshId, shapeId);
      if (!Number.isSafeInteger(info.inbetweenCount) || info.inbetweenCount > 65536 ||
          recordCount + 1 + info.inbetweenCount > 65536) {
        throw new RangeError('getMeshBlendShapes: excessive inbetween count');
      }
      recordCount += 1 + info.inbetweenCount;
      const plan = {shapeId, info, inbetweens: []};
      plans.push(plan);
      addPayload(querySize('_lightusd_next_render_mesh_blend_shape_name',
        [meshId, shapeId, -1], 'shape name'), 'shape name');
      addPayload(querySize('_lightusd_next_render_mesh_blend_shape_offsets',
        [meshId, shapeId, -1, 0], 'point offsets'), 'point offsets');
      addPayload(querySize('_lightusd_next_render_mesh_blend_shape_offsets',
        [meshId, shapeId, -1, 1], 'normal offsets'), 'normal offsets');
      for (let inbetweenId = 0; inbetweenId < info.inbetweenCount; ++inbetweenId) {
        const between = this.meshBlendShapeInfo(meshId, shapeId, inbetweenId);
        plan.inbetweens.push({inbetweenId, between});
        addPayload(querySize('_lightusd_next_render_mesh_blend_shape_name',
          [meshId, shapeId, inbetweenId], 'inbetween name'), 'inbetween name');
        addPayload(querySize('_lightusd_next_render_mesh_blend_shape_offsets',
          [meshId, shapeId, inbetweenId, 0], 'inbetween offsets'), 'inbetween offsets');
      }
    }
    preflightRenderAggregate(state, aggregateBytes, 'getMeshBlendShapes');
    return plans.map(({shapeId, info, inbetweens: planned}) => {
      const inbetweens = planned.map(({inbetweenId, between}) => ({
        name: this.meshBlendShapeName(meshId, shapeId, inbetweenId),
        weight: between.weight,
        pointOffsets: this.meshBlendShapeOffsets(meshId, shapeId, inbetweenId)
      }));
      return {name: this.meshBlendShapeName(meshId, shapeId), weight: info.weight,
        pointOffsets: this.meshBlendShapeOffsets(meshId, shapeId),
        normalOffsets: this.meshBlendShapeOffsets(meshId, shapeId, -1, 1),
        pointIndices: [], inbetweens};
    });
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMesh', {value: function(meshId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('getMesh: wrong argument count');
    if (typeof meshId !== 'number') throw new TypeError('getMesh: expected number');
    const geometry = this.getMeshGeometryView(meshId);
    if (geometry.error) return geometry;
    const out = {...geometry, material: this.getOutputMaterial(geometry.materialId),
      purpose: this.meshPurpose(meshId)};
    const subsets = this.getMeshSubsetOutput(meshId);
    if (subsets) Object.assign(out, subsets);
    const shapes = this.getMeshBlendShapes(meshId);
    if (shapes) out.blendShapes = shapes;
    return out;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getMeshCopy', {value: function(meshId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof meshId !== 'number' || !Number.isInteger(meshId)) {
      throw new TypeError('getMeshCopy: expected one integer mesh id');
    }
    if (meshId < 0 || meshId >= this.numMeshes()) return {error: 'invalid mesh index'};

    const streams = [
      ['points', 0, 'meshPointsBuffer'], ['indices', 1, 'meshIndicesBuffer'],
      ['normals', 2, 'meshNormalsBuffer'], ['uv0', 3, 'meshUVBuffer'],
      ['tangents', 4, 'meshTangentsBuffer'], ['colors', 5, 'meshColorsBuffer'],
      ['opacities', 6, 'meshOpacitiesBuffer'],
      ['jointIndices', 7, 'meshJointIndicesBuffer'],
      ['jointWeights', 8, 'meshJointWeightsBuffer'],
      ['secondaryUVs', 9, 'meshSecondaryUVBuffer']
    ];
    let aggregateBytes = 0;
    ++state.busy;
    try {
      for (const [, kind] of streams) {
        let bytes;
        try {
          bytes = Module['_lightusd_next_render_mesh_buffer'](state.handle, meshId, kind, 0, 0);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          bytes = Module['_lightusd_next_render_mesh_buffer'](
            state.handle, meshId, kind, BigInt(0), 0);
        }
        const elementBytes = kind === 7 ? 2 : 4;
        if (!Number.isSafeInteger(bytes) || bytes < 0 || bytes % elementBytes !== 0) {
          throw new RangeError('getMeshCopy: mesh buffer size query failed');
        }
        aggregateBytes += bytes;
        if (aggregateBytes > 512 * 1024 * 1024) {
          throw new RangeError('getMeshCopy: aggregate payload exceeds 512 MiB limit');
        }
      }
      let remaining;
      try { remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
      }
      if (!Number.isSafeInteger(remaining) || remaining < 0 || aggregateBytes > remaining) {
        throw new RangeError('getMeshCopy: aggregate payload exceeds remaining memory limit');
      }
    } finally {
      --state.busy;
    }

    const out = this.getMesh(meshId);
    if (out.error) return out;
    for (const [name, , method] of streams) {
      const data = this[method](meshId);
      if (name === 'points' || data.length !== 0) out[name] = data;
    }
    // Keep the common legacy spelling as an alias over the same owned bytes.
    out.faceVertexIndices = out.indices;
    out.absPath = out.primPath;
    return out;
  }});
  const renderMaterialFields = [
    ['materialShaderType', 0], ['materialAlphaMode', 1],
    ['materialDoubleSided', 2], ['materialOpacityScaled', 3],
    ['materialRoughnessScaled', 4], ['materialClearcoatScaled', 5],
    ['materialClearcoatRoughnessScaled', 6], ['materialDefaultFallback', 7]
  ];
  for (const [name, field] of renderMaterialFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(materialId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) {
        throw new TypeError('Invalid LightUSD receiver');
      }
      if (arguments.length !== 1 || typeof materialId !== 'number') {
        throw new TypeError(name + ': expected one numeric material id');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_material_field'](state.handle, materialId, field);
      } finally {
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'getMaterialDiagnostics', {value: function(materialId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof materialId !== 'number' || !Number.isInteger(materialId)) {
      throw new TypeError('getMaterialDiagnostics: expected one integer material id');
    }
    if (materialId < 0 || materialId >= this.numMaterials()) return [];
    ++state.busy;
    let count;
    try {
      count = Module['_lightusd_next_render_material_diagnostic_count'](
        state.handle, materialId);
    } finally { --state.busy; }
    if (count < 0) throw new RangeError('getMaterialDiagnostics: invalid material id');
    if (!Number.isSafeInteger(count) || count > 65536) {
      throw new RangeError('getMaterialDiagnostics: excessive diagnostic count');
    }
    const stringBytes = (diagnosticId, kind) => {
      const query = pointer => {
        try {
          return Module['_lightusd_next_render_material_diagnostic_string'](
            state.handle, materialId, diagnosticId, kind, pointer, 0);
        } catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_next_render_material_diagnostic_string'](
            state.handle, materialId, diagnosticId, kind,
            typeof pointer === 'bigint' ? Number(pointer) : BigInt(pointer), 0);
        }
      };
      const size = query(0);
      if (!Number.isSafeInteger(size) || size < 0 || size > 0x20000000) {
        throw new RangeError('getMaterialDiagnostics: invalid or excessive string size');
      }
      return size;
    };
    let aggregateBytes = count * 4096;
    if (aggregateBytes > 0x20000000) {
      throw new RangeError('getMaterialDiagnostics: aggregate exceeds 512 MiB limit');
    }
    for (let diagnosticId = 0; diagnosticId < count; ++diagnosticId) {
      for (let kind = 0; kind < 4; ++kind) {
        const size = stringBytes(diagnosticId, kind);
        aggregateBytes += size * 2 + 128;
        if (!Number.isSafeInteger(aggregateBytes) || aggregateBytes > 0x20000000) {
          throw new RangeError('getMaterialDiagnostics: aggregate exceeds 512 MiB limit');
        }
      }
    }
    preflightRenderAggregate(state, aggregateBytes, 'getMaterialDiagnostics');
    const string = (diagnosticId, kind) => outputString(this,
      '_lightusd_next_render_material_diagnostic_string',
      [materialId, diagnosticId, kind], 'getMaterialDiagnostics');
    return Array.from({length: count}, (_, diagnosticId) => {
      ++state.busy;
      let kind;
      try {
        kind = Module['_lightusd_next_render_material_diagnostic_kind'](
          state.handle, materialId, diagnosticId);
      } finally { --state.busy; }
      if (kind < 0) throw new RangeError('getMaterialDiagnostics: invalid diagnostic');
      return {
        kind,
        material_path: string(diagnosticId, 0),
        node_path: string(diagnosticId, 1),
        shader_id: string(diagnosticId, 2),
        message: string(diagnosticId, 3)
      };
    });
  }});
  const materialParamNames = [
    ['materialBaseColorBuffer', 0], ['materialEmissiveBuffer', 1],
    ['materialMetallicBuffer', 2], ['materialRoughnessBuffer', 3],
    ['materialOpacityBuffer', 4]
  ];
  for (const [name, param] of materialParamNames) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(materialId) {
      if (arguments.length !== 1 || typeof materialId !== 'number') {
        throw new TypeError(name + ': expected one numeric material id');
      }
      return copyRenderBuffer(this, '_lightusd_next_render_material_param_buffer',
        [materialId, param], name, Float32Array, 'invalid material or parameter');
    }});
    const textureName = name.replace('Buffer', 'TextureId');
    Object.defineProperty(Module.RenderStream.prototype, textureName, {value: function(materialId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== 1 || typeof materialId !== 'number') {
        throw new TypeError(textureName + ': expected one numeric material id');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_material_param_texture'](state.handle, materialId, param);
      } finally { --state.busy; }
    }});
  }
  const kMaxMaterialAggregateBytes = 0x20000000;
  const materialPayloadEstimate = (stream, materialId) => {
    const state = live.get(stream);
    let total = 512;
    const query = (symbol, args, pointer = true) => {
      if (!pointer) return Module[symbol](state.handle, ...args);
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    const chargeString = bytes => {
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('material payload: string size query failed');
      }
      total += bytes * 2 + 64;
      if (total > kMaxMaterialAggregateBytes) {
        throw new RangeError('material payload: aggregate returned data exceeds 512 MiB limit');
      }
    };
    chargeString(query('_lightusd_next_render_resource_name', [2, materialId]));
    chargeString(query('_lightusd_next_render_resource_path', [2, materialId]));
    const diagnosticCount = query(
      '_lightusd_next_render_material_diagnostic_count', [materialId], false);
    if (!Number.isSafeInteger(diagnosticCount) || diagnosticCount < 0 || diagnosticCount > 65536) {
      throw new RangeError('material payload: invalid or excessive diagnostic count');
    }
    total += diagnosticCount * 256;
    for (let diagnosticId = 0; diagnosticId < diagnosticCount; ++diagnosticId) {
      for (let kind = 0; kind < 4; ++kind) {
        chargeString(query('_lightusd_next_render_material_diagnostic_string',
          [materialId, diagnosticId, kind]));
      }
    }
    const shaderType = stream.materialShaderType(materialId);
    if (shaderType !== 0) {
      for (let param = 0; param < 5; ++param) {
        const bytes = query('_lightusd_next_render_material_param_buffer',
          [materialId, param]);
        if (!Number.isSafeInteger(bytes) || bytes < 0 || bytes % 4 !== 0) {
          throw new RangeError('material payload: parameter buffer size query failed');
        }
        total += bytes * (param < 2 ? 3 : 1);
        if (total > kMaxMaterialAggregateBytes) {
          throw new RangeError('material payload: aggregate returned data exceeds 512 MiB limit');
        }
      }
    }
    return total;
  };
  Object.defineProperty(Module.RenderStream.prototype, 'getMaterialRecord', {value: function(materialId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof materialId !== 'number' || !Number.isInteger(materialId)) {
      throw new TypeError('getMaterialRecord: expected one integer material id');
    }
    if (materialId < 0 || materialId >= this.numMaterials()) return {};
    materialPayloadEstimate(this, materialId);
    const shaderType = this.materialShaderType(materialId);
    const record = {
      id: materialId,
      name: nodeDecoder.decode(this.resourceNameBuffer(2, materialId)),
      primPath: nodeDecoder.decode(this.resourcePathBuffer(2, materialId)),
      shaderType,
      alphaMode: this.materialAlphaMode(materialId),
      doubleSided: this.materialDoubleSided(materialId) !== 0,
      defaultFallback: this.materialDefaultFallback(materialId) !== 0,
      opacity: this.materialOpacityScaled(materialId) / 1000000,
      roughness: this.materialRoughnessScaled(materialId) / 1000000,
      clearcoat: this.materialClearcoatScaled(materialId) / 1000000,
      clearcoatRoughness: this.materialClearcoatRoughnessScaled(materialId) / 1000000,
      textureIds: {},
      diagnostics: this.getMaterialDiagnostics(materialId)
    };
    // ShaderType::None has no common shading parameters. Preserve its identity
    // and scalar metadata without turning an unsupported material into an error.
    if (shaderType !== 0) {
      const baseColor = this.materialBaseColorBuffer(materialId);
      const emissive = this.materialEmissiveBuffer(materialId);
      record.baseColor = Array.from(baseColor.subarray(0, 3));
      record.emissive = Array.from(emissive.subarray(0, 3));
      for (const [name, buffer, textureId] of [
        ['baseColor', 'materialBaseColorBuffer', 'materialBaseColorTextureId'],
        ['emissive', 'materialEmissiveBuffer', 'materialEmissiveTextureId'],
        ['metallic', 'materialMetallicBuffer', 'materialMetallicTextureId'],
        ['roughness', 'materialRoughnessBuffer', 'materialRoughnessTextureId'],
        ['opacity', 'materialOpacityBuffer', 'materialOpacityTextureId']
      ]) {
        record.textureIds[name] = this[textureId](materialId);
        if (name !== 'baseColor' && name !== 'emissive') {
          record[name] = this[buffer](materialId)[0];
        }
      }
    }
    return record;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getAllMaterials', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getAllMaterials: wrong argument count');
    const count = this.numMaterials();
    if (!Number.isSafeInteger(count) || count < 0 || count > 65536) {
      throw new RangeError('getAllMaterials: invalid or excessive material count');
    }
    let aggregateBytes = 0;
    for (let materialId = 0; materialId < count; ++materialId) {
      aggregateBytes += materialPayloadEstimate(this, materialId);
      if (aggregateBytes > kMaxMaterialAggregateBytes) {
        throw new RangeError('getAllMaterials: aggregate returned data exceeds 512 MiB limit');
      }
    }
    preflightRenderAggregate(state, aggregateBytes, 'getAllMaterials');
    return Array.from({length: count}, (_, materialId) => this.getMaterialRecord(materialId));
  }});
  const renderTextureFields = [
    ['textureImageId', 0], ['textureWidth', 1], ['textureHeight', 2],
    ['textureChannels', 3], ['textureMipLevels', 4], ['textureLoaded', 5],
    ['textureWrapS', 6], ['textureWrapT', 7], ['textureOutputChannel', 8],
    ['textureRotationScaled', 9], ['textureHasTransform2d', 10],
    ['textureIsUDIM', 11], ['textureUDIMId', 12]
  ];
  for (const [name, field] of renderTextureFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function(textureId) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) {
        throw new TypeError('Invalid LightUSD receiver');
      }
      if (arguments.length !== 1 || typeof textureId !== 'number') {
        throw new TypeError(name + ': expected one numeric texture id');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_texture_field'](state.handle, textureId, field);
      } finally {
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'textureImageBuffer', {value: function(textureId) {
    if (arguments.length !== 1 || typeof textureId !== 'number') {
      throw new TypeError('textureImageBuffer: expected one numeric texture id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_texture_buffer',
      [textureId], 'textureImageBuffer', Uint8Array, 'invalid texture id');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'textureSamplingBuffer', {value: function(textureId) {
    if (arguments.length !== 1 || typeof textureId !== 'number') {
      throw new TypeError('textureSamplingBuffer: expected one numeric texture id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_texture_sampling_buffer',
      [textureId], 'textureSamplingBuffer', Float32Array, 'invalid texture id');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'textureTransformBuffer', {value: function(textureId) {
    if (arguments.length !== 1 || typeof textureId !== 'number') {
      throw new TypeError('textureTransformBuffer: expected one numeric texture id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_texture_transform_buffer',
      [textureId], 'textureTransformBuffer', Float32Array, 'invalid texture id');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'textureUDIMRemapBuffer', {value: function(textureId) {
    if (arguments.length !== 1 || typeof textureId !== 'number') {
      throw new TypeError('textureUDIMRemapBuffer: expected one numeric texture id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_texture_udim_remap_buffer',
      [textureId], 'textureUDIMRemapBuffer', Float32Array, 'invalid texture id');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'udimTileCount', {value: function(udimId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof udimId !== 'number' || !Number.isInteger(udimId)) {
      throw new TypeError('udimTileCount: expected one integer UDIM id');
    }
    ++state.busy;
    try { return Module['_lightusd_next_render_udim_tile_count'](state.handle, udimId); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'numUDIMTextures', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('numUDIMTextures: wrong argument count');
    ++state.busy;
    try { return Module['_lightusd_next_render_udim_count'](state.handle); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'udimTileBuffer', {value: function(udimId) {
    if (arguments.length !== 1 || typeof udimId !== 'number' || !Number.isInteger(udimId)) {
      throw new TypeError('udimTileBuffer: expected one integer UDIM id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_udim_tiles',
      [udimId], 'udimTileBuffer', Int32Array, 'invalid UDIM id');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'udimString', {value: function(udimId, kind) {
    if (arguments.length !== 2 || typeof udimId !== 'number' ||
        typeof kind !== 'number' || !Number.isInteger(kind) || kind < 0 || kind > 3) {
      throw new TypeError('udimString: expected UDIM id and string kind 0..3');
    }
    return outputString(this, '_lightusd_next_render_udim_string',
      [udimId, kind], 'udimString');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'textureColorTransformBuffer', {value: function(textureId) {
    if (arguments.length !== 1 || typeof textureId !== 'number') {
      throw new TypeError('textureColorTransformBuffer: expected one numeric texture id');
    }
    return copyRenderBuffer(this, '_lightusd_next_render_texture_color_transform_buffer',
      [textureId], 'textureColorTransformBuffer', Float32Array, 'invalid texture id');
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'textureString', {value: function(textureId, kind) {
    if (arguments.length !== 2 || typeof textureId !== 'number' ||
        typeof kind !== 'number' || !Number.isInteger(kind) || kind < 0 || kind > 2) {
      throw new TypeError('textureString: expected texture id and string kind 0..2');
    }
    return outputString(this, '_lightusd_next_render_texture_string',
      [textureId, kind], 'textureString');
  }});
  const texturePayloadEstimate = (stream, textureId) => {
    const state = live.get(stream);
    let total = 1024;
    const check = label => {
      if (!Number.isSafeInteger(total) || total > 0x20000000) {
        throw new RangeError(label + ': returned payload exceeds 512 MiB limit');
      }
    };
    const querySize = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    for (const [symbol, args] of [
      ['_lightusd_next_render_resource_name', [3, textureId]],
      ['_lightusd_next_render_resource_path', [3, textureId]],
      ...[0, 1, 2].map(kind => ['_lightusd_next_render_texture_string', [textureId, kind]])
    ]) {
      const bytes = querySize(symbol, args);
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('texture payload: string size query failed');
      }
      total += bytes * 2 + 64;
      check('texture payload');
    }
    for (const symbol of ['_lightusd_next_render_texture_sampling_buffer',
      '_lightusd_next_render_texture_color_transform_buffer',
      '_lightusd_next_render_texture_transform_buffer',
      '_lightusd_next_render_texture_udim_remap_buffer']) {
      const bytes = querySize(symbol, [textureId]);
      if (!Number.isSafeInteger(bytes) || bytes < 0 || bytes % 4 !== 0) {
        throw new RangeError('texture payload: buffer size query failed');
      }
      total += bytes * 2;
      check('texture payload');
    }
    if (stream.textureIsUDIM(textureId) !== 0) {
      const udimId = stream.textureUDIMId(textureId);
      if (udimId >= 0) {
        const tileBytes = querySize('_lightusd_next_render_udim_tiles', [udimId]);
        if (!Number.isSafeInteger(tileBytes) || tileBytes < 0 || tileBytes % 16 !== 0 ||
            tileBytes > 1600) throw new RangeError('texture payload: invalid UDIM tile buffer');
        total += tileBytes * 2;
        for (let kind = 0; kind < 4; ++kind) {
          const bytes = querySize('_lightusd_next_render_udim_string', [udimId, kind]);
          if (!Number.isSafeInteger(bytes) || bytes < 0) {
            throw new RangeError('texture payload: UDIM string size query failed');
          }
          total += bytes * 2 + 64;
          check('texture payload');
        }
      }
      check('texture payload');
    }
    if (stream.textureLoaded(textureId) !== 0) {
      const imageId = stream.textureImageId(textureId);
      const bytes = Module['_lightusd_next_render_image_field'](state.handle, imageId, 7);
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('texture payload: invalid or oversized image payload');
      }
      total += bytes * 2;
      check('texture payload');
    }
    return total;
  };
  Object.defineProperty(Module.RenderStream.prototype, 'getTextureRecord', {value: function(textureId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof textureId !== 'number' || !Number.isInteger(textureId)) {
      throw new TypeError('getTextureRecord: expected one integer texture id');
    }
    if (textureId < 0 || textureId >= this.numTextures()) return {};
    texturePayloadEstimate(this, textureId);
    const imageId = this.textureImageId(textureId);
    const record = {
      id: textureId,
      name: nodeDecoder.decode(this.resourceNameBuffer(3, textureId)),
      uri: nodeDecoder.decode(this.resourcePathBuffer(3, textureId)),
      imageId,
      width: this.textureWidth(textureId),
      height: this.textureHeight(textureId),
      channels: this.textureChannels(textureId),
      mipLevels: this.textureMipLevels(textureId),
      loaded: this.textureLoaded(textureId) !== 0,
      wrapS: this.textureWrapS(textureId),
      wrapT: this.textureWrapT(textureId),
      outputChannel: this.textureOutputChannel(textureId),
      rotation: this.textureRotationScaled(textureId) / 1000000,
      uvPrimvar: this.textureString(textureId, 0),
      sourceColorSpace: this.textureString(textureId, 1),
      targetColorSpace: this.textureString(textureId, 2),
      sampling: Array.from(this.textureSamplingBuffer(textureId))
    };
    const legacyTransform = this.textureTransformBuffer(textureId);
    record.hasTransform2d = this.textureHasTransform2d(textureId) !== 0;
    record.isUDIM = this.textureIsUDIM(textureId) !== 0;
    record.udimTextureId = this.textureUDIMId(textureId);
    if (record.isUDIM) {
      const remap = this.textureUDIMRemapBuffer(textureId);
      record.udimUvScaleU = remap[0];
      record.udimUvScaleV = remap[1];
      record.udimUvOffsetU = remap[2];
      record.udimUvOffsetV = remap[3];
    }
    record.txRotation = legacyTransform[0];
    record.txScaleU = legacyTransform[1];
    record.txScaleV = legacyTransform[2];
    record.txTranslationU = legacyTransform[3];
    record.txTranslationV = legacyTransform[4];
    const colorTransform = this.textureColorTransformBuffer(textureId);
    record.colorTransformValid = colorTransform[0] !== 0;
    record.colorTransformBypass = colorTransform[1] !== 0;
    record.sourceColorIsData = colorTransform[2] !== 0;
    record.sourceGamma = colorTransform[3];
    record.sourceLinearBias = colorTransform[4];
    record.sourceToDisplayLinear = Array.from(colorTransform.subarray(5, 14));
    if (record.loaded) {
      const byteLength = Module['_lightusd_next_render_image_field'](
        state.handle, imageId, 7);
      if (byteLength < 0) throw new RangeError('getTextureRecord: invalid or oversized image payload');
      if (byteLength > 0x20000000) {
        throw new RangeError('getTextureRecord: image exceeds 512 MiB limit');
      }
      record.data = this.textureImageBuffer(textureId);
      if (record.data.byteLength !== byteLength) {
        throw new RangeError('getTextureRecord: image payload changed during copy');
      }
    }
    return record;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getTexture', {value: function(textureId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof textureId !== 'number' || !Number.isInteger(textureId)) {
      throw new TypeError('getTexture: expected one integer texture id');
    }
    if (textureId < 0 || textureId >= this.numTextures()) return {};
    const sampling = this.textureSamplingBuffer(textureId);
    const transform = this.textureTransformBuffer(textureId);
    const wraps = ['repeat', 'clamp_to_edge', 'mirror', 'clamp_to_border'];
    const wrapS = this.textureWrapS(textureId), wrapT = this.textureWrapT(textureId);
    const result = {
      textureImageId: this.textureImageId(textureId),
      wrapS: wraps[wrapS] ?? 'clamp_to_edge',
      wrapT: wraps[wrapT] ?? 'clamp_to_edge',
      hasTransform2d: this.textureHasTransform2d(textureId) !== 0,
      txRotation: transform[0], txScaleU: transform[1], txScaleV: transform[2],
      txTranslationU: transform[3], txTranslationV: transform[4],
      bias: Array.from(sampling.subarray(5, 9)),
      scale: Array.from(sampling.subarray(9, 13)),
      isUDIM: this.textureIsUDIM(textureId) !== 0
    };
    if (result.isUDIM) {
      result.udimTextureId = this.textureUDIMId(textureId);
      result.udimUvScaleU = 1; result.udimUvScaleV = 1;
      result.udimUvOffsetU = 0; result.udimUvOffsetV = 0;
    }
    return result;
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getAllTextures', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getAllTextures: wrong argument count');
    const count = this.numTextures();
    if (!Number.isSafeInteger(count) || count < 0 || count > 65536) {
      throw new RangeError('getAllTextures: invalid or excessive texture count');
    }
    let aggregateBytes = 0;
    for (let textureId = 0; textureId < count; ++textureId) {
      aggregateBytes += texturePayloadEstimate(this, textureId);
      if (aggregateBytes > 0x20000000) {
        throw new RangeError('getAllTextures: aggregate returned data exceeds 512 MiB limit');
      }
    }
    const remaining = Module['_lightusd_next_render_remaining_memory_bytes'](state.handle);
    if (!Number.isSafeInteger(remaining) || remaining < 0 || aggregateBytes > remaining) {
      throw new RangeError('getAllTextures: aggregate exceeds remaining memory limit');
    }
    return Array.from({length: count}, (_, textureId) => this.getTextureRecord(textureId));
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getUDIMTextureRecord', {value: function(udimId) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof udimId !== 'number' || !Number.isInteger(udimId)) {
      throw new TypeError('getUDIMTextureRecord: expected one integer UDIM id');
    }
    const count = this.udimTileCount(udimId);
    if (count < 0) return {};
    if (count > 100) throw new RangeError('getUDIMTextureRecord: excessive tile count');
    const querySize = (symbol, args) => {
      try { return Module[symbol](state.handle, ...args, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        return Module[symbol](state.handle, ...args, BigInt(0), 0);
      }
    };
    let estimatedBytes = querySize('_lightusd_next_render_udim_tiles', [udimId]);
    if (!Number.isSafeInteger(estimatedBytes) || estimatedBytes !== count * 16) {
      throw new RangeError('getUDIMTextureRecord: invalid tile payload size');
    }
    for (let kind = 0; kind < 4; ++kind) {
      const bytes = querySize('_lightusd_next_render_udim_string', [udimId, kind]);
      if (!Number.isSafeInteger(bytes) || bytes < 0) {
        throw new RangeError('getUDIMTextureRecord: invalid string size');
      }
      estimatedBytes += bytes * 2 + 64;
      if (estimatedBytes > 0x20000000) {
        throw new RangeError('getUDIMTextureRecord: payload exceeds 512 MiB limit');
      }
    }
    const strings = [0, 1, 2, 3].map(kind => this.udimString(udimId, kind));
    const tiles = this.udimTileBuffer(udimId);
    if (tiles.length !== count * 4) throw new RangeError('getUDIMTextureRecord: tile payload changed during copy');
    return {
      id: udimId,
      primName: strings[0],
      absPath: strings[1],
      displayName: strings[2],
      assetIdentifier: strings[3],
      tiles: Array.from({length: count}, (_, i) => ({
        udim: tiles[i * 4], u: tiles[i * 4 + 1],
        v: tiles[i * 4 + 2], imageId: tiles[i * 4 + 3]
      }))
    };
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getUDIMTexture', {value: function(udimId) {
    if (arguments.length !== 1) throw new TypeError('getUDIMTexture: expected one integer UDIM id');
    const record = this.getUDIMTextureRecord(udimId);
    if (!Object.keys(record).length) return {};
    return {
      primName: record.primName,
      absPath: record.absPath,
      displayName: record.displayName,
      assetIdentifier: record.assetIdentifier,
      tiles: record.tiles
    };
  }});
  const renderSceneFields = [
    ['sceneMetersPerUnitScaled', 0], ['sceneUpAxis', 1],
    ['sceneStartTimeScaled', 2], ['sceneEndTimeScaled', 3],
    ['sceneFramesPerSecondScaled', 4]
  ];
  for (const [name, field] of renderSceneFields) {
    Object.defineProperty(Module.RenderStream.prototype, name, {value: function() {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 4) {
        throw new TypeError('Invalid LightUSD receiver');
      }
      if (arguments.length !== 0) {
        throw new TypeError(name + ': wrong argument count');
      }
      ++state.busy;
      try {
        return Module['_lightusd_next_render_scene_field'](state.handle, field);
      } finally {
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.RenderStream.prototype, 'getUpAxis', {value: function() {
    if (arguments.length !== 0) throw new TypeError('getUpAxis: wrong argument count');
    return this.sceneUpAxisName();
  }});
  Object.defineProperty(Module.RenderStream.prototype, 'getURI', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 4) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 0) throw new TypeError('getURI: wrong argument count');
    return state.sourceURI;
  }});

}
