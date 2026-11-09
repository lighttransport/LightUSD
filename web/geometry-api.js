// SPDX-License-Identifier: Apache-2.0
{
  const live = new WeakMap();
  Module['SubdivStreamer'] = class SubdivStreamer {
    constructor() {
      const handle = Module['_lu_geometry_create'](2) >>> 0;
      if (!handle) throw new RangeError('Subdivision allocation failed');
      live.set(this, {handle, kind: 2, busy: 0});
    }
    delete() {
      const state = live.get(this);
      if (!state?.handle) throw new TypeError('Subdivision object already deleted');
      if (state.busy) throw new TypeError('Cannot delete an active subdivision object');
      Module['_lu_geometry_destroy'](state.handle); state.handle = 0;
    }
    isDeleted() { return !live.get(this)?.handle; }
  };
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
        block = Module['_lu_feature_alloc'](total);
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
      } finally { if (block) Module['_lu_feature_free'](block); }
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
      block = Module['_lu_feature_alloc'](total);
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
      const call = p => Module['_lu_geometry_subdiv_refine'](
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
      try { length = Module['_lu_geometry_subdiv_error'](state.handle, 0, 0); }
      catch (error) {
        if (!(error instanceof TypeError)) throw error;
        length = Module['_lu_geometry_subdiv_error'](state.handle, 0n, 0);
      }
      if (length < 0) throw new RangeError('refineStream: error query failed');
      const message = Module['_lu_feature_alloc'](length + 1);
      if (!message) throw new RangeError('refineStream: error allocation failed');
      try {
        const read = ptr => Module['_lu_geometry_subdiv_error'](state.handle, ptr, length);
        let size;
        try { size = read(message); }
        catch (error) {
          if (!(error instanceof TypeError)) throw error;
          size = read(BigInt(message));
        }
        if (size !== length) throw new RangeError('refineStream: error changed');
        return new TextDecoder().decode(Module.HEAPU8.subarray(Number(message), Number(message) + length));
      } finally { Module['_lu_feature_free'](message); }
    } finally {
      if (block) Module['_lu_feature_free'](block);
      subdivCallbacks.delete(callbackId);
      --state.busy;
    }
  }});


}
