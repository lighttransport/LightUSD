export const TARGET_PROFILES = Object.freeze({
  'mobile-ar': Object.freeze({ version: 1, label: 'Mobile AR', maxTriangles: 150000, maxMaterials: 8, maxTextureDimension: 4096, maxGpuBytes: 256 * 1024 * 1024, requireUVs: true, requirePhysicalUnits: true, requireUSDMetadata: true, materialParameterization: Object.freeze({ primvar: true, variant: true }) }),
  'apple-ar': Object.freeze({ version: 1, label: 'Apple AR', maxTriangles: 100000, maxMaterials: 8, maxTextureDimension: 2048, maxGpuBytes: 256 * 1024 * 1024, requireUVs: true, requirePhysicalUnits: true, requireUSDMetadata: true, materialParameterization: Object.freeze({ primvar: true, variant: true }) }),
  'web-viewer': Object.freeze({ version: 1, label: 'Web product viewer', maxTriangles: 500000, maxMaterials: 16, maxTextureDimension: 8192, maxGpuBytes: 512 * 1024 * 1024, requireUVs: true, materialParameterization: Object.freeze({ primvar: true, variant: true }) }),
  'portable-usdz': Object.freeze({ version: 1, label: 'Portable USDZ', maxTriangles: 500000, maxMaterials: 8, maxTextureDimension: 4096, maxGpuBytes: 512 * 1024 * 1024, requireUVs: true, requirePhysicalUnits: true, requireUSDMetadata: true, materialParameterization: Object.freeze({ primvar: true, variant: true }) }),
  'external-reference': Object.freeze({ version: 1, label: 'USDZ with external references', maxTriangles: 500000, maxMaterials: 8, maxTextureDimension: 4096, maxGpuBytes: 512 * 1024 * 1024, requireUVs: true, allowExternalReferences: true, materialParameterization: Object.freeze({ primvar: true, variant: true }) }),
  'desktop-realtime': Object.freeze({ version: 1, label: 'Desktop real-time', maxTriangles: 2000000, maxMaterials: 64, maxTextureDimension: 16384, maxGpuBytes: 2 * 1024 * 1024 * 1024, requireUVs: false, materialParameterization: Object.freeze({ primvar: true, variant: true }) }),
  physics: Object.freeze({ version: 1, label: 'Physics simulation', maxTriangles: 500000, maxMaterials: 16, maxTextureDimension: 4096, maxGpuBytes: 512 * 1024 * 1024, requireUVs: false, materialParameterization: Object.freeze({ primvar: false, variant: false }) }),
  physx: Object.freeze({ version: 1, label: 'PhysX collider', maxTriangles: 100000, maxHullVertices: 64, maxMaterials: 1, maxTextureDimension: null, maxGpuBytes: 256 * 1024 * 1024, requireUVs: false, requireWatertight: true, materialParameterization: Object.freeze({ primvar: false, variant: false }) }),
  bullet: Object.freeze({ version: 1, label: 'Bullet collider', maxTriangles: 100000, maxHullVertices: 64, maxMaterials: 1, maxTextureDimension: null, maxGpuBytes: 256 * 1024 * 1024, requireUVs: false, requireWatertight: true, materialParameterization: Object.freeze({ primvar: false, variant: false }) }),
  jolt: Object.freeze({ version: 1, label: 'Jolt collider', maxTriangles: 100000, maxHullVertices: 64, maxMaterials: 1, maxTextureDimension: null, maxGpuBytes: 256 * 1024 * 1024, requireUVs: false, requireWatertight: true, materialParameterization: Object.freeze({ primvar: false, variant: false }) }),
  'generic-usd': Object.freeze({ version: 1, label: 'Generic USD physics', maxTriangles: 500000, maxHullVertices: 255, maxMaterials: 1, maxTextureDimension: null, maxGpuBytes: 512 * 1024 * 1024, requireUVs: false, requireWatertight: true, materialParameterization: Object.freeze({ primvar: false, variant: false }) }),
  archival: Object.freeze({ version: 1, label: 'Archival USD', maxTriangles: 10000000, maxMaterials: 256, maxTextureDimension: 32768, maxGpuBytes: 8 * 1024 * 1024 * 1024, requireUVs: false, materialParameterization: Object.freeze({ primvar: true, variant: true }) }),
  '3d-printing': Object.freeze({ version: 1, label: '3D printing', maxTriangles: 2000000, maxMaterials: 1, maxTextureDimension: null, maxGpuBytes: 8 * 1024 * 1024 * 1024, requireUVs: false, requireWatertight: true, requirePhysicalUnits: true, materialParameterization: Object.freeze({ primvar: false, variant: false }) }),
});

