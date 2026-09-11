import { LuciaError } from './utils.js';
import { buildIndexedEdgeUses, buildIndexedEdgeUsesForFaces, forEachIndexedTriangle, hasInvalidValue, remapIndexedVertices, validateIndexedMesh } from './indexed-mesh.js';

const edgeKey = (a, b) => a < b ? `${a},${b}` : `${b},${a}`;

export function extractConnectedComponents({ positions, indices, groups = [], attributes = [], faceVaryingAttributes = [], sharpEdges = null, sharpEdgeSharpness = null, minFaces = 1, componentFaces = null, splitAngle = null, splitConcavity = false }) {
  if (!positions?.length || positions.length % 3 || !indices?.length || indices.length % 3) throw new LuciaError('LUCIA_COMPONENT_INPUT', 'Component extraction requires indexed triangle geometry.');
  let vertexCount;
  try { vertexCount = validateIndexedMesh({ positions, indices }).vertexCount; } catch (error) { throw new LuciaError('LUCIA_COMPONENT_INPUT', `Component extraction received invalid mesh buffers: ${error.message}`); }
  if (sharpEdges != null && (!Array.isArray(sharpEdges) || sharpEdges.some((edge) => !Array.isArray(edge) || edge.length !== 2 || !edge.every(Number.isSafeInteger) || edge[0] < 0 || edge[1] < 0 || edge[0] >= vertexCount || edge[1] >= vertexCount || edge[0] === edge[1]))) throw new LuciaError('LUCIA_COMPONENT_SHARP_EDGES', 'Component sharp edges must contain distinct, in-range vertex pairs.');
  if (sharpEdgeSharpness != null && (!Array.isArray(sharpEdgeSharpness) || !sharpEdges || sharpEdgeSharpness.length !== sharpEdges.length || sharpEdgeSharpness.some((value) => !Number.isFinite(Number(value)) || Number(value) <= 0))) throw new LuciaError('LUCIA_COMPONENT_SHARP_EDGES', 'Component sharpEdgeSharpness must contain one positive finite value per sharp edge.');
  if (faceVaryingAttributes != null && !Array.isArray(faceVaryingAttributes)) throw new LuciaError('LUCIA_COMPONENT_FACEVARYING', 'Component face-varying attributes must be an array.');
  faceVaryingAttributes = faceVaryingAttributes || [];
  for (const attribute of faceVaryingAttributes) {
    if (!attribute?.array || !attribute?.indices || !Number.isInteger(attribute.itemSize) || attribute.itemSize < 1 || attribute.itemSize > 4 || attribute.array.length % attribute.itemSize || attribute.indices.length !== indices.length || hasInvalidValue(attribute.array, (value) => !Number.isFinite(value)) || hasInvalidValue(attribute.indices, (index) => !Number.isSafeInteger(index) || index < 0 || index >= attribute.array.length / attribute.itemSize)) throw new LuciaError('LUCIA_COMPONENT_FACEVARYING', 'Component extraction received invalid face-varying primvar buffers.');
  }
  const faceCount = indices.length / 3;
  if (splitAngle != null && (!Number.isFinite(splitAngle) || splitAngle < 0 || splitAngle > 180)) throw new LuciaError('LUCIA_COMPONENT_ANGLE', 'Component split angle must be between 0 and 180 degrees.');
  if (typeof splitConcavity !== 'boolean') throw new LuciaError('LUCIA_COMPONENT_CONCAVITY', 'Component concavity splitting must be boolean.');
  const groupForIndex = (index) => { const group = groups.find((candidate) => Number.isInteger(candidate?.start) && Number.isInteger(candidate?.count) && index >= candidate.start && index < candidate.start + candidate.count); return Number.isInteger(group?.materialIndex) && group.materialIndex >= 0 ? group.materialIndex : null; };
  const topology = buildIndexedEdgeUses(indices, { vertexCount }), adjacency = Array.from({ length: faceCount }, () => []), edges = topology.edges, faceNormals = new Float64Array(faceCount * 3);
  const calculateNormal = (face, a, b, c) => { const ax = positions[a * 3], ay = positions[a * 3 + 1], az = positions[a * 3 + 2], bx = positions[b * 3], by = positions[b * 3 + 1], bz = positions[b * 3 + 2], cx = positions[c * 3], cy = positions[c * 3 + 1], cz = positions[c * 3 + 2], ux = bx - ax, uy = by - ay, uz = bz - az, vx = cx - ax, vy = cy - ay, vz = cz - az, nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, length = Math.hypot(nx, ny, nz); if (length > 1e-15) { faceNormals[face * 3] = nx / length; faceNormals[face * 3 + 1] = ny / length; faceNormals[face * 3 + 2] = nz / length; } };
  forEachIndexedTriangle(indices, (face, a, b, c) => calculateNormal(face, a, b, c));
  let meshSignedVolume = 0;
  forEachIndexedTriangle(indices, (_face, a, b, c) => { const ax = positions[a * 3], ay = positions[a * 3 + 1], az = positions[a * 3 + 2], bx = positions[b * 3], by = positions[b * 3 + 1], bz = positions[b * 3 + 2], cx = positions[c * 3], cy = positions[c * 3 + 1], cz = positions[c * 3 + 2]; meshSignedVolume += (ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx)) / 6; });
  let allFaceNormalsValid = true;
  for (let face = 0; face < faceCount; face++) if (!(Math.hypot(faceNormals[face * 3], faceNormals[face * 3 + 1], faceNormals[face * 3 + 2]) > 1e-15)) { allFaceNormalsValid = false; break; }
  const concavityReliable = splitConcavity && allFaceNormalsValid && Math.abs(meshSignedVolume) > 1e-12 && [...edges.values()].every((list) => list.length === 2);
  const edgeIsConcave = (key, first, second) => {
    const [low, high] = key.split(',').map(Number), direction = [positions[high * 3] - positions[low * 3], positions[high * 3 + 1] - positions[low * 3 + 1], positions[high * 3 + 2] - positions[low * 3 + 2]];
    const orientation = (face) => { for (let corner = 0; corner < 3; corner++) { const from = indices[face * 3 + corner], to = indices[face * 3 + (corner + 1) % 3]; if (from === low && to === high) return 1; if (from === high && to === low) return -1; } return 0; };
    const firstOrientation = orientation(first), orderedFirst = firstOrientation >= 0 ? first : second, orderedSecond = firstOrientation >= 0 ? second : first;
    const crossX = faceNormals[orderedFirst * 3 + 1] * faceNormals[orderedSecond * 3 + 2] - faceNormals[orderedFirst * 3 + 2] * faceNormals[orderedSecond * 3 + 1], crossY = faceNormals[orderedFirst * 3 + 2] * faceNormals[orderedSecond * 3] - faceNormals[orderedFirst * 3] * faceNormals[orderedSecond * 3 + 2], crossZ = faceNormals[orderedFirst * 3] * faceNormals[orderedSecond * 3 + 1] - faceNormals[orderedFirst * 3 + 1] * faceNormals[orderedSecond * 3], turn = (crossX * direction[0] + crossY * direction[1] + crossZ * direction[2]) * (meshSignedVolume < 0 ? -1 : 1);
    return turn < -1e-12;
  };
  const minimumDot = splitAngle == null ? -1 : Math.cos(splitAngle * Math.PI / 180);
  for (const [key, list] of edges) for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) { const first = list[i], second = list[j], dot = faceNormals[first * 3] * faceNormals[second * 3] + faceNormals[first * 3 + 1] * faceNormals[second * 3 + 1] + faceNormals[first * 3 + 2] * faceNormals[second * 3 + 2]; if (splitAngle != null && dot + 1e-12 < minimumDot || concavityReliable && edgeIsConcave(key, first, second)) continue; adjacency[first].push(second); adjacency[second].push(first); }
  const visited = new Uint8Array(faceCount), output = [], minimum = Math.max(1, Math.floor(Number(minFaces) || 1));
  const faceSets = Array.isArray(componentFaces) ? componentFaces.map((faces) => [...new Set(faces)].filter((face) => Number.isSafeInteger(face) && face >= 0 && face < faceCount).sort((a, b) => a - b)) : null;
  const assignedFaces = new Set();
  if (faceSets && faceSets.some((faces) => !faces.length || faces.some((face) => assignedFaces.has(face) || (assignedFaces.add(face), false)))) throw new LuciaError('LUCIA_COMPONENT_CORRECTION', 'Corrected component face sets must be non-empty and non-overlapping.');
  const computedFaceSets = faceSets || (() => { const sets = []; for (let root = 0; root < faceCount; root++) if (!visited[root]) { const faces = [], queue = [root]; visited[root] = 1; for (let head = 0; head < queue.length; head++) { const face = queue[head]; faces.push(face); for (const next of adjacency[face]) if (!visited[next]) { visited[next] = 1; queue.push(next); } } sets.push(faces); } return sets; })();
  for (const faces of computedFaceSets) {
    if (faces.length < minimum) continue;
    const remap = new Map(), used = [], localIndices = [], materials = [];
    const localVertex = (source) => { let target = remap.get(source); if (target === undefined) { target = used.length; remap.set(source, target); used.push(source); } return target; };
    for (const face of faces) { localIndices.push(localVertex(indices[face * 3]), localVertex(indices[face * 3 + 1]), localVertex(indices[face * 3 + 2])); materials.push(groupForIndex(face * 3)); }
    const localAttributeInputs = attributes.filter((attribute) => attribute?.array && Number.isInteger(attribute.itemSize) && attribute.itemSize > 0 && attribute.array.length === vertexCount * attribute.itemSize), remapped = remapIndexedVertices({ positions, usedVertices: used, attributes: localAttributeInputs }), localPositions = remapped.positions, localAttributes = remapped.attributes;
    const localSharp = new Map();
    if (sharpEdges) for (let edgeIndex = 0; edgeIndex < sharpEdges.length; edgeIndex++) { const [from, to] = sharpEdges[edgeIndex], a = remap.get(from), b = remap.get(to); if (a == null || b == null || a === b) continue; const pair = a < b ? [a, b] : [b, a], key = `${pair[0]}:${pair[1]}`, value = sharpEdgeSharpness ? Number(sharpEdgeSharpness[edgeIndex]) : 1; localSharp.set(key, { edge: pair, value: Math.max(value, localSharp.get(key)?.value || 0) }); }
    const localSharpEntries = [...localSharp.values()];
    const localFaceVaryingAttributes = faceVaryingAttributes.map((attribute) => {
      const valueMap = new Map(), values = [], localCornerIndices = [];
      const localValue = (source) => { let target = valueMap.get(source); if (target === undefined) { target = valueMap.size; valueMap.set(source, target); for (let component = 0; component < attribute.itemSize; component++) values.push(attribute.array[source * attribute.itemSize + component]); } return target; };
      for (const face of faces) for (let corner = 0; corner < 3; corner++) localCornerIndices.push(localValue(attribute.indices[face * 3 + corner]));
      return { ...attribute, array: new (Array.isArray(attribute.array) ? Float32Array : attribute.array.constructor)(values), indices: Uint32Array.from(localCornerIndices) };
    });
    const localGroups = [];
    for (let face = 0; face < materials.length;) { const materialIndex = materials[face]; let end = face + 1; while (end < materials.length && materials[end] === materialIndex) end++; if (materialIndex != null) localGroups.push({ start: face * 3, count: (end - face) * 3, materialIndex }); face = end; }
    const componentEdges = buildIndexedEdgeUsesForFaces(indices, faces), componentFaceSet = new Set(faces), boundaryGraph = new Map(), boundaryFace = new Map(); for (const [key, entries] of componentEdges) if (entries.length === 1) { const [a, b] = key.split(',').map(Number), neighborsA = boundaryGraph.get(a) || [], neighborsB = boundaryGraph.get(b) || []; neighborsA.push(b); neighborsB.push(a); boundaryGraph.set(a, neighborsA); boundaryGraph.set(b, neighborsB); boundaryFace.set(key, (edges.get(key) || [])[0]); }
    const boundaryVisited = new Set(), boundaryLoopsData = []; for (const start of [...boundaryGraph.keys()].sort((a, b) => a - b)) if (!boundaryVisited.has(start)) { const vertices = [], queue = [start]; boundaryVisited.add(start); for (let head = 0; head < queue.length; head++) { const vertex = queue[head]; vertices.push(vertex); for (const neighbor of boundaryGraph.get(vertex) || []) if (!boundaryVisited.has(neighbor)) { boundaryVisited.add(neighbor); queue.push(neighbor); } } const closed = vertices.length >= 3 && vertices.every((vertex) => (boundaryGraph.get(vertex) || []).length === 2), firstNeighbor = (boundaryGraph.get(start) || [])[0], referenceFace = firstNeighbor == null ? null : (boundaryFace.get(edgeKey(start, firstNeighbor)) ?? null); boundaryLoopsData.push({ vertices, closed, referenceFace }); }
    let signedVolume = 0; for (const face of faces) { const a = indices[face * 3], b = indices[face * 3 + 1], c = indices[face * 3 + 2], ax = positions[a * 3], ay = positions[a * 3 + 1], az = positions[a * 3 + 2], bx = positions[b * 3], by = positions[b * 3 + 1], bz = positions[b * 3 + 2], cx = positions[c * 3], cy = positions[c * 3 + 1], cz = positions[c * 3 + 2]; signedVolume += (ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx)) / 6; }
    const boundaryEdges = [...componentEdges.values()].filter((entries) => entries.length === 1).length, nonManifoldEdges = [...componentEdges.values()].filter((entries) => entries.length > 2).length, concavityReliable = boundaryEdges === 0 && nonManifoldEdges === 0 && Math.abs(signedVolume) > 1e-12;
    let maxDihedralDegrees = 0, curvatureEdges = 0, concaveEdges = 0, convexEdges = 0; for (const [key, edgeEntries] of componentEdges) if (edgeEntries.length === 2) { const entries = edges.get(key) || []; if (entries.length !== 2 || !componentFaceSet.has(entries[0]) || !componentFaceSet.has(entries[1])) continue; const first = entries[0], second = entries[1], dot = Math.max(-1, Math.min(1, faceNormals[first * 3] * faceNormals[second * 3] + faceNormals[first * 3 + 1] * faceNormals[second * 3 + 1] + faceNormals[first * 3 + 2] * faceNormals[second * 3 + 2])), angle = Math.acos(dot) * 180 / Math.PI; if (Number.isFinite(angle)) { maxDihedralDegrees = Math.max(maxDihedralDegrees, angle); if (angle > 5) curvatureEdges++; if (concavityReliable && angle > 5) { const [low, high] = key.split(',').map(Number), direction = [positions[high * 3] - positions[low * 3], positions[high * 3 + 1] - positions[low * 3 + 1], positions[high * 3 + 2] - positions[low * 3 + 2]], orientation = (face) => { for (let corner = 0; corner < 3; corner++) { const from = indices[face * 3 + corner], to = indices[face * 3 + (corner + 1) % 3]; if (from === low && to === high) return 1; if (from === high && to === low) return -1; } return 0; }, firstOrientation = orientation(first), orderedFirst = firstOrientation >= 0 ? first : second, orderedSecond = firstOrientation >= 0 ? second : first, crossX = faceNormals[orderedFirst * 3 + 1] * faceNormals[orderedSecond * 3 + 2] - faceNormals[orderedFirst * 3 + 2] * faceNormals[orderedSecond * 3 + 1], crossY = faceNormals[orderedFirst * 3 + 2] * faceNormals[orderedSecond * 3] - faceNormals[orderedFirst * 3] * faceNormals[orderedSecond * 3 + 2], crossZ = faceNormals[orderedFirst * 3] * faceNormals[orderedSecond * 3 + 1] - faceNormals[orderedFirst * 3 + 1] * faceNormals[orderedSecond * 3], turn = (crossX * direction[0] + crossY * direction[1] + crossZ * direction[2]) * (signedVolume < 0 ? -1 : 1); if (turn < -1e-12) concaveEdges++; else if (turn > 1e-12) convexEdges++; } } }
    const boundaryLoops = boundaryLoopsData.filter((loop) => loop.closed), planarBoundaryLoopData = boundaryLoops.filter((loop) => { if (loop.referenceFace == null) return false; const face = loop.referenceFace, nx = faceNormals[face * 3], ny = faceNormals[face * 3 + 1], nz = faceNormals[face * 3 + 2], length = Math.hypot(nx, ny, nz), origin = loop.vertices[0], ox = positions[origin * 3], oy = positions[origin * 3 + 1], oz = positions[origin * 3 + 2], tolerance = Math.max(1e-8, Math.max(...loop.vertices.map((vertex) => Math.hypot(positions[vertex * 3] - ox, positions[vertex * 3 + 1] - oy, positions[vertex * 3 + 2] - oz))) * 1e-5); return length > 0 && loop.vertices.every((vertex) => Math.abs(nx * (positions[vertex * 3] - ox) + ny * (positions[vertex * 3 + 1] - oy) + nz * (positions[vertex * 3 + 2] - oz)) <= tolerance); }), planarBoundaryLoops = planarBoundaryLoopData.length, toLocalLoop = (loop) => loop.vertices.map((vertex) => remap.get(vertex)), planarBoundaryGroups = [];
    const componentExtent = localPositions.reduce((maximum, value) => Math.max(maximum, Math.abs(value)), 0), planeTolerance = Math.max(1e-8, componentExtent * 1e-5), minimumPlaneNormalDot = Math.cos(5 * Math.PI / 180);
    planarBoundaryLoopData.forEach((loop, loopIndex) => {
      const face = loop.referenceFace, normal = [faceNormals[face * 3], faceNormals[face * 3 + 1], faceNormals[face * 3 + 2]], origin = loop.vertices[0], point = [positions[origin * 3], positions[origin * 3 + 1], positions[origin * 3 + 2]], group = planarBoundaryGroups.find((candidate) => { const dot = normal[0] * candidate.normal[0] + normal[1] * candidate.normal[1] + normal[2] * candidate.normal[2], sign = dot < 0 ? -1 : 1, distance = (point[0] - candidate.point[0]) * candidate.normal[0] + (point[1] - candidate.point[1]) * candidate.normal[1] + (point[2] - candidate.point[2]) * candidate.normal[2]; return Math.abs(dot) >= minimumPlaneNormalDot && Math.abs(distance * sign) <= planeTolerance; });
      if (group) group.loops.push(loopIndex); else planarBoundaryGroups.push({ normal, point, loops: [loopIndex] });
    });
    const normal = faces.find((face) => Math.hypot(faceNormals[face * 3], faceNormals[face * 3 + 1], faceNormals[face * 3 + 2]) > 0); let normalDeviationDegrees = 0; if (normal != null) for (const face of faces) { const dot = Math.max(-1, Math.min(1, faceNormals[normal * 3] * faceNormals[face * 3] + faceNormals[normal * 3 + 1] * faceNormals[face * 3 + 1] + faceNormals[normal * 3 + 2] * faceNormals[face * 3 + 2])); normalDeviationDegrees = Math.max(normalDeviationDegrees, Math.acos(dot) * 180 / Math.PI); }
    output.push({ positions: localPositions, indices: Uint32Array.from(localIndices), attributes: localAttributes, ...(localFaceVaryingAttributes.length ? { faceVaryingAttributes: localFaceVaryingAttributes } : {}), ...(localGroups.length ? { groups: localGroups } : {}), ...(sharpEdges ? { sharpEdges: localSharpEntries.map(({ edge }) => edge), sharpEdgeSharpness: localSharpEntries.map(({ value }) => value) } : {}), sourceFaces: faces, sourceVertexCount: used.length, triangleCount: faces.length, boundaryEdges, nonManifoldEdges, boundaryLoops: boundaryLoops.length, planarBoundaryLoops, planarBoundaryLoopVertices: planarBoundaryLoopData.map(toLocalLoop), planarBoundaryGroups: planarBoundaryGroups.map((group) => [...group.loops]), normalDeviationDegrees: Number(normalDeviationDegrees.toFixed(6)), maxDihedralDegrees: Number(maxDihedralDegrees.toFixed(6)), curvatureEdges, concavityReliable, concaveEdges, convexEdges, nearlyPlanar: normal != null && normalDeviationDegrees <= 5 });
  }
  return output;
}

