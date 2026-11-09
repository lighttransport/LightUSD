// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {Worker} from 'node:worker_threads';
import {checkFeatures} from './next-features-contract.mjs';
const bits = process.env.LIGHTUSD_WASM64 === '1' ? 64 : 32;
const {default: factory} = await import(`../src/lightusd/lightusd_next${bits === 64 ? '_64' : ''}.js`);
assert.deepEqual(await checkFeatures(factory), {pointerBits: bits, heaps: 5});
const worker = new Worker(new URL('./next-features-worker.mjs', import.meta.url), {workerData: bits});
try {
  const result = await new Promise((resolve,reject) => {
    worker.once('message',resolve); worker.once('error',reject);
    worker.once('exit',code => {if (code) reject(new Error(`Worker exited ${code}`));});
  });
  assert.deepEqual(result, {pointerBits: bits, heaps: 5});
} finally {await worker.terminate();}
console.log(`WASM${bits} feature loading, ABI checks, independent heaps and Worker transfers: PASS`);
