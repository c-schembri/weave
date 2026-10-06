"""Bounded Windows CI comparison and publication of its latest complete main-branch report."""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import random
import re
from statistics import median
import sys
import time

from bench_tokio import BACKENDS, analyze, measure
from support.common import ROOT, capture, cli, file_hash, read_json, require, run, write_json
from support.gate_process import run_owned
from support.metadata import compiler_versions, environment, source_files, timing_build
from support.paired_stats import interval


PROTOCOL = "windows-tcp-ci-v1"
REPETITIONS = 7
DURATION_MS = 1000
WARMUP_MS = 250
TIMEOUT_MS = 300_000
SEED = 60106
RUST_VERSION = "1.94.0"
WORKLOADS = (
    ("64-small", 64, 1024, 0, False),
    ("1024-small", 1024, 1024, 0, False),
    ("64-large", 64, 65536, 0, False),
    ("256-uneven-cpu", 256, 1024, 20000, True),
)
LABELS = ("64 clients / 1 KiB", "1,024 clients / 1 KiB", "64 clients / 64 KiB", "256 clients / uneven CPU")
BEGIN = "<!-- benchmark-results:start -->"
END = "<!-- benchmark-results:end -->"


def validate_masks(masks):
    require(set(masks) == {"server", "client"}, "Unexpected CPU partition.")
    require(all(type(value) is int and 0 < value < 2**64 for value in masks.values()), "Invalid CPU masks.")
    workers = masks["server"].bit_count()
    require(workers in (1, 2) and masks["client"].bit_count() == workers and
            not masks["server"] & masks["client"], "CI requires one or two physically isolated cores per process.")
    return workers


def schedule():
    # Every block includes every backend exactly once for each workload. No selective reruns.
    randomizer = random.Random(SEED)
    for repetition in range(REPETITIONS):
        workloads = list(WORKLOADS)
        randomizer.shuffle(workloads)
        for workload in workloads:
            backends = list(BACKENDS)
            randomizer.shuffle(backends)
            for backend in backends:
                yield repetition, workload, backend


def report(samples, metadata):
    require(metadata.get("protocol") == PROTOCOL and metadata.get("repetitions") == REPETITIONS and
            metadata.get("duration_ms") == DURATION_MS and metadata.get("warmup_ms") == WARMUP_MS and
            metadata.get("seed") == SEED, "Wrong CI protocol.")
    workers = validate_masks(metadata["cpu_masks"])
    require(metadata["workers"] == workers and metadata["client_workers"] == workers, "Mismatched worker budgets.")
    require(re.fullmatch(r"[0-9a-f]{40}", metadata["revision"]) is not None, "Invalid source revision.")
    require(type(metadata["worktree"]) is list, "Missing worktree provenance.")
    require(all(sample.get("server_workers") == workers for sample in samples), "Mismatched server worker counts.")
    require(all(DURATION_MS / 1000 <= sample["wall_seconds"] <= 2.0 for sample in samples),
            "A sample missed the fixed measurement window; do not publish a stalled run.")
    result = analyze(samples, WORKLOADS, BACKENDS, REPETITIONS)
    rows = {(row["workload"], row["backend"]): row for row in result["rows"]}
    paired = []
    for workload in WORKLOADS:
        groups = {backend: {sample["repetition"]: sample for sample in samples
                            if sample["workload"] == workload[0] and sample["backend"] == backend}
                  for backend in BACKENDS}
        for baseline in ("asio", "tokio"):
            ratios = [groups["weave"][index]["roundtrips_per_second"] /
                      groups[baseline][index]["roundtrips_per_second"] for index in range(REPETITIONS)]
            low, high = interval(ratios)
            center = median(ratios)
            paired.append({"workload": workload[0], "baseline": baseline, "ratio": center,
                           "ci90": [low, high], "noisy": (high - low) / center > 0.20})
    for row in rows.values():
        metrics = row["metrics"]
        row["throughput_noisy"] = metrics["roundtrips_per_second"]["cv_pct"] > 10
        row["p99_noisy"] = metrics["p99_us"]["cv_pct"] > 25
        row["noisy"] = row["throughput_noisy"] or row["p99_noisy"]
        row["client_busy"] = metrics["client_cores"]["median"] >= workers * 0.90
    throughput_noisy = any(row["throughput_noisy"] for row in rows.values()) or any(pair["noisy"] for pair in paired)
    p99_noisy = any(row["p99_noisy"] for row in rows.values())
    result.update({"paired": paired, "throughput_noisy": throughput_noisy, "p99_noisy": p99_noisy,
                   "noisy": throughput_noisy or p99_noisy})
    return result


