// SPDX-License-Identifier: Apache-2.0
// Load diagnostics: the next RenderStream progress/cancel/diagnostic contract
// is the product behavior. Results that match the combined legacy loader are
// compared directly; where next differs (progress records, error text) or
// adds a capability legacy lacks (cancelling or observing a load from the
// progress callback), both sides are pinned so a change on either fails.
// Rows naming this test in next-wasm-parity-gaps.json must match what it
// exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-load-diagnostics-behavior-parity.test.mjs';
const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const combinedUrl = process.env.LIGHTUSD_COMBINED_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_COMBINED_MODULE)).href
  : new URL(wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                   : '../src/lightusd/lightusd_combined.js', import.meta.url).href;
const legacyModule = await loadWasm(() => import(combinedUrl));
const {default: createNext} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const nextModule = await createNext();
const encode = text => new TextEncoder().encode(text);

const exercised = new Map();  // method -> 'match' | 'delta'
const note = (method, kind) => {
  if (exercised.get(method) !== 'delta') exercised.set(method, kind);
};
const valid = encode(['#usda 1.0', 'def Mesh "M" {', ' int[] faceVertexCounts = [3]',
  ' int[] faceVertexIndices = [0,1,2]', ' point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]',
  '}', ''].join('\n'));
const broken = encode('#usda 1.0\ndef Xform "A" {\n float x = \n');
const idleProgress = {progress: 0, stage: 'idle', currentOperation: '', cancelRequested: false,
  errorMessage: '', bytesProcessed: 0, totalBytes: 0, percentage: 0, meshesProcessed: 0,
  meshesTotal: 0, currentMeshName: '', materialsProcessed: 0, materialsTotal: 0, tydraStage: ''};
const pick = (record, keys) => Object.fromEntries(keys.map(key => [key, record[key]]));
const sameState = (loader, stream, label) => {
  for (const method of ['ok', 'warn', 'isParsingInProgress', 'wasCancelled']) {
    assert.deepEqual(stream[method](), loader[method](), `${label}: ${method}`);
    note(method, 'match');
  }
};

const loader = new legacyModule.LightUSDLoaderNative();
const stream = new nextModule.RenderStream();
try {
  // Fresh objects agree, including the idle progress record.
  sameState(loader, stream, 'fresh');
  assert.equal(stream.error(), loader.error());
  assert.deepEqual(loader.getProgress(), idleProgress);
  assert.deepEqual(stream.getProgress(), idleProgress);

  // Successful load: same state; next's record reports the completed parse
  // with byte counts, legacy reports an idle stage after conversion.
  assert.equal(loader.loadFromBinary(valid, 'mesh.usda'), true);
  assert.equal(stream.begin(valid, 'mesh.usda').success, true);
  sameState(loader, stream, 'loaded');
  assert.equal(stream.error(), '');
  assert.equal(loader.error(), '');
  const progressKeys = ['progress', 'stage', 'currentOperation', 'bytesProcessed', 'totalBytes',
    'percentage', 'errorMessage', 'tydraStage'];
  assert.deepEqual(pick(loader.getProgress(), progressKeys), {progress: 1, stage: 'idle',
    currentOperation: 'Conversion complete', bytesProcessed: 0, totalBytes: 0, percentage: 100,
    errorMessage: '', tydraStage: 'complete'});
  assert.deepEqual(pick(stream.getProgress(), progressKeys), {progress: 1, stage: 'complete',
    currentOperation: 'complete', bytesProcessed: valid.length, totalBytes: valid.length,
    percentage: 100, errorMessage: '', tydraStage: ''});

  // Failed parse: both report failure; next's error is the next parser's
  // located message and its record carries the error stage, while legacy
  // keeps the previous "complete" record.
  assert.equal(loader.loadFromBinary(broken, 'broken.usda'), false);
  assert.equal(stream.begin(broken, 'broken.usda').success, false);
  sameState(loader, stream, 'failed');
  assert.match(loader.error(), /Failed to parse USDA/);
  assert.match(stream.error(), /^Line \d+, column \d+: Expected float value$/);
  assert.deepEqual(pick(loader.getProgress(), ['progress', 'stage', 'errorMessage']),
    {progress: 1, stage: 'idle', errorMessage: ''});
  assert.deepEqual(pick(stream.getProgress(), ['progress', 'stage', 'errorMessage', 'totalBytes']),
    {progress: 0, stage: 'error', errorMessage: stream.error(), totalBytes: broken.length});
  exercised.set('error', 'delta');
  exercised.set('getProgress', 'delta');

  // resetProgress returns both to the idle record.
  assert.equal(stream.resetProgress(), loader.resetProgress());
  assert.deepEqual(loader.getProgress(), idleProgress);
  assert.deepEqual(stream.getProgress(), idleProgress);
  note('resetProgress', 'match');

  // An idle cancelParsing() never cancels the next load on either product;
  // only legacy records it in its progress record.
  assert.equal(stream.cancelParsing(), loader.cancelParsing());
  assert.equal(loader.getProgress().cancelRequested, true);
  assert.equal(stream.getProgress().cancelRequested, false);
  assert.equal(loader.loadFromBinary(valid, 'mesh.usda'), true);
  assert.equal(stream.begin(valid, 'mesh.usda').success, true);
  assert.equal(stream.numMeshes(), loader.numMeshes());
  sameState(loader, stream, 'after idle cancel');
  exercised.set('cancelParsing', 'delta');

  // releaseSourceLayer keeps the converted scene; reset clears it.
  assert.equal(stream.releaseSourceLayer(), loader.releaseSourceLayer());
  sameState(loader, stream, 'released');
  assert.equal(stream.numMeshes(), loader.numMeshes());
  assert.equal(stream.hasSublayers(), loader.hasSublayers());
  note('releaseSourceLayer', 'match');
  assert.equal(stream.reset(), loader.reset());
  sameState(loader, stream, 'reset');
  assert.equal(stream.ok(), false);
  assert.equal(stream.numMeshes(), 0);
  assert.equal(loader.numMeshes(), 0);
  assert.equal(stream.error(), loader.error());
  note('reset', 'match');
} finally { loader.delete(); stream.delete(); }

