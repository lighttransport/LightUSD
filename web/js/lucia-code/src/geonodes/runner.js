import { LuciaError } from '../utils.js';
import { workerPayloadBytes } from '../worker-policy.js';
import { GeoNodesCache, evaluateGraph, geometryToMeshData } from './evaluate.js';
import { geometryStats, hashValue, meshGeometry } from './geometry.js';

// Fan-triangulate authored USD topology into the indexed-mesh interchange.
export function triangulateAuthoredMesh({ points, faceVertexCounts, faceVertexIndices }) {
  if (!points || !faceVertexCounts || !faceVertexIndices) throw new LuciaError('LUCIA_GEONODES_SOURCE', 'The source mesh needs untimed points and valid face topology.');
  const indices = [];
  for (let face = 0, offset = 0; face < faceVertexCounts.length; offset += faceVertexCounts[face++])
    for (let k = 1; k + 1 < faceVertexCounts[face]; k++) indices.push(faceVertexIndices[offset], faceVertexIndices[offset + k], faceVertexIndices[offset + k + 1]);
  return { positions: Float32Array.from(points), indices: Uint32Array.from(indices) };
}

export function sourceInputHash(source) {
  return hashValue(source ? [source.positions, source.indices] : 'empty');
}

export async function evaluateGraphToMesh(graph, source, cache = null) {
  const groupInput = source ? meshGeometry(source.positions, source.indices) : undefined;
  const result = await evaluateGraph(graph, { groupInput, inputHash: sourceInputHash(source), cache });
  const mesh = geometryToMeshData(result.geometry);
  // Copy: the arrays may be shared with cached node outputs and are
  // transferred (detached) when posted back from the worker.
  return { positions: Float32Array.from(mesh.positions), indices: Uint32Array.from(mesh.indices), key: result.key, inputHash: result.inputHash, stats: geometryStats(result.geometry) };
}

// Owns a persistent evaluation worker so its node cache survives between
// parameter tweaks. Only the newest request resolves; superseded requests
// reject with LUCIA_CANCELLED. Falls back to in-thread evaluation when
// Workers are unavailable (Node tests).
export class GeoNodesRunner {
  constructor() { this.worker = null; this.nextId = 1; this.pending = new Map(); this.cache = new GeoNodesCache(); }

  ensureWorker() {
    if (this.worker || typeof Worker === 'undefined') return this.worker;
    const worker = this.worker = new Worker(new URL('../geonodes-worker.js', import.meta.url), { type: 'module' });
    worker.onmessage = ({ data }) => {
      const request = this.pending.get(data.id); if (!request) return;
      this.pending.delete(data.id);
      if (data.type === 'error') request.reject(new LuciaError(data.code || 'LUCIA_GEONODES_WORKER', data.message, data.details || {}));
      else request.resolve(data.result);
    };
    worker.onerror = (event) => { this.reset(new LuciaError('LUCIA_GEONODES_WORKER', event.message || 'Geometry node worker failed.')); };
    return worker;
  }

  async evaluate(graph, source) {
    for (const [id, request] of this.pending) { request.reject(new LuciaError('LUCIA_CANCELLED', 'Superseded by a newer geometry node evaluation.')); this.pending.delete(id); }
    const worker = this.ensureWorker();
    if (!worker) return evaluateGraphToMesh(graph, source, this.cache);
    const id = this.nextId++, payload = { type: 'evaluate', id, graph, source: source ? { positions: new Float32Array(source.positions), indices: new Uint32Array(source.indices) } : null };
    workerPayloadBytes(payload);
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      worker.postMessage(payload, payload.source ? [payload.source.positions.buffer, payload.source.indices.buffer] : []);
    });
  }

  reset(error = new LuciaError('LUCIA_CANCELLED', 'Geometry node evaluation was cancelled.')) {
    this.worker?.terminate(); this.worker = null; this.cache.clear();
    for (const request of this.pending.values()) request.reject(error);
    this.pending.clear();
  }

  dispose() { this.reset(); }
}
