"""Measure local Windows runtime scaling with fixed, physically separate client cores."""

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
import zipfile

from bench_tokio import BACKENDS, WORKLOADS, analyze, measure
from support.common import ROOT, capture, cli, file_hash, read_json, require, sha256, write_json
from support.gate_process import run_owned
from support.metadata import compiler_versions, environment, source_files, timing_build
from support.paired_stats import interval


PROTOCOL = "windows-runtime-scaling-v1"
CORE_COUNTS = (1, 2, 4, 8, 16, 32)
LABELS = ("1,024 clients / 1 KiB", "4,096 clients / 1 KiB", "256 clients / 64 KiB", "1,024 clients / uneven CPU")


def plan(load_binary, counts, client_cores):
    masks, unavailable = {}, {}
    available = None
    client_mask = None
    for count in counts:
        topology = json.loads(capture([load_binary, "--scaling-cpu-masks", count, client_cores]))
        physical = topology["available_physical_cores"]
        require(type(physical) is int and physical > 0, "No usable physical cores.")
        require(available is None or available == physical, "CPU topology changed during planning.")
        available = physical
        server, client = topology["server"], topology["client"]
        require(type(server) is int and type(client) is int and 0 <= server < 2**64 and 0 <= client < 2**64,
                "Invalid affinity masks.")
        if count + client_cores > physical:
            require(server == client == 0, "Unsupported core count unexpectedly received an affinity mask.")
            unavailable[str(count)] = f"Needs {count} server + {client_cores} client physical cores; only {physical} available."
            continue
        require(server.bit_count() == count and client.bit_count() == client_cores and not server & client,
                "Cannot isolate the requested server/client cores.")
        require(client_mask is None or client == client_mask, "Client placement must remain fixed across the sweep.")
        client_mask = client
        masks[str(count)] = {"server": server, "client": client}
    require("1" in masks, "The one-core baseline and separate client cores must be available.")
    return {"cpu_masks": masks, "unavailable": unavailable, "available_physical_cores": available}


def schedule(counts, workloads, repetitions, seed):
    randomizer = random.Random(seed)
    for repetition in range(repetitions):
        cases = [(count, workload) for count in counts for workload in workloads]
        randomizer.shuffle(cases)
        for count, workload in cases:
            backends = list(BACKENDS)
            randomizer.shuffle(backends)
            for backend in backends:
                yield repetition, count, workload, backend


def report(samples, metadata):
    require(metadata["protocol"] == PROTOCOL and not metadata["smoke"], "Not scaling performance evidence.")
    require(re.fullmatch(r"[0-9a-f]{40}", metadata["revision"]) is not None and
            type(metadata["worktree"]) is list, "Missing source provenance.")
    require(metadata["duration_ms"] >= 1000 and metadata["repetitions"] >= 7, "Insufficient measurement windows.")
    requested = metadata["requested_cores"]
    masks = metadata["cpu_masks"]
    require(len(set(requested)) == len(requested) and 1 in requested and
            all(type(count) is int and count in CORE_COUNTS for count in requested), "Invalid requested core counts.")
    require(set(masks).isdisjoint(metadata["unavailable"]) and
            set(masks) | set(metadata["unavailable"]) == {str(count) for count in requested}, "Incomplete core-count accounting.")
    require("1" in masks, "Missing one-core baseline.")
    require(metadata["workloads"] == [list(workload) for workload in WORKLOADS], "Changed workload matrix.")
    client_cores = metadata["client_cores"]
    require(type(client_cores) is int and 1 <= client_cores <= 32 and metadata["client_workers"] == client_cores,
            "Invalid client budget.")
    client_mask = masks["1"]["client"]
    for count, partition in masks.items():
        require(set(partition) == {"server", "client"} and
                all(type(value) is int and 0 < value < 2**64 for value in partition.values()), "Invalid CPU masks.")
        require(partition["server"].bit_count() == int(count) and partition["client"].bit_count() == client_cores and
                not partition["server"] & partition["client"] and partition["client"] == client_mask,
                "Worker budgets or fixed client placement do not match.")
        require(int(count) + client_cores <= metadata["available_physical_cores"], "Oversubscribed physical cores.")
    for count in metadata["unavailable"]:
        require(int(count) + client_cores > metadata["available_physical_cores"], "A supported core count was silently skipped.")
    duration = metadata["duration_ms"] / 1000
    require(all(type(sample.get("server_workers")) is int and str(sample["server_workers"]) in masks and
                duration <= sample["wall_seconds"] <= duration + 1 and sample["client_cores"] <= client_cores + 0.5
                for sample in samples), "A sample missed its worker budget or measurement window.")
    rows = []
    comparisons = []
    repetitions = metadata["repetitions"]
    for count in sorted(map(int, masks)):
        group = [sample for sample in samples if sample["server_workers"] == count]
        result = analyze(group, WORKLOADS, BACKENDS, repetitions)
        for row in result["rows"]:
            if "throughput_ratio_ci90" in row:
                row["throughput_ratio_ci90"] = list(row["throughput_ratio_ci90"])
            workload, backend = row["workload"], row["backend"]
            current = {sample["repetition"]: sample for sample in group
                       if sample["workload"] == workload and sample["backend"] == backend}
            base = {sample["repetition"]: sample for sample in samples if sample["server_workers"] == 1 and
                    sample["workload"] == workload and sample["backend"] == backend}
            require(set(base) == set(range(repetitions)), "Incomplete one-core baseline.")
            ratios = [current[index]["roundtrips_per_second"] / base[index]["roundtrips_per_second"]
                      for index in range(repetitions)]
            low, high = interval(ratios)
            speedup = median(ratios)
            row.update(server_workers=count, speedup=speedup, speedup_ci90=[low, high], efficiency=speedup / count,
                       throughput_noisy=row["metrics"]["roundtrips_per_second"]["cv_pct"] > 10 or (high - low) / speedup > 0.20,
                       p99_noisy=row["metrics"]["p99_us"]["cv_pct"] > 25,
                       client_busy=row["metrics"]["client_cores"]["median"] >= client_cores * 0.90)
            rows.append(row)
        for workload in WORKLOADS:
            for baseline in ("asio", "tokio"):
                groups = {backend: {sample["repetition"]: sample for sample in group
                                   if sample["workload"] == workload[0] and sample["backend"] == backend}
                          for backend in ("weave", baseline)}
                ratios = [groups["weave"][index]["roundtrips_per_second"] /
                          groups[baseline][index]["roundtrips_per_second"] for index in range(repetitions)]
                comparisons.append({"server_workers": count, "workload": workload[0], "baseline": baseline,
                                    "ratio": median(ratios), "ci90": list(interval(ratios))})
    return {"protocol": PROTOCOL, "rows": rows, "comparisons": comparisons}


