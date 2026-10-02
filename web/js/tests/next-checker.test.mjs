// Full native lusdchecker / WASM report parity and memory-only resolver contracts.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const {default: factory} = await import(wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js');
const module = await factory();
const encode = text => new TextEncoder().encode(text);
const header = '#usda 1.0\n(\n defaultPrim = "World"\n upAxis = "Y"\n metersPerUnit = 1\n)\n';
const clean = header + 'def Xform "World" {}\n';
const checker = process.env.LUSDCHECKER_PATH || path.resolve('../../build_ninja/lusdchecker');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'wasm-checker-'));
const store = new module.NextAssetStore();
let compared = 0;
function check(name, text, options = {}, flags = []) {
  const bytes = typeof text === 'string' ? encode(text) : text;
  const actual = module.checkUSD(bytes, name, options, store);
  if (fs.existsSync(checker)) {
    fs.writeFileSync(path.join(directory, name), bytes);
    const run = spawnSync(checker, ['--json', ...flags, name], {cwd: directory, encoding: 'utf8'});
    assert.ok(run.status === 0 || run.status === 1 || run.status === 2, run.stderr);
    assert.deepEqual(actual, JSON.parse(run.stdout), `${name} native/WASM mismatch`);
    compared++;
  }
  return actual;
}
function issue(report, rule) { assert.ok(report.issues.some(i => i.ruleId === rule), JSON.stringify(report)); }
// Small stored, aligned ZIPs generated in memory; no binary fixtures in Git.
function packageBytes(entries) {
  const locals = [], central = []; let offset = 0;
  for (const [filename, source] of entries) {
    const name = Buffer.from(filename), data = Buffer.from(source);
    let crc = 0xffffffff;
    for (const byte of data) { crc ^= byte; for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
    crc = (crc ^ 0xffffffff) >>> 0;
    let extra = (64 - (offset + 30 + name.length) % 64) % 64;
    if (extra > 0 && extra < 4) extra += 64;
    const local = Buffer.alloc(30 + name.length + extra);
    local.writeUInt32LE(0x04034b50); local.writeUInt16LE(20, 4);
    local.writeUInt32LE(crc, 14); local.writeUInt32LE(data.length, 18); local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(name.length, 26); local.writeUInt16LE(extra, 28); name.copy(local, 30);
    if (extra) { local.writeUInt16LE(0x1986, 30 + name.length); local.writeUInt16LE(extra - 4, 32 + name.length); }
    const row = Buffer.alloc(46 + name.length);
    row.writeUInt32LE(0x02014b50); row.writeUInt16LE(20, 4); row.writeUInt16LE(20, 6);
    row.writeUInt32LE(crc, 16); row.writeUInt32LE(data.length, 20); row.writeUInt32LE(data.length, 24);
    row.writeUInt16LE(name.length, 28); row.writeUInt32LE(offset, 42); name.copy(row, 46);
    locals.push(local, data); central.push(row); offset += local.length + data.length;
  }
  const directory = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, directory, end]);
}
try {
  assert.equal(typeof module.checkUSD, 'function');
  assert.equal(check('clean.usda', clean).valid, true);
  assert.equal(module.checkUSD(encode(clean), 'bundle.usdz[root.usda]').valid, true,
    'explicitly supplied package-member bytes must take precedence over archive lookup');
  assert.equal(check('clean.usda', clean, {profile: 'strict'}, ['--profile', 'strict']).valid, true);
  assert.equal(check('clean.usda', clean, {profile: 'aousd-core-1.0.1'}, ['--profile', 'aousd-core-1.0.1']).conformance, 'passed');
  check('fragment.usda', '#usda 1.0\nover "World" {}', {strict: true}, ['--strict']);
  check('malformed.usda', '#usda 1.0\ndef {', {profile: 'strict'}, ['--profile', 'strict']);
  check('future.usda', clean.replace('1.0\n', '1.2\n'), {profile: 'aousd-core-1.0.1'}, ['--profile', 'aousd-core-1.0.1']);
  const unknown = header + 'def VendorThing "World" {}';
  const missing = check('unknown.usda', unknown, {profile: 'strict'}, ['--profile', 'strict']);
  assert.equal(missing.complete, false); issue(missing, 'checker.coverage.schema');
  const schema = {formatVersion: 1, schemas: [{name: 'VendorThing', kind: 'concrete', inherits: 'Xform', properties: []}]};
  assert.equal(module.checkUSD(encode(unknown), 'custom.usda', {profile: 'strict', schemaDefinitions: [schema]}).valid, true);
  assert.equal(module.checkUSD(encode(unknown), 'custom.usda', {profile: 'strict'}).complete, false, 'manifests must not leak between runs');
  const shader = header + 'def Xform "World" {\n def Material "M" {\n def Shader "S" {\n uniform token info:id = "ND_vendor"\n float inputs:value = 1\n }\n }\n}';
  const shaderDefinitions = [{formatVersion: 1, shaders: [{identifier: 'ND_vendor', sourceType: 'mtlx', inputs: {value: 'float'}, outputs: {out: 'float'}}]}];
  issue(module.checkUSD(encode(shader), 'shader.usda', {profile: 'strict'}), 'checker.coverage.shader');
  assert.equal(module.checkUSD(encode(shader), 'shader.usda', {profile: 'strict', shaderDefinitions}).valid, true);
  const animation = header + `def Xform "World" {
 def Mesh "M" {
  point3f[] points.timeSamples = {0: [(0,0,0),(1,0,0),(0,1,0)], 1: [(0,0,0),(1,0,0),(0,1,0)]}
  int[] faceVertexCounts.timeSamples = {0: [3], 1: [3]}
  int[] faceVertexIndices.timeSamples = {0: [0,1,2], 1: [0,1,99]}
 }
}`;
  const animated = check('animated.usda', animation, {profile: 'strict'}, ['--profile', 'strict']);
  assert.ok(animated.issues.some(i => i.ruleId === 'geom.mesh.topology.index' && i.time === 1));
  const baselined = module.checkUSD(encode(animation), 'animated.usda', {profile: 'strict', baseline: animated});
  assert.equal(baselined.valid, false); assert.equal(baselined.gatePassed, true);
  assert.equal(module.checkUSD(encode(unknown), 'unknown.usda', {profile: 'strict', baseline: missing}).gatePassed, false);

  issue(check('limited.usda', animation, {profile: 'strict', maxSamples: 1}, ['--profile', 'strict', '--max-samples', '1']), 'checker.coverage.samples');
  const dependency = header + `def Xform "World" (
 prepend variantSets = "shape"
 variants = { string shape = "good" }
) {
 variantSet "shape" = {
  "good" { def Sphere "S" { double radius = 1 } }
  "bad" { def Sphere "S" { float radius = 1 } }
 }
}`;
  fs.mkdirSync(path.join(directory, 'parts'));
  fs.writeFileSync(path.join(directory, 'parts/variant.usda'), dependency);
  store.registerMemoryAsset('parts/variant.usda', encode(dependency));
  const reference = header + 'def Xform "World" (\n references = @./parts/variant.usda@</World>\n) {}';
  const variants = check('reference.usda', reference, {profile: 'strict'}, ['--profile', 'strict']);
  assert.ok(variants.issues.some(i => i.variants?.includes('shape}=bad')));
  const packageRoot = header + 'def Xform "World" (\n references = @parts/variant.usda@</World>\n) {}';
  const packaged = packageBytes([['root.usda', encode(packageRoot)], ['parts/variant.usda', encode(dependency)]]);
  const packageReport = check('variants.usdz', packaged, {profile: 'strict'}, ['--profile', 'strict']);
  assert.ok(packageReport.issues.some(i => i.variants?.includes('shape}=bad')));
  const nestedPackage = packageBytes([['root.usda', encode(clean)], ['inner.usdz', packaged]]);
  check('nested.usdz', nestedPackage, {profile: 'strict'}, ['--profile', 'strict']);
  const absent = module.checkUSD(encode(reference), 'reference.usda', {profile: 'strict'});
  assert.equal(absent.complete, false); issue(absent, 'checker.coverage.dependencies');
  assert.equal(module.checkUSD(encode(clean), 'bad.usda', {profile: 'strict', groups: ['core']}).executionSuccessful, false);
  for (const options of [{unknown: true}, {maxSamples: 0}, {maxMemoryMB: -1}, {strict: 'true'}, {groups: []}, {profile: 'wat'}, {schemaDefinitions: ['not JSON']}])
    assert.equal(module.checkUSD(encode(clean), 'bad.usda', options).executionSuccessful, false);
  assert.throws(() => module.checkUSD([], 'bad.usda'), TypeError);
  assert.throws(() => module.checkUSD(encode(clean), 'bad.usda', '{}'), TypeError);
  assert.throws(() => module.checkUSD(encode(clean), 'bad.usda', {}, {}), TypeError);
  for (const name of ['', '-', '--help', 'a\0b']) assert.equal(module.checkUSD(encode(clean), name).executionSuccessful, false);
  const sarif = module.checkUSD(encode(animation), 'animated.usda', {profile: 'strict', format: 'sarif'});
  assert.equal(sarif.version, '2.1.0'); assert.ok(sarif.runs[0].results.length > 0);
  for (const [name, relative] of [['cube.usdc', '../../../tests/usdc/cube-000.usdc'], ['cube.usdz', '../../../models/cube.usdz']]) {
    const bytes = new Uint8Array(fs.readFileSync(new URL(relative, import.meta.url)));
    check(name, bytes);
    check(name, bytes, {profile: 'strict'}, ['--profile', 'strict']);
  }
  const heap = module._lightusd_next_alloc(encode(clean).length);
  try {
    module.HEAPU8.set(encode(clean), Number(heap));
    assert.equal(module.checkUSD(module.HEAPU8.subarray(Number(heap), Number(heap) + encode(clean).length), 'heap.usda').valid, true);
  } finally { module._lightusd_next_free(heap); }
} finally { store.delete(); fs.rmSync(directory, {recursive: true, force: true}); }
assert.throws(() => module.checkUSD(encode(clean), 'dead.usda', {}, store), TypeError);
console.log(`ok - WASM lusdchecker profiles, coverage, manifests, assets, samples, variants, containers, SARIF; ${compared} native reports matched (${wasm64 ? 'wasm64' : 'wasm32'})`);
