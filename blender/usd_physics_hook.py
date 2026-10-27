# SPDX-License-Identifier: Apache-2.0
# USD Physics Export/Import Hook for Blender
#
# Exports Blender rigid body properties to standard UsdPhysics schemas and
# optionally to MuJoCo (mjcPhysics) custom attributes.  On import, reads
# those attributes back into Blender rigid body / constraint settings.
#
# Install:  Edit > Preferences > Add-ons > Install from Disk
# Requires: Blender 4.0+ with bundled pxr (OpenUSD) Python modules.

bl_info = {
    "name": "USD Physics Hook (UsdPhysics + MuJoCo)",
    "author": "LightUSD Contributors",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "File > Export/Import USD",
    "description": "Export/import Blender rigid body physics as UsdPhysics "
                   "and MuJoCo mjcPhysics attributes",
    "category": "Import-Export",
}

import bpy
import math
import logging

log = logging.getLogger(__name__)

try:
    from pxr import Usd, UsdGeom, UsdPhysics, Sdf, Gf
    _HAS_PXR = True
except ImportError:
    _HAS_PXR = False
    log.warning("[USDPhysicsHook] pxr modules not available")


# ---------------------------------------------------------------------------
# Preferences — flag to control USD / MuJoCo / both
# ---------------------------------------------------------------------------

class USDPhysicsHookPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    physics_mode: bpy.props.EnumProperty(
        name="Physics Schema",
        description="Which physics schemas to export/import",
        items=[
            ('USD', "USD Physics Only",
             "Standard UsdPhysics schemas (RigidBodyAPI, CollisionAPI, joints)"),
            ('MJC', "MuJoCo Only",
             "MuJoCo mjcPhysics custom attributes only"),
            ('BOTH', "USD + MuJoCo",
             "Both standard UsdPhysics and MuJoCo mjcPhysics attributes"),
        ],
        default='BOTH',
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "physics_mode")


def _get_mode():
    """Return current physics_mode from addon preferences."""
    prefs = bpy.context.preferences.addons.get(__name__)
    if prefs:
        return prefs.preferences.physics_mode
    return 'BOTH'


# ---------------------------------------------------------------------------
# Export helpers
# ---------------------------------------------------------------------------

def _apply_rigid_body_api(prim, rb, mode):
    """Apply RigidBodyAPI and CollisionAPI to a prim from Blender rigid body."""

    if mode in ('USD', 'BOTH'):
        # RigidBodyAPI
        rb_api = UsdPhysics.RigidBodyAPI.Apply(prim)
        if rb.type == 'ACTIVE':
            rb_api.CreateRigidBodyEnabledAttr(True)
        else:
            rb_api.CreateRigidBodyEnabledAttr(False)
            rb_api.CreateKinematicEnabledAttr(rb.kinematic)

        # MassAPI
        mass_api = UsdPhysics.MassAPI.Apply(prim)
        mass_api.CreateMassAttr(rb.mass)

        # CollisionAPI
        UsdPhysics.CollisionAPI.Apply(prim)

        # MaterialAPI — friction / restitution
        mat_api = UsdPhysics.MaterialAPI.Apply(prim)
        mat_api.CreateStaticFrictionAttr(rb.friction)
        mat_api.CreateDynamicFrictionAttr(rb.friction)
        mat_api.CreateRestitutionAttr(rb.restitution)

    if mode in ('MJC', 'BOTH'):
        # MuJoCo collision parameters as custom attributes
        prim.CreateAttribute("mjc:condim",
                             Sdf.ValueTypeNames.Int).Set(3)
        prim.CreateAttribute("mjc:friction",
                             Sdf.ValueTypeNames.Double).Set(float(rb.friction))
        prim.CreateAttribute("mjc:solmix",
                             Sdf.ValueTypeNames.Double).Set(1.0)
        prim.CreateAttribute("mjc:margin",
                             Sdf.ValueTypeNames.Double).Set(0.0)


def _apply_scene_physics(stage, scene, mode):
    """Create a PhysicsScene prim with gravity settings."""

    scene_path = "/PhysicsScene"
    scene_prim = stage.DefinePrim(scene_path, "PhysicsScene")

    grav = scene.gravity
    mag = math.sqrt(grav[0]**2 + grav[1]**2 + grav[2]**2)

    if mode in ('USD', 'BOTH'):
        phys_scene = UsdPhysics.Scene(scene_prim)
        if mag > 1e-6:
            direction = Gf.Vec3d(grav[0]/mag, grav[1]/mag, grav[2]/mag)
            phys_scene.CreateGravityDirectionAttr(direction)
        phys_scene.CreateGravityMagnitudeAttr(mag)

    if mode in ('MJC', 'BOTH'):
        rbw = scene.rigidbody_world
        if rbw:
            scene_prim.CreateAttribute(
                "mjc:option:timestep", Sdf.ValueTypeNames.Double
            ).Set(float(scene.render.fps_base) / scene.render.fps)
            scene_prim.CreateAttribute(
                "mjc:option:iterations", Sdf.ValueTypeNames.Int
            ).Set(rbw.solver_iterations)
            scene_prim.CreateAttribute(
                "mjc:flag:gravity", Sdf.ValueTypeNames.Bool
            ).Set(mag > 1e-6)


