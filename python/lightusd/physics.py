# SPDX-License-Identifier: Apache-2.0
"""USD physics inspection and authoring, including MuJoCo and Newton metadata.

These helpers author descriptive USD data; they do not run a physics engine.
All angles authored here use USD units (degrees), lengths use stage units,
and quaternions use USD order ``(real, i, j, k)``.
"""
from __future__ import annotations
from dataclasses import dataclass, field
import json
from types import MappingProxyType
from typing import Any, Mapping

__all__ = ["PhysicsRecord", "PhysicsData", "PhysicsValidationIssue", "PhysicsValidationReport",
           "inspect", "validate", "apply_schema", "define_prim", "schema_definitions"]


@dataclass(frozen=True)
class PhysicsRecord:
    prim_path: str
    type_name: str
    api_schemas: tuple[str, ...]
    attributes: Mapping[str, Any]
    relationships: Mapping[str, tuple[str, ...]]
    property_types: Mapping[str, str] = field(default_factory=lambda: MappingProxyType({}))
    uniform_properties: frozenset[str] = frozenset()
    blocked_properties: frozenset[str] = frozenset()


@dataclass(frozen=True)
class PhysicsData:
    scenes: tuple[PhysicsRecord, ...] = ()
    rigid_bodies: tuple[PhysicsRecord, ...] = ()
    colliders: tuple[PhysicsRecord, ...] = ()
    joints: tuple[PhysicsRecord, ...] = ()
    materials: tuple[PhysicsRecord, ...] = ()
    articulation_roots: tuple[PhysicsRecord, ...] = ()
    filtered_pairs: tuple[PhysicsRecord, ...] = ()
    actuators: tuple[PhysicsRecord, ...] = ()
    sites: tuple[PhysicsRecord, ...] = ()
    tendons: tuple[PhysicsRecord, ...] = ()
    equalities: tuple[PhysicsRecord, ...] = ()
    sensors: tuple[PhysicsRecord, ...] = ()
    contacts: tuple[PhysicsRecord, ...] = ()
    keyframes: tuple[PhysicsRecord, ...] = ()
    extensions: tuple[PhysicsRecord, ...] = ()
    collision_groups: tuple[PhysicsRecord, ...] = ()


@dataclass(frozen=True)
class PhysicsValidationIssue:
    severity: str
    rule_id: str
    location: str
    message: str
    source_asset: str = ""
    variants: str = ""
    time: float | None = None


@dataclass(frozen=True)
class PhysicsValidationReport:
    issues: tuple[PhysicsValidationIssue, ...]
    checked_groups: tuple[str, ...]
    error_count: int
    warning_count: int
    omitted_issues: int
    complete: bool
    valid: bool

    def __bool__(self):
        return self.valid


def validate(stage, *, include_core: bool = True, samples: bool = False,
             require_complete: bool = False, max_issues: int = 10000,
             max_report_bytes: int = 8 * 1024**2, max_samples: int = 256) -> PhysicsValidationReport:
    """Run shared native physics (and optionally core schema) validation.

    Findings return an immutable report. Invalid API arguments raise exceptions.
    ``samples`` also checks coherent snapshots at authored sample times. Counts
    describe retained findings; report limits force ``complete`` and ``valid``
    false and expose ``omitted_issues``. This is semantic validation, not an
    engine compilation or simulation check.
    """
    from . import _core
    if min(max_issues, max_report_bytes, max_samples) < 1:
        raise ValueError("validation limits must be positive")
    data = _core._validate_physics(stage, include_core=include_core, samples=samples,
        require_complete=require_complete, max_issues=max_issues,
        max_report_bytes=max_report_bytes, max_samples=max_samples)
    data["issues"] = tuple(PhysicsValidationIssue(**issue) for issue in data["issues"])
    data["checked_groups"] = tuple(data["checked_groups"])
    return PhysicsValidationReport(**data)


def _freeze(value, array_views=False):
    from . import Array
    if isinstance(value, Array):
        if array_views:
            return value
        value = memoryview(value).tolist()
    if isinstance(value, (list, tuple)):
        return tuple(_freeze(v, array_views) for v in value)
    if isinstance(value, dict):
        return MappingProxyType({k: _freeze(v, array_views) for k, v in value.items()})
    return value


