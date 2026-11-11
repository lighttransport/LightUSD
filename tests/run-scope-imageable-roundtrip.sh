#!/usr/bin/env bash
#
# Scope is a UsdGeomImageable: it carries `visibility` and `purpose`, and
# authoring purpose on a Scope is the standard way to ship an asset's render and
# proxy representations side by side.
#
# Both are parsed into TYPED fields rather than the generic `props` map, so
# every writer has to emit them explicitly. Neither did: `visibility` was parsed
# and then silently dropped by the USDA printer AND the crate writer (Scope is
# not a GPrim, so ExtractGPrimProperties skipped it, and stage-converter had no
# Scope case at all). A file round-tripped through lusdcat came back with the
# attribute simply gone -- silent data loss.
#
# This pins both directions: usda -> usda and usda -> usdc -> usda.
set -uo pipefail

SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LUSDCAT="${1:-${LUSDCAT:-$REPO_ROOT/build/lusdcat}}"
# Core lusdcat needs --rewrite-layer for a compose-free layer round-trip
# (USDC output is otherwise rejected); legacy lusdcat writes the layer as-is.
# Detect core by its "loaded: N prims" summary. Override via LUSDCAT_LAYER_ARGS.
if [ -z "${LUSDCAT_LAYER_ARGS+x}" ]; then
  LUSDCAT_LAYER_ARGS=""
  _probe=$(mktemp --suffix=.usda); printf '#usda 1.0\n' > "$_probe"
  if "$LUSDCAT" "$_probe" 2>/dev/null | head -1 | grep -q '^loaded:'; then
    LUSDCAT_LAYER_ARGS="--rewrite-layer --openusd-compat"
  fi
  rm -f "$_probe"
fi


if [ ! -x "$LUSDCAT" ]; then
  echo "SKIP: lusdcat binary not found at $LUSDCAT"
  exit "$SKIP"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/scope.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "World"
)

def Scope "World"
{
    token visibility = "invisible"
    uniform token purpose = "proxy"

    def Cube "shape"
    {
    }
}
USD

status=0

# Every authored attribute must survive; `shape` guards against the whole prim
# being dropped rather than just the attributes.
check() {
  local label="$1" file="$2"
  local out="$TMP/$label.usda"
  if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS "$file" > "$out" 2>"$TMP/$label.err"; then
    echo "FAIL[$label]: lusdcat exited nonzero"
    cat "$TMP/$label.err"
    status=1
    return
  fi
  for expect in 'purpose = "proxy"' 'visibility = "invisible"' 'def Cube "shape"'; do
    if ! grep -qF "$expect" "$out"; then
      echo "FAIL[$label]: lost '$expect' on round-trip"
      echo "--- got ---"
      cat "$out"
      status=1
      return
    fi
  done
  echo "ok[$label]: Scope visibility + purpose survived"
}

check usda-to-usda "$TMP/scope.usda"

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/scope.usdc" "$TMP/scope.usda" \
     >"$TMP/write.log" 2>&1; then
  echo "FAIL: lusdcat could not write usdc"
  cat "$TMP/write.log"
  exit 1
fi
check usdc-to-usda "$TMP/scope.usdc"

# Scope was one instance of a class: `visibility` / `purpose` are TYPED fields on
# every imageable, so each writer must emit them, and several forgot. All the
# lights and Volume lost BOTH through the crate writer; Material and NodeGraph
# lost `purpose` through both writers. Sweep every imageable prim type so a new
# one cannot quietly start dropping them again.
TYPES="Xform Scope Mesh Sphere Cube Cylinder Cone Capsule Plane Points BasisCurves
NurbsCurves PointInstancer Camera SkelRoot Skeleton Material NodeGraph SphereLight
DistantLight RectLight DiskLight DomeLight Volume GeomSubset Model"

{
  echo '#usda 1.0'
  echo '('
  echo '    defaultPrim = "W"'
  echo ')'
  echo
  echo 'def Xform "W"'
  echo '{'
  for t in $TYPES; do
    echo "    def $t \"P$t\""
    echo '    {'
    echo '        token visibility = "invisible"'
    echo '        uniform token purpose = "render"'
    echo '    }'
  done
  echo '}'
} > "$TMP/imageable.usda"

# Reports every prim that came back missing either attribute.
sweep() {
  local label="$1" file="$2"
  local out="$TMP/sweep-$label.usda"
  if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS "$file" > "$out" 2>/dev/null; then
    echo "FAIL[$label]: lusdcat exited nonzero on the imageable sweep"
    status=1
    return
  fi
  local lost=""
  for t in $TYPES; do
    # The prim's body runs until the next `def` or the closing brace.
    local body
    body="$(sed -n "/def $t \"P$t\"/,/^    }/p" "$out")"
    case "$body" in
      *'purpose = "render"'*) ;;
      *) lost="$lost $t:purpose" ;;
    esac
    case "$body" in
      *'visibility = "invisible"'*) ;;
      *) lost="$lost $t:visibility" ;;
    esac
  done
  if [ -n "$lost" ]; then
    echo "FAIL[$label]: dropped on round-trip:$lost"
    status=1
  else
    echo "ok[$label]: all imageable prim types kept visibility + purpose"
  fi
}

sweep all-types-usda "$TMP/imageable.usda"

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/imageable.usdc" "$TMP/imageable.usda" \
     >"$TMP/write2.log" 2>&1; then
  echo "FAIL: lusdcat could not write the imageable sweep to usdc"
  cat "$TMP/write2.log"
  exit 1
fi
sweep all-types-usdc "$TMP/imageable.usdc"

