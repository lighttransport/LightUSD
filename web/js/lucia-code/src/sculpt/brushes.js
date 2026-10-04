import { LuciaError } from '../utils.js';

export const SCULPT_BRUSHES = Object.freeze(['draw', 'smooth', 'inflate', 'grab', 'flatten', 'pinch', 'mask']);
export const SCULPT_FALLOFFS = Object.freeze(['smooth', 'sphere', 'linear', 'constant']);
export const SCULPT_DEFAULTS = Object.freeze({ brush: 'draw', radius: 0.25, strength: 0.5, falloff: 'smooth', symmetry: [false, false, false], invert: false, spacing: 0.15 });

export function falloffWeight(kind, t) {
  if (t >= 1) return 0;
  switch (kind) {
    case 'smooth': { const s = 1 - t; return s * s * (3 - 2 * s); }
    case 'sphere': return Math.sqrt(1 - t * t);
    case 'linear': return 1 - t;
    case 'constant': return 1;
    default: throw new LuciaError('LUCIA_SCULPT_SETTINGS', `Unknown falloff: ${kind}`);
  }
}

export function normalizeSculptSettings(input = {}) {
  const settings = { ...SCULPT_DEFAULTS, ...input };
  if (!SCULPT_BRUSHES.includes(settings.brush)) throw new LuciaError('LUCIA_SCULPT_SETTINGS', `Unknown brush: ${settings.brush}`);
  if (!SCULPT_FALLOFFS.includes(settings.falloff)) throw new LuciaError('LUCIA_SCULPT_SETTINGS', `Unknown falloff: ${settings.falloff}`);
  if (!(Number.isFinite(settings.radius) && settings.radius > 0)) throw new LuciaError('LUCIA_SCULPT_SETTINGS', 'Brush radius must be positive.');
  if (!(Number.isFinite(settings.strength) && settings.strength >= 0 && settings.strength <= 1)) throw new LuciaError('LUCIA_SCULPT_SETTINGS', 'Brush strength must be between 0 and 1.');
  if (!(Number.isFinite(settings.spacing) && settings.spacing > 0 && settings.spacing <= 1)) throw new LuciaError('LUCIA_SCULPT_SETTINGS', 'Brush spacing must be in (0, 1].');
  if (!Array.isArray(settings.symmetry) || settings.symmetry.length !== 3) throw new LuciaError('LUCIA_SCULPT_SETTINGS', 'Symmetry must be [x, y, z] booleans.');
  return { ...settings, symmetry: settings.symmetry.map(Boolean), invert: Boolean(settings.invert) };
}

// Mirror variants for the active symmetry axes, including identity.
export function symmetryMirrors(symmetry) {
  let mirrors = [[1, 1, 1]];
  symmetry.forEach((enabled, axis) => { if (enabled) mirrors = mirrors.flatMap((m) => [m, m.map((s, i) => i === axis ? -s : s)]); });
  return mirrors;
}
const mirror = (v, m) => [v[0] * m[0], v[1] * m[1], v[2] * m[2]];

