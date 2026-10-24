#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare Newton's direct MJCF/URDF import with Newton's import of LightUSD output.

For each MJCF (or, with ``--urdf DIR`` / ``*.urdf`` inputs, URDF) file this
builds two Newton models in the same process:

* reference: ``ModelBuilder.add_mjcf(xml)`` / ``add_urdf(urdf)`` (Newton's
  own importers)
* candidate: ``lightusd.import_mjcf/import_urdf(path, profile=P)`` saved as
  USD, then ``ModelBuilder.add_usd(..., schema_resolvers=[Mjc, Newton])``

URDF differences that follow urdf-usd-converter's documented rules rather than
add_urdf's are arbitrated from the URDF itself and recorded under
``reference_differs``: collapsed ghost / dummy-inertia links (masses merged
through Newton's rest poses), 3-DOF planar joints, unlimited continuous
joints, links outside the first tree, a missing ``<inertia>`` element,
per-object OBJ colliders and package:// meshes add_urdf cannot resolve.
urdf-usd-converter's ``error_*.urdf`` fixtures count as ``rejected`` when
LightUSD refuses them.

When ``mujoco`` is importable its compiled body masses arbitrate mass
differences: a body where LightUSD matches MuJoCo but Newton's own MJCF
importer does not is recorded under ``reference_differs``, not as a mismatch.

and compares structure and physical parameters: body/joint/DOF/collision
shape/site counts, per-body mass, COM and inertia, per-DOF limits, armature,
damping and friction, and per-shape sliding friction. This is a schema
consumption check, not a dynamics-equivalence claim.

Optional: requires ``newton`` (with ``SolverMuJoCo``), ``warp`` and
``usd-core`` in the selected interpreter, plus an isolated ``lightusd`` build
on ``PYTHONPATH``. Example::

    PYTHONPATH=$ISOLATED_PYTHON_PACKAGE $NEWTON_PYTHON \\
      tests/next/run-newton-compare.py --menagerie $MENAGERIE_ROOT \\
      --profile newton --profile legacy --out build_ninja/newton-compare.json
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
import tempfile
import time
import traceback
import warnings


def _model_from_mjcf(newton, solver, path):
    builder = newton.ModelBuilder()
    solver.register_custom_attributes(builder)
    builder.add_mjcf(str(path), collapse_fixed_joints=False,
                     enable_self_collisions=False)
    return builder.finalize(device="cpu")


def _model_from_urdf(newton, solver, path):
    builder = newton.ModelBuilder()
    solver.register_custom_attributes(builder)
    builder.add_urdf(str(path), collapse_fixed_joints=False,
                     enable_self_collisions=False)
    return builder.finalize(device="cpu")


def _cpu_sdf_fallback(path, notes):
    """Newton builds NewtonSDFCollisionAPI SDFs on CUDA only; without a CUDA
    device, compare those colliders as the meshes the SDF is built from."""
    import warp as wp
    if wp.get_cuda_device_count() > 0:
        return path
    from pxr import Usd
    stage = Usd.Stage.Open(str(path))
    swapped = 0
    for prim in stage.Traverse():
        if "NewtonSDFCollisionAPI" in prim.GetAppliedSchemas():
            prim.RemoveAppliedSchema("NewtonSDFCollisionAPI")
            prim.AddAppliedSchema("NewtonMeshCollisionAPI")
            swapped += 1
    if not swapped:
        return path
    out = path.with_name(path.stem + ".cpu-mesh" + path.suffix)
    stage.GetRootLayer().Export(str(out))
    notes.append(f"no CUDA device: {swapped} NewtonSDFCollisionAPI collider(s) compared as meshes")
    return out


def _model_from_usd(newton, solver, resolvers, path):
    builder = newton.ModelBuilder()
    solver.register_custom_attributes(builder)
    builder.add_usd(str(path), collapse_fixed_joints=False,
                    enable_self_collisions=False, schema_resolvers=resolvers)
    return builder.finalize(device="cpu")


def _leaf(label):
    return str(label).rstrip("/").split("/")[-1]


def _usd_name(label):
    """MJCF names as LightUSD sanitizes them into prim names."""
    import re
    name = re.sub(r"[^A-Za-z0-9_]", "_", str(label))
    return "_" + name if name[:1].isdigit() else name


def _match(reference, candidates):
    """Map Newton MJCF labels (model/worldbody/body/.../name, where MJCF names
    may themselves contain '/') to LightUSD prim names by longest suffix."""
    mapping, used = {}, set()
    # Exact names first, then longest suffixes; each candidate is used once.
    for label in reference:
        if label in candidates:
            mapping[label] = label
            used.add(label)
    for label in sorted(reference, key=len, reverse=True):
        if label in mapping:
            continue
        best = None
        for name in candidates:
            if name not in used and label.endswith("_" + name) and (best is None or len(name) > len(best)):
                best = name
        if best is not None:
            mapping[label] = best
            used.add(best)
    return mapping


def _rest_poses(model):
    """World body transforms at the initial joint coordinates (finalize()
    leaves model.body_q at identity for articulated bodies)."""
    import newton
    state = model.state()
    newton.eval_fk(model, model.joint_q, model.joint_qd, state)
    return state.body_q.numpy()


def _summary(model, np, usd):
    # USD prim leaves; MJCF labels sanitized; URDF <robot>/<link> leaves sanitized.
    name = (lambda x: _usd_name(_leaf(x))) if usd == "urdf" else _leaf if usd else _usd_name
    flags = model.shape_flags.numpy()
    body = [name(x) for x in model.body_label]
    collide = (flags & 2) != 0
    site = (flags & 8) != 0
    joint_type = model.joint_type.numpy()
    dof_dim = model.joint_dof_dim.numpy()
    joints = {}
    qd_start = model.joint_qd_start.numpy()
    for j, label in enumerate(model.joint_label):
        start = int(qd_start[j])
        ndof = int(dof_dim[j][0] + dof_dim[j][1])
        joints[name(label)] = (int(joint_type[j]), start, ndof)
    return {
        "bodies": body,
        "mass": model.body_mass.numpy(),
        "com": model.body_com.numpy(),
        "inertia": model.body_inertia.numpy(),
        "joints": joints,
        "dofs": model.joint_dof_count,
        "lower": model.joint_limit_lower.numpy(),
        "upper": model.joint_limit_upper.numpy(),
        "armature": model.joint_armature.numpy(),
        "damping": model.joint_damping.numpy() if hasattr(model, "joint_damping") else None,
        "friction": model.joint_friction.numpy(),
        "collision_shapes": int(collide.sum()),
        "sites": int(site.sum()),
        "mu": np.sort(model.shape_material_mu.numpy()[collide]),
        "body_q": _rest_poses(model),
    }


def _close(a, b, rtol=1e-4, atol=1e-6):
    import numpy as np
    return bool(np.allclose(a, b, rtol=rtol, atol=atol))


def _mujoco_masses(path):
    """MuJoCo's compiled body masses by LightUSD prim name, when available."""
    try:
        import mujoco
    except ImportError:
        return None
    try:
        model = mujoco.MjModel.from_xml_path(str(path))
    except Exception:
        return None
    masses = {}
    for b in range(1, model.nbody):
        # Unnamed bodies follow LightUSD's body_<N> numbering (world excluded).
        name = mujoco.mj_id2name(model, mujoco.mjtObj.mjOBJ_BODY, b) or f"body_{b - 1}"
        masses[_usd_name(name)] = float(model.body_mass[b])
        masses["__inertia__" + _usd_name(name)] = sorted(float(x) for x in model.body_inertia[b])
        # body_ipos is the COM in the body frame, as in Newton's body_com.
        masses["__com__" + _usd_name(name)] = [float(x) for x in model.body_ipos[b]]
    # Hinge/slide joints MuJoCo compiles as unlimited (limited="false" wins
    # over a range; Newton's add_mjcf keeps the range).
    masses["__unlimited__"] = {
        _usd_name(mujoco.mj_id2name(model, mujoco.mjtObj.mjOBJ_JOINT, j) or "")
        for j in range(model.njnt)
        if model.jnt_type[j] in (2, 3) and not model.jnt_limited[j]}
    # MuJoCo collides a geom iff contype|conaffinity is nonzero.
    masses["__colliding_geoms__"] = int(sum(1 for g in range(model.ngeom)
                                            if model.geom_contype[g] or model.geom_conaffinity[g]))
    return masses


def _urdf_oracle(path):
    """URDF facts that arbitrate add_urdf vs add_usd differences: explicit
    masses/zero tensors, and the fixed-joint children LightUSD collapses
    (urdf-usd-converter's ghost and dummy-inertia links)."""
    import xml.etree.ElementTree as ET
    root = ET.parse(str(path)).getroot()
    links = {link.get("name"): link for link in root.findall("link")}
    parent = {}
    for joint in root.findall("joint"):
        child, par = joint.find("child"), joint.find("parent")
        if child is not None and par is not None:
            parent[child.get("link")] = (par.get("link"), joint.get("type"), joint.get("name"))
    oracle = {"__urdf__": True, "__links__": {}, "__extra_colliders__": 0,
              # URDF continuous joints are unlimited even with a <limit> range.
              "__unlimited__": {_usd_name(j.get("name")) for j in root.findall("joint")
                                if j.get("type") == "continuous"}}
    # LightUSD (as urdf-usd-converter) writes one collider per OBJ object;
    # add_urdf loads each collision mesh as one shape.
    try:
        import lightusd
    except ImportError:
        lightusd = None
    for collision in root.iter("collision"):
        mesh = collision.find("geometry/mesh")
        if lightusd is None or mesh is None:
            continue
        file = mesh.get("filename", "")
        target = pathlib.Path(path).parent / file
        if file.lower().endswith(".obj") and "://" not in file and target.is_file():
            try:
                parts = sum(1 for p in lightusd.import_obj(str(target)) if p.type_name == "Mesh")
                oracle["__extra_colliders__"] += max(parts - 1, 0)
            except Exception:
                pass
    roots = [n for n in links if n not in parent]
    oracle["__extra_roots__"] = {_usd_name(n) for n in roots[1:]}  # add_urdf keeps the first tree
    oracle["__planar__"] = {_usd_name(j.get("name")) for j in root.findall("joint") if j.get("type") == "planar"}
    oracle["__no_inertia_tensor__"] = set()
    floating_massless = 0
    for joint in root.findall("joint"):
        if joint.get("type") == "floating" and joint.find("child") is not None:
            child = links.get(joint.find("child").get("link"))
            m = child.find("inertial/mass") if child is not None else None
            colliders = child is not None and child.findall("collision")
            if (m is None or float(m.get("value", "0")) <= 0) and not colliders:
                floating_massless += 1  # no rigid body mass: add_usd adds no free joint
    oracle["__floating_massless__"] = floating_massless
    # add_urdf resolves package://NAME only through an ancestor directory
    # named NAME; LightUSD (as urdf-usd-converter) also searches for the path.
    ancestors = {p.name for p in pathlib.Path(path).resolve().parents}
    oracle["__newton_unresolved__"] = sum(
        1 for c in root.iter("collision")
        if (c.find("geometry/mesh") is not None and
            c.find("geometry/mesh").get("filename", "").startswith("package://") and
            c.find("geometry/mesh").get("filename")[10:].split("/")[0] not in ancestors))
    for name, link in links.items():
        inertial = link.find("inertial")
        mass = None
        if inertial is not None and inertial.find("mass") is not None:
            mass = float(inertial.find("mass").get("value", "0"))
            oracle[_usd_name(name)] = mass
            tensor = inertial.find("inertia")
            if tensor is not None and not any(float(tensor.get(k, "0")) for k in
                                              ("ixx", "iyy", "izz", "ixy", "ixz", "iyz")):
                oracle["__inertia__" + _usd_name(name)] = [0.0, 0.0, 0.0]
            if tensor is None and mass > 0:
                oracle["__no_inertia_tensor__"].add(_usd_name(name))
        oracle["__links__"][_usd_name(name)] = {
            "parent": _usd_name(parent[name][0]) if name in parent else None,
            "fixed": name in parent and parent[name][1] == "fixed",
            "joint": _usd_name(parent[name][2]) if name in parent else None,
            "mass": mass,
            "zero_inertial": mass == 0 and ("__inertia__" + _usd_name(name)) in oracle,
            "shapes": bool(link.findall("visual") or link.findall("collision")),
        }
    return oracle


def _urdf_collapse(ref, cand, np, oracle, notes):
    """Drop the reference bodies LightUSD collapsed (fixed-joint children that
    are ghost or inertial-only links) and fold their mass properties into the
    nearest kept ancestor, using Newton's rest poses."""
    links = oracle["__links__"]
    names = list(ref["bodies"])
    kept = set(cand["bodies"])
    # Fixed-joint ghost/inertial-only children, and a ghost root (its child
    # is welded to the world instead).
    dropped = [n for n in names if n not in kept and n in links and
               ((links[n]["fixed"] and not (links[n]["shapes"] and links[n]["mass"])) or
                (links[n]["parent"] is None and (links[n]["mass"] is None or links[n]["zero_inertial"])
                 and not links[n]["shapes"]))]
    if not dropped:
        return ref
    index = {n: i for i, n in enumerate(names)}

    def frame(i):
        q = ref["body_q"][i]
        x, y, z, w = q[3], q[4], q[5], q[6]
        rot = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])
        return rot, np.asarray(q[:3], dtype=float)

    mass = np.array(ref["mass"], dtype=float)
    com = np.array(ref["com"], dtype=float)
    inertia = np.array(ref["inertia"], dtype=float)
    merged = {}
    for name in dropped:
        target = links[name]["parent"]
        while target in dropped:
            target = links[target]["parent"]
        if target is None or target not in index:
            continue
        merged.setdefault(target, []).append(name)
    for target, children in merged.items():
        t = index[target]
        # A target without its own <inertial> is massless in URDF; add_urdf
        # derived its mass from colliders instead.
        own = links.get(target, {}).get("mass") is not None
        parts = ([t] if own else []) + [index[c] for c in children]
        rt, pt = frame(t)
        total = sum(mass[i] for i in parts)
        if total <= 0:
            continue
        world_coms = {i: frame(i)[0] @ com[i] + frame(i)[1] for i in parts}
        c = sum(mass[i] * world_coms[i] for i in parts) / total
        tensor = np.zeros((3, 3))
        for i in parts:
            ri, _ = frame(i)
            d = world_coms[i] - c
            tensor += ri @ inertia[i] @ ri.T + mass[i] * (np.dot(d, d) * np.eye(3) - np.outer(d, d))
        mass[t] = total
        com[t] = rt.T @ (c - pt)
        inertia[t] = rt.T @ tensor @ rt
        if notes is not None:
            notes.append(f"mass[{target}]: merged {', '.join(children)}"
                         + ("" if own else " (no <inertial>; add_urdf derived it from colliders)"))
    keep = [i for i, n in enumerate(names) if n not in dropped]
    # A dropped ghost root takes add_urdf's own base joint ("fixed_base") with it;
    # its child joints attach to the world in LightUSD's output.
    roots = {d for d in dropped if links[d]["parent"] is None}
    joints = {n: v for n, v in ref["joints"].items()
              if not any(links[d]["joint"] == n for d in dropped) and
              not (roots and n == "fixed_base")}
    if notes is not None:
        notes.append(f"collapsed links (urdf-usd-converter rules): {len(dropped)}")
    return dict(ref, bodies=[names[i] for i in keep], mass=mass[keep], com=com[keep],
                inertia=inertia[keep], body_q=ref["body_q"][keep], joints=joints)


