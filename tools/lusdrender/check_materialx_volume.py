#!/usr/bin/env python3
"""CPU MaterialX volume look regression using the public OpenUSD VDB fixture."""
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

from check_lusdrender_smoke import read_png_rgba, write_png_rgba


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_materialx_volume.py <lusdrender>")
    fixture = os.environ.get("LIGHTUSD_OPENUSD_VDB_FIXTURES")
    if not fixture and os.environ.get("OPENUSD_ROOT"):
        fixture = str(Path(os.environ["OPENUSD_ROOT"]) / "pxr/usdImaging/usdImagingGL/testenv/testUsdImagingGLVdbVolume")
    scene = Path(fixture or "") / "vdbVolumeTwoFields.usda"
    if not fixture or not scene.is_file():
        print("SKIP: set LIGHTUSD_OPENUSD_VDB_FIXTURES or OPENUSD_ROOT")
        return 77
    exe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="lightusd-mtlx-volume-") as directory:
        tmp = Path(directory)
        graph = tmp / "volume.mtlx"
        graph.write_text('''<materialx version="1.39">
  <geompropvalue name="density" type="float">
    <input name="geomprop" type="string" value="density"/>
    <input name="default" type="float" value="0"/>
  </geompropvalue>
  <multiply name="tint" type="color3">
    <input name="in1" type="color3" value="1,0,0"/>
    <input name="in2" type="float" nodename="density"/>
  </multiply>
  <uniform_edf name="emission" type="EDF"><input name="color" type="color3" nodename="tint"/></uniform_edf>
  <volume name="volume" type="volumeshader"><input name="edf" type="EDF" nodename="emission"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="binding" geom="/Volume" material="Mat"/></look>
</materialx>''', encoding="utf-8")
        original = graph.read_text(encoding="utf-8")

        def render(threads, name="volume", background="0,0,0", extra=(),
                   source=scene, camera="/main_cam"):
            output = tmp / f"{name}-{threads}.png"
            run = subprocess.run([exe, str(source.resolve()), str(output), "-rtPreview",
                                  "-camera", camera, "-w", "32", "-height", "32",
                                  "-samples", "1", "-threads", str(threads), "-bg", background,
                                  "--displayTransform", "srgb", "--volume-mtlx", str(graph),
                                  "--volume-mtlx-look", "Look", *extra],
                                 capture_output=True, text=True, timeout=120)
            if run.returncode:
                raise RuntimeError(f"MaterialX volume render failed: {run.stderr}")
            _, _, pixels = read_png_rgba(output)
            return pixels

        single = render(1)
        threaded = render(2)
        if single != threaded:
            raise RuntimeError("MaterialX volume render differs between worker counts")
        rgb = [single[i:i + 3] for i in range(0, len(single), 4)]
        if not any(p[0] > 8 and p[1] <= 1 and p[2] <= 1 for p in rgb):
            raise RuntimeError("field-driven red volume emission is missing")
        if any(p[1] > 2 or p[2] > 2 for p in rgb):
            raise RuntimeError("volume acquired unexpected green or blue emission")
        graph.write_text('''<materialx version="1.39">
  <uniform_edf name="emission" type="EDF"><input name="color" type="color3" value="0.002,0.004,0"/></uniform_edf>
  <volume name="volume" type="volumeshader"><input name="edf" type="EDF" nodename="emission"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="binding" geom="/Volume" material="Mat"/></look>
</materialx>''', encoding="utf-8")
        constant = render(1, "constant")
        def linear(value):
            x = value / 255.0
            return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4
        pairs = [(linear(constant[i]), linear(constant[i + 1]))
                 for i in range(0, len(constant), 4)
                 if 15 < constant[i] < 140 and constant[i + 1] < 245]
        if not pairs or any(abs(g / r - 2.0) > 0.15 for r, g in pairs):
            raise RuntimeError("zero-extinction EDF does not integrate linearly per channel")
        graph.write_text('''<materialx version="1.39">
  <time name="clock" type="float"/>
  <multiply name="tint" type="color3"><input name="in1" type="color3" value="0.002,0.004,0"/><input name="in2" type="float" nodename="clock"/></multiply>
  <uniform_edf name="emission" type="EDF"><input name="color" type="color3" nodename="tint"/></uniform_edf>
  <volume name="volume" type="volumeshader"><input name="edf" type="EDF" nodename="emission"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="binding" geom="/Volume" material="Mat"/></look>
</materialx>''', encoding="utf-8")
        animated = render(1, "time", extra=("-timecode", "24"))
        if animated != constant:
            raise RuntimeError("MaterialX volume time does not use stage timeCodesPerSecond")
        simple_scene = tmp / "camera.usda"
        scene_text = scene.read_text(encoding="utf-8")
        scene_text = scene_text.replace("@densityAndHeat.vdb@",
                                        "@" + str((scene.parent / "densityAndHeat.vdb").resolve()) + "@")
        simple_scene.write_text(scene_text + '''
def Camera "TestCam" {
  double3 xformOp:translate = (0, 0, 150)
  uniform token[] xformOpOrder = ["xformOp:translate"]
}
''', encoding="utf-8")
        scattering_scene = tmp / "scattering.usda"
        scattering_scene.write_text(scene_text.split('def Xform "Lights"')[0] + '''
 def Camera "TestCam" {
  float2 clippingRange = (0.1, 200)
  double3 xformOp:translate = (0, 0, 150)
  uniform token[] xformOpOrder = ["xformOp:translate"]
 }
 def DistantLight "TestLight" {
  color3f inputs:color = (1, 0.25, 0)
  float inputs:intensity = 4
  float xformOp:rotateY = 180
  uniform token[] xformOpOrder = ["xformOp:rotateY"]
 }
''', encoding="utf-8")
        def scattering_graph(g):
            return f'''<materialx version="1.39">
  <anisotropic_vdf name="fog" type="VDF"><input name="scattering" type="vector3" value="0.03,0.03,0.03"/><input name="anisotropy" type="float" value="{g}"/></anisotropic_vdf>
  <volume name="volume" type="volumeshader"><input name="vdf" type="VDF" nodename="fog"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="binding" geom="/Volume" material="Mat"/></look>
</materialx>'''
        graph.write_text(scattering_graph(0.6), encoding="utf-8")
        forward = render(1, "forward-scattering", source=scattering_scene, camera="/TestCam")
        if forward != render(2, "forward-scattering", source=scattering_scene, camera="/TestCam"):
            raise RuntimeError("scene-lit volume differs between worker counts")
        graph.write_text(scattering_graph(-0.6), encoding="utf-8")
        backward = render(1, "backward-scattering", source=scattering_scene, camera="/TestCam")
        if sum(forward[0::4]) <= sum(backward[0::4]) * 2:
            raise RuntimeError("scene light does not follow the anisotropic phase function")
        if not any(forward[i] > forward[i + 1] > forward[i + 2]
                   for i in range(0, len(forward), 4)):
            raise RuntimeError("volume scattering does not preserve the light color")
        def area_scene(width, rotation=180):
            return scattering_scene.read_text(encoding="utf-8").split('def DistantLight "TestLight"')[0] + f'''
def RectLight "Panel" {{
  color3f inputs:color = (1, 0, 0)
  float inputs:intensity = 30000
  float inputs:width = {width}
  float inputs:height = {width}
  float xformOp:rotateY = {rotation}
  double3 xformOp:translate = (0, 0, -120)
  uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateY"]
}}
'''
        area_small_scene, area_large_scene = tmp / "area-small.usda", tmp / "area-large.usda"
        area_small_scene.write_text(area_scene(0.5), encoding="utf-8")
        area_large_scene.write_text(area_scene(1.0), encoding="utf-8")
        graph.write_text(scattering_graph(0), encoding="utf-8")
        small_area = render(1, "area-small", source=area_small_scene, camera="/TestCam",
                            extra=("-noShadows",))
        large_area = render(1, "area-large", source=area_large_scene, camera="/TestCam",
                            extra=("-noShadows",))
        small_flux = sum(linear(x) for x in small_area[0::4])
        large_flux = sum(linear(x) for x in large_area[0::4])
        if small_flux < 0.01 or not 3.5 < large_flux / small_flux < 4.5:
            raise RuntimeError(f"area light radiance does not scale with area: {small_flux}, {large_flux}")
        back_scene = tmp / "area-back.usda"
        back_scene.write_text(area_scene(1.0, 0), encoding="utf-8")
        back_area = render(1, "area-back", source=back_scene, camera="/TestCam",
                           extra=("-noShadows",))
        if sum(back_area[0::4]) > sum(large_area[0::4]) * 0.01:
            raise RuntimeError("back-facing area light illuminated the volume")
        normalized_small, normalized_large = tmp / "normalized-small.usda", tmp / "normalized-large.usda"
        normalized_small.write_text(area_scene(0.5).replace('float inputs:width', 'bool inputs:normalize = true\n  float inputs:width'), encoding="utf-8")
        normalized_large.write_text(area_scene(1.0).replace('float inputs:width', 'bool inputs:normalize = true\n  float inputs:width'), encoding="utf-8")
        normalized_a = render(1, "area-normalized-small", source=normalized_small,
                              camera="/TestCam", extra=("-noShadows",))
        normalized_b = render(1, "area-normalized-large", source=normalized_large,
                              camera="/TestCam", extra=("-noShadows",))
        if any(abs(a - b) > 2 for a, b in zip(normalized_a, normalized_b)):
            raise RuntimeError("normalized area light changed power with its size")
        for kind in ("DiskLight", "SphereLight", "CylinderLight"):
            shape_scene = tmp / f"{kind}.usda"
            shape_scene.write_text(area_scene(1).replace('RectLight', kind).replace(
                'float inputs:width = 1', 'float inputs:radius = 3\n  float inputs:length = 5'),
                encoding="utf-8")
            shape = render(1, kind, source=shape_scene, camera="/TestCam", extra=("-noShadows",))
            if sum(shape[0::4]) <= 20:
                raise RuntimeError(f"{kind} did not illuminate the volume")
        wide_area_scene = tmp / "area-wide.usda"
        wide_area_scene.write_text(area_scene(100).replace("inputs:intensity = 30000",
                                                         "inputs:intensity = 2"), encoding="utf-8")
        def blocker(xmax):
            return f'''
def Mesh "PanelBlocker" {{
  point3f[] points = [(-200,-200,-80), ({xmax},-200,-80), ({xmax},200,-80), (-200,200,-80)]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  uniform token subdivisionScheme = "none"
  uniform bool doubleSided = true
}}
'''
        partial_scene, blocked_scene = tmp / "area-partial.usda", tmp / "area-blocked.usda"
        partial_scene.write_text(wide_area_scene.read_text(encoding="utf-8") + blocker(0), encoding="utf-8")
        blocked_scene.write_text(wide_area_scene.read_text(encoding="utf-8") + blocker(200), encoding="utf-8")
        wide_area = render(1, "area-wide", source=wide_area_scene, camera="/TestCam")
        partial_area = render(1, "area-partial", source=partial_scene, camera="/TestCam")
        blocked_area = render(1, "area-blocked", source=blocked_scene, camera="/TestCam")
        wide_sum, partial_sum, blocked_sum = (sum(p[0::4]) for p in (wide_area, partial_area, blocked_area))
        if not blocked_sum < partial_sum * 0.8 < wide_sum * 0.64:
            raise RuntimeError(f"area light did not produce a partial shadow: {blocked_sum}, {partial_sum}, {wide_sum}")
        mesh_scene = tmp / "area-mesh.usda"
        mesh_scene.write_text(scattering_scene.read_text(encoding="utf-8").split('def DistantLight "TestLight"')[0] + '''
def Mesh "Panel" (prepend apiSchemas = ["MeshLightAPI"]) {
  color3f inputs:color = (1, 0, 0)
  float inputs:intensity = 2
  point3f[] points = [(-50,-50,-120), (50,-50,-120), (50,50,-120), (-50,50,-120)]
  int[] faceVertexCounts = [3,3]
  int[] faceVertexIndices = [0,1,2,0,2,3]
  uniform token subdivisionScheme = "none"
  uniform bool doubleSided = true
}
''', encoding="utf-8")
        mesh_blocked_scene = tmp / "area-mesh-blocked.usda"
        mesh_partial_scene = tmp / "area-mesh-partial.usda"
        mesh_blocked_scene.write_text(mesh_scene.read_text(encoding="utf-8") + blocker(200), encoding="utf-8")
        mesh_partial_scene.write_text(mesh_scene.read_text(encoding="utf-8") + blocker(0), encoding="utf-8")
        mesh_unshadowed = render(1, "mesh-unshadowed", source=mesh_scene, camera="/TestCam",
                                 extra=("-noShadows",))
        mesh_lit = render(1, "mesh-lit", source=mesh_scene, camera="/TestCam")
        mesh_blocked = render(1, "mesh-blocked", source=mesh_blocked_scene, camera="/TestCam")
        mesh_partial = render(1, "mesh-partial", source=mesh_partial_scene, camera="/TestCam")
        if not (sum(mesh_unshadowed[0::4]) > 20 and sum(mesh_lit[0::4]) > 20 and
                sum(mesh_blocked[0::4]) < sum(mesh_partial[0::4]) * 0.2 and
                sum(mesh_partial[0::4]) < sum(mesh_lit[0::4]) * 0.85):
            raise RuntimeError("mesh light did not produce full and partial volume shadows")
        if mesh_lit != render(2, "mesh-lit-threaded", source=mesh_scene, camera="/TestCam"):
            raise RuntimeError("mesh-light volume render differs between worker counts")
        dome_scene = tmp / "scattering-dome.usda"
        dome_scene.write_text(scattering_scene.read_text(encoding="utf-8").split('def DistantLight "TestLight"')[0] + '''
def DomeLight "Environment" {
  color3f inputs:color = (0.7, 0.2, 0.05)
  float inputs:intensity = 0.5
}
''', encoding="utf-8")
        graph.write_text(scattering_graph(0).replace("0.03,0.03,0.03", "0,0,0"), encoding="utf-8")
        dome_baseline = render(1, "dome-baseline", source=dome_scene, camera="/TestCam",
                               extra=("-noShadows",))
        graph.write_text(scattering_graph(0.8), encoding="utf-8")
        dome_unshadowed = render(1, "dome-scattering", source=dome_scene, camera="/TestCam",
                                 extra=("-noShadows",))
        if any(abs(a - b) > 2 for a, b in zip(dome_baseline, dome_unshadowed)):
            raise RuntimeError("uniform environment does not conserve scattering radiance")
        dome_shadowed = render(1, "dome-shadowed", source=dome_scene, camera="/TestCam")
        if sum(dome_shadowed[0::4]) >= sum(dome_unshadowed[0::4]) - 10:
            raise RuntimeError("fog does not attenuate the incident environment light")
        environment = tmp / "directional-env.png"
        env_pixels = bytearray()
        for _y in range(8):
            for x in range(16):
                env_pixels.extend((255, 0, 0, 255) if x < 8 else (0, 0, 255, 255))
        write_png_rgba(environment, 16, 8, env_pixels)
        directional_scene = tmp / "directional-dome.usda"
        directional_scene.write_text(dome_scene.read_text(encoding="utf-8").replace(
            'color3f inputs:color = (0.7, 0.2, 0.05)',
            f'asset inputs:texture:file = @{environment}@'), encoding="utf-8")
        graph.write_text(scattering_graph(-0.8), encoding="utf-8")
        directional = render(1, "directional-dome", source=directional_scene, camera="/TestCam",
                             extra=("-noShadows",))
        graph.write_text(scattering_graph(0).replace("0.03,0.03,0.03", "0,0,0"), encoding="utf-8")
        directional_baseline = render(1, "directional-baseline", source=directional_scene,
                                      camera="/TestCam", extra=("-noShadows",))
        if not any(abs(directional[i] - directional_baseline[i]) > 5 and
                   abs(directional[i + 2] - directional_baseline[i + 2]) > 5
                   for i in range(0, len(directional), 4)):
            raise RuntimeError("directional dome does not affect anisotropic scattering")
        absorbing_scene = tmp / "scattering-absorber.usda"
        absorbing_scene.write_text(scattering_scene.read_text(encoding="utf-8") + '''
def Volume "Absorber" {
  rel field:density = </Volume/Density>
  float xformOp:rotateX = -90
  double3 xformOp:translate = (0, -3, -130)
  uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
}
''', encoding="utf-8")
        absorber_graph = scattering_graph(0.6).replace(
            '</materialx>', '''<absorption_vdf name="absorb" type="VDF"><input name="absorption" type="vector3" value="0.08,0,0"/></absorption_vdf>
  <volume name="absorbing" type="volumeshader"><input name="vdf" type="VDF" nodename="absorb"/></volume>
  <volumematerial name="Absorbing"><input name="volumeshader" type="volumeshader" nodename="absorbing"/></volumematerial>
</materialx>''').replace('</look>', '<materialassign name="absorber" geom="/Absorber" material="Absorbing"/></look>')
        graph.write_text(absorber_graph, encoding="utf-8")
        absorbed = render(1, "absorbing-scattering", source=absorbing_scene, camera="/TestCam")
        red_before, green_before = sum(forward[0::4]), sum(forward[1::4])
        red_after, green_after = sum(absorbed[0::4]), sum(absorbed[1::4])
        if not (red_after < red_before * 0.75 and abs(green_after - green_before) < green_before * 0.05):
            raise RuntimeError(f"colored medium did not attenuate the light: "
                               f"before {(red_before, green_before)}, after {(red_after, green_after)}")
        no_shadow_absorbed = render(1, "absorber-no-shadows", source=absorbing_scene,
                                    camera="/TestCam", extra=("-noShadows",))
        graph.write_text(scattering_graph(0.6), encoding="utf-8")
        no_shadow_forward = render(1, "forward-no-shadows", source=scattering_scene,
                                   camera="/TestCam", extra=("-noShadows",))
        if any(abs(a - b) > 1 for a, b in zip(no_shadow_absorbed, no_shadow_forward)):
            raise RuntimeError("-noShadows did not disable volume attenuation along light rays")
        vdb_tool = shutil.which("vdb_tool")
        if vdb_tool:
            sphere = tmp / "shadow-sphere.vdb"
            subprocess.run([vdb_tool, "-sphere", "radius=40", "dim=64", "name=sphere",
                            "-write", str(sphere)], check=True, capture_output=True, timeout=60)
            level_set_scene = tmp / "scattering-level-set.usda"
            level_set_scene.write_text(scattering_scene.read_text(encoding="utf-8") + f'''
def Volume "LevelSetBlocker" {{
  rel field:sdf = </LevelSetBlocker/Sdf>
  double3 xformOp:translate = (0, 0, -130)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  def OpenVDBAsset "Sdf" {{
    asset filePath = @{sphere}@
    token fieldName = "sphere"
    token fieldClass = "levelSet"
  }}
}}
''', encoding="utf-8")
            level_shadow = render(1, "level-set-shadow", source=level_set_scene, camera="/TestCam")
            level_unshadowed = render(1, "level-set-no-shadow", source=level_set_scene,
                                      camera="/TestCam", extra=("-noShadows",))
            if sum(level_shadow[0::4]) >= sum(forward[0::4]) * 0.95:
                raise RuntimeError("level-set surface does not shadow volume lighting")
            if any(abs(a - b) > 1 for a, b in zip(level_unshadowed, no_shadow_forward)):
                raise RuntimeError("-noShadows did not disable level-set shadows")
        else:
            print("SKIP: vdb_tool unavailable for level-set light shadow check")
        shadow_scene = tmp / "scattering-shadow.usda"
        shadow_scene.write_text(scattering_scene.read_text(encoding="utf-8") + '''
 def Mesh "LightBlocker" {
  point3f[] points = [(-500,-500,-120), (500,-500,-120), (500,500,-120), (-500,500,-120)]
  int[] faceVertexCounts = [4]
  int[] faceVertexIndices = [0,1,2,3]
  uniform token subdivisionScheme = "none"
  uniform bool doubleSided = true
 }
''', encoding="utf-8")
        graph.write_text(scattering_graph(0.6), encoding="utf-8")
        shadow = render(1, "shadow-scattering", source=shadow_scene, camera="/TestCam")
        if sum(shadow[0::4]) >= sum(forward[0::4]) // 4:
            raise RuntimeError("geometry does not shadow the volume's direct light")
        def view_graph(from_space, to_space, position, factor):
            return f'''<materialx version="1.39">
  <transformpoint name="p" type="vector3"><input name="in" type="vector3" value="{position}"/><input name="fromspace" type="string" value="{from_space}"/><input name="tospace" type="string" value="{to_space}"/></transformpoint>
  <extract name="z" type="float"><input name="in" type="vector3" nodename="p"/><input name="index" type="integer" value="2"/></extract>
  <multiply name="weight" type="float"><input name="in1" type="float" nodename="z"/><input name="in2" type="float" value="{factor}"/></multiply>
  <multiply name="tint" type="color3"><input name="in1" type="color3" value="0.002,0.004,0"/><input name="in2" type="float" nodename="weight"/></multiply>
  <uniform_edf name="emission" type="EDF"><input name="color" type="color3" nodename="tint"/></uniform_edf>
  <volume name="volume" type="volumeshader"><input name="edf" type="EDF" nodename="emission"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="binding" geom="/Volume" material="Mat"/></look>
</materialx>'''
        graph.write_text(view_graph("view", "world", "0,0,0", 1.0 / 150.0), encoding="utf-8")
        from_view = render(1, "from-view", source=simple_scene, camera="/TestCam")
        graph.write_text(view_graph("world", "view", "0,0,149", -1), encoding="utf-8")
        to_view = render(1, "to-view", source=simple_scene, camera="/TestCam")
        if not any(from_view[i] > 8 for i in range(0, len(from_view), 4)):
            raise RuntimeError("camera-transform fixture did not render a volume")
        if any(abs(a - b) > 1 for a, b in zip(from_view, to_view)):
            raise RuntimeError("MaterialX volume view transforms are inconsistent")
        second_volume = '''
def Volume "Volume2" {
    rel field:density = </Volume/Density>
}
'''
        before_scene, after_scene = tmp / "before.usda", tmp / "after.usda"
        before_scene.write_text(scene_text.replace('def Volume "Volume" (',
                                second_volume + '\ndef Volume "Volume" (', 1), encoding="utf-8")
        after_scene.write_text(scene_text + second_volume, encoding="utf-8")
        phase_nodes = '''<anisotropic_vdf name="red" type="VDF"><input name="scattering" type="vector3" value="0.03,0,0"/><input name="anisotropy" type="float" value="0.6"/></anisotropic_vdf>
  <anisotropic_vdf name="blue" type="VDF"><input name="scattering" type="vector3" value="0,0,0.03"/><input name="anisotropy" type="float" value="-0.6"/></anisotropic_vdf>'''
        combined_graph = f'''<materialx version="1.39">{phase_nodes}
  <add name="mixed" type="VDF"><input name="in1" type="VDF" nodename="red"/><input name="in2" type="VDF" nodename="blue"/></add>
  <volume name="volume" type="volumeshader"><input name="vdf" type="VDF" nodename="mixed"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="a" geom="/Volume" material="Mat"/></look>
</materialx>'''
        def single_phase_graph(which, absorption):
            return f'''<materialx version="1.39">{phase_nodes}
  <absorption_vdf name="other" type="VDF"><input name="absorption" type="vector3" value="{absorption}"/></absorption_vdf>
  <add name="mixed" type="VDF"><input name="in1" type="VDF" nodename="{which}"/><input name="in2" type="VDF" nodename="other"/></add>
  <volume name="volume" type="volumeshader"><input name="vdf" type="VDF" nodename="mixed"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="a" geom="/Volume" material="Mat"/></look>
</materialx>'''
        phase_scene = tmp / "phase-light.usda"
        phase_scene.write_text(scattering_scene.read_text(encoding="utf-8").replace(
            'inputs:color = (1, 0.25, 0)', 'inputs:color = (1, 0.25, 1)'), encoding="utf-8")
        graph.write_text(combined_graph, encoding="utf-8")
        combined = render(1, "phase-combined", source=phase_scene, camera="/TestCam", extra=("-noShadows",))
        reversed_graph = combined_graph.replace('nodename="red"/><input name="in2" type="VDF" nodename="blue"',
                                                'nodename="blue"/><input name="in2" type="VDF" nodename="red"')
        graph.write_text(reversed_graph, encoding="utf-8")
        reversed_phase = render(1, "phase-reversed", source=phase_scene, camera="/TestCam", extra=("-noShadows",))
        graph.write_text(single_phase_graph("red", "0,0,0.03"), encoding="utf-8")
        red_only = render(1, "phase-red", source=phase_scene, camera="/TestCam", extra=("-noShadows",))
        graph.write_text(single_phase_graph("blue", "0.03,0,0"), encoding="utf-8")
        blue_only = render(1, "phase-blue", source=phase_scene, camera="/TestCam", extra=("-noShadows",))
        if any(abs(a - b) > 1 for a, b in zip(combined, reversed_phase)):
            raise RuntimeError("composed anisotropic VDFs depend on branch order")
        if (any(abs(a - b) > 1 for a, b in zip(combined[0::4], red_only[0::4])) or
            any(abs(a - b) > 1 for a, b in zip(combined[2::4], blue_only[2::4])) or
            max(combined[0::4]) < 10 or max(combined[2::4]) < 10):
            raise RuntimeError("composed anisotropic VDF channels differ from their individual phase responses")
        two_media = '''<materialx version="1.39">
  <absorption_vdf name="a" type="VDF"><input name="absorption" type="vector3" value="0.2,0.2,0.2"/></absorption_vdf>
  <uniform_edf name="red" type="EDF"><input name="color" type="color3" value="0.2,0,0"/></uniform_edf>
  <uniform_edf name="blue" type="EDF"><input name="color" type="color3" value="0,0,0.2"/></uniform_edf>
  <volume name="r" type="volumeshader"><input name="vdf" type="VDF" nodename="a"/><input name="edf" type="EDF" nodename="red"/></volume>
  <volume name="b" type="volumeshader"><input name="vdf" type="VDF" nodename="a"/><input name="edf" type="EDF" nodename="blue"/></volume>
  <volumematerial name="Red"><input name="volumeshader" type="volumeshader" nodename="r"/></volumematerial>
  <volumematerial name="Blue"><input name="volumeshader" type="volumeshader" nodename="b"/></volumematerial>
  <look name="Look"><materialassign name="r" geom="/Volume" material="Red"/><materialassign name="b" geom="/Volume2" material="Blue"/></look>
</materialx>'''
        graph.write_text(two_media, encoding="utf-8")
        first = render(1, "red-first", source=before_scene)
        second = render(1, "blue-first", source=after_scene)
        if any(abs(a - b) > 1 for a, b in zip(first, second)):
            raise RuntimeError("overlapping MaterialX volumes depend on scene order")
        if not any(first[i] > 10 and abs(first[i] - first[i + 2]) <= 1
                   for i in range(0, len(first), 4)):
            raise RuntimeError("overlapping red and blue media did not integrate together")
        graph.write_text(two_media.replace('<materialassign name="b" geom="/Volume2" material="Blue"/>', ""),
                         encoding="utf-8")
        mixed_first = render(1, "mixed-first", source=before_scene)
        mixed_second = render(1, "mixed-second", source=after_scene)
        if any(abs(a - b) > 1 for a, b in zip(mixed_first, mixed_second)):
            raise RuntimeError("mixed MaterialX and legacy volumes depend on scene order")
        occluded_scene = tmp / "occluded.usda"
        occluded_scene.write_text(simple_scene.read_text(encoding="utf-8") + second_volume + '''
def Mesh "Occluder" {
    point3f[] points = [(-500,-500,125), (500,-500,125), (500,500,125), (-500,500,125)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    uniform token subdivisionScheme = "none"
    uniform bool doubleSided = true
}
''', encoding="utf-8")
        graph.write_text(two_media, encoding="utf-8")
        behind_red = render(1, "occluded-red", source=occluded_scene, camera="/TestCam")
        graph.write_text(two_media.replace('value="0.2,0,0"', 'value="0,0.2,0"'), encoding="utf-8")
        behind_green = render(1, "occluded-green", source=occluded_scene, camera="/TestCam")
        if not any(behind_red[i] > 2 for i in range(0, len(behind_red), 4)):
            raise RuntimeError("opaque-geometry fixture did not render")
        if behind_red != behind_green:
            raise RuntimeError("volume behind opaque geometry affected the image")
        graph.write_text(original.replace('value="density"', 'value="missing"'), encoding="utf-8")
        missing = render(1, "missing")
        if any(missing[i] > 1 for i in range(0, len(missing), 4)):
            raise RuntimeError("missing field did not use its zero default")
        graph.write_text('''<materialx version="1.39">
  <absorption_vdf name="fog" type="VDF"><input name="absorption" type="vector3" value="1,0,0"/></absorption_vdf>
  <volume name="volume" type="volumeshader"><input name="vdf" type="VDF" nodename="fog"/></volume>
  <volumematerial name="Mat"><input name="volumeshader" type="volumeshader" nodename="volume"/></volumematerial>
  <look name="Look"><materialassign name="binding" geom="/Volume" material="Mat"/></look>
</materialx>''', encoding="utf-8")
        absorption = render(1, "absorption", "1,1,1")
        if not any(absorption[i] + 8 < absorption[i + 1] and
                   abs(absorption[i + 1] - absorption[i + 2]) <= 1
                   for i in range(0, len(absorption), 4)):
            raise RuntimeError("RGB volume absorption is missing")
        negative = graph.read_text(encoding="utf-8").replace('value="1,0,0"', 'value="-1,0,0"')
        graph.write_text(negative, encoding="utf-8")
        failure = subprocess.run([exe, str(scene.resolve()), str(tmp / "invalid.png"),
                                  "-rtPreview", "-camera", "/main_cam", "-w", "16", "-height", "16",
                                  "-samples", "1", "-threads", "1", "--volume-mtlx", str(graph),
                                  "--volume-mtlx-look", "Look"],
                                 capture_output=True, text=True, timeout=120)
        if failure.returncode == 0 or "negative coefficients" not in failure.stderr:
            raise RuntimeError(f"negative volume coefficients were not rejected: {failure.stderr}")
        incompatible = subprocess.run(
            [exe, str(scene.resolve()), str(tmp / "depth.pfm"), "--volume-mtlx", str(graph),
             "--volume-mtlx-look", "Look", "--aov", "depth"],
            capture_output=True, text=True, timeout=120,
        )
        if incompatible.returncode == 0 or "require a USD color render" not in incompatible.stderr:
            raise RuntimeError("MaterialX volume look was silently ignored for a data AOV")
    print("PASS: MaterialX field-driven CPU volume render")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
