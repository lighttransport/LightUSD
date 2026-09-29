// USDA dependency-layer composition through the wasm next flatten paths.
//
// Covers Phase-1 "USDA in next-core composition" wiring:
//  - next-only module: NextFlattenSession need-layer protocol with a USDA root
//    referencing a USDA dependency layer (and a USDC dependency for mixing).
//  - combined module: nextFlattenAsync* session accepting USDA dependency bytes
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

  assert.deepEqual(result.layerDependencies, ['dep.usda'],
    'successful flatten reports the source layer consumed by composition');
  assert.equal(result.layerDependencyCount, 1);

  const directStream = new native.RenderStream();
  const directStore = new native.NextAssetStore();
  try {
    directStore.registerMemoryAsset('dep.usda', encode(DEP_USDA));
    directStream.setAssetStore(directStore);
    const directLoad = directStream.begin(encode(ROOT_USDA));
    assert.ok(directLoad.success, directLoad.error || directStream.error());
    const report = directStream.compositionReport();
    assert.ok(report.dependencies.some(id => id.includes('dep.usda')),
      `RenderStream exposes resolved PCP layer dependencies: ${JSON.stringify(report)}`);
    assert.ok(Array.isArray(report.issues));
    assert.equal(report.issues.length, 0,
      'a clean composition exposes an empty typed issue list');
    const compositionQuery = native._lightusd_next_render_composition_record;
    const allocate = native._lightusd_next_alloc;
    let allocations = 0;
    native._lightusd_next_render_composition_record = (_handle, field) =>
      field === 0 ? 65537 : 0;
    native._lightusd_next_alloc = size => { ++allocations; return allocate(size); };
    try {
      assert.throws(() => directStream.compositionReport(), /excessive record count/);
      assert.equal(allocations, 0,
        'composition record counts are bounded before allocating strings');
    } finally {
      native._lightusd_next_render_composition_record = compositionQuery;
      native._lightusd_next_alloc = allocate;
    }
  } finally {
    directStream.end();
    directStream.delete();
    directStore.delete();
  }

  const missingStream = new native.RenderStream();
  try {
    const missingLoad = missingStream.begin(encode(ROOT_USDA));
    assert.ok(missingLoad.success, missingLoad.error || missingStream.error());
    const report = missingStream.compositionReport();
    assert.ok(report.issues.some(issue => issue.code === 3 &&
      issue.site.includes('dep.usda') && issue.message.includes('not found')),
    'RenderStream exposes a typed InvalidAssetPath issue with site and message');
  } finally {
    missingStream.end();
    missingStream.delete();
  }

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

await testAsync('next flatten reports dependencies loaded before another layer request', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: file => new URL(file, wasmDir).href,
  });
  const session = new native.NextFlattenSession();
  try {
    const root = encode(`#usda 1.0
(
    subLayers = [@a.usda@, @b.usda@]
)
def Xform "Root" { }
`);
    assert.equal(session.begin(root, 'root.usda', true).success, true);
    const first = session.step(null);
    assert.equal(first.status, 'need-layer');
    assert.ok(['a.usda', 'b.usda'].includes(first.key));
    assert.equal(first.layerDependencies, undefined,
      'the first layer request has not consumed a dependency yet');
    assert.deepEqual(session.provideLayer(first.key,
      encode(`#usda 1.0\ndef Xform "${first.key[0].toUpperCase()}" {}\n`)),
      {success: true});
    const second = session.step(null);
    assert.equal(second.status, 'need-layer');
    assert.notEqual(second.key, first.key);
    assert.deepEqual(second.layerDependencies, [first.key],
      'need-layer results include dependencies already loaded in that attempt');
    assert.deepEqual(session.provideLayer(second.key,
      encode(`#usda 1.0\ndef Xform "${second.key[0].toUpperCase()}" {}\n`)),
      {success: true});
    const done = session.step(null);
    assert.equal(done.status, 'done', done.error);
    assert.deepEqual(done.layerDependencies, ['a.usda', 'b.usda']);
  } finally {
    session.end();
    session.delete();
  }
});

