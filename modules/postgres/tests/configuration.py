"""Run configuration regressions in processes with isolated environment and files."""

import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    base = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
    base.update(PGUSER="env_user", PGPASSWORD="env_password", PGAPPNAME="env_app", PGHOST="env_host",
                PGDATESTYLE="ISO, MDY", PGTZ="UTC", PGGEQO="off", PGLOADBALANCEHOSTS="random",
                PGREQUIREAUTH="scram-sha-256", PGMINPROTOCOLVERSION="3.0", PGMAXPROTOCOLVERSION="3.2")
    security = "PostgreSQL ambient security overrides*"
    files = "PostgreSQL ambient service and password files*"
    peer = "PostgreSQL ambient peer policy*"
    controls = []
    with tempfile.TemporaryDirectory(prefix="weave-postgres-configuration-") as directory:
        base.update(HOME=directory, APPDATA=directory)

        def run(environment, *options, expected=1):
            result = subprocess.run([args.executable, directory, *options], env=environment,
                                    capture_output=True, text=True, timeout=15,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            print(result.stdout, end="", flush=True)
            print(result.stderr, end="", file=sys.stderr, flush=True)
            result.check_returncode()
            cases = re.search(r"test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", result.stdout)
            assertions = re.search(r"assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", result.stdout)
            if not cases or tuple(map(int, cases.groups())) != (expected, expected, 0):
                raise RuntimeError("Missing or empty configuration test selection")
            if not assertions or int(assertions[1]) <= 0 or int(assertions[1]) != int(assertions[2]) or int(assertions[3]):
                raise RuntimeError("Missing configuration assertion evidence")
            controls.append(dict(options=list(options), cases=expected, assertions=int(assertions[1])))

        run(base, f"--test-case-exclude={security},{files},{peer}", expected=12)
        run(dict(base, PGSSLMODE="prefer"), f"--test-case={security}")
        run(dict(base, PGREQUIRESSL="1"), f"--test-case={security}")
        root = Path(directory)
        run(dict(base, PGSERVICE="env-service", PGSERVICEFILE=str(root / "env-service.conf"),
                 PGPASSFILE=str(root / "env-password"), PGSYSCONFDIR=directory), f"--test-case={files}")
        username = "postgres"
        if os.name != "nt":
            import pwd
            username = pwd.getpwuid(os.geteuid()).pw_name
        run(dict(base, PGREQUIREPEER=username), f"--test-case={peer}")

    if Path(directory).exists():
        raise RuntimeError("Owned configuration fixture survived")
    print(json.dumps(dict(controls=controls, fixture_removed=True)), flush=True)


if __name__ == "__main__":
    main()
