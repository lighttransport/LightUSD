import { recomputeFaceVaryingNormals, recomputeVertexNormals } from './normal-recompute.js';
self.onmessage = ({ data }) => {
  if (data?.type !== 'recompute') return;
  try {
    if (data.interpolation != null && !['vertex', 'faceVarying'].includes(data.interpolation)) throw new Error('Normal interpolation must be vertex or faceVarying.');
    const normals = data.interpolation === 'faceVarying' ? recomputeFaceVaryingNormals(data) : recomputeVertexNormals(data);
    self.postMessage({ type: 'result', normals }, [normals.buffer]);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || String(error) });
  }
};
