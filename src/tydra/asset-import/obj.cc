// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <tuple>

#include "external/filesystem/include/ghc/filesystem.hpp"
#include "external/tinyobjloader-c/tiny_obj_c.h"
#include "import.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace {
struct Budget {
  size_t used = 0;
  Reader& reader;
  bool exceeded = false;
  explicit Budget(Reader& r) : reader(r) {}
  static void* Alloc(void* ud, size_t size, size_t) {
    auto& b = *static_cast<Budget*>(ud);
    const size_t remaining = b.reader.options.max_memory -
        std::min(b.reader.bytes, b.reader.options.max_memory);
    if (size > remaining) {
      if (!b.exceeded && b.reader.options.report)
        b.reader.options.report->Diagnostic("error", "import.memory_limit", "OBJ memory budget exceeded", b.reader.root_source);
      b.exceeded = true;
      return nullptr;
    }
    void* p = std::malloc(size ? size : 1);
    if (p) {
      b.used += size;
      b.reader.bytes += size;
      b.reader.peak_bytes = std::max(b.reader.peak_bytes, b.reader.bytes);
      if (b.reader.options.report) b.reader.options.report->ObserveMemory(b.reader.bytes);
    }
    return p;
  }
  static void Free(void* ud, void* p, size_t size) {
    auto& b = *static_cast<Budget*>(ud);
    if (p) {
      b.used -= std::min(size, b.used);
      b.reader.bytes -= std::min(size, b.reader.bytes);
      std::free(p);
    }
  }
};
struct Materials {
  struct Info {
    size_t source;
    bool roughness = false, metallic = false, ior = false;
    // Dissolve: `d` is authoritative over `Tr` (tinyobjloader C++ order).
    bool dissolve = false, transparency = false;
  };
  Reader* reader;
  std::string source;
  std::vector<std::string> anchors;
  std::vector<Info> entries;  // Match material definition order, not names.
  static tobj_result Resolve(void* ud, const char* name, tobj_mtl_source* out) {
    auto& m = *static_cast<Materials*>(ud);
    auto* bytes = new (std::nothrow) std::vector<uint8_t>;
    if (!bytes) return TOBJ_ERR_ALLOC;
    std::string resolved, err;
    if (!m.reader->Read(name, m.source, bytes, &resolved, &err)) {
      delete bytes;
      if (!m.reader->failure.empty()) return TOBJ_ERR_LIMIT_EXCEEDED;
      m.reader->Warn("MTL: " + err, "obj.material_unresolved", m.source);
      return TOBJ_ERR_NOT_FOUND;
    }
    // Duplicate names are independent definitions. Keep source/field presence
    // by material id so a later definition cannot change the first one's maps.
    if (!m.reader->Charge(2 * sizeof(std::string), &err) ||
        !m.reader->Charge(resolved.size(), &err)) {
      delete bytes;
      return TOBJ_ERR_LIMIT_EXCEEDED;
    }
    const size_t anchor = m.anchors.size();
    m.anchors.push_back(std::move(resolved));
    std::string_view text(bytes->empty() ? "" : reinterpret_cast<const char*>(bytes->data()),
                          bytes->size());
    size_t pos = text.substr(0, 3) == "\xef\xbb\xbf" ? 3 : 0;
    size_t current = SIZE_MAX;
    while (pos < text.size()) {
      size_t end = text.find_first_of("\r\n", pos);
      if (end == std::string::npos) end = text.size();
      std::string_view line = text.substr(pos, end - pos);
      size_t begin = line.find_first_not_of(" \t");
      if (begin != std::string::npos) {
        size_t last = line.find_first_of(" \t", begin);
        if (last == std::string::npos) last = line.size();
        const auto command = line.substr(begin, last - begin);
        if (command == "newmtl") {
          if (!m.reader->Charge(2 * sizeof(Info), &err)) {
            delete bytes;
            return TOBJ_ERR_LIMIT_EXCEEDED;
          }
          current = m.entries.size();
          m.entries.push_back(Info{anchor});
        } else if (current != SIZE_MAX) {
          if (command == "Pr") m.entries[current].roughness = true;
          if (command == "Pm") m.entries[current].metallic = true;
          if (command == "Ni") m.entries[current].ior = true;
          if (command == "d") m.entries[current].dissolve = true;
          if (command == "Tr") m.entries[current].transparency = true;
        }
      }
      pos = end;
      if (pos < text.size() && text[pos] == '\r') ++pos;
      if (pos < text.size() && text[pos] == '\n') ++pos;
    }
    out->data = bytes->data();
    out->len = bytes->size();
    out->release_ud = bytes;
    out->release = [](void* p, const uint8_t*, size_t) {
      delete static_cast<std::vector<uint8_t>*>(p);
    };
    return TOBJ_OK;
  }
};
std::array<double, 3> Unit(std::array<double, 3> n) {
  // Finite float positions bound these area vectors well inside double's
  // squared-length range, including areas from subnormal coordinates.
  const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (len > 0)
    for (double& v : n) v /= len;
  return n;
}
std::array<double, 3> FaceNormal(const tobj_index* indices, size_t count,
                                const float* positions) {
  // Indices have been checked before normal generation. Newell's formula
  // retains polygon area for smoothing and works for non-triangular faces.
  std::array<double, 3> n{};
  for (size_t k = 0; k < count; ++k) {
    const float* p = positions + size_t(indices[k].vertex_index) * 3;
    const float* q = positions + size_t(indices[(k + 1) % count].vertex_index) * 3;
    n[0] += (double(p[1]) - q[1]) * (double(p[2]) + q[2]);
    n[1] += (double(p[2]) - q[2]) * (double(p[0]) + q[0]);
    n[2] += (double(p[0]) - q[0]) * (double(p[1]) + q[1]);
  }
  return n;
}
// Not `RGB`: <wingdi.h> defines that as a macro on Windows.
Json ColorJSON(const float* v) { return Json::array({v[0], v[1], v[2]}); }
}  // namespace

