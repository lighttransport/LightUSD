import * as THREE from 'three';
import { LuciaError } from '../utils.js';
import { SculptMesh } from './sculpt-mesh.js';
import { SculptStroke, normalizeSculptSettings, SCULPT_DEFAULTS } from './brushes.js';

// Interactive sculpt mode for one mesh prim. Strokes deform the preview
// geometry in place (no USD round-trip); pointerup commits the stroke as a
// single undoable `points` edit through LuciaApp.runMutation.
export class LuciaSculptController extends EventTarget {
  constructor(app) {
    super();
    this.app = app; this.path = null; this.sculptMesh = null; this.stroke = null; this.settings = { ...SCULPT_DEFAULTS };
    this.onMove = (event) => this.pointerMove(event);
    this.onUp = (event) => this.pointerUp(event);
    this.onCancel = () => this.cancelStroke();
    this.onHover = (event) => { this.hoverEvent = event; this.hoverFrame ||= requestAnimationFrame(() => { this.hoverFrame = 0; this.hover(this.hoverEvent); }); };
    this.queue = []; this.frame = 0;
  }

  get active() { return this.path != null; }
  get bridge() { return this.app.bridge; }

  async enter(path) {
    if (this.active) this.exit();
    const mesh = this.bridge.renderableMeshForPath(path);
    if (!mesh?.geometry?.attributes?.position) throw new LuciaError('LUCIA_SCULPT_UNSUPPORTED', 'Select a mesh with editable points to sculpt.');
    if (mesh.isSkinnedMesh || mesh.morphTargetInfluences || mesh.isInstancedMesh) throw new LuciaError('LUCIA_SCULPT_UNSUPPORTED', 'Skinned, blend-shape and instanced meshes cannot be sculpted.');
    const authored = await this.app.session.getAuthoredMesh(path);
    if (!authored?.points || authored.animated) throw new LuciaError('LUCIA_SCULPT_UNSUPPORTED', 'Sculpting needs untimed authored points (animated or referenced meshes are not supported).');
    this.path = path;
    this.settings.radius = this.defaultRadius(mesh);
    const controls = this.bridge.controls;
    this.savedButtons = { ...controls.mouseButtons };
    // Left button sculpts; middle orbits, right pans.
    controls.mouseButtons.LEFT = null; controls.mouseButtons.MIDDLE = THREE.MOUSE.ROTATE;
    this.bridge.preserveCamera = true;
    this.bridge.pointerTool = (event) => this.pointerDown(event);
    this.bridge.renderer.domElement.addEventListener('pointermove', this.onHover);
    this.dispatchEvent(new Event('change'));
  }

  exit() {
    if (!this.active) return;
    this.cancelStroke();
    const controls = this.bridge.controls;
    if (this.savedButtons) Object.assign(controls.mouseButtons, this.savedButtons);
    this.bridge.preserveCamera = false; this.bridge.pointerTool = null;
    this.bridge.renderer.domElement.removeEventListener('pointermove', this.onHover);
    cancelAnimationFrame(this.hoverFrame); this.hoverFrame = 0; this.lastHover = null;
    this.bridge.clearBrushCursor(); this.bridge.clearMaskPreview();
    this.path = null; this.sculptMesh = null;
    this.dispatchEvent(new Event('change'));
  }

  defaultRadius(mesh) {
    mesh.geometry.computeBoundingSphere();
    const radius = mesh.geometry.boundingSphere?.radius;
    return Number.isFinite(radius) && radius > 0 ? Number((radius * 0.15).toPrecision(3)) : SCULPT_DEFAULTS.radius;
  }

  setSettings(patch) {
    this.settings = normalizeSculptSettings({ ...this.settings, ...patch });
    this.dispatchEvent(new Event('change'));
  }

  // The render mesh is rebuilt after every commit/undo; reuse the working
  // copy (and its mask) when the rebuilt geometry still matches it.
  acquireMesh() {
    const mesh = this.bridge.renderableMeshForPath(this.path), geometry = mesh?.geometry;
    if (!geometry?.attributes?.position) throw new LuciaError('LUCIA_SCULPT_UNSUPPORTED', 'The sculpted mesh is no longer available.');
    const positions = geometry.attributes.position.array;
    const reusable = this.sculptMesh && this.sculptMesh.vertexCount === geometry.attributes.position.count && this.sculptMesh.positions.every((value, index) => value === positions[index]);
    if (!reusable) this.sculptMesh = new SculptMesh({ positions: geometry.attributes.position.array, indices: geometry.index?.array || null });
    else this.sculptMesh.commit();
    return mesh;
  }

