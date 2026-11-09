import assert from 'node:assert/strict';
import { AssetBudget, chooseRootFile, formatBytes, normalizeAssetKey } from '../src/online-usd-viewer-core.js';
import { githubContentsRequest } from '../src/online-usd-viewer-core.js';

assert.equal(normalizeAssetKey('./folder\\scene.usda'), 'folder/scene.usda');
assert.equal(formatBytes(200 * 1024 * 1024), '200.0 MiB');
const budget = new AssetBudget(10);
assert.equal(budget.claim('a', 6), true);
assert.equal(budget.claim('a', 6), false);
assert.throws(() => budget.claim('b', 5), /budget exceeded/);
assert.equal(chooseRootFile([{ name: 'folder/other.usda' }, { name: 'root.usda' }]).name, 'root.usda');
assert.equal(githubContentsRequest('https://raw.githubusercontent.com/usd-wg/assets/main/test_assets/a.usda').url,
  'https://api.github.com/repos/usd-wg/assets/contents/test_assets/a.usda?ref=main');
assert.equal(githubContentsRequest('https://github.com/usd-wg/assets/blob/main/test_assets/a.usda').headers.Accept,
  'application/vnd.github.raw+json');
assert.equal(githubContentsRequest('https://example.com/a.usda'), null);
console.log('online USD viewer helpers: ok');
