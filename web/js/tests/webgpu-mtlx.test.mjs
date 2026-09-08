// SPDX-License-Identifier: Apache-2.0
import test from 'node:test';
import assert from 'node:assert/strict';
import { compileGraph, literal, GraphError, contextWGSL } from '../src/webgpu-mtlx/graph.js';
import { packScene, syntheticScene } from '../src/webgpu-mtlx/scene.js';
import { encodeEXR } from '../src/webgpu-mtlx/capture.js';
import { packImages } from '../src/webgpu-mtlx/textures.js';
import { shaderSource } from '../src/webgpu-mtlx/shaders.js';
import { validateSpectrum, sampleSpectrum } from '../src/webgpu-mtlx/spectrum.js';
import { cieXYZ } from '../src/webgpu-mtlx/cie-data.js';
import { refineDisplacementScene } from '../src/webgpu-mtlx/displacement.js';
import { fetchResource, inspectEXR, inspectEXRHeader, decodeImage } from '../src/webgpu-mtlx/resources.js';
import { appendRectLights } from '../src/webgpu-mtlx/usd-lights.js';
import { mayEmit } from '../src/webgpu-mtlx/emission.js';
import { materialXFromUSD } from '../src/webgpu-mtlx/usd-graph.js';
import { USDTextureSources } from '../src/webgpu-mtlx/usd-texture-sources.js';
import { triangleMaterialIds } from '../src/webgpu-mtlx/usd-scene.js';
const constant = (name, value, type = 'float') => ({ name, category: 'constant', type, inputs: { value: { type, value } } });

test('USD mesh submeshes preserve per-face material bindings', () => {
  assert.deepEqual(triangleMaterialIds(18, 2, [
    { start: 0, count: 6, materialId: 4 },
    { start: 6, count: 3, materialId: 1 }
  ]), [4, 4, 1, 2, 2, 2]);
  assert.throws(() => triangleMaterialIds(6, 0, [{ start: 0, count: 6, materialId: 1 }, { start: 3, count: 3, materialId: 2 }]), /overlapping/);
  assert.throws(() => triangleMaterialIds(6, 0, [{ start: 1, count: 3, materialId: 1 }]), /invalid/);
});

test('USD texture provenance keeps anchors and rejects ambiguous source layers', () => {
  const snapshot = value => ({ prims: [{ path: '/Shader', properties: { 'inputs:file': { type: 'asset', value } } }] });
  const sources = new USDTextureSources(), context = { propertyPath: '/Remapped.inputs:file', colorspace: 'acescg' };
  sources.register(snapshot('../maps/color.exr'), 'https://example.test/layers/a.usda');
  const key = sources.resolveAsset('../maps/color.exr', context);
  assert.equal(sources.requests.get(key).url, 'https://example.test/maps/color.exr');
  const rawKey = sources.resolveAsset('../maps/color.exr', { ...context, colorspace: 'raw' });
  assert.notEqual(rawKey, key);
  sources.register(snapshot('../maps/color.exr'), 'https://example.test/layers/b.usda');
  assert.equal(sources.resolveAsset('../maps/color.exr', context), key);
  sources.register(snapshot('../maps/color.exr'), 'https://example.test/nested/layers/c.usda');
  assert.throws(() => sources.resolveAsset('../maps/color.exr', context), /ambiguous/);
  assert.throws(() => sources.resolveAsset('missing.exr', context), /no source-layer/);
  for (const asset of ['https://other.test/image.exr', 'a[image.exr]', 'image.<UDIM>.exr', 'file:///image.exr']) {
    sources.register(snapshot(asset), 'https://example.test/root.usda');
    assert.throws(() => sources.resolveAsset(asset, context), /unsupported/);
  }
  sources.register({ assetPaths: [{ propertyPath: '/Override.inputs:file', authored: './override.exr' }] }, 'https://example.test/layer.usda');
  const override = sources.resolveAsset('./override.exr', context);
  assert.equal(sources.requests.get(override).url, 'https://example.test/override.exr');
});

test('standard and OpenPBR terminal aliases preserve authored graph inputs', () => {
  const standard = { nodes: [
    { name: 'surface', category: 'standard_surface', type: 'surfaceshader', inputs: {
      base_color: { type: 'color3', value: [.2,.3,.4] }, metalness: { type: 'float', value: .5 },
      specular_color: { type: 'color3', value: [1, .8, .7] }, specular_IOR: { type: 'float', value: 1.7 },
      specular_roughness: { type: 'float', value: .2 }, emission: { type: 'float', value: 0 }
    } }
  ] };
  assert.doesNotThrow(() => compileGraph(standard, { material: true }));
  const preview=compileGraph({nodes:[{name:'s',category:'UsdPreviewSurface',type:'surfaceshader',inputs:{diffuseColor:{type:'color3',value:[.8,.2,.1]},metallic:{type:'float',value:.7},roughness:{type:'float',value:.25},clearcoat:{type:'float',value:.4},opacity:{type:'float',value:.8},normal:{type:'vector3',value:[0,0,1]}}}]},{material:true});
  assert.match(preview.body,/materialFromClosure/); assert.match(preview.body,/nativeDielectric/); assert.match(preview.body,/vec3f\(0\.0,0\.0,1\.0\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'s',category:'UsdPreviewSurface',type:'surfaceshader',inputs:{displacement:{type:'float',value:1}}}]},{material:true}),/displacement/);
  const unlit=compileGraph({nodes:[{name:'u',category:'surface_unlit',type:'surfaceshader',inputs:{emission:{type:'float',value:2},emission_color:{type:'color3',value:[1,.5,0]},transmission:{type:'float',value:.25},opacity:{type:'float',value:.8}}}]},{material:true});
  assert.match(unlit.body,/materialFromClosure/); assert.match(unlit.body,/vec3f\(1\.0,0\.5,0\.0\)\*max/); assert.match(unlit.body,/nativeDielectric/);
  const open = { nodes: [{ name: 'surface', category: 'open_pbr_surface', type: 'surfaceshader', inputs: {
    base_weight: { type: 'float', value: 1 }, base_color: { type: 'color3', value: [.2,.3,.4] },
    specular_weight: { type: 'float', value: 1 }, specular_ior: { type: 'float', value: 1.5 },
    specular_roughness_anisotropy: { type: 'float', value: 0 }, emission_luminance: { type: 'float', value: 0 },
    geometry_opacity: { type: 'float', value: 1 }
  } }] };
  assert.doesNotThrow(() => compileGraph(open, { material: true }));
  const coated = structuredClone(standard); coated.nodes[0].inputs.coat = { type: 'float', value: .2 };
  assert.match(compileGraph(coated, { material: true }).body, /closureAdd/);
});

test('MaterialX unit annotations are validated and preserved', () => {
  const rotate = { nodes: [{ name: 'r', category: 'rotate2d', type: 'vector2', inputs: {
    in: { type: 'vector2', value: [1, 0] }, amount: { type: 'float', value: 90, unit: 'degree' }
  } }] };
  assert.doesNotThrow(() => compileGraph(rotate));
  const bad = structuredClone(rotate); bad.nodes[0].inputs.amount.unit = 'furlong';
  assert.throws(() => compileGraph(bad), /unsupported MaterialX unit/);
  assert.match(compileGraph({nodes:[{name:'r',category:'rotate2d',type:'vector2',inputs:{in:{type:'vector2',value:[1,0]}}}]}).body,/0\.0 \* 0\.017453292519943295/);
});

test('USD graph translation preserves interfaces and exact NodeDef typing', () => {
  const p = (type, value, connections = []) => ({ type, ...(value === undefined ? {} : { value }), connections, timeSampled: false });
  const snapshot = { version: 1, prims: [
    { path: '/M', type: 'Material', properties: { 'outputs:mtlx:surface': p('token', undefined, ['/M/G.outputs:surface']) } },
    { path: '/M/G', type: 'NodeGraph', properties: { 'outputs:surface': p('token', undefined, ['/M/S.outputs:out']), 'inputs:color': p('color3f', [.2,.4,.6]) } },
    { path: '/M/S', type: 'Shader', properties: { 'info:id': p('token', 'ND_standard_surface_surfaceshader'), 'inputs:base_color': p('color3f', [1,0,0], ['/M/G.inputs:color']), 'outputs:out': p('token') } }
  ] };
  const library = { definitions: { ND_standard_surface_surfaceshader: { node: 'standard_surface', inputs: { base_color: { type: 'color3' } }, outputs: { out: { type: 'surfaceshader' } } } } };
  const doc = materialXFromUSD(snapshot, '/M', { library });
  assert.equal(doc.nodes.length, 1);
  assert.deepEqual(doc.nodes[0].inputs.base_color, { type: 'color3', value: [.2,.4,.6], colorspace: 'lin_rec709' });
  assert.doesNotThrow(() => compileGraph(doc, { material: true }));
  const changed = () => structuredClone(snapshot);
  let bad = changed(); bad.prims[1].properties['inputs:color'].connections = ['/M/S.inputs:base_color'];
  assert.throws(() => materialXFromUSD(bad, '/M', { library }), /cycle/);
  bad = changed(); bad.prims[1].properties['inputs:color'].timeSampled = true;
  assert.throws(() => materialXFromUSD(bad, '/M', { library }), /time-sampled/);
  bad = changed(); bad.prims[1].properties['inputs:color'].type = 'float';
  assert.throws(() => materialXFromUSD(bad, '/M', { library }), /type mismatch/);
  assert.throws(() => materialXFromUSD(snapshot, '/M'), /missing exact/);
  bad = changed(); bad.prims.push(bad.prims[0]);
  assert.throws(() => materialXFromUSD(bad, '/M', { library }), /duplicate/);
  bad = changed(); bad.prims[2].properties['inputs:unknown'] = p('float', 1);
  assert.throws(() => materialXFromUSD(bad, '/M', { library }), /absent from NodeDef/);
  bad = changed(); bad.prims[0].properties['outputs:displacement'] = p('token', undefined, ['/M/S.outputs:out']);
  assert.throws(() => materialXFromUSD(bad, '/M', { library }), /non-surface/);
  const colored = changed(); colored.colorSpaces = { '/M': { value: 'lin_ap1_scene', timeSampled: false } };
  assert.equal(materialXFromUSD(colored, '/M', { library }).nodes[0].inputs.base_color.colorspace, 'acescg');
  colored.prims[1].properties['inputs:color'].colorSpace = 'srgb_rec709_scene';
  // Destination metadata must not recolor a connected source value.
  colored.prims[2].properties['inputs:base_color'].colorSpace = 'data';
  assert.equal(materialXFromUSD(colored, '/M', { library }).nodes[0].inputs.base_color.colorspace, 'srgb_texture');
  colored.prims[1].properties['inputs:color'].colorSpace = 'custom_unknown';
  assert.throws(() => materialXFromUSD(colored, '/M', { library }), /unsupported USD color/);
});

