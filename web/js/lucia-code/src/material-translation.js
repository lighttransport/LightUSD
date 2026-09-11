const FORMATS = new Set(['materialx-standard-surface', 'usd-preview-surface', 'metallic-roughness']);
const CHANNELS = Object.freeze({
  baseColor: ['baseColor', 'base_color', 'diffuseColor', 'diffuse_color'],
  metallic: ['metallic', 'metalness'],
  roughness: ['roughness', 'specular_roughness'],
  opacity: ['opacity', 'alpha'],
  emissive: ['emissive', 'emissiveColor', 'emission_color', 'emissionColor'],
  normal: ['normal', 'normalMap', 'normal_map'],
  ior: ['ior', 'indexOfRefraction'],
  specular: ['specular', 'specularColor', 'specular_color'],
});
const OUTPUT_KEYS = Object.freeze({
  'materialx-standard-surface': { baseColor: 'base_color', metallic: 'metalness', roughness: 'specular_roughness', opacity: 'opacity', emissive: 'emission_color', normal: 'normal', ior: 'ior', specular: 'specular_weight' },
  'usd-preview-surface': { baseColor: 'diffuseColor', metallic: 'metallic', roughness: 'roughness', opacity: 'opacity', emissive: 'emissiveColor', normal: 'normal', ior: 'ior', specular: 'specularColor' },
  'metallic-roughness': { baseColor: 'baseColor', metallic: 'metallic', roughness: 'roughness', opacity: 'alpha', emissive: 'emissive', normal: 'normal', ior: 'ior', specular: 'specular' },
});
const aliases = new Map(Object.entries(CHANNELS).flatMap(([channel, names]) => names.map((name) => [name, channel])));
const validChannelValue = (value) => typeof value === 'number' && Number.isFinite(value) || Array.isArray(value) && value.length > 0 && value.length <= 4 && value.every((component) => typeof component === 'number' && Number.isFinite(component));
const graphType = (value) => String(value || '').toLowerCase().replace(/[ _-]/g, '');
const cloneGraphValue = (value, seen = new WeakSet()) => { if (!value || typeof value !== 'object') return value; if (seen.has(value)) throw new Error('Material graph values must be acyclic.'); seen.add(value); if (ArrayBuffer.isView(value)) { const copy = new value.constructor(value); seen.delete(value); return copy; } if (Array.isArray(value)) { const copy = value.map((child) => cloneGraphValue(child, seen)); seen.delete(value); return copy; } const prototype = Object.getPrototypeOf(value); if (prototype !== Object.prototype && prototype !== null) { seen.delete(value); throw new Error('Material graph values must be plain objects, arrays, or typed arrays.'); } const copy = Object.fromEntries(Object.entries(value).map(([key, child]) => [key, cloneGraphValue(child, seen)])); seen.delete(value); return copy; };
const targetGraphType = Object.freeze({ 'materialx-standard-surface': 'standard_surface', 'usd-preview-surface': 'usdpreviewsurface', 'metallic-roughness': 'metallic_roughness' });
const sourceGraphTypes = new Set(['standardsurface', 'usdpreviewsurface', 'principledbsdf', 'metallicroughness', 'metallic_roughness', 'ndstandardsurfacesurfaceshader', 'ndusdpreviewsurface']);
const SHADER_IDS = Object.freeze({ 'materialx-standard-surface': Object.freeze(['ND_standard_surface_surfaceshader', 'standard_surface', 'StandardSurface']), 'usd-preview-surface': Object.freeze(['UsdPreviewSurface']), 'metallic-roughness': Object.freeze([]) });
const graphEntries = (source) => { if (source instanceof Map) return [...source.entries()].map(([id, node]) => [String(node?.id ?? id), node]); if (Array.isArray(source)) return source.map((node, index) => [String(node?.id ?? node?.name ?? `node:${index}`), node]); if (!source || typeof source !== 'object' || ArrayBuffer.isView(source)) return []; return Object.entries(source); };
const graphInputEntries = (candidate) => candidate instanceof Map ? [...candidate.entries()] : Object.entries(candidate);
const graphInputKey = (node) => { const candidate = node?.inputs !== undefined ? node.inputs : node?.parameters; return candidate && typeof candidate === 'object' && !Array.isArray(candidate) && !ArrayBuffer.isView(candidate) ? (node?.inputs !== undefined ? 'inputs' : 'parameters') : null; };

