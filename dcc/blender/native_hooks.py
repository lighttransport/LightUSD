import bpy


class LightUSDNativeHook(bpy.types.USDHook):
    bl_idname = "LIGHTUSD_NATIVE_HOOK"
    bl_label = "LightUSD USD Bridge"

    @staticmethod
    def on_export(_context):
        return True

    @staticmethod
    def on_import(_context):
        return True

    @staticmethod
    def on_material_export(_context):
        return True

    @staticmethod
    def material_import_poll(_context):
        return False

    @staticmethod
    def on_material_import(_context):
        return True


def register():
    from .preferences import get_preferences
    try:
        if get_preferences().enable_hooks:
            bpy.utils.register_class(LightUSDNativeHook)
    except (KeyError, RuntimeError):
        pass


def unregister():
    try:
        bpy.utils.unregister_class(LightUSDNativeHook)
    except RuntimeError:
        pass
