"""Matched, sequential PostgreSQL client-concurrency measurements; never correctness CI."""

import argparse
import datetime
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time

from run import source_fingerprint


LIBRARIES = ("weave-affine", "weave-stealing", "libpq")
WORKLOADS = ("simple", "prepared", "batch", "rows")
ORDERS = tuple(itertools.permutations(LIBRARIES))


def placement(cores, workers, reserve):
    flat = [cpu for core in cores for cpu in core]
    if (workers < 1 or reserve < 0 or not cores or any(not core for core in cores) or
            any(type(cpu) is not int or cpu < 0 for cpu in flat) or len(flat) != len(set(flat))):
        raise ValueError("Missing or ambiguous CPU topology")
    if workers > len(cores) - reserve:
        return None
    return [core[0] for core in cores[-workers:]]


def distribution(values):
    median = statistics.median(values)
    return {
        "median": median,
        "spread_pct": 100 * (max(values) - min(values)) / median,
        "cv_pct": 100 * statistics.stdev(values) / statistics.mean(values),
    }


def summary(samples, cpu_time_reliable=True):
    output = {}
    for key in ("ops_per_second", "p50_us", "p95_us", "p99_us"):
        output[key] = distribution([sample[key] for sample in samples])
    output["cpu_pct"] = None
    output["cpu_us_per_operation"] = None
    if cpu_time_reliable:
        output["cpu_pct"] = statistics.median(100 * sample["cpu_seconds"] / sample["seconds"] for sample in samples)
        output["cpu_us_per_operation"] = statistics.median(
            1_000_000 * sample["cpu_seconds"] / sample["operations"] for sample in samples)
    output["cycles_per_operation"] = None
    if all(sample["cpu_cycles"] > 0 for sample in samples):
        output["cycles_per_operation"] = distribution(
            [sample["cpu_cycles"] / sample["operations"] for sample in samples])
    return output


def quality(samples, statistics_row, seconds, cpu_time_reliable):
    short = min(sample["seconds"] for sample in samples) < seconds * 0.75
    cycles = statistics_row["cycles_per_operation"]
    return {
        "throughput": "short" if short else "noisy" if statistics_row["ops_per_second"]["spread_pct"] > 10 else "stable",
        "p99": "short" if short else "noisy" if statistics_row["p99_us"]["spread_pct"] > 30 else "stable",
        "cpu_time": "available" if cpu_time_reliable else "unreliable counter; unavailable",
        "cycles": "unavailable" if cycles is None else "short" if short else "noisy" if cycles["spread_pct"] > 10 else "stable",
    }


def paired_interval(candidate, baseline, key, inverse=False):
    ratios = [left[key] / right[key] for left, right in zip(candidate, baseline, strict=True)]
    if inverse:
        ratios = [1 / value for value in ratios]
    generator = random.Random(728391)
    estimates = sorted(statistics.median(generator.choices(ratios, k=len(ratios))) for _ in range(10000))
    return {"median": statistics.median(ratios), "low": estimates[250], "high": estimates[9749]}


