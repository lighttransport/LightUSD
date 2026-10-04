import test from 'node:test';
import assert from 'node:assert/strict';
import { composeTRS, createDefaultGraph, decomposeInstanceTransform, evaluateGraph, geometryToOutput, GeoNodesCache, geometryStats, geometryToMeshData, getNodeType, listNodeTypes, mergeByDistance, meshGeometry, realize, registerNode, subdivideMidpoint, topoOrder, validateGraph, valueNoise3 } from '../src/geonodes/index.js';
import { evaluateGraphToMesh, GeoNodesRunner, sourceInputHash, triangulateAuthoredMesh } from '../src/geonodes/runner.js';
import { LuciaUsdSession } from '../src/usd-session.js';
import { LuciaCommandStack, sessionCommand } from '../src/command-stack.js';
if (!globalThis.CustomEvent) globalThis.CustomEvent = class extends Event { constructor(type, options = {}) { super(type); this.detail = options.detail; } };

const graphOf = (nodes, links) => ({ version: 1, nodes: [...nodes, { id: 'out', type: 'GroupOutput' }], links });
const scatter = (seed = 3) => graphOf([
  { id: 'grid', type: 'MeshGrid', params: { verticesX: 6, verticesY: 6 } }, { id: 'noise', type: 'NoiseTexture', params: { scale: 3 } },
  { id: 'disp', type: 'Displace', params: { strength: 0.2 } }, { id: 'pts', type: 'DistributePointsOnFaces', params: { density: 4, seed } },
  { id: 'cube', type: 'MeshCube', params: { size: [0.1, 0.1, 0.1] } }, { id: 'inst', type: 'InstanceOnPoints' }, { id: 'join', type: 'JoinGeometry' },
], [
  { from: ['grid', 'mesh'], to: ['disp', 'geometry'] }, { from: ['noise', 'value'], to: ['disp', 'height'] }, { from: ['disp', 'geometry'], to: ['pts', 'mesh'] },
  { from: ['pts', 'points'], to: ['inst', 'points'] }, { from: ['cube', 'mesh'], to: ['inst', 'instance'] },
  { from: ['disp', 'geometry'], to: ['join', 'a'] }, { from: ['inst', 'instances'], to: ['join', 'b'] }, { from: ['join', 'geometry'], to: ['out', 'geometry'] },
]);

test('graph validation rejects unknown types, bad sockets, duplicate inputs and cycles', () => {
  assert.throws(() => validateGraph({ version: 2, nodes: [], links: [] }), /version/);
  assert.throws(() => validateGraph(graphOf([{ id: 'x', type: 'Nope' }], [])), /Unknown node type/);
  assert.throws(() => validateGraph({ version: 1, nodes: [{ id: 'a', type: 'MeshCube' }], links: [] }), /exactly one Group Output/);
  assert.throws(() => validateGraph(graphOf([{ id: 'v', type: 'Value' }], [{ from: ['v', 'value'], to: ['out', 'geometry'] }])), /Cannot connect float to geometry/);
  assert.throws(() => validateGraph(graphOf([{ id: 'c', type: 'MeshCube' }, { id: 'd', type: 'MeshCube' }], [{ from: ['c', 'mesh'], to: ['out', 'geometry'] }, { from: ['d', 'mesh'], to: ['out', 'geometry'] }])), /more than one link/);
  assert.throws(() => validateGraph(graphOf([{ id: 'm', type: 'MeshCube', params: { size: 'big' } }], [])), /invalid vector/);
  assert.throws(() => validateGraph(graphOf([{ id: 'm', type: 'Math', params: { operation: 'explode' } }], [])), /invalid enum/);
  assert.throws(() => validateGraph(graphOf([{ id: 'a', type: 'Transform' }, { id: 'b', type: 'Transform' }], [{ from: ['a', 'geometry'], to: ['b', 'geometry'] }, { from: ['b', 'geometry'], to: ['a', 'geometry'] }, { from: ['b', 'geometry'], to: ['out', 'geometry'] }])), /cycle/);
  // Unreachable nodes are legal and skipped.
  const graph = validateGraph(graphOf([{ id: 'c', type: 'MeshCube' }, { id: 'lonely', type: 'MeshGrid' }], [{ from: ['c', 'mesh'], to: ['out', 'geometry'] }]));
  assert.deepEqual(topoOrder(graph), ['c', 'out']);
  assert.ok(listNodeTypes().length >= 20);
  assert.throws(() => registerNode('MeshCube', { evaluate() {} }), /already registered/);
});

