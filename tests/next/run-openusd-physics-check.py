#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate LightUSD MJCF/URDF imports with OpenUSD's own tools.

For each input and output profile, the converted stage is saved as USDA and
reopened with OpenUSD (``usd-core``). Every registered ``UsdValidation``
validator runs on it (``usdPhysicsValidators:*``, geometry, shading and stage
metadata checks), and ``UsdPhysics.LoadUsdPhysicsFromRange`` reports what the
standard UsdPhysics parser (used by Newton and Omniverse) recovers.

Optional: requires ``pxr`` and an isolated ``lightusd`` build on PYTHONPATH::

    PYTHONPATH=$ISOLATED_PYTHON_PACKAGE $OPENUSD_PYTHON \\
      tests/next/run-openusd-physics-check.py robot.xml other.urdf

Exits nonzero if any validator reports an issue.
"""
import argparse
import collections
import pathlib
import sys
import tempfile
import warnings


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", type=pathlib.Path)
    ap.add_argument("--profile", action="append", default=[])
    ap.add_argument("--layout", default="flat", choices=("flat", "nested"))
    ap.add_argument("--require-kind", action="append", default=[], metavar="KIND=COUNT",
                    help="require at least COUNT parsed records of KIND in each result")
    args = ap.parse_args()
    profiles = args.profile or ["legacy", "newton", "openusd"]
    required_kinds = {}
    for item in args.require_kind:
        name, separator, count = item.partition("=")
        if not separator or not name or not count.isascii() or not count.isdecimal():
            ap.error("--require-kind expects KIND=COUNT with a nonnegative integer")
        required_kinds[name] = int(count)

    warnings.filterwarnings("ignore")
    from pxr import Usd, UsdPhysics, UsdValidation
    import lightusd

    validators = list(UsdValidation.ValidationRegistry().GetOrLoadAllValidators())
    names = {str(validator.GetMetadata().name) for validator in validators}
    required = {"usdPhysicsValidators:RigidBodyChecker",
                "usdPhysicsValidators:PhysicsJointChecker",
                "usdPhysicsValidators:ColliderChecker",
                "usdPhysicsValidators:ArticulationChecker"}
    missing = required - names
    if missing:
        print("OpenUSD physics validators unavailable: " + ", ".join(sorted(missing)),
              file=sys.stderr)
        return 2
    print(f"OpenUSD {'.'.join(map(str, Usd.GetVersion()))}; "
          f"{len(validators)} validators including {len(required)} physics validators")
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for source in args.inputs:
            fmt = "urdf" if source.suffix.lower() == ".urdf" else "mjcf"
            for profile in profiles:
                path = pathlib.Path(tmp) / f"{source.stem}.{profile}.usda"
                lightusd._import(path=str(source), format=fmt, profile=profile, layout=args.layout).save(str(path))
                stage = Usd.Stage.Open(str(path))
                if not stage:
                    print(f"{source.name} [{profile}]: OpenUSD could not open converted stage",
                          file=sys.stderr)
                    failures += 1
                    continue
                issues = UsdValidation.ValidationContext(validators).Validate(stage)
                parsed = UsdPhysics.LoadUsdPhysicsFromRange(stage, ["/"])
                kinds = {str(k).split(".")[-1]: len(v[0]) for k, v in parsed.items()}
                counts = dict(collections.Counter(e.GetName() for e in issues))
                failures += len(issues)
                print(f"{source.name} [{profile}] issues={counts} usdPhysics={kinds}")
                for kind, expected in required_kinds.items():
                    if kinds.get(kind, 0) < expected:
                        failures += 1
                        print(f"    missing {kind}: expected at least {expected}, "
                              f"got {kinds.get(kind, 0)}", file=sys.stderr)
                for issue in list(issues)[:5]:
                    print(f"    {issue.GetName()}: {issue.GetMessage()}")
    print(f"{len(validators)} validators; {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
