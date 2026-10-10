"""Owning lifecycle controls with the module's owned notification/COPY peer."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location("peer", Path(__file__).with_name("notifications.py"))
peer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peer)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["context", "reject", "blocking", "reentrant"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    report = {"kind": "owning lifecycle correctness", "complete": False, "results": []}
    for mode in modes:
        result = peer.run(args.executable, mode)
        result["expected_contract_failure"] = mode == "reentrant"
        report["results"].append(result)
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        if mode == "reentrant":
            valid = (result["returncode"] != 0 and result["connections"] == 1
                     and not result["peer_errors"]
                     and result["stdout"].strip() == "Observed result reentrancy reached"
                     and re.fullmatch(r"Weave contract: .*events\.cpp:\d+\s*", result["stderr"]))
            if not valid:
                raise SystemExit(1)
            continue
        expected_connections = {"context": 4, "reject": 1, "blocking": 2}.get(mode, 128)
        if (result["returncode"] or result["peer_errors"] or result["stderr"]
                or result["connections"] != expected_connections
                or not re.search(r"Selective-copy controls passed: [1-9]\d* checks", result["stdout"])):
            raise SystemExit(1)
    report["complete"] = True
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
