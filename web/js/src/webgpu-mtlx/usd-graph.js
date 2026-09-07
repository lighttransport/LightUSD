// SPDX-License-Identifier: Apache-2.0
import { GraphError } from './graph.js';

const usdTypes = Object.freeze({ float: 'float', int: 'integer', bool: 'boolean',
  color3f: 'color3', color4f: 'color4', float2: 'vector2', float3: 'vector3',
  float4: 'vector4', vector3f: 'vector3', normal3f: 'vector3', point3f: 'vector3',
  matrix3d: 'matrix33', matrix4d: 'matrix44', asset: 'filename', string: 'string' });
const own = (object, key) => Object.hasOwn(object || {}, key);
const fail = (path, message) => { throw new GraphError('USD_GRAPH', path, message); };

/** Flatten reachable USD graph interfaces using exact library NodeDef IDs.
 * Asset resolution is caller-owned: a composed relative path has no reliable
 * source-layer anchor. This function never guesses one or fetches resources.
 */
export function materialXFromUSD(snapshot, materialPath, { library = {}, resolveAsset } = {}) {
  if (snapshot?.version !== 1 || !Array.isArray(snapshot.prims) || snapshot.prims.length > 100000) fail(materialPath, 'invalid or oversized snapshot');
  const prims = new Map(), nodes = [], built = new Map(), active = new Set(), building = new Set();
  for (const prim of snapshot.prims) {
    if (typeof prim.path !== 'string' || !prim.path.startsWith('/') || prims.has(prim.path)) fail(materialPath, 'invalid or duplicate prim path');
    prims.set(prim.path, prim);
  }
  const material = prims.get(materialPath);
  if (material?.type !== 'Material') fail(materialPath, 'expected Material');
  for (const name of ['outputs:displacement', 'outputs:mtlx:displacement', 'outputs:volume', 'outputs:mtlx:volume']) {
    const p = material.properties?.[name];
    if (p && (p.connections?.length || own(p, 'value') || p.timeSampled)) fail(`${materialPath}.${name}`, 'non-surface terminals are not yet translated');
  }
  const definitions = Object.create(null);
  // OpenUSD ColorSpaceAPI precedence; only the renderer's supported built-ins.
  // https://openusd.org/release/user_guides/color_user_guide.html
  function colorSpace(primPath, property) {
    let name = property.colorSpace;
    if (name === undefined || name === '') {
      for (let path = primPath; path; path = path.slice(0, path.lastIndexOf('/'))) {
        if (!own(snapshot.colorSpaces, path)) continue;
        const entry = snapshot.colorSpaces[path];
        if (entry.timeSampled || typeof entry.value !== 'string') fail(path, 'invalid or time-sampled color space');
        name = entry.value; if (name) break;
      }
    }
    const aliases = { lin_rec709_scene: 'lin_rec709', srgb_rec709_scene: 'srgb_texture', lin_ap1_scene: 'acescg',
      data: 'raw', raw: 'raw', lin_rec709: 'lin_rec709', srgb_texture: 'srgb_texture', acescg: 'acescg' };
    if (name === undefined || name === '') return 'lin_rec709';
    if (!own(aliases, name)) fail(primPath, `unsupported USD color space ${name}`);
    return aliases[name];
  }
  function definition(id, chain = new Set()) {
    if (own(definitions, id)) return definitions[id];
    if (!own(library.definitions, id)) fail(id, 'missing exact MaterialX NodeDef');
    if (chain.has(id) || chain.size >= 128) fail(id, 'NodeDef inheritance cycle or depth limit');
    chain.add(id);
    const d = library.definitions[id], parent = d.inherit ? definition(d.inherit, chain) : {};
    const result = { ...parent, ...d, inputs: { ...parent.inputs, ...d.inputs }, outputs: { ...parent.outputs, ...d.outputs } };
    chain.delete(id); definitions[id] = result; return result;
  }
  function split(path) {
    if (typeof path !== 'string') fail(materialPath, 'connection must be an absolute property path');
    const dot = path.lastIndexOf('.');
    if (!path.startsWith('/') || dot < 1) fail(path, 'connection must be an absolute property path');
    const prim = prims.get(path.slice(0, dot)), name = path.slice(dot + 1);
    if (!prim || !own(prim.properties, name)) fail(path, 'missing connected property');
    return { prim, name, property: prim.properties[name] };
  }
  function port(path, expected) {
    if (active.has(path) || active.size >= 256) fail(path, 'connection cycle or depth limit');
    active.add(path);
    try {
      const { prim, name, property: p } = split(path);
      if (p.timeSampled) fail(path, 'time-sampled shading is not yet evaluated');
      const actual = usdTypes[p.type];
      // USD token outputs carry shader/closure types through their NodeDef.
      if (actual && expected && actual !== expected) fail(path, `type mismatch ${actual} -> ${expected}`);
      if (!actual && p.type !== 'token') fail(path, `unsupported USD type ${p.type}`);
      if (!Array.isArray(p.connections) || p.connections.length > 1) fail(path, 'expected zero or one connection');
      if (p.connections.length) return port(p.connections[0], expected || actual);
      if (prim.type === 'Shader' && name.startsWith('outputs:')) {
        const node = shader(prim), output = name.slice(8), def = definition(node.nodedef);
        if (!own(def.outputs, output)) fail(path, 'output is absent from NodeDef');
        const type = def.outputs[output].type;
        if (expected && type !== expected) fail(path, `NodeDef output mismatch ${type} -> ${expected}`);
        return { nodename: node.name, output, type };
      }
      if (!own(p, 'value') || (p.value !== null && typeof p.value === 'object' && !Array.isArray(p.value))) fail(path, 'missing or unsupported default value');
      const type = expected || actual;
      if (!type) fail(path, 'ambiguous token value type');
      if (p.type === 'token' && type !== 'string') fail(path, 'token literals require a string NodeDef input');
      if (type === 'filename') {
        if (typeof resolveAsset !== 'function') fail(path, 'asset requires source-layer-aware resolution');
        const colorspace = colorSpace(prim.path, p);
        const value = resolveAsset(p.value, { primPath: prim.path, propertyPath: path, colorspace });
        if (typeof value !== 'string' || !value) fail(path, 'asset resolver must return a nonempty resource key');
        return { type, value };
      }
      return { type, value: p.value, ...(['color3', 'color4'].includes(type) ? { colorspace: colorSpace(prim.path, p) } : {}) };
    } finally { active.delete(path); }
  }
  function shader(prim) {
    if (building.has(prim.path)) fail(prim.path, 'shader dependency cycle');
    if (built.has(prim.path)) return built.get(prim.path);
    const id = prim.properties?.['info:id'];
    if (!id || id.timeSampled || id.connections?.length || typeof id.value !== 'string') fail(prim.path, 'expected static info:id');
    const def = definition(id.value);
    if (!def.node) fail(prim.path, 'NodeDef has no category');
    if (nodes.length >= 4096) fail(prim.path, 'reachable graph exceeds 4096 nodes');
    const outputs = Object.values(def.outputs || {});
    const type = def.type || (outputs.length === 1 ? outputs[0].type : 'multioutput');
    const node = { name: `usd_${nodes.length}`, category: def.node, nodedef: id.value, type, inputs: Object.create(null), source: prim.path };
    nodes.push(node); built.set(prim.path, node); building.add(prim.path);
    for (const name of Object.keys(prim.properties || {})) {
      if (!name.startsWith('inputs:')) continue;
      const input = name.slice(7);
      if (!own(def.inputs, input)) fail(`${prim.path}.${name}`, 'authored input is absent from NodeDef');
      node.inputs[input] = port(`${prim.path}.${name}`, def.inputs[input].type);
    }
    building.delete(prim.path); return node;
  }
  if (!own(material.properties, 'outputs:mtlx:surface')) fail(materialPath, 'missing MaterialX surface terminal');
  const output = port(`${materialPath}.outputs:mtlx:surface`, 'surfaceshader');
  return { version: '1.39', nodes, output, definitions, graphs: library.graphs || {},
    source: materialPath, provenance: { materialPath, source: 'USD layer snapshot', referenceReady: false } };
}
