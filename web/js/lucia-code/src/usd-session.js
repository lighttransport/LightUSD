import { bytesToBase64, decoder, encoder, escapeRegExp, LuciaError, validIdentifier, validPrimPath } from './utils.js';
import { localizeUSDDependencies, repairInheritedMaterialBindings, validateUSDZArchive } from './usd-doctor.js';
import { findEquivalentMaterialMapping, foldLiteralUSDShaderNodes, mergeMaterialDefinitions, removeUnreachableMaterialShaders, rewriteMaterialBindings, rewriteMaterialCollectionBindings } from './material-repair.js';
import { translateAuthoredPBRProperties } from './material-translation.js';
import { hasInvalidValue, INDEXED_MESH_MAX_INDICES, isSupportedNumericArray, numericArrayKind, validateIndexedMesh } from './indexed-mesh.js';

const MAX_INPUT_MB = 256;
const SUBDIVISION_SCHEMES = new Set(['none', 'catmullClark', 'loop']);
const SUBDIVISION_METADATA = Object.freeze({ interpolateBoundary: new Set(['none', 'edgeAndCorner', 'edgeOnly', 'alwaysSharp']), faceVaryingLinearInterpolation: new Set(['none', 'cornersOnly', 'cornersPlus1', 'cornersPlus2', 'boundaries', 'all']), triangleSubdivisionRule: new Set(['catmullClark', 'smooth']) });
const MATERIAL_PARAMETER_INPUTS = Object.freeze({ baseColor: ['baseColor', 'base_color', 'diffuseColor', 'diffuse_color'], metallic: ['metallic', 'metalness'], roughness: ['roughness', 'specular_roughness'], opacity: ['opacity', 'alpha'], emissive: ['emissive', 'emissiveColor', 'emission_color', 'emissionColor'] });

export function normalizeSubdivisionScheme(value) {
  if (value == null) return null;
  const spelling = String(value).replace(/[._-]/g, '').toLowerCase();
  const scheme = spelling === 'catmullclark' ? 'catmullClark' : spelling === 'loop' ? 'loop' : spelling === 'none' ? 'none' : null;
  if (!scheme || !SUBDIVISION_SCHEMES.has(scheme)) throw new LuciaError('LUCIA_SUBDIVISION_SCHEME', `Unsupported subdivisionScheme "${value}".`);
  return scheme;
}

export function normalizeSubdivisionMetadata(name, value) {
  const allowed = SUBDIVISION_METADATA[name];
  if (!allowed) throw new LuciaError('LUCIA_SUBDIVISION_METADATA', `Unsupported subdivision metadata field "${name}".`);
  if (value == null) return null;
  const spelling = String(value).replace(/[._-]/g, '').toLowerCase(), normalized = [...allowed].find((token) => token.replace(/[._-]/g, '').toLowerCase() === spelling);
  if (!normalized) throw new LuciaError('LUCIA_SUBDIVISION_METADATA', `Unsupported ${name} "${value}".`);
  return normalized;
}

function validateMeshAttribute(values, expectedLength, name, integer = false) {
  if (values == null) return;
  let valid = values.length === expectedLength;
  if (valid) for (let i = 0; i < values.length; i += 1) {
    const value = values[i];
    if (!Number.isFinite(value) || integer && !Number.isInteger(value)) { valid = false; break; }
  }
  if (!valid) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', `${name} must contain exactly ${expectedLength} finite${integer ? ' integer' : ''} components.`);
}

function validateCustomMeshAttribute(attribute, label, expectedLength = null) {
  let finite = Boolean(attribute?.array?.length);
  if (finite) finite = !hasInvalidValue(attribute.array, (value) => !Number.isFinite(value));
  if (!attribute || !/^[A-Za-z_][\w:]*$/.test(attribute.name || '') || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || !isSupportedNumericArray(attribute.array) || !finite || expectedLength != null && attribute.array.length !== expectedLength * attribute.itemSize) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', `${label} must use a supported numeric typed array and contain finite, vertex/corner aligned values.`);
  return true;
}

function validateAttributeInterpolation(attribute, expected, label) {
  if (attribute?.interpolation != null && attribute.interpolation !== expected) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', `${label} currently supports only ${expected} interpolation.`);
}

export function normalizeInterpolationToken(value, fallback = 'vertex') {
  if (value == null) return fallback;
  const token = String(value).replace(/[._-]/g, '').toLowerCase();
  if (token === 'vertex') return 'vertex';
  if (token === 'facevarying') return 'faceVarying';
  if (token === 'constant') return 'constant';
  if (token === 'uniform') return 'uniform';
  if (token === 'varying') return 'varying';
  if (token === 'instance') return 'instance';
  throw new LuciaError('LUCIA_MESH_ATTRIBUTE', `Unsupported interpolation token "${value}".`);
}

export function normalizeMeshNormals(values) {
  if (values == null) return null;
  const normalized = new Float32Array(values.length);
  for (let vertex = 0; vertex < values.length / 3; vertex++) {
    const offset = vertex * 3, x = Number(values[offset]), y = Number(values[offset + 1]), z = Number(values[offset + 2]), length = Math.hypot(x, y, z);
    if (!Number.isFinite(length) || length <= 1e-12) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Normals must contain non-zero finite vectors before USD authoring.');
    normalized[offset] = x / length; normalized[offset + 1] = y / length; normalized[offset + 2] = z / length;
  }
  return normalized;
}

function setDisplayColorText(source, path, value) {
  const block = findPrimBlock(source, path);
  if (!block) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Prim not found: ${path}`, { path });
  const body = source.slice(block.start, block.end);
  const line = /(^|\n)([ \t]*)(?:uniform\s+|varying\s+)?color3f\[\]\s+primvars:displayColor\s*=\s*\[[\s\S]*?\](?:\s*\([^\n{}]*\))?/m;
  const replacement = `$1$2color3f[] primvars:displayColor = ${value}`;
  const nextBody = line.test(body) ? body.replace(line, replacement) : `${body}\n        color3f[] primvars:displayColor = ${value}\n    `;
  return source.slice(0, block.start) + nextBody + source.slice(block.end);
}

function removeDisplayColorIndicesText(source, path) {
  const block = findPrimBlock(source, path);
  if (!block) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Prim not found: ${path}`, { path });
  const body = source.slice(block.start, block.end);
  const nextBody = body.replace(/\s*(?:uniform\s+)?int\[\]\s+primvars:displayColor:indices\s*=\s*\[[\s\S]*?\]/, '');
  return source.slice(0, block.start) + nextBody + source.slice(block.end);
}

function normalizeSharpEdges(edges, vertexCount) {
  if (edges == null) return null;
  if (!Array.isArray(edges)) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', 'Sharp edges must be an array of vertex pairs.');
  const unique = new Map();
  for (const edge of edges) {
    if (!Array.isArray(edge) || edge.length !== 2 || !edge.every((value) => Number.isSafeInteger(value)) || edge[0] < 0 || edge[1] < 0 || edge[0] >= vertexCount || edge[1] >= vertexCount || edge[0] === edge[1]) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', 'Sharp edges must contain distinct, in-range vertex pairs.');
    const pair = edge[0] < edge[1] ? edge : [edge[1], edge[0]], key = `${pair[0]}:${pair[1]}`;
    unique.set(key, pair);
  }
  return [...unique.values()];
}

function parseNativeResult(raw) {
  let result;
  try { result = JSON.parse(raw); } catch { throw new LuciaError('LUCIA_NATIVE_RESPONSE', 'LightUSD returned an invalid response.'); }
  if (result?.isError || result?.error) {
    const text = result.error || result.content?.map((v) => v.text).join('\n') || 'Native operation failed.';
    throw new LuciaError('LUCIA_NATIVE_TOOL', text);
  }
  return result;
}

function findPrimBlock(source, path) {
  if (!validPrimPath(path) || path === '/') return null;
  const names = path.split('/').filter(Boolean);
  let range = { start: 0, end: source.length };
  let declarationStart = -1, declarationEnd = -1;
  for (const name of names) {
    const segment = source.slice(range.start, range.end);
    const re = new RegExp(`\\b(?:def|over|class)\\s+(?:[A-Za-z_][\\w:]*)?\\s*"${escapeRegExp(name)}"\\s*(?:\\([^)]*\\)\\s*)?\\{`, 'g');
    const match = re.exec(segment);
    if (!match) return null;
    declarationStart = range.start + match.index;
    const open = declarationStart + match[0].lastIndexOf('{');
    declarationEnd = open;
    let depth = 1;
    let quote = false;
    let escaped = false;
    let close = -1;
    for (let i = open + 1; i < range.end; i++) {
      const ch = source[i];
      if (quote) {
        if (escaped) escaped = false;
        else if (ch === '\\') escaped = true;
        else if (ch === '"') quote = false;
        continue;
      }
      if (ch === '"') quote = true;
      else if (ch === '{') depth++;
      else if (ch === '}' && --depth === 0) { close = i; break; }
    }
    if (close < 0) return null;
    range = { start: open + 1, end: close, open, close };
  }
  return { ...range, declarationStart, declarationEnd };
}

