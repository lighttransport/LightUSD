// Tests for the usdcat CLI (cli/usdcat.js).
//   node tests/usdcat-cli.test.mjs
//
// Unit tests cover arg parsing + format resolution (no wasm). The integration
// test loads the WASM module and exercises register -> load -> flatten -> export
// across USDA / USDC using a tiny reference-composition fixture.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { parseArgs, resolveFormat, buildInput, runUsdcat } from '../cli/usdcat.js';
import { buildUSDZWithNewRoot, parseUSDZEntries, loadWasm } from '../src/usdzconvert.js';

function test(name, fn) {
  try { fn(); console.log(`ok - ${name}`); }
  catch (err) { console.error(`not ok - ${name}`); console.error(err); process.exitCode = 1; }
}
async function testAsync(name, fn) {
  try { await fn(); console.log(`ok - ${name}`); }
  catch (err) { console.error(`not ok - ${name}`); console.error(err); process.exitCode = 1; }
}
function withTempDir(fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lightusd-usdcat-'));
  try { return fn(dir); } finally { fs.rmSync(dir, { recursive: true, force: true }); }
}

// ---- Unit: parseArgs / resolveFormat -------------------------------------

test('parseArgs: defaults flatten on, format empty', () => {
  const o = parseArgs(['scene.usdz']);
  assert.equal(o.input, 'scene.usdz');
  assert.equal(o.flatten, true);
  assert.equal(o.format, '');
  assert.equal(o.output, null);
});

test('parseArgs: -o / --output-format / --no-flatten / caps', () => {
  const o = parseArgs(['in.usd', '-o', 'out.usdc', '--output-format', 'usdc',
                       '--no-flatten', '--max-usdc-mb', '4096', '--max-mem-mb', '8192']);
  assert.equal(o.output, 'out.usdc');
  assert.equal(o.format, 'usdc');
  assert.equal(o.flatten, false);
  assert.equal(o.maxUsdcMb, 4096);
  assert.equal(o.maxMemMb, 8192);
});

test('resolveFormat: explicit > extension > default usda', () => {
  assert.equal(resolveFormat({ format: 'usdz', output: 'x.usdc' }), 'usdz'); // explicit wins
  assert.equal(resolveFormat({ format: '', output: 'x.usdc' }), 'usdc');     // from extension
  assert.equal(resolveFormat({ format: '', output: 'x.usda' }), 'usda');
  assert.equal(resolveFormat({ format: '', output: 'x.usdz' }), 'usdz');
  assert.equal(resolveFormat({ format: '', output: null }), 'usda');         // default
});

// ---- Integration: flatten a reference fixture ----------------------------

const SUB_USDA = `#usda 1.0
def Xform "Geo"
{
    def Cube "Box"
    {
        double size = 3
    }
}
`;
const MAIN_USDA = `#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World"
{
    def "Inst" (
        references = @./sub.usda@</Geo>
    )
    {
    }
}
`;

await testAsync('buildInput: folder picks root + collects siblings', () => withTempDir((dir) => {
  fs.writeFileSync(path.join(dir, 'main.usda'), MAIN_USDA);
  fs.writeFileSync(path.join(dir, 'sub.usda'), SUB_USDA);
  const { map, rootName } = buildInput(dir, null);
  assert.equal(map.size, 2);
  assert.ok(map.has('sub.usda'));
  assert.equal(rootName, 'main.usda'); // shallow non-usdz root
}));

const glueUrl = new URL(process.env.LIGHTUSD_WASM64 === '1' ? '../src/lightusd/lightusd_64.js' : '../src/lightusd/lightusd.js', import.meta.url).href;
const native = await loadWasm(() => import(glueUrl));

await testAsync('runUsdcat: flatten inlines referenced prim (USDA)', () => withTempDir((dir) => {
  fs.writeFileSync(path.join(dir, 'main.usda'), MAIN_USDA);
  fs.writeFileSync(path.join(dir, 'sub.usda'), SUB_USDA);
  const { map, rootName } = buildInput(dir, null);
  const res = runUsdcat(native, { map, rootName, flatten: true, format: 'usda' });
  assert.equal(res.format, 'usda');
  assert.ok(res.text.includes('Box'), 'flattened USDA should inline the referenced Cube');
  assert.ok(res.text.includes('size = 3'), 'flattened USDA should carry the referenced attribute');
}));

await testAsync('runUsdcat: --no-flatten keeps the arc (no inlined child)', () => withTempDir((dir) => {
  fs.writeFileSync(path.join(dir, 'main.usda'), MAIN_USDA);
  fs.writeFileSync(path.join(dir, 'sub.usda'), SUB_USDA);
  const { map, rootName } = buildInput(dir, null);
  const res = runUsdcat(native, { map, rootName, flatten: false, format: 'usda' });
  assert.ok(!res.text.includes('size = 3'), 'un-flattened USDA must not inline the referenced attribute');
}));

await testAsync('runUsdcat: USDC export reloads cleanly', () => withTempDir((dir) => {
  fs.writeFileSync(path.join(dir, 'main.usda'), MAIN_USDA);
  fs.writeFileSync(path.join(dir, 'sub.usda'), SUB_USDA);
  const { map, rootName } = buildInput(dir, null);
  const res = runUsdcat(native, { map, rootName, flatten: true, format: 'usdc' });
  assert.equal(res.format, 'usdc');
  assert.ok(res.bytes.length > 8, 'usdc bytes present');
  // PXR-USDC magic header.
  assert.equal(Buffer.from(res.bytes.slice(0, 8)).toString('latin1'), 'PXR-USDC');
  // Reload the produced crate to confirm it parses.
  const usd = new native.NextUSDZConverterNative();
  try {
    assert.ok(usd.loadFromBinary(res.bytes, 'flat.usdc'), 'produced USDC should reload: ' + usd.error());
  } finally { usd.delete(); }
}));

await testAsync('runUsdcat: anchored textures and first-entry nested USDZ root', () => withTempDir((dir) => {
  const encoder = new TextEncoder();
  const root = encoder.encode('#usda 1.0\ndef Xform "World" (references=@../deps/sub.usda@</Geo>) {}\n');
  const sub = encoder.encode('#usda 1.0\ndef Xform "Geo" { asset texture=@same.png@ }\n');
  const packed = buildUSDZWithNewRoot('nested/root.usda', root, [
    {name:'deps/sub.usda',data:sub}, {name:'deps/same.png',data:new Uint8Array([1,2,3])},
    {name:'same.png',data:new Uint8Array([9])}]);
  const input = path.join(dir, 'scene.usdz'); fs.writeFileSync(input, packed);
  const prepared = buildInput(input);
  assert.equal(prepared.rootName, 'nested/root.usda');
  const result = runUsdcat(native, {...prepared,format:'usdz'});
  const entries = parseUSDZEntries(result.bytes);
  const document = new native.LayerDocument();
  try {
    assert.equal(document.load(entries[0].data).success,true);
    assert.match(document.exportUSDA().text, /@deps\/same.png@/);
    assert.deepEqual([...entries.find(entry=>entry.name==='deps/same.png').data],[1,2,3]);
  } finally {document.delete();}
  assert.throws(()=>runUsdcat(native,{map:new Map([['root.usda',root]]),rootName:'root.usda'}),/Missing USD dependency/);
}));

console.log('usdcat-cli tests done');