export const QUALITY_GATE_SCHEMA_VERSION = 1;

const check = (id, label, value, limit, message) => ({ id, label, value, limit, pass: value <= limit, message });
const INVALID_METRIC = Number.MAX_SAFE_INTEGER;
const metric = (value) => value == null ? 0 : Number.isFinite(Number(value)) && Number(value) >= 0 ? Number(value) : INVALID_METRIC;
const countMetric = (value) => value == null ? 0 : Number.isSafeInteger(value) && value >= 0 ? value : INVALID_METRIC;

export function evaluateTargetProfile(report, profile = 'web-viewer', usdReport = null) {
  const key = TARGET_PROFILES[profile] ? profile : 'web-viewer', constraints = TARGET_PROFILES[key], rawStage = report?.stage, stage = rawStage == null ? {} : rawStage, stageValid = rawStage != null && typeof stage === 'object' && !Array.isArray(stage);
  const stageMetric = (value) => stageValid ? metric(value) : INVALID_METRIC;
  const stageCountMetric = (value) => stageValid ? countMetric(value) : INVALID_METRIC;
  const rawMeshes = report?.meshes, reportedMeshCount = stageValid && stage.meshes != null ? Number(stage.meshes) : NaN, meshInventoryValid = Array.isArray(rawMeshes) ? !Number.isFinite(reportedMeshCount) || rawMeshes.length === reportedMeshCount : rawMeshes == null && (!Number.isFinite(reportedMeshCount) || reportedMeshCount === 0), malformedMeshEntries = Array.isArray(rawMeshes) && rawMeshes.some((mesh) => !mesh || typeof mesh !== 'object'), meshes = meshInventoryValid ? rawMeshes || [] : [], meshPaths = meshes.map((mesh) => mesh?.path).filter(Boolean).sort();
  const meshTransformIssues = meshInventoryValid && !malformedMeshEntries ? meshes.reduce((total, mesh) => total + metric(mesh?.nonFiniteTransform) + metric(mesh?.extremeTransform), 0) : INVALID_METRIC;
  const transformIssues = Math.max(stageMetric(stage.nonFiniteTransforms) + stageMetric(stage.extremeTransforms), meshTransformIssues);
  const annotate = (result, affectedPaths, suggestedFixes) => ({ ...result, affectedPaths: [...new Set((affectedPaths || []).filter((path) => typeof path === 'string'))].sort(), suggestedFixes: [...new Set((suggestedFixes || []).filter((fix) => typeof fix === 'string'))].sort() });
  const results = [
    annotate(check('triangles', 'Triangle budget', stageCountMetric(stage.triangles), constraints.maxTriangles, `Triangle count must be ≤ ${constraints.maxTriangles.toLocaleString()}.`), meshPaths, ['mesh.retopo']),
    annotate(check('gpu-memory', 'Estimated GPU memory', stageMetric(stage.estimatedGpuBytes), constraints.maxGpuBytes, `Estimated GPU memory must be ≤ ${(constraints.maxGpuBytes / (1024 * 1024)).toLocaleString()} MiB.`), meshPaths, ['mesh.retopo']),
    annotate({ id: 'transform-stability', label: 'Transform stability', value: transformIssues, limit: 0, pass: transformIssues === 0, message: 'Export transforms must contain finite components and remain within the stable scale/translation range.' }, meshInventoryValid && !malformedMeshEntries ? meshes.filter((mesh) => (mesh?.nonFiniteTransform || 0) > 0 || (mesh?.extremeTransform || 0) > 0).map((mesh) => mesh?.path) : ['/'], []),
  ];
  if (constraints.maxMaterials != null) {
    const materials = report?.materials, reportedMaterialCount = stageValid && stage.materialCount != null ? stageCountMetric(stage.materialCount) : null, malformedEntries = Array.isArray(materials) && materials.some((material) => !material || typeof material !== 'object'), cardinalityMismatch = reportedMaterialCount != null && reportedMaterialCount !== INVALID_METRIC && (materials == null ? reportedMaterialCount !== 0 : !Array.isArray(materials) || materials.length !== reportedMaterialCount), malformedMaterials = malformedEntries || cardinalityMismatch || reportedMaterialCount === INVALID_METRIC, materialPaths = Array.isArray(materials) ? materials.flatMap((material) => Array.isArray(material?.paths) ? material.paths : []) : materials == null ? [] : ['/'], materialValue = malformedMaterials ? constraints.maxMaterials + 1 : materials == null ? reportedMaterialCount ?? 0 : materials.length;
    results.push(annotate(check('materials', 'Material budget', materialValue, constraints.maxMaterials, `Material count must be ≤ ${constraints.maxMaterials.toLocaleString()} and contain valid material entries.`), malformedMaterials ? [...materialPaths, '/'] : materialPaths, []));
  }
  if (constraints.maxTextureDimension != null) {
    let dimensions = 0;
    const malformedTextureEntries = Array.isArray(report?.textures) && report.textures.some((texture) => !texture || typeof texture !== 'object');
    if (report?.textures != null) {
      if (Array.isArray(report.textures)) for (const texture of report.textures) dimensions = Math.max(dimensions, metric(texture?.width), metric(texture?.height));
      else dimensions = INVALID_METRIC;
    }
    if (malformedTextureEntries) dimensions = INVALID_METRIC;
    const affected = Array.isArray(report?.textures) ? report.textures.filter((texture) => Math.max(Number(texture?.width) || 0, Number(texture?.height) || 0) > constraints.maxTextureDimension).map((texture) => texture?.id || '/') : [];
    if (malformedTextureEntries) affected.push('/');
    results.push(annotate(check('texture-dimension', 'Texture dimension', dimensions, constraints.maxTextureDimension, `Texture dimensions must be ≤ ${constraints.maxTextureDimension}px.`), affected, ['texture.resize']));
  }
  const issueEntries = Array.isArray(report?.issues) ? report.issues : report?.issues == null ? [] : [{ path: '/', ruleId: 'texture.invalidDimensions', message: 'Malformed issue collection.' }];
  const malformedTextureIssues = [...new Map(issueEntries.filter((item) => ['texture.invalidDimensions', 'texture.invalidPixels'].includes(item?.ruleId)).map((item) => [`${item.ruleId}\0${item.path}\0${item.message}`, item])).values()];
  const textureEntries = report?.textures == null ? [] : Array.isArray(report.textures) ? report.textures : null;
  if (textureEntries == null) malformedTextureIssues.push({ path: '/', ruleId: 'texture.invalidDimensions' });
  else if (textureEntries.some((texture) => !texture || typeof texture !== 'object')) malformedTextureIssues.push({ path: '/', ruleId: 'texture.invalidDimensions' });
  for (const texture of textureEntries || []) {
    const hasDimensions = texture?.hasImage === true || texture?.width > 0 || texture?.height > 0;
    const dimensionsValid = !hasDimensions || (Number.isInteger(texture.width) && texture.width > 0 && Number.isInteger(texture.height) && texture.height > 0);
    const pixelsValid = texture?.pixelCount == null || texture?.expectedPixelCount == null || texture.pixelCount === texture.expectedPixelCount;
    if (!dimensionsValid || !pixelsValid) malformedTextureIssues.push({ path: texture?.id || '/', ruleId: !dimensionsValid ? 'texture.invalidDimensions' : 'texture.invalidPixels' });
  }
  const uniqueMalformedTextures = [...new Map(malformedTextureIssues.map((item) => [`${item.ruleId}\0${item.path}`, item])).values()];
  results.push(annotate({ id: 'texture-validity', label: 'Texture buffer validity', value: uniqueMalformedTextures.length, limit: 0, pass: uniqueMalformedTextures.length === 0, message: 'Texture images must have valid dimensions and complete RGBA pixel storage.' }, uniqueMalformedTextures.map((item) => item.path), []));
  if (constraints.requireUVs) { const meshCount = stageCountMetric(stage.meshes), uvMeshCount = stageCountMetric(stage.uvMeshes), missingUVMeshes = meshInventoryValid && !malformedMeshEntries ? meshes.filter((mesh) => mesh?.hasUVs !== true) : []; results.push(annotate({ id: 'uvs', label: 'UV coverage', value: uvMeshCount, limit: meshCount, pass: stageValid && meshCount !== INVALID_METRIC && uvMeshCount === meshCount && meshInventoryValid && !malformedMeshEntries && missingUVMeshes.length === 0, message: 'Every mesh must provide UV coordinates and valid mesh inventory entries.' }, meshInventoryValid && !malformedMeshEntries ? missingUVMeshes.map((mesh) => mesh?.path) : ['/'], ['mesh.unwrapUV'])); }
  if (constraints.requireWatertight) {
    const affected = meshInventoryValid ? meshes.filter((mesh) => (mesh?.boundaryEdges || 0) > 0 || (mesh?.nonManifoldEdges || 0) > 0).map((mesh) => mesh?.path) : ['/'];
    const boundaryEdges = stageMetric(stage.boundaryEdges), nonManifoldEdges = stageMetric(stage.nonManifoldEdges);
    results.push(annotate({ id: 'watertight', label: 'Watertight mesh', value: boundaryEdges + nonManifoldEdges, limit: 0, pass: boundaryEdges === 0 && nonManifoldEdges === 0, message: 'Collider meshes must have no boundary or non-manifold edges.' }, affected, ['mesh.cleanup']));
  }
  if (constraints.maxHullVertices != null) {
    const hullCandidates = meshes.filter((mesh) => mesh?.physics?.convexHull?.vertexCount != null), malformedHullMeshes = hullCandidates.filter((mesh) => !Number.isSafeInteger(mesh.physics.convexHull.vertexCount) || mesh.physics.convexHull.vertexCount < 0), hullMeshes = hullCandidates.filter((mesh) => Number.isSafeInteger(mesh.physics.convexHull.vertexCount) && mesh.physics.convexHull.vertexCount >= 0);
    let maxHullVertices = 0;
    for (const mesh of hullMeshes) maxHullVertices = Math.max(maxHullVertices, mesh.physics.convexHull.vertexCount);
    const affected = [...malformedHullMeshes, ...hullMeshes.filter((mesh) => mesh.physics.convexHull.vertexCount > constraints.maxHullVertices)].map((mesh) => mesh.path), hullValue = malformedHullMeshes.length ? constraints.maxHullVertices + 1 : maxHullVertices;
    results.push(annotate({ id: 'hull-vertices', label: 'Convex hull vertex budget', value: hullValue, limit: constraints.maxHullVertices, pass: hullValue <= constraints.maxHullVertices, message: `Convex hulls must contain ≤ ${constraints.maxHullVertices} non-negative integer vertices.` }, affected, ['scene.generate_convex_hull']));
    if (['physx', 'bullet', 'jolt'].includes(key)) {
      const dynamicTriangleColliders = meshes.filter((mesh) => /_TriangleCollider$/.test(String(mesh?.path || ''))).map((mesh) => mesh?.path);
      results.push(annotate({ id: 'dynamic-concave-collider', label: 'Dynamic collider shape', value: dynamicTriangleColliders.length, limit: 0, pass: dynamicTriangleColliders.length === 0, message: 'Dynamic-body profiles cannot use reduced concave triangle colliders; generate a convex hull instead.' }, dynamicTriangleColliders, ['scene.generate_convex_hull']));
      const approximateHulls = hullMeshes.filter((mesh) => mesh.physics.convexHull.approximate).map((mesh) => mesh.path);
      results.push(annotate({ id: 'hull-cooking-readiness', label: 'Exact hull cooking data', value: approximateHulls.length, limit: 0, pass: approximateHulls.length === 0, message: 'Engine cooking requires exact convex-hull metrics; review or author an exact guide hull for sampled meshes.' }, approximateHulls, ['scene.generate_convex_hull']));
    }
  }
  if (constraints.requirePhysicalUnits) {
    const metersPerUnit = Number(usdReport?.metersPerUnit), pass = Number.isFinite(metersPerUnit) && metersPerUnit > 0;
    results.push(annotate({ id: 'physical-units', label: 'Physical units', value: pass ? metersPerUnit : 0, limit: null, pass, message: usdReport ? 'Stage must declare a positive metersPerUnit value for physical-scale output.' : 'Physical-scale output requires a USD Doctor report before export.' }, ['/'], ['scene.repair_usd_metadata']));
  }
  if (constraints.requireUSDMetadata) {
    const hasDefaultPrim = Boolean(usdReport && typeof usdReport.defaultPrim === 'string' && usdReport.defaultPrim && (!Array.isArray(usdReport.rootPrims) || usdReport.rootPrims.includes(usdReport.defaultPrim))), hasUpAxis = Boolean(usdReport && ['Y', 'Z'].includes(usdReport.upAxis)), missing = Number(!hasDefaultPrim) + Number(!hasUpAxis);
    results.push(annotate({ id: 'usd-metadata', label: 'Portable USD metadata', value: missing, limit: 0, pass: missing === 0, message: usdReport ? 'Portable output requires a valid defaultPrim and Y/Z upAxis.' : 'Portable output requires a USD Doctor report before export.' }, ['/'], ['scene.repair_usd_metadata']));
  }
  if (key === 'portable-usdz' && !constraints.allowExternalReferences) {
    const usdIssues = Array.isArray(usdReport?.issues) ? usdReport.issues : usdReport?.issues == null ? [] : [{ path: '/', severity: 'error' }], usdDependencies = Array.isArray(usdReport?.dependencies) ? usdReport.dependencies : usdReport?.dependencies == null ? [] : [{ path: '/', status: 'missing', present: false }], usdFailures = usdIssues.filter((item) => item?.severity === 'error'), missing = usdDependencies.filter((item) => item?.status ? item.status !== 'available' : !item?.present), affected = usdReport ? [...usdFailures.map((item) => item?.path || '/'), ...missing.map((item) => item?.path || '/')].filter((path) => typeof path === 'string') : ['/'];
    results.push(annotate({ id: 'usd-portability', label: 'USDZ portability', value: affected.length, limit: 0, pass: affected.length === 0, message: usdReport ? 'Portable USDZ requires no unresolved dependencies or USD Doctor errors.' : 'Portable USDZ requires a USD Doctor report before export.' }, affected, ['usd.doctor']));
  }
  return { schemaVersion: QUALITY_GATE_SCHEMA_VERSION, profile: key, profileVersion: constraints.version, label: constraints.label, pass: results.every((result) => result.pass), checks: results };
}

