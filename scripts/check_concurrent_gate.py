"""Validate and analyze completed gate evidence; never run measurements."""

import argparse
import math
from pathlib import Path
import sys

from support.common import finite, read_json, require, sha256, write_json
from support.paired_stats import independent_interval, interval, summary


PROTOCOL = "weave-task-policy-5m-v1"
WORKLOADS = ("64/4/1024/0", "1024/1/1024/0", "1024/4/1024/0", "1024/8/1024/0", "256/4/65536/0", "1024/4/1024/512")
METRICS = ("roundtrips_per_second", "client_cycles_per_op", "p99_us")
EXIT_CODES = {"pass": 0, "regression": 1, "inconclusive": 2, "invalid": 3, "timeout": 4, "measurement_unreliable": 5}


def measurement_plan():
    for fixture in range(12):
        for block in range(7):
            for order in range(2):
                phase = (block + fixture + order) % 2
                for slot in range(4):
                    label = int(slot in (1, 2)) ^ ((block + fixture + phase) % 2)
                    yield fixture, phase, block, slot, label, int(phase == 1 and label == 1)
        for block in range(7):
            yield fixture, 2, block, 0, 2, 2


def validate(environment, data, *, historical=False):
    protocols = (PROTOCOL, "weave2-paired-5m-v2") if historical else (PROTOCOL,)
    require(environment["profile"] in protocols, "Gate requires the paired five-minute CI profile.")
    require(type(environment["isolate_cpus"]) is bool, "Missing CPU placement configuration.")
    require(environment["duration_ms"] == 250 and environment["repetitions"] == 7, "CI gate requires 250 ms and 7 paired blocks.")
    limits = environment["limits"]
    require(limits["throughput_regression_pct"] == 5 and limits["cpu_cycles_per_op_regression_pct"] == 5
            and limits["p99_regression_pct"] == 10, "Gate limits differ from the predeclared protocol.")
    context = data["context"]
    require(context["protocol"] in protocols and context["smoke"] == "false" and context["injected_work"] == "0",
            "Not an unmodified full paired run.")
    require(context["duration_ms"] == "250" and context["blocks"] == "7"
            and context["weave_warmup_windows"] == "4" and context["asio_warmup_windows"] == "2",
            "Measurement/warmup plan mismatch.")
    require(context["library_build_type"] == "release", "Gate requires a Release measurement.")
    client_mask, peer_mask = int(context["client_affinity_mask"]), int(context["peer_affinity_mask"])
    require(0 <= client_mask < 2**64 and 0 <= peer_mask < 2**64, "Invalid affinity mask.")
    if environment["isolate_cpus"]:
        require(client_mask.bit_count() == 8 and peer_mask.bit_count() == 4 and not client_mask & peer_mask, "Invalid CPU placement.")
    else:
        require(client_mask == peer_mask == 0, "Unexpected CPU placement.")
    samples = data["benchmarks"]
    require(isinstance(samples, list) and len(samples) == 756, "Incomplete or extra measurement windows.")
    roundtrips = 0
    for sequence, (sample, plan) in enumerate(zip(samples, measurement_plan())):
        require(not sample.get("error_occurred"), "Benchmark reported an error.")
        for key, expected in zip(("fixture_id", "phase", "block", "slot", "label", "implementation"), plan):
            require(type(sample.get(key)) in (int, float) and sample[key] == expected, f"Wrong {key} at window {sequence}.")
        require(sample.get("sequence") == sequence, "Missing, duplicated, or reordered window.")
        fixture = plan[0]
        scheduler = "Affine" if fixture < 6 else "Stealing"
        connections, workers, size, cpu = WORKLOADS[fixture % 6].split("/")
        name = f"Paired{scheduler}/connections:{connections}/workers:{workers}/bytes:{size}/cpu:{cpu}"
        require(sample["run_name"] == name and sample["run_type"] == "iteration" and sample["repetitions"] == 7
                and sample["repetition_index"] == sample["block"], "Workload or repetition mismatch.")
        positive = (*METRICS, "peer_cycles_per_op", "wall_seconds", "samples", "min_connection_samples",
                    "max_connection_samples", "p50_us", "p95_us", "p999_us", "max_us")
        for metric in positive:
            finite(sample.get(metric), math.ulp(0.0), f"{metric} at window {sequence}")
        for metric in ("client_cpu_us_per_op", "client_cores", "peer_cores"):
            finite(sample.get(metric), 0, f"{metric} at window {sequence}")
        count, low, high = sample["samples"], sample["min_connection_samples"], sample["max_connection_samples"]
        require(count >= 1000 and count == math.floor(count) and 1 <= low <= high, "Insufficient or invalid progress.")
        require(int(connections) * low <= count <= int(connections) * high, "Connection accounting mismatch.")
        require(sample["wall_seconds"] >= 0.25, "Truncated measurement.")
        require(sample["p50_us"] <= sample["p95_us"] <= sample["p99_us"] <= sample["p999_us"] <= sample["max_us"],
                "Invalid percentile ordering.")
        require(abs(sample["roundtrips_per_second"] / (count / sample["wall_seconds"]) - 1) < 1e-8,
                "Throughput accounting mismatch.")
        roundtrips += count
    return samples, roundtrips