test('primitive nodes produce expected element counts', async () => {
  const counts = async (type, params) => geometryStats((await evaluateGraph(graphOf([{ id: 'p', type, params }], [{ from: ['p', 'mesh'], to: ['out', 'geometry'] }]))).geometry);
  assert.deepEqual(await counts('MeshCube', {}), { vertices: 24, triangles: 12, points: 0, instances: 0 });
  assert.deepEqual(await counts('MeshGrid', { verticesX: 4, verticesY: 3 }), { vertices: 12, triangles: 12, points: 0, instances: 0 });
  assert.deepEqual(await counts('MeshUVSphere', { segments: 8, rings: 4 }), { vertices: 2 + 8 * 3, triangles: 8 * 2 + 8 * 2 * 2, points: 0, instances: 0 });
  assert.deepEqual(await counts('MeshCylinder', { vertices: 6 }), { vertices: 14, triangles: 24, points: 0, instances: 0 });
  await assert.rejects(counts('MeshGrid', { verticesX: 2048, verticesY: 2048 }), /limit/);
});

test('evaluation is deterministic and the node cache makes re-evaluation incremental', async () => {
  const cache = new GeoNodesCache(), first = await evaluateGraph(scatter(), { cache }), misses = cache.misses;
  const again = await evaluateGraph(scatter(), { cache });
  assert.equal(again.key, first.key);
  assert.equal(cache.misses, misses, 'unchanged graph is fully cached');
  const realized = geometryToMeshData(first.geometry), fresh = geometryToMeshData((await evaluateGraph(scatter())).geometry);
  assert.deepEqual(realized.positions, fresh.positions);
  // A downstream-only edit (seed) re-evaluates only pts, inst, join and out.
  const before = cache.misses, edited = await evaluateGraph(scatter(9), { cache });
  assert.notEqual(edited.key, first.key);
  assert.equal(cache.misses - before, 4);
  assert.ok(geometryStats(first.geometry).instances > 0);
  assert.equal(geometryStats(realize(first.geometry)).instances, 0);
});

test('fields drive per-vertex math through Set Position', async () => {
  const graph = graphOf([
    { id: 'grid', type: 'MeshGrid', params: { verticesX: 3, verticesY: 3 } }, { id: 'pos', type: 'Position' }, { id: 'sep', type: 'SeparateXYZ' },
    { id: 'mul', type: 'Math', params: { operation: 'multiply', b: 2 } }, { id: 'vec', type: 'Vector' }, { id: 'set', type: 'SetPosition' },
  ], [
    { from: ['grid', 'mesh'], to: ['set', 'geometry'] }, { from: ['pos', 'position'], to: ['sep', 'vector'] }, { from: ['sep', 'x'], to: ['mul', 'a'] },
    { from: ['mul', 'value'], to: ['vec', 'z'] }, { from: ['vec', 'vector'], to: ['set', 'offset'] }, { from: ['set', 'geometry'], to: ['out', 'geometry'] },
  ]);
  const { positions } = geometryToMeshData((await evaluateGraph(graph)).geometry);
  for (let i = 0; i < positions.length; i += 3) assert.ok(Math.abs(positions[i + 2] - positions[i] * 2) < 1e-6);
});

test('mesh helpers: subdivide, merge, noise and group input', async () => {
  const quad = meshGeometry(new Float32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]), new Uint32Array([0, 1, 2, 0, 2, 3])).mesh;
  const sub = subdivideMidpoint(quad);
  assert.equal(sub.indices.length / 3, 8); assert.equal(sub.positions.length / 3, 9);
  const split = { positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0, 1e-5, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2, 3, 5, 4]) };
  assert.equal(mergeByDistance(split, 1e-3).positions.length / 3, 3);
  assert.equal(valueNoise3(0.3, 0.7, 0.1, 5), valueNoise3(0.3, 0.7, 0.1, 5));
  assert.notEqual(valueNoise3(0.3, 0.7, 0.1, 5), valueNoise3(0.3, 0.7, 0.1, 6));
  const source = triangulateAuthoredMesh({ points: [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], faceVertexCounts: [4], faceVertexIndices: [0, 1, 2, 3] });
  assert.deepEqual([...source.indices], [0, 1, 2, 0, 2, 3]);
  const result = await evaluateGraphToMesh(createDefaultGraph(), source);
  assert.deepEqual([...result.positions], [...source.positions]);
  assert.equal(result.inputHash, sourceInputHash(source));
  const runner = new GeoNodesRunner();
  assert.equal((await runner.evaluate(createDefaultGraph(), source)).key, result.key);
  await assert.rejects(evaluateGraphToMesh(graphOf([], []), null), /no mesh faces/);
  assert.equal(getNodeType('Subdivide').label, 'Subdivide Mesh');
});

const MESH_USDA = `#usda 1.0
( defaultPrim = "World" )
def Xform "World" {
    def Mesh "Plane" {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        int[] faceVertexIndices = [0, 1, 2, 3]
        int[] faceVertexCounts = [4]
    }
}
`;

