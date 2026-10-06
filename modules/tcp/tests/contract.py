"""Check execution scope and TCP binding contracts in isolated processes."""

import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    modes = ("discard", "listen", "connect", "detach", "wrong-accept", "wrong-connect", "wrong-read",
             "wrong-read-exactly", "wrong-write")
    for mode in modes:
        result = subprocess.run([args.executable, mode], capture_output=True, timeout=10)
        if mode == "discard":
            if result.returncode != 0:
                raise AssertionError(f"Unstarted setup touched the Context: {result.stderr!r}")
        elif result.returncode == 0 or b"Weave contract:" not in result.stderr:
            raise AssertionError(f"{mode} did not enforce its Context contract: {result.returncode}, {result.stderr!r}")
        if mode.startswith("wrong-") and b"incompatible execution Context" not in result.stderr:
            raise AssertionError(f"{mode} failed before the compatibility check: {result.stderr!r}")


if __name__ == "__main__":
    main()
