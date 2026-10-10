"""Sequential, matched PostgreSQL measurements. Never run by correctness CI."""

import argparse
import datetime
import json
import hashlib
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time


WORKLOADS = ("connect", "simple", "extended", "prepared", "binary", "batch", "rows", "copy", "lo_read", "lo_write", "lo_append")


def source_fingerprint():
    root = Path(__file__).resolve().parents[3]
    digest = hashlib.sha256()
    files = [root / "CMakeLists.txt", root / "cmake" / "WeaveModule.cmake"]
    files.append(root / "test_support" / "tls_certificates.hpp")
    files.append(root / "test_support" / "benchmark_affinity.hpp")
    for module in ("core", "io", "runtime", "tcp", "sync", "tls", "postgres"):
        files.extend(path for path in (root / "modules" / module).rglob("*")
                     if path.is_file() and path.suffix in (".cpp", ".hpp", ".txt", ".py"))
    for path in sorted(files, key=lambda value: value.relative_to(root).as_posix()):
        digest.update(str(path.relative_to(root)).replace("\\", "/").encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=55432)
    parser.add_argument("--ca", default="plain")
    parser.add_argument("--seconds", type=float, default=1.0)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--workloads", nargs="+", choices=WORKLOADS,
                        default=[workload for workload in WORKLOADS if workload != "lo_write"])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0 or args.repetitions < 3 or not os.environ.get("WEAVE_PG_PASSWORD"):
        parser.error("Set WEAVE_PG_PASSWORD; use positive sample duration and at least three repetitions")
    if len(set(args.workloads)) != len(args.workloads):
        parser.error("Select each workload at most once")

    if args.output.exists():
        parser.error("Output already exists; raw evidence must not be overwritten")

    evidence = {
        "time": datetime.datetime.now(datetime.UTC).isoformat(),
        "platform": platform.platform(),
        "cpu": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "arguments": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "records": [],
        "complete": False,
        "cpu_time_reliable": platform.system() != "Windows",
        "cpu_boundary": "client process only; Windows GetProcessTimes readings retained but not ranked",
        "source_sha256": source_fingerprint(),
        "fingerprint_order": "relative_posix_path",
        "executable_sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
    }

    def run(library, workload, count, phase, repetition):
        command = [str(args.executable), library, workload, str(count), str(args.batch), args.host, str(args.port), args.ca]
        start = time.monotonic()
        try:
            process = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
        except subprocess.TimeoutExpired as error:
            evidence["records"].append({
                "library": library, "workload": workload, "iterations": count, "phase": phase,
                "repetition": repetition, "command": command, "exit_code": None, "failure": "timeout",
                "stdout": str(error.stdout), "stderr": str(error.stderr), "wall_seconds": time.monotonic() - start,
            })
            raise
        record = {
            "library": library, "workload": workload, "iterations": count, "phase": phase,
            "repetition": repetition, "command": command, "exit_code": process.returncode,
            "stdout": process.stdout, "stderr": process.stderr, "wall_seconds": time.monotonic() - start,
        }
        evidence["records"].append(record)
        if process.returncode:
            raise RuntimeError(f"{library}/{workload} failed: {process.stderr}")

        record["metrics"] = json.loads(process.stdout)
        metrics = record["metrics"]
        if metrics["seconds"] <= 0 or metrics["operations"] <= 0:
            raise RuntimeError("Invalid measurement")
        return metrics

    try:
        print("| Workload | Weave ops/s | libpq ops/s | Weave / libpq | Sample spread W / PQ | P99 us W / PQ | CPU % W / PQ |", flush=True)
        print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |", flush=True)
        for workload in args.workloads:
            pilot_count = 4 if workload == "connect" else 32
            pilots = [run(library, workload, pilot_count, "pilot", 0) for library in ("weave", "libpq")]
            if pilots[0]["checksum"] != pilots[1]["checksum"]:
                raise RuntimeError(f"Mismatched consumed results: {workload}")

            slowest = max(value["seconds"] for value in pilots)
            maximum = 8192 if workload == "lo_append" else 500000
            count = max(pilot_count, min(maximum, int(pilot_count * args.seconds / slowest)))
            samples = {"weave": [], "libpq": []}
            for repetition in range(args.repetitions):
                order = ("weave", "libpq") if repetition % 2 == 0 else ("libpq", "weave")
                pair = {}
                for library in order:
                    pair[library] = run(library, workload, count, "sample", repetition)
                    samples[library].append(pair[library])
                if pair["weave"]["checksum"] != pair["libpq"]["checksum"]:
                    raise RuntimeError(f"Mismatched consumed results: {workload}, repetition {repetition}")

            rates = {}
            spreads = {}
            for library in samples:
                values = [value["ops_per_second"] for value in samples[library]]
                rates[library] = statistics.median(values)
                spreads[library] = (max(values) - min(values)) / rates[library] * 100

            ratio = rates["weave"] / rates["libpq"]
            p99 = {library: statistics.median(value["p99_us"] for value in values) for library, values in samples.items()}
            cpu = {library: statistics.median(value["cpu_seconds"] / value["seconds"] * 100 for value in values)
                   for library, values in samples.items()}
            cpu_text = f"{cpu['weave']:.1f} / {cpu['libpq']:.1f}" if evidence["cpu_time_reliable"] else "unavailable"
            print(f"| {workload} | {rates['weave']:,.0f} | {rates['libpq']:,.0f} | {ratio:.2f}x | "
                  f"{spreads['weave']:.1f}% / {spreads['libpq']:.1f}% | {p99['weave']:.1f} / {p99['libpq']:.1f} | "
                  f"{cpu_text} |", flush=True)

        if source_fingerprint() != evidence["source_sha256"]:
            raise RuntimeError("Source changed during measurement; results are diagnostic only")
        evidence["complete"] = True
    finally:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(evidence, output, indent=2)


if __name__ == "__main__":
    main()
