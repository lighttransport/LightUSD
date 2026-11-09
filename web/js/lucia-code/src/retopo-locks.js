import { buildIndexedEdgeUses, INDEXED_MESH_MAX_INDICES } from './indexed-mesh.js';

export const MAX_SHARP_CHAINS = 1_000_000;
export const MAX_SHARP_CHAIN_VERTICES = INDEXED_MESH_MAX_INDICES;
export const MAX_SHARP_CHAIN_TEXT_LENGTH = 1_048_576;
export const MAX_SHARP_EDGES = 1_000_000;

export function deriveBoundaryLocks(indices, groups, vertexCount) {
  const locks = new Uint8Array(vertexCount);
  for (const group of groups) {
    const range = indices?.subarray ? indices.subarray(group.start, group.start + group.count) : indices?.slice?.(group.start, group.start + group.count), edges = buildIndexedEdgeUses(range).edges;
    for (const [key, entries] of edges) if (entries.length === 1) { const [a, b] = key.split(',').map(Number); if (a >= 0 && b >= 0 && a < vertexCount && b < vertexCount) { locks[a] = 1; locks[b] = 1; } }
  }
  return locks;
}

// UV seams are commonly represented by duplicated render vertices: the
// positions coincide, but the UV values differ. Lock every member of such a
// duplicate set so simplification cannot collapse the seam into one island.
export function deriveUVSeamLocks(positions, uvs, vertexCount) {
  const locks = new Uint8Array(vertexCount);
  if (!positions || !uvs || positions.length !== vertexCount * 3 || uvs.length !== vertexCount * 2) return locks;
  const duplicates = new Map();
  for (let vertex = 0; vertex < vertexCount; vertex++) {
    const positionKey = `${positions[vertex * 3]},${positions[vertex * 3 + 1]},${positions[vertex * 3 + 2]}`;
    const entries = duplicates.get(positionKey) || [];
    entries.push({ vertex, u: uvs[vertex * 2], v: uvs[vertex * 2 + 1] });
    duplicates.set(positionKey, entries);
  }
  for (const entries of duplicates.values()) {
    if (entries.length < 2) continue;
    const first = entries[0];
    if (entries.some((entry) => entry.u !== first.u || entry.v !== first.v)) for (const { vertex } of entries) locks[vertex] = 1;
  }
  return locks;
}

export function deriveEdgeLocks(edges, vertexCount) {
  const locks = new Uint8Array(vertexCount);
  for (const edge of edges || []) if (Array.isArray(edge) && edge.length === 2 && Number.isInteger(edge[0]) && Number.isInteger(edge[1]) && edge[0] !== edge[1] && edge[0] >= 0 && edge[1] >= 0 && edge[0] < vertexCount && edge[1] < vertexCount) { locks[edge[0]] = 1; locks[edge[1]] = 1; }
  return locks;
}

export function validateVertexLockMask(values, vertexCount) {
  if (values == null) return null;
  if (!Number.isInteger(vertexCount) || vertexCount < 0 || values.length !== vertexCount) throw new Error('Vertex lock mask must contain one value per source vertex.');
  for (let i = 0; i < values.length; i += 1) if (values[i] !== 0 && values[i] !== 1) throw new Error('Vertex lock mask must contain only zero or one values.');
  return new Uint8Array(values);
}

// Parse the compact form used by the Operations inspector, for example
// "0, 4, 10-13".  Keeping this as a mask makes the worker boundary explicit
// and avoids passing UI strings into meshoptimizer.
export function parseVertexLockSelection(value, vertexCount) {
  if (!Number.isInteger(vertexCount) || vertexCount < 0) throw new Error('Vertex count must be a non-negative integer.');
  const locks = new Uint8Array(vertexCount);
  const text = String(value ?? '').trim();
  if (!text) return locks;
  for (const token of text.split(',')) {
    const part = token.trim();
    if (!part) throw new Error('Vertex locks contain an empty item. Use comma-separated indices or ranges.');
    const match = part.match(/^(\d+)(?:\s*-\s*(\d+))?$/);
    if (!match) throw new Error(`Invalid vertex lock "${part}". Use indices or inclusive ranges such as 10-13.`);
    const start = Number(match[1]), end = match[2] == null ? start : Number(match[2]);
    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start > end || end >= vertexCount) throw new Error(`Vertex lock "${part}" is outside the valid range 0-${Math.max(0, vertexCount - 1)}.`);
    locks.fill(1, start, end + 1);
  }
  return locks;
}

