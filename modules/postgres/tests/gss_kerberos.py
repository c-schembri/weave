"""Owned disposable KDC, principals and credentials; no global realm/cache changes."""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time


def run(command, environment, data=None):
    try:
        done = subprocess.run(command, env=environment, input=data, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Disposable KDC setup timed out: {Path(command[0]).name}") from None
    if done.returncode:
        raise RuntimeError(f"Disposable KDC setup failed: {Path(command[0]).name}")
    return done


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--kdc", default="krb5kdc")
    parser.add_argument("--admin", default="kadmin.local")
    parser.add_argument("--database", default="kdb5_util")
    parser.add_argument("--kinit", default="kinit")
    parser.add_argument("--protection", action="store_true")
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--audit", type=Path)
    parser.add_argument("--asan", type=Path)
    args = parser.parse_args()
    if args.protection and (not args.audit or not args.audit.is_file()):
        raise RuntimeError("Protection qualification requires the native lifetime audit")
    if args.output and args.output.exists():
        raise SystemExit("Refusing to overwrite evidence")
    record = {"kind": "Native GSSAPI/Kerberos provider correctness, not PostgreSQL integration or performance",
              "steps": [], "passed": False}
    if args.protection:
        record["kind"] = "Native GSS record protection/lifetime correctness, not PostgreSQL transport or performance"
    profiles = ("kerberos", "corrupt", "proof-corrupt", "truncated", "missing", "wrong-service",
                "empty", "oversized", "handoff", "corrupt-handoff", "delegate")
    if args.protection:
        profiles = ("round-trip", "write-bound", "tamper", "truncate", "empty", "oversized",
                    "integrity-only", "replay", "gap", "retirement-context", "retirement-failure")
        if args.runtime:
            profiles += ("retirement-affine", "retirement-stealing")

    def checkpoint():
        if args.output:
            args.output.write_text(json.dumps(record, indent=2), encoding="utf-8")
    checkpoint()
    with tempfile.TemporaryDirectory(prefix="weave-gss-") as directory:
        root = Path(directory)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        configuration = root / "krb5.conf"
        configuration.write_text(
            f"[libdefaults]\n default_realm = WEAVE.TEST\n dns_lookup_kdc = false\n dns_lookup_realm = false\n"
            f" rdns = false\n dns_canonicalize_hostname = false\n udp_preference_limit = 1\n"
            ' qualify_shortname = ""\n'
            f"[realms]\n WEAVE.TEST = {{\n kdc = 127.0.0.1:{port}\n }}\n"
            f"[domain_realm]\n localhost = WEAVE.TEST\n", encoding="utf-8")
        profile = root / "kdc.conf"
        profile.write_text(
            f"[kdcdefaults]\n kdc_ports = {port}\n kdc_tcp_ports = {port}\n"
            f"[realms]\n WEAVE.TEST = {{\n database_name = {root}/principal\n key_stash_file = {root}/stash\n"
            f" acl_file = {root}/acl\n }}\n", encoding="utf-8")
        environment = {name: value for name, value in os.environ.items() if not name.startswith("KRB5")}
        environment.update(KRB5_CONFIG=str(configuration), KRB5_KDC_PROFILE=str(profile),
                           KRB5CCNAME=f"FILE:{root}/client.ccache", KRB5_KTNAME=f"FILE:{root}/server.keytab")
        run([args.database, "create", "-s", "-r", "WEAVE.TEST", "-P", secrets.token_urlsafe(32)], environment)
        password = secrets.token_urlsafe(32)
        run([args.admin, "-r", "WEAVE.TEST", "-q", f"addprinc -pw {password} client@WEAVE.TEST"], environment)
        run([args.admin, "-r", "WEAVE.TEST", "-q", "addprinc -randkey postgres/localhost@WEAVE.TEST"], environment)
        run([args.admin, "-r", "WEAVE.TEST", "-q", f"ktadd -k {root}/server.keytab postgres/localhost@WEAVE.TEST"], environment)
        (root / "server.keytab").chmod(0o600)
        log = (root / "kdc.log").open("w", encoding="utf-8")
        process = subprocess.Popen([args.kdc, "-n", "-r", "WEAVE.TEST"], env=environment, stdout=log, stderr=log)
        try:
            ready = False
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("Owned KDC exited")
                with socket.socket() as probe:
                    probe.settimeout(0.1)
                    if probe.connect_ex(("127.0.0.1", port)) == 0:
                        ready = True
                        break
                time.sleep(0.02)
            if not ready:
                raise RuntimeError("Owned KDC did not start")
            run([args.kinit, "-f", "client@WEAVE.TEST"], environment, password + "\n")
            for mode in profiles:
                cache = environment["KRB5CCNAME"] if mode != "missing" else f"FILE:{root}/absent"
                command = [args.executable, mode, cache]
                trace = root / (mode + ".trace")
                environment["KRB5_TRACE"] = str(trace)
                start = time.monotonic()
                client_environment = environment.copy()
                if args.protection:
                    preload = [str(args.asan)] if args.asan else []
                    preload.append(str(args.audit))
                    client_environment["LD_PRELOAD"] = ":".join(preload)
                done = subprocess.run(command, env=client_environment, capture_output=True, text=True, timeout=30)
                record["steps"].append({"mode": mode, "command": command, "returncode": done.returncode,
                                        "elapsed": time.monotonic()-start, "stdout": done.stdout, "stderr": done.stderr,
                                        "trace": trace.read_text() if trace.exists() else ""})
                checkpoint()
                print(mode, done.returncode, flush=True)
                if done.returncode:
                    print(done.stdout, done.stderr, flush=True)
                    raise RuntimeError(f"{mode} failed")
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
            log.close()
    record["passed"] = True
    checkpoint()


if __name__ == "__main__":
    main()
