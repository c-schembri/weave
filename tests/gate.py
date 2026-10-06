"""Synthetic gate decisions and read-only analysis checks, not benchmarks."""

import copy
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from check_concurrent_gate import check_gate
from support.common import ROOT, read_json, write_json


def fixture_environment():
    return {
        "profile": "weave-task-policy-5m-v1", "isolate_cpus": True, "duration_ms": 250, "repetitions": 7,
        "cpu": [{"Name": "Fixture CPU", "NumberOfCores": 12, "NumberOfLogicalProcessors": 24}],
        "os": {"Version": "10.0", "BuildNumber": "fixture"}, "power_scheme": "fixture power",
        "limits": {"throughput_regression_pct": 5, "cpu_cycles_per_op_regression_pct": 5, "p99_regression_pct": 10},
    }


def fixture_data(mode="pass"):
    samples = []
    workloads = ((64, 4, 1024, 0), (1024, 1, 1024, 0), (1024, 4, 1024, 0),
                 (1024, 8, 1024, 0), (256, 4, 65536, 0), (1024, 4, 1024, 512))
    for fixture in range(12):
        connections, workers, size, cpu = workloads[fixture % 6]
        scheduler = "Affine" if fixture < 6 else "Stealing"
        plan = []
        for block in range(7):
            for order in range(2):
                phase = (block + fixture + order) % 2
                for slot in range(4):
                    label = int(slot in (1, 2)) ^ ((block + fixture + phase) % 2)
                    plan.append((phase, block, slot, label))
        plan.extend((2, block, 0, 2) for block in range(7))
        for phase, block, slot, label in plan:
            scale = math.exp(0.08 * slot + 0.03 * block) if mode == "drift" else 1
            rate, cycles, tail = 100000.0 * scale, 1000.0 * scale, 100.0 * scale
            if mode == "historical_cycles":
                cycles *= 1.20
            if mode == "historical_noise":
                cycles *= 1.10 if block % 2 else 0.99
            if phase == label == 1:
                if mode == "throughput":
                    rate *= 0.9
                if mode == "cycles":
                    cycles *= 1.10
                if mode == "tail":
                    tail *= 1.20
                if mode == "noisy":
                    rate *= 0.90 if block % 2 else 1.02
            if phase == 0 and label == 1:
                if mode == "aa_bias":
                    rate *= 0.8
                if mode == "aa_improves":
                    rate *= 1.2
            count = round(rate * 0.25)
            samples.append({
                "run_name": f"Paired{scheduler}/connections:{connections}/workers:{workers}/bytes:{size}/cpu:{cpu}",
                "run_type": "iteration", "repetitions": 7, "repetition_index": block,
                "fixture_id": fixture, "phase": phase, "block": block, "slot": slot, "label": label,
                "implementation": 2 if phase == 2 else int(phase == label == 1), "sequence": len(samples),
                "samples": count, "min_connection_samples": 1, "max_connection_samples": count,
                "wall_seconds": 0.25, "roundtrips_per_second": count / 0.25, "client_cycles_per_op": cycles,
                "peer_cycles_per_op": 1000, "client_cpu_us_per_op": 10, "client_cores": 1, "peer_cores": 1,
                "p50_us": 25, "p95_us": 50, "p99_us": tail, "p999_us": tail * 2, "max_us": tail * 3,
            })
    return {"context": {
        "protocol": "weave-task-policy-5m-v1", "smoke": "false", "injected_work": "0", "blocks": "7",
        "library_build_type": "release", "duration_ms": "250", "weave_warmup_windows": "4", "asio_warmup_windows": "2",
        "client_affinity_mask": "255", "peer_affinity_mask": "3840",
    }, "benchmarks": samples}


class GateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="weave-gate-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = fixture_environment()

    def check(self, expected, data=None, *, baseline=None):
        write_json(self.root / "environment.json", self.environment)
        write_json(self.root / "paired.json", data if data is not None else fixture_data())
        result = check_gate(self.root, baseline)
        self.assertEqual(result["decision"], expected, result.get("error"))
        if expected != "invalid":
            self.assertEqual(len(result["checks"]), 72)
            self.assertEqual(len(result["rows"]), 12)
        return result

    def test_decisions(self):
        for mode, expected in (("pass", "pass"), ("drift", "pass"), ("throughput", "regression"),
                               ("cycles", "regression"), ("tail", "regression"), ("noisy", "inconclusive"),
                               ("aa_bias", "measurement_unreliable"), ("aa_improves", "measurement_unreliable")):
            with self.subTest(mode=mode):
                self.check(expected, fixture_data(mode))

    def test_historical(self):
        baseline = self.root / "baseline"
        baseline.mkdir()
        write_json(baseline / "environment.json", self.environment)
        write_json(baseline / "paired.json", fixture_data())
        for mode, expected in (("pass", "pass"), ("historical_cycles", "regression"), ("historical_noise", "inconclusive")):
            with self.subTest(mode=mode):
                result = self.check(expected, fixture_data(mode), baseline=baseline)
                self.assertEqual(len(result["historical_checks"]), 36)
        legacy_environment = copy.deepcopy(self.environment)
        legacy_environment["cpu"][0]["Name"] += "   "
        write_json(baseline / "environment.json", legacy_environment)
        self.check("pass", baseline=baseline)
        legacy_environment["cpu"][0]["Name"] = "Different CPU"
        write_json(baseline / "environment.json", legacy_environment)
        self.check("invalid", baseline=baseline)

    def test_malformed_windows(self):
        invalid_fields = (("sequence", -1), ("implementation", 2), ("client_cycles_per_op", None), ("p99_us", 0),
                          ("min_connection_samples", 0), ("error_occurred", True), ("samples", 1), ("wall_seconds", 0.1),
                          ("p50_us", 200), ("roundtrips_per_second", 1), ("client_cycles_per_op", "1000"),
                          ("client_cores", True))
        for field, value in invalid_fields:
            with self.subTest(field=field, value=value):
                data = fixture_data()
                data["benchmarks"][0][field] = value
                self.check("invalid", data)
        for samples in (fixture_data()["benchmarks"][1:], fixture_data()["benchmarks"] + [{}]):
            data = fixture_data()
            data["benchmarks"] = samples
            self.check("invalid", data)
        data = fixture_data()
        del data["benchmarks"][0]["client_cycles_per_op"]
        self.check("invalid", data)
        data = fixture_data()
        data["benchmarks"][1]["sequence"] = 0
        self.check("invalid", data)

    def test_placement_and_plan(self):
        self.environment["isolate_cpus"] = False
        self.check("invalid")
        data = fixture_data()
        data["context"].update(client_affinity_mask="0", peer_affinity_mask="0")
        self.check("pass", data)
        for key, value in (("smoke", "true"), ("injected_work", "1"), ("blocks", "6"),
                           ("duration_ms", "100"), ("library_build_type", "debug"), ("client_affinity_mask", "-1")):
            with self.subTest(key=key):
                invalid = copy.deepcopy(data)
                invalid["context"][key] = value
                self.check("invalid", invalid)

    def test_cli_failure_and_json_encodings(self):
        write_json(self.root / "environment.json", self.environment)
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check_concurrent_gate.py"), "--directory", str(self.root)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 3)
        self.assertEqual(read_json(self.root / "gate.json")["decision"], "invalid")
        for encoding in ("utf-8-sig", "utf-16"):
            path = self.root / "encoded.json"
            path.write_text('{"value": 1}', encoding=encoding)
            self.assertEqual(read_json(path), {"value": 1})

    def test_read_only_analysis_preserves_decisions_intervals_and_artifacts(self):
        baseline = self.root / "baseline"
        baseline.mkdir()
        write_json(baseline / "environment.json", self.environment)
        write_json(baseline / "paired.json", fixture_data("drift"))
        expected = self.check("inconclusive", fixture_data("drift"), baseline=baseline)
        artifacts = {path: path.read_bytes() for path in self.root.rglob("*.json")}
        result = check_gate(self.root, baseline, publish=False)
        self.assertEqual(result["decision"], expected["decision"], result.get("error"))
        for key in ("checks", "historical_checks"):
            self.assertEqual(len(result[key]), len(expected[key]))
            for before, after in zip(expected[key], result[key]):
                self.assertEqual(before["status"], after["status"])
                for a, b in zip(before["ratio_ci90"], after["ratio_ci90"]):
                    self.assertAlmostEqual(a, b, places=12)
        self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*.json")}, artifacts)


if __name__ == "__main__":
    unittest.main()