function outsideUSDAStrings(text, callback) {
  let output = '', quote = false, escaped = false, comment = false, index = 0;
  while (index < text.length) {
    const character = text[index];
    if (comment) { output += character; index++; if (character === '\n' || character === '\r') comment = false; continue; }
    if (quote) { output += character; index++; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '#') { comment = true; output += character; index++; continue; }
    if (character === '"') { quote = true; output += character; index++; continue; }
    const replacement = callback(text, index);
    if (replacement) { output += replacement.value; index += replacement.length; } else { output += character; index++; }
  }
  return output;
}

function authoredInputNames(text) {
  const names = new Set();
  outsideUSDAStrings(text, (source, index) => {
    const match = source.slice(index).match(/^inputs:([A-Za-z_][\w]*)/);
    if (!match || index > 0 && /[A-Za-z0-9_:]/.test(source[index - 1])) return null;
    names.add(match[1]);
    return null;
  });
  return [...names];
}

function hasAuthoredInput(text, name) {
  let found = false;
  outsideUSDAStrings(text, (source, index) => {
    if (source.startsWith(`inputs:${name}` , index) && (index === 0 || !/[A-Za-z0-9_:]/.test(source[index - 1])) && /[:.\s=]/.test(source[index + 7 + name.length] || '')) found = true;
    return null;
  });
  return found;
}

function translateAuthoredShaderId(text, sourceFormat, targetFormat) {
  const targetIds = SHADER_IDS[targetFormat] || [], target = targetIds[0];
  if (!target) return { body: text, changed: false, from: null, to: null };
  const sourceIds = new Set(SHADER_IDS[sourceFormat] || []);
  let changed = false, from = null;
  const body = outsideUSDAStrings(text, (source, index) => {
    if (index > 0 && /[A-Za-z0-9_:]/.test(source[index - 1])) return null;
    const match = source.slice(index).match(/^(?:uniform\s+)?token\s+info:id\s*=\s*"([^"]+)"/);
    if (!match || !sourceIds.has(match[1])) return null;
    changed = true; from = match[1];
    return { value: match[0].replace(`"${match[1]}"`, `"${target}"`), length: match[0].length };
  });
  return { body, changed, from, to: changed ? target : null };
}

function renameAuthoredInput(text, from, to) {
  return outsideUSDAStrings(text, (source, index) => {
    const token = `inputs:${from}`;
    if (!source.startsWith(token, index) || index > 0 && /[A-Za-z0-9_:]/.test(source[index - 1]) || !/[:.\s=]/.test(source[index + token.length] || '')) return null;
    return { value: `inputs:${to}`, length: token.length };
  });
}

export const normalizePBRFormat = (format) => {
  const value = String(format || '').toLowerCase().replaceAll('_', '-').replaceAll(' ', '-');
  if (value === 'standard-surface' || value === 'materialx' || value === 'materialx-standard') return 'materialx-standard-surface';
  if (value === 'usdpreviewsurface' || value === 'usd-preview') return 'usd-preview-surface';
  if (value === 'metallic-roughness' || value === 'pbr') return 'metallic-roughness';
  if (!FORMATS.has(value)) throw new Error(`Unsupported PBR material format: ${format}`);
  return value;
};

