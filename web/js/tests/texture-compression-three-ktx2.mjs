import assert from 'node:assert/strict';
import { createReadStream } from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import process from 'node:process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

import createLightUSD from '../src/lightusd/lightusd_legacy.js';

const TEST_DIR = path.dirname(fileURLToPath(import.meta.url));
const JS_ROOT = path.resolve(TEST_DIR, '..');
const NODE_MODULES = path.join(JS_ROOT, 'node_modules');
const require = createRequire(path.join(JS_ROOT, 'package.json'));
function isEphemeralListenError(error) {
  return /listen (?:EPERM|EACCES)|Permission denied/i.test(String(error?.message || error || '')) ||
    error?.code === 'EPERM' || error?.code === 'EACCES';
}
const KTX2_ID = [0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
  0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a];

function makeAstcKTX2(blocks, width, height) {
  const dfdOffset = 104;
  const levelOffset = 160;
  const data = new Uint8Array(levelOffset + blocks.byteLength);
  const view = new DataView(data.buffer);
  data.set(KTX2_ID);
  view.setUint32(12, 158, true); // VK_FORMAT_ASTC_4x4_SRGB_BLOCK
  view.setUint32(16, 1, true);
  view.setUint32(20, width, true);
  view.setUint32(24, height, true);
  view.setUint32(36, 1, true);
  view.setUint32(40, 1, true);
  view.setUint32(48, dfdOffset, true);
  view.setUint32(52, 44, true);
  view.setBigUint64(80, BigInt(levelOffset), true);
  view.setBigUint64(88, BigInt(blocks.byteLength), true);
  view.setBigUint64(96, BigInt(blocks.byteLength), true);
  view.setUint32(dfdOffset, 44, true);
  view.setUint32(dfdOffset + 8, 2 | (40 << 16), true);
  data[dfdOffset + 12] = 162; // KHR_DF_MODEL_ASTC
  data[dfdOffset + 13] = 1;   // BT709
  data[dfdOffset + 14] = 2;   // sRGB
  data[dfdOffset + 16] = 3;
  data[dfdOffset + 17] = 3;
  data[dfdOffset + 20] = 16;
  data[dfdOffset + 30] = 127;
  view.setUint32(dfdOffset + 40, 0xffffffff, true);
  data.set(blocks, levelOffset);
  return data;
}

const lightusd = await createLightUSD();
const width = 8;
const height = 8;
const rgba = new Uint8Array(width * height * 4);
for (let y = 0; y < height; ++y) {
  for (let x = 0; x < width; ++x) {
    const offset = (y * width + x) * 4;
    rgba[offset] = x * 255 / (width - 1);
    rgba[offset + 1] = y * 255 / (height - 1);
    rgba[offset + 2] = 64;
    rgba[offset + 3] = 255;
  }
}
const uni = lightusd.compressTextureToUni(rgba, width, height, false);
assert.equal(uni.success, true, uni.error);
const astcBlocks = new Uint8Array(uni.data);
const ktx2 = makeAstcKTX2(astcBlocks, width, height);
const astcLiteral = JSON.stringify(Array.from(astcBlocks));
const rgbaLiteral = JSON.stringify(Array.from(rgba));

const html = `<!doctype html>
<meta charset="utf-8">
<canvas id="canvas" width="8" height="8"></canvas>
<script type="importmap">
{"imports":{"three":"/node_modules/three/build/three.module.js",
"three/":"/node_modules/three/",
"three/addons/":"/node_modules/three/examples/jsm/"}}
</script>
<script type="module">
import * as THREE from 'three';
import { KTX2Loader } from 'three/addons/loaders/KTX2Loader.js';
import { LightUSDLoaderUtils } from '/src/lightusd/LightUSDLoaderUtils.js';
try {
  const renderer = new THREE.WebGLRenderer({canvas: document.querySelector('#canvas')});
  const loader = new KTX2Loader()
    .setTranscoderPath('/node_modules/three/examples/jsm/libs/basis/')
    .detectSupport(renderer);
  const texture = await loader.loadAsync('/fixture.ktx2');
  const scene = new THREE.Scene();
  const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2),
    new THREE.MeshBasicMaterial({map: texture}));
  scene.add(quad);
  const target = new THREE.WebGLRenderTarget(8, 8);
  const pixels = new Uint8Array(8 * 8 * 4);
  const camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
  const ranges = (map) => {
    quad.material.map = map;
    quad.material.needsUpdate = true;
    renderer.setRenderTarget(target);
    renderer.render(scene, camera);
    renderer.readRenderTargetPixels(target, 0, 0, 8, 8, pixels);
    let r0 = 255, r1 = 0, g0 = 255, g1 = 0;
    for (let i = 0; i < pixels.length; i += 4) {
      r0 = Math.min(r0, pixels[i]); r1 = Math.max(r1, pixels[i]);
      g0 = Math.min(g0, pixels[i + 1]); g1 = Math.max(g1, pixels[i + 1]);
    }
    return {redRange: r1 - r0, greenRange: g1 - g0};
  };
  const ktxRanges = ranges(texture);

  // Exercise the real scene-texture constructor, not only KTX2Loader. Three
  // requires RGBA_ASTC_4x4_Format here; SRGBColorSpace makes WebGLUtils select
  // COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR at upload.
  LightUSDLoaderUtils.setLightUSD({
    compressTextureToUni() {
      return {success: true, data: new Uint8Array(16), byteLength: 16};
    },
    transcodeTextureUni() {
      const data = new Uint8Array(${astcLiteral});
      return {success: true, data, byteLength: data.byteLength};
    }
  });
  LightUSDLoaderUtils.detectTextureCompressionTarget = () => ({
    target: 'astc4x4',
    linearFormat: THREE.RGBA_ASTC_4x4_Format,
    srgbFormat: 0x93d0,
    name: 'ASTC 4x4'
  });
  const sceneTexture = await LightUSDLoaderUtils.getTextureFromUSD({
    getTexture() {
      return {textureImageId: 0, wrapS: 'repeat', wrapT: 'repeat',
        hasTransform2d: false, isUDIM: false};
    },
    getImageCopy() {
      return {uri: 'decoded-scene.png', bufferId: 0, decoded: true,
        width: 8, height: 8, channels: 4,
        data: new Uint8Array(${rgbaLiteral})};
    }
  }, 0, 'map');
  const sceneRanges = ranges(sceneTexture);
  window.__result = {
    compressed: texture.isCompressedTexture === true,
    width: texture.image.width,
    height: texture.image.height,
    format: texture.format,
    ...ktxRanges,
    sceneCompressed: sceneTexture.isCompressedTexture === true,
    sceneFormat: sceneTexture.format,
    sceneColorSpace: sceneTexture.colorSpace,
    sceneRedRange: sceneRanges.redRange,
    sceneGreenRange: sceneRanges.greenRange
  };
  sceneTexture.dispose();
  target.dispose();
  quad.geometry.dispose();
  quad.material.dispose();
  loader.dispose();
  renderer.dispose();
} catch (error) {
  window.__error = error?.stack || error?.message || String(error);
}
</script>`;

