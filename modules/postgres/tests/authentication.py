"""Independent authentication-policy and protocol negotiation backend, without libpq."""

import argparse
import hashlib
import socket
import ssl
import struct
import subprocess
import os
import queue
import threading


def exact(connection, size):
    output = bytearray()
    while len(output) < size:
        chunk = connection.recv(size - len(output))
        if not chunk:
            raise EOFError("Incomplete frontend message")
        output.extend(chunk)
    return bytes(output)


def request(connection):
    kind, size = struct.unpack("!cI", exact(connection, 5))
    if not 4 <= size <= 65536:
        raise RuntimeError("Unbounded frontend message")
    return kind, exact(connection, size - 4)


def message(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


def auth(connection, method, body=b""):
    connection.sendall(message(b"R", struct.pack("!I", method) + body))


def closed(connection):
    if connection.recv(1):
        raise RuntimeError("Rejected challenge produced a frontend message")


def backend(connection, mode, certificate, key, secured, expected, version, requested):
    connection.settimeout(5)
    if secured:
        if exact(connection, 8) != struct.pack("!II", 8, 80877103):
            raise RuntimeError("Expected SSLRequest")
        connection.sendall(b"S")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        connection = context.wrap_socket(connection, server_side=True)
    try:
        size = struct.unpack("!I", exact(connection, 4))[0]
        if not 8 <= size <= 65536:
            raise RuntimeError("Unbounded startup packet")
        startup = exact(connection, size - 4)
        if startup[:4] != struct.pack("!I", requested):
            raise RuntimeError("Wrong requested protocol")
        if mode.startswith("version"):
            minor = 0 if mode not in ("version-one", "version-future", "version-upgrade") else {
                "version-one": 1, "version-future": 3, "version-upgrade": 2}[mode]
            negotiation = message(b"v", struct.pack("!II", minor, 0))
            if mode == "version-options":
                negotiation = message(b"v", struct.pack("!II", 0, 1) + b"_pq_.unknown\0")
            elif mode == "version-short":
                negotiation = message(b"v", struct.pack("!I", 0))
            connection.sendall(negotiation)
            if mode == "version-duplicate":
                connection.sendall(negotiation)
            if expected != "success":
                closed(connection)
                return
        if mode.startswith("md5") or mode.startswith("password"):
            password = mode.startswith("password")
            salt = b"salt"
            auth(connection, 3 if password else 5, b"" if password else salt)
            if mode.endswith("reject"):
                closed(connection)
                return
            kind, response = request(connection)
            inner = hashlib.md5(b"pencilweave").hexdigest().encode()
            wanted = b"pencil\0" if password else b"md5" + hashlib.md5(inner + salt).hexdigest().encode() + b"\0"
            if kind != b"p" or response != wanted:
                raise RuntimeError("Password response failed independent verification")
            if mode.endswith("repeat") or mode.endswith("switch"):
                next_method = (5 if password else 3) if mode.endswith("switch") else (3 if password else 5)
                auth(connection, next_method, salt if next_method == 5 else b"")
                closed(connection)
                return
        elif mode == "sasl-unstarted":
            auth(connection, 11, b"r=nonce,s=c2FsdA==,i=4096")
            closed(connection)
            return
        elif mode == "gss-reject":
            auth(connection, 7)
            closed(connection)
            return
        elif mode == "late-version":
            auth(connection, 10, b"SCRAM-SHA-256\0\0")
            request(connection)
            connection.sendall(message(b"v", struct.pack("!II", 0, 0)))
            closed(connection)
            return
        auth(connection, 0)
        if mode == "authenticated-version":
            connection.sendall(message(b"v", struct.pack("!II", 0, 0)))
            closed(connection)
            return
        if mode.endswith("reject"):
            closed(connection)
            return
        cancel = bytes(range(32)) if version == 196610 else b"salt"
        if mode == "bad-key":
            cancel = bytes(range(32))
        connection.sendall(message(b"K", struct.pack("!I", 42) + cancel) + message(b"Z", b"I"))
        if expected != "success":
            closed(connection)
            return
        if request(connection) != (b"Q", b"SELECT 42\0"):
            raise RuntimeError("Unexpected query")
        columns = struct.pack("!H", 1) + b"value\0" + struct.pack("!IHIhiH", 0, 0, 23, 4, -1, 0)
        response = message(b"T", columns) + message(b"D", struct.pack("!HI", 1, 2) + b"42")
        connection.sendall(response + message(b"C", b"SELECT 1\0") + message(b"Z", b"I"))
        if request(connection)[0] != b"X":
            raise RuntimeError("Missing Terminate")
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    # mode, options, expected error, method, effective version, transport, weak opt-in
    cases = (
        ("trust", "", "success", 0, 196610, False, False),
        ("trust", "require_auth=none", "success", 0, 196610, False, False),
        ("trust-reject", "require_auth=scram-sha-256", "authentication", 0, 196610, False, False),
        ("trust-reject", "require_auth=!none", "authentication", 0, 196610, False, False),
        ("trust", "require_auth=!gss,!sspi,!oauth", "success", 0, 196610, False, False),
        ("md5-reject", "", "unsupported", 2, 196610, False, False),
        ("md5", "require_auth=md5", "success", 2, 196610, False, True),
        ("md5-reject", "require_auth=!md5", "authentication", 2, 196610, False, True),
        ("md5-reject", "require_auth=scram-sha-256", "authentication", 2, 196610, False, True),
        ("md5-repeat", "", "protocol", 2, 196610, False, True),
        ("md5-switch", "", "protocol", 2, 196610, False, True),
        ("password-reject", "", "unsupported", 1, 196610, True, False),
        ("password", "require_auth=password", "success", 1, 196610, True, True),
        ("password-reject", "require_auth=!password", "authentication", 1, 196610, True, True),
        ("password-reject", "require_auth=password", "unsupported", 1, 196610, False, True),
        ("password-repeat", "", "protocol", 1, 196610, True, True),
        ("password-switch", "", "protocol", 1, 196610, True, True),
        ("trust", "max_protocol_version=3.0", "success", 0, 196608, False, False),
        ("version-down", "", "success", 0, 196608, False, False),
        ("version-reject", "min_protocol_version=3.2", "version", 0, 196608, False, False),
        ("version-one", "", "protocol", 0, 196610, False, False),
        ("version-future", "", "protocol", 0, 196610, False, False),
        ("version-upgrade", "max_protocol_version=3.0", "protocol", 0, 196608, False, False),
        ("version-duplicate", "", "protocol", 0, 196608, False, False),
        ("version-options", "", "protocol", 0, 196608, False, False),
        ("version-short", "", "protocol", 0, 196608, False, False),
        ("late-version", "", "protocol", 3, 196610, False, False),
        ("authenticated-version", "", "protocol", 0, 196610, False, False),
        ("bad-key", "max_protocol_version=3.0", "protocol", 0, 196608, False, False),
        ("sasl-unstarted", "", "protocol", 3, 196610, False, False),
        ("gss-reject", "", "unsupported", 4, 196610, False, False),
    )
    for mode, options, expected, method, version, secured, weak in cases:
        process = subprocess.Popen([args.executable, expected, str(method), str(version), "allow" if weak else "deny"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        lines = queue.Queue()

        def read_fixture():
            lines.put(tuple(process.stdout.readline().strip() for _ in range(3)))

        reader = threading.Thread(target=read_fixture, daemon=True)
        reader.start()
        try:
            ca, certificate, key = lines.get(timeout=10)
            if not ca or not certificate or not key:
                raise RuntimeError("Certificate fixture failed")
            with socket.socket() as listener, socket.socket() as fallback:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                listener.settimeout(5)
                fallback.bind(("127.0.0.1", 0))
                fallback.listen()
                fallback.setblocking(False)
                config = (f"host=127.0.0.1,127.0.0.1 port={listener.getsockname()[1]},{fallback.getsockname()[1]} "
                          "user=weave password=pencil ")
                config += ("sslmode=verify-full " if secured else "sslmode=disable ") + options
                process.stdin.write(config + "\n")
                process.stdin.flush()
                connection, _ = listener.accept()
                requested = 196608 if "max_protocol_version=3.0" in options else 196610
                backend(connection, mode, certificate, key, secured, expected, version, requested)
                output, error = process.communicate(timeout=5)
                try:
                    unexpected, _ = fallback.accept()
                except BlockingIOError:
                    pass
                else:
                    unexpected.close()
                    raise RuntimeError("Unexpected host failover after authentication/protocol failure")
            if process.returncode:
                raise RuntimeError(f"Client failed ({process.returncode}): {output} {error}")
            print(f"{mode} {options}: passed")
        except Exception as failure:
            if process.poll() is None:
                process.kill()
            reader.join(timeout=10)
            output, error = process.communicate(timeout=5)
            raise RuntimeError(f"{mode} {options}: {failure}; {output} {error}") from failure
        finally:
            if process.poll() is None:
                process.kill()
            reader.join(timeout=10)
            process.communicate(timeout=5)


if __name__ == "__main__":
    main()
