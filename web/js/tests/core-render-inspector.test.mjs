// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {PNG} from 'pngjs';
import {LightUSDLoader} from '../src/lightusd/LightUSDLoader.js';
import {buildUSDZWithNewRoot} from '../src/usdzconvert.js';
import {createCoreRenderInspector,getCoreInspectorMesh} from '../src/lightusd/CoreRenderInspector.js';
const suffix=process.env.LIGHTUSD_WASM64==='1'?'_64':'';
const {default:factory}=await import(`../src/lightusd/lightusd${suffix}.js`);
const native=await factory();
const source=new TextEncoder().encode(`#usda 1.0
(defaultPrim="World")
def Xform "World" {
 double3 xformOp:translate=(3,4,5)
 uniform token[] xformOpOrder=["xformOp:translate"]
 def Sphere "Ball" { rel material:binding=</World/Look>; }
 def Material "Look" {
  token outputs:surface.connect=</World/Look/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id="UsdPreviewSurface"
   color3f inputs:diffuseColor.connect=</World/Look/Texture.outputs:rgb>
   token outputs:surface
  }
  def Shader "Texture" {
   uniform token info:id="UsdUVTexture"
   asset inputs:file=@textures/check.png@
   float3 outputs:rgb
  }
 }
}`);
const encode=color=>PNG.sync.write({width:1,height:1,data:Buffer.from(color)});
const archive=buildUSDZWithNewRoot('nested/root.usda',source,[
  {name:'nested/textures/check.png',data:encode([12,34,56,255])},
  {name:'textures/check.png',data:encode([255,0,0,255])}]);
const loader=new LightUSDLoader();loader.native_=native;
const adapter=await loader.parseAsync(archive,'nested.usdz');
assert.equal(adapter.meshes.length,1);
assert.equal(adapter.meshes[0].texturePaths.baseColor,'nested/textures/check.png');
assert.deepEqual([...adapter.getArchiveTextureBytes(adapter.meshes[0].texturePaths.baseColor)],
  [...encode([12,34,56,255])]);
adapter.delete();
function crate(bytes) {
  const document=new native.LayerDocument();
  try {
    assert.equal(document.load(bytes).success,true);
    const exported=document.exportUSDC();assert.equal(exported.success,true);
    return new Uint8Array(exported.data);
  } finally {document.delete();}
}
for(const binary of [false,true]) {
  const extension=binary?'usdc':'usda';
  const refRoot=new TextEncoder().encode(`#usda 1.0\ndef Xform "Parent" (references=@looks/look.${extension}@</World>) {}\n`);
  const refArchive=buildUSDZWithNewRoot('nested/root.'+extension,binary?crate(refRoot):refRoot,[
    {name:'nested/looks/look.'+extension,data:binary?crate(source):source},
    {name:'nested/looks/textures/check.png',data:encode([12,34,56,255])},
    {name:'textures/check.png',data:encode([255,0,0,255])}]);
  const referenced=await loader.parseAsync(refArchive,'referenced.usdz');
  assert.equal(referenced.meshes.length,1);
  assert.equal(referenced.meshes[0].texturePaths.baseColor,'nested/looks/textures/check.png');
  assert.deepEqual([...referenced.getArchiveTextureBytes(referenced.meshes[0].texturePaths.baseColor)],
    [...encode([12,34,56,255])]);
  referenced.delete();
}
const fast=await createCoreRenderInspector(native,archive,'nested.usdz',{
  loadTextures:false,omitTexturePayloads:true});
