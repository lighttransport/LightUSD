#!/usr/bin/env bash
# Semantic texture grid: vector surface normals, OpenPBR coat normals,
# occlusion/coat response, and external-vs-USDZ texture parity.
set -uo pipefail
SKIP=77
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN="${LUSDVIEW:-$ROOT/build_ninja/lusdview}"
if [ "${LUSDVIEW_SEMANTIC_GENERATE_ONLY:-0}" != 1 ]; then
  [ -x "$BIN" ] || { echo "SKIP: lusdview not found"; exit "$SKIP"; }
fi
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit "$SKIP"; }
command -v zip >/dev/null || { echo "SKIP: zip missing"; exit "$SKIP"; }
VK_DEVICE_ARGS=()
if [ -n "${LUSDVIEW_VK_DEVICE:-}" ]; then
  VK_DEVICE_ARGS=(--vk-device "$LUSDVIEW_VK_DEVICE")
fi
# A raster preflight is insufficient for a requested RT device: the viewer
# may select a different adapter until --rt forces ray-query initialization.
# Keep this separate from the actual backend arguments so the raster lane is
# still tested without RT below.
VK_PREFLIGHT_RT_ARGS=()
case " ${LUSDVIEW_SEMANTIC_BACKENDS:-} " in
  *" vk-rt "*) VK_PREFLIGHT_RT_ARGS=(--rt) ;;
esac
OUT="${LUSDVIEW_TEST_OUT:-$(mktemp -d)}"; mkdir -p "$OUT"
[ -n "${LUSDVIEW_TEST_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
printf '%s\n' '{"window_size":{"width":256,"height":256}}' > "$OUT/config.json"

python3 - "$OUT" <<'PY'
import binascii,os,struct,sys,zlib
def ppm(name, p):
 with open(os.path.join(sys.argv[1],name),'wb') as f: f.write(b'P6\n8 1\n255\n'+bytes(p))
def png(name, p):
 raw=b'\0'+bytes(p)
 def chunk(kind,payload):
  return (struct.pack('>I',len(payload))+kind+payload+
          struct.pack('>I',binascii.crc32(kind+payload)&0xffffffff))
 data=(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,1,8,2,0,0,0))+
       chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b''))
 with open(os.path.join(sys.argv[1],name),'wb') as f: f.write(data)
ppm('normal.ppm',(255,128,192)*4+(128,128,255)*4) # +X-ish, then +Z
png('coat_normal.png',(255,128,192)*4+(128,128,255)*4)
ppm('normal.1001.ppm',(255,128,192)*8)
ppm('normal.1002.ppm',(128,128,255)*8)
png('coat_normal.1001.png',(255,128,192)*8)
png('coat_normal.1002.png',(128,128,255)*8)
ppm('occlusion.ppm',(32,32,32)*4+(224,224,224)*4)
ppm('occlusion.1001.ppm',(32,32,32)*8)
ppm('occlusion.1002.ppm',(224,224,224)*8)
# Keep the low side visibly separated from the viewer clear color so the
# backend-neutral foreground detector retains both halves of the quad.
ppm('coat_weight.ppm',(64,64,64)*4+(224,224,224)*4)
ppm('coat_roughness.ppm',(64,64,64)*4+(224,224,224)*4)
ppm('coat_color.ppm',(240,32,32)*4+(32,32,240)*4)
ppm('coat_weight.1001.ppm',(64,64,64)*8)
ppm('coat_weight.1002.ppm',(224,224,224)*8)
ppm('coat_roughness.1001.ppm',(64,64,64)*8)
ppm('coat_roughness.1002.ppm',(224,224,224)*8)
ppm('coat_color.1001.ppm',(240,32,32)*8)
ppm('coat_color.1002.ppm',(32,32,240)*8)
ppm('base.ppm',(240,32,32)*4+(32,32,240)*4)
ppm('base.1001.ppm',(240,32,32)*8)
ppm('base.1002.ppm',(32,32,240)*8)
ppm('specular.ppm',(240,32,32)*4+(32,32,240)*4)
ppm('specular_udim.1001.ppm',(240,32,32)*8)
ppm('specular_udim.1002.ppm',(32,32,240)*8)
# Packed ORM-style source: R/B rise while G falls. Each scalar connection must
# retain its own channel even though all three slots share one image.
ppm('orm.ppm',(64,224,64)*4+(224,64,224)*4)
ppm('orm.1001.ppm',(64,224,64)*8)
ppm('orm.1002.ppm',(224,64,224)*8)
ppm('emission.ppm',(24,24,24)*4+(232,232,232)*4)
ppm('emission.1001.ppm',(24,24,24)*8)
ppm('emission.1002.ppm',(232,232,232)*8)
ppm('opacity.ppm',(24,24,24)*4+(232,232,232)*4)
ppm('opacity.1001.ppm',(24,24,24)*8)
ppm('opacity.1002.ppm',(232,232,232)*8)
PY

quad() {
  cat <<'USDA'
  def Mesh "Quad" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,1)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
USDA
}
quad_degenerate() {
  cat <<'USDA'
  def Mesh "Quad" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0), (0,0), (0,0), (0,0)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
USDA
}
quad_udim() {
  cat <<'USDA'
  def Mesh "Quad" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    texCoord2f[] primvars:st = [(0,0), (2,0), (2,1), (0,1)] (interpolation = "vertex")
    rel material:binding = </World/M>
  }
