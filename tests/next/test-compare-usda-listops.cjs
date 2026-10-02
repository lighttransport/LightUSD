#!/usr/bin/env node
const assert = require('node:assert/strict');
const { compareUsda, parseUsda } = require('../compare-usda.js');
const scene = (body, metadata = '') => parseUsda(
  `#usda 1.0\ndef Scope "S" (\n${metadata}\n) {\n${body}\n}\n`);
function equal(left, right) {
  assert.deepEqual(compareUsda(left, right), []);
  assert.deepEqual(compareUsda(right, left), []);
}
function different(left, right) {
  assert.notDeepEqual(compareUsda(left, right), []);
  assert.notDeepEqual(compareUsda(right, left), []);
}
for (const qualifier of ['', 'prepend ', 'append ', 'delete ', 'add ', 'reorder ']) {
  equal(scene(`${qualifier}rel r = None`), scene(`${qualifier}rel r = []`));
  for (const field of ['inherits', 'specializes', 'references', 'payload',
                       'apiSchemas', 'variantSets', 'clipSets']) {
    equal(scene('', `${qualifier}${field} = None`),
          scene('', `${qualifier}${field} = []`));
  }
}
different(scene('rel r'), scene('rel r = None'));
different(scene('rel r'), scene('rel r = []'));
different(scene('rel r = []'), scene('prepend rel r = []'));
different(scene('prepend rel r = []'), scene('append rel r = []'));
different(scene('prepend rel r = </A>\nappend rel r = </B>'),
          scene('prepend rel r = </C>\nappend rel r = </B>'));
different(scene('float[] a = None'), scene('float[] a = []'));
different(scene('float[] a.timeSamples = {0: None}'),
          scene('float[] a.timeSamples = {0: []}'));
different(scene('', 'customField = None'), scene('', 'customField = []'));
equal(scene('custom rel r = None'), scene('rel r = []'));
equal(scene('rel custom = None'), scene('rel custom = []'));
different(scene('rel custom = None'), scene('rel r = []'));
different(scene('', 'apiSchemas = "None"'), scene('', 'apiSchemas = []'));
console.log('compare-usda list-op tests passed');
