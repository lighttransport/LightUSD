// SPDX-License-Identifier: Apache-2.0
import {checkFeatures} from './next-features-contract.mjs';
const isNode = typeof process !== 'undefined' && process.versions?.node;
const {parentPort, workerData} = isNode ? await import('node:worker_threads') : {};
const run = async bits => {
  const suffix = bits === 64 ? '_64' : '';
  const {default: factory} = await import(`../src/lightusd/lightusd_next${suffix}.js`);
  return checkFeatures(factory);
};
if (isNode) {
  try { parentPort.postMessage(await run(workerData)); }
  catch (error) { throw error; }
} else self.onmessage = async ({data}) => {
  try { self.postMessage({result: await run(data)}); }
  catch (error) { self.postMessage({error: error.stack}); }
};
