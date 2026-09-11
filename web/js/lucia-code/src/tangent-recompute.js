import { forEachIndexedTriangle, validateIndexedMesh } from './indexed-mesh.js';

// Lengyel/Mikk-compatible vertex tangent frame for indexed triangle meshes.
// Tangents are vec4: xyz is orthonormal to the supplied normal and w is the
// bitangent handedness used by Three.js and LightUSD's WebGL bindings.
export function recomputeVertexTangents({ positions, indices, uvs, normals }) {
  const normalized = validateIndexedMesh({ positions, indices }), vertexCount = normalized.vertexCount, triangles = normalized.indices;
  const tangent = new Float64Array(vertexCount * 3), bitangent = new Float64Array(vertexCount * 3), out = new Float32Array(vertexCount * 4);
  forEachIndexedTriangle(triangles, (_face, a, b, c) => {
    if (a >= vertexCount || b >= vertexCount || c >= vertexCount || a === b || b === c || a === c) return;
    const ap = a * 3, bp = b * 3, cp = c * 3, au = a * 2, bu = b * 2, cu = c * 2;
    const e1x = positions[bp] - positions[ap], e1y = positions[bp + 1] - positions[ap + 1], e1z = positions[bp + 2] - positions[ap + 2];
    const e2x = positions[cp] - positions[ap], e2y = positions[cp + 1] - positions[ap + 1], e2z = positions[cp + 2] - positions[ap + 2];
    const du1 = uvs[bu] - uvs[au], dv1 = uvs[bu + 1] - uvs[au + 1], du2 = uvs[cu] - uvs[au], dv2 = uvs[cu + 1] - uvs[au + 1];
    const det = du1 * dv2 - du2 * dv1;
    if (!Number.isFinite(det) || Math.abs(det) <= 1e-12) return;
    const r = 1 / det, sx = (dv2 * e1x - dv1 * e2x) * r, sy = (dv2 * e1y - dv1 * e2y) * r, sz = (dv2 * e1z - dv1 * e2z) * r;
    const tx = (du1 * e2x - du2 * e1x) * r, ty = (du1 * e2y - du2 * e1y) * r, tz = (du1 * e2z - du2 * e1z) * r;
    for (const v of [a, b, c]) { const o = v * 3; tangent[o] += sx; tangent[o + 1] += sy; tangent[o + 2] += sz; bitangent[o] += tx; bitangent[o + 1] += ty; bitangent[o + 2] += tz; }
  });
  for (let i = 0; i < vertexCount; i++) {
    const o = i * 3, nx = normals[o], ny = normals[o + 1], nz = normals[o + 2], dot = nx * tangent[o] + ny * tangent[o + 1] + nz * tangent[o + 2];
    let tx = tangent[o] - nx * dot, ty = tangent[o + 1] - ny * dot, tz = tangent[o + 2] - nz * dot, length = Math.hypot(tx, ty, tz);
    if (!(length > 1e-12) || !Number.isFinite(length)) { tx = Math.abs(nx) < 0.9 ? 0 : 1; ty = Math.abs(nx) < 0.9 ? nz : 0; tz = Math.abs(nx) < 0.9 ? -ny : 0; length = Math.hypot(tx, ty, tz) || 1; }
    tx /= length; ty /= length; tz /= length;
    const cx = ny * tz - nz * ty, cy = nz * tx - nx * tz, cz = nx * ty - ny * tx;
    out[i * 4] = tx; out[i * 4 + 1] = ty; out[i * 4 + 2] = tz; out[i * 4 + 3] = cx * bitangent[o] + cy * bitangent[o + 1] + cz * bitangent[o + 2] < 0 ? -1 : 1;
  }
  return out;
}