def quality(rows):
    notes = []
    if any(row["throughput_noisy"] for row in rows):
        notes.append("throughput noisy")
    if any(row["p99_noisy"] for row in rows):
        notes.append("p99 noisy")
    if any(row["client_busy"] for row in rows):
        notes.append("client busy")
    return "; ".join(notes) or "within limits"


def compact_markdown(result, metadata, evidence_url, workload=WORKLOADS[0][0]):
    rows = {(row["server_workers"], row["backend"]): row for row in result["rows"] if row["workload"] == workload}
    label = dict(zip((case[0] for case in WORKLOADS), LABELS, strict=True))[workload]
    cpu = ", ".join(processor["Name"] for processor in metadata["cpu"])
    source = metadata["revision"]
    dirty = " (uncommitted working tree)" if metadata["worktree"] else ""
    timestamp = datetime.fromisoformat(metadata["timestamp"]).strftime("%Y-%m-%d %H:%M %z")
    lines = [f"Local Windows run: {timestamp}, {cpu}. "
             f"Source: [`{source[:7]}`](https://github.com/c-schembri/weave/commit/{source}){dirty}.", "",
             f"Workload: **{label}** per round trip.", "",
             f"{metadata['repetitions']} x {metadata['duration_ms'] / 1000:g}s per library/core count/workload. "
             f"One worker per server core; {metadata['client_cores']} fixed, physically separate client cores. "
             "Median validated round trips/second; higher is better.", "",
             "| Server cores | Weave | Asio | Tokio | Weave vs 1 core | Max throughput CV | Notes |",
             "| ---: | ---: | ---: | ---: | ---: | ---: | --- |"]
    for count in metadata["requested_cores"]:
        if str(count) not in metadata["cpu_masks"]:
            lines.append(f"| {count} | N/A | N/A | N/A | N/A | N/A | insufficient physical cores |")
            continue
        group = [rows[count, backend] for backend in BACKENDS]
        weave, tokio, asio = group
        throughput = lambda row: row["metrics"]["roundtrips_per_second"]["median"]
        cv = max(row["metrics"]["roundtrips_per_second"]["cv_pct"] for row in group)
        lines.append(f"| {count} | {throughput(weave):,.0f} | {throughput(asio):,.0f} | {throughput(tokio):,.0f} | "
                     f"{weave['speedup']:.2f}x | {cv:.1f}% | {quality(group)} |")
    lines.extend(["", "Closed-loop loopback results, not universal runtime rankings. Client-busy rows do not establish "
                  "maximum server capacity; noisy comparisons are inconclusive for the affected metric.", "",
                  f"[Full measurements and raw evidence]({evidence_url}) / "
                  "[methodology](https://github.com/c-schembri/weave/blob/main/docs/runtime-scaling.md)."])
    return "\n".join(lines) + "\n"


