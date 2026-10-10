"""Opt-in independent libpq password control on a disposable PostgreSQL deployment."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from password_hashes import verify


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--server-bin", required=True)
    parser.add_argument("--windows-client", action="store_true")
    args = parser.parse_args()
    command = [sys.executable, str(Path(__file__).with_name("server.py")), "--executable", args.executable,
               "--server-bin", args.server_bin]
    if args.windows_client:
        command.append("--windows-client")
    result = subprocess.run(command, capture_output=True, text=True, timeout=150)
    if result.returncode or "libpq password live controls passed:" not in result.stdout:
        raise RuntimeError(f"libpq password controls failed ({result.returncode}): {result.stdout}\n{result.stderr}")
    encoded = "\n".join(line.removeprefix("Verifier: ") for line in result.stdout.splitlines()
                        if line.startswith("Verifier: "))
    print(result.stdout, end="", flush=True)
    print(json.dumps(verify(encoded, baseline=True)), flush=True)


if __name__ == "__main__":
    main()