def validate_metrics(metrics, workload, connections, iterations, batch):
    expected = connections * iterations
    multiplier = batch if workload == "batch" else 1
    if (type(metrics["operations"]) is not int or type(metrics["roundtrips"]) is not int or
            metrics["operations"] != expected * multiplier or metrics["roundtrips"] != expected):
        raise ValueError("Missing or extra completed operations")
    fields = ("seconds", "cpu_seconds", "ops_per_second", "p50_us", "p95_us", "p99_us")
    if any(type(metrics[key]) not in (int, float) or not math.isfinite(metrics[key]) for key in fields):
        raise ValueError("Nonfinite metrics")
    if metrics["seconds"] <= 0 or metrics["cpu_seconds"] < 0 or metrics["ops_per_second"] <= 0:
        raise ValueError("Invalid timing or CPU counter")
    if not 0 < metrics["p50_us"] <= metrics["p95_us"] <= metrics["p99_us"]:
        raise ValueError("Invalid latency distribution")
    rate = metrics["operations"] / metrics["seconds"]
    if not math.isclose(rate, metrics["ops_per_second"], rel_tol=0.00001):
        raise ValueError("Throughput is inconsistent with completed operations")
    if (type(metrics["checksum"]) is not int or not 0 <= metrics["checksum"] < 2**64 or
            type(metrics["libpq_version"]) is not int or metrics["libpq_version"] < 180000):
        raise ValueError("Missing result checksum or unsupported libpq")
    if type(metrics["cpu_cycles"]) is not int or metrics["cpu_cycles"] < 0:
        raise ValueError("Invalid raw CPU cycle count")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=55432)
    parser.add_argument("--ca", default="plain")
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 2, 4])
    parser.add_argument("--connections", type=int, default=32)
    parser.add_argument("--reserve-cores", type=int, default=2)
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=32)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=list(WORKLOADS))
    parser.add_argument("--server-description", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (not math.isfinite(args.seconds) or args.seconds <= 0 or args.repetitions < 3 or args.reserve_cores < 0 or
            not 1 <= args.connections <= 256 or not 1 <= args.warmup <= 10000 or not 1 <= args.batch <= 256 or
            not 1 <= args.port <= 65535 or any(not 1 <= worker <= 32 for worker in args.workers)):
        parser.error("Invalid measurement bounds")
    if len(set(args.workers)) != len(args.workers) or len(set(args.workloads)) != len(args.workloads):
        parser.error("Repeated worker counts or workloads")
    if args.output.exists() or not os.environ.get("WEAVE_PG_PASSWORD"):
        parser.error("Use a new evidence path and set WEAVE_PG_PASSWORD")
    args.executable = args.executable.resolve(strict=True)
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
    topology = subprocess.run([str(args.executable), "--topology"], env=environment,
                              check=True, capture_output=True, text=True, timeout=10)
    cores = json.loads(topology.stdout)["cores"]
    placements = {workers: placement(cores, workers, args.reserve_cores) for workers in args.workers}
    if all(cpus is None for cpus in placements.values()):
        parser.error("No requested worker count fits the physical-core budget")
    if any(workers > args.connections for workers in args.workers if placements[workers] is not None):
        parser.error("At least one session is required per worker")

    cpu_time_reliable = platform.system() != "Windows"
    evidence = {
        "time": datetime.datetime.now(datetime.UTC).isoformat(),
        "platform": platform.platform(), "processor": platform.processor(), "logical_cpus": os.cpu_count(),
        "arguments": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "topology": json.loads(topology.stdout), "placements": placements,
        "source_sha256": source_fingerprint(),
        "executable_sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
        "records": [], "summaries": [], "complete": False,
        "before": None, "change_pct": None,
        "boundaries": {
            "startup_warmup_cleanup": "outside measurement", "latency": "complete request, or complete batch through Sync",
            "cpu": "entire client process; server/WSL host CPU excluded",
            "cpu_time_reliable": cpu_time_reliable,
            "windows_cpu_time": "GetProcessTimes underreported sustained worker activity, independently confirmed with Get-Process; raw readings retained, not ranked",
            "cycles": "Windows QueryProcessCycleTime across all process threads; raw cycles, not converted to time or utilization; unavailable on Linux",
            "libpq_driver": "nonblocking PQsend/PQflush/PQconsumeInput/PQisBusy/PQgetResult; poll or WSAPoll per worker",
            "server_placement": "externally controlled; description is mandatory but does not certify isolation",
        },
    }

    def run(library, workload, workers, iterations, phase, repetition):
        cpus = ",".join(str(cpu) for cpu in placements[workers])
        command = [str(args.executable), library, workload, str(workers), str(args.connections), str(iterations),
                   str(args.batch), args.host, str(args.port), args.ca, cpus, str(args.warmup)]
        record = {"library": library, "workload": workload, "workers": workers, "connections": args.connections,
                  "iterations": iterations, "phase": phase, "repetition": repetition, "command": command}
        evidence["records"].append(record)
        start = time.monotonic()
        try:
            process = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=180)
        except subprocess.TimeoutExpired as failure:
            record.update(exit_code=None, failure="timeout", stdout=str(failure.stdout), stderr=str(failure.stderr),
                          wall_seconds=time.monotonic() - start)
            raise
        record.update(exit_code=process.returncode, stdout=process.stdout, stderr=process.stderr,
                      wall_seconds=time.monotonic() - start)
        if process.returncode:
            record["failure"] = "nonzero exit"
            raise RuntimeError(f"{library}/{workload}/{workers} failed: {process.stderr}")
        try:
            record["metrics"] = json.loads(process.stdout)
            validate_metrics(record["metrics"], workload, args.connections, iterations, args.batch)
        except (ValueError, KeyError, TypeError) as failure:
            record["failure"] = f"Invalid metrics: {failure}"
            raise
        return record["metrics"]

    try:
        print("| Workers | Workload | Client | ops/s | / libpq | Spread % | P99 us | CPU % | Cycles/op | Quality |", flush=True)
        print("| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |", flush=True)
        for workers in args.workers:
            if placements[workers] is None:
                print(f"| {workers} | unavailable: insufficient physical cores after reservation |", flush=True)
                continue
            for workload in args.workloads:
                count = 128
                pilots = [run(library, workload, workers, count, "pilot", 0) for library in LIBRARIES]
                if len({pilot["checksum"] for pilot in pilots}) != 1:
                    raise RuntimeError("Pilot consumed results differ")
                minimum = min(value["seconds"] for value in pilots)
                if minimum < 0.2:
                    count = min(2_000_000 // args.connections, math.ceil(count * 0.2 / minimum))
                    pilots = [run(library, workload, workers, count, "calibration", 0) for library in LIBRARIES]
                    if len({pilot["checksum"] for pilot in pilots}) != 1:
                        raise RuntimeError("Calibration consumed results differ")
                    minimum = min(value["seconds"] for value in pilots)
                count = min(2_000_000 // args.connections, max(count, math.ceil(count * args.seconds / minimum)))
                samples = {library: [] for library in LIBRARIES}
                for repetition in range(args.repetitions):
                    pair = {}
                    for library in ORDERS[repetition % len(ORDERS)]:
                        pair[library] = run(library, workload, workers, count, "sample", repetition)
                        samples[library].append(pair[library])
                    if len({value["checksum"] for value in pair.values()}) != 1:
                        raise RuntimeError("Measured consumed results differ")
                baseline = samples["libpq"]
                baseline_statistics = summary(baseline, cpu_time_reliable)
                baseline_quality = quality(baseline, baseline_statistics, args.seconds, cpu_time_reliable)
                for library, values in samples.items():
                    statistics_row = summary(values, cpu_time_reliable)
                    interval = paired_interval(values, baseline, "ops_per_second")
                    p99_interval = paired_interval(values, baseline, "p99_us")
                    rate = statistics_row["ops_per_second"]
                    p99 = statistics_row["p99_us"]
                    metric_quality = quality(values, statistics_row, args.seconds, cpu_time_reliable)
                    comparable = {
                        key: metric_quality[key] == baseline_quality[key] == "stable"
                        for key in ("throughput", "p99", "cycles")
                    }
                    notes = [f"{key} {value}" for key, value in metric_quality.items() if value not in ("stable", "available")]
                    cycles = statistics_row["cycles_per_operation"]
                    cpu_text = f"{statistics_row['cpu_pct']:.1f}" if cpu_time_reliable else "unavailable"
                    cycles_text = f"{cycles['median']:,.0f}" if cycles else "unavailable"
                    evidence["summaries"].append({
                        "workers": workers, "workload": workload, "library": library, "statistics": statistics_row,
                        "throughput_ratio": interval, "p99_ratio": p99_interval, "quality": metric_quality,
                        "comparable_to_libpq": comparable,
                        "cycles_ratio": paired_interval(values, baseline, "cpu_cycles") if cycles and baseline_statistics["cycles_per_operation"] else None,
                    })
                    print(f"| {workers} | {workload} | {library} | {rate['median']:,.0f} | {interval['median']:.2f}x | "
                          f"{rate['spread_pct']:.1f} | {p99['median']:.1f} | {cpu_text} | {cycles_text} | "
                          f"{', '.join(notes) or 'stable'} |", flush=True)
        if source_fingerprint() != evidence["source_sha256"]:
            raise RuntimeError("Source changed during measurements; diagnostic only")
        if hashlib.sha256(args.executable.read_bytes()).hexdigest() != evidence["executable_sha256"]:
            raise RuntimeError("Executable changed during measurements; diagnostic only")
        evidence["complete"] = True
    except BaseException as failure:
        evidence["failure"] = f"{type(failure).__name__}: {failure}"
        raise
    finally:
        evidence["source_sha256_after"] = source_fingerprint()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(evidence, output, indent=2)


if __name__ == "__main__":
    main()
