import { escapeRegExp, LuciaError, validPrimPath } from './utils.js';

function structuralMask(source) {
  const text = String(source || ''), masked = [...text]; let quote = false, escaped = false, comment = false;
  for (let index = 0; index < text.length; index++) {
    const character = text[index];
    if (comment) { if (character === '\n' || character === '\r') comment = false; else masked[index] = ' '; continue; }
    if (quote) { if (character !== '\n' && character !== '\r') masked[index] = ' '; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '#') { comment = true; masked[index] = ' '; } else if (character === '"') { quote = true; masked[index] = ' '; }
  }
  return masked.join('');
}

function findPrimBlock(source, path) {
  const names = String(path || '').split('/').filter(Boolean);
  let range = { start: 0, end: String(source || '').length };
  for (const name of names) {
    const segment = String(source).slice(range.start, range.end);
    const re = new RegExp(`\\b(?:def|over|class)\\s+(?:[A-Za-z_][\\w:]*)?\\s*"${escapeRegExp(name)}"\\s*(?:\\([^{}]*\\)\\s*)?\\{`, 'g');
    const masked = structuralMask(segment); let match;
    while ((match = re.exec(segment)) && masked[match.index] === ' ') {}
    if (!match) return null;
    const open = range.start + match.index + match[0].lastIndexOf('{');
    const sourceMask = structuralMask(source); let depth = 1, close = open + 1;
    while (close < range.end && depth) { if (sourceMask[close] === '{') depth++; else if (sourceMask[close] === '}') depth--; close++; }
    if (depth) return null;
    range = { start: range.start + match.index, open, close, end: close };
  }
  return range;
}

