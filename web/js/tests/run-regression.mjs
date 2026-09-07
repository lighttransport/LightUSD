#!/usr/bin/env node

// Single entry point for the web/WASM regression gate.  The individual test
// programs remain useful for focused debugging, but this file owns the full
// ordering, output directories, environment, and exit status.

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const WEB_JS_DIR = path.resolve(SCRIPT_DIR, '..');
const REPO_ROOT = path.resolve(WEB_JS_DIR, '../..');
const NODE = process.execPath;

const NODE_TESTS = [
  ['WebGPU MaterialX graph and scene', 'tests/webgpu-mtlx.test.mjs'],
  ['usdzconvert helpers', 'tests/test-usdzconvert.js'],
  ['combined RenderStream regression', 'tests/regression-usdzconvert-material-dedup.mjs'],
  ['next usdzconvert', 'tests/usdzconvert-next.test.mjs'],
  ['next-only usdzconvert', 'tests/usdzconvert-next-only.test.mjs'],
  ['next USDA composition', 'tests/next-usda-composition.test.mjs'],
  ['variant selection overloads', 'tests/apply-variant-selection-overload.test.mjs'],
  ['usdcat CLI helpers', 'tests/usdcat-cli.test.mjs'],
  ['URDF/MJCF CLI', 'tests/urdf-to-usd-cli.test.mjs'],
  ['loader fast materials', 'tests/loader-utils-fast-materials.mjs'],
  ['typed array ownership', 'tests/typed-array-ownership.test.mjs'],
  ['texture compression loader', 'tests/loader-utils-texture-compression.mjs'],
  ['Basis/KTX2 loader', 'tests/loader-utils-basis-ktx2.mjs'],
  ['texture compression WASM ABI', 'tests/texture-compression-wasm.mjs'],
  ['texture memory budget', 'tests/texture-memory-budget.test.mjs'],
  ['MaterialX JSON regression', 'tests/materialx-json-regression.js'],
  ['USD skeleton animation', 'tests/usdskel-animation-test.js'],
];

const OPTIONAL_NODE_TESTS = [
  ['validation parity', 'tests/validation-parity.mjs'],
  ['memory64 capability', 'tests/memory64-test.js'],
];

function usage() {
  console.log(`
LightUSD web regression gate

Usage:
  node tests/run-regression.mjs [options]

Options:
  --profile <full|node|physics|browser>  Gate profile (default: full)
  --menagerie <dir>                     Menagerie checkout
  --out <dir>                           Generated output directory
  --software                            Force software browser rendering
  --hardware                            Request hardware browser rendering
  --keep-output                         Keep output after the run
  --help                               Show this help
`);
}

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    profile: 'full',
    menagerie: process.env.MENAGERIE_DIR || process.env.MUJOCO_MENAGERIE ||
      path.join(WEB_JS_DIR, '.cache', 'mujoco_menagerie'),
    out: process.env.LIGHTUSD_REGRESSION_OUT ||
      path.join(WEB_JS_DIR, '.regression', `run-${process.pid}`),
    browserMode: 'auto',
    keepOutput: false,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') {
      usage();
      process.exit(0);
    } else if (arg === '--profile') {
      opts.profile = requireValue(argv, ++i, arg);
    } else if (arg === '--menagerie') {
      opts.menagerie = path.resolve(requireValue(argv, ++i, arg));
    } else if (arg === '--out') {
      opts.out = path.resolve(requireValue(argv, ++i, arg));
    } else if (arg === '--software') {
      opts.browserMode = 'software';
    } else if (arg === '--hardware') {
      opts.browserMode = 'hardware';
    } else if (arg === '--keep-output') {
      opts.keepOutput = true;
    } else {
      throw new Error(`Unknown option: ${arg}`);
    }
  }

  if (!['full', 'node', 'physics', 'browser'].includes(opts.profile)) {
    throw new Error(`--profile must be full, node, physics, or browser`);
  }
  return opts;
}

function requireValue(argv, index, option) {
  const value = argv[index];
  if (!value || value.startsWith('-')) throw new Error(`${option} requires a value`);
  return value;
}

