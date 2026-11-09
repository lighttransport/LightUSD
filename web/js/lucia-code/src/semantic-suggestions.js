import { validPrimPath } from './utils.js';

// Read-only, deterministic semantic hints. These never author, rename, or
// restructure a USD prim without an explicit user decision.
const ROLES = Object.freeze([
  ['headlight', 'lighting', 0.94], ['headlights', 'lighting', 0.94], ['taillight', 'lighting', 0.94], ['taillights', 'lighting', 0.94],
  ['wheel', 'wheel', 0.92], ['wheels', 'wheel', 0.92], ['tire', 'wheel', 0.92], ['tires', 'wheel', 0.92],
  ['door', 'door', 0.9], ['doors', 'door', 0.9], ['handle', 'handle', 0.9], ['handles', 'handle', 0.9],
  ['fastener', 'fastener', 0.9], ['fasteners', 'fastener', 0.9], ['bolt', 'fastener', 0.9], ['bolts', 'fastener', 0.9], ['screw', 'fastener', 0.9], ['screws', 'fastener', 0.9],
  ['ground', 'ground-contact', 0.88], ['floor', 'ground-contact', 0.88],
  ['chassis', 'chassis', 0.86], ['body', 'body', 0.82], ['window', 'glazing', 0.86],
  ['glass', 'glazing', 0.84], ['mirror', 'mirror', 0.86], ['seat', 'seat', 0.84],
]);

function tokens(path) {
  return String(path || '').split('/').at(-1).replace(/([a-z])([A-Z])/g, '$1_$2').split(/[^A-Za-z0-9]+/).map((token) => token.toLowerCase()).filter(Boolean);
}
const finiteBoundsVector = (value) => Array.isArray(value) && value.length === 3 && value.every((component) => Number.isFinite(component)) ? value : null;
const safeHierarchyPath = (value) => typeof value === 'string' && /^\/(?:[A-Za-z_][A-Za-z0-9_-]*)(?:\/(?:[A-Za-z_][A-Za-z0-9_-]*))*$/.test(value) && !/[\u0000-\u001f\u007f]/.test(value) ? value : '/';
const safeSuggestionText = (value, maximum = 240) => typeof value === 'string' && value.length > 0 && value.length <= maximum && !/[\u0000-\u001f\u007f]/.test(value) ? value : null;
const safeSuggestionRole = (value) => typeof value === 'string' && /^[a-z][a-z0-9-]{0,47}$/.test(value) ? value : null;
const safeSuggestedName = (value) => typeof value === 'string' && /^[A-Za-z_][A-Za-z0-9_]{0,127}$/.test(value) ? value : null;

export function uniqueComponentName(baseName, id, reservedNames = new Set()) {
  const reserved = reservedNames instanceof Set ? reservedNames : new Set(reservedNames);
  const base = String(baseName), safeId = Number.isSafeInteger(id) && id >= 0 ? id : 0;
  if (!reserved.has(base)) return base;
  let name = `${base}_${safeId}`, suffix = 2;
  while (reserved.has(name)) name = `${base}_${safeId}_${suffix++}`;
  return name;
}

