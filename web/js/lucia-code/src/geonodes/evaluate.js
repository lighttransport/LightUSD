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
  return { geometry: results.get(outputNode.id).geometry, key: keys.get(outputNode.id), inputHash: sourceStamp };
}

// Converts an evaluation result to the mesh payload written to USD.
// Instances are realized; loose points cannot be represented on a Mesh prim.
export function geometryToMeshData(geometry) {
  const mesh = realize(geometry).mesh;
  if (!mesh || !mesh.indices.length) throw new LuciaError('LUCIA_GEONODES_EMPTY', 'The geometry node graph produced no mesh faces.');
  return { positions: mesh.positions, indices: mesh.indices };
}