export function translatePBRInputs(request = {}) {
  const { inputs = {}, from, to } = request && typeof request === 'object' ? request : {};
  const sourceFormat = normalizePBRFormat(from), targetFormat = normalizePBRFormat(to);
  if (!FORMATS.has(sourceFormat) || !FORMATS.has(targetFormat)) throw new Error(`Unsupported PBR material format: ${from} -> ${to}`);
  const output = {}, unsupported = [], approximations = [], invalid = new Set(), consumed = new Set();
  const orderedInputs = Object.entries(inputs || {}).sort(([left], [right]) => {
    const leftChannel = aliases.get(left), rightChannel = aliases.get(right), leftRank = leftChannel ? CHANNELS[leftChannel].indexOf(left) : Number.MAX_SAFE_INTEGER, rightRank = rightChannel ? CHANNELS[rightChannel].indexOf(right) : Number.MAX_SAFE_INTEGER;
    return `${leftChannel || ''}:${String(leftRank).padStart(3, '0')}:${left}`.localeCompare(`${rightChannel || ''}:${String(rightRank).padStart(3, '0')}:${right}`);
  });
  for (const [key, value] of orderedInputs) {
    const channel = aliases.get(key);
    if (!channel) { unsupported.push(key); continue; }
    consumed.add(key);
    if (!validChannelValue(value)) { unsupported.push(key); invalid.add(key); continue; }
    if (channel === 'specular' && targetFormat === 'metallic-roughness') {
      unsupported.push(key);
      approximations.push({ input: key, reason: 'Metallic-roughness has no independent specular input; the value is not authored.' });
      continue;
    }
    if (channel === 'ior' && targetFormat === 'metallic-roughness') {
      unsupported.push(key);
      approximations.push({ input: key, reason: 'Metallic-roughness does not carry an IOR control in the portable subset.' });
      continue;
    }
    const targetKey = OUTPUT_KEYS[targetFormat][channel];
    if (!targetKey) { unsupported.push(key); continue; }
    // Prefer the canonical alias listed first in CHANNELS. This prevents
    // equivalent input names from producing insertion-order-dependent output.
    if (Object.hasOwn(output, targetKey)) {
      approximations.push({ input: key, output: targetKey, reason: 'Equivalent alias was ignored in favor of the canonical input; review conflicting authored values.' });
      continue;
    }
    output[targetKey] = value;
    if (key !== targetKey) approximations.push({ input: key, output: targetKey, reason: 'Renamed portable PBR channel.' });
  }
  const uniqueUnsupported = [...new Set(unsupported)].sort(), differences = [...approximations, ...uniqueUnsupported.map((input) => ({ input, output: null, reason: invalid.has(input) ? 'Input is not a finite scalar or 1–4 component numeric vector; the contribution is not authored.' : 'No portable target input exists; the contribution is not authored.' }))].sort((a, b) => `${a.input}:${a.output || ''}`.localeCompare(`${b.input}:${b.output || ''}`));
  return { from: sourceFormat, to: targetFormat, inputs: output, unsupported: uniqueUnsupported, approximations: approximations.sort((a, b) => `${a.input}:${a.output || ''}`.localeCompare(`${b.input}:${b.output || ''}`)), differences, consumed: [...consumed].sort() };
}

export function inspectPBRTranslation(request = {}) {
  const { inputs = {}, from, to } = request && typeof request === 'object' ? request : {};
  const result = translatePBRInputs({ inputs, from, to });
  return { ...result, lossless: result.unsupported.length === 0 && result.approximations.every((item) => item.reason === 'Renamed portable PBR channel.') };
}

// Translate authored USD shader input names inside one shader body. Values and
// connection expressions are intentionally untouched; only recognized channel
// names are renamed, and an existing target input is never overwritten.
export function translateAuthoredPBRProperties(body, options = {}) {
  const { from, to } = options && typeof options === 'object' ? options : {};
  const sourceFormat = normalizePBRFormat(from), targetFormat = normalizePBRFormat(to);
  if (!FORMATS.has(sourceFormat) || !FORMATS.has(targetFormat)) throw new Error(`Unsupported PBR material format: ${from} -> ${to}`);
  const sourceNames = new Set(Object.values(CHANNELS).flat()), targetNames = OUTPUT_KEYS[targetFormat], text = String(body || ''), shader = translateAuthoredShaderId(text, sourceFormat, targetFormat), authored = authoredInputNames(shader.body);
  let output = shader.body, changed = shader.changed ? 1 : 0, skipped = [], unsupported = [], approximations = shader.changed ? [{ input: `info:id:${shader.from}`, output: `info:id:${shader.to}`, reason: 'Renamed recognized shader implementation.' }] : [];
  for (const name of authored.sort()) {
    if (!sourceNames.has(name)) continue;
    const channel = aliases.get(name), target = targetNames[channel];
    if (targetFormat === 'metallic-roughness' && ['specular', 'ior'].includes(channel)) { unsupported.push(name); continue; }
    if (!target || target === name) continue;
    if (hasAuthoredInput(output, target)) { skipped.push(name); continue; }
    output = renameAuthoredInput(output, name, target); changed++;
    approximations.push({ input: name, output: target, reason: 'Renamed authored portable PBR channel.' });
  }
  return { from: sourceFormat, to: targetFormat, body: output, changed, skipped: skipped.sort(), unsupported: [...new Set(unsupported)].sort(), approximations: approximations.sort((a, b) => `${a.input}:${a.output}`.localeCompare(`${b.input}:${b.output}`)), shaderId: shader.changed ? { from: shader.from, to: shader.to } : null };
}

