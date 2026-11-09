#!/usr/bin/env node
const assert = require('node:assert/strict');
const { compareUsda, parseUsda } = require('../compare-usda.js');

const source = `#usda 1.0
def Xform "Root" (
    prepend references = [@model.usda@</Model> (
        offset = 2; scale = 3; customData = {
            int priority = 7
            dictionary nested = { string label = "</Decoy> (offset = 99)" }
        }
    )]
) { int after = 42 }
`;
const formatted = `#usda 1.0
def Xform "Root" (
    prepend references = @model.usda@</Model> (offset = 2; scale = 3; customData = {
        dictionary nested = { string label = "</Decoy> (offset = 99)" }
        int priority = 7
    })
) { int after = 42 }
`;
const parsed = parseUsda(source);
assert.deepEqual(compareUsda(parsed, parseUsda(formatted)), [],
  'reference parameters must stay inside the arc, independent of formatting');
for (const [before, after] of [
  ['priority = 7', 'priority = 8'],
  ['</Model>', '</Different>'],
  ['offset = 2', 'offset = 4'],
  ['scale = 3', 'scale = 5'],
  ['int after = 42', 'int after = 43'],
]) {
  assert.notDeepEqual(compareUsda(parsed, parseUsda(formatted.replace(before, after))), [],
    `comparison must detect changes to ${before}`);
}
const layer = arc => parseUsda(`#usda 1.0\ndef "Root" (references = ${arc}) {}`);
assert.deepEqual(compareUsda(layer('@model.usda@'),
  layer('[@model.usda@ (offset = 0; scale = 1; customData = {})]')), [],
  'omitted reference defaults compare equivalently');
assert.deepEqual(compareUsda(layer('</Local>'), layer('[</Local>]')), [],
  'internal references preserve the prim path');
assert.notDeepEqual(compareUsda(layer('</Local>'), layer('</Other>')), []);
const payload = arc => parseUsda(`#usda 1.0\ndef "Root" (payload = ${arc}) {}`);
assert.deepEqual(compareUsda(payload('@model.usda@</Model> (offset = 2)'),
  payload('[@model.usda@</Model> (offset = 2)]')), []);
console.log('compare-usda reference tests passed');
