import { buildIndexedEdgeUses, compactIndexedCorners, forEachIndexedTriangle, hasInvalidValue, normalizeIndexedMesh, remapIndexedVertices } from './indexed-mesh.js';
import { extractConnectedComponents } from './mesh-components.js';

const arrayConstructor = (values, fallback = Float32Array) => Array.isArray(values) ? fallback : values.constructor;
const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';

export function cleanupMesh(data) {
  if (!data || typeof data !== 'object' || Array.isArray(data)) throw new Error('Cleanup requires a mesh data object.');
  const mesh = normalizeIndexedMesh({ positions: data.positions, indices: data.indices || null }), positions = mesh.positions, sourceIndices = mesh.indices;
  if (data.fillPlanarHoles != null && typeof data.fillPlanarHoles !== 'boolean') throw new Error('Cleanup fillPlanarHoles must be boolean.');
  const vertexCount = positions.length / 3;
  const sharpEdges = data.sharpEdges == null ? null : data.sharpEdges, sharpness = data.sharpEdgeSharpness == null ? null : data.sharpEdgeSharpness;
  if (sharpEdges != null && (!Array.isArray(sharpEdges) || sharpEdges.some((edge) => !Array.isArray(edge) || edge.length !== 2 || !edge.every(Number.isSafeInteger) || edge[0] < 0 || edge[1] < 0 || edge[0] >= vertexCount || edge[1] >= vertexCount || edge[0] === edge[1]))) throw new Error('Cleanup sharp edges must contain distinct, in-range vertex pairs.');
  if (sharpness != null && (!Array.isArray(sharpness) || !sharpEdges || sharpness.length !== sharpEdges.length || sharpness.some((value) => !Number.isFinite(Number(value)) || Number(value) <= 0))) throw new Error('Cleanup sharpEdgeSharpness must contain one positive finite value per sharp edge.');
  if (data.jointIndices && data.jointIndices.length !== vertexCount * 4) throw new Error('Cleanup joint indices must contain four influences per vertex.');
  if (data.jointWeights && data.jointWeights.length !== vertexCount * 4) throw new Error('Cleanup joint weights must contain four influences per vertex.');
  if (data.jointIndices && hasInvalidValue(data.jointIndices, (value) => !Number.isSafeInteger(value) || value < 0 || value > 65535)) throw new Error('Cleanup joint indices must be non-negative 16-bit integers.');
  if (data.jointWeights && hasInvalidValue(data.jointWeights, (value) => !Number.isFinite(value) || value < 0)) throw new Error('Cleanup joint weights must be finite and non-negative.');
  if (data.groups != null && !Array.isArray(data.groups)) throw new Error('Cleanup material groups must be an array.');
  const sourceGroups = data.groups || [], orderedGroups = sourceGroups.map((group) => ({ start: group?.start, count: group?.count, materialIndex: group?.materialIndex })).sort((a, b) => (a.start || 0) - (b.start || 0));
  let groupEnd = 0;
  for (const group of orderedGroups) {
    if (!Number.isSafeInteger(group.start) || !Number.isSafeInteger(group.count) || !Number.isSafeInteger(group.materialIndex) || group.start < 0 || group.count <= 0 || group.start % 3 || group.count % 3 || group.start + group.count > sourceIndices.length || group.start < groupEnd || group.materialIndex < 0) throw new Error('Cleanup material groups overlap or fall outside the index buffer.');
    groupEnd = group.start + group.count;
  }
  const groupForIndex = (index) => {
    const group = sourceGroups.find((candidate) => Number.isInteger(candidate?.start) && Number.isInteger(candidate?.count) && index >= candidate.start && index < candidate.start + candidate.count);
    return Number.isInteger(group?.materialIndex) && group.materialIndex >= 0 ? group.materialIndex : null;
  };
  const aligned = (values, itemSize, name) => { if (values == null) return null; if (!isIterableBuffer(values) || values.length !== vertexCount * itemSize || hasInvalidValue(values, (value) => !Number.isFinite(value))) throw new Error(`Cleanup ${name} must be a finite vertex-aligned buffer.`); return new Float32Array(values); };
  const faceVaryingUV = data.uvIndices != null;
  if (faceVaryingUV && (!isIterableBuffer(data.uvs) || !isIterableBuffer(data.uvIndices) || data.uvs.length < 2 || data.uvs.length % 2 || data.uvIndices.length !== sourceIndices.length || hasInvalidValue(data.uvs, (value) => !Number.isFinite(value)) || hasInvalidValue(data.uvIndices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= data.uvs.length / 2))) throw new Error('Cleanup face-varying UVs must contain finite values and one valid index per face corner.');
  const normals = aligned(data.normals, 3, 'normals'), uvs = faceVaryingUV ? new Float32Array(data.uvs) : aligned(data.uvs, 2, 'UVs'), colors = aligned(data.colors, 3, 'colors'), tangents = aligned(data.tangents, 4, 'tangents'), jointIndices = data.jointIndices ? new Uint16Array(data.jointIndices) : null, jointWeights = data.jointWeights ? new Float32Array(data.jointWeights) : null;
  if (data.customAttributes != null && !Array.isArray(data.customAttributes)) throw new Error('Cleanup custom attributes must be an array.');
  if (data.faceVaryingAttributes != null && !Array.isArray(data.faceVaryingAttributes)) throw new Error('Cleanup face-varying attributes must be an array.');
  const attributeNames = new Set();
  const customAttributes = (data.customAttributes || []).map((attribute) => { if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || attributeNames.has(attribute.name) || !isIterableBuffer(attribute?.array) || !Number.isInteger(attribute?.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length !== vertexCount * attribute.itemSize || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value))) throw new Error('Cleanup custom attributes must be finite, named, unique, vertex-aligned buffers with itemSize 1–4.'); attributeNames.add(attribute.name); return { ...attribute, array: new (arrayConstructor(attribute.array))(attribute.array) }; });
  const faceVaryingAttributes = (data.faceVaryingAttributes || []).map((attribute) => { if (!/^[A-Za-z_][\w:]*$/.test(attribute?.name || '') || attributeNames.has(attribute.name) || !isIterableBuffer(attribute?.array) || !isIterableBuffer(attribute?.indices) || !Number.isInteger(attribute?.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length % attribute.itemSize || attribute.indices.length !== sourceIndices.length || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value)) || hasInvalidValue(attribute.indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= attribute.array.length / attribute.itemSize)) throw new Error('Cleanup face-varying attributes must have finite, named, unique values and one valid index per face corner.'); attributeNames.add(attribute.name); return { ...attribute, array: new (arrayConstructor(attribute.array))(attribute.array), indices: new Uint32Array(attribute.indices) }; });
  let minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (let i = 0; i < positions.length; i += 3) { minX = Math.min(minX, positions[i]); minY = Math.min(minY, positions[i + 1]); minZ = Math.min(minZ, positions[i + 2]); maxX = Math.max(maxX, positions[i]); maxY = Math.max(maxY, positions[i + 1]); maxZ = Math.max(maxZ, positions[i + 2]); }
  const scale = Math.max(maxX - minX, maxY - minY, maxZ - minZ, 1e-12), rawTolerance = data.tolerance == null ? 0 : Number(data.tolerance);
  if (!Number.isFinite(rawTolerance) || rawTolerance < 0) throw new Error('Cleanup tolerance must be finite and non-negative.');
  const tolerance = Math.min(.1, rawTolerance), minimumArea = Math.max(0, Math.min(1, Number(data.minArea) || 0)) * scale * scale, quantum = tolerance * scale;
  const canonical = new Uint32Array(positions.length / 3), vertexMap = new Map(), weldBuckets = new Map(), weldAttributes = new Map();
  for (let i = 0; i < canonical.length; i++) {
    const values = [positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]];
    if (normals?.length === positions.length) values.push(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]);
    if (!faceVaryingUV && uvs?.length === canonical.length * 2) values.push(uvs[i * 2], uvs[i * 2 + 1]);
    if (colors?.length === positions.length) values.push(colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]);
    if (tangents?.length === positions.length / 3 * 4) values.push(...tangents.subarray(i * 4, i * 4 + 4));
    if (jointIndices?.length === positions.length / 3 * 4) values.push(...jointIndices.subarray(i * 4, i * 4 + 4));
    if (jointWeights?.length === positions.length / 3 * 4) values.push(...jointWeights.subarray(i * 4, i * 4 + 4));
    for (const attribute of customAttributes) if (attribute.array.length === canonical.length * attribute.itemSize) for (let c = 0; c < attribute.itemSize; c++) values.push(attribute.array[i * attribute.itemSize + c]);
    let existing;
    if (quantum) {
      const x = positions[i * 3], y = positions[i * 3 + 1], z = positions[i * 3 + 2], bx = Math.floor(x / quantum), by = Math.floor(y / quantum), bz = Math.floor(z / quantum), attributeKey = values.slice(3).join(',');
      for (let dx = -1; dx <= 1 && existing === undefined; dx++) for (let dy = -1; dy <= 1 && existing === undefined; dy++) for (let dz = -1; dz <= 1 && existing === undefined; dz++) {
        const candidates = weldBuckets.get(`${bx + dx},${by + dy},${bz + dz}`) || [];
        for (const candidate of candidates) {
          const cx = positions[candidate * 3], cy = positions[candidate * 3 + 1], cz = positions[candidate * 3 + 2], distance = Math.hypot(x - cx, y - cy, z - cz);
          if (weldAttributes.get(candidate) === attributeKey && distance <= quantum) { existing = candidate; break; }
        }
      }
      if (existing === undefined) { existing = i; const bucket = `${bx},${by},${bz}`, candidates = weldBuckets.get(bucket) || []; candidates.push(i); weldBuckets.set(bucket, candidates); weldAttributes.set(i, attributeKey); }
    } else {
      const key = values.every(Number.isFinite) ? values.join(',') : `unique:${i}`;
      existing = vertexMap.get(key); if (existing === undefined) { existing = i; vertexMap.set(key, i); }
    }
    canonical[i] = existing;
  }
  const faces = [], faceMaterials = [], faceAreas = [], faceVaryingCorners = faceVaryingAttributes.map(() => []), uvCorners = [], seenFaces = new Set();
  forEachIndexedTriangle(sourceIndices, (face, sourceA, sourceB, sourceC) => {
    const i = face * 3;
    if (sourceA >= canonical.length || sourceB >= canonical.length || sourceC >= canonical.length) return;
    const a = canonical[sourceA], b = canonical[sourceB], c = canonical[sourceC];
    if (a >= positions.length / 3 || b >= positions.length / 3 || c >= positions.length / 3 || a === b || b === c || a === c) return;
    const ax = positions[a * 3], ay = positions[a * 3 + 1], az = positions[a * 3 + 2];
    const abx = positions[b * 3] - ax, aby = positions[b * 3 + 1] - ay, abz = positions[b * 3 + 2] - az;
    const acx = positions[c * 3] - ax, acy = positions[c * 3 + 1] - ay, acz = positions[c * 3 + 2] - az;
    const nx = aby * acz - abz * acy, ny = abz * acx - abx * acz, nz = abx * acy - aby * acx;
    if (![ax, ay, az, nx, ny, nz].every(Number.isFinite) || Math.sqrt(nx * nx + ny * ny + nz * nz) * .5 <= Math.max(1e-10, minimumArea)) return;
    const key = [a, b, c].sort((x, y) => x - y).join(',');
    if (seenFaces.has(key)) return;
    seenFaces.add(key); faces.push(a, b, c); faceMaterials.push(groupForIndex(i)); faceAreas.push(Math.sqrt(nx * nx + ny * ny + nz * nz) * .5); if (faceVaryingUV) uvCorners.push([data.uvIndices[i], data.uvIndices[i + 1], data.uvIndices[i + 2]]);
    faceVaryingAttributes.forEach((attribute, index) => faceVaryingCorners[index].push([attribute.indices[i], attribute.indices[i + 1], attribute.indices[i + 2]]));
  });
  const topology = buildIndexedEdgeUses(faces); let edgeMap = topology.edges, faceCount = faces.length / 3, adjacent = topology.faceAdjacency;
  const edgeKey = (a, b) => a < b ? `${a},${b}` : `${b},${a}`;
  const visited = new Uint8Array(faceCount), hasEdge = (face, from, to) => {
    for (let corner = 0; corner < 3; corner++) if (faces[face * 3 + corner] === from && faces[face * 3 + (corner + 1) % 3] === to) return true;
    return false;
  };
  for (let root = 0; root < faceCount; root++) if (!visited[root]) {
    const queue = [root]; let head = 0; visited[root] = 1;
    while (head < queue.length) {
      const face = queue[head++];
      for (const neighbor of adjacent[face]) if (!visited[neighbor]) {
        let shouldFlip = false;
        for (let corner = 0; corner < 3; corner++) {
          const from = faces[face * 3 + corner], to = faces[face * 3 + (corner + 1) % 3];
          if (hasEdge(neighbor, from, to)) { shouldFlip = true; break; }
        }
        if (shouldFlip) { [faces[neighbor * 3 + 1], faces[neighbor * 3 + 2]] = [faces[neighbor * 3 + 2], faces[neighbor * 3 + 1]]; faceVaryingCorners.forEach((corners) => { [corners[neighbor][1], corners[neighbor][2]] = [corners[neighbor][2], corners[neighbor][1]]; }); if (faceVaryingUV) [uvCorners[neighbor][1], uvCorners[neighbor][2]] = [uvCorners[neighbor][2], uvCorners[neighbor][1]]; }
        visited[neighbor] = 1; queue.push(neighbor);
      }
    }
  }
  let filledHoles = 0;
  const holeTriangles = [];
  if (data.fillPlanarHoles && faceCount) {
    const boundary = new Map();
    for (const [key, entries] of edgeMap) if (entries.length === 1) {
      const face = entries[0], offset = face * 3;
      for (let corner = 0; corner < 3; corner++) {
        const from = faces[offset + corner], to = faces[offset + (corner + 1) % 3];
        if (edgeKey(from, to) === key) { const list = boundary.get(from) || []; list.push({ to, face }); boundary.set(from, list); break; }
      }
    }
    const consumed = new Set(), loops = [];
    for (const [start, outgoing] of boundary) for (const first of outgoing) {
      const edgeId = `${start}:${first.to}`; if (consumed.has(edgeId)) continue;
      const loop = [start]; let from = start, edge = first, valid = true;
      for (let guard = 0; guard <= boundary.size + 1; guard++) {
        const id = `${from}:${edge.to}`; if (consumed.has(id)) { valid = edge.to === start; break; }
        consumed.add(id); loop.push(edge.to);
        if (edge.to === start) break;
        const next = boundary.get(edge.to) || [];
        if (next.length !== 1) { valid = false; break; }
        from = edge.to; edge = next[0];
        if (guard === boundary.size) valid = false;
      }
      if (valid && loop.length >= 4 && loop.at(-1) === start) loops.push(loop.slice(0, -1));
    }
    const point = (index) => [positions[index * 3], positions[index * 3 + 1], positions[index * 3 + 2]];
    const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const area2 = (a, b, c) => (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    const pointInTriangle = (p, a, b, c, sign) => sign * area2(a, b, p) >= -1e-10 && sign * area2(b, c, p) >= -1e-10 && sign * area2(c, a, p) >= -1e-10;
    for (const loop of loops) {
      const points = loop.map(point), normal = [0, 0, 0];
      for (let i = 0; i < points.length; i++) { const a = points[i], b = points[(i + 1) % points.length]; normal[0] += (a[1] - b[1]) * (a[2] + b[2]); normal[1] += (a[2] - b[2]) * (a[0] + b[0]); normal[2] += (a[0] - b[0]) * (a[1] + b[1]); }
      const normalLength = Math.hypot(...normal); if (normalLength <= 1e-10) continue;
      const n = normal.map((value) => value / normalLength), axis = n.map(Math.abs).indexOf(Math.max(...n.map(Math.abs))), projected = points.map((p) => axis === 0 ? [p[1], p[2]] : axis === 1 ? [p[0], p[2]] : [p[0], p[1]]);
      const origin = points[0], planarTolerance = scale * 1e-5; if (points.some((p) => Math.abs(dot(n, [p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]])) > planarTolerance)) continue;
      const signedArea = projected.reduce((sum, p, i) => sum + p[0] * projected[(i + 1) % projected.length][1] - p[1] * projected[(i + 1) % projected.length][0], 0) * .5, sign = signedArea >= 0 ? 1 : -1, remaining = loop.map((_, i) => i), triangles = [];
      while (remaining.length > 2) {
        let ear = -1;
        for (let i = 0; i < remaining.length; i++) { const prev = remaining[(i + remaining.length - 1) % remaining.length], current = remaining[i], next = remaining[(i + 1) % remaining.length]; if (sign * area2(projected[prev], projected[current], projected[next]) <= 1e-10) continue; if (remaining.some((candidate) => candidate !== prev && candidate !== current && candidate !== next && pointInTriangle(projected[candidate], projected[prev], projected[current], projected[next], sign))) continue; ear = i; break; }
        if (ear < 0) { triangles.length = 0; break; }
        const prev = remaining[(ear + remaining.length - 1) % remaining.length], current = remaining[ear], next = remaining[(ear + 1) % remaining.length]; triangles.push([loop[prev], loop[current], loop[next]]); remaining.splice(ear, 1);
      }
      if (!triangles.length) continue;
      holeTriangles.push(...triangles);
      const material = (() => { for (const index of loop) for (const candidate of edgeMap.get(edgeKey(index, loop[(loop.indexOf(index) + 1) % loop.length])) || []) return faceMaterials[candidate]; return null; })();
      for (const triangle of triangles) { faces.push(...triangle); faceMaterials.push(material); faceAreas.push(Math.hypot(...cross([positions[(triangle[1]) * 3] - positions[triangle[0] * 3], positions[triangle[1] * 3 + 1] - positions[triangle[0] * 3 + 1], positions[triangle[1] * 3 + 2] - positions[triangle[0] * 3 + 2]], [positions[triangle[2] * 3] - positions[triangle[0] * 3], positions[triangle[2] * 3 + 1] - positions[triangle[0] * 3 + 1], positions[triangle[2] * 3 + 2] - positions[triangle[0] * 3 + 2]])) * .5); if (faceVaryingUV) { const fallback = new Map(); for (let i = 0; i < sourceIndices.length; i++) if (!fallback.has(sourceIndices[i])) fallback.set(sourceIndices[i], data.uvIndices[i]); uvCorners.push(triangle.map((vertex) => fallback.get(vertex) ?? 0)); } faceVaryingCorners.forEach((corners, attributeIndex) => { const attribute = faceVaryingAttributes[attributeIndex], fallback = new Map(); for (let i = 0; i < sourceIndices.length; i++) if (!fallback.has(sourceIndices[i])) fallback.set(sourceIndices[i], attribute.indices[i]); corners.push(triangle.map((vertex) => fallback.get(vertex) ?? 0)); }); }
      filledHoles++;
    }
  }
  if (filledHoles) { const rebuiltTopology = buildIndexedEdgeUses(faces); edgeMap = rebuiltTopology.edges; faceCount = faces.length / 3; adjacent = rebuiltTopology.faceAdjacency; }
  const componentThreshold = Math.max(0, Math.min(1, Number(data.minComponentArea) || 0)) * scale * scale;
  const volumeThreshold = Math.max(0, Math.min(1, Number(data.minComponentVolume) || 0)) * scale * scale * scale;
  if ((componentThreshold > 0 || volumeThreshold > 0) && faceCount) {
    const componentIds = new Int32Array(faceCount).fill(-1), componentAreas = [], componentVolumes = [], componentClosed = [];
    for (let root = 0; root < faceCount; root++) if (componentIds[root] < 0) {
      const component = componentAreas.length, queue = [root]; let area = 0, volume = 0, closed = true; componentIds[root] = component;
      for (let head = 0; head < queue.length; head++) {
        const face = queue[head], a = faces[face * 3], b = faces[face * 3 + 1], c = faces[face * 3 + 2];
        area += faceAreas[face];
        const ax = positions[a * 3], ay = positions[a * 3 + 1], az = positions[a * 3 + 2], bx = positions[b * 3], by = positions[b * 3 + 1], bz = positions[b * 3 + 2], cx = positions[c * 3], cy = positions[c * 3 + 1], cz = positions[c * 3 + 2];
        volume += (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)) / 6;
        for (let corner = 0; corner < 3; corner++) { const edge = edgeMap.get(edgeKey(faces[face * 3 + corner], faces[face * 3 + (corner + 1) % 3])); if (edge?.length !== 2) closed = false; }
        for (const neighbor of adjacent[face]) if (componentIds[neighbor] < 0) { componentIds[neighbor] = component; queue.push(neighbor); }
      }
      componentAreas.push(area); componentVolumes.push(Math.abs(volume)); componentClosed.push(closed);
    }
    const filteredFaces = [], filteredMaterials = [], filteredCorners = faceVaryingCorners.map(() => []), filteredUVCorners = [];
    for (let face = 0; face < faceCount; face++) { const component = componentIds[face], keepArea = componentAreas[component] > componentThreshold, keepVolume = !volumeThreshold || !componentClosed[component] || componentVolumes[component] > volumeThreshold; if (keepArea && keepVolume) { filteredFaces.push(faces[face * 3], faces[face * 3 + 1], faces[face * 3 + 2]); filteredMaterials.push(faceMaterials[face]); if (faceVaryingUV) filteredUVCorners.push(uvCorners[face]); faceVaryingCorners.forEach((corners, index) => filteredCorners[index].push(corners[face])); } }
    faces.splice(0, faces.length, ...filteredFaces); faceMaterials.splice(0, faceMaterials.length, ...filteredMaterials); if (faceVaryingUV) uvCorners.splice(0, uvCorners.length, ...filteredUVCorners); faceVaryingCorners.forEach((corners, index) => { corners.splice(0, corners.length, ...filteredCorners[index]); });
  }
  const remap = new Int32Array(positions.length / 3).fill(-1), used = [];
  for (const index of faces) if (remap[index] < 0) { remap[index] = used.length; used.push(index); }
  const remapAttributes = [
    ...(normals?.length === positions.length ? [{ name: 'normals', itemSize: 3, array: normals }] : []),
    ...(uvs?.length === positions.length / 3 * 2 && !faceVaryingUV ? [{ name: 'uvs', itemSize: 2, array: uvs }] : []),
    ...(colors?.length === positions.length ? [{ name: 'colors', itemSize: 3, array: colors }] : []),
    ...(tangents?.length === positions.length / 3 * 4 ? [{ name: 'tangents', itemSize: 4, array: tangents }] : []),
    ...(jointIndices?.length === positions.length / 3 * 4 ? [{ name: 'jointIndices', itemSize: 4, array: jointIndices }] : []),
    ...(jointWeights?.length === positions.length / 3 * 4 ? [{ name: 'jointWeights', itemSize: 4, array: jointWeights }] : []),
    ...customAttributes,
  ];
  const remapped = remapIndexedVertices({ positions, usedVertices: used, attributes: remapAttributes }), outPositions = remapped.positions;
  let remappedIndex = 0;
  const nextRemapped = (present) => present ? remapped.attributes[remappedIndex++].array : null;
  const outNormals = nextRemapped(normals?.length === positions.length), outUvs = nextRemapped(uvs?.length === positions.length / 3 * 2 && !faceVaryingUV), outColors = nextRemapped(colors?.length === positions.length), outTangents = nextRemapped(tangents?.length === positions.length / 3 * 4), outJointIndices = nextRemapped(jointIndices?.length === positions.length / 3 * 4), outJointWeights = nextRemapped(jointWeights?.length === positions.length / 3 * 4), outCustomAttributes = remapped.attributes.slice(remappedIndex);
  const outIndices = Uint32Array.from(faces, (index) => remap[index]), compactedUV = faceVaryingUV ? compactIndexedCorners(uvs, uvCorners, 2) : null, outFaceVaryingAttributes = faceVaryingAttributes.map((attribute, attributeIndex) => {
    const compacted = compactIndexedCorners(attribute.array, faceVaryingCorners[attributeIndex], attribute.itemSize);
    return { ...attribute, array: compacted.values, indices: compacted.indices };
  });
  const weightedSharpEdges = new Map();
  if (sharpEdges) for (let index = 0; index < sharpEdges.length; index++) {
    const [from, to] = sharpEdges[index], mappedFrom = remap[from], mappedTo = remap[to];
    if (mappedFrom < 0 || mappedTo < 0 || mappedFrom === mappedTo) continue;
    const pair = mappedFrom < mappedTo ? [mappedFrom, mappedTo] : [mappedTo, mappedFrom], key = `${pair[0]}:${pair[1]}`, value = sharpness ? Number(sharpness[index]) : 1;
    weightedSharpEdges.set(key, { edge: pair, value: Math.max(value, weightedSharpEdges.get(key)?.value || 0) });
  }
  const remappedSharpEntries = [...weightedSharpEdges.values()];
  const groups = [];
  if (sourceGroups.length) {
    for (let face = 0; face < faceMaterials.length;) {
      const materialIndex = faceMaterials[face];
      let end = face + 1;
      while (end < faceMaterials.length && faceMaterials[end] === materialIndex) end++;
      if (materialIndex != null) groups.push({ start: face * 3, count: (end - face) * 3, materialIndex });
      face = end;
    }
  }
  const retainedTriangles = Math.max(0, faces.length / 3 - holeTriangles.length), beforeTriangles = Math.floor(sourceIndices.length / 3);
  return { positions: outPositions, indices: outIndices, normals: outNormals, uvs: compactedUV?.values || outUvs, ...(compactedUV ? { uvIndices: compactedUV.indices } : {}), colors: outColors, tangents: outTangents, jointIndices: outJointIndices, jointWeights: outJointWeights, customAttributes: outCustomAttributes, faceVaryingAttributes: outFaceVaryingAttributes, ...(sourceGroups.length ? { groups } : {}), ...(sharpEdges ? { sharpEdges: remappedSharpEntries.map(({ edge }) => edge), sharpEdgeSharpness: remappedSharpEntries.map(({ value }) => value) } : {}), beforeVertices: positions.length / 3, afterVertices: used.length, beforeTriangles, retainedTriangles, afterTriangles: outIndices.length / 3, removedTriangles: Math.max(0, beforeTriangles - retainedTriangles), filledHoles, proposedHoleTriangles: holeTriangles.map((triangle) => [...triangle]) };
}

