"""Independent protocol peer for pool capacity, reuse, expiry and cancellation."""

import argparse
import json
import os
import socket
import struct
import subprocess
import threading


def exact(stream, size):
    output = bytearray()
    while len(output) < size:
        block = stream.recv(size - len(output))
        if not block:
            raise EOFError
        output.extend(block)
    return bytes(output)


def packet(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


def run(executable, mode):
    stopped = threading.Event()
    errors, commands, workers = [], [], []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(32)
        listener.settimeout(0.05)

        def handle(stream):
            try:
                with stream:
                    stream.settimeout(15)
                    size = struct.unpack("!I", exact(stream, 4))[0]
                    if not 8 <= size <= 65536:
                        raise RuntimeError("Invalid startup bound")
                    exact(stream, size - 4)
                    stream.sendall(packet(b"R", struct.pack("!I", 0)) + packet(b"Z", b"I"))
                    transaction = b"I"
                    fail_reset = False
                    while True:
                        kind = exact(stream, 1)
                        size = struct.unpack("!I", exact(stream, 4))[0]
                        if not 4 <= size <= 1024:
                            raise RuntimeError("Invalid query bound")
                        sql = exact(stream, size - 4)
                        allowed = (b"BEGIN\0", b"NOOP\0", b"ROLLBACK\0", b"DISCARD ALL\0", b"FAIL_RESET\0")
                        if kind != b"Q" or sql not in allowed:
                            raise RuntimeError(f"Unexpected command: {kind!r}, {sql!r}")
                        if sql == b"DISCARD ALL\0" and transaction != b"I":
                            raise RuntimeError("Session reset inside a transaction")
                        transaction = b"T" if sql == b"BEGIN\0" else b"I"
                        commands.append(sql)
                        if sql == b"FAIL_RESET\0":
                            fail_reset = True
                        if sql == b"DISCARD ALL\0" and fail_reset:
                            failure = b"SERROR\0VERROR\0C08006\0MReset refused\0\0"
                            stream.sendall(packet(b"E", failure) + packet(b"Z", b"I"))
                        else:
                            stream.sendall(packet(b"C", b"OK\0") + packet(b"Z", transaction))
            except EOFError:
                pass
            except Exception as error:
                errors.append(repr(error))

        def accept():
            while not stopped.is_set():
                try:
                    stream, _ = listener.accept()
                except socket.timeout:
                    continue
                worker = threading.Thread(target=handle, args=(stream,))
                workers.append(worker)
                worker.start()

        server = threading.Thread(target=accept)
        server.start()
        try:
            result = subprocess.run([executable, str(listener.getsockname()[1]), mode],
                                    capture_output=True, text=True, timeout=30)
        finally:
            stopped.set()
            server.join()
            for worker in workers:
                worker.join(timeout=16)
            if any(worker.is_alive() for worker in workers):
                errors.append("Peer worker did not drain")

    pools = 4 if mode.endswith("affine") else 1
    expected_sessions = pools * 10
    report = dict(mode=mode, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr,
                  sessions=len(workers), expected_sessions=expected_sessions, peer_errors=errors)
    print(json.dumps(report), flush=True)
    if result.returncode or errors or len(workers) != expected_sessions:
        raise RuntimeError("Pool protocol qualification failed")
    if commands.count(b"ROLLBACK\0") != pools or commands.count(b"NOOP\0") != pools * 35:
        raise RuntimeError("Missing pooled workload or transaction cleanup")
    if commands.count(b"DISCARD ALL\0") != pools * 36 or commands.count(b"FAIL_RESET\0") != pools:
        raise RuntimeError("Missing session resets")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["context"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    for mode in modes:
        run(args.executable, mode)


if __name__ == "__main__":
    main()
