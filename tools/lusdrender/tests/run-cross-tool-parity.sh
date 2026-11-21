#!/usr/bin/env bash
#
# Cross-tool parity oracle: lusdview and lusdrender consume the SAME tydra
# RenderScene, so they must interpret it the same way. Nothing else in the suite
# renders one scene through both binaries and compares -- which is exactly how a
# string of lusdview-only material/UV regressions stayed invisible while every
# per-tool test kept passing.
#
# The fixture (models/parity-material-uv-subset.usda) is four coplanar quads in a
# row, framed head-on, each face bound to a different material through a
# `materialBind` GeomSubset:  red | green | blue | (texture read via `uvSet1`).
# The uvSet1 crop lands inside the texture's yellow quadrant, so a correct render
# is four saturated bands: R G B Y.
#
# Asserts:
#   1. lusdview   renders the four bands as r g b y  (GeomSubset materials + uv1)
#   2. lusdrender renders the four bands as r g b y
#   3. the two AGREE region-by-region, within tolerance
#
# Each band is a different lens on the same class of bug:
#   * a constant-color material graying out        -> bands 1-3 go gray
#   * GeomSubset bindings not applied              -> the bands collapse to one color
#   * the secondary UV set never reaching the shader -> band 4 goes flat-but-wrong
#     (a corner texel, or the averaged mip) instead of yellow
#   * a texture routed to uv0 instead of uv1       -> band 4 shows all four quadrants
#
# lusdview is driven headless (Vulkan only); lusdrender uses its -rtPreview next
# path. Exits 77 (skip) when a binary, the fixture, or a GPU is unavailable.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

LUSDRENDER="${LUSDRENDER:-$REPO_ROOT/build/tools/lusdrender/lusdrender}"
LUSDVIEW="${LUSDVIEW:-$REPO_ROOT/build/lusdview}"
MODEL="$REPO_ROOT/models/parity-material-uv-subset.usda"

