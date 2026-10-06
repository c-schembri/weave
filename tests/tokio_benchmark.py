"""Validate the manual Tokio comparison's evidence with synthetic fixtures only."""

from contextlib import redirect_stdout
import copy
import io
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bench_tokio


def sample(workload, backend, repetition, factor=1.0):
    result = {metric: 100.0 for metric in bench_tokio.METRICS}
    result.update({"workload": workload, "backend": backend, "repetition": repetition,
                   "samples": 2000, "min_connection_samples": 1, "max_connection_samples": 20,
                   "connections": 1024, "bytes": 1024, "cpu_iterations": 0, "uneven": False,
                   "wall_seconds": 2.0 / factor, "roundtrips_per_second": 1000.0 * factor,
                   "client_cycles_per_op": 100, "server_cores": 2, "p50_us": 10, "p95_us": 20,
                   "p99_us": 30, "p999_us": 40, "max_us": 50})
    return result


class EvidenceTests(unittest.TestCase):
    def test_complete_paired_matrix_and_obvious_slowdown(self):
        workload = bench_tokio.WORKLOADS[:1]
        rows = [sample(workload[0][0], backend, repetition, 0.5 if backend == "weave" else 1.0)
                for repetition in range(7) for backend in bench_tokio.BACKENDS]
        report = bench_tokio.analyze(rows, workload, bench_tokio.BACKENDS, 7)
        self.assertEqual(len(report["rows"]), 3)
        weave = report["rows"][0]
        self.assertEqual(weave["throughput_vs_tokio_pct"], -50)
        self.assertEqual(weave["throughput_ratio_ci90"], (0.5, 0.5))
        with redirect_stdout(io.StringIO()) as output:
            bench_tokio.print_report(report)
        self.assertIn("-50.0%", output.getvalue())
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            bench_tokio.analyze(rows[:-1], workload, bench_tokio.BACKENDS, 7)
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            bench_tokio.analyze(rows + [rows[0]], workload, bench_tokio.BACKENDS, 7)
        with self.assertRaisesRegex(ValueError, "Unmatched"):
            bench_tokio.analyze([row for row in rows if row["backend"] != "asio"], workload, bench_tokio.BACKENDS, 7)
        wrong = copy.deepcopy(rows)
        wrong[0]["bytes"] = 65536
        with self.assertRaisesRegex(ValueError, "Mismatched workload"):
            bench_tokio.analyze(wrong, workload, bench_tokio.BACKENDS, 7)

    def test_stalled_connections_invalid_counts_and_nan_are_rejected(self):
        baseline = sample("fixture", "tokio", 0)
        cases = ({"min_connection_samples": 0}, {"samples": 0}, {"samples": 2000.0},
                 {"p99_us": float("nan")}, {"roundtrips_per_second": 2000},
                 {"p999_us": 20}, {"server_cores": 8}, {"connections": 0}, {"bytes": 0},
                 {"cpu_iterations": -1}, {"uneven": 1}, {"repetition": True},
                 {"min_connection_samples": 100}, {"server_cycles_per_op": 0})
        for change in cases:
            with self.subTest(change=change), self.assertRaises(ValueError):
                value = copy.deepcopy(baseline)
                value.update(change)
                bench_tokio.validate_sample(value)

    def test_zero_cpu_time_is_not_silently_converted_from_cycles(self):
        value = sample("fixture", "tokio", 0)
        value.update(server_cores=0, server_cpu_us_per_op=0, client_cores=0)
        bench_tokio.validate_sample(value)
        self.assertEqual(value["server_cores"], 0)
        self.assertGreater(value["server_cycles_per_op"], 0)


if __name__ == "__main__":
    unittest.main()
