#!/usr/bin/env python3
"""Records a Quench program with Tracy, without the Tracy GUI, and writes zone statistics (docs/performance.md,
"Profiling with Tracy").

    python3 tools/tracy_capture.py [options] -- PROGRAM [ARGS...]
    python3 tools/tracy_capture.py [options] --stats FILE.tracy

The program is a build with QUENCH_TRACY (the `profile` preset). The script picks a free port on localhost, starts
tracy-capture on it, starts the program with TRACY_PORT set to it (pass offscreen flags: --script, --quit-after,
--audio-device null), waits for both, and exports the capture with tracy-csvexport. The statistics go to
OUTPUT_zones.csv, one row per zone, GPU zone and plot, for the whole capture ("all") and for every perf region
(perf_begin .. perf_end, found by the app's "perf_begin LABEL" and "perf_end LABEL" messages):

    region, kind (cpu, gpu, plot), name, count, per_frame, ms_per_frame, mean_ms, p50_ms, p99_ms, max_ms, total_ms,
    self_mean_ms, self_total_ms

Times of plots are their values. per_frame and ms_per_frame are the count and the total time over the region's
"app.frame" zones; a GPU zone is an interval in which a timed range kept the GPU busy, so a range's ms_per_frame is
its busy time per frame, as in the app's perf lines. p50 and p99 are nearest-rank percentiles of single zones; self
times leave out the zone's child zones. Tracy's statistics leave out zones shorter than its timer's tick (about 42 ns
on Apple silicon), so tiny zones are undercounted. A table of the zones with the most time is printed per region.

Options:
    --build-dir DIR     where tracy-capture and tracy-csvexport are (default: build/profile)
    --output PATH       output path without suffix (default: build/profile/tracy/capture): PATH.tracy,
                        PATH_zones.csv, PATH_app.log, PATH_capture.log
    --seconds N         stop recording after N seconds (the program keeps running)
    --timeout N         give up on the program after N seconds (default 900)
    --top N             rows per table (default 15)
    --expect NAME       fail unless a CPU zone (or plot) of that name was recorded; repeatable
    --expect-gpu NAME   the same for a GPU zone

Exit code 0: recorded, exported and every expected zone found; 1 otherwise. Standard library only.
"""

import csv
import os
import re
import socket
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

ENGINE_DIR = Path(__file__).resolve().parents[1]
SEP = "\t"


def fail(message):
    print(f"tracy_capture: {message}", file=sys.stderr)
    sys.exit(1)


def parse_args(argv):
    opts = {"build_dir": ENGINE_DIR / "build" / "profile", "output": None, "seconds": None, "timeout": 900.0,
            "top": 15, "expect": [], "expect_gpu": [], "stats": None, "program": []}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--":
            opts["program"] = argv[i + 1:]
            break
        value = argv[i + 1] if i + 1 < len(argv) else None
        if a in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        if value is None:
            fail(f"{a} needs a value")
        if a == "--build-dir":
            opts["build_dir"] = Path(value)
        elif a == "--output":
            opts["output"] = Path(value)
        elif a == "--seconds":
            opts["seconds"] = int(value)
        elif a == "--timeout":
            opts["timeout"] = float(value)
        elif a == "--top":
            opts["top"] = int(value)
        elif a == "--expect":
            opts["expect"].append(value)
        elif a == "--expect-gpu":
            opts["expect_gpu"].append(value)
        elif a == "--stats":
            opts["stats"] = Path(value)
        else:
            fail(f"unknown option {a} (--help)")
        i += 2
    if not opts["program"] and not opts["stats"]:
        fail("no program after -- and no --stats file (--help)")
    if opts["output"] is None:
        opts["output"] = opts["stats"].with_suffix("") if opts["stats"] else opts["build_dir"] / "tracy" / "capture"
    return opts


def tool(build_dir, name):
    path = Path(build_dir) / name
    if not path.exists():
        fail(f"{path} not found: build the profile preset (cmake --build --preset profile)")
    return str(path)


