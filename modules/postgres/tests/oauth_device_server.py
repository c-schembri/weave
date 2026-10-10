"""Owned PG18 opaque-token validator fixture for native device-flow tests."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import signal
import subprocess
import sys
import tempfile


def run(arguments, environment):
    return subprocess.run(arguments, check=True, capture_output=True, text=True, timeout=40, env=environment)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--issuer", required=True)
    parser.add_argument("--ca", required=True)
    parser.add_argument("--certificate", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--windows-client", action="store_true")
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--validator", type=Path, required=True)
    args = parser.parse_args()
    assert args.issuer.startswith("https://127.0.0.1:") and '"' not in args.issuer
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
    server = args.server_bin.resolve()
    validator = args.validator.resolve()
    address = gateway = "127.0.0.1"
    if args.windows_client:
        interfaces = json.loads(run(["ip", "-j", "-4", "address", "show", "dev", "eth0"], environment).stdout)
        address = str(ipaddress.IPv4Address(interfaces[0]["addr_info"][0]["local"]))
        routes = json.loads(run(["ip", "-j", "-4", "route", "show", "default"], environment).stdout)
        gateway = str(ipaddress.IPv4Address(routes[0]["gateway"]))
        args.ca, args.certificate, args.key = (
            run(["wslpath", "-u", value], environment).stdout.strip()
            for value in (args.ca, args.certificate, args.key))
    with tempfile.TemporaryDirectory(prefix="weave-native-device-pg-") as directory:
        root = Path(directory).resolve()
        data = root / "data"
        try:
            run([str(server / "initdb"), "-D", str(data), "-U", "administrator", "--auth=trust", "--no-instructions"], environment)
            private_key = root / "server.key"
            shutil.copyfile(args.key, private_key)
            private_key.chmod(0o600)
            shutil.copyfile(validator, root / "weave_oauth_validator.so")
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            listen = "127.0.0.1" if not args.windows_client else "127.0.0.1," + address
            with (data / "postgresql.conf").open("a", encoding="utf-8") as config:
                config.write(f"\nlisten_addresses='{listen}'\nport={port}\nmax_connections=160\nssl=on\n")
                config.write(f"ssl_cert_file='{args.certificate}'\nssl_key_file='{private_key}'\nssl_ca_file='{args.ca}'\n")
                config.write(f"unix_socket_directories='{root}'\ndynamic_library_path='{root}'\n")
                config.write("oauth_validator_libraries='weave_oauth_validator'\n")
            with (data / "pg_hba.conf").open("w", encoding="utf-8") as hba:
                hba.write("local all all trust\n")
                for peer in sorted({"127.0.0.1", gateway}):
                    hba.write(f'hostssl all weave {peer}/32 oauth issuer="{args.issuer}" '
                              'scope="read write" validator=weave_oauth_validator\n')
            run([str(server / "pg_ctl"), "-D", str(data), "-l", str(root / "server.log"), "-w", "start"], environment)
            run([str(server / "psql"), "-h", str(root), "-p", str(port), "-U", "administrator", "-d", "postgres",
                 "-X", "-v", "ON_ERROR_STOP=1", "-c", "CREATE ROLE weave LOGIN;"], environment)
            print(port, address, sep="\n", flush=True)
            sys.stdin.readline()
            print((root / "server.log").read_text(encoding="utf-8"), file=sys.stderr)
        finally:
            if (data / "postmaster.pid").exists():
                assert data.resolve().parent == root
                run([str(server / "pg_ctl"), "-D", str(data), "-m", "immediate", "-w", "stop"], environment)


if __name__ == "__main__":
    def stop(signum, frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, stop)
    main()
