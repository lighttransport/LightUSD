# SPDX-License-Identifier: Apache-2.0
"""The optional OpenUSD comparison must not pass when its oracle is absent."""
import importlib.util
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace


def _script():
    path = Path(__file__).resolve().parents[2] / "tests/next/run-openusd-physics-check.py"
    spec = importlib.util.spec_from_file_location("openusd_physics_check", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_missing_physics_validators_fails(monkeypatch, capsys):
    pxr = ModuleType("pxr")
    pxr.Usd = SimpleNamespace()
    pxr.UsdPhysics = SimpleNamespace()
    pxr.UsdValidation = SimpleNamespace(
        ValidationRegistry=lambda: SimpleNamespace(GetOrLoadAllValidators=lambda: []))
    monkeypatch.setitem(sys.modules, "pxr", pxr)
    monkeypatch.setitem(sys.modules, "lightusd", ModuleType("lightusd"))
    monkeypatch.setattr(sys, "argv", ["check", "fixture.xml"])
    assert _script().main() == 2
    assert "physics validators unavailable" in capsys.readouterr().err


def test_unopenable_output_fails(monkeypatch, capsys):
    names = ("RigidBodyChecker", "PhysicsJointChecker", "ColliderChecker", "ArticulationChecker")
    validators = [SimpleNamespace(GetMetadata=lambda name=name: SimpleNamespace(
        name="usdPhysicsValidators:" + name)) for name in names]
    pxr = ModuleType("pxr")
    pxr.Usd = SimpleNamespace(GetVersion=lambda: (0, 26, 3),
                              Stage=SimpleNamespace(Open=lambda path: None))
    pxr.UsdPhysics = SimpleNamespace()
    pxr.UsdValidation = SimpleNamespace(ValidationRegistry=lambda: SimpleNamespace(
        GetOrLoadAllValidators=lambda: validators))
    module = ModuleType("lightusd")
    module._import = lambda **kwargs: SimpleNamespace(save=lambda path: None)
    monkeypatch.setitem(sys.modules, "pxr", pxr)
    monkeypatch.setitem(sys.modules, "lightusd", module)
    monkeypatch.setattr(sys, "argv", ["check", "--profile", "openusd", "fixture.xml"])
    assert _script().main() == 1
    assert "could not open converted stage" in capsys.readouterr().err


def test_required_physics_kind_fails_when_parser_returns_no_records(monkeypatch, capsys):
    names = ("RigidBodyChecker", "PhysicsJointChecker", "ColliderChecker", "ArticulationChecker")
    validators = [SimpleNamespace(GetMetadata=lambda name=name: SimpleNamespace(
        name="usdPhysicsValidators:" + name)) for name in names]
    pxr = ModuleType("pxr")
    pxr.Usd = SimpleNamespace(GetVersion=lambda: (0, 26, 3),
                              Stage=SimpleNamespace(Open=lambda path: object()))
    pxr.UsdPhysics = SimpleNamespace(LoadUsdPhysicsFromRange=lambda stage, paths: {})
    pxr.UsdValidation = SimpleNamespace(
        ValidationRegistry=lambda: SimpleNamespace(GetOrLoadAllValidators=lambda: validators),
        ValidationContext=lambda validators: SimpleNamespace(Validate=lambda stage: []))
    module = ModuleType("lightusd")
    module._import = lambda **kwargs: SimpleNamespace(save=lambda path: None)
    monkeypatch.setitem(sys.modules, "pxr", pxr)
    monkeypatch.setitem(sys.modules, "lightusd", module)
    monkeypatch.setattr(sys, "argv", ["check", "--profile", "openusd", "--require-kind",
                                       "RevoluteJoint=1", "fixture.xml"])
    assert _script().main() == 1
    assert "missing RevoluteJoint" in capsys.readouterr().err