function scanPrimDeclarations(source) {
  const text = String(source || ''), structural = structuralMask(text), declarations = [], re = /\b(?:def|over|class)\s+(?:[A-Za-z_][\w:]*)?\s*"([A-Za-z_][\w]*)"\s*(?:\([^{}]*\)\s*)?\{/g;
  let match;
  while ((match = re.exec(text))) {
    if (structural[match.index] === ' ') continue;
    const open = match.index + match[0].lastIndexOf('{'); let depth = 1, close = open + 1;
    while (close < text.length && depth) { if (structural[close] === '{') depth++; else if (structural[close] === '}') depth--; close++; }
    if (depth) continue;
    declarations.push({ name: match[1], start: match.index, open, close, declaration: match[0] });
  }
  return declarations;
}

function canonicalizeNestedBlocks(text) {
  const source = String(text || ''); let output = '', quote = false, escaped = false;
  for (let index = 0; index < source.length; index++) {
    const character = source[index];
    if (quote) { output += character; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '"') { quote = true; output += character; continue; }
    if (character !== '{') { output += character; continue; }
    let depth = 1, close = index + 1, nestedQuote = false, nestedEscaped = false;
    for (; close < source.length && depth; close++) {
      const nested = source[close];
      if (nestedQuote) { if (nestedEscaped) nestedEscaped = false; else if (nested === '\\') nestedEscaped = true; else if (nested === '"') nestedQuote = false; }
      else if (nested === '"') nestedQuote = true;
      else if (nested === '{') depth++;
      else if (nested === '}') depth--;
    }
    if (depth) { output += character; continue; }
    output += `{${canonicalMaterialBody(source.slice(index + 1, close - 1))}}`; index = close - 1;
  }
  return output;
}

function shaderDeclarationEntries(body) {
  const text = String(body || ''), structural = structuralMask(text), entries = [], re = /\b(?:def|over|class)\s+Shader\s+"([A-Za-z_][\w]*)"\s*\{/g;
  let match;
  while ((match = re.exec(text))) {
    if (structural[match.index] === ' ') continue;
    const open = text.indexOf('{', match.index + match[0].length - 1); let depth = 1, quote = false, escaped = false, comment = false, close = open + 1;
    for (; close < text.length && depth; close++) {
      const character = text[close];
      if (comment) { if (character === '\n' || character === '\r') comment = false; continue; }
      if (quote) { if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
      if (character === '#') comment = true; else if (character === '"') quote = true; else if (character === '{') depth++; else if (character === '}') depth--;
    }
    if (!open || depth) continue;
    entries.push({ name: match[1], start: match.index, open, close });
  }
  return entries;
}

function canonicalShaderOrder(body, names) {
  const entries = shaderDeclarationEntries(body), known = [...names].sort((a, b) => b.length - a.length), bodies = new Map(entries.map((entry) => [entry.name, stripUSDAComments(String(body).slice(entry.open + 1, entry.close - 1))]));
  const replaceReferences = (text, labels, fallback = '<local>') => {
    const masked = maskUSDAStringsAndComments(text), replacements = [];
    for (const name of known) {
      const expression = new RegExp(`(<|\\.|/)${escapeRegExp(name)}(?=[.:/>\\s])`, 'g');
      let match;
      while ((match = expression.exec(masked))) replacements.push({ start: match.index, end: match.index + match[0].length, value: `${match[1]}${labels.get(name) || fallback}` });
    }
    let output = text;
    for (const replacement of replacements.sort((a, b) => b.start - a.start)) output = output.slice(0, replacement.start) + replacement.value + output.slice(replacement.end);
    return output;
  };
  let labels = new Map([...bodies.keys()].map((name) => [name, '<local>'])), signatures = new Map();
  for (let pass = 0; pass < Math.min(16, entries.length + 1); pass++) {
    signatures = new Map([...bodies.entries()].map(([name, shaderBody]) => [name, canonicalMaterialBody(replaceReferences(shaderBody, labels))]));
    labels = new Map([...signatures.entries()].sort((a, b) => `${a[1]}\0${a[0]}`.localeCompare(`${b[1]}\0${b[0]}`)).map(([name], index) => [name, `ref${index}`]));
  }
  return entries.map((entry) => ({ name: entry.name, signature: canonicalMaterialBody(replaceReferences(bodies.get(entry.name), labels)).replace(/\s+/g, ' ').trim() })).sort((a, b) => `${a.signature}\0${a.name}`.localeCompare(`${b.signature}\0${b.name}`)).map((entry) => entry.name);
}

function canonicalMaterialBody(body) {
  const statements = [], text = canonicalizeNestedBlocks(String(body || '')); let start = 0, square = 0, round = 0, curly = 0, quote = false, escaped = false;
  const push = (end) => { const statement = text.slice(start, end).replace(/\s+/g, ' ').trim(); if (statement) statements.push(statement); start = end + 1; };
  for (let index = 0; index < text.length; index++) {
    const character = text[index];
    if (quote) { if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '"') { quote = true; continue; }
    if (character === '[') square++; else if (character === ']') square = Math.max(0, square - 1); else if (character === '(') round++; else if (character === ')') round = Math.max(0, round - 1); else if (character === '{') curly++; else if (character === '}') curly = Math.max(0, curly - 1); else if ((character === '\n' || character === ';') && square === 0 && round === 0 && curly === 0) push(index);
  }
  const tail = text.slice(start).replace(/\s+/g, ' ').trim(); if (tail) statements.push(tail);
  return statements.sort().join('\n');
}

function stripUSDAComments(body) {
  const text = String(body || ''); let output = '', quote = false, escaped = false, comment = false;
  for (const character of text) {
    if (comment) { if (character === '\n' || character === '\r') { comment = false; output += character; } continue; }
    if (quote) { output += character; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '"') { quote = true; output += character; } else if (character === '#') comment = true; else output += character;
  }
  return output;
}

function canonicalizeAssetPaths(body) {
  const normalize = (value) => {
    const path = String(value).trim().replaceAll('\\', '/');
    if (!path || path.startsWith('/') || /^[A-Za-z][A-Za-z0-9+.-]*:/.test(path)) return path;
    const parts = [];
    for (const part of path.split('/')) {
      if (!part || part === '.') continue;
      if (part === '..' && parts.length && parts.at(-1) !== '..') parts.pop();
      else parts.push(part);
    }
    return parts.join('/');
  };
  const source = String(body || ''); let output = '', quote = false, escaped = false;
  for (let index = 0; index < source.length;) {
    const character = source[index];
    if (quote) { output += character; index++; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '"') { quote = true; output += character; index++; continue; }
    if (character === '@') {
      const end = source.indexOf('@', index + 1);
      if (end >= 0) { output += `@${normalize(source.slice(index + 1, end))}@`; index = end + 1; continue; }
    }
    output += character; index++;
  }
  return output;
}

function maskUSDAStringsAndComments(body) {
  const text = String(body || ''), masked = [...text]; let quote = false, escaped = false, comment = false;
  for (let index = 0; index < text.length; index++) {
    const character = text[index];
    if (comment) { if (character === '\n' || character === '\r') comment = false; else masked[index] = ' '; continue; }
    if (quote) { masked[index] = ' '; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '#') { comment = true; masked[index] = ' '; continue; }
    if (character === '"') { quote = true; masked[index] = ' '; }
  }
  return masked.join('');
}

function canonicalizeLocalShaderNames(body, rootPath = null) {
  const text = String(body || ''), names = [...new Set([...text.matchAll(/\b(?:def|over|class)\s+Shader\s+"([A-Za-z_][\w]*)"/g)].map((match) => match[1]))], orderedNames = canonicalShaderOrder(text, names);
  if (!names.length) return text;
  const replacements = new Map(orderedNames.map((name, index) => [name, `__shader${index}`]));
  let output = text;
  for (const [name, canonical] of replacements) output = output.replace(new RegExp(`(\\b(?:def|over|class)\\s+Shader\\s+")${escapeRegExp(name)}(")`, 'g'), `$1${canonical}$2`);
  let result = '', quote = false, escaped = false;
  for (let index = 0; index < output.length;) {
    const character = output[index];
    if (quote) { result += character; index++; if (escaped) escaped = false; else if (character === '\\') escaped = true; else if (character === '"') quote = false; continue; }
    if (character === '"') { quote = true; result += character; index++; continue; }
    let replaced = false;
    for (const [name, canonical] of replacements) {
      const previous = output[index - 1], next = output[index + name.length];
      if (output.startsWith(name, index) && ['<', '.', '/'].includes(previous) && (next == null || /[.:/>\s]/.test(next))) { result += canonical; index += name.length; replaced = true; break; }
    }
    if (!replaced) { result += character; index++; }
  }
  result = result.replace(/<\.\/(__shader\d+)(?=[.>])/g, '<$1');
  if (validPrimPath(rootPath)) for (const canonical of replacements.values()) result = result.replace(new RegExp(`<${escapeRegExp(rootPath)}/${escapeRegExp(canonical)}(?=[.>])`, 'g'), `<${canonical}`);
  return result;
}

export function findEquivalentMaterialMapping(source) {
  const declarations = scanPrimDeclarations(source), materials = declarations.filter((item) => /\bMaterial\s+"/.test(item.declaration)).map((item) => {
    const parents = declarations.filter((candidate) => candidate.start < item.start && candidate.close >= item.close).sort((a, b) => a.start - b.start);
    const path = `/${[...parents.map((parent) => parent.name), item.name].join('/')}`;
    const originalBody = String(source).slice(item.open + 1, item.close - 1);
    let bodySource = originalBody;
    try {
      const folded = foldLiteralUSDShaderNodes(source, path), cleaned = removeUnreachableMaterialShaders(folded.source, path), cleanedBlock = findPrimBlock(cleaned.source, path);
      if (cleanedBlock) bodySource = cleaned.source.slice(cleanedBlock.open + 1, cleanedBlock.close - 1);
    } catch { /* Preserve the structural fingerprint when optional cleanup cannot safely analyze this material. */ }
    const body = canonicalMaterialBody(canonicalizeAssetPaths(canonicalizeLocalShaderNames(stripUSDAComments(bodySource), path)));
    return { path, body };
  });
  const groups = new Map();
  for (const material of materials) { const entries = groups.get(material.body) || []; entries.push(material); groups.set(material.body, entries); }
  const mapping = {};
  for (const entries of groups.values()) if (entries.length > 1) { entries.sort((a, b) => a.path.localeCompare(b.path)); for (const duplicate of entries.slice(1)) mapping[duplicate.path] = entries[0].path; }
  return mapping;
}

export function rewriteMaterialBindings(source, mapping = {}) {
  const raw = Object.entries(mapping && typeof mapping === 'object' && !Array.isArray(mapping) ? mapping : {}).filter(([from, to]) => from !== to);
  if (raw.some(([from, to]) => !validPrimPath(from) || !validPrimPath(to))) throw new LuciaError('LUCIA_MATERIAL_BINDING_PATH', 'Material binding rewrites require absolute USD prim paths.');
  const direct = new Map(raw), resolving = new Set(), resolved = new Map();
  const resolve = (path) => {
    if (!direct.has(path)) return path;
    if (resolved.has(path)) return resolved.get(path);
    if (resolving.has(path)) throw new LuciaError('LUCIA_MATERIAL_BINDING_CYCLE', 'Material binding rewrites must not contain cycles.');
    resolving.add(path); const target = resolve(direct.get(path)); resolving.delete(path); resolved.set(path, target); return target;
  };
  const entries = raw.map(([from]) => [from, resolve(from)]).filter(([from, to]) => from !== to);
  let changed = 0, text = String(source || '');
  if (entries.length) {
    const lookup = new Map(entries), expression = new RegExp(`(\\bmaterial:binding\\s*=\\s*)<(${entries.map(([from]) => escapeRegExp(from)).join('|')})>`, 'g'), masked = maskUSDAStringsAndComments(text), replacements = [];
    let match;
    while ((match = expression.exec(masked))) replacements.push({ start: match.index, end: match.index + match[0].length, value: `${match[1]}<${lookup.get(match[2])}>` });
    for (const replacement of replacements.reverse()) { text = text.slice(0, replacement.start) + replacement.value + text.slice(replacement.end); changed++; }
  }
  return { source: text, changed, mappings: entries.map(([from, to]) => ({ from, to })) };
}

// Collection bindings target collection prims, not Material prims. Keep this
// operation separate so a reviewed collection map cannot rewrite ordinary
// material bindings.
export function rewriteMaterialCollectionBindings(source, mapping = {}) {
  const raw = Object.entries(mapping && typeof mapping === 'object' && !Array.isArray(mapping) ? mapping : {}).filter(([from, to]) => from !== to);
  if (raw.some(([from, to]) => !validPrimPath(from) || !validPrimPath(to))) throw new LuciaError('LUCIA_COLLECTION_BINDING_PATH', 'Collection binding rewrites require absolute USD prim paths.');
  const direct = new Map(raw), resolving = new Set(), resolved = new Map();
  const resolve = (path) => {
    if (!direct.has(path)) return path;
    if (resolved.has(path)) return resolved.get(path);
    if (resolving.has(path)) throw new LuciaError('LUCIA_COLLECTION_BINDING_CYCLE', 'Collection binding rewrites must not contain cycles.');
    resolving.add(path); const target = resolve(direct.get(path)); resolving.delete(path); resolved.set(path, target); return target;
  };
  const entries = raw.map(([from]) => [from, resolve(from)]).filter(([from, to]) => from !== to);
  let changed = 0, text = String(source || '');
  if (entries.length) {
    const lookup = new Map(entries), expression = new RegExp(`(\\bmaterial:binding:collection(?::[A-Za-z_][\\w]*)?\\s*=\\s*)<(${entries.map(([from]) => escapeRegExp(from)).join('|')})>`, 'g'), masked = maskUSDAStringsAndComments(text), replacements = [];
    let match;
    while ((match = expression.exec(masked))) replacements.push({ start: match.index, end: match.index + match[0].length, value: `${match[1]}<${lookup.get(match[2])}>` });
    for (const replacement of replacements.reverse()) { text = text.slice(0, replacement.start) + replacement.value + text.slice(replacement.end); changed++; }
  }
  return { source: text, changed, mappings: entries.map(([from, to]) => ({ from, to })) };
}

// Merge only explicitly approved material paths. Definitions are removed only
// when they are Material prims and no non-binding reference remains after the
// exact binding rewrite; inherited and collection bindings are left untouched.
export function mergeMaterialDefinitions(source, mapping = {}) {
  const rewritten = rewriteMaterialBindings(source, mapping), removals = [];
  for (const { from, to } of rewritten.mappings) {
    const sourceBlock = findPrimBlock(rewritten.source, from), targetBlock = findPrimBlock(rewritten.source, to);
    if (!sourceBlock || !targetBlock) throw new LuciaError('LUCIA_MATERIAL_DEFINITION', `Material merge requires defined source and target prims: ${from} → ${to}`);
    const declaration = rewritten.source.slice(sourceBlock.start, sourceBlock.open + 1);
    if (!/\bdef\s+Material\s+"/.test(declaration)) throw new LuciaError('LUCIA_MATERIAL_DEFINITION', `Material merge source is not a def Material prim: ${from}`);
    const residual = new RegExp(`<${escapeRegExp(from)}>`, 'g');
    if (residual.test(structuralMask(rewritten.source))) throw new LuciaError('LUCIA_MATERIAL_REFERENCES', `Material ${from} has references other than exact material bindings; definition was retained.`);
    removals.push({ start: sourceBlock.start, end: sourceBlock.close });
  }
  let text = rewritten.source;
  for (const removal of removals.sort((a, b) => b.start - a.start)) {
    const lineStart = text.lastIndexOf('\n', removal.start - 1) + 1;
    const nextNewline = text.indexOf('\n', removal.end), lineEnd = nextNewline < 0 ? text.length : nextNewline;
    const occupiesLine = !text.slice(lineStart, removal.start).trim() && !text.slice(removal.end, lineEnd).trim();
    const start = occupiesLine ? lineStart : removal.start, end = occupiesLine ? (nextNewline < 0 ? lineEnd : nextNewline + 1) : removal.end;
    text = text.slice(0, start) + text.slice(end);
  }
  return { source: text, changed: rewritten.changed, removed: removals.length, mappings: rewritten.mappings };
}

// Remove only nested Shader prims that are unreachable from a Material's
// surface output.  This is deliberately narrower than a general USDA graph
// optimizer: literals, connections, and authored values are never rewritten,
// and a shader referenced from outside the selected Material is retained.
export function removeUnreachableMaterialShaders(source, materialPath) {
  if (!validPrimPath(materialPath)) throw new LuciaError('LUCIA_MATERIAL_GRAPH_PATH', 'Material graph cleanup requires an absolute USD prim path.');
  const text = String(source || ''), material = findPrimBlock(text, materialPath);
  if (!material) throw new LuciaError('LUCIA_MATERIAL_GRAPH_PATH', `Material prim not found: ${materialPath}`);
  const declarations = scanPrimDeclarations(text), shaders = declarations.filter((item) => /\bShader\s+"/.test(item.declaration) && item.start > material.open && item.close < material.close);
  if (!shaders.length) return { source: text, changed: false, removed: [], retained: [] };
  const descendants = (item) => declarations.filter((candidate) => candidate.start > material.open && candidate.start < item.start && candidate.close >= item.close).sort((a, b) => a.start - b.start).map((candidate) => candidate.name);
  const shaderPaths = new Map(shaders.map((shader) => [shader, `/${[...materialPath.split('/').filter(Boolean), ...descendants(shader), shader.name].join('/')}`]));
  const byPath = new Map([...shaderPaths].map(([shader, path]) => [path, shader]));
  const resolve = (reference) => {
    const value = String(reference || '').trim().replace(/^\.?\//, ''), prim = value.split('.')[0];
    if (!prim) return null;
    const candidates = prim.startsWith('/') ? [prim] : [`${materialPath}/${prim}`, `/${prim}`];
    return candidates.map((candidate) => byPath.get(candidate)).find(Boolean) || null;
  };
  const blankNested = (body) => { let output = body; for (const shader of shaders) { const start = Math.max(0, shader.start - (material.open + 1)), end = Math.min(output.length, shader.close - (material.open + 1)); if (start < end) output = output.slice(0, start) + ' '.repeat(end - start) + output.slice(end); } return output; };
  const materialBody = text.slice(material.open + 1, material.close - 1), roots = [];
  for (const match of structuralMask(blankNested(materialBody)).matchAll(/\boutputs:[A-Za-z_][\w:]*\.connect\s*=\s*<([^>]+)>/g)) { const shader = resolve(match[1]); if (shader) roots.push(shader); }
  if (!roots.length) return { source: text, changed: false, removed: [], retained: shaders.map((shader) => shaderPaths.get(shader)).sort() };
  const reachable = new Set(), visit = (shader) => { if (!shader || reachable.has(shader)) return; reachable.add(shader); const body = structuralMask(text.slice(shader.open + 1, shader.close - 1)); for (const match of body.matchAll(/<([^>]+)>/g)) visit(resolve(match[1])); };
  roots.forEach(visit);
  const outside = text.slice(0, material.start) + text.slice(material.close);
  const outsideStructural = structuralMask(outside);
  // Keep both direct prim references (`<.../Shader>`) and property
  // references (`<.../Shader.outputs:result>`). A relationship may consume
  // the Shader prim without naming one of its properties.
  const externallyReferenced = (path) => new RegExp(`<${escapeRegExp(path)}(?:[.>])`).test(outsideStructural);
  const removable = shaders.filter((shader) => !reachable.has(shader) && !externallyReferenced(shaderPaths.get(shader)));
  const topLevel = removable.filter((shader) => !removable.some((parent) => parent !== shader && parent.start < shader.start && parent.close >= shader.close));
  let output = text;
  for (const shader of [...topLevel].sort((a, b) => b.start - a.start)) {
    const lineStart = output.lastIndexOf('\n', shader.start - 1) + 1, nextNewline = output.indexOf('\n', shader.close), lineEnd = nextNewline < 0 ? output.length : nextNewline;
    const wholeLine = !output.slice(lineStart, shader.start).trim() && !output.slice(shader.close, lineEnd).trim(), start = wholeLine ? lineStart : shader.start, end = wholeLine ? (nextNewline < 0 ? lineEnd : nextNewline + 1) : shader.close;
    output = output.slice(0, start) + output.slice(end);
  }
  return { source: output, changed: topLevel.length > 0, removed: topLevel.map((shader) => shaderPaths.get(shader)).sort(), retained: shaders.filter((shader) => !topLevel.includes(shader)).map((shader) => shaderPaths.get(shader)).sort() };
}

// Fold only the small, unambiguous arithmetic subset commonly emitted by USDA
// material graphs. Finite color/vector triples use same-shape or scalar
// broadcast rules. Connections and unknown shader IDs are retained; this
// helper never evaluates arbitrary shader code or deletes external references.
export function foldLiteralUSDShaderNodes(source, materialPath) {
  if (!validPrimPath(materialPath)) throw new LuciaError('LUCIA_MATERIAL_GRAPH_PATH', 'Material graph folding requires an absolute USD prim path.');
  const number = '[-+]?\\d*\\.?\\d+(?:[eE][-+]?\\d+)?', tuple3 = `\\(\\s*${number}\\s*,\\s*${number}\\s*,\\s*${number}\\s*\\)`, tuple4 = `\\(\\s*${number}\\s*,\\s*${number}\\s*,\\s*${number}\\s*,\\s*${number}\\s*\\)`, valueToken = `(?:${number}|${tuple3}|${tuple4}|true|false)`, valueTypes = '(?:bool|float|color3f|float3|vector3f|color4f|float4|int|integer)';
  const isValue = (value) => typeof value === 'boolean' || Number.isFinite(value) || (Array.isArray(value) && (value.length === 3 || value.length === 4) && value.every(Number.isFinite));
  const componentwise = (a, b, fn) => { const av = Array.isArray(a) ? a : null, bv = Array.isArray(b) ? b : null; if (av && bv && av.length !== bv.length) return undefined; const size = av?.length || bv?.length || 1, aa = av || Array(size).fill(a), bb = bv || Array(size).fill(b), result = aa.map((item, index) => fn(item, bb[index])); return av || bv ? result : result[0]; };
  const unary = (value, fn) => Array.isArray(value) ? value.map(fn) : fn(value);
  const text = String(source || ''), material = findPrimBlock(text, materialPath);
  if (!material) throw new LuciaError('LUCIA_MATERIAL_GRAPH_PATH', `Material prim not found: ${materialPath}`);
  const declarations = scanPrimDeclarations(text), shaders = declarations.filter((item) => /\bShader\s+"/.test(item.declaration) && item.start > material.open && item.close < material.close);
  const descendants = (item) => declarations.filter((candidate) => candidate.start > material.open && candidate.start < item.start && candidate.close >= item.close).sort((a, b) => a.start - b.start).map((candidate) => candidate.name);
  const shaderPaths = new Map(shaders.map((shader) => [shader, `/${[...materialPath.split('/').filter(Boolean), ...descendants(shader), shader.name].join('/')}`]));
  const operations = { UsdAdd: { inputs: ['a', 'b'], evaluate: (a, b) => componentwise(a, b, (x, y) => x + y) }, UsdSubtract: { inputs: ['a', 'b'], evaluate: (a, b) => componentwise(a, b, (x, y) => x - y) }, UsdMultiply: { inputs: ['a', 'b'], evaluate: (a, b) => componentwise(a, b, (x, y) => x * y) }, UsdDivide: { inputs: ['a', 'b'], evaluate: (a, b) => componentwise(a, b, (x, y) => y === 0 ? undefined : x / y) }, UsdMin: { inputs: ['a', 'b'], evaluate: (a, b) => componentwise(a, b, Math.min) }, UsdMax: { inputs: ['a', 'b'], evaluate: (a, b) => componentwise(a, b, Math.max) }, UsdPower: { inputs: ['a', 'b'], evaluate: (a, b) => componentwise(a, b, (x, y) => x ** y) }, UsdDot: { inputs: ['a', 'b'], evaluate: (a, b) => Array.isArray(a) && Array.isArray(b) && a.length === b.length ? a.reduce((sum, value, index) => sum + value * b[index], 0) : undefined }, UsdLength: { inputs: ['value'], evaluate: (value) => Array.isArray(value) ? Math.hypot(...value) : undefined }, UsdNormalize: { inputs: ['value'], evaluate: (value) => { if (!Array.isArray(value)) return undefined; const length = Math.hypot(...value); return length > 0 ? value.map((item) => item / length) : undefined; } }, UsdCross: { inputs: ['a', 'b'], evaluate: (a, b) => Array.isArray(a) && Array.isArray(b) && a.length === 3 && b.length === 3 ? [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]] : undefined }, UsdAbs: { inputs: ['value'], evaluate: (value) => unary(value, Math.abs) }, UsdNegate: { inputs: ['value'], evaluate: (value) => unary(value, (x) => -x) }, UsdSqrt: { inputs: ['value'], evaluate: (value) => unary(value, (x) => x < 0 ? undefined : Math.sqrt(x)) }, UsdFloor: { inputs: ['value'], evaluate: (value) => unary(value, Math.floor) }, UsdCeil: { inputs: ['value'], evaluate: (value) => unary(value, Math.ceil) }, UsdRound: { inputs: ['value'], evaluate: (value) => unary(value, Math.round) }, UsdFract: { inputs: ['value'], evaluate: (value) => unary(value, (x) => x - Math.floor(x)) }, UsdSign: { inputs: ['value'], evaluate: (value) => unary(value, Math.sign) }, UsdExp: { inputs: ['value'], evaluate: (value) => unary(value, Math.exp) }, UsdLog: { inputs: ['value'], evaluate: (value) => unary(value, (x) => x > 0 ? Math.log(x) : undefined) }, UsdReciprocal: { inputs: ['value'], evaluate: (value) => unary(value, (x) => x === 0 ? undefined : 1 / x) }, UsdInverse: { inputs: ['value'], evaluate: (value) => unary(value, (x) => x === 0 ? undefined : 1 / x) }, UsdSin: { inputs: ['value'], evaluate: (value) => unary(value, Math.sin) }, UsdCos: { inputs: ['value'], evaluate: (value) => unary(value, Math.cos) }, UsdTan: { inputs: ['value'], evaluate: (value) => unary(value, Math.tan) }, UsdOneMinus: { inputs: ['value'], evaluate: (value) => unary(value, (x) => 1 - x) }, UsdSaturate: { inputs: ['value'], evaluate: (value) => unary(value, (x) => Math.max(0, Math.min(1, x))) }, UsdClamp: { inputs: ['value', 'min', 'max'], evaluate: (value, min, max) => componentwise(componentwise(value, min, Math.max), max, Math.min) }, UsdLerp: { inputs: ['a', 'b', 't'], evaluate: (a, b, t) => Array.isArray(t) ? undefined : componentwise(a, b, (x, y) => x * (1 - t) + y * t) }, UsdSmoothStep: { inputs: ['edge0', 'edge1', 'x'], evaluate: (edge0, edge1, x) => Array.isArray(edge0) || Array.isArray(edge1) || Array.isArray(x) ? undefined : edge0 === edge1 ? undefined : (() => { const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0))); return t * t * (3 - 2 * t); })() }, UsdSelect: { inputs: ['condition', 'ifTrue', 'ifFalse'], evaluate: (condition, ifTrue, ifFalse) => typeof condition === 'boolean' || (typeof condition === 'number' && Number.isFinite(condition)) ? condition !== 0 ? ifTrue : ifFalse : undefined } };
  const authoredOperation = (id) => { if (operations[id]) return { operation: operations[id], inputs: operations[id].inputs }; const constant = /^ND_constant_(float|color3|vector3|color4|vector4)$/.exec(id); if (constant) return { operation: { inputs: ['value'], evaluate: (value) => value }, inputs: ['value'] }; const remap = /^(?:ND_)?(?:remap|range)_[A-Za-z0-9]+$/.exec(id); if (remap) return { operation: { inputs: ['in', 'inlow', 'inhigh', 'outlow', 'outhigh'], evaluate: (value, inlow, inhigh, outlow, outhigh) => { const arrays = [value, inlow, inhigh, outlow, outhigh].filter(Array.isArray), lengths = [...new Set(arrays.map((item) => item.length))]; if (lengths.length > 1) return undefined; const size = lengths[0] || 1, at = (item, index) => Array.isArray(item) ? item[index] : item, result = Array.from({ length: size }, (_, index) => { const low = at(inlow, index), high = at(inhigh, index), denominator = high - low; return at(outlow, index) + (Math.abs(denominator) > 1e-12 ? (at(value, index) - low) / denominator : 0) * (at(outhigh, index) - at(outlow, index)); }); return size > 1 ? result : result[0]; } }, inputs: ['in', 'inlow', 'inhigh', 'outlow', 'outhigh'] }; const mix = /^ND_mix_[A-Za-z0-9]+$/.exec(id); if (mix) return { operation: { inputs: ['bg', 'fg', 'mix'], evaluate: (background, foreground, factor) => Number.isFinite(factor) && !Array.isArray(factor) ? componentwise(background, foreground, (from, to) => from * (1 - factor) + to * factor) : undefined }, inputs: ['bg', 'fg', 'mix'] }; const saturateColor = /^ND_saturate_(?:color3|color3f)$/.exec(id); if (saturateColor) return { operation: { inputs: ['in', 'amount'], evaluate: (color, amount) => { if (!Array.isArray(color) || color.length !== 3 || Array.isArray(amount) || !Number.isFinite(amount)) return undefined; const luminance = 0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2]; return color.map((component) => luminance + (component - luminance) * amount); } }, inputs: ['in', 'amount'] }; const hsvAdjustSeparate = /^ND_hsv_adjust_color3$/.exec(id); if (hsvAdjustSeparate) return { operation: { inputs: ['in', 'hue', 'saturation', 'value', 'fac'], evaluate: (color, hueShift, saturationScale, valueScale, fac) => { if (!Array.isArray(color) || color.length !== 3 || [hueShift, saturationScale, valueScale, fac].some((value) => !Number.isFinite(value))) return undefined; const maximum = Math.max(...color), minimum = Math.min(...color), delta = maximum - minimum; let hue = 0; if (delta > 0) { hue = maximum === color[0] ? ((color[1] - color[2]) / delta) % 6 : maximum === color[1] ? (color[2] - color[0]) / delta + 2 : (color[0] - color[1]) / delta + 4; hue /= 6; if (hue < 0) hue += 1; } const saturation = maximum === 0 ? 0 : delta / maximum, adjustedHue = ((hue + hueShift) % 1 + 1) % 1, adjustedSaturation = saturation * saturationScale, adjustedValue = maximum * valueScale, sector = adjustedHue * 6, sectorIndex = Math.floor(sector), fraction = sector - sectorIndex, p = adjustedValue * (1 - adjustedSaturation), q = adjustedValue * (1 - adjustedSaturation * fraction), t = adjustedValue * (1 - adjustedSaturation * (1 - fraction)); const adjusted = sectorIndex % 6 === 0 ? [adjustedValue, t, p] : sectorIndex === 1 ? [q, adjustedValue, p] : sectorIndex === 2 ? [p, adjustedValue, t] : sectorIndex === 3 ? [p, q, adjustedValue] : sectorIndex === 4 ? [t, p, adjustedValue] : [adjustedValue, p, q]; return color.map((component, index) => component * (1 - fac) + adjusted[index] * fac); } }, inputs: ['in', 'hue', 'saturation', 'value', 'fac'] }; const hsvAdjust = /^ND_hsvadjust_color3$/.exec(id); if (hsvAdjust) return { operation: { inputs: ['in', 'amount'], evaluate: (value, amount) => { if (!Array.isArray(value) || value.length !== 3 || !Array.isArray(amount) || amount.length !== 3) return undefined; const max = Math.max(...value), min = Math.min(...value), delta = max - min; let hue = 0; if (delta > 0) { hue = max === value[0] ? ((value[1] - value[2]) / delta) % 6 : max === value[1] ? (value[2] - value[0]) / delta + 2 : (value[0] - value[1]) / delta + 4; hue /= 6; if (hue < 0) hue += 1; } const saturation = max === 0 ? 0 : delta / max, adjustedHue = ((hue + amount[0]) % 1 + 1) % 1, adjustedSaturation = saturation * amount[1], adjustedValue = max * amount[2], sector = adjustedHue * 6, index = Math.floor(sector), fraction = sector - index, p = adjustedValue * (1 - adjustedSaturation), q = adjustedValue * (1 - adjustedSaturation * fraction), t = adjustedValue * (1 - adjustedSaturation * (1 - fraction)); return index % 6 === 0 ? [adjustedValue, t, p] : index === 1 ? [q, adjustedValue, p] : index === 2 ? [p, adjustedValue, t] : index === 3 ? [p, q, adjustedValue] : index === 4 ? [t, p, adjustedValue] : [adjustedValue, p, q]; } }, inputs: ['in', 'amount'] }; const luminance = /^ND_luminance_[A-Za-z0-9]+$/.exec(id); if (luminance) return { operation: { inputs: ['in'], evaluate: (value) => Array.isArray(value) && value.length === 3 ? 0.2126 * value[0] + 0.7152 * value[1] + 0.0722 * value[2] : undefined }, inputs: ['in'] }; const separate = /^ND_separate([234])_[A-Za-z0-9]+$/.exec(id); if (separate) return { operation: { inputs: ['in'], evaluate: (value) => Array.isArray(value) && value.length === Number(separate[1]) ? value : undefined }, inputs: ['in'] }; const swizzle = /^ND_swizzle_[A-Za-z0-9]+$/.exec(id); if (swizzle) return { operation: { inputs: ['in', 'channels'], evaluate: (value, channels) => { if (!Array.isArray(value) || !channels || channels.length < 1 || channels.length > 4) return undefined; const indices = { r: 0, g: 1, b: 2, a: 3, x: 0, y: 1, z: 2, w: 3 }; const result = [...channels].map((channel) => indices[channel]); return result.every((index) => index != null && index < value.length) ? result.map((index) => value[index]) : undefined; } }, inputs: ['in', 'channels'], stringInputs: ['channels'] }; const combine = /^ND_combine([234])_[A-Za-z0-9]+$/.exec(id); if (combine) { const count = Number(combine[1]); return { operation: { inputs: Array.from({ length: count }, (_, index) => 'in' + (index + 1)), evaluate: (...values) => values.every((value) => !Array.isArray(value) && Number.isFinite(value)) ? values : undefined }, inputs: Array.from({ length: count }, (_, index) => 'in' + (index + 1)) }; } const extraction = /^ND_extract_[A-Za-z0-9]+$/.exec(id); if (extraction) return { operation: { inputs: ['in', 'index'], evaluate: (value, index) => Array.isArray(value) && Number.isInteger(index) && index >= 0 && index < value.length ? value[index] : undefined }, inputs: ['in', 'index'] }; const conditional = /^(?:ND_)?if(greater|greatereq|less|lesseq|equal)_[A-Za-z0-9]+$/.exec(id); if (conditional) return { operation: { inputs: [['value1', 'in1'], ['value2', 'in2'], ['in1', 'value1'], ['in2', 'value2']], evaluate: (a, b, yes, no) => { if (Array.isArray(a) || Array.isArray(b)) return undefined; const selected = conditional[1] === 'greater' ? a > b : conditional[1] === 'greatereq' ? a >= b : conditional[1] === 'less' ? a < b : conditional[1] === 'lesseq' ? a <= b : a === b; return selected ? yes : no; } }, inputs: [['value1', 'in1'], ['value2', 'in2'], ['in1', 'value1'], ['in2', 'value2']] }; const conversion = /^ND_convert_(float|color3|vector3)_(float|color3|vector3)$/.exec(id); if (conversion) return { operation: { inputs: ['in'], evaluate: (value) => { const fromVector = conversion[1] !== 'float', toVector = conversion[2] !== 'float'; if (fromVector && (!Array.isArray(value) || value.length !== 3)) return undefined; if (!fromVector && Array.isArray(value)) return undefined; if (toVector) return Array.isArray(value) ? value : [value, value, value]; return Array.isArray(value) ? value[0] : value; } }, inputs: ['in'] }; const match = /^ND_(add|subtract|multiply|divide|min|max|power|dotproduct|crossproduct|absval|sqrt|floor|ceil|round|fract|sign|exp|log|normalize|magnitude|clamp|saturate|reciprocal|inverse|sin|cos|tan|oneminus|one_minus|invert|negate|negative)_[A-Za-z0-9]+$/.exec(id); if (!match) return null; const key = { add: 'UsdAdd', subtract: 'UsdSubtract', multiply: 'UsdMultiply', divide: 'UsdDivide', min: 'UsdMin', max: 'UsdMax', power: 'UsdPower', dotproduct: 'UsdDot', crossproduct: 'UsdCross', absval: 'UsdAbs', sqrt: 'UsdSqrt', floor: 'UsdFloor', ceil: 'UsdCeil', round: 'UsdRound', fract: 'UsdFract', sign: 'UsdSign', exp: 'UsdExp', log: 'UsdLog', reciprocal: 'UsdReciprocal', inverse: 'UsdReciprocal', sin: 'UsdSin', cos: 'UsdCos', tan: 'UsdTan', oneminus: 'UsdOneMinus', one_minus: 'UsdOneMinus', invert: 'UsdOneMinus', negate: 'UsdNegate', negative: 'UsdNegate', normalize: 'UsdNormalize', magnitude: 'UsdLength', clamp: 'UsdClamp', saturate: 'UsdSaturate' }[match[1]], operation = operations[key], inputs = key === 'UsdClamp' ? ['in', 'low', 'high'] : operation.inputs.map((name, index) => key === 'UsdNormalize' || key === 'UsdLength' || key === 'UsdAbs' || key === 'UsdSqrt' || key === 'UsdFloor' || key === 'UsdCeil' || key === 'UsdOneMinus' || key === 'UsdNegate' || key === 'UsdSaturate' || key === 'UsdReciprocal' || key === 'UsdSin' || key === 'UsdCos' || key === 'UsdTan' || key === 'UsdRound' || key === 'UsdFract' || key === 'UsdSign' || key === 'UsdExp' || key === 'UsdLog' ? 'in' : 'in' + (index + 1)); return { operation, inputs }; };
  const parseValue = (token) => token === 'true' ? true : token === 'false' ? false : token.startsWith('(') ? token.slice(1, -1).split(',').map((item) => Number(item.trim())) : Number(token);
  const inputTypePattern = new RegExp(`${valueTypes}\\s+inputs:`);
  const known = new Map(), literal = (body, name) => { const structural = structuralMask(body), match = body.match(new RegExp(`(?:^|[^A-Za-z0-9_:])(?:uniform\\s+)?${valueTypes}\\s+inputs:${name}\\s*=\\s*(${valueToken})`)); if (match && structural[match.index + match[0].search(inputTypePattern)] !== ' ') { const value = parseValue(match[1]); if (isValue(value)) return value; } const connection = body.match(new RegExp(`(?:^|[^A-Za-z0-9_:])(?:uniform\\s+)?${valueTypes}\\s+inputs:${name}\\s*=\\s*<([^>]+)>`)); if (!connection || structural[connection.index + connection[0].search(inputTypePattern)] === ' ') return null; const reference = connection[1].split('.outputs:')[0].replaceAll('./', '').replaceAll('//', '/'), path = reference.startsWith('/') ? reference : `${materialPath}/${reference}`; return known.get(path) ?? null; }, readLiteral = (body, names) => Array.isArray(names) ? names.map((name) => literal(body, name)).find((value) => value != null) ?? null : literal(body, names);
  const readString = (body, name) => { const structural = structuralMask(body), match = body.match(new RegExp('inputs:' + name + '\\s*=\\s*"([^"]*)"')); return match && structural[match.index] !== ' ' ? match[1] : null; };
  const candidates = [], foldedPaths = new Set();
  for (let pass = 0; pass < shaders.length; pass++) {
    let progress = false;
    for (const shader of shaders) {
      const path = shaderPaths.get(shader); if (foldedPaths.has(path)) continue;
      const body = text.slice(shader.open + 1, shader.close - 1), structural = structuralMask(body), idMatch = body.match(/(?:^|[^A-Za-z0-9_:])(?:uniform\s+)?token\s+info:id\s*=\s*"([^"]+)"/);
      const resolved = idMatch && structural[idMatch.index + idMatch[0].lastIndexOf('token')] !== ' ' ? authoredOperation(idMatch[1]) : null;
      if (!resolved) continue;
      const values = resolved.inputs.map((name) => resolved.stringInputs?.includes(name) ? readString(body, name) : readLiteral(body, name)), value = values.some((item) => item == null) || values.some((item) => typeof item === 'boolean') && idMatch[1] !== 'UsdSelect' ? undefined : resolved.operation.evaluate(...values);
      if (value !== undefined && isValue(value)) { candidates.push({ shader, path, value }); foldedPaths.add(path); known.set(path, value); progress = true; }
    }
    if (!progress) break;
  }
  if (!candidates.length) return { source: text, changed: false, folded: [], removed: [] };
  const structural = structuralMask(text), outside = text.slice(0, material.start) + text.slice(material.close), outsideStructural = structuralMask(outside), replacements = [], removals = [];
  for (const candidate of candidates) {
    if (new RegExp(`<${escapeRegExp(candidate.path)}(?:[.>])`).test(outsideStructural)) continue;
    // Keep the expression explicit to avoid matching text in USDA strings or comments.
    const reference = new RegExp(`<(${escapeRegExp(candidate.path)}|\\.?/${escapeRegExp(candidate.shader.name)}|${escapeRegExp(candidate.shader.name)})\\.outputs:(result|out|out[rgba]|out[xyzw])>`, 'g');
    const candidateReplacements = [];
    let match;
    while ((match = reference.exec(structural))) if (match.index >= material.start && match.index < material.close && !(match.index >= candidate.shader.start && match.index < candidate.shader.close)) { const channel = { r: 0, g: 1, b: 2, a: 3, x: 0, y: 1, z: 2, w: 3 }[match[2]?.at(-1)], value = Array.isArray(candidate.value) && channel != null ? candidate.value[channel] : candidate.value; if (value !== undefined) candidateReplacements.push({ start: match.index, end: match.index + match[0].length, value: Array.isArray(value) ? `(${value.join(', ')})` : String(value) }); }
    if (candidateReplacements.length) {
      replacements.push(...candidateReplacements);
      const reference = new RegExp(`<(${escapeRegExp(candidate.path)}|\\.?/${escapeRegExp(candidate.shader.name)}|${escapeRegExp(candidate.shader.name)})(?:[.>/][^>]*)?>`, 'g');
      let retained = false;
      while ((match = reference.exec(structural))) if (match.index >= material.start && match.index < material.close && !(match.index >= candidate.shader.start && match.index < candidate.shader.close) && !candidateReplacements.some((item) => item.start === match.index)) { retained = true; break; }
      if (!retained) removals.push({ start: candidate.shader.start, end: candidate.shader.close, path: candidate.path, value: candidate.value });
    }
  }
  if (!replacements.length) return { source: text, changed: false, folded: [], removed: [] };
  let output = text;
  for (const operation of [...replacements.map(({ start, end, value }) => ({ start, end, value })), ...removals.map(({ start, end }) => ({ start, end, value: '' }))].sort((a, b) => b.start - a.start)) output = output.slice(0, operation.start) + operation.value + output.slice(operation.end);
  const removed = removals.map(({ path }) => path).sort();
  return { source: output, changed: true, folded: candidates.map(({ path, value }) => ({ path, value })).sort((a, b) => a.path.localeCompare(b.path)), removed };
}
