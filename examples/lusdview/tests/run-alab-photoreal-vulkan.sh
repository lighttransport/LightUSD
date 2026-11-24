#!/usr/bin/env bash
# External ALab quality smoke test. No ALab/HDR data or rendered output enters
# the repository; callers provide the two public-asset locations.
set -uo pipefail
export LUSDVIEW_LOG=debug

SKIP=77
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN="${LUSDVIEW:-$ROOT/build_ninja/lusdview}"
ALAB_GENERATED="${ALAB_GENERATED:-}"
HDR="${LUSDVIEW_HDR:-}"

if [ ! -x "$BIN" ]; then echo "SKIP: lusdview not found: $BIN"; exit "$SKIP"; fi
if [ -z "$ALAB_GENERATED" ] || [ ! -d "$ALAB_GENERATED/ALab" ]; then
  echo "SKIP: set ALAB_GENERATED to the ALab generated folder"; exit "$SKIP"
fi
if [ -z "$HDR" ] || [ ! -f "$HDR" ]; then
  echo "SKIP: set LUSDVIEW_HDR to an HDR/EXR environment"; exit "$SKIP"
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
LANTERN="$ALAB_GENERATED/alab-static-lantern.usda"
STOAT="$ALAB_GENERATED/alab-character-stoat.usda"
OPAQUE="$ALAB_GENERATED/ALab/entity/decor_clip01/decor_clip01.usda"
for asset in "$LANTERN" "$STOAT"; do
  if [ ! -f "$asset" ]; then echo "SKIP: missing $asset"; exit "$SKIP"; fi
done
if [ ! -f "$OPAQUE" ]; then echo "SKIP: missing $OPAQUE"; exit "$SKIP"; fi

render() {
  local tag="$1" asset="$2" quality="$3"
  timeout --kill-after=10s "${LUSDVIEW_ALAB_TIMEOUT:-240s}" \
    env LUSDVIEW_TIME_GPU=1 "$BIN" --headless --backend vk --core \
      --load-payloads --texture-fit never --frames 4 --size 1920x1080 \
      --no-grid --raster-quality "$quality" --envmap "$HDR" \
      --envmap-intensity "${LUSDVIEW_HDR_INTENSITY:-1}" \
      --screenshot "$OUT/$tag.ppm" "$asset" >"$OUT/$tag.log" 2>&1
  local rc=$?
  if [ "$rc" -ne 0 ] || [ ! -s "$OUT/$tag.ppm" ]; then
    if grep -qiE 'no vulkan|no suitable gpu|failed to create.*device' "$OUT/$tag.log"; then
      echo "SKIP: no usable Vulkan GPU"; exit "$SKIP"
    fi
    echo "FAIL: $tag render failed ($rc)"; sed -n '1,80p' "$OUT/$tag.log"; exit 1
  fi
}

render lantern-current "$LANTERN" current
render lantern-high "$LANTERN" high
render stoat-high "$STOAT" high

# Keep an inexpensive opaque RT sentinel separate from Lantern's glass and
# subdivision complexity, then exercise the complete Lantern at authored
# resolution. Hardware ray-query may deliberately fall back to compute-BVH on
# drivers whose cold pipeline compilation is unsafe; both are Vulkan RT paths.
render_rt() {
  local tag="$1" asset="$2"; shift 2
  timeout --kill-after=10s "${LUSDVIEW_ALAB_TIMEOUT:-240s}" \
    "$BIN" --headless --backend vk --core --load-payloads --rt \
      --frames 1 --size 640x640 --no-grid --envmap "$HDR" \
      --envmap-intensity "${LUSDVIEW_HDR_INTENSITY:-1}" "$@" \
      --screenshot "$OUT/$tag.ppm" "$asset" >"$OUT/$tag.log" 2>&1
  test -s "$OUT/$tag.ppm" || {
    echo "FAIL: $tag Vulkan RT render failed"; sed -n '1,100p' "$OUT/$tag.log"; exit 1;
  }
  grep -q 'Vulkan ray tracing enabled' "$OUT/$tag.log" || {
    echo "FAIL: $tag did not enable Vulkan RT"; exit 1;
  }
}
render_rt opaque-rt "$OPAQUE" --subdivision-level 0 --no-subdivision-auto
render_rt lantern-rt-nosubd "$LANTERN" --subdivision-level 0 --no-subdivision-auto

grep -qE 'drawn tris [1-9][0-9]*' "$OUT/opaque-rt.log" || {
  echo "FAIL: opaque RT sentinel drew no geometry"; exit 1;
}
awk '/core: .*unique tris/ { for (i=1;i<=NF;i++) if ($(i+1)=="unique" && $i+0>0 && $i+0<100000) ok=1 }
     END { exit ok ? 0 : 1 }' "$OUT/lantern-rt-nosubd.log" || {
  echo "FAIL: Lantern no-subdivision RT did not retain lightweight topology"; exit 1;
}

grep -qE '4 materials, 7 textures' "$OUT/lantern-high.log" || {
  echo "FAIL: Lantern did not select the full-purpose PBR texture set"; exit 1;
}
grep -q 'runtime dome IBL ready' "$OUT/lantern-high.log" || {
  echo "FAIL: runtime HDR environment was not baked"; exit 1;
}
grep -q 'high-quality screen-space refraction ready' "$OUT/lantern-high.log" || {
  echo "FAIL: high-quality weighted-OIT scene-color refraction was not ready"; exit 1;
}

python3 - "$OUT/lantern-current.ppm" "$OUT/lantern-high.ppm" <<'PY'
import sys

def pixels(path):
    data = open(path, 'rb').read()
    if not data.startswith(b'P6'):
        raise SystemExit(f'FAIL: {path} is not P6')
    i, tokens = 2, []
    while len(tokens) < 3:
        if data[i] == 35:
            while data[i] not in (10, 13): i += 1
        elif chr(data[i]).isspace(): i += 1
        else:
            start = i
            while not chr(data[i]).isspace(): i += 1
            tokens.append(int(data[start:i]))
    return data[i + 1:]

a, b = pixels(sys.argv[1]), pixels(sys.argv[2])
if len(a) != len(b): raise SystemExit('FAIL: image dimensions differ')
mad = sum(abs(x-y) for x, y in zip(a, b)) / len(a)
if mad < 1.0: raise SystemExit(f'FAIL: high-quality path had no visual effect (MAD {mad:.3f})')
print(f'PASS: full PBR/HDR high-quality output differs from current path (MAD {mad:.3f})')
PY

grep -h '\[gpu\].*total=' "$OUT"/*.log | tail -n 6 || true
echo "PASS: ALab Lantern and Stoat Vulkan high-quality smoke"
