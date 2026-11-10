// SPDX-License-Identifier: Apache-2.0
// Legacy Stage bridge over the shared, USD-independent rigid-body solver.
#include "core/safe-arithmetic.hh"
#include "../../tydra/rb-constraints-internal.hh"
#include <cstring>
#include <cmath>
#include <memory>
#include <cstdint>
/* ======================================================================== */
/* PART 2: C++ Bridge                                                       */
/* ======================================================================== */

#ifdef __cplusplus

#include "rb-dynamics.hh"
#include "lightusd.hh"
#include "core/prim.hh"
#include "usdPhysics.hh"
#include "mjcPhysics.hh"
#include "usdGeom.hh"

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

namespace lightusd { namespace legacy {
namespace tydra {

namespace {

/* Recursive prim traversal (same pattern as physics-to-json.cc) */
using PrimVisitor = std::function<void(const Prim &, const std::string &)>;
void TraversePrims(const std::vector<Prim> &prims,
                   const std::string &parent_path,
                   PrimVisitor visitor) {
  for (const auto &prim : prims) {
    std::string path = parent_path + "/" + prim.element_name();
    visitor(prim, path);
    TraversePrims(prim.children(), path, visitor);
  }
}

using MutablePrimVisitor = std::function<void(Prim &, const std::string &)>;
void TraversePrimsMutable(std::vector<Prim> &prims,
                          const std::string &parent_path,
                          MutablePrimVisitor visitor) {
  for (auto &prim : prims) {
    const std::string path = parent_path + "/" + prim.element_name();
    visitor(prim, path);
    TraversePrimsMutable(prim.children(), path, visitor);
  }
}

const Xformable *GetXformable(const Prim &prim) {
  if (const auto *v = prim.as<GeomSphere>()) return v;
  if (const auto *v = prim.as<GeomCube>()) return v;
  if (const auto *v = prim.as<GeomCapsule>()) return v;
  if (const auto *v = prim.as<GeomCylinder>()) return v;
  if (const auto *v = prim.as<Xform>()) return v;
  return nullptr;
}

Xformable *GetXformable(Prim &prim) {
  if (auto *v = prim.get_data().as<GeomSphere>()) return v;
  if (auto *v = prim.get_data().as<GeomCube>()) return v;
  if (auto *v = prim.get_data().as<GeomCapsule>()) return v;
  if (auto *v = prim.get_data().as<GeomCylinder>()) return v;
  if (auto *v = prim.get_data().as<Xform>()) return v;
  return nullptr;
}

/* Check if a prim has a specific API schema in its apiSchemas metadata */
bool HasAPISchema(const Prim &prim, APISchemas::APIName api_name) {
  const PrimMeta &meta = prim.metas();
  if (!meta.has_apiSchemas()) return false;
  const APISchemas &schemas = meta.get_apiSchemas();
  for (const auto &entry : schemas.names) {
    if (entry.first == api_name) return true;
  }
  return false;
}

/* Helper to get a float property from a prim's props map */
bool GetFloatProp(const std::map<std::string, Property> &props,
                  const std::string &name, float *out) {
  auto it = props.find(name);
  if (it == props.end()) return false;

  const Property &prop = it->second;
  if (prop.is_attribute()) {
    const Attribute &attr = prop.get_attribute();
    float fval;
    if (attr.get_value(&fval)) {
      *out = fval;
      return true;
    }
    /* Try double */
    double dval;
    if (attr.get_value(&dval)) {
      *out = static_cast<float>(dval);
      return true;
    }
  }
  return false;
}

/* Relationship target path extraction */
std::string GetRelTargetPath(const RelationshipProperty &rp) {
  if (!rp.authored()) return "";
  const auto &rel = rp.relationship();
  if (rel.is_path()) {
    return rel.targetPath.full_path_name();
  }
  return "";
}

/* Helper to build TydraPhysCollider from geometry prim */
void BuildColliderFromGeom(const Prim &prim,
                           const std::string & /* path */,
                           int32_t body_index,
                           float default_friction,
                           float default_restitution,
                           TydraPhysCollider *col) {
  memset(col, 0, sizeof(TydraPhysCollider));
  col->body_index = body_index;
  col->local_pose = tp_xform_identity();
  col->static_friction = default_friction;
  col->dynamic_friction = default_friction;
  col->restitution = default_restitution;
  col->collision_group = 0xFFFFFFFF;
  col->collision_mask = 0xFFFFFFFF;

  /* Determine shape from prim type */
  if (auto *sphere = prim.as<GeomSphere>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_SPHERE;
    double r = 2.0;
    sphere->radius.get_value().get_scalar(&r);
    col->shape.data.sphere.radius = static_cast<float>(r);
  } else if (auto *cube = prim.as<GeomCube>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_BOX;
    double sz = 2.0;
    cube->size.get_value().get_scalar(&sz);
    float half = static_cast<float>(sz) * 0.5f;
    col->shape.data.box.half_extents = tp_v3(half, half, half);
  } else if (auto *capsule = prim.as<GeomCapsule>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_CAPSULE;
    double r = 0.5, h = 2.0;
    capsule->radius.get_value().get_scalar(&r);
    capsule->height.get_value().get_scalar(&h);
    col->shape.data.capsule.radius = static_cast<float>(r);
    col->shape.data.capsule.half_height = static_cast<float>(h) * 0.5f;
  } else if (auto *cylinder = prim.as<GeomCylinder>()) {
    col->shape.type = TYDRA_PHYS_SHAPE_CYLINDER;
    double r = 1.0, h = 2.0;
    cylinder->radius.get_value().get_scalar(&r);
    cylinder->height.get_value().get_scalar(&h);
    col->shape.data.cylinder.radius = static_cast<float>(r);
    col->shape.data.cylinder.half_height = static_cast<float>(h) * 0.5f;
  } else {
    /* Default to a unit sphere for unsupported types */
    col->shape.type = TYDRA_PHYS_SHAPE_SPHERE;
    col->shape.data.sphere.radius = 1.0f;
  }
}

/* Convert USD axis token to TydraPhysVec3 */
TydraPhysVec3 AxisTokenToVec3(const value::token &tok) {
  std::string s = tok.str();
  if (s == "X") return tp_v3(1, 0, 0);
  if (s == "Y") return tp_v3(0, 1, 0);
  if (s == "Z") return tp_v3(0, 0, 1);
  return tp_v3(0, 1, 0); /* Default Y */
}

/* Build anchor transform from joint's localPos/localRot */
TydraPhysTransform BuildAnchorTransform(
    const TypedAttributeWithFallback<value::point3f> &pos,
    const TypedAttributeWithFallback<value::quatf> &rot) {
  TydraPhysTransform xf = tp_xform_identity();

  if (pos.authored()) {
    value::point3f p = pos.get_value();
    xf.position = tp_v3(p[0], p[1], p[2]);
  }

  if (rot.authored()) {
    value::quatf q = rot.get_value();
    /* value::quatf is (x,y,z,w) or (w,x,y,z)? Check convention. */
    /* lightusd quatf: real part first? Actually
     * value::quatf stores as [x, y, z, w] based on USD convention. */
    xf.rotation = tp_q(q[0], q[1], q[2], q[3]);
  }

  return xf;
}

}  // anonymous namespace

bool BuildPhysWorld(
    const Stage &stage,
    TydraPhysWorld *out_world,
    std::string *err,
    const PhysWorldBuildOptions &options) {

  if (!out_world) {
    if (err) *err = "out_world is null";
    return false;
  }

  if (options.max_bodies <= 0 || options.max_colliders <= 0 ||
      options.max_joints <= 0 || options.max_contacts <= 0 ||
      options.max_pairs <= 0 || options.max_islands <= 0) {
    if (err) *err = "Physics world buffer capacities must be positive";
    return false;
  }

  size_t island_body_count = 0;
  if (!safe::mul(size_t(options.max_bodies), size_t(2), &island_body_count)) {
    if (err) *err = "Integer overflow computing physics island body buffer size";
    return false;
  }

  /* Validate allocation sizes against memory budget and overflow */
  {
    size_t total_bytes = 0;
    size_t part_bytes;

#define RB_MUL_CHECK(n, sizet) \
    do { \
      if (!safe::mul(size_t(n), sizet, &part_bytes)) { \
        if (err) *err = "Integer overflow computing physics buffer allocation size"; \
        return false; \
      } \
      if (!safe::add(total_bytes, part_bytes, &total_bytes)) { \
        if (err) *err = "Integer overflow in physics buffer total allocation size"; \
        return false; \
      } \
    } while (0)

    RB_MUL_CHECK(options.max_bodies, sizeof(TydraPhysBody));
    RB_MUL_CHECK(options.max_colliders, sizeof(TydraPhysCollider));
    RB_MUL_CHECK(options.max_joints, sizeof(TydraPhysJoint));
    RB_MUL_CHECK(options.max_contacts, sizeof(TydraPhysContact));
    RB_MUL_CHECK(options.max_colliders, sizeof(TydraPhysAABB));
    RB_MUL_CHECK(options.max_pairs, sizeof(TydraPhysCollisionPair));
    RB_MUL_CHECK(options.max_colliders, sizeof(int32_t));
    RB_MUL_CHECK(options.max_islands, sizeof(TydraPhysIsland));
    RB_MUL_CHECK(options.max_bodies, sizeof(int32_t));
    RB_MUL_CHECK(island_body_count, sizeof(int32_t));
    RB_MUL_CHECK(options.max_contacts, sizeof(ContactConstraint));

#undef RB_MUL_CHECK

    if (options.max_memory_limit_mb > 0) {
      size_t limit_bytes;
      if (!safe::mul(options.max_memory_limit_mb, size_t(1024 * 1024), &limit_bytes)) {
        if (err) *err = "Integer overflow computing max_memory_limit_mb";
        return false;
      }
      if (total_bytes > limit_bytes) {
        if (err) *err = "Physics world buffer allocation exceeds max_memory_limit_mb";
        return false;
      }
    }
  }

  /* Allocate all buffers — wrapped in unique_ptr for RAII cleanup */
  std::unique_ptr<TydraPhysBody[]> body_buf(new TydraPhysBody[
      static_cast<size_t>(options.max_bodies)]);
  std::unique_ptr<TydraPhysCollider[]> col_buf(new TydraPhysCollider[
      static_cast<size_t>(options.max_colliders)]);
  std::unique_ptr<TydraPhysJoint[]> joint_buf(new TydraPhysJoint[
      static_cast<size_t>(options.max_joints)]);
  std::unique_ptr<TydraPhysContact[]> contact_buf(new TydraPhysContact[
      static_cast<size_t>(options.max_contacts)]);
  std::unique_ptr<TydraPhysAABB[]> aabb_buf(new TydraPhysAABB[
      static_cast<size_t>(options.max_colliders)]);
  std::unique_ptr<TydraPhysCollisionPair[]> pair_buf(new TydraPhysCollisionPair[
      static_cast<size_t>(options.max_pairs)]);
  std::unique_ptr<int32_t[]> sort_buf(new int32_t[
      static_cast<size_t>(options.max_colliders)]);
  std::unique_ptr<TydraPhysIsland[]> island_buf(new TydraPhysIsland[
      static_cast<size_t>(options.max_islands)]);
  std::unique_ptr<int32_t[]> union_buf(new int32_t[
      static_cast<size_t>(options.max_bodies)]);
  /* island_body_buf needs space for counts (max_bodies) + body lists (max_bodies) */
  std::unique_ptr<int32_t[]> island_body_buf(new int32_t[
      island_body_count]);

  tydra_phys_world_init(out_world,
                         body_buf.get(), options.max_bodies,
                         col_buf.get(), options.max_colliders,
                         joint_buf.get(), options.max_joints,
                         contact_buf.get(), options.max_contacts,
                         aabb_buf.get(),
                         pair_buf.get(), options.max_pairs,
                         sort_buf.get(),
                         island_buf.get(), options.max_islands,
                         union_buf.get(),
                         island_body_buf.get());

  /* Per-world contact constraint cache (thread-safe, unlike static fallback) */
  std::unique_ptr<ContactConstraint[]> contact_cache(new ContactConstraint[
      static_cast<size_t>(options.max_contacts)]);

  /* Transfer ownership to out_world — FreePhysWorld will delete[] them */
  body_buf.release();
  col_buf.release();
  joint_buf.release();
  contact_buf.release();
  aabb_buf.release();
  pair_buf.release();
  sort_buf.release();
  island_buf.release();
  union_buf.release();
  island_body_buf.release();
  out_world->contact_cache = contact_cache.release();

  /* Maps for resolving prim paths to body indices */
  std::unordered_map<std::string, int32_t> path_to_body;
  std::vector<std::string> body_paths; /* parallel to body indices */

  /* First pass: find PhysicsScene and configure world */
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string &path) {
      (void)path;
      if (auto *scene = prim.as<PhysicsScene>()) {
        /* Gravity */
        TydraPhysVec3 grav_dir = tp_v3(0, 0, -1);
        float grav_mag = 9.81f;

        auto gd_opt = scene->gravityDirection.get_value();
        if (gd_opt.has_value()) {
          value::vector3f gd = gd_opt.value();
          grav_dir = tp_v3(gd[0], gd[1], gd[2]);
        }
        auto gm_opt = scene->gravityMagnitude.get_value();
        if (gm_opt.has_value()) {
          grav_mag = gm_opt.value();
        }

        out_world->gravity = tp_v3_scale(
            tp_v3_normalize(grav_dir), grav_mag);

        /* MuJoCo scene parameters */
        if (options.use_mjc_params && scene->mjcScene.has_value()) {
          const MjcSceneAPI &mjc = scene->mjcScene.value();
          out_world->timestep = static_cast<float>(
              mjc.timestep.get_value());
          out_world->solver_iterations = mjc.iterations.get_value();
        }
      }
    });

