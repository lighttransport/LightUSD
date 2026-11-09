// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// AR/Interactive and Media annotations to JSON Converter Implementation
//

#include "ar-to-json.hh"
#include "materialx-to-json.hh"  // for EscapeJsonString

#include <sstream>
#include <functional>
#include <vector>

#include "core/prim.hh"
#include "stage.hh"
#include "usdAR.hh"
#include "usdMedia.hh"

namespace lightusd {
namespace tydra {

namespace {

std::string Indent(int level, int spaces) {
  return std::string(static_cast<size_t>(level * spaces), ' ');
}

std::string JsonStr(const std::string &s) {
  return "\"" + EscapeJsonString(s) + "\"";
}

std::string JsonBool(bool b) {
  return b ? "true" : "false";
}

template <typename T>
std::string JsonNum(T v) {
  std::ostringstream oss;
  oss << v;
  return oss.str();
}

std::string JsonVec3d(const value::vector3d &v) {
  std::ostringstream oss;
  oss << "[" << v[0] << ", " << v[1] << ", " << v[2] << "]";
  return oss.str();
}

std::string JsonPoint3d(const value::point3d &v) {
  std::ostringstream oss;
  oss << "[" << v[0] << ", " << v[1] << ", " << v[2] << "]";
  return oss.str();
}

void EmitKV(std::ostringstream &ss, int ind, int sp, const std::string &key,
            const std::string &val, bool comma = true) {
  ss << Indent(ind, sp) << "\"" << key << "\": " << val;
  if (comma) ss << ",";
  ss << "\n";
}

// Recursive prim traversal
using PrimVisitor = std::function<void(const Prim &, const std::string &)>;
void TraversePrims(const std::vector<Prim> &prims, const std::string &parent_path,
                   PrimVisitor visitor) {
  for (const auto &prim : prims) {
    std::string path = parent_path + "/" + prim.element_name();
    visitor(prim, path);
    TraversePrims(prim.children(), path, visitor);
  }
}

}  // anonymous namespace

bool ConvertARToJson(
    const Stage &stage,
    std::string *json_str,
    std::string *err,
    const ARJsonExportOptions &options) {

  if (!json_str) {
    if (err) *err = "json_str is null";
    return false;
  }

  int sp = options.indent;

  struct ARData {
    std::vector<std::pair<std::string, const Preliminary_PhysicsGravitationalForce*>> gravForces;
    std::vector<std::pair<std::string, const Preliminary_InfiniteColliderPlane*>> colliderPlanes;
    std::vector<std::pair<std::string, const Preliminary_ReferenceImage*>> refImages;
    std::vector<std::pair<std::string, const Preliminary_Behavior*>> behaviors;
    std::vector<std::pair<std::string, const Preliminary_Trigger*>> triggers;
    std::vector<std::pair<std::string, const Preliminary_Action*>> actions;
    std::vector<std::pair<std::string, const Preliminary_Text*>> texts;
    std::vector<std::pair<std::string, const SpatialAudio*>> audioNodes;
  } data;

  TraversePrims(stage.root_prims(), "", [&](const Prim &prim, const std::string &path) {
    if (auto *grav = prim.as<Preliminary_PhysicsGravitationalForce>()) data.gravForces.emplace_back(path, grav);
    else if (auto *plane = prim.as<Preliminary_InfiniteColliderPlane>()) data.colliderPlanes.emplace_back(path, plane);
    else if (auto *ref_image = prim.as<Preliminary_ReferenceImage>()) data.refImages.emplace_back(path, ref_image);
    else if (auto *behavior = prim.as<Preliminary_Behavior>()) data.behaviors.emplace_back(path, behavior);
    else if (auto *trigger = prim.as<Preliminary_Trigger>()) data.triggers.emplace_back(path, trigger);
    else if (auto *action = prim.as<Preliminary_Action>()) data.actions.emplace_back(path, action);
    else if (auto *text = prim.as<Preliminary_Text>()) data.texts.emplace_back(path, text);
    else if (auto *audio = prim.as<SpatialAudio>()) data.audioNodes.emplace_back(path, audio);
  });

  std::ostringstream ss;
  ss << "{\n";

  // Behaviors
  {
    ss << Indent(1, sp) << "\"behaviors\": [\n";
    for (size_t i = 0; i < data.behaviors.size(); i++) {
      const auto &[path, b] = data.behaviors[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      EmitKV(ss, 3, sp, "exclusive", JsonBool(b->exclusive.get_value()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.behaviors.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "],\n";
  }

  // Triggers
  {
    ss << Indent(1, sp) << "\"triggers\": [\n";
    for (size_t i = 0; i < data.triggers.size(); i++) {
      const auto &[path, t] = data.triggers[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      auto id = t->info_id.get_value();
      if (id) EmitKV(ss, 3, sp, "info:id", JsonStr(id.value().str()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.triggers.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "],\n";
  }

  // Actions
  {
    ss << Indent(1, sp) << "\"actions\": [\n";
    for (size_t i = 0; i < data.actions.size(); i++) {
      const auto &[path, a] = data.actions[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      auto id = a->info_id.get_value();
      if (id) EmitKV(ss, 3, sp, "info:id", JsonStr(id.value().str()));
      EmitKV(ss, 3, sp, "multiplePerformOperation",
             JsonStr(a->multiplePerformOperation.get_value().str()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.actions.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "],\n";
  }

  // Texts
  {
    ss << Indent(1, sp) << "\"texts\": [\n";
    for (size_t i = 0; i < data.texts.size(); i++) {
      const auto &[path, t] = data.texts[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      EmitKV(ss, 3, sp, "content", JsonStr(t->content.get_value()));
      EmitKV(ss, 3, sp, "pointSize", JsonNum(t->pointSize.get_value()));
      EmitKV(ss, 3, sp, "depth", JsonNum(t->depth.get_value()));
      EmitKV(ss, 3, sp, "wrapMode", JsonStr(t->wrapMode.get_value().str()));
      EmitKV(ss, 3, sp, "horizontalAlignment", JsonStr(t->horizontalAlignment.get_value().str()));
      EmitKV(ss, 3, sp, "verticalAlignment", JsonStr(t->verticalAlignment.get_value().str()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.texts.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "],\n";
  }

  // Reference Images
  {
    ss << Indent(1, sp) << "\"referenceImages\": [\n";
    for (size_t i = 0; i < data.refImages.size(); i++) {
      const auto &[path, r] = data.refImages[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      auto img = r->image.get_value();
      if (img) EmitKV(ss, 3, sp, "image", JsonStr(img.value().GetAssetPath()));
      EmitKV(ss, 3, sp, "physicalWidth", JsonNum(r->physicalWidth.get_value()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.refImages.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "],\n";
  }

  // Gravitational Forces
  {
    ss << Indent(1, sp) << "\"gravitationalForces\": [\n";
    for (size_t i = 0; i < data.gravForces.size(); i++) {
      const auto &[path, g] = data.gravForces[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      EmitKV(ss, 3, sp, "acceleration", JsonVec3d(g->acceleration.get_value()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.gravForces.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "],\n";
  }

  // Infinite Collider Planes
  {
    ss << Indent(1, sp) << "\"infiniteColliderPlanes\": [\n";
    for (size_t i = 0; i < data.colliderPlanes.size(); i++) {
      const auto &[path, c] = data.colliderPlanes[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      EmitKV(ss, 3, sp, "position", JsonPoint3d(c->position.get_value()));
      EmitKV(ss, 3, sp, "normal", JsonVec3d(c->normal.get_value()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.colliderPlanes.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "],\n";
  }

  // Spatial Audio
  {
    ss << Indent(1, sp) << "\"spatialAudio\": [\n";
    for (size_t i = 0; i < data.audioNodes.size(); i++) {
      const auto &[path, a] = data.audioNodes[i];
      ss << Indent(2, sp) << "{\n";
      EmitKV(ss, 3, sp, "path", JsonStr(path));
      auto fp = a->filePath.get_value();
      if (fp) EmitKV(ss, 3, sp, "filePath", JsonStr(fp.value().GetAssetPath()));
      EmitKV(ss, 3, sp, "auralMode", JsonStr(a->auralMode.get_value().str()));
      EmitKV(ss, 3, sp, "playbackMode", JsonStr(a->playbackMode.get_value().str()));
      EmitKV(ss, 3, sp, "mediaOffset", JsonNum(a->mediaOffset.get_value()));
      EmitKV(ss, 3, sp, "gain", JsonNum(a->gain.get_value()), false);
      ss << Indent(2, sp) << "}";
      if (i + 1 < data.audioNodes.size()) ss << ",";
      ss << "\n";
    }
    ss << Indent(1, sp) << "]\n";
  }

  ss << "}\n";

  *json_str = ss.str();
  return true;
}

}  // namespace tydra
}  // namespace lightusd
