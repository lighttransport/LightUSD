import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const packageRoot = path.resolve(__dirname, '..');
const distDir = path.resolve(packageRoot, 'dist');
const smokeDir = path.join(os.tmpdir(), `lightusd-npm-smoke-${process.pid}`);

const REQUIRED_STAGED_FILES = [
  'index.js',
  'LICENSE',
  'README.md',
  'package.json',
  'lightusd.js',
  'lightusd.wasm',
  'lightusd.wasm.zst',
  'lightusd_64.js',
  'lightusd_64.wasm',
  'lightusd_64.wasm.zst',
  'lightusd_next.js',
  'lightusd_next.wasm',
  'lightusd_next.wasm.zst',
  'lightusd_next_64.js',
  'lightusd_next_64.wasm',
  'lightusd_next_64.wasm.zst',
  'lightusd_geometry.js',
  'lightusd_geometry.wasm',
  'lightusd_geometry.wasm.zst',
  'lightusd_geometry_64.js',
  'lightusd_geometry_64.wasm',
  'lightusd_geometry_64.wasm.zst',
  'lightusd_raytrace.js',
  'lightusd_raytrace.wasm',
  'lightusd_raytrace.wasm.zst',
  'lightusd_raytrace_64.js',
  'lightusd_raytrace_64.wasm',
  'lightusd_raytrace_64.wasm.zst',
  'lightusd_validator.js',
  'lightusd_validator.wasm',
  'lightusd_validator.wasm.zst',
  'lightusd_validator_64.js',
  'lightusd_validator_64.wasm',
  'lightusd_validator_64.wasm.zst',
  'lightusd_exr.js',
  'lightusd_exr.wasm',
  'lightusd_exr.wasm.zst',
  'lightusd_exr_64.js',
  'lightusd_exr_64.wasm',
  'lightusd_exr_64.wasm.zst',
  'LightUSDLoader.js',
  'LightUSDLoaderUtils.js',
  'LightUSDWorkerLoader.js',
  'usdzconvert.js',
  'udim-bake.js',
  'texture-memory-budget.mjs'
];

function readPublishManifest() {
  return JSON.parse(fs.readFileSync(path.join(distDir, 'package.json'), 'utf8'));
}

function patternToRegExp(pattern) {
  const escaped = pattern
    .replace(/[.+^${}()|[\]\\]/g, '\\$&')
    .replace(/\*/g, '[^/]*');
  return new RegExp(`^${escaped}$`);
}

function listPublishableFiles(manifest) {
  const entries = fs.readdirSync(distDir)
    .filter((entry) => fs.statSync(path.join(distDir, entry)).isFile())
    .sort();
  const patterns = (manifest.files || []).map(patternToRegExp);
  const included = new Set(['package.json']);

  for (const entry of entries) {
    if (entry === 'package.json') {
      continue;
    }
    if (patterns.some((regex) => regex.test(entry))) {
      included.add(entry);
    }
  }

  return included;
}

function validateStagedFiles() {
  for (const file of REQUIRED_STAGED_FILES) {
    const filePath = path.join(distDir, file);
    if (!fs.existsSync(filePath)) {
      throw new Error(`Missing staged file: ${filePath}`);
    }
  }
}

function validatePublishableContents(manifest) {
  const publishableFiles = listPublishableFiles(manifest);
  for (const file of REQUIRED_STAGED_FILES) {
    if (!publishableFiles.has(file)) {
      throw new Error(`File would be omitted from published package: ${file}`);
    }
  }
}

