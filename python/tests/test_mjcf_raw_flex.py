# SPDX-License-Identifier: Apache-2.0
"""Raw MJCF <deformable><flex> (as mj_saveXML writes flexcomps) -> soft bodies."""
import numpy as np
import pytest

import lightusd


def _point(name, pos, extra=""):
    return (f'<body name="{name}" pos="{pos}"><inertial pos="0 0 0" mass="0.25" diaginertia="1e-6 1e-6 1e-6"/>'
            f'<joint type="slide" axis="1 0 0" damping=".1"/><joint type="slide" axis="0 1 0"/>'
            f'<joint type="slide"/>{extra}</body>')


def _model(flex_extra="", root_extra="", tagged=""):
    # A 2x2 cloth: v0 rides the holder, v1-v3 are point masses (v3 may carry a site).
    bodies = (_point("v1", ".1 0 1") + _point("v2", "0 .1 1") + _point("v3", ".1 .1 1", tagged))
    return (f'<mujoco><worldbody><body name="holder" pos="0 0 1"><geom type="box" size=".01 .01 .01"/>'
            f'</body>{bodies}</worldbody><deformable>'
            f'<flex name="cloth" dim="2" radius=".004" rgba=".2 .4 .8 1" body="holder v1 v2 v3" '
            f'vertex="0 0 0  0 0 0  0 0 0  0 0 0" element="0 1 3  0 3 2" {flex_extra}>'
            f'<elasticity young="1e4" poisson=".3" thickness=".002" elastic2d="stretch"/></flex>'
            f'</deformable>{root_extra}</mujoco>').encode()


def test_point_bodies_are_absorbed_and_the_holder_pins():
    stage = lightusd.load_bytes(_model(), format="mjcf")
    cloth = stage.prim_at("/World/Deformables/cloth")
    assert cloth.type_name == "Mesh"
    assert "PhysicsSurfaceDeformableSimAPI" in cloth.metadata("apiSchemas")
    pts = np.asarray(cloth["points"]).reshape(-1, 3)
    assert pts == pytest.approx(np.array([[0, 0, 1], [.1, 0, 1], [0, .1, 1], [.1, .1, 1]]))
    assert list(cloth["faceVertexIndices"]) == [0, 1, 3, 0, 3, 2]
    assert list(cloth["physics:masses"]) == pytest.approx([.25] * 4)
    pins = stage.prim_at("/World/Deformables/cloth_pins")
    assert list(pins["physics:indices0"]) == [0]
    assert pins.relationship("physics:src1").targets == ("/World/Links/holder",)
    links = {p.name for p in stage if p.path.startswith("/World/Links/")}
    assert "holder" in links and not links & {"v1", "v2", "v3"}
    assert not [p for p in stage if p.type_name == "PhysicsPrismaticJoint"]
    assert "vertex joint damping is dropped" in stage.warnings()


def test_referenced_vertex_bodies_and_world_vertices_pin():
    xml = _model(tagged='<site name="tag"/>').replace(b'body="holder v1', b'body="world v1')
    stage = lightusd.load_bytes(xml, format="mjcf")
    links = {p.name for p in stage if p.path.startswith("/World/Links/")}
    assert "v3" in links and not links & {"v1", "v2"}
    world = stage.prim_at("/World/Deformables/cloth_pins")
    assert list(world["physics:indices0"]) == [0] and "physics:src1" not in world.relationships
    body = stage.prim_at("/World/Deformables/cloth_pins_1")
    assert list(body["physics:indices0"]) == [3]
    assert body.relationship("physics:src1").targets == ("/World/Links/v3",)


@pytest.mark.parametrize("variant, message", [
    (dict(root_extra='<keyframe><key qpos="0 0 0 0 0 0 0 0 0"/></keyframe>'), "keyframes"),
    (dict(flex_extra='dof="trilinear"'), "interpolated"),
    (dict(elements="0 1 7"), "element or body reference is missing"),
    (dict(elements="0 1 2.5"), "element indices must be int32 values"),
    (dict(elements="0 1 2147483648"), "element indices must be int32 values"),
])
def test_unconvertible_flexes_are_reported(variant, message):
    elements = variant.pop("elements", None)
    xml = _model(**variant)
    if elements:
        xml = xml.replace(b'element="0 1 3  0 3 2"', f'element="{elements}"'.encode())
    stage = lightusd.load_bytes(xml, format="mjcf")
    assert stage.get_prim_at("/World/Deformables/cloth") is None
    assert message in stage.warnings()
    assert {"v1", "v2", "v3"} <= {p.name for p in stage if p.path.startswith("/World/Links/")}
    with pytest.raises(lightusd.UsdError):
        lightusd.load_bytes(xml, format="mjcf", unsupported_policy="error")


def test_other_profiles_keep_reporting_the_deformable():
    stage = lightusd._import(data=_model(), format="mjcf", profile="openusd")
    assert stage.get_prim_at("/World/Deformables/cloth") is None
    assert "Unsupported MJCF element: deformable" in stage.warnings()
    assert {"v1", "v2", "v3"} <= {p.name for p in stage if p.path.startswith("/World/Links/")}
