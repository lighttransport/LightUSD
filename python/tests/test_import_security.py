# SPDX-License-Identifier: Apache-2.0
"""Dependency containment for OBJ/MTL, URDF and MJCF imports."""
import os

import lightusd

OBJ = b'v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl m\nf 1/1 2/2 3/3\n'
PNG = (b'\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02'
       b'\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDAT\x08\xd7c\xf8\xcf\xc0\x00\x00\x03\x01'
       b'\x01\x00\x18\xdd\x8d\xb0\x00\x00\x00\x00IEND\xaeB`\x82')


def _secret(tmp_path):
    outside = tmp_path / 'outside'
    outside.mkdir()
    (outside / 'secret.mtl').write_text('newmtl m\nKd 1 0 0\n')
    (outside / 'secret.png').write_bytes(PNG)
    return outside


def _work(tmp_path, monkeypatch):
    work = tmp_path / 'work'
    work.mkdir()
    monkeypatch.chdir(work)
    return work


def _materials(stage):
    return stage.prims_of_type('Material')


def _textures(stage):
    return [str(p["inputs:file"]) for p in stage.prims_of_type("Shader")
            if p.get("info:id") == "UsdUVTexture"]


def test_bare_source_rejects_absolute_mtllib(tmp_path, monkeypatch):
    outside = _secret(tmp_path)
    _work(tmp_path, monkeypatch)
    data = b'mtllib ' + os.fsencode(outside / 'secret.mtl') + b'\n' + OBJ
    stage = lightusd._import(data=data, source='mesh.obj', format='obj')
    assert not _materials(stage)
    assert 'outside allowed roots' in stage.warnings()


def test_bare_source_rejects_absolute_texture(tmp_path, monkeypatch):
    outside = _secret(tmp_path)
    work = _work(tmp_path, monkeypatch)
    (work / 'mesh.mtl').write_text('newmtl m\nKd 1 1 1\nmap_Kd %s\n' % (outside / 'secret.png'))
    stage = lightusd._import(data=b'mtllib mesh.mtl\n' + OBJ, source='mesh.obj', format='obj')
    assert len(_materials(stage)) == 1
    assert 'outside allowed roots' in stage.warnings()
    assert not _textures(stage)


def test_bare_source_accepts_relative_dependencies(tmp_path, monkeypatch):
    work = _work(tmp_path, monkeypatch)
    (work / 'tex').mkdir()
    (work / 'tex' / 'albedo.png').write_bytes(PNG)
    (work / 'mesh.mtl').write_text('newmtl m\nKd 1 1 1\nmap_Kd tex/albedo.png\n')
    stage = lightusd._import(data=b'mtllib mesh.mtl\n' + OBJ, source='mesh.obj', format='obj')
    assert len(_materials(stage)) == 1
    assert 'outside allowed roots' not in stage.warnings()
    assert any('albedo.png' in name for name in _textures(stage))


def test_mtl_subdirectory_parent_escape(tmp_path):
    _secret(tmp_path)
    root = tmp_path / 'asset'
    (root / 'mtl').mkdir(parents=True)
    (root / 'mtl' / 'mesh.mtl').write_text('newmtl m\nKd 1 1 1\nmap_Kd ../../outside/secret.png\n')
    (root / 'mesh.obj').write_bytes(b'mtllib mtl/mesh.mtl\n' + OBJ)
    stage = lightusd.import_obj(root / 'mesh.obj')
    assert 'outside allowed roots' in stage.warnings()
    assert not _textures(stage)


def test_symlink_escape(tmp_path):
    outside = _secret(tmp_path)
    root = tmp_path / 'asset'
    root.mkdir()
    try:
        os.symlink(outside / 'secret.png', root / 'link.png')
    except OSError:
        import pytest
        pytest.skip('symlinks unavailable')
    (root / 'mesh.mtl').write_text('newmtl m\nKd 1 1 1\nmap_Kd link.png\n')
    (root / 'mesh.obj').write_bytes(b'mtllib mesh.mtl\n' + OBJ)
    stage = lightusd.import_obj(root / 'mesh.obj')
    assert 'outside allowed roots' in stage.warnings()
    assert not _textures(stage)


def test_empty_asset_dir_is_not_a_root(tmp_path, monkeypatch):
    outside = _secret(tmp_path)
    _work(tmp_path, monkeypatch)
    data = b'mtllib ' + os.fsencode(outside / 'secret.mtl') + b'\n' + OBJ
    stage = lightusd._import(data=data, source='mesh.obj', format='obj', asset_dirs=[''])
    assert not _materials(stage)


def test_explicit_asset_dir_allows_dependency(tmp_path):
    outside = _secret(tmp_path)
    root = tmp_path / 'asset'
    root.mkdir()
    (root / 'mesh.obj').write_bytes(b'mtllib secret.mtl\n' + OBJ)
    stage = lightusd.import_obj(root / 'mesh.obj', asset_dirs=[outside])
    assert len(_materials(stage)) == 1


def test_backslash_texture_path(tmp_path):
    root = tmp_path / 'asset'
    (root / 'tex').mkdir(parents=True)
    (root / 'tex' / 'albedo.png').write_bytes(PNG)
    (root / 'mesh.mtl').write_text('newmtl m\nKd 1 1 1\nmap_Kd tex\\albedo.png\n')
    (root / 'mesh.obj').write_bytes(b'mtllib mesh.mtl\n' + OBJ)
    stage = lightusd.import_obj(root / 'mesh.obj')
    assert any('albedo.png' in name for name in _textures(stage))


def test_mjcf_bare_source_rejects_absolute_mesh(tmp_path, monkeypatch):
    outside = _secret(tmp_path)
    (outside / 'm.obj').write_bytes(OBJ)
    _work(tmp_path, monkeypatch)
    xml = ('<mujoco><asset><mesh name="m" file="%s"/></asset><worldbody><body>'
           '<geom type="mesh" mesh="m"/></body></worldbody></mujoco>' % (outside / 'm.obj'))
    try:
        stage = lightusd._import(data=xml.encode(), source='robot.xml', format='mjcf')
    except Exception as exc:  # missing meshes are fatal unless allow_missing
        assert 'outside allowed roots' in str(exc)
    else:
        assert 'outside allowed roots' in stage.warnings()
