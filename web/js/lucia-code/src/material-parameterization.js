import { TARGET_PROFILES } from './target-profiles.js';
import { INDEXED_MESH_MAX_INDICES } from './indexed-mesh.js';

const CHANNEL_ALIASES = Object.freeze({
  baseColor: ['baseColor', 'base_color', 'diffuseColor', 'diffuse_color'],
  metallic: ['metallic', 'metalness'],
  roughness: ['roughness', 'specular_roughness'],
  opacity: ['opacity', 'alpha'],
  emissive: ['emissive', 'emissiveColor', 'emission_color', 'emissionColor'],
  normal: ['normal', 'normalMap', 'normal_map'],
});
const CHANNEL_BY_ALIAS = new Map(Object.entries(CHANNEL_ALIASES).flatMap(([channel, aliases]) => aliases.map((alias) => [alias, channel])));
const vectorValue = (value) => Array.isArray(value) || ArrayBuffer.isView(value) ? Array.from(value) : null;
const isFiniteValue = (value) => typeof value === 'number' && Number.isFinite(value) || (() => { const vector = vectorValue(value); return Boolean(vector) && vector.length > 0 && vector.length <= 4 && vector.every((component) => typeof component === 'number' && Number.isFinite(component)); })();
const detached = (value) => { const vector = vectorValue(value); return vector ? [...vector] : value; };
const stableValue = (value) => JSON.stringify(detached(value));
const canonicalValue = (channels, aliases) => {
  for (const alias of aliases) if (Object.hasOwn(channels, alias) && isFiniteValue(channels[alias])) return { alias, value: detached(channels[alias]) };
  return null;
};

export function normalizeParameterizationProfile(profile = 'web-viewer') {
  const key = String(profile || 'web-viewer').toLowerCase();
  if (!TARGET_PROFILES[key]) throw new Error(`Unsupported material parameterization profile: ${profile}`);
  return key;
}

export function planMaterialParameterization({ materials = [], targetProfile = 'web-viewer', mode = 'auto', channels = null } = {}) {
  if (!Array.isArray(materials)) throw new Error('Material parameterization requires a material array.');
  const profile = normalizeParameterizationProfile(targetProfile), capabilities = TARGET_PROFILES[profile].materialParameterization;
  if (!['auto', 'primvar', 'variant'].includes(mode)) throw new Error(`Unsupported material parameterization mode: ${mode}`);
  const requested = channels == null ? Object.keys(CHANNEL_ALIASES) : Array.isArray(channels) ? [...new Set(channels.map((channel) => String(channel)))] : null;
  if (!requested || requested.some((channel) => !CHANNEL_ALIASES[channel])) throw new Error('Material parameterization channels must be known PBR channel names.');
  const entries = materials.map((material, index) => {
    if (!material || typeof material !== 'object' || Array.isArray(material)) throw new Error(`Material parameterization entry ${index} is malformed.`);
    const path = String(material.path || material.id || `material:${index}`);
    if (!path || /[\u0000-\u001f\u007f]/.test(path)) throw new Error(`Material parameterization entry ${index} has an unsafe path.`);
    const source = material.channels && typeof material.channels === 'object' && !Array.isArray(material.channels) ? material.channels : material;
    return { path, source };
  }).sort((left, right) => left.path.localeCompare(right.path));
  const unsupported = [], candidates = [];
  for (const channel of requested) {
    const values = [], aliases = CHANNEL_ALIASES[channel];
    if (!entries.some((entry) => aliases.some((alias) => Object.hasOwn(entry.source, alias)))) continue;
    for (const entry of entries) {
      const resolved = canonicalValue(entry.source, aliases);
      if (!resolved) { unsupported.push({ channel, path: entry.path, reason: 'Material does not provide a finite supported channel value.' }); continue; }
      values.push({ path: entry.path, value: resolved.value, alias: resolved.alias });
    }
    if (values.length !== entries.length) continue;
    const distinct = new Set(values.map((entry) => stableValue(entry.value)));
    if (distinct.size < 2) continue;
    const selectedMode = mode === 'auto' ? capabilities.primvar ? 'primvar' : capabilities.variant ? 'variant' : null : mode;
    if (!selectedMode || !capabilities[selectedMode]) { unsupported.push({ channel, path: '/', reason: `Target profile ${profile} does not support ${mode === 'auto' ? 'material parameterization' : `${selectedMode} material parameterization`}.` }); continue; }
    candidates.push({ channel, mode: selectedMode, parameter: `${selectedMode === 'primvar' ? 'primvars' : 'variants'}:lucia:${channel}`, materialPaths: values.map((entry) => entry.path), values: values.map((entry) => entry.value), sourceAliases: [...new Set(values.map((entry) => entry.alias))].sort(), distinctValueCount: distinct.size, lossless: true });
  }
  candidates.sort((left, right) => left.channel.localeCompare(right.channel));
  unsupported.sort((left, right) => `${left.channel}:${left.path}:${left.reason}`.localeCompare(`${right.channel}:${right.path}:${right.reason}`));
  return { profile, mode, candidates, unsupported, changed: candidates.length > 0, lossless: unsupported.length === 0 };
}

