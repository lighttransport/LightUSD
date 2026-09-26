"""UsdPhysics: scenes, rigid bodies, colliders, physics materials and joints.

    from miniusd import physics
    physics.add_scene(stage, "/PhysicsScene", gravity_magnitude=9.81)
    physics.add_rigid_body(box, mass=2.0, velocity=(0, 0, 1))
    physics.add_collider(box_mesh, approximation="convexHull")
    physics.set_material(box, static_friction=0.6, dynamic_friction=0.6, restitution=0.2)
    j = physics.add_joint(stage, "/World/Hinge", "revolute", body0=a, body1=b,
                          axis="Z", lower_limit=-45, upper_limit=45)
    physics.add_drive(j, "angular", stiffness=100, damping=10, target_position=30)

Units follow the stage (metersPerUnit / kilogramsPerUnit); angles are degrees.
`describe(stage)` returns the physics setup as plain data.
"""

from .values import ListOp

APPROXIMATIONS = ("none", "convexDecomposition", "convexHull", "boundingSphere", "boundingCube",
                  "meshSimplification")
JOINT_TYPES = {"fixed": "PhysicsFixedJoint", "revolute": "PhysicsRevoluteJoint",
               "prismatic": "PhysicsPrismaticJoint", "spherical": "PhysicsSphericalJoint",
               "distance": "PhysicsDistanceJoint", "generic": "PhysicsJoint"}
DOFS = ("transX", "transY", "transZ", "rotX", "rotY", "rotZ", "linear", "angular", "distance")


def _path(x):
    return getattr(x, "path", x)


def _define(parent, path, type_name):
    if hasattr(parent, "root"):
        return parent.define(path, type_name)
    parts = [x for x in str(path).split("/") if x]
    for part in parts[:-1]:
        parent = parent.children.get(part) or parent.define(part)
    return parent.define(parts[-1], type_name)


def _set(prim, name, type_name, value, uniform=False):
    if value is not None:
        prim.create_attribute(name, type_name, value, uniform=uniform)


def add_scene(stage, path="/PhysicsScene", gravity_direction=None, gravity_magnitude=None):
    """PhysicsScene. Unset gravity means 'down along the stage up axis, earth gravity'."""
    sc = _define(stage, path, "PhysicsScene")
    _set(sc, "physics:gravityDirection", "vector3f", gravity_direction)
    _set(sc, "physics:gravityMagnitude", "float", gravity_magnitude)
    return sc


def add_rigid_body(prim, mass=None, density=None, velocity=None, angular_velocity=None,
                   kinematic=None, enabled=True, center_of_mass=None, diagonal_inertia=None,
                   principal_axes=None, starts_asleep=None, simulation_owner=None):
    """Apply PhysicsRigidBodyAPI (+ PhysicsMassAPI when mass properties are given)."""
    prim.apply_api("PhysicsRigidBodyAPI")
    _set(prim, "physics:rigidBodyEnabled", "bool", enabled)
    _set(prim, "physics:kinematicEnabled", "bool", kinematic)
    _set(prim, "physics:velocity", "vector3f", velocity)
    _set(prim, "physics:angularVelocity", "vector3f", angular_velocity)
    _set(prim, "physics:startsAsleep", "bool", starts_asleep, uniform=True)
    if simulation_owner is not None:
        prim.create_relationship("physics:simulationOwner", [_path(simulation_owner)])
    if any(v is not None for v in (mass, density, center_of_mass, diagonal_inertia, principal_axes)):
        set_mass(prim, mass, density, center_of_mass, diagonal_inertia, principal_axes)
    return prim


def set_mass(prim, mass=None, density=None, center_of_mass=None, diagonal_inertia=None,
             principal_axes=None):
    prim.apply_api("PhysicsMassAPI")
    _set(prim, "physics:mass", "float", mass)
    _set(prim, "physics:density", "float", density)
    _set(prim, "physics:centerOfMass", "point3f", center_of_mass)
    _set(prim, "physics:diagonalInertia", "float3", diagonal_inertia)
    _set(prim, "physics:principalAxes", "quatf", principal_axes)
    return prim


