#!/usr/bin/env python3
"""Raw USDA and decoded Crate metadata compare equally; value edits do not."""
import pathlib
import subprocess
import sys
import tempfile

diff, root = sys.argv[1:]
root = pathlib.Path(root)
ascii_file = root / "tests/usda/sceneLibrary-001.usda"
crate_file = root / "tests/usdc/sceneLibrary-001.usdc"
subprocess.run([diff, str(ascii_file), str(crate_file)], check=True)
with tempfile.TemporaryDirectory(prefix="lightusd-extension-") as directory:
    changed = pathlib.Path(directory) / "changed.usda"
    changed.write_text(ascii_file.read_text().replace('"Primary Scene"', '"Changed Scene"'))
    result = subprocess.run([diff, str(changed), str(crate_file)],
                            capture_output=True, text=True)
    assert result.returncode == 1, result.stdout + result.stderr
    assert "extensionFields" in result.stdout, result.stdout