await testAsync('next flatten session authors and composes a new sublayer', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: file => new URL(file, wasmDir).href
  });
  const session = new native.NextFlattenSession();
  const bounded = new native.NextFlattenSession();
  const stream = new native.RenderStream();
  const root = encode('#usda 1.0\ndef Xform "Root" {}\n');
  const child = encode(`#usda 1.0
def Mesh "ChildMesh" {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
}
`);
  try {
    assert.deepEqual(session.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(session.addSublayer('child.usda'), {success: true});
    assert.throws(() => session.addSublayer(), /expected one non-empty path/);
    const request = session.step(null);
    assert.equal(request.status, 'need-layer');
    assert.equal(request.key, 'child.usda');
    assert.deepEqual(session.provideLayer('child.usda', child), {success: true});
    const done = session.step(null);
    assert.equal(done.status, 'done', done.error);
    assert.ok(done.layerDependencies.includes('child.usda'));
    assert.equal(stream.begin(done.data).success, true, stream.error());
    assert.equal(stream.meshCount(), 1,
      'the authored sublayer contributes its mesh to flattened output');
    assert.deepEqual(bounded.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(bounded.setMaxInputBytes(80), {success: true});
    assert.equal(bounded.addSublayer('x'.repeat(256)).success, false);
    assert.match(bounded.error(), /aggregate input byte limit/);
    assert.equal(bounded.step(null).status, 'done',
      'a rejected sublayer edit leaves the original root unchanged');
  } finally {
    stream.end(); stream.delete();
    session.end(); session.delete();
    bounded.end(); bounded.delete();
  }
});

await testAsync('next flatten session authors prim composition arcs', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: file => new URL(file, wasmDir).href
  });
  const root = encode(`#usda 1.0
def Xform "Base" {
  def Mesh "Geo" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  }
}
def Xform "Site" {}
`);
  const child = encode(`#usda 1.0
def Xform "Asset" {
  def Mesh "Geo" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  }
}
`);
  for (const [kind, assetPath, targetPath, needsLayer, listOp] of [
    [0, 'child.usda', '/Asset', true, 'prepend'],
    [1, 'child.usda', '/Asset', true, 'append'],
    [2, '', '/Base', false, 'add'],
    [3, '', '/Base', false, 'explicit']
  ]) {
    const session = new native.NextFlattenSession();
    const stream = new native.RenderStream();
    try {
      assert.deepEqual(session.begin(root, 'root.usda', true),
        {success: true, status: 'ready'});
      assert.equal(session.addPrimArc(kind, '/Missing', assetPath, targetPath).success, false);
      assert.match(session.error(), /site does not exist/);
      assert.deepEqual(session.addPrimArc(kind, '/Site', assetPath, targetPath, listOp),
        {success: true});
      let result = session.step(null);
      if (needsLayer) {
        assert.equal(result.status, 'need-layer');
        assert.equal(result.key, 'child.usda');
        assert.deepEqual(session.provideLayer('child.usda', child), {success: true});
        result = session.step(null);
      }
      assert.equal(result.status, 'done', result.error);
      assert.equal(stream.begin(result.data).success, true, stream.error());
      assert.equal(stream.meshCount(), 2,
        `arc kind ${kind} composes geometry at its target site`);
    } finally {
      stream.end(); stream.delete();
      session.end(); session.delete();
    }
  }
  const bounded = new native.NextFlattenSession();
  try {
    assert.deepEqual(bounded.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(bounded.setMaxInputBytes(root.length + 32), {success: true});
    assert.equal(bounded.addPrimArc(0, '/Site', 'x'.repeat(128), '/Asset').success, false);
    assert.match(bounded.error(), /aggregate input byte limit/);
    assert.equal(bounded.step(null).status, 'done',
      'a rejected arc edit leaves the original layer unmodified');
  } finally {
    bounded.end(); bounded.delete();
  }
});

await testAsync('next flatten session applies reference delete list-ops', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: file => new URL(file, wasmDir).href
  });
  const session = new native.NextFlattenSession();
  const stream = new native.RenderStream();
  const root = encode(`#usda 1.0
(
  subLayers = [@weak.usda@]
)
over Xform "Site" {}
`);
  const weak = encode(`#usda 1.0
def Xform "Site" (references = @base.usda@</Base>) {}
`);
  try {
    assert.deepEqual(session.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(session.addPrimArc(0, '/Site', 'base.usda', '/Base', 'delete'),
      {success: true});
    const request = session.step(null);
    assert.equal(request.status, 'need-layer');
    assert.equal(request.key, 'weak.usda');
    assert.deepEqual(session.provideLayer('weak.usda', weak), {success: true});
    const done = session.step(null);
    assert.equal(done.status, 'done', done.error);
    assert.ok(!done.layerDependencies.includes('base.usda'),
      'the delete opinion removes the weaker reference before resolving it');
    assert.equal(stream.begin(done.data).success, true, stream.error());
    assert.equal(stream.meshCount(), 0);
  } finally {
    stream.end(); stream.delete();
    session.end(); session.delete();
  }
});


