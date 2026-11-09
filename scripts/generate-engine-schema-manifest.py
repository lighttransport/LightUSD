#!/usr/bin/env python3
"""Generate a versioned manifest of engine physics schemas (Newton, MuJoCo).

The output has the same per-schema shape as ``openusd-schema-*.json`` so
``generate-checker-definitions.py`` can compile both into the native C11
descriptor tables. LightUSD does not depend on either project at runtime.

Inputs are the published declarative schema files:

* newton-usd-schemas: ``newton_usd_schemas/generatedSchema.usda``
* MuJoCo: ``src/experimental/usd/mjcPhysics/schema.usda``

Example::

    scripts/generate-engine-schema-manifest.py \\
      --newton-schema ../newton-usd-schemas/newton_usd_schemas/generatedSchema.usda \\
      --newton-version 0.6.0 \\
      --mujoco-schema mjcPhysics/schema.usda --mujoco-version 3.16.0 \\
      --mujoco-commit a892750516336d8358949f4518fcc4fbbd85beae
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import re
import subprocess

_KINDS = {"singleApplyAPI": "singleApply", "multipleApplyAPI": "multipleApply",
          "concreteTyped": "concrete", "abstractTyped": "abstract",
          "nonAppliedAPI": "nonApplied"}


def _plug_info(path: pathlib.Path) -> dict[str, dict[str, object]]:
    """Schema kind and apply restrictions live in plugInfo.json, not .usda."""
    text = re.sub(r"^\s*#.*$", "", path.read_text(encoding="utf-8"), flags=re.M)
    info: dict[str, dict[str, object]] = {}
    for plugin in json.loads(text).get("Plugins", []):
        for cpp, entry in plugin.get("Info", {}).get("Types", {}).items():
            name = entry.get("alias", {}).get("UsdSchemaBase", cpp)
            info[name] = entry
    return info


def _openusd_parser():
    path = pathlib.Path(__file__).with_name("generate-openusd-schema-manifest.py")
    spec = importlib.util.spec_from_file_location("openusd_manifest", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _git_commit(path: pathlib.Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(path.parent), "rev-parse", "HEAD"],
            text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def _builtin_apis(text: str, name: str) -> list[str]:
    header = re.search(r'^class\s+(?:\w+\s+)?"%s"\s*\((.*?)^\)' % re.escape(name),
                       text, re.MULTILINE | re.DOTALL)
    if not header:
        return []
    found = re.search(r"apiSchemas\s*=\s*\[(.*?)\]", header.group(1), re.DOTALL)
    return re.findall(r'"([^"]+)"', found.group(1)) if found else []


def _engine(parser, path: pathlib.Path, plug: pathlib.Path, domain: str,
            source: str, prefixes: tuple[str, ...]) -> list[dict[str, object]]:
    schemas = parser._classes(path.resolve(), path.resolve().parent)
    info = _plug_info(plug)
    text = path.read_text(encoding="utf-8")
    result = []
    for schema in schemas:
        name = str(schema["name"])
        if not name.startswith(prefixes):
            continue
        entry = info.get(name)
        if entry is None:
            raise SystemExit(f"{name} is missing from {plug}")
        schema["kind"] = _KINDS[entry["schemaKind"]]
        if schema["kind"] in ("singleApply", "multipleApply"):
            schema["inherits"] = "APISchemaBase"
        elif "inherits" not in schema:
            schema["inherits"] = "Typed"
        if entry.get("apiSchemaCanOnlyApplyTo"):
            schema["canOnlyApplyTo"] = sorted(entry["apiSchemaCanOnlyApplyTo"])
        builtin = _builtin_apis(text, name)
        if builtin:
            schema["builtinAPISchemas"] = builtin
        schema["domain"] = domain
        schema["source"] = source
        result.append(schema)
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--newton-schema", required=True, type=pathlib.Path)
    ap.add_argument("--newton-plug-info", type=pathlib.Path)
    ap.add_argument("--mujoco-plug-info", type=pathlib.Path)
    ap.add_argument("--newton-version", required=True)
    ap.add_argument("--newton-commit", default="")
    ap.add_argument("--mujoco-schema", required=True, type=pathlib.Path)
    ap.add_argument("--mujoco-version", required=True)
    ap.add_argument("--mujoco-commit", default="")
    ap.add_argument("--output", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[1] /
                    "doc/generated/engine-physics-schemas.json")
    args = ap.parse_args()

    parser = _openusd_parser()
    schemas = _engine(parser, args.newton_schema,
                      args.newton_plug_info or args.newton_schema.with_name("plugInfo.json"),
                      "newtonPhysics", "newton_usd_schemas/generatedSchema.usda",
                      ("Newton",))
    schemas += _engine(parser, args.mujoco_schema,
                       args.mujoco_plug_info or args.mujoco_schema.with_name("plugInfo.json"),
                       "mjcPhysics", "src/experimental/usd/mjcPhysics/schema.usda",
                       ("Mjc",))
    schemas.sort(key=lambda item: (str(item["domain"]), str(item["name"])))
    manifest = {
        "formatVersion": 1,
        "engines": {
            "newton-usd-schemas": {
                "version": args.newton_version,
                "commit": args.newton_commit or _git_commit(args.newton_schema),
            },
            "mujoco": {
                "version": args.mujoco_version,
                "commit": args.mujoco_commit or _git_commit(args.mujoco_schema),
            },
        },
        "schemaCount": len(schemas),
        "schemas": schemas,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
