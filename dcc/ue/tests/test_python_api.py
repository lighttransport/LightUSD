import importlib.util
import sys
from pathlib import Path


def _load_api():
    path = Path(__file__).parents[1] / "Content/Python/lightusd_ue/api.py"
    spec = importlib.util.spec_from_file_location("lightusd_ue_api", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_api_is_safe_outside_unreal():
    api = _load_api()
    assert api.capabilities()["unreal"] is False
    assert set(api.capabilities()["backends"]) == {"auto", "native", "lightusd"}
