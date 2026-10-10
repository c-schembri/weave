"""Opt-in owned HTTPS Keycloak/PostgreSQL deployment with independent device approval."""
import argparse
from html.parser import HTMLParser
import hashlib
import http.cookiejar
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
import tarfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


def run(command, environment, data=None, timeout=90):
    child = subprocess.Popen(command, env=environment, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        output, error = child.communicate(data, timeout=timeout)
        if child.returncode:
            print(error[-2000:])
            raise RuntimeError("Setup failed: " + Path(command[0]).name)
        return subprocess.CompletedProcess(command, child.returncode, output, error)
    finally:
        if child.poll() is None:
            stop_group(child)


def stop_group(child):
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        child.wait(timeout=30)
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=10)
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    for _ in range(30):
        try:
            os.killpg(child.pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.1)
    raise RuntimeError("Owned process group failed to drain")


def free_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def extract(archive, expected, parent):
    with archive.open("rb") as source:
        assert hashlib.file_digest(source, "sha256").hexdigest() == expected, "Release digest mismatch"
    with tarfile.open(archive) as document:
        members = document.getmembers()
        directory = members[0].name.split("/", 1)[0]
        assert directory and all(member.name == directory or member.name.startswith(directory + "/") for member in members)
        document.extractall(parent, filter="data")
    return parent / directory


class Forms(HTMLParser):
    def __init__(self):
        super().__init__()
        self.forms = []
        self.current = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "form":
            self.current = {"action": attrs.get("action", ""), "fields": {}, "buttons": []}
            self.forms.append(self.current)
        if self.current is not None and tag in ("input", "button") and attrs.get("name"):
            name = attrs["name"]
            value = attrs.get("value", "")
            if attrs.get("type") == "hidden":
                self.current["fields"][name] = value
            elif tag == "button" or attrs.get("type") == "submit":
                self.current["buttons"].append((name, value))

    def handle_endtag(self, tag):
        if tag == "form":
            self.current = None


class PinnedRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self, origin):
        self.origin = origin

    def redirect_request(self, request, fp, code, message, headers, new_url):
        parsed = urllib.parse.urlsplit(new_url)
        if parsed.scheme + "://" + parsed.netloc != self.origin:
            raise RuntimeError("Device approval redirect escaped the owned IdP")
        return super().redirect_request(request, fp, code, message, headers, new_url)


