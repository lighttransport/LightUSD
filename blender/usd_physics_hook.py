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
            ).Set(1.0 / scene.render.fps)
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

        # RigidBodyAPI on mesh prims
        if mode in ('USD', 'BOTH') and prim.HasAPI(UsdPhysics.RigidBodyAPI):
            rb_data = _read_rigid_body(prim, mode)
            if rb_data:
                result['bodies'].append((prim.GetName(), rb_data))

        # Joint prims
        if prim_type in ('PhysicsRevoluteJoint', 'PhysicsPrismaticJoint',
                         'PhysicsSphericalJoint', 'PhysicsFixedJoint',
                         'PhysicsDistanceJoint', 'PhysicsJoint'):
            joint_data = _read_joint(prim, mode)
            result['joints'].append((prim.GetName(), joint_data))

    return result


def _read_rigid_body(prim, mode):
    """Extract rigid body data from a USD prim."""
    data = {
        'type': 'ACTIVE',
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
    # child prim (for example, Cube). Retain descendant mesh names so import
    # can resolve the physics data to that merged object.
    for child in Usd.PrimRange(prim):
        if child == prim or not child.IsA(UsdGeom.Mesh):
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

    rb_api = UsdPhysics.RigidBodyAPI(prim)
    enabled = rb_api.GetRigidBodyEnabledAttr().Get()
    if enabled is not None and not enabled:
        data['type'] = 'PASSIVE'

    kin = rb_api.GetKinematicEnabledAttr().Get()
    if kin is not None:
        data['kinematic'] = kin

    if prim.HasAPI(UsdPhysics.MassAPI):
        mass_api = UsdPhysics.MassAPI(prim)
        m = mass_api.GetMassAttr().Get()
        if m is not None:
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
