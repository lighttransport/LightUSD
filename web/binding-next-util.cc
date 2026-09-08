// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-common.hh"
namespace lightusd {
namespace web_next {

EM_JS(void, reportNextCrateProgress, (const char* phase, double current, double total), {
  if (typeof Module.onNextCrateProgress === 'function') {
    const cur = Number(current);
    const tot = Number(total);
    Module.onNextCrateProgress({
      phase: UTF8ToString(Number(phase)),
      current: cur,
      total: tot,
      percentage: tot > 0 ? (cur / tot) * 100 : 0
    });
  }
});
std::string CopyUint8ArrayToString(const emscripten::val& bytes,
                                   std::string* error) {
  if (bytes.isNull() || bytes.isUndefined()) {
    if (error) *error = "Input is null or undefined";
    return {};
  }
  const size_t size = bytes["byteLength"].as<size_t>();
  constexpr size_t kMaxLayerBytes = size_t(1) << 30;  // 1 GiB
  if (size > kMaxLayerBytes) {
    if (error) *error = "Input exceeds 1 GiB limit";
    return {};
  }
  std::string out(size, '\0');
  if (size > 0) {
    emscripten::val view = emscripten::val::global("Uint8Array").new_(
        bytes["buffer"], bytes["byteOffset"],
        emscripten::val(static_cast<double>(size)));
    emscripten::val heap_view = emscripten::val(
        emscripten::typed_memory_view(size, reinterpret_cast<uint8_t*>(&out[0])));
    heap_view.call<void>("set", view);
  }
  return out;
}

bool IsUSDCBytes(const std::string& bytes) {
  return bytes.size() >= 8 &&
         std::memcmp(bytes.data(), "PXR-USDC", 8) == 0;
}

emscripten::val Uint8ArrayFromBytes(const uint8_t* data, size_t size) {
  emscripten::val out = emscripten::val::global("Uint8Array").new_(
      emscripten::val(static_cast<double>(size)));
  if (size > 0) {
    emscripten::val view = emscripten::val(
        emscripten::typed_memory_view(size, data));
    out.call<void>("set", view);
  }
  return out;
}

emscripten::val Uint8ArrayFromString(const std::string& s) {
  return Uint8ArrayFromBytes(reinterpret_cast<const uint8_t*>(s.data()),
                             s.size());
}

emscripten::val Uint8ArrayFromVector(const std::vector<uint8_t>& v) {
  return Uint8ArrayFromBytes(v.data(), v.size());
}

std::array<double, 16> IdentityMatrix() {
  return {1.0, 0.0, 0.0, 0.0,
          0.0, 1.0, 0.0, 0.0,
          0.0, 0.0, 1.0, 0.0,
          0.0, 0.0, 0.0, 1.0};
}

std::array<double, 16> MatrixToArray(const tr::Matrix4& m) {
  std::array<double, 16> out{};
  for (size_t i = 0; i < 16; ++i) out[i] = static_cast<double>(m.m[i]);
  return out;
}

const char* NodeTypeName(tr::NodeType type) {
  switch (type) {
    case tr::NodeType::Xform:
      return "xform";
    case tr::NodeType::Mesh:
      return "mesh";
    case tr::NodeType::Points:
      return "points";
    case tr::NodeType::PointInstancer:
      return "pointInstancer";
    case tr::NodeType::Camera:
      return "camera";
    case tr::NodeType::PointLight:
      return "pointLight";
    case tr::NodeType::DirectionalLight:
      return "directionalLight";
    case tr::NodeType::SpotLight:
      return "spotLight";
    case tr::NodeType::RectLight:
      return "rectLight";
    case tr::NodeType::DiskLight:
      return "diskLight";
    case tr::NodeType::DomeLight:
      return "domeLight";
    case tr::NodeType::SphereLight:
      return "sphereLight";
    case tr::NodeType::Skeleton:
      return "skeleton";
    case tr::NodeType::Curves:
      return "curves";
    default:
      return "unknown";
  }
}

const char* InterpolationName(tr::Interpolation interpolation) {
  switch (interpolation) {
    case tr::Interpolation::Constant:
      return "constant";
    case tr::Interpolation::Uniform:
      return "uniform";
    case tr::Interpolation::Vertex:
      return "vertex";
    case tr::Interpolation::Varying:
      return "varying";
    case tr::Interpolation::FaceVarying:
      return "faceVarying";
    default:
      return "constant";
  }
}

const char* CurveTypeName(tr::CurveType type) {
  return type == tr::CurveType::Linear ? "linear" : "cubic";
}

const char* CurveBasisName(tr::CurveBasis basis) {
  switch (basis) {
    case tr::CurveBasis::Bezier:
      return "bezier";
    case tr::CurveBasis::BSpline:
      return "bspline";
    case tr::CurveBasis::CatmullRom:
      return "catmullRom";
    default:
      return "bezier";
  }
}

const char* CurveWrapName(tr::CurveWrap wrap) {
  switch (wrap) {
    case tr::CurveWrap::Nonperiodic:
      return "nonperiodic";
    case tr::CurveWrap::Periodic:
      return "periodic";
    case tr::CurveWrap::Pinned:
      return "pinned";
    default:
      return "nonperiodic";
  }
}

const char* LightTypeName(tr::LightType type) {
  switch (type) {
    case tr::LightType::Point:
      return "point";
    case tr::LightType::Directional:
      return "directional";
    case tr::LightType::Spot:
      return "spot";
    case tr::LightType::Rect:
      return "rect";
    case tr::LightType::Disk:
      return "disk";
    case tr::LightType::Dome:
      return "dome";
    case tr::LightType::Sphere:
      return "sphere";
    case tr::LightType::Cylinder:
      return "cylinder";
    case tr::LightType::Geometry:
      return "geometry";
    default:
      return "unknown";
  }
}

const char* AnimationPathName(tr::AnimationChannel::TargetPath path) {
  switch (path) {
    case tr::AnimationChannel::TargetPath::Translation:
      return "Translation";
    case tr::AnimationChannel::TargetPath::Rotation:
      return "Rotation";
    case tr::AnimationChannel::TargetPath::Scale:
      return "Scale";
    case tr::AnimationChannel::TargetPath::Weights:
      return "Weights";
    case tr::AnimationChannel::TargetPath::CustomProperty:
      return "CustomProperty";
    default:
      return "Unknown";
  }
}

const char* AnimationInterpolationName(
    tr::AnimationChannel::Interpolation interp) {
  switch (interp) {
    case tr::AnimationChannel::Interpolation::Step:
      return "STEP";
    case tr::AnimationChannel::Interpolation::Linear:
      return "LINEAR";
    case tr::AnimationChannel::Interpolation::CubicSpline:
      return "CUBICSPLINE";
    default:
      return "LINEAR";
  }
}

const char* CameraTypeName(tr::CameraType type) {
  switch (type) {
    case tr::CameraType::Perspective:
      return "perspective";
    case tr::CameraType::Orthographic:
      return "orthographic";
    default:
      return "unknown";
  }
}

emscripten::val MatrixValue(const std::array<double, 16>& m) {
  emscripten::val a = emscripten::val::array();
  for (double v : m) a.call<void>("push", v);
  return a;
}

emscripten::val Matrix3Value(const float m[9]) {
  emscripten::val a = emscripten::val::array();
  for (size_t i = 0; i < 9; ++i) a.call<void>("push", m[i]);
  return a;
}

emscripten::val Float3Value(const tr::Float3& c) {
  emscripten::val a = emscripten::val::array();
  a.call<void>("push", c.x);
  a.call<void>("push", c.y);
  a.call<void>("push", c.z);
  return a;
}

emscripten::val Float4Value(const tr::Float4& c) {
  emscripten::val a = emscripten::val::array();
  a.call<void>("push", c.x);
  a.call<void>("push", c.y);
  a.call<void>("push", c.z);
  a.call<void>("push", c.w);
  return a;
}

uint32_t ChunkedU32At(const tr::UInt32Chunked& values, size_t i,
                      uint32_t fallback) {
  return i < values.size() ? values[i] : fallback;
}

const tr::RenderTexture* TextureAt(const tr::RenderScene& scene, int32_t id) {
  if (id < 0 || static_cast<size_t>(id) >= scene.textures.size()) return nullptr;
  return &scene.textures[static_cast<size_t>(id)];
}

std::string TexturePath(const tr::RenderScene& scene, const tr::ShaderParam& p) {
  const tr::RenderTexture* tex = TextureAt(scene, p.texture_id);
  return tex ? tex->asset_path : std::string();
}

std::string MaterialKey(const tr::RenderScene& scene, int32_t material_id) {
  const tr::RenderMaterial* mat = scene.get_material(material_id);
  if (!mat) return "__default";
  std::ostringstream ss;
  ss << material_id << "|" << mat->prim_path;
  return ss.str();
}

std::string JsonEscape(const std::string& s) {
  std::ostringstream out;
  for (char c : s) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c))
              << std::dec << std::setfill(' ');
        } else {
          out << c;
        }
        break;
    }
  }
  return out.str();
}

