# SPDX-License-Identifier: Apache-2.0
"""Native import telemetry, failed attempts and independent resource limits."""
import concurrent.futures
import json
import os
import subprocess

import lightusd
import pytest

OBJ = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"


def test_report_is_snapshot_not_drained_or_serialized():
    with lightusd.load_bytes(OBJ, format="obj", source="memory:mesh.obj") as stage:
        report = stage.import_report()
        assert report["version"] == 1 and report["status"] == "success"
        assert report["format"] == "obj" and report["source"] == "memory:mesh.obj"
        assert report["input_bytes"] == len(OBJ)
        assert report["counts"]["output_vertices"] == 3
        assert report["counts"]["output_indices"] == 3
        assert report["estimated_peak_memory_bytes"] >= len(OBJ)
        assert report["elapsed_seconds"] >= 0
        assert report["limits"]["max_depth"] == 64
        stage.warnings()
        assert stage.import_report() == report
        report["counts"]["meshes"] = 100
        assert stage.import_report()["counts"]["meshes"] == 1
        assert "estimated_peak_memory_bytes" not in stage.export_usda()
    with pytest.raises(lightusd.UsdError, match="closed"):
        stage.import_report()
    with lightusd.loads('#usda 1.0\ndef Xform "Empty" {}') as empty:
        assert empty.import_report() == {}


def test_warning_and_failed_import_have_reports():
    with lightusd.load_bytes(b"mtllib absent.mtl\n" + OBJ, format="obj") as stage:
        report = stage.import_report()
        assert any(d["code"] == "obj.material_unresolved" for d in report["diagnostics"])
        assert report["dependencies"][0]["name"] == "absent.mtl"
        assert not report["dependencies"][0]["success"]
        assert not report["dependencies"][0]["read"]
        assert stage.warnings()
        assert stage.import_report() == report
    with pytest.raises(lightusd.UsdError) as error:
        lightusd.load_bytes(b"v invalid 0 0\n", format="obj", source="memory:broken.obj")
    report = error.value.import_report
    assert report["status"] == "failed" and report["source"] == "memory:broken.obj"
    assert any(d["severity"] == "error" and d["line"] == 1 for d in report["diagnostics"])
    assert any(d["code"] == "import.failed" for d in report["diagnostics"])


def test_report_counts_dependencies_without_retaining_resolver():
    resolver = lightusd.AssetResolver()
    files = {"report:mesh.obj": b"mtllib report:mat.mtl\n" + OBJ,
             "report:mat.mtl": b"newmtl m\nmap_Kd report:a.png\nmap_Ks report:b.png\n",
             "report:image.png": b"arbitrary bytes retained without decoding"}
    def resolve(identifier, anchor, context):
        return "report:image.png" if identifier in ("report:a.png", "report:b.png") else identifier
    resolver.register_scheme("report", resolve, lambda name: files[name])
    stage = lightusd.import_obj("report:mesh.obj", resolver=resolver)
    report = stage.import_report()
    assert report["dependency_reads"] == 3  # source, material, image
    assert report["dependency_bytes"] == sum(map(len, files.values()))
    assert report["retained_texture_bytes"] == len(files["report:image.png"])
    assert report["texture_cache_hits"] >= 1
    assert any(d["cached"] and not d["read"] for d in report["dependencies"])
    resolver.unregister_scheme("report")
    assert stage.import_report() == report


def test_resolver_exception_keeps_original_type_and_report():
    resolver = lightusd.AssetResolver()
    def broken(*args):
        raise RuntimeError("resolver original error")
    resolver.register_scheme("report", broken)
    with pytest.raises(RuntimeError, match="original error") as error:
        lightusd.import_obj("report:broken.obj", resolver=resolver)
    assert error.value.import_report["status"] == "failed"


def test_independent_input_and_memory_limits(tmp_path):
    padded = OBJ + b"# " + b"a" * 1000 + b"\n"
    with pytest.raises(lightusd.UsdError) as error:
        lightusd.load_bytes(padded, format="obj", max_memory=8 * 1024**2, max_input=128)
    assert error.value.import_report["limits"]["max_input_bytes"] == 128
    assert error.value.import_report["status"] == "failed"
    stage = lightusd.load_bytes(padded, format="obj", max_memory=8 * 1024**2, max_input=2048)
    assert stage.import_report()["limits"]["max_memory_bytes"] == 8 * 1024**2
    assert lightusd.load_bytes(OBJ, format="obj", max_memory=8 * 1024**2).import_report()["limits"]["max_input_bytes"] == 8 * 1024**2
    assert lightusd.load_bytes(OBJ, format="obj", max_memory=8 * 1024**2, max_input=0).import_report()["limits"]["max_input_bytes"] == 512 * 1024**2
    path = tmp_path / "mesh.obj"
    path.write_bytes(padded)
    with pytest.raises(lightusd.UsdError) as file_error:
        lightusd.load(path, max_input=128)
    assert file_error.value.import_report["source"] == str(path)
    with pytest.raises(lightusd.UsdError) as memory_error:
        lightusd.load_bytes(OBJ * 100, format="obj", max_memory=4096, max_input=1024**2)
    assert memory_error.value.import_report["limits"]["max_input_bytes"] == 1024**2


