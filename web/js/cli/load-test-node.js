// require nodejs v24.0 or later(to load wasm)
import { LightUSDLoader } from '../src/lightusd/LightUSDLoader.js';

import fs from 'node:fs';

function reportMemUsage() {
 const used = process.memoryUsage()
 const messages = []
 for (let key in used) {
    console.log(`${key}: ${Math.round(used[key] / 1024 / 1024 * 100) / 100} MB`)
  }
}

function loadFile(filename) {
  try {
    const data = fs.readFileSync(filename);
    const mimeType = 'application/octet-stream';
    const blob = new Blob([data], { type: mimeType });

    const f = new File([blob], filename, { type: blob.type });

    return f;
    //const base64 = data.toString('base64');
    //data = null;
    //return `data:${mimeType};base64,${base64}`;
    //console.log(data);
  } catch (err) {
    console.error(err);
  }
  return null;
}

async function streamFileToAsset(filename, loader, assetName) {
  const result=await loader.streamFileToWasm(filename,assetName,{chunkSize:65536});
  return result.usdInstance;
}

console.log(process.versions.node);

function checkMemory64Support() {
    try {
        // Try creating a 64-bit memory
        const memory = new WebAssembly.Memory({
            initial: 1,
            maximum: 65536,
            index: 'i64'  // This specifies 64-bit indexing
        });
        return true;
    } catch (e) {
        return false;
    }
}

const memory64Supported = checkMemory64Support();
console.log("memory64:", memory64Supported);

function printUsage() {
  console.log('Usage: node load-test-node.js [--wasm64|--wasm32] <path-to-usd-file>');
  console.log('');
  console.log('Options:');
  console.log('  --wasm64  Use the 64-bit WASM module');
  console.log('  --wasm32  Use the 32-bit WASM module (default)');
  console.log('');
  console.log('Examples:');
  console.log('  node load-test-node.js model.usdz');
  console.log('  node load-test-node.js --wasm64 model.usdz');
  console.log('  node load-test-node.js ../../models/suzanne-subd-lv4.usdc');
}

// Parse command line arguments
const args = process.argv.slice(2);
if (args.length === 0) {
  printUsage();
  process.exit(1);
}

let useMemory64 = false;
let usd_filename = null;

for (const arg of args) {
  if (arg === '--wasm64') {
    useMemory64 = true;
  } else if (arg === '--wasm32') {
    useMemory64 = false;
  } else if (arg === '--help' || arg === '-h') {
    printUsage();
    process.exit(0);
  } else if (arg.startsWith('--')) {
    console.error(`Error: Unknown option: ${arg}`);
    printUsage();
    process.exit(1);
  } else if (!usd_filename) {
    usd_filename = arg;
  } else {
    console.error(`Error: Unexpected argument: ${arg}`);
    printUsage();
    process.exit(1);
  }
}

if (!usd_filename) {
  console.error('Error: Missing path-to-usd-file');
  printUsage();
  process.exit(1);
}

if (useMemory64 && !memory64Supported) {
  console.error('Error: --wasm64 requested, but this Node.js runtime does not support WebAssembly memory64.');
  process.exit(1);
}

// Check if file exists
if (!fs.existsSync(usd_filename)) {
  console.error(`Error: File not found: ${usd_filename}`);
  process.exit(1);
}

async function initScene() {

  const loader = new LightUSDLoader();
  await loader.init({useMemory64});
  loader.setMaxMemoryLimitMB(200);

  // Option 1: Use traditional file loading (existing method)
  console.log("\n=== Traditional file loading ===");
  console.log(`WASM: ${useMemory64 ? 'wasm64' : 'wasm32'}`);
  console.log(`File: ${usd_filename}`);
  const f = loadFile(usd_filename);
  if (!f) {
    console.error('Failed to load file');
    process.exit(1);
  }
  const url = URL.createObjectURL(f);
  //const usd = await loader.loadTestAsync(url);
  const loadStartMs = performance.now();
  const usd = await loader.loadAsync(url);
  const loadMs = performance.now() - loadStartMs;
  //console.log("Traditional loading completed");
  console.log(`Load+parse time: ${loadMs.toFixed(2)} ms`);
  console.log("Load completed");
  reportMemUsage();

  const directMeshes=usd.numMeshes();
  usd.delete();URL.revokeObjectURL(url);
  console.log("\n=== Streaming file loading ===");
  const streamedUsd=await streamFileToAsset(usd_filename,loader,usd_filename);
  try {
    const loaded=streamedUsd.beginStreamedAsset(usd_filename);
    if(!loaded.success)throw Error(loaded.error||streamedUsd.error());
    if(streamedUsd.numMeshes()!==directMeshes)throw Error('Streaming mesh count differs from direct loading');
    console.log(`Streaming loading completed: ${directMeshes} meshes`);
    reportMemUsage();
  }finally{streamedUsd.delete();}

}

console.log(WebAssembly);
/*
Object [WebAssembly] {
  compile: [Function: compile],
  validate: [Function: validate],
  instantiate: [Function: instantiate]
}
*/


initScene().catch(err => {
  console.error('Error:', err);
  process.exit(1);
});
