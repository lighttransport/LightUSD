// SPDX-License-Identifier: Apache 2.0
// Unit tests for usdzconvert.js pure functions + integration tests.
//
// Run:  vite-node tests/test-usdzconvert.js
//       (from web/js/ directory)

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  isImageName,
  isUsdName,
  imageFormatFromName,
  outputFormatForImage,
  parseByteSize,
  replaceExt,
  rootUsdFromMap,
  loadWasm,
  convertFolderToUSDZ,
  convertSourceToUSDZStreaming,
  unpackUSDZ,
  expandUsdzInputs,
  isAudioName,
  parseUSDZEntries,
  buildUSDZWithNewRoot,
} from '../src/usdzconvert.js';
import { createNodeTextureProcessor } from '../src/texture-processor-node.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
let passed = 0;
let failed = 0;

function firstZipEntryName(bytes) {
  assert.equal(bytes[0], 0x50, 'first byte should be P (ZIP magic)');
  assert.equal(bytes[1], 0x4b, 'second byte should be K (ZIP magic)');
  assert.equal(bytes[2], 0x03, 'third byte should be local file header marker');
  assert.equal(bytes[3], 0x04, 'fourth byte should be local file header marker');
  const nameLen = bytes[26] | (bytes[27] << 8);
  const extraLen = bytes[28] | (bytes[29] << 8);
  assert.ok(nameLen > 0, 'first ZIP entry should have a name');
  assert.ok(extraLen >= 0, 'extra field length should be valid');
  const nameBytes = bytes.slice(30, 30 + nameLen);
  return new TextDecoder().decode(nameBytes);
}

function zipEntries(bytes) {
  const entries = new Map();
  let offset = 0;
  const decoder = new TextDecoder();
  while (offset + 30 <= bytes.length) {
    if (bytes[offset] !== 0x50 || bytes[offset + 1] !== 0x4b ||
        bytes[offset + 2] !== 0x03 || bytes[offset + 3] !== 0x04) {
      break;
    }
    const method = bytes[offset + 8] | (bytes[offset + 9] << 8);
    const compressedSize =
      bytes[offset + 18] | (bytes[offset + 19] << 8) |
      (bytes[offset + 20] << 16) | (bytes[offset + 21] << 24);
    const nameLen = bytes[offset + 26] | (bytes[offset + 27] << 8);
    const extraLen = bytes[offset + 28] | (bytes[offset + 29] << 8);
    const nameStart = offset + 30;
    const dataStart = nameStart + nameLen + extraLen;
    const dataEnd = dataStart + compressedSize;
    const name = decoder.decode(bytes.slice(nameStart, nameStart + nameLen));
    assert.equal(method, 0, `ZIP entry ${name} should be stored`);
    assert.ok(dataEnd <= bytes.length, `ZIP entry ${name} should fit`);
    entries.set(name, bytes.slice(dataStart, dataEnd));
    offset = dataEnd;
  }
  return entries;
}

function makeMemoryZipSink() {
  let buf = new Uint8Array(1024);
  let len = 0;
  const ensure = (need) => {
    if (need <= buf.length) return;
    let next = buf.length;
    while (next < need) next *= 2;
    const grown = new Uint8Array(next);
    grown.set(buf);
    buf = grown;
  };
  return {
    sink: {
      write(chunk) {
        const bytes = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
        ensure(len + bytes.length);
        buf.set(bytes, len);
        len += bytes.length;
      },
      patch(pos, chunk) {
        const bytes = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
        ensure(pos + bytes.length);
        buf.set(bytes, pos);
        len = Math.max(len, pos + bytes.length);
      },
    },
    bytes() {
      return buf.slice(0, len);
    },
  };
}

function makeFixtureSource(rootDir, entries, includeFetchSync = true) {
  const source = {
    keys: Object.keys(entries),
    async fetch(key) {
      if (!Object.prototype.hasOwnProperty.call(entries, key)) return null;
      return new Uint8Array(await fs.promises.readFile(path.join(rootDir, entries[key])));
    },
  };
  if (includeFetchSync) {
    source.fetchSync = (key) => {
      if (!Object.prototype.hasOwnProperty.call(entries, key)) return null;
      return new Uint8Array(fs.readFileSync(path.join(rootDir, entries[key])));
    };
  }
  return source;
}

function test(name, fn) {
  try {
    fn();
    passed++;
    console.log(`  ✓ ${name}`);
  } catch (e) {
    failed++;
    console.error(`  ✗ ${name}`);
    console.error(`    ${e.message}`);
  }
}

async function testAsync(name, fn) {
  try {
    await fn();
    passed++;
    console.log(`  ✓ ${name}`);
  } catch (e) {
    failed++;
    console.error(`  ✗ ${name}`);
    console.error(`    ${e.message}`);
  }
}

// ============================================================
console.log('isImageName');
// ============================================================
test('png is image', () => assert.equal(isImageName('tex.png'), true));
test('jpg is image', () => assert.equal(isImageName('tex.jpg'), true));
test('jpeg is image', () => assert.equal(isImageName('tex.jpeg'), true));
test('exr is image', () => assert.equal(isImageName('tex.exr'), true));
test('avif is image', () => assert.equal(isImageName('tex.avif'), true));
test('usda is not image', () => assert.equal(isImageName('scene.usda'), false));
test('txt is not image', () => assert.equal(isImageName('readme.txt'), false));
test('case insensitive PNG', () => assert.equal(isImageName('TEX.PNG'), true));
test('case insensitive JPG', () => assert.equal(isImageName('photo.JPG'), true));
test('path with dirs', () => assert.equal(isImageName('textures/tex.png'), true));
test('no extension', () => assert.equal(isImageName('noext'), false));
test('empty string', () => assert.equal(isImageName(''), false));

// ============================================================
console.log('isUsdName');
// ============================================================
test('usda is usd', () => assert.equal(isUsdName('scene.usda'), true));
test('usdc is usd', () => assert.equal(isUsdName('scene.usdc'), true));
test('usdz is usd', () => assert.equal(isUsdName('scene.usdz'), true));
test('usd is usd', () => assert.equal(isUsdName('scene.usd'), true));
test('png is not usd', () => assert.equal(isUsdName('tex.png'), false));
test('case insensitive USDA', () => assert.equal(isUsdName('SCENE.USDA'), true));
test('path with dirs', () => assert.equal(isUsdName('models/scene.usda'), true));
test('empty string', () => assert.equal(isUsdName(''), false));

