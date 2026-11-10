// test-stream-load.js
// Simple examples demonstrating streaming transfer from JS to WASM
//
// Usage (Node.js):
//   node test-stream-load.js <path-to-usd-file>
//
// Usage (Browser):
//   Include this script and call the browser examples with a URL

import { LightUSDLoader } from './src/lightusd/LightUSDLoader.js';
import {parseUSDZEntries} from './src/usdzconvert.js';
import {createCoreRenderInspector} from './src/lightusd/CoreRenderInspector.js';

function writeProgress(message) {
  if(typeof process!=='undefined'&&process.stdout)process.stdout.write(message);
  else console.log(message);
}

// ============================================================
// Memory Usage Reporting (Node.js only)
// ============================================================
function reportMemUsage() {
  if(typeof process==='undefined'||!process.memoryUsage)return;
  const used = process.memoryUsage();
  for (let key in used) {
    console.log(`${key}: ${Math.round(used[key] / 1024 / 1024 * 100) / 100} MB`);
  }
}

// ============================================================
// Example 1: Stream fetch from URL (Browser/Node.js with fetch)
// ============================================================
async function exampleStreamFetch(url) {
  console.log('=== Stream Fetch Example ===');
  console.log(`URL: ${url}`);

  const loader = new LightUSDLoader();
  await loader.init({ useMemory64: true });

  const startTime = performance.now();

  try {
    // Stream fetch directly to WASM with progress reporting
    const result = await loader.streamFetchToWasm(url, 'test-asset.usd', {
      onProgress: (loaded, total) => {
        const percent = total > 0 ? ((loaded / total) * 100).toFixed(1) : '?';
        writeProgress(`\rStreaming: ${(loaded / 1024).toFixed(1)} KB / ${(total / 1024).toFixed(1)} KB (${percent}%)`);
      }
    });

    console.log('\n');
    console.log('Stream result:', { success: result.success, bytesTransferred: result.bytesTransferred, assetPath: result.assetPath });

    const transferTime = performance.now() - startTime;
    console.log(`Transfer time: ${transferTime.toFixed(2)} ms`);

    // Load from cached asset using the same instance (cache is per-instance)
    const usd = result.usdInstance;
    const loadOk = usd.beginStreamedAsset('test-asset.usd').success;

    if (loadOk) {
      console.log('USD loaded successfully from cached asset!');
      console.log(`Total time: ${(performance.now() - startTime).toFixed(2)} ms`);
      reportMemUsage();
    } else {
      const error=usd.error();usd.delete();throw Error('Failed to load USD: '+error);
    }

    return result;
  } catch (error) {
    console.error('Stream fetch failed:', error);
    throw error;
  }
}

// ============================================================
// Example 2: Stream file read (Node.js only)
// ============================================================
async function exampleStreamFile(filePath) {
  console.log('=== Stream File Example (Node.js) ===');
  console.log(`File: ${filePath}`);

  const loader = new LightUSDLoader();
  await loader.init({ useMemory64: true });

  const startTime = performance.now();

  try {
    // Stream file directly to WASM with progress reporting
    const result = await loader.streamFileToWasm(filePath, 'test-file.usd', {
      chunkSize: 64 * 1024, // 64KB chunks
      onProgress: (loaded, total) => {
        const percent = ((loaded / total) * 100).toFixed(1);
        writeProgress(`\rStreaming: ${(loaded / 1024).toFixed(1)} KB / ${(total / 1024).toFixed(1)} KB (${percent}%)`);
      }
    });

    console.log('\n');
    console.log('Stream result:', { success: result.success, bytesTransferred: result.bytesTransferred, assetPath: result.assetPath });

    const transferTime = performance.now() - startTime;
    console.log(`Transfer time: ${transferTime.toFixed(2)} ms`);

    // Load from cached asset using the same instance (cache is per-instance)
    const usd = result.usdInstance;
    const loadOk = usd.beginStreamedAsset('test-file.usd').success;

    if (loadOk) {
      console.log('USD loaded successfully from cached asset!');
      console.log(`Total time: ${(performance.now() - startTime).toFixed(2)} ms`);
      reportMemUsage();
    } else {
      const error=usd.error();usd.delete();throw Error('Failed to load USD: '+error);
    }

    return result;
  } catch (error) {
    console.error('Stream file failed:', error);
    throw error;
  }
}