def _blender_shape_to_usd(shape_enum):
    """Map Blender collision_shape to best-fit USD type name."""
    mapping = {
        'BOX': 'Cube',
        'SPHERE': 'Sphere',
        'CAPSULE': 'Capsule',
        'CYLINDER': 'Cylinder',
        'CONE': 'Cone',
        'MESH': 'Mesh',
        'CONVEX_HULL': 'Mesh',
    }
    return mapping.get(shape_enum, 'Mesh')


def _export_collision_groups(stage, objects):
    """Export pair filters for Blender's 20-bit shared collision collections."""
    members = {}
    for obj in objects:
        rb = obj.rigid_body
        if not rb:
            continue
        path = _find_prim_path(stage, obj.name)
        if not path:
            continue
        mask = sum(1 << index for index, enabled in enumerate(rb.collision_collections) if enabled)
        prim = stage.GetPrimAtPath(path)
        prim.CreateAttribute('userProperties:blender:collisionMask', Sdf.ValueTypeNames.Int).Set(mask)
        members.setdefault(mask, []).append(Sdf.Path(path))
    if len(members) > 512:
        log.warning("[USDPhysicsHook] Too many distinct collision masks (%d); USD collision filters omitted",
                    len(members))
        return
    root = '/BlenderPhysicsCollisionGroups'
    groups = {}
    for mask in sorted(members):
        group = UsdPhysics.CollisionGroup.Define(stage, f'{root}/mask_{mask:05x}')
        collection = group.GetCollidersCollectionAPI()
        collection.CreateExpansionRuleAttr().Set('explicitOnly')
        collection.CreateIncludesRel().SetTargets(members[mask])
        groups[mask] = group
    for mask, group in groups.items():
        filtered = [other.GetPath() for other_mask, other in groups.items() if not (mask & other_mask)]
        group.CreateFilteredGroupsRel().SetTargets(filtered)


def _joint_type_and_attrs(rbc):
    """Return (usd_joint_type, axis, lower, upper) from a Blender constraint."""
    if rbc.type == 'HINGE':
        lower = math.degrees(rbc.limit_ang_z_lower) if rbc.use_limit_ang_z else -360
        upper = math.degrees(rbc.limit_ang_z_upper) if rbc.use_limit_ang_z else 360
        return 'PhysicsRevoluteJoint', 'Z', lower, upper
    elif rbc.type == 'SLIDER':
        lower = rbc.limit_lin_x_lower if rbc.use_limit_lin_x else -1e6
        upper = rbc.limit_lin_x_upper if rbc.use_limit_lin_x else 1e6
        return 'PhysicsPrismaticJoint', 'X', lower, upper
    elif rbc.type == 'FIXED':
        return 'PhysicsFixedJoint', None, None, None
    elif rbc.type == 'POINT':
        return 'PhysicsSphericalJoint', 'Y', None, None
    elif rbc.type == 'GENERIC_SPRING':
        return 'PhysicsJoint', None, None, None
    else:
        return 'PhysicsFixedJoint', None, None, None


