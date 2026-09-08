// SPDX-License-Identifier: Apache-2.0
import { mayEmit } from './emission.js';
export const sub = (a, b) => a.map((v, i) => v - b[i]);
export const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
export const normalize = a => { const l = Math.hypot(...a); return l > 1e-20 ? a.map(v => v / l) : [0, 1, 0]; };
export function surfaceDocument(color = [0.6, 0.2, 0.05], metal = 0, roughness = 0.3) {
  return { nodes: [{ name: 'surface', category: 'standard_surface', type: 'surfaceshader', inputs: {
    base_color: { type: 'color3', value: color }, metalness: { type: 'float', value: metal }, specular_roughness: { type: 'float', value: roughness },
  } }] };
}
export function syntheticScene(preset = 'copper') {
  const positions = [], normals = [], uvs = [], indices = [], materialIds = [];
  const materials = [surfaceDocument([0.5, 0.5, 0.5], 0, 0.8), surfaceDocument([0.95, 0.55, 0.22], preset === 'copper' ? 1 : 0, 0.22)];
  if (['glass','rough-glass','sss'].includes(preset)) {
    materials[1] = surfaceDocument([1,1,1], 0, preset === 'glass' ? 0 : 0.22);
    materials[1].nodes[0].inputs.transmission = { type: 'float', value: 1 };
    materials[1].nodes[0].inputs.transmission_color = { type: 'color3', value: [.92,.98,1] };
    if(preset==='sss') {
      materials[1].nodes[0].inputs.transmission_color.value=[1,1,1];
      materials[1].nodes[0].inputs.specular_ior={type:'float',value:1.3};
      materials[1].nodes.unshift({name:'interior',category:'anisotropic_vdf',type:'VDF',inputs:{absorption:{type:'color3',value:[.08,.3,.7]},scattering:{type:'color3',value:[3,3,3]},anisotropy:{type:'float',value:.2}}});
      materials[1].mediumOutput={nodename:'interior'};
    }
  }
  if (preset === 'image') {
    const data = new Float32Array(64 * 64 * 4);
    for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) data.set(((x >> 3) + (y >> 3)) % 2 ? [.9,.3,.05,1] : [.05,.3,.9,1], (y * 64 + x) * 4);
    materials[1].images = { checker: { width: 64, height: 64, data, colorspace: 'srgb_texture' } };
    materials[1].nodes[0].inputs.base_color = { nodename: 'checker' };
    materials[1].nodes.unshift({ name: 'checker', category: 'image', type: 'color3', inputs: { file: { type: 'filename', value: 'checker' } } });
  }
  if (preset === 'graph') {
    materials[1] = { nodes: [
      { name: 'uv', category: 'texcoord', type: 'vector2', inputs: {} },
      { name: 'x', category: 'extract', type: 'float', inputs: { in: { nodename: 'uv' }, index: { type: 'integer', value: 0 } } },
      { name: 'frequency', category: 'multiply', type: 'float', inputs: { in1: { nodename: 'x' }, in2: { type: 'float', value: 100 } } },
      { name: 'wave', category: 'sin', type: 'float', inputs: { in: { nodename: 'frequency' } } },
      { name: 'mask', category: 'smoothstep', type: 'float', inputs: { in: { nodename: 'wave' }, low: { type: 'float', value: -0.1 }, high: { type: 'float', value: 0.1 } } },
      { name: 'color', category: 'mix', type: 'color3', inputs: { bg: { type: 'color3', value: [0.015, 0.05, 0.2] }, fg: { type: 'color3', value: [0.9, 0.6, 0.1] }, mix: { nodename: 'mask' } } },
      { name: 'surface', category: 'standard_surface', type: 'surfaceshader', inputs: { base_color: { nodename: 'color' }, specular_roughness: { type: 'float', value: 0.28 } } },
    ] };
  }
  if (preset === 'ops') {
    materials[1] = { nodes: [
      { name: 'uv', category: 'texcoord', type: 'vector2', inputs: {} },
      { name: 'rot', category: 'rotate2d', type: 'vector2', inputs: { in: { nodename: 'uv' }, amount: { type: 'float', value: 35 } } },
      { name: 'u', category: 'extract', type: 'float', inputs: { in: { nodename: 'rot' }, index: { type: 'integer', value: 0 } } },
      { name: 'mapped', category: 'range', type: 'float', inputs: { in: { nodename: 'u' }, inlow: { type: 'float', value: -.5 }, inhigh: { type: 'float', value: .5 }, outlow: { type: 'float', value: 0 }, outhigh: { type: 'float', value: 1 }, gamma: { type: 'float', value: 1 }, doclamp: { type: 'boolean', value: true } } },
      { name: 'mask', category: 'ifgreater', type: 'float', inputs: { in1: { nodename: 'mapped' }, in2: { type: 'float', value: 0 }, value1: { nodename: 'mapped' }, value2: { type: 'float', value: .5 } } },
      { name: 'color', category: 'combine3', type: 'color3', inputs: { in1: { nodename: 'mask' }, in2: { type: 'float', value: .2 }, in3: { type: 'float', value: .8 } } },
      { name: 'surface', category: 'standard_surface', type: 'surfaceshader', inputs: { base_color: { nodename: 'color' }, specular_roughness: { type: 'float', value: .3 } } },
    ] };
  }
  if (preset === 'ops-advanced') {
    materials[1] = { nodes: [
      { name: 'uv', category: 'texcoord', type: 'vector2', inputs: {} },
      { name: 'uv3', category: 'convert', type: 'vector3', inputs: { in: { nodename: 'uv' } } },
      { name: 'swz', category: 'swizzle', type: 'vector2', inputs: { in: { nodename: 'uv3' }, channels: { type: 'string', value: 'yx' } } },
      { name: 'parts', category: 'separate2', type: 'float', inputs: { in: { nodename: 'swz' } } },
      { name: 'power', category: 'power', type: 'float', inputs: { in1: { nodename: 'parts', output: 'outx' }, in2: { type: 'float', value: 2 } } },
      { name: 'cross', category: 'crossproduct', type: 'vector3', inputs: { in1: { nodename: 'uv3' }, in2: { type: 'vector3', value: [0, 0, 1] } } },
      { name: 'dot', category: 'dotproduct', type: 'float', inputs: { in1: { nodename: 'cross' }, in2: { type: 'vector3', value: [1, 0, 0] } } },
      { name: 'chosen', category: 'ifequal', type: 'float', inputs: { in1: { nodename: 'power' }, in2: { nodename: 'dot' }, value1: { nodename: 'parts', output: 'outx' }, value2: { type: 'float', value: 0 } } },
      { name: 'color', category: 'combine3', type: 'color3', inputs: { in1: { nodename: 'chosen' }, in2: { nodename: 'power' }, in3: { nodename: 'dot' } } },
      { name: 'surface', category: 'standard_surface', type: 'surfaceshader', inputs: { base_color: { nodename: 'color' }, specular_roughness: { type: 'float', value: .36 } } },
    ] };
  }
  if (preset === 'layered') {
    materials[1] = { nodes: [
      { name: 'diffuse', category: 'oren_nayar_diffuse_bsdf', type: 'BSDF', inputs: { color: { type: 'color3', value: [.7, .18, .08] }, roughness: { type: 'float', value: .45 }, weight: { type: 'float', value: 1 } } },
      { name: 'medium', category: 'anisotropic_vdf', type: 'VDF', inputs: { absorption: { type: 'color3', value: [.08, .18, .35] }, scattering: { type: 'color3', value: [2, 1.2, .8] }, anisotropy: { type: 'float', value: .25 } } },
      { name: 'layer', category: 'layer', type: 'BSDF', inputs: { top: { nodename: 'diffuse' }, base: { nodename: 'medium' } } },
      { name: 'surface', category: 'surface', type: 'surfaceshader', inputs: { bsdf: { nodename: 'layer' } } },
    ] };
  }
  if (preset === 'edf') {
    materials[1] = { nodes: [
      { name: 'emit', category: 'uniform_edf', type: 'EDF', inputs: { color: { type: 'color3', value: [2.5, .8, .15] } } },
      { name: 'surface', category: 'surface', type: 'surfaceshader', inputs: { edf: { nodename: 'emit' } } },
    ] };
  }
  if (preset === 'native-film') {
    materials[1] = { nodes: [
      { name: 'film', category: 'dielectric_bsdf', type: 'BSDF', inputs: { scatter_mode: { type: 'string', value: 'RT' }, roughness: { type: 'vector2', value: [.08, .12] }, ior: { type: 'float', value: 1.5 }, thinfilm_thickness: { type: 'float', value: 180 }, thinfilm_IOR: { type: 'float', value: 1.4 } } },
      { name: 'surface', category: 'surface', type: 'surfaceshader', inputs: { bsdf: { nodename: 'film' } } },
    ] };
  }
  if(preset==='displacement') {
    materials[1].nodes.unshift(
      {name:'uv',category:'texcoord',type:'vector2',inputs:{}},
      {name:'u',category:'extract',type:'float',inputs:{in:{nodename:'uv'},index:{type:'integer',value:0}}},
      {name:'freq',category:'multiply',type:'float',inputs:{in1:{nodename:'u'},in2:{type:'float',value:50.2654824574}}},
      {name:'wave',category:'sin',type:'float',inputs:{in:{nodename:'freq'}}},
      {name:'height',category:'multiply',type:'float',inputs:{in1:{nodename:'wave'},in2:{type:'float',value:.08}}}
    );materials[1].displacementOutput={nodename:'height'};
  }
  if(preset==='hair') {
    materials[1]={nodes:[
      {name:'fiber',category:'hair_bsdf',type:'BSDF',inputs:{melanin:{type:'float',value:.35},melanin_redness:{type:'float',value:.2},longitudinal_roughness:{type:'float',value:.25},azimuthal_roughness:{type:'float',value:.2}}},
      {name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'fiber'}}}
    ]};
  }
  if(preset==='thin-film') {
    materials[1]=surfaceDocument([.72,.18,.06],0,.18);
    materials[1].nodes[0].inputs.thin_film_thickness={type:'float',value:180};
    materials[1].nodes[0].inputs.thin_film_IOR={type:'float',value:1.4};
  }
  if(preset==='subsurface') {
    materials[1]=surfaceDocument([.82,.22,.12],0,.28);
    materials[1].nodes[0].inputs.subsurface={type:'float',value:.55};
    materials[1].nodes[0].inputs.subsurface_color={type:'color3',value:[1,.25,.12]};
    materials[1].nodes[0].inputs.subsurface_radius={type:'color3',value:[1,.35,.15]};
  }
  if(preset==='normalmap') {
    materials[1]=surfaceDocument([.45,.35,.18],0,.24);
    materials[1].nodes.unshift(
      {name:'normal',category:'normalmap',type:'vector3',inputs:{in:{type:'vector3',value:[.72,.42,.98]},scale:{type:'vector2',value:[1.2,.8]}}}
    );
    materials[1].nodes.at(-1).inputs.normal={nodename:'normal'};
  }
  if(preset==='normalmap-image') {
    materials[1]=surfaceDocument([.45,.35,.18],0,.24);
    const normalData = new Float32Array([
      .5,.5,1,1,  .72,.42,.98,1,
      .35,.65,.98,1, .5,.5,1,1
    ]);
    materials[1].images={normalTex:{width:2,height:2,data:normalData,colorspace:'raw'}};
    materials[1].nodes.unshift(
      {name:'normalTex',category:'image',type:'color3',colorspace:'raw',inputs:{file:{type:'filename',value:'normalTex'},filtertype:{type:'string',value:'linear'}}},
      {name:'normal',category:'normalmap',type:'vector3',inputs:{in:{nodename:'normalTex'},scale:{type:'vector2',value:[1.2,.8]}}}
    );
    materials[1].nodes.at(-1).inputs.normal={nodename:'normal'};
  }
  if(preset==='bump') {
    materials[1]=surfaceDocument([.36,.12,.04],0,.3);
    materials[1].nodes.unshift(
      {name:'bumpUV',category:'texcoord',type:'vector2'},
      {name:'bumpU',category:'extract',type:'float',inputs:{in:{nodename:'bumpUV'},index:{type:'integer',value:0}}},
      {name:'bumpFrequency',category:'multiply',type:'float',inputs:{in1:{nodename:'bumpU'},in2:{type:'float',value:40}}},
      {name:'bumpHeight',category:'sin',type:'float',inputs:{in:{nodename:'bumpFrequency'}}},
      {name:'bump',category:'bump',type:'vector3',inputs:{height:{nodename:'bumpHeight'},scale:{type:'float',value:.025}}});
    materials[1].nodes.at(-1).inputs.normal={nodename:'bump'};
  }
  if(preset==='coat') {
    materials[1]=surfaceDocument([.28,.08,.025],0,.32);
    materials[1].nodes[0].inputs.coat={type:'float',value:.65};
    materials[1].nodes[0].inputs.coat_color={type:'color3',value:[1,.92,.78]};
    materials[1].nodes[0].inputs.coat_roughness={type:'float',value:.08};
    materials[1].nodes[0].inputs.coat_IOR={type:'float',value:1.5};
  }
  if(preset==='sheen') {
    materials[1]=surfaceDocument([.12,.25,.42],0,.3);
    materials[1].nodes[0].inputs.sheen={type:'float',value:.55};
    materials[1].nodes[0].inputs.sheen_color={type:'color3',value:[.8,.9,1]};
    materials[1].nodes[0].inputs.sheen_roughness={type:'float',value:.38};
  }
  if(preset==='thin-walled') {
    materials[1]=surfaceDocument([.92,.98,1],0,.08);
    materials[1].nodes[0].inputs.transmission={type:'float',value:1};
    materials[1].nodes[0].inputs.transmission_color={type:'color3',value:[.9,.96,1]};
    materials[1].nodes[0].inputs.thin_walled={type:'boolean',value:true};
  }
  if(preset==='transmission-depth') {
    materials[1]=surfaceDocument([.95,.98,1],0,.12);
    materials[1].nodes[0].inputs.transmission={type:'float',value:1};
    materials[1].nodes[0].inputs.transmission_color={type:'color3',value:[1,.92,.8]};
    materials[1].nodes[0].inputs.transmission_depth={type:'float',value:2.5};
    materials[1].nodes[0].inputs.transmission_scatter={type:'color3',value:[.08,.2,.5]};
  }
  if(preset==='generalized-schlick') {
    materials[1]={nodes:[
      {name:'fresnel',category:'generalized_schlick_bsdf',type:'BSDF',inputs:{color0:{type:'color3',value:[.04,.08,.16]},color90:{type:'color3',value:[.8,.95,1]},roughness:{type:'vector2',value:[.12,.2]},weight:{type:'float',value:1},exponent:{type:'float',value:4}}},
      {name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'fresnel'}}}
    ]};
  }
  if(preset==='open-pbr-weight') {
    materials[1]={nodes:[
      {name:'surface',category:'open_pbr_surface',type:'surfaceshader',inputs:{
        base_weight:{type:'float',value:.35},
        base_color:{type:'color3',value:[.8,.2,.08]},
        base_metalness:{type:'float',value:0},
        specular_weight:{type:'float',value:.42},
        specular_color:{type:'color3',value:[.7,.85,1]},
        base_diffuse_roughness:{type:'float',value:.72},
        specular_ior:{type:'float',value:1.5},
        geometry_opacity:{type:'float',value:1}
      }}
    ]};
  }
  if(preset==='open-pbr-film') {
    materials[1]={nodes:[
      {name:'surface',category:'open_pbr_surface',type:'surfaceshader',inputs:{
        base_weight:{type:'float',value:1}, base_color:{type:'color3',value:[.3,.12,.04]},
        base_metalness:{type:'float',value:0}, base_diffuse_roughness:{type:'float',value:.3},
        thin_film_weight:{type:'float',value:.8}, thin_film_thickness:{type:'float',value:220},
        thin_film_ior:{type:'float',value:1.4}, geometry_opacity:{type:'float',value:1}
      }}
    ]};
  }
  if(preset==='open-pbr-normal') {
    materials[1]={nodes:[
      {name:'surface',category:'open_pbr_surface',type:'surfaceshader',inputs:{
        base_color:{type:'color3',value:[.42,.18,.05]}, base_diffuse_roughness:{type:'float',value:.28},
        geometry_normal:{type:'vector3',value:[.72,.42,.98]}, geometry_opacity:{type:'float',value:1}
      }}
    ]};
  }
  if(preset==='opacity') {
    materials[1]=surfaceDocument([.72,.18,.04],0,.26);
    materials[1].nodes[0].inputs.opacity={type:'float',value:.55};
  }
  if(preset==='native-copper'||preset==='native-glass') {
    const glass=preset==='native-glass';
    materials[1]={nodes:[
      {name:'closure',category:glass?'dielectric_bsdf':'conductor_bsdf',type:'BSDF',inputs:glass?{scatter_mode:{type:'string',value:'RT'},roughness:{type:'vector2',value:[.04,.15]}}:{}},
      {name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'closure'}}}
    ]};
  }
  function sphere(cx, cy, cz, r, mat, segments = 64, rings = 32) {
    const first = positions.length / 3;
    for (let y = 0; y <= rings; y++) for (let x = 0; x <= segments; x++) {
      const theta = y / rings * Math.PI, phi = x / segments * Math.PI * 2;
      const n = [Math.sin(theta) * Math.cos(phi), Math.cos(theta), Math.sin(theta) * Math.sin(phi)];
      positions.push(cx + n[0] * r, cy + n[1] * r, cz + n[2] * r); normals.push(...n); uvs.push(x / segments, y / rings);
    }
    for (let y = 0; y < rings; y++) for (let x = 0; x < segments; x++) {
      const a = first + y * (segments + 1) + x, b = a + segments + 1;
      if (y > 0) { indices.push(a, a + 1, b); materialIds.push(mat); }
      if (y < rings - 1) { indices.push(a + 1, b + 1, b); materialIds.push(mat); }
    }
  }
  sphere(0, 1, 0, 1, 1);
  const first = positions.length / 3;
  positions.push(-8, 0, -8, -8, 0, 8, 8, 0, 8, 8, 0, -8);
  for (let i = 0; i < 4; i++) normals.push(0, 1, 0);
  uvs.push(0, 0, 0, 1, 1, 1, 1, 0);
  indices.push(first, first + 1, first + 2, first, first + 2, first + 3); materialIds.push(0, 0);
  return { positions, normals, uvs, indices, materialIds, materials, ...(preset === 'displacement' ? { displacementRefinement: 2 } : {}), camera: { origin: [3.6, 2.6, 4.4], target: [0, 0.8, 0], fov: 42 }, provenance: { synthetic: preset } };
}

