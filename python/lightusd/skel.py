# SPDX-License-Identifier: Apache-2.0
"""Bind imported OBJ ``vw`` skin weights to a skeleton supplied in USD.

OBJ ``vw`` rows carry joint *indices* and weights but no rig. ``bind_skin``
checks those indices against an existing ``Skeleton``, optionally remaps them,
and authors the UsdSkel binding on the mesh: ``SkelBindingAPI``,
``skel:skeleton``, an optional ``skel:joints`` subset and
``primvars:skel:geomBindTransform``. Normalization and pruning only happen when
requested; by default the source influences are kept exactly.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping, Optional, Sequence, Union

__all__ = ["SkinBinding", "bind_skin"]

_INDICES = "primvars:skel:jointIndices"
_WEIGHTS = "primvars:skel:jointWeights"


@dataclass(frozen=True)
class SkinBinding:
    """Result of :func:`bind_skin`."""

    points: int
    influences: int
    joint_count: int
    normalized_points: int
    pruned_influences: int


def _element_size(prim, name: str, count: int) -> int:
    attribute = prim.attribute(name)
    size = attribute.metadata("elementSize") if attribute is not None else None
    if size is None:
        return 1
    size = int(size)
    if size < 1 or count % size:
        raise ValueError(f"{prim.path}.{name}: elementSize {size} does not divide {count} values")
    return size


def bind_skin(stage, mesh_path: str, skeleton_path: str, *,
              joints: Optional[Sequence[str]] = None,
              joint_remap: Optional[Union[Sequence[int], Mapping[int, Union[int, str]]]] = None,
              geom_bind_transform=None, normalize: bool = False,
              max_influences: Optional[int] = None) -> SkinBinding:
    """Bind ``mesh_path``'s imported skin weights to ``skeleton_path``.

    Joint indices address the skeleton's ``joints`` order, or ``joints`` (a
    list of names from that skeleton, authored as ``skel:joints``) when given.
    ``joint_remap`` maps each source OBJ joint id to a new index, or to a
    joint name, in that order. Every resulting index must be in range;
    otherwise ``ValueError`` is raised and the stage is left unchanged.

    ``max_influences`` keeps the strongest influences per point (ties keep
    source order). ``normalize`` scales each point's weights to sum to one
    (points whose weights sum to zero are left as is).
    """
    mesh = stage.prim_at(mesh_path)
    skeleton = stage.prim_at(skeleton_path)
    if skeleton.type_name != "Skeleton":
        raise ValueError(f"{skeleton_path} is not a Skeleton")
    if _INDICES not in mesh.attributes or _WEIGHTS not in mesh.attributes:
        raise ValueError(f"{mesh_path} has no imported skin weights")
    skeleton_joints = [str(j) for j in skeleton["joints"]] if "joints" in skeleton.attributes else []
    if not skeleton_joints:
        raise ValueError(f"{skeleton_path} has no joints")
    order = list(joints) if joints is not None else skeleton_joints
    unknown = [j for j in order if j not in skeleton_joints]
    if unknown:
        raise ValueError(f"joints not in {skeleton_path}: {unknown[:5]}")
    position = {name: i for i, name in enumerate(order)}

    indices = [int(i) for i in mesh[_INDICES]]
    weights = [float(w) for w in mesh[_WEIGHTS]]
    if len(indices) != len(weights):
        raise ValueError(f"{mesh_path}: joint index and weight arrays differ in length")
    size = _element_size(mesh, _INDICES, len(indices))
    if _element_size(mesh, _WEIGHTS, len(weights)) != size:
        raise ValueError(f"{mesh_path}: joint index and weight element sizes differ")

    def remap(index: int) -> int:
        if joint_remap is None:
            target = index
        elif isinstance(joint_remap, Mapping):
            if index not in joint_remap:
                raise ValueError(f"joint id {index} has no remap entry")
            target = joint_remap[index]
        else:
            if not 0 <= index < len(joint_remap):
                raise ValueError(f"joint id {index} is outside joint_remap")
            target = joint_remap[index]
        if isinstance(target, str):
            if target not in position:
                raise ValueError(f"remapped joint {target!r} is not in the joint order")
            target = position[target]
        if not 0 <= int(target) < len(order):
            raise ValueError(f"joint index {target} is out of range for {len(order)} joints")
        return int(target)

    points = len(indices) // size
    rows = []
    for p in range(points):
        row = [(remap(indices[p * size + k]), weights[p * size + k]) for k in range(size)]
        rows.append(row)

    pruned = 0
    if max_influences is not None:
        if max_influences < 1:
            raise ValueError("max_influences must be positive")
        if max_influences < size:
            for p, row in enumerate(rows):
                ranked = sorted(range(size), key=lambda k: -abs(row[k][1]))[:max_influences]
                pruned += sum(1 for k in range(size) if k not in ranked and row[k][1] != 0.0)
                rows[p] = [row[k] for k in sorted(ranked)]
            size = max_influences
    normalized = 0
    if normalize:
        for p, row in enumerate(rows):
            total = sum(w for _, w in row)
            if total != 0.0 and abs(total - 1.0) > 1e-7:
                rows[p] = [(j, w / total) for j, w in row]
                normalized += 1

    mesh.set_metadata("apiSchemas", tuple(dict.fromkeys(
        list(mesh.metadata("apiSchemas") or ()) + ["SkelBindingAPI"])))
    mesh.set(_INDICES, [j for row in rows for j, _ in row], type="int[]")
    mesh.set(_WEIGHTS, [w for row in rows for _, w in row], type="float[]")
    for name in (_INDICES, _WEIGHTS):
        mesh.attribute(name).set_metadata("interpolation", "vertex")
        mesh.attribute(name).set_metadata("elementSize", size)
    if "skel:skeleton" in mesh.relationships:
        mesh.remove_property("skel:skeleton")
    mesh.add_relationship("skel:skeleton", [skeleton_path])
    if joints is not None:
        mesh.set("skel:joints", list(order), type="token[]", uniform=True)
    if geom_bind_transform is not None:
        mesh.set("primvars:skel:geomBindTransform",
                 tuple(tuple(float(v) for v in row) for row in geom_bind_transform),
                 type="matrix4d")
        mesh.attribute("primvars:skel:geomBindTransform").set_metadata("interpolation", "constant")
    return SkinBinding(points=points, influences=size, joint_count=len(order),
                       normalized_points=normalized, pruned_influences=pruned)