// ============================================================
// Example 3: High-level loadWithStreaming (combines fetch + parse)
// ============================================================
async function exampleLoadWithStreaming(url) {
  console.log('=== Load With Streaming Example ===');
  console.log(`URL: ${url}`);

  const loader = new LightUSDLoader();
  await loader.init({ useMemory64: true });

  const startTime = performance.now();

  try {
    const usd = await loader.loadWithStreaming(url, {
      onProgress: (info) => {
        const bar = '='.repeat(Math.floor(info.progress * 20)).padEnd(20, ' ');
        writeProgress(`\r[${bar}] ${info.percentage.toFixed(0)}% - ${info.message}`);
      }
    });

    console.log('\n');
    console.log(`Total time: ${(performance.now() - startTime).toFixed(2)} ms`);
    console.log('USD object loaded:', usd ? 'success' : 'failed');
    reportMemUsage();

    return usd;
  } catch (error) {
    console.error('\nLoad with streaming failed:', error);
    throw error;
  }
}

// ============================================================
// Example 4: Stream multiple assets in parallel
// ============================================================
async function exampleStreamMultiple(assets) {
  console.log('=== Stream Multiple Assets Example ===');
  console.log(`Assets: ${assets.length}`);

  const loader = new LightUSDLoader();
  await loader.init({ useMemory64: true });

  const startTime = performance.now();

  try {
    const results = await loader.streamFetchMultipleToWasm(assets, {
      concurrency: 4,
      onProgress: (completed, total, currentAsset) => {
        console.log(`Progress: ${completed}/${total} - Completed: ${currentAsset}`);
      },
      onAssetProgress: (assetPath, loaded, total) => {
        // Per-asset progress (optional)
      }
    });

    console.log('\nResults:');
    for (const result of results) {
      if (result.success) {
        console.log(`  OK: ${result.assetPath} (${result.bytesTransferred} bytes)`);
      } else {
        console.log(`  FAIL: ${result.assetPath} - ${result.error}`);
      }
    }

    console.log(`Total time: ${(performance.now() - startTime).toFixed(2)} ms`);
    reportMemUsage();

    return results;
  } catch (error) {
    console.error('Stream multiple failed:', error);
    throw error;
  }
}

// ============================================================
// Example 5: USD Load Only (no scene conversion for Three.js)
// ============================================================
// This mode loads USD as a Layer only, without converting to RenderScene.
// Useful for measuring pure USD parsing memory usage.
function layerBytes(bytes) {
  if(bytes[0]===0x50&&bytes[1]===0x4b) {
    const entries=parseUSDZEntries(bytes);
    if(!entries[0]||!/\.usd[ac]?$/i.test(entries[0].name))throw Error('USDZ first entry is not a USD root layer');
    return entries[0].data;
  }
  return bytes;
}

async function exampleLoadOnly(filePath) {
  const loader=new LightUSDLoader();await loader.init({useMemory64:true});
  const fs=await import('node:fs');
  const bytes=fs.readFileSync(filePath);
  const start=performance.now();
  const document=new loader.native_.LayerDocument();
  try {
    const result=document.load(layerBytes(bytes));
    if(!result.success)throw Error(result.error||document.error());
    const loadTime=performance.now()-start;
    console.log(`USD Layer loaded successfully! Load time: ${loadTime.toFixed(2)} ms`);
    console.log('Meshes: 0 (layer-only parsing)');reportMemUsage();
    return {success:true,loadTime};
  }finally{document.delete();}
}

async function exampleCompareLoadModes(filePath) {
  const layer=await exampleLoadOnly(filePath);
  const loader=new LightUSDLoader();await loader.init({useMemory64:true});
  const fs=await import('node:fs');const bytes=fs.readFileSync(filePath);
  const start=performance.now();
  const stream=await createCoreRenderInspector(loader.native_,bytes,filePath);
  try {
    // Force the same geometry/material work that a full scene load performs.
    const meshes=stream.numMeshes(),materials=stream.numMaterials(),images=stream.numImages();
    for(let i=0;i<meshes;i++)stream.getMeshCopy(i);
    const fullLoadTime=performance.now()-start;
    console.log(`RenderScene Stats: Meshes: ${meshes}, Materials: ${materials}, Images: ${images}`);
    console.log(`Load Only (Layer): ${layer.loadTime.toFixed(2)} ms`);
    console.log(`Full Load (RenderScene): ${fullLoadTime.toFixed(2)} ms`);
    reportMemUsage();return {success:true,layerTime:layer.loadTime,fullLoadTime,meshes};
  }finally{stream.delete();}
}