def compare(ref, cand, np, oracle=None, notes=None):
    if oracle and oracle.get("__urdf__"):
        ref = _urdf_collapse(ref, cand, np, oracle, notes)
        extra = [n for n in cand["bodies"] if n in oracle["__extra_roots__"]]
        if extra:
            # add_urdf drops links outside the first tree; LightUSD welds each
            # extra root to the world.
            keep = [i for i, n in enumerate(cand["bodies"]) if n not in extra]
            cand = dict(cand, bodies=[cand["bodies"][i] for i in keep], mass=cand["mass"][keep],
                        com=cand["com"][keep], inertia=cand["inertia"][keep],
                        joints={n: v for n, v in cand["joints"].items()
                                if n not in {f"__world_to_{e}" for e in extra}})
            if notes is not None:
                notes.append(f"extra root links (outside add_urdf's tree): {', '.join(extra)}")
        planar = [n for n in oracle["__planar__"] if n in cand["joints"]]
        floating = oracle["__floating_massless__"]
        if planar or floating:
            free = [n for n, v in ref["joints"].items() if v[0] == 4][:floating]
            ref = dict(ref, dofs=ref["dofs"] + len(planar) - 6 * len(free),
                       joints={n: v for n, v in ref["joints"].items() if n not in free})
            if notes is not None and planar:
                notes.append(f"planar joints: 3 DOF (urdf-usd-converter), add_urdf 2: {', '.join(planar)}")
            if notes is not None and floating:
                notes.append(f"{floating} massless floating joint(s): no free joint in USD (as urdf-usd-converter)")
    issues = []
    for key in ("dofs", "collision_shapes", "sites"):
        extra = (oracle or {}).get("__extra_colliders__", 0) + (oracle or {}).get("__newton_unresolved__", 0)
        if key == "collision_shapes" and extra and ref[key] + extra == cand[key]:
            if notes is not None:
                notes.append(f"collision_shapes: {extra} extra (multi-object OBJ meshes, "
                             "package:// meshes add_urdf cannot resolve)")
            continue
        if ref[key] != cand[key]:
            if (key == "collision_shapes" and oracle and
                    oracle.get("__colliding_geoms__") == cand[key]):
                if notes is not None:
                    notes.append(f"collision_shapes: mujoco={cand[key]} newton-mjcf={ref[key]}")
                continue
            issues.append(f"{key}: mjcf={ref[key]} usd={cand[key]}")
    if len(ref["bodies"]) != len(cand["bodies"]):
        issues.append(f"bodies: mjcf={len(ref['bodies'])} usd={len(cand['bodies'])}")
    index = {name: i for i, name in enumerate(cand["bodies"])}
    body_map = _match(ref["bodies"], cand["bodies"])
    for i, name in enumerate(ref["bodies"]):
        j = index.get(body_map.get(name, name))
        if j is None:
            continue
        if not _close(ref["mass"][i], cand["mass"][j]):
            truth = (oracle or {}).get(cand["bodies"][j])
            if truth is not None and _close(truth, cand["mass"][j], rtol=1e-3):
                # LightUSD matches MuJoCo's compiler; Newton's add_mjcf
                # approximates this body differently (e.g. mesh-fit geoms).
                if notes is not None:
                    notes.append(f"mass[{name}]: mujoco={truth:.6g} usd={cand['mass'][j]:.6g} "
                                 f"newton-mjcf={ref['mass'][i]:.6g}")
                continue
            issues.append(f"mass[{name}]: {ref['mass'][i]:.6g} vs {cand['mass'][j]:.6g}")
        body = cand["bodies"][j]
        if not _close(ref["com"][i], cand["com"][j], atol=1e-5):
            truth = (oracle or {}).get("__com__" + body)
            if truth is not None and _close(truth, cand["com"][j], atol=1e-5):
                if notes is not None:
                    notes.append(f"com[{name}] matches mujoco, not newton-mjcf")
            else:
                issues.append(f"com[{name}]")
        # float32 tensors (and URDF merges through float32 rest poses): an
        # absolute tolerance relative to the tensor's size.
        scale = float(np.abs(np.asarray(ref["inertia"][i])).max())
        if not _close(ref["inertia"][i], cand["inertia"][j], rtol=1e-3, atol=max(1e-7, 1e-6 * scale)):
            truth = (oracle or {}).get("__inertia__" + body)
            moments = sorted(np.linalg.eigvalsh(np.asarray(cand["inertia"][j], dtype=float)))
            if truth is not None and _close(truth, moments, rtol=2e-3, atol=1e-8):
                if notes is not None:
                    notes.append(f"inertia[{name}] matches mujoco, not newton-mjcf")
            elif body in (oracle or {}).get("__no_inertia_tensor__", ()) and \
                    np.abs(np.asarray(ref["inertia"][i])).max() <= 1.1e-6:  # 0, or finalize's floor
                # add_urdf takes a missing <inertia> as zero; LightUSD (as
                # urdf-usd-converter) leaves it unauthored for the consumer.
                if notes is not None:
                    notes.append(f"inertia[{name}]: no <inertia> element (add_urdf uses zero)")
            elif ((oracle or {}).get(body) == 0.0 and 0.0 < cand["mass"][j] <= 1e-9 and
                  max(moments) <= 1.1e-6 and not np.asarray(truth or [0.0]).any()):
                # An explicit MJCF mass 0 is authored as the smallest positive
                # mass (UsdPhysics reads 0 as unspecified); Newton's finalize
                # then raises its inertia to a 1e-6 floor that only an exact
                # zero mass escapes.
                if notes is not None:
                    notes.append(f"inertia[{name}]: explicit zero mass, Newton finalize inertia floor")
            else:
                issues.append(f"inertia[{name}]")
    # Free joints are implicit in USD (an unjointed body) and fixed joints of
    # jointless MJCF bodies are synthesized; both importers label them
    # differently, so compare those by count, not name.
    for kind, jtype in (("free", 4), ("fixed", 3)):  # newton.JointType
        a = sum(1 for v in ref["joints"].values() if v[0] == jtype)
        b = sum(1 for v in cand["joints"].values() if v[0] == jtype)
        if a != b:
            issues.append(f"{kind} joints: mjcf={a} usd={b}")
    ref = dict(ref, joints={n: v for n, v in ref["joints"].items() if v[0] not in (3, 4)})
    # Newton names unnamed MJCF joints (equality loop closures) joint_<N>;
    # compare those by type count instead of name.
    import re
    unnamed = {n for n in ref["joints"] if re.search(r"(^|_)joint_\d+$", n) and n not in cand["joints"]}
    if unnamed:
        types = sorted(ref["joints"][n][0] for n in unnamed)
        named = {n: v for n, v in ref["joints"].items() if n not in unnamed}
        matched = set(_match(named, cand["joints"]).values()) | (set(named) & set(cand["joints"]))
        spare = sorted(v[0] for n, v in cand["joints"].items() if n not in matched and v[0] not in (3, 4))
        if sorted(types) != sorted(spare):
            issues.append(f"unnamed joints: mjcf types={types} usd spare types={spare}")
        ref = dict(ref, joints={n: v for n, v in ref["joints"].items() if n not in unnamed})
    joint_map = _match(ref["joints"], cand["joints"])
    # Newton merges stacked joints into one D6 labeled by all member names;
    # LightUSD's stacked joints merge under the first joint's name.
    used = set(joint_map.values())
    for label, value in ref["joints"].items():
        if label in joint_map or value[0] != 6:
            continue
        for name, other in cand["joints"].items():
            if name not in used and other[0] == 6 and ("_" + name + "_") in ("_" + label + "_"):
                used.add(name)
                joint_map[label] = name
                break
    ref = dict(ref, joints={joint_map.get(n, n): v for n, v in ref["joints"].items()})
    missing = sorted(set(ref["joints"]) - set(cand["joints"]))
    if missing:
        issues.append("joints missing: " + ",".join(missing[:8]))
    per_dof = ("lower", "upper", "armature", "damping", "friction")
    for name, (jtype, start, ndof) in ref["joints"].items():
        other = cand["joints"].get(name)
        if other is None:
            continue
        if other[0] != jtype or other[2] != ndof:
            if name in (oracle or {}).get("__planar__", ()) and jtype == other[0] == 6 and \
                    (ndof, other[2]) == (2, 3):
                continue
            issues.append(f"joint[{name}] type/dofs {jtype}/{ndof} vs {other[0]}/{other[2]}")
            continue
        diffs, permuted = [], True
        unlimited_in_mujoco = name in (oracle or {}).get("__unlimited__", ())
        for key in per_dof:
            if ref[key] is None or cand[key] is None:
                continue
            a = ref[key][start:start + ndof]
            b = cand[key][other[1]:other[1] + ndof]
            finite = np.isfinite(a) & np.isfinite(b)
            if (np.isfinite(a) != np.isfinite(b)).any() or not _close(a[finite], b[finite], atol=1e-5):
                if (key in ("lower", "upper") and unlimited_in_mujoco and
                        bool((~np.isfinite(b) | (np.abs(b) >= 1e9)).all())):
                    if notes is not None:
                        notes.append(f"{key}[{name}] unlimited in the source (MJCF limited=\"false\" / URDF continuous), not in Newton's importer")
                    continue
                diffs.append(f"{key}[{name}]: {a.tolist()} vs {b.tolist()}")
                sa, sb = np.sort(a), np.sort(b)
                permuted &= bool((np.isfinite(sa) == np.isfinite(sb)).all() and
                                 _close(sa[np.isfinite(sa)], sb[np.isfinite(sb)], atol=1e-5))
        if diffs and jtype == 6 and permuted:
            # add_usd orders a merged D6 by sorted joint path, not prim order.
            if notes is not None:
                notes.append(f"dof_order[{name}]")
        else:
            issues.extend(diffs)
    if len(ref["mu"]) == len(cand["mu"]) and not _close(ref["mu"], cand["mu"]):
        issues.append("shape friction (sorted mu) differs")
    return issues


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", type=pathlib.Path)
    ap.add_argument("--menagerie", type=pathlib.Path,
                    help="test every <robot>/scene*.xml or top-level model XML")
    ap.add_argument("--urdf", type=pathlib.Path, action="append", default=[],
                    help="test every *.urdf below this directory (Newton add_urdf as reference)")
    ap.add_argument("--profile", action="append", default=[])
    ap.add_argument("--layout", default="flat", choices=("flat", "nested"))
    ap.add_argument("--format", default="usda", choices=("usda", "usdc"))
    ap.add_argument("--out", type=pathlib.Path)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    profiles = args.profile or ["newton"]

    warnings.filterwarnings("ignore")
    import numpy as np
    import warp as wp
    wp.config.quiet = True
    import newton
    from newton.solvers import SolverMuJoCo
    from newton.usd import SchemaResolverMjc, SchemaResolverNewton
    import lightusd

    inputs = list(args.inputs)
    if args.menagerie:
        for robot in sorted(p for p in args.menagerie.iterdir() if p.is_dir()):
            candidates = sorted(robot.glob("*.xml"))
            # Skip include fragments (keyframes, assets): a model has a body tree.
            models = [p for p in candidates if not p.name.startswith("scene")
                      and b"<worldbody" in p.read_bytes()]
            inputs.extend(models[:1] or candidates[:1])
    for root in args.urdf:
        inputs.extend(sorted(root.rglob("*.urdf")))
    if args.limit:
        inputs = inputs[:args.limit]

    resolvers = [SchemaResolverMjc(), SchemaResolverNewton()]
    results = []
    totals = {p: {"pass": 0, "mismatch": 0, "error": 0, "rejected": 0} for p in profiles}
    with tempfile.TemporaryDirectory() as tmp:
        for path in inputs:
            entry = {"input": str(path), "profiles": {}}
            urdf = path.suffix.lower() == ".urdf"
            try:
                load = _model_from_urdf if urdf else _model_from_mjcf
                # Newton labels URDF bodies <robot>/<link>: compare link names.
                ref = _summary(load(newton, SolverMuJoCo, path), np, "urdf" if urdf else False)
            except Exception as exc:  # reference importer limitation
                entry["reference_error"] = f"{type(exc).__name__}: {exc}"
                results.append(entry)
                print(f"SKIP {path}: Newton add_{'urdf' if urdf else 'mjcf'} failed: {exc}", flush=True)
                continue
            # MuJoCo arbitrates MJCF differences only (its URDF compiler is
            # not the URDF semantics Newton or LightUSD follow).
            oracle = _urdf_oracle(path) if urdf else _mujoco_masses(path)
            for profile in profiles:
                started = time.perf_counter()
                record = {}
                try:
                    if urdf:
                        # add_urdf skips missing meshes; match it.
                        stage = lightusd.import_urdf(path, profile=profile, layout=args.layout,
                                                     allow_missing=True)
                    else:
                        stage = lightusd.import_mjcf(path, profile=profile, layout=args.layout)
                    out = pathlib.Path(tmp) / f"{len(results)}-{path.stem}.{profile}.{args.format}"  # unique: USD caches layers by path
                    stage.save(str(out))
                    notes = []
                    out = _cpu_sdf_fallback(out, notes)
                    cand = _summary(_model_from_usd(newton, SolverMuJoCo, resolvers, out), np, True)
                    issues = compare(ref, cand, np, oracle, notes)
                    record["issues"] = issues
                    if notes:
                        record["reference_differs"] = notes
                    record["status"] = "pass" if not issues else "mismatch"
                except Exception as exc:
                    record["status"] = "error"
                    record["error"] = f"{type(exc).__name__}: {exc}"
                    record["trace"] = traceback.format_exc(limit=3)
                    # urdf-usd-converter's invalid-input fixtures (error_*.urdf):
                    # rejecting them is the expected result.
                    if urdf and path.name.startswith("error_") and type(exc).__name__ == "UsdParseError":
                        record["status"] = "rejected"
                record["seconds"] = round(time.perf_counter() - started, 3)
                totals[profile][record["status"]] += 1
                entry["profiles"][profile] = record
                detail = "; ".join(record.get("issues", [])[:4]) or record.get("error", "")
                print(f"{record['status'].upper():8s} {profile:8s} {path}  {detail}", flush=True)
            results.append(entry)
    report = {"versions": {"newton": newton.__version__, "warp": wp.__version__},
              "totals": totals, "results": results}
    print(json.dumps(totals))
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
