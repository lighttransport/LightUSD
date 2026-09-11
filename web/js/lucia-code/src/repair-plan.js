import { validPrimPath } from './utils.js';

const SUPPORTED_FIXES = new Map([
  ['mesh.cleanup', { title: 'Clean mesh topology', benefit: 'Remove degenerate, duplicate, and invalid faces.', destructive: true, cost: 'medium', order: 10, options: { preset: 'game-ready' } }],
  ['mesh.splitComponents', { title: 'Split disconnected parts', benefit: 'Separate disconnected mesh components into editable part siblings.', destructive: true, cost: 'medium', order: 15, options: { minFaces: 1 } }],
  ['mesh.unwrapUV', { title: 'Generate packed UV atlas', benefit: 'Create usable normalized UVs for texturing.', destructive: true, cost: 'high', order: 20, options: { preset: 'balanced', resolution: 1024, padding: 2 } }],
  ['mesh.recomputeNormals', { title: 'Recompute vertex normals', benefit: 'Restore finite, consistently oriented shading normals.', destructive: true, cost: 'medium', order: 30, options: { weighting: 'area', smoothingAngle: 180 } }],
  ['mesh.recomputeTangents', { title: 'Recompute vertex tangents', benefit: 'Restore a tangent frame for normal-map shading.', destructive: true, cost: 'medium', order: 40, options: {} }],
]);

const issueKey = (item) => `${item.fix}:${item.path}`;

// Build a stable proposal only. Execution stays in LuciaOperations so every
// step can be confirmed, made undoable, and re-analyzed independently.
export function proposeRepairPlan(report, { includeDestructive = true } = {}) {
  const candidates = new Map();
  const issues = Array.isArray(report?.issues) ? report.issues : [], meshes = Array.isArray(report?.meshes) ? report.meshes : [];
  for (const item of issues) {
    if (!item || typeof item !== 'object' || typeof item.path !== 'string' || !validPrimPath(item.path)) continue;
    const descriptor = SUPPORTED_FIXES.get(item.fix);
    if (!descriptor || (!includeDestructive && descriptor.destructive)) continue;
    const key = issueKey(item);
    if (!candidates.has(key)) candidates.set(key, { id: key, fix: item.fix, path: item.path, ...descriptor, issueIds: [] });
    if (typeof item.ruleId === 'string' && item.ruleId) candidates.get(key).issueIds.push(item.ruleId);
  }
  const meshStats = new Map(meshes.filter((mesh) => mesh && typeof mesh === 'object').map((mesh) => [mesh.path, mesh]));
  const workMultiplier = { low: 1, medium: 2, high: 4 };
  const plans = [...candidates.values()].map((item) => {
    const mesh = meshStats.get(item.path), triangles = Number.isSafeInteger(mesh?.triangles) && mesh.triangles >= 0 ? mesh.triangles : 0, vertices = Number.isSafeInteger(mesh?.vertices) && mesh.vertices >= 0 ? mesh.vertices : 0;
    return {
    ...item,
    issueIds: [...new Set(item.issueIds)].sort(),
    dependsOn: [],
    estimatedCost: item.cost,
    estimatedWork: Math.min(Number.MAX_SAFE_INTEGER, Math.max(1, Math.ceil((triangles || vertices || 1) * (workMultiplier[item.cost] || 1)))),
  }; });
  const byPath = new Map(plans.map((item) => [item.path, plans.filter((candidate) => candidate.path === item.path)]));
  for (const item of plans) {
    const samePath = byPath.get(item.path) || [];
    if (item.fix === 'mesh.recomputeTangents') {
      item.dependsOn = samePath.filter((candidate) => candidate.fix === 'mesh.unwrapUV' || candidate.fix === 'mesh.recomputeNormals').map((candidate) => candidate.id);
    } else if (item.fix === 'mesh.recomputeNormals') {
      item.dependsOn = samePath.filter((candidate) => candidate.fix === 'mesh.cleanup').map((candidate) => candidate.id);
    } else if (item.fix === 'mesh.splitComponents') {
      item.dependsOn = samePath.filter((candidate) => candidate.fix === 'mesh.cleanup').map((candidate) => candidate.id);
    } else if (item.fix === 'mesh.unwrapUV') {
      item.dependsOn = samePath.filter((candidate) => candidate.fix === 'mesh.cleanup').map((candidate) => candidate.id);
    }
    item.dependsOn.sort();
  }
  plans.sort((a, b) => String(a.path).localeCompare(String(b.path)) || a.order - b.order || a.fix.localeCompare(b.fix));
  return { schemaVersion: 1, operations: plans };
}

