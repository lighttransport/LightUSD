#!/usr/bin/env node

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const repoRoot = path.resolve(__dirname, '..', '..', '..');
const lusdcat = process.env.LUSDCAT_PATH || path.join(repoRoot, 'build', 'lusdcat');
const wasmDir = process.env.LIGHTUSD_WASM_DIR ||
  path.join(repoRoot, 'web', 'js', 'src', 'lightusd');
const wasmEntry = path.join(wasmDir, 'lightusd.js');

const cases = [
  'tests/usda/validation/valid/clean.usda',
  'tests/usda/validation/invalid/multi-violation.usda',
  'tests/usda/validation/invalid/primvar-reader-bad-result.usda',
  'tests/usdc/cube-000.usdc',
  'models/cube.usdz',
  'models/texture-cat-plane.usdz',
];

const optionalCases = [
  ...(process.env.LIGHTUSD_PARITY_EXTRA
    ? process.env.LIGHTUSD_PARITY_EXTRA.split(path.delimiter).filter(Boolean)
    : []),
];

function requireFile(filePath, label) {
  if (!fs.existsSync(filePath)) {
    throw new Error(`${label} not found: ${filePath}`);
  }
}

function skipIfStaleWasm() {
  if (process.env.LIGHTUSD_PARITY_SKIP_STALE_WASM !== '1') {
    return;
  }
  const wasmPath = path.join(wasmDir, 'lightusd.wasm');
  const wasmTime = fs.statSync(wasmPath).mtimeMs;
  const sources = [
    path.join(repoRoot, 'src', 'usd-validation.cc'),
    path.join(repoRoot, 'src', 'usd-validation.hh'),
    path.join(repoRoot, 'web', 'binding.cc'),
  ];
  const newestSourceTime = Math.max(
    ...sources.filter((filePath) => fs.existsSync(filePath))
      .map((filePath) => fs.statSync(filePath).mtimeMs),
  );
  if (wasmTime < newestSourceTime) {
    console.log('SKIP web validation parity: WASM artifacts are older than validation sources');
    process.exit(0);
  }
}

function spawnCommand(file, args) {
  return new Promise((resolve, reject) => {
    const proc = spawn(file, args, {
      cwd: repoRoot,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    proc.stdout.on('data', (chunk) => {
      stdout += String(chunk);
    });
    proc.stderr.on('data', (chunk) => {
      stderr += String(chunk);
    });
    proc.on('error', (error) => {
      reject(error);
    });
    proc.on('close', () => {
      if (stderr) {
        stderr = stderr.trim();
      }
      resolve(stdout);
    });
  });
}

function parseNativeIssues(stdout) {
  const issues = [];
  for (const line of stdout.split(/\r?\n/)) {
    const match = /^(ERROR|WARN ) \[([^\]]+)\] (.*)$/.exec(line);
    if (!match) {
      continue;
    }
    const severity = match[1] === 'ERROR' ? 'error' : 'warning';
    const rule_id = match[2];
    const rest = match[3];
    const sep = rest.indexOf(': ');
    const location = sep >= 0 ? rest.slice(0, sep) : '';
    const message = sep >= 0 ? rest.slice(sep + 2) : rest;
    issues.push({ severity, rule_id, location, message });
  }
  return issues;
}

async function runNative(filePath) {
  const stdout = await spawnCommand(lusdcat, ['--validate-all', filePath]);
  assert.notEqual(stdout, '', `lusdcat produced no validation report for ${filePath}`);
  assert.match(stdout, /^AOUSD validation report /m,
    'validation parity requires the legacy lusdcat --validate-all report; set LUSDCAT_PATH to that executable');
  return parseNativeIssues(stdout);
}

async function runWeb(native, filePath) {
  const data = fs.readFileSync(filePath);
  const raw = native.validateFromBinary(
    new Uint8Array(data),
    path.basename(filePath),
    JSON.stringify({ groups: ['core', 'geom', 'shade', 'lux', 'physics', 'crate'] }),
  );
  const result = JSON.parse(raw);
  assert.equal(result.parse_ok, true, `WASM parse failed for ${filePath}: ${result.error || ''}`);
  return result.issues || [];
}

function compareIssues(filePath, nativeIssues, webIssues) {
  assert.deepEqual(
    webIssues,
    nativeIssues,
    `web validation issues differ from lusdcat --validate-all for ${filePath}`,
  );
}

requireFile(lusdcat, 'lusdcat');
requireFile(wasmEntry, 'WASM JS module');
requireFile(path.join(wasmDir, 'lightusd.wasm'), 'WASM binary');
skipIfStaleWasm();

const createLightUSD = (await import(pathToFileURL(wasmEntry).href)).default;
const module = await createLightUSD({ locateFile: (file) => path.join(wasmDir, file) });
const native = new module.LightUSDLoaderNative();

try {
  const allCases = [
    ...cases.map((rel) => path.join(repoRoot, rel)),
    ...optionalCases.filter((filePath) => fs.existsSync(filePath)),
  ];

  for (const filePath of allCases) {
    const nativeIssues = await runNative(filePath);
    const webIssues = await runWeb(native, filePath);
    compareIssues(filePath, nativeIssues, webIssues);

    if (filePath.endsWith('primvar-reader-bad-result.usda')) {
      assert.equal(
        webIssues.filter((issue) => issue.rule_id === 'shade.primvarReader.result').length,
        1,
        'minimal PrimvarReader fixture should report one outputs:result warning',
      );
    }
    console.log(`OK ${path.relative(repoRoot, filePath) || filePath}`);
  }
} finally {
  native.delete();
}
