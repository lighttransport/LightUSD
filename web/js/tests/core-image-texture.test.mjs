// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import * as THREE from 'three';
import {createDecodedImageTexture,hasImageBytes} from '../src/lightusd/CoreImageTexture.js';
const make=(data,componentType,channels=3)=>createDecodedImageTexture({
  data:new Uint8Array(data.buffer,data.byteOffset,data.byteLength),componentType,
  width:1,height:1,channels,decoded:true,colorSpace:1});
const rgb=new Uint8Array([10,20,30]);
const byte=make(rgb,0);
assert.deepEqual([...byte.image.data],[10,20,30,255]);
assert.equal(byte.type,THREE.UnsignedByteType);
rgb.fill(0);assert.equal(byte.image.data[0],10);
const hdr=make(new Float32Array([2.5,-0.5,4]),7);
assert.equal(hdr.type,THREE.FloatType);
assert.deepEqual([...hdr.image.data],[2.5,-0.5,4,1]);
const half=make(new Uint16Array([0x4000,0x3800,0x3c00]),6);
assert.equal(half.type,THREE.HalfFloatType);
assert.deepEqual([...half.image.data],[0x4000,0x3800,0x3c00,0x3c00]);
const u16=make(new Uint16Array([65535,32768,0]),2);
assert.equal(u16.type,THREE.FloatType);
assert.deepEqual([...u16.image.data],[1,Math.fround(32768/65535),0,1]);
assert.deepEqual([...make(new Int16Array([-32768,0,32767]),3).image.data],[-1,0,1,1]);
assert.equal(make(new Uint8Array([12]),0,1).format,THREE.RedFormat);
assert.equal(make(new Uint8Array([12,34]),0,2).format,THREE.RGFormat);
assert.throws(()=>make(new Uint8Array([12]),0),/Truncated/);
assert.throws(()=>make(new Float64Array([Infinity,0,0]),8),/Non-finite/);
assert.equal(hasImageBytes({data:new Uint8Array(1)}),true);
assert.equal(hasImageBytes({data:[1]}),true);
assert.equal(hasImageBytes({data:[]}),false);
for(const texture of [byte,hdr,half,u16])texture.dispose();
console.log('PASS: core owned RGB, 16-bit, half/float HDR texture data and channel formats');
