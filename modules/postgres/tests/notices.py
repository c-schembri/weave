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


def notice(text, state=b"00000"):
    return b"SNOTICE\0VNOTICE\0C" + state + b"\0M" + text + b"\0\0"


def session(stream):
    stream.settimeout(15)
    size = struct.unpack("!I", exact(stream, 4))[0]
    if size < 8 or size > 1024:
        raise RuntimeError("Invalid startup size")
    startup = exact(stream, size - 4)
    if b"user\0bad\0" in startup:
        stream.sendall(message(b"N", notice(b"startup")) +
                       message(b"E", b"SERROR\0C28P01\0Mbad login\0\0"))
        return
    stream.sendall(message(b"R", struct.pack("!I", 0)) +
                   message(b"S", b"client_encoding\0UTF8\0") +
                   message(b"N", notice(b"startup")) + message(b"Z", b"I"))
    while True:
        try:
            kind = exact(stream, 1)
        except EOFError:
            return
        size = struct.unpack("!I", exact(stream, 4))[0]
        if size < 4 or size > 65536:
            raise RuntimeError("Invalid frontend size")
        body = exact(stream, size - 4)
        if kind == b"X":
            return
        if kind != b"Q":
            raise RuntimeError("Expected simple query")
        command = body.rstrip(b"\0")
        if command == b"MALFORMED":
            stream.sendall(message(b"N", b"C00000\0C00000\0Mbad\0\0"))
        elif command == b"OVERSIZED":
            stream.sendall(message(b"N", notice(b"x" * 2048)))
        elif command == b"ERROR":
            stream.sendall(message(b"N", notice(b"before error")) +
                           message(b"E", b"SERROR\0C22012\0Mdivision by zero\0\0") + message(b"Z", b"I"))
        else:
            count = 1 if command == b"ONE_NOTICE" else 4
            notices = b"".join(message(b"N", notice(f"notice {index}".encode())) for index in range(count))
            stream.sendall(notices + message(b"C", b"DO\0") + message(b"Z", b"I"))


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
                            session(connection)
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
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    results = []
    modes = ["observe", "malformed", "oversized", "blocking"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    for mode in modes:
        result = run(args.executable, mode)
        results.append(result)
        if args.output:
            args.output.write_text(json.dumps({"kind": "controlled notice correctness", "results": results}, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        if result["returncode"] or result["peer_errors"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
