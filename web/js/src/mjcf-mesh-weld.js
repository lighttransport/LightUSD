// SPDX-License-Identifier: Apache-2.0
// Mesh welding for MJCF mesh assets, matching the native urdf-to-usd example.
//
// STL files are triangle soups (every corner repeats its position), and
// three.js loaders return non-indexed geometry for both STL and OBJ. MuJoCo
// removes repeated STL vertices and computes one normal per vertex, so the
// welded, indexed mesh is both ~6x smaller and closer to MuJoCo's shading.
import * as THREE from 'three';

const bitsKey = (u32, offset, count) => {
  let key = '';
  for (let i = 0; i < count; ++i) key += u32[offset + i] + ',';
  return key;
};

// MuJoCo vertex normals: face normals weighted by area; unless smoothnormal,
// a face whose normal deviates from the vertex average by more than
// acos(0.8) is left out so sharp edges stay sharp.
export function mujocoVertexNormals(positions, indices, smoothnormal = false) {
  const nv = positions.length / 3;
  const nf = indices.length / 3;
  const acc = new Float64Array(nv * 3);
  const face = new Float64Array(nf * 3);
  for (let f = 0; f < nf; ++f) {
    const a = indices[f * 3] * 3, b = indices[f * 3 + 1] * 3, c = indices[f * 3 + 2] * 3;
    const e1x = positions[b] - positions[a], e1y = positions[b + 1] - positions[a + 1], e1z = positions[b + 2] - positions[a + 2];
    const e2x = positions[c] - positions[a], e2y = positions[c + 1] - positions[a + 1], e2z = positions[c + 2] - positions[a + 2];
    const nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
    face[f * 3] = nx; face[f * 3 + 1] = ny; face[f * 3 + 2] = nz;
    for (const v of [a, b, c]) { acc[v] += nx; acc[v + 1] += ny; acc[v + 2] += nz; }
  }
  const sum = Float64Array.from(acc);
  if (!smoothnormal) {
    for (let f = 0; f < nf; ++f) {
      const fl = Math.hypot(face[f * 3], face[f * 3 + 1], face[f * 3 + 2]);
      if (!(fl > 0)) continue;
      for (let j = 0; j < 3; ++j) {
        const v = indices[f * 3 + j] * 3;
        const al = Math.hypot(acc[v], acc[v + 1], acc[v + 2]);
        if (!(al > 0)) continue;
        const dot = (acc[v] * face[f * 3] + acc[v + 1] * face[f * 3 + 1] + acc[v + 2] * face[f * 3 + 2]) / (al * fl);
        if (dot < 0.8) {
          sum[v] -= face[f * 3]; sum[v + 1] -= face[f * 3 + 1]; sum[v + 2] -= face[f * 3 + 2];
        }
      }
    }
  }
  const normals = new Float32Array(nv * 3);
  for (let v = 0; v < nv * 3; v += 3) {
    let src = sum;
    let l = Math.hypot(sum[v], sum[v + 1], sum[v + 2]);
    if (!(l > 0)) { src = acc; l = Math.hypot(acc[v], acc[v + 1], acc[v + 2]); }
    if (l > 0) { normals[v] = src[v] / l; normals[v + 1] = src[v + 1] / l; normals[v + 2] = src[v + 2] / l; }
  }
  return normals;
}

// Weld a (possibly non-indexed) geometry. mode 'stl': merge bit-identical
// positions and recompute MuJoCo normals. mode 'keep': merge corners that
// agree on position, normal and uv, keeping the authored attributes.
export function weldMeshGeometry(geometry, { mode = 'keep', smoothnormal = false } = {}) {
  const pos = geometry.getAttribute('position');
  if (!pos || pos.count < 3) return geometry;
  const nrm = mode === 'stl' ? null : geometry.getAttribute('normal');
  const uv = mode === 'stl' ? null : geometry.getAttribute('uv');
  const index = geometry.getIndex();
  const corners = index ? index.count : pos.count;
  const p32 = new Float32Array(3), n32 = new Float32Array(3), t32 = new Float32Array(2);
  const pu = new Uint32Array(p32.buffer), nu = new Uint32Array(n32.buffer), tu = new Uint32Array(t32.buffer);
  const map = new Map();
  const outPos = [], outNrm = [], outUv = [], outIdx = new Uint32Array(corners);
  for (let c = 0; c < corners; ++c) {
    const i = index ? index.getX(c) : c;
    p32[0] = pos.getX(i); p32[1] = pos.getY(i); p32[2] = pos.getZ(i);
    let key = bitsKey(pu, 0, 3);
    if (nrm) { n32[0] = nrm.getX(i); n32[1] = nrm.getY(i); n32[2] = nrm.getZ(i); key += bitsKey(nu, 0, 3); }
    if (uv) { t32[0] = uv.getX(i); t32[1] = uv.getY(i); key += bitsKey(tu, 0, 2); }
    let out = map.get(key);
    if (out === undefined) {
      out = outPos.length / 3;
      map.set(key, out);
      outPos.push(p32[0], p32[1], p32[2]);
      if (nrm) outNrm.push(n32[0], n32[1], n32[2]);
      if (uv) outUv.push(t32[0], t32[1]);
    }
    outIdx[c] = out;
  }
  const welded = new THREE.BufferGeometry();
  const positions = new Float32Array(outPos);
  welded.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  if (mode === 'stl') {
    welded.setAttribute('normal', new THREE.Float32BufferAttribute(
      mujocoVertexNormals(positions, outIdx, smoothnormal), 3));
  } else if (nrm) {
    welded.setAttribute('normal', new THREE.Float32BufferAttribute(outNrm, 3));
  }
  if (uv) welded.setAttribute('uv', new THREE.Float32BufferAttribute(outUv, 2));
  welded.setIndex(new THREE.BufferAttribute(outIdx, 1));
  return welded;
}