test('geometry node commits persist the graph, author a sibling output and undo atomically', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA(MESH_USDA);
    const commands = new LuciaCommandStack(), original = session.exportUSDA(), path = '/World/Plane';
    const graph = graphOf([{ id: 'in', type: 'GroupInput' }, { id: 'sub', type: 'Subdivide', params: { level: 1 } }], [{ from: ['in', 'geometry'], to: ['sub', 'geometry'] }, { from: ['sub', 'geometry'], to: ['out', 'geometry'] }]);
    const commit = async () => { const result = await evaluateGraphToMesh(graph, triangulateAuthoredMesh(await session.getAuthoredMesh(path))); return session.commitGeomNodes(path, graph, result, { inputHash: result.inputHash, graphKey: result.key }); };
    assert.equal(await commands.execute(sessionCommand(session, 'Geometry Nodes', [path], commit)), true);
    assert.deepEqual(session.getGeomNodesGraph(path), validateGraph(graph));
    const info = session.getGeomNodesOutputInfo('/World/Plane_geonodes');
    assert.equal(info.source, path);
    assert.equal(info.inputHash, sourceInputHash(triangulateAuthoredMesh(await session.getAuthoredMesh(path))));
    assert.equal((await session.getMeshPoints('/World/Plane_geonodes')).length / 3, 9);
    assert.match(session.exportUSDA(), /def Mesh "Plane"[^}]*token visibility = "invisible"/);
    assert.match(session.exportUSDA(), /def Mesh "Plane_geonodes"[^}]*float3\[\] extent = \[\(0, 0, 0\), \(1, 1, 0\)\]/);
    // Re-commit updates the same output instead of colliding.
    await commit();
    assert.equal(session.exportUSDA().match(/Plane_geonodes/g).length, 1);
    await commands.undo();
    assert.equal(session.getGeomNodesGraph(path), null);
    assert.equal(session.getGeomNodesOutputInfo('/World/Plane_geonodes'), null);
    assert.equal(session.exportUSDA(), original);
    // Apply collapses into the source; remove restores it.
    await commit();
    const evaluated = await evaluateGraphToMesh(graph, triangulateAuthoredMesh(await session.getAuthoredMesh(path)));
    await session.applyGeomNodes(path, evaluated);
    assert.equal(session.getGeomNodesGraph(path), null);
    assert.doesNotMatch(session.exportUSDA(), /Plane_geonodes|visibility/);
    assert.equal((await session.getMeshPoints(path)).length / 3, 9);
    await commit();
    await session.removeGeomNodes(path);
    assert.doesNotMatch(session.exportUSDA(), /Plane_geonodes|lucia:geomNodes|visibility/);
  } finally { session.dispose(); }
});

test('geometry node commits refuse to overwrite foreign prims', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA(MESH_USDA.replace('def Xform "World" {', 'def Xform "World" {\n    def Xform "Plane_geonodes" {}'));
    const source = triangulateAuthoredMesh(await session.getAuthoredMesh('/World/Plane')), result = await evaluateGraphToMesh(createDefaultGraph(), source), before = session.exportUSDA();
    await assert.rejects(session.commitGeomNodes('/World/Plane', createDefaultGraph(), result, { inputHash: result.inputHash }), /not owned/);
    assert.equal(session.exportUSDA(), before);
  } finally { session.dispose(); }
});

test('instance transforms decompose into PointInstancer TRS and reject shear', () => {
  const rotate = ([w, x, y, z], v) => { const t = [2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])]; return [v[0] + w * t[0] + y * t[2] - z * t[1], v[1] + w * t[1] + z * t[0] - x * t[2], v[2] + w * t[2] + x * t[1] - y * t[0]]; };
  for (const [t, r, k] of [[[1, 2, 3], [0, 0, 0], [1, 1, 1]], [[-4, 0, 9], [30, 45, 60], [2, 0.5, 3]], [[0, 0, 0], [170, -80, 10], [1, 1, 1]], [[1, 1, 1], [0, 90, 0], [-1, 2, 1]]]) {
    const m = composeTRS(t, r, k), d = decomposeInstanceTransform(m);
    assert.ok(d, `decomposable ${r}`);
    assert.ok(Math.abs(Math.hypot(...d.orientation) - 1) < 1e-5);
    for (const v of [[1, 0, 0], [0, 1, 0], [0.3, -2, 5]]) {
      const expected = [0, 1, 2].map((i) => m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i]);
      const actual = rotate(d.orientation, v.map((x, i) => x * d.scale[i])).map((x, i) => x + d.translate[i]);
      for (let i = 0; i < 3; i++) assert.ok(Math.abs(actual[i] - expected[i]) < 1e-4, `${r} ${k}: ${actual} vs ${expected}`);
    }
  }
  const shear = composeTRS(); shear[4] = 0.5;
  assert.equal(decomposeInstanceTransform(shear), null);
});

