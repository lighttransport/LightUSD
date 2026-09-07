// SPDX-License-Identifier: Apache-2.0
// Typed, deterministic MaterialX value-graph compiler. No shader-source eval.
import { closureTypesWGSL, MAX_CLOSURE_LOBES } from './closures.js';
import { colorToLinearRec709 } from './color.js';
export const MATERIALX_VERSION = '1.39.5';
export class GraphError extends Error {
  constructor(code, path, message) { super(`${path}: ${message}`); this.name = 'GraphError'; this.code = code; this.path = path; }
}
const types = { float: 'f32', integer: 'i32', boolean: 'bool', color3: 'vec3f', color4: 'vec4f', vector2: 'vec2f', vector3: 'vec3f', vector4: 'vec4f', matrix33: 'mat3x3f', matrix44: 'mat4x4f', VDF: 'Medium', BSDF: 'Closure', EDF: 'vec3f' };
const widths = { float: 1, integer: 1, boolean: 1, color3: 3, vector3: 3, color4: 4, vector4: 4, vector2: 2, matrix33: 9, matrix44: 16 };
export const valueCategories = new Set(['constant', 'add', 'subtract', 'multiply', 'divide', 'modulo', 'power', 'min', 'max', 'absval', 'sign', 'floor', 'ceil', 'round', 'sqrt', 'ln', 'exp', 'sin', 'cos', 'tan', 'asin', 'acos', 'atan2', 'clamp', 'mix', 'smoothstep', 'invert', 'normalize', 'magnitude', 'dotproduct', 'crossproduct', 'texcoord', 'position', 'normal', 'tangent', 'bitangent', 'time', 'frame', 'convert', 'combine2', 'combine3', 'combine4', 'extract', 'swizzle', 'ifequal', 'ifgreater', 'ifgreatereq', 'remap', 'range', 'rotate2d', 'dot', 'separate2', 'separate3', 'separate4']);
const materialCategories = new Set(['standard_surface', 'open_pbr_surface', 'surfacematerial', 'surface']);
for(const category of ['transformmatrix','normalmap','bump3','heighttonormal'])valueCategories.add(category);
function fail(code, path, message) { throw new GraphError(code, path, message); }
export function literal(type, value, path = '') {
  if(value===''&&type==='BSDF')return 'emptyClosure()';
  if(value===''&&type==='EDF')return 'vec3f(0)';
  if(value===''&&type==='VDF')return 'Medium(vec3f(0),vec3f(0),0)';
  const t = types[type];
  if (!t) fail('TYPE', path, `unsupported value type ${type}`);
  if (type === 'boolean') {
    if (![true, false, 'true', 'false'].includes(value)) fail('VALUE', path, 'expected boolean');
    return value === true || value === 'true' ? 'true' : 'false';
  }
  if (typeof value === 'string' && value.split(',').some(v => !v.trim())) fail('VALUE', path, 'empty numeric component');
  const values = Array.isArray(value) ? value : typeof value === 'string' ? value.split(',').map(v => Number(v.trim())) : [value];
  if (values.length !== widths[type] || values.some(v => typeof v !== 'number' || !Number.isFinite(v))) fail('VALUE', path, `expected ${widths[type]} finite ${type} components`);
  if (type === 'integer') {
    if (!Number.isInteger(values[0]) || values[0] < -2147483648 || values[0] > 2147483647) fail('VALUE', path, 'integer out of range');
    return `${values[0]}i`;
  }
  const f = v => Number.isInteger(v) ? `${v}.0` : String(v);
  return widths[type] === 1 ? f(values[0]) : `${t}(${values.map(f).join(',')})`;
}
const attrs = el => Object.fromEntries([...el.attributes].map(a => [a.name, a.value]));
const ports = el => Object.fromEntries([...el.children].filter(c => c.tagName === 'input').map(c => [c.getAttribute('name'), attrs(c)]));
const outputs = el => Object.fromEntries([...el.children].filter(c => c.tagName === 'output').map(c => [c.getAttribute('name'), attrs(c)]));

/** Browser XML entry point; DTDs/entities are deliberately disallowed. */
export function parseMaterialX(xml, { source = '', parser = globalThis.DOMParser, allowIncludes = false } = {}) {
  if (typeof xml !== 'string' || xml.length > 16 * 1024 * 1024) fail('LIMIT', source, 'XML exceeds 16 MiB');
  if (/<!DOCTYPE|<!ENTITY/i.test(xml)) fail('XML', source, 'DTD/entity declarations are not allowed');
  if (!parser) fail('XML', source, 'DOMParser is required for XML import');
  const doc = new parser().parseFromString(xml, 'application/xml');
  if (doc.querySelector('parsererror') || doc.documentElement.tagName !== 'materialx') fail('XML', source, 'invalid MaterialX XML');
  const includes = [...doc.getElementsByTagName('*')].filter(el=>el.localName==='include');
  if (includes.length && !allowIncludes) fail('INCLUDE', source, 'resolve MaterialX includes before compilation');
  if (includes.some(el=>el.parentElement!==doc.documentElement || !el.getAttribute('href') || el.hasAttribute('xpointer') || (el.getAttribute('parse') && el.getAttribute('parse')!=='xml'))) fail('INCLUDE',source,'only top-level whole-document XML includes are supported');
  const result = { version: doc.documentElement.getAttribute('version'), colorspace: doc.documentElement.getAttribute('colorspace') || undefined, includes:includes.map(el=>el.getAttribute('href')), nodes: [], graphs: Object.create(null), definitions: Object.create(null), source };
  function node(el) {
    let colorspace, fileprefix = '';
    const ancestors = []; for (let p = el; p?.nodeType === 1; p = p.parentElement) ancestors.unshift(p);
    for (const p of ancestors) { if (p.hasAttribute('colorspace')) colorspace = p.getAttribute('colorspace'); fileprefix += p.getAttribute('fileprefix') || ''; }
    return { ...attrs(el), colorspace, fileprefix, source, category: el.tagName, inputs: ports(el), outputs: outputs(el) };
  }
  for (const el of doc.documentElement.children) {
    if (el.localName === 'include') continue;
    if (el.tagName === 'nodedef') result.definitions[el.getAttribute('name')] = { ...attrs(el), inputs: ports(el), outputs: outputs(el) };
    else if (el.tagName === 'nodegraph') result.graphs[el.getAttribute('name')] = { ...attrs(el), inputs: ports(el), outputs: outputs(el), nodes: [...el.children].filter(c => !['input', 'output', 'token'].includes(c.tagName)).map(node) };
    else if (!['typedef', 'geompropdef', 'unittypedef', 'unitdef', 'implementation', 'look', 'collection', 'propertyset'].includes(el.tagName)) result.nodes.push(node(el));
  }
  const materials=result.nodes.filter(n=>n.category==='surfacematerial');
  if(materials.length===1)result.output={nodename:materials[0].name};
  return result;
}