/** Stackless median BVH. Immutable copied scene data, no references into WASM. */
export function packScene(scene, { maxTriangles = 2_000_000 } = {}) {
  const { positions, indices, normals, uvs, uvSets, colors, materialIds, materials } = scene;
  const hasColors = !!colors?.length;
  if (!positions || !indices || positions.length % 3 || indices.length % 3 || indices.length === 0) throw new Error('Invalid triangle mesh');
  if (indices.length / 3 > maxTriangles) throw new Error(`Triangle budget exceeded (${maxTriangles})`);
  if (!materials?.length || materials.length > 64) throw new Error('Expected 1–64 materials');
  if (normals && normals.length !== positions.length) throw new Error('Normal count mismatch');
  if (uvs && uvs.length !== positions.length / 3 * 2) throw new Error('UV count mismatch');
  if (uvSets) for (const set of uvSets) if (set && set.length !== positions.length / 3 * 2) throw new Error('UV set count mismatch');
  if (hasColors && colors.length !== positions.length / 3 * 3 && colors.length !== positions.length / 3 * 4) throw new Error('Color count mismatch');
  if (materialIds && materialIds.length !== indices.length / 3) throw new Error('Material count mismatch');
  for (const a of [positions, normals, uvs, hasColors ? colors : null]) if (a && !Array.from(a).every(v => Number.isFinite(v) && Number.isFinite(Math.fround(v)))) throw new Error('Non-finite float32 vertex attributes');
  const tris = [];
  for (let t = 0; t < indices.length / 3; t++) {
    const ids = Array.from(indices.slice(t * 3, t * 3 + 3));
    if (ids.some(i => !Number.isInteger(i) || i < 0 || i >= positions.length / 3)) throw new Error('Invalid vertex index');
    const mat = materialIds?.[t] ?? 0;
    if (!Number.isInteger(mat) || mat < 0 || mat >= materials.length) throw new Error('Invalid material index');
    const p = ids.map(i => Array.from(positions.slice(i * 3, i * 3 + 3)));
    const geometric = normalize(cross(sub(p[1], p[0]), sub(p[2], p[0])));
    const uvSlot = Number.isInteger(materials[mat]?.uvIndex) && materials[mat].uvIndex >= 0 ? materials[mat].uvIndex : 0;
    const selectedUVs = uvSets?.[uvSlot] || uvs;
    tris.push({ p, n: ids.map(i => normals ? Array.from(normals.slice(i * 3, i * 3 + 3)) : geometric), uv: ids.map(i => selectedUVs ? Array.from(selectedUVs.slice(i * 2, i * 2 + 2)) : [0, 0]), color: ids.map(i => hasColors ? (colors.length === positions.length / 3 * 4 ? Array.from(colors.slice(i * 4, i * 4 + 4)) : [...colors.slice(i * 3, i * 3 + 3), 1]) : [0, 0, 0, 1]), mat, center: [0, 1, 2].map(k => (p[0][k] + p[1][k] + p[2][k]) / 3) });
  }
  const nodes = [], ordered = [];
  function build(items) {
    const idx = nodes.length, lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (const t of items) for (const p of t.p) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
    const node = { lo, hi, first: 0, count: 0, escape: 0 }; nodes.push(node);
    if (items.length <= 4) { node.first = ordered.length; node.count = items.length; ordered.push(...items); }
    else {
      let axis = 0; for (let k = 1; k < 3; k++) if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;
      items.sort((a, b) => a.center[axis] - b.center[axis]); const mid = items.length >> 1;
      build(items.slice(0, mid)); build(items.slice(mid));
    }
    node.escape = nodes.length; return idx;
  }
  build(tris);
  const nodeData = new Float32Array(nodes.length * 12);
  nodes.forEach((n, i) => nodeData.set([...n.lo, n.first, ...n.hi, n.count, n.escape, 0, 0, 0], i * 12));
  const triangleData = new Float32Array(ordered.length * 48);
  let areaCDF=0;
  const emitters=materials.map(mayEmit);
  ordered.forEach((t, i) => {
    const area=.5*Math.hypot(...cross(sub(t.p[1],t.p[0]),sub(t.p[2],t.p[0]))),start=areaCDF;areaCDF=Math.fround(areaCDF+(emitters[t.mat]?area:0));
    if(!Number.isFinite(areaCDF))throw new Error('Triangle area CDF exceeds float32');
    for (let v = 0; v < 3; v++) triangleData.set([...t.p[v], [start,areaCDF,area][v], ...t.n[v], 0, ...t.uv[v], t.mat, 0, ...t.color[v]], i * 48 + v * 16);
  });
  return { nodeData, triangleData, triangleCount: ordered.length, nodeCount: nodes.length, bounds: nodes[0], camera: scene.camera, lighting: scene.lighting, materials, provenance: scene.provenance || {} };
}