// Translate serializable graph shader nodes without mutating the source. Node
// connections are opaque values here and are copied exactly; only recognized
// PBR input keys and the shader node type are changed. Unsupported channels and
// target-key collisions are reported instead of being silently discarded.
export function translateMaterialGraph(graph, options = {}) {
  const sourceFormat = normalizePBRFormat(options?.from), targetFormat = normalizePBRFormat(options?.to), source = graph?.nodes || graph, entries = graphEntries(source);
  const unsupported = [], collisions = [], changedNodes = [], nodes = entries.sort(([left], [right]) => left.localeCompare(right)).map(([id, original]) => {
    const sourceNode = original && typeof original === 'object' && !Array.isArray(original) ? original : {}, sourceInputKey = graphInputKey(sourceNode), normalizedNode = sourceInputKey ? { ...sourceNode, [sourceInputKey]: Object.fromEntries(graphInputEntries(sourceNode[sourceInputKey])) } : sourceNode, node = { ...cloneGraphValue(normalizedNode), id: String(original?.id ?? id) }, typeKey = node.type !== undefined ? 'type' : node.nodeType !== undefined ? 'nodeType' : node.category !== undefined ? 'category' : 'type', kind = graphType(node[typeKey]), inputKey = graphInputKey(node);
    if (!sourceGraphTypes.has(kind)) return node;
    const inputs = inputKey ? Object.fromEntries(graphInputEntries(node[inputKey])) : {}, translated = {};
    for (const key of Object.keys(inputs).sort()) {
      const channel = aliases.get(key), target = channel ? OUTPUT_KEYS[targetFormat][channel] : null;
      if (!channel || (targetFormat === 'metallic-roughness' && ['specular', 'ior'].includes(channel))) { if (channel) unsupported.push(`${node.id}.${key}`); translated[key] = inputs[key]; continue; }
      if (!target) { unsupported.push(`${node.id}.${key}`); translated[key] = inputs[key]; continue; }
      if (Object.hasOwn(translated, target)) { collisions.push(`${node.id}.${key} → ${target}`); translated[key] = inputs[key]; continue; }
      translated[target] = inputs[key]; if (key !== target) changedNodes.push(`${node.id}.${key} → ${target}`);
    }
    const translatedType = targetGraphType[targetFormat], typeChanged = translatedType && node[typeKey] !== translatedType;
    if (typeChanged) changedNodes.push(`${node.id}.${typeKey} → ${translatedType}`);
    return { ...node, ...(Object.keys(inputs).length ? { [inputKey]: translated } : {}), ...(typeChanged ? { [typeKey]: translatedType } : {}) };
  });
  const unique = (values) => [...new Set(values)].sort();
  return { from: sourceFormat, to: targetFormat, nodes, changed: changedNodes.length > 0, changedNodes: unique(changedNodes), unsupported: unique(unsupported), collisions: unique(collisions), differences: [...unique(unsupported).map((input) => ({ input, reason: 'No portable target input exists; the authored contribution is retained for review.' })), ...unique(collisions).map((input) => ({ input, reason: 'Target input already exists; the source connection is retained to avoid overwriting it.' }))] };
}