def free_port():
    # Away from Tracy's default ports (8086 and the 19 after it), which other profiled programs may hold.
    for _ in range(50):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        if not 8086 <= port < 8106:
            return port
    fail("no free port")


def record(opts, trace):
    capture = tool(opts["build_dir"], "tracy-capture")
    port = free_port()
    out = opts["output"]
    out.parent.mkdir(parents=True, exist_ok=True)
    command = [capture, "-o", str(trace), "-a", "127.0.0.1", "-p", str(port), "-f"]
    if opts["seconds"]:
        command += ["-s", str(opts["seconds"])]
    with open(f"{out}_app.log", "w") as app_log:
        env = dict(os.environ, TRACY_PORT=str(port))
        started = time.monotonic()
        print(f"tracy_capture: recording on port {port}: {' '.join(opts['program'])}", flush=True)
        app = subprocess.Popen(opts["program"], stdout=app_log, stderr=subprocess.STDOUT, env=env)
        # On macOS tracy-capture started before the program listens fails at once ("disconnected during the
        # initial connection handshake", exit code 3): it is started again until it connects.
        while True:
            with open(f"{out}_capture.log", "w") as capture_log:
                attempt = time.monotonic()
                capturer = subprocess.Popen(command, stdout=capture_log, stderr=subprocess.STDOUT)
                try:
                    capture_code = capturer.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    break
            if capture_code != 3 or time.monotonic() - attempt > 1.5 or app.poll() is not None:
                capturer = None
                break
            time.sleep(0.05)
        try:
            app_code = app.wait(timeout=max(1.0, opts["timeout"] - (time.monotonic() - started)))
        except subprocess.TimeoutExpired:
            app.kill()
            if capturer:
                capturer.kill()
            fail(f"the program ran longer than {opts['timeout']:.0f} s ({out}_app.log)")
        if capturer:
            try:
                capture_code = capturer.wait(timeout=120)
            except subprocess.TimeoutExpired:
                capturer.kill()
                fail(f"tracy-capture did not finish after the program ({out}_capture.log)")
    print(f"tracy_capture: program exit code {app_code}, {time.monotonic() - started:.1f} s; tracy-capture exit code "
          f"{capture_code}", flush=True)
    if capture_code != 0 or not trace.exists():
        fail(f"no capture ({out}_capture.log)")
    return app_code


def export(csvexport, trace, *flags):
    result = subprocess.run([csvexport, "-s", SEP, *flags, str(trace)], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"tracy-csvexport {' '.join(flags)}: {result.stderr.strip()}")
    lines = result.stdout.splitlines()
    if not lines or "There are currently no" in lines[0]:
        return []
    return list(csv.DictReader(lines, delimiter=SEP))


def percentile(sorted_values, p):
    return sorted_values[min(len(sorted_values) - 1, int(p * len(sorted_values)))]


def regions_of(messages, end_time):
    regions = [("all", 0, end_time)]
    open_regions = {}
    for row in messages:
        text, t = row["MessageName"], int(row["total_ns"])
        begin = re.match(r"^perf_begin (.+)$", text)
        end = re.match(r"^perf_end (.+)$", text)
        if begin:
            open_regions[begin.group(1)] = t
        elif end and end.group(1) in open_regions:
            regions.append((end.group(1), open_regions.pop(end.group(1)), t))
    return regions


