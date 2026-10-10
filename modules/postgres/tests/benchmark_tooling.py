"""Synthetic concurrency-harness regression tests. No database or timing runs."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "benchmarks"))
import run_concurrent as bench


def metrics(operations=128, seconds=1, cycles=100000):
    return {"operations": operations, "roundtrips": operations, "seconds": seconds,
            "cpu_seconds": 0.5, "cpu_cycles": cycles, "ops_per_second": operations / seconds,
            "p50_us": 10, "p95_us": 20, "p99_us": 30, "checksum": 42, "libpq_version": 180004}


class MetricsTests(unittest.TestCase):
    def test_fixed_physical_core_budget(self):
        cores = [[0, 1], [2, 3], [4, 5], [6, 7]]
        self.assertEqual(bench.placement(cores, 1, 2), [6])
        self.assertEqual(bench.placement(cores, 2, 2), [4, 6])
        self.assertIsNone(bench.placement(cores, 4, 2))
        invalid = ([], [[0], []], [[0, 1], [1, 2]], [[-1]], [[True]])
        for topology in invalid:
            with self.subTest(topology=topology), self.assertRaises(ValueError):
                bench.placement(topology, 1, 0)

    def test_completed_counts_and_latency_validation(self):
        sample = metrics(64)
        bench.validate_metrics(sample, "simple", 8, 8, 32)
        sample["operations"] *= 32
        sample["ops_per_second"] *= 32
        bench.validate_metrics(sample, "batch", 8, 8, 32)
        invalid = {"operations": 1.0, "roundtrips": 0, "seconds": float("nan"), "cpu_seconds": -1,
                   "cpu_cycles": -1, "ops_per_second": 1, "p99_us": 5, "checksum": True,
                   "libpq_version": 170000}
        for key, value in invalid.items():
            with self.subTest(field=key), self.assertRaises(ValueError):
                bench.validate_metrics(dict(sample, **{key: value}), "batch", 8, 8, 32)

    def test_unreliable_cpu_time_is_not_zero_cost(self):
        samples = [dict(metrics(), cpu_seconds=0) for _ in range(3)]
        statistics = bench.summary(samples, cpu_time_reliable=False)
        self.assertIsNone(statistics["cpu_pct"])
        self.assertIsNone(statistics["cpu_us_per_operation"])
        self.assertEqual(statistics["cycles_per_operation"]["median"], 100000 / 128)
        quality = bench.quality(samples, statistics, 1, False)
        self.assertEqual(quality["throughput"], "stable")
        self.assertEqual(quality["cpu_time"], "unreliable counter; unavailable")

    def test_linux_cpu_time_and_unavailable_cycles(self):
        samples = [metrics(cycles=0) for _ in range(3)]
        statistics = bench.summary(samples)
        self.assertEqual(statistics["cpu_pct"], 50)
        self.assertIsNone(statistics["cycles_per_operation"])

    def test_noise_is_metric_specific_and_outliers_are_retained(self):
        samples = [metrics() for _ in range(7)]
        samples[-1]["p99_us"] = 90
        statistics = bench.summary(samples)
        quality = bench.quality(samples, statistics, 1, True)
        self.assertEqual(quality["throughput"], "stable")
        self.assertEqual(quality["p99"], "noisy")
        self.assertEqual(len(samples), 7)
        self.assertEqual(statistics["p99_us"]["spread_pct"], 200)
        self.assertEqual(bench.quality(samples, statistics, 2, True)["throughput"], "short")

    def test_paired_ratios_are_deterministic_and_require_matching_samples(self):
        baseline = [metrics(100), metrics(200), metrics(300)]
        candidate = [metrics(200), metrics(400), metrics(600)]
        interval = bench.paired_interval(candidate, baseline, "ops_per_second")
        self.assertEqual(interval, {"median": 2, "low": 2, "high": 2})
        self.assertEqual(interval, bench.paired_interval(candidate, baseline, "ops_per_second"))
        with self.assertRaises(ValueError):
            bench.paired_interval(candidate[:2], baseline, "ops_per_second")


class EvidenceTests(unittest.TestCase):
    def exercise(self, failure=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "synthetic-fixture"
            executable.write_bytes(b"not executed")
            output = root / "evidence.json"
            commands = []

            def fake_run(command, **kwargs):
                commands.append(command)
                self.assertNotIn("PGPASSWORD", kwargs["env"])
                if command[-1] == "--topology":
                    return subprocess.CompletedProcess(command, 0, json.dumps({
                        "cores": [[0, 1], [2, 3], [4, 5], [6, 7]], "libpq_version": 180004}), "")
                if failure == "exit":
                    return subprocess.CompletedProcess(command, 1, "", "retained setup error")
                if failure == "timeout":
                    raise subprocess.TimeoutExpired(command, 180, output="partial output", stderr="partial error")
                sample = metrics(int(command[4]) * int(command[5]))
                if failure == "invalid":
                    sample["roundtrips"] = 0
                if failure == "checksum" and command[1] == "libpq":
                    sample["checksum"] = 43
                return subprocess.CompletedProcess(command, 0, json.dumps(sample), "")

            arguments = ["run_concurrent.py", "--executable", str(executable), "--workers", "1", "2", "4",
                         "--connections", "8", "--seconds", "1", "--repetitions", "3", "--workloads", "simple",
                         "--server-description", "Synthetic database; not executed", "--output", str(output)]
            with (patch.object(sys, "argv", arguments), patch.dict("os.environ", {
                    "WEAVE_PG_PASSWORD": "synthetic", "PGPASSWORD": "must be stripped"}),
                  patch.object(bench.subprocess, "run", side_effect=fake_run),
                  patch.object(bench.platform, "system", return_value="Windows"),
                  patch.object(bench.platform, "platform", return_value="synthetic"),
                  patch.object(bench.platform, "processor", return_value="synthetic"),
                  patch.object(bench, "source_fingerprint", return_value="frozen"), redirect_stdout(io.StringIO())):
                if failure:
                    with self.assertRaises((RuntimeError, ValueError, subprocess.TimeoutExpired)):
                        bench.main()
                else:
                    bench.main()
            return json.loads(output.read_text(encoding="utf-8")), commands

    def test_success_records_all_samples_and_unavailable_counts(self):
        evidence, commands = self.exercise()
        self.assertTrue(evidence["complete"])
        self.assertIsNone(evidence["placements"]["4"])
        self.assertEqual(len(evidence["records"]), 24)
        self.assertEqual(len(evidence["summaries"]), 6)
        self.assertEqual(evidence["source_sha256"], evidence["source_sha256_after"])
        self.assertIsNone(evidence["before"])
        for row in evidence["summaries"]:
            self.assertIsNone(row["statistics"]["cpu_pct"])
            self.assertTrue(row["comparable_to_libpq"]["throughput"])
        sample_order = [record["library"] for record in evidence["records"] if record["phase"] == "sample"]
        self.assertEqual(sample_order[:9], [library for order in bench.ORDERS[:3] for library in order])
        self.assertEqual(len(commands), 25)

    def test_failures_are_evidence_not_successful_or_retried_runs(self):
        cases = ("exit", "timeout", "invalid", "checksum")
        for case in cases:
            with self.subTest(failure=case):
                evidence, commands = self.exercise(case)
                self.assertFalse(evidence["complete"])
                self.assertIn("failure", evidence)
                self.assertFalse(evidence["summaries"])
                self.assertEqual(len(evidence["records"]), 3 if case == "checksum" else 1)
                if case == "exit":
                    self.assertEqual(evidence["records"][0]["stderr"], "retained setup error")
                if case == "timeout":
                    self.assertEqual(evidence["records"][0]["exit_code"], None)


if __name__ == "__main__":
    unittest.main()