/** Compile a normalized graph. Connections are {nodename, output} or {nodegraph, output}. */
export function compileGraph(document, { output, library = {}, material = false, imageDescriptors = {} } = {}) {
  const rawDefinitions = Object.assign(Object.create(null), library.definitions, document.definitions), definitions = Object.create(null);
  function inherit(name, chain = new Set()) {
    if (definitions[name]) return definitions[name];
    if (chain.has(name)) fail('CYCLE', name, 'NodeDef inheritance cycle');
    if (chain.size > 128) fail('LIMIT', name, 'NodeDef inheritance exceeds 128 levels');
    const d = rawDefinitions[name]; if (!d) fail('NODEDEF', name, 'missing inherited NodeDef');
    chain.add(name); const parent = d.inherit ? inherit(d.inherit, chain) : {};
    return definitions[name] = { ...parent, ...d, inputs: { ...parent.inputs, ...d.inputs }, outputs: { ...parent.outputs, ...d.outputs } };
  }
  Object.keys(rawDefinitions).forEach(name => inherit(name));
  const graphs = Object.assign(Object.create(null), library.graphs, document.graphs);
  if (!Array.isArray(document.nodes) || document.nodes.length > 4096) fail('LIMIT', '', 'expected at most 4096 nodes');
  const root = { nodes: document.nodes, inputs: {}, name: '$root' };
  const lines = [], active = new Set(), cached = new Map(), used = new Set();
  let serial = 0, portDepth = 0;
  const scopes = new WeakMap();
  function map(scope) {
    if (!scopes.has(scope)) {
      const m = new Map();
      for (const n of scope.nodes) { if (m.has(n.name)) fail('DUPLICATE', scope.name, n.name); m.set(n.name, n); }
      scopes.set(scope, m);
    }
    return scopes.get(scope);
  }
  function definition(n) {
    if (n.nodedef) {
      if (!definitions[n.nodedef]) fail('NODEDEF', n.name, `unknown NodeDef ${n.nodedef}`);
      return definitions[n.nodedef];
    }
    const matches = Object.values(definitions).filter(d => d.node === n.category && (!n.type || d.type === n.type || n.type==='multioutput'&&Object.keys(d.outputs||{}).length>1 || Object.values(d.outputs || {}).some(o => o.type === n.type)) && (!n.version || d.version === n.version) && Object.entries(n.inputs||{}).every(([name,p])=>!p.type||!d.inputs?.[name]?.type||p.type===d.inputs[name].type));
    if (matches.length > 1) {
      const defaults = matches.filter(d => d.isdefaultversion === 'true');
      if (defaults.length === 1) return defaults[0];
      fail('NODEDEF', n.name, 'ambiguous NodeDef; specify nodedef/version');
    }
    return matches[0];
  }
  function port(p, scope, env, wanted, path) {
    if (++portDepth > 256) fail('LIMIT', path, 'port resolution exceeds 256 levels');
    try {
    if (p == null) fail('INPUT', path, 'missing input');
    if (p.unit) fail('SEMANTICS', path, 'unit conversion is not implemented yet');
    let result;
    if (p.nodename) result = evaluate(p.nodename, p.output || 'out', scope, env);
    else if (p.nodegraph) {
      const g = graphs[p.nodegraph];
      if (!g) fail('GRAPH', path, `unknown graph ${p.nodegraph}`);
      const key = `graph:${p.nodegraph}:${p.output || 'out'}`;
      if (active.has(key)) fail('CYCLE', path, 'graph output cycle');
      active.add(key);
      result = port(g.outputs[p.output || 'out'], g, {}, wanted, path);
      active.delete(key);
    } else if (p.interfacename) {
      const binding = env[p.interfacename];
      if (binding) result = port(binding.port, binding.scope, binding.env, wanted, path);
      else result = port(scope.inputs[p.interfacename], scope, {}, wanted, path);
    } else if(p.defaultgeomprop) {
      const geometry={Nworld:['vector3','ctx.normal'],Tworld:['vector3','ctx.tangent'],Bworld:['vector3','ctx.bitangent'],Pworld:['vector3','ctx.position'],UV0:['vector2','ctx.uv']};
      const value=geometry[p.defaultgeomprop];if(!value)fail('GEOMETRY',path,`unsupported default geometry ${p.defaultgeomprop}`);
      result={type:value[0],code:value[1]};
    } else {
      const type=p.type||wanted;let value=p.value;
      if(p.colorspace&&['color3','color4'].includes(type)) {
        literal(type,value,path);
        try{value=colorToLinearRec709(Array.isArray(value)?value:String(value).split(',').map(Number),p.colorspace);}catch(e){fail('SEMANTICS',path,e.message);}
      } else if(p.colorspace&&!['raw','lin_rec709'].includes(p.colorspace))fail('SEMANTICS',path,'colorspace on non-color input');
      result={type,code:literal(type,value,path)};
    }
    if (wanted && result.type !== wanted) fail('TYPE', path, `expected ${wanted}, got ${result.type}`);
    return result;
    } finally { portDepth--; }
  }
  function evaluate(name, out, scope, env) {
    const n = map(scope).get(name);
    if (!n) fail('NODE', name, 'unknown node');
    const key = `${scope.name}/${name}.${out}`;
    if (cached.has(key)) return cached.get(key);
    if (active.has(key)) fail('CYCLE', key, 'connection cycle');
    if (active.size > 128) fail('LIMIT', key, 'graph depth exceeds 128');
    active.add(key);
    const def = definition(n);
    const declared = def?.outputs?.[out];
    if (out !== 'out' && !declared && !n.outputs?.[out] && !/^separate[234]$/.test(n.category)) fail('OUTPUT', key, 'unknown named output');
    const type = declared?.type || n.outputs?.[out]?.type || (/^separate[234]$/.test(n.category)&&out!=='out'?'float':n.type) || def?.type;
    const authoredInputs=Object.fromEntries(Object.entries(n.inputs||{}).map(([key,p])=>[key,p.value!==undefined&&['color3','color4'].includes(p.type)&&!p.colorspace?{...p,colorspace:n.colorspace||(n.source&&n.source!==document.source?undefined:document.colorspace)}:p]));
    const ins = { ...def?.inputs, ...authoredInputs };
    const compound = def && Object.values(graphs).find(g => g.nodedef === def.name);
    let result;
    if (compound) {
      const child = { ...compound, name: key };
      const bindings = Object.fromEntries(Object.entries(ins).map(([k, p]) => [k, { port: p, scope, env }]));
      result = port(compound.outputs[out], child, bindings, type, key);
    } else {
      const dependencies=new Set([n.category]);
      const input = (k, fallback, expected) => {
        const p = ins[k] ?? (fallback !== undefined ? { type: expected || type, value: fallback } : null);
        const value=port(p, scope, env, expected, `${key}/${k}`);for(const category of value.categories||[])dependencies.add(category);return value;
      };
      const x = (k, fallback, expected) => input(k, fallback, expected).code;
      const same = k => x(k, undefined, type);
      const scalarOrSame = (k, fallback) => {
        const p=input(k,fallback);
        if(p.type===type)return p.code;
        if(p.type==='float'&&widths[type]>=2&&widths[type]<=4)return `${types[type]}(${p.code})`;
        fail('TYPE',key,`expected ${type} or scalar float for ${k}`);
      };
      const binary = op => `(${same('in1')} ${op} ${scalarOrSame('in2')})`;
      let code, closureCount = 0, hasInterior = false, interiorCategories=[];
      switch (n.category) {
        case 'uniform_edf': code = x('color',[1,1,1],'color3'); break;
        case 'generalized_schlick_edf': code = x('base',[0,0,0],'EDF'); break;
        case 'surface': {
          const opacity = ins.opacity ? x('opacity', 1, 'float') : '1.0';
          const thin = ins.thin_walled ? `select(0u,1u,${x('thin_walled', false, 'boolean')})` : '0u';
          const bsdfValue=ins.bsdf?.value===''||!ins.bsdf ? null : input('bsdf',undefined,'BSDF');
          const bsdf=bsdfValue?.code||'emptyClosure()';hasInterior=bsdfValue?.hasInterior||false;interiorCategories=bsdfValue?.interiorCategories||[];
          const edf=ins.edf?.value===''||!ins.edf ? 'vec3f(0)' : x('edf',undefined,'EDF');
          code=`surfaceEmission(${bsdf},${edf},clamp(${opacity},0.0,1.0),${thin},ctx.normal)`;break;
        }
        case 'dielectric_bsdf': case 'conductor_bsdf': case 'oren_nayar_diffuse_bsdf': {
          if(ins.retroreflective && ![false,'false'].includes(ins.retroreflective.value))fail('UNSUPPORTED',key,'retroreflection is not implemented');
          if(ins.thinfilm_thickness && !ins.thinfilm_thickness.nodename && !ins.thinfilm_thickness.nodegraph && !ins.thinfilm_thickness.interfacename && Number(ins.thinfilm_thickness.value)!==0)fail('UNSUPPORTED',key,'thin film is not implemented');
          if(ins.distribution && ins.distribution.value!=='ggx')fail('UNSUPPORTED',key,'only GGX microfacets are implemented');
          if(n.category==='oren_nayar_diffuse_bsdf') {
            code=`nativeDiffuse(${x('color',[.18,.18,.18],'color3')},${x('weight',1,'float')},${x('roughness',0,'float')})`;
          } else if(n.category==='conductor_bsdf') {
            code=`nativeConductor(${x('ior',[.183,.421,1.373],'color3')},${x('extinction',[3.424,2.346,1.77],'color3')},${x('roughness',[.05,.05],'vector2')},${x('weight',1,'float')})`;
          } else {
            const mode=['R','T','RT'].indexOf(ins.scatter_mode?.value??'R');if(mode<0||ins.scatter_mode?.nodename||ins.scatter_mode?.nodegraph||ins.scatter_mode?.interfacename)fail('UNSUPPORTED',key,'invalid or connected scatter_mode');
            code=`nativeDielectric(${x('tint',[1,1,1],'color3')},${x('ior',1.5,'float')},${x('roughness',[.05,.05],'vector2')},${x('weight',1,'float')},${mode+1}u)`;
          }
          code=`closureLeaf(${code})`;closureCount=1;break;
        }
        case 'sheen_bsdf': {
          if (ins.mode?.nodename || ins.mode?.nodegraph || ins.mode?.interfacename || (ins.mode?.value && !['conty_kulla', 'zeltner'].includes(ins.mode.value))) fail('UNSUPPORTED', key, 'dynamic or unknown sheen mode is not implemented');
          code=`closureLeaf(nativeDiffuse(${x('color',[1,1,1],'color3')},${x('weight',1,'float')},${x('roughness',.3,'float')}))`;closureCount=1;break;
        }
        case 'subsurface_bsdf': {
          // Approximate fallback: preserve weight/color and use a broad diffuse
          // lobe. Radius/anisotropy are retained as diagnostics until the true
          // random-walk BSSRDF is wired into the path state.
          code=`closureLeaf(nativeDiffuse(${x('color',[.18,.18,.18],'color3')},${x('weight',1,'float')},.9))`;closureCount=1;break;
        }
        case 'translucent_bsdf': {
          // Diffuse-transmission approximation; full two-sided transport is
          // still represented by the native dielectric path only.
          code=`closureLeaf(nativeDielectric(${x('color',[1,1,1],'color3')},1.5,vec2f(.5),${x('weight',1,'float')},2u))`;closureCount=1;break;
        }
        case 'hair_bsdf': {
          // Normalize legacy melanin and explicit-color forms into a bounded
          // fiber lobe; longitudinal and azimuthal roughness remain dynamic.
          const color = ins.color ? x('color',[.6,.25,.08],'color3') : ins.base_color ? x('base_color',[.6,.25,.08],'color3') :
            `mix(vec3f(.85,.55,.32),vec3f(.03,.008,.002),clamp(${x('melanin',0,'float')},0.0,1.0))`;
          const longitudinal = ins.longitudinal_roughness ? x('longitudinal_roughness',.35,'float') : x('roughness',.35,'float');
          const azimuthal = ins.azimuthal_roughness ? x('azimuthal_roughness',.3,'float') : longitudinal;
          code=`closureLeaf(nativeHair(${color},${x('weight',1,'float')},${longitudinal},${azimuthal}))`;closureCount=1;break;
        }
        case 'generalized_schlick_bsdf': {
          code=`closureLeaf(nativeGeneralizedSchlick(${x('color0',[1,1,1],'color3')},${x('color90',[1,1,1],'color3')},${x('roughness',[.05,.05],'vector2')},${x('weight',1,'float')},${x('exponent',5,'float')}))`;closureCount=1;break;
        }
        case 'layer': {
          const top=input('top',undefined,'BSDF'),base=input('base');
          if (base.type==='BSDF') {
            code=`${top.hasInterior || base.hasInterior ? 'closureAddPreservingInterior' : 'closureAdd'}(${top.code},${base.code})`;closureCount=(top.closureCount||0)+(base.closureCount||0);hasInterior=top.hasInterior||base.hasInterior;interiorCategories=top.hasInterior?top.interiorCategories:base.interiorCategories;break;
          }
          if(base.type!=='VDF')fail('UNSUPPORTED',key,'layer base must be a VDF or BSDF closure');
          if(top.hasInterior)fail('SEMANTICS',key,'closure already has an interior');
          code=`closureInterior(${top.code},${base.code})`;closureCount=top.closureCount||0;hasInterior=true;interiorCategories=base.categories||[];break;
        }
        case 'anisotropic_vdf': {
          const absorption=ins.absorption ? input('absorption') : {type:'color3',code:'vec3f(0)'}, scattering=ins.scattering ? input('scattering') : {type:'color3',code:'vec3f(0)'};
          if(!['color3','vector3'].includes(absorption.type)||!['color3','vector3'].includes(scattering.type))fail('TYPE',key,'volume coefficients must be color3/vector3');
          code=`Medium(${absorption.code},${scattering.code},${x('anisotropy',0,'float')})`; break;
        }
        case 'image': {
          if (!['float', 'color3', 'color4', 'vector2', 'vector3', 'vector4'].includes(type)) fail('TYPE', key, 'invalid image output type');
          for (const name of Object.keys(ins)) if (!['file', 'default', 'texcoord', 'uaddressmode', 'vaddressmode', 'filtertype', 'layer', 'framerange', 'frameoffset', 'frameendaction'].includes(name)) fail('UNSUPPORTED', key, `unsupported image input ${name}`);
          for (const name of ['layer', 'framerange', 'frameoffset']) if (ins[name] && !['', '0', 0].includes(ins[name].value)) fail('UNSUPPORTED', key, `image ${name} is not implemented`);
          const file = ins.file?.value ?? '';
          if (ins.file && (ins.file.nodename || ins.file.nodegraph || ins.file.interfacename)) fail('UNSUPPORTED', key, 'connected image filenames are not implemented');
          const fallback = x('default', widths[type] === 1 ? 0 : Array(widths[type]).fill(0), type);
          if (!file) { code = fallback; break; }
          const descriptor = Object.hasOwn(imageDescriptors, file) && imageDescriptors[file];
          if (!descriptor) fail('RESOURCE', key, `missing decoded image ${file}`);
          if (n.colorspace && n.colorspace !== descriptor.colorspace) fail('SEMANTICS', key, 'image colorspace differs from decoded resource');
          const address = name => {
            const p = ins[name]; const mode = ['constant', 'clamp', 'periodic', 'mirror'].indexOf(p?.value ?? 'periodic');
            if (mode < 0 || p?.nodename || p?.interfacename || p?.nodegraph) fail('UNSUPPORTED', key, 'invalid or connected image address mode');
            return `${mode}u`;
          };
          const filter = ins.filtertype?.value ?? 'linear';
          if (!['closest', 'linear'].includes(filter) || ins.filtertype?.nodename || ins.filtertype?.interfacename || ins.filtertype?.nodegraph) fail('UNSUPPORTED', key, 'only static closest/linear image filters are implemented');
          const uv = ins.texcoord ? x('texcoord', undefined, 'vector2') : 'ctx.uv';
          const fill = widths[type] === 4 ? fallback : widths[type] === 3 ? `vec4f(${fallback},0)` : widths[type] === 2 ? `vec4f(${fallback},0,0)` : `vec4f(${fallback})`;
          const swizzle = ({ float: 'r', vector2: 'rg', vector3: 'rgb', color3: 'rgb', vector4: 'rgba', color4: 'rgba' })[type];
          const size = `vec2f(${descriptor.width}.0,${descriptor.height}.0)`;
          const lod = `log2(max(1.0,max(length(ctx.uvDx*${size}),length(ctx.uvDy*${size}))))`;
          code = `imageSample(${descriptor.offset}u,vec2u(${descriptor.width}u,${descriptor.height}u),${descriptor.levels}u,${uv},${lod},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${filter === 'linear'},${fill}).${swizzle}`;
          break;
        }
        case 'constant': code = same('value'); break;
        case 'add': {
          if(type==='BSDF') {const a=input('in1',undefined,'BSDF'),b=input('in2',undefined,'BSDF');code=`${a.hasInterior||b.hasInterior?'closureAddPreservingInterior':'closureAdd'}(${a.code},${b.code})`;closureCount=(a.closureCount||0)+(b.closureCount||0);hasInterior=a.hasInterior||b.hasInterior;interiorCategories=a.hasInterior?a.interiorCategories:b.interiorCategories;}
          else code = binary('+'); break;
        }
        case 'subtract': code = binary('-'); break;
        case 'multiply': case 'divide': {
          const a = input('in1', undefined, type), b = input('in2');
          if(type==='BSDF') {
            if(n.category!=='multiply'||!['float','color3'].includes(b.type))fail('TYPE',key,'BSDF weighting requires float or color3 multiplication');
            code=`closureScale(${a.code},${b.type==='float'?`vec3f(${b.code})`:b.code})`;closureCount=a.closureCount||0;hasInterior=a.hasInterior||false;interiorCategories=a.interiorCategories||[];break;
          }
          if (b.type !== type && b.type !== 'float' && !(type==='EDF' && b.type==='color3')) fail('TYPE', key, 'invalid scalar/vector arithmetic');
          const rhs=b.type==='float'&&widths[type]>=2&&widths[type]<=4?`${types[type]}(${b.code})`:b.code;
          code = `(${a.code} ${n.category === 'multiply' ? '*' : '/'} ${rhs})`; break;
        }
        case 'modulo': code = `(${same('in1')} - ${same('in2')} * floor(${same('in1')} / ${same('in2')}))`; break;
        case 'power': case 'min': case 'max': code = `${n.category === 'power' ? 'pow' : n.category}(${same('in1')},${scalarOrSame('in2')})`; break;
        case 'absval': case 'sign': case 'floor': case 'ceil': case 'round': case 'sqrt': case 'ln': case 'exp': case 'sin': case 'cos': case 'tan': case 'asin': case 'acos': case 'normalize': code = `${({ absval: 'abs', ln: 'log' })[n.category] || n.category}(${same('in')})`; break;
        case 'atan2': code = `atan2(${same('iny')},${same('inx')})`; break;
        case 'clamp': code = `clamp(${same('in')},${scalarOrSame('low')},${scalarOrSame('high')})`; break;
        case 'mix': {
          if(type==='BSDF'){const a=input('bg',undefined,'BSDF'),b=input('fg',undefined,'BSDF');code=`${a.hasInterior||b.hasInterior?'closureMixPreservingInterior':'closureMix'}(${a.code},${b.code},${x('mix',0,'float')})`;closureCount=(a.closureCount||0)+(b.closureCount||0);hasInterior=a.hasInterior||b.hasInterior;interiorCategories=a.hasInterior?a.interiorCategories:b.interiorCategories;}
          else code = `mix(${same('bg')},${same('fg')},${x('mix')})`; break;
        }
        case 'smoothstep': code = `smoothstep(${scalarOrSame('low')},${scalarOrSame('high')},${same('in')})`; break;
        case 'invert': code = `(${scalarOrSame('amount')} - ${same('in')})`; break;
        case 'dot': result = input('in',undefined,type); break;
        case 'magnitude': code = `length(${x('in')})`; break;
        case 'dotproduct': code = `dot(${x('in1')},${x('in2')})`; break;
        case 'crossproduct': code = `cross(${x('in1', undefined, 'vector3')},${x('in2', undefined, 'vector3')})`; break;
        case 'texcoord':
          if (ins.index && Number(ins.index.value) !== 0) fail('GEOMETRY', key, 'only texcoord index 0 is available');
          code = type === 'vector2' ? 'ctx.uv' : 'vec3f(ctx.uv,0.0)'; break;
        case 'position': case 'normal': case 'tangent': case 'bitangent':
          if (ins.space?.value && ins.space.value !== 'world') fail('GEOMETRY', key, 'only world-space geometric vectors are available');
          code = `ctx.${n.category}`; break;
        case 'time': code = 'ctx.time'; break;
        case 'frame': code = 'ctx.frame'; break;
        case 'convert': {
          const p=input('in'),from=widths[p.type],to=widths[type];
          if(!from||!to||from>4||to>4)fail('TYPE',key,'unsupported conversion');
          if(p.type==='boolean')code=type==='integer'?`select(0i,1i,${p.code})`:`${types[type]}(select(0.0,1.0,${p.code}))`;
          else if(type==='boolean'){if(from!==1)fail('TYPE',key,'vector to boolean conversion is ambiguous');code=`(${p.code}!=${p.type==='integer'?'0i':'0.0'})`;}
          else if(from===1)code=`${types[type]}(${p.type==='integer'&&to>1?`f32(${p.code})`:p.code})`;
          else if(to===1)fail('TYPE',key,'vector to scalar conversion is ambiguous; use extract');
          else if(to<=from)code=`${types[type]}(${p.code}.${'xyzw'.slice(0,to)})`;
          else code=`${types[type]}(${p.code},${Array.from({length:to-from},(_,i)=>from+i===3?'1.0':'0.0').join(',')})`;
          break;
        }
        case 'combine2': case 'combine3': case 'combine4': {
          const values=Array.from({length:Number(n.category.at(-1))},(_,i)=>input(`in${i+1}`));
          if(values.some(p=>!['float','color3','vector2','vector3'].includes(p.type))||values.reduce((n,p)=>n+widths[p.type],0)!==widths[type])fail('TYPE',key,'combine inputs do not match output width');
          code=`${types[type]}(${values.map(p=>p.code).join(',')})`;break;
        }
        case 'transformmatrix': {
          const p=input('in',undefined,type),m=input('mat');
          if(type==='vector2'&&m.type==='matrix33')code=`(${m.code}*vec3f(${p.code},1.0)).xy`;
          else if(type==='vector3'&&m.type==='matrix44')code=`(${m.code}*vec4f(${p.code},1.0)).xyz`;
          else if(type==='vector3'&&m.type==='matrix33'||type==='vector4'&&m.type==='matrix44')code=`(${m.code}*${p.code})`;
          else fail('TYPE',key,'unsupported matrix transform overload');break;
        }
        case 'normalmap': {
          const scale=ins.scale?input('scale'):{type:'float',code:'1.0'};if(!['float','vector2'].includes(scale.type))fail('TYPE',key,'normalmap scale must be float or vector2');
          const vector=(k,field)=>ins[k]?x(k,undefined,'vector3'):`ctx.${field}`;
          code=`mxNormalmap(${x('in',[.5,.5,1],'vector3')},vec2f(${scale.code}),${vector('normal','normal')},${vector('tangent','tangent')},${vector('bitangent','bitangent')})`;break;
        }
        case 'bump3': case 'heighttonormal': {
          const height=x('in',0,'float'), scale=x('scale',1,'float');
          const vector=(k,field)=>ins[k]?x(k,undefined,'vector3'):`ctx.${field}`;
          code=`mxBumpHeight(${height},${scale},${vector('normal','normal')},${vector('tangent','tangent')},${vector('bitangent','bitangent')})`;break;
        }
        case 'open_pbr_anisotropy': {
          const rough=x('roughness',0,'float'), anisotropy=x('anisotropy',0,'float');
          const inv=`(1.0-${anisotropy})`, alphaX=`(${rough}*${rough}*sqrt(2.0/(${inv}*${inv}+1.0)))`;
          code=`vec2f(${alphaX},${inv}*${alphaX})`; break;
        }
        case 'extract': {
          const v = input('in'), idx = Number(ins.index?.value);
          if (!Number.isInteger(idx) || idx < 0 || idx >= widths[v.type] || widths[v.type] > 4) fail('INDEX', key, 'invalid or dynamic extraction index');
          code = `${v.code}[${idx}]`; break;
        }
        case 'swizzle': {
          const v = input('in'), channels = ins.channels?.value;
          if (!/^[rgbaxyzw01]{1,4}$/.test(channels || '') || channels.length !== widths[type]) fail('SWIZZLE', key, 'invalid channels');
          code = `${types[type]}(${[...channels].map(c => {
            if (c === '0' || c === '1') return `${c}.0`;
            const idx = 'xyzw'.indexOf(({ r: 'x', g: 'y', b: 'z', a: 'w' })[c] || c);
            if (idx >= widths[v.type]) fail('SWIZZLE', key, 'channel outside input');
            return `${v.code}[${idx}]`;
          }).join(',')})`; break;
        }
        case 'separate2': case 'separate3': case 'separate4': {
          const idx = ({ outx: 0, outy: 1, outz: 2, outw: 3, outr: 0, outg: 1, outb: 2, outa: 3 })[out];
          if (idx == null || idx >= Number(n.category.at(-1))) fail('OUTPUT', key, 'invalid separate output');
          code = `${x('in')}[${idx}]`; break;
        }
        case 'ifequal': case 'ifgreater': case 'ifgreatereq': code = `select(${same('in2')},${same('in1')},${x('value1')} ${({ ifequal: '==', ifgreater: '>', ifgreatereq: '>=' })[n.category]} ${x('value2')})`; break;
        case 'remap': case 'range': {
          const fallback=v=>widths[type]===1?v:Array(widths[type]).fill(v);
          const low=scalarOrSame('inlow',fallback(0)),high=scalarOrSame('inhigh',fallback(1)),outlow=scalarOrSame('outlow',fallback(0)),outhigh=scalarOrSame('outhigh',fallback(1));
          let q = `((${same('in')} - ${low}) / (${high} - ${low}))`;
          if(n.category==='range')q=`(sign(${q})*pow(abs(${q}),${literal(type,fallback(1))}/${scalarOrSame('gamma',fallback(1))}))`;
          code = `(${outlow} + ${q} * (${outhigh} - ${outlow}))`;
          if(n.category==='range')code=`select(${code},clamp(${code},${outlow},${outhigh}),${x('doclamp',false,'boolean')})`;break;
        }
        case 'rotate2d': {
          const v = x('in', undefined, 'vector2'), a = `(${x('amount', undefined, 'float')} * 0.017453292519943295)`;
          code = `(mat2x2f(cos(${a}),sin(${a}),-sin(${a}),cos(${a})) * ${v})`; break;
        }
        case 'standard_surface': case 'open_pbr_surface': {
          if (!material) fail('CONTEXT', key, 'surface requires material compilation');
          const open = n.category === 'open_pbr_surface';
          // This baseline mapping is explicitly approximate, not a reference closure.
          const pick = (names, fallback, expected) => x(names.find(name => ins[name]) || names[0], fallback, expected);
          const baseColorInput = pick(['base_color'], [0.8, 0.8, 0.8], 'color3');
          const baseWeight = open && ins.base_weight ? x('base_weight', 1, 'float') : '1.0';
          const baseColor = `(${baseColorInput} * vec3f(${baseWeight}))`;
          const fields = [baseColor, pick(open ? ['base_metalness'] : ['metalness'], 0, 'float'), pick(open ? ['specular_roughness', 'base_diffuse_roughness'] : ['specular_roughness', 'diffuse_roughness'], 0.3, 'float'), pick(open ? ['specular_ior'] : ['specular_IOR', 'specular_ior'], 1.5, 'float'), pick(open ? ['transmission_weight'] : ['transmission'], 0, 'float'), pick(['emission_color'], [1, 1, 1], 'color3'), pick(open ? ['emission_luminance'] : ['emission'], 0, 'float'), pick(open ? ['specular_roughness_anisotropy'] : ['specular_anisotropy'], 0, 'float'), pick(['transmission_color'], [1,1,1], 'color3')];
          const supported = new Set(open ? ['base_weight', 'base_color', 'base_diffuse_roughness', 'base_metalness', 'specular_weight', 'specular_color', 'specular_roughness', 'specular_ior', 'specular_roughness_anisotropy', 'transmission_weight', 'transmission_color', 'transmission_depth', 'transmission_scatter', 'subsurface_weight', 'subsurface_color', 'subsurface_radius', 'coat_weight', 'coat_color', 'coat_roughness', 'coat_ior', 'fuzz', 'fuzz_color', 'thin_film_weight', 'thin_film_thickness', 'thin_film_ior', 'emission_color', 'emission_luminance', 'geometry_opacity', 'geometry_thin_walled', 'normal'] : ['base', 'base_color', 'diffuse_roughness', 'metalness', 'specular', 'specular_color', 'specular_roughness', 'specular_IOR', 'specular_ior', 'specular_anisotropy', 'transmission', 'transmission_color', 'transmission_depth', 'transmission_scatter', 'subsurface', 'subsurface_color', 'subsurface_radius', 'coat', 'coat_color', 'coat_roughness', 'coat_IOR', 'sheen', 'sheen_color', 'emission_color', 'emission', 'opacity', 'thin_walled', 'normal', 'thin_film_thickness', 'thin_film_IOR']);
          for (const k of Object.keys(n.inputs || {})) if (!supported.has(k)) fail('UNSUPPORTED', `${key}/${k}`, 'surface input not yet implemented');
          const opacityInput = open ? 'geometry_opacity' : 'opacity';
          const opacity = ins[opacityInput] ? x(opacityInput, 1, 'float') : '1.0';
          const thin = open ? (ins.geometry_thin_walled ? `select(0u,1u,${x('geometry_thin_walled', false, 'boolean')})` : '0u') : (ins.thin_walled ? `select(0u,1u,${x('thin_walled', false, 'boolean')})` : '0u');
          const filmThickness = open && ins.thin_film_weight && ins.thin_film_thickness ? `(${x('thin_film_thickness', 0, 'float')}*clamp(${x('thin_film_weight', 0, 'float')},0.0,1.0))` : !open && ins.thin_film_thickness ? x('thin_film_thickness', 0, 'float') : '0.0';
          const filmIOR = open && ins.thin_film_ior ? x('thin_film_ior', 1.5, 'float') : !open && ins.thin_film_IOR ? x('thin_film_IOR', 1.5, 'float') : '1.5';
          const specularWeight = open ? (ins.specular_weight ? x('specular_weight', 1, 'float') : '1.0') : (ins.specular ? x('specular', 1, 'float') : '1.0');
          const subsurfaceWeight = open ? (ins.subsurface_weight ? x('subsurface_weight', 0, 'float') : '0.0') : (ins.subsurface ? x('subsurface', 0, 'float') : '0.0');
          const subsurfaceColor = ins.subsurface_color ? x('subsurface_color', [0.8, 0.8, 0.8], 'color3') : fields[0];
          const baseLobe = `makeMaterial(${fields.join(',')},${thin},${filmThickness},${filmIOR})`;
          const transmissionDepth = ins.transmission_depth ? x('transmission_depth', 0, 'float') : '0.0';
          const transmissionScatter = ins.transmission_scatter ? x('transmission_scatter', [0, 0, 0], 'color3') : 'vec3f(0)';
          const transmittedLobe = `withSpecular(withTransmission(${baseLobe},${transmissionDepth},${transmissionScatter}),${specularWeight})`;
          const closure = `closureMix(closureLeaf(${transmittedLobe}),closureLeaf(nativeDiffuse(${subsurfaceColor},1.0,.9)),clamp(${subsurfaceWeight},0.0,1.0))`;
          const coatWeight = open ? (ins.coat_weight ? x('coat_weight', 0, 'float') : '0.0') : (ins.coat ? x('coat', 0, 'float') : '0.0');
          const coatColor = ins.coat_color ? x('coat_color', [1, 1, 1], 'color3') : 'vec3f(1)';
          const coatRoughness = ins.coat_roughness ? x('coat_roughness', .1, 'float') : '.1';
          const coatIOR = open ? (ins.coat_ior ? x('coat_ior', 1.5, 'float') : '1.5') : (ins.coat_IOR ? x('coat_IOR', 1.5, 'float') : '1.5');
          const coated = `closureAdd(${closure},closureScale(closureLeaf(nativeDielectric(${coatColor},${coatIOR},vec2f(${coatRoughness}*${coatRoughness}),1.0,1u)),vec3f(clamp(${coatWeight},0.0,1.0))))`;
          const sheenWeight = open ? (ins.fuzz ? x('fuzz', 0, 'float') : '0.0') : (ins.sheen ? x('sheen', 0, 'float') : '0.0');
          const sheenColor = open ? (ins.fuzz_color ? x('fuzz_color', [1, 1, 1], 'color3') : 'vec3f(1)') : (ins.sheen_color ? x('sheen_color', [1, 1, 1], 'color3') : 'vec3f(1)');
          const finalClosure = `closureAdd(${coated},closureScale(closureLeaf(nativeDiffuse(${sheenColor},1.0,.6)),vec3f(clamp(${sheenWeight},0.0,1.0))))`;
          const normal = ins.normal ? x('normal', undefined, 'vector3') : 'ctx.normal';
          code = `materialFromClosure(${finalClosure},${fields[5]}*${fields[6]},clamp(${opacity},0.0,1.0),normalize(${normal}))`; break;
        }
        case 'surfacematerial': result = input('surfaceshader'); break;
        default: fail('UNSUPPORTED', key, `node ${n.category} (${type}) is not implemented`);
      }
      if (!result) {
        if(closureCount>MAX_CLOSURE_LOBES)fail('LIMIT',key,`closure exceeds ${MAX_CLOSURE_LOBES} lobes`);
        const target = materialCategories.has(n.category) ? 'Material' : type==='EDF' ? 'vec3f' : types[type];
        if (!target) fail('TYPE', key, `unsupported output type ${type}`);
        const id = `n${serial++}`;
        lines.push(`let ${id}: ${target} = ${code};`);
        result = { type, code: id, closureCount, hasInterior, interiorCategories, categories:[...dependencies] };
      }
    }
    used.add(n.category); active.delete(key); cached.set(key, result); return result;
  }
  const selected = output || { nodename: document.nodes.at(-1)?.name };
  const value = port(selected, root, {}, undefined, '$output');
  return { body: lines.join('\n'), expression: value.code, type: value.type, categories: [...used].sort(), hasInterior:value.hasInterior||false,interiorCategories:value.interiorCategories||[],diagnostics: [], referenceReady: false };
}