def _export_constraint(stage, obj, mode):
    """Export a rigid body constraint as a USD joint prim."""
    rbc = obj.rigid_body_constraint
    if not rbc or not rbc.object1 or not rbc.object2:
        return

    frames = []
    if mode in ('USD', 'BOTH'):
        for body in (rbc.object1, rbc.object2):
            try:
                relative = body.matrix_world.inverted() @ obj.matrix_world
            except ValueError:
                log.warning("[USDPhysicsHook] Cannot export constraint '%s': body '%s' has a singular transform",
                            obj.name, body.name)
                return
            position = relative.translation
            rotation = relative.to_quaternion().normalized()
            frames.append((position, rotation))

    joint_type, axis, lower, upper = _joint_type_and_attrs(rbc)
    if rbc.type == 'GENERIC_SPRING' and mode == 'MJC':
        # The MuJoCo-only representation remains the legacy scalar mapping.
        joint_type = 'PhysicsDistanceJoint'

    # Create joint prim under /Joints/
    safe_name = obj.name.replace(".", "_")
    joint_path = f"/Joints/{safe_name}"
    joint_prim = stage.DefinePrim(joint_path, joint_type)

    if mode in ('USD', 'BOTH'):
        # Body relationships
        body0_path = _find_prim_path(stage, rbc.object1.name)
        body1_path = _find_prim_path(stage, rbc.object2.name)
        if body0_path:
            joint_prim.CreateRelationship("physics:body0").SetTargets(
                [Sdf.Path(body0_path)])
        if body1_path:
            joint_prim.CreateRelationship("physics:body1").SetTargets(
                [Sdf.Path(body1_path)])

        # The constraint object is a world-space joint frame. Transform it
        # into each body's local space, including parent transforms and rotation.
        for index, (position, rotation) in enumerate(frames):
            joint_prim.CreateAttribute(
                f"physics:localPos{index}", Sdf.ValueTypeNames.Point3f
            ).Set(Gf.Vec3f(position[0], position[1], position[2]))
            joint_prim.CreateAttribute(
                f"physics:localRot{index}", Sdf.ValueTypeNames.Quatf
            ).Set(Gf.Quatf(rotation.w, Gf.Vec3f(rotation.x, rotation.y, rotation.z)))

        # Axis and limits
        if axis:
            joint_prim.CreateAttribute(
                "physics:axis", Sdf.ValueTypeNames.Token).Set(axis)
        if lower is not None:
            joint_prim.CreateAttribute(
                "physics:lowerLimit", Sdf.ValueTypeNames.Float).Set(lower)
        if upper is not None:
            joint_prim.CreateAttribute(
                "physics:upperLimit", Sdf.ValueTypeNames.Float).Set(upper)

        joint_prim.CreateAttribute(
            "physics:jointEnabled", Sdf.ValueTypeNames.Bool).Set(rbc.enabled)

        if rbc.type == 'GENERIC_SPRING':
            _export_generic_spring(joint_prim, rbc)

    if mode in ('MJC', 'BOTH'):
        joint_prim.CreateAttribute(
            "mjc:group", Sdf.ValueTypeNames.Int).Set(0)
        # Map Blender constraint spring/damping if available
        if hasattr(rbc, 'spring_damping_x'):
            joint_prim.CreateAttribute(
                "mjc:damping", Sdf.ValueTypeNames.Double
            ).Set(float(rbc.spring_damping_x))
        if hasattr(rbc, 'spring_stiffness_x'):
            joint_prim.CreateAttribute(
                "mjc:stiffness", Sdf.ValueTypeNames.Double
            ).Set(float(rbc.spring_stiffness_x))


def _spring_axes():
    """USD DOF, Blender property suffix, limit suffix, angular flag."""
    for axis in 'xyz':
        yield 'trans' + axis.upper(), axis, 'lin_' + axis, False
        yield 'rot' + axis.upper(), 'ang_' + axis, 'ang_' + axis, True


def _export_generic_spring(prim, rbc):
    """Represent the enabled axes as D6 drives and limits.

    USD angular drives use degrees; Blender's angular displacements use
    radians. Scale coefficients accordingly. Solver behavior remains an
    approximation, particularly for Blender's legacy spring model.
    """
    for dof, spring_suffix, limit_suffix, angular in _spring_axes():
        if getattr(rbc, 'use_spring_' + spring_suffix, False):
            drive = UsdPhysics.DriveAPI.Apply(prim, dof)
            factor = math.pi / 180.0 if angular else 1.0
            drive.CreateStiffnessAttr(float(getattr(rbc, 'spring_stiffness_' + spring_suffix)) * factor)
            drive.CreateDampingAttr(float(getattr(rbc, 'spring_damping_' + spring_suffix)) * factor)
        if getattr(rbc, 'use_limit_' + limit_suffix, False):
            limit = UsdPhysics.LimitAPI.Apply(prim, dof)
            lower = float(getattr(rbc, 'limit_' + limit_suffix + '_lower'))
            upper = float(getattr(rbc, 'limit_' + limit_suffix + '_upper'))
            if angular:
                lower, upper = math.degrees(lower), math.degrees(upper)
            limit.CreateLowAttr(lower)
            limit.CreateHighAttr(upper)


def _find_prim_path(stage, blender_name):
    """Find a prim path in the stage matching a Blender object name."""
    for prim in stage.Traverse():
        if prim.GetName() == blender_name:
            return str(prim.GetPath())
        # Also check userProperties for original Blender name
        attr = prim.GetAttribute("userProperties:blender:object_name")
        if attr and attr.IsValid() and attr.Get() == blender_name:
            return str(prim.GetPath())
    return None


# ---------------------------------------------------------------------------
# Import helpers
# ---------------------------------------------------------------------------

