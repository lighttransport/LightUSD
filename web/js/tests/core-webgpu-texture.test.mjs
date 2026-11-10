// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {DataUtils} from 'three';
import {coreWebGPUImage} from '../src/lightusd/CoreWebGPUTexture.js';
const image=(channels,data,componentType=0)=>({width:1,height:1,channels,data,componentType,decoded:true,colorSpace:0});
const gray=coreWebGPUImage(image(1,new Uint8Array([128])));
assert.deepEqual(Array.from(gray.data),[128,128,128,255]);assert.equal(gray.format,'rgba8unorm-srgb');
const alpha=coreWebGPUImage(image(2,new Uint8Array([128,64])));
assert.deepEqual(Array.from(alpha.data),[128,128,128,64]);
const hdr=coreWebGPUImage(image(3,new Float32Array([0.25,2.5,4]),7));
assert.equal(hdr.format,'rgba16float');assert.equal(hdr.bytesPerPixel,8);
assert.deepEqual(Array.from(hdr.data,DataUtils.fromHalfFloat),[0.25,2.5,4,1]);
const half=coreWebGPUImage(image(4,hdr.data,6));assert.deepEqual(half.data,hdr.data);
assert.throws(()=>coreWebGPUImage(image(4,new Uint8Array([1]))),/Truncated/);
console.log('PASS core WebGPU texture normalization, grayscale/alpha and HDR uploads');