export const contextWGSL = `struct ShadingContext { position: vec3f, normal: vec3f, tangent: vec3f, bitangent: vec3f, uv: vec2f, time: f32, frame: f32, uvDx: vec2f, uvDy: vec2f }
fn mxNormalmap(value:vec3f,scale:vec2f,n:vec3f,t:vec3f,b:vec3f)->vec3f {
 let decoded=select(value*2.0-1.0,vec3f(0,0,1),dot(value,value)==0.0);
 return normalize(t*decoded.x*scale.x+b*decoded.y*scale.y+n*decoded.z);
}
fn mxBumpHeight(height:f32,scale:f32,n:vec3f,t:vec3f,b:vec3f)->vec3f {
  // Bounded height-to-normal fallback. Texture-aware finite differences are
  // supplied by normalmap/image graphs; this node keeps scalar bump graphs
  // explicit without silently turning them into geometric displacement.
  return normalize(n+t*(height*scale)+b*(height*scale));
}
struct Lobe { base: vec3f, metal: f32, roughness: f32, ior: f32, transmission: f32, emission: vec3f, emissionWeight: f32, anisotropy: f32, transmissionColor: vec3f, kind:u32, weight:f32, alpha:vec2f, complexIOR:vec3f, extinction:vec3f, scatterMode:u32, thinWalled:u32, thinFilmThickness:f32, thinFilmIOR:f32, transmissionDepth:f32, transmissionScatter:vec3f, schlickColor90:vec3f, schlickExponent:f32 }
struct Medium { absorption: vec3f, scattering: vec3f, anisotropy: f32 }
fn makeMaterial(base:vec3f,metal:f32,rough:f32,ior:f32,trans:f32,emission:vec3f,emissionWeight:f32,anisotropy:f32,tint:vec3f,thinWalled:u32,thinFilmThickness:f32,thinFilmIOR:f32)->Lobe {
 return Lobe(base,metal,rough,ior,trans,emission,emissionWeight,anisotropy,tint,0u,1.0,vec2f(rough*rough),vec3f(ior),vec3f(0),3u,thinWalled,thinFilmThickness,thinFilmIOR,0.0,vec3f(0),base,5.0);
}
fn withTransmission(lobe:Lobe,depth:f32,scatter:vec3f)->Lobe {var m=lobe;m.transmissionDepth=max(0.0,depth);m.transmissionScatter=max(vec3f(0),scatter);return m;}
fn withSpecular(lobe:Lobe,weight:f32)->Lobe {var m=lobe;m.weight=clamp(weight,0.0,1.0);return m;}
fn nativeDiffuse(color:vec3f,weight:f32,rough:f32)->Lobe {var m=makeMaterial(color,0,rough,1.5,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=3u;m.weight=weight;return m;}
fn nativeHair(color:vec3f,weight:f32,longitudinal:f32,azimuthal:f32)->Lobe {var m=makeMaterial(color,0,longitudinal,1.55,0,vec3f(0),0,azimuthal,vec3f(1),0u,0.0,1.5);m.kind=4u;m.weight=weight;m.alpha=vec2f(max(.02,longitudinal),max(.02,azimuthal));return m;}
fn nativeGeneralizedSchlick(color0:vec3f,color90:vec3f,alpha:vec2f,weight:f32,exponent:f32)->Lobe {var m=makeMaterial(color0,0,sqrt(max(alpha.x,alpha.y)),1.5,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=5u;m.weight=weight;m.alpha=alpha;m.schlickColor90=color90;m.schlickExponent=max(.01,exponent);return m;}
fn nativeDielectric(tint:vec3f,ior:f32,alpha:vec2f,weight:f32,mode:u32)->Lobe {var m=makeMaterial(tint,0,sqrt(max(alpha.x,alpha.y)),ior,1,vec3f(0),0,0,tint,0u,0.0,1.5);m.kind=1u;m.weight=weight;m.alpha=alpha;m.scatterMode=mode;return m;}
fn nativeConductor(ior:vec3f,k:vec3f,alpha:vec2f,weight:f32)->Lobe {var m=makeMaterial(vec3f(1),1,sqrt(max(alpha.x,alpha.y)),1.5,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=2u;m.complexIOR=ior;m.extinction=k;m.alpha=alpha;m.weight=weight;return m;}
${closureTypesWGSL}`;
