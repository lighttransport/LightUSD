// SPDX-License-Identifier: Apache-2.0
{
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
      block = Module['_lu_feature_alloc'](Math.max(total, 1));
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
    } finally { if (block) Module['_lu_feature_free'](block); }
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
        buffer = Module['_lu_feature_alloc'](size);
        if (!buffer) throw new RangeError('error: allocation failed');
        if (query(buffer, size) !== size) throw new RangeError('error: text changed');
        return new TextDecoder().decode(Module.HEAPU8.subarray(
          Number(buffer), Number(buffer) + size));
      } finally { if (buffer) Module['_lu_feature_free'](buffer); }
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


}
