import assert from 'node:assert/strict';
import test from 'node:test';

import { getUSDSceneMetadata } from '../src/lightusd/USDSceneMetadata.js';

function scene(metadata) {
  return { getSceneMetadata: () => metadata };
}

test('uses authored framesPerSecond for playback', () => {
  const metadata = getUSDSceneMetadata(scene({
    framesPerSecond: 30,
    timeCodesPerSecond: 48,
  }));
  assert.equal(metadata.framesPerSecond, 30);
  assert.equal(metadata.timeCodesPerSecond, 48);
});

test('falls back to timeCodesPerSecond when framesPerSecond is absent', () => {
  const metadata = getUSDSceneMetadata(scene({ timeCodesPerSecond: 24 }));
  assert.equal(metadata.framesPerSecond, 24);
  assert.equal(metadata.timeCodesPerSecond, 24);
});

test('falls back to USD default rate for invalid rates', () => {
  const metadata = getUSDSceneMetadata(scene({
    framesPerSecond: 0,
    timeCodesPerSecond: Number.NaN,
  }));
  assert.equal(metadata.framesPerSecond, 24);
  assert.equal(metadata.timeCodesPerSecond, 24);
});