const mime = new Map([
  ['.js', 'text/javascript; charset=utf-8'],
  ['.wasm', 'application/wasm'],
  ['.ktx2', 'image/ktx2']
]);
function send(response, filename) {
  response.writeHead(200, {
    'Content-Type': mime.get(path.extname(filename)) || 'application/octet-stream'
  });
  createReadStream(filename).pipe(response);
}
const server = http.createServer((request, response) => {
  const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
  if (pathname === '/') {
    response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
    response.end(html);
  } else if (pathname === '/fixture.ktx2') {
    response.writeHead(200, {'Content-Type': 'image/ktx2'});
    response.end(ktx2);
  } else if (pathname.startsWith('/node_modules/')) {
    const filename = path.resolve(NODE_MODULES,
      pathname.slice('/node_modules/'.length));
    const root = `${path.resolve(NODE_MODULES)}${path.sep}`;
    if (filename.startsWith(root)) send(response, filename);
    else { response.writeHead(403); response.end(); }
  } else if (pathname.startsWith('/src/')) {
    const filename = path.resolve(JS_ROOT, pathname.slice(1));
    const root = `${path.resolve(JS_ROOT, 'src')}${path.sep}`;
    if (filename.startsWith(root)) send(response, filename);
    else { response.writeHead(403); response.end(); }
  } else {
    response.writeHead(404); response.end();
  }
});

let browser;
try {
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const puppeteer = require('puppeteer');
  const launch = {
    headless: true,
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage',
      '--use-angle=swiftshader', '--enable-unsafe-swiftshader']
  };
  if (process.env.PUPPETEER_EXECUTABLE_PATH) {
    launch.executablePath = process.env.PUPPETEER_EXECUTABLE_PATH;
  }
  browser = await puppeteer.launch(launch);
  const page = await browser.newPage();
  const address = server.address();
  await page.goto(`http://127.0.0.1:${address.port}/`, {waitUntil: 'load'});
  await page.waitForFunction(() => window.__result || window.__error,
    {timeout: 60000});
  const state = await page.evaluate(() => ({
    result: window.__result,
    error: window.__error
  }));
  assert.equal(state.error, undefined, state.error);
  assert.equal(state.result.compressed, true);
  assert.equal(state.result.width, width);
  assert.equal(state.result.height, height);
  assert.ok(state.result.redRange >= 100, JSON.stringify(state.result));
  assert.ok(state.result.greenRange >= 100, JSON.stringify(state.result));
  assert.equal(state.result.sceneCompressed, true);
  assert.equal(state.result.sceneFormat, 0x93b0);
  assert.equal(state.result.sceneColorSpace, 'srgb');
  assert.ok(state.result.sceneRedRange >= 100, JSON.stringify(state.result));
  assert.ok(state.result.sceneGreenRange >= 100, JSON.stringify(state.result));
  console.log(`texture compression Three KTX2: PASS ${JSON.stringify(state.result)}`);
} catch (error) {
  const message = error?.message || String(error);
  if (isEphemeralListenError(error)) {
    console.log(`SKIP texture-compression-three-ktx2: ${error.message || String(error)}`);
    process.exitCode = 0;
  } else {
    throw error;
  }
} finally {
  await browser?.close().catch(() => {});
  if (server.listening) {
    await new Promise((resolve) => server.close(resolve));
  }
}