def tolerance(metric):
    return 0.10 if metric == "p99_us" else 0.05


def classify(metric, bounds, *, control=False):
    low, high = bounds
    limit = tolerance(metric)
    if control:
        return "pass" if low >= 1 - limit and high <= 1 + limit else "unreliable"
    if metric == "roundtrips_per_second":
        return "pass" if low >= 0.95 else "regression" if high < 0.95 else "inconclusive"
    return "pass" if high <= 1 + limit else "regression" if low > 1 + limit else "inconclusive"


def block_ratios(case, metric, phase):
    return [math.exp(sum((1 if window["label"] == 1 else -1) * math.log(window[metric]) / 2
                         for window in case if window["phase"] == phase and window["block"] == block))
            for block in range(7)]


def policy_checks(samples):
    checks, rows = [], []
    for fixture in range(12):
        case = [sample for sample in samples if sample["fixture_id"] == fixture]
        scheduler = "affine" if fixture < 6 else "stealing"
        workload = WORKLOADS[fixture % 6]
        metrics = {}
        for metric in (*METRICS, "client_cpu_us_per_op"):
            values = {}
            for label, name in enumerate(("explicit_result", "weave", "asio")):
                phase = 2 if label == 2 else 1
                middle, cv = summary([window[metric] for window in case if window["phase"] == phase and window["label"] == label])
                values[name] = {"median": middle, "cv": cv}
            if metric in METRICS:
                for phase, kind in enumerate(("AA", "AB")):
                    ratios = block_ratios(case, metric, phase)
                    middle, cv = summary(ratios)
                    bounds = interval(ratios)
                    status = classify(metric, bounds, control=phase == 0)
                    values[kind] = {"ratio": middle, "ratio_ci90": bounds, "block_cv": cv, "block_ratios": ratios, "status": status}
                    checks.append({"kind": kind, "scheduler": scheduler, "workload": workload, "metric": metric,
                                   "change_pct": (middle - 1) * 100, "ratio_ci90": bounds, "status": status})
            metrics[metric] = values
        rows.append({"scheduler": scheduler, "workload": workload, "metrics": metrics})
    return checks, rows


def historical_checks(directory, environment, data, samples):
    raw_path, environment_path = directory / "paired.json", directory / "environment.json"
    baseline, baseline_environment = read_json(raw_path), read_json(environment_path)
    prior, _ = validate(baseline_environment, baseline, historical=True)
    for key in ("client_affinity_mask", "peer_affinity_mask"):
        require(baseline["context"][key] == data["context"][key], "Historical CPU placement differs.")
    # CIM padded CPU names; the registry-backed Python collector does not.
    def cpu_identity(items):
        return [(item["Name"].strip(), item["NumberOfCores"], item["NumberOfLogicalProcessors"]) for item in items]

    require(cpu_identity(baseline_environment["cpu"]) == cpu_identity(environment["cpu"])
            and all(baseline_environment["os"][key] == environment["os"][key] for key in ("Version", "BuildNumber"))
            and baseline_environment["power_scheme"] == environment["power_scheme"],
            "Historical hardware, OS, or power policy differs.")
    checks = []
    for fixture in range(12):
        before_case = [window for window in prior if window["fixture_id"] == fixture]
        after_case = [window for window in samples if window["fixture_id"] == fixture]
        for metric in METRICS:
            require(classify(metric, interval(block_ratios(before_case, metric, 0)), control=True) == "pass",
                    "Historical same-code controls failed.")
            groups = []
            for case in (before_case, after_case):
                groups.append([math.exp(sum(math.log(window[metric]) / 2 for window in case
                                            if window["phase"] == 1 and window["label"] == 1 and window["block"] == block))
                               for block in range(7)])
            before, after = summary(groups[0])[0], summary(groups[1])[0]
            bounds = independent_interval(*groups)
            checks.append({"scheduler": "affine" if fixture < 6 else "stealing", "workload": WORKLOADS[fixture % 6],
                           "metric": metric, "before": before, "after": after, "change_pct": (after / before - 1) * 100,
                           "ratio_ci90": bounds, "status": classify(metric, bounds)})
    provenance = {"raw_sha256": sha256(raw_path), "environment_sha256": sha256(environment_path),
                  "protocol": baseline["context"]["protocol"]}
    return checks, provenance