export function parseSharpEdgeSelection(value, vertexCount) {
  if (!Number.isInteger(vertexCount) || vertexCount < 0) throw new Error('Vertex count must be a non-negative integer.');
  const text = String(value ?? '').trim();
  if (!text) return [];
  if (text.length > MAX_SHARP_CHAIN_TEXT_LENGTH) throw new Error(`Sharp edge text must be at most ${MAX_SHARP_CHAIN_TEXT_LENGTH.toLocaleString()} characters.`);
  const edges = [], seen = new Set();
  for (const token of text.split(';')) {
    const part = token.trim(), match = part.match(/^(\d+)\s*[:\-]\s*(\d+)$/);
    if (!match) throw new Error(`Invalid sharp edge "${part}". Use pairs such as 0:2; 4:5.`);
    const a = Number(match[1]), b = Number(match[2]);
    if (!Number.isSafeInteger(a) || !Number.isSafeInteger(b) || a >= vertexCount || b >= vertexCount || a === b) throw new Error(`Sharp edge "${part}" is outside the valid vertex range or has identical endpoints.`);
    const edge = a < b ? [a, b] : [b, a], key = edge.join(':');
    if (!seen.has(key)) { if (edges.length >= MAX_SHARP_EDGES) throw new Error(`Sharp edges must contain at most ${MAX_SHARP_EDGES.toLocaleString()} unique edges.`); seen.add(key); edges.push(edge); }
  }
  return edges;
}

export function validateSharpChains(chains, sharpness = null, vertexCount = Infinity) {
  if (!Array.isArray(chains) || chains.length > MAX_SHARP_CHAINS) throw new Error(`Sharp chains must contain at most ${MAX_SHARP_CHAINS.toLocaleString()} chains.`);
  let vertexTotal = 0;
  for (const chain of chains) {
    const invalidVertex = Array.isArray(chain) && chain.some((value, index) => !Number.isSafeInteger(value) || value < 0 || value >= vertexCount || index > 0 && value === chain[index - 1]);
    if (!Array.isArray(chain) || chain.length < 2 || chain.length > MAX_SHARP_CHAIN_VERTICES || vertexTotal + chain.length > MAX_SHARP_CHAIN_VERTICES || invalidVertex) throw new Error('Sharp chains must contain at least two distinct, in-range integer vertices within the mesh safety limit.');
    vertexTotal += chain.length;
  }
  if (sharpness != null && (!Array.isArray(sharpness) || sharpness.length !== chains.length || sharpness.some((value) => !Number.isFinite(Number(value)) || Number(value) <= 0))) throw new Error('Sharp chain sharpness must contain one positive finite value per chain.');
  return { chains: chains.map((chain) => [...chain]), sharpness: sharpness == null ? chains.map(() => 1) : sharpness.map(Number) };
}

export function parseSharpChainSelection(value, vertexCount) {
  const text = String(value ?? '').trim();
  if (!text) return { chains: [], sharpness: [] };
  if (text.length > MAX_SHARP_CHAIN_TEXT_LENGTH) throw new Error(`Sharp chain text must be at most ${MAX_SHARP_CHAIN_TEXT_LENGTH.toLocaleString()} characters.`);
  const chains = [], sharpness = [];
  for (const token of text.split(';')) {
    const match = token.trim().match(/^(\d+(?:\s*>\s*\d+)+)(?:\s*@\s*((?:\d+(?:\.\d+)?|\.\d+)(?:[eE][+-]?\d+)?))?$/);
    if (!match) throw new Error(`Invalid sharp chain "${token.trim()}". Use chains such as 0>1>2@0.5; 4>5.`);
    chains.push(match[1].split('>').map((item) => Number(item.trim()))); sharpness.push(match[2] == null ? 1 : Number(match[2]));
  }
  return validateSharpChains(chains, sharpness, vertexCount);
}

export function expandSharpChains(chains, sharpness = null) {
  const normalized = validateSharpChains(chains, sharpness), weighted = new Map();
  for (let chainIndex = 0; chainIndex < normalized.chains.length; chainIndex++) for (let index = 1; index < normalized.chains[chainIndex].length; index++) {
    const a = normalized.chains[chainIndex][index - 1], b = normalized.chains[chainIndex][index], pair = a < b ? [a, b] : [b, a], key = `${pair[0]}:${pair[1]}`, value = normalized.sharpness[chainIndex];
    weighted.set(key, { edge: pair, value: Math.max(value, weighted.get(key)?.value || 0) });
  }
  const entries = [...weighted.values()].sort((left, right) => left.edge[0] - right.edge[0] || left.edge[1] - right.edge[1]);
  return { edges: entries.map(({ edge }) => edge), sharpness: entries.map(({ value }) => value) };
}
