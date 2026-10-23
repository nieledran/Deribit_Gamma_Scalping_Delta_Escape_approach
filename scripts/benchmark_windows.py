"""Compare four offline demo windows from two builds; no profiles or network access.
CPU is reported as percent of one logical CPU. WM_NULL measures UI message responsiveness.
These are static demos; use performance_tests for the synthetic changing-market workload.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import statistics
import subprocess
import time

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
user = ctypes.WinDLL("user32", use_last_error=True)
kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
user.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user.GetWindow.restype = wintypes.HWND
user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
    wintypes.LPARAM, wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
user.SendMessageTimeoutW.restype = wintypes.LPARAM
def cpu(process):
    times = [wintypes.FILETIME() for _ in range(4)]
    if not kernel.GetProcessTimes(wintypes.HANDLE(int(process._handle)), *(ctypes.byref(t) for t in times)):
        raise ctypes.WinError(ctypes.get_last_error())
    return sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in times[2:]) / 1e7

def window_for(pid):
    found = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.GetWindow(hwnd, 4) == 0:
            found.append(hwnd)
        return True
    callback = callback_type(visit)
    user.EnumWindows(callback, 0)
    return found[0] if found else None

def measure(executable, seconds):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    processes = [subprocess.Popen([str(executable), "--demo"], cwd=executable.parent,
                                 startupinfo=startup) for _ in range(4)]
    try:
        time.sleep(2)
        windows = [window_for(p.pid) for p in processes]
        if any(p.poll() is not None for p in processes):
            raise RuntimeError(f"A demo process exited: {[p.poll() for p in processes]}")
        before = sum(cpu(p) for p in processes)
        started = time.perf_counter()
        delays, failures = [], 0
        while time.perf_counter() - started < seconds:
            for hwnd in windows:
                if not hwnd:
                    continue
                result = ctypes.c_size_t()
                at = time.perf_counter()
                ok = user.SendMessageTimeoutW(hwnd, 0, 0, 0, 2, 1000, ctypes.byref(result))
                delays.append((time.perf_counter() - at) * 1000)
                failures += not bool(ok)
            time.sleep(.05)
        elapsed = time.perf_counter() - started
        consumed = sum(cpu(p) for p in processes) - before
        delays.sort()
        return {"workload": "4 static offline demo windows", "elapsed_s": elapsed,
                "cpu_seconds": consumed, "cpu_percent_one_core": consumed / elapsed * 100,
                "ui_messages_available": all(windows),
                "message_p50_ms": statistics.median(delays) if delays else None,
                "message_p95_ms": delays[int(.95 * (len(delays)-1))] if delays else None,
                "message_timeouts": failures}
    finally:
        for process in processes:
            if process.poll() is None: process.terminate()
        for process in processes: process.wait(timeout=10)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--output", type=Path, default=Path("build/windows-benchmark.json"))
    args = parser.parse_args()
    data = {key: measure(path.resolve(), args.seconds)
            for key, path in (("before", args.before), ("after", args.after))}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(json.dumps(data, indent=2))
if __name__ == "__main__":
    main()