USDA
}
write_normal_material() {
  local family="$1" file="$2" udim="${3:-0}" degenerate="${4:-0}" shader_id base_name normal_name
  local texture=normal.ppm
  [ "$udim" = 0 ] || texture='normal.<UDIM>.ppm'
  case "$family" in
    preview) shader_id=UsdPreviewSurface; base_name=diffuseColor; normal_name=normal;;
    openpbr) shader_id=OpenPBRSurface; base_name=base_color; normal_name=geometry_normal;;
    standard) shader_id=ND_standard_surface_surfaceshader; base_name=base_color; normal_name=normal;;
    *) return 2;;
  esac
  {
    echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
    if [ "$degenerate" = 1 ]; then quad_degenerate
    elif [ "$udim" = 1 ]; then quad_udim
    else quad
    fi
    cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "$shader_id"
      color3f inputs:$base_name = (0.35,0.35,0.35)
      normal3f inputs:$normal_name.connect = </World/M/N.outputs:rgb>
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "N" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$texture@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      float4 inputs:scale = (2,2,2,1)
      float4 inputs:bias = (-1,-1,-1,0)
      token inputs:sourceColorSpace = "raw"
      color3f outputs:rgb
    }
  }
}
USDA
  } > "$file"
}
write_normal_material preview "$OUT/normal.usda"
write_normal_material preview "$OUT/normal-preview-udim.usda" 1
write_normal_material openpbr "$OUT/normal-openpbr.usda"
write_normal_material openpbr "$OUT/normal-openpbr-udim.usda" 1
write_normal_material standard "$OUT/normal-standard.usda"
write_normal_material standard "$OUT/normal-standard-udim.usda" 1
write_normal_material preview "$OUT/normal-degenerate.usda" 0 1
write_coat_normal_material() {
  local file="$1" udim="${2:-0}" degenerate="${3:-0}" texture=coat_normal.png
  [ "$udim" = 0 ] || texture='coat_normal.<UDIM>.png'
{
  echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
  if [ "$degenerate" = 1 ]; then quad_degenerate
  elif [ "$udim" = 1 ]; then quad_udim
  else quad
  fi
  cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:base_color = (0.03,0.03,0.03)
      float inputs:base_weight = 0.2
      float inputs:specular_roughness = 0.12
      float inputs:coat_weight = 1
      float inputs:coat_roughness = 0.08
      color3f inputs:coat_color = (1,1,1)
      normal3f inputs:geometry_coat_normal.connect = </World/M/N.outputs:out>
      token outputs:surface
    }
    def Shader "N" {
      uniform token info:id = "ND_image_color3"
      asset inputs:file = @./$texture@
      color3f outputs:out
    }
  }
  def DistantLight "Key" {
    float inputs:intensity = 4
    float3 xformOp:rotateXYZ = (0,35,0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
  }
}
USDA
} > "$file"
}
write_coat_normal_material "$OUT/coat-normal.usda"
write_coat_normal_material "$OUT/coat-normal-udim.usda" 1
write_coat_normal_material "$OUT/coat-normal-degenerate.usda" 0 1
write_standard_coat_normal_material() {
  local file="$1" udim="${2:-0}" texture=coat_normal.png
  [ "$udim" = 0 ] || texture='coat_normal.<UDIM>.png'
  {
    echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
    if [ "$udim" = 1 ]; then quad_udim; else quad; fi
    cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_standard_surface_surfaceshader"
      color3f inputs:base_color = (0.03,0.03,0.03)
      float inputs:specular_roughness = 0.12
      float inputs:coat = 1
      float inputs:coat_roughness = 0.08
      color3f inputs:coat_color = (1,1,1)
      normal3f inputs:coat_normal.connect = </World/M/N.outputs:out>
      token outputs:surface
    }
    def Shader "N" {
      uniform token info:id = "ND_image_color3"
      asset inputs:file = @./$texture@
      color3f outputs:out
    }
  }
}
USDA
  } > "$file"
}
write_standard_coat_normal_material "$OUT/coat-normal-standard.usda"
write_standard_coat_normal_material "$OUT/coat-normal-standard-udim.usda" 1
write_occlusion_material() {
  local file="$1" udim="${2:-0}" texture=occlusion.ppm
  [ "$udim" = 0 ] || texture='occlusion.<UDIM>.ppm'
  {
    echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
    if [ "$udim" = 1 ]; then quad_udim; else quad; fi
    cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.7,0.7,0.7)
      float inputs:occlusion.connect = </World/M/O.outputs:r>
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "O" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$texture@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "raw"
      float outputs:r
    }
  }
}
USDA
  } > "$file"
}
write_occlusion_material "$OUT/occlusion.usda"
write_occlusion_material "$OUT/occlusion-udim.usda" 1
write_coat_material() {
  local file="$1" udim="${2:-0}" weight=coat_weight.ppm color=coat_color.ppm rough=coat_roughness.ppm
  if [ "$udim" = 1 ]; then
    weight='coat_weight.<UDIM>.ppm'; color='coat_color.<UDIM>.ppm'; rough='coat_roughness.<UDIM>.ppm'
  fi
{
  echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
  if [ "$udim" = 1 ]; then quad_udim; else quad; fi
  cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:base_color = (0.02,0.02,0.02)
      float inputs:base_weight = 0.1
      float inputs:specular_roughness = 0.2
      float inputs:coat_weight.connect = </World/M/W.outputs:out>
      color3f inputs:coat_color.connect = </World/M/C.outputs:out>
      float inputs:coat_roughness.connect = </World/M/R.outputs:out>
      token outputs:surface
    }
    def Shader "W" {
      uniform token info:id = "ND_image_float"
      asset inputs:file = @./$weight@
      float outputs:out
    }
    def Shader "C" {
      uniform token info:id = "ND_image_color3"
      asset inputs:file = @./$color@
      color3f outputs:out
    }
    def Shader "R" {
      uniform token info:id = "ND_image_float"
      asset inputs:file = @./$rough@
      float outputs:out
    }
  }
  def DistantLight "Key" {
    float inputs:intensity = 5
    float3 xformOp:rotateXYZ = (0,25,0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
  }
}
USDA
} > "$file"
}
write_coat_material "$OUT/coat.usda"
write_coat_material "$OUT/coat-udim.usda" 1

write_preview_coat_material() {
  local file="$1" udim="${2:-0}" weight=coat_weight.ppm rough=coat_roughness.ppm
  if [ "$udim" = 1 ]; then
    weight='coat_weight.<UDIM>.ppm'; rough='coat_roughness.<UDIM>.ppm'
  fi
  {
    echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
    if [ "$udim" = 1 ]; then quad_udim; else quad; fi
    cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.02,0.02,0.02)
      float inputs:roughness = 0.2
      float inputs:clearcoat.connect = </World/M/W.outputs:r>
      float inputs:clearcoatRoughness.connect = </World/M/R.outputs:r>
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "W" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$weight@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "raw"
      float outputs:r
    }
    def Shader "R" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$rough@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "raw"
      float outputs:r
    }
  }
}
USDA
  } > "$file"
}
write_preview_coat_material "$OUT/coat-preview.usda"
write_preview_coat_material "$OUT/coat-preview-udim.usda" 1

write_standard_coat_material() {
  local file="$1" udim="${2:-0}" weight=coat_weight.ppm color=coat_color.ppm rough=coat_roughness.ppm
  if [ "$udim" = 1 ]; then
    weight='coat_weight.<UDIM>.ppm'; color='coat_color.<UDIM>.ppm'
    rough='coat_roughness.<UDIM>.ppm'
  fi
  {
    echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
    if [ "$udim" = 1 ]; then quad_udim; else quad; fi
    cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "ND_standard_surface_surfaceshader"
      color3f inputs:base_color = (0.02,0.02,0.02)
      float inputs:specular_roughness = 0.2
      float inputs:coat.connect = </World/M/W.outputs:out>
      color3f inputs:coat_color.connect = </World/M/C.outputs:out>
      float inputs:coat_roughness.connect = </World/M/R.outputs:out>
      token outputs:surface
    }
    def Shader "W" {
      uniform token info:id = "ND_image_float"
      asset inputs:file = @./$weight@
      float outputs:out
    }
    def Shader "C" {
      uniform token info:id = "ND_image_color3"
      asset inputs:file = @./$color@
      color3f outputs:out
    }
    def Shader "R" {
      uniform token info:id = "ND_image_float"
      asset inputs:file = @./$rough@
      float outputs:out
    }
  }
}
USDA
  } > "$file"
}
write_standard_coat_material "$OUT/coat-standard.usda"
write_standard_coat_material "$OUT/coat-standard-udim.usda" 1

