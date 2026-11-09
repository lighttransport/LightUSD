"""Bounded Linux subprocess execution, with separate exact and sampled RSS."""
import importlib.util
from pathlib import Path
import re
import subprocess
import threading
import time

_spec = importlib.util.spec_from_file_location('ousddiff_memory_guard', Path(__file__).resolve().parents[1] / 'lusdpreview/memory_guard.py')
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)
MIB = 1024 ** 2


def execute(argv, directory, timeout, env=None, max_rss_mib=30720, reserve_mib=4096,
            gpu_vram_path=None, gpu_budget_mib=None, on_start=None):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if timeout <= 0 or max_rss_mib <= 0 or reserve_mib < 0:
        raise ValueError('invalid process resource limits')
    if (gpu_vram_path is None)!=(gpu_budget_mib is None) or (gpu_budget_mib is not None and gpu_budget_mib<=0):
        raise ValueError('GPU monitoring requires a readable counter and positive budget')
    available = guard.available_bytes()
    budget = min(int(max_rss_mib * MIB), max(0, available - reserve_mib * MIB))
    row = dict(argv=list(argv), status='unavailable', wall_s=0.0,
               peak_rss_bytes=None, peak_tree_rss_bytes=0, rss_sample_interval_s=0.05,
               rss_budget_bytes=budget, stdout=str(directory / 'stdout.log'),
               stderr=str(directory / 'stderr.log'), returncode=None)
    if gpu_vram_path is not None:
        row.update(gpu_vram_counter=str(gpu_vram_path), gpu_budget_bytes=int(gpu_budget_mib*MIB),
                   peak_device_vram_bytes=0, vram_scope='whole device, including other clients')
        try:
            row['initial_device_vram_bytes']=int(Path(gpu_vram_path).read_text().strip())
            if row['initial_device_vram_bytes']<0:raise ValueError('negative GPU memory counter')
        except (OSError,ValueError) as error:
            row.update(error=str(error),reason='GPU memory counter unavailable')
            return row
        row['peak_device_vram_bytes']=row['initial_device_vram_bytes']
        if row['initial_device_vram_bytes']>row['gpu_budget_bytes']:
            row.update(status='resource_limit',reason='initial whole-device GPU memory budget')
            return row
    if budget <= 0:
        row.update(status='resource_limit', reason='available memory reserve')
        return row
    timed = ['/usr/bin/time', '-f', '__RSS__ %M\n__CPU__ %U %S', '-o', str(directory / 'time.txt'), *argv]
    started = time.monotonic()
    proc = None
    finished = threading.Event()
    ended = []
    try:
        with open(row['stdout'], 'w') as stdout, open(row['stderr'], 'w') as stderr:
            proc = subprocess.Popen(timed, stdout=stdout, stderr=stderr,
                                    env=env, start_new_session=True)
            if on_start is not None:
                on_start(proc)
            def watch():
                proc.wait()
                ended.append(time.monotonic())
                finished.set()
            threading.Thread(target=watch, daemon=True).start()
            while not finished.is_set():
                row['peak_tree_rss_bytes'] = max(row['peak_tree_rss_bytes'], guard.group_rss_bytes(proc.pid))
                if gpu_vram_path is not None:
                    try:
                        usage=int(Path(gpu_vram_path).read_text().strip())
                        if usage<0:raise ValueError('negative GPU memory counter')
                    except (OSError,ValueError):
                        row.update(status='resource_limit',reason='GPU memory counter became unavailable')
                        break
                    row['peak_device_vram_bytes']=max(row['peak_device_vram_bytes'],usage)
                    if usage>row['gpu_budget_bytes']:
                        row.update(status='resource_limit',reason='whole-device GPU memory budget')
                        break
                if time.monotonic() - started > timeout:
                    row.update(status='timeout', reason='wall time limit')
                    break
                if row['peak_tree_rss_bytes'] > budget:
                    row.update(status='resource_limit', reason='sampled process tree RSS budget')
                    break
                if guard.available_bytes() < reserve_mib * MIB:
                    row.update(status='resource_limit', reason='available memory reserve')
                    break
                finished.wait(0.05)
            if row['status'] in ('timeout', 'resource_limit'):
                guard.stop_group(proc, grace=0.2)
            else:
                proc.wait()
                row['status'] = 'ok' if proc.returncode == 0 else 'render_error'
            row['returncode'] = proc.returncode
            row['signal'] = -proc.returncode if proc.returncode < 0 else None
    except OSError as exc:
        row['error'] = str(exc)
    finally:
        # Also clean descendants whose leader exited normally or was interrupted.
        if proc is not None:
            guard.stop_group(proc, grace=0.2)
        row['wall_s'] = (ended[0] if ended else time.monotonic()) - started
    try:
        timing = (directory / 'time.txt').read_text()
        rss = re.search(r'__RSS__ (\d+)', timing)
        cpu = re.search(r'__CPU__ ([\d.]+) ([\d.]+)', timing)
        terminated = re.search(r'Command terminated by signal (\d+)', timing)
        if terminated:
            row['signal'] = int(terminated[1])
        if rss:
            row['peak_rss_bytes'] = int(rss[1]) * 1024
        if cpu:
            row.update(user_s=float(cpu[1]), system_s=float(cpu[2]))
    except OSError:
        pass
    return row


def parse_stats(log):
    stats = {}
    for key, pattern in {
        'load_s': r'^load seconds:\s*([\d.eE+-]+)',
        'render_s': r'^render seconds:\s*([\d.eE+-]+)',
        'stream_s': r'^rt triangle stream seconds:\s*([\d.eE+-]+)',
        'bvh_s': r'^rt bvh build seconds:\s*([\d.eE+-]+)',
        'triangles': r'^triangles:\s*(\d+)',
        'missing_textures': r'^rt missing textures:\s*(\d+)',
        'skipped_meshes': r'^rt skipped meshes:\s*(\d+)',
    }.items():
        match = re.search(pattern, log, re.M)
        if match:
            stats[key] = float(match[1]) if key.endswith('_s') else int(match[1])
    for key, pattern in [('backend', r'^backend:\s*(.+)'), ('device', r'^(?:Vulkan|HIP|CUDA) device:\s*(.+)')]:
        match = re.search(pattern, log, re.M)
        if match:
            stats[key] = match[1]
    if 'device' in stats:
        # RADV may interleave a loader warning with the renderer's VRAM query.
        # Keep a stable device name rather than warning text or memory suffixes.
        stats['device'] = re.sub(r'\s+\((?:WARNING:|[0-9]+\s+MiB\b).*$', '', stats['device'])
    if 'device' not in stats:
        match = re.search(r'^backend: shared (?:CUDA|HIP) RT \(([^)]+)\)', log, re.M)
        if match:
            stats['device'] = match[1]
    stats['software_device'] = bool(re.search(r'llvmpipe|lavapipe|swiftshader|software rasterizer', log, re.I))
    stats['fallback'] = 'fallback' in stats.get('backend', '').lower()
    return stats