[ -x "$LUSDRENDER" ] || { echo "SKIP: lusdrender not found at $LUSDRENDER"; exit $SKIP; }
[ -x "$LUSDVIEW" ]   || { echo "SKIP: lusdview not found at $LUSDVIEW";     exit $SKIP; }
[ -f "$MODEL" ]      || { echo "SKIP: fixture $MODEL missing";              exit $SKIP; }
[ -f "$REPO_ROOT/models/textures/parity-quadrants.png" ] || {
  echo "SKIP: fixture texture missing"; exit $SKIP; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# lusdview --headless is windowless Vulkan: it needs a real Vulkan DEVICE but no
# display, so there is no DISPLAY/Xvfb dance here. Without a device the render
# fails and we skip -- a software stack cannot answer this question.
if ! timeout 300 "$LUSDVIEW" --headless --backend vk --core --frames 3 \
        --camera Cam --screenshot "$TMP/lusdview.ppm" "$MODEL" \
        > "$TMP/lusdview.log" 2>&1; then
  echo "SKIP: lusdview headless render failed (no Vulkan device?)"
  sed -n '1,20p' "$TMP/lusdview.log"
  exit $SKIP
fi
[ -s "$TMP/lusdview.ppm" ] || { echo "SKIP: lusdview produced no screenshot"; exit $SKIP; }

if ! timeout 300 "$LUSDRENDER" "$MODEL" "$TMP/lusdrender.png" -rtPreview \
        -camera Cam -w 320 -height 320 -samples 4 > "$TMP/lusdrender.log" 2>&1; then
  echo "FAIL: lusdrender render failed"
  sed -n '1,20p' "$TMP/lusdrender.log"
  exit 1
fi

python3 - "$TMP/lusdview.ppm" "$TMP/lusdrender.png" <<'PY'
import re
import struct
import sys
import zlib

EXPECT = ["r", "g", "b", "y"]
# Region means must agree across the tools within this (0-255, per channel). The
# two use different shading models, so it is deliberately loose: it is here to
# catch a band landing on the WRONG COLOR, not to pin exact shading.
MAX_CHANNEL_DELTA = 70
# How saturated a band must be to count as a color rather than gray/background.
MIN_SAT = 45


def load(path):
    """Read a P6 .ppm or an RGB(A)8 .png -> (w, h, nch, pixels)."""
    if path.endswith(".ppm"):
        d = open(path, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
        if not m:
            return None
        return int(m.group(1)), int(m.group(2)), 3, d[m.end():]

    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, w, h, color = 8, b"", 0, 0, 2
    while pos + 8 <= len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ, body = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, color = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    if color not in (2, 6):
        return None
    nch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    prev = bytearray(stride)
    out = bytearray()
    p = 0
    for _y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                c = prev[i - nch] if i >= nch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        prev = line
        out += line
    return w, h, nch, out


def columns(img):
    """Mean RGB per column, averaged over a band around mid-height."""
    w, h, nch, d = img
    ys = list(range(h // 2 - h // 10, h // 2 + h // 10, max(1, h // 40)))
    cols = []
    for x in range(w):
        r = g = b = 0
        for y in ys:
            o = (y * w + x) * nch
            r += d[o]
            g += d[o + 1]
            b += d[o + 2]
        n = len(ys)
        cols.append((r // n, g // n, b // n))
    return cols


def bands(img, tag):
    """Locate the strip by SATURATION -- lusdview draws a gray ground grid, so a
    brightness threshold would swallow the whole frame -- then split it into 4."""
    cols = columns(img)
    sat = [max(c) - min(c) for c in cols]
    lit = [x for x, s in enumerate(sat) if s > MIN_SAT]
    if not lit:
        print(f"FAIL [{tag}]: no saturated geometry in the frame -- the strip did "
              f"not render, or every band came out gray/white.")
        sys.exit(1)
    x0, x1 = lit[0], lit[-1]
    if x1 - x0 < 8:
        print(f"FAIL [{tag}]: the strip spans only {x1 - x0}px -- framing is wrong.")
        sys.exit(1)

    out = []
    for k in range(4):
        a = x0 + (x1 - x0) * k // 4
        z = x0 + (x1 - x0) * (k + 1) // 4
        # Trim the band edges: the seam between two quads blends their materials.
        pad = max(1, (z - a) // 5)
        a, z = a + pad, z - pad
        rs = gs = bs = 0
        for x in range(a, z):
            c = cols[x]
            rs += c[0]
            gs += c[1]
            bs += c[2]
        n = max(1, z - a)
        out.append((rs // n, gs // n, bs // n))
    return out


def label(c):
    r, g, b = c
    mx, mn = max(c), min(c)
    if mx - mn < MIN_SAT:
        return "gray" if mx > 60 else "dark"
    if r == mx and g > 100 and b < g - 40 and b < r - 40:
        return "y"                       # yellow: red AND green high, blue low
    if r == mx:
        return "r"
    if g == mx:
        return "g"
    return "b"


tv = load(sys.argv[1])
tr = load(sys.argv[2])
if tv is None or tr is None:
    print("FAIL: could not read the renders back")
    sys.exit(1)

tv_b = bands(tv, "lusdview")
tr_b = bands(tr, "lusdrender")
tv_l = [label(c) for c in tv_b]
tr_l = [label(c) for c in tr_b]

print(f"lusdview   bands: {' '.join(tv_l)}  {tv_b}")
print(f"lusdrender bands: {' '.join(tr_l)}  {tr_b}")

rc = 0
for tag, got in (("lusdview", tv_l), ("lusdrender", tr_l)):
    if got != EXPECT:
        print(f"FAIL [{tag}]: bands are {' '.join(got)}, expected {' '.join(EXPECT)}. "
              f"Bands 1-3 are GeomSubset-bound constant-color materials; band 4 is a "
              f"texture cropped through `uvSet1` onto the yellow quadrant. A gray "
              f"band means a constant material lost its color; a band 4 that is "
              f"flat-but-not-yellow means the secondary UV set never reached the "
              f"shader; a band 4 showing several colors means it sampled uv0.")
        rc = 1

if rc == 0:
    for i in range(4):
        d = max(abs(a - b) for a, b in zip(tv_b[i], tr_b[i]))
        if d > MAX_CHANNEL_DELTA:
            print(f"FAIL: band {i + 1} disagrees across the tools by {d} "
                  f"(lusdview {tv_b[i]} vs lusdrender {tr_b[i]}, max "
                  f"{MAX_CHANNEL_DELTA}). The two are reading the same scene "
                  f"differently.")
            rc = 1

if rc == 0:
    print(f"PASS: lusdview and lusdrender agree -- r g b y, region means within "
          f"{MAX_CHANNEL_DELTA}")
sys.exit(rc)
PY
exit $?
