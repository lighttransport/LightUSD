// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import puppeteer from 'puppeteer';
const root = fileURLToPath(new URL('../', import.meta.url));
const server = http.createServer((req,res) => {
  const name = decodeURIComponent(new URL(req.url,'http://localhost').pathname);
  if (name === '/') {res.setHeader('Content-Type','text/html'); res.end('<!doctype html><title>Features</title>'); return;}
  const file = path.resolve(root,'.'+name);
  if (!file.startsWith(root) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404); res.end(); return;
  }
  res.setHeader('Content-Type',file.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
  fs.createReadStream(file).pipe(res);
});
await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
let browser;
try {
  browser = await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage']});
  const page = await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  for (const bits of [32,64]) {
    const results = await page.evaluate(async bits => {
      const {checkFeatures} = await import('/tests/next-features-contract.mjs');
      const {default: factory} = await import(`/src/lightusd/lightusd_next${bits === 64 ? '_64' : ''}.js`);
      const main = await checkFeatures(factory);
      const worker = new Worker('/tests/next-features-worker.mjs',{type:'module'});
      try {
        const background = await new Promise((resolve,reject) => {
          const timeout = setTimeout(() => reject(Error('feature Worker timed out')),60000);
          worker.onerror = event => {clearTimeout(timeout); reject(Error(event.message));};
          worker.onmessage = ({data}) => {
            clearTimeout(timeout); data.error ? reject(Error(data.error)) : resolve(data.result);
          };
          worker.postMessage(bits);
        });
        return [main,background];
      } finally {worker.terminate();}
    },bits);
    assert.deepEqual(results,[{pointerBits:bits,heaps:5},{pointerBits:bits,heaps:5}]);
  }
  console.log('Browser main and module Worker feature contracts: wasm32 + wasm64 PASS');
} finally {
  if (browser) await browser.close();
  await new Promise(resolve => server.close(resolve));
}
