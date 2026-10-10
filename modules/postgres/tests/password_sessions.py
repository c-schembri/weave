"""Independent password peer: shape, encoding, policy, tracing and pending cancellation."""
import argparse
import base64
import hashlib
import hmac
import json
import socket
import struct
import subprocess
import threading


def exact(stream, count):
    value = bytearray()
    while len(value) < count:
        part = stream.recv(count - len(value))
        if not part:
            raise EOFError
        value.extend(part)
    return bytes(value)


def message(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


def query(stream):
    kind = exact(stream, 1)
    size = struct.unpack("!I", exact(stream, 4))[0]
    if not 4 <= size <= 65536:
        raise RuntimeError("Unbounded query")
    body = exact(stream, size - 4)
    if kind == b"X" and not body:
        return None
    if kind != b"Q" or body[-1:] != b"\0" or b"\0" in body[:-1]:
        raise RuntimeError("Invalid query frame")
    return body[:-1]


def verify(verifier, user):
    password = b"~" * 127
    if verifier.startswith(b"md5"):
        if verifier != b"md5" + hashlib.md5(password + user).hexdigest().encode():
            raise RuntimeError("Incorrect MD5 password change")
        return
    algorithm, parameters, keys = verifier.split(b"$")
    count, encoded = parameters.split(b":")
    salt = base64.b64decode(encoded, validate=True)
    stored, server = (base64.b64decode(value, validate=True) for value in keys.split(b":"))
    if algorithm != b"SCRAM-SHA-256" or count != b"4096" or len(salt) != 16:
        raise RuntimeError("Invalid SCRAM password change")
    salted = hashlib.pbkdf2_hmac("sha256", password, salt, 4096)
    if not hmac.compare_digest(stored, hashlib.sha256(hmac.digest(salted, b"Client Key", "sha256")).digest()):
        raise RuntimeError("Incorrect stored key")
    if not hmac.compare_digest(server, hmac.digest(salted, b"Server Key", "sha256")):
        raise RuntimeError("Incorrect server key")


def session(stream, mode, events):
    stream.settimeout(15)
    length = struct.unpack("!I", exact(stream, 4))[0]
    if not 8 <= length <= 4096:
        raise RuntimeError("Unbounded startup")
    exact(stream, length - 4)
    encoding = b"SJIS" if mode == "quote_sjis" else b"UNKNOWN" if mode == "unknown_encoding" else b"UTF8"
    metadata = b"" if mode == "missing_encoding" else message(b"S", b"client_encoding\0" + encoding + b"\0")
    ready = message(b"Z", b"I")
    stream.sendall(message(b"R", struct.pack("!I", 0)) + metadata + ready)
    user = b'test'
    if mode == "quote_utf8":
        user = b'na\xc3\xafve"role'
    if mode == "quote_sjis":
        user = b'\x83\x5c"role'
    seen_policy = False
    seen_change = False
    while True:
        try:
            text = query(stream)
        except EOFError:
            if mode not in {"no_rows", "two_rows", "null", "binary", "two_columns", "no_columns",
                            "cancel_show", "cancel_alter"}:
                raise RuntimeError("Unexpected terminal connection")
            events.append("closed")
            return
        if text is None:
            events.append("terminate")
            return
        if text == b"SHOW password_encryption":
            if seen_policy or seen_change or mode.startswith("explicit") or mode == "limit":
                raise RuntimeError("Unintended policy query")
            seen_policy = True
            events.append("policy")
            if mode == "cancel_show":
                stream.sendall(message(b"N", b"SNOTICE\0C00000\0MPENDING\0\0"))
                continue
            columns = 0 if mode == "no_columns" else 2 if mode == "two_columns" else 1
            field = b"password_encryption\0" + struct.pack("!IhIhih", 0, 0, 25, -1, -1, 1 if mode == "binary" else 0)
            response = message(b"T", struct.pack("!h", columns) + field * columns)
            algorithm = b"md5" if mode == "normal_md5" else b"rot13" if mode == "unknown_algorithm" else b"scram-sha-256"
            cell = struct.pack("!i", -1) if mode == "null" else struct.pack("!i", len(algorithm)) + algorithm
            row = message(b"D", struct.pack("!h", columns) + cell * columns)
            if mode != "no_rows":
                response += row * (2 if mode == "two_rows" else 1)
            if mode == "policy_encoding":
                response += message(b"S", b"client_encoding\0UNKNOWN\0")
            stream.sendall(response + message(b"C", b"SHOW\0") + ready)
        elif text.startswith(b"ALTER USER "):
            if seen_change or mode in {"missing_encoding", "unknown_encoding", "policy_encoding", "unknown_algorithm", "limit"}:
                raise RuntimeError("Unexpected password command")
            if not seen_policy and not mode.startswith("explicit"):
                raise RuntimeError("Missing automatic policy query")
            seen_change = True
            quoted_user = b'"' + user.replace(b'"', b'""') + b'"'
            expected = b"ALTER USER " + quoted_user + b" PASSWORD '"
            if not text.startswith(expected) or text[-1:] != b"'":
                raise RuntimeError("Incorrect quoted user")
            verify(text[len(expected):-1], user)
            events.append("verified_change")
            if mode == "cancel_alter":
                stream.sendall(message(b"N", b"SNOTICE\0C00000\0MPENDING\0\0"))
                continue
            if mode == "sql_error":
                stream.sendall(message(b"E", b"SERROR\0C42501\0MPermission denied\0\0") + ready)
            else:
                stream.sendall(message(b"C", b"ALTER ROLE\0") + ready)
        elif text == b"NOOP":
            if mode in {"cancel_show", "cancel_alter"}:
                raise RuntimeError("Busy query incorrectly reached peer")
            events.append("reuse")
            stream.sendall(message(b"C", b"SELECT 1\0") + ready)
        else:
            raise RuntimeError("Unexpected command")


def run(executable, mode):
    errors, events = [], []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(20)

        def serve():
            try:
                stream, _ = listener.accept()
                with stream:
                    session(stream, mode, events)
            except Exception as error:
                errors.append(repr(error))

        worker = threading.Thread(target=serve)
        worker.start()
        try:
            result = subprocess.run([executable, str(listener.getsockname()[1]), mode],
                                    capture_output=True, text=True, timeout=30)
        finally:
            worker.join(timeout=20)
        if worker.is_alive():
            raise RuntimeError("Password peer did not drain")
    return {"mode": mode, "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
            "peer_errors": errors, "events": events}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    modes = ["normal_scram", "normal_md5", "explicit_scram", "explicit_md5", "quote_utf8", "quote_sjis",
             "unknown_algorithm", "no_rows", "two_rows", "null", "binary", "two_columns", "no_columns",
             "missing_encoding", "unknown_encoding", "policy_encoding", "limit", "sql_error", "cancel_show", "cancel_alter"]
    for mode in modes:
        result = run(args.executable, mode)
        print(json.dumps(result), flush=True)
        if result["returncode"] or result["peer_errors"] or "Password session controls passed:" not in result["stdout"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
