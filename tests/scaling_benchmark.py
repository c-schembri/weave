"""Synthetic checks for physical core accounting, matched scaling, and evidence packaging."""

from contextlib import redirect_stdout
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bench_scaling
from bench_tokio import BACKENDS, METRICS, WORKLOADS
from support.common import write_json


def evidence():
    counts = [1, 2, 4, 8]
    metadata = {"protocol": bench_scaling.PROTOCOL, "requested_cores": list(bench_scaling.CORE_COUNTS),
                "cpu_masks": {str(count): {"server": (1 << count) - 1, "client": 0xF00} for count in counts},
                "available_physical_cores": 12, "unavailable": {"16": "insufficient cores", "32": "insufficient cores"},
                "client_cores": 4, "client_workers": 4, "duration_ms": 2000, "warmup_ms": 250, "repetitions": 7,
                "seed": 60106, "smoke": False, "workloads": [list(workload) for workload in WORKLOADS],
                "revision": "a" * 40, "worktree": [], "cpu": [{"Name": "Synthetic CPU"}],
                "timestamp": "2026-10-06T17:00:00+11:00", "elapsed_seconds": 900.0}
    samples = []
    for repetition, cores, workload, backend in bench_scaling.schedule(counts, WORKLOADS, 7, 60106):
        name, connections, size, work, uneven = workload
        count = (6000 if backend == "weave" else 5000) * cores
        sample = {metric: 1.0 for metric in METRICS}
        sample.update(workload=name, backend=backend, repetition=repetition, server_workers=cores,
                      connections=connections, bytes=size, cpu_iterations=work, uneven=uneven,
                      samples=count, min_connection_samples=1, max_connection_samples=2000, wall_seconds=2.01,
                      roundtrips_per_second=count / 2.01, client_cycles_per_op=100, p50_us=10, p95_us=20,
                      p99_us=30, p999_us=40, max_us=100)
        samples.append(sample)
    return samples, metadata


