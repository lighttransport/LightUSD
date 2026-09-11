import { analyzePhysicsMesh } from './physics-analysis.js';
import { analyzeSkinning } from './rig-analysis.js';
import { buildIndexedEdgeUses, forEachIndexedTriangle, forEachMeshTriangle, hasInvalidValue, inspectIndexedMesh } from './indexed-mesh.js';
import { graphEntries, graphInputs, materialGraphRootIds, MATERIAL_GRAPH_AUDITABLE_TYPES, MATERIAL_GRAPH_BAKEABLE_TYPES } from './material-graph.js';

const finite = (value) => Number.isFinite(value);
const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';
const stableText = (value, fallback) => value == null || (typeof value !== 'string' && typeof value !== 'number') ? fallback : String(value);
const bytesHash = (bytes) => { let hash = 2166136261; if (!bytes || typeof bytes.length !== 'number') return hash >>> 0; for (let index = 0; index < bytes.length; index++) { const value = Number(bytes[index]); hash ^= Number.isFinite(value) ? value : 0; hash = Math.imul(hash, 16777619); } return hash >>> 0; };
const arrayHash = (array) => array?.byteLength ? bytesHash(new Uint8Array(array.buffer, array.byteOffset, array.byteLength)) : 0;
const sameArray = (left, right) => Boolean(left && right && left.length === right.length && left.every((value, index) => value === right[index]));
function canonicalGeometryParts(position, index) {
  const vertexCount = position.length / 3, indices = index ? index : Uint32Array.from({ length: vertexCount }, (_, value) => value);
  const vertices = Array.from({ length: vertexCount }, (_, value) => `${position[value * 3]},${position[value * 3 + 1]},${position[value * 3 + 2]}`), order = [...Array(vertexCount).keys()].sort((a, b) => vertices[a].localeCompare(vertices[b]) || a - b), remap = new Uint32Array(vertexCount);
  order.forEach((old, next) => { remap[old] = next; });
  const triangles = [];
  forEachIndexedTriangle(indices, (face, a, b, c) => triangles.push([remap[a], remap[b], remap[c]].sort((left, right) => left - right).join(',')));
  triangles.sort();
  return { vertexCount, indexCount: indices.length, vertices: order.map((value) => vertices[value]), triangles };
}
function canonicalGeometryEqual(leftPosition, leftIndex, rightPosition, rightIndex) {
  const left = canonicalGeometryParts(leftPosition, leftIndex), right = canonicalGeometryParts(rightPosition, rightIndex);
  return left.vertexCount === right.vertexCount && left.indexCount === right.indexCount && left.vertices.length === right.vertices.length && left.vertices.every((value, index) => value === right.vertices[index]) && left.triangles.length === right.triangles.length && left.triangles.every((value, index) => value === right.triangles[index]);
}
function geometryFingerprint(position, index) {
  const vertexCount = position.length / 3, indices = index ? index : Uint32Array.from({ length: vertexCount }, (_, value) => value);
  if (vertexCount > 100000 || indices.length > 300000) return { key: `${vertexCount}:${indices.length}:${arrayHash(position)}:${arrayHash(index)}`, canonical: false };
  const parts = canonicalGeometryParts(position, index);
  let hash = 2166136261;
  for (const value of [...parts.vertices, ...parts.triangles]) for (const character of value) { hash ^= character.charCodeAt(0); hash = Math.imul(hash, 16777619); }
  return { key: `${vertexCount}:${indices.length}:${hash >>> 0}`, canonical: true };
}

const AUDITABLE_NODE_TYPES = new Set(MATERIAL_GRAPH_AUDITABLE_TYPES);
export function inventoryMaterialGraph(material) {
  const source = material?.userData?.nodes || material?.nodes;
  if (!source) return { nodes: [], unsupportedNodes: [], bakeableNodes: [], unresolvedReferences: [] };
  const entries = graphEntries(source);
  const nodes = entries.map(([key, node]) => {
    const type = String(node?.type || node?.nodeType || node?.category || 'unknown').trim();
    const inputs = Object.keys(graphInputs(node)).sort();
    return { id: String(key), type, inputs };
  }).sort((a, b) => a.id.localeCompare(b.id) || a.type.localeCompare(b.type));
  const unsupportedNodes = nodes.filter((node) => !AUDITABLE_NODE_TYPES.has(node.type.toLowerCase().replace(/[ -]/g, ''))).map((node) => node.id);
  const bakeableNodes = nodes.filter((node) => MATERIAL_GRAPH_BAKEABLE_TYPES.includes(node.type.toLowerCase().replace(/[ -]/g, ''))).map((node) => node.id);
  const byId = new Map(entries.map(([key, node]) => [String(key), node])), roots = materialGraphRootIds(nodes);
  const reachable = new Set(), visit = (id) => { if (reachable.has(id)) return; reachable.add(id); const node = byId.get(id); for (const value of Object.values(graphInputs(node))) { const references = value && typeof value === 'object' ? [value.node, value.id, value.source] : [value]; for (const reference of references) if (reference != null && byId.has(String(reference))) visit(String(reference)); } };
  for (const root of (roots.length ? roots : nodes.map((node) => node.id))) visit(root);
  const unreachableNodes = roots.length ? nodes.filter((node) => !reachable.has(node.id)).map((node) => node.id) : [];
  const unresolvedReferences = [];
  for (const [nodeId, node] of entries) for (const [input, value] of Object.entries(graphInputs(node))) {
    const candidates = value && typeof value === 'object' ? [value.node, value.id, value.source] : [];
    for (const reference of candidates.filter((item) => item != null)) if (!byId.has(String(reference))) unresolvedReferences.push(`${nodeId}.${input} → ${reference}`);
  }
  return { nodes, unsupportedNodes, bakeableNodes, unreachableNodes, unresolvedReferences: [...new Set(unresolvedReferences)].sort() };
}
function graphFingerprint(material) {
  const source = material?.userData?.nodes || material?.nodes;
  if (!source || typeof source !== 'object') return null;
  const entries = graphEntries(source), byId = new Map(entries.map(([id, node]) => [String(id), node])), active = new Set(), memo = new Map();
  const roots = materialGraphRootIds(entries.map(([id, node]) => ({ ...(node || {}), id: String(id) })));
  const reachable = new Set(), collect = (id) => { if (reachable.has(id)) return; const node = byId.get(id); if (!node) return; reachable.add(id); for (const value of Object.values(graphInputs(node))) { const referenceId = value && typeof value === 'object' ? value.node ?? value.id ?? value.source : null; if (referenceId != null && byId.has(String(referenceId))) collect(String(referenceId)); } };
  (roots.length ? roots : entries.map(([id]) => String(id))).forEach(collect);
  const stable = (value) => { if (Array.isArray(value)) return `[${value.map(stable).join(',')}]`; if (value && typeof value === 'object') return `{${Object.keys(value).sort().map((key) => `${JSON.stringify(key)}:${stable(value[key])}`).join(',')}}`; return JSON.stringify(value); };
  const encode = (value) => {
    if (!value || typeof value !== 'object') return stable(value);
    const referenceId = value.node ?? value.id ?? value.source;
    if (referenceId != null && byId.has(String(referenceId))) { const extras = { ...value }; delete extras.node; delete extras.id; delete extras.source; return `@${visit(String(referenceId))}${Object.keys(extras).length ? stable(extras) : ''}`; }
    return stable(value);
  };
  const visit = (id) => {
    if (memo.has(id)) return memo.get(id);
    if (active.has(id)) return '<cycle>';
    active.add(id); const node = byId.get(id) || {}, inputs = graphInputs(node), signature = `${String(node.type || node.nodeType || node.category || 'unknown').toLowerCase().replace(/[ _-]/g, '')}(${Object.keys(inputs).sort().map((key) => `${key}=${encode(inputs[key])}`).join(';')})${Object.hasOwn(node, 'value') ? `=${encode(node.value)}` : ''}`;
    active.delete(id); memo.set(id, signature); return signature;
  };
  return [...reachable].sort().map((id) => visit(id)).sort().join('|');
}

