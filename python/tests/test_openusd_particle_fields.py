# SPDX-License-Identifier: Apache-2.0
"""Compare CPU particle-field extraction with the optional OpenUSD parser."""
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip('pxr')
from pxr import Usd, UsdGeom, UsdVol

import lightusd
from lightusd import tydra


@pytest.mark.parametrize('precision', ['float', 'half'])
def test_gaussian_field_matches_openusd(tmp_path, precision):
    fixture = Path(__file__).resolve().parents[2] / 'tests/usda' / (
        'lusdview-gaussian-half.usda' if precision == 'half' else 'lusdview-gaussian-splat.usda')
    source = fixture.read_text().replace('(1, 0, 0, 0), (1, 0, 0, 0)',
                                         '(0.5, 0.5, -0.5, 0.5), (1, 0, 0, 0)')
    # OpenUSD's XformCache only evaluates transforms on Xformable prims.
    source = source.replace('def "GaussianHalfFixture"', 'def Xform "GaussianHalfFixture"')
    path = tmp_path / 'particles.usda'
    path.write_text(source)
    usd_stage = Usd.Stage.Open(str(path))
    assert usd_stage
    prim = next(p for p in usd_stage.Traverse() if p.IsA(UsdVol.ParticleField3DGaussianSplat))
    particle = tydra.to_render_scene(lightusd.load(str(path))).particle_fields()[0]
    suffix = 'h' if precision == 'half' else ''

    assert particle['prim_path'] == str(prim.GetPath())
    assert particle['kernel'] == 'gaussianEllipsoid'
    assert particle['projection_mode_hint'] == prim.GetAttribute('projectionModeHint').Get()
    assert particle['sorting_mode_hint'] == prim.GetAttribute('sortingModeHint').Get()
    assert particle['sh_degree'] == prim.GetAttribute('radiance:sphericalHarmonicsDegree').Get()
    for output, usd_name in [
        ('positions', 'positions'), ('scales', 'scales'), ('opacities', 'opacities'),
        ('sh_coefficients', 'radiance:sphericalHarmonicsCoefficients'),
    ]:
        expected = np.asarray(prim.GetAttribute(usd_name + suffix).Get(), dtype=np.float32)
        np.testing.assert_array_equal(np.asarray(particle[output]), expected)
    quats = prim.GetAttribute('orientations' + suffix).Get()
    expected_quats = [[q.GetReal(), *q.GetImaginary()] for q in quats]
    np.testing.assert_array_equal(np.asarray(particle['orientations']), expected_quats)
    assert particle['count'] == len(quats)
    matrix = UsdGeom.XformCache().GetLocalToWorldTransform(prim)
    np.testing.assert_array_equal(np.asarray(particle['world_matrix']),
                                  np.array([[matrix[i][j] for j in range(4)] for i in range(4)]))


@pytest.mark.parametrize('parent_type', ['', 'Scope', 'Xform'])
@pytest.mark.parametrize('reset', [False, True])
def test_parent_transform_matches_openusd(tmp_path, parent_type, reset):
    path = Path(__file__).resolve().parents[2] / 'tests/usda/lusdview-gaussian-half.usda'
    source = path.read_text().replace('def "GaussianHalfFixture"',
                                      f'def {parent_type} "GaussianHalfFixture"')
    if reset:
        source = source.replace('["xformOp:scale"]', '["!resetXformStack!", "xformOp:scale"]')
    source = source.replace('#usda 1.0', '#usda 1.0\n\ndef Xform "Outer" {\n'
                            'double3 xformOp:translate = (3, 4, 5)\n'
                            'uniform token[] xformOpOrder = ["xformOp:translate"]', 1) + '\n}\n'
    path = tmp_path / 'parent.usda'
    path.write_text(source)
    usd_stage = Usd.Stage.Open(str(path))
    prim = usd_stage.GetPrimAtPath('/Outer/GaussianHalfFixture/SplatField')
    matrix = UsdGeom.XformCache().GetLocalToWorldTransform(prim)
    particle = tydra.to_render_scene(lightusd.load(str(path))).particle_fields()[0]
    np.testing.assert_array_equal(np.asarray(particle['world_matrix']),
                                  np.array([[matrix[i][j] for j in range(4)] for i in range(4)]))


