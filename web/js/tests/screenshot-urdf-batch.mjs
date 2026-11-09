#!/usr/bin/env node
// Batch visual-verification screenshotter for the urdf.js web demo.
//
// For each MuJoCo model it drives the real urdf.html UI through headless Chrome
// (Puppeteer): index the robot's mesh assets, import the MJCF/URDF (renders the
// source view), click "URDF/MJCF -> USD" (renders the converted USD view), then
// screenshot the split-view comparison. This verifies the read -> convert ->
// render path visually, per robot.
//
// The demo uses bare ES module specifiers + a WASM module, so it is served by a
// throwaway vite dev server that this script spawns and tears down.
//
// Always run under xvfb so Chrome has an (off-screen) display for WebGL and
// never steals window focus — this works for BOTH software and --hw GPU modes:
//   xvfb-run -a node tests/screenshot-urdf-batch.mjs            # curated, SwiftShader
//   xvfb-run -a node tests/screenshot-urdf-batch.mjs --all --hw # every robot, NVIDIA GPU
//   xvfb-run -a node tests/screenshot-urdf-batch.mjs a.xml b.xml
//   xvfb-run -a node tests/screenshot-urdf-batch.mjs --out /tmp/shots
//
// Options:
//   --out <dir>        Screenshot output dir (default: tests/screenshots)
//   --menagerie <dir>  Dataset root (default: MENAGERIE_DIR or MUJOCO_MENAGERIE or ../.. /mujoco_menagerie)
//   --all              Screenshot the primary MJCF of every robot directory
//   --port <n>         vite dev-server port (default: 5188)
//   --width/--height   Viewport size (default: 1600x900)
//   --headful          Launch a visible browser (needs a real display)
//   --hw               Real GPU (NVIDIA) WebGL via ANGLE+Vulkan. Run under
//                      `xvfb-run -a` (off-screen, no focus stealing); does NOT
//                      need a real DISPLAY. Chrome's true headless can't do HW
//                      WebGL on Linux. Software SwiftShader is the default and
//                      is very slow for large scenes (e.g. robot_soccer_kit:
//                      363 meshes) — use --hw when a GPU is available.
//   --home-pose        Capture the model's home keyframe pose
//   --allow-blank      Don't fail when a view renders no visible meshes
//   --timeout <ms>     Per-step timeout (default: 180000)
//   -h, --help

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const WEB_JS_DIR = path.resolve(SCRIPT_DIR, '..');
const DEFAULT_MENAGERIE_ROOT = path.resolve(SCRIPT_DIR, '../..', 'mujoco_menagerie');

// Mesh/texture assets plus .xml so MJCF <include> files (some models split the
// robot across dozens of included XMLs, e.g. ms_human_700) are resolvable.
const ASSET_EXTS = new Set(['.stl', '.obj', '.mtl', '.png', '.jpg', '.jpeg', '.xml', '.msh']);

// Curated representative set (relative to the menagerie root).
const DEFAULT_ROBOTS = [
  'universal_robots_ur5e/ur5e.xml',
  'franka_emika_panda/panda.xml',
  'unitree_go2/go2.xml',
  'unitree_g1/g1_with_hands.xml',
  'agility_cassie/cassie.xml',
];

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    out: path.join(SCRIPT_DIR, 'screenshots'),
    menagerie: path.resolve(process.env.MENAGERIE_DIR || process.env.MUJOCO_MENAGERIE || DEFAULT_MENAGERIE_ROOT),
    all: false,
    port: 5188,
    width: 1920,
    height: 1080,
    headful: false,
    hw: false,
    homePose: false,
    allowBlank: false,
    timeout: 180000,
    explicit: []
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-h' || a === '--help') { printHelp(); process.exit(0); }
    else if (a === '--out') opts.out = path.resolve(argv[++i]);
    else if (a === '--menagerie') opts.menagerie = path.resolve(argv[++i]);
    else if (a === '--all') opts.all = true;
    else if (a === '--port') opts.port = Number(argv[++i]);
    else if (a === '--width') opts.width = Number(argv[++i]);
    else if (a === '--height') opts.height = Number(argv[++i]);
    else if (a === '--headful') opts.headful = true;
    else if (a === '--hw') opts.hw = true;
    else if (a === '--home-pose') opts.homePose = true;
    else if (a === '--collisions') opts.collisions = true;
    else if (a === '--allow-blank') opts.allowBlank = true;
    else if (a === '--timeout') opts.timeout = Number(argv[++i]);
    else if (a.startsWith('-')) throw new Error(`Unknown option: ${a}`);
    else opts.explicit.push(a);
  }
  return opts;
}

