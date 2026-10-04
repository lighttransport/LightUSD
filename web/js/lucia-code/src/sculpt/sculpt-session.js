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
    this.onHover = (event) => this.hover(event);
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
    if (this.stroke || this.app.activity?.active) return;
    const hit = this.bridge.surfaceHit(event, this.path);
    if (hit) this.bridge.setBrushCursor(hit.mesh, hit.point, hit.normal, this.settings.radius); else this.bridge.clearBrushCursor();
  }

  pointerDown(event) {
    if (!this.active || event.button !== 0 || this.app.commands?.busy || this.app.activity?.active) return false;
    const hit = this.bridge.surfaceHit(event, this.path);
    if (!hit) return false;
    try {
      this.renderMesh = this.acquireMesh();
      // Ctrl inverts, Shift temporarily smooths (Blender conventions).
      const settings = { ...this.settings, invert: this.settings.invert !== event.ctrlKey, brush: event.shiftKey ? 'smooth' : this.settings.brush, pressure: undefined };
      this.stroke = new SculptStroke(this.sculptMesh, settings);
      this.anchor = hit.point;
      this.stroke.add({ point: hit.point, pressure: event.pressure || 1 });
      this.syncGeometry();
    } catch (error) { this.stroke = null; this.app.showError(error); return true; }
    try { event.target.setPointerCapture?.(event.pointerId); } catch { /* synthetic or already-released pointer */ }
    window.addEventListener('pointermove', this.onMove); window.addEventListener('pointerup', this.onUp);
    return true;
  }

  pointerMove(event) {
    if (!this.stroke) return;
    const grab = this.stroke.settings.brush === 'grab';
    const point = grab ? this.bridge.pointerOnPlane(event, this.renderMesh, this.anchor) : this.bridge.surfaceHit(event, this.path)?.point;
    if (!point) return;
    if (this.stroke.add({ point, pressure: event.pressure || 1 })) this.syncGeometry();
    if (!grab) { const hit = this.bridge.surfaceHit(event, this.path); if (hit) this.bridge.setBrushCursor(hit.mesh, hit.point, hit.normal, this.settings.radius); }
  }

  async pointerUp() {
    window.removeEventListener('pointermove', this.onMove); window.removeEventListener('pointerup', this.onUp);
    const stroke = this.stroke; this.stroke = null;
    if (!stroke) return;
    if (stroke.settings.brush === 'mask') { this.bridge.setMaskPreview(this.renderMesh, this.sculptMesh.positions, this.sculptMesh.mask); return; }
    if (!this.sculptMesh.changed()) return;
    await this.commit(`Sculpt: ${stroke.settings.brush}`);
  }

  async commit(summary) {
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
    window.removeEventListener('pointermove', this.onMove); window.removeEventListener('pointerup', this.onUp);
    if (this.stroke) { this.stroke = null; this.revert(); }
  }

  syncGeometry() {
    const geometry = this.renderMesh?.geometry;
    if (!geometry) return;
    geometry.attributes.position.array.set(this.sculptMesh.positions);
    geometry.attributes.position.needsUpdate = true;
    geometry.computeVertexNormals();
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
