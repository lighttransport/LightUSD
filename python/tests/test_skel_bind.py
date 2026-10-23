# SPDX-License-Identifier: Apache-2.0
"""Binding OBJ vw skin weights to a supplied Skeleton (lightusd.skel)."""
import pytest

import lightusd
from lightusd import skel

SKIN_OBJ = b'''v 0 0 0
v 1 0 0
v 0 1 0
vw 0 0 0.25 1 0.75
vw 1 1 2.0
vw 2 0 0.5 1 0.25
f 1 2 3
'''

RIG = '''#usda 1.0
def SkelRoot "Rig" (prepend references = @skin.obj@) {
    def Skeleton "Skeleton" {
        uniform token[] joints = ["root", "root/a", "root/a/b"]
    }
}
'''


def _rig(tmp_path):
    (tmp_path / 'skin.obj').write_bytes(SKIN_OBJ)
    (tmp_path / 'rig.usda').write_text(RIG)
    stage = lightusd.load(tmp_path / 'rig.usda')
    mesh = next(p for p in stage if p.type_name == 'Mesh')
    return stage, mesh


def test_bind_validates_remaps_and_authors(tmp_path):
    stage, mesh = _rig(tmp_path)
    result = skel.bind_skin(stage, mesh.path, '/Rig/Skeleton', joint_remap={0: 'root/a', 1: 'root/a/b'},
                            geom_bind_transform=[[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])
    assert result.points == 3 and result.influences == 2 and result.joint_count == 3
    assert mesh.relationship('skel:skeleton').targets == ('/Rig/Skeleton',)
    assert 'SkelBindingAPI' in mesh.metadata('apiSchemas')
    assert list(mesh['primvars:skel:jointIndices'])[:2] == [1, 2]
    # Default: weights are kept exactly as authored.
    assert list(mesh['primvars:skel:jointWeights'])[:4] == pytest.approx([.25, .75, 2.0, 0.0])
    assert mesh.attribute('primvars:skel:jointIndices').metadata('elementSize') == 2
    assert 'primvars:skel:geomBindTransform' in mesh.attributes


def test_prune_normalize_and_joint_subset(tmp_path):
    stage, mesh = _rig(tmp_path)
    result = skel.bind_skin(stage, mesh.path, '/Rig/Skeleton', joints=['root/a/b', 'root'],
                            joint_remap=[1, 0], max_influences=1, normalize=True)
    assert result.influences == 1 and result.pruned_influences == 2
    assert list(mesh['primvars:skel:jointIndices']) == [0, 0, 1]
    assert list(mesh['primvars:skel:jointWeights']) == pytest.approx([1, 1, 1])
    assert list(mesh['skel:joints']) == ['root/a/b', 'root']


def test_out_of_range_joint_is_rejected_without_changes(tmp_path):
    stage, mesh = _rig(tmp_path)
    before = list(mesh['primvars:skel:jointIndices'])
    with pytest.raises(ValueError, match='out of range'):
        skel.bind_skin(stage, mesh.path, '/Rig/Skeleton', joints=['root'])
    assert list(mesh['primvars:skel:jointIndices']) == before
    assert 'skel:skeleton' not in mesh.relationships
    with pytest.raises(ValueError, match='not in'):
        skel.bind_skin(stage, mesh.path, '/Rig/Skeleton', joints=['missing'])
