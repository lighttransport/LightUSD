// USDA dependency-layer composition through the wasm next flatten paths.
//
// Covers Phase-1 "USDA in next-core composition" wiring:
//  - next-only module: NextFlattenSession need-layer protocol with a USDA root
//    referencing a USDA dependency layer (and a USDC dependency for mixing).
//  - legacy module: nextFlattenAsync* session accepting USDA dependency bytes
//    (previously hard-rejected with "not a USDC crate").
//
// Runs on wasm32 by default; set LIGHTUSD_WASM64=1 for the 64-bit glue.

import assert from 'node:assert/strict';

import { loadWasm } from '../src/usdzconvert.js';

const ROOT_USDA = `#usda 1.0
(
    defaultPrim = "Root"
    upAxis = "Y"
)

def Xform "Root" (
    references = @dep.usda@</Base>
)
{
    double localOnly = 1.0
}
`;

const DEP_USDA = `#usda 1.0

def Xform "Base"
{
    def Mesh "Geo"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}
`;

async function testAsync(name, fn) {
  try { await fn(); console.log(`ok - ${name}`); }
  catch (err) { console.error(`not ok - ${name}`); console.error(err); process.exitCode = 1; }
}

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const wasmDir = new URL('../src/lightusd/', import.meta.url);

function encode(text) {
  return new TextEncoder().encode(text);
}

function isUSDC(bytes) {
  return bytes.length >= 8 && new TextDecoder().decode(bytes.slice(0, 8)) === 'PXR-USDC';
}

// Drive a session handle ({step, provideLayer, close}) through the need-layer
// loop with an in-memory layer map, returning the final step result.
function driveSession(handle, layerMap) {
  try {
    for (let i = 0; i < 32; i++) {
      const step = handle.step();
      assert.ok(step && step.success, `flatten step failed: ${step?.error}`);
      if (step.status === 'need-layer') {
        const bytes = layerMap.get(step.key);
        assert.ok(bytes, `unexpected layer request: ${step.key}`);
        const provided = handle.provideLayer(step.key, bytes);
        assert.ok(provided && provided.success, `provideLayer failed: ${provided?.error}`);
        continue;
      }
      assert.equal(step.status, 'done', `unexpected status: ${step.status}`);
      return step;
    }
  } finally {
    handle.close();
  }
  throw new Error('need-layer loop did not converge');
}

await testAsync('next-only module composes USDA root + USDA dependency', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: (file) => new URL(file, wasmDir).href,
  });
  assert.equal(typeof native.NextFlattenSession, 'function',
    'next-only glue should expose NextFlattenSession');

  const session = new native.NextFlattenSession();
  const begin = session.begin(encode(ROOT_USDA), 'root.usda', true);
  assert.ok(begin && begin.success, `begin failed: ${begin?.error}`);

  const result = driveSession({
    step: () => session.step(null),
    provideLayer: (key, bytes) => session.provideLayer(key, bytes),
    close: () => { session.end(); session.delete(); },
  }, new Map([['dep.usda', encode(DEP_USDA)]]));

  const out = new Uint8Array(result.data);
  assert.ok(isUSDC(out), 'flatten output should be a USDC crate');
  assert.ok(result.primCount >= 2, 'composed output should contain grafted prims');

  // The flattened crate renders: the referenced mesh must appear.
  const stream = new native.RenderStream();
  try {
    const load = stream.begin(out);
    assert.ok(load && load.success, `RenderStream failed: ${load?.error || stream.error()}`);
    assert.equal(load.meshCount, 1, 'referenced USDA mesh should survive composition');
  } finally {
    stream.end();
    stream.delete();
  }
});

const VARIANT_USDA = `#usda 1.0
(
    defaultPrim = "Root"
)

def Xform "Root" (
    variants = {
        string lod = "high"
    }
    prepend variantSets = "lod"
)
{
    variantSet "lod" = {
        "high" {
            def Mesh "HighGeo"
            {
                int[] faceVertexCounts = [3, 3]
                int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
                point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
            }
        }
        "low" {
            def Mesh "LowGeo"
            {
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
                point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            }
        }
    }
}
`;

await testAsync('next-only RenderStream applies variant selections', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: (file) => new URL(file, wasmDir).href,
  });

  // Authored selection ("high") composes by default.
  const stream = new native.RenderStream();
  try {
    const load = stream.begin(encode(VARIANT_USDA));
    assert.ok(load && load.success, `variant scene load failed: ${load?.error || stream.error()}`);
    assert.equal(load.meshCount, 1, 'selected variant should contribute one mesh');
    const variants = stream.listVariants();
    assert.equal(variants.length, 1, 'authored variant set should be listed');
    assert.equal(variants[0].setName, 'lod');
    assert.equal(variants[0].selected, 'high');
    assert.deepEqual(Array.from(variants[0].variants), ['high', 'low']);
    const highMesh = stream.getMesh(0);
    assert.equal(highMesh.points.length, 12, 'high variant mesh should have 4 points');

    // Override to "low" and reload.
    stream.setVariantOverride('lod', 'low');
    const reload = stream.begin(encode(VARIANT_USDA));
    assert.ok(reload && reload.success, `variant override reload failed: ${reload?.error}`);
    assert.equal(reload.meshCount, 1, 'override variant should contribute one mesh');
    const lowMesh = stream.getMesh(0);
    assert.equal(lowMesh.points.length, 9, 'low variant mesh should have 3 points');
  } finally {
    stream.end();
    stream.delete();
  }
});