class ScalingTests(unittest.TestCase):
    def test_before_and_shared_candidates_are_complete_matched_cohorts(self):
        samples, metadata = evidence()
        metadata["before"] = {"revision": "b" * 40, "worktree": [],
                              "binary": {"Hash": "B" * 64}, "evidence": {"Hash": "C" * 64}}
        metadata["shared_candidate"] = True
        additions = []
        for sample in samples:
            if sample["backend"] != "weave":
                continue
            for backend, ratio in (("weave-before", 1 / 1.2), ("weave-shared", 2)):
                count = round(sample["samples"] * ratio)
                additions.append(dict(sample, backend=backend, samples=count, roundtrips_per_second=count / sample["wall_seconds"]))
        samples.extend(additions)
        result = bench_scaling.report(samples, metadata)
        self.assertEqual(len(result["rows"]), 80)
        self.assertEqual(len(result["comparisons"]), 96)
        pairs = [pair for pair in result["comparisons"] if pair["baseline"] == "weave-before"]
        self.assertEqual(len(pairs), 32)
        for pair in pairs:
            self.assertAlmostEqual(pair["ratio"], 1.2 if pair["candidate"] == "weave" else 2.4)
        text = bench_scaling.compact_markdown(result, metadata, "https://example.test/evidence")
        self.assertIn("Weave before | Weave after | Paired change", text)
        self.assertIn("+20.0%", text)
        table = [line for line in text.splitlines() if line.startswith("|")]
        self.assertEqual(len(table), 8)
        self.assertIn("\n".join(table), text)
        shared = bench_scaling.compact_markdown(result, metadata, "https://example.test/evidence", candidate="weave-shared")
        self.assertIn("+140.0%", shared)
        self.assertIn("not the default", shared)
        self.assertIn("not the fast path in isolation", shared)
        self.assertIn("weave-shared", bench_scaling.full_markdown(result, metadata, "https://example.test/evidence"))
        self.assertEqual(len(list(bench_scaling.schedule([1, 2, 4, 8], WORKLOADS, 7, 60106, bench_scaling.backends(metadata)))), 560)
        with self.assertRaises(ValueError):
            bench_scaling.report(samples[:-1], metadata)
        changed = copy.deepcopy(metadata)
        changed["before"]["binary"]["Hash"] = "invalid"
        with self.assertRaises(ValueError):
            bench_scaling.report(samples, changed)

    def test_schedule_is_serial_complete_and_matched_across_core_counts(self):
        cases = list(bench_scaling.schedule([1, 2, 4, 8], WORKLOADS, 7, 60106))
        self.assertEqual(cases, list(bench_scaling.schedule([1, 2, 4, 8], WORKLOADS, 7, 60106)))
        self.assertEqual(len(cases), 336)
        self.assertEqual(len(set(cases)), 336)
        for repetition in range(7):
            block = [case[1:] for case in cases if case[0] == repetition]
            self.assertEqual(len(block), 48)
            self.assertEqual(set(block), {(count, workload, backend) for count in (1, 2, 4, 8)
                                         for workload in WORKLOADS for backend in BACKENDS})

    def test_planning_preserves_fixed_clients_and_reports_unavailable_counts(self):
        def capture(arguments):
            count = arguments[-2]
            return json.dumps({"server": (1 << count) - 1 if count <= 8 else 0,
                               "client": 0xF00 if count <= 8 else 0, "available_physical_cores": 12})
        with patch.object(bench_scaling, "capture", side_effect=capture):
            result = bench_scaling.plan("load", bench_scaling.CORE_COUNTS, 4)
        self.assertEqual(set(result["cpu_masks"]), {"1", "2", "4", "8"})
        self.assertEqual(set(result["unavailable"]), {"16", "32"})
        self.assertEqual({mask["client"] for mask in result["cpu_masks"].values()}, {0xF00})
        malformed = ({"server": 1, "client": 1, "available_physical_cores": 12},
                     {"server": 0, "client": 0, "available_physical_cores": 12},
                     {"server": True, "client": 0xF00, "available_physical_cores": 12})
        for value in malformed:
            with self.subTest(value=value), patch.object(bench_scaling, "capture", return_value=json.dumps(value)), \
                    self.assertRaises(ValueError):
                bench_scaling.plan("load", (1,), 4)

    def test_failed_window_stops_the_sweep_without_complete_analysis(self):
        samples, metadata = evidence()
        calls = []

        def measure(args, backend, workload, repetition, masks):
            expected = samples[len(calls)]
            self.assertEqual((args.server_workers, backend, workload[0], repetition),
                             (expected["server_workers"], expected["backend"], expected["workload"], expected["repetition"]))
            self.assertEqual(masks["client"], 0xF00)
            calls.append(expected)
            if len(calls) == 3:
                raise ValueError("Synthetic setup failure")
            return expected

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = SimpleNamespace(output_directory=root, load_binary="load", server_cores=list(bench_scaling.CORE_COUNTS),
                                   client_cores=4, duration_ms=2000, warmup_ms=250, repetitions=7, seed=60106, smoke=False)
            allocation = {name: metadata[name] for name in ("cpu_masks", "unavailable", "available_physical_cores")}
            with patch.object(bench_scaling, "read_json", return_value=copy.deepcopy(metadata)), \
                    patch.object(bench_scaling, "plan", return_value=allocation), \
                    patch.object(bench_scaling, "measure", side_effect=measure), \
                    patch.object(bench_scaling, "write_json") as write, patch.object(bench_scaling, "report") as report, \
                    redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, "Synthetic setup failure"):
                bench_scaling.worker(args)
            self.assertEqual(len(calls), 3)
            self.assertNotIn("analysis.json", [call.args[0].name for call in write.call_args_list])
            self.assertFalse((root / "summary.md").exists())
            report.assert_not_called()

    def test_complete_report_has_per_backend_scaling_and_all_measurements(self):
        samples, metadata = evidence()
        result = bench_scaling.report(samples, metadata)
        self.assertEqual(len(result["rows"]), 48)
        self.assertEqual(len(result["comparisons"]), 32)
        self.assertEqual(result, json.loads(json.dumps(result)))
        for row in result["rows"]:
            self.assertAlmostEqual(row["speedup"], row["server_workers"])
            self.assertAlmostEqual(row["efficiency"], 1.0)
            self.assertFalse(row["throughput_noisy"])
        for pair in result["comparisons"]:
            self.assertAlmostEqual(pair["ratio"], 1.2)
        text = bench_scaling.compact_markdown(result, metadata, "https://example.test/evidence")
        self.assertIn("16 | N/A", text)
        self.assertIn("32 | N/A", text)
        self.assertIn("4 fixed, physically separate client cores", text)
        self.assertIn("source", text.lower())
        self.assertIn("p99.9", bench_scaling.full_markdown(result, metadata, "https://example.test/evidence"))

    def test_partial_wrong_budgets_changed_protocol_and_stalled_windows_fail(self):
        samples, metadata = evidence()
        mutations = (samples[:-1], samples + [samples[0]], [dict(samples[0], server_workers=16), *samples[1:]],
                     [dict(samples[0], wall_seconds=3.001), *samples[1:]],
                     [dict(samples[0], wall_seconds=0.5), *samples[1:]],
                     [dict(samples[0], min_connection_samples=0), *samples[1:]])
        for changed in mutations:
            with self.subTest(change=changed[0]), self.assertRaises(ValueError):
                bench_scaling.report(changed, metadata)
        for change in ({"smoke": True}, {"protocol": "other"}, {"client_workers": 8},
                       {"workloads": []}, {"revision": "invalid"}, {"unavailable": {}}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                bench_scaling.report(samples, dict(metadata, **change))
        changed = copy.deepcopy(metadata)
        changed["cpu_masks"]["8"]["client"] = 0xF000
        with self.assertRaises(ValueError):
            bench_scaling.report(samples, changed)

    def test_throughput_tail_and_client_limit_warnings_are_independent(self):
        samples, metadata = evidence()
        samples[0].update(p99_us=1000, p999_us=1000, max_us=1000)
        for sample in samples:
            sample["client_cores"] = 3.95
        result = bench_scaling.report(samples, metadata)
        self.assertTrue(any(row["p99_noisy"] for row in result["rows"]))
        self.assertFalse(any(row["throughput_noisy"] for row in result["rows"]))
        self.assertTrue(all(row["client_busy"] for row in result["rows"]))
        self.assertIn("client busy", bench_scaling.compact_markdown(result, metadata, "https://example.test/evidence"))
        samples[0].update(samples=100000, roundtrips_per_second=100000 / 2.01)
        result = bench_scaling.report(samples, metadata)
        self.assertTrue(any(row["throughput_noisy"] for row in result["rows"]))

    def test_packaging_revalidates_raw_data_and_never_overwrites_evidence(self):
        samples, metadata = evidence()
        analysis = bench_scaling.report(samples, metadata)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, value in (("environment", metadata), ("samples", {"samples": samples}), ("analysis", analysis)):
                write_json(root / f"{name}.json", value)
            (root / "run.log").write_text("Complete synthetic run\n")
            (root / "unrelated.txt").write_text("Not evidence\n")
            args = SimpleNamespace(directory=root, output=root / "evidence.zip", evidence_url="https://example.test/evidence")
            originals = {path.name: path.read_bytes() for path in root.glob("*.json")}
            with redirect_stdout(io.StringIO()):
                bench_scaling.package(args)
            self.assertEqual(originals, {path.name: path.read_bytes() for path in root.glob("*.json")})
            with zipfile.ZipFile(args.output) as archive:
                self.assertEqual(set(archive.namelist()), {"environment.json", "samples.json", "analysis.json", "run.log", "summary.md"})
                self.assertEqual(json.loads(archive.read("samples.json"))["samples"], samples)
            with self.assertRaisesRegex(ValueError, "overwrite"):
                bench_scaling.package(args)
            args.output = root / "different.zip"
            analysis["rows"][0]["speedup"] = 999
            write_json(root / "analysis.json", analysis)
            with self.assertRaisesRegex(ValueError, "does not match"):
                bench_scaling.package(args)

    def test_diagnostic_packaging_keeps_overruns_without_promoting_a_failed_run(self):
        samples, metadata = evidence()
        samples[0].update(wall_seconds=5.8, roundtrips_per_second=samples[0]["samples"] / 5.8)
        with self.assertRaisesRegex(ValueError, "measurement window"):
            bench_scaling.report(samples, metadata)
        result = bench_scaling.report(samples, metadata, diagnostic=True)
        self.assertFalse(result["timing_valid"])
        self.assertEqual(len(result["window_failures"]), 1)
        self.assertEqual(len(result["rows"]), 48)
        self.assertEqual(sum(not pair["timing_valid"] for pair in result["comparisons"]), 2)
        text = bench_scaling.full_markdown(result, metadata, "https://example.test/evidence")
        self.assertIn("5.800000", text)
        self.assertIn("not a passed benchmark run", text)
        self.assertIn("timing failure; non-comparable", text)
        with self.assertRaises(ValueError):
            bench_scaling.report(samples[:-1], metadata, diagnostic=True)
        invalid = [dict(samples[0], server_workers=16), *samples[1:]]
        with self.assertRaises(ValueError):
            bench_scaling.report(invalid, metadata, diagnostic=True)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_json(root / "environment.json", metadata)
            write_json(root / "samples.json", {"samples": samples})
            (root / "run.log").write_text("Error: A sample missed its measurement window.\n")
            originals = {path.name: path.read_bytes() for path in root.glob("*.json")}
            args = SimpleNamespace(directory=root, output=root / "diagnostic.zip",
                                   evidence_url="https://example.test/evidence", diagnostic=True)
            with patch.object(bench_scaling, "capture", return_value="synthetic"), redirect_stdout(io.StringIO()):
                bench_scaling.package(args)
            self.assertEqual(originals, {path.name: path.read_bytes() for path in root.glob("*.json")})
            with zipfile.ZipFile(args.output) as archive:
                self.assertNotIn("analysis.json", archive.namelist())
                self.assertEqual(json.loads(archive.read("diagnostic-analysis.json")), result)
                self.assertEqual(json.loads(archive.read("samples.json"))["samples"], samples)
                self.assertIn("analysis-provenance.json", archive.namelist())

    def test_one_core_timing_failure_flags_all_affected_scaling_rows(self):
        samples, metadata = evidence()
        base = next(sample for sample in samples if sample["server_workers"] == 1 and sample["backend"] == "weave")
        base.update(wall_seconds=5.8, roundtrips_per_second=base["samples"] / 5.8)
        result = bench_scaling.report(samples, metadata, diagnostic=True)
        affected = [row for row in result["rows"] if row["workload"] == base["workload"] and row["backend"] == "weave"]
        self.assertEqual(len(affected), 4)
        self.assertTrue(all(row["window_failures"] for row in affected))


if __name__ == "__main__":
    unittest.main()
