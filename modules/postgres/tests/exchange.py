"""Independent exchange protocol and duplex regression peer, without libpq."""

import argparse
import os
import socket
import struct
import subprocess
import threading


def exact(connection, size):
    output = bytearray()
    while len(output) < size:
        value = connection.recv(size - len(output))
        if not value:
            raise EOFError("Incomplete frontend message")
        output.extend(value)
    return bytes(output)


def request(connection):
    kind, size = struct.unpack("!cI", exact(connection, 5))
    if size < 4 or size > 16 * 1024 * 1024:
        raise RuntimeError("Unbounded frontend message")
    return kind, exact(connection, size - 4)


def message(kind, body=b""):
    return kind + struct.pack("!I", len(body) + 4) + body


READY = message(b"Z", b"I")
COMPLETE = message(b"C", b"SELECT 1\0")
ERROR = message(b"E", b"SERROR\0C22000\0Mfixture error\0\0")
COLUMNS = message(b"T", struct.pack("!H", 1) + b"value\0" + struct.pack("!IHIhih", 0, 0, 25, -1, -1, 0))
ROW = message(b"D", struct.pack("!HI", 1, 2) + b"42")
OUTPUT = message(b"H", b"\0\0\0")
INPUT = message(b"G", b"\0\0\0")
BOTH = message(b"W", b"\0\0\0")
DONE = message(b"c")
KEEPALIVE = message(b"d", b"k" + bytes(17))


def close_expected(connection):
    try:
        if connection.recv(1):
            raise RuntimeError("Terminal operation still produced frontend bytes")
    except ConnectionResetError:
        pass


