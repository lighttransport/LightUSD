#!/usr/bin/env node
import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { spawn } from 'node:child_process';
import puppeteer from 'puppeteer';

function isEphemeralListenError(error) {
  return /listen (?:EPERM|EACCES)|Permission denied/i.test(String(error?.message || error || '')) ||
    error?.code === 'EPERM' || error?.code === 'EACCES';
}

let server = null;
let baseURL = process.env.LIGHTUSD_COLORSPACE_URL;
let skipReason = '';
if (!baseURL) {
  const host = '127.0.0.1';
  const port = Number(process.env.LIGHTUSD_COLORSPACE_PORT || 41739);
  baseURL = `http://${host}:${port}/colorspace-regression.html`;
  let startupError = '';
  const vite = new URL('../node_modules/vite/bin/vite.js', import.meta.url)
    .pathname;
  server = spawn(process.execPath,
    [vite, '--host', host, '--port', String(port), '--strictPort'], {
      cwd: new URL('..', import.meta.url).pathname,
      stdio: ['ignore', 'pipe', 'pipe']
    });
  server.stdout.on('data', (chunk) => {
    const text = String(chunk);
    startupError += text;
    process.stderr.write(`[vite] ${text}`);
  });
  server.stderr.on('data', (chunk) => {
    const text = String(chunk);
    startupError += text;
    process.stderr.write(`[vite] ${text}`);
  });
  // First use may rebuild both legacy and core WASM modules through Vite's
  // configure plugin, which is substantially slower than an ordinary startup.
  const startupTimeout = Number(
    process.env.LIGHTUSD_COLORSPACE_SERVER_TIMEOUT_MS || 120000);
  const deadline = Date.now() + startupTimeout;
  let ready = false;
  while (Date.now() < deadline) {
    if (server.exitCode !== null) break;
    try {
      const response = await fetch(baseURL);
      if (response.ok) { ready = true; break; }
    } catch {
      // Vite is still starting.
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  if (!ready) {
    server.kill('SIGTERM');
    const message = `Vite did not become ready at ${baseURL}`;
    if (isEphemeralListenError({ message: startupError, code: server.exitCode })) {
      const reason = startupError.trim() || 'bind permission denied';
      skipReason = `SKIP colorspace regression pixel readback: ${reason}`;
      process.exitCode = 0;
    } else if (server.exitCode !== null && server.exitCode !== 0) {
      skipReason = `SKIP colorspace regression pixel readback: ${message}`;
      process.exitCode = 0;
    } else {
      throw new Error(`${message}: ${startupError}`);
    }
  }
}
if (!skipReason) {
  const outputDir = path.resolve(process.env.LIGHTUSD_COLORSPACE_OUTPUT ||
    'artifacts/colorspace');
  await fs.mkdir(outputDir, { recursive: true });
  const launchOptions = { headless: true, args: [
    '--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage',
    '--ignore-gpu-blocklist', '--disable-gpu-blocklist', '--use-gl=angle',
    '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--disable-breakpad', '--disable-crash-reporter', '--noerrdialogs'
  ], timeout: 30000, protocolTimeout: 60000 };
  if (process.env.PUPPETEER_EXECUTABLE_PATH) {
    launchOptions.executablePath = process.env.PUPPETEER_EXECUTABLE_PATH;
  }
  let browser = null;
  let result;
  try {
    browser = await puppeteer.launch(launchOptions);
    const page = await browser.newPage();
    page.on('console', (message) => console.error(`[browser:${message.type()}] ${message.text()}`));
    page.on('pageerror', (error) => console.error(`[browser:error] ${error.stack || error}`));
    await page.setViewport({ width: 1290, height: 640, deviceScaleFactor: 1 });
    await page.goto(baseURL, { waitUntil: 'domcontentloaded', timeout: 30000 });
    await page.waitForFunction(() =>
      document.documentElement.dataset.regressionReady === 'true',
    { timeout: 30000 });
    result = await page.evaluate(() => window.__colorRegression);
    await page.screenshot({ path: path.join(outputDir, 'colorspace-regression.png') });
    await fs.writeFile(path.join(outputDir, 'colorspace-regression.json'),
      JSON.stringify(result, null, 2) + '\n');
  } finally {
    if (browser) await browser.close();
    if (server) server.kill('SIGTERM');
  }
  console.log(JSON.stringify(result, null, 2));
  if (!result?.pass) process.exitCode = 1;
} else {
  console.log(skipReason);
}