Json MeshRefJSON(const Mesh& m, Reader& reader) {
  if (!reader.options.typed_meshes) return MeshJSON(m);
  Mesh rest;
  rest.name = m.name;
  rest.type = m.type;
  rest.counts = m.counts;
  rest.colors = m.colors;
  rest.materials = m.materials;
  rest.joint_indices = m.joint_indices;
  rest.joint_weights = m.joint_weights;
  rest.influences = m.influences;
  Json mesh = MeshJSON(rest);
  const std::string id = "mesh_" + std::to_string(reader.meshes.size());
  reader.meshes[id] = MeshBuffer{m.positions, m.normals, m.uvs, m.indices};
  mesh["meshRef"] = id;
  return mesh;
}

Json MeshJSON(const Mesh& m) {
  // Assign by move: braced initializer lists copy every (large) array.
  Json geometry = Json::object();
  geometry["positions"] = Array(m.positions);
  geometry["normals"] = Array(m.normals);
  geometry["uvs"] = Array(m.uvs);
  geometry["colors"] = Array(m.colors);
  geometry["indices"] = Array(m.indices);
  geometry["faceVertexCounts"] = Array(m.counts);
  geometry["materialIndices"] = Array(m.materials);
  geometry["jointIndices"] = Array(m.joint_indices);
  geometry["jointWeights"] = Array(m.joint_weights);
  geometry["influences"] = m.influences;
  Json mesh = Json::object();
  mesh["name"] = m.name;
  mesh["type"] = m.type;
  mesh["geometry"] = std::move(geometry);
  return mesh;
}