// Keep the serialized quality-gate envelope independent from the UI. This is
// used by export manifests and can be validated when a recipe or report is
// loaded from an untrusted project file.
export function createQualityGate(report, { profile = 'web-viewer', usdReport = null, blocking = false } = {}) {
  return { ...evaluateTargetProfile(report, profile, usdReport), blocking: Boolean(blocking) };
}

function expectedQualityCheckIds(profile) {
  const constraints = TARGET_PROFILES[profile];
  if (!constraints) return [];
  const ids = ['triangles', 'gpu-memory', 'transform-stability'];
  if (constraints.maxMaterials != null) ids.push('materials');
  if (constraints.maxTextureDimension != null) ids.push('texture-dimension');
  ids.push('texture-validity');
  if (constraints.requireUVs) ids.push('uvs');
  if (constraints.requireWatertight) ids.push('watertight');
  if (constraints.maxHullVertices != null) {
    ids.push('hull-vertices');
    if (['physx', 'bullet', 'jolt'].includes(profile)) ids.push('dynamic-concave-collider', 'hull-cooking-readiness');
  }
  if (constraints.requirePhysicalUnits) ids.push('physical-units');
  if (constraints.requireUSDMetadata) ids.push('usd-metadata');
  if (profile === 'portable-usdz' && !constraints.allowExternalReferences) ids.push('usd-portability');
  return ids;
}