function printHelp() {
  const text = fs.readFileSync(fileURLToPath(import.meta.url), 'utf8');
  console.log(text.split('\n').slice(2, 33).join('\n'));
}

// Pick the primary MJCF of a robot directory (skip scene/keyframe/mjx variants).
function discoverAll(menagerie) {
  const out = [];
  for (const entry of fs.readdirSync(menagerie, { withFileTypes: true })) {
    if (!entry.isDirectory() || entry.name.startsWith('.')) continue;
    const dir = path.join(menagerie, entry.name);
    const xmls = fs.readdirSync(dir).filter((f) => f.endsWith('.xml')).sort();
    for (const f of xmls) {
      if (/scene|keyframe|mjx|nohand|_left|left_/i.test(f)) continue;
      const text = fs.readFileSync(path.join(dir, f), 'utf8');
      if (text.includes('<worldbody') && text.includes('<body')) { out.push(path.join(dir, f)); break; }
    }
  }
  return out;
}

// All mesh/texture assets under a robot directory (uploaded flat; the demo
// resolves them by basename).
function collectAssetFiles(robotDir) {
  const files = [];
  const walk = (dir, depth) => {
    if (depth > 4) return;
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) walk(p, depth + 1);
      else if (ASSET_EXTS.has(path.extname(e.name).toLowerCase())) files.push(p);
    }
  };
  walk(robotDir, 0);
  return files;
}

async function waitForServer(url, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(url);
      if (res.ok) return true;
    } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 300));
  }
  throw new Error(`vite server did not become ready at ${url}`);
}

function startVite(port) {
  const bin = path.join(WEB_JS_DIR, 'node_modules', '.bin', 'vite');
  const proc = spawn(bin, ['--force', '--port', String(port), '--strictPort'], {
    cwd: WEB_JS_DIR,
    env: { ...process.env, LIGHTUSD_SKIP_WASM_PREPARE: '1' },
    stdio: ['ignore', 'pipe', 'pipe']
  });
  proc.stdout.on('data', () => {});
  proc.stderr.on('data', (d) => process.stderr.write(`[vite] ${d}`));
  return proc;
}

async function statusText(page) {
  return page.$eval('#status', (el) => el.textContent || '').catch(() => '');
}

async function waitForStatus(page, matcher, timeout) {
  await page.waitForFunction(
    (re) => new RegExp(re, 'i').test(document.getElementById('status')?.textContent || ''),
    { timeout },
    matcher
  );
}