def backend(connection, mode):
    connection.settimeout(8)
    size = struct.unpack("!I", exact(connection, 4))[0]
    startup = exact(connection, size - 4)
    if startup[:4] != struct.pack("!I", 196610):
        raise RuntimeError("Expected PostgreSQL protocol 3.2")
    connection.sendall(message(b"R", bytes(4)) + READY)
    if request(connection) != (b"Q", b"probe\0"):
        raise RuntimeError("Wrong command")

    if mode == "duplex":
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)
        connection.sendall(BOTH)
        failures = []
        def writer():
            try:
                chunk = message(b"d", b"x" * (1024 * 1024))
                for _ in range(4):
                    connection.sendall(chunk)
                connection.sendall(DONE)
            except Exception as error:
                failures.append(error)
        sender = threading.Thread(target=writer)
        sender.start()
        try:
            for _ in range(4):
                if request(connection) != (b"d", b"y" * (1024 * 1024)):
                    raise RuntimeError("Wrong client CopyData")
            if request(connection) != (b"c", b""):
                raise RuntimeError("Missing client CopyDone")
        finally:
            sender.join(timeout=8)
        if sender.is_alive() or failures:
            raise RuntimeError(f"Duplex writer did not finish: {failures}")
        connection.sendall(message(b"C", b"COPY 0\0") + message(b"C", b"START_REPLICATION\0") + READY)
        if request(connection) != (b"X", b""):
            raise RuntimeError("Missing Terminate")
        return

    scripts = {
        "output": OUTPUT + message(b"d") + message(b"d", b"42\n") + DONE + COMPLETE + COMPLETE + READY,
        "boundary": COLUMNS + ROW + COLUMNS + ROW + OUTPUT + message(b"d", b"archive") + DONE + COMPLETE + COMPLETE + READY,
        "bad_format": message(b"H", b"\2\0\0"),
        "bad_column": message(b"H", b"\0\0\1\0\2"),
        "bad_count": message(b"H", b"\0\0\1"),
        "wrong_data": INPUT + message(b"d", b"wrong"),
        "early_command": OUTPUT + COMPLETE,
        "missing_command": OUTPUT + DONE + READY,
        "duplicate_done": OUTPUT + DONE + DONE,
        "ordinary_boundary": COLUMNS + ROW + COLUMNS,
        "ready_without_command": READY,
        "empty_command": message(b"C", b"\0"),
        "output_error": OUTPUT + ERROR + READY,
        "input_deferred": INPUT + ERROR + READY,
        "limit": COLUMNS + 2 * message(b"D", struct.pack("!HI", 1, 700) + b"x" * 700),
        "eof_header": b"H\0\0",
        "eof_body": b"H" + struct.pack("!I", 7) + b"\0",
        "abandon": OUTPUT,
    }
    if mode in scripts:
        if mode == "wrong_data":
            connection.sendall(INPUT)
            if request(connection) != (b"c", b""):
                raise RuntimeError("Expected client CopyDone")
            connection.sendall(message(b"d", b"wrong"))
        else:
            connection.sendall(scripts[mode])
    elif mode in ("keepalive", "bad_keepalive", "late_data", "both_error"):
        connection.sendall(BOTH)
        if request(connection) != (b"c", b""):
            raise RuntimeError("Expected client CopyDone")
        if mode == "both_error":
            connection.sendall(ERROR)
        else:
            tail = KEEPALIVE if mode == "keepalive" else (
                message(b"d", b"k" + bytes(16) + b"\2") if mode == "bad_keepalive" else message(b"d", b"w" + bytes(24)))
            connection.sendall(DONE + tail + COMPLETE + COMPLETE + READY)
    elif mode == "input_error":
        connection.sendall(INPUT)
        if request(connection) != (b"f", b"fixture rejection\0"):
            raise RuntimeError("Expected client CopyFail")
        connection.sendall(ERROR + READY)
    elif mode in ("cancel_header", "cancel_read"):
        if mode == "cancel_read":
            connection.sendall(OUTPUT)
        close_expected(connection)
        return
    else:
        raise RuntimeError(f"Unknown case: {mode}")

    if mode in ("eof_header", "eof_body"):
        connection.shutdown(socket.SHUT_WR)
    if mode in ("output", "boundary", "keepalive", "input_error", "output_error"):
        if request(connection) != (b"Q", b"reuse\0"):
            raise RuntimeError("Connection was not reusable")
        connection.sendall(COMPLETE + READY)
        if request(connection) != (b"X", b""):
            raise RuntimeError("Missing Terminate")
    else:
        close_expected(connection)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = (
        "output", "boundary", "keepalive", "bad_format", "bad_column", "bad_count", "wrong_data",
        "early_command", "missing_command", "duplicate_done", "ordinary_boundary", "ready_without_command",
        "empty_command", "input_error", "output_error", "input_deferred", "both_error", "bad_keepalive",
        "late_data", "limit", "eof_header", "eof_body", "cancel_header", "cancel_read", "abandon", "duplex",
    )
    cases = [(mode, [], 1) for mode in modes]
    if args.runtime:
        layouts = ("sharded", "shared") if os.name == "nt" else ("sharded",)
        for scheduler in ("affine", "stealing"):
            for layout in layouts:
                cases.append(("duplex", [scheduler, layout], 16))
    for mode, execution, clients in cases:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(10)
            failures = []
            peers = []
            def handle(connection):
                try:
                    with connection:
                        backend(connection, mode)
                except Exception as error:
                    failures.append(error)
            def serve():
                try:
                    for _ in range(clients):
                        peer = threading.Thread(target=handle, args=(listener.accept()[0],))
                        peers.append(peer)
                        peer.start()
                except Exception as error:
                    failures.append(error)
                finally:
                    for peer in peers:
                        peer.join(timeout=10)
            server = threading.Thread(target=serve)
            server.start()
            process = subprocess.Popen([args.executable, str(listener.getsockname()[1]), mode, *execution],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                output, error = process.communicate(timeout=30)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
                server.join(timeout=10)
            if process.returncode or server.is_alive() or any(peer.is_alive() for peer in peers) or failures:
                raise RuntimeError(f"{mode}: exit={process.returncode}, peer={failures}, {output}, {error}")
            print(f"{mode} {' '.join(execution)} ({clients} clients): passed")
    print(f"{len(cases)} independent exchange cases passed")


if __name__ == "__main__":
    main()
