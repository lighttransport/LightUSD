// SPDX-License-Identifier: Apache-2.0
#include "material-atlas.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "layer/asset-anchor.hh"
#include "material-optimize.hh"
#include "sha256.hh"
#include "writer/usda-writer.hh"

namespace lightusd::usdz {
namespace {
constexpr char kFile[] = "inputs:file";
constexpr char kUV[] = "primvars:st";
constexpr char kUVIndices[] = "primvars:st:indices";
constexpr char kAtlasUVName[] = "lightusdMaterialAtlasUV";
constexpr char kAtlasUV[] = "primvars:lightusdMaterialAtlasUV";
constexpr char kAtlasUVIndices[] = "primvars:lightusdMaterialAtlasUV:indices";

const Value* Property(const PrimSpec& prim, const char* name) {
  return prim.property_value(name);
}

StringView Token(const PrimSpec& prim, const char* name) {
  const auto* value = Property(prim, name);
  return value ? value->as_token() : StringView{};
}

bool Connects(const PrimSpec& prim, const char* input, const PrimSpec& target,
              const char* output) {
  const auto* connection = prim.connection(input);
  return connection && connection->size() == 1 &&
         connection->front().str() == target.path().str() + "." + output;
}

struct Material {
  uint32_t texture = 0;
  uint32_t reader = 0;
  std::string file;
  bool srgb = true;
  bool eligible = true;
  std::vector<uint32_t> meshes;
};

bool Describe(const Layer& layer, const PrimSpec& material, Material* result) {
  if (material.child_indices().size() != 3 || material.has_any_time_samples() ||
      material.path().str() == "/" + layer.meta().defaultPrim)
    return false;
  const PrimSpec* surface = nullptr;
  const PrimSpec* texture = nullptr;
  const PrimSpec* reader = nullptr;
  for (uint32_t index : material.child_indices()) {
    const auto* shader = layer.prim(index);
    if (shader->type_name() != "Shader" || shader->has_any_time_samples() ||
        !shader->child_indices().empty() || shader->has_relationship_data())
      return false;
    const auto id = Token(*shader, "info:id");
    if (id && *id == "UsdPreviewSurface")
      surface = shader;
    else if (id && *id == "UsdUVTexture") {
      texture = shader;
      result->texture = index;
    } else if (id && *id == "UsdPrimvarReader_float2") {
      reader = shader;
      result->reader = index;
    } else
      return false;
  }
  if (!surface || !texture || !reader ||
      !Connects(material, "outputs:surface", *surface, "outputs:surface") ||
      !Connects(*texture, "inputs:st", *reader, "outputs:result"))
    return false;
  const auto* varname = Property(*reader, "inputs:varname");
  if (!varname) return false;
  auto uv = varname->as_token();
  if (!uv) uv = varname->as_string();
  if (!uv || *uv != "st") return false;
  bool uses_rgb = false;
  for (const auto* prim : {&material, surface, texture, reader}) {
    for (const auto& name : prim->authored_property_names()) {
      if (prim == &material && name != "outputs:surface") return false;
      if (!prim->connection(name)) continue;
      if (prim == &material && name == "outputs:surface") continue;
      if (prim == texture && name == "inputs:st") continue;
      if (prim == surface && name == "inputs:diffuseColor" &&
          Connects(*surface, name.c_str(), *texture, "outputs:rgb")) {
        uses_rgb = true;
        continue;
      }
      // The same diffuse texture may supply opacity through its alpha channel.
      if (prim == surface && name == "inputs:opacity" &&
          Connects(*surface, name.c_str(), *texture, "outputs:a"))
        continue;
      return false;
    }
  }
  if (!uses_rgb) return false;
  for (const char* name : {"inputs:wrapS", "inputs:wrapT"}) {
    const auto wrap = Token(*texture, name);
    if (!wrap || *wrap != "clamp") return false;
  }
  const auto color = Token(*texture, "inputs:sourceColorSpace");
  if (!color || (*color != "sRGB" && *color != "raw")) return false;
  result->srgb = *color == "sRGB";
  const auto* file = Property(*texture, kFile);
  const auto path = file ? file->as_asset_path() : StringView{};
  if (!path || path->empty() || path->find('\0') != std::string::npos ||
      path->find('<') != std::string::npos)
    return false;
  result->file = ResolveAssetAnchorPath(std::string(*path),
                                        texture->asset_anchor_id()
                                            ? texture->asset_anchor_id()
                                            : layer.source_asset_anchor_id());
  return true;
}

bool ReadUV(const PrimSpec& mesh, size_t budget, std::vector<float>* uv) {
  const auto* value = Property(mesh, kUV);
  if (!value || !value->is_array() || !value->array_size() ||
      (value->type_id() != TypeId::Float2 &&
       value->type_id() != TypeId::Texcoord2f) ||
      value->array_size() > budget / (sizeof(float) * 4))
    return false;
  const auto* meta = mesh.property_meta(kUV);
  if (meta && (meta->elementSize != 1 || meta->unauthoredValuesIndex >= 0))
    return false;
  size_t bytes = 0;
  const auto* data = value->view().raw_bytes(&bytes);
  if (!data || bytes != value->array_size() * sizeof(float) * 2) return false;
  uv->resize(value->array_size() * 2);
  std::memcpy(uv->data(), data, bytes);
  return std::all_of(uv->begin(), uv->end(), [](float v) {
    return std::isfinite(v) && v >= 0 && v <= 1;
  });
}

bool CanRemap(const PrimSpec& mesh) {
  if (mesh.type_name() != "Mesh" || !mesh.child_indices().empty() ||
      mesh.has_any_time_samples() || mesh.meta().instanceable ||
      mesh.connection(kUV) || mesh.connection(kUVIndices) ||
      mesh.property(GetPropNameTable().intern(kAtlasUV)) ||
      mesh.property(GetPropNameTable().intern(kAtlasUVIndices)) ||
      mesh.relationship_names().size() != 1)
    return false;
  // Reject named material purposes and other authored binding qualifiers.
  const auto* binding = mesh.relationship("material:binding");
  return binding && binding->size() == 1 &&
         mesh.relationship_edits().find("material:binding") ==
             mesh.relationship_edits().end();
}
}  // namespace

bool BakeMaterialAtlases(Layer& layer, const MaterialAtlasOptions& options,
                         const udim::Fetch& fetch,
                         std::map<std::string, std::vector<uint8_t>>* assets,
                         std::string* warning, std::string* error) {
  const int64_t stride = int64_t(options.tile) + 2 * int64_t(options.padding);
  if (options.edge < 1 || options.edge > 32768 || options.tile < 1 ||
      options.padding < 1 || options.padding > 1024 || options.minimum < 2 ||
      stride > options.edge) {
    if (error)
      *error = "Invalid material atlas size, padding or minimum group size";
    return false;
  }
  std::map<std::string, Material> materials;
  for (size_t i = 0; i < layer.prim_count(); ++i) {
    const auto& prim = *layer.prim(static_cast<uint32_t>(i));
    Material material;
    if (prim.type_name() == "Material" && Describe(layer, prim, &material))
      materials.emplace(prim.path().str(), std::move(material));
  }
  // Audit every relationship and connection to candidate materials, including
  // connections into the shader subtree. Only local graph edges and direct
  // bindings on supported meshes can survive UV remapping.
  for (size_t i = 0; i < layer.prim_count(); ++i) {
    const auto& prim = *layer.prim(static_cast<uint32_t>(i));
    const auto use = [&](const std::string& name, const Path& target,
                         bool relationship) {
      std::string path = target.prim_path().str();
      const std::string exact = path;
      while (!path.empty()) {
        auto candidate = materials.find(path);
        if (candidate != materials.end()) {
          auto& material = candidate->second;
          if (!relationship &&
              (prim.path().str() == path ||
               prim.path().str().compare(0, path.size() + 1, path + "/") == 0))
            return;
          if (relationship && name == "material:binding" && exact == path &&
              target.str() == exact && CanRemap(prim))
            material.meshes.push_back(static_cast<uint32_t>(i));
          else
            material.eligible = false;
          return;
        }
        const auto slash = path.rfind('/');
        if (slash == std::string::npos) return;
        path.resize(slash);
      }
    };
    for (const auto& name : prim.relationship_names()) {
      if (const auto* targets = prim.relationship(name))
        for (const auto& target : *targets) use(name, target, true);
    }
    for (const auto& name : prim.authored_property_names()) {
      if (const auto* targets = prim.connection(name))
        for (const auto& target : *targets) use(name, target, false);
    }
  }
  struct Group {
    Layer normalized;
    std::vector<Material*> materials;
  };
  std::map<std::string, std::vector<Group>> grouped;
  for (auto& pair : materials) {
    auto& material = pair.second;
    if (!material.eligible || material.meshes.empty()) continue;
    std::vector<float> uv;
    if (std::any_of(material.meshes.begin(), material.meshes.end(),
                    [&](uint32_t index) {
                      return !ReadUV(*layer.prim(index), options.memory_budget,
                                     &uv);
                    }))
      continue;
    auto* texture = layer.prim_mutable(material.texture);
    const auto file_id = GetPropNameTable().intern(kFile);
    Value original = *texture->property_value(kFile);
    texture->set_property_value(file_id, Value::MakeAssetPath(""));
    Layer normalized =
        NormalizeMaterialSubtree(layer, layer.index_at_path(pair.first));
    texture->set_property_value(file_id, std::move(original));
    auto& candidates = grouped[WriteLayerToString(normalized)];
    auto equal = std::find_if(
        candidates.begin(), candidates.end(), [&](const Group& group) {
          return EqualNormalizedMaterials(group.normalized, normalized);
        });
    if (equal == candidates.end())
      candidates.push_back({std::move(normalized), {&material}});
    else
      equal->materials.push_back(&material);
  }

  size_t retained = 0;
  for (const auto& asset : *assets) {
    if (asset.second.size() > options.memory_budget - retained) {
      if (error)
        *error = "Existing generated textures exceed the material atlas budget";
      return false;
    }
    retained += asset.second.size();
  }
  const int columns = int(options.edge / stride);
  const size_t capacity = std::min<size_t>(8998, size_t(columns) * columns - 1);
  size_t baked = 0;
  std::vector<const std::vector<Material*>*> groups;
  for (const auto& bucket : grouped)
    for (const auto& group : bucket.second)
      if (group.materials.size() >= size_t(options.minimum))
        groups.push_back(&group.materials);
  for (const auto* group : groups) {
    const auto& entries = *group;
    if (capacity < size_t(options.minimum)) break;
    for (size_t start = 0; start < entries.size(); start += capacity) {
      const size_t count = std::min(capacity, entries.size() - start);
      if (count < size_t(options.minimum)) break;
      udim::Options config;
      config.mode = udim::BakeMode::Dense;
      config.max_tiles = count;
      config.max_atlas_size = options.edge;
      config.dense_padding = options.padding;
      config.memory_budget_bytes = options.memory_budget - retained;
      std::vector<uint32_t> ids;
      for (size_t i = 0; i < count; ++i) ids.push_back(1001 + uint32_t(i));
      udim::AtlasBuilder builder;
      std::string reason;
      bool ok = builder.begin(ids, options.tile, options.tile, config,
                              entries[start]->srgb, &reason);
      std::vector<std::pair<uint32_t, std::vector<float>>> edits;
      size_t uv_bytes = 0;
      for (size_t i = 0; ok && i < count; ++i) {
        auto& material = *entries[start + i];
        const size_t budget = builder.inputBudget();
        std::vector<uint8_t> encoded;
        ok = fetch(material.file, &encoded, &reason, budget) &&
             encoded.size() <= budget &&
             builder.add(ids[i], encoded.data(), encoded.size(), material.file,
                         &reason);
      }
      for (size_t i = 0; ok && i < count; ++i) {
        auto& material = *entries[start + i];
        for (uint32_t index : material.meshes) {
          if (!ok) break;
          std::vector<float> uv;
          ok =
              ReadUV(*layer.prim(index), builder.inputBudget() - uv_bytes, &uv);
          if (!ok) {
            reason = "unsupported or out-of-range UVs";
            break;
          }
          uv_bytes += uv.size() * sizeof(float);
          const float u = float(i % 10), v = float(i / 10);
          for (size_t j = 0; j < uv.size(); j += 2) {
            const auto mapped =
                builder.layout().remap(uv[j] + u, uv[j + 1] + v, ids[i]);
            uv[j] = mapped[0];
            uv[j + 1] = mapped[1];
          }
          edits.emplace_back(index, std::move(uv));
        }
      }
      udim::Atlas atlas;
      if (!ok || !builder.finish("png", 95, &atlas, &reason, uv_bytes)) {
        if (warning)
          *warning += "Material atlas group skipped: " + reason + "\n";
        continue;
      }
      const std::string name =
          "textures/material_atlas_" +
          sha256(reinterpret_cast<const char*>(atlas.bytes.data()),
                 atlas.bytes.size()) +
          "." + atlas.extension;
      if (!assets->count(name)) {
        retained += atlas.bytes.size();
        assets->emplace(name, std::move(atlas.bytes));
      }
      for (auto& edit : edits) {
        auto* mesh = layer.prim_mutable(edit.first);
        const auto type = mesh->property_value(kUV)->type_id();
        const auto uv =
            ValueArrayView<const float>(edit.second.data(), edit.second.size());
        mesh->add_property(GetPropNameTable().intern(kAtlasUV),
                           Value::MakeFloatCompArray(uv, type, 2));
        if (const auto* meta = mesh->property_meta(kUV))
          mesh->set_property_meta(kAtlasUV, PropMeta(*meta));
        if (const auto* indices = mesh->property_value(kUVIndices))
          mesh->add_property(GetPropNameTable().intern(kAtlasUVIndices),
                             *indices);
      }
      for (size_t i = 0; i < count; ++i) {
        auto* texture = layer.prim_mutable(entries[start + i]->texture);
        texture->set_property_value(GetPropNameTable().intern(kFile),
                                    Value::MakeAssetPath(name));
        texture->set_asset_anchor_id(0);
        auto* reader = layer.prim_mutable(entries[start + i]->reader);
        const auto varname = GetPropNameTable().intern("inputs:varname");
        const bool token =
            reader->property_value(varname)->type_id() == TypeId::Token;
        reader->set_property_value(varname, token
                                                ? Value::MakeToken(kAtlasUVName)
                                                : Value(kAtlasUVName));
      }
      baked += count;
    }
  }
  if (baked) DedupeMaterials(&layer, true);
  return true;
}
}  // namespace lightusd::usdz