const char* RenderMaterialShaderTypeName(tr::RenderMaterial::ShaderType type) {
  switch (type) {
    case tr::RenderMaterial::ShaderType::PreviewSurface:
      return "PreviewSurface";
    case tr::RenderMaterial::ShaderType::OpenPBR:
      return "OpenPBR";
    case tr::RenderMaterial::ShaderType::None:
    default:
      return "None";
  }
}

std::string ShaderParamJson(const tr::RenderScene& scene,
                            const tr::ShaderParam& p) {
  std::ostringstream ss;
  ss << "{\"value\":[" << p.value.x << "," << p.value.y << ","
     << p.value.z << "," << p.value.w << "]";
  if (p.texture_id >= 0) {
    ss << ",\"texture\":\"" << JsonEscape(TexturePath(scene, p)) << "\"";
    ss << ",\"textureId\":" << p.texture_id;
    if (static_cast<size_t>(p.texture_id) < scene.textures.size()) {
      ss << ",\"colorspace\":\""
         << JsonEscape(scene.textures[static_cast<size_t>(p.texture_id)]
                           .source_color_space)
         << "\"";
    }
  }
  ss << "}";
  return ss.str();
}

std::string RenderMaterialJson(const tr::RenderScene& scene,
                               const tr::RenderMaterial& mat) {
  std::ostringstream ss;
  ss << "{\"name\":\"" << JsonEscape(mat.name) << "\",";
  ss << "\"primPath\":\"" << JsonEscape(mat.prim_path) << "\",";
  ss << "\"shaderType\":\"" << RenderMaterialShaderTypeName(mat.shader_type)
     << "\",";
  ss << "\"workingColorSpace\":\""
     << JsonEscape(scene.working_color_space) << "\",";
  ss << "\"workingToDisplayLinear\":[";
  for (size_t i = 0; i < 9; ++i) {
    if (i) ss << ",";
    ss << scene.working_to_display_linear[i];
  }
  ss << "],";
  ss << "\"materialXConfig\":{";
  ss << "\"authored\":" << (mat.mtlx_config.authored ? "true" : "false");
  ss << ",\"version\":\"" << JsonEscape(mat.mtlx_config.version) << "\"";
  ss << ",\"namespace\":\"" << JsonEscape(mat.mtlx_config.name_space) << "\"";
  ss << ",\"colorspace\":\"" << JsonEscape(mat.mtlx_config.colorspace) << "\"";
  ss << ",\"sourceUri\":\"" << JsonEscape(mat.mtlx_config.source_uri) << "\"";
  ss << "}";
  if (mat.preview_surface) {
    const tr::PreviewSurfaceShader& ps = *mat.preview_surface;
    ss << ",\"previewSurface\":{";
    ss << "\"diffuseColor\":" << ShaderParamJson(scene, ps.diffuse_color);
    ss << ",\"emissiveColor\":" << ShaderParamJson(scene, ps.emissive_color);
    ss << ",\"metallic\":" << ShaderParamJson(scene, ps.metallic);
    ss << ",\"roughness\":" << ShaderParamJson(scene, ps.roughness);
    ss << ",\"opacity\":" << ShaderParamJson(scene, ps.opacity);
    ss << ",\"normal\":" << ShaderParamJson(scene, ps.normal);
    ss << "}";
  }
  if (mat.openpbr) {
    const tr::OpenPBRSurfaceShader& op = *mat.openpbr;
    ss << ",\"openPBR\":{";
    ss << "\"baseColor\":" << ShaderParamJson(scene, op.base_color);
    ss << ",\"baseWeight\":" << ShaderParamJson(scene, op.base_weight);
    ss << ",\"baseRoughness\":" << ShaderParamJson(scene, op.base_roughness);
    ss << ",\"baseMetalness\":" << ShaderParamJson(scene, op.base_metalness);
    ss << ",\"specularWeight\":" << ShaderParamJson(scene, op.specular_weight);
    ss << ",\"specularColor\":" << ShaderParamJson(scene, op.specular_color);
    ss << ",\"specularRoughness\":"
       << ShaderParamJson(scene, op.specular_roughness);
    ss << ",\"specularIor\":" << ShaderParamJson(scene, op.specular_ior);
    ss << ",\"specularAnisotropy\":"
       << ShaderParamJson(scene, op.specular_anisotropy);
    ss << ",\"specularRotation\":"
       << ShaderParamJson(scene, op.specular_rotation);
    ss << ",\"transmissionWeight\":"
       << ShaderParamJson(scene, op.transmission_weight);
    ss << ",\"transmissionColor\":"
       << ShaderParamJson(scene, op.transmission_color);
    ss << ",\"transmissionDepth\":"
       << ShaderParamJson(scene, op.transmission_depth);
    ss << ",\"subsurfaceWeight\":"
       << ShaderParamJson(scene, op.subsurface_weight);
    ss << ",\"subsurfaceColor\":"
       << ShaderParamJson(scene, op.subsurface_color);
    ss << ",\"coatWeight\":" << ShaderParamJson(scene, op.coat_weight);
    ss << ",\"coatColor\":" << ShaderParamJson(scene, op.coat_color);
    ss << ",\"coatRoughness\":"
       << ShaderParamJson(scene, op.coat_roughness);
    ss << ",\"coatIor\":" << ShaderParamJson(scene, op.coat_ior);
    ss << ",\"sheenWeight\":" << ShaderParamJson(scene, op.sheen_weight);
    ss << ",\"sheenColor\":" << ShaderParamJson(scene, op.sheen_color);
    ss << ",\"sheenRoughness\":"
       << ShaderParamJson(scene, op.sheen_roughness);
    ss << ",\"thinFilmWeight\":"
       << ShaderParamJson(scene, op.thin_film_weight);
    ss << ",\"thinFilmThickness\":"
       << ShaderParamJson(scene, op.thin_film_thickness);
    ss << ",\"thinFilmIor\":" << ShaderParamJson(scene, op.thin_film_ior);
    ss << ",\"emissionColor\":" << ShaderParamJson(scene, op.emission_color);
    ss << ",\"emissionLuminance\":"
       << ShaderParamJson(scene, op.emission_luminance);
    ss << ",\"opacity\":" << ShaderParamJson(scene, op.opacity);
    ss << ",\"normal\":" << ShaderParamJson(scene, op.normal);
    ss << ",\"normalMapScale\":" << op.normal_map_scale;
    ss << ",\"tangentRotation\":" << op.tangent_rotation;
    ss << ",\"nodegraphJson\":\"" << JsonEscape(op.nodegraph_json) << "\"";
    ss << "}";
  }
  ss << "}";
  return ss.str();
}

