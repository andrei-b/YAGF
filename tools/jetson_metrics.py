#!/usr/bin/env python3
"""Read-only Jetson observations, run beside an OCR benchmark on the device."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import re
import selectors
import shutil
import subprocess
import time


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def text_file(path):
    try:
        return Path(path).read_text(errors="replace").replace("\0", "").strip()[:1024]
    except OSError:
        return None


def parse_tegrastats(line):
    result = {}
    memory = re.search(r"\bRAM (\d+)/(\d+)MB", line)
    gpu = re.search(r"\bGR3D_FREQ (\d+)%", line)
    if memory:
        result.update(ram_used_bytes=int(memory[1])*1024*1024,
                      ram_total_bytes=int(memory[2])*1024*1024)
    if gpu:
        result["gpu_percent"] = int(gpu[1])
    temperatures = re.findall(r"\b([A-Za-z0-9_]+)@(-?\d+(?:\.\d+)?)C", line)
    if temperatures:
        result["temperatures_celsius"] = {name: float(value) for name, value in temperatures}
    return result


def model_processes():
    result = []
    for process in Path("/proc").glob("[0-9]*"):
        try:
            name = (process/"comm").read_text().strip()
            if name not in ("llama-server", "vllm", "tritonserver"):
                continue
            args = (process/"cmdline").read_bytes().decode(errors="replace").split("\0")
            model = None
            for flag in ("--model", "-m"):
                if flag in args and args.index(flag)+1 < len(args):
                    model = Path(args[args.index(flag)+1]).name
            status = (process/"status").read_text()
            record = {"pid": int(process.name), "process": name, "model": model}
            for label, field in (("VmRSS", "rss_bytes"), ("VmHWM", "lifetime_peak_rss_bytes")):
                match = re.search(rf"^{label}:\s+(\d+) kB", status, re.MULTILINE)
                if match:
                    record[field] = int(match[1])*1024
            result.append(record)
        except (OSError, ValueError):
            continue
    return result


def collect(duration=10):
    report = {"schema": "yagf-hardware/1", "started_utc": utc(),
              "architecture": platform.machine(), "kernel": platform.release(),
              "device": text_file("/sys/firmware/devicetree/base/model") or text_file("/proc/device-tree/model"),
              "l4t": text_file("/etc/nv_tegra_release"),
              "scope": "System samples and inference process lifetime RSS; not per-request GPU allocation or energy",
              "processes_before": model_processes()}
    executable = shutil.which("tegrastats")
    process = subprocess.Popen([executable, "--interval", "1000"], stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL) if executable else None
    samples = []
    selector = selectors.DefaultSelector()
    pending = b""
    if process:
        os.set_blocking(process.stdout.fileno(), False)
        selector.register(process.stdout, selectors.EVENT_READ)
    started = time.monotonic()
    try:
        while time.monotonic()-started < duration:
            if not process:
                time.sleep(min(.25, max(0, duration-(time.monotonic()-started))))
                continue
            for key, _ in selector.select(.25):
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    selector.unregister(key.fileobj)
                    process.wait(timeout=2)
                    break
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    fields = parse_tegrastats(line.decode(errors="replace"))
                    if fields:
                        samples.append(dict(elapsed_seconds=round(time.monotonic()-started, 3), **fields))
                pending = pending[-65536:]
            if process.poll() is not None:
                break
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        selector.close()
    report.update(finished_utc=utc(), samples=samples, processes_after=model_processes())
    if not samples:
        report["note"] = "No tegrastats samples available; GPU utilization is unknown"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=int, default=10, help="Sampling seconds (1–300)")
    parser.add_argument("--output", help="Report file, otherwise stdout")
    args = parser.parse_args()
    if not 1 <= args.duration <= 300:
        parser.error("duration must be 1–300 seconds")
    report = json.dumps(collect(args.duration), indent=2, allow_nan=False)
    if args.output:
        Path(args.output).write_text(report+"\n")
    else:
        print(report)


if __name__ == "__main__":
    main()