await testAsync('legacy module next session accepts USDA dependency layers', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_64.js' : '../src/lightusd/lightusd.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href));
  const usd = new native.LightUSDLoaderNative();
  try {
    // Root goes in via the zero-copy buffer protocol used by the legacy module.
    const rootBytes = encode(ROOT_USDA);
    const info = usd.allocateZeroCopyBuffer('__test_usda_root__', rootBytes.length, 0);
    assert.ok(info && info.success, `allocateZeroCopyBuffer failed: ${info?.error}`);
    native.HEAPU8.set(rootBytes, Number(info.bufferPtr));

    const begin = usd.nextFlattenAsyncBegin(info.uuid, 'root.usda', true);
    assert.ok(begin && begin.success, `begin failed: ${begin?.error}`);
    const session = begin.session;

    const result = driveSession({
      step: () => usd.nextFlattenAsyncStep(session, null),
      provideLayer: (key, bytes) => usd.nextFlattenAsyncProvideLayer(session, key, bytes),
      close: () => usd.nextFlattenAsyncEnd(session),
    }, new Map([['dep.usda', encode(DEP_USDA)]]));

    const out = new Uint8Array(result.data);
    assert.ok(isUSDC(out), 'flatten output should be a USDC crate');
    assert.ok(result.primCount >= 2, 'composed output should contain grafted prims');
  } finally {
    if (typeof usd.delete === 'function') usd.delete();
  }
});

await testAsync('next-only module usddiff diffs USDA layers', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: (file) => new URL(file, wasmDir).href,
  });
  assert.equal(typeof native.usddiff, 'function', 'next-only glue should expose usddiff');

  const same = native.usddiff({
    left: { data: encode(DEP_USDA), name: 'a.usda' },
    right: { data: encode(DEP_USDA), name: 'b.usda' },
    format: 'both'
  });
  assert.ok(same.success, `usddiff failed: ${same.error}`);
  assert.equal(same.hasDiffs, false, 'identical layers should have no diffs');
  assert.match(same.text, /No differences found/);

  const changed = native.usddiff({
    left: { data: encode(DEP_USDA), name: 'a.usda' },
    right: { data: encode(DEP_USDA.replace('(1, 0, 0)', '(2, 0, 0)')), name: 'b.usda' },
    format: 'both'
  });
  assert.ok(changed.success, `usddiff failed: ${changed.error}`);
  assert.equal(changed.hasDiffs, true, 'value change should be detected');
  assert.match(changed.text, /Property modified/);
  const json = JSON.parse(changed.json);
  assert.ok(json.property_diffs, 'json output should carry property_diffs');
});

await testAsync('next-only module validates USD from binary', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: (file) => new URL(file, wasmDir).href,
  });
  assert.equal(typeof native.validateFromBinary, 'function',
    'next-only glue should expose validateFromBinary');

  const ok = JSON.parse(native.validateFromBinary(
    encode(DEP_USDA), 'ok.usda', JSON.stringify({ groups: ['core', 'geom'] })));
  assert.equal(ok.parse_ok, true, 'valid USDA should parse');
  assert.ok(Array.isArray(ok.issues), 'issues should be an array');
  assert.ok(ok.checked_groups.includes('core'), 'core group should be checked');
  assert.ok(ok.checked_groups.includes('geom'), 'geom group should be checked');
  assert.equal(typeof ok.spec_version, 'string', 'spec_version should be present');

  // metersPerUnit = 0 is an AOUSD core violation.
  const bad = JSON.parse(native.validateFromBinary(
    encode('#usda 1.0\n(\n    metersPerUnit = 0\n)\n'), 'bad.usda',
    JSON.stringify({ groups: ['core'] })));
  assert.equal(bad.parse_ok, true, 'bad-meta USDA still parses');
  assert.equal(bad.ok, false, 'violations should fail validation');
  assert.ok(bad.error_count >= 1, 'error_count should reflect violations');

  const noParse = JSON.parse(native.validateFromBinary(
    encode('not a usd file at all'), 'junk.bin', '{}'));
  assert.equal(noParse.parse_ok, false, 'junk input should report parse failure');
  assert.equal(typeof noParse.error, 'string', 'parse failure should carry error');
});

console.log(`next-usda-composition tests done (${wasm64 ? 'wasm64' : 'wasm32'})`);