test('instances become a PointInstancer unless Group Output realizes them', async () => {
  const result = await evaluateGraph(scatter());
  const output = geometryToOutput(result.geometry, { realizeInstances: result.realizeInstances });
  const count = geometryStats(result.geometry).instances;
  assert.equal(result.realizeInstances, false);
  assert.equal(output.instancer.protoIndices.length, count);
  assert.equal(output.instancer.prototypes.length, 1);
  assert.equal(output.instancer.positions.length, count * 3);
  assert.equal(output.instancer.orientations.length, count * 4);
  assert.equal(output.mesh.positions.length / 3, 36, 'displaced grid stays a mesh');
  const realizedGraph = scatter(); realizedGraph.nodes.find((node) => node.type === 'GroupOutput').params = { realizeInstances: true };
  const realized = await evaluateGraphToMesh(realizedGraph, null);
  assert.equal(realized.output.instancer, null);
  assert.equal(realized.output.mesh.positions.length, realized.positions.length);
});

test('PointInstancer output is authored with prototypes, rebuilt on re-commit and cleaned up', async () => {
  const session = new LuciaUsdSession(); await session.init();
  try {
    await session.loadUSDA(MESH_USDA.replace('def Mesh "Plane" {', 'def Mesh "Plane" {\n        double3 xformOp:translate = (0, 2, 0)\n        uniform token[] xformOpOrder = ["xformOp:translate"]'));
    const commands = new LuciaCommandStack(), original = session.exportUSDA(), path = '/World/Plane';
    const graph = graphOf([{ id: 'in', type: 'GroupInput' }, { id: 'pts', type: 'DistributePointsOnFaces', params: { density: 6, seed: 2 } }, { id: 'cube', type: 'MeshCube', params: { size: [0.1, 0.1, 0.1] } }, { id: 'inst', type: 'InstanceOnPoints' }, { id: 'join', type: 'JoinGeometry' }],
      [{ from: ['in', 'geometry'], to: ['pts', 'mesh'] }, { from: ['pts', 'points'], to: ['inst', 'points'] }, { from: ['cube', 'mesh'], to: ['inst', 'instance'] }, { from: ['in', 'geometry'], to: ['join', 'a'] }, { from: ['inst', 'instances'], to: ['join', 'b'] }, { from: ['join', 'geometry'], to: ['out', 'geometry'] }]);
    const commit = async (g = graph) => { const result = await evaluateGraphToMesh(g, triangulateAuthoredMesh(await session.getAuthoredMesh(path))); await session.commitGeomNodes(path, g, result, { inputHash: result.inputHash, graphKey: result.key }); return result; };
    let result;
    assert.equal(await commands.execute(sessionCommand(session, 'Geometry Nodes', [path], async () => { const before = session.exportUSDA(); result = await commit(); return before; })), true);
    const usda = session.exportUSDA(), count = result.output.instancer.protoIndices.length;
    assert.ok(count > 0);
    assert.match(usda, /def PointInstancer "Plane_geonodes_instances"/);
    assert.match(usda, /rel prototypes = <\/World\/Plane_geonodes_instances\/Prototypes\/Proto0>/);
    assert.match(usda, /def PointInstancer "Plane_geonodes_instances"\s*\{[^}]*xformOp:translate = \(0, 2, 0\)/, 'instancer inherits the source transform');
    assert.equal(usda.match(/int\[\] protoIndices = \[([^\]]*)\]/)[1].split(',').length, count);
    assert.equal(session.getGeomNodesOutputInfo('/World/Plane_geonodes_instances').source, path);
    assert.match(usda, /def Mesh "Plane_geonodes"/, 'joined source mesh stays a mesh output');
    await commands.undo();
    assert.equal(session.exportUSDA(), original, 'mesh + instancer commit undoes as one step');
    await commit(); await commit();
    assert.equal(session.exportUSDA().match(/def PointInstancer/g).length, 1);
    // Instance-only graph drops the now-empty mesh output.
    const instancesOnly = structuredClone(graph); instancesOnly.links = instancesOnly.links.filter((link) => link.to[0] !== 'join'); instancesOnly.links.push({ from: ['inst', 'instances'], to: ['join', 'a'] });
    await commit(instancesOnly);
    assert.doesNotMatch(session.exportUSDA(), /def Mesh "Plane_geonodes"/);
    // Realizing removes the instancer.
    const realized = structuredClone(graph); realized.nodes.find((node) => node.id === 'out').params = { realizeInstances: true };
    await commit(realized);
    assert.doesNotMatch(session.exportUSDA(), /PointInstancer/);
    await commit();
    await session.removeGeomNodes(path);
    assert.doesNotMatch(session.exportUSDA(), /Plane_geonodes|lucia:geomNodes|visibility/);
  } finally { session.dispose(); }
});
