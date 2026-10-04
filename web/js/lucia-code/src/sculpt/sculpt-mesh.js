import { normalizeIndexedMesh } from '../indexed-mesh.js';
import { LuciaError } from '../utils.js';

export const SCULPT_MAX_VERTICES = 4_000_000;
// Hashed cell key; collisions only add candidates (distances are re-checked).
const cellKey = (x, y, z) => Math.imul(x, 73856093) ^ Math.imul(y, 19349663) ^ Math.imul(z, 83492791);

// Working copy of a fixed-topology mesh for sculpting. Render meshes split
// vertices at UV/normal seams, so coincident vertices are welded into one
// "representative" and always move together; brushes only touch reps.
export class SculptMesh {
  constructor({ positions, indices = null }) {
    let normalized;
    try { normalized = normalizeIndexedMesh({ positions, indices }); }
    catch (error) { throw new LuciaError('LUCIA_SCULPT_MESH', `Mesh cannot be sculpted: ${error.message}`); }
    const count = normalized.positions.length / 3;
    if (count > SCULPT_MAX_VERTICES) throw new LuciaError('LUCIA_SCULPT_BUDGET', `Sculpting supports up to ${SCULPT_MAX_VERTICES} vertices.`);
    this.positions = Float32Array.from(normalized.positions);
    this.original = Float32Array.from(normalized.positions);
    this.indices = Uint32Array.from(normalized.indices);
    this.vertexCount = count;
    this.mask = new Float32Array(count);
    this.weld();
    this.buildTopology();
    this.normals = new Float32Array(count * 3);
    this.updateNormals(null);
    this.grid = null;
  }

  weld() {
    // Sort by exact coordinate bits, then group equal runs (no string keys).
    const bits = new Uint32Array(this.positions.buffer, this.positions.byteOffset, this.positions.length), order = new Uint32Array(this.vertexCount), rep = new Uint32Array(this.vertexCount);
    for (let i = 0; i < this.vertexCount; i++) order[i] = i;
    order.sort((a, b) => bits[a * 3] - bits[b * 3] || bits[a * 3 + 1] - bits[b * 3 + 1] || bits[a * 3 + 2] - bits[b * 3 + 2] || a - b);
    const reps = [];
    for (let k = 0; k < order.length; k++) {
      const i = order[k], j = k ? order[k - 1] : -1;
      if (j >= 0 && bits[i * 3] === bits[j * 3] && bits[i * 3 + 1] === bits[j * 3 + 1] && bits[i * 3 + 2] === bits[j * 3 + 2]) rep[i] = rep[j];
      else { rep[i] = i; reps.push(i); }
    }
    this.rep = rep;
    this.reps = Uint32Array.from(reps).sort();
    // members: CSR list of all vertices sharing a representative.
    const counts = new Uint32Array(this.vertexCount + 1);
    for (let i = 0; i < this.vertexCount; i++) counts[rep[i] + 1]++;
    for (let i = 0; i < this.vertexCount; i++) counts[i + 1] += counts[i];
    const fill = counts.slice(0, -1), members = new Uint32Array(this.vertexCount);
    for (let i = 0; i < this.vertexCount; i++) members[fill[rep[i]]++] = i;
    this.memberOffsets = counts; this.members = members;
  }

