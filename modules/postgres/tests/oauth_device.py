"""Independent stdlib HTTPS IdP with strict device-flow request/timing oracles."""
import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import ssl
import subprocess
import sys
import threading
import time
from urllib.parse import parse_qs, unquote_plus


def run(args):
    process = subprocess.Popen([args.executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        published = queue.Queue()
        reader = threading.Thread(target=lambda: published.put(tuple(process.stdout.readline().strip()
                                   for _ in range(3))), daemon=True)
        reader.start()
        ca, certificate, key = published.get(timeout=10)
        if not all((ca, certificate, key)):
            _, error = process.communicate(timeout=10)
            raise RuntimeError("No fixture certificates: " + error)
        errors = queue.Queue()
        lock = threading.Lock()
        polls = 0
        previous = None
        gaps = []
        requests = []

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *ignored):
                pass

            def reply(self, status, value):
                data = json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass

            def do_GET(self):
                requests.append(self.path)
                try:
                    assert self.path == "/" + args.mode + "/.well-known/openid-configuration"
                    assert not self.headers.get("Authorization")
                    self.reply(200, {"issuer": issuer if args.mode != "mismatch" else issuer + "-evil",
                        "device_authorization_endpoint": base + "/device", "token_endpoint": base + "/token",
                        "grant_types_supported": ["urn:ietf:params:oauth:grant-type:device_code"],
                        "token_endpoint_auth_methods_supported": ["client_secret_post" if args.mode in ("post", "request_post")
                                                                  else "client_secret_basic"]})
                except BaseException as error:
                    errors.put(repr(error))
                    self.reply(500, {"error": "fixture"})

            def do_POST(self):
                nonlocal polls, previous
                requests.append(self.path)
                try:
                    length = int(self.headers["Content-Length"])
                    assert 0 <= length <= 65536
                    assert self.headers["Content-Type"] == "application/x-www-form-urlencoded"
                    body = parse_qs(self.rfile.read(length).decode(), strict_parsing=True, keep_blank_values=True)
                    assert all(len(values) == 1 for values in body.values())
                    authorization = self.headers.get("Authorization")
                    if args.mode in ("basic", "request_basic", "request_override", "concurrent_secret", "live"):
                        assert authorization.startswith("Basic ")
                        value = base64.b64decode(authorization[6:], validate=True).decode()
                        client, secret = value.split(":", 1)
                        assert unquote_plus(client) == "client:/ +&=" and unquote_plus(secret) == "s:e c+/&="
                        assert "client_id" not in body and "client_secret" not in body
                    else:
                        assert not authorization and body.pop("client_id") == ["client:/ +&="]
                        if args.mode in ("post", "request_post"):
                            assert body.pop("client_secret") == ["s:e c+/&="]
                    if self.path == "/device":
                        assert body == {"scope": ["read write"]}
                        if args.mode == "stale":
                            time.sleep(1.3)
                        self.reply(200, {"device_code": "code:/ +&=", "user_code": "TEST-1234",
                            "verification_uri": base + "/approve" + ("#device" if args.mode == "fragment" else ""),
                            "verification_uri_complete": base + "/approve?user_code=TEST-1234" +
                                ("#device" if args.mode == "fragment" else ""),
                            "expires_in": 1 if args.mode == "stale" else 2 if args.mode == "expiry" else 60,
                            "interval": 1})
                    else:
                        assert self.path == "/token"
                        assert body == {"grant_type": ["urn:ietf:params:oauth:grant-type:device_code"],
                                        "device_code": ["code:/ +&="]}
                        with lock:
                            polls += 1
                            number = polls
                            now = time.monotonic()
                            if previous is not None:
                                gaps.append(now - previous)
                            previous = now
                        if args.mode == "timeout_retry" and number == 1:
                            time.sleep(1.5)
                        error = {"denied": "access_denied", "expired": "expired_token"}.get(args.mode)
                        if args.mode in ("cancel", "expiry"):
                            error = "authorization_pending"
                        if args.mode == "pending" and number == 1:
                            error = "authorization_pending"
                        if args.mode == "slow" and number == 1:
                            error = "slow_down"
                        self.reply(400 if error else 200, {"error": error} if error else
                                   {"access_token": "abc", "token_type": "Bearer", "expires_in": 600})
                except BaseException as error:
                    errors.put(repr(error))
                    self.reply(500, {"error": "fixture"})

        class Server(ThreadingHTTPServer):
            # The fixture must admit a 32-client wave rather than become its bottleneck.
            request_queue_size = 128

        server = Server(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.set_alpn_protocols(["http/1.1"])
        context.load_cert_chain(certificate, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        base = "https://127.0.0.1:" + str(server.server_port)
        issuer = base + "/" + args.mode
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        database = None
        try:
            input_text = issuer + "\n" + args.mode + "\n"
            if args.postgres:
                helper = Path(__file__).with_name("oauth_device_server.py")
                command = [sys.executable, str(helper)]
                if os.name == "nt":
                    if not args.wsl_fixture:
                        raise ValueError("Windows real-server controls require an explicit --wsl-fixture path")
                    command = ["wsl", "-d", "Ubuntu", "--exec", "python3", args.wsl_fixture]
                command += ["--issuer", issuer, "--ca", ca, "--certificate", certificate, "--key", key]
                command += ["--server-bin", args.server_bin, "--validator", args.validator]
                if os.name == "nt":
                    command.append("--windows-client")
                database = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, text=True)
                ready = queue.Queue()
                reader = threading.Thread(target=lambda: ready.put(tuple(database.stdout.readline().strip()
                                             for _ in range(2))), daemon=True)
                reader.start()
                port, address = ready.get(timeout=90)
                assert port.isdecimal() and address, "Owned PostgreSQL failed to publish its endpoint"
                input_text += port + "\n" + address + "\n"
            output, error = process.communicate(input_text, timeout=120)
            if process.returncode:
                raise RuntimeError(f"client {process.returncode}: requests={len(requests)} polls={polls}\n{output}\n{error}")
            assert errors.empty(), list(errors.queue)
            if args.postgres:
                expected = 132 if args.runtime else 4
                assert polls == expected and len(requests) == expected * 3, (polls, len(requests))
            elif args.mode == "request_none" or args.mode.startswith("missing_"):
                assert len(requests) == 0 and polls == 0
            elif args.mode == "mismatch":
                assert len(requests) == 1 and polls == 0
            elif args.mode in ("stale", "handler_error", "prompt_cancel", "shutdown"):
                assert len(requests) == 2 and polls == 0
            elif args.mode in ("concurrent", "concurrent_secret"):
                assert polls == 64 and len(requests) == 192
            elif args.mode in ("pending", "slow", "timeout_retry"):
                assert polls == 2
                minimum = 6 if args.mode == "slow" else 2 if args.mode == "timeout_retry" else 1
                assert gaps[0] >= minimum - 0.02, gaps
            else:
                assert polls == 1
            print(output.strip(), "requests=", len(requests), "poll_gaps=", gaps[:3])
        finally:
            try:
                if database is not None:
                    _, database_error = database.communicate("stop\n", timeout=90)
                    if process.returncode:
                        print(database_error, file=sys.stderr)
                    if database.returncode:
                        raise RuntimeError("Owned PostgreSQL failed: " + database_error)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)


def main():
    modes = ("none", "basic", "post", "pending", "slow", "timeout_retry", "denied", "expired", "expiry", "cancel",
             "mismatch", "fragment", "stale", "handler_error", "prompt_cancel", "shutdown", "concurrent",
             "request_basic", "request_post", "request_override", "request_none", "missing_basic", "missing_post",
             "concurrent_secret")
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--mode", choices=modes + ("live",))
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--postgres", action="store_true")
    parser.add_argument("--server-bin")
    parser.add_argument("--validator")
    parser.add_argument("--wsl-fixture")
    args = parser.parse_args()
    if args.postgres:
        if not args.server_bin or not args.validator:
            parser.error("--postgres requires --server-bin and --validator")
        args.mode = "live"
        run(args)
    else:
        selected = (args.mode,) if args.mode else modes
        for mode in selected:
            if mode in ("concurrent", "concurrent_secret") and not args.runtime:
                continue
            args.mode = mode
            run(args)


if __name__ == "__main__":
    main()
