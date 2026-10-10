"""Independent TLS callbacks audit SNI, startup, reset, cancellation and pinned OAuth reconnects."""

import argparse
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
    output = bytearray()
    while len(output) < size:
        block = stream.recv(size - len(output))
        if not block:
            raise RuntimeError("Unexpected EOF")
        output.extend(block)
    return bytes(output)


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
        output = stream.recv(4096)
    except (ConnectionResetError, ConnectionAbortedError):
        return "reset"
    except ssl.SSLError as error:
        if error.reason != "UNEXPECTED_EOF_WHILE_READING":
            raise
        return "tls_eof"
    if output:
        raise RuntimeError("Unexpected application bytes on closing connection")
    return "eof"


def scenario(args, mode, policy, version, negotiation):
    command = [args.executable, mode, str(int(policy)), version, negotiation]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    listeners, acceptors, handlers = [], [], []
    stop = threading.Event()
    lock = threading.Lock()
    counts = [0, 0]
    observed_names = {}
    audit, errors, paths = [], [], []
    output = error = ""
    try:
        paths = [process.stdout.readline().strip() for _ in range(3)]
        if not all(Path(path).is_file() for path in paths):
            raise RuntimeError("Missing owned TLS fixtures")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = (
            ssl.TLSVersion.TLSv1_2 if version == "12" else ssl.TLSVersion.TLSv1_3)
        context.load_cert_chain(paths[1], paths[2])
        if negotiation == "direct":
            context.set_alpn_protocols(["postgresql"])

        def sni_callback(stream, name, _):
            stream.observed_sni = name
            with lock:
                observed_names[stream.getpeername()] = name

        context.set_servername_callback(sni_callback)
        for _ in range(2):
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            listener.listen(128)
            listener.settimeout(0.1)
            listeners.append(listener)
        process.stdin.write("".join(str(listener.getsockname()[1]) + "\n" for listener in listeners))
        process.stdin.flush()

        def handle(raw, endpoint, ordinal):
            event = {"endpoint": endpoint, "ordinal": ordinal}
            reject = mode in ("hostname", "untrusted")
            try:
                with raw:
                    raw.settimeout(10)
                    if negotiation == "postgres":
                        request = exactly(raw, 8)
                        if request != struct.pack("!II", 8, 80877103):
                            raise RuntimeError("Missing SSLRequest")
                        raw.sendall(b"S")
                        event["ssl_request"] = True
                    beginning = raw.recv(2, socket.MSG_PEEK)
                    if beginning != b"\x16\x03":
                        raise RuntimeError("Missing TLS record")
                    event["first_tls_bytes"] = beginning.hex()
                    address = raw.getpeername()
                    try:
                        secured = context.wrap_socket(raw, server_side=True)
                    except (ConnectionResetError, ConnectionAbortedError) as failure:
                        if not reject:
                            raise
                        with lock:
                            event["sni"] = observed_names.get(address, "not_captured")
                        event["rejected"] = True
                        event["handshake_failure"] = str(failure)
                        return
                    except ssl.SSLError as failure:
                        allowed = {"UNEXPECTED_EOF_WHILE_READING", "SSLV3_ALERT_BAD_CERTIFICATE",
                                   "TLSV1_ALERT_UNKNOWN_CA", "SSLV3_ALERT_CERTIFICATE_UNKNOWN"}
                        if not reject or failure.reason not in allowed:
                            raise
                        with lock:
                            event["sni"] = observed_names.get(address, "not_captured")
                        event["rejected"] = True
                        event["handshake_failure"] = str(failure)
                        return
                    with secured:
                        event["sni"] = secured.observed_sni
                        event["version"] = secured.version()
                        event["alpn"] = secured.selected_alpn_protocol()
                        if reject:
                            event["rejected"] = True
                            event["closure"] = closure(secured)
                            return
                        size = struct.unpack("!I", exactly(secured, 4))[0]
                        if not 8 <= size <= 65536:
                            raise RuntimeError("Invalid protected startup length")
                        startup = exactly(secured, size - 4)
                        protocol = struct.unpack("!I", startup[:4])[0]
                        if protocol == 80877102:
                            if startup[4:] != struct.pack("!II", 1234, 5678):
                                raise RuntimeError("Invalid cancellation snapshot")
                            event["kind"] = "cancel"
                            if args.native:
                                secured.unwrap().close()
                                event["closure"] = "close_notify"
                            else:
                                event["closure"] = closure(secured)
                            return
                        if protocol not in (196608, 196610) or b"user\x00weave\x00" not in startup:
                            raise RuntimeError("Invalid protected Startup")
                        event["kind"] = "startup"
                        if mode == "oauth":
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
                                raise RuntimeError("Unexpected bearer response")
                        secured.sendall(packet(b"R", struct.pack("!I", 0)) +
                                        packet(b"K", struct.pack("!II", 1234, 5678)) +
                                        packet(b"S", b"client_encoding\x00UTF8\x00") + packet(b"Z", b"I"))
                        try:
                            kind, body = message(secured)
                            if kind != b"X" or body:
                                raise RuntimeError("Unexpected query work")
                            event["finish"] = "terminate"
                            if not args.native:
                                event["closure"] = closure(secured)
                        except (ConnectionResetError, ConnectionAbortedError):
                            event["finish"] = "reset_closed"
                        except RuntimeError as failure:
                            if str(failure) != "Unexpected EOF":
                                raise
                            event["finish"] = "reset_closed"
            except Exception as failure:
                with lock:
                    errors.append(str(failure))
            finally:
                with lock:
                    audit.append(event)

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
            worker.join()
        for worker in handlers:
            worker.join()
        for listener in listeners:
            listener.close()

    sessions = 1 if args.native else 2 + ((32 if os.name == "nt" else 16) if args.runtime else 0)
    expected = sessions if mode in ("hostname", "untrusted") else sessions * (3 if args.native else 4)
    if mode == "oauth":
        expected += sessions * 2
    live_threads = sum(worker.is_alive() for worker in acceptors + handlers)
    fixture_removed = bool(paths) and not Path(paths[0]).parent.exists()
    report = dict(mode=mode, policy=policy, version=version, negotiation=negotiation, native=args.native,
                  expected=expected, counts=counts, audit=audit, errors=errors, live_threads=live_threads,
                  fixture_removed=fixture_removed, returncode=process.returncode, stdout=output, stderr=error)
    print(json.dumps(report), flush=True)
    if process.returncode or errors or error or counts != [expected, 0] or len(audit) != expected:
        raise RuntimeError("SNI peer gate failed")
    if live_threads or not fixture_removed:
        raise RuntimeError("Owned fixtures did not drain")
    if not re.fullmatch(r"SNI (native|PostgreSQL): .+\n", output):
        raise RuntimeError("Missing native/client-side assertions")

    for event in audit:
        if event["endpoint"] != 0 or event.get("first_tls_bytes") != "1603":
            raise RuntimeError("Wrong endpoint or record")
        if mode in ("hostname", "untrusted"):
            if not event.get("rejected") or event.get("kind"):
                raise RuntimeError("Rejected TLS carried application bytes")
        elif event.get("version") != ("TLSv1.2" if version == "12" else "TLSv1.3"):
            raise RuntimeError("Wrong TLS version")
        if negotiation == "direct" and not event.get("rejected") and event.get("alpn") != "postgresql":
            raise RuntimeError("Direct TLS lost mandatory ALPN")

    # Reset flips Weave's policy. Both old and current cancellation snapshots must retain theirs.
    expected_names = {"wrong.invalid" if mode == "hostname" else "localhost": expected if policy else 0,
                      None: 0 if policy else expected}
    if not args.native and mode not in ("hostname", "untrusted"):
        expected_names = {"localhost": expected // 2, None: expected // 2}
    observed = {name: sum(event.get("sni") == name for event in audit) for name in expected_names}
    if observed != expected_names:
        raise RuntimeError("Observed SNI did not match per-handshake policy")

    if mode not in ("hostname", "untrusted"):
        initial_name = "localhost" if policy else None
        reset_name = None if policy else "localhost"
        if args.native:
            ordered = (("startup", initial_name), ("startup", initial_name), ("cancel", initial_name))
        elif mode == "oauth":
            ordered = (("discovery", initial_name), ("startup", initial_name),
                       ("discovery", reset_name), ("startup", reset_name),
                       ("cancel", initial_name), ("cancel", reset_name)) * 2
        else:
            ordered = (("startup", initial_name), ("startup", reset_name),
                       ("cancel", initial_name), ("cancel", reset_name)) * 2
        serial = sorted(audit, key=lambda event: event["ordinal"])[:len(ordered)]
        if tuple((event.get("kind"), event["sni"]) for event in serial) != ordered:
            raise RuntimeError("Serial Context/blocking/native SNI policy was swapped or lost")
    if mode not in ("hostname", "untrusted"):
        cancellations = [event for event in audit if event.get("kind") == "cancel"]
        if len(cancellations) != sessions * (1 if args.native else 2):
            raise RuntimeError("Missing cancellation handshakes")
        if not args.native and sum(event["sni"] == "localhost" for event in cancellations) != sessions:
            raise RuntimeError("Cancellation policy did not survive reset")
        if mode == "oauth" and sum(event.get("kind") == "discovery" for event in audit) != sessions * 2:
            raise RuntimeError("Missing protected OAuth reconnects")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--mode")
    args = parser.parse_args()
    modes = (args.mode,) if args.mode else (("positive", "hostname", "untrusted") if args.native else
                                          ("positive", "hostname", "untrusted", "oauth"))
    for version in ("12", "13"):
        for negotiation in ("postgres", "direct"):
            for policy in (False, True):
                for mode in modes:
                    scenario(args, mode, policy, version, negotiation)


if __name__ == "__main__":
    main()
