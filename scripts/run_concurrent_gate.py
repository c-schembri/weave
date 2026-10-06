"""Run the manual performance gate within a hard five-minute process-tree deadline."""

import argparse
from datetime import datetime
from pathlib import Path
import sys
import time

from bench_concurrent import collect
from check_concurrent_gate import EXIT_CODES, PROTOCOL, check_gate, print_report
from support.common import ROOT, cli, read_json, require, sha256, write_json
from support.gate_process import run_owned


def worker(args):
    if args.baseline_directory is not None:
        baseline = read_json(args.baseline_directory / "environment.json")
        require(baseline["isolate_cpus"] == args.isolate_cpus, "Match the baseline CPU placement before measuring.")
    collect(args.build_directory, args.output_directory, profile="ci", isolate_cpus=args.isolate_cpus)
    report = check_gate(args.output_directory, args.baseline_directory)
    print_report(report)
    return EXIT_CODES[report["decision"]]


def supervise(args):
    start = time.monotonic()
    output = args.output_directory.resolve()
    output.mkdir(parents=True, exist_ok=False)
    code, failure = 3, None
    try:
        build = args.build_directory.resolve(strict=True)
        arguments = ["-u", str(Path(__file__).resolve()), "--worker", "--build-directory", str(build), "--output-directory", str(output)]
        if args.isolate_cpus:
            arguments.append("--isolate-cpus")
        if args.baseline_directory is not None:
            arguments.extend(["--baseline-directory", str(args.baseline_directory.resolve(strict=True))])
        print(f"Gate: full matrix, 7 paired A/A and A/B blocks at 250 ms/window; {args.timeout_seconds}s total budget.", flush=True)
        remaining = int((args.timeout_seconds - (time.monotonic() - start) - 3) * 1000)
        code = run_owned(sys.executable, arguments, ROOT, output / "run.log", remaining)
        if code == -1:
            code, failure = 4, "Wall-clock budget exhausted; worker and all descendants terminated."
        elif code not in (0, 1, 2, 3, 5):
            failure, code = f"Worker failed with exit code {code}", 3
    except (OSError, ValueError) as error:
        code, failure = 3, str(error)
    except KeyboardInterrupt:
        code, failure = 3, "Interrupted; owned process tree terminated."
    if failure or not (output / "gate.json").is_file():
        if not failure:
            code, failure = 3, "Worker did not produce a gate decision."
        report = {"protocol": PROTOCOL, "decision": "timeout" if code == 4 else "invalid", "error": failure}
        write_json(output / "gate.json", report)
    report = read_json(output / "gate.json")
    record = {"protocol": PROTOCOL, "decision": report["decision"], "exit_code": code,
              "budget_seconds": args.timeout_seconds, "elapsed_seconds": time.monotonic() - start,
              "runner_sha256": sha256(__file__), "supervisor_sha256": sha256(Path(__file__).parent / "support/gate_process.py")}
    write_json(output / "run.json", record)
    print_report(report)
    print(f"Elapsed {record['elapsed_seconds']:.1f}s; exit {code}.\nEvidence: {output}")
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, default=ROOT / "build/windows")
    parser.add_argument("--output-directory", type=Path, default=ROOT / "benchmarks/results" / datetime.now().strftime("ci-%Y%m%d-%H%M%S"))
    parser.add_argument("--baseline-directory", type=Path)
    parser.add_argument("--isolate-cpus", action="store_true")
    parser.add_argument("--timeout-seconds", type=int, default=300)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not 5 <= args.timeout_seconds <= 300:
        parser.error("--timeout-seconds must be between 5 and 300")
    if args.worker:
        return cli(lambda: worker(args))
    try:
        return supervise(args)
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
