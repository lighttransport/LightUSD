// SPDX-License-Identifier: Apache-2.0
// Public core flatten contracts, independent of legacy modules.
import assert from 'node:assert/strict';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const {default: createCore} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_64.js' : '../src/lightusd/lightusd.js',
  import.meta.url));
const coreModule = await createCore();
const encode = text => new TextEncoder().encode(text);

const singleUSDA = encode(`#usda 1.0
def Xform "A" (
 variants = {
  string shape = "round"
 }
 prepend variantSets = "shape"
) {
 asset tex = @old.png@
 variantSet "shape" = {
  "round" {
   def Sphere "S" {
   }
  }
  "square" {
   def Cube "C" {
   }
  }
 }
}
`);
const rootUSDA = encode(`#usda 1.0\ndef Xform "Inst" (\n references = @ref.usda@</Asset>\n) {\n asset tex = @old.png@\n}\n`);
const depUSDA = encode(`#usda 1.0\ndef Xform "Asset" {\n float weight = 2\n}\n`);
function toUSDC(bytes) {
  const document = new coreModule.LayerDocument();
  try {
    assert.equal(document.load(bytes).success, true);
    const crate = document.exportUSDC();
    assert.equal(crate.success, true);
    return new Uint8Array(crate.data);
  } finally { document.delete(); }
}
const singleUSDC = toUSDC(singleUSDA);
const remap = {'old.png': 'textures/new.png'};
const variants = {shape: 'square'};
const concat = chunks => {
  const out = new Uint8Array(chunks.reduce((sum, chunk) => sum + chunk.length, 0));
  let offset = 0;
  for (const chunk of chunks) { out.set(chunk, offset); offset += chunk.length; }
  return out;
};

// Drive the public core session with optional sink output.
function flatten(root, {remapping, overrides, sink} = {}) {
  const session = new coreModule.FlattenSession();
  try {
    if (overrides) for (const [set, selection] of Object.entries(overrides))
      session.setVariantOverride(set, selection);
    assert.equal(session.begin(root, 'root.usda', true).success, true);
    if (remapping) session.setAssetPathRemap(remapping);
    const chunks = [];
    for (let i = 0; i < 8; ++i) {
      const step = session.step(sink ? chunk => { chunks.push(chunk.slice()); return true; } : null);
      assert.equal(step.success, true, step.error);
      if (step.status === 'need-layer') {
        assert.equal(step.key, 'ref.usda');
        session.provideLayer(step.key, depUSDA);
        continue;
      }
      return sink ? concat(chunks) : new Uint8Array(step.data);
    }
    throw new Error('flatten did not converge');
  } finally { session.end(); session.delete(); }
}

// Core contracts remain executable without a legacy runtime.
const layerText = bytes => {
  const document = new coreModule.LayerDocument();
  try {
    assert.equal(document.load(bytes).success, true);
    return document.exportUSDA().text;
  } finally { document.delete(); }
};
const localEdits = encode('#usda 1.0\ndef Xform "Local" {\ncustom int[] a = edit [append 1; append 2]\n}\n');
assert.match(layerText(flatten(localEdits)), /int\[\] a = \[1, 2\]/);
assert.equal(layerText(flatten(toUSDC(localEdits))), layerText(flatten(localEdits)));
assert.equal(layerText(flatten(singleUSDA)), layerText(flatten(singleUSDC)));
for (const root of [singleUSDA, singleUSDC, rootUSDA]) {
  for (const options of [{}, {remapping: remap}, {remapping: remap, overrides: variants}]) {
    const buffered = flatten(root, options);
    const streamed = flatten(root, {...options, sink: true});
    assert.equal(layerText(streamed), layerText(buffered), 'sink and buffered output agree');
    const text = layerText(buffered);
    if (options.remapping) assert.match(text, /textures\/new.png/);
    if (root === rootUSDA) assert.match(text, /weight = 2/);
  }
}
assert.notEqual(layerText(flatten(singleUSDC, {overrides: variants})),
  layerText(flatten(singleUSDC)), 'variant overrides change authored output');
console.log('ok - core flatten USDA/USDC, sparse edits, remaps, variants, dependency protocol and sinks');
