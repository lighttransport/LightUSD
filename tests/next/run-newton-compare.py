#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare Newton's direct MJCF import with Newton's import of LightUSD output.

For each MJCF file this builds two Newton models in the same process:

* reference: ``ModelBuilder.add_mjcf(xml)`` (Newton's own MJCF importer)
* candidate: ``lightusd.import_mjcf(xml, profile=P)`` saved as USD, then
  ``ModelBuilder.add_usd(..., schema_resolvers=[Mjc, Newton])``

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


def _summary(model, np, usd):
    name = _leaf if usd else _usd_name
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
    # MuJoCo collides a geom iff contype|conaffinity is nonzero.
    masses["__colliding_geoms__"] = int(sum(1 for g in range(model.ngeom)
                                            if model.geom_contype[g] or model.geom_conaffinity[g]))
    return masses


def compare(ref, cand, np, oracle=None, notes=None):
    issues = []
    for key in ("dofs", "collision_shapes", "sites"):
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
        if not _close(ref["inertia"][i], cand["inertia"][j], rtol=1e-3, atol=1e-7):
            truth = (oracle or {}).get("__inertia__" + body)
            moments = sorted(np.linalg.eigvalsh(np.asarray(cand["inertia"][j], dtype=float)))
            if truth is not None and _close(truth, moments, rtol=2e-3, atol=1e-8):
                if notes is not None:
                    notes.append(f"inertia[{name}] matches mujoco, not newton-mjcf")
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
            issues.append(f"joint[{name}] type/dofs {jtype}/{ndof} vs {other[0]}/{other[2]}")
            continue
        diffs, permuted = [], True
        for key in per_dof:
            if ref[key] is None or cand[key] is None:
                continue
            a = ref[key][start:start + ndof]
            b = cand[key][other[1]:other[1] + ndof]
            finite = np.isfinite(a) & np.isfinite(b)
            if (np.isfinite(a) != np.isfinite(b)).any() or not _close(a[finite], b[finite], atol=1e-5):
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
    ap.add_argument("--profile", action="append", default=[])
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
    if args.limit:
        inputs = inputs[:args.limit]

    resolvers = [SchemaResolverMjc(), SchemaResolverNewton()]
    results = []
    totals = {p: {"pass": 0, "mismatch": 0, "error": 0} for p in profiles}
    with tempfile.TemporaryDirectory() as tmp:
        for path in inputs:
            entry = {"input": str(path), "profiles": {}}
            try:
                ref = _summary(_model_from_mjcf(newton, SolverMuJoCo, path), np, False)
            except Exception as exc:  # reference importer limitation
                entry["reference_error"] = f"{type(exc).__name__}: {exc}"
                results.append(entry)
                print(f"SKIP {path}: Newton add_mjcf failed: {exc}", flush=True)
                continue
            oracle = _mujoco_masses(path)
            for profile in profiles:
                started = time.perf_counter()
                record = {}
                try:
                    stage = lightusd.import_mjcf(path, profile=profile)
                    out = pathlib.Path(tmp) / f"{len(results)}-{path.stem}.{profile}.{args.format}"  # unique: USD caches layers by path
                    stage.save(str(out))
                    cand = _summary(_model_from_usd(newton, SolverMuJoCo, resolvers, out), np, True)
                    notes = []
                    issues = compare(ref, cand, np, oracle, notes)
                    record["issues"] = issues
                    if notes:
                        record["reference_differs"] = notes
                    record["status"] = "pass" if not issues else "mismatch"
                except Exception as exc:
                    record["status"] = "error"
                    record["error"] = f"{type(exc).__name__}: {exc}"
                    record["trace"] = traceback.format_exc(limit=3)
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
