import test from 'node:test';
import assert from 'node:assert/strict';
import { SculptMesh } from '../src/sculpt/sculpt-mesh.js';
import { applyDab, falloffWeight, normalizeSculptSettings, SculptStroke, symmetryMirrors, SCULPT_BRUSHES } from '../src/sculpt/brushes.js';
import { getNodeType } from '../src/geonodes/index.js';
import { LuciaUsdSession } from '../src/usd-session.js';
import { LuciaCommandStack, sessionCommand } from '../src/command-stack.js';
if (!globalThis.CustomEvent) globalThis.CustomEvent = class extends Event { constructor(type, options = {}) { super(type); this.detail = options.detail; } };

const grid = (n = 41, size = 2) => getNodeType('MeshGrid').evaluate({ sizeX: size, sizeY: size, verticesX: n, verticesY: n }).mesh.mesh;
const nearest = (mesh, x, y) => { let best = 0, distance = Infinity; for (let i = 0; i < mesh.vertexCount; i++) { const d = Math.hypot(mesh.original[i * 3] - x, mesh.original[i * 3 + 1] - y); if (d < distance) { distance = d; best = i; } } return best; };
const z = (mesh, v) => mesh.positions[v * 3 + 2];
const stroke = (mesh, settings, points) => { const s = new SculptStroke(mesh, settings); for (const point of points) s.add({ point }); return s; };
const laplacianEnergy = (mesh) => { let e = 0; for (const v of mesh.reps) { const a = mesh.neighborOffsets[v], b = mesh.neighborOffsets[v + 1]; if (b === a) continue; for (let k = 0; k < 3; k++) { let avg = 0; for (let j = a; j < b; j++) avg += mesh.positions[mesh.neighbors[j] * 3 + k]; e += (avg / (b - a) - mesh.positions[v * 3 + k]) ** 2; } } return e; };

test('settings, falloff and symmetry helpers validate input', () => {
  assert.equal(falloffWeight('smooth', 0), 1); assert.equal(falloffWeight('linear', 0.5), 0.5); assert.equal(falloffWeight('constant', 1), 0);
  assert.throws(() => normalizeSculptSettings({ brush: 'clay' }), /Unknown brush/);
  assert.throws(() => normalizeSculptSettings({ radius: 0 }), /radius/);
  assert.throws(() => normalizeSculptSettings({ strength: 2 }), /strength/);
  assert.equal(symmetryMirrors([true, false, true]).length, 4);
  assert.deepEqual(SCULPT_BRUSHES, ['draw', 'smooth', 'inflate', 'grab', 'flatten', 'pinch', 'mask']);
});

test('seam-split vertices are welded and move together', () => {
  // Two triangles sharing an edge, with the shared vertices duplicated (UV seam).
  const mesh = new SculptMesh({ positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2, 3, 4, 5]) });
  assert.equal(mesh.reps.length, 4);
  stroke(mesh, { brush: 'draw', radius: 3, strength: 1 }, [[0.5, 0.5, 0]]);
  assert.equal(mesh.positions[1 * 3 + 2], mesh.positions[3 * 3 + 2]);
  assert.equal(mesh.positions[2 * 3 + 2], mesh.positions[5 * 3 + 2]);
  assert.ok(mesh.changed());
});

test('draw, inflate and invert displace along the surface normal', () => {
  const draw = new SculptMesh(grid()), center = nearest(draw, 0, 0), edge = nearest(draw, 0.9, 0);
  stroke(draw, { brush: 'draw', radius: 0.5, strength: 1 }, [[0, 0, 0]]);
  assert.ok(z(draw, center) > 0.04, 'centre rises'); assert.equal(z(draw, edge), 0, 'outside radius untouched');
  const inverted = new SculptMesh(grid());
  stroke(inverted, { brush: 'draw', radius: 0.5, strength: 1, invert: true }, [[0, 0, 0]]);
  assert.ok(Math.abs(z(inverted, center) + z(draw, center)) < 1e-6);
  const inflate = new SculptMesh(grid());
  stroke(inflate, { brush: 'inflate', radius: 0.5, strength: 1 }, [[0, 0, 0]]);
  assert.ok(z(inflate, center) > 0);
});

test('smooth reduces Laplacian energy and flatten pulls toward the area plane', () => {
  const mesh = new SculptMesh(grid());
  stroke(mesh, { brush: 'draw', radius: 0.4, strength: 1 }, [[0, 0, 0], [0.01, 0, 0]]);
  const rough = laplacianEnergy(mesh), peak = z(mesh, nearest(mesh, 0, 0));
  stroke(mesh, { brush: 'smooth', radius: 0.6, strength: 1 }, [[0, 0, 0]]);
  assert.ok(laplacianEnergy(mesh) < rough);
  stroke(mesh, { brush: 'flatten', radius: 0.6, strength: 1 }, [[0, 0, 0]]);
  assert.ok(z(mesh, nearest(mesh, 0, 0)) < peak);
});

test('pinch pulls toward the dab centre and grab follows the drag delta', () => {
  const pinch = new SculptMesh(grid()), v = nearest(pinch, 0.2, 0);
  stroke(pinch, { brush: 'pinch', radius: 0.5, strength: 1 }, [[0, 0, 0]]);
  assert.ok(pinch.positions[v * 3] < 0.2);
  const grab = new SculptMesh(grid()), c = nearest(grab, 0, 0), far = nearest(grab, 0.9, 0.9);
  stroke(grab, { brush: 'grab', radius: 0.4, strength: 1, falloff: 'constant' }, [[0, 0, 0], [0, 0, 0.3]]);
  assert.ok(Math.abs(z(grab, c) - 0.3) < 1e-6);
  assert.equal(z(grab, far), 0);
});