// Read-only preview contract for callers that need to show the proposed fill
// result before authoring. Cleanup remains the single deterministic classifier.
export function previewPlanarHoleFill(data = {}) {
  const result = cleanupMesh({ ...data, fillPlanarHoles: true });
  return { filledHoles: result.filledHoles, beforeTriangles: result.beforeTriangles, retainedTriangles: result.retainedTriangles, removedTriangles: result.removedTriangles, afterTriangles: result.afterTriangles, addedTriangles: Math.max(0, result.afterTriangles - result.retainedTriangles), proposedTriangles: result.proposedHoleTriangles };
}

// Preview the explicit effect of tolerance welding on disconnected pieces.
// This is intentionally separate from generic cleanup so a caller can show
// which component merges a confirmation will cause before authoring.
export function previewCrackMerge(data = {}, { tolerance = data.tolerance } = {}) {
  const mesh = normalizeIndexedMesh({ positions: data?.positions, indices: data?.indices || null }), positions = mesh.positions, indices = mesh.indices;
  if (!positions || !indices || indices.length % 3) throw new Error('Crack-merge preview requires complete indexed triangle geometry.');
  const normalizedTolerance = Number(tolerance);
  if (!Number.isFinite(normalizedTolerance) || normalizedTolerance <= 0) throw new Error('Crack-merge preview requires a positive finite tolerance.');
  const before = extractConnectedComponents({ positions, indices }), result = cleanupMesh({ ...data, indices, tolerance: normalizedTolerance }), after = extractConnectedComponents({ positions: result.positions, indices: result.indices });
  return { ...result, beforeComponents: before.length, afterComponents: after.length, mergedComponents: Math.max(0, before.length - after.length), changed: before.length !== after.length || result.afterTriangles !== result.beforeTriangles || result.afterVertices !== result.beforeVertices };
}
