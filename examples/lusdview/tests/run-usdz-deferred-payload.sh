#!/usr/bin/env bash
# Exercise both lusdview loaders' deferred USDZ payload recomposition through MCP.
# Exit 77 when the required headless Vulkan/Node/zip environment is unavailable.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDVIEW="${LUSDVIEW:-$REPO_ROOT/build/lusdview}"

if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW"
  exit $SKIP
fi
if ! command -v node >/dev/null 2>&1 || ! command -v zip >/dev/null 2>&1; then
  echo "SKIP: node and zip are required for the deferred USDZ payload test"
  exit $SKIP
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CONFIG="$TMP/config.json"
cat > "$CONFIG" <<'JSON'
{"window_size":{"width":160,"height":160}}
JSON

ARCHIVE="$TMP/archive"
mkdir -p "$ARCHIVE"
cat > "$ARCHIVE/root.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "World"
)
def Xform "World" (
    prepend payload = @payload.usda@</Payload>
)
{
}
USD
cat > "$ARCHIVE/payload.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "Payload"
)
def Xform "Payload"
{
    def Mesh "Triangle"
    {
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        uniform token subdivisionScheme = "none"
    }
}
USD

PACKAGE="$TMP/deferred-payload.usdz"
(cd "$ARCHIVE" && zip -0 -q "$PACKAGE" root.usda payload.usda)

# Distinguish an unavailable Vulkan runtime from a composition regression.
PROBE_LOG="$TMP/probe.log"
if ! "$LUSDVIEW" --config "$CONFIG" --headless --backend vk --legacy-load \
     --frames 1 "$ARCHIVE/root.usda" >"$PROBE_LOG" 2>&1; then
  echo "SKIP: headless Vulkan is unavailable"
  exit $SKIP
fi

run_case() {
  local loader="$1"
  node - "$LUSDVIEW" "$CONFIG" "$PACKAGE" "$loader" <<'NODE'
const {spawn} = require('child_process');

const viewer = process.argv[2];
const config = process.argv[3];
const packagePath = process.argv[4];
const loader = process.argv[5];
const loaderArg = loader === 'legacy' ? '--legacy-load' : '--next';
const child = spawn(viewer, [
  '--config', config, '--headless', '--backend', 'vk', loaderArg,
  '--defer-payloads', '--frames', '100000', '--mcp-stdio', packagePath
], {stdio: ['pipe', 'pipe', 'pipe']});

let stdout = '';
let stderr = '';
let nextId = 1;
const pending = new Map();

child.stderr.on('data', data => {
  stderr += data.toString();
  if (stderr.length > 65536) stderr = stderr.slice(-65536);
});
child.stdout.on('data', data => {
  stdout += data.toString();
  for (;;) {
    const nl = stdout.indexOf('\n');
    if (nl < 0) break;
    const line = stdout.slice(0, nl);
    stdout = stdout.slice(nl + 1);
    if (!line) continue;
    let response;
    try { response = JSON.parse(line); } catch (_) { continue; }
    const waiter = pending.get(response.id);
    if (!waiter) continue;
    pending.delete(response.id);
    clearTimeout(waiter.timer);
    if (response.error) waiter.reject(new Error(JSON.stringify(response.error)));
    else waiter.resolve(response.result.structuredContent);
  }
});

function call(name, args = {}) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`MCP ${name} timed out`));
    }, 15000);
    pending.set(id, {resolve, reject, timer});
    child.stdin.write(JSON.stringify({
      jsonrpc: '2.0', id, method: 'tools/call',
      params: {name, arguments: args}
    }) + '\n');
  });
}

function stop() {
  if (child.exitCode === null) child.kill('SIGTERM');
}

child.on('exit', (code, signal) => {
  for (const [id, waiter] of pending) {
    clearTimeout(waiter.timer);
    waiter.reject(new Error(
      `lusdview exited before MCP response (code=${code}, signal=${signal})\n${stderr}`));
    pending.delete(id);
  }
});

(async () => {
  const initial = await call('get_scene_info');
  if (!initial.loaded || !initial.composed || initial.deferred_payload_count !== 1) {
    throw new Error(`unexpected deferred scene: ${JSON.stringify(initial)}`);
  }
  if (initial.deferred_payloads.length !== 1 ||
      initial.deferred_payloads[0].prim !== '/World' ||
      initial.deferred_payloads[0].arc !== 'payload') {
    throw new Error(`unexpected deferred paths: ${JSON.stringify(initial.deferred_payloads)}`);
  }
  if (loader === 'legacy' && initial.triangle_count !== 0) {
    throw new Error(`legacy payload geometry was not deferred: ${JSON.stringify(initial)}`);
  }
  // The next loader visualizes a deferred payload with a bounded proxy cube;
  // require that marker rather than confusing it with eagerly loaded content.
  if (loader === 'next' &&
      (initial.triangle_count !== 12 || !initial.truncated)) {
    throw new Error(`next deferred-payload proxy is missing: ${JSON.stringify(initial)}`);
  }

  const started = await call('load_payloads');
  if (!started.started) {
    throw new Error(`payload recomposition did not start: ${JSON.stringify(started)}`);
  }

  let lastInfo = initial;
  for (let attempt = 0; attempt < 120; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 25));
    const info = await call('get_scene_info');
    lastInfo = info;
    if (info.loaded && info.deferred_payload_count === 0 &&
        info.triangle_count === 1) {
      console.log(`PASS: ${loader} deferred package payload resolved to one triangle`);
      stop();
      return;
    }
  }
  throw new Error(`package payload did not finish recomposing: ${JSON.stringify(lastInfo)}`);
})().catch(error => {
  console.error(error.message);
  if (stderr) console.error(stderr);
  stop();
  process.exitCode = 1;
});
NODE
}

run_case legacy || exit $?
run_case next