export function proposeComponentNames(sourcePath, preview, reservedNames = []) {
  const authoredSourcePath = safeHierarchyPath(sourcePath), sourceLeaf = String(sourcePath || '').split('/').at(-1), source = sourceLeaf.replace(/[^A-Za-z0-9_]/g, '_').replace(/^[^A-Za-z_]+/, '') || 'Mesh', sourceTokens = new Set(tokens(sourcePath)), sourceRole = ROLES.find(([token]) => sourceTokens.has(token) && !['body', 'chassis'].includes(token));
  if (!preview || !Array.isArray(preview.components)) return [];
  const reserved = new Set((reservedNames instanceof Set ? [...reservedNames] : Array.isArray(reservedNames) ? reservedNames : []).filter((name) => typeof name === 'string'));
  return preview.components.map((component, index) => {
    const id = Number.isSafeInteger(component?.id) && component.id >= 0 ? component.id : index;
    const geometryEvidence = { boundaryEdges: Number.isSafeInteger(component?.boundaryEdges) && component.boundaryEdges >= 0 ? component.boundaryEdges : null, nonManifoldEdges: Number.isSafeInteger(component?.nonManifoldEdges) && component.nonManifoldEdges >= 0 ? component.nonManifoldEdges : null, boundaryLoops: Number.isSafeInteger(component?.boundaryLoops) && component.boundaryLoops >= 0 ? component.boundaryLoops : null, planarBoundaryLoops: Number.isSafeInteger(component?.planarBoundaryLoops) && component.planarBoundaryLoops >= 0 ? component.planarBoundaryLoops : null, normalDeviationDegrees: Number.isFinite(component?.normalDeviationDegrees) && component.normalDeviationDegrees >= 0 ? component.normalDeviationDegrees : null, maxDihedralDegrees: Number.isFinite(component?.maxDihedralDegrees) && component.maxDihedralDegrees >= 0 ? component.maxDihedralDegrees : null, curvatureEdges: Number.isSafeInteger(component?.curvatureEdges) && component.curvatureEdges >= 0 ? component.curvatureEdges : null, concavityReliable: typeof component?.concavityReliable === 'boolean' ? component.concavityReliable : null, concaveEdges: Number.isSafeInteger(component?.concaveEdges) && component.concaveEdges >= 0 ? component.concaveEdges : null, convexEdges: Number.isSafeInteger(component?.convexEdges) && component.convexEdges >= 0 ? component.convexEdges : null, nearlyPlanar: typeof component?.nearlyPlanar === 'boolean' ? component.nearlyPlanar : null };
    const geometryRole = !sourceRole && !['mesh', 'geometry'].includes(source.toLowerCase()) && geometryEvidence.nearlyPlanar === true && Number.isSafeInteger(geometryEvidence.planarBoundaryLoops) && geometryEvidence.planarBoundaryLoops > 0 ? ['planar boundary', 'panel', .64] : null, namingRole = sourceRole || geometryRole, rolePrefix = namingRole ? namingRole[1].split('-').map((part) => part[0].toUpperCase() + part.slice(1)).join('') : source;
    const baseName = `${rolePrefix}_Part${index + 1}`, name = uniqueComponentName(baseName, id, reserved);
    reserved.add(name);
    return { id, name, sourceFaces: Array.isArray(component?.faces) ? [...component.faces] : [], geometryEvidence, hierarchy: { mode: 'child', parentPath: authoredSourcePath, rationale: 'Keep the generated component below its source mesh so placement and material review remain local and undoable.' }, confidence: sourceRole ? Number(Math.min(.88, .68 + sourceRole[2] * .2).toFixed(3)) : geometryRole ? geometryRole[2] : .5, rationale: sourceRole ? `Source mesh name contains the role token “${sourceRole[0]}”; numbered parts preserve deterministic ordering and require review.` : geometryRole ? 'Nearly planar geometry with a planar boundary loop suggests a panel-like part; review before authoring a semantic name.' : 'Stable generated component label; review the supplied boundary and planarity evidence before authoring a semantic name.', deterministic: true, authoringRequired: true };
  });
}

