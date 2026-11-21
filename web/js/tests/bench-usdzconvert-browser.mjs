#!/usr/bin/env node
// Browser bench driver for the scene->USDZ conversion pipeline. Serves web/js
// through vite, drives bench-usdzconvert.html with Puppeteer (same hw/sw GPU
// setup as screenshot-usd-assets-batch.mjs), and tabulates per-stage timings:
// folder fetch, wasm init, convert (compose/flatten + texture decode/resize/
// re-encode + usdz pack), and an in-page reload validation of the output.
//
// Run from web/js:
//   xvfb-run -a node tests/bench-usdzconvert-browser.mjs --hw \
//     --scene /path/to/SceneA --root SceneA_01.usd
//   node tests/bench-usdzconvert-browser.mjs --sw --scene <dir> --root <rel> \
//     --case browser:png:1024 --case wasm:png:1024
//
// Each --case is codec:textureFormat:resize
// (codec = browser|wasm, optionally prefixed with stream- or stream-core-).

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const WEB_JS_DIR = path.resolve(SCRIPT_DIR, '..');

function parseArgs(argv = process.argv.slice(2)) {
  const opts = {
    scenes: [],   // { dir, root }
    cases: [],    // { codec, pipeline, textureFormat, resize }
    out: path.join(SCRIPT_DIR, 'bench-usdzconvert'),
    port: 5191,
    hw: false,
    sw: false,
    headful: false,
    timeout: 1800000,
    wasm64: true,
    concurrency: 8,
    jpegQuality: 90,
    includeUnusedTextures: false,
    maxUsdcMb: 0,
    maxMemMb: 0,
    textureMemoryBudget: 0,
  };
  let pendingDir = null;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-h' || a === '--help') { printHelp(); process.exit(0); }
    else if (a === '--scene') pendingDir = path.resolve(argv[++i]);
    else if (a === '--root') {
      if (!pendingDir) throw new Error('--root must follow --scene');
      opts.scenes.push({ dir: pendingDir, root: argv[++i] });
      pendingDir = null;
    }
    else if (a === '--case') {
      // codec:format:resize. codec may carry a "stream-" or "stream-core-"
      // prefix to use the lazy streaming source path instead of the in-memory
      // folder upload.
      let [codec, textureFormat, resize] = argv[++i].split(':');
      let pipeline = 'memory';
      if (codec.startsWith('stream-core-')) { pipeline = 'stream-core'; codec = codec.slice(12); }
      if (codec.startsWith('stream-')) { pipeline = 'stream'; codec = codec.slice(7); }
      if (codec !== 'browser' && codec !== 'wasm') throw new Error(`--case codec must be [stream-|stream-core-]browser|wasm (got ${codec})`);
      opts.cases.push({ codec, pipeline, textureFormat: textureFormat || 'keep', resize: Number(resize || 0) });
    }
    else if (a === '--out') opts.out = path.resolve(argv[++i]);
    else if (a === '--port') opts.port = Number(argv[++i]);
    else if (a === '--hw') opts.hw = true;
    else if (a === '--sw') opts.sw = true;
    else if (a === '--headful') opts.headful = true;
    else if (a === '--timeout') opts.timeout = Number(argv[++i]);
    else if (a === '--wasm32') opts.wasm64 = false;
    else if (a === '--concurrency') opts.concurrency = Number(argv[++i]);
    else if (a === '--jpeg-quality') opts.jpegQuality = Number(argv[++i]);
    else if (a === '--include-unused-textures') opts.includeUnusedTextures = true;
    else if (a === '--max-usdc-mb') opts.maxUsdcMb = Number(argv[++i]);
    else if (a === '--max-mem-mb') opts.maxMemMb = Number(argv[++i]);
    else if (a === '--texture-memory-budget') {
      opts.textureMemoryBudget = parseByteSize(argv[++i]);
      if (!opts.textureMemoryBudget) throw new Error('--texture-memory-budget must be a positive byte size');
    }
    else throw new Error(`Unknown option: ${a}`);
  }
  if (pendingDir) throw new Error('--scene without --root');
  if (!opts.scenes.length) throw new Error('at least one --scene <dir> --root <rel> is required');
  if (!opts.cases.length) {
    opts.cases = [
      { codec: 'wasm', textureFormat: 'png', resize: 1024 },
      { codec: 'browser', textureFormat: 'png', resize: 1024 },
      { codec: 'wasm', textureFormat: 'jpeg', resize: 1024 },
      { codec: 'browser', textureFormat: 'jpeg', resize: 1024 },
    ];
  }
  return opts;
}

