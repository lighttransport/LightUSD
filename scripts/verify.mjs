#!/usr/bin/env node

// Hermetic verification entry point. Preparation may use the network; test
// profiles only consume already-prepared, ignored cache directories.

import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { spawnSync } from 'node:child_process';

function value(args, index, option) {
  const result = args[index];
  if (!result || result.startsWith('-')) throw new Error(`${option} requires a value`);
  return result;
}

function parseArgs(argv) {
  const opts = { action: 'doctor', profile: 'native', target: null, offline: false, browser: 'software', root: null };
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--root') opts.root = path.resolve(value(argv, ++i, arg));
    else if (arg === '--profile') opts.profile = value(argv, ++i, arg);
    else if (arg === '--target' || arg === '--component') opts.target = value(argv, ++i, arg);
    else if (arg === '--offline') opts.offline = true;
    else if (arg === '--software') opts.browser = 'software';
    else if (arg === '--hardware') opts.browser = 'hardware';
    else if (arg === '-h' || arg === '--help') opts.help = true;
    else if (arg.startsWith('-')) throw new Error(`unknown option: ${arg}`);
    else positional.push(arg);
  }
  if (positional.length) opts.action = positional[0];
  if (!['prepare', 'test', 'doctor'].includes(opts.action)) {
    throw new Error('action must be prepare, test, or doctor');
  }
  if (!['native', 'next', 'web', 'oracle', 'assets', 'gpu', 'full'].includes(opts.profile)) {
    throw new Error('profile must be native, next, web, oracle, assets, gpu, or full');
  }
  const targets = ['npm', 'menagerie', 'mujoco-wasm', 'wasm', 'web', 'assets', 'openusd',
    'native', 'next', 'web-node', 'web-physics', 'web-browser', 'physics', 'oracle', 'gpu'];
  if (opts.target && !targets.includes(opts.target)) {
    throw new Error(`target must be one of: ${targets.join(', ')}`);
  }
  return opts;
}

