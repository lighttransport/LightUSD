"""LightUSD USD bridge for Blender 5.2 and newer."""

bl_info = {
    "name": "LightUSD USD Bridge",
    "author": "Light Transport Entertainment Inc.",
    "version": (0, 1, 0),
    "blender": (5, 2, 0),
    "location": "File > Import/Export",
    "category": "Import-Export",
}

from . import operators, native_hooks


def register():
    operators.register()
    native_hooks.register()


def unregister():
    native_hooks.unregister()
    operators.unregister()
