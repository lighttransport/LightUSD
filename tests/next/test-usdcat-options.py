#!/usr/bin/env python3
"""Exercise format dispatch and benchmark modes through the real CLI."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="next-usdcat-options-") as tmp:
        root = Path(tmp)
        source = root / "root.usda"
        source.write_text('#usda 1.0\n( subLayers = [@child.usda@] )\n'
                          'def Xform "Root" { float value = 2.5 }\n')
        (root / "child.usda").write_text('#usda 1.0\ndef Scope "Child" {}\n')

        def run(*args, ok=True):
            result = subprocess.run([binary, *map(str, args)], capture_output=True,
                                    env=dict(os.environ, LIGHTUSD_NEXT_TIMING="1"))
            assert (result.returncode == 0) == ok, result.stderr.decode()
            return result

        crate = root / "binary.usda"
        run("--rewrite-layer", "--output-format", "usdc", "--write-threads", 1,
            "-o", crate, source)
        assert crate.read_bytes().startswith(b"PXR-USDC")
        renamed = root / "binary.usdc"
        crate.rename(renamed)
        crate = renamed
        run("-l", crate)
        second = root / "threaded.usdc"
        run("--rewrite-layer", "--output-format", "usdc", "--write-threads", 8,
            "-o", second, source)
        assert crate.read_bytes() == second.read_bytes()
        text = root / "text.usdc"
        run("--rewrite-layer", "--output-format", "usda", "-o", text, crate)
        assert b"@child.usda@" in text.read_bytes()
        run("-f", "--output-format", "usda", "-o", text, source)
        assert text.read_bytes().startswith(b"#usda")
        assert b"subLayers" not in text.read_bytes()
        assert b'"Child"' in text.read_bytes()
        result = run("--compose-only", "--parse-threads", 1, source)
        assert b"composed: 2 prims" in result.stdout
        assert b"load+compose=" in result.stderr and b"teardown=" in result.stderr
        run("-f", "--output-format", "usdc", "-o", os.devnull, source)
        run("--compose-only", "-o", text, source, ok=False)
        run("--rewrite-layer", "--output-format", "usdc", source, ok=False)
        run("--parse-threads", "nonsense", source, ok=False)
        run("-l", "--max-crate-elements", 1, crate, ok=False)
        run("-l", "--max-crate-elements", 1024, crate)
        run("--max-crate-elements", "-1", source, ok=False)
        run("--max-crate-elements", "4294967296", source, ok=False)
        run("--max-memory-mib", "0", source, ok=False)
        run("-l", "--max-memory-mib", 16, source)
        run("--output-format", "invalid", source, ok=False)


if __name__ == "__main__":
    main()
