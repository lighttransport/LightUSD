const vectorValue = (value) => Array.isArray(value) ? value : ArrayBuffer.isView(value) ? Array.from(value) : null;
const finiteValue = (value) => typeof value === 'number' ? Number.isFinite(value) : (() => { const vector = vectorValue(value); return Boolean(vector) && vector.length > 0 && vector.length <= 4 && vector.every(Number.isFinite); })();
const normalizedValue = (value) => { const vector = vectorValue(value); return vector ? [...vector] : value; };
const inputEntries = (candidate) => candidate instanceof Map ? [...candidate.entries()] : Object.entries(candidate);
export const graphInputs = (node) => { const candidate = node?.inputs !== undefined ? node.inputs : node?.parameters; return candidate && typeof candidate === 'object' && !Array.isArray(candidate) && !ArrayBuffer.isView(candidate) ? Object.fromEntries(inputEntries(candidate)) : {}; };
const detachedValue = (value, seen = new WeakSet()) => { if (!value || typeof value !== 'object') return value; if (seen.has(value)) throw new Error('Material graph values must be acyclic.'); seen.add(value); if (ArrayBuffer.isView(value)) { const copy = new value.constructor(value); seen.delete(value); return copy; } if (Array.isArray(value)) { const copy = value.map((child) => detachedValue(child, seen)); seen.delete(value); return copy; } const prototype = Object.getPrototypeOf(value); if (prototype !== Object.prototype && prototype !== null) { seen.delete(value); throw new Error('Material graph values must be plain objects, arrays, or typed arrays.'); } const copy = Object.fromEntries(Object.entries(value).map(([key, child]) => [key, detachedValue(child, seen)])); seen.delete(value); return copy; };
const typeOf = (node) => String(node?.type || node?.nodeType || node?.category || '').toLowerCase().replace(/[ _-]/g, '');
export const materialGraphRootIds = (nodes) => {
  const values = nodes instanceof Map ? [...nodes.entries()].map(([id, node]) => ({ ...(node || {}), id: String(node?.id ?? id) })) : Array.isArray(nodes) ? nodes.map((node, index) => ({ ...(node || {}), id: String(node?.id ?? node?.name ?? `node:${index}`) })) : nodes && typeof nodes === 'object' && !ArrayBuffer.isView(nodes) ? Object.entries(nodes).map(([id, node]) => ({ ...(node || {}), id: String(node?.id ?? id) })) : [];
  const outputRoots = values.filter((node) => /output/i.test(typeOf(node)) || /^output$/i.test(String(node?.id ?? '')));
  const shaderRoots = values.filter((node) => /^(?:standardsurface|usdpreviewsurface|principledbsdf|material)$/.test(typeOf(node)));
  const roots = outputRoots.length ? outputRoots : shaderRoots.length ? shaderRoots : values.filter((node) => /(?:surface|material)/i.test(typeOf(node)) || /(?:surface|material)/i.test(String(node?.id ?? '')));
  return [...new Set(roots.map((node) => String(node?.id ?? '')))].filter(Boolean);
};
export const graphEntries = (graph) => { const source = graph && typeof graph === 'object' && Object.hasOwn(graph, 'nodes') ? graph.nodes : graph; if (source instanceof Map) return [...source.entries()].map(([id, node]) => [String(node?.id ?? id), node]); if (Array.isArray(source)) return source.map((node, index) => [String(node?.id ?? node?.name ?? `node:${index}`), node]); if (!source || typeof source !== 'object') return []; return Object.entries(source); };
export const MATERIAL_GRAPH_BAKEABLE_TYPES = Object.freeze(['constant', 'float', 'color', 'vector', 'image', 'texture', 'multiply', 'add', 'subtract', 'divide', 'power', 'min', 'max', 'dot', 'length', 'normalize', 'cross', 'oneminus', 'negate', 'negative', 'abs', 'sqrt', 'floor', 'ceil', 'round', 'fract', 'sign', 'exp', 'log', 'reciprocal', 'inverse', 'sin', 'cos', 'tan', 'saturate', 'clamp', 'mix', 'lerp', 'smoothstep', 'select', 'if', 'ifgreater', 'ifgreatereq', 'ifless', 'iflesseq', 'ifequal', 'normalmap']);
export const MATERIAL_GRAPH_AUDITABLE_TYPES = Object.freeze(['output', 'surface', ...MATERIAL_GRAPH_BAKEABLE_TYPES, 'standard_surface', 'usdpreviewsurface', 'principledbsdf']);
const reference = (value) => value && typeof value === 'object' && (value.node ?? value.id ?? value.source) != null ? String(value.node ?? value.id ?? value.source) : null;
const literal = (value, values) => { const id = reference(value); if (id) return values.get(id); if (value && typeof value === 'object' && 'value' in value) return literal(value.value, values); return typeof value === 'boolean' || finiteValue(value) ? normalizedValue(value) : undefined; };
const operate = (left, right, fn) => { const leftVector = vectorValue(left), rightVector = vectorValue(right), leftSize = leftVector ? leftVector.length : 1, rightSize = rightVector ? rightVector.length : 1; if (leftVector && rightVector && leftSize !== rightSize && leftSize !== 1 && rightSize !== 1) return undefined; const size = Math.max(leftSize, rightSize), a = leftVector ? leftSize === 1 ? Array(size).fill(leftVector[0]) : leftVector : Array(size).fill(left), b = rightVector ? rightSize === 1 ? Array(size).fill(rightVector[0]) : rightVector : Array(size).fill(right), output = a.map((value, index) => fn(value, b[index])); return leftVector || rightVector ? output : output[0]; };
const operateUnary = (value, fn) => { const vector = vectorValue(value); return vector ? vector.map(fn) : fn(value); };
const operateTernary = (left, right, factor, fn) => {
  const vectors = [vectorValue(left), vectorValue(right), vectorValue(factor)], sizes = vectors.map((vector) => vector ? vector.length : 1), size = Math.max(...sizes);
  if (sizes.some((value) => value !== 1 && value !== size)) return undefined;
  const values = [left, right, factor].map((value, index) => vectors[index] ? (vectors[index].length === 1 ? Array(size).fill(vectors[index][0]) : vectors[index]) : Array(size).fill(value));
  const output = values[0].map((value, index) => fn(value, values[1][index], values[2][index]));
  return vectors.some(Boolean) ? output : output[0];
};
const pickInput = (inputs, keys, names, index) => { const name = names.find((candidate) => Object.hasOwn(inputs, candidate)); return name == null ? inputs[keys[index]] : inputs[name]; };
const operationResult = (kind, inputs, keys) => {
  if (kind === 'add' || kind === 'subtract' || kind === 'multiply' || kind === 'divide' || kind === 'power' || kind === 'min' || kind === 'max') {
    const left = pickInput(inputs, keys, ['a', 'left', 'input1'], 0), right = pickInput(inputs, keys, ['b', 'right', 'input2'], 1);
    if (keys.length < 2 || (kind === 'divide' && operate(left, right, (a, b) => b === 0 ? NaN : a) === undefined)) return undefined;
    return operate(left, right, (a, b) => kind === 'add' ? a + b : kind === 'subtract' ? a - b : kind === 'multiply' ? a * b : kind === 'divide' ? b === 0 ? NaN : a / b : kind === 'power' ? Math.pow(a, b) : kind === 'min' ? Math.min(a, b) : Math.max(a, b));
  }
  if (kind === 'dot' && keys.length >= 2) {
    const left = vectorValue(pickInput(inputs, keys, ['a', 'left', 'input1'], 0)), right = vectorValue(pickInput(inputs, keys, ['b', 'right', 'input2'], 1));
    if (!left || !right || left.length !== right.length || left.length < 2 || left.length > 4) return undefined;
    return left.reduce((sum, value, index) => sum + value * right[index], 0);
  }
  if (kind === 'length' && keys.length) {
    const value = vectorValue(pickInput(inputs, keys, ['value', 'input', 'x'], 0));
    if (!value || value.length < 2 || value.length > 4) return undefined;
    return Math.sqrt(value.reduce((sum, component) => sum + component * component, 0));
  }
  if (kind === 'normalize' && keys.length) {
    const value = vectorValue(pickInput(inputs, keys, ['value', 'input', 'x'], 0));
    if (!value || value.length < 2 || value.length > 4) return undefined;
    const magnitude = Math.sqrt(value.reduce((sum, component) => sum + component * component, 0));
    return magnitude > 0 ? value.map((component) => component / magnitude) : undefined;
  }
  if (kind === 'cross' && keys.length >= 2) {
    const left = vectorValue(pickInput(inputs, keys, ['a', 'left', 'input1'], 0)), right = vectorValue(pickInput(inputs, keys, ['b', 'right', 'input2'], 1));
    if (!left || !right || left.length !== 3 || right.length !== 3) return undefined;
    return [left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2], left[0] * right[1] - left[1] * right[0]];
  }
  if (kind === 'oneminus' || kind === 'negate' || kind === 'negative' || kind === 'abs' || kind === 'sqrt' || kind === 'floor' || kind === 'ceil' || kind === 'round' || kind === 'fract' || kind === 'sign' || kind === 'exp' || kind === 'log' || kind === 'reciprocal' || kind === 'inverse' || kind === 'sin' || kind === 'cos' || kind === 'tan' || kind === 'saturate') return keys.length ? operateUnary(pickInput(inputs, keys, ['value', 'input', 'x'], 0), (value) => kind === 'oneminus' ? 1 - value : kind === 'negate' || kind === 'negative' ? -value : kind === 'abs' ? Math.abs(value) : kind === 'sqrt' ? value < 0 ? NaN : Math.sqrt(value) : kind === 'floor' ? Math.floor(value) : kind === 'ceil' ? Math.ceil(value) : kind === 'round' ? Math.round(value) : kind === 'fract' ? value - Math.floor(value) : kind === 'sign' ? Math.sign(value) : kind === 'exp' ? Math.exp(value) : kind === 'log' ? value > 0 ? Math.log(value) : NaN : kind === 'reciprocal' || kind === 'inverse' ? value === 0 ? NaN : 1 / value : kind === 'sin' ? Math.sin(value) : kind === 'cos' ? Math.cos(value) : kind === 'tan' ? Math.tan(value) : Math.max(0, Math.min(1, value))) : undefined;
  if (kind === 'clamp' && keys.length >= 3) return operate(operate(pickInput(inputs, keys, ['value', 'input', 'x'], 0), pickInput(inputs, keys, ['min', 'minimum'], 1), (value, min) => Math.max(value, min)), pickInput(inputs, keys, ['max', 'maximum'], 2), (value, max) => Math.min(value, max));
  if ((kind === 'mix' || kind === 'lerp') && keys.length >= 3) return operateTernary(pickInput(inputs, keys, ['a', 'left', 'input1'], 0), pickInput(inputs, keys, ['b', 'right', 'input2'], 1), pickInput(inputs, keys, ['factor', 'weight', 'amount', 't'], 2), (left, right, factor) => left * (1 - factor) + right * factor);
  if (kind === 'smoothstep' && keys.length >= 3) return operateTernary(pickInput(inputs, keys, ['edge0', 'min', 'low'], 0), pickInput(inputs, keys, ['edge1', 'max', 'high'], 1), pickInput(inputs, keys, ['x', 'value', 'input'], 2), (edge0, edge1, value) => { if (edge0 === edge1) return NaN; const factor = Math.max(0, Math.min(1, (value - edge0) / (edge1 - edge0))); return factor * factor * (3 - 2 * factor); });
  if ((kind === 'select' || kind === 'if') && keys.length >= 3) {
    const condition = pickInput(inputs, keys, ['condition', 'cond', 'predicate', 'selector'], 0), branchTrue = pickInput(inputs, keys, ['true', 'ifTrue', 'then', 'yes'], 1), branchFalse = pickInput(inputs, keys, ['false', 'ifFalse', 'else', 'no'], 2);
    if (typeof condition !== 'boolean' && (typeof condition !== 'number' || !Number.isFinite(condition))) return undefined;
    return condition !== 0 ? normalizedValue(branchTrue) : normalizedValue(branchFalse);
  }
  const comparison = { ifgreater: (a, b) => a > b, ifgreatereq: (a, b) => a >= b, ifless: (a, b) => a < b, iflesseq: (a, b) => a <= b, ifequal: (a, b) => a === b }[kind];
  if (comparison && keys.length >= 4) {
    const left = pickInput(inputs, keys, ['value1', 'a', 'in1'], 0), right = pickInput(inputs, keys, ['value2', 'b', 'in2'], 1), branchTrue = pickInput(inputs, keys, ['ifTrue', 'true', 'then'], 2), branchFalse = pickInput(inputs, keys, ['ifFalse', 'false', 'else'], 3);
    if (typeof left !== 'number' || !Number.isFinite(left) || typeof right !== 'number' || !Number.isFinite(right)) return undefined;
    return comparison(left, right) ? normalizedValue(branchTrue) : normalizedValue(branchFalse);
  }
  return undefined;
};