function prepareSmokeFixture() {
  fs.rmSync(smokeDir, { recursive: true, force: true });
  fs.mkdirSync(path.join(smokeDir, 'node_modules'), { recursive: true });
  fs.symlinkSync(distDir, path.join(smokeDir, 'node_modules', 'lightusd'), 'dir');

  const script = `
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import * as pkg from 'lightusd';
import { LightUSDLoader as DeepLoader } from 'lightusd/LightUSDLoader.js';
import LightUSDWorkerLoaderDefault, { LightUSDWorkerLoader } from 'lightusd/LightUSDWorkerLoader.js';

const require = createRequire(import.meta.url);

assert.equal(typeof pkg.LightUSDLoader, 'function');
assert.equal(typeof pkg.LightUSDLoaderUtils, 'function');
assert.equal(typeof pkg.TextureLoadingManager, 'function');
assert.equal(typeof pkg.LightUSDComposer, 'function');
assert.equal(typeof pkg.MaterialX, 'object');
assert.equal(typeof pkg.OpenPBRWebGL, 'object');
assert.equal(typeof pkg.OpenPBRTSL, 'object');
assert.equal(pkg.LightUSDLoader, DeepLoader);
assert.equal(LightUSDWorkerLoaderDefault, LightUSDWorkerLoader);
assert.equal(require.resolve('lightusd/LightUSDLoader.js').endsWith('LightUSDLoader.js'), true);
assert.equal(require.resolve('lightusd/lightusd.wasm').endsWith('lightusd.wasm'), true);
assert.equal(require.resolve('lightusd/lightusd.wasm.zst').endsWith('lightusd.wasm.zst'), true);
assert.equal(require.resolve('lightusd/lightusd_64.wasm').endsWith('lightusd_64.wasm'), true);
assert.equal(require.resolve('lightusd/lightusd_64.wasm.zst').endsWith('lightusd_64.wasm.zst'), true);
assert.equal(require.resolve('lightusd/lightusd_next.wasm').endsWith('lightusd_next.wasm'), true);
assert.equal(require.resolve('lightusd/lightusd_next.wasm.zst').endsWith('lightusd_next.wasm.zst'), true);
assert.equal(require.resolve('lightusd/lightusd_next_64.wasm').endsWith('lightusd_next_64.wasm'), true);
assert.equal(require.resolve('lightusd/lightusd_next_64.wasm.zst').endsWith('lightusd_next_64.wasm.zst'), true);

// Instantiate the lean next-only modules (wasm32 + wasm64) and check the
// C API surface and independently linked features used by the next-first loader.
for (const nextModule of ['lightusd/lightusd_next.js', 'lightusd/lightusd_next_64.js']) {
  const factory = (await import(nextModule)).default;
  assert.equal(typeof factory, 'function', nextModule + ' default export is a module factory');
  const instance = await factory();
  for (const name of ['geometry', 'raytrace', 'validator', 'exr']) {
    const feature = await instance.loadFeature(name);
    assert.notEqual(feature.HEAPU8.buffer, instance.HEAPU8.buffer);
    assert.deepEqual(feature.featureInfo, instance.featureInfo);
  }
  for (const api of ['NextUSDZConverterNative', 'NextFlattenSession', 'RenderStream']) {
    assert.equal(typeof instance[api], 'function', nextModule + ' exposes ' + api);
  }
}

// The default lightusd product is the explicit legacy-compatible module.
for (const legacyModule of ['lightusd/lightusd.js', 'lightusd/lightusd_64.js']) {
  const factory = (await import(legacyModule)).default;
  assert.equal(typeof factory, 'function', legacyModule + ' default export is a module factory');
  const instance = await factory();
  assert.equal(typeof instance.LightUSDLoaderNative, 'function',
    legacyModule + ' exposes the legacy loader');
  assert.equal(typeof instance.NextUSDZConverterNative, 'undefined',
    legacyModule + ' must not silently include the next-only converter');
}
`;

  fs.writeFileSync(path.join(smokeDir, 'smoke.mjs'), script.trimStart(), 'utf8');
}

async function runSmokeTest() {
  prepareSmokeFixture();
  await import(pathToFileURL(path.join(smokeDir, 'smoke.mjs')).href);
}

async function main() {
  const packOnly = process.argv.includes('--pack-only');

  if (!fs.existsSync(distDir)) {
    throw new Error(`Package dist directory not found: ${distDir}. Run npm run build first.`);
  }

  validateStagedFiles();

  const manifest = readPublishManifest();
  validatePublishableContents(manifest);

  if (!packOnly) {
    await runSmokeTest();
  }

  console.log(`Validated npm package staging in ${distDir}`);
}

try {
  await main();
} catch (error) {
  console.error(`[validate-package] ${error.message}`);
  process.exit(1);
}
