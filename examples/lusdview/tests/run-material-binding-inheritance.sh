#!/usr/bin/env bash
#
# Two production material regressions, both of which make a scene render as
# untextured flat-shaded soup. Pass the scene to test as $1 (default: the binding
# fixture):
#
#   models/lusdview-material-binding-inheritance.usda
#     Binds its textured material PURPOSE-SCOPED (material:binding:preview / :full)
#     on an ANCESTOR Xform and never authors a plain `material:binding` on the
#     Mesh -- exactly how ALab binds. Reading only the Mesh's own
#     `material:binding` finds nothing and falls back to the default gray material.
#
#   models/nested-look-texture/root.usda
#     References a look layer in `look/` that reaches its texture with
#     `../tex/...`, correct RELATIVE TO THAT LAYER. Anchoring the path at the
#     stage root instead resolves it out of the asset and drops the texture.
#
# Asserts the loader (a) reports a decoded texture and (b) actually SAMPLES it:
# the render must be non-uniform (the checkerboard), not a flat color.
#
# Env: LUSDVIEW=/path/to/lusdview
set -uo pipefail
SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ -n "${LUSDVIEW:-}" ]; then BIN="$LUSDVIEW"
elif [ -x "$REPO_ROOT/build_ninja/lusdview" ]; then BIN="$REPO_ROOT/build_ninja/lusdview"
else BIN="$REPO_ROOT/build/lusdview"; fi
if [ ! -x "$BIN" ]; then echo "SKIP: lusdview not found ($BIN)"; exit "$SKIP"; fi

# The assertion is about GPU texture sampling, so a software-only Vulkan
# installation cannot answer it. Skip only when software is the ONLY option:
# this used to skip whenever llvmpipe/lavapipe was merely PRESENT, which on any
# machine that has Mesa installed alongside a real GPU meant the test never ran
# at all -- it reported "software Vulkan" on a discrete NVIDIA card. The render
# itself is still checked below against the device lusdview actually selected,
# which is the authoritative test.
if command -v vulkaninfo >/dev/null 2>&1 &&
   command -v timeout >/dev/null 2>&1 &&
   timeout 10s vulkaninfo --summary >/tmp/mbi-vulkaninfo.$$ 2>&1; then
  if grep -q 'PHYSICAL_DEVICE_TYPE_CPU' /tmp/mbi-vulkaninfo.$$ &&
     ! grep -Eq 'PHYSICAL_DEVICE_TYPE_(DISCRETE|INTEGRATED|VIRTUAL)_GPU' \
       /tmp/mbi-vulkaninfo.$$; then
    rm -f /tmp/mbi-vulkaninfo.$$
    echo "SKIP: software-only Vulkan cannot validate texture sampling"
    exit "$SKIP"
  fi
  rm -f /tmp/mbi-vulkaninfo.$$
fi

ASSET="${1:-$REPO_ROOT/models/lusdview-material-binding-inheritance.usda}"
if [ ! -f "$ASSET" ]; then echo "SKIP: asset missing: $ASSET"; exit "$SKIP"; fi
echo "scene: $ASSET"

OUT="$(mktemp -d)"
mkdir -p "$OUT/config/lusdview"
trap 'rm -rf "$OUT"' EXIT
# Headless Vulkan takes its offscreen extent from the startup config. Keep this
# texture-sampling regression compact: it checks distinct sampled texels and
# texture sampling, not high-resolution anti-aliasing, and a small extent avoids
# needlessly long two-loader hardware runs.
printf '%s\n' '{"window_size":{"width":640,"height":480}}' \
  >"$OUT/config/lusdview/config.json"