export function materializePrimvarParameterization({ candidate, materialPaths = [], faceMaterialIndices } = {}) {
  if (!candidate || typeof candidate !== 'object' || Array.isArray(candidate) || candidate.mode !== 'primvar' || !/^primvars:[A-Za-z_][\w:]*$/.test(candidate.parameter || '')) throw new Error('Material primvar authoring requires a valid primvar candidate.');
  if (!Array.isArray(materialPaths) || !materialPaths.length || materialPaths.some((path) => typeof path !== 'string' || !path)) throw new Error('Material primvar authoring requires a non-empty material path array.');
  if (!faceMaterialIndices || !Number.isSafeInteger(faceMaterialIndices.length) || !faceMaterialIndices.length || faceMaterialIndices.length % 3 || faceMaterialIndices.length > INDEXED_MESH_MAX_INDICES) throw new Error('Material primvar authoring requires one material index per face corner within the shared mesh safety limit.');
  if (!Array.isArray(candidate.materialPaths) || !Array.isArray(candidate.values) || candidate.materialPaths.length !== candidate.values.length || !candidate.materialPaths.length) throw new Error('Material primvar candidate values are malformed.');
  const candidateValues = new Map();
  for (let index = 0; index < candidate.materialPaths.length; index++) {
    const path = candidate.materialPaths[index], value = candidate.values[index], vector = vectorValue(value);
    if (typeof path !== 'string' || !path || !isFiniteValue(value) || candidateValues.has(path)) throw new Error('Material primvar candidate contains invalid or duplicate material values.');
    candidateValues.set(path, vector ? [...vector] : value);
  }
  const itemSize = vectorValue(candidate.values[0])?.length || 1, values = [], indices = new Uint32Array(faceMaterialIndices.length);
  for (let corner = 0; corner < faceMaterialIndices.length; corner++) {
    const materialIndex = faceMaterialIndices[corner];
    if (!Number.isSafeInteger(materialIndex) || materialIndex < 0 || materialIndex >= materialPaths.length) throw new Error('Material primvar face indices contain an out-of-range material.');
    const value = candidateValues.get(materialPaths[materialIndex]), vector = vectorValue(value);
    if (value == null || vector && vector.length !== itemSize || !vector && itemSize !== 1) throw new Error('Material primvar material paths do not cover every face material.');
    indices[corner] = corner;
    if (vector) values.push(...vector); else values.push(value);
  }
  return { name: candidate.parameter.slice('primvars:'.length), itemSize, array: Float32Array.from(values), indices, interpolation: 'faceVarying', channel: candidate.channel, materialPaths: [...materialPaths] };
}

export function materializeVariantParameterization({ candidate, materialPaths = [] } = {}) {
  if (!candidate || typeof candidate !== 'object' || Array.isArray(candidate) || candidate.mode !== 'variant' || !/^variants:[A-Za-z_][\w:]*$/.test(candidate.parameter || '')) throw new Error('Material variant authoring requires a valid variant candidate.');
  if (!Array.isArray(materialPaths) || !materialPaths.length || materialPaths.some((path) => typeof path !== 'string' || !/^\/(?:[A-Za-z_][A-Za-z0-9_]*)(?:\/(?:[A-Za-z_][A-Za-z0-9_]*))*$/.test(path))) throw new Error('Material variant authoring requires absolute material prim paths.');
  if (!Array.isArray(candidate.materialPaths) || !Array.isArray(candidate.values) || candidate.materialPaths.length !== candidate.values.length || candidate.materialPaths.length < 2 || candidate.values.some((value) => !isFiniteValue(value))) throw new Error('Material variant candidate values are malformed.');
  const seen = new Set(), variants = [];
  for (const path of candidate.materialPaths) {
    if (!materialPaths.includes(path) || seen.has(path)) throw new Error('Material variant candidate does not map uniquely to the selected materials.');
    seen.add(path);
    const leaf = path.split('/').at(-1), name = `material_${leaf}`.replace(/[^A-Za-z0-9_]/g, '_');
    variants.push({ name: name || `material_${variants.length}`, materialPath: path });
  }
  if (new Set(variants.map(({ name }) => name)).size !== variants.length) throw new Error('Material variant names collide after USD identifier normalization.');
  return { name: candidate.parameter.slice('variants:'.length), channel: candidate.channel, variants };
}