def add_collider(prim, approximation=None, enabled=True, simulation_owner=None):
    """Apply PhysicsCollisionAPI; for meshes also PhysicsMeshCollisionAPI with
    an approximation (one of APPROXIMATIONS)."""
    prim.apply_api("PhysicsCollisionAPI")
    _set(prim, "physics:collisionEnabled", "bool", enabled)
    if simulation_owner is not None:
        prim.create_relationship("physics:simulationOwner", [_path(simulation_owner)])
    if approximation is not None:
        if approximation not in APPROXIMATIONS:
            raise ValueError("approximation must be one of %s" % (APPROXIMATIONS,))
        prim.apply_api("PhysicsMeshCollisionAPI")
        prim.create_attribute("physics:approximation", "token", approximation, uniform=True)
    return prim


def set_material(prim, static_friction=None, dynamic_friction=None, restitution=None, density=None):
    """Apply PhysicsMaterialAPI directly on a prim (Blender's export style) or
    on a Material prim (see add_material + bind_material)."""
    prim.apply_api("PhysicsMaterialAPI")
    _set(prim, "physics:staticFriction", "float", static_friction)
    _set(prim, "physics:dynamicFriction", "float", dynamic_friction)
    _set(prim, "physics:restitution", "float", restitution)
    _set(prim, "physics:density", "float", density)
    return prim


def add_material(stage, path, static_friction=0.5, dynamic_friction=0.5, restitution=0.0, density=None):
    """A Material prim carrying PhysicsMaterialAPI."""
    return set_material(_define(stage, path, "Material"), static_friction, dynamic_friction,
                        restitution, density)


def bind_material(prim, material):
    """Physics-purpose material binding (material:binding:physics)."""
    prim.apply_api("MaterialBindingAPI")
    prim.create_relationship("material:binding:physics", [_path(material)])
    return prim


def add_joint(stage, path, joint_type="fixed", body0=None, body1=None, local_pos0=None,
              local_rot0=None, local_pos1=None, local_rot1=None, axis=None, lower_limit=None,
              upper_limit=None, cone_angle0_limit=None, cone_angle1_limit=None, min_distance=None,
              max_distance=None, break_force=None, break_torque=None, collision_enabled=None,
              enabled=None, exclude_from_articulation=None):
    """Joint prim between two bodies. local_pos*/local_rot* place the joint frame
    in each body's space (rot = quaternion (w, x, y, z)). Limits are degrees for
    revolute / spherical and distance units for prismatic / distance joints."""
    if joint_type not in JOINT_TYPES:
        raise ValueError("joint_type must be one of %s" % (list(JOINT_TYPES),))
    j = _define(stage, path, JOINT_TYPES[joint_type])
    if body0 is not None:
        j.create_relationship("physics:body0", [_path(body0)])
    if body1 is not None:
        j.create_relationship("physics:body1", [_path(body1)])
    _set(j, "physics:localPos0", "point3f", local_pos0)
    _set(j, "physics:localRot0", "quatf", local_rot0)
    _set(j, "physics:localPos1", "point3f", local_pos1)
    _set(j, "physics:localRot1", "quatf", local_rot1)
    if axis is not None:
        j.create_attribute("physics:axis", "token", axis, uniform=True)
    _set(j, "physics:lowerLimit", "float", lower_limit)
    _set(j, "physics:upperLimit", "float", upper_limit)
    _set(j, "physics:coneAngle0Limit", "float", cone_angle0_limit)
    _set(j, "physics:coneAngle1Limit", "float", cone_angle1_limit)
    _set(j, "physics:minDistance", "float", min_distance)
    _set(j, "physics:maxDistance", "float", max_distance)
    _set(j, "physics:breakForce", "float", break_force)
    _set(j, "physics:breakTorque", "float", break_torque)
    _set(j, "physics:collisionEnabled", "bool", collision_enabled)
    _set(j, "physics:jointEnabled", "bool", enabled)
    _set(j, "physics:excludeFromArticulation", "bool", exclude_from_articulation, uniform=True)
    return j


def add_drive(joint, dof="angular", drive_type=None, stiffness=None, damping=None,
              target_position=None, target_velocity=None, max_force=None):
    """Apply PhysicsDriveAPI:<dof> (dof: angular/linear/rotX/.../transZ)."""
    if dof not in DOFS:
        raise ValueError("dof must be one of %s" % (DOFS,))
    joint.apply_api("PhysicsDriveAPI:" + dof)
    pre = "drive:%s:physics:" % dof
    if drive_type is not None:
        joint.create_attribute(pre + "type", "token", drive_type, uniform=True)
    _set(joint, pre + "stiffness", "float", stiffness)
    _set(joint, pre + "damping", "float", damping)
    _set(joint, pre + "targetPosition", "float", target_position)
    _set(joint, pre + "targetVelocity", "float", target_velocity)
    _set(joint, pre + "maxForce", "float", max_force)
    return joint


