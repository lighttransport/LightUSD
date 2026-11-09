// Exercise the shared validation UI from both web applications.
import assert from 'node:assert/strict';
import path from 'node:path';
import fs from 'node:fs';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {createServer, build, preview} from 'vite';
import puppeteer from 'puppeteer';
const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'checker-browser-'));
const header = '#usda 1.0\n(\n defaultPrim = "World"\n upAxis = "Y"\n metersPerUnit = 1\n)\n';
fs.writeFileSync(path.join(directory, 'unknown.usda'), header + 'def VendorThing "World" {}');
fs.writeFileSync(path.join(directory, 'reference.usda'), header + 'def Xform "World" (\n references = @dep.usda@</World>\n) {}');
fs.writeFileSync(path.join(directory, 'dep.usda'), header + 'def Xform "World" {}');
const browser = await puppeteer.launch({headless: true,
  executablePath: process.env.PUPPETEER_EXECUTABLE_PATH ||
    (fs.existsSync('/usr/bin/google-chrome-stable') ? '/usr/bin/google-chrome-stable' : undefined),
  args: ['--no-sandbox', '--disable-dev-shm-usage']});
try {
  for (const app of ['js', 'demo']) {
    const alias = process.env.LIGHTUSD_NEXT_MODULE ? [{
      find: /^\.\/src\/lightusd\/lightusd_next\.(js|wasm)(\?url)?$/,
      replacement: path.resolve(process.env.LIGHTUSD_NEXT_MODULE).replace(/\.js$/, '') + '.$1$2'
    }] : [];
    const config = {configFile: false, root: path.join(web, app), base: './',
      resolve: {alias: [...alias, {find: 'lightusd', replacement: path.join(web, 'js/src/lightusd')}]},
      optimizeDeps: {noDiscovery: true, include: []}, worker: {format: 'es'},
      server: {host: '127.0.0.1', port: 0, hmr: false, fs: {allow: [web]}}};
    let server;
    if (process.argv.includes('--production')) {
      config.build = {outDir: path.join(directory, app), emptyOutDir: true,
        rollupOptions: {input: path.join(web, app, 'validation.html')}};
      await build(config);
      server = await preview({...config, preview: {host: '127.0.0.1', port: 0}});
    } else { server = await createServer(config); await server.listen(); }
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    try {
      await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/validation.html`);
      await page.waitForFunction(() => document.querySelector('#status')?.textContent === 'Ready', {timeout: 60000});
      await page.select('#profile', 'strict');
      assert.equal(await page.$eval('#ruleGroups', el => el.disabled), true);
      async function validate() {
        await page.click('#validate');
        await page.waitForFunction(() => document.querySelector('#status').textContent === 'Validation complete', {timeout: 30000});
        return JSON.parse(await page.$eval('#report', el => el.textContent));
      }
      assert.equal((await validate()).valid, true);
      await page.evaluate(() => { document.getElementById('validate').click(); document.getElementById('cancel').click(); });
      await page.waitForFunction(() => document.querySelector('#status').textContent === 'Ready');
      assert.equal((await validate()).valid, true, 'validation recovers after worker cancellation');
      await (await page.$('#fileInput')).uploadFile(path.join(directory, 'unknown.usda'));
      await page.waitForFunction(() => document.querySelector('#fileInfo').textContent.startsWith('unknown.usda'));
      assert.equal((await validate()).complete, false);
      assert.equal(await page.$eval('#summaryStatus', el => el.textContent), 'Incomplete');
      assert.match(await page.$eval('#issueRows', el => el.textContent), /checker.coverage.schema/);
      await page.select('#profile', 'aousd-core-1.0.1');
      assert.equal((await validate()).conformance, 'passed');
      await page.select('#profile', 'strict');
      await (await page.$('#fileInput')).uploadFile(path.join(directory, 'reference.usda'));
      await page.waitForFunction(() => document.querySelector('#fileInfo').textContent.startsWith('reference.usda'));
      assert.equal((await validate()).complete, false);
      await (await page.$('#dependencies')).uploadFile(path.join(directory, 'dep.usda'));
      await page.waitForFunction(() => document.querySelector('#assetInfo').textContent === '1 supplied files');
      assert.equal((await validate()).valid, true);
      await page.setViewport({width: 390, height: 844});
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), true, 'mobile page must fit');
      assert.deepEqual(errors, []);
      console.log(`ok - ${app}/validation.html profiles, incomplete coverage, dependencies, responsive layout`);
    } catch (error) {
      throw new Error(`${app}: ${error.message}\n${await page.$eval('body', el => el.innerText)}\n${errors.join('\n')}`);
    } finally { await page.close();
      if (server.close) await server.close();
      else await new Promise(resolve => server.httpServer.close(resolve)); }
  }
} finally { await browser.close(); fs.rmSync(directory, {recursive: true, force: true}); }