// Read-only segmentation contract for previews. Omitted components retain the
// -1 sentinel so callers cannot mistake a filtered fragment for an authored
// part.
export function previewConnectedComponents(data = {}) {
  const indices = data?.indices;
  if (!indices || indices.length % 3) throw new LuciaError('LUCIA_COMPONENT_INPUT', 'Component preview requires complete indexed triangle geometry.');
  const components = extractConnectedComponents(data), faceLabels = new Int32Array(indices.length / 3).fill(-1), summaries = [];
  components.forEach((component, componentId) => {
    for (const face of component.sourceFaces || []) if (Number.isSafeInteger(face) && face >= 0 && face < faceLabels.length) faceLabels[face] = componentId;
    summaries.push({ id: componentId, faces: [...(component.sourceFaces || [])], triangleCount: component.triangleCount, vertexCount: component.sourceVertexCount, materialIndices: [...new Set((component.groups || []).map((group) => group.materialIndex))].sort((a, b) => a - b), boundaryEdges: component.boundaryEdges, nonManifoldEdges: component.nonManifoldEdges, boundaryLoops: component.boundaryLoops, planarBoundaryLoops: component.planarBoundaryLoops, planarBoundaryLoopVertices: (component.planarBoundaryLoopVertices || []).map((loop) => [...loop]), planarBoundaryGroups: (component.planarBoundaryGroups || []).map((group) => [...group]), normalDeviationDegrees: component.normalDeviationDegrees, maxDihedralDegrees: component.maxDihedralDegrees, curvatureEdges: component.curvatureEdges, concavityReliable: component.concavityReliable, concaveEdges: component.concaveEdges, convexEdges: component.convexEdges, nearlyPlanar: component.nearlyPlanar });
  });
  return { faceLabels, components: summaries };
}

