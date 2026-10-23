// SPDX-License-Identifier: Apache-2.0
// Bounded COLLADA (.dae) triangle-mesh reader for URDF/MJCF mesh assets.
//
// Supports <triangles>, <polylist> and <polygons> primitives with POSITION,
// NORMAL and TEXCOORD (first set) inputs, instanced through the visual scene
// node hierarchy (<matrix>, <translate>, <rotate>, <scale>, <instance_node>),
// <bind_material> symbols and profile_COMMON diffuse color/texture effects.
// <asset><unit meter> scales positions. Like urdf-usd-converter (pycollada),
// <up_axis> is reported but not applied. Output matches the OBJ Mesh layout
// (per-corner vertices, per-face material ids, OBJ-style material records).
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "external/pugixml.hpp"
#include "import.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace {

using Matrix = std::array<double, 16>;  // column-vector, row-major storage

Matrix Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = sum;
    }
  return r;
}

// Whitespace-separated numbers; returns false on malformed or non-finite.
template <class T>
bool Numbers(const char* text, std::vector<T>* out, size_t limit) {
  out->clear();
  const char* p = text ? text : "";
  const char* end = p + std::char_traits<char>::length(p);
  while (true) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    if (p >= end) return true;
    if (out->size() >= limit) return false;
    T value{};
    const auto parsed = std::from_chars(p, end, value);
    if (parsed.ec != std::errc()) return false;
    if constexpr (std::is_floating_point<T>::value) {
      if (!std::isfinite(value)) return false;
    }
    out->push_back(value);
    p = parsed.ptr;
  }
}

std::string Id(const char* url) {
  return url && *url == '#' ? std::string(url + 1) : std::string(url ? url : "");
}

struct Source {
  std::vector<float> data;
  size_t stride = 0;
};

struct Reader2 {
  Reader& reader;
  const std::string& source;
  std::map<std::string, pugi::xml_node> ids;
  std::map<std::string, Source> sources;
  std::map<std::string, int32_t> material_ids;  // material id -> OBJ index
  OBJ* obj;
  double unit = 1.0;
  std::string* err;

  bool Fail(const std::string& message) {
    if (err) *err = message + ": " + source;
    return false;
  }

  bool LoadSource(const std::string& id, const Source** out) {
    auto found = sources.find(id);
    if (found != sources.end()) {
      *out = &found->second;
      return true;
    }
    auto node = ids.find(id);
    if (node == ids.end()) return Fail("COLLADA source `" + id + "` not found");
    pugi::xml_node src = node->second;
    // <vertices> indirection: its POSITION input names the real source.
    if (std::string(src.name()) == "vertices") {
      for (auto input : src.children("input"))
        if (std::string(input.attribute("semantic").as_string()) == "POSITION")
          return LoadSource(Id(input.attribute("source").as_string()), out);
      return Fail("COLLADA <vertices> has no POSITION input");
    }
    auto array = src.child("float_array");
    auto accessor = src.child("technique_common").child("accessor");
    Source s;
    s.stride = accessor ? accessor.attribute("stride").as_uint(1) : 1;
    const size_t limit = reader.options.max_memory / sizeof(float) + 1;
    if (!array || s.stride == 0 || !Numbers(array.child_value(), &s.data, limit))
      return Fail("Invalid COLLADA float_array in `" + id + "`");
    if (!reader.ChargeArray(s.data.size(), sizeof(float), err)) return false;
    *out = &sources.emplace(id, std::move(s)).first->second;
    return true;
  }

