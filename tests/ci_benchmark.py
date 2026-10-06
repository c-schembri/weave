"""Synthetic checks for the locked CI protocol, reporting, and trusted publication."""

from collections import Counter
from contextlib import redirect_stdout
import copy
import io
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bench_ci
from bench_tokio import METRICS


def evidence():
    metadata = {"protocol": bench_ci.PROTOCOL, "repetitions": bench_ci.REPETITIONS,
                "duration_ms": bench_ci.DURATION_MS, "warmup_ms": bench_ci.WARMUP_MS, "seed": bench_ci.SEED,
                "cpu_masks": {"server": 5, "client": 10}, "workers": 2, "client_workers": 2,
                "revision": "a" * 40, "worktree": [], "timestamp": "2026-10-06T12:00:00+11:00",
                "repository": "c-schembri/weave", "run_id": "123", "elapsed_seconds": 115.0}
    samples = []
    for repetition, workload, backend in bench_ci.schedule():
        name, connections, size, work, uneven = workload
        sample = {metric: 1.0 for metric in METRICS}
        count = 24000 if backend == "weave" else 20000
        sample.update({"workload": name, "backend": backend, "repetition": repetition,
                       "connections": connections, "bytes": size, "cpu_iterations": work, "uneven": uneven,
                       "server_workers": 2, "samples": count, "min_connection_samples": 1,
                       "max_connection_samples": 4000, "wall_seconds": 1.01,
                       "roundtrips_per_second": count / 1.01, "client_cycles_per_op": 100,
                       "p50_us": 10, "p95_us": 20, "p99_us": 30, "p999_us": 40, "max_us": 100})
        samples.append(sample)
    return samples, metadata


class ProtocolTests(unittest.TestCase):
    def test_schedule_is_fixed_complete_and_matched(self):
        cases = list(bench_ci.schedule())
        self.assertEqual(cases, list(bench_ci.schedule()))
        self.assertEqual(len(cases), 84)
        self.assertEqual(len(set(cases)), 84)
        for repetition in range(7):
            counts = Counter((workload[0], backend) for index, workload, backend in cases if index == repetition)
            self.assertEqual(set(counts.values()), {1})
            self.assertEqual(len(counts), 12)
        self.assertEqual(bench_ci.TIMEOUT_MS, 300000)
        self.assertEqual(bench_ci.DURATION_MS, 1000)
        self.assertEqual(bench_ci.WARMUP_MS, 250)

    def test_cpu_budgets_reject_overlapping_oversubscribed_or_invalid_masks(self):
        self.assertEqual(bench_ci.validate_masks({"server": 1, "client": 2}), 1)
        self.assertEqual(bench_ci.validate_masks({"server": 5, "client": 10}), 2)
        invalid = ({"server": 1, "client": 1}, {"server": 0, "client": 2}, {"server": True, "client": 2},
                   {"server": 7, "client": 56}, {"server": 5, "client": 8}, {"server": -1, "client": 2},
                   {"server": 2**64, "client": 1}, {"server": 1, "client": 2, "extra": 4})
        for masks in invalid:
            with self.subTest(masks=masks), self.assertRaises(ValueError):
                bench_ci.validate_masks(masks)

    def test_complete_report_has_real_units_pairs_and_no_variation(self):
        samples, metadata = evidence()
        result = bench_ci.report(samples, metadata)
        self.assertEqual(len(result["rows"]), 12)
        self.assertEqual(len(result["paired"]), 8)
        self.assertFalse(result["noisy"])
        for pair in result["paired"]:
            self.assertAlmostEqual(pair["ratio"], 1.2)
            self.assertEqual(pair["ci90"], [pair["ratio"], pair["ratio"]])
        markdown = bench_ci.full_markdown(result, metadata)
        self.assertIn("90% whole-block", markdown)
        self.assertIn("Server CPU us/op", markdown)
        self.assertIn("p99.9 ms", markdown)
        self.assertIn("300s", markdown)
        self.assertIn("actions/runs/123", markdown)

    def test_noisy_measurements_are_retained_not_filtered(self):
        samples, metadata = evidence()
        samples[0].update(samples=200000, roundtrips_per_second=200000 / 1.01)
        result = bench_ci.report(samples, metadata)
        self.assertTrue(result["noisy"])
        first = next(row for row in result["rows"] if row["backend"] == "weave" and row["workload"] == "64-small")
        self.assertEqual(first["metrics"]["roundtrips_per_second"]["max"], 200000 / 1.01)
        self.assertIn("inconclusive", bench_ci.compact_markdown(result, metadata))

    def test_tail_variance_and_client_limits_are_not_hidden_by_stable_throughput(self):
        samples, metadata = evidence()
        samples[0].update(p99_us=1000, p999_us=1000, max_us=1000)
        for sample in samples:
            sample["client_cores"] = 1.95
        result = bench_ci.report(samples, metadata)
        self.assertTrue(result["noisy"])
        self.assertTrue(all(row["client_busy"] for row in result["rows"]))
        self.assertIn("client busy", bench_ci.compact_markdown(result, metadata))

    def test_partial_duplicates_parameter_changes_and_stalled_windows_fail(self):
        samples, metadata = evidence()
        mutations = (samples[:-1], samples + [samples[0]],
                     [dict(samples[0], bytes=65536), *samples[1:]],
                     [dict(samples[0], wall_seconds=2.001), *samples[1:]],
                     [dict(samples[0], min_connection_samples=0), *samples[1:]],
                     [dict(samples[0], server_workers=1), *samples[1:]])
        for changed in mutations:
            with self.subTest(change=changed[0]), self.assertRaises(ValueError):
                bench_ci.report(changed, metadata)
        for changed in ({"warmup_ms": 0}, {"duration_ms": 100}, {"repetitions": 1},
                        {"seed": 1}, {"workers": 1}, {"revision": "not a revision"}):
            with self.subTest(metadata=changed), self.assertRaises(ValueError):
                bench_ci.report(samples, dict(metadata, **changed))

    def test_readme_update_preserves_everything_outside_the_markers(self):
        original = f"Intro\n{bench_ci.BEGIN}\nold\n{bench_ci.END}\nAPI\n"
        expected = f"Intro\n{bench_ci.BEGIN}\nnew\n{bench_ci.END}\nAPI\n"
        self.assertEqual(bench_ci.replace_results(original, "new\n"), expected)
        malformed = ("no markers", original + original, original.replace(bench_ci.END, ""))
        for text in malformed:
            with self.subTest(text=text), self.assertRaises(ValueError):
                bench_ci.replace_results(text, "new\n")


