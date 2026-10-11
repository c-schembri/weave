"""Sequential owning/borrowed/libpq measurements against an owned Windows cluster."""

import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import secrets
import socket
import statistics
import subprocess
import tempfile

from server import affinity, invoke
from run import source_fingerprint


def sample_count(pilots, seconds):
    if not pilots or not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("Positive finite duration and matched pilots required")
    count = pilots[0]["operations"]
    if type(count) is not int or not 1 <= count <= 100000:
        raise ValueError("Invalid pilot count")
    for value in pilots:
        if value["operations"] != count or not math.isfinite(value["seconds"]) or value["seconds"] <= 0:
            raise ValueError("Invalid or unmatched pilot")
    estimate = count / min(value["seconds"] for value in pilots) * seconds * 1.1
    return max(count, math.ceil(min(100000, estimate)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--before", type=Path)
    parser.add_argument("--topology-executable", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=1)
    parser.add_argument("--repetitions", type=int, default=7)
    args = parser.parse_args()
    if os.name != "nt" or args.output.exists() or not math.isfinite(args.seconds) or args.seconds <= 0 or args.repetitions < 3:
        parser.error("Requires Windows, a new output path, positive duration and at least three repetitions")

    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith("PG") and key.upper() != "WEAVE_PG_COUNT_ALLOCATIONS"}
    environment["PATH"] = str(args.server_bin) + os.pathsep + environment["PATH"]
    topology = json.loads(subprocess.check_output([str(args.topology_executable), "--topology"],
                                                env=environment, text=True))
    cores = topology["cores"]
    if len(cores) < 3:
        raise RuntimeError("Requires two server cores and a separate client core")
    server_mask = sum(1 << cpu for core in cores[:2] for cpu in core)
    client_mask = 1 << cores[-1][0]
    evidence = {
        "complete": False, "time": datetime.datetime.now(datetime.UTC).isoformat(),
        "platform": platform.platform(), "topology": topology,
        "processor": platform.processor(),
        "server_version": subprocess.check_output([str(args.server_bin / "postgres.exe"), "--version"],
                                                   env=environment, text=True).strip(),
        "server_mask": server_mask, "client_mask": client_mask,
        "source_sha256": source_fingerprint(), "records": [], "summary": [], "setup": [],
        "arguments": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "binary_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (args.executable, args.before) if path},
        "allocation_boundary": "Driving-thread C++ new requests only; excludes C/DLL allocators and RSS",
        "allocation_timing": "Separate instrumented runs, not used in throughput/latency ranking",
        "calibration": "Untimed warmup, larger pilots, fixed matched count targeting the fastest pilot plus 10%",
    }
    started = False
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="weave-buffer-benchmark-") as directory:
        root = Path(directory)
        data = root / "data"
        password_file = root / "password"
        password = secrets.token_urlsafe(24)
        password_file.write_text(password, encoding="utf-8")
        environment["WEAVE_PG_PASSWORD"] = password
        environment["PGPASSWORD"] = password
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]

        def command(name, arguments):
            log = root / (name + ".log")
            record = {"name": name, "command": arguments}
            evidence["setup"].append(record)
            try:
                result = invoke(arguments, environment, log)
                record["exit_code"] = result.returncode
                return result
            finally:
                record["output"] = log.read_text(encoding="utf-8") if log.exists() else "unavailable"

        def measure(label, workload, count, phase, repetition=0):
            executable = args.before if label == "before" else args.executable
            library = "weave" if label in ("before", "owning") else label
            arguments = [str(executable), library, workload, str(count), "32", "127.0.0.1", str(port), "plain"]
            selected = dict(environment)
            if phase == "allocations":
                selected["WEAVE_PG_COUNT_ALLOCATIONS"] = "1"
            record = {"label": label, "workload": workload, "phase": phase, "repetition": repetition,
                      "command": arguments}
            evidence["records"].append(record)
            try:
                process = subprocess.run(arguments, env=selected, text=True, capture_output=True, timeout=60)
                record.update(exit_code=process.returncode, stdout=process.stdout, stderr=process.stderr)
                if process.returncode:
                    raise RuntimeError(f"{label}/{workload} failed: {process.stderr}")
                metrics = json.loads(process.stdout)
                record["metrics"] = metrics
                if metrics["operations"] != count or metrics["seconds"] <= 0:
                    raise RuntimeError("Invalid measurement")
                return metrics
            except subprocess.TimeoutExpired as error:
                record.update(failure="timeout", stdout=str(error.stdout), stderr=str(error.stderr))
                raise

        try:
            affinity(server_mask)
            command("initdb", [str(args.server_bin / "initdb.exe"), "-D", str(data), "-U", "weave",
                               "--auth-host=scram-sha-256", "--auth-local=trust", "--pwfile", str(password_file),
                               "--no-locale", "--encoding=UTF8", "--no-instructions"])
            password_file.unlink()
            started = True
            command("start", [str(args.server_bin / "pg_ctl.exe"), "-D", str(data), "-l", str(root / "server.log"),
                              "-o", f"-h 127.0.0.1 -p {port}", "-w", "start"])
            affinity(client_mask)
            print("| Workload | Before ops/s | Owning ops/s | Borrowed ops/s | Change vs before | libpq ops/s | Spread |")
            print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |", flush=True)
            workloads = ("extended", "prepared", "binary", "copy_out")
            for workload in workloads:
                labels = ["owning", "weave-borrowed", "libpq"]
                if args.before and workload != "copy_out":
                    labels.insert(0, "before")
                warmup_count = 4 if workload == "copy_out" else 256
                warmup = [measure(label, workload, warmup_count, "warmup") for label in labels]
                if len({value["checksum"] for value in warmup}) != 1:
                    raise RuntimeError("Warmup results differ")
                pilot_count = 64 if workload == "copy_out" else 1024
                pilots = [measure(label, workload, pilot_count, "pilot") for label in labels]
                if len({value["checksum"] for value in pilots}) != 1:
                    raise RuntimeError("Pilot results differ")
                count = sample_count(pilots, args.seconds)
                samples = {label: [] for label in labels}
                for repetition in range(args.repetitions):
                    # Rotate order without concurrent client processes or selective retries.
                    offset = repetition % len(labels)
                    order = labels[offset:] + labels[:offset]
                    pair = [measure(label, workload, count, "sample", repetition) for label in order]
                    if len({value["checksum"] for value in pair}) != 1:
                        raise RuntimeError("Consumed results differ")
                    for label, metrics in zip(order, pair):
                        samples[label].append(metrics)
                medians = {label: statistics.median(value["ops_per_second"] for value in values)
                           for label, values in samples.items()}
                spreads = {label: (max(value["ops_per_second"] for value in values) -
                                  min(value["ops_per_second"] for value in values)) / medians[label] * 100
                           for label, values in samples.items()}
                p99 = {label: statistics.median(value["p99_us"] for value in values)
                       for label, values in samples.items()}
                shortest = min(value["seconds"] for values in samples.values() for value in values)
                allocation_samples = {label: measure(label, workload, 256, "allocations")
                                      for label in ("owning", "weave-borrowed")}
                if len({value["checksum"] for value in allocation_samples.values()}) != 1:
                    raise RuntimeError("Allocation-pass results differ")
                allocations = {label: {key: value[key] / 256
                                      for key in ("cpp_allocation_calls", "cpp_requested_bytes")}
                               for label, value in allocation_samples.items()}
                before = medians.get("before")
                change = (medians["weave-borrowed"] / before - 1) * 100 if before else None
                evidence["summary"].append({"workload": workload, "iterations": count, "rates": medians,
                                            "spread_pct": spreads, "p99_us": p99, "change_pct": change,
                                            "shortest_sample_seconds": shortest,
                                            "short_sample": shortest < args.seconds * 0.8,
                                            "allocations_per_operation": allocations})
                before_text = f"{before:,.0f}" if before else "unavailable"
                change_text = f"{change:+.1f}%" if change is not None else "unavailable"
                spread_text = " / ".join(f"{spreads[label]:.1f}%" for label in labels)
                print(f"| {workload} | {before_text} | {medians['owning']:,.0f} | "
                      f"{medians['weave-borrowed']:,.0f} | {change_text} | {medians['libpq']:,.0f} | {spread_text} |",
                      flush=True)
            if source_fingerprint() != evidence["source_sha256"]:
                raise RuntimeError("Sources changed during measurements")
            evidence["complete"] = True
        finally:
            try:
                if started:
                    command("stop", [str(args.server_bin / "pg_ctl.exe"), "-D", str(data), "-m", "immediate", "-w", "stop"])
            except Exception:
                evidence["complete"] = False
                evidence["cleanup_failed"] = True
                raise
            finally:
                log = root / "server.log"
                evidence["server_log"] = log.read_text(encoding="utf-8") if log.exists() else "unavailable"
                with args.output.open("x", encoding="utf-8") as output:
                    json.dump(evidence, output, indent=2)


if __name__ == "__main__":
    main()
