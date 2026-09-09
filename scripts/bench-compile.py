#!/usr/bin/env python3
"""Measure clean Ninja targets or isolated translation units without shell eval.

Configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON first. Reports and temporary
objects belong in ignored build directories. Run timing passes without other
builds running; compare identical toolchains, flags, targets, and source sets.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import tempfile


def capture(argv, cwd):
    return subprocess.check_output(argv, cwd=cwd, text=True).strip()


def snapshot(root):
    digest = hashlib.sha256()
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root).split(b"\0")
    for name in sorted(set(names) - {b""}):
        path = root / os.fsdecode(name)
        digest.update(name + b"\0")
        if path.is_symlink():
            digest.update(os.fsencode(os.readlink(path)))
        elif path.is_file():
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
        else:
            digest.update(b"<deleted>")
        digest.update(b"\0")
    return {"head": capture(["git", "rev-parse", "HEAD"], root),
            "working_tree_sha256": digest.hexdigest()}


def timed(argv, cwd, directory, label):
    timing = directory / (label + ".time")
    log = directory / (label + ".log")
    environment = dict(os.environ, CCACHE_DISABLE="1", SCCACHE_RECACHE="1")
    with log.open("w") as output:
        result = subprocess.run(
            ["/usr/bin/time", "-f", "%e %U %S %M", "-o", str(timing),
             *argv], cwd=cwd, env=environment, stdout=output,
            stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")
    wall, user, system, rss = map(float, timing.read_text().split())
    return {"wall_seconds": wall, "cpu_seconds": user + system,
            "peak_child_rss_kib": int(rss)}


def summarize(samples):
    return {key: {"median": statistics.median(s[key] for s in samples),
                  "min": min(s[key] for s in samples),
                  "max": max(s[key] for s in samples)} for key in samples[0]}


def artifact(path, size_tool):
    data = path.read_bytes()
    result = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if path.suffix in (".wasm", ".js", ".mjs"):
        result["gzip_bytes"] = len(gzip.compress(data, mtime=0))
    if size_tool and path.suffix in (".o", ".a", ".wasm", ""):
        proc = subprocess.run([size_tool, "--format=berkeley", str(path)],
                              capture_output=True, text=True)
        if proc.returncode == 0:
            rows = [list(map(int, m.groups())) for line in proc.stdout.splitlines()
                    if (m := re.match(r"\s*(\d+)\s+(\d+)\s+(\d+)\s+", line))]
            if rows:
                result["sections"] = dict(zip(
                    ("text", "data", "bss"), map(sum, zip(*rows))))
        else:
            result["sections_unavailable"] = True
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--source", action="append", help="Exact repo-relative source; repeatable")
    mode.add_argument("--target", action="append", help="Ninja target to clean and rebuild; repeatable")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--jobs", type=int, default=16)
    parser.add_argument("--artifact", type=Path, action="append", default=[])
    parser.add_argument("--size-tool", default=shutil.which("llvm-size") or shutil.which("size"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repeat < 1 or args.jobs < 1:
        parser.error("repeat and jobs must be positive")
    root = Path(capture(["git", "rev-parse", "--show-toplevel"], Path.cwd()))
    build = args.build_dir.resolve()
    if not (build / "build.ninja").is_file():
        parser.error("build-dir must be a configured Ninja tree")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    logs = output.parent / (output.stem + ".logs")
    logs.mkdir(exist_ok=True)
    database = build / "compile_commands.json"
    entries = json.loads(database.read_text()) if database.exists() else []
    compilers = sorted({(e.get("arguments") or shlex.split(e["command"]))[0]
                        for e in entries})
    report = {"snapshot": snapshot(root), "platform": platform.platform(),
              "jobs": args.jobs, "repeat": args.repeat,
              "compiler_versions": {c: capture([c, "--version"], root) for c in compilers},
              "cache_sha256": hashlib.sha256((build / "CMakeCache.txt").read_bytes()).hexdigest(),
              "measurements": {}, "artifacts": {},
              "notes": ["RSS is the largest child process, not aggregate parallel-build RSS.",
                        "Timed commands disable ccache hits and force sccache recompilation.",
                        "Object file bytes include metadata; compare sections and final artifacts too."]}
    with tempfile.TemporaryDirectory(prefix="bench-compile-", dir=build) as temp:
        directory = Path(temp)
        if args.source:
            for index, source in enumerate(args.source):
                matches = [e for e in entries if Path(e["file"]).resolve() == (root / source).resolve()]
                if not matches:
                    raise RuntimeError(f"No compile command for {source}")
                # A shared support source can belong to several targets with
                # different flags. Measure every variant, never pick one by
                # accident or merge unlike object/compile-time measurements.
                for variant, entry in enumerate(matches):
                    argv = entry.get("arguments") or shlex.split(entry["command"])
                    label = f"{index}-{variant}"
                    obj = directory / f"source-{label}.o"
                    filtered = []
                    i = 0
                    while i < len(argv):
                        if argv[i] in ("-o", "-MF", "-MT", "-MQ"):
                            i += 2
                        elif argv[i] in ("-MD", "-MMD", "-MP"):
                            i += 1
                        else:
                            filtered.append(argv[i])
                            i += 1
                    filtered += ["-o", str(obj)]
                    samples = [timed(filtered, entry["directory"], logs, f"{label}-{r}")
                               for r in range(args.repeat)]
                    key = source if len(matches) == 1 else f"{source} [variant {variant + 1}]"
                    report["measurements"][key] = {
                        "samples": samples, "summary": summarize(samples),
                        "command": argv, "compiler": capture([argv[0], "--version"], root),
                        "object": artifact(obj, args.size_tool)}
        else:
            samples = []
            for r in range(args.repeat):
                subprocess.run(["ninja", "-C", str(build), "-t", "clean", *args.target], check=True)
                samples.append(timed(["cmake", "--build", str(build), "--parallel",
                                      str(args.jobs), "--target", *args.target],
                                     root, logs, f"build-{r}"))
            report["measurements"]["clean_build"] = {
                "targets": args.target, "samples": samples, "summary": summarize(samples)}
        for path in args.artifact:
            report["artifacts"][str(path)] = artifact(path, args.size_tool)
        if snapshot(root) != report["snapshot"]:
            raise RuntimeError("Source tree changed during measurement; discard this run")
        output.write_text(json.dumps(report, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    main()