def full_markdown(result, metadata, evidence_url):
    lines = ["# Local Windows Runtime Scaling", ""]
    for workload, label in zip(WORKLOADS, LABELS, strict=True):
        lines.extend([f"## {label}", "", compact_markdown(result, metadata, evidence_url, workload[0])])
    lines.extend(["## Per-library Measurements", "",
                  "Medians of per-window percentiles, not pooled latency. CPU time can be quantized or zero; "
                  "cycles/op is independent and never converted into seconds.", "",
                  "| Workload | Cores | Library | p99 ms | p99.9 ms | Server kcycles/RTT | Client cores | Private MiB | Speedup CI90 | Notes |",
                  "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |"])
    for row in result["rows"]:
        value = lambda key: row["metrics"][key]["median"]
        low, high = row["speedup_ci90"]
        lines.append(f"| {row['workload']} | {row['server_workers']} | {row['backend']} | "
                     f"{value('p99_us') / 1000:.3f} | {value('p999_us') / 1000:.3f} | "
                     f"{value('server_cycles_per_op') / 1000:.2f} | {value('client_cores'):.2f} | "
                     f"{value('server_private_mb'):.2f} | [{low:.3f}, {high:.3f}] | {quality([row])} |")
    lines.extend(["", "## Matched Throughput Comparisons", "", "Weave / baseline, matched by repetition; "
                  "90% whole-block bootstrap intervals are exploratory, not simultaneous guarantees.", "",
                  "| Workload | Cores | Baseline | Median ratio | Paired CI90 |",
                  "| --- | ---: | --- | ---: | --- |"])
    for pair in result["comparisons"]:
        low, high = pair["ci90"]
        lines.append(f"| {pair['workload']} | {pair['server_workers']} | {pair['baseline']} | {pair['ratio']:.3f} | "
                     f"[{low:.3f}, {high:.3f}] |")
    lines.extend(["", "All samples and outliers retained; no selective retries or adaptive stopping. "
                  f"Elapsed measurement supervisor time: {metadata['elapsed_seconds']:.1f}s."])
    return "\n".join(lines) + "\n"


def worker(args):
    start = time.monotonic()
    root = args.output_directory
    metadata = read_json(root / "environment.json")
    metadata.update(plan(args.load_binary, args.server_cores, args.client_cores))
    write_json(root / "environment.json", metadata)
    counts = sorted(map(int, metadata["cpu_masks"]))
    workloads = WORKLOADS if not args.smoke else tuple((name, 32, size, work, uneven) for name, _, size, work, uneven in WORKLOADS)
    for count in counts:
        (root / f"cores-{count:02d}").mkdir()
    samples = []
    total = len(counts) * len(workloads) * len(BACKENDS) * args.repetitions
    for repetition, count, workload, backend in schedule(counts, workloads, args.repetitions, args.seed):
        args.server_workers = count
        args.output_directory = root / f"cores-{count:02d}"
        sample = measure(args, backend, workload, repetition, metadata["cpu_masks"][str(count)])
        samples.append(sample)
        write_json(root / "samples.json", {"samples": samples})
        print(f"{len(samples):03d}/{total} {count} cores {workload[0]} {backend}: "
              f"{sample['roundtrips_per_second']:,.0f} RTT/s", flush=True)
    metadata["elapsed_seconds"] = time.monotonic() - start
    write_json(root / "environment.json", metadata)
    if not args.smoke:
        result = report(samples, metadata)
        write_json(root / "analysis.json", result)
        (root / "summary.md").write_text(full_markdown(result, metadata, "."), encoding="utf-8")


