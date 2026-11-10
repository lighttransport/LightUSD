// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {PNG} from 'pngjs';
import {loadWasm} from '../src/usdzconvert.js';
const suffix=process.env.LIGHTUSD_WASM64==='1'?'_64':'';
const native=await loadWasm(()=>import(`../src/lightusd/lightusd${suffix}.js`),{
  locateFile:file=>new URL('../src/lightusd/'+file,import.meta.url).pathname,
});
// Encode through the existing public image API, then exercise the new counted
// conversion/fit/repack bridge with borrowed subviews and owned result buffers.
const pixels=new Uint8Array(16*8*4);
for(let y=0;y<8;y++)for(let x=0;x<16;x++){
 const i=(y*16+x)*4;pixels.set([x*17,y*31,128,200],i);
}
const png=new Uint8Array(PNG.sync.write({width:16,height:8,data:Buffer.from(pixels)}));
const wrapped=new Uint8Array(png.length+10);wrapped.set(png,5);
const borrowed=wrapped.subarray(5,5+png.length);
for(const lowMemory of [0,1]) {
  const converted=native.convertImage(borrowed,{format:'png',maxSize:4,lowMemory});
  assert.equal(converted.success,true,converted.error);
  assert.equal(converted.width,4);assert.equal(converted.height,2);assert.equal(converted.resized,true);
  assert.notEqual(converted.data.buffer,native.HEAPU8.buffer);
  assert.equal(PNG.sync.read(Buffer.from(converted.data)).width,4);
}
// Auto-stream a compressible source whose decoded pixels exceed the budget.
const largePNG=new Uint8Array(PNG.sync.write({width:2048,height:2048,data:Buffer.alloc(2048*2048*4,127)}));
const streamed=native.convertImage(largePNG,{format:'png',maxSize:64,memoryBudgetBytes:8*1024*1024});
assert.equal(streamed.success,true,streamed.error);
assert.equal(PNG.sync.read(Buffer.from(streamed.data)).width,64);
const jpeg=native.convertImage(png,{format:'jpeg',maxSize:8,jpegQuality:75});
assert.equal(jpeg.success,true,jpeg.error);assert.deepEqual([...jpeg.data.subarray(0,2)],[255,216]);
assert.equal(native.convertImage(png,{memoryBudgetBytes:64}).success,false);
assert.equal(native.convertImage(png,{width:1e9}).success,false);
assert.equal(native.convertImage(new Uint8Array([1,2,3]),{}).success,false);
const halves=new Uint16Array([257,258,259,65535,50001,50002,50003,65535]);
const png16=new Uint8Array(PNG.sync.write({width:2,height:1,data:halves},{bitDepth:16,inputColorType:6,colorType:6}));
const sixteen=native.convertImage(png16,{format:'png'});
assert.equal(sixteen.success,true,sixteen.error);assert.equal(sixteen.data[24],16);
assert.deepEqual([...PNG.sync.read(Buffer.from(sixteen.data),{skipRescale:true}).data],[...halves]);
const resized16=native.convertImage(png16,{format:'png',width:1,height:1});
assert.equal(resized16.success,true,resized16.error);assert.equal(resized16.data[24],16);
const blackWhite=new Uint8Array(PNG.sync.write({width:2,height:1,data:Buffer.from([0,0,0,255,255,255,255,255])}));
for(const lowMemory of [0,1]) {
 const linear=native.convertImage(blackWhite,{format:'png',width:1,height:1,resizeColorspace:'linear',lowMemory});
 const srgb=native.convertImage(blackWhite,{format:'png',width:1,height:1,resizeColorspace:'srgb',lowMemory});
 assert.equal(linear.success,true,linear.error);assert.equal(srgb.success,true,srgb.error);
 assert.ok(PNG.sync.read(Buffer.from(srgb.data)).data[0]>PNG.sync.read(Buffer.from(linear.data)).data[0]+30);
}
const packed=native.repackChannels({width:2,height:1,r:{data:png16,channel:0},g:{const:7},b:{const:9},a:{const:255}});
// uint16 channel packing is intentionally rejected by the existing 8-bit pack API.
assert.equal(packed.success,false);
const packed8=native.repackChannels({width:16,height:8,r:{data:png,channel:2},g:{const:7},b:{const:9},a:{const:255}});
assert.equal(packed8.success,true,packed8.error);
assert.deepEqual([...PNG.sync.read(Buffer.from(packed8.data)).data.subarray(0,4)],[128,7,9,255]);
for(const strategy of ['size','quality']) {
 const fit=native.fitTextures({images:[{data:png,name:'a.png'},{data:png,name:'b.png'}],targetBytes:500,
   strategy,minTextureSize:1,minQuality:10,startMaxSize:4});
 assert.equal(fit.success,true,fit.error);assert.equal(fit.results.length,2);
 assert.equal(fit.totalBytes,fit.results.reduce((n,result)=>n+result.data.length,0));
 assert.ok(fit.results.every(result=>result.data.buffer!==native.HEAPU8.buffer));
}
const source=new TextEncoder().encode('#usda 1.0\ndef Shader "Color" {uniform token info:id="UsdUVTexture";asset inputs:file=@red/tex.png@;token inputs:sourceColorSpace="sRGB"; }\ndef Shader "Data" {uniform token info:id="UsdUVTexture";asset inputs:file=@blue/tex.png@;token inputs:sourceColorSpace="raw"; }');
assert.deepEqual(native.getTextureColorspaceMap(source),{'red/tex.png':'sRGB','blue/tex.png':'raw'});
const ktx=fs.readFileSync(new URL('../../../models/ktx2-uni-plane.ktx2',import.meta.url));
const decodedKTX=native.convertImage(ktx,{format:'png'});
assert.equal(decodedKTX.success,true,decodedKTX.error);
assert.ok(PNG.sync.read(Buffer.from(decodedKTX.data)).width>0);
const exr=await native.loadFeature('exr');
const hdr=exr.encode(new Uint8Array(new Float32Array([4,2,1,0.5,8,4,2,1]).buffer),2,1,4,32);
const kept=native.convertImage(hdr,{format:'exr',width:1,height:1});
assert.equal(kept.success,true,kept.error);assert.equal(exr.decodeEXR(kept.data).width,1);
const ldr=native.convertImage(hdr,{format:'png'});
assert.equal(ldr.success,true,ldr.error);assert.equal(PNG.sync.read(Buffer.from(ldr.data)).data[3],128);
const fitted=native.fitTextures({images:[{name:'hdr.exr',data:hdr}],targetBytes:1,strategy:'quality',minTextureSize:1,minQuality:10});
assert.equal(fitted.success,true,fitted.error);
assert.ok(fitted.results[0].data.length>0);
console.log('PASS: counted core image conversion, uint16, HDR, colorspaces, packing, fits and bounded errors');
