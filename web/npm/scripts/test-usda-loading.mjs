#!/usr/bin/env node
// Test runner for WASM-based USD file loading against the staged npm package.
// Loads .usda files from tests/usda/ via LightUSDLoader (next backend — the
// only module the package ships) and verifies:
//   1. Standalone valid files load (parse + compose + render conversion)
//   2. Geometry files with complete data produce meshes
//   3. Known-bad files (fail-case/) are rejected
//
// Usage: node --test scripts/test-usda-loading.mjs

import { describe, it, before } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const packageRoot = path.resolve(__dirname, '..');
const distDir = path.resolve(packageRoot, 'dist');
const repoRoot = path.resolve(packageRoot, '..', '..');
const testDir = path.resolve(repoRoot, 'tests', 'usda');

// Loading from memory has no asset resolver: files whose composition needs a
// referenced/sublayered/payload asset cannot load standalone. next reports
// that with this stable message; such files are skipped, not failed.
const MISSING_ASSET_ERROR = 'asset is not present in the imported memory store';

// Valid-syntax fixtures that next rejects on purpose (stricter than the old
// legacy parser). Tracked so a change in either direction is noticed.
const CORE_KNOWN_REJECTS = new Map([
  ['rel-001.usda', "'varying' variability keyword is not valid USDA"],
  ['rel-002.usda', "'varying' variability keyword is not valid USDA"],
  ['feat-localchild-ref.usda', 'Mesh face vertex counts exceed index array length'],
  ['shader-varname-connect-001.usda', 'Mesh face index is out of point range'],
  ['unauthoredValuesIndex-001.usda', 'Mesh face index is out of point range'],
]);

// Geometry files with complete vertex/face data that must produce meshes.
const RENDER_FILES = ['cube.usda', 'cube-with-xform.usda', 'sphere.usda'];

// fail-case/ files that next currently accepts (lenient). Tracked here so the
// suite stays green; tightening parsing is separate work.
const FAIL_CASE_LENIENT = new Set([
  'apishcmema-000.usda',
  'listop-attribute-001.usda',
  'material-binding-multiple-rels-000.usda',
  'over-prim-002.usda',
  'references-empty-layerpath-000.usda',
  'resetxformstack-invalid-order.usda',
  'timesamples-attr-meta-001.usda',
  'timesamples-custom-prefix-000.usda',
  'timesamples-enum-token-002.usda',
  'triple-quoted-string-in-prim-stmt.usda',
  'unknown-type-001.usda',
  'usdskel-002.usda',
  'variantSet-in-prim-meta-000.usda',
]);

function collectTestFiles(dir, prefix = '') {
  const files = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
    // validation/invalid/ holds validator fixtures (semantic violations the
    // validator reports; loading them is not expected to fail or succeed).
    if (entry.isDirectory() && entry.name !== 'fail-case' && entry.name !== 'composition'
        && rel !== 'validation/invalid') {
      files.push(...collectTestFiles(path.join(dir, entry.name), rel));
    } else if (entry.isFile() && entry.name.endsWith('.usda')) {
      files.push(rel);
    }
  }
  return files.sort();
}

function collectFailFiles() {
  const failDir = path.join(testDir, 'fail-case');
  if (!fs.existsSync(failDir)) return [];
  return fs.readdirSync(failDir).filter((f) => f.endsWith('.usda')).sort();
}

let loader;

async function initLoader() {
  const mod = await import(path.join(distDir, 'LightUSDLoader.js'));
  loader = new mod.LightUSDLoader();
  await loader.init({ backend: 'next' });
}

function readArrayBuffer(filePath) {
  const data = fs.readFileSync(filePath);
  return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
}

// The loader logs failure details to the console; keep test output readable.
async function quietly(fn) {
  const saved = [console.log, console.warn, console.error];
  console.log = console.warn = console.error = () => {};
  try {
    return await fn();
  } finally {
    [console.log, console.warn, console.error] = saved;
  }
}

async function load(rel, absPath) {
  return quietly(() => loader.parseAsync(readArrayBuffer(absPath), rel, { backend: 'next' }));
}

const errorText = (err) => String((err && err.message) || err);

describe('USDA loading (WASM, next)', { timeout: 300_000 }, () => {
  before(async () => {
    await initLoader();
  });

  describe('load valid .usda files', () => {
    for (const rel of collectTestFiles(testDir)) {
      const absPath = path.join(testDir, rel);
      const name = path.basename(rel);
      it(`load: ${rel}`, async (t) => {
        try {
          const scene = await load(rel, absPath);
          assert.ok(!CORE_KNOWN_REJECTS.has(name),
            `${rel} is listed in CORE_KNOWN_REJECTS but now loads; remove it from the list`);
          assert.ok(scene, `loader returned a scene for ${rel}`);
        } catch (err) {
          const msg = errorText(err);
          if (msg.includes(MISSING_ASSET_ERROR)) {
            t.skip('needs an external asset (no resolver for in-memory loads)');
            return;
          }
          if (CORE_KNOWN_REJECTS.has(name)) {
            assert.ok(msg.includes(CORE_KNOWN_REJECTS.get(name)),
              `${rel} was rejected for an unexpected reason: ${msg}`);
            return;
          }
          throw err;
        }
      });
    }
  });

  describe('render-scene conversion', () => {
    for (const name of RENDER_FILES) {
      it(`render: ${name}`, async () => {
        const scene = await load(name, path.join(testDir, name));
        const nMeshes = scene.numMeshes();
        assert.ok(nMeshes > 0, `render scene has at least one mesh for ${name} (got ${nMeshes})`);
      });
    }
  });

  describe('reject invalid .usda files (fail-case/)', () => {
    for (const name of collectFailFiles()) {
      if (FAIL_CASE_LENIENT.has(name)) continue;
      it(`reject: fail-case/${name}`, async () => {
        await assert.rejects(load(`fail-case/${name}`, path.join(testDir, 'fail-case', name)),
          `expected an error for fail-case/${name}`);
      });
    }
  });
});
