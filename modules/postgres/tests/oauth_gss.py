"""Owned Kerberos/PostgreSQL and independent HTTPS IdP; no global state changes."""
import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import secrets
import shutil
import signal
import socket
import ssl
import subprocess
import tempfile
import threading
import time
from urllib.parse import parse_qs, unquote_plus


def run(command, environment, data=None):
    result = subprocess.run(command, env=environment, input=data, capture_output=True, text=True, timeout=30)
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
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--server-bin", type=Path, required=True)
    parser.add_argument("--kdc", required=True)
    parser.add_argument("--admin", required=True)
    parser.add_argument("--database", required=True)
    parser.add_argument("--kinit", required=True)
    args = parser.parse_args()
    if args.output and args.output.exists():
        raise RuntimeError("Refusing to overwrite evidence")
    record = {"complete": False, "kind": "Linux native GSS/OAuth correctness, not benchmarks or real IdP"}
    with tempfile.TemporaryDirectory(prefix="weave-oauth-gss-") as directory:
        root = Path(directory).resolve()
        data = root / "data"
        kdc_port, port = free_port(), free_port()
        while kdc_port == port:
            port = free_port()
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
        kdc_log = (root / "kdc.log").open("w")
        kdc = subprocess.Popen([args.kdc, "-n", "-r", "WEAVE.TEST"], env=environment,
                               stdout=kdc_log, stderr=kdc_log)
        client = None
        server = None
        thread = None
        started = False
        pg = args.server_bin.resolve()
        requests = []
        polls = 0
        errors = queue.Queue()
        lock = threading.Lock()
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
            password = None
            client_environment = dict(environment)
            client_environment.pop("LSAN_OPTIONS", None)
            client_environment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
            client = subprocess.Popen([args.executable], env=client_environment, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            published = queue.Queue()
            reader = threading.Thread(target=lambda: published.put(tuple(client.stdout.readline().strip()
                                      for _ in range(3))), daemon=True)
            reader.start()
            ca, certificate, key = published.get(timeout=10)
            reader.join(timeout=5)
            assert not reader.is_alive(), "Certificate reader drained"
            if not all((ca, certificate, key)):
                raise RuntimeError("Client did not publish its owned HTTPS certificates")

            class Handler(BaseHTTPRequestHandler):
                protocol_version = "HTTP/1.1"

                def setup(self):
                    super().setup()
                    self.connection.settimeout(5)

                def log_message(self, *ignored):
                    pass

                def reply(self, status, value):
                    body = json.dumps(value).encode()
                    self.send_response(status)
                    self.send_header("Content-Length", str(len(body)))
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(body)

                def do_GET(self):
                    with lock:
                        requests.append(self.path)
                    try:
                        assert self.path == "/tenant/.well-known/openid-configuration"
                        assert not self.headers.get("Authorization")
                        self.reply(200, {"issuer": issuer, "device_authorization_endpoint": base + "/device",
                            "token_endpoint": base + "/token", "grant_types_supported":
                            ["urn:ietf:params:oauth:grant-type:device_code"],
                            "token_endpoint_auth_methods_supported": ["client_secret_basic"]})
                    except BaseException as error:
                        errors.put(repr(error))
                        self.reply(500, {"error": "fixture"})

                def do_POST(self):
                    nonlocal polls
                    with lock:
                        requests.append(self.path)
                    try:
                        length = int(self.headers["Content-Length"])
                        assert 0 <= length <= 65536
                        assert self.headers["Content-Type"] == "application/x-www-form-urlencoded"
                        body = parse_qs(self.rfile.read(length).decode(), strict_parsing=True, keep_blank_values=True)
                        authorization = self.headers["Authorization"]
                        assert authorization.startswith("Basic ")
                        encoded = base64.b64decode(authorization[6:], validate=True).decode()
                        client_id, secret = encoded.split(":", 1)
                        assert unquote_plus(client_id) == "client:/ +&=" and unquote_plus(secret) == "s:e c+/&="
                        if self.path == "/device":
                            assert body == {"scope": ["read write"]}
                            self.reply(200, {"device_code": "code:/ +&=", "user_code": "TEST-1234",
                                "verification_uri": base + "/approve", "expires_in": 60, "interval": 1})
                        else:
                            assert self.path == "/token"
                            assert body == {"grant_type": ["urn:ietf:params:oauth:grant-type:device_code"],
                                            "device_code": ["code:/ +&="]}
                            with lock:
                                polls += 1
                            self.reply(200, {"access_token": "abc", "token_type": "Bearer"})
                    except BaseException as error:
                        errors.put(repr(error))
                        self.reply(500, {"error": "fixture"})

            class Idp(ThreadingHTTPServer):
                request_queue_size = 128

            server = Idp(("127.0.0.1", 0), Handler)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.set_alpn_protocols(["http/1.1"])
            context.load_cert_chain(certificate, key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            base = "https://127.0.0.1:" + str(server.server_port)
            issuer = base + "/tenant"
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            run([str(pg / "initdb"), "-D", str(data), "-U", "administrator", "--auth=trust", "--no-locale"], environment)
            shutil.copyfile(args.validator, root / "weave_oauth_validator.so")
            with (data / "postgresql.conf").open("a") as configuration:
                configuration.write(f"\nport={port}\nlisten_addresses='127.0.0.1'\nunix_socket_directories='{root}'\n"
                    f"krb_server_keyfile='{root}/server.keytab'\nmax_connections=160\nssl=off\n"
                    f"dynamic_library_path='{root}'\noauth_validator_libraries='weave_oauth_validator'\n")
            (data / "pg_hba.conf").write_text("local all all trust\n"
                f'hostgssenc all weave 127.0.0.1/32 oauth issuer="{issuer}" scope="read write" validator=weave_oauth_validator\n'
                "hostgssenc all client 127.0.0.1/32 gss include_realm=0 krb_realm=WEAVE.TEST\n")
            started = True
            run([str(pg / "pg_ctl"), "-D", str(data), "-l", str(root / "pg.log"), "-w", "start"], environment)
            run([str(pg / "psql"), "-h", str(root), "-p", str(port), "-U", "administrator", "-d", "postgres",
                "-v", "ON_ERROR_STOP=1", "-c", "CREATE ROLE client LOGIN; CREATE ROLE weave LOGIN;"], environment)
            control = run([str(pg / "psql"), f"host=localhost hostaddr=127.0.0.1 port={port} user=client dbname=postgres "
                "gssencmode=require sslmode=disable require_auth=gss", "-Atc",
                "SELECT encrypted FROM pg_stat_gssapi WHERE pid=pg_backend_pid()"], environment)
            assert control.stdout.strip() == "t", "Independent libpq fixture encryption control"
            output, error = client.communicate(f"{port}\n{issuer}\n{environment['KRB5CCNAME']}\n", timeout=180)
            record.update(returncode=client.returncode, stdout=output, stderr=error,
                          idp_requests=len(requests), idp_polls=polls, fixture_gss_control=control.stdout.strip())
            print(output, error, flush=True)
            assert client.returncode == 0, "Combined client failed"
            assert errors.empty(), list(errors.queue)
            expected = 132 if args.runtime else 4
            assert polls == expected and len(requests) == expected * 3, (polls, len(requests))
            control = subprocess.run([args.baseline], env=environment, input=f"{port}\n{issuer}\n",
                                     capture_output=True, text=True, timeout=30)
            record["baseline"] = {"returncode": control.returncode, "stdout": control.stdout,
                                  "stderr": control.stderr}
            print(control.stdout, control.stderr, flush=True)
            assert control.returncode == 0, "Independent libpq control failed"
            record["complete"] = True
        except BaseException as error:
            record["failure"] = type(error).__name__
            raise
        finally:
            if client is not None:
                if client.poll() is None:
                    client.kill()
                client.wait(timeout=10)
            if server is not None:
                server.shutdown()
                server.server_close()
            if thread is not None:
                thread.join(timeout=5)
            record["server_log"] = (root / "pg.log").read_text() if (root / "pg.log").exists() else ""
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
                kdc_log.close()
                if args.output:
                    args.output.write_text(json.dumps(record, indent=2) + "\n")


def stop(signum, frame):
    raise SystemExit(143)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, stop)
    main()