await testAsync('next flatten session reorders references from a weaker layer', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: file => new URL(file, wasmDir).href
  });
  const session = new native.NextFlattenSession();
  const stream = new native.RenderStream();
  const root = encode(`#usda 1.0
(
  subLayers = [@weak.usda@]
)
over Xform "Site" {}
`);
  const weak = encode(`#usda 1.0
def Xform "Site" (
  references = [@a.usda@</A>, @b.usda@</B>]
) {}
`);
  const asset = target => encode(`#usda 1.0
def Xform "${target}" {
  def Mesh "Geo" {
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  }
}
`);
  try {
    assert.deepEqual(session.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(session.addPrimArc(0, '/Site', 'b.usda', '/B', 'reorder'),
      {success: true});
    assert.deepEqual(session.addPrimArc(0, '/Site', 'a.usda', '/A', 'reorder'),
      {success: true});
    const weakRequest = session.step(null);
    assert.equal(weakRequest.status, 'need-layer');
    assert.equal(weakRequest.key, 'weak.usda',
      'reorder entries do not act as newly authored reference arcs');
    assert.deepEqual(session.provideLayer('weak.usda', weak), {success: true});
    const strongestReference = session.step(null);
    assert.equal(strongestReference.status, 'need-layer');
    assert.equal(strongestReference.key, 'b.usda',
      'the stronger reorder edit changes the first weaker reference resolved');
    assert.deepEqual(session.provideLayer('b.usda', asset('B')), {success: true});
    const nextReference = session.step(null);
    assert.equal(nextReference.status, 'need-layer');
    assert.equal(nextReference.key, 'a.usda');
    assert.deepEqual(session.provideLayer('a.usda', asset('A')), {success: true});
    const done = session.step(null);
    assert.equal(done.status, 'done', done.error);
    assert.equal(stream.begin(done.data).success, true, stream.error());
    assert.ok(stream.meshCount() >= 1, 'the reordered references both flatten successfully');
  } finally {
    stream.end(); stream.delete();
    session.end(); session.delete();
  }
});


