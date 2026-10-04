import { LuciaError } from '../utils.js';
import { workerPayloadBytes } from '../worker-policy.js';
import { GeoNodesCache, evaluateGraph, geometryToMeshData, geometryToOutput } from './evaluate.js';
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

// Evaluates a graph into:
//   positions/indices — the fully realized mesh (preview and Apply)
//   output            — what Commit authors: { mesh|null, instancer|null }
// Arrays are fresh copies: cached node outputs may share buffers and results
// are transferred (detached) when posted back from the worker.
export async function evaluateGraphToMesh(graph, source, cache = null) {
  const groupInput = source ? meshGeometry(source.positions, source.indices) : undefined;
  const result = await evaluateGraph(graph, { groupInput, inputHash: sourceInputHash(source), cache });
  const mesh = geometryToMeshData(result.geometry), output = geometryToOutput(result.geometry, { realizeInstances: result.realizeInstances });
  const copyMesh = (m) => m && { positions: Float32Array.from(m.positions), indices: Uint32Array.from(m.indices) };
  return {
    positions: Float32Array.from(mesh.positions), indices: Uint32Array.from(mesh.indices),
    output: { mesh: copyMesh(output.mesh), instancer: output.instancer && { ...output.instancer, prototypes: output.instancer.prototypes.map(copyMesh) } },
    key: result.key, inputHash: result.inputHash, stats: geometryStats(result.geometry),
  };
}

export function resultTransferables(result) {
  const buffers = [result.positions.buffer, result.indices.buffer], output = result.output;
  if (output?.mesh) buffers.push(output.mesh.positions.buffer, output.mesh.indices.buffer);
  if (output?.instancer) { const i = output.instancer; buffers.push(i.positions.buffer, i.orientations.buffer, i.scales.buffer, i.protoIndices.buffer); for (const p of i.prototypes) buffers.push(p.positions.buffer, p.indices.buffer); }
  return buffers;
}

// Owns a persistent evaluation worker so its node cache survives between
// parameter tweaks. Only the newest preview resolves; superseded previews
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

  // Previews (preview: true) supersede earlier previews only; commit-path
  // evaluations are never cancelled by a later preview.
  async evaluate(graph, source, { preview = false } = {}) {
    if (preview) for (const [id, request] of this.pending) if (request.preview) { request.reject(new LuciaError('LUCIA_CANCELLED', 'Superseded by a newer geometry node evaluation.')); this.pending.delete(id); }
    const worker = this.ensureWorker();
    if (!worker) return evaluateGraphToMesh(graph, source, this.cache);
    const id = this.nextId++, payload = { type: 'evaluate', id, graph, source: source ? { positions: new Float32Array(source.positions), indices: new Uint32Array(source.indices) } : null };
    workerPayloadBytes(payload);
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, preview });
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
