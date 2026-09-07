// SPDX-License-Identifier: Apache-2.0
import { readFile } from 'node:fs/promises';
import { validateUSDGraphSnapshot } from '../src/webgpu-mtlx/usd-graph-validation.js';
const wasmBinary = await readFile(new URL('../src/lightusd/lightusd_combined.wasm', import.meta.url));
console.log(await validateUSDGraphSnapshot({ wasmBinary }));
