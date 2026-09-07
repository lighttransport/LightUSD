// SPDX-License-Identifier: Apache-2.0
// Isolated renderer development: synthetic/GPU tests do not require a WASM build.
import { defineConfig } from 'vite';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const root = path.dirname(fileURLToPath(import.meta.url));
const cache = path.resolve(root, '../../.cache/lightusd-verification');
const roots = { '/__assets/': process.env.USD_WG_ASSETS_DIR || path.join(cache, 'usd-assets'), '/__mtlx/': process.env.MATERIALX_DIR || path.join(cache, 'MaterialX') };
function files(dir) { return fs.readdirSync(dir, { withFileTypes: true }).flatMap(d => d.isDirectory() ? files(path.join(dir, d.name)) : [path.join(dir, d.name)]); }
export default defineConfig({
  optimizeDeps: { include: ['three', 'three/addons/loaders/EXRLoader.js'] },
  root, appType: 'mpa', server: { host: '127.0.0.1', strictPort: true },
  plugins: [{ name: 'materialx-local-fixtures', configureServer(server) {
    server.middlewares.use((req, res, next) => {
      const prefix = Object.keys(roots).find(p => req.url?.startsWith(p)); if (!prefix) return next();
      try {
        const base = fs.realpathSync(roots[prefix]);
        if (prefix === '/__mtlx/' && req.url === '/__mtlx/catalog.json') {
          const entries = files(path.join(base, 'libraries')).filter(p => p.endsWith('.mtlx')).map(p => `${prefix}${path.relative(base, p).split(path.sep).join('/')}`);
          res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify({ version: '1.39.5', files: entries })); return;
        }
        const relative = decodeURIComponent(req.url.slice(prefix.length).split('?')[0]);
        const resolved = fs.realpathSync(path.resolve(base, relative));
        if (resolved !== base && !resolved.startsWith(base + path.sep)) { res.statusCode = 403; res.end('Outside fixture root'); return; }
        if (!fs.statSync(resolved).isFile()) { res.statusCode = 404; res.end('Not a file'); return; }
        res.setHeader('Content-Type', resolved.endsWith('.json') ? 'application/json' : resolved.endsWith('.mtlx') || resolved.endsWith('.usda') ? 'text/plain' : 'application/octet-stream');
        fs.createReadStream(resolved).pipe(res);
      } catch { res.statusCode = 404; res.end('Fixture missing: run the documented asset preparation'); }
    });
  } }],
});
