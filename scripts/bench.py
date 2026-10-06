"""Build, test, and explicitly run the main benchmark suite."""

import argparse
from datetime import datetime
import math
import sys

from support.common import ROOT, capture, cli, read_json, require, run, write_json
from support.metadata import compiler_versions, environment, timing_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=0.5)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--filter", default=".")
    args = parser.parse_args()
    require(math.isfinite(args.seconds) and args.seconds > 0 and args.repetitions >= 1, "Invalid benchmark duration or repetitions.")
    run(["cmake", "--preset", "windows"])
    run(["cmake", "--build", "--preset", "release", "--parallel"])
    run(["ctest", "--preset", "release"])
    build = ROOT / "build/windows"
    timing_build(build)
    directory = ROOT / "out" / datetime.now().strftime("%Y%m%d-%H%M%S")
    directory.mkdir(parents=True, exist_ok=False)
    output = directory / "benchmark.json"
    run([build / "Release/weave_bench.exe", f"--benchmark_min_time={args.seconds:g}s",
         f"--benchmark_repetitions={args.repetitions}", f"--benchmark_filter={args.filter}",
         "--benchmark_enable_random_interleaving=true", f"--benchmark_out={output}", "--benchmark_out_format=json"])
    results = read_json(output)
    require(not any(sample.get("error_occurred") for sample in results["benchmarks"]), "A benchmark reported an error.")
    metadata = environment()
    metadata.update(compiler=compiler_versions(build), cmake=capture(["cmake", "--version"]).splitlines()[0],
                    seconds=args.seconds, repetitions=args.repetitions, filter=args.filter, configuration="Release")
    write_json(directory / "environment.json", metadata)
    print(f"Results: {directory}")


if __name__ == "__main__":
    sys.exit(cli(main))