def inspect(stage, *, time: float | None = 0.0, array_views: bool = False,
            max_memory: int = 0, max_records: int = 1000000) -> PhysicsData:
    """Take an immutable native snapshot of authored physics values and targets.

    ``time=None`` queries USD DefaultTime. A prim may appear in several groups.
    Unknown engine properties retain their native types. With ``array_views``,
    numeric arrays remain read-only ``lightusd.Array`` buffers rather than
    tuples; both representations survive stage edits and closure. Resource
    limits bound native snapshot accounting (zero memory selects 512 MiB).
    """
    from . import _core
    if max_memory < 0 or max_records < 1:
        raise ValueError("max_memory must be non-negative and max_records positive")
    groups = {name: [] for name in PhysicsData.__dataclass_fields__}
    for item in _core._query_physics(stage, time=time, max_memory=max_memory,
                                   max_records=max_records):
        record = PhysicsRecord(
            item["prim_path"], item["type_name"], item["api_schemas"],
            _freeze(item["attributes"], array_views), _freeze(item["relationships"]),
            _freeze(item["property_types"]), frozenset(item["uniform_properties"]),
            frozenset(item["blocked_properties"]),
        )
        for name in item["groups"]:
            groups[name].append(record)
    return PhysicsData(**{k: tuple(v) for k, v in groups.items()})


# Canonical USD property types. Extension properties outside this table can
# be authored explicitly as (USD_type_name, value); no guessed array types.
_TYPES = {}

def _types(type_name, *names):
    _TYPES.update(dict.fromkeys(names, type_name))

_types("bool", "newton:gravityEnabled", "newton:selfCollisionEnabled",
       "newton:mimicEnabled", "mjc:freeJoint", "mjc:mocap")
_types("token", "mjc:type", "mjc:dynType", "mjc:gainType", "mjc:biasType",
       "mjc:trnType", "mjc:inertia", "mjc:objtype", "mjc:objname", "mjc:reftype",
       "mjc:refname", "mjc:plugin", "mjc:instance")
_types("int", "newton:timeStepsPerSecond", "newton:maxSolverIterations", "newton:maxHullVertices",
       "newton:delaySteps", "mjc:group", "mjc:condim", "mjc:geomContype", "mjc:geomConaffinity", "mjc:priority")
for _name in ("contactMargin", "contactGap", "torsionalFriction", "rollingFriction", "mimicCoef0",
              "mimicCoef1", "kp", "kd", "ki", "integralMax", "constEffort", "maxEffort",
              "maxMotorEffort", "saturationEffort", "velocityLimit"):
    _types("float", "newton:" + _name)
_types("float[]", "newton:lookupPositions", "newton:lookupEfforts")
_types("asset", "newton:modelPath")
_types("int[]", "mjc:path:indices", "mjc:path:segments", "mjc:sideSites:indices")
for _name in ("damping", "stiffness", "armature", "frictionloss", "margin", "gap", "solmix",
              "width", "cutoff", "noise", "torsionalfriction", "rollingfriction", "timestep"):
    _types("double", "mjc:" + _name)
for _name in ("solref", "solimp", "geomFriction", "geomSize", "gainPrm", "biasPrm", "gear",
              "lengthRange", "ctrlRange", "forceRange", "springlength", "qpos", "qvel", "act",
              "ctrl", "mpos", "mquat", "path:coef", "path:divisors"):
    _types("double[]", "mjc:" + _name)
for _name in ("timestep", "impratio", "density", "viscosity", "o_margin", "tolerance", "ls_tolerance",
              "noslip_tolerance", "ccd_tolerance"):
    _types("double", "mjc:option:" + _name)
for _name in ("iterations", "ls_iterations", "noslip_iterations", "ccd_iterations", "sdf_iterations", "sdf_initpoints"):
    _types("int", "mjc:option:" + _name)
for _name in ("integrator", "cone", "jacobian", "solver"):
    _types("token", "mjc:option:" + _name)


# UsdPhysics, MuJoCo (mjc:*) and Newton (newton:*) names, types and variability
# come from the same generated native C11 definitions used by validation (the
# engine rows are versioned in doc/generated/engine-physics-schemas.json). The
# table above only types legacy extension properties outside those schemas.
from . import _core as _native_core
_SCHEMA_DEFINITIONS = _native_core._physics_schema_definitions()
_SCHEMA_PROVENANCE = json.loads(_native_core._physics_schema_provenance())
_STANDARD_PROPERTIES = {}
_MULTIPLE_SCHEMAS = []
for _schema in _SCHEMA_DEFINITIONS.values():
    if _schema["kind"] == "multipleApply":
        _MULTIPLE_SCHEMAS.append(_schema)
    else:
        _STANDARD_PROPERTIES.update(_schema["properties"])
