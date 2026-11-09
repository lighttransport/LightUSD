import { raycastTriangles } from './projection-bake.js';
import { forEachIndexedTriangle } from './indexed-mesh.js';

function hash(value) { value |= 0; value = Math.imul(value ^ value >>> 16, 0x45d9f3b); value = Math.imul(value ^ value >>> 16, 0x45d9f3b); return (value ^ value >>> 16) >>> 0; }
function random(seed) { return (hash(seed) & 0xffffff) / 0x1000000; }

export function rasterizeOcclusionSurface({ positions, indices, uvs, normals, resolution }) {
  const pixels = resolution * resolution, points = new Float32Array(pixels * 3), surfaceNormals = new Float32Array(pixels * 3), covered = new Uint8Array(pixels);
  forEachIndexedTriangle(indices, (_face, a, b, c) => {
    const ids = [a, b, c], uv = ids.map((id) => [uvs[id * 2], uvs[id * 2 + 1]]), xs = uv.map(([u]) => u * resolution), ys = uv.map(([, v]) => (1 - v) * resolution);
    const minX = Math.max(0, Math.floor(Math.min(...xs))), maxX = Math.min(resolution - 1, Math.ceil(Math.max(...xs))), minY = Math.max(0, Math.floor(Math.min(...ys))), maxY = Math.min(resolution - 1, Math.ceil(Math.max(...ys)));
    const denominator = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2]);
    if (Math.abs(denominator) <= 1e-12) return;
    for (let y = minY; y <= maxY; y++) for (let x = minX; x <= maxX; x++) {
      const px = x + .5, py = y + .5, a = ((ys[1] - ys[2]) * (px - xs[2]) + (xs[2] - xs[1]) * (py - ys[2])) / denominator, b = ((ys[2] - ys[0]) * (px - xs[2]) + (xs[0] - xs[2]) * (py - ys[2])) / denominator, c = 1 - a - b;
      if (a < 0 || b < 0 || c < 0) continue;
      const pixel = y * resolution + x; if (covered[pixel]) continue; covered[pixel] = 1;
      for (const [weight, id] of [[a, ids[0]], [b, ids[1]], [c, ids[2]]]) { const po = pixel * 3, offset = id * 3; points[po] += weight * positions[offset]; points[po + 1] += weight * positions[offset + 1]; points[po + 2] += weight * positions[offset + 2]; surfaceNormals[po] += weight * normals[offset]; surfaceNormals[po + 1] += weight * normals[offset + 1]; surfaceNormals[po + 2] += weight * normals[offset + 2]; }
      const no = pixel * 3, length = Math.hypot(surfaceNormals[no], surfaceNormals[no + 1], surfaceNormals[no + 2]) || 1; surfaceNormals[no] /= length; surfaceNormals[no + 1] /= length; surfaceNormals[no + 2] /= length;
    }
  });
  return { points, surfaceNormals, covered };
}

export function occlusionHemisphere(normal, seed) {
  const r = Math.sqrt(random(seed)), phi = Math.PI * 2 * random(seed + 1), z = Math.sqrt(Math.max(0, 1 - r * r)), up = Math.abs(normal[1]) < .9 ? [0, 1, 0] : [1, 0, 0], tx = up[1] * normal[2] - up[2] * normal[1], ty = up[2] * normal[0] - up[0] * normal[2], tz = up[0] * normal[1] - up[1] * normal[0], tl = Math.hypot(tx, ty, tz) || 1, t = [tx / tl, ty / tl, tz / tl], b = [normal[1] * t[2] - normal[2] * t[1], normal[2] * t[0] - normal[0] * t[2], normal[0] * t[1] - normal[1] * t[0]];
  return [t[0] * r * Math.cos(phi) + b[0] * r * Math.sin(phi) + normal[0] * z, t[1] * r * Math.cos(phi) + b[1] * r * Math.sin(phi) + normal[1] * z, t[2] * r * Math.cos(phi) + b[2] * r * Math.sin(phi) + normal[2] * z];
}

export function buildOcclusionRays(surface, samples = 1) {
  const origins = [], directions = [], pixelIds = [], count = Math.max(1, Math.min(16, samples | 0));
  for (let pixel = 0; pixel < surface.covered.length; pixel++) if (surface.covered[pixel]) { const po = pixel * 3, normal = [surface.surfaceNormals[po], surface.surfaceNormals[po + 1], surface.surfaceNormals[po + 2]]; for (let sample = 0; sample < count; sample++) { origins.push(surface.points[po] + normal[0] * 1e-4, surface.points[po + 1] + normal[1] * 1e-4, surface.points[po + 2] + normal[2] * 1e-4); directions.push(...occlusionHemisphere(normal, pixel * 17 + sample)); pixelIds.push(pixel); } }
  return { origins: new Float32Array(origins), directions: new Float32Array(directions), pixelIds: Uint32Array.from(pixelIds), samples: count };
}

export function bakeOcclusionCPU({ positions, indices, uvs, normals, resolution, samples = 1, radius = 1 }) {
  const surface = rasterizeOcclusionSurface({ positions, indices, uvs, normals, resolution }), rays = buildOcclusionRays(surface, samples), hits = new Uint16Array(resolution * resolution), visibility = rays.origins.length ? raycastTriangles({ origins: rays.origins, directions: rays.directions, positions, indices, maxDistance: radius > 0 ? radius : Infinity }).triangle : new Int32Array();
  for (let i = 0; i < visibility.length; i++) if (visibility[i] >= 0) hits[rays.pixelIds[i]]++;
  const pixels = new Uint8ClampedArray(resolution * resolution * 4); let covered = 0;
  for (let pixel = 0; pixel < surface.covered.length; pixel++) { if (surface.covered[pixel]) covered++; const value = surface.covered[pixel] ? Math.round((1 - hits[pixel] / rays.samples) * 255) : 0, offset = pixel * 4; pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = value; pixels[offset + 3] = surface.covered[pixel] ? 255 : 0; }
  return { pixels, resolution, covered, total: resolution * resolution, missedTexels: resolution * resolution - covered, coveredRatio: covered / (resolution * resolution) };
}
