// SPDX-License-Identifier: Apache-2.0
import puppeteer from 'puppeteer';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const hardware = process.argv.includes('--hardware');
const shaderball = process.argv.includes('--shaderball');
const executablePath = process.env.CHROME_PATH || (process.platform === 'win32' ? 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe' : undefined);
const port = Number(process.env.WEBGPU_MTLX_TEST_PORT || 5198);
const server = spawn(process.execPath, ['node_modules/vite/bin/vite.js', '--config', 'vite.webgpu-mtlx.config.mjs', '--port', String(port)], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
let log = ''; server.stdout.on('data', d => { log += d; }); server.stderr.on('data', d => { log += d; });
let browser;
try {
  let started = false;
  for (let i = 0; i < 100; i++) { try { if ((await fetch(`http://127.0.0.1:${port}/webgpu-mtlx.html`)).ok) { started = true; break; } } catch {} await new Promise(r => setTimeout(r, 100)); }
  if (!started) throw new Error(`Vite failed: ${log}`);
  browser = await puppeteer.launch({ executablePath, headless: true, args: hardware ? [] : ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const page = await browser.newPage(); await page.setViewport({ width: 1100, height: 700 });
  const errors = []; page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/webgpu-mtlx.html?manual&width=96&height=64`);
  await page.waitForFunction(() => window.__webgpuMtlx?.ready || window.__webgpuMtlx?.errors.length, { timeout: 60000 });
  const initial = await page.evaluate(() => ({ ready: window.__webgpuMtlx.ready, errors: window.__webgpuMtlx.errors }));
  assert.deepEqual(initial.errors, [], 'initialization errors'); assert.ok(initial.ready);
  const results = await page.evaluate(async () => {
    const state = window.__webgpuMtlx, r = state.renderer;
    const { validateValueKernels } = await import('/src/webgpu-mtlx/gpu-validation.js');
    const numeric = await validateValueKernels(r.device);
    for (let i = 0; i < 4; i++) await r.renderStep();
    const capture = await r.capture({ format: 'float32' });
    const luminance = []; for (let i = 0; i < capture.pixels.length; i += 4) luminance.push(capture.pixels[i] + capture.pixels[i + 1] + capture.pixels[i + 2]);
    const samples = r.samples; r.setOptions({ exposure: 1 }); const exposureSamples = r.samples;
    r.setCamera(r.camera); const reset = r.samples; await r.renderStep();
    r.setMode('realtime'); await r.renderStep(); const raster = { ...r.stats };
    r.setMode('path-preview'); r.setOptions({ resolutionScale: 0.5 }); await r.renderStep();
    const resized = await r.capture({ format: 'float32' });
    const { syntheticScene } = await import('/src/webgpu-mtlx/scene.js'); await r.loadScene(syntheticScene('graph')); await r.renderStep();
    return { numeric, samples, exposureSamples, reset, raster, resized: resized.metadata, min: Math.min(...luminance), max: Math.max(...luminance), finite: capture.pixels.every(Number.isFinite), adapter: r.stats.adapter, errors: [...state.errors, ...r.errors] };
  });
  assert.equal(results.samples, 4); assert.equal(results.exposureSamples, 4); assert.equal(results.reset, 0);
  assert.ok(results.finite); assert.ok(results.max - results.min > 0.1, 'nonblank radiance');
  assert.equal(results.resized.width, 48); assert.equal(results.resized.height, 32);
  assert.deepEqual(results.errors, []); assert.deepEqual(errors, []);
  await page.click('#inventory');
  await page.waitForFunction(() => window.__webgpuMtlx.inventory || window.__webgpuMtlx.errors.length, { timeout: 60000 });
  const inventory = await page.evaluate(() => window.__webgpuMtlx.inventory);
  assert.ok(inventory?.length > 100, 'pinned library inventory');
  if (hardware) assert.ok(!results.adapter.isFallbackAdapter && !/swiftshader|software/i.test(JSON.stringify(results.adapter)), 'hardware gate must not use software');
  const out = path.join(root, '.regression/webgpu-mtlx'); fs.mkdirSync(out, { recursive: true });
  await page.evaluate(async () => {
    const r = window.__webgpuMtlx.renderer;
    r.canvas.width = 640; r.canvas.height = 360; r.setOptions({ resolutionScale: 1, exposure: 0 });
    for (let i = 0; i < 32; i++) await r.renderStep();
  });
  await page.screenshot({ path: path.join(out, 'chrome.png') });
  let shaderballResult;
  if (shaderball) {
    await page.select('#scene', 'shaderball');
    await page.waitForFunction(() => window.__webgpuMtlx.ready || window.__webgpuMtlx.errors.length, { timeout: 120000 });
    shaderballResult = await page.evaluate(async () => {
      const state = window.__webgpuMtlx;
      if (state.errors.length) return { errors: state.errors };
      await state.renderer.renderStep();
      return { stats: state.renderer.stats, provenance: state.renderer.scene.provenance, errors: state.errors };
    });
    assert.deepEqual(shaderballResult.errors, []); assert.ok(shaderballResult.stats.triangles > 1000);
    await page.screenshot({ path: path.join(out, 'shaderball.png') });
  }
  let performanceResult;
  if (process.argv.includes('--performance')) {
    performanceResult = await page.evaluate(async () => {
      const r = window.__webgpuMtlx.renderer; r.canvas.width = 1280; r.canvas.height = 720; r.setMode('realtime'); r.setOptions({ resolutionScale: 1, autoResolution: false });
      for (let i = 0; i < 30; i++) await r.renderStep();
      const times = [], start = performance.now();
      while (performance.now() - start < 30000) { await r.renderStep(); times.push(r.stats.milliseconds); }
      times.sort((a, b) => a - b);
      const png = await r.capture({ format: 'png' });
      return { width: 1280, height: 720, seconds: (performance.now() - start) / 1000, frames: times.length, median: times[Math.floor(times.length * 0.5)], p95: times[Math.floor(times.length * 0.95)], pngBytes: png.bytes.length };
    });
    assert.ok(performanceResult.pngBytes > 100); assert.ok(performanceResult.p95 <= 33.3, `720p frame p95 ${performanceResult.p95} exceeds target`);
  }
  const report = { browser: await browser.version(), requestedHardware: hardware, inventoriedNodeDefs: inventory.length, shaderball: shaderballResult, performance: performanceResult, ...results };
  fs.writeFileSync(path.join(out, 'chrome.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ browser: report.browser, adapter: results.adapter, inventoriedNodeDefs: inventory.length, numericPassed: results.numeric.length, samples: results.samples, shaderball: shaderballResult, performance: performanceResult, errors: results.errors, report: path.relative(root, path.join(out, 'chrome.json')) }, null, 2));
} finally { await browser?.close(); server.kill(); }
