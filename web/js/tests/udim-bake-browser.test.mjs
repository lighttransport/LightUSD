// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import puppeteer from 'puppeteer';
import {PNG} from 'pngjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const fixture = fs.readFileSync(new URL('./udim-bake.test.mjs', import.meta.url), 'utf8');
const scene = fixture.match(/const scene = `([\s\S]*?)`;/)[1];
const png = new PNG({width: 2, height: 2});
for (let i = 0; i < png.data.length; i += 4) png.data.set([255, 0, 0, 255], i);
const tile = [...PNG.sync.write(png)];
const server = http.createServer((req, res) => {
  const name = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  const file = path.resolve(root, '.' + name);
  if (!file.startsWith(root) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404); res.end(); return;
  }
  res.setHeader('Content-Type', file.endsWith('.wasm') ? 'application/wasm'
    : /\.(js|mjs)$/.test(file) ? 'text/javascript' : 'text/html');
  fs.createReadStream(file).pipe(res);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
  browser = await puppeteer.launch({headless: true, args: ['--no-sandbox', '--disable-dev-shm-usage']});
  const page = await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/usdzconvert.html`);
  await page.waitForSelector('#udimBake');
  assert.equal(await page.$eval('#udimMaxTiles', input => input.value), '100');
  const results = await page.evaluate(async ({scene, tile}) => {
    const {loadWasm, convertFolderToUSDZ, parseUSDZEntries} = await import('./src/usdzconvert.js');
    const native = await loadWasm(() => import('./src/lightusd/lightusd_combined.js'));
    const assets = new Map([['root.usda', new TextEncoder().encode(scene)],
      ['textures/tile.1001.png', new Uint8Array(tile)]]);
    const results = [];
    for (const mode of ['grid', 'dense']) {
      const result = await convertFolderToUSDZ(native, assets,
        {udimBake: mode, reencode: false, rootLayerFormat: 'usda'});
      const entries = parseUSDZEntries(result.usdz);
      results.push({kind: 'main', mode, root: new TextDecoder().decode(entries[0].data),
        atlases: entries.filter(e => /udim_.*\.png/.test(e.name)).length});
    }
    for (const pipeline of ['legacy', 'next-only', 'stream']) {
      for (const mode of ['grid', 'dense']) {
        const worker = new Worker('./usdzconvert.worker.js', {type: 'module'});
        try {
          const result = await new Promise((resolve, reject) => {
            const timeout = setTimeout(() => reject(new Error('Worker bake timed out')), 60000);
            worker.onerror = event => {clearTimeout(timeout); reject(new Error(event.message));};
            worker.onmessage = event => {
              if (event.data.type === 'complete') {clearTimeout(timeout); resolve(event.data);}
              if (event.data.type === 'error') {clearTimeout(timeout); reject(new Error(event.data.message || event.data.error));}
            };
            worker.postMessage({type: 'convert', files: [...assets].map(([path, bytes]) =>
              ({path, file: new File([bytes], path.split('/').pop())})),
              opts: {pipeline, flatten: pipeline !== 'next-only', udimBake: mode,
                reencode: false, rootLayerFormat: 'usda', rootPath: 'root.usda'}});
          });
          const entries = parseUSDZEntries(result.usdz);
          results.push({kind: pipeline, mode, atlases: entries.filter(e => /udim_.*\.png/.test(e.name)).length,
            tiles: result.stats.udimTiles});
        } finally {worker.terminate();}
      }
    }
    return results;
  }, {scene, tile});
  for (const result of results) {
    assert.equal(result.atlases, 1, JSON.stringify(result));
    if (result.root) {
      assert.doesNotMatch(result.root, /<UDIM>/);
      assert.match(result.root, result.mode === 'grid' ? /UsdTransform2d/ : /primvars:_udimAtlas0/);
    } else assert.equal(result.tiles, 1);
  }
  console.log(`UDIM browser controls, main-thread conversion and Workers: ${results.length} cases passed`);
} finally {
  if (browser) await browser.close();
  await new Promise(resolve => server.close(resolve));
}
