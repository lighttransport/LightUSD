#!/usr/bin/env bash
#
# `proxy` and `render` are ALTERNATIVES in USD, not two things to draw: a proxy
# is a cheap stand-in for a render subtree. Drawing both puts the stand-in on
# top of the geometry it stands in for -- in intent-vfx's simpleAsset a proxy
# Cube (size 2) exactly encloses the render Sphere (radius 1) and hid it
# completely.
#
# This asset reproduces that layout (purpose authored on Scopes, as real assets
# do). Both loaders must draw ONLY the render sphere. The proxy-only variant
# below pins the other half of the rule: with no render geometry to supersede
# it, the proxy still draws -- which is the whole point of authoring one.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LUSDVIEW="${1:-${LUSDVIEW:-$REPO_ROOT/build/lusdview}}"

if [ ! -x "$LUSDVIEW" ]; then
  echo "SKIP: lusdview binary not found at $LUSDVIEW"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/both.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "asset"
    upAxis = "Y"
)

def Xform "asset" (kind = "component")
{
    def Scope "geo"
    {
        def Scope "proxy"
        {
            uniform token purpose = "proxy"
            def Cube "shape" {}
        }

        def Scope "render"
        {
            uniform token purpose = "render"
            def Sphere "shape" {}
        }
    }
}
USD

# The supersede must be scoped to the ASSET that authored both alternatives, not
# to any shared ancestor. Here two unrelated models sit side by side: one is
# proxy-only, the other render-only. Nothing supersedes the proxy -- it is the
# only representation its model has -- so all 2 meshes must draw. Apple's
# stage_composition/purpose.usda has exactly this shape, and a shared-ancestor
# rule silently deletes the proxy cube.
cat > "$TMP/unrelated.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Scope "World" (kind = "group")
{
    def Xform "ProxyOnlyAsset" (kind = "component")
    {
        uniform token purpose = "proxy"
        double3 xformOp:translate = (4, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        def Cube "shape" {}
    }

    def Xform "RenderOnlyAsset" (kind = "component")
    {
        uniform token purpose = "render"
        def Sphere "shape" {}
    }
}
USD

cat > "$TMP/proxy-only.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "asset"
    upAxis = "Y"
)

def Xform "asset" (kind = "component")
{
    def Scope "geo"
    {
        def Scope "proxy"
        {
            uniform token purpose = "proxy"
            def Cube "shape" {}
        }
    }
}
USD

# Renders headless and echoes the "render stats: meshes N/M visible" count.
drawn_meshes() {
  local asset="$1" loader="$2"
  local log="$TMP/$(basename "$asset" .usda)$loader.log"
  local args=(--headless --size 64x64 --frames 2)
  [ -n "$loader" ] && args+=("$loader")
  if ! timeout 120 "$LUSDVIEW" "${args[@]}" "$asset" >"$log" 2>&1; then
    echo "FAIL: lusdview exited nonzero on $asset $loader" >&2
    cat "$log" >&2
    return 1
  fi
  sed -n 's/.*render stats: meshes \([0-9]*\)\/.*/\1/p' "$log" | tail -1
}

status=0
for loader in "--core"; do
  name="${loader:---core}"

  got="$(drawn_meshes "$TMP/both.usda" "$loader")" || { status=1; continue; }
  if [ "$got" != "1" ]; then
    echo "FAIL[$name]: proxy+render asset drew '$got' meshes, expected 1"
    echo "       (the proxy Cube must be superseded by the render Sphere)"
    status=1
  else
    echo "ok[$name]: proxy superseded by render geometry"
  fi

  got="$(drawn_meshes "$TMP/proxy-only.usda" "$loader")" || { status=1; continue; }
  if [ "$got" != "1" ]; then
    echo "FAIL[$name]: proxy-only asset drew '$got' meshes, expected 1"
    echo "       (with no render geometry the proxy must still draw)"
    status=1
  else
    echo "ok[$name]: proxy-only asset still draws its proxy"
  fi

  got="$(drawn_meshes "$TMP/unrelated.usda" "$loader")" || { status=1; continue; }
  if [ "$got" != "2" ]; then
    echo "FAIL[$name]: unrelated proxy-only + render-only models drew '$got' meshes, expected 2"
    echo "       (a render model must not supersede a DIFFERENT model's proxy)"
    status=1
  else
    echo "ok[$name]: supersede stays scoped to the model that authored both"
  fi
done

exit "$status"