class PublicationTests(unittest.TestCase):
    def setUp(self):
        samples, metadata = evidence()
        self.publication = {"samples": samples, "metadata": metadata}
        self.environment = {"GITHUB_REPOSITORY": "c-schembri/weave", "GITHUB_REF": "refs/heads/main",
                            "GITHUB_EVENT_NAME": "push", "GITHUB_SHA": metadata["revision"], "GITHUB_RUN_ID": "123"}
        self.args = SimpleNamespace(report=Path("synthetic-publication.json"))

    def test_forks_pull_requests_other_refs_and_local_invocations_cannot_publish(self):
        changes = ({"GITHUB_REPOSITORY": "fork/weave"}, {"GITHUB_REF": "refs/pull/1/merge"},
                   {"GITHUB_EVENT_NAME": "pull_request"}, {"GITHUB_EVENT_NAME": "pull_request_target"},
                   {"GITHUB_REF": "refs/heads/feature"})
        for change in changes:
            with self.subTest(change=change), patch.dict(os.environ, dict(self.environment, **change), clear=True), \
                    patch.object(bench_ci, "run") as run, self.assertRaisesRegex(ValueError, "trusted main"):
                bench_ci.publish(self.args)
            run.assert_not_called()
        with patch.dict(os.environ, {}, clear=True), self.assertRaisesRegex(ValueError, "trusted main"):
            bench_ci.publish(self.args)

    def test_dirty_mismatched_source_and_wrong_run_evidence_cannot_publish(self):
        changes = ({"worktree": ["M source.cpp"]}, {"revision": "b" * 40}, {"run_id": "456"})
        for change in changes:
            publication = copy.deepcopy(self.publication)
            publication["metadata"].update(change)
            with self.subTest(change=change), patch.dict(os.environ, self.environment, clear=True), \
                    patch.object(bench_ci, "read_json", return_value=publication), patch.object(bench_ci, "run") as run, \
                    self.assertRaisesRegex(ValueError, "clean main"):
                bench_ci.publish(self.args)
            run.assert_not_called()

    def test_advanced_main_is_skipped_without_writes_or_git_mutations(self):
        with patch.dict(os.environ, self.environment, clear=True), \
                patch.object(bench_ci, "read_json", return_value=self.publication), \
                patch.object(bench_ci, "capture", side_effect=["", "b" * 40]), \
                patch.object(bench_ci, "run") as run, redirect_stdout(io.StringIO()) as output:
            bench_ci.publish(self.args)
        run.assert_not_called()
        self.assertIn("stale", output.getvalue())

    def test_current_clean_main_updates_only_readme_and_never_force_pushes(self):
        with tempfile.TemporaryDirectory(prefix="weave-publish-test-") as temporary:
            root = Path(temporary)
            readme = root / "README.md"
            readme.write_text(f"Intro\n{bench_ci.BEGIN}\npending\n{bench_ci.END}\nAPI\n", encoding="utf-8")
            with patch.dict(os.environ, self.environment, clear=True), patch.object(bench_ci, "ROOT", root), \
                    patch.object(bench_ci, "read_json", return_value=self.publication), \
                    patch.object(bench_ci, "capture", side_effect=["", "a" * 40]), patch.object(bench_ci, "run") as run:
                bench_ci.publish(self.args)
            updated = readme.read_text(encoding="utf-8")
            self.assertTrue(updated.startswith("Intro\n"))
            self.assertTrue(updated.endswith("\nAPI\n"))
            self.assertIn("Median round trips/second", updated)
            self.assertEqual(run.call_args_list[-1].args[0], ["git", "push", "origin", "HEAD:main"])
            self.assertIn(["git", "add", "--", "README.md"], [call.args[0] for call in run.call_args_list])


if __name__ == "__main__":
    unittest.main()
