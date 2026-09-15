import bpy
from bpy_extras.io_utils import ExportHelper, ImportHelper
from bpy.props import EnumProperty, StringProperty

from . import converter
from .preferences import LightUSDPreferences, get_preferences


def _backend_items(_self, _context):
    return [("BUILTIN", "Blender OpenUSD", ""), ("LIGHTUSD", "LightUSD", ""),
            ("BUILTIN_HOOKS", "OpenUSD + hooks", "")]


class LIGHTUSD_OT_import(bpy.types.Operator, ImportHelper):
    bl_idname = "lightusd.import_usd"
    bl_label = "Import USD (LightUSD Bridge)"
    filename_ext = ".usd"
    filter_glob: StringProperty(default="*.usd;*.usda;*.usdc;*.usdz", options={"HIDDEN"})
    backend: EnumProperty(name="Backend", items=_backend_items)

    def execute(self, _context):
        if self.backend == "LIGHTUSD":
            converter.import_file(self.filepath)
        else:
            bpy.ops.wm.usd_import(filepath=self.filepath, import_materials=True,
                                   import_curves=True, import_meshes=True)
        return {"FINISHED"}


class LIGHTUSD_OT_export(bpy.types.Operator, ExportHelper):
    bl_idname = "lightusd.export_usd"
    bl_label = "Export USD (LightUSD Bridge)"
    filename_ext = ".usdc"
    filter_glob: StringProperty(default="*.usd;*.usda;*.usdc;*.usdz", options={"HIDDEN"})
    backend: EnumProperty(name="Backend", items=_backend_items)

    def execute(self, _context):
        if self.backend == "LIGHTUSD":
            converter.export_file(self.filepath, selected=False)
        else:
            bpy.ops.wm.usd_export(filepath=self.filepath, export_materials=True,
                                   export_hair=True, export_curves=True, export_animation=True,
                                   export_custom_properties=True, generate_materialx_network=True)
        return {"FINISHED"}


CLASSES = (LightUSDPreferences, LIGHTUSD_OT_import, LIGHTUSD_OT_export)


def _menu_import(self, _context):
    self.layout.operator(LIGHTUSD_OT_import.bl_idname, text="USD (LightUSD Bridge)")


def _menu_export(self, _context):
    self.layout.operator(LIGHTUSD_OT_export.bl_idname, text="USD (LightUSD Bridge)")


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
