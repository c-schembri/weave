"""Opt-in real PostgreSQL qualification against a private, disposable cluster."""

import argparse
import base64
import hashlib
import hmac
import ipaddress
import json
import os
from pathlib import Path
import queue
import secrets
import shutil
import socket
import subprocess
import tempfile
import threading


def run(command):
    if os.name == "nt":
        # pg_ctl's server inherits output handles; pipes would wait for the server to exit.
        with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
            result = subprocess.run(command, stdout=output, stderr=errors, timeout=30)
            output.seek(0)
            errors.seek(0)
            result.stdout = output.read().decode("utf-8", errors="replace")
            result.stderr = errors.read().decode("utf-8", errors="replace")
            result.check_returncode()
            return result
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--windows-client", action="store_true",
                        help="Run a native Windows executable from WSL against its disposable Linux server")
    parser.add_argument("--local-abstract", action="store_true",
                        help="Qualify the Linux local client against an abstract PostgreSQL socket")
    parser.add_argument("--exchanges", action="store_true",
                        help="Enable logical replication fixtures for mixed query/COPY qualification")
    parser.add_argument("--encoding", action="store_true",
                        help="Add a SQL_ASCII database for encoding qualification")
    parser.add_argument("--scram-keys", action="store_true",
                        help="Derive passthrough keys from the owned disposable role verifier")
    args = parser.parse_args()
    if args.windows_client and os.name == "nt":
        parser.error("--windows-client is a WSL-only transport qualification mode")
    if args.local_abstract and (os.name == "nt" or args.windows_client):
        parser.error("--local-abstract requires a native Linux client")
    client_address = "127.0.0.1"
    client_gateway = "127.0.0.1"
    if args.windows_client:
        addresses = json.loads(run(["ip", "-j", "-4", "address", "show", "dev", "eth0"]).stdout)
        client_address = str(ipaddress.IPv4Address(addresses[0]["addr_info"][0]["local"]))
        routes = json.loads(run(["ip", "-j", "-4", "route", "show", "default"]).stdout)
        client_gateway = str(ipaddress.IPv4Address(routes[0]["gateway"]))
    suffix = ".exe" if os.name == "nt" else ""
    initdb = str(args.server_bin / ("initdb" + suffix))
    pg_ctl = str(args.server_bin / ("pg_ctl" + suffix))
    psql = str(args.server_bin / ("psql" + suffix))
    pg_basebackup = str(args.server_bin / ("pg_basebackup" + suffix))
    process = subprocess.Popen([args.executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    lines = queue.Queue()
    reader = threading.Thread(target=lambda: lines.put(tuple(process.stdout.readline().strip() for _ in range(3))), daemon=True)
    reader.start()
    started = False
    standby_started = False
    password = None
    with tempfile.TemporaryDirectory(prefix="weave-postgres-") as directory:
        root = Path(directory)
        local_address = "@weave-postgres-" + secrets.token_hex(12) if args.local_abstract else str(root)
        data = root / "data"
        standby = root / "standby"
        try:
            ca, certificate, key = lines.get(timeout=10)
            if not ca or not certificate or not key:
                raise RuntimeError("TLS fixture failed")

            if args.windows_client:
                def linux_path(path):
                    return run(["wslpath", "-u", path]).stdout.strip()

                ca, certificate, key = (linux_path(path) for path in (ca, certificate, key))

            password = secrets.token_urlsafe(24)
            password_file = root / "password"
            password_file.write_text(password, encoding="utf-8")
            password_file.chmod(0o600)
            run([initdb, "-D", str(data), "-U", "administrator", "--auth-host=scram-sha-256", "--auth-local=trust",
                 "--pwfile", str(password_file), "--no-instructions"])
            password_file.unlink()

            private_key = root / "server.key"
            shutil.copyfile(key, private_key)
            private_key.chmod(0o600)
            def configuration_path(path):
                return str(path).replace("\\", "/").replace("'", "''")

            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            with (data / "postgresql.conf").open("a", encoding="utf-8") as configuration:
                listen = "127.0.0.1" if not args.windows_client else f"127.0.0.1,{client_address}"
                configuration.write(f"\nlisten_addresses='{listen}'\nport={port}\nssl=on\n")
                configuration.write(f"ssl_cert_file='{configuration_path(certificate)}'\n")
                configuration.write(f"ssl_key_file='{configuration_path(private_key)}'\n")
                configuration.write(f"ssl_ca_file='{configuration_path(ca)}'\nmax_connections=100\n")
                if args.exchanges:
                    configuration.write("wal_level=logical\nmax_wal_senders=40\nmax_replication_slots=40\n")
                if os.name != "nt":
                    configuration.write(f"unix_socket_directories='{configuration_path(local_address)}'\n")
            with (data / "pg_hba.conf").open("w", encoding="utf-8") as rules:
                addresses = ["127.0.0.1"]
                if args.windows_client:
                    addresses.append(client_gateway)
                for address in addresses:
                    rules.write(f"hostssl all weave_md5 {address}/32 md5 clientcert=verify-ca\n")
                    rules.write(f"hostssl all weave_password {address}/32 password clientcert=verify-ca\n")
                rules.write("local all all trust\nhostssl all all 127.0.0.1/32 scram-sha-256 clientcert=verify-ca\n")
                rules.write("hostnossl all all 127.0.0.1/32 scram-sha-256\n")
                rules.write("hostnossl replication administrator 127.0.0.1/32 scram-sha-256\n")
                rules.write("hostssl replication weave_replication 127.0.0.1/32 scram-sha-256 clientcert=verify-ca\n")
                rules.write("hostnossl replication weave_replication 127.0.0.1/32 scram-sha-256\n")
                if args.windows_client:
                    rules.write(f"hostssl all weave {client_gateway}/32 scram-sha-256 clientcert=verify-ca\n")
                    rules.write(f"hostnossl all weave {client_gateway}/32 scram-sha-256\n")
                    rules.write(f"hostssl all weave_replication {client_gateway}/32 scram-sha-256 clientcert=verify-ca\n")
                    rules.write(f"hostnossl all weave_replication {client_gateway}/32 scram-sha-256\n")
                    rules.write(f"hostssl replication weave_replication {client_gateway}/32 scram-sha-256 clientcert=verify-ca\n")
                    rules.write(f"hostnossl replication weave_replication {client_gateway}/32 scram-sha-256\n")

            run([pg_ctl, "-D", str(data), "-l", str(root / "server.log"), "-w", "start"])
            started = True
            environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
            environment.update(PGPASSWORD=password, PGSSLMODE="disable")
            role = "CREATE ROLE weave LOGIN PASSWORD '" + password.replace("'", "''") + "';\n"
            role += "CREATE ROLE weave_replication LOGIN REPLICATION PASSWORD '" + password.replace("'", "''") + "';\n"
            role += "CREATE ROLE weave_password LOGIN PASSWORD '" + password.replace("'", "''") + "';\n"
            role += "SET password_encryption = 'md5';\n"
            role += "CREATE ROLE weave_md5 LOGIN PASSWORD '" + password.replace("'", "''") + "';\n"
            if args.encoding:
                role += "CREATE DATABASE encoding_ascii ENCODING 'SQL_ASCII' "
                role += "LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;\n"
            if args.exchanges:
                role += "CREATE TABLE public.logical_probe(key INT, value TEXT);\n"
                role += "ALTER TABLE public.logical_probe OWNER TO weave_replication;\n"
                role += "GRANT INSERT ON public.logical_probe TO weave;\n"
                role += "GRANT CREATE ON DATABASE postgres TO weave_replication;\n"
            subprocess.run([psql, "-h", "127.0.0.1", "-p", str(port), "-U", "administrator", "-d", "postgres",
                            "-X", "-v", "ON_ERROR_STOP=1"], input=role,
                           env=environment, check=True, capture_output=True, text=True, timeout=30)
            if args.scram_keys:
                verifier = subprocess.run(
                    [psql, "-h", "127.0.0.1", "-p", str(port), "-U", "administrator",
                     "-d", "postgres", "-X", "-At",
                     "-c", "SELECT rolpassword FROM pg_authid WHERE rolname='weave'"],
                    env=environment, check=True, capture_output=True, text=True, timeout=30).stdout.strip()
                algorithm, parameters, keys = verifier.split("$")
                iterations, salt = parameters.split(":")
                stored_key, server_key = (base64.b64decode(value, validate=True) for value in keys.split(":"))
                if algorithm != "SCRAM-SHA-256":
                    raise RuntimeError("Unexpected verifier format")
                salted = hashlib.pbkdf2_hmac("sha256", password.encode(), base64.b64decode(salt), int(iterations))
                client_key = hmac.digest(salted, b"Client Key", "sha256")
                if hashlib.sha256(client_key).digest() != stored_key or hmac.digest(salted, b"Server Key", "sha256") != server_key:
                    raise RuntimeError("Independent key derivation disagrees with server")
            subprocess.run([pg_basebackup, "--checkpoint=fast", "-h", "127.0.0.1", "-p", str(port), "-U", "administrator",
                            "-D", str(standby), "-X", "stream", "-R", "--no-password"],
                           env=environment, check=True, capture_output=True, text=True, timeout=30)
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                standby_port = reservation.getsockname()[1]
            run([pg_ctl, "-D", str(standby), "-l", str(root / "standby.log"), "-o", f"-p {standby_port}",
                 "-w", "start"])
            standby_started = True
            local_directory = local_address if os.name != "nt" and not args.windows_client else ""
            process.stdin.write(f"{port}\n{password}\n{standby_port}\n{client_address}\n{local_directory}\n")
            if args.scram_keys:
                process.stdin.write(base64.b64encode(client_key).decode() + "\n")
                process.stdin.write(base64.b64encode(server_key).decode() + "\n")
            if args.exchanges:
                plugin_suffix = ".dll" if os.name == "nt" else ".so"
                plugin = args.server_bin.parent / "lib" / ("test_decoding" + plugin_suffix)
                process.stdin.write("1\n" if plugin.is_file() else "0\n")
            process.stdin.flush()
            try:
                output, error = process.communicate(timeout=120)
            except subprocess.TimeoutExpired as failure:
                for captured in (failure.stdout, failure.stderr):
                    if captured:
                        diagnostic = captured.decode("utf-8", errors="replace") if isinstance(captured, bytes) else captured
                        print(diagnostic.replace(password, "<fixture password>"))
                raise
            if process.returncode:
                raise RuntimeError(f"Native qualification failed ({process.returncode}): {output}\n{error}")
            print(output)
        except Exception:
            for name in ("server.log", "standby.log"):
                log = root / name
                if log.exists():
                    diagnostic = log.read_text(encoding="utf-8", errors="replace")
                    print(diagnostic.replace(password, "<fixture password>") if password else diagnostic)
            raise
        finally:
            if process.poll() is None:
                process.kill()
            reader.join(timeout=10)
            process.communicate(timeout=10)
            cleanup_error = None
            clusters = ((standby, standby_started), (data, started))
            for cluster, running in clusters:
                if running or (cluster / "postmaster.pid").exists():
                    try:
                        run([pg_ctl, "-D", str(cluster), "-m", "immediate", "-w", "stop"])
                    except Exception as error:
                        if cleanup_error is None:
                            cleanup_error = error
            if cleanup_error is not None:
                raise cleanup_error


if __name__ == "__main__":
    main()