// ============================================================
console.log('isAudioName');
// ============================================================
test('m4a is audio', () => assert.equal(isAudioName('clip.m4a'), true));
test('mp3 is audio', () => assert.equal(isAudioName('clip.mp3'), true));
test('wav is audio', () => assert.equal(isAudioName('clip.wav'), true));
test('aac is audio', () => assert.equal(isAudioName('clip.AAC'), true));
test('png is not audio', () => assert.equal(isAudioName('tex.png'), false));
test('usd is not audio', () => assert.equal(isAudioName('scene.usda'), false));
test('audio path with dirs', () => assert.equal(isAudioName('audio/voice.mp3'), true));

// ============================================================
console.log('imageFormatFromName');
// ============================================================
test('png format', () => assert.equal(imageFormatFromName('tex.png'), 'png'));
test('jpg format', () => assert.equal(imageFormatFromName('tex.jpg'), 'jpeg'));
test('jpeg format', () => assert.equal(imageFormatFromName('tex.jpeg'), 'jpeg'));
test('exr is exr', () => assert.equal(imageFormatFromName('tex.exr'), 'exr'));
test('avif returns null', () => assert.equal(imageFormatFromName('tex.avif'), null));
test('no extension', () => assert.equal(imageFormatFromName('noext'), null));
test('case insensitive', () => assert.equal(imageFormatFromName('TEX.PNG'), 'png'));

// ============================================================
console.log('outputFormatForImage');
// ============================================================
test('keep png', () => assert.deepEqual(outputFormatForImage('tex.png'), { format: 'png', ext: 'png' }));
test('keep jpeg preserves extension', () => assert.deepEqual(outputFormatForImage('tex.jpeg'), { format: 'jpeg', ext: 'jpeg' }));
test('force jpeg', () => assert.deepEqual(outputFormatForImage('tex.png', 'jpeg'), { format: 'jpeg', ext: 'jpg' }));
test('force png', () => assert.deepEqual(outputFormatForImage('tex.jpg', 'png'), { format: 'png', ext: 'png' }));
test('keep exr stays exr', () => assert.deepEqual(outputFormatForImage('tex.exr'), { format: 'exr', ext: 'exr' }));
test('exr to png', () => assert.deepEqual(outputFormatForImage('tex.exr', 'png'), { format: 'png', ext: 'png' }));
test('exr to jpeg', () => assert.deepEqual(outputFormatForImage('tex.exr', 'jpeg'), { format: 'jpeg', ext: 'jpg' }));
test('png to exr', () => assert.deepEqual(outputFormatForImage('tex.png', 'exr'), { format: 'exr', ext: 'exr' }));
test('unsupported avif keep', () => assert.deepEqual(outputFormatForImage('tex.avif'), { format: null, ext: null }));

// ============================================================
console.log('parseByteSize');
// ============================================================
test('100', () => assert.equal(parseByteSize('100'), 100));
test('100b', () => assert.equal(parseByteSize('100b'), 100));
test('1kb', () => assert.equal(parseByteSize('1kb'), 1024));
test('1KB', () => assert.equal(parseByteSize('1KB'), 1024));
test('1mb', () => assert.equal(parseByteSize('1mb'), 1048576));
test('1MB', () => assert.equal(parseByteSize('1MB'), 1048576));
test('1gb', () => assert.equal(parseByteSize('1gb'), 1073741824));
test('1tb', () => assert.equal(parseByteSize('1tb'), 1099511627776));
test('1.5mb', () => assert.equal(parseByteSize('1.5mb'), Math.floor(1.5 * 1048576)));
test('  100MB  ', () => assert.equal(parseByteSize('  100MB  '), 100 * 1048576));
test('number input', () => assert.equal(parseByteSize(2048), 2048));
test('negative number', () => assert.equal(parseByteSize(-1), 0));
test('invalid string', () => assert.equal(parseByteSize('abc'), 0));
test('empty string', () => assert.equal(parseByteSize(''), 0));
test('null', () => assert.equal(parseByteSize(null), 0));
test('undefined', () => assert.equal(parseByteSize(undefined), 0));
test('float mb', () => assert.equal(parseByteSize('2.5mb'), Math.floor(2.5 * 1048576)));
test('1024', () => assert.equal(parseByteSize('1024'), 1024));

// ============================================================
console.log('replaceExt');
// ============================================================
test('png to jpg', () => assert.equal(replaceExt('a/b.png', 'jpg'), 'a/b.jpg'));
test('no dir', () => assert.equal(replaceExt('tex.png', 'jpg'), 'tex.jpg'));
test('no existing ext', () => assert.equal(replaceExt('tex', 'png'), 'tex.png'));
test('deep path', () => assert.equal(replaceExt('a/b/c/d.exr', 'png'), 'a/b/c/d.png'));
test('multiple dots', () => assert.equal(replaceExt('tex.v2.png', 'jpg'), 'tex.v2.jpg'));

// ============================================================
console.log('rootUsdFromMap');
// ============================================================
test('picks non-usdz over usdz', () => {
  const m = new Map([['a.usdz', null], ['b.usda', null]]);
  assert.equal(rootUsdFromMap(m), 'b.usda');
});
test('picks shallow over deep', () => {
  const m = new Map([['a/b/c.usda', null], ['x.usda', null]]);
  assert.equal(rootUsdFromMap(m), 'x.usda');
});
test('preferred path wins', () => {
  const m = new Map([['deep/scene.usda', null], ['other.usda', null]]);
  assert.equal(rootUsdFromMap(m, 'deep/scene.usda'), 'deep/scene.usda');
});
test('preferred missing falls back', () => {
  const m = new Map([['a.usda', null]]);
  assert.equal(rootUsdFromMap(m, 'missing.usda'), 'a.usda');
});
test('no usd files returns null', () => {
  const m = new Map([['tex.png', null]]);
  assert.equal(rootUsdFromMap(m), null);
});
test('empty map returns null', () => {
  assert.equal(rootUsdFromMap(new Map()), null);
});
test('only usdz files', () => {
  const m = new Map([['a.usdz', null], ['b.usdz', null]]);
  assert.equal(rootUsdFromMap(m), 'a.usdz');
});

