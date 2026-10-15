"""Analyze the two independent full-profile concurrent benchmark runs."""

import argparse
import math
from pathlib import Path
from statistics import median
import sys

from support.common import cli, finite, read_json, require, sha256, write_json
from support.paired_stats import independent_interval, summary


METRICS = ("roundtrips_per_second", "client_cycles_per_op", "p99_us", "client_cpu_us_per_op",
           "client_cores", "p50_us", "p999_us", "peer_cores")


def json_number(value):
    return value if math.isfinite(value) else None


def analyze(directory):
    directory = Path(directory)
    rows, sample_counts = [], {}
    for run in ("comparison", "confirmation"):
        data = read_json(directory / f"{run}.json")
        require(not any(sample.get("error_occurred") for sample in data["benchmarks"]), f"Invalid run: {run}")
        samples = [sample for sample in data["benchmarks"] if sample["run_type"] == "iteration"]
        sample_counts[run] = len(samples)
        for scheduler in ("Affine", "Stealing"):
            prefix = f"WeaveExplicitConcurrent{scheduler}"
            asio_prefix = "AsioConcurrentAffine" if scheduler == "Affine" else "AsioConcurrentShared"
            names = dict.fromkeys(sample["run_name"] for sample in samples if sample["run_name"].startswith(prefix + "/"))
            for name in names:
                suffix = name[len(prefix):]
                groups = [[sample for sample in samples if sample["run_name"] == match]
                          for match in (name, f"WeaveConcurrent{scheduler}{suffix}", asio_prefix + suffix)]
                require(len(groups[0]) >= 7 and len({len(group) for group in groups}) == 1, f"Unmatched samples: {name}")
                row = {"run": run, "scheduler": scheduler, "workload": suffix, "repetitions": len(groups[0]), "metrics": {}}
                index = 0
                for metric in METRICS:
                    values = [[sample[metric] for sample in group] for group in groups]
                    for group in values:
                        for value in group:
                            finite(value, 0, metric)
                    a, b, c = [median(group) for group in values]
                    bounds = None
                    if a > 0:
                        bounds = independent_interval(values[0], values[1], 1729 + index, exploratory=True)
                        index += 1
                    status = "diagnostic"
                    if metric in ("roundtrips_per_second", "client_cycles_per_op", "p99_us"):
                        require(bounds is not None, f"Zero baseline for {metric}.")
                        if metric == "roundtrips_per_second":
                            status = "within_limit" if bounds[0] >= 0.95 else "regression" if bounds[1] < 0.95 else "inconclusive"
                        else:
                            limit = 1.10 if metric == "p99_us" else 1.05
                            status = "within_limit" if bounds[1] <= limit else "regression" if bounds[0] > limit else "inconclusive"
                    row["metrics"][metric] = {
                        "explicit_result": a, "weave": b, "asio": c, "change_pct": 100 * (b / a - 1) if a > 0 else None,
                        "ratio_ci90": [json_number(value) for value in bounds] if bounds is not None else None,
                        "cv_pct": [json_number(100 * summary(group)[1]) if sum(group) else None for group in values],
                        "status": status,
                    }
                rows.append(row)
    require(bool(rows), "No matched concurrent workloads found.")
    report = {
        "method": "Independent percentile bootstrap of median ratios, 20000 resamples, deterministic seeds, 90% intervals; repetitions are the unit of resampling, not individual RTTs",
        "caution": "Exploratory per-case intervals, not simultaneous family-wise guarantees; CPU cycles are not CPU seconds; tail percentiles are medians of per-repetition percentiles, not a pooled percentile",
        "non_finite_numbers": "A null interval endpoint or CV denotes an undefined or unbounded value, including CPU-time ratios with zero baseline samples",
        "provenance": {"analyzer_sha256": sha256(__file__), "stats_sha256": sha256(Path(__file__).parent / "support/paired_stats.py"),
                       "comparison_sha256": sha256(directory / "comparison.json"), "confirmation_sha256": sha256(directory / "confirmation.json")},
        "sample_counts": sample_counts, "rows": rows,
    }
    write_json(directory / "analysis.json", report)
    return report


def print_report(report):
    for row in report["rows"]:
        changes = [f"{name} {row['metrics'][metric]['change_pct']:+.1f}% ({row['metrics'][metric]['status']})"
                   for name, metric in (("throughput", "roundtrips_per_second"), ("cycles/op", "client_cycles_per_op"), ("p99", "p99_us"))]
        print(f"{row['run']} {row['scheduler']} {row['workload']}: " + "; ".join(changes))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    print_report(analyze(args.directory))


if __name__ == "__main__":
    sys.exit(cli(main))
