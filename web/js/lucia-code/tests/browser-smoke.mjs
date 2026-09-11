#!/usr/bin/env node
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import net from 'node:net';
import puppeteer from 'puppeteer';

const configuredPort = Number(process.env.LUCIA_SMOKE_PORT || 0);
if (!Number.isInteger(configuredPort) || configuredPort < 0 || configuredPort > 65535) throw new Error('LUCIA_SMOKE_PORT must be an integer between 0 and 65535.');
const startupTimeoutMs = Number(process.env.LUCIA_SMOKE_START_TIMEOUT_MS || 120000);
if (!Number.isInteger(startupTimeoutMs) || startupTimeoutMs < 1000 || startupTimeoutMs > 600000) throw new Error('LUCIA_SMOKE_START_TIMEOUT_MS must be an integer between 1000 and 600000.');
const reservePort = () => new Promise((resolve, reject) => {
  const probe = net.createServer();
  probe.once('error', reject);
  probe.listen(0, '127.0.0.1', () => { const port = probe.address().port; probe.close((error) => error ? reject(error) : resolve(port)); });
});
const port = configuredPort || await reservePort();
const server = spawn(process.execPath, ['./node_modules/vite/bin/vite.js', '--host', '127.0.0.1', '--strictPort', '--port', String(port)], { cwd: new URL('../..', import.meta.url), env: { ...process.env, LIGHTUSD_SKIP_WASM_PREPARE: '1' }, stdio: ['ignore', 'pipe', 'pipe'] });
let serverStderr = '';
let serverStdout = '';
let serverUrl = '';
const serverDiagnostics = () => [serverStdout, serverStderr].filter(Boolean).join('\n').trim();
const waitForServer = new Promise((resolve, reject) => { const timer = setTimeout(() => reject(new Error(`Vite did not start on 127.0.0.1:${port}${serverDiagnostics() ? `: ${serverDiagnostics()}` : ''}`)), startupTimeoutMs); const consumeOutput = (stream, assign) => stream.on('data', (chunk) => { const text = String(chunk); if (assign === 'stdout') serverStdout += text; else serverStderr += text; const match = text.match(/Local:\s+(https?:\/\/[^\s]+)/); if (match) { serverUrl = match[1].replace(/\/$/, ''); clearTimeout(timer); resolve(serverUrl); } }); server.once('error', (error) => { clearTimeout(timer); reject(new Error(`Could not start Vite on 127.0.0.1:${port}: ${error.message}${serverDiagnostics() ? `\n${serverDiagnostics()}` : ''}`)); }); consumeOutput(server.stdout, 'stdout'); consumeOutput(server.stderr, 'stderr'); server.once('close', (code) => { clearTimeout(timer); reject(new Error(`Vite exited ${code} on 127.0.0.1:${port}${serverDiagnostics() ? `\n${serverDiagnostics()}` : ''}`)); }); });
try {
  await waitForServer;
  const browser = await puppeteer.launch({
    headless: true,
    executablePath: process.env.PUPPETEER_EXECUTABLE_PATH || '/usr/bin/google-chrome-stable',
    args: ['--no-sandbox', '--disable-setuid-sandbox', '--disable-dev-shm-usage',
      '--ignore-gpu-blocklist', '--use-gl=angle', '--use-angle=swiftshader',
      '--enable-unsafe-swiftshader'],
  });
  try {
    const page = await browser.newPage(); const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    page.on('console', (entry) => { if (entry.type() === 'error') errors.push(entry.text()); });
    page.on('requestfailed', (request) => errors.push(`${request.url()}: ${request.failure()?.errorText}`));
    await page.goto(`${serverUrl}/lucia-code/`, { waitUntil: 'networkidle0', timeout: 90000 });
    try { await page.waitForSelector('#scene-tree .tree-row', { timeout: 30000 }); }
    catch (error) { throw new Error(`${error.message}\nPage: ${await page.$eval('body', (el) => el.innerText)}\nConsole: ${errors.join('\n')}`); }
    assert.equal(await page.$eval('.brand strong', (el) => el.textContent), 'Lucia Code');
    assert.equal(errors.length, 0, errors.join('\n'));
    await page.click('#health-button');
    await page.waitForSelector('#inspector .score-cards', { timeout: 30000 });
    assert.equal(await page.$('#merge-material-definitions') !== null, true);
    assert.equal(await page.$eval('#selection-label', (el) => el.textContent), 'Asset Health Report');
    assert.equal(await page.$eval('#inspector', (el) => el.textContent.includes('Semantic suggestions')), true);
    assert.equal(await page.$eval('#inspector', (el) => el.textContent.includes('Suggested repair plan')), true);
    assert.equal(await page.$eval('#inspector', (el) => el.textContent.includes('Transform inspection')), true);
    assert.equal(await page.$$eval('#inspector h3', (headings) => headings.filter((heading) => heading.textContent.trim() === 'Topology inspection').length), 1, 'Health report must render one topology inspection section.');
    assert.equal(await page.evaluate(() => Object.keys(localStorage).filter((key) => key.startsWith('lucia:asset-report:') && key !== 'lucia:asset-report:index').length), 1, 'Health report should persist one cache entry.');
    assert.equal(errors.length, 0, errors.join('\n'));
    await page.click('#usd-doctor-button');
    await page.waitForSelector('#usd-manifest-button', { timeout: 30000 });
    assert.equal(await page.$('#usd-manifest-load-button') !== null, true);
    assert.equal(await page.$eval('#selection-label', (el) => el.textContent), 'USD Doctor');
    await page.reload({ waitUntil: 'networkidle0', timeout: 90000 });
    await page.waitForSelector('#scene-tree .tree-row', { timeout: 30000 });
    await page.click('#health-button');
    await page.waitForSelector('#inspector .score-cards', { timeout: 30000 });
    assert.equal(await page.$eval('#selection-label', (el) => el.textContent), 'Asset Health Report');
    await page.setViewport({ width: 390, height: 844 });
    await new Promise((resolve) => setTimeout(resolve, 100));
    const responsive = await page.evaluate(() => { const right = document.querySelector('.right-panel')?.getBoundingClientRect(); return { documentOverflow: document.documentElement.scrollWidth > window.innerWidth + 1 || document.documentElement.scrollHeight > window.innerHeight + 1, inspectorFits: !right || right.width <= window.innerWidth + 1 }; });
    assert.equal(responsive.documentOverflow, false, 'Lucia narrow layout must not overflow the viewport.');
    assert.equal(responsive.inspectorFits, true, 'Lucia inspector must remain within the narrow viewport.');
    await page.setViewport({ width: 320, height: 568 });
    await new Promise((resolve) => setTimeout(resolve, 100));
    const handset = await page.evaluate(() => { const panel = document.querySelector('.right-panel')?.getBoundingClientRect(), report = document.querySelector('#inspector'); return { documentOverflow: document.documentElement.scrollWidth > window.innerWidth + 1, panelFits: !panel || panel.width <= window.innerWidth + 1, reportScrollable: !report || report.scrollHeight > report.clientHeight, viewport: { width: window.innerWidth, height: window.innerHeight, scrollWidth: document.documentElement.scrollWidth, scrollHeight: document.documentElement.scrollHeight } }; });
    assert.equal(handset.documentOverflow, false, `Lucia handset layout must not overflow horizontally: ${JSON.stringify(handset)}`);
    assert.equal(handset.panelFits, true, 'Lucia inspector must fit a 320px handset viewport.');
    assert.equal(handset.reportScrollable, true, 'Lucia report must retain an independent scroll region on handset layouts.');
    await page.setViewport({ width: 390, height: 844 });
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(await page.$eval('#panel-inspector', (el) => el.getAttribute('aria-label')), 'Toggle inspector');
    await page.click('#panel-inspector');
    assert.equal(await page.$eval('#panel-inspector', (el) => el.getAttribute('aria-expanded')), 'false');
    assert.equal(await page.$eval('#right-panel', (el) => el.classList.contains('collapsed')), true);
    await page.click('#panel-inspector');
    assert.equal(await page.$eval('#panel-inspector', (el) => el.getAttribute('aria-expanded')), 'true');
    assert.equal(await page.$eval('#panel-tree', (el) => el.getAttribute('aria-label')), 'Toggle scene tree');
    await page.click('#panel-tree');
    assert.equal(await page.$eval('#panel-tree', (el) => el.getAttribute('aria-expanded')), 'false');
    assert.equal(await page.$eval('#left-panel', (el) => el.classList.contains('collapsed')), true);
    await page.click('#panel-tree');
    assert.equal(await page.$eval('#panel-tree', (el) => el.getAttribute('aria-expanded')), 'true');
    const meshPath = await page.$$eval('#scene-tree .tree-row', (rows) => rows.find((row) => row.dataset.path === '/World/Hero')?.dataset.path || rows.find((row) => ['Mesh', 'Sphere', 'Cylinder'].includes(row.querySelector('.badge')?.textContent))?.dataset.path);
    assert.ok(meshPath, `Lucia smoke fixture must expose a mesh row for preview controls: ${await page.$$eval('#scene-tree .tree-row', (rows) => rows.map((row) => `${row.dataset.path}:${row.querySelector('.badge')?.textContent}`).join(', '))}`);
    await page.evaluate((path) => [...document.querySelectorAll('#scene-tree .tree-row')].find((row) => row.dataset.path === path)?.click(), meshPath);
    const selectedAfterRow = await page.$eval('.tree-row.selected', (row) => row.dataset.path);
    assert.equal(selectedAfterRow, meshPath, 'Lucia must retain the selected mesh after scene-tree activation.');
    await page.evaluate(() => document.querySelector('[data-tab="operations"]')?.click());
    assert.equal(await page.$eval('.tree-row.selected', (row) => row.dataset.path), meshPath, 'Opening Operations must not change the selected mesh.');
    await page.waitForSelector('#wireframe-button', { timeout: 30000 });
    assert.equal(await page.$eval('[data-bake-normal-control]', (el) => el.hidden), true, 'Normal bake controls should be hidden for the default base-color channel.');
    assert.equal(await page.$eval('[data-bake-occlusion-control]', (el) => el.hidden), true, 'Ray radius should be hidden for non-occlusion channels.');
    await page.select('#bake-channel', 'normal');
    assert.equal(await page.$eval('[data-bake-normal-control]', (el) => el.hidden), false, 'Normal bake controls should appear for normal baking.');
    await page.select('#bake-channel', 'occlusion');
    assert.equal(await page.$eval('[data-bake-normal-control]', (el) => el.hidden), true, 'Normal bake controls should hide for occlusion baking.');
    assert.equal(await page.$eval('[data-bake-occlusion-control]', (el) => el.hidden), false, 'Ray radius should appear for occlusion baking.');
    await page.select('#bake-channel', 'baseColor');
    assert.equal(await page.$('#material-parameterization-preview') !== null, true, 'Operations must expose material parameterization preview.');
    assert.deepEqual(await page.$$eval('#material-parameterization-mode option', (options) => options.map((option) => option.value)), ['auto', 'primvar', 'variant']);
    await page.select('#material-parameterization-mode', 'variant');
    assert.equal(await page.$eval('#material-parameterization-preview', (button) => button.onclick !== null), true, 'Material parameterization preview handler must survive inspector rerenders.');
    await page.$eval('#material-parameterization-preview', (button) => button.click());
    await page.waitForFunction(() => { const status = document.querySelector('#material-parameterization-status'); return Boolean(status && status.textContent !== 'No material parameterization preview yet.'); }, { timeout: 30000 });
    assert.equal(await page.$eval('#material-parameterization-status', (el) => /candidate|unsupported|material/i.test(el.textContent)), true, 'Material parameterization preview should report a result.');
    assert.equal(await page.$('#retopo-lock-uv-seams') !== null, true, 'Operations must expose UV-seam locking.');
    assert.equal(await page.$('#retopo-sharp-chains') !== null, true, 'Retopology must expose crease-chain input.');
    assert.equal(await page.$('#normal-sharp-chains') !== null, true, 'Normal recompute must expose crease-chain input.');
    await page.$eval('#retopo-lock-uv-seams', (el) => el.click());
    assert.equal(await page.$eval('#retopo-lock-uv-seams', (el) => el.checked), false);
    await page.evaluate(() => document.querySelector('[data-tab="transform"]')?.click());
    await page.evaluate(() => document.querySelector('[data-tab="operations"]')?.click());
    assert.equal(await page.$eval('#retopo-lock-uv-seams', (el) => el.checked), false, 'UV-seam policy must survive inspector rerenders.');
    await page.evaluate(() => document.querySelector('[data-tab="textures"]')?.click());
    if (await page.$('#pack-output-channels')) {
      await page.select('#pack-output-channels', '2');
      assert.equal(await page.$eval('#pack-b', (el) => el.disabled), true, 'Packed source slots beyond the output count should be disabled.');
      assert.equal(await page.$eval('#pack-b-channel', (el) => el.disabled), true, 'Packed source channel selectors beyond the output count should be disabled.');
      await page.select('#pack-output-channels', '4');
    }
    await page.evaluate(() => document.querySelector('[data-tab="operations"]')?.click());
    await page.evaluate(() => document.querySelector('#wireframe-button')?.click());
    assert.equal(await page.$eval('#wireframe-button', (el) => el.getAttribute('aria-pressed')), 'true');
    await page.evaluate(() => document.querySelector('#wireframe-button')?.click());
    assert.equal(await page.$eval('#wireframe-button', (el) => el.getAttribute('aria-pressed')), 'false');
    await page.evaluate((path) => [...document.querySelectorAll('#scene-tree .tree-row')].find((row) => row.dataset.path === path)?.click(), meshPath);
    assert.equal(await page.$eval('.tree-row.selected', (row) => row.dataset.path), meshPath, 'Material editing must start from the selected mesh.');
    await page.evaluate(() => document.querySelector('[data-tab="material"]')?.click());
    await page.waitForSelector('#material-translate', { timeout: 30000 });
    await page.waitForSelector('#material-apply', { timeout: 30000 });
    await page.$eval('#material-color', (input) => { input.value = '#ff2878'; input.dispatchEvent(new Event('input', { bubbles: true })); input.dispatchEvent(new Event('change', { bubbles: true })); });
    await page.evaluate(() => document.querySelector('#material-apply')?.click());
    await page.waitForSelector('#viewport-overlay[hidden]', { timeout: 30000 });
    await new Promise((resolve) => setTimeout(resolve, 250));
    const materialState = await page.evaluate(() => ({ hidden: document.querySelector('#comparison-material')?.hidden, selection: document.querySelector('#selection-label')?.textContent, color: document.querySelector('#material-color')?.value }));
    assert.equal(materialState.hidden, false, `Display-color mutation did not expose material comparison: ${JSON.stringify(materialState)}`);
    assert.equal(await page.$eval('#comparison-material', (el) => el.textContent.includes('material difference')), true);
    await page.evaluate(() => document.querySelector('[data-bottom="source"]')?.click());
    assert.match(await page.$eval('#activity-list', (el) => el.textContent), /interpolation = "constant"/);
    await page.evaluate(() => document.querySelector('[data-bottom="changes"]')?.click());
    await page.click('#frame-button');
    await page.type('#chat-input', 'validate');
    await page.click('#chat-form button');
    await page.waitForFunction(() => [...document.querySelectorAll('.message')].some((el) => /Valid|issues/i.test(el.textContent)), { timeout: 30000 });
    assert.equal(errors.length, 0, errors.join('\n'));
  } finally { await browser.close(); }
} finally { server.kill('SIGTERM'); }
