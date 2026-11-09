// SPDX-License-Identifier: Apache-2.0
// Shared Node, browser and module-Worker contract for independently linked heaps.
export async function checkFeatures(factory) {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const core = await factory();
  for (const key of ['SubdivStreamer', 'MeshoptSimplifier', 'LightRTPathTracer',
    'loadEXR', 'loadValidator', 'decodeEXR', 'checkUSD', 'validateFromBinary'])
    check(core[key] === undefined, `obsolete core API: ${key}`);
  check(core.featureInfo.abiMajor === 5 && core.featureInfo.coreMajor === 2, 'ABI versions');
  check(Object.isFrozen(core.featureInfo), 'immutable ABI metadata');
  let rejected = false;
  try { await core.loadFeature('missing'); } catch { rejected = true; }
  check(rejected, 'unknown feature must reject');
  // Each mismatch must reject before the feature can be used. Failed promises
  // leave the slot retryable; a successful promise remains shared by callers.
  for (const key of Object.keys(core.featureInfo)) {
    const info = {...core.featureInfo, [key]: core.featureInfo[key] + 1};
    core.locateFeature = () => 'data:text/javascript,' + encodeURIComponent(
      `export default async () => ({featureInfo:${JSON.stringify(info)},HEAPU8:new Uint8Array(8)})`);
    rejected = false;
    try { await core.loadFeature('geometry'); }
    catch (error) { rejected = error.message.includes(key); }
    check(rejected, `metadata rejection: ${key}`);
  }
  core.locateFeature = () => 'data:text/javascript,' + encodeURIComponent(
    'export default async () => {throw Error("factory failed")}');
  rejected = false;
  try { await core.loadFeature('geometry'); } catch (error) { rejected = /factory failed/.test(error.message); }
  check(rejected, 'factory failure');
  delete core.locateFeature;
  const first = core.loadFeature('geometry');
  check(first === core.loadFeature('geometry'), 'pending promise deduplication');
  const geometry = await first;
  check(first === core.loadFeature('geometry'), 'resolved promise deduplication');
  const [raytrace, validator, exr] = await Promise.all(
    ['raytrace', 'validator', 'exr'].map(name => core.loadFeature(name)));
  const features = [core, geometry, raytrace, validator, exr];
  check(new Set(features.map(m => m.HEAPU8.buffer)).size === 5, 'separate heaps');
  for (const feature of features)
    check(JSON.stringify(feature.featureInfo) === JSON.stringify(core.featureInfo), 'matching layouts');
  const points = new Float32Array([0,0,0, 1,0,0, 1,1,0, 0,1,0]);
  const ptr = core._lightusd_next_alloc(points.byteLength);
  try {
    const source = new Float32Array(core.HEAPU8.buffer, Number(ptr), points.length);
    source.set(points);
    const subdiv = new geometry.SubdivStreamer();
    let batches = 0;
    try {
      const error = subdiv.refineStream(source, new Uint32Array([4]),
        new Uint32Array([0,1,2,3]), null, null, 0,0,0,1,1,0,0,true,
        (positions, normals, indices) => {
          check(positions.buffer === geometry.HEAPU8.buffer, 'subdivision output heap');
          check(normals.length === positions.length && indices.length > 0, 'subdivision batch');
          ++batches;
        });
      check(error === '' && batches > 0, 'subdivision counted transfer');
    } finally { subdiv.delete(); }
    const simplifier = new geometry.MeshoptSimplifier();
    try {
      const result = simplifier.simplify(source, new Uint32Array([0,1,2,0,2,3]),
        null,null,null,3,1,0);
      check(result.indices.length >= 3 && result.indices.length <= 6, 'simplification');
      check(result.indices.buffer !== geometry.HEAPU8.buffer, 'owned simplification result');
    } finally { simplifier.delete(); }
    const tracer = new raytrace.LightRTPathTracer();
    try {
      check(tracer.build(source.subarray(0,9), new Float32Array(9), new Float32Array(9),
        new Float32Array(12), new Int32Array([0]), new Float32Array(10)), 'ray scene transfer');
      const hits = tracer.raycast(new Float32Array([.75,.25,1]), new Float32Array([0,0,-1]),10);
      check(hits.triangle[0] === 0 && Math.abs(hits.distance[0] - 1) < 1e-5, 'ray hit');
      check(hits.triangle.buffer !== raytrace.HEAPU8.buffer, 'owned ray result');
    } finally { tracer.delete(); }
  } finally { core._lightusd_next_free(ptr); }
  check(validator.checkUSD(new TextEncoder().encode('#usda 1.0\ndef Xform "World" {}'),
    'scene.usda').valid, 'validator transfer');
  const encoded = exr.encode(new Uint8Array([255,0,0,255]),1,1,4);
  const decoded = exr.decodeEXR(encoded);
  check(decoded.success && decoded.width === 1 && decoded.height === 1, 'EXR round trip');
  check(decoded.data.buffer !== exr.HEAPU8.buffer, 'owned EXR result');
  return {pointerBits: core.featureInfo.pointerBits, heaps: features.length};
}
