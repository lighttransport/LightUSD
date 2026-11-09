import { recomputeVertexTangents } from './tangent-recompute.js';
self.onmessage = ({ data }) => {
  if (data?.type !== 'recompute') return;
  try {
    const tangents = recomputeVertexTangents(data);
    self.postMessage({ type: 'result', tangents }, [tangents.buffer]);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || String(error) });
  }
};
