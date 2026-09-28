// SPDX-License-Identifier: Apache-2.0
#include "gltf-export.hh"
#include "minijson.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>

namespace lightusd { namespace tydra { namespace next {
namespace {
using Json = minijson::Value;
void U32(std::vector<uint8_t>& bytes, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(n >> (8 * i)));
}
class Exporter {
 public:
  Exporter(const RenderScene& s, const GltfExportOptions& o) : scene(s), opt(o) {}
  const RenderScene& scene;
  const GltfExportOptions& opt;
  GltfExportResult result;
  Json document = Json::object();
  std::vector<uint8_t> binary;
  std::map<int, int> texture_ids;
  std::set<std::string> losses;
  std::map<std::pair<int, bool>, int> material_variants;
  void Loss(const std::string& path, const std::string& what) { losses.insert(path + ": " + what); }
  bool Room(size_t bytes) {
    if (binary.size() > opt.max_output_bytes || bytes > opt.max_output_bytes - binary.size()) {
      result.error = "GLB output byte limit exceeded"; return false;
    }
    return true;
  }
  int View(const uint8_t* bytes, size_t count, bool vertex = false) {
    while (binary.size() % 4) binary.push_back(0);
    if (!Room(count)) return -1;
    const int index = int(document["bufferViews"].size());
    Json view{{"buffer", 0}, {"byteOffset", uint64_t(binary.size())}, {"byteLength", uint64_t(count)}};
    if (vertex) view["target"] = 34962;
    document["bufferViews"].push_back(std::move(view));
    if (count) binary.insert(binary.end(), bytes, bytes + count);
    return index;
  }
  int Attribute(const std::vector<float>& values, unsigned components, bool bounds) {
    if (values.empty() || values.size() % components || values.size() > opt.max_output_bytes / 4) {
      result.error = "invalid/oversized vertex attribute"; return -1;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(values.size() * 4);
    for (float f : values) {
      if (!std::isfinite(f)) { result.error = "non-finite vertex attribute"; return -1; }
      uint32_t bits; std::memcpy(&bits, &f, 4); U32(bytes, bits);
    }
    const int view = View(bytes.data(), bytes.size(), true);
    if (view < 0) return -1;
    Json attr{{"bufferView", view}, {"componentType", 5126},
              {"count", uint64_t(values.size() / components)},
              {"type", components == 2 ? "VEC2" : "VEC3"}};
    if (bounds) {
      Json lo = Json::array(), hi = Json::array();
      for (unsigned c = 0; c < components; ++c) {
        float minimum = values[c], maximum = values[c];
        for (size_t i = c; i < values.size(); i += components) {
          minimum = std::min(minimum, values[i]); maximum = std::max(maximum, values[i]);
        }
        lo.push_back(minimum); hi.push_back(maximum);
      }
      attr["min"] = lo; attr["max"] = hi;
    }
    const int id = int(document["accessors"].size());
    document["accessors"].push_back(std::move(attr)); return id;
  }
  int Texture(int id, const std::string& path) {
    if (id < 0 || size_t(id) >= scene.textures.size()) return -1;
    if (texture_ids.count(id)) return texture_ids[id];
    texture_ids[id] = -1;
    const auto& tex = scene.textures[size_t(id)];
    if (tex.offset.x != 0 || tex.offset.y != 0 || tex.scale.x != 1 || tex.scale.y != 1 || tex.rotation != 0) {
      Loss(path, "transformed texture omitted"); return -1;
    }
    if (tex.image_id < 0 || size_t(tex.image_id) >= scene.images.size()) { Loss(path, "unresolved texture image"); return -1; }
    const auto& image = scene.images[size_t(tex.image_id)];
    std::vector<uint8_t> data;
    std::string error;
    if (opt.resolver) {
      const std::string resolved = opt.asset_anchor.empty()
          ? image.resolved_path
          : opt.resolver->Resolve(image.resolved_path, opt.asset_anchor).resolved_path;
      if (!opt.resolver->ReadAsset(resolved, &data, &error)) { Loss(path, "unreadable texture: " + image.resolved_path); return -1; }
    } else {
      std::ifstream in(image.resolved_path, std::ios::binary | std::ios::ate);
      const auto size = in.tellg();
      if (!in || size < 0 || uint64_t(size) > opt.max_output_bytes) { Loss(path, "unreadable/oversized texture"); return -1; }
      data.resize(size_t(size)); in.seekg(0);
      if (size && !in.read(reinterpret_cast<char*>(data.data()), size)) { Loss(path, "texture read failed"); return -1; }
    }
    const char* mime = nullptr;
    if (data.size() >= 8 && std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) == 0) mime = "image/png";
    if (data.size() >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) mime = "image/jpeg";
    if (!mime) { Loss(path, "texture format requires PNG/JPEG transcoding"); return -1; }
    const int view = View(data.data(), data.size());
    if (view < 0) return -1;
    const int image_id = int(document["images"].size());
    document["images"].push_back(Json{{"bufferView", view}, {"mimeType", mime}, {"name", image.name}});
    auto wrap = [&](WrapMode mode) {
      if (mode == WrapMode::Black) Loss(path, "black texture border approximated with clamp");
      return mode == WrapMode::Repeat ? 10497 : mode == WrapMode::Mirror ? 33648 : 33071;
    };
    const int sampler = int(document["samplers"].size());
    document["samplers"].push_back(Json{{"wrapS", wrap(tex.wrap_s)}, {"wrapT", wrap(tex.wrap_t)}});
    const int output = int(document["textures"].size());
    document["textures"].push_back(Json{{"source", image_id}, {"sampler", sampler}});
    texture_ids[id] = output; return output;
  }
  float Factor(float value, const std::string& path) {
    if (!std::isfinite(value)) { result.error = "non-finite material factor: " + path; return 0; }
    if (value < 0 || value > 1) Loss(path, "material factor clamped to glTF range [0,1]");
    return std::max(0.f, std::min(1.f, value));
  }
  int MeshMaterial(int id, bool double_sided) {
    const auto key = std::make_pair(id, double_sided);
    auto found = material_variants.find(key);
    if (found != material_variants.end()) return found->second;
    if (id >= 0 && size_t(id) < scene.materials.size() && scene.materials[size_t(id)].double_sided == double_sided) return id;
    if (id < 0 && !double_sided) return -1;
    Json m = id >= 0 && size_t(id) < scene.materials.size() ? document["materials"][size_t(id)] : Json::object();
    m["doubleSided"] = double_sided;
    const int index = int(document["materials"].size());
    document["materials"].push_back(m); material_variants[key] = index;
    return index;
  }
  void Materials() {
    for (const auto& m : scene.materials) {
      Json material{{"name", m.name}, {"doubleSided", m.double_sided}};
      if (m.alpha_mode == RenderMaterial::AlphaMode::Blend) material["alphaMode"] = "BLEND";
      if (m.alpha_mode == RenderMaterial::AlphaMode::Mask) { material["alphaMode"] = "MASK"; material["alphaCutoff"] = Factor(m.alpha_cutoff, m.prim_path); }
      Json pbr = Json::object();
      if (m.preview_surface) {
        const auto& p = *m.preview_surface;
        pbr["baseColorFactor"] = Json::array({p.diffuse_color.is_texture() ? 1.f : Factor(p.diffuse_color.value.x, m.prim_path),
            p.diffuse_color.is_texture() ? 1.f : Factor(p.diffuse_color.value.y, m.prim_path),
            p.diffuse_color.is_texture() ? 1.f : Factor(p.diffuse_color.value.z, m.prim_path), Factor(p.opacity.as_float(), m.prim_path)});
        pbr["metallicFactor"] = p.metallic.is_texture() ? 1.f : Factor(p.metallic.as_float(), m.prim_path);
        pbr["roughnessFactor"] = p.roughness.is_texture() ? 1.f : Factor(p.roughness.as_float(), m.prim_path);
        auto bind = [&](Json& dst, const char* key, const ShaderParam& param) {
          if (!param.is_texture()) return;
          if (size_t(param.texture_id) >= scene.textures.size()) { Loss(m.prim_path, "invalid texture reference"); return; }
          const auto& t = scene.textures[size_t(param.texture_id)];
          const bool normal = std::string(key) == "normalTexture";
          const bool normal_decode = normal && t.scale_value.x == 2 && t.scale_value.y == 2 && t.scale_value.z == 2 &&
              t.bias.x == -1 && t.bias.y == -1 && t.bias.z == -1;
          if (!normal_decode && (t.scale_value.x != 1 || t.scale_value.y != 1 || t.scale_value.z != 1 || t.scale_value.w != 1 ||
              t.bias.x != 0 || t.bias.y != 0 || t.bias.z != 0 || t.bias.w != 0)) {
            Loss(m.prim_path, "texture value scale/bias requires baking; texture omitted"); return;
          }
          const std::string role(key);
          const auto channel = t.output_channel;
          if ((role == "occlusionTexture" && channel != RenderTexture::Channel::R) ||
              ((role == "baseColorTexture" || role == "emissiveTexture" || normal) &&
               channel != RenderTexture::Channel::RGB && channel != RenderTexture::Channel::RGBA)) {
            Loss(m.prim_path, "texture channel swizzle requires baking; texture omitted"); return;
          }
          if ((role == "baseColorTexture" || role == "emissiveTexture") &&
              (t.source_color_is_data || (!t.source_color_space.empty() && t.source_color_space != "auto" && t.source_color_space != "sRGB" && t.source_color_space != "srgb")))
            Loss(m.prim_path, "color texture uses glTF sRGB decoding; source colorspace differs");
          const int texture = Texture(param.texture_id, m.prim_path);
          if (texture >= 0) dst[key] = Json{{"index", texture}};
        };
        bind(pbr, "baseColorTexture", p.diffuse_color);
        bind(material, "normalTexture", p.normal);
        bind(material, "occlusionTexture", p.occlusion);
        bind(material, "emissiveTexture", p.emissive_color);
        material["emissiveFactor"] = p.emissive_color.is_texture() ? Json::array({1, 1, 1}) :
            Json::array({Factor(p.emissive_color.value.x, m.prim_path), Factor(p.emissive_color.value.y, m.prim_path), Factor(p.emissive_color.value.z, m.prim_path)});
        if (p.metallic.is_texture() || p.roughness.is_texture()) {
          const bool valid = p.metallic.is_texture() && p.roughness.is_texture() &&
              size_t(p.metallic.texture_id) < scene.textures.size() && size_t(p.roughness.texture_id) < scene.textures.size();
          if (valid && scene.textures[size_t(p.metallic.texture_id)].image_id == scene.textures[size_t(p.roughness.texture_id)].image_id &&
              scene.textures[size_t(p.metallic.texture_id)].output_channel == RenderTexture::Channel::B &&
              scene.textures[size_t(p.roughness.texture_id)].output_channel == RenderTexture::Channel::G)
            bind(pbr, "metallicRoughnessTexture", p.roughness);
          else Loss(m.prim_path, "separate metallic/roughness textures require channel packing");
        }
        if (p.opacity.is_texture()) Loss(m.prim_path, "separate opacity texture omitted");
        if (p.use_specular_workflow || p.clearcoat.is_texture() || p.clearcoat.as_float() != 0)
          Loss(m.prim_path, "specular workflow/clearcoat requires glTF material extensions");
      } else { Loss(m.prim_path, "non-PreviewSurface material uses glTF defaults"); }
      if (m.default_fallback || !m.diagnostics.empty()) Loss(m.prim_path, "material conversion contains approximations");
      if (m.has_displacement || m.has_volume) Loss(m.prim_path, "displacement/volume terminal omitted");
      material["pbrMetallicRoughness"] = std::move(pbr);
      document["materials"].push_back(std::move(material));
    }
  }
  bool Meshes() {
    for (const auto& mesh : scene.meshes) {
      Json primitives = Json::array();
      if (!mesh.is_triangulated || mesh.triangulated_indices.size() % 3) { result.error = "mesh must be triangulated: " + mesh.prim_path; return false; }
      if (!mesh.triangulated_face_vertex_indices.empty() &&
          mesh.triangulated_face_vertex_indices.size() != mesh.triangulated_indices.size()) {
        result.error = "invalid triangulated corner mapping: " + mesh.prim_path; return false;
      }
      if (mesh.skin || !mesh.blend_shapes.empty()) Loss(mesh.prim_path, "skinning/morph targets omitted; static geometry exported");
      if (!mesh.colors.empty()) Loss(mesh.prim_path, "displayColor omitted");
      const size_t triangle_count = mesh.triangulated_indices.size() / 3;
      if (triangle_count > opt.max_output_bytes / (3 * 8 * sizeof(float))) { result.error = "mesh exceeds export budget"; return false; }
      std::map<int, std::vector<size_t>> groups;
      for (size_t t = 0; t < triangle_count; ++t) {
        int material = mesh.material_id;
        for (const auto& sub : mesh.material_subsets)
          if (t >= sub.face_start && t - sub.face_start < sub.face_count) material = sub.material_id;
        groups[material].push_back(t);
      }
      for (const auto& group : groups) {
        std::vector<float> positions, normals, uv;
        auto attr = [&](const FloatChunked& data, Interpolation interpolation, unsigned width,
                        size_t vertex, size_t corner, size_t face, std::vector<float>& out) {
          const size_t index = interpolation == Interpolation::Constant ? 0 :
              interpolation == Interpolation::Uniform ? face : interpolation == Interpolation::FaceVarying ? corner : vertex;
          if (index >= data.size() / width) return false;
          for (unsigned c = 0; c < width; ++c) out.push_back(data[index * width + c]);
          return true;
        };
        for (size_t t : group.second) {
          size_t face = t;
          if (!mesh.face_triangle_offsets.empty()) {
            auto it = std::upper_bound(mesh.face_triangle_offsets.begin(), mesh.face_triangle_offsets.end(), uint32_t(t));
            face = it == mesh.face_triangle_offsets.begin() ? 0 : size_t(it - mesh.face_triangle_offsets.begin() - 1);
          }
          for (size_t j = 0; j < 3; ++j) {
            const size_t k = t * 3 + j, vertex = mesh.triangulated_indices[k];
            const size_t corner = mesh.triangulated_face_vertex_indices.empty() ? k : mesh.triangulated_face_vertex_indices[k];
            if (!attr(mesh.points, Interpolation::Vertex, 3, vertex, corner, face, positions) ||
                (!mesh.normals.empty() && !attr(mesh.normals, mesh.normals_interp, 3, vertex, corner, face, normals)) ||
                (!mesh.texcoords_0.empty() && !attr(mesh.texcoords_0, mesh.texcoords_0_interp, 2, vertex, corner, face, uv))) {
              result.error = "out-of-range vertex attribute: " + mesh.prim_path; return false;
            }
          }
        }
        for (size_t i = 1; i < uv.size(); i += 2) uv[i] = 1.f - uv[i];
        for (size_t i = 0; i < normals.size(); i += 3) {
          const double length = std::sqrt(double(normals[i]) * normals[i] + double(normals[i+1]) * normals[i+1] + double(normals[i+2]) * normals[i+2]);
          if (length <= 0 || !std::isfinite(length)) { result.error = "invalid normal: " + mesh.prim_path; return false; }
          for (size_t c = 0; c < 3; ++c) normals[i+c] = float(normals[i+c] / length);
        }
        Json attrs{{"POSITION", Attribute(positions, 3, true)}};
        if (!normals.empty()) attrs["NORMAL"] = Attribute(normals, 3, false);
        if (!uv.empty()) attrs["TEXCOORD_0"] = Attribute(uv, 2, false);
        if (!result.error.empty()) return false;
        Json primitive{{"attributes", attrs}, {"mode", 4}};
        const int material_id = MeshMaterial(group.first, mesh.double_sided);
        if (material_id >= 0) primitive["material"] = material_id;
        if (group.first >= 0 && size_t(group.first) < scene.materials.size()) {
          const auto& material = scene.materials[size_t(group.first)];
          if (material.preview_surface) {
            const auto& p = *material.preview_surface;
            for (const auto* param : {&p.diffuse_color, &p.normal, &p.roughness, &p.metallic, &p.emissive_color, &p.occlusion}) {
              if (!param->is_texture() || size_t(param->texture_id) >= scene.textures.size()) continue;
              const auto& texture = scene.textures[size_t(param->texture_id)];
              const std::string uv_name = texture.uv_primvar.empty() ? "st" : texture.uv_primvar;
              if (mesh.texcoords_0.empty() || uv_name != mesh.texcoords_0_name)
                Loss(mesh.prim_path, "texture UV set differs from exported TEXCOORD_0: " + uv_name);
            }
          }
        }
        primitives.push_back(std::move(primitive));
      }
      if (primitives.empty()) {
        result.error = "empty mesh cannot be exported: " + mesh.prim_path; return false;
      }
      document["meshes"].push_back(Json{{"name", mesh.name}, {"primitives", primitives}});
    }
    return true;
  }
  bool Run() {
    if (!opt.max_output_bytes || opt.max_output_bytes > UINT32_MAX) { result.error = "GLB budget must be in 1..UINT32_MAX"; return false; }
    document["asset"] = Json{{"version", "2.0"}, {"generator", "LightUSD Tydra-next"}};
    for (const char* key : {"bufferViews", "accessors", "meshes", "nodes", "materials", "images", "textures", "samplers"}) document[key] = Json::array();
    Materials();
    if (!result.error.empty() || !Meshes()) return false;
    if (!scene.animations.empty()) Loss("/", "animation omitted; selected time exported");
    if (!scene.point_instancers.empty()) Loss("/", "PointInstancer draws omitted");
    if (!scene.points.empty() || !scene.curves.empty()) Loss("/", "points/curves omitted");
    if (!scene.lights.empty() || !scene.cameras.empty()) Loss("/", "lights/cameras omitted");
    for (const auto& u : scene.unsupported_renderables) Loss(u.prim_path, u.reason);
    if (scene.nodes.size() > opt.max_output_bytes / 64) { result.error = "node count exceeds output budget"; return false; }
    std::vector<unsigned> parents(scene.nodes.size(), 0);
    for (const auto& node : scene.nodes) for (int child : node.children) {
      if (child < 0 || size_t(child) >= parents.size() || ++parents[size_t(child)] > 1) {
        result.error = "invalid node hierarchy"; return false;
      }
    }
    std::vector<size_t> queue;
    for (size_t i = 0; i < parents.size(); ++i) if (!parents[i]) queue.push_back(i);
    for (size_t i = 0; i < queue.size(); ++i) for (int child : scene.nodes[queue[i]].children) queue.push_back(size_t(child));
    if (queue.size() != scene.nodes.size()) { result.error = "cyclic node hierarchy"; return false; }
    for (int root : scene.root_nodes) if (root < 0 || size_t(root) >= parents.size() || parents[size_t(root)] != 0) {
      result.error = "invalid root node"; return false;
    }
    for (const auto& node : scene.nodes) {
      Json matrix = Json::array(), children = Json::array();
      for (float f : node.local_transform.m) {
        if (!std::isfinite(f)) { result.error = "non-finite transform"; return false; }
        matrix.push_back(f);
      }
      for (int child : node.children) {
        if (child < 0 || size_t(child) >= scene.nodes.size()) { result.error = "invalid node hierarchy"; return false; }
        children.push_back(child);
      }
      Json n{{"name", node.name}, {"matrix", matrix}, {"extras", Json{{"usdPrimPath", node.prim_path}}}};
      if (!children.empty()) n["children"] = children;
      if (node.type == NodeType::Mesh && node.visible && node.data_id >= 0 && size_t(node.data_id) < scene.meshes.size()) n["mesh"] = node.data_id;
      document["nodes"].push_back(std::move(n));
    }
    Json roots = Json::array();
    for (int root : scene.root_nodes) {
      if (root < 0 || size_t(root) >= scene.nodes.size()) { result.error = "invalid root node"; return false; }
      roots.push_back(root);
    }
    const float unit = scene.meters_per_unit;
    if (!std::isfinite(unit) || unit <= 0) { result.error = "invalid metersPerUnit"; return false; }
    if (!roots.empty() && (unit != 1 || scene.up_axis == RenderScene::UpAxis::Z)) {
      Json root{{"name", "USD coordinate conversion"}, {"children", roots}, {"scale", Json::array({unit, unit, unit})}};
      if (scene.up_axis == RenderScene::UpAxis::Z) root["rotation"] = Json::array({-0.7071067811865476, 0, 0, 0.7071067811865476});
      roots = Json::array({uint64_t(document["nodes"].size())});
      document["nodes"].push_back(std::move(root));
    }
    document["scene"] = 0;
    document["scenes"] = Json::array({roots.empty() ? Json::object() : Json{{"nodes", roots}}});
    result.losses.assign(losses.begin(), losses.end());
    if (opt.fail_on_loss && !result.losses.empty()) { result.error = "export refused because conversion is lossy"; return false; }
    Json loss = Json::array(); for (const auto& l : result.losses) loss.push_back(l);
    document["extras"] = Json{{"lightusdConversionLosses", loss}};
    if (!binary.empty()) document["buffers"] = Json::array({Json{{"byteLength", uint64_t(binary.size())}}});
    // Empty optional arrays violate glTF's minItems constraints.
    Json compact = Json::object();
    for (const auto& member : *document.object_items())
      if (!member.value().is_array() || !member.value().empty()) compact[member.key] = member.value();
    std::string text = compact.dump();
    while (text.size() % 4) text += ' ';
    while (binary.size() % 4) binary.push_back(0);
    const uint64_t total = 20ull + text.size() + (binary.empty() ? 0 : 8ull + binary.size());
    if (total > opt.max_output_bytes || total > UINT32_MAX) { result.error = "GLB output byte limit exceeded"; return false; }
    U32(result.glb, 0x46546c67); U32(result.glb, 2); U32(result.glb, uint32_t(total));
    U32(result.glb, uint32_t(text.size())); U32(result.glb, 0x4e4f534a);
    result.glb.insert(result.glb.end(), text.begin(), text.end());
    if (!binary.empty()) { U32(result.glb, uint32_t(binary.size())); U32(result.glb, 0x004e4942); result.glb.insert(result.glb.end(), binary.begin(), binary.end()); }
    return true;
  }
};
}  // namespace
GltfExportResult ExportGLB(const RenderScene& scene, const GltfExportOptions& options) {
  Exporter exporter(scene, options);
  exporter.result.success = exporter.Run();
  exporter.result.losses.assign(exporter.losses.begin(), exporter.losses.end());
  return std::move(exporter.result);
}
} } }
