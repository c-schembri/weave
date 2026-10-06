"""Manually stress four-worker Weave/Tokio/Asio servers with an identical external load generator."""

import argparse
from datetime import datetime
import json
import math
import os
from pathlib import Path
import queue
import random
from statistics import median
import subprocess
import sys
import threading

from support.common import ROOT, capture, cli, file_hash, finite, read_json, require, write_json
from support.gate_process import run_owned
from support.metadata import compiler_versions, environment, source_files, timing_build
from support.paired_stats import interval, summary
from support.process_counters import counters


WORKLOADS = (
    ("1024-small", 1024, 1024, 0, False),
    ("4096-small", 4096, 1024, 0, False),
    ("256-large", 256, 65536, 0, False),
    ("1024-uneven-cpu", 1024, 1024, 20000, True),
)
BACKENDS = ("weave", "tokio", "asio")
METRICS = ("roundtrips_per_second", "p50_us", "p99_us", "p999_us", "server_cores",
           "server_cpu_us_per_op", "server_cycles_per_op", "client_cores", "server_private_mb")


class Child:
    def __init__(self, arguments, log, *, control=False):
        self.log = Path(log).open("xb")
        self.process = subprocess.Popen([str(value) for value in arguments], cwd=ROOT, text=True,
                                        stdin=subprocess.PIPE if control else subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=self.log,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def line(self, timeout=25):
        try:
            value = self.lines.get(timeout=timeout)
        except queue.Empty as error:
            raise ValueError(f"Process output timed out; see {self.log.name}") from error
        require(value is not None, f"Process ended prematurely; see {self.log.name}")
        return value

    def send(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        self.process.stdout.close()
        if self.process.stdin:
            self.process.stdin.close()
        self.log.close()


def validate_sample(sample, *, smoke=False):
    require(type(sample.get("connections")) is int and 1 <= sample["connections"] <= 8192, "Invalid connection count.")
    require(type(sample.get("bytes")) is int and 16 <= sample["bytes"] <= 65536, "Invalid frame size.")
    require(type(sample.get("cpu_iterations")) is int and 0 <= sample["cpu_iterations"] <= 2_000_000,
            "Invalid CPU workload.")
    require(type(sample.get("uneven")) is bool and type(sample.get("repetition")) is int and sample["repetition"] >= 0,
            "Invalid workload or repetition metadata.")
    require(type(sample.get("samples")) is int and sample["samples"] >= (1 if smoke else 1000), "Too few samples.")
    require(type(sample.get("min_connection_samples")) is int and sample["min_connection_samples"] >= 1,
            "A connection stalled during measurement.")
    require(type(sample.get("max_connection_samples")) is int and
            sample["max_connection_samples"] >= sample["min_connection_samples"], "Invalid progress range.")
    require(sample["connections"] * sample["min_connection_samples"] <= sample["samples"] <=
            sample["connections"] * sample["max_connection_samples"], "Inconsistent per-connection progress.")
    for metric in (*METRICS, "wall_seconds", "client_cycles_per_op", "p95_us", "max_us"):
        finite(sample[metric], 0, metric)
    require(sample["wall_seconds"] > 0 and sample["roundtrips_per_second"] > 0, "Empty measurement.")
    require(math.isclose(sample["roundtrips_per_second"], sample["samples"] / sample["wall_seconds"], rel_tol=1e-6),
            "Throughput does not match sample count.")
    require(sample["p50_us"] <= sample["p95_us"] <= sample["p99_us"] <= sample["p999_us"] <= sample["max_us"],
            "Invalid latency percentiles.")
    require(sample["server_cores"] <= 4.5, "Server exceeded its four-core CPU budget.")
    require(sample["server_cycles_per_op"] > 0 and sample["client_cycles_per_op"] > 0, "Missing process cycle measurements.")


def measure(args, backend, workload, repetition, masks):
    name, connections, size, work, uneven = workload
    prefix = args.output_directory / f"{name}-{repetition:02d}-{backend}"
    common = [size, work, int(uneven), masks["server"]]
    server_command = [args.tokio_binary, *common] if backend == "tokio" else [args.server_binary, backend, *common]
    server = client = None
    phase = "server startup"
    try:
        server = Child(server_command, prefix.with_suffix(".server.log"))
        ready = json.loads(server.line())
        require(ready.get("workers") == 4 and 0 < ready.get("port", 0) <= 65535, "Invalid server readiness.")
        phase = "client setup/warmup"
        client = Child([args.load_binary, ready["port"], connections, size, work, int(uneven),
                        args.duration_ms, masks["client"], args.client_workers],
                       prefix.with_suffix(".client.log"), control=True)
        require(client.line() == "READY", "Invalid client warmup/readiness.")
        phase = "measurement"
        before = counters(server.process)
        client.send("GO")
        require(client.line() == "MEASURED", "Client did not finish the measurement.")
        phase = "measurement result"
        after = counters(server.process)
        sample = json.loads(client.line())
        cpu = after["cpu_seconds"] - before["cpu_seconds"]
        sample.update({"backend": backend, "workload": name, "repetition": repetition,
                       "connections": connections, "bytes": size, "cpu_iterations": work, "uneven": uneven,
                       "server_cores": cpu / sample["wall_seconds"],
                       "server_cpu_us_per_op": cpu * 1e6 / sample["samples"],
                       "server_cycles_per_op": (after["cycles"] - before["cycles"]) / sample["samples"],
                       "server_private_mb": after["private_bytes"] / (1024 * 1024),
                       "server_working_set_mb": after["working_set_bytes"] / (1024 * 1024)})
        validate_sample(sample, smoke=args.smoke)
        require(server.process.poll() is None, "Server exited during measurement.")
        phase = "cleanup"
        server.close()
        server = None
        client.send("STOP")
        require(client.process.wait(timeout=5) == 0, "Client cleanup failed.")
        return sample
    except Exception as error:
        processes = {}
        for process_name, child in (("server", server), ("client", client)):
            if child:
                log = Path(child.log.name)
                processes[process_name] = {"pid": child.process.pid, "exit_code": child.process.poll(),
                                           "stderr_log": str(log),
                                           "stderr_tail": log.read_text(encoding="utf-8", errors="replace")[-8192:]}
        failure = prefix.with_suffix(".failure.json")
        write_json(failure, {"backend": backend, "workload": name,
                             "repetition": repetition, "phase": phase, "error": str(error),
                             "processes": processes})
        raise ValueError(f"{backend} {workload[0]} repetition {repetition}: {phase} failed; see {failure}: {error}") from error
    finally:
        if server:
            server.close()
        if client:
            client.close()


def analyze(samples, workloads, backends, repetitions):
    grouped = {}
    for sample in samples:
        validate_sample(sample)
        key = (sample["workload"], sample["backend"])
        group = grouped.setdefault(key, {})
        require(sample["repetition"] not in group, "Duplicate repetition.")
        group[sample["repetition"]] = sample
    require(set(grouped) == {(workload[0], backend) for workload in workloads for backend in backends}, "Unmatched matrix.")
    rows = []
    for workload in workloads:
        name = workload[0]
        for backend in backends:
            group = grouped[name, backend]
            require(set(group) == set(range(repetitions)), "Incomplete repetitions.")
            require(all((sample["connections"], sample["bytes"], sample["cpu_iterations"], sample["uneven"]) == workload[1:]
                        for sample in group.values()), "Mismatched workload parameters.")
            row = {"workload": name, "backend": backend, "metrics": {}}
            for metric in METRICS:
                values = [group[index][metric] for index in range(repetitions)]
                center, cv = summary(values)
                row["metrics"][metric] = {"median": center, "cv_pct": cv * 100, "min": min(values), "max": max(values)}
            if "tokio" in backends and backend != "tokio":
                base = grouped[name, "tokio"]
                ratios = [group[index]["roundtrips_per_second"] / base[index]["roundtrips_per_second"]
                          for index in range(repetitions)]
                row["throughput_vs_tokio_pct"] = (median(ratios) - 1) * 100
                row["throughput_ratio_ci90"] = interval(ratios)
            rows.append(row)
    return {"repetitions": repetitions, "rows": rows}


def print_report(report):
    print("\nWorkload | Backend | RTT/s | p99 ms | p99.9 ms | server kcycles/op | CPU-time cores (diagnostic) | throughput CV")
    for row in report["rows"]:
        metrics = row["metrics"]
        value = lambda key: metrics[key]["median"]
        print(f"{row['workload']} | {row['backend']} | {value('roundtrips_per_second'):,.0f} | "
              f"{value('p99_us') / 1000:.3f} | {value('p999_us') / 1000:.3f} | "
              f"{value('server_cycles_per_op') / 1000:.2f} | {value('server_cores'):.2f} | "
              f"{metrics['roundtrips_per_second']['cv_pct']:.1f}%")
        if "throughput_vs_tokio_pct" in row:
            low, high = row["throughput_ratio_ci90"]
            print(f"  vs Tokio: {row['throughput_vs_tokio_pct']:+.1f}%; paired ratio CI90 [{low:.3f}, {high:.3f}]")


def worker(args):
    workloads = WORKLOADS if not args.workloads else tuple(w for w in WORKLOADS if w[0] in args.workloads)
    if args.smoke:
        workloads = tuple((name, min(count, 32), size, work, uneven) for name, count, size, work, uneven in workloads)
    masks = json.loads(capture([args.load_binary, "--cpu-masks"]))
    require(masks["server"].bit_count() == 4 and masks["client"].bit_count() == 8 and not masks["server"] & masks["client"],
            "Expected four server cores and eight physically separate client cores.")
    randomizer = random.Random(args.seed)
    samples = []
    for repetition in range(args.repetitions):
        cases = list(workloads)
        randomizer.shuffle(cases)
        for workload in cases:
            backends = list(args.backends)
            randomizer.shuffle(backends)
            for backend in backends:
                sample = measure(args, backend, workload, repetition, masks)
                samples.append(sample)
                write_json(args.output_directory / "samples.json", {"samples": samples, "cpu_masks": masks})
                print(f"{len(samples):02d} {workload[0]} {backend}: {sample['roundtrips_per_second']:,.0f} RTT/s; "
                      f"p99 {sample['p99_us'] / 1000:.2f} ms; server {sample['server_cores']:.2f} cores; "
                      f"client {sample['client_cores']:.2f} cores", flush=True)
    if not args.smoke:
        report = analyze(samples, workloads, args.backends, args.repetitions)
        write_json(args.output_directory / "analysis.json", report)
        print_report(report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, default=ROOT / "build/windows")
    parser.add_argument("--tokio-binary", type=Path, default=ROOT / "build/tokio/release/weave-tokio-bench.exe")
    parser.add_argument("--output-directory", type=Path, default=ROOT / "benchmarks/results" / datetime.now().strftime("tokio-%Y%m%d-%H%M%S"))
    parser.add_argument("--duration-ms", type=int, default=2000)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--client-workers", type=int, default=8)
    parser.add_argument("--backends", nargs="+", choices=(*BACKENDS, "weave-shared"), default=list(BACKENDS))
    parser.add_argument("--workloads", nargs="+", choices=[w[0] for w in WORKLOADS])
    parser.add_argument("--seed", type=int, default=60106)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    require(os.name == "nt", "This comparison requires Windows.")
    require(50 <= args.duration_ms <= 10000 and args.repetitions >= 1 and args.timeout_seconds > 0 and
            1 <= args.client_workers <= 32 and len(set(args.backends)) == len(args.backends), "Invalid execution limits.")
    if not args.smoke:
        require(args.duration_ms >= 1000 and args.repetitions >= 7, "Evidence requires at least 1s and seven repetitions.")
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
    provenance.update({"protocol": "four-worker TCP server; separate native Asio client; randomized matched repetition blocks",
                       "workers": 4, "client_workers": args.client_workers, "duration_ms": args.duration_ms,
                       "repetitions": args.repetitions, "seed": args.seed, "smoke": args.smoke,
                       "latency": "Closed-loop RTTs, every completed sample; one outstanding request per connection; no coordinated-omission correction",
                       "cpu_accounting": "Server GetProcessTimes and QueryProcessCycleTime between GO and MEASURED; client counters sampled natively",
                       "cpu_time_caution": "GetProcessTimes has shown zero/quantized deltas despite substantial cycle counts on this host; CPU time is diagnostic, cycles/op is independent and never converted to seconds",
                       "configuration": "Release; Weave work-stealing, default sharded IOCP; optional shared layout; Tokio multi-thread; Asio shared io_context; all servers four workers",
                       "warmup_roundtrips_per_connection": 8, "tcp_no_delay": True, "listen_backlog": 8192,
                       "workloads": list(WORKLOADS), "backends": args.backends,
                       "compiler": compiler_versions(args.build_directory), "rustc": capture(["rustc", "--version"]),
                       "rust_dependencies": capture(["cargo", "tree", "--locked", "--manifest-path", ROOT / "modules/tcp/benchmarks/tokio/Cargo.toml"]),
                       "binaries": [file_hash(args.server_binary), file_hash(args.load_binary), file_hash(args.tokio_binary)],
                       "sources": [file_hash(path) for path in source_files()]})
    write_json(args.output_directory / "environment.json", provenance)
    log = args.output_directory / "run.log"
    code = run_owned(sys.executable, [Path(__file__).resolve(), *sys.argv[1:], "--worker",
                                     "--output-directory", args.output_directory], ROOT, log, args.timeout_seconds * 1000)
    print(log.read_text(encoding="utf-8", errors="replace"))
    require(code == 0, f"Benchmark {'timed out' if code == -1 else 'failed'}; partial evidence retained in {args.output_directory}")
    print(f"Evidence: {args.output_directory}")


if __name__ == "__main__":
    sys.exit(cli(main))