# Lights are Xformable as well as imageable, and NO light extractor wrote
# xformOps -- so a scene round-tripped through .usdc came back with every light
# at the world origin. intensity/color/exposure were copy-pasted per light type
# and half of them omitted `exposure`; RectLight never wrote its texture.
cat > "$TMP/light.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def Xform "W"
{
    def RectLight "Key"
    {
        float inputs:exposure = 1.5
        float inputs:intensity = 900
        asset inputs:texture:file = @key.png@
        double3 xformOp:translate = (5, 3, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def DomeLight "Sky"
    {
        float inputs:exposure = 0.5
        asset inputs:texture:file = @sky.hdr@
        token inputs:texture:format = "latlong"
        double3 xformOp:translate = (0, 10, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/light.usdc" "$TMP/light.usda" \
     >"$TMP/write3.log" 2>&1; then
  echo "FAIL: lusdcat could not write the light scene to usdc"
  cat "$TMP/write3.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/light.usdc" > "$TMP/light-rt.usda" 2>/dev/null
lost=""
for expect in \
  'xformOp:translate = (5, 3, 0)' \
  'xformOp:translate = (0, 10, 0)' \
  'inputs:exposure = 1.5' \
  'inputs:exposure = 0.5' \
  'inputs:texture:file = @key.png@' \
  'inputs:texture:file = @sky.hdr@' \
  'inputs:texture:format = "latlong"'; do
  grep -qF "$expect" "$TMP/light-rt.usda" || lost="$lost
    $expect"
done
if [ -n "$lost" ]; then
  echo "FAIL[lights-usdc]: dropped on round-trip:$lost"
  status=1
else
  echo "ok[lights-usdc]: light transforms, exposure and textures survived"
fi

# A genuinely typeless prim (`def "bora"`, no typeName authored at all) was
# invented a typeName of "Model" by the crate writer: the writer's "infer a
# schema name for in-memory-built prims" fallback (stage-converter.cc) read
# the internal C++ label of the catch-all `Model` struct that backs typeless
# prims and treated it as if it were an authored/inferable typeName. Cover
# `def`/`over`/`class` -- the same fallback backs all three specifiers.
cat > "$TMP/typeless.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def "W"
{
    over "child"
    {
    }
}

class "TheClass"
{
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/typeless.usdc" "$TMP/typeless.usda" \
     >"$TMP/write4.log" 2>&1; then
  echo "FAIL: lusdcat could not write the typeless-prim scene to usdc"
  cat "$TMP/write4.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/typeless.usdc" > "$TMP/typeless-rt.usda" 2>/dev/null
lost=""
for expect in 'def "W"' 'over "child"' 'class "TheClass"'; do
  grep -qF "$expect" "$TMP/typeless-rt.usda" || lost="$lost
    $expect"
done
if [ -n "$lost" ]; then
  echo "FAIL[typeless-usdc]: typeless prim gained an invented typeName:$lost"
  echo "--- got ---"
  cat "$TMP/typeless-rt.usda"
  status=1
else
  echo "ok[typeless-usdc]: typeless def/over/class stayed typeless"
fi

# Camera's `shutter:open` / `shutter:close` are namespaced in the schema
# (usdGeom.hh, prim-property-tables.hh), unlike this Camera's other
# attributes, which are all plain names. The crate writer wrote them under
# the plain names `shutterOpen`/`shutterClose` instead -- a property name the
# reader never looks for -- so a round-trip came back as if they were never
# authored at all (the printer then fell back to the schema default of 0.0
# for both, silently changing an authored motion-blur interval).
cat > "$TMP/camera.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def Camera "W"
{
    double shutter:open = -0.25
    double shutter:close = 0.25
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/camera.usdc" "$TMP/camera.usda" \
     >"$TMP/write5.log" 2>&1; then
  echo "FAIL: lusdcat could not write the camera scene to usdc"
  cat "$TMP/write5.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/camera.usdc" > "$TMP/camera-rt.usda" 2>/dev/null
lost=""
for expect in 'shutter:open = -0.25' 'shutter:close = 0.25'; do
  grep -qF "$expect" "$TMP/camera-rt.usda" || lost="$lost
    $expect"
done
if [ -n "$lost" ]; then
  echo "FAIL[camera-shutter-usdc]: dropped/renamed on round-trip:$lost"
  echo "--- got ---"
  cat "$TMP/camera-rt.usda"
  status=1
else
  echo "ok[camera-shutter-usdc]: Camera shutter:open/shutter:close survived"
fi

# apiSchemas list-ops: the crate writer rebuilt a single ListOp from the
# RESOLVED view (APISchemas::names/unknownSchemas), which can only ever be one
# explicit-or-prepend op. That silently dropped every `delete apiSchemas`
# (Omniverse/Newton-asset pattern: `prepend` + `delete` on one prim), dropped
# `apiSchemas = None` entirely (both resolved vectors are empty, so the old
# code wrote nothing -- not even an explicit empty list), and reordered
# `prepend` items because known (`names`) and unknown (`unknownSchemas`)
# schemas live in separate vectors that got concatenated back in the wrong
# order. Covers all three in one round-trip.
cat > "$TMP/apischemas.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def Xform "W"
{
    def Xform "deleteAndPrepend" (
        prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsArticulationRootAPI"]
        delete apiSchemas = ["PhysicsArticulationRootAPI"]
    )
    {
    }

    def Xform "explicitNone" (
        apiSchemas = None
    )
    {
    }

    def Xform "preserveOrder" (
        prepend apiSchemas = ["HoudiniViewportGuideAPI", "GeomModelAPI", "MotionAPI"]
    )
    {
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/apischemas.usdc" "$TMP/apischemas.usda" \
     >"$TMP/write6.log" 2>&1; then
  echo "FAIL: lusdcat could not write the apiSchemas scene to usdc"
  cat "$TMP/write6.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/apischemas.usdc" > "$TMP/apischemas-rt.usda" 2>/dev/null
lost=""
for expect in \
  'delete apiSchemas = ["PhysicsArticulationRootAPI"]' \
  'prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsMassAPI", "PhysicsArticulationRootAPI"]' \
  'apiSchemas = None' \
  'prepend apiSchemas = ["HoudiniViewportGuideAPI", "GeomModelAPI", "MotionAPI"]'; do
  grep -qF "$expect" "$TMP/apischemas-rt.usda" || lost="$lost
    $expect"
done
if [ -n "$lost" ]; then
  echo "FAIL[apischemas-usdc]: dropped/reordered on round-trip:$lost"
  echo "--- got ---"
  cat "$TMP/apischemas-rt.usda"
  status=1
else
  echo "ok[apischemas-usdc]: delete+prepend, explicit None, and prepend order survived"
fi

# Relationships: several distinct properties were dropped by
# ConvertRelationshipToFields (src/stage-converter.cc) because it never
# received the enclosing Property's `custom` flag, hardcoded `variability` to
# Uniform instead of consulting Relationship::is_varying_authored(), and never
# wrote `bindMaterialAs` metadata at all (only ConvertAttributeToFields did).
# Separately, GPrim's `proxyPrim` was parsed into a typed field and never
# re-emitted (same class of bug as the earlier Scope visibility/purpose fix),
# and the collection-based material-binding writer
# (mat_binding->materialBindingCollectionMap() in sconv-geom.cc) built
# `material:binding:collection:<name>:<purpose>` when a purpose is present,
# the wrong order -- it must be `<purpose>:<name>`.
cat > "$TMP/relationships.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def "W"
{
    varying rel myrel = </W>
    append custom varying rel myrel2 = </W>

    rel material:binding = </W> (
        bindMaterialAs = "strongerThanDescendants"
    )
}

def Xform "hasProxy"
{
    rel proxyPrim = </W>

    # material:binding:collection:<name>[:<purpose>] only goes through the
    # typed MaterialBinding map (the buggy code path) on a schema type that
    # inherits it -- a typeless prim stores these as generic named
    # relationships and would pass even with the writer bug present.
    rel material:binding:collection:beauty = </W>
    rel material:binding:collection:mypurpose:beauty = </W>
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/relationships.usdc" "$TMP/relationships.usda" \
     >"$TMP/write7.log" 2>&1; then
  echo "FAIL: lusdcat could not write the relationships scene to usdc"
  cat "$TMP/write7.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/relationships.usdc" > "$TMP/relationships-rt.usda" 2>/dev/null
lost=""
for expect in \
  'varying rel myrel = </W>' \
  'bindMaterialAs = "strongerThanDescendants"' \
  'rel proxyPrim = </W>' \
  'material:binding:collection:beauty = </W>' \
  'material:binding:collection:mypurpose:beauty = </W>'; do
  grep -qF "$expect" "$TMP/relationships-rt.usda" || lost="$lost
    $expect"
done
# custom + list-op: legacy spells `append custom varying rel r = <..>`, core
# and pxr spell a `custom varying rel r` declaration + `append varying rel r`.
grep -qF 'append custom varying rel myrel2 = </W>' "$TMP/relationships-rt.usda" ||
  { grep -qF 'custom varying rel myrel2' "$TMP/relationships-rt.usda" &&
    grep -qF 'append varying rel myrel2 = </W>' "$TMP/relationships-rt.usda"; } ||
  lost="$lost
    append custom varying rel myrel2 = </W>"

if [ -n "$lost" ]; then
  echo "FAIL[relationships-usdc]: dropped/altered on round-trip:$lost"
  echo "--- got ---"
  cat "$TMP/relationships-rt.usda"
  status=1
else
  echo "ok[relationships-usdc]: varying/custom, bindMaterialAs, proxyPrim, and collection-binding order survived"
fi

# Variant statement metadata: a variant carries its own PrimMeta block
# (`active`, `hidden`, `kind`, and `variantSets` when it nests a variantSet),
# populated by the readers exactly like a Prim's. ConvertVariantToFields
# (src/stage-converter.cc) wrote only the `specifier` field and never called
# ExtractPrimMeta, so every one of those dropped on write.
cat > "$TMP/variant-meta.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "V"
)

def Xform "V" (
    variants = {
        string geo = "a"
    }
    prepend variantSets = "geo"
)
{
    variantSet "geo" = {
        "a" (
            active = true
            hidden = true
            kind = "component"
        ) {
            def Capsule "Inner"
            {
            }
        }

        "b" (
            prepend variantSets = "sub"
        ) {
            variantSet "sub" = {
                "x" {
                }
            }
        }
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/variant-meta.usdc" "$TMP/variant-meta.usda" \
     >"$TMP/write8.log" 2>&1; then
  echo "FAIL: lusdcat could not write the variant-meta scene to usdc"
  cat "$TMP/write8.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/variant-meta.usdc" > "$TMP/variant-meta-rt.usda" 2>/dev/null
lost=""
for expect in \
  'active = true' \
  'hidden = true' \
  'kind = "component"' \
  'variantSets = "sub"'; do
  grep -qF "$expect" "$TMP/variant-meta-rt.usda" || lost="$lost
    $expect"
done
if [ -n "$lost" ]; then
  echo "FAIL[variant-meta-usdc]: variant statement metadata dropped on round-trip:$lost"
  echo "--- got ---"
  cat "$TMP/variant-meta-rt.usda"
  status=1
else
  echo "ok[variant-meta-usdc]: variant active/hidden/kind/variantSets survived"
fi

# Shader input connections must not be BAKED DOWN TO CONSTANTS. A shader input
# is `authored` when it is declared, whether or not it carries a value -- and
# TypedAttributeWithFallback::get_value() silently returns the SCHEMA FALLBACK
# when no value was authored. Every typed input in sconv-shader.cc except
# inputs:st / inputs:file / inputs:in / inputs:normal read get_value() without
# first checking is_value_empty(), and never passed its connections through, so
# a connection-only input (`token inputs:wrapS.connect = </...>`) was written as
# the fallback CONSTANT ("useMetadata") and the connection was lost -- silently
# rewriting the asset's shading network rather than dropping inert metadata.
#
# Separately, a Material terminal is a TypedConnection whose has_value() IS its
# connection count, so a DECLARED-but-unconnected `token outputs:surface` was
# dropped entirely by AddMaterialOutputSpecs.
cat > "$TMP/shader-connect.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "mat"
)

def Material "mat"
{
    token outputs:surface

    def Shader "pbr"
    {
        uniform token info:id = "UsdPreviewSurface"
        int inputs:useSpecularWorkflowDriver = 1
        token inputs:wrapSDriver = "repeat"
        token inputs:sourceColorSpace = "raw"
        int inputs:useSpecularWorkflow.connect = </mat/pbr.inputs:useSpecularWorkflowDriver>
    }

    def Shader "tex"
    {
        uniform token info:id = "UsdUVTexture"
        token inputs:wrapS.connect = </mat/pbr.inputs:wrapSDriver>
        token inputs:sourceColorSpace.connect = </mat/pbr.inputs:sourceColorSpace>
    }

    def Shader "xf"
    {
        uniform token info:id = "UsdTransform2d"
        float inputs:rotationDriver = 45
        float inputs:rotation.connect = </mat/xf.inputs:rotationDriver>
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/shader-connect.usdc" "$TMP/shader-connect.usda" \
     >"$TMP/write9.log" 2>&1; then
  echo "FAIL: lusdcat could not write the shader-connect scene to usdc"
  cat "$TMP/write9.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/shader-connect.usdc" > "$TMP/shader-connect-rt.usda" 2>/dev/null
lost=""
for expect in \
  'int inputs:useSpecularWorkflow.connect = </mat/pbr.inputs:useSpecularWorkflowDriver>' \
  'token inputs:wrapS.connect = </mat/pbr.inputs:wrapSDriver>' \
  'token inputs:sourceColorSpace.connect = </mat/pbr.inputs:sourceColorSpace>' \
  'float inputs:rotation.connect = </mat/xf.inputs:rotationDriver>' \
  'token outputs:surface'; do
  grep -qF "$expect" "$TMP/shader-connect-rt.usda" || lost="$lost
    $expect"
done
# The connections must not have been replaced by their schema fallbacks.
for baked in \
  'inputs:useSpecularWorkflow = 0' \
  'inputs:wrapS = "useMetadata"' \
  'inputs:sourceColorSpace = "auto"' \
  'inputs:rotation = 0'; do
  if grep -qF "$baked" "$TMP/shader-connect-rt.usda"; then
    lost="$lost
    BAKED TO CONSTANT: $baked"
  fi
done
if [ -n "$lost" ]; then
  echo "FAIL[shader-connect-usdc]: connections lost or baked to constants:$lost"
  echo "--- got ---"
  cat "$TMP/shader-connect-rt.usda"
  status=1
else
  echo "ok[shader-connect-usdc]: shader input connections and the declared-but-unconnected Material terminal survived"
fi

# UNAUTHORED attributes must not be INVENTED on write. A TypedAttributeWithFallback
# always yields a value from get_value() -- the schema fallback when nothing was
# authored -- so any writer that emits it unconditionally (or gates on has_value(),
# which for that type returns !_empty and is therefore true for an UNAUTHORED attr)
# turns "no opinion" into an AUTHORED opinion on read-back. That is not cosmetic:
# an authored opinion is a STRONG one and blocks weaker opinions during
# composition, so a scene that composed one way before the round-trip composes
# differently after it.
#
# This bit: Skeleton/SkelRoot visibility+purpose (sconv-skel.cc gated on
# has_value()), GeomSubset elementType (written unconditionally), and everything
# behind the EXTRACT_FALLBACK / EXTRACT_TOKEN_FALLBACK macros -- physics
# (invertFilteredGroups) and media (mediaOffset, gain) among them.
#
# Also pinned here: a Mesh's blendShapes is the NAMESPACED UsdSkelBindingAPI
# attribute `skel:blendShapes`; the Mesh writer emitted it unprefixed. (A
# SkelAnimation's own `blendShapes` IS unprefixed -- both spellings are correct,
# on different prim types, so this guards against "fixing" the wrong one.)
cat > "$TMP/unauthored.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def Xform "W"
{
    def SkelRoot "R"
    {
        def Skeleton "S"
        {
        }

        def SkelAnimation "A"
        {
            uniform token[] blendShapes = ["a", "b"]
        }

        def Mesh "M"
        {
            uniform token[] skel:blendShapes = ["a", "b"]

            def GeomSubset "sub"
            {
            }
        }
    }

    def SpatialAudio "Sound"
    {
    }

    def PhysicsCollisionGroup "CG"
    {
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/unauthored.usdc" "$TMP/unauthored.usda" \
     >"$TMP/write10.log" 2>&1; then
  echo "FAIL: lusdcat could not write the unauthored-attrs scene to usdc"
  cat "$TMP/write10.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/unauthored.usdc" > "$TMP/unauthored-rt.usda" 2>/dev/null
bad=""
# Nothing below was authored, so none of it may come back.
for invented in \
  'token visibility' \
  'uniform token purpose' \
  'uniform token elementType' \
  'physics:invertFilteredGroups' \
  'uniform double mediaOffset'; do
  grep -qF "$invented" "$TMP/unauthored-rt.usda" && bad="$bad
    INVENTED (never authored): $invented"
done
# ...while the two legitimately-authored, differently-spelled blendShapes must
# both survive, each on its own prim type.
for expect in \
  'uniform token[] blendShapes = ["a", "b"]' \
  'uniform token[] skel:blendShapes = ["a", "b"]'; do
  grep -qF "$expect" "$TMP/unauthored-rt.usda" || bad="$bad
    LOST: $expect"
done
if [ -n "$bad" ]; then
  echo "FAIL[unauthored-usdc]: writer invented and/or lost attributes:$bad"
  echo "--- got ---"
  cat "$TMP/unauthored-rt.usda"
  status=1
else
  echo "ok[unauthored-usdc]: no unauthored fallbacks invented; both blendShapes spellings survived"
fi

# Stage metadata + `reorder` body statements.
#
# 1. The Stage write path (stage-converter.cc) had drifted behind the Layer path
#    (sconv-layer.cc): kilogramsPerUnit and the two USDZ playback metas were
#    written by the latter and simply vanished through the former. An
#    AUTHORED-but-EMPTY `customLayerData = {}` was dropped too -- !empty() is not
#    an authored test, which is what the customLayerDataAuthored flag is for.
#    (The timecode family is written FURTHER UP in the same function. Do not
#    "helpfully" add it again: a duplicate root field corrupts the fieldset
#    encoding and the crate then fails to read back AT ALL -- caught here by
#    stage-meta-001 going from pass to a zero-byte round-trip.)
#
# 2. `reorder nameChildren` / `reorder properties` were parsed into PrimMeta and
#    written by nobody. Their crate spelling is primOrder / propertyOrder (NOT
#    primChildren / properties, which are the full name vectors). Both sides
#    needed work: the reader had no branch for them, AND stage-converter.cc has a
#    kPrimFields WHITELIST -- any field name not on it is re-routed into an
#    ATTRIBUTE spec, so before whitelisting them they came back as bogus
#    `token[] primOrder = [...]` properties rather than as metadata.
cat > "$TMP/stagemeta.usda" <<'USD'
#usda 1.0
(
    customLayerData = {
    }
    endTimeCode = 21
    framesPerSecond = 10
    kilogramsPerUnit = 3.14
    startTimeCode = 3
    upAxis = "Y"
    autoPlay = false
    playbackMode = "loop"
)

def Xform "W"
{
    reorder nameChildren = ["C", "A"]
    reorder properties = ["y", "x"]
    double x = 1
    double y = 2

    def Sphere "A"
    {
    }

    def Cone "C"
    {
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/stagemeta.usdc" "$TMP/stagemeta.usda" \
     >"$TMP/write11.log" 2>&1; then
  echo "FAIL: lusdcat could not write the stage-meta scene to usdc"
  cat "$TMP/write11.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/stagemeta.usdc" > "$TMP/stagemeta-rt.usda" 2>/dev/null
# A corrupt fieldset makes the read produce NOTHING -- check that first, or the
# grep loop below just reports every line as "lost" and buries the real cause.
if [ ! -s "$TMP/stagemeta-rt.usda" ]; then
  echo "FAIL[stagemeta-usdc]: crate read back EMPTY -- the fieldset encoding is corrupt"
  echo "(a duplicate root field will do this)"
  status=1
else
  lost=""
  for expect in \
    'kilogramsPerUnit = 3.14' \
    'autoPlay = false' \
    'playbackMode = "loop"' \
    'customLayerData = {' \
    'startTimeCode = 3' \
    'endTimeCode = 21' \
    'framesPerSecond = 10' \
    'reorder nameChildren = ["C", "A"]' \
    'reorder properties = ["y", "x"]'; do
    grep -qF "$expect" "$TMP/stagemeta-rt.usda" || lost="$lost
    $expect"
  done
  # reorder must come back as METADATA, not as a re-routed attribute spec.
  for bogus in 'token[] primOrder' 'token[] propertyOrder'; do
    grep -qF "$bogus" "$TMP/stagemeta-rt.usda" && lost="$lost
    RE-ROUTED TO A PROPERTY (missing from kPrimFields): $bogus"
  done
  if [ -n "$lost" ]; then
    echo "FAIL[stagemeta-usdc]: stage metadata / reorder lost on round-trip:$lost"
    echo "--- got ---"
    cat "$TMP/stagemeta-rt.usda"
    status=1
  else
    echo "ok[stagemeta-usdc]: stage metadata and reorder nameChildren/properties survived"
  fi
fi

# A BLOCKED xformOp value (`float xformOp:rotateZ:spin = None`) came back as the
# type's ZERO -- `= 0` -- which is not the same thing at all: None blocks weaker
# opinions, 0 is an authored number. ExtractXformOpsFromXformable (sconv-geom.cc)
# tested has_default() before is_blocked(), and has_default() is has_value(),
# which deliberately reports TRUE for a ValueBlock -- so the block fell into the
# value branch and ConvertValue rendered it as zero. ConvertAttributeToFields (the
# generic path) already had the two ordered correctly.
#
# Note it must consult XformOp::is_blocked(), not XformOp::_var.is_blocked():
# XformOp keeps its OWN _is_blocked flag (the one the reader sets) and only the
# accessor ORs the two.
cat > "$TMP/xformblock.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def Xform "W"
{
    float xformOp:rotateZ:spin = None
    uniform token[] xformOpOrder = ["xformOp:rotateZ:spin"]
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/xformblock.usdc" "$TMP/xformblock.usda" \
     >"$TMP/write12.log" 2>&1; then
  echo "FAIL: lusdcat could not write the blocked-xformOp scene to usdc"
  cat "$TMP/write12.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/xformblock.usdc" > "$TMP/xformblock-rt.usda" 2>/dev/null
if grep -qF 'xformOp:rotateZ:spin = None' "$TMP/xformblock-rt.usda"; then
  echo "ok[xformop-block-usdc]: a blocked xformOp survived as None"
else
  echo "FAIL[xformop-block-usdc]: blocked xformOp did not survive (a `= 0` here means"
  echo "  the ValueBlock was rendered as the type's zero)"
  echo "--- got ---"
  cat "$TMP/xformblock-rt.usda"
  status=1
fi

# Several TYPED attribute writers emitted only has_default() and had NO
# timeSamples branch at all, so animation on those attributes was dropped
# wholesale on write: SkelRoot/GPrim `extent`, PointInstancer
# positions/orientations/scales (and velocities/accelerations), and Camera's
# token `projection`. The generic attribute path and the xformOp path both
# handled timeSamples fine -- this was per-typed-attribute omission.
#
# Two traps worth keeping pinned:
#   - An ENUM's samples are int64 in memory and TOKENS on disk (projection, like
#     visibility). A pass-through copy emits ints and the file is wrong.
#   - `projection` is in stage-converter.cc's kUniformProps list, which stamped
#     Variability::Uniform UNCONDITIONALLY. A uniform attribute cannot vary over
#     time, and the READER enforces that by failing the entire prim ("declared
#     `uniform`, but a time-sampled value was authored") -- so the moment the
#     writer started emitting projection.timeSamples, the file became unreadable.
#     Uniform is now stamped only when the attribute has no samples.
#
# Also pinned: `uniform token orientation = "rightHanded"` was skipped BECAUSE it
# equals the schema default. An authored opinion is a strong one even when it
# matches the default -- dropping it changes what the layer means.
cat > "$TMP/typedts.usda" <<'USD'
#usda 1.0
(
    defaultPrim = "W"
)

def Xform "W"
{
    def SkelRoot "R"
    {
        float3[] extent.timeSamples = {
            2: [(-12, 5, -11), (12, 12, 6)],
            3: [(-13, 5, -12), (13, 13, 7)],
        }
    }

    def Camera "Cam"
    {
        token projection.timeSamples = {
            1: "perspective",
            30: "orthographic",
        }
    }

    def PointInstancer "PI"
    {
        point3f[] positions.timeSamples = {
            1: [(0, 0, 5)],
            12: [(0, 1, 5)],
        }
    }

    def Mesh "M"
    {
        uniform token orientation = "rightHanded"
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/typedts.usdc" "$TMP/typedts.usda" \
     >"$TMP/write13.log" 2>&1; then
  echo "FAIL: lusdcat could not write the typed-timeSamples scene to usdc"
  cat "$TMP/write13.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/typedts.usdc" > "$TMP/typedts-rt.usda" 2>/dev/null
if [ ! -s "$TMP/typedts-rt.usda" ]; then
  echo "FAIL[typed-timesamples-usdc]: crate read back EMPTY -- the prim failed to"
  echo "  reconstruct (a `uniform` attribute carrying timeSamples will do this)"
  status=1
else
  lost=""
  for expect in \
    'extent.timeSamples' \
    'projection.timeSamples' \
    'positions.timeSamples' \
    '30: "orthographic"' \
    'uniform token orientation = "rightHanded"'; do
    grep -qF "$expect" "$TMP/typedts-rt.usda" || lost="$lost
    $expect"
  done
  if [ -n "$lost" ]; then
    echo "FAIL[typed-timesamples-usdc]: dropped on round-trip:$lost"
    echo "--- got ---"
    cat "$TMP/typedts-rt.usda"
    status=1
  else
    echo "ok[typed-timesamples-usdc]: extent/projection/positions timeSamples and an authored-equals-default orientation survived"
  fi
fi

# -------------------------------------------------------------------------
# 16. Metadata blocks on prims and on individual attributes.
#
# The crate writer only emitted the handful of prim metas it knew by name and
# emitted no attribute metadata at all, so `sdrMetadata`, any unregistered
# (custom) prim meta, an attribute's `customData`, and a texture input's
# `colorSpace` were all silently dropped on write.
# -------------------------------------------------------------------------
cat > "$TMP/meta.usda" <<'USD'
#usda 1.0

def Sphere "S" (
    myCustomMeta = "hello"
)
{
    double radius = 3 (
        customData = {
            string note = "authored"
        }
    )
}

def Shader "Tex" (
    sdrMetadata = {
        string role = "texture"
    }
)
{
    uniform token info:id = "UsdUVTexture"
    asset inputs:file = @./tex.png@ (
        colorSpace = "Raw"
    )
    float outputs:r
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/meta.usdc" "$TMP/meta.usda" \
     >"$TMP/write16.log" 2>&1; then
  echo "FAIL: lusdcat could not write the metadata scene to usdc"
  cat "$TMP/write16.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/meta.usdc" > "$TMP/meta-rt.usda" 2>/dev/null
lost=""
for expect in \
  'myCustomMeta = "hello"' \
  'sdrMetadata' \
  'string role = "texture"' \
  'string note = "authored"' \
  'colorSpace = "Raw"'; do
  grep -qF "$expect" "$TMP/meta-rt.usda" || lost="$lost
    $expect"
done
if [ -n "$lost" ]; then
  echo "FAIL[metadata-usdc]: dropped on round-trip:$lost"
  echo "--- got ---"
  cat "$TMP/meta-rt.usda"
  status=1
else
  echo "ok[metadata-usdc]: sdrMetadata, unregistered prim meta, attribute customData and inputs:file colorSpace survived"
fi

# -------------------------------------------------------------------------
# 17. A SkelRoot's xformOps, and a Mesh's subsetFamily familyType.
#
# SkelRoot is Xformable, but only ExtractSkeletonProperties called
# ExtractXformOpsFromXformable -- so a rig's transform root snapped back to the
# origin on write. And `subsetFamily:<name>:familyType` lives in a map on the
# MESH (not on the GeomSubset), so it belongs to no schema struct and the writer
# had no branch for it at all.
# -------------------------------------------------------------------------
cat > "$TMP/skelsubset.usda" <<'USD'
#usda 1.0

def SkelRoot "Rig"
{
    double3 xformOp:translate = (1, 2, 3)
    uniform token[] xformOpOrder = ["xformOp:translate"]

    def Mesh "M"
    {
        int[] faceVertexCounts = [3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        uniform token subsetFamily:side:familyType = "partition"

        def GeomSubset "sub0"
        {
            uniform token elementType = "face"
            uniform token familyName = "side"
            int[] indices = [0]
        }
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/skelsubset.usdc" "$TMP/skelsubset.usda" \
     >"$TMP/write17.log" 2>&1; then
  echo "FAIL: lusdcat could not write the skelroot/subset scene to usdc"
  cat "$TMP/write17.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/skelsubset.usdc" > "$TMP/skelsubset-rt.usda" 2>/dev/null
if [ ! -s "$TMP/skelsubset-rt.usda" ]; then
  echo "FAIL[skelroot-subsetfamily-usdc]: crate read back EMPTY -- the prim failed"
  echo "  to reconstruct (a non-uniform subsetFamily familyType will do this)"
  status=1
else
  lost=""
  for expect in \
    'double3 xformOp:translate = (1, 2, 3)' \
    'uniform token[] xformOpOrder = ["xformOp:translate"]' \
    'uniform token subsetFamily:side:familyType = "partition"'; do
    grep -qF "$expect" "$TMP/skelsubset-rt.usda" || lost="$lost
    $expect"
  done
  if [ -n "$lost" ]; then
    echo "FAIL[skelroot-subsetfamily-usdc]: dropped on round-trip:$lost"
    echo "--- got ---"
    cat "$TMP/skelsubset-rt.usda"
    status=1
  else
    echo "ok[skelroot-subsetfamily-usdc]: SkelRoot xformOps and the mesh's subsetFamily familyType survived"
  fi
fi

# -------------------------------------------------------------------------
# 18. Typed-attribute connections, listOp qualifier order, bare-string attribute
#     metadata, and a non-conformant shader terminal type.
#
# - USD lets ANY attribute be connected, not just shader inputs, but the typed
#   writers only ever emitted the value, so `double size.connect` was dropped.
# - A crate ListOp has no record of the order its qualifiers were authored in,
#   and the reader decoded the buckets append-before-prepend, swapping the two
#   lines on the way back.
# - A bare string in an attribute's metadata block lands in AttrMeta::stringData,
#   which had no crate field at all.
# - A shader terminal's type came from the C++ template parameter, so an
#   authored-but-non-conformant `token outputs:result` was rewritten to float2.
# -------------------------------------------------------------------------
cat > "$TMP/misc.usda" <<'USD'
#usda 1.0

def Cube "bora"
{
    double size = 100.5
    double size.connect = </bora.value>
    double doc_size = 1 (
        """
        muda
        """
    )
}

def Xform "StackedRefs" (
    prepend references = </bora>
    append references = </Mtl>
)
{
}

def Scope "Mtl"
{
    def Shader "Reader"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        token outputs:result
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/misc.usdc" "$TMP/misc.usda" \
     >"$TMP/write18.log" 2>&1; then
  echo "FAIL: lusdcat could not write the misc scene to usdc"
  cat "$TMP/write18.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/misc.usdc" > "$TMP/misc-rt.usda" 2>/dev/null
lost=""
for expect in \
  'double size.connect = </bora.value>' \
  'token outputs:result' \
  'muda'; do
  grep -qF "$expect" "$TMP/misc-rt.usda" || lost="$lost
    $expect"
done
# `prepend references` must be printed BEFORE `append references`
if ! awk '/prepend references/{p=NR} /append references/{a=NR} END{exit !(p && a && p < a)}' \
     "$TMP/misc-rt.usda"; then
  lost="$lost
    prepend references before append references"
fi
if [ -n "$lost" ]; then
  echo "FAIL[attr-connect-listop-stringdata-usdc]: wrong or dropped on round-trip:$lost"
  echo "--- got ---"
  cat "$TMP/misc-rt.usda"
  status=1
else
  echo "ok[attr-connect-listop-stringdata-usdc]: typed-attr .connect, listOp qualifier order, bare-string attr meta and a non-conformant shader terminal type survived"
fi

# -------------------------------------------------------------------------
# 19. Declaration-only attributes, and an animated asset path.
#
# An attribute is authored() the moment it is DECLARED, and
# TypedAttributeWithFallback::get_value() returns the SCHEMA FALLBACK when no
# value was authored -- so the typed writers turned a bare `double radius` into
# `double radius = 2`, INVENTING an opinion. That is not cosmetic: an authored
# opinion is a strong one, so the fabricated value wins over the weaker opinions
# it should have deferred to during composition.
#
# `asset inputs:file` with timeSamples and no default is the same shape: the
# samples must ride in the SAME spec as the typeName, or the reader has no value
# to build the Property from.
# -------------------------------------------------------------------------
cat > "$TMP/defonly.usda" <<'USD'
#usda 1.0

def Sphere "S"
{
    double radius
}

def Cube "C"
{
    double size
}

def Shader "Tex"
{
    uniform token info:id = "UsdUVTexture"
    asset inputs:file.timeSamples = {
        0: @a.png@,
        1: @b.png@,
    }
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/defonly.usdc" "$TMP/defonly.usda" \
     >"$TMP/write19.log" 2>&1; then
  echo "FAIL: lusdcat could not write the declaration-only scene to usdc"
  cat "$TMP/write19.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/defonly.usdc" > "$TMP/defonly-rt.usda" 2>/dev/null
if [ ! -s "$TMP/defonly-rt.usda" ]; then
  echo "FAIL[defonly-usdc]: crate read back EMPTY -- a spec carrying only a"
  echo "  typeName has no value for the reader to build a Property from"
  status=1
else
  bad=""
  # The fallbacks must NOT have been invented (Sphere radius is 2, Cube size 2).
  grep -qE 'double radius *=' "$TMP/defonly-rt.usda" && bad="$bad
    radius came back WITH a value (fallback baked in)"
  grep -qE 'double size *=' "$TMP/defonly-rt.usda" && bad="$bad
    size came back WITH a value (fallback baked in)"
  grep -qF 'double radius' "$TMP/defonly-rt.usda" || bad="$bad
    radius declaration dropped entirely"
  grep -qF 'double size' "$TMP/defonly-rt.usda" || bad="$bad
    size declaration dropped entirely"
  grep -qF 'inputs:file.timeSamples' "$TMP/defonly-rt.usda" || bad="$bad
    animated inputs:file timeSamples dropped"
  if [ -n "$bad" ]; then
    echo "FAIL[defonly-usdc]:$bad"
    echo "--- got ---"
    cat "$TMP/defonly-rt.usda"
    status=1
  else
    echo "ok[defonly-usdc]: declaration-only radius/size kept their (absent) value, animated inputs:file survived"
  fi
fi

# -------------------------------------------------------------------------
# 20. The SAME declaration-only bug across every shape that has a fallback.
#
# Check 19 covers Sphere/Cube because those are the ones the fixtures happened to
# exercise. Cylinder, Cone, Capsule and Plane have exactly the same
# TypedAttributeWithFallback<Animatable<double>> radius/height/width/length with a
# NON-ZERO fallback, so each was inventing a value too -- silently, with no
# fixture to catch it. They all go through EmitTypedAnimatableAttr now; this pins
# every one of them so the next shape added cannot quietly regress.
#
# Every attribute below is DECLARED with no value, so NONE may come back with one.
# -------------------------------------------------------------------------
cat > "$TMP/shapes.usda" <<'USD'
#usda 1.0

def Cylinder "Cy"
{
    double radius
    double height
}

def Cone "Co"
{
    double radius
    double height
}

def Capsule "Ca"
{
    double radius
    double height
}

def Plane "Pl"
{
    double width
    double length
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/shapes.usdc" "$TMP/shapes.usda" \
     >"$TMP/write20.log" 2>&1; then
  echo "FAIL: lusdcat could not write the shapes scene to usdc"
  cat "$TMP/write20.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/shapes.usdc" > "$TMP/shapes-rt.usda" 2>/dev/null
bad=""
for prop in radius height width length; do
  # declared, so it must survive...
  grep -qE "double $prop\$" "$TMP/shapes-rt.usda" || bad="$bad
    $prop: declaration dropped"
  # ...and must NOT have acquired the schema fallback
  grep -qE "double $prop *=" "$TMP/shapes-rt.usda" && bad="$bad
    $prop: came back WITH a value (fallback invented)"
done
if [ -n "$bad" ]; then
  echo "FAIL[shape-defonly-usdc]:$bad"
  echo "--- got ---"
  cat "$TMP/shapes-rt.usda"
  status=1
else
  echo "ok[shape-defonly-usdc]: Cylinder/Cone/Capsule/Plane radius/height/width/length stayed declaration-only"
fi

# -------------------------------------------------------------------------
# 21. The same fallback-invention bug across the LIGHT and CAMERA writers.
#
# Audited by probing, not by reading: declare a value-less attribute on every
# writer and see which come back with a value. sconv-light.cc had 38 authored()
# guards and ZERO is_value_empty() checks -- every light input invented its
# fallback (intensity 1, color (1,1,1), radius 0.5, DistantLight angle 0.53).
# GeomCamera did the same (focalLength 50, clippingRange (0.1, 1e6)), GPrim's
# doubleSided invented `false`, and a declared-but-value-less `extent` was
# DROPPED outright (has_value() is not an authored test).
#
# Every attribute below is DECLARED with no value: none may come back with one,
# and none may vanish.
# -------------------------------------------------------------------------
cat > "$TMP/fallback.usda" <<'USD'
#usda 1.0

def Mesh "M"
{
    bool doubleSided
    float3[] extent
}

def SphereLight "L"
{
    float inputs:intensity
    color3f inputs:color
    float inputs:radius
    float inputs:exposure
    bool inputs:normalize
}

def DistantLight "DL"
{
    float inputs:angle
}

def Camera "C"
{
    float focalLength
    float2 clippingRange
    float fStop
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/fallback.usdc" "$TMP/fallback.usda" \
     >"$TMP/write21.log" 2>&1; then
  echo "FAIL: lusdcat could not write the fallback scene to usdc"
  cat "$TMP/write21.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/fallback.usdc" > "$TMP/fallback-rt.usda" 2>/dev/null
bad=""
for prop in doubleSided extent "inputs:intensity" "inputs:color" "inputs:radius" \
            "inputs:exposure" "inputs:normalize" "inputs:angle" focalLength \
            clippingRange fStop; do
  grep -qF "$prop" "$TMP/fallback-rt.usda" || bad="$bad
    $prop: declaration dropped"
  grep -qE "$prop *=" "$TMP/fallback-rt.usda" && bad="$bad
    $prop: came back WITH a value (fallback invented)"
done
if [ -n "$bad" ]; then
  echo "FAIL[light-camera-defonly-usdc]:$bad"
  echo "--- got ---"
  cat "$TMP/fallback-rt.usda"
  status=1
else
  echo "ok[light-camera-defonly-usdc]: light inputs, camera params, doubleSided and extent all stayed declaration-only"
fi

# -------------------------------------------------------------------------
# 22. "Authored but empty" -- the last data-model gap in the sweep.
#
# There was no way to record that something was authored yet holds nothing:
#   - PrimVar::has_timesamples() was `ts.size() > 0`, so `x.timeSamples = {}`
#     was indistinguishable from no timeSamples at all, and was dropped.
#   - ListOp<T>::Has*Items() was `.size() > 0`, so a list-edit qualifier on a
#     relationship with NO targets (`append rel myval`, `delete rel myheight`)
#     had nowhere to live and came back as a bare `rel myval`.
#
# value::TimeSamples and ListOp<T> now carry explicit authored flags. The crate
# format needed NO change: it has always had a presence bit per ListOp bucket.
# -------------------------------------------------------------------------
cat > "$TMP/empty.usda" <<'USD'
#usda 1.0

def Xform "muda"
{
    float xformOp:rotateZ:spin.timeSamples = {}
    uniform token[] xformOpOrder = ["xformOp:rotateZ:spin"]
    custom float plain.timeSamples = {}
}

def "bora"
{
    append custom rel myval
    delete rel myheight
    prepend rel myother
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/empty.usdc" "$TMP/empty.usda" \
     >"$TMP/write22.log" 2>&1; then
  echo "FAIL: lusdcat could not write the authored-but-empty scene to usdc"
  cat "$TMP/write22.log"
  exit 1
fi

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/empty.usdc" > "$TMP/empty-rt.usda" 2>/dev/null
bad=""
# Each entry is "legacy spelling|pxr spelling". Legacy keeps target-less
# list-edit qualifiers and `custom` inline on `.timeSamples`; pxr (and core
# lusdcat, which matches it byte-for-byte) drops the target-less qualifier to a
# bare declaration and writes `custom float plain` as its own line.
for expect in \
  'float xformOp:rotateZ:spin.timeSamples = {|float xformOp:rotateZ:spin.timeSamples = {' \
  'custom float plain.timeSamples = {|custom float plain' \
  'append rel myval|custom rel myval' \
  'delete rel myheight|rel myheight' \
  'prepend rel myother|rel myother'; do
  { grep -qF "${expect%%|*}" "$TMP/empty-rt.usda" ||
    grep -qxF "    ${expect#*|}" "$TMP/empty-rt.usda"; } || bad="$bad
    ${expect%%|*}"
done
grep -qF 'plain.timeSamples = {' "$TMP/empty-rt.usda" || bad="$bad
    plain.timeSamples"
if [ -n "$bad" ]; then
  echo "FAIL[authored-but-empty-usdc]: dropped on round-trip:$bad"
  echo "--- got ---"
  cat "$TMP/empty-rt.usda"
  status=1
else
  echo "ok[authored-but-empty-usdc]: empty timeSamples and target-less list-edit qualifiers survived"
fi

# -------------------------------------------------------------------------
# 23. The empty timeSamples block must be readable by OpenUSD, not just by us.
#
# Our writer encoded an empty array by INLINING payload=0 (crate-writer-inline.cc).
# Our reader takes payload=0 to mean "empty array", but OpenUSD reads the payload
# as a FILE OFFSET: it seeks to byte 0, tries to unpack a vector<double> out of
# the bootstrap header, and rejects the whole file -- "Corrupt asset". pxr writes
# the empty times array OUT-OF-LINE (a real offset holding a count of zero).
#
# This is the bug class our own tests CANNOT see: reader and writer agreed with
# each other, so all 427 fixtures round-tripped and every check above passed. It
# took a real pxr binary to find. Set USDCAT_PATH (or install OpenUSD) to run it.
# -------------------------------------------------------------------------
PXR_USDCAT="${USDCAT_PATH:-$HOME/work/OpenUSD/dist/bin/usdcat}"
if [ ! -x "$PXR_USDCAT" ]; then
  echo "skip[pxr-empty-timesamples]: OpenUSD usdcat not found at $PXR_USDCAT (set USDCAT_PATH)"
else
  cat > "$TMP/pxrts.usda" <<'USD'
#usda 1.0

def Xform "m"
{
    custom float v.timeSamples = {}
}
USD

  if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/pxrts.usdc" "$TMP/pxrts.usda" \
       >"$TMP/write23.log" 2>&1; then
    echo "FAIL: lusdcat could not write the empty-timeSamples scene to usdc"
    cat "$TMP/write23.log"
    exit 1
  fi

  if ! "$PXR_USDCAT" "$TMP/pxrts.usdc" > "$TMP/pxrts-rt.usda" 2>"$TMP/pxr23.err"; then
    echo "FAIL[pxr-empty-timesamples]: OpenUSD cannot read the .usdc we wrote"
    cat "$TMP/pxr23.err"
    status=1
  elif ! grep -qF "v.timeSamples" "$TMP/pxrts-rt.usda"; then
    echo "FAIL[pxr-empty-timesamples]: OpenUSD read the file but lost the timeSamples block"
    cat "$TMP/pxrts-rt.usda"
    status=1
  else
    echo "ok[pxr-empty-timesamples]: OpenUSD reads our empty timeSamples block"
  fi
fi

# -------------------------------------------------------------------------
# 24. OpenUSD must be able to read a variant hierarchy out of our .usdc.
#
# Four independent wire-format bugs, all invisible to lightusd-only tests (our
# reader tolerated or mirrored every one of them):
#   - the path-table element token for /A{v=sel} was the whole slash-segment
#     ("A{v=sel}"), not the bare "{v=sel}" element -> "Invalid prim name";
#   - a VariantSet spec was addressed as {set}, a form SdfPath does not have;
#     pxr puts VariantSet specs at the EMPTY selection, {set=};
#   - the prim spec carried no `variantSetChildren` and the variant spec no
#     `primChildren`, so pxr's traversal never DESCENDED into the variants it
#     could otherwise see (opinion fields like variantSetNames do not count);
#   - both children fields must be the dedicated TokenVector crate type, like
#     primChildren/properties, not a Token[] array.
# -------------------------------------------------------------------------
if [ ! -x "$PXR_USDCAT" ]; then
  echo "skip[pxr-variant-hierarchy]: OpenUSD usdcat not found at $PXR_USDCAT (set USDCAT_PATH)"
else
  cat > "$TMP/pxrvar.usda" <<'USD'
#usda 1.0

def Xform "Root" (
    append variantSets = "look"
)
{
    variantSet "look" = {
        "red" {
            def Sphere "Ball"
            {
            }
        }
        "blue" {
        }
    }
}
USD

  if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/pxrvar.usdc" "$TMP/pxrvar.usda" \
       >"$TMP/write24.log" 2>&1; then
    echo "FAIL: lusdcat could not write the variant scene to usdc"
    cat "$TMP/write24.log"
    exit 1
  fi

  if ! "$PXR_USDCAT" "$TMP/pxrvar.usdc" > "$TMP/pxrvar-rt.usda" 2>"$TMP/pxr24.err"; then
    echo "FAIL[pxr-variant-hierarchy]: OpenUSD cannot open the variant .usdc we wrote"
    cat "$TMP/pxr24.err"
    status=1
  else
    lost=""
    for expect in 'variantSet "look"' '"red"' '"blue"' 'def Sphere "Ball"'; do
      grep -qF "$expect" "$TMP/pxrvar-rt.usda" || lost="$lost
    $expect"
    done
    if [ -n "$lost" ]; then
      echo "FAIL[pxr-variant-hierarchy]: OpenUSD opened the file but the variant content is gone:$lost"
      echo "--- pxr saw ---"
      cat "$TMP/pxrvar-rt.usda"
      status=1
    else
      echo "ok[pxr-variant-hierarchy]: OpenUSD reads our variantSet, both variants, and the prim inside"
    fi
  fi
fi

# -------------------------------------------------------------------------
# 25. Unregistered (unknown-schema) metadata must survive at EVERY level.
#
# OpenUSD preserves metadata it does not recognise (SdfUnregisteredValue) at
# the layer, prim, attribute, and relationship level. lightusd used to parse
# and DISCARD three of the four: only the prim level survived. The raw USDA
# text of the value is kept verbatim (quotes included for strings) in the
# unregisteredMetas maps on LayerMetas / PrimMeta / AttrMetas, and travels
# through the crate as the UNREGISTERED_VALUE wire type -- the same encoding
# pxr uses, so a pxr usdcat read-back of our .usdc shows the original values.
# -------------------------------------------------------------------------
cat > "$TMP/unreg.usda" <<'USD'
#usda 1.0
(
    extensionLayerProbe = "kept-by-openusd"
)

def Scope "P" (
    extensionPrimProbe = 17
)
{
    int v = 1 (
        extensionProbe = 42
    )
    rel r (
        extensionRelProbe = "kept-by-openusd"
    )
}
USD

check_unreg_output() {
  # $1: file to check, $2: label
  local lost=""
  for expect in \
      'extensionLayerProbe = "kept-by-openusd"' \
      'extensionPrimProbe = 17' \
      'extensionProbe = 42' \
      'extensionRelProbe = "kept-by-openusd"'; do
    grep -qF "$expect" "$1" || lost="$lost
    $expect"
  done
  if [ -n "$lost" ]; then
    echo "FAIL[$2]: unregistered metadata dropped or corrupted:$lost"
    echo "--- output ---"
    cat "$1"
    status=1
  else
    echo "ok[$2]"
  fi
}

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/unreg.usda" > "$TMP/unreg-ascii.usda" 2>/dev/null
check_unreg_output "$TMP/unreg-ascii.usda" "unregistered-metadata-usda"

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/unreg.usdc" "$TMP/unreg.usda" \
     >"$TMP/write25.log" 2>&1; then
  echo "FAIL: lusdcat could not write the unregistered-metadata scene to usdc"
  cat "$TMP/write25.log"
  exit 1
fi
"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/unreg.usdc" > "$TMP/unreg-crate.usda" 2>/dev/null
check_unreg_output "$TMP/unreg-crate.usda" "unregistered-metadata-usdc"

if [ -x "$PXR_USDCAT" ]; then
  if ! "$PXR_USDCAT" "$TMP/unreg.usdc" > "$TMP/unreg-pxr.usda" 2>"$TMP/pxr25.err"; then
    echo "FAIL[pxr-unregistered-metadata]: OpenUSD cannot read the .usdc we wrote"
    cat "$TMP/pxr25.err"
    status=1
  else
    check_unreg_output "$TMP/unreg-pxr.usda" "pxr-unregistered-metadata"
  fi
else
  echo "skip[pxr-unregistered-metadata]: OpenUSD usdcat not found at $PXR_USDCAT (set USDCAT_PATH)"
fi

# -------------------------------------------------------------------------
# 26. OpenUSD must SEE the sublayers in our .usdc.
#
# Two wire bugs, both invisible to lightusd-only tests:
#   - `subLayers` must be the dedicated StringVector crate type. As a
#     VtArray<string> the field decodes but SdfLayer does not recognise it,
#     so pxr silently composed NOTHING from any sublayer we wrote.
#   - `subLayerOffsets` must ALWAYS accompany subLayers with the same length
#     (pxr writes identity offsets); omitting it for default offsets raises
#     "Invalid sublayer index" and makes the whole file unreadable.
# -------------------------------------------------------------------------
cat > "$TMP/sub26.usda" <<'USD'
#usda 1.0

def Scope "FromSub"
{
}
USD
cat > "$TMP/main26.usda" <<'USD'
#usda 1.0
(
    subLayers = [ @./sub26.usda@ ]
)

def Scope "Main"
{
}
USD

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/main26.usdc" "$TMP/main26.usda" \
     >"$TMP/write26.log" 2>&1; then
  echo "FAIL: lusdcat could not write the sublayer scene to usdc"
  cat "$TMP/write26.log"
  exit 1
fi

if [ -x "$PXR_USDCAT" ]; then
  if ! "$PXR_USDCAT" --flatten "$TMP/main26.usdc" > "$TMP/main26-flat.usda" 2>"$TMP/pxr26.err"; then
    echo "FAIL[pxr-sublayers]: OpenUSD cannot open the sublayer .usdc we wrote"
    cat "$TMP/pxr26.err"
    status=1
  elif ! grep -qF 'def Scope "FromSub"' "$TMP/main26-flat.usda"; then
    echo "FAIL[pxr-sublayers]: OpenUSD opened the file but composed nothing from the sublayer"
    cat "$TMP/main26-flat.usda"
    status=1
  else
    echo "ok[pxr-sublayers]: OpenUSD composes prims out of the sublayer we wrote"
  fi
else
  echo "skip[pxr-sublayers]: OpenUSD usdcat not found at $PXR_USDCAT (set USDCAT_PATH)"
fi

# -------------------------------------------------------------------------
# 27. The AUTHORED typeName must survive -- role spellings and legacy types.
#
# Two families of silent re-typing:
#   - schema role types: `point3f[] points` came back (and was shown to
#     OpenUSD) as `float3[] points`, because ConvertValue degrades role types
#     to their underlying storage and the spec typeName was inferred from the
#     VALUE. The writers now declare the role spelling explicitly.
#   - authored variants the schema accepts: `token inputs:varname` (older
#     UsdPrimvarReader spec) was re-typed to `string`, and an authored
#     `float3 inputs:diffuseColor` to `color3f`. usdchecker flags (or not)
#     these AS AUTHORED; re-typing made our output lie about the source.
# -------------------------------------------------------------------------
cat > "$TMP/roletype.usda" <<'USD'
#usda 1.0

def Mesh "M"
{
    point3f[] points = [(0, 0, 0)]
}

def Shader "TexReader"
{
    uniform token info:id = "UsdPrimvarReader_float2"
    token inputs:varname = "st"
    float2 outputs:result
}
USD

check_roletype_output() {
  local lost=""
  for expect in 'point3f[] points' 'token inputs:varname'; do
    grep -qF "$expect" "$1" || lost="$lost
    $expect"
  done
  if [ -n "$lost" ]; then
    echo "FAIL[$2]: authored typeName re-typed:$lost"
    echo "--- output ---"
    cat "$1"
    status=1
  else
    echo "ok[$2]"
  fi
}

"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/roletype.usda" > "$TMP/roletype-ascii.usda" 2>/dev/null
check_roletype_output "$TMP/roletype-ascii.usda" "authored-typename-usda"

if ! "$LUSDCAT" $LUSDCAT_LAYER_ARGS --output-format usdc -o "$TMP/roletype.usdc" "$TMP/roletype.usda" \
     >"$TMP/write27.log" 2>&1; then
  echo "FAIL: lusdcat could not write the role-type scene to usdc"
  cat "$TMP/write27.log"
  exit 1
fi
"$LUSDCAT" $LUSDCAT_LAYER_ARGS "$TMP/roletype.usdc" > "$TMP/roletype-crate.usda" 2>/dev/null
check_roletype_output "$TMP/roletype-crate.usda" "authored-typename-usdc"

if [ -x "$PXR_USDCAT" ]; then
  if ! "$PXR_USDCAT" "$TMP/roletype.usdc" > "$TMP/roletype-pxr.usda" 2>"$TMP/pxr27.err"; then
    echo "FAIL[pxr-authored-typename]: OpenUSD cannot read the .usdc we wrote"
    cat "$TMP/pxr27.err"
    status=1
  else
    check_roletype_output "$TMP/roletype-pxr.usda" "pxr-authored-typename"
  fi
else
  echo "skip[pxr-authored-typename]: OpenUSD usdcat not found at $PXR_USDCAT (set USDCAT_PATH)"
fi

exit "$status"
