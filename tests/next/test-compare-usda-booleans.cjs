#!/usr/bin/env node
const assert = require('node:assert/strict');
const { compareUsda, parseUsda } = require('../compare-usda.js');
function compare(left, right) {
  const scene = attr => parseUsda(`#usda 1.0\ndef Scope "S" {\n${attr}\n}\n`);
  return compareUsda(scene(left), scene(right));
}
for (const [left, right] of [
  ['bool b = false', 'bool b = 0'],
  ['custom uniform bool b = true', 'custom uniform bool b = 1'],
  ['bool[] b = [true, false]', 'bool[] b = [1, 0]'],
  ['bool b.timeSamples = {0: false, 1: true}', 'bool b.timeSamples = {0: 0, 1: 1}'],
  ['bool[] b.timeSamples = {0: [false, true]}', 'bool[] b.timeSamples = {0: [0, 1]}'],
]) {
  assert.deepEqual(compare(left, right), [], `${left} equals ${right}`);
  assert.deepEqual(compare(right, left), [], 'comparison is symmetric');
}
for (const [left, right] of [
  ['bool b = false', 'bool b = 1'],
  ['bool b = true', 'bool b = 2'],
  ['bool b = true', 'bool b = 1.0000001'],
  ['bool b = false', 'bool b = 0.0000001'],
  ['bool[] b = [true]', 'bool[] b = [1.0000001]'],
  ['bool b.timeSamples = {0: true}', 'bool b.timeSamples = {0: 1.0000001}'],
  ['bool[] b = [true, false]', 'bool[] b = [0, 1]'],
  ['bool[] b = [true]', 'bool[] b = [1, 0]'],
  ['bool b.timeSamples = {0: false}', 'bool b.timeSamples = {1: 0}'],
  ['bool b.timeSamples = {0: false}', 'bool b.timeSamples = {0: 1}'],
  ['string b = "false"', 'string b = "0"'],
  ['token b = "true"', 'token b = "1"'],
  ['bool b = true', 'int b = 1'],
  ['bool b = true', 'bool[] b = [1]'],
]) {
  assert.notDeepEqual(compare(left, right), [], `${left} differs from ${right}`);
}
console.log('compare-usda boolean tests passed');