  /* Second pass: create bodies and colliders for prims with RigidBodyAPI */
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string &path) {
      bool has_rb = HasAPISchema(prim, APISchemas::APIName::PhysicsRigidBodyAPI);
      if (!has_rb) return;

      /* Create body */
      TydraPhysBody body;
      tydra_phys_body_default(&body);

      PhysicsRigidBodyAPI rigid_api;
      if (GetPhysicsRigidBodyAPI(prim, &rigid_api)) {
        if (!rigid_api.rigidBodyEnabled.get_value()) {
          body.body_type = TYDRA_PHYS_BODY_STATIC;
          body.flags &=
              ~static_cast<uint32_t>(TYDRA_PHYS_BODY_FLAG_GRAVITY);
        } else if (rigid_api.kinematicEnabled.get_value()) {
          body.body_type = TYDRA_PHYS_BODY_KINEMATIC;
          body.flags &=
              ~static_cast<uint32_t>(TYDRA_PHYS_BODY_FLAG_GRAVITY);
        }
        if (rigid_api.startsAsleep.get_value()) {
          body.flags |= TYDRA_PHYS_BODY_FLAG_SLEEPING;
        }
        if (rigid_api.velocity.authored()) {
          value::vector3f velocity;
          const auto authored = rigid_api.velocity.get_value();
          if (authored.has_value() && authored.value().get_scalar(&velocity)) {
            body.linear_velocity = tp_v3(velocity[0], velocity[1], velocity[2]);
          }
        }
        if (rigid_api.angularVelocity.authored()) {
          value::vector3f velocity;
          const auto authored = rigid_api.angularVelocity.get_value();
          if (authored.has_value() && authored.value().get_scalar(&velocity)) {
            body.angular_velocity = tp_v3(velocity[0], velocity[1], velocity[2]);
          }
        }
      }

      if (const Xformable *xformable = GetXformable(prim)) {
        auto matrix = xformable->GetLocalMatrix();
        if (matrix) {
          value::double3 translation{{0.0, 0.0, 0.0}};
          value::double3 scale{{1.0, 1.0, 1.0}};
          value::quatd rotation{};
          if (decompose(matrix.value(), &translation, &rotation, &scale)) {
            body.xform.position = tp_v3(static_cast<float>(translation[0]),
                                        static_cast<float>(translation[1]),
                                        static_cast<float>(translation[2]));
            body.xform.rotation = tp_q(static_cast<float>(rotation[0]),
                                       static_cast<float>(rotation[1]),
                                       static_cast<float>(rotation[2]),
                                       static_cast<float>(rotation[3]));
          }
        }
      }

      /* Extract mass from props */
      float mass = options.default_mass;

      /* Try to access mass and velocity from the prim's props map.
       * The physics API properties are stored under "physics:" namespace. */
      const std::map<std::string, Property> *props = nullptr;

      /* Check various geometry types for their props map */
      if (auto *sphere = prim.as<GeomSphere>()) {
        props = &sphere->props;
      } else if (auto *cube = prim.as<GeomCube>()) {
        props = &cube->props;
      } else if (auto *capsule = prim.as<GeomCapsule>()) {
        props = &capsule->props;
      } else if (auto *cylinder = prim.as<GeomCylinder>()) {
        props = &cylinder->props;
      } else if (auto *xform = prim.as<Xform>()) {
        props = &xform->props;
      }

      if (props) {
        float m;
        if (GetFloatProp(*props, "physics:mass", &m) && m > 0.0f) {
          mass = m;
        }
      }

      body.inverse_mass = (mass > TP_EPSILON) ? (1.0f / mass) : 0.0f;

      /* Compute diagonal inverse inertia (approximate: uniform sphere) */
      if (mass > TP_EPSILON) {
        float inv = 1.0f / mass;
        /* Simple: I = (2/5)*m*r^2 for sphere; just use 1/mass as default */
        body.local_inv_inertia = tp_v3(inv, inv, inv);
      }

      int32_t body_idx = tydra_phys_add_body(out_world, &body);
      if (body_idx < 0) {
        if (err) *err = "Max bodies exceeded at path: " + path;
        return;
      }

      path_to_body[path] = body_idx;
      body_paths.push_back(path);

      /* Create collider */
      TydraPhysCollider col;
      BuildColliderFromGeom(prim, path, body_idx,
                            options.default_friction,
                            options.default_restitution, &col);

      int32_t col_idx = tydra_phys_add_collider(out_world, &col);
      if (col_idx >= 0) {
        out_world->bodies[body_idx].collider_start = col_idx;
        out_world->bodies[body_idx].collider_count = 1;
      }
    });

  /* Third pass: create joints */
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string & /* path */) {

      /* Helper lambda to build a joint from a joint prim */
      auto buildJoint = [&](const PhysicsJointBase &base,
                            TydraPhysJointType type,
                            TydraPhysJoint *jnt) {
        memset(jnt, 0, sizeof(TydraPhysJoint));
        jnt->type = type;
        jnt->body_a = -1;
        jnt->body_b = -1;

        /* Resolve body0/body1 paths to body indices */
        std::string b0_path = GetRelTargetPath(base.body0);
        std::string b1_path = GetRelTargetPath(base.body1);
        if (!b0_path.empty()) {
          auto it = path_to_body.find(b0_path);
          if (it != path_to_body.end()) jnt->body_a = it->second;
        }
        if (!b1_path.empty()) {
          auto it = path_to_body.find(b1_path);
          if (it != path_to_body.end()) jnt->body_b = it->second;
        }

        /* Anchor transforms */
        jnt->local_anchor_a = BuildAnchorTransform(base.localPos0,
                                                    base.localRot0);
        jnt->local_anchor_b = BuildAnchorTransform(base.localPos1,
                                                    base.localRot1);

        /* MuJoCo joint parameters */
        if (options.use_mjc_params && base.mjcJoint.has_value()) {
          const MjcJointAPI &mjc = base.mjcJoint.value();
          jnt->stiffness = static_cast<float>(mjc.stiffness.get_value());
          jnt->damping = static_cast<float>(mjc.damping.get_value());
        }
      };

      if (auto *rj = prim.as<PhysicsRevoluteJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*rj, TYDRA_PHYS_JOINT_HINGE, &jnt);

        /* Axis */
        auto axis_opt = rj->axis.get_value();
        if (axis_opt.has_value()) {
          jnt.axis = AxisTokenToVec3(axis_opt.value());
        } else {
          jnt.axis = tp_v3(0, 1, 0);
        }

        /* Limits */
        auto lower = rj->lowerLimit.get_value();
        auto upper = rj->upperLimit.get_value();
        if (lower.has_value() || upper.has_value()) {
          jnt.limit_enabled = 1;
          jnt.lower_limit = lower.has_value()
              ? (lower.value() * TP_PI / 180.0f) : -TP_PI;
          jnt.upper_limit = upper.has_value()
              ? (upper.value() * TP_PI / 180.0f) : TP_PI;
        }

        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *pj = prim.as<PhysicsPrismaticJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*pj, TYDRA_PHYS_JOINT_SLIDER, &jnt);

        auto axis_opt = pj->axis.get_value();
        if (axis_opt.has_value()) {
          jnt.axis = AxisTokenToVec3(axis_opt.value());
        } else {
          jnt.axis = tp_v3(0, 1, 0);
        }

        auto lower = pj->lowerLimit.get_value();
        auto upper = pj->upperLimit.get_value();
        if (lower.has_value() || upper.has_value()) {
          jnt.limit_enabled = 1;
          jnt.lower_limit = lower.has_value()
              ? lower.value() : -TP_UNLIMITED;
          jnt.upper_limit = upper.has_value()
              ? upper.value() : TP_UNLIMITED;
        }

        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *sj = prim.as<PhysicsSphericalJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*sj, TYDRA_PHYS_JOINT_BALL, &jnt);

        /* Spherical joints may have cone limits; store them but the ball
         * joint solver doesn't enforce cone limits currently */
        auto cone0 = sj->coneAngle0Limit.get_value();
        auto cone1 = sj->coneAngle1Limit.get_value();
        if (cone0.has_value() || cone1.has_value()) {
          jnt.limit_enabled = 1;
          jnt.lower_limit = cone0.has_value()
              ? (cone0.value() * TP_PI / 180.0f) : TP_PI;
          jnt.upper_limit = cone1.has_value()
              ? (cone1.value() * TP_PI / 180.0f) : TP_PI;
        }

        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *fj = prim.as<PhysicsFixedJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*fj, TYDRA_PHYS_JOINT_FIXED, &jnt);
        tydra_phys_add_joint(out_world, &jnt);

      } else if (auto *dj = prim.as<PhysicsDistanceJoint>()) {
        TydraPhysJoint jnt;
        buildJoint(*dj, TYDRA_PHYS_JOINT_DISTANCE, &jnt);

        auto mind = dj->minDistance.get_value();
        auto maxd = dj->maxDistance.get_value();
        jnt.min_distance = mind.has_value() ? mind.value() : 0.0f;
        jnt.max_distance = maxd.has_value() ? maxd.value() : TP_UNLIMITED;

        tydra_phys_add_joint(out_world, &jnt);
      }
    });

  return true;
}

