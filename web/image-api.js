// SPDX-License-Identifier: Apache-2.0
// Bounded image-job API, split out of core-api.js.
// Loaded only by the core module.
(() => {
  if (!Module.__lightusdApiInternals) return;
  // Synchronous, counted image jobs. Every returned byte array owns its memory.
  const imageJobEncoder = new TextEncoder(), imageJobDecoder = new TextDecoder();
  function imageBytes(value) {
    if (value instanceof ArrayBuffer) return new Uint8Array(value);
    if (!ArrayBuffer.isView(value)) throw new TypeError('Image data must be an ArrayBuffer or byte view');
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    return bytes.buffer === Module.HEAPU8.buffer ? bytes.slice() : bytes;
  }
  function imageCall(parts, callback) {
    const sizes = parts.map(part => part.byteLength);
    const total = sizes.reduce((a,b) => a+b,0);
    if (total > 0x20000000) throw new RangeError('Image bridge input exceeds 512 MiB');
    let block = 0;
    try {
      block = Module['_lightusd_wasm_alloc'](Math.max(total,1));
      if (!block) throw new RangeError('Image bridge allocation failed');
      const offsets = [];let offset = 0;
      for (const part of parts) {offsets.push(offset);Module.HEAPU8.set(part,Number(block)+offset);offset+=part.byteLength;}
      const run = p => callback(index => p(Number(block)+offsets[index]),sizes);
      try { return run(x => x); }
      catch(error) {if (!(error instanceof TypeError)) throw error;return run(x => BigInt(x));}
    } finally {if (block)Module['_lightusd_wasm_free'](block);}
  }
  function imageJSON(options) {
    if (!options || typeof options !== 'object' || Array.isArray(options)) throw new TypeError('Image options must be an object');
    const bytes = imageJobEncoder.encode(JSON.stringify(options));
    if(bytes.length>65536)throw new RangeError('Image options exceed 64 KiB');
    return bytes;
  }
  function imageInfo(handle) {
    let buffer = 0;
    try {
      const call = (p,cap) => {
        try {return Module['_lightusd_image_job_info'](handle,p,cap);}
        catch(error){if(!(error instanceof TypeError))throw error;return Module['_lightusd_image_job_info'](handle,BigInt(p),cap);}
      };
      const size = call(0,0);if(size<0)throw new RangeError('Invalid image job');
      buffer=Module['_lightusd_wasm_alloc'](size);
      if(!buffer)throw new RangeError('Image result allocation failed');
      if(call(buffer,size)!==size)throw new RangeError('Image result changed');
      return JSON.parse(imageJobDecoder.decode(Module.HEAPU8.subarray(Number(buffer),Number(buffer)+size)));
    }finally{if(buffer)Module['_lightusd_wasm_free'](buffer);}
  }
  function imageOutput(handle,index) {
    const size=Module['_lightusd_image_job_size'](handle,index);
    const pointer=Number(Module['_lightusd_image_job_data'](handle,index));
    if(size<0 || !pointer || pointer+size>Module.HEAPU8.length)throw new RangeError('Invalid image output span');
    return Module.HEAPU8.slice(pointer,pointer+size);
  }
  Module['convertImage']=(data,options={}) => {
    const handle=imageCall([imageBytes(data),imageJSON(options)],(p,sizes) =>
      Module['_lightusd_image_convert'](p(0),sizes[0],p(1),sizes[1]));
    if(!handle)return {success:false,error:'Image job limit/allocation failure'};
    try {const info=imageInfo(handle);if(info.success)info.data=imageOutput(handle,0);delete info.results;return info;}
    finally{Module['_lightusd_image_job_release'](handle);}
  };
  Module['fitTextures']=options => {
    if(!options || !Array.isArray(options.images))return {success:false,error:'fitTextures: missing images'};
    const {images,...config}=options;
    if(images.length>4096)throw new RangeError('Texture fit input count exceeds 4096');
    const encoded=imageJSON(config),handle=Module['_lightusd_image_fit_begin']();
    if(!handle)return {success:false,error:'Image job limit/allocation failure'};
    try {
      for(const image of images) {
        const added=imageCall([imageBytes(image.data),imageJobEncoder.encode(String(image.name || 'image.png'))],(p,sizes) =>
          Module['_lightusd_image_fit_add'](handle,p(0),sizes[0],p(1),sizes[1]));
        if(!added)return imageInfo(handle);
      }
      imageCall([encoded],(p,sizes) => Module['_lightusd_image_fit_finish'](handle,p(0),sizes[0]));
      const info=imageInfo(handle);
      if(info.success)info.results.forEach((result,index) => {result.data=imageOutput(handle,index);});
      return info;
    }finally{Module['_lightusd_image_job_release'](handle);}
  };
  Module['repackChannels']=(options={}) => {
    const config={...options};const inputs=[];let inferred=0;
    for(const [index,key] of ['r','g','b','a'].entries()) {
      const slot=options[key];if(slot==null){delete config[key];continue;}
      inferred=index+1;config[key]={channel:slot.channel??0,const:slot.const??(index===3?255:0)};
      if(slot.data!=null){config[key].input=inputs.length;inputs.push({data:slot.data,name:key+'.png'});}
    }
    config.channels=options.channels || inferred || 4;
    const encoded=imageJSON(config),handle=Module['_lightusd_image_fit_begin']();
    if(!handle)return {success:false,error:'Image job limit/allocation failure'};
    try {
      for(const image of inputs) {
        const added=imageCall([imageBytes(image.data),imageJobEncoder.encode(image.name)],(p,sizes) =>
          Module['_lightusd_image_fit_add'](handle,p(0),sizes[0],p(1),sizes[1]));
        if(!added)return imageInfo(handle);
      }
      imageCall([encoded],(p,sizes) => Module['_lightusd_image_repack_finish'](handle,p(0),sizes[0]));
      const info=imageInfo(handle);if(info.success)info.data=imageOutput(handle,0);delete info.results;return info;
    }finally{Module['_lightusd_image_job_release'](handle);}
  };
  Module['getTextureColorspaceMap']=data => {
    const handle=imageCall([imageBytes(data)],(p,sizes) => Module['_lightusd_image_colorspaces'](p(0),sizes[0]));
    if(!handle)throw new RangeError('Image job limit/allocation failure');
    try {const info=imageInfo(handle);if(!info.success)throw new Error(info.error);return info.map;}
    finally{Module['_lightusd_image_job_release'](handle);}
  };
  Module['getTextureColorspaceMap'].fullPaths=true;

})();
