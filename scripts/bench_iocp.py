"""Compare shared and sharded IOCP layouts manually, within a five-minute total budget."""

import argparse
from datetime import datetime
import math
from pathlib import Path
from statistics import median
import sys
import time

from support.common import ROOT, cli, file_hash, finite, read_json, require, write_json
from support.gate_process import run_owned
from support.metadata import compiler_versions, environment, source_files, timing_build
from support.paired_stats import independent_interval, summary


FAMILIES = ("WeaveIocpShardedAffine", "WeaveIocpSharedAffine", "WeaveIocpShardedStealing",
            "WeaveIocpSharedStealing", "AsioIocpSharded", "AsioIocpShared")
WORKLOADS = ((1, 1, 1024, 0, 0), (64, 2, 1024, 0, 0), (64, 4, 1024, 0, 0),
             (1024, 1, 1024, 0, 0), (1024, 2, 1024, 0, 0), (1024, 4, 1024, 0, 0),
             (1024, 8, 1024, 0, 0), (256, 4, 65536, 0, 0), (1024, 4, 1024, 512, 0),
             (1024, 4, 1024, 20000, 1))
METRICS = ("roundtrips_per_second", "client_cycles_per_op", "client_cpu_us_per_op", "p50_us", "p99_us", "p999_us")


def analyze(data, deadline=None):
    require(not any(sample.get("error_occurred") for sample in data["benchmarks"]), "A benchmark failed.")
    samples = [sample for sample in data["benchmarks"] if sample["run_type"] == "iteration"]
    require(len(samples) == len(FAMILIES) * len(WORKLOADS) * 7, "Incomplete workload matrix.")
    groups = {}
    for sample in samples:
        name = sample["run_name"]
        require(sample["samples"] >= 1000 and sample["min_connection_samples"] >= 1,
                f"Insufficient evidence or a stalled connection: {name}")
        for metric in METRICS:
            finite(sample[metric], 0, metric)
        groups.setdefault(name, []).append(sample)
    rows = []
    for scheduler in ("Affine", "Stealing"):
        for workload in WORKLOADS:
            require(deadline is None or time.monotonic() < deadline, "Five-minute analysis deadline exceeded.")
            suffix = "/" + "/".join(f"{key}:{value}" for key, value in
                                     zip(("connections", "workers", "bytes", "cpu", "uneven"), workload)) + "/iterations:1/manual_time"
            prefixes = (f"WeaveIocpSharded{scheduler}", f"WeaveIocpShared{scheduler}",
                        "AsioIocpSharded" if scheduler == "Affine" else "AsioIocpShared")
            matched = [groups.get(prefix + suffix, []) for prefix in prefixes]
            require(all(len(group) == 7 and {s["repetition_index"] for s in group} == set(range(7))
                        for group in matched), f"Missing, duplicate or unmatched repetitions: {suffix}")
            row = {"scheduler": scheduler, "workload": workload, "repetitions": 7, "metrics": {}}
            for index, metric in enumerate(METRICS):
                values = [[sample[metric] for sample in group] for group in matched]
                before, after, asio = [median(group) for group in values]
                interval = independent_interval(values[0], values[1], 1729 + index, exploratory=True) if before else None
                row["metrics"][metric] = {
                    "sharded": before, "shared": after, "asio": asio,
                    "change_pct": 100 * (after / before - 1) if before else None,
                    "ratio_ci90": [value if math.isfinite(value) else None for value in interval] if interval else None,
                    "cv_pct": [100 * summary(group)[1] if sum(group) else None for group in values],
                }
            rows.append(row)
    return {"method": "Seven randomized 250 ms windows per case; medians; independent bootstrap 90% median-ratio intervals",
            "caution": "Exploratory, not a promotion gate. Closed-loop RTT; no coordinated-omission correction. "
                       "Asio is sharded for Affine and shared for Stealing. Short windows limit tail confidence. "
                       "Uneven workload gives every eighth connection the CPU work; other connections remain active.",
            "rows": rows}


def collect(build, output, *, isolate_cpus=False):
    deadline = time.monotonic() + 300
    build, output = Path(build).resolve(strict=True), Path(output).resolve()
    require(not output.exists(), "Refusing to overwrite existing evidence.")
    timing_build(build)
    binary = (build / "Release/weave_concurrent.exe").resolve(strict=True)
    output.mkdir(parents=True)
    metadata = environment()
    metadata.update({"compiler": compiler_versions(build), "duration_ms": 250, "repetitions": 7,
                     "isolate_cpus": isolate_cpus, "executable": file_hash(binary),
                     "sources": [file_hash(path) for path in source_files()]})
    write_json(output / "environment.json", metadata)
    arguments = ["--weave_duration_ms=250", "--benchmark_filter=^(Weave|Asio)Iocp",
                 "--benchmark_repetitions=7", "--benchmark_enable_random_interleaving=true",
                 "--benchmark_display_aggregates_only=true", f"--benchmark_out={output / 'measurements.json'}",
                 "--benchmark_out_format=json"]
    if isolate_cpus:
        arguments.append("--weave_isolate_cpus")
    # Reserve time for validation and analysis; kill the benchmark and every peer on timeout.
    code = run_owned(binary, arguments, ROOT, output / "benchmark.log",
                     max(0, int((deadline - time.monotonic() - 15) * 1000)))
    require(code == 0, f"Benchmark failed or timed out ({code}); see {output / 'benchmark.log'}.")
    report = analyze(read_json(output / "measurements.json"), deadline)
    report["elapsed_seconds"] = 300 - (deadline - time.monotonic())
    write_json(output / "analysis.json", report)
    for row in report["rows"]:
        metrics = row["metrics"]
        print(f"{row['scheduler']} {row['workload']}: " + "; ".join(
            f"{metric} {metrics[metric]['change_pct']:+.1f}%" for metric in
            ("roundtrips_per_second", "client_cycles_per_op", "p99_us")))
    print(f"Evidence: {output} ({report['elapsed_seconds']:.1f}s)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, default=ROOT / "build/windows")
    parser.add_argument("--output-directory", type=Path,
                        default=ROOT / "benchmarks/results" / datetime.now().strftime("iocp-%Y%m%d-%H%M%S"))
    parser.add_argument("--isolate-cpus", action="store_true")
    args = parser.parse_args()
    collect(args.build_directory, args.output_directory, isolate_cpus=args.isolate_cpus)


if __name__ == "__main__":
    sys.exit(cli(main))
