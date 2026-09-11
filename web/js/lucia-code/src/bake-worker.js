import { rasterizeBaseColor, validateBakeResult } from './texture-bake.js';

self.onmessage = ({ data }) => {
  if (data?.type !== 'bake-base-color') return;
  try {
    const result = validateBakeResult(rasterizeBaseColor(data));
    self.postMessage({ type: 'result', ...result }, [result.pixels.buffer]);
  } catch (error) {
    self.postMessage({ type: 'error', message: error?.message || String(error) });
  }
};
