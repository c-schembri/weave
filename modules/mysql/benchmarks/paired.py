"""Interleave before/native/after binaries against live.py's single private server."""

import csv
import io
import math
import os
from pathlib import Path
import subprocess
import sys
import time


WORKLOADS = (
    "text", "rows64", "prepared", "blob64k", "stream64", "multi", "batch4", "reset", "connect",
    "runtime4", "runtime4_affine",
)
METRICS = ("seconds", "ops_per_second", "p99_us", "cpu_percent", "cycles_per_operation", "max_us")


def parse_sample(output, workload, driver):
    rows = list(csv.DictReader(io.StringIO(output)))
    if len(rows) != 1 or rows[0]["workload"] != workload or rows[0]["driver"] != driver:
        raise ValueError("expected exactly one checked sample for the requested workload and driver")
    row = rows[0]
    if int(row["operations"]) <= 0 or int(row["repetition"]) != 1:
        raise ValueError("invalid sample operation/repetition count")
    expected_workers = 4 if workload.startswith("runtime4") else 1
    if int(row["workers"]) != expected_workers:
        raise ValueError("sample worker count does not match the workload")
    values = {key: float(row[key]) for key in METRICS}
    if any(not math.isfinite(value) or value < 0 for value in values.values()):
        raise ValueError("invalid sample metrics")
    if values["seconds"] <= 0 or values["ops_per_second"] <= 0 or values["p99_us"] > values["max_us"]:
        raise ValueError("invalid sample duration, throughput or latency")
    return row


def trial_order(repetition):
    orders = (
        ("before", "native", "after"),
        ("after", "native", "before"),
        ("native", "before", "after"),
        ("after", "before", "native"),
        ("before", "after", "native"),
        ("native", "after", "before"),
    )
    return orders[(repetition - 1) % len(orders)]


def main():
    if len(sys.argv) != 4:
        raise ValueError("run through modules/mysql/tests/live.py")
    binaries = {label: Path(os.environ[f"WEAVE_MYSQL_BENCHMARK_{label.upper()}"]).resolve()
                for label in ("before", "after")}
    if any(not path.is_file() for path in binaries.values()):
        raise ValueError("before and after comparison executables must exist")
    repetitions = int(os.environ.get("WEAVE_MYSQL_PAIRED_REPETITIONS", "7"))
    if not 1 <= repetitions <= 31:
        raise ValueError("paired repetitions must be between 1 and 31")
    settle = float(os.environ.get("WEAVE_MYSQL_PAIRED_SETTLE_SECONDS", "0"))
    if not math.isfinite(settle) or not 0 <= settle <= 300:
        raise ValueError("settling period must be between 0 and 300 seconds")
    time.sleep(settle)

    workloads = WORKLOADS + (("runtime4_shared",) if os.name == "nt" else ())
    selected = os.environ.get("WEAVE_MYSQL_PAIRED_WORKLOAD")
    if selected:
        if selected not in workloads:
            raise ValueError("unknown paired workload")
        workloads = (selected,)
    writer = None
    version = None
    for workload in workloads:
        for repetition in range(1, repetitions + 1):
            for label in trial_order(repetition):
                environment = dict(os.environ)
                environment.pop("WEAVE_MYSQL_DIAGNOSTICS", None)
                environment["WEAVE_MYSQL_BENCHMARK_WORKLOAD"] = workload
                environment["WEAVE_MYSQL_BENCHMARK_DRIVER"] = "native" if label == "native" else "weave"
                environment["WEAVE_MYSQL_BENCHMARK_REPETITIONS"] = "1"
                executable = binaries["after" if label == "native" else label]
                result = subprocess.run([str(executable), *sys.argv[1:]], env=environment,
                                        capture_output=True, text=True, timeout=30)
                print(f"{workload}/{repetition}/{label}: {result.stderr.strip()}", file=sys.stderr, flush=True)
                result.check_returncode()
                identity = result.stderr.split(";", 1)[0].strip()
                if not identity.startswith("Native MySQL client: ") or (version and identity != version):
                    raise ValueError("comparison binaries must use the same native client version")
                version = identity
                row = parse_sample(result.stdout, workload, environment["WEAVE_MYSQL_BENCHMARK_DRIVER"])
                row["driver"] = label
                row["repetition"] = str(repetition)
                if writer is None:
                    writer = csv.DictWriter(sys.stdout, fieldnames=list(row))
                    writer.writeheader()
                writer.writerow(row)
                sys.stdout.flush()


if __name__ == "__main__":
    main()
