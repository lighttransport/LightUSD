#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare MuJoCo's compile of an MJCF file with MuJoCo's compile of LightUSD's
USD for it, read back through MuJoCo's own USD decoder.

For each MJCF file:

* reference: ``mujoco-model-dump model.xml`` (MuJoCo compiles the MJCF)
* candidate: ``lightusd.import_mjcf(xml, profile=P)`` saved as USDA, then
  ``mujoco-model-dump model.usda libusd_decoder_plugin.so`` (MuJoCo's
  ``plugin/usd_decoder`` turns the UsdPhysics / MjcPhysics / Newton stage
  into an mjSpec, which MuJoCo compiles)

and compares the compiled models: element counts, options, and per named
element body mass/COM/inertia, joint type/limits/armature/damping/
frictionloss/stiffness, colliding geom contact parameters, actuator
gain/bias/gear/ranges, tendon stiffness/damping/range and equality types.
The USD decoder names elements by prim path; elements are matched by the
path's leaf, which is the (sanitized) MJCF name.

What MuJoCo 3.16's decoder cannot read back is recorded under
``decoder_limits`` instead of failing a model (see ``_decoder_limit``). Three
decoder bugs make otherwise valid stages fail or misread outright and need the
local patch in ``issues/mujoco-usd-decoder/local-patch.diff`` (newton:inertia
next to diagonalInertia, physics-purpose material bindings, site gprims
decoded as geoms). Expected residual: hello_robot_stretch_3's gripper, whose
OBJ has a 215-vertex polygon that MuJoCo's PyPI wheel drops (LightUSD follows
the wheel) while a CMake build of the same tag keeps it.

Requires a MuJoCo built with ``-DMUJOCO_WITH_USD=ON`` against a standard
OpenUSD build (pxr namespace, TBB) and installed (``cmake --install``; the
schema resources are only complete there), and
``tests/next/mujoco-model-dump.cc`` compiled against that install:

    c++ -std=c++17 -I$MJ/include tests/next/mujoco-model-dump.cc \\
        -L$MJ/lib -lmujoco -Wl,-rpath,$MJ/lib -o mujoco-model-dump
    PYTHONPATH=$ISOLATED_PYTHON_PACKAGE python3 tests/next/run-mujoco-usd-compare.py \\
        --mujoco $MJ --dump ./mujoco-model-dump --menagerie $MENAGERIE_ROOT \\
        --out build_ninja/mujoco-usd-compare.json
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time


def _usd_name(label):
    name = re.sub(r"[^A-Za-z0-9_]", "_", str(label))
    return "_" + name if name[:1].isdigit() else name


def _leaf(name):
    return str(name).rstrip("/").split("/")[-1]


def _dump(args, path, usd):
    cmd = [str(args.dump), str(path)]
    env = dict(os.environ)
    env["MUJOCO_PLUGIN_DIR"] = str(pathlib.Path(args.mujoco) / "bin" / "mujoco_plugin")
    env["LD_LIBRARY_PATH"] = os.pathsep.join([str(pathlib.Path(args.mujoco) / "lib"),
                                              env.get("LD_LIBRARY_PATH", "")])
    if usd:
        prefix = pathlib.Path(args.mujoco)
        cmd.append(str(prefix / "bin" / "mujoco_plugin" / "libusd_decoder_plugin.so"))
        resources = prefix / "lib" / "mujoco-usd-resources"
        paths = [str(p) for p in sorted(resources.iterdir()) if p.is_dir()]
        env["PXR_PLUGINPATH_NAME"] = os.pathsep.join(paths + [env.get("PXR_PLUGINPATH_NAME", "")])
        # The plugin links libmjcPhysics/OpenUSD from the install's lib/.
        env["LD_LIBRARY_PATH"] = os.pathsep.join([str(prefix / "lib"), env.get("LD_LIBRARY_PATH", "")])
    out = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=600)
    line = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else "{}"
    data = json.loads(line)
    if "error" in data:
        raise RuntimeError(data["error"])
    data["stderr"] = out.stderr[-2000:]
    return data


def _close(a, b, rtol, atol):
    if isinstance(a, list):
        return len(a) == len(b) and all(_close(x, y, rtol, atol) for x, y in zip(a, b))
    return abs(a - b) <= atol + rtol * abs(b)


