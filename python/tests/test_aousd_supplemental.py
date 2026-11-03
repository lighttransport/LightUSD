# SPDX-License-Identifier: Apache-2.0
"""AOUSD Core supplemental conformance via lightusd Python binding.

Discovers the supplemental checkout via (first wins):
* $AOUSD_CORE_SUPPLEMENTAL_ROOT
* $LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT
* ~/.cache/lightusd/core-spec-supplemental-release_dec2025/releases/1.0.1
* ../core-spec-supplemental-public (symlink created by fetch script)
* ./aousd-supplemental (dev symlink)
* /mnt/nvme02/work/lightusd-repo/core-spec-supplemental-public

If no checkout is found the whole module is skipped (CI without data).
Each category is exercised with a capped sample so the suite stays fast;
set LIGHTUSD_AOUSD_CAP=0 for full corpus or LIGHTUSD_AOUSD_FILTER=prefix.
"""
import os
import pathlib
import json
import pytest

import lightusd

np = pytest.importorskip("numpy")


def _find_aousd_root():
    for env in ("AOUSD_CORE_SUPPLEMENTAL_ROOT", "LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT"):
        v = os.environ.get(env)
        if v and pathlib.Path(v).is_dir():
            return pathlib.Path(v)
    candidates = [
        pathlib.Path.home() / ".cache/lightusd/core-spec-supplemental-release_dec2025/releases/1.0.1",
        pathlib.Path.home() / ".cache/lightusd/core-spec-supplemental-release_dec2025",
        pathlib.Path(__file__).resolve().parents[3] / "core-spec-supplemental-public" / "releases" / "1.0.1",
        pathlib.Path(__file__).resolve().parents[3] / "core-spec-supplemental-public",
        pathlib.Path(__file__).resolve().parents[2] / "aousd-supplemental",
        pathlib.Path(__file__).resolve().parents[3] / "aousd-supplemental",
        pathlib.Path("/mnt/nvme02/work/lightusd-repo/core-spec-supplemental-public/releases/1.0.1"),
        pathlib.Path("/mnt/nvme02/work/lightusd-repo/core-spec-supplemental-public"),
        pathlib.Path("/mnt/nvme02/work/core-spec-supplemental-public/releases/1.0.1"),
    ]
    for p in candidates:
        if p.is_dir() and (p / "LICENSE").is_file() and (p / "composition").is_dir():
            return p
        # cache root is releases/1.0.1 parent, check child
        if p.name == "core-spec-supplemental-public" and (p / "releases" / "1.0.1" / "LICENSE").is_file():
            return p / "releases" / "1.0.1"
    # Try to find via dev symlink we created
    for p in [pathlib.Path("/mnt/nvme02/work/lightusd-repo/dev/aousd-supplemental")]:
        if p.is_dir() and (p / "LICENSE").is_file():
            return p
    return None


AOUSD_ROOT = _find_aousd_root()
CAP = int(os.environ.get("LIGHTUSD_AOUSD_CAP", "50"))
FILTER = os.environ.get("LIGHTUSD_AOUSD_FILTER", "")

pytestmark = [
    pytest.mark.aousd,
    pytest.mark.skipif(
        AOUSD_ROOT is None,
        reason="AOUSD supplemental not found (run scripts/fetch-aousd-supplemental.sh)",
    ),
]


def test_aousd_discovery(capsys):
    print(f"[aousd] AOUSD_ROOT={AOUSD_ROOT} cap={CAP} filter={FILTER!r}")
    assert AOUSD_ROOT is not None
    for cat in ("composition", "data_types", "file_formats", "value_resolution"):
        d = AOUSD_ROOT / cat
        print(f"  {cat}: {d} exists={d.is_dir()}")


def _enumerate_file_formats(limit=CAP):
    root = AOUSD_ROOT / "file_formats" / "tests" / "assets"
    if not root.is_dir():
        return []
    files = sorted(root.rglob("*.usda")) + sorted(root.rglob("*.usdc"))
    if FILTER:
        files = [p for p in files if FILTER in str(p)]
    return files[:limit] if limit else files


# file_formats assets next does not load, with the reason. Each entry must
# still fail (so a fix is noticed and the entry pruned); every other asset
# must load.
KNOWN_FILE_FORMAT_FAILURES = {}