async function shootRobot(browser, baseUrl, mjcf, opts) {
  const robotDir = path.dirname(mjcf);
  const sub = path.basename(robotDir);
  const name = path.basename(mjcf, path.extname(mjcf));
  const page = await browser.newPage();
  page.setDefaultTimeout(opts.timeout);
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e.message || e)));
  page.on('error', (e) => errors.push(`PAGE CRASH: ${e.message || e}`));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });

  try {
    await page.setViewport({ width: opts.width, height: opts.height });
    await page.goto(`${baseUrl}/urdf.html`, { waitUntil: 'load' });
    await page.waitForSelector('#convertToUSD', { timeout: opts.timeout });

    // 1) Index mesh assets (must precede import: loadRobotFile resolves meshes now).
    // Puppeteer can't drive a `webkitdirectory` input, so drop that attribute and
    // upload the assets flat — the demo resolves meshes by basename anyway.
    const assets = collectAssetFiles(robotDir);
    if (assets.length) {
      await page.$eval('#assetInput', (el) => el.removeAttribute('webkitdirectory'));
      const assetInput = await page.$('#assetInput');
      await assetInput.uploadFile(...assets);
      await waitForStatus(page, 'asset files indexed', opts.timeout);
    }

    // 2) Import the source model -> renders the source view.
    const urdfInput = await page.$('#urdfInput');
    await urdfInput.uploadFile(mjcf);
    await waitForStatus(page, `Loaded .* ${name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}`, opts.timeout);

    // Optionally switch to the model's home keyframe pose before converting, so
    // both views render posed (the "Home pose" GUI toggle).
    if (opts.homePose) {
      await page.evaluate(() => {
        const ctrl = [...document.querySelectorAll('.lil-gui .controller')]
          .find((el) => el.querySelector('.name')?.textContent === 'Home pose');
        const cb = ctrl?.querySelector('input[type="checkbox"]');
        if (cb && !cb.checked) cb.click();
      });
      await new Promise((r) => setTimeout(r, 300));
    }

    // Optionally show collision geoms (and hide visuals) to inspect collision geometry.
    if (opts.collisions) {
      await page.evaluate(() => {
        const ctrls = [...document.querySelectorAll('.lil-gui .controller')];
        const set = (label, want) => {
          const cb = ctrls.find((el) => el.querySelector('.name')?.textContent === label)?.querySelector('input[type="checkbox"]');
          if (cb && cb.checked !== want) cb.click();
        };
        set('Collision meshes', true);
        set('Visual meshes', false);
      });
      await new Promise((r) => setTimeout(r, 200));
    }

    // 3) Convert to USD -> renders the converted view in the split comparison.
    await page.click('#convertToUSD');
    await waitForStatus(page, 'Converted', opts.timeout);

    // Fold/hide the overlay UI so it doesn't occlude the views, then refit.
    await page.evaluate(() => {
      for (const sel of ['#panel', '.lil-gui', '#topbar']) {
        const el = document.querySelector(sel);
        if (el) el.style.display = 'none';
      }
      if (typeof window.__fitView === 'function') window.__fitView();
    });

    // Let a couple of animation frames settle, then capture.
    await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));
    await new Promise((r) => setTimeout(r, 400));

    const stats = await page.evaluate(() => ({
      links: document.getElementById('linkCount')?.textContent,
      joints: document.getElementById('jointCount')?.textContent,
      meshes: document.getElementById('meshCount')?.textContent,
      status: document.getElementById('status')?.textContent
    }));

    // Non-empty-render check: a model can convert + screenshot "successfully"
    // yet render blank (e.g. all geoms hidden/misclassified). Fail loudly when
    // a robot is loaded but a view draws no visible meshes.
    const render = await page.evaluate(() => (window.__viewerStats ? window.__viewerStats() : null));
    const outPath = path.join(opts.out, `${sub}__${name}.png`);
    await page.screenshot({ path: outPath });
    if (!opts.allowBlank && render && render.links > 0 &&
        (render.sourceVisibleMeshes === 0 || render.usdVisibleMeshes === 0)) {
      return {
        ok: false,
        error: `blank render (visible meshes: source=${render.sourceVisibleMeshes}, usd=${render.usdVisibleMeshes})`,
        status: stats.status, errors, failPath: outPath
      };
    }
    return { ok: true, outPath, stats, render };
  } catch (err) {
    // Capture a failure screenshot for debugging.
    const failPath = path.join(opts.out, `${sub}__${name}.FAIL.png`);
    await page.screenshot({ path: failPath }).catch(() => {});
    return { ok: false, error: err.message, status: await statusText(page), errors, failPath };
  } finally {
    await page.close().catch(() => {});
  }
}