def _read_usd_physics(stage, mode):
    """Read USD physics prims and return structured data for applying to Blender."""
    result = {
        'scene': None,      # (gravity_dir, gravity_mag, mjc_options)
        'bodies': [],       # [(prim_name, rb_data)]
        'joints': [],       # [(prim_name, joint_data)]
    }

    for prim in stage.Traverse():
        prim_type = prim.GetTypeName()

        # PhysicsScene
        if prim_type == "PhysicsScene":
            scene_data = {'gravity_dir': None, 'gravity_mag': 9.81, 'mjc': {}}
            if mode in ('USD', 'BOTH'):
                phys_scene = UsdPhysics.Scene(prim)
                gd = phys_scene.GetGravityDirectionAttr().Get()
                gm = phys_scene.GetGravityMagnitudeAttr().Get()
                if gd:
                    scene_data['gravity_dir'] = (gd[0], gd[1], gd[2])
                if gm is not None:
                    scene_data['gravity_mag'] = gm
            if mode in ('MJC', 'BOTH'):
                ts = prim.GetAttribute("mjc:option:timestep")
                if ts and ts.IsValid():
                    scene_data['mjc']['timestep'] = ts.Get()
                it = prim.GetAttribute("mjc:option:iterations")
                if it and it.IsValid():
                    scene_data['mjc']['iterations'] = it.Get()
            result['scene'] = scene_data

        # Rigid bodies and stand-alone static collision prims. A collider
        # nested below a rigid body belongs to that body and must not become a
        # second Blender body.
        if mode in ('USD', 'BOTH'):
            has_rigid_body = prim.HasAPI(UsdPhysics.RigidBodyAPI)
            has_collision_api = prim.HasAPI(UsdPhysics.CollisionAPI)
            has_geometry = prim.IsA(UsdGeom.Gprim)
            if not has_geometry and (has_rigid_body or has_collision_api):
                has_geometry = any(child.IsA(UsdGeom.Gprim)
                                   for child in Usd.PrimRange(prim)
                                   if child != prim)
            static_collider = (has_collision_api and has_geometry and
                               not has_rigid_body and
                               not _has_rigid_body_ancestor(prim))
            if (has_rigid_body and has_geometry) or static_collider:
                rb_data = _read_rigid_body(prim, mode, static_collider)
                if rb_data:
                    result['bodies'].append((prim.GetName(), rb_data))

        # Joint prims
        if prim_type in ('PhysicsRevoluteJoint', 'PhysicsPrismaticJoint',
                         'PhysicsSphericalJoint', 'PhysicsFixedJoint',
                         'PhysicsDistanceJoint', 'PhysicsJoint'):
            joint_data = _read_joint(prim, mode)
            result['joints'].append((prim.GetName(), joint_data))

    if mode in ('USD', 'BOTH'):
        _infer_blender_collision_collections(stage, result['bodies'])

    return result


def _has_rigid_body_ancestor(prim):
    ancestor = prim.GetParent()
    while ancestor and ancestor.IsValid() and ancestor.GetPath() != Sdf.Path('/'):
        if ancestor.HasAPI(UsdPhysics.RigidBodyAPI):
            return True
        ancestor = ancestor.GetParent()
    return False


def _read_rigid_body(prim, mode, passive_collision=False):
    """Extract rigid body data from a USD prim."""
    data = {
        'type': 'PASSIVE' if passive_collision else 'ACTIVE',
        'prim_path': str(prim.GetPath()),
        'mass': 1.0,
        'friction': 0.5,
        'restitution': 0.0,
        'kinematic': False,
        'shape': 'CONVEX_HULL',
        'collision_collections': None,
        'geometry_names': [],
    }

    # Blender's USD importer merges parent Xforms by default. A rigid-body
    # prim such as /BodyA can then arrive as the mesh object named after its
    # child prim (for example, Cube). Retain descendant geometry names so
    # import can resolve the physics data to that merged object; primitive
    # Gprims need not use the Mesh schema.
    for child in Usd.PrimRange(prim):
        if child == prim or not child.IsA(UsdGeom.Gprim):
            continue
        data['geometry_names'].append(child.GetName())
        data_name = child.GetAttribute('userProperties:blender:data_name')
        if data_name and data_name.IsValid():
            authored_name = data_name.Get()
            if authored_name and authored_name not in data['geometry_names']:
                data['geometry_names'].append(authored_name)

    mask_attr = prim.GetAttribute('userProperties:blender:collisionMask')
    if mask_attr and mask_attr.HasAuthoredValueOpinion():
        mask = mask_attr.Get()
        if isinstance(mask, int) and 0 <= mask < (1 << 20):
            data['collision_collections'] = [bool(mask & (1 << i)) for i in range(20)]

    if not passive_collision:
        rb_api = UsdPhysics.RigidBodyAPI(prim)
        enabled = rb_api.GetRigidBodyEnabledAttr().Get()
        if enabled is not None and not enabled:
            data['type'] = 'PASSIVE'

        kin = rb_api.GetKinematicEnabledAttr().Get()
        if kin is not None:
            data['kinematic'] = kin

    if prim.HasAPI(UsdPhysics.MassAPI):
        mass_api = UsdPhysics.MassAPI(prim)
        mass_attr = mass_api.GetMassAttr()
        if mass_attr.HasAuthoredValueOpinion():
            m = mass_attr.Get()
            if m is not None and math.isfinite(float(m)) and m > 0.0:
                data['mass'] = m

    if prim.HasAPI(UsdPhysics.MaterialAPI):
        mat_api = UsdPhysics.MaterialAPI(prim)
        sf = mat_api.GetStaticFrictionAttr().Get()
        if sf is not None:
            data['friction'] = sf
        rest = mat_api.GetRestitutionAttr().Get()
        if rest is not None:
            data['restitution'] = rest

    # Infer collision shape from prim type
    prim_type = prim.GetTypeName()
    shape_map = {
        'Cube': 'BOX', 'Sphere': 'SPHERE', 'Capsule': 'CAPSULE',
        'Cylinder': 'CYLINDER', 'Cone': 'CONE', 'Mesh': 'MESH',
    }
    data['shape'] = shape_map.get(prim_type, 'CONVEX_HULL')

    return data


