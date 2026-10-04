import assert from 'node:assert/strict';

// Sculpt and Geometry Nodes through the real UI: synthesized pointer strokes,
// commits through runMutation, undo, and USDZ export of the procedural output.
export async function runToolsBrowserChecks(page) {
  const results = await page.evaluate(async () => {
    const { app } = await import('/lucia-code/src/main.js');
    const { validateUSDZArchive } = await import('/lucia-code/src/usd-doctor.js');
    const check = (condition, message) => { if (!condition) throw new Error(message); };
    const until = async (predicate, message, timeout = 30000) => { const start = performance.now(); while (!(await predicate())) { if (performance.now() - start > timeout) throw new Error(message); await new Promise((resolve) => setTimeout(resolve, 50)); } };
    const n = 21, points = [], faces = [];
    for (let j = 0; j < n; j++) for (let i = 0; i < n; i++) points.push(`(${(i / (n - 1) * 2 - 1).toFixed(3)}, 0, ${(j / (n - 1) * 2 - 1).toFixed(3)})`);
    for (let j = 0; j < n - 1; j++) for (let i = 0; i < n - 1; i++) { const a = j * n + i; faces.push(a, a + n, a + n + 1, a + 1); }
    const source = `#usda 1.0\n(defaultPrim = "World" upAxis = "Y")\ndef Xform "World" {\n def Mesh "Plane" {\n  point3f[] points = [${points.join(', ')}]\n  int[] faceVertexCounts = [${faces.map(() => 4).slice(0, faces.length / 4).join(', ')}]\n  int[] faceVertexIndices = [${faces.join(', ')}]\n  uniform token subdivisionScheme = "none"\n }\n}\n`;
    await app.openFile(new File([source], 'tools.usda'));
    const path = '/World/Plane', before = await app.session.getMeshPoints(path);
    app.select(path); app.currentInspectorTab = 'sculpt'; app.renderInspector();
    await app.$('#sculpt-toggle').onclick();
    check(app.sculpt.active && app.$('[data-sculpt-brush="draw"]'), 'Sculpt mode did not activate');
    app.sculpt.setSettings({ radius: 0.3, strength: 1, symmetry: [true, false, false] });
    const canvas = app.bridge.renderer.domElement, box = canvas.getBoundingClientRect(), camera = app.bridge.camera;
    camera.position.set(0, 4, 0.01); camera.lookAt(0, 0, 0); app.bridge.controls.target.set(0, 0, 0); camera.updateMatrixWorld();
    const screen = (x, z) => { const v = camera.position.clone().set(x, 0, z).project(camera); return { clientX: box.left + (v.x + 1) / 2 * box.width, clientY: box.top + (1 - v.y) / 2 * box.height }; };
    const fire = (target, type, at) => target.dispatchEvent(new PointerEvent(type, { ...at, button: 0, buttons: type === 'pointerup' ? 0 : 1, pointerId: 1, pointerType: 'mouse', pressure: 0.5, bubbles: true }));
    const undoBefore = app.commands.undoItems.length;
    fire(canvas, 'pointerdown', screen(-0.4, -0.3));
    for (let k = 1; k <= 6; k++) fire(window, 'pointermove', screen(-0.4, -0.3 + k * 0.1));
    fire(window, 'pointerup', screen(-0.4, 0.3));
    await until(() => app.commands.undoItems.length === undoBefore + 1, 'Sculpt stroke did not commit exactly one undo step');
    await until(() => !app.activity.active, 'Sculpt commit did not finish');
    const sculpted = await app.session.getMeshPoints(path);
    check(sculpted.length === before.length, 'Sculpt changed topology');
    let maxY = 0, mirrored = 0; for (let i = 0; i < sculpted.length; i += 3) { maxY = Math.max(maxY, sculpted[i + 1]); }
    const index = (x, z) => (Math.round((z + 1) / 2 * (n - 1)) * n + Math.round((x + 1) / 2 * (n - 1))) * 3 + 1;
    mirrored = Math.abs(sculpted[index(-0.4, 0)] - sculpted[index(0.4, 0)]);
    check(maxY > 0.01, `Sculpt stroke did not raise the surface (max ${maxY})`);
    check(mirrored < 1e-4, `X symmetry was not applied: ${sculpted[index(-0.4, 0)]} vs ${sculpted[index(0.4, 0)]} max ${maxY}`);
    await app.commands.undo(); await app.refreshAll(false);
    check((await app.session.getMeshPoints(path)).every((value, i) => value === before[i]), 'Undo did not restore sculpted points');
    app.sculpt.exit();
    app.currentInspectorTab = 'nodes'; app.renderInspector();
    await app.$('#geonodes-edit').onclick();
    check(app.$('#geonodes-editor') && app.geoNodes.path === path, 'Geometry node editor did not open');
    app.geoNodes.addNode('Subdivide');
    const sub = app.geoNodes.graph.nodes.find((node) => node.type === 'Subdivide').id;
    check(app.geoNodes.edit((graph) => { graph.links = [{ from: ['input', 'geometry'], to: [sub, 'geometry'] }, { from: [sub, 'geometry'], to: ['output', 'geometry'] }]; }), 'Graph edit rejected');
    check(app.$$('#geonodes-links path').length === 2, 'Links were not drawn');
    await until(() => app.bridge.geoNodesPreview, 'Live preview did not appear');
    check(!app.geoNodes.edit((graph) => { graph.links.push({ from: [sub, 'geometry'], to: ['output', 'geometry'] }); }) && /more than one link/.test(app.$('#geonodes-status').textContent), 'Invalid edit was not reported');
    await app.$('#geonodes-commit').onclick();
    await until(() => !app.activity.active && !app.$('#geonodes-editor'), 'Geometry node commit did not finish');
    const usda = app.session.exportUSDA();
    check(/def Mesh "Plane_geonodes"/.test(usda) && /lucia:geomNodes/.test(usda) && /lucia:geomNodesSource/.test(usda), 'Procedural output was not authored');
    const outputVertices = (await app.session.getMeshPoints('/World/Plane_geonodes')).length / 3;
    check(outputVertices > before.length / 3, 'Subdivide did not add vertices');
    validateUSDZArchive(app.session.exportUSDZ(app.project.assets));
    app.select('/World/Plane_geonodes'); app.renderInspector();
    await app.commands.undo(); await app.refreshAll(false);
    check(!/Plane_geonodes|lucia:geomNodes/.test(app.session.exportUSDA()), 'Undo did not remove the procedural output');
    return { sculptMaxY: maxY, outputVertices };
  });
  console.log('Lucia sculpt + geometry nodes browser:', JSON.stringify(results));
  assert.ok(results.outputVertices > 0);
}