test('USD graph asset resolution requires explicit source-aware ownership', () => {
  const p = (type, value, connections = []) => ({ type, value, connections, timeSampled: false });
  const snapshot = { version: 1, prims: [
    { path: '/M', type: 'Material', properties: { 'outputs:mtlx:surface': p('token', undefined, ['/S.outputs:out']) } },
    { path: '/S', type: 'Shader', properties: { 'info:id': p('token', 'custom_surface'), 'inputs:file': p('asset', '../maps/image.exr'), 'outputs:out': p('token') } }
  ] };
  const library = { definitions: { custom_surface: { node: 'custom_surface', inputs: { file: { type: 'filename' } }, outputs: { out: { type: 'surfaceshader' } } } } };
  assert.throws(() => materialXFromUSD(snapshot, '/M', { library }), /source-layer-aware/);
  const doc = materialXFromUSD(snapshot, '/M', { library, resolveAsset(value, context) {
    assert.equal(value, '../maps/image.exr'); assert.equal(context.propertyPath, '/S.inputs:file');
    assert.equal(context.colorspace, 'lin_rec709');
    return 'resolved-resource';
  } });
  assert.equal(doc.nodes[0].inputs.file.value, 'resolved-resource');
  assert.throws(() => materialXFromUSD(snapshot, '/M', { library, resolveAsset: () => Promise.resolve('key') }), /nonempty resource key/);
});
test('layered medium bounds follow interior dependencies, not unrelated surface nodes',()=>{
  const doc={nodes:[
    {name:'position',category:'position',type:'vector3'},
    {name:'color',category:'convert',type:'color3',inputs:{in:{nodename:'position'}}},
    {name:'diffuse',category:'oren_nayar_diffuse_bsdf',type:'BSDF',inputs:{color:{nodename:'color'}}},
    {name:'medium',category:'anisotropic_vdf',type:'VDF',inputs:{scattering:{type:'color3',value:[1,1,1]}}},
    {name:'layer',category:'layer',type:'BSDF',inputs:{top:{nodename:'diffuse'},base:{nodename:'medium'}}},
    {name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'layer'}}}
  ]};
  assert.equal(compileGraph(doc,{material:true}).hasInterior,true);shaderSource([doc]);
  doc.nodes[3].inputs.scattering={nodename:'color'};
  assert.throws(()=>shaderSource([doc]),/layered media require/);
  doc.mediumMajorant=5;shaderSource([doc]);
});
test('emission sampling excludes proven dark surfaces but retains unknown and spectral emitters',()=>{
  const scene=syntheticScene();assert.equal(mayEmit(scene.materials[0]),false);
  const packed=packScene(scene);assert.equal(packed.triangleData[(packed.triangleCount-1)*48+19],0);
  scene.materials[1].nodes[0].inputs.emission={type:'float',value:1};assert.equal(mayEmit(scene.materials[1]),true);
  assert.ok(packScene(scene).triangleData[(packed.triangleCount-1)*48+19]>0);
  assert.equal(mayEmit({nodes:[{name:'custom',category:'custom'}]}),true);
  const doc=syntheticScene().materials[1];doc.spectra={emission_color:[[360,1],[830,1]]};assert.equal(mayEmit(doc),true);
});
test('authored literal colors inherit source colorspace and preserve alpha',()=>{
  const doc={colorspace:'srgb_texture',nodes:[constant('color',[.5,.5,.5,.25],'color4')]};
  assert.match(compileGraph(doc).body,/0.21404114048223255/);assert.match(compileGraph(doc).body,/,0.25\)/);
  doc.colorspace='unknown';assert.throws(()=>compileGraph(doc),/Unsupported color space/);
});
test('NodeDef overload resolution uses authored input types',()=>{
  const definitions=Object.fromEntries(['float','vector3'].map(type=>[type,{name:type,node:'add',inputs:{in1:{type:'vector3'},in2:{type}},outputs:{out:{type:'vector3'}}}]));
  const doc={definitions,nodes:[{name:'sum',type:'vector3',category:'add',inputs:{in1:{type:'vector3',value:[1,2,3]},in2:{type:'float',value:2}}}]};
  assert.match(compileGraph(doc).body,/vec3f\(2.0\)/);
});
test('USD rect lights preserve world-space area, radiance and negative-Z orientation',()=>{
  const scene={positions:[],normals:[],uvs:[],indices:[],materials:[]};
  const light={type:'rect',width:2,height:3,intensity:12,exposure:1,normalize:true,color:[1,.5,.25],transform:[2,0,0,0,0,3,0,0,0,0,1,0,1,2,3,1]};
  const r=appendRectLights(scene,[light]);
  assert.equal(r.provenance.rectLights[0].worldArea,36);
  assert.deepEqual(r.provenance.rectLights[0].radiance,[2/3,1/3,1/6]);
  assert.equal(r.normals[2],-1);assert.equal(r.positions[2],3);assert.equal(scene.positions.length,0);
  assert.equal(r.materials[0].twoSidedEmission,false);
  assert.throws(()=>appendRectLights(scene,[{...light,textureFile:'light.exr'}]),/Unsupported/);
  assert.throws(()=>appendRectLights(scene,[{...light,width:0}]),/Invalid/);
});
test('resource fetch enforces streaming budgets and HTTP errors',async()=>{
  const fetcher=async()=>new Response(new Uint8Array([1,2,3,4]));
  assert.deepEqual(await fetchResource('test',{fetcher,maxBytes:4}),new Uint8Array([1,2,3,4]));
  await assert.rejects(fetchResource('test',{fetcher,maxBytes:3}),/budget/);
  await assert.rejects(fetchResource('test',{fetcher:async()=>new Response('',{status:404})}),/HTTP 404/);
});
test('EXR resource preflight bounds allocation and decode preserves bottom-up rows',async()=>{
  const bytes=encodeEXR(1,2,new Float32Array([1,2,3,1,4,5,6,1]));
  assert.deepEqual(inspectEXR(bytes),{width:1,height:2});
  assert.throws(()=>inspectEXR(bytes,1),/budget/);
  assert.throws(()=>inspectEXR(bytes.subarray(0,20)),/EXR/);
  const image=await decodeImage(bytes,{filename:'test.exr',colorspace:'raw'});
  assert.deepEqual(Array.from(image.data),[4,5,6,1,1,2,3,1]);
  await assert.rejects(decodeImage(bytes,{filename:'test.exr',maxPixels:1}),/budget/);
  const reduced=await decodeImage(bytes,{filename:'test.exr',maxPixels:1,allowDownsample:true});
  assert.deepEqual([reduced.width,reduced.height,reduced.resizedFrom.width,reduced.resizedFrom.height],[1,1,1,2]);
  assert.ok(reduced.data.every(Number.isFinite));
});

test('oversized uncompressed EXR uses scanline reduction without full-resolution allocation', async () => {
  const rgba = new Float32Array([
    1,0,0,1, 3,0,0,1, 5,0,0,1, 7,0,0,1,
    9,0,0,1, 11,0,0,1, 13,0,0,1, 15,0,0,1
  ]);
  const bytes = encodeEXR(4, 2, rgba);
  const image = await decodeImage(bytes, { filename: 'large.exr', maxPixels: 4, allowDownsample: true });
  assert.deepEqual({ width: image.width, height: image.height, resizedFrom: image.resizedFrom }, { width: 2, height: 1, resizedFrom: { width: 4, height: 2 } });
  assert.ok(Math.abs(image.data[0] - 6) < 1e-5);
  assert.ok(Math.abs(image.data[4] - 10) < 1e-5);
  assert.equal(image.data[3], 1);
});

