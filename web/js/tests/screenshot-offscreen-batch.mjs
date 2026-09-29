#!/usr/bin/env node

// Full Menagerie browser gate for the real OffscreenCanvas + Worker renderer.
// The MJCF conversion is deliberately performed by run-mjcf-roundtrip.sh;
// this harness consumes those converted USD files and verifies the browser
// rendering leg independently.

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer';
import { PNG } from 'pngjs';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const WEB_JS_DIR = path.resolve(SCRIPT_DIR, '..');
const DEFAULT_MENAGERIE = path.resolve(SCRIPT_DIR, '../.cache/mujoco_menagerie');

const DEFAULT_ROBOTS = [
  'universal_robots_ur5e/ur5e.xml',
  'franka_emika_panda/panda.xml',
  'kuka_iiwa_14/iiwa14.xml',
  'trossen_vx300s/vx300s.xml',
  'unitree_go2/go2.xml',
  'anybotics_anymal_c/anymal_c.xml',
  'boston_dynamics_spot/spot_arm.xml',
  'unitree_h1/h1.xml',
  'unitree_g1/g1_with_hands.xml',
  'robotiq_2f85/2f85.xml',
  'shadow_hand/right_hand.xml',
  'agility_cassie/cassie.xml',
  'google_robot/robot.xml',
  'skydio_x2/x2.xml',
];

function usage() {
  console.log(`
OffscreenCanvas Menagerie renderer

Usage:
  node tests/screenshot-offscreen-batch.mjs [options]

Options:
  --menagerie <dir>       Menagerie checkout
  --converted-dir <dir>   run-mjcf-roundtrip output directory
  --out <dir>             Screenshot output directory
  --all                   Use every discovered primary MJCF
  --hw                    Use ANGLE/Vulkan hardware rendering
  --sw                    Use headless SwiftShader rendering
  --port <n>              Vite port (default: 5190)
  --timeout <ms>          Per-model timeout (default: 180000)
  -h, --help              Show this help
`);
}

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    menagerie: path.resolve(process.env.MENAGERIE_DIR || process.env.MUJOCO_MENAGERIE || DEFAULT_MENAGERIE),
    convertedDir: null,
    out: path.join(SCRIPT_DIR, 'screenshots', 'offscreen'),
    all: false,
    hw: false,
    port: 5190,
    timeout: 180000,
    explicit: [],
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '-h' || arg === '--help') { usage(); process.exit(0); }
    else if (arg === '--menagerie') opts.menagerie = path.resolve(argv[++i]);
    else if (arg === '--converted-dir') opts.convertedDir = path.resolve(argv[++i]);
    else if (arg === '--out') opts.out = path.resolve(argv[++i]);
    else if (arg === '--all') opts.all = true;
    else if (arg === '--hw') opts.hw = true;
    else if (arg === '--sw') opts.hw = false;
    else if (arg === '--port') opts.port = Number(argv[++i]);
    else if (arg === '--timeout') opts.timeout = Number(argv[++i]);
    else if (arg.startsWith('-')) throw new Error(`Unknown option: ${arg}`);
    else opts.explicit.push(path.resolve(arg));
  }
  if (!opts.convertedDir) throw new Error('--converted-dir is required');
  return opts;
}

function discoverAll(menagerie) {
  const out = [];
  for (const entry of fs.readdirSync(menagerie, { withFileTypes: true })) {
    if (!entry.isDirectory() || entry.name.startsWith('.')) continue;
    const dir = path.join(menagerie, entry.name);
    const xmls = fs.readdirSync(dir).filter((file) => file.endsWith('.xml')).sort();
    for (const file of xmls) {
      if (/scene|keyframe|mjx|nohand|_left|left_/i.test(file)) continue;
      const full = path.join(dir, file);
      const source = fs.readFileSync(full, 'utf8');
      if (source.includes('<worldbody') && source.includes('<body')) {
        out.push(full);
        break;
      }
    }
  }
  return out;
}

function convertedPath(convertedDir, mjcf) {
  const model = path.basename(mjcf, path.extname(mjcf));
  const robot = path.basename(path.dirname(mjcf));
  return path.join(convertedDir, robot, `${model}.usdc`);
}