def quality_notes(rows, paired=()):
    notes = []
    if any(row["throughput_noisy"] for row in rows) or any(pair["noisy"] for pair in paired):
        notes.append("throughput noisy")
    if any(row["p99_noisy"] for row in rows):
        notes.append("p99 noisy")
    if any(row["client_busy"] for row in rows):
        notes.append("client busy")
    return "; ".join(notes) or "within limits"


def precision_summary(result):
    if result["throughput_noisy"]:
        status = "**Noisy throughput comparisons are inconclusive; see per-case details.**"
    else:
        status = "Throughput variation is within limits."
    noisy_tails = sum(row["p99_noisy"] for row in result["rows"])
    if noisy_tails:
        unit = "measurement is" if noisy_tails == 1 else "measurements are"
        status += f" **{noisy_tails} library/workload p99 {unit} noisy; tail-latency comparisons involving them are inconclusive.**"
    return status


def compact_markdown(result, metadata):
    workers = metadata["workers"]
    revision = metadata["revision"]
    repository = metadata.get("repository", "c-schembri/weave")
    require(repository == "c-schembri/weave", "Unexpected publication repository.")
    run_id = metadata.get("run_id", "")
    require(not run_id or re.fullmatch(r"[0-9]+", run_id), "Invalid workflow run ID.")
    source = f"[`{revision[:7]}`](https://github.com/{repository}/commit/{revision})"
    if metadata["worktree"]:
        source += " (uncommitted working tree)"
    link = f"[full results and raw evidence](https://github.com/{repository}/actions/runs/{run_id})" if run_id else "local validation"
    timestamp = datetime.fromisoformat(metadata["timestamp"]).strftime("%Y-%m-%d %H:%M %z")
    cpu = ", ".join(str(cpu["Name"]) for cpu in metadata.get("cpu", [])) or "CPU not recorded"
    cpu = cpu.replace("|", "/").replace("\r", " ").replace("\n", " ")
    lines = [f"Latest complete run: {source}, {timestamp}; {link}.", "",
             f"Windows x64 / {cpu}; server/client workers: {workers}/{workers}, on separate cores. "
             f"{REPETITIONS} x {DURATION_MS / 1000:g}s per library/workload. Median round trips/second; higher is better.", "",
             "| Workload | Weave | Asio | Tokio | Max throughput CV | Notes |",
             "| --- | ---: | ---: | ---: | ---: | --- |"]
    rows = {(row["workload"], row["backend"]): row for row in result["rows"]}
    for workload, label in zip(WORKLOADS, LABELS, strict=True):
        metrics = [rows[workload[0], backend]["metrics"]["roundtrips_per_second"] for backend in BACKENDS]
        weave, tokio, asio = metrics
        case_rows = [rows[workload[0], backend] for backend in BACKENDS]
        case_pairs = [pair for pair in result["paired"] if pair["workload"] == workload[0]]
        notes = quality_notes(case_rows, case_pairs)
        lines.append(f"| {label} | {weave['median']:,.0f} | {asio['median']:,.0f} | {tokio['median']:,.0f} | "
                     f"{max(metric['cv_pct'] for metric in metrics):.1f}% | {notes} |")
    lines.extend(["", precision_summary(result)])
    if any(row["client_busy"] for row in result["rows"]):
        lines.extend(["", "Client busy: at least one backend's load generator used >=90% of its core budget. "
                      "These are end-to-end loopback results, not maximum server capacity."])
    lines.extend(["", "CPU use, p99/p99.9 latency, paired confidence intervals, and limitations: "
                  "[benchmark protocol](docs/ci-benchmarks.md)."])
    return "\n".join(lines) + "\n"


