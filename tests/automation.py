"""Python tooling tests use synthetic measurements and mocked benchmark processes."""

import argparse
from contextlib import redirect_stdout
import io
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import analyze_concurrent
import bench_concurrent
import bench_iocp
import run_concurrent_gate
from paired_smoke import validate_smoke
from support.common import ROOT, read_json, write_json
from support.metadata import source_files, timing_build
from support.paired_stats import ProtocolRandom, independent_interval, interval, summary


class AutomationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="weave-python-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve(strict=True)

    def test_statistics_match_original_fixed_seed_protocol(self):
        random = ProtocolRandom(3601)
        self.assertEqual([random.next(100000) for _ in range(12)],
                         [97979, 41776, 82377, 16615, 618, 46488, 35819, 771, 48138, 14640, 25853, 82003])
        self.assertEqual(interval([0.8, 0.9, 1, 1.01, 1.05, 1.2, 1.3]), (0.9, 1.2))
        self.assertEqual(independent_interval([1, 2, 3, 4, 5, 6, 7], [2, 3, 5, 7, 11, 13, 17]), (0.75, 13 / 3))
        self.assertEqual(summary([0, 0]), (0, 0))
        self.assertAlmostEqual(summary([1, 2, 3])[1], 0.5)
        self.assertTrue(math.isnan(independent_interval([0] * 7, [0] * 7)[0]))

    def test_iocp_matrix_requires_all_cases_repetitions_and_connection_progress(self):
        samples = []
        for family in bench_iocp.FAMILIES:
            for workload in bench_iocp.WORKLOADS:
                suffix = "/" + "/".join(f"{key}:{value}" for key, value in
                                         zip(("connections", "workers", "bytes", "cpu", "uneven"), workload))
                for repetition in range(7):
                    sample = {"run_name": family + suffix + "/iterations:1/manual_time", "run_type": "iteration",
                              "repetition_index": repetition, "samples": 1000, "min_connection_samples": 1}
                    sample.update({metric: 100 for metric in bench_iocp.METRICS})
                    samples.append(sample)
        with patch.object(bench_iocp, "independent_interval", return_value=(1, 1)):
            report = bench_iocp.analyze({"benchmarks": samples})
            self.assertEqual(len(report["rows"]), 20)
            self.assertEqual(report["rows"][0]["metrics"]["p99_us"]["change_pct"], 0)
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                bench_iocp.analyze({"benchmarks": samples[:-1]})
            samples[-1]["repetition_index"] = 0
            with self.assertRaisesRegex(ValueError, "duplicate"):
                bench_iocp.analyze({"benchmarks": samples})
            samples[-1]["repetition_index"] = 6
            samples[-1]["min_connection_samples"] = 0
            with self.assertRaisesRegex(ValueError, "stalled"):
                bench_iocp.analyze({"benchmarks": samples})

    def test_analyzer_zero_cpu_diagnostics_and_matched_runs(self):
        samples = []
        for prefix in ("WeaveExplicitConcurrentAffine", "WeaveConcurrentAffine", "AsioConcurrentAffine"):
            for repetition in range(7):
                sample = {"run_name": prefix + "/fixture", "run_type": "iteration", "repetition_index": repetition}
                sample.update({metric: 100 for metric in analyze_concurrent.METRICS})
                sample["client_cpu_us_per_op"] = 0
                samples.append(sample)
        for run in ("comparison", "confirmation"):
            write_json(self.root / f"{run}.json", {"benchmarks": samples})
        report = analyze_concurrent.analyze(self.root)
        self.assertEqual(len(report["rows"]), 2)
        self.assertEqual(report["rows"][0]["metrics"]["client_cycles_per_op"]["status"], "within_limit")
        diagnostic = report["rows"][0]["metrics"]["client_cpu_us_per_op"]
        self.assertIsNone(diagnostic["ratio_ci90"])
        self.assertEqual(diagnostic["cv_pct"], [None] * 3)
        self.assertEqual(read_json(self.root / "analysis.json")["sample_counts"], {"comparison": 21, "confirmation": 21})
        write_json(self.root / "comparison.json", {"benchmarks": samples[:-1]})
        with self.assertRaisesRegex(ValueError, "Unmatched"):
            analyze_concurrent.analyze(self.root)

    def test_collector_arguments_and_evidence_protection(self):
        build = self.root / "build"
        (build / "Release").mkdir(parents=True)
        binary = build / "Release/weave_concurrent.exe"
        binary.write_bytes(b"synthetic executable; never launched")
        (build / "CMakeCache.txt").write_text("WEAVE_ENABLE_ASAN:BOOL=OFF\nWEAVE_PROFILE_RUNTIME:BOOL=OFF\n")
        metadata = {"cpu": [], "os": {}, "power_scheme": "fixture"}
        with patch.object(bench_concurrent, "environment", return_value=metadata), \
             patch.object(bench_concurrent, "source_files", return_value=[binary]), \
             patch.object(bench_concurrent, "run") as run, redirect_stdout(io.StringIO()):
            output = self.root / "paired"
            bench_concurrent.collect(build, output, profile="ci", isolate_cpus=True)
            arguments = run.call_args.args[0]
            self.assertIn("--weave_isolate_cpus", arguments)
            self.assertIn(f"--weave_paired_out={output / 'paired.json'}", arguments)
            environment = read_json(output / "environment.json")
            self.assertEqual((environment["duration_ms"], environment["repetitions"]), (250, 7))
            self.assertEqual(environment["executable"]["Algorithm"], "SHA256")
            with self.assertRaisesRegex(ValueError, "overwrite"):
                bench_concurrent.collect(build, output, profile="ci")
            with self.assertRaisesRegex(ValueError, "full workload matrix"):
                bench_concurrent.collect(build, self.root / "bad", profile="ci", filter="partial")
            with self.assertRaisesRegex(ValueError, "at least"):
                bench_concurrent.collect(build, self.root / "bad", duration_ms=10)
            self.assertEqual(run.call_count, 1)

    def test_instrumented_builds_rejected_and_cache_files_not_hashed(self):
        for flag in ("WEAVE_ENABLE_ASAN", "WEAVE_PROFILE_RUNTIME"):
            (self.root / "CMakeCache.txt").write_text(f"{flag}:BOOL=ON\n")
            with self.assertRaisesRegex(ValueError, flag):
                timing_build(self.root)
        self.assertTrue(all("__pycache__" not in path.parts and path.suffix != ".pyc" for path in source_files()))

    def test_full_collector_runs_both_sets_and_prints_analysis(self):
        build = self.root / "build"
        (build / "Release").mkdir(parents=True)
        (build / "Release/weave_concurrent.exe").write_bytes(b"not executable")
        (build / "CMakeCache.txt").write_text("WEAVE_ENABLE_ASAN:BOOL=OFF\nWEAVE_PROFILE_RUNTIME:BOOL=OFF\n")

        def fake_run(arguments):
            output = next(str(argument).split("=", 1)[1] for argument in arguments
                          if str(argument).startswith("--benchmark_out="))
            write_json(output, {"benchmarks": [{"run_type": "iteration", "samples": 1000, "min_connection_samples": 1}]})

        report = {"rows": []}
        with patch.object(bench_concurrent, "environment", return_value={}), \
             patch.object(bench_concurrent, "source_files", return_value=[]), \
             patch.object(bench_concurrent, "run", side_effect=fake_run) as run, \
             patch.object(bench_concurrent, "analyze", return_value=report) as analyze, \
             patch.object(bench_concurrent, "print_report") as print_report, redirect_stdout(io.StringIO()):
            output = self.root / "full"
            bench_concurrent.collect(build, output, duration_ms=1200, repetitions=9, filter="fixture")
            self.assertEqual(run.call_count, 2)
            for call in run.call_args_list:
                self.assertIn("--weave_duration_ms=1200", call.args[0])
                self.assertIn("--benchmark_repetitions=9", call.args[0])
                self.assertIn("--benchmark_filter=fixture", call.args[0])
            analyze.assert_called_once_with(output)
            print_report.assert_called_once_with(report)

    def test_timeout_and_worker_failure_publication_without_measurements(self):
        for code, expected, exit_code in ((-1, "timeout", 4), (99, "invalid", 3), (0, "invalid", 3)):
            with self.subTest(code=code):
                output = self.root / str(code)
                args = argparse.Namespace(output_directory=output, build_directory=self.root, baseline_directory=None,
                                          isolate_cpus=False, timeout_seconds=5)
                with patch.object(run_concurrent_gate, "run_owned", return_value=code) as run, redirect_stdout(io.StringIO()):
                    self.assertEqual(run_concurrent_gate.supervise(args), exit_code)
                self.assertEqual(read_json(output / "gate.json")["decision"], expected)
                self.assertEqual(read_json(output / "run.json")["exit_code"], exit_code)
                self.assertLessEqual(run.call_args.args[-1], 2000)
                self.assertGreater(run.call_args.args[-1], 0)

    def test_smoke_validator_detects_missing_positive_control(self):
        samples = []
        for fixture in (0, 6):
            for index in range(18):
                label = index % 2
                samples.append({"sequence": len(samples), "fixture_id": fixture, "phase": 1, "label": label,
                                "min_connection_samples": 1, "client_cycles_per_op": 100 if label == 0 else 200})
        data = {"context": {"injected_work": "200000"}, "benchmarks": samples}
        with redirect_stdout(io.StringIO()):
            validate_smoke(data)
        for sample in samples:
            sample["client_cycles_per_op"] = 100
        with self.assertRaisesRegex(ValueError, "injected CPU cost"):
            validate_smoke(data)

    def test_all_entrypoints_have_help_without_running_work(self):
        scripts = [ROOT / "scripts" / f"{name}.py" for name in
                   ("bench", "bench_concurrent", "analyze_concurrent", "check_concurrent_gate", "run_concurrent_gate")]
        scripts.extend([ROOT / "tests/paired_smoke.py", ROOT / "modules/tcp/tests/echo_example.py"])
        for script in scripts:
            with self.subTest(script=script.name):
                result = subprocess.run([sys.executable, str(script), "--help"], cwd=self.root,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("usage:", result.stdout)


if __name__ == "__main__":
    unittest.main()