await testAsync('next flatten session remaps asset-valued properties within its input budget', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: file => new URL(file, wasmDir).href
  });
  const root = encode(`#usda 1.0
def Shader "Texture" {
  uniform token info:id = "UsdUVTexture"
  asset inputs:file = @old.png@
}
`);
  const verify = (bytes, expected) => {
    const converter = new native.NextUSDZConverterNative();
    try {
      const result = converter.rewriteRoot(bytes, 'remapped.usdc',
        {rootLayerFormat: 'usda'});
      assert.equal(result.success, true, result.error);
      const text = new TextDecoder().decode(result.data);
      assert.ok(text.includes(`@${expected}@`), text);
      return text;
    } finally { converter.delete(); }
  };
  const session = new native.NextFlattenSession();
  const bounded = new native.NextFlattenSession();
  const direct = new native.NextFlattenSession();
  const outputBounded = new native.NextFlattenSession();
  try {
    assert.deepEqual(session.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    const before = session.inputBytes();
    assert.deepEqual(session.setAssetPathRemap({'old.png': 'textures/new.png'}),
      {success: true});
    assert.equal(session.inputBytes(), before + 'old.png'.length + 'textures/new.png'.length,
      'retained remap entries count against the aggregate input budget');
    const done = session.step(null);
    assert.equal(done.status, 'done', done.error);
    verify(done.data, 'textures/new.png');

    assert.deepEqual(direct.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.equal(direct.remapLayerAssetPaths({'old.png': encode('renamed.png')}), 1,
      'direct mutation reports the number of asset values changed');
    assert.equal(direct.remapLayerAssetPaths({'absent.png': 'unused.png'}), 0);
    const directlyRemapped = direct.step(null);
    assert.equal(directlyRemapped.status, 'done', directlyRemapped.error);
    verify(directlyRemapped.data, 'renamed.png');

    assert.deepEqual(outputBounded.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(outputBounded.setAssetPathRemap({'old.png': 'x.png'}),
      {success: true});
    assert.deepEqual(outputBounded.setMaxInputBytes(root.length + 12), {success: true});
    assert.throws(() => outputBounded.remapLayerAssetPaths(
      {'old.png': 'a-longer-name.png'}), /aggregate input byte limit/i);
    const atomic = outputBounded.step(null);
    assert.equal(atomic.status, 'done', atomic.error);
    verify(atomic.data, 'x.png');

    assert.deepEqual(bounded.begin(root, 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(bounded.setAssetPathRemap({'old.png': 'x.png'}),
      {success: true});
    assert.deepEqual(bounded.setMaxInputBytes(root.length + 12), {success: true});
    assert.throws(() => bounded.setAssetPathRemap({'old.png': 'textures/new.png'}),
      /aggregate input byte limit/);
    assert.throws(() => bounded.setAssetPathRemap({'old.png': 42}), /string or byte view/);
    const unchanged = bounded.step(null);
    assert.equal(unchanged.status, 'done', unchanged.error);
    verify(unchanged.data, 'x.png');
  } finally {
    session.end(); session.delete();
    bounded.end(); bounded.delete();
    direct.end(); direct.delete();
    outputBounded.end(); outputBounded.delete();
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

const REFERENCED_VARIANT_ROOT = `#usda 1.0
(
    defaultPrim = "Root"
)
def Xform "Root" (
    references = @variant-dep.usda@</Asset>
)
{
}
`;

const REFERENCED_VARIANT_DEP = `#usda 1.0
def Xform "Asset" (
    variants = { string lod = "high" }
    prepend variantSets = "lod"
)
{
    variantSet "lod" = {
        "high" {
            def Mesh "Geo" {
                int[] faceVertexCounts = [3, 3]
                int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
                point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
            }
        }
        "low" {
            def Mesh "Geo" {
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
                point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            }
        }
    }
}
`;

const LOD_VARIANT_USDA = `#usda 1.0
def Xform "Root" (
    variants = { string LOD = "one" }
    prepend variantSets = "LOD"
)
{
    variantSet "LOD" = {
        "one" {}
        "two" {}
        "three" {}
    }
    def Xform "Child" (
        variants = { string LOD = "a" }
        prepend variantSets = "LOD"
    )
    {
        variantSet "LOD" = {
            "a" {}
            "b" {}
        }
    }
}
`;

await testAsync('next-only flatten session applies typed variant override', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: (file) => new URL(file, wasmDir).href,
  });
  const session = new native.NextFlattenSession();
  const stream = new native.RenderStream();
  try {
    assert.deepEqual(session.begin(encode(VARIANT_USDA), 'variants.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(session.setVariantOverride('lod', 'low'), {success: true});
    const flattened = session.step(null);
    assert.equal(flattened.success, true, flattened.error);
    assert.equal(flattened.status, 'done');
    const load = stream.begin(flattened.data);
    assert.equal(load.success, true, load.error);
    assert.equal(load.meshCount, 1);
    assert.equal(stream.getMesh(0).points.length, 9,
      'flattened low variant should have three vertices');
  } finally {
    stream.end();
    stream.delete();
    session.end();
    session.delete();
  }
});

await testAsync('scoped variant override applies inside a referenced dependency', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: (file) => new URL(file, wasmDir).href,
  });
  const session = new native.NextFlattenSession();
  const stream = new native.RenderStream();
  try {
    assert.deepEqual(session.begin(encode(REFERENCED_VARIANT_ROOT), 'root.usda', true),
      {success: true, status: 'ready'});
    assert.deepEqual(session.setVariantOverride('/Root{lod}', 'low'), {success: true});
    const flattened = driveSession({
      step: () => session.step(null),
      provideLayer: (key, bytes) => session.provideLayer(key, bytes),
      close: () => {}
    }, new Map([['variant-dep.usda', encode(REFERENCED_VARIANT_DEP)]]));
    assert.deepEqual(flattened.layerDependencies, ['variant-dep.usda']);
    const load = stream.begin(flattened.data);
    assert.ok(load.success, load.error || stream.error());
    assert.equal(load.meshCount, 1);
    assert.equal(stream.meshPointsBuffer(0).length, 9,
      'the scoped low selection must replace the referenced high topology');
  } finally {
    stream.end();
    stream.delete();
    session.end();
    session.delete();
  }
});

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
    assert.equal(variants[0].primPath, '/Root');
    assert.equal(variants[0].setName, 'lod');
    assert.equal(variants[0].selected, 'high');
    assert.deepEqual(Array.from(variants[0].variants), ['high', 'low']);
    assert.equal(stream.lodVariantCount(), 0,
      'the legacy helper counts only the exact uppercase LOD set name');
    const highMesh = stream.getMesh(0);
    assert.equal(highMesh.points.length, 12, 'high variant mesh should have 4 points');

    // Override to "low" and reload.
    stream.setVariantOverride('lod', 'low');
    const reload = stream.begin(encode(VARIANT_USDA));
    assert.ok(reload && reload.success, `variant override reload failed: ${reload?.error}`);
    assert.equal(reload.meshCount, 1, 'override variant should contribute one mesh');
    const lowMesh = stream.getMesh(0);
    assert.equal(lowMesh.points.length, 9, 'low variant mesh should have 3 points');
    assert.deepEqual(stream.listVariants(), variants,
      'variant listing should retain authored selection after an override');
  } finally {
    stream.end();
    stream.delete();
  }
});

