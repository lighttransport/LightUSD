// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "change-set-internal.hh"
#include "stage.hh"
#include "../diff/layer-diff.hh"
#include <algorithm>
#include <map>

namespace lightusd {
namespace next {
namespace change_set_detail {
namespace {
std::string ChildPath(const std::string& parent, const std::string& child) {
  if (parent.empty() || parent == "/") return "/" + child;
  return parent + "/" + child;
}

StageChangeFlag ClassifyPropertyChange(const Stage& stage,
                                       const std::string& prim_path,
                                       const std::string& property,
                                       const std::vector<std::string>& reasons) {
  StageChangeFlag flags = StageChangeFlag::None;
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  if (property == "visibility" || property == "purpose") {
    flags |= StageChangeFlag::Visibility;
  }
  if (contains(property, "xformOp") || property == "xformOpOrder" ||
      property == "resetXformStack" || property == "positions" ||
      property == "orientations" || property == "scales" ||
      property == "velocities" || property == "angularVelocities") {
    flags |= StageChangeFlag::Transform;
  }
  if (property == "points" || property == "faceVertexCounts" ||
      property == "faceVertexIndices" || property == "curveVertexCounts" ||
      property == "protoIndices" || property == "prototypes" ||
      property == "ids") {
    flags |= StageChangeFlag::Topology;
  }
  if (property == "invisibleIds") flags |= StageChangeFlag::Visibility;
  if (contains(property, "primvars:") || property == "normals" ||
      property == "widths" || property == "displayColor" ||
      property == "displayOpacity") {
    flags |= StageChangeFlag::Primvar;
  }
  if (contains(property, "material:") || contains(property, "inputs:") ||
      contains(property, "outputs:") || contains(property, "surface") ||
      contains(property, "displacement") || contains(property, "volume")) {
    flags |= StageChangeFlag::Material;
  }
  if (contains(property, "file") || contains(property, "texture") ||
      contains(property, "sourceColorSpace")) {
    flags |= StageChangeFlag::Texture;
  }
  if (property == "focalLength" || contains(property, "Aperture") ||
      property == "projection" || property == "clippingRange" ||
      property == "clippingPlanes" || property == "focusDistance" ||
      property == "fStop" || contains(property, "shutter:") ||
      property == "stereoRole") {
    flags |= StageChangeFlag::Camera;
  }
  for (const std::string& reason : reasons) {
    if (contains(reason, "timeSample")) flags |= StageChangeFlag::Animation;
    if (contains(reason, "meta:")) flags |= StageChangeFlag::Metadata;
  }

  const UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (prim) {
    const std::string& type = prim.GetTypeName();
    if (type == "Camera") flags |= StageChangeFlag::Camera;
    if (contains(type, "Light") || type == "DomeLight") {
      flags |= StageChangeFlag::Light;
    }
    if (type == "Material" || type == "Shader" || type == "NodeGraph") {
      flags |= StageChangeFlag::Material;
    }
  }
  if (flags == StageChangeFlag::None) flags = StageChangeFlag::Metadata;
  return flags;
}


}  // namespace

StageChangeSet BuildStageChangeSet(const Stage* previous, const Stage& next,
                                   uint64_t base_revision,
                                   uint64_t new_revision) {
  StageChangeSet out;
  out.base_revision = base_revision;
  out.new_revision = new_revision;
  if (!previous || !previous->GetRootLayer() || !next.GetRootLayer()) {
    out.full_resync = true;
    PrimChange root;
    root.path = Path("/");
    root.flags = StageChangeFlag::Resync;
    out.prims.push_back(std::move(root));
    return out;
  }

  std::unordered_map<std::string, PrimSpecDiff> prim_diffs;
  std::unordered_map<std::string, PropDiff> prop_diffs;
  LayerMetaDiff meta_diff;
  Diff(*previous->GetRootLayer(), *next.GetRootLayer(), prim_diffs,
       prop_diffs, DiffOptions{}, &meta_diff);
  out.stage_metadata_changed = meta_diff.changed();

  std::map<std::string, PrimChange> changes;
  auto resync = [&](const std::string& path) {
    PrimChange& c = changes[path];
    c.path = Path(path);
    c.flags |= StageChangeFlag::Resync;
  };
  for (const auto& item : prim_diffs) {
    for (const std::string& name : item.second.addedPS) {
      resync(ChildPath(item.first, name));
    }
    for (const std::string& name : item.second.deletedPS) {
      resync(ChildPath(item.first, name));
    }
    for (const std::string& name : item.second.modifiedPS) {
      resync(ChildPath(item.first, name));
    }
  }
  for (const auto& item : prop_diffs) {
    PrimChange& c = changes[item.first];
    c.path = Path(item.first);
    auto add_prop = [&](const std::string& name,
                        const std::vector<std::string>& reasons) {
      c.properties.push_back(name);
      c.flags |= ClassifyPropertyChange(next, item.first, name, reasons);
    };
    for (const std::string& name : item.second.addedProps) add_prop(name, {});
    for (const std::string& name : item.second.deletedProps) add_prop(name, {});
    for (const PropDiff::ModifiedProp& detail :
         item.second.modifiedPropDetails) {
      add_prop(detail.name, detail.reasons);
    }
    std::sort(c.properties.begin(), c.properties.end());
    c.properties.erase(
        std::unique(c.properties.begin(), c.properties.end()),
        c.properties.end());
  }
  out.prims.reserve(changes.size());
  for (auto& item : changes) out.prims.push_back(std::move(item.second));
  return out;
}

}  // namespace change_set_detail

}  // namespace next
}  // namespace lightusd