export function optimizeMaterialGraph(graph) {
  const entries = graphEntries(graph), inputKeys = new Map(), nodes = new Map(entries.map(([id, node]) => { const key = String(id), inputKey = node?.inputs !== undefined ? 'inputs' : node?.parameters !== undefined ? 'parameters' : 'inputs', normalized = { ...(node || {}) }; delete normalized.inputs; delete normalized.parameters; inputKeys.set(key, inputKey); return [key, { ...normalized, id: String(node?.id ?? id), inputs: { ...graphInputs(node) } }]; }));
  // Prefer explicit output nodes as roots. A disconnected surface/material
  // helper is not an output merely because its type or identifier contains
  // "surface"; treating every such node as a root defeats unreachable-node
  // cleanup. Surface/material roots remain a useful fallback for graphs that
  // omit an explicit output node.
  const roots = materialGraphRootIds(nodes).map((id) => nodes.get(id)).filter(Boolean);
  const reachable = new Set(), visit = (id) => {
    const stack = [id];
    while (stack.length) {
      const current = stack.pop();
      if (reachable.has(current)) continue;
      const node = nodes.get(current);
      if (!node) continue;
      reachable.add(current);
      const children = Object.values(node.inputs || {}).map(reference).filter(Boolean);
      for (let index = children.length - 1; index >= 0; index--) stack.push(children[index]);
    }
  };
  for (const root of (roots.length ? roots : nodes.values())) visit(root.id);
  const values = new Map(), folded = [], ordered = [], orderedIds = new Set();
  const orderVisit = (root) => {
    if (!root || orderedIds.has(root.id)) return;
    const stack = [[root, false]];
    while (stack.length) {
      const [node, expanded] = stack.pop();
      if (!node) continue;
      if (expanded) { ordered.push(node); continue; }
      if (orderedIds.has(node.id)) continue;
      orderedIds.add(node.id);
      stack.push([node, true]);
      const children = Object.values(node.inputs || {}).map(reference).filter(Boolean);
      for (let index = children.length - 1; index >= 0; index--) stack.push([nodes.get(children[index]), false]);
    }
  };
  for (const root of [...(roots.length ? roots : nodes.values())].sort((a, b) => a.id.localeCompare(b.id))) orderVisit(root);
  for (const node of [...nodes.values()].sort((a, b) => a.id.localeCompare(b.id))) orderVisit(node);
  for (const node of ordered) {
    const kind = typeOf(node), inputs = Object.fromEntries(Object.entries(node.inputs || {}).map(([key, value]) => [key, literal(value, values)])), keys = Object.keys(inputs), allLiteral = keys.length > 0 && keys.every((key) => inputs[key] !== undefined);
    let result;
    if (reachable.has(node.id) && kind === 'constant' && finiteValue(node.value ?? inputs.value)) result = normalizedValue(node.value ?? inputs.value);
    else if (reachable.has(node.id) && allLiteral) {
      result = operationResult(kind, inputs, keys);
    }
    if (result !== undefined && finiteValue(result)) { values.set(node.id, result); folded.push(node.id); }
  }
  const output = ordered.filter((node) => reachable.has(node.id)).map((node) => { const value = values.get(node.id), inputKey = inputKeys.get(node.id) || 'inputs'; if (value === undefined) { const detached = detachedValue(node); delete detached.inputs; return { ...detached, [inputKey]: detachedValue(graphInputs(node)) }; } const detached = detachedValue(node); delete detached.inputs; return { ...detached, type: 'constant', [inputKey]: {}, value: normalizedValue(value) }; });
  return { nodes: output, folded: folded.sort(), removed: ordered.filter((node) => !reachable.has(node.id)).map((node) => node.id).sort(), changed: folded.length > 0 || output.length !== ordered.length };
}

