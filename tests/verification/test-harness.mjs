#!/usr/bin/env node

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');

const manifest = JSON.parse(fs.readFileSync(path.join(here, 'manifest.json'), 'utf8'));
assert.equal(manifest.schema, 1);
assert.equal(manifest.repositories.menagerie.ref.length, 40);
assert.equal(manifest.repositories.usd_assets.ref.length, 40);

const forbiddenFiles = [
  'web/js/vite.config.ts',
  'web/js/phys-sim.js',
  'web/js/cli/phys-sim.js',
  'tests/core/run-core-pxr-flatten-diff.mjs',
  'tests/parse-asset-corpus.mjs',
];
for (const relative of forbiddenFiles) {
  const text = fs.readFileSync(path.join(root, relative), 'utf8');
  assert.doesNotMatch(text, /\/home\/syoyo|\/mnt\/nvme02\/work\/usd|\/path\/to\/mujoco/,
    `${relative} contains a machine-specific default`);
}

console.log('verification harness tests: PASS');
