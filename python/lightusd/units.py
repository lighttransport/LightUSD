# SPDX-License-Identifier: Apache-2.0
"""Physics-aware unit conversion for LightUSD stages.

``convert_units`` rescales a stage from its ``metersPerUnit`` /
``kilogramsPerUnit`` (and optionally ``upAxis``) into another convention, for
example to bring an SI robot into a centimeter Z-up stage. Every converted
attribute is scaled by ``L**a * M**b`` with ``L = old_mpu / new_mpu`` and
``M = old_kpu / new_kpu``, where ``(a, b)`` is the attribute's length and mass
dimension. Joint, drive and limit dimensions depend on whether the degree of
freedom is linear or angular (angles stay in degrees). Time samples are
converted with their defaults.

Covered: UsdGeom gprim sizes, points, widths, extents and translations;
UsdPhysics mass properties, joint frames, linear limits, drives and joint
state, scene gravity, materials' density; NewtonJointAPI / NewtonMassAPI /
contact distances; deformable masses and thicknesses. ``mjc:*`` attributes
keep MuJoCo's own (model) units and are not touched; the returned report
lists anything skipped.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

__all__ = ["UnitConversion", "convert_units"]

# Attribute name -> (length, mass) dimension, independent of the prim.
_FIXED = {
    "points": (1, 0), "extent": (1, 0), "widths": (1, 0),
    "radius": (1, 0), "height": (1, 0), "size": (1, 0), "width": (1, 0), "length": (1, 0),
    "physics:localPos0": (1, 0), "physics:localPos1": (1, 0),
    "physics:centerOfMass": (1, 0), "physics:velocity": (1, 0),
    "physics:gravityMagnitude": (1, 0),
    "physics:mass": (0, 1), "physics:density": (-3, 1),
    "physics:diagonalInertia": (2, 1), "newton:inertia": (2, 1),
    "newton:contactMargin": (1, 0), "newton:contactGap": (1, 0),
    "physics:masses": (0, 1), "physics:thicknesses": (1, 0),
}
_GPRIM_SIZES = {"radius", "height", "size", "width", "length"}
# Per degree-of-freedom quantities: (linear dims, angular dims).
_DOF = {
    "targetPosition": ((1, 0), (0, 0)), "targetVelocity": ((1, 0), (0, 0)),
    "stiffness": ((0, 1), (2, 1)), "damping": ((0, 1), (2, 1)), "maxForce": ((1, 1), (2, 1)),
    "position": ((1, 0), (0, 0)), "velocity": ((1, 0), (0, 0)),
    "low": ((1, 0), (0, 0)), "high": ((1, 0), (0, 0)),
}
_NEWTON_JOINT = {"newton:armature": ((0, 1), (2, 1)), "newton:damping": ((0, 1), (2, 1)),
                 "newton:friction": ((1, 1), (2, 1)), "newton:velocityLimit": ((1, 0), (0, 0))}
_UP_ROTATION = {("Y", "Z"): 90.0, ("Z", "Y"): -90.0}


@dataclass
class UnitConversion:
    """What :func:`convert_units` changed."""

    length_scale: float
    mass_scale: float
    converted: int = 0
    rotated_roots: int = 0
    skipped: list = field(default_factory=list)


def _scale(value, factor):
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return value * factor
    if isinstance(value, (list, tuple)):
        return type(value)(_scale(v, factor) for v in value) if isinstance(value, tuple) else [_scale(v, factor) for v in value]
    try:
        return [_scale(v, factor) for v in value]
    except TypeError:
        return value


def _dof_kind(prim, name: str) -> Optional[bool]:
    """True for a linear DOF, False for angular, None if unknown."""
    for key in ("linear", "transX", "transY", "transZ", "distance"):
        if f":{key}:" in name:
            return True
    for key in ("angular", "rotX", "rotY", "rotZ"):
        if f":{key}:" in name:
            return False
    if prim.type_name == "PhysicsPrismaticJoint":
        return True
    if prim.type_name in ("PhysicsRevoluteJoint", "PhysicsSphericalJoint"):
        return False
    return None


def _dims(prim, name: str):
    if name in _FIXED:
        if name in _GPRIM_SIZES and prim.type_name in ("", "Xform", "Scope", "Material", "Shader"):
            return None
        return _FIXED[name]
    if name.startswith("xformOp:translate"):
        return (1, 0)
    if name in _NEWTON_JOINT:
        kind = _dof_kind(prim, name)
        return None if kind is None else _NEWTON_JOINT[name][0 if kind else 1]
    if name in ("physics:lowerLimit", "physics:upperLimit") and prim.type_name == "PhysicsPrismaticJoint":
        return (1, 0)
    if name.startswith(("drive:", "limit:", "state:")) and ":physics:" in name:
        field_name = name.rsplit(":", 1)[1]
        kind = _dof_kind(prim, name)
        if field_name in _DOF and kind is not None:
            return _DOF[field_name][0 if kind else 1]
    return None


def convert_units(stage, *, meters_per_unit: Optional[float] = None,
                  kilograms_per_unit: Optional[float] = None,
                  up_axis: Optional[str] = None) -> UnitConversion:
    """Convert ``stage`` in place to the given units and up axis.

    Unset targets keep the stage's current convention. An unauthored
    ``metersPerUnit`` is USD's 0.01 and ``kilogramsPerUnit`` is 1.
    """
    old_mpu = float(stage.get_metadata("metersPerUnit") or 0.01)
    old_kpu = float(stage.get_metadata("kilogramsPerUnit") or 1.0)
    new_mpu = float(meters_per_unit or old_mpu)
    new_kpu = float(kilograms_per_unit or old_kpu)
    if new_mpu <= 0 or new_kpu <= 0:
        raise ValueError("units must be positive")
    old_up = str(stage.get_metadata("upAxis") or "Y")
    new_up = up_axis or old_up
    if new_up not in ("Y", "Z"):
        raise ValueError("up_axis must be Y or Z")
    report = UnitConversion(length_scale=old_mpu / new_mpu, mass_scale=old_kpu / new_kpu)
    L, M = report.length_scale, report.mass_scale

    for prim in stage:
        for name in prim.attributes:
            attribute = prim.attribute(name)
            if attribute is None or attribute.is_connection:
                continue
            if name == "xformOp:transform":
                dims = (1, 0)
            else:
                dims = _dims(prim, name)
            if dims is None:
                # Dimensional DOF data whose linear/angular kind is unknown.
                if (name.startswith(("drive:", "limit:", "state:")) or name in _NEWTON_JOINT) and \
                        _dof_kind(prim, name) is None:
                    report.skipped.append(f"{prim.path}.{name}")
                continue
            factor = L ** dims[0] * M ** dims[1]
            if factor == 1.0:
                continue

            def convert(value):
                if name == "xformOp:transform":
                    # Row-major (row-vector) matrix, flat or nested: scale the
                    # translation row only.
                    flat = [float(v) for row in value for v in (row if isinstance(row, (list, tuple)) else (row,))]
                    flat[12:15] = [v * L for v in flat[12:15]]
                    return tuple(flat)
                return _scale(value, factor)

            type_name, uniform = attribute.type_name, attribute.is_uniform
            if attribute.has_timesamples:
                for time, value in list(attribute.timesamples):
                    prim.set(name, convert(value), type=type_name, time=time)
            value = attribute.get()
            if value is not None:
                prim.set(name, convert(value), type=type_name, uniform=uniform)
            report.converted += 1

    if new_up != old_up:
        angle = _UP_ROTATION[(old_up, new_up)]
        op = "xformOp:rotateX:upAxis"
        for prim in stage.root_prims:
            order = [str(o) for o in prim["xformOpOrder"]] if "xformOpOrder" in prim.attributes else []
            if op in order:
                # A previous conversion's rotation: accumulate, drop at zero.
                total = float(prim[op]) + angle
                if abs(total) < 1e-9:
                    prim.remove_property(op)
                    prim.set("xformOpOrder", [o for o in order if o != op], type="token[]", uniform=True)
                else:
                    prim.set(op, total, type="double")
            else:
                prim.set(op, angle, type="double")
                prim.set("xformOpOrder", [op] + order, type="token[]", uniform=True)
            report.rotated_roots += 1
        for scene in stage.prims_of_type("PhysicsScene"):
            if "physics:gravityDirection" in scene.attributes:
                x, y, z = scene["physics:gravityDirection"]
                # Express the old-up-axis gravity in the rotated frame.
                # Rx(+90) (Y-up -> Z-up) maps (x, y, z) -> (x, -z, y).
                rotated = (x, -z, y) if new_up == "Z" else (x, z, -y)
                scene.set("physics:gravityDirection", rotated, type="vector3f")
        stage.up_axis = new_up
    stage.meters_per_unit = new_mpu
    stage.set_metadata("kilogramsPerUnit", new_kpu)
    return report