write_core_material() {
  local family="$1" file="$2" udim="${3:-0}" shader_id base_name metal_name rough_name emit_name emit_enable
  local base_tex=base.ppm orm_tex=orm.ppm emission_tex=emission.ppm
  if [ "$udim" = 1 ]; then
    base_tex='base.<UDIM>.ppm'; orm_tex='orm.<UDIM>.ppm'
    emission_tex='emission.<UDIM>.ppm'
  fi
  case "$family" in
    preview)
      shader_id=UsdPreviewSurface; base_name=diffuseColor; metal_name=metallic
      rough_name=roughness; emit_name=emissiveColor; emit_enable=;;
    openpbr)
      shader_id=OpenPBRSurface; base_name=base_color; metal_name=base_metalness
      rough_name=base_roughness; emit_name=emission_color
      emit_enable='float inputs:emission_luminance = 1';;
    standard)
      shader_id=ND_standard_surface_surfaceshader; base_name=base_color
      metal_name=metalness; rough_name=specular_roughness; emit_name=emission_color
      emit_enable='float inputs:emission = 1';;
    *) return 2;;
  esac
  {
    echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
    if [ "$udim" = 1 ]; then quad_udim; else quad; fi
    cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "$shader_id"
      color3f inputs:$base_name.connect = </World/M/Base.outputs:rgb>
      float inputs:$metal_name.connect = </World/M/ORM.outputs:b>
      float inputs:$rough_name.connect = </World/M/ORM.outputs:g>
      color3f inputs:$emit_name.connect = </World/M/Emit.outputs:rgb>
      $emit_enable
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "Base" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$base_tex@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "sRGB"
      color3f outputs:rgb
    }
    def Shader "ORM" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$orm_tex@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "raw"
      float outputs:r
      float outputs:g
      float outputs:b
    }
    def Shader "Emit" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$emission_tex@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "sRGB"
      color3f outputs:rgb
    }
  }
}
USDA
  } > "$file"
}
write_core_material preview "$OUT/core-preview.usda"
write_core_material openpbr "$OUT/core-openpbr.usda"
write_core_material standard "$OUT/core-standard.usda"
write_core_material preview "$OUT/core-preview-udim.usda" 1
write_core_material openpbr "$OUT/core-openpbr-udim.usda" 1
write_core_material standard "$OUT/core-standard-udim.usda" 1

write_ior_material() {
  local family="$1" file="$2" shader_id ior_name
  case "$family" in
    preview) shader_id=UsdPreviewSurface; ior_name=ior;;
    openpbr) shader_id=OpenPBRSurface; ior_name=specular_ior;;
    standard) shader_id=ND_standard_surface_surfaceshader; ior_name=specular_IOR;;
    *) return 2;;
  esac
  cat > "$file" <<USDA
#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
  def Mesh "Left" {
    uniform bool doubleSided = 1
    point3f[] points = [(-1,-1,0), (0,-1,0), (0,1,0), (-1,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </World/ML>
  }
  def Mesh "Right" {
    uniform bool doubleSided = 1
    point3f[] points = [(0,-1,0), (1,-1,0), (1,1,0), (0,1,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    rel material:binding = </World/MR>
  }
  def Material "ML" {
    token outputs:surface.connect = </World/ML/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "$shader_id"
      float inputs:$ior_name = 1.5
      token outputs:surface
    }
  }
  def Material "MR" {
    token outputs:surface.connect = </World/MR/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "$shader_id"
      float inputs:$ior_name = 2.5
      token outputs:surface
    }
  }
}
USDA
}
write_ior_material preview "$OUT/ior-preview.usda"
write_ior_material openpbr "$OUT/ior-openpbr.usda"
write_ior_material standard "$OUT/ior-standard.usda"

write_opacity_material() {
  local family="$1" file="$2" udim="${3:-0}" shader_id base_name opacity_name
  local texture=opacity.ppm
  [ "$udim" = 0 ] || texture='opacity.<UDIM>.ppm'
  case "$family" in
    preview) shader_id=UsdPreviewSurface; base_name=diffuseColor; opacity_name=opacity;;
    openpbr) shader_id=OpenPBRSurface; base_name=base_color; opacity_name=geometry_opacity;;
    standard) shader_id=ND_standard_surface_surfaceshader; base_name=base_color; opacity_name=opacity;;
    *) return 2;;
  esac
  {
    echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
    if [ "$udim" = 1 ]; then quad_udim; else quad; fi
    cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "$shader_id"
      color3f inputs:$base_name = (0.8,0.8,0.8)
      float inputs:$opacity_name.connect = </World/M/O.outputs:r>
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "O" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$texture@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "raw"
      float outputs:r
    }
  }
}
USDA
  } > "$file"
}
for family in preview openpbr standard; do
  write_opacity_material "$family" "$OUT/opacity-$family.usda"
  write_opacity_material "$family" "$OUT/opacity-$family-udim.usda" 1
done
mkdir -p "$OUT/pkg-coat-openpbr"
cp "$OUT/coat.usda" "$OUT/coat_weight.ppm" "$OUT/coat_color.ppm" \
  "$OUT/coat_roughness.ppm" "$OUT/pkg-coat-openpbr/"
(cd "$OUT/pkg-coat-openpbr" && zip -0 -q "$OUT/coat.usdz" coat.usda \
  coat_weight.ppm coat_color.ppm coat_roughness.ppm)
mkdir -p "$OUT/pkg-coat-preview"
cp "$OUT/coat-preview.usda" "$OUT/coat_weight.ppm" \
  "$OUT/coat_roughness.ppm" "$OUT/pkg-coat-preview/"
(cd "$OUT/pkg-coat-preview" && zip -0 -q "$OUT/coat-preview.usdz" \
  coat-preview.usda coat_weight.ppm coat_roughness.ppm)
mkdir -p "$OUT/pkg-coat-standard"
cp "$OUT/coat-standard.usda" "$OUT/coat_weight.ppm" "$OUT/coat_color.ppm" \
  "$OUT/coat_roughness.ppm" "$OUT/pkg-coat-standard/"
(cd "$OUT/pkg-coat-standard" && zip -0 -q "$OUT/coat-standard.usdz" \
  coat-standard.usda coat_weight.ppm coat_color.ppm coat_roughness.ppm)
for family in preview openpbr standard; do
  mkdir -p "$OUT/pkg-opacity-$family"
  cp "$OUT/opacity-$family.usda" "$OUT/opacity.ppm" \
    "$OUT/pkg-opacity-$family/"
  (cd "$OUT/pkg-opacity-$family" && zip -0 -q "$OUT/opacity-$family.usdz" \
    "opacity-$family.usda" opacity.ppm)
done

