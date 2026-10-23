# SPDX-License-Identifier: Apache-2.0
"""MTL dependencies supplied entirely through the in-memory resolver."""
import base64
import math

import lightusd
import pytest

OBJ = b"mtllib materials.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl test\nf 1 2 3\n"


def material_stage(properties, **options):
    resolver = lightusd.AssetResolver()
    resolver.register_memory("materials.mtl", b"newmtl test\n" + properties)
    resolver.register_memory("123.png", base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1ZkAAAAASUVORK5CYII="))
    return lightusd.load_bytes(OBJ, format="obj", resolver=resolver, **options)


@pytest.mark.parametrize("properties", [b"Kd nan 0 0", b"Kd 1e100 0 0", b"Ks 0 .1junk 0",
    b"Ni infinity", b"Ns -2", b"Ns -3", b"Pr", b"illum 2147483648", b"illum 2junk",
    b"map_Kd -s 1e999 123.png", b"map_Kd -s 1 nan 123.png",
    b"map_Kd -mm 1 inf 123.png", b"map_Kd -texres 1junk 123.png",
    b"map_Kd -clamp maybe 123.png", b"map_Kd -s 1"])
def test_invalid_mtl_rejects_import(properties):
    with pytest.raises(lightusd.UsdError, match="invalid|parse"):
        material_stage(properties)


def test_optional_uv_components_and_numeric_texture_filename():
    stage = material_stage(b"Kd .5 .25 .125\nmap_Kd -s 2 3 -o .1 .2 -clamp on 123.png\n")
    transform = stage.prim_at("/World/Materials/test/Map_0_Transform")
    assert list(transform["inputs:scale"]) == pytest.approx([2, 3])
    assert list(transform["inputs:translation"]) == pytest.approx([.1, .2])
    texture = stage.prim_at("/World/Materials/test/Map_0")
    assert texture["inputs:file"] == "123.png"
    assert texture["inputs:wrapS"] == "clamp"
    assert not stage.warnings()


def test_large_finite_shininess_maps_to_finite_roughness():
    stage = material_stage(b"Ns 3e38\n")
    roughness = stage.prim_at("/World/Materials/test/PreviewSurface")["inputs:roughness"]
    assert math.isfinite(roughness) and 0 < roughness < 1


@pytest.mark.parametrize("in_memory", [True, False])
def test_oversized_mtl_is_fatal(tmp_path, in_memory):
    properties = b"#" + b"x" * (512 * 1024) + b"\n"
    with pytest.raises(lightusd.UsdError, match="limit|budget"):
        if in_memory:
            material_stage(properties, max_memory=256 * 1024)
        else:
            (tmp_path / "materials.mtl").write_bytes(properties)
            lightusd.load_bytes(OBJ, format="obj", source=tmp_path / "scene.obj", max_memory=256 * 1024)


def test_oversized_texture_is_fatal():
    resolver = lightusd.AssetResolver()
    resolver.register_memory("materials.mtl", b"newmtl test\nmap_Kd 123.png\n")
    resolver.register_memory("123.png", b"x" * (512 * 1024))
    with pytest.raises(lightusd.UsdError, match="limit|budget"):
        lightusd.load_bytes(OBJ, format="obj", resolver=resolver, max_memory=256 * 1024)


def test_repeated_material_library_is_loaded_once():
    resolver = lightusd.AssetResolver()
    resolver.register_memory("materials.mtl", b"newmtl test\nKd .2 .3 .4\n")
    source = OBJ.replace(b"mtllib materials.mtl\n", b"mtllib materials.mtl materials.mtl\nmtllib materials.mtl\n")
    stage = lightusd.load_bytes(source, format="obj", resolver=resolver)
    assert len(stage.prims_of_type("Material")) == 1
    assert stage.prims_of_type("GeomSubset")[0].relationship("material:binding").targets == ("/World/Materials/test",)
    assert not stage.warnings()


def test_many_material_bindings_preserve_source_ids():
    count = 513
    definitions = "".join(f"newmtl material_{i}\nKd {i / (count - 1)} .25 .5\n"
                          for i in reversed(range(count))).encode()
    source = b"mtllib materials.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n" + "".join(
        f"usemtl material_{i}\nf 1 2 3\n" for i in range(count)).encode()
    resolver = lightusd.AssetResolver()
    resolver.register_memory("materials.mtl", definitions)
    stage = lightusd.load_bytes(source, format="obj", resolver=resolver)
    assert len(stage.prims_of_type("Material")) == count
    subsets = stage.prims_of_type("GeomSubset")
    assert len(subsets) == count
    visited = set()
    for subset in subsets:
        faces = list(subset["indices"])
        assert len(faces) == 1
        face = faces[0]
        visited.add(face)
        target, = subset.relationship("material:binding").targets
        assert target == f"/World/Materials/material_{face}"
        color = stage.prim_at(target + "/PreviewSurface")["inputs:diffuseColor"]
        assert list(color) == pytest.approx([face / (count - 1), .25, .5])
    assert visited == set(range(count))


@pytest.mark.parametrize("newline", [b"\n", b"\r\n", b"\r"])
@pytest.mark.parametrize("bom", [b"", b"\xef\xbb\xbf"])
def test_duplicate_material_names_keep_their_source_metadata(tmp_path, newline, bom):
    png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1ZkAAAAASUVORK5CYII=")
    for directory, properties in [("first", b"Ns 100"), ("second", b"Pr .8\nPm .7")]:
        folder = tmp_path / directory
        folder.mkdir()
        (folder / "color.png").write_bytes(png)
        text = b"newmtl shared\n" + properties + b"\nmap_Kd color.png\n"
        (folder / "library.mtl").write_bytes(bom + text.replace(b"\n", newline))
    source = OBJ.replace(b"mtllib materials.mtl", b"mtllib first/library.mtl second/library.mtl").replace(b"usemtl test", b"usemtl shared")
    stage = lightusd.load_bytes(source, format="obj", source=tmp_path / "scene.obj")
    first = stage.prim_at("/World/Materials/shared/PreviewSurface")
    second = stage.prim_at("/World/Materials/shared_1/PreviewSurface")
    assert first["inputs:roughness"] == pytest.approx(math.sqrt(2 / 102))
    assert first["inputs:useSpecularWorkflow"] == 1
    assert second["inputs:roughness"] == pytest.approx(.8)
    assert second["inputs:useSpecularWorkflow"] == 0
    assert stage.prim_at("/World/Materials/shared/Map_0")["inputs:file"] == str(tmp_path / "first/color.png")
    assert stage.prim_at("/World/Materials/shared_1/Map_0")["inputs:file"] == str(tmp_path / "second/color.png")
    assert stage.prims_of_type("GeomSubset")[0].relationship("material:binding").targets == ("/World/Materials/shared",)


def _surface(stage):
    return [p for p in stage.prims_of_type("Shader") if p.get("info:id") == "UsdPreviewSurface"][0]


def _mtl_stage(tmp_path, mtl, **options):
    (tmp_path / "m.mtl").write_text(mtl)
    (tmp_path / "t.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    (tmp_path / "m.obj").write_text("mtllib m.mtl\nusemtl m\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                    "vt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n")
    return lightusd.import_obj(tmp_path / "m.obj", **options)


def test_absent_ni_does_not_author_ior(tmp_path):
    surface = _surface(_mtl_stage(tmp_path, "newmtl m\nKd 1 1 1\nPm 1\n"))
    assert "inputs:ior" not in surface
    surface = _surface(_mtl_stage(tmp_path, "newmtl m\nKd 1 1 1\nNi 1.45\n"))
    assert surface["inputs:ior"] == pytest.approx(1.45)


@pytest.mark.parametrize("lines", ["d 0.25\nTr 0.9\n", "Tr 0.9\nd 0.25\n"])
def test_dissolve_wins_over_tr(tmp_path, lines):
    surface = _surface(_mtl_stage(tmp_path, "newmtl m\n" + lines))
    assert surface["inputs:opacity"] == pytest.approx(0.25)


def test_clearcoat_channel_and_bump_multiplier(tmp_path):
    stage = _mtl_stage(tmp_path, "newmtl m\nPc 0.5\nPcr 0.2\nmap_Pr -imfchan g t.png\n"
                                 "norm -bm 0.5 t.png\n")
    surface = _surface(stage)
    assert surface["inputs:clearcoat"] == pytest.approx(0.5)
    assert surface["inputs:clearcoatRoughness"] == pytest.approx(0.2)
    text = stage.export_usda()
    assert ".outputs:g>" in text  # roughness reads the green channel
    normal = [p for p in stage.prims_of_type("Shader") if p.get("info:id") == "UsdUVTexture"
              and p.get("inputs:bias") is not None
              and tuple(p.get("inputs:bias"))[:1] == pytest.approx((-0.5,))]
    assert len(normal) == 1 and tuple(normal[0]["inputs:scale"])[:2] == pytest.approx((1, 1))


def test_obj_units_and_kind_by_profile(tmp_path):
    stage = _mtl_stage(tmp_path, "newmtl m\n")
    assert stage.meters_per_unit == 1
    assert stage.prim_at("/World").kind == "component"
    legacy = _mtl_stage(tmp_path, "newmtl m\n", profile="legacy")
    assert legacy.prim_at("/World").kind in (None, "")


def test_obj_indexed_topology_keeps_source_points(tmp_path):
    # Two quads sharing an edge, with per-face normals and a UV seam.
    source = b'''v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 2 0 0
v 2 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vt .5 .5
vn 0 0 1
vn 0 0 -1
f 1/1/1 2/2/1 3/3/1 4/4/1
f 2/5/2 5/2/2 6/3/2 3/5/2
'''
    path = tmp_path / 'quads.obj'
    path.write_bytes(source)
    expanded = next(p for p in lightusd.import_obj(path) if p.type_name == 'Mesh')
    indexed = next(p for p in lightusd.import_obj(path, obj_topology='indexed') if p.type_name == 'Mesh')
    assert len(expanded['points']) == 8  # split where normals/UVs differ
    assert len(indexed['points']) == 6
    assert list(indexed['faceVertexIndices']) == [0, 1, 2, 3, 1, 4, 5, 2]
    assert list(indexed['primvars:normals:indices']) == [0, 0, 0, 0, 1, 1, 1, 1]
    assert len(indexed['primvars:normals']) == 2
    assert list(indexed['primvars:st:indices']) == [0, 1, 2, 3, 4, 1, 2, 4]
    assert 'normals' not in indexed.attributes
    with pytest.raises(ValueError):
        lightusd.import_obj(path, obj_topology='compact')
