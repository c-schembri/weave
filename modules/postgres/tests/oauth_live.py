"""Owned PostgreSQL 18 OAuth validator fixture; never uses an existing cluster."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import tempfile
import threading
import re


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=30, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, default=Path("/usr/lib/postgresql/18/bin"))
    parser.add_argument("--windows-client", action="store_true")
    args = parser.parse_args()
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
    server = args.server_bin.resolve()
    address = gateway = "127.0.0.1"
    if args.windows_client:
        interfaces = json.loads(run(["ip", "-j", "-4", "address", "show", "dev", "eth0"]).stdout)
        address = str(ipaddress.IPv4Address(interfaces[0]["addr_info"][0]["local"]))
        routes = json.loads(run(["ip", "-j", "-4", "route", "show", "default"]).stdout)
        gateway = str(ipaddress.IPv4Address(routes[0]["gateway"]))
    process = subprocess.Popen([args.executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, env=environment)
    lines = queue.Queue()
    reader = threading.Thread(target=lambda: lines.put(tuple(process.stdout.readline().strip() for _ in range(3))),
                              daemon=True)
    reader.start()
    with tempfile.TemporaryDirectory(prefix="weave-oauth-native-") as directory:
        root = Path(directory).resolve()
        data = root / "data"
        started = False
        try:
            ca, certificate, key = lines.get(timeout=10)
            if not all((ca, certificate, key)):
                raise RuntimeError("client failed to publish its TLS fixture")
            if args.windows_client:
                ca, certificate, key = (run(["wslpath", "-u", path]).stdout.strip()
                                        for path in (ca, certificate, key))
            run([str(server / "initdb"), "-D", str(data), "-U", "administrator", "--auth=trust",
                 "--no-instructions"], env=environment)
            private_key = root / "server.key"
            shutil.copyfile(key, private_key)
            private_key.chmod(0o600)
            shutil.copyfile(args.validator, root / "weave_oauth_validator.so")
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            listen = "127.0.0.1" if not args.windows_client else "127.0.0.1," + address
            with (data / "postgresql.conf").open("a", encoding="utf-8") as config:
                config.write(f"\nlisten_addresses='{listen}'\nport={port}\nmax_connections=160\nssl=on\n")
                config.write(f"ssl_cert_file='{certificate}'\nssl_key_file='{private_key}'\nssl_ca_file='{ca}'\n")
                config.write(f"unix_socket_directories='{root}'\ndynamic_library_path='{root}'\n")
                config.write("oauth_validator_libraries='weave_oauth_validator'\n")
                config.write("log_connections=on\n")
            with (data / "pg_hba.conf").open("w", encoding="utf-8") as hba:
                hba.write("local all all trust\n")
                addresses = sorted({"127.0.0.1", gateway})
                for peer in addresses:
                    hba.write(f"hostssl all weave {peer}/32 oauth issuer=\"https://issuer.example/tenant\" "
                              "scope=\"read write\" validator=weave_oauth_validator\n")
            run([str(server / "pg_ctl"), "-D", str(data), "-l", str(root / "server.log"), "-w", "start"],
                env=environment)
            started = True
            run([str(server / "psql"), "-h", str(root), "-p", str(port), "-U", "administrator", "-d", "postgres",
                 "-X", "-v", "ON_ERROR_STOP=1", "-c", "CREATE ROLE weave LOGIN;"], env=environment)
            process.stdin.write(str(port) + "\n" + address + "\n")
            process.stdin.flush()
            output, error = process.communicate(timeout=180)
            print(output, end="")
            if process.returncode:
                print(error)
                print((root / "server.log").read_text(encoding="utf-8"))
                raise RuntimeError(f"native OAuth client exited {process.returncode}")
            match = re.search(r"callbacks=(\d+) lookups=(\d+) libpq_cache_lookups=(\d+) libpq_control=1", output)
            if not match:
                raise RuntimeError("Missing completed cache/fallback/libpq controls")
            callbacks, lookups, baseline = map(int, match.groups())
            assert lookups == 2 * callbacks and baseline == 2
            log = (root / "server.log").read_text(encoding="utf-8")
            received = log.count("connection received:")
            expected = 3 * callbacks + baseline + 1
            if received != expected:
                print(log)
                raise RuntimeError(f"Expected {expected} physical connections, received {received}")
            print(f"Independent server connection receipts={received}, expected={expected}")
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            # Only the newly initialized, explicitly owned directory may be stopped.
            if started or (data / "postmaster.pid").exists():
                assert data.resolve().parent == root
                run([str(server / "pg_ctl"), "-D", str(data), "-m", "immediate", "-w", "stop"], env=environment)


if __name__ == "__main__":
    main()
