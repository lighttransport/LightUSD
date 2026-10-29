// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
// Output profiles and generated schema descriptors for URDF/MJCF authoring.
#include "urdf-to-usd-internal.hh"
#include "next/c/lightusd-core-c.h"
#include <cstring>
#include <map>
#include <unordered_map>

namespace lightusd { namespace tydra { namespace next {
namespace urdf_detail {
namespace {
thread_local const Profile *active_profile = nullptr;

const Profile &LegacyProfile() {
  static const Profile profile = [] {
    Profile p;
    p.name = "legacy";
    return p;
  }();
  return profile;
}

std::string Text(lu_view v) { return std::string(v.data, v.len); }

struct Descriptors {
  std::unordered_map<std::string, SchemaProperty> single;
  // multiple-apply namespace ("limit", "drive") -> local property -> definition
  std::map<std::string, std::unordered_map<std::string, SchemaProperty>> multiple;
};

const Descriptors &PhysicsDescriptors() {
  static const Descriptors table = [] {
    Descriptors d;
    size_t count = 0;
    if (lu_validation_schema_count(&count) != LU_OK) return d;
    for (size_t i = 0; i < count; ++i) {
      lu_validation_schema_view schema{};
      if (lu_validation_schema_at(i, &schema) != LU_OK) continue;
      const std::string name = Text(schema.name);
      if (name.compare(0, 7, "Physics") != 0 && name.compare(0, 3, "Mjc") != 0 &&
          name.compare(0, 6, "Newton") != 0)
        continue;
      const bool multiple = Text(schema.kind) == "multipleApply";
      auto &target = multiple ? d.multiple[Text(schema.property_namespace)] : d.single;
      for (size_t p = 0; p < schema.property_count; ++p) {
        lu_validation_property_view property{};
        if (lu_validation_property_at(i, p, &property) != LU_OK) continue;
        SchemaProperty definition;
        definition.type = Text(property.type);
        definition.uniform = Text(property.variability) == "uniform";
        definition.relationship = property.is_relationship != 0;
        // Several engine schemas share a property (mjc:group, newton:kp); the
        // generated rows agree on type and variability, so first wins.
        target.emplace(Text(property.name), std::move(definition));
      }
    }
    return d;
  }();
  return table;
}
}  // namespace

bool ParseProfile(const std::string &name, Profile *out) {
  Profile p;
  if (name.empty() || name == "legacy") {
    p.name = "legacy";
  } else if (name == "newton") {
    p.name = "newton";
    p.legacy = false;
    p.newton = true;
  } else if (name == "openusd") {
    p.name = "openusd";
    p.legacy = false;
    p.engine = false;
  } else {
    return false;
  }
  *out = p;
  return true;
}

const Profile &ActiveProfile() {
  return active_profile ? *active_profile : LegacyProfile();
}

ProfileScope::ProfileScope(const Profile &profile) : previous_(active_profile) {
  active_profile = &profile;
}
ProfileScope::~ProfileScope() { active_profile = previous_; }

const SchemaProperty *FindSchemaProperty(const std::string &name) {
  const Descriptors &d = PhysicsDescriptors();
  auto single = d.single.find(name);
  if (single != d.single.end()) return &single->second;
  // <namespace>:<instance>:<local>, e.g. limit:rotX:physics:low
  const size_t first = name.find(':');
  if (first == std::string::npos) return nullptr;
  auto ns = d.multiple.find(name.substr(0, first));
  if (ns == d.multiple.end()) return nullptr;
  const size_t second = name.find(':', first + 1);
  if (second == std::string::npos) return nullptr;
  auto local = ns->second.find(name.substr(second + 1));
  return local == ns->second.end() ? nullptr : &local->second;
}

bool IsEngineName(const std::string &name) {
  return name.compare(0, 4, "mjc:") == 0 || name.compare(0, 7, "newton:") == 0 ||
         name.compare(0, 5, "physx") == 0 || name.compare(0, 5, "urdf:") == 0 ||
         name.compare(0, 9, "lightusd:") == 0;
}

bool IsEngineSchema(const std::string &schema) {
  return schema.compare(0, 3, "Mjc") == 0 || schema.compare(0, 6, "Newton") == 0 ||
         schema.compare(0, 5, "Physx") == 0;
}

}  // namespace urdf_detail
} } }  // namespace lightusd::tydra::next
