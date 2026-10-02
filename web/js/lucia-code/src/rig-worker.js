import { transferNearestSkinWeights, analyzeRigPoseSequence } from './rig-analysis.js';

self.onmessage = ({ data }) => {
  if (!['transfer', 'poses'].includes(data?.type)) return;
  try {
    if (data.type === 'poses') { self.postMessage({ type: 'result', result: analyzeRigPoseSequence(data) }); return; }
    const result = transferNearestSkinWeights(data);
    self.postMessage({ type: 'result', result }, [result.jointIndices.buffer, result.jointWeights.buffer, result.distances.buffer]);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || 'Skin transfer failed.' });
  }
};
