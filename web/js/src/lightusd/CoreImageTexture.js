// SPDX-License-Identifier: Apache-2.0
import * as THREE from 'three';

export function hasImageBytes(image) {
  return !!image?.data && (image.data.byteLength ?? image.data.length ?? 0) > 0;
}

// RenderStream image copies own raw component bytes, without a legacy bufferId.
export function createDecodedImageTexture(image) {
  const {width,height,channels}=image;
  if(!image.decoded || !Number.isSafeInteger(width) || !Number.isSafeInteger(height) ||
      width<=0 || height<=0 || ![1,2,3,4].includes(channels))throw Error('Invalid decoded image dimensions');
  const count=width*height*channels;
  const component=image.componentType??0;
  const sizes=[1,1,2,2,4,4,2,4,8];
  const size=sizes[component];
  if(!size || !Number.isSafeInteger(count*size))throw Error('Invalid image component type or size');
  const source=ArrayBuffer.isView(image.data)
    ?new Uint8Array(image.data.buffer,image.data.byteOffset,image.data.byteLength)
    :Uint8Array.from(image.data||[]);
  if(source.byteLength<count*size)throw Error('Truncated decoded image pixels');
  const bytes=source.slice(0,count*size); // Own, aligned storage for typed views.
  const views=[Uint8Array,Int8Array,Uint16Array,Int16Array,Uint32Array,Int32Array,Uint16Array,Float32Array,Float64Array];
  let pixels=new views[component](bytes.buffer);
  let type=component===0?THREE.UnsignedByteType:component===6?THREE.HalfFloatType:THREE.FloatType;
  if(component!==0 && component!==6 && component!==7) {
    const divisors=[255,127,65535,32767,4294967295,2147483647,1,1,1];
    const values=new Float32Array(count);
    for(let i=0;i<count;i++) {
      values[i]=component===8?pixels[i]:Math.max(-1,pixels[i]/divisors[component]);
      if(!Number.isFinite(values[i]))throw Error('Non-finite decoded image component');
    }
    pixels=values;
  }
  if(channels===3) {
    const expanded=new pixels.constructor(width*height*4);
    const alpha=type===THREE.UnsignedByteType?255:type===THREE.HalfFloatType?0x3c00:1;
    for(let i=0;i<width*height;i++) {
      expanded.set(pixels.subarray(i*3,i*3+3),i*4);expanded[i*4+3]=alpha;
    }
    pixels=expanded;
  }
  const format=channels===1?THREE.RedFormat:channels===2?THREE.RGFormat:THREE.RGBAFormat;
  const texture=new THREE.DataTexture(pixels,width,height,format,type);
  texture.flipY=true;
  texture.colorSpace=image.colorSpace===0?THREE.SRGBColorSpace:
    image.colorSpace===1?THREE.LinearSRGBColorSpace:THREE.NoColorSpace;
  texture.userData.lightusdImageColorSpace=image.colorSpace;
  texture.needsUpdate=true;
  return texture;
}
