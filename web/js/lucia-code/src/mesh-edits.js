import { numericArrayKind } from './indexed-mesh.js';

// Input is the validated indexed-mesh interchange. Keep value buffers intact
// until the typed layer API copies them; no number-to-text round trip.
export function meshAttributeEdits(path, data, normals, staleNames = [], subsetPaths = []) {
  const edits = [];
  const set = (attr_name, type, value, metadata = {}, uniform = false, primPath = path) =>
    edits.push({ args: { path: primPath, attr_name, value: { type, value }, metadata, uniform } });
  const remove = attr_name => edits.push({ args: { path, attr_name, remove: true } });
  const primvar = (name, type, values, interpolation = 'vertex', indices = null, elementSize = null) => {
    set(`primvars:${name}`, type, values, { interpolation, ...(elementSize ? { elementSize } : {}) });
    if (indices) set(`primvars:${name}:indices`, 'int[]', Array.from(indices));
    else remove(`primvars:${name}:indices`);
  };
  set('points', 'point3f[]', data.positions);
  set('faceVertexIndices', 'int[]', Array.from(data.indices));
  set('faceVertexCounts', 'int[]', new Int32Array(data.indices.length / 3).fill(3));
  if (data.subdivisionScheme) set('subdivisionScheme', 'token', data.subdivisionScheme, {}, true);
  for (const name of ['interpolateBoundary', 'faceVaryingLinearInterpolation', 'triangleSubdivisionRule'])
    if (data[name]) set(name, 'token', data[name], {}, true);
  const chains = data.sharpChains || data.sharpEdges;
  if (chains) {
    set('creaseIndices', 'int[]', chains.flat());
    set('creaseLengths', 'int[]', chains.map(chain => chain.length));
    set('creaseSharpness', 'float[]', data.sharpChainSharpness || data.sharpEdgeSharpness || chains.map(() => 1));
  }
  if (data.uvs) primvar(data.uvSet === 'lightmap' ? 'st1' : 'st', 'texCoord2f[]', data.uvs,
    data.uvIndices ? 'faceVarying' : 'vertex', data.uvIndices);
  if (normals) {
    set('normals', 'normal3f[]', normals, { interpolation: data.normalIndices ? 'faceVarying' : 'vertex' });
    if (data.normalIndices) set('normals:indices', 'int[]', Array.from(data.normalIndices));
    else remove('normals:indices');
  }
  if (data.colors) primvar('displayColor', 'color3f[]', data.colors);
  if (data.jointIndices) primvar('skel:jointIndices', 'int[]', Array.from(data.jointIndices), 'vertex', null, 4);
  if (data.jointWeights) primvar('skel:jointWeights', 'float[]', data.jointWeights, 'vertex', null, 4);
  if (data.tangents) primvar('tangents', 'float4[]', data.tangents);
  else remove('primvars:tangents');
  if (data.clearMissingAttributes) {
    for (const [key, names] of [['uvs', ['primvars:st', 'primvars:st1']], ['normals', ['normals']],
      ['colors', ['primvars:displayColor']], ['jointIndices', ['primvars:skel:jointIndices']],
      ['jointWeights', ['primvars:skel:jointWeights']]]) {
      if (!data[key]) for (const name of names) { remove(name); remove(`${name}:indices`); }
    }
    const retained = new Set([...(data.customAttributes || []), ...(data.faceVaryingAttributes || [])].map(a => a.name));
    for (const name of staleNames) if (!retained.has(name) && !['st', 'st1'].includes(name)) {
      remove(`primvars:${name}`); remove(`primvars:${name}:indices`);
    }
  }
  for (const attribute of [...(data.customAttributes || []), ...(data.faceVaryingAttributes || [])]) {
    const kind = numericArrayKind(attribute.array);
    // Integer values must be checked by the ABI before narrowing, not wrapped
    // by an Int32Array conversion. Float64 input keeps its authored precision.
    const scalar = kind === 'float' && attribute.array instanceof Float64Array ? 'double' : kind;
    const type = attribute.name === 'st1' ? 'texCoord2f[]' :
      `${scalar}${attribute.itemSize === 1 ? '' : attribute.itemSize}[]`;
    const values = kind === 'int' ? Array.from(attribute.array) : attribute.array;
    primvar(attribute.name, type, values, attribute.interpolation, attribute.indices);
  }
  for (let subset = 0; subset < subsetPaths.length; subset++) {
    const faces = [];
    for (const group of data.groups || []) if (group.materialIndex === subset)
      for (let face = group.start / 3; face < (group.start + group.count) / 3; face++) faces.push(face);
    set('indices', 'int[]', faces, {}, false, subsetPaths[subset]);
  }
  return edits;
}
