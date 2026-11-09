import { analyzeAsset } from './asset-report.js';

self.onmessage = ({ data }) => {
  try { self.postMessage({ report: analyzeAsset({ traverse: visit => data.nodes.forEach(visit) }) }); }
  catch (error) { self.postMessage({ error: error.message || 'Health analysis failed.' }); }
};