  buildTopology() {
    // Rep-level neighbor lists and vertex→face incidence, both CSR.
    const faceCount = this.indices.length / 3, degree = new Uint32Array(this.vertexCount + 1), faceCounts = new Uint32Array(this.vertexCount + 1);
    const corner = (f, c) => this.rep[this.indices[f * 3 + c]];
    for (let f = 0; f < faceCount; f++) for (let c = 0; c < 3; c++) { const v = corner(f, c); degree[v + 1] += 2; faceCounts[v + 1]++; }
    for (let i = 0; i < this.vertexCount; i++) { degree[i + 1] += degree[i]; faceCounts[i + 1] += faceCounts[i]; }
    const raw = new Uint32Array(degree[this.vertexCount]), rawFill = degree.slice(0, -1), faces = new Uint32Array(faceCounts[this.vertexCount]), faceFill = faceCounts.slice(0, -1);
    for (let f = 0; f < faceCount; f++) for (let c = 0; c < 3; c++) {
      const v = corner(f, c), a = corner(f, (c + 1) % 3), b = corner(f, (c + 2) % 3);
      raw[rawFill[v]++] = a; raw[rawFill[v]++] = b; faces[faceFill[v]++] = f;
    }
    // Deduplicate each neighbor list in place (lists are short).
    const offsets = new Uint32Array(this.vertexCount + 1), neighbors = new Uint32Array(raw.length);
    let out = 0;
    for (let v = 0; v < this.vertexCount; v++) {
      offsets[v] = out;
      const list = raw.subarray(degree[v], degree[v + 1]).sort();
      for (let k = 0; k < list.length; k++) if (list[k] !== v && (k === 0 || list[k] !== list[k - 1])) neighbors[out++] = list[k];
    }
    offsets[this.vertexCount] = out;
    this.neighborOffsets = offsets; this.neighbors = neighbors.slice(0, out);
    this.faceOffsets = faceCounts; this.vertexFaces = faces;
  }

  // Recompute area-weighted normals for the given reps (all when null).
  updateNormals(reps) {
    const list = reps || this.reps, p = this.positions, n = this.normals;
    for (const v of list) {
      let nx = 0, ny = 0, nz = 0;
      for (let k = this.faceOffsets[v]; k < this.faceOffsets[v + 1]; k++) {
        const f = this.vertexFaces[k] * 3, a = this.indices[f] * 3, b = this.indices[f + 1] * 3, c = this.indices[f + 2] * 3;
        const ux = p[b] - p[a], uy = p[b + 1] - p[a + 1], uz = p[b + 2] - p[a + 2], wx = p[c] - p[a], wy = p[c + 1] - p[a + 1], wz = p[c + 2] - p[a + 2];
        nx += uy * wz - uz * wy; ny += uz * wx - ux * wz; nz += ux * wy - uy * wx;
      }
      const length = Math.hypot(nx, ny, nz) || 1;
      n[v * 3] = nx / length; n[v * 3 + 1] = ny / length; n[v * 3 + 2] = nz / length;
    }
  }

  // Uniform grid over reps, rebuilt at stroke start. Vertices move during a
  // stroke, so queries widen the search by the stroke's max displacement.
  buildGrid(cellSize) {
    const size = Math.max(cellSize, 1e-6), cells = new Map(), p = this.positions;
    for (const v of this.reps) { const key = cellKey(Math.floor(p[v * 3] / size), Math.floor(p[v * 3 + 1] / size), Math.floor(p[v * 3 + 2] / size)), list = cells.get(key); if (list) list.push(v); else cells.set(key, [v]); }
    this.grid = { size, cells, drift: 0 };
  }

  queryRadius(center, radius) {
    if (!this.grid || this.grid.size < radius * 0.5) this.buildGrid(radius);
    const { size, cells, drift } = this.grid, reach = radius + drift, p = this.positions, out = [];
    const lo = center.map((c) => Math.floor((c - reach) / size)), hi = center.map((c) => Math.floor((c + reach) / size)), r2 = radius * radius;
    for (let x = lo[0]; x <= hi[0]; x++) for (let y = lo[1]; y <= hi[1]; y++) for (let z = lo[2]; z <= hi[2]; z++) for (const v of cells.get(cellKey(x, y, z)) || []) {
      const dx = p[v * 3] - center[0], dy = p[v * 3 + 1] - center[1], dz = p[v * 3 + 2] - center[2], d2 = dx * dx + dy * dy + dz * dz;
      if (d2 <= r2) out.push([v, Math.sqrt(d2)]);
    }
    return out;
  }

