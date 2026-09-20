#!/usr/bin/env bash
set -euo pipefail

SKIP=77
LUSDVIEW="${LUSDVIEW:-build_ninja/lusdview}"
command -v node >/dev/null 2>&1 || { echo "SKIP: node unavailable"; exit "$SKIP"; }
[ -x "$LUSDVIEW" ] || { echo "SKIP: lusdview unavailable"; exit "$SKIP"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SCENE="$TMP/topology.usda"
CONFIG="$TMP/config.json"
cat >"$CONFIG" <<'JSON'
{}
JSON
cat >"$SCENE" <<'USDA'
#usda 1.0
def Xform "Model" (
    append variantSets = ["shape", "look"]
    variants = {
        string shape = "Triangle"
        string look = "Red"
    }
) {
    def Mesh "Keep" {
        rel material:binding = </Model/Mat>
        double3 xformOp:translate.timeSamples = {
            0: (0, 0, 0),
            1: (0, 0, 0)
        }
        uniform token[] xformOpOrder = ["xformOp:translate"]
        point3f[] points = [(0, 0, 1), (1, 0, 1), (0, 1, 1)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
    variantSet "shape" = {
        "Triangle" {
            def Mesh "Changing" {
                rel material:binding = </Model/Mat>
                point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
            }
        }
        "Quad" {
            def Mesh "Changing" {
                rel material:binding = </Model/Mat>
                point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
                int[] faceVertexCounts = [4]
                int[] faceVertexIndices = [0, 1, 2, 3]
            }
            def Mesh "Added" {
                rel material:binding = </Model/Mat>
                double3 xformOp:translate.timeSamples = {
                    0: (0, 0, 0),
                    1: (0, 0, 0)
                }
                uniform token[] xformOpOrder = ["xformOp:translate"]
                point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 1, 0)]
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
            }
        }
    }
    variantSet "look" = {
        "Red" {
            def Material "Mat" {
                token outputs:surface.connect = </Model/Mat/Preview.outputs:surface>
                def Shader "Preview" {
                    uniform token info:id = "UsdPreviewSurface"
                    color3f inputs:diffuseColor = (1, 0, 0)
                    token outputs:surface
                }
            }
        }
        "Blue" {
            def Material "Mat" {
                token outputs:surface.connect = </Model/Mat/Preview.outputs:surface>
                def Shader "Preview" {
                    uniform token info:id = "UsdPreviewSurface"
                    color3f inputs:diffuseColor = (0, 0, 1)
                    token outputs:surface
                }
            }
        }
    }
}
USDA

if ! "$LUSDVIEW" --config "$CONFIG" --headless --backend vk --next \
     --frames 1 "$SCENE" >"$TMP/probe.log" 2>&1; then
  echo "SKIP: headless Vulkan is unavailable"
  exit "$SKIP"
fi

node - "$LUSDVIEW" "$CONFIG" "$SCENE" <<'NODE'
const {spawn} = require('child_process');
const child = spawn(process.argv[2], [
  '--config', process.argv[3], '--headless', '--backend', 'vk', '--next',
  '--frames', '100000', '--mcp-stdio', process.argv[4]
], {stdio: ['pipe', 'pipe', 'pipe']});
let stdout = '', stderr = '', id = 1;
const pending = new Map();
child.stderr.on('data', d => { stderr += d.toString(); });
child.stdout.on('data', d => {
  stdout += d.toString();
  for (;;) {
    const nl = stdout.indexOf('\n');
    if (nl < 0) break;
    const line = stdout.slice(0, nl); stdout = stdout.slice(nl + 1);
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
  if (!initial.loaded || initial.mesh_count !== 2 || initial.triangle_count <= 0)
    throw new Error(`unexpected initial scene: ${JSON.stringify(initial)}`);
  const edit = await call('variant_set_selection', {
    path:'/Model', variant_set:'shape', variant:'Quad'
  });
  if (!edit.started) throw new Error(`variant edit did not start: ${JSON.stringify(edit)}`);
  let info = initial;
  for (let attempt = 0; attempt < 160; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    info = await call('get_scene_info');
    if (info.loaded && info.mesh_count === 3 &&
        info.triangle_count !== initial.triangle_count) break;
  }
  if (info.triangle_count === initial.triangle_count)
    throw new Error(`variant topology did not publish: ${JSON.stringify(info)}\n${stderr}`);
  if (!stderr.includes('incremental scene update: retained 1/3 mesh GPU slots; replaced 2 meshes'))
    throw new Error(`incremental topology transaction was not used\n${stderr}`);
  if (!stderr.includes('RenderSession prepared revision 2 off-thread') ||
      stderr.includes('RenderSession commit rejected'))
    throw new Error(`topology edit did not use the prepared RenderSession transaction\n${stderr}`);
  const revert = await call('variant_set_selection', {
    path:'/Model', variant_set:'shape', variant:'Triangle'
  });
  if (!revert.started) throw new Error(`variant revert did not start: ${JSON.stringify(revert)}`);
  for (let attempt = 0; attempt < 160; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    info = await call('get_scene_info');
    if (info.loaded && info.triangle_count === initial.triangle_count) break;
  }
  if (info.triangle_count !== initial.triangle_count ||
      stderr.split('incremental scene update: retained 1/3 mesh GPU slots; replaced 2 meshes').length < 3)
    throw new Error(`incremental structural removal was not used\n${stderr}`);
  const materialEdit = await call('variant_set_selection', {
    path:'/Model', variant_set:'look', variant:'Blue'
  });
  if (!materialEdit.started)
    throw new Error(`material variant edit did not start: ${JSON.stringify(materialEdit)}`);
  for (let attempt = 0; attempt < 160; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    await call('get_scene_info');
    if (stderr.includes('and 1 materials')) break;
  }
  if (!stderr.includes('incremental scene update: retained 3/3 mesh GPU slots; replaced 0 meshes, updated 0 vertex buffers, 0 transforms, 0 instance buffers, 1 materials, and 0 textures'))
    throw new Error(`incremental material transaction was not used\n${stderr}`);
  console.log('PASS: Vulkan retained slots across topology, addition, removal, and material edits');
  stop();
})().catch(error => { console.error(error.message); stop(); process.exitCode = 1; });
NODE
