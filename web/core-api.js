// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Internal post-JS bridge for the counted C dispatch. Object IDs are
// uint32 on both wasm32 and memory64; no native pointer is exposed to JS.
{
  const live = new WeakMap();
  function defineClass(name, kind) {
    const Native = class {
      constructor() {
        // The module that owns this object's heap. The core module delegates
        // its render API to the Tydra feature, so buffer descriptors returned
        // by a stream must be read through that module's HEAPU8.
        Object.defineProperty(this, '__lightusdModule', {value: Module});
        const handle = Module['_lightusd_wasm_create'](kind) >>> 0;
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
        Module['_lightusd_wasm_destroy'](state.handle);
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
      input = Module['_lightusd_wasm_alloc'](total);
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
      const call = p => Module['_lightusd_wasm_diff_json'](
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
      if (output) Module['_lightusd_wasm_free'](output);
      if (input) Module['_lightusd_wasm_free'](input);
    }
  };
  const optionalFeatures = new Map();
  const featureNames = new Set(['geometry', 'raytrace', 'validator', 'exr', 'tydra']);
  Module['loadFeature'] = name => {
    if (!featureNames.has(name)) return Promise.reject(new TypeError(`Unknown LightUSD feature: ${name}`));
    if (!optionalFeatures.has(name)) {
      const pending = (async () => {
        const expected = Module['featureInfo'];
        const suffix = expected.pointerBits === 64 ? '_64' : '';
        const file = `lightusd_${name}${suffix}.js`;
        const url = Module['locateFeature'] ? String(Module['locateFeature'](file))
          : new URL(file, import.meta.url).href;
        const factory = (await import(/* @vite-ignore */ url)).default;
        const feature = await factory({locateFile: file => Module['locateFeature']
          ? String(Module['locateFeature'](file)) : new URL(file, url).href});
        const actual = feature['featureInfo'];
        for (const key of Object.keys(expected)) {
          if (!actual || actual[key] !== expected[key])
            throw new Error(`Incompatible LightUSD ${name} feature: ${key}`);
        }
        if (feature.HEAPU8.buffer === Module.HEAPU8.buffer)
          throw new Error('LightUSD features require independent heaps');
        if (name === 'exr') installEXR(feature);
        if (name === 'validator') installValidator(feature);
        return feature;
      })();
      optionalFeatures.set(name, pending);
      pending.catch(() => { if (optionalFeatures.get(name) === pending) optionalFeatures.delete(name); });
    }
    return optionalFeatures.get(name);
  };
  let exr, validator;
  const installEXR = value => {
    exr = value;
    Module['exrBridge'] = (op, ptr, size, width, height, channels, bits, budget, out, pointerBytes) => {
      let allocated = 0;
      try {
        if (ptr > Module.HEAPU8.length - size || size < 0) return -1;
        const result = exr.codec(op, Module.HEAPU8.subarray(ptr, ptr + size), width, height,
          channels, bits, Math.min(budget, 0x20000000));
        if (result.data.length) {
          allocated = Module['_lightusd_wasm_alloc'](result.data.length);
          if (!allocated) return -4;
          Module.HEAPU8.set(result.data, Number(allocated));
        }
        const view = new DataView(Module.HEAPU8.buffer);
        if (pointerBytes === 8) {
          view.setBigUint64(out, BigInt(allocated), true);
          view.setBigUint64(out + 8, BigInt(result.data.length), true);
        } else {
          view.setUint32(out, Number(allocated), true);
          view.setUint32(out + 4, result.data.length, true);
        }
        for (const [i, v] of [result.width, result.height, result.channels, result.bits].entries())
          view.setUint32(out + pointerBytes * 2 + i * 4, v, true);
        return 0;
      } catch (error) {
        if (allocated) Module['_lightusd_wasm_free'](allocated);
        return -7;
      }
    };
  };
  const requireValidator = () => {
    if (!validator) throw new Error("Validation feature not loaded; await module.loadFeature('validator') first");
    return validator;
  };
  const installValidator = feature => {
    validator = feature;
    feature['checkUSD'] = (bytes, filename, options = {}, assetStore = null) => {
    if (!options || typeof options !== 'object' || Array.isArray(options))
      throw new TypeError('checkUSD: options must be an object');
    if (!ArrayBuffer.isView(bytes)) throw new TypeError('checkUSD: expected byte view');
    const store = assetStore === null ? null : live.get(assetStore);
    // The render API (and its AssetStore) lives in the Tydra feature's heap.
    // Accept it by its public JS surface instead of this module's handle table.
    const foreignStore = assetStore !== null && !store &&
      typeof assetStore?.assetIdentifiers === 'function' &&
      typeof assetStore?.readMemoryAsset === 'function';
    if (assetStore !== null && !store && !foreignStore)
      throw new TypeError('checkUSD: assetStore must be a live AssetStore');
    // Asset queries may grow the core heap; snapshot a heap-backed root first.
    if (bytes.buffer === Module.HEAPU8.buffer)
      bytes = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength).slice();
    let snapshot = null;
    if (store) {
      const ptr = Module['_lightusd_wasm_asset_store_snapshot'](store.handle);
      if (!ptr) throw new RangeError('checkUSD: asset metadata exceeds limits');
      let metadata;
      try { metadata = JSON.parse(UTF8ToString(Number(ptr))); }
      finally { Module['_lightusd_wasm_free'](ptr); }
      snapshot = {metadata, read: name => assetStore.readMemoryAsset(name)};
    } else if (foreignStore) {
      const assets = assetStore.assetIdentifiers().map(name =>
        ({name, size: assetStore.readMemoryAsset(name).length}));
      snapshot = {metadata: {assets}, read: name => assetStore.readMemoryAsset(name)};
    }
    return JSON.parse(feature.runValidation(bytes, filename, JSON.stringify(options), true, snapshot));
  };
  };
  defineClass('USDZConverterNative', 1);
  defineClass('AssetStore', 5);
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
    const size = Module['_lightusd_wasm_layer_error_size'](state.handle);
    if (size <= 0) return '';
    let ptr = 0;
    try {
      ptr = Module['_lightusd_wasm_alloc'](size);
      if (!ptr) throw new RangeError('LayerDocument: error allocation failed');
      const copied = layerCall('_lightusd_wasm_layer_error_copy',
        [state.handle, ptr, size], [1]);
      if (copied !== size) throw new RangeError('LayerDocument: error copy failed');
      return layerDecoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally { if (ptr) Module['_lightusd_wasm_free'](ptr); }
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(total, 1));
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
      if (ptr) Module['_lightusd_wasm_free'](ptr);
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(input.length, 1));
      if (!ptr) throw new RangeError('load: allocation failed');
      if (input.heapOffset >= 0) {
        Module.HEAPU8.copyWithin(Number(ptr), input.heapOffset,
          input.heapOffset + input.length);
      } else {
        Module.HEAPU8.set(input.bytes, Number(ptr));
      }
      const status = layerCall('_lightusd_wasm_layer_load',
        [state.handle, ptr, input.length], [1]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true, primCount: Module['_lightusd_wasm_layer_prim_count'](state.handle)};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
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
      ptr = Module['_lightusd_wasm_alloc'](input.length);
      if (!ptr) throw new RangeError('loadJSON: allocation failed');
      Module.HEAPU8.set(input, Number(ptr));
      const status = layerCall('_lightusd_wasm_layer_load_json',
        [state.handle, ptr, input.length], [1]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true, primCount: Module['_lightusd_wasm_layer_prim_count'](state.handle)};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
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
    const previous = Module.__lightusdLayerProgressCallback;
    let callbackError = null;
    Module.__lightusdLayerProgressCallback = event => {
      try { return callback(event) !== false; }
      catch (error) { callbackError = error; return false; }
    };
    try {
      ptr = Module['_lightusd_wasm_alloc'](Math.max(input.length, 1));
      if (!ptr) throw new RangeError('loadWithProgress: allocation failed');
      if (input.heapOffset >= 0) {
        Module.HEAPU8.copyWithin(Number(ptr), input.heapOffset,
          input.heapOffset + input.length);
      } else {
        Module.HEAPU8.set(input.bytes, Number(ptr));
      }
      const status = layerCall('_lightusd_wasm_layer_load_with_progress',
        [state.handle, ptr, input.length], [1]);
      if (callbackError) throw callbackError;
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true, primCount: Module['_lightusd_wasm_layer_prim_count'](state.handle)};
    } finally {
      Module.__lightusdLayerProgressCallback = previous;
      if (ptr) Module['_lightusd_wasm_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getMhProfileJSON', {value: function() {
    const state = layerState(this);
    if (arguments.length !== 0) throw new TypeError('getMhProfileJSON: wrong argument count');
    ++state.busy;
    let ptr = 0;
    try {
      const size = Module['_lightusd_wasm_layer_mh_profile_json_size'](state.handle);
      if (size < 0) throw new RangeError(layerError(state) || 'getMhProfileJSON: query failed');
      if (size > 0x20000000) throw new RangeError('getMhProfileJSON: output exceeds 512 MiB');
      ptr = Module['_lightusd_wasm_alloc'](Math.max(size, 1));
      if (!ptr) throw new RangeError('getMhProfileJSON: allocation failed');
      let copied;
      try { copied = Module['_lightusd_wasm_layer_mh_profile_json_copy'](state.handle, ptr, size); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        copied = Module['_lightusd_wasm_layer_mh_profile_json_copy'](state.handle, BigInt(ptr), size);
      }
      if (copied !== size) throw new RangeError('getMhProfileJSON: result changed during copy');
      return layerDecoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'getShadingGraphJSON', {value: function() {
    const state = layerState(this);
    if (arguments.length !== 0) throw new TypeError('getShadingGraphJSON: wrong argument count');
    ++state.busy;
    let ptr = 0;
    try {
      const size = Module['_lightusd_wasm_layer_shading_graph_json_size'](state.handle);
      if (size < 0) throw new RangeError(layerError(state) || 'getShadingGraphJSON: query failed');
      if (size === 0) return '';
      if (size > 0x20000000) throw new RangeError('getShadingGraphJSON: output exceeds 512 MiB');
      ptr = Module['_lightusd_wasm_alloc'](size);
      if (!ptr) throw new RangeError('getShadingGraphJSON: allocation failed');
      let copied;
      try { copied = Module['_lightusd_wasm_layer_shading_graph_json_copy'](state.handle, ptr, size); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        copied = Module['_lightusd_wasm_layer_shading_graph_json_copy'](state.handle, BigInt(ptr), size);
      }
      if (copied !== size) throw new RangeError('getShadingGraphJSON: result changed during copy');
      return layerDecoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'definePrim', {value: function(path, typeName = '') {
    if (arguments.length < 1 || arguments.length > 2) throw new TypeError('definePrim: expected path and optional type');
    return layerStringsCall(this, '_lightusd_wasm_layer_define_prim',
      [path, typeName]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'removePrim', {value: function(path) {
    if (arguments.length !== 1) throw new TypeError('removePrim: expected path');
    return layerStringsCall(this, '_lightusd_wasm_layer_remove_prim', [path]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'removeAttribute', {value: function(path, name) {
    if (arguments.length !== 2) throw new TypeError('removeAttribute: expected path and name');
    return layerStringsCall(this, '_lightusd_wasm_layer_remove_attribute', [path, name]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'setStringAttribute', {value: function(path, name, value) {
    if (arguments.length !== 3) throw new TypeError('setStringAttribute: expected path, name and value');
    return layerStringsCall(this, '_lightusd_wasm_layer_set_string', [path, name, value]);
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'setNumberAttribute', {value: function(path, name, value) {
    if (arguments.length !== 3 || typeof value !== 'number' || !Number.isFinite(value)) {
      throw new TypeError('setNumberAttribute: expected path, name and finite number');
    }
    return layerStringsCall(this, '_lightusd_wasm_layer_set_number', [path, name], [value]);
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('setAttributeMetadata: allocation failed');
      const base = Number(ptr);
      Module.HEAPU8.set(pathBytes, base);
      Module.HEAPU8.set(nameBytes, base + pathBytes.length);
      Module.HEAPU8.set(keyBytes, base + pathBytes.length + nameBytes.length);
      Module.HEAPU8.set(payload, base + pathBytes.length + nameBytes.length + keyBytes.length);
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const status = layerCall('_lightusd_wasm_layer_set_attribute_metadata',
        [state.handle, pointer(0), pathBytes.length,
          pointer(pathBytes.length), nameBytes.length,
          pointer(pathBytes.length + nameBytes.length), keyBytes.length, kind,
          pointer(pathBytes.length + nameBytes.length + keyBytes.length),
          payload.length, number, integer], [1, 3, 5, 8]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
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
      input = Module['_lightusd_wasm_alloc'](Math.max(inputSize, 1));
      info = Module['_lightusd_wasm_alloc'](1);
      if (!input || !info) throw new RangeError('getAttributeMetadata: allocation failed');
      Module.HEAPU8.set(pathBytes, Number(input));
      Module.HEAPU8.set(nameBytes, Number(input) + pathBytes.length);
      Module.HEAPU8.set(keyBytes, Number(input) + pathBytes.length + nameBytes.length);
      const pointer = (ptr, offset = 0) => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const call = (out, cap) => layerCall('_lightusd_wasm_layer_get_attribute_metadata',
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
        output = Module['_lightusd_wasm_alloc'](Math.max(required, 1));
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
      } finally { if (output) Module['_lightusd_wasm_free'](output); }
    } finally {
      if (info) Module['_lightusd_wasm_free'](info);
      if (input) Module['_lightusd_wasm_free'](input);
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(keyBytes.length + textBytes.length, 1));
      if (!ptr) throw new RangeError('setStageMetadata: allocation failed');
      Module.HEAPU8.set(keyBytes, Number(ptr));
      Module.HEAPU8.set(textBytes, Number(ptr) + keyBytes.length);
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const args = [state.handle, pointer(0), keyBytes.length, kind,
        pointer(keyBytes.length), textBytes.length, numeric ? value : 0];
      const status = layerCall('_lightusd_wasm_layer_set_stage_metadata', args, [1, 4]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('setPrimMetadata: allocation failed');
      const base = Number(ptr);
      Module.HEAPU8.set(pathBytes, base);
      Module.HEAPU8.set(keyBytes, base + pathBytes.length);
      Module.HEAPU8.set(payload, base + pathBytes.length + keyBytes.length);
      const pointer = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const args = [state.handle, pointer(0), pathBytes.length,
        pointer(pathBytes.length), keyBytes.length, kind,
        pointer(pathBytes.length + keyBytes.length), payload.length, count];
      const status = layerCall('_lightusd_wasm_layer_set_prim_metadata',
        args, [1, 3, 6]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
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
      input = Module['_lightusd_wasm_alloc'](Math.max(pathBytes.length + keyBytes.length, 1));
      info = Module['_lightusd_wasm_alloc'](8);
      if (!input || !info) throw new RangeError('getPrimMetadata: allocation failed');
      Module.HEAPU8.set(pathBytes, Number(input));
      Module.HEAPU8.set(keyBytes, Number(input) + pathBytes.length);
      const pointer = (ptr, offset = 0) => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const call = (out, cap) => layerCall('_lightusd_wasm_layer_get_prim_metadata',
        [state.handle, input, pathBytes.length, pointer(input, pathBytes.length),
          keyBytes.length, info, pointer(info, 4), out, cap], [1, 3, 5, 6, 7]);
      const authored = layerCall('_lightusd_wasm_layer_prim_metadata_is_authored',
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
        output = Module['_lightusd_wasm_alloc'](Math.max(required, 1));
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
      } finally { if (output) Module['_lightusd_wasm_free'](output); }
    } finally {
      if (info) Module['_lightusd_wasm_free'](info);
      if (input) Module['_lightusd_wasm_free'](input);
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
      input = Module['_lightusd_wasm_alloc'](Math.max(keyBytes.length, 1));
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
      const authored = call('_lightusd_wasm_layer_stage_metadata_is_authored',
        [state.handle, keyPtr, keyBytes.length], [1]);
      if (authored < 0) return {success: false, error: layerError(state)};
      const kind = stageMetadataKinds.get(key);
      if (kind === 3) {
        let output = 0;
        try {
          output = Module['_lightusd_wasm_alloc'](8);
          if (!output) throw new RangeError('getStageMetadata: result allocation failed');
          const status = call('_lightusd_wasm_layer_get_stage_metadata_number',
            [state.handle, keyPtr, keyBytes.length, output], [1, 3]);
          if (status !== 0) return {success: false, error: layerError(state)};
          return {success: true, authored: authored !== 0,
            value: new DataView(Module.HEAPU8.buffer,
              Number(output), 8).getFloat64(0, true)};
        } finally { if (output) Module['_lightusd_wasm_free'](output); }
      }
      const query = call('_lightusd_wasm_layer_get_stage_metadata_string',
        [state.handle, keyPtr, keyBytes.length, 0, 0], [1, 3]);
      if (query < 0) return {success: false, error: layerError(state)};
      if (query > 0x20000000) throw new RangeError('getStageMetadata: value exceeds 512 MiB');
      let output = 0;
      try {
        output = Module['_lightusd_wasm_alloc'](Math.max(query, 1));
        if (!output) throw new RangeError('getStageMetadata: result allocation failed');
        const copied = call('_lightusd_wasm_layer_get_stage_metadata_string',
          [state.handle, keyPtr, keyBytes.length, output, query], [1, 3]);
        if (copied !== query) throw new RangeError('getStageMetadata: value changed during copy');
        return {success: true, authored: authored !== 0, value: layerDecoder.decode(
          Module.HEAPU8.subarray(Number(output), Number(output) + query))};
      } finally { if (output) Module['_lightusd_wasm_free'](output); }
    } finally {
      if (input) Module['_lightusd_wasm_free'](input);
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(total, 1));
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
      const status = layerCall('_lightusd_wasm_layer_set_relationship_targets',
        args, [1, 3, 5]);
      if (status !== 0) return {success: false, error: layerError(state)};
      return {success: true};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
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
      input = Module['_lightusd_wasm_alloc'](Math.max(pathBytes.length + nameBytes.length, 1));
      if (!input) throw new RangeError('getRelationshipTargets: allocation failed');
      const base = Number(input);
      Module.HEAPU8.set(pathBytes, base);
      Module.HEAPU8.set(nameBytes, base + pathBytes.length);
      const pointer = offset => typeof input === 'bigint' ? input + BigInt(offset) : input + offset;
      const args = [state.handle, pointer(0), pathBytes.length,
        pointer(pathBytes.length), nameBytes.length];
      let count;
      try { count = layerCall('_lightusd_wasm_layer_relationship_target_count', args, [1, 3]); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        count = layerCall('_lightusd_wasm_layer_relationship_target_count',
          args.map((value, index) => index === 1 || index === 3 ? BigInt(value) : value));
      }
      if (count < 0) return {success: false, error: layerError(state)};
      let total = 0;
      const targets = [];
      for (let index = 0; index < count; ++index) {
        const queryArgs = [...args, index, 0, 0];
        let size;
        try { size = layerCall('_lightusd_wasm_layer_relationship_target_copy', queryArgs, [1, 3, 6]); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          size = layerCall('_lightusd_wasm_layer_relationship_target_copy',
            queryArgs.map((value, i) => i === 1 || i === 3 || i === 6 ? BigInt(value) : value));
        }
        if (size < 0 || total > 0x20000000 - size) {
          throw new RangeError('getRelationshipTargets: invalid or oversized target payload');
        }
        total += size;
        let output = 0;
        try {
          output = Module['_lightusd_wasm_alloc'](Math.max(size, 1));
          if (!output) throw new RangeError('getRelationshipTargets: target allocation failed');
          const copyArgs = [...args, index, output, size];
          let copied;
          try { copied = layerCall('_lightusd_wasm_layer_relationship_target_copy', copyArgs, [1, 3, 6]); }
          catch (error) {
            if (!(error instanceof TypeError)) throw error;
            copied = layerCall('_lightusd_wasm_layer_relationship_target_copy',
              copyArgs.map((value, i) => i === 1 || i === 3 || i === 6 ? BigInt(value) : value));
          }
          if (copied !== size) throw new RangeError('getRelationshipTargets: target changed');
          targets.push(layerDecoder.decode(Module.HEAPU8.subarray(Number(output), Number(output) + size)));
        } finally { if (output) Module['_lightusd_wasm_free'](output); }
      }
      return {success: true, targets};
    } finally {
      if (input) Module['_lightusd_wasm_free'](input);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'removeRelationship', {value: function(path, name) {
    if (arguments.length !== 2) throw new TypeError('removeRelationship: expected path and name');
    return layerStringsCall(this, '_lightusd_wasm_layer_remove_relationship', [path, name]);
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(total, 1));
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
      const status = layerCall('_lightusd_wasm_layer_set_typed', args, [1, 3, 5, 8]);
      return status === 0 ? {success: true} : {success: false, error: layerError(state)};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'primCount', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('primCount: wrong argument count');
    const count = Module['_lightusd_wasm_layer_prim_count'](state.handle);
    if (count < 0) throw new RangeError('primCount: invalid receiver');
    return count;
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'loaded', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('loaded: wrong argument count');
    return Module['_lightusd_wasm_layer_loaded'](state.handle) === 1;
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
      const size = Module['_lightusd_wasm_layer_export_usda_size'](state.handle);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'USDA output exceeds 512 MiB limit'};
      const data = Module['_lightusd_wasm_layer_export_usda_data'](state.handle);
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
      const size = Module['_lightusd_wasm_layer_export_json_size'](state.handle);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'Layer JSON output exceeds 512 MiB limit'};
      const data = Module['_lightusd_wasm_layer_export_json_data'](state.handle);
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
    if (Module['_lightusd_wasm_layer_loaded'](state.handle) !== 1) {
      return JSON.stringify({parse_ok: false, ok: false,
        error: 'No Layer is loaded. Use loadAsLayerFromBinary first.'});
    }
    const feature = requireValidator();
    const size = Module['_lightusd_wasm_layer_export_usda_size'](state.handle);
    if (size < 0) return JSON.stringify({parse_ok: false, ok: false,
      error: layerError(state) || 'Layer export failed'});
    if (!Number.isSafeInteger(size) || size > 0x20000000)
      throw new RangeError('validateLoadedLayer: USDA exceeds 512 MiB limit');
    const ptr = Module['_lightusd_wasm_layer_export_usda_data'](state.handle);
    if (!ptr && size) throw new RangeError('validateLoadedLayer: retained USDA is missing');
    ++state.busy;
    try { return feature.validateFromBinary(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size),
      '<layer-document>.usda', options); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportUSDC', {value: function() {
    const state = layerState(this);
    if (arguments.length) throw new TypeError('exportUSDC: wrong argument count');
    ++state.busy;
    try {
      const size = Module['_lightusd_wasm_layer_export_usdc_size'](state.handle);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'USDC output exceeds 512 MiB limit'};
      const data = Module['_lightusd_wasm_layer_export_usdc_data'](state.handle);
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
    if (arguments.length > 2) throw new TypeError('exportUSDZ: expected options and optional AssetStore');
    if (options === null) options = {};
    if (typeof options !== 'object' || ArrayBuffer.isView(options)) {
      throw new TypeError('exportUSDZ: options must be an object');
    }
    const root = options.rootLayerFormat === undefined ? 'usda' : options.rootLayerFormat;
    if (root !== 'usda' && root !== 'usdc') {
      throw new TypeError('exportUSDZ: rootLayerFormat must be usda or usdc');
    }
    if (Module['_lightusd_wasm_layer_loaded'](state.handle) !== 1) {
      return {success: false, error: layerError(state) || 'No layer loaded'};
    }
    let assetState = null;
    if (assetStore !== null) {
      assetState = live.get(assetStore);
      if (!assetState?.handle || assetState.kind !== 5) {
        throw new TypeError('exportUSDZ: assetStore must be a live AssetStore');
      }
    }
    ++state.busy;
    if (assetState) ++assetState.busy;
    let ptr = 0;
    try {
      const size = Module['_lightusd_wasm_layer_export_usdz'](
        state.handle, assetState ? assetState.handle : 0, root === 'usdc' ? 1 : 0);
      if (size < 0) return {success: false, error: layerError(state)};
      if (size > 0x20000000) return {success: false, error: 'USDZ output exceeds 512 MiB limit'};
      const output = new Uint8Array(size);
      if (size) {
        ptr = Module['_lightusd_wasm_alloc'](size);
        if (!ptr) throw new RangeError('exportUSDZ: output allocation failed');
        const copied = layerCall('_lightusd_wasm_layer_export_usdz_copy',
          [state.handle, ptr, size], [1]);
        if (copied !== size) throw new RangeError('exportUSDZ: retained output changed');
        output.set(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
      }
      return {success: true, data: output};
    } finally {
      if (ptr) Module['_lightusd_wasm_free'](ptr);
      if (assetState) --assetState.busy;
      --state.busy;
    }
  }});
  Object.defineProperty(Module.LayerDocument.prototype, 'exportUSDCToBuffer', {value: function(buffer, options) {
    const state = layerState(this);
    if (arguments.length !== 2) throw new TypeError('exportUSDCToBuffer: expected buffer and options');
    // Preserve legacy validation order: loaded layer, output kind, then capacity.
    if (Module['_lightusd_wasm_layer_loaded'](state.handle) !== 1) {
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
    Module['_lightusd_wasm_layer_end'](state.handle);
  }});

  const assetStoreEncoder = new TextEncoder();
  const assetStoreDecoder = new TextDecoder();
  const assetStoreStringResult = (state, symbol, value) => {
    const input = assetStoreEncoder.encode(value);
    let inputPtr = 0, outputPtr = 0;
    try {
      inputPtr = Module['_lightusd_wasm_alloc'](Math.max(input.length, 1));
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
      outputPtr = Module['_lightusd_wasm_alloc'](size);
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
      if (outputPtr) Module['_lightusd_wasm_free'](outputPtr);
      if (inputPtr) Module['_lightusd_wasm_free'](inputPtr);
    }
  };
  const kMaxAssetStoreAggregateBytes = 0x20000000;
  const assetIdentifierSizePlan = state => {
    const count = Module['_lightusd_wasm_asset_store_identifier_count'](state.handle);
    if (!Number.isSafeInteger(count) || count < 0 || count > 65536) {
      throw new RangeError('assetIdentifiers: invalid or excessive identifier count');
    }
    let estimate = count * 128;
    const sizes = [];
    for (let index = 0; index < count; ++index) {
      let size;
      try { size = Module['_lightusd_wasm_asset_store_identifier_copy'](state.handle, index, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        size = Module['_lightusd_wasm_asset_store_identifier_copy'](state.handle, index, 0n, 0);
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
      inputPtr = Module['_lightusd_wasm_alloc'](Math.max(input.length, 1));
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
    } finally { if (inputPtr) Module['_lightusd_wasm_free'](inputPtr); }
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
    const ptr = Module['_lightusd_wasm_alloc'](size);
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
    } finally { Module['_lightusd_wasm_free'](ptr); }
  };
  const assetStoreSetPath = (state, symbol, path, label) => {
    if (typeof path !== 'string') throw new TypeError(label + ': expected a path string');
    const bytes = assetStoreEncoder.encode(path);
    const ptr = Module['_lightusd_wasm_alloc'](Math.max(bytes.length, 1));
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
    } finally { Module['_lightusd_wasm_free'](ptr); }
  };
  const assetStoreBytes = (value, label) => {
    if (!ArrayBuffer.isView(value)) throw new TypeError(label + ': expected byte view');
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  };
  Object.defineProperty(Module.AssetStore.prototype, 'registerMemoryAsset', {value: function(identifier, bytes) {
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
      ptr = Module['_lightusd_wasm_alloc'](Math.max(total, 1));
      if (!ptr) throw new RangeError('registerMemoryAsset: allocation failed');
      const base = Number(ptr);
      const payloadOffset = base + idBytes.length;
      const outputOffset = payloadOffset + payload.length;
      Module.HEAPU8.set(idBytes, base);
      Module.HEAPU8.set(payload, payloadOffset);
      const p = offset => typeof ptr === 'bigint' ? ptr + BigInt(offset) : ptr + offset;
      const call = (handle, idPtr, dataPtr, outPtr) => Module['_lightusd_wasm_asset_store_register'](
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
    } finally { if (ptr) Module['_lightusd_wasm_free'](ptr); --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setAssetFromRawPointer', {value: function(identifier, pointer, byteLength) {
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
      keyPtr = Module['_lightusd_wasm_alloc'](Math.max(id.length, 1));
      if (!keyPtr) throw new RangeError('setAssetFromRawPointer: allocation failed');
      if (id.length) Module.HEAPU8.set(id, Number(keyPtr));
      const rawPtr = typeof keyPtr === 'bigint' ? BigInt(offset) : offset;
      const invoke = (handle, key) => Module['_lightusd_wasm_asset_store_set_raw'](
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
    } finally { if (keyPtr) Module['_lightusd_wasm_free'](keyPtr); --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'unregisterMemoryAsset', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier) {
      throw new TypeError('unregisterMemoryAsset: expected a non-empty identifier');
    }
    const bytes = assetStoreEncoder.encode(identifier);
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_wasm_alloc'](bytes.length);
      if (!ptr) throw new RangeError('unregisterMemoryAsset: allocation failed');
      Module.HEAPU8.set(bytes, Number(ptr));
      const invoke = handle => Module['_lightusd_wasm_asset_store_unregister'](handle, ptr, bytes.length);
      let status;
      try { status = invoke(state.handle); }
      catch (error) { if (!(error instanceof TypeError)) throw error; status = Module['_lightusd_wasm_asset_store_unregister'](state.handle, BigInt(ptr), bytes.length); }
      if (status < 0) throw new RangeError('unregisterMemoryAsset: operation failed');
      return status === 1;
    } finally { if (ptr) Module['_lightusd_wasm_free'](ptr); --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'readMemoryAsset', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier) {
      throw new TypeError('readMemoryAsset: expected a non-empty identifier');
    }
    const name = assetStoreEncoder.encode(identifier);
    ++state.busy;
    let namePtr = 0, dataPtr = 0;
    try {
      namePtr = Module['_lightusd_wasm_alloc'](name.length);
      if (!namePtr) throw new RangeError('readMemoryAsset: allocation failed');
      Module.HEAPU8.set(name, Number(namePtr));
      const call = (out, cap) => Module['_lightusd_wasm_asset_store_read'](
        state.handle, namePtr, name.length, out, cap);
      let size;
      try { size = call(0, 0); }
      catch (error) { if (!(error instanceof TypeError)) throw error;
        size = Module['_lightusd_wasm_asset_store_read'](state.handle, BigInt(namePtr), name.length, 0n, 0); }
      if (size === -2) return null;
      if (size < 0) throw new RangeError('readMemoryAsset: read failed');
      if (size === 0) return new Uint8Array(0);
      dataPtr = Module['_lightusd_wasm_alloc'](size);
      if (!dataPtr) throw new RangeError('readMemoryAsset: allocation failed');
      try { if (call(dataPtr, size) !== size) throw new RangeError('readMemoryAsset: read changed'); }
      catch (error) { if (!(error instanceof TypeError)) throw error;
        if (Module['_lightusd_wasm_asset_store_read'](state.handle, BigInt(namePtr), name.length, BigInt(dataPtr), size) !== size) throw new RangeError('readMemoryAsset: read changed'); }
      return new Uint8Array(Module.HEAPU8.buffer, Number(dataPtr), size).slice();
    } finally {
      if (dataPtr) Module['_lightusd_wasm_free'](dataPtr);
      if (namePtr) Module['_lightusd_wasm_free'](namePtr);
      --state.busy;
    }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetCacheDataAsMemoryView', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string')
      throw new TypeError('getAssetCacheDataAsMemoryView: expected an identifier');
    if (!identifier) return undefined;
    const name = assetStoreEncoder.encode(identifier);
    let namePtr = 0, infoPtr = 0;
    try {
      namePtr = Module['_lightusd_wasm_alloc'](name.length);
      infoPtr = Module['_lightusd_wasm_alloc'](16);
      if (!namePtr || !infoPtr) throw new RangeError('getAssetCacheDataAsMemoryView: allocation failed');
      Module.HEAPU8.set(name, Number(namePtr));
      const infoOffset = Number(infoPtr);
      Module.HEAPU8.fill(0, infoOffset, infoOffset + 16);
      const call = (idPointer, infoPointer) => Module['_lightusd_wasm_asset_store_view'](
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
      if (infoPtr) Module['_lightusd_wasm_free'](infoPtr);
      if (namePtr) Module['_lightusd_wasm_free'](namePtr);
    }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAsset', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string')
      throw new TypeError('getAsset: expected an identifier');
    const data = identifier ? this.readMemoryAsset(identifier) : null;
    if (data === null) return {};
    return {name: identifier, data, sha256: this.getAssetHash(identifier),
      uuid: this.getAssetUUID(identifier)};
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setAlias', {value: function(authored, resolved) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof authored !== 'string' || !authored ||
        typeof resolved !== 'string' || !resolved) throw new TypeError('setAlias: expected two non-empty identifiers');
    const a = assetStoreEncoder.encode(authored), r = assetStoreEncoder.encode(resolved);
    const total = a.length + r.length;
    ++state.busy;
    let ptr = 0;
    try {
      ptr = Module['_lightusd_wasm_alloc'](total);
      if (!ptr) throw new RangeError('setAlias: allocation failed');
      const base = Number(ptr); Module.HEAPU8.set(a, base); Module.HEAPU8.set(r, base + a.length);
      const invoke = (handle, p) => Module['_lightusd_wasm_asset_store_set_alias'](
        handle, p, a.length, typeof p === 'bigint' ? p + BigInt(a.length) : p + a.length, r.length);
      let status;
      try { status = invoke(state.handle, ptr); }
      catch (error) { if (!(error instanceof TypeError)) throw error; status = invoke(state.handle, BigInt(ptr)); }
      if (status === -2) throw new RangeError('setAlias: metadata limit exceeded');
      if (status !== 0) throw new RangeError('setAlias: alias rejected');
    } finally { if (ptr) Module['_lightusd_wasm_free'](ptr); --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'assetIdentifiers', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('assetIdentifiers: wrong argument count');
    const sizes = assetIdentifierSizePlan(state);
    return sizes.map((size, index) => assetStoreOutputString(state,
      '_lightusd_wasm_asset_store_identifier_copy', [index], 'assetIdentifiers'));
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetUUID', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier)
      throw new TypeError('getAssetUUID: expected a non-empty identifier');
    return assetStoreStringResult(state, '_lightusd_wasm_asset_store_uuid', identifier);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getStreamingAssetUUID', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier)
      throw new TypeError('getStreamingAssetUUID: expected a non-empty asset identifier');
    return assetStoreStringResult(state, '_lightusd_wasm_asset_store_streaming_uuid', identifier);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'findAssetByUUID', {value: function(uuid) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof uuid !== 'string' || !uuid)
      throw new TypeError('findAssetByUUID: expected a non-empty UUID');
    return assetStoreStringResult(state, '_lightusd_wasm_asset_store_find_uuid', uuid);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetByUUID', {value: function(uuid) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof uuid !== 'string')
      throw new TypeError('getAssetByUUID: expected a UUID');
    const identifier = uuid ? assetStoreStringResult(state,
      '_lightusd_wasm_asset_store_find_uuid', uuid) : '';
    if (!identifier) return {error: 'Asset not found with UUID: ' + uuid};
    return this.getAsset(identifier);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetHash', {value: function(identifier) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof identifier !== 'string' || !identifier)
      throw new TypeError('getAssetHash: expected a non-empty identifier');
    return assetStoreStringResult(state, '_lightusd_wasm_asset_store_hash', identifier);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'verifyAssetHash', {value: function(identifier, hash) {
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
      ptr = Module['_lightusd_wasm_alloc'](total);
      if (!ptr) throw new RangeError('verifyAssetHash: allocation failed');
      const base = Number(ptr), hashOffset = base + idBytes.length;
      Module.HEAPU8.set(idBytes, base);
      Module.HEAPU8.set(hashBytes, hashOffset);
      const invoke = (basePtr, hashPtr) => Module['_lightusd_wasm_asset_store_verify_hash'](
        state.handle, basePtr, idBytes.length, hashPtr, hashBytes.length);
      let status;
      try { status = invoke(ptr, typeof ptr === 'bigint' ? ptr + BigInt(idBytes.length) : ptr + idBytes.length); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = invoke(BigInt(ptr), BigInt(ptr) + BigInt(idBytes.length));
      }
      if (status < 0) throw new RangeError('verifyAssetHash: operation failed');
      return status === 1;
    } finally { if (ptr) Module['_lightusd_wasm_free'](ptr); }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAllAssetUUIDs', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAllAssetUUIDs: wrong argument count');
    const identifiers = this.assetIdentifiers();
    let estimate = identifiers.length * 256;
    for (const identifier of identifiers) {
      const identifierCharge = assetStoreEncoder.encode(identifier).length * 2 + 64;
      const uuidBytes = assetStoreStringSize(state,
        '_lightusd_wasm_asset_store_uuid', identifier, 'getAllAssetUUIDs');
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
        '_lightusd_wasm_asset_store_uuid', identifier), enumerable: true,
        configurable: true, writable: true});
    return result;
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'deleteAssetByUUID', {value: function(uuid) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof uuid !== 'string' || !uuid)
      throw new TypeError('deleteAssetByUUID: expected a non-empty UUID');
    const bytes = assetStoreEncoder.encode(uuid);
    let ptr = 0;
    try {
      ptr = Module['_lightusd_wasm_alloc'](bytes.length);
      if (!ptr) throw new RangeError('deleteAssetByUUID: allocation failed');
      Module.HEAPU8.set(bytes, Number(ptr));
      let status;
      try { status = Module['_lightusd_wasm_asset_store_delete_uuid'](state.handle, ptr, bytes.length); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_wasm_asset_store_delete_uuid'](state.handle, BigInt(ptr), bytes.length);
      }
      if (status < 0) throw new RangeError('deleteAssetByUUID: operation failed');
      return status === 1;
    } finally { if (ptr) Module['_lightusd_wasm_free'](ptr); }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'assetExists', {value: function(nameOrUuid) {
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
      const probe = Module['_lightusd_wasm_alloc'](1);
      memory64Build = typeof probe === 'bigint';
      if (probe) Module['_lightusd_wasm_free'](probe);
    }
    return memory64Build ? BigInt(value) : Number(value);
  };
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetCount', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetCount: wrong argument count');
    return legacyCacheSize(Module['_lightusd_wasm_asset_store_identifier_count'](state.handle));
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setBaseWorkingPath', {value: function(path) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('setBaseWorkingPath: expected one path');
    ++state.busy;
    try { assetStoreSetPath(state, '_lightusd_wasm_asset_store_set_base_path', path, 'setBaseWorkingPath'); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getBaseWorkingPath', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getBaseWorkingPath: wrong argument count');
    ++state.busy;
    try { return assetStoreOutputString(state, '_lightusd_wasm_asset_store_base_path', [], 'getBaseWorkingPath'); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'addAssetSearchPath', {value: function(path) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1) throw new TypeError('addAssetSearchPath: expected one path');
    ++state.busy;
    try { assetStoreSetPath(state, '_lightusd_wasm_asset_store_add_search_path', path, 'addAssetSearchPath'); }
    finally { --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'clearAssetSearchPaths', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('clearAssetSearchPaths: wrong argument count');
    ++state.busy;
    try {
      if (Module['_lightusd_wasm_asset_store_clear_search_paths'](state.handle) !== 0)
        throw new RangeError('clearAssetSearchPaths: update failed');
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setAllowParentRelativeAssetPaths', {value: function(allow) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || typeof allow !== 'boolean')
      throw new TypeError('setAllowParentRelativeAssetPaths: expected one boolean');
    ++state.busy;
    try {
      if (Module['_lightusd_wasm_asset_store_set_allow_parent_paths'](state.handle, allow ? 1 : 0) !== 0)
        throw new RangeError('setAllowParentRelativeAssetPaths: update failed');
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAllowParentRelativeAssetPaths', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAllowParentRelativeAssetPaths: wrong argument count');
    ++state.busy;
    try {
      const value = Module['_lightusd_wasm_asset_store_get_allow_parent_paths'](state.handle);
      if (value !== 0 && value !== 1) throw new RangeError('getAllowParentRelativeAssetPaths: query failed');
      return value === 1;
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetSearchPaths', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetSearchPaths: wrong argument count');
    ++state.busy;
    try {
      const count = Module['_lightusd_wasm_asset_store_search_path_count'](state.handle);
      if (!Number.isInteger(count) || count < 0 || count > 65536)
        throw new RangeError('getAssetSearchPaths: invalid or excessive path count');
      const sizes = [];
      let total = count * 32;
      for (let i = 0; i < count; ++i) {
        let size;
        try { size = Module['_lightusd_wasm_asset_store_search_path_copy'](state.handle, i, 0, 0); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          size = Module['_lightusd_wasm_asset_store_search_path_copy'](state.handle, i, 0n, 0);
        }
        if (!Number.isSafeInteger(size) || size < 0) throw new RangeError('getAssetSearchPaths: invalid path size');
        total += size;
        if (!Number.isSafeInteger(total) || total > 0x20000000)
          throw new RangeError('getAssetSearchPaths: aggregate exceeds 512 MiB limit');
        sizes.push(size);
      }
      return sizes.map((size, i) => assetStoreOutputString(state,
        '_lightusd_wasm_asset_store_search_path_copy', [i], 'getAssetSearchPaths'));
    } finally { --state.busy; }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'hasAsset', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('hasAsset: wrong argument count');
    return this.assetExists(name);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setAsset', {value: function(name, bytes) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 2 || typeof name !== 'string' || !name)
      throw new TypeError('setAsset: expected a non-empty name and byte view');
    const overwritten = this.assetIdentifiers().includes(name);
    this.registerMemoryAsset(name, bytes);
    return overwritten;
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'deleteAssetByName', {value: function(name) {
    if (arguments.length !== 1) throw new TypeError('deleteAssetByName: wrong argument count');
    return this.unregisterMemoryAsset(name);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'deleteAsset', {value: function(nameOrUuid) {
    if (arguments.length !== 1 || typeof nameOrUuid !== 'string' || !nameOrUuid)
      throw new TypeError('deleteAsset: expected a non-empty name or UUID');
    if (this.assetIdentifiers().includes(nameOrUuid)) return this.deleteAssetByName(nameOrUuid);
    return this.deleteAssetByUUID(nameOrUuid);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'clearAssets', {value: function() {
    if (arguments.length) throw new TypeError('clearAssets: wrong argument count');
    return this.clear();
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setMemoryLimitBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit < 1 || limit > 0x40000000) {
      throw new TypeError('setMemoryLimitBytes: expected 1 byte through 1 GiB');
    }
    const status = Module['_lightusd_wasm_asset_store_set_memory_limit'](state.handle, limit);
    if (status !== 0) throw new RangeError('setMemoryLimitBytes: limit below current use');
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setMetadataLimitBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 ||
        !(typeof limit === 'bigint' ? limit >= 1n && limit <= 0x40000000n :
          Number.isSafeInteger(limit) && limit >= 1 && limit <= 0x40000000)) {
      throw new TypeError('setMetadataLimitBytes: expected 1 byte through 1 GiB');
    }
    const value = BigInt(limit);
    if (Module['_lightusd_wasm_asset_store_set_metadata_limit'](state.handle, value) !== 0) {
      throw new RangeError('setMetadataLimitBytes: limit below current metadata use');
    }
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'metadataBytesUsed', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('metadataBytesUsed: wrong argument count');
    return Module['_lightusd_wasm_asset_store_metadata_bytes'](state.handle);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'metadataLimitBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('metadataLimitBytes: wrong argument count');
    return Module['_lightusd_wasm_asset_store_metadata_limit'](state.handle);
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'memoryStats', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('memoryStats: wrong argument count');
    return {assetCount: Module['_lightusd_wasm_asset_store_identifier_count'](state.handle),
      bytesUsed: Module['_lightusd_wasm_asset_store_memory_bytes'](state.handle),
      limitBytes: Module['_lightusd_wasm_asset_store_memory_limit'](state.handle)};
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetCacheSizeBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetCacheSizeBytes: wrong argument count');
    return legacyCacheSize(Module['_lightusd_wasm_asset_store_cache_bytes'](state.handle));
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'getAssetCacheMaxSizeBytes', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('getAssetCacheMaxSizeBytes: wrong argument count');
    return legacyCacheSize(Module['_lightusd_wasm_asset_store_cache_max_bytes'](state.handle));
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'setAssetCacheMaxSizeBytes', {value: function(limit) {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length !== 1 ||
        !(typeof limit === 'bigint' ? limit >= 0n && limit <= 0xffffffffffffffffn :
          Number.isSafeInteger(limit) && limit >= 0))
      throw new TypeError('setAssetCacheMaxSizeBytes: expected a uint64 byte limit');
    if (Module['_lightusd_wasm_asset_store_set_cache_max_bytes'](state.handle,
      BigInt(limit)) !== 0) throw new RangeError('setAssetCacheMaxSizeBytes: update failed');
  }});
  Object.defineProperty(Module.AssetStore.prototype, 'clear', {value: function() {
    const state = live.get(this);
    if (!state?.handle || state.kind !== 5) throw new TypeError('Invalid LightUSD receiver');
    if (arguments.length) throw new TypeError('clear: wrong argument count');
    if (Module['_lightusd_wasm_asset_store_clear'](state.handle) !== 0) throw new RangeError('clear: failed');
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
      const allowed = Module['_lightusd_wasm_converter_root_preflight'](
        state.handle, data.length);
      if (allowed < 0) return false;
    }
    if (kind === 1) {
      let keyBlock = 0;
      ++state.busy;
      try {
        keyBlock = Module['_lightusd_wasm_alloc'](Math.max(nameBytes.length, 1));
        if (!keyBlock) throw new RangeError(method + ': name allocation failed');
        Module.HEAPU8.set(nameBytes, Number(keyBlock));
        const call = ptr => Module['_lightusd_wasm_converter_asset_preflight'](
          state.handle, ptr, nameBytes.length, data.length);
        let allowed;
        try { allowed = call(keyBlock); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          allowed = call(BigInt(keyBlock));
        }
        if (allowed < 0) return false;
      } finally {
        if (keyBlock) Module['_lightusd_wasm_free'](keyBlock);
        --state.busy;
      }
    }
    const total = Math.max(nameBytes.length + data.length, 1);
    let block = 0;
    ++state.busy;
    try {
      block = Module['_lightusd_wasm_alloc'](total);
      if (!block) throw new RangeError(method + ': allocation failed');
      const base = Number(block);
      if (heapSource) {
        data = new Uint8Array(Module.HEAPU8.buffer, heapSourceOffset, heapSourceLength);
      }
      Module.HEAPU8.set(nameBytes, base);
      Module.HEAPU8.set(data, base + nameBytes.length);
      const pointer = offset => typeof block === 'bigint' ? block + BigInt(offset)
                                                           : block + offset;
      const call = p => Module['_lightusd_wasm_converter_set_bytes'](
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
      if (block) Module['_lightusd_wasm_free'](block);
      --state.busy;
    }
  }
  Object.defineProperty(Module.USDZConverterNative.prototype, 'loadFromBinary', {value: function(bytes, filename) {
    converterState(this);
    if (arguments.length !== 2) throw new TypeError('loadFromBinary: wrong argument count');
    if (typeof filename !== 'string') throw new TypeError('loadFromBinary: expected filename');
    return converterSetBytes(this, 0, filename,
      converterByteView(bytes, 'loadFromBinary', false), 'loadFromBinary');
  }});
  Object.defineProperty(Module.USDZConverterNative.prototype, 'createSampleScene', {
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
  Object.defineProperty(Module.USDZConverterNative.prototype, 'setAsset', {value: function(name, bytes) {
    converterState(this);
    if (arguments.length !== 2) throw new TypeError('setAsset: wrong argument count');
    if (typeof name !== 'string') throw new TypeError('setAsset: expected name');
    return converterSetBytes(this, 1, name,
      converterByteView(bytes, 'setAsset', false), 'setAsset');
  }});
  Object.defineProperty(Module.USDZConverterNative.prototype, 'setMaxAssetBytes', {value: function(limit) {
    const state = converterState(this);
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
      throw new RangeError('setMaxAssetBytes: expected an integer from 1 through 1 GiB');
    }
    const status = Module['_lightusd_wasm_converter_control'](state.handle, 2, limit, 0);
    if (status !== 0) return {success: false, error: this.error()};
    return {success: true};
  }});
  for (const [name, field] of [['assetBytes', 0], ['maxAssetBytes', 1]]) {
    Object.defineProperty(Module.USDZConverterNative.prototype, name, {value: function() {
      const state = converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      const value = Module['_lightusd_wasm_converter_control'](state.handle, 3, field, 0);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    }});
  }
  Object.defineProperty(Module.USDZConverterNative.prototype, 'setMaxMeshBytes', {value: function(limit) {
    const state = converterState(this);
    if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
      throw new RangeError('setMaxMeshBytes: expected an integer from 1 through 1 GiB');
    }
    const status = Module['_lightusd_wasm_converter_control'](state.handle, 4, limit, 0);
    return status === 0 ? {success: true} : {success: false, error: this.error()};
  }});
  for (const [name, field] of [['meshBytes', 0], ['maxMeshBytes', 1]]) {
    Object.defineProperty(Module.USDZConverterNative.prototype, name, {value: function() {
      const state = converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      const value = Module['_lightusd_wasm_converter_control'](state.handle, 5, field, 0);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    }});
  }
  Object.defineProperty(Module.USDZConverterNative.prototype,
    'setMaxRetainedPayloadBytes', {value: function(limit) {
      const state = converterState(this);
      if (arguments.length !== 1 || !Number.isInteger(limit) || limit <= 0 || limit > 0x40000000) {
        throw new RangeError('setMaxRetainedPayloadBytes: expected an integer from 1 through 1 GiB');
      }
      const status = Module['_lightusd_wasm_converter_control'](state.handle, 6, limit, 0);
      return status === 0 ? {success: true} : {success: false, error: this.error()};
    }});
  for (const [name, field] of [['retainedPayloadBytes', 0],
                                ['maxRetainedPayloadBytes', 1]]) {
    Object.defineProperty(Module.USDZConverterNative.prototype, name, {value: function() {
      const state = converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      const value = Module['_lightusd_wasm_converter_control'](state.handle, 7, field, 0);
      if (value < 0) throw new TypeError('Invalid LightUSD receiver');
      return value;
    }});
  }
  Object.defineProperty(Module.USDZConverterNative.prototype, 'createURDFPhysicsScene', {value: function(json) {
    converterState(this);
    if (arguments.length !== 1) throw new TypeError('createURDFPhysicsScene: wrong argument count');
    if (typeof json !== 'string') throw new TypeError('createURDFPhysicsScene: expected string');
    return converterSetBytes(this, 2, '', converterEncoder.encode(json), 'createURDFPhysicsScene');
  }});
  Object.defineProperty(Module.USDZConverterNative.prototype, 'clearURDFMeshBuffers', {value: function() {
    const state = converterState(this);
    if (arguments.length !== 0) throw new TypeError('clearURDFMeshBuffers: wrong argument count');
    if (Module['_lightusd_wasm_converter_control'](state.handle, 0, 0, 0) !== 0) {
      throw new TypeError('Invalid LightUSD receiver');
    }
  }});
  Object.defineProperty(Module.USDZConverterNative.prototype, 'setUSDCExportLimitMB', {value: function(fileMB, memoryMB) {
    const state = converterState(this);
    if (arguments.length !== 2) throw new TypeError('setUSDCExportLimitMB: wrong argument count');
    if (typeof fileMB !== 'number' || typeof memoryMB !== 'number') {
      throw new TypeError('setUSDCExportLimitMB: expected numbers');
    }
    const status = Module['_lightusd_wasm_converter_control'](
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
      preflightBlock = Module['_lightusd_wasm_alloc'](Math.max(nameBytes.length, 1));
      if (!preflightBlock) throw new RangeError(method + ': allocation failed');
      const preBase = Number(preflightBlock);
      Module.HEAPU8.set(nameBytes, preBase);
      const prePointer = typeof preflightBlock === 'bigint' ? preflightBlock : preBase;
      preflight = Module['_lightusd_wasm_converter_mesh_preflight'](
        state.handle, prePointer, nameBytes.length,
        arrays[0].length, arrays[1].length, arrays[2].length, arrays[3].length);
      if (preflight < 0) throw new RangeError(method + ': invalid input or handle');
      if (preflight === 0) return false;
    } finally {
      if (preflightBlock) Module['_lightusd_wasm_free'](preflightBlock);
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
      block = Module['_lightusd_wasm_alloc'](Math.max(total, 1));
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
      const call = p => Module['_lightusd_wasm_converter_set_mesh'](
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
      if (block) Module['_lightusd_wasm_free'](block);
      --state.busy;
    }
  }
  for (const [name, kind] of [['setVisualMesh', 0], ['setCollisionMesh', 1]]) {
    Object.defineProperty(Module.USDZConverterNative.prototype, name, {value: function(meshName, positions, normals, uvs, indices) {
      converterState(this);
      if (arguments.length !== 5) throw new TypeError(name + ': wrong argument count');
      return converterSetMesh(this, kind, meshName, positions, normals, uvs, indices, name);
    }});
  }
  function converterExport(self, kind, method, rootFormat, remapBytes = null, configured = false) {
    const state = converterState(self);
    let remapPtr = 0;
    ++state.busy;
    try {
      let status;
      if (remapBytes) {
        remapPtr = Module['_lightusd_wasm_alloc'](Math.max(remapBytes.length, 1));
        if (!remapPtr) throw new RangeError(method + ': remap allocation failed');
        Module.HEAPU8.set(remapBytes, Number(remapPtr));
        const call = ptr => configured
          ? Module['_lightusd_wasm_converter_export_configured'](state.handle, ptr, remapBytes.length)
          : Module['_lightusd_wasm_converter_export_with_remap'](state.handle, ptr, remapBytes.length, rootFormat);
        try { status = call(remapPtr); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          status = call(BigInt(remapPtr));
        }
      } else {
        status = rootFormat == null
          ? Module['_lightusd_wasm_converter_export'](state.handle, kind)
          : Module['_lightusd_wasm_converter_export_with_options'](
              state.handle, kind, rootFormat);
      }
      if (status < 0) throw new RangeError(method + ': invalid handle');
      if (status === 0) return kind <= 1 ? '' : null;
      const copy = (ptr, cap) => {
        try { return Module['_lightusd_wasm_converter_export_buffer'](state.handle, ptr, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_wasm_converter_export_buffer'](state.handle, BigInt(ptr), cap);
        }
      };
      const size = copy(0, 0);
      if (size < 0) throw new RangeError(method + ': output query failed');
      if (!size) return kind <= 1 ? '' : new Uint8Array(0);
      const pointer = Module['_lightusd_wasm_converter_export_data'](state.handle);
      if (!pointer) throw new RangeError(method + ': output data unavailable');
      const start = Number(pointer);
      const end = start + size;
      if (!Number.isSafeInteger(start) || end > Module.HEAPU8.byteLength) {
        throw new RangeError(method + ': output span is outside WASM memory');
      }
      const bytes = Module.HEAPU8.slice(start, end);
      return kind <= 1 ? converterDecoder.decode(bytes) : bytes;
    } finally {
      if (remapPtr) Module['_lightusd_wasm_free'](remapPtr);
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
    Object.defineProperty(Module.USDZConverterNative.prototype, name, {value: function() {
      converterState(this);
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      return converterExport(this, kind, name);
    }});
  }
  Object.defineProperty(Module.USDZConverterNative.prototype,
    'exportAsUSDZWithRemap', {value: function(remap) {
      converterState(this);
      if (arguments.length !== 1) throw new TypeError('exportAsUSDZWithRemap: expected one remap object');
      return converterExport(this, 3, 'exportAsUSDZWithRemap', 0,
        converterRemapBytes(remap, 'exportAsUSDZWithRemap'));
    }});
  Object.defineProperty(Module.USDZConverterNative.prototype,
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
      const config = {...exportOptions, rootLayerFormat: format,
        remap: hasRemap ? remapOrOptions : {}};
      // Validate the remap before serializing options, as the older entrypoint did.
      converterRemapBytes(config.remap, 'exportAsUSDZWithOptions');
      return converterExport(this, 3, 'exportAsUSDZWithOptions', format === 'usda' ? 1 : 0,
        converterEncoder.encode(JSON.stringify(config)), true);
    }});
  function converterString(self, kind, method) {
    const state = live.get(self);
    if (!state?.handle || state.kind !== 1) throw new TypeError('Invalid LightUSD receiver');
    let buffer = 0;
    try {
      const call = (ptr, cap) => {
        try { return Module['_lightusd_wasm_converter_string'](state.handle, kind, ptr, cap); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          return Module['_lightusd_wasm_converter_string'](state.handle, kind, BigInt(ptr), cap);
        }
      };
      const size = call(0, 0);
      if (size < 0) throw new RangeError(method + ': string query failed');
      if (!size) return '';
      buffer = Module['_lightusd_wasm_alloc'](size);
      if (!buffer) throw new RangeError(method + ': allocation failed');
      if (call(buffer, size) !== size) throw new RangeError(method + ': string changed');
      return converterDecoder.decode(Module.HEAPU8.subarray(Number(buffer), Number(buffer) + size));
    } finally { if (buffer) Module['_lightusd_wasm_free'](buffer); }
  }
  for (const [name, kind] of [['error', 0], ['warn', 1]]) {
    Object.defineProperty(Module.USDZConverterNative.prototype, name, {value: function() {
      if (arguments.length !== 0) throw new TypeError(name + ': wrong argument count');
      return converterString(this, kind, name);
    }});
  }
  for (const [name, apply] of [['describeUDIM', 0], ['applyUDIM', 1]]) {
    Object.defineProperty(Module.USDZConverterNative.prototype, name, {value: function(request) {
      const state = live.get(this);
      if (!state?.handle || state.kind !== 1) throw new TypeError('Invalid LightUSD receiver');
      if (arguments.length !== apply) throw new TypeError(name + ': wrong argument count');
      const bytes = converterEncoder.encode(apply ? JSON.stringify(request) : '');
      if (bytes.length > 4 * 1024 * 1024) throw new RangeError(name + ': edit plans exceed 4 MiB');
      let input = 0, output = 0;
      ++state.busy;
      try {
        input = Module['_lightusd_wasm_alloc'](Math.max(1, bytes.length));
        if (!input) throw new RangeError(name + ': allocation failed');
        Module.HEAPU8.set(bytes, Number(input));
        const size = Module['_lightusd_wasm_converter_udim'](state.handle, apply, input, bytes.length);
        if (size < 0) throw new RangeError(name + ': layer query failed');
        output = Module['_lightusd_wasm_alloc'](Math.max(1, size));
        if (!output) throw new RangeError(name + ': allocation failed');
        if (Module['_lightusd_wasm_converter_udim_copy'](state.handle, output, size) !== size) throw new RangeError(name + ': output changed');
        return JSON.parse(converterDecoder.decode(Module.HEAPU8.subarray(Number(output), Number(output) + size)));
      } finally {
        if (output) Module['_lightusd_wasm_free'](output);
        if (input) Module['_lightusd_wasm_free'](input);
        --state.busy;
      }
    }});
  }
  Object.defineProperty(Module.USDZConverterNative.prototype, 'rewriteRoot', {value: function(
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
      input = Module['_lightusd_wasm_alloc'](total);
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
      const call = p => Module['_lightusd_wasm_converter_rewrite'](
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
        output = Module['_lightusd_wasm_alloc'](size);
        if (!output) throw new RangeError('rewriteRoot: output allocation failed');
        const copy = ptr => Module['_lightusd_wasm_converter_rewrite_buffer'](
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
      if (output) Module['_lightusd_wasm_free'](output);
      if (input) Module['_lightusd_wasm_free'](input);
      --state.busy;
    }
  }});
  // Helpers borrowed by the render/image API post-JS files. `exr` is a
  // getter because loadFeature('exr') reassigns it after module creation.
  Module.__lightusdApiInternals = {
    live, defineClass, converterByteView, assetStoreDecoder,
    get exr() { return exr; }
  };
}
