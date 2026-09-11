import { validateBakeResult } from './texture-bake.js';
import { buildOcclusionRays, rasterizeOcclusionSurface } from './occlusion-bake.js';
import { expandIndexedAttribute } from './indexed-mesh.js';

let createLightUSD;

self.onmessage = async ({ data }) => {
  if (data?.type !== 'bake-occlusion') return;
  try {
    createLightUSD ||= (await import('../../src/lightusd/lightusd.js')).default;
    const module = await createLightUSD(), tracer = new module.LightRTPathTracer();
    try {
      const resolution = Math.max(64, Math.min(2048, data.resolution | 0)), samples = Math.max(1, Math.min(16, data.samples | 0));
    const triangles = data.indices.length / 3, soupPositions = expandIndexedAttribute(data.positions, data.indices, 3), soupNormals = expandIndexedAttribute(data.normals, data.indices, 3), soupColors = new Float32Array(triangles * 9).fill(1), params = new Float32Array(triangles * 12), materialIds = new Int32Array(triangles), materials = new Float32Array(10);
    if (!tracer.build(soupPositions, soupNormals, soupColors, params, materialIds, materials)) throw new Error(tracer.error());
    const surface = rasterizeOcclusionSurface({ ...data, resolution }), rays = buildOcclusionRays(surface, samples), output = new Uint8ClampedArray(resolution * resolution * 4), coveredCount = surface.covered.reduce((sum, value) => sum + value, 0);
    const visibility = rays.origins.length ? tracer.occluded(rays.origins, rays.directions, data.radius > 0 ? data.radius : 1e30) : new Uint8Array();
    const hits = new Uint16Array(resolution * resolution); for (let i = 0; i < visibility.length; i++) hits[rays.pixelIds[i]] += visibility[i];
    for (let pixel = 0; pixel < surface.covered.length; pixel++) { const value = surface.covered[pixel] ? Math.round((1 - hits[pixel] / rays.samples) * 255) : 0, o = pixel * 4; output[o] = output[o + 1] = output[o + 2] = value; output[o + 3] = surface.covered[pixel] ? 255 : 0; }
      const result = validateBakeResult({ type: 'result', pixels: output, resolution, covered: coveredCount, total: resolution * resolution, missedTexels: resolution * resolution - coveredCount, coveredRatio: coveredCount / (resolution * resolution) }); self.postMessage(result, [output.buffer]);
    } finally {
      tracer.delete();
    }
  } catch (error) { self.postMessage({ type: 'error', message: error?.message || String(error) }); }
};
