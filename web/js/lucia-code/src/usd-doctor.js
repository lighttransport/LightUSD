import { validateQualityGate } from './target-profiles.js';
import { validPrimPath } from './utils.js';

const issue = (severity, ruleId, path, message) => ({ severity, ruleId, path, message });
const normalizeAssetPath = (path) => String(path).replaceAll('\\', '/').split('/').reduce((parts, part) => { if (!part || part === '.') return parts; if (part === '..') parts.pop(); else parts.push(part); return parts; }, []).join('/');
const unsafeAssetReference = (path) => /^(?:[A-Za-z]:[\\/]|[\\/])/.test(String(path || '')) || String(path || '').replaceAll('\\', '/').split('/').includes('..');
const resolverAssetReference = (path) => /^[A-Za-z][A-Za-z0-9+.-]*:/.test(String(path || '')) && !/^[A-Za-z]:[\\/]/.test(String(path || '')) && !String(path || '').startsWith('anon:');
const bytesHash = (bytes) => { let hash = 2166136261; const values = bytes && typeof bytes[Symbol.iterator] === 'function' ? bytes : []; for (const value of values) { hash ^= value; hash = Math.imul(hash, 16777619); } return hash >>> 0; };
const dependencyStatus = (path, present) => String(path || '').startsWith('anon:') ? 'anonymous' : unsafeAssetReference(path) ? 'unsafe' : resolverAssetReference(path) ? 'resolver' : present ? 'available' : 'missing';
const DEPENDENCY_STATUSES = new Set(['available', 'missing', 'unsafe', 'resolver', 'anonymous']);
const normalizedDependencyStatus = (path, present, status) => DEPENDENCY_STATUSES.has(status) ? status : dependencyStatus(path, present);
export function normalizeResolverPlugins(plugins = []) {
  const entries = plugins instanceof Map ? [...plugins.entries()].map(([scheme, plugin]) => ({ scheme, ...(plugin && typeof plugin === 'object' ? plugin : { name: plugin }) })) : Array.isArray(plugins) ? plugins : plugins && typeof plugins === 'object' ? Object.entries(plugins).map(([scheme, plugin]) => ({ scheme, ...(plugin && typeof plugin === 'object' ? plugin : { name: plugin }) })) : [];
  const normalized = new Map();
  for (const entry of entries) {
    const scheme = String(entry?.scheme || '').replace(/:$/, '').toLowerCase(), name = String(entry?.name || '').trim();
    if (!/^[a-z][a-z0-9+.-]*$/.test(scheme) || !name || /[\u0000-\u001f\u007f]/.test(name)) continue;
    const capabilities = [...new Set((Array.isArray(entry.capabilities) ? entry.capabilities : []).filter((value) => typeof value === 'string' && /^[a-z][a-z0-9+.-]*$/i.test(value)).map((value) => value.toLowerCase()))].sort();
    const candidate = { scheme, name, ...(entry.version != null && /^[A-Za-z0-9._+-]+$/.test(String(entry.version)) ? { version: String(entry.version) } : {}), ...(capabilities.length ? { capabilities } : {}) }, existing = normalized.get(scheme);
    if (!existing || JSON.stringify(candidate) < JSON.stringify(existing)) normalized.set(scheme, candidate);
  }
  return [...normalized.values()].sort((a, b) => a.scheme.localeCompare(b.scheme));
}
const USDZ_ALLOWED_EXTENSIONS = new Set(['.usd', '.usda', '.usdc', '.png', '.jpg', '.jpeg', '.exr', '.hdr', '.tif', '.tiff']);
function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) { crc ^= byte; for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1)); }
  return (crc ^ 0xffffffff) >>> 0;
}

