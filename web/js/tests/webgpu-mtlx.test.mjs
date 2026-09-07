// SPDX-License-Identifier: Apache-2.0
import test from 'node:test';
import assert from 'node:assert/strict';
import { compileGraph, literal, GraphError } from '../src/webgpu-mtlx/graph.js';
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
const constant = (name, value, type = 'float') => ({ name, category: 'constant', type, inputs: { value: { type, value } } });

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
  const packed=packScene(scene);assert.equal(packed.triangleData[(packed.triangleCount-1)*36+15],0);
  scene.materials[1].nodes[0].inputs.emission={type:'float',value:1};assert.equal(mayEmit(scene.materials[1]),true);
  assert.ok(packScene(scene).triangleData[(packed.triangleCount-1)*36+15]>0);
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

test('EXR authored color-space metadata is read from the header, never filename', () => {
  const bytes = encodeEXR(1, 1, new Float32Array([1, 2, 3, 1]), { colorspace: 'lin_ap1_scene' });
  assert.equal(inspectEXRHeader(bytes).colorSpace, 'lin_ap1_scene');
  assert.deepEqual(inspectEXR(bytes), { width: 1, height: 1 });
  assert.throws(() => encodeEXR(1, 1, new Float32Array([1,2,3,1]), { colorspace: 'bad\nspace' }), /metadata/);
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
  assert.throws(()=>compileGraph(doc,{material:true}),/thin film/);
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
  assert.match(compileGraph(subsurface,{material:true}).body,/subsurface_radius|clamp\(max/);
  assert.match(shaderSource([subsurface]),/closureMix/);
  const normal=syntheticScene('normalmap').materials[1];
  assert.match(compileGraph(normal,{material:true}).body,/normalize\(n/);
  assert.match(shaderSource([normal]),/surface\.normal/);
  const colorNormal={nodes:[{name:'normal',category:'normalmap',type:'vector3',inputs:{in:{type:'color3',value:[.65,.45,.95]},scale:{type:'float',value:1}}}]};
  assert.match(compileGraph(colorNormal).body,/mxNormalmap\(vec3f/);
  const openNormal=syntheticScene('open-pbr-normal').materials[1];
  assert.match(compileGraph(openNormal,{material:true}).body,/geometry_normal|normalize\(vec3f/);
  const opacity=syntheticScene('opacity').materials[1];
  assert.match(shaderSource([opacity]),/surface\.opacity/);
  const bump=syntheticScene('bump').materials[1];
  assert.match(compileGraph(bump,{material:true}).body,/mxBumpHeight/);
  assert.match(shaderSource([bump]),/mxBumpHeight/);
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
test('displacement refinement preserves bounds, typed indices and material assignment',()=>{
  const scene={positions:new Float32Array([0,0,0,1,0,0,0,1,0]),indices:new Uint32Array([0,1,2]),materials:[{}]};
  const r=refineDisplacementScene(scene,2);assert.equal(r.indices.length,48);assert.equal(r.materialIds.length,16);assert.ok(r.positions.every(v=>v>=0&&v<=1));
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
  image.inputs.filtertype = { value: 'cubic' }; assert.throws(() => shaderSource([doc]), /closest\/linear/);
  image.inputs.file.value = ''; assert.match(shaderSource([doc]), /vec3f\(0.0,0.0,0.0\)/);
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
  assert.equal(packed.triangleData.length, leaves * 36);
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