validate_textured_render() {
  local tag="$1"
  local image="$2"
  local log="$3"
  local rc="$4"

  if [ "$rc" -ne 0 ] || [ ! -s "$image" ]; then
  # No GPU / no Vulkan device in this environment -> skip rather than fail.
    if grep -qiE 'no vulkan|failed to (create|find).*(device|instance)|no suitable gpu' "$log"; then
      echo "SKIP: no usable GPU device"; sed -n '1,20p' "$log"; exit "$SKIP"
    fi
    echo "FAIL: $tag render failed (rc=$rc)"; sed -n '1,40p' "$log"; exit 1
  fi

# Mesa's software Vulkan path is useful for API/validation smoke tests but does
# not fetch the viewer's textured vertex attributes reliably, so a flat software
# render cannot distinguish bad asset anchoring from the device limitation. Ask
# the viewer what it selected (caps line) rather than pattern-matching driver
# marketing strings, which is how this check drifted out of date before.
  if grep -Eq 'caps: v1 .*device=cpu' "$log"; then
    echo "SKIP: software Vulkan cannot validate texture sampling"
    exit "$SKIP"
  fi

# (a) The texture must survive material resolution + path anchoring, and decode.
  if ! grep -qE '[0-9]+ materials, [1-9][0-9]* textures' "$log"; then
    echo "FAIL: $tag resolved no texture -- the material binding was dropped, or the"
    echo "      texture's asset path was anchored at the stage root instead of at"
    echo "      the layer that authored it."
    grep -oE '[0-9]+ materials, [0-9]+ textures' "$log" || true
    exit 1
  fi

# (b) The texture must actually be SAMPLED: a flat fallback material renders a
#     uniform surface, the checkerboard does not.
  python3 - "$image" <<'PY'
import sys

data = open(sys.argv[1], "rb").read()
if not data.startswith(b"P6"):
    print("FAIL: not a binary PPM"); sys.exit(1)

# Parse the P6 header (whitespace-separated, '#' comments allowed).
i, tok = 2, []
while len(tok) < 3 and i < len(data):
    c = data[i]
    if c == 35:  # '#'
        while i < len(data) and data[i] not in (10, 13):
            i += 1
    elif chr(c).isspace():
        i += 1
    else:
        s = i
        while i < len(data) and not chr(data[i]).isspace():
            i += 1
        tok.append(int(data[s:i]))
w, h, _maxv = tok
i += 1  # single whitespace byte after maxval
px = data[i:]

# Collect the distinct colors of the rendered (non-background) surface.
colors = {}
for y in range(0, h, 2):
    for x in range(0, w, 2):
        o = (y * w + x) * 3
        if o + 2 >= len(px):
            continue
        c = (px[o], px[o + 1], px[o + 2])
        colors[c] = colors.get(c, 0) + 1

# The checkerboard gives many distinct shades across the quad; a dropped texture
# (default gray / flat diffuse) gives essentially background + one flat color.
surface = [c for c, n in colors.items() if n >= 8]
if len(surface) < 3:
    print(f"FAIL: render is flat ({len(surface)} dominant colors) -- "
          f"texture not sampled: {sorted(colors.items(), key=lambda kv: -kv[1])[:4]}")
    sys.exit(1)
print(f"OK: textured render, {len(surface)} dominant surface colors")
PY
  [ "$?" -eq 0 ] || exit 1
  grep -oE '[0-9]+ materials, [0-9]+ textures' "$log" || true
}

for tag in core; do
  image="$OUT/$tag.ppm"
  log="$OUT/$tag.log"
  loader_args=()
  if command -v timeout >/dev/null 2>&1; then
    timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-60s}" \
      env XDG_CONFIG_HOME="$OUT/config" \
      "$BIN" --headless --frames 4 --screenshot "$image" "${loader_args[@]}" "$ASSET" \
      >"$log" 2>&1
  else
    env XDG_CONFIG_HOME="$OUT/config" \
    "$BIN" --headless --frames 4 --screenshot "$image" "${loader_args[@]}" "$ASSET" \
      >"$log" 2>&1
  fi
  validate_textured_render "$tag" "$image" "$log" "$?"
done

echo "PASS: core loader resolves and samples the material texture"
exit 0
