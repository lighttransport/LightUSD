# SPDX-License-Identifier: Apache-2.0
"""Shared texture payloads survive import, resolver removal and USDZ export."""
import base64
import collections
import zipfile

import lightusd
import pytest


@pytest.mark.parametrize("format", ["obj", "urdf", "mjcf"])
def test_texture_aliases_read_and_package_once(tmp_path, format):
    image = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1ZkAAAAASUVORK5CYII=")
    image += b"\0" * (1024 * 1024 - len(image))
    count = 8
    mtl = "".join(f"newmtl m{i}\nmap_Kd cache:a.png\nmap_Ks cache:b.png\n" for i in range(count)).encode()
    if format == "obj":
        source = b"mtllib cache:materials.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n" + "".join(
            f"usemtl m{i}\nf 1 2 3\n" for i in range(count)).encode()
    elif format == "urdf":
        materials = "".join(f'<material name="m{i}"><texture filename="cache:{"a" if i % 2 else "b"}.png"/></material>' for i in range(count))
        visuals = "".join(f'<visual><geometry><box size="1 1 1"/></geometry><material name="m{i}"/></visual>' for i in range(count))
        source = f'<robot name="cache">{materials}<link name="body">{visuals}</link></robot>'.encode()
    else:
        materials = "".join(f'<material name="m{i}" texture="t{i % 2}"/>' for i in range(count))
        source = (f'<mujoco><asset><texture name="t0" type="2d" file="cache:a.png"/>'
                  f'<texture name="t1" type="2d" file="cache:b.png"/>{materials}</asset>'
                  '<worldbody><body name="body"><geom type="sphere" size=".1" material="m0"/></body></worldbody></mujoco>').encode()
    reads = collections.Counter()
    def resolve(identifier, anchor, context):
        if identifier in ("cache:a.png", "cache:b.png", "cache:shared.png"):
            return "cache:shared.png"
        return identifier if identifier == "cache:materials.mtl" else ""
    def read(identifier):
        reads[identifier] += 1
        return mtl if identifier == "cache:materials.mtl" else image
    resolver = lightusd.AssetResolver()
    resolver.register_scheme("cache", resolve, read)
    stage = lightusd.load_bytes(source, format=format, source=f"cache:models/scene.{format}",
                               resolver=resolver, max_memory=4 * 1024 * 1024)
    assert reads["cache:shared.png"] == 1

    if format == "obj":
        assert reads["cache:materials.mtl"] == 1
    textures = [p for p in stage.prims_of_type("Shader") if p.get("info:id") == "UsdUVTexture"]
    assert len(textures) >= count
    before = [p["inputs:file"] for p in textures]
    assert resolver.unregister_scheme("cache")
    for index, save in enumerate((stage.save, stage.save_usdz)):
        path = tmp_path / f"shared_{index}.usdz"
        save(str(path))
        with zipfile.ZipFile(path) as archive:
            packaged = [name for name in archive.namelist() if name != "root.usdc"]
            assert len(packaged) == 1
            assert archive.read(packaged[0]) == image
        with lightusd.load(path) as reopened:
            assert "textures/" in reopened.export_usda()
    assert [p["inputs:file"] for p in textures] == before
    assert reads["cache:shared.png"] == 1


@pytest.mark.parametrize("packaged", [False, True])
def test_composed_obj_retains_one_texture_payload(tmp_path, packaged):
    image = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1ZkAAAAASUVORK5CYII=")
    obj = b"mtllib materials.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl a\nf 1 2 3\n"
    reads = collections.Counter()
    resolver = lightusd.AssetResolver()
    if packaged:
        root = tmp_path / "input.usdz"
        with zipfile.ZipFile(root, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("root.usda", '#usda 1.0\ndef Xform "Asset" (prepend references = @mesh.obj@) {}\n')
            archive.writestr("mesh.obj", obj)
            archive.writestr("materials.mtl", "newmtl a\nmap_Kd tex.png\nmap_Ks ./tex.png\n")
            archive.writestr("tex.png", image)
    else:
        root = tmp_path / "input.usda"
        root.write_text('#usda 1.0\ndef Xform "Asset" (prepend references = @shared:mesh.obj@) {}\n')
        obj = obj.replace(b"materials.mtl", b"shared:materials.mtl")
        mtl = b"newmtl a\nmap_Kd shared:a.png\nmap_Ks shared:b.png\n"
        def resolve(identifier, anchor, context):
            return "shared:tex.png" if identifier in ("shared:a.png", "shared:b.png") else identifier
        def read(identifier):
            reads[identifier] += 1
            return {"shared:mesh.obj": obj, "shared:materials.mtl": mtl, "shared:tex.png": image}[identifier]
        resolver.register_scheme("shared", resolve, read)
    stage = lightusd.load(root, resolver=resolver)
    if not packaged:
        assert reads["shared:tex.png"] == 1
        resolver.unregister_scheme("shared")
    else:
        root.unlink()
    output = tmp_path / "retained.usdz"
    stage.save_usdz(str(output))
    with zipfile.ZipFile(output) as archive:
        assets = [name for name in archive.namelist() if name != "root.usdc"]
        assert len(assets) == 1
        assert archive.read(assets[0]) == image
    with lightusd.load(output) as reopened:
        for shader in reopened.prims_of_type("Shader"):
            if shader.get("info:id") == "UsdUVTexture":
                assert shader["inputs:file"].startswith("textures/")