function exists(relativePath) {
  return fs.existsSync(path.join(WEB_JS_DIR, relativePath));
}

function roundtripSummaryIsComplete(summaryPath) {
  try {
    const summary = JSON.parse(fs.readFileSync(summaryPath, 'utf8'));
    const counts = summary.counts;
    return counts && counts.models > 0 && counts.pass === counts.models &&
      counts.fail === 0 && counts.error === 0;
  } catch {
    return false;
  }
}

function run(label, command, args, env, cwd = WEB_JS_DIR) {
  return new Promise((resolve) => {
    const started = Date.now();
    console.log(`\n>>> ${label}`);
    console.log(`    ${command} ${args.join(' ')}`);
    const child = spawn(command, args, {
      cwd,
      env: { ...process.env, ...env },
      stdio: 'inherit',
    });
    child.once('error', (error) => {
      console.error(`<<< ${label}: ERROR ${error.message}`);
      resolve({ label, ok: false, code: 127, seconds: (Date.now() - started) / 1000 });
    });
    child.once('close', (code, signal) => {
      const ok = code === 0;
      console.log(`<<< ${label}: ${ok ? 'PASS' : `FAIL (${signal || code})`} ` +
        `[${((Date.now() - started) / 1000).toFixed(1)}s]`);
      resolve({ label, ok, code: code ?? 1, signal, seconds: (Date.now() - started) / 1000 });
    });
  });
}

async function runNodeTests(results) {
  for (const [label, relative] of NODE_TESTS) {
    if (!exists(relative)) {
      results.push({ label, ok: false, code: 2, skipped: false, error: `missing ${relative}` });
      continue;
    }
    results.push(await run(label, NODE, [relative], {}));
  }

  for (const [label, relative] of OPTIONAL_NODE_TESTS) {
    if (!exists(relative)) continue;
    if (label === 'validation parity' &&
        !fs.existsSync(process.env.LUSDCAT_PATH || path.join(REPO_ROOT, 'build', 'lusdcat'))) {
      console.log(`\n>>> ${label}: SKIP (native lusdcat is not built)`);
      results.push({ label, ok: true, skipped: true, reason: 'native lusdcat is not built' });
      continue;
    }
    results.push(await run(label, NODE, [relative], {}));
  }
}

function browserArgs(relativeScript, args, opts) {
  const requestedHardware = opts.browserMode !== 'software';
  const scriptArgs = [relativeScript, ...args];
  if (!requestedHardware) return [NODE, scriptArgs];
  // The screenshot scripts detect missing DISPLAY/driver and fall back to
  // SwiftShader.  When Xvfb is available, wrap the process so ANGLE/Vulkan
  // can use a real GPU without exposing a window.
  const xvfb = spawnSync('which', ['xvfb-run'], { stdio: 'ignore' });
  if (xvfb.status !== 0) return [NODE, scriptArgs];
  return ['xvfb-run', ['-a', NODE, ...scriptArgs]];
}

async function runBrowser(label, relativeScript, args, env, opts) {
  const [command, commandArgs] = browserArgs(relativeScript, args, opts);
  return run(label, command, commandArgs, env);
}

async function runPhysics(results, opts) {
  if (!fs.existsSync(opts.menagerie)) {
    results.push({
      label: 'Menagerie setup', ok: false, code: 2,
      error: `missing ${opts.menagerie}; run ./setup-mujoco-menagerie.sh --cache-dir "${opts.menagerie}"`,
    });
    return;
  }

  const physicsEnv = {
    MENAGERIE_DIR: opts.menagerie,
    MUJOCO_MENAGERIE: opts.menagerie,
    LIGHTUSD_REGRESSION_OUT: opts.out,
  };
  results.push(await run(
    'MuJoCo physics-only binding smoke test',
    NODE,
    ['tests/mujoco-physics-bindings.test.mjs'],
    physicsEnv,
  ));
  results.push(await run(
    'USD Physics + MuJoCo simulation',
    NODE,
    ['cli/phys-sim.js', '--json'],
    physicsEnv,
  ));

  const roundtripOut = path.join(opts.out, 'mjcf-roundtrip');
  fs.mkdirSync(roundtripOut, { recursive: true });
  results.push(await run(
    'Menagerie MJCF/USD/MJCF closure',
    'bash',
    ['run-mjcf-roundtrip.sh', '--all', '--closure', '--json',
      path.join(roundtripOut, 'summary.json'), '--menagerie', opts.menagerie,
      '--out', roundtripOut],
    physicsEnv,
  ));
}