export function validateUSDZArchive(bytes) {
  const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || []), view = new DataView(data.buffer, data.byteOffset, data.byteLength), read16 = (offset) => view.getUint16(offset, true), read32 = (offset) => view.getUint32(offset, true), fail = (message) => { throw new Error(`USDZ archive: ${message}`); };
  if (data.length < 22) fail('truncated end-of-central-directory record');
  let eocd = -1; for (let offset = data.length - 22; offset >= Math.max(0, data.length - 0xffff - 22); offset--) if (read32(offset) === 0x06054b50) { eocd = offset; break; }
  if (eocd < 0) fail('missing end-of-central-directory record');
  const disk = read16(eocd + 4), centralDisk = read16(eocd + 6), count = read16(eocd + 10), centralSize = read32(eocd + 12), centralOffset = read32(eocd + 16);
  if (disk || centralDisk || count === 0xffff || centralSize === 0xffffffff || centralOffset === 0xffffffff) fail('multi-disk or ZIP64 archives are not supported');
  if (centralOffset + centralSize > eocd || centralOffset < 0) fail('central directory is outside the archive');
  const decoder = new TextDecoder(), names = new Set(), entries = []; let cursor = centralOffset;
  for (let i = 0; i < count; i++) {
    if (cursor + 46 > data.length || read32(cursor) !== 0x02014b50) fail('invalid central-directory entry');
    const flags = read16(cursor + 8), method = read16(cursor + 10), checksum = read32(cursor + 16), compressed = read32(cursor + 20), uncompressed = read32(cursor + 24), nameLength = read16(cursor + 28), extraLength = read16(cursor + 30), commentLength = read16(cursor + 32), localOffset = read32(cursor + 42), nameStart = cursor + 46;
    if (nameStart + nameLength + extraLength + commentLength > data.length) fail('truncated central-directory entry');
    const name = decoder.decode(data.subarray(nameStart, nameStart + nameLength));
    if (!name || /[\u0000-\u001f\u007f]/.test(name) || names.has(name) || name.startsWith('/') || name.includes('\\') || name.split('/').includes('..')) fail(`unsafe or duplicate member path: ${name || '<empty>'}`);
    const extension = name.slice(name.lastIndexOf('.')).toLowerCase(); if (!USDZ_ALLOWED_EXTENSIONS.has(extension)) fail(`unsupported member type: ${name}`);
    if (flags & 1) fail(`encrypted member: ${name}`); if (method !== 0) fail(`compressed member: ${name}`);
    if (localOffset + 30 > data.length || read32(localOffset) !== 0x04034b50) fail(`invalid local header: ${name}`);
    if (read16(localOffset + 6) !== flags || read16(localOffset + 8) !== method) fail(`local and central compression flags differ: ${name}`);
    const localNameLength = read16(localOffset + 26), localExtraLength = read16(localOffset + 28), localNameStart = localOffset + 30;
    if (localNameStart + localNameLength > data.length || decoder.decode(data.subarray(localNameStart, localNameStart + localNameLength)) !== name) fail(`local and central member names differ: ${name}`);
    const payloadOffset = localNameStart + localNameLength + localExtraLength;
    if (payloadOffset % 64) fail(`member data is not 64-byte aligned: ${name}`);
    if (payloadOffset + compressed > data.length || compressed !== uncompressed) fail(`member bounds or size mismatch: ${name}`);
    if (crc32(data.subarray(payloadOffset, payloadOffset + uncompressed)) !== checksum) fail(`member CRC mismatch: ${name}`);
    names.add(name); entries.push({ name, compressedSize: compressed, uncompressedSize: uncompressed, dataOffset: payloadOffset });
    cursor = nameStart + nameLength + extraLength + commentLength;
  }
  if (cursor !== centralOffset + centralSize) fail('central-directory size mismatch');
  return { entries };
}
const USD_KINDS = new Set(['assembly', 'component', 'group', 'model', 'subcomponent']);
const USD_PURPOSES = new Set(['default', 'render', 'proxy', 'guide']);
const USD_REPAIR_KEYS = new Set(['defaultPrim', 'upAxis', 'metersPerUnit', 'kind', 'purpose']);
const validUsdIdentifier = (value) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(String(value));
const safePackagePath = (value) => {
  const path = String(value || '').replaceAll('\\', '/');
  return Boolean(path) && !/[\u0000-\u001f\u007f]/.test(path) && !unsafeAssetReference(path) && !resolverAssetReference(path) && normalizeAssetPath(path) === path && !path.endsWith('/');
};
const stripCommentsAndStrings = (source) => {
  const text = String(source || ''); let output = '', quote = false, escaped = false, comment = false;
  for (const character of text) {
    if (comment) { if (character === '\n' || character === '\r') { comment = false; output += character; } continue; }
    if (quote) { if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') { quote = false; output += '""'; } continue; }
    if (character === '"') { quote = true; output += '"'; } else if (character === '#') comment = true; else output += character;
  }
  return output;
};
const structuralSource = (source) => {
  const text = String(source || ''); let output = '', quote = false, escaped = false, comment = false;
  for (const character of text) {
    if (comment) { output += character === '\n' || character === '\r' ? character : ' '; if (character === '\n' || character === '\r') comment = false; continue; }
    if (quote) { output += character === '\n' || character === '\r' ? character : ' '; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '#') { comment = true; output += ' '; } else if (character === '"') { quote = true; output += ' '; } else output += character;
  }
  return output;
};
const layerMetadataRange = (source) => {
  const text = String(source || ''), headerEnd = text.indexOf('\n');
  if (headerEnd < 0) return null;
  const structural = structuralSource(text), searchStart = headerEnd + 1, open = structural.indexOf('(', searchStart);
  if (open < 0 || !/^(?:\s|#[^\r\n]*(?:\r?\n|$))*$/.test(text.slice(searchStart, open))) return null;
  let depth = 1, close = open + 1;
  while (close < structural.length && depth) { if (structural[close] === '(') depth++; else if (structural[close] === ')') depth--; close++; }
  return depth ? null : { open, close };
};
const layerMetadataBody = (source) => { const text = String(source || ''), range = layerMetadataRange(text); return range ? text.slice(range.open + 1, range.close - 1) : ''; };
const replaceInvalidQuotedAssignments = (source, key, value, allowed, protectedRange = null) => {
  const text = String(source || ''), structural = structuralSource(text), expression = new RegExp(`(?:^|[^A-Za-z0-9_:])${key}\\s*=\\s*`, 'g'), replacements = [];
  let match;
  while ((match = expression.exec(structural))) {
    if (protectedRange && match.index >= protectedRange.open && match.index < protectedRange.close) continue;
    const equals = text.indexOf('=', match.index), quoteStart = equals < 0 ? -1 : equals + 1 + (text.slice(equals + 1).match(/^[ \t]*/) || [''])[0].length;
    if (text[quoteStart] !== '"') continue;
    let quoteEnd = quoteStart + 1, escaped = false;
    for (; quoteEnd < text.length; quoteEnd++) { const character = text[quoteEnd]; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') break; }
    if (quoteEnd < text.length && !allowed.has(text.slice(quoteStart + 1, quoteEnd))) replacements.push({ start: quoteStart + 1, end: quoteEnd, value: String(value) });
  }
  let output = text;
  for (const replacement of replacements.reverse()) output = output.slice(0, replacement.start) + replacement.value + output.slice(replacement.end);
  return output;
};
const quotedAssignmentValues = (source, key, protectedRange = null) => {
  const text = String(source || ''), structural = structuralSource(text), expression = new RegExp(`(?:^|[^A-Za-z0-9_:])${key}\\s*=\\s*`, 'g'), values = [];
  let match;
  while ((match = expression.exec(structural))) {
    if (protectedRange && match.index >= protectedRange.open && match.index < protectedRange.close) continue;
    const equals = text.indexOf('=', match.index), quoteStart = equals < 0 ? -1 : equals + 1 + (text.slice(equals + 1).match(/^[ \t]*/) || [''])[0].length;
    if (text[quoteStart] !== '"') continue;
    let quoteEnd = quoteStart + 1, escaped = false;
    for (; quoteEnd < text.length; quoteEnd++) { const character = text[quoteEnd]; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') break; }
    if (quoteEnd < text.length) values.push(text.slice(quoteStart + 1, quoteEnd));
  }
  return values;
};
function discoverInactivePrimPaths(source) {
  const text = String(source || ''), structural = structuralSource(text), declarations = [], declarationPattern = /\b(?:def|over|class)\s+(?:[A-Za-z_][\w:]*)?\s*"([^"\r\n]+)"\s*(?:\([^{}]*\))?\s*\{/g;
  let match;
  while ((match = declarationPattern.exec(text))) {
    const open = structural.indexOf('{', match.index + match[0].length - 1);
    if (open < 0) continue;
    let depth = 0, close = -1;
    for (let index = open; index < structural.length; index++) {
      if (structural[index] === '{') depth++;
      else if (structural[index] === '}' && --depth === 0) { close = index; break; }
    }
    if (close >= 0) declarations.push({ name: match[1], start: match.index, open, close });
  }
  const pathFor = (declaration) => declarations.filter((candidate) => candidate.start < declaration.start && candidate.close > declaration.close).sort((a, b) => b.start - a.start).map((candidate) => candidate.name).concat(declaration.name).join('/');
  const paths = new Set();
  for (const inactive of structural.matchAll(/\bactive\s*=\s*false\b/g)) {
    const declaration = declarations.filter((candidate) => candidate.start < inactive.index && candidate.close > inactive.index).sort((a, b) => b.start - a.start)[0];
    if (declaration) paths.add(`/${pathFor(declaration)}`);
  }
  return [...paths].sort();
}
function discoverRootPrimNames(source) {
  return discoverPrimRanges(source).filter((range) => range.path.split('/').filter(Boolean).length === 1).map((range) => range.name);
}

function discoverPrimRanges(source) {
  const text = String(source || ''), structural = structuralSource(text), declarations = [], pattern = /\b(?:def|over|class)\s+(?:[A-Za-z_][\w:]*)?\s*"([^"\r\n]+)"\s*(?:\([^{}]*\))?\s*\{/g;
  let match;
  while ((match = pattern.exec(text))) {
    if (structural[match.index] === ' ') continue;
    const open = match.index + match[0].lastIndexOf('{'); let depth = 1, close = open + 1;
    while (close < structural.length && depth) { if (structural[close] === '{') depth++; else if (structural[close] === '}') depth--; close++; }
    if (depth) continue;
    declarations.push({ name: match[1], start: match.index, open, close });
  }
  return declarations.map((declaration) => ({ ...declaration, path: `/${declarations.filter((candidate) => candidate.start < declaration.start && candidate.close >= declaration.close).sort((a, b) => b.start - a.start).map((candidate) => candidate.name).concat(declaration.name).join('/')}` }));
}

export function diagnoseUSD(source, assets = new Map(), options = {}) {
  const rawAssets = assets instanceof Map ? assets : assets && typeof assets === 'object' ? new Map(Object.entries(assets)) : new Map();
  assets = new Map([...rawAssets].filter(([path]) => typeof path === 'string' && path));
  const resolverPlugins = normalizeResolverPlugins(options?.resolverPlugins), resolverPluginMap = new Map(resolverPlugins.map((plugin) => [plugin.scheme, plugin])), text = String(source || ''), issues = [];
  const header = text.match(/#usda\s+([\d.]+)/);
  if (!header) issues.push(issue('error', 'usd.header', '/', 'Missing USDA file header.'));
  const metadataBody = layerMetadataBody(text), defaultPrimValue = quotedAssignmentValues(metadataBody, 'defaultPrim')[0] || null, defaultPrim = defaultPrimValue == null ? null : { 1: defaultPrimValue };
  const rootPrimNames = discoverRootPrimNames(text);
  if (!defaultPrim) issues.push(issue('warning', 'usd.missingDefaultPrim', '/', 'Stage has no defaultPrim; external consumers may not know what to open.'));
  else if (!rootPrimNames.includes(defaultPrim[1])) issues.push(issue('error', 'usd.invalidDefaultPrim', '/', `defaultPrim "${defaultPrim[1]}" does not name a root prim.`));
  const upAxisValue = quotedAssignmentValues(metadataBody, 'upAxis')[0] || null, upAxis = upAxisValue == null ? null : { 1: upAxisValue };
  if (!upAxis) issues.push(issue('warning', 'usd.missingUpAxis', '/', 'Stage does not declare upAxis.'));
  else if (!['Y', 'Z'].includes(upAxis[1])) issues.push(issue('error', 'usd.invalidUpAxis', '/', `Unsupported upAxis "${upAxis[1]}".`));
  const meters = metadataBody.match(/\bmetersPerUnit\s*=\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)/);
  if (!meters) issues.push(issue('warning', 'usd.missingMetersPerUnit', '/', 'Stage does not declare metersPerUnit.'));
  else if (!(Number(meters[1]) > 0)) issues.push(issue('error', 'usd.invalidMetersPerUnit', '/', 'metersPerUnit must be greater than zero.'));
  const kindValues = quotedAssignmentValues(text, 'kind', layerMetadataRange(text)), purposeValues = quotedAssignmentValues(text, 'purpose', layerMetadataRange(text));
  for (const value of kindValues) if (!USD_KINDS.has(value)) issues.push(issue('warning', 'usd.invalidKind', '/', `Unsupported kind "${value}"; expected assembly, component, group, model, or subcomponent.`));
  for (const value of purposeValues) if (!USD_PURPOSES.has(value)) issues.push(issue('warning', 'usd.invalidPurpose', '/', `Unsupported purpose "${value}"; expected default, render, proxy, or guide.`));
  const materialDeclarationMask = structuralSource(text), materialDefinitions = [...text.matchAll(/\bdef\s+Material\s+"([^"]+)"\s*\{/g)].filter((match) => materialDeclarationMask[match.index] !== ' ').map((match) => match[1]);
  const inactivePrimPaths = discoverInactivePrimPaths(text), inactivePrimCount = [...stripCommentsAndStrings(text).matchAll(/\bactive\s*=\s*false\b/g)].length;
  if (inactivePrimCount) issues.push(issue('info', 'usd.inactivePrim', '/', `${inactivePrimCount} prim${inactivePrimCount === 1 ? ' is' : 's are'} authored inactive and will not participate in normal composition.`));
  const materialBindingSource = structuralSource(text);
  const primRanges = discoverPrimRanges(text), bindingPath = (index) => primRanges.filter((range) => range.start < index && range.close > index).sort((a, b) => b.start - a.start)[0]?.path || '/';
  const materialBindings = [...materialBindingSource.matchAll(/\bmaterial:binding\s*=\s*<([^>]+)>/g)].map((match) => match[1].split('/').filter(Boolean).at(-1)).filter(Boolean);
  const boundMaterials = new Set(materialBindings);
  const collectionMaterialBindings = [...materialBindingSource.matchAll(/\bmaterial:binding:collection(?::[A-Za-z_][\w]*)?\s*=/g)];
  if (collectionMaterialBindings.length) issues.push(issue('info', 'usd.collectionMaterialBinding', collectionMaterialBindings.length === 1 ? bindingPath(collectionMaterialBindings[0].index) : '/', `${collectionMaterialBindings.length} collection material binding${collectionMaterialBindings.length === 1 ? '' : 's'} require composed collection evaluation; explicit material usage cannot be determined from this layer alone.`));
  const inheritedMaterialBindings = [...materialBindingSource.matchAll(/\b(?:inherits|specializes)\s*=\s*</g)];
  if (inheritedMaterialBindings.length) issues.push(issue('info', 'usd.inheritedMaterialBinding', inheritedMaterialBindings.length === 1 ? bindingPath(inheritedMaterialBindings[0].index) : '/', `${inheritedMaterialBindings.length} inherits/specializes arc${inheritedMaterialBindings.length === 1 ? '' : 's'} may provide material bindings through composition; explicit material usage cannot be determined from this layer alone.`));
  const inferredInheritedBindings = inferInheritedMaterialBindings(text), materialBindingReview = [...collectionMaterialBindings.map((match) => ({ kind: 'collection', authored: match[0].split('=')[0].trim(), path: bindingPath(match.index), automaticFixSafe: false, reason: 'Requires composed collection membership evaluation.' })), ...inheritedMaterialBindings.map((match) => ({ kind: 'inheritance', authored: match[0].split('=')[0].trim(), path: bindingPath(match.index), automaticFixSafe: false, reason: 'Requires composed inherits/specializes evaluation.' })), ...inferredInheritedBindings.map((candidate) => ({ kind: 'inferred-inheritance', authored: `${candidate.arcKind} ${candidate.sourcePath}`, path: candidate.path, materialPath: candidate.materialPath, automaticFixSafe: false, reason: 'Same-layer evidence suggests a direct override; review composition strength before authoring.' }))];
  for (const name of boundMaterials) if (!materialDefinitions.includes(name)) issues.push(issue('info', 'usd.externalMaterialBinding', '/', `Material binding targets ${name}, which is not defined in this layer; inherited or referenced material definitions may provide it.`));
  if (!collectionMaterialBindings.length && !inheritedMaterialBindings.length) for (const name of materialDefinitions) if (!boundMaterials.has(name)) issues.push(issue('info', 'usd.unusedMaterial', `/${name}`, `Material ${name} has no explicit material:binding in this layer; inherited or collection bindings are not evaluated.`));
  const seen = new Set(), dependencies = new Map(), normalizedPackagePaths = new Map();
  for (const path of assets.keys()) { const normalized = normalizeAssetPath(path); if (normalizedPackagePaths.has(normalized)) issues.push(issue('error', 'usdz.pathCollision', '/', `Package members collide after path normalization: ${normalizedPackagePaths.get(normalized)} and ${path}.`)); else normalizedPackagePaths.set(normalized, path); }
  const dependencySource = structuralSource(text);
  for (const match of dependencySource.matchAll(/@([^@]+)@/g)) {
    const path = text.slice(match.index + 1, match.index + match[0].length - 1).trim(); if (!path || seen.has(path)) continue; seen.add(path);
    const context = text.slice(Math.max(0, match.index - 120), match.index);
    const kindMatches = [...context.matchAll(/\b(references|payload|subLayers|asset)\b/gi)];
    const kind = kindMatches.at(-1)?.[1].toLowerCase() || 'asset';
    const present = path.startsWith('anon:') || (!unsafeAssetReference(path) && (assets.has(path) || normalizedPackagePaths.has(normalizeAssetPath(path))));
    dependencies.set(path, { path, kind, present, status: dependencyStatus(path, present) });
    if (/^(?:[A-Za-z]:[\\/]|\/)/.test(path)) issues.push(issue('warning', 'usd.absoluteAssetPath', '/', `Asset path is absolute: ${path}`));
    if (/^(?:\.\.\/?|\.\.\\)/.test(path)) issues.push(issue('warning', 'usd.parentAssetPath', '/', `Asset path escapes its package: ${path}`));
    if (resolverAssetReference(path)) issues.push(issue('info', 'usd.resolverDependency', '/', `Asset path requires an external resolver: ${path}`));
    if (!present) issues.push(issue('warning', 'usd.unresolvedAsset', '/', `Referenced asset is not present in the current package: ${path}`));
  }
  const packageAssets = [...assets.entries()].map(([path, asset]) => ({ path, bytes: asset?.bytes?.byteLength ?? asset?.bytes?.length ?? 0, referenced: seen.has(path) || [...dependencies.keys()].some((dependency) => normalizeAssetPath(dependency) === normalizeAssetPath(path)) })).sort((a, b) => a.path.localeCompare(b.path));
  const payloads = new Map();
  for (const asset of packageAssets) {
    if (!asset.path || asset.path.startsWith('/') || asset.path.includes('\\') || asset.path.split('/').includes('..')) issues.push(issue('error', 'usdz.unsafeAssetPath', '/', `Package member path is unsafe: ${asset.path}`));
    const dot = asset.path.lastIndexOf('.'), extension = dot >= 0 ? asset.path.slice(dot).toLowerCase() : '';
    if (!USDZ_ALLOWED_EXTENSIONS.has(extension)) issues.push(issue('error', 'usdz.unsupportedAssetType', '/', `Package member type is not allowed in a portable USDZ package: ${asset.path}`));
    if (!asset.bytes) issues.push(issue('error', 'usdz.emptyAsset', '/', `Package member is empty: ${asset.path}`));
    if (!asset.referenced) issues.push(issue('warning', 'usd.orphanedAsset', '/', `Package member is not referenced by the current layer: ${asset.path}`));
    const source = assets.get(asset.path)?.bytes, hash = `${asset.bytes}:${bytesHash(source)}`;
    if (source && payloads.has(hash)) issues.push(issue('warning', 'usdz.duplicateAsset', '/', `Package member duplicates ${payloads.get(hash)} byte-for-byte: ${asset.path}`));
    else if (source) payloads.set(hash, asset.path);
  }
  // Keep the graph deliberately small and serializable: the current layer is
  // the root, composition references are directed edges, and unreferenced
  // package members remain visible as disconnected orphan nodes.
  const dependencyGraphNodes = [{ id: 'root', kind: 'layer', path: null, present: true, status: 'available' }];
  const dependencyGraphEdges = [];
  const graphNodeIds = new Set(['root']);
  const graphEdgeIds = new Set();
  const graphQueue = [{ id: 'root', text }], graphQueued = new Set(['root']);
  const packageEntries = new Map([...assets.entries()].map(([path, asset]) => [normalizeAssetPath(path), asset]));
  const packageAssetByPath = new Map(packageAssets.map((asset) => [normalizeAssetPath(asset.path), asset]));
  const packageEntry = (path) => assets.get(path) || packageEntries.get(normalizeAssetPath(path));
  const resolveGraphPath = (reference, fromPath) => {
    const value = String(reference || '').trim();
    if (!value || /^(?:anon:|[A-Za-z]:[\\/]|[\\/])/.test(value) || resolverAssetReference(value) || !fromPath) return value;
    const directory = fromPath.includes('/') ? fromPath.slice(0, fromPath.lastIndexOf('/')) : '';
    return normalizeAssetPath(directory ? `${directory}/${value}` : value);
  };
  const graphReferences = (layer) => { const text = String(layer || ''), structural = structuralSource(text); return [...structural.matchAll(/@([^@]+)@/g)].map((match) => {
    const path = text.slice(match.index + 1, match.index + match[0].length - 1).trim(), context = text.slice(Math.max(0, match.index - 120), match.index);
    const kind = [...context.matchAll(/\b(references|payload|subLayers|asset)\b/gi)].at(-1)?.[1].toLowerCase() || 'asset';
    return { path, kind };
  }).filter(({ path }) => path); };
  const addGraphReference = (from, dependency) => {
    const knownDependency = dependencies.get(dependency.path);
    if (!knownDependency) {
      dependencies.set(dependency.path, { path: dependency.path, kind: dependency.kind, present: dependency.present, status: dependencyStatus(dependency.path, dependency.present) });
      seen.add(dependency.path);
      const packageAsset = packageAssetByPath.get(normalizeAssetPath(dependency.path));
      if (packageAsset) packageAsset.referenced = true;
      const issuePath = from === 'root' ? '/' : from.replace(/^asset:/, '');
      if (/^(?:[A-Za-z]:[\\/]|\/)/.test(dependency.path)) issues.push(issue('warning', 'usd.absoluteAssetPath', issuePath, `Asset path is absolute: ${dependency.path}`));
      if (/^(?:\.\.\/?|\.\.\\)/.test(dependency.path)) issues.push(issue('warning', 'usd.parentAssetPath', issuePath, `Asset path escapes its package: ${dependency.path}`));
      if (resolverAssetReference(dependency.path)) issues.push(issue('info', 'usd.resolverDependency', issuePath, `Asset path requires an external resolver: ${dependency.path}`));
      if (!dependency.present) issues.push(issue('warning', 'usd.unresolvedAsset', issuePath, `Referenced asset is not present in the current package: ${dependency.path}`));
    }
    const id = `asset:${dependency.path}`;
    if (!graphNodeIds.has(id)) {
      dependencyGraphNodes.push({ id, kind: dependency.kind, path: dependency.path, present: dependency.present, status: dependency.status || dependencyStatus(dependency.path, dependency.present) });
      graphNodeIds.add(id);
    }
    const edgeId = `${from}\0${id}\0${dependency.kind}`;
    if (!graphEdgeIds.has(edgeId)) {
      dependencyGraphEdges.push({ from, to: id, kind: dependency.kind, status: dependency.status || dependencyStatus(dependency.path, dependency.present) });
      graphEdgeIds.add(edgeId);
    }
    if (dependency.present && !graphQueued.has(id)) {
      const entry = packageEntry(dependency.path), bytes = entry?.bytes;
      if (/\.usd(a)?$/i.test(dependency.path) && bytes) {
        try { graphQueue.push({ id, path: dependency.path, text: new TextDecoder().decode(bytes) }); graphQueued.add(id); } catch { /* Binary or malformed members stay terminal. */ }
      }
    }
  };
  for (const dependency of dependencies.values()) addGraphReference('root', dependency);
  for (let cursor = 0; cursor < graphQueue.length; cursor++) {
    const layer = graphQueue[cursor];
    for (const reference of graphReferences(layer.text)) {
      const resolvedPath = resolveGraphPath(reference.path, layer.id === 'root' ? '' : layer.path);
      const present = resolvedPath.startsWith('anon:') || (!unsafeAssetReference(resolvedPath) && (assets.has(resolvedPath) || normalizedPackagePaths.has(normalizeAssetPath(resolvedPath))));
      addGraphReference(layer.id, { ...reference, path: resolvedPath, present });
    }
  }
  for (const asset of packageAssets) {
    const id = `asset:${asset.path}`;
    if (!graphNodeIds.has(id)) { dependencyGraphNodes.push({ id, kind: 'packageAsset', path: asset.path, present: true, status: 'available' }); graphNodeIds.add(id); }
  }
  // A recursive layer graph can otherwise look fully resolved while still
  // being unloadable by consumers that reject cyclic composition. Report each
  // back edge once, with a stable node path, without rewriting authored data.
  const graphAdjacency = new Map();
  for (const edge of dependencyGraphEdges) {
    if (!graphAdjacency.has(edge.from)) graphAdjacency.set(edge.from, []);
    graphAdjacency.get(edge.from).push(edge.to);
  }
  for (const targets of graphAdjacency.values()) targets.sort();
  const graphState = new Map(), cycleEdges = new Set();
  const visitGraph = (start) => {
    graphState.set(start, 1);
    const stack = [{ node: start, cursor: 0 }];
    while (stack.length) {
      const frame = stack.at(-1), targets = graphAdjacency.get(frame.node) || [];
      if (frame.cursor >= targets.length) { graphState.set(frame.node, 2); stack.pop(); continue; }
      const target = targets[frame.cursor++], targetState = graphState.get(target);
      if (targetState === 1) cycleEdges.add(`${frame.node}\0${target}`);
      else if (!targetState) { graphState.set(target, 1); stack.push({ node: target, cursor: 0 }); }
    }
  };
  for (const node of dependencyGraphNodes.map((item) => item.id).sort()) if (!graphState.has(node)) visitGraph(node);
  for (const edge of [...cycleEdges].sort()) {
    const [from, to] = edge.split('\0'), node = dependencyGraphNodes.find((item) => item.id === to);
    issues.push(issue('error', 'usd.dependencyCycle', node?.path || '/', `Composition dependency cycle detected from ${from} to ${to}.`));
  }
  dependencyGraphNodes.sort((a, b) => a.id.localeCompare(b.id));
  dependencyGraphEdges.sort((a, b) => `${a.from}\0${a.to}\0${a.kind}`.localeCompare(`${b.from}\0${b.to}\0${b.kind}`));
  issues.sort((a, b) => `${a.path}\0${a.ruleId}`.localeCompare(`${b.path}\0${b.ruleId}`));
  const resolverCapability = (dependency) => { const path = String(dependency.path).toLowerCase(); if (['references', 'payload', 'sublayers'].includes(dependency.kind) || /\.(?:usd|usda|usdc)$/.test(path)) return 'usd'; if (/\.(?:png|jpe?g|exr|hdr|tiff?|ktx2?|basis)$/.test(path)) return 'texture'; return null; };
  const resolverDependencies = [...dependencies.values()].filter((dependency) => dependency.status === 'resolver').map((dependency) => { const scheme = String(dependency.path).match(/^([A-Za-z][A-Za-z0-9+.-]*):/)?.[1].toLowerCase() || '', plugin = resolverPluginMap.get(scheme), requiredCapability = resolverCapability(dependency), capabilitySupported = !requiredCapability || Boolean(plugin?.capabilities?.includes(requiredCapability)); return { path: dependency.path, scheme, registered: Boolean(plugin), ...(plugin ? { plugin: plugin.name } : {}), ...(requiredCapability ? { requiredCapability, capabilitySupported } : {}) }; }).sort((a, b) => a.path.localeCompare(b.path));
  for (const dependency of resolverDependencies) { if (!dependency.registered) issues.push(issue('warning', 'usd.unregisteredResolver', '/', 'No host resolver plugin is registered for scheme "' + dependency.scheme + '" used by ' + dependency.path + '.')); else if (dependency.requiredCapability && !dependency.capabilitySupported) issues.push(issue('warning', 'usd.resolverCapability', '/', 'Host resolver "' + dependency.plugin + '" does not advertise the required ' + dependency.requiredCapability + ' capability for ' + dependency.path + '.')); }
  issues.sort((a, b) => String(a.path).localeCompare(String(b.path)) || String(a.ruleId).localeCompare(String(b.ruleId)));
  const score = Math.max(0, 100 - issues.reduce((sum, item) => sum + (item.severity === 'error' ? 25 : 8), 0));
  const dependencyStatusCounts = Object.fromEntries(['available', 'missing', 'unsafe', 'resolver', 'anonymous'].map((status) => [status, [...dependencies.values()].filter((dependency) => (dependency.status || dependencyStatus(dependency.path, dependency.present)) === status).length]));
  const orphanMaterials = collectionMaterialBindings.length || inheritedMaterialBindings.length ? [] : [...new Set(materialDefinitions)].filter((name) => !boundMaterials.has(name)).sort();
  const orphanAssets = packageAssets.filter((asset) => !asset.referenced).map((asset) => asset.path).sort();
  // Resolver dependencies are computed before issue scoring so missing host
  // registrations are visible in the Doctor score.
  return { format: header?.[1] ? `USDA ${header[1]}` : 'Unknown', defaultPrim: defaultPrim?.[1] || null, rootPrims: [...new Set(rootPrimNames)], upAxis: upAxis?.[1] || null, metersPerUnit: meters ? Number(meters[1]) : null, kindValues: [...new Set(kindValues)].sort(), purposeValues: [...new Set(purposeValues)].sort(), inactivePrimCount, inactivePrimPaths, materials: { definitions: [...new Set(materialDefinitions)].sort(), explicitBindings: [...boundMaterials].sort() }, orphanMaterials, materialBindingReview, ...(inferredInheritedBindings.length ? { inferredInheritedBindings } : {}), referencedAssets: [...seen].sort(), orphanAssets, dependencyStatusCounts, dependencies: [...dependencies.values()].sort((a, b) => a.path.localeCompare(b.path)), ...(resolverPlugins.length ? { resolverPlugins, resolverDependencies } : {}), packageAssets, dependencyGraph: { nodes: dependencyGraphNodes, edges: dependencyGraphEdges }, issues, score };
}

export function repairUSDMetadata(source, fixes = {}) {
  fixes = fixes && typeof fixes === 'object' && !Array.isArray(fixes) ? fixes : {};
  let text = String(source || ''), metadataRange = layerMetadataRange(text), rootPrimNames = discoverRootPrimNames(text);
  let syntheticMetadata = false;
  if (!metadataRange) {
    const header = text.match(/#usda[^\n]*\n/);
    if (!header) throw new Error('USDA metadata block was not found.');
    metadataRange = { open: header.index + header[0].length, close: header.index + header[0].length, synthetic: true };
    syntheticMetadata = true;
  }
  let body = syntheticMetadata ? '' : text.slice(metadataRange.open + 1, metadataRange.close - 1), stageChange = false;
  for (const [key, value] of Object.entries(fixes)) {
    if (value == null || !USD_REPAIR_KEYS.has(key)) continue;
    if (key === 'kind' || key === 'purpose') continue;
    if (key === 'defaultPrim' && (!validUsdIdentifier(value) || !rootPrimNames.includes(String(value)))) continue;
    if (key === 'upAxis' && !['Y', 'Z'].includes(String(value))) continue;
    if (key === 'kind' && !USD_KINDS.has(String(value))) continue;
    if (key === 'purpose' && !USD_PURPOSES.has(String(value))) continue;
    if (key === 'metersPerUnit' && !(Number.isFinite(Number(value)) && Number(value) > 0)) continue;
    const rendered = key === 'defaultPrim' || key === 'upAxis' ? `"${value}"` : String(value);
    const line = new RegExp(`(^|\\n)([ \\t]*)${key}\\s*=\\s*[^\\n]+`, 'm');
    const replacement = `$1$2${key} = ${rendered}`;
    body = line.test(body) ? body.replace(line, replacement) : `${body.trimEnd()}\n    ${key} = ${rendered}\n`;
    stageChange = true;
  }
  if (!syntheticMetadata && stageChange) text = text.slice(0, metadataRange.open + 1) + body + text.slice(metadataRange.close - 1);
  else if (syntheticMetadata && stageChange) text = text.slice(0, metadataRange.open) + '(' + body + ')\n' + text.slice(metadataRange.open);
  const protectedMetadata = layerMetadataRange(text);
  for (const [key, value] of [['kind', fixes.kind], ['purpose', fixes.purpose]]) {
    if (value == null || (key === 'kind' ? !USD_KINDS.has(String(value)) : !USD_PURPOSES.has(String(value)))) continue;
    text = replaceInvalidQuotedAssignments(text, key, value, key === 'kind' ? USD_KINDS : USD_PURPOSES, protectedMetadata);
  }
  return text;
}

// Author a reviewed direct binding on prims whose effective material binding
// came from an inherits/specializes arc. The Doctor cannot safely compose
// arbitrary layers in the browser, so callers must provide the composed result
// explicitly; this function never infers or edits collection membership.
export function repairInheritedMaterialBindings(source, repairs = []) {
  if (!Array.isArray(repairs) || !repairs.length) throw new Error('Inherited material binding repairs require a non-empty reviewed list.');
  const text = String(source || ''), ranges = discoverPrimRanges(text), byPath = new Map(ranges.map((range) => [range.path, range])), normalized = repairs.map((repair) => ({ path: repair?.path, materialPath: repair?.materialPath }));
  if (normalized.some(({ path, materialPath }) => !validPrimPath(path) || !validPrimPath(materialPath))) throw new Error('Inherited material binding repairs require absolute prim and material paths.');
  if (new Set(normalized.map(({ path }) => path)).size !== normalized.length) throw new Error('Inherited material binding repairs must contain one entry per prim path.');
  for (const { path } of normalized) if (!byPath.has(path)) throw new Error(`Inherited material binding repair target was not found: ${path}`);
  let output = text, changed = 0;
  for (const repair of normalized.map((item) => ({ ...item, range: byPath.get(item.path) })).sort((a, b) => b.range.start - a.range.start)) {
    const bodyStart = repair.range.open + 1, bodyEnd = repair.range.close - 1, body = output.slice(bodyStart, bodyEnd), structural = structuralSource(body), matches = [...structural.matchAll(/\b(?:rel\s+)?material:binding\s*=\s*<([^>]+)>/g)];
    if (matches.length > 1) throw new Error(`Inherited material binding repair found multiple direct bindings on ${repair.path}.`);
    if (matches.length && matches[0][1] !== repair.materialPath) throw new Error(`Inherited material binding repair would overwrite an explicit binding on ${repair.path}.`);
    if (matches.length) continue;
    const line = output.slice(0, repair.range.start).match(/(?:^|\n)([ \t]*)[^\n]*$/), indent = `${line?.[1] || ''}    `, insert = `${body.endsWith('\n') ? '' : '\n'}${indent}rel material:binding = <${repair.materialPath}>${body.endsWith('\n') ? '' : '\n'}`;
    output = output.slice(0, bodyEnd) + insert + output.slice(bodyEnd);
    changed++;
  }
  return { source: output, changed, repairs: normalized.map((repair) => ({ ...repair })) };
}

// Read-only same-layer evidence for inherited binding repair. Composition from
// external layers and collection membership remain intentionally unresolved.
export function inferInheritedMaterialBindings(source) {
  const text = String(source || ''), structural = structuralSource(text), ranges = discoverPrimRanges(text), byPath = new Map(ranges.map((range) => [range.path, range])), owner = (index) => ranges.filter((candidate) => candidate.start < index && candidate.close > index).sort((a, b) => b.start - a.start)[0] || null;
  const directBinding = (range) => [...structural.matchAll(/\b(?:rel\s+)?material:binding\s*=\s*<([^>]+)>/g)].filter((match) => owner(match.index) === range).map((match) => match[1]).filter((path) => validPrimPath(path));
  const arcs = (range) => [...structural.matchAll(/\b(inherits|specializes)\s*=\s*<([^>]+)>/g)].filter((match) => owner(match.index) === range);
  const bindingMemo = new Map(), resolving = new Set(), resolveBinding = (range) => {
    if (!range || resolving.has(range.path)) return null;
    if (bindingMemo.has(range.path)) return bindingMemo.get(range.path);
    resolving.add(range.path);
    const direct = [...new Set(directBinding(range))], inherited = direct.length ? [] : arcs(range).map((match) => resolveBinding(byPath.get(match[2]))).filter(Boolean), values = [...new Set([...direct, ...inherited])], result = values.length === 1 ? values[0] : null;
    resolving.delete(range.path); bindingMemo.set(range.path, result); return result;
  };
  const candidates = [];
  for (const match of structural.matchAll(/\b(inherits|specializes)\s*=\s*<([^>]+)>/g)) {
    const range = owner(match.index);
    if (!range) continue;
    const sourcePath = match[2], materialPath = resolveBinding(byPath.get(sourcePath));
    if (!materialPath || !validPrimPath(range.path)) continue;
    candidates.push({ path: range.path, materialPath, sourcePath, arcKind: match[1] });
  }
  const unique = new Map(candidates.map((candidate) => [`${candidate.path}\n${candidate.materialPath}`, candidate]));
  return [...unique.values()].sort((a, b) => a.path.localeCompare(b.path) || a.materialPath.localeCompare(b.materialPath) || a.arcKind.localeCompare(b.arcKind));
}

// Localization is deliberately explicit: callers must provide the chosen
// package names, so a confirmation UI can show every path that will change.
// Only asset references are rewritten; authored resolver, absolute, and
// parent-traversal references remain untouched unless explicitly mapped to a
// safe package-relative destination.
export function localizeUSDDependencies(source, pathMap = {}) {
  const entries = Object.entries(pathMap && typeof pathMap === 'object' && !Array.isArray(pathMap) ? pathMap : {}).filter(([from, to]) => String(from) !== String(to));
  const destinations = new Set();
  for (const [from, to] of entries) {
    if (!String(from) || /[\u0000-\u001f\u007f]/.test(String(from)) || String(from).includes('@') || !safePackagePath(to)) throw new Error('USD dependency localization requires safe dependency path keys and package-relative destinations.');
    if (destinations.has(String(to))) throw new Error(`USD dependency localization has a destination collision: ${to}`);
    destinations.add(String(to));
  }
  const replacements = new Map(entries);
  let changed = 0;
  const original = String(source || ''), structural = structuralSource(original), expression = /@([^@]+)@/g, edits = [];
  let match;
  while ((match = expression.exec(structural))) {
    const rawPath = original.slice(match.index + 1, match.index + match[0].length - 1), path = String(rawPath).trim(), replacement = replacements.get(path);
    if (replacement != null) edits.push({ start: match.index, end: match.index + match[0].length, value: `@${replacement}@` });
  }
  let text = original;
  for (const edit of edits.reverse()) { text = text.slice(0, edit.start) + edit.value + text.slice(edit.end); changed++; }
  return { source: text, changed, mappings: entries.map(([from, to]) => ({ from, to })) };
}

export function createDependencyManifest(report, sourceName = 'scene.usda', qualityGate = null, activity = []) {
  if (qualityGate != null && !validateQualityGate(qualityGate)) throw new Error('Invalid quality-gate schema in dependency manifest.');
  const safeSourceName = String(sourceName || 'scene.usda').replaceAll('\\', '/').split('/').filter(Boolean).at(-1) || 'scene.usda';
  const sourceReport = report && typeof report === 'object' && !Array.isArray(report) ? report : {}, dependencyMap = new Map((Array.isArray(sourceReport.dependencies) ? sourceReport.dependencies : []).filter((dependency) => dependency && typeof dependency === 'object' && typeof dependency.path === 'string' && dependency.path).map((dependency) => [dependency.path, { path: dependency.path, kind: typeof dependency.kind === 'string' ? dependency.kind : 'asset', present: dependency.present === true, status: normalizedDependencyStatus(dependency.path, dependency.present === true, dependency.status) }]));
  const rawGraph = sourceReport.dependencyGraph && typeof sourceReport.dependencyGraph === 'object' && !Array.isArray(sourceReport.dependencyGraph) ? sourceReport.dependencyGraph : {}, graph = { nodes: (Array.isArray(rawGraph.nodes) ? rawGraph.nodes : []).filter((node) => node && typeof node === 'object' && typeof node.id === 'string' && node.id && (node.path == null || typeof node.path === 'string')).sort((a, b) => a.id.localeCompare(b.id)), edges: (Array.isArray(rawGraph.edges) ? rawGraph.edges : []).filter((edge) => edge && typeof edge === 'object' && typeof edge.from === 'string' && typeof edge.to === 'string' && typeof edge.kind === 'string' && edge.from && edge.to && edge.kind).sort((a, b) => `${a.from}\0${a.to}\0${a.kind}`.localeCompare(`${b.from}\0${b.to}\0${b.kind}`)) };
  for (const node of graph.nodes) if (node && typeof node === 'object' && typeof node.path === 'string' && node.path && !dependencyMap.has(node.path)) dependencyMap.set(node.path, { path: node.path, kind: typeof node.kind === 'string' ? node.kind : 'asset', present: node.present !== false, status: normalizedDependencyStatus(node.path, node.present !== false, node.status) });
  const packageAssets = (Array.isArray(sourceReport.packageAssets) ? sourceReport.packageAssets : []).filter((asset) => asset && typeof asset === 'object' && typeof asset.path === 'string' && asset.path).map((asset) => ({ path: asset.path, bytes: Number.isFinite(Number(asset.bytes)) && Number(asset.bytes) >= 0 ? Number(asset.bytes) : 0, referenced: asset.referenced === true }));
  const orphanMaterials = [...new Set((Array.isArray(sourceReport.orphanMaterials) ? sourceReport.orphanMaterials : []).filter((path) => typeof path === 'string' && path))].sort();
  const orphanAssets = [...new Set((Array.isArray(sourceReport.orphanAssets) ? sourceReport.orphanAssets : []).filter((path) => typeof path === 'string' && path))].sort();
  const inactivePrimPaths = [...new Set((Array.isArray(sourceReport.inactivePrimPaths) ? sourceReport.inactivePrimPaths : []).filter((path) => validPrimPath(path)))].sort();
  const resolverPlugins = normalizeResolverPlugins(sourceReport.resolverPlugins), resolverDependencies = (Array.isArray(sourceReport.resolverDependencies) ? sourceReport.resolverDependencies : []).filter((dependency) => dependency && typeof dependency === 'object' && typeof dependency.path === 'string' && dependency.path && /^[a-z][a-z0-9+.-]*$/i.test(dependency.scheme || '')).map((dependency) => { const plugin = dependency.registered === true && typeof dependency.plugin === 'string' ? dependency.plugin.replace(/[\u0000-\u001f\u007f]/g, ' ').trim() : '', registered = Boolean(plugin), requiredCapability = typeof dependency.requiredCapability === 'string' && /^[a-z][a-z0-9+.-]*$/i.test(dependency.requiredCapability) ? dependency.requiredCapability.toLowerCase() : null; return { path: dependency.path, scheme: String(dependency.scheme).toLowerCase(), registered, ...(registered ? { plugin } : {}), ...(requiredCapability ? { requiredCapability, capabilitySupported: registered && dependency.capabilitySupported === true } : {}) }; }).filter((dependency) => dependency.path && dependency.scheme).sort((a, b) => a.path.localeCompare(b.path));
  const safeReviewText = (value, fallback) => { const text = String(value ?? fallback).replace(/[\u0000-\u001f\u007f]/g, ' ').trim(); return text || fallback; }, materialBindingReview = (Array.isArray(sourceReport.materialBindingReview) ? sourceReport.materialBindingReview : []).filter((item) => item && typeof item === 'object' && ['collection', 'inheritance', 'inferred-inheritance'].includes(item.kind) && typeof item.authored === 'string' && item.authored).map((item) => ({ kind: item.kind, authored: safeReviewText(item.authored, 'binding'), path: validPrimPath(item.path) ? item.path : '/', automaticFixSafe: false, reason: safeReviewText(item.reason, 'Requires composed-stage evaluation.'), ...(validPrimPath(item.materialPath) ? { materialPath: item.materialPath } : {}), ...(validPrimPath(item.sourcePath) ? { sourcePath: item.sourcePath } : {}) })).sort((a, b) => `${a.kind}\0${a.authored}\0${a.path}`.localeCompare(`${b.kind}\0${b.authored}\0${b.path}`));
  const assistantActivity = (Array.isArray(activity) ? activity : []).filter((entry) => entry?.kind === 'assistant' && typeof entry.tool === 'string' && ['proposed', 'declined', 'executed', 'rejected'].includes(entry.decision)).map((entry) => ({ tool: entry.tool, decision: entry.decision, paths: [...new Set((Array.isArray(entry.paths) ? entry.paths : []).filter((path) => typeof path === 'string' && validPrimPath(path)))].sort() }));
  return {
    schemaVersion: 1,
    source: safeSourceName,
    format: sourceReport.format,
    metadata: { defaultPrim: sourceReport.defaultPrim, upAxis: sourceReport.upAxis, metersPerUnit: sourceReport.metersPerUnit },
    dependencies: [...dependencyMap.values()].sort((a, b) => a.path.localeCompare(b.path)),
    packageAssets,
    ...(orphanMaterials.length ? { orphanMaterials } : {}),
    ...(orphanAssets.length ? { orphanAssets } : {}),
    ...(inactivePrimPaths.length ? { inactivePrimPaths } : {}),
    ...(resolverPlugins.length ? { resolverPlugins, ...(resolverDependencies.length ? { resolverDependencies } : {}) } : {}),
    dependencyGraph: graph,
    ...(materialBindingReview.length ? { materialBindingReview } : {}),
    ...(sourceReport.dependencyStatusCounts && typeof sourceReport.dependencyStatusCounts === 'object' && !Array.isArray(sourceReport.dependencyStatusCounts) ? { dependencyStatusCounts: Object.fromEntries([...DEPENDENCY_STATUSES].sort().map((status) => [status, [...dependencyMap.values()].filter((dependency) => dependency.status === status).length])) } : {}),
    issueCount: Array.isArray(sourceReport.issues) ? sourceReport.issues.length : 0,
    score: Number.isFinite(Number(sourceReport.score)) ? Math.max(0, Math.min(100, Number(sourceReport.score))) : 0,
    ...(qualityGate ? { qualityGate } : {}),
    ...(assistantActivity.length ? { assistantActivity } : {}),
  };
}

// Read only the portable subset written by createDependencyManifest.  This is
// intentionally tolerant of future/hand-edited manifests: malformed records
// are ignored instead of becoming executable project state.
export function parseAssistantActivityManifest(manifest) {
  if (!manifest || manifest.schemaVersion !== 1 || !Array.isArray(manifest.assistantActivity)) return [];
  const decisions = new Set(['proposed', 'declined', 'executed', 'rejected']);
  return manifest.assistantActivity.filter((entry) => entry && typeof entry.tool === 'string' && decisions.has(entry.decision)).map((entry) => ({
    kind: 'assistant',
    tool: entry.tool,
    decision: entry.decision,
    paths: [...new Set((Array.isArray(entry.paths) ? entry.paths : []).filter((path) => typeof path === 'string' && validPrimPath(path)))].sort(),
  }));
}
