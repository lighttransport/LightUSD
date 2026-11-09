#!/usr/bin/env bash
set -euo pipefail

SKIP=77
LUSDVIEW="${LUSDVIEW:-build_ninja/lusdview}"
BACKEND="${BACKEND:-vk}"
command -v node >/dev/null 2>&1 || { echo "SKIP: node unavailable"; exit "$SKIP"; }
[ -x "$LUSDVIEW" ] || { echo "SKIP: lusdview unavailable"; exit "$SKIP"; }
if [ "$BACKEND" = gl ] && ! xdpyinfo >/dev/null 2>&1; then
  echo "SKIP: no usable X display"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat >"$TMP/config.json" <<'JSON'
{}
JSON
cat >"$TMP/root.usda" <<'USDA'
#usda 1.0
(
    subLayers = [@./geometry.usda@]
)
USDA
cat >"$TMP/geometry.usda" <<'USDA'
#usda 1.0
def Xform "World" {
    def Mesh "Shape" {
        uniform token subdivisionScheme = "none"
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
    def PointInstancer "Instances" {
        def Mesh "Prototype" {
            uniform token subdivisionScheme = "none"
            point3f[] points = [(0, 0, 0), (0.5, 0, 0), (0, 0.5, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
        }
        rel prototypes = </World/Instances/Prototype>
        int[] protoIndices = [0, 0]
        point3f[] positions = [(0, 0, 0), (2, 0, 0)]
        color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0)] (
            interpolation = "vertex"
        )
        float[] primvars:displayOpacity = [1, 1] (
            interpolation = "vertex"
        )
    }
}
USDA

if ! "$LUSDVIEW" --config "$TMP/config.json" --headless --backend "$BACKEND" --next \
     --frames 1 "$TMP/root.usda" >"$TMP/probe.log" 2>&1; then
  echo "SKIP: headless Vulkan is unavailable"
  exit "$SKIP"
fi

node - "$LUSDVIEW" "$TMP/config.json" "$TMP/root.usda" "$TMP/geometry.usda" "$BACKEND" <<'NODE'
const fs = require('fs');
const {spawn} = require('child_process');
const child = spawn(process.argv[2], [
  '--config', process.argv[3], '--headless', '--backend', process.argv[6], '--next',
  '--frames', '100000', '--mcp-stdio', process.argv[4]
], {stdio: ['pipe', 'pipe', 'pipe']});
let stdout = '', stderr = '', id = 1;
const pending = new Map();
child.stderr.on('data', data => { stderr += data.toString(); });
child.stdout.on('data', data => {
  stdout += data.toString();
  for (;;) {
    const newline = stdout.indexOf('\n');
    if (newline < 0) break;
    const line = stdout.slice(0, newline); stdout = stdout.slice(newline + 1);
    let response;
    try { response = JSON.parse(line); } catch (_) { continue; }
    const waiter = pending.get(response.id);
    if (!waiter) continue;
    pending.delete(response.id); clearTimeout(waiter.timer);
    response.error ? waiter.reject(new Error(JSON.stringify(response.error)))
                   : waiter.resolve(response.result.structuredContent);
  }
});
function call(name, args = {}) {
  const callId = id++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${name} timed out\n${stderr}`)), 15000);
    pending.set(callId, {resolve, reject, timer});
    child.stdin.write(JSON.stringify({jsonrpc:'2.0', id:callId,
      method:'tools/call', params:{name, arguments:args}}) + '\n');
  });
}
function stop() { if (child.exitCode === null) child.kill('SIGTERM'); }
(async () => {
  const initial = await call('get_scene_info');
  if (!initial.loaded || initial.mesh_count !== 2 || initial.stage_revision < 1)
    throw new Error(`unexpected initial scene: ${JSON.stringify(initial)}\n${stderr}`);
  const stageInfo = await call('stage_info');
  if (!stageInfo.loaded || stageInfo.primCount !== 4 || stageInfo.upAxis !== 'Y')
    throw new Error(`public stage metadata mismatch: ${JSON.stringify(stageInfo)}`);
  const limited = await call('list_prims', {max: 1});
  if (limited.count !== 1 || limited.paths[0] !== '/World')
    throw new Error(`public prim-list limit/order mismatch: ${JSON.stringify(limited)}`);
  const subtree = await call('prim_list', {path: '/World/Instances'});
  if (subtree.count !== 2 || subtree.prims[0].path !== '/World/Instances' ||
      subtree.prims[1].path !== '/World/Instances/Prototype')
    throw new Error(`public subtree query mismatch: ${JSON.stringify(subtree)}`);
  const meshes = await call('query_prims_by_type', {type: 'Mesh'});
  if (meshes.count !== 2 || meshes.prims.some(p => p.type !== 'Mesh' || !p.active))
    throw new Error(`public type query mismatch: ${JSON.stringify(meshes)}`);
  const search = await call('search', {query: 'Prototype'});
  if (search.count !== 1 || search.prims[0].name !== 'Prototype')
    throw new Error(`public search mismatch: ${JSON.stringify(search)}`);
  const dependency = initial.layer_dependencies.find(
    path => path.endsWith('/geometry.usda') || path.endsWith('\\geometry.usda'));
  if (!dependency)
    throw new Error(`geometry dependency was not reported: ${JSON.stringify(initial)}`);
  fs.writeFileSync(process.argv[5], `#usda 1.0
def Xform "World" {
    def Mesh "Shape" {
        uniform token subdivisionScheme = "none"
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
    def PointInstancer "Instances" {
        def Mesh "Prototype" {
            uniform token subdivisionScheme = "none"
            point3f[] points = [(0, 0, 0), (0.5, 0, 0), (0, 0.5, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
        }
        rel prototypes = </World/Instances/Prototype>
        int[] protoIndices = [0, 0]
        point3f[] positions = [(0, 0, 0), (3, 0, 0)]
        color3f[] primvars:displayColor = [(0, 0, 1), (1, 1, 0)] (
            interpolation = "vertex"
        )
        float[] primvars:displayOpacity = [1, 1] (
            interpolation = "vertex"
        )
    }
}
`);
  const reload = await call('reload_layer', {path: dependency});
  if (!reload.started) throw new Error(`layer reload did not start: ${JSON.stringify(reload)}`);
  let info = initial;
  for (let attempt = 0; attempt < 200; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    info = await call('get_scene_info');
    if (!info.loading && info.stage_revision > initial.stage_revision) break;
  }
  if (info.stage_revision <= initial.stage_revision)
    throw new Error(`reloaded revision was not published: ${JSON.stringify(info)}\n${stderr}`);
  const expected = 'incremental scene update: retained 2/2 mesh GPU slots; replaced 0 meshes, updated 0 vertex buffers, 0 transforms, 1 instance buffers, 0 materials, and 0 textures';
  if (!stderr.includes(expected))
    throw new Error(`dependency reload did not retain the renderer slot\n${stderr}`);
  if (!stderr.includes('RenderSession prepared revision 2 off-thread') ||
      stderr.includes('RenderSession commit rejected'))
    throw new Error(`dependency reload did not use the prepared RenderSession transaction\n${stderr}`);

  fs.writeFileSync(process.argv[5], `#usda 1.0
def Xform "World" {
    def Mesh "Shape" {
        uniform token subdivisionScheme = "none"
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
    def PointInstancer "Instances" {
        def Mesh "Prototype" {
            uniform token subdivisionScheme = "none"
            point3f[] points = [(0, 0, 0), (0.5, 0, 0), (0, 0.5, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
        }
        rel prototypes = </World/Instances/Prototype>
        int[] protoIndices = [0, 0, 0]
        point3f[] positions = [(0, 0, 0), (3, 0, 0), (1.5, 1.5, 0)]
        color3f[] primvars:displayColor = [(0, 0, 1), (1, 1, 0), (1, 0, 1)] (
            interpolation = "vertex"
        )
        float[] primvars:displayOpacity = [1, 1, 1] (
            interpolation = "vertex"
        )
    }
}
`);
  const priorRevision = info.stage_revision;
  const countReload = await call('reload_layer', {path: dependency});
  if (!countReload.started)
    throw new Error(`instance-count reload did not start: ${JSON.stringify(countReload)}`);
  for (let attempt = 0; attempt < 200; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    info = await call('get_scene_info');
    if (!info.loading && info.stage_revision > priorRevision) break;
  }
  if (info.stage_revision <= priorRevision)
    throw new Error(`instance-count revision was not published: ${JSON.stringify(info)}\n${stderr}`);
  const replaced = 'incremental scene update: retained 1/2 mesh GPU slots; replaced 1 meshes, updated 0 vertex buffers, 0 transforms, 0 instance buffers, 0 materials, and 0 textures';
  if (!stderr.includes(replaced))
    throw new Error(`instance-count reload did not replace only its stable slot\n${stderr}`);

  fs.writeFileSync(process.argv[5], fs.readFileSync(process.argv[5], 'utf8')
    .replace('float[] primvars:displayOpacity = [1, 1, 1]',
             'float[] primvars:displayOpacity = [0.7, 0.4, 0.8]'));
  const countRevision = info.stage_revision;
  const opacityReload = await call('reload_layer', {path: dependency});
  if (!opacityReload.started)
    throw new Error(`opacity reload did not start: ${JSON.stringify(opacityReload)}`);
  for (let attempt = 0; attempt < 200; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    info = await call('get_scene_info');
    if (!info.loading && info.stage_revision > countRevision) break;
  }
  if (info.stage_revision <= countRevision)
    throw new Error(`opacity revision was not published: ${JSON.stringify(info)}\n${stderr}`);
  if (stderr.split(replaced).length < 3)
    throw new Error(`opaque-to-translucent reload did not replace only its stable slot\n${stderr}`);
  console.log('PASS: dependency reload retained stable GPU slots across instance data, count, and opacity-class edits');
  stop();
})().catch(error => { console.error(error.message); stop(); process.exitCode = 1; });
NODE