bool ReadOBJ(std::string_view text, const std::string& source, Reader& reader,
             bool triangulate, OBJ* obj, std::string* err, bool collect_vertices) {
  if (!obj || text.size() > reader.options.max_input) {
    if (err) *err = "OBJ input limit exceeded or null output";
    return false;
  }
  // Numeric validation is done by the bounded C11 scanner, without a second
  // pass or a NUL-terminated copy of a potentially multi-gigabyte input.
  Budget budget(reader);
  const size_t remaining = reader.options.max_memory -
      std::min(reader.bytes, reader.options.max_memory);
  tobj_load_config cfg = tobj_default_config();
  cfg.allocator = {};
  cfg.allocator.alloc = Budget::Alloc;
  cfg.allocator.free = Budget::Free;
  cfg.allocator.user_data = &budget;
  cfg.allocator.max_alloc_size = 0;  // Budget::Alloc enforces the aggregate cap.
  cfg.triangulate = triangulate;
  cfg.vertex_color_fallback = false;
  cfg.store_vertex_colors = true;
  cfg.max_input_bytes = reader.options.max_input;
  cfg.max_vertices =
      std::min(size_t(INT32_MAX), remaining / (sizeof(float) * 3));
  cfg.max_indices =
      std::min(size_t(INT32_MAX), remaining / sizeof(tobj_index));
  cfg.max_faces = cfg.max_indices;
  cfg.max_face_arity = 65536;
  cfg.max_line_bytes =
      std::min(size_t(16) * 1024 * 1024, reader.options.max_input);
  Materials materials{&reader, source, {}, {}};
  cfg.mtl_resolver = Materials::Resolve;
  cfg.mtl_resolver_user_data = &materials;
  tobj_scene_f scene{};
  tobj_diag diag{};
  diag.user_data = &reader;
  diag.on_message = [](void* p, tobj_severity severity, size_t line,
                       const char* message, size_t len) {
    auto& r = *static_cast<Reader*>(p);
    std::string msg =
        "OBJ line " + std::to_string(line) + ": " + std::string(message, len);
    if (severity == TOBJ_SEV_WARNING)
      r.Warn(msg, "obj.parser_warning", r.root_source, line);
    else {
      r.failure = msg;
      if (r.options.report) r.options.report->Diagnostic("error", "obj.parse_error", msg, r.root_source, line);
    }
  };
  tobj_result status = tobj_load_obj_from_memory_f(
      &scene, reinterpret_cast<const uint8_t*>(text.data()), text.size(), &cfg,
      &diag);
  if (status != TOBJ_OK) {
    if (err)
      *err = budget.exceeded
                 ? "OBJ memory budget exceeded"
                 : (reader.failure.empty() ? tobj_result_string(status)
                                           : reader.failure);
    return false;
  }
  struct Cleanup {
    tobj_scene_f* scene;
    ~Cleanup() { tobj_scene_free_f(scene); }
  } cleanup{&scene};
  const auto& a = scene.attrib;
  std::vector<const tobj_skin_weight_f*> weights;
  size_t influence_count = 0;
  if (a.num_skin_weights) {
    if (!reader.ChargeArray(a.vertices.count / 3, sizeof(void*), err)) return false;
    weights.resize(a.vertices.count / 3, nullptr);
    for (size_t i = 0; i < a.num_skin_weights; ++i) {
      const auto& row = a.skin_weights[i];
      weights[static_cast<size_t>(row.vertex_id)] = &row;
      influence_count = std::max(influence_count, row.num_weights);
    }
    if (!influence_count)
      reader.Warn("OBJ vw records have no influences; omitted skin binding", "obj.empty_skin_weights", source);
  }
  auto skin = [&](Mesh* mesh) {
    if (!influence_count) return true;
    if (influence_count > size_t(INT32_MAX) ||
        !reader.ChargeArray(mesh->source_vertices.size(), influence_count * 8, err))
      return false;
    const size_t elements = mesh->source_vertices.size() * influence_count;
    mesh->joint_indices.reserve(elements);
    mesh->joint_weights.reserve(elements);
    mesh->influences = static_cast<int32_t>(influence_count);
    size_t missing = 0;
    for (int32_t source_id : mesh->source_vertices) {
      const auto* row = weights[static_cast<size_t>(source_id)];
      if (!row || !row->num_weights) ++missing;
      for (size_t k = 0; k < influence_count; ++k) {
        const bool present = row && k < row->num_weights;
        mesh->joint_indices.push_back(present ? row->weights[k].joint_id : 0);
        mesh->joint_weights.push_back(present ? row->weights[k].weight : 0.0f);
      }
    }
    if (missing)
      reader.Warn("OBJ missing skin weights for " + std::to_string(missing) +
                  " points on " + mesh->name + "; padded with zero weights", "obj.missing_skin_weights", source);
    return true;
  };
  for (size_t i = 0; i < a.vertices.count; i++)
    if (!std::isfinite(a.vertices.ptr[i])) {
      if (err) *err = "OBJ has non-finite positions";
      return false;
    }
  if (materials.entries.size() != scene.num_materials) {
    if (err) *err = "OBJ material source metadata count mismatch";
    return false;
  }
  if (!reader.ChargeArray(scene.num_materials, 4096, err)) return false;
  for (size_t mi = 0; mi < scene.num_materials; mi++) {
    const auto& m = scene.materials[mi];
    const auto& info = materials.entries[mi];
    const std::string name = m.name ? m.name : "material";
    Json mat{{"name", name},
             {"rgba", Json::array({m.diffuse[0], m.diffuse[1], m.diffuse[2],
                                   m.dissolve})},
             {"specularColor", ColorJSON(m.specular)},
             {"emissiveColor", ColorJSON(m.emission)},
             {"roughness", info.roughness
                               ? m.roughness
                               : float(std::sqrt(2.0 / (m.shininess + 2.0)))},
             {"metallic", m.metallic},
             {"useSpecularWorkflow",
              info.metallic ? 0 : 1}};
    // An absent Ni must not author ior = 1, which zeroes dielectric F0.
    if (info.ior) mat["ior"] = m.ior;
    if (m.clearcoat_thickness > 0) {
      mat["clearcoat"] = m.clearcoat_thickness;
      mat["clearcoatRoughness"] = m.clearcoat_roughness;
    }
    Json maps = Json::array();
    auto texture = [&](const char* file, const char* input,
                       const tobj_texture_option_f& opt, bool color) {
      if (!file || !*file) return;
      AssetBytes bytes;
      std::string resolved, why;
      const std::string& anchor = materials.anchors[info.source];
      if (!reader.ReadTexture(file, anchor, &bytes, &resolved, &why)) {
        reader.Warn("Texture: " + why, "import.texture_unresolved", anchor);
        return;
      }
      maps.push_back(Json{
          {"input", input},
          {"file", resolved},
          {"color", color},
          {"scale", Json::array({opt.scale[0], opt.scale[1]})},
          {"offset", Json::array({opt.origin_offset[0], opt.origin_offset[1]})},
          {"clamp", opt.clamp}});
      Json& map = maps[maps.size() - 1];
      // -imfchan selects the scalar channel; -bm scales normal maps.
      if (opt.imfchan) map["channel"] = std::string(1, opt.imfchan);
      if (std::string(input) == "normal" && opt.bump_multiplier != 1)
        map["bumpMultiplier"] = opt.bump_multiplier;
      if (opt.colorspace && *opt.colorspace) map["colorSpace"] = opt.colorspace;
    };
    texture(m.diffuse_texname, "diffuseColor", m.diffuse_texopt, true);
    texture(m.specular_texname, "specularColor", m.specular_texopt, true);
    texture(m.emissive_texname, "emissiveColor", m.emissive_texopt, true);
    texture(m.alpha_texname, "opacity", m.alpha_texopt, false);
    texture(m.roughness_texname, "roughness", m.roughness_texopt, false);
    texture(m.metallic_texname, "metallic", m.metallic_texopt, false);
    texture(m.normal_texname, "normal", m.normal_texopt, false);
    if (m.bump_texname && *m.bump_texname) {
      std::string data;
      if (reader.options.unsupported_policy == UnsupportedPolicy::Preserve)
        data = Json{{"material", name}, {"bump", m.bump_texname},
                    {"bump_multiplier", m.bump_texopt.bump_multiplier}}.dump();
      if (!reader.Unsupported(
              "MTL bump maps require height-to-normal conversion and were not connected",
              "obj.bump_map_unconnected", "obj.bump_map", materials.anchors[info.source],
              0, std::move(data), "json")) { if (err) *err = reader.failure; return false; }
    }
    if ((m.displacement_texname && *m.displacement_texname) || m.sheen != 0 ||
        m.anisotropy != 0) {
      std::string data;
      if (reader.options.unsupported_policy == UnsupportedPolicy::Preserve)
        data = Json{{"material", name}, {"displacement", m.displacement_texname ? m.displacement_texname : ""},
                    {"sheen", m.sheen}, {"anisotropy", m.anisotropy}}.dump();
      if (!reader.Unsupported("MTL contains shading features outside UsdPreviewSurface",
              "obj.shading_unsupported", "obj.shading", materials.anchors[info.source],
              0, std::move(data), "json")) { if (err) *err = reader.failure; return false; }
    }
    mat["maps"] = std::move(maps);
    obj->materials.push_back(std::move(mat));
  }
  // First identify the position/group pairs that actually need generated
  // normals. Fully authored normals need neither a smoothing map nor a
  // geometric-normal array proportional to the face count.
  using SmoothKey = std::pair<int, unsigned>;
  std::map<SmoothKey, std::array<double, 3>> smooth;
  const auto has_normal = [&](const tobj_index& ix) {
    return ix.normal_index >= 0 && size_t(ix.normal_index) < a.normals.count / 3;
  };
  bool any_authored_normals = false;
  if (a.normals.count)
    for (size_t si = 0; si < scene.num_shapes; ++si) {
      const auto& mesh = scene.shapes[si].mesh;
      if (mesh.num_indices &&
          std::any_of(mesh.indices, mesh.indices + mesh.num_indices, has_normal)) {
        any_authored_normals = true;
        break;
      }
    }
  for (size_t si = 0; si < scene.num_shapes; si++) {
    const auto& shape = scene.shapes[si];
    size_t off = 0;
    for (size_t f = 0; f < shape.mesh.num_faces; f++) {
      unsigned count = shape.mesh.num_face_vertices[f];
      if (count < 3 || off > shape.mesh.num_indices ||
          count > shape.mesh.num_indices - off) {
        if (err) *err = "Invalid OBJ face";
        return false;
      }
      const unsigned sg = shape.mesh.smoothing_group_ids
                              ? shape.mesh.smoothing_group_ids[f] : 0;
      for (unsigned k = 0; k < count; k++) {
        const auto& ix = shape.mesh.indices[off + k];
        if (ix.vertex_index < 0 || size_t(ix.vertex_index) >= a.vertices.count / 3) {
          if (err) *err = "OBJ vertex index out of range";
          return false;
        }
      }
      // With entirely generated normals, every smoothing key is needed and
      // can receive its contribution immediately, avoiding a second map pass.
      const auto normal = sg && !any_authored_normals
                              ? FaceNormal(shape.mesh.indices + off, count, a.vertices.ptr)
                              : std::array<double, 3>{};
      if (sg)
        for (unsigned k = 0; k < count; ++k) {
          const auto& ix = shape.mesh.indices[off + k];
          if (has_normal(ix)) continue;
          const SmoothKey key{ix.vertex_index, sg};
          auto found = smooth.find(key);
          if (found == smooth.end()) {
            if (!reader.Charge(sizeof(SmoothKey) + sizeof(std::array<double, 3>) +
                               4 * sizeof(void*), err)) return false;
            found = smooth.emplace(key, std::array<double, 3>{}).first;
          }
          if (!any_authored_normals)
            for (size_t c = 0; c < 3; ++c) found->second[c] += normal[c];
        }
      off += count;
    }
    if (off != shape.mesh.num_indices) {
      if (err) *err = "Invalid OBJ face index count";
      return false;
    }
  }
  // Every incident face contributes to a requested pair, including faces with
  // authored normals and faces in another object/group partition.
  if (any_authored_normals && !smooth.empty())
    for (size_t si = 0; si < scene.num_shapes; ++si) {
      const auto& mesh = scene.shapes[si].mesh;
      size_t off = 0;
      for (size_t f = 0; f < mesh.num_faces; ++f) {
        const size_t count = mesh.num_face_vertices[f];
        const unsigned sg = mesh.smoothing_group_ids ? mesh.smoothing_group_ids[f] : 0;
        const auto* indices = mesh.indices + off;
        if (sg && std::any_of(indices, indices + count, [&](const tobj_index& ix) {
              return smooth.find({ix.vertex_index, sg}) != smooth.end();
            })) {
          const auto normal = FaceNormal(indices, count, a.vertices.ptr);
          for (size_t k = 0; k < count; ++k) {
            auto found = smooth.find({indices[k].vertex_index, sg});
            if (found != smooth.end())
              for (size_t c = 0; c < 3; ++c) found->second[c] += normal[c];
          }
        }
        off += count;
      }
    }
  for (auto& entry : smooth) entry.second = Unit(entry.second);
  for (size_t si = 0; si < scene.num_shapes; si++) {
    const auto& shape = scene.shapes[si];
    if ((triangulate && (shape.lines.num_lines || shape.points.num_indices)) ||
        shape.freeform)
      reader.Warn(
          "OBJ line, point or free-form primitives are not converted to "
          "polygon meshes");
    if (!shape.mesh.num_faces) continue;
    Mesh mesh;
    mesh.name = shape.name && *shape.name ? shape.name : "mesh";
    size_t off = 0;
    bool uv_complete = true;
    // Corner dedup on (vertex, normal, uv, smoothing group, face): open
    // addressing over the corner list. Indices follow first insertion, so the
    // result matches an ordered map.
    struct Key {
      int v, n, t;
      unsigned sg;
      size_t face;
      bool operator==(const Key& o) const {
        return v == o.v && n == o.n && t == o.t && sg == o.sg && face == o.face;
      }
    };
    std::vector<Key> keys;
    std::vector<int32_t> slots;
    {
      size_t capacity = 16;
      while (capacity < 2 * shape.mesh.num_indices) capacity <<= 1;
      slots.assign(capacity, -1);
      keys.reserve(std::min(shape.mesh.num_indices, a.vertices.count / 3 + 16));
    }
    auto hash = [](const Key& k) {
      uint64_t h = 1469598103934665603ull;
      for (uint64_t x : {uint64_t(uint32_t(k.v)), uint64_t(uint32_t(k.n)), uint64_t(uint32_t(k.t)),
                         uint64_t(k.sg), uint64_t(k.face)}) {
        h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 1099511628211ull;
      }
      return h;
    };
    // Slot holding `key`, or the empty slot to claim for it.
    auto probe = [&](const Key& key) -> int32_t& {
      const size_t mask = slots.size() - 1;
      size_t i = static_cast<size_t>(hash(key)) & mask;
      while (slots[i] >= 0 && !(keys[size_t(slots[i])] == key)) i = (i + 1) & mask;
      return slots[i];
    };
    // Output vectors (including capacity slack), corner-map nodes and scratch.
    // Use checked multiplication before any large allocations or int casts.
    if (shape.mesh.num_indices > size_t(INT32_MAX) ||
        !reader.ChargeArray(shape.mesh.num_indices, 160, err)) {
      if (err && err->empty()) *err = "OBJ expanded index limit exceeded";
      return false;
    }
    const size_t initial_points = std::min(shape.mesh.num_indices, a.vertices.count / 3);
    mesh.positions.reserve(initial_points * 3);
    mesh.normals.reserve(initial_points * 3);
    mesh.source_vertices.reserve(initial_points);
    mesh.indices.reserve(shape.mesh.num_indices);
    mesh.counts.reserve(shape.mesh.num_faces);
    mesh.materials.reserve(shape.mesh.num_faces);
    if (a.texcoords.count) mesh.uvs.reserve(initial_points * 2);
    if (a.colors.count) mesh.colors.reserve(initial_points * 3);
    if (reader.obj_indexed) {
      // Compact topology: points are the shape's source vertices (first-use
      // order); normals and UVs are face-varying tables with per-corner
      // indices, so positions and per-vertex skin weights are not split.
      std::vector<int32_t> point_of(a.vertices.count / 3, -1);
      std::vector<int32_t> uv_of(a.texcoords.count / 2, -1);
      struct NormalKey {
        int kind, a;
        size_t b;
        bool operator==(const NormalKey& o) const { return kind == o.kind && a == o.a && b == o.b; }
      };
      struct NormalHash {
        size_t operator()(const NormalKey& k) const {
          return std::hash<uint64_t>()((uint64_t(k.kind) << 60) ^ (uint64_t(uint32_t(k.a)) << 28) ^ k.b);
        }
      };
      std::unordered_map<NormalKey, int32_t, NormalHash> normal_of;
      for (size_t f = 0; f < shape.mesh.num_faces; f++) {
        const unsigned count = shape.mesh.num_face_vertices[f];
        const unsigned sg = shape.mesh.smoothing_group_ids ? shape.mesh.smoothing_group_ids[f] : 0;
        mesh.counts.push_back(static_cast<int32_t>(count));
        mesh.materials.push_back(shape.mesh.material_ids ? shape.mesh.material_ids[f] : -1);
        std::array<double, 3> face_normal{};
        bool have_face_normal = false;
        for (unsigned k = 0; k < count; k++) {
          const auto ix = shape.mesh.indices[off + k];
          int32_t& point = point_of[size_t(ix.vertex_index)];
          if (point < 0) {
            point = static_cast<int32_t>(mesh.positions.size() / 3);
            mesh.source_vertices.push_back(ix.vertex_index);
            const float* p = a.vertices.ptr + size_t(ix.vertex_index) * 3;
            mesh.positions.insert(mesh.positions.end(), p, p + 3);
            if (a.colors.count == a.vertices.count)
              mesh.colors.insert(mesh.colors.end(), a.colors.ptr + size_t(ix.vertex_index) * 3,
                                 a.colors.ptr + size_t(ix.vertex_index) * 3 + 3);
          }
          mesh.indices.push_back(point);
          const bool hn = has_normal(ix);
          const NormalKey key = hn ? NormalKey{0, ix.normal_index, 0}
                                   : sg ? NormalKey{1, ix.vertex_index, sg} : NormalKey{2, 0, f};
          auto found = normal_of.find(key);
          if (found == normal_of.end()) {
            if (!hn && !sg && !have_face_normal) {
              face_normal = Unit(FaceNormal(shape.mesh.indices + off, count, a.vertices.ptr));
              have_face_normal = true;
            }
            const std::array<double, 3> n =
                hn ? std::array<double, 3>{{a.normals.ptr[size_t(ix.normal_index) * 3],
                                            a.normals.ptr[size_t(ix.normal_index) * 3 + 1],
                                            a.normals.ptr[size_t(ix.normal_index) * 3 + 2]}}
                   : (sg ? smooth.find({ix.vertex_index, sg})->second : face_normal);
            found = normal_of.emplace(key, static_cast<int32_t>(mesh.normals.size() / 3)).first;
            for (double c : n) mesh.normals.push_back(static_cast<float>(c));
          }
          mesh.normal_indices.push_back(found->second);
          const bool ht = ix.texcoord_index >= 0 && size_t(ix.texcoord_index) < a.texcoords.count / 2;
          uv_complete &= ht;
          if (ht) {
            int32_t& uv = uv_of[size_t(ix.texcoord_index)];
            if (uv < 0) {
              uv = static_cast<int32_t>(mesh.uvs.size() / 2);
              mesh.uvs.push_back(a.texcoords.ptr[size_t(ix.texcoord_index) * 2]);
              mesh.uvs.push_back(a.texcoords.ptr[size_t(ix.texcoord_index) * 2 + 1]);
            }
            mesh.uv_indices.push_back(uv);
          }
        }
        off += count;
      }
      if (!uv_complete) mesh.uv_indices.clear();
    } else
    {
    for (size_t f = 0; f < shape.mesh.num_faces; f++) {
        unsigned count = shape.mesh.num_face_vertices[f];
        unsigned sg = shape.mesh.smoothing_group_ids
                          ? shape.mesh.smoothing_group_ids[f]
                          : 0;
        mesh.counts.push_back(static_cast<int32_t>(count));
        mesh.materials.push_back(
            shape.mesh.material_ids ? shape.mesh.material_ids[f] : -1);
        std::array<double, 3> face_normal{};
        bool have_face_normal = false;
        for (unsigned k = 0; k < count; k++) {
          const auto ix = shape.mesh.indices[off + k];
          const bool hn = has_normal(ix);
          bool ht = ix.texcoord_index >= 0 &&
                    size_t(ix.texcoord_index) < a.texcoords.count / 2;
          uv_complete &= ht;
          const Key key{ix.vertex_index, hn ? ix.normal_index : -1,
                        ht ? ix.texcoord_index : -1, hn ? 0u : sg, hn || sg ? 0 : f + 1};
          int32_t& slot = probe(key);
          if (slot < 0) {
            slot = static_cast<int32_t>(keys.size());
            keys.push_back(key);
            mesh.source_vertices.push_back(ix.vertex_index);
            const float* p = a.vertices.ptr + size_t(ix.vertex_index) * 3;
            mesh.positions.insert(mesh.positions.end(), p, p + 3);
            if (!hn && !sg && !have_face_normal) {
              face_normal = Unit(FaceNormal(shape.mesh.indices + off, count, a.vertices.ptr));
              have_face_normal = true;
            }
            auto n = hn ? std::array<
                              double,
                              3>{{a.normals.ptr[size_t(ix.normal_index) * 3],
                                  a.normals.ptr[size_t(ix.normal_index) * 3 + 1],
                                  a.normals.ptr[size_t(ix.normal_index) * 3 + 2]}}
                        : (sg ? smooth.find({ix.vertex_index, sg})->second
                              : face_normal);
            for (double c : n) mesh.normals.push_back(static_cast<float>(c));
            mesh.uvs.push_back(ht ? a.texcoords.ptr[size_t(ix.texcoord_index) * 2]
                                  : 0);
            mesh.uvs.push_back(
                ht ? a.texcoords.ptr[size_t(ix.texcoord_index) * 2 + 1] : 0);
            if (a.colors.count == a.vertices.count)
              mesh.colors.insert(mesh.colors.end(),
                                 a.colors.ptr + size_t(ix.vertex_index) * 3,
                                 a.colors.ptr + size_t(ix.vertex_index) * 3 + 3);
          }
          mesh.indices.push_back(slot);
        }
        off += count;
      }
    }
    if (!uv_complete) {
      mesh.uvs.clear();
      if (a.texcoords.count)
        reader.Warn("OBJ has incomplete UVs; omitted primvars:st on " +
                    mesh.name);
    }
    if (!skin(&mesh)) return false;
    obj->meshes.push_back(std::move(mesh));
  }
  if (!triangulate)
    for (size_t si = 0; si < scene.num_shapes; ++si) {
      const auto& shape = scene.shapes[si];
      for (bool lines : {true, false}) {
        const size_t count =
            lines ? shape.lines.num_indices : shape.points.num_indices;
        if (!count) continue;
        if (count > size_t(INT32_MAX) || !reader.ChargeArray(count, 64, err)) {
          if (err && err->empty()) *err = "OBJ point/line index limit exceeded";
          return false;
        }
        Mesh primitive;
        primitive.positions.reserve(count * 3);
        primitive.source_vertices.reserve(count);
        primitive.type = lines ? "BasisCurves" : "Points";
        primitive.name =
            std::string(shape.name && *shape.name ? shape.name : "mesh") +
            (lines ? "_curves" : "_points");
        bool complete_uv = true;
        for (size_t i = 0; i < count; ++i) {
          const auto ix =
              lines ? shape.lines.indices[i] : shape.points.indices[i];
          if (ix.vertex_index < 0 ||
              size_t(ix.vertex_index) >= a.vertices.count / 3) {
            if (err) *err = "OBJ point/line index out of range";
            return false;
          }
          const size_t v = static_cast<size_t>(ix.vertex_index);
          primitive.source_vertices.push_back(ix.vertex_index);
          primitive.positions.insert(primitive.positions.end(),
                                     a.vertices.ptr + v * 3,
                                     a.vertices.ptr + v * 3 + 3);
          if (a.colors.count == a.vertices.count)
            primitive.colors.insert(primitive.colors.end(),
                                    a.colors.ptr + v * 3,
                                    a.colors.ptr + v * 3 + 3);
          if (ix.texcoord_index >= 0 &&
              size_t(ix.texcoord_index) < a.texcoords.count / 2) {
            const size_t t = static_cast<size_t>(ix.texcoord_index) * 2;
            primitive.uvs.insert(primitive.uvs.end(), a.texcoords.ptr + t,
                                 a.texcoords.ptr + t + 2);
          } else
            complete_uv = false;
        }
        if (!complete_uv) primitive.uvs.clear();
        if (lines)
          for (size_t i = 0; i < shape.lines.num_lines; ++i)
            primitive.counts.push_back(shape.lines.num_line_vertices[i]);
        if (!skin(&primitive)) return false;
        obj->meshes.push_back(std::move(primitive));
      }
    }
  if (obj->meshes.empty()) {
    if (!collect_vertices) {
      if (err)
        *err = triangulate ? "OBJ contains no polygon faces"
                           : "OBJ contains no supported primitives";
      return false;
    }
    if (!a.vertices.count) {
      if (err) *err = "OBJ contains no vertices";
      return false;
    }
    // Only a face-less file needs its positions: MJCF hulls them the way
    // MuJoCo does. Files with faces do not pay for this copy.
    if (!reader.ChargeArray(a.vertices.count, sizeof(float), err)) return false;
    obj->vertices.assign(a.vertices.ptr, a.vertices.ptr + a.vertices.count);
  }
  return true;
}

