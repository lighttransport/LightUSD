// SPDX-License-Identifier: Apache-2.0
#include "lightrt_mtlx_graph_phases.hh"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lightrt_mtlx_bridge.hh"

namespace lusdview {
namespace {

bool IsMtlxTypeName(const std::string& s) {
  return s == "float" || s == "color3" || s == "color4" ||
         s == "vector2" || s == "vector3" || s == "vector4" ||
         s == "matrix33" || s == "matrix44" ||
         s == "integer" || s == "boolean" || s == "string" ||
         s == "filename";
}

std::string NormalizeMtlxType(const std::string& type) {
  if (IsMtlxTypeName(type)) return type;
  if (type == "float2") return "vector2";
  if (type == "float3") return "vector3";
  if (type == "float4") return "vector4";
  if (type == "color3f") return "color3";
  if (type == "color4f") return "color4";
  if (type == "asset") return "filename";
  if (type.rfind("ND_", 0) == 0) {
    const size_t last = type.rfind('_');
    if (last != std::string::npos && last + 1 < type.size()) {
      const std::string suffix = type.substr(last + 1);
      if (IsMtlxTypeName(suffix)) return suffix;
    }
  }
  return "float";
}

std::string JsonString(const nlohmann::json& obj, const char* key,
                       const std::string& fallback = std::string()) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return fallback;
  return it->get<std::string>();
}

std::string OpenPBREvalInputName(const std::string& name) {
  if (name == "roughness") return "specular_roughness";
  if (name == "metalness") return "base_metalness";
  if (name == "specular") return "specular_weight";
  if (name == "transmission") return "transmission_weight";
  if (name == "subsurface") return "subsurface_weight";
  if (name == "coat") return "coat_weight";
  if (name == "emission") return "emission_luminance";
  if (name == "base_roughness") return "specular_roughness";
  if (name == "opacity") return "geometry_opacity";
  if (name == "normal") return "geometry_normal";
  if (name == "tangent") return "geometry_tangent";
  if (name == "coat_normal") return "geometry_coat_normal";
  if (name == "coat_tangent") return "geometry_coat_tangent";
  return name;
}

std::string NormalizeMtlxCategory(const std::string& category,
                                  const std::string& type) {
  if (category == "MaterialXMultiply") return "multiply";
  if (category == "MaterialXMix") return "mix";
  if (category == "MaterialXNoise") return "noise3d";
  if (category == "MaterialXConstant") return "constant";
  if (category.rfind("ND_", 0) == 0) {
    std::string stem = category.substr(3);
    const std::string normalized_type = NormalizeMtlxType(type);
    if (!normalized_type.empty()) {
      const std::string suffix = "_" + normalized_type;
      if (stem.size() > suffix.size() &&
          stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
        stem.resize(stem.size() - suffix.size());
    }
    if (stem == "gltf_colorimage") return "gltf_colorimage";
    if (stem == "gltf_image") return "gltf_image";
    if (stem == "gltf_normalmap") return "gltf_normalmap";
    if (stem.rfind("swizzle_", 0) == 0) return "swizzle";
    if (stem == "open_pbr_surface_surfaceshader") return "open_pbr_surface";
    if (stem == "standard_surface_surfaceshader") return "standard_surface";
    if (stem == "UsdPreviewSurface_surfaceshader") return "UsdPreviewSurface";
    return stem;
  }
  return category;
}

nlohmann::json InputNamed(const nlohmann::json& node, const char* name,
                          const nlohmann::json& fallback) {
  const auto it = node.find("inputs");
  if (it != node.end() && it->is_array()) {
    for (const auto& input : *it) {
      if (JsonString(input, "name") == name) return input;
    }
  }
  return fallback;
}

}  // namespace

bool PackMaterialXGraphRuntimePhase(
    const nlohmann::json& j, const nlohmann::json& ng,
    const nlohmann::json& runtimeNodes,
    const std::map<std::string, std::map<std::string, std::string>>& closureLanes,
    MaterialXGraphRuntimeCPU* graphOut, std::string* err) {
  if (!graphOut) {
    if (err) *err = "MaterialX graph output is null";
    return false;
  }
  MaterialXGraphRuntimeCPU& graph = *graphOut;
  std::unordered_map<std::string, int> nodeIds;
  nodeIds.reserve(runtimeNodes.size());
  for (const nlohmann::json& node : runtimeNodes) {
    const std::string name = JsonString(node, "name");
    if (!name.empty() && !nodeIds.count(name))
      nodeIds[name] = static_cast<int>(nodeIds.size());
  }
  std::unordered_set<std::string> emittedNodes;
  emittedNodes.reserve(runtimeNodes.size());
  for (const nlohmann::json& node : runtimeNodes) {
    const std::string name = JsonString(node, "name");
    if (name.empty() || !emittedNodes.insert(name).second) continue;
    MaterialXGraphNodeCPU out;
    out.name = name;
    const std::string type = JsonString(node, "type");
    const std::string cat = NormalizeMtlxCategory(JsonString(node, "category"), type);
    if (cat == "constant") out.op = MaterialXGraphOpCPU::Constant;
    else if (cat == "image" || cat == "gltf_image" || cat == "gltf_colorimage") {
      out.op = MaterialXGraphOpCPU::Image;
      graph.hasImages = true;
    } else if (cat == "tiledimage" || cat == "hextiledimage") {
      out.op = MaterialXGraphOpCPU::TiledImage;
      graph.hasImages = true;
    } else if (cat == "normalmap" || cat == "gltf_normalmap" ||
               cat == "hextilednormalmap") {
      out.op = MaterialXGraphOpCPU::NormalMap;
      graph.hasImages = true;
    } else if (cat == "add" || cat == "plus") out.op = MaterialXGraphOpCPU::Add;
    else if (cat == "subtract" || cat == "minus") out.op = MaterialXGraphOpCPU::Subtract;
    else if (cat == "multiply") out.op = MaterialXGraphOpCPU::Multiply;
    else if (cat == "divide") out.op = MaterialXGraphOpCPU::Divide;
    else if (cat == "mix") out.op = MaterialXGraphOpCPU::Mix;
    else if (cat == "clamp") out.op = MaterialXGraphOpCPU::Clamp;
    else if (cat == "saturate") {
      out.op = MaterialXGraphOpCPU::Saturate;
      out.value[1][0] = out.value[1][1] =
          out.value[1][2] = out.value[1][3] = 1.0f;
    }
    else if (cat == "dot" || cat == "dotproduct") out.op = MaterialXGraphOpCPU::Dot;
    else if (cat == "normalize") out.op = MaterialXGraphOpCPU::Normalized;
    else if (cat == "power" || cat == "pow" || cat == "safepower")
      out.op = MaterialXGraphOpCPU::Power;
    else if (cat == "min" || cat == "minimum")
      out.op = MaterialXGraphOpCPU::Minimum;
    else if (cat == "max" || cat == "maximum")
      out.op = MaterialXGraphOpCPU::Maximum;
    else if (cat == "abs" || cat == "absval") out.op = MaterialXGraphOpCPU::Absolute;
    else if (cat == "sqrt") out.op = MaterialXGraphOpCPU::SquareRoot;
    else if (cat == "sin") out.op = MaterialXGraphOpCPU::Sine;
    else if (cat == "cos") out.op = MaterialXGraphOpCPU::Cosine;
    else if (cat == "luminance") out.op = MaterialXGraphOpCPU::Luminance;
    else if (cat == "select") out.op = MaterialXGraphOpCPU::Select;
    else if (cat == "ifgreater") out.op = MaterialXGraphOpCPU::IfGreater;
    else if (cat == "ifgreatereq" || cat == "ifgreaterequal")
      out.op = MaterialXGraphOpCPU::IfGreaterEqual;
    else if (cat == "ifequal") out.op = MaterialXGraphOpCPU::IfEqual;
    else if (cat == "texcoord" || cat == "texcoord0" || cat == "texcoord1") {
      out.op = MaterialXGraphOpCPU::Texcoord;
      // Preserve the explicit second-set form in the graph IR.  The z lane
      // of the third fallback value is otherwise unused by texcoord nodes;
      // the w lane remains reserved for image UV-input routing.
      out.value[2][2] = (cat == "texcoord1") ? 1.0f : 0.0f;
    }
    else if (cat == "floor") out.op = MaterialXGraphOpCPU::Floor;
    else if (cat == "ceil" || cat == "ceiling") out.op = MaterialXGraphOpCPU::Ceil;
    else if (cat == "fract" || cat == "fraction") out.op = MaterialXGraphOpCPU::Fract;
    else if (cat == "step") out.op = MaterialXGraphOpCPU::Step;
    else if (cat == "smoothstep") out.op = MaterialXGraphOpCPU::Smoothstep;
    else if (cat == "cross" || cat == "crossproduct") out.op = MaterialXGraphOpCPU::Cross;
    else if (cat == "length" || cat == "magnitude") out.op = MaterialXGraphOpCPU::Length;
    else if (cat == "noise3d") out.op = MaterialXGraphOpCPU::Noise3D;
    else if (cat == "noise2d" || cat == "noise")
      out.op = MaterialXGraphOpCPU::Noise2D;
    else if (cat == "tan") out.op = MaterialXGraphOpCPU::Tangent;
    else if (cat == "tangent") out.op = MaterialXGraphOpCPU::GeometricTangent;
    else if (cat == "normal") out.op = MaterialXGraphOpCPU::GeometricNormal;
    else if (cat == "rotate3d" || cat == "rotate")
      out.op = MaterialXGraphOpCPU::Rotate3D;
    else if (cat == "transform2d" || cat == "place2d" ||
             cat == "place2dtransform")
      out.op = MaterialXGraphOpCPU::Transform2D;
    else if (cat == "exp" || cat == "exponential")
      out.op = MaterialXGraphOpCPU::Exponential;
    else if (cat == "log" || cat == "ln" || cat == "logarithm")
      out.op = MaterialXGraphOpCPU::Logarithm;
    else if (cat == "modulo" || cat == "mod") out.op = MaterialXGraphOpCPU::Modulo;
    else if (cat == "invert") out.op = MaterialXGraphOpCPU::Invert;
    else if (cat == "oneminus") out.op = MaterialXGraphOpCPU::Invert;
    else if (cat == "remap" || cat == "range") out.op = MaterialXGraphOpCPU::Remap;
    else if (cat == "atan2" || cat == "arctan2") out.op = MaterialXGraphOpCPU::Atan2;
    else if (cat == "sign" || cat == "signum") out.op = MaterialXGraphOpCPU::Sign;
    else if (cat == "round") out.op = MaterialXGraphOpCPU::Round;
    else if (cat == "combine2" || cat == "combine3" || cat == "combine4")
      out.op = MaterialXGraphOpCPU::Combine;
    else if (cat == "extract" || cat == "separate" || cat == "separate2" ||
             cat == "separate3" || cat == "separate4")
      out.op = MaterialXGraphOpCPU::Extract;
    else if (cat.rfind("convert", 0) == 0)
      out.op = MaterialXGraphOpCPU::Convert;
    else if (cat == "position") out.op = MaterialXGraphOpCPU::Position;
    else if (cat == "geompropvalue" || cat == "geompropvalueuniform") {
      const std::string prop=JsonString(InputNamed(node,"geomprop",{}),"value");
      if(prop=="st"||prop=="uv"||prop=="texcoord") out.op=MaterialXGraphOpCPU::Texcoord;
      else if(prop=="displayColor"||prop=="Cd"||prop=="color") out.op=MaterialXGraphOpCPU::GeomColor;
      else if(prop=="P"||prop=="position") out.op=MaterialXGraphOpCPU::Position;
      else if(prop=="N"||prop=="normal") out.op=MaterialXGraphOpCPU::GeometricNormal;
      else if(prop=="tangent") out.op=MaterialXGraphOpCPU::GeometricTangent;
      else if(prop=="bitangent") out.op=MaterialXGraphOpCPU::Bitangent;
      else {
        out.op = MaterialXGraphOpCPU::GeomProp;
        out.geomPropName = prop;
        const bool matrixColumn = node.contains("matrix_column");
        out.auxValue[1] = matrixColumn
                              ? node.value("matrix_components", 16.0f)
                              : 1.0f;
        out.auxValue[2] = matrixColumn
                              ? node.value("matrix_column", 0.0f)
                              : 0.0f;
        const std::string valueType = JsonString(node, "type");
        if (!matrixColumn) {
          out.auxValue[1] = valueType == "vector2" ? 2.0f :
                            valueType == "vector3" || valueType == "color3" ? 3.0f :
                            valueType == "vector4" || valueType == "color4" ? 4.0f : 1.0f;
        }
      }
    }
    else if (cat == "viewdirection" || cat == "viewdir")
      out.op = MaterialXGraphOpCPU::ViewDirection;
    else if (cat == "time") out.op = MaterialXGraphOpCPU::Time;
    else if (cat == "frame") out.op = MaterialXGraphOpCPU::Frame;
    else if (cat == "blackbody") out.op = MaterialXGraphOpCPU::Blackbody;
    else if (cat == "roughness_anisotropy")
      out.op = MaterialXGraphOpCPU::RoughnessAnisotropy;
    else if (cat == "roughness_dual")
      out.op = MaterialXGraphOpCPU::RoughnessDual;
    else if (cat == "artisticiorcore")
      out.op = MaterialXGraphOpCPU::ArtisticIor;
    else if (cat == "chianghairabsorptioncore")
      out.op = MaterialXGraphOpCPU::ChiangHairAbsorption;
    else if (cat == "hsvadjust") {
      out.op = MaterialXGraphOpCPU::HsvAdjust;
      out.value[1][1] = out.value[1][2] = 1.0f;
    }
    else if (cat == "rgbtohsv") out.op = MaterialXGraphOpCPU::RgbToHsv;
    else if (cat == "hsvtorgb") out.op = MaterialXGraphOpCPU::HsvToRgb;
    else if (cat == "rotate2d") out.op = MaterialXGraphOpCPU::Rotate2D;
    else if (cat == "distance") out.op = MaterialXGraphOpCPU::Distance;
    else if (cat == "reflect") out.op = MaterialXGraphOpCPU::Reflect;
    else if (cat == "refract") out.op = MaterialXGraphOpCPU::Refract;
    else if (cat == "premult") out.op = MaterialXGraphOpCPU::Premult;
    else if (cat == "unpremult") out.op = MaterialXGraphOpCPU::Unpremult;
    else if (cat == "mincomponent") out.op = MaterialXGraphOpCPU::MinComponent;
    else if (cat == "maxcomponent") out.op = MaterialXGraphOpCPU::MaxComponent;
    else if (cat == "and") out.op = MaterialXGraphOpCPU::LogicalAnd;
    else if (cat == "or") out.op = MaterialXGraphOpCPU::LogicalOr;
    else if (cat == "xor") out.op = MaterialXGraphOpCPU::LogicalXor;
    else if (cat == "not") out.op = MaterialXGraphOpCPU::LogicalNot;
    else if (cat == "inside") out.op = MaterialXGraphOpCPU::Inside;
    else if (cat == "outside") out.op = MaterialXGraphOpCPU::Outside;
    else if (cat == "geomcolor") {
      out.op = MaterialXGraphOpCPU::GeomColor;
    }
    else if (cat == "bitangent") out.op = MaterialXGraphOpCPU::Bitangent;
    else if (cat == "difference" || cat == "in" || cat == "mask" ||
             cat == "matte" || cat == "out" || cat == "over" ||
             cat == "disjointover") {
      out.op = cat == "difference" ? MaterialXGraphOpCPU::Difference :
               cat == "in" ? MaterialXGraphOpCPU::In :
               cat == "mask" ? MaterialXGraphOpCPU::Mask :
               cat == "matte" ? MaterialXGraphOpCPU::Matte :
               cat == "out" ? MaterialXGraphOpCPU::Out :
               cat == "over" ? MaterialXGraphOpCPU::Over :
               MaterialXGraphOpCPU::DisjointOver;
      out.value[2][0] = out.value[2][1] =
          out.value[2][2] = out.value[2][3] = 1.0f;
    }
    else if (cat == "setalpha") out.op = MaterialXGraphOpCPU::SetAlpha;
    else if (cat == "cellnoise2d") out.op = MaterialXGraphOpCPU::CellNoise2D;
    else if (cat == "cellnoise3d") out.op = MaterialXGraphOpCPU::CellNoise3D;
    else if (cat == "fractal2dcore") out.op = MaterialXGraphOpCPU::Fractal2D;
    else if (cat == "worleynoise2d") out.op = MaterialXGraphOpCPU::WorleyNoise2D;
    else if (cat == "worleynoise3d") out.op = MaterialXGraphOpCPU::WorleyNoise3D;
    else if (cat == "fractal3dcore") out.op = MaterialXGraphOpCPU::Fractal3D;
    else if (cat == "cloverleaf") out.op = MaterialXGraphOpCPU::Cloverleaf;
    else if (cat == "hexagon") out.op = MaterialXGraphOpCPU::Hexagon;
    else if (cat=="gridcore"||cat=="gridstaggeredcore") out.op=MaterialXGraphOpCPU::Grid;
    else if (cat=="crosshatchcore"||cat=="crosshatchstaggeredcore") out.op=MaterialXGraphOpCPU::Crosshatch;
    else if (cat=="tiledcirclescore"||cat=="tiledcirclesstaggeredcore") out.op=MaterialXGraphOpCPU::TiledCircles;
    else if (cat=="tiledcloverleafscore"||cat=="tiledcloverleafsstaggeredcore") out.op=MaterialXGraphOpCPU::TiledCloverleafs;
    else if (cat=="tiledhexagonscore"||cat=="tiledhexagonsstaggeredcore") out.op=MaterialXGraphOpCPU::TiledHexagons;
    else if(cat=="rampcoord")out.op=MaterialXGraphOpCPU::RampCoordinate;
    else if(cat=="rampcore")out.op=MaterialXGraphOpCPU::Ramp;
    else if(cat=="rampgradientcore")out.op=MaterialXGraphOpCPU::RampGradient;
    else if(cat=="flakecore")out.op=MaterialXGraphOpCPU::Flake;
    else if(cat=="matrixtransformcore")out.op=MaterialXGraphOpCPU::MatrixTransform;
    else if(cat=="matrixtransposecore")out.op=MaterialXGraphOpCPU::MatrixTranspose;
    else if(cat=="matrixinversecore")out.op=MaterialXGraphOpCPU::MatrixInverse;
    else if(cat=="matrixdeterminantcore")out.op=MaterialXGraphOpCPU::MatrixDeterminant;
    else if (cat == "heighttonormal")
      out.op = MaterialXGraphOpCPU::HeightToNormal;
    else if (cat == "asin" || cat == "arcsin")
      out.op = MaterialXGraphOpCPU::Arcsine;
    else if (cat == "acos" || cat == "arccos")
      out.op = MaterialXGraphOpCPU::Arccosine;
    else if (cat == "atan" || cat == "arctan")
      out.op = MaterialXGraphOpCPU::Arctangent;
    else if (cat == "contrast")
      out.op = MaterialXGraphOpCPU::Contrast;
    else if (cat == "screen") {
      out.op = MaterialXGraphOpCPU::Screen;
      out.value[2][0] = out.value[2][1] =
          out.value[2][2] = out.value[2][3] = 1.0f;
    }
    else if (cat == "overlay") {
      out.op = MaterialXGraphOpCPU::Overlay;
      out.value[2][0] = out.value[2][1] =
          out.value[2][2] = out.value[2][3] = 1.0f;
    }
    else if (cat == "burn") {
      out.op = MaterialXGraphOpCPU::Burn;
      out.value[2][0] = out.value[2][1] =
          out.value[2][2] = out.value[2][3] = 1.0f;
    }
    else if (cat == "dodge") {
      out.op = MaterialXGraphOpCPU::Dodge;
      out.value[2][0] = out.value[2][1] =
          out.value[2][2] = out.value[2][3] = 1.0f;
    }
    else if (cat == "ramplr")
      out.op = MaterialXGraphOpCPU::RampLR;
    else if (cat == "ramptb")
      out.op = MaterialXGraphOpCPU::RampTB;
    else if (cat == "splitlr")
      out.op = MaterialXGraphOpCPU::SplitLR;
    else if (cat == "splittb")
      out.op = MaterialXGraphOpCPU::SplitTB;
    else if (cat == "swizzle" || cat.rfind("swizzle_", 0) == 0) {
      out.op = MaterialXGraphOpCPU::Swizzle;
      out.value[1][0] = 0.0f;
      out.value[1][1] = 1.0f;
      out.value[1][2] = 2.0f;
      out.value[1][3] = 3.0f;
    }
    if (out.op == MaterialXGraphOpCPU::Unknown) {
      if (err) *err = "Unsupported MaterialX graph node category: " + cat;
      return false;
    }
    if (out.op == MaterialXGraphOpCPU::Image ||
        out.op == MaterialXGraphOpCPU::TiledImage)
      out.value[2][3] = -1.0f;
    const auto inputsIt = node.find("inputs");
    int nextInput = 0;
    int uvInput = -1;
    bool usedInput[3]{false, false, false};
    if (inputsIt != node.end() && inputsIt->is_array()) {
      for (const nlohmann::json& input : *inputsIt) {
        const std::string inputName = JsonString(input, "name");
        const auto valueIt = input.find("value");
        const std::string inputType = NormalizeMtlxType(JsonString(input, "type"));
        const bool conditional = cat == "ifgreater" || cat == "ifgreatereq" ||
                                 cat == "ifgreaterequal" || cat == "ifequal";
        if (((cat == "splitlr" || cat == "splittb") &&
             inputName == "texcoord") ||
            (conditional && inputName == "in2") ||
            ((cat == "fractal2dcore" || cat == "fractal3dcore") &&
             inputName == "diminish") ||
            ((cat.find("core")!=std::string::npos) &&
             (inputName=="thickness"||inputName=="size"))) {
          const std::string source = JsonString(input, "nodename");
          const auto found = nodeIds.find(source);
          if (found != nodeIds.end()) out.auxInput = found->second;
          if (valueIt != input.end()) {
            if (valueIt->is_number()) {
              const float v = valueIt->get<float>();
              for (float& lane : out.auxValue) lane = v;
            } else if (valueIt->is_array()) {
              for (size_t c = 0; c < valueIt->size() && c < 4; ++c)
                if ((*valueIt)[c].is_number())
                  out.auxValue[c] = (*valueIt)[c].get<float>();
            }
          }
          continue;
        }
        if ((cat == "swizzle" || cat.rfind("swizzle_", 0) == 0) &&
            inputName == "channels" &&
            valueIt != input.end() && valueIt->is_string()) {
          const std::string channels = valueIt->get<std::string>();
          auto selector = [](char ch) -> float {
            switch (ch) {
              case 'r': case 'x': return 0.0f;
              case 'g': case 'y': return 1.0f;
              case 'b': case 'z': return 2.0f;
              case 'a': case 'w': return 3.0f;
              case '0': return 4.0f;
              case '1': return 5.0f;
              default: return 0.0f;
            }
          };
          for (size_t lane = 0; lane < channels.size() && lane < 4; ++lane)
            out.value[1][lane] = selector(channels[lane]);
          continue;
        }
        if (inputName == "file" || inputType == "filename") {
          out.imagePath = JsonString(input, "value");
          if (!out.imagePath.empty()) graph.hasImages = true;
          continue;
        }
        // Tiled-image graphs commonly author a local UV scale/offset on the
        // image node. Keep these controls out of the value-input arity so the
        // graph's arithmetic inputs retain their canonical indices.
        const bool uvControlNode = cat == "image" || cat == "tiledimage" ||
            cat == "hextiledimage" || cat == "transform2d" ||
            cat == "place2d" || cat == "place2dtransform";
        if (uvControlNode && valueIt != input.end() && valueIt->is_array() &&
            (inputName == "scale" || inputName == "uv_scale" ||
             inputName == "offset" || inputName == "uv_offset")) {
          float* dst = (inputName == "offset" || inputName == "uv_offset")
                           ? out.uvOffset : out.uvScale;
          for (size_t c = 0; c < valueIt->size() && c < 2; ++c)
            if ((*valueIt)[c].is_number()) dst[c] = (*valueIt)[c].get<float>();
          continue;
        }
        if (valueIt != input.end() && valueIt->is_number() &&
            (inputName == "rotation" || inputName == "rotate" ||
             inputName == "angle") &&
            (cat == "transform2d" || cat == "place2d" ||
             cat == "place2dtransform")) {
          // The packed node has no spare scalar lane. Transform2D's value[2].w
          // is reserved for this authored rotation (degrees); other nodes keep
          // their normal fallback value untouched.
          out.value[2][3] = valueIt->get<float>();
          continue;
        }
        int inputSlot = -1;
        if ((cat == "rotate3d" || cat == "rotate") && inputName == "amount")
          inputSlot = 0;
        else if ((cat == "rotate3d" || cat == "rotate") && inputName == "axis")
          inputSlot = 1;
        else if ((cat == "rotate3d" || cat == "rotate") && inputName == "in")
          inputSlot = 2;
        else if (cat == "rotate2d" && inputName == "in") inputSlot = 0;
        else if (cat == "rotate2d" && inputName == "amount") inputSlot = 1;
        else if (cat == "clamp" && inputName == "low") inputSlot = 1;
        else if (cat == "clamp" && inputName == "high") inputSlot = 2;
        else if (cat == "mix" && (inputName == "bg" || inputName == "in1"))
          inputSlot = 0;
        else if (cat == "mix" && (inputName == "fg" || inputName == "in2"))
          inputSlot = 1;
        else if (cat == "mix" && (inputName == "mix" || inputName == "amount"))
          inputSlot = 2;
        else if (cat == "saturate" && inputName == "in")
          inputSlot = 0;
        else if (cat == "saturate" && inputName == "amount")
          inputSlot = 1;
        else if (cat == "hsvadjust" && inputName == "in") inputSlot = 0;
        else if (cat == "hsvadjust" && inputName == "amount") inputSlot = 1;
        else if (cat == "contrast" && inputName == "in") inputSlot = 0;
        else if (cat == "contrast" && inputName == "amount") inputSlot = 1;
        else if (cat == "contrast" && inputName == "pivot") inputSlot = 2;
        else if (cat == "setalpha" && inputName == "in") inputSlot = 0;
        else if (cat == "setalpha" && inputName == "alpha") inputSlot = 1;
        else if (cat == "fractal2dcore" && inputName == "texcoord") inputSlot = 0;
        else if (cat == "fractal2dcore" && inputName == "octaves") inputSlot = 1;
        else if (cat == "fractal2dcore" && inputName == "lacunarity") inputSlot = 2;
        else if (cat == "fractal3dcore" && inputName == "position") inputSlot = 0;
        else if (cat == "fractal3dcore" && inputName == "octaves") inputSlot = 1;
        else if (cat == "fractal3dcore" && inputName == "lacunarity") inputSlot = 2;
        else if ((cat == "noise2d" || cat == "noise3d") &&
                 (inputName == "texcoord" || inputName == "position")) inputSlot = 0;
        else if ((cat == "noise2d" || cat == "noise3d") && inputName == "amplitude") inputSlot = 1;
        else if ((cat == "noise2d" || cat == "noise3d") && inputName == "pivot") inputSlot = 2;
        else if ((cat == "cloverleaf" || cat == "hexagon") && inputName == "texcoord") inputSlot = 0;
        else if ((cat == "cloverleaf" || cat == "hexagon") && inputName == "center") inputSlot = 1;
        else if ((cat == "cloverleaf" || cat == "hexagon") && inputName == "radius") inputSlot = 2;
        else if (cat.find("core")!=std::string::npos && inputName=="texcoord") inputSlot=0;
        else if (cat.find("core")!=std::string::npos && inputName=="uvtiling") inputSlot=1;
        else if (cat.find("core")!=std::string::npos && inputName=="uvoffset") inputSlot=2;
        else if(cat=="rampcoord"&&inputName=="texcoord")inputSlot=0;
        else if(cat=="rampcoord"&&inputName=="type")inputSlot=1;
        else if((cat=="rampcore"||cat=="rampgradientcore")&&inputName=="x")inputSlot=0;
        else if((cat=="rampcore"||cat=="rampgradientcore")&&inputName=="interpolation")inputSlot=1;
        else if((cat=="rampcore"||cat=="rampgradientcore")&&inputName=="num_intervals")inputSlot=2;
        else if ((cat == "worleynoise2d" || cat == "worleynoise3d") &&
                 (inputName == "texcoord" || inputName == "position")) inputSlot = 0;
        else if ((cat == "worleynoise2d" || cat == "worleynoise3d") &&
                 inputName == "jitter") inputSlot = 1;
        else if ((cat == "worleynoise2d" || cat == "worleynoise3d") &&
                 inputName == "style") inputSlot = 2;
        else if (conditional && inputName == "value1") inputSlot = 0;
        else if (conditional && inputName == "value2") inputSlot = 1;
        else if (conditional && inputName == "in1") inputSlot = 2;
        else if (cat == "select" && (inputName == "in" || inputName == "which"))
          inputSlot = 0;
        else if (cat == "select" && inputName == "in1") inputSlot = 1;
        else if (cat == "select" && inputName == "in2") inputSlot = 2;
        else if ((cat == "screen" || cat == "overlay" || cat == "burn" ||
                  cat == "dodge" || cat == "difference" || cat == "in" ||
                  cat == "mask" || cat == "matte" || cat == "out" ||
                  cat == "over" || cat == "disjointover") && inputName == "fg")
          inputSlot = 0;
        else if ((cat == "screen" || cat == "overlay" || cat == "burn" ||
                  cat == "dodge" || cat == "difference" || cat == "in" ||
                  cat == "mask" || cat == "matte" || cat == "out" ||
                  cat == "over" || cat == "disjointover") && inputName == "bg")
          inputSlot = 1;
        else if ((cat == "screen" || cat == "overlay" || cat == "burn" ||
                  cat == "dodge" || cat == "difference" || cat == "in" ||
                  cat == "mask" || cat == "matte" || cat == "out" ||
                  cat == "over" || cat == "disjointover") && inputName == "mix")
          inputSlot = 2;
        else if (cat == "ramplr" && inputName == "valuel")
          inputSlot = 0;
        else if (cat == "ramplr" && inputName == "valuer")
          inputSlot = 1;
        else if (cat == "ramptb" && inputName == "valuet")
          inputSlot = 0;
        else if (cat == "ramptb" && inputName == "valueb")
          inputSlot = 1;
        else if ((cat == "ramplr" || cat == "ramptb") &&
                 inputName == "texcoord")
          inputSlot = 2;
        else if (cat == "splitlr" && inputName == "valuel")
          inputSlot = 0;
        else if (cat == "splitlr" && inputName == "valuer")
          inputSlot = 1;
        else if (cat == "splittb" && inputName == "valuet")
          inputSlot = 0;
        else if (cat == "splittb" && inputName == "valueb")
          inputSlot = 1;
        else if ((cat == "splitlr" || cat == "splittb") &&
                 inputName == "center")
          inputSlot = 2;
        else if (inputName == "in" || inputName == "in1" ||
            inputName == "value" || inputName == "color" ||
            inputName == "position" || inputName == "texcoord" ||
            inputName == "uv" || inputName == "st" || inputName == "coord")
          inputSlot = 0;
        else if (inputName == "in2" || inputName == "amount" ||
                 inputName == "index" || inputName == "lacunarity" ||
                 inputName == "scale")
          inputSlot = 1;
        else if (inputName == "in3" || inputName == "octaves")
          inputSlot = 2;
        if (inputSlot < 0) {
          while (nextInput < 3 && usedInput[nextInput]) ++nextInput;
          inputSlot = nextInput;
        }
        if (inputSlot < 0 || inputSlot >= 3) continue;
        usedInput[inputSlot] = true;
        nextInput = std::max(nextInput, inputSlot + 1);
        // Preserve connected graph coordinates for image nodes. The runtime
        // interpreters use this metadata instead of silently sampling the hit
        // UV whenever an image has a texcoord/place2d input.
        if ((inputName == "texcoord" || inputName == "uv" ||
             inputName == "st" || inputName == "coord") &&
            (cat == "image" || cat == "tiledimage" ||
             cat == "hextiledimage")) {
          uvInput = inputSlot;
        }
        std::string source = JsonString(input, "nodename");
        const std::string sourceOutput = JsonString(input, "output");
        if(!source.empty()&&!sourceOutput.empty()&&sourceOutput!="out")
          source += "__" + sourceOutput;
        if (!source.empty()) {
          const auto found = nodeIds.find(source);
          if (found != nodeIds.end()) out.input[inputSlot] = found->second;
        }
        if (valueIt != input.end()) {
          if (valueIt->is_number() || valueIt->is_boolean()) {
            // MaterialX promotes scalar inputs lane-wise when a polymorphic
            // vector/color operation consumes them. Keeping only x made
            // colorcorrect gamma/gain/exposure affect red while green and blue
            // saw the record's unrelated zero/one defaults.
            const float scalar = valueIt->is_boolean()
                                     ? (valueIt->get<bool>() ? 1.0f : 0.0f)
                                     : valueIt->get<float>();
            for (float& lane : out.value[inputSlot]) lane = scalar;
          }
          else if (valueIt->is_array()) {
            for (size_t c = 0; c < valueIt->size() && c < 4; ++c)
              if ((*valueIt)[c].is_number())
                out.value[inputSlot][c] = (*valueIt)[c].get<float>();
          }
        }
      }
    }
    if (cat == "fractal2dcore" || cat == "fractal3dcore") {
      out.auxValue[3] = (type.find('4') != std::string::npos) ? 4.0f :
                        (type.find('3') != std::string::npos) ? 3.0f :
                        (type.find('2') != std::string::npos) ? 2.0f : 1.0f;
    }
    if (cat == "worleynoise2d" || cat == "worleynoise3d") {
      out.value[2][1] = (type.find('3') != std::string::npos) ? 3.0f :
                        (type.find('2') != std::string::npos) ? 2.0f : 1.0f;
    }
    if (cat == "noise2d" || cat == "noise3d") {
      out.value[2][3] = (type.find('4') != std::string::npos) ? 4.0f :
                        (type.find('3') != std::string::npos) ? 3.0f :
                        (type.find('2') != std::string::npos) ? 2.0f : 1.0f;
    }
    if(cat=="gridstaggeredcore"||cat=="crosshatchstaggeredcore"||
       cat=="tiledcirclesstaggeredcore"||cat=="tiledcloverleafsstaggeredcore"||
       cat=="tiledhexagonsstaggeredcore")out.auxValue[1]=1.0f;
    if(cat=="rampcore"||cat=="rampgradientcore"){
      const auto first=nodeIds.find(name+(cat=="rampcore"?"__interval1":"__interval1"));
      if(first!=nodeIds.end())out.auxValue[0]=static_cast<float>(first->second);
    }
    if(cat=="flakecore"){
      const size_t marker=name.rfind("__");
      const std::string base=marker==std::string::npos?name:name.substr(0,marker);
      const auto first=nodeIds.find(base+"__size");
      if(first!=nodeIds.end())out.auxValue[0]=static_cast<float>(first->second);
      out.auxValue[1]=node.value("flake_output",0);
      out.auxValue[2]=node.value("flake_3d",false)?1.0f:0.0f;
    }
    if(cat=="artisticiorcore")out.auxValue[0]=node.value("artistic_output",0);
    if(cat=="matrixtransformcore"||cat=="matrixtransposecore"||
       cat=="matrixinversecore"||cat=="matrixdeterminantcore"){
      const auto source=nodeIds.find(node.value("matrix_source",std::string()));
      if(source!=nodeIds.end())out.auxValue[0]=static_cast<float>(source->second);
      out.auxValue[1]=static_cast<float>(node.value("matrix_dim",4));
      out.auxValue[2]=static_cast<float>(node.value("matrix_column",0));
      out.auxValue[3]=(type.find('4')!=std::string::npos)?4.0f:
                      (type.find('3')!=std::string::npos)?3.0f:
                      (type.find('2')!=std::string::npos)?2.0f:1.0f;
    }
    if (uvInput >= 0) out.value[2][3] = static_cast<float>(uvInput);
    graph.nodes.push_back(std::move(out));
  }
  if (graph.nodes.empty()) {
    if (err) *err = "MaterialX graph node list is empty";
    return false;
  }
  std::map<std::string, std::string> outputs;
  const auto outputsIt = ng.find("outputs");
  if (outputsIt != ng.end() && outputsIt->is_array()) {
    for (const nlohmann::json& output : *outputsIt) {
      const std::string name = JsonString(output, "name");
      std::string node = JsonString(output, "nodename");
      const std::string selectedOutput=JsonString(output,"output");
      if(!node.empty()&&!selectedOutput.empty()&&selectedOutput!="out")
        node += "__"+selectedOutput;
      if (!name.empty() && !node.empty()) outputs[name] = node;
    }
  }
  const auto connIt = j.find("connections");
  const std::map<std::string, int> closureLaneOutput = {
      {"base_color",0},{"base_metalness",1},{"specular_roughness",2},
      {"geometry_opacity",3},{"emission_color",4},{"geometry_normal",5},
      {"subsurface_weight",6},{"subsurface_color",7},{"subsurface_radius",8},
      {"specular_weight",9},{"specular_color",10},{"transmission_weight",11},
      {"transmission_color",12},{"coat_weight",13},{"coat_color",14},
      {"coat_roughness",15},{"sheen_weight",16},{"sheen_color",17},
      {"sheen_roughness",18},{"specular_ior",19},{"base_weight",20},
      {"base_diffuse_roughness",21},{"transmission_scatter",22},
      {"transmission_depth",23},{"transmission_scatter_anisotropy",24},
      {"subsurface_scale",25},{"subsurface_anisotropy",26},{"coat_ior",27},
      {"thin_film_weight",28},{"thin_film_thickness",29},{"thin_film_ior",30},
      {"specular_anisotropy",31},{"specular_rotation",32},
      {"specular_roughness_anisotropy",33},{"transmission_dispersion",34},
      {"transmission_dispersion_abbe_number",35},
      {"transmission_dispersion_scale",36},{"coat_anisotropy",37},
      {"coat_rotation",38},{"coat_roughness_anisotropy",39},
      {"volume_density",40},{"volume_albedo",41},
      {"volume_emission_color",42},{"volume_emission_scale",43},
      {"emission_luminance",44},{"coat_affect_color",45},
      {"coat_affect_roughness",46},{"coat_darkening",47},
      {"subsurface_scatter_anisotropy",48}};
  int subsurfaceRadiusScaleNode = -1;
  if (connIt != j.end() && connIt->is_array()) {
    for (const nlohmann::json& connection : *connIt) {
      const std::string input = OpenPBREvalInputName(
          JsonString(connection, "input"));
      const auto outputIt = outputs.find(JsonString(connection, "output"));
      if (outputIt == outputs.end()) continue;
      if (input == "bsdf" || input == "edf" || input == "vdf" ||
          input == "material" || input == "surfaceshader" ||
          input == "volumeshader") {
        const auto closure = closureLanes.find(outputIt->second);
        if (closure == closureLanes.end()) continue;
        for (const auto& lane : closure->second) {
          const auto route = closureLaneOutput.find(lane.first);
          const auto node = nodeIds.find(lane.second);
          if (route != closureLaneOutput.end() && node != nodeIds.end())
            graph.output[static_cast<size_t>(route->second)] = node->second;
        }
        continue;
      }
      const auto closure = closureLanes.find(outputIt->second);
      if (closure != closureLanes.end()) {
        const auto lane = closure->second.find(input);
        const auto node = lane == closure->second.end()
                              ? nodeIds.end() : nodeIds.find(lane->second);
        const auto route = closureLaneOutput.find(input);
        if (lane != closure->second.end() && node != nodeIds.end() &&
            route != closureLaneOutput.end()) {
          graph.output[static_cast<size_t>(route->second)] = node->second;
          continue;
        }
      }
      const auto nodeIt = nodeIds.find(outputIt->second);
      if (nodeIt == nodeIds.end()) continue;
      if (input == "subsurface_radius_scale") {
        // The fixed graph ABI has a vector radius lane but no separate radius
        // scale lane. Preserve the authored vector by composing it with the
        // radius node after all connections have been collected; routing it
        // through the scalar subsurface_scale lane loses two components.
        subsurfaceRadiusScaleNode = nodeIt->second;
        continue;
      }
      int* destination = nullptr;
      if (input == "base_color") destination = &graph.output[0];
      else if (input == "base_metalness") destination = &graph.output[1];
      else if (input == "specular_roughness") destination = &graph.output[2];
      else if (input == "geometry_opacity") destination = &graph.output[3];
      else if (input == "emission_color") destination = &graph.output[4];
      else if (input == "geometry_normal") destination = &graph.output[5];
      else if (input == "subsurface_weight") destination = &graph.output[6];
      else if (input == "subsurface_color") destination = &graph.output[7];
      else if (input == "subsurface_radius") destination = &graph.output[8];
      else if (input == "specular_weight") destination = &graph.output[9];
      else if (input == "specular_color") destination = &graph.output[10];
      else if (input == "transmission_weight") destination = &graph.output[11];
      else if (input == "transmission_color") destination = &graph.output[12];
      else if (input == "coat_weight") destination = &graph.output[13];
      else if (input == "coat_color") destination = &graph.output[14];
      else if (input == "coat_roughness") destination = &graph.output[15];
      else if (input == "fuzz_weight" || input == "sheen_weight")
        destination = &graph.output[16];
      else if (input == "fuzz_color" || input == "sheen_color")
        destination = &graph.output[17];
      else if (input == "fuzz_roughness" || input == "sheen_roughness")
        destination = &graph.output[18];
      else if (input == "specular_ior") destination = &graph.output[19];
      else if (input == "base_weight") destination = &graph.output[20];
      else if (input == "base_diffuse_roughness" ||
               input == "diffuse_roughness") destination = &graph.output[21];
      else if (input == "transmission_scatter") destination = &graph.output[22];
      else if (input == "transmission_depth") destination = &graph.output[23];
      else if (input == "transmission_scatter_anisotropy")
        destination = &graph.output[24];
      else if (input == "subsurface_scale") destination = &graph.output[25];
      else if (input == "subsurface_anisotropy")
        destination = &graph.output[26];
      else if (input == "subsurface_scatter_anisotropy")
        destination = &graph.output[48];
      else if (input == "coat_ior") destination = &graph.output[27];
      else if (input == "thin_film_weight") destination = &graph.output[28];
      else if (input == "thin_film_thickness") destination = &graph.output[29];
      else if (input == "thin_film_ior") destination = &graph.output[30];
      else if (input == "specular_anisotropy") destination = &graph.output[31];
      else if (input == "specular_rotation") destination = &graph.output[32];
      else if (input == "specular_roughness_anisotropy")
        destination = &graph.output[33];
      else if (input == "transmission_dispersion") destination = &graph.output[34];
      else if (input == "transmission_dispersion_abbe_number")
        destination = &graph.output[35];
      else if (input == "transmission_dispersion_scale")
        destination = &graph.output[36];
      else if (input == "coat_anisotropy") destination = &graph.output[37];
      else if (input == "coat_rotation") destination = &graph.output[38];
      else if (input == "coat_roughness_anisotropy")
        destination = &graph.output[39];
      else if (input == "volume_density") destination = &graph.output[40];
      else if (input == "volume_albedo") destination = &graph.output[41];
      else if (input == "volume_emission_color") destination = &graph.output[42];
      else if (input == "volume_emission_scale") destination = &graph.output[43];
      else if (input == "emission_luminance") destination = &graph.output[44];
      else if (input == "coat_affect_color") destination = &graph.output[45];
      else if (input == "coat_affect_roughness") destination = &graph.output[46];
      else if (input == "coat_darkening") destination = &graph.output[47];
      if (destination) *destination = nodeIt->second;
    }
  }
  if (subsurfaceRadiusScaleNode >= 0) {
    if (graph.output[8] >= 0) {
      MaterialXGraphNodeCPU combined;
      combined.op = MaterialXGraphOpCPU::Multiply;
      combined.input[0] = graph.output[8];
      combined.input[1] = subsurfaceRadiusScaleNode;
      combined.name = "lusdview_subsurface_radius_scaled";
      graph.output[8] = static_cast<int>(graph.nodes.size());
      graph.nodes.push_back(std::move(combined));
    } else {
      graph.output[8] = subsurfaceRadiusScaleNode;
    }
  }
  // GPU interpreters execute a single bounded pass. Canonicalize the retained
  // graph into dependency-first order here so runtime evaluation never needs
  // the old 64x64 fixed-point fallback (a severe NVRTC/Vulkan driver-JIT cost).
  // Cyclic MaterialX graphs are malformed and keep the caller's bake fallback.
  std::vector<unsigned char> visit(graph.nodes.size(), 0);
  std::vector<int> order;
  order.reserve(graph.nodes.size());
  std::function<bool(int)> emitDependencyFirst = [&](int index) {
    if (index < 0 || static_cast<size_t>(index) >= graph.nodes.size()) return true;
    unsigned char& state = visit[static_cast<size_t>(index)];
    if (state == 2) return true;
    if (state == 1) return false;
    state = 1;
    for (int input : graph.nodes[static_cast<size_t>(index)].input)
      if (!emitDependencyFirst(input)) return false;
    if (!emitDependencyFirst(graph.nodes[static_cast<size_t>(index)].auxInput))
      return false;
    state = 2;
    order.push_back(index);
    return true;
  };
  // Only nodes reachable from a MaterialX/OpenPBR surface output are needed
  // by the raster and RT surface interpreters.  Older versions walked every
  // node in the JSON document, which made unused authoring helpers consume the
  // fixed 64-node budget and inflated every per-fragment graph evaluation.
  std::vector<int> roots;
  roots.reserve(graph.output.size());
  for (int output : graph.output) {
    if (output >= 0) roots.push_back(output);
  }
  // Flake is a multi-output MaterialX node.  Its lowering emits a shared
  // input table plus four flakecore nodes, while the packed runtime ABI keeps
  // those relationships in auxValue rather than ordinary input edges.  A
  // surface may select only flakenormal even though another authored node
  // consumes presence/rand/id, so ordinary reachability would discard part
  // of the lowered group (and change its stable ABI indices).  Keep the
  // authored/lowered sequence intact for this operator; this is bounded by
  // the same 64-node check below and preserves all multi-output consumers.
  const bool hasFlake = std::any_of(
      graph.nodes.begin(), graph.nodes.end(), [](const MaterialXGraphNodeCPU& node) {
        return node.op == MaterialXGraphOpCPU::Flake;
      });
  if (hasFlake) {
    roots.clear();
    roots.reserve(graph.nodes.size());
    for (size_t i = 0; i < graph.nodes.size(); ++i)
      roots.push_back(static_cast<int>(i));
  }
  // A standalone node graph may intentionally have no surface outputs (for
  // example, a graph inspection or interchange test).  In that case there is
  // no reachability root, so retain every authored node instead of silently
  // producing an empty runtime graph.
  if (roots.empty()) {
    roots.reserve(graph.nodes.size());
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
      roots.push_back(static_cast<int>(i));
    }
  }
  for (int root : roots) {
    if (!emitDependencyFirst(root)) {
      if (err) *err = "MaterialX graph contains a dependency cycle";
      return false;
    }
  }
  std::vector<int> oldToNew(graph.nodes.size(), -1);
  std::vector<MaterialXGraphNodeCPU> sorted;
  sorted.reserve(graph.nodes.size());
  for (int oldIndex : order) {
    oldToNew[static_cast<size_t>(oldIndex)] = static_cast<int>(sorted.size());
    sorted.push_back(std::move(graph.nodes[static_cast<size_t>(oldIndex)]));
  }
  if (sorted.size() > kRtMaterialGraphMaxNodes) {
    if (err) *err = "MaterialX graph exceeds the 64-node runtime limit";
    return false;
  }
  for (MaterialXGraphNodeCPU& node : sorted) {
    for (int& input : node.input)
      if (input >= 0) input = oldToNew[static_cast<size_t>(input)];
    if (node.auxInput >= 0)
      node.auxInput = oldToNew[static_cast<size_t>(node.auxInput)];
  }
  for (int& output : graph.output)
    if (output >= 0) output = oldToNew[static_cast<size_t>(output)];
  graph.nodes = std::move(sorted);
  graph.valid = true;
  return true;
}

}  // namespace lusdview