test('mask blocks deformation and symmetry mirrors dabs', () => {
  const mesh = new SculptMesh(grid()), left = nearest(mesh, -0.3, 0), right = nearest(mesh, 0.3, 0);
  stroke(mesh, { brush: 'mask', radius: 0.2, strength: 1, falloff: 'constant' }, [[0.3, 0, 0]]);
  assert.equal(mesh.mask[right], 1);
  assert.equal(mesh.changed(), false, 'mask strokes do not move points');
  stroke(mesh, { brush: 'draw', radius: 0.2, strength: 1, symmetry: [true, false, false] }, [[-0.3, 0, 0]]);
  assert.ok(z(mesh, left) > 0); assert.equal(z(mesh, right), 0, 'mirrored dab is masked');
  const sym = new SculptMesh(grid());
  stroke(sym, { brush: 'draw', radius: 0.2, strength: 1, symmetry: [true, false, false] }, [[-0.3, 0, 0]]);
  assert.ok(Math.abs(z(sym, left) - z(sym, right)) < 1e-6);
});

test('stroke spacing interpolates dabs and topology never changes', () => {
  const mesh = new SculptMesh(grid()), indices = mesh.indices.slice();
  const s = stroke(mesh, { brush: 'draw', radius: 0.2, strength: 0.2, spacing: 0.25 }, [[-0.5, 0, 0], [0.5, 0, 0]]);
  assert.equal(s.dabs, 1 + 20);
  assert.deepEqual(mesh.indices, indices);
  assert.equal(applyDab(mesh, normalizeSculptSettings({ radius: 0.01 }), { center: [5, 5, 5] }).length, 0);
  mesh.revert(); assert.equal(mesh.changed(), false);
  const hit = mesh.raycast([0.1, 0.1, 5], [0, 0, -1]);
  assert.ok(Math.abs(hit.distance - 5) < 1e-6); assert.deepEqual(hit.normal.map(Math.abs), [0, 0, 1]);
});

test('sculpt commit writes only points, maps reordered render vertices and undoes as one step', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA(`#usda 1.0
( defaultPrim = "World" )
def Xform "World" {
    def Mesh "Plane" {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        int[] faceVertexIndices = [0, 1, 2, 3]
        int[] faceVertexCounts = [4]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] ( interpolation = "vertex" )
    }
}
`);
    const commands = new LuciaCommandStack(), original = session.exportUSDA();
    // Render mesh with reversed vertex order and a duplicated seam vertex.
    const mesh = new SculptMesh({ positions: new Float32Array([0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]), indices: new Uint32Array([3, 2, 1, 4, 1, 0]) });
    stroke(mesh, { brush: 'draw', radius: 3, strength: 1, falloff: 'constant' }, [[0.5, 0.5, 0]]);
    const mapped = mesh.mapToAuthoredPoints(await session.getMeshPoints('/World/Plane'));
    assert.equal(mapped.length, 12);
    assert.ok(mapped[2] > 0 && mapped[2] === mapped[5] && mapped[5] === mapped[8]);
    assert.equal(await commands.execute(sessionCommand(session, 'Sculpt: draw', ['/World/Plane'], () => session.setMeshPoints('/World/Plane', mapped))), true);
    assert.deepEqual(await session.getMeshPoints('/World/Plane'), mapped);
    assert.match(session.exportUSDA(), /primvars:st/);
    assert.match(session.exportUSDA(), /extent = \[\(0, 0, 0\.3\d*\), \(1, 1, 0\.3\d*\)\]/);
    assert.match(session.exportUSDA(), /faceVertexCounts = \[4\]/);
    await commands.undo();
    assert.equal(session.exportUSDA(), original);
    assert.throws(() => new SculptMesh({ positions: new Float32Array([9, 9, 9, 8, 8, 8, 7, 7, 7]), indices: new Uint32Array([0, 1, 2]) }).mapToAuthoredPoints(mapped), /no longer matches/);
    await assert.rejects(session.setMeshPoints('/World/Plane', new Float32Array([NaN, 0, 0])), /finite/);
  } finally { session.dispose(); }
});

test('stroke-local raycasts match full raycasts and track dirty vertices', () => {
  const mesh = new SculptMesh(grid());
  stroke(mesh, { brush: 'draw', radius: 0.3, strength: 1 }, [[0, 0, 0]]);
  assert.ok(mesh.dirty.size > 0); mesh.dirty.clear();
  for (const [x, y] of [[0.05, 0.02], [0.2, -0.1], [0.9, 0.9]]) {
    const full = mesh.raycast([x, y, 5], [0, 0, -1]), near = mesh.raycast([x, y, 5], [0, 0, -1], mesh.facesNear([0, 0, 0], 0.9));
    if (Math.hypot(x, y) < 0.5) { assert.ok(near); assert.ok(Math.abs(near.distance - full.distance) < 1e-9); }
  }
  assert.equal(mesh.raycast([3, 3, 5], [0, 0, -1], mesh.facesNear([0, 0, 0], 0.2)), null, 'far rays miss the local set (caller falls back)');
  assert.ok(mesh.facesNear([0, 0, 0], 0.1).length < mesh.indices.length / 3 / 10);
});
