#!/usr/bin/env python3
"""Memory guard regressions; no scene assets or graphics devices required."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from memory_guard import MIB, available_bytes, group_rss_bytes, stop_group


class MemoryGuardTests(unittest.TestCase):
    def test_cgroup_ancestor_headroom(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            proc = root / "proc"
            (proc / "self").mkdir(parents=True)
            (proc / "meminfo").write_text("MemAvailable: 16777216 kB\n")
            (proc / "self/cgroup").write_text("0::/user/session\n")
            cg = root / "cgroup"
            leaf = cg / "user/session"
            leaf.mkdir(parents=True)
            (leaf / "memory.max").write_text("max\n")
            (leaf / "memory.current").write_text("0\n")
            (leaf.parent / "memory.max").write_text(str(4096 * MIB))
            (leaf.parent / "memory.current").write_text(str(3072 * MIB))
            self.assertEqual(available_bytes(proc, cg), 1024 * MIB)
            (leaf.parent / "memory.current").write_text(str(5000 * MIB))
            self.assertEqual(available_bytes(proc, cg), 0)
            (proc / "self/cgroup").unlink()
            self.assertEqual(available_bytes(proc, cg), 16384 * MIB)

    def test_group_includes_workers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for pid, parent, pgid, kib in ((10, 1, 10, 100), (11, 10, 10, 200),
                                          (12, 1, 12, 900), (13, 11, 13, 400)):
                entry = root / str(pid)
                entry.mkdir()
                (entry / "stat").write_text(f"{pid} (worker (name)) S {parent} {pgid} 0")
                (entry / "status").write_text(f"VmRSS: {kib} kB\n")
            self.assertEqual(group_rss_bytes(10, root), 700 * 1024)

    def test_sigterm_resistant_command_is_reaped(self):
        proc = subprocess.Popen([sys.executable, "-c",
                                 "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                                 "print('ready',flush=True); time.sleep(60)"],
                                stdout=subprocess.PIPE, start_new_session=True, text=True)
        try:
            self.assertEqual(proc.stdout.readline().strip(), "ready")
            stop_group(proc, grace=.05)
            self.assertEqual(proc.returncode, -9)
        finally:
            stop_group(proc, grace=.05)
            proc.stdout.close()

    def test_timeout_is_failure(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("memory_guard.py")),
                                 "--reserve-mib", "1", "--timeout", ".2", "--",
                                 sys.executable, "-c", "import time; time.sleep(60)"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("result=timeout", result.stdout)

    def test_worker_allocation_trips_rss_limit(self):
        worker = "import time; data=bytearray(40*1024**2); time.sleep(60)"
        parent = ("import subprocess,sys; "
                  f"subprocess.Popen([sys.executable, '-c', {worker!r}], start_new_session=True).wait()")
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("memory_guard.py")),
                                 "--reserve-mib", "1", "--max-rss-mib", "32", "--",
                                 sys.executable, "-c", parent],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("result=process group RSS limit", result.stdout)


if __name__ == "__main__":
    unittest.main()
