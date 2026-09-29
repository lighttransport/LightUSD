// SPDX-License-Identifier: Apache-2.0
// Typed compatibility adapters layered over selected combined-module C calls.
{
  const decoder = new TextDecoder();

  function withPointerFallback(call, ptr, ...args) {
    try { return call(ptr, ...args); }
    catch (error) {
      if (!(error instanceof TypeError) || typeof ptr === 'bigint') throw error;
      return call(BigInt(ptr), ...args);
    }
  }

  function copyError() {
    let ptr = 0;
    try {
      const size = withPointerFallback(
        (p, cap) => Module['_lightusd_combined_next_flatten_error'](p, cap), 0, 0);
      if (size < 0) return 'USDC flatten failed';
      if (!size) return '';
      ptr = Module['_lightusd_combined_alloc'](size);
      if (!ptr) return 'USDC flatten failed';
      const copied = withPointerFallback(
        (p, cap) => Module['_lightusd_combined_next_flatten_error'](p, cap),
        ptr, size);
      if (copied !== size) return 'USDC flatten failed';
      return decoder.decode(Module.HEAPU8.subarray(Number(ptr), Number(ptr) + size));
    } finally {
      if (ptr) Module['_lightusd_combined_free'](ptr);
    }
  }

  function finishFlatten(status, infoPtr) {
    if (status < 0) throw new TypeError('next flatten: invalid input');
    if (status === 0) return {success: false, error: copyError()};
    const info = new DataView(Module.HEAPU8.buffer, Number(infoPtr), 88);
    const size = info.getUint32(4, true);
    const stats = {
      inputBytes: info.getFloat64(16, true),
      outputBytes: info.getFloat64(24, true),
      primCount: info.getFloat64(32, true),
      arraysPassedThrough: info.getFloat64(40, true),
      arraysReencoded: info.getFloat64(48, true),
      assetPathsRemapped: info.getFloat64(56, true),
      readMs: info.getFloat64(64, true),
      composeMs: info.getFloat64(72, true),
      writeMs: info.getFloat64(80, true)
    };
    return Object.assign({success: true, data: copyFlattenOutput(size)}, stats);
  }

  function copyFlattenOutput(size) {
    const output = Module['_lightusd_combined_alloc'](Math.max(size, 1));
    if (!output) throw new RangeError('next flatten: output allocation failed');
    try {
      const copied = withPointerFallback(
        (ptr, cap) => Module['_lightusd_combined_next_flatten_copy'](ptr, cap),
        output, size);
      if (copied !== size) throw new RangeError('next flatten: output changed');
      return Module.HEAPU8.slice(Number(output), Number(output) + size);
    } finally {
      Module['_lightusd_combined_free'](output);
    }
  }

  function requireLiveLoader(loader) {
    const state = loader && loader.$$;
    if (!(loader instanceof Module.LightUSDLoaderNative) || !state ||
        state.ptr == null || state.ptr === 0 || state.ptr === 0n) {
      throw new TypeError('Invalid LightUSDLoaderNative receiver');
    }
    return state.ptr;
  }

  function flattenUSDC(data, lazyArrays) {
    requireLiveLoader(this);
    if (arguments.length !== 2) throw new TypeError('nextFlattenUSDC: wrong argument count');
    if (!ArrayBuffer.isView(data)) throw new TypeError('nextFlattenUSDC: expected byte view');
    let source = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (source.byteLength > 0x40000000) {
      return {success: false, error: 'Input exceeds 1 GiB limit'};
    }
    if (source.buffer === Module.HEAPU8.buffer) source = source.slice();

    let input = 0, infoPtr = 0, output = 0;
    try {
      input = Module['_lightusd_combined_alloc'](Math.max(source.byteLength, 1));
      infoPtr = Module['_lightusd_combined_alloc'](88);
      if (!input || !infoPtr) throw new RangeError('nextFlattenUSDC: allocation failed');
      const inputOffset = Number(input);
      const infoOffset = Number(infoPtr);
      Module.HEAPU8.set(source, inputOffset);
      const info = new DataView(Module.HEAPU8.buffer, infoOffset, 88);
      info.setUint32(0, 88, true);
      let status;
      try {
        status = Module['_lightusd_combined_next_flatten_usdc'](
          input, source.byteLength, Number(!!lazyArrays), infoPtr);
      } catch (error) {
        if (!(error instanceof TypeError) || typeof input === 'bigint') throw error;
        status = Module['_lightusd_combined_next_flatten_usdc'](
          BigInt(input), source.byteLength, Number(!!lazyArrays), BigInt(infoPtr));
      }
      return finishFlatten(status, infoPtr);
    } finally {
      if (output) Module['_lightusd_combined_free'](output);
      if (infoPtr) Module['_lightusd_combined_free'](infoPtr);
      if (input) Module['_lightusd_combined_free'](input);
    }
  }

  function flattenBuffer(uuid, lazyArrays, remap) {
    const loaderPtr = requireLiveLoader(this);
    const hasRemap = arguments.length === 3 || arguments.length === 4;
    const hasVariants = arguments.length === 4;
    if (arguments.length < 2 || arguments.length > 4) {
      throw new TypeError('nextFlattenBuffer: wrong argument count');
    }
    if (typeof uuid !== 'string' || uuid.length === 0) {
      throw new TypeError('nextFlattenBuffer: expected buffer UUID');
    }
    const uuidBytes = new TextEncoder().encode(uuid);
    const remapBytes = hasRemap ? encodeStringMap(remap) : new Uint8Array(0);
    const variantBytes = hasVariants
      ? encodeStringMap(arguments[3], true) : new Uint8Array(0);
    let uuidPtr = 0, remapPtr = 0, variantPtr = 0, infoPtr = 0;
    try {
      uuidPtr = Module['_lightusd_combined_alloc'](uuidBytes.length);
      if (remapBytes.length) remapPtr = Module['_lightusd_combined_alloc'](remapBytes.length);
      if (variantBytes.length) variantPtr = Module['_lightusd_combined_alloc'](variantBytes.length);
      infoPtr = Module['_lightusd_combined_alloc'](88);
      if (!uuidPtr || (remapBytes.length && !remapPtr) ||
          (variantBytes.length && !variantPtr) || !infoPtr) {
        throw new RangeError('nextFlattenBuffer: allocation failed');
      }
      Module.HEAPU8.set(uuidBytes, Number(uuidPtr));
      if (remapBytes.length) Module.HEAPU8.set(remapBytes, Number(remapPtr));
      if (variantBytes.length) Module.HEAPU8.set(variantBytes, Number(variantPtr));
      const info = new DataView(Module.HEAPU8.buffer, Number(infoPtr), 88);
      info.setUint32(0, 88, true);
      const invoke = (loader, uuidBuffer, remapBuffer, variantBuffer, infoBuffer) =>
        hasRemap
          ? Module['_lightusd_combined_next_flatten_buffer_maps'](
            loader, uuidBuffer, uuidBytes.length, Number(!!lazyArrays),
            remapBuffer, remapBytes.length, variantBuffer, variantBytes.length,
            infoBuffer)
          : Module['_lightusd_combined_next_flatten_buffer'](
            loader, uuidBuffer, uuidBytes.length, Number(!!lazyArrays), infoBuffer);
      let status;
      try {
        status = invoke(loaderPtr, uuidPtr, remapPtr, variantPtr, infoPtr);
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        const asPointer = (pointer) => typeof pointer === 'bigint'
          ? pointer : BigInt(pointer);
        status = invoke(asPointer(loaderPtr), asPointer(uuidPtr),
          asPointer(remapPtr), asPointer(variantPtr), asPointer(infoPtr));
      }
      return finishFlatten(status, infoPtr);
    } finally {
      if (infoPtr) Module['_lightusd_combined_free'](infoPtr);
      if (variantPtr) Module['_lightusd_combined_free'](variantPtr);
      if (remapPtr) Module['_lightusd_combined_free'](remapPtr);
      if (uuidPtr) Module['_lightusd_combined_free'](uuidPtr);
    }
  }

  function endFlattenSession(session) {
    const loaderPtr = requireLiveLoader(this);
    if (arguments.length !== 1) {
      throw new TypeError('nextFlattenAsyncEnd: wrong argument count');
    }
    if (typeof session !== 'string') {
      throw new TypeError('nextFlattenAsyncEnd: expected session ID');
    }
    const sessionBytes = new TextEncoder().encode(session);
    let sessionPtr = 0;
    try {
      sessionPtr = Module['_lightusd_combined_alloc'](Math.max(sessionBytes.length, 1));
      if (!sessionPtr) throw new RangeError('nextFlattenAsyncEnd: allocation failed');
      Module.HEAPU8.set(sessionBytes, Number(sessionPtr));
      let status;
      try {
        status = Module['_lightusd_combined_next_flatten_async_end'](
          loaderPtr, sessionPtr, sessionBytes.length);
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        status = Module['_lightusd_combined_next_flatten_async_end'](
          BigInt(loaderPtr), typeof sessionPtr === 'bigint'
            ? sessionPtr : BigInt(sessionPtr), sessionBytes.length);
      }
      if (status < 0) throw new TypeError('nextFlattenAsyncEnd: invalid receiver');
      return {success: status === 1};
    } finally {
      if (sessionPtr) Module['_lightusd_combined_free'](sessionPtr);
    }
  }

  function provideFlattenLayer(session, key, data) {
    const loaderPtr = requireLiveLoader(this);
    if (arguments.length !== 3) {
      throw new TypeError('nextFlattenAsyncProvideLayer: wrong argument count');
    }
    if (typeof session !== 'string' || typeof key !== 'string') {
      throw new TypeError('nextFlattenAsyncProvideLayer: expected string IDs');
    }
    if (!ArrayBuffer.isView(data)) {
      throw new TypeError('nextFlattenAsyncProvideLayer: expected byte view');
    }
    let bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    const oversized = bytes.byteLength > 0x40000000;
    const layerSize = oversized ? 0x40000001 : bytes.byteLength;
    if (!oversized && bytes.buffer === Module.HEAPU8.buffer) bytes = bytes.slice();
    const sessionBytes = new TextEncoder().encode(session);
    const keyBytes = new TextEncoder().encode(key);
    let sessionPtr = 0, keyPtr = 0, dataPtr = 0;
    try {
      sessionPtr = Module['_lightusd_combined_alloc'](Math.max(sessionBytes.length, 1));
      keyPtr = Module['_lightusd_combined_alloc'](Math.max(keyBytes.length, 1));
      dataPtr = oversized ? 0 : Module['_lightusd_combined_alloc'](Math.max(bytes.byteLength, 1));
      if (!sessionPtr || !keyPtr || (!dataPtr && !oversized)) {
        throw new RangeError('nextFlattenAsyncProvideLayer: allocation failed');
      }
      Module.HEAPU8.set(sessionBytes, Number(sessionPtr));
      Module.HEAPU8.set(keyBytes, Number(keyPtr));
      if (!oversized) Module.HEAPU8.set(bytes, Number(dataPtr));
      const invoke = (loader, sessionBuffer, keyBuffer, dataBuffer) =>
        Module['_lightusd_combined_next_flatten_async_provide_layer'](
          loader, sessionBuffer, sessionBytes.length, keyBuffer, keyBytes.length,
          dataBuffer, layerSize);
      let status;
      try {
        status = invoke(loaderPtr, sessionPtr, keyPtr, dataPtr);
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        const asPointer = (pointer) => typeof pointer === 'bigint'
          ? pointer : BigInt(pointer);
        status = invoke(asPointer(loaderPtr), asPointer(sessionPtr),
          asPointer(keyPtr), asPointer(dataPtr));
      }
      if (status < 0) throw new TypeError('nextFlattenAsyncProvideLayer: invalid receiver');
      if (status === 1) {
        return {success: false, error: `Unknown next flatten session: ${session}`};
      }
      if (status === 2) {
        return {success: false, error: `Invalid or empty layer data for: ${key}`};
      }
      return {success: true};
    } finally {
      if (dataPtr) Module['_lightusd_combined_free'](dataPtr);
      if (keyPtr) Module['_lightusd_combined_free'](keyPtr);
      if (sessionPtr) Module['_lightusd_combined_free'](sessionPtr);
    }
  }

  function encodeStringMap(map, variantOverrides = false) {
    if (map == null) return new Uint8Array(0);
    if (typeof map !== 'object' && typeof map !== 'string') {
      throw new TypeError('nextFlattenAsyncBeginRemap: expected string map');
    }
    const entries = [];
    for (const key of Object.keys(map)) {
      const value = map[key];
      if (variantOverrides && (key.length === 0 || value == null)) continue;
      if (typeof value !== 'string') {
        throw new TypeError('nextFlattenAsyncBeginRemap: map values must be strings');
      }
      if (variantOverrides && value.length === 0) continue;
      entries.push([new TextEncoder().encode(key), new TextEncoder().encode(value)]);
    }
    const byteSize = 4 + entries.reduce(
      (size, [key, value]) => size + 8 + key.length + value.length, 0);
    if (byteSize > 0xffffffff) throw new RangeError('flatten map is too large');
    const packed = new Uint8Array(byteSize);
    const view = new DataView(packed.buffer);
    view.setUint32(0, entries.length, true);
    let offset = 4;
    for (const [key, value] of entries) {
      view.setUint32(offset, key.length, true);
      view.setUint32(offset + 4, value.length, true);
      offset += 8;
      packed.set(key, offset);
      offset += key.length;
      packed.set(value, offset);
      offset += value.length;
    }
    return packed;
  }

  function beginFlattenSession(uuid, rootName, lazyArrays, remap) {
    const loaderPtr = requireLiveLoader(this);
    const hasRemap = arguments.length === 4 || arguments.length === 5;
    const hasVariants = arguments.length === 5;
    if (arguments.length !== 3 && !hasRemap && !hasVariants) {
      throw new TypeError('nextFlattenAsyncBegin: wrong argument count');
    }
    if (typeof uuid !== 'string' || typeof rootName !== 'string') {
      throw new TypeError('nextFlattenAsyncBegin: expected string names');
    }
    const uuidBytes = new TextEncoder().encode(uuid);
    const rootBytes = new TextEncoder().encode(rootName);
    const remapBytes = hasRemap ? encodeStringMap(remap) : new Uint8Array(0);
    const variantBytes = hasVariants
      ? encodeStringMap(arguments[4], true) : new Uint8Array(0);
    let uuidPtr = 0, rootPtr = 0, remapPtr = 0, variantPtr = 0;
    let sessionPtr = 0, sizePtr = 0;
    try {
      uuidPtr = Module['_lightusd_combined_alloc'](Math.max(uuidBytes.length, 1));
      rootPtr = Module['_lightusd_combined_alloc'](Math.max(rootBytes.length, 1));
      if (remapBytes.length) {
        remapPtr = Module['_lightusd_combined_alloc'](remapBytes.length);
      }
      if (variantBytes.length) {
        variantPtr = Module['_lightusd_combined_alloc'](variantBytes.length);
      }
      sessionPtr = Module['_lightusd_combined_alloc'](64);
      sizePtr = Module['_lightusd_combined_alloc'](4);
      if (!uuidPtr || !rootPtr || (remapBytes.length && !remapPtr) ||
          (variantBytes.length && !variantPtr) ||
          !sessionPtr || !sizePtr) {
        throw new RangeError('nextFlattenAsyncBegin: allocation failed');
      }
      Module.HEAPU8.set(uuidBytes, Number(uuidPtr));
      Module.HEAPU8.set(rootBytes, Number(rootPtr));
      if (remapBytes.length) Module.HEAPU8.set(remapBytes, Number(remapPtr));
      if (variantBytes.length) Module.HEAPU8.set(variantBytes, Number(variantPtr));
      const invoke = (loader, uuidBuffer, rootBuffer, remapBuffer, variantBuffer,
                      sessionBuffer, sizeBuffer) => hasVariants
        ? Module['_lightusd_combined_next_flatten_async_begin_remap_variants'](
          loader, uuidBuffer, uuidBytes.length, rootBuffer, rootBytes.length,
          Number(!!lazyArrays), remapBuffer, remapBytes.length,
          variantBuffer, variantBytes.length, sessionBuffer, 64, sizeBuffer)
        : hasRemap
        ? Module['_lightusd_combined_next_flatten_async_begin_remap'](
          loader, uuidBuffer, uuidBytes.length, rootBuffer, rootBytes.length,
          Number(!!lazyArrays), remapBuffer, remapBytes.length,
          sessionBuffer, 64, sizeBuffer)
        : Module['_lightusd_combined_next_flatten_async_begin'](
          loader, uuidBuffer, uuidBytes.length, rootBuffer, rootBytes.length,
          Number(!!lazyArrays), sessionBuffer, 64, sizeBuffer);
      let status;
      try {
        status = invoke(loaderPtr, uuidPtr, rootPtr, remapPtr, variantPtr,
          sessionPtr, sizePtr);
      } catch (error) {
        if (!(error instanceof TypeError)) throw error;
        const asPointer = (pointer) => typeof pointer === 'bigint'
          ? pointer : BigInt(pointer);
        status = invoke(asPointer(loaderPtr), asPointer(uuidPtr), asPointer(rootPtr),
          asPointer(remapPtr), asPointer(variantPtr), asPointer(sessionPtr),
          asPointer(sizePtr));
      }
      if (status < 0) throw new TypeError('nextFlattenAsyncBegin: invalid receiver');
      if (status === 0) {
        return {success: false, error: `Unknown or empty zero-copy buffer: ${uuid}`};
      }
      if (status !== 1) {
        throw new TypeError('nextFlattenAsyncBegin: invalid remap data');
      }
      const sessionSize = new DataView(
        Module.HEAPU8.buffer, Number(sizePtr), 4).getUint32(0, true);
      const session = decoder.decode(Module.HEAPU8.subarray(
        Number(sessionPtr), Number(sessionPtr) + sessionSize));
      return {success: true, session, status: 'ready'};
    } finally {
      if (sizePtr) Module['_lightusd_combined_free'](sizePtr);
      if (sessionPtr) Module['_lightusd_combined_free'](sessionPtr);
      if (variantPtr) Module['_lightusd_combined_free'](variantPtr);
      if (remapPtr) Module['_lightusd_combined_free'](remapPtr);
      if (rootPtr) Module['_lightusd_combined_free'](rootPtr);
      if (uuidPtr) Module['_lightusd_combined_free'](uuidPtr);
    }
  }

  // JS callbacks named by ID for the C bridges in binding-combined-api.cc.
  // Entries registered for one call share `ctx`: the first exception stops
  // every later callback of that call and is rethrown once the C call returns.
  const flattenCallbacks = new Map();
  let nextFlattenCallbackId = 1;

  function registerFlattenCallback(ctx, callback) {
    if (callback == null) return 0;
    let id;
    do {
      id = nextFlattenCallbackId++ >>> 0;
    } while (id === 0 || flattenCallbacks.has(id));
    flattenCallbacks.set(id, {callback, ctx, fetched: null});
    ctx.ids.push(id);
    return id;
  }

  function invokeFlattenCallback(id, failure, run) {
    const entry = flattenCallbacks.get(id);
    if (!entry || entry.ctx.errorSet) return failure;
    try {
      return run(entry);
    } catch (error) {
      entry.ctx.error = error;
      entry.ctx.errorSet = true;
      return failure;
    }
  }

  function heapString(offset, size) {
    return decoder.decode(Module.HEAPU8.subarray(offset, offset + size));
  }

  Module['__lightusdCombinedFlattenEmit'] = (id, offset, size) =>
    invokeFlattenCallback(id, 0, (entry) => entry.callback(
      new Uint8Array(Module.HEAPU8.buffer, offset, size)) === false ? 0 : 1);
  Module['__lightusdCombinedFlattenExists'] = (id, offset, size) =>
    invokeFlattenCallback(id, 0, (entry) =>
      entry.callback(heapString(offset, size)) === true ? 1 : 0);
  Module['__lightusdCombinedFlattenFetch'] = (id, offset, size) =>
    invokeFlattenCallback(id, -1, (entry) => {
      entry.fetched = null;
      const value = entry.callback(heapString(offset, size));
      let bytes;
      if (value instanceof ArrayBuffer) bytes = new Uint8Array(value);
      else if (ArrayBuffer.isView(value)) {
        bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      } else return -1;
      if (bytes.byteLength === 0 || bytes.byteLength > 0x40000000) return -1;
      // The native copy buffer may grow memory and detach a heap view.
      entry.fetched = bytes.buffer === Module.HEAPU8.buffer ? bytes.slice() : bytes;
      return bytes.byteLength;
    });
  Module['__lightusdCombinedFlattenFetchCopy'] = (id, offset, size) => {
    const entry = flattenCallbacks.get(id);
    const bytes = entry && entry.fetched;
    if (entry) entry.fetched = null;
    if (!bytes || bytes.byteLength !== size) return 0;
    Module.HEAPU8.set(bytes, offset);
    return 1;
  };

  const STEP_INFO_SIZE = 96;
  const MAX_COMPOSITION_ERRORS = 20;

  // Two-phase bounded copy: `query(ptr, cap)` returns the byte count.
  function copyCountedString(label, query) {
    const size = withPointerFallback(query, 0, 0);
    if (size < 0) throw new RangeError(`${label}: invalid result string`);
    if (size === 0) return '';
    const ptr = Module['_lightusd_combined_alloc'](size);
    if (!ptr) throw new RangeError(`${label}: string allocation failed`);
    try {
      if (withPointerFallback(query, ptr, size) !== size) {
        throw new RangeError(`${label}: result changed`);
      }
      return heapString(Number(ptr), size);
    } finally {
      Module['_lightusd_combined_free'](ptr);
    }
  }

  function copyFlattenString(kind, index) {
    return copyCountedString('next flatten', (ptr, cap) =>
      Module['_lightusd_combined_next_flatten_string'](kind, index, ptr, cap));
  }

  // Runs one callback-driven flatten call. `inputs` are byte arrays copied
  // into the heap; `invoke(ptrs, infoPtr)` receives their pointers (numbers,
  // or BigInts on the memory64 retry) and returns the C status. Returns the
  // decoded step record, or rethrows a callback exception.
  function runFlattenStep(label, loaderPtr, inputs, ctx, invoke) {
    const ptrs = [];
    let infoPtr = 0;
    try {
      for (const bytes of inputs) {
        // Empty inputs cross as null pointers; C rejects a non-null empty map.
        if (!bytes.length) {
          ptrs.push(0);
          continue;
        }
        const ptr = Module['_lightusd_combined_alloc'](bytes.length);
        if (!ptr) throw new RangeError(`${label}: allocation failed`);
        ptrs.push(ptr);
        Module.HEAPU8.set(bytes, Number(ptr));
      }
      infoPtr = Module['_lightusd_combined_alloc'](STEP_INFO_SIZE);
      if (!infoPtr) throw new RangeError(`${label}: allocation failed`);
      new DataView(Module.HEAPU8.buffer, Number(infoPtr), STEP_INFO_SIZE)
        .setUint32(0, STEP_INFO_SIZE, true);
      let status;
      try {
        status = invoke(loaderPtr, ptrs, infoPtr);
      } catch (error) {
        if (!(error instanceof TypeError) || ctx.errorSet) throw error;
        const asPointer = (pointer) => typeof pointer === 'bigint'
          ? pointer : BigInt(pointer);
        status = invoke(asPointer(loaderPtr), ptrs.map(asPointer), asPointer(infoPtr));
      }
      if (ctx.errorSet) throw ctx.error;
      if (status < 0) throw new TypeError(`${label}: invalid receiver`);
      const info = new DataView(Module.HEAPU8.buffer, Number(infoPtr), STEP_INFO_SIZE);
      const step = {
        status: info.getInt32(4, true),
        dataSize: info.getUint32(8, true),
        assetPathCount: info.getUint32(12, true),
        compositionErrorCount: info.getUint32(16, true),
        stats: {
          inputBytes: info.getFloat64(24, true),
          outputBytes: info.getFloat64(32, true),
          primCount: info.getFloat64(40, true),
          arraysPassedThrough: info.getFloat64(48, true),
          arraysReencoded: info.getFloat64(56, true),
          assetPathsRemapped: info.getFloat64(64, true),
          readMs: info.getFloat64(72, true),
          composeMs: info.getFloat64(80, true),
          writeMs: info.getFloat64(88, true)
        }
      };
      if (step.status <= 0) step.error = copyError();
      if (step.status === 2) step.key = copyFlattenString(0, 0);
      if (step.status === 3) {
        if (step.dataSize) step.data = copyFlattenOutput(step.dataSize);
        step.assetPaths = [];
        for (let i = 0; i < step.assetPathCount; ++i) {
          step.assetPaths.push(copyFlattenString(1, i));
        }
        step.compositionErrors = [];
        const errorCount = Math.min(step.compositionErrorCount, MAX_COMPOSITION_ERRORS);
        for (let i = 0; i < errorCount; ++i) {
          step.compositionErrors.push(copyFlattenString(2, i));
        }
      }
      return step;
    } finally {
      Module['_lightusd_combined_next_flatten_release']();
      for (const id of ctx.ids) flattenCallbacks.delete(id);
      if (infoPtr) Module['_lightusd_combined_free'](infoPtr);
      for (const ptr of ptrs) if (ptr) Module['_lightusd_combined_free'](ptr);
    }
  }

  function requireCallback(label, callback, optional) {
    if (optional && callback == null) return;
    if (typeof callback !== 'function') {
      throw new TypeError(`${label}: expected callback${optional ? ' or null' : ''}`);
    }
  }

  function flattenBufferToSink(uuid, lazyArrays, chunkCb, remap, variants) {
    const label = 'nextFlattenBufferToSink';
    const loaderPtr = requireLiveLoader(this);
    if (arguments.length < 3 || arguments.length > 5) {
      throw new TypeError(`${label}: wrong argument count`);
    }
    if (typeof uuid !== 'string') throw new TypeError(`${label}: expected buffer UUID`);
    requireCallback(label, chunkCb, false);
    const uuidBytes = new TextEncoder().encode(uuid);
    const remapBytes = encodeStringMap(remap);
    const variantBytes = encodeStringMap(variants, true);
    const ctx = {ids: [], error: null, errorSet: false};
    const sinkId = registerFlattenCallback(ctx, chunkCb);
    const step = runFlattenStep(label, loaderPtr,
      [uuidBytes, remapBytes, variantBytes], ctx,
      (loader, [uuidPtr, remapPtr, variantPtr], infoPtr) =>
        Module['_lightusd_combined_next_flatten_to_sink'](
          loader, uuidPtr, uuidBytes.length, Number(!!lazyArrays),
          sinkId, remapPtr, remapBytes.length, variantPtr, variantBytes.length,
          infoPtr));
    if (step.status !== 3) return {success: false, error: step.error};
    return Object.assign({success: true}, step.stats);
  }

  function flattenMultiBuffer(uuid, rootName, lazyArrays, chunkCb,
                              layerExistsCb, layerFetchCb, remap, variants) {
    const label = 'nextFlattenMultiBufferToSink';
    const loaderPtr = requireLiveLoader(this);
    if (arguments.length < 4 || arguments.length > 8) {
      throw new TypeError(`${label}: wrong argument count`);
    }
    if (typeof uuid !== 'string' || typeof rootName !== 'string') {
      throw new TypeError(`${label}: expected string names`);
    }
    requireCallback(label, chunkCb, true);
    requireCallback(label, layerExistsCb, true);
    requireCallback(label, layerFetchCb, true);
    const encoder = new TextEncoder();
    const uuidBytes = encoder.encode(uuid);
    const rootBytes = encoder.encode(rootName);
    const remapBytes = encodeStringMap(remap);
    const variantBytes = encodeStringMap(variants, true);
    const ctx = {ids: [], error: null, errorSet: false};
    const sinkId = registerFlattenCallback(ctx, chunkCb);
    const existsId = registerFlattenCallback(ctx, layerExistsCb);
    const fetchId = registerFlattenCallback(ctx, layerFetchCb);
    const step = runFlattenStep(label, loaderPtr,
      [uuidBytes, rootBytes, remapBytes, variantBytes], ctx,
      (loader, [uuidPtr, rootPtr, remapPtr, variantPtr], infoPtr) =>
        Module['_lightusd_combined_next_flatten_multi'](
          loader, uuidPtr, uuidBytes.length, rootPtr, rootBytes.length,
          Number(!!lazyArrays), sinkId, existsId, fetchId,
          remapPtr, remapBytes.length, variantPtr, variantBytes.length, infoPtr));
    if (step.status !== 3) return {success: false, error: step.error};
    const result = {success: true};
    if (!sinkId) result.data = step.data || new Uint8Array(0);
    return Object.assign(result, step.stats, {
      assetPaths: step.assetPaths,
      assetPathCount: step.assetPathCount,
      compositionErrors: step.compositionErrors,
      compositionErrorCount: step.compositionErrorCount
    });
  }

  function stepFlattenSession(session, chunkCb) {
    const label = 'nextFlattenAsyncStep';
    const loaderPtr = requireLiveLoader(this);
    if (arguments.length !== 2) throw new TypeError(`${label}: wrong argument count`);
    if (typeof session !== 'string') throw new TypeError(`${label}: expected session ID`);
    requireCallback(label, chunkCb, true);
    const sessionBytes = new TextEncoder().encode(session);
    const ctx = {ids: [], error: null, errorSet: false};
    const sinkId = registerFlattenCallback(ctx, chunkCb);
    const step = runFlattenStep(label, loaderPtr, [sessionBytes], ctx,
      (loader, [sessionPtr], infoPtr) =>
        Module['_lightusd_combined_next_flatten_async_step'](
          loader, sessionPtr, sessionBytes.length, sinkId, infoPtr));
    switch (step.status) {
      case -1: return {success: false, error: step.error};
      case 0: return {success: false, status: 'error', error: step.error};
      case 1: return {success: true, status: 'ready'};
      case 2: return {success: true, status: 'need-layer', key: step.key};
      case 3: break;
      default: throw new RangeError(`${label}: invalid status`);
    }
    const result = {success: true, status: 'done'};
    if (!sinkId) result.data = step.data || new Uint8Array(0);
    return Object.assign(result, step.stats, {
      assetPaths: step.assetPaths,
      assetPathCount: step.assetPathCount
    });
  }

  // Calls `invoke` with the given pointers, retrying with BigInts when the
  // memory64 build rejects numbers.
  function invokeWithPointers(invoke, pointers) {
    try {
      return invoke(...pointers);
    } catch (error) {
      if (!(error instanceof TypeError)) throw error;
      return invoke(...pointers.map((pointer) => typeof pointer === 'bigint'
        ? pointer : BigInt(pointer)));
    }
  }

  const LAYER_OPS = [
    ['hasSublayers', 0], ['hasReferences', 1], ['hasPayload', 2],
    ['hasInherits', 3], ['hasVariants', 4], ['composeSublayers', 5],
    ['composeReferences', 6], ['composePayload', 7], ['composeInherits', 8],
    ['composeVariants', 9], ['lodVariantCount', 10]
  ];
  for (const [name, op] of LAYER_OPS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function() {
        const loaderPtr = requireLiveLoader(this);
        if (arguments.length !== 0) throw new TypeError(`${name}: wrong argument count`);
        const result = invokeWithPointers(
          (loader) => Module['_lightusd_combined_layer_op'](loader, op), [loaderPtr]);
        if (result < 0) throw new TypeError(`${name}: invalid receiver`);
        return name === 'lodVariantCount' ? result : result === 1;
      }
    });
  }

  function applyVariantSelection(primPathOrVariant, variantSet, variant) {
    const label = 'applyVariantSelection';
    const loaderPtr = requireLiveLoader(this);
    const global = arguments.length === 1;
    if (!global && arguments.length !== 3) {
      throw new TypeError(`${label}: wrong argument count`);
    }
    const args = global ? [primPathOrVariant] : [primPathOrVariant, variantSet, variant];
    if (!args.every((value) => typeof value === 'string')) {
      throw new TypeError(`${label}: expected string names`);
    }
    const encoder = new TextEncoder();
    const bytes = args.map((value) => encoder.encode(value));
    const ptrs = [];
    try {
      for (const value of bytes) {
        const ptr = value.length ? Module['_lightusd_combined_alloc'](value.length) : 0;
        if (value.length && !ptr) throw new RangeError(`${label}: allocation failed`);
        if (ptr) Module.HEAPU8.set(value, Number(ptr));
        ptrs.push(ptr);
      }
      const result = global
        ? invokeWithPointers((loader, name) =>
          Module['_lightusd_combined_apply_global_variant_selection'](
            loader, name, bytes[0].length), [loaderPtr, ptrs[0]])
        : invokeWithPointers((loader, path, set, name) =>
          Module['_lightusd_combined_apply_variant_selection'](
            loader, path, bytes[0].length, set, bytes[1].length, name,
            bytes[2].length), [loaderPtr, ...ptrs]);
      if (result < 0) throw new TypeError(`${label}: invalid receiver`);
      return result === 1;
    } finally {
      for (const ptr of ptrs) if (ptr) Module['_lightusd_combined_free'](ptr);
    }
  }

  // Reads one layer string table; see lightusd_combined_layer_strings.
  function readLayerStrings(label, loaderPtr, kind) {
    let sizePtr = 0, shapePtr = 0;
    try {
      sizePtr = Module['_lightusd_combined_alloc'](4);
      if (!sizePtr) throw new RangeError(`${label}: allocation failed`);
      const count = invokeWithPointers((loader, size) =>
        Module['_lightusd_combined_layer_strings'](loader, kind, size),
      [loaderPtr, sizePtr]);
      if (count < 0) throw new TypeError(`${label}: invalid receiver`);
      const shapeSize = new DataView(Module.HEAPU8.buffer, Number(sizePtr), 4)
        .getUint32(0, true);
      const strings = [];
      for (let i = 0; i < count; ++i) {
        strings.push(copyCountedString(label, (p, cap) =>
          Module['_lightusd_combined_table_string'](i, p, cap)));
      }
      let shape = new Uint32Array(0);
      if (shapeSize) {
        shapePtr = Module['_lightusd_combined_alloc'](shapeSize * 4);
        if (!shapePtr) throw new RangeError(`${label}: allocation failed`);
        if (withPointerFallback((p) => Module['_lightusd_combined_table_shape'](p, shapeSize),
          shapePtr) !== 0) {
          throw new RangeError(`${label}: shape changed`);
        }
        shape = new Uint32Array(Module.HEAPU8.slice(
          Number(shapePtr), Number(shapePtr) + shapeSize * 4).buffer);
      }
      return {strings, shape};
    } finally {
      Module['_lightusd_combined_table_release']();
      if (shapePtr) Module['_lightusd_combined_free'](shapePtr);
      if (sizePtr) Module['_lightusd_combined_free'](sizePtr);
    }
  }

  for (const [name, kind] of [['extractSublayerAssetPaths', 0],
    ['extractReferencesAssetPaths', 1], ['extractPayloadAssetPaths', 2]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function() {
        const loaderPtr = requireLiveLoader(this);
        if (arguments.length !== 0) throw new TypeError(`${name}: wrong argument count`);
        return readLayerStrings(name, loaderPtr, kind).strings;
      }
    });
  }

  function extractVariants() {
    const label = 'extractVariants';
    const loaderPtr = requireLiveLoader(this);
    if (arguments.length !== 0) throw new TypeError(`${label}: wrong argument count`);
    const {strings, shape} = readLayerStrings(label, loaderPtr, 3);
    const prims = [];
    let s = 0, k = 0;
    while (s < strings.length) {
      if (k >= shape.length) throw new RangeError(`${label}: invalid shape`);
      const prim = {primPath: strings[s++], variantSets: []};
      const setCount = shape[k++];
      for (let i = 0; i < setCount; ++i) {
        if (k >= shape.length || s + 2 > strings.length) {
          throw new RangeError(`${label}: invalid shape`);
        }
        const set = {name: strings[s++], selection: strings[s++], options: []};
        const optionCount = shape[k++];
        if (s + optionCount > strings.length) throw new RangeError(`${label}: invalid shape`);
        set.options = strings.slice(s, s + optionCount);
        s += optionCount;
        prim.variantSets.push(set);
      }
      prims.push(prim);
    }
    if (k !== shape.length) throw new RangeError(`${label}: invalid shape`);
    return prims;
  }

  // Loader configuration. Argument conversion follows Embind: bools use JS
  // truthiness; numbers accept number or boolean, integers throw outside
  // their C range and truncate (NaN becomes 0).
  function configNumber(label, value) {
    if (typeof value !== 'number' && typeof value !== 'boolean') {
      throw new TypeError(`${label}: expected a number`);
    }
    return Number(value);
  }

  function configInteger(label, value, min, max) {
    const number = configNumber(label, value);
    if (number < min || number > max) throw new TypeError(`${label}: value out of range`);
    return Math.trunc(number) || 0;
  }

  // Match std::string's UTF-8/string and one-byte-view inputs. Copy heap-backed
  // views before allocating, since an allocation may grow/detach WASM memory.
  function streamBytes(label, value) {
    if (typeof value === 'string') return new TextEncoder().encode(value);
    if (value instanceof ArrayBuffer) value = new Uint8Array(value);
    if (!ArrayBuffer.isView(value) || value.BYTES_PER_ELEMENT !== 1) {
      throw new TypeError(`${label}: expected a string or byte view`);
    }
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
  }

  function withStreamBytes(label, values, callback) {
    const bytes = values.map(value => streamBytes(label, value));
    const pointers = [];
    try {
      for (const data of bytes) {
        if (data.length > 0xffffffff) throw new RangeError(`${label}: input too large`);
        const ptr = Module['_lightusd_combined_alloc'](Math.max(data.length, 1));
        if (!ptr) throw new RangeError(`${label}: allocation failed`);
        pointers.push(ptr);
        Module.HEAPU8.set(data, Number(ptr));
      }
      return callback(pointers, bytes.map(data => data.length));
    } finally {
      for (const ptr of pointers) Module['_lightusd_combined_free'](ptr);
    }
  }

  function streamOp(loaderPtr, op, keyPtr = 0, keySize = 0,
    dataPtr = 0, dataSize = 0, value = 0) {
    return invokeWithPointers((loader, key, data) =>
      Module['_lightusd_combined_stream_op'](
        loader, op, key, keySize, data, dataSize, value),
    [loaderPtr, keyPtr, dataPtr]);
  }

  const STREAM_OPS = [
    ['startStreamingAsset', 0, 2, 'size'],
    ['appendAssetChunk', 1, 2, 'bytes'],
    ['finalizeStreamingAsset', 2, 1],
    ['isStreamingAssetComplete', 3, 1],
    ['getZeroCopyBufferPtr', 4, 1, 'pointer'],
    ['getZeroCopyBufferPtrAtOffset', 5, 2, 'size-pointer'],
    ['markZeroCopyBytesWritten', 6, 2, 'size'],
    ['finalizeZeroCopyBuffer', 7, 1],
    ['cancelZeroCopyBuffer', 8, 1],
    ['setMMapZeroCopy', 9, 1, 'bool'],
    ['getMMapZeroCopy', 10, 0]
  ];
  for (const [name, op, argc, type] of STREAM_OPS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const loader = requireLiveLoader(this);
        if (args.length !== argc) throw new TypeError(`${name}: wrong argument count`);
        const scalar = type === 'bool' ? Number(!!args[0])
          : type === 'size' || type === 'size-pointer'
            ? exactSize(name, loader, args[1]) : 0;
        const strings = op >= 9 ? [] : type === 'bytes' ? args : [args[0]];
        const result = withStreamBytes(name, strings, (ptrs, sizes) => {
          if (type === 'size' || type === 'size-pointer') {
            const [low, high] = sizeWords(scalar);
            return invokeWithPointers((handle, key) =>
              Module['_lightusd_combined_stream_size_op'](
                handle, op, key, sizes[0], low, high), [loader, ptrs[0]]);
          }
          return streamOp(loader, op, ptrs[0] || 0, sizes[0] || 0,
            ptrs[1] || 0, sizes[1] || 0, scalar);
        });
        if (result < 0) throw new TypeError(`${name}: invalid argument`);
        if (op === 9) return;
        return type === 'pointer' || type === 'size-pointer' ? result : result === 1;
      }
    });
  }

  function streamTableString(label, index) {
    return copyCountedString(label, (ptr, cap) =>
      Module['_lightusd_combined_table_string'](index, ptr, cap));
  }

  function streamInfo(label, loader, kind, key, size = 0, maxBytes = 0) {
    return withStreamBytes(label, [key], ([keyPtr], [keySize]) => {
      let out = 0;
      try {
        out = Module['_lightusd_combined_alloc'](40);
        if (!out) throw new RangeError(`${label}: allocation failed`);
        new DataView(Module.HEAPU8.buffer, Number(out), 40).setUint32(0, 40, true);
        const result = invokeWithPointers((handle, name, record) => {
          if (kind === 2) {
            const [low, high] = sizeWords(size);
            const [maxLow, maxHigh] = sizeWords(maxBytes);
            return Module['_lightusd_combined_stream_allocate'](
              handle, name, keySize, low, high, maxLow, maxHigh, record);
          }
          return Module['_lightusd_combined_stream_info_get'](
            handle, kind, name, keySize, 0, 0, record);
        }, [loader, keyPtr, out]);
        if (result < 0) throw new TypeError(`${label}: invalid argument`);
        if (!result) return kind === 2
          ? {success: false, error: streamTableString(label, 0)} : {exists: false};
        const view = new DataView(Module.HEAPU8.buffer, Number(out), 40);
        const flags = view.getUint32(4, true);
        const total = view.getFloat64(8, true);
        const current = view.getFloat64(16, true);
        const progress = view.getFloat64(24, true);
        const bufferPtr = view.getFloat64(32, true);
        const uuid = streamTableString(label, 0);
        if (kind === 0) return {exists: true, current, total,
          complete: !!(flags & 1), uuid, percentage: progress};
        const assetName = streamTableString(label, 1);
        if (kind === 2) return {success: true, uuid, assetName,
          bufferPtr, totalSize: total};
        return {exists: true, uuid, assetName, totalSize: total,
          bytesWritten: current, progress, isComplete: !!(flags & 1),
          finalized: !!(flags & 2), bufferPtr};
      } finally {
        Module['_lightusd_combined_table_release']();
        if (out) Module['_lightusd_combined_free'](out);
      }
    });
  }

  for (const [name, kind] of [['getStreamingProgress', 0],
    ['getZeroCopyProgress', 1], ['allocateZeroCopyBuffer', 2]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const loader = requireLiveLoader(this);
        if (args.length !== (kind === 2 ? 3 : 1)) {
          throw new TypeError(`${name}: wrong argument count`);
        }
        return streamInfo(name, loader, kind, args[0],
          kind === 2 ? exactSize(name, loader, args[1]) : 0,
          kind === 2 ? exactSize(name, loader, args[2]) : 0);
      }
    });
  }

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getActiveZeroCopyBuffers', {
    configurable: true,
    value: function() {
      const label = 'getActiveZeroCopyBuffers';
      const loader = requireLiveLoader(this);
      if (arguments.length) throw new TypeError(`${label}: wrong argument count`);
      const keys = [];
      try {
        const count = streamOp(loader, 11);
        if (count < 0) throw new TypeError(`${label}: invalid receiver`);
        for (let i = 0; i < count; ++i) keys.push(streamTableString(label, i));
      } finally {
        Module['_lightusd_combined_table_release']();
      }
      return keys.map(uuid => {
        const {exists, ...info} = streamInfo(label, loader, 1, uuid);
        if (!exists) throw new RangeError(`${label}: buffer disappeared`);
        return {uuid, info};
      });
    }
  });

  function memory64Loader(loader) {
    return streamOp(loader, 12) > 0xffffffff;
  }

  // size_t/uintptr_t use BigInt on memory64 under Embind. Preserve both the
  // full unsigned range and the accepted number-or-BigInt input types.
  function exactSize(label, loader, value) {
    if (!memory64Loader(loader)) return configInteger(label, value, 0, 0xffffffff);
    if (typeof value !== 'number' && typeof value !== 'bigint') {
      throw new TypeError(`${label}: expected a number or BigInt`);
    }
    const integer = BigInt(value); // Rejects fractional/NaN/infinite numbers.
    if (integer < 0n || integer > 0xffffffffffffffffn) {
      throw new TypeError(`${label}: value out of range`);
    }
    return integer;
  }

  function sizeWords(value) {
    const integer = BigInt(value);
    return [Number(integer & 0xffffffffn), Number(integer >> 32n)];
  }

  const ASSET_OPS = [
    ['setAsset', 0, 2, 2, 'void'],
    ['hasAsset', 1, 1, 1, 'bool'],
    ['deleteAsset', 2, 1, 1, 'bool'],
    ['deleteAssetByUUID', 3, 1, 1, 'bool'],
    ['deleteAssetByName', 4, 1, 1, 'bool'],
    ['assetExists', 9, 1, 1, 'bool'],
    ['clearAssets', 10, 0, 0, 'void'],
    ['setAllowParentRelativeAssetPaths', 11, 1, 0, 'void'],
    ['getAllowParentRelativeAssetPaths', 12, 0, 0, 'bool'],
    ['setBaseWorkingPath', 13, 1, 1, 'void'],
    ['clearAssetSearchPaths', 14, 0, 0, 'void'],
    ['addAssetSearchPath', 15, 1, 1, 'void'],
    ['verifyAssetHash', 16, 2, 2, 'bool']
  ];
  for (const [name, op, argc, stringCount, resultType] of ASSET_OPS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const loaderPtr = requireLiveLoader(this);
        if (args.length !== argc) throw new TypeError(`${name}: wrong argument count`);
        const scalar = op === 11 ? Number(!!args[0]) : 0;
        const result = withStreamBytes(name, args.slice(0, stringCount), (ptrs, sizes) =>
          invokeWithPointers((loader, key, data) =>
            Module['_lightusd_combined_asset_op'](loader, op, key, sizes[0] || 0,
              data, sizes[1] || 0, scalar), [loaderPtr, ptrs[0] || 0, ptrs[1] || 0]));
        if (result < 0) throw new TypeError(`${name}: invalid argument`);
        if (resultType === 'bool') return result === 1;
      }
    });
  }

  for (const [name, op] of [['getAssetCount', 5], ['getAssetCacheSizeBytes', 6],
    ['setAssetCacheMaxSizeBytes', 7], ['getAssetCacheMaxSizeBytes', 8]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const loader = requireLiveLoader(this);
        if (args.length !== (op === 7 ? 1 : 0)) {
          throw new TypeError(`${name}: wrong argument count`);
        }
        const [low, high] = op === 7 ? sizeWords(exactSize(name, loader, args[0])) : [0, 0];
        let out = 0;
        try {
          if (op !== 7) {
            out = Module['_lightusd_combined_alloc'](8);
            if (!out) throw new RangeError(`${name}: allocation failed`);
          }
          const status = invokeWithPointers((handle, record) =>
            Module['_lightusd_combined_asset_size_op'](handle, op, low, high, record),
          [loader, out]);
          if (status !== 0) throw new TypeError(`${name}: invalid argument`);
          if (op === 7) return;
          const view = new DataView(Module.HEAPU8.buffer, Number(out), 8);
          const value = BigInt(view.getUint32(0, true)) |
            (BigInt(view.getUint32(4, true)) << 32n);
          return memory64Loader(loader) ? value : Number(value);
        } finally {
          if (out) Module['_lightusd_combined_free'](out);
        }
      }
    });
  }

  const ASSET_STRINGS = [
    ['getAssetHash', 0, 1], ['getAssetUUID', 1, 1],
    ['getStreamingAssetUUID', 2, 1], ['findAssetByUUID', 3, 1],
    ['getBaseWorkingPath', 4, 0], ['getAssetSearchPaths', 5, 0],
    ['getAllAssetUUIDs', 6, 0]
  ];
  for (const [name, kind, argc] of ASSET_STRINGS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const loaderPtr = requireLiveLoader(this);
        if (args.length !== argc) throw new TypeError(`${name}: wrong argument count`);
        return withStreamBytes(name, args, (ptrs, sizes) => {
          try {
            const count = invokeWithPointers((loader, key) =>
              Module['_lightusd_combined_asset_strings'](loader, kind, key, sizes[0] || 0),
            [loaderPtr, ptrs[0] || 0]);
            if (count < 0) throw new TypeError(`${name}: invalid argument`);
            const strings = [];
            for (let i = 0; i < count; ++i) strings.push(streamTableString(name, i));
            if (kind < 5) return strings[0];
            if (kind === 5) return strings;
            if (count % 2) throw new RangeError(`${name}: invalid UUID table`);
            const result = {};
            for (let i = 0; i < count; i += 2) result[strings[i]] = strings[i + 1];
            return result;
          } finally {
            Module['_lightusd_combined_table_release']();
          }
        });
      }
    });
  }

  for (const [name, byUUID, borrowed] of [['getAsset', 0, false],
    ['getAssetByUUID', 1, false], ['getAssetCacheDataAsMemoryView', 0, true]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(key) {
        const loaderPtr = requireLiveLoader(this);
        if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
        return withStreamBytes(name, [key], ([keyPtr], [keySize]) => {
          let out = 0;
          try {
            out = Module['_lightusd_combined_alloc'](24);
            if (!out) throw new RangeError(`${name}: allocation failed`);
            new DataView(Module.HEAPU8.buffer, Number(out), 24).setUint32(0, 24, true);
            const result = invokeWithPointers((loader, keyData, record) =>
              Module['_lightusd_combined_asset_info_get'](loader, byUUID, keyData, keySize, record),
            [loaderPtr, keyPtr, out]);
            if (result < 0) throw new TypeError(`${name}: invalid argument`);
            if (!result) {
              if (borrowed) return undefined;
              return byUUID ? {error: streamTableString(name, 0)} : {};
            }
            const view = new DataView(Module.HEAPU8.buffer, Number(out), 24);
            const size = view.getFloat64(8, true);
            const ptr = view.getFloat64(16, true);
            if (borrowed) return new Uint8Array(Module.HEAPU8.buffer, ptr, size);
            const assetName = streamTableString(name, 0);
            const sha256 = streamTableString(name, 1);
            const uuid = streamTableString(name, 2);
            return {name: assetName, data: Module.HEAPU8.slice(ptr, ptr + size), sha256, uuid};
          } finally {
            Module['_lightusd_combined_table_release']();
            if (out) Module['_lightusd_combined_free'](out);
          }
        });
      }
    });
  }

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'setAssetFromRawPointer', {
    configurable: true,
    value: function(key, pointer, size) {
      const label = 'setAssetFromRawPointer';
      const loaderPtr = requireLiveLoader(this);
      if (arguments.length !== 3) throw new TypeError(`${label}: wrong argument count`);
      const rawPointer = exactSize(label, loaderPtr, pointer);
      const [low, high] = sizeWords(exactSize(label, loaderPtr, size));
      return withStreamBytes(label, [key], ([keyPtr], [keySize]) => {
        const result = invokeWithPointers((loader, keyData, data) =>
          Module['_lightusd_combined_asset_set_raw'](loader, keyData, keySize, data, low, high),
        [loaderPtr, keyPtr, rawPointer]);
        if (result < 0) throw new TypeError(`${label}: invalid argument`);
        return result === 1;
      });
    }
  });

  // Keep user callback exceptions outside native frames and pointer conversion.
  const loadingFrames = [];
  Module['__lightusdLoadingCallback'] = function(name, event) {
    const frame = loadingFrames[loadingFrames.length - 1];
    if (!frame) return Module[name](event);
    if (frame.failed) return;
    try { Module[name](event); }
    catch (error) { frame.failed = true; frame.error = error; }
  };
  function loadingCall(invoke, pointers, exactPointers = false) {
    const frame = {failed: false};
    loadingFrames.push(frame);
    try {
      const result = exactPointers ? invoke(...pointers) : invokeWithPointers(invoke, pointers);
      if (frame.failed) throw frame.error;
      return result;
    } finally { loadingFrames.pop(); }
  }
  const LOADING_OPS = [
    ['loadFromBinary', 0, 2], ['loadAsLayerFromBinary', 1, 2],
    ['loadFromBinaryWithProgress', 2, 2], ['loadAsLayerFromBinaryWithProgress', 3, 2],
    ['loadFromCachedAsset', 4, 1], ['loadAsLayerFromCachedAsset', 5, 1],
    ['loadLayerFromJSON', 6, 1], ['loadTest', 7, 2],
    ['cancelParsing', 8, 0, 'void'], ['wasCancelled', 9, 0],
    ['isParsingInProgress', 10, 0], ['resetProgress', 11, 0, 'void'],
    ['releaseSourceLayer', 12, 0, 'void'], ['reset', 13, 0, 'void'],
    ['ok', 14, 0], ['error', 15, 0, 'string'], ['warn', 16, 0, 'string'],
    ['validateFromBinary', 17, 3, 'string'], ['validateLoadedLayer', 18, 1, 'string']
  ];
  for (const [name, op, argc, resultType] of LOADING_OPS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const ptr = requireLiveLoader(this);
        if (args.length !== argc) throw new TypeError(`${name}: wrong argument count`);
        if (op === 7 && ArrayBuffer.isView(args[1])) {
          const view = args[1];
          args[1] = view.byteLength > 1024 * 1024 * 1024 ? new Uint8Array(0)
            : new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
        }
        const owner = this.clone();
        try {
          return withStreamBytes(name, args, (p, sizes) => {
            const result = loadingCall((loader, a, b, c) =>
              Module['_lightusd_combined_loading_op'](loader, op,
                a, sizes[0] || 0, b, sizes[1] || 0, c, sizes[2] || 0),
            [ptr, p[0] || 0, p[1] || 0, p[2] || 0]);
            if (result < 0) throw new TypeError(`${name}: invalid argument`);
            if (resultType === 'string') return streamTableString(name, 0);
            return resultType === 'void' ? undefined : result === 1;
          });
        } finally {
          if (resultType === 'string') Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  }
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getProgress', {
    configurable: true,
    value: function() {
      const ptr = requireLiveLoader(this);
      if (arguments.length) throw new TypeError('getProgress: wrong argument count');
      const out = Module['_lightusd_combined_alloc'](72);
      if (!out) throw new RangeError('getProgress: allocation failed');
      try {
        new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 72, true);
        if (invokeWithPointers((loader, record) =>
          Module['_lightusd_combined_loading_progress_get'](loader, record), [ptr, out]) < 0)
          throw new TypeError('getProgress: invalid record');
        const view = new DataView(Module.HEAPU8.buffer, Number(out), 72);
        const result = {cancelRequested: Boolean(view.getUint32(4, true) & 1)};
        ['progress', 'percentage', 'bytesProcessed', 'totalBytes', 'meshesProcessed',
          'meshesTotal', 'materialsProcessed', 'materialsTotal'].forEach((name, i) => {
          result[name] = view.getFloat64(8 + i * 8, true);
        });
        ['stage', 'currentOperation', 'errorMessage', 'currentMeshName', 'tydraStage']
          .forEach((name, i) => { result[name] = streamTableString('getProgress', i); });
        return result;
      } finally {
        Module['_lightusd_combined_table_release']();
        Module['_lightusd_combined_free'](out);
      }
    }
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'testValueMemoryUsage', {
    configurable: true,
    value: function(value) {
      const ptr = requireLiveLoader(this);
      if (arguments.length !== 1) throw new TypeError('testValueMemoryUsage: wrong argument count');
      const length = value == null ? 10000 : configInteger('testValueMemoryUsage', value, -2147483648, 2147483647);
      let out = 0;
      try {
        const count = invokeWithPointers(loader =>
          Module['_lightusd_combined_loading_memory_probe'](loader, length), [ptr]);
        if (count < 0) return {success: false, error: 'Memory probe exceeds configured memory budget'};
        out = Module['_lightusd_combined_alloc'](count * 8);
        if (!out) throw new RangeError('testValueMemoryUsage: allocation failed');
        if (withPointerFallback(p => Module['_lightusd_combined_table_shape'](p, count * 2), out) !== 0)
          throw new RangeError('testValueMemoryUsage: invalid shape');
        const view = new DataView(Module.HEAPU8.slice(Number(out), Number(out) + count * 8).buffer);
        const tests = [];
        let total = 0n;
        const wide = memory64Loader(ptr);
        for (let i = 0; i < count; ++i) {
          const bytes = BigInt(view.getUint32(i * 8, true)) + (BigInt(view.getUint32(i * 8 + 4, true)) << 32n);
          total += bytes;
          tests.push({name: streamTableString('testValueMemoryUsage', i), bytes: wide ? bytes : Number(bytes)});
        }
        total = BigInt.asUintN(wide ? 64 : 32, total);
        return {tests, success: true, totalTests: count, arrayLength: length, totalMemory: wide ? total : Number(total)};
      } finally {
        Module['_lightusd_combined_table_release']();
        if (out) Module['_lightusd_combined_free'](out);
      }
    }
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'loadFromBinaryAsync', {
    configurable: true,
    value: function(binary, filename) {
      const ptr = requireLiveLoader(this);
      if (arguments.length !== 2) throw new TypeError('loadFromBinaryAsync: wrong argument count');
      const owner = this.clone();
      let task = 0, out = 0;
      const cleanup = () => {
        if (task) invokeWithPointers(loader => Module['_lightusd_combined_loading_async_end'](loader, task), [ptr]);
        if (out) Module['_lightusd_combined_free'](out);
        owner.delete();
      };
      try {
        task = withStreamBytes('loadFromBinaryAsync', [binary, filename], (p, sizes) =>
          invokeWithPointers((loader, data, name) => Module['_lightusd_combined_loading_async_begin'](
            loader, data, sizes[0], name, sizes[1]), [ptr, ...p]));
        if (!task) throw new RangeError('loadFromBinaryAsync: cannot create task');
        out = Module['_lightusd_combined_alloc'](16);
        if (!out) throw new RangeError('loadFromBinaryAsync: allocation failed');
      } catch (error) { cleanup(); throw error; }
      return (async () => {
        try {
          for (;;) {
            new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 16, true);
            const status = loadingCall((loader, record) =>
              Module['_lightusd_combined_loading_async_step'](loader, task, record), [ptr, out]);
            if (status < 0) throw new TypeError('loadFromBinaryAsync: invalid task');
            if (status === 0) {
              try { return {success: false, error: streamTableString('loadFromBinaryAsync', 0)}; }
              finally { Module['_lightusd_combined_table_release'](); }
            }
            if (status === 2) {
              const view = new DataView(Module.HEAPU8.buffer, Number(out), 16);
              return {success: true, meshCount: view.getUint32(4, true),
                materialCount: view.getUint32(8, true), textureCount: view.getUint32(12, true)};
            }
            await new Promise(resolve => typeof requestAnimationFrame === 'function'
              ? requestAnimationFrame(resolve) : setTimeout(resolve, 0));
          }
        } finally { cleanup(); }
      })();
    }
  });

  const MCP_OPS = [
    ['mcpCreateContext', 0, 1], ['mcpSelectContext', 1, 1],
    ['mcpToolsList', 2, 0], ['mcpToolsCall', 3, 2],
    ['mcpResourcesList', 4, 0], ['mcpResourcesRead', 5, 1]
  ];
  for (const [name, op, argc] of MCP_OPS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const ptr = requireLiveLoader(this);
        if (args.length !== argc) throw new TypeError(`${name}: wrong argument count`);
        // Determine the ABI using a side-effect-free query. Never retry a tool
        // operation if JavaScript called by that operation throws TypeError.
        const pointer = memory64Loader(ptr) ? value => BigInt(value) : value => Number(value);
        const owner = this.clone();
        try {
          return withStreamBytes(name, args, (p, sizes) => {
            const result = Module['_lightusd_combined_mcp_op'](pointer(ptr), op,
              pointer(p[0] || 0), sizes[0] || 0, pointer(p[1] || 0), sizes[1] || 0);
            if (result < 0) throw new TypeError(`${name}: invalid argument`);
            return op < 2 ? result === 1 : streamTableString(name, 0);
          });
        } finally {
          if (op >= 2) Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  }

  const EXPORT_OPS = [
    ['layerToString', 0], ['layerToJSON', 1], ['layerToJSONWithOptions', 2],
    ['exportAsUSDA', 3], ['flattenLayer', 4], ['layerToRenderScene', 5]
  ];
  for (const [name, op] of EXPORT_OPS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const ptr = requireLiveLoader(this);
        if (args.length !== (op === 2 ? 2 : 0)) throw new TypeError(`${name}: wrong argument count`);
        const owner = this.clone();
        try {
          return withStreamBytes(name, op === 2 ? [args[1]] : [], (p, sizes) => {
            const result = loadingCall((loader, mode) =>
              Module['_lightusd_combined_export_op'](loader, op, op === 2 && args[0] ? 1 : 0,
                mode, sizes[0] || 0), [ptr, p[0] || 0]);
            if (result < 0) throw new TypeError(`${name}: invalid argument`);
            return op >= 4 ? result === 1 : streamTableString(name, 0);
          });
        } finally {
          if (op < 4) Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  }
  for (const [name, asLayer] of [['exportAsUSDC', 0], ['exportLayerAsUSDCWithOptions', 1]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function() {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== asLayer) throw new TypeError(`${name}: wrong argument count`);
        const out = Module['_lightusd_combined_alloc'](24);
        if (!out) throw new RangeError(`${name}: allocation failed`);
        const owner = this.clone();
        let result = 0;
        try {
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 24, true);
          loadingCall((loader, record) => {
            result = Module['_lightusd_combined_export_usdc'](loader, asLayer, record);
            return result;
          }, [ptr, out]);
          if (!result) return null;
          const view = new DataView(Module.HEAPU8.buffer, Number(out), 24);
          const size = view.getFloat64(8, true), start = view.getFloat64(16, true);
          return Module.HEAPU8.slice(start, start + size);
        } finally {
          if (result) Module['_lightusd_combined_export_release'](result);
          Module['_lightusd_combined_free'](out);
          owner.delete();
        }
      }
    });
  }

  for (const [name, asLayer] of [['exportStageAsUSDCToBufferWithOptions', 0],
    ['exportLayerAsUSDCToBufferWithOptions', 1]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(buffer, options) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== 2) throw new TypeError(`${name}: wrong argument count`);
        const owner = this.clone();
        let out = 0, result = 0;
        try {
          if (asLayer && invokeWithPointers(loader =>
            Module['_lightusd_combined_export_layer_ready'](loader), [ptr]) !== 1)
            return {success: false, size: 0, error: owner.error()};
          let kind = 0, capacity = 0;
          if (buffer == null) kind = 1;
          else {
            const length = buffer.byteLength;
            if (typeof length !== 'number' || !Number.isFinite(length) || length < 0) kind = 2;
            else capacity = length;
          }
          out = Module['_lightusd_combined_alloc'](24);
          if (!out) throw new RangeError(`${name}: allocation failed`);
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 24, true);
          loadingCall((loader, record) => {
            result = Module['_lightusd_combined_export_usdc_buffer'](loader, asLayer, kind, capacity, record);
            return result;
          }, [ptr, out]);
          if (!result) return {success: false, size: 0, error: owner.error()};
          const view = new DataView(Module.HEAPU8.buffer, Number(out), 24);
          const size = view.getFloat64(8, true), start = view.getFloat64(16, true);
          if (size) buffer.set(Module.HEAPU8.subarray(start, start + size), 0);
          invokeWithPointers((loader, bytes) =>
            Module['_lightusd_combined_export_buffer_finish'](loader, bytes), [ptr, result]);
          return {success: true, size, warn: streamTableString(name, 0)};
        } finally {
          Module['_lightusd_combined_table_release']();
          if (result) Module['_lightusd_combined_export_release'](result);
          if (out) Module['_lightusd_combined_free'](out);
          owner.delete();
        }
      }
    });
  }

  function encodeExportMap(label, map) {
    const pairs = Object.keys(map).map(key => [streamBytes(label, key), streamBytes(label, map[key])]);
    const size = 4 + pairs.reduce((sum, [key, value]) => sum + 8 + key.length + value.length, 0);
    if (size > 0xffffffff) throw new RangeError(`${label}: map too large`);
    const bytes = new Uint8Array(size), view = new DataView(bytes.buffer);
    view.setUint32(0, pairs.length, true);
    let offset = 4;
    for (const [key, value] of pairs) {
      view.setUint32(offset, key.length, true);
      view.setUint32(offset + 4, value.length, true);
      offset += 8;
      bytes.set(key, offset); offset += key.length;
      bytes.set(value, offset); offset += value.length;
    }
    return bytes;
  }
  function optimizePackage(label, ptr, options, kind) {
    if (options == null) return true;
    let mode = kind === 0 ? options.optimizeMaterials : options.optimizeGeometry;
    if (mode == null) mode = kind === 0 ? options.materialOptimization : options.optimizeMeshes;
    if (mode == null && kind === 1) mode = options.geometryOptimization;
    const bytes = mode == null ? new Uint8Array(0) : streamBytes(label, mode);
    const name = decoder.decode(bytes).toLowerCase();
    const active = kind === 0 ? ['dedupe', 'dedup', 'preview', 'previewsurface', 'usdpreviewsurface', 'atlas']
                             : ['mergemeshes', 'merge', 'meshmerge'];
    const fields = kind === 0
      ? ['materialAtlasSize', 'materialAtlasTileSize', 'materialAtlasPadding', 'materialAtlasMinGroupSize']
      : ['meshMergeMaxInputFaces', 'meshMergeMaxInputPoints', 'meshMergeMaxAggregateFaces', 'meshMergeMinGroupSize'];
    let present = 0;
    const values = [0, 0, 0, 0];
    if (active.includes(name)) fields.forEach((field, i) => {
      const value = options[field];
      if (value != null) {
        values[i] = configInteger(label, value, -2147483648, 2147483647);
        present |= 1 << i;
      }
    });
    const record = Module['_lightusd_combined_alloc'](24);
    if (!record) throw new RangeError(`${label}: allocation failed`);
    try {
      return withStreamBytes(label, [bytes], ([modePtr], [length]) => {
        const view = new DataView(Module.HEAPU8.buffer, Number(record), 24);
        view.setUint32(0, 24, true); view.setUint32(4, present, true);
        values.forEach((value, i) => view.setInt32(8 + 4 * i, value, true));
        const result = loadingCall((loader, data, opts) =>
          Module['_lightusd_combined_export_optimize'](loader, kind, data, length, opts),
        [ptr, modePtr, record]);
        if (result < 0) throw new TypeError(`${label}: invalid optimization record`);
        return result === 1;
      });
    } finally { Module['_lightusd_combined_free'](record); }
  }
  const PACKAGE_OPS = [
    ['exportAsUSDZ', 0, 0], ['exportAsUSDZWithRemap', 1, 1],
    ['exportAsUSDZWithOptions', 2, 2], ['exportLayerAsUSDZWithOptions', 3, 1]
  ];
  for (const [name, kind, argc] of PACKAGE_OPS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const ptr = requireLiveLoader(this);
        if (args.length !== argc) throw new TypeError(`${name}: wrong argument count`);
        const owner = this.clone();
        const asLayer = kind === 3;
        let task = 0, out = 0;
        try {
          const options = kind === 2 ? args[1] : asLayer ? args[0] : null;
          if (asLayer && invokeWithPointers(loader =>
            Module['_lightusd_combined_export_layer_ready'](loader), [ptr]) !== 1) return null;
          if (!optimizePackage(name, ptr, options, 0) || !optimizePackage(name, ptr, options, 1)) return null;
          loadingCall(loader => {
            task = Module['_lightusd_combined_package_begin'](loader, asLayer ? 1 : 0);
            return task;
          }, [ptr]);
          if (!task) return null;
          const remap = encodeExportMap(name, kind === 1 ? args[0]
            : kind === 2 && args[0] != null ? args[0] : {});
          let arkit = false, format = '';
          if (options != null) {
            if (!asLayer) arkit = !!options.arkitCompatible;
            const value = options.rootLayerFormat;
            if (value != null) format = value;
          }
          out = Module['_lightusd_combined_alloc'](24);
          if (!out) throw new RangeError(`${name}: allocation failed`);
          return withStreamBytes(name, [remap, format], (p, sizes) => {
            new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 24, true);
            const result = loadingCall((loader, state, paths, root, record) =>
              Module['_lightusd_combined_package_write'](loader, state, paths, sizes[0],
                root, sizes[1], arkit ? 1 : 0, record), [ptr, task, p[0], p[1], out]);
            if (result < 0) throw new TypeError(`${name}: invalid package request`);
            if (!result) return null;
            const view = new DataView(Module.HEAPU8.buffer, Number(out), 24);
            return new Uint8Array(Module.HEAPU8.buffer, view.getFloat64(16, true), view.getFloat64(8, true));
          });
        } finally {
          if (task) Module['_lightusd_combined_package_end'](task);
          if (out) Module['_lightusd_combined_free'](out);
          owner.delete();
        }
      }
    });
  }
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'remapLayerAssetPaths', {
    configurable: true,
    value: function(map) {
      const label = 'remapLayerAssetPaths';
      const ptr = requireLiveLoader(this);
      if (arguments.length !== 1) throw new TypeError(`${label}: wrong argument count`);
      const owner = this.clone();
      try {
        if (!owner.ok()) return invokeWithPointers((loader, zero) =>
          Module['_lightusd_combined_export_remap'](loader, zero, 0), [ptr, 0]);
        return withStreamBytes(label, [encodeExportMap(label, map)], ([data], [size]) => {
          const result = loadingCall((loader, pairs) =>
            Module['_lightusd_combined_export_remap'](loader, pairs, size), [ptr, data]);
          if (result < -1) throw new TypeError(`${label}: invalid map`);
          return result;
        });
      } finally { owner.delete(); }
    }
  });

  ['getMhProfileJSON', 'getShadingGraphJSON'].forEach((name, query) => {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function() {
        const ptr = requireLiveLoader(this);
        if (arguments.length) throw new TypeError(`${name}: wrong argument count`);
        const owner = this.clone();
        try {
          // Select the ABI before dispatch: stage reconstruction may emit debug
          // callbacks, so a TypeError must never retry the inspection itself.
          const loader = memory64Loader(ptr) ? BigInt(ptr) : Number(ptr);
          if (loadingCall(() => Module['_lightusd_combined_render_json'](loader, query), [], true) !== 0)
            throw new TypeError(`${name}: invalid receiver`);
          return streamTableString(name, 0);
        } finally {
          Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  });

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'generateBoneTexture', {
    configurable: true,
    value: function(id, maximum) {
      const name = 'generateBoneTexture', ptr = requireLiveLoader(this);
      if (arguments.length !== 2) throw new TypeError(`${name}: wrong argument count`);
      const index = configInteger(name, id, -2147483648, 2147483647);
      const influences = configInteger(name, maximum, -2147483648, 2147483647);
      const is64 = memory64Loader(ptr), pointer = value => is64 ? BigInt(value) : Number(value);
      const owner = this.clone();
      let out = 0, result = 0;
      try {
        out = Module['_lightusd_combined_alloc'](72);
        if (!out) throw new RangeError(`${name}: allocation failed`);
        new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 72, true);
        const status = loadingCall(() => Module['_lightusd_combined_bone_texture_begin'](
          pointer(ptr), index, influences, pointer(out)), [], true);
        if (status < 0) throw new RangeError(`${name}: invalid record or allocation failure`);
        if (!status) return {error: streamTableString(name, 0)};
        const view = new DataView(Module.HEAPU8.buffer, Number(out), 72);
        result = view.getBigUint64(32, true);
        const data = {
          textureWidth: view.getUint32(4, true), textureHeight: view.getUint32(8, true),
          texelsPerVertex: view.getUint32(12, true), maxInfluences: view.getUint32(16, true),
          vertexCount: view.getUint32(20, true), originalElementSize: view.getUint32(24, true)
        };
        const address = Number(view.getBigUint64(40, true)), count = Number(view.getBigUint64(48, true));
        const offsets = Number(view.getBigUint64(56, true)), offsetCount = Number(view.getBigUint64(64, true));
        const copy = (ptr, length) => {
          if (!Number.isSafeInteger(ptr) || !Number.isSafeInteger(length) || ptr % 4 ||
              ptr > Module.HEAPU8.byteLength || length > Math.floor((Module.HEAPU8.byteLength - ptr) / 4))
            throw new RangeError(`${name}: invalid result span`);
          return new Float32Array(Module.HEAPU8.buffer, ptr, length).slice();
        };
        data.textureData = copy(address, count);
        data.vertexOffsets = copy(offsets, offsetCount);
        return data;
      } finally {
        if (result) Module['_lightusd_combined_bone_texture_end'](pointer(result));
        if (out) Module['_lightusd_combined_free'](out);
        Module['_lightusd_combined_table_release']();
        owner.delete();
      }
    }
  });

  for (const [name, copy] of [['getMesh', false], ['getMeshCopy', true]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
        const index = configInteger(name, id, -2147483648, 2147483647);
        const pointer = memory64Loader(ptr) ? value => BigInt(value) : value => Number(value);
        const owner = this.clone();
        let out = 0, result = 0;
        try {
          if (!copy && Module['_lightusd_combined_mesh_warn'](pointer(ptr)) === 1)
            console.warn('[lightusd] getMesh() is deprecated; prefer getMeshPtr()/getMeshCopy(). (Heap views from the old API alias WASM memory and can dangle; the *Ptr/*Copy accessors make the contract explicit.)');
          out = Module['_lightusd_combined_alloc'](80);
          if (!out) throw new RangeError(`${name}: allocation failed`);
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 80, true);
          const status = loadingCall(() => Module['_lightusd_combined_mesh_value_begin'](pointer(ptr), index, pointer(out)), [], true);
          if (status < 0) throw new RangeError(`${name}: invalid record or allocation failure`);
          if (!status) return {};
          let view = new DataView(Module.HEAPU8.buffer, Number(out), 80);
          result = view.getBigUint64(72, true);
          const flags = view.getUint32(4, true), attributes = view.getUint32(20, true), groups = view.getUint32(24, true);
          const skeleton = view.getInt32(16, true);
          const mesh = {materialId: view.getInt32(8, true), elementSize: view.getInt32(12, true),
            doubleSided: !!(flags & 1), isAreaLight: !!(flags & 2), hasGeomBindTransform: !!(flags & 8), uvSets: {}};
          if (skeleton >= 0) mesh.skel_id = skeleton;
          if (flags & 16) mesh.displayColor = [32,40,48].map(offset => view.getFloat64(offset, true));
          if (flags & 2) {
            mesh.lightIntensity = view.getFloat64(56, true); mesh.lightExposure = view.getFloat64(64, true);
            mesh.lightNormalize = !!(flags & 4);
          }
          mesh.primName = streamTableString(name, 0); mesh.displayName = streamTableString(name, 1); mesh.absPath = streamTableString(name, 2);
          if (flags & 2) mesh.lightMaterialSyncMode = streamTableString(name, 3);
          const names = ['points', 'faceVertexIndices', 'faceVertexCounts', 'normals', '',
            'vertexColors', 'colors', 'colorOpacities', 'tangents', 'texcoords', 'tangentsPacked',
            'lightColor', 'jointIndices', 'jointWeights', 'geomBindTransform'];
          const constructors = [Float32Array, Uint32Array, Int8Array, Int16Array, Uint8Array, Int8Array, Int32Array, Float64Array];
          // Read records first. Build views only after all native calls that
          // could grow memory, so every returned view uses the current heap.
          const records = [];
          for (let i = 0; i < attributes; ++i) {
            new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 40, true);
            if (Module['_lightusd_combined_mesh_pointer_attribute'](pointer(result), i, pointer(out)) !== 1)
              throw new RangeError(`${name}: missing attribute`);
            view = new DataView(Module.HEAPU8.buffer, Number(out), 40);
            records.push({key: view.getUint32(4, true), slot: view.getUint32(8, true), dtype: view.getUint32(12, true),
              address: Number(view.getBigUint64(24, true)), count: Number(view.getBigUint64(32, true))});
          }
          if (flags & 32) mesh.submeshes = [];
          for (let i = 0; i < groups; ++i) {
            new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 16, true);
            if (Module['_lightusd_combined_mesh_pointer_submesh'](pointer(result), i, pointer(out)) !== 1)
              throw new RangeError(`${name}: missing submesh`);
            view = new DataView(Module.HEAPU8.buffer, Number(out), 16);
            mesh.submeshes.push({start: view.getInt32(4, true), count: view.getInt32(8, true), materialId: view.getInt32(12, true)});
          }
          for (const {key, slot, dtype, address, count} of records) {
            const Type = constructors[dtype], width = Type?.BYTES_PER_ELEMENT;
            if (key > 14 || !width || !Number.isSafeInteger(address) || !Number.isSafeInteger(count) || address % width ||
                address > Module.HEAPU8.byteLength || count > Math.floor((Module.HEAPU8.byteLength - address) / width))
              throw new RangeError(`${name}: invalid attribute span`);
            const array = new Type(Module.HEAPU8.buffer, address, count);
            const value = copy ? array.slice() : array;
            if (key === 4) mesh.uvSets['uv' + slot] = {data: value, vertexCount: Math.floor(count / 2), slotId: slot | 0};
            else mesh[names[key]] = value;
            if (key <= 2) mesh[names[key] + 'Length'] = count;
            if (key === 3) mesh.normalsFormat = dtype === 2 ? 'snorm8' : dtype === 3 ? 'snorm16' : 'float32';
            if (key === 6 || key === 7) mesh[names[key] + 'Format'] = 'float32';
            if (key === 10) mesh.tangentsPackedFormat = 'INT_2_10_10_10_REV';
          }
          return mesh;
        } finally {
          if (result) Module['_lightusd_combined_mesh_pointer_end'](pointer(result));
          if (out) Module['_lightusd_combined_free'](out);
          Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  }

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getMeshPtr', {
    configurable: true,
    value: function(id) {
      const name = 'getMeshPtr', ptr = requireLiveLoader(this);
      if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
      const index = configInteger(name, id, -2147483648, 2147483647);
      const pointer = memory64Loader(ptr) ? value => BigInt(value) : value => Number(value);
      const owner = this.clone();
      let out = 0, result = 0;
      try {
        out = Module['_lightusd_combined_alloc'](64);
        if (!out) throw new RangeError(`${name}: allocation failed`);
        new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 64, true);
        const status = loadingCall(() => Module['_lightusd_combined_mesh_pointer_begin'](pointer(ptr), index, pointer(out)), [], true);
        if (status < 0) throw new RangeError(`${name}: invalid record or allocation failure`);
        if (!status) return {};
        let view = new DataView(Module.HEAPU8.buffer, Number(out), 64);
        result = view.getBigUint64(56, true);
        const flags = view.getUint32(4, true), attributes = view.getUint32(24, true), groups = view.getUint32(28, true);
        const mesh = {vertexCount: Number(view.getBigUint64(16, true)), materialId: view.getInt32(8, true),
          doubleSided: !!(flags & 16), hasSubmeshes: !!(flags & 1), singleIndexable: !!(flags & 2), triangulated: !!(flags & 4)};
        if (flags & 8) mesh.displayColor = [32,40,48].map(offset => view.getFloat64(offset, true));
        mesh.primName = streamTableString(name, 0); mesh.displayName = streamTableString(name, 1); mesh.absPath = streamTableString(name, 2);
        const names = ['points', 'indices', 'faceVertexCounts', 'normals', '', 'vertexColors', 'colors', 'colorOpacities'];
        const types = ['f32', 'u32', 'snorm8', 'snorm16', 'u8', 'i8'], widths = [4,4,1,2,1,1];
        for (let i = 0; i < attributes; ++i) {
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 40, true);
          if (Module['_lightusd_combined_mesh_pointer_attribute'](pointer(result), i, pointer(out)) !== 1)
            throw new RangeError(`${name}: missing attribute`);
          view = new DataView(Module.HEAPU8.buffer, Number(out), 40);
          const key = view.getUint32(4, true), slot = view.getUint32(8, true), dtype = view.getUint32(12, true), comps = view.getUint32(16, true);
          const address = Number(view.getBigUint64(24, true)), length = Number(view.getBigUint64(32, true));
          const width = widths[dtype];
          if (key > 8 || !width || !comps || !Number.isSafeInteger(address) || !Number.isSafeInteger(length) ||
              address % width || address > Module.HEAPU8.byteLength || length > Math.floor((Module.HEAPU8.byteLength - address) / width))
            throw new RangeError(`${name}: invalid attribute span`);
          if (key === 8) {
            if (dtype !== 0 || comps !== 4) throw new RangeError(`${name}: invalid tangent format`);
            mesh.tangents = new Float32Array(Module.HEAPU8.buffer, address, length).slice();
          } else {
            const descriptor = {ptr: address, length, comps, count: Math.floor(length / comps), dtype: types[dtype], byteLength: length * width};
            if (key === 4) {
              (mesh.uvSets ||= {})[String(slot)] = descriptor;
              if (slot === 0) mesh.uv0 = {...descriptor};
            } else mesh[names[key]] = descriptor;
          }
        }
        if (groups) mesh.submeshes = [];
        for (let i = 0; i < groups; ++i) {
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 16, true);
          if (Module['_lightusd_combined_mesh_pointer_submesh'](pointer(result), i, pointer(out)) !== 1)
            throw new RangeError(`${name}: missing submesh`);
          view = new DataView(Module.HEAPU8.buffer, Number(out), 16);
          mesh.submeshes.push({start: view.getInt32(4, true), count: view.getInt32(8, true), materialId: view.getInt32(12, true)});
        }
        return mesh;
      } finally {
        if (result) Module['_lightusd_combined_mesh_pointer_end'](pointer(result));
        if (out) Module['_lightusd_combined_free'](out);
        Module['_lightusd_combined_table_release']();
        owner.delete();
      }
    }
  });

  ['getMeshPrimvarsJSON', 'computeMeshTangents'].forEach((name, operation) => {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
        const index = configInteger(name, id, -2147483648, 2147483647);
        const owner = this.clone();
        try {
          const loader = memory64Loader(ptr) ? BigInt(ptr) : Number(ptr);
          const result = loadingCall(() => Module['_lightusd_combined_mesh_operation'](loader, operation, index), [], true);
          if (result < 0) throw new TypeError(`${name}: invalid operation`);
          return operation === 0 ? streamTableString(name, 0) : !!result;
        } finally {
          if (operation === 0) Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  });

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getInstance', {
    configurable: true,
    value: function(id) {
      const ptr = requireLiveLoader(this), name = 'getInstance';
      if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
      const index = configInteger(name, id, -2147483648, 2147483647);
      const out = Module['_lightusd_combined_alloc'](280);
      if (!out) throw new RangeError(`${name}: allocation failed`);
      try {
        new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 280, true);
        const status = invokeWithPointers((loader, record) =>
          Module['_lightusd_combined_instance_get'](loader, index, record), [ptr, out]);
        if (status < 0) throw new TypeError(`${name}: invalid record`);
        if (!status) return null;
        const view = new DataView(Module.HEAPU8.buffer, Number(out), 280);
        // Copy every numeric field before string allocations can grow memory.
        const result = {
          prototypeIndex: view.getInt32(8, true),
          meshId: view.getInt32(12, true),
          materialId: view.getInt32(16, true),
          localMatrix: Array.from({length: 16}, (_, i) => view.getFloat64(24 + i * 8, true)),
          globalMatrix: Array.from({length: 16}, (_, i) => view.getFloat64(152 + i * 8, true)),
          visible: view.getUint32(4, true) !== 0
        };
        return {primName: streamTableString(name, 0), absPath: streamTableString(name, 1),
          displayName: streamTableString(name, 2), ...result};
      } finally {
        Module['_lightusd_combined_table_release']();
        Module['_lightusd_combined_free'](out);
      }
    }
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getInstancesForMesh', {
    configurable: true,
    value: function(id) {
      const ptr = requireLiveLoader(this), name = 'getInstancesForMesh';
      if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
      const index = configInteger(name, id, -2147483648, 2147483647);
      let out = 0;
      try {
        const count = invokeWithPointers(loader =>
          Module['_lightusd_combined_instances_for_mesh'](loader, index), [ptr]);
        if (count < 0 || count > 0x3fffffff) throw new RangeError(`${name}: invalid receiver or result size`);
        if (!count) return [];
        out = Module['_lightusd_combined_alloc'](count * 4);
        if (!out) throw new RangeError(`${name}: allocation failed`);
        if (withPointerFallback(p => Module['_lightusd_combined_table_shape'](p, count), out) !== 0)
          throw new RangeError(`${name}: result changed`);
        return Array.from(new Uint32Array(Module.HEAPU8.buffer, Number(out), count));
      } finally {
        Module['_lightusd_combined_table_release']();
        if (out) Module['_lightusd_combined_free'](out);
      }
    }
  });

  for (const [name, useDefault] of [['getRootNode', 0], ['getDefaultRootNode', 1]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== (useDefault ? 0 : 1)) throw new TypeError(`${name}: wrong argument count`);
        const index = useDefault ? 0 : configInteger(name, id, -2147483648, 2147483647);
        const owner = this.clone();
        let out = 0, cursor = 0;
        try {
          const is64 = memory64Loader(ptr);
          const pointer = value => is64 ? BigInt(value) : Number(value);
          out = Module['_lightusd_combined_alloc'](288);
          if (!out) throw new RangeError(`${name}: allocation failed`);
          const status = Module['_lightusd_combined_nodes_begin'](pointer(ptr), index, useDefault, pointer(out));
          if (status < 0) throw new RangeError(`${name}: cannot open hierarchy cursor`);
          if (!status) return {};
          const header = new DataView(Module.HEAPU8.buffer, Number(out), 8);
          cursor = is64 ? header.getBigUint64(0, true) : header.getUint32(0, true);
          const stack = [];
          let root;
          while (true) {
            new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 288, true);
            const next = Module['_lightusd_combined_nodes_next'](cursor, pointer(out));
            if (next < 0) throw new TypeError(`${name}: invalid hierarchy record`);
            if (!next) break;
            const view = new DataView(Module.HEAPU8.buffer, Number(out), 288);
            const childCount = Number(view.getBigUint64(24, true));
            if (childCount > 0xffffffff) throw new RangeError(`${name}: too many children`);
            const flags = view.getUint32(4, true);
            const fields = {
              contentId: view.getInt32(8, true),
              localMatrix: Array.from({length: 16}, (_, i) => view.getFloat64(32 + i * 8, true)),
              globalMatrix: Array.from({length: 16}, (_, i) => view.getFloat64(160 + i * 8, true)),
              hasResetXform: (flags & 1) !== 0,
              isInstance: (flags & 2) !== 0,
              prototypeIndex: view.getInt32(12, true),
              instanceId: view.getInt32(16, true)
            };
            const node = {
              primName: streamTableString(name, 0), displayName: streamTableString(name, 1),
              absPath: streamTableString(name, 2), nodeCategory: streamTableString(name, 3),
              nodeType: streamTableString(name, 4), ...fields, children: []
            };
            while (stack.length && !stack[stack.length - 1].remaining) stack.pop();
            if (stack.length) {
              const parent = stack[stack.length - 1];
              parent.node.children.push(node);
              --parent.remaining;
            } else if (root) {
              throw new RangeError(`${name}: unexpected hierarchy root`);
            } else { root = node; }
            if (childCount) stack.push({node, remaining: childCount});
          }
          if (stack.some(frame => frame.remaining)) throw new RangeError(`${name}: incomplete hierarchy`);
          return root || {};
        } finally {
          if (cursor) Module['_lightusd_combined_nodes_end'](cursor);
          Module['_lightusd_combined_table_release']();
          if (out) Module['_lightusd_combined_free'](out);
          owner.delete();
        }
      }
    });
  }

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getUDIMTexture', {
    configurable: true,
    value: function(id) {
      const ptr = requireLiveLoader(this), name = 'getUDIMTexture';
      if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
      const index = configInteger(name, id, -2147483648, 2147483647);
      const countOut = Module['_lightusd_combined_alloc'](4);
      if (!countOut) throw new RangeError(`${name}: allocation failed`);
      let out = 0;
      try {
        const status = invokeWithPointers((loader, count) =>
          Module['_lightusd_combined_udim_get'](loader, index, count), [ptr, countOut]);
        if (status < 0) throw new RangeError(`${name}: invalid arguments or result size`);
        if (!status) return {};
        const count = new DataView(Module.HEAPU8.buffer, Number(countOut), 4).getUint32(0, true);
        if (count > 0x0fffffff) throw new RangeError(`${name}: result exceeds allocator limit`);
        const tiles = [];
        if (count) {
          out = Module['_lightusd_combined_alloc'](count * 16);
          if (!out) throw new RangeError(`${name}: allocation failed`);
          if (withPointerFallback(p => Module['_lightusd_combined_table_shape'](p, count * 4), out) !== 0)
            throw new RangeError(`${name}: result changed`);
          const view = new DataView(Module.HEAPU8.buffer, Number(out), count * 16);
          for (let i = 0; i < count; ++i) tiles.push({
            udim: view.getInt32(i * 16, true), u: view.getInt32(i * 16 + 4, true),
            v: view.getInt32(i * 16 + 8, true), imageId: view.getInt32(i * 16 + 12, true)
          });
        }
        return {primName: streamTableString(name, 0), absPath: streamTableString(name, 1),
          displayName: streamTableString(name, 2), assetIdentifier: streamTableString(name, 3), tiles};
      } finally {
        Module['_lightusd_combined_table_release']();
        if (out) Module['_lightusd_combined_free'](out);
        Module['_lightusd_combined_free'](countOut);
      }
    }
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'extractUnresolvedTexturePaths', {
    configurable: true,
    value: function() {
      const ptr = requireLiveLoader(this), name = 'extractUnresolvedTexturePaths';
      if (arguments.length) throw new TypeError(`${name}: wrong argument count`);
      try {
        const count = invokeWithPointers(loader =>
          Module['_lightusd_combined_unresolved_textures'](loader), [ptr]);
        if (count < 0) throw new RangeError(`${name}: invalid receiver or result size`);
        const result = [];
        for (let i = 0; i < count; ++i) result.push(streamTableString(name, i));
        return result;
      } finally { Module['_lightusd_combined_table_release'](); }
    }
  });

  for (const [name, mode] of [['getImage', 0], ['getImagePtr', 1], ['getImageCopy', 2]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
        const index = configInteger(name, id, -2147483648, 2147483647);
        const owner = this.clone();
        let out = 0;
        try {
          const is64 = memory64Loader(ptr);
          const pointer = value => is64 ? BigInt(value) : Number(value);
          // Warning state lives on the native loader, including across clones
          // and resets. Mark before invoking console.warn, as the legacy API did.
          if (mode === 0 && Module['_lightusd_combined_image_warn'](pointer(ptr)) === 1) {
            console.warn('[lightusd] getImage() is deprecated; prefer getImagePtr()/getImageCopy().' +
              ' (Heap views from the old API alias WASM memory and can dangle; the *Ptr/*Copy accessors make the contract explicit.)');
          }
          out = Module['_lightusd_combined_alloc'](128);
          if (!out) throw new RangeError(`${name}: allocation failed`);
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 128, true);
          const status = Module['_lightusd_combined_image_get'](pointer(ptr), index, mode === 2 ? 1 : 0, pointer(out));
          if (status < 0) throw new TypeError(`${name}: invalid record`);
          if (!status) return {};
          const view = new DataView(Module.HEAPU8.buffer, Number(out), 128);
          const flags = view.getUint32(4, true), bufferId = view.getInt32(20, true);
          const address = Number(view.getBigUint64(24, true)), byteLength = Number(view.getBigUint64(32, true));
          const dimensions = {width: view.getInt32(8, true), height: view.getInt32(12, true), channels: view.getInt32(16, true)};
          const color = {
            colorTransformValid: (flags & 2) !== 0, colorTransformApplied: (flags & 4) !== 0,
            colorTransformBypass: (flags & 8) !== 0, sourceColorIsData: (flags & 16) !== 0,
            sourceGamma: view.getFloat64(40, true), sourceLinearBias: view.getFloat64(48, true),
            sourceToDisplayLinear: Array.from({length: 9}, (_, i) => view.getFloat64(56 + i * 8, true))
          };
          const uri = streamTableString(name, 0);
          const metadata = {decoded: (flags & 1) !== 0, colorSpace: streamTableString(name, 1),
            usdColorSpace: streamTableString(name, 2), sourceColorSpaceName: streamTableString(name, 3), ...color};
          const result = mode === 1 ? {...dimensions, ...metadata, uri, bufferId}
                                    : {...dimensions, uri, ...metadata, bufferId};
          if (flags & 32) {
            if (!Number.isSafeInteger(address) || !Number.isSafeInteger(byteLength) ||
                address > Module.HEAPU8.byteLength || byteLength > Module.HEAPU8.byteLength - address)
              throw new RangeError(`${name}: invalid image buffer span`);
            if (mode === 1) { result.ptr = address; result.byteLength = byteLength; }
            else {
              // All allocating string copies precede creation of the heap view.
              const bytes = new Uint8Array(Module.HEAPU8.buffer, address, byteLength);
              result.data = mode === 2 ? bytes.slice() : bytes;
            }
          }
          return result;
        } finally {
          Module['_lightusd_combined_table_release']();
          if (out) Module['_lightusd_combined_free'](out);
          owner.delete();
        }
      }
    });
  }

  const LIGHT_FIELDS = [["color", 3], ["intensity", 1], ["exposure", 1], ["diffuse", 1], ["specular", 1], ["colorTemperature", 1], ["transform", 16], ["position", 3], ["direction", 3], ["radius", 1], ["width", 1], ["height", 1], ["length", 1], ["angle", 1], ["shapingConeAngle", 1], ["shapingConeSoftness", 1], ["shapingFocus", 1], ["shapingFocusTint", 3], ["shapingIesAngleScale", 1], ["shadowColor", 3], ["shadowDistance", 1], ["shadowFalloff", 1], ["shadowFalloffGamma", 1], ["guideRadius", 1]];
  function readLight(ptr, id) {
    const name = 'getLight', out = Module['_lightusd_combined_alloc'](424);
    if (!out) throw new RangeError(`${name}: allocation failed`);
    try {
      new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 424, true);
      const status = invokeWithPointers((loader, record) =>
        Module['_lightusd_combined_light_get'](loader, id, record), [ptr, out]);
      if (status < 0) throw new TypeError(`${name}: invalid record`);
      if (!status) return {error: streamTableString(name, 0)};
      const view = new DataView(Module.HEAPU8.buffer, Number(out), 424);
      const flags = view.getUint32(4, true), fields = {};
      let offset = 32;
      for (const [key, count] of LIGHT_FIELDS) {
        fields[key] = count === 1 ? view.getFloat64(offset, true)
          : Array.from({length: count}, (_, i) => view.getFloat64(offset + i * 8, true));
        offset += count * 8;
      }
      Object.assign(fields, {normalize: !!(flags & 1), enableColorTemperature: !!(flags & 2),
        shapingIesNormalize: !!(flags & 4), shadowEnable: !!(flags & 8),
        envmapTextureId: view.getInt32(8, true), geometryMeshId: view.getInt32(12, true)});
      let samples;
      if (flags & 16) {
        const address = Number(view.getBigUint64(16, true)), count = Number(view.getBigUint64(24, true));
        if (!Number.isSafeInteger(address) || !Number.isSafeInteger(count) ||
            address > Module.HEAPU8.byteLength || count > Math.floor((Module.HEAPU8.byteLength - address) / 8))
          throw new RangeError(`${name}: invalid spectral span`);
        const values = new Float32Array(Module.HEAPU8.buffer, address, count * 2);
        samples = Array.from({length: count}, (_, i) => [values[i * 2], values[i * 2 + 1]]);
      }
      const result = {name: streamTableString(name, 0), absPath: streamTableString(name, 1),
        displayName: streamTableString(name, 2), type: streamTableString(name, 3), ...fields,
        textureFile: streamTableString(name, 4), shapingIesFile: streamTableString(name, 5),
        domeTextureFormat: streamTableString(name, 6), materialSyncMode: streamTableString(name, 7)};
      if (samples) result.spectralEmission = {samples, interpolation: streamTableString(name, 8),
        unit: streamTableString(name, 9), preset: streamTableString(name, 10)};
      return result;
    } finally {
      Module['_lightusd_combined_table_release']();
      Module['_lightusd_combined_free'](out);
    }
  }
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getLight', {
    configurable: true,
    value: function(id) {
      const ptr = requireLiveLoader(this);
      if (arguments.length !== 1) throw new TypeError('getLight: wrong argument count');
      const index = configInteger('getLight', id, -2147483648, 2147483647), owner = this.clone();
      try { return readLight(ptr, index); } finally { owner.delete(); }
    }
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getAllLights', {
    configurable: true,
    value: function() {
      const ptr = requireLiveLoader(this);
      if (arguments.length) throw new TypeError('getAllLights: wrong argument count');
      const owner = this.clone();
      try {
        const count = invokeWithPointers(loader => Module['_lightusd_combined_lights_count'](loader), [ptr]);
        if (count < 0) throw new RangeError('getAllLights: invalid count');
        return Array.from({length: count}, (_, i) => readLight(ptr, i));
      } finally { owner.delete(); }
    }
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'encodeImageNative', {
    configurable: true,
    value: function(pixels, width, height, channels, format) {
      const ptr = requireLiveLoader(this), name = 'encodeImageNative';
      if (arguments.length !== 5) throw new TypeError(`${name}: wrong argument count`);
      const w = configInteger(name, width, -2147483648, 2147483647);
      const h = configInteger(name, height, -2147483648, 2147483647);
      const c = configInteger(name, channels, -2147483648, 2147483647);
      const pointer = memory64Loader(ptr) ? value => BigInt(value) : value => Number(value);
      const owner = this.clone();
      let out = 0;
      try {
        return withStreamBytes(name, [pixels, format], (data, sizes) => {
          out = Module['_lightusd_combined_alloc'](24);
          if (!out) throw new RangeError(`${name}: allocation failed`);
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 24, true);
          const status = loadingCall(() => Module['_lightusd_combined_encode_image'](
            pointer(ptr), pointer(data[0]), sizes[0], w, h, c, pointer(data[1]), sizes[1], pointer(out)), [], true);
          if (status < 0) throw new TypeError(`${name}: invalid arguments`);
          if (status === 0) return null;
          if (status === 2) return {success: false, error: 'Invalid image dimensions.'};
          const view = new DataView(Module.HEAPU8.buffer, Number(out), 24);
          const size = view.getFloat64(8, true), address = view.getFloat64(16, true);
          if (!Number.isSafeInteger(size) || size < 0 || !Number.isSafeInteger(address) || address < 0 ||
              address > Module.HEAPU8.length || size > Module.HEAPU8.length - address || (!address && size)) {
            throw new RangeError(`${name}: invalid output span`);
          }
          return Module.HEAPU8.subarray(address, address + size);
        });
      } finally {
        if (out) Module['_lightusd_combined_free'](out);
        owner.delete();
      }
    }
  });

  for (const [name, operation, argc] of [['extractPhysicsSceneJSON', 0, 0],
    ['createSampleScene', 1, 0], ['clearURDFMeshBuffers', 2, 0], ['createURDFPhysicsScene', 3, 1]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const ptr = requireLiveLoader(this);
        if (args.length !== argc) throw new TypeError(`${name}: wrong argument count`);
        const pointer = memory64Loader(ptr) ? value => BigInt(value) : value => Number(value);
        const owner = this.clone();
        try {
          return withStreamBytes(name, args, (data, sizes) => {
            const status = loadingCall(() => Module['_lightusd_combined_schema_operation'](
              pointer(ptr), operation, pointer(data[0] || 0), sizes[0] || 0), [], true);
            if (status < 0) throw new TypeError(`${name}: invalid arguments`);
            if (operation === 0) return streamTableString(name, 0);
            if (operation !== 2) return status !== 0;
          });
        } finally {
          if (operation === 0) Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  }
  function schemaMeshBytes(name, array) {
    if (array == null) return new Uint8Array();
    if (!ArrayBuffer.isView(array) || !Number.isInteger(array.length)) {
      throw new TypeError(`${name}: expected a typed array`);
    }
    // Preserve the legacy raw 32-bit reinterpretation, including Uint32 indices.
    const size = array.length * 4;
    if (array.length > 0x10000000 || size > array.buffer.byteLength - array.byteOffset) {
      return new Uint8Array();
    }
    if (array.byteOffset % 4) throw new RangeError(`${name}: unaligned mesh array`);
    return new Uint8Array(array.buffer, array.byteOffset, size);
  }
  for (const name of ['setVisualMesh', 'setCollisionMesh']) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(meshName, positions, normals, uvs, indices) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== 5) throw new TypeError(`${name}: wrong argument count`);
        const pointer = memory64Loader(ptr) ? value => BigInt(value) : value => Number(value);
        const owner = this.clone();
        try {
          const key = streamBytes(name, meshName);
          const arrays = [positions, normals, uvs, indices].map(array =>
            key.length ? schemaMeshBytes(name, array) : new Uint8Array());
          // withStreamBytes snapshots every span before any allocation grows the heap.
          return withStreamBytes(name, [key, ...arrays], (data, sizes) => {
            const status = loadingCall(() => Module['_lightusd_combined_schema_mesh'](
              pointer(ptr), pointer(data[0]), sizes[0], pointer(data[1]), sizes[1] / 4,
              pointer(data[2]), sizes[2] / 4, pointer(data[3]), sizes[3] / 4,
              pointer(data[4]), sizes[4] / 4), [], true);
            if (status < 0) throw new TypeError(`${name}: invalid arguments`);
            return status !== 0;
          });
        } finally { owner.delete(); }
      }
    });
  }

  for (const [name, formatted] of [['getMaterial', false], ['getMaterialWithFormat', true]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id, format) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== (formatted ? 2 : 1)) throw new TypeError(`${name}: wrong argument count`);
        const index = configInteger(name, id, -2147483648, 2147483647);
        const pointer = memory64Loader(ptr) ? value => BigInt(value) : value => Number(value);
        const owner = this.clone();
        let out = 0;
        try {
          return withStreamBytes(name, [formatted ? format : 'json'], ([data], [size]) => {
            out = Module['_lightusd_combined_alloc'](240);
            if (!out) throw new RangeError(`${name}: allocation failed`);
            new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 240, true);
            const status = loadingCall(() => Module['_lightusd_combined_material_get'](
              pointer(ptr), index, pointer(data), size, pointer(out)), [], true);
            if (status < 0) throw new TypeError(`${name}: invalid arguments`);
            if (status === 0) return {error: streamTableString(name, 0)};
            if (status === 1) return {data: streamTableString(name, 0), format: streamTableString(name, 1)};
            const view = new DataView(Module.HEAPU8.buffer, Number(out), 240);
            const flags = view.getUint32(4, true), mask = view.getUint32(8, true);
            const textures = Array.from({length: 13}, (_, i) => view.getInt32(16 + i * 4, true));
            const values = Array.from({length: 21}, (_, i) => view.getFloat64(72 + i * 8, true));
            const material = {materialXConfig: {authored: !!(flags & 1), version: streamTableString(name, 1),
              namespace: streamTableString(name, 2), colorspace: streamTableString(name, 3), sourceUri: streamTableString(name, 4)}};
            if (!(flags & 2)) return {...material, error: streamTableString(name, 0)};
            material.useSpecularWorkflow = !!(flags & 4);
            const names = ['diffuseColor', 'emissiveColor', 'specularColor', 'metallic', 'roughness', 'clearcoat',
              'clearcoatRoughness', 'opacity', 'opacityThreshold', 'ior', 'normal', 'displacement', 'occlusion'];
            const offsets = [0,3,6,12,13,14,15,16,17,18,9,19,20];
            for (let i = 0; i < names.length; ++i) {
              if ((i === 2 && !(flags & 4)) || (i === 3 && (flags & 4))) continue;
              material[names[i]] = i < 3 || i === 10 ? values.slice(offsets[i], offsets[i] + 3) : values[offsets[i]];
              if (mask & (1 << i)) material[names[i] + 'TextureId'] = textures[i];
            }
            return material;
          });
        } finally {
          if (out) Module['_lightusd_combined_free'](out);
          Module['_lightusd_combined_table_release']();
          owner.delete();
        }
      }
    });
  }

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getLightWithFormat', {
    configurable: true,
    value: function(id, format) {
      const ptr = requireLiveLoader(this), name = 'getLightWithFormat';
      if (arguments.length !== 2) throw new TypeError(`${name}: wrong argument count`);
      const index = configInteger(name, id, -2147483648, 2147483647), owner = this.clone();
      try {
        const is64 = memory64Loader(ptr), pointer = value => is64 ? BigInt(value) : Number(value);
        return withStreamBytes(name, [format], ([data], [size]) => {
          const status = loadingCall(() => Module['_lightusd_combined_light_format'](
            pointer(ptr), index, pointer(data), size), [], true);
          if (status < 0) throw new TypeError(`${name}: invalid arguments`);
          const text = streamTableString(name, 0);
          return status ? {data: text, format: streamTableString(name, 1)} : {error: text};
        });
      } finally {
        Module['_lightusd_combined_table_release']();
        owner.delete();
      }
    }
  });

  function readSkeleton(ptr, id, flat) {
    const name = flat ? 'getSkeletonJointsFlat' : 'getSkeleton';
    const is64 = memory64Loader(ptr), pointer = value => is64 ? BigInt(value) : Number(value);
    const out = Module['_lightusd_combined_alloc'](272);
    if (!out) throw new RangeError(`${name}: allocation failed`);
    let cursor = 0;
    try {
      new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 16, true);
      const status = Module['_lightusd_combined_skeleton_begin'](pointer(ptr), id, pointer(out));
      if (status < 0) throw new RangeError(`${name}: cannot open skeleton cursor`);
      if (!status) return {error: streamTableString(name, 0)};
      const header = new DataView(Module.HEAPU8.buffer, Number(out), 16);
      cursor = pointer(header.getBigUint64(8, true));
      const animId = header.getInt32(4, true);
      const result = flat ? {joint_names: [], joint_paths: [], joint_ids: [], parent_indices: [],
        bind_matrices: [], rest_matrices: [], num_joints: 0}
        : {id, prim_name: streamTableString(name, 0), abs_path: streamTableString(name, 1),
          display_name: streamTableString(name, 2), anim_id: animId};
      const stack = [];
      let index = 0;
      while (true) {
        new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 272, true);
        const status = Module['_lightusd_combined_skeleton_next'](cursor, pointer(out));
        if (status < 0) throw new TypeError(`${name}: invalid joint record`);
        if (!status) break;
        const view = new DataView(Module.HEAPU8.buffer, Number(out), 272);
        const jointId = view.getInt32(4, true), childCount = Number(view.getBigUint64(8, true));
        if (childCount > 0xffffffff || index > 0x7fffffff) throw new RangeError(`${name}: hierarchy too large`);
        const bind = Array.from({length: 16}, (_, i) => view.getFloat64(16 + i * 8, true));
        const rest = Array.from({length: 16}, (_, i) => view.getFloat64(144 + i * 8, true));
        const jointPath = streamTableString(name, 0), jointName = streamTableString(name, 1);
        while (stack.length && !stack[stack.length - 1].remaining) stack.pop();
        const parent = stack[stack.length - 1];
        if (!parent && index) throw new RangeError(`${name}: unexpected joint root`);
        let node;
        if (flat) {
          result.joint_names.push(jointName); result.joint_paths.push(jointPath);
          result.joint_ids.push(jointId); result.parent_indices.push(parent ? parent.index : -1);
          result.bind_matrices.push(...bind); result.rest_matrices.push(...rest);
        } else {
          node = {joint_path: jointPath, joint_name: jointName, joint_id: jointId,
            bind_transform: bind, rest_transform: rest, children: []};
          if (parent) parent.node.children.push(node);
          else result.root_node = node;
        }
        if (parent) --parent.remaining;
        if (childCount) stack.push({node, index, remaining: childCount});
        ++index;
      }
      if (stack.some(frame => frame.remaining)) throw new RangeError(`${name}: incomplete hierarchy`);
      if (flat) result.num_joints = index;
      return result;
    } finally {
      if (cursor) Module['_lightusd_combined_skeleton_end'](cursor);
      Module['_lightusd_combined_table_release']();
      Module['_lightusd_combined_free'](out);
    }
  }
  for (const [name, flat] of [['getSkeleton', false], ['getSkeletonJointsFlat', true]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
        const index = configInteger(name, id, -2147483648, 2147483647), owner = this.clone();
        try { return readSkeleton(ptr, index, flat); } finally { owner.delete(); }
      }
    });
  }
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getAllSkeletons', {
    configurable: true,
    value: function() {
      const ptr = requireLiveLoader(this);
      if (arguments.length) throw new TypeError('getAllSkeletons: wrong argument count');
      const owner = this.clone();
      let out = 0;
      try {
        out = Module['_lightusd_combined_alloc'](8);
        if (!out) throw new RangeError('getAllSkeletons: allocation failed');
        if (invokeWithPointers((loader, record) =>
          Module['_lightusd_combined_render_scalar'](loader, 10, record), [ptr, out]) !== 0)
          throw new TypeError('getAllSkeletons: invalid receiver');
        const count = new DataView(Module.HEAPU8.buffer, Number(out), 8).getFloat64(0, true);
        return Array.from({length: count}, (_, i) => readSkeleton(ptr, i, false));
      } finally {
        if (out) Module['_lightusd_combined_free'](out);
        owner.delete();
      }
    }
  });

  function readAnimation(ptr, id, summary) {
    const name = summary ? 'getAnimationInfo' : 'getAnimation';
    const is64 = memory64Loader(ptr), pointer = value => is64 ? BigInt(value) : Number(value);
    const out = Module['_lightusd_combined_alloc'](64);
    if (!out) throw new RangeError(`${name}: allocation failed`);
    const record = pointer(out), loader = pointer(ptr);
    try {
      new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 64, true);
      const status = Module['_lightusd_combined_animation_get'](loader, id, record);
      if (status < 0) throw new RangeError(`${name}: invalid record or result size`);
      if (!status) return {};
      let view = new DataView(Module.HEAPU8.buffer, Number(out), 64);
      const flags = view.getUint32(4, true), channelCount = view.getUint32(8, true);
      const samplerCount = view.getUint32(12, true), targetCount = view.getUint32(16, true);
      const assetCount = view.getUint32(28, true), duration = view.getFloat64(32, true);
      const source = {numAnimatedJoints: view.getInt32(20, true), numAnimatedNodes: view.getInt32(24, true),
        hasValueClip: !!(flags & 1), valueClipBaked: !!(flags & 2),
        valueClipStartTime: view.getFloat64(40, true), valueClipEndTime: view.getFloat64(48, true),
        valueClipSampleRate: view.getFloat64(56, true)};
      const clipName = streamTableString(name, 0), primName = streamTableString(name, 1);
      const absPath = streamTableString(name, 2), displayName = streamTableString(name, 3);
      const sourceType = streamTableString(name, 4);
      source.clipAssetPaths = Array.from({length: assetCount}, (_, i) => streamTableString(name, i + 5));
      if (summary) return {id, name: clipName, duration, numTracks: channelCount,
        numSamplers: samplerCount, numTargetNodes: targetCount, sourceType, ...source, numClipAssetPaths: assetCount};
      const result = {name: clipName, primName, absPath, displayName, duration, sourceType, ...source};
      const copyFloats = (address, count) => {
        if (!Number.isSafeInteger(address) || !Number.isSafeInteger(count) || address % 4 ||
            address > Module.HEAPU8.byteLength || count > Math.floor((Module.HEAPU8.byteLength - address) / 4))
          throw new RangeError(`${name}: invalid sampler span`);
        return new Float32Array(Module.HEAPU8.buffer, address, count).slice();
      };
      const samplers = [];
      for (let i = 0; i < samplerCount; ++i) {
        new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 40, true);
        if (Module['_lightusd_combined_animation_sampler'](loader, id, i, record) !== 1)
          throw new RangeError(`${name}: sampler changed`);
        view = new DataView(Module.HEAPU8.buffer, Number(out), 40);
        const interpolation = ['LINEAR', 'STEP', 'CUBICSPLINE'][view.getUint32(4, true)] || 'LINEAR';
        const times = Number(view.getBigUint64(8, true)), timeCount = Number(view.getBigUint64(16, true));
        const values = Number(view.getBigUint64(24, true)), valueCount = Number(view.getBigUint64(32, true));
        samplers.push({times: copyFloats(times, timeCount), values: copyFloats(values, valueCount), interpolation});
      }
      const channels = [], tracks = [];
      for (let i = 0; i < channelCount; ++i) {
        new DataView(Module.HEAPU8.buffer).setUint32(Number(out), 32, true);
        if (Module['_lightusd_combined_animation_channel'](loader, id, i, record) !== 1)
          throw new RangeError(`${name}: channel changed`);
        view = new DataView(Module.HEAPU8.buffer, Number(out), 32);
        const flags = view.getUint32(4, true), path = view.getUint32(24, true);
        const channel = {sampler: view.getInt32(8, true), target_node: view.getInt32(12, true),
          skeleton_id: view.getInt32(16, true), joint_id: view.getInt32(20, true),
          target_type: flags & 8 ? 'SkeletonJoint' : 'SceneNode',
          path: ['Translation', 'Rotation', 'Scale', 'Weights', 'CustomProperty'][path] || 'Unknown',
          isCustomProperty: !!(flags & 4)};
        const property = streamTableString(name, 0), nodeName = streamTableString(name, 1);
        const base = streamTableString(name, 2);
        if ((flags & 4) && property) channel.propertyName = property;
        channels.push(channel);
        const sampler = samplers[channel.sampler];
        if (!(flags & 1) || !sampler || !sampler.times.length) continue;
        const track = {};
        if (flags & 2) {
          const suffix = ['.position', '.quaternion', '.scale', '.morphTargetInfluences'][path] || '';
          let type = ['vector3', 'quaternion', 'vector3', 'number'][path];
          if (path === 4) {
            const components = sampler.values.length && sampler.values.length % sampler.times.length === 0
              ? sampler.values.length / sampler.times.length : 0;
            type = components >= 2 && components <= 4 ? 'vector' + components : 'number';
          }
          if (type) track.type = type;
          track.name = base + (path === 4 ? '.' + (property || 'value') : suffix);
          track.isCustomProperty = !!(flags & 4);
          if (flags & 4) track.propertyName = property;
          track.nodeName = nodeName; track.nodeIndex = channel.target_node;
        }
        track.interpolation = sampler.interpolation;
        // Tracks and raw samplers retain independent owned arrays, as before.
        track.times = sampler.times.slice(); track.values = sampler.values.slice();
        track.path = ['translation', 'rotation', 'scale', 'weights', 'custom'][path] || 'unknown';
        tracks.push(track);
      }
      result.tracks = tracks; result.channels = channels; result.samplers = samplers;
      return result;
    } finally {
      Module['_lightusd_combined_table_release']();
      Module['_lightusd_combined_free'](out);
    }
  }
  for (const [name, summary] of [['getAnimation', false], ['getAnimationInfo', true]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== 1) throw new TypeError(`${name}: wrong argument count`);
        const index = configInteger(name, id, -2147483648, 2147483647), owner = this.clone();
        try { return readAnimation(ptr, index, summary); } finally { owner.delete(); }
      }
    });
  }
  for (const [name, summary] of [['getAllAnimations', false], ['getAllAnimationInfos', true]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function() {
        const ptr = requireLiveLoader(this);
        if (arguments.length) throw new TypeError(`${name}: wrong argument count`);
        const owner = this.clone();
        try {
          const count = invokeWithPointers(loader => Module['_lightusd_combined_animations_count'](loader), [ptr]);
          if (count < 0) throw new RangeError(`${name}: invalid count`);
          return Array.from({length: count}, (_, i) => readAnimation(ptr, i, summary));
        } finally { owner.delete(); }
      }
    });
  }

  const RENDER_SCALARS = ['numMeshes', 'numInstances', 'numMaterials', 'numTextures',
    'numImages', 'numLights', 'numCameras', 'numUDIMTextures', 'numRootNodes',
    'numAnimations', 'numSkeletons', 'getDefaultRootNodeId', 'getURI', 'getUpAxis'];
  RENDER_SCALARS.forEach((name, key) => {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function() {
        const ptr = requireLiveLoader(this);
        if (arguments.length) throw new TypeError(`${name}: wrong argument count`);
        const out = Module['_lightusd_combined_alloc'](8);
        if (!out) throw new RangeError(`${name}: allocation failed`);
        try {
          if (invokeWithPointers((loader, record) =>
            Module['_lightusd_combined_render_scalar'](loader, key, record), [ptr, out]) !== 0)
            throw new TypeError(`${name}: invalid receiver`);
          return key >= 12 ? streamTableString(name, 0)
            : new DataView(Module.HEAPU8.buffer, Number(out), 8).getFloat64(0, true);
        } finally {
          if (key >= 12) Module['_lightusd_combined_table_release']();
          Module['_lightusd_combined_free'](out);
        }
      }
    });
  });
  for (const [name, kind, bytes] of [['getCamera', 0, 72], ['getSceneMetadata', 1, 128], ['getTexture', 2, 152]]) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(id) {
        const ptr = requireLiveLoader(this);
        if (arguments.length !== (kind === 1 ? 0 : 1)) throw new TypeError(`${name}: wrong argument count`);
        const index = kind === 1 ? 0 : configInteger(name, id, -2147483648, 2147483647);
        const out = Module['_lightusd_combined_alloc'](bytes);
        if (!out) throw new RangeError(`${name}: allocation failed`);
        try {
          new DataView(Module.HEAPU8.buffer).setUint32(Number(out), bytes, true);
          const status = invokeWithPointers((loader, record) => kind === 0
            ? Module['_lightusd_combined_camera_get'](loader, index, record)
            : kind === 1 ? Module['_lightusd_combined_metadata_get'](loader, record)
                        : Module['_lightusd_combined_texture_get'](loader, index, record), [ptr, out]);
          if (status < 0) throw new TypeError(`${name}: invalid record`);
          if (!status) return kind === 0 ? {error: streamTableString(name, 0)} : {};
          const view = new DataView(Module.HEAPU8.buffer, Number(out), bytes);
          const flags = view.getUint32(4, true), result = {};
          const numbers = (names, offset) => names.forEach((field, i) => { result[field] = view.getFloat64(offset + i * 8, true); });
          const array = (offset, count) => Array.from({length: count}, (_, i) => view.getFloat64(offset + i * 8, true));
          let strings;
          if (kind === 0) {
            numbers(['focalLength', 'verticalAperture', 'horizontalAperture', 'znear', 'zfar', 'yfov', 'xfov', 'aspectRatio'], 8);
            strings = ['name', 'absPath', 'displayName', 'projection'];
          } else if (kind === 1) {
            numbers(['metersPerUnit', 'kilogramsPerUnit', 'framesPerSecond', 'timeCodesPerSecond', 'startTimeCode', 'endTimeCode'], 8);
            result.autoPlay = !!(flags & 1);
            if (!(flags & 2)) result.startTimeCode = null;
            if (!(flags & 4)) result.endTimeCode = null;
            result.workingToDisplayLinear = array(56, 9);
            strings = ['copyright', 'comment', 'upAxis', 'renderSettingsPrimPath', 'workingColorSpace'];
          } else {
            result.textureImageId = view.getInt32(8, true);
            result.hasTransform2d = !!(flags & 1); result.isUDIM = !!(flags & 2);
            numbers(['txRotation', 'txScaleU', 'txScaleV', 'txTranslationU', 'txTranslationV'], 16);
            result.bias = array(56, 4); result.scale = array(88, 4);
            if (result.isUDIM) {
              result.udimTextureId = view.getInt32(12, true);
              numbers(['udimUvScaleU', 'udimUvScaleV', 'udimUvOffsetU', 'udimUvOffsetV'], 120);
            }
            strings = ['wrapS', 'wrapT'];
          }
          strings.forEach((field, i) => { result[field] = streamTableString(name, i); });
          return result;
        } finally {
          Module['_lightusd_combined_table_release']();
          Module['_lightusd_combined_free'](out);
        }
      }
    });
  }

  const CONFIG_ARGS = {
    bool: (label, value) => (value ? 1 : 0),
    int32: (label, value) => configInteger(label, value, -2147483648, 2147483647),
    uint32: (label, value) => configInteger(label, value, 0, 4294967295),
    float: configNumber,
    double: configNumber
  };

  const CONFIG_SETTERS = [
    ['setCombineUDIMTiles', 0, ['bool']],
    ['setDeferTangentComputation', 1, ['bool']],
    ['setEnableBoneReduction', 2, ['bool']],
    ['setEnableValueClips', 3, ['bool']],
    ['setMaxMemoryLimitMB', 4, ['int32']],
    ['setRoundBoneCount', 5, ['bool']],
    ['setSphereSubdivisions', 6, ['int32']],
    ['setTargetBoneCount', 7, ['uint32']],
    ['setValueClipSampleRate', 8, ['float']],
    ['setValueClipUseTimeRange', 9, ['bool']],
    ['setValueClipTimeRange', 12, ['double', 'double']],
    ['setEnableComposition', 13, ['bool']],
    ['setLoadTextureInNative', 14, ['bool']],
    ['setNativeFlattenRenderTree', 15, ['bool']],
    ['setNativeMaterialDedup', 16, ['bool']],
    ['setNativeMeshMerge', 17, ['bool']],
    ['setNativeMeshMergeBakeTransform', 18, ['bool']],
    ['setUSDCExportLimitMB', 19, ['int32', 'int32']]
  ];
  for (const [name, key, types] of CONFIG_SETTERS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function(...args) {
        const loaderPtr = requireLiveLoader(this);
        if (args.length !== types.length) throw new TypeError(`${name}: wrong argument count`);
        const values = types.map((type, i) => CONFIG_ARGS[type](name, args[i]));
        const result = invokeWithPointers((loader) => Module['_lightusd_combined_config_set'](
          loader, key, values[0], values.length > 1 ? values[1] : 0), [loaderPtr]);
        if (result !== 0) throw new TypeError(`${name}: invalid receiver or value`);
      }
    });
  }

  const CONFIG_GETTERS = [
    ['getCombineUDIMTiles', 0, true], ['getDeferTangentComputation', 1, true],
    ['getEnableBoneReduction', 2, true], ['getEnableValueClips', 3, true],
    ['getMaxMemoryLimitMB', 4, false], ['getRoundBoneCount', 5, true],
    ['getSphereSubdivisions', 6, false], ['getTargetBoneCount', 7, false],
    ['getValueClipSampleRate', 8, false], ['getValueClipUseTimeRange', 9, true],
    ['getValueClipStartTime', 10, false], ['getValueClipEndTime', 11, false],
    ['getNativeFlattenRenderTree', 15, true], ['getNativeMaterialDedup', 16, true],
    ['getNativeMeshMerge', 17, true], ['getNativeMeshMergeBakeTransform', 18, true]
  ];
  for (const [name, key, isBool] of CONFIG_GETTERS) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: function() {
        const loaderPtr = requireLiveLoader(this);
        if (arguments.length !== 0) throw new TypeError(`${name}: wrong argument count`);
        let outPtr = 0;
        try {
          outPtr = Module['_lightusd_combined_alloc'](8);
          if (!outPtr) throw new RangeError(`${name}: allocation failed`);
          const result = invokeWithPointers((loader, out) =>
            Module['_lightusd_combined_config_get'](loader, key, out), [loaderPtr, outPtr]);
          if (result !== 0) throw new TypeError(`${name}: invalid receiver`);
          const value = new DataView(Module.HEAPU8.buffer, Number(outPtr), 8)
            .getFloat64(0, true);
          return isBool ? value !== 0 : value;
        } finally {
          if (outPtr) Module['_lightusd_combined_free'](outPtr);
        }
      }
    });
  }

  const MEMORY_STATS_SIZE = 104;
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'getMemoryStats', {
    configurable: true,
    value: function() {
      const loaderPtr = requireLiveLoader(this);
      if (arguments.length !== 0) throw new TypeError('getMemoryStats: wrong argument count');
      let outPtr = 0;
      try {
        outPtr = Module['_lightusd_combined_alloc'](MEMORY_STATS_SIZE);
        if (!outPtr) throw new RangeError('getMemoryStats: allocation failed');
        new DataView(Module.HEAPU8.buffer, Number(outPtr), MEMORY_STATS_SIZE)
          .setUint32(0, MEMORY_STATS_SIZE, true);
        const result = invokeWithPointers((loader, out) =>
          Module['_lightusd_combined_memory_stats_get'](loader, out), [loaderPtr, outPtr]);
        if (result !== 0) throw new TypeError('getMemoryStats: invalid receiver');
        const info = new DataView(Module.HEAPU8.buffer, Number(outPtr), MEMORY_STATS_SIZE);
        const field = (index) => info.getFloat64(8 + index * 8, true);
        const bufferMemoryBytes = field(7);
        return {
          numMeshes: field(0), numMaterials: field(1), numTextures: field(2),
          numImages: field(3), numBuffers: field(4), numNodes: field(5),
          numLights: field(6), bufferMemoryBytes,
          bufferMemoryMB: bufferMemoryBytes / (1024.0 * 1024.0),
          assetCacheCount: field(8), assetCacheSizeBytes: field(9),
          assetCacheMaxBytes: field(10), reorderedMeshCacheCount: field(11)
        };
      } finally {
        if (outPtr) Module['_lightusd_combined_free'](outPtr);
      }
    }
  });

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'debugLogMemory', {
    configurable: true,
    value: function(label) {
      const loaderPtr = requireLiveLoader(this);
      if (arguments.length !== 1) throw new TypeError('debugLogMemory: wrong argument count');
      if (typeof label !== 'string') throw new TypeError('debugLogMemory: expected label');
      const bytes = new TextEncoder().encode(label);
      let ptr = 0;
      try {
        if (bytes.length) {
          ptr = Module['_lightusd_combined_alloc'](bytes.length);
          if (!ptr) throw new RangeError('debugLogMemory: allocation failed');
          Module.HEAPU8.set(bytes, Number(ptr));
        }
        const heapBytes = invokeWithPointers((loader, labelPtr) =>
          Module['_lightusd_combined_debug_log_memory'](loader, labelPtr, bytes.length),
        [loaderPtr, ptr]);
        if (heapBytes < 0) throw new TypeError('debugLogMemory: invalid receiver');
        return {label, heapBytes};
      } finally {
        if (ptr) Module['_lightusd_combined_free'](ptr);
      }
    }
  });

  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'applyVariantSelection', {
    configurable: true,
    value: applyVariantSelection
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'extractVariants', {
    configurable: true,
    value: extractVariants
  });

  for (const name of ['nextFlattenBufferToSink', 'nextFlattenBufferToSinkRemap',
    'nextFlattenBufferToSinkRemapVariants']) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: flattenBufferToSink
    });
  }
  for (const name of ['nextFlattenMultiBufferToSink',
    'nextFlattenMultiBufferToSinkFetch', 'nextFlattenMultiBufferToSinkFetchRemap',
    'nextFlattenMultiBufferToSinkFetchRemapVariants']) {
    Object.defineProperty(Module.LightUSDLoaderNative.prototype, name, {
      configurable: true,
      value: flattenMultiBuffer
    });
  }
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenAsyncStep', {
    configurable: true,
    value: stepFlattenSession
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenUSDC', {
    configurable: true,
    value: flattenUSDC
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenBuffer', {
    configurable: true,
    value: flattenBuffer
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenBufferRemap', {
    configurable: true,
    value: flattenBuffer
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenBufferRemapVariants', {
    configurable: true,
    value: flattenBuffer
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenAsyncEnd', {
    configurable: true,
    value: endFlattenSession
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenAsyncProvideLayer', {
    configurable: true,
    value: provideFlattenLayer
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenAsyncBegin', {
    configurable: true,
    value: beginFlattenSession
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenAsyncBeginRemap', {
    configurable: true,
    value: beginFlattenSession
  });
  Object.defineProperty(Module.LightUSDLoaderNative.prototype, 'nextFlattenAsyncBeginRemapVariants', {
    configurable: true,
    value: beginFlattenSession
  });
}
