#!/usr/bin/env python3
"""Validate the generated AOUSD Core schema/field coverage tables."""

import argparse
import pathlib
import re
import sys


SCHEMAS = {
    "ColorSpaceDefinitionAPI": {
        "name", "redChroma", "greenChroma", "blueChroma", "whitePoint",
        "gamma", "linearBias",
    },
    "ColorSpaceAPI": {"colorSpace:name"},
    "CollectionAPI": {
        "collection:__INSTANCE__:expansionRule",
        "collection:__INSTANCE__:includeRoot",
        "collection:__INSTANCE__:includes",
        "collection:__INSTANCE__:excludes",
        "collection:__INSTANCE__",
    },
}

MINIMUM_FIELDS = {
    ("Layer", "expressionVariables"), ("Prim", "displayGroupOrder"),
    ("Property", "comment"), ("Attribute", "allowedTokens"),
    ("Relationship", "targetPaths"),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true",
                        help="fail when generated coverage is incomplete")
    args = parser.parse_args()
    generated = args.root / "src/schema/generated"
    schema_text = (generated / "aousd-core-schema-definitions.inc").read_text()
    actual = {}
    for schema, name in re.findall(
            r'AOUSD_SCHEMA_(?:FALLBACK|DECLARE)\("([^"]+)",\s*"([^"]+)"',
            schema_text, re.S):
        actual.setdefault(schema, set()).add(name)
    missing = []
    for schema, names in SCHEMAS.items():
        for name in sorted(names - actual.get(schema, set())):
            missing.append(f"schema {schema}.{name}")

    field_text = (generated / "aousd-elective-field-coverage.inc").read_text()
    fields = set(re.findall(r"AOUSD_FIELD\((\w+),\s*([\w]+),\s*\w+\)",
                            field_text))
    for scope, name in sorted(MINIMUM_FIELDS - fields):
        missing.append(f"field {scope}.{name}")
    if len(fields) < 70:
        missing.append(f"field table has only {len(fields)} entries (expected >=70)")

    # Spec-matrix floors in the conformance binary: the table-driven tests in
    # tests/core/test_aousd_conformance.cc must not silently shrink. Rows are
    # counted between each `k<Name>Matrix[] = {` and its closing `};`.
    conformance = (args.root /
                   "tests/core/test_aousd_conformance.cc").read_text()
    matrix_floors = {
        "kPathGrammarMatrix": 25,   # §8.3 production-derived path cases
        "kEscapeMatrix": 14,        # string escape decode/round-trip cases
        "kCrateVersionMatrix": 5,   # crate feature -> min version rows
    }
    for name, floor in matrix_floors.items():
        m = re.search(name + r"\[\]\s*=\s*\{(.*?)\n  \};", conformance,
                      re.S)
        if not m:
            missing.append(f"matrix {name} not found in conformance test")
            continue
        rows = len(re.findall(r'\{\s*"', m.group(1)))
        if rows < floor:
            missing.append(
                f"matrix {name} has only {rows} rows (expected >={floor})")

    # VR edge matrix (pxr-baked expectations) floor.
    vr_inc = args.root / "tests/core/generated/vr-edge-expected.inc"
    vr_rows = len(re.findall(r"^VR_EDGE_CASE\(", vr_inc.read_text(), re.M))
    if vr_rows < 70:
        missing.append(
            f"vr-edge-expected.inc has only {vr_rows} rows (expected >=70)")
    if missing:
        print("AOUSD generated coverage is incomplete:", file=sys.stderr)
        for item in missing:
            print(f"  {item}", file=sys.stderr)
        return 1
    print(f"AOUSD generated tables: {sum(map(len, actual.values()))} schema "
          f"properties, {len(fields)} elective fields")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