  hover(event) {
    if (!this.active || this.stroke || this.app.activity?.active) return;
    // Reuse the working copy for a local raycast when it is still current.
    const current = this.sculptMesh && this.renderMesh?.parent && this.bridge.renderableMeshForPath(this.path) === this.renderMesh;
    let hit = null;
    if (current && this.lastHover) {
      const { origin, direction } = this.bridge.localRay(event, this.renderMesh);
      const near = this.sculptMesh.raycast(origin, direction, this.sculptMesh.facesNear(this.lastHover, this.settings.radius * 3));
      if (near) hit = { mesh: this.renderMesh, point: near.point, normal: near.normal };
    }
    hit ||= this.bridge.surfaceHit(event, this.path);
    this.lastHover = hit?.point || null;
    if (hit) this.bridge.setBrushCursor(hit.mesh, hit.point, hit.normal, this.settings.radius); else this.bridge.clearBrushCursor();
  }

  pointerDown(event) {
    if (!this.active || event.button !== 0) return false;
    // While a stroke is committing, swallow input: starting a stroke would move
    // the mapping baseline under the pending commit.
    if (this.committing || this.app.commands?.busy || this.app.activity?.active) return this.committing;
    const hit = this.bridge.surfaceHit(event, this.path);
    if (!hit) return false;
    try {
      this.renderMesh = this.acquireMesh();
      // Ctrl inverts, Shift temporarily smooths (Blender conventions).
      const settings = { ...this.settings, invert: this.settings.invert !== event.ctrlKey, brush: event.shiftKey ? 'smooth' : this.settings.brush, pressure: undefined };
      this.stroke = new SculptStroke(this.sculptMesh, settings);
      this.anchor = hit.point; this.lastHit = hit.point; this.queue = [];
      this.stroke.add({ point: hit.point, pressure: event.pressure || 1 });
      this.syncDirty();
    } catch (error) { this.stroke = null; this.app.showError(error); return true; }
    try { event.target.setPointerCapture?.(event.pointerId); } catch { /* synthetic or already-released pointer */ }
    this.listen(true);
    return true;
  }

  // Pointer moves are coalesced into one update per animation frame.
  pointerMove(event) {
    if (!this.stroke) return;
    this.queue.push(event);
    this.frame ||= requestAnimationFrame(() => this.flush());
  }

  flush() {
    cancelAnimationFrame(this.frame); this.frame = 0;
    const queue = this.queue; this.queue = [];
    if (!this.stroke || !queue.length) return;
    const grab = this.stroke.settings.brush === 'grab';
    let cursor = null;
    for (const event of queue) {
      const hit = grab ? null : this.strokeHit(event), point = grab ? this.bridge.pointerOnPlane(event, this.renderMesh, this.anchor) : hit?.point;
      if (!point) continue;
      this.stroke.add({ point, pressure: event.pressure || 1 });
      if (hit) { cursor = hit; this.lastHit = hit.point; }
    }
    this.syncDirty();
    if (cursor) this.bridge.setBrushCursor(this.renderMesh, cursor.point, cursor.normal, this.settings.radius);
  }

  // Raycast only triangles near the previous hit (O(brush area)); fall back
  // to a full raycast when the pointer jumps away.
  strokeHit(event) {
    const { origin, direction } = this.bridge.localRay(event, this.renderMesh);
    const near = this.lastHit ? this.sculptMesh.raycast(origin, direction, this.sculptMesh.facesNear(this.lastHit, this.settings.radius * 3)) : null;
    if (near) return near;
    const full = this.bridge.surfaceHit(event, this.path);
    return full && { point: full.point, normal: full.normal };
  }

  listen(on) {
    const method = on ? 'addEventListener' : 'removeEventListener';
    window[method]('pointermove', this.onMove); window[method]('pointerup', this.onUp); window[method]('pointercancel', this.onCancel);
  }

  async pointerUp() {
    this.listen(false);
    this.flush();
    const stroke = this.stroke; this.stroke = null;
    if (!stroke) return;
    this.finishGeometry();
    if (stroke.settings.brush === 'mask') { this.bridge.setMaskPreview(this.renderMesh, this.sculptMesh.positions, this.sculptMesh.mask); return; }
    if (!this.sculptMesh.changed()) return;
    await this.commit(`Sculpt: ${stroke.settings.brush}`);
  }