size_t AnimationComponentCount(const tr::AnimationChannel& channel) {
  switch (channel.target_path) {
    case tr::AnimationChannel::TargetPath::Rotation:
      return 4;
    case tr::AnimationChannel::TargetPath::Weights:
      return 1;
    case tr::AnimationChannel::TargetPath::CustomProperty:
      return 4;
    case tr::AnimationChannel::TargetPath::Translation:
    case tr::AnimationChannel::TargetPath::Scale:
      return 3;
    default:
      return 4;
  }
}

void AppendAnimationKeyframeValues(const tr::AnimationChannel& channel,
                                        const tr::Keyframe& keyframe,
                                        std::vector<float>* values) {
  if (!values) return;

  const size_t component_count = AnimationComponentCount(channel);
  const tr::Float4& v = keyframe.value;

  switch (component_count) {
    case 1:
      values->push_back(v.x);
      break;
    case 3:
      values->push_back(v.x);
      values->push_back(v.y);
      values->push_back(v.z);
      break;
    default:
      values->push_back(v.x);
      values->push_back(v.y);
      values->push_back(v.z);
      values->push_back(v.w);
      break;
  }
}

int AnimationTargetNodeCount(const tr::AnimationClip& clip) {
  std::set<int32_t> node_ids;
  for (const tr::AnimationChannel& channel : clip.channels) {
    if (channel.target_node >= 0) {
      node_ids.insert(channel.target_node);
    }
  }
  return static_cast<int>(node_ids.size());
}

