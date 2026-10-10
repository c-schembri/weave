"""Isolated environment/service precedence and invalid SNI controls."""

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
    with tempfile.TemporaryDirectory(prefix="weave-sni-configuration-") as temporary:
        service = Path(temporary) / "service.conf"
        service.write_text("[off]\nuser=weave\nsslsni=0\n[on]\nuser=weave\nsslsni=1\n[bad]\nuser=weave\nsslsni=true\n")
        cases = ((None, "user=weave", "1"), ("0", "user=weave", "0"), ("1", "user=weave", "1"),
                 ("", "user=weave", "1"), ("true", "user=weave", "invalid"),
                 ("0", "user=weave sslsni=1", "1"), ("1", "user=weave sslsni=0", "0"),
                 ("1", "service=off", "0"), ("0", "service=on", "1"),
                 (None, "service=bad", "invalid"), ("1", "service=off sslsni=1", "1"),
                 ("0", "service=on sslsni=0", "0"))
        for value, text, expected in cases:
            env = dict(base, PGSERVICEFILE=str(service))
            if value is not None:
                env["PGSSLSNI"] = value
            run = subprocess.run([args.executable, "--load", expected, text], env=env,
                                 capture_output=True, text=True, timeout=20)
            event = dict(environment=value, input=text, expected=expected, returncode=run.returncode,
                         stdout=run.stdout, stderr=run.stderr)
            controls.append(event)
            if run.returncode or run.stderr or not run.stdout.startswith("SNI loader: "):
                raise RuntimeError(json.dumps(event))
    if Path(temporary).exists():
        raise RuntimeError("Owned service fixture survived")
    print(json.dumps(dict(controls=controls, fixture_removed=True)), flush=True)


if __name__ == "__main__":
    main()
