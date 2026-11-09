#!/usr/bin/env python3
"""Generate a USDA authored-data fixture for every in-scope OpenUSD schema."""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


def ident(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", text)

def required_schemas(ledger: dict[str, object]) -> list[dict[str, object]]:
    schemas = ledger.get("schemas")
    if not isinstance(schemas, list):
        raise ValueError("capability ledger has no schemas array")
    required = [schema for schema in schemas
                if isinstance(schema, dict) and schema.get("scope") == "required"]
    declared = ledger.get("requiredSchemaCount")
    if declared != len(required):
        raise ValueError(
            f"requiredSchemaCount={declared!r}, but found {len(required)} entries")
    return required


def generate(ledger: dict[str, object]) -> str:
    required = required_schemas(ledger)
    out = ["#usda 1.0", "(",
           '    doc = "Generated supported-schema coverage; do not edit"',
           ")", ""]
    for i, schema in enumerate(required):
        name, kind = schema["name"], schema["kind"]
        applied = kind in {"singleApply", "multipleApply"}
        typename = "Xform" if applied or kind in {"abstract", "nonApplied"} else name
        out.append(f'def {typename} "S{i:03d}_{ident(name)}" (')
        if applied:
            api = f"{name}:fixture" if kind == "multipleApply" else name
            out.append(f'    prepend apiSchemas = ["{api}"]')
        out.extend([
            ")", "{", f'    custom string parity:schema = "{name}"',
            f'    custom token parity:kind = "{kind}"',
            f'    custom string parity:domain = "{schema["domain"]}"', "}", ""])
    return "\n".join(out)


def generate_registry(ledger: dict[str, object]) -> str:
    schemas = [str(schema["name"]) for schema in required_schemas(ledger)]
    return "// Generated from the OpenUSD capability ledger; do not edit.\n" + "".join(
        f'LIGHTUSD_NEXT_SUPPORTED_SCHEMA("{name}")\n' for name in schemas)

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ledger", type=pathlib.Path, default=pathlib.Path(
        "doc/generated/lightusd-capabilities-openusd-26.08.json"))
    p.add_argument("--output", type=pathlib.Path, default=pathlib.Path(
        "tests/usda/generated/openusd-supported-schema-26.08.usda"))
    p.add_argument("--registry-output", type=pathlib.Path,
                   default=pathlib.Path(
                       "src/schema/generated/"
                       "openusd-supported-schema-names.inc"))
    p.add_argument("--check", action="store_true")
    p.add_argument("--stdout", action="store_true")
    p.add_argument("--registry-stdout", action="store_true")
    a = p.parse_args()
    ledger = json.loads(a.ledger.read_text(encoding="utf-8"))
    expected = generate(ledger)
    expected_registry = generate_registry(ledger)
    if a.stdout:
        sys.stdout.write(expected)
        return 0
    if a.registry_stdout:
        sys.stdout.write(expected_registry)
        return 0
    if a.check:
        actual = a.output.read_text(encoding="utf-8") if a.output.exists() else ""
        actual_registry = (a.registry_output.read_text(encoding="utf-8")
                           if a.registry_output.exists() else "")
        if (actual.rstrip() != expected.rstrip() or
                actual_registry.rstrip() != expected_registry.rstrip()):
            print("supported-schema fixture or registry table is stale",
                  file=sys.stderr)
            return 1
        print("OpenUSD supported-schema fixture: "
              f"{ledger['requiredSchemaCount']} schemas")
        return 0
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(expected, encoding="utf-8")
    a.registry_output.parent.mkdir(parents=True, exist_ok=True)
    a.registry_output.write_text(expected_registry, encoding="utf-8")
    print(f"wrote {a.output} and {a.registry_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
