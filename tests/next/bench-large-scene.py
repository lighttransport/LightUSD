#!/usr/bin/env python3
"""Paired, warm-input next-core benchmarks; serialized bytes go to /dev/null.

The manifest is a JSON list of {name, input, mode, format, payloads,
compose_threads, parse_threads, write_threads, args}. mode defaults to flatten;
parse, compose and rewrite are also supported. Paths are supplied locally,
never embedded in the repository. Reports retain commands and binary hashes.
Use --legacy-baseline for the CLI preceding explicit output-format support.
Use --max-load N to refuse to start when the 1-minute load average is above N
(concurrent builds/benchmarks invalidate paired timings).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import statistics
import subprocess
import time


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def available_memory():
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    raise RuntimeError("MemAvailable is unavailable")


def check_headroom(min_available, max_load=0.0):
    if available_memory() < min_available:
        raise RuntimeError("host available memory fell below the benchmark reserve")
    fs = os.statvfs("/dev/shm")
    if (fs.f_blocks - fs.f_bfree) * fs.f_frsize > 48_000_000_000:
        raise RuntimeError("/dev/shm usage exceeds the 48 GB cap")
    if max_load > 0:
        load1 = float(Path("/proc/loadavg").read_text().split()[0])
        if load1 > max_load:
            raise RuntimeError(
                f"host load {load1:.2f} exceeds --max-load {max_load:.2f}; "
                "wait for concurrent builds/benchmarks to finish")


def sample_child_memory(pid):
    """Sample the command below /usr/bin/time, including resident swap."""
    try:
        children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
        if not children:
            return {}
        proc = Path(f"/proc/{children[0]}")
        status = (proc / "status").read_text().splitlines()
        sample = {line.split(':')[0]: int(line.split()[1]) for line in status
                  if line.startswith(("VmRSS:", "VmHWM:", "VmSwap:", "Threads:",
                                      "RssAnon:", "RssFile:", "RssShmem:"))}
        # The command name may contain spaces or parentheses. Fields after
        # its final ')' start with state (field 3 in proc_pid_stat(5)).
        stat = (proc / "stat").read_text().rsplit(')', 1)[1].split()
        sample.update(state=stat[0], minor_faults=int(stat[7]),
                      major_faults=int(stat[9]), user_ticks=int(stat[11]),
                      system_ticks=int(stat[12]))
        return sample
    except (FileNotFoundError, ProcessLookupError):
        return {}  # process exited between the poll and the sample


def command(binary, case, legacy, null_crate):
    mode = case.get("mode", "flatten")
    fmt = case.get("format", "usda")
    if mode not in ("parse", "compose", "rewrite", "flatten"):
        raise ValueError("unknown benchmark mode: " + mode)
    if fmt not in ("usda", "usdc"):
        raise ValueError("unknown output format: " + fmt)
    if legacy and mode == "rewrite" and fmt == "usdc":
        raise ValueError("legacy CLI cannot rewrite an uncomposed layer to USDC")
    args = [str(binary)]
    args += {"parse": ["-l"], "compose": ["-f"] if legacy else ["--compose-only"],
             "rewrite": ["--rewrite-layer"], "flatten": ["-f"]}[mode]
    if mode in ("compose", "flatten"):
        args += ["--compose-threads", str(case.get("compose_threads", 0)),
                 "--load-payloads" if case.get("payloads", True) else "--defer-payloads"]
    if not legacy:
        args += ["--parse-threads", str(case.get("parse_threads", 0)),
                 "--write-threads", str(case.get("write_threads", 0))]
    if mode in ("rewrite", "flatten") or (mode == "compose" and legacy):
        if legacy:
            args += ["-o", str(null_crate) if fmt == "usdc" else "/dev/null"]
        else:
            args += ["--output-format", fmt, "-o", "/dev/null"]
    args += case.get("args", [])
    # Preserve symlink overlay anchors: resolving the entry's real path would
    # bypass sibling files supplied by a merged asset package.
    args += [str(Path(case["input"]).absolute())]
    return args


def parse_log(log):
    metrics = {}
    for line in log.splitlines():
        prims = re.fullmatch(r"(?:loaded|composed): (\d+) prims", line)
        if prims:
            metrics["prim_count"] = int(prims.group(1))
        match = re.search(r"\[(next_[a-z_]+|bench)\] (.*)", line)
        if not match:
            continue
        tag, body = match.groups()
        for key, value, unit in re.findall(
                r"([\w+]+)=([0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?)(ms)?", body):
            name = tag + "." + key + ("_ms" if unit else "")
            # Reader phases occur for multiple external layers; retain their
            # sum as CPU-work attribution, not an end-to-end wall-time sum.
            metrics[name] = metrics.get(name, 0) + float(value)
        wrote = re.search(r"wrote (\d+) bytes", body)
        if wrote:
            metrics["output_bytes"] = int(wrote.group(1))
    return metrics


def run_one(binary, case, legacy, args, label, sequence):
    check_headroom(args.min_available_gib * 1024**3, args.max_load)
    case_name = case["name"]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", case_name):
        raise ValueError("case name must be a simple filename")
    log_path = args.output / f"{case_name}.{label}.{sequence}.log"
    cmd = command(binary, case, legacy, args.output / "null.usdc")
    env = dict(os.environ, LIGHTUSD_NEXT_TIMING="1",
               LIGHTUSD_NEXT_NUM_THREADS=str(case.get("write_threads", 0)))
    time_format = "[bench] elapsed=%e user=%U system=%S peak_rss_kib=%M major_faults=%F minor_faults=%R"
    started = time.monotonic()
    with log_path.open("w") as log, log_path.with_suffix(".memory.jsonl").open("w") as memory_log:
        process = subprocess.Popen(["/usr/bin/time", "-f", time_format] + cmd,
                                   stdout=log, stderr=log, env=env,
                                   start_new_session=True)
        try:
            next_sample = started
            while process.poll() is None:
                check_headroom(args.min_available_gib * 1024**3, args.max_load)
                now = time.monotonic()
                if now >= next_sample:
                    sample = sample_child_memory(process.pid)
                    sample.update(elapsed=now - started, log_bytes=log.tell())
                    memory_log.write(json.dumps(sample) + "\n")
                    memory_log.flush()
                    max_swap_mib = getattr(args, "max_process_swap_mib", 0)
                    if (max_swap_mib > 0 and
                            sample.get("VmSwap", 0) > max_swap_mib * 1024):
                        raise RuntimeError(
                            f"child swap exceeds {max_swap_mib:g} MiB; "
                            "paging invalidates the timed comparison")
                    next_sample = now + 1
                if now - started > args.timeout:
                    raise RuntimeError("benchmark timeout")
                time.sleep(0.25)
        except BaseException:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
    text = log_path.read_text()
    metrics = parse_log(text)
    samples = [json.loads(line) for line in
               log_path.with_suffix(".memory.jsonl").read_text().splitlines()]
    metrics["bench.sampled_peak_swap_kib"] = max(
        (s.get("VmSwap", 0) for s in samples), default=0)
    metrics["bench.sampled_peak_rss_plus_swap_kib"] = max(
        (s.get("VmRSS", 0) + s.get("VmSwap", 0) for s in samples), default=0)
    record = dict(case=case_name, binary=label, sequence=sequence,
                  command=cmd, returncode=process.returncode,
                  metrics=metrics, log=str(log_path))
    with (args.output / "runs.jsonl").open("a") as output:
        output.write(json.dumps(record) + "\n")
    if process.returncode:
        raise RuntimeError(f"{case_name}/{label} failed; see {log_path}")
    print(json.dumps(record["metrics"], sort_keys=True), flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--legacy-baseline", action="store_true")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=10800)
    parser.add_argument("--min-available-gib", type=float, default=16)
    parser.add_argument("--max-process-swap-mib", type=float, default=0,
                        help="stop when sampled child swap exceeds this limit "
                             "(0 disables the limit)")
    parser.add_argument("--max-load", type=float, default=0,
                        help="refuse to start when the 1-minute load average "
                             "exceeds this value (0 disables)")
    parser.add_argument("--max-rss-growth", type=float, default=0.05,
                        help="maximum allowed growth of the largest measured peak RSS")
    args = parser.parse_args()
    if args.repeat < 1 or args.warmup < 0:
        parser.error("repeat must be positive and warmup nonnegative")
    if args.max_process_swap_mib < 0:
        parser.error("max-process-swap-mib must be nonnegative")
    if args.max_load < 0:
        parser.error("max-load must be nonnegative")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "null.usdc").symlink_to("/dev/null")
    cases = json.loads(args.manifest.read_text())
    binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    metadata = {name: {"path": str(path), "sha256": sha256(path)}
                for name, path in binaries.items()}
    metadata.update(manifest=cases, repeat=args.repeat, warmup=args.warmup,
                    malloc_conf=os.environ.get("MALLOC_CONF", ""),
                    min_available_gib=args.min_available_gib,
                    max_process_swap_mib=args.max_process_swap_mib,
                    max_rss_growth=args.max_rss_growth,
                    clock_ticks_per_second=os.sysconf("SC_CLK_TCK"))
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    records = []
    for case in cases:
        for iteration in range(-args.warmup, args.repeat):
            order = ["baseline", "candidate"]
            if iteration % 2:
                order.reverse()
            for name in order:
                print(f"{case['name']} {name} {iteration}", flush=True)
                record = run_one(binaries[name], case,
                                 args.legacy_baseline and name == "baseline",
                                 args, name, iteration)
                if iteration >= 0:
                    records.append(record)
    summary = {}
    for case in cases:
        by_binary = {}
        for name in binaries:
            samples = [r["metrics"] for r in records
                       if r["case"] == case["name"] and r["binary"] == name]
            keys = set.intersection(*(set(sample) for sample in samples))
            by_binary[name] = {key: {"min": min(s[key] for s in samples),
                                     "median": statistics.median(s[key] for s in samples),
                                     "max": max(s[key] for s in samples)}
                               for key in sorted(keys)}
        summary[case["name"]] = by_binary
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    comparisons = {}
    for case, values in summary.items():
        baseline, candidate = values["baseline"], values["candidate"]
        rss_ratio = (candidate["bench.peak_rss_kib"]["max"] /
                     baseline["bench.peak_rss_kib"]["max"])
        comparisons[case] = {
            "elapsed_median_ratio": (candidate["bench.elapsed"]["median"] /
                                     baseline["bench.elapsed"]["median"]),
            "peak_rss_max_ratio": rss_ratio,
            "rss_pass": rss_ratio <= 1 + args.max_rss_growth,
        }
    (args.output / "comparison.json").write_text(json.dumps(comparisons, indent=2) + "\n")
    if not all(case["rss_pass"] for case in comparisons.values()):
        raise RuntimeError("peak RSS gate failed; see comparison.json (retain logs and investigate)")


if __name__ == "__main__":
    main()
