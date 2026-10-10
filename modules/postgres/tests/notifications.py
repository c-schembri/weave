"""Owned synthetic notification controls, not deployment evidence."""
import argparse
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


def notification(payload):
    return message(b"A", struct.pack("!I", 42) + b"events\0" + payload + b"\0")


def session(stream):
    stream.settimeout(15)
    size = struct.unpack("!I", exact(stream, 4))[0]
    if not 8 <= size <= 1024:
        raise RuntimeError("Invalid startup frame bound")
    startup = exact(stream, size - 4)
    if b"user\0bad\0" in startup:
        stream.sendall(notification(b"bad startup") + message(b"E", b"SERROR\0C28P01\0Mbad login\0\0"))
        return
    ready = message(b"Z", b"I")
    complete = message(b"C", b"SELECT 1\0")
    stream.sendall(message(b"R", struct.pack("!I", 0)) + notification(b"startup") + ready)
    while True:
        kind = exact(stream, 1)
        size = struct.unpack("!I", exact(stream, 4))[0]
        if not 4 <= size <= 65536:
            raise RuntimeError("Invalid frontend bound")
        body = exact(stream, size - 4)
        if kind == b"X":
            return
        if kind == b"P":
            stream.sendall(message(b"1"))
        elif kind == b"B":
            stream.sendall(message(b"2"))
        elif kind == b"D":
            stream.sendall(message(b"n"))
        elif kind == b"E":
            stream.sendall(notification(b"pipeline") + complete)
        elif kind == b"S":
            stream.sendall(ready)
        elif kind == b"H":
            pass
        elif kind == b"Q":
            command = body.rstrip(b"\0")
            if command == b"BURST":
                burst = b"".join(notification(f"notification {index}".encode()) for index in range(8))
                stream.sendall(burst + complete + ready)
            elif command == b"ERROR":
                stream.sendall(notification(b"before error") + message(b"E", b"SERROR\0C22012\0Mdivision by zero\0\0") + ready)
            elif command == b"ARM":
                stream.sendall(complete + ready + notification(b"idle"))
            elif command == b"HOLD":
                stream.sendall(complete + ready)
            elif command == b"COPY BOTH":
                stream.sendall(notification(b"copy start") + message(b"W", b"\0\0\0"))
                chunk = notification(b"copy data") + message(b"d", b"x" * 512)
                stream.sendall(chunk + chunk + message(b"c"))
                for _ in range(2):
                    tag = exact(stream, 1)
                    length = struct.unpack("!I", exact(stream, 4))[0]
                    if tag != b"d" or length != 516 or exact(stream, length - 4) != b"y" * 512:
                        raise RuntimeError("COPY payload differs")
                if exact(stream, 5) != b"c\0\0\0\x04":
                    raise RuntimeError("COPY completion missing")
                stream.sendall(notification(b"copy end") + message(b"C", b"COPY 2\0") + ready)
            elif command == b"MALFORMED":
                stream.sendall(message(b"A", struct.pack("!I", 42) + b"events\0unterminated"))
            elif command == b"OVERSIZED":
                stream.sendall(notification(b"x" * 2048))
            elif command == b"ARM MALFORMED":
                stream.sendall(complete + ready + message(b"A", struct.pack("!I", 42) + b"events\0unterminated"))
            elif command == b"ARM OVERSIZED":
                stream.sendall(complete + ready + notification(b"x" * 2048))
            else:
                raise RuntimeError("Unexpected simple query")
        else:
            raise RuntimeError("Unexpected frontend tag")


def run(executable, mode):
    errors = []
    stop = threading.Event()
    workers = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(128)
        listener.settimeout(0.2)

        def serve():
            while not stop.is_set():
                try:
                    stream, _ = listener.accept()
                except socket.timeout:
                    continue

                def handle(client):
                    try:
                        with client:
                            session(client)
                    except (EOFError, BrokenPipeError, ConnectionResetError):
                        pass
                    except Exception as error:
                        errors.append(repr(error))

                worker = threading.Thread(target=handle, args=(stream,))
                workers.append(worker)
                worker.start()

        server = threading.Thread(target=serve)
        server.start()
        try:
            result = subprocess.run([str(executable), str(listener.getsockname()[1]), mode],
                                    capture_output=True, text=True, timeout=35)
        finally:
            stop.set()
            server.join(timeout=20)
            for worker in workers:
                worker.join(timeout=20)
        if server.is_alive() or any(worker.is_alive() for worker in workers):
            raise RuntimeError("Owned notification fixture did not drain")
    return {"mode": mode, "returncode": result.returncode, "stdout": result.stdout,
            "stderr": result.stderr, "peer_errors": errors, "connections": len(workers)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["observe", "blocking", "competing", "backlog", "MALFORMED", "OVERSIZED", "MALFORMED_WAIT", "OVERSIZED_WAIT"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    report = {"kind": "synthetic notification correctness", "complete": False, "results": []}
    for mode in modes:
        result = run(args.executable, mode)
        report["results"].append(result)
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        if result["returncode"] or result["peer_errors"]:
            raise SystemExit(1)
    report["complete"] = True
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