await testAsync('next-only lodVariantCount reports the largest LOD option set', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                      : '../src/lightusd/lightusd_next.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href), {
    locateFile: file => new URL(file, wasmDir).href
  });
  const stream = new native.RenderStream();
  try {
    assert.equal(stream.lodVariantCount(), 0);
    assert.equal(stream.begin(encode(LOD_VARIANT_USDA)).success, true);
    assert.equal(stream.lodVariantCount(), 3,
      'nested LOD sets use the maximum option count across prims');
  } finally {
    stream.end();
    stream.delete();
  }
});

if (process.env.LIGHTUSD_NEXT_ONLY !== '1') await testAsync(
  'combined module next session accepts USDA dependency layers', async () => {
  const glue = wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                      : '../src/lightusd/lightusd_combined.js';
  const native = await loadWasm(() => import(new URL(glue, import.meta.url).href));
  const usd = new native.LightUSDLoaderNative();
  try {
    // Root goes in via the zero-copy buffer protocol of the LightUSDLoaderNative API.
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
  assert.deepEqual(native.usddiff(null),
    {success: false, error: 'usddiff: missing options'});
  assert.deepEqual(native.usddiff({left: {}}),
    {success: false, error: "usddiff: 'left' and 'right' are required"});

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
  const padded = encode(`x${DEP_USDA}y`).subarray(1, -1);
  const jsonOnly = native.usddiff({
    left: {data: padded, name: 'left.usda'},
    right: {data: encode(DEP_USDA), name: 'right.usda'},
    format: 'json', ulps: 0, compareMetadata: false, fuzzyAssetPaths: false,
  });
  assert.equal(jsonOnly.success, true, jsonOnly.error);
  assert.equal(jsonOnly.hasDiffs, false);
  assert.equal(Object.hasOwn(jsonOnly, 'text'), false);
  assert.equal(typeof jsonOnly.json, 'string');
  const heapPtr = native._lightusd_next_alloc(padded.length + 2);
  try {
    native.HEAPU8.set(padded, Number(heapPtr) + 1);
    const heapInput = native.HEAPU8.subarray(
      Number(heapPtr) + 1, Number(heapPtr) + 1 + padded.length);
    const heapResult = native.usddiff({
      left: {data: heapInput}, right: {data: padded}, format: 'text'
    });
    assert.equal(heapResult.success, true, heapResult.error);
    assert.equal(heapResult.hasDiffs, false);
    assert.equal(Object.hasOwn(heapResult, 'json'), false);
  } finally {
    native._lightusd_next_free(heapPtr);
  }
  const invalid = native.usddiff({
    left: {data: encode('not USD'), name: 'broken.usda'},
    right: {data: padded}
  });
  assert.equal(invalid.success, false);
  assert.match(invalid.error, /Error loading broken\.usda/);
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
