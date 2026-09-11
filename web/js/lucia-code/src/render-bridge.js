import * as THREE from 'three';

export function materialSnapshotsDiffer(before, after) {
  if (!before || !after || before.path !== after.path || before.triangleCount !== after.triangleCount) return false;
  return before.triangles.some((signature, index) => signature !== after.triangles[index]);
}
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { LightUSDLoaderUtils } from '../../src/lightusd/LightUSDLoaderUtils.js';
import { choosePreviewBudget, previewGeometryDrawCount, previewTextureScale } from './preview-budget.js';

export class LuciaRenderBridge extends EventTarget {
  constructor(container) {
    super(); this.container = container; this.pathObjects = new Map(); this.content = null; this.selection = null;
    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color(0x15151e);
    this.camera = new THREE.PerspectiveCamera(45, 1, .01, 10000);
    this.camera.position.set(7, 5, 8);
    this.renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: 'high-performance' });
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.localClippingEnabled = true;
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = 1.1;
    this.renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
    this.renderer.domElement.tabIndex = 0;
    this.renderer.domElement.setAttribute('aria-label', 'Lucia 3D viewport');
    container.appendChild(this.renderer.domElement);
    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enableDamping = true;
    this.scene.add(new THREE.HemisphereLight(0xc8d2ff, 0x282238, 1.35));
    const key = new THREE.DirectionalLight(0xffead7, 2.2); key.position.set(5, 8, 5); this.scene.add(key);
    this.grid = new THREE.GridHelper(40, 40, 0x605784, 0x2c2c38); this.scene.add(this.grid);
    this.axes = new THREE.AxesHelper(1.5); this.scene.add(this.axes);
    this.resizeObserver = new ResizeObserver(() => this.resize()); this.resizeObserver.observe(container);
    this.renderer.domElement.addEventListener('pointerdown', (event) => this.pick(event));
    this.running = true; this.animate(); this.resize();
  }
  async rebuild(nativeScene, onProgress) {
    this.clearWireframePreview();
    if (this.content) { this.scene.remove(this.content); this.disposeObject(this.content); }
    this.clearSelection();
    this.clearVertexLockPreview();
    this.pathObjects.clear(); this.lodGroups = new Map(); this.colliderObjects = [];
    const root = new THREE.Group(); root.name = 'LuciaUSD';
    const total = nativeScene.numRootNodes();
    for (let i = 0; i < total; i++) {
      const object = await LightUSDLoaderUtils.buildThreeNode(nativeScene.getRootNode(i), null, nativeScene, { onProgress, yieldMode: 'raf' });
      root.add(object);
    }
    let triangles = 0, meshes = 0, textureBytes = 0; const countedTextures = new Set();
    root.traverse((object) => { const path = object.userData?.['primMeta.absPath']; if (path) this.pathObjects.set(path, object); if (!object.isMesh) return; meshes++; const geometry = object.geometry; triangles += geometry?.index ? Math.floor(geometry.index.count / 3) : Math.floor((geometry?.attributes?.position?.count || 0) / 3); const materials = Array.isArray(object.material) ? object.material : [object.material]; for (const material of materials) for (const value of Object.values(material || {})) if (value?.isTexture && !countedTextures.has(value)) { countedTextures.add(value); textureBytes += (value.image?.width || 0) * (value.image?.height || 0) * 4; } });
    this.clearChangeOverlay();
    this.previewBudget = choosePreviewBudget({ triangles, meshes, textureBytes });
    this.previewBudget.geometryDecimatedCount = 0; this.previewBudget.geometryOmittedTriangles = 0;
    if (this.previewBudget.geometrySampleRatio < 1) root.traverse((object) => { if (!object.isMesh || !object.geometry) return; const geometry = object.geometry, indexCount = geometry.index?.count || 0, vertexCount = geometry.attributes?.position?.count || 0, sourceCount = indexCount || Math.floor(vertexCount / 3) * 3, drawCount = previewGeometryDrawCount({ indexCount, vertexCount, sampleRatio: this.previewBudget.geometrySampleRatio }); if (drawCount < sourceCount) { geometry.setDrawRange(0, drawCount); this.previewBudget.geometryDecimatedCount++; this.previewBudget.geometryOmittedTriangles += Math.floor((sourceCount - drawCount) / 3); } });
    this.previewBudget.textureDegradedCount = await this.downsamplePreviewTextures(root, this.previewBudget.maxTextureDimension, this.previewBudget.maxTextureBytes);
    this.previewBudget.textureSkippedCount = this.previewTextureSkippedCount || 0;
    this.renderer.setPixelRatio(this.previewBudget.pixelRatio);
    this.content = root; this.scene.add(root); this.discoverLODGroups(); this.discoverColliderObjects(); this.setColliderPreview(this.colliderPreview); this.updateLODVisibility(); this.frameAll(); if (this.pendingTransferError) { const pending = this.pendingTransferError; this.pendingTransferError = null; this.showTransferErrorPreview(pending.path, pending.distances); }
  }
  async downsamplePreviewTextures(root, maxDimension, maxBytes = Infinity) {
    if (!Number.isInteger(maxDimension) || maxDimension < 1 || typeof document === 'undefined') return 0;
    const textures = new Set();
    root.traverse((object) => { const materials = Array.isArray(object.material) ? object.material : [object.material]; for (const material of materials) for (const value of Object.values(material || {})) if (value?.isTexture) textures.add(value); });
    const records = [...textures].map((texture) => ({ texture, width: texture.image?.width || 0, height: texture.image?.height || 0 })).filter(({ width, height }) => width > 0 && height > 0);
    const totalBytes = records.reduce((sum, record) => sum + record.width * record.height * 4, 0);
    let degraded = 0; this.previewTextureSkippedCount = 0;
    for (const { texture, width, height } of records) {
      const image = texture.image, scale = previewTextureScale({ width, height, maxDimension, maxBytes, totalBytes });
      if (scale >= 1) continue;
      const canvas = document.createElement('canvas'); canvas.width = Math.max(1, Math.round(width * scale)); canvas.height = Math.max(1, Math.round(height * scale));
      try { const context = canvas.getContext('2d'); if (!context) { this.previewTextureSkippedCount++; continue; } if (image.data && (image.data instanceof Uint8Array || image.data instanceof Uint8ClampedArray) && image.data.length === width * height * 4) { const sourceCanvas = document.createElement('canvas'); sourceCanvas.width = width; sourceCanvas.height = height; const sourceContext = sourceCanvas.getContext('2d'); if (!sourceContext) { this.previewTextureSkippedCount++; continue; } sourceContext.putImageData(new ImageData(new Uint8ClampedArray(image.data), width, height), 0, 0); context.drawImage(sourceCanvas, 0, 0, canvas.width, canvas.height); } else if (image.data) { this.previewTextureSkippedCount++; continue; } else context.drawImage(image, 0, 0, canvas.width, canvas.height); texture.image = canvas; texture.needsUpdate = true; texture.userData = { ...(texture.userData || {}), luciaPreviewResize: { sourceWidth: width, sourceHeight: height, width: canvas.width, height: canvas.height, maxDimension, maxBytes, sourceType: image.data ? 'rgba8-data' : 'drawable' } }; degraded++; } catch { this.previewTextureSkippedCount++; /* Non-drawable images remain at their authored preview size. */ }
    }
    return degraded;
  }
  pick(event) {
    if (!this.content) return;
    const box = this.renderer.domElement.getBoundingClientRect();
    const pointer = new THREE.Vector2((event.clientX - box.left) / box.width * 2 - 1, -(event.clientY - box.top) / box.height * 2 + 1);
    const ray = new THREE.Raycaster(); ray.setFromCamera(pointer, this.camera);
    const intersection = ray.intersectObject(this.content, true)[0], hit = intersection?.object;
    let object = hit;
    while (object && !object.userData?.['primMeta.absPath']) object = object.parent;
    if (object) {
      const path = object.userData['primMeta.absPath'];
      if ((event.shiftKey || event.altKey) && intersection?.object?.isMesh) {
        const vertexIndex = this.closestHitVertex(intersection.object, intersection);
        if (vertexIndex != null) { this.dispatchEvent(new CustomEvent('vertex-select', { detail: { path, vertexIndex, toggle: event.altKey } })); return; }
      }
      this.dispatchEvent(new CustomEvent('select', { detail: path }));
    }
  }
  closestHitVertex(mesh, intersection) {
    const geometry = mesh?.geometry, position = geometry?.attributes?.position, face = intersection?.faceIndex;
    if (!position?.array || !Number.isInteger(face) || face < 0) return null;
    const index = geometry.index?.array, offset = face * 3, candidates = index ? [index[offset], index[offset + 1], index[offset + 2]] : [offset, offset + 1, offset + 2];
    if (candidates.some((value) => !Number.isInteger(value) || value < 0 || value >= position.count) || !intersection.point) return null;
    let best = candidates[0], distance = Infinity;
    for (const candidate of candidates) { const point = new THREE.Vector3().fromBufferAttribute(position, candidate).applyMatrix4(mesh.matrixWorld); const next = point.distanceToSquared(intersection.point); if (next < distance) { distance = next; best = candidate; } }
    return best;
  }
  clearSelection() {
    if (!this.selection) return;
    this.scene.remove(this.selection);
    for (const helper of this.selection.children || []) { helper.geometry?.dispose?.(); helper.material?.dispose?.(); }
    this.selection = null;
  }
  selectPaths(paths = []) {
    this.clearSelection();
    const group = new THREE.Group();
    for (const path of [...new Set(paths)].sort()) {
      const object = this.pathObjects.get(path); if (!object) continue;
      const helper = new THREE.BoxHelper(object, 0xa88bff); helper.material.depthTest = false; helper.renderOrder = 999; group.add(helper);
    }
    if (!group.children.length) return;
    this.selection = group; this.scene.add(group);
  }
  select(path) { this.selectPaths(path ? [path] : []); }
  clearVertexLockPreview() {
    if (!this.vertexLockPreview) return;
    this.scene.remove(this.vertexLockPreview); this.vertexLockPreview.geometry.dispose(); this.vertexLockPreview.material.dispose(); this.vertexLockPreview = null;
  }
  clearWireframePreview() {
    if (!this.wireframePreview) return;
    this.wireframePreview.parent?.remove(this.wireframePreview);
    this.wireframePreview.geometry?.dispose?.(); this.wireframePreview.material?.dispose?.(); this.wireframePreview = null;
  }
  renderableMeshForPath(path) {
    const object = this.pathObjects.get(path); let mesh = object?.isMesh || object?.geometry ? object : object?.getObjectByProperty?.('isMesh', true);
    if (!mesh) object?.traverse?.((child) => { if (!mesh && child.isMesh) mesh = child; });
    if (!mesh) for (const [candidatePath, candidate] of this.pathObjects) if (candidatePath.startsWith(`${path}/`)) { if (candidate.isMesh) { mesh = candidate; break; } candidate.traverse?.((child) => { if (!mesh && child.isMesh) mesh = child; }); if (mesh) break; }
    if (!mesh) this.content?.traverse?.((child) => { if (!mesh && (child.isMesh || child.geometry) && (child.userData?.['primMeta.absPath'] === path || child.name === path.split('/').at(-1))) mesh = child; });
    return mesh || null;
  }
  captureMaterialSnapshot(path) {
    const mesh = this.renderableMeshForPath(path), geometry = mesh?.geometry, position = geometry?.attributes?.position;
    if (!mesh || !position?.array || !Number.isInteger(position.count)) return null;
    const index = geometry.index?.array || null, triangleCount = Math.floor((index?.length || position.count) / 3), materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material], colors = geometry.attributes?.color;
    const displayColor = mesh.userData?.luciaDisplayColor;
    const signature = (material) => JSON.stringify({ type: material?.type || null, color: material?.color?.isColor ? material.color.getHexString() : null, roughness: material?.roughness ?? null, metalness: material?.metalness ?? null, opacity: material?.opacity ?? null, map: material?.map?.uuid || null, normalMap: material?.normalMap?.uuid || null, displayColor: Array.isArray(displayColor) ? displayColor.map((value) => Number(value).toFixed(6)) : null });
    const colorSignature = (triangle) => {
      if (!colors?.array || colors.itemSize !== 3) return '';
      const ids = index ? [index[triangle * 3], index[triangle * 3 + 1], index[triangle * 3 + 2]] : [triangle * 3, triangle * 3 + 1, triangle * 3 + 2];
      return ids.map((id) => { const offset = id * 3; return [colors.array[offset], colors.array[offset + 1], colors.array[offset + 2]].map((value) => Number.isFinite(value) ? Number(value).toFixed(6) : 'nan').join(','); }).join(';');
    };
    const materialSignatures = materials.map(signature), triangles = new Array(triangleCount).fill('');
    for (const group of geometry.groups || []) { const start = Math.floor(group.start / 3), end = Math.min(triangleCount, Math.ceil((group.start + group.count) / 3)); for (let triangle = start; triangle < end; triangle++) triangles[triangle] = materialSignatures[group.materialIndex] || ''; }
    if (!geometry.groups?.length) triangles.fill(materialSignatures[0] || '');
    for (let triangle = 0; triangle < triangleCount; triangle++) triangles[triangle] += `|${colorSignature(triangle)}`;
    return { path, triangleCount, triangles };
  }
  showMaterialDifference(before, after) {
    this.clearChangeOverlay();
    if (!materialSnapshotsDiffer(before, after) || !after.triangleCount) return false;
    const mesh = this.renderableMeshForPath(after.path), geometry = mesh?.geometry, position = geometry?.attributes?.position;
    if (!mesh || !position?.array) return false;
    const index = geometry.index?.array || null, count = Math.min(before.triangleCount, after.triangleCount), edges = [], matrix = mesh.matrixWorld;
    for (let triangle = 0; triangle < count && edges.length < 20000 * 18; triangle++) {
      if (before.triangles[triangle] === after.triangles[triangle]) continue;
      const ids = index ? [index[triangle * 3], index[triangle * 3 + 1], index[triangle * 3 + 2]] : [triangle * 3, triangle * 3 + 1, triangle * 3 + 2];
      if (ids.some((id) => !Number.isInteger(id) || id < 0 || id >= position.count)) continue;
      for (const [a, b] of [[ids[0], ids[1]], [ids[1], ids[2]], [ids[2], ids[0]]]) { const start = new THREE.Vector3().fromBufferAttribute(position, a).applyMatrix4(matrix), end = new THREE.Vector3().fromBufferAttribute(position, b).applyMatrix4(matrix); edges.push(start.x, start.y, start.z, end.x, end.y, end.z); }
    }
    if (!edges.length) return false;
    const lineGeometry = new THREE.BufferGeometry(); lineGeometry.setAttribute('position', new THREE.Float32BufferAttribute(edges, 3));
    const material = new THREE.LineBasicMaterial({ color: 0xff4f91, transparent: true, opacity: .95, depthTest: false });
    this.changeOverlay = new THREE.LineSegments(lineGeometry, material); this.changeOverlay.renderOrder = 999; this.changeOverlay.name = 'LuciaMaterialDifference'; this.comparisonMaterial = true; this.scene.add(this.changeOverlay); return true;
  }
  setWireframePreview(path, enabled) {
    this.clearWireframePreview();
    if (!enabled) return false;
    const mesh = this.renderableMeshForPath(path), geometry = mesh?.geometry;
    if (!mesh || !geometry) return false;
    const lines = new THREE.LineSegments(new THREE.WireframeGeometry(geometry), new THREE.LineBasicMaterial({ color: 0x8ee8ff, transparent: true, opacity: .72, depthTest: false }));
    lines.name = 'LuciaWireframeDensity'; lines.renderOrder = 997; mesh.add(lines); this.wireframePreview = lines; return true;
  }
  showVertexLockPreview(path, indices = []) {
    this.clearVertexLockPreview();
    const object = this.pathObjects.get(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), position = mesh?.geometry?.attributes?.position;
    if (!mesh || !position?.array || !indices?.length) return;
    const points = new Float32Array(indices.length * 3); let count = 0;
    for (const index of indices) {
      if (!Number.isInteger(index) || index < 0 || index >= position.count) continue;
      const point = new THREE.Vector3().fromBufferAttribute(position, index).applyMatrix4(mesh.matrixWorld); point.toArray(points, count * 3); count++;
    }
    if (!count) return;
    const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', new THREE.BufferAttribute(points.subarray(0, count * 3), 3));
    const material = new THREE.PointsMaterial({ color: 0xffc45c, size: .06, sizeAttenuation: true, depthTest: false });
    this.vertexLockPreview = new THREE.Points(geometry, material); this.vertexLockPreview.renderOrder = 1000; this.vertexLockPreview.name = 'LuciaRetopoVertexLocks'; this.vertexLockPreview.userData = { luciaSamplePath: path }; this.scene.add(this.vertexLockPreview);
  }
  showTransferErrorPreview(path, distances) {
    this.clearVertexLockPreview();
    const object = this.pathObjects.get(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), position = mesh?.geometry?.attributes?.position;
    if (!mesh || !position?.array || !distances || distances.length !== position.count) return;
    const finiteDistances = [...distances].filter(Number.isFinite), max = Math.max(...finiteDistances, 1e-8), points = new Float32Array(position.count * 3), colors = new Float32Array(position.count * 3), world = mesh.matrixWorld;
    for (let index = 0; index < position.count; index++) { const point = new THREE.Vector3().fromBufferAttribute(position, index).applyMatrix4(world); point.toArray(points, index * 3); const color = new THREE.Color().setHSL(Math.max(0, .34 - Math.min(1, Math.max(0, distances[index]) / max) * .34), .9, .52); colors.set([color.r, color.g, color.b], index * 3); }
    const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', new THREE.BufferAttribute(points, 3)); geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    const material = new THREE.PointsMaterial({ size: Math.max(this.comparisonBounds?.span || 1, 1) * .014, vertexColors: true, depthTest: false, transparent: true, opacity: .95 });
    this.vertexLockPreview = new THREE.Points(geometry, material); this.vertexLockPreview.renderOrder = 1000; this.vertexLockPreview.name = 'LuciaSkinTransferError'; this.vertexLockPreview.userData = { luciaTransferPath: path, maxDistance: max }; this.scene.add(this.vertexLockPreview);
  }
  captureGeometry(path, maxTriangles = 20000) {
    const object = this.pathObjects.get(path), mesh = object?.isMesh ? object : object?.getObjectByProperty?.('isMesh', true), geometry = mesh?.geometry, position = geometry?.attributes?.position;
    if (!position?.array || !Number.isInteger(position.count) || position.count < 3) return null;
    const indices = geometry.index?.array || null, triangleCount = Math.min(maxTriangles, Math.floor((indices ? indices.length : position.count) / 3));
    if (!triangleCount) return null;
    const output = new Float32Array(triangleCount * 18), matrix = mesh.matrixWorld;
    const write = (target, source) => { const x = position.array[source * 3], y = position.array[source * 3 + 1], z = position.array[source * 3 + 2]; target.set([x, y, z], 0); if (matrix) new THREE.Vector3(x, y, z).applyMatrix4(matrix).toArray(target, 0); };
    let cursor = 0;
    for (let face = 0; face < triangleCount; face++) {
      const a = indices ? indices[face * 3] : face * 3, b = indices ? indices[face * 3 + 1] : face * 3 + 1, c = indices ? indices[face * 3 + 2] : face * 3 + 2;
      if (![a, b, c].every((value) => Number.isInteger(value) && value >= 0 && value < position.count)) continue;
      for (const [from, to] of [[a, b], [b, c], [c, a]]) { const start = cursor; write(output.subarray(start, start + 3), from); write(output.subarray(start + 3, start + 6), to); cursor += 6; }
    }
    return cursor ? output.subarray(0, cursor) : null;
  }
  clearChangeOverlay() {
    if (this.changeOverlay) {
      this.scene.remove(this.changeOverlay);
      this.changeOverlay.traverse((object) => { object.geometry?.dispose?.(); object.material?.dispose?.(); });
      this.changeOverlay = null;
    }
    this.comparisonBounds = null;
    this.comparisonMaterial = false; this.comparisonGhost = false; this.comparisonHeatmap = false;
  }
  showChangeOverlay(edges) {
    this.clearChangeOverlay();
    if (!edges?.length) return;
    const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', new THREE.BufferAttribute(edges, 3));
    const material = new THREE.LineBasicMaterial({ color: 0xff6b55, transparent: true, opacity: .8, depthTest: false });
    this.changeOverlay = new THREE.LineSegments(geometry, material); this.changeOverlay.renderOrder = 998; this.changeOverlay.name = 'LuciaChangedElements'; this.scene.add(this.changeOverlay);
  }
  showBeforeAfter(beforeEdges, afterEdges) {
    this.clearChangeOverlay();
    if (!beforeEdges?.length && !afterEdges?.length) return;
    this.comparisonHeatmap = false; this.comparisonGhost = false;
    const all = [...(beforeEdges || []), ...(afterEdges || [])];
    const bounds = new THREE.Box3();
    for (let i = 0; i < all.length; i += 3) bounds.expandByPoint(new THREE.Vector3(all[i], all[i + 1], all[i + 2]));
    if (bounds.isEmpty()) return;
    const min = bounds.min.x, max = bounds.max.x, span = Math.max(max - min, 1e-6);
    const make = (edges, color, normal) => {
      if (!edges?.length) return null;
      const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', new THREE.BufferAttribute(edges, 3));
      const material = new THREE.LineBasicMaterial({ color, transparent: true, opacity: .9, depthTest: false, clippingPlanes: [new THREE.Plane(normal, 0)] });
      const object = new THREE.LineSegments(geometry, material); object.renderOrder = 998; return object;
    };
    const group = new THREE.Group(); group.name = 'LuciaBeforeAfterWipe';
    group.userData.beforeEdges = beforeEdges; group.userData.afterEdges = afterEdges;
    this.comparisonEdges = { beforeEdges, afterEdges };
    group.userData.make = make; group.userData.min = min; group.userData.max = max;
    this.scene.add(group); this.changeOverlay = group; this.comparisonBounds = { min, max, span };
    this.setComparisonWipe(.5);
  }
  showGhostCompare() {
    this.clearChangeOverlay();
    if (!this.comparisonEdges?.beforeEdges?.length && !this.comparisonEdges?.afterEdges?.length) return;
    const make = (edges, color, opacity) => {
      if (!edges?.length) return null;
      const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', new THREE.BufferAttribute(edges, 3));
      const material = new THREE.LineBasicMaterial({ color, transparent: true, opacity, depthTest: false });
      const object = new THREE.LineSegments(geometry, material); object.renderOrder = 998; return object;
    };
    const group = new THREE.Group(); group.name = 'LuciaGhostCompare';
    const before = make(this.comparisonEdges.beforeEdges, 0xffa14a, .3), after = make(this.comparisonEdges.afterEdges, 0x62d8ff, .85);
    if (before) group.add(before); if (after) group.add(after);
    if (!group.children.length) return;
    this.changeOverlay = group; this.comparisonGhost = true; this.comparisonHeatmap = false; this.scene.add(group);
  }
  setComparisonWipe(value) {
    if (!this.changeOverlay?.isGroup || !this.comparisonBounds) return;
    const group = this.changeOverlay, t = Math.max(0, Math.min(1, Number(value) || 0));
    for (const child of [...group.children]) { child.geometry?.dispose?.(); child.material?.dispose?.(); group.remove(child); }
    const split = this.comparisonBounds.min + this.comparisonBounds.span * t;
    const before = group.userData.make(group.userData.beforeEdges, 0xffa14a, new THREE.Vector3(-1, 0, 0));
    const after = group.userData.make(group.userData.afterEdges, 0x62d8ff, new THREE.Vector3(1, 0, 0));
    if (before) { before.material.clippingPlanes[0].constant = -split; group.add(before); }
    if (after) { after.material.clippingPlanes[0].constant = split; group.add(after); }
    group.userData.wipe = t;
  }
  showSurfaceDistanceHeatmap(comparison) {
    this.clearChangeOverlay();
    if (!comparison?.samples?.length) return;
    const positions = new Float32Array(comparison.samples.length * 3), colors = new Float32Array(comparison.samples.length * 3);
    comparison.samples.forEach((sample, index) => { positions.set(sample.position, index * 3); const color = new THREE.Color().setHSL(Math.max(0, Math.min(1, .34 - sample.normalized * .34)), .9, .52); colors.set([color.r, color.g, color.b], index * 3); });
    const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3)); geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    const material = new THREE.PointsMaterial({ size: Math.max(this.comparisonBounds?.span || 1, 1) * .012, vertexColors: true, depthTest: false, transparent: true, opacity: .95 });
    this.changeOverlay = new THREE.Points(geometry, material); this.changeOverlay.renderOrder = 999; this.changeOverlay.name = 'LuciaErrorHeatmap'; this.comparisonData = comparison; this.comparisonGhost = false; this.comparisonHeatmap = true; this.scene.add(this.changeOverlay);
  }
  showErrorHeatmap(comparison) { this.showSurfaceDistanceHeatmap(comparison); }
  showNormalDeviationHeatmap(comparison) {
    this.clearChangeOverlay();
    const samples = comparison?.normals?.samples;
    if (!samples?.length) return;
    const positions = new Float32Array(samples.length * 3), colors = new Float32Array(samples.length * 3);
    samples.forEach((sample, index) => { positions.set(sample.position, index * 3); const color = new THREE.Color().setHSL(Math.max(0, .34 - Math.min(1, sample.normalized) * .34), .9, .52); colors.set([color.r, color.g, color.b], index * 3); });
    const geometry = new THREE.BufferGeometry(); geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3)); geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    const material = new THREE.PointsMaterial({ size: Math.max(this.comparisonBounds?.span || 1, 1) * .014, vertexColors: true, depthTest: false, transparent: true, opacity: .95 });
    this.changeOverlay = new THREE.Points(geometry, material); this.changeOverlay.renderOrder = 999; this.changeOverlay.name = 'LuciaNormalDeviationHeatmap'; this.comparisonGhost = false; this.comparisonHeatmap = true; this.scene.add(this.changeOverlay);
  }
  restoreBeforeAfter() { if (this.comparisonEdges) this.showBeforeAfter(this.comparisonEdges.beforeEdges, this.comparisonEdges.afterEdges); }
  objectForPath(path) { return this.pathObjects.get(path) || null; }
  discoverLODGroups() {
    this.lodGroups.clear();
    for (const [path, object] of this.pathObjects) {
      const match = path.match(/^(.*)_LOD([1-9][0-9]*)$/); if (!match || !this.pathObjects.has(match[1])) continue;
      const levels = this.lodGroups.get(match[1]) || [{ level: 0, object: this.pathObjects.get(match[1]) }];
      levels.push({ level: Number(match[2]), object }); levels.sort((a, b) => a.level - b.level); this.lodGroups.set(match[1], levels);
    }
  }
  setLODPreview(enabled) { this.lodPreview = Boolean(enabled); this.updateLODVisibility(); }
  updateLODVisibility() {
    if (!this.lodGroups?.size) return;
    for (const levels of this.lodGroups.values()) {
      if (!this.lodPreview) { for (const level of levels) level.object.visible = true; continue; }
      const source = levels[0].object, box = new THREE.Box3().setFromObject(source), center = box.getCenter(new THREE.Vector3()), size = Math.max(box.getSize(new THREE.Vector3()).length(), 1), distance = this.camera.position.distanceTo(center) / size;
      const selected = Math.min(levels.length - 1, Math.max(0, Math.floor(Math.log2(Math.max(1, distance / 8)))));
      levels.forEach((level, index) => { level.object.visible = index === selected; });
    }
  }
  discoverColliderObjects() { this.colliderObjects = [...this.pathObjects.entries()].filter(([path]) => /_(?:Collider|TriangleCollider)$/.test(path)).map(([, object]) => object); }
  setColliderPreview(enabled) { this.colliderPreview = Boolean(enabled); for (const object of this.colliderObjects || []) object.visible = this.colliderPreview; }
  frame(path) { const object = this.pathObjects.get(path); if (object) this.frameObject(object); }
  frameAll() { if (this.content) this.frameObject(this.content); }
  frameObject(object) {
    const box = new THREE.Box3().setFromObject(object); if (box.isEmpty()) return;
    const center = box.getCenter(new THREE.Vector3()); const size = Math.max(box.getSize(new THREE.Vector3()).length(), 1);
    const direction = new THREE.Vector3(1, .7, 1).normalize();
    this.camera.position.copy(center).addScaledVector(direction, size * 1.4);
    this.camera.near = Math.max(.01, size / 1000); this.camera.far = Math.max(100, size * 100); this.camera.updateProjectionMatrix();
    this.controls.target.copy(center); this.controls.update();
  }
  resize() { const { clientWidth: w, clientHeight: h } = this.container; if (!w || !h) return; this.camera.aspect = w / h; this.camera.updateProjectionMatrix(); this.renderer.setSize(w, h, false); }
  animate() { if (!this.running) return; requestAnimationFrame(() => this.animate()); this.controls.update(); this.updateLODVisibility(); if (this.selection?.children) this.selection.children.forEach((helper) => helper.update()); else this.selection?.update(); this.renderer.render(this.scene, this.camera); }
  disposeObject(root) { root.traverse((o) => { o.geometry?.dispose?.(); const mats = Array.isArray(o.material) ? o.material : [o.material]; mats.forEach((m) => { if (!m) return; for (const v of Object.values(m)) if (v?.isTexture) v.dispose(); m.dispose?.(); }); }); }
  dispose() { this.running = false; this.resizeObserver.disconnect(); this.controls.dispose(); this.clearChangeOverlay(); this.clearSelection(); this.clearWireframePreview(); this.lodGroups?.clear(); if (this.content) this.disposeObject(this.content); this.renderer.dispose(); this.renderer.domElement.remove(); }
}