def test_xml_depth_independent_from_input_size():
    xml = b'<robot><link name="base"/>' + b'<unknown>' * 12 + b'</unknown>' * 12 + b'</robot>'
    with pytest.raises(lightusd.UsdError, match="depth") as error:
        lightusd.load_bytes(xml, format="urdf", max_depth=8)
    assert error.value.import_report["limits"]["max_depth"] == 8
    assert lightusd.load_bytes(xml, format="urdf", max_depth=32).import_report()["status"] == "success"


@pytest.mark.parametrize("format", [None, "auto"])
def test_file_auto_dispatch_uses_native_import_limits_and_reports(tmp_path, format):
    path = tmp_path / "source.urdf"
    path.write_bytes(b'<robot><link name="base"/></robot>')
    stage = lightusd.load(path, format=format, max_input=1024)
    assert stage.import_report()["format"] == "urdf"
    assert stage.import_report()["limits"]["max_input_bytes"] == 1024


@pytest.mark.parametrize("options", [dict(max_input=-1), dict(max_depth=0), dict(max_depth=1025), dict(max_depth=2**32), dict(max_memory=-1)])
def test_invalid_limits(options):
    with pytest.raises(ValueError):
        lightusd.load_bytes(OBJ, format="obj", **options)


def test_report_thread_isolation_and_large_warning_limit():
    def failed(index):
        source = f"memory:broken{index}.obj"
        with pytest.raises(lightusd.UsdError) as error:
            lightusd.load_bytes(b"v invalid 0 0\n", format="obj", source=source)
        assert error.value.import_report["source"] == source
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(failed, range(32)))
    materials = b"".join(f"mtllib missing{i}.mtl\n".encode() for i in range(1500))
    with lightusd.load_bytes(materials + OBJ, format="obj") as stage:
        report = stage.import_report()
        assert report["dropped_records"] > 0
        assert len(json.dumps(report)) < 2 * 1024**2
        assert report["status"] == "success"


UNSUPPORTED = (b'<mujoco><worldbody><body name="base"><geom type="sphere" size=".1"/></body>'
               b'</worldbody><deformable><flex name="unhandled" dim="2"/></deformable>'
               b'<equality><tendon name="unhandled_eq" tendon1="source"/></equality></mujoco>')


def test_unsupported_policy_preserves_source_metadata_and_roundtrips(tmp_path):
    with lightusd.load_bytes(UNSUPPORTED, format="mjcf") as stage:
        assert "lightusdImportUnsupported" not in stage.custom_layer_data
        assert sum(f["disposition"] == "skipped" for f in stage.import_report()["features"]) == 2
    with pytest.raises(lightusd.UsdError) as error:
        lightusd.load_bytes(UNSUPPORTED, format="mjcf", unsupported_policy="error")
    assert any(f["disposition"] == "rejected" for f in error.value.import_report["features"])
    with lightusd.load_bytes(UNSUPPORTED, format="mjcf", unsupported_policy="preserve") as stage:
        raw = stage.custom_layer_data["lightusdImportUnsupported"]
        data = json.loads(raw)
        assert len(data) == 2 and all(not item["executable"] for item in data)
        assert all(item["encoding"] == "xml" for item in data)
        assert "flex" in data[0]["data"] and "unhandled_eq" in data[1]["data"]
        assert sum(f["disposition"] == "preserved" for f in stage.import_report()["features"]) == 2
        for extension in ("usda", "usdc", "usdz"):
            path = tmp_path / f"preserved.{extension}"
            stage.save(str(path))
            with lightusd.load(path) as reopened:
                assert reopened.custom_layer_data["lightusdImportUnsupported"] == raw
                assert reopened.import_report() == {}  # telemetry is runtime only


def test_unsupported_included_xml_and_depth_are_checked(tmp_path):
    included = tmp_path / "included.xml"
    included.write_bytes(b'<mujoco><deformable><flex name="included"/></deformable></mujoco>')
    root = tmp_path / "scene.xml"
    root.write_bytes(b'<mujoco><include file="included.xml"/><worldbody><body name="base"/></worldbody></mujoco>')
    stage = lightusd.import_mjcf(root, unsupported_policy="preserve")
    data = json.loads(stage.custom_layer_data["lightusdImportUnsupported"])
    assert len(data) == 1 and data[0]["source"] == included.as_posix()
    with pytest.raises(lightusd.UsdError, match="Unsupported"):
        lightusd.import_mjcf(root, unsupported_policy="error")
    # A permissive policy must still enforce include/XML depth limits.
    included.write_bytes(b'<mujoco><worldbody>' + b'<body>' * 12 + b'</body>' * 12 + b'</worldbody></mujoco>')
    with pytest.raises(lightusd.UsdError, match="depth"):
        lightusd.import_mjcf(root, max_depth=8)


