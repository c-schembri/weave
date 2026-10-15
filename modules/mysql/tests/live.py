"""Qualify against a private MySQL instance, never an existing database service."""

import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from placement import current_cpus, parse_cpus, placed, validate


def run(arguments, **kwargs):
    return subprocess.run(arguments, check=True, timeout=30, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--test", required=True, type=Path)
    parser.add_argument("--test-timeout", type=float, default=30, help="Bounded opt-in workload timeout in seconds")
    parser.add_argument("--client-cpus", type=parse_cpus, help="Optional logical CPU IDs inherited by the test")
    parser.add_argument("--server-cpus", type=parse_cpus, help="Disjoint logical CPU IDs inherited by the private server")
    args = parser.parse_args()
    if args.client_cpus is not None or args.server_cpus is not None:
        validate(args.client_cpus, args.server_cpus, current_cpus())
        print(f"Logical CPU placement: client={sorted(args.client_cpus)} server={sorted(args.server_cpus)}",
              file=sys.stderr, flush=True)
    openssl = shutil.which("openssl")
    if not openssl:
        raise RuntimeError("openssl executable required to create isolated test certificates")

    with tempfile.TemporaryDirectory(prefix="weave-mysql-") as temporary:
        root = Path(temporary)
        data = root / "data"
        certificate = root / "cert.pem"
        key = root / "key.pem"
        base = args.server.resolve().parent.parent
        run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
             "-addext", "basicConstraints=critical,CA:TRUE", "-keyout", str(key), "-out", str(certificate)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = [str(args.server.resolve()), "--no-defaults", f"--basedir={base}",
                  f"--datadir={data}", "--mysqlx=0", "--secure-file-priv=NULL"]
        if os.name == "nt":
            # Own the actual server, not a RESTART monitor whose child survives terminate().
            server.append("--no-monitor")
        if os.name != "nt" and os.geteuid() == 0:
            server.append("--user=root")
        run([*server, "--initialize-insecure"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        with socket.socket() as allocation:
            allocation.bind(("127.0.0.1", 0))
            port = allocation.getsockname()[1]
        log = root / "server.log"
        with log.open("wb") as output:
            process = None
            try:
                with placed(args.server_cpus):
                    process = subprocess.Popen([
                        *server, "--bind-address=127.0.0.1", f"--port={port}", f"--socket={root / 'mysql.sock'}",
                        f"--pid-file={root / 'mysql.pid'}", f"--ssl-ca={certificate}", f"--ssl-cert={certificate}",
                        f"--ssl-key={key}", "--max-allowed-packet=67108864"], stdout=output, stderr=output)
                deadline = time.monotonic() + 15
                while True:
                    if process.poll() is not None:
                        raise RuntimeError(log.read_text(errors="replace"))
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("MySQL fixture startup timed out")
                        time.sleep(0.05)

                setup = [str(args.client.resolve()), "--no-defaults", "--protocol=TCP", "--host=127.0.0.1",
                         f"--port={port}", "--user=root", "--ssl-mode=DISABLED"]
                # Fixture-only root is empty-password and private; password literals are never production credentials.
                sql = ("CREATE DATABASE weave_test; "
                       "CREATE USER 'weave'@'%' IDENTIFIED BY 'fixture-password'; "
                       "GRANT ALL ON weave_test.* TO 'weave'@'%';")
                run(setup, input=sql, text=True, stdout=subprocess.DEVNULL)
                executable = [str(args.test.resolve())]
                if args.test.suffix == ".py":
                    executable.insert(0, sys.executable)
                with placed(args.client_cpus):
                    subprocess.run([*executable, str(port), str(certificate), "fixture-password"],
                                   check=True, timeout=args.test_timeout)
            finally:
                if process is not None:
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


if __name__ == "__main__":
    main()