async function runBrowserTests(results, opts) {
  if (!fs.existsSync(opts.menagerie)) {
    results.push({
      label: 'Browser dataset setup', ok: false, code: 2,
      error: `missing ${opts.menagerie}; run ./setup-mujoco-menagerie.sh --cache-dir "${opts.menagerie}"`,
    });
    return;
  }

  const roundtripOut = path.join(opts.out, 'mjcf-roundtrip');
  const browserEnv = {
    MENAGERIE_DIR: opts.menagerie,
    MUJOCO_MENAGERIE: opts.menagerie,
    LIGHTUSD_REGRESSION_OUT: opts.out,
  };
  const roundtripSummary = path.join(roundtripOut, 'summary.json');
  if (!roundtripSummaryIsComplete(roundtripSummary)) {
    // Keep the browser profile independently runnable while reusing the same
    // conversion output when the full profile has already produced it.
    results.push(await run(
      'Menagerie browser conversion prerequisite',
      'bash',
      ['run-mjcf-roundtrip.sh', '--all', '--closure', '--json', roundtripSummary,
        '--menagerie', opts.menagerie, '--out', roundtripOut],
      browserEnv,
    ));
    if (!results.at(-1).ok) return;
  }
  const browserModeArg = opts.browserMode === 'software' ? [] : ['--hw'];
  // The full Menagerie breadth is covered by the CLI closure and the direct
  // OffscreenCanvas worker sweep below.  Keep the regular UI conversion gate
  // on its curated representative set: importing every textured MJCF through
  // the main-thread demo is intentionally much slower than the worker path.
  results.push(await runBrowser(
    'Menagerie urdf.html conversion/render sweep',
    'tests/screenshot-urdf-batch.mjs',
    ['--menagerie', opts.menagerie, '--out', path.join(opts.out, 'urdf'),
      ...browserModeArg],
    browserEnv,
    opts,
  ));
  results.push(await runBrowser(
    'Menagerie OffscreenCanvas worker render sweep',
    'tests/screenshot-offscreen-batch.mjs',
    ['--all', '--menagerie', opts.menagerie, '--converted-dir', roundtripOut,
      '--out', path.join(opts.out, 'offscreen'), ...browserModeArg],
    browserEnv,
    opts,
  ));
}

async function main() {
  const opts = parseArgs();
  fs.mkdirSync(opts.out, { recursive: true });
  const results = [];

  if (opts.profile === 'node' || opts.profile === 'full') await runNodeTests(results);
  if (opts.profile === 'physics' || opts.profile === 'full') await runPhysics(results, opts);
  if (opts.profile === 'browser' || opts.profile === 'full') await runBrowserTests(results, opts);

  const failed = results.filter((result) => !result.ok);
  const summary = {
    profile: opts.profile,
    menagerie: opts.menagerie,
    output: opts.out,
    passed: results.length - failed.length,
    failed: failed.length,
    results,
  };
  fs.writeFileSync(path.join(opts.out, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);

  console.log('\n================ web regression summary ================');
  console.log(`profile: ${opts.profile}`);
  console.log(`passed: ${summary.passed}   failed: ${summary.failed}`);
  console.log(`summary: ${path.join(opts.out, 'summary.json')}`);
  if (failed.length) {
    for (const result of failed) console.log(`  FAIL: ${result.label}${result.error ? ` — ${result.error}` : ''}`);
    process.exitCode = 1;
  }

  if (!opts.keepOutput && failed.length === 0) {
    fs.rmSync(opts.out, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(`run-regression: ${error.message}`);
  if (process.env.DEBUG) console.error(error.stack);
  process.exitCode = 2;
});
