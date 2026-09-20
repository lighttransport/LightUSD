#!/usr/bin/env bash
set -euo pipefail

SKIP=77
LUSDVIEW="${LUSDVIEW:-build_ninja/lusdview}"
BACKEND="${BACKEND:-vk}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
command -v node >/dev/null 2>&1 || { echo "SKIP: node unavailable"; exit "$SKIP"; }
[ -x "$LUSDVIEW" ] || { echo "SKIP: lusdview unavailable"; exit "$SKIP"; }
if [ "$BACKEND" = gl ] && ! xdpyinfo >/dev/null 2>&1; then
  echo "SKIP: no usable X display"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp "$ROOT/models/textures/parity-quadrants.png" "$TMP/red.png"
cp "$ROOT/models/textures/checkerboard.png" "$TMP/blue.png"
cat >"$TMP/config.json" <<'JSON'
{}
JSON
cat >"$TMP/texture.usda" <<'USDA'
#usda 1.0
def Xform "Model" (
    append variantSets = "look"
    variants = { string look = "Red" }
) {
    def Mesh "Quad" {
        rel material:binding = </Model/Mat>
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
    }
    variantSet "look" = {
        "Red" {
            def Material "Mat" {
                token outputs:surface.connect = </Model/Mat/Preview.outputs:surface>
                def Shader "Preview" {
                    uniform token info:id = "UsdPreviewSurface"
                    color3f inputs:diffuseColor = (0.18, 0.18, 0.18)
                    color3f inputs:diffuseColor.connect = </Model/Mat/Tex.outputs:rgb>
                    token outputs:surface
                }
                def Shader "Tex" {
                    uniform token info:id = "UsdUVTexture"
                    asset inputs:file = @red.png@
                    token inputs:sourceColorSpace = "sRGB"
                    float2 inputs:st.connect = </Model/Mat/St.outputs:result>
                    float3 outputs:rgb
                }
                def Shader "St" {
                    uniform token info:id = "UsdPrimvarReader_float2"
                    token inputs:varname = "st"
                    float2 outputs:result
                }
            }
        }
        "Blue" {
            def Material "Mat" {
                token outputs:surface.connect = </Model/Mat/Preview.outputs:surface>
                def Shader "Preview" {
                    uniform token info:id = "UsdPreviewSurface"
                    color3f inputs:diffuseColor = (0.18, 0.18, 0.18)
                    color3f inputs:diffuseColor.connect = </Model/Mat/Tex.outputs:rgb>
                    token outputs:surface
                }
                def Shader "Tex" {
                    uniform token info:id = "UsdUVTexture"
                    asset inputs:file = @blue.png@
                    token inputs:sourceColorSpace = "sRGB"
                    float2 inputs:st.connect = </Model/Mat/St.outputs:result>
                    float3 outputs:rgb
                }
                def Shader "St" {
                    uniform token info:id = "UsdPrimvarReader_float2"
                    token inputs:varname = "st"
                    float2 outputs:result
                }
            }
        }
    }
}
USDA

VIEW_ARGS=(--config "$TMP/config.json" --backend "$BACKEND" --next --frames 1)
[ "$BACKEND" = vk ] && VIEW_ARGS+=(--headless)
if ! "$LUSDVIEW" "${VIEW_ARGS[@]}" "$TMP/texture.usda" >"$TMP/probe.log" 2>&1; then
  echo "SKIP: $BACKEND renderer is unavailable"
  cat "$TMP/probe.log"
  exit "$SKIP"
fi

BACKEND="$BACKEND" node - "$LUSDVIEW" "$TMP/config.json" "$TMP/texture.usda" <<'NODE'
const {spawn} = require('child_process');
const backend = process.env.BACKEND || 'vk';
const args = ['--config', process.argv[3], '--backend', backend, '--next',
  '--frames', '100000', '--mcp-stdio', process.argv[4]];
if (backend === 'vk') args.splice(2, 0, '--headless');
const child = spawn(process.argv[2], args, {stdio: ['pipe', 'pipe', 'pipe']});
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
  if (!initial.loaded || initial.mesh_count !== 1)
    throw new Error(`unexpected initial scene: ${JSON.stringify(initial)}\n${stderr}`);
  const edit = await call('variant_set_selection', {
    path:'/Model', variant_set:'look', variant:'Blue'
  });
  if (!edit.started) throw new Error(`texture variant edit did not start: ${JSON.stringify(edit)}`);
  for (let attempt = 0; attempt < 200; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    await call('get_scene_info');
    if (stderr.includes('and 1 textures')) break;
  }
  const expected = 'incremental scene update: retained 1/1 mesh GPU slots; replaced 0 meshes, updated 0 vertex buffers, 0 transforms, 0 instance buffers, 0 materials, and 1 textures';
  if (!stderr.includes(expected))
    throw new Error(`incremental texture transaction was not used\n${stderr}`);
  if (!stderr.includes('RenderSession prepared revision 2 off-thread') ||
      stderr.includes('RenderSession commit rejected'))
    throw new Error(`texture edit did not use the prepared RenderSession transaction\n${stderr}`);
  console.log(`PASS: ${backend} replaced one texture while retaining mesh and material slots`);
  stop();
})().catch(error => { console.error(error.message); stop(); process.exitCode = 1; });
NODE