async function main() {
  const opts = parseArgs();
  fs.mkdirSync(opts.out, { recursive: true });

  let models;
  if (opts.explicit.length) models = opts.explicit.map((p) => path.resolve(p));
  else if (opts.all) models = discoverAll(opts.menagerie);
  else models = DEFAULT_ROBOTS.map((r) => path.join(opts.menagerie, r)).filter((p) => fs.existsSync(p));

  if (!models.length) { console.error('No models to screenshot.'); process.exit(2); }

  console.log(`Screenshotting ${models.length} model(s) -> ${opts.out}`);
  const baseUrl = `http://localhost:${opts.port}`;
  const vite = startVite(opts.port);
  let browser;
  let pass = 0;
  const failures = [];

  try {
    const requestedViteTimeoutMs = Number(process.env.LIGHTUSD_VITE_TIMEOUT_MS);
    const viteTimeoutMs = Number.isFinite(requestedViteTimeoutMs) && requestedViteTimeoutMs > 0
      ? requestedViteTimeoutMs : 120000;
    await waitForServer(`${baseUrl}/urdf.html`, viteTimeoutMs);
    // Two render paths:
    //  * --hw: real GPU (NVIDIA) WebGL. Chrome's true headless (--headless=new)
    //    can't do hardware WebGL on Linux (it falls back to SwiftShader), so run
    //    `headless:false` under a virtual framebuffer — launch with
    //    `xvfb-run -a node ...` (NOT a real DISPLAY). xvfb gives Chrome an
    //    off-screen X11 connection (ANGLE's Vulkan backend needs one) with no
    //    visible window, so it won't steal focus, while ANGLE+Vulkan uses the
    //    NVIDIA GPU. See https://zenn.dev/syoyo/articles/4f084b2288428f .
    //  * fallback / default: true-headless Chrome + software WebGL (SwiftShader).
    //    Needs neither xvfb nor a GPU, so it works anywhere (CI, headless boxes)
    //    -- just slow for huge scenes. We auto-fall-back to this if --hw was
    //    asked for but there's no display or no NVIDIA Vulkan stack.
    // Headful/xvfb Chrome throttles rAF/timers on unfocused/occluded windows,
    // which stalls the demo's async convert chain — disable that for determinism.
    const NV_ICD = '/usr/share/vulkan/icd.d/nvidia_icd.json';
    const NV_EGL = '/usr/share/glvnd/egl_vendor.d/10_nvidia.json';
    let useHw = opts.hw;
    if (opts.hw) {
      const reasons = [];
      if (!process.env.DISPLAY) reasons.push('no DISPLAY (run under `xvfb-run -a`)');
      if (!fs.existsSync(NV_ICD) && !fs.existsSync(NV_EGL)) reasons.push('no NVIDIA Vulkan/EGL driver');
      if (reasons.length) {
        useHw = false;
        console.warn(`  (note) --hw unavailable: ${reasons.join('; ')}; falling back to headless software WebGL (SwiftShader).`);
      }
    }
    const commonArgs = ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage',
      '--ignore-gpu-blocklist', '--disable-gpu-blocklist', `--window-size=${opts.width},${opts.height}`,
      '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
      '--disable-background-timer-throttling', '--disable-gpu-vsync', '--disable-frame-rate-limit'];
    const args = useHw
      ? [...commonArgs, '--use-gl=angle', '--use-angle=vulkan', '--enable-features=Vulkan',
         '--enable-gpu-rasterization', '--enable-zero-copy']
      : [...commonArgs, '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'];
    // Force the NVIDIA GPU under PRIME/glvnd so ANGLE/Vulkan doesn't fall back
    // to Mesa llvmpipe (software) when running through xvfb.
    const hwEnv = useHw ? {
      __NV_PRIME_RENDER_OFFLOAD: '1',
      __GLX_VENDOR_LIBRARY_NAME: 'nvidia',
      __EGL_VENDOR_LIBRARY_FILENAMES: '/usr/share/glvnd/egl_vendor.d/10_nvidia.json'
    } : {};
    // GPU path needs an X display (xvfb) -> headless:false; software + --headful
    // also use headless:false; otherwise true headless (no display/GPU needed).
    browser = await puppeteer.launch({
      headless: (useHw || opts.headful) ? false : true,
      args,
      env: { ...process.env, ...hwEnv }
    });

    for (const mjcf of models) {
      const label = `${path.basename(path.dirname(mjcf))}/${path.basename(mjcf)}`;
      if (!fs.existsSync(mjcf)) { console.log(`  SKIP  ${label} (missing)`); failures.push(`${label} (missing)`); continue; }
      process.stdout.write(`  ...   ${label}`);
      const res = await shootRobot(browser, baseUrl, mjcf, opts);
      if (res.ok) {
        pass++;
        const vis = res.render ? ` vis(src/usd)=${res.render.sourceVisibleMeshes}/${res.render.usdVisibleMeshes}` : '';
        console.log(`\r  OK    ${label}  [links=${res.stats.links} joints=${res.stats.joints} meshes=${res.stats.meshes}${vis}] -> ${path.basename(res.outPath)}`);
      } else {
        failures.push(`${label}: ${res.error}`);
        console.log(`\r  FAIL  ${label}: ${res.error}`);
        if (res.status) console.log(`          status: ${res.status}`);
        if (res.errors?.length) console.log(`          page: ${res.errors.slice(0, 2).join(' | ')}`);
      }
    }
  } finally {
    if (browser) await browser.close().catch(() => {});
    vite.kill('SIGTERM');
  }

  console.log(`\n==== screenshots: ${pass}/${models.length} OK -> ${opts.out} ====`);
  if (failures.length) {
    console.log('--- failures ---');
    for (const f of failures) console.log(`  - ${f}`);
  }
  process.exit(failures.length ? 1 : 0);
}

main().catch((err) => {
  console.error(`screenshot-urdf-batch: ${err.message}`);
  console.error(err.stack);
  process.exit(1);
});
