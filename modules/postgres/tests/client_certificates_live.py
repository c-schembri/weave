"""Owned PostgreSQL 18 clusters for certificate policy and actual cancellation controls."""

import argparse
import json
import os
from pathlib import Path
import queue
import re
import secrets
import shutil
import socket
import subprocess
import tempfile
import threading


def invoke(command, environment, output=None, **kwargs):
    if output is None:
        result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=45, **kwargs)
    else:
        # The long-lived server can inherit pg_ctl's handles. Files do not wait for pipe EOF.
        with output.open("wb") as log:
            result = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=45, **kwargs)
        result.stdout = output.read_text(encoding="utf-8", errors="replace")
        result.stderr = ""
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command[0]}\n{result.stdout}\n{result.stderr}")
    return result


def scenario(executable, server_bin, request, version, evidence, native=False, legacy_native=False):
    environment = {name: value for name, value in os.environ.items() if not name.upper().startswith("PG")}
    server_environment = environment | {"PATH": str(server_bin) + os.pathsep + environment["PATH"]}
    client = subprocess.Popen([str(executable)], env=environment, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                              creationflags=subprocess.CREATE_NO_WINDOW)
    lines = queue.Queue()
    reader = threading.Thread(target=lambda: lines.put(tuple(client.stdout.readline().strip() for _ in range(3))))
    reader.start()
    started = False
    result = None
    with tempfile.TemporaryDirectory(prefix="pg-cert-") as directory:
        root = Path(directory)
        data = root / "data"
        try:
            ca, certificate, key = lines.get(timeout=10)
            reader.join(timeout=10)
            if reader.is_alive() or not all((ca, certificate, key)):
                raise RuntimeError("Client fixture did not publish three certificate paths")
            password = secrets.token_urlsafe(24)
            password_file = root / "password"
            password_file.write_text(password, encoding="utf-8")
            invoke([str(server_bin / "initdb.exe"), "-D", str(data), "-U", "administrator",
                    "--auth-host=scram-sha-256", "--auth-local=trust", "--pwfile", str(password_file),
                    "--no-locale", "--encoding=UTF8", "--no-instructions"], server_environment)
            password_file.unlink()
            server_key = root / "server.key"
            shutil.copyfile(key, server_key)
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]

            def quote(path):
                return str(path).replace("\\", "/").replace("'", "''")

            settings = [f"listen_addresses='127.0.0.1'", f"port={port}", "ssl=on", "max_connections=80",
                        f"ssl_cert_file='{quote(certificate)}'", f"ssl_key_file='{quote(server_key)}'",
                        f"ssl_min_protocol_version='TLSv1.{version[-1]}'",
                        f"ssl_max_protocol_version='TLSv1.{version[-1]}'"]
            if request:
                settings.append(f"ssl_ca_file='{quote(ca)}'")
            with (data / "postgresql.conf").open("a", encoding="utf-8") as config:
                config.write("\n" + "\n".join(settings) + "\n")
            hba = ["local all all trust",
                   "hostnossl all administrator 127.0.0.1/32 scram-sha-256"]
            if request:
                hba.append("hostssl all weave_mtls 127.0.0.1/32 scram-sha-256 clientcert=verify-ca")
            hba.append("hostssl all weave 127.0.0.1/32 scram-sha-256")
            (data / "pg_hba.conf").write_text("\n".join(hba) + "\n", encoding="utf-8")
            invoke([str(server_bin / "pg_ctl.exe"), "-D", str(data), "-l", str(root / "server.log"),
                    "-w", "start"], server_environment,
                   output=evidence / f"request-{int(request)}-tls-{version}.start.log")
            started = True
            admin_environment = server_environment | {"PGPASSWORD": password, "PGSSLMODE": "disable"}
            roles = "\n".join(f"CREATE ROLE {role} LOGIN PASSWORD '{password}';" for role in ("weave", "weave_mtls"))
            invoke([str(server_bin / "psql.exe"), "-h", "127.0.0.1", "-p", str(port), "-U", "administrator",
                    "-d", "postgres", "-X", "-v", "ON_ERROR_STOP=1"], admin_environment, input=roles)
            version_check = invoke([str(server_bin / "psql.exe"), "-h", "127.0.0.1", "-p", str(port),
                                    "-U", "administrator", "-d", "postgres", "-X", "-At",
                                    "-c", "SHOW server_version_num"], admin_environment).stdout.strip()
            if version_check != "180006":
                raise RuntimeError(f"Unexpected real server version: {version_check}")
            if legacy_native:
                client.stdin.write(f"{port}\n{password}\n0\n127.0.0.1\n\n")
            else:
                client.stdin.write(f"{port}\n{password}\n{int(request)}\n{version}\n")
            client.stdin.flush()
            try:
                output, error = client.communicate(timeout=240)
            except subprocess.TimeoutExpired:
                client.kill()
                output, error = client.communicate(timeout=10)
                raise RuntimeError(f"Certificate control timed out\n{output}\n{error}")
            name = f"request-{int(request)}-tls-{version}"
            (evidence / (name + ".stdout")).write_text(output, encoding="utf-8")
            (evidence / (name + ".stderr")).write_text(error, encoding="utf-8")
            print(output, end="", flush=True)
            print(error, end="", flush=True)
            if client.returncode or error:
                raise RuntimeError(f"Certificate control failed: exit={client.returncode}")
            if legacy_native:
                match = re.search(r"Native direct/legacy TLS: (\d+) checks, 2 observed query cancellations, "
                                  r"libpq=180004 OpenSSL=OpenSSL 3\.6\.5 29 Sep 2026", output)
                if not match or int(match[1]) <= 0:
                    raise RuntimeError("Missing positive permanent native TLS summary")
                result = {"checks": int(match[1]), "query_cancellations": 2,
                          "request": request, "tls": version, "server_version": version_check}
            else:
                label = "Native real certificate policy" if native else "Real certificate policy"
                match = re.search(label + r": (\d+) checks, (\d+) cases, (\d+) sessions, "
                                  r"(\d+) observed query cancellations", output)
                if not match:
                    raise RuntimeError("Missing positive real-server certificate summary")
                counts = dict(zip(("checks", "cases", "sessions", "query_cancellations"), map(int, match.groups())))
                if counts["checks"] <= 0 or counts["cases"] != (14 if native else 20) or counts["query_cancellations"] <= 0:
                    raise RuntimeError("Empty or incomplete real-server controls")
                result = counts | {"request": request, "tls": version, "server_version": version_check}
            if native:
                extra = re.search(r"(\d+) active stale-request controls, (\d+) invalid-identity controls, "
                                  r"libpq=180004 OpenSSL=OpenSSL 3\.6\.5 29 Sep 2026", output)
                if not extra or int(extra[1]) <= 0 or int(extra[2]) != 4:
                    raise RuntimeError("Missing native version/stale-query evidence")
                result["active_stale_controls"] = int(extra[1])
                result["invalid_identity_controls"] = int(extra[2])
            elif not legacy_native:
                extra = re.search(r"(\d+) active stale-request controls", output)
                if not extra or int(extra[1]) <= 0:
                    raise RuntimeError("Missing active stale-query evidence")
                result["active_stale_controls"] = int(extra[1])
        finally:
            if client.poll() is None:
                client.kill()
            reader.join(timeout=10)
            residual_output, residual_error = client.communicate(timeout=10)
            if result is None:
                print(f"Failed client exit: {client.returncode}\n{residual_output}\n{residual_error}", flush=True)
                (evidence / f"request-{int(request)}-tls-{version}.failure.json").write_text(
                    json.dumps({"exit": client.returncode, "stdout": residual_output, "stderr": residual_error}, indent=2),
                    encoding="utf-8")
            if started or (data / "postmaster.pid").exists():
                invoke([str(server_bin / "pg_ctl.exe"), "-D", str(data), "-m", "immediate", "-w", "stop"],
                       server_environment, output=evidence / f"request-{int(request)}-tls-{version}.stop.log")
            log = root / "server.log"
            if log.exists():
                text = log.read_text(encoding="utf-8", errors="replace")
                if "password" in locals():
                    text = text.replace(password, "<fixture password>")
                (evidence / f"request-{int(request)}-tls-{version}.server.log").write_text(text, encoding="utf-8")
    if root.exists():
        raise RuntimeError("Disposable cluster was not removed")
    result["fixture_removed"] = True
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--evidence", type=Path,
                        help="Retain fixture logs in a new directory; omitted uses disposable test output")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--native", action="store_true")
    modes.add_argument("--legacy-native", action="store_true")
    args = parser.parse_args()
    if args.evidence is None:
        with tempfile.TemporaryDirectory(prefix="weave-cert-policy-") as directory:
            execute(args, Path(directory))
    else:
        args.evidence.mkdir(parents=True, exist_ok=False)
        execute(args, args.evidence)


def execute(args, evidence):
    results = []
    for request in (False, True):
        for version in ("12", "13"):
            results.append(scenario(args.executable, args.server_bin, request, version, evidence,
                                    args.native, args.legacy_native))
    report = {"clusters": results, "actual_server": "PostgreSQL 18.6", "native_windows": True,
              "installed_service": False, "native_client": args.native or args.legacy_native,
              "permanent_native_client": args.legacy_native}
    (evidence / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
