#!/usr/bin/env node

const assert = require('node:assert/strict');
const path = require('node:path');
const {
  compareUsda,
  parseUsda,
  resolveAssetPathForCompare,
} = require('../compare-usda.js');

const anchor = path.resolve('/tmp/lightusd-asset-anchor');
const relative = parseUsda(`#usda 1.0
def Shader "S"
{
    asset inputs:file = @textures/a.png@
    asset[] inputs:files = [@textures/a.png@, @pack/look.usdz[inside.usda]@]
    dictionary inputs:manifest = {
        asset primary = @textures/a.png@
    }
    asset inputs:animated.timeSamples = {
        0: @textures/a.png@,
        1: @textures/b.png@
    }
}
`);
const absolute = parseUsda(`#usda 1.0
def Shader "S"
{
    asset inputs:file = @${path.join(anchor, 'textures/a.png')}@
    asset[] inputs:files = [@${path.join(anchor, 'textures/a.png')}@, @${path.join(anchor, 'pack/look.usdz')}[inside.usda]@]
    dictionary inputs:manifest = {
        asset primary = @${path.join(anchor, 'textures/a.png')}@
    }
    asset inputs:animated.timeSamples = {
        0: @${path.join(anchor, 'textures/a.png')}@,
        1: @${path.join(anchor, 'textures/b.png')}@
    }
}
`);

assert.equal(compareUsda(relative, absolute).length, 4,
  'authored identifiers remain exact by default');
assert.deepEqual(compareUsda(relative, absolute, {
  resolveAssetPaths: true,
  assetPathBase1: anchor,
  assetPathBase2: anchor,
}), [], 'scalar, array, package and time-sampled assets resolve equivalently');

assert.notDeepEqual(compareUsda(relative, absolute, {
  resolveAssetPaths: true,
  assetPathBase1: path.join(anchor, 'different'),
  assetPathBase2: anchor,
}), [], 'different source-layer anchors must not compare equal');

assert.equal(resolveAssetPathForCompare('https://example.com/a.png', anchor),
  'https://example.com/a.png', 'resolver schemes are not filesystem-resolved');

console.log('compare-usda resolved asset-path tests passed');