def statistics(opts, trace):
    csvexport = tool(opts["build_dir"], "tracy-csvexport")
    cpu = export(csvexport, trace, "-u", "-p")
    cpu_self = export(csvexport, trace, "-u", "-e")
    gpu = export(csvexport, trace, "-g")
    messages = export(csvexport, trace, "-m")

    events = []  # (kind, name, start ns, value: ms, self ms)
    self_ms = {(r["name"], r["ns_since_start"], r["thread"]): int(r["exec_time_ns"]) * 1e-6 for r in cpu_self}
    for row in cpu:
        if row["src_file"]:
            events.append(("cpu", row["name"], int(row["ns_since_start"]), int(row["exec_time_ns"]) * 1e-6,
                           self_ms.get((row["name"], row["ns_since_start"], row["thread"]))))
        elif row["value"]:
            events.append(("plot", row["name"], int(row["ns_since_start"]), float(row["value"]), None))
    for row in gpu:
        events.append(("gpu", row["name"], int(row["Time from start of program"]),
                       int(row["GPU execution time"]) * 1e-6, None))
    end_time = max([e[2] for e in events] + [int(m["total_ns"]) for m in messages] + [0]) + 1

    rows = []
    found = set()
    for label, start, end in regions_of(messages, end_time):
        groups = defaultdict(list)
        for kind, name, t, value, own in events:
            if start <= t < end:
                groups[(kind, name)].append((value, own))
        frames = len(groups.get(("cpu", "app.frame"), []))
        for (kind, name), values in groups.items():
            found.add((kind, name))
            ms = sorted(v for v, _ in values)
            selfs = [s for _, s in values if s is not None]
            rows.append({
                "region": label, "kind": kind, "name": name, "count": len(ms),
                "per_frame": f"{len(ms) / frames:.3f}" if frames and kind != "plot" else "",
                "ms_per_frame": f"{sum(ms) / frames:.4f}" if frames and kind != "plot" else "",
                "mean_ms": f"{sum(ms) / len(ms):.4f}", "p50_ms": f"{percentile(ms, 0.5):.4f}",
                "p99_ms": f"{percentile(ms, 0.99):.4f}", "max_ms": f"{ms[-1]:.4f}",
                "total_ms": f"{sum(ms):.3f}" if kind != "plot" else "",
                "self_mean_ms": f"{sum(selfs) / len(selfs):.4f}" if selfs else "",
                "self_total_ms": f"{sum(selfs):.3f}" if selfs else ""})
    path = Path(f"{opts['output']}_zones.csv")
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["region"])
        writer.writeheader()
        writer.writerows(rows)

    for label in dict.fromkeys(r["region"] for r in rows):
        region = [r for r in rows if r["region"] == label]
        frames = next((r["count"] for r in region if r["kind"] == "cpu" and r["name"] == "app.frame"), 0)
        print(f"\n{label}: {frames} frames")
        for kind, key in (("cpu", "self_total_ms"), ("gpu", "total_ms")):
            top = sorted((r for r in region if r["kind"] == kind), key=lambda r: -float(r[key] or 0))[:opts["top"]]
            if not top:
                continue
            title = "CPU zones by self time" if kind == "cpu" else "GPU zones by time"
            print(f"  {title}: name, count, per frame; ms per frame, mean, p50, p99"
                  + (", self mean" if kind == "cpu" else ""))
            for r in top:
                extra = f" {float(r['self_mean_ms']):8.3f}" if kind == "cpu" and r["self_mean_ms"] else ""
                print(f"    {r['name'][:36]:36s} {r['count']:7d} {r['per_frame'] or '-':>7s} "
                      f"{float(r['ms_per_frame'] or 0):8.3f} {float(r['mean_ms']):8.3f} {float(r['p50_ms']):8.3f} "
                      f"{float(r['p99_ms']):8.3f}{extra}")
    print(f"\ntracy_capture: {path}")

    missing = [f"zone {n}" for n in opts["expect"] if ("cpu", n) not in found and ("plot", n) not in found]
    missing += [f"GPU zone {n}" for n in opts["expect_gpu"] if ("gpu", n) not in found]
    return missing


def main():
    opts = parse_args(sys.argv[1:])
    trace = opts["stats"] or Path(f"{opts['output']}.tracy")
    app_code = 0
    if opts["program"]:
        app_code = record(opts, trace)
    print(f"tracy_capture: {trace}", flush=True)
    missing = statistics(opts, trace)
    if missing:
        fail("not recorded: " + ", ".join(missing))
    if app_code != 0:
        fail(f"the program exited with code {app_code} ({opts['output']}_app.log)")


if __name__ == "__main__":
    main()