// One dab of a brush. Returns the set of touched reps.
export function applyDab(mesh, settings, dab) {
  const { brush, radius, falloff } = settings, sign = settings.invert ? -1 : 1, pressure = dab.pressure ?? 1;
  const hits = brush === 'grab' ? dab.grabSet : mesh.queryRadius(dab.center, radius);
  if (!hits?.length) return [];
  const p = mesh.positions, n = mesh.normals, touched = [];
  const weightOf = (v, distance) => falloffWeight(falloff, distance / radius) * settings.strength * pressure * (brush === 'mask' ? 1 : 1 - mesh.mask[v]);
  // Area normal/center: falloff-weighted average under the brush.
  let an = [0, 0, 0], ac = [0, 0, 0], total = 0;
  for (const [v, d] of hits) { const w = falloffWeight(falloff, d / radius) || 1e-6; total += w; for (let k = 0; k < 3; k++) { an[k] += n[v * 3 + k] * w; ac[k] += p[v * 3 + k] * w; } }
  const al = Math.hypot(...an) || 1; an = an.map((x) => x / al); ac = ac.map((x) => x / total);
  const step = radius * 0.1;
  let drift = 0;
  const updates = [];
  for (const [v, d] of hits) {
    const w = weightOf(v, d);
    if (w <= 0) continue;
    const x = p[v * 3], y = p[v * 3 + 1], z = p[v * 3 + 2];
    let out;
    switch (brush) {
      case 'draw': out = [x + an[0] * w * step * sign, y + an[1] * w * step * sign, z + an[2] * w * step * sign]; break;
      case 'inflate': out = [x + n[v * 3] * w * step * sign, y + n[v * 3 + 1] * w * step * sign, z + n[v * 3 + 2] * w * step * sign]; break;
      case 'smooth': {
        const start = mesh.neighborOffsets[v], end = mesh.neighborOffsets[v + 1];
        if (end === start) continue;
        const avg = [0, 0, 0];
        for (let k = start; k < end; k++) { const u = mesh.neighbors[k] * 3; avg[0] += p[u]; avg[1] += p[u + 1]; avg[2] += p[u + 2]; }
        const t = Math.min(1, w), count = end - start;
        out = [x + (avg[0] / count - x) * t, y + (avg[1] / count - y) * t, z + (avg[2] / count - z) * t];
        break;
      }
      case 'flatten': { const dist = (x - ac[0]) * an[0] + (y - ac[1]) * an[1] + (z - ac[2]) * an[2], t = Math.min(1, w) * sign; out = [x - an[0] * dist * t, y - an[1] * dist * t, z - an[2] * dist * t]; break; }
      case 'pinch': { const t = Math.min(1, w) * 0.5 * sign; out = [x + (dab.center[0] - x) * t, y + (dab.center[1] - y) * t, z + (dab.center[2] - z) * t]; break; }
      case 'grab': out = [x + dab.delta[0] * w, y + dab.delta[1] * w, z + dab.delta[2] * w]; break;
      case 'mask': mesh.setRepMask(v, Math.min(1, Math.max(0, mesh.mask[v] + w * sign))); touched.push(v); continue;
      default: throw new LuciaError('LUCIA_SCULPT_SETTINGS', `Unknown brush: ${brush}`);
    }
    updates.push([v, out]);
  }
  // Smooth reads neighbors, so apply all moves after computing them.
  for (const [v, out] of updates) { drift = Math.max(drift, mesh.setRepPosition(v, out[0], out[1], out[2])); touched.push(v); }
  if (mesh.grid) mesh.grid.drift += drift;
  if (brush !== 'mask' && touched.length) {
    const ring = new Set(touched);
    for (const v of touched) for (let k = mesh.neighborOffsets[v]; k < mesh.neighborOffsets[v + 1]; k++) ring.add(mesh.neighbors[k]);
    mesh.updateNormals(ring);
  }
  return touched;
}

// A stroke turns a sequence of surface samples into evenly spaced dabs and
// applies them (with symmetry) to a SculptMesh. Coordinates are mesh-local.
export class SculptStroke {
  constructor(mesh, settings) {
    this.mesh = mesh;
    this.settings = normalizeSculptSettings(settings);
    this.mirrors = symmetryMirrors(this.settings.symmetry);
    this.last = null; this.distance = 0; this.dabs = 0; this.touched = new Set(); this.grab = null;
    mesh.buildGrid(this.settings.radius);
  }

  // sample = { point:[3], pressure? }. Returns the number of dabs applied.
  add(sample) {
    const point = sample.point;
    if (!Array.isArray(point) || point.length !== 3 || !point.every(Number.isFinite)) throw new LuciaError('LUCIA_SCULPT_INPUT', 'Stroke samples need a finite 3D point.');
    if (this.settings.brush === 'grab') return this.addGrab(point);
    if (!this.last) { this.dab(point, sample.pressure); this.last = point; return 1; }
    const spacing = this.settings.radius * this.settings.spacing, delta = point.map((x, i) => x - this.last[i]), length = Math.hypot(...delta);
    let applied = 0;
    while (this.distance + length >= spacing * (applied + 1) && applied < 1024) {
      const t = (spacing * (applied + 1) - this.distance) / length;
      this.dab(this.last.map((x, i) => x + delta[i] * t), sample.pressure); applied++;
    }
    this.distance = (this.distance + length) - spacing * applied;
    this.last = point;
    return applied;
  }

  addGrab(point) {
    if (!this.grab) {
      // Capture the affected region once; grab moves it rigidly with falloff.
      this.grab = this.mirrors.map((m) => ({ m, origin: mirror(point, m), set: this.mesh.queryRadius(mirror(point, m), this.settings.radius) }));
      this.last = point; return 0;
    }
    const delta = point.map((x, i) => x - this.last[i]);
    for (const { m, set } of this.grab) for (const v of applyDab(this.mesh, this.settings, { center: null, delta: mirror(delta, m), grabSet: set })) this.touched.add(v);
    this.last = point; this.dabs++;
    return 1;
  }

  dab(center, pressure = 1) {
    for (const m of this.mirrors) for (const v of applyDab(this.mesh, this.settings, { center: mirror(center, m), pressure })) this.touched.add(v);
    this.dabs++;
  }
}
