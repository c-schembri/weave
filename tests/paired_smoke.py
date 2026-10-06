"""Manual native positive-control smoke; excluded from automatic CI."""

import argparse
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from support.common import cli, read_json, require, run
from support.paired_stats import summary


def validate_smoke(data):
    require(data["context"]["injected_work"] == "200000" and len(data["benchmarks"]) == 36, "Incorrect smoke plan.")
    for sequence, sample in enumerate(data["benchmarks"]):
        require(sample["sequence"] == sequence and not sample.get("error_occurred") and sample["min_connection_samples"] >= 1,
                "Invalid or incomplete native sample.")
    for fixture in (0, 6):
        case = [sample for sample in data["benchmarks"] if sample["fixture_id"] == fixture and sample["phase"] == 1]
        a, b = [summary([sample["client_cycles_per_op"] for sample in case if sample["label"] == label])[0] for label in (0, 1)]
        require(b / a >= 1.5, "Harness did not detect deliberately injected CPU cost.")
        print(f"Positive control fixture {fixture}: {b / a:.2f}x client cycles/op")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="weave-paired-") as directory:
        path = Path(directory) / "paired.json"
        run([args.binary.resolve(strict=True), "--weave_paired_smoke", "--weave_paired_positive_control", f"--weave_paired_out={path}"], timeout=50)
        validate_smoke(read_json(path))


if __name__ == "__main__":
    sys.exit(cli(main))