function printHelp() {
  console.log(`Usage: node tests/bench-usdzconvert-browser.mjs [options]

Options:
  --scene <dir> --root <rel>   Scene folder + root USD (repeatable, in pairs)
  --case codec:fmt:resize      [stream-|stream-core-]browser|wasm : png|jpeg|keep : N
                               (repeatable; stream prefixes = lazy streaming
                               pipeline; stream-core uses core flattening;
                               keep:0 = passthrough)
                               default: wasm/browser x png/jpeg @1024
  --hw                         ANGLE/Vulkan GPU path; run under xvfb-run -a
  --sw                         Force true-headless SwiftShader
  --headful                    Visible browser
  --wasm32                     Use the wasm32 module (default: wasm64)
  --concurrency <n>            Browser texture-processor concurrency (default 8)
  --include-unused-textures    With stream-core, package unreferenced texture files
  --max-usdc-mb <n>            Raise USDC writer size cap (0 = conservative default;
                               2048 = cross-browser-safe 2 GB ceiling)
  --max-mem-mb <n>             Raise USDC writer memory cap (0 = default)
  --texture-memory-budget <size>
                               Best-effort browser texture working-set budget
                               (e.g. 1GB); caps processor concurrency
  --timeout <ms>               Per-case timeout (default 1800000)
  --out <dir>                  Output dir (default tests/bench-usdzconvert)
  --port <n>                   vite port (default 5191)
  -h, --help`);
}

function posixPath(p) { return p.split(path.sep).join('/'); }
function fileUrlPath(abs) { return `/@fs${posixPath(abs)}`; }

function parseByteSize(value) {
  const match = String(value || '').trim().match(/^([0-9]+(?:\.[0-9]+)?)\s*(b|kb|mb|gb)?$/i);
  if (!match) return 0;
  const units = { b: 1, kb: 1024, mb: 1024 ** 2, gb: 1024 ** 3 };
  const bytes = Number(match[1]) * units[(match[2] || 'b').toLowerCase()];
  return Number.isSafeInteger(Math.floor(bytes)) && bytes > 0 ? Math.floor(bytes) : 0;
}

function buildManifest(sceneDir, baseUrl) {
  const entries = [];
  const walk = (dir) => {
    for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
      if (ent.name.startsWith('.')) continue;
      const p = path.join(dir, ent.name);
      if (ent.isDirectory()) walk(p);
      else if (ent.isFile()) {
        entries.push({
          key: posixPath(path.relative(sceneDir, p)),
          url: `${baseUrl}${fileUrlPath(p)}`,
        });
      }
    }
  };
  walk(sceneDir);
  return entries;
}

async function waitForServer(url, timeoutMs, proc) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (proc && proc.exitCode !== null) {
      throw new Error(`vite exited before ready (exit ${proc.exitCode})`);
    }
    try {
      const res = await fetch(url);
      if (res.ok) return;
    } catch (_) {}
    await new Promise((r) => setTimeout(r, 300));
  }
  throw new Error(`vite did not become ready at ${url}`);
}

function startVite(port, fsAllowDir) {
  const bin = path.join(WEB_JS_DIR, 'node_modules', '.bin', 'vite');
  const proc = spawn(bin, ['--host', '127.0.0.1', '--port', String(port), '--strictPort'], {
    cwd: WEB_JS_DIR,
    stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...process.env, USD_WG_ASSETS_DIR: fsAllowDir },
  });
  proc.stdout.on('data', () => {});
  proc.stderr.on('data', (d) => process.stderr.write(`[vite] ${d}`));
  return proc;
}

