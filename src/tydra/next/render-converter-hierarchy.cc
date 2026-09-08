// SPDX-License-Identifier: Apache-2.0
#include "render-converter.hh"
#include "next/schema/usd-lux.hh"
#include <string>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::UsdPrim; using ::lightusd::next::Value;
namespace {
bool LocalVisibility(const UsdPrim& prim) {
  const Value* value = prim.GetPropertyValue("visibility");
  if (!value) return true;
  if (const std::string* token = value->as_token()) {
    return *token != "invisible";
  }
  if (const std::string* str = value->as_string()) {
    return *str != "invisible";
  }
  return true;
}
}  // namespace

void RenderSceneConverter::BuildNodeHierarchy(const RenderExtractResult& extracted,
                                              RenderScene* scene) {
  const size_t record_count = extracted.records.size();
  scene->nodes.reserve(record_count);
  scene->node_by_path.reserve(record_count);
  scene->node_by_path.max_load_factor(0.7f);

  for (const RenderPrimRecord& rec : extracted.records) {
    const UsdPrim& prim = rec.prim;
    SceneNode node;
    node.name = prim.GetName();
    node.prim_path = rec.path;

    // Determine node type
    const std::string& type = rec.type_name;
    if (IsMeshRenderableTypeName(type)) node.type = NodeType::Mesh;
    else if (type == "Points") node.type = NodeType::Points;
    else if (type == "BasisCurves" || type == "NurbsCurves" ||
             type == "HermiteCurves") node.type = NodeType::Curves;
    else if (type == "PointInstancer") node.type = NodeType::PointInstancer;
    else if (type == "Xform") node.type = NodeType::Xform;
    else if (type == "Camera") node.type = NodeType::Camera;
    else if (type == "Skeleton") node.type = NodeType::Skeleton;
    else if (::lightusd::tydra::next::IsLight(prim)) {
      LightKind kind = GetLightKind(prim);
      switch (kind) {
        case LightKind::DistantLight: node.type = NodeType::DirectionalLight; break;
        case LightKind::DomeLight: node.type = NodeType::DomeLight; break;
        case LightKind::RectLight: node.type = NodeType::RectLight; break;
        case LightKind::DiskLight: node.type = NodeType::DiskLight; break;
        case LightKind::SphereLight: node.type = NodeType::SphereLight; break;
        case LightKind::PointLight: node.type = NodeType::PointLight; break;
        case LightKind::GeometryLight: node.type = NodeType::PointLight; break;
        case LightKind::PortalLight: node.type = NodeType::RectLight; break;
        case LightKind::PluginLight: node.type = NodeType::PointLight; break;
        case LightKind::LightFilter: node.type = NodeType::PointLight; break;
        case LightKind::PluginLightFilter: node.type = NodeType::PointLight; break;
        case LightKind::Unknown: node.type = NodeType::PointLight; break;
        default: node.type = NodeType::PointLight; break;
      }
    }

    // Compute transforms
    for (int i = 0; i < 16; ++i) {
      node.local_transform.m[i] = static_cast<float>(rec.local[i]);
      node.world_transform.m[i] = static_cast<float>(rec.world[i]);
    }

    int32_t node_id = static_cast<int32_t>(scene->nodes.size());
    scene->node_by_path[node.prim_path] = node_id;

    // Set parent
    std::string parent_path = GetParentPath(node.prim_path);
    bool parent_visible = true;
    if (!parent_path.empty() && parent_path != "/") {
      auto it = scene->node_by_path.find(parent_path);
      if (it != scene->node_by_path.end()) {
        node.parent_id = it->second;
        scene->nodes[it->second].children.push_back(node_id);
        parent_visible = scene->nodes[it->second].visible;
      }
    } else {
      scene->root_nodes.push_back(node_id);
    }

    node.visible = parent_visible && LocalVisibility(prim);

    scene->nodes.push_back(std::move(node));
  }
}
}}}  // namespace lightusd::tydra::next
