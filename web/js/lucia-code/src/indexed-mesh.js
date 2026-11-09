export const INDEXED_MESH_MAX_VERTICES = 10_000_000;
export const INDEXED_MESH_MAX_INDICES = 30_000_000;
const SUPPORTED_NUMERIC_ARRAYS = new Set([Float32Array, Float64Array, Uint8Array, Uint8ClampedArray, Uint16Array, Uint32Array, Int8Array, Int16Array, Int32Array]);
export const isSupportedNumericArray = (value) => ArrayBuffer.isView(value) && SUPPORTED_NUMERIC_ARRAYS.has(value.constructor);
export function numericArrayKind(value) {
  if (value instanceof Float32Array || value instanceof Float64Array) return 'float';
  if (isSupportedNumericArray(value)) return 'int';
  return null;
}

export function hasInvalidValue(values, predicate) {
  for (let i = 0; i < values.length; i += 1) if (predicate(values[i])) return true;
  return false;
}

// Keep triangle order and corner access identical across analyzers and
// processors without allocating a temporary [a, b, c] tuple per face.
export function forEachIndexedTriangle(indices, callback, start = 0, end = indices.length) {
  for (let offset = start, face = Math.floor(start / 3); offset + 2 < Math.min(end, indices.length); offset += 3, face += 1) callback(face, indices[offset], indices[offset + 1], indices[offset + 2]);
}

// Shared indexed topology primitive. The map values retain source face order,
// allowing analyzers and processors to apply their own winding or boundary
// policy without rebuilding the edge walk.
export function buildIndexedEdgeUses(indices, { vertexCount = null, accept = null } = {}) {
  const count = indices == null ? Number.isSafeInteger(vertexCount) && vertexCount >= 0 ? Math.floor(vertexCount / 3) : 0 : Number.isSafeInteger(indices.length) && indices.length >= 0 ? Math.floor(indices.length / 3) : 0;
  const adjacency = Array.from({ length: Number.isSafeInteger(vertexCount) && vertexCount >= 0 ? vertexCount : 0 }, () => new Set()), edges = new Map(), usedVertices = new Set(), orientedEdges = new Map();
  let windingConflicts = 0;
  for (let face = 0; face < count; face++) {
    const offset = face * 3, a = indices == null ? offset : indices[offset], b = indices == null ? offset + 1 : indices[offset + 1], c = indices == null ? offset + 2 : indices[offset + 2];
    if (accept && !accept(face, a, b, c)) continue;
    usedVertices.add(a); usedVertices.add(b); usedVertices.add(c);
    adjacency[a]?.add(b); adjacency[a]?.add(c); adjacency[b]?.add(a); adjacency[b]?.add(c); adjacency[c]?.add(a); adjacency[c]?.add(b);
    for (const [from, to] of [[a, b], [b, c], [c, a]]) {
      const key = from < to ? `${from},${to}` : `${to},${from}`;
      const entries = edges.get(key) || [];
      entries.push(face);
      edges.set(key, entries);
      const orientation = from < to ? 1 : -1;
      if (orientedEdges.get(key) === orientation) windingConflicts++;
      else if (!orientedEdges.has(key)) orientedEdges.set(key, orientation);
    }
  }
  const faceAdjacency = Array.from({ length: count }, () => []);
  for (const entries of edges.values()) if (entries.length === 2) { faceAdjacency[entries[0]].push(entries[1]); faceAdjacency[entries[1]].push(entries[0]); }
  return { edges, adjacency, faceAdjacency, usedVertices, windingConflicts };
}

// Build the same edge-use representation for a selected, source-face-ordered
// subset. Component analysis uses this after feature-aware face splitting.
export function buildIndexedEdgeUsesForFaces(indices, faces) {
  const edges = new Map();
  for (const face of faces || []) {
    if (!Number.isSafeInteger(face) || face < 0 || face * 3 + 2 >= indices.length) continue;
    const offset = face * 3;
    for (const [from, to] of [[indices[offset], indices[offset + 1]], [indices[offset + 1], indices[offset + 2]], [indices[offset + 2], indices[offset]]]) {
      const key = from < to ? `${from},${to}` : `${to},${from}`;
      const entries = edges.get(key) || [];
      entries.push(face);
      edges.set(key, entries);
    }
  }
  return edges;
}

