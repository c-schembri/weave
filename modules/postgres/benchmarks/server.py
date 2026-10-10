"""Local Windows campaign against an owned native server. Never invoked by CI."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile


def affinity(mask):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.SetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), mask):
        raise ctypes.WinError(ctypes.get_last_error())


def invoke(command, environment, path, timeout=60, input=None):
    # pg_ctl may hand its output handles to its long-lived server; no pipe EOF dependency.
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        process = subprocess.run(command, env=environment, stdout=output, stderr=errors,
                                 input=input, text=True, timeout=timeout)
        output.seek(0)
        errors.seek(0)
        process.stdout = output.read().decode("utf-8", errors="replace")
        process.stderr = errors.read().decode("utf-8", errors="replace")
    path.write_text(process.stdout + process.stderr, encoding="utf-8")
    print(process.stdout, end="", flush=True)
    if process.returncode:
        raise RuntimeError(f"Campaign command failed ({process.returncode}): {command[0]}\n{process.stderr}")
    return process


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", type=Path, required=True)
    parser.add_argument("--concurrent", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=2)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--phase", choices=("all", "concurrent"), default="all")
    parser.add_argument("--workers", type=int, nargs="+", choices=(1, 2, 4), default=[1, 2, 4])
    args = parser.parse_args()
    if os.name != "nt" or args.output.exists() or args.seconds <= 0 or args.repetitions < 3:
        parser.error("Requires Windows, a new output directory and positive samples with at least three pairs")

    args.output.mkdir(parents=True)
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
    environment["PATH"] = str(args.server_bin) + os.pathsep + environment["PATH"]
    topology = json.loads(subprocess.check_output([str(args.concurrent), "--topology"], env=environment, text=True))
    cores = topology["cores"]
    if len(cores) < 6:
        raise RuntimeError("Need two physical server cores and four distinct client cores")
    server_cpus = [cpu for core in cores[:2] for cpu in core]
    client_cpus = [core[0] for core in cores[-4:]]
    server_mask = sum(1 << cpu for cpu in server_cpus)
    client_mask = sum(1 << cpu for cpu in client_cpus)
    manifest = {"complete": False, "topology": topology, "server_cpus": server_cpus,
                "client_cpus": client_cpus, "native_windows_server": True,
                "before": None, "change_pct": None, "commands": [], "memory": [],
                "binaries": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in (args.serial, args.concurrent, args.fixture, args.server_bin / "postgres.exe")}}
    fixture = subprocess.Popen([str(args.fixture)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, env=environment,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    started = False
    with tempfile.TemporaryDirectory(prefix="weave-benchmark-") as directory:
        root = Path(directory)
        data = root / "data"
        try:
            ca, certificate, key, openssl = [fixture.stdout.readline().strip() for _ in range(4)]
            if not all((ca, certificate, key, openssl)):
                raise RuntimeError("Certificate fixture did not publish complete metadata")
            manifest["openssl"] = openssl
            password = secrets.token_urlsafe(24)
            password_file = root / "password"
            password_file.write_text(password, encoding="utf-8")
            environment["WEAVE_PG_PASSWORD"] = password
            environment["PGPASSWORD"] = password

            def run(name, command, timeout=60, input=None):
                manifest["commands"].append(command)
                return invoke(command, environment, args.output / (name + ".log"), timeout, input)

            affinity(server_mask)
            run("initdb", [str(args.server_bin / "initdb.exe"), "-D", str(data), "-U", "weave",
                           "--auth-host=scram-sha-256", "--auth-local=trust", "--pwfile", str(password_file),
                           "--no-locale", "--encoding=UTF8", "--no-instructions"])
            password_file.unlink()
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]

            def quoted(path):
                return str(path).replace("\\", "/").replace("'", "''")

            settings = ["listen_addresses='127.0.0.1'", f"port={port}", "max_connections=80", "ssl=on",
                        f"ssl_cert_file='{quoted(certificate)}'", f"ssl_key_file='{quoted(key)}'",
                        "ssl_min_protocol_version='TLSv1.3'", "ssl_max_protocol_version='TLSv1.3'"]
            with (data / "postgresql.conf").open("a", encoding="utf-8") as config:
                config.write("\n" + "\n".join(settings) + "\n")
            run("start", [str(args.server_bin / "pg_ctl.exe"), "-D", str(data), "-l", str(root / "server.log"),
                          "-w", "start"])
            started = True
            environment["PGSSLMODE"] = "disable"
            version = run("version", [str(args.server_bin / "psql.exe"), "-h", "127.0.0.1", "-p", str(port),
                                     "-U", "weave", "-d", "postgres", "-X", "-At", "-c", "SHOW server_version_num"])
            manifest["server_version"] = version.stdout.strip()
            drivers = Path(__file__).resolve().parent
            for label, trust in (("plain", "plain"), ("tls", ca)):
                if args.phase == "all":
                    affinity(1 << client_cpus[-1])
                    run(label + "-serial", [sys.executable, "-B", str(drivers / "run.py"), "--executable", str(args.serial),
                                       "--port", str(port), "--ca", trust, "--seconds", str(args.seconds),
                                       "--repetitions", str(args.repetitions), "--workloads", "connect", "simple", "extended",
                                       "prepared", "binary", "batch", "rows", "copy", "lo_read", "lo_write", "lo_append",
                                           "--output", str(args.output / (label + "-serial.json"))], timeout=900)
                affinity(client_mask)
                description = f"Native PostgreSQL {manifest['server_version']}; server CPUs {server_cpus}; no WSL forwarding"
                run(label + "-concurrent", [sys.executable, "-B", str(drivers / "run_concurrent.py"),
                                           "--executable", str(args.concurrent), "--port", str(port), "--ca", trust,
                                           "--workers", *map(str, args.workers), "--connections", "32", "--reserve-cores", "0",
                                           "--seconds", str(args.seconds), "--repetitions", str(args.repetitions),
                                           "--server-description", description,
                                           "--output", str(args.output / (label + "-concurrent.json"))], timeout=1500)
            affinity(1 << client_cpus[-1])
            for rows in ((1, 1000, 10000) if args.phase == "all" else ()):
                for width in (16, 128, 1024):
                    pair = {}
                    for library in ("weave", "libpq"):
                        result = run(f"memory-{rows}-{width}-{library}", [str(args.serial), library, "memory", str(rows),
                                     str(width), "127.0.0.1", str(port), "plain"])
                        pair[library] = json.loads(result.stdout)
                    if pair["weave"]["checksum"] != pair["libpq"]["checksum"]:
                        raise RuntimeError("Memory measurement consumed different results")
                    manifest["memory"].append(pair)
            manifest["complete"] = True
        finally:
            affinity(server_mask)
            cleanup_errors = []
            if started or (data / "postmaster.pid").exists():
                try:
                    invoke([str(args.server_bin / "pg_ctl.exe"), "-D", str(data), "-m", "immediate", "-w", "stop"],
                           environment, args.output / "stop.log")
                except Exception as error:
                    cleanup_errors.append(str(error))
            try:
                if fixture.poll() is None:
                    fixture.stdin.write("stop\n")
                    fixture.stdin.flush()
                output, error = fixture.communicate(timeout=30)
                manifest["fixture_exit"] = fixture.returncode
                if fixture.returncode or error:
                    cleanup_errors.append(f"Certificate fixture cleanup failed: {output}\n{error}")
            except Exception as error:
                cleanup_errors.append(str(error))
            log = root / "server.log"
            if log.exists():
                args.output.joinpath("server.log").write_text(log.read_text(encoding="utf-8", errors="replace"), encoding="utf-8")
            manifest["cleanup_errors"] = cleanup_errors
            manifest["complete"] = manifest["complete"] and not cleanup_errors
            args.output.joinpath("manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            if cleanup_errors:
                raise RuntimeError("\n".join(cleanup_errors))
    if root.exists():
        raise RuntimeError("Benchmark cluster was not removed")


if __name__ == "__main__":
    main()