def _index(items, usd):
    out = {}
    for item in items:
        name = _leaf(item["name"]) if usd else _usd_name(item["name"]) if item["name"] else ""
        if name:
            out[name] = item
    return out


# (field, rtol, atol) per element kind.
FIELDS = {
    "bodies": [("mass", 1e-4, 1e-7), ("ipos", 1e-4, 1e-6), ("gravcomp", 0, 1e-9)],
    "joints": [("type", 0, 0), ("limited", 0, 0), ("range", 1e-5, 1e-6), ("armature", 1e-5, 1e-9),
               ("damping", 1e-5, 1e-9), ("frictionloss", 1e-5, 1e-9), ("stiffness", 1e-5, 1e-9),
               ("springref", 1e-5, 1e-6)],
    "geoms": [("contype", 0, 0), ("conaffinity", 0, 0), ("condim", 0, 0), ("priority", 0, 0),
              ("friction", 1e-5, 1e-9), ("solref", 1e-5, 1e-9), ("solimp", 1e-5, 1e-9),
              ("margin", 1e-5, 1e-9), ("gap", 1e-5, 1e-9), ("solmix", 1e-5, 1e-9)],
    "actuators": [("trntype", 0, 0), ("dyntype", 0, 0), ("gaintype", 0, 0), ("biastype", 0, 0),
                  ("gainprm", 1e-5, 1e-9), ("biasprm", 1e-5, 1e-9), ("dynprm", 1e-5, 1e-9),
                  ("gear", 1e-5, 1e-9), ("ctrlrange", 1e-5, 1e-9), ("forcerange", 1e-5, 1e-9),
                  ("ctrllimited", 0, 0), ("forcelimited", 0, 0)],
    "tendons": [("num", 0, 0), ("limited", 0, 0), ("range", 1e-5, 1e-6), ("stiffness", 1e-5, 1e-9),
                ("damping", 1e-5, 1e-9)],
    "equalities": [("type", 0, 0), ("active", 0, 0)],
}
COUNTS = ("nbody", "njnt", "nu", "ntendon", "neq", "nsite", "ncolliding", "nkey")
OPTION = (("timestep", 1e-9, 0), ("iterations", 0, 0), ("integrator", 0, 0), ("cone", 0, 0),
          ("solver", 0, 0), ("gravity", 1e-6, 1e-9))


# What MuJoCo 3.16's USD decoder cannot read back, whatever the stage says.
# Differences of these kinds are recorded as notes, not issues:
# * revolute/prismatic limits are read with Get(), so an unauthored limit
#   decodes as limited with the schema's -inf..inf fallback;
# * contype/conaffinity bitmasks have no MjcPhysics attribute (colliders
#   decode as 1/1);
# * body gravcomp is not read (only joint actuatorgravcomp);
# * tree PhysicsSphericalJoints are skipped ("Unsupported joint type").
FREE, BALL = 0, 1


def _decoder_limit(kind, field, item, other):
    if kind == "joints" and field in ("limited", "range"):
        return item["limited"] == 0 and other["range"] == [-1e308, 1e308]
    if kind == "geoms" and field in ("contype", "conaffinity"):
        return other["contype"] == 1 and other["conaffinity"] == 1
    if kind == "bodies" and field == "gravcomp":
        return other["gravcomp"] == [0]
    return False


