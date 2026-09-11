import { transferNearestSkinWeights } from './rig-analysis.js';

self.onmessage = ({ data }) => {
  if (data?.type !== 'transfer') return;
  try {
    const result = transferNearestSkinWeights(data);
    self.postMessage({ type: 'result', result }, [result.jointIndices.buffer, result.jointWeights.buffer, result.distances.buffer]);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || 'Skin transfer failed.' });
  }
};