bool SyncPhysWorldToStage(
    const TydraPhysWorld &world,
    Stage *stage,
    std::string *err) {
  if (!stage) {
    if (err) *err = "stage is null";
    return false;
  }
  int32_t body_index = 0;
  bool overflow = false;
  TraversePrimsMutable(stage->root_prims(), "",
    [&](Prim &prim, const std::string &) {
      if (!HasAPISchema(prim, APISchemas::APIName::PhysicsRigidBodyAPI)) return;
      if (body_index >= world.num_bodies) {
        overflow = true;
        return;
      }
      Xformable *xformable = GetXformable(prim);
      const TydraPhysBody &body = world.bodies[body_index++];
      if (!xformable) return;

      value::double3 scale{{1.0, 1.0, 1.0}};
      auto original = xformable->GetLocalMatrix();
      if (original) {
        value::double3 ignored_translation;
        value::quatd ignored_rotation;
        decompose(original.value(), &ignored_translation, &ignored_rotation, &scale);
      }
      XformOp translate;
      translate.op_type = XformOp::OpType::Translate;
      translate.set_value(value::double3{{
          static_cast<double>(body.xform.position.x),
          static_cast<double>(body.xform.position.y),
          static_cast<double>(body.xform.position.z)}});
      XformOp orient;
      orient.op_type = XformOp::OpType::Orient;
      value::quatd rotation;
      rotation[0] = static_cast<double>(body.xform.rotation.x);
      rotation[1] = static_cast<double>(body.xform.rotation.y);
      rotation[2] = static_cast<double>(body.xform.rotation.z);
      rotation[3] = static_cast<double>(body.xform.rotation.w);
      orient.set_value(rotation);
      xformable->xformOps.clear();
      xformable->xformOps.push_back(std::move(translate));
      xformable->xformOps.push_back(std::move(orient));
      if (std::fabs(scale[0] - 1.0) > 1.0e-12 ||
          std::fabs(scale[1] - 1.0) > 1.0e-12 ||
          std::fabs(scale[2] - 1.0) > 1.0e-12) {
        XformOp scale_op;
        scale_op.op_type = XformOp::OpType::Scale;
        scale_op.set_value(scale);
        xformable->xformOps.push_back(std::move(scale_op));
      }
    });
  if (overflow || body_index != world.num_bodies) {
    if (err) *err = "Rigid-body count no longer matches the source Stage";
    return false;
  }
  return true;
}

