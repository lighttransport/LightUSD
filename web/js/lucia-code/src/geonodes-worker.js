import { GeoNodesCache } from './geonodes/evaluate.js';
import { evaluateGraphToMesh } from './geonodes/runner.js';

// Persistent worker: the node cache makes re-evaluation after a parameter
// edit cost only the nodes downstream of the change.
const cache = new GeoNodesCache();

self.onmessage = async ({ data }) => {
  if (data?.type !== 'evaluate') return;
  try {
    const result = await evaluateGraphToMesh(data.graph, data.source, cache);
    self.postMessage({ type: 'result', id: data.id, result }, [result.positions.buffer, result.indices.buffer]);
  } catch (error) {
    self.postMessage({ type: 'error', id: data.id, code: error.code, message: error.message, details: error.details });
  }
};