// Next-only: the progress callback observes the parse and can cancel it.
// Legacy loads synchronously without a JS callback, so it has no equivalent.
{
  const cancelling = new nextModule.RenderStream();
  try {
    const phases = [];
    let parsingSeen = false;
    let busyReset = null;
    cancelling.setProgressCallback(event => {
      phases.push(event.phase);
      parsingSeen ||= cancelling.isParsingInProgress();
      if (phases.length === 1) {
        try { cancelling.resetProgress(); } catch (error) { busyReset = error; }
      }
      if (phases.length === 2) cancelling.cancelParsing();
      return true;
    });
    const result = cancelling.begin(valid, 'mesh.usda');
    assert.equal(result.success, false);
    assert.equal(parsingSeen, true, 'isParsingInProgress is true inside the callback');
    assert.ok(busyReset instanceof TypeError && /busy/.test(busyReset.message),
      'resetProgress is rejected while a parse is running');
    assert.equal(cancelling.wasCancelled(), true);
    assert.equal(cancelling.isParsingInProgress(), false);
    assert.equal(cancelling.ok(), false);
    assert.match(cancelling.error(), /USDA parse cancelled/);
    assert.deepEqual(pick(cancelling.getProgress(), ['stage', 'cancelRequested', 'errorMessage']),
      {stage: 'cancelled', cancelRequested: true, errorMessage: cancelling.error()});

    // Returning false from the callback cancels too; the next begin clears
    // the cancellation state.
    cancelling.setProgressCallback(() => false);
    assert.equal(cancelling.begin(valid, 'mesh.usda').success, false);
    assert.equal(cancelling.wasCancelled(), true);
    assert.equal(cancelling.getProgress().stage, 'cancelled');
    cancelling.setProgressCallback(null);
    assert.equal(cancelling.begin(valid, 'mesh.usda').success, true);
    assert.equal(cancelling.wasCancelled(), false);
    assert.equal(cancelling.getProgress().cancelRequested, false);
    assert.equal(cancelling.ok(), true);
  } finally { cancelling.delete(); }
  for (const method of ['isParsingInProgress', 'wasCancelled', 'resetProgress', 'cancelParsing'])
    exercised.set(method, 'delta');
}

// ---------------------------------------------------------- matrix sync
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
const claimed = matrix.filter(row => row.behaviorTest === TEST_NAME);
assert.deepEqual(claimed.map(row => row.legacyMethod).sort(), [...exercised.keys()].sort(),
  'every method exercised here must carry this behaviorTest in the parity matrix, and vice versa');
for (const row of claimed) {
  const kind = exercised.get(row.legacyMethod);
  if (row.parityStatus === 'behavior_verified') {
    assert.equal(Boolean(row.edgeDifference), kind === 'delta',
      `${row.legacyMethod}: edgeDifference must be documented exactly when a delta is pinned`);
  } else {
    assert.equal(row.parityStatus, 'known_behavior_gap', row.legacyMethod);
    assert.equal(kind, 'delta', `${row.legacyMethod}: a known gap must pin its delta`);
  }
}
const gaps = claimed.filter(row => row.parityStatus === 'known_behavior_gap').length;
console.log(`ok - load diagnostics behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length - gaps} methods verified against legacy, ${gaps} pinned next-contract gaps`);
