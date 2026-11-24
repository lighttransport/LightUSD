// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {PNG} from 'pngjs';
import {loadWasm, convertFolderToUSDZ, convertSourceToUSDZStreaming, unpackUSDZ} from '../src/usdzconvert.js';
const suffix = process.env.LIGHTUSD_WASM64 === '1' ? '_64' : '';
const native = await loadWasm(() => import(`../src/lightusd/lightusd${suffix}.js`), {
  locateFile:file => new URL('../src/lightusd/' + file, import.meta.url).pathname,
});
const png = new PNG({width:16,height:8});
for (let i=0;i<png.data.length;i++) png.data[i]=(i*17)%256;
const image=PNG.sync.write(png);
const root=new TextEncoder().encode('#usda 1.0\ndef Xform "Root" {def Shader "Texture" {uniform token info:id="UsdUVTexture"; asset inputs:file=@tex.png@}}');
for (const streaming of [false,true]) for (const rootLayerFormat of ['usda','usdc']) {
  const input=new Map([['root.usda',root],['tex.png',image]]);
  const options={rootPath:'root.usda',rootLayerFormat,reencode:true,textureFormat:'jpeg',maxTextureSize:8};
  const result=streaming ? await convertSourceToUSDZStreaming(native,{
    keys:[...input.keys()],fetch:async key=>input.get(key),
  },options) : await convertFolderToUSDZ(native,input,options);
  const archive=unpackUSDZ(result.usdz);
  const jpeg=[...archive.entries].find(([name])=>/\.jpe?g$/.test(name));
  assert.ok(jpeg,[...archive.entries.keys()].join(','));
  assert.equal(jpeg[1][0],255); assert.equal(jpeg[1][1],216);
  assert.ok(!archive.entries.has('tex.png'));
  const reader=new native.USDZConverterNative();
  try {
    assert.equal(reader.loadFromBinary(result.usdz,'result.usdz'),true,reader.error());
    assert.ok(reader.exportAsUSDA().includes('@'+jpeg[0]+'@'));
  } finally {reader.delete();}
}
// Global fitting and same-stem format conversion must retain both assets.
for (const streaming of [false,true]) for (const strategy of ['size','quality']) {
  const jpg=native.convertImage(image,{format:'jpeg'}).data;
  const input=new Map([['root.usda',root],['tex.png',image],['tex.jpg',jpg]]);
  const options={rootPath:'root.usda',textureFormat:'jpeg',targetTextureBytes:1500,
    fitStrategy:strategy,fitMinTextureSize:1,fitMinQuality:10};
  const result=streaming ? await convertSourceToUSDZStreaming(native,{
    keys:[...input.keys()],fetch:async key=>input.get(key),
  },options) : await convertFolderToUSDZ(native,input,options);
  const archive=unpackUSDZ(result.usdz);
  const textures=[...archive.entries].filter(([name])=>/\.(png|jpe?g)$/.test(name));
  assert.equal(textures.length,2);
  assert.equal(new Set(textures.map(([name])=>name)).size,2);
  assert.equal(result.stats.fitTotalBytes,textures.reduce((n,[,bytes])=>n+bytes.length,0));
}
// ARKit policy forces composition, a USDC root, Y-up and PNG/JPEG textures.
const bmp = new Uint8Array(70), header = new DataView(bmp.buffer);
bmp.set([66,77]);header.setUint32(2,70,true);header.setUint32(10,54,true);
header.setUint32(14,40,true);header.setInt32(18,2,true);header.setInt32(22,2,true);
header.setUint16(26,1,true);header.setUint16(28,24,true);header.setUint32(34,16,true);
for(const offset of [54,57,62,65])bmp.set([0,0,255],offset);
const arkitRoot=new TextEncoder().encode('#usda 1.0\n(upAxis="Z")\ndef Xform "Root" (references=@../deps/model.usda@</Asset>) {}\n');
const arkitLayer=new TextEncoder().encode('#usda 1.0\ndef Xform "Asset" { def Shader "Tex" { uniform token info:id="UsdUVTexture"; asset inputs:file=@tex.bmp@; } }\n');
for(const streaming of [false,true])for(const fitStrategy of [null,'size','quality']) {
 const input=new Map([['nested/root.usda',arkitRoot],['deps/model.usda',arkitLayer],['deps/tex.bmp',bmp]]);
 const options={rootPath:'nested/root.usda',rootLayerFormat:'usda',flatten:false,
  arkitCompatible:true,reencode:false,...(fitStrategy?{fitStrategy,targetTextureBytes:1000,fitMinTextureSize:1}:{} )};
 const result=streaming?await convertSourceToUSDZStreaming(native,{keys:[...input.keys()],fetch:async key=>input.get(key)},options)
  :await convertFolderToUSDZ(native,input,options);
 assert.equal(result.stats.flatten,true);assert.equal(result.stats.arkitCompatible,true);
 const archive=unpackUSDZ(result.usdz);
 assert.equal(archive.order[0],'root.usdc');
 assert.equal([...archive.entries.keys()].filter(name=>/\.usd[ac]?$/.test(name)).length,1);
 const texture=[...archive.entries].find(([name])=>/\.(png|jpe?g)$/.test(name));assert.ok(texture);
 assert.ok(!archive.entries.has('deps/tex.bmp'));
 const decoded=native.convertImage(texture[1],{format:'png'});assert.equal(decoded.success,true,decoded.error);
 const reader=new native.USDZConverterNative();
 try {
  assert.ok(reader.loadFromBinary(result.usdz,'arkit.usdz'),reader.error());
  const text=reader.exportAsUSDA();assert.match(text,/upAxis = "Y"/);assert.ok(text.includes('@'+texture[0]+'@'));
 }finally{reader.delete();}
}
console.log('Core converter texture integration passed');
