#!/usr/bin/env python3
"""CPU MaterialX volume look regression using the public OpenUSD VDB fixture."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from check_lusdrender_smoke import read_png_rgba


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
