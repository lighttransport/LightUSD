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
    args = ap.parse_args()
    profiles = args.profile or ["legacy", "newton", "openusd"]

    warnings.filterwarnings("ignore")
    from pxr import Usd, UsdPhysics, UsdValidation
    import lightusd

    validators = list(UsdValidation.ValidationRegistry().GetOrLoadAllValidators())
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for source in args.inputs:
            fmt = "urdf" if source.suffix.lower() == ".urdf" else "mjcf"
            for profile in profiles:
                path = pathlib.Path(tmp) / f"{source.stem}.{profile}.usda"
                lightusd._import(path=str(source), format=fmt, profile=profile, layout=args.layout).save(str(path))
                stage = Usd.Stage.Open(str(path))
                issues = UsdValidation.ValidationContext(validators).Validate(stage)
                parsed = UsdPhysics.LoadUsdPhysicsFromRange(stage, ["/"])
                kinds = {str(k).split(".")[-1]: len(v[0]) for k, v in parsed.items()}
                counts = dict(collections.Counter(e.GetName() for e in issues))
                failures += len(issues)
                print(f"{source.name} [{profile}] issues={counts} usdPhysics={kinds}")
                for issue in list(issues)[:5]:
                    print(f"    {issue.GetName()}: {issue.GetMessage()}")
    print(f"{len(validators)} validators; {failures} issues")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
