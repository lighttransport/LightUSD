// SPDX-License-Identifier: Apache-2.0
// Explicit network setup. Existing checkouts are verified, never rewritten.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const cache = path.join(root, '.cache/lightusd-verification');
const fixtures = [
  { name: 'usd-assets', url: 'https://github.com/usd-wg/assets.git', commit: '3b75c2dad6a494897557dcca0098257bcf42a8c6', folder: 'full_assets/StandardShaderBall' },
  { name: 'MaterialX', url: 'https://github.com/AcademySoftwareFoundation/MaterialX.git', commit: '7b64921ef1d42f2d57871e9d2c43dc11f041f26b', folder: 'libraries' },
];
function git(args, capture = false) { return execFileSync('git', args, { encoding: 'utf8', stdio: capture ? ['ignore', 'pipe', 'pipe'] : 'inherit', windowsHide: true }); }
fs.mkdirSync(cache, { recursive: true });
for (const fixture of fixtures) {
  const dir = path.join(cache, fixture.name);
  if (!fs.existsSync(dir)) {
    git(['clone', '--filter=blob:none', '--no-checkout', '--sparse', fixture.url, dir]);
    git(['-C', dir, 'sparse-checkout', 'set', fixture.folder]);
    git(['-C', dir, 'checkout', '--detach', fixture.commit]);
  }
  const commit = git(['-C', dir, 'rev-parse', 'HEAD'], true).trim();
  if (commit !== fixture.commit) throw new Error(`${fixture.name}: expected ${fixture.commit}, found ${commit}; existing checkout left unchanged`);
  if (git(['-C', dir, 'status', '--porcelain'], true).trim()) throw new Error(`${fixture.name}: modified checkout; left unchanged`);
  if (!fs.existsSync(path.join(dir, fixture.folder))) throw new Error(`${fixture.name}: missing sparse-checkout folder ${fixture.folder}`);
  console.log(`${fixture.name}: verified ${commit}`);
}