// Evaluate one connected, literal-only output without invoking arbitrary
// shader code. This is intentionally conservative: texture/image and unknown
// nodes are reported as unsupported, while unresolved references are reported
// separately so bake callers can choose a texture or scalar fallback.
export function evaluateMaterialGraph(graph, { output = 'surface', resolveTexture = null, maxDepth = 1024, maxNodes = 4096 } = {}) {
  const depthLimit = Number.isSafeInteger(maxDepth) && maxDepth >= 1 && maxDepth <= 4096 ? maxDepth : 1024, nodeLimit = Number.isSafeInteger(maxNodes) && maxNodes >= 1 && maxNodes <= 100000 ? maxNodes : 4096, entries = graphEntries(graph), nodes = new Map(entries.map(([id, node]) => [String(id), { ...(node || {}), id: String(node?.id ?? id), inputs: { ...graphInputs(node) } }])), unsupportedNodes = new Set(), unresolvedReferences = new Set(), visiting = new Set(), memo = new Map();
  const evaluateValue = (value, depth = 0) => { const id = reference(value); if (id) { if (!nodes.has(id)) { unresolvedReferences.add(id); return undefined; } return evaluateNode(id, depth); } if (value && typeof value === 'object' && 'value' in value) return evaluateValue(value.value, depth); return finiteValue(value) ? normalizedValue(value) : undefined; };
  const evaluateNode = (id, depth = 0) => {
    if (memo.has(id)) return memo.get(id);
    if (depth > depthLimit || memo.size + visiting.size >= nodeLimit) { unsupportedNodes.add(id); return undefined; }
    if (visiting.has(id)) { unsupportedNodes.add(id); return undefined; }
    const node = nodes.get(id); if (!node) { unresolvedReferences.add(id); return undefined; }
    visiting.add(id); const kind = typeOf(node), keys = Object.keys(node.inputs || {}).sort(); let value;
    if (['constant', 'float', 'color', 'vector'].includes(kind) && finiteValue(node.value ?? node.inputs.value)) value = normalizedValue(node.value ?? node.inputs.value);
    else if (['image', 'texture'].includes(kind) && typeof resolveTexture === 'function') { try { const resolved = resolveTexture(detachedValue(node), id); if (finiteValue(resolved)) value = normalizedValue(resolved); else unsupportedNodes.add(id); } catch { unsupportedNodes.add(id); } }
    else if (keys.length) {
      const inputs = Object.fromEntries(keys.map((key) => [key, evaluateValue(node.inputs[key], depth + 1)]));
      if (keys.every((key) => inputs[key] !== undefined)) value = operationResult(kind, inputs, keys);
      if (value === undefined && !/(?:output|surface|material)/i.test(kind)) unsupportedNodes.add(id);
    } else if (!/(?:output|surface|material)/i.test(kind)) unsupportedNodes.add(id);
    visiting.delete(id); if (value !== undefined && finiteValue(value)) memo.set(id, normalizedValue(value)); return value;
  };
  const roots = materialGraphRootIds(nodes).map((id) => nodes.get(id)).filter(Boolean).sort((a, b) => { const rank = (node) => /output/i.test(typeOf(node)) || /^output$/i.test(node.id) ? 0 : 1; return rank(a) - rank(b) || a.id.localeCompare(b.id); });
  const root = roots[0]; if (!root) return { value: undefined, nodeId: null, supported: false, unsupportedNodes: [], unresolvedReferences: [], reason: 'No output or surface root was found.' };
  const rootInputs = root.inputs || {}, rootKind = typeOf(root), channelKeys = Object.keys(rootInputs).sort(), shaderRoot = rootKind.includes('surface') || rootKind === 'material';
  let requestedKey = rootInputs[output] !== undefined ? output : rootInputs.surface !== undefined ? 'surface' : shaderRoot ? null : channelKeys[0], channelValues;
  const surfaceReference = requestedKey === 'surface' ? reference(rootInputs.surface) : null, surfaceNode = surfaceReference ? nodes.get(surfaceReference) : null;
  if (!shaderRoot && surfaceNode && (typeOf(surfaceNode).includes('surface') || typeOf(surfaceNode) === 'material')) {
    const aliases = { baseColor: ['baseColor', 'base_color', 'diffuseColor', 'diffuse_color'], roughness: ['roughness', 'specular_roughness'], metallic: ['metallic', 'metalness'], opacity: ['opacity', 'alpha'], emissive: ['emissive', 'emissiveColor', 'emission_color', 'emissionColor'], normal: ['normal', 'normalMap', 'normal_map'] }[output] || [output];
    const surfaceKey = aliases.find((key) => Object.hasOwn(surfaceNode.inputs || {}, key));
    if (surfaceKey) { requestedKey = output; channelValues = { [output]: evaluateValue(surfaceNode.inputs[surfaceKey]) }; }
  }
  if (!channelValues) { const evaluatedKeys = requestedKey ? [requestedKey] : channelKeys; channelValues = Object.fromEntries(evaluatedKeys.map((key) => [key, evaluateValue(rootInputs[key])])); }
  const value = requestedKey ? channelValues[requestedKey] : channelKeys.length ? channelValues : undefined, allChannelsFinite = channelKeys.length > 0 && channelKeys.every((key) => channelValues[key] !== undefined), requestedValueFinite = requestedKey ? channelValues[requestedKey] !== undefined : allChannelsFinite, supported = requestedValueFinite && !unsupportedNodes.size && !unresolvedReferences.size;
  return { value: supported ? value : undefined, nodeId: root.id, supported, unsupportedNodes: [...unsupportedNodes].sort(), unresolvedReferences: [...unresolvedReferences].sort(), ...(supported ? {} : { reason: unsupportedNodes.size ? 'Output depends on an unsupported node.' : unresolvedReferences.size ? 'Output contains an unresolved node reference.' : 'Output could not be evaluated as a finite literal.' }) };
}