bool AnimationHasSkeletalChannels(const tr::AnimationClip& clip) {
  for (const tr::AnimationChannel& channel : clip.channels) {
    if (channel.is_skeletal) return true;
  }
  return false;
}

std::unique_ptr<tn::Layer> ParseNextLayerBytes(
    const uint8_t* data, size_t size, const std::string& key,
    const tn::CrateReadOptions& read_opts, std::string* error) {
  if (size >= 8 && std::memcmp(data, "PXR-USDC", 8) == 0) {
    tn::CrateReader reader(read_opts);
    tn::CrateReadResult rr = reader.Read(data, size);
    if (!rr.success) {
      if (error) {
        *error = rr.errors.empty() ? ("crate read failed: " + key)
                                   : rr.errors[0].message;
      }
      return nullptr;
    }
    std::unique_ptr<tn::Layer> layer = rr.stage.ReleaseRootLayer();
    if (layer) layer->build_path_index();  // compositor looks prims up by path
    return layer;
  }

  tn::pcp::LayerLoadOptions lopts;
  lopts.max_memory = read_opts.max_memory;
  std::string warn;
  std::string parse_err;
  std::shared_ptr<tn::Layer> loaded =
      tn::pcp::LoadLayerFromMemory(key, data, size, &warn, &parse_err, lopts);
  if (!loaded) {
    if (error) {
      *error = parse_err.empty() ? ("failed to parse layer: " + key)
                                 : parse_err;
    }
    return nullptr;
  }
  std::unique_ptr<tn::Layer> layer(new tn::Layer(std::move(*loaded)));
  layer->build_path_index();
  return layer;
}
}  // namespace web_next
}  // namespace lightusd
