#!/usr/bin/env python3
"""KTX2 conversion must package a decoded image and rewrite the shader asset."""
import pathlib
import subprocess
import sys
import tempfile
import zipfile

converter, cat, root = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="lightusd-ktx2-") as directory:
    output = pathlib.Path(directory) / "converted.usdz"
    subprocess.run([converter, str(pathlib.Path(root) / "models/ktx2-uni-plane.usda"),
                    str(output), "--rootLayerFormat", "usda"], check=True)
    with zipfile.ZipFile(output) as package:
        names = package.namelist()
        images = [n for n in names if n.endswith(".png")]
        assert len(images) == 1, names
        assert package.read(images[0]).startswith(b"\x89PNG\r\n\x1a\n")
        assert not any(n.endswith(".ktx2") for n in names), names
        layers = [n for n in names if n.endswith(".usda")]
        assert len(layers) == 1, names
        text = package.read(layers[0]).decode("utf-8")
        assert "@" + images[0] + "@" in text or "@./" + images[0] + "@" in text, text
    subprocess.run([cat, str(output)], check=True, stdout=subprocess.DEVNULL)

    # A broken KTX2 must produce a clear failure, not an archive with a missing
    # or unusable texture. Keep this fixture local to the temporary directory.
    broken = pathlib.Path(directory) / "broken.ktx2"
    broken.write_bytes(b"\xabKTX 20\xbb\r\n\x1a\n")
    scene = pathlib.Path(directory) / "broken.usda"
    source = pathlib.Path(root) / "models/ktx2-uni-plane.usda"
    scene.write_text(source.read_text().replace("@ktx2-uni-plane.ktx2@", "@broken.ktx2@"))
    failed_output = pathlib.Path(directory) / "broken.usdz"
    result = subprocess.run([converter, str(scene), str(failed_output)],
                            capture_output=True, text=True)
    assert result.returncode != 0, result.stdout + result.stderr
    assert "KTX2" in result.stderr, result.stderr
    assert not failed_output.exists()
