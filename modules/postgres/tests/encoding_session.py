"""Independent bounded peer for encoding metadata, admission and lifetime checks."""

import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import threading


def exact(stream, size):
    data = bytearray()
    while len(data) < size:
        part = stream.recv(size - len(data))
        if not part:
            raise EOFError
        data.extend(part)
    return bytes(data)


def message(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


def status(encoding):
    return message(b"S", b"client_encoding\0" + encoding + b"\0")


def session(stream, mode):
    stream.settimeout(15)
    size = struct.unpack("!I", exact(stream, 4))[0]
    if not 8 <= size <= 1024:
        raise RuntimeError("Invalid startup size")
    startup = exact(stream, size - 4)
    if b"client_encoding\0UTF8\0" not in startup:
        raise RuntimeError("Unexpected startup encoding")
    encoding = b"UNKNOWN" if mode == "unknown" else b"UTF8"
    stream.sendall(message(b"R", struct.pack("!I", 0)) +
                   (b"" if mode == "absent" else status(encoding)) + message(b"Z", b"I"))

    while True:
        try:
            kind = exact(stream, 1)
        except EOFError:
            return
        size = struct.unpack("!I", exact(stream, 4))[0]
        if not 4 <= size <= 65536:
            raise RuntimeError("Invalid query size")
        command = exact(stream, size - 4)
        if kind == b"X":
            if command:
                raise RuntimeError("Invalid Terminate body")
            return
        if kind != b"Q" or command[-1:] != b"\0" or b"\0" in command[:-1]:
            raise RuntimeError("Invalid query framing")
        command = command[:-1]
        response = b""
        if command == b"SET client_encoding TO 'LATIN1'":
            if mode == "cancel":
                if stream.recv(1):
                    raise RuntimeError("Expected cancelled connection to close")
                return
            if mode == "missing":
                pass
            elif mode == "wrong":
                response = status(b"WIN1252")
            elif mode == "sql_error":
                stream.sendall(message(b"E", b"SERROR\0C0A000\0MUnsupported conversion\0\0") + message(b"Z", b"I"))
                continue
            else:
                response = status(b"LATIN1")
        elif command == b"SET client_encoding TO 'UTF8'":
            response = status(b"UTF8")
        elif command == b"SET_DIRECT":
            response = status(b"SJIS")
        elif command == b"SET_UNKNOWN":
            response = status(b"UNKNOWN")
        elif command != b"NOOP":
            raise RuntimeError(f"Unexpected query: {command!r}")
        stream.sendall(response + message(b"C", b"SET\0") + message(b"Z", b"I"))


def run(executable, mode):
    errors = []
    stop = threading.Event()
    clients = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.2)

        def server():
            while not stop.is_set():
                try:
                    stream, _ = listener.accept()
                except socket.timeout:
                    continue

                def client(connection):
                    try:
                        with connection:
                            session(connection, mode)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    except Exception as error:
                        errors.append(repr(error))

                worker = threading.Thread(target=client, args=(stream,))
                clients.append(worker)
                worker.start()

        thread = threading.Thread(target=server)
        thread.start()
        try:
            command = [str(executable), str(listener.getsockname()[1]), mode]
            result = subprocess.run(command, text=True, capture_output=True, timeout=45)
        finally:
            stop.set()
            thread.join(timeout=20)
            for worker in clients:
                worker.join(timeout=20)
        if thread.is_alive() or any(worker.is_alive() for worker in clients):
            raise RuntimeError("Fixture did not stop")
    return {"mode": mode, "returncode": result.returncode, "stdout": result.stdout,
            "stderr": result.stderr, "peer_errors": errors}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=pathlib.Path, required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["normal", "missing", "wrong", "unknown", "absent", "sql_error", "cancel", "blocking"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    for mode in modes:
        result = run(args.executable, mode)
        print(json.dumps(result), flush=True)
        if result["returncode"] or result["peer_errors"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