bool BuildOBJScene(std::string_view text, const std::string& source,
                   const Options& options, OBJ* obj, Result* result,
                   std::string* err) {
  Options effective = options;
  ImportSession session(&effective, source, "obj", err);
  effective.report->format = "obj";
  effective.report->input_bytes = text.size();
  if (!obj || !result || text.size() > options.max_input ||
      text.size() > options.max_memory) {
    if (err) *err = "OBJ input limit exceeded";
    return false;
  }
  *result = Result{};
  result->report = effective.report;
  if (options.up_axis != "Z" && options.up_axis != "Y") {
    if (err) *err = "up_axis must be Z or Y";
    return false;
  }
  Reader reader(effective);
  reader.root_source = source;
  reader.allow_unanchored = !source.empty();
  reader.obj_indexed = options.obj_topology == "indexed";
  *obj = OBJ{};
  if (!reader.Charge(text.size(), err) ||
      !ReadOBJ(text, source, reader, false, obj, err)) return false;
  result->payload = Json{{"sourceFormat", "obj"}, {"noPhysics", true},
      {"upAxis", options.up_axis}, {"profile", options.profile},
      {"materials", std::move(obj->materials)}};
  if (!GatherTextures(reader, result, err)) return false;
  if (!reader.preserved.empty()) result->payload["unsupported"] = std::move(reader.preserved);
  result->warnings = reader.warnings;
  result->memory_estimate = reader.bytes;
  auto& report = *effective.report;
  report.counts["meshes"] = uint64_t(obj->meshes.size());
  report.counts["materials"] = uint64_t(result->payload["materials"].size());
  size_t vertices = 0, faces = 0, indices = 0;
  for (const auto& mesh : obj->meshes) {
    vertices += mesh.positions.size() / 3;
    faces += mesh.counts.size();
    indices += mesh.indices.size();
  }
  report.counts["output_vertices"] = uint64_t(vertices);
  report.counts["output_faces_or_curves"] = uint64_t(faces);
  report.counts["output_indices"] = uint64_t(indices);
  report.Feature("obj.geometry", "imported", source);
  if (!result->payload["materials"].empty()) report.Feature("obj.materials", "imported", source);
  session.Succeed();
  return true;
}

