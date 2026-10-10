"""Isolated client-certificate mode loading and precedence."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    base = {name: value for name, value in os.environ.items() if not name.upper().startswith("PG")}
    controls = []
    with tempfile.TemporaryDirectory(prefix="weave-certificate-mode-") as temporary:
        service = Path(temporary) / "service.conf"
        service.write_text("[off]\nuser=weave\nsslcertmode=disable\n[on]\nuser=weave\nsslcertmode=require\n"
                           "[bad]\nuser=weave\nsslcertmode=true\n")
        cases = [(None, "user=weave", "allow"), ("disable", "user=weave", "disable"),
                 ("allow", "user=weave", "allow"), ("require", "user=weave", "require"),
                 ("", "user=weave", "allow"), ("true", "user=weave", "invalid"),
                 ("require", "user=weave sslcertmode=disable", "disable"),
                 ("disable", "service=on", "require"), ("require", "service=off", "disable"),
                 (None, "service=bad", "invalid"), ("require", "service=off sslcertmode=allow", "allow")]
        for value, text, expected in cases:
            env = dict(base, PGSERVICEFILE=str(service))
            if value is not None:
                env["PGSSLCERTMODE"] = value
            run = subprocess.run([args.executable, "--load", expected, text], env=env,
                                 capture_output=True, text=True, timeout=20)
            event = dict(environment=value, input=text, expected=expected, returncode=run.returncode,
                         stdout=run.stdout, stderr=run.stderr)
            controls.append(event)
            if run.returncode or run.stderr or not run.stdout.startswith("Certificate loader: "):
                raise RuntimeError(json.dumps(event))
    if Path(temporary).exists():
        raise RuntimeError("Owned service fixture survived")
    print(json.dumps(dict(controls=controls, fixture_removed=True)), flush=True)


if __name__ == "__main__":
    main()