  // profile_COMMON diffuse color / texture -> OBJ-style material record.
  int32_t Material(const std::string& material_id) {
    auto known = material_ids.find(material_id);
    if (known != material_ids.end()) return known->second;
    auto node = ids.find(material_id);
    if (node == ids.end()) return -1;
    std::string name = node->second.attribute("name").as_string();
    if (name.empty()) name = material_id;
    Json mat{{"name", name}};
    auto effect = ids.find(Id(node->second.child("instance_effect").attribute("url").as_string()));
    if (effect != ids.end()) {
      pugi::xml_node technique = effect->second.child("profile_COMMON").child("technique");
      pugi::xml_node shading;
      for (const char* kind : {"phong", "blinn", "lambert", "constant"})
        if ((shading = technique.child(kind))) break;
      std::vector<double> rgba;
      if (Numbers(shading.child("diffuse").child("color").child_value(), &rgba, 4) &&
          rgba.size() >= 3) {
        if (rgba.size() < 4) rgba.push_back(1.0);
        mat["rgba"] = Array(rgba);
      }
      if (auto tex = shading.child("diffuse").child("texture")) {
        // texture -> sampler2D newparam -> surface newparam -> image.
        std::string ref = tex.attribute("texture").as_string();
        auto profile = effect->second.child("profile_COMMON");
        for (int hop = 0; hop < 3; ++hop) {
          bool advanced = false;
          for (auto param : profile.children("newparam")) {
            if (std::string(param.attribute("sid").as_string()) != ref) continue;
            if (auto sampler = param.child("sampler2D")) {
              ref = sampler.child_value("source");
              advanced = true;
            } else if (auto surface = param.child("surface")) {
              ref = surface.child_value("init_from");
              advanced = true;
            }
            break;
          }
          if (!advanced) break;
        }
        auto image = ids.find(ref);
        std::string file;
        if (image != ids.end()) {
          file = image->second.child("init_from").child_value("ref");
          if (file.empty()) file = image->second.child_value("init_from");
        }
        if (!file.empty()) {
          AssetBytes bytes;
          std::string resolved, why;
          if (reader.ReadTexture(file, source, &bytes, &resolved, &why)) {
            mat["maps"] = Json::array({Json{{"input", "diffuseColor"}, {"file", resolved},
                                            {"color", true}}});
          } else {
            reader.Warn("Texture: " + why, "import.texture_unresolved", source);
          }
        }
      }
    }
    const int32_t index = static_cast<int32_t>(obj->materials.size());
    obj->materials.push_back(std::move(mat));
    material_ids.emplace(material_id, index);
    return index;
  }

