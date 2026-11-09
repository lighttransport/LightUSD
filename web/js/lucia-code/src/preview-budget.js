const MiB = 1024 * 1024;

export function previewTextureScale({ width = 0, height = 0, maxDimension = Infinity, maxBytes = Infinity, totalBytes = 0 } = {}) {
  const safeWidth = Math.max(0, Number(width) || 0), safeHeight = Math.max(0, Number(height) || 0), largest = Math.max(safeWidth, safeHeight);
  if (!largest) return 1;
  const aggregateScale = totalBytes > maxBytes && maxBytes > 0 ? Math.sqrt(maxBytes / totalBytes) : 1;
  return Math.min(1, Number(maxDimension) > 0 ? Number(maxDimension) / largest : 1, Number.isFinite(aggregateScale) ? aggregateScale : 1);
}

export function previewTriangleBudget({ maxTriangles = 100000, geometrySampleRatio = 1 } = {}) {
  const safeMax = Math.max(1000, Math.min(100000, Math.floor(Number(maxTriangles) || 100000))), ratio = Number.isFinite(Number(geometrySampleRatio)) ? Math.max(0.1, Math.min(1, Number(geometrySampleRatio))) : 1;
  return Math.max(1000, Math.floor(safeMax * ratio));
}

export function previewGeometryDrawCount({ indexCount = 0, vertexCount = 0, sampleRatio = 1 } = {}) {
  const count = Number.isFinite(Number(indexCount)) && Number(indexCount) > 0 ? Math.floor(Number(indexCount) / 3) * 3 : Math.floor(Math.max(0, Number(vertexCount) || 0) / 3) * 3;
  const ratio = Number.isFinite(Number(sampleRatio)) ? Math.max(.1, Math.min(1, Number(sampleRatio))) : 1;
  if (!count) return 0;
  return Math.max(3, Math.floor(count * ratio / 3) * 3);
}

// Preview limits are deliberately independent from export/quality-gate limits.
// They protect interactive rendering while leaving authored scene data intact.
export function choosePreviewBudget({ triangles = 0, meshes = 0, textureBytes = 0 } = {}) {
  const safeTriangles = Number.isFinite(triangles) ? Math.max(0, triangles) : 0;
  const safeMeshes = Number.isFinite(meshes) ? Math.max(0, meshes) : 0;
  const safeTextureBytes = Number.isFinite(textureBytes) ? Math.max(0, textureBytes) : 0;
  const estimatedBytes = safeTriangles * 96 + safeMeshes * 4096 + safeTextureBytes;
  if (estimatedBytes > 2048 * MiB || safeTriangles > 5000000) return { pixelRatio: 0.75, maxTextureDimension: 2048, maxTextureBytes: 64 * MiB, geometrySampleRatio: 0.5, estimatedBytes, reason: 'Very large scene: reduced viewport resolution; preview textures are capped by dimension and total bytes.' };
  if (estimatedBytes > 768 * MiB || safeTriangles > 2000000) return { pixelRatio: 1, maxTextureDimension: 4096, maxTextureBytes: 128 * MiB, geometrySampleRatio: 0.75, estimatedBytes, reason: 'Large scene: capped viewport resolution; preview textures are capped by dimension and total bytes.' };
  if (estimatedBytes > 256 * MiB || safeTriangles > 750000) return { pixelRatio: Math.min(1.5, globalThis.devicePixelRatio || 1), maxTextureDimension: 4096, maxTextureBytes: 256 * MiB, geometrySampleRatio: 1, estimatedBytes, reason: 'Dense scene: capped viewport pixel ratio and preview texture bytes for responsiveness.' };
  return { pixelRatio: Math.min(2, globalThis.devicePixelRatio || 1), maxTextureDimension: 8192, maxTextureBytes: 512 * MiB, geometrySampleRatio: 1, estimatedBytes, reason: null };
}
