#!/usr/bin/env python3
"""Linux memory guard for preview checks and serial development commands."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import time


MIB = 1024**2


def available_bytes(proc_root=Path("/proc"), cgroup_root=Path("/sys/fs/cgroup")):
    """Use the smallest host or cgroup-v2 ancestor headroom."""
    available = next(int(line.split()[1]) * 1024
                     for line in (proc_root / "meminfo").read_text().splitlines()
                     if line.startswith("MemAvailable:"))
    try:
        memberships = (proc_root / "self/cgroup").read_text().splitlines()
    except FileNotFoundError:
        memberships = []
    for line in memberships:
        if not line.startswith("0::"):
            continue
        relative = Path(line[3:].lstrip("/"))
        if ".." in relative.parts:
            continue
        current = cgroup_root / relative
        while True:
            try:
                limit = (current / "memory.max").read_text().strip()
                used = int((current / "memory.current").read_text())
                if limit != "max":
                    available = min(available, max(0, int(limit) - used))
            except FileNotFoundError:
                pass
            if current == cgroup_root:
                break
            current = current.parent
    return available


def process_tree(pgid, proc_root=Path("/proc")):
    """Snapshot workers and descendants that start their own sessions."""
    processes = {}
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            # comm can contain spaces and parentheses; fields follow its last ')'.
            fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
            processes[int(entry.name)] = (int(fields[1]), int(fields[2]), entry)
        except (FileNotFoundError, ProcessLookupError):
            pass  # A worker exited between the directory scan and read.
    included = {pgid}
    included.update(pid for pid, (_, group, _) in processes.items() if group == pgid)
    while True:
        descendants = {pid for pid, (parent, _, _) in processes.items() if parent in included}
        if descendants <= included:
            break
        included.update(descendants)
    return {pid: processes[pid][2] for pid in included if pid in processes}


def group_rss_bytes(pgid, proc_root=Path("/proc")):
    """Include all observed descendants in the command's resident budget."""
    rss = 0
    members = process_tree(pgid, proc_root)
    for entry in members.values():
        try:
            for line in (entry / "status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    rss += int(line.split()[1]) * 1024
                    break
        except (FileNotFoundError, ProcessLookupError):
            pass
    return rss


def stop_group(proc, grace=2):
    """Bound cleanup even if a worker ignores SIGTERM."""
    descendants = process_tree(proc.pid)
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    for pid in descendants:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        proc.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        pass
    # The leader may exit before a worker, so always kill remaining group members.
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    for pid in descendants:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    proc.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--max-rss-mib", type=int, default=8192)
    ap.add_argument("--reserve-mib", type=int, default=4096)
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command or min(args.max_rss_mib, args.reserve_mib, args.timeout) <= 0:
        ap.error("a command and positive limits are required")
    minimum_available = available_bytes()
    if minimum_available < args.reserve_mib * MIB:
        ap.error("insufficient available memory to start")
    started = time.monotonic()
    peak = 0
    reason = None
    proc = subprocess.Popen(command, start_new_session=True)
    try:
        while proc.poll() is None:
            peak = max(peak, group_rss_bytes(proc.pid))
            minimum_available = min(minimum_available, available_bytes())
            if peak > args.max_rss_mib * MIB:
                reason = "process group RSS limit"
            elif minimum_available < args.reserve_mib * MIB:
                reason = "available memory reserve"
            elif time.monotonic() - started > args.timeout:
                reason = "timeout"
            if reason:
                break
            time.sleep(.05)
    finally:
        stop_group(proc)
    print(f"Memory guard: peak group RSS={peak / MIB:.1f} MiB; "
          f"minimum available={minimum_available / MIB:.1f} MiB; "
          f"result={reason or proc.returncode}", flush=True)
    raise SystemExit(1 if reason else (proc.returncode if proc.returncode >= 0 else 128 - proc.returncode))


if __name__ == "__main__":
    main()
