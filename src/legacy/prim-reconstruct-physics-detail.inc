// SPDX-License-Identifier: Apache 2.0
// Shared physics-joint reconstruct helpers, #included INSIDE namespace lightusd::prim by
// prim-reconstruct-physics{,3,4}.cc (each instantiates them for its joint types; internal linkage).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
// Helper: check if any property starts with a given prefix
static bool HasPropertyPrefix(const PropertyMap &properties, const std::string &prefix) {
  for (const auto &prop : properties) {
    if (prop.first.compare(0, prefix.size(), prefix) == 0) {
      return true;
    }
  }
  return false;
}

// Helper: populate the multi-apply PhysicsDriveAPI / PhysicsLimitAPI typed maps
// from the namespaced `physics:drive:<dof>:*` / `physics:limit:<dof>:*` props.
//
// The props are LEFT IN `properties` (never added to the consumed `table`), so
// they still round-trip verbatim through the generic stage-converter props pass
// and the USDA printer — this is an additive read-model only. Mirrors the field
// routing in src/next/schema/physics-api.cc and src/usd-validation.cc.
template <typename JointT>
static void PopulateDriveLimitMaps(const PropertyMap &properties, JointT *joint) {
  auto split_dof_field = [](const std::string &rest, std::string *dof,
                            std::string *field) -> bool {
    auto pos = rest.find(':');
    if (pos == std::string::npos) return false;
    *dof = rest.substr(0, pos);
    *field = rest.substr(pos + 1);
    // dof + field are single tokens (e.g. "rotX", "maxForce"); reject deeper
    // namespaces so unrelated keys don't accidentally match.
    return !dof->empty() && !field->empty() &&
           field->find(':') == std::string::npos;
  };

  const std::string drive_pre = "physics:drive:";
  const std::string limit_pre = "physics:limit:";

  for (const auto &prop : properties) {
    const std::string &key = prop.first;
    if (!prop.second.is_attribute()) continue;
    const auto &attr = prop.second.get_attribute();

    if (key.compare(0, drive_pre.size(), drive_pre) == 0) {
      std::string dof, field;
      if (!split_dof_field(key.substr(drive_pre.size()), &dof, &field)) continue;
      PhysicsDriveAPI &d = joint->drives[dof];
      d.dof = dof;
      if (field == "type") {
        if (auto v = attr.get_value<value::token>()) d.type.set_value(v.value());
      } else if (field == "maxForce") {
        if (auto v = attr.get_value<float>()) d.maxForce.set_value(v.value());
      } else if (field == "targetPosition") {
        if (auto v = attr.get_value<float>()) d.targetPosition.set_value(v.value());
      } else if (field == "targetVelocity") {
        if (auto v = attr.get_value<float>()) d.targetVelocity.set_value(v.value());
      } else if (field == "damping") {
        if (auto v = attr.get_value<float>()) d.damping.set_value(v.value());
      } else if (field == "stiffness") {
        if (auto v = attr.get_value<float>()) d.stiffness.set_value(v.value());
      }
    } else if (key.compare(0, limit_pre.size(), limit_pre) == 0) {
      std::string dof, field;
      if (!split_dof_field(key.substr(limit_pre.size()), &dof, &field)) continue;
      PhysicsLimitAPI &l = joint->limits[dof];
      l.dof = dof;
      if (field == "low") {
        if (auto v = attr.get_value<float>()) l.low.set_value(v.value());
      } else if (field == "high") {
        if (auto v = attr.get_value<float>()) l.high.set_value(v.value());
      }
    }
  }
}

// Helper: reconstruct PhysicsJointBase properties (shared by all joint types)
template <typename JointT>
static bool ReconstructJointBaseProperties(
    ParsePropertyTable &table,
    PropertyMap &properties,
    JointT *joint,
    std::string *warn,
    std::string *err,
    const PrimReconstructOptions &options) {

  (void)warn;
  (void)err;
  (void)options;

  // Check for extension properties to auto-create typed API structs. Other
  // vendor namespaces (physxJoint:* / physxLimit:* / state:*) intentionally
  // remain as untyped properties and round-trip through the generic props map.
  if (!joint->mjcJoint.has_value() && HasPropertyPrefix(properties, "mjc:")) {
    joint->mjcJoint = MjcJointAPI();
  }
  if (!joint->newtonMimic.has_value() && HasPropertyPrefix(properties, "newton:")) {
    joint->newtonMimic = NewtonMimicAPI();
  }

  for (auto &prop : properties) {
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, "physics:body0", joint->body0)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, "physics:body1", joint->body1)
  }

  PopulateDriveLimitMaps(properties, joint);

  return true;
}
#pragma GCC diagnostic pop