// Apply explicit review decisions to a preview without touching mesh data.
// IDs are remapped deterministically after discards and merge groups.
export function correctComponentPreview(preview, { mergeGroups = [], discard = [] } = {}) {
  if (!preview || typeof preview !== 'object' || Array.isArray(preview) || !preview.faceLabels || typeof preview.faceLabels.length !== 'number' || !Number.isSafeInteger(preview.faceLabels.length) || !Array.isArray(preview.components)) throw new LuciaError('LUCIA_COMPONENT_CORRECTION', 'Component corrections require a valid preview result.');
  if (!Array.isArray(mergeGroups) || !Array.isArray(discard) || mergeGroups.some((group) => !Array.isArray(group) || group.length < 2 || group.some((id) => !Number.isSafeInteger(id))) || discard.some((id) => !Number.isSafeInteger(id))) throw new LuciaError('LUCIA_COMPONENT_CORRECTION', 'Component corrections require integer merge groups and discard IDs.');
  const ids = new Set(preview.components.map((component) => component?.id));
  if (ids.size !== preview.components.length || [...ids].some((id) => !Number.isSafeInteger(id)) || mergeGroups.flat().some((id) => !ids.has(id)) || discard.some((id) => !ids.has(id))) throw new LuciaError('LUCIA_COMPONENT_CORRECTION', 'Component corrections reference an unknown component ID.');
  const discarded = new Set(discard), mergedInto = new Map();
  for (const group of mergeGroups) {
    const unique = [...new Set(group)].filter((id) => !discarded.has(id));
    if (unique.length < 2 || unique.some((id) => mergedInto.has(id))) throw new LuciaError('LUCIA_COMPONENT_CORRECTION', 'Component merge groups must not overlap or contain discarded components.');
    const target = Math.min(...unique); unique.forEach((id) => mergedInto.set(id, target));
  }
  const buckets = new Map();
  for (const component of [...preview.components].sort((a, b) => a.id - b.id)) {
    if (discarded.has(component.id)) continue;
    const key = mergedInto.get(component.id) ?? component.id, bucket = buckets.get(key) || { sourceIds: [], faces: [], triangleCount: 0, vertexCount: 0, materialIndices: new Set() };
    bucket.sourceIds.push(component.id); bucket.faces.push(...(Array.isArray(component.faces) ? component.faces : [])); bucket.triangleCount += Number.isSafeInteger(component.triangleCount) ? component.triangleCount : 0; bucket.vertexCount += Number.isSafeInteger(component.vertexCount) ? component.vertexCount : 0; for (const material of component.materialIndices || []) if (Number.isSafeInteger(material)) bucket.materialIndices.add(material); buckets.set(key, bucket);
  }
  const ordered = [...buckets.entries()].sort(([a], [b]) => a - b), remap = new Map(ordered.map(([source], id) => [source, id])), sourceToOutput = new Map();
  for (const [source, bucket] of ordered) for (const id of bucket.sourceIds) sourceToOutput.set(id, remap.get(source));
  const faceLabels = new Int32Array(preview.faceLabels.length).fill(-1);
  preview.faceLabels.forEach((source, face) => { if (Number.isSafeInteger(source) && sourceToOutput.has(source)) faceLabels[face] = sourceToOutput.get(source); });
  const components = ordered.map(([, bucket], id) => ({ id, sourceIds: [...bucket.sourceIds], faces: [...new Set(bucket.faces)].sort((a, b) => a - b), triangleCount: bucket.triangleCount, vertexCount: bucket.vertexCount, materialIndices: [...bucket.materialIndices].sort((a, b) => a - b) }));
  const normalizedMergeGroups = [...buckets.values()].filter((bucket) => bucket.sourceIds.length > 1).map((bucket) => [...bucket.sourceIds]).sort((a, b) => a[0] - b[0]);
  return { faceLabels, components, discardedIds: [...discarded].sort((a, b) => a - b), mergedGroups: normalizedMergeGroups, changed: discarded.size > 0 || normalizedMergeGroups.length > 0 };
}

// Materialize a reviewed correction while retaining the original corner data.
// The corrected face sets are fed back through the same extractor, so merged
// components get fresh vertex/face-varying remaps instead of sharing stale
// preview-only labels.
export function extractCorrectedComponents(data = {}, corrections = {}) {
  const preview = previewConnectedComponents(data), corrected = correctComponentPreview(preview, corrections);
  return { ...corrected, components: extractConnectedComponents({ ...data, componentFaces: corrected.components.map((component) => component.faces), minFaces: 1 }) };
}
