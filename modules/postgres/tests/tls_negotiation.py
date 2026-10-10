"""Independent SSL/PostgreSQL peers audit direct TLS across startup, reset, cancellation and OAuth."""

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

MODES = ("missing", "unrelated", "untrusted", "hostname", "reset", "cancel", "oauth", "oauth-missing")


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
        raise RuntimeError("Invalid frontend message length")
    return header[:1], exactly(stream, size - 4)


def closed(stream):
    try:
        block = stream.recv(4096)
    except ConnectionResetError:
        return "reset"
    except ssl.SSLError as error:
        if error.reason != "UNEXPECTED_EOF_WHILE_READING":
            raise
        return "tls_eof"
    if block:
        raise RuntimeError("Application bytes escaped a rejected handshake")
    return "eof"


def scenario(executable, mode, version, runtime):
    process = subprocess.Popen([executable, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    listeners = []
    workers = []
    handlers = []
    stop = threading.Event()
    audit = []
    errors = []
    counts = [0, 0]
    lock = threading.Lock()
    output = error = ""
    paths = []
    try:
        paths = [process.stdout.readline().strip() for _ in range(3)]
        contexts = []
        for selected in ("postgresql", "", "unrelated"):
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = context.maximum_version = version
            context.load_cert_chain(paths[1], paths[2])
            if selected:
                context.set_alpn_protocols([selected])
            contexts.append(context)
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
            try:
                with raw:
                    raw.settimeout(5)
                    beginning = raw.recv(2, socket.MSG_PEEK)
                    event["first_bytes"] = beginning.hex()
                    if beginning != b"\x16\x03":
                        raise RuntimeError("Direct connection did not begin with TLS")
                    reject = mode in ("missing", "unrelated", "untrusted", "hostname") or (
                        mode in ("reset", "cancel", "oauth-missing") and ordinal % 2 == 0)
                    policy_rejection = reject and mode not in ("untrusted", "hostname")
                    event["rejected"] = reject
                    context = contexts[2 if mode == "unrelated" else (1 if policy_rejection else 0)]
                    try:
                        secured = context.wrap_socket(raw, server_side=True)
                    except ssl.SSLError as error:
                        # The client rejects the negotiated policy before flushing Finished.
                        # Preserve the native observation; require the matching C++ error too.
                        if not reject or error.reason != "UNEXPECTED_EOF_WHILE_READING":
                            raise
                        event["handshake_aborted"] = str(error)
                        return
                    with secured:
                        event["tls_version"] = secured.version()
                        event["alpn"] = secured.selected_alpn_protocol()
                        if reject:
                            event["closure"] = closed(secured)
                            event["application_bytes"] = 0
                            return
                        size = struct.unpack("!I", exactly(secured, 4))[0]
                        if not 8 <= size <= 65536:
                            raise RuntimeError("Invalid protected Startup length")
                        startup = exactly(secured, size - 4)
                        if startup[:2] != b"\x00\x03" or b"user\x00weave\x00" not in startup:
                            raise RuntimeError("Invalid protected Startup")
                        event["startup"] = True
                        if mode.startswith("oauth"):
                            secured.sendall(packet(b"R", struct.pack("!I", 10) + b"OAUTHBEARER\x00\x00"))
                            kind, initial = message(secured)
                            if kind != b"p" or not initial.startswith(b"OAUTHBEARER\x00"):
                                raise RuntimeError("Invalid SASL initial response")
                            payload = initial[len(b"OAUTHBEARER\x00") + 4:]
                            if payload == b"n,,\x01auth=\x01\x01":
                                challenge = json.dumps({"status": "invalid_token", "scope": "read write",
                                    "openid-configuration": "https://issuer.example/tenant/.well-known/openid-configuration"}).encode()
                                secured.sendall(packet(b"R", struct.pack("!I", 11) + challenge))
                                if message(secured) != (b"p", b"\x01"):
                                    raise RuntimeError("Missing discovery acknowledgement")
                                secured.sendall(packet(b"E", b"SFATAL\x00C28000\x00Mdiscovery done\x00\x00"))
                                event["closure"] = closed(secured)
                                event["oauth"] = "discovery_closed"
                                return
                            if payload != b"n,,\x01auth=Bearer abc\x01\x01":
                                raise RuntimeError("Unexpected protected token")
                            event["oauth"] = "token"
                        secured.sendall(packet(b"R", struct.pack("!I", 0)) +
                                        packet(b"K", struct.pack("!II", 1234, 5678)) + packet(b"Z", b"I"))
                        try:
                            kind, body = message(secured)
                            if kind != b"X" or body:
                                raise RuntimeError("Unexpected frontend work")
                            event["finish"] = "terminate"
                        except RuntimeError as error:
                            if str(error) != "Unexpected EOF":
                                raise
                            event["finish"] = "reset_closed"
            except Exception as error:
                with lock:
                    errors.append(str(error))
            finally:
                with lock:
                    audit.append(event)

        def accept(endpoint):
            # Drain pending accepts before auditing the supposedly unused fallback.
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

        for index in range(2):
            worker = threading.Thread(target=accept, args=(index,))
            workers.append(worker)
            worker.start()
        output, error = process.communicate(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=10)
        stop.set()
        for worker in workers:
            worker.join()
        for worker in handlers:
            worker.join()
        for listener in listeners:
            listener.close()

    layouts = 2 if os.name == "nt" else 1
    sessions = 2 + (16 * layouts if runtime else 0)
    expected = sessions * 4 if mode == "oauth" else (
        4 if mode in ("reset", "cancel", "oauth-missing") else sessions)
    live_threads = sum(worker.is_alive() for worker in workers + handlers)
    fixture_removed = bool(paths) and all(not Path(path).exists() for path in paths)
    report = {"mode": mode, "version": version.name, "code": process.returncode, "counts": counts,
              "audit": sorted(audit, key=lambda event: (event["endpoint"], event["ordinal"])),
              "errors": errors, "stdout": output, "stderr": error, "runtime": runtime,
              "live_threads": live_threads, "fixture_removed": fixture_removed}
    print(json.dumps(report, indent=2), flush=True)

    if (process.returncode or counts != [expected, 0] or errors or error or len(audit) != expected
            or live_threads or not fixture_removed):
        raise RuntimeError("Direct TLS peer gate failed")
    if not re.fullmatch(rf"Direct TLS negotiation {re.escape(mode)}: \d+ checks, \d+ callbacks\n", output):
        raise RuntimeError("Missing client-side policy assertions")
    for event in audit:
        if event["first_bytes"] != "1603" or event["endpoint"] != 0:
            raise RuntimeError("Unpinned/non-direct transport")
        if event["rejected"]:
            if event.get("startup") or not (event.get("handshake_aborted") or event.get("application_bytes") == 0):
                raise RuntimeError("Rejected handshake leaked application work")
        elif not event.get("startup") or event.get("alpn") != "postgresql":
            raise RuntimeError("Missing protected PostgreSQL Startup")

    if mode == "oauth":
        if sum(event.get("oauth") == "token" for event in audit) != sessions * 2 or sum(
                event.get("oauth") == "discovery_closed" for event in audit) != sessions * 2:
            raise RuntimeError("Incomplete OAuth reconnect controls")
        if sum(event.get("finish") == "terminate" for event in audit) != sessions or sum(
                event.get("finish") == "reset_closed" for event in audit) != sessions:
            raise RuntimeError("Incomplete OAuth reset/finish controls")
    elif mode in ("reset", "cancel", "oauth-missing"):
        if sum(event["rejected"] for event in audit) != 2:
            raise RuntimeError("Incomplete reset/cancellation/reconnect rejection")
        expected_event = {"reset": ("finish", "reset_closed"), "cancel": ("finish", "terminate"),
                          "oauth-missing": ("oauth", "discovery_closed")}[mode]
        if sum(event.get(expected_event[0]) == expected_event[1] for event in audit) != 2:
            raise RuntimeError("Missing original session cleanup")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--mode", choices=MODES)
    parser.add_argument("--version", choices=("12", "13"))
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    versions = {"12": ssl.TLSVersion.TLSv1_2, "13": ssl.TLSVersion.TLSv1_3}
    for version in ((versions[args.version],) if args.version else versions.values()):
        for mode in ((args.mode,) if args.mode else MODES):
            scenario(args.executable, mode, version, args.runtime)


if __name__ == "__main__":
    main()