// The diagnostic form keeps implicit (unindexed) topology allocation-free.
// Callers intentionally remain responsible for validating the values because
// inspection paths need to report malformed triangles rather than throw.
export function forEachMeshTriangle(indices, vertexCount, callback) {
  const count = indices == null ? Number.isSafeInteger(vertexCount) && vertexCount >= 0 ? Math.floor(vertexCount / 3) : 0 : Number.isSafeInteger(indices.length) && indices.length >= 0 ? Math.floor(indices.length / 3) : 0;
  for (let face = 0; face < count; face++) {
    const offset = face * 3;
    callback(face, indices == null ? offset : indices[offset], indices == null ? offset + 1 : indices[offset + 1], indices == null ? offset + 2 : indices[offset + 2]);
  }
}

const sliceValues = (values, start, end) => values.subarray ? values.subarray(start, end) : values.slice(start, end);
const outputConstructor = (values, fallback = Float32Array) => Array.isArray(values) ? fallback : values.constructor;

// Shared vertex compaction primitive for indexed processors. `usedVertices`
// is an ordered list of source vertex IDs; aligned attributes retain their
// element type while plain arrays become typed buffers at the boundary.
export function remapIndexedVertices({ positions, usedVertices, attributes = [] } = {}) {
  const positionOutput = new (outputConstructor(positions))(usedVertices.length * 3), attributeOutputs = attributes.map((attribute) => ({ ...attribute, array: new (outputConstructor(attribute.array))(usedVertices.length * attribute.itemSize) }));
  for (let target = 0; target < usedVertices.length; target++) {
    const source = usedVertices[target];
    positionOutput.set(sliceValues(positions, source * 3, source * 3 + 3), target * 3);
    for (let attributeIndex = 0; attributeIndex < attributeOutputs.length; attributeIndex++) {
      const attribute = attributeOutputs[attributeIndex], input = attributes[attributeIndex];
      attribute.array.set(sliceValues(input.array, source * input.itemSize, source * input.itemSize + input.itemSize), target * input.itemSize);
    }
  }
  return { positions: positionOutput, attributes: attributeOutputs };
}

// Expand one indexed vertex buffer into the triangle-corner layout required by
// consumers such as LightRT. The source index order is preserved exactly.
export function expandIndexedAttribute(values, indices, itemSize) {
  if (!values || typeof values.length !== 'number' || !indices || typeof indices.length !== 'number' || !Number.isSafeInteger(itemSize) || itemSize < 1 || values.length % itemSize) throw new Error('Indexed attribute expansion requires aligned values, indices, and a positive item size.');
  const output = new (outputConstructor(values))(indices.length * itemSize);
  for (let corner = 0; corner < indices.length; corner++) {
    const index = indices[corner];
    if (!Number.isSafeInteger(index) || index < 0 || index >= values.length / itemSize) throw new Error('Indexed attribute expansion received an out-of-range vertex index.');
    const source = index * itemSize;
    output.set(sliceValues(values, source, source + itemSize), corner * itemSize);
  }
  return output;
}

export function expandIndexedMesh({ positions, indices } = {}) {
  return { positions: expandIndexedAttribute(positions, indices, 3) };
}

// Compact face-varying values after faces have been filtered or reordered.
// `corners` contains one three-entry source-index tuple per retained face.
export function compactIndexedCorners(values, corners, itemSize) {
  const valueMap = new Map(), outputValues = [], outputIndices = [];
  for (const face of corners) for (const source of face) {
    let target = valueMap.get(source);
    if (target === undefined) {
      target = valueMap.size;
      valueMap.set(source, target);
      outputValues.push(...sliceValues(values, source * itemSize, source * itemSize + itemSize));
    }
    outputIndices.push(target);
  }
  return { values: new (outputConstructor(values))(outputValues), indices: Uint32Array.from(outputIndices) };
}

