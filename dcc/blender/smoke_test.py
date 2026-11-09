import sys
import tempfile

import bpy

from dcc.blender import converter
import dcc.blender as addon


def main():
    addon.register()
    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    out = tempfile.mktemp(suffix=".usda")
    converter.export_file(out)
    bpy.ops.object.select_all(action="SELECT")
    converter.import_file(out)
    print("LIGHTUSD_BLENDER_SMOKE_OK", out, len(bpy.data.objects))
    addon.unregister()
    bpy.ops.wm.quit_blender()


if __name__ == "__main__":
    main()
