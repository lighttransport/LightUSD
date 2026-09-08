// SPDX-License-Identifier: Apache-2.0
// Typed, deterministic MaterialX value-graph compiler. No shader-source eval.
import { closureTypesWGSL, MAX_CLOSURE_LOBES } from './closures.js';
import { colorToLinearRec709, normalizeColorSpace } from './color.js';
export const MATERIALX_VERSION = '1.39.5';
export class GraphError extends Error {
  constructor(code, path, message) { super(`${path}: ${message}`); this.name = 'GraphError'; this.code = code; this.path = path; }
}
const types = { float: 'f32', integer: 'i32', boolean: 'bool', color3: 'vec3f', color4: 'vec4f', vector2: 'vec2f', vector3: 'vec3f', vector4: 'vec4f', matrix33: 'mat3x3f', matrix44: 'mat4x4f', VDF: 'Medium', BSDF: 'Closure', EDF: 'vec3f', lightshader: 'vec3f', volumeshader: 'Medium', displacementshader: 'f32' };
const widths = { float: 1, integer: 1, boolean: 1, color3: 3, vector3: 3, color4: 4, vector4: 4, vector2: 2, matrix33: 9, matrix44: 16 };
// Units are semantic annotations; implementations consume their authored
// convention (for example degrees for rotate2d and nanometers for thin film).
const units = new Set(['none', 'unitless', 'degree', 'radian', 'nanometer', 'micron', 'micrometer', 'millimeter', 'centimeter', 'meter', 'inch', 'second', 'millisecond', 'microsecond', 'percent']);
export const valueCategories = new Set(['constant', 'add', 'subtract', 'plus', 'minus', 'multiply', 'divide', 'modulo', 'power', 'safepower', 'min', 'max', 'screen', 'difference', 'burn', 'dodge', 'overlay', 'disjointover', 'in', 'mask', 'matte', 'out', 'over', 'inside', 'outside', 'and', 'or', 'not', 'xor', 'absval', 'sign', 'floor', 'ceil', 'round', 'fract', 'sqrt', 'ln', 'log10', 'exp', 'exp2', 'exp10', 'log2', 'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2', 'sinh', 'cosh', 'tanh', 'asinh', 'acosh', 'atanh', 'radians', 'degrees', 'clamp', 'mix', 'smoothstep', 'invert', 'normalize', 'magnitude', 'distance', 'reflect', 'refract', 'fresnel', 'facing_ratio', 'luminance', 'average', 'rgbtohsv', 'hsvtorgb', 'hsvadjust', 'colorcorrect', 'saturate', 'contrast', 'premult', 'unpremult', 'blackbody', 'artistic_ior', 'roughness_anisotropy', 'glossiness_anisotropy', 'roughness_dual', 'gooch_shade', 'cloverleaf', 'hexagon', 'tiledcloverleafs', 'tiledhexagons', 'ramp4', 'triplanarprojection', 'g18_rec709_to_lin_rec709', 'g22_rec709_to_lin_rec709', 'rec709_display_to_lin_rec709', 'g22_ap1_to_lin_rec709', 'srgb_texture_to_lin_rec709', 'lin_adobergb_to_lin_rec709', 'adobergb_to_lin_rec709', 'srgb_displayp3_to_lin_rec709', 'lin_displayp3_to_lin_rec709', 'acescg_to_lin_rec709', 'lin_rec709_to_acescg', 'lin_rec709_to_srgb', 'srgb_to_lin_rec709', 'transpose', 'determinant', 'invertmatrix', 'creatematrix', 'select', 'switch', 'noise2d', 'noise3d', 'cellnoise2d', 'cellnoise3d', 'dotproduct', 'crossproduct', 'texcoord', 'geompropvalue', 'geomcolor', 'position', 'normal', 'tangent', 'bitangent', 'viewdirection', 'time', 'frame', 'convert', 'combine2', 'combine3', 'combine4', 'extract', 'swizzle', 'ifequal', 'ifgreater', 'ifgreatereq', 'remap', 'range', 'rotate2d', 'place2d', 'dot', 'separate2', 'separate3', 'separate4']);
const materialCategories = new Set(['standard_surface', 'open_pbr_surface', 'UsdPreviewSurface', 'surface_unlit', 'surfacematerial', 'surface']);
for(const category of ['transformmatrix','transformnormal','transformpoint','transformvector','trianglewave','normalmap','hextilednormalmap','flake2d','flake3d','bump','bump3','heighttonormal','rotate3d','reorder','fractal2d','fractal3d','worleynoise2d','worleynoise3d','unifiednoise2d','unifiednoise3d','latlongimage','splitlr','splittb','ramp','ramp_gradient','ramplr','ramptb','checkerboard','line','circle','grid','crosshatch','tiledcircles','randomfloat','randomcolor','UsdUVTexture','usduvtexture','UsdPrimvarReader','UsdTransform2d','facingratio','geompropvalueuniform'])valueCategories.add(category);
function fail(code, path, message) { throw new GraphError(code, path, message); }
export function literal(type, value, path = '') {
  if(value===''&&type==='BSDF')return 'emptyClosure()';
  if(value===''&&type==='EDF')return 'vec3f(0)';
  if(value===''&&type==='VDF')return 'Medium(vec3f(0),vec3f(0),0,vec3f(0))';
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
export function compileGraph(document, { output, library = {}, material = false, imageDescriptors = {}, uvIndex = 0, geompropName = '', geompropNames = undefined, measuredProfileIds = {} } = {}) {
  if (document?.version !== undefined && !['1.39', MATERIALX_VERSION].includes(String(document.version))) fail('VERSION', '', `unsupported MaterialX version ${document.version}; expected 1.39.5`);
  const customGeompropNames = (geompropNames ?? (geompropName ? [geompropName] : [])).slice(0, 8).map(name => String(name).toLowerCase().replace(/[_-]/g, ''));
  const customGeomprop = name => { const slot = customGeompropNames.indexOf(name); return slot < 0 ? null : `ctx.geomprop${slot ? slot : ''}`; };
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
  let serial = 0, portDepth = 0, contextName = 'ctx', bumpDepth = 0;
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
    if (p.unit !== undefined && (!['string', 'number'].includes(typeof p.unit) || !units.has(String(p.unit).toLowerCase()))) fail('SEMANTICS', path, `unsupported MaterialX unit ${p.unit}`);
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
      result={type:value[0],code:value[1].replace(/\bctx\b/g,contextName)};
    } else {
      const type=p.type||wanted;let value=p.value;
      if (String(p.unit || '').toLowerCase() === 'percent' && ['float','vector2','vector3','vector4','color3','color4'].includes(type)) {
        const values = Array.isArray(value) ? value : typeof value === 'string' ? value.split(',').map(v => Number(v.trim())) : [value];
        value = values.map(v => Number(v) / 100);
      }
      if(p.colorspace&&['color3','color4'].includes(type)) {
        literal(type,value,path);
        try{value=colorToLinearRec709(Array.isArray(value)?value:String(value).split(',').map(Number),p.colorspace);}catch(e){fail('SEMANTICS',path,e.message);}
      } else if (p.colorspace) {
        let normalized;
        try { normalized = normalizeColorSpace(p.colorspace); } catch (e) { fail('SEMANTICS', path, e.message); }
        if (!['raw', 'lin_rec709'].includes(normalized)) fail('SEMANTICS', path, 'colorspace on non-color input');
      }
      result={type,code:literal(type,value,path)};
    }
    if (wanted && result.type !== wanted) fail('TYPE', path, `expected ${wanted}, got ${result.type}`);
    return result;
    } finally { portDepth--; }
  }
  function evaluate(name, out, scope, env) {
    const n = map(scope).get(name);
    if (!n) fail('NODE', name, 'unknown node');
    const key = `${scope.name}/${name}.${out}${contextName==='ctx'?'':`@${contextName}`}`;
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
      const angle = (k, fallback) => {
        const code = x(k, fallback, 'float');
        return String(ins[k]?.unit || '').toLowerCase() === 'radian' ? `(${code}*57.29577951308232)` : code;
      };
      const nanometer = (k, fallback) => {
        const code = x(k, fallback, 'float');
        const scale = ({ micron: 1e3, micrometer: 1e3, millimeter: 1e6, centimeter: 1e7, meter: 1e9, inch: 2.54e7 })[String(ins[k]?.unit || '').toLowerCase()];
        return scale ? `(${code}*${scale})` : code;
      };
      const same = k => x(k, undefined, type);
      const scalarOrSame = (k, fallback) => {
        const p=input(k,fallback);
        if(p.type===type)return p.code;
        if(p.type==='float'&&widths[type]>=2&&widths[type]<=4)return `${types[type]}(${p.code})`;
        fail('TYPE',key,`expected ${type} or scalar float for ${k}`);
      };
      const binary = op => `(${same('in1')} ${op} ${scalarOrSame('in2')})`;
      let code, closureCount = 0, hasInterior = false, interiorCategories=[], emissionCone = null, emissionSchlick = null, emissionProfile = null, lightInfo = null, volumeEmission = false, normal = null;
      switch (n.category) {
        case 'uniform_edf': code = x('color',[1,1,1],'color3'); break;
        case 'light': {
          const edf = !ins.edf || (!ins.edf.nodename && !ins.edf.nodegraph && !ins.edf.interfacename && (ins.edf.value === '' || ins.edf.value === undefined)) ? { type: 'EDF', code: 'vec3f(0)' } : input('edf', undefined, 'EDF');
          code = `${edf.code}*max(0.0,${x('intensity',1,'float')})*pow(2.0,${x('exposure',0,'float')})`;
          break;
        }
        case 'point_light': {
          lightInfo = {
            kind: 'point',
            position: x('position', undefined, 'vector3'),
            color: x('color', [1, 1, 1], 'color3'),
            intensity: x('intensity', 1, 'float'),
            decayRate: x('decay_rate', 2, 'float')
          };
          code = `${lightInfo.color}*max(0.0,${lightInfo.intensity})`;
          break;
        }
        case 'directional_light': {
          lightInfo = {
            kind: 'directional',
            direction: x('direction', undefined, 'vector3'),
            color: x('color', [1, 1, 1], 'color3'),
            intensity: x('intensity', 1, 'float')
          };
          code = `${lightInfo.color}*max(0.0,${lightInfo.intensity})`;
          break;
        }
        case 'spot_light': {
          lightInfo = {
            kind: 'spot',
            position: x('position', undefined, 'vector3'),
            direction: x('direction', undefined, 'vector3'),
            color: x('color', [1, 1, 1], 'color3'),
            intensity: x('intensity', 1, 'float'),
            decayRate: x('decay_rate', 2, 'float'),
            innerAngle: angle('inner_angle', 0),
            outerAngle: angle('outer_angle', 0)
          };
          code = `${lightInfo.color}*max(0.0,${lightInfo.intensity})`;
          break;
        }
        case 'volume': {
          const vdf = ins.vdf?.value === '' || !ins.vdf ? { type: 'VDF', code: 'Medium(vec3f(0),vec3f(0),0,vec3f(0))' } : input('vdf', undefined, 'VDF');
          const edfEmpty = !ins.edf || (!ins.edf.nodename && !ins.edf.nodegraph && !ins.edf.interfacename && (ins.edf.value === '' || ins.edf.value === undefined));
          const edf = edfEmpty ? { type: 'EDF', code: 'vec3f(0)' } : input('edf', undefined, 'EDF');
          volumeEmission = !edfEmpty;
          code = `mediumWithEmission(${vdf.code},${edf.code})`;
          break;
        }
        case 'measured_edf': {
          const file = ins.file?.value;
          if (typeof file !== 'string' || !file) fail('RESOURCE', key, 'measured_edf requires a static filename');
          const profile = document.measuredProfiles?.[file];
          if (!profile || !Array.isArray(profile.samples)) fail('RESOURCE', key, `missing parsed IES profile ${file}`);
          const id = Number(measuredProfileIds[file] || 1);
          if (!Number.isInteger(id) || id < 1) fail('RESOURCE', key, `invalid IES profile id ${file}`);
          emissionProfile = { direction: x('normal', undefined, 'vector3'), id };
          code = x('color', [1, 1, 1], 'color3');
          break;
        }
        case 'displacement':
          if (type !== 'displacementshader') fail('TYPE', key, 'displacement output must be displacementshader');
          code=`${x('displacement',0,'float')}*${x('scale',1,'float')}`; break;
        case 'conical_edf': {
          const direction=x('normal',undefined,'vector3');
          const inner=angle('inner_angle',60), outer=angle('outer_angle',0);
          code=x('color',[1,1,1],'color3');
          // MaterialX specifies full cone angles; emissionFactor compares the
          // direction cosine against the corresponding half-angle threshold.
          emissionCone={direction,innerCos:`cos(radians(max(${inner},${outer}))*0.5)`,outerCos:`cos(radians(min(${inner},${outer}))*0.5)`};
          break;
        }
        case 'generalized_schlick_edf': {
          const base=ins.base?.value===''||!ins.base ? {type:'EDF',code:'vec3f(0)'} : input('base',undefined,'EDF');
          code=base.code;
          emissionCone=base.emissionCone||null;
          emissionProfile=base.emissionProfile||null;
          emissionSchlick={color0:x('color0',[1,1,1],'color3'),color90:x('color90',[1,1,1],'color3'),exponent:x('exponent',5,'float')};
          break;
        }
        case 'surface': {
          const opacity = ins.opacity ? x('opacity', 1, 'float') : '1.0';
          const thin = ins.thin_walled ? `select(0u,1u,${x('thin_walled', false, 'boolean')})` : '0u';
          const bsdfValue=ins.bsdf?.value===''||!ins.bsdf ? null : input('bsdf',undefined,'BSDF');
          const bsdf=bsdfValue?.code||'emptyClosure()';hasInterior=bsdfValue?.hasInterior||false;interiorCategories=bsdfValue?.interiorCategories||[];
          const surfaceNormal=ins.normal ? x('normal',undefined,'vector3') : (bsdfValue?.normal || 'ctx.normal');
          const edfValue=ins.edf?.value===''||!ins.edf ? {code:'vec3f(0)'} : input('edf',undefined,'EDF');
          const cone=edfValue.emissionCone, schlick=edfValue.emissionSchlick;
          const profile=edfValue.emissionProfile;
          code=`surfaceEmission(${bsdf},${edfValue.code},clamp(${opacity},0.0,1.0),${thin},${surfaceNormal},${profile?.direction||cone?.direction||'ctx.normal'},${cone?.innerCos||'-1.0'},${cone?.outerCos||'-1.0'},${schlick?.color0||'vec3f(1)'},${schlick?.color90||'vec3f(1)'},${schlick?.exponent||'5.0'},${cone?'1u':'0u'},${schlick?'1u':'0u'},${profile ? `${profile.id}u` : '0u'})`; normal=surfaceNormal;break;
        }
        case 'dielectric_bsdf': case 'conductor_bsdf': case 'oren_nayar_diffuse_bsdf': case 'burley_diffuse_bsdf': {
          normal = ins.normal ? x('normal', undefined, 'vector3') : null;
          if(ins.retroreflective && ![false,'false'].includes(ins.retroreflective.value))fail('UNSUPPORTED',key,'retroreflection is not implemented');
          if (ins.tangent && (ins.tangent.value !== undefined || ins.tangent.nodename || ins.tangent.nodegraph || ins.tangent.interfacename)) fail('UNSUPPORTED', key, `${n.category} authored tangent is not implemented`);
          if (n.category === 'oren_nayar_diffuse_bsdf' && ins.energy_compensation && ![false, 'false'].includes(ins.energy_compensation.value)) fail('UNSUPPORTED', key, 'energy-compensated Oren-Nayar is not implemented');
          const filmThicknessInput=ins.thinfilm_thickness||ins.thin_film_thickness;
          const filmIORInput=ins.thinfilm_IOR||ins.thinfilm_ior||ins.thin_film_IOR||ins.thin_film_ior;
          if(ins.distribution && ins.distribution.value!=='ggx')fail('UNSUPPORTED',key,'only GGX microfacets are implemented');
          if(n.category==='oren_nayar_diffuse_bsdf') {
            code=`nativeDiffuse(${x('color',[.18,.18,.18],'color3')},${x('weight',1,'float')},${x('roughness',0,'float')})`;
          } else if (n.category === 'burley_diffuse_bsdf') {
            code=`nativeBurley(${x('color',[.18,.18,.18],'color3')},${x('weight',1,'float')},${x('roughness',0,'float')})`;
          } else if(n.category==='conductor_bsdf') {
            const conductor=`nativeConductor(${x('ior',[.183,.421,1.373],'color3')},${x('extinction',[3.424,2.346,1.77],'color3')},${x('roughness',[.05,.05],'vector2')},${x('weight',1,'float')})`;
            code=filmThicknessInput?`withThinFilm(${conductor},${nanometer(filmThicknessInput===ins.thinfilm_thickness?'thinfilm_thickness':'thin_film_thickness',0)},${filmIORInput?x(filmIORInput===ins.thinfilm_IOR?'thinfilm_IOR':filmIORInput===ins.thinfilm_ior?'thinfilm_ior':filmIORInput===ins.thin_film_IOR?'thin_film_IOR':'thin_film_ior',1.5,'float'):'1.5'})`:conductor;
          } else {
            const mode=['R','T','RT'].indexOf(ins.scatter_mode?.value??'R');if(mode<0||ins.scatter_mode?.nodename||ins.scatter_mode?.nodegraph||ins.scatter_mode?.interfacename)fail('UNSUPPORTED',key,'invalid or connected scatter_mode');
            const dielectric=`nativeDielectric(${x('tint',[1,1,1],'color3')},${x('ior',1.5,'float')},${x('roughness',[.05,.05],'vector2')},${x('weight',1,'float')},${mode+1}u)`;
            const film=filmThicknessInput?`withThinFilm(${dielectric},${nanometer(filmThicknessInput===ins.thinfilm_thickness?'thinfilm_thickness':'thin_film_thickness',0)},${filmIORInput?x(filmIORInput===ins.thinfilm_IOR?'thinfilm_IOR':filmIORInput===ins.thinfilm_ior?'thinfilm_ior':filmIORInput===ins.thin_film_IOR?'thin_film_IOR':'thin_film_ior',1.5,'float'):'1.5'})`:dielectric;
            code=film;
          }
          code=`closureLeaf(${code})`;closureCount=1;break;
        }
        case 'sheen_bsdf': {
          normal = ins.normal ? x('normal', undefined, 'vector3') : null;
          if (ins.mode?.nodename || ins.mode?.nodegraph || ins.mode?.interfacename || (ins.mode?.value && !['conty_kulla', 'zeltner'].includes(ins.mode.value))) fail('UNSUPPORTED', key, 'dynamic or unknown sheen mode is not implemented');
          const mode=ins.mode?.value==='zeltner'?1:0;
          code=`closureLeaf(nativeSheen(${x('color',[1,1,1],'color3')},${x('weight',1,'float')},${x('roughness',.3,'float')},${mode}u))`;closureCount=1;break;
        }
        case 'deon_hair_absorption_from_melanin': {
          if (type !== 'vector3') fail('TYPE', key, 'deon_hair_absorption_from_melanin output must be vector3');
          const concentration=x('melanin_concentration',.25,'float'), redness=x('melanin_redness',.5,'float');
          const melanin=`(-log(max(1.0-${concentration},0.0001)))`;
          const eumelanin=`(${melanin}*(1.0-${redness}))`, pheomelanin=`(${melanin}*${redness})`;
          const eumelaninColor=x('eumelanin_color',[.657704,.498077,.254107],'color3'), pheomelaninColor=x('pheomelanin_color',[.829444,.67032,.349938],'color3');
          code=`max(${eumelanin}*(-log(${eumelaninColor}))+${pheomelanin}*(-log(${pheomelaninColor})),vec3f(0.0))`; break;
        }
        case 'chiang_hair_absorption_from_color': {
          if (type !== 'vector3') fail('TYPE', key, 'chiang_hair_absorption_from_color output must be vector3');
          const beta=x('azimuthal_roughness',.2,'float'), color=x('color',[1,1,1],'color3');
          const b2=`(${beta}*${beta})`, b4=`(${b2}*${b2})`;
          const factor=`(5.969-0.215*${beta}+2.532*${b2}-10.73*${b2}*${beta}+5.574*${b4}+0.245*${b4}*${beta})`;
          const sigma=`(log(min(max(${color},vec3f(0.001)),vec3f(1.0)))/${factor})`;
          code=`${sigma}*${sigma}`; break;
        }
        case 'chiang_hair_roughness': {
          if (!['roughness_R','roughness_TT','roughness_TRT'].includes(out) || type !== 'vector2') fail('OUTPUT', key, 'chiang_hair_roughness output must be one of roughness_R, roughness_TT, or roughness_TRT');
          const longitudinal=x('longitudinal',.1,'float'), azimuthal=x('azimuthal',.2,'float'), scaleTT=x('scale_TT',.5,'float'), scaleTRT=x('scale_TRT',2.0,'float');
          const lr=`clamp(${longitudinal},0.001,1.0)`, ar=`clamp(${azimuthal},0.001,1.0)`;
          const v=`pow(0.726*${lr}+0.812*${lr}*${lr}+3.7*pow(${lr},20.0),2.0)`, s=`0.265*${ar}+1.194*${ar}*${ar}+5.372*pow(${ar},22.0)`;
          code=out==='roughness_R'?`vec2f(${v},${s})`:out==='roughness_TT'?`vec2f(${v}*${scaleTT}*${scaleTT},${s})`:`vec2f(${v}*${scaleTRT}*${scaleTRT},${s})`; break;
        }
        case 'subsurface_bsdf': {
          if (ins.tangent?.value!==undefined || ins.tangent?.nodename || ins.tangent?.nodegraph || ins.tangent?.interfacename) fail('UNSUPPORTED',key,'subsurface authored tangent is not implemented');
          normal=ins.normal ? x('normal',undefined,'vector3') : null;
          code=`closureLeaf(nativeSubsurface(${x('color',[.18,.18,.18],'color3')},${x('weight',1,'float')},${x('radius',[1,1,1],'color3')},${x('anisotropy',0,'float')}))`;closureCount=1;break;
        }
        case 'translucent_bsdf': {
          // MaterialX translucent_bsdf is a diffuse transmission closure.
          // The bounded transport lobe samples the opposite hemisphere and
          // carries the authored color and weight through direct/indirect paths.
          normal = ins.normal ? x('normal', undefined, 'vector3') : null;
          if (ins.tangent && (ins.tangent.value !== undefined || ins.tangent.nodename || ins.tangent.nodegraph || ins.tangent.interfacename)) fail('UNSUPPORTED', key, 'translucent_bsdf authored tangent is not implemented');
          code=`closureLeaf(nativeTranslucent(${x('color',[1,1,1],'color3')},${x('weight',1,'float')}))`;closureCount=1;break;
        }
        case 'hair_bsdf': case 'chiang_hair_bsdf': {
          // Normalize legacy melanin and explicit-color forms into a bounded
          // fiber lobe; longitudinal and azimuthal roughness remain dynamic.
          normal = ins.normal ? x('normal', undefined, 'vector3') : null;
          if (ins.tangent && (ins.tangent.value !== undefined || ins.tangent.nodename || ins.tangent.nodegraph || ins.tangent.interfacename)) fail('UNSUPPORTED', key, `${n.category} authored tangent is not implemented`);
          const color = ins.absorption_coefficient ? `exp(-${x('absorption_coefficient',[0,0,0],'vector3')})` : ins.color ? x('color',[.6,.25,.08],'color3') : ins.base_color ? x('base_color',[.6,.25,.08],'color3') : ins.tint_R ? x('tint_R',[1,1,1],'color3') :
            `mix(vec3f(.85,.55,.32),vec3f(.03,.008,.002),clamp(${x('melanin',0,'float')},0.0,1.0))`;
          const longitudinal = ins.longitudinal_roughness ? x('longitudinal_roughness',.35,'float') : ins.roughness_R ? `${x('roughness_R',[.1,.1],'vector2')}.x` : x('roughness',.35,'float');
          const azimuthal = ins.azimuthal_roughness ? x('azimuthal_roughness',.3,'float') : ins.roughness_TT ? `${x('roughness_TT',[.05,.05],'vector2')}.x` : longitudinal;
          code=`closureLeaf(nativeHair(${color},${x('weight',1,'float')},${longitudinal},${azimuthal},${x('ior',1.55,'float')}))`;closureCount=1;break;
        }
        case 'generalized_schlick_bsdf': {
          const connected=name=>n.inputs?.[name]&&(n.inputs[name].nodename||n.inputs[name].nodegraph||n.inputs[name].interfacename);
          if (connected('retroreflective') || n.inputs?.retroreflective?.value===true || n.inputs?.retroreflective?.value==='true') fail('UNSUPPORTED',key,'generalized Schlick retroreflection is not implemented');
          if (connected('distribution') || n.inputs?.distribution?.value && n.inputs.distribution.value!=='ggx') fail('UNSUPPORTED',key,'generalized Schlick supports only GGX distribution');
          if (connected('scatter_mode') || n.inputs?.scatter_mode?.value && n.inputs.scatter_mode.value!=='R') fail('UNSUPPORTED',key,'generalized Schlick supports reflection scatter_mode R only');
          const filmThicknessInput=ins.thinfilm_thickness||ins.thin_film_thickness;
          const filmIORInput=ins.thinfilm_IOR||ins.thinfilm_ior||ins.thin_film_IOR||ins.thin_film_ior;
          const filmThickness=filmThicknessInput ? nanometer(filmThicknessInput===ins.thinfilm_thickness?'thinfilm_thickness':'thin_film_thickness',0) : '0.0';
          const filmIOR=filmIORInput ? x(filmIORInput===ins.thinfilm_IOR?'thinfilm_IOR':filmIORInput===ins.thinfilm_ior?'thinfilm_ior':filmIORInput===ins.thin_film_IOR?'thin_film_IOR':'thin_film_ior',1.5,'float') : '1.5';
          if (n.inputs?.tangent && (n.inputs.tangent.nodename||n.inputs.tangent.nodegraph||n.inputs.tangent.interfacename||n.inputs.tangent.value!==undefined)) fail('UNSUPPORTED',key,'generalized Schlick authored tangent is not implemented');
          normal=n.inputs?.normal ? x('normal',undefined,'vector3') : null;
          code=`closureLeaf(nativeGeneralizedSchlick(${x('color0',[1,1,1],'color3')},${x('color82',[1,1,1],'color3')},${x('color90',[1,1,1],'color3')},${x('roughness',[.05,.05],'vector2')},${x('weight',1,'float')},${x('exponent',5,'float')},${filmThickness},${filmIOR}))`;closureCount=1;break;
        }
        case 'layer': {
          const top=input('top',undefined,'BSDF'),base=input('base');
          if (base.type==='BSDF') {
            if ((base.closureCount || 0) === 0) {
              code=top.code;closureCount=top.closureCount||0;hasInterior=top.hasInterior||false;interiorCategories=top.interiorCategories||[];break;
            }
            fail('UNSUPPORTED', key, 'BSDF-over-BSDF layering requires recursive interface transport');
          }
          if(base.type!=='VDF')fail('UNSUPPORTED',key,'layer base must be a VDF or BSDF closure');
          if(top.hasInterior)fail('SEMANTICS',key,'closure already has an interior');
          code=`closureInterior(${top.code},${base.code})`;closureCount=top.closureCount||0;hasInterior=true;interiorCategories=base.categories||[];break;
        }
        case 'anisotropic_vdf': {
          const absorption=ins.absorption ? input('absorption') : {type:'color3',code:'vec3f(0)'}, scattering=ins.scattering ? input('scattering') : {type:'color3',code:'vec3f(0)'};
          if(!['color3','vector3'].includes(absorption.type)||!['color3','vector3'].includes(scattering.type))fail('TYPE',key,'volume coefficients must be color3/vector3');
          code=`Medium(${absorption.code},${scattering.code},${x('anisotropy',0,'float')},vec3f(0))`; break;
        }
        case 'absorption_vdf': {
          const absorption=input('absorption', [0, 0, 0]);
          if (!['color3','vector3'].includes(absorption.type)) fail('TYPE', key, 'absorption coefficient must be color3/vector3');
          code=`Medium(${absorption.code},vec3f(0),0.0,vec3f(0))`; break;
        }
        case 'triplanarprojection': {
          if (!['float','color3','color4','vector2','vector3','vector4'].includes(type)) fail('TYPE', key, 'triplanarprojection requires an image-compatible output type');
          for (const name of Object.keys(ins)) if (!['filex','filey','filez','position','normal','default','filtertype','uaddressmode','vaddressmode'].includes(name)) fail('UNSUPPORTED', key, `unsupported triplanar input ${name}`);
          const fallback=x('default',widths[type]===1?0:Array(widths[type]).fill(0),type), files=['filex','filey','filez'];
          const descriptor=files.map(file=>{const p=ins[file];if(!p?.value||p.nodename||p.nodegraph||p.interfacename)fail('RESOURCE',key,`${file} requires a static resolved filename`);const d=Object.hasOwn(imageDescriptors,p.value)&&imageDescriptors[p.value];if(!d)fail('RESOURCE',key,`missing decoded image ${p.value}`);if(n.colorspace&&normalizeColorSpace(n.colorspace)!==normalizeColorSpace(d.colorspace))fail('SEMANTICS',key,`${file} colorspace differs from decoded resource`);return {file,p,d};});
          const address=name=>{const p=ins[name];const mode=['constant','clamp','periodic','mirror'].indexOf(p?.value??'periodic');if(mode<0||p?.nodename||p?.interfacename||p?.nodegraph)fail('UNSUPPORTED',key,'invalid triplanar image address mode');return `${mode}u`;};
          const filter=ins.filtertype?.value??'linear';if(!['closest','linear','cubic'].includes(filter)||ins.filtertype?.nodename||ins.filtertype?.interfacename||ins.filtertype?.nodegraph)fail('UNSUPPORTED',key,'invalid triplanar image filter');
          const swizzle=({float:'r',vector2:'rg',vector3:'rgb',color3:'rgb',vector4:'rgba',color4:'rgba'})[type], pos=ins.position?x('position',undefined,'vector3'):'ctx.position', nrm=ins.normal?x('normal',undefined,'vector3'):'ctx.normal';
          const uv=[`${pos}.yz`,`${pos}.xz`,`${pos}.xy`], projectedDerivatives=[['(ctx.dpdu*ctx.uvDx.x+ctx.dpdv*ctx.uvDx.y).yz','(ctx.dpdu*ctx.uvDy.x+ctx.dpdv*ctx.uvDy.y).yz'],['(ctx.dpdu*ctx.uvDx.x+ctx.dpdv*ctx.uvDx.y).xz','(ctx.dpdu*ctx.uvDy.x+ctx.dpdv*ctx.uvDy.y).xz'],['(ctx.dpdu*ctx.uvDx.x+ctx.dpdv*ctx.uvDx.y).xy','(ctx.dpdu*ctx.uvDy.x+ctx.dpdv*ctx.uvDy.y).xy']], samples=descriptor.map(({d},i)=>{const fill=widths[type]===4?fallback:widths[type]===3?`vec4f(${fallback},0)`:widths[type]===2?`vec4f(${fallback},0,0)`:`vec4f(${fallback})`;const grid=d.udim&&`vec2u(${d.udim.columns}u,${d.udim.rows}u)`;const lod=`log2(max(1.0,max(length(${projectedDerivatives[i][0]}*vec2f(${d.width}.0,${d.height}.0)),length(${projectedDerivatives[i][1]}*vec2f(${d.width}.0,${d.height}.0)))))`;const call=filter==='cubic'?(d.udim?`imageSampleCubicUDIM(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv[i]},${grid},${lod},${fill})`:`imageSampleCubic(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv[i]},${lod},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${fill})`):(d.udim?`imageSampleUDIM(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv[i]},${grid},${lod},${filter==='linear'},${fill})`:`imageSample(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv[i]},${lod},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${filter==='linear'},${fill})`);return `${call}.${swizzle}`;});
          const stableNormal=`safeNormal(${nrm},vec3f(0.0,0.0,1.0))`, weights=`abs(${stableNormal})/max(1e-6,dot(abs(${stableNormal}),vec3f(1.0)))`;code=`${samples[0]}*${weights}.x+${samples[1]}*${weights}.y+${samples[2]}*${weights}.z`;break;
        }
        case 'flake2d': case 'flake3d': {
          if (!['id','rand','presence','flakenormal'].includes(out)) fail('OUTPUT',key,`${n.category} output must be id, rand, presence, or flakenormal`);
          const allowed=['size','roughness','coverage','normal','tangent','bitangent',n.category==='flake2d'?'texcoord':'position'];for(const name of Object.keys(ins))if(!allowed.includes(name))fail('UNSUPPORTED',key,`unsupported ${n.category} input ${name}`);
          const size=x('size',.01,'float'),roughness=x('roughness',.1,'float'),coverage=x('coverage',.5,'float'),normal=ins.normal?x('normal',undefined,'vector3'):'ctx.normal',tangent=ins.tangent?x('tangent',undefined,'vector3'):'ctx.tangent',bitangent=ins.bitangent?x('bitangent',undefined,'vector3'):'ctx.bitangent',position=n.category==='flake2d'?(ins.texcoord?`vec3f(${x('texcoord',undefined,'vector2')},0.0)`:'vec3f(ctx.uv,0.0)'):(ins.position?x('position',undefined,'vector3'):'ctx.position');
          code=`mxFlake(${size},${roughness},${coverage},${position},${normal},${tangent},${bitangent}).${out}`;break;
        }
        case 'hextiledimage': {
          if (!['color3','color4'].includes(type)) fail('TYPE',key,'hextiledimage output must be color3 or color4');
          for (const name of Object.keys(ins)) if (!['file','default','texcoord','tiling','rotation','rotationrange','scale','scalerange','offset','offsetrange','falloff','falloffcontrast','lumacoeffs','filtertype','uaddressmode','vaddressmode'].includes(name)) fail('UNSUPPORTED',key,`unsupported hextiledimage input ${name}`);
          const file=ins.file?.value??'', fallback=x('default',type==='color4'?[0,0,0,0]:[0,0,0],type); if(!file){code=fallback;break;}
          if(ins.file?.nodename||ins.file?.nodegraph||ins.file?.interfacename)fail('UNSUPPORTED',key,'connected hextiledimage filenames are not implemented');
          const descriptor=Object.hasOwn(imageDescriptors,file)&&imageDescriptors[file];if(!descriptor)fail('RESOURCE',key,`missing decoded image ${file}`);const udim=descriptor.udim;if(udim&&(!Number.isInteger(udim.columns)||!Number.isInteger(udim.rows)||udim.columns<1||udim.rows<1))fail('RESOURCE',key,'invalid UDIM atlas descriptor');if(n.colorspace&&normalizeColorSpace(n.colorspace)!==normalizeColorSpace(descriptor.colorspace))fail('SEMANTICS',key,'hextiledimage colorspace differs from decoded resource');
          const address=name=>{const p=ins[name],mode=['constant','clamp','periodic','mirror'].indexOf(p?.value??'periodic');if(mode<0||p?.nodename||p?.nodegraph||p?.interfacename)fail('UNSUPPORTED',key,'invalid or connected hextiledimage address mode');return `${mode}u`;};
          const filter=ins.filtertype?.value??'linear';if(!['closest','linear'].includes(filter)||ins.filtertype?.nodename||ins.filtertype?.nodegraph||ins.filtertype?.interfacename)fail('UNSUPPORTED',key,'hextiledimage supports only static closest/linear filtering');
          const uv=ins.texcoord?x('texcoord',undefined,'vector2'):'ctx.uv',tiling=x('tiling',[1,1],'vector2'),coord=`(${uv}*${tiling})`,ddx=`(ctx.uvDx*${tiling})`,ddy=`(ctx.uvDy*${tiling})`,fill=type==='color4'?fallback:`vec4f(${fallback},0.0)`,hexArgs=`${descriptor.offset}u,vec2u(${descriptor.width}u,${descriptor.height}u),${descriptor.levels}u,${coord},${ddx},${ddy},${x('rotation',1,'float')},${x('rotationrange',[0,360],'vector2')},${x('scale',1,'float')},${x('scalerange',[.5,2],'vector2')},${x('offset',1,'float')},${x('offsetrange',[0,1],'vector2')},${x('falloff',.5,'float')},${x('falloffcontrast',.5,'float')},${x('lumacoeffs',[.2722287,.6740818,.0536895],'color3')}`;
          const sample=udim?`imageHextileUDIM(${hexArgs},vec2u(${udim.columns}u,${udim.rows}u),${filter==='linear'},${fill})`:`imageHextile(${hexArgs},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${filter==='linear'},${fill})`;
          code=type==='color4'?sample:`${sample}.rgb`;break;
        }
        case 'hextilednormalmap': {
          if (type !== 'vector3') fail('TYPE',key,'hextilednormalmap output must be vector3');
          for (const name of Object.keys(ins)) if (!['file','default','texcoord','tiling','rotation','rotationrange','scale','scalerange','offset','offsetrange','falloff','strength','flip_g','normal','tangent','bitangent','filtertype','uaddressmode','vaddressmode'].includes(name)) fail('UNSUPPORTED',key,`unsupported hextilednormalmap input ${name}`);
          const file=ins.file?.value??'',fallback=x('default',[.5,.5,1],'vector3');if(!file){code=fallback;break;}if(ins.file?.nodename||ins.file?.nodegraph||ins.file?.interfacename)fail('UNSUPPORTED',key,'connected hextilednormalmap filenames are not implemented');
          const descriptor=Object.hasOwn(imageDescriptors,file)&&imageDescriptors[file];if(!descriptor)fail('RESOURCE',key,`missing decoded image ${file}`);const udim=descriptor.udim;if(udim&&(!Number.isInteger(udim.columns)||!Number.isInteger(udim.rows)||udim.columns<1||udim.rows<1))fail('RESOURCE',key,'invalid UDIM atlas descriptor');if(n.colorspace&&normalizeColorSpace(n.colorspace)!==normalizeColorSpace(descriptor.colorspace))fail('SEMANTICS',key,'hextilednormalmap colorspace differs from decoded resource');
          const address=name=>{const p=ins[name],mode=['constant','clamp','periodic','mirror'].indexOf(p?.value??'periodic');if(mode<0||p?.nodename||p?.nodegraph||p?.interfacename)fail('UNSUPPORTED',key,'invalid or connected hextilednormalmap address mode');return `${mode}u`;};const filter=ins.filtertype?.value??'linear';if(!['closest','linear'].includes(filter)||ins.filtertype?.nodename||ins.filtertype?.nodegraph||ins.filtertype?.interfacename)fail('UNSUPPORTED',key,'hextilednormalmap supports only static closest/linear filtering');
          const uv=ins.texcoord?x('texcoord',undefined,'vector2'):'ctx.uv',tiling=x('tiling',[1,1],'vector2'),coord=`(${uv}*${tiling})`,ddx=`(ctx.uvDx*${tiling})`,ddy=`(ctx.uvDy*${tiling})`,normal=ins.normal?x('normal',undefined,'vector3'):'ctx.normal',tangent=ins.tangent?x('tangent',undefined,'vector3'):'ctx.tangent',bitangent=ins.bitangent?x('bitangent',undefined,'vector3'):'ctx.bitangent',fill=`vec4f(${fallback},0.0)`,hexNormalArgs=`${descriptor.offset}u,vec2u(${descriptor.width}u,${descriptor.height}u),${descriptor.levels}u,${coord},${ddx},${ddy},${x('rotation',1,'float')},${x('rotationrange',[0,360],'vector2')},${x('scale',1,'float')},${x('scalerange',[.5,2],'vector2')},${x('offset',1,'float')},${x('offsetrange',[0,1],'vector2')},${x('falloff',.5,'float')},${x('strength',1,'float')},${x('flip_g',false,'boolean')},${normal},${tangent},${bitangent}`;
          code=udim?`imageHextileNormalUDIM(${hexNormalArgs},vec2u(${udim.columns}u,${udim.rows}u),${filter==='linear'},${fill})`:`imageHextileNormal(${hexNormalArgs},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${filter==='linear'},${fill})`;break;
        }
        case 'image': case 'tiledimage': case 'gltf_image': case 'UsdUVTexture': case 'usduvtexture': {
          const usdTexture = n.category === 'UsdUVTexture' || n.category === 'usduvtexture';
          const gltfTexture = n.category === 'gltf_image';
          if (!['float', 'color3', 'color4', 'vector2', 'vector3', 'vector4'].includes(type)) fail('TYPE', key, 'invalid image output type');
          const allowedInputs = ['file', 'default', 'texcoord', 'uaddressmode', 'vaddressmode', 'filtertype', 'layer', 'framerange', 'frameoffset', 'frameendaction', 'uvtiling', 'uvoffset', 'realworldimagesize', 'realworldtilesize'];
          if (gltfTexture) allowedInputs.push('factor', 'pivot', 'scale', 'rotate', 'offset', 'operationorder');
          if (usdTexture) allowedInputs.push('st', 'fallback', 'scale', 'bias', 'sourceColorSpace', 'wrapS', 'wrapT');
          for (const name of Object.keys(ins)) if (!allowedInputs.includes(name)) fail('UNSUPPORTED', key, `unsupported image input ${name}`);
          let realScale='vec2f(1.0)';
          if (ins.realworldimagesize || ins.realworldtilesize) {
            const imageSize=ins.realworldimagesize, tileSize=ins.realworldtilesize;
            const vector=p=>Array.isArray(p?.value)?p.value:typeof p?.value==='string'?p.value.split(',').map(Number):null;
            const imageValue=vector(imageSize), tileValue=vector(tileSize);
            if (!imageSize || !tileSize || imageSize.nodename || imageSize.nodegraph || imageSize.interfacename || tileSize.nodename || tileSize.nodegraph || tileSize.interfacename || !imageValue || !tileValue || imageValue.length!==2 || tileValue.length!==2) fail('UNSUPPORTED', key, 'tiled image real-world sizes require paired static vector2 values');
            if (imageValue.some(v=>typeof v!=='number'||!Number.isFinite(v)||v<=0) || tileValue.some(v=>typeof v!=='number'||!Number.isFinite(v)||v<=0)) fail('SEMANTICS', key, 'tiled image real-world sizes must be positive');
            const format=v=>Number.isInteger(v)?`${v}.0`:String(v); realScale=`vec2f(${format(imageValue[0]/tileValue[0])},${format(imageValue[1]/tileValue[1])})`;
          }
          const file = ins.file?.value ?? '';
          if (ins.file && (ins.file.nodename || ins.file.nodegraph || ins.file.interfacename)) fail('UNSUPPORTED', key, 'connected image filenames are not implemented');
          const fallback4 = usdTexture ? x('fallback', [0, 0, 0, 0], 'color4') : null;
          const fallback = usdTexture ? (type === 'float' ? `${fallback4}.r` : type === 'color3' || type === 'vector3' ? `${fallback4}.rgb` : type === 'color4' || type === 'vector4' ? fallback4 : `${fallback4}.r`) : x('default', widths[type] === 1 ? 0 : Array(widths[type]).fill(0), type);
          if (!file) { code = fallback; break; }
          const descriptor = Object.hasOwn(imageDescriptors, file) && imageDescriptors[file];
          if (!descriptor) fail('RESOURCE', key, `missing decoded image ${file}`);
          const frames=descriptor.frames;
          const layers=descriptor.layers;
          const authored = p => p && (p.value !== undefined || p.nodename || p.nodegraph || p.interfacename);
          if (frames && layers) fail('RESOURCE', key, 'image resource cannot combine layers and frames');
          if (authored(ins.layer) && !layers) fail('RESOURCE', key, 'image layer selection requires a decoded layered image');
          if (authored(ins.layer) && ins.layer.value !== undefined && !ins.layer.nodename && !ins.layer.nodegraph && !ins.layer.interfacename && !Number.isInteger(Number(ins.layer.value))) fail('SEMANTICS', key, 'image layer must be an integer');
          if (!frames && (authored(ins.framerange) || authored(ins.frameoffset) || authored(ins.frameendaction))) {
            for (const name of ['framerange','frameoffset']) if (authored(ins[name]) && (ins[name].nodename || ins[name].nodegraph || ins[name].interfacename || !['', '0', 0].includes(ins[name].value))) fail('UNSUPPORTED', key, `image ${name} requires decoded sequence frames`);
            if (authored(ins.frameendaction) && (ins.frameendaction.nodename || ins.frameendaction.nodegraph || ins.frameendaction.interfacename || ins.frameendaction.value !== undefined && ins.frameendaction.value !== 'constant')) fail('UNSUPPORTED', key, 'image frameendaction requires decoded sequence frames');
          }
          if (frames && frames.length > 0 && ins.frameendaction && (ins.frameendaction.nodename || ins.frameendaction.nodegraph || ins.frameendaction.interfacename || !['constant','cycle','mirror'].includes(ins.frameendaction.value ?? 'constant'))) fail('UNSUPPORTED', key, 'image frameendaction must be constant, cycle, or mirror');
          if (n.colorspace && normalizeColorSpace(n.colorspace) !== normalizeColorSpace(descriptor.colorspace)) fail('SEMANTICS', key, 'image colorspace differs from decoded resource');
          if (usdTexture && ins.sourceColorSpace) {
            if (ins.sourceColorSpace.nodename || ins.sourceColorSpace.nodegraph || ins.sourceColorSpace.interfacename) fail('UNSUPPORTED', key, 'connected sourceColorSpace is not supported');
            let sourceSpace;
            try { sourceSpace = normalizeColorSpace(ins.sourceColorSpace.value); } catch (e) { fail('SEMANTICS', key, e.message); }
            if (sourceSpace !== normalizeColorSpace(descriptor.colorspace)) fail('SEMANTICS', key, 'UsdUVTexture sourceColorSpace differs from decoded resource');
          }
          const address = name => {
            const p = ins[name] || (usdTexture && name === 'uaddressmode' ? ins.wrapS : usdTexture && name === 'vaddressmode' ? ins.wrapT : undefined); const mode = ['constant', 'clamp', 'periodic', 'mirror'].indexOf(p?.value ?? 'periodic');
            if (mode < 0 || p?.nodename || p?.interfacename || p?.nodegraph) fail('UNSUPPORTED', key, 'invalid or connected image address mode');
            return `${mode}u`;
          };
          const filter = ins.filtertype?.value ?? 'linear';
          if (!['closest', 'linear', 'cubic'].includes(filter) || ins.filtertype?.nodename || ins.filtertype?.interfacename || ins.filtertype?.nodegraph) fail('UNSUPPORTED', key, 'only static closest/linear/cubic image filters are implemented');
          const uvBase = usdTexture ? (ins.st ? x('st', undefined, 'vector2') : 'ctx.uv') : (ins.texcoord ? x('texcoord', undefined, 'vector2') : 'ctx.uv');
          const uvGltf = gltfTexture ? `((mat2x2f(cos(-${x('rotate',0,'float')}*0.017453292519943295),sin(-${x('rotate',0,'float')}*0.017453292519943295),-sin(-${x('rotate',0,'float')}*0.017453292519943295),cos(-${x('rotate',0,'float')}*0.017453292519943295)) * ((${uvBase}-${x('pivot',[0,1],'vector2')})*${x('scale',[1,1],'vector2')}))+${x('pivot',[0,1],'vector2')}+vec2f(${x('offset',[0,0],'vector2')}.x,-${x('offset',[0,0],'vector2')}.y))` : uvBase;
          const uvScale = ins.uvtiling || realScale !== 'vec2f(1.0)' ? `(${ins.uvtiling ? x('uvtiling',[1,1],'vector2') : 'vec2f(1.0)'}*${realScale})` : 'vec2f(1.0)';
          const uvTiled = `(${uvGltf}*${uvScale})`;
          const uv = ins.uvoffset ? `(${uvTiled}-${x('uvoffset',[0,0],'vector2')})` : uvTiled;
          const fill = usdTexture ? fallback4 : widths[type] === 4 ? fallback : widths[type] === 3 ? `vec4f(${fallback},0)` : widths[type] === 2 ? `vec4f(${fallback},0,0)` : `vec4f(${fallback})`;
          const swizzle = ({ float: 'r', vector2: 'rg', vector3: 'rgb', color3: 'rgb', vector4: 'rgba', color4: 'rgba' })[type];
          const size = `vec2f(${descriptor.width}.0,${descriptor.height}.0)`;
          const lodScale = gltfTexture ? `(${uvScale}*abs(${x('scale',[1,1],'vector2')}))` : uvScale;
          const lod = `log2(max(1.0,max(length(ctx.uvDx*${size}*${lodScale}),length(ctx.uvDy*${size}*${lodScale}))))`;
          const udim = descriptor.udim;
          if (udim && (!Number.isInteger(udim.columns) || !Number.isInteger(udim.rows) || udim.columns < 1 || udim.rows < 1)) fail('RESOURCE', key, 'invalid UDIM atlas descriptor');
          const grid = udim && `vec2u(${udim.columns}u,${udim.rows}u)`;
          const sampleFor = d => filter === 'cubic'
            ? (udim ? `imageSampleCubicUDIM(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv},${grid},${lod},${fill})` : `imageSampleCubic(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv},${lod},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${fill})`)
            : (udim ? `imageSampleUDIM(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv},${grid},${lod},${filter === 'linear'},${fill})` : `imageSample(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv},${lod},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${filter === 'linear'},${fill})`);
          let sample=sampleFor(descriptor);
          if (layers) {
            const layerInput=authored(ins.layer)?x('layer',0,'integer'):'0i';
            const layerIndex=`u32(clamp(${layerInput},0i,${layers.length-1}i))`;
            sample=layers.map(layer=>sampleFor(layer)).reduce((value,layer,index)=>index?`select(${value},${layer},${layerIndex}==${index}u)`:layer);
          }
          if(frames){
            const range=ins.framerange ? x('framerange',[0,frames.length-1],'vector2') : `vec2f(0.0,${frames.length-1}.0)`;
            const frameOffset=ins.frameoffset ? x('frameoffset',0,'float') : '0.0';
            const action=['constant','cycle','mirror'].indexOf(ins.frameendaction?.value ?? 'constant');
            const frameIndex=`imageSequenceIndex(ctx.frame,${range}.x,${range}.y,${frameOffset},${frames.length}u,${action}u)`;
            sample=frames.map((frame,index)=>`${sampleFor(frame)}`).reduce((value,frame,index)=>index?`select(${value},${frame},${frameIndex}==${index}u)`:frame);
          }
          if (usdTexture) {
            const scale = x('scale', [1, 1, 1, 1], 'color4'), bias = x('bias', [0, 0, 0, 0], 'color4');
            code = `((${sample}*${scale}+${bias})).${swizzle}`;
          } else code = `${sample}.${swizzle}`;
          if (gltfTexture) code = `(${code}*${x('factor', widths[type] === 1 ? 1 : Array(widths[type]).fill(1), type)})`;
          break;
        }
        case 'latlongimage': {
          if (type !== 'color3') fail('TYPE', key, 'latlongimage output must be color3');
          for (const name of Object.keys(ins)) if (!['file','default','viewdir','rotation','layer'].includes(name)) fail('UNSUPPORTED', key, `unsupported latlongimage input ${name}`);
          const file=ins.file?.value??''; const fallback=x('default',[0,0,0],'color3');
          if (!file) { code=fallback; break; }
          if (ins.file?.nodename||ins.file?.nodegraph||ins.file?.interfacename) fail('UNSUPPORTED',key,'connected latlongimage filenames are not implemented');
          const descriptor=Object.hasOwn(imageDescriptors,file)&&imageDescriptors[file]; if(!descriptor) fail('RESOURCE',key,`missing decoded image ${file}`);
          const layers=descriptor.layers;
          const authored = p => p && (p.value !== undefined || p.nodename || p.nodegraph || p.interfacename);
          if (authored(ins.layer) && !layers) fail('RESOURCE', key, 'latlongimage layer selection requires a decoded layered image');
          if (authored(ins.layer) && ins.layer.value !== undefined && !ins.layer.nodename && !ins.layer.nodegraph && !ins.layer.interfacename && !Number.isInteger(Number(ins.layer.value))) fail('SEMANTICS', key, 'latlongimage layer must be an integer');
          if(n.colorspace&&normalizeColorSpace(n.colorspace)!==normalizeColorSpace(descriptor.colorspace)) fail('SEMANTICS',key,'latlongimage colorspace differs from decoded resource');
          const direction=`safeNormal(${x('viewdir',[0,0,1],'vector3')},vec3f(0.0,0.0,1.0))`, rotation=`(${x('rotation',0,'float')}*0.017453292519943295)`, pi='3.141592653589793';
          const uv=`vec2f(fract(atan2(${direction}.z,${direction}.x)/(2.0*${pi})+0.5+${rotation}/(2.0*${pi})),acos(clamp(${direction}.y,-1.0,1.0))/${pi})`;
          const lod=`log2(max(1.0,max(length(ctx.uvDx*vec2f(${descriptor.width}.0,${descriptor.height}.0)),length(ctx.uvDy*vec2f(${descriptor.width}.0,${descriptor.height}.0)))))`;
          const sampleFor = d => (d.udim ? `imageSampleUDIM(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv},vec2u(${d.udim.columns}u,${d.udim.rows}u),${lod},true,vec4f(${fallback},0))` : `imageSample(${d.offset}u,vec2u(${d.width}u,${d.height}u),${d.levels}u,${uv},${lod},vec2u(2u,1u),true,vec4f(${fallback},0))`);
          let sample = sampleFor(descriptor);
          if (layers) {
            const layerInput = authored(ins.layer) ? x('layer', 0, 'integer') : '0i';
            const layerIndex = `u32(clamp(${layerInput},0i,${layers.length - 1}i))`;
            sample = layers.map(layer => sampleFor(layer)).reduce((value, layer, index) => index ? `select(${value},${layer},${layerIndex}==${index}u)` : layer);
          }
          code=`${sample}.rgb`; break;
        }
        case 'splitlr': case 'splittb': {
          const uv=x('texcoord',[0,0],'vector2'), center=x('center',.5,'float');
          if (n.category==='splitlr') code=`select(${x('valuer',widths[type]===1?0:Array(widths[type]).fill(0),type)},${x('valuel',widths[type]===1?0:Array(widths[type]).fill(0),type)},${uv}.x<${center})`;
          else code=`select(${x('valueb',widths[type]===1?0:Array(widths[type]).fill(0),type)},${x('valuet',widths[type]===1?0:Array(widths[type]).fill(0),type)},${uv}.y>=${center})`;
          break;
        }
        case 'ramp_gradient': {
          if (type !== 'color4') fail('TYPE', key, 'ramp_gradient output must be color4');
          const q=x('x',0,'float'), lo=x('interval1',0,'float'), hi=x('interval2',1,'float'), a=x('color1',[0,0,0,1],'color4'), b=x('color2',[1,1,1,1],'color4'), mode=x('interpolation',1,'integer');
          const u=`clamp((${q}-${lo})/max(${hi}-${lo},1e-6),0.0,1.0)`, smooth=`smoothstep(0.0,1.0,${u})`, mixValue=`mix(${a},${b},select(select(${u},${smooth},${mode}==1i),select(0.0,1.0,${u}>=1.0),${mode}==2i))`;
          code=mixValue; break;
        }
        case 'ramp': {
          if (type !== 'color4') fail('TYPE', key, 'ramp output must be color4');
          const uv=x('texcoord',[0,0],'vector2'), shape=x('type',0,'integer'), interpolation=x('interpolation',1,'integer');
          const t=`select(select(select(${uv}.x,length(${uv}-vec2f(.5)),${shape}==1i),fract(atan2(${uv}.y-.5,${uv}.x-.5)/(2.0*3.141592653589793)+.5),${shape}==2i),max(abs(${uv}.x-.5),abs(${uv}.y-.5))*2.0,${shape}==3i)`;
          const countPort=ins.num_intervals; if(countPort&&(countPort.nodename||countPort.nodegraph||countPort.interfacename)) fail('UNSUPPORTED',key,'ramp num_intervals must be static');
          const count=Math.max(2,Math.min(10,Number(countPort?.value??2))); if(!Number.isInteger(count)) fail('VALUE',key,'ramp num_intervals must be an integer');
          const blend=(i)=>{const lo=x(`interval${i}`,i===1?0:1,'float'),hi=x(`interval${i+1}`,1,'float'),a=x(`color${i}`,[0,0,0,1],'color4'),b=x(`color${i+1}`,[1,1,1,1],'color4'),u=`clamp((${t}-${lo})/max(${hi}-${lo},1e-6),0.0,1.0)`,smooth=`smoothstep(0.0,1.0,${u})`;return `mix(${a},${b},select(select(${u},${smooth},${interpolation}==1i),select(0.0,1.0,${u}>=1.0),${interpolation}==2i))`;};
          let result=x(`color${count}`,[1,1,1,1],'color4'); for(let i=count-1;i>=1;i--) result=`select(${result},${blend(i)},${t}<${x(`interval${i+1}`,1,'float')})`; code=result; break;
        }
        case 'ramplr': case 'ramptb': {
          const uv=x('texcoord',[0,0],'vector2'), amount=n.category==='ramplr'?`${uv}.x`:`${uv}.y`, first=n.category==='ramplr'?x('valuel',widths[type]===1?0:Array(widths[type]).fill(0),type):x('valueb',widths[type]===1?0:Array(widths[type]).fill(0),type), second=n.category==='ramplr'?x('valuer',widths[type]===1?0:Array(widths[type]).fill(0),type):x('valuet',widths[type]===1?0:Array(widths[type]).fill(0),type);
          code=`mix(${first},${second},${amount})`; break;
        }
        case 'checkerboard': {
          if (type !== 'color3') fail('TYPE', key, 'checkerboard output must be color3');
          const uv=x('texcoord',[0,0],'vector2'), tiling=x('uvtiling',[8,8],'vector2'), offset=x('uvoffset',[0,0],'vector2'), c1=x('color1',[1,1,1],'color3'), c2=x('color2',[0,0,0],'color3');
          const point=`(${uv}*${tiling}+${offset})`, even=`fract((floor(${point}.x)+floor(${point}.y))*0.5)<0.5`;
          code=`select(${c2},${c1},${even})`; break;
        }
        case 'circle': {
          const uv=x('texcoord',[0,0],'vector2'), center=x('center',[0,0],'vector2'), radius=x('radius',.5,'float');
          code=`select(0.0,1.0,distance(${uv},${center})<=max(0.0,${radius}))`; break;
        }
        case 'line': {
          const uv=x('texcoord',[0,0],'vector2'), center=x('center',[0,0],'vector2'), p1=x('point1',[.25,.25],'vector2'), p2=x('point2',[.75,.75],'vector2'), radius=x('radius',.1,'float');
          const a=`(${uv}-(${p1}+${center}))`, b=`(${p2}-${p1})`, h=`clamp(dot(${a},${b})/max(dot(${b},${b}),1e-6),0.0,1.0)`, d=`length(${a}-${b}*${h})`;
          code=`select(0.0,1.0,${d}<=max(0.0,${radius}))`; break;
        }
        case 'grid': case 'crosshatch': {
          if (type !== 'color3') fail('TYPE', key, `${n.category} output must be color3`);
          const uv=x('texcoord',[0,0],'vector2'), tiling=x('uvtiling',[1,1],'vector2'), offset=x('uvoffset',[0,0],'vector2'), thickness=x('thickness',.05,'float'), staggered=x('staggered',false,'boolean');
          const p=`(${uv}*${tiling}+${offset})`, staggeredP=`vec2f(${p}.x,${p}.y+select(0.0,0.5,${staggered}&&fract(floor(${p}.x)*0.5)>0.0))`, f=`fract(${staggeredP})`, edge=`min(min(${f}.x,1.0-${f}.x),min(${f}.y,1.0-${f}.y))`, base=`select(0.0,1.0,${edge}<max(0.0,${thickness})*.5)`;
          const cross=`select(0.0,1.0,min(abs(fract(${staggeredP}.x+${staggeredP}.y)-.5),abs(fract(${staggeredP}.x-${staggeredP}.y)-.5))<max(0.0,${thickness})*.5)`;
          code=`vec3f(${n.category==='grid'?base:`max(${base},${cross})`})`; break;
        }
        case 'tiledcircles': {
          if (type !== 'color3') fail('TYPE', key, 'tiledcircles output must be color3');
          const uv=x('texcoord',[0,0],'vector2'), tiling=x('uvtiling',[1,1],'vector2'), offset=x('uvoffset',[0,0],'vector2'), size=x('size',.5,'float'), staggered=x('staggered',false,'boolean');
          const p=`(${uv}*${tiling}+${offset})`, q=`vec2f(${p}.x,${p}.y+select(0.0,0.5,${staggered}&&fract(floor(${p}.x)*0.5)>0.0))`, local=`(fract(${q})-0.5)`, mask=`select(0.0,1.0,length(${local})<=clamp(${size},0.0,1.0)*0.5)`;
          code=`vec3f(${mask})`; break;
        }
        case 'randomfloat': {
          if (type !== 'float') fail('TYPE', key, 'randomfloat output must be float');
          const value=input('in',0), valueCode=value.type==='integer'?`f32(${value.code})`:value.code, seed=x('seed',0,'integer'), lo=x('min',0,'float'), hi=x('max',1,'float');
          code=`mix(${lo},${hi},mxHash2(vec2f(${valueCode},f32(${seed}))))`; break;
        }
        case 'randomcolor': {
          if (type !== 'color3') fail('TYPE', key, 'randomcolor output must be color3');
          const value=input('in',0), valueCode=value.type==='integer'?`f32(${value.code})`:value.code, seed=x('seed',0,'integer');
          const base=`vec3f(${valueCode},f32(${seed}),17.0)`, h=`mxHash3(${base})`, s=`mxHash3(${base}+vec3f(31.0,7.0,13.0))`, v=`mxHash3(${base}+vec3f(73.0,19.0,43.0))`;
          const hue=`mix(${x('huelow',0,'float')},${x('huehigh',1,'float')},${h})`, sat=`mix(${x('saturationlow',.825,'float')},${x('saturationhigh',1,'float')},${s})`, brightness=`mix(${x('brightnesslow',1,'float')},${x('brightnesshigh',1,'float')},${v})`;
          code=`mxHsvToRgb(vec3f(fract(${hue}),clamp(${sat},0.0,1.0),max(0.0,${brightness})))`; break;
        }
        case 'unifiednoise2d': case 'unifiednoise3d': {
          if (type !== 'float') fail('TYPE', key, `${n.category} output must be float`);
          const is2=n.category==='unifiednoise2d', coordinate=x(is2?'texcoord':'position',is2?[0,0]:[0,0,0],is2?'vector2':'vector3'), frequency=x('freq',is2?[1,1]:[1,1,1],is2?'vector2':'vector3'), offset=x('offset',is2?[0,0]:[0,0,0],is2?'vector2':'vector3'), jitter=x('jitter',1,'float'), noiseType=x('type',0,'integer'), style=x('style',0,'integer'), octaves=x('octaves',3,'integer'), lacunarity=x('lacunarity',2,'float'), diminish=x('diminish',.5,'float');
          const p=`(${coordinate}*${frequency}+${offset})`, perlin=is2?`mxNoise2(${p})`:`mxNoise3(${p})`, cell=is2?`mxHash2(floor(${p}))`:`mxHash3(floor(${p}))`, worley=is2?`mxWorley2(${p},${jitter},${style}).x`:`mxWorley3(${p},${jitter},${style}).x`, fractal=is2?`mxFractal2(${p},${octaves},${lacunarity},${diminish})`:`mxFractal3(${p},${octaves},${lacunarity},${diminish})`;
          const raw=`select(select(select(${perlin},${cell},${noiseType}==1i),${worley},${noiseType}==2i),${fractal},${noiseType}==3i)`, lo=x('outmin',0,'float'), hi=x('outmax',1,'float'), mapped=`(${lo}+${raw}*(${hi}-${lo}))`;
          code=`select(${mapped},clamp(${mapped},min(${lo},${hi}),max(${lo},${hi})),${x('clampoutput',true,'boolean')})`; break;
        }
        case 'constant': code = same('value'); break;
        case 'blur': {
          if (!['float','color3','color4'].includes(type)) fail('TYPE', key, 'blur supports float, color3, and color4 overloads');
          const filter=ins.filtertype?.value??'box';
          if (!['box','gaussian'].includes(filter) || ins.filtertype?.nodename || ins.filtertype?.nodegraph || ins.filtertype?.interfacename) fail('UNSUPPORTED',key,'blur filtertype must be a static box or gaussian value');
          x('size',0,'float');
          code=x('in',widths[type]===1?0:Array(widths[type]).fill(0),type); break;
        }
        case 'add': {
          if(type==='BSDF') {const a=input('in1',undefined,'BSDF'),b=input('in2',undefined,'BSDF');if(a.hasInterior&&b.hasInterior)fail('SEMANTICS',key,'BSDF add cannot combine two interior-bearing closures');code=`${a.hasInterior||b.hasInterior?'closureAddPreservingInterior':'closureAdd'}(${a.code},${b.code})`;closureCount=(a.closureCount||0)+(b.closureCount||0);hasInterior=a.hasInterior||b.hasInterior;interiorCategories=a.hasInterior?a.interiorCategories:b.interiorCategories;}
          else code = binary('+'); break;
        }
        case 'subtract': code = binary('-'); break;
        case 'plus': case 'minus': {
          if (!['float','color3','color4'].includes(type)) fail('TYPE',key,'compositing requires float/color3/color4');
          for (const name of Object.keys(ins)) if (!['fg','bg','mix'].includes(name)) fail('INPUT',key,`unsupported compositing input ${name}`);
          const fallback=type==='float'?0:Array(widths[type]).fill(0), fg=x('fg',fallback,type), bg=x('bg',fallback,type), amount=x('mix',1,'float');
          code=`mix(${bg},(${bg}${n.category==='plus'?'+':'-'}${fg}),${amount})`; break;
        }
        case 'difference': code = `abs(${same('in1')}-${scalarOrSame('in2')})`; break;
        case 'screen': {
          const one=type==='float'?'1.0':`${types[type]}(1.0)`; code=`(${one}-(${one}-${same('in1')})*(${one}-${scalarOrSame('in2')}))`; break;
        }
        case 'burn': case 'dodge': case 'overlay': {
          if (!['float','color3','color4'].includes(type)) fail('TYPE', key, `${n.category} requires float/color3/color4`);
          for (const name of Object.keys(ins)) if (!['fg','bg','mix'].includes(name)) fail('INPUT', key, `unsupported ${n.category} input`);
          const fg=x('fg',type==='float'?0:Array(widths[type]).fill(0),type), bg=x('bg',type==='float'?0:Array(widths[type]).fill(0),type), amount=x('mix',1,'float');
          const one=type==='float'?'1.0':`${types[type]}(1.0)`;
          const raw=n.category==='burn'?`(${one}-(${one}-${bg})/max(${fg},${types[type]}(1e-6)))`:n.category==='dodge'?`${bg}/max(${one}-${fg},${types[type]}(1e-6))`:`select(2.0*${fg}*${bg},${one}-2.0*(${one}-${fg})*(${one}-${bg}),${bg}>=${types[type]}(.5))`;
          code=`mix(${bg},${raw},${amount})`; break;
        }
        case 'disjointover': {
          if (type !== 'color4') fail('TYPE', key, 'disjointover requires color4');
          for (const name of Object.keys(ins)) if (!['fg','bg','mix'].includes(name)) fail('INPUT', key, 'unsupported disjointover input');
          const fg=x('fg',[0,0,0,0],'color4'), bg=x('bg',[0,0,0,0],'color4'), amount=x('mix',1,'float'), alpha=`min(${fg}.a+${bg}.a,1.0)`, rgb=`select(${fg}.rgb+${bg}.rgb,${fg}.rgb+${bg}.rgb*(1.0-${fg}.a)/max(${bg}.a,1e-6),${fg}.a+${bg}.a>1.0)`;
          code=`mix(${bg},vec4f(${rgb},${alpha}),${amount})`; break;
        }
        case 'in': case 'mask': case 'matte': case 'out': case 'over': {
          if (type !== 'color4') fail('TYPE', key, `${n.category} requires color4`);
          for (const name of Object.keys(ins)) if (!['fg','bg','mix'].includes(name)) fail('INPUT', key, `unsupported ${n.category} input`);
          const fg=x('fg',[0,0,0,0],'color4'), bg=x('bg',[0,0,0,0],'color4'), amount=x('mix',1,'float');
          const alpha = n.category==='in' ? `${fg}.a*${bg}.a` : n.category==='mask' ? `${bg}.a*${fg}.a` : n.category==='matte' ? `${fg}.a+${bg}.a*(1.0-${fg}.a)` : n.category==='out' ? `${fg}.a*(1.0-${bg}.a)` : `${fg}.a+${bg}.a*(1.0-${fg}.a)`;
          const rgb = n.category==='in' ? `${fg}.rgb*${bg}.a` : n.category==='mask' ? `${bg}.rgb*${fg}.a` : n.category==='matte' ? `${fg}.rgb*${fg}.a+${bg}.rgb*(1.0-${fg}.a)` : n.category==='out' ? `${fg}.rgb*(1.0-${bg}.a)` : `${fg}.rgb+${bg}.rgb*(1.0-${fg}.a)`;
          code=`mix(${bg},vec4f(${rgb},${alpha}),${amount})`; break;
        }
        case 'inside': case 'outside': {
          if (!['float','color3','color4'].includes(type)) fail('TYPE', key, `${n.category} requires float/color3/color4`);
          for (const name of Object.keys(ins)) if (!['in','mask'].includes(name)) fail('INPUT', key, `unsupported ${n.category} input`);
          const value=x('in',type==='float'?0:Array(widths[type]).fill(0),type), mask=x('mask',n.category==='inside'?1:0,'float');
          code=`${value}*${types[type]}(${n.category==='inside'?mask:`(1.0-${mask})`})`; break;
        }
        case 'not': if (type !== 'boolean') fail('TYPE', key, 'not requires boolean input'); code=`!${x('in',false,'boolean')}`; break;
        case 'and': case 'or': case 'xor': {
          if (type !== 'boolean') fail('TYPE', key, `${n.category} requires boolean inputs`);
          const a=x('in1',false,'boolean'), b=x('in2',false,'boolean'); code=n.category==='and'?`(${a}&&${b})`:n.category==='or'?`(${a}||${b})`:`(${a}!=${b})`; break;
        }
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
        case 'safepower': { const base=same('in1'), exponent=scalarOrSame('in2'); code=`(sign(${base})*pow(abs(${base}),${exponent}))`; break; }
        case 'absval': case 'sign': case 'floor': case 'ceil': case 'round': case 'fract': case 'sqrt': case 'ln': case 'exp': case 'exp2': case 'log2': case 'sin': case 'cos': case 'tan': case 'asin': case 'acos': case 'atan': case 'sinh': case 'cosh': case 'tanh': case 'asinh': case 'acosh': case 'atanh': code = `${({ absval: 'abs', ln: 'log' })[n.category] || n.category}(${same('in')})`; break;
        case 'exp10': code=`pow(${types[type]}(10.0),${same('in')})`; break;
        case 'normalize': {
          const value = x('in', undefined, type), fallback = widths[type] === 2 ? 'vec2f(0.0,1.0)' : widths[type] === 3 ? 'vec3f(0.0,0.0,1.0)' : 'vec4f(0.0,0.0,0.0,1.0)';
          if (![2, 3, 4].includes(widths[type])) fail('TYPE', key, 'normalize requires a vector input');
          code = `mxSafeNormalize${widths[type]}(${value},${fallback})`; break;
        }
        case 'log10': code=`log(${same('in')})*0.4342944819032518`; break;
        case 'radians': code=`${same('in')}*0.017453292519943295`; break;
        case 'degrees': code=`${same('in')}*57.29577951308232`; break;
        case 'atan2': code = `atan2(${same('iny')},${same('inx')})`; break;
        case 'clamp': { const fallback=v=>widths[type]===1?v:Array(widths[type]).fill(v); code=`clamp(${same('in')},${scalarOrSame('low',fallback(0))},${scalarOrSame('high',fallback(1))})`; break; }
        case 'mix': {
          if(type==='BSDF'){const a=input('bg',undefined,'BSDF'),b=input('fg',undefined,'BSDF');if(a.hasInterior&&b.hasInterior)fail('SEMANTICS',key,'BSDF mix cannot combine two interior-bearing closures');code=`${a.hasInterior||b.hasInterior?'closureMixPreservingInterior':'closureMix'}(${a.code},${b.code},clamp(${x('mix',0,'float')},0.0,1.0))`;closureCount=(a.closureCount||0)+(b.closureCount||0);hasInterior=a.hasInterior||b.hasInterior;interiorCategories=a.hasInterior?a.interiorCategories:b.interiorCategories;}
          else code = `mix(${same('bg')},${same('fg')},${x('mix',0,'float')})`; break;
        }
        case 'smoothstep': { const fallback=v=>widths[type]===1?v:Array(widths[type]).fill(v); code=`smoothstep(${scalarOrSame('low',fallback(0))},${scalarOrSame('high',fallback(1))},${same('in')})`; break; }
        case 'invert': { const fallback=widths[type]===1?1:Array(widths[type]).fill(1); code=`(${scalarOrSame('amount',fallback)} - ${same('in')})`; break; }
        case 'select': {
          const condition=x('condition',undefined,'boolean'), whenTrue=input('truevalue'), whenFalse=input('falsevalue');
          if(whenTrue.type!==whenFalse.type||whenTrue.type!==type)fail('TYPE',key,'select branches must match output type');
          code=`select(${whenFalse.code},${whenTrue.code},${condition})`; break;
        }
        case 'switch': {
          if (type === 'BSDF' || type === 'EDF' || type === 'VDF') fail('TYPE', key, 'switch requires a value type');
          const fallback=widths[type]===1?0:Array(widths[type]).fill(0), which=x('which',0,'float');
          let selected=x('in10',fallback,type);
          for (let i=9;i>=1;i--) selected=`select(${x(`in${i}`,fallback,type)},${selected},${which}>=${i}.0)`;
          code=selected; break;
        }
        case 'noise2d': case 'noise3d': {
          const dimension = n.category === 'noise2d' ? 'vector2' : 'vector3';
          for (const name of Object.keys(ins)) if (!['in', 'scale', 'amplitude', 'pivot', 'octaves', 'lacunarity', 'diminish'].includes(name)) fail('UNSUPPORTED', key, `unsupported ${n.category} input ${name}`);
          for (const name of ['octaves', 'lacunarity', 'diminish']) {
            const p = ins[name];
            if (p && (p.nodename || p.nodegraph || p.interfacename || (name === 'octaves' ? Number(p.value) !== 1 : Number(p.value) !== (name === 'lacunarity' ? 2 : 0.5)))) fail('UNSUPPORTED', key, `${n.category} ${name} is only supported at its single-octave default`);
          }
          const coordinate = x('in', undefined, dimension), scale = x('scale', 1, 'float');
          const amplitude = x('amplitude', 1, 'float'), pivot = x('pivot', 0.5, 'float');
          const sample = n.category === 'noise2d' ? `mxNoise2(${coordinate}*${scale})` : `mxNoise3(${coordinate}*${scale})`;
          code = `((${sample}-0.5)*${amplitude}+${pivot})`; break;
        }
        case 'fractal2d': case 'fractal3d': {
          const dimension=n.category==='fractal2d'?'vector2':'vector3', coordinate=x('texcoord',dimension==='vector2'?[0,0]:[0,0,0],dimension), octaves=x('octaves',3,'integer'), lacunarity=x('lacunarity',2,'float'), diminish=x('diminish',.5,'float');
          const amp=input('amplitude',widths[type]===1?1:Array(widths[type]).fill(1),type), ampCode=amp.type==='float'&&widths[type]>1?`${types[type]}(${amp.code})`:amp.code;
          const sample=n.category==='fractal2d'?`mxFractal2(${coordinate},${octaves},${lacunarity},${diminish})`:`mxFractal3(${coordinate},${octaves},${lacunarity},${diminish})`;
          code=`(${sample}*${ampCode})`; break;
        }
        case 'worleynoise2d': case 'worleynoise3d': {
          if (!['float','vector2','vector3'].includes(type)) fail('TYPE', key, `${n.category} supports float/vector2/vector3 outputs`);
          const is2=n.category==='worleynoise2d', coordinate=x(is2?'texcoord':'position',is2?[0,0]:[0,0,0],is2?'vector2':'vector3'), jitter=x('jitter',1,'float'), style=x('style',0,'integer');
          const value=is2?`mxWorley2(${coordinate},${jitter},${style})`:`mxWorley3(${coordinate},${jitter},${style})`;
          code=type==='float'?`${value}.x`:type==='vector2'?`${value}.xy`:value; break;
        }
        case 'cellnoise2d': code=`mxHash2(floor(${x('in',undefined,'vector2')}))`; break;
        case 'cellnoise3d': code=`mxHash3(floor(${x('in',undefined,'vector3')}))`; break;
        case 'dot': result = input('in',undefined,type); break;
        case 'magnitude': code = `length(${x('in')})`; break;
        case 'distance': code = `distance(${x('in1')},${x('in2')})`; break;
        case 'reflect': code = `reflect(${x('in')},${x('normal',undefined,'vector3')})`; break;
        case 'refract': code = `refract(${x('in')},${x('normal',undefined,'vector3')},${x('ior',1,'float')})`; break;
        case 'fresnel': {
          const direction=x('in',undefined,'vector3'), normal=x('normal',undefined,'vector3'), ior=x('ior',1.5,'float');
          const cosine=`abs(dot(normalize(${direction}),normalize(${normal})))`;
          const f0=`pow((${ior}-1.0)/(${ior}+1.0),2.0)`;
          code=`(${f0}+(1.0-${f0})*pow(1.0-clamp(${cosine},0.0,1.0),5.0))`; break;
        }
        case 'facing_ratio': {
          const direction=x('in',undefined,'vector3'), normal=x('normal',undefined,'vector3'), exponent=x('exponent',1,'float');
          code=`pow(1.0-clamp(abs(dot(normalize(${direction}),normalize(${normal}))),0.0,1.0),max(0.0,${exponent}))`; break;
        }
        case 'facingratio': {
          if (type !== 'float') fail('TYPE', key, 'facingratio output must be float');
          const direction=ins.viewdirection?x('viewdirection',undefined,'vector3'):'ctx.viewdir', normal=ins.normal?x('normal',undefined,'vector3'):'ctx.normal';
          const faceforward=x('faceforward',true,'boolean'), invert=x('invert',false,'boolean'), dot=`dot(${direction},${normal})`, facing=`select(-${dot},abs(${dot}),${faceforward})`;
          code=`select(${facing},1.0-${facing},${invert})`; break;
        }
        case 'luminance': code=`dot(vec3f(0.2126,0.7152,0.0722),${x('in',undefined,'color3')})`; break;
        case 'average': {
          const value=input('in');
          if(!['color3','vector3','vector4','color4'].includes(value.type))fail('TYPE',key,'average requires a vector or color input');
          const width=widths[value.type]; code=`dot(${value.code},${types[value.type]}(${Array(width).fill((1/width).toFixed(10)).join(',')}))`; break;
        }
        case 'rgbtohsv': code=`mxRgbToHsv(${x('in',undefined,'color3')})`; break;
        case 'hsvtorgb': code=`mxHsvToRgb(${x('in',undefined,'color3')})`; break;
        case 'hsvadjust': {
          if (!['color3','color4'].includes(type)) fail('TYPE', key, 'hsvadjust output must be color3/color4');
          const value=input('in',undefined,type), rgb=type==='color4'?`${value.code}.rgb`:value.code;
          const hsv=`mxRgbToHsv(${rgb})`, amount=x('amount',[0,1,1],'vector3');
          const adjusted=`mxHsvToRgb(vec3f(fract(${hsv}.x+${amount}.x),max(0.0,${hsv}.y*${amount}.y),max(0.0,${hsv}.z*${amount}.z)))`;
          code=type==='color4'?`vec4f(${adjusted},${value.code}.a)`:adjusted; break;
        }
        case 'colorcorrect': {
          if (!['color3','color4'].includes(type)) fail('TYPE', key, 'colorcorrect output must be color3/color4');
          const value=x('in',type==='color4'?[1,1,1,1]:[1,1,1],type), source=type==='color4'?`${value}.rgb`:value, hue=x('hue',0,'float'), saturation=x('saturation',1,'float');
          const hsv=`mxRgbToHsv(${source})`, rgb=`mxHsvToRgb(vec3f(fract(${hsv}.x+${hue}),max(0.0,${hsv}.y),max(0.0,${hsv}.z)))`;
          const lift=x('lift',0,'float'), gain=x('gain',1,'float'), contrast=x('contrast',1,'float'), pivot=x('contrastpivot',.5,'float'), exposure=x('exposure',0,'float'), gamma=x('gamma',1,'float');
          // Match NG_colorcorrect: hue, luminance saturation, signed gamma,
          // lift, gain, contrast, exposure. Alpha bypasses all adjustments.
          const saturated=`mix(vec3f(dot(vec3f(.2126,.7152,.0722),${rgb})),${rgb},${saturation})`;
          const corrected=`(sign(${saturated})*pow(abs(${saturated}),vec3f(1.0/${gamma})))`;
          const lifted=`(${corrected}*(1.0-${lift})+vec3f(${lift}))`, contrasted=`((${lifted}*${gain}-vec3f(${pivot}))*${contrast}+vec3f(${pivot}))`;
          const resultColor=`(${contrasted}*exp2(${exposure}))`;
          code=type==='color4'?`vec4f(${resultColor},${value}.a)`:resultColor; break;
        }
        case 'blackbody': {
          if (type !== 'color3') fail('TYPE', key, 'blackbody output must be color3');
          const temperature = x('temperature',5000,'float');
          code = `mxBlackbody(${temperature})`; break;
        }
        case 'artistic_ior': {
          if (type !== 'color3' || !['ior','extinction'].includes(out)) fail('TYPE', key, 'artistic_ior requires a color3 ior/extinction output');
          const reflectivity=`clamp(${x('reflectivity',[.944,.776,.373],'color3')},vec3f(0.0),vec3f(.99))`, edge=x('edge_color',[.998,.981,.751],'color3');
          const root=`sqrt(${reflectivity})`, nmin=`((vec3f(1.0)-${reflectivity})/(vec3f(1.0)+${reflectivity}))`, nmax=`((vec3f(1.0)+${root})/(vec3f(1.0)-${root}))`, ior=`mix(${nmax},${nmin},${edge})`;
          const np1=`(${ior}+vec3f(1.0))`, nm1=`(${ior}-vec3f(1.0))`, k2=`max((${np1}*${np1}*${reflectivity}-${nm1}*${nm1})/max(vec3f(1.0)-${reflectivity},vec3f(1e-6)),vec3f(0.0))`;
          code=out==='ior'?ior:`sqrt(${k2})`; break;
        }
        case 'roughness_anisotropy': case 'glossiness_anisotropy': {
          if (type !== 'vector2') fail('TYPE', key, `${n.category} output must be vector2`);
          const source=n.category==='glossiness_anisotropy'?`(1.0-${x('glossiness',1,'float')})`:x('roughness',0,'float'), anisotropy=x('anisotropy',0,'float');
          const squared=`clamp(${source}*${source},1e-8,1.0)`, aspect=`sqrt(1.0-clamp(${anisotropy},0.0,.98))`;
          code=`select(vec2f(${squared}),vec2f(min(${squared}/max(${aspect},1e-6),1.0),${squared}*${aspect}),${anisotropy}>0.0)`; break;
        }
        case 'roughness_dual': {
          if (type !== 'vector2') fail('TYPE', key, 'roughness_dual output must be vector2');
          const roughness=x('roughness',[0,0],'vector2');
          const secondary=`select(${roughness}.y,${roughness}.x,${roughness}.y<0.0)`;
          code=`clamp(vec2f(${roughness}.x*${roughness}.x,${secondary}*${secondary}),vec2f(1e-8),vec2f(1.0))`; break;
        }
        case 'gooch_shade': {
          if (type !== 'color3') fail('TYPE', key, 'gooch_shade output must be color3');
          const warm=x('warm_color',[.8,.8,.7],'color3'), cool=x('cool_color',[.3,.3,.8],'color3');
          const specularIntensity=x('specular_intensity',1,'float'), shininess=x('shininess',64,'float');
          const light=`normalize(${x('light_direction',[1,-.5,-.5],'vector3')})`, normal='normalize(ctx.normal)', view='normalize(ctx.viewdir)';
          const coolIntensity=`(1.0+dot(${normal},${light}))/2.0`, diffuse=`mix(${warm},${cool},${coolIntensity})`;
          const viewReflect=`reflect(${view},${normal})`, highlight=`pow(max(dot(-${light},${viewReflect}),0.0),${shininess})*${specularIntensity}`;
          code=`${diffuse}+vec3f(${highlight})`; break;
        }
        case 'cloverleaf': {
          if (type !== 'float') fail('TYPE', key, 'cloverleaf output must be float');
          const uv=x('texcoord',[0,0],'vector2'), center=x('center',[0,0],'vector2'), radius=`max(0.0,${x('radius',.5,'float')})`;
          const sample=`(${uv}*2.0)`, origin=`(${center}*2.0)`;
          const circles=[`distance(${sample}+vec2f(${radius},0.0),${origin})`, `distance(${sample}-vec2f(${radius},0.0),${origin})`, `distance(${sample}+vec2f(0.0,${radius}),${origin})`, `distance(${sample}-vec2f(0.0,${radius}),${origin})`];
          code=`select(0.0,1.0,min(min(${circles[0]},${circles[1]}),min(${circles[2]},${circles[3]}))<=${radius})`; break;
        }
        case 'hexagon': {
          if (type !== 'float') fail('TYPE', key, 'hexagon output must be float');
          const uv=x('texcoord',[0,0],'vector2'), center=x('center',[0,0],'vector2'), radius=`max(0.0,${x('radius',.5,'float')})`;
          const p=`abs(${uv}-${center})`, k='vec2f(-0.8660254,0.5)', kz='0.5773503';
          const folded=`(${p}-2.0*min(dot(${k},${p}),0.0)*${k})`, shifted=`(${folded}-vec2f(clamp(${folded}.x,-${kz}*${radius},${kz}*${radius}),${radius}))`;
          const signed=`length(${shifted})*sign(${shifted}.y)`;
          code=`select(0.0,1.0,${signed}<=0.0)`; break;
        }
        case 'tiledcloverleafs': case 'tiledhexagons': {
          if (type !== 'color3') fail('TYPE', key, `${n.category} output must be color3`);
          const uv=x('texcoord',[0,0],'vector2'), tiling=x('uvtiling',[1,1],'vector2'), offset=x('uvoffset',[0,0],'vector2'), size=`max(0.0,${x('size',.5,'float')})`, staggered=x('staggered',false,'boolean');
          const p=`(${uv}*${tiling}-${offset})`, cell=`fract(${p})`, centered=`(${cell}*2.0-1.0)`, staggeredP=`vec2f(${cell}.x+select(0.0,0.5,${staggered}&&fract(floor(${p}.y)*0.5)>0.0),${cell}.y)`, local=`(fract(${staggeredP})*2.0-1.0)`;
          let mask;
          if(n.category==='tiledcloverleafs') {
            const q=`(${local}*2.0)`, r=size;
            const d=[`distance(${q}+vec2f(${r},0.0),vec2f(0))`,`distance(${q}-vec2f(${r},0.0),vec2f(0))`,`distance(${q}+vec2f(0.0,${r}),vec2f(0))`,`distance(${q}-vec2f(0.0,${r}),vec2f(0))`];
            mask=`select(0.0,1.0,min(min(${d[0]},${d[1]}),min(${d[2]},${d[3]}))<=${r})`;
          } else {
            const pk=`abs(${centered})`, k='vec2f(-0.8660254,0.5)', kz='0.5773503', folded=`(${pk}-2.0*min(dot(${k},${pk}),0.0)*${k})`, shifted=`(${folded}-vec2f(clamp(${folded}.x,-${kz}*${size},${kz}*${size}),${size}))`;
            mask=`select(0.0,1.0,length(${shifted})*sign(${shifted}.y)<=0.0)`;
          }
          code=`vec3f(${mask})`; break;
        }
        case 'saturate': {
          if (!['color3','color4'].includes(type)) fail('TYPE', key, 'saturate output must be color3/color4');
          const value=input('in',undefined,type), rgb=type==='color4'?`${value.code}.rgb`:value.code;
          const coeff=x('lumacoeffs',[0.2126,0.7152,0.0722],'color3'), amount=x('amount',1,'float');
          const luma=`dot(${coeff},${rgb})`, adjusted=`mix(vec3f(${luma}),${rgb},${amount})`;
          code=type==='color4'?`vec4f(${adjusted},${value.code}.a)`:adjusted; break;
        }
        case 'ramp4': {
          const uv=ins.texcoord?x('texcoord',undefined,'vector2'):'ctx.uv', tl=x('valuetl',undefined,type), tr=x('valuetr',undefined,type), bl=x('valuebl',undefined,type), br=x('valuebr',undefined,type);
          const top=`mix(${tl},${tr},${uv}.x)`, bottom=`mix(${bl},${br},${uv}.x)`; code=`mix(${bottom},${top},${uv}.y)`; break;
        }
        case 'contrast': {
          const amount=x('amount',1,'float'), pivot=x('pivot',.5,'float'), value=input('in',undefined,type);
          if (!['float','color3','color4','vector2','vector3','vector4'].includes(type)) fail('TYPE', key, 'contrast requires a numeric value');
          code=`((${value.code}-${types[type]}(${pivot}))*${types[type]}(${amount})+${types[type]}(${pivot}))`; break;
        }
        case 'premult': {
          if (type !== 'color4') fail('TYPE', key, 'premult output must be color4');
          const value=input('in',undefined,'color4'); code=`vec4f(${value.code}.rgb*${value.code}.a,${value.code}.a)`; break;
        }
        case 'unpremult': {
          if (type !== 'color4') fail('TYPE', key, 'unpremult output must be color4');
          const value=input('in',undefined,'color4'); code=`select(vec4f(0.0),vec4f(${value.code}.rgb/max(${value.code}.a,1e-6),${value.code}.a),${value.code}.a>0.0)`; break;
        }
        case 'g18_rec709_to_lin_rec709': case 'g22_rec709_to_lin_rec709': case 'rec709_display_to_lin_rec709': case 'g22_ap1_to_lin_rec709': case 'srgb_texture_to_lin_rec709': case 'lin_adobergb_to_lin_rec709': case 'adobergb_to_lin_rec709': case 'srgb_displayp3_to_lin_rec709': case 'lin_displayp3_to_lin_rec709': {
          if (!['color3','color4'].includes(type)) fail('TYPE', key, `${n.category} output must be color3/color4`);
          const value=input('in',type==='color4'?[0,0,0,1]:[0,0,0],type), rgb=type==='color4'?`${value.code}.rgb`:value.code;
          let transformed;
          const displayP3=`mat3x3f(1.22493029,-0.04205868,-0.01964128,-0.22492968,1.04205894,-0.07864794,0.00000006,-0.00000001,1.09828925)*`;
          const adobe=`mat3x3f(1.39835574e+00,-2.50233861e-16,2.77555756e-17,-3.98355744e-01,1.00000000e+00,-4.29289893e-02,0.00000000e+00,0.00000000e+00,1.04292899)*`;
          if (n.category==='srgb_texture_to_lin_rec709') transformed=`mxSrgbToLinRec709(${rgb})`;
          else if (n.category==='g22_ap1_to_lin_rec709') transformed=`mxAcescgToLinRec709(pow(max(${rgb},vec3f(0.0)),vec3f(2.2)))`;
          else if (n.category==='lin_adobergb_to_lin_rec709') transformed=`${adobe}${rgb}`;
          else if (n.category==='adobergb_to_lin_rec709') transformed=`${adobe}pow(max(${rgb},vec3f(0.0)),vec3f(2.19921875))`;
          else if (n.category==='lin_displayp3_to_lin_rec709') transformed=`${displayP3}${rgb}`;
          else if (n.category==='srgb_displayp3_to_lin_rec709') transformed=`${displayP3}mxSrgbToLinRec709(${rgb})`;
          else { const gamma=n.category==='g18_rec709_to_lin_rec709'?1.8:n.category==='g22_rec709_to_lin_rec709'?2.2:2.4; transformed=`pow(max(${rgb},vec3f(0.0)),vec3f(${gamma}))`; }
          code=type==='color4'?`vec4f(${transformed},${value.code}.a)`:transformed; break;
        }
        case 'acescg_to_lin_rec709': code=`mxAcescgToLinRec709(${x('in',undefined,'color3')})`; break;
        case 'lin_rec709_to_acescg': code=`mxLinRec709ToAcescg(${x('in',undefined,'color3')})`; break;
        case 'lin_rec709_to_srgb': code=`mxLinRec709ToSrgb(${x('in',undefined,'color3')})`; break;
        case 'srgb_to_lin_rec709': code=`mxSrgbToLinRec709(${x('in',undefined,'color3')})`; break;
        case 'dotproduct': code = `dot(${x('in1')},${x('in2')})`; break;
        case 'crossproduct': code = `cross(${x('in1', undefined, 'vector3')},${x('in2', undefined, 'vector3')})`; break;
        case 'texcoord':
          if (ins.index && Number(ins.index.value) !== uvIndex) fail('GEOMETRY', key, `texcoord index ${Number(ins.index.value)} does not match material UV slot ${uvIndex}`);
          code = type === 'vector2' ? 'ctx.uv' : 'vec3f(ctx.uv,0.0)'; break;
        case 'UsdPrimvarReader': {
          const selector = ins.varname;
          if (selector && (selector.nodename || selector.nodegraph || selector.interfacename)) fail('GEOMETRY', key, 'UsdPrimvarReader varname must be static');
          const name = String(selector?.value ?? '').toLowerCase().replace(/[_-]/g, '');
          const uvName = name.match(/^(?:uv|uvset)([0-9]+)$/);
          const properties = {st:['vector2','ctx.uv'],uv:['vector2','ctx.uv'],uv0:['vector2','ctx.uv'],texcoord:['vector2','ctx.uv'],p:['vector3','ctx.position'],position:['vector3','ctx.position'],n:['vector3','ctx.normal'],normal:['vector3','ctx.normal'],t:['vector3','ctx.tangent'],tangent:['vector3','ctx.tangent'],b:['vector3','ctx.bitangent'],bitangent:['vector3','ctx.bitangent'],color:['color3','ctx.geomcolor.rgb'],displaycolor:['color3','ctx.geomcolor.rgb'],opacity:['float','ctx.geomcolor.a'],displayopacity:['float','ctx.geomcolor.a']};
          if (uvName) {
            if (Number(uvName[1]) !== uvIndex) fail('GEOMETRY', key, `UsdPrimvarReader ${name} does not match material UV slot ${uvIndex}`);
            properties[name] = ['vector2', 'ctx.uv'];
          }
          const property=properties[name];
          if (property) { if (property[0] !== type) fail('TYPE', key, `UsdPrimvarReader ${name} has type ${property[0]}, not ${type}`); code=property[1]; }
          else if (customGeomprop(name) && ['float','vector2','vector3','vector4','color3','color4'].includes(type)) {
            const fallback=x('fallback',widths[type]===1?0:Array(widths[type]).fill(0),type);
            const prop=customGeomprop(name), value=type==='float'?`${prop}.r`:type==='vector2'?`${prop}.rg`:type==='vector3'||type==='color3'?`${prop}.rgb`:`${prop}.rgba`;
            code=`select(${fallback},${value},${prop}.a>0.5)`;
          }
          else code=x('fallback',widths[type]===1?0:Array(widths[type]).fill(0),type);
          break;
        }
        case 'geomcolor': {
          const index=ins.index;
          if(index && (index.nodename||index.nodegraph||index.interfacename)) fail('UNSUPPORTED',key,'geomcolor index must be a static zero');
          if(index && Number(index.value??0)!==0) fail('UNSUPPORTED',key,'only the primary geometry color stream is available');
          if(!['float','color3','color4'].includes(type)) fail('TYPE',key,'geomcolor supports float/color3/color4 outputs');
          code=type==='float'?'ctx.geomcolor.r':type==='color3'?'ctx.geomcolor.rgb':'ctx.geomcolor'; break;
        }
        case 'geompropvalue': case 'geompropvalueuniform': {
          const selector = ins.geomprop;
          if (selector && (selector.nodename || selector.nodegraph || selector.interfacename)) fail('GEOMETRY', key, 'geomprop must be a static token');
          const name = String(selector?.value ?? '').toLowerCase().replace(/[_-]/g, '');
          const properties = {
            st: ['vector2', 'ctx.uv'], uv: ['vector2', 'ctx.uv'], uv0: ['vector2', 'ctx.uv'], texcoord: ['vector2', 'ctx.uv'], texcoord0: ['vector2', 'ctx.uv'],
            p: ['vector3', 'ctx.position'], position: ['vector3', 'ctx.position'],
            n: ['vector3', 'ctx.normal'], normal: ['vector3', 'ctx.normal'],
            t: ['vector3', 'ctx.tangent'], tangent: ['vector3', 'ctx.tangent'],
            b: ['vector3', 'ctx.bitangent'], bitangent: ['vector3', 'ctx.bitangent'],
            color: ['color3', 'ctx.geomcolor.rgb'], displaycolor: ['color3', 'ctx.geomcolor.rgb'],
            opacity: ['float', 'ctx.geomcolor.a'], displayopacity: ['float', 'ctx.geomcolor.a']
          };
          const uvName = name.match(/^(?:uv|uvset)([0-9]+)$/);
          if (uvName) {
            if (Number(uvName[1]) !== uvIndex) fail('GEOMETRY', key, `geompropvalue ${name} does not match material UV slot ${uvIndex}`);
            properties[name] = ['vector2', 'ctx.uv'];
          }
          const property = properties[name];
          if (property) {
            if (property[0] !== type) fail('TYPE', key, `geomprop ${name} has type ${property[0]}, not ${type}`);
            code = property[1];
          } else if (customGeomprop(name) && ['float','vector2','vector3','vector4','color3','color4'].includes(type)) {
            const fallback = x('default', widths[type] === 1 ? 0 : Array(widths[type]).fill(0), type);
            const prop=customGeomprop(name), value = type === 'float' ? `${prop}.r` : type === 'vector2' ? `${prop}.rg` : type === 'vector3' || type === 'color3' ? `${prop}.rgb` : `${prop}.rgba`;
            code = `select(${fallback},${value},${prop}.a>0.5)`;
          } else {
            const fallback = widths[type] === 1 ? 0 : Array(widths[type]).fill(0);
            code = x('default', fallback, type);
          }
          break;
        }
        case 'position': case 'normal': case 'tangent': case 'bitangent':
          if (ins.space?.value && ins.space.value !== 'world') fail('GEOMETRY', key, 'only world-space geometric vectors are available');
          code = `ctx.${n.category}`; break;
        case 'viewdirection':
          if (type !== 'vector3') fail('TYPE', key, 'viewdirection output must be vector3');
          if (ins.space?.value && ins.space.value !== 'world') fail('GEOMETRY', key, 'only world-space view directions are available');
          code = 'ctx.viewdir'; break;
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
        case 'transpose': {
          if (!['matrix33','matrix44'].includes(type)) fail('TYPE', key, 'transpose requires matrix33/matrix44 output');
          const value=input('in',undefined,type); code=`transpose(${value.code})`; break;
        }
        case 'determinant': {
          if (type !== 'float') fail('TYPE', key, 'determinant output must be float');
          const value=input('in'); if (!['matrix33','matrix44'].includes(value.type)) fail('TYPE', key, 'determinant input must be matrix33/matrix44');
          code=`determinant(${value.code})`; break;
        }
        case 'invertmatrix': {
          if (!['matrix33','matrix44'].includes(type)) fail('TYPE', key, 'invertmatrix requires matrix33/matrix44 output');
          const value=input('in',undefined,type); code=`inverse(${value.code})`; break;
        }
        case 'creatematrix': {
          if (!['matrix33','matrix44'].includes(type)) fail('TYPE', key, 'creatematrix output must be matrix33/matrix44');
          const count=type==='matrix33'?3:4, values=[];
          for(let i=1;i<=count;i++){const value=input(`in${i}`);if(type==='matrix33'&&value.type!=='vector3')fail('TYPE',key,'matrix33 inputs must be vector3');if(type==='matrix44'&&!['vector3','vector4'].includes(value.type))fail('TYPE',key,'matrix44 inputs must be vector3/vector4');values.push(type==='matrix44'&&value.type==='vector3'?`vec4f(${value.code},${i===4?'1.0':'0.0'})`:value.code);}
          code=`${types[type]}(${values.join(',')})`; break;
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
        case 'transformnormal': case 'transformpoint': case 'transformvector': {
          if (type !== 'vector3') fail('TYPE', key, `${n.category} output must be vector3`);
          for (const name of ['fromspace','tospace']) if (ins[name]?.nodename || ins[name]?.nodegraph || ins[name]?.interfacename) fail('GEOMETRY', key, `${n.category} spaces must be static`);
          const from=String(ins.fromspace?.value??'').toLowerCase(), to=String(ins.tospace?.value??'').toLowerCase();
          const known=new Set(['','world','object','tangent']);
          if (!known.has(from)||!known.has(to)) fail('GEOMETRY', key, `${n.category} has an unsupported space`);
          const worldAlias=(from===''||from==='world')&&(to===''||to==='world');
          if (from!==to && !worldAlias) fail('GEOMETRY', key, `${n.category} cannot transform between non-world spaces`);
          code=x('in',n.category==='transformnormal'?[0,0,1]:[0,0,0],'vector3');
          break;
        }
        case 'trianglewave': {
          if (type !== 'float') fail('TYPE', key, 'trianglewave output must be float');
          const value=x('in',0,'float'); code=`(0.5-abs(fract(abs(${value}))-0.5))`; break;
        }
        case 'normalmap': {
          const scale=ins.scale?input('scale'):{type:'float',code:'1.0'};if(!['float','vector2'].includes(scale.type))fail('TYPE',key,'normalmap scale must be float or vector2');
          const vector=(k,field)=>ins[k]?x(k,undefined,'vector3'):`ctx.${field}`;
          const normalInput=ins.in?input('in'):{type:'vector3',code:'vec3f(.5,.5,1.0)'};
          if(!['color3','vector3'].includes(normalInput.type))fail('TYPE',key,'normalmap input must be color3/vector3');
          code=`mxNormalmap(${normalInput.code},vec2f(${scale.code}),${vector('normal','normal')},${vector('tangent','tangent')},${vector('bitangent','bitangent')})`;break;
        }
        case 'gltf_normalmap': {
          if (type !== 'vector3') fail('TYPE', key, 'gltf_normalmap output must be vector3');
          const allowedInputs = ['file', 'default', 'texcoord', 'pivot', 'scale', 'rotate', 'offset', 'operationorder', 'uaddressmode', 'vaddressmode', 'filtertype'];
          for (const name of Object.keys(ins)) if (!allowedInputs.includes(name)) fail('UNSUPPORTED', key, `unsupported gltf_normalmap input ${name}`);
          const fallback = x('default', [.5, .5, 1], 'vector3');
          const file = ins.file?.value ?? '';
          const normal = ins.normal ? x('normal', undefined, 'vector3') : 'ctx.normal';
          const tangent = ins.tangent ? x('tangent', undefined, 'vector3') : 'ctx.tangent';
          const bitangent = ins.bitangent ? x('bitangent', undefined, 'vector3') : 'ctx.bitangent';
          const normalMap = value => `mxNormalmap(${value},vec2f(1.0),${normal},${tangent},${bitangent})`;
          if (!file) { code = normalMap(fallback); break; }
          if (ins.file.nodename || ins.file.nodegraph || ins.file.interfacename) fail('UNSUPPORTED', key, 'connected gltf_normalmap filenames are not implemented');
          const descriptor = Object.hasOwn(imageDescriptors, file) && imageDescriptors[file];
          if (!descriptor) fail('RESOURCE', key, `missing decoded image ${file}`);
          if (n.colorspace && normalizeColorSpace(n.colorspace) !== normalizeColorSpace(descriptor.colorspace)) fail('SEMANTICS', key, 'gltf_normalmap colorspace differs from decoded resource');
          const address = name => { const p = ins[name]; const mode = ['constant', 'clamp', 'periodic', 'mirror'].indexOf(p?.value ?? 'periodic'); if (mode < 0 || p?.nodename || p?.interfacename || p?.nodegraph) fail('UNSUPPORTED', key, 'invalid or connected gltf_normalmap address mode'); return `${mode}u`; };
          const filter = ins.filtertype?.value ?? 'linear';
          if (!['closest', 'linear', 'cubic'].includes(filter) || ins.filtertype?.nodename || ins.filtertype?.interfacename || ins.filtertype?.nodegraph) fail('UNSUPPORTED', key, 'only static closest/linear/cubic gltf_normalmap filters are implemented');
          const uvBase = ins.texcoord ? x('texcoord', undefined, 'vector2') : 'ctx.uv';
          const uv = `((mat2x2f(cos(-${x('rotate',0,'float')}*0.017453292519943295),sin(-${x('rotate',0,'float')}*0.017453292519943295),-sin(-${x('rotate',0,'float')}*0.017453292519943295),cos(-${x('rotate',0,'float')}*0.017453292519943295)) * ((${uvBase}-${x('pivot',[0,1],'vector2')})*${x('scale',[1,1],'vector2')}))+${x('pivot',[0,1],'vector2')}+vec2f(${x('offset',[0,0],'vector2')}.x,-${x('offset',[0,0],'vector2')}.y))`;
          const fill = `vec4f(${fallback},0)`;
          const lodScale = `abs(${x('scale',[1,1],'vector2')})`;
          const lod = `log2(max(1.0,max(length(ctx.uvDx*vec2f(${descriptor.width}.0,${descriptor.height}.0)*${lodScale}),length(ctx.uvDy*vec2f(${descriptor.width}.0,${descriptor.height}.0)*${lodScale}))))`;
          const udim = descriptor.udim, grid = udim && `vec2u(${udim.columns}u,${udim.rows}u)`;
          const sample = filter === 'cubic'
            ? (udim ? `imageSampleCubicUDIM(${descriptor.offset}u,vec2u(${descriptor.width}u,${descriptor.height}u),${descriptor.levels}u,${uv},${grid},${lod},${fill})` : `imageSampleCubic(${descriptor.offset}u,vec2u(${descriptor.width}u,${descriptor.height}u),${descriptor.levels}u,${uv},${lod},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${fill})`)
            : (udim ? `imageSampleUDIM(${descriptor.offset}u,vec2u(${descriptor.width}u,${descriptor.height}u),${descriptor.levels}u,${uv},${grid},${lod},${filter === 'linear'},${fill})` : `imageSample(${descriptor.offset}u,vec2u(${descriptor.width}u,${descriptor.height}u),${descriptor.levels}u,${uv},${lod},vec2u(${address('uaddressmode')},${address('vaddressmode')}),${filter === 'linear'},${fill})`);
          code = normalMap(`${sample}.rgb`); break;
        }
        case 'gltf_iridescence_thickness': {
          if (type !== 'float') fail('TYPE', key, 'gltf_iridescence_thickness output must be float');
          const wrapperInputs = new Set(['thicknessMin', 'thicknessMax']);
          const imageInputs = Object.fromEntries(Object.entries(ins).filter(([name]) => !wrapperInputs.has(name)));
          const imageNode = { ...n, name: `${n.name || key}_image`, category: 'gltf_image', type: 'vector3', inputs: imageInputs, outputs: undefined, nodedef: undefined, version: undefined };
          const nested = compileGraph({ nodes: [imageNode] }, { output: { nodename: imageNode.name }, imageDescriptors, uvIndex, geompropNames: customGeompropNames });
          const prefix = `gltfit${serial++}_`, rename = source => source.replace(/\bn\d+\b/g, match => `${prefix}${match}`);
          if (nested.body) lines.push(rename(nested.body));
          const image = rename(nested.expression);
          code = `mix(${x('thicknessMin',100,'float')},${x('thicknessMax',400,'float')},clamp(${image}.g,0.0,1.0))`;
          break;
        }
        case 'gltf_anisotropy_image': {
          if (!['anisotropy_strength_out', 'anisotropy_rotation_out'].includes(out)) fail('OUTPUT', key, 'gltf_anisotropy_image output must be anisotropy_strength_out or anisotropy_rotation_out');
          const wrapperInputs = new Set(['anisotropy_strength', 'anisotropy_rotation']);
          const imageInputs = Object.fromEntries(Object.entries(ins).filter(([name]) => !wrapperInputs.has(name)));
          const imageNode = { ...n, name: `${n.name || key}_image`, category: 'gltf_image', type: 'vector3', inputs: imageInputs, outputs: undefined, nodedef: undefined, version: undefined };
          const nested = compileGraph({ nodes: [imageNode] }, { output: { nodename: imageNode.name }, imageDescriptors, uvIndex, geompropNames: customGeompropNames });
          const prefix = `gltfani${serial++}_`, rename = source => source.replace(/\bn\d+\b/g, match => `${prefix}${match}`);
          if (nested.body) lines.push(rename(nested.body));
          const image = rename(nested.expression), strength = `${x('anisotropy_strength',1,'float')}*${image}.b`, rotation = `${x('anisotropy_rotation',0,'float')}+atan2(${image}.g*2.0-1.0,${image}.r*2.0-1.0)`;
          code = out === 'anisotropy_strength_out' ? strength : rotation;
          break;
        }
        case 'gltf_colorimage': {
          if (!['outcolor', 'outa'].includes(out)) fail('OUTPUT', key, 'gltf_colorimage output must be outcolor or outa');
          const imageInputs = Object.fromEntries(Object.entries(ins).filter(([name]) => !['color', 'geomcolor'].includes(name)));
          const imageNode = { ...n, name: `${n.name || key}_image`, category: 'gltf_image', type: 'color4', inputs: imageInputs, outputs: undefined, nodedef: undefined, version: undefined };
          const nested = compileGraph({ nodes: [imageNode] }, { output: { nodename: imageNode.name }, imageDescriptors, uvIndex, geompropNames: customGeompropNames });
          const prefix = `gci${serial++}_`;
          const rename = source => source.replace(/\bn\d+\b/g, match => `${prefix}${match}`);
          if (nested.body) lines.push(rename(nested.body));
          const image = rename(nested.expression);
          const color = x('color', [1, 1, 1, 1], 'color4'), geomcolor = x('geomcolor', [1, 1, 1, 1], 'color4');
          const combined = `(${image}*${color}*${geomcolor})`;
          code = out === 'outa' ? `${combined}.a` : `${combined}.rgb`;
          break;
        }
        case 'bump': case 'bump3': case 'heighttonormal': {
          if (type !== 'vector3') fail('TYPE',key,'bump output must be vector3');
          const encoded=n.category==='heighttonormal';
          const heightName=n.category==='bump'?'height':'in';
          input(heightName,0,'float');
          const scale=x('scale',1,'float'), normal=!encoded&&ins.normal?x('normal',undefined,'vector3'):'ctx.normal';
          if(++bumpDepth>2)fail('LIMIT',key,'bump nesting exceeds two levels');
          const parent=contextName, step=literal('float',Math.max(1e-6,Math.min(.001,...Object.values(imageDescriptors).map(d=>.5/Math.max(d.width,d.height)))));
          const heights=[], coordinates=[];
          try {
            for(const delta of [`vec2f(${step},0)`,`vec2f(-${step},0)`,`vec2f(0,${step})`,`vec2f(0,-${step})`]) {
              const shifted=`bumpCtx${serial++}`;
              lines.push(`let ${shifted}=mxOffsetContext(${parent},${delta});`);
              contextName=shifted;heights.push(x(heightName,0,'float'));
              if(encoded)coordinates.push(ins.texcoord?x('texcoord',undefined,'vector2'):`${shifted}.uv`);
            }
          } finally {contextName=parent;bumpDepth--;}
          const gradient=`vec2f(${heights[0]}-${heights[1]},${heights[2]}-${heights[3]})/(2.0*${step})`;
          if(encoded){
            code=`mxHeightToNormal(${gradient},(${coordinates[0]}-${coordinates[1]})/(2.0*${step}),(${coordinates[2]}-${coordinates[3]})/(2.0*${step}),${scale})`;break;
          }
          const du=ins.tangent?x('tangent',undefined,'vector3'):'ctx.tangent';
          const dv=ins.bitangent?x('bitangent',undefined,'vector3'):'ctx.bitangent';
          code=`mxBumpGradient(${gradient},${scale},${normal},${du},${dv})`;break;
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
        case 'swizzle': case 'reorder': {
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
          const v = x('in', undefined, 'vector2'), a = `(${angle('amount', 0)} * 0.017453292519943295)`;
          code = `(mat2x2f(cos(${a}),sin(${a}),-sin(${a}),cos(${a})) * ${v})`; break;
        }
        case 'rotate3d': {
          if (type !== 'vector3') fail('TYPE', key, 'rotate3d output must be vector3');
          const v = x('in', undefined, 'vector3'), axis = `safeNormal(${x('axis', [0, 0, 1], 'vector3')},vec3f(0.0,0.0,1.0))`;
          const a = `(${angle('amount', 0)} * 0.017453292519943295)`, c = `cos(${a})`, s = `sin(${a})`;
          code = `(${v}*${c}+cross(${axis},${v})*${s}+${axis}*dot(${axis},${v})*(1.0-${c}))`; break;
        }
        case 'UsdTransform2d': {
          if (type !== 'vector2') fail('TYPE', key, 'UsdTransform2d output must be vector2');
          const v=x('in',[0,0],'vector2'), scale=x('scale',[1,1],'vector2'), translation=x('translation',[0,0],'vector2'), a=`(${angle('rotation',0)}*0.017453292519943295)`;
          code=`((mat2x2f(cos(${a}),sin(${a}),-sin(${a}),cos(${a})) * (${v}*${scale}))+${translation})`; break;
        }
        case 'place2d': {
          if (type !== 'vector2') fail('TYPE', key, 'place2d output must be vector2');
          const uv=ins.texcoord?x('texcoord',undefined,'vector2'):'ctx.uv', pivot=x('pivot',[0,0],'vector2'), scale=x('scale',[1,1],'vector2'), rotate=`(${angle('rotate',0)}*0.017453292519943295)`, offset=x('offset',[0,0],'vector2');
          code=`((mat2x2f(cos(${rotate}),sin(${rotate}),-sin(${rotate}),cos(${rotate})) * ((${uv}-${pivot})/${scale}))+${pivot}-${offset})`; break;
        }
        case 'UsdPreviewSurface': {
          if (!material) fail('CONTEXT', key, 'UsdPreviewSurface requires material compilation');
          const supported = new Set(['diffuseColor','emissiveColor','useSpecularWorkflow','specularColor','metallic','roughness','clearcoat','clearcoatRoughness','opacity','opacityMode','opacityThreshold','ior','normal','displacement','occlusion']);
          for (const k of Object.keys(n.inputs || {})) if (!supported.has(k)) fail('UNSUPPORTED', `${key}/${k}`, 'UsdPreviewSurface input not implemented');
          const displacement=ins.displacement;
          if (displacement && (displacement.nodename || displacement.nodegraph || displacement.interfacename || Number(displacement.value)!==0)) fail('UNSUPPORTED', `${key}/displacement`, 'UsdPreviewSurface displacement requires a displacement terminal');
          const base=`(${x('diffuseColor',[.18,.18,.18],'color3')}*${x('occlusion',1,'float')})`, metallic=x('metallic',0,'float'), roughness=x('roughness',.5,'float'), ior=x('ior',1.5,'float');
          const specularColor=x('specularColor',[0,0,0],'color3'), emission=x('emissiveColor',[0,0,0],'color3');
          const lobe=`withSpecularColorMode(makeMaterial(${base},${metallic},${roughness},${ior},0.0,${emission},1.0,0.0,vec3f(1),0u,0.0,1.5),${specularColor},${x('useSpecularWorkflow',false,'boolean')})`;
          const coat=`closureScale(closureLeaf(nativeDielectric(vec3f(1),1.5,vec2f(${x('clearcoatRoughness',.01,'float')}),1.0,1u)),vec3f(clamp(${x('clearcoat',0,'float')},0.0,1.0)))`;
          const closure=`closureAdd(closureLeaf(${lobe}),${coat})`, opacity=x('opacity',1,'float'), mode=x('opacityMode',0,'integer'), threshold=x('opacityThreshold',0,'float');
          const alpha=`select(clamp(${opacity},0.0,1.0),select(0.0,1.0,${opacity}>=${threshold}),${mode}==1i)`;
          const normal = ins.normal ? x('normal', undefined, 'vector3') : 'ctx.normal';
          code=`materialFromClosure(${closure},${emission},${alpha},${normal})`; break;
        }
        case 'surface_unlit': {
          if (!material) fail('CONTEXT', key, 'surface_unlit requires material compilation');
          const allowed=new Set(['emission','emission_color','transmission','transmission_color','opacity']);
          for(const k of Object.keys(n.inputs||{})) if(!allowed.has(k)) fail('UNSUPPORTED',`${key}/${k}`,'surface_unlit input not implemented');
          const emission=x('emission',1,'float'), emissionColor=x('emission_color',[1,1,1],'color3'), transmission=x('transmission',0,'float'), transmissionColor=x('transmission_color',[1,1,1],'color3');
          const bsdf=`closureLeaf(nativeDielectric(${transmissionColor},1.0,vec2f(0.0),clamp(${transmission},0.0,1.0),2u))`;
          code=`materialFromClosure(${bsdf},${emissionColor}*max(0.0,${emission}),clamp(${x('opacity',1,'float')},0.0,1.0),ctx.normal)`; break;
        }
        case 'standard_surface': case 'open_pbr_surface': {
          if (!material) fail('CONTEXT', key, 'surface requires material compilation');
          const open = n.category === 'open_pbr_surface';
          // This baseline mapping is explicitly approximate, not a reference closure.
          const pick = (names, fallback, expected) => x(names.find(name => ins[name]) || names[0], fallback, expected);
          const baseColorInput = pick(['base_color'], [0.8, 0.8, 0.8], 'color3');
          const baseWeight = open ? (ins.base_weight ? x('base_weight', 1, 'float') : '1.0') : (ins.base ? x('base', 1, 'float') : '1.0');
          const baseColor = `(${baseColorInput} * vec3f(${baseWeight}))`;
          const fields = [baseColor, pick(open ? ['base_metalness'] : ['metalness'], 0, 'float'), pick(open ? ['specular_roughness', 'base_diffuse_roughness'] : ['specular_roughness', 'diffuse_roughness'], 0.3, 'float'), pick(open ? ['specular_ior'] : ['specular_IOR', 'specular_ior'], 1.5, 'float'), pick(open ? ['transmission_weight'] : ['transmission'], 0, 'float'), pick(['emission_color'], [1, 1, 1], 'color3'), pick(open ? ['emission_luminance'] : ['emission'], 0, 'float'), pick(open ? ['specular_roughness_anisotropy'] : ['specular_anisotropy'], 0, 'float'), pick(['transmission_color'], [1,1,1], 'color3')];
          const supported = new Set(open ? ['base_weight', 'base_color', 'base_diffuse_roughness', 'base_metalness', 'specular_weight', 'specular_color', 'specular_roughness', 'specular_ior', 'specular_roughness_anisotropy', 'transmission_weight', 'transmission_color', 'transmission_depth', 'transmission_scatter', 'subsurface_weight', 'subsurface_color', 'subsurface_radius', 'subsurface_scale', 'subsurface_anisotropy', 'coat_weight', 'coat_color', 'coat_roughness', 'coat_ior', 'fuzz', 'fuzz_color', 'fuzz_roughness', 'thin_film_weight', 'thin_film_thickness', 'thin_film_ior', 'emission_color', 'emission_luminance', 'geometry_opacity', 'geometry_thin_walled', 'normal', 'geometry_normal'] : ['base', 'base_color', 'diffuse_roughness', 'metalness', 'specular', 'specular_color', 'specular_roughness', 'specular_IOR', 'specular_ior', 'specular_anisotropy', 'transmission', 'transmission_color', 'transmission_depth', 'transmission_scatter', 'subsurface', 'subsurface_color', 'subsurface_radius', 'subsurface_anisotropy', 'coat', 'coat_color', 'coat_roughness', 'coat_IOR', 'sheen', 'sheen_color', 'sheen_roughness', 'emission_color', 'emission', 'opacity', 'thin_walled', 'normal', 'thin_film_thickness', 'thin_film_IOR']);
          for (const k of Object.keys(n.inputs || {})) if (!supported.has(k)) fail('UNSUPPORTED', `${key}/${k}`, 'surface input not yet implemented');
          const opacityInput = open ? 'geometry_opacity' : 'opacity';
          const opacity = ins[opacityInput] ? x(opacityInput, 1, 'float') : '1.0';
          const thin = open ? (ins.geometry_thin_walled ? `select(0u,1u,${x('geometry_thin_walled', false, 'boolean')})` : '0u') : (ins.thin_walled ? `select(0u,1u,${x('thin_walled', false, 'boolean')})` : '0u');
          const filmThickness = open && ins.thin_film_weight && ins.thin_film_thickness ? `(${nanometer('thin_film_thickness', 0)}*clamp(${x('thin_film_weight', 0, 'float')},0.0,1.0))` : !open && ins.thin_film_thickness ? nanometer('thin_film_thickness', 0) : '0.0';
          const filmIOR = open && ins.thin_film_ior ? x('thin_film_ior', 1.5, 'float') : !open && ins.thin_film_IOR ? x('thin_film_IOR', 1.5, 'float') : '1.5';
          const specularWeight = open ? (ins.specular_weight ? x('specular_weight', 1, 'float') : '1.0') : (ins.specular ? x('specular', 1, 'float') : '1.0');
          const specularColor = ins.specular_color ? x('specular_color', [1, 1, 1], 'color3') : 'vec3f(1)';
          const subsurfaceWeight = open ? (ins.subsurface_weight ? x('subsurface_weight', 0, 'float') : '0.0') : (ins.subsurface ? x('subsurface', 0, 'float') : '0.0');
          const subsurfaceColor = ins.subsurface_color ? x('subsurface_color', [0.8, 0.8, 0.8], 'color3') : fields[0];
          const authoredSubsurfaceRadius = ins.subsurface_radius ? x('subsurface_radius', [1, 1, 1], 'color3') : 'vec3f(1)';
          const subsurfaceScale = open && ins.subsurface_scale ? `max(0.0,${x('subsurface_scale', 1, 'float')})` : '1.0';
          const subsurfaceRadius = `(${authoredSubsurfaceRadius}*${subsurfaceScale})`;
          const subsurfaceAnisotropy = ins.subsurface_anisotropy ? x('subsurface_anisotropy', 0, 'float') : '0.0';
          const subsurfaceRoughness = `clamp(max(max(${subsurfaceRadius}.x,${subsurfaceRadius}.y),${subsurfaceRadius}.z),0.02,1.0)`;
          const baseLobe = `makeMaterial(${fields.join(',')},${thin},${filmThickness},${filmIOR})`;
          const transmissionDepth = ins.transmission_depth ? x('transmission_depth', 0, 'float') : '0.0';
          const transmissionScatter = ins.transmission_scatter ? x('transmission_scatter', [0, 0, 0], 'color3') : 'vec3f(0)';
          const weightedTransmission = `withSpecular(withTransmission(${baseLobe},${transmissionDepth},${transmissionScatter}),${specularWeight})`;
          const transmittedLobe = ins.specular_color ? `withSpecularColorTint(${weightedTransmission},${specularColor})` : weightedTransmission;
          const closure = `closureMix(closureLeaf(${transmittedLobe}),closureLeaf(nativeSubsurface(${subsurfaceColor},1.0,${subsurfaceRadius},${subsurfaceAnisotropy})),clamp(${subsurfaceWeight},0.0,1.0))`;
          const coatWeight = open ? (ins.coat_weight ? x('coat_weight', 0, 'float') : '0.0') : (ins.coat ? x('coat', 0, 'float') : '0.0');
          const coatColor = ins.coat_color ? x('coat_color', [1, 1, 1], 'color3') : 'vec3f(1)';
          const coatRoughness = ins.coat_roughness ? x('coat_roughness', .1, 'float') : '.1';
          const coatIOR = open ? (ins.coat_ior ? x('coat_ior', 1.5, 'float') : '1.5') : (ins.coat_IOR ? x('coat_IOR', 1.5, 'float') : '1.5');
          const coated = `closureAdd(${closure},closureScale(closureLeaf(nativeDielectric(${coatColor},${coatIOR},vec2f(${coatRoughness}*${coatRoughness}),1.0,1u)),vec3f(clamp(${coatWeight},0.0,1.0))))`;
          const sheenWeight = open ? (ins.fuzz ? x('fuzz', 0, 'float') : '0.0') : (ins.sheen ? x('sheen', 0, 'float') : '0.0');
          const sheenColor = open ? (ins.fuzz_color ? x('fuzz_color', [1, 1, 1], 'color3') : 'vec3f(1)') : (ins.sheen_color ? x('sheen_color', [1, 1, 1], 'color3') : 'vec3f(1)');
          const sheenRoughness = open ? (ins.fuzz_roughness ? x('fuzz_roughness', .6, 'float') : '.6') : (ins.sheen_roughness ? x('sheen_roughness', .6, 'float') : '.6');
          const finalClosure = `closureAdd(${coated},closureScale(closureLeaf(nativeSheen(${sheenColor},1.0,clamp(${sheenRoughness},0.02,1.0),0u)),vec3f(clamp(${sheenWeight},0.0,1.0))))`;
          const normal = ins.normal ? x('normal', undefined, 'vector3') : ins.geometry_normal ? x('geometry_normal', undefined, 'vector3') : 'ctx.normal';
          code = `materialFromClosure(${finalClosure},${fields[5]}*${fields[6]},clamp(${opacity},0.0,1.0),normalize(${normal}))`; break;
        }
        case 'surfacematerial': result = input('surfaceshader'); break;
        case 'volumematerial': result = input('volumeshader'); break;
        default: fail('UNSUPPORTED', key, `node ${n.category} (${type}) is not implemented`);
      }
      if (!result) {
        if(closureCount>MAX_CLOSURE_LOBES)fail('LIMIT',key,`closure exceeds ${MAX_CLOSURE_LOBES} lobes`);
        const target = materialCategories.has(n.category) ? 'Material' : type==='EDF' ? 'vec3f' : types[type];
        if (!target) fail('TYPE', key, `unsupported output type ${type}`);
        const id = `n${serial++}`;
        if(serial>32768)fail('LIMIT',key,'expanded graph exceeds 32768 expressions');
        lines.push(`let ${id}: ${target} = ${code.replace(/\bctx\b/g,contextName)};`);
        const replaceContext = value => typeof value === 'string' ? value.replace(/\bctx\b/g, contextName) : value;
        result = { type, code: id, closureCount, hasInterior, interiorCategories, categories:[...dependencies], ...(volumeEmission ? { volumeEmission: true } : {}), ...(normal ? { normal: replaceContext(normal) } : {}), ...(lightInfo ? { lightInfo: Object.fromEntries(Object.entries(lightInfo).map(([key, value]) => [key, replaceContext(value)])) } : {}), ...(emissionCone ? { emissionCone: { direction: replaceContext(emissionCone.direction), innerCos: replaceContext(emissionCone.innerCos), outerCos: replaceContext(emissionCone.outerCos) } } : {}), ...(emissionSchlick ? { emissionSchlick: { color0: replaceContext(emissionSchlick.color0), color90: replaceContext(emissionSchlick.color90), exponent: replaceContext(emissionSchlick.exponent) } } : {}), ...(emissionProfile ? { emissionProfile: { direction: replaceContext(emissionProfile.direction), id: emissionProfile.id } } : {}) };
      }
    }
    used.add(n.category); active.delete(key); cached.set(key, result); return result;
  }
  const selected = output || { nodename: document.nodes.at(-1)?.name };
  const value = port(selected, root, {}, undefined, '$output');
  return { body: lines.join('\n'), expression: value.code, type: value.type, categories: [...used].sort(), hasInterior:value.hasInterior||false,interiorCategories:value.interiorCategories||[], volumeEmission:value.volumeEmission||false, ...(value.lightInfo ? { lightInfo: value.lightInfo } : {}), diagnostics: [], referenceReady: false };
}

