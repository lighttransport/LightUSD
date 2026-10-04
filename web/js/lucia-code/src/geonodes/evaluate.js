import { LuciaError } from '../utils.js';
import { getNodeType, realize } from './nodes.js';
import { topoOrder, validateGraph } from './graph.js';
import { EMPTY_GEOMETRY, hashValue } from './geometry.js';

// Per-node output cache. A node's key is the hash of its type, parameters and
// upstream keys, so editing one parameter only re-evaluates downstream nodes.
export class GeoNodesCache {
  constructor(maxEntries = 256) { this.entries = new Map(); this.maxEntries = maxEntries; this.hits = 0; this.misses = 0; }
  get(key) { const value = this.entries.get(key); if (value) { this.hits++; this.entries.delete(key); this.entries.set(key, value); } else this.misses++; return value; }
  set(key, value) { this.entries.set(key, value); while (this.entries.size > this.maxEntries) this.entries.delete(this.entries.keys().next().value); }
  clear() { this.entries.clear(); this.hits = 0; this.misses = 0; }
}

// Evaluates a validated (or raw) graph. `groupInput` is the source geometry,
// `inputHash` a stable stamp of it (computed when omitted).
export async function evaluateGraph(graph, { groupInput = EMPTY_GEOMETRY, inputHash = null, cache = null, signal = null, onProgress = null } = {}) {
  const normalized = validateGraph(graph), order = topoOrder(normalized), nodes = new Map(normalized.nodes.map((node) => [node.id, node]));
  const sourceStamp = inputHash || hashValue(groupInput?.mesh ? [groupInput.mesh.positions, groupInput.mesh.indices] : 'empty');
  const linksTo = new Map();
  for (const link of normalized.links) linksTo.set(`${link.to[0]}.${link.to[1]}`, link.from);
  const results = new Map(), keys = new Map();
  for (let step = 0; step < order.length; step++) {
    if (signal?.aborted) throw new LuciaError('LUCIA_GEONODES_CANCELLED', 'Geometry node evaluation was cancelled.');
    const node = nodes.get(order[step]), definition = getNodeType(node.type);
    const upstream = definition.inputs.map((socket) => { const from = linksTo.get(`${node.id}.${socket.name}`); return from ? `${keys.get(from[0])}:${from[1]}` : null; });
    const key = hashValue({ type: node.type, params: node.params, upstream, source: node.type === 'GroupInput' ? sourceStamp : null });
    keys.set(node.id, key);
    let outputs = cache?.get(key);
    if (!outputs) {
      const inputs = {};
      for (const socket of definition.inputs) {
        const from = linksTo.get(`${node.id}.${socket.name}`);
        inputs[socket.name] = from ? results.get(from[0])[from[1]] : node.params[socket.name] ?? socket.default ?? null;
      }
      try { outputs = await definition.evaluate(inputs, { groupInput }); }
      catch (error) { if (error instanceof LuciaError) { error.details = { ...error.details, nodeId: node.id }; throw error; } throw new LuciaError('LUCIA_GEONODES_EVAL', `Node ${node.id} (${node.type}) failed: ${error.message}`, { nodeId: node.id }); }
      cache?.set(key, outputs);
    }
    results.set(node.id, outputs);
    onProgress?.({ percentage: Math.round((step + 1) / order.length * 100), message: `Evaluated ${definition.label}` });
  }
  const outputNode = normalized.nodes.find((node) => node.type === 'GroupOutput');
  const output = results.get(outputNode.id);
  return { geometry: output.geometry, realizeInstances: output.realizeInstances, key: keys.get(outputNode.id), inputHash: sourceStamp };
}

