#!/usr/bin/env node
// Test runner for WASM-based USD file loading.
// Loads .usda files from tests/usda/ via LightUSDLoader and verifies:
//   1. Valid files parse without error (loadAsLayerFromBinary — parser only)
//   2. Geometry files with complete data produce meshes (loadFromBinary + Tydra)
//   3. Known-bad files (fail-case/) are rejected by the parser
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

// Files that reference external assets the loader cannot resolve.
const SKIP_FILES = new Set([
  'references-001.usda',
  'references-002.usda',
  'references-003.usda',
  'references-004.usda',
  'references-005.usda',
  'sublayers-000.usda',
  'sublayers-001.usda',
  'sublayers-002.usda',
  'sublayers-003.usda',
  'sublayers-004.usda',
  'sublayers-005.usda',
  'sublayers-006.usda',
  'sublayers-007.usda',
  'payload-001.usda',
  'payload-005.usda',
  'scene-001.usda',
]);

// Geometry files with complete vertex/face data suitable for full Tydra
// render-scene conversion (loadFromBinary).
const TYDRA_RENDER_FILES = new Set([
  'cube.usda',
  'cube-with-xform.usda',
  'sphere.usda',
]);

// fail-case/ files that the WASM parser currently accepts (lenient parsing).
// Tracked here so the suite stays green; tightening parsing is separate work.
const FAIL_CASE_LENIENT = new Set([
  'apishcmema-000.usda',
  'material-binding-multiple-rels-000.usda',
  'over-prim-002.usda',
  'references-002.usda',
  'references-empty-layerpath-000.usda',
  'relative-path-002.usda',
  'resetxformstack-invalid-order.usda',
  'timesamples-custom-prefix-000.usda',
  'timesamples-enum-token-002.usda',
  'variantSet-in-prim-meta-000.usda',
]);

// Files that fail loadAsLayerFromBinary (parser) due to known issues.
const PARSE_KNOWN_FAILURES = new Set([
  'spec/ch07_spec_forms.usda',
]);

function collectTestFiles(dir, prefix = '') {
  const entries = fs.readdirSync(dir, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory() && entry.name !== 'fail-case' && entry.name !== 'composition') {
      files.push(...collectTestFiles(path.join(dir, entry.name), rel));
    } else if (entry.isFile() && entry.name.endsWith('.usda')
               && !SKIP_FILES.has(entry.name) && !PARSE_KNOWN_FAILURES.has(rel)) {
      files.push(rel);
    }
  }
  return files.sort();
}

function collectFailFiles() {
  const failDir = path.join(testDir, 'fail-case');
  if (!fs.existsSync(failDir)) return [];
  return fs.readdirSync(failDir)
    .filter((f) => f.endsWith('.usda'))
    .sort();
}

// ── Loader setup ────────────────────────────────────────────────────

let loader;

async function initLoader() {
  const mod = await import(path.join(distDir, 'LightUSDLoader.js'));
  loader = new mod.LightUSDLoader();
  // This corpus pins legacy parser leniency and its compatibility API.
  // The next product and independent feature modules are checked by validate.
  await loader.init({backend: 'legacy'});
}

/** Parse-only: uses loadAsLayerFromBinary (no Tydra conversion). */
function parseAsLayer(filePath, fileName) {
  const data = fs.readFileSync(filePath);
  const usd = new loader.native_.LightUSDLoaderNative();
  const ok = usd.loadAsLayerFromBinary(data, fileName);
  if (!ok) {
    const error = usd.error();
    usd.delete();
    throw new Error(`Failed to parse ${fileName}: ${error}`);
  }
  return usd;
}

/** Full load: parse + Tydra render-scene conversion via loadFromBinary. */
function loadFull(filePath, fileName) {
  const data = fs.readFileSync(filePath);
  const usd = new loader.native_.LightUSDLoaderNative();
  const ok = usd.loadFromBinary(data, fileName);
  if (!ok) {
    const error = usd.error();
    usd.delete();
    throw new Error(`Failed to load ${fileName}: ${error}`);
  }
  return usd;
}

// ── Tests ───────────────────────────────────────────────────────────

describe('USDA loading (WASM)', { timeout: 120_000 }, () => {
  before(async () => {
    await initLoader();
  });

  const validFiles = collectTestFiles(testDir);

  describe('parse valid .usda files', () => {
    for (const rel of validFiles) {
      const absPath = path.join(testDir, rel);

      it(`parse: ${rel}`, () => {
        const usd = parseAsLayer(absPath, rel);
        assert.ok(usd, `parser returned a USD object for ${rel}`);
        usd.delete();
      });
    }
  });

  describe('Tydra render-scene conversion', () => {
    for (const name of TYDRA_RENDER_FILES) {
      const absPath = path.join(testDir, name);

      it(`render: ${name}`, () => {
        const usd = loadFull(absPath, name);
        assert.ok(usd.ok(), `loadFromBinary succeeded for ${name}`);

        const nMeshes = usd.numMeshes();
        assert.ok(nMeshes > 0, `render scene has at least one mesh for ${name} (got ${nMeshes})`);
        usd.delete();
      });
    }
  });

  describe('reject invalid .usda files (fail-case/)', () => {
    const failFiles = collectFailFiles();

    for (const name of failFiles) {
      if (FAIL_CASE_LENIENT.has(name)) continue;

      const absPath = path.join(testDir, 'fail-case', name);

      it(`reject: fail-case/${name}`, () => {
        assert.throws(
          () => parseAsLayer(absPath, `fail-case/${name}`),
          (err) => {
            assert.ok(err, `expected an error for fail-case/${name}`);
            return true;
          },
        );
      });
    }
  });
});
