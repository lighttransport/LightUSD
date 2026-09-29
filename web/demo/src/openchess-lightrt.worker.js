// LightRTPathTracer is retained in the deprecated combined module for this path tracing worker.
import createLightUSDModule from 'lightusd/lightusd.js';

let native = null;
let tracer = null;
let cancelledGeneration = 0;

async function ensureNative() {
  if (!native) native = await createLightUSDModule();
  return native;
}

self.onmessage = async ({ data }) => {
  const { type, generation = 0 } = data;
  try {
    if (type === 'build') {
      const module = await ensureNative();
      if (typeof module.LightRTPathTracer !== 'function') throw new Error('WASM module has no LightRTPathTracer binding');
      tracer?.delete?.();
      tracer = new module.LightRTPathTracer();
      const s = data.scene;
      if (!tracer.build(s.positions, s.normals, s.colors, s.vertexParams, s.materialIds, s.materials)) {
        throw new Error(tracer.error());
      }
      self.postMessage({ type: 'built', generation, triangles: tracer.triangleCount() });
    } else if (type === 'export-webgpu') {
      const scene = tracer.webGPUScene();
      const transfer = ['nodes', 'blocks', 'normals', 'colors', 'vertexParams', 'materialIds', 'materials']
        .map((key) => scene[key].buffer);
      self.postMessage({ type: 'webgpu-scene', generation, scene }, transfer);
    } else if (type === 'trace') {
      cancelledGeneration = generation;
      const result = tracer.trace(data.invViewProjection, data.cameraPosition,
        data.width, data.height, data.sampleStart, data.sampleCount,
        data.bounces, data.exposure);
      if (generation !== cancelledGeneration) return;
      self.postMessage({ type: 'samples', generation, sampleStart: data.sampleStart,
        sampleCount: data.sampleCount, width: data.width, height: data.height,
        pixels: result }, [result.buffer]);
    } else if (type === 'cancel') {
      cancelledGeneration = generation;
    }
  } catch (error) {
    self.postMessage({ type: 'error', generation, message: error?.message || String(error) });
  }
};