// Same GPU selection logic as screenshot-usd-assets-batch.mjs.
function browserLaunchOptions(opts) {
  const NV_ICD = '/usr/share/vulkan/icd.d/nvidia_icd.json';
  const NV_EGL = '/usr/share/glvnd/egl_vendor.d/10_nvidia.json';
  let browserExecutable = process.env.PUPPETEER_EXECUTABLE_PATH;
  if (!browserExecutable) {
    try {
      const bundled = puppeteer.executablePath();
      if (fs.existsSync(bundled)) browserExecutable = bundled;
    } catch (_) {
      // The exact Puppeteer revision is not installed; try system browsers.
    }
  }
  browserExecutable ||= [
    '/usr/bin/google-chrome-stable',
    '/usr/bin/google-chrome',
    '/usr/bin/chromium',
    '/snap/bin/chromium',
  ].find((candidate) => candidate && fs.existsSync(candidate));
  let useHw = opts.hw && !opts.sw;
  if (useHw) {
    const reasons = [];
    if (!process.env.DISPLAY) reasons.push('no DISPLAY (run under `xvfb-run -a`)');
    if (!fs.existsSync(NV_ICD) && !fs.existsSync(NV_EGL)) reasons.push('no NVIDIA Vulkan/EGL driver');
    if (reasons.length) {
      useHw = false;
      console.warn(`  (note) --hw unavailable: ${reasons.join('; ')}; falling back to headless SwiftShader.`);
    }
  }
  const commonArgs = ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage',
    '--ignore-gpu-blocklist', '--disable-gpu-blocklist',
    '--disable-backgrounding-occluded-windows', '--disable-renderer-backgrounding',
    '--disable-background-timer-throttling',
    // multi-GB scenes: let the renderer/wasm heap grow
    '--js-flags=--max-old-space-size=16384'];
  const args = useHw
    ? [...commonArgs, '--use-gl=angle', '--use-angle=vulkan', '--enable-features=Vulkan',
       '--enable-gpu-rasterization', '--enable-zero-copy']
    : [...commonArgs, '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'];
  const hwEnv = useHw ? {
    __NV_PRIME_RENDER_OFFLOAD: '1',
    __GLX_VENDOR_LIBRARY_NAME: 'nvidia',
    __EGL_VENDOR_LIBRARY_FILENAMES: '/usr/share/glvnd/egl_vendor.d/10_nvidia.json',
  } : {};
  return {
    useHw,
    launch: {
      headless: (useHw || opts.headful) ? false : true,
      args,
      env: { ...process.env, ...hwEnv },
      protocolTimeout: opts.timeout + 60000,
      ...(browserExecutable ? { executablePath: browserExecutable } : {}),
    },
  };
}

async function runCase(browser, baseUrl, manifestUrl, scene, kase, opts) {
  const page = await browser.newPage();
  page.setDefaultTimeout(opts.timeout);
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e.message || e)));
  page.on('error', (e) => errors.push(`PAGE CRASH: ${e.message || e}`));
  try {
    const params = new URLSearchParams({
      manifest: manifestUrl,
      root: scene.root,
      resize: String(kase.resize || 0),
      textureFormat: kase.textureFormat,
      codec: kase.codec,
      pipeline: kase.pipeline || 'memory',
      wasm64: opts.wasm64 ? '1' : '0',
      concurrency: String(opts.concurrency),
      jpegQuality: String(opts.jpegQuality),
      includeUnusedTextures: opts.includeUnusedTextures ? '1' : '0',
      maxUsdcMb: String(opts.maxUsdcMb),
      maxMemMb: String(opts.maxMemMb),
      textureMemoryBudget: String(opts.textureMemoryBudget),
    });
    await page.goto(`${baseUrl}/bench-usdzconvert.html?${params.toString()}`,
                    { waitUntil: 'load', timeout: opts.timeout });
    // Live progress: long cases (e.g. wasm png re-encode of hundreds of
    // textures) run for many minutes — surface the page's status line.
    const started = Date.now();
    const progress = setInterval(async () => {
      try {
        const line = await page.evaluate(() => {
          const el = document.getElementById('status');
          const t = el ? el.textContent.trim().split('\n') : [];
          return t.length ? t[t.length - 1] : '';
        });
        const secs = Math.round((Date.now() - started) / 1000);
        console.log(`          [${secs}s] ${line}`);
      } catch (_) {}
    }, 30000);
    try {
      await page.waitForFunction(() => {
        const b = window.__usdzBench;
        return !!b && (b.ready || b.error);
      }, { timeout: opts.timeout, polling: 500 });
    } finally {
      clearInterval(progress);
    }
    const result = await page.evaluate(() => {
      const b = window.__usdzBench;
      return JSON.parse(JSON.stringify(b));
    });
    if (result.error) throw new Error(result.error);
    return { ok: true, ...result, pageErrors: errors };
  } catch (err) {
    return { ok: false, error: String(err.message || err), pageErrors: errors };
  } finally {
    await page.close().catch(() => {});
  }
}

function fmtMs(ms) {
  if (ms == null) return '';
  return ms >= 10000 ? `${(ms / 1000).toFixed(1)}s` : `${Math.round(ms)}ms`;
}

