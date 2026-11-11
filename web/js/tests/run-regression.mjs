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
  ['core WebGPU texture uploads', 'tests/core-webgpu-texture.test.mjs'],
  ['core streaming transfer', 'tests/core-stream-transfer.test.mjs'],
  ['core export CLI', 'tests/core-export-cli.test.mjs'],
  ['core usdcat CLI', 'tests/usdcat-cli.test.mjs'],
  ['core memory diagnostic', 'cli/memory-usage-test.js'],
  ['core physics roundtrip CLI', 'cli/usd-urdf-roundtrip.js'],
  ['core image textures', 'tests/core-image-texture.test.mjs'],
  ['core render inspector', 'tests/core-render-inspector.test.mjs'],
  ['core progress CLI', 'cli/test-progress-cli.js'],
  ['core loader progress', 'tests/core-loader-progress.test.mjs'],
  ['core texture bindings', 'tests/core-texture-bindings.test.mjs'],
  ['WebGPU MaterialX graph and scene', 'tests/webgpu-mtlx.test.mjs'],
  ['WebGPU MaterialX USD graph snapshot', 'tests/webgpu-mtlx-usd-graph.mjs'],
  ['core physics CLI', 'tests/core-physics-cli.test.mjs'],
  ['core public composer', 'tests/core-composer.test.mjs'],
  ['core compact physics snapshots', 'tests/core-physics-snapshot.test.mjs'],
  ['core generated colors', 'tests/core-generated-colors.test.mjs'],
  ['core xatlas', 'tests/core-xatlas.test.mjs'],
  ['core image conversion', 'tests/core-image-convert.test.mjs'],
  ['core converter textures', 'tests/core-converter-textures.test.mjs'],
  ['core converter orchestration', 'tests/core-converter-orchestration.test.mjs'],
  ['core converter geometry', 'tests/core-converter-geometry.test.mjs'],
  ['core converter materials', 'tests/core-converter-materials.test.mjs'],
  ['usdzconvert helpers', 'tests/test-usdzconvert.js'],
  ['core RenderStream regression', 'tests/regression-usdzconvert-material-dedup.mjs'],
  ['core usdzconvert', 'tests/usdzconvert-core.test.mjs'],
  ['core-only usdzconvert', 'tests/usdzconvert-core-only.test.mjs'],
  ['core USDA composition', 'tests/core-usda-composition.test.mjs'],
  ['core HTTP asset fallback', 'tests/core-http-asset-fallback.test.mjs'],
  ['core HTTP texture alias dedup', 'tests/core-http-texture-alias.test.mjs'],
  ['core asset alias composition', 'tests/core-asset-alias-compose.test.mjs'],
  ['core independent feature modules', 'tests/core-features.test.mjs'],
  ['core C dispatch', 'tests/core-c-dispatch.test.mjs'],
  ['core mesh budget cache', 'tests/core-mesh-budget-cache.test.mjs'],
  ['core binding copy contracts', 'tests/core-binding-copy-contracts.test.mjs'],
  ['core WASM parity gap matrix', 'tests/core-wasm-parity-matrix.test.mjs'],
  ['core flatten workflow parity map', 'tests/core-flatten-parity.test.mjs'],
  ['core composition workflow parity map', 'tests/core-composition-parity.test.mjs'],
  ['core loading workflow parity map', 'tests/core-loading-parity.test.mjs'],
  ['core lusdchecker', 'tests/core-checker.test.mjs'],
  ['core mesh ownership parity map', 'tests/core-mesh-access-parity.test.mjs'],
  ['core asset-store parity matrix', 'tests/core-asset-store-parity.test.mjs'],
  ['core flatten behavior parity', 'tests/core-flatten-behavior-parity.test.mjs'],
  ['core allocation-failure gate', 'tests/core-allocation-failure.test.mjs'],
  ['core volume query contract', 'tests/core-volume-query-contract.test.mjs'],
  ['core MCP server module', 'tests/core-mcp-server.test.mjs'],
  ['URDF/MJCF CLI', 'tests/urdf-to-usd-cli.test.mjs'],
  ['loader fast materials', 'tests/loader-utils-fast-materials.mjs'],
  ['typed array ownership', 'tests/typed-array-ownership.test.mjs'],
  ['texture compression loader', 'tests/loader-utils-texture-compression.mjs'],
  ['Basis/KTX2 loader', 'tests/loader-utils-basis-ktx2.mjs'],
  ['LightRT BVH occlusion WASM ABI', 'tests/lightrt-wasm.mjs'],
  ['Lucia UDIM workflow', 'lucia-code/tests/udim-workflow.test.mjs'],
  ['Lucia scene activity gate', 'lucia-code/tests/activity-gate.test.mjs'],
  ['Lucia source deltas and asset snapshots', 'lucia-code/tests/source-delta.test.mjs'],
  ['Lucia geometry and dependency fidelity', 'lucia-code/tests/fidelity.test.mjs'],
  ['Lucia persistent project storage', 'lucia-code/tests/project-storage.test.mjs'],
  ['Lucia composition and authoring integration', 'lucia-code/tests/workflow-depth.test.mjs'],
  ['Lucia validation failures', 'lucia-code/tests/validation.test.mjs'],
  ['texture memory budget', 'tests/texture-memory-budget.test.mjs'],
  ['UDIM baking', 'tests/udim-bake.test.mjs'],
  ['USD skeleton animation', 'tests/usdskel-animation-test.js'],
];