export const contextWGSL = `struct ShadingContext { position: vec3f, normal: vec3f, tangent: vec3f, bitangent: vec3f, uv: vec2f, time: f32, frame: f32, uvDx: vec2f, uvDy: vec2f, dpdu:vec3f, dpdv:vec3f, viewdir:vec3f, geomcolor:vec4f, geomprop:vec4f, geomprop1:vec4f, geomprop2:vec4f, geomprop3:vec4f, geomprop4:vec4f, geomprop5:vec4f, geomprop6:vec4f, geomprop7:vec4f }
fn mxOffsetContext(ctx:ShadingContext,delta:vec2f)->ShadingContext {
  var shifted=ctx;shifted.uv+=delta;shifted.position+=ctx.dpdu*delta.x+ctx.dpdv*delta.y;return shifted;
}
fn mxBumpGradient(gradient:vec2f,scale:f32,normal:vec3f,du:vec3f,dv:vec3f)->vec3f {
  // Match NG_bump_vector3: heighttonormal at default scale, then normalmap.
  return mxNormalmap(mxHeightToNormal(gradient,vec2f(1,0),vec2f(0,1),1.0),vec2f(scale),normal,du,dv);
}
fn mxSurfaceDerivatives(n:vec3f,p1:vec3f,p2:vec3f,uv1:vec2f,uv2:vec2f)->mat2x3f {
  let det=uv1.x*uv2.y-uv1.y*uv2.x;
  if(abs(det)<=1e-7*max(length(uv1)*length(uv2),1e-30)){
    let frame=mxSurfaceFrame(n,p1,p2,uv1,uv2);return mat2x3f(frame[0],frame[1]);
  }
  return mat2x3f((p1*uv2.y-p2*uv1.y)/det,(p2*uv1.x-p1*uv2.x)/det);
}
fn mxHsvToRgb(c:vec3f)->vec3f {
  let k=vec4f(1.0,2.0/3.0,1.0/3.0,3.0);
  let p=abs(fract(c.xxx+k.xyz)*6.0-k.www);
  return c.z*mix(k.xxx,clamp(p-k.xxx,vec3f(0),vec3f(1)),c.y);
}
fn mxRgbToHsv(c:vec3f)->vec3f {
  let p=select(vec4f(c.bg,-1.0,2.0/3.0),vec4f(c.gb,0.0,-1.0/3.0),c.g<c.b);
  let q=select(vec4f(p.xyw,c.r),vec4f(c.r,p.yzx),c.r<p.x);
  let d=q.x-min(q.w,q.y); let e=1e-10;
  return vec3f(abs(q.z+(q.w-q.y)/(6.0*d+e)),d/(q.x+e),q.x);
}
fn mxAcescgToLinRec709(c:vec3f)->vec3f {
  return mat3x3f(1.705050992658,-.130256417507,-.024003356805,
                 -.621792120657,1.140804736575,-.128968976065,
                 -.083258872001,-.010548319068,1.15297233287)*c;
}
fn mxLinRec709ToAcescg(c:vec3f)->vec3f {
  return mat3x3f(.613097402401,.070193722470,.020615592882,
                 .339523146184,.916353879058,.109569772938,
                 .047379451415,.013452398473,.869814634179)*c;
}
fn mxLinRec709ToSrgb(c:vec3f)->vec3f {
  let a=abs(c); let encoded=select(12.92*a,1.055*pow(a,vec3f(1.0/2.4))-.055,a>vec3f(.0031308));
  return sign(c)*encoded;
}
fn mxSrgbToLinRec709(c:vec3f)->vec3f {
  let a=abs(c); let linear=select(a/12.92,pow((a+.055)/1.055,vec3f(2.4)),a>vec3f(.04045));
  return sign(c)*linear;
}
fn mxHash2(p:vec2f)->f32 { return fract(sin(dot(p,vec2f(127.1,311.7)))*43758.5453123); }
fn mxHash3(p:vec3f)->f32 { return fract(sin(dot(p,vec3f(127.1,311.7,74.7)))*43758.5453123); }
fn mxRotl32(x:u32,k:u32)->u32 { return (x<<k)|(x>>(32u-k)); }
fn mxBjMix(a0:u32,b0:u32,c0:u32)->vec3u { var a=a0;var b=b0;var c=c0;a-=c;a^=mxRotl32(c,4u);c+=b;b-=a;b^=mxRotl32(a,6u);a+=c;c-=b;c^=mxRotl32(b,8u);b+=a;a-=c;a^=mxRotl32(c,16u);c+=b;b-=a;b^=mxRotl32(a,19u);a+=c;c-=b;c^=mxRotl32(b,4u);b+=a;return vec3u(a,b,c); }
fn mxBjFinal(a0:u32,b0:u32,c0:u32)->u32 { var a=a0;var b=b0;var c=c0;c^=b;c-=mxRotl32(b,14u);a^=c;a-=mxRotl32(c,11u);b^=a;b-=mxRotl32(a,25u);c^=b;c-=mxRotl32(b,16u);a^=c;a-=mxRotl32(c,4u);b^=a;b-=mxRotl32(a,14u);c^=b;c-=mxRotl32(b,24u);return c; }
fn mxCell3(p:vec3f)->f32 { let q=vec3i(i32(floor(p.x)),i32(floor(p.y)),i32(floor(p.z)));let seed=0xdeadbeefu+(3u<<2u)+13u;return f32(mxBjFinal(seed+u32(q.x),seed+u32(q.y),seed+u32(q.z)))/4294967295.0; }
fn mxCell4(p:vec4f)->f32 { let q=vec4i(i32(floor(p.x)),i32(floor(p.y)),i32(floor(p.z)),i32(floor(p.w)));let seed=0xdeadbeefu+(4u<<2u)+13u;let m=mxBjMix(seed+u32(q.x),seed+u32(q.y),seed+u32(q.z));return f32(mxBjFinal(m.x+u32(q.w),m.y,m.z))/4294967295.0; }
fn mxFlakeCell3(p:vec3f,seed:f32)->vec3f { let q=vec3i(i32(floor(p.x)),i32(floor(p.y)),i32(floor(p.z)));if(seed<1.0){let s=0xdeadbeefu+(4u<<2u)+13u;let m=mxBjMix(s+u32(q.x),s+u32(q.y),s+u32(q.z));return vec3f(f32(mxBjFinal(m.x,m.y,m.z))/4294967295.0,f32(mxBjFinal(m.x+1u,m.y,m.z))/4294967295.0,f32(mxBjFinal(m.x+2u,m.y,m.z))/4294967295.0);}let s=0xdeadbeefu+(5u<<2u)+13u;let m=mxBjMix(s+u32(q.x),s+u32(q.y),s+u32(q.z));let a=m.x+u32(i32(seed));return vec3f(f32(mxBjFinal(a,m.y,m.z))/4294967295.0,f32(mxBjFinal(a,m.y+1u,m.z))/4294967295.0,f32(mxBjFinal(a,m.y+2u,m.z))/4294967295.0); }
fn mxRotateFlake(p:vec3f,i:vec3f)->vec3f { let theta=6.283185307179586*i.x;let phi=6.283185307179586*i.y;let z=i.z*2.0;let r=sqrt(max(0.0,z));let vx=sin(phi)*r;let vy=cos(phi)*r;let vz=sqrt(max(0.0,2.0-z));let st=sin(theta);let ct=cos(theta);let sx=vx*ct-vy*st;let sy=vx*st+vy*ct;return vec3f((vx*sx-ct)*p.x+(vy*sx+st)*p.y+vz*sx*p.z,(vx*sy-st)*p.x+(vy*sy-ct)*p.y+vz*sy*p.z,vx*vz*p.x+vy*vz*p.y+(1.0-z)*p.z); }
fn mxFlakeDensityProbability(x0:f32)->f32 { let x=clamp(x0,0.0,1.0);let xx=x*x;return (-26.19771808*xx+26.39663835*x)/(85.53857017*xx*x-102.35069432*xx-101.42634862*x+118.45082288); }
struct MxFlakeData { id:i32, rand:f32, presence:f32, flakenormal:vec3f }
fn mxFlake(size0:f32,roughness:f32,coverage:f32,position:vec3f,normal0:vec3f,tangent:vec3f,bitangent:vec3f)->MxFlakeData {
  let normal=safeNormal(normal0,vec3f(0.0,0.0,1.0));let size=max(abs(size0),1e-6);let P=position/vec3f(size);let base=floor(P);let diameter=1.5/sqrt(3.0);let probability=mxFlakeDensityProbability(coverage);var priority=0.0;var cell=vec3f(0.0);
  for(var i=-1i;i<2i;i=i+1i){for(var j=-1i;j<2i;j=j+1i){for(var k=-1i;k<2i;k=k+1i){let cp=base+vec3f(f32(i),f32(j),f32(k));var pp=P-cp-vec3f(0.5);if(dot(pp,pp)>=diameter*diameter*3.0){continue;}if(mxCell3(cp)>probability){continue;}let p=mxCell4(vec4f(cp,3.0));if(p<priority){continue;}pp=mxRotateFlake(pp,mxFlakeCell3(cp,0.0));if(all(abs(pp)<=vec3f(diameter))){priority=p;cell=cp;}}}}
  if(priority<=0.0){return MxFlakeData(0,0.0,0.0,normal);}let noise=mxFlakeCell3(cell,2.0);let phi=6.283185307179586*noise.x;let xi=clamp(noise.y,0.0,0.999999);let tanTheta=max(0.0,roughness)*max(0.0,roughness)*sqrt(xi)/sqrt(max(1e-6,1.0-xi));let sinTheta=tanTheta/sqrt(1.0+tanTheta*tanTheta);let cosTheta=sqrt(max(0.0,1.0-sinTheta*sinTheta));let flake=safeNormal(tangent*cos(phi)*sinTheta+bitangent*sin(phi)*sinTheta+normal*cosTheta,normal);let rand=noise.z;return MxFlakeData(i32(rand*16777215.0),rand,priority,flake);
}
fn mxNoise2(p:vec2f)->f32 {
  let i=floor(p); let f=fract(p); let u=f*f*(vec2f(3.0)-2.0*f);
  let a=mxHash2(i); let b=mxHash2(i+vec2f(1,0)); let c=mxHash2(i+vec2f(0,1)); let d=mxHash2(i+vec2f(1,1));
  return mix(mix(a,b,u.x),mix(c,d,u.x),u.y);
}
fn mxNoise3(p:vec3f)->f32 {
  let i=floor(p); let f=fract(p); let u=f*f*(vec3f(3.0)-2.0*f);
  let c000=mxHash3(i); let c100=mxHash3(i+vec3f(1,0,0)); let c010=mxHash3(i+vec3f(0,1,0)); let c110=mxHash3(i+vec3f(1,1,0));
  let c001=mxHash3(i+vec3f(0,0,1)); let c101=mxHash3(i+vec3f(1,0,1)); let c011=mxHash3(i+vec3f(0,1,1)); let c111=mxHash3(i+vec3f(1,1,1));
  let x0=mix(mix(c000,c100,u.x),mix(c010,c110,u.x),u.y); let x1=mix(mix(c001,c101,u.x),mix(c011,c111,u.x),u.y);
  return mix(x0,x1,u.z);
}
fn mxFractal2(p:vec2f,octaves:i32,lacunarity:f32,diminish:f32)->f32 {
  var sum=0.0; var amplitude=1.0; var point=p;
  for(var i=0i;i<8i;i=i+1i){if(i<octaves){sum+=mxNoise2(point)*amplitude;point*=lacunarity;amplitude*=diminish;}}
  return sum;
}
fn mxFractal3(p:vec3f,octaves:i32,lacunarity:f32,diminish:f32)->f32 {
  var sum=0.0; var amplitude=1.0; var point=p;
  for(var i=0i;i<8i;i=i+1i){if(i<octaves){sum+=mxNoise3(point)*amplitude;point*=lacunarity;amplitude*=diminish;}}
  return sum;
}
fn mxWorley2(p:vec2f,jitter:f32,style:i32)->vec3f {
  let cell=floor(p); var best=1e6; var id=0.0;
  for(var y=-1i;y<=1i;y=y+1i){for(var x=-1i;x<=1i;x=x+1i){
    let c=cell+vec2f(f32(x),f32(y)); let h=vec2f(mxHash2(c+vec2f(17.0,31.0)),mxHash2(c+vec2f(47.0,73.0)));
    let d=length(p-(c+(h-0.5)*clamp(jitter,0.0,1.0))); if(d<best){best=d;id=mxHash2(c+vec2f(101.0,19.0));}
  }}
  let distanceValue=clamp(best,0.0,1.0); let solid=step(0.5,distanceValue);
  return vec3f(select(distanceValue,solid,style==1i),fract(id),fract(id*7.13));
}
fn mxWorley3(p:vec3f,jitter:f32,style:i32)->vec3f {
  let cell=floor(p); var best=1e6; var id=0.0;
  for(var z=-1i;z<=1i;z=z+1i){for(var y=-1i;y<=1i;y=y+1i){for(var x=-1i;x<=1i;x=x+1i){
    let c=cell+vec3f(f32(x),f32(y),f32(z)); let h=vec3f(mxHash3(c+vec3f(17.0,31.0,47.0)),mxHash3(c+vec3f(73.0,101.0,19.0)),mxHash3(c+vec3f(43.0,59.0,83.0)));
    let d=length(p-(c+(h-0.5)*clamp(jitter,0.0,1.0))); if(d<best){best=d;id=mxHash3(c+vec3f(107.0,127.0,149.0));}
  }}}
  let distanceValue=clamp(best,0.0,1.0); let solid=step(0.5,distanceValue);
  return vec3f(select(distanceValue,solid,style==1i),fract(id),fract(id*7.13));
}
fn safeNormal(v:vec3f,fallback:vec3f)->vec3f {
  let l2=dot(v,v); let valid=l2>1e-20 && all(v==v);
  return select(fallback,v*inverseSqrt(max(l2,1e-20)),valid);
}
fn mxSafeNormalize2(v:vec2f,fallback:vec2f)->vec2f {
  let l2=dot(v,v); let valid=l2>1e-20 && all(v==v);
  return select(fallback,v*inverseSqrt(max(l2,1e-20)),valid);
}
fn mxSafeNormalize3(v:vec3f,fallback:vec3f)->vec3f { return safeNormal(v,fallback); }
fn mxSafeNormalize4(v:vec4f,fallback:vec4f)->vec4f {
  let l2=dot(v,v); let valid=l2>1e-20 && all(v==v);
  return select(fallback,v*inverseSqrt(max(l2,1e-20)),valid);
}
fn mxNormalmap(value:vec3f,scale:vec2f,n:vec3f,t:vec3f,b:vec3f)->vec3f {
 let decoded=select(value*2.0-1.0,vec3f(0,0,1),dot(value,value)==0.0);
 return safeNormal(t*decoded.x*scale.x+b*decoded.y*scale.y+n*decoded.z,n);
}
fn mxSurfaceFrame(normal:vec3f,p1:vec3f,p2:vec3f,uv1:vec2f,uv2:vec2f)->mat3x3f {
  let n=safeNormal(normal,vec3f(0,0,1));
  let fallback=safeNormal(cross(select(vec3f(0,1,0),vec3f(1,0,0),abs(n.y)>.9),n),vec3f(1,0,0));
  let det=uv1.x*uv2.y-uv1.y*uv2.x;
  let uvAreaScale=max(length(uv1)*length(uv2),1e-30);
  if(abs(det)<=1e-7*uvAreaScale){return mat3x3f(fallback,cross(n,fallback),n);}
  // Only direction is needed; using det's sign avoids overflow at tiny UV scales.
  let du=(p1*uv2.y-p2*uv1.y)*sign(det);
  let dv=(p2*uv1.x-p1*uv2.x)*sign(det);
  let t=safeNormal(du-n*dot(n,du),fallback);
  let b=cross(n,t)*select(1.0,-1.0,dot(cross(n,t),dv)<0.0);
  return mat3x3f(t,b,n);
}
fn mxHeightToNormal(gradient:vec2f,du:vec2f,dv:vec2f,scale:f32)->vec3f {
  // MaterialX 1.39.5 mx_heighttonormal_vector3: Sobel parity scale and
  // encoded tangent-space output. Gradients are central UV differences.
  let h=gradient*(scale/16.0);
  var n=cross(vec3f(du,h.x),vec3f(dv,h.y));
  if(dot(n,n)<1e-16){n=vec3f(0,0,1);}else if(n.z<0.0){n=-n;}
  return normalize(n)*0.5+0.5;
}
fn mxBlackbody(k:f32)->vec3f {
  // MaterialX 1.39.5 pbrlib/genglsl/mx_blackbody.glsl (Apache-2.0):
  // Kang et al. chromaticity approximation, Y=1, linear Rec.709 output.
  let kelvin=clamp(k,800.0,25000.0);
  let t=1000.0/kelvin; let t2=t*t; let t3=t2*t;
  var x=-3.0258469*t3+2.1070379*t2+0.2226347*t+0.240390;
  if(kelvin<4000.0){x=-0.2661239*t3-0.2343580*t2+0.8776956*t+0.179910;}
  let x2=x*x; let x3=x2*x;
  var y=3.0817580*x3-5.87338670*x2+3.75112997*x-0.37001483;
  if(kelvin<2222.0){y=-1.1063814*x3-1.34811020*x2+2.18555832*x-0.20219683;}
  else if(kelvin<4000.0){y=-0.9549476*x3-1.37418593*x2+2.09137015*x-0.16748867;}
  if(y<=0.0){return vec3f(1.0);}
  let xyz=vec3f(x/y,1.0,(1.0-x-y)/y);
  return max(mat3x3f(vec3f(3.2406,-0.9689,0.0557),vec3f(-1.5372,1.8758,-0.2040),vec3f(-0.4986,0.0415,1.0570))*xyz,vec3f(0.0));
}
struct Lobe { base: vec3f, metal: f32, roughness: f32, ior: f32, transmission: f32, emission: vec3f, emissionWeight: f32, anisotropy: f32, transmissionColor: vec3f, kind:u32, weight:f32, alpha:vec2f, complexIOR:vec3f, extinction:vec3f, scatterMode:u32, thinWalled:u32, thinFilmThickness:f32, thinFilmIOR:f32, transmissionDepth:f32, transmissionScatter:vec3f, schlickColor82:vec3f, schlickColor90:vec3f, schlickExponent:f32, subsurfaceRadius:vec3f, specularColor:vec3f, specularColorEnabled:u32 }
struct Medium { absorption: vec3f, scattering: vec3f, anisotropy: f32, emission: vec3f }
fn mediumWithEmission(input:Medium,emission:vec3f)->Medium {var m=input;m.emission=emission;return m;}
fn makeMaterial(base:vec3f,metal:f32,rough:f32,ior:f32,trans:f32,emission:vec3f,emissionWeight:f32,anisotropy:f32,tint:vec3f,thinWalled:u32,thinFilmThickness:f32,thinFilmIOR:f32)->Lobe {
 return Lobe(base,metal,rough,ior,trans,emission,emissionWeight,anisotropy,tint,0u,1.0,vec2f(rough*rough),vec3f(ior),vec3f(0),3u,thinWalled,thinFilmThickness,thinFilmIOR,0.0,vec3f(0),vec3f(1),vec3f(1),5.0,vec3f(1),vec3f(0),0u);
}
fn withTransmission(lobe:Lobe,depth:f32,scatter:vec3f)->Lobe {var m=lobe;m.transmissionDepth=max(0.0,depth);m.transmissionScatter=max(vec3f(0),scatter);return m;}
fn withThinFilm(lobe:Lobe,thickness:f32,ior:f32)->Lobe {var m=lobe;m.thinFilmThickness=max(0.0,thickness);m.thinFilmIOR=max(1.0,ior);return m;}
fn withSpecular(lobe:Lobe,weight:f32)->Lobe {var m=lobe;m.weight=clamp(weight,0.0,1.0);return m;}
fn withSpecularColor(lobe:Lobe,color:vec3f)->Lobe {var m=lobe;m.schlickColor90=max(vec3f(0),color);return m;}
fn withSpecularColorMode(lobe:Lobe,color:vec3f,enabled:bool)->Lobe {var m=lobe;m.specularColor=max(vec3f(0),color);m.specularColorEnabled=select(0u,1u,enabled);return m;}
fn withSpecularColorTint(lobe:Lobe,color:vec3f)->Lobe {var m=lobe;let f0=mix(vec3f(pow((m.ior-1.0)/(m.ior+1.0),2.0)),m.base,m.metal);m.specularColor=clamp(f0*max(vec3f(0),color),vec3f(0),vec3f(1));m.specularColorEnabled=1u;return m;}
fn nativeDiffuse(color:vec3f,weight:f32,rough:f32)->Lobe {var m=makeMaterial(color,0,rough,1.5,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=3u;m.weight=weight;return m;}
fn nativeBurley(color:vec3f,weight:f32,rough:f32)->Lobe {var m=makeMaterial(color,0,rough,1.5,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=9u;m.weight=weight;return m;}
fn nativeSubsurface(color:vec3f,weight:f32,radius:vec3f,anisotropy:f32)->Lobe {var m=makeMaterial(color,0,1.0,1.3,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=6u;m.weight=weight;m.anisotropy=clamp(anisotropy,-.9,.9);m.subsurfaceRadius=max(vec3f(.02),radius);m.alpha=vec2f(max(.02,max(m.subsurfaceRadius.x,max(m.subsurfaceRadius.y,m.subsurfaceRadius.z))),max(.02,m.subsurfaceRadius.x));return m;}
fn nativeTranslucent(color:vec3f,weight:f32)->Lobe {var m=makeMaterial(color,0,1.0,1.0,1,vec3f(0),0,0,vec3f(1),1u,0.0,1.5);m.kind=7u;m.weight=weight;return m;}
fn nativeSheen(color:vec3f,weight:f32,roughness:f32,mode:u32)->Lobe {var m=makeMaterial(color,0,roughness,1.5,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=8u;m.weight=weight;m.alpha=vec2f(clamp(roughness,.01,1.0),0.0);m.scatterMode=mode;return m;}
fn nativeHair(color:vec3f,weight:f32,longitudinal:f32,azimuthal:f32,ior:f32)->Lobe {var m=makeMaterial(color,0,longitudinal,ior,0,vec3f(0),0,azimuthal,vec3f(1),0u,0.0,1.5);m.kind=4u;m.weight=weight;m.alpha=vec2f(max(.02,longitudinal),max(.02,azimuthal));return m;}
fn nativeGeneralizedSchlick(color0:vec3f,color82:vec3f,color90:vec3f,alpha:vec2f,weight:f32,exponent:f32,filmThickness:f32,filmIOR:f32)->Lobe {var m=makeMaterial(color0,0,sqrt(max(alpha.x,alpha.y)),1.5,0,vec3f(0),0,0,vec3f(1),0u,filmThickness,filmIOR);m.kind=5u;m.weight=weight;m.alpha=alpha;m.schlickColor82=max(vec3f(0),color82);m.schlickColor90=max(vec3f(0),color90);m.schlickExponent=max(.01,exponent);return m;}
fn nativeDielectric(tint:vec3f,ior:f32,alpha:vec2f,weight:f32,mode:u32)->Lobe {var m=makeMaterial(tint,0,sqrt(max(alpha.x,alpha.y)),ior,1,vec3f(0),0,0,tint,0u,0.0,1.5);m.kind=1u;m.weight=weight;m.alpha=alpha;m.scatterMode=mode;return m;}
fn nativeConductor(ior:vec3f,k:vec3f,alpha:vec2f,weight:f32)->Lobe {var m=makeMaterial(vec3f(1),1,sqrt(max(alpha.x,alpha.y)),1.5,0,vec3f(0),0,0,vec3f(1),0u,0.0,1.5);m.kind=2u;m.complexIOR=ior;m.extinction=k;m.alpha=alpha;m.weight=weight;return m;}
${closureTypesWGSL}`;