def _infer_blender_collision_collections(stage, bodies):
    try:
        _infer_blender_collision_collections_impl(stage, bodies)
    except Exception as exc:
        log.warning("[USDPhysicsHook] Failed to evaluate USD collision groups "
                    "(%s); imported body collisions disabled",
                    type(exc).__name__)
        for _, data in bodies:
            data['collision_collections'] = [False] * 20


def _infer_blender_collision_collections_impl(stage, bodies):
    """Map UsdPhysics collision-group filtering to Blender's 20 bit layers.

    Blender tests collisions when two objects share a collision-collection bit.
    The exact USD collision graph can be represented as a cover of allowed
    collider pairs by cliques. If more than 20 cliques are needed, disable the
    imported body pairs rather than accidentally re-enabling a filtered pair.
    """
    group_prims = [prim for prim in stage.Traverse()
                   if prim.IsA(UsdPhysics.CollisionGroup)]
    if not group_prims or not bodies:
        return

    authored_masks = [data['collision_collections'] is not None
                      for _, data in bodies]
    if any(authored_masks):
        if not all(authored_masks):
            log.warning("[USDPhysicsHook] Collision-group inference skipped: "
                        "Blender masks are authored for only some bodies; "
                        "unannotated body collisions disabled")
            for _, data in bodies:
                if data['collision_collections'] is None:
                    data['collision_collections'] = [False] * 20
        return

    if len(bodies) > 256 or len(group_prims) > 256:
        log.warning("[USDPhysicsHook] Collision-group inference skipped for %d "
                    "bodies and %d groups (limit 256 each); imported body "
                    "collisions disabled", len(bodies), len(group_prims))
        for _, data in bodies:
            data['collision_collections'] = [False] * 20
        return

    path_to_key = {}
    groups = {}
    for prim in group_prims:
        merge_attr = prim.GetAttribute('physics:mergeGroupName')
        merge_name = merge_attr.Get() if merge_attr and merge_attr.IsValid() else None
        key = ('merge', str(merge_name)) if merge_name else ('path', str(prim.GetPath()))
        path_to_key[prim.GetPath()] = key
        group = groups.setdefault(key, {'queries': [], 'rules': []})
        usd_group = UsdPhysics.CollisionGroup(prim)
        collection = usd_group.GetCollidersCollectionAPI()
        query = collection.ComputeMembershipQuery()
        group['queries'].append(query)
        invert_attr = usd_group.GetInvertFilteredGroupsAttr()
        invert = bool(invert_attr.Get()) if invert_attr and invert_attr.IsValid() else False
        targets = set(usd_group.GetFilteredGroupsRel().GetTargets())
        group['rules'].append((invert, targets))

    for group in groups.values():
        merged_rules = {}
        for invert, targets in group['rules']:
            merged_rules.setdefault(invert, set()).update(
                path_to_key[target] for target in targets if target in path_to_key)
        group['rules'] = list(merged_rules.items())

    body_memberships = [set() for _ in bodies]
    for index, (_, data) in enumerate(bodies):
        body_prim = stage.GetPrimAtPath(data['prim_path'])
        body_paths = [prim.GetPath() for prim in Usd.PrimRange(body_prim)]
        for key, group in groups.items():
            if any(query.IsPathIncluded(path)
                   for query in group['queries'] for path in body_paths):
                body_memberships[index].add(key)

    def group_disables(group_key, other_memberships):
        for invert, target_keys in groups[group_key]['rules']:
            matches_target = bool(other_memberships & target_keys)
            if (invert and not matches_target) or (not invert and matches_target):
                return True
        return False

    count = len(bodies)
    neighbors = [set() for _ in bodies]
    for first in range(count):
        for second in range(first + 1, count):
            disabled = any(group_disables(key, body_memberships[second])
                           for key in body_memberships[first])
            if not disabled:
                disabled = any(group_disables(key, body_memberships[first])
                               for key in body_memberships[second])
            if not disabled:
                neighbors[first].add(second)
                neighbors[second].add(first)

    possible_pairs = count * (count - 1) // 2
    if sum(len(entries) for entries in neighbors) // 2 == possible_pairs:
        return

    remaining = {(first, second) for first in range(count)
                 for second in neighbors[first] if first < second}
    masks = [0] * count
    bit = 0
    while remaining and bit < 20:
        degrees = [sum(1 for pair in remaining if index in pair)
                   for index in range(count)]
        first, second = max(
            remaining,
            key=lambda pair: (degrees[pair[0]] + degrees[pair[1]],
                              max(degrees[pair[0]], degrees[pair[1]]),
                              -pair[0], -pair[1]))
        clique = {first, second}
        candidates = neighbors[first] & neighbors[second]
        while candidates:
            candidate = max(
                candidates,
                key=lambda index: (sum(1 for member in clique
                                       if tuple(sorted((index, member))) in remaining),
                                   degrees[index], -index))
            clique.add(candidate)
            candidates &= neighbors[candidate]
        for index in clique:
            masks[index] |= 1 << bit
        members = sorted(clique)
        for first_index, first_member in enumerate(members):
            for second_member in members[first_index + 1:]:
                remaining.discard((first_member, second_member))
        bit += 1

    if remaining:
        log.warning("[USDPhysicsHook] USD collision filtering needs more than "
                    "20 Blender layers; imported body collisions disabled")
        masks = [0] * count
    else:
        log.info("[USDPhysicsHook] Mapped USD collision groups to %d Blender "
                 "collision layers", bit)

    for index, (_, data) in enumerate(bodies):
        data['collision_collections'] = [bool(masks[index] & (1 << layer))
                                         for layer in range(20)]


