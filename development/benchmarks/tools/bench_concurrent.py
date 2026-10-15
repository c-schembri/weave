"""Explicitly collect concurrent benchmark evidence from an existing Release build."""

import argparse
from datetime import datetime
from pathlib import Path
import sys

from analyze_concurrent import analyze, print_report
from check_concurrent_gate import PROTOCOL
from support.common import ROOT, cli, file_hash, read_json, require, run, write_json
from support.metadata import compiler_versions, environment, source_files, timing_build


def collect(build, output, *, duration_ms=1000, repetitions=7, filter=".", isolate_cpus=False, profile="full"):
    if profile == "ci":
        require(filter == ".", "The CI gate requires the full workload matrix.")
        duration_ms, repetitions = 250, 7
    else:
        require(profile == "full" and 1000 <= duration_ms <= 10000 and repetitions >= 7,
                "Performance evidence requires at least 1000 ms and 7 repetitions; use the executable directly for smoke tests.")
    build, output = Path(build).resolve(strict=True), Path(output).resolve()
    binary = (build / "Release/weave_concurrent.exe").resolve(strict=True)
    require(not any((output / name).exists() for name in ("environment.json", "paired.json", "comparison.json", "confirmation.json")),
            "Refusing to overwrite existing evidence.")
    timing_build(build)
    output.mkdir(parents=True, exist_ok=True)
    metadata = environment()
    metadata.update({
        "configuration": "Release; exceptions disabled; native Task IOCP; separate peer process",
        "reference": "Current Task with explicit as_result checks versus automatic propagation; not a historical release baseline",
        "compiler": compiler_versions(build), "duration_ms": duration_ms, "profile": PROTOCOL if profile == "ci" else "full",
        "repetitions": repetitions, "filter": filter, "peer_workers": 4, "random_interleaving": profile != "ci",
        "pairing": "Same fixture; ABBA/BAAB; explicit/explicit controls and explicit/automatic comparison; all blocks retained" if profile == "ci" else None,
        "isolate_cpus": isolate_cpus,
        "limits": {"throughput_regression_pct": 5, "cpu_cycles_per_op_regression_pct": 5, "p99_regression_pct": 10},
        "cpu_accounting": "GetProcessTimes CPU seconds/core equivalents and QueryProcessCycleTime cycles per operation; no conversion from cycles to elapsed time",
        "latency": "Closed-loop, every completed RTT recorded, p99 per repetition; no coordinated-omission correction or open-loop arrival model",
        "executable": file_hash(binary), "sources": [file_hash(path) for path in source_files()],
    })
    write_json(output / "environment.json", metadata)
    placement = ["--weave_isolate_cpus"] if isolate_cpus else []
    if profile == "ci":
        run([binary, *placement, f"--weave_paired_out={output / 'paired.json'}"])
    else:
        for name in ("comparison", "confirmation"):
            path = output / f"{name}.json"
            run([binary, *placement, f"--weave_duration_ms={duration_ms}", f"--benchmark_filter={filter}",
                 f"--benchmark_repetitions={repetitions}", "--benchmark_enable_random_interleaving=true",
                 "--benchmark_display_aggregates_only=true", f"--benchmark_out={path}", "--benchmark_out_format=json"])
            data = read_json(path)
            require(not any(sample.get("error_occurred") for sample in data["benchmarks"]), f"Invalid samples: {name}")
            raw = [sample for sample in data["benchmarks"] if sample["run_type"] == "iteration"]
            require(raw and all(sample["samples"] >= 1000 and sample["min_connection_samples"] >= 1 for sample in raw),
                    f"Insufficient samples or stalled connection: {name}")
        print_report(analyze(output))
    print(f"Evidence: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, default=ROOT / "build/windows")
    parser.add_argument("--output-directory", type=Path, default=ROOT / "benchmarks/results" / datetime.now().strftime("concurrent-%Y%m%d-%H%M%S"))
    parser.add_argument("--duration-ms", type=int, default=1000)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--filter", default=".")
    parser.add_argument("--isolate-cpus", action="store_true")
    parser.add_argument("--profile", choices=("full", "ci"), default="full")
    args = parser.parse_args()
    collect(args.build_directory, args.output_directory, duration_ms=args.duration_ms, repetitions=args.repetitions,
            filter=args.filter, isolate_cpus=args.isolate_cpus, profile=args.profile)


if __name__ == "__main__":
    sys.exit(cli(main))
