"""Owned independent TLS/PG peers audit certificate policy, reset and cancellation snapshots."""

import argparse
import itertools
import json
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import subprocess
import threading


def exactly(stream, size):
    data = bytearray()
    while len(data) < size:
        part = stream.recv(size - len(data))
        if not part:
            raise EOFError("Unexpected EOF")
        data.extend(part)
    return bytes(data)


def packet(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


def message(stream):
    header = exactly(stream, 5)
    size = struct.unpack("!I", header[1:])[0]
    if not 4 <= size <= 65536:
        raise RuntimeError("Invalid frontend length")
    return header[:1], exactly(stream, size - 4)


def closure(stream):
    try:
        data = stream.recv(4096)
    except (ConnectionResetError, ConnectionAbortedError):
        return "reset"
    except ssl.SSLError as error:
        if error.reason != "UNEXPECTED_EOF_WHILE_READING":
            raise
        return "tls_eof"
    if data:
        raise RuntimeError("Closing client sent unexpected application bytes")
    return "eof"


def scenario(args, policy, identity, request, version, negotiation):
    plaintext = negotiation == "plaintext"
    available = identity in ("file", "prebuilt", "oauth")
    oauth = identity == "oauth"
    success = policy != "require" or (available and request)
    command = [args.executable, policy, identity, str(int(request)), version, negotiation]
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=environment)
    listeners, acceptors, handlers = [], [], []
    stop = threading.Event()
    lock = threading.Lock()
    counts, events, failures, paths = [0, 0], [], [], []
    output = error = ""
    try:
        paths = [process.stdout.readline().strip() for _ in range(5)]
        if not all(Path(path).is_file() for path in paths):
            raise RuntimeError("Missing owned TLS fixture")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = (
            ssl.TLSVersion.TLSv1_2 if version == "12" else ssl.TLSVersion.TLSv1_3)
        context.load_cert_chain(paths[1], paths[2])
        context.load_verify_locations(paths[0])
        context.verify_mode = ssl.CERT_OPTIONAL if request else ssl.CERT_NONE
        if negotiation == "direct":
            context.set_alpn_protocols(["postgresql"])
        for _ in range(2):
            listener = socket.socket()
            listeners.append(listener)
            listener.bind(("127.0.0.1", 0))
            listener.listen(128)
            listener.settimeout(0.1)
        process.stdin.write("".join(str(listener.getsockname()[1]) + "\n" for listener in listeners))
        process.stdin.flush()

        def handle(raw, endpoint, ordinal):
            event = {"endpoint": endpoint, "ordinal": ordinal}
            try:
                with raw:
                    raw.settimeout(10)
                    if negotiation == "postgres":
                        if exactly(raw, 8) != struct.pack("!II", 8, 80877103):
                            raise RuntimeError("Missing SSLRequest")
                        raw.sendall(b"S")
                        event["ssl_request"] = True
                    event["plaintext"] = plaintext
                    if plaintext:
                        secured = raw
                    else:
                        if raw.recv(2, socket.MSG_PEEK) != b"\x16\x03":
                            raise RuntimeError("Missing TLS record")
                        event["tls_record"] = True
                        try:
                            secured = context.wrap_socket(raw, server_side=True)
                        except (ConnectionResetError, ConnectionAbortedError, ssl.SSLError) as failure:
                            allowed = not success and not args.native
                            if isinstance(failure, ssl.SSLError):
                                allowed = allowed and failure.reason == "UNEXPECTED_EOF_WHILE_READING"
                            if not allowed:
                                raise
                            event["rejected"] = "handshake"
                            return
                    with secured:
                        event["peer_certificate"] = False
                        if not plaintext:
                            event["peer_certificate"] = bool(secured.getpeercert(binary_form=True))
                            event["version"] = secured.version()
                            event["alpn"] = secured.selected_alpn_protocol()
                        if not success and not args.native:
                            event["closure"] = closure(secured)
                            event["rejected"] = "before_startup"
                            return
                        size = struct.unpack("!I", exactly(secured, 4))[0]
                        if not 8 <= size <= 65536:
                            raise RuntimeError("Invalid Startup length")
                        startup = exactly(secured, size - 4)
                        protocol = struct.unpack("!I", startup[:4])[0]
                        if protocol == 80877102:
                            if startup[4:] != struct.pack("!II", 1234, 5678):
                                raise RuntimeError("Invalid independent cancellation snapshot")
                            event["kind"] = "cancel"
                            if args.native and not plaintext:
                                secured.unwrap().close()
                                event["closure"] = "close_notify"
                            elif args.native:
                                event["closure"] = "server_eof"
                            else:
                                event["closure"] = closure(secured)
                            return
                        if protocol != 196608 or b"user\x00weave\x00" not in startup:
                            raise RuntimeError("Invalid protected Startup")
                        event["kind"] = "startup"
                        if oauth:
                            secured.sendall(packet(b"R", struct.pack("!I", 10) + b"OAUTHBEARER\x00\x00"))
                            kind, initial = message(secured)
                            if kind != b"p" or not initial.startswith(b"OAUTHBEARER\x00"):
                                raise RuntimeError("Invalid OAuth response")
                            payload = initial[len(b"OAUTHBEARER\x00") + 4:]
                            if payload == b"n,,\x01auth=\x01\x01":
                                challenge = json.dumps({"status": "invalid_token", "scope": "read write",
                                    "openid-configuration": "https://issuer.example/tenant/.well-known/openid-configuration"}).encode()
                                secured.sendall(packet(b"R", struct.pack("!I", 11) + challenge))
                                if message(secured) != (b"p", b"\x01"):
                                    raise RuntimeError("Missing discovery acknowledgement")
                                secured.sendall(packet(b"E", b"SFATAL\x00C28000\x00Mdiscovery\x00\x00"))
                                event["kind"] = "discovery"
                                event["closure"] = closure(secured)
                                return
                            if payload != b"n,,\x01auth=Bearer abc\x01\x01":
                                raise RuntimeError("Invalid protected OAuth token")
                        secured.sendall(packet(b"R", struct.pack("!I", 0)) +
                                        packet(b"K", struct.pack("!II", 1234, 5678)) +
                                        packet(b"S", b"client_encoding\x00UTF8\x00") + packet(b"Z", b"I"))
                        if not success:
                            event["closure"] = closure(secured)
                            event["rejected"] = "authentication"
                            return
                        try:
                            kind, body = message(secured)
                            if kind != b"X" or body:
                                raise RuntimeError("Unexpected query work")
                            event["finish"] = "terminate"
                            if not args.native:
                                event["closure"] = closure(secured)
                        except (EOFError, ConnectionResetError, ConnectionAbortedError):
                            event["finish"] = "reset_closed"
            except Exception as failure:
                with lock:
                    failures.append(repr(failure))
            finally:
                with lock:
                    events.append(event)

        def accept(endpoint):
            while True:
                try:
                    raw, _ = listeners[endpoint].accept()
                except socket.timeout:
                    if stop.is_set():
                        return
                    continue
                with lock:
                    counts[endpoint] += 1
                    ordinal = counts[endpoint]
                worker = threading.Thread(target=handle, args=(raw, endpoint, ordinal))
                handlers.append(worker)
                worker.start()

        for endpoint in range(2):
            worker = threading.Thread(target=accept, args=(endpoint,))
            acceptors.append(worker)
            worker.start()
        output, error = process.communicate(timeout=120)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=10)
        stop.set()
        for worker in acceptors:
            worker.join(timeout=15)
        for worker in handlers:
            worker.join(timeout=15)
        for listener in listeners:
            listener.close()

    live_threads = sum(worker.is_alive() for worker in acceptors + handlers)
    fixture_removed = bool(paths) and not Path(paths[0]).parent.exists()
    sessions = 1 if args.native else 2
    if args.runtime and not args.native:
        layouts = 2 if os.name == "nt" else 1
        sessions += 4 * 2 * layouts
    expected = sessions * (3 if args.native else 6 if oauth else 4) if success else sessions
    report = dict(peer_library=ssl.OPENSSL_VERSION, policy=policy, identity=identity, request=request, version=version, negotiation=negotiation,
                  native=args.native, expected=expected, counts=counts, events=events, failures=failures,
                  live_threads=live_threads, fixture_removed=fixture_removed, returncode=process.returncode,
                  stdout=output, stderr=error)
    print(json.dumps(report), flush=True)
    if process.returncode or error or failures or counts != [expected, 0] or len(events) != expected:
        raise RuntimeError("Certificate peer gate failed")
    if live_threads or not fixture_removed:
        raise RuntimeError("Owned fixtures did not drain")
    match = re.fullmatch(r"Certificate (?:PostgreSQL|native): (.+)\n", output)
    if not match:
        raise RuntimeError("Missing client assertions")
    if not args.native and f", {sessions} sessions, 0 providers" not in match[1]:
        raise RuntimeError("Wrong client execution scope")
    expected_callbacks = sessions * 2 if oauth and success else 0
    if not args.native and not match[1].endswith(f", {expected_callbacks} oauth"):
        raise RuntimeError("Wrong OAuth reconnect count")
    for event in events:
        if event["endpoint"] != 0 or event.get("plaintext") != plaintext or (not plaintext and not event.get("tls_record")):
            raise RuntimeError("Wrong endpoint or transport")
        if not success:
            expected_kind = "startup" if args.native else None
            if not event.get("rejected") or event.get("kind") != expected_kind:
                raise RuntimeError("Wrong certificate failure boundary")
        if "version" in event:
            if event["version"] != ("TLSv1.2" if version == "12" else "TLSv1.3"):
                raise RuntimeError("Wrong negotiated TLS version")
            if negotiation == "direct" and event["alpn"] != "postgresql":
                raise RuntimeError("Direct TLS lost required ALPN")
    if success:
        initial = available and request and policy != "disable"
        reset = initial if args.native else available and request and policy == "disable"
        serial = sorted(events, key=lambda event: event["ordinal"])
        if args.native:
            ordered = (("startup", initial), ("startup", initial), ("cancel", initial))
        elif oauth:
            ordered = (("discovery", initial), ("startup", initial), ("discovery", reset), ("startup", reset),
                       ("cancel", initial), ("cancel", reset)) * 2
        else:
            ordered = (("startup", initial), ("startup", reset), ("cancel", initial), ("cancel", reset)) * 2
        if tuple((event.get("kind"), event.get("peer_certificate")) for event in serial[:len(ordered)]) != ordered:
            raise RuntimeError("Serial certificate reset/cancellation policy was lost")
        if sum(event.get("kind") == "cancel" for event in events) != sessions * (1 if args.native else 2):
            raise RuntimeError("Missing independent cancellation")
        if oauth and sum(event.get("kind") == "discovery" for event in events) != sessions * 2:
            raise RuntimeError("Missing pinned OAuth reconnects")
        expected_certificates = expected * initial if args.native else (expected // 2) * (initial + reset)
        if sum(event.get("peer_certificate", False) for event in events) != expected_certificates:
            raise RuntimeError("Concurrent identity policy was lost")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--plaintext-only", action="store_true")
    parser.add_argument("--oauth-only", action="store_true")
    args = parser.parse_args()
    if args.plaintext_only:
        for policy in ("disable", "allow", "require"):
            scenario(args, policy, "absent", False, "12", "plaintext")
        return
    if args.oauth_only:
        for policy, request, version, negotiation in itertools.product(
                ("disable", "allow", "require"), (False, True), ("12", "13"), ("postgres", "direct")):
            scenario(args, policy, "oauth", request, version, negotiation)
        return
    identities = ("file", "absent") if args.native else ("file", "prebuilt", "absent")
    requests = (False, True)
    if args.smoke:
        identities, requests = ("file",), (True,)
    for policy, identity, request, version, negotiation in itertools.product(
            ("disable", "allow", "require"), identities, requests, ("12", "13"), ("postgres", "direct")):
        scenario(args, policy, identity, request, version, negotiation)
    if not args.smoke:
        for request, version, negotiation in itertools.product(requests, ("12", "13"), ("postgres", "direct")):
            scenario(args, "disable", "ignored", request, version, negotiation)


if __name__ == "__main__":
    main()