def _read_joint(prim, mode):
    """Extract joint data from a USD joint prim."""
    data = {
        'type': prim.GetTypeName(),
        'body0': None,
        'body1': None,
        'axis': 'Z',
        'lower': None,
        'upper': None,
        'enabled': True,
        'mjc': {},
        'axes': {},
        'frames': {},
    }

    if mode in ('USD', 'BOTH'):
        b0_rel = prim.GetRelationship("physics:body0")
        if b0_rel:
            targets = b0_rel.GetTargets()
            if targets:
                data['body0'] = str(targets[0])
        b1_rel = prim.GetRelationship("physics:body1")
        if b1_rel:
            targets = b1_rel.GetTargets()
            if targets:
                data['body1'] = str(targets[0])

        ax = prim.GetAttribute("physics:axis")
        if ax and ax.IsValid():
            data['axis'] = ax.Get()

        ll = prim.GetAttribute("physics:lowerLimit")
        if ll and ll.IsValid():
            data['lower'] = ll.Get()
        ul = prim.GetAttribute("physics:upperLimit")
        if ul and ul.IsValid():
            data['upper'] = ul.Get()

        je = prim.GetAttribute("physics:jointEnabled")
        if je and je.IsValid():
            data['enabled'] = je.Get()

        for index in (0, 1):
            pos = prim.GetAttribute(f"physics:localPos{index}")
            rot = prim.GetAttribute(f"physics:localRot{index}")
            if pos and rot and (pos.HasAuthoredValueOpinion() or rot.HasAuthoredValueOpinion()):
                p, q = pos.Get(), rot.Get()
                if p is not None and q is not None:
                    imag = q.GetImaginary()
                    data['frames'][index] = (tuple(p), (q.GetReal(), imag[0], imag[1], imag[2]))

        if prim.GetTypeName() == 'PhysicsJoint':
            for dof, _, _, angular in _spring_axes():
                axis = {}
                if prim.HasAPI(UsdPhysics.DriveAPI, dof):
                    drive = UsdPhysics.DriveAPI(prim, dof)
                    factor = 180.0 / math.pi if angular else 1.0
                    axis['stiffness'] = drive.GetStiffnessAttr().Get() * factor
                    axis['damping'] = drive.GetDampingAttr().Get() * factor
                if prim.HasAPI(UsdPhysics.LimitAPI, dof):
                    limit = UsdPhysics.LimitAPI(prim, dof)
                    lower, upper = limit.GetLowAttr().Get(), limit.GetHighAttr().Get()
                    if lower is not None and upper is not None:
                        axis['lower'] = math.radians(lower) if angular else lower
                        axis['upper'] = math.radians(upper) if angular else upper
                if axis:
                    data['axes'][dof] = axis

    if mode in ('MJC', 'BOTH'):
        for attr_name in ('mjc:group', 'mjc:damping', 'mjc:stiffness',
                          'mjc:armature', 'mjc:frictionloss'):
            attr = prim.GetAttribute(attr_name)
            if attr and attr.IsValid():
                key = attr_name.replace("mjc:", "")
                data['mjc'][key] = attr.Get()

    return data