export function validateQualityGate(gate) {
  if (!gate || gate.schemaVersion !== QUALITY_GATE_SCHEMA_VERSION || typeof gate.profile !== 'string' || !TARGET_PROFILES[gate.profile] || gate.label !== TARGET_PROFILES[gate.profile].label || !Number.isInteger(gate.profileVersion) || gate.profileVersion !== TARGET_PROFILES[gate.profile].version || typeof gate.pass !== 'boolean' || !Array.isArray(gate.checks) || typeof gate.blocking !== 'boolean') return false;
  const isSortedUniqueStrings = (values) => Array.isArray(values) && values.every((value, index) => typeof value === 'string' && (index === 0 || values[index - 1] < value));
  const expectedIds = expectedQualityCheckIds(gate.profile), ids = new Set(), valueDerived = new Set(expectedIds.filter((id) => id !== 'uvs')), validChecks = gate.checks.length === expectedIds.length && gate.checks.every((checkResult, index) => checkResult && checkResult.id === expectedIds[index] && !ids.has(checkResult.id) && (ids.add(checkResult.id), typeof checkResult.label === 'string') && typeof checkResult.pass === 'boolean' && Number.isFinite(checkResult.value) && checkResult.value >= 0 && (checkResult.limit == null || Number.isFinite(checkResult.limit) && checkResult.limit >= 0) && (checkResult.limit == null || !valueDerived.has(checkResult.id) || checkResult.pass === checkResult.value <= checkResult.limit) && typeof checkResult.message === 'string' && isSortedUniqueStrings(checkResult.affectedPaths) && isSortedUniqueStrings(checkResult.suggestedFixes));
  return validChecks && gate.pass === gate.checks.every((checkResult) => checkResult.pass);
}