def test_obj_material_unsupported_preservation():
    resolver = lightusd.AssetResolver()
    resolver.register_scheme("report", lambda *args: args[0],
                             lambda name: b"newmtl m\nbump height.png\ndisp displacement.png\nPs .2\naniso .3\n")
    source = b"mtllib report:mat.mtl\n" + OBJ
    with lightusd.load_bytes(source, format="obj", resolver=resolver,
                            unsupported_policy="preserve") as stage:
        data = json.loads(stage.custom_layer_data["lightusdImportUnsupported"])
        assert len(data) == 2
        assert json.loads(data[0]["data"])["bump"] == "height.png"
        assert json.loads(data[1]["data"])["displacement"] == "displacement.png"
        assert all(item["encoding"] == "json" and not item["executable"] for item in data)
    with pytest.raises(lightusd.UsdError, match="bump"):
        lightusd.load_bytes(source, format="obj", resolver=resolver, unsupported_policy="error")


def test_preservation_memory_limit_fails_instead_of_dropping_source():
    source = b'<mujoco><worldbody><body name="base"/></worldbody><deformable><flex note="' + b'x' * 20000 + b'"/></deformable></mujoco>'
    base = lightusd.load_bytes(source, format="mjcf").import_report()["estimated_peak_memory_bytes"]
    full = lightusd.load_bytes(source, format="mjcf", unsupported_policy="preserve").import_report()["estimated_peak_memory_bytes"]
    assert full > base
    budget = (base + full) // 2
    assert lightusd.load_bytes(source, format="mjcf", max_memory=budget).import_report()["status"] == "success"
    with pytest.raises(lightusd.UsdError, match="(memory|budget|limit)") as error:
        lightusd.load_bytes(source, format="mjcf", max_memory=budget, unsupported_policy="preserve")
    assert error.value.import_report["status"] == "failed"


def test_invalid_policy():
    with pytest.raises(ValueError, match="unsupported_policy"):
        lightusd.load_bytes(OBJ, format="obj", unsupported_policy="ignore")


def test_preserved_invalid_utf8_fails_instead_of_losing_data():
    with pytest.raises(lightusd.UsdError, match="(UTF-8|XML)") as error:
        lightusd.load_bytes(b'<mujoco><worldbody/><deformable><flex note="\xff"/></deformable></mujoco>',
                           format="mjcf", unsupported_policy="preserve")
    assert error.value.import_report["status"] == "failed"


def test_preserved_metadata_openusd(tmp_path):
    executable = os.environ.get("OPENUSD_PYTHON")
    if not executable:
        pytest.skip("set OPENUSD_PYTHON for the optional metadata oracle")
    stage = lightusd.load_bytes(UNSUPPORTED, format="mjcf", unsupported_policy="preserve")
    script = '''
import json, sys
from pxr import Usd
stage = Usd.Stage.Open(sys.argv[1])
assert stage
data = json.loads(stage.GetRootLayer().customLayerData['lightusdImportUnsupported'])
assert len(data) == 2 and all(not item['executable'] for item in data)
assert 'unhandled_eq' in data[1]['data']
'''
    env = dict(os.environ, PYTHONPATH=os.environ.get("OPENUSD_PYTHONPATH", ""),
               PYTHONDONTWRITEBYTECODE="1")
    for extension in ("usda", "usdc", "usdz"):
        path = tmp_path / f"oracle.{extension}"
        stage.save(str(path))
        result = subprocess.run([executable, "-c", script, str(path)],
                                capture_output=True, text=True, env=env)
        assert result.returncode == 0, result.stdout + result.stderr


def test_import_progress_and_cancellation(tmp_path):
    # Enough geometry to cross the 1 MiB progress granularity.
    obj = b''.join(b'v %d 0 0\nv %d 1 0\nv %d 0 1\n' % (i, i, i) for i in range(20000))
    obj += b''.join(b'f %d %d %d\n' % (3 * i + 1, 3 * i + 2, 3 * i + 3) for i in range(20000))
    mesh = tmp_path / 'big.obj'
    mesh.write_bytes(obj)
    xml = tmp_path / 'robot.xml'
    xml.write_bytes(b'<mujoco><asset><mesh name="m" file="big.obj"/></asset><worldbody>'
                    b'<body name="b"><freejoint/><geom type="mesh" mesh="m"/></body></worldbody></mujoco>')
    calls = []
    lightusd.import_mjcf(xml, progress=lambda phase, done, total: calls.append((phase, done, total)))
    phases = [c[0] for c in calls]
    assert 'import' in phases and phases[-1] == 'convert'
    done = [c[1] for c in calls if c[0] == 'import']
    assert done == sorted(done) and done[0] > 0
    with pytest.raises(lightusd.UsdError, match='cancelled'):
        lightusd.import_mjcf(xml, progress=lambda *args: False)
    class Stop(Exception):
        pass
    def boom(*args):
        raise Stop('stop')
    with pytest.raises(Stop):
        lightusd.import_mjcf(xml, progress=boom)
    with pytest.raises(TypeError):
        lightusd.import_mjcf(xml, progress=42)