def _apply_physics_to_blender(result):
    """Apply imported physics data to Blender objects."""
    scene = bpy.context.scene

    def activate_object(obj):
        # USD import callbacks may run without an active/selected object.
        # Rigid-body operators poll the active selection, so establish it
        # explicitly instead of relying on the importer's current context.
        for selected in getattr(bpy.context, 'selected_objects', ()):
            if hasattr(selected, 'select_set'):
                selected.select_set(False)
        if hasattr(obj, 'select_set'):
            obj.select_set(True)
        bpy.context.view_layer.objects.active = obj

    # Scene gravity
    if result['scene']:
        sd = result['scene']
        mag = sd.get('gravity_mag', 9.81)
        gdir = sd.get('gravity_dir', (0, 0, -1))
        if gdir:
            scene.gravity = (gdir[0]*mag, gdir[1]*mag, gdir[2]*mag)

        # MuJoCo scene options -> rigid body world
        if sd.get('mjc'):
            if scene.rigidbody_world is None:
                bpy.ops.rigidbody.world_add()
            rbw = scene.rigidbody_world
            if 'timestep' in sd['mjc']:
                timestep = float(sd['mjc']['timestep'])
                if math.isfinite(timestep) and timestep > 0.0:
                    frame_rate = 1.0 / timestep
                    fps = int(round(frame_rate))
                    fps_base = frame_rate / fps if fps else 0.0
                    if 1 <= fps <= 32767 and math.isfinite(fps_base) and fps_base > 0.0:
                        scene.render.fps = fps
                        scene.render.fps_base = fps_base
                    else:
                        log.warning("[USDPhysicsHook] Ignoring unsupported "
                                    "physics timestep %.9g", timestep)
                else:
                    log.warning("[USDPhysicsHook] Ignoring invalid physics "
                                "timestep %r", timestep)
            if 'iterations' in sd['mjc']:
                rbw.solver_iterations = sd['mjc']['iterations']

    # Rigid bodies
    body_objects = {}
    for prim_name, rb_data in result['bodies']:
        obj = bpy.data.objects.get(prim_name)
        # With merge_parent_xform=False the body prim is an Empty and its
        # collider mesh is a child. Apply the rigid body to that mesh when
        # there is exactly one; otherwise preserve the exact prim match.
        if obj and getattr(obj, 'type', None) == 'EMPTY':
            mesh_children = [child for child in getattr(obj, 'children_recursive', ())
                             if getattr(child, 'type', None) == 'MESH']
            if len(mesh_children) == 1:
                obj = mesh_children[0]
            elif len(mesh_children) > 1:
                log.warning("[USDPhysicsHook] Body '%s' has multiple imported mesh "
                            "children; cannot map it to one Blender rigid body",
                            prim_name)
                obj = None
        if not obj:
            mesh_candidates = []
            for geometry_name in rb_data.get('geometry_names', ()):
                for candidate in (geometry_name, geometry_name.replace('.', '_')):
                    candidate_obj = bpy.data.objects.get(candidate)
                    if candidate_obj and getattr(candidate_obj, 'type', None) == 'MESH':
                        if candidate_obj not in mesh_candidates:
                            mesh_candidates.append(candidate_obj)
            if len(mesh_candidates) == 1:
                obj = mesh_candidates[0]
            elif len(mesh_candidates) > 1:
                log.warning("[USDPhysicsHook] Body '%s' maps to multiple imported "
                            "mesh objects; cannot choose one rigid body", prim_name)
        if not obj:
            log.warning(f"[USDPhysicsHook] Object '{prim_name}' not found, "
                        f"skipping rigid body import")
            continue

        body_objects[prim_name] = obj
        if ((rb_data['type'] == 'ACTIVE' or rb_data['kinematic']) and
                obj.parent and not obj.parent.rigid_body):
            # Blender does not simulate a rigid body while an un-simulated
            # parent continues to impose its transform. Preserve world-space
            # scale and pose, then detach the collider from the imported
            # Xform container before enabling dynamics.
            bpy.context.view_layer.update()
            world_transform = obj.matrix_world.copy()
            obj.parent = None
            obj.matrix_world = world_transform
        activate_object(obj)
        if not obj.rigid_body:
            bpy.ops.rigidbody.object_add(type=rb_data['type'])
        rb = obj.rigid_body
        rb.type = rb_data['type']
        rb.mass = rb_data['mass']
        rb.friction = rb_data['friction']
        rb.restitution = rb_data['restitution']
        rb.kinematic = rb_data['kinematic']
        rb.collision_shape = rb_data['shape']
        if rb_data['collision_collections'] is not None:
            rb.collision_collections = rb_data['collision_collections']

    # Joints -> rigid body constraints
    for joint_name, jd in result['joints']:
        # Find or create the constraint empty
        obj = bpy.data.objects.get(joint_name)
        if not obj:
            obj = bpy.data.objects.new(joint_name, None)
            scene.collection.objects.link(obj)
            obj.name = joint_name

        # Map USD joint type to Blender constraint type
        blender_type_map = {
            'PhysicsRevoluteJoint': 'HINGE',
            'PhysicsPrismaticJoint': 'SLIDER',
            'PhysicsSphericalJoint': 'POINT',
            'PhysicsFixedJoint': 'FIXED',
            'PhysicsDistanceJoint': 'GENERIC_SPRING',
            'PhysicsJoint': 'GENERIC_SPRING' if any('stiffness' in a for a in jd['axes'].values()) else 'GENERIC',
        }
        bl_type = blender_type_map.get(jd['type'], 'FIXED')

        activate_object(obj)
        if not obj.rigid_body_constraint:
            bpy.ops.rigidbody.constraint_add(type=bl_type)
        rbc = obj.rigid_body_constraint
        rbc.type = bl_type
        rbc.enabled = jd['enabled']

        if jd['type'] == 'PhysicsJoint':
            for dof, spring_suffix, limit_suffix, _ in _spring_axes():
                axis = jd['axes'].get(dof, {})
                if bl_type == 'GENERIC_SPRING':
                    setattr(rbc, 'use_spring_' + spring_suffix, 'stiffness' in axis)
                    if 'stiffness' in axis:
                        setattr(rbc, 'spring_stiffness_' + spring_suffix, axis['stiffness'])
                        setattr(rbc, 'spring_damping_' + spring_suffix, axis['damping'])
                setattr(rbc, 'use_limit_' + limit_suffix, 'lower' in axis)
                if 'lower' in axis:
                    setattr(rbc, 'limit_' + limit_suffix + '_lower', axis['lower'])
                    setattr(rbc, 'limit_' + limit_suffix + '_upper', axis['upper'])

        # Resolve body references
        if jd['body0']:
            leaf = jd['body0'].rsplit('/', 1)[-1]
            rbc.object1 = body_objects.get(leaf) or bpy.data.objects.get(leaf)
        if jd['body1']:
            leaf = jd['body1'].rsplit('/', 1)[-1]
            rbc.object2 = body_objects.get(leaf) or bpy.data.objects.get(leaf)

        # Reconstruct the world frame from either resolved body and its local
        # frame. Body 0 is preferred when both are present.
        for index, body in enumerate((rbc.object1, rbc.object2)):
            if body and index in jd['frames']:
                from mathutils import Matrix, Quaternion
                position, rotation = jd['frames'][index]
                obj.matrix_world = (body.matrix_world @ Matrix.Translation(position) @
                                    Quaternion(rotation).to_matrix().to_4x4())
                break

        # Limits
        if bl_type == 'HINGE' and jd['lower'] is not None:
            rbc.use_limit_ang_z = True
            rbc.limit_ang_z_lower = math.radians(jd['lower'])
            rbc.limit_ang_z_upper = math.radians(jd['upper'])
        elif bl_type == 'SLIDER' and jd['lower'] is not None:
            rbc.use_limit_lin_x = True
            rbc.limit_lin_x_lower = jd['lower']
            rbc.limit_lin_x_upper = jd['upper']


