"""Independent wire peers verify TLS policy, pinned retries and cancellation transport."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
import re
import socket
import ssl
import struct
import subprocess
import tempfile
import threading


MODES = ("disable", "allow", "allow-upgrade", "prefer", "prefer-n", "prefer-broken", "require", "require-n",
         "require-ca", "verify-ca", "verify-ca-untrusted", "verify-full", "verify-full-wrong",
         "prefer-invalid", "prefer-auth-failure", "allow-auth-failure", "prefer-timeout",
         "prefer-ca-untrusted", "require-password", "allow-password")
FAILED = {"require-n", "verify-ca-untrusted", "verify-full-wrong", "prefer-invalid", "prefer-auth-failure",
          "allow-auth-failure", "prefer-timeout", "prefer-ca-untrusted", "require-password", "allow-password"}
PLAINTEXT = {"disable", "allow", "prefer-n", "prefer-broken"}


def exactly(stream, length):
    output = bytearray()
    while len(output) < length:
        data = stream.recv(length - len(output))
        if not data:
            raise RuntimeError("unexpected EOF")
        output.extend(data)
    return bytes(output)


def packet(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


def scenario(executable, mode, version, sessions, native=False):
    with tempfile.TemporaryDirectory(prefix="weave-tls-mode-home-") as home:
        environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
        environment.update(HOME=home, APPDATA=home)
        process = subprocess.Popen([executable, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, env=environment)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(128)
        listener.settimeout(0.05)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = version
        paths = [process.stdout.readline().strip() for _ in range(3)]
        audits, failures = [], []
        lock = threading.Lock()
        stopped = threading.Event()

        def handle(raw):
            stream = raw
            event = {"mode": mode}
            try:
                raw.settimeout(4)
                length = struct.unpack("!I", exactly(raw, 4))[0]
                if not 8 <= length <= 65536:
                    raise RuntimeError("invalid startup length")
                header = exactly(raw, length - 4)
                if length == 8 and header == struct.pack("!I", 80877103):
                    if mode.endswith("-n") or mode == "prefer-invalid":
                        raw.sendall(b"X" if mode == "prefer-invalid" else b"N")
                        if mode in FAILED:
                            if raw.recv(1):
                                raise RuntimeError("startup after rejected negotiation")
                            event["rejected"] = True
                            return
                    elif mode == "prefer-broken":
                        raw.sendall(b"S")
                        event["broken"] = True
                        return
                    elif mode == "prefer-timeout":
                        raw.sendall(b"S")
                        while raw.recv(4096):
                            pass
                        event["timeout"] = True
                        return
                    else:
                        raw.sendall(b"S")
                        try:
                            stream = context.wrap_socket(raw, server_side=True)
                        except (ssl.SSLError, ConnectionError) as error:
                            if mode not in ("verify-full-wrong", "verify-ca-untrusted", "prefer-ca-untrusted"):
                                raise
                            event["verification_rejected"] = getattr(error, "reason", type(error).__name__)
                            return
                    try:
                        protected_length = stream.recv(4)
                    except ConnectionError:
                        if not native or mode != "verify-full-wrong":
                            raise
                        event["verification_rejected"] = "native post-handshake hostname reset"
                        return
                    if not protected_length and native and mode == "verify-full-wrong":
                        event["verification_rejected"] = "native post-handshake hostname check"
                        return
                    length = struct.unpack("!I", protected_length + exactly(stream, 4 - len(protected_length)))[0]
                    if not 8 <= length <= 65536:
                        raise RuntimeError("invalid protected startup length")
                    header = exactly(stream, length - 4)
                event["tls"] = isinstance(stream, ssl.SSLSocket)
                if header[:4] == struct.pack("!I", 80877102):
                    if header[4:] != struct.pack("!II", 1234, 5678):
                        raise RuntimeError("invalid cancellation packet")
                    event["cancel"] = True
                    if event["tls"] != (mode not in PLAINTEXT):
                        raise RuntimeError("cancellation changed session security")
                    if event["tls"]:
                        stream = stream.unwrap()
                    elif stream.recv(1):
                        raise RuntimeError("data after cancellation")
                    return
                if header[:2] != b"\x00\x03" or b"user\x00weave\x00" not in header:
                    raise RuntimeError("invalid Startup")
                if mode == "allow-upgrade" and not event["tls"]:
                    stream.sendall(packet(b"E", b"SFATAL\x00C28000\x00MTLS required\x00\x00"))
                    event["upgrade"] = True
                    return
                if mode.endswith("auth-failure"):
                    stream.sendall(packet(b"E", b"SFATAL\x00C28P01\x00Mauthentication failed\x00\x00"))
                    event["auth_failure"] = True
                    return
                if mode.endswith("-password"):
                    stream.sendall(packet(b"R", struct.pack("!I", 3)))
                    if stream.recv(1):
                        raise RuntimeError("password sent over unverified transport")
                    event["password_rejected"] = True
                    return
                stream.sendall(packet(b"R", struct.pack("!I", 0)) + packet(b"S", b"client_encoding\x00UTF8\x00") +
                               packet(b"K", struct.pack("!II", 1234, 5678)) + packet(b"Z", b"I"))
                kind = exactly(stream, 1)
                length = struct.unpack("!I", exactly(stream, 4))[0]
                if length > 65536:
                    raise RuntimeError("oversized query")
                query = exactly(stream, length - 4)
                if kind != b"Q" or query != b"SELECT 42\x00":
                    raise RuntimeError("invalid query")
                column = struct.pack("!H", 1) + b"answer\x00" + struct.pack("!IhIhih", 0, 0, 23, 4, -1, 0)
                row = struct.pack("!HI", 1, 2) + b"42"
                stream.sendall(packet(b"T", column) + packet(b"D", row) + packet(b"C", b"SELECT 1\x00") + packet(b"Z", b"I"))
                if native and event["tls"]:
                    # libpq 18 can send its TLS close_notify without Terminate on PQfinish.
                    ending = stream.recv(5)
                    if ending and ending != packet(b"X"):
                        raise RuntimeError("invalid native termination")
                    event["query"] = True
                    return
                if exactly(stream, 5) != packet(b"X"):
                    raise RuntimeError("missing terminate")
                event["query"] = True
            except Exception as error:
                with lock:
                    failures.append(str(error))
            finally:
                stream.close()
                with lock:
                    audits.append(event)

        def accept(pool):
            while not stopped.is_set():
                try:
                    raw, _ = listener.accept()
                except socket.timeout:
                    continue
                pool.submit(handle, raw)

        try:
            context.load_cert_chain(paths[1], paths[2])
            process.stdin.write(str(listener.getsockname()[1]) + "\n")
            process.stdin.flush()
            with ThreadPoolExecutor(max_workers=32) as pool:
                worker = threading.Thread(target=accept, args=(pool,))
                worker.start()
                try:
                    output, error = process.communicate(timeout=20)
                finally:
                    stopped.set()
                    worker.join(timeout=2)
            if process.returncode:
                raise RuntimeError(f"client failed: {output}\n{error}\npeer failures: {failures}\naudits: {audits}")
            if failures:
                raise RuntimeError(f"wire peer failures: {failures}")
            evidence = re.search(r"TLS modes: (\d+) sessions, (\d+) checks", output)
            if not evidence or int(evidence[1]) != sessions or int(evidence[2]) <= 0:
                raise RuntimeError("missing execution evidence")
            successful = mode not in FAILED
            if sum(bool(event.get("query")) for event in audits) != (sessions if successful else 0):
                raise RuntimeError("query evidence mismatch")
            cancellations = sessions if successful and not native else 0
            if sum(bool(event.get("cancel")) for event in audits) != cancellations:
                raise RuntimeError("cancellation evidence mismatch")
            extra = sessions if mode in ("allow-upgrade", "prefer-broken") else 0
            if len(audits) != sessions + cancellations + extra:
                raise RuntimeError("unexpected retry or missing connection")
            print(json.dumps(dict(mode=mode, version=version.name, sessions=sessions, connections=len(audits),
                                 queries=sessions if successful else 0, checks=int(evidence[2]))), flush=True)
        finally:
            listener.close()
            if process.poll() is None:
                process.kill()
                process.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--native", action="store_true")
    arguments = parser.parse_args()
    if arguments.native and arguments.runtime:
        parser.error("native control has no Weave runtime")
    sessions = 1 if arguments.native else 2 + (4 * 2 * (2 if os.name == "nt" else 1) if arguments.runtime else 0)
    modes = MODES[:13] if arguments.native else MODES
    for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
        for mode in modes:
            scenario(arguments.executable, mode, version, sessions, arguments.native)


if __name__ == "__main__":
    main()