write_specular_material() {
  local family="$1" file="$2" texture="${3:-specular.ppm}" shader_id spec_name ior_line workflow_line
  case "$family" in
    preview) shader_id=UsdPreviewSurface; spec_name=specularColor
      ior_line=''; workflow_line='      int inputs:useSpecularWorkflow = 1';;
    openpbr) shader_id=OpenPBRSurface; spec_name=specular_color
      ior_line='      float inputs:specular_ior = 4.0'; workflow_line='';;
    standard) shader_id=ND_standard_surface_surfaceshader; spec_name=specular_color
      ior_line='      float inputs:specular_IOR = 4.0'; workflow_line='';;
    *) return 2;;
  esac
{
  echo '#usda 1.0'; echo '(defaultPrim = "World" upAxis = "Y")'; echo 'def Xform "World" {'
  if [[ "$texture" == *'<UDIM>'* ]]; then quad_udim; else quad; fi
  cat <<USDA
  def Material "M" {
    token outputs:surface.connect = </World/M/P.outputs:surface>
    def Shader "P" {
      uniform token info:id = "$shader_id"
      color3f inputs:diffuseColor = (0.2,0.2,0.2)
$workflow_line
$ior_line
      color3f inputs:$spec_name.connect = </World/M/S.outputs:rgb>
      token outputs:surface
    }
    def Shader "ST" {
      uniform token info:id = "UsdPrimvarReader_float2"
      token inputs:varname = "st"
      float2 outputs:result
    }
    def Shader "S" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @./$texture@
      float2 inputs:st.connect = </World/M/ST.outputs:result>
      token inputs:sourceColorSpace = "sRGB"
      color3f outputs:rgb
    }
  }
}
USDA
} > "$file"
}
write_specular_material preview "$OUT/specular-preview.usda"
write_specular_material openpbr "$OUT/specular-openpbr.usda"
write_specular_material standard "$OUT/specular-standard.usda"
write_specular_material preview "$OUT/specular-preview-udim.usda" 'specular_udim.<UDIM>.ppm'
write_specular_material openpbr "$OUT/specular-openpbr-udim.usda" 'specular_udim.<UDIM>.ppm'
write_specular_material standard "$OUT/specular-standard-udim.usda" 'specular_udim.<UDIM>.ppm'
for family in preview openpbr standard; do
  mkdir -p "$OUT/pkg-specular-$family"
  cp "$OUT/specular-$family.usda" "$OUT/specular.ppm" \
    "$OUT/pkg-specular-$family/"
  (cd "$OUT/pkg-specular-$family" && zip -0 -q "$OUT/specular-$family.usdz" \
    "specular-$family.usda" specular.ppm)
done

mkdir -p "$OUT/pkg"; cp "$OUT/normal.usda" "$OUT/normal.ppm" "$OUT/pkg/"
(cd "$OUT/pkg" && zip -0 -q "$OUT/normal.usdz" normal.usda normal.ppm)
for family in openpbr standard; do
  mkdir -p "$OUT/pkg-normal-$family"
  cp "$OUT/normal-$family.usda" "$OUT/normal.ppm" \
    "$OUT/pkg-normal-$family/"
  (cd "$OUT/pkg-normal-$family" && zip -0 -q "$OUT/normal-$family.usdz" \
    "normal-$family.usda" normal.ppm)
done
mkdir -p "$OUT/pkg-occlusion"
cp "$OUT/occlusion.usda" "$OUT/occlusion.ppm" "$OUT/pkg-occlusion/"
(cd "$OUT/pkg-occlusion" && zip -0 -q "$OUT/occlusion.usdz" \
  occlusion.usda occlusion.ppm)
mkdir -p "$OUT/pkg-coat"; cp "$OUT/coat-normal.usda" "$OUT/coat_normal.png" "$OUT/pkg-coat/"
(cd "$OUT/pkg-coat" && zip -0 -q "$OUT/coat-normal.usdz" coat-normal.usda coat_normal.png)
mkdir -p "$OUT/pkg-standard-coat"
cp "$OUT/coat-normal-standard.usda" "$OUT/coat_normal.png" "$OUT/pkg-standard-coat/"
(cd "$OUT/pkg-standard-coat" && zip -0 -q "$OUT/coat-normal-standard.usdz" coat-normal-standard.usda coat_normal.png)
# External USDA texture resolution is rooted at the stage's `/World` in the
# viewer, whereas the packaged USDZ fixtures resolve relative to the archive.
# Keep the archive inputs portable and make the external generated fixtures
# point at the texture explicitly.
sed -i "s#@\./coat_normal.png@#@$OUT/coat_normal.png@#g" \
  "$OUT/coat-normal.usda" "$OUT/coat-normal-degenerate.usda" \
  "$OUT/coat-normal-standard.usda"
sed -i "s#@./coat_normal.<UDIM>.png@#@$OUT/coat_normal.<UDIM>.png@#g" \
  "$OUT/coat-normal-udim.usda" "$OUT/coat-normal-standard-udim.usda"
for family in preview openpbr standard; do
  mkdir -p "$OUT/pkg-core-$family"
  cp "$OUT/core-$family.usda" "$OUT/base.ppm" "$OUT/orm.ppm" \
    "$OUT/emission.ppm" "$OUT/pkg-core-$family/"
  (cd "$OUT/pkg-core-$family" && zip -0 -q "$OUT/core-$family.usdz" \
    "core-$family.usda" base.ppm orm.ppm emission.ppm)
done

# Reuse the exact semantic fixtures from headless renderer parity tests without
# starting a viewer or duplicating the USDA/texture generators. The caller owns
# LUSDVIEW_TEST_OUT in this mode, so the normal cleanup trap is already disabled.
if [ "${LUSDVIEW_SEMANTIC_GENERATE_ONLY:-0}" = 1 ]; then
  echo "PASS: generated semantic MaterialX fixtures in $OUT"
  exit 0
fi