  async commit(summary) {
    this.committing = true;
    try { return await this.commitStroke(summary); } finally { this.committing = false; }
  }

  async commitStroke(summary) {
    const path = this.path, sculptMesh = this.sculptMesh;
    let mapped;
    try {
      const authored = await this.app.session.getMeshPoints(path);
      if (!authored) throw new LuciaError('LUCIA_SCULPT_UNSUPPORTED', 'The mesh points became animated or unavailable.');
      mapped = sculptMesh.mapToAuthoredPoints(authored);
    } catch (error) { this.revert(); this.app.showError(error); return false; }
    const ok = await this.app.runMutation(summary, [path], () => this.app.session.setMeshPoints(path, mapped, `${summary} ${path}`), ['scene', 'usd'], { comparison: false });
    if (!ok) { this.revert(); return false; }
    this.dispatchEvent(new CustomEvent('committed', { detail: { path } }));
    if (this.active) { try { this.renderMesh = this.acquireMesh(); this.bridge.setMaskPreview(this.renderMesh, this.sculptMesh.positions, this.sculptMesh.mask); } catch {} }
    return true;
  }

  revert() {
    if (!this.sculptMesh) return;
    this.sculptMesh.revert(); this.syncGeometry();
  }

  cancelStroke() {
    this.listen(false);
    cancelAnimationFrame(this.frame); this.frame = 0; this.queue = [];
    if (this.stroke) { this.stroke = null; this.revert(); }
  }

  // Full upload (revert / cancel).
  syncGeometry() {
    const geometry = this.renderMesh?.geometry;
    if (!geometry) return;
    geometry.attributes.position.array.set(this.sculptMesh.positions);
    geometry.attributes.position.needsUpdate = true;
    geometry.computeVertexNormals();
    this.sculptMesh.dirty.clear();
    this.finishGeometry();
  }

  // Per-frame upload of only the vertices whose position/normal changed,
  // reusing the kernel's incrementally maintained area-weighted normals.
  syncDirty() {
    const geometry = this.renderMesh?.geometry, mesh = this.sculptMesh;
    if (!geometry || !mesh.dirty.size) return;
    const position = geometry.attributes.position, normal = geometry.attributes.normal?.count === position.count ? geometry.attributes.normal : null;
    let lo = Infinity, hi = -1;
    for (const rep of mesh.dirty) for (let k = mesh.memberOffsets[rep]; k < mesh.memberOffsets[rep + 1]; k++) {
      const v = mesh.members[k], o = v * 3;
      position.array[o] = mesh.positions[o]; position.array[o + 1] = mesh.positions[o + 1]; position.array[o + 2] = mesh.positions[o + 2];
      if (normal) { normal.array[o] = mesh.normals[rep * 3]; normal.array[o + 1] = mesh.normals[rep * 3 + 1]; normal.array[o + 2] = mesh.normals[rep * 3 + 2]; }
      if (v < lo) lo = v; if (v > hi) hi = v;
    }
    mesh.dirty.clear();
    for (const attribute of [position, normal]) {
      if (!attribute) continue;
      attribute.clearUpdateRanges?.(); attribute.addUpdateRange?.(lo * 3, (hi - lo + 1) * 3);
      attribute.needsUpdate = true;
    }
    if (!normal) geometry.computeVertexNormals();
    // Bounds stay stale until the stroke ends (finishGeometry): nulling them
    // makes three.js frustum culling recompute them, O(n), every frame.
  }

  finishGeometry() {
    const geometry = this.renderMesh?.geometry;
    if (!geometry) return;
    geometry.computeBoundingSphere(); geometry.computeBoundingBox();
  }

  clearMask() {
    if (!this.sculptMesh) return;
    this.sculptMesh.mask.fill(0); this.bridge.clearMaskPreview();
  }

  invertMask() {
    if (!this.sculptMesh) { try { this.renderMesh = this.acquireMesh(); } catch (error) { this.app.showError(error); return; } }
    const mask = this.sculptMesh.mask;
    for (let i = 0; i < mask.length; i++) mask[i] = 1 - mask[i];
    this.bridge.setMaskPreview(this.renderMesh, this.sculptMesh.positions, mask);
  }
}