def test_aousd_file_formats_load():
    files = _enumerate_file_formats()
    assert files, "no file_formats assets"
    assets_root = AOUSD_ROOT / "file_formats" / "tests" / "assets"
    ok = 0
    for f in files:
        known = KNOWN_FILE_FORMAT_FAILURES.get(f.relative_to(assets_root).as_posix())
        if known:
            with pytest.raises(lightusd.UsdError):
                lightusd.load(str(f), composed=False)
            continue
        try:
            # The file_formats suite is parser-level: its assets reference
            # dependency layers (e.g. layermetadata.usda's subLayers) that the
            # corpus does not ship, so load them uncomposed.
            st = lightusd.load(str(f), composed=False)
        except lightusd.UsdError as e:
            pytest.fail(f"file_formats load failed {f}: {e}")
        assert st.stats["prim_count"] >= 0
        # zero-copy sanity on first mesh
        for prim in st:
            if "points" in prim:
                arr = prim["points"]
                a = np.asarray(arr)
                assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
                break
        ok += 1
    print(f"[aousd] file_formats ok {ok}/{len(files)}")


def _enumerate_value_resolution(limit=CAP):
    root = AOUSD_ROOT / "value_resolution" / "tests" / "assets"
    if not root.is_dir():
        return []
    # each case is a directory with entry.usd (or entry.usda)
    entries = sorted(root.glob("*/entry.usd")) + sorted(root.glob("*/entry.usda"))
    if FILTER:
        entries = [p for p in entries if FILTER in str(p)]
    return entries[:limit] if limit else entries


def test_aousd_value_resolution_load():
    entries = _enumerate_value_resolution()
    if not entries:
        pytest.skip("no value_resolution entries")
    ok = 0
    failures = []
    for e in entries:
        try:
            st = lightusd.load(str(e))
        except lightusd.UsdError as e2:
            failures.append(f"{e.name}: {e2}")
            continue
        assert st.stats["prim_count"] >= 0
        ok += 1
    print(f"[aousd] value_resolution ok {ok}/{len(entries)}")
    # Every shipped value-resolution case loads today; keep it a hard ratchet
    # so a newly unsupported case is visible instead of silently skipped.
    assert not failures, "\n".join(failures)


def _enumerate_composition(limit=CAP):
    root = AOUSD_ROOT / "composition" / "tests" / "assets"
    if not root.is_dir():
        return []
    # each case dir contains pcp.json
    cases = sorted(root.glob("*/pcp.json"))
    if FILTER:
        cases = [p for p in cases if FILTER in str(p.parent)]
    return cases[:limit] if limit else cases


# Expected `Composing` paths that this bridge cannot confirm, each with the
# reason. The set is asserted exactly: a fixed case must remove its entry.
KNOWN_COMPOSING_EXCEPTIONS = {
    ("BasicInstancingAndNestedInstances_root", "/Set_1/PropInstance2/geom"):
        "instance-proxy path; prim_at resolves physical prim paths, not proxies",
    ("BasicNestedVariants_root", "/DirectlyNestedVariants/anim_spooky_anim_sphere"):
        "bridge forces standin=render, so the anim outer variant is intentionally "
        "not selected",
    ("BasicNestedVariants_root", "/DirectlyNestedVariants/anim_spooky_sphere"):
        "bridge forces standin=render, so the anim outer variant is intentionally "
        "not selected",
}