export function repairOperationNeeded(report, operation) {
  if (typeof operation?.fix !== 'string' || !operation.fix || typeof operation.path !== 'string' || !validPrimPath(operation.path)) return false;
  // Operations authored outside the analyzer have no issue IDs and remain
  // explicit requests. Analyzer-generated steps can be skipped once all of
  // their triggering issues have disappeared.
  if (!Array.isArray(operation.issueIds) || !operation.issueIds.length) return true;
  const issues = Array.isArray(report?.issues) ? report.issues : [];
  return issues.some((item) => item && typeof item === 'object' && item.path === operation.path && item.fix === operation.fix && operation.issueIds.includes(item.ruleId));
}

export function summarizeRepairPlan(operations = []) {
  const items = (Array.isArray(operations) ? operations : []).filter((operation) => operation && typeof operation === 'object');
  const benefits = [...new Set(items.map((operation) => typeof operation.benefit === 'string' ? operation.benefit : '').filter(Boolean))].sort();
  const dependencies = new Set(items.flatMap((operation) => Array.isArray(operation.dependsOn) ? operation.dependsOn.filter((dependency) => typeof dependency === 'string' && dependency) : []));
  const work = items.reduce((sum, operation) => sum + (Number.isFinite(operation.estimatedWork) && operation.estimatedWork >= 0 ? operation.estimatedWork : 0), 0);
  const destructive = items.filter((operation) => operation.destructive === true).length;
  const costs = [...new Set(items.map((operation) => typeof operation.estimatedCost === 'string' ? operation.estimatedCost : typeof operation.cost === 'string' ? operation.cost : '').filter(Boolean))].sort();
  return `Benefits: ${benefits.length ? benefits.join(' ') : 'No benefit description provided.'} Cost levels: ${costs.length ? costs.join(', ') : 'unspecified'}. Estimated work: ${work.toLocaleString('en-US')} units. Destructive/undoable steps: ${destructive}/${items.length}. Dependencies: ${dependencies.size}.`;
}

// Keep graph construction separate from the portable recipe envelope. This
// lets callers visualize dependencies without making graph metadata part of
// the authored/exported operation format.
export function buildRepairOperationGraph(plan) {
  const seenOperations = new Set(), operations = (Array.isArray(plan?.operations) ? plan.operations : []).filter((operation) => operation && typeof operation === 'object' && typeof operation.id === 'string' && operation.id && typeof operation.path === 'string' && validPrimPath(operation.path) && !seenOperations.has(operation.id) && (seenOperations.add(operation.id), true)), nodes = operations.map((operation) => ({ id: operation.id, kind: 'operation', fix: operation.fix, path: operation.path, title: operation.title, benefit: operation.benefit, destructive: Boolean(operation.destructive), cost: operation.estimatedCost || operation.cost, estimatedWork: operation.estimatedWork })), known = new Set(nodes.map((node) => node.id)), edges = [], edgeKeys = new Set(), issueNodes = new Map();
  for (const operation of operations) {
    for (const ruleId of (Array.isArray(operation.issueIds) ? operation.issueIds : []).filter((value) => typeof value === 'string' && value)) {
      const id = `issue:${ruleId}:${operation.path}`;
      if (!issueNodes.has(id)) issueNodes.set(id, { id, kind: 'issue', ruleId, path: operation.path });
      const edgeKey = `${id}\0${operation.id}\0triggers`; if (!edgeKeys.has(edgeKey)) { edgeKeys.add(edgeKey); edges.push({ from: id, to: operation.id, kind: 'triggers' }); }
    }
    for (const dependency of (Array.isArray(operation.dependsOn) ? operation.dependsOn : []).filter((value) => typeof value === 'string')) if (known.has(dependency)) { const edgeKey = `${dependency}\0${operation.id}\0dependsOn`; if (!edgeKeys.has(edgeKey)) { edgeKeys.add(edgeKey); edges.push({ from: dependency, to: operation.id, kind: 'dependsOn' }); } }
  }
  nodes.push(...issueNodes.values());
  nodes.sort((a, b) => a.id.localeCompare(b.id)); edges.sort((a, b) => `${a.from}\0${a.to}`.localeCompare(`${b.from}\0${b.to}`));
  return { nodes, edges };
}