def full_markdown(result, metadata):
    lines = ["# Windows benchmark results", "", compact_markdown(result, metadata),
             "## Per-library measurements", "",
             "Medians of seven independent windows; CV is between-window variation, not per-request variation.", "",
             "| Workload | Library | RTT/s | p99 ms | p99.9 ms | Server CPU us/op* | Server kcycles/op | Client cores* | Private MiB | RTT CV | p99 CV | Quality |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"]
    for row in result["rows"]:
        metrics = row["metrics"]
        value = lambda key: metrics[key]["median"]
        quality = quality_notes([row])
        lines.append(f"| {row['workload']} | {row['backend']} | {value('roundtrips_per_second'):,.0f} | "
                     f"{value('p99_us') / 1000:.3f} | {value('p999_us') / 1000:.3f} | "
                     f"{value('server_cpu_us_per_op'):.2f} | {value('server_cycles_per_op') / 1000:.2f} | "
                     f"{value('client_cores'):.2f} | {value('server_private_mb'):.2f} | "
                     f"{metrics['roundtrips_per_second']['cv_pct']:.1f}% | {metrics['p99_us']['cv_pct']:.1f}% | {quality} |")
    lines.extend(["", "*Windows CPU-time counters can be quantized (including zero); diagnostic only. "
                  "Cycles/op is measured independently, never converted into CPU seconds.", "",
                  "## Matched Throughput Ratios", "", "Weave / baseline; above 1 favors Weave. "
                  "90% whole-block bootstrap intervals describe this run, not cross-machine regressions.", "",
                  "| Workload | Baseline | Median ratio | Paired CI90 | Precision |",
                  "| --- | --- | ---: | --- | --- |"])
    for pair in result["paired"]:
        low, high = pair["ci90"]
        lines.append(f"| {pair['workload']} | {pair['baseline']} | {pair['ratio']:.3f} | "
                     f"[{low:.3f}, {high:.3f}] | {'noisy' if pair['noisy'] else 'within thresholds'} |")
    lines.extend(["", "All samples retained; no outlier deletion, adaptive stopping, or selective retries. "
                  "These closed-loop loopback tests do not measure open-loop service latency or internet performance.", "",
                  f"Measurement supervisor elapsed: {metadata['elapsed_seconds']:.1f}s; hard limit: 300s. "
                  "Configure, build, and artifact publication are outside that limit."])
    return "\n".join(lines) + "\n"


def worker(args):
    start = time.monotonic()
    masks = json.loads(capture([args.load_binary, "--ci-cpu-masks"]))
    args.server_workers = args.client_workers = validate_masks(masks)
    args.duration_ms, args.warmup_ms, args.smoke = DURATION_MS, WARMUP_MS, False
    metadata = read_json(args.output_directory / "environment.json")
    metadata.update(cpu_masks=masks, workers=args.server_workers, client_workers=args.client_workers)
    write_json(args.output_directory / "environment.json", metadata)
    samples = []
    for repetition, workload, backend in schedule():
        sample = measure(args, backend, workload, repetition, masks)
        samples.append(sample)
        write_json(args.output_directory / "samples.json", {"samples": samples, "cpu_masks": masks})
        print(f"{len(samples):02d}/84 {workload[0]} {backend}: {sample['roundtrips_per_second']:,.0f} RTT/s", flush=True)
    metadata["elapsed_seconds"] = time.monotonic() - start
    result = report(samples, metadata)
    write_json(args.output_directory / "environment.json", metadata)
    write_json(args.output_directory / "analysis.json", result)
    write_json(args.output_directory / "publication.json", {"metadata": metadata, "samples": samples})
    (args.output_directory / "summary.md").write_text(full_markdown(result, metadata), encoding="utf-8")
    print(full_markdown(result, metadata), flush=True)