// Small, allocation-free interchange boundary shared by mesh processors.
// Callers can retain the original arrays; the normalized index view is only
// created when an unindexed mesh needs an explicit topology.
export function validateIndexedMesh({ positions, indices = null, requireTriangles = true, materializeIndices = true, maxVertices = INDEXED_MESH_MAX_VERTICES, maxIndices = INDEXED_MESH_MAX_INDICES } = {}) {
  if (typeof requireTriangles !== 'boolean') throw new Error('Mesh triangle-validation mode must be boolean.');
  if (typeof materializeIndices !== 'boolean') throw new Error('Mesh index materialization mode must be boolean.');
  if (!Number.isSafeInteger(maxVertices) || maxVertices < 0 || maxVertices > INDEXED_MESH_MAX_VERTICES || !Number.isSafeInteger(maxIndices) || maxIndices < 0 || maxIndices > INDEXED_MESH_MAX_INDICES) throw new Error('Mesh safety limits must be non-negative integers within the global bounds.');
  if (!positions || typeof positions.length !== 'number' || positions.length % 3) throw new Error('Positions must contain xyz triplets.');
  const vertexCount = positions.length / 3;
  if (!Number.isSafeInteger(vertexCount) || vertexCount > maxVertices) throw new Error(`Vertex count exceeds the safety limit of ${maxVertices}.`);
  if (hasInvalidValue(positions, (value) => !Number.isFinite(value))) throw new Error('Positions contain non-finite coordinates.');
  if (indices == null && vertexCount > maxIndices) throw new Error(`Generated index count exceeds the safety limit of ${maxIndices}.`);
  if (indices != null) {
    if (typeof indices.length !== 'number' || (requireTriangles && indices.length % 3)) throw new Error('Indices must contain complete triangle triplets.');
    if (!Number.isSafeInteger(indices.length) || indices.length > maxIndices) throw new Error(`Index count exceeds the safety limit of ${maxIndices}.`);
    if (hasInvalidValue(indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= vertexCount)) throw new Error('Indices must be in range of the position vertices.');
  }
  return { vertexCount, indexCount: indices?.length ?? vertexCount, indices: indices || (materializeIndices ? Uint32Array.from({ length: vertexCount }, (_, index) => index) : null) };
}

// Canonical worker interchange view. Validation is performed before any
// unindexed expansion, and indexed buffers are widened only when a processor
// needs the common Uint32 representation. Position storage is retained when
// it is already a Float32Array, avoiding an unnecessary copy at the boundary.
export function normalizeIndexedMesh({ positions, indices = null, maxVertices = INDEXED_MESH_MAX_VERTICES, maxIndices = INDEXED_MESH_MAX_INDICES } = {}) {
  const checked = validateIndexedMesh({ positions, indices, maxVertices, maxIndices });
  const sourcePositions = positions instanceof Float32Array ? positions : Float32Array.from(positions);
  if (hasInvalidValue(sourcePositions, (value) => !Number.isFinite(value))) throw new Error('Positions exceed the finite Float32 worker range.');
  const normalizedIndices = checked.indices instanceof Uint32Array ? checked.indices : Uint32Array.from(checked.indices);
  return { positions: sourcePositions, indices: normalizedIndices, vertexCount: checked.vertexCount, indexCount: normalizedIndices.length };
}

// Non-throwing companion for analyzers: diagnostics must continue scanning a
// stage after finding one malformed mesh.
export function inspectIndexedMesh({ positions, indices = null, maxVertices = INDEXED_MESH_MAX_VERTICES, maxIndices = INDEXED_MESH_MAX_INDICES } = {}) {
  const result = { positionsValid: false, indicesPresent: indices != null, indicesValid: indices == null, vertexCount: 0, indexCount: indices?.length ?? 0 };
  if (!Number.isSafeInteger(maxVertices) || maxVertices < 0 || maxVertices > INDEXED_MESH_MAX_VERTICES || !Number.isSafeInteger(maxIndices) || maxIndices < 0 || maxIndices > INDEXED_MESH_MAX_INDICES) return result;
  if (!positions || typeof positions.length !== 'number' || positions.length % 3) return result;
  result.vertexCount = positions.length / 3;
  if (!Number.isSafeInteger(result.vertexCount) || result.vertexCount > maxVertices) return result;
  result.positionsValid = true;
  result.positionsValid = !hasInvalidValue(positions, (value) => !Number.isFinite(value));
  if (indices == null) { result.indexCount = result.vertexCount; return result; }
  if (!Number.isSafeInteger(indices.length) || indices.length > maxIndices) return result;
  result.indicesValid = typeof indices.length === 'number' && indices.length % 3 === 0;
  if (result.indicesValid) {
    result.indicesValid = !hasInvalidValue(indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= result.vertexCount);
  }
  return result;
}
