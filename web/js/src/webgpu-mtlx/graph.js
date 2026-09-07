// SPDX-License-Identifier: Apache-2.0
// Typed, deterministic MaterialX value-graph compiler. No shader-source eval.
export const MATERIALX_VERSION = '1.39.5';
export class GraphError extends Error {
  constructor(code, path, message) { super(`${path}: ${message}`); this.name = 'GraphError'; this.code = code; this.path = path; }
}
const types = { float: 'f32', integer: 'i32', boolean: 'bool', color3: 'vec3f', color4: 'vec4f', vector2: 'vec2f', vector3: 'vec3f', vector4: 'vec4f', matrix33: 'mat3x3f', matrix44: 'mat4x4f' };
const widths = { float: 1, integer: 1, boolean: 1, color3: 3, vector3: 3, color4: 4, vector4: 4, vector2: 2, matrix33: 9, matrix44: 16 };
export const valueCategories = new Set(['constant', 'add', 'subtract', 'multiply', 'divide', 'modulo', 'power', 'min', 'max', 'absval', 'sign', 'floor', 'ceil', 'round', 'sqrt', 'ln', 'exp', 'sin', 'cos', 'tan', 'asin', 'acos', 'atan2', 'clamp', 'mix', 'smoothstep', 'invert', 'normalize', 'magnitude', 'dotproduct', 'crossproduct', 'texcoord', 'position', 'normal', 'tangent', 'bitangent', 'time', 'frame', 'convert', 'combine2', 'combine3', 'combine4', 'extract', 'swizzle', 'ifequal', 'ifgreater', 'ifgreatereq', 'remap', 'range', 'rotate2d', 'dot', 'separate2', 'separate3', 'separate4']);
const materialCategories = new Set(['standard_surface', 'open_pbr_surface', 'surfacematerial']);
function fail(code, path, message) { throw new GraphError(code, path, message); }
export function literal(type, value, path = '') {
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
export function parseMaterialX(xml, { source = '', parser = globalThis.DOMParser } = {}) {
  if (typeof xml !== 'string' || xml.length > 16 * 1024 * 1024) fail('LIMIT', source, 'XML exceeds 16 MiB');
  if (/<!DOCTYPE|<!ENTITY/i.test(xml)) fail('XML', source, 'DTD/entity declarations are not allowed');
  if (!parser) fail('XML', source, 'DOMParser is required for XML import');
  const doc = new parser().parseFromString(xml, 'application/xml');
  if (doc.querySelector('parsererror') || doc.documentElement.tagName !== 'materialx') fail('XML', source, 'invalid MaterialX XML');
  if (doc.querySelector('include')) fail('INCLUDE', source, 'resolve MaterialX includes before compilation');
  const result = { version: doc.documentElement.getAttribute('version'), nodes: [], graphs: Object.create(null), definitions: Object.create(null), source };
  function node(el) { return { ...attrs(el), category: el.tagName, inputs: ports(el), outputs: outputs(el) }; }
  for (const el of doc.documentElement.children) {
    if (el.tagName === 'nodedef') result.definitions[el.getAttribute('name')] = { ...attrs(el), inputs: ports(el), outputs: outputs(el) };
    else if (el.tagName === 'nodegraph') result.graphs[el.getAttribute('name')] = { ...attrs(el), inputs: ports(el), outputs: outputs(el), nodes: [...el.children].filter(c => !['input', 'output', 'token'].includes(c.tagName)).map(node) };
    else if (!['typedef', 'geompropdef', 'unittypedef', 'unitdef', 'implementation', 'look', 'collection', 'propertyset'].includes(el.tagName)) result.nodes.push(node(el));
  }
  return result;
}

/** Compile a normalized graph. Connections are {nodename, output} or {nodegraph, output}. */
export function compileGraph(document, { output, library = {}, material = false } = {}) {
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
    const matches = Object.values(definitions).filter(d => d.node === n.category && (!n.type || d.type === n.type || Object.values(d.outputs || {}).some(o => o.type === n.type)) && (!n.version || d.version === n.version));
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
    if (p.unit || p.colorspace) fail('SEMANTICS', path, 'unit/colorspace conversion is not implemented yet');
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
    } else result = { type: p.type || wanted, code: literal(p.type || wanted, p.value, path) };
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
    const type = declared?.type || n.outputs?.[out]?.type || n.type || def?.type;
    const ins = { ...def?.inputs, ...n.inputs };
    const compound = def && Object.values(graphs).find(g => g.nodedef === def.name);
    let result;
    if (compound) {
      const child = { ...compound, name: key };
      const bindings = Object.fromEntries(Object.entries(ins).map(([k, p]) => [k, { port: p, scope, env }]));
      result = port(compound.outputs[out], child, bindings, type, key);
    } else {
      const input = (k, fallback, expected) => {
        const p = ins[k] ?? (fallback !== undefined ? { type: expected || type, value: fallback } : null);
        return port(p, scope, env, expected, `${key}/${k}`);
      };
      const x = (k, fallback, expected) => input(k, fallback, expected).code;
      const same = k => x(k, undefined, type);
      const binary = op => `(${same('in1')} ${op} ${same('in2')})`;
      let code;
      switch (n.category) {
        case 'constant': code = same('value'); break;
        case 'add': code = binary('+'); break;
        case 'subtract': code = binary('-'); break;
        case 'multiply': case 'divide': {
          const a = input('in1', undefined, type), b = input('in2');
          if (b.type !== type && b.type !== 'float') fail('TYPE', key, 'invalid scalar/vector arithmetic');
          code = `(${a.code} ${n.category === 'multiply' ? '*' : '/'} ${b.code})`; break;
        }
        case 'modulo': code = `(${same('in1')} - ${same('in2')} * floor(${same('in1')} / ${same('in2')}))`; break;
        case 'power': case 'min': case 'max': code = `${n.category === 'power' ? 'pow' : n.category}(${same('in1')},${same('in2')})`; break;
        case 'absval': case 'sign': case 'floor': case 'ceil': case 'round': case 'sqrt': case 'ln': case 'exp': case 'sin': case 'cos': case 'tan': case 'asin': case 'acos': case 'normalize': code = `${({ absval: 'abs', ln: 'log' })[n.category] || n.category}(${same('in')})`; break;
        case 'atan2': code = `atan2(${same('iny')},${same('inx')})`; break;
        case 'clamp': code = `clamp(${same('in')},${x('low', undefined, type)},${x('high', undefined, type)})`; break;
        case 'mix': code = `mix(${same('bg')},${same('fg')},${x('mix')})`; break;
        case 'smoothstep': code = `smoothstep(${same('low')},${same('high')},${same('in')})`; break;
        case 'invert': code = `(${same('amount')} - ${same('in')})`; break;
        case 'dot': code = same('in'); break;
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
        case 'convert': code = `${types[type]}(${x('in')})`; break;
        case 'combine2': case 'combine3': case 'combine4': code = `${types[type]}(${Array.from({ length: Number(n.category.at(-1)) }, (_, i) => x(`in${i + 1}`, undefined, 'float')).join(',')})`; break;
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
          const q = `((${same('in')} - ${same('inlow')}) / (${same('inhigh')} - ${same('inlow')}))`;
          if (n.category === 'range') fail('UNSUPPORTED', key, 'range gamma/clamp semantics not implemented');
          code = `(${same('outlow')} + ${q} * (${same('outhigh')} - ${same('outlow')}))`; break;
        }
        case 'rotate2d': {
          const v = x('in', undefined, 'vector2'), a = `(${x('amount', undefined, 'float')} * 0.017453292519943295)`;
          code = `(mat2x2f(cos(${a}),sin(${a}),-sin(${a}),cos(${a})) * ${v})`; break;
        }
        case 'standard_surface': case 'open_pbr_surface': {
          if (!material) fail('CONTEXT', key, 'surface requires material compilation');
          const open = n.category === 'open_pbr_surface';
          const transmission = n.inputs?.[open ? 'transmission_weight' : 'transmission'];
          if (transmission && (transmission.nodename || transmission.nodegraph || Number(transmission.value) !== 0)) fail('UNSUPPORTED', key, 'transmission transport is not implemented yet');
          // This baseline mapping is explicitly approximate, not a reference closure.
          const fields = [x('base_color', [0.8, 0.8, 0.8], 'color3'), x(open ? 'base_metalness' : 'metalness', 0, 'float'), x('specular_roughness', 0.3, 'float'), x('specular_ior', 1.5, 'float'), x(open ? 'transmission_weight' : 'transmission', 0, 'float'), x('emission_color', [1, 1, 1], 'color3'), x(open ? 'emission_luminance' : 'emission', 0, 'float')];
          const supported = new Set(['base_color', 'base_metalness', 'metalness', 'specular_roughness', 'specular_ior', 'transmission_weight', 'transmission', 'emission_color', 'emission_luminance', 'emission']);
          for (const k of Object.keys(n.inputs || {})) if (!supported.has(k)) fail('UNSUPPORTED', `${key}/${k}`, 'surface input not yet implemented');
          code = `Material(${fields.join(',')})`; break;
        }
        case 'surfacematerial': result = input('surfaceshader'); break;
        default: fail('UNSUPPORTED', key, `node ${n.category} (${type}) is not implemented`);
      }
      if (!result) {
        const target = materialCategories.has(n.category) ? 'Material' : types[type];
        if (!target) fail('TYPE', key, `unsupported output type ${type}`);
        const id = `n${serial++}`;
        lines.push(`let ${id}: ${target} = ${code};`);
        result = { type, code: id };
      }
    }
    used.add(n.category); active.delete(key); cached.set(key, result); return result;
  }
  const selected = output || { nodename: document.nodes.at(-1)?.name };
  const value = port(selected, root, {}, undefined, '$output');
  return { body: lines.join('\n'), expression: value.code, type: value.type, categories: [...used].sort(), diagnostics: [], referenceReady: false };
}

export const contextWGSL = `struct ShadingContext { position: vec3f, normal: vec3f, tangent: vec3f, bitangent: vec3f, uv: vec2f, time: f32, frame: f32 }
struct Material { base: vec3f, metal: f32, roughness: f32, ior: f32, transmission: f32, emission: vec3f, emissionWeight: f32 }`;
