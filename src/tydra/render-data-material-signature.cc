// SPDX-License-Identifier: Apache-2.0
// Material-source signature generation split from render-data-material.cc.
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common-macros.inc"
#include "core/prim.hh"
#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "usdShade.hh"
#include "value-pprint.hh"

namespace lightusd {

namespace tydra {

static std::string NormalizeSourceSignaturePath(const Path &path,
                                                const std::string &root_path) {
  std::string prim = path.prim_part();
  if (!root_path.empty() && prim.compare(0, root_path.size(), root_path) == 0) {
    prim = std::string("<M>") + prim.substr(root_path.size());
  }
  std::string result = prim;
  if (!path.prop_part().empty()) {
    result += ".";
    result += path.prop_part();
  }
  return result;
}

static void ReplaceAll(std::string &s, const std::string &from,
                       const std::string &to) {
  if (from.empty()) {
    return;
  }
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

static void AppendSourceSignatureProperty(std::ostringstream &ss,
                                          const std::string &name,
                                          const Property &prop,
                                          const std::string &root_path) {
  ss << "prop:" << name << ":custom=" << (prop.has_custom() ? 1 : 0) << ":";
  if (prop.is_attribute()) {
    const Attribute &attr = prop.get_attribute();
    ss << "attr:type=" << attr.type_name()
       << ":var=" << int(attr.variability())
       << ":varying=" << (attr.is_varying_authored() ? 1 : 0);
    if (attr.has_value()) {
      ss << ":value=" << value::pprint_value(attr.get_var().value_raw());
    }
    if (attr.has_connections()) {
      ss << ":connections=[";
      const std::vector<Path> &conns = attr.connections();
      for (size_t i = 0; i < conns.size(); i++) {
        if (i) {
          ss << ",";
        }
        ss << NormalizeSourceSignaturePath(conns[i], root_path);
      }
      ss << "]";
    }
    if (attr.has_timesamples()) {
      // BuildMaterialSourceSignature rejects time-sampled material graphs.
      ss << ":timesamples=1";
    }
  } else if (prop.is_relationship()) {
    const Relationship &rel = prop.get_relationship();
    ss << "rel:blocked=" << (rel.is_blocked() ? 1 : 0) << ":targets=[";
    std::vector<Path> targets;
    if (rel.is_path()) {
      targets.push_back(rel.targetPath);
    } else if (rel.is_pathvector()) {
      targets = rel.targetPathVector;
    }
    for (size_t i = 0; i < targets.size(); i++) {
      if (i) {
        ss << ",";
      }
      ss << NormalizeSourceSignaturePath(targets[i], root_path);
    }
    ss << "]";
  } else {
    ss << "empty";
  }
  ss << ";";
}

static const std::map<std::string, Property> *GetUsdShadePrimProps(
    const Prim &prim) {
  if (const Material *p = prim.as<Material>()) {
    return &p->props;
  }
  if (const Shader *p = prim.as<Shader>()) {
    return &p->props;
  }
  if (const NodeGraph *p = prim.as<NodeGraph>()) {
    return &p->props;
  }
  return nullptr;
}

static bool AppendSourceSignaturePrimRec(const Stage &stage, const Prim &prim,
                                         const Path &prim_path,
                                         const std::string &root_path,
                                         std::set<std::string> *visited,
                                         std::ostringstream &ss,
                                         int depth) {
  if (!visited || depth >= 64) {
    return false;
  }

  const std::string path_key = prim_path.prim_part();
  if (!visited->insert(path_key).second) {
    return true;
  }

  ss << "prim:" << NormalizeSourceSignaturePath(prim_path, root_path)
     << ":type=" << prim.prim_type_name() << "{";

  std::string prim_payload = value::pprint_prim_value(
      prim.data(), 0, /* closing_brace */ false);
  if (prim_payload.find("timeSamples") != std::string::npos) {
    return false;
  }
  ReplaceAll(prim_payload, root_path, "<M>");
  if (prim_path.prim_part() == root_path) {
    ReplaceAll(prim_payload, std::string(prim.element_name()), "<Material>");
  }
  ss << "payload=" << prim_payload << ";";

  const std::map<std::string, Property> *props = GetUsdShadePrimProps(prim);
  if (!props) {
    ss << "}";
    return true;
  }

  std::vector<std::string> prop_names;
  prop_names.reserve(props->size());
  for (const auto &kv : *props) {
    prop_names.push_back(kv.first);
  }
  std::sort(prop_names.begin(), prop_names.end());

  std::vector<Path> local_connections;
  for (const std::string &name : prop_names) {
    auto it = props->find(name);
    if (it == props->end()) {
      continue;
    }
    AppendSourceSignatureProperty(ss, name, it->second, root_path);
    if (it->second.is_attribute()) {
      const Attribute &attr = it->second.get_attribute();
      if (attr.has_timesamples()) {
        return false;
      }
      if (attr.has_connections()) {
        for (const Path &conn : attr.connections()) {
          const std::string conn_prim = conn.prim_part();
          if (!root_path.empty() &&
              conn_prim.compare(0, root_path.size(), root_path) == 0) {
            local_connections.push_back(Path(conn_prim, ""));
          }
        }
      }
    }
  }

  std::vector<const Prim *> children;
  children.reserve(prim.children().size());
  for (const Prim &child : prim.children()) {
    children.push_back(&child);
  }
  std::sort(children.begin(), children.end(),
            [](const Prim *a, const Prim *b) {
              return a->element_name() < b->element_name();
            });
  for (const Prim *child : children) {
    if (!child) {
      continue;
    }
    Path child_path = prim_path;
    child_path = child_path.append_element(child->element_name());
    if (!AppendSourceSignaturePrimRec(stage, *child, child_path, root_path,
                                      visited, ss, depth + 1)) {
      return false;
    }
  }

  std::sort(local_connections.begin(), local_connections.end(),
            [](const Path &a, const Path &b) {
              return a.full_path_name() < b.full_path_name();
            });
  local_connections.erase(
      std::unique(local_connections.begin(), local_connections.end(),
                  [](const Path &a, const Path &b) {
                    return a.full_path_name() == b.full_path_name();
                  }),
      local_connections.end());
  for (const Path &conn_prim_path : local_connections) {
    const Prim *conn_prim{nullptr};
    std::string err;
    if (stage.find_prim_at_path(conn_prim_path, conn_prim, &err) &&
        conn_prim) {
      if (!AppendSourceSignaturePrimRec(stage, *conn_prim, conn_prim_path,
                                        root_path, visited, ss, depth + 1)) {
        return false;
      }
    }
  }

  ss << "}";
  return true;
}

bool RenderSceneConverter::BuildMaterialSourceSignature(
    const RenderSceneConverterEnv &env, const Path &mat_abs_path,
    const Material &material, std::string *signature) {
  if (!signature) {
    return false;
  }
  signature->clear();

  const Prim *mat_prim{nullptr};
  std::string err;
  if (!env.stage.find_prim_at_path(Path(mat_abs_path.prim_part(), ""),
                                   mat_prim, &err) ||
      !mat_prim) {
    return false;
  }

  std::ostringstream ss;
  ss << "material-source-v1;";
  ss << "surface=";
  for (const Path &p : material.surface.get_connections()) {
    ss << NormalizeSourceSignaturePath(p, mat_abs_path.prim_part()) << ",";
  }
  ss << ";mtlxSurface=";
  for (const Path &p : material.mtlxSurface.get_connections()) {
    ss << NormalizeSourceSignaturePath(p, mat_abs_path.prim_part()) << ",";
  }
  ss << ";displacement=";
  for (const Path &p : material.displacement.get_connections()) {
    ss << NormalizeSourceSignaturePath(p, mat_abs_path.prim_part()) << ",";
  }
  ss << ";volume=";
  for (const Path &p : material.volume.get_connections()) {
    ss << NormalizeSourceSignaturePath(p, mat_abs_path.prim_part()) << ",";
  }
  ss << ";";

  std::set<std::string> visited;
  if (!AppendSourceSignaturePrimRec(env.stage, *mat_prim,
                                    Path(mat_abs_path.prim_part(), ""),
                                    mat_abs_path.prim_part(), &visited, ss,
                                    0)) {
    return false;
  }

  *signature = ss.str();
  return true;
}

bool RenderSceneConverter::FindMaterialSourceSignature(
    const std::string &signature, int64_t *material_id) const {
  auto it = _materialSourceSignatureCache.find(signature);
  if (it == _materialSourceSignatureCache.end()) {
    return false;
  }
  if (material_id) {
    *material_id = it->second;
  }
  return true;
}

void RenderSceneConverter::RememberMaterialSourceSignature(
    const std::string &signature, int64_t material_id) {
  _materialSourceSignatureCache.emplace(signature, material_id);
}

bool RenderSceneConverter::GetOrCreateDefaultMaterial(
    const RenderSceneConverterEnv &env, int *material_id) {
  if (!material_id) {
    PUSH_ERROR_AND_RETURN("material_id argument is nullptr.");
  }
  *material_id = -1;

  if (!env.material_config.assign_default_material) {
    return true;
  }

  constexpr const char *kDefaultMaterialPath =
      "/__lightusd_default_material__";
  if (auto it = materialMap.find(kDefaultMaterialPath);
      it != materialMap.s_end()) {
    *material_id = int(it->second);
    return true;
  }

  RenderMaterial material;
  material.name = env.material_config.default_material_name.empty()
                      ? "defaultMaterial"
                      : env.material_config.default_material_name;
  material.abs_path = kDefaultMaterialPath;
  material.display_name = material.name;

  PreviewSurfaceShader shader;
  shader.diffuseColor.value = env.material_config.default_material_diffuse_color;
  shader.roughness.value = env.material_config.default_material_roughness;
  shader.metallic.value = env.material_config.default_material_metallic;
  shader.opacity.value = env.material_config.default_material_opacity;
  material.surfaceShader = shader;
  material.computeMaterialTag();

  const int id = int(materials.size());
  materials.emplace_back(std::move(material));
  materialMap.add(kDefaultMaterialPath, uint64_t(id));
  *material_id = id;

  if (!EmitMaterial(size_t(id), kDefaultMaterialPath)) {
    PUSH_ERROR_AND_RETURN("Conversion cancelled by user.");
  }
  return true;
}

}  // namespace tydra
}  // namespace lightusd