export function wireMaterialPrimvarParameters(source, materialPaths, channels) {
  if (!Array.isArray(materialPaths) || !materialPaths.length || materialPaths.some((path) => !validPrimPath(path))) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', 'Material primvar wiring requires absolute Material paths.');
  if (!Array.isArray(channels) || !channels.length || channels.some((channel) => !MATERIAL_PARAMETER_INPUTS[channel])) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', 'Material primvar wiring requires known PBR channels.');
  let output = source;
  for (const materialPath of [...new Set(materialPaths)].sort()) {
    const material = findPrimBlock(output, materialPath);
    if (!material) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', `Material prim not found: ${materialPath}`);
    const materialBody = output.slice(material.open + 1, material.close), surfaceConnection = materialBody.match(/\boutputs:surface\.connect\s*=\s*<([^>]+)>/);
    if (!surfaceConnection) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', `Material has no resolvable surface output: ${materialPath}`);
    const reference = surfaceConnection[1].split('.outputs:')[0], shaderPath = reference.startsWith('/') ? reference : `${materialPath}/${reference.replace(/^\.\//, '')}`, shader = findPrimBlock(output, shaderPath);
    if (!shader || shader.start <= material.open || shader.close >= material.close) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', `Material surface shader could not be resolved: ${materialPath}`);
    let shaderBody = output.slice(shader.open + 1, shader.close), readers = '';
    for (const channel of [...new Set(channels)].sort()) {
      const inputNames = MATERIAL_PARAMETER_INPUTS[channel], input = inputNames.map((name) => ({ name, expression: new RegExp(`((?:uniform\\s+)?(?:float|color3f|float3|vector3f)\\s+inputs:${name}\\s*=\\s*)(<[^>]+>|\\([^)]*\\)|"[^"]*"|[^\\s;{}]+)`) })).map(({ name, expression }) => ({ name, match: expression.exec(shaderBody) })).find(({ match }) => match);
      if (!input) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION', `Surface shader has no supported ${channel} input: ${materialPath}`);
      const readerType = ['baseColor', 'emissive'].includes(channel) ? 'color3f' : 'float', readerId = `LuciaPrimvar_${channel}`, existing = new RegExp(`\\b(?:def|over|class)\\s+Shader\\s+"${readerId}(?:_[0-9]+)?"`).test(materialBody), readerName = existing ? `${readerId}_1` : readerId;
      shaderBody = shaderBody.slice(0, input.match.index) + input.match[1] + `<./${readerName}.outputs:result>` + shaderBody.slice(input.match.index + input.match[0].length);
      readers += `\n        def Shader "${readerName}" {\n            token info:id = "UsdPrimvarReader_${readerType}"\n            token inputs:varname = "lucia:${channel}"\n            ${readerType} outputs:result\n        }`;
    }
    output = output.slice(0, shader.open + 1) + shaderBody + output.slice(shader.close);
    const refreshedMaterial = findPrimBlock(output, materialPath);
    output = output.slice(0, refreshedMaterial.close) + readers + output.slice(refreshedMaterial.close);
  }
  return output;
}

function isMaterialPrim(source, path) {
  const block = findPrimBlock(source, path);
  if (!block) return false;
  return /\b(?:def|over|class)\s+Material\s+"/.test(source.slice(block.declarationStart, block.declarationEnd));
}

function setAttributeText(source, path, declaration, value) {
  const block = findPrimBlock(source, path);
  if (!block) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Prim not found: ${path}`, { path });
  const name = declaration.trim().split(/\s+/).at(-1);
  const body = source.slice(block.start, block.end);
  const line = new RegExp(`(^|\\n)([ \\t]*)(?:uniform\\s+|varying\\s+)?[\\w:\\[\\]]+\\s+${escapeRegExp(name)}\\s*=\\s*[^\\n}]+`, 'm');
  const replacement = `$1$2${declaration} = ${value}`;
  const nextBody = line.test(body) ? body.replace(line, replacement) : `${body}\n        ${declaration} = ${value}\n    `;
  return source.slice(0, block.start) + nextBody + source.slice(block.end);
}

function removeAttributeText(source, path, declaration) {
  const block = findPrimBlock(source, path);
  if (!block) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Prim not found: ${path}`, { path });
  const name = declaration.trim().split(/\s+/).at(-1), body = source.slice(block.start, block.end);
  const line = new RegExp(`(^|\\n|[ \\t])(?:uniform\\s+|varying\\s+)?[\\w:\\[\\]]+\\s+${escapeRegExp(name)}\\s*=\\s*[^\\n}]+`, 'm');
  if (!line.test(body)) return source;
  return source.slice(0, block.start) + body.replace(line, '$1') + source.slice(block.end);
}

function findGeomSubsetPaths(source, meshPath) {
  const mesh = findPrimBlock(source, meshPath);
  if (!mesh) return [];
  const body = source.slice(mesh.start, mesh.end), paths = [];
  const re = /\b(?:def|over|class)\s+GeomSubset\s+"([A-Za-z_][\w]*)"\s*(?:\([^)]*\)\s*)?\{/g;
  let match;
  while ((match = re.exec(body))) paths.push(`${meshPath}/${match[1]}`);
  return paths;
}