  bool Geometry(pugi::xml_node geometry, const Matrix& m,
                const std::map<std::string, std::string>& bindings, Mesh* mesh) {
    pugi::xml_node body = geometry.child("mesh");
    if (!body) {
      reader.Warn("COLLADA geometry `" + std::string(geometry.attribute("id").as_string()) +
                      "` is not a mesh; skipped", "dae.geometry_unsupported", source);
      return true;
    }
    for (auto prim : body.children()) {
      const std::string kind = prim.name();
      if (kind != "triangles" && kind != "polylist" && kind != "polygons") {
        if (kind == "tristrips" || kind == "trifans" || kind == "lines" || kind == "linestrips")
          reader.Warn("COLLADA <" + kind + "> primitives are not converted",
                      "dae.primitive_unsupported", source);
        continue;
      }
      const Source *pos = nullptr, *nrm = nullptr, *uv = nullptr;
      size_t pos_off = 0, nrm_off = 0, uv_off = 0, stride = 1;
      for (auto input : prim.children("input")) {
        const std::string semantic = input.attribute("semantic").as_string();
        const size_t offset = input.attribute("offset").as_uint(0);
        stride = std::max(stride, offset + 1);
        const std::string id = Id(input.attribute("source").as_string());
        if (semantic == "VERTEX") {
          if (!LoadSource(id, &pos)) return false;
          pos_off = offset;
        } else if (semantic == "NORMAL") {
          if (!LoadSource(id, &nrm)) return false;
          nrm_off = offset;
        } else if (semantic == "TEXCOORD" && !uv) {
          if (!LoadSource(id, &uv)) return false;
          uv_off = offset;
        }
      }
      if (!pos || pos->stride < 3) return Fail("COLLADA primitive has no VERTEX input");
      if (nrm && nrm->stride < 3) nrm = nullptr;
      if (uv && uv->stride < 2) uv = nullptr;
      std::vector<uint32_t> counts, p;
      const size_t limit = reader.options.max_memory / sizeof(uint32_t) + 1;
      if (kind == "polygons") {
        for (auto poly : prim.children("p")) {
          std::vector<uint32_t> one;
          if (!Numbers(poly.child_value(), &one, limit)) return Fail("Invalid COLLADA <p>");
          counts.push_back(static_cast<uint32_t>(one.size() / stride));
          p.insert(p.end(), one.begin(), one.end());
        }
      } else {
        if (!Numbers(prim.child_value("p"), &p, limit)) return Fail("Invalid COLLADA <p>");
        if (kind == "polylist") {
          if (!Numbers(prim.child_value("vcount"), &counts, limit))
            return Fail("Invalid COLLADA <vcount>");
        } else {
          counts.assign(p.size() / stride / 3, 3);
        }
      }
      if (!reader.ChargeArray(p.size(), 48, err)) return false;
      auto symbol = bindings.find(prim.attribute("material").as_string());
      const int32_t material =
          Material(symbol != bindings.end() ? symbol->second : prim.attribute("material").as_string());
      size_t corner = 0;
      const double nm[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
      auto emit = [&](size_t c) -> bool {
        const size_t base = c * stride;
        if (base + stride > p.size()) return false;
        const size_t vi = p[base + pos_off];
        if ((vi + 1) * pos->stride > pos->data.size()) return false;
        const float* v = &pos->data[vi * pos->stride];
        for (int r = 0; r < 3; ++r)
          mesh->positions.push_back(static_cast<float>(
              unit * (m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3])));
        if (nrm) {
          const size_t ni = p[base + nrm_off];
          if ((ni + 1) * nrm->stride > nrm->data.size()) return false;
          const float* n = &nrm->data[ni * nrm->stride];
          double out[3];
          for (int r = 0; r < 3; ++r)
            out[r] = nm[r * 3] * n[0] + nm[r * 3 + 1] * n[1] + nm[r * 3 + 2] * n[2];
          const double len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
          for (double x : out) mesh->normals.push_back(static_cast<float>(len > 0 ? x / len : 0));
        }
        if (uv) {
          const size_t ti = p[base + uv_off];
          if ((ti + 1) * uv->stride > uv->data.size()) return false;
          mesh->uvs.push_back(uv->data[ti * uv->stride]);
          mesh->uvs.push_back(uv->data[ti * uv->stride + 1]);
        }
        return true;
      };
      for (uint32_t count : counts) {
        if (count < 3) {
          corner += count;
          continue;
        }
        // Fan-triangulate each face; one corner per output vertex.
        for (uint32_t k = 1; k + 1 < count; ++k) {
          for (size_t c : {corner, corner + k, corner + k + 1}) {
            if (mesh->positions.size() / 3 >= size_t(INT32_MAX) || !emit(c))
              return Fail("COLLADA primitive index out of range");
            mesh->indices.push_back(static_cast<int32_t>(mesh->positions.size() / 3 - 1));
          }
          mesh->materials.push_back(material);
        }
        corner += count;
      }
    }
    return true;
  }

  bool Node(pugi::xml_node node, Matrix m, size_t depth, Mesh* mesh) {
    if (depth > reader.options.max_depth) return Fail("COLLADA node hierarchy too deep");
    for (auto child : node.children()) {
      const std::string kind = child.name();
      std::vector<double> v;
      if (kind == "matrix" && Numbers(child.child_value(), &v, 16) && v.size() == 16) {
        Matrix t;
        std::copy(v.begin(), v.end(), t.begin());
        m = Multiply(m, t);
      } else if (kind == "translate" && Numbers(child.child_value(), &v, 3) && v.size() == 3) {
        Matrix t = Identity();
        t[3] = v[0], t[7] = v[1], t[11] = v[2];
        m = Multiply(m, t);
      } else if (kind == "scale" && Numbers(child.child_value(), &v, 3) && v.size() == 3) {
        Matrix t = Identity();
        t[0] = v[0], t[5] = v[1], t[10] = v[2];
        m = Multiply(m, t);
      } else if (kind == "rotate" && Numbers(child.child_value(), &v, 4) && v.size() == 4) {
        const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 0) {
          const double x = v[0] / len, y = v[1] / len, z = v[2] / len;
          const double a = v[3] * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a),
                       k = 1 - c;
          Matrix t = {x * x * k + c,     x * y * k - z * s, x * z * k + y * s, 0,
                      y * x * k + z * s, y * y * k + c,     y * z * k - x * s, 0,
                      z * x * k - y * s, z * y * k + x * s, z * z * k + c,     0,
                      0,                 0,                 0,                 1};
          m = Multiply(m, t);
        }
      }
    }
    for (auto child : node.children()) {
      const std::string kind = child.name();
      if (kind == "instance_geometry") {
        auto geometry = ids.find(Id(child.attribute("url").as_string()));
        if (geometry == ids.end()) continue;
        std::map<std::string, std::string> bindings;
        for (auto bound : child.child("bind_material").child("technique_common").children("instance_material"))
          bindings[bound.attribute("symbol").as_string()] = Id(bound.attribute("target").as_string());
        if (!Geometry(geometry->second, m, bindings, mesh)) return false;
      } else if (kind == "node") {
        if (!Node(child, m, depth + 1, mesh)) return false;
      } else if (kind == "instance_node") {
        auto target = ids.find(Id(child.attribute("url").as_string()));
        if (target != ids.end() && !Node(target->second, m, depth + 1, mesh)) return false;
      }
    }
    return true;
  }
};

}  // namespace