bool BuildOBJPayload(std::string_view text, const std::string& source,
                     const Options& options, Result* result, std::string* err) {
  Options effective = options;
  // The JSON payload carries expanded (per-corner) meshes only.
  effective.obj_topology = "expanded";
  ImportSession session(&effective, source, "obj", err);
  OBJ obj;
  if (!BuildOBJScene(text, source, effective, &obj, result, err)) return false;
  // The next reader bypasses this compatibility bridge. Bound its scalar JSON
  // nodes as well, so legacy/CLI imports cannot silently amplify a small input.
  Reader conversion_budget(effective);
  conversion_budget.bytes = result->memory_estimate;
  for (const auto& mesh : obj.meshes) {
    for (size_t count : {mesh.positions.size(), mesh.normals.size(), mesh.uvs.size(),
                        mesh.colors.size(), mesh.indices.size(), mesh.counts.size(),
                        mesh.materials.size(), mesh.joint_indices.size(), mesh.joint_weights.size()})
      if (!conversion_budget.ChargeArray(count, sizeof(Json), err)) return false;
    if (!conversion_budget.ChargeArray(result->payload["materials"].size(), sizeof(Json), err) ||
        !conversion_budget.Charge(2048, err)) return false;
  }
  Json names = Json::array(), visuals = Json::array();
  for (const auto& m : result->payload["materials"]) names.push_back(m["name"]);
  for (const auto& m : obj.meshes) {
    Json visual = MeshJSON(m);
    visual["renderOnly"] = true;
    visual["geometry"]["materialNames"] = names;
    visuals.push_back(std::move(visual));
  }
  // Moves, not a braced initializer list (which would copy every mesh).
  Json link = Json::object();
  link["name"] = "Geometry";
  link["visuals"] = std::move(visuals);
  Json links = Json::array();
  links.push_back(std::move(link));
  result->payload["links"] = std::move(links);
  session.Succeed();
  return true;
}

bool BuildOBJPayloadFile(const std::string& filename, const Options& options,
                         Result* result, std::string* err) {
  Options effective = options;
  ImportSession session(&effective, filename, "obj", err);
  Reader reader(effective);
  std::vector<uint8_t> bytes;
  std::string source;
  if (!reader.Read(filename, "", &bytes, &source, err)) return false;
  if (!BuildOBJPayload(
      std::string_view(bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()), bytes.size()),
      source, effective, result, err)) return false;
  session.Succeed();
  return true;
}
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