def add_limit(joint, dof, low=None, high=None):
    """Apply PhysicsLimitAPI:<dof> (low > high locks the axis)."""
    if dof not in DOFS:
        raise ValueError("dof must be one of %s" % (DOFS,))
    joint.apply_api("PhysicsLimitAPI:" + dof)
    _set(joint, "limit:%s:physics:low" % dof, "float", low)
    _set(joint, "limit:%s:physics:high" % dof, "float", high)
    return joint


def set_articulation_root(prim):
    prim.apply_api("PhysicsArticulationRootAPI")
    return prim


def add_collision_group(stage, path, members=(), filtered_groups=(), merge_group=None,
                        invert_filtered_groups=None):
    """PhysicsCollisionGroup; members go into its 'colliders' collection."""
    g = _define(stage, path, "PhysicsCollisionGroup")
    g.apply_api("CollectionAPI:colliders")
    if members:
        g.create_relationship("collection:colliders:includes", [_path(m) for m in members])
    if filtered_groups:
        g.create_relationship("physics:filteredGroups", [_path(m) for m in filtered_groups])
    _set(g, "physics:mergeGroup", "string", merge_group)
    _set(g, "physics:invertFilteredGroups", "bool", invert_filtered_groups)
    return g


def filter_pairs(prim, *others):
    """Disable collisions between `prim` and the given bodies/colliders."""
    prim.apply_api("PhysicsFilteredPairsAPI")
    r = prim.create_relationship("physics:filteredPairs")
    r.set_targets(*(r.get_targets() + [_path(o) for o in others if _path(o) not in r.get_targets()]))
    return prim


# ---------------------------------------------------------------------------
# reading
# ---------------------------------------------------------------------------

def _schemas(prim):
    lo = prim.metadata.get("apiSchemas")
    return lo.items() if isinstance(lo, ListOp) else []


def _props(prim, prefix):
    out = {}
    for name, p in prim.properties.items():
        if name.startswith(prefix):
            key = name[len(prefix):]
            if p.is_attribute:
                v = p.get()
                out[key] = v.tolist() if hasattr(v, "tolist") else v
            else:
                out[key] = p.get_targets()
    return out


def describe(stage):
    """Physics setup as plain data: scenes, bodies, colliders, materials, joints."""
    out = {"scenes": [], "rigid_bodies": [], "colliders": [], "materials": [], "joints": [],
           "collision_groups": []}
    for prim in stage.traverse():
        api = _schemas(prim)
        phys = _props(prim, "physics:")
        if prim.type_name == "PhysicsScene":
            out["scenes"].append({"path": prim.path, **phys})
        if prim.type_name == "PhysicsCollisionGroup":
            out["collision_groups"].append({"path": prim.path, **phys,
                                            "members": _props(prim, "collection:colliders:").get("includes", [])})
        if prim.type_name in JOINT_TYPES.values():
            d = {"path": prim.path, "type": prim.type_name, **phys}
            drives = {}
            for s in api:
                if s.startswith("PhysicsDriveAPI:"):
                    dof = s.split(":", 1)[1]
                    drives[dof] = _props(prim, "drive:%s:physics:" % dof)
                if s.startswith("PhysicsLimitAPI:"):
                    dof = s.split(":", 1)[1]
                    d.setdefault("limits", {})[dof] = _props(prim, "limit:%s:physics:" % dof)
            if drives:
                d["drives"] = drives
            out["joints"].append(d)
        if "PhysicsRigidBodyAPI" in api:
            out["rigid_bodies"].append({"path": prim.path, "type": prim.type_name, **{
                k: v for k, v in phys.items() if k in (
                    "rigidBodyEnabled", "kinematicEnabled", "velocity", "angularVelocity", "mass",
                    "density", "centerOfMass", "diagonalInertia", "principalAxes", "startsAsleep")}})
        if "PhysicsCollisionAPI" in api:
            out["colliders"].append({"path": prim.path, "type": prim.type_name,
                                     "approximation": phys.get("approximation"),
                                     "enabled": phys.get("collisionEnabled", True)})
        if "PhysicsMaterialAPI" in api:
            out["materials"].append({"path": prim.path, **{
                k: v for k, v in phys.items() if k in ("staticFriction", "dynamicFriction",
                                                         "restitution", "density")}})
    return out
