"""Validate the manual Tokio comparison's evidence with synthetic fixtures only."""

from contextlib import redirect_stdout
import copy
import io
import json
from pathlib import Path
import sys
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

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
    def test_ci_worker_and_timed_warmup_options_reach_the_native_processes(self):
        for workers in (1, 2):
            with self.subTest(workers=workers), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                server, client = Mock(), Mock()
                server.line.return_value = json.dumps({"workers": workers, "port": 8080})
                client.line.side_effect = ["READY", "MEASURED", json.dumps(sample("1024-small", "weave", 0)), "CLOSED"]
                server.process.poll.return_value = None
                client.process.wait.return_value = 0
                args = SimpleNamespace(output_directory=root, server_binary="server", tokio_binary="tokio",
                                       load_binary="load", duration_ms=1000, client_workers=workers,
                                       server_workers=workers, warmup_ms=250, smoke=False)
                before = {"cpu_seconds": 0, "cycles": 0}
                after = {"cpu_seconds": 1, "cycles": 200000, "private_bytes": 4096, "working_set_bytes": 4096}
                with patch.object(bench_tokio, "Child", side_effect=[server, client]) as child, \
                        patch.object(bench_tokio, "counters", side_effect=[before, after]):
                    result = bench_tokio.measure(args, "weave", bench_tokio.WORKLOADS[0], 0, {"server": 5, "client": 10})
                self.assertEqual(result["server_workers"], workers)
                self.assertEqual(child.call_args_list[0].args[0][-1], workers)
                self.assertEqual(child.call_args_list[1].args[0][-2:], [workers, 250])

    def test_socket_cleanup_acknowledgment_and_process_exit_have_distinct_limits(self):
        for failure_phase in (None, "client socket cleanup", "client process exit"):
            with self.subTest(failure_phase=failure_phase), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                log = root / "client.log"
                log.write_text("Closing 4096 client sockets\n", encoding="utf-8")
                server, client = Mock(), Mock()
                server.log.name = client.log.name = str(log)
                server.process.poll.return_value = client.process.poll.return_value = None
                server.process.pid, client.process.pid = 123, 456
                server.line.return_value = json.dumps({"workers": 4, "port": 8080})
                last_line = TimeoutError("Close deadline") if failure_phase == "client socket cleanup" else "CLOSED"
                client.line.side_effect = ["READY", "MEASURED", json.dumps(sample("1024-small", "weave", 0)), last_line]
                if failure_phase == "client process exit":
                    client.process.wait.side_effect = subprocess.TimeoutExpired("load", 5)
                else:
                    client.process.wait.return_value = 0
                args = SimpleNamespace(output_directory=root, server_binary="server", tokio_binary="tokio",
                                       load_binary="load", duration_ms=1000, client_workers=8, smoke=False)
                before = {"cpu_seconds": 0, "cycles": 0}
                after = {"cpu_seconds": 4, "cycles": 200000, "private_bytes": 4096, "working_set_bytes": 4096}
                with patch.object(bench_tokio, "Child", side_effect=[server, client]), \
                        patch.object(bench_tokio, "counters", side_effect=[before, after]):
                    if failure_phase:
                        with self.assertRaisesRegex(ValueError, failure_phase):
                            bench_tokio.measure(args, "weave", bench_tokio.WORKLOADS[0], 0, {"server": 15, "client": 240})
                        failure = json.loads((root / "1024-small-00-weave.failure.json").read_text(encoding="utf-8"))
                        self.assertEqual(failure["phase"], failure_phase)
                    else:
                        result = bench_tokio.measure(args, "weave", bench_tokio.WORKLOADS[0], 0, {"server": 15, "client": 240})
                        self.assertEqual(result["samples"], 2000)
                self.assertEqual(client.line.call_args.kwargs, {"timeout": 25})
                if failure_phase != "client socket cleanup":
                    client.process.wait.assert_called_once_with(timeout=5)
                else:
                    client.process.wait.assert_not_called()
                server.close.assert_called_once()
                client.close.assert_called_once()

    def test_server_startup_failure_is_recorded_without_starting_a_client(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "server.log"
            log.write_text("Listener setup failed\n", encoding="utf-8")
            server = Mock()
            server.log.name = str(log)
            server.process.pid = 123
            server.process.poll.return_value = 1
            server.line.side_effect = ValueError("Process ended prematurely")
            args = SimpleNamespace(output_directory=root, server_binary="server", tokio_binary="tokio")
            with patch.object(bench_tokio, "Child", return_value=server) as child, \
                    self.assertRaisesRegex(ValueError, "server startup failed"):
                bench_tokio.measure(args, "weave", bench_tokio.WORKLOADS[0], 0, {"server": 15})
            child.assert_called_once()
            server.close.assert_called_once()
            failure = json.loads((root / "1024-small-00-weave.failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["phase"], "server startup")
            self.assertEqual(set(failure["processes"]), {"server"})
            self.assertEqual(failure["processes"]["server"]["exit_code"], 1)
            self.assertIn("Listener setup failed", failure["processes"]["server"]["stderr_tail"])

    def test_failed_warmup_retains_native_diagnostics_and_cleans_both_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            server_log, client_log = root / "server.log", root / "client.log"
            server_log.write_text("Client failed: code=10054\n", encoding="utf-8")
            client_log.write_text("Connection 19: stage=warmup_read warmup=3/8\n", encoding="utf-8")
            server, client = Mock(), Mock()
            server.log.name, client.log.name = str(server_log), str(client_log)
            server.process.pid, client.process.pid = 123, 456
            server.process.poll.return_value = None
            client.process.poll.return_value = 2
            server.line.return_value = json.dumps({"workers": 4, "port": 8080})
            client.line.side_effect = ValueError("Process ended prematurely")
            args = SimpleNamespace(output_directory=root, server_binary="server", tokio_binary="tokio",
                                   load_binary="load", duration_ms=1000, client_workers=8, smoke=False)
            with patch.object(bench_tokio, "Child", side_effect=[server, client]), \
                    patch.object(bench_tokio, "counters") as counters, \
                    self.assertRaisesRegex(ValueError, "client setup/warmup failed"):
                bench_tokio.measure(args, "weave", bench_tokio.WORKLOADS[0], 6, {"server": 15, "client": 240})
            failure = json.loads((root / "1024-small-06-weave.failure.json").read_text(encoding="utf-8"))
            self.assertEqual(failure["workload"], "1024-small")
            self.assertEqual(failure["repetition"], 6)
            self.assertEqual(failure["phase"], "client setup/warmup")
            self.assertIsNone(failure["processes"]["server"]["exit_code"])
            self.assertEqual(failure["processes"]["client"]["exit_code"], 2)
            self.assertIn("code=10054", failure["processes"]["server"]["stderr_tail"])
            self.assertIn("warmup=3/8", failure["processes"]["client"]["stderr_tail"])
            counters.assert_not_called()
            server.close.assert_called_once()
            client.close.assert_called_once()
            self.assertFalse((root / "analysis.json").exists())

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
        missing_baseline = [row for row in rows if not (row["backend"] == "tokio" and row["repetition"] == 6)]
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            bench_tokio.analyze(missing_baseline, workload, bench_tokio.BACKENDS, 7)
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
