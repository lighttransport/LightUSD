// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {LightUSDComposer} from '../src/lightusd/LightUSDComposer.js';
const {default:create} = await import(process.env.LIGHTUSD_WASM64 === '1'
  ? '../src/lightusd/lightusd_64.js' : '../src/lightusd/lightusd.js');
const native = await create();
const enc = text => new TextEncoder().encode(text);
const root = `#usda 1.0
( subLayers = [@layers/base.usda@] )
def Xform "Ref" (references = @refs/ref.usda@</Asset>) {}
def Xform "Payload" (payload = @payload.usda@</Asset>) {}
`;
const assets = new Map([
 ['layers/base.usda',enc('#usda 1.0\n( subLayers=[@nested.usda@] )\ndef Scope "Base" {}')],
 ['layers/nested.usda',enc('#usda 1.0\ndef Scope "Nested" {}')],
 ['refs/ref.usda',enc('#usda 1.0\ndef Xform "Asset" { float weight=2 }')],
 ['payload.usda',enc('#usda 1.0\ndef Xform "Asset" { float payloadValue=3 }')],
]);
for (const payload of [false,true]) {
 const layer = new native.LayerDocument();
 try {
  assert.equal(layer.load(enc(root)).success,true);
  const composer = new LightUSDComposer();composer.setLayer(layer);composer.setUSDLoader({native_:native});
  assert.equal(composer.getUSDLoader().native_,native);
  assert.equal(LightUSDComposer.hasSublayer(layer),true);
  assert.deepEqual(LightUSDComposer.extractReferencesAssetPaths(layer),['refs/ref.usda']);
  assert.deepEqual(LightUSDComposer.extractPayloadAssetPaths(layer),['payload.usda']);
  const requests=[];
  composer.setAssetResolver({async resolveAsync(key){requests.push(key);const clean=key.replace(/^\.\//,'');if(!assets.has(clean))throw Error(`Missing ${key}`);return [key,assets.get(clean)];}});
  assert.equal(await composer.progressiveComposition({composePayload:payload}),layer);
  const text=layer.exportUSDA().text;
  assert.match(text,/Nested/);assert.match(text,/weight = 2/);
  assert.equal(text.includes('payloadValue'),payload);
  assert.equal(LightUSDComposer.hasPayload(layer),!payload);
  assert.equal(requests.some(x=>x.endsWith('payload.usda')),payload);
  assert.equal(LightUSDComposer.hasReferences(layer),false);
 } finally {layer.delete();}
}
{
 const layer=new native.LayerDocument();
 try {
  layer.load(enc(root));const composer=new LightUSDComposer();composer.setLayer(layer);composer.setUSDLoader({native_:native});
  for(const [key,value]of assets)composer.getAssetMap().set(key,value);
  await composer.resolveSublayerAssets(0);
  assert.equal(LightUSDComposer.composeSublayer(layer),true);
  assert.equal(LightUSDComposer.hasSublayer(layer),false);
  assert.equal(LightUSDComposer.hasReferences(layer),true);
  assert.equal(LightUSDComposer.composeReferences(layer),true);
  assert.equal(LightUSDComposer.hasReferences(layer),false);
  assert.equal(LightUSDComposer.hasPayload(layer),true);
 }finally{layer.delete();}
}
{
 const layer=new native.LayerDocument();
 try {
  const load = layer.load(enc(`#usda 1.0
class Xform "Base" { float inherited = 4; }
def Xform "Item" (
 inherits = </Base>
 variants = { string look = "blue" }
 prepend variantSets = "look"
) {
 variantSet "look" = { "blue" { float tint = 5; } }
}
`));
  assert.equal(load.success,true,load.error);
  const composer=new LightUSDComposer();composer.setLayer(layer);composer.setUSDLoader({native_:native});
  assert.equal(LightUSDComposer.hasInherits(layer),true);
  assert.equal(LightUSDComposer.hasVariants(layer),true);
  assert.equal(LightUSDComposer.composeInherits(layer),true);
  assert.equal(LightUSDComposer.hasInherits(layer),false);
  assert.equal(LightUSDComposer.hasVariants(layer),true);
  assert.equal(LightUSDComposer.composeVariants(layer),true);
  assert.equal(LightUSDComposer.hasVariants(layer),false);
  assert.match(layer.exportUSDA().text,/tint = 5/);
  assert.match(layer.exportUSDA().text,/inherited = 4/);
 }finally{layer.delete();}
}
{
 const session=new native.FlattenSession();
 try {
  assert.throws(()=>session.setCompositionMask(63),/session not started/);
  for(const value of [-1,64,1.5,'1',null])assert.throws(()=>session.setCompositionMask(value),/integer/);
 }finally{session.delete();}
 const layer=new native.LayerDocument();
 try {
  layer.load(enc(root));const composer=new LightUSDComposer();composer.setLayer(layer);composer.setUSDLoader({native_:native});
  composer.setAssetSearchPaths([]);composer.setAssetResolver({async resolveAsync(){throw Error('fixture missing');}});
  await assert.rejects(composer.progressiveComposition(),/fixture missing/);
  assert.equal(LightUSDComposer.hasSublayer(layer),true);
 }finally{layer.delete();}
}
console.log('PASS core public composer: nested anchors, references, optional payloads, prefetch, partial composition and failures');