function usage() {
  console.log(`Usage: scripts/verify.sh <prepare|test|doctor> [options]

Options:
  --profile <native|next|web|oracle|assets|gpu|full>
  --target <component>  run one focused prepare/test target (or use --component)
  --offline       never fetch; require all inputs in the cache
  --software      use software browser rendering (default for test)
  --hardware      request Xvfb/GPU browser rendering
`);
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function run(root, label, command, args, env = {}, cwd = root) {
  console.log(`\n>>> ${label}`);
  console.log(`    ${command} ${args.join(' ')}`);
  const result = spawnSync(command, args, {
    cwd,
    env: { ...process.env, ...env },
    stdio: 'inherit',
  });
  if (result.error) throw new Error(`${label}: ${result.error.message}`);
  if (result.status !== 0) throw new Error(`${label}: exited ${result.status}`);
}

function loadContext(opts) {
  const root = opts.root || process.cwd();
  const manifestPath = path.join(root, 'tests/verification/manifest.json');
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const cache = path.resolve(root, process.env.LIGHTUSD_VERIFY_CACHE || manifest.cache_dir);
  const reportDir = path.join(cache, 'reports');
  fs.mkdirSync(reportDir, { recursive: true });
  const manifestDigest = sha256(manifestPath);
  return { ...opts, root, manifest, manifestPath, manifestDigest, cache, reportDir };
}

function report(ctx, status, error = null) {
  const output = {
    schema: 1,
    action: ctx.action,
    profile: ctx.profile,
    target: ctx.target,
    status,
    error,
    manifest: ctx.manifestDigest,
    host: { platform: process.platform, arch: process.arch, node: process.version, cpus: os.cpus().length },
    cache: ctx.cache,
    generated_at: new Date().toISOString(),
  };
  const file = path.join(ctx.reportDir, `${ctx.action}-${ctx.profile}.json`);
  fs.writeFileSync(file, `${JSON.stringify(output, null, 2)}\n`);
  console.log(`verification report: ${file}`);
}

function prepareMenagerie(ctx) {
  const env = { LIGHTUSD_VERIFY_MANIFEST: ctx.manifestPath };
  if (ctx.offline) env.LIGHTUSD_VERIFY_OFFLINE = '1';
  run(ctx.root, 'MuJoCo Menagerie', 'bash', ['web/js/setup-mujoco-menagerie.sh', '--cache-dir', path.join(ctx.cache, 'menagerie')], env);
}

function prepareMujoco(ctx) {
  const env = { LIGHTUSD_VERIFY_MANIFEST: ctx.manifestPath, LIGHTUSD_VERIFY_CACHE: ctx.cache };
  if (ctx.offline) env.LIGHTUSD_VERIFY_OFFLINE = '1';
  run(ctx.root, 'MuJoCo physics WASM', 'bash', ['scripts/prepare-mujoco-wasm.sh'], env);
}

function prepareAssets(ctx) {
  const env = { LIGHTUSD_VERIFY_MANIFEST: ctx.manifestPath, LIGHTUSD_VERIFY_CACHE: ctx.cache };
  if (ctx.offline) env.LIGHTUSD_VERIFY_OFFLINE = '1';
  run(ctx.root, 'USD-WG assets', 'bash', ['scripts/prepare-usd-assets.sh'], env);
}

function prepareOpenUsd(ctx) {
  const env = { OPENUSD_SRC_DIR: path.join(ctx.cache, 'openusd'), OPENUSD_BUILD_DIR: path.join(ctx.cache, 'openusd-build'), OPENUSD_INSTALL_DIR: path.join(ctx.cache, 'openusd-install'), OPENUSD_FETCH: ctx.offline ? '0' : '1' };
  run(ctx.root, 'OpenUSD oracle', 'bash', ['scripts/build-openusd-usdcat.sh'], env);
}

function prepareWeb(ctx) {
  prepareNpm(ctx);
  run(ctx.root, 'Pinned Menagerie', 'bash', ['web/js/setup-mujoco-menagerie.sh', '--cache-dir', path.join(ctx.cache, 'menagerie')], ctx.offline ? { LIGHTUSD_VERIFY_OFFLINE: '1' } : {});
  prepareMujoco(ctx);
  prepareWasm(ctx);
}

function prepareNpm(ctx) {
  run(ctx.root, 'Web npm dependencies', 'npm', ctx.offline ? ['ci', '--offline'] : ['ci'], {}, path.join(ctx.root, 'web/js'));
}

function prepareWasm(ctx) {
  run(ctx.root, 'LightUSD WASM modules', 'bash', ['web/demo/scripts/prepare-local-lightusd.sh'], { LIGHTUSD_VERIFY_CACHE: ctx.cache });
}

function testNative(ctx) {
  const build = path.join(ctx.cache, 'native-build');
  run(ctx.root, 'Native configure', 'cmake', ['-S', ctx.root, '-B', build, '-G', 'Ninja', '-DLIGHTUSD_BUILD_TESTS=ON', '-DLIGHTUSD_BUILD_EXAMPLES=ON', `-DLIGHTUSD_TEST_FIXTURE_DIR=${ctx.root}`]);
  run(ctx.root, 'Native build', 'cmake', ['--build', build]);
  run(ctx.root, 'Native CTest', 'ctest', ['--test-dir', build, '--output-on-failure'], {
    USD_WG_ASSETS_DIR: path.join(ctx.cache, 'usd-assets'),
  });
}

function testNext(ctx) {
  const build = path.join(ctx.cache, 'next-build');
  run(ctx.root, 'next configure', 'cmake', ['-S', path.join(ctx.root, 'src'), '-B', build, '-G', 'Ninja', '-DLIGHTUSD_BUILD_TESTS=ON', '-DCMAKE_BUILD_TYPE=Debug']);
  run(ctx.root, 'next build', 'cmake', ['--build', build]);
  run(ctx.root, 'next CTest', 'ctest', ['--test-dir', build, '--output-on-failure']);
}

function testWeb(ctx) {
  const env = {
    MENAGERIE_DIR: path.join(ctx.cache, 'menagerie'),
    MUJOCO_MENAGERIE: path.join(ctx.cache, 'menagerie'),
    MUJOCO_WASM_DIR: path.join(ctx.cache, 'mujoco', 'wasm', 'dist'),
    VITE_MUJOCO_WASM_DIR: path.join(ctx.cache, 'mujoco', 'wasm', 'dist'),
    USD_WG_ASSETS_DIR: path.join(ctx.cache, 'usd-assets'),
    LIGHTUSD_SKIP_WASM_PREPARE: '1',
  };
  const args = ['tests/run-regression.mjs', '--profile', 'full', ctx.browser === 'hardware' ? '--hardware' : '--software'];
  run(ctx.root, 'Web regression', 'node', args, env, path.join(ctx.root, 'web/js'));
}

function testMujocoWasm(ctx) {
  const env = {
    LIGHTUSD_VERIFY_CACHE: ctx.cache,
    MUJOCO_WASM_DIR: path.join(ctx.cache, 'mujoco', 'wasm', 'dist'),
  };
  run(ctx.root, 'MuJoCo WASM binding smoke test', 'node',
    ['tests/mujoco-physics-bindings.test.mjs'], env, path.join(ctx.root, 'web/js'));
  run(ctx.root, 'MuJoCo WASM physics smoke test', 'node', ['cli/phys-sim.js', '--json'], env, path.join(ctx.root, 'web/js'));
}

function testWebProfile(ctx, profile) {
  const env = {
    MENAGERIE_DIR: path.join(ctx.cache, 'menagerie'),
    MUJOCO_MENAGERIE: path.join(ctx.cache, 'menagerie'),
    MUJOCO_WASM_DIR: path.join(ctx.cache, 'mujoco', 'wasm', 'dist'),
    VITE_MUJOCO_WASM_DIR: path.join(ctx.cache, 'mujoco', 'wasm', 'dist'),
    LIGHTUSD_VERIFY_CACHE: ctx.cache,
    LIGHTUSD_SKIP_WASM_PREPARE: '1',
  };
  const browserArg = profile === 'browser' ? (ctx.browser === 'hardware' ? '--hardware' : '--software') : null;
  const args = ['tests/run-regression.mjs', '--profile', profile, ...(browserArg ? [browserArg] : [])];
  run(ctx.root, `Web ${profile} regression`, 'node', args, env, path.join(ctx.root, 'web/js'));
}

function testOracle(ctx) {
  const env = {
    LUSDCAT_PATH: path.join(ctx.cache, 'native-build', 'lusdcat'),
    USDCAT_PATH: path.join(ctx.cache, 'openusd-install', 'bin', 'usdcat'),
  };
  run(ctx.root, 'LightUSD/OpenUSD comparison', 'bash', ['tests/run-usdcat-compare.sh'], env);
}

function testAssets(ctx) {
  const env = {
    USD_WG_ASSETS_DIR: path.join(ctx.cache, 'usd-assets'),
    LUSDCAT_PATH: path.join(ctx.cache, 'native-build', 'lusdcat'),
  };
  run(ctx.root, 'USD asset parser sweep', 'node', ['tests/parse-asset-corpus.mjs', '--assets', env.USD_WG_ASSETS_DIR, '--lusdcat', env.LUSDCAT_PATH, '--max-fail', '0'], env);
}

function testGpu(ctx) {
  const env = { USD_ASSETS_ROOT: path.join(ctx.cache, 'usd-assets') };
  run(ctx.root, 'GPU/lusdview CTest', 'ctest', ['--test-dir', path.join(ctx.cache, 'native-build'), '-R', '^lusdview', '--output-on-failure'], env);
}

function prepare(ctx) {
  if (ctx.target) {
    if (ctx.target === 'npm') return prepareNpm(ctx);
    if (ctx.target === 'menagerie') return prepareMenagerie(ctx);
    if (ctx.target === 'mujoco-wasm') return prepareMujoco(ctx);
    if (ctx.target === 'wasm') return prepareWasm(ctx);
    if (ctx.target === 'web') return prepareWeb(ctx);
    if (ctx.target === 'assets') return prepareAssets(ctx);
    if (ctx.target === 'openusd') return prepareOpenUsd(ctx);
    throw new Error(`prepare does not support target ${ctx.target}`);
  }
  if (ctx.profile === 'web' || ctx.profile === 'full') prepareWeb(ctx);
  if (ctx.profile === 'assets' || ctx.profile === 'full') prepareAssets(ctx);
  if (ctx.profile === 'oracle' || ctx.profile === 'full') prepareOpenUsd(ctx);
  if (ctx.profile === 'gpu') prepareAssets(ctx);
}

function test(ctx) {
  if (ctx.target) {
    if (ctx.target === 'mujoco-wasm') return testMujocoWasm(ctx);
    if (ctx.target === 'web-node' || ctx.target === 'wasm') return testWebProfile(ctx, 'node');
    if (ctx.target === 'web-physics' || ctx.target === 'physics' || ctx.target === 'menagerie') return testWebProfile(ctx, 'physics');
    if (ctx.target === 'web-browser') return testWebProfile(ctx, 'browser');
    if (ctx.target === 'web') return testWeb(ctx);
    if (ctx.target === 'native') return testNative(ctx);
    if (ctx.target === 'next') return testNext(ctx);
    if (ctx.target === 'oracle') return testOracle(ctx);
    if (ctx.target === 'assets') return testAssets(ctx);
    if (ctx.target === 'gpu') return testGpu(ctx);
    throw new Error(`test does not support target ${ctx.target}`);
  }
  if (ctx.profile === 'native' || ctx.profile === 'full') testNative(ctx);
  if (ctx.profile === 'next' || ctx.profile === 'full') testNext(ctx);
  if (ctx.profile === 'web' || ctx.profile === 'full') testWeb(ctx);
  if (ctx.profile === 'oracle' || ctx.profile === 'full') testOracle(ctx);
  if (ctx.profile === 'assets' || ctx.profile === 'full') testAssets(ctx);
  if (ctx.profile === 'gpu') testGpu(ctx);
}

function doctor(ctx) {
  for (const command of ['node', 'cmake', 'ctest', 'bash']) {
    if (spawnSync('which', [command], { stdio: 'ignore' }).status !== 0) {
      throw new Error(`required command not found: ${command}`);
    }
  }
  console.log(`manifest: ${ctx.manifestPath}`);
  console.log(`manifest sha256: ${ctx.manifestDigest}`);
  console.log(`cache: ${ctx.cache}`);
  console.log(`profile: ${ctx.profile}`);
  if (ctx.target) console.log(`target: ${ctx.target}`);
}

const opts = parseArgs(process.argv.slice(2));
if (opts.help) { usage(); process.exit(0); }
const ctx = loadContext(opts);
try {
  if (ctx.action === 'doctor') doctor(ctx);
  else if (ctx.action === 'prepare') prepare(ctx);
  else test(ctx);
  report(ctx, 'pass');
} catch (error) {
  console.error(`verify: ${error.message}`);
  report(ctx, 'fail', error.message);
  process.exitCode = 1;
}