// ============================================================
console.log('createNodeTextureProcessor (worker pool)');
// ============================================================
await testAsync('node texture processor resizes PNGs across a worker pool and destroys cleanly', async () => {
  const { PNG } = await import('pngjs');
  const W = 32, H = 32;
  const img = new PNG({ width: W, height: H });
  for (let i = 0; i < W * H; i++) {
    const o = i * 4;
    img.data[o] = (i * 7) & 255; img.data[o + 1] = (i * 13) & 255;
    img.data[o + 2] = (i * 29) & 255; img.data[o + 3] = 255;
  }
  const src = new Uint8Array(PNG.sync.write(img));

  const { processor, destroy, concurrency } = createNodeTextureProcessor({ concurrency: 2 });
  try {
    assert.equal(concurrency, 2);
    // More jobs than workers, launched together, exercises acquire/release and
    // the waiter queue across the refactored pool (spawn/fail/release).
    const results = await Promise.all([0, 1, 2, 3].map(() => processor({
      data: src, maxTextureSize: 8, reencode: true, textureFormat: 'png',
      resizeColorspace: 'srgb',
    })));
    for (const r of results) {
      assert.ok(r && r.data && r.resized, 'each job should return a resized result');
      const out = PNG.sync.read(Buffer.from(r.data));
      assert.ok(Math.max(out.width, out.height) <= 8, 'output should be <= 8px');
    }
  } finally {
    destroy();
  }
});

await testAsync('node texture processor caps concurrency to its memory budget', async () => {
  const pool = createNodeTextureProcessor({
    concurrency: 8,
    memoryBudgetBytes: 600 * 1024 * 1024,
  });
  try {
    assert.equal(pool.concurrency, 1);
  } finally {
    pool.destroy();
  }
});

// ============================================================
console.log('Integration: loadWasm + convertFolderToUSDZ');
// ============================================================
// The next-backed conversion paths need the combined product; the legacy
// lightusd.js excludes the next core.
const wasmJs = path.resolve(__dirname, '../src/lightusd/lightusd_combined.js');
const hasWasm = fs.existsSync(wasmJs);