// Produce an authoring-ready graph snapshot without mutating the caller's
// graph. Folded values are inlined into their consumers, then the now-unused
// folded nodes and unreachable nodes are omitted. Unsupported nodes and
// unresolved references are retained so this helper cannot silently change
// material meaning.
export function rewriteMaterialGraph(graph) {
  const optimized = optimizeMaterialGraph(graph), originalInputs = new Map(graphEntries(graph).map(([id, node]) => [String(id), Object.keys(graphInputs(node)).sort()])), folded = new Map(optimized.nodes.filter((node) => optimized.folded.includes(node.id) && node.type === 'constant').map((node) => [node.id, normalizedValue(node.value)])), roots = new Set(materialGraphRootIds(optimized.nodes));
  const nodes = optimized.nodes.filter((node) => !folded.has(node.id) || roots.has(node.id)).map((node) => {
    const inputKey = node.inputs !== undefined ? 'inputs' : node.parameters !== undefined ? 'parameters' : 'inputs', inputs = Object.fromEntries(Object.entries(graphInputs(node)).map(([key, value]) => { const id = reference(value); return [key, id && folded.has(id) ? normalizedValue(folded.get(id)) : value]; }));
    return { ...node, ...(Object.keys(inputs).length || node[inputKey] !== undefined ? { [inputKey]: inputs } : {}) };
  });
  const inlined = [...folded.keys()].filter((id) => !roots.has(id)).sort(), removedInputs = inlined.map((nodeId) => ({ nodeId, inputs: originalInputs.get(nodeId) || [] })).filter((item) => item.inputs.length);
  return { ...optimized, nodes, inlined, removedInputs, changed: optimized.changed || nodes.length !== optimized.nodes.length };
}
