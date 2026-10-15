"""Synthetic checks for paired benchmarking; never executes a timing workload."""

import importlib.util
from pathlib import Path
import unittest
from unittest import mock
from contextlib import contextmanager
from types import SimpleNamespace
import placement
import live


source = Path(__file__).resolve().parents[1] / "benchmarks" / "paired.py"
spec = importlib.util.spec_from_file_location("mysql_paired", source)
paired = importlib.util.module_from_spec(spec)
spec.loader.exec_module(paired)

HEADER = "workload,driver,workers,repetition,operations,seconds,ops_per_second,p99_us,cpu_percent,cycles_per_operation,max_us\n"


class PairedBenchmarks(unittest.TestCase):
    def test_accepts_one_verified_sample(self):
        row = paired.parse_sample(HEADER + "text,weave,1,1,7500,0.75,10000,150,50,0,200\n", "text", "weave")
        self.assertEqual(row["operations"], "7500")

    def test_rejects_missing_extra_or_wrong_samples(self):
        sample = "text,weave,1,1,7500,0.75,10000,150,50,0,200\n"
        cases = (
            HEADER, HEADER + sample + sample, HEADER + sample.replace("text", "prepared"),
            HEADER + sample.replace("weave", "native"), HEADER + sample.replace(",1,1,7500", ",4,1,7500"),
            HEADER + sample.replace(",1,1,7500", ",1,2,7500"), HEADER + sample.replace("7500", "0"),
        )
        for output in cases:
            with self.subTest(output=output), self.assertRaises(ValueError):
                paired.parse_sample(output, "text", "weave")

    def test_rejects_invalid_metrics(self):
        samples = (
            "text,weave,1,1,7500,0,10000,150,50,0,200\n",
            "text,weave,1,1,7500,0.75,nan,150,50,0,200\n",
            "text,weave,1,1,7500,0.75,10000,150,-50,0,200\n",
            "text,weave,1,1,7500,0.75,10000,250,50,0,200\n",
        )
        for sample in samples:
            with self.subTest(sample=sample), self.assertRaises(ValueError):
                paired.parse_sample(HEADER + sample, "text", "weave")

    def test_requires_four_runtime_workers(self):
        sample = "runtime4,native,1,1,7500,0.75,10000,150,50,0,200\n"
        with self.assertRaises(ValueError):
            paired.parse_sample(HEADER + sample, "runtime4", "native")

    def test_rotates_all_trial_positions_without_omissions(self):
        orders = [paired.trial_order(repetition) for repetition in range(1, 7)]
        self.assertEqual(len(set(orders)), 6)
        labels = {"before", "native", "after"}
        for order in orders:
            self.assertEqual(set(order), labels)
        for label in labels:
            for position in range(3):
                self.assertEqual(sum(order[position] == label for order in orders), 2)
        self.assertEqual(paired.trial_order(7), orders[0])


class CpuPlacement(unittest.TestCase):
    def test_cpu_lists_reject_invalid_or_repeated_ids(self):
        self.assertEqual(placement.parse_cpus("0,2,4,6"), frozenset({0, 2, 4, 6}))
        invalid = ("", "0,", "-1", "+1", "0,0", " 0", "a")
        for text in invalid:
            with self.subTest(text=text), self.assertRaises(ValueError):
                placement.parse_cpus(text)

    def test_requires_two_disjoint_available_sets(self):
        available = frozenset({0, 1, 2, 3})
        placement.validate(None, None, available)
        placement.validate(frozenset({0, 1}), frozenset({2, 3}), available)
        invalid = (
            (None, frozenset({0})), (frozenset({0}), None), (frozenset(), frozenset({1})),
            (frozenset({0}), frozenset({0, 1})), (frozenset({0}), frozenset({4})),
        )
        for client, server in invalid:
            with self.subTest(client=client, server=server), self.assertRaises(ValueError):
                placement.validate(client, server, available)

    def test_restores_placement_after_success_and_failure(self):
        original, selected = frozenset({0, 1, 2, 3}), frozenset({2, 3})
        for fail in (False, True):
            state = {"cpus": original}

            def assign(cpus):
                state["cpus"] = cpus

            with mock.patch.object(placement, "current_cpus", side_effect=lambda: state["cpus"]), \
                 mock.patch.object(placement, "set_cpus", side_effect=assign):
                try:
                    with placement.placed(selected):
                        self.assertEqual(state["cpus"], selected)
                        if fail:
                            raise RuntimeError("synthetic failure")
                except RuntimeError:
                    self.assertTrue(fail)
                else:
                    self.assertFalse(fail)
                self.assertEqual(state["cpus"], original)

    def test_rejected_os_placement_still_attempts_restoration(self):
        original, selected = frozenset({0, 1}), frozenset({1})
        with mock.patch.object(placement, "current_cpus", return_value=original), \
             mock.patch.object(placement, "set_cpus", side_effect=[OSError("synthetic denial"), None]) as setter:
            with self.assertRaises(OSError):
                with placement.placed(selected):
                    self.fail("denied placement must not start the workload")
            self.assertEqual(setter.call_args_list, [mock.call(selected), mock.call(original)])

    def test_default_fixture_does_not_touch_affinity(self):
        with mock.patch.object(placement, "current_cpus") as reader, \
             mock.patch.object(placement, "set_cpus") as setter:
            with placement.placed(None):
                pass
            reader.assert_not_called()
            setter.assert_not_called()

    def test_fixture_reaps_server_when_parent_restoration_fails(self):
        @contextmanager
        def rejected_restore(cpus):
            yield
            raise RuntimeError("synthetic restore failure")

        arguments = SimpleNamespace(
            server=Path("fixture/mysqld"), client=Path("fixture/mysql"), test=Path("fixture/test.py"),
            test_timeout=30, client_cpus=frozenset({0}), server_cpus=frozenset({1}),
        )
        process = mock.Mock()
        process.poll.return_value = None
        process.wait.return_value = 0
        with mock.patch.object(live.argparse.ArgumentParser, "parse_args", return_value=arguments), \
             mock.patch.object(live, "current_cpus", return_value=frozenset({0, 1})), \
             mock.patch.object(live.shutil, "which", return_value="openssl"), \
             mock.patch.object(live, "run"), mock.patch.object(live, "placed", rejected_restore), \
             mock.patch.object(live.subprocess, "Popen", return_value=process), \
             mock.patch.object(live.sys, "stderr"):
            with self.assertRaisesRegex(RuntimeError, "restore failure"):
                live.main()
        process.terminate.assert_called_once()
        process.wait.assert_called_once_with(timeout=10)


if __name__ == "__main__":
    unittest.main()