if (hasWasm) {
  const wasmGlue = new URL('../src/lightusd/lightusd_combined.js', import.meta.url).href;

  await testAsync('loadWasm succeeds', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    assert.ok(native, 'native module should be truthy');
    assert.ok(typeof native.LightUSDLoaderNative === 'function', 'LightUSDLoaderNative should exist');
  });

  // Create a minimal USDA file for conversion testing.
  const tmpDir = path.join(__dirname, '_test_usdzconvert_tmp');
  if (!fs.existsSync(tmpDir)) fs.mkdirSync(tmpDir, { recursive: true });

  const usdaContent = `#usda 1.0
(
    defaultPrim = "root"
    upAxis = "Y"
)

def Xform "root"
{
    def Mesh "cube"
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0,1,2,3, 4,5,6,7, 0,1,5,4, 2,3,7,6, 0,3,7,4, 1,2,6,5]
        point3f[] points = [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5)]
    }
}
`;
  const usdaPath = path.join(tmpDir, 'scene.usda');
  fs.writeFileSync(usdaPath, usdaContent);
  const rootWithSublayer = `#usda 1.0
(
    defaultPrim = "root"
    subLayers = [
        @sub.usda@
    ]
)

def Xform "root"
{
}
`;
  const sublayerContent = `#usda 1.0

def Xform "fromSub"
{
}
`;

  await testAsync('convertFolderToUSDZ produces valid USDZ', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('scene.usda', new Uint8Array(fs.readFileSync(usdaPath)));

    const { usdz, stats } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'scene.usda',
    });
    assert.ok(usdz instanceof Uint8Array, 'result should be Uint8Array');
    assert.ok(usdz.length > 0, 'USDZ should not be empty');
    assert.equal(stats.rootPath, 'scene.usda');
    assert.equal(stats.textures, 0, 'no textures in this scene');
    // USDZ magic bytes: "PK" (zip)
    assert.equal(usdz[0], 0x50, 'first byte should be P (ZIP magic)');
    assert.equal(usdz[1], 0x4b, 'second byte should be K (ZIP magic)');
  });

  await testAsync('convertFolderToUSDZ with resize option', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('scene.usda', new Uint8Array(fs.readFileSync(usdaPath)));

    const { usdz } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'scene.usda',
      maxTextureSize: 64,
    });
    assert.ok(usdz.length > 0, 'USDZ with resize should not be empty');
  });

  await testAsync('convertFolderToUSDZ can request USDA root layer', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('scene.usda', new Uint8Array(fs.readFileSync(usdaPath)));

    const { usdz, stats } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'scene.usda',
      rootLayerFormat: 'usda',
    });
    assert.ok(usdz.length > 0, 'USDZ should not be empty');
    assert.equal(stats.rootLayerFormat, 'usda');
    assert.equal(firstZipEntryName(usdz), 'root.usda');
  });

  await testAsync('convertFolderToUSDZ arkitCompatible forces USDC root layer', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('scene.usda', new Uint8Array(fs.readFileSync(usdaPath)));

    const { usdz, stats } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'scene.usda',
      rootLayerFormat: 'usda',
      arkitCompatible: true,
    });
    assert.ok(usdz.length > 0, 'USDZ should not be empty');
    assert.equal(stats.rootLayerFormat, 'usdc');
    assert.equal(firstZipEntryName(usdz), 'root.usdc');
  });

  await testAsync('convertFolderToUSDZ flatten composes local sublayers', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('asset/root.usda', new TextEncoder().encode(rootWithSublayer));
    assetMap.set('asset/sub.usda', new TextEncoder().encode(sublayerContent));

    const { usdz, stats } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'asset/root.usda',
      rootLayerFormat: 'usda',
      flatten: true,
    });
    assert.equal(stats.flatten, true);
    const entries = zipEntries(usdz);
    const root = new TextDecoder().decode(entries.get('root.usda'));
    assert.match(root, /def Xform "fromSub"/);
  });

  await testAsync('convertFolderToUSDZ non-flatten packages local sublayers', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('asset/root.usda', new TextEncoder().encode(rootWithSublayer));
    assetMap.set('asset/sub.usda', new TextEncoder().encode(sublayerContent));

    const { usdz, stats } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'asset/root.usda',
      flatten: false,
    });
    assert.equal(stats.flatten, false);
    assert.equal(stats.rootLayerFormat, 'usda');
    const entries = zipEntries(usdz);
    assert.ok(entries.has('root.usda'));
    assert.ok(entries.has('sub.usda'));
    const root = new TextDecoder().decode(entries.get('root.usda'));
    assert.match(root, /@sub\.usda@/);
  });

  await testAsync('stream-next fetches referenced USDC layers and skips unused layers', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const repoRoot = path.resolve(__dirname, '../../..');
    const source = makeFixtureSource(repoRoot, {
      'root.usdc': 'tests/usdc/composition/references-001.usdc',
      'scene-001.usdc': 'tests/usdc/composition/scene-001.usdc',
      'unused.usdc': 'models/cube.usdc',
    });
    const mem = makeMemoryZipSink();
    const { stats } = await convertSourceToUSDZStreaming(native, source, {
      rootPath: 'root.usdc',
      pipeline: 'next',
      reencode: false,
      textureFormat: 'keep',
      zipSink: mem.sink,
    });
    assert.equal(stats.pipeline, 'next');
    const entries = zipEntries(mem.bytes());
    assert.ok(entries.has('root.usdc'), 'flattened root should be written');
    assert.ok(!entries.has('scene-001.usdc'), 'referenced layer should be flattened into root');
    assert.ok(!entries.has('unused.usdc'), 'unused USD layer should not be packed');
  });

  await testAsync('stream-next async source can provide referenced layers on demand', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const repoRoot = path.resolve(__dirname, '../../..');
    const source = makeFixtureSource(repoRoot, {
      'root.usdc': 'tests/usdc/composition/references-001.usdc',
      'scene-001.usdc': 'tests/usdc/composition/scene-001.usdc',
      'unused.usdc': 'models/cube.usdc',
    }, false);
    const mem = makeMemoryZipSink();
    const { stats } = await convertSourceToUSDZStreaming(native, source, {
      rootPath: 'root.usdc',
      pipeline: 'next',
      reencode: false,
      textureFormat: 'keep',
      zipSink: mem.sink,
    });
    assert.equal(stats.pipeline, 'next');
    const entries = zipEntries(mem.bytes());
    assert.deepEqual([...entries.keys()], ['root.usdc']);
  });

  await testAsync('stream-next packages only referenced textures', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const repoRoot = path.resolve(__dirname, '../../..');
    const source = makeFixtureSource(repoRoot, {
      'root.usdc': 'models/texturedcube.usdc',
      'textures/01.jpg': 'models/textures/01.jpg',
      'textures/unused.jpg': 'models/textures/texture-cat.jpg',
      'unused.usdc': 'models/cube.usdc',
    });
    const mem = makeMemoryZipSink();
    const { stats } = await convertSourceToUSDZStreaming(native, source, {
      rootPath: 'root.usdc',
      pipeline: 'next',
      reencode: false,
      textureFormat: 'keep',
      zipSink: mem.sink,
    });
    assert.equal(stats.pipeline, 'next');
    assert.equal(stats.textures, 1);
    const entries = zipEntries(mem.bytes());
    assert.ok(entries.has('root.usdc'), 'flattened root should be written');
    assert.ok(entries.has('textures/01.jpg'), 'referenced texture should be packed');
    assert.ok(!entries.has('textures/unused.jpg'), 'unused texture should not be packed');
    assert.ok(!entries.has('unused.usdc'), 'unused USD layer should not be packed');
  });

  await testAsync('stream-next can include unused textures on request', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const repoRoot = path.resolve(__dirname, '../../..');
    const source = makeFixtureSource(repoRoot, {
      'root.usdc': 'models/texturedcube.usdc',
      'textures/01.jpg': 'models/textures/01.jpg',
      'textures/unused.jpg': 'models/textures/texture-cat.jpg',
      'unused.usdc': 'models/cube.usdc',
    });
    const mem = makeMemoryZipSink();
    const { stats } = await convertSourceToUSDZStreaming(native, source, {
      rootPath: 'root.usdc',
      pipeline: 'next',
      includeUnusedTextures: true,
      reencode: false,
      textureFormat: 'keep',
      zipSink: mem.sink,
    });
    assert.equal(stats.pipeline, 'next');
    assert.equal(stats.textures, 2);
    assert.equal(stats.unusedTexturesIncluded, 1);
    const entries = zipEntries(mem.bytes());
    assert.ok(entries.has('root.usdc'), 'flattened root should be written');
    assert.ok(entries.has('textures/01.jpg'), 'referenced texture should be packed');
    assert.ok(entries.has('textures/unused.jpg'), 'unused texture should be packed when requested');
    assert.ok(!entries.has('unused.usdc'), 'unused USD layer should still not be packed');
  });

  await testAsync('stream-next remaps texture format changes without falling back', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const repoRoot = path.resolve(__dirname, '../../..');
    const source = makeFixtureSource(repoRoot, {
      'root.usdc': 'models/texturedcube.usdc',
      'textures/01.jpg': 'models/textures/01.jpg',
      'textures/unused.jpg': 'models/textures/texture-cat.jpg',
    });
    const mem = makeMemoryZipSink();
    const { stats } = await convertSourceToUSDZStreaming(native, source, {
      rootPath: 'root.usdc',
      pipeline: 'next',
      reencode: false,
      textureFormat: 'png',
      zipSink: mem.sink,
    });
    assert.equal(stats.pipeline, 'next');
    assert.ok(stats.assetPathsRemapped > 0,
      `expected next flatten to remap texture references, got ${stats.assetPathsRemapped}`);
    assert.equal(stats.textures, 1);
    assert.equal(stats.reencoded, 1);
    const entries = zipEntries(mem.bytes());
    assert.ok(entries.has('root.usdc'), 'flattened root should be written');
    assert.ok(entries.has('textures/01.png'), 'referenced texture should be packed under renamed PNG path');
    assert.ok(!entries.has('textures/01.jpg'), 'old texture path should not be packed after remap');
    assert.ok(!entries.has('textures/unused.png'), 'unused renamed texture should not be packed');
  });

  // Regression: a texture that fails to fetch during the bounded prefetch must
  // surface as a clean rejection, not a process-killing unhandledRejection. The
  // pump launches up to `width` processOne() promises before awaiting them FIFO,
  // so a non-head failure used to reject an un-awaited promise (Node >=15 then
  // terminates the process). Here a.png fetches fine while b.png/c.png fail; all
  // three are launched before a.png is awaited.
  await testAsync('stream-next reports a clean error (no unhandledRejection) when a prefetched texture fetch fails', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const png = new Uint8Array(native.repackChannels({ channels: 3, width: 8, height: 8,
      r: { const: 10 }, g: { const: 20 }, b: { const: 30 } }).data);
    const rootBytes = new TextEncoder().encode(usdaContent);  // texture-less cube
    const failKeys = new Set(['b.png', 'c.png']);
    const get = (key) => {
      if (key === 'scene.usda') return rootBytes;
      if (key === 'a.png') return png;
      if (failKeys.has(key)) throw new Error(`disk read error for ${key}`);
      return null;
    };
    const source = {
      keys: ['scene.usda', 'a.png', 'b.png', 'c.png'],
      fetchSync: get,  // sync; used only for the USD root layer
      async fetch(key) {
        if (key === 'a.png') {
          // Delay the head texture past a macrotask so the non-head rejections
          // sit unhandled across an event-loop turn — the exact pre-fix crash
          // window. Without this the loop may re-attach a handler in the same
          // microtask drain and Node would never flag the rejection.
          await new Promise((resolve) => setTimeout(resolve, 25));
          return png;
        }
        return get(key);  // b.png / c.png throw -> rejected promise
      },
    };

    let unhandled = null;
    const onUnhandled = (err) => { unhandled = err; };
    process.on('unhandledRejection', onUnhandled);
    let threw = null;
    try {
      await convertSourceToUSDZStreaming(native, source, {
        rootPath: 'scene.usda', pipeline: 'next',
        reencode: false, textureFormat: 'keep',
        zipSink: makeMemoryZipSink().sink,
      });
    } catch (e) {
      threw = e;
    }
    // Drain the task queue so any stray rejection would surface before we check.
    await new Promise((resolve) => setTimeout(resolve, 50));
    process.removeListener('unhandledRejection', onUnhandled);

    assert.ok(threw, 'conversion should reject when a referenced texture cannot be fetched');
    assert.match(threw.message, /b\.png/,
      `error should name the failed texture, got: ${threw && threw.message}`);
    assert.equal(unhandled, null,
      `no unhandledRejection should escape (got: ${unhandled && unhandled.message})`);
  });

  await testAsync('convertFolderToUSDZ errors on no USD file', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('readme.txt', new Uint8Array([1, 2, 3]));
    try {
      await convertFolderToUSDZ(native, assetMap);
      assert.fail('should throw');
    } catch (e) {
      assert.ok(e.message.includes('No USD file'), 'error should mention missing USD');
    }
  });

  await testAsync('convertFolderToUSDZ errors on invalid USD bytes', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('bad.usda', new Uint8Array([0, 1, 2, 3, 4, 5]));
    try {
      await convertFolderToUSDZ(native, assetMap, { rootPath: 'bad.usda' });
      assert.fail('should throw');
    } catch (e) {
      assert.ok(e.message.includes('Failed to load USD'), 'error should mention load failure');
    }
  });

  // --- USDZ unpack + repack round-trips ---
  await testAsync('unpackUSDZ round-trips a generated USDZ', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map();
    assetMap.set('scene.usda', new TextEncoder().encode(usdaContent));
    const { usdz } = await convertFolderToUSDZ(native, assetMap, { rootPath: 'scene.usda' });
    const { entries, order } = unpackUSDZ(usdz);
    assert.ok(order.length >= 1, 'archive should have at least one entry');
    assert.ok(order.some(isUsdName), 'archive should contain a USD layer');
    for (const name of order) {
      assert.ok(entries.get(name) instanceof Uint8Array, `${name} should be bytes`);
    }
  });

  await testAsync('unpackUSDZ rejects a non-zip buffer', () => {
    assert.throws(() => unpackUSDZ(new Uint8Array([1, 2, 3, 4, 5])), /USDZ|ZIP/);
  });

  await testAsync('expandUsdzInputs unpacks a .usdz into its contents', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const seed = new Map([['scene.usda', new TextEncoder().encode(usdaContent)]]);
    const { usdz } = await convertFolderToUSDZ(native, seed, { rootPath: 'scene.usda' });
    const { assetMap, innerRoot } = expandUsdzInputs(new Map([['model.usdz', usdz]]));
    assert.ok(innerRoot && isUsdName(innerRoot), 'innerRoot should be a USD layer');
    assert.ok(!assetMap.has('model.usdz'), 'archive itself should be expanded away');
    assert.ok(assetMap.has(innerRoot), 'expanded map should contain the inner root');
  });

  await testAsync('convertFolderToUSDZ flattens a .usdz input with low heap root rewrite', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const seed = new Map([['scene.usda', new TextEncoder().encode(usdaContent)]]);
    const first = await convertFolderToUSDZ(native, seed, { rootPath: 'scene.usda' });
    // Feed the produced USDZ back in as the sole input. With flatten enabled
    // (the default), this should rewrite only the root layer and copy entries in JS.
    const repacked = await convertFolderToUSDZ(
      native,
      new Map([['model.usdz', first.usdz]]),
      { rootPath: 'model.usdz', reencode: false },
    );
    assert.ok(repacked.usdz instanceof Uint8Array, 'repack should produce bytes');
    assert.equal(repacked.usdz[0], 0x50, 'output should be a ZIP (P)');
    assert.equal(firstZipEntryName(repacked.usdz), 'root.usdc',
      'low-heap flattened output should write a USDC root');
    assert.equal(repacked.stats.lowHeapFlatten, true,
      'stats should identify the low-heap flatten path');
    assert.ok(!/\.usdz$/i.test(repacked.stats.rootPath),
      'root should resolve to the inner layer, not the .usdz');
    // The repacked archive must still be loadable.
    const usd = new native.LightUSDLoaderNative();
    try {
      assert.ok(usd.loadFromBinary(repacked.usdz, 'repacked.usdz'),
        'repacked USDZ should load: ' + usd.error());
    } finally {
      usd.delete();
    }
  });

  await testAsync('convertFolderToUSDZ passthrough keeps a .usdz byte-identical when flatten is disabled', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const seed = new Map([['scene.usda', new TextEncoder().encode(usdaContent)]]);
    const first = await convertFolderToUSDZ(native, seed, { rootPath: 'scene.usda' });
    const repacked = await convertFolderToUSDZ(
      native,
      new Map([['model.usdz', first.usdz]]),
      { rootPath: 'model.usdz', reencode: false, flatten: false },
    );
    assert.deepEqual(Array.from(repacked.usdz), Array.from(first.usdz),
      'non-flatten passthrough should be byte-identical');
    assert.equal(repacked.stats.passthrough, true,
      'stats should identify the exact passthrough path');
  });

  // The low-heap flatten path rewrites the root as a top-level `root.usdc` and
  // strips the root's directory prefix. For a USDZ whose root layer lives in a
  // subdirectory that rename would re-anchor relative asset references, so the
  // converter must fall back to the standard repack path instead.
  await testAsync('convertFolderToUSDZ falls back to standard repack for a subdirectory-rooted .usdz', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const seed = new Map([['scene.usda', new TextEncoder().encode(usdaContent)]]);
    const first = await convertFolderToUSDZ(native, seed, { rootPath: 'scene.usda' });
    // Re-pack the produced (self-contained) root layer so it lives in a
    // subdirectory inside the archive.
    const innerRoot = parseUSDZEntries(first.usdz).find((e) => isUsdName(e.name));
    assert.ok(innerRoot, 'seed USDZ should contain a USD root');
    const nested = buildUSDZWithNewRoot('sub/root.usdc', innerRoot.data, []);
    assert.equal(firstZipEntryName(nested), 'sub/root.usdc',
      'fixture should have a subdirectory-rooted layer');

    const repacked = await convertFolderToUSDZ(
      native,
      new Map([['model.usdz', nested]]),
      { rootPath: 'model.usdz', reencode: false },
    );
    assert.ok(repacked.usdz instanceof Uint8Array, 'repack should produce bytes');
    assert.equal(repacked.usdz[0], 0x50, 'output should be a ZIP (P)');
    // The low-heap flatten path must be skipped for a subdirectory-rooted input.
    assert.notEqual(repacked.stats.lowHeapFlatten, true,
      'subdirectory-rooted input must fall back to the standard repack path');
    // The fallback output must still be loadable.
    const usd = new native.LightUSDLoaderNative();
    try {
      assert.ok(usd.loadFromBinary(repacked.usdz, 'repacked.usdz'),
        'repacked nested-root USDZ should load: ' + usd.error());
    } finally {
      usd.delete();
    }
  });

  // Regression: skel:animationSource (SkelBindingAPI relationship) must survive a
  // USDC-root export, otherwise skeletal animation is silently dropped on convert.
  await testAsync('convertFolderToUSDZ preserves skeletal animation (USDC root)', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const skelUsda = `#usda 1.0
(
    defaultPrim = "root"
    upAxis = "Y"
)

def SkelRoot "root"
{
    def Skeleton "skel"
    {
        uniform matrix4d[] bindTransforms = [( (1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1) )]
        uniform token[] joints = ["joint0"]
        uniform matrix4d[] restTransforms = [( (1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1) )]
        rel skel:animationSource = </root/anim>
    }

    def SkelAnimation "anim"
    {
        uniform token[] joints = ["joint0"]
        quatf[] rotations = [(1, 0, 0, 0)]
        half3[] scales = [(1, 1, 1)]
        float3[] translations.timeSamples = {
            0: [(0, 0, 0)],
            10: [(1, 0, 0)],
        }
    }
}
`;
    const assetMap = new Map([['skel.usda', new TextEncoder().encode(skelUsda)]]);
    // Sanity-check the source actually has a skeleton + animation.
    const src = new native.LightUSDLoaderNative();
    assert.ok(src.loadFromBinary(new TextEncoder().encode(skelUsda), 'skel.usda'));
    assert.equal(src.numSkeletons(), 1, 'source should have a skeleton');
    assert.equal(src.numAnimations(), 1, 'source should have a skel animation');
    src.delete();

    const { usdz } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'skel.usda', rootLayerFormat: 'usdc',
    });
    const out = new native.LightUSDLoaderNative();
    try {
      assert.ok(out.loadFromBinary(usdz, 'out.usdz'), 'converted USDZ should load');
      assert.equal(out.numSkeletons(), 1, 'skeleton should survive conversion');
      assert.equal(out.numAnimations(), 1,
        'skeletal animation must survive USDC conversion (skel:animationSource binding)');
    } finally {
      out.delete();
    }
  });

  // Regression: a UsdPrimvarReader's connected `inputs:varname` (and the uniform
  // `info:id`) must survive a USDC write. Dropping the varname connection breaks
  // UsdUVTexture evaluation, making the whole render-scene load fail.
  await testAsync('USDC write preserves shader varname connection + uniform info:id', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const matUsda = `#usda 1.0
def Material "M"
{
    token inputs:stPrimvarName = "st"
    token outputs:surface.connect = </M/S.outputs:surface>
    def Shader "P"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname.connect = </M.inputs:stPrimvarName>
        float2 outputs:result
    }
    def Shader "S"
    {
        uniform token info:id = "UsdPreviewSurface"
        token outputs:surface
    }
}
`;
    const usd = new native.LightUSDLoaderNative();
    assert.ok(usd.loadAsLayerFromBinary(new TextEncoder().encode(matUsda), 'm.usda'));
    const usdc = usd.exportAsUSDC();
    usd.delete();
    const rt = new native.LightUSDLoaderNative();
    try {
      assert.ok(rt.loadAsLayerFromBinary(new Uint8Array(usdc), 'rt.usdc'), 'USDC reload');
      const out = rt.exportAsUSDA();
      assert.match(out, /inputs:varname\.connect\s*=\s*<\/M\.inputs:stPrimvarName>/,
        'varname connection must survive the USDC round-trip');
      assert.match(out, /uniform token info:id\s*=\s*"UsdPrimvarReader_float2"/,
        'info:id must remain uniform after the USDC round-trip');
    } finally {
      rt.delete();
    }
  });

  await testAsync('convertFolderToUSDZ passes audio assets through into the USDZ', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const audioBytes = new Uint8Array([0x66, 0x4c, 0x61, 0x43, 1, 2, 3, 4, 5, 6, 7, 8]); // 'fLaC' + filler
    const assetMap = new Map([
      ['scene.usda', new TextEncoder().encode(usdaContent)],
      ['audio/voice.wav', audioBytes],
    ]);
    const { usdz, stats } = await convertFolderToUSDZ(native, assetMap, { rootPath: 'scene.usda' });
    assert.equal(stats.audio, 1, 'one audio asset should be counted');
    const { entries } = unpackUSDZ(usdz);
    assert.ok(entries.has('audio/voice.wav'), 'audio file should be packed into the USDZ');
    assert.deepEqual(entries.get('audio/voice.wav'), audioBytes, 'audio bytes should pass through unchanged');
  });

  await testAsync('audioProcessor hook can replace audio bytes', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const assetMap = new Map([
      ['scene.usda', new TextEncoder().encode(usdaContent)],
      ['snd.mp3', new Uint8Array([1, 1, 1])],
    ]);
    const replacement = new Uint8Array([9, 9, 9, 9]);
    const { usdz } = await convertFolderToUSDZ(native, assetMap, {
      rootPath: 'scene.usda',
      audioProcessor: () => ({ data: replacement }),
    });
    const { entries } = unpackUSDZ(usdz);
    assert.deepEqual(entries.get('snd.mp3'), replacement, 'audioProcessor output should be packed');
  });

  // EXR texture handling: keep+resize as EXR, and transcode EXR -> PNG/JPEG.
  // (Regression: native EXR decode had an inverted success check, and EXR
  // resize/encode were unimplemented.)
  await testAsync('convertImage keeps + resizes EXR and transcodes to PNG/JPEG', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    // Synthesize an EXR from a solid 16x16 PNG (no external fixture needed).
    const packed = native.repackChannels({ channels: 3, width: 16, height: 16,
      r: { const: 200 }, g: { const: 100 }, b: { const: 50 } });
    assert.ok(packed && packed.success, 'repackChannels should produce a PNG');
    const exrRes = native.convertImage(new Uint8Array(packed.data), { format: 'exr' });
    assert.ok(exrRes && exrRes.success, 'PNG -> EXR: ' + (exrRes && exrRes.error));
    const exr = new Uint8Array(exrRes.data);
    assert.deepEqual([...exr.slice(0, 4)], [0x76, 0x2f, 0x31, 0x01], 'valid EXR magic');

    // Keep EXR + resize.
    const kept = native.convertImage(exr, { format: 'exr', maxSize: 8 });
    assert.ok(kept && kept.success, 'EXR resize: ' + (kept && kept.error));
    assert.equal(kept.width, 8);
    assert.equal(kept.height, 8);
    assert.equal(kept.resized, true);
    assert.deepEqual([...new Uint8Array(kept.data).slice(0, 2)], [0x76, 0x2f], 'still EXR');
    // The resized EXR must re-decode.
    const redec = native.convertImage(new Uint8Array(kept.data), { format: 'png' });
    assert.ok(redec && redec.success, 'resized EXR should re-decode');

    // Transcode EXR -> PNG and EXR -> JPEG.
    const toPng = native.convertImage(exr, { format: 'png' });
    assert.ok(toPng && toPng.success, 'EXR -> PNG: ' + (toPng && toPng.error));
    assert.deepEqual([...new Uint8Array(toPng.data).slice(0, 4)], [0x89, 0x50, 0x4e, 0x47], 'PNG magic');
    const toJpg = native.convertImage(exr, { format: 'jpeg', jpegQuality: 80 });
    assert.ok(toJpg && toJpg.success, 'EXR -> JPEG: ' + (toJpg && toJpg.error));
    assert.deepEqual([...new Uint8Array(toJpg.data).slice(0, 2)], [0xff, 0xd8], 'JPEG magic');
  });

  // EXR -> PNG applies an ACES filmic tonemap (not a plain clamp). A 0.502
  // linear gray maps to ~165 under ACES+sRGB vs ~188 for a plain sRGB clamp.
  await testAsync('EXR -> PNG applies ACES filmic tonemap', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const { PNG } = await import('pngjs');
    const packed = native.repackChannels({ channels: 3, width: 4, height: 4,
      r: { const: 128 }, g: { const: 128 }, b: { const: 128 } });
    const exr = new Uint8Array(native.convertImage(new Uint8Array(packed.data), { format: 'exr' }).data);
    const pngRes = native.convertImage(exr, { format: 'png' });
    assert.ok(pngRes && pngRes.success, 'EXR -> PNG: ' + (pngRes && pngRes.error));
    const png = PNG.sync.read(Buffer.from(new Uint8Array(pngRes.data)));
    const v = png.data[0];
    assert.ok(v >= 158 && v <= 172, `expected ACES-tonemapped gray ~165, got ${v}`);
  });

  // fitTextures: EXR participates in the SIZE strategy (kept as EXR) and is
  // passed through unchanged under the QUALITY strategy (no HDR->JPEG).
  await testAsync('fitTextures keeps EXR under size, passes through under quality', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const packed = native.repackChannels({ channels: 3, width: 64, height: 64,
      r: { const: 200 }, g: { const: 100 }, b: { const: 50 } });
    const exr = new Uint8Array(native.convertImage(new Uint8Array(packed.data), { format: 'exr' }).data);

    const sized = native.fitTextures({ images: [{ data: exr, name: 'e.exr' }],
      targetBytes: 256, strategy: 'size', minTextureSize: 8 });
    assert.ok(sized && sized.success, sized && sized.error);
    assert.equal(sized.results[0].ext, 'exr', 'EXR stays EXR under size strategy');
    assert.deepEqual([...new Uint8Array(sized.results[0].data).slice(0, 2)], [0x76, 0x2f], 'EXR magic');

    const qual = native.fitTextures({ images: [{ data: exr, name: 'e.exr' }],
      targetBytes: 256, strategy: 'quality', minQuality: 30 });
    assert.ok(qual && qual.success, qual && qual.error);
    assert.equal(qual.results[0].ext, 'exr', 'EXR untouched under quality strategy');
    assert.equal(new Uint8Array(qual.results[0].data).length, exr.length, 'EXR passed through verbatim');
  });

  // --- Image decode/write coverage for the imageproc features ---

  await testAsync('repackChannels packs constant channels exactly', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const { PNG } = await import('pngjs');
    const res = native.repackChannels({ channels: 3, width: 8, height: 8,
      r: { const: 200 }, g: { const: 100 }, b: { const: 50 } });
    assert.ok(res && res.success, 'repackChannels: ' + (res && res.error));
    const png = PNG.sync.read(Buffer.from(new Uint8Array(res.data)));
    assert.equal(png.data[0], 200, 'R');
    assert.equal(png.data[1], 100, 'G');
    assert.equal(png.data[2], 50, 'B');
  });

  await testAsync('convertImage colorspace sRGB->linear applies the LUT', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const { PNG } = await import('pngjs');
    const src = native.repackChannels({ channels: 3, width: 8, height: 8,
      r: { const: 128 }, g: { const: 128 }, b: { const: 128 } });
    assert.ok(src && src.success);
    const res = native.convertImage(new Uint8Array(src.data),
      { format: 'png', colorspace: 'srgb-to-linear' });
    assert.ok(res && res.success, 'colorspace convert: ' + (res && res.error));
    const png = PNG.sync.read(Buffer.from(new Uint8Array(res.data)));
    // sRGB 128/255 -> linear ~0.216 -> ~55.
    assert.ok(png.data[0] >= 53 && png.data[0] <= 57,
      `sRGB->linear of 128 expected ~55, got ${png.data[0]}`);
  });

  await testAsync('convertImage resizeColorspace srgb differs from linear', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const { PNG } = await import('pngjs');
    // High-contrast checker so gamma-space vs linear-light downsample differ.
    const W = 16, H = 16;
    const chk = new PNG({ width: W, height: H });
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const o = (y * W + x) * 4, c = ((x ^ y) & 1) ? 255 : 0;
      chk.data[o] = c; chk.data[o + 1] = c; chk.data[o + 2] = c; chk.data[o + 3] = 255;
    }
    const src = PNG.sync.write(chk);
    const lin = native.convertImage(new Uint8Array(src), { format: 'png', maxSize: 4, resizeColorspace: 'linear' });
    const srgb = native.convertImage(new Uint8Array(src), { format: 'png', maxSize: 4, resizeColorspace: 'srgb' });
    assert.ok(lin.success && srgb.success, 'resize: ' + (lin.error || srgb.error));
    const pl = PNG.sync.read(Buffer.from(new Uint8Array(lin.data)));
    const ps = PNG.sync.read(Buffer.from(new Uint8Array(srgb.data)));
    let differ = false;
    for (let i = 0; i < pl.data.length; i++) if (pl.data[i] !== ps.data[i]) { differ = true; break; }
    assert.ok(differ, 'srgb-aware resize should differ from gamma-space on high contrast');
  });

  await testAsync('getTextureColorspaceMap reads UsdUVTexture sourceColorSpace', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    assert.equal(typeof native.getTextureColorspaceMap, 'function',
      'getTextureColorspaceMap should be exported');
    const usda = `#usda 1.0
def "M"
{
    def Shader "a"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @tex/albedo.png@
        token inputs:sourceColorSpace = "sRGB"
    }
    def Shader "n"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @tex/normal.png@
        token inputs:sourceColorSpace = "raw"
    }
}
`;
    const map = native.getTextureColorspaceMap(new TextEncoder().encode(usda));
    assert.equal(map['albedo.png'], 'sRGB');
    assert.equal(map['normal.png'], 'raw');
  });

  await testAsync('convertImage EXR->EXR fp16 round-trips losslessly', async () => {
    const native = await loadWasm(() => import(wasmGlue));
    const { PNG } = await import('pngjs');
    const src = native.repackChannels({ channels: 3, width: 16, height: 16,
      r: { const: 200 }, g: { const: 100 }, b: { const: 50 } });
    const exr0 = native.convertImage(new Uint8Array(src.data), { format: 'exr' });
    assert.ok(exr0.success, 'PNG->EXR: ' + (exr0 && exr0.error));
    const exr1 = native.convertImage(new Uint8Array(exr0.data), { format: 'exr' });  // EXR->EXR fp16
    assert.ok(exr1.success, 'EXR->EXR: ' + (exr1 && exr1.error));
    const dec = (e) => PNG.sync.read(Buffer.from(new Uint8Array(
      native.convertImage(new Uint8Array(e.data), { format: 'png' }).data)));
    const p0 = dec(exr0), p1 = dec(exr1);
    let eq = p0.data.length === p1.data.length;
    for (let i = 0; eq && i < p0.data.length; i++) if (p0.data[i] !== p1.data[i]) eq = false;
    assert.ok(eq, 'EXR->EXR fp16 decode->encode should be lossless');
  });

  // Cleanup temp files.
  try { fs.rmSync(tmpDir, { recursive: true }); } catch {}
} else {
  console.log('  (WASM module not found — skipping integration tests)');
}

// ============================================================
// Summary
// ============================================================
console.log(`\n${passed + failed} tests: ${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
