#!/usr/bin/env bash
set -euo pipefail

UNIT="${1:?unit-test-lightusd path required}"
DATA="${2:-}"
RENDERER="${3:-}"

# No corpus: skip (ctest SKIP_RETURN_CODE).
if [ -z "$DATA" ] || [ ! -f "$DATA/reference/ref_float.vdb" ]; then
  echo "SKIP: tinyvdbio data directory not given or incomplete" >&2
  exit 77
fi

test -f "$DATA/reference/ref_float.vdb"
test -f "$DATA/smoke.vdb"
test -f "$DATA/sphere_points.vdb"
TINYVDBIO_DATA_DIR="$DATA" "$UNIT" usdvol_vdb_corpus_test
TINYVDBIO_DATA_DIR="$DATA" "$UNIT" usdvol_material_binding_test

# Optional end-to-end material/field regression. The two scenes differ only in
# the bound volume shader's density; deterministic output must differ. Keeping
# the VDB outside Git avoids adding a multi-megabyte binary fixture.
if [ -n "$RENDERER" ]; then
  test -x "$RENDERER"
  TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/lightusd-vdb-render.XXXXXX")"
  trap 'rm -rf "$TMP_ROOT"' EXIT
  write_scene() {
    local density="$1"
    local output="$2"
    {
      printf '%s\n' '#usda 1.0'
      printf 'def Volume "Fire" (prepend apiSchemas = ["MaterialBindingAPI"]) {\n'
      printf '  rel field:density = </Fire/Density>\n'
      printf '  rel field:temperature = </Fire/Temperature>\n'
      printf '  rel material:binding = </FireMaterial>\n'
      printf '  def OpenVDBAsset "Density" {\n    asset filePath = @%s/fire.vdb@\n    token fieldName = "density"\n  }\n' "$DATA"
      printf '  def OpenVDBAsset "Temperature" {\n    asset filePath = @%s/fire.vdb@\n    token fieldName = "temperature"\n  }\n' "$DATA"
      printf '}\n'
      printf 'def Material "FireMaterial" {\n'
      printf '  token outputs:volume.connect = </FireMaterial/Shader.outputs:volume>\n'
      printf '  def Shader "EmissionColor" {\n    uniform token info:id = "ND_constant_color3"\n'
      printf '    color3f inputs:value = (0.2, 0.6, 1.0)\n    color3f outputs:out\n  }\n'
      printf '  def Shader "EmissionScale" {\n    uniform token info:id = "ND_constant_float"\n'
      printf '    float inputs:value = 0.5\n    float outputs:out\n  }\n'
      printf '  def Shader "EmissionMultiply" {\n    uniform token info:id = "ND_multiply_color3FA"\n'
      printf '    color3f inputs:in1.connect = </FireMaterial/EmissionColor.outputs:out>\n'
      printf '    float inputs:in2.connect = </FireMaterial/EmissionScale.outputs:out>\n'
      printf '    color3f outputs:out\n  }\n'
      printf '  def Shader "Shader" {\n    uniform token info:id = "ND_standard_volume_volume"\n'
      printf '    float inputs:density = %s\n' "$density"
      printf '    color3f inputs:scattering_color = (0.04, 0.02, 0.01)\n'
      printf '    color3f inputs:emission_color.connect = </FireMaterial/EmissionMultiply.outputs:out>\n'
      printf '    float inputs:emission_intensity = 3\n    token outputs:volume\n  }\n}\n'
    } > "$output"
  }
  write_scene 0 "$TMP_ROOT/off.usda"
  write_scene 0.35 "$TMP_ROOT/on.usda"
  "$RENDERER" "$TMP_ROOT/off.usda" "$TMP_ROOT/off.png" -w 48 -height 48 -samples 1
  "$RENDERER" "$TMP_ROOT/on.usda" "$TMP_ROOT/on.png" -w 48 -height 48 -samples 1
  if cmp -s "$TMP_ROOT/off.png" "$TMP_ROOT/on.png"; then
    echo "ERROR: bound volume material density did not affect rendering" >&2
    exit 1
  fi
fi
