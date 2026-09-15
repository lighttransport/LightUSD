import os
import sys
from pathlib import Path

import bpy
from bpy.props import BoolProperty, EnumProperty, IntProperty, StringProperty


class LightUSDPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__
    backend_default: EnumProperty(
        name="Default backend", items=[
            ("BUILTIN", "Blender OpenUSD", "Use Blender's native USD implementation"),
            ("LIGHTUSD", "LightUSD", "Use the dependency-free LightUSD Python binding"),
            ("BUILTIN_HOOKS", "OpenUSD + hooks", "Use Blender OpenUSD with LightUSD-aware hooks"),
        ], default="LIGHTUSD")
    lightusd_path: StringProperty(
        name="LightUSD Python path", subtype="DIR_PATH",
        description="Directory containing the lightusd package and abi3 extension")
    max_memory_mb: IntProperty(name="LightUSD memory limit (MB)", default=0, min=0)
    preserve_unsupported_nodes: BoolProperty(name="Preserve unsupported shader nodes", default=True)
    enable_hooks: BoolProperty(name="Enable native USD hooks", default=True)

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, "backend_default")
        layout.prop(self, "lightusd_path")
        layout.prop(self, "max_memory_mb")
        layout.prop(self, "preserve_unsupported_nodes")
        layout.prop(self, "enable_hooks")


def get_preferences():
    addon = bpy.context.preferences.addons.get(__package__)
    if addon:
        return addon.preferences
    class Defaults:
        lightusd_path = ""
        max_memory_mb = 0
        enable_hooks = True
        preserve_unsupported_nodes = True
    return Defaults()


def load_lightusd():
    candidates = []
    pref = get_preferences()
    if pref.lightusd_path:
        candidates.append(pref.lightusd_path)
    env_path = os.environ.get("LIGHTUSD_PYTHON_PATH")
    if env_path:
        candidates.append(env_path)
    addon_root = Path(__file__).resolve().parent
    candidates.extend((str(addon_root / "vendor"), str(addon_root.parent.parent / "python")))
    for candidate in candidates:
        if candidate and candidate not in sys.path:
            sys.path.insert(0, candidate)
    try:
        import lightusd
    except ImportError as exc:
        raise RuntimeError(
            "LightUSD is unavailable. Set LightUSD Python path in Preferences "
            "or LIGHTUSD_PYTHON_PATH to the binding directory.") from exc
    return lightusd
