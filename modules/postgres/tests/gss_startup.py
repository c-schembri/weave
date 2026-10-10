"""Owned Kerberos realm and PostgreSQL startup qualification; no global configuration changes."""
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
        result = subprocess.run(command, env=environment, input=data, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Setup timed out: {Path(command[0]).name}") from None
    if result.returncode:
        raise RuntimeError(f"Setup failed: {Path(command[0]).name}")
    return result


def free_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--transport-executable")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--delay", type=Path, required=True)
    parser.add_argument("--asan", type=Path)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--kdc", required=True)
    parser.add_argument("--admin", required=True)
    parser.add_argument("--database", required=True)
    parser.add_argument("--kinit", required=True)
    args = parser.parse_args()
    if args.output and args.output.exists():
        raise RuntimeError("Refusing to overwrite evidence")
    record = {"kind": "GSS PostgreSQL authentication and encrypted transport correctness, not performance",
              "passed": False, "steps": []}
    def checkpoint():
        if args.output:
            args.output.write_text(json.dumps(record, indent=2), encoding="utf-8")

    checkpoint()
    with tempfile.TemporaryDirectory(prefix="weave-gss-startup-") as directory:
        root = Path(directory)
        kdc_port, pg_port = free_port(), free_port()
        (root / "krb5.conf").write_text(
            "[libdefaults]\n default_realm = WEAVE.TEST\n dns_lookup_kdc = false\n dns_lookup_realm = false\n"
            ' rdns = false\n dns_canonicalize_hostname = false\n udp_preference_limit = 1\n qualify_shortname = ""\n'
            f"[realms]\n WEAVE.TEST = {{\n kdc = 127.0.0.1:{kdc_port}\n }}\n"
            "[domain_realm]\n localhost = WEAVE.TEST\n")
        (root / "kdc.conf").write_text(
            f"[kdcdefaults]\n kdc_ports = {kdc_port}\n kdc_tcp_ports = {kdc_port}\n"
            f"[realms]\n WEAVE.TEST = {{\n database_name = {root}/principal\n key_stash_file = {root}/stash\n"
            f" acl_file = {root}/acl\n }}\n")
        environment = {key: value for key, value in os.environ.items() if not key.startswith(("KRB5", "PG"))}
        environment.update(KRB5_CONFIG=str(root / "krb5.conf"), KRB5_KDC_PROFILE=str(root / "kdc.conf"),
                           KRB5CCNAME=f"FILE:{root}/client.ccache", KRB5_KTNAME=f"FILE:{root}/server.keytab")
        run([args.database, "create", "-s", "-r", "WEAVE.TEST", "-P", secrets.token_urlsafe(32)], environment)
        password = secrets.token_urlsafe(32)
        run([args.admin, "-r", "WEAVE.TEST", "-q", f"addprinc -pw {password} client@WEAVE.TEST"], environment)
        run([args.admin, "-r", "WEAVE.TEST", "-q", "addprinc -randkey postgres/localhost@WEAVE.TEST"], environment)
        run([args.admin, "-r", "WEAVE.TEST", "-q", f"ktadd -k {root}/server.keytab postgres/localhost@WEAVE.TEST"], environment)
        (root / "server.keytab").chmod(0o600)
        pg = args.server_bin
        data = root / "data"
        run([str(pg / "initdb"), "-D", str(data), "-U", "administrator", "--auth=trust", "--no-locale"], environment)
        with (data / "postgresql.conf").open("a") as configuration:
            configuration.write(f"\nport={pg_port}\nlisten_addresses='127.0.0.1'\nunix_socket_directories='{root}'\n"
                                f"krb_server_keyfile='{root}/server.keytab'\nmax_connections=100\n")
        (data / "pg_hba.conf").write_text(
            "local all all trust\nhost all client 127.0.0.1/32 gss include_realm=0 krb_realm=WEAVE.TEST\n"
            "host all administrator 127.0.0.1/32 trust\n")
        kdc_log = (root / "kdc.log").open("w")
        kdc = subprocess.Popen([args.kdc, "-n", "-r", "WEAVE.TEST"], env=environment, stdout=kdc_log, stderr=kdc_log)
        started = False
        try:
            for _ in range(100):
                if kdc.poll() is not None:
                    raise RuntimeError("Owned KDC exited")
                with socket.socket() as probe:
                    if probe.connect_ex(("127.0.0.1", kdc_port)) == 0:
                        break
                time.sleep(0.02)
            else:
                raise RuntimeError("Owned KDC did not start")
            run([args.kinit, "-f", "client@WEAVE.TEST"], environment, password + "\n")
            started = True
            run([str(pg / "pg_ctl"), "-D", str(data), "-l", str(root / "pg.log"), "-w", "start"], environment)
            run([str(pg / "psql"), "-h", str(root), "-p", str(pg_port), "-U", "administrator", "-d", "postgres",
                 "-v", "ON_ERROR_STOP=1", "-c", "CREATE ROLE client LOGIN"], environment)
            # The native libpq control proves the owned server/credentials before testing Weave.
            control = run([str(pg / "psql"), f"host=localhost hostaddr=127.0.0.1 port={pg_port} user=client "
                           "dbname=postgres gssencmode=disable sslmode=disable require_auth=gss", "-Atc", "SELECT 42"], environment)
            if control.stdout.strip() != "42":
                raise RuntimeError("libpq fixture control failed")
            if args.transport_executable:
                protected = run([str(pg / "psql"), f"host=localhost hostaddr=127.0.0.1 port={pg_port} user=client "
                                 "dbname=postgres gssencmode=require sslmode=disable require_auth=gss", "-Atc",
                                 "SELECT encrypted FROM pg_stat_gssapi WHERE pid=pg_backend_pid()"], environment)
                if protected.stdout.strip() != "t":
                    raise RuntimeError("Encrypted libpq fixture control failed")
            modes = ["normal", "default", "capture-default", "blocking", "delegate", "missing", "wrong-service", "binding", "policy",
                     "forged-ok", "duplicate", "empty", "corrupt", "oversized", "mixed", "pre-cancel", "cancel", "context-stop"]
            if args.runtime:
                modes.extend(("affine", "stealing", "saturation", "runtime-cancel"))
            if args.transport_executable:
                modes.extend(("enc-normal", "enc-hostile", "enc-fallback", "enc-no-failover", "enc-prefer", "enc-tls-priority", "enc-none", "enc-excluded",
                              "enc-missing", "enc-wrong-service", "enc-binding", "enc-policy", "enc-pre-cancel",
                              "enc-cancel", "enc-context-stop", "enc-wrap-cancel", "enc-unwrap-cancel",
                              "enc-wrap-stop", "enc-unwrap-stop"))
                if args.runtime:
                    modes.extend(("enc-saturation", "enc-runtime-cancel"))
            for mode in modes:
                child_environment = dict(environment)
                if mode.removeprefix("enc-") in ("cancel", "context-stop", "saturation", "runtime-cancel"):
                    preload = f"{args.asan}:{args.delay}" if args.asan else str(args.delay)
                    child_environment.update(LD_PRELOAD=preload, WEAVE_PROBE_DELAY="1")
                if mode in ("enc-wrap-cancel", "enc-unwrap-cancel", "enc-wrap-stop", "enc-unwrap-stop"):
                    preload = f"{args.asan}:{args.delay}" if args.asan else str(args.delay)
                    child_environment.update(LD_PRELOAD=preload, WEAVE_PROBE_RECORD_DELAY="1")
                cache = environment["KRB5CCNAME"] if mode.removeprefix("enc-") != "missing" else f"FILE:{root}/absent"
                before = time.monotonic()
                executable = args.transport_executable if mode.startswith("enc-") else args.executable
                result = subprocess.run([executable, str(pg_port), mode, cache], env=child_environment,
                                        capture_output=True, text=True, timeout=90)
                record["steps"].append({"mode": mode, "returncode": result.returncode, "stdout": result.stdout,
                                        "stderr": result.stderr, "elapsed": time.monotonic() - before})
                checkpoint()
                print(mode, result.returncode, flush=True)
                if result.returncode:
                    print(result.stdout, result.stderr, flush=True)
                    raise RuntimeError(f"{mode} failed")
            record["profiles_passed"] = True
        finally:
            record["server_log"] = (root / "pg.log").read_text() if (root / "pg.log").exists() else ""
            try:
                if started:
                    run([str(pg / "pg_ctl"), "-D", str(data), "-m", "immediate", "-w", "stop"], environment)
            finally:
                if kdc.poll() is None:
                    kdc.terminate()
                try:
                    kdc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    kdc.kill()
                    kdc.wait(timeout=10)
                kdc_log.close()
                checkpoint()
    record["passed"] = True
    checkpoint()


if __name__ == "__main__":
    main()
