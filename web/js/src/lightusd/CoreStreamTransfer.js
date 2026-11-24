// SPDX-License-Identifier: Apache-2.0
// Transfer chunks into a retained core stream. The caller owns successful
// results; failed transfers release only streams created by this helper.
export async function transferCoreChunks(loader, chunks, assetPath, totalSize, options={}) {
  const limitMB=options.maxMemoryLimitMB??loader.maxMemoryLimitMB_??512;
  if(!Number.isInteger(limitMB)||limitMB<1||limitMB>8192)throw RangeError('Invalid memory limit');
  const limit=Math.min(limitMB*1024*1024,0x40000000);
  const checkAbort=()=>{if(options.signal?.aborted)throw new DOMException('Stream transfer aborted','AbortError');};
  checkAbort();
  if(totalSize!==null&&(!Number.isSafeInteger(totalSize)||totalSize<0||totalSize>limit))throw RangeError('Streaming input exceeds memory limit');
  const owned=!options.usdInstance;
  const stream=options.usdInstance||new loader.native_.RenderStream();
  loader.activeCoreTransfers_??=new Set();
  loader.activeCoreTransfers_.add(stream);
  let started=false,success=false;
  try {
    if(owned)stream.setMaxMemoryLimitMB(limitMB);
    let transferred=0;
    const pending=[];
    if(totalSize!==null){if(!stream.startStreamingAsset(assetPath,totalSize))throw Error(stream.error());started=true;}
    for await(const chunk of chunks) {
      checkAbort();
      const bytes=new Uint8Array(chunk.buffer,chunk.byteOffset,chunk.byteLength);
      if(bytes.length>limit-transferred)throw RangeError('Streaming input exceeds memory limit');
      if(totalSize!==null) {
        if(bytes.length>totalSize-transferred)throw RangeError('Response exceeds declared content length');
        if(bytes.length&&!stream.appendStreamingAsset(assetPath,bytes))throw Error('Failed to append streaming bytes');
      }else pending.push(bytes.slice());
      transferred+=bytes.length;
      options.onProgress?.(transferred,totalSize??0);
    }
    checkAbort();
    if(totalSize!==null&&transferred!==totalSize)throw Error('Truncated streaming input');
    if(totalSize===null) {
      if(!stream.startStreamingAsset(assetPath,transferred))throw Error(stream.error());started=true;
      for(const bytes of pending)if(bytes.length&&!stream.appendStreamingAsset(assetPath,bytes))throw Error('Failed to append streaming bytes');
    }
    // Keep the completed buffer available for beginStreamedAsset(). Native
    // finalization instead moves it into the dependency cache.
    success=true;
    return {success:true,bytesTransferred:transferred,assetPath,usdInstance:stream};
  }finally {
    loader.activeCoreTransfers_.delete(stream);
    if(!success){try{if(started)stream.cancelStreamingAsset(assetPath);}finally{if(owned)stream.delete();}}
  }
}

export async function* responseChunks(response) {
  if(!response.body){yield new Uint8Array(await response.arrayBuffer());return;}
  const reader=response.body.getReader();
  let complete=false;
  try{for(;;){const {done,value}=await reader.read();if(done){complete=true;break;}yield value;}}
  finally{if(!complete)await reader.cancel().catch(()=>{});reader.releaseLock();}
}
