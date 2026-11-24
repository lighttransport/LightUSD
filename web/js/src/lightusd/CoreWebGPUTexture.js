// SPDX-License-Identifier: Apache-2.0
import {DataUtils,UnsignedByteType,HalfFloatType,SRGBColorSpace} from 'three';
import {createDecodedImageTexture} from './CoreImageTexture.js';

// Own RGBA pixels suitable for a filterable WebGPU texture. Preserve HDR in
// half floats, and normalize integer source formats through the shared codec.
export function coreWebGPUImage(image) {
  const texture=createDecodedImageTexture(image);
  try {
    const source=texture.image.data,channels=source.length/(image.width*image.height);
    const byte=texture.type===UnsignedByteType;
    const pixels=byte?new Uint8Array(image.width*image.height*4):new Uint16Array(image.width*image.height*4);
    const one=byte?255:0x3c00;
    for(let i=0;i<image.width*image.height;i++) {
      const value=c=>{
        const v=source[i*channels+c];
        return byte||texture.type===HalfFloatType?v:DataUtils.toHalfFloat(v);
      };
      pixels[i*4]=value(0);
      pixels[i*4+1]=value(channels<=2?0:1);
      pixels[i*4+2]=value(channels<=2?0:2);
      pixels[i*4+3]=channels===2?value(1):channels===4?value(3):one;
    }
    return {data:pixels,width:image.width,height:image.height,
      format:byte?(texture.colorSpace===SRGBColorSpace?'rgba8unorm-srgb':'rgba8unorm'):'rgba16float',
      bytesPerPixel:byte?4:8};
  }finally{texture.dispose();}
}