def compare(ref, cand, notes=None, keys=True):
    issues = []
    balls = [j["name"] for j in ref["joints"] if j["type"] == BALL]
    for key in COUNTS:
        if key == "nkey" and not keys:
            continue  # only <size nkey> placeholders, no authored <key>
        expected = ref["counts"].get(key)
        if key == "njnt" and balls:
            expected -= len(balls)
        if expected != cand["counts"].get(key):
            issues.append(f"{key}: mjcf={ref['counts'].get(key)} usd={cand['counts'].get(key)}")
    for key, rtol, atol in OPTION:
        if not _close(ref["option"][key], cand["option"][key], rtol, atol):
            issues.append(f"option.{key}: {ref['option'][key]} vs {cand['option'][key]}")
    for kind, fields in FIELDS.items():
        a = _index(ref[kind], False)
        b = _index(cand[kind], True)
        if kind == "geoms":  # visual-only geoms are layout, not physics
            a = {k: v for k, v in a.items() if v["contype"] or v["conaffinity"]}
            b = {k: v for k, v in b.items() if v["contype"] or v["conaffinity"]}
        missing = sorted(set(a) - set(b))
        if kind == "joints":
            skipped = [n for n in missing if a[n]["type"] == BALL]
            if skipped and notes is not None:
                notes.append(f"ball joints the decoder skips: {', '.join(skipped[:6])}")
            # A free joint is no USD prim; the decoder adds an unnamed one.
            missing = [n for n in missing if a[n]["type"] not in (BALL, FREE)]
        if kind == "equalities" and missing:
            # The decoder names a joint equality after the follower joint
            # prim: compare the unmatched ones by type.
            spare = sorted(v["type"] for k, v in b.items() if k not in a)
            if sorted(a[n]["type"] for n in missing) == spare:
                missing = []
        if missing:
            issues.append(f"{kind} missing: {', '.join(missing[:6])}")
        for name, item in a.items():
            other = b.get(name)
            if other is None:
                continue
            for field, rtol, atol in fields:
                if not _close(item[field], other[field], rtol, atol):
                    message = f"{kind}[{name}].{field}: {item[field]} vs {other[field]}"
                    if _decoder_limit(kind, field, item, other):
                        if notes is not None:
                            notes.append("decoder: " + message)
                    else:
                        issues.append(message)
            if kind == "bodies":
                ia, ib = sorted(item["inertia"]), sorted(other["inertia"])
                scale = max(abs(x) for x in ia) if ia else 0
                if not _close(ia, ib, 1e-3, max(1e-9, 1e-6 * scale)):
                    issues.append(f"bodies[{name}].inertia: {ia} vs {ib}")
    return issues


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", type=pathlib.Path)
    ap.add_argument("--menagerie", type=pathlib.Path)
    ap.add_argument("--mujoco", type=pathlib.Path, required=True,
                    help="MuJoCo install prefix (cmake --install of a MUJOCO_WITH_USD=ON build)")
    ap.add_argument("--dump", type=pathlib.Path, required=True, help="mujoco-model-dump binary")
    ap.add_argument("--profile", default="newton")
    ap.add_argument("--out", type=pathlib.Path)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    import lightusd

    inputs = list(args.inputs)
    if args.menagerie:
        for robot in sorted(p for p in args.menagerie.iterdir() if p.is_dir()):
            candidates = sorted(robot.glob("*.xml"))
            models = [p for p in candidates if not p.name.startswith("scene")
                      and b"<worldbody" in p.read_bytes()]
            inputs.extend(models[:1] or candidates[:1])
    if args.limit:
        inputs = inputs[:args.limit]
    totals = {"pass": 0, "mismatch": 0, "error": 0, "skip": 0}
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        for n, path in enumerate(inputs):
            started = time.perf_counter()
            entry = {"input": str(path)}
            try:
                ref = _dump(args, path, False)
            except Exception as exc:
                entry.update(status="skip", error=f"mjcf: {exc}")
                totals["skip"] += 1
                results.append(entry)
                print(f"SKIP     {path}  {exc}", flush=True)
                continue
            try:
                out = pathlib.Path(tmp) / f"{n}-{path.stem}.usda"
                lightusd.import_mjcf(path, profile=args.profile).save(str(out))
                cand = _dump(args, out, True)
                notes = []
                issues = compare(ref, cand, notes, b"<key" in path.read_bytes())
                entry.update(status="pass" if not issues else "mismatch", issues=issues)
                if notes:
                    entry["decoder_limits"] = sorted(set(n.split(":")[0] if n.startswith("decoder") else n
                                                         for n in notes))[:20]
                    entry["decoder_limit_count"] = len(notes)
                deprecations = sorted(set(re.findall(r"uses deprecated (mjc:[\w:]+)", cand["stderr"])))
                if deprecations:
                    entry["deprecated"] = deprecations
            except Exception as exc:
                entry.update(status="error", error=f"{type(exc).__name__}: {exc}")
            entry["seconds"] = round(time.perf_counter() - started, 3)
            totals[entry["status"]] += 1
            results.append(entry)
            detail = "; ".join(entry.get("issues", [])[:4]) or entry.get("error", "")
            print(f"{entry['status'].upper():8s} {path}  {detail}", flush=True)
    print(json.dumps(totals))
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps({"totals": totals, "results": results}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