def approve(uri, context, origin, password, deny=False):
    assert uri.startswith(origin + "/realms/weave/")
    browser = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context),
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()), PinnedRedirect(origin))
    response = browser.open(uri, timeout=10)
    for _ in range(8):
        page = response.read(1024 * 1024).decode()
        forms = Forms()
        forms.feed(page)
        if "Device Login Successful" in page or "Successfully authenticated" in page:
            assert not deny, "Denied device flow became successful"
            return
        if "Device Login Failed" in page:
            assert deny, "Approved device flow was denied"
            return
        assert forms.forms, "No approval form or success state"
        form = forms.forms[0]
        action = urllib.parse.urljoin(response.url, form["action"])
        parsed = urllib.parse.urlsplit(action)
        assert parsed.scheme + "://" + parsed.netloc == origin, "Form action escaped the owned IdP"
        fields = form["fields"]
        if "kc-form-login" in page:
            fields.update(username="weave", password=password)
        else:
            buttons = dict(form["buttons"])
            selected = "cancel" if deny else "accept"
            assert selected in buttons, ("Unknown approval form", tuple(buttons))
            fields[selected] = buttons[selected]
        response = browser.open(urllib.request.Request(action, urllib.parse.urlencode(fields).encode()), timeout=10)
    raise RuntimeError("Device approval did not complete")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--validator", type=Path, required=True)
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--output", type=Path)
    output.add_argument("--output-dir", type=Path)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--tools", type=Path, required=True)
    parser.add_argument("--server-bin", type=Path, required=True)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        args.output = args.output_dir / f"keycloak-{os.getpid()}-{time.time_ns()}.json"
    assert not args.output.exists(), "Refusing to overwrite evidence"
    tools = args.tools.resolve()
    pg = args.server_bin.resolve()
    record = {"complete": False, "scope": "Owned real Keycloak device flow and server introspection, not a security audit"}
    with tempfile.TemporaryDirectory(prefix="weave-keycloak-") as directory:
        root = Path(directory).resolve()
        record["owned_root"] = str(root)
        data = root / "data"
        distribution = None
        port, idp_port = free_port(), free_port()
        while port == idp_port:
            idp_port = free_port()
        origin = f"https://127.0.0.1:{idp_port}"
        issuer = origin + "/realms/weave"
        secret, password, database_password = (secrets.token_urlsafe(32) for _ in range(3))
        secret += ": +/&="
        environment = {key: value for key, value in os.environ.items() if not key.startswith(("PG", "KC_", "JAVA_", "WEAVE_KEYCLOAK"))}
        environment["JAVA_OPTS_APPEND"] = "-Xms128m -Xmx512m -XX:ActiveProcessorCount=4"
        client = None
        keycloak = None
        started = False
        logs = None
        client_errors = None
        events = queue.Queue()
        reader = None
        certificate_directory = None
        passed = False
        try:
            distribution = extract(tools / "keycloak-26.8.0.tar.gz",
                "9e41da899f838a58cd510fc98ed4f7cadc715aed5683e42aca20a0c9a2a3980a", root)
            jre = extract(tools / "OpenJDK21U-jre_x64_linux_hotspot_21.0.12.1_1.tar.gz",
                "2413149700df0f7d440500a84a8f764c535f21e5a5e87d38328b64eec2c5b500", root)
            environment["JAVA_HOME"] = str(jre)
            version = run([str(jre / "bin/java"), "-version"], environment)
            assert "21.0.12.1" in version.stderr, "Pinned JRE version"
            record["jre_version"] = version.stderr
            client_environment = dict(environment)
            client_environment.pop("LSAN_OPTIONS", None)
            client_environment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
            client_errors = (root / "client.stderr").open("w")
            client = subprocess.Popen([args.executable], env=client_environment,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=client_errors, text=True)

            def read_events():
                for line in client.stdout:
                    events.put(line.strip())
                events.put(None)

            reader = threading.Thread(target=read_events)
            reader.start()
            ca, certificate, key = (events.get(timeout=10) for _ in range(3))
            assert all((ca, certificate, key)), "Client did not publish its owned TLS identity"
            certificate_directory = Path(ca).resolve().parent
            assert certificate_directory.parent == Path(tempfile.gettempdir()).resolve()
            assert certificate_directory.name.startswith("weave-tls-")
            assert all(Path(source).resolve().parent == certificate_directory for source in (certificate, key))
            certificate_copies = ((ca, "ca.pem"), (certificate, "server.pem"), (key, "server.key"))
            for source, name in certificate_copies:
                shutil.copyfile(source, root / name)
            (root / "server.key").chmod(0o600)
            run(["openssl", "verify", "-x509_strict", "-CAfile", str(root / "ca.pem"), str(root / "server.pem")], environment)
            record["strict_certificate_chain"] = True
            environment.update(WEAVE_KEYCLOAK_ISSUER=issuer, WEAVE_KEYCLOAK_SECRET=secret,
                WEAVE_KEYCLOAK_INTROSPECTION=issuer + "/protocol/openid-connect/token/introspect",
                WEAVE_KEYCLOAK_CA=str(root / "ca.pem"))
            run([str(pg / "initdb"), "-D", str(data), "-U", "administrator", "--auth=trust", "--no-locale", "--encoding=UTF8"], environment)
            shutil.copyfile(args.validator, root / "weave_keycloak_validator.so")
            with (data / "postgresql.conf").open("a") as configuration:
                configuration.write(f"\nport={port}\nlisten_addresses='127.0.0.1'\nunix_socket_directories='{root}'\n"
                    f"ssl=on\nssl_cert_file='{root}/server.pem'\nssl_key_file='{root}/server.key'\n"
                    f"dynamic_library_path='{root}'\noauth_validator_libraries='weave_keycloak_validator'\n")
            (data / "pg_hba.conf").write_text("local all all trust\n"
                "hostssl all keycloak 127.0.0.1/32 scram-sha-256\n"
                f'hostssl all all 127.0.0.1/32 oauth issuer="{issuer}" scope="openid profile" validator=weave_keycloak_validator\n')
            started = True
            run([str(pg / "pg_ctl"), "-D", str(data), "-l", str(root / "pg.log"), "-w", "start"], environment)
            run([str(pg / "psql"), "-h", str(root), "-p", str(port), "-U", "administrator", "-d", "postgres",
                "-v", "ON_ERROR_STOP=1"], environment,
                f"CREATE ROLE keycloak LOGIN PASSWORD '{database_password}';\n"
                "CREATE DATABASE keycloak OWNER keycloak;\nCREATE ROLE weave LOGIN;\nCREATE ROLE other LOGIN;\n")

            (distribution / "data/import").mkdir(parents=True, exist_ok=True)
            username = {
                "name": "username", "protocol": "openid-connect",
                "protocolMapper": "oidc-usermodel-property-mapper",
                "config": {
                    "user.attribute": "username", "claim.name": "preferred_username",
                    "jsonType.label": "String", "introspection.token.claim": "true",
                    "access.token.claim": "true", "id.token.claim": "true", "userinfo.token.claim": "true",
                },
            }
            pg_audience = {
                "name": "pg-audience", "protocol": "openid-connect", "protocolMapper": "oidc-audience-mapper",
                "config": {
                    "included.custom.audience": "weave-postgres", "introspection.token.claim": "true",
                    "access.token.claim": "true", "id.token.claim": "false",
                },
            }
            introspection_audience = {
                "name": "introspection-client", "protocol": "openid-connect", "protocolMapper": "oidc-audience-mapper",
                "config": {
                    "included.client.audience": "weave-pg", "introspection.token.claim": "true",
                    "access.token.claim": "true", "id.token.claim": "false",
                },
            }
            client_scopes = [
                {
                    "name": "profile", "protocol": "openid-connect",
                    "attributes": {"include.in.token.scope": "true"}, "protocolMappers": [username],
                },
                {
                    "name": "pg-audience", "protocol": "openid-connect",
                    "attributes": {"include.in.token.scope": "true"}, "protocolMappers": [pg_audience],
                },
            ]
            realm = {
                "realm": "weave", "enabled": True, "sslRequired": "all", "registrationAllowed": False,
                "oauth2DeviceCodeLifespan": 60, "oauth2DevicePollingInterval": 1, "accessTokenLifespan": 10,
                "clientScopes": client_scopes,
                "users": [{
                    "username": "weave", "enabled": True, "emailVerified": True,
                    "firstName": "Test", "lastName": "User", "email": "weave@example.invalid",
                    "credentials": [{"type": "password", "value": password, "temporary": False}],
                }],
                "clients": [{
                    "clientId": "weave-pg", "enabled": True, "protocol": "openid-connect", "publicClient": False,
                    "secret": secret, "standardFlowEnabled": True,
                    "directAccessGrantsEnabled": False, "fullScopeAllowed": False,
                    "attributes": {"oauth2.device.authorization.grant.enabled": "true"},
                    "defaultClientScopes": [], "optionalClientScopes": ["profile", "pg-audience"],
                    "protocolMappers": [introspection_audience],
                }],
            }
            realm_path = distribution / "data/import/weave-realm.json"
            realm_path.write_text(json.dumps(realm))
            realm_path.chmod(0o600)
            keycloak_environment = dict(environment)
            keycloak_environment.update(KC_DB="postgres", KC_DB_USERNAME="keycloak", KC_DB_PASSWORD=database_password,
                KC_DB_URL=f"jdbc:postgresql://127.0.0.1:{port}/keycloak?sslmode=verify-full&sslrootcert={root}/ca.pem")
            run([str(distribution / "bin/kc.sh"), "build", "--db=postgres"], keycloak_environment, timeout=120)
            logs = (root / "keycloak.log").open("w")
            keycloak = subprocess.Popen([str(distribution / "bin/kc.sh"), "start", "--optimized", "--import-realm",
                "--hostname=" + origin, "--http-host=127.0.0.1", "--http-enabled=false", "--https-port=" + str(idp_port),
                "--https-certificate-file=" + str(root / "server.pem"), "--https-certificate-key-file=" + str(root / "server.key"),
                "--db-pool-max-size=16", "--cache=local"], env=keycloak_environment, stdout=logs, stderr=logs,
                start_new_session=True)
            context = ssl.create_default_context(cafile=ca)
            discovery = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context))
            metadata = None
            last_discovery_error = None
            startup_deadline = time.monotonic() + 90
            while time.monotonic() < startup_deadline:
                if keycloak.poll() is not None:
                    raise RuntimeError("Owned Keycloak exited during startup")
                try:
                    with discovery.open(issuer + "/.well-known/openid-configuration", timeout=1) as response:
                        metadata = json.load(response)
                    break
                except (urllib.error.URLError, TimeoutError) as error:
                    if isinstance(getattr(error, "reason", None), ssl.SSLCertVerificationError):
                        raise RuntimeError("Independent HTTPS client rejected the owned certificate chain") from error
                    last_discovery_error = str(error)
                    time.sleep(0.5)
            record["discovery_error"] = last_discovery_error if not metadata else None
            assert metadata and metadata["issuer"] == issuer, "Real discovery metadata"
            deployment = (root / "keycloak.log").read_text()
            assert "Keycloak 26.8.0" in deployment and "Profile prod activated" in deployment
            record["keycloak_version"] = "26.8.0"
            record["production_profile"] = True
            client.stdin.write(f"{issuer}\n{port}\n{secret}\n")
            client.stdin.flush()
            approvals = 0
            denials = 0
            pending = 0
            messages = []
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                try:
                    message = events.get(timeout=1)
                except queue.Empty:
                    continue
                if message is None:
                    break
                if message.startswith("APPROVE "):
                    approve(message[8:], context, origin, password)
                    approvals += 1
                elif message.startswith("DENY "):
                    approve(message[5:], context, origin, password, deny=True)
                    denials += 1
                elif message.startswith("WAIT "):
                    pending += 1
                else:
                    messages.append(message)
                    print(message, flush=True)
            client.wait(timeout=10)
            error = (root / "client.stderr").read_text()
            record.update(
                returncode=client.returncode, messages=messages, approvals=approvals,
                denials=denials, pending=pending, stderr=error,
            )
            print(error, flush=True)
            expected_approvals = 77 if args.runtime else 13
            assert client.returncode == 0, "Real native client failed"
            assert approvals == expected_approvals and denials == 1 and pending == 1, "Real native control counts"
            expected_native = expected_approvals + 2
            assert messages == [f"Real Keycloak controls passed: native={expected_native} cache=7"]
            passed = True
        except BaseException as error:
            record["failure"] = type(error).__name__
            raise
        finally:
            if client is not None:
                if client.poll() is None:
                    client.kill()
                client.wait(timeout=10)
            if reader is not None:
                reader.join(timeout=5)
                assert not reader.is_alive(), "Owned client output reader failed to drain"
            if client_errors is not None:
                client_errors.close()
                record["stderr"] = (root / "client.stderr").read_text()
            if keycloak is not None:
                stop_group(keycloak)
            if logs is not None:
                logs.close()
            record["keycloak_log"] = (root / "keycloak.log").read_text() if (root / "keycloak.log").exists() else ""
            record["postgres_log"] = (root / "pg.log").read_text() if (root / "pg.log").exists() else ""
            if started or (data / "postmaster.pid").exists():
                assert data.resolve().parent == root
                run([str(pg / "pg_ctl"), "-D", str(data), "-m", "immediate", "-w", "stop"], environment)
            if certificate_directory is not None:
                certificate_files = ("ca.pem", "other-ca.pem", "server.pem", "expired.pem", "key.pem", "client.pem", "client-key.pem")
                for name in certificate_files:
                    (certificate_directory / name).unlink(missing_ok=True)
                if certificate_directory.exists():
                    certificate_directory.rmdir()
            record["owned_services_drained"] = True
            record["complete"] = passed
            evidence = json.dumps(record, indent=2)
            for credential in (secret, password, database_password):
                evidence = evidence.replace(credential, "[redacted]")
            args.output.write_text(evidence + "\n")


def stop(signum, frame):
    raise SystemExit(143)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, stop)
    main()
