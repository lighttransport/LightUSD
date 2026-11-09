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
lists anything skipped. The conversion runs natively
(``lightusd_stage_convert_units`` in the C API).
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

from . import _core

__all__ = ["UnitConversion", "convert_units"]


@dataclass
class UnitConversion:
    """What :func:`convert_units` changed."""

    length_scale: float
    mass_scale: float
    converted: int = 0
    rotated_roots: int = 0
    skipped: list = field(default_factory=list)


def convert_units(stage, *, meters_per_unit: Optional[float] = None,
                  kilograms_per_unit: Optional[float] = None,
                  up_axis: Optional[str] = None) -> UnitConversion:
    """Convert ``stage`` in place to the given units and up axis.

    Unset targets keep the stage's current convention. An unauthored
    ``metersPerUnit`` is USD's 0.01 and ``kilogramsPerUnit`` is 1.
    """
    if (meters_per_unit is not None and meters_per_unit < 0) or \
            (kilograms_per_unit is not None and kilograms_per_unit < 0):
        raise ValueError("units must be positive")
    if up_axis is not None and up_axis not in ("Y", "Z"):
        raise ValueError("up_axis must be Y or Z")
    length, mass, converted, rotated, skipped = _core._convert_units(
        stage, meters_per_unit or 0.0, kilograms_per_unit or 0.0, up_axis)
    return UnitConversion(length_scale=length, mass_scale=mass, converted=converted,
                          rotated_roots=rotated, skipped=skipped)