// Tests of the legacy WASM module (lightusd_legacy[_64].js, compile-only and
// not published): run with --profile legacy after building
// LIGHTUSD_WASM_PRODUCT=legacy.
const LEGACY_NODE_TESTS = [
  ['core material format parity', 'tests/material-format-parity.test.mjs'],
  ['core layer export workflow parity map', 'tests/core-layer-export-parity.test.mjs'],
  ['core validation parity', 'tests/core-validation-parity.test.mjs'],
  ['core layer-to-render parity', 'tests/core-layer-render-parity.test.mjs'],
  ['core asset cache/streaming behavior parity', 'tests/core-asset-cache-behavior-parity.test.mjs'],
  ['core loader configuration behavior parity', 'tests/core-loader-config-behavior-parity.test.mjs'],
  ['core load diagnostics behavior parity', 'tests/core-load-diagnostics-behavior-parity.test.mjs'],
  ['core loading behavior parity', 'tests/core-loading-behavior-parity.test.mjs'],
  ['core render-scene behavior parity', 'tests/core-render-scene-behavior-parity.test.mjs'],
  ['core composition behavior parity', 'tests/core-composition-behavior-parity.test.mjs'],
  ['core layer export behavior parity', 'tests/core-layer-export-behavior-parity.test.mjs'],
  ['core schema/image utility behavior parity', 'tests/core-schema-utilities-behavior-parity.test.mjs'],

  ['usdcat CLI helpers', 'tests/usdcat-cli.test.mjs'],
  ['texture compression WASM ABI', 'tests/texture-compression-wasm.mjs'],
  ['xatlas WASM mapping', 'tests/xatlas-wasm.mjs'],
  ['Lucia Code core processors', 'lucia-code/tests/core.test.mjs'],
  ['validation parity', 'tests/validation-parity.mjs'],
];

const OPTIONAL_NODE_TESTS = [
  ['memory64 capability', 'tests/memory64-test.js'],
];

function usage() {
  console.log(`
LightUSD web regression gate

Usage:
  node tests/run-regression.mjs [options]

Options:
  --profile <full|node|physics|browser|legacy>  Gate profile (default: full)
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
    out: process.env.LIGHTUSD_REGRESSION_OUT || null,
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
  results.push(await run('MaterialX core inspectors', NODE,
    ['tests/core-materialx-demos.test.mjs'], {}));
  results.push(await run('Core codec and colorspace demos', NODE,
    ['tests/core-codec-colorspace-demos.test.mjs'], {}));
  results.push(await run('Standalone core demos', NODE,
    ['tests/standalone-core-demos.test.mjs'], {}));
  results.push(await run('Core ShaderBall adapter', NODE,
    ['tests/core-shaderball-adapter.test.mjs'], {}));
  results.push(await run('Core OpenPBR WebGPU demo', NODE,
    ['tests/core-openpbr-webgpu-demo.test.mjs'], {}));
  results.push(await run('Core material-dedup demo', NODE,
    ['tests/core-material-dedup-demo.test.mjs'], {}));
  results.push(await run('Core HTTP asset demo', NODE,
    ['tests/core-http-demo.test.mjs'], {}));
  results.push(await run('Core streaming demo', NODE,
    ['tests/core-streaming-demo.test.mjs'], {}));
  results.push(await run('Independent features in browser and Worker', NODE,
    ['tests/core-features-browser.test.mjs'], {}));
  results.push(await run('USD validation demos', NODE,
    ['tests/validation-browser.test.mjs'], {}));
  results.push(await run('UDIM browser and Worker baking', NODE,
    ['tests/udim-bake-browser.test.mjs'], {}));
  if (!results.at(-1).ok) return;
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
  if (!opts.out) {
    const parent = path.join(WEB_JS_DIR, '.regression');
    fs.mkdirSync(parent, { recursive: true });
    // Separate containers can share a PID and the same workspace. A unique
    // directory prevents one successful gate from deleting another's output.
    opts.out = fs.mkdtempSync(path.join(parent, 'run-'));
  }
  fs.mkdirSync(opts.out, { recursive: true });
  const results = [];

  if (opts.profile === 'node' || opts.profile === 'full') await runNodeTests(results);
  if (opts.profile === 'physics' || opts.profile === 'full') await runPhysics(results, opts);
  if (opts.profile === 'browser' || opts.profile === 'full') await runBrowserTests(results, opts);
  if (opts.profile === 'legacy') {
    for (const [label, relative] of LEGACY_NODE_TESTS) {
      if (!exists(relative)) continue;
      results.push(await run(label, NODE, [relative], {}));
    }
  }

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
