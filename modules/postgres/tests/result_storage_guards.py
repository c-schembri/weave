import argparse
import os
from pathlib import Path
import re
import signal
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    normal = subprocess.run([str(executable), "valid"], capture_output=True, text=True, timeout=15)
    assert normal.returncode == 0 and normal.stderr == "", normal
    assert normal.stdout.endswith("guard valid passed\n"), normal
    scenarios = [
        "pool-failure", "block-failure", "copy-failure", "missing-allocate", "missing-deallocate",
        "count-overflow", "size-overflow", "invalid-alignment", "zero-alignment", "double-free"
    ]
    for scenario in scenarios:
        result = subprocess.run([str(executable), scenario], capture_output=True, text=True, timeout=15)
        expected = {3, 0x40000015, 0xc0000409} if os.name == "nt" else {-signal.SIGABRT}
        assert result.returncode in expected, (scenario, result)
        assert result.stdout.startswith(f"guard {scenario} reached\n"), (scenario, result)
        assert re.search(r"Weave contract: .*result_storage\.hpp:\d+", result.stderr), (scenario, result)
        assert "incorrectly returned" not in result.stdout
        assert "AddressSanitizer" not in result.stderr and "LeakSanitizer" not in result.stderr, (scenario, result)
        requests = re.findall(r"^request (\d+)$", result.stdout, re.MULTILINE)
        if scenario == "copy-failure":
            armed = re.search(r"^armed (\d+)$", result.stdout, re.MULTILINE)
            assert armed and int(requests[-1]) == int(armed[1]) + 1, (scenario, result)
        else:
            expected_count = 0 if scenario.startswith("missing-") else 2 if scenario in ("block-failure", "double-free") else 1
            assert len(requests) == expected_count, (scenario, result)
        print("guard", scenario, "passed", flush=True)
    print("Result storage guards: 10 expected contract failures and one valid control", flush=True)


if __name__ == "__main__":
    main()