probe() {
  python3 - "$1" "$2" <<'PY'
import collections,re,sys
d=open(sys.argv[1],'rb').read(); m=re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s',d)
if not m or int(m.group(3)) != 255: sys.exit(2)
w,h=map(int,m.groups()[:2]); p=d[m.end():]; px=[tuple(p[i:i+3]) for i in range(0,w*h*3,3)]
bg=collections.Counter(px).most_common(1)[0][0]
a=[(i%w,i//w) for i,c in enumerate(px) if max(abs(c[j]-bg[j]) for j in range(3))>4]
if len(a)<100: sys.exit(1)
x0,x1=min(x for x,y in a),max(x for x,y in a)+1; y0,y1=min(y for x,y in a),max(y for x,y in a)+1
def mean(lo,hi):
 q=[px[y*w+x] for y in range(y0+(y1-y0)*3//10,y0+(y1-y0)*7//10) for x in range(x0+(x1-x0)*lo//10,x0+(x1-x0)*hi//10)]
 return tuple(sum(c[i] for c in q)/len(q) for i in range(3))
l,r=mean(2,4),mean(6,8); kind=sys.argv[2]; print(kind,'left=',l,'right=',r)
if kind=='vector': ok=l[0]-l[2]>=25 and r[2]-r[0]>=25 and l[0]-r[0]>=45
elif kind=='coat-normal': ok=l[0]-l[2]>=25 and r[2]-r[0]>=25 and l[0]-r[0]>=45
elif kind in ('degenerate-normal','degenerate-coat-normal'):
 # Constant UVs must still produce a finite, visible normal AOV on both sides.
 # Do not require identical values: the renderer may face-forward the shading
 # normal independently per fragment. The regression is black/NaN output from
 # a degenerate TBN, so require meaningful encoded color in both sample bands.
 ok=(sum(l) >= 180 and sum(r) >= 180 and
     max(l) >= 80 and max(r) >= 80)
elif kind=='occlusion': ok=abs(sum(l)-sum(r))>=12
# Coat currently has no scalar AOV; this controlled shaded probe only requires
# a visible response and is deliberately looser than the diagnostic AOVs.
elif kind=='coat': ok=abs(sum(l)-sum(r))>=6
elif kind=='coat-weight': ok=sum(r)-sum(l)>=45
elif kind=='coat-color': ok=l[0]-l[2]>=40 and r[2]-r[0]>=40
elif kind=='coat-roughness': ok=sum(r)-sum(l)>=45
elif kind=='specular-f0': ok=l[0]-l[2]>=40 and r[2]-r[0]>=40
elif kind=='ior-f0': ok=sum(r)-sum(l)>=45
elif kind=='albedo': ok=l[0]-l[2]>=40 and r[2]-r[0]>=40
elif kind in ('metallic','emissive','opacity'): ok=sum(r)-sum(l)>=45
elif kind=='roughness': ok=sum(l)-sum(r)>=45
else: sys.exit(2)
sys.exit(0 if ok else 1)
PY
}
compare() {
  python3 - "$1" "$2" <<'PY'
import re,sys
def load(p):
 d=open(p,'rb').read(); m=re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s',d)
 if not m: raise ValueError(p)
 return m.groups()[:2],d[m.end():]
sa,a=load(sys.argv[1]); sb,b=load(sys.argv[2])
if sa!=sb or len(a)!=len(b): sys.exit(1)
mad=sum(abs(x-y) for x,y in zip(a,b))/len(a); print('image MAD=',mad)
sys.exit(0 if mad<=2.0 else 1)
PY
}
run() {
  if command -v timeout >/dev/null; then
    timeout --kill-after=5s "${LUSDVIEW_RENDER_TIMEOUT:-60s}" "$@"
  else
    "$@"
  fi
}
ran=0; fail=0; vk_software=0; degraded=0
declare -A backend_available=()
declare -A backend_unavailable=()
# Detect an all-CPU Vulkan installation before the first RT mode sweep. Some
# software drivers advertise ray-query extensions but spend minutes in shader
# JIT/initialization without producing a log line that the post-launch guard
# can inspect. Keep Vulkan raster coverage, but skip only the hardware RT lane.
if command -v vulkaninfo >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1 &&
   timeout 10s vulkaninfo --summary >"$OUT/vulkaninfo-summary.log" 2>&1; then
  if [ -z "${LUSDVIEW_VK_DEVICE:-}" ] &&
     grep -q 'PHYSICAL_DEVICE_TYPE_CPU' "$OUT/vulkaninfo-summary.log" &&
     ! grep -Eq 'PHYSICAL_DEVICE_TYPE_(DISCRETE|INTEGRATED|VIRTUAL)_GPU' \
       "$OUT/vulkaninfo-summary.log"; then
    vk_software=1
    backend_unavailable[vk-rt]=1
    echo "SKIP: Vulkan RT on software Vulkan (preflight)"
  fi
elif command -v timeout >/dev/null 2>&1 && [ -f "$ROOT/models/suzanne-pbr.usda" ]; then
  # Minimal fallback for hosts without vulkaninfo. The viewer reports the
  # selected device before entering its headless loop; timeout is expected on
  # builds that keep the window loop alive after the first frame.
  timeout 10s "$BIN" --headless --backend vk "${VK_DEVICE_ARGS[@]}" "${VK_PREFLIGHT_RT_ARGS[@]}" --frames 1 --mode shaded \
    --screenshot "$OUT/vulkan-preflight.ppm" "$ROOT/models/suzanne-pbr.usda" \
    >"$OUT/vulkan-preflight.log" 2>&1 || :
  if grep -Eqi 'llvmpipe|lavapipe|\(cpu, driver|software rasterizer|device=cpu|rt=off|rt_available=0|no Vulkan physical device|no matching.*Vulkan|Vulkan device.*not found' \
      "$OUT/vulkan-preflight.log"; then
    vk_software=1
    backend_unavailable[vk-rt]=1
    echo "SKIP: Vulkan RT unavailable (preflight viewer probe)"
  fi
fi
# `vulkaninfo` can report adapters that are not selectable through the active
# PRIME/offload environment. When CTest requests a specific device, validate
# that selection itself even if the global summary contains a discrete GPU.
if [ "$vk_software" = 0 ] && [ -n "${LUSDVIEW_VK_DEVICE:-}" ] &&
   command -v timeout >/dev/null 2>&1 && [ -f "$ROOT/models/suzanne-pbr.usda" ]; then
  timeout 10s "$BIN" --headless --backend vk "${VK_DEVICE_ARGS[@]}" "${VK_PREFLIGHT_RT_ARGS[@]}" --frames 1 --mode shaded \
    --screenshot "$OUT/vulkan-device-preflight.ppm" "$ROOT/models/suzanne-pbr.usda" \
    >"$OUT/vulkan-device-preflight.log" 2>&1 || :
  if grep -Eqi 'rt=off|rt_available=0|no Vulkan physical device|no matching.*Vulkan|Vulkan device.*not found' \
      "$OUT/vulkan-device-preflight.log"; then
    vk_software=1
    backend_unavailable[vk-rt]=1
    echo "SKIP: requested Vulkan RT device unavailable (preflight viewer probe)"
  fi
fi
# `vulkaninfo` may exit successfully after reporting an XCB error and can
# still list an adapter whose ray-query path is disabled. Probe the viewer's
# actual selected device whenever this invocation requests Vulkan RT, so
# `rt=off` cannot be mistaken for a usable RT backend.
case " ${LUSDVIEW_SEMANTIC_BACKENDS:-} " in
  *" vk-rt "*)
    if [ "${backend_unavailable[vk-rt]:-0}" = 0 ] &&
       command -v timeout >/dev/null 2>&1 && [ -f "$ROOT/models/suzanne-pbr.usda" ]; then
      timeout 10s "$BIN" --headless --backend vk "${VK_DEVICE_ARGS[@]}" "${VK_PREFLIGHT_RT_ARGS[@]}" --frames 1 --mode shaded \
        --screenshot "$OUT/vulkan-rt-preflight.ppm" "$ROOT/models/suzanne-pbr.usda" \
        >"$OUT/vulkan-rt-preflight.log" 2>&1 || :
      if grep -Eqi 'rt=off|rt_available=0|no Vulkan physical device|no matching.*Vulkan|Vulkan device.*not found' \
          "$OUT/vulkan-rt-preflight.log"; then
        vk_software=1
        backend_unavailable[vk-rt]=1
        echo "SKIP: Vulkan RT disabled by selected device (preflight viewer probe)"
      fi
    fi
    ;;
esac
GL_RUN=()
if [ -n "${DISPLAY:-}" ] && command -v xdpyinfo >/dev/null 2>&1 &&
   xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
  : # Prefer a usable inherited display when it exposes hardware OpenGL.
elif command -v xvfb-run >/dev/null 2>&1; then
  GL_RUN=(xvfb-run -a)
fi
if [ "${#GL_RUN[@]}" -gt 0 ] && command -v timeout >/dev/null 2>&1 &&
   command -v xdpyinfo >/dev/null 2>&1; then
  if ! timeout 10s "${GL_RUN[@]}" xdpyinfo >/dev/null 2>&1; then
    backend_unavailable[gl]=1
    echo "SKIP: OpenGL display unavailable (Xvfb preflight)"
  fi
fi
want_mode() {
  local needle="$1" item
  for item in ${LUSDVIEW_SEMANTIC_MODES:-albedo metallic roughness emissive opacity vector coat-normal occlusion coat-weight coat-color coat-roughness specular-f0 ior-f0}; do
    [ "$item" = "$needle" ] && return 0
  done
  return 1
}
want_family() {
  local needle="$1" item
  for item in ${LUSDVIEW_SEMANTIC_FAMILIES:-preview openpbr standard}; do
    [ "$item" = "$needle" ] && return 0
  done
  return 1
}
declare -A prerendered_log=()
# Render every requested mode of ONE asset in a single process (--mode-sweep),
# writing the exact files case_run expects. lusdview gives each mode the same
# frame budget it would get in its own process, so the images are byte-identical
# to the one-process-per-mode path -- this only removes repeated process start
# and Vulkan device creation, which is ~90% of a small case's cost.
sweep_prerender() {
  local tag="$1" source="$2" prefix="$3" modes="$4"; shift 4
  local backend_key="$tag"
  if [ "${backend_unavailable[$backend_key]:-0}" = 1 ]; then return; fi
  [ -n "$modes" ] || return
  # Only backends whose screenshot comes from the in-loop viewport capture can
  # be swept. CUDA and HIP trace AFTER the frame loop and write the image
  # themselves ("CUDA RT wrote ..."), so a sweep would hand them the raster
  # viewport instead -- caught as failing cuda albedo/metallic/roughness probes.
  case "$backend_key" in
    gl|vk-raster|vk-rt) ;;
    *) return ;;
  esac
  local log="$OUT/$tag-$prefix-sweep.log"
  if [[ "$tag" = gl* ]]; then
    run "${GL_RUN[@]}" "$@" --config "$OUT/config.json" --size 256x256 --mode-sweep "$modes" --frames 4 --view-dir 0,0,-1 --screenshot "$OUT/$tag-$prefix-{mode}.ppm" "$source" >"$log" 2>&1
  else
    run "$@" --config "$OUT/config.json" --size 256x256 --mode-sweep "$modes" --frames 4 --view-dir 0,0,-1 --screenshot "$OUT/$tag-$prefix-{mode}.ppm" "$source" >"$log" 2>&1
  fi
  if [[ "$tag" = vk-rt* ]] &&
     grep -Eqi 'llvmpipe|lavapipe|\(cpu, driver|software rasterizer' "$log"; then
    backend_unavailable[$backend_key]=1
    degraded=1
    echo "SKIP: Vulkan RT on software Vulkan"
    return
  fi
  # Only claim the images the sweep actually produced; anything missing falls
  # back to its own render in case_run, so a sweep failure degrades rather than
  # silently dropping coverage.
  local m
  for m in ${modes//,/ }; do
    [ -s "$OUT/$tag-$prefix-$m.ppm" ] && prerendered_log["$OUT/$tag-$prefix-$m.ppm"]="$log"
  done
}

case_run() {
  local tag="$1" marker="$2" source="$3" mode="$4" kind="$5" case_id="$6"; shift 6
  local backend_key="$tag"
  if [ "${backend_unavailable[$backend_key]:-0}" = 1 ]; then return; fi
  # The main AOV shards exercise every requested semantic on ordinary external
  # textures.  Package parity is a separate shard and owns the duplicated UDIM
  # and USDZ cases.  Keeping the switches here lets focused/manual runs retain
  # the historical exhaustive matrix while CI avoids paying renderer startup
  # cost twice for equivalent semantic evaluation.
  if [ "${LUSDVIEW_SEMANTIC_UDIM:-1}" = 0 ] && [[ "$case_id" = *udim* ]]; then
    return
  fi
  local img="$OUT/$tag-$case_id.ppm" log="$OUT/$tag-$case_id.log"
  local run_rc=0
  if [ -n "${prerendered_log[$img]:-}" ] && [ -s "$img" ]; then
    # Already produced by sweep_prerender: one process rendered every mode of
    # this asset (--mode-sweep), byte-identical to rendering them separately.
    log="${prerendered_log[$img]}"
  elif [[ "$tag" = gl* ]]; then
    run "${GL_RUN[@]}" "$@" --config "$OUT/config.json" --size 256x256 --mode "$mode" --frames 4 --view-dir 0,0,-1 --screenshot "$img" "$source" >"$log" 2>&1
    run_rc=$?
  else
    run "$@" --config "$OUT/config.json" --size 256x256 --mode "$mode" --frames 4 --view-dir 0,0,-1 --screenshot "$img" "$source" >"$log" 2>&1
    run_rc=$?
  fi
  # Hardware ray-query semantic coverage is not meaningful on llvmpipe. The
  # raster path records this capability after its first render, but vk-rt can
  # otherwise enter the expensive RT matrix before that flag is set. Classify
  # the device from the first RT log and skip the remaining cases promptly.
  if [[ "$tag" = vk-rt* ]] &&
     grep -Eqi 'llvmpipe|lavapipe|\(cpu, driver|software rasterizer' "$log"; then
    backend_unavailable[$backend_key]=1
    degraded=1
    echo "SKIP: Vulkan RT on software Vulkan"
    return
  fi
  if grep -q '\[lusdview\]\[error\] load failed:' "$log"; then
    echo "FAIL: $tag $case_id asset load"; fail=1; return
  fi
  if ! grep -q "$marker" "$log"; then
    if [ "${backend_available[$backend_key]:-0}" = 1 ]; then
      echo "FAIL: $tag $kind stopped producing its backend marker (exit $run_rc)"
      fail=1
    else
      backend_unavailable[$backend_key]=1
      degraded=1
      if [ "$run_rc" -eq 124 ] || [ "$run_rc" -eq 137 ]; then
        echo "SKIP: $tag backend probe timed out; skipping its remaining cases"
      else
        echo "SKIP: $tag backend unavailable; skipping its remaining cases"
      fi
    fi
    return
  fi
  backend_available[$backend_key]=1
  if [ ! -s "$img" ]; then
    echo "FAIL: $tag $kind produced no screenshot"; fail=1; return
  fi
  [[ "$tag" = vk-raster* ]] && grep -Eqi 'llvmpipe|lavapipe|\(cpu, driver|software rasterizer' "$log" && vk_software=1
  ran=$((ran+1)); probe "$img" "$kind" || { echo "FAIL: $tag $kind"; fail=1; }
  if [ "${LUSDVIEW_SEMANTIC_PACKAGE_PARITY:-1}" != 0 ] &&
     [ "$case_id" = preview-vector ]; then
    local packed="$OUT/$tag-vector-usdz.ppm"
    if [[ "$tag" = gl* ]]; then
      run "${GL_RUN[@]}" "$@" --config "$OUT/config.json" --size 256x256 --mode normals --frames 4 --view-dir 0,0,-1 --screenshot "$packed" "$OUT/normal.usdz" >"$OUT/$tag-vector-usdz.log" 2>&1
    else
      run "$@" --config "$OUT/config.json" --size 256x256 --mode normals --frames 4 --view-dir 0,0,-1 --screenshot "$packed" "$OUT/normal.usdz" >"$OUT/$tag-vector-usdz.log" 2>&1
    fi
    [ -s "$packed" ] && compare "$img" "$packed" || { echo "FAIL: $tag external/USDZ"; fail=1; }
  fi
  local package=
  case "$case_id" in
    coat-normal) package="$OUT/coat-normal.usdz";;
    standard-coat-normal) package="$OUT/coat-normal-standard.usdz";;
    preview-albedo|preview-metallic|preview-roughness|preview-emissive)
      package="$OUT/core-preview.usdz";;
    openpbr-albedo|openpbr-metallic|openpbr-roughness|openpbr-emissive)
      package="$OUT/core-openpbr.usdz";;
    standard-albedo|standard-metallic|standard-roughness|standard-emissive)
      package="$OUT/core-standard.usdz";;
    preview-opacity) package="$OUT/opacity-preview.usdz";;
    openpbr-opacity) package="$OUT/opacity-openpbr.usdz";;
    standard-opacity) package="$OUT/opacity-standard.usdz";;
    preview-specular-f0) package="$OUT/specular-preview.usdz";;
    openpbr-specular-f0) package="$OUT/specular-openpbr.usdz";;
    standard-specular-f0) package="$OUT/specular-standard.usdz";;
    coat-weight|coat-color|coat-roughness) package="$OUT/coat.usdz";;
    preview-coat-weight|preview-coat-roughness)
      package="$OUT/coat-preview.usdz";;
    standard-coat-weight|standard-coat-color|standard-coat-roughness)
      package="$OUT/coat-standard.usdz";;
    openpbr-vector) package="$OUT/normal-openpbr.usdz";;
    standard-vector) package="$OUT/normal-standard.usdz";;
    occlusion) package="$OUT/occlusion.usdz";;
  esac
  if [ "${LUSDVIEW_SEMANTIC_PACKAGE_PARITY:-1}" != 0 ] &&
     [ -n "$package" ]; then
    local packed="$OUT/$tag-$case_id-usdz.ppm"
    if [[ "$tag" = gl* ]]; then
      run "${GL_RUN[@]}" "$@" --config "$OUT/config.json" --size 256x256 --mode "$mode" --frames 4 --view-dir 0,0,-1 --screenshot "$packed" "$package" >"$OUT/$tag-$case_id-usdz.log" 2>&1
    else
      run "$@" --config "$OUT/config.json" --size 256x256 --mode "$mode" --frames 4 --view-dir 0,0,-1 --screenshot "$packed" "$package" >"$OUT/$tag-$case_id-usdz.log" 2>&1
    fi
    [ -s "$packed" ] && compare "$img" "$packed" || { echo "FAIL: $tag $case_id external/USDZ"; fail=1; }
  fi
}
for loader in ${LUSDVIEW_SEMANTIC_LOADERS:-default}; do
  case "$loader" in
    default|core) loader_args=(); loader_suffix=;;
    *) echo "unknown loader: $loader" >&2; exit 2;;
  esac
  for backend in ${LUSDVIEW_SEMANTIC_BACKENDS:-gl vk-raster vk-rt cuda hip}; do
    case "$backend" in
      gl) args=("$BIN" --backend gl --texture-compress off --texture-mips off "${loader_args[@]}"); tag=gl$loader_suffix; marker='render stats';;
      vk-raster) args=("$BIN" --headless --backend vk "${VK_DEVICE_ARGS[@]}" --texture-compress off --texture-mips off "${loader_args[@]}"); tag=vk-raster$loader_suffix; marker='render stats';;
      vk-rt) [ "$vk_software" = 0 ] || { echo 'SKIP: Vulkan RT on software Vulkan'; continue; }; args=("$BIN" --headless --backend vk "${VK_DEVICE_ARGS[@]}" --rt --texture-compress off --texture-mips off "${loader_args[@]}"); tag=vk-rt$loader_suffix; marker='caps: v1 .*rt=hardware';;
      cuda) args=("$BIN" --headless --cuda --texture-compress off --texture-mips off "${loader_args[@]}"); tag=cuda$loader_suffix; marker='CUDA RT wrote';;
      hip) args=("$BIN" --headless --hip --texture-compress off --texture-mips off "${loader_args[@]}"); tag=hip$loader_suffix; marker='HIP RT wrote';;
      *) echo "unknown backend: $backend" >&2; exit 2;;
    esac
    for family in preview openpbr standard; do
      want_family "$family" || continue
      local_normal="$OUT/normal-$family.usda"
      [ "$family" != preview ] || local_normal="$OUT/normal.usda"
      want_mode vector && case_run "$tag" "$marker" "$local_normal" normals vector "$family-vector" "${args[@]}"
      want_mode vector && case_run "$tag" "$marker" "$OUT/normal-$family-udim.usda" normals vector "$family-udim-vector" "${args[@]}"
    done
    want_mode coat-normal && case_run "$tag" "$marker" "$OUT/coat-normal.usda" coat-normal coat-normal coat-normal "${args[@]}"
    want_mode coat-normal && case_run "$tag" "$marker" "$OUT/coat-normal-udim.usda" coat-normal coat-normal coat-normal-udim "${args[@]}"
    want_mode degenerate-normal && case_run "$tag" "$marker" "$OUT/normal-degenerate.usda" normals degenerate-normal degenerate-normal "${args[@]}"
    want_mode degenerate-coat-normal && case_run "$tag" "$marker" "$OUT/coat-normal-degenerate.usda" coat-normal degenerate-coat-normal degenerate-coat-normal "${args[@]}"
    if want_family standard; then
      want_mode coat-normal && case_run "$tag" "$marker" "$OUT/coat-normal-standard.usda" coat-normal coat-normal standard-coat-normal "${args[@]}"
      want_mode coat-normal && case_run "$tag" "$marker" "$OUT/coat-normal-standard-udim.usda" coat-normal coat-normal standard-coat-normal-udim "${args[@]}"
    fi
    want_mode occlusion && case_run "$tag" "$marker" "$OUT/occlusion.usda" shaded occlusion occlusion "${args[@]}"
    want_mode occlusion && case_run "$tag" "$marker" "$OUT/occlusion-udim.usda" shaded occlusion occlusion-udim "${args[@]}"
    want_mode coat && case_run "$tag" "$marker" "$OUT/coat.usda" shaded coat coat "${args[@]}"
    want_mode coat-weight && case_run "$tag" "$marker" "$OUT/coat.usda" coat-weight coat-weight coat-weight "${args[@]}"
    want_mode coat-color && case_run "$tag" "$marker" "$OUT/coat.usda" coat-color coat-color coat-color "${args[@]}"
    want_mode coat-roughness && case_run "$tag" "$marker" "$OUT/coat.usda" coat-roughness coat-roughness coat-roughness "${args[@]}"
    want_mode coat-weight && case_run "$tag" "$marker" "$OUT/coat-udim.usda" coat-weight coat-weight coat-weight-udim "${args[@]}"
    want_mode coat-color && case_run "$tag" "$marker" "$OUT/coat-udim.usda" coat-color coat-color coat-color-udim "${args[@]}"
    want_mode coat-roughness && case_run "$tag" "$marker" "$OUT/coat-udim.usda" coat-roughness coat-roughness coat-roughness-udim "${args[@]}"
    if want_family preview; then
      want_mode coat-weight && case_run "$tag" "$marker" "$OUT/coat-preview.usda" coat-weight coat-weight preview-coat-weight "${args[@]}"
      want_mode coat-roughness && case_run "$tag" "$marker" "$OUT/coat-preview.usda" coat-roughness coat-roughness preview-coat-roughness "${args[@]}"
      want_mode coat-weight && case_run "$tag" "$marker" "$OUT/coat-preview-udim.usda" coat-weight coat-weight preview-coat-weight-udim "${args[@]}"
      want_mode coat-roughness && case_run "$tag" "$marker" "$OUT/coat-preview-udim.usda" coat-roughness coat-roughness preview-coat-roughness-udim "${args[@]}"
    fi
    if want_family standard; then
      want_mode coat-weight && case_run "$tag" "$marker" "$OUT/coat-standard.usda" coat-weight coat-weight standard-coat-weight "${args[@]}"
      want_mode coat-color && case_run "$tag" "$marker" "$OUT/coat-standard.usda" coat-color coat-color standard-coat-color "${args[@]}"
      want_mode coat-roughness && case_run "$tag" "$marker" "$OUT/coat-standard.usda" coat-roughness coat-roughness standard-coat-roughness "${args[@]}"
      want_mode coat-weight && case_run "$tag" "$marker" "$OUT/coat-standard-udim.usda" coat-weight coat-weight standard-coat-weight-udim "${args[@]}"
      want_mode coat-color && case_run "$tag" "$marker" "$OUT/coat-standard-udim.usda" coat-color coat-color standard-coat-color-udim "${args[@]}"
      want_mode coat-roughness && case_run "$tag" "$marker" "$OUT/coat-standard-udim.usda" coat-roughness coat-roughness standard-coat-roughness-udim "${args[@]}"
    fi
    for family in preview openpbr standard; do
      want_family "$family" || continue
      want_mode specular-f0 && case_run "$tag" "$marker" "$OUT/specular-$family.usda" specular-f0 specular-f0 "$family-specular-f0" "${args[@]}"
    done
    for family in preview openpbr standard; do
      want_family "$family" || continue
      want_mode specular-f0 && case_run "$tag" "$marker" "$OUT/specular-$family-udim.usda" specular-f0 specular-f0 "$family-udim-specular-f0" "${args[@]}"
    done
    for family in preview openpbr standard; do
      want_family "$family" || continue
      want_mode ior-f0 && case_run "$tag" "$marker" "$OUT/ior-$family.usda" ior-f0 ior-f0 "$family-ior-f0" "${args[@]}"
    done
    for family in preview openpbr standard; do
      want_family "$family" || continue
      sweep_list=""
      for _m in albedo metallic roughness emissive; do
        want_mode "$_m" && sweep_list="${sweep_list:+$sweep_list,}$_m"
      done
      sweep_prerender "$tag" "$OUT/core-$family.usda" "$family" "$sweep_list" "${args[@]}"
      sweep_prerender "$tag" "$OUT/core-$family-udim.usda" "$family-udim" "$sweep_list" "${args[@]}"
      want_mode albedo && case_run "$tag" "$marker" "$OUT/core-$family.usda" albedo albedo "$family-albedo" "${args[@]}"
      want_mode metallic && case_run "$tag" "$marker" "$OUT/core-$family.usda" metallic metallic "$family-metallic" "${args[@]}"
      want_mode roughness && case_run "$tag" "$marker" "$OUT/core-$family.usda" roughness roughness "$family-roughness" "${args[@]}"
      want_mode emissive && case_run "$tag" "$marker" "$OUT/core-$family.usda" emissive emissive "$family-emissive" "${args[@]}"
      want_mode albedo && case_run "$tag" "$marker" "$OUT/core-$family-udim.usda" albedo albedo "$family-udim-albedo" "${args[@]}"
      want_mode metallic && case_run "$tag" "$marker" "$OUT/core-$family-udim.usda" metallic metallic "$family-udim-metallic" "${args[@]}"
      want_mode roughness && case_run "$tag" "$marker" "$OUT/core-$family-udim.usda" roughness roughness "$family-udim-roughness" "${args[@]}"
      want_mode emissive && case_run "$tag" "$marker" "$OUT/core-$family-udim.usda" emissive emissive "$family-udim-emissive" "${args[@]}"
    done
    # The raster opacity AOV intentionally writes alpha=1, but blended material
    # draws are excluded from that diagnostic pass. Use the controlled shaded
    # response so the authored alpha ramp is still measured end-to-end.
    for family in preview openpbr standard; do
      want_family "$family" || continue
      want_mode opacity && case_run "$tag" "$marker" "$OUT/opacity-$family.usda" shaded opacity "$family-opacity" "${args[@]}"
      want_mode opacity && case_run "$tag" "$marker" "$OUT/opacity-$family-udim.usda" shaded opacity "$family-udim-opacity" "${args[@]}"
    done
  done
done
[ "$ran" -gt 0 ] || exit "$SKIP"
[ "$fail" -eq 0 ] || exit 1
# A backend that dropped out part way through must NOT report a green pass: it
# ran some cases and silently abandoned the rest, which reads as coverage that
# was never actually exercised. Report it as skipped so CTest says so. (This
# suite once "passed" in 52 s that way, while a full GL run needs ~436 s.)
if [ "$degraded" -ne 0 ]; then
  echo "SKIP: a backend became unavailable part way through; $ran case(s) ran, "\
       "the rest were abandoned -- not reporting this as a pass"
  exit "$SKIP"
fi
echo 'PASS: requested semantic material AOVs, coat/occlusion response, loader comparisons, and USDZ texture parity'