export function proposeSemanticSuggestions(report, options = {}) {
  const suggestions = [];
  const meshes = Array.isArray(report?.meshes) ? report.meshes.filter((mesh) => mesh && typeof mesh === 'object' && !Array.isArray(mesh) && validPrimPath(mesh.path)) : [], meshesByPath = new Map(meshes.map((mesh) => [mesh.path, mesh]));
  for (const mesh of [...meshes].sort((a, b) => String(a.path).localeCompare(String(b.path)))) {
    const meshTokens = new Set(tokens(mesh.path)), matches = ROLES.filter(([token]) => meshTokens.has(token));
    if (matches.length) {
      const [token, role, confidence] = matches[0], repeated = Number.isInteger(mesh.components) && mesh.components > 1;
      suggestions.push({ path: mesh.path, kind: 'semantic-role', role, label: role.replace('-', ' '), confidence: repeated ? Math.max(0, confidence - 0.04) : confidence, rationale: `Mesh name contains the role token “${token}”${repeated ? ' and has multiple disconnected components' : ''}.`, hierarchy: { mode: 'child', parentPath: mesh.path, rationale: 'Keep any reviewed semantic role authoring below the existing mesh path until hierarchy changes are explicitly approved.' }, deterministic: true, authoringRequired: true });
    }
    if (typeof mesh.duplicateOf === 'string' && mesh.duplicateOf.startsWith('/')) { const source = meshesByPath.get(mesh.duplicateOf), materialCompatible = source ? mesh.materialCompatible !== false : null, transformCompatible = source ? mesh.transformCompatible !== false : null; suggestions.push({ path: mesh.path, kind: 'instancing-candidate', role: 'instance', label: 'instance candidate', confidence: materialCompatible === false ? 0.62 : 0.8, rationale: `Geometry matches ${mesh.duplicateOf}; ${materialCompatible === false ? 'material bindings differ, so review replacement carefully. ' : ''}${transformCompatible === false ? 'local transforms differ and must be preserved. ' : ''}Review transforms and material bindings before replacing it with an instance.`, deterministic: true, authoringRequired: true, sourcePath: mesh.duplicateOf, materialCompatible, transformCompatible }); }
    const bounds = mesh.bounds, size = finiteBoundsVector(bounds?.size), center = finiteBoundsVector(bounds?.center), diagonal = size ? Math.hypot(...size) : 0, centerOffset = center ? Math.hypot(...center) : 0;
    if (size && center && diagonal > 0 && centerOffset > diagonal * 0.05) suggestions.push({ path: mesh.path, kind: 'pivot-candidate', role: 'pivot', label: 'off-center pivot candidate', confidence: 0.74, rationale: `The local bounds center is ${centerOffset.toPrecision(3)} units from the mesh origin; review a centered pivot before authoring transforms.`, pivot: center.map((value) => Number(value.toPrecision(6))), deterministic: true, authoringRequired: true });
    if (size && diagonal > 0) {
      const axes = ['X', 'Y', 'Z'], dominant = size.indexOf(Math.max(...size)), minor = Math.max(1e-12, Math.min(...size.filter((value) => value > 0)) || 1e-12), anisotropy = size[dominant] / minor;
      if (anisotropy >= 2) suggestions.push({ path: mesh.path, kind: 'orientation-candidate', role: 'orientation', label: `dominant ${axes[dominant]} orientation candidate`, confidence: Math.min(0.86, 0.58 + Math.min(0.28, (anisotropy - 2) * 0.04)), rationale: `The local bounds are ${anisotropy.toPrecision(3)}× longer along ${axes[dominant]} than the shortest non-zero axis; review orientation before authoring it.`, axis: axes[dominant], deterministic: true, authoringRequired: true });
    }
    if (Number.isInteger(mesh.components) && mesh.components > 1) suggestions.push({ path: mesh.path, kind: 'grouping-candidate', role: 'group', label: 'disconnected-component grouping candidate', confidence: 0.76, rationale: `The mesh contains ${mesh.components} disconnected components; review whether they should become stable child prims.`, componentCount: mesh.components, hierarchy: { mode: 'child', parentPath: mesh.path, rationale: 'Keep generated component prims below their source mesh so placement and material review remain local and undoable.' }, deterministic: true, authoringRequired: true });
  }
  if (options?.approvedInference === true && Array.isArray(options.inference)) {
    const seen = new Set(suggestions.map((item) => `${item.path}:${item.kind}`));
    for (const candidate of options.inference) {
      if (!candidate || typeof candidate !== 'object' || Array.isArray(candidate)) continue;
      const path = candidate.path, kind = candidate.kind === 'semantic-role' || candidate.kind === 'name-candidate' ? candidate.kind : null, role = safeSuggestionRole(candidate.role), confidence = candidate.confidence, label = safeSuggestionText(candidate.label, 96), rationale = safeSuggestionText(candidate.rationale), source = candidate.source === 'image' || candidate.source === 'model' ? candidate.source : null;
      if (!validPrimPath(path) || !kind || !role || typeof confidence !== 'number' || !Number.isFinite(confidence) || confidence < 0 || confidence > 1 || !label || !rationale || !source) continue;
      const key = `${path}:${kind}`;
      if (seen.has(key)) continue;
      const hasHierarchy = candidate.hierarchy != null, hierarchy = hasHierarchy && candidate.hierarchy && typeof candidate.hierarchy === 'object' && !Array.isArray(candidate.hierarchy) && (candidate.hierarchy.mode === 'child' || candidate.hierarchy.mode === 'sibling') && validPrimPath(candidate.hierarchy.parentPath) && safeSuggestionText(candidate.hierarchy.rationale) ? { mode: candidate.hierarchy.mode, parentPath: candidate.hierarchy.parentPath, rationale: candidate.hierarchy.rationale } : undefined;
      const hasSuggestedName = candidate.suggestedName != null, suggestedName = safeSuggestedName(candidate.suggestedName);
      if ((hasHierarchy && !hierarchy) || (hasSuggestedName && !suggestedName)) continue;
      suggestions.push({ path, kind, role, label, confidence, rationale, ...(suggestedName ? { suggestedName } : {}), ...(hierarchy ? { hierarchy } : {}), deterministic: false, authoringRequired: true, inferenceApproved: true, inferenceSource: source });
      seen.add(key);
    }
  }
  return suggestions;
}