def test_aousd_composition_load():
    cases = _enumerate_composition()
    assert cases, "no composition cases"
    ok = 0
    rejected = []
    unexpected = []
    missing_entry = []
    unresolved = set()
    path_checks = 0
    for pcp_json in cases:
        data = json.loads(pcp_json.read_text())
        case_dir = pcp_json.parent
        entry = case_dir / data["Entry"]
        # prefer usda text entry if present
        text_entry = case_dir / "usda" / data["Entry"]
        if text_entry.is_file():
            entry = text_entry
        if not entry.is_file():
            missing_entry.append(case_dir.name)
            continue
        # A case whose pcp.json authors "Errors" is a negative dependency case:
        # the corpus itself expects composition diagnostics, so the secure
        # (default) loader may reject it. Any other rejection is a regression.
        authored_errors = data.get("Errors") or []
        try:
            # next default is now fallback-free; corpus expects standin=render fallback for some cases (e.g. /FergusCloak)
            st = lightusd.load(str(entry), variants={"standin": "render"})
        except lightusd.UsdError as e:
            if authored_errors:
                rejected.append(case_dir.name)
            else:
                unexpected.append((case_dir.name, str(e)))
            continue
        # basic sanity
        assert st.stats["prim_count"] >= 0
        # Every prim the corpus says must compose has to be addressable, not
        # merely "some prims loaded".
        for prim_path in data.get("Composing") or {}:
            path_checks += 1
            try:
                prim = st.prim_at(prim_path)
                resolved = prim is not None and prim.path == prim_path
            except Exception:  # KeyError for an uncomposed path
                resolved = False
            if not resolved:
                unresolved.add((case_dir.name, prim_path))
        ok += 1
    known = set(KNOWN_COMPOSING_EXCEPTIONS)
    print(f"[aousd] composition ok {ok}/{len(cases)}; "
          f"composing paths {path_checks - len(unresolved)}/{path_checks} resolved; "
          f"rejected-as-authored {sorted(rejected)}; "
          f"missing entry {sorted(missing_entry)}")
    assert not unexpected, "\n".join(
        f"{name}: {err}" for name, err in unexpected)
    assert not missing_entry, f"cases without an entry file: {missing_entry}"
    assert ok + len(rejected) == len(cases)
    assert unresolved <= known, "unexpected unresolved composing paths:\n" + "\n".join(
        f"{name} {path}" for name, path in sorted(unresolved - known))
    assert unresolved >= known, "known composing exceptions no longer fail:\n" + "\n".join(
        f"{name} {path}" for name, path in sorted(known - unresolved))


# USD value types the corpus names but the current value/type model has no
# representation for. `opaque` is metadata-oriented and `group` is its role
# alias. Keep them explicit so implementing one of them fails this test until
# the list is updated.
KNOWN_UNSUPPORTED_DATA_TYPES = {"opaque", "group"}


def _data_type_element(name):
    """Strip array/list-op syntax to the element type the C API exposes."""
    if name.endswith("[]"):
        name = name[:-2]
    if name.startswith("listop<") and name.endswith(">"):
        name = name[7:-1]
    return name


def test_aousd_data_types_types():
    # data_types JSON describes expected type parsing; drive the whole pinned
    # table through the C API instead of a hand-picked sample.
    dt_root = AOUSD_ROOT / "data_types" / "tests"
    if not dt_root.is_dir():
        pytest.skip("no data_types")
    docs = sorted(dt_root.glob("*.json"))
    assert docs, "no data_types json"
    names = set()
    aliases = []
    for doc in docs:
        data = json.loads(doc.read_text())
        names.update(data.get("named_types", []))
        aliases.extend(data.get("semantic_aliases", []))
    assert len(names) >= 60, f"corpus type table unexpectedly small: {len(names)}"
    assert aliases, "no semantic aliases"

    unsupported = set()
    for name in sorted(names):
        element = _data_type_element(name)
        tid = lightusd.type_from_name(element)
        if tid == 0:
            unsupported.add(element)
            continue
        assert lightusd.type_name(tid) == element, (
            f"{name}: element {element} does not round-trip through type_name")
    for alias, base, role in aliases:
        element = _data_type_element(alias)
        baseline = _data_type_element(base)
        if element in unsupported or baseline in unsupported:
            unsupported.update({element, baseline})
            continue
        tid = lightusd.type_from_name(element)
        assert tid != 0 and lightusd.type_name(tid) == element, (
            f"semantic alias {alias} ({role}) does not resolve")
        assert lightusd.type_from_name(baseline) != 0, (
            f"semantic alias {alias} base {base} does not resolve")
    print(f"[aousd] data_types {len(names)} named, {len(aliases)} aliases, "
          f"unsupported {sorted(unsupported)}")
    assert unsupported == KNOWN_UNSUPPORTED_DATA_TYPES, (
        f"unsupported type set changed: {sorted(unsupported)}; update the list")
