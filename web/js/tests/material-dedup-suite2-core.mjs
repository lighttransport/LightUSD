#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { LightUSDLoader } from '../src/lightusd/LightUSDLoader.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_CONFIG = path.resolve(HERE, 'fixtures/material-dedup-suite2-cases.json');

function parseArgs(argv) {
  const args = { child: false, config: DEFAULT_CONFIG, file: '', maxMs: 0 };
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] === '--child') args.child = true;
    else if (argv[i] === '--config') args.config = path.resolve(argv[++i]);
    else if (argv[i] === '--file') args.file = path.resolve(argv[++i]);
    else if (argv[i] === '--max-ms') args.maxMs = Number(argv[++i]);
    else throw new Error(`unknown argument: ${argv[i]}`);
  }
  return args;
}

function loadCases(configPath) {
  assert.ok(fs.existsSync(configPath), `Suite2 config is missing: ${configPath}`);
  const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  assert.ok(config && Array.isArray(config.cases), 'Suite2 config must contain a cases array');
  const configDir = path.dirname(configPath);
  return config.cases.map((testCase, index) => {
    assert.equal(typeof testCase.file, 'string', `cases[${index}].file must be a string`);
    assert.ok(testCase.file.length > 0, `cases[${index}].file must not be empty`);
    const maxMs = testCase.maxMs ?? 0;
    assert.ok(Number.isFinite(maxMs) && maxMs >= 0,
      `cases[${index}].maxMs must be a non-negative number`);
    assert.ok(testCase.expected === undefined ||
      (testCase.expected && typeof testCase.expected === 'object' && !Array.isArray(testCase.expected)),
      `cases[${index}].expected must be an object`);
    return {
      file: path.resolve(configDir, testCase.file),
      maxMs,
      expected: testCase.expected,
    };
  });
}

async function parseCore(bytes, filename) {
  const loader = new LightUSDLoader({ suppressNativeInfoLogs: true });
  await loader.init({ useCoreOnlyWasm: true });
  return new Promise((resolve, reject) => {
    loader.parse(bytes, filename, resolve, reject, {
      backend: 'core',
      meshOnly: true,
      materialDedup: true,
      mergeMeshes: true,
      mergeMeshesBakeTransform: true,
      flattenRenderTree: false,
    });
  });
}

async function runChild(file, maxMs, expected) {
  assert.ok(fs.existsSync(file), `Suite2 asset is missing: ${file}`);
  const bytes = new Uint8Array(fs.readFileSync(file));
  const start = performance.now();
  const scene = await parseCore(bytes, path.basename(file));
  try {
    // The demo worker transfers every optimized mesh to the main thread. Copy
    // each mesh here too, so the CLI gate covers the same conversion payload
    // rather than timing only native crate parsing.
    let triangles = 0;
    for (let i = 0; i < scene.numMeshes(); ++i) {
      const mesh = scene.getMeshCopy(i);
      assert.ok(mesh && !mesh.error, `mesh ${i} failed: ${mesh?.error || 'unknown error'}`);
      triangles += Math.floor((mesh.indices?.length || 0) / 3);
    }

    const elapsedMs = performance.now() - start;
    const stats = scene.getStats();
    assert.ok(stats.sourceMeshes > stats.optimizedMeshes && stats.optimizedMeshes > 0,
      `mesh merge did not reduce count: ${stats.sourceMeshes} -> ${stats.optimizedMeshes}`);
    assert.ok(stats.sourceMaterials > stats.optimizedMaterials && stats.optimizedMaterials > 0,
      `material dedup did not reduce count: ${stats.sourceMaterials} -> ${stats.optimizedMaterials}`);
    assert.ok(stats.optimizedTextures > 0, 'optimized scene should retain textures');
    assert.ok(triangles > 0, 'optimized scene should retain triangles');
    if (expected) {
      for (const [key, value] of Object.entries(expected)) {
        assert.equal(stats[key], value, `${key} changed for ${path.basename(file)}`);
      }
    }
    if (maxMs > 0) {
      assert.ok(elapsedMs <= maxMs,
        `conversion exceeded ${maxMs} ms budget: ${elapsedMs.toFixed(0)} ms`);
    }
    console.log(JSON.stringify({
      asset: path.basename(file),
      elapsedMs: Math.round(elapsedMs),
      meshes: `${stats.sourceMeshes}->${stats.optimizedMeshes}`,
      materials: `${stats.sourceMaterials}->${stats.optimizedMaterials}`,
      textures: stats.optimizedTextures,
      triangles,
    }));
  } finally {
    scene.delete();
  }
}

const args = parseArgs(process.argv.slice(2));
const cases = loadCases(args.config);
if (args.child) {
  const testCase = cases.find((entry) => entry.file === args.file);
  assert.ok(testCase, `Suite2 asset is not present in config: ${args.file}`);
  await runChild(args.file, args.maxMs, testCase.expected);
} else {
  for (const testCase of cases) {
    assert.ok(fs.existsSync(testCase.file), `Suite2 asset is missing: ${testCase.file}`);
    const result = spawnSync(process.execPath, [fileURLToPath(import.meta.url),
      '--child', '--config', args.config, '--file', testCase.file,
      '--max-ms', String(testCase.maxMs)], {
      stdio: 'inherit',
      timeout: testCase.maxMs + 120000,
    });
    if (result.error) throw result.error;
    assert.equal(result.status, 0, `${path.basename(testCase.file)} child failed`);
  }
  console.log(`Suite2 core material-dedup CLI tests passed: ${cases.length}/${cases.length}`);
}
