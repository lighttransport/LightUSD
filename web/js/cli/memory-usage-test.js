// SPDX-License-Identifier: Apache-2.0
// Core WASM asset-store allocation and release diagnostic.
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import {LightUSDLoader} from '../src/lightusd/LightUSDLoader.js';

export function formatBytes(bytes) {
  const n = Number(bytes);
  if (!n) return '0 Bytes';
  const i = Math.min(3, Math.floor(Math.log(n) / Math.log(1024)));
  return `${Number((n / 1024 ** i).toFixed(2))} ${['Bytes', 'KB', 'MB', 'GB'][i]}`;
}

export async function testMemoryUsage() {
  const loader = new LightUSDLoader();
  await loader.init({useMemory64: process.env.LIGHTUSD_WASM64 === '1'});
  const store = new loader.native_.AssetStore();
  try {
    const baseline = store.memoryStats();
    console.log('Core WASM asset-store memory usage');
    for (const size of [100, 10000, 100000, 10000000]) {
      const id = store.registerMemoryAsset(`memory-test-${size}.bin`, new Uint8Array(size));
      try {
        const stats = store.memoryStats();
        assert.equal(Number(stats.assetCount), Number(baseline.assetCount) + 1);
        assert.ok(Number(stats.bytesUsed) >= Number(baseline.bytesUsed) + size);
        assert.equal(store.readMemoryAsset(id).byteLength, size);
        console.log(`${size} input bytes: ${formatBytes(stats.bytesUsed)} tracked`);
      } finally { store.unregisterMemoryAsset(id); }
      const released = store.memoryStats();
      assert.equal(Number(released.assetCount), Number(baseline.assetCount));
      assert.equal(Number(released.bytesUsed), Number(baseline.bytesUsed));
    }
    console.log(`WASM linear memory: ${formatBytes(loader.native_.HEAPU8.byteLength)}`);
    if (process.memoryUsage) console.log(`Node RSS: ${formatBytes(process.memoryUsage().rss)}`);
    console.log('PASS core memory allocation, readback and release');
  } finally { store.delete(); }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  testMemoryUsage().catch(error => { console.error(error); process.exitCode = 1; });
}