async function waitForServer(url, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url);
      if (response.ok) return;
    } catch { /* server is still starting */ }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw new Error(`Vite server did not become ready at ${url}`);
}

function startVite(port) {
  const bin = path.join(WEB_JS_DIR, 'node_modules', '.bin', 'vite');
  const server = spawn(bin, ['--force', '--port', String(port), '--strictPort'], {
    cwd: WEB_JS_DIR,
    env: { ...process.env, LIGHTUSD_SKIP_WASM_PREPARE: '1' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  server.stdout.on('data', () => {});
  server.stderr.on('data', (data) => process.stderr.write(`[vite] ${data}`));
  return server;
}

// Count pixels that differ from the environment background. The background is
// a vertical gradient, so each pixel is compared against the same row's
// right-edge pixel. The element capture also contains the HUD overlays (stats
// panel, load button, badge, help bar), which are masked out.
//
// SwiftShader can render a pale model whose RGB contrast against the HDR
// background is only a few dozen channel levels. A viewport fraction test
// also rejects small end-effectors even when their geometry is present. Use a
// small per-pixel contrast threshold and a minimum pixel count instead. The
// blank worker frame's background variation stays below this threshold.
function renderedPixelCount(image) {
  const {width, height, data} = image;
  const masked = (x, y) => y < 70 || y > height - 60 || (x < 260 && y < 320);
  const contrastThreshold = 8;
  let changed = 0;
  for (let y = 0; y < height; ++y) {
    const ref = (y * width + width - 3) * 4;
    for (let x = 0; x < width; ++x) {
      if (masked(x, y)) continue;
      const i = (y * width + x) * 4;
      const diff = Math.abs(data[i] - data[ref]) + Math.abs(data[i + 1] - data[ref + 1]) +
        Math.abs(data[i + 2] - data[ref + 2]);
      if (diff > contrastThreshold) changed++;
    }
  }
  return changed;
}

function hasRenderedPixels(buffer) {
  return renderedPixelCount(PNG.sync.read(buffer)) >= 8;
}

async function renderOne(browser, baseUrl, mjcf, opts) {
  const robot = path.basename(path.dirname(mjcf));
  const model = path.basename(mjcf, path.extname(mjcf));
  const usd = convertedPath(opts.convertedDir, mjcf);
  const label = `${robot}/${model}`;
  if (!fs.existsSync(usd)) return { ok: false, label, error: `missing converted USD: ${usd}` };

  const page = await browser.newPage();
  page.setDefaultTimeout(opts.timeout);
  const errors = [];
  page.on('pageerror', (error) => errors.push(`page: ${error.message}`));
  page.on('error', (error) => errors.push(`crash: ${error.message}`));
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(`console: ${message.text()}`);
  });

  const output = path.join(opts.out, `${robot}__${model}.png`);
  const failOutput = path.join(opts.out, `${robot}__${model}.FAIL.png`);
  try {
    await page.setViewport({ width: 1280, height: 900 });
    await page.goto(`${baseUrl}/offscreengl.html?test=1`, { waitUntil: 'load' });
    await page.waitForFunction(() => window.__offscreenTestState?.initialized === true);
    await page.waitForFunction(() => window.__offscreenTestState?.ready === true,
      { timeout: opts.timeout });
    const input = await page.$('#file-input');
    await input.uploadFile(usd);
    await page.waitForFunction(
      () => (window.__offscreenTestState?.loaded?.length || 0) >= 1,
      { timeout: opts.timeout },
    );
    await page.waitForFunction(
      () => (window.__offscreenTestState?.errors?.length || 0) === 0,
      { timeout: 1000 },
    ).catch(() => {});
    await new Promise((resolve) => setTimeout(resolve, 500));

    const state = await page.evaluate(() => ({
      loaded: window.__offscreenTestState.loaded,
      errors: window.__offscreenTestState.errors,
      status: document.getElementById('status')?.textContent || '',
      meshes: Number(document.getElementById('mesh-count')?.textContent || 0),
    }));
    if (state.errors.length) throw new Error(state.errors.join(' | '));
    if (!state.meshes || !state.loaded[0]?.meshCount) {
      throw new Error(`worker reported no renderable meshes (${state.status})`);
    }

    // The first frame uploads every vertex buffer, which takes seconds on
    // large scenes (ms_human_700 carries ~1 GB), so poll until it lands.
    const canvas = await page.$('#gl');
    const renderDeadline = Date.now() + opts.timeout;
    let image = await canvas.screenshot({ encoding: 'binary' });
    while (!hasRenderedPixels(image) && Date.now() < renderDeadline) {
      await new Promise((resolve) => setTimeout(resolve, 1000));
      image = await canvas.screenshot({ encoding: 'binary' });
    }
    if (!hasRenderedPixels(image)) throw new Error('OffscreenCanvas render is blank');
    fs.writeFileSync(output, image);
    return { ok: true, label, output, meshes: state.meshes };
  } catch (error) {
    await page.screenshot({ path: failOutput }).catch(() => {});
    return { ok: false, label, error: `${error.message}${errors.length ? ` [${errors.join(' | ')}]` : ''}`, output: failOutput };
  } finally {
    await page.close().catch(() => {});
  }
}

async function main() {
  const opts = parseArgs();
  if (!fs.existsSync(opts.menagerie)) throw new Error(`missing Menagerie: ${opts.menagerie}`);
  if (!fs.existsSync(opts.convertedDir)) throw new Error(`missing converted directory: ${opts.convertedDir}`);
  fs.mkdirSync(opts.out, { recursive: true });
  const models = opts.explicit.length ? opts.explicit :
    (opts.all ? discoverAll(opts.menagerie) : DEFAULT_ROBOTS
      .map((relative) => path.join(opts.menagerie, relative))
      .filter((file) => fs.existsSync(file)));
  if (!models.length) throw new Error('no Menagerie models selected');

  const baseUrl = `http://localhost:${opts.port}`;
  const server = startVite(opts.port);
  let browser;
  const results = [];
  try {
    const requestedViteTimeoutMs = Number(process.env.LIGHTUSD_VITE_TIMEOUT_MS);
    const viteTimeoutMs = Number.isFinite(requestedViteTimeoutMs) && requestedViteTimeoutMs > 0
      ? requestedViteTimeoutMs : 120000;
    await waitForServer(`${baseUrl}/offscreengl.html`, viteTimeoutMs);
    const commonArgs = [
      '--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage',
      '--ignore-gpu-blocklist', '--disable-gpu-blocklist',
      '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
      '--disable-background-timer-throttling', '--disable-gpu-vsync',
      '--disable-frame-rate-limit', '--window-size=1280,900',
    ];
    const args = opts.hw
      ? [...commonArgs, '--use-gl=angle', '--use-angle=vulkan', '--enable-features=Vulkan',
        '--enable-gpu-rasterization', '--enable-zero-copy']
      : [...commonArgs, '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'];
    browser = await puppeteer.launch({
      headless: !opts.hw,
      args,
      env: opts.hw ? {
        ...process.env,
        __NV_PRIME_RENDER_OFFLOAD: '1',
        __GLX_VENDOR_LIBRARY_NAME: 'nvidia',
        __EGL_VENDOR_LIBRARY_FILENAMES: '/usr/share/glvnd/egl_vendor.d/10_nvidia.json',
      } : process.env,
    });
    for (const model of models) {
      process.stdout.write(`  ...   ${path.basename(path.dirname(model))}/${path.basename(model)}\n`);
      const result = await renderOne(browser, baseUrl, model, opts);
      results.push(result);
      console.log(result.ok ? `  OK    ${result.label} [meshes=${result.meshes}]` : `  FAIL  ${result.label}: ${result.error}`);
    }
  } finally {
    if (browser) await browser.close().catch(() => {});
    server.kill('SIGTERM');
  }

  const failed = results.filter((result) => !result.ok);
  fs.writeFileSync(path.join(opts.out, 'summary.json'), `${JSON.stringify({
    models: results.length, passed: results.length - failed.length, failed: failed.length, results,
  }, null, 2)}\n`);
  console.log(`\n==== OffscreenCanvas: ${results.length - failed.length}/${results.length} OK ====\n`);
  if (failed.length) process.exitCode = 1;
}

main().catch((error) => {
  console.error(`screenshot-offscreen-batch: ${error.message}`);
  process.exitCode = 1;
});
