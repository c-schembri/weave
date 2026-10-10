"""Owned Linux Kerberos/PostgreSQL control: accepted GSS encryption takes priority over direct TLS."""

import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time


def run(arguments, environment, data=None):
    result = subprocess.run(arguments, env=environment, input=data, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"Owned fixture command failed: {Path(arguments[0]).name}: {result.stderr}")
    return result


def port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--kdc", required=True)
    parser.add_argument("--admin", required=True)
    parser.add_argument("--database", required=True)
    parser.add_argument("--kinit", required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    pg = args.server_bin
    kdc_port, pg_port = port(), port()
    with tempfile.TemporaryDirectory(prefix="weave-direct-gss-") as directory:
        root = Path(directory).resolve()
        data = root / "data"
        configuration = root / "krb5.conf"
        profile = root / "kdc.conf"
        configuration.write_text(
            "[libdefaults]\n default_realm = WEAVE.TEST\n dns_lookup_kdc = false\n dns_lookup_realm = false\n"
            " rdns = false\n dns_canonicalize_hostname = false\n udp_preference_limit = 1\n"
            ' qualify_shortname = ""\n'
            f"[realms]\n WEAVE.TEST = {{\n kdc = 127.0.0.1:{kdc_port}\n }}\n"
            "[domain_realm]\n localhost = WEAVE.TEST\n", encoding="utf-8")
        profile.write_text(
            f"[kdcdefaults]\n kdc_ports = {kdc_port}\n kdc_tcp_ports = {kdc_port}\n"
            f"[realms]\n WEAVE.TEST = {{\n database_name = {root}/principal\n key_stash_file = {root}/stash\n"
            f" acl_file = {root}/acl\n }}\n", encoding="utf-8")
        environment = {key: value for key, value in os.environ.items()
                       if not key.upper().startswith(("PG", "KRB5"))}
        environment.update(KRB5_CONFIG=str(configuration), KRB5_KDC_PROFILE=str(profile),
                           KRB5CCNAME=f"FILE:{root}/client.ccache", KRB5_KTNAME=f"FILE:{root}/server.keytab")
        run([args.database, "create", "-s", "-r", "WEAVE.TEST", "-P", secrets.token_urlsafe(32)], environment)
        password = secrets.token_urlsafe(32)
        run([args.admin, "-r", "WEAVE.TEST", "-q", f"addprinc -pw {password} client@WEAVE.TEST"], environment)
        run([args.admin, "-r", "WEAVE.TEST", "-q", "addprinc -randkey postgres/localhost@WEAVE.TEST"], environment)
        run([args.admin, "-r", "WEAVE.TEST", "-q",
             f"ktadd -k {root}/server.keytab postgres/localhost@WEAVE.TEST"], environment)
        (root / "server.keytab").chmod(0o600)
        run([str(pg / "initdb"), "-D", str(data), "-U", "administrator", "--auth=trust", "--no-locale"], environment)
        with (data / "postgresql.conf").open("a", encoding="utf-8") as file:
            file.write(f"\nport={pg_port}\nlisten_addresses='127.0.0.1'\nunix_socket_directories='{root}'\n"
                       f"krb_server_keyfile='{root}/server.keytab'\nmax_connections=100\nssl=off\n")
        (data / "pg_hba.conf").write_text(
            "local all all trust\nhostgssenc all client 127.0.0.1/32 gss include_realm=0 krb_realm=WEAVE.TEST\n",
            encoding="utf-8")
        log = (root / "kdc.log").open("w", encoding="utf-8")
        kdc = subprocess.Popen([args.kdc, "-n", "-r", "WEAVE.TEST"], env=environment, stdout=log, stderr=log)
        started = False
        report = {"passed": False, "runtime": args.runtime, "server_tls": False}
        try:
            for _ in range(100):
                if kdc.poll() is not None:
                    raise RuntimeError("Owned KDC exited")
                with socket.socket() as probe:
                    probe.settimeout(0.1)
                    if probe.connect_ex(("127.0.0.1", kdc_port)) == 0:
                        break
                time.sleep(0.02)
            else:
                raise RuntimeError("Owned KDC did not start")
            run([args.kinit, "-f", "client@WEAVE.TEST"], environment, password + "\n")
            run([str(pg / "pg_ctl"), "-D", str(data), "-l", str(root / "pg.log"), "-w", "start"], environment)
            started = True
            run([str(pg / "psql"), "-h", str(root), "-p", str(pg_port), "-U", "administrator", "-d", "postgres",
                 "-v", "ON_ERROR_STOP=1", "-c", "CREATE ROLE client LOGIN"], environment)
            controls = []
            policies = ("prefer", "require")
            for mode in policies:
                settings = (f"host=localhost hostaddr=127.0.0.1 port={pg_port} user=client dbname=postgres "
                            f"gssencmode={mode} sslnegotiation=direct sslmode=verify-full require_auth=gss")
                result = run([str(pg / "psql"), settings, "-Atc",
                              "SELECT encrypted FROM pg_stat_gssapi WHERE pid=pg_backend_pid()"], environment)
                if result.stdout.strip() != "t":
                    raise RuntimeError("Native direct TLS/GSS priority fixture control failed")
                controls.append({"mode": mode, "encrypted": True})
            client = subprocess.run([args.executable, str(pg_port), environment["KRB5CCNAME"]],
                                    env=environment, capture_output=True, text=True, timeout=90)
            report.update(code=client.returncode, stdout=client.stdout, stderr=client.stderr, native=controls)
            if client.returncode or client.stderr:
                raise RuntimeError("Direct TLS/GSS priority probe failed")
            report["passed"] = True
        finally:
            report["server_log"] = (root / "pg.log").read_text() if (root / "pg.log").exists() else ""
            try:
                if started or (data / "postmaster.pid").exists():
                    assert data.resolve().parent == root
                    run([str(pg / "pg_ctl"), "-D", str(data), "-m", "immediate", "-w", "stop"], environment)
            finally:
                if kdc.poll() is None:
                    kdc.terminate()
                try:
                    kdc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    kdc.kill()
                    kdc.wait(timeout=10)
                log.close()
                report["kdc_log"] = (root / "kdc.log").read_text()
                print(json.dumps(report, indent=2), flush=True)

    cleanup = {"fixture_removed": not root.exists(), "kdc_drained": kdc.poll() is not None}
    print(json.dumps({"cleanup": cleanup}, indent=2), flush=True)
    if not all(cleanup.values()):
        raise RuntimeError("Owned GSS priority fixture did not drain and clean up")


if __name__ == "__main__":
    main()