def evaluate(directory, baseline_directory=None):
    environment_path, raw_path = directory / "environment.json", directory / "paired.json"
    environment, data = read_json(environment_path), read_json(raw_path)
    samples, roundtrips = validate(environment, data)
    checks, rows = policy_checks(samples)
    unreliable = sum(check["kind"] == "AA" and check["status"] != "pass" for check in checks)
    regressed = sum(check["kind"] == "AB" and check["status"] == "regression" for check in checks)
    inconclusive = sum(check["kind"] == "AB" and check["status"] == "inconclusive" for check in checks)
    decision = "measurement_unreliable" if unreliable else "regression" if regressed else "inconclusive" if inconclusive else "pass"
    historical, provenance = [], None
    if baseline_directory is not None:
        historical, provenance = historical_checks(Path(baseline_directory), environment, data, samples)
        if not unreliable:
            if any(check["status"] == "regression" for check in historical):
                decision = "regression"
            elif decision == "pass" and any(check["status"] == "inconclusive" for check in historical):
                decision = "inconclusive"
    return {
        "protocol": PROTOCOL, "decision": decision,
        "comparison": "Historical Task artifact plus in-process propagation-policy controls" if baseline_directory is not None
                      else "Propagation policy only; not a version-to-version non-regression decision",
        "historical_confidence": "Independent 90% bootstrap intervals over seven block means per version; cross-run drift is not cancelled; not paired across binaries",
        "historical_checks": historical, "baseline_provenance": provenance,
        "scope": "Windows IOCP; warmed persistent connections; fixed-concurrency closed-loop; not CPU-time or production-SLO certification",
        "limits": {"throughput_min_ratio": 0.95, "client_cycles_max_ratio": 1.05, "p99_max_ratio": 1.10},
        "confidence": "90% percentile bootstrap of median paired block ratios; 20000 whole-block resamples; 7 ABBA/BAAB blocks per phase/case; exploratory per-case coverage",
        "controls_passed": 36 - unreliable, "controls_unreliable": unreliable,
        "checks_passed": 36 - regressed - inconclusive, "checks_regressed": regressed, "checks_inconclusive": inconclusive,
        "trials": len(samples), "verified_roundtrips": roundtrips,
        "provenance": {"checker_sha256": sha256(__file__), "stats_sha256": sha256(Path(__file__).parent / "support/paired_stats.py"),
                       "environment_sha256": sha256(environment_path), "raw_sha256": sha256(raw_path)},
        "checks": checks, "rows": rows,
    }


def check_gate(directory, baseline_directory=None, *, publish=True):
    directory = Path(directory)
    try:
        report = evaluate(directory, baseline_directory)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError, ZeroDivisionError) as error:
        report = {"protocol": PROTOCOL, "decision": "invalid", "error": str(error)}
    if publish and directory.is_dir():
        write_json(directory / "gate.json", report)
    return report


def print_report(report):
    print(f"Task gate: {report['decision']}")
    if "error" in report:
        print(report["error"])
    else:
        print(f"{report['controls_passed']}/36 A/A controls; {report['checks_passed']}/36 A/B checks passed; "
              f"{report['checks_regressed']} regressed; {report['checks_inconclusive']} inconclusive.")
    if report.get("historical_checks"):
        print(f"{sum(check['status'] == 'pass' for check in report['historical_checks'])}/36 historical checks passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--baseline-directory", type=Path)
    args = parser.parse_args()
    report = check_gate(args.directory, args.baseline_directory)
    print_report(report)
    return EXIT_CODES[report["decision"]]


if __name__ == "__main__":
    sys.exit(main())
