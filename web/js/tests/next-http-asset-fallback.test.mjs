import assert from 'node:assert/strict';
import { flattenNextOverHttp } from '../http-asset-resolver.js';
import { parseUSDZEntries } from '../src/usdzconvert.js';

const encode = (value) => new TextEncoder().encode(value);
const requested = [];
let provided = false;

class FlattenSession {
  begin() { return { success: true }; }
  step() {
    return provided
      ? { success: true, status: 'done', data: encode('PXR-USDC'), assetPaths: [] }
      : { success: true, status: 'need-layer', key: 'assets/dep.usda' };
  }
  provideLayer(key, bytes) {
    assert.equal(key, 'assets/dep.usda');
    assert.match(new TextDecoder().decode(bytes), /^#usda 1\.0/);
    provided = true;
    return { success: true };
  }
  end() {}
  delete() {}
}

const resolver = {
  async resolveAsync(path) {
    requested.push(path);
    if (path === 'assets/dep.usda') return [path, encode('<!doctype html><html>fallback</html>'), path];
    if (path === 'dep.usda') return [path, encode('#usda 1.0\n'), path];
    throw new Error(`Unexpected asset request: ${path}`);
  },
  setAsset() {},
};

const result = await flattenNextOverHttp({
  renderer: { native: { NextFlattenSession: FlattenSession } },
  rootBytes: encode('#usda 1.0\n'),
  filename: 'root.usda',
  resolver,
});
assert.deepEqual(requested, ['assets/dep.usda', 'dep.usda']);
assert.equal(provided, true);
assert.equal(parseUSDZEntries(result.usdz)[0].name, 'root.usdc');
console.log('ok - next HTTP flatten rejects an HTML fallback and resolves a prefixed dependency');