// Decompose a column-major affine matrix into translate / quaternion (w,x,y,z)
// / scale. Returns null for shear or degenerate matrices, which a
// PointInstancer cannot represent.
export function decomposeInstanceTransform(m, tolerance = 1e-4) {
  const columns = [[m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]], scale = columns.map((c) => Math.hypot(...c));
  if (scale.some((s) => !(s > 1e-12) || !Number.isFinite(s)) || Math.abs(m[3]) + Math.abs(m[7]) + Math.abs(m[11]) > tolerance || Math.abs(m[15] - 1) > tolerance) return null;
  const r = columns.map((c, i) => c.map((v) => v / scale[i]));
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  if (Math.abs(dot(r[0], r[1])) > tolerance || Math.abs(dot(r[0], r[2])) > tolerance || Math.abs(dot(r[1], r[2])) > tolerance) return null;
  const cross = [r[0][1] * r[1][2] - r[0][2] * r[1][1], r[0][2] * r[1][0] - r[0][0] * r[1][2], r[0][0] * r[1][1] - r[0][1] * r[1][0]];
  if (dot(cross, r[2]) < 0) { scale[0] = -scale[0]; r[0] = r[0].map((v) => -v); }
  // Rotation matrix (row i, col j) = r[j][i] → quaternion.
  const a = (i, j) => r[j][i], trace = a(0, 0) + a(1, 1) + a(2, 2);
  let w, x, y, z;
  if (trace > 0) { const s = Math.sqrt(trace + 1) * 2; w = s / 4; x = (a(2, 1) - a(1, 2)) / s; y = (a(0, 2) - a(2, 0)) / s; z = (a(1, 0) - a(0, 1)) / s; }
  else if (a(0, 0) > a(1, 1) && a(0, 0) > a(2, 2)) { const s = Math.sqrt(1 + a(0, 0) - a(1, 1) - a(2, 2)) * 2; w = (a(2, 1) - a(1, 2)) / s; x = s / 4; y = (a(0, 1) + a(1, 0)) / s; z = (a(0, 2) + a(2, 0)) / s; }
  else if (a(1, 1) > a(2, 2)) { const s = Math.sqrt(1 + a(1, 1) - a(0, 0) - a(2, 2)) * 2; w = (a(0, 2) - a(2, 0)) / s; x = (a(0, 1) + a(1, 0)) / s; y = s / 4; z = (a(1, 2) + a(2, 1)) / s; }
  else { const s = Math.sqrt(1 + a(2, 2) - a(0, 0) - a(1, 1)) * 2; w = (a(1, 0) - a(0, 1)) / s; x = (a(0, 2) + a(2, 0)) / s; y = (a(1, 2) + a(2, 1)) / s; z = s / 4; }
  return { translate: [m[12], m[13], m[14]], orientation: [w, x, y, z], scale };
}

// Converts an evaluation result to the USD output payload: the realized mesh
// part plus, unless realizing, a PointInstancer description. Instance groups
// whose transforms cannot be decomposed (shear) are realized into the mesh.
export function geometryToOutput(geometry, { realizeInstances = false } = {}) {
  if (realizeInstances || !geometry?.instances?.length) return { mesh: realize(geometry).mesh?.indices.length ? realize(geometry).mesh : null, instancer: null };
  const prototypes = [], protoIndex = new Map(), positions = [], orientations = [], scales = [], protoIndices = [], fallback = [];
  for (const group of geometry.instances) {
    const decomposed = [];
    for (let t = 0; t < group.transforms.length; t += 16) { const d = decomposeInstanceTransform(group.transforms.subarray(t, t + 16)); if (!d) { decomposed.length = 0; break; } decomposed.push(d); }
    if (!decomposed.length) { fallback.push(group); continue; }
    if (!protoIndex.has(group.mesh)) { protoIndex.set(group.mesh, prototypes.length); prototypes.push({ positions: group.mesh.positions, indices: group.mesh.indices }); }
    const index = protoIndex.get(group.mesh);
    for (const d of decomposed) { positions.push(...d.translate); orientations.push(...d.orientation); scales.push(...d.scale); protoIndices.push(index); }
  }
  const base = realize({ ...geometry, instances: fallback }).mesh;
  const instancer = protoIndices.length ? { prototypes, positions: new Float32Array(positions), orientations: new Float32Array(orientations), scales: new Float32Array(scales), protoIndices: new Int32Array(protoIndices) } : null;
  return { mesh: base?.indices.length ? base : null, instancer };
}

// Converts an evaluation result to the mesh payload written to USD.
// Instances are realized; loose points cannot be represented on a Mesh prim.
export function geometryToMeshData(geometry) {
  const mesh = realize(geometry).mesh;
  if (!mesh || !mesh.indices.length) throw new LuciaError('LUCIA_GEONODES_EMPTY', 'The geometry node graph produced no mesh faces.');
  return { positions: mesh.positions, indices: mesh.indices };
}