def test_gaussian_ply_conversion_matches_openusd(tmp_path):
    converter = pytest.importorskip('usd_convert_gsplat')
    property_names = [
        'x', 'y', 'z', 'scale_0', 'scale_1', 'scale_2',
        'rot_0', 'rot_1', 'rot_2', 'rot_3', 'opacity',
        'f_dc_0', 'f_dc_1', 'f_dc_2',
        *(f'f_rest_{i}' for i in range(9)),
    ]
    header = [
        'ply', 'format ascii 1.0', 'element vertex 2',
        *(f'property float {name}' for name in property_names),
        'end_header',
    ]
    rows = [
        {
            'x': 1.25, 'y': -2.5, 'z': 3.75,
            'scale_0': -0.69314718, 'scale_1': 0.0, 'scale_2': 0.40546511,
            'rot_0': 2.0, 'rot_1': 0.0, 'rot_2': 0.0, 'rot_3': 0.0,
            'opacity': 0.0,
            'f_dc_0': 0.1, 'f_dc_1': 0.2, 'f_dc_2': 0.3,
            **{f'f_rest_{i}': (i + 1) / 100.0 for i in range(9)},
        },
        {
            'x': -4.0, 'y': 0.5, 'z': 8.0,
            'scale_0': 0.69314718, 'scale_1': -0.69314718, 'scale_2': 0.0,
            'rot_0': 0.5, 'rot_1': 0.5, 'rot_2': -0.5, 'rot_3': 0.5,
            'opacity': 1.09861229,
            'f_dc_0': 0.4, 'f_dc_1': 0.5, 'f_dc_2': 0.6,
            **{f'f_rest_{i}': -(i + 1) / 100.0 for i in range(9)},
        },
    ]
    rows_text = [' '.join(format(row[name], '.9g') for name in property_names)
                 for row in rows]
    ply_path = tmp_path / 'synthetic-3dgs.ply'
    ply_path.write_text('\n'.join([*header, *rows_text, '']))

    usd_path = tmp_path / 'synthetic-3dgs.usda'
    splat_data = converter.read_ply(str(ply_path))
    assert splat_data.count == len(rows)
    converter.write_gaussian_splat_usd(
        splat_data, str(usd_path), source_file=str(ply_path), prim_name='ConvertedSplats')

    usd_stage = Usd.Stage.Open(str(usd_path))
    assert usd_stage
    prim = usd_stage.GetPrimAtPath('/ConvertedSplats')
    schema = UsdVol.ParticleField3DGaussianSplat(prim)
    assert schema
    assert prim.IsA(UsdVol.ParticleField3DGaussianSplat)

    particle = tydra.to_render_scene(lightusd.load(str(usd_path))).particle_fields()[0]
    assert particle['prim_path'] == str(prim.GetPath())
    assert particle['kernel'] == 'gaussianEllipsoid'
    assert particle['count'] == len(rows)

    for output, attribute in [
        ('positions', 'positions'),
        ('scales', 'scales'),
        ('opacities', 'opacities'),
        ('sh_coefficients', 'radiance:sphericalHarmonicsCoefficients'),
    ]:
        expected = np.asarray(prim.GetAttribute(attribute).Get(), dtype=np.float32)
        if output == 'sh_coefficients':
            expected = expected.reshape(len(rows), -1)
        np.testing.assert_array_equal(np.asarray(particle[output]), expected)

    quaternions = prim.GetAttribute('orientations').Get()
    expected_orientations = [[q.GetReal(), *q.GetImaginary()] for q in quaternions]
    np.testing.assert_array_equal(np.asarray(particle['orientations']),
                                  np.asarray(expected_orientations, dtype=np.float32))
    assert prim.GetAttribute('radiance:sphericalHarmonicsDegree').Get() == 1
    assert particle['sh_degree'] == 1
    matrix = UsdGeom.XformCache().GetLocalToWorldTransform(prim)
    np.testing.assert_array_equal(
        np.asarray(particle['world_matrix']),
        np.array([[matrix[i][j] for j in range(4)] for i in range(4)]))
