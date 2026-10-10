"""Independent SCRAM verifier and fragmented PostgreSQL backend, no external database."""

import argparse
import base64
import hashlib
import hmac
import os
import socket
import ssl
import struct
import subprocess
import queue
import threading


def exact(connection, size):
    output = bytearray()
    while len(output) < size:
        value = connection.recv(size - len(output))
        if not value:
            raise EOFError("Peer closed before its message completed")
        output.extend(value)
    return bytes(output)


def request(connection):
    kind, size = struct.unpack("!cI", exact(connection, 5))
    if size < 4 or size > 65536:
        raise RuntimeError("Unbounded frontend message")
    return kind, exact(connection, size - 4)


def message(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


def authentication(connection, method, body=b""):
    connection.sendall(message(b"R", struct.pack("!I", method) + body))


def attributes(value):
    return dict(part.split(b"=", 1) for part in value.split(b","))


def backend(connection, mode, certificate, key):
    connection.settimeout(10)
    secured = mode in ("tls", "binding", "keys_tls")
    if secured:
        if exact(connection, 8) != struct.pack("!II", 8, 80877103):
            raise RuntimeError("Expected PostgreSQL SSLRequest")
        connection.sendall(b"S")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        connection = context.wrap_socket(connection, server_side=True)

    try:
        size = struct.unpack("!I", exact(connection, 4))[0]
        if size < 8 or size > 65536:
            raise RuntimeError("Invalid startup size")
        startup = exact(connection, size - 4)
        if startup[:4] != struct.pack("!I", 196610):
            raise RuntimeError("Expected protocol 3.2")

        mechanisms = b"SCRAM-SHA-256-PLUS\0SCRAM-SHA-256\0\0" if secured else b"SCRAM-SHA-256\0\0"
        authentication(connection, 10, mechanisms)
        if mode in ("deny", "exclude", "keys_deny", "keys_no_server_no_password"):
            if connection.recv(1):
                raise RuntimeError("Authentication policy rejection still produced a credential response")
            return
        kind, initial = request(connection)
        mechanism, initial = initial.split(b"\0", 1)
        first_size = struct.unpack("!I", initial[:4])[0]
        first = initial[4:]
        if kind != b"p" or first_size != len(first):
            raise RuntimeError("Malformed SASL initial response")

        gs2_end = first.index(b",,") + 2
        header, bare = first[:gs2_end], first[gs2_end:]
        if secured and mechanism != b"SCRAM-SHA-256-PLUS":
            raise RuntimeError("Client did not require channel binding")
        if mode in ("incomplete", "keys_incomplete"):
            authentication(connection, 0)
            if connection.recv(1):
                raise RuntimeError("Incomplete SCRAM handshake was accepted")
            return
        nonce = attributes(bare)[b"r"]
        salt = b"weave-independent-salt"
        server_nonce = nonce + b"server-nonce"
        if mode in ("nonce", "keys_nonce"):
            server_nonce = b"unrelated-nonce"
        iterations = 1000001 if mode in ("iterations", "keys_iterations") else 4096
        challenge = b"r=" + server_nonce + b",s=" + base64.b64encode(salt) + b",i=" + str(iterations).encode()
        if mode in ("duplicate", "keys_duplicate"):
            challenge += b",r=duplicate"
        if mode == "keys_changed_salt":
            challenge = b"r=" + server_nonce + b",s=" + base64.b64encode(b"different-salt") + b",i=8192"
        authentication(connection, 11, challenge)
        if mode in ("nonce", "iterations", "duplicate", "keys_nonce", "keys_iterations", "keys_duplicate"):
            if connection.recv(1):
                raise RuntimeError("Rejected SCRAM challenge still produced a response")
            return

        kind, final = request(connection)
        fields = attributes(final)
        binding = base64.b64decode(fields[b"c"], validate=True)
        expected_binding = header
        if secured:
            with open(certificate, encoding="ascii") as file:
                der = ssl.PEM_cert_to_DER_cert(file.read())
            expected_binding += hashlib.sha256(der).digest()
        if binding != expected_binding or fields[b"r"] != server_nonce or kind != b"p":
            raise RuntimeError("SCRAM channel binding/nonce mismatch")

        password = b"IX" if mode == "unicode" else b"pencil"
        salted = hashlib.pbkdf2_hmac("sha256", password, salt, iterations)
        client_key = hmac.digest(salted, b"Client Key", "sha256")
        stored_key = hashlib.sha256(client_key).digest()
        transcript = bare + b"," + challenge + b"," + final.rsplit(b",p=", 1)[0]
        signature = hmac.digest(stored_key, transcript, "sha256")
        expected_proof = bytes(left ^ right for left, right in zip(client_key, signature))
        proof_matches = hmac.compare_digest(base64.b64decode(fields[b"p"], validate=True), expected_proof)
        if mode == "keys_wrong_client":
            if proof_matches:
                raise RuntimeError("Wrong client key unexpectedly produced a correct proof")
            connection.sendall(message(b"E", b"SFATAL\0C28P01\0Minvalid client proof\0\0"))
            if connection.recv(1):
                raise RuntimeError("Server rejection was accepted")
            return
        if not proof_matches:
            raise RuntimeError("Client SCRAM proof failed independent verification")

        server_key = hmac.digest(salted, b"Server Key", "sha256")
        proof = hmac.digest(server_key, transcript, "sha256")
        if mode in ("proof", "binding", "keys_bad_verifier"):
            proof = bytes([proof[0] ^ 1]) + proof[1:]
        authentication(connection, 12, b"v=" + base64.b64encode(proof))
        if mode in ("proof", "binding", "keys_bad_verifier", "keys_wrong_server"):
            if connection.recv(1):
                raise RuntimeError("Bad server proof was accepted")
            return

        authentication(connection, 0)
        connection.sendall(message(b"K", struct.pack("!I", 42) + bytes(range(32))) + message(b"Z", b"I"))
        kind, query = request(connection)
        if kind != b"Q" or query != b"SELECT 42\0":
            raise RuntimeError("Unexpected query")

        columns = struct.pack("!H", 1) + b"value\0" + struct.pack("!IHIhiH", 0, 0, 23, 4, -1, 0)
        response = message(b"T", columns) + message(b"D", struct.pack("!HI", 1, 2) + b"42")
        response += message(b"C", b"SELECT 1\0") + message(b"Z", b"I")
        for offset in range(0, len(response), 3):
            connection.sendall(response[offset:offset + 3])
        if request(connection)[0] != b"X":
            raise RuntimeError("Missing Terminate")
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    modes = ("plain", "tls", "unicode", "proof", "binding", "nonce", "iterations", "duplicate",
             "allow", "deny", "exclude", "optional", "incomplete", "keys_plain", "keys_tls",
             "keys_wrong_password", "keys_client_password", "keys_server_password", "keys_parse",
             "keys_wrong_client", "keys_wrong_server", "keys_bad_verifier", "keys_nonce", "keys_iterations",
             "keys_duplicate", "keys_deny", "keys_incomplete", "keys_changed_salt", "keys_no_server_no_password")
    for mode in modes:
        process = subprocess.Popen([args.executable, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True,
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
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                listener.settimeout(10)
                process.stdin.write(str(listener.getsockname()[1]) + "\n")
                process.stdin.flush()
                connection, _ = listener.accept()
                with connection:
                    backend(connection, mode, certificate, key)
            output, error = process.communicate(timeout=10)
            if process.returncode:
                raise RuntimeError(f"{mode}: native client failed ({process.returncode}): {output} {error}")
            print(f"{mode}: independent SCRAM verification passed")
        except Exception as failure:
            if process.poll() is None:
                process.kill()
            reader.join(timeout=10)
            output, error = process.communicate(timeout=10)
            raise RuntimeError(f"{mode}: {failure}; native output: {output}; native error: {error}") from failure
        finally:
            if process.poll() is None:
                process.kill()
            reader.join(timeout=10)
            process.communicate(timeout=10)


if __name__ == "__main__":
    main()
