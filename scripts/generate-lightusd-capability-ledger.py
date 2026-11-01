#!/usr/bin/env python3
"""Generate/check LightUSD's supported-feature OpenUSD parity contract.

The OpenUSD schema manifest is descriptive upstream data. This ledger adds the
product decision about which domains LightUSD promises to preserve, compose,
validate, and expose without turning generic preservation into a support claim.
It deliberately records targets, not hand-maintained completion percentages;
test evidence is attached separately as the implementation advances.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys


IN_SCOPE_DOMAINS = {
    "usd",
    "usdGeom",
    "usdLux",
    "usdMedia",
    "usdMtlx",
    "usdPhysics",
    "usdProc",
    "usdShade",
    "usdSkel",
    "usdUI",
    "usdVol",
}

EXCLUDED_DOMAINS = {
    "usdHydra": "Hydra/imaging integration is outside LightUSD's renderer-neutral core",
    "usdLod": "not part of the currently supported schema surface",
    "usdProfiles": "not part of the currently supported schema surface",
    "usdRender": "render-product/pass execution is explicitly excluded",
    "usdRi": "RenderMan-specific",
    "usdSemantics": "not part of the currently supported schema surface",
}

REQUIRED_CAPABILITIES = [
    "authored_data_roundtrip",
    "usda_read_write",
    "usdc_read_write",
    "composition_and_value_resolution",
    "schema_fallbacks_and_validation",
]


def build(source: dict[str, object]) -> dict[str, object]:
    schemas = source["schemas"]
    present_domains = {schema["domain"] for schema in schemas}
    classified_domains = IN_SCOPE_DOMAINS | set(EXCLUDED_DOMAINS)
    missing = present_domains - classified_domains
    stale = classified_domains - present_domains
    if missing or stale:
        raise ValueError(
            "domain classification drift: "
            f"missing={sorted(missing)} stale={sorted(stale)}")

    entries: list[dict[str, object]] = []
    for schema in schemas:
        domain = schema["domain"]
        in_scope = domain in IN_SCOPE_DOMAINS
        entry: dict[str, object] = {
            "domain": domain,
            "name": schema["name"],
            "kind": schema["kind"],
            "propertyCount": len(schema["properties"]),
            "scope": "required" if in_scope else "excluded",
        }
        if in_scope:
            entry["requiredCapabilities"] = REQUIRED_CAPABILITIES
            entry["typedConvenienceAccessors"] = "demand-driven"
            entry["tydraExtraction"] = "classify-per-schema"
            entry["rendering"] = "classify-per-feature-and-backend"
        else:
            entry["reason"] = EXCLUDED_DOMAINS[domain]
        entries.append(entry)

    entries.sort(key=lambda item: (item["domain"], item["name"]))
    required = [entry for entry in entries if entry["scope"] == "required"]
    excluded = [entry for entry in entries if entry["scope"] == "excluded"]
    return {
        "formatVersion": 1,
        "openusd": source["openusd"],
        "policy": {
            "runtimeOpenUSDDependency": False,
            "parityKind": "behavioral-within-supported-domains",
            "requiredDomains": sorted(IN_SCOPE_DOMAINS),
            "excludedDomains": EXCLUDED_DOMAINS,
            "localExtensions": ["PreliminaryAR", "MuJoCo"],
        },
        "requiredSchemaCount": len(required),
        "excludedSchemaCount": len(excluded),
        "schemas": entries,
    }


def serialized(payload: dict[str, object]) -> str:
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=pathlib.Path,
        default=pathlib.Path("doc/generated/openusd-schema-26.08.json"))
    parser.add_argument(
        "--output", type=pathlib.Path,
        default=pathlib.Path(
            "doc/generated/lightusd-capabilities-openusd-26.08.json"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    source = json.loads(args.manifest.read_text(encoding="utf-8"))
    expected = serialized(build(source))
    if args.check:
        try:
            actual = args.output.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"missing generated capability ledger: {args.output}",
                  file=sys.stderr)
            return 1
        if actual != expected:
            print(
                "capability ledger is stale; regenerate with "
                "scripts/generate-lightusd-capability-ledger.py",
                file=sys.stderr)
            return 1
        payload = json.loads(actual)
        print(
            "LightUSD/OpenUSD capability contract: "
            f"{payload['requiredSchemaCount']} required, "
            f"{payload['excludedSchemaCount']} excluded")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(expected, encoding="utf-8")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