  // Writes rep position to every coincident member; returns the move distance
  // so brushes can widen the grid search by the accumulated drift.
  setRepPosition(v, x, y, z) {
    const p = this.positions, moved = Math.hypot(x - p[v * 3], y - p[v * 3 + 1], z - p[v * 3 + 2]);
    for (let k = this.memberOffsets[v]; k < this.memberOffsets[v + 1]; k++) { const m = this.members[k] * 3; p[m] = x; p[m + 1] = y; p[m + 2] = z; }
    return moved;
  }

  setRepMask(v, value) {
    for (let k = this.memberOffsets[v]; k < this.memberOffsets[v + 1]; k++) this.mask[this.members[k]] = value;
  }

  // Raycast in mesh-local space (Möller–Trumbore); used by headless tests and
  // as a fallback when no renderer is available.
  raycast(origin, direction) {
    const p = this.positions; let best = null;
    for (let f = 0; f < this.indices.length; f += 3) {
      const a = this.indices[f] * 3, b = this.indices[f + 1] * 3, c = this.indices[f + 2] * 3;
      const e1 = [p[b] - p[a], p[b + 1] - p[a + 1], p[b + 2] - p[a + 2]], e2 = [p[c] - p[a], p[c + 1] - p[a + 1], p[c + 2] - p[a + 2]];
      const h = [direction[1] * e2[2] - direction[2] * e2[1], direction[2] * e2[0] - direction[0] * e2[2], direction[0] * e2[1] - direction[1] * e2[0]];
      const det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2]; if (Math.abs(det) < 1e-12) continue;
      const s = [origin[0] - p[a], origin[1] - p[a + 1], origin[2] - p[a + 2]], u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) / det; if (u < 0 || u > 1) continue;
      const q = [s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]], w = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) / det; if (w < 0 || u + w > 1) continue;
      const t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
      if (t > 1e-9 && (!best || t < best.distance)) {
        const n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]], l = Math.hypot(...n) || 1;
        best = { distance: t, point: origin.map((o, i) => o + direction[i] * t), normal: n.map((x) => x / l), face: f / 3 };
      }
    }
    return best;
  }

  // Map sculpted positions onto the authored USD `points` array. Render
  // meshes may split or reorder vertices, so authored points are matched by
  // their exact float32 position against the pre-stroke baseline.
  mapToAuthoredPoints(authored) {
    const baseline = new Uint32Array(this.original.buffer, this.original.byteOffset, this.original.length), lookup = new Map();
    for (let i = 0; i < this.vertexCount; i++) {
      const key = `${baseline[i * 3]},${baseline[i * 3 + 1]},${baseline[i * 3 + 2]}`;
      if (!lookup.has(key)) lookup.set(key, i);
    }
    const source = Float32Array.from(authored), bits = new Uint32Array(source.buffer), out = new Float32Array(source.length);
    for (let i = 0; i < source.length / 3; i++) {
      const vertex = lookup.get(`${bits[i * 3]},${bits[i * 3 + 1]},${bits[i * 3 + 2]}`);
      if (vertex == null) throw new LuciaError('LUCIA_SCULPT_MAPPING', 'Sculpted mesh no longer matches the authored USD points (points may be transformed, subdivided or animated).');
      out[i * 3] = this.positions[vertex * 3]; out[i * 3 + 1] = this.positions[vertex * 3 + 1]; out[i * 3 + 2] = this.positions[vertex * 3 + 2];
    }
    return out;
  }

  changed() {
    for (let i = 0; i < this.positions.length; i++) if (this.positions[i] !== this.original[i]) return true;
    return false;
  }

  // Accept the current state as the new baseline (after a commit).
  commit() { this.original.set(this.positions); }
  revert() { this.positions.set(this.original); this.updateNormals(null); this.grid = null; }
}