def benchmark(args):
    require(os.name == "nt", "Windows is required.")
    require(1 in args.server_cores and len(set(args.server_cores)) == len(args.server_cores), "Include one baseline per core count.")
    require(1 <= args.client_cores <= 32 and 50 <= args.duration_ms <= 10000 and 0 <= args.warmup_ms <= 5000 and
            args.repetitions >= 1 and args.timeout_seconds > 0, "Invalid measurement limits.")
    require(args.smoke or (args.duration_ms >= 1000 and args.repetitions >= 7), "Performance needs >=1s and seven repetitions.")
    args.client_workers = args.client_cores
    args.build_directory = args.build_directory.resolve(strict=True)
    args.output_directory = args.output_directory.resolve()
    args.server_binary = (args.build_directory / "Release/weave_runtime_server.exe").resolve(strict=True)
    args.load_binary = (args.build_directory / "Release/weave_runtime_load.exe").resolve(strict=True)
    args.tokio_binary = args.tokio_binary.resolve(strict=True)
    if args.worker:
        return worker(args)
    timing_build(args.build_directory)
    require(not args.output_directory.exists(), "Refusing to overwrite evidence.")
    metadata = environment()
    require(args.smoke or not metadata["worktree"], "Commit source changes before collecting publishable evidence.")
    metadata.update(protocol=PROTOCOL, requested_cores=args.server_cores, client_cores=args.client_cores,
                    client_workers=args.client_workers, duration_ms=args.duration_ms, repetitions=args.repetitions,
                    warmup_ms=args.warmup_ms, seed=args.seed, smoke=args.smoke, workloads=[list(workload) for workload in WORKLOADS],
                    compiler=compiler_versions(args.build_directory), rustc=capture(["rustc", "--version"]),
                    rust_dependencies=capture(["cargo", "tree", "--locked", "--manifest-path", ROOT / "modules/tcp/benchmarks/tokio/Cargo.toml"]),
                    binaries=[file_hash(args.server_binary), file_hash(args.load_binary), file_hash(args.tokio_binary)],
                    sources=[file_hash(path) for path in source_files()])
    args.output_directory.mkdir(parents=True)
    write_json(args.output_directory / "environment.json", metadata)
    log = args.output_directory / "run.log"
    code = run_owned(sys.executable, [Path(__file__).resolve(), *sys.argv[1:], "--worker",
                                     "--output-directory", args.output_directory], ROOT, log, args.timeout_seconds * 1000)
    print(log.read_text(encoding="utf-8", errors="replace"))
    require(code == 0, f"Benchmark {'timed out' if code == -1 else 'failed'}; partial evidence retained in {args.output_directory}")
    require(args.smoke or metadata["sources"] == [file_hash(path) for path in source_files()], "Sources changed during measurement.")
    print(f"Evidence: {args.output_directory}")


def package(args):
    root = args.directory.resolve(strict=True)
    metadata = read_json(root / "environment.json")
    require(not metadata["worktree"], "Publish a clean-source run, not an uncommitted working tree.")
    result = report(read_json(root / "samples.json")["samples"], metadata)
    require(result == read_json(root / "analysis.json"), "Stored analysis does not match raw evidence.")
    require(not args.output.exists(), "Refusing to overwrite an evidence archive.")
    files = [root / name for name in ("environment.json", "samples.json", "analysis.json", "run.log")]
    for pattern in ("*.client.log", "*.server.log", "*.failure.json"):
        files.extend(root.glob(f"cores-*/*{pattern[1:]}"))
    with zipfile.ZipFile(args.output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(files):
            archive.write(path, path.relative_to(root))
        archive.writestr("summary.md", full_markdown(result, metadata, args.evidence_url))
    print(compact_markdown(result, metadata, args.evidence_url))
    print(f"Archive SHA256: {sha256(args.output)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    comparison = commands.add_parser("run")
    comparison.add_argument("--build-directory", type=Path, default=ROOT / "build/windows-runtime-bench")
    comparison.add_argument("--tokio-binary", type=Path, default=ROOT / "build/tokio/release/weave-tokio-bench.exe")
    comparison.add_argument("--output-directory", type=Path, default=ROOT / "benchmarks/results" / datetime.now().strftime("scaling-%Y%m%d-%H%M%S"))
    comparison.add_argument("--server-cores", nargs="+", type=int, choices=CORE_COUNTS, default=list(CORE_COUNTS))
    comparison.add_argument("--client-cores", type=int, default=4)
    comparison.add_argument("--duration-ms", type=int, default=2000)
    comparison.add_argument("--repetitions", type=int, default=7)
    comparison.add_argument("--warmup-ms", type=int, default=250)
    comparison.add_argument("--seed", type=int, default=60106)
    comparison.add_argument("--timeout-seconds", type=int, default=2100)
    comparison.add_argument("--smoke", action="store_true")
    comparison.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    archival = commands.add_parser("package")
    archival.add_argument("--directory", type=Path, required=True)
    archival.add_argument("--output", type=Path, required=True)
    archival.add_argument("--evidence-url", required=True)
    args = parser.parse_args()
    return benchmark(args) if args.command == "run" else package(args)


if __name__ == "__main__":
    sys.exit(cli(main))