function findFaceVaryingPrimvarNames(source, path) {
  const block = findPrimBlock(source, path);
  if (!block) return [];
  const names = [], body = source.slice(block.start, block.end), re = /(?:uniform\s+|varying\s+)?(?:float|float2|float3|float4|int|int2|int3|int4|color3f|texCoord2f|normal3f)\[\]\s+primvars:([A-Za-z_][\w:]*)\s*=\s*\[[\s\S]*?\]\s*\(\s*interpolation\s*=\s*"faceVarying"/g;
  let match;
  while ((match = re.exec(body))) names.push(match[1]);
  return [...new Set(names)];
}

function parseIntegerArrayProperty(source, path, property) {
  const block = findPrimBlock(source, path);
  if (!block) return null;
  const body = source.slice(block.start, block.end), match = new RegExp(`(?:uniform\\s+)?int\\[\\]\\s+${escapeRegExp(property)}\\s*=\\s*\\[([^\\]]*)\\]`).exec(body);
  if (!match) return null;
  const text = match[1].trim();
  if (!text) return [];
  const tokens = text.split(',').map((value) => value.trim());
  if (tokens.some((value) => !/^-?\d+$/.test(value))) return null;
  const values = tokens.map((value) => Number(value));
  return values.every(Number.isSafeInteger) ? values : null;
}

function parseFloatArrayProperty(source, path, property) {
  const block = findPrimBlock(source, path);
  if (!block) return null;
  const body = source.slice(block.start, block.end), match = new RegExp(`(?:uniform\\s+)?float\\[\\]\\s+${escapeRegExp(property)}\\s*=\\s*\\[([^\\]]*)\\]`).exec(body);
  if (!match) return null;
  const text = match[1].trim();
  if (!text) return [];
  const tokens = text.split(',').map((value) => value.trim());
  if (tokens.some((value) => !/^[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?$/.test(value))) return null;
  const values = tokens.map(Number);
  return values.every(Number.isFinite) ? values : null;
}

function parseTupleArrayProperty(source, path, type, property) {
  const block = findPrimBlock(source, path);
  if (!block) return null;
  const body = source.slice(block.start, block.end), match = new RegExp(`(?:uniform\\s+|varying\\s+)?${escapeRegExp(type)}\\[\\]\\s+${escapeRegExp(property)}\\s*=\\s*\\[([\\s\\S]*?)\\]`).exec(body);
  if (!match) return null;
  const values = [], tuplePattern = /\(\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*,\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*\)/g;
  let tuple;
  while ((tuple = tuplePattern.exec(match[1]))) values.push(Number(tuple[1]), Number(tuple[2]));
  return values.length && values.every(Number.isFinite) ? new Float32Array(values) : null;
}

function parseFaceVaryingPrimvars(source, path, indexCount) {
  const block = findPrimBlock(source, path);
  if (!block) return [];
  if (!Number.isSafeInteger(indexCount) || indexCount < 0 || indexCount > INDEXED_MESH_MAX_INDICES) throw new LuciaError('LUCIA_CLEANUP_FACEVARYING', 'Face-varying cleanup data exceeds the indexed mesh safety limit.');
  const body = source.slice(block.start, block.end), typeSizes = { float: 1, float2: 2, float3: 3, float4: 4, int: 1, int2: 2, int3: 3, int4: 4, color3f: 3, texCoord2f: 2, normal3f: 3 }, attributes = [];
  const declaration = /(?:uniform\s+|varying\s+)?(float|float2|float3|float4|int|int2|int3|int4|color3f|texCoord2f|normal3f)\[\]\s+primvars:([A-Za-z_][\w:]*)\s*=\s*\[([\s\S]*?)\]\s*\(\s*interpolation\s*=\s*"faceVarying"/g;
  let match;
  while ((match = declaration.exec(body))) {
    const name = match[2];
    if (name === 'st' || name === 'st1') continue;
    const itemSize = typeSizes[match[1]], values = [], scalarPattern = /[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?/g;
    let value;
    while ((value = scalarPattern.exec(match[3]))) values.push(Number(value[0]));
    const indices = parseIntegerArrayProperty(source, path, `primvars:${name}:indices`);
    if (!values.length || values.length % itemSize || !indices || indices.length !== indexCount || !values.every(Number.isFinite) || !indices.every((index) => Number.isSafeInteger(index) && index >= 0 && index < values.length / itemSize)) throw new LuciaError('LUCIA_CLEANUP_FACEVARYING', `Face-varying primvar ${name} has malformed values or corner indices.`);
    attributes.push({ name, itemSize, array: new (match[1].startsWith('int') ? Int32Array : Float32Array)(values), indices: Uint32Array.from(indices), interpolation: 'faceVarying' });
  }
  return attributes;
}

export class LuciaUsdSession extends EventTarget {
  constructor() {
    super();
    this.module = null;
    this.author = null;
    this.render = null;
    this.sessionId = `lucia-${crypto.randomUUID()}`;
    this.usda = '';
    this.filename = 'untitled.usda';
  }

  async init() {
    const factory = (await import('../../src/lightusd/lightusd.js')).default;
    this.module = await factory();
    this.author = new this.module.LightUSDLoaderNative();
    this.render = new this.module.LightUSDLoaderNative();
    if (!this.author.mcpCreateContext(this.sessionId)) throw new LuciaError('LUCIA_INIT', 'Could not create the Lucia USD session.');
    this.author.mcpSelectContext(this.sessionId);
  }

  call(tool, args = {}) {
    return parseNativeResult(this.author.mcpToolsCall(tool, JSON.stringify(args)));
  }

  async loadBytes(bytes, filename = 'scene.usda') {
    const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (u8.byteLength > MAX_INPUT_MB * 1024 * 1024) throw new LuciaError('LUCIA_PARSE_MEMORY', `Input exceeds the ${MAX_INPUT_MB} MB browser limit.`);
    const format = String(filename).toLowerCase().endsWith('.usda') ? 'usda' : String(filename).toLowerCase().endsWith('.usdc') ? 'usdc' : 'auto';
    this.call('stage_load_data', { data: bytesToBase64(u8), name: filename, format });
    this.filename = filename;
    this.usda = this.call('stage_to_string').usda;
    await this.rebuildRender();
    this.dispatchEvent(new CustomEvent('stagechange'));
  }

  async loadUSDA(source, filename = 'scene.usda') {
    await this.loadBytes(encoder.encode(source), filename);
  }

  async rebuildRender(onProgress) {
    if (this.render) this.render.delete();
    this.render = new this.module.LightUSDLoaderNative();
    this.render.setMaxMemoryLimitMB(MAX_INPUT_MB);
    const ok = this.render.loadFromBinary(encoder.encode(this.usda), this.filename);
    if (!ok) throw new LuciaError('LUCIA_RENDER_CONVERT', this.render.error() || 'Could not build the preview scene.');
    onProgress?.({ stage: 'preview', percentage: 100, message: 'Preview ready' });
  }

  async replaceUSDA(source, summary = 'Edit scene') {
    const previous = this.usda;
    try {
      this.call('stage_load_data', { data: bytesToBase64(encoder.encode(source)), name: this.filename, format: 'usda' });
      this.usda = this.call('stage_to_string').usda;
      await this.rebuildRender();
      this.dispatchEvent(new CustomEvent('stagechange', { detail: { summary } }));
      return previous;
    } catch (error) {
      this.call('stage_load_data', { data: bytesToBase64(encoder.encode(previous)), name: this.filename, format: 'usda' });
      this.usda = previous;
      throw error;
    }
  }

  async nativeMutation(tool, args, summary) {
    const previous = this.usda;
    this.call(tool, args);
    this.usda = this.call('stage_to_string').usda;
    await this.rebuildRender();
    this.dispatchEvent(new CustomEvent('stagechange', { detail: { summary } }));
    return previous;
  }

  async restore(source) { await this.replaceUSDA(source, 'Restore scene'); }
  tree() { return this.call('prim_list', { path: '/', max_depth: -1, include_attributes: false }).prims || []; }
  prim(path) { return this.call('prim_get', { path, include_attributes: true, include_metadata: true }).prim; }

  async renamePrim(path, newName) {
    if (!validIdentifier(newName)) throw new LuciaError('LUCIA_INVALID_IDENTIFIER', 'USD names must start with a letter or underscore and contain only letters, digits, and underscores.');
    return this.nativeMutation('prim_rename', { path, new_name: newName }, `Rename ${path} to ${newName}`);
  }

  async deletePrim(path) {
    if (path === '/' || path === '/World') throw new LuciaError('LUCIA_ROOT_DELETE', 'The stage root cannot be deleted.');
    return this.nativeMutation('prim_remove', { path }, `Delete ${path}`);
  }

  async createPrim(parent, type, name) {
    if (!validIdentifier(name)) throw new LuciaError('LUCIA_INVALID_IDENTIFIER', 'Enter a valid USD identifier.');
    const path = `${parent === '/' ? '' : parent}/${name}`;
    return this.nativeMutation('prim_create', { path, type_name: type, specifier: 'def' }, `Create ${type} ${path}`);
  }

  async setTransform(path, transform) {
    let source = this.usda;
    const vec = (v) => `(${v.map((n) => Number(n).toFixed(6).replace(/\.?0+$/, '') || '0').join(', ')})`;
    source = setAttributeText(source, path, 'double3 xformOp:translate', vec(transform.translate));
    source = setAttributeText(source, path, 'float3 xformOp:rotateXYZ', vec(transform.rotate));
    source = setAttributeText(source, path, 'float3 xformOp:scale', vec(transform.scale));
    source = setAttributeText(source, path, 'uniform token[] xformOpOrder', '["xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"]');
    return this.replaceUSDA(source, `Transform ${path}`);
  }

  async setVisibility(path, visible) {
    return this.replaceUSDA(setAttributeText(this.usda, path, 'token visibility', visible ? '"inherited"' : '"invisible"'), `${visible ? 'Show' : 'Hide'} ${path}`);
  }

  async setMaterialVariants(path, variants, variantSet = 'luciaMaterial') {
    if (!validIdentifier(variantSet) || !Array.isArray(variants) || variants.length < 2 || variants.some((variant) => !validIdentifier(variant?.name) || !validPrimPath(variant?.materialPath) || variant.materialPath === '/') || new Set(variants.map((variant) => variant.name)).size !== variants.length || new Set(variants.map((variant) => variant.materialPath)).size !== variants.length) throw new LuciaError('LUCIA_MATERIAL_VARIANT', 'Material variants require at least two unique variant names and Material prim paths.');
    const block = findPrimBlock(this.usda, path);
    if (!block) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Prim not found: ${path}`, { path });
    const body = this.usda.slice(block.start, block.end);
    if (new RegExp(`\\bvariantSet\\s+"${escapeRegExp(variantSet)}"`).test(body)) throw new LuciaError('LUCIA_MATERIAL_VARIANT_COLLISION', `Variant set already exists on ${path}: ${variantSet}`);
    for (const variant of variants) if (!isMaterialPrim(this.usda, variant.materialPath)) throw new LuciaError('LUCIA_MATERIAL_VARIANT_PATH', `Material prim not found: ${variant.materialPath}`);
    const subsetPaths = findGeomSubsetPaths(this.usda, path), variantBinding = (variant) => subsetPaths.length ? subsetPaths.map((subsetPath) => `                over "${subsetPath.split('/').at(-1)}" {\n                    rel material:binding = <${variant.materialPath}>\n                }`).join('\n') : `                rel material:binding = <${variant.materialPath}>`;
    const variantBody = `\n        variantSet "${variantSet}" = {\n${variants.map((variant) => `            "${variant.name}" {\n${variantBinding(variant)}\n            }`).join('\n')}\n        }\n    `;
    let source = this.usda.slice(0, block.end) + variantBody + this.usda.slice(block.end);
    const declaration = source.slice(block.declarationStart, block.declarationEnd), selection = `variants = { string "${variantSet}" = "${variants[0].name}" }`;
    const variantMetadata = /(?<prefix>\bvariants\s*=\s*\{)(?<entries>[\s\S]*?)(?<close>\})/.exec(declaration);
    if (variantMetadata) {
      if (new RegExp(`\\b${escapeRegExp(variantSet)}\\b`).test(variantMetadata.groups.entries)) throw new LuciaError('LUCIA_MATERIAL_VARIANT_COLLISION', `Variant selection already exists on ${path}: ${variantSet}`);
      const insert = block.declarationStart + variantMetadata.index + variantMetadata[0].length - 1;
      source = source.slice(0, insert) + `\n            string ${variantSet} = "${variants[0].name}"` + source.slice(insert);
    } else {
      const close = declaration.search(/\s*$/) >= 0 ? (() => { let index = block.declarationEnd - 1; while (index >= block.declarationStart && /\s/.test(source[index])) index--; return source[index] === ')' ? index : -1; })() : -1;
      if (close >= 0) source = source.slice(0, close) + `\n        ${selection}\n    ` + source.slice(close);
      else source = source.slice(0, block.declarationEnd) + ` (\n        ${selection}\n    )` + source.slice(block.declarationEnd);
    }
    return this.replaceUSDA(source, `Author ${variantSet} material variants on ${path}`);
  }

  async wireMaterialPrimvarParameters(materialPaths, channels) {
    const source = wireMaterialPrimvarParameters(this.usda, materialPaths, channels);
    if (source === this.usda) throw new LuciaError('LUCIA_MATERIAL_PARAMETERIZATION_NOOP', 'Material primvar readers are already authored.');
    return this.replaceUSDA(source, `Wire material primvar parameter${channels.length === 1 ? '' : 's'}`);
  }

  async rewriteMaterialBindings(mapping) {
    const result = rewriteMaterialBindings(this.usda, mapping);
    if (!result.changed) throw new LuciaError('LUCIA_MATERIAL_BINDING_NOOP', 'No exact material bindings matched the requested rewrite map.');
    return this.replaceUSDA(result.source, `Merge ${result.changed} equivalent material binding${result.changed === 1 ? '' : 's'}`);
  }

  async rewriteMaterialCollectionBindings(mapping) {
    const result = rewriteMaterialCollectionBindings(this.usda, mapping);
    if (!result.changed) throw new LuciaError('LUCIA_COLLECTION_BINDING_NOOP', 'No exact collection material bindings matched the requested rewrite map.');
    return this.replaceUSDA(result.source, `Repair ${result.changed} collection material binding${result.changed === 1 ? '' : 's'}`);
  }

  async repairInheritedMaterialBindings(repairs) {
    const result = repairInheritedMaterialBindings(this.usda, repairs);
    if (!result.changed) throw new LuciaError('LUCIA_INHERITED_BINDING_NOOP', 'All reviewed inherited material bindings already have the requested direct binding.');
    return this.replaceUSDA(result.source, `Repair ${result.changed} inherited material binding${result.changed === 1 ? '' : 's'}`);
  }

  async mergeMaterialDefinitions(mapping) {
    const result = mergeMaterialDefinitions(this.usda, mapping);
    if (!result.removed) throw new LuciaError('LUCIA_MATERIAL_MERGE_NOOP', 'No duplicate material definitions were removed.');
    return this.replaceUSDA(result.source, `Merge ${result.removed} duplicate material definition${result.removed === 1 ? '' : 's'}`);
  }

  previewEquivalentMaterialDefinitions() {
    const mapping = findEquivalentMaterialMapping(this.usda);
    const candidates = Object.entries(mapping)
      .sort(([fromA], [fromB]) => fromA.localeCompare(fromB))
      .map(([from, to]) => ({ from, to }));
    return { mapping, candidates };
  }

  async mergeEquivalentMaterialDefinitions() {
    const preview = this.previewEquivalentMaterialDefinitions();
    if (!preview.candidates.length) throw new LuciaError('LUCIA_MATERIAL_MERGE_NOOP', 'No structurally identical material definitions were found.');
    return this.mergeMaterialDefinitions(preview.mapping);
  }

  async removeUnreachableMaterialShaders(path) {
    const result = removeUnreachableMaterialShaders(this.usda, path);
    if (!result.changed) throw new LuciaError('LUCIA_MATERIAL_GRAPH_NOOP', 'No unreachable nested Shader prims were found for the selected Material.');
    return this.replaceUSDA(result.source, `Remove ${result.removed.length} unreachable material shader${result.removed.length === 1 ? '' : 's'}`);
  }

  async optimizeMaterialGraph(path) {
    const folded = foldLiteralUSDShaderNodes(this.usda, path);
    let result = folded, source = folded.source;
    try {
      const cleaned = removeUnreachableMaterialShaders(source, path);
      if (cleaned.changed) result = { ...result, source: cleaned.source, removed: [...new Set([...(result.removed || []), ...cleaned.removed])] };
    } catch (error) { if (error?.code !== 'LUCIA_MATERIAL_GRAPH_PATH') throw error; }
    if (!result.changed && !(result.removed || []).length) throw new LuciaError('LUCIA_MATERIAL_GRAPH_NOOP', 'No safe constant-folding or unreachable material shader cleanup was found.');
    return this.replaceUSDA(result.source, `Optimize material graph ${path}`);
  }

  async translateMaterialShader(path, from, to) {
    const block = findPrimBlock(this.usda, path);
    if (!block) throw new LuciaError('LUCIA_MATERIAL_SHADER_PATH', `Material shader prim not found: ${path}`);
    const body = this.usda.slice(block.open + 1, block.close - 1), result = translateAuthoredPBRProperties(body, { from, to });
    if (!result.changed) throw new LuciaError('LUCIA_MATERIAL_TRANSLATION_NOOP', result.skipped.length ? `Target shader inputs already exist; no authored inputs were changed: ${path}` : `No recognized authored PBR inputs were found: ${path}`);
    const source = this.usda.slice(0, block.open + 1) + result.body + this.usda.slice(block.close - 1);
    return this.replaceUSDA(source, `Translate material shader ${path} to ${result.to}`);
  }

  getBoundMaterialShaderPath(meshPath) {
    const mesh = findPrimBlock(this.usda, meshPath);
    if (!mesh) return null;
    const meshBody = this.usda.slice(mesh.open + 1, mesh.close - 1), binding = meshBody.match(/\bmaterial:binding\s*=\s*<([^>]+)>/);
    if (!binding) return null;
    const materialPath = binding[1].split('.').at(0), material = findPrimBlock(this.usda, materialPath);
    if (!material) return null;
    const body = this.usda.slice(material.open + 1, material.close - 1), connection = body.match(/\boutputs:surface\.connect\s*=\s*<([^>]+)>/);
    return connection ? connection[1].replace(/\.outputs:surface$/, '') : null;
  }

  async localizeDependencies(mapping) {
    const result = localizeUSDDependencies(this.usda, mapping);
    if (!result.changed) throw new LuciaError('LUCIA_DEPENDENCY_LOCALIZATION_NOOP', 'No exact USD asset references matched the requested localization map.');
    return this.replaceUSDA(result.source, `Localize ${result.changed} USD dependenc${result.changed === 1 ? 'y' : 'ies'}`);
  }

  async setDisplayColor(path, rgb) {
    if (!rgb || typeof rgb.length !== 'number' || rgb.length < 3 || ![0, 1, 2].every((index) => Number.isFinite(Number(rgb[index])))) throw new LuciaError('LUCIA_DISPLAY_COLOR', 'Display color must contain three finite numeric components.');
    const value = `[(${[0, 1, 2].map((index) => Math.max(0, Math.min(1, Number(rgb[index]))).toFixed(4)).join(', ')})] ( interpolation = "constant" )`;
    let source = setDisplayColorText(this.usda, path, value);
    source = removeDisplayColorIndicesText(source, path);
    return this.replaceUSDA(source, `Set color on ${path}`);
  }

  async setMeshGeometry(path, data, summary = 'Update mesh geometry') {
    if (data?.uvSet != null && !['default', 'lightmap'].includes(data.uvSet)) throw new LuciaError('LUCIA_UV_SET', `Unsupported UV set: ${data.uvSet}`);
    try { data = { ...data, indices: validateIndexedMesh({ positions: data?.positions, indices: data?.indices || null }).indices, subdivisionScheme: normalizeSubdivisionScheme(data?.subdivisionScheme), ...Object.fromEntries(Object.keys(SUBDIVISION_METADATA).map((name) => [name, normalizeSubdivisionMetadata(name, data?.[name])])) }; } catch (error) { throw error.code === 'LUCIA_SUBDIVISION_SCHEME' || error.code === 'LUCIA_SUBDIVISION_METADATA' ? error : new LuciaError('LUCIA_MESH_AUTHORING', `Mesh authoring received invalid indexed data: ${error.message}`); }
    const vertexCount = data.positions.length / 3, sharpEdges = normalizeSharpEdges(data.sharpEdges, vertexCount);
    if (sharpEdges) data.sharpEdges = sharpEdges;
    if (data.sharpChains != null && sharpEdges != null) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', 'Provide sharpChains or sharpEdges, not both.');
    let sharpChains = null;
    if (data.sharpChains != null) {
      if (!Array.isArray(data.sharpChains) || data.sharpChains.some((chain) => !Array.isArray(chain) || chain.length < 2 || chain.some((value, index) => !Number.isSafeInteger(value) || value < 0 || value >= vertexCount || index > 0 && value === chain[index - 1]))) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', 'sharpChains must contain chains of at least two distinct, in-range integer vertices.');
      sharpChains = data.sharpChains.map((chain) => [...chain]);
    }
    let sharpEdgeSharpness = null;
    if (data.sharpEdgeSharpness != null) {
      if (!sharpEdges || !Array.isArray(data.sharpEdgeSharpness) || data.sharpEdgeSharpness.length !== sharpEdges.length || data.sharpEdgeSharpness.some((value) => !Number.isFinite(Number(value)) || Number(value) <= 0)) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', 'sharpEdgeSharpness must contain one positive finite value per unique sharp edge.');
      sharpEdgeSharpness = data.sharpEdgeSharpness.map(Number);
    }
    let sharpChainSharpness = null;
    if (data.sharpChainSharpness != null) {
      if (!sharpChains || !Array.isArray(data.sharpChainSharpness) || data.sharpChainSharpness.length !== sharpChains.length || data.sharpChainSharpness.some((value) => !Number.isFinite(Number(value)) || Number(value) <= 0)) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', 'sharpChainSharpness must contain one positive finite value per sharp chain.');
      sharpChainSharpness = data.sharpChainSharpness.map(Number);
    }
    if (data.uvIndices == null) validateMeshAttribute(data.uvs, vertexCount * 2, 'UVs');
    else { if (!data.uvs || data.uvs.length < 2 || data.uvs.length % 2 || hasInvalidValue(data.uvs, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Face-varying UVs must contain finite two-component values.'); }
    const normalIndices = data.normalIndices;
    if (normalIndices == null) validateMeshAttribute(data.normals, vertexCount * 3, 'Normals');
    else {
      if (!isSupportedNumericArray(data.normals) || data.normals.length < 3 || data.normals.length % 3 || hasInvalidValue(data.normals, (value) => !Number.isFinite(value))) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Face-varying normals must contain finite three-component values.');
      if (!isSupportedNumericArray(normalIndices) || normalIndices.length !== data.indices.length || hasInvalidValue(normalIndices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= data.normals.length / 3)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Normal indices must align with face corners and reference the normal buffer.');
    }
    const normalizedNormals = normalizeMeshNormals(data.normals);
    validateMeshAttribute(data.colors, vertexCount * 3, 'Colors');
    validateMeshAttribute(data.tangents, vertexCount * 4, 'Tangents');
    validateMeshAttribute(data.jointIndices, vertexCount * 4, 'Joint indices', true);
    validateMeshAttribute(data.jointWeights, vertexCount * 4, 'Joint weights');
    if (Boolean(data.jointIndices) !== Boolean(data.jointWeights)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Joint indices and weights must be provided together.');
    if (data.jointIndices && hasInvalidValue(data.jointIndices, (value) => value < 0 || value > 65535)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Joint indices must fit in unsigned 16-bit values.');
    if (data.jointWeights && hasInvalidValue(data.jointWeights, (value) => value < 0)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Joint weights must be non-negative.');
    if (data.uvIndices != null) { if (!data.uvs || data.uvIndices.length !== data.indices.length || hasInvalidValue(data.uvIndices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= data.uvs.length / 2)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'UV indices must align with face corners and reference the UV buffer.'); }
    const normalizeAttributeInterpolation = (attribute, expected, label) => { try { return { ...attribute, interpolation: normalizeInterpolationToken(attribute?.interpolation, expected) }; } catch (error) { throw new LuciaError('LUCIA_MESH_ATTRIBUTE', `${label} currently supports only ${expected} interpolation.`); } };
    data.customAttributes = (data.customAttributes || []).map((attribute) => normalizeAttributeInterpolation(attribute, 'vertex', 'Custom attributes'));
    data.faceVaryingAttributes = (data.faceVaryingAttributes || []).map((attribute) => normalizeAttributeInterpolation(attribute, 'faceVarying', 'Face-varying attributes'));
    const authoredAttributeNames = new Set(['st', 'st1', 'displayColor', 'tangents', 'skel:jointIndices', 'skel:jointWeights']);
    for (const attribute of data.customAttributes) { validateCustomMeshAttribute(attribute, 'Custom attributes', vertexCount); validateAttributeInterpolation(attribute, 'vertex', 'Custom attributes'); if (authoredAttributeNames.has(attribute.name)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', `Mesh attribute name is reserved or duplicated: ${attribute.name}`); authoredAttributeNames.add(attribute.name); }
    for (const attribute of data.faceVaryingAttributes) { if (attribute.name === 'st1') { if (data.faceVaryingAttributes.filter((candidate) => candidate.name === 'st1').length !== 1 || !attribute.array || attribute.itemSize !== 2 || attribute.array.length % 2 || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value)) || !attribute.indices || attribute.indices.length !== data.indices.length || hasInvalidValue(attribute.indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= attribute.array.length / 2)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Lightmap UVs must contain finite two-component values and one valid index per face corner.'); continue; } validateCustomMeshAttribute(attribute, 'Face-varying attributes'); validateAttributeInterpolation(attribute, 'faceVarying', 'Face-varying attributes'); if (authoredAttributeNames.has(attribute.name)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', `Mesh attribute name is reserved or duplicated: ${attribute.name}`); authoredAttributeNames.add(attribute.name); if (!attribute.indices || attribute.indices.length !== data.indices.length || hasInvalidValue(attribute.indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= attribute.array.length / attribute.itemSize)) throw new LuciaError('LUCIA_MESH_ATTRIBUTE', 'Face-varying attribute indices must align with face corners and reference the value buffer.'); }
    let source = this.usda;
    const points = `[${Array.from({ length: data.positions.length / 3 }, (_, i) => `(${data.positions[i * 3]}, ${data.positions[i * 3 + 1]}, ${data.positions[i * 3 + 2]})`).join(', ')}]`;
    const indices = `[${Array.from(data.indices).join(', ')}]`;
    const counts = `[${Array.from({ length: data.indices.length / 3 }, () => 3).join(', ')}]`;
    source = setAttributeText(source, path, 'point3f[] points', points);
    source = setAttributeText(source, path, 'int[] faceVertexIndices', indices);
    source = setAttributeText(source, path, 'int[] faceVertexCounts', counts);
    if (data.subdivisionScheme) source = setAttributeText(source, path, 'token subdivisionScheme', `"${data.subdivisionScheme}"`);
    for (const name of Object.keys(SUBDIVISION_METADATA)) if (data[name]) source = setAttributeText(source, path, `token ${name}`, `"${data[name]}"`);
    if (sharpChains || sharpEdges) {
      const chains = sharpChains || sharpEdges.map((edge) => edge), sharpness = sharpChainSharpness || sharpEdgeSharpness || chains.map(() => 1);
      source = setAttributeText(source, path, 'int[] creaseIndices', `[${chains.flat().join(', ')}]`);
      source = setAttributeText(source, path, 'int[] creaseLengths', `[${chains.map((chain) => chain.length).join(', ')}]`);
      source = setAttributeText(source, path, 'float[] creaseSharpness', `[${sharpness.join(', ')}]`);
    }
    const uvDeclaration = data.uvSet === 'lightmap' ? 'texCoord2f[] primvars:st1' : 'texCoord2f[] primvars:st';
    if (data.uvs?.length && data.uvIndices?.length === data.indices.length) {
      const uvs = `[${Array.from({ length: data.uvIndices.length }, (_, i) => { const uvIndex = data.uvIndices[i] * 2; return `(${data.uvs[uvIndex]}, ${data.uvs[uvIndex + 1]})`; }).join(', ')}]`;
      source = setAttributeText(source, path, uvDeclaration, `${uvs} ( interpolation = "faceVarying" )`);
      source = setAttributeText(source, path, `${data.uvSet === 'lightmap' ? 'int[] primvars:st1:indices' : 'int[] primvars:st:indices'}`, `[${Array.from(data.uvIndices).join(', ')}]`);
    } else if (data.uvs?.length === data.positions.length / 3 * 2) {
      const uvs = `[${Array.from({ length: data.uvs.length / 2 }, (_, i) => `(${data.uvs[i * 2]}, ${data.uvs[i * 2 + 1]})`).join(', ')}]`;
      source = setAttributeText(source, path, uvDeclaration, `${uvs} ( interpolation = "vertex" )`);
      source = removeAttributeText(source, path, `${data.uvSet === 'lightmap' ? 'int[] primvars:st1:indices' : 'int[] primvars:st:indices'}`);
    }
    const lightmapAttribute = (data.faceVaryingAttributes || []).find((attribute) => attribute.name === 'st1');
    if (lightmapAttribute) {
      const values = `[${Array.from({ length: lightmapAttribute.array.length / 2 }, (_, i) => `(${lightmapAttribute.array[i * 2]}, ${lightmapAttribute.array[i * 2 + 1]})`).join(', ')}]`;
      source = setAttributeText(source, path, 'texCoord2f[] primvars:st1', `${values} ( interpolation = "faceVarying" )`);
      source = setAttributeText(source, path, 'int[] primvars:st1:indices', `[${Array.from(lightmapAttribute.indices).join(', ')}]`);
    }
    if (normalIndices != null) {
      const normals = `[${Array.from({ length: normalizedNormals.length / 3 }, (_, i) => `(${normalizedNormals[i * 3]}, ${normalizedNormals[i * 3 + 1]}, ${normalizedNormals[i * 3 + 2]})`).join(', ')}]`;
      source = setAttributeText(source, path, 'normal3f[] normals', `${normals} ( interpolation = "faceVarying" )`);
      source = setAttributeText(source, path, 'int[] normals:indices', `[${Array.from(normalIndices).join(', ')}]`);
    } else if (normalizedNormals?.length === data.positions.length) {
      const normals = `[${Array.from({ length: normalizedNormals.length / 3 }, (_, i) => `(${normalizedNormals[i * 3]}, ${normalizedNormals[i * 3 + 1]}, ${normalizedNormals[i * 3 + 2]})`).join(', ')}]`;
      source = setAttributeText(source, path, 'normal3f[] normals', `${normals} ( interpolation = "vertex" )`);
      source = removeAttributeText(source, path, 'int[] normals:indices');
    }
    if (data.colors?.length === data.positions.length) {
      const colors = `[${Array.from({ length: data.colors.length / 3 }, (_, i) => `(${data.colors[i * 3]}, ${data.colors[i * 3 + 1]}, ${data.colors[i * 3 + 2]})`).join(', ')}]`;
      source = setAttributeText(source, path, 'color3f[] primvars:displayColor', `${colors} ( interpolation = "vertex" )`);
      source = removeAttributeText(source, path, 'int[] primvars:displayColor:indices');
    }
    if (data.jointIndices?.length === data.positions.length / 3 * 4) {
      const jointIndices = `[${Array.from(data.jointIndices).join(', ')}]`;
      source = setAttributeText(source, path, 'int[] primvars:skel:jointIndices', `${jointIndices} ( interpolation = "vertex" )`);
      source = removeAttributeText(source, path, 'int[] primvars:skel:jointIndices:indices');
    }
    if (data.jointWeights?.length === data.positions.length / 3 * 4) {
      const jointWeights = `[${Array.from(data.jointWeights).join(', ')}]`;
      source = setAttributeText(source, path, 'float[] primvars:skel:jointWeights', `${jointWeights} ( interpolation = "vertex" )`);
      source = removeAttributeText(source, path, 'int[] primvars:skel:jointWeights:indices');
    }
    if (data.tangents?.length === data.positions.length / 3 * 4) {
      const tangents = `[${Array.from({ length: data.tangents.length / 4 }, (_, i) => `(${data.tangents[i * 4]}, ${data.tangents[i * 4 + 1]}, ${data.tangents[i * 4 + 2]})`).join(', ')}]`;
      source = setAttributeText(source, path, 'float3[] primvars:tangents', `${tangents} ( interpolation = "vertex" )`);
      source = removeAttributeText(source, path, 'int[] primvars:tangents:indices');
    } else source = removeAttributeText(source, path, 'float3[] primvars:tangents');
    if (data.clearMissingAttributes) {
      if (!data.uvs) for (const declaration of ['texCoord2f[] primvars:st', 'int[] primvars:st:indices', 'texCoord2f[] primvars:st1', 'int[] primvars:st1:indices']) source = removeAttributeText(source, path, declaration);
      if (!data.normals) for (const declaration of ['normal3f[] normals', 'int[] normals:indices']) source = removeAttributeText(source, path, declaration);
      if (!data.colors) for (const declaration of ['color3f[] primvars:displayColor', 'int[] primvars:displayColor:indices']) source = removeAttributeText(source, path, declaration);
      if (!data.jointIndices) for (const declaration of ['int[] primvars:skel:jointIndices', 'int[] primvars:skel:jointIndices:indices']) source = removeAttributeText(source, path, declaration);
      if (!data.jointWeights) for (const declaration of ['float[] primvars:skel:jointWeights', 'int[] primvars:skel:jointWeights:indices']) source = removeAttributeText(source, path, declaration);
      const retainedFaceVarying = new Set((data.faceVaryingAttributes || []).map((attribute) => attribute?.name));
      for (const name of findFaceVaryingPrimvarNames(source, path)) if (!retainedFaceVarying.has(name) && name !== 'st' && name !== 'st1') for (const declaration of [`float[] primvars:${name}`, `float2[] primvars:${name}`, `float3[] primvars:${name}`, `float4[] primvars:${name}`, `int[] primvars:${name}`, `int2[] primvars:${name}`, `int3[] primvars:${name}`, `int4[] primvars:${name}`, `color3f[] primvars:${name}`, `texCoord2f[] primvars:${name}`, `normal3f[] primvars:${name}`, `int[] primvars:${name}:indices`]) source = removeAttributeText(source, path, declaration);
      if (Array.isArray(data.customAttributeNames)) {
        const retained = new Set((data.customAttributes || []).map((attribute) => attribute?.name));
        for (const name of data.customAttributeNames) if (/^[A-Za-z_][\w:]*$/.test(name) && !retained.has(name)) for (const declaration of [`float[] primvars:${name}`, `float2[] primvars:${name}`, `float3[] primvars:${name}`, `float4[] primvars:${name}`, `int[] primvars:${name}`, `int2[] primvars:${name}`, `int3[] primvars:${name}`, `int4[] primvars:${name}`, `int[] primvars:${name}:indices`]) source = removeAttributeText(source, path, declaration);
      }
    }
    for (const attribute of data.customAttributes || []) {
      if (!/^[A-Za-z_][\w:]*$/.test(attribute.name) || !attribute.array?.length || attribute.array.length !== data.positions.length / 3 * attribute.itemSize) continue;
      const kind = numericArrayKind(attribute.array), type = kind ? (attribute.itemSize === 1 ? `${kind}[]` : `${kind}${attribute.itemSize}[]`) : null;
      if (!type) continue;
      const values = attribute.itemSize === 1 ? `[${Array.from(attribute.array).join(', ')}]` : `[${Array.from({ length: attribute.array.length / attribute.itemSize }, (_, i) => `(${Array.from({ length: attribute.itemSize }, (_, c) => attribute.array[i * attribute.itemSize + c]).join(', ')})`).join(', ')}]`;
      source = setAttributeText(source, path, `${type} primvars:${attribute.name}`, `${values} ( interpolation = "vertex" )`);
      source = removeAttributeText(source, path, `int[] primvars:${attribute.name}:indices`);
    }
    for (const attribute of data.faceVaryingAttributes || []) {
      if (attribute.name === 'st1') continue;
      if (!/^[A-Za-z_][\w:]*$/.test(attribute.name) || !attribute.array?.length || !attribute.indices?.length || attribute.indices.length !== data.indices.length || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length % attribute.itemSize) continue;
      const kind = numericArrayKind(attribute.array), type = kind ? (attribute.itemSize === 1 ? `${kind}[]` : `${kind}${attribute.itemSize}[]`) : null;
      if (!type || hasInvalidValue(attribute.indices, (index) => !Number.isInteger(index) || index < 0 || index >= attribute.array.length / attribute.itemSize) || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) continue;
      const values = attribute.itemSize === 1 ? `[${Array.from(attribute.array).join(', ')}]` : `[${Array.from({ length: attribute.array.length / attribute.itemSize }, (_, i) => `(${Array.from({ length: attribute.itemSize }, (_, c) => attribute.array[i * attribute.itemSize + c]).join(', ')})`).join(', ')}]`;
      source = setAttributeText(source, path, `${type} primvars:${attribute.name}`, `${values} ( interpolation = "faceVarying" )`);
      source = setAttributeText(source, path, `int[] primvars:${attribute.name}:indices`, `[${Array.from(attribute.indices).join(', ')}]`);
    }
    if (Array.isArray(data.groups) && data.groups.length) {
      const subsetPaths = findGeomSubsetPaths(source, path);
      const maxMaterialIndex = data.groups.reduce((max, group) => Math.max(max, Number.isInteger(group?.materialIndex) ? group.materialIndex : -1), -1);
      if (subsetPaths.length && maxMaterialIndex >= subsetPaths.length) throw new LuciaError('LUCIA_MATERIAL_GROUPS', 'Cleaned material groups do not match the mesh GeomSubset bindings.');
      for (let subsetIndex = 0; subsetIndex < subsetPaths.length; subsetIndex++) {
        const indices = [];
        for (const group of data.groups) if (group?.materialIndex === subsetIndex && Number.isInteger(group.start) && Number.isInteger(group.count)) for (let index = group.start / 3; index < (group.start + group.count) / 3; index++) indices.push(index);
        source = setAttributeText(source, subsetPaths[subsetIndex], 'int[] indices', `[${indices.join(', ')}]`);
      }
    } else {
      // A cleanup can legitimately remove all material groups. Clear authored
      // subset indices so they cannot continue to describe the old topology.
      for (const subsetPath of findGeomSubsetPaths(source, path)) source = setAttributeText(source, subsetPath, 'int[] indices', '[]');
    }
    return this.replaceUSDA(source, summary);
  }

  async setMeshGeometrySibling(path, name, data, summary = 'Create mesh LOD') {
    if (!validIdentifier(name)) throw new LuciaError('LUCIA_INVALID_IDENTIFIER', 'LOD names must be valid USD identifiers.');
    const block = findPrimBlock(this.usda, path);
    if (!block) return [];
    const parent = path.slice(0, path.lastIndexOf('/')) || '/', siblingPath = `${parent === '/' ? '' : parent}/${name}`;
    if (findPrimBlock(this.usda, siblingPath)) throw new LuciaError('LUCIA_LOD_COLLISION', `LOD prim already exists: ${siblingPath}`);
    let start = this.usda.lastIndexOf('\n', block.open) + 1;
    if (/^\s*\{$/.test(this.usda.slice(start, block.open + 1))) start = this.usda.lastIndexOf('\n', start - 2) + 1;
    const end = block.close + 1, originalBlock = this.usda.slice(start, end), leaf = path.split('/').at(-1);
    const declaration = new RegExp(`(\\b(?:def|over|class)\\s+(?:[A-Za-z_][\\w:]*)?\\s*")${escapeRegExp(leaf)}("\\s*\\{)`).exec(originalBlock);
    if (!declaration) throw new LuciaError('LUCIA_LOD_SOURCE', `Could not clone mesh declaration: ${path}`);
    const clone = originalBlock.slice(0, declaration.index) + declaration[1] + name + declaration[2] + originalBlock.slice(declaration.index + declaration[0].length);
    const original = this.usda, clonedSource = original.slice(0, end) + `\n${clone}` + original.slice(end);
    this.usda = clonedSource;
    try {
      await this.setMeshGeometry(siblingPath, data, summary);
      return original;
    } catch (error) {
      this.usda = original;
      throw error;
    }
  }

  async setMeshGeometryChild(path, name, data, summary = 'Create child mesh') {
    if (!validIdentifier(name)) throw new LuciaError('LUCIA_INVALID_IDENTIFIER', 'Child mesh names must be valid USD identifiers.');
    const block = findPrimBlock(this.usda, path);
    if (!block) return [];
    const childPath = `${path}/${name}`;
    if (findPrimBlock(this.usda, childPath)) throw new LuciaError('LUCIA_CHILD_COLLISION', `Child mesh already exists: ${childPath}`);
    let start = this.usda.lastIndexOf('\n', block.open) + 1;
    if (/^\s*\{$/.test(this.usda.slice(start, block.open + 1))) start = this.usda.lastIndexOf('\n', start - 2) + 1;
    const originalBlock = this.usda.slice(start, block.close + 1), leaf = path.split('/').at(-1), declaration = new RegExp(`(\\b(?:def|over|class)\\s+(?:[A-Za-z_][\\w:]*)?\\s*")${escapeRegExp(leaf)}("\\s*\\{)`).exec(originalBlock);
    if (!declaration) throw new LuciaError('LUCIA_CHILD_SOURCE', `Could not clone mesh declaration: ${path}`);
    const clone = originalBlock.slice(0, declaration.index) + declaration[1] + name + declaration[2] + originalBlock.slice(declaration.index + declaration[0].length), original = this.usda;
    this.usda = original.slice(0, block.close) + `\n${clone}\n` + original.slice(block.close);
    try { await this.setMeshGeometry(childPath, data, summary); return original; }
    catch (error) { this.usda = original; throw error; }
  }

  async createInstanceSibling(sourcePath, name, summary = 'Create USD instance', transformSourcePath = null) {
    if (!validPrimPath(sourcePath) || sourcePath === '/' || !validIdentifier(name)) throw new LuciaError('LUCIA_INSTANCE_INPUT', 'Instances require a valid source prim path and identifier.');
    if (transformSourcePath != null && (!validPrimPath(transformSourcePath) || transformSourcePath === '/')) throw new LuciaError('LUCIA_INSTANCE_INPUT', 'The optional instance transform source must be a valid prim path.');
    const sourceBlock = findPrimBlock(this.usda, sourcePath), transformBlock = transformSourcePath ? findPrimBlock(this.usda, transformSourcePath) : null, parent = sourcePath.slice(0, sourcePath.lastIndexOf('/')) || '/', parentBlock = parent === '/' ? null : findPrimBlock(this.usda, parent);
    if (!sourceBlock || parent !== '/' && !parentBlock) throw new LuciaError('LUCIA_INSTANCE_PATH', `Could not find the instance source or parent: ${sourcePath}.`);
    if (transformSourcePath && !transformBlock) throw new LuciaError('LUCIA_INSTANCE_TRANSFORM', `Could not find the transform source: ${transformSourcePath}.`);
    const instancePath = `${parent === '/' ? '' : parent}/${name}`;
    if (findPrimBlock(this.usda, instancePath)) throw new LuciaError('LUCIA_INSTANCE_COLLISION', `Instance prim already exists: ${instancePath}`);
    let declarationStart = this.usda.lastIndexOf('\n', sourceBlock.open) + 1;
    if (/^\s*\{$/.test(this.usda.slice(declarationStart, sourceBlock.open + 1))) declarationStart = this.usda.lastIndexOf('\n', declarationStart - 2) + 1;
    const sourceDeclaration = this.usda.slice(declarationStart, sourceBlock.open).match(/\b(?:def|over|class)\s+([A-Za-z_][\w:]*)/);
    if (!sourceDeclaration) throw new LuciaError('LUCIA_INSTANCE_SOURCE', `Could not determine the source prim type: ${sourcePath}.`);
    const transformLines = transformBlock ? this.usda.slice(transformBlock.start, transformBlock.end).split('\n').map((line) => line.trim()).filter((line) => /\bxformOp(?::|Order\b)/.test(line)) : [], indent = parent === '/' ? '    ' : ' '.repeat(Math.max(4, (this.usda.slice(0, parentBlock.close).split('\n').at(-1)?.match(/^\s*/)?.[0].length || 0) + 4)), block = `\n${indent}def ${sourceDeclaration[1]} "${name}" (\n${indent}    instanceable = true\n${indent}    references = <${sourcePath}>\n${indent}) {${transformLines.length ? `\n${transformLines.map((line) => `${indent}    ${line}`).join('\n')}\n${indent}` : ' '}}\n`, original = this.usda, insertion = parent === '/' ? sourceBlock.close + 1 : parentBlock.close;
    this.usda = original.slice(0, insertion) + block + original.slice(insertion);
    try { await this.replaceUSDA(this.usda, summary); return original; } catch (error) { this.usda = original; throw error; }
  }

  async createGuideMeshSibling(path, name, data, summary = 'Create guide mesh', approximation = 'convexHull') {
    if (!validIdentifier(name)) throw new LuciaError('LUCIA_INVALID_IDENTIFIER', 'Guide mesh names must be valid USD identifiers.');
    const parent = path.slice(0, path.lastIndexOf('/')) || '/', parentBlock = findPrimBlock(this.usda, parent);
    if (!parentBlock) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Parent prim not found: ${parent}`, { path: parent });
    const siblingPath = `${parent === '/' ? '' : parent}/${name}`;
    if (findPrimBlock(this.usda, siblingPath)) throw new LuciaError('LUCIA_GUIDE_COLLISION', `Guide prim already exists: ${siblingPath}`);
    const indent = ' '.repeat(Math.max(4, (this.usda.slice(0, parentBlock.close).split('\n').at(-1)?.match(/^\s*/)?.[0].length || 0) + 4));
    if (!['none', 'convexHull', 'convexDecomposition', 'boundingSphere', 'boundingCube', 'meshSimplification'].includes(approximation)) throw new LuciaError('LUCIA_COLLIDER_APPROXIMATION', `Unsupported collider approximation: ${approximation}`);
    const block = `\n${indent}def Mesh "${name}" (\n${indent}    apiSchemas = ["PhysicsCollisionAPI", "PhysicsMeshCollisionAPI"]\n${indent}) {\n${indent}    token purpose = "guide"\n${indent}    uniform token physics:approximation = "${approximation}"\n${indent}}\n`;
    const original = this.usda;
    this.usda = original.slice(0, parentBlock.close) + block + original.slice(parentBlock.close);
    try { await this.setMeshGeometry(siblingPath, data, summary); return original; } catch (error) { this.usda = original; throw error; }
  }

  async createCollisionGroup(parentPath, name, colliderPaths = [], filteredGroupPaths = [], summary = 'Create collision group', { mergeGroup = '', invertFilteredGroups = false } = {}) {
    if (!validPrimPath(parentPath) || !validIdentifier(name)) throw new LuciaError('LUCIA_COLLISION_GROUP_PATH', 'Collision groups require a valid parent path and identifier.');
    if (!Array.isArray(colliderPaths) || !Array.isArray(filteredGroupPaths) || [...colliderPaths, ...filteredGroupPaths].some((path) => !validPrimPath(path))) throw new LuciaError('LUCIA_COLLISION_GROUP_TARGET', 'Collision-group targets must be valid USD prim paths.');
    const parentBlock = findPrimBlock(this.usda, parentPath);
    if (!parentBlock) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Parent prim not found: ${parentPath}`, { path: parentPath });
    const groupPath = `${parentPath === '/' ? '' : parentPath}/${name}`;
    if (findPrimBlock(this.usda, groupPath)) throw new LuciaError('LUCIA_COLLISION_GROUP_COLLISION', `Collision group already exists: ${groupPath}`);
    const unique = (paths) => [...new Set(paths)];
    const includes = unique(colliderPaths), filters = unique(filteredGroupPaths), indent = ' '.repeat(Math.max(4, (this.usda.slice(0, parentBlock.close).split('\n').at(-1)?.match(/^\s*/)?.[0].length || 0) + 4));
    const targets = includes.length ? `\n${indent}    rel collection:colliders:includes = [${includes.map((path) => `<${path}>`).join(', ')}]` : '';
    const filtered = filters.length ? `\n${indent}    rel physics:filteredGroups = [${filters.map((path) => `<${path}>`).join(', ')}]` : '';
    if (typeof mergeGroup !== 'string' || mergeGroup && !validIdentifier(mergeGroup) || typeof invertFilteredGroups !== 'boolean') throw new LuciaError('LUCIA_COLLISION_GROUP_METADATA', 'Collision-group mergeGroup must be an identifier and invertFilteredGroups must be boolean.');
    const merge = mergeGroup ? `\n${indent}    string physics:mergeGroup = "${mergeGroup}"` : '', invert = invertFilteredGroups ? `\n${indent}    bool physics:invertFilteredGroups = 1` : '';
    const block = `\n${indent}def PhysicsCollisionGroup "${name}" (\n${indent}    prepend apiSchemas = ["CollectionAPI:colliders"]\n${indent}) {${targets}${filtered}${merge}${invert}\n${indent}}\n`, original = this.usda;
    this.usda = original.slice(0, parentBlock.close) + block + original.slice(parentBlock.close);
    try { return await this.replaceUSDA(this.usda, summary); } catch (error) { this.usda = original; throw error; }
  }

  getMeshMaterialGroups(path, indexCount) {
    const subsetPaths = findGeomSubsetPaths(this.usda, path);
    if (!subsetPaths.length) return null;
    const faceMaterials = new Array(Math.floor(indexCount / 3)).fill(null);
    for (let subsetIndex = 0; subsetIndex < subsetPaths.length; subsetIndex++) {
      const indices = parseIntegerArrayProperty(this.usda, subsetPaths[subsetIndex], 'indices');
      if (!indices) throw new LuciaError('LUCIA_MATERIAL_GROUPS', `GeomSubset indices are not readable: ${subsetPaths[subsetIndex]}`);
      for (const face of indices) {
        if (face < 0 || face >= faceMaterials.length || faceMaterials[face] != null) throw new LuciaError('LUCIA_MATERIAL_GROUPS', `GeomSubset face index is invalid or overlaps another subset: ${subsetPaths[subsetIndex]}`);
        faceMaterials[face] = subsetIndex;
      }
    }
    const groups = [];
    for (let face = 0; face < faceMaterials.length;) {
      const materialIndex = faceMaterials[face];
      let end = face + 1;
      while (end < faceMaterials.length && faceMaterials[end] === materialIndex) end++;
      if (materialIndex != null) groups.push({ start: face * 3, count: (end - face) * 3, materialIndex });
      face = end;
    }
    return groups;
  }

  getMeshMaterialPaths(path) {
    const mesh = findPrimBlock(this.usda, path);
    if (!mesh) return null;
    const bindingPath = (primPath) => {
      const block = findPrimBlock(this.usda, primPath), body = block ? this.usda.slice(block.start, block.end) : '';
      return body.match(/\bmaterial:binding\s*=\s*<([^>]+)>/)?.[1] || null;
    };
    const subsetPaths = findGeomSubsetPaths(this.usda, path);
    if (subsetPaths.length) {
      const paths = subsetPaths.map(bindingPath);
      return paths.every((materialPath) => materialPath) ? paths : null;
    }
    const direct = bindingPath(path);
    return direct ? [direct] : null;
  }

  getMeshUVData(path, uvSet = 'default') {
    if (!['default', 'lightmap'].includes(uvSet)) throw new LuciaError('LUCIA_UV_SET', `Unsupported UV set: ${uvSet}`);
    const property = uvSet === 'lightmap' ? 'primvars:st1' : 'primvars:st', indicesProperty = `${property}:indices`, block = findPrimBlock(this.usda, path);
    if (!block) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Prim not found: ${path}`, { path });
    const body = this.usda.slice(block.start, block.end), declaration = new RegExp(`(?:uniform\\s+|varying\\s+)?texCoord2f\\[\\]\\s+${escapeRegExp(property)}\\s*=\\s*\\[[\\s\\S]*?\\]\\s*(\\(\\s*interpolation\\s*=\\s*"([A-Za-z]+)"\\s*\\))?`).exec(body), values = parseTupleArrayProperty(this.usda, path, 'texCoord2f', property);
    if (!values && declaration) throw new LuciaError('LUCIA_UV_DATA', `Mesh UV set is malformed: ${path} (${uvSet}).`);
    if (!values) return null;
    const faceVarying = declaration?.[2] === 'faceVarying';
    if (!faceVarying) return { uvs: values, uvIndices: null };
    const indices = parseIntegerArrayProperty(this.usda, path, indicesProperty);
    if (!indices) throw new LuciaError('LUCIA_UV_DATA', `Face-varying mesh UV indices are malformed: ${path} (${uvSet}).`);
    return { uvs: values, uvIndices: Uint32Array.from(indices) };
  }

  getMeshFaceVaryingPrimvars(path, indexCount) {
    if (!Number.isInteger(indexCount) || indexCount < 3) return [];
    return parseFaceVaryingPrimvars(this.usda, path, indexCount);
  }

  getMeshSharpChainData(path) {
    const block = findPrimBlock(this.usda, path);
    if (!block) throw new LuciaError('LUCIA_PATH_NOT_FOUND', `Prim not found: ${path}`, { path });
    const body = this.usda.slice(block.start, block.end), hasIndices = /\b(?:uniform\s+)?int\[\]\s+creaseIndices\s*=/.test(body), hasLengths = /\b(?:uniform\s+)?int\[\]\s+creaseLengths\s*=/.test(body);
    if (!hasIndices && !hasLengths) return { chains: [], sharpness: [] };
    const indices = parseIntegerArrayProperty(this.usda, path, 'creaseIndices'), lengths = parseIntegerArrayProperty(this.usda, path, 'creaseLengths'), sharpness = parseFloatArrayProperty(this.usda, path, 'creaseSharpness');
    if (!indices || !lengths || Boolean(indices.length) !== Boolean(lengths.length)) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', `Mesh creaseIndices and creaseLengths must be readable paired integer arrays: ${path}`);
    if (sharpness && sharpness.length !== lengths.length) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', `Mesh creaseSharpness must contain one value per crease chain: ${path}`);
    if (!indices.length) return { chains: [], sharpness: [] };
    const chains = [];
    let cursor = 0;
    for (const length of lengths) {
      if (!Number.isInteger(length) || length < 2 || cursor + length > indices.length) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', `Mesh creaseLengths contains an invalid chain length: ${path}`);
      chains.push(indices.slice(cursor, cursor + length)); cursor += length;
    }
    if (cursor !== indices.length) throw new LuciaError('LUCIA_MESH_SHARP_EDGES', `Mesh creaseLengths does not cover creaseIndices: ${path}`);
    return { chains, sharpness: sharpness ? sharpness.slice() : chains.map(() => 1) };
  }

  getMeshSharpEdgeData(path) {
    const { chains, sharpness } = this.getMeshSharpChainData(path), weighted = new Map();
    for (let chainIndex = 0; chainIndex < chains.length; chainIndex++) if (sharpness[chainIndex] > 0) for (let i = 1; i < chains[chainIndex].length; i++) {
      const a = chains[chainIndex][i - 1], b = chains[chainIndex][i], pair = a < b ? [a, b] : [b, a], key = `${pair[0]}:${pair[1]}`, value = sharpness[chainIndex];
      weighted.set(key, { edge: pair, value: Math.max(value, weighted.get(key)?.value || 0) });
    }
    const entries = [...weighted.values()];
    return { edges: entries.map(({ edge }) => edge), sharpness: entries.map(({ value }) => value) };
  }

  getMeshSharpEdges(path) {
    return this.getMeshSharpEdgeData(path).edges;
  }

  async validate() {
    // The embind method takes an options JSON string even when no validation
    // options are requested. Passing an explicit empty object also keeps this
    // path compatible with native builds that do not provide a JS default.
    const result = this.render.validateLoadedLayer('{}');
    if (typeof result === 'string') return JSON.parse(result);
    return result || { valid: true, issues: [] };
  }

  exportUSDA() { return this.usda; }
  exportUSDZ(assets = new Map(), remap = {}) {
    const exporter = new this.module.LightUSDLoaderNative();
    try {
      for (const [name, asset] of assets) exporter.setAsset(name, asset.bytes);
      if (!exporter.loadAsLayerFromBinary(encoder.encode(this.usda), this.filename)) throw new LuciaError('LUCIA_EXPORT_LOAD', exporter.error());
      const data = Object.keys(remap).length ? exporter.exportAsUSDZWithRemap(remap) : exporter.exportAsUSDZ();
      if (!data) throw new LuciaError('LUCIA_EXPORT_USDZ', exporter.error() || 'USDZ export failed.');
      const archive = new Uint8Array(data);
      try { validateUSDZArchive(archive); } catch (error) { throw new LuciaError('LUCIA_EXPORT_USDZ_LAYOUT', error.message); }
      return archive;
    } finally { exporter.delete(); }
  }

  dispose() {
    this.author?.delete();
    this.render?.delete();
    this.author = this.render = null;
  }
}

export { findPrimBlock, setAttributeText };
