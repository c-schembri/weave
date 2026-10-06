import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from support.gate_process import run_owned


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True)
    parser.add_argument("--directory", type=Path)
    parser.add_argument("arguments", nargs="*")
    args = parser.parse_args()
    if args.mode == "exit":
        print("expected exit", flush=True)
        return 7
    if args.mode == "args":
        print(json.dumps(args.arguments), flush=True)
        return 0
    if args.mode == "supervisor":
        return run_owned(sys.executable, [__file__, "--mode", "hang", "--directory", str(args.directory)],
                         args.directory, args.directory / "nested.log", 60000)
    name = "child" if args.mode == "child" else "parent"
    (args.directory / f"{name}.pid").write_text(str(os.getpid()), encoding="ascii")
    if args.mode != "child":
        subprocess.Popen([sys.executable, __file__, "--mode", "child", "--directory", str(args.directory)],
                         creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        deadline = time.monotonic() + 10
        while not (args.directory / "child.pid").exists():
            if time.monotonic() >= deadline:
                return 8
            time.sleep(0.01)
        if args.mode == "orphan":
            return 0
    time.sleep(60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