function materialFingerprint(material) {
  const scalar = ['roughness', 'metalness', 'opacity', 'transparent', 'side', 'alphaTest', 'emissiveIntensity'].map((key) => [key, material?.[key] ?? null]);
  const color = (value) => value == null ? null : value.isColor && typeof value.getHexString === 'function' ? value.getHexString() : Array.isArray(value) ? value.slice(0, 4) : typeof value === 'object' ? [value.r, value.g, value.b, value.a].map((component) => component ?? null) : value;
  const colors = ['color', 'emissive'].map((key) => [key, color(material?.[key])]);
  const textures = ['map', 'normalMap', 'roughnessMap', 'metalnessMap', 'aoMap', 'emissiveMap', 'alphaMap', 'bumpMap', 'displacementMap'].map((key) => [key, material?.[key]?.uuid || null]);
  return JSON.stringify({ shader: material?.type || null, scalar, colors, textures, graph: graphFingerprint(material) });
}

function issue(severity, ruleId, path, message, fix = null) {
  return { severity, ruleId, path, message, automaticFixSafe: Boolean(fix), fix };
}

export function analyzeAsset(root) {
  const meshes = [], materials = new Map(), materialFingerprints = new Map(), textures = new Map(), textureContent = new Map(), geometryFingerprints = new Map(), missingTextureIds = new Set(), issues = [];
  let triangles = 0, vertices = 0, gpuBytes = 0, duplicateGeometryTotal = 0, uvMeshes = 0, uvNonFinite = 0, uvOutOfRange = 0, uvZeroArea = 0, uvOverlapTotal = 0, uvIslandTotal = 0, normalCountMismatch = 0, normalNonFinite = 0, normalZeroLength = 0, invertedNormalTotal = 0, tangentCountMismatch = 0, tangentNonFinite = 0, missingTangentTotal = 0, duplicateIndexTotal = 0, invalidIndexTotal = 0, unusedVertexTotal = 0, boundaryEdgeTotal = 0, nonManifoldEdgeTotal = 0, inconsistentWindingTotal = 0, disconnectedComponentTotal = 0, negativeScaleTotal = 0, extremeTransformTotal = 0, nonFiniteTransformTotal = 0, missingTextureImages = 0, invalidTextureDimensions = 0, invalidTexturePixels = 0, malformedAttributeTotal = 0, indexCountMismatchTotal = 0, uvDensityCount = 0, uvDensitySum = 0, uvDensitySquaredSum = 0;
  let uvMinU = Infinity, uvMinV = Infinity, uvMaxU = -Infinity, uvMaxV = -Infinity, uvOverlapGrid = new Uint16Array(64 * 64), uvDistortionGridSum = new Float64Array(64 * 64), uvDistortionGridCount = new Uint32Array(64 * 64);
  root?.traverse?.((object) => {
    if (!object || typeof object !== 'object' || Array.isArray(object)) { malformedAttributeTotal++; issues.push(issue('error', 'scene.invalidNode', '/', 'Scene traversal returned a non-object node.')); return; }
    const path = object.userData?.['primMeta.absPath'] || object.name || '/';
    const scale = object.scale, scaleValues = scale ? [scale.x, scale.y, scale.z] : [], positionValues = object.position ? [object.position.x, object.position.y, object.position.z] : [], rotationValues = object.rotation ? [object.rotation.x, object.rotation.y, object.rotation.z] : [], nonFiniteScale = scaleValues.filter((value) => !finite(value)).length, nonFinitePose = [...positionValues, ...rotationValues].filter((value) => !finite(value)).length, negativeScale = scaleValues.filter((value) => finite(value) && value < 0).length, extremeScale = scaleValues.filter((value) => finite(value) && value !== 0 && (Math.abs(value) > 1e4 || Math.abs(value) < 1e-4)).length, extremeTranslation = positionValues.filter((value) => finite(value) && Math.abs(value) > 1e6).length;
    nonFiniteTransformTotal += nonFiniteScale + nonFinitePose; negativeScaleTotal += negativeScale; extremeTransformTotal += extremeScale + extremeTranslation;
    if (nonFiniteScale) issues.push(issue('error', 'transform.nonFiniteScale', path, `${nonFiniteScale} transform scale axes are non-finite.`));
    if (nonFinitePose) issues.push(issue('error', 'transform.nonFinite', path, `${nonFinitePose} transform position or rotation components are non-finite.`));
    if (negativeScale) issues.push(issue('info', 'transform.negativeScale', path, `${negativeScale} transform scale axes are negative; winding and tangent handedness may be mirrored.`));
    if (extremeScale) issues.push(issue('warning', 'transform.extremeScale', path, `${extremeScale} transform scale axes are outside the stable range [1e-4, 1e4].`));
    if (extremeTranslation) issues.push(issue('warning', 'transform.extremeTranslation', path, `${extremeTranslation} transform position components exceed the stable translation range ±1e6.`));
    if (!object.isMesh) return;
    const geometry = object.geometry, position = geometry?.attributes?.position;
    if (!position) { malformedAttributeTotal++; issues.push(issue('error', 'topology.missingPosition', path, 'Mesh has no position attribute; topology cannot be analyzed.')); return; }
    if (!position.array || typeof position.array.length !== 'number' || !Number.isInteger(position.count) || position.count < 0) { malformedAttributeTotal++; issues.push(issue('error', 'topology.positionArray', path, 'Position attribute has no valid numeric backing array or count.')); return; }
    const normalRaw = geometry.attributes.normal, tangentRaw = geometry.attributes.tangent, uvRaw = geometry.attributes.uv, index = geometry.index, skinIndex = geometry.attributes.skinIndex, skinWeight = geometry.attributes.skinWeight, malformedOptional = [['normal', normalRaw], ['tangent', tangentRaw], ['uv', uvRaw]].filter(([, attribute]) => attribute?.array != null && !isIterableBuffer(attribute.array)), malformedGroups = geometry.groups != null && !Array.isArray(geometry.groups);
    for (const [name] of malformedOptional) { malformedAttributeTotal++; issues.push(issue('error', 'topology.attributeArray', path, `${name} attribute has a non-iterable backing array.`)); }
    if (malformedGroups) { malformedAttributeTotal++; issues.push(issue('error', 'topology.groupCollection', path, 'Mesh material groups must be an array.')); }
    const normal = malformedOptional.some(([name]) => name === 'normal') ? null : normalRaw, tangent = malformedOptional.some(([name]) => name === 'tangent') ? null : tangentRaw, uv = malformedOptional.some(([name]) => name === 'uv') ? null : uvRaw, skin = analyzeSkinning({ skinIndices: skinIndex?.array, skinWeights: skinWeight?.array, boneCount: object.skeleton?.bones?.length ?? null, boneMatrices: object.skeleton?.boneMatrices, bindMatrices: object.skeleton?.boneInverses, skeleton: object.skeleton }), meshInspection = inspectIndexedMesh({ positions: position.array, indices: index?.array });
    const positionMalformed = (position.itemSize != null && position.itemSize !== 3) || position.array.length !== position.count * 3 || !meshInspection.positionsValid, indexArrayMalformed = Boolean(index && (!index.array || typeof index.array.length !== 'number' || !Number.isInteger(index.count) || index.count < 0)), indexCountMismatch = Boolean(index?.array && index.count != null && index.count !== index.array.length);
    if (positionMalformed) { malformedAttributeTotal++; issues.push(issue('error', 'topology.positionAttribute', path, 'Position attribute stride or count does not match its backing array.', 'mesh.cleanup')); }
    if (indexArrayMalformed) { malformedAttributeTotal++; issues.push(issue('error', 'topology.indexArray', path, 'Index attribute has no valid numeric backing array or count.', 'mesh.cleanup')); }
    if (indexCountMismatch) { indexCountMismatchTotal++; issues.push(issue('error', 'topology.indexCountMismatch', path, `Index count ${index.count} does not match backing array length ${index.array.length}.`, 'mesh.cleanup')); }
    if (indexArrayMalformed) return;
    const vertexCount = position.count, indexCount = index ? index.count : vertexCount, materialList = Array.isArray(object.material) ? object.material : [object.material], materialIds = materialList.filter(Boolean).map((material) => stableText(material.uuid, stableText(material.name, 'material'))).sort(), transformValues = [...positionValues, ...rotationValues, ...scaleValues], transformSignature = transformValues.length ? (transformValues.every((value) => finite(value)) ? transformValues.map((value) => Number(value)) : null) : [];
    const triangleCount = Math.floor(indexCount / 3);
    const fingerprint = positionMalformed || !meshInspection.indicesValid ? null : geometryFingerprint(position.array, index?.array || null);
    const candidates = fingerprint ? geometryFingerprints.get(fingerprint.key) || [] : [];
    const duplicateSource = fingerprint ? candidates.find((candidate) => fingerprint.canonical ? candidate.canonical && canonicalGeometryEqual(candidate.position, candidate.index, position.array, index?.array || null) : sameArray(candidate.position, position.array) && ((!candidate.index && !index) || (candidate.index && index && sameArray(candidate.index, index.array)))) : null;
    const duplicateOf = duplicateSource?.path || null;
    if (duplicateOf) { duplicateGeometryTotal++; issues.push(issue('info', 'geom.duplicate', path, `Geometry is identical to ${duplicateOf} and may be suitable for instancing.`)); }
    if (fingerprint) { candidates.push({ path, canonical: fingerprint.canonical, position: position.array, index: index?.array || null, materialIds, transformSignature }); geometryFingerprints.set(fingerprint.key, candidates); }
    vertices += vertexCount; triangles += triangleCount;
    gpuBytes += position.array.byteLength + (index?.array.byteLength || 0) + (normal?.array.byteLength || 0) + (uv?.array.byteLength || 0);
    let nonFinite = 0, degenerate = 0, tinyFaces = 0, invalidIndices = 0, meshNormalNonFinite = 0, meshNormalZeroLength = 0, invertedNormals = 0, meshTangentNonFinite = 0, meshUvNonFinite = 0, meshUvOutOfRange = 0, meshUvZeroArea = 0, duplicateIndices = 0, windingConflicts = 0;
    let minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
    for (let i = 0; i + 2 < position.array.length; i += 3) { const x = position.array[i], y = position.array[i + 1], z = position.array[i + 2]; if (finite(x) && finite(y) && finite(z)) { minX = Math.min(minX, x); minY = Math.min(minY, y); minZ = Math.min(minZ, z); maxX = Math.max(maxX, x); maxY = Math.max(maxY, y); maxZ = Math.max(maxZ, z); } }
    const extent = Math.max(maxX - minX, maxY - minY, maxZ - minZ, 0), tinyArea = Math.max(extent * extent * 1e-8, 1e-20);
    const uvTriangles = [];
    for (const value of position.array) if (!finite(value)) nonFinite++;
    if (normal?.array) for (let i = 0; i + 2 < normal.array.length; i += 3) {
      const nx = normal.array[i], ny = normal.array[i + 1], nz = normal.array[i + 2];
      if (![nx, ny, nz].every(finite)) meshNormalNonFinite++;
      else if (nx * nx + ny * ny + nz * nz <= 1e-12) meshNormalZeroLength++;
    }
    if (tangent?.array) for (const value of tangent.array) if (!finite(value)) meshTangentNonFinite++;
    if (uv?.array) {
      uvMeshes++;
      for (const value of uv.array) {
        if (!finite(value)) meshUvNonFinite++;
        else if (value < 0 || value > 1) meshUvOutOfRange++;
      }
      for (let i = 0; i + 1 < uv.array.length; i += 2) {
        const u = uv.array[i], v = uv.array[i + 1];
        if (finite(u) && finite(v)) { uvMinU = Math.min(uvMinU, u); uvMinV = Math.min(uvMinV, v); uvMaxU = Math.max(uvMaxU, u); uvMaxV = Math.max(uvMaxV, v); }
      }
      uvNonFinite += meshUvNonFinite; uvOutOfRange += meshUvOutOfRange;
    }
    forEachMeshTriangle(index?.array || null, vertexCount, (i, ia, ib, ic) => {
      if (ia === ib || ib === ic || ia === ic) duplicateIndices++;
      const a = ia * 3, b = ib * 3, c = ic * 3;
      if (![ia, ib, ic].every(Number.isInteger) || ia < 0 || ib < 0 || ic < 0 || ia >= vertexCount || ib >= vertexCount || ic >= vertexCount || a + 2 >= position.array.length || b + 2 >= position.array.length || c + 2 >= position.array.length) { invalidIndices++; return; }
      if (uv && ia * 2 + 1 < uv.array.length && ib * 2 + 1 < uv.array.length && ic * 2 + 1 < uv.array.length) { const triangleUV = [ia, ib, ic].map((id) => [uv.array[id * 2], uv.array[id * 2 + 1]]); if (triangleUV.flat().every(finite)) uvTriangles.push({ ids: [ia, ib, ic], uv: triangleUV, density: null }); }
      const ax = position.array[b] - position.array[a], ay = position.array[b + 1] - position.array[a + 1], az = position.array[b + 2] - position.array[a + 2];
      const bx = position.array[c] - position.array[a], by = position.array[c + 1] - position.array[a + 1], bz = position.array[c + 2] - position.array[a + 2];
      const cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
      const geometryArea = Math.sqrt(cx * cx + cy * cy + cz * cz) * .5;
      if (geometryArea <= 1e-10) degenerate++;
      else if (geometryArea <= tinyArea) tinyFaces++;
      if (normal && normal.count === vertexCount && geometryArea > 1e-10) { const faceLength = geometryArea * 2, nx = (normal.array[ia * 3] + normal.array[ib * 3] + normal.array[ic * 3]) / 3, ny = (normal.array[ia * 3 + 1] + normal.array[ib * 3 + 1] + normal.array[ic * 3 + 1]) / 3, nz = (normal.array[ia * 3 + 2] + normal.array[ib * 3 + 2] + normal.array[ic * 3 + 2]) / 3; if (finite(nx + ny + nz) && nx * nx + ny * ny + nz * nz > 1e-12 && (cx * nx + cy * ny + cz * nz) / (faceLength * Math.hypot(nx, ny, nz)) < -0.25) invertedNormals++; }
      if (uv) {
        const au = ia * 2, bu = ib * 2, cu = ic * 2;
        if (cu + 1 < uv.array.length && bu + 1 < uv.array.length && au + 1 < uv.array.length) {
          const uvArea = (uv.array[bu] - uv.array[au]) * (uv.array[cu + 1] - uv.array[au + 1]) - (uv.array[bu + 1] - uv.array[au + 1]) * (uv.array[cu] - uv.array[au]);
          if (finite(uvArea) && Math.abs(uvArea) <= 1e-12) meshUvZeroArea++;
          else if (finite(uvArea) && geometryArea > 1e-10) {
            const density = Math.sqrt(Math.abs(uvArea) / geometryArea);
            uvDensityCount++; uvDensitySum += density; uvDensitySquaredSum += density * density; if (uvTriangles.length) uvTriangles.at(-1).density = density;
          }
        }
      }
    });
    const topology = buildIndexedEdgeUses(index?.array || null, { vertexCount, accept: (_face, ia, ib, ic) => [ia, ib, ic].every(Number.isInteger) && ia >= 0 && ib >= 0 && ic >= 0 && ia < vertexCount && ib < vertexCount && ic < vertexCount }), edgeUses = new Map([...topology.edges].map(([key, faces]) => [key.replace(',', ':'), faces])), adjacency = topology.adjacency, usedVertices = topology.usedVertices;
    windingConflicts = topology.windingConflicts;
    const uvParent = uvTriangles.map((_, index) => index), findUV = (index) => { while (uvParent[index] !== index) { uvParent[index] = uvParent[uvParent[index]]; index = uvParent[index]; } return index; }, unionUV = (a, b) => { a = findUV(a); b = findUV(b); if (a !== b) uvParent[b] = a; };
    const sharedUVEdges = new Map();
    uvTriangles.forEach((triangle, faceIndex) => triangle.ids.forEach((left, corner) => { const right = triangle.ids[(corner + 1) % 3], key = left < right ? `${left}:${right}` : `${right}:${left}`, edgeUV = [triangle.uv[corner], triangle.uv[(corner + 1) % 3]]; const other = sharedUVEdges.get(key); if (other && other.edgeUV.every((value, index) => Math.abs(value[0] - edgeUV[index][0]) <= 1e-6 && Math.abs(value[1] - edgeUV[index][1]) <= 1e-6)) unionUV(faceIndex, other.faceIndex); else if (!other) sharedUVEdges.set(key, { faceIndex, edgeUV }); }));
    const meshUvIslands = new Set(uvTriangles.map((_, index) => findUV(index))).size; uvIslandTotal += meshUvIslands;
    const overlapGrid = new Uint16Array(64 * 64);
    for (const { uv: triangleUV } of uvTriangles) {
      const xs = triangleUV.map(([u]) => u * 63), ys = triangleUV.map(([, v]) => (1 - v) * 63), minTX = Math.max(0, Math.floor(Math.min(...xs))), maxTX = Math.min(63, Math.ceil(Math.max(...xs))), minTY = Math.max(0, Math.floor(Math.min(...ys))), maxTY = Math.min(63, Math.ceil(Math.max(...ys))), denominator = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2]);
      if (Math.abs(denominator) <= 1e-12) continue;
      for (let y = minTY; y <= maxTY; y++) for (let x = minTX; x <= maxTX; x++) { const aa = ((ys[1] - ys[2]) * (x + .5 - xs[2]) + (xs[2] - xs[1]) * (y + .5 - ys[2])) / denominator, bb = ((ys[2] - ys[0]) * (x + .5 - xs[2]) + (xs[0] - xs[2]) * (y + .5 - ys[2])) / denominator, cc = 1 - aa - bb; if (aa >= 0 && bb >= 0 && cc >= 0) overlapGrid[y * 64 + x]++; }
    }
    const meshUvOverlap = overlapGrid.filter((count) => count > 1).length; uvOverlapTotal += meshUvOverlap; for (let i = 0; i < uvOverlapGrid.length; i++) uvOverlapGrid[i] = Math.min(65535, uvOverlapGrid[i] + overlapGrid[i]);
    for (const triangle of uvTriangles) { if (!finite(triangle.density)) continue; const u = (triangle.uv[0][0] + triangle.uv[1][0] + triangle.uv[2][0]) / 3, v = (triangle.uv[0][1] + triangle.uv[1][1] + triangle.uv[2][1]) / 3; const x = Math.max(0, Math.min(63, Math.floor(u * 64))), y = Math.max(0, Math.min(63, Math.floor((1 - v) * 64))), cellIndex = y * 64 + x; uvDistortionGridSum[cellIndex] += triangle.density; uvDistortionGridCount[cellIndex]++; }
    if (meshUvOverlap) issues.push(issue('warning', 'uv.overlap', path, `${meshUvOverlap} coarse UV texels contain overlapping faces.`));
    const components = new Uint8Array(vertexCount); let componentCount = 0;
    for (const start of usedVertices) if (!components[start]) { componentCount++; const queue = [start]; components[start] = 1; for (let cursor = 0; cursor < queue.length; cursor++) for (const next of adjacency[queue[cursor]]) if (!components[next]) { components[next] = 1; queue.push(next); } }
    const boundaryEdges = [...edgeUses.values()].filter((entries) => entries.length === 1).length, nonManifoldEdges = [...edgeUses.values()].filter((entries) => entries.length > 2).length, unusedVertices = vertexCount - usedVertices.size;
    duplicateIndexTotal += duplicateIndices; invalidIndexTotal += invalidIndices; unusedVertexTotal += unusedVertices; boundaryEdgeTotal += boundaryEdges; nonManifoldEdgeTotal += nonManifoldEdges; inconsistentWindingTotal += windingConflicts; disconnectedComponentTotal += componentCount > 1 ? componentCount : 0; invertedNormalTotal += invertedNormals;
    if (duplicateIndices) issues.push(issue('warning', 'topology.duplicateIndex', path, `${duplicateIndices} triangles repeat a vertex index.`, 'mesh.cleanup'));
    if (invalidIndices) issues.push(issue('error', 'topology.invalidIndex', path, `${invalidIndices} triangles reference vertices outside the position array.`, 'mesh.cleanup'));
    if (unusedVertices) issues.push(issue('warning', 'topology.unusedVertex', path, `${unusedVertices} vertices are not referenced by any valid triangle.`, 'mesh.cleanup'));
    if (boundaryEdges) issues.push(issue('info', 'topology.boundaryEdge', path, `${boundaryEdges} boundary edges detected.`));
    if (nonManifoldEdges) issues.push(issue('warning', 'topology.nonManifoldEdge', path, `${nonManifoldEdges} edges are shared by more than two triangles.`));
    if (windingConflicts) issues.push(issue('warning', 'topology.inconsistentWinding', path, `${windingConflicts} shared edges have inconsistent winding.`, 'mesh.cleanup'));
    if (componentCount > 1) issues.push(issue('info', 'topology.disconnectedComponent', path, `${componentCount} disconnected mesh components detected.`, 'mesh.splitComponents'));
    const detachedSkinGeometry = skin.present && (!object.skeleton || !Array.isArray(object.skeleton.bones) || !object.skeleton.bones.length);
    if (skin.present) {
      if (detachedSkinGeometry) issues.push(issue('warning', 'rig.detachedGeometry', path, 'Mesh has skin weights but no attached skeleton with bones; deformation cannot be evaluated safely.'));
      if (skin.malformed) issues.push(issue('error', 'rig.skinArray', path, 'Skin index and weight buffers must both contain four values per vertex.'));
      if (skin.unweightedVertices) issues.push(issue('warning', 'rig.unweightedVertex', path, `${skin.unweightedVertices} vertices have no positive skin weight.`));
      if (skin.weightSumMismatches) issues.push(issue('warning', 'rig.weightSum', path, `${skin.weightSumMismatches} vertices have weights that do not sum to one.`));
      if (skin.negativeWeights) issues.push(issue('error', 'rig.negativeWeight', path, `${skin.negativeWeights} skin weights are negative.`));
      if (skin.excessiveInfluences) issues.push(issue('warning', 'rig.excessiveInfluences', path, `${skin.excessiveInfluences} vertices exceed the supported influence count.`));
      if (skin.invalidJointIndices) issues.push(issue('error', 'rig.invalidJoint', path, `${skin.invalidJointIndices} skin influences reference an invalid joint index.`));
      if (skin.nonFiniteValues) issues.push(issue('error', 'rig.nonFinite', path, `${skin.nonFiniteValues} skin indices or weights are non-finite.`));
      if (skin.nonInvertibleBones) issues.push(issue('error', 'rig.nonInvertibleBone', path, `${skin.nonInvertibleBones} bone matrices are non-finite or non-invertible.`));
      if (skin.bindTransforms?.countMismatch) issues.push(issue('error', 'rig.bindCountMismatch', path, `Skeleton has ${skin.skeleton.boneCount} bones but ${skin.bindTransforms.count} inverse-bind matrices.`));
      if (skin.bindTransforms?.nonFinite) issues.push(issue('error', 'rig.nonFiniteBindTransform', path, `${skin.bindTransforms.nonFinite} inverse-bind matrices contain non-finite values.`));
      if (skin.bindTransforms?.nonInvertible) issues.push(issue('error', 'rig.nonInvertibleBindTransform', path, `${skin.bindTransforms.nonInvertible} inverse-bind matrices are non-invertible.`));
      if (skin.bindPose?.present && !skin.bindPose.valid) issues.push(issue('info', 'rig.bindPoseMismatch', path, `Current bone transforms differ from inverse-bind matrices by up to ${skin.bindPose.maxResidual.toExponential(2)}; this may represent an animated pose.`));
      if (skin.skeleton.duplicateNames) issues.push(issue('warning', 'rig.duplicateBoneName', path, `${skin.skeleton.duplicateNames} skeleton bones have duplicate names.`));
      if (skin.skeleton.missingParents) issues.push(issue('error', 'rig.missingBoneParent', path, `${skin.skeleton.missingParents} bones reference a parent outside the skeleton.`));
      if (skin.skeleton.cycles) issues.push(issue('error', 'rig.hierarchyCycle', path, `${skin.skeleton.cycles} skeleton hierarchy cycle(s) detected.`));
      if (skin.skeleton.malformedTransforms) issues.push(issue('error', 'rig.malformedBoneTransform', path, `${skin.skeleton.malformedTransforms} bone transforms do not contain exactly 16 matrix components.`));
      if (skin.skeleton.nonFiniteTransforms) issues.push(issue('error', 'rig.nonFiniteBoneTransform', path, `${skin.skeleton.nonFiniteTransforms} bone transforms contain non-finite values.`));
      if (skin.skeleton.nonInvertibleTransforms) issues.push(issue('error', 'rig.nonInvertibleBoneTransform', path, `${skin.skeleton.nonInvertibleTransforms} bone transforms are non-invertible.`));
    }
    if (nonFinite) issues.push(issue('error', 'geom.nonFinitePosition', path, `${nonFinite} non-finite position components.`));
    if (normal && normal.count !== vertexCount) { normalCountMismatch++; issues.push(issue('error', 'normal.countMismatch', path, `Normal count ${normal.count} does not match position count ${vertexCount}.`, 'mesh.recomputeNormals')); }
    if (meshNormalNonFinite) { normalNonFinite += meshNormalNonFinite; issues.push(issue('error', 'normal.nonFinite', path, `${meshNormalNonFinite} normals contain non-finite components.`, 'mesh.recomputeNormals')); }
    if (meshNormalZeroLength) { normalZeroLength += meshNormalZeroLength; issues.push(issue('warning', 'normal.zeroLength', path, `${meshNormalZeroLength} normals have near-zero length.`, 'mesh.recomputeNormals')); }
    if (invertedNormals) issues.push(issue('warning', 'normal.inverted', path, `${invertedNormals} face normals oppose the authored vertex-normal direction.`, 'mesh.recomputeNormals'));
    if (meshUvNonFinite) issues.push(issue('error', 'uv.nonFinite', path, `${meshUvNonFinite} non-finite UV components.`));
    if (meshUvOutOfRange) issues.push(issue('warning', 'uv.outOfRange', path, `${meshUvOutOfRange} UV components fall outside the 0–1 range.`));
    if (meshUvZeroArea) issues.push(issue('warning', 'uv.zeroAreaFace', path, `${meshUvZeroArea} triangles have zero UV area.`));
    uvZeroArea += meshUvZeroArea;
    if (index && indexCount % 3) issues.push(issue('error', 'geom.triangleIndexCount', path, 'Index count is not divisible by three.'));
    if (degenerate) issues.push(issue('warning', 'geom.zeroAreaFace', path, `${degenerate} zero-area triangles detected.`, 'mesh.cleanup'));
    if (tinyFaces) issues.push(issue('warning', 'geom.tinyFace', path, `${tinyFaces} triangles are tiny relative to the mesh bounds.`));
    if (!normal) issues.push(issue('warning', 'geom.missingNormals', path, 'Mesh has no normal attribute.', 'mesh.recomputeNormals'));
    if (!uv) issues.push(issue('warning', 'geom.missingUV', path, 'Mesh has no UV attribute.', 'mesh.unwrapUV'));
    let needsTangents = false;
    for (const [materialIndex, material] of materialList.entries()) {
      if (!material) continue;
      const materialId = stableText(material.uuid, stableText(material.name, 'material'));
      const materialName = stableText(material.name, materialId);
      const shader = stableText(material.type, material.isMeshPhysicalMaterial ? 'MeshPhysicalMaterial' : material.isMeshStandardMaterial ? 'MeshStandardMaterial' : material.isMeshBasicMaterial ? 'MeshBasicMaterial' : 'UnknownMaterial'), graph = inventoryMaterialGraph(material), fingerprint = materialFingerprint(material), equivalent = materialFingerprints.get(fingerprint), duplicateOf = equivalent && equivalent !== materialId ? equivalent : null, entry = materials.get(materialId) || { id: materialId, name: materialName, shader, paths: [], bindings: [], subsets: [], ...(duplicateOf ? { duplicateOf } : {}), ...(graph.nodes.length ? { nodes: graph.nodes, unsupportedNodes: graph.unsupportedNodes, bakeableNodes: graph.bakeableNodes, unreachableNodes: graph.unreachableNodes, unresolvedReferences: graph.unresolvedReferences } : {}) };
      if (!materialFingerprints.has(fingerprint)) materialFingerprints.set(fingerprint, materialId);
      if (!entry.paths.includes(path)) entry.paths.push(path);
      for (const group of malformedGroups ? [] : geometry.groups || []) if (group.materialIndex === materialIndex && !entry.subsets.some((subset) => subset.path === path && subset.start === group.start && subset.count === group.count)) entry.subsets.push({ path, start: group.start, count: group.count, materialIndex });
      materials.set(materialId, entry);
      if (shader === 'ShaderMaterial' || shader === 'RawShaderMaterial' || shader === 'UnknownMaterial') issues.push(issue('warning', 'material.unsupportedShader', path, `Material ${materialName} uses ${shader}, which cannot be fully audited as a portable PBR graph.`));
      if (duplicateOf) issues.push(issue('info', 'material.duplicate', path, `Material ${materialName} is structurally equivalent to ${duplicateOf} and may be merged after reviewing bindings.`));
      for (const nodeId of graph.unsupportedNodes) issues.push(issue('warning', 'material.unsupportedNode', path, `Material ${materialName} contains unsupported graph node ${nodeId}.`));
      for (const nodeId of graph.unreachableNodes || []) issues.push(issue('info', 'material.unreachableNode', path, `Material ${materialName} contains unreachable graph node ${nodeId}; review before removing it.`));
      for (const reference of graph.unresolvedReferences || []) issues.push(issue('warning', 'material.unresolvedNodeReference', path, `Material ${materialName} contains an unresolved graph reference ${reference}.`));
      for (const [key, value] of Object.entries(material)) {
        if (key === 'normalMap' && value?.isTexture) needsTangents = true;
        if (!value?.isTexture) continue;
        const image = value.image, textureId = stableText(value.uuid, `${image?.width}x${image?.height}`), colorSpace = stableText(value.colorSpace || image?.colorSpace, 'unknown'), normalY = value.normalY || value.userData?.normalY || image?.normalY || null, imagePixels = image?.data || image?.pixels, contentHash = image?.width && image?.height && imagePixels?.length ? `${image.width}x${image.height}:${bytesHash(imagePixels)}` : null, duplicateOf = contentHash ? textureContent.get(contentHash) || null : null;
        const hasDimensions = Number.isSafeInteger(image?.width) && image.width > 0 && Number.isSafeInteger(image?.height) && image.height > 0 && Number.isSafeInteger(image.width * image.height * 4);
        const hasImage = Boolean(image && hasDimensions);
        const expectedPixelCount = hasImage ? image.width * image.height * 4 : null;
        const texture = textures.get(textureId) || { id: textureId, width: hasImage ? image.width : 0, height: hasImage ? image.height : 0, bytes: expectedPixelCount || 0, hasImage, pixelCount: imagePixels?.length ?? null, expectedPixelCount, invalidPixels: false, colorSpace, normalY, ...(duplicateOf ? { duplicateOf } : {}), slots: [] };
        if (!texture.normalY && normalY) texture.normalY = normalY;
        if (contentHash && !textureContent.has(contentHash)) textureContent.set(contentHash, textureId);
        if (duplicateOf && duplicateOf !== textureId && !issues.some((item) => item.ruleId === 'texture.duplicate' && item.path === path && item.message.includes(textureId))) issues.push(issue('info', 'texture.duplicate', path, `Texture ${textureId} is byte-identical to ${duplicateOf}.`));
        if (!texture.slots.includes(key)) texture.slots.push(key);
        textures.set(textureId, texture);
        if (image && !hasDimensions) { invalidTextureDimensions++; issues.push(issue('error', 'texture.invalidDimensions', path, `Texture ${textureId} has invalid image dimensions; width and height must be positive integers.`)); }
        const invalidPixelValues = imagePixels && typeof imagePixels.length === 'number' && hasInvalidValue(imagePixels, (pixel) => !finite(pixel));
        if (hasImage && imagePixels && (imagePixels.length !== expectedPixelCount || invalidPixelValues)) { invalidTexturePixels++; texture.invalidPixels = true; issues.push(issue('error', 'texture.invalidPixels', path, `Texture ${textureId} has ${imagePixels.length} pixel values${invalidPixelValues ? ' including non-finite values' : ''}, expected ${expectedPixelCount} RGBA values.`)); }
        if (hasImage && (image.width > 8192 || image.height > 8192)) issues.push(issue('warning', 'texture.oversized', path, `${key} texture ${textureId} is ${image.width}×${image.height}; Lucia bake processing is bounded to 8192px per axis.`));
        if (!entry.bindings.some((binding) => binding.slot === key && binding.textureId === textureId)) entry.bindings.push({ slot: key, textureId, colorSpace, ...(normalY ? { normalY } : {}) });
        if (!image && !missingTextureIds.has(textureId)) { missingTextureIds.add(textureId); missingTextureImages++; issues.push(issue('warning', 'texture.missingImage', path, `Texture ${textureId} has no loaded image dimensions.`)); }
        if (/^(?:normalMap|roughnessMap|metalnessMap|aoMap|displacementMap|bumpMap|alphaMap)$/.test(key) && String(colorSpace).toLowerCase() === 'srgb') issues.push(issue('warning', 'texture.dataColorSpace', path, `${key} texture ${textureId} is tagged sRGB but should use a linear/data color space.`));
        if (key === 'normalMap' && normalY != null && !['opengl', 'directx'].includes(String(normalY).toLowerCase())) issues.push(issue('warning', 'texture.normalConvention', path, `Normal texture ${textureId} has unsupported Y convention metadata: ${normalY}.`));
      }
    }
    const tangentFix = uv && normal && normal.count === vertexCount ? 'mesh.recomputeTangents' : null;
    if (tangent && tangent.count !== vertexCount) { tangentCountMismatch++; issues.push(issue('error', 'tangent.countMismatch', path, `Tangent count ${tangent.count} does not match position count ${vertexCount}.`, tangentFix)); }
    if (meshTangentNonFinite) { tangentNonFinite += meshTangentNonFinite; issues.push(issue('error', 'tangent.nonFinite', path, `${meshTangentNonFinite} tangent components are non-finite.`, tangentFix)); }
    if (needsTangents && !tangent) { missingTangentTotal++; issues.push(issue('warning', 'tangent.missing', path, 'A normal-map material is present but the mesh has no tangent attribute.', tangentFix)); }
    const bounds = [minX, minY, minZ, maxX, maxY, maxZ].every(finite) ? { min: [minX, minY, minZ], max: [maxX, maxY, maxZ], center: [(minX + maxX) / 2, (minY + maxY) / 2, (minZ + maxZ) / 2], size: [maxX - minX, maxY - minY, maxZ - minZ] } : null;
    const duplicate = duplicateSource ? { materialCompatible: duplicateSource.materialIds ? duplicateSource.materialIds.length === materialIds.length && duplicateSource.materialIds.every((value, index) => value === materialIds[index]) : null, transformCompatible: duplicateSource.transformSignature && transformSignature ? duplicateSource.transformSignature.length === transformSignature.length && duplicateSource.transformSignature.every((value, index) => value === transformSignature[index]) : null } : {};
    meshes.push({ path, vertices: vertexCount, triangles: triangleCount, hasNormals: Boolean(normal), hasTangents: Boolean(tangent), hasUVs: Boolean(uv), degenerate, tinyFaces, nonFinite, invalidIndices, invalidNormals: meshNormalNonFinite + meshNormalZeroLength + invertedNormals, invertedNormals, invalidTangents: meshTangentNonFinite, duplicateOf, ...duplicate, materialIds, transformSignature, unusedVertices, boundaryEdges, nonManifoldEdges, inconsistentWinding: windingConflicts, components: componentCount, bounds, physics: analyzePhysicsMesh({ positions: position.array, indices: index?.array || null }), skin: skin.present ? { ...skin, detachedGeometry: detachedSkinGeometry } : skin, negativeScale, extremeTransform: extremeScale + extremeTranslation, nonFiniteTransform: nonFiniteScale + nonFinitePose, uvIslands: meshUvIslands, uvOverlap: meshUvOverlap, uvNonFinite: meshUvNonFinite, uvOutOfRange: meshUvOutOfRange, uvZeroArea: meshUvZeroArea });
  });
  issues.sort((a, b) => `${a.path}\0${a.ruleId}`.localeCompare(`${b.path}\0${b.ruleId}`));
  const penalty = issues.reduce((sum, item) => sum + (item.severity === 'error' ? 25 : item.severity === 'warning' ? 8 : 0), 0);
  const score = Math.max(0, Math.min(100, 100 - penalty));
  const uvBounds = uvMeshes ? { minU: uvMinU, minV: uvMinV, maxU: uvMaxU, maxV: uvMaxV, area: Math.max(0, (uvMaxU - uvMinU) * (uvMaxV - uvMinV)) } : null;
  const uvDensityMean = uvDensityCount ? uvDensitySum / uvDensityCount : null;
  const uvDensityVariance = uvDensityCount ? Math.max(0, uvDensitySquaredSum / uvDensityCount - uvDensityMean * uvDensityMean) : null;
  const textureScore = Math.max(0, 100 - (missingTextureImages + invalidTextureDimensions + invalidTexturePixels) * 20), materialPenalty = issues.reduce((sum, item) => sum + (item.ruleId === 'material.unsupportedShader' ? 25 : item.ruleId === 'material.unsupportedNode' ? 10 : item.ruleId === 'material.unresolvedNodeReference' ? 8 : item.ruleId === 'texture.dataColorSpace' ? 10 : 0), 0), materialScore = Math.max(0, 100 - materialPenalty), physicsScore = Math.max(0, 100 - invalidIndexTotal * 25 - nonManifoldEdgeTotal * 12 - boundaryEdgeTotal * 2), uvDistortionGrid = Array.from(uvDistortionGridSum, (sum, i) => uvDistortionGridCount[i] && uvDensityMean ? sum / uvDistortionGridCount[i] / uvDensityMean : 0);
  const materialInventory = [...materials.values()].map((item) => ({ ...item, paths: [...item.paths].sort(), bindings: [...item.bindings].sort((a, b) => `${a.slot}:${a.textureId}`.localeCompare(`${b.slot}:${b.textureId}`)), subsets: [...item.subsets].sort((a, b) => `${a.path}:${a.start}`.localeCompare(`${b.path}:${b.start}`)), ...(item.nodes ? { nodes: [...item.nodes], unsupportedNodes: [...new Set(item.unsupportedNodes || [])].sort(), bakeableNodes: [...new Set(item.bakeableNodes || [])].sort(), unreachableNodes: [...new Set(item.unreachableNodes || [])].sort() } : {}) }));
  const shaderTypes = [...new Set(materialInventory.map((item) => item.shader).filter(Boolean))].sort();
  const unsupportedShaderTypes = new Set(['ShaderMaterial', 'RawShaderMaterial', 'UnknownMaterial']);
  const textureInventory = [...textures.values()].map((item) => ({ ...item, slots: [...item.slots].sort() }));
  return { stage: { meshes: meshes.length, triangles, vertices, estimatedGpuBytes: gpuBytes, duplicateGeometries: duplicateGeometryTotal, materialCount: materialInventory.length, shaderTypes, unsupportedShaderCount: shaderTypes.filter((type) => unsupportedShaderTypes.has(type)).length, uvMeshes, uvNonFinite, uvOutOfRange, uvZeroArea, uvOverlap: uvOverlapTotal, uvOverlapGrid: [...uvOverlapGrid], uvDistortionGrid, uvIslands: uvIslandTotal, normalCountMismatch, normalNonFinite, normalZeroLength, invertedNormals: invertedNormalTotal, tangentCountMismatch, tangentNonFinite, missingTangents: missingTangentTotal, duplicateIndices: duplicateIndexTotal, invalidIndices: invalidIndexTotal, unusedVertices: unusedVertexTotal, boundaryEdges: boundaryEdgeTotal, nonManifoldEdges: nonManifoldEdgeTotal, inconsistentWinding: inconsistentWindingTotal, disconnectedComponents: disconnectedComponentTotal, negativeScales: negativeScaleTotal, extremeTransforms: extremeTransformTotal, nonFiniteTransforms: nonFiniteTransformTotal, malformedAttributes: malformedAttributeTotal, indexCountMismatches: indexCountMismatchTotal, uvBounds, uvDensityMean, uvDensityVariance, missingTextureImages, invalidTextureDimensions, invalidTexturePixels }, meshes, materials: materialInventory.sort((a, b) => a.id.localeCompare(b.id)), textures: textureInventory.sort((a, b) => a.id.localeCompare(b.id)), issues, score: { geometry: score, materials: materialScore, textures: textureScore, usd: null, physics: physicsScore } };
}
