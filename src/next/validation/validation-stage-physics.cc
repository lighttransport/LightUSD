// SPDX-License-Identifier: Apache-2.0
#include "validation-context.hh"
#include "usd-validation-internal.hh"
#include "../eval/xform-eval.hh"
#include <algorithm>
#include <cmath>
#include <limits>

namespace lightusd { namespace next {
namespace {
using namespace validation_detail;
bool Api(const PrimSpec& p, const std::string& name) {
  for (const auto& api : p.meta().apiSchemas()) if (api == name) return true;
  return false;
}
bool Boolean(const PrimSpec& p, const std::string& name, bool fallback) {
  const Value* value = p.property_value(name);
  return value && value->type_id() == TypeId::Bool ? *value->as_bool() : fallback;
}
std::string Parent(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}
}
void ValidateRegisteredPhysics(const Layer& layer, const ValidationOptions& options,
                               USDValidationResult* result) {
  if (!options.physics) return;
  const auto& registry = options.registry ? *options.registry : GetBuiltinValidationRegistry();
  Stage stage;
  stage.SetRootLayer(layer.Clone());
  for (const auto& p : layer.prims()) {
    const bool body = Api(p, "PhysicsRigidBodyAPI");
    const bool collider = Api(p, "PhysicsCollisionAPI") && registry.InheritsFrom(p.type_name(), "Gprim");
    const bool articulation = Api(p, "PhysicsArticulationRootAPI");
    const auto error = [&](const char* rule, const std::string& message) {
      AddError(result, rule, p.path().str(), message);
    };
    if (body && !registry.InheritsFrom(p.type_name(), "Xformable"))
      error("physics.rigidBody.xformable", "RigidBodyAPI requires an Xformable prim");
    if (body && Boolean(p, "physics:rigidBodyEnabled", true) && !Boolean(p, "physics:kinematicEnabled", false)) {
      for (std::string path = Parent(p.path().str()); path != "/"; path = Parent(path)) {
        const auto* parent = layer.prim_at_path(path);
        if (parent && parent->meta().instanceable) {
          error("physics.rigidBody.instanceProxy", "Enabled dynamic rigid body is an instance-proxy descendant");
          break;
        }
      }
    }
    if (articulation && body && !Boolean(p, "physics:rigidBodyEnabled", true))
      error("physics.articulation.staticBody", "Articulation root cannot be a disabled rigid body");
    if (body || collider) {
      double matrix[16];
      if (ComputeWorldTransform(stage, stage.GetPrimAtPath(p.path()), matrix,
                                std::numeric_limits<double>::quiet_NaN())) {
        // A*A^T is diagonal exactly when scale has no nontrivial orientation.
        // Uniform scale is a scalar multiple of identity, independent of rotation.
        double gram[3][3] = {};
        for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j)
          for (size_t k = 0; k < 3; ++k) gram[i][j] += matrix[i*4+k] * matrix[j*4+k];
        const double scale = std::max({std::abs(gram[0][0]), std::abs(gram[1][1]), std::abs(gram[2][2]), 1e-30});
        const bool oriented = std::abs(gram[0][1]) > 1e-5 * scale ||
                              std::abs(gram[0][2]) > 1e-5 * scale ||
                              std::abs(gram[1][2]) > 1e-5 * scale;
        const bool uniform = !oriented && std::abs(gram[0][0] - gram[1][1]) <= 2e-5 * scale &&
                             std::abs(gram[0][0] - gram[2][2]) <= 2e-5 * scale;
        if (body && oriented)
          error("physics.rigidBody.scaleOrientation", "Rigid bodies do not support oriented nonuniform scale");
        if (collider && !uniform) for (const char* type : {"Sphere", "Capsule", "Capsule_1", "Cylinder", "Cylinder_1", "Cone", "Points"})
          if (registry.InheritsFrom(p.type_name(), type)) {
            error("physics.collider.nonUniformScale", "Collider requires uniform world scale"); break;
          }
      }
      if (Api(p, "PhysicsMassAPI")) {
        double value = 0;
        if (GetNumericScalarProperty(p, "physics:mass", &value) && value < 0)
          error("physics.mass", "Mass cannot be negative");
        if (GetNumericScalarProperty(p, "physics:density", &value) && value < 0)
          error("physics.density", "Density cannot be negative");
        const Value* axes = p.property_value("physics:principalAxes");
        const Value* inertia = p.property_value("physics:diagonalInertia");
        if (bool(axes) != bool(inertia))
          error("physics.inertia", "principalAxes and diagonalInertia must both be authored or neither");
        if (axes && inertia) {
          std::array<double, 4> q{}; std::array<double, 3> diagonal{};
          if (ValueToQuat(*axes, &q) && ValueToVec3(*inertia, &diagonal)) {
            const double norm = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
            const bool zero = diagonal[0] == 0 && diagonal[1] == 0 && diagonal[2] == 0;
            if ((norm == 0) != zero || (norm != 0 &&
                (std::abs(std::sqrt(norm) - 1) > 1e-5 ||
                 diagonal[0] <= 0 || diagonal[1] <= 0 || diagonal[2] <= 0)))
              error("physics.inertia", "Inertia requires paired fallback values or unit axes and positive diagonal values");
          }
        }
      }
    }
    if (collider && registry.InheritsFrom(p.type_name(), "Points")) {
      size_t points = 0, widths = 0;
      if (!GetPoint3ArrayLen(p, "points", &points) || !GetArrayLengthProperty(p, "widths", &widths) ||
          points == 0 || widths != points)
        error("physics.collider.points", "Points collider needs nonempty matching widths and positions");
    }
    if (registry.InheritsFrom(p.type_name(), "PhysicsJoint")) {
      for (const char* rel : {"physics:body0", "physics:body1"}) {
        const auto* targets = p.relationship(rel);
        if (!targets) continue;
        if (targets->size() > 1)
          error("physics.joint.bodyCount", "Joint body relationship must have at most one target");
        for (const auto& target : *targets)
          if (!layer.prim_at_path(target.str()))
            error("physics.joint.bodyTarget", "Joint body target does not resolve to a prim: " + target.str());
      }
    }
  }
}
} }