def benchmark(args):
    require(os.name == "nt", "This benchmark requires Windows.")
    args.build_directory = args.build_directory.resolve(strict=True)
    args.output_directory = args.output_directory.resolve()
    args.server_binary = (args.build_directory / "Release/weave_runtime_server.exe").resolve(strict=True)
    args.load_binary = (args.build_directory / "Release/weave_runtime_load.exe").resolve(strict=True)
    args.tokio_binary = args.tokio_binary.resolve(strict=True)
    if args.worker:
        return worker(args)
    timing_build(args.build_directory)
    require(not args.output_directory.exists(), "Refusing to overwrite existing evidence.")
    args.output_directory.mkdir(parents=True)
    provenance = environment()
    provenance.update({"protocol": PROTOCOL, "repetitions": REPETITIONS, "duration_ms": DURATION_MS,
                       "warmup_ms": WARMUP_MS, "seed": SEED,
                       "repository": os.environ.get("GITHUB_REPOSITORY", "c-schembri/weave"),
                       "run_id": os.environ.get("GITHUB_RUN_ID", ""), "runner": os.environ.get("RUNNER_NAME", "local"),
                       "runner_image": os.environ.get("ImageVersion", "local"),
                       "warmup_roundtrips_per_connection": 8, "tcp_no_delay": True, "listen_backlog": 8192,
                       "compiler": compiler_versions(args.build_directory),
                       "rustc": capture(["rustup", "run", RUST_VERSION, "rustc", "--version"]),
                       "rust_dependencies": capture(["rustup", "run", RUST_VERSION, "cargo", "tree", "--locked",
                                                     "--manifest-path", ROOT / "modules/tcp/benchmarks/tokio/Cargo.toml"]),
                       "configuration": "Release; Weave work stealing/sharded IOCP, Asio shared io_context, Tokio multi-thread",
                       "binaries": [file_hash(args.server_binary), file_hash(args.load_binary), file_hash(args.tokio_binary)],
                       "sources": [file_hash(path) for path in sorted(set(source_files()) |
                                   {ROOT / "CMakePresets.json", ROOT / ".github/workflows/benchmarks-windows.yml"})]})
    write_json(args.output_directory / "environment.json", provenance)
    log = args.output_directory / "run.log"
    start = time.monotonic()
    code = run_owned(sys.executable, [Path(__file__).resolve(), "run", "--worker",
                                     "--build-directory", args.build_directory, "--tokio-binary", args.tokio_binary,
                                     "--output-directory", args.output_directory], ROOT, log, TIMEOUT_MS)
    print(log.read_text(encoding="utf-8", errors="replace"))
    require(code == 0, f"Benchmark {'timed out' if code == -1 else 'failed'}; partial evidence retained in {args.output_directory}")
    publication = read_json(args.output_directory / "publication.json")
    publication["metadata"]["elapsed_seconds"] = time.monotonic() - start
    result = report(publication["samples"], publication["metadata"])
    write_json(args.output_directory / "publication.json", publication)
    write_json(args.output_directory / "environment.json", publication["metadata"])
    markdown = full_markdown(result, publication["metadata"])
    (args.output_directory / "summary.md").write_text(markdown, encoding="utf-8")
    if summary_path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary_path).open("a", encoding="utf-8") as output:
            output.write(markdown)
    if result["noisy"]:
        print(f"::warning::Benchmark completed. {precision_summary(result).replace('**', '')}")
    print(f"Evidence: {args.output_directory}")


def replace_results(readme, markdown):
    require(readme.count(BEGIN) == 1 and readme.count(END) == 1, "README must contain exactly one results block.")
    before, remaining = readme.split(BEGIN)
    _, after = remaining.split(END)
    return before + BEGIN + "\n" + markdown + END + after


def publish(args):
    require(os.environ.get("GITHUB_REPOSITORY") == "c-schembri/weave" and
            os.environ.get("GITHUB_REF") == "refs/heads/main" and
            os.environ.get("GITHUB_EVENT_NAME") in ("push", "workflow_dispatch"), "Publish only from trusted main runs.")
    publication = read_json(args.report)
    metadata = publication["metadata"]
    require(metadata["revision"] == os.environ.get("GITHUB_SHA") and
            metadata["run_id"] == os.environ.get("GITHUB_RUN_ID") and not metadata["worktree"],
            "Publish only complete evidence from this clean main revision.")
    result = report(publication["samples"], metadata)
    require(not capture(["git", "status", "--porcelain"]), "Publication checkout must be clean.")
    if capture(["git", "rev-parse", "HEAD"]) != metadata["revision"]:
        print("Main advanced while benchmarks ran; skipping stale README update.")
        return
    path = ROOT / "README.md"
    current = path.read_text(encoding="utf-8")
    updated = replace_results(current, compact_markdown(result, metadata))
    if updated == current:
        return
    path.write_text(updated, encoding="utf-8")
    run(["git", "config", "user.name", "github-actions[bot]"])
    run(["git", "config", "user.email", "41898282+github-actions[bot]@users.noreply.github.com"])
    run(["git", "add", "--", "README.md"])
    run(["git", "commit", "-m", f"docs: update Windows benchmarks for {metadata['revision'][:7]}"])
    # Normal fast-forward push only. A concurrent source push must win, never be overwritten.
    run(["git", "push", "origin", "HEAD:main"], timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    comparison = commands.add_parser("run")
    comparison.add_argument("--build-directory", type=Path, default=ROOT / "build/windows-bench-ci")
    comparison.add_argument("--tokio-binary", type=Path, default=ROOT / "build/tokio-ci/release/weave-tokio-bench.exe")
    comparison.add_argument("--output-directory", type=Path,
                            default=ROOT / "benchmarks/results" / datetime.now().strftime("ci-%Y%m%d-%H%M%S"))
    comparison.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    publication = commands.add_parser("publish")
    publication.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    return benchmark(args) if args.command == "run" else publish(args)


if __name__ == "__main__":
    sys.exit(cli(main))
