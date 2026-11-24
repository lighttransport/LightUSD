// SPDX-License-Identifier: Apache-2.0
import {parseUSDZEntries,isImageName} from '../usdzconvert.js';

// Retain a core RenderStream for tools that query materials, images and meshes
// interactively. The caller owns the returned stream and must delete it.
export async function createCoreRenderInspector(native, input, filename='', options={}) {
  const bytes=ArrayBuffer.isView(input)
    ?new Uint8Array(input.buffer,input.byteOffset,input.byteLength):new Uint8Array(input);
  if(options.omitTexturePayloads && options.loadTextures!==false)
    throw TypeError('Omitting texture payloads requires off-heap texture loading');
  const limit=options.maxMemoryLimitMB??512;
  const stream=new native.RenderStream();
  let store;
  try {
    stream.setMaxMemoryLimitMB(limit);
    stream.setLoadTextureInNative(options.loadTextures!==false);
    let root=bytes;
    let sourceURI=filename;
    if(bytes.length>=4 && bytes[0]===0x50 && bytes[1]===0x4b && bytes[2]===3 && bytes[3]===4) {
      const entries=parseUSDZEntries(bytes);
      if(options.loadTextures!==false && entries.some(entry=>/\.exr$/i.test(entry.name))) {
        if(typeof native.loadFeature!=='function')throw Error('EXR textures require the core EXR feature');
        await native.loadFeature('exr');
      }
      const layer=entries[0];
      if(!layer||!/\.(usd|usda|usdc)$/i.test(layer.name))throw Error('USDZ has no root layer');
      store=new native.AssetStore();
      store.setMemoryLimitBytes(Math.min(limit*1024*1024,1024*1024*1024));
      const slash=layer.name.lastIndexOf('/');
      if(slash>=0)store.setBaseWorkingPath(layer.name.slice(0,slash));
      for(const entry of entries) {
        if(!entry.name.endsWith('/'))store.registerMemoryAsset(entry.name,options.omitTexturePayloads&&isImageName(entry.name)?new Uint8Array():entry.data);
      }
      stream.importAssetStore(store);
      root=layer.data;
      sourceURI=layer.name;
    }
    const result=stream.begin(root,sourceURI);
    if(!result.success)throw Error(result.error||`Failed to parse ${filename}`);
    return stream;
  } catch(error) {
    stream.delete();
    throw error;
  } finally {
    store?.delete();
  }
}

// Keep inspector geometry/material code independent of the raw stream's field
// names. Every alias references the owned copies returned by the core API.
export function getCoreInspectorMesh(stream,index) {
  const mesh=stream.getMeshCopy(index);
  if(mesh.error)throw Error(mesh.error);
  mesh.name=mesh.primName;
  mesh.texcoords=mesh.uv0;
  mesh.vertexColors=mesh.colors;
  mesh.transform=mesh.worldMatrix;
  mesh.uvSets={};
  if(mesh.uv0?.length)mesh.uvSets.uv0={slotId:0,data:mesh.uv0};
  if(mesh.secondaryUVs?.length)mesh.uvSets.uv1={slotId:1,data:mesh.secondaryUVs};
  const subsets=stream.getMeshSubsetOutput(index);
  if(subsets)mesh.submeshes=subsets.submeshes.map(group=>({
    start:group.start,count:group.count,
    materialId:subsets.materials[group.materialIndex]?.id??-1
  }));
  return mesh;
}