bool SyncStageToPhysWorld(const Stage &stage, TydraPhysWorld *world,
                          std::string *err) {
  if (!world) {
    if (err) *err = "world is null";
    return false;
  }
  int32_t body_index = 0;
  bool overflow = false;
  TraversePrims(stage.root_prims(), "",
    [&](const Prim &prim, const std::string &) {
      if (!HasAPISchema(prim, APISchemas::APIName::PhysicsRigidBodyAPI)) return;
      if (body_index >= world->num_bodies) {
        overflow = true;
        return;
      }
      TydraPhysBody &body = world->bodies[body_index++];
      if (body.body_type != TYDRA_PHYS_BODY_KINEMATIC) return;
      const Xformable *xformable = GetXformable(prim);
      if (!xformable) return;
      auto matrix = xformable->GetLocalMatrix();
      if (!matrix) return;
      value::double3 translation, scale;
      value::quatd rotation;
      if (!decompose(matrix.value(), &translation, &rotation, &scale)) return;
      const TydraPhysTransform previous = body.xform;
      const TydraPhysTransform next = {
          tp_v3(static_cast<float>(translation[0]),
                static_cast<float>(translation[1]),
                static_cast<float>(translation[2])),
          tp_q_normalize(tp_q(static_cast<float>(rotation[0]),
                              static_cast<float>(rotation[1]),
                              static_cast<float>(rotation[2]),
                              static_cast<float>(rotation[3])))};
      const float inverse_dt = world->timestep > TP_EPSILON
                                   ? 1.0f / world->timestep
                                   : 0.0f;
      body.linear_velocity = tp_v3_scale(
          tp_v3_sub(next.position, previous.position), inverse_dt);
      const TydraPhysQuat delta = tp_q_mul(
          next.rotation, tp_q_conjugate(previous.rotation));
      TydraPhysVec3 axis;
      const float angle = tp_q_to_axis_angle(delta, &axis);
      body.angular_velocity = tp_v3_scale(axis, angle * inverse_dt);
      body.xform = next;
    });
  if (overflow || body_index != world->num_bodies) {
    if (err) *err = "Rigid-body count no longer matches the source Stage";
    return false;
  }
  return true;
}

void FreePhysWorld(TydraPhysWorld *world) {
  if (!world) return;

  delete[] world->bodies;
  delete[] world->colliders;
  delete[] world->joints;
  delete[] world->contacts;
  delete[] world->broadphase.aabbs;
  delete[] world->broadphase.pairs;
  delete[] world->broadphase.sorted_indices;
  delete[] world->islands;
  delete[] world->island_union;
  delete[] world->island_body_buf;
  delete[] static_cast<ContactConstraint *>(world->contact_cache);

  memset(world, 0, sizeof(TydraPhysWorld));
}

}  // namespace tydra
}  // namespace legacy
}  // namespace lightusd

#endif /* __cplusplus */