test('EXR authored color-space metadata is read from the header, never filename', () => {
  const bytes = encodeEXR(1, 1, new Float32Array([1, 2, 3, 1]), { colorspace: 'lin_ap1_scene' });
  assert.equal(inspectEXRHeader(bytes).colorSpace, 'lin_ap1_scene');
  assert.deepEqual(inspectEXR(bytes), { width: 1, height: 1 });
  assert.throws(() => encodeEXR(1, 1, new Float32Array([1,2,3,1]), { colorspace: 'bad\nspace' }), /metadata/);
});
test('EXR color-space aliases normalize before image packing', async () => {
  const bytes = encodeEXR(1, 1, new Float32Array([1, 0, 0, 1]), { colorspace: 'lin_ap1_scene' });
  const image = await decodeImage(bytes, { filename: 'metadata.exr' });
  assert.equal(image.colorspace, 'acescg');
  const packed = packImages([image]);
  assert.ok(Number.isFinite(packed.data[0]));
});
test('ACEScg image conversion matches pinned MaterialX matrix without alpha or gamut clipping',()=>{
  const image=packImages([{width:1,height:1,colorspace:'acescg',data:[1,0,0,.3]}]);
  const expected=[1.705050992658,-.130256417507,-.024003356805,.3];
  expected.forEach((v,i)=>assert.ok(Math.abs(image.data[i]-v)<1e-7));
});
test('closure composition preserves lobe bounds and interior ownership',()=>{
  const leaf={name:'leaf',category:'oren_nayar_diffuse_bsdf',type:'BSDF'};
  const nodes=[leaf];
  let previous='leaf';
  for(let i=1;i<=16;i++) {
    nodes.push({name:`sum${i}`,category:'add',type:'BSDF',inputs:{in1:{nodename:previous},in2:{nodename:'leaf'}}});
    previous=`sum${i}`;
    if(i<16) assert.match(compileGraph({nodes}).body,/closureAdd/);
  }
  assert.throws(()=>compileGraph({nodes}),/exceeds 16 lobes/);
  const layered=[leaf,{name:'medium',category:'anisotropic_vdf',type:'VDF'},
    {name:'layer',category:'layer',type:'BSDF',inputs:{top:{nodename:'leaf'},base:{nodename:'medium'}}}];
  assert.match(compileGraph({nodes:layered}).body,/closureInterior/);
  layered.push({name:'sum',category:'add',type:'BSDF',inputs:{in1:{nodename:'layer'},in2:{nodename:'leaf'}}});
  assert.match(compileGraph({nodes:layered}).body,/closureAddPreservingInterior/);
});
test('native closures compile and unsupported uniform inputs are diagnosed',()=>{
  for(const preset of ['native-copper','native-glass']) {
    const resources={};const source=shaderSource(syntheticScene(preset).materials,resources);
    assert.equal(resources.requiresPhysical,true);assert.match(source,/surfaceEmission/);
  }
  const doc=syntheticScene('native-glass').materials[1];
  doc.nodes[0].inputs.scatter_mode={nodename:'dynamic_mode'};
  assert.throws(()=>compileGraph(doc,{material:true}),/connected scatter_mode/);
  doc.nodes[0].inputs.scatter_mode={value:'RT'};
  doc.nodes[0].inputs.thinfilm_thickness={type:'float',value:100};
  assert.match(compileGraph(doc,{material:true}).body,/withThinFilm/);
  const volume=syntheticScene('sss').materials[1];volume.mediumMajorant=-1;
  assert.throws(()=>shaderSource([volume]),/majorant/);
  const hair=syntheticScene('hair').materials[1];
  assert.match(compileGraph(hair,{material:true}).body,/nativeHair/);
  assert.match(shaderSource([hair]),/nativeHair/);
  const film=syntheticScene().materials[1];
  film.nodes[0].inputs.thin_film_thickness={type:'float',value:180};
  film.nodes[0].inputs.thin_film_IOR={type:'float',value:1.4};
  assert.match(compileGraph(film,{material:true}).body,/180\.0/);
  assert.match(shaderSource([film]),/thinFilmFresnel/);
  const subsurface=syntheticScene('subsurface').materials[1];
  assert.match(compileGraph(subsurface,{material:true}).body,/materialFromClosure/);
  assert.match(compileGraph(subsurface,{material:true}).body,/nativeSubsurface/);
  assert.match(shaderSource([subsurface]),/closureMix/);
  const normal=syntheticScene('normalmap').materials[1];
  assert.match(compileGraph(normal,{material:true}).body,/normalize\(n/);
  assert.match(shaderSource([normal]),/surface\.normal/);
  assert.match(shaderSource([normal]),/var n=safeNormal\(surface\.normal/);
  const imageNormal=syntheticScene('normalmap-image').materials[1];
  const imageResources={};
  assert.match(shaderSource([imageNormal],imageResources),/imageSample\(0u/);
  assert.match(compileGraph(imageNormal,{material:true,imageDescriptors:{normalTex:{offset:0,width:2,height:2,levels:1,colorspace:'raw'}}}).body,/mxNormalmap\(n0/);
  assert.equal(imageResources.imageData.length,20);
  const colorNormal={nodes:[{name:'normal',category:'normalmap',type:'vector3',inputs:{in:{type:'color3',value:[.65,.45,.95]},scale:{type:'float',value:1}}}]};
  assert.match(compileGraph(colorNormal).body,/mxNormalmap\(vec3f/);
  const openNormal=syntheticScene('open-pbr-normal').materials[1];
  assert.match(compileGraph(openNormal,{material:true}).body,/geometry_normal|normalize\(vec3f/);
  const opacity=syntheticScene('opacity').materials[1];
  assert.match(shaderSource([opacity]),/surface\.opacity/);
  assert.match(shaderSource([opacity]),/directionalRadiance\(\)\*clamp\(surface\.opacity/);
  assert.match(shaderSource([opacity]),/let opacity=clamp\(surface\.opacity/);
  assert.match(shaderSource([opacity]),/random\(rng\)>opacity/);
  const bump=syntheticScene('bump').materials[1];
  assert.match(compileGraph(bump,{material:true}).body,/mxBumpGradient/);
  assert.match(shaderSource([bump]),/mxBumpGradient/);
  assert.equal(syntheticScene('displacement').displacementRefinement,2);
  const ops=syntheticScene('ops').materials[1];
  const opsBody=compileGraph(ops,{material:true}).body;
  assert.match(opsBody,/mat2x2f/);
  assert.match(opsBody,/sign\(/);
  assert.match(opsBody,/select\(/);
  assert.match(opsBody,/vec3f\(n/);
  const advanced=syntheticScene('ops-advanced').materials[1];
  const advancedBody=compileGraph(advanced,{material:true}).body;
  assert.match(advancedBody,/vec3f\(n0,0\.0\)/);
  assert.match(advancedBody,/vec2f\(n1\[1\],n1\[0\]\)/);
  assert.match(advancedBody,/cross\(/);
  assert.match(advancedBody,/dot\(/);
  assert.match(advancedBody,/pow\(/);
  const layeredScene=syntheticScene('layered').materials[1];
  assert.match(compileGraph(layeredScene,{material:true}).body,/closureInterior/);
  const edf=syntheticScene('edf').materials[1];
  assert.match(compileGraph(edf,{material:true}).body,/surfaceEmission/);
  assert.match(compileGraph(edf,{material:true}).body,/vec3f\(2\.5,0\.8,0\.15\)/);
  const nativeFilm=syntheticScene('native-film').materials[1];
  assert.match(compileGraph(nativeFilm,{material:true}).body,/withThinFilm/);
  const conductorFilm={nodes:[{name:'film',category:'conductor_bsdf',type:'BSDF',inputs:{thinfilm_thickness:{type:'float',value:120},thinfilm_IOR:{type:'float',value:1.4}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'film'}}}]};
  assert.match(compileGraph(conductorFilm,{material:true}).body,/withThinFilm/);
  const coat=syntheticScene('coat').materials[1];
  assert.match(compileGraph(coat,{material:true}).body,/closureAdd/);
  assert.match(shaderSource([coat]),/closureAdd/);
  const sheen=syntheticScene('sheen').materials[1];
  assert.match(compileGraph(sheen,{material:true}).body,/nativeDiffuse/);
  assert.match(compileGraph(sheen,{material:true}).body,/0\.38/);
  assert.match(shaderSource([sheen]),/nativeDiffuse/);
  const thin=syntheticScene('thin-walled').materials[1];
  assert.match(compileGraph(thin,{material:true}).body,/select\(0u,1u,true\)/);
  assert.match(shaderSource([thin]),/thinWalled/);
  assert.match(shaderSource([thin]),/refract\(-wo,n/);
  const depth=syntheticScene('transmission-depth').materials[1];
  assert.match(compileGraph(depth,{material:true}).body,/withTransmission/);
  assert.match(shaderSource([depth]),/transmissionAttenuation/);
  const schlick=syntheticScene('generalized-schlick').materials[1];
  assert.match(compileGraph(schlick,{material:true}).body,/nativeGeneralizedSchlick/);
  assert.match(shaderSource([schlick]),/generalizedSchlickFresnel/);
  const openPbrWeight=syntheticScene('open-pbr-weight').materials[1];
  assert.match(compileGraph(openPbrWeight,{material:true}).body,/0\.35/);
  assert.match(compileGraph(openPbrWeight,{material:true}).body,/withSpecular/);
  assert.match(shaderSource([openPbrWeight]),/vec3f\(0\.35/);
  assert.match(shaderSource([openPbrWeight]),/clamp\(m\.weight\*mix/);
  assert.match(compileGraph(openPbrWeight,{material:true}).body,/withSpecularColor/);
  const openPbrFilm=syntheticScene('open-pbr-film').materials[1];
  assert.match(compileGraph(openPbrFilm,{material:true}).body,/220\.0/);
  assert.match(shaderSource([openPbrFilm]),/thinFilmFresnel/);
  const diffuseRoughness = { nodes: [{ name: 'surface', category: 'standard_surface', type: 'surfaceshader', inputs: {
    base_color: { type: 'color3', value: [.4, .2, .1] }, diffuse_roughness: { type: 'float', value: .72 }
  } }] };
  assert.match(compileGraph(diffuseRoughness,{material:true}).body,/0\.72/);
});
test('standard MaterialX Burley, Chiang hair, and absorption VDF nodes compile',()=>{
  const burley=compileGraph({nodes:[{name:'b',category:'burley_diffuse_bsdf',type:'BSDF',inputs:{color:{type:'color3',value:[.5,.4,.3]},roughness:{type:'float',value:.2}}}]});
  assert.match(burley.body,/nativeDiffuse/);
  const hair=compileGraph({nodes:[{name:'h',category:'chiang_hair_bsdf',type:'BSDF',inputs:{tint_R:{type:'color3',value:[.8,.7,.6]},roughness_R:{type:'vector2',value:[.1,.2]},roughness_TT:{type:'vector2',value:[.05,.1]}}}]});
  assert.match(hair.body,/nativeHair/); assert.match(hair.body,/\.x/);
  const medium=compileGraph({nodes:[{name:'m',category:'absorption_vdf',type:'VDF',inputs:{absorption:{type:'vector3',value:[.1,.2,.3]}}}]});
  assert.match(medium.body,/Medium\(vec3f\(0\.1,0\.2,0\.3\),vec3f\(0\),0\.0\)/);
});
test('displacement refinement preserves bounds, typed indices and material assignment',()=>{
  const scene={positions:new Float32Array([0,0,0,1,0,0,0,1,0]),indices:new Uint32Array([0,1,2]),materials:[{}]};
  const r=refineDisplacementScene(scene,2);assert.equal(r.indices.length,48);assert.equal(r.materialIds.length,16);assert.ok(r.positions.every(v=>v>=0&&v<=1));
  assert.ok(r.normals.every(Number.isFinite));
  for(let i=0;i<r.normals.length;i+=3)assert.ok(Math.abs(Math.hypot(r.normals[i],r.normals[i+1],r.normals[i+2])-1)<1e-5);
  assert.throws(()=>refineDisplacementScene(scene,5,1),/budget/);
});
test('CIE table agrees with official column-sum validation', () => {
  assert.equal(cieXYZ.length,471);
  [106.865469489595,106.8569171011719,106.892251278636].forEach((expected,k)=>assert.ok(Math.abs(cieXYZ.reduce((sum,row)=>sum+row[k],0)-expected)<1e-9));
});
test('measured spectra validate ordering, coverage and interpolate linearly', () => {
  const p=validateSpectrum([[360,1.6],[830,1.4]],{ior:true});assert.equal(sampleSpectrum(p,595),1.5);assert.equal(sampleSpectrum(p,350),0);
  assert.throws(()=>validateSpectrum([[400,1.5],[700,1.4]],{ior:true}),/cover/);
  assert.throws(()=>validateSpectrum([[400,1],[400,2]]),/unordered/);
  assert.throws(()=>validateSpectrum([[400,1],[700,-1]]),/Invalid/);
});
test('image packing linearizes before mip filtering and preserves alpha', () => {
  const packed = packImages([{ width: 3, height: 1, colorspace: 'srgb_texture', data: [0,0,0,.2, .5,.5,.5,.4, 1,1,1,.6] }]);
  assert.deepEqual(packed.descriptors, [{ offset: 0, width: 3, height: 1, levels: 2 }]);
  assert.ok(Math.abs(packed.data[4] - .21404114) < 1e-6);
  assert.ok(Math.abs(packed.data[12] - (1 + .21404114) / 3) < 1e-6);
  assert.ok(Math.abs(packed.data[15] - .4) < 1e-6);
});
test('image packing can bounded-box downsample before GPU mip allocation', () => {
  const data = new Float32Array(4 * 2 * 4); for (let i = 0; i < data.length; i += 4) { data[i] = 1; data[i + 3] = 1; }
  const packed = packImages([{ width: 4, height: 2, data }], { maxDimension: 2, maxBytes: 1024 });
  assert.deepEqual(packed.descriptors[0].resizedFrom, [4, 2]);
  assert.equal(packed.descriptors[0].width, 2); assert.equal(packed.descriptors[0].height, 1);
  assert.equal(packed.data[0], 1); assert.equal(packed.data[3], 1);
  assert.throws(() => packImages([{ width: 4, height: 2, data }], { maxDimension: 2, maxBytes: 15 }), /budget/);
});
test('image budgets, finite float32, dimensions and color interpretation are validated', () => {
  const image = { width: 1, height: 1, data: [0,0,0,1] };
  assert.throws(() => packImages([image], { maxBytes: 15 }), /budget/);
  assert.throws(() => packImages([{ ...image, width: .5 }]), /dimensions/);
  assert.throws(() => packImages([{ ...image, data: [1e100,0,0,1] }]), /non-finite/);
  assert.throws(() => packImages([{ ...image, colorspace: 'unknown' }]), /colorspace/);
});
test('image graph resource resolution and unsupported filtering diagnostics', () => {
  const doc = syntheticScene().materials[1];
  const image = { name: 'image', category: 'image', type: 'color3', inputs: { file: { type: 'filename', value: 'test' } } };
  doc.nodes[0].inputs.base_color = { nodename: 'image' }; doc.nodes.unshift(image);
  assert.throws(() => shaderSource([doc]), /missing decoded image/);
  doc.images = { test: { width: 1, height: 1, data: [1,0,0,1] } };
  const resources = {}; assert.match(shaderSource([doc], resources), /imageSample\(0u/); assert.equal(resources.imageData.length, 4);
  image.colorspace = 'lin_ap1_scene'; doc.images.test.colorspace = 'acescg';
  assert.match(shaderSource([doc], {}), /imageSample\(0u/);
  image.inputs.filtertype = { value: 'cubic' }; assert.match(shaderSource([doc]), /imageSampleCubic\(0u/);
  image.inputs.file.value = ''; assert.match(shaderSource([doc]), /vec3f\(0.0,0.0,0.0\)/);
});
test('UsdUVTexture maps st/fallback/scale/bias and named channel outputs', () => {
  const doc={images:{tex:{width:2,height:2,data:[1,0,0,1],colorspace:'raw'}},nodes:[{name:'tex',category:'UsdUVTexture',type:'multioutput',outputs:{rgb:{type:'color3'},r:{type:'float'}},inputs:{file:{type:'filename',value:'tex'},st:{type:'vector2',value:[.25,.5]},fallback:{type:'color4',value:[.1,.2,.3,1]},scale:{type:'color4',value:[2,2,2,1]},bias:{type:'color4',value:[.1,0,0,0]}}}]};
  const descriptor={tex:{offset:0,width:2,height:2,levels:1,colorspace:'raw'}};
  const rgb=compileGraph(doc,{output:{nodename:'tex',output:'rgb'},imageDescriptors:descriptor});
  assert.match(rgb.body,/ctx\.uv/); assert.match(rgb.body,/\*vec4f\(2\.0,2\.0,2\.0,1\.0\)\+vec4f\(0\.1,0\.0,0\.0,0\.0\)/);
  const red=compileGraph(doc,{output:{nodename:'tex',output:'r'},imageDescriptors:descriptor}); assert.match(red.body,/\)\.r/);
  assert.throws(()=>compileGraph({...doc,nodes:[{...doc.nodes[0],inputs:{...doc.nodes[0].inputs,sourceColorSpace:{type:'string',value:'srgb_texture'}}}]},{output:{nodename:'tex',output:'rgb'},imageDescriptors:descriptor}),/sourceColorSpace differs/);
});
test('USD primvar readers and transform2d resolve standard geometry inputs', () => {
  const uv=compileGraph({nodes:[{name:'u',category:'UsdPrimvarReader',type:'vector2',inputs:{varname:{type:'string',value:'st'},fallback:{type:'vector2',value:[.2,.3]}}}]});
  assert.match(uv.body,/ctx\.uv/);
  const fallback=compileGraph({nodes:[{name:'c',category:'UsdPrimvarReader',type:'color3',inputs:{varname:{type:'string',value:'displayColor'},fallback:{type:'color3',value:[.2,.3,.4]}}}]});
  assert.match(fallback.body,/vec3f\(0\.2,0\.3,0\.4\)/);
  const transformed=compileGraph({nodes:[{name:'t',category:'UsdTransform2d',type:'vector2',inputs:{in:{type:'vector2',value:[1,0]},scale:{type:'vector2',value:[2,1]},rotation:{type:'float',value:90},translation:{type:'vector2',value:[.1,.2]}}}]});
  assert.match(transformed.body,/mat2x2f\(cos\(/); assert.match(transformed.body,/vec2f\(0\.1,0\.2\)/);
});
test('triplanarprojection blends three typed image planes by normal weights', () => {
  const descriptor={x:{offset:0,width:2,height:2,levels:1,colorspace:'raw'},y:{offset:4,width:2,height:2,levels:1,colorspace:'raw'},z:{offset:8,width:2,height:2,levels:1,colorspace:'raw'}};
  const doc={nodes:[{name:'tri',category:'triplanarprojection',type:'color3',inputs:{filex:{type:'filename',value:'x'},filey:{type:'filename',value:'y'},filez:{type:'filename',value:'z'},normal:{type:'vector3',value:[1,0,0]},filtertype:{type:'string',value:'linear'}}}]};
  const source=compileGraph(doc,{imageDescriptors:descriptor}); assert.equal((source.body.match(/imageSample\(/g)||[]).length,3); assert.match(source.body,/safeNormal\(vec3f\(1\.0,0\.0,0\.0\),vec3f\(0\.0,0\.0,1\.0\)\)/);
  assert.throws(()=>compileGraph({...doc,nodes:[{...doc.nodes[0],inputs:{...doc.nodes[0].inputs,filez:{type:'filename',value:'missing'}}}]},{imageDescriptors:descriptor}),/missing decoded image/);
  assert.throws(()=>compileGraph({...doc,nodes:[{...doc.nodes[0],colorspace:'acescg'}]},{imageDescriptors:descriptor}),/filex colorspace/);
});
test('finite typed literals cannot inject shader code', () => {
  assert.equal(literal('color3', [1, 0.25, 0]), 'vec3f(1.0,0.25,0.0)');
  for (const v of [NaN, Infinity, '1.0); return;', [1, 2]]) assert.throws(() => literal('float', v), GraphError);
  assert.throws(() => literal('integer', 1.5)); assert.throws(() => literal('boolean', 'yes'));
});
test('shared dependency emitted once and deterministic', () => {
  const doc = { nodes: [constant('a', 2), { name: 'sum', category: 'add', type: 'float', inputs: { in1: { nodename: 'a' }, in2: { nodename: 'a' } } }] };
  const a = compileGraph(doc); assert.equal(a.body.split('\n').length, 2); assert.deepEqual(a, compileGraph(doc));
});
test('screen and difference compositing nodes preserve numeric types', () => {
  const doc={nodes:[
    {name:'a',category:'constant',type:'color3',inputs:{value:{type:'color3',value:[.2,.4,.6]}}},
    {name:'b',category:'constant',type:'color3',inputs:{value:{type:'color3',value:[.1,.3,.5]}}},
    {name:'screen',category:'screen',type:'color3',inputs:{in1:{nodename:'a'},in2:{nodename:'b'}}},
    {name:'out',category:'difference',type:'color3',inputs:{in1:{nodename:'screen'},in2:{nodename:'b'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/vec3f\(1\.0\)-\(vec3f\(1\.0\)-n0\)/); assert.match(source.body,/abs\(/);
});
test('MaterialX compositing nodes preserve typed blend controls and alpha coverage', () => {
  const burn=compileGraph({nodes:[{name:'b',category:'burn',type:'color3',inputs:{fg:{type:'color3',value:[.25,.5,.75]},bg:{type:'color3',value:[.8,.6,.4]},mix:{type:'float',value:.5}}}]});
  assert.match(burn.body,/mix\(vec3f\(0\.8,0\.6,0\.4\)/); assert.match(burn.body,/max\(vec3f\(0\.25,0\.5,0\.75\),vec3f\(1e-6\)\)/);
  const overlay=compileGraph({nodes:[{name:'o',category:'overlay',type:'float',inputs:{fg:{type:'float',value:.25},bg:{type:'float',value:.75}}}]});
  assert.match(overlay.body,/select\(2\.0\*/); assert.match(overlay.body,/1\.0-2\.0/);
  const disjoint=compileGraph({nodes:[{name:'d',category:'disjointover',type:'color4',inputs:{fg:{type:'color4',value:[1,0,0,.7]},bg:{type:'color4',value:[0,1,0,.6]}}}]});
  assert.match(disjoint.body,/min\(.*\.a\+.*\.a,1\.0\)/); assert.match(disjoint.body,/max\(.*\.a,1e-6\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'d',category:'disjointover',type:'color3',inputs:{}}]}),/requires color4/);
  assert.throws(()=>compileGraph({nodes:[{name:'b',category:'burn',type:'vector3',inputs:{}}]}),/requires float\/color3\/color4/);
});
test('MaterialX alpha compositing and mask nodes preserve Porter-Duff channels', () => {
  const over=compileGraph({nodes:[{name:'o',category:'over',type:'color4',inputs:{fg:{type:'color4',value:[1,0,0,.6]},bg:{type:'color4',value:[0,1,0,.5]}}}]});
  assert.match(over.body,/vec4f\(/); assert.match(over.body,/\.rgb\+.*\.rgb\*\(1\.0-.*\.a\)/); assert.match(over.body,/\.a\+.*\.a\*\(1\.0-.*\.a\)/);
  const masked=compileGraph({nodes:[{name:'m',category:'mask',type:'color4',inputs:{fg:{type:'color4',value:[1,0,0,.6]},bg:{type:'color4',value:[0,1,0,.5]}}}]});
  assert.match(masked.body,/\.rgb\*.*\.a/); assert.match(masked.body,/\.a\*.*\.a/);
  const matte=compileGraph({nodes:[{name:'m',category:'matte',type:'color4',inputs:{fg:{type:'color4',value:[1,0,0,.6]},bg:{type:'color4',value:[0,1,0,.5]}}}]});
  assert.match(matte.body,/\.rgb\*.*\.a\+.*\.rgb\*\(1\.0-.*\.a\)/);
  const inside=compileGraph({nodes:[{name:'i',category:'inside',type:'color3',inputs:{in:{type:'color3',value:[1,.5,0]},mask:{type:'float',value:.25}}}]});
  assert.match(inside.body,/vec3f\(1\.0,0\.5,0\.0\)\*vec3f\(0\.25\)/);
  const outside=compileGraph({nodes:[{name:'o',category:'outside',type:'float',inputs:{in:{type:'float',value:.8},mask:{type:'float',value:.25}}}]});
  assert.match(outside.body,/0\.8\*f32\(\(1\.0-0\.25\)\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'i',category:'inside',type:'vector2',inputs:{}}]}),/requires float\/color3\/color4/);
});
test('boolean logic nodes enforce boolean ports', () => {
  const doc={nodes:[
    {name:'a',category:'constant',type:'boolean',inputs:{value:{type:'boolean',value:true}}},
    {name:'b',category:'constant',type:'boolean',inputs:{value:{type:'boolean',value:false}}},
    {name:'and',category:'and',type:'boolean',inputs:{in1:{nodename:'a'},in2:{nodename:'b'}}},
    {name:'not',category:'not',type:'boolean',inputs:{in:{nodename:'and'}}},
    {name:'out',category:'xor',type:'boolean',inputs:{in1:{nodename:'not'},in2:{nodename:'b'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/&&/); assert.match(source.body,/!/); assert.match(source.body,/!=/);
  assert.throws(()=>compileGraph({...doc,nodes:[{...doc.nodes[2],type:'float'}]}),/and requires boolean inputs/);
});
test('vector reflection, refraction and distance nodes compile with typed ports', () => {
  const doc={nodes:[
    {name:'in',category:'constant',type:'vector3',inputs:{value:{type:'vector3',value:[0,0,-1]}}},
    {name:'normal',category:'constant',type:'vector3',inputs:{value:{type:'vector3',value:[0,0,1]}}},
    {name:'r',category:'reflect',type:'vector3',inputs:{in:{nodename:'in'},normal:{nodename:'normal'}}},
    {name:'t',category:'refract',type:'vector3',inputs:{in:{nodename:'in'},normal:{nodename:'normal'},ior:{type:'float',value:1.5}}},
    {name:'d',category:'distance',type:'float',inputs:{in1:{nodename:'r'},in2:{nodename:'t'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/reflect\(/); assert.match(source.body,/refract\(/); assert.match(source.body,/distance\(/);
});
test('fresnel and facing-ratio nodes compile with safe vector normalization', () => {
  const doc={nodes:[
    {name:'direction',category:'constant',type:'vector3',inputs:{value:{type:'vector3',value:[0,0,-1]}}},
    {name:'normal',category:'normal',type:'vector3',inputs:{}},
    {name:'f',category:'fresnel',type:'float',inputs:{in:{nodename:'direction'},normal:{nodename:'normal'},ior:{type:'float',value:1.5}}},
    {name:'face',category:'facing_ratio',type:'float',inputs:{in:{nodename:'direction'},normal:{nodename:'normal'},exponent:{type:'float',value:2}}},
    {name:'out',category:'add',type:'float',inputs:{in1:{nodename:'f'},in2:{nodename:'face'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/pow\(\(1\.5-1\.0\)/); assert.match(source.body,/normalize\(n1\)/);
});
test('normalize uses finite fallbacks for vector widths', () => {
  const v2=compileGraph({nodes:[{name:'n',category:'normalize',type:'vector2',inputs:{in:{type:'vector2',value:[0,0]}}}]});
  assert.match(v2.body,/mxSafeNormalize2\(vec2f\(0\.0,0\.0\),vec2f\(0\.0,1\.0\)\)/);
  const v4=compileGraph({nodes:[{name:'n',category:'normalize',type:'vector4',inputs:{in:{type:'vector4',value:[1,2,3,4]}}}]});
  assert.match(v4.body,/mxSafeNormalize4/); assert.match(contextWGSL,/fn mxSafeNormalize4/);
  assert.throws(()=>compileGraph({nodes:[{name:'n',category:'normalize',type:'float',inputs:{in:{type:'float',value:1}}}]}),/requires a vector/);
});
test('reorder remaps typed channels with the same bounds as swizzle', () => {
  const source=compileGraph({nodes:[{name:'r',category:'reorder',type:'color3',inputs:{in:{type:'color4',value:[.1,.2,.3,.4]},channels:{type:'string',value:'bgr'}}}]});
  assert.match(source.body,/vec3f\(vec4f\(0\.1,0\.2,0\.3,0\.4\)\[2\],vec4f\(0\.1,0\.2,0\.3,0\.4\)\[1\],vec4f\(0\.1,0\.2,0\.3,0\.4\)\[0\]\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'r',category:'reorder',type:'color3',inputs:{in:{type:'color3',value:[1,0,0]},channels:{type:'string',value:'rgba'}}}]}),/invalid channels/);
});
test('luminance and average nodes preserve explicit component semantics', () => {
  const doc={nodes:[
    {name:'color',category:'constant',type:'color3',inputs:{value:{type:'color3',value:[1,.5,0]}}},
    {name:'lum',category:'luminance',type:'float',inputs:{in:{nodename:'color'}}},
    {name:'avg',category:'average',type:'float',inputs:{in:{nodename:'color'}}},
    {name:'out',category:'add',type:'float',inputs:{in1:{nodename:'lum'},in2:{nodename:'avg'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/0\.2126,0\.7152,0\.0722/); assert.match(source.body,/0\.3333333333/);
});
test('RGB/HSV conversion nodes emit bounded color conversion helpers', () => {
  const doc={nodes:[
    {name:'rgb',category:'constant',type:'color3',inputs:{value:{type:'color3',value:[.2,.4,.8]}}},
    {name:'hsv',category:'rgbtohsv',type:'color3',inputs:{in:{nodename:'rgb'}}},
    {name:'out',category:'hsvtorgb',type:'color3',inputs:{in:{nodename:'hsv'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/mxRgbToHsv/); assert.match(source.body,/mxHsvToRgb/);
});
test('color utility nodes compile with typed alpha and HSV controls', () => {
  const doc={nodes:[
    {name:'rgba',category:'constant',type:'color4',inputs:{value:{type:'color4',value:[.2,.4,.6,.5]}}},
    {name:'premult',category:'premult',type:'color4',inputs:{in:{nodename:'rgba'}}},
    {name:'unpremult',category:'unpremult',type:'color4',inputs:{in:{nodename:'premult'}}},
    {name:'hsv',category:'hsvadjust',type:'color3',inputs:{in:{type:'color3',value:[1,0,0]},amount:{type:'vector3',value:[.25,.8,1.2]}}},
    {name:'out',category:'contrast',type:'color3',inputs:{in:{nodename:'hsv'},amount:{type:'float',value:1.1},pivot:{type:'float',value:.5}}}
  ]};
  const alpha=compileGraph({nodes:doc.nodes.slice(0,3)}); assert.match(alpha.body,/vec4f\(n0\.rgb\*n0\.a,n0\.a\)/); assert.match(alpha.body,/max\(n1\.a,1e-6\)/);
  const source=compileGraph(doc); assert.match(source.body,/mxRgbToHsv/); assert.match(source.body,/fract/); assert.match(source.body,/1\.1/);
  const color4=compileGraph({nodes:[{name:'hsv',category:'hsvadjust',type:'color4',inputs:{in:{type:'color4',value:[1,0,0,.25]},amount:{type:'vector3',value:[0,1,1]}}}]});
  assert.match(color4.body,/vec4f\(mxHsvToRgb/); assert.match(color4.body,/\.a/);
  const saturated=compileGraph({nodes:[{name:'s',category:'saturate',type:'color4',inputs:{in:{type:'color4',value:[1,.2,.1,.4]},amount:{type:'float',value:0},lumacoeffs:{type:'color3',value:[.2,.7,.1]}}}]});
  assert.match(saturated.body,/mix\(vec3f\(dot\(/); assert.match(saturated.body,/\.a/);
  assert.throws(()=>compileGraph({...doc,nodes:[{...doc.nodes[2],type:'color3'}]}),/unpremult output must be color4/);
});
test('common trigonometric and angle-unit nodes map to WGSL math', () => {
  const doc={nodes:[
    {name:'x',category:'constant',type:'float',inputs:{value:{type:'float',value:45}}},
    {name:'r',category:'radians',type:'float',inputs:{in:{nodename:'x'}}},
    {name:'a',category:'atan',type:'float',inputs:{in:{nodename:'r'}}},
    {name:'l',category:'log10',type:'float',inputs:{in:{nodename:'x'}}},
    {name:'e',category:'exp2',type:'float',inputs:{in:{nodename:'x'}}},
    {name:'sum',category:'add',type:'float',inputs:{in1:{nodename:'a'},in2:{nodename:'l'}}},
    {name:'out',category:'add',type:'float',inputs:{in1:{nodename:'sum'},in2:{nodename:'e'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/0\.017453292519943295/); assert.match(source.body,/log\(n0\)\*0\.4342944819032518/); assert.match(source.body,/exp2\(n0\)/);
});
test('safepower preserves the sign of negative bases', () => {
  const source=compileGraph({nodes:[{name:'p',category:'safepower',type:'float',inputs:{in1:{type:'float',value:-2},in2:{type:'float',value:3}}}]});
  assert.match(source.body,/sign\(-2\.0\)\*pow\(abs\(-2\.0\),3\.0\)/);
});
test('place2d applies pivot, inverse scale, degree rotation and offset', () => {
  const source=compileGraph({nodes:[{name:'p',category:'place2d',type:'vector2',inputs:{texcoord:{type:'vector2',value:[1,0]},pivot:{type:'vector2',value:[.5,.5]},scale:{type:'vector2',value:[2,2]},rotate:{type:'float',value:90},offset:{type:'vector2',value:[.1,.2]}}}]});
  assert.match(source.body,/mat2x2f\(cos\(/); assert.match(source.body,/0\.017453292519943295/); assert.match(source.body,/vec2f\(0\.1,0\.2\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'p',category:'place2d',type:'color3',inputs:{}}]}),/place2d output must be vector2/);
  assert.match(compileGraph({nodes:[{name:'p',category:'place2d',type:'vector2',inputs:{}}]}).body,/ctx\.uv/);
});
test('geompropvalue maps standard authored geometry properties with typed fallbacks', () => {
  const uv=compileGraph({nodes:[{name:'uv',category:'geompropvalue',type:'vector2',inputs:{geomprop:{type:'string',value:'st'}}}]});
  assert.equal(uv.expression,'n0'); assert.match(uv.body,/ctx\.uv/);
  const normal=compileGraph({nodes:[{name:'n',category:'geompropvalue',type:'vector3',inputs:{geomprop:{type:'string',value:'N'}}}]});
  assert.match(normal.body,/ctx\.normal/);
  const fallback=compileGraph({nodes:[{name:'x',category:'geompropvalue',type:'color3',inputs:{geomprop:{type:'string',value:'custom'},default:{type:'color3',value:[.2,.3,.4]}}}]});
  assert.match(fallback.body,/vec3f\(0\.2,0\.3,0\.4\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'bad',category:'geompropvalue',type:'float',inputs:{geomprop:{type:'string',value:'P'}}}]}),/has type vector3/);
});
test('standard geometry aliases preserve facing ratio and uniform property semantics', () => {
  const facing=compileGraph({nodes:[{name:'f',category:'facingratio',type:'float',inputs:{viewdirection:{type:'vector3',value:[0,0,1]},normal:{type:'vector3',value:[0,0,1]},faceforward:{type:'boolean',value:true},invert:{type:'boolean',value:true}}}]});
  assert.match(facing.body,/abs\(dot\(/); assert.match(facing.body,/1\.0-select\(/);
  const uniform=compileGraph({nodes:[{name:'u',category:'geompropvalueuniform',type:'vector3',inputs:{geomprop:{type:'string',value:'P'}}}]});
  assert.match(uniform.body,/ctx\.position/);
  const fallback=compileGraph({nodes:[{name:'u',category:'geompropvalueuniform',type:'float',inputs:{geomprop:{type:'string',value:'custom'},default:{type:'float',value:.25}}}]});
  assert.match(fallback.body,/0\.25/);
  assert.throws(()=>compileGraph({nodes:[{name:'u',category:'geompropvalueuniform',type:'float',inputs:{geomprop:{type:'string',value:'N'}}}]}),/has type vector3/);
  const color=compileGraph({nodes:[{name:'c',category:'geomcolor',type:'color4',inputs:{index:{type:'integer',value:0}}}]});
  assert.match(color.body,/ctx\.geomcolor/); assert.throws(()=>compileGraph({nodes:[{name:'c',category:'geomcolor',type:'color3',inputs:{index:{type:'integer',value:1}}}]}),/primary geometry color/);
});
test('viewdirection exposes the normalized outgoing shading direction', () => {
  const source=compileGraph({nodes:[{name:'v',category:'viewdirection',type:'vector3',inputs:{space:{type:'string',value:'world'}}}]});
  assert.match(source.expression,/^n\d+$/);
  assert.match(source.body,/ctx\.viewdir/);
  assert.throws(()=>compileGraph({nodes:[{name:'v',category:'viewdirection',type:'float',inputs:{}}]}),/output must be vector3/);
  assert.throws(()=>compileGraph({nodes:[{name:'v',category:'viewdirection',type:'vector3',inputs:{space:{type:'string',value:'tangent'}}}]}),/world-space/);
});
test('rotate3d uses a normalized axis and degree-valued Rodrigues rotation', () => {
  const source=compileGraph({nodes:[{name:'r',category:'rotate3d',type:'vector3',inputs:{in:{type:'vector3',value:[1,0,0]},axis:{type:'vector3',value:[0,0,1]},amount:{type:'float',value:90}}}]});
  assert.match(source.body,/safeNormal\(vec3f\(0\.0,0\.0,1\.0\)/);
  assert.match(source.body,/cross\(/); assert.match(source.body,/0\.017453292519943295/);
  assert.throws(()=>compileGraph({nodes:[{name:'r',category:'rotate3d',type:'color3',inputs:{}}]}),/rotate3d output must be vector3/);
});
test('core math nodes honor MaterialX default bounds and amounts', () => {
  const doc={nodes:[
    {name:'x',category:'constant',type:'float',inputs:{value:{type:'float',value:.25}}},
    {name:'clamp',category:'clamp',type:'float',inputs:{in:{nodename:'x'}}},
    {name:'smooth',category:'smoothstep',type:'float',inputs:{in:{nodename:'clamp'}}},
    {name:'invert',category:'invert',type:'float',inputs:{in:{nodename:'smooth'}}},
    {name:'out',category:'mix',type:'float',inputs:{bg:{nodename:'invert'},fg:{nodename:'x'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/clamp\(n0,0\.0,1\.0\)/); assert.match(source.body,/smoothstep\(0\.0,1\.0/); assert.match(source.body,/mix\(n3,n0,0\.0\)/);
});
test('ramp4 evaluates four corners with bilinear interpolation', () => {
  const source=compileGraph({nodes:[{name:'r',category:'ramp4',type:'color3',inputs:{texcoord:{type:'vector2',value:[.25,.75]},valuetl:{type:'color3',value:[1,0,0]},valuetr:{type:'color3',value:[0,1,0]},valuebl:{type:'color3',value:[0,0,1]},valuebr:{type:'color3',value:[1,1,1]}}}]});
  assert.match(source.body,/mix\(mix\(vec3f\(0\.0,0\.0,1\.0\),vec3f\(1\.0,1\.0,1\.0\),vec2f\(0\.25,0\.75\)\.x\),mix/);
  assert.match(compileGraph({nodes:[{name:'r',category:'ramp4',type:'float',inputs:{valuetl:{type:'float',value:0},valuetr:{type:'float',value:1},valuebl:{type:'float',value:1},valuebr:{type:'float',value:0}}}]}).body,/ctx\.uv/);
});
test('ACEScg conversion node uses the pinned MaterialX matrix', () => {
  const doc={nodes:[{name:'aces',category:'acescg_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[1,0,0]}}}]};
  const source=compileGraph(doc); assert.match(source.body,/mxAcescgToLinRec709/); assert.match(contextWGSL,/1\.705050992658/);
});
test('linear Rec.709 to ACEScg conversion exposes the inverse matrix', () => {
  const doc={nodes:[{name:'lin',category:'lin_rec709_to_acescg',type:'color3',inputs:{in:{type:'color3',value:[1,0,0]}}}]};
  const source=compileGraph(doc); assert.match(source.body,/mxLinRec709ToAcescg/); assert.match(contextWGSL,/(?:0)?\.613097402401/);
});
test('sRGB transfer nodes preserve the signed piecewise transfer', () => {
  const doc={nodes:[
    {name:'lin',category:'lin_rec709_to_srgb',type:'color3',inputs:{in:{type:'color3',value:[.18,-.01,.8]}}},
    {name:'out',category:'srgb_to_lin_rec709',type:'color3',inputs:{in:{nodename:'lin'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/mxLinRec709ToSrgb/); assert.match(source.body,/mxSrgbToLinRec709/); assert.match(contextWGSL,/(?:0)?\.0031308/);
});
test('MaterialX cmlib gamma and texture transforms preserve color4 alpha', () => {
  const gamma=compileGraph({nodes:[{name:'g',category:'g22_rec709_to_lin_rec709',type:'color4',inputs:{in:{type:'color4',value:[.25,.5,.75,.4]}}}]});
  assert.match(gamma.body,/pow\(max\(vec4f\(0\.25,0\.5,0\.75,0\.4\)\.rgb,vec3f\(0\.0\)\),vec3f\(2\.2\)\)/); assert.match(gamma.body,/\.a/);
  const display=compileGraph({nodes:[{name:'d',category:'rec709_display_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[.25,.5,.75]}}}]});
  assert.match(display.body,/vec3f\(2\.4\)/);
  const texture=compileGraph({nodes:[{name:'s',category:'srgb_texture_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[.25,.5,.75]}}}]});
  assert.match(texture.body,/mxSrgbToLinRec709/);
  const ap1=compileGraph({nodes:[{name:'a',category:'g22_ap1_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[.25,.5,.75]}}}]});
  assert.match(ap1.body,/mxAcescgToLinRec709/); assert.match(ap1.body,/vec3f\(2\.2\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'g',category:'g18_rec709_to_lin_rec709',type:'float',inputs:{}}]}),/output must be color3\/color4/);
});
test('MaterialX cmlib Adobe RGB and Display P3 transforms use pinned matrices', () => {
  const adobe=compileGraph({nodes:[{name:'a',category:'adobergb_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[.25,.5,.75]}}}]});
  assert.match(adobe.body,/2\.19921875/); assert.match(adobe.body,/1\.39835574/);
  const linearAdobe=compileGraph({nodes:[{name:'a',category:'lin_adobergb_to_lin_rec709',type:'color4',inputs:{in:{type:'color4',value:[.25,.5,.75,.4]}}}]});
  assert.match(linearAdobe.body,/1\.39835574/); assert.match(linearAdobe.body,/\.a/);
  const p3=compileGraph({nodes:[{name:'p',category:'srgb_displayp3_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[.25,.5,.75]}}}]});
  assert.match(p3.body,/mxSrgbToLinRec709/); assert.match(p3.body,/1\.22493029/);
  const linearP3=compileGraph({nodes:[{name:'p',category:'lin_displayp3_to_lin_rec709',type:'color3',inputs:{in:{type:'color3',value:[.25,.5,.75]}}}]});
  assert.match(linearP3.body,/1\.22493029/); assert.doesNotMatch(linearP3.body,/mxSrgbToLinRec709/);
});
test('matrix utility nodes preserve typed dimensions and vector columns', () => {
  const matrix={type:'matrix33',value:[1,0,0,0,2,0,0,0,3]};
  const transpose=compileGraph({nodes:[{name:'t',category:'transpose',type:'matrix33',inputs:{in:matrix}}]});
  assert.match(transpose.body,/transpose\(mat3x3f\(/);
  const determinant=compileGraph({nodes:[{name:'d',category:'determinant',type:'float',inputs:{in:matrix}}]});
  assert.match(determinant.body,/determinant\(mat3x3f\(/);
  const inverse=compileGraph({nodes:[{name:'i',category:'invertmatrix',type:'matrix33',inputs:{in:matrix}}]});
  assert.match(inverse.body,/inverse\(mat3x3f\(/);
  const create=compileGraph({nodes:[{name:'m',category:'creatematrix',type:'matrix44',inputs:{in1:{type:'vector3',value:[1,0,0]},in2:{type:'vector3',value:[0,1,0]},in3:{type:'vector3',value:[0,0,1]},in4:{type:'vector3',value:[2,3,4]}}}]});
  assert.match(create.body,/mat4x4f\(vec4f\(vec3f\(1\.0,0\.0,0\.0\),0\.0\)/); assert.match(create.body,/vec4f\(vec3f\(2\.0,3\.0,4\.0\),1\.0\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'d',category:'determinant',type:'float',inputs:{in:{type:'vector3',value:[1,2,3]}}}]}),/matrix33\/matrix44/);
});
test('select node enforces boolean condition and matching branch types', () => {
  const doc={nodes:[
    {name:'condition',category:'constant',type:'boolean',inputs:{value:{type:'boolean',value:true}}},
    {name:'yes',category:'constant',type:'color3',inputs:{value:{type:'color3',value:[1,0,0]}}},
    {name:'no',category:'constant',type:'color3',inputs:{value:{type:'color3',value:[0,0,1]}}},
    {name:'out',category:'select',type:'color3',inputs:{condition:{nodename:'condition'},truevalue:{nodename:'yes'},falsevalue:{nodename:'no'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/select\(n2,n1,n0\)/);
  assert.throws(()=>compileGraph({...doc,nodes:[...doc.nodes.slice(0,-1),{...doc.nodes.at(-1),type:'float'}]}),/select branches/);
});
test('procedural noise nodes compile deterministic bounded WGSL helpers', () => {
  const doc={nodes:[
    {name:'uv',category:'texcoord',type:'vector2',inputs:{}},
    {name:'n2',category:'noise2d',type:'float',inputs:{in:{nodename:'uv'}}},
    {name:'p',category:'position',type:'vector3',inputs:{}},
    {name:'n3',category:'noise3d',type:'float',inputs:{in:{nodename:'p'}}},
    {name:'out',category:'add',type:'float',inputs:{in1:{nodename:'n2'},in2:{nodename:'n3'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/mxNoise2/); assert.match(source.body,/mxNoise3/); assert.match(contextWGSL,/mxHash3/);
});
test('noise controls affect the coordinate and output without silent octave loss', () => {
  const source=compileGraph({nodes:[{name:'n',category:'noise2d',type:'float',inputs:{in:{type:'vector2',value:[.25,.5]},scale:{type:'float',value:2},amplitude:{type:'float',value:.75},pivot:{type:'float',value:.2}}}]});
  assert.match(source.body,/mxNoise2\(vec2f\(0\.25,0\.5\)\*2\.0\)/);
  assert.match(source.body,/\*0\.75\+0\.2/);
  assert.throws(()=>compileGraph({nodes:[{name:'n',category:'noise3d',type:'float',inputs:{in:{type:'vector3',value:[0,0,0]},octaves:{type:'integer',value:3}}}]}),/octaves/);
});
test('fractal noise emits bounded fBm helpers with authored controls', () => {
  const source=compileGraph({nodes:[{name:'f',category:'fractal2d',type:'color3',inputs:{texcoord:{type:'vector2',value:[.1,.2]},amplitude:{type:'color3',value:[1,.5,.25]},octaves:{type:'integer',value:4},lacunarity:{type:'float',value:2},diminish:{type:'float',value:.5}}}]});
  assert.match(source.body,/mxFractal2\(vec2f\(0\.1,0\.2\),4i,2\.0,0\.5\)/); assert.match(source.body,/vec3f\(1\.0,0\.5,0\.25\)/);
  const source3=compileGraph({nodes:[{name:'f',category:'fractal3d',type:'float',inputs:{texcoord:{type:'vector3',value:[0,0,0]}}}]});
  assert.match(source3.body,/mxFractal3/); assert.match(contextWGSL,/for\(var i=0i;i<8i/);
});
test('Worley noise nodes provide bounded distance and hash channels', () => {
  const a=compileGraph({nodes:[{name:'w',category:'worleynoise2d',type:'vector3',inputs:{texcoord:{type:'vector2',value:[.1,.2]},jitter:{type:'float',value:.8},style:{type:'integer',value:0}}}]});
  assert.match(a.body,/mxWorley2\(vec2f\(0\.1,0\.2\),0\.8,0i\)/); assert.match(contextWGSL,/for\(var y=-1i/);
  const b=compileGraph({nodes:[{name:'w',category:'worleynoise3d',type:'float',inputs:{position:{type:'vector3',value:[0,0,0]}}}]});
  assert.match(b.body,/mxWorley3/); assert.throws(()=>compileGraph({nodes:[{name:'w',category:'worleynoise2d',type:'color3',inputs:{}}]}),/supports float/);
});
test('latlongimage maps view direction to periodic longitude and clamped latitude', () => {
  const doc={images:{env:{width:4,height:2,data:[1,1,1,1],colorspace:'raw'}},nodes:[{name:'e',category:'latlongimage',type:'color3',inputs:{file:{type:'filename',value:'env'},viewdir:{type:'vector3',value:[1,0,0]},rotation:{type:'float',value:90},default:{type:'color3',value:[.1,.2,.3]}}}]};
  const source=compileGraph(doc,{imageDescriptors:{env:{offset:0,width:4,height:2,levels:1,colorspace:'raw'}}});
  assert.match(source.body,/atan2\(safeNormal\(vec3f\(1\.0,0\.0,0\.0\)/); assert.match(source.body,/vec2u\(2u,1u\)/);
  assert.throws(()=>compileGraph({...doc,nodes:[{...doc.nodes[0],type:'float'}]},{imageDescriptors:{env:{offset:0,width:4,height:2,levels:1,colorspace:'raw'}}}),/output must be color3/);
});
test('splitlr and splittb select typed matte values from UV coordinates', () => {
  const lr=compileGraph({nodes:[{name:'s',category:'splitlr',type:'color3',inputs:{valuel:{type:'color3',value:[1,0,0]},valuer:{type:'color3',value:[0,1,0]},center:{type:'float',value:.4},texcoord:{type:'vector2',value:[.2,.5]}}}]});
  assert.match(lr.body,/select\(vec3f\(0\.0,1\.0,0\.0\),vec3f\(1\.0,0\.0,0\.0\),vec2f\(0\.2,0\.5\)\.x<0\.4\)/);
  const tb=compileGraph({nodes:[{name:'s',category:'splittb',type:'float',inputs:{valuet:{type:'float',value:1},valueb:{type:'float',value:2},texcoord:{type:'vector2',value:[.2,.8]}}}]});
  assert.match(tb.body,/\.y>=0\.5/);
});
test('ramp and ramp_gradient compile static control points and interpolation modes', () => {
  const gradient=compileGraph({nodes:[{name:'g',category:'ramp_gradient',type:'color4',inputs:{x:{type:'float',value:.25},interval1:{type:'float',value:0},interval2:{type:'float',value:1},color1:{type:'color4',value:[0,0,0,1]},color2:{type:'color4',value:[1,0,0,1]},interpolation:{type:'integer',value:1}}}]});
  assert.match(gradient.body,/smoothstep/);
  const ramp=compileGraph({nodes:[{name:'r',category:'ramp',type:'color4',inputs:{texcoord:{type:'vector2',value:[.75,.5]},num_intervals:{type:'integer',value:3},interval1:{type:'float',value:0},color1:{type:'color4',value:[0,0,0,1]},interval2:{type:'float',value:.5},color2:{type:'color4',value:[1,0,0,1]},interval3:{type:'float',value:1},color3:{type:'color4',value:[1,1,1,1]}}}]});
  assert.match(ramp.body,/select\(/); assert.match(ramp.body,/vec2f\(0\.75,0\.5\)\.x/);
  assert.throws(()=>compileGraph({nodes:[{name:'r',category:'ramp',type:'color3',inputs:{}}]}),/ramp output must be color4/);
});
test('ramplr and ramptb interpolate typed values along the requested UV axis', () => {
  const lr=compileGraph({nodes:[{name:'r',category:'ramplr',type:'color3',inputs:{valuel:{type:'color3',value:[1,0,0]},valuer:{type:'color3',value:[0,1,0]},texcoord:{type:'vector2',value:[.25,.8]}}}]});
  assert.match(lr.body,/mix\(vec3f\(1\.0,0\.0,0\.0\),vec3f\(0\.0,1\.0,0\.0\),vec2f\(0\.25,0\.8\)\.x\)/);
  const tb=compileGraph({nodes:[{name:'r',category:'ramptb',type:'float',inputs:{valuet:{type:'float',value:1},valueb:{type:'float',value:0},texcoord:{type:'vector2',value:[.25,.8]}}}]});
  assert.match(tb.body,/mix\(0\.0,1\.0,vec2f\(0\.25,0\.8\)\.y\)/);
});
test('checkerboard applies UV tiling/offset and alternates typed colors', () => {
  const source=compileGraph({nodes:[{name:'c',category:'checkerboard',type:'color3',inputs:{texcoord:{type:'vector2',value:[.1,.2]},uvtiling:{type:'vector2',value:[4,4]},uvoffset:{type:'vector2',value:[.25,0]},color1:{type:'color3',value:[1,0,0]},color2:{type:'color3',value:[0,0,1]}}}]});
  assert.match(source.body,/floor\(\(vec2f\(0\.1,0\.2\)\*vec2f\(4\.0,4\.0\)\+vec2f\(0\.25,0\.0\)\)\.x\)/); assert.match(source.body,/select\(vec3f\(0\.0,0\.0,1\.0\)/);
});
test('circle and line procedural masks use bounded UV distance tests', () => {
  const circle=compileGraph({nodes:[{name:'c',category:'circle',type:'float',inputs:{texcoord:{type:'vector2',value:[.5,.5]},center:{type:'vector2',value:[.5,.5]},radius:{type:'float',value:.25}}}]});
  assert.match(circle.body,/distance\(vec2f\(0\.5,0\.5\),vec2f\(0\.5,0\.5\)\)<=max/);
  const line=compileGraph({nodes:[{name:'l',category:'line',type:'float',inputs:{texcoord:{type:'vector2',value:[.5,.5]},point1:{type:'vector2',value:[0,0]},point2:{type:'vector2',value:[1,1]},radius:{type:'float',value:.1}}}]});
  assert.match(line.body,/clamp\(dot\(/);
});
test('grid and crosshatch patterns apply typed tiling, offset, thickness and staggering', () => {
  const grid=compileGraph({nodes:[{name:'g',category:'grid',type:'color3',inputs:{texcoord:{type:'vector2',value:[.2,.3]},uvtiling:{type:'vector2',value:[4,4]},thickness:{type:'float',value:.1},staggered:{type:'boolean',value:true}}}]});
  assert.match(grid.body,/fract\(/); assert.match(grid.body,/vec3f\(/);
  const cross=compileGraph({nodes:[{name:'c',category:'crosshatch',type:'color3',inputs:{texcoord:{type:'vector2',value:[.2,.3]},thickness:{type:'float',value:.05}}}]});
  assert.match(cross.body,/max\(/); assert.match(cross.body,/fract\(/);
});
test('tiledcircles generates cell-local circle masks with authored size', () => {
  const source=compileGraph({nodes:[{name:'c',category:'tiledcircles',type:'color3',inputs:{texcoord:{type:'vector2',value:[.2,.3]},uvtiling:{type:'vector2',value:[2,3]},size:{type:'float',value:.6},staggered:{type:'boolean',value:true}}}]});
  assert.match(source.body,/fract\(/); assert.match(source.body,/length\(/); assert.match(source.body,/vec3f\(/);
});
test('randomfloat hashes input and seed within authored bounds', () => {
  const source=compileGraph({nodes:[{name:'r',category:'randomfloat',type:'float',inputs:{in:{type:'float',value:2},min:{type:'float',value:-1},max:{type:'float',value:3},seed:{type:'integer',value:7}}}]});
  assert.match(source.body,/mix\(-1\.0,3\.0,mxHash2\(vec2f\(2\.0,f32\(7i\)\)\)\)/);
  assert.match(contextWGSL,/fn mxHash2/);
});
test('randomcolor derives bounded HSV channels from input and seed', () => {
  const source=compileGraph({nodes:[{name:'r',category:'randomcolor',type:'color3',inputs:{in:{type:'float',value:2},huelow:{type:'float',value:.1},huehigh:{type:'float',value:.4},seed:{type:'integer',value:7},brightnesshigh:{type:'float',value:.8}}}]});
  assert.match(source.body,/mxHsvToRgb/); assert.match(source.body,/mxHash3\(vec3f\(2\.0,f32\(7i\),17\.0\)\)/);
});
test('unified noise dispatches typed noise families and output remapping', () => {
  const source=compileGraph({nodes:[{name:'u',category:'unifiednoise2d',type:'float',inputs:{texcoord:{type:'vector2',value:[.1,.2]},freq:{type:'vector2',value:[2,3]},type:{type:'integer',value:3},octaves:{type:'integer',value:4},outmin:{type:'float',value:-1},outmax:{type:'float',value:2}}}]});
  assert.match(source.body,/mxFractal2/); assert.match(source.body,/clamp\(/); assert.match(source.body,/select\(/);
  const source3=compileGraph({nodes:[{name:'u',category:'unifiednoise3d',type:'float',inputs:{position:{type:'vector3',value:[0,0,0]},type:{type:'integer',value:2}}}]});
  assert.match(source3.body,/mxWorley3/);
});
test('bump derivatives reevaluate shared height graphs in isolated contexts', () => {
  const nodes=[
    {name:'uv',category:'texcoord',type:'vector2'},
    {name:'h',category:'extract',type:'float',inputs:{in:{nodename:'uv'},index:{type:'integer',value:0}}},
    {name:'b',category:'bump',type:'vector3',inputs:{height:{nodename:'h'}}},
  ];
  const result=compileGraph({nodes});
  assert.equal((result.body.match(/mxOffsetContext\(/g)||[]).length,4);
  for(const m of result.body.matchAll(/let (bumpCtx\d+)=/g))assert.ok(result.body.includes(`${m[1]}.uv`));
  assert.match(result.body,/ctx.tangent,ctx.bitangent/);
  const encoded=compileGraph({nodes:[...nodes.slice(0,2),{name:'encoded',category:'heighttonormal',type:'vector3',inputs:{in:{nodename:'h'}}}]});
  assert.match(encoded.body,/mxHeightToNormal/);
  assert.doesNotMatch(encoded.body,/mxBumpGradient/);
});

test('stdlib utility aliases, blackbody and bump compile with bounded controls', () => {
  const plus=compileGraph({nodes:[{name:'p',category:'plus',type:'float',inputs:{fg:{type:'float',value:.25},bg:{type:'float',value:.5}}}]});
  assert.match(plus.body,/mix\(0\.5,\(0\.5\+0\.25\),1\.0\)/);
  const fract=compileGraph({nodes:[{name:'f',category:'fract',type:'vector3',inputs:{in:{type:'vector3',value:[-1.25,.25,2.5]}}}]});
  assert.match(fract.body,/fract\(/);
  const blackbody=compileGraph({nodes:[{name:'b',category:'blackbody',type:'color3',inputs:{temperature:{type:'float',value:3200}}}]});
  assert.match(blackbody.body,/mxBlackbody\(3200\.0\)/);
  const defaultBlackbody=compileGraph({nodes:[{name:'b',category:'blackbody',type:'color3',inputs:{}}]});
  assert.match(defaultBlackbody.body,/mxBlackbody\(5000\.0\)/);
  const bump=compileGraph({nodes:[{name:'b',category:'bump',type:'vector3',inputs:{height:{type:'float',value:.2},scale:{type:'float',value:1.5}}}]});
  assert.match(bump.body,/mxBumpGradient/);
});
test('stdlib color correction and switch preserve typed authored controls', () => {
  const color=compileGraph({nodes:[{name:'c',category:'colorcorrect',type:'color3',inputs:{in:{type:'color3',value:[.2,.4,.8]},hue:{type:'float',value:.1},gamma:{type:'float',value:2},exposure:{type:'float',value:1}}}]});
  assert.match(color.body,/mxRgbToHsv/); assert.match(color.body,/exp2\(/); assert.match(color.body,/pow\(/);
  const selected=compileGraph({nodes:[{name:'s',category:'switch',type:'float',inputs:{in1:{type:'float',value:1},in2:{type:'float',value:2},in10:{type:'float',value:10},which:{type:'float',value:1}}}]});
  assert.match(selected.body,/>=1\.0/); assert.match(selected.body,/10\.0/);
});
test('stdlib PBR conversion nodes preserve artistic IOR and anisotropic roughness', () => {
  const outputs={ior:{type:'color3'},extinction:{type:'color3'}};
  const ior=compileGraph({nodes:[{name:'a',category:'artistic_ior',type:'multioutput',outputs,inputs:{reflectivity:{type:'color3',value:[.9,.5,.2]},edge_color:{type:'color3',value:[1,.8,.4]}}}]},{output:{nodename:'a',output:'ior'}});
  assert.match(ior.body,/mix\(/); assert.match(ior.body,/sqrt\(/);
  const extinction=compileGraph({nodes:[{name:'a',category:'artistic_ior',type:'multioutput',outputs,inputs:{}}]},{output:{nodename:'a',output:'extinction'}});
  assert.match(extinction.body,/max\(/); assert.match(extinction.body,/sqrt\(/);
  const rough=compileGraph({nodes:[{name:'r',category:'roughness_anisotropy',type:'vector2',inputs:{roughness:{type:'float',value:.4},anisotropy:{type:'float',value:.5}}}]});
  assert.match(rough.body,/min\(/); assert.match(rough.body,/vec2f/);
  const gloss=compileGraph({nodes:[{name:'g',category:'glossiness_anisotropy',type:'vector2',inputs:{glossiness:{type:'float',value:.8},anisotropy:{type:'float',value:-.25}}}]});
  assert.match(gloss.body,/1\.0-0\.8/);
  const dual=compileGraph({nodes:[{name:'d',category:'roughness_dual',type:'vector2',inputs:{roughness:{type:'vector2',value:[.4,-1]}}}]});
  assert.match(dual.body,/select\(.*\.y,.*\.x,.*\.y<0\.0\)/); assert.match(dual.body,/clamp\(vec2f/);
  const gooch=compileGraph({nodes:[{name:'g',category:'gooch_shade',type:'color3',inputs:{warm_color:{type:'color3',value:[.8,.8,.7]},cool_color:{type:'color3',value:[.3,.3,.8]},light_direction:{type:'vector3',value:[1,-.5,-.5]},shininess:{type:'float',value:32}}}]});
  assert.match(gooch.body,/ctx\.normal/); assert.match(gooch.body,/ctx\.viewdir/); assert.match(gooch.body,/mix\(/); assert.match(gooch.body,/pow\(/);
  const clover=compileGraph({nodes:[{name:'c',category:'cloverleaf',type:'float',inputs:{texcoord:{type:'vector2',value:[.5,.5]},center:{type:'vector2',value:[.5,.5]},radius:{type:'float',value:.25}}}]});
  assert.match(clover.body,/distance\(/); assert.match(clover.body,/min\(/);
  const hexagon=compileGraph({nodes:[{name:'h',category:'hexagon',type:'float',inputs:{texcoord:{type:'vector2',value:[.5,.5]},center:{type:'vector2',value:[.5,.5]},radius:{type:'float',value:.25}}}]});
  assert.match(hexagon.body,/dot\(/); assert.match(hexagon.body,/sign\(/); assert.match(hexagon.body,/clamp\(/);
  const tiled=compileGraph({nodes:[{name:'tc',category:'tiledcloverleafs',type:'color3',inputs:{texcoord:{type:'vector2',value:[.25,.25]},uvtiling:{type:'vector2',value:[2,2]},size:{type:'float',value:.4},staggered:{type:'boolean',value:true}}}]});
  assert.match(tiled.body,/fract\(/); assert.match(tiled.body,/vec3f\(/);
  const tiledHex=compileGraph({nodes:[{name:'th',category:'tiledhexagons',type:'color3',inputs:{uvtiling:{type:'vector2',value:[3,2]},size:{type:'float',value:.6}}}]});
  assert.match(tiledHex.body,/dot\(/); assert.match(tiledHex.body,/sign\(/);
});
test('stdlib transform aliases and trianglewave keep space semantics explicit', () => {
  const normal=compileGraph({nodes:[{name:'n',category:'transformnormal',type:'vector3',inputs:{in:{type:'vector3',value:[0,0,1]},fromspace:{type:'string',value:'world'},tospace:{type:'string',value:'world'}}}]});
  assert.match(normal.body,/vec3f\(0\.0,0\.0,1\.0\)/);
  assert.doesNotMatch(normal.body,/safeNormal|normalize/);
  const point=compileGraph({nodes:[{name:'p',category:'transformpoint',type:'vector3',inputs:{in:{type:'vector3',value:[1,2,3]},fromspace:{type:'string',value:''},tospace:{type:'string',value:'world'}}}]});
  assert.match(point.body,/vec3f\(1\.0,2\.0,3\.0\)/);
  assert.throws(()=>compileGraph({nodes:[{name:'p',category:'transformpoint',type:'vector3',inputs:{fromspace:{type:'string',value:'object'},tospace:{type:'string',value:'world'}}}]}),/non-world/);
  const wave=compileGraph({nodes:[{name:'w',category:'trianglewave',type:'float',inputs:{in:{type:'float',value:1.25}}}]});
  assert.match(wave.body,/0\.5-abs\(fract\(abs/);
});
test('cell-noise nodes hash integer cells without interpolation', () => {
  const doc={nodes:[
    {name:'uv',category:'texcoord',type:'vector2',inputs:{}},
    {name:'c2',category:'cellnoise2d',type:'float',inputs:{in:{nodename:'uv'}}},
    {name:'p',category:'position',type:'vector3',inputs:{}},
    {name:'c3',category:'cellnoise3d',type:'float',inputs:{in:{nodename:'p'}}},
    {name:'out',category:'add',type:'float',inputs:{in1:{nodename:'c2'},in2:{nodename:'c3'}}}
  ]};
  const source=compileGraph(doc); assert.match(source.body,/mxHash2\(floor/); assert.match(source.body,/mxHash3\(floor/);
});
test('tiledimage compiles the validated single-tile resource path', () => {
  const doc={images:{tile:{width:1,height:1,data:[1,0,0,1],colorspace:'raw'}},nodes:[{name:'tile',category:'tiledimage',type:'color3',inputs:{file:{type:'filename',value:'tile'},uvtiling:{type:'vector2',value:[1,1]}}}]};
  const descriptor={tile:{offset:0,width:1,height:1,levels:1,colorspace:'raw'}};
  const source=compileGraph(doc,{imageDescriptors:descriptor}); assert.match(source.body,/imageSample\(0u/);
  const tiled=compileGraph({...doc,nodes:[{...doc.nodes[0],inputs:{...doc.nodes[0].inputs,uvtiling:{type:'vector2',value:[2,1]}}}]},{imageDescriptors:descriptor});
  assert.match(tiled.body,/ctx\.uv\*\(vec2f\(2\.0,1\.0\)\*vec2f\(1\.0\)\)/);
  assert.match(tiled.body,/ctx\.uvDx\*vec2f\(1\.0,1\.0\)\*\(vec2f\(2\.0,1\.0\)\*vec2f\(1\.0\)\)/);
  const offset=compileGraph({...doc,nodes:[{...doc.nodes[0],inputs:{...doc.nodes[0].inputs,uvoffset:{type:'vector2',value:[.25,.5]}}}]},{imageDescriptors:descriptor});
  assert.match(offset.body,/ctx\.uv\*\(vec2f\(1\.0,1\.0\)\*vec2f\(1\.0\)\)\)-vec2f\(0\.25,0\.5\)/);
  const real=compileGraph({...doc,nodes:[{...doc.nodes[0],inputs:{...doc.nodes[0].inputs,realworldimagesize:{type:'vector2',value:[2,1]},realworldtilesize:{type:'vector2',value:[1,1]}}}]},{imageDescriptors:descriptor});
  assert.match(real.body,/vec2f\(2\.0,1\.0\)/);
  assert.throws(()=>compileGraph({...doc,nodes:[{...doc.nodes[0],inputs:{...doc.nodes[0].inputs,realworldimagesize:{type:'vector2',value:[2,1]}}}]},{imageDescriptors:descriptor}),/paired static/);
});
test('path shading carries a bounded UV ray footprint for image mips', () => {
  const source = shaderSource(syntheticScene('ops').materials, {});
  assert.match(source, /uvScale/); assert.match(source, /tri\.b\.uv\.xy-tri\.a\.uv\.xy/); assert.match(source, /geometricNormal/); assert.match(source, /safeNormal/); assert.match(source, /pathCounters\[3\]/);
});
test('volume transport includes direct HG light estimation at scattering events', () => {
  const source=shaderSource(syntheticScene('sss').materials, {});
  assert.match(source,/hgPhase\(dot\(-p\.direction\.xyz,light\)/);
  assert.match(source,/p\.beta\.xyz\*tr\*hgPhase/);
  assert.match(source,/envColor\*\(4\.0\*PI\)/);
  assert.match(source,/let tr=exp\(-sigmaT\*shadow\.t\)/);
  assert.match(source,/triangles\[shadow\.id\]\.a\.uv\.z\)==p\.media\[mediumDepth\]-1u/);
});
test('cycle, missing node, mismatch, duplicate, unknown operation fail', () => {
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', category: 'absval', type: 'float', inputs: { in: { nodename: 'x' } } }] }), /cycle/);
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', category: 'absval', type: 'float', inputs: { in: { nodename: 'missing' } } }] }), /unknown node/);
  assert.throws(() => compileGraph({ nodes: [constant('x', [1, 2, 3], 'color3'), { name: 'y', category: 'absval', type: 'float', inputs: { in: { nodename: 'x' } } }] }), /expected float/);
  assert.throws(() => compileGraph({ nodes: [constant('x', 1), constant('x', 2)] }), /x/);
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', category: 'arbitrary', type: 'float' }] }), /not implemented/);
});
test('compound NodeDef inputs bind separately for each instance', () => {
  const doc = { definitions: { ND_double: { name: 'ND_double', node: 'double', outputs: { out: { type: 'float' } }, inputs: { x: { type: 'float', value: 2 } } } }, graphs: { impl: { name: 'impl', nodedef: 'ND_double', inputs: {}, outputs: { out: { nodename: 'sum' } }, nodes: [{ name: 'sum', category: 'add', type: 'float', inputs: { in1: { interfacename: 'x' }, in2: { interfacename: 'x' } } }] } }, nodes: [
    { name: 'a', category: 'double', type: 'float', nodedef: 'ND_double', inputs: { x: { type: 'float', value: 3 } } },
    { name: 'b', category: 'double', type: 'float', nodedef: 'ND_double', inputs: { x: { type: 'float', value: 4 } } },
    { name: 'out', category: 'add', type: 'float', inputs: { in1: { nodename: 'a' }, in2: { nodename: 'b' } } },
  ] };
  const compiled = compileGraph(doc); assert.match(compiled.body, /3.0 \+ 3.0/); assert.match(compiled.body, /4.0 \+ 4.0/);
});
test('graph output cycle fails', () => {
  assert.throws(() => compileGraph({ nodes: [], graphs: { g: { name: 'g', nodes: [], outputs: { out: { nodegraph: 'g', output: 'out' } } } } }, { output: { nodegraph: 'g', output: 'out' } }), /cycle/);
});
test('prototype names do not resolve as definitions or graphs', () => {
  assert.throws(() => compileGraph({ nodes: [{ name: 'x', nodedef: 'constructor', category: 'constant', type: 'float' }] }), /unknown NodeDef/);
  assert.throws(() => compileGraph({ nodes: [] }, { output: { nodegraph: '__proto__' } }), /unknown graph/);
});
test('unsupported physical features never silently become opaque defaults', () => {
  const doc = syntheticScene().materials[1]; doc.nodes[0].inputs.transmission = { type: 'float', value: 1 };
  assert.match(compileGraph(doc, { material: true }).body, /Material/);
  delete doc.nodes[0].inputs.transmission; doc.nodes[0].inputs.subsurface = { type: 'float', value: 0.5 };
  assert.match(compileGraph(doc, { material: true }).body, /materialFromClosure/);
});
test('BVH escape links progress, leaves cover every triangle exactly once', () => {
  const scene = syntheticScene('graph'), packed = packScene(scene); let leaves = 0;
  for (let i = 0; i < packed.nodeCount; i++) {
    const n = packed.nodeData.subarray(i * 12, i * 12 + 12);
    assert.ok(n[8] > i && n[8] <= packed.nodeCount); leaves += n[7];
    for (let k = 0; k < 3; k++) assert.ok(n[k] <= n[k + 4]);
  }
  assert.equal(leaves, scene.indices.length / 3);
  assert.equal(packed.triangleData.length, leaves * 48);
  const colored={...scene,colors:new Array(scene.positions.length/3*4).fill(0).map((v,i)=>i%4===0?1:i%4===3?1:0)};
  assert.deepEqual(Array.from(packScene(colored).triangleData.slice(12,16)),[1,0,0,1]);
});
test('scene resource limits and indices validated before GPU allocation', () => {
  const scene = syntheticScene(); assert.throws(() => packScene(scene, { maxTriangles: 1 }), /budget/);
  scene.indices[0] = -1; assert.throws(() => packScene(scene), /index/);
});
test('EXR float roundtrip through independent Three.js decoder', async () => {
  const { EXRLoader } = await import('three/addons/loaders/EXRLoader.js');
  const { FloatType } = await import('three');
  const pixels = new Float32Array([0.25, 2, 32, 1, 0, 0.5, 1, 1]);
  const encoded = encodeEXR(2, 1, pixels); assert.equal(new DataView(encoded.buffer).getInt32(0, true), 20000630);
  const decoded = new EXRLoader().setDataType(FloatType).parse(encoded.buffer);
  assert.equal(decoded.width, 2); assert.equal(decoded.height, 1); assert.deepEqual([...decoded.data], [...pixels]);
});

test('spectral EXR declares XYZ channels and preserves component order',()=>{
  const bytes=encodeEXR(1,1,new Float32Array([.25,2,32,1]),{channels:['X','Y','Z']}),view=new DataView(bytes.buffer);let offset=8;
  const z=()=>{const start=offset;while(bytes[offset]!==0)offset++;return new TextDecoder().decode(bytes.subarray(start,offset++));};
  let channels=[];
  while(bytes[offset]!==0){const name=z();z();const size=view.getUint32(offset,true);offset+=4;const end=offset+size;if(name==='channels'){while(bytes[offset]!==0){channels.push(z());assert.equal(view.getUint32(offset,true),2);offset+=16;}}offset=end;}
  offset++;assert.deepEqual(channels,['X','Y','Z']);const row=Number(view.getBigUint64(offset,true));assert.equal(view.getUint32(row+4,true),12);
  assert.deepEqual([0,1,2].map(i=>view.getFloat32(row+8+i*4,true)),[.25,2,32]);
});