async function main() {
  const opts = parseArgs();
  fs.mkdirSync(opts.out, { recursive: true });

  // fs.allow root: the common parent of all scene dirs.
  const fsAllowDir = path.dirname(opts.scenes[0].dir);

  const baseUrl = `http://127.0.0.1:${opts.port}`;
  const vite = startVite(opts.port, fsAllowDir);
  let browser;
  const rows = [];
  try {
    // Vite's local-development preflight may rebuild the generated WASM glue.
    // A clean build routinely takes longer than 30 seconds, so give startup a
    // bounded five-minute share of the per-case timeout.
    await waitForServer(`${baseUrl}/bench-usdzconvert.html`,
                        Math.min(opts.timeout, 300000), vite);
    const launch = browserLaunchOptions(opts);
    browser = await puppeteer.launch(launch.launch);
    const mode = launch.useHw ? 'ANGLE/Vulkan GPU' : 'SwiftShader/headless';
    console.log(`usdzconvert browser bench (${mode}, ${opts.wasm64 ? 'wasm64' : 'wasm32'})`);

    for (const scene of opts.scenes) {
      // Manifest file served through vite's /@fs.
      const sceneName = path.basename(scene.dir);
      const manifestPath = path.join(opts.out, `${sceneName}.manifest.json`);
      const manifest = buildManifest(scene.dir, baseUrl);
      fs.writeFileSync(manifestPath, JSON.stringify(manifest));
      const manifestUrl = `${baseUrl}${fileUrlPath(manifestPath)}`;
      const sceneMB = manifest.length ? 'n/a' : '0';
      console.log(`\n== ${sceneName} (${manifest.length} files) root=${scene.root}`);

      for (const kase of opts.cases) {
        const prefix = kase.pipeline === 'stream-core' ? 'stream-core-' : (kase.pipeline === 'stream' ? 'stream-' : '');
        const label = `${prefix}${kase.codec}:${kase.textureFormat}:${kase.resize}`;
        process.stdout.write(`  ...   ${label}`);
        // eslint-disable-next-line no-await-in-loop
        const res = await runCase(browser, baseUrl, manifestUrl, scene, kase, opts);
        rows.push({ scene: sceneName, mode, ...kase, ...res });
        if (res.ok) {
          const t = res.timings || {};
          const tex = res.textureStats;
          console.log(`\r  OK    ${label}  fetch=${fmtMs(t.fetchMs)} wasm=${fmtMs(t.wasmInitMs)} ` +
            `convert=${fmtMs(t.convertMs)} validate=${fmtMs(t.validateMs)}${res.validate?.skipped ? ' [validate skipped]' : (res.validate?.ok ? '' : ' [RELOAD FAIL]')} ` +
            `out=${(res.usdzBytes / 1e6).toFixed(0)}MB ` +
            `wasmHeap=${(res.wasmHeapBytes / 1e6 || 0).toFixed(0)}MB jsHeap=${(res.jsHeapBytes / 1e6 || 0).toFixed(0)}MB` +
            (tex ? `  [tex: ${tex.processed} jobs=${res.textureConcurrency} ` +
              `dec=${fmtMs(tex.decodeMs)} raster=${fmtMs(tex.rasterMs)} ` +
              `enc=${fmtMs(tex.encodeMs)}]` : ''));
        } else {
          console.log(`\r  FAIL  ${label}: ${res.error}`);
          if (res.pageErrors?.length) console.log(`          page: ${res.pageErrors.slice(0, 2).join(' | ')}`);
        }
      }
    }
  } finally {
    if (browser) await browser.close().catch(() => {});
    vite.kill('SIGTERM');
  }

  const jsonPath = path.join(opts.out, 'summary.json');
  fs.writeFileSync(jsonPath, JSON.stringify({ generatedAt: new Date().toISOString(), rows }, null, 2));
  const tsvPath = path.join(opts.out, 'summary.tsv');
  fs.writeFileSync(tsvPath, ['status\tmode\tscene\tpipeline\tcodec\tformat\tresize\tgpu\tfetchMs\twasmInitMs\tconvertMs\tvalidateMs\usdzBytes\twasmHeapBytes\tjsHeapBytes\ttextureConcurrency\ttexProcessed\ttexDecodeMs\ttexRasterMs\ttexEncodeMs\terror',
    ...rows.map((r) => [
      r.ok ? 'PASS' : 'FAIL', r.mode, r.scene, r.pipeline || 'memory', r.codec, r.textureFormat, r.resize,
      r.gpu || '', Math.round(r.timings?.fetchMs ?? -1), Math.round(r.timings?.wasmInitMs ?? -1),
      Math.round(r.timings?.convertMs ?? -1), Math.round(r.timings?.validateMs ?? -1),
      r.usdzBytes ?? '', r.wasmHeapBytes ?? '', r.jsHeapBytes ?? '',
      r.textureConcurrency ?? '',
      r.textureStats?.processed ?? '',
      Math.round(r.textureStats?.decodeMs ?? -1), Math.round(r.textureStats?.rasterMs ?? -1),
      Math.round(r.textureStats?.encodeMs ?? -1), r.error || '',
    ].join('\t')),
  ].join('\n') + '\n');

  const pass = rows.filter((r) => r.ok).length;
  console.log(`\n==== bench: ${pass}/${rows.length} OK -> ${opts.out} ====`);
  process.exit(pass === rows.length ? 0 : 1);
}

main().catch((err) => {
  console.error(`bench-usdzconvert-browser: ${err.message}`);
  console.error(err.stack);
  process.exit(1);
});
