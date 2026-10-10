"""Owned synthetic protocol controls; no performance measurements."""
import argparse
import hashlib
import json
import os
from pathlib import Path
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


def receive(stream):
    kind = exact(stream, 1)
    size = struct.unpack("!I", exact(stream, 4))[0]
    if not 4 <= size <= 256 * 1024:
        raise RuntimeError("Invalid client frame bound")
    return kind, exact(stream, size - 4)


def session(stream):
    stream.settimeout(15)
    size = struct.unpack("!I", exact(stream, 4))[0]
    if not 8 <= size <= 1024:
        raise RuntimeError("Invalid startup frame")
    startup = exact(stream, size - 4)
    if b"user\0bad\0" in startup:
        stream.sendall(message(b"E", b"SERROR\0C28P01\0Mbad login\0\0"))
        return
    stream.sendall(message(b"R", struct.pack("!I", 5) + b"salt"))
    kind, response = receive(stream)
    digest = hashlib.md5(b"trace-passwordweave").hexdigest().encode()
    expected = b"md5" + hashlib.md5(digest + b"salt").hexdigest().encode() + b"\0"
    if kind != b"p" or response != expected:
        raise RuntimeError("Actual MD5 exchange differs")
    ready = message(b"Z", b"I")
    complete = message(b"C", b"SELECT 1\0")
    stream.sendall(message(b"R", struct.pack("!I", 0)) + message(b"K", b"PID!KEY!") + ready)
    while True:
        kind, body = receive(stream)
        if kind == b"X":
            return
        if kind == b"P":
            stream.sendall(message(b"1"))
        elif kind == b"B":
            stream.sendall(message(b"2"))
        elif kind == b"D":
            stream.sendall(message(b"n"))
        elif kind == b"E":
            stream.sendall(complete)
        elif kind == b"S":
            stream.sendall(ready)
        elif kind == b"H":
            pass
        elif kind == b"Q":
            if body == b"COPY BOTH\0":
                stream.sendall(message(b"W", b"\0\0\0"))
                chunks = message(b"d", b"x" * (128 * 1024))
                failures = []

                def write():
                    try:
                        stream.sendall(chunks + chunks + message(b"c"))
                    except Exception as error:
                        failures.append(repr(error))

                writer = threading.Thread(target=write)
                writer.start()
                try:
                    for _ in range(2):
                        tag, data = receive(stream)
                        if tag != b"d" or data != b"y" * (128 * 1024):
                            raise RuntimeError("COPY payload differs")
                    if receive(stream) != (b"c", b""):
                        raise RuntimeError("COPY send completion missing")
                finally:
                    writer.join(timeout=15)
                if writer.is_alive() or failures:
                    raise RuntimeError("COPY writer did not finish")
                stream.sendall(message(b"C", b"COPY 2\0") + ready)
            elif body == b"MALFORMED\0":
                stream.sendall(b"C" + struct.pack("!I", 3))
                return
            elif body == b"CANCEL\0":
                if stream.recv(1):
                    raise RuntimeError("Cancelled session sent more bytes")
                return
            else:
                stream.sendall(complete + ready)
        else:
            raise RuntimeError("Unexpected client message " + repr(kind))


def run(executable, mode):
    errors = []
    stop = threading.Event()
    clients = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(128)
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
                            session(connection)
                    except (EOFError, BrokenPipeError, ConnectionResetError):
                        pass
                    except Exception as error:
                        errors.append(repr(error))

                worker = threading.Thread(target=client, args=(stream,))
                clients.append(worker)
                worker.start()

        thread = threading.Thread(target=server)
        thread.start()
        try:
            result = subprocess.run([str(executable), str(listener.getsockname()[1]), mode],
                                    text=True, capture_output=True, timeout=40)
        finally:
            stop.set()
            thread.join(timeout=20)
            for worker in clients:
                worker.join(timeout=20)
        if thread.is_alive() or any(worker.is_alive() for worker in clients):
            raise RuntimeError("Owned fixture did not drain")
    return {"mode": mode, "returncode": result.returncode, "stdout": result.stdout,
            "stderr": result.stderr, "peer_errors": errors}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["context", "blocking"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    results = []
    for mode in modes:
        result = run(args.executable, mode)
        results.append(result)
        if args.output:
            args.output.write_text(json.dumps({"kind": "controlled tracing correctness", "results": results}, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        if result["returncode"] or result["peer_errors"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