for _name, _property in _STANDARD_PROPERTIES.items():
    if not _property["relationship"]:
        _TYPES[_name] = _property["type"]


def schema_definitions():
    """Return immutable generated UsdPhysics, MuJoCo and Newton descriptors.

    Definitions list local properties, parent schema, multiple-apply namespace,
    native source, types, variability, relationships and allowed token values.
    ``source`` identifies the originating schema file; ``provenance`` contains
    the upstream project, pinned version and commit for each schema.
    """
    definitions = _native_core._physics_schema_definitions()
    for name, definition in definitions.items():
        if name.startswith("Mjc"):
            project = "mujoco"
            provenance = _SCHEMA_PROVENANCE["engines"][project]
        elif name.startswith("Newton"):
            project = "newton-usd-schemas"
            provenance = _SCHEMA_PROVENANCE["engines"][project]
        else:
            project = "openusd"
            provenance = _SCHEMA_PROVENANCE[project]
        definition["provenance"] = {"project": project, **provenance}
    return _freeze(definitions)


def _property_definition(name):
    definition = _STANDARD_PROPERTIES.get(name)
    if definition is not None:
        return definition
    for schema in _MULTIPLE_SCHEMAS:
        prefix = schema["property_namespace"] + ":"
        if not name.startswith(prefix):
            continue
        for suffix, definition in schema["properties"].items():
            suffix = ":" + suffix
            if name.endswith(suffix) and len(name) > len(prefix) + len(suffix):
                return definition
    return None


def _prepare(attributes, relationships):
    result = []
    for name, value in (attributes or {}).items():
        definition = _property_definition(name)
        type_name = definition["type"] if definition and not definition["relationship"] else _TYPES.get(name)
        if isinstance(value, tuple) and len(value) == 2 and isinstance(value[0], str):
            type_name, value = value
        if type_name is None:
            raise ValueError(f"unknown property type for {name!r}; pass (type_name, value)")
        uniform = definition["variability"] == "uniform" if definition else name.startswith(("newton:", "mjc:"))
        result.append((name, value, type_name, uniform))
    targets = {}
    for name, paths in (relationships or {}).items():
        if isinstance(paths, str): paths = (paths,)
        paths = tuple(paths)
        if not all(isinstance(p, str) and p.startswith("/") for p in paths):
            raise ValueError("physics relationship targets must be absolute prim paths")
        targets[name] = paths
    return result, targets


_SCHEMAS = frozenset(name for name in _SCHEMA_DEFINITIONS if name.endswith("API"))


def _check_schema(name):
    if not isinstance(name, str) or name.split(":", 1)[0] not in _SCHEMAS:
        raise ValueError(f"unknown physics API schema: {name!r}")
    base, _, instance = name.partition(":")
    multiple = _SCHEMA_DEFINITIONS.get(base, {}).get("kind") == "multipleApply"
    if multiple != bool(instance) or (not multiple and ":" in name):
        raise ValueError(f"invalid physics API schema instance: {name!r}")


def apply_schema(prim, schema_name: str, *, attributes=None, relationships=None):
    """Apply a physics API schema, then author typed properties and targets.

    Property names are canonical USD names. Supply ``(type_name, value)`` for
    an extension property without a built-in type. Existing APIs are kept.
    """
    _check_schema(schema_name)
    values, targets = _prepare(attributes, relationships)
    schemas = list(prim.metadata("apiSchemas") or ())
    if schema_name not in schemas: schemas.append(schema_name)
    prim.set_metadata("apiSchemas", schemas)
    for name, value, type_name, uniform in values:
        prim.set(name, value, type=type_name, uniform=uniform)
    for name, paths in targets.items(): prim.add_relationship(name, paths)
    return prim


def define_prim(stage, path: str, type_name: str, *, api_schemas=(), attributes=None, relationships=None):
    """Define a physics prim with canonical USD property types and targets."""
    if not isinstance(type_name, str) or not type_name.startswith(("Physics", "Mjc", "Newton")):
        raise ValueError("expected a Physics, Mjc or Newton prim type")
    for schema in api_schemas: _check_schema(schema)
    values, targets = _prepare(attributes, relationships)
    prim = stage.define_prim(path, type_name)
    if api_schemas: prim.set_metadata("apiSchemas", list(api_schemas))
    for name, value, type_name, uniform in values: prim.set(name, value, type=type_name, uniform=uniform)
    for name, paths in targets.items(): prim.add_relationship(name, paths)
    return prim
