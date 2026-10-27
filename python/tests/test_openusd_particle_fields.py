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