bool ReadDAE(std::string_view text, const std::string& source, Reader& reader,
             OBJ* obj, std::string* err) {
  pugi::xml_document doc;
  // pugixml parses in place from a copy; charge it like the input text.
  if (!reader.Charge(text.size(), err)) return false;
  // pugixml reads UTF-16/32 input in 2/4-byte units; give it an aligned copy
  // when the caller's view is not.
  std::string aligned;
  std::string_view source_text = text;
  if (reinterpret_cast<uintptr_t>(text.data()) % alignof(uint32_t)) {
    aligned.assign(text.data(), text.size());
    source_text = aligned;
  }
  const auto parsed = doc.load_buffer(source_text.data(), source_text.size());
  if (!parsed) {
    if (err) *err = std::string("COLLADA parse failed: ") + parsed.description() + ": " + source;
    return false;
  }
  pugi::xml_node root = doc.child("COLLADA");
  if (!root) {
    if (err) *err = "Not a COLLADA document: " + source;
    return false;
  }
  *obj = OBJ{};
  Reader2 r{reader, source, {}, {}, {}, obj, 1.0, err};
  // Index every element id (bounded by the document size already charged).
  std::vector<pugi::xml_node> stack{root};
  while (!stack.empty()) {
    pugi::xml_node node = stack.back();
    stack.pop_back();
    if (auto id = node.attribute("id")) r.ids.emplace(id.as_string(), node);
    for (auto child : node.children())
      if (child.type() == pugi::node_element) stack.push_back(child);
  }
  if (auto unit = root.child("asset").child("unit")) {
    const double meter = unit.attribute("meter").as_double(1.0);
    if (std::isfinite(meter) && meter > 0) r.unit = meter;
  }
  const std::string up = root.child("asset").child_value("up_axis");
  if (!up.empty() && up != "Z_UP")
    reader.Warn("COLLADA up_axis " + up + " is not applied (URDF/ROS convention)",
                "dae.up_axis_ignored", source);
  Mesh mesh;
  mesh.name = "dae";
  const auto scene_url = Id(root.child("scene").child("instance_visual_scene").attribute("url").as_string());
  auto scene = r.ids.find(scene_url);
  bool ok = true;
  if (scene != r.ids.end()) {
    ok = r.Node(scene->second, Identity(), 0, &mesh);
  } else {
    // No scene: every geometry once, untransformed.
    for (auto geometry : root.child("library_geometries").children("geometry"))
      if (!(ok = r.Geometry(geometry, Identity(), {}, &mesh))) break;
  }
  if (!ok) return false;
  if (mesh.indices.empty()) {
    if (err) *err = "COLLADA document contains no triangles: " + source;
    return false;
  }
  if (mesh.normals.size() != mesh.positions.size()) mesh.normals.clear();
  if (mesh.uvs.size() / 2 != mesh.positions.size() / 3) mesh.uvs.clear();
  obj->meshes.push_back(std::move(mesh));
  return true;
}

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
