# SPDX-License-Identifier: Apache-2.0
import pytest
import lightusd


@pytest.mark.parametrize('joint',['<joint name="root" type="hinge"/>','<joint name="root" type="slide"/>',''])
def test_world_anchored_articulation_root_is_on_base_body(joint):
    # MuJoCo's USD decoder treats any ArticulationRootAPI prim as a body, so a
    # fixed-base articulation must put the API on the base body rather than on
    # the world-anchoring joint. The reference MuJoCo converter does the same.
    xml=f'<mujoco><worldbody><body name="body">{joint}<geom size=".1" mass="1"/></body></worldbody></mujoco>'
    stage=lightusd.load_bytes(xml.encode(),format='mjcf',profile='newton')
    body=stage.prim_at('/World/Links/body')
    apis=body.metadata('apiSchemas') or ()
    assert 'PhysicsArticulationRootAPI' in apis
    assert 'NewtonArticulationRootAPI' in apis
    roots=[p for p in stage if 'PhysicsArticulationRootAPI' in (p.metadata('apiSchemas') or ())]
    assert [p.path for p in roots]==[body.path]
    assert not [p for p in stage if p.type_name.endswith('Joint')
                and 'PhysicsArticulationRootAPI' in (p.metadata('apiSchemas') or ())]


def test_floating_articulation_root_stays_on_body():
    stage=lightusd.load_bytes(b'<mujoco><worldbody><body name="body"><freejoint/><geom size=".1" mass="1"/></body></worldbody></mujoco>',format='mjcf',profile='newton')
    body=stage.prim_at('/World/Links/body')
    assert 'PhysicsArticulationRootAPI' in body.metadata('apiSchemas')


def test_visual_plane_authors_texture_domain_for_usd_consumers(tmp_path):
    path = tmp_path / "plane.xml"
    path.write_text('<mujoco><worldbody><geom name="floor" type="plane" size="1 1 .01"/></worldbody></mujoco>')
    stage = lightusd.load(str(path))
    planes = [p for p in stage if p.type_name == "Plane"]
    assert len(planes) == 1
    st = planes[0].attribute("primvars:st")
    assert st.get().tolist() == [(1, 0), (0, 0), (0, 1), (1, 1)]
    assert st.metadata("interpolation") == "vertex"