# ---------------------------------------------------------------------------
# USD Hooks
# ---------------------------------------------------------------------------

class USDPhysicsExportHook(bpy.types.USDHook):
    """Export Blender rigid body physics to USD Physics / MuJoCo attributes."""
    bl_idname = "usd_physics_export"
    bl_label = "USD Physics Export"
    bl_description = "Write Blender rigid body data as UsdPhysics and/or " \
                     "MuJoCo mjcPhysics attributes"

    @staticmethod
    def on_export(export_context):
        if not _HAS_PXR:
            log.warning("[USDPhysicsHook] pxr not available — skipping export")
            return True

        stage = export_context.get_stage()
        if not stage:
            return False

        mode = _get_mode()
        scene = bpy.context.scene
        exported_count = 0

        # Export physics scene (gravity, solver settings)
        if scene.rigidbody_world:
            _apply_scene_physics(stage, scene, mode)
            exported_count += 1

        # Export rigid bodies
        for obj in bpy.data.objects:
            if obj.rigid_body:
                prim_path = _find_prim_path(stage, obj.name)
                if prim_path:
                    prim = stage.GetPrimAtPath(prim_path)
                    if prim:
                        _apply_rigid_body_api(prim, obj.rigid_body, mode)
                        exported_count += 1

        if mode in ('USD', 'BOTH'):
            _export_collision_groups(stage, bpy.data.objects)

        # Export constraints as joints
        for obj in bpy.data.objects:
            if obj.rigid_body_constraint:
                _export_constraint(stage, obj, mode)
                exported_count += 1

        log.info(f"[USDPhysicsHook] Exported {exported_count} physics "
                 f"elements (mode={mode})")
        return True


class USDPhysicsImportHook(bpy.types.USDHook):
    """Import USD Physics / MuJoCo attributes into Blender rigid body data."""
    bl_idname = "usd_physics_import"
    bl_label = "USD Physics Import"
    bl_description = "Read UsdPhysics and/or MuJoCo mjcPhysics attributes " \
                     "into Blender rigid body settings"

    @staticmethod
    def on_import(import_context):
        if not _HAS_PXR:
            log.warning("[USDPhysicsHook] pxr not available — skipping import")
            return True

        stage = import_context.get_stage()
        if not stage:
            return False

        mode = _get_mode()
        result = _read_usd_physics(stage, mode)

        body_count = len(result['bodies'])
        joint_count = len(result['joints'])
        has_scene = result['scene'] is not None

        if body_count == 0 and joint_count == 0 and not has_scene:
            log.info("[USDPhysicsHook] No physics data found in USD file")
            return True

        _apply_physics_to_blender(result)

        log.info(f"[USDPhysicsHook] Imported physics: scene={'yes' if has_scene else 'no'}, "
                 f"{body_count} bodies, {joint_count} joints (mode={mode})")
        return True


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

_classes = (
    USDPhysicsHookPreferences,
    USDPhysicsExportHook,
    USDPhysicsImportHook,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    log.info("[USDPhysicsHook] Registered export + import hooks")


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
    log.info("[USDPhysicsHook] Unregistered hooks")


if __name__ == "__main__":
    register()