assert.equal(fast.getMesh(0).material.baseColorTexture,'nested/textures/check.png');
fast.delete();
const stream=await createCoreRenderInspector(native,archive,'nested.usdz');
assert.equal(stream.numMeshes(),1);assert.equal(stream.numMaterials(),1);
assert.equal(stream.numImages(),1);
const inspectorMesh=getCoreInspectorMesh(stream,0);
assert.equal(inspectorMesh.texcoords.length/2,inspectorMesh.points.length/3);
assert.deepEqual(inspectorMesh.transform.slice(12,15),[3,4,5]);
const image=stream.getImageCopy(0);
assert.equal(image.decoded,true);
assert.deepEqual([...image.data],[12,34,56,255]);
stream.delete();
assert.deepEqual([...image.data],[12,34,56,255]);
const loose=await createCoreRenderInspector(native,new TextEncoder().encode('#usda 1.0\ndef Cube "Box" {}\n'),'box.usda');
assert.equal(loose.numMeshes(),1);loose.delete();
const cache=new native.AssetStore();
cache.registerMemoryAsset('nested/root.usda',source);
cache.registerMemoryAsset('nested/textures/check.png',encode([12,34,56,255]));
cache.registerMemoryAsset('textures/check.png',encode([255,0,0,255]));
const cached=new native.RenderStream();cached.setLoadTextureInNative(true);
assert.equal(cached.beginCachedAsset(cache,'nested/root.usda').success,true);
assert.deepEqual([...cached.getImageCopy(0).data],[12,34,56,255]);
cached.delete();
const streamed=new native.RenderStream();streamed.setLoadTextureInNative(true);
streamed.importAssetStore(cache);
streamed.startStreamingAsset('nested/root.usda',source.length);
streamed.appendStreamingAsset('nested/root.usda',source);
assert.equal(streamed.beginStreamedAsset('nested/root.usda').success,true);
assert.deepEqual([...streamed.getImageCopy(0).data],[12,34,56,255]);
streamed.delete();cache.delete();
const viewInput=new Uint8Array(source.length+8);viewInput.set(source,4);
const view=await createCoreRenderInspector(native,new DataView(viewInput.buffer,4,source.length),'view.usda',{loadTextures:false});
assert.equal(view.numMeshes(),1);view.delete();
const invalid=new native.RenderStream();
assert.throws(()=>invalid.begin(source,'bad'+String.fromCharCode(0)+'name'),/NUL/);
assert.throws(()=>invalid.begin(source,'x'.repeat(0x100001)),/1 MiB/);
invalid.delete();
const handle=native._lightusd_wasm_create(4);
const ptr=native._lightusd_wasm_alloc(1),zero=typeof ptr==='bigint'?0n:0;
try {
  assert.equal(native._lightusd_wasm_render_begin_named(handle,zero,0,zero,1),-1);
  assert.equal(native._lightusd_wasm_render_begin_named(handle,zero,0,zero,0x100001),-1);
  native.HEAPU8[Number(ptr)]=0;
  assert.equal(native._lightusd_wasm_render_begin_named(handle,zero,0,ptr,1),-1);
} finally {native._lightusd_wasm_free(ptr);native._lightusd_wasm_destroy(handle);}
const wrongRoot=buildUSDZWithNewRoot('texture.png',encode([1,2,3,255]),[{name:'root.usda',data:source}]);
await assert.rejects(createCoreRenderInspector(native,wrongRoot,'wrong-root.usdz'),/root layer/);
await assert.rejects(createCoreRenderInspector(native,new Uint8Array([0x50,0x4b,3,4]),'bad.usdz'),/small|valid|ZIP/);
await assert.rejects(createCoreRenderInspector(native,source,'scene.usda',{maxMemoryLimitMB:0}),/integer/);
const exr=await native.loadFeature('exr');
const hdrPixels=new Float32Array([0.25,2.5,4]);
const hdrBytes=exr.encode(new Uint8Array(hdrPixels.buffer),1,1,3,32);
const hdrSource=new TextEncoder().encode(new TextDecoder().decode(source).replace('check.png','check.exr'));
const hdrArchive=buildUSDZWithNewRoot('nested/root.usda',hdrSource,[
  {name:'nested/textures/check.exr',data:hdrBytes}]);
const hdrStream=await createCoreRenderInspector(native,hdrArchive,'hdr.usdz');
const hdrImage=hdrStream.getImageCopy(0);
assert.equal(hdrImage.decoded,true);
assert.equal(hdrImage.componentType,7);
assert.deepEqual([...new Float32Array(hdrImage.data.buffer)].slice(0,3),[0.25,2.5,4]);
hdrStream.delete();
console.log('PASS: core inspector package anchors, same-name textures, owned pixels and loose input');
