# SPDX-License-Identifier: Apache-2.0
"""Portable light-filter registry exposed through the Python binding."""
import pytest

import lightusd

FIXTURE = """#usda 1.0
def Scope "World"
{
    def LightFilter "Half"
    {
        uniform token lightusd:filter:kind = "intensity"
        float lightusd:filter:intensity = 0.5
    }
    def LightFilter "Gain"
    {
        uniform token lightusd:filter:kind = "gain"
        float lightusd:filter:value0 = 0.25
        color3f lightusd:filter:color = (0.5, 1, 1)
    }
    def LightFilter "Mystery"
    {
        uniform token lightusd:filter:kind = "not-registered"
    }
}
"""


def test_builtin_kinds_listed():
    kinds = lightusd.light_filter_kinds()
    assert "intensity" in kinds
    assert "color" in kinds


def test_builtin_filter_effective():
    stage = lightusd.loads(FIXTURE)
    intensity, color = lightusd.light_filter_effective(stage, "/World/Half")
    assert intensity == pytest.approx(0.5)
    assert color == pytest.approx((1.0, 1.0, 1.0))


def test_python_custom_kind_drives_the_registry():
    stage = lightusd.loads(FIXTURE)
    seen = {}

    def gain(params):
        seen.update(params)
        return params["values"][0], params["color"]

    lightusd.register_light_filter("gain", gain)
    try:
        intensity, color = lightusd.light_filter_effective(stage, "/World/Gain")
        assert intensity == pytest.approx(0.25)
        assert color == pytest.approx((0.5, 1.0, 1.0))
        assert seen["kind"] == "gain"
        assert seen["prim_path"] == "/World/Gain"
        assert len(seen["values"]) == 8
    finally:
        lightusd.unregister_light_filter("gain")


def test_unknown_kind_reports_and_never_applies():
    stage = lightusd.loads(FIXTURE)
    with pytest.raises(ValueError) as info:
        lightusd.light_filter_effective(stage, "/World/Mystery")
    assert "not-registered" in str(info.value)


def test_registration_validation():
    with pytest.raises(ValueError):
        lightusd.register_light_filter("intensity", lambda params: 1.0)
    with pytest.raises(TypeError):
        lightusd.register_light_filter("bad", 3)
    with pytest.raises(ValueError):
        lightusd.unregister_light_filter("not-registered")