async function exampleComparePerformance(filePath) {
  const loader=new LightUSDLoader();await loader.init({useMemory64:true});
  const fs=await import('node:fs');
  const start=performance.now();let bytes=fs.readFileSync(filePath);
  const traditional=await createCoreRenderInspector(loader.native_,bytes,filePath);
  const meshes=traditional.numMeshes();
  for(let i=0;i<meshes;i++)traditional.getMeshCopy(i);
  traditional.numMaterials();traditional.numImages();
  const traditionalTime=performance.now()-start;
  traditional.delete();bytes=null;
  if(globalThis.gc)globalThis.gc();
  const streamStart=performance.now();
  const result=await loader.streamFileToWasm(filePath,filePath,{chunkSize:65536});
  const stream=result.usdInstance;
  try {
    const loaded=stream.beginStreamedAsset(filePath);
    if(!loaded.success)throw Error(loaded.error||stream.error());
    if(stream.numMeshes()!==meshes)throw Error('Streaming geometry differs from direct loading');
    for(let i=0;i<meshes;i++)stream.getMeshCopy(i);
    stream.numMaterials();stream.numImages();
    console.log(`Traditional: ${traditionalTime.toFixed(2)} ms`);
    console.log(`Streaming: ${(performance.now()-streamStart).toFixed(2)} ms`);
    reportMemUsage();return {success:true,meshes};
  }finally{stream.delete();}
}

// ============================================================
// CLI Entry Point
// ============================================================
async function main() {
  const args = process.argv.slice(2);

  if (args.length === 0) {
    console.log('Usage: node test-stream-load.js <path-to-usd-file> [options]');
    console.log('');
    console.log('Options:');
    console.log('  --load-only     Load USD only (no scene conversion) - measure pure parsing');
    console.log('  --compare-modes Compare load-only vs full load with scene conversion');
    console.log('  --compare       Compare streaming vs traditional loading');
    console.log('  --url           Treat input as URL (auto-detected for http/https)');
    console.log('');
    console.log('Examples:');
    console.log('  node test-stream-load.js model.usdz');
    console.log('  node test-stream-load.js model.usdz --load-only');
    console.log('  node test-stream-load.js model.usdz --compare-modes');
    console.log('  node test-stream-load.js model.usdz --compare');
    console.log('  node test-stream-load.js https://example.com/model.usdz --url');
    process.exit(1);
  }

  const input = args[0];
  const isUrl = args.includes('--url') || input.startsWith('http://') || input.startsWith('https://');
  const doCompare = args.includes('--compare');
  const doLoadOnly = args.includes('--load-only');
  const doCompareModes = args.includes('--compare-modes');

  try {
    if (isUrl) {
      // URL-based examples
      (await exampleStreamFetch(input)).usdInstance.delete();
      console.log('\n');
      (await exampleLoadWithStreaming(input)).delete();
    } else {
      // File-based examples
      if (doLoadOnly) {
        await exampleLoadOnly(input);
      } else if (doCompareModes) {
        await exampleCompareLoadModes(input);
      } else if (doCompare) {
        await exampleComparePerformance(input);
      } else {
        (await exampleStreamFile(input)).usdInstance.delete();
      }
    }

    console.log('\nAll examples completed successfully!');
  } catch (error) {
    console.error('\nExample failed:', error);
    process.exit(1);
  }
}

// Run if executed directly
const isMainModule = typeof process!=='undefined' && import.meta.url === `file://${process.argv[1]}`;
if (isMainModule) {
  main();
}

// Export for use as module
export {
  exampleStreamFetch,
  exampleStreamFile,
  exampleLoadWithStreaming,
  exampleStreamMultiple,
  exampleLoadOnly,
  exampleCompareLoadModes,
  exampleComparePerformance
};
