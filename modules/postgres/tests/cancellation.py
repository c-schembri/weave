import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import queue
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import threading
import tempfile


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        data = stream.recv(size - len(result))
        if not data:
            raise EOFError("Incomplete packet")
        result.extend(data)
    return bytes(result)


def packet(stream):
    size, = struct.unpack("!I", exact(stream, 4))
    if not 8 <= size <= 4096:
        raise RuntimeError("Invalid packet size")
    return exact(stream, size - 4)


def message(kind, body):
    return kind + struct.pack("!I", len(body) + 4) + body


def upgrade(stream, context, secured):
    if secured:
        if packet(stream) != struct.pack("!I", 80877103):
            raise RuntimeError("Expected SSLRequest")
        stream.sendall(b"S")
        return context.wrap_socket(stream, server_side=True)
    return stream


def credentials(index, version):
    return struct.pack("!I", 42 + index) + bytes(range(index, index + (4 if version == "3.0" else 32)))


def session(stream, context, secured, version, index, reset):
    stream.settimeout(8)
    stream = upgrade(stream, context, secured)
    try:
        startup = packet(stream)
        wanted = 196608 if version == "3.0" else 196610
        if startup[:4] != struct.pack("!I", wanted):
            raise RuntimeError("Unexpected protocol version")
        authenticated = message(b"R", struct.pack("!I", 0))
        backend_key = message(b"K", credentials(index, version))
        ready = message(b"Z", b"I")
        stream.sendall(authenticated + backend_key + ready)
        if reset:
            if stream.recv(1):
                raise RuntimeError("Reset left old session open")
        elif exact(stream, 5) != b"X\x00\x00\x00\x04":
            raise RuntimeError("Missing Terminate")
        if not reset and secured and stream.recv(1):
            raise RuntimeError("Unexpected data after Terminate")
    finally:
        stream.close()


def cancellation(stream, context, bad_context, secured, version, mode, index, completion):
    stream.settimeout(8)
    try:
        if secured:
            if packet(stream) != struct.pack("!I", 80877103):
                raise RuntimeError("Expected cancellation SSLRequest")
            if mode in ("refused", "invalid_ssl_reply"):
                stream.sendall(b"N" if mode == "refused" else b"?")
                if stream.recv(1):
                    raise RuntimeError("Secret sent after TLS refusal")
                return
            if mode == "deadline_ssl_reply":
                if stream.recv(1):
                    raise RuntimeError("Secret sent before SSL negotiation")
                return
            stream.sendall(b"S")
            if mode == "deadline_handshake":
                while stream.recv(4096):
                    pass
                return
            if mode == "certificate":
                try:
                    stream = bad_context.wrap_socket(stream, server_side=True)
                except ssl.SSLError:
                    return
                raise RuntimeError("Client accepted expired cancellation certificate")
            stream = context.wrap_socket(stream, server_side=True)
        expected = struct.pack("!I", 80877102) + credentials(index, version)
        if packet(stream) != expected:
            raise RuntimeError("Cancellation did not retain the correct backend snapshot")
        if mode in ("reset_after_packet", "reset_after_packet_blocking"):
            if os.name != "nt":
                raise RuntimeError("Post-packet reset requires Windows cancellation")
            # PostgreSQL can close immediately after this complete packet, before close_notify.
            stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("HH", 1, 0))
            return
        if stream.recv(1):
            raise RuntimeError("Unexpected cancellation data after packet")
        if mode == "protocol":
            stream.sendall(b"!")
        elif mode in ("deadline_eof", "parent_cancel"):
            # Plain TCP EOF is its expected half-close; keep the response direction
            # open until the client process reaches its deadline and disconnects.
            if secured:
                while stream.recv(4096):
                    pass
            completion.wait(5)
        elif mode == "close_notify" and secured:
            raw = stream.unwrap()
            raw.close()
        elif mode in ("reset_eof", "reset_eof_blocking"):
            if os.name != "nt":
                raise RuntimeError("Reset-as-EOF requires Windows cancellation")
            # The full key and client's TLS close_notify were decoded above.
            stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("HH", 1, 0))
    finally:
        stream.close()


def cancellation_count(mode, workers):
    no_requests = ("pre_cancel", "moved_from", "blocking_in_task", "closed_request")
    if mode in no_requests:
        return 0
    if workers or mode in ("blocking", "reset_eof_blocking", "reset_after_packet_blocking"):
        return 16
    if mode == "reset":
        return 2
    return 1


def read_certificates(process, lines):
    lines.put(tuple(process.stdout.readline().strip() for _ in range(4)))


def run_case(executable, mode, transport, version, workers=0, scheduler="affine", layout="sharded"):
    completion = threading.Event()
    command = [executable, mode, transport, version, str(workers), scheduler, layout]
    process = subprocess.Popen(
        command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    lines = queue.Queue()
    reader = threading.Thread(target=read_certificates, args=(process, lines), daemon=True)
    reader.start()
    tasks = []
    fixture_directory = None
    try:
        ca, certificate, key, expired = lines.get(timeout=10)
        if not all((ca, certificate, key, expired)):
            raise RuntimeError("Missing certificate fixture")
        paths = tuple(Path(name).resolve() for name in (ca, certificate, key, expired))
        parent = paths[0].parent
        if parent.parent != Path(tempfile.gettempdir()).resolve() or not parent.name.startswith("weave-tls-"):
            raise RuntimeError("Invalid owned certificate fixture directory")
        if any(path.parent != parent for path in paths):
            raise RuntimeError("Certificate fixture paths disagree")
        fixture_directory = parent
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        context.load_verify_locations(ca)
        context.verify_mode = ssl.CERT_REQUIRED
        bad_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        bad_context.load_cert_chain(expired, key)
        with socket.socket() as listener, ThreadPoolExecutor(max_workers=16) as pool:
            listener.bind(("127.0.0.1", 0))
            listener.listen(32)
            listener.settimeout(8)
            process.stdin.write(str(listener.getsockname()[1]) + "\n")
            process.stdin.flush()
            two_sessions = mode in ("reset", "deferred_reset", "closed_request")
            for index in range(2 if two_sessions else 1):
                accepted = listener.accept()[0]
                if mode == "deferred_reset" and index == 1:
                    # The replacement remains open while the old cancellation is sent.
                    tasks.append(pool.submit(session, accepted, context, transport == "tls", version, index, False))
                else:
                    reset = mode in ("reset", "deferred_reset") and index == 0
                    session(accepted, context, transport == "tls", version, index, reset)

            count = cancellation_count(mode, workers)
            for index in range(count):
                tasks.append(pool.submit(cancellation, listener.accept()[0], context, bad_context,
                    transport == "tls", version, mode, index if mode == "reset" else 0, completion))
            output, error = process.communicate(timeout=10)
            completion.set()
            for task in tasks:
                task.result(timeout=10)
            listener.setblocking(False)
            try:
                unexpected, _ = listener.accept()
            except BlockingIOError:
                pass
            else:
                unexpected.close()
                raise RuntimeError("Unexpected additional connection")
        if mode in ("moved_from", "blocking_in_task"):
            if not process.returncode or "Weave contract:" not in error or "connection.cpp" not in error:
                raise RuntimeError(f"Missing cancellation contract: {process.returncode} {error}")
        elif process.returncode:
            raise RuntimeError(f"Client failed ({process.returncode}): {output} {error}")
        print(f"{mode} {transport} {version} {workers} {scheduler} {layout}: passed")
    finally:
        completion.set()
        if process.poll() is None:
            process.kill()
        reader.join(timeout=10)
        process.communicate(timeout=10)
        if fixture_directory is not None:
            owned_files = ("ca.pem", "other-ca.pem", "server.pem", "expired.pem", "key.pem", "client.pem", "client-key.pem")
            for filename in owned_files:
                (fixture_directory / filename).unlink(missing_ok=True)
            try:
                fixture_directory.rmdir()
            except FileNotFoundError:
                pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = (
        "success", "owned_request", "deferred_destroy", "deferred_move", "closed_request",
        "protocol", "reset", "deferred_reset", "deadline_eof", "parent_cancel", "pre_cancel",
        "blocking", "moved_from", "blocking_in_task")
    tls_modes = (
        "close_notify", "refused", "invalid_ssl_reply", "certificate",
        "deadline_ssl_reply", "deadline_handshake")
    if os.name == "nt":
        modes += ("reset_eof", "reset_eof_blocking", "reset_after_packet", "reset_after_packet_blocking")
    versions = ("3.0", "3.2")
    transports = ("plain", "tls")
    schedulers = ("affine", "stealing")
    worker_counts = (1, 4)
    layouts = ("sharded", "shared") if os.name == "nt" else ("sharded",)
    for version in versions:
        for transport in transports:
            for mode in modes:
                run_case(args.executable, mode, transport, version)
            if transport == "tls":
                for mode in tls_modes:
                    run_case(args.executable, mode, transport, version)
            if args.runtime:
                for scheduler in schedulers:
                    for layout in layouts:
                        for workers in worker_counts:
                            run_case(args.executable, "concurrent", transport, version, workers, scheduler, layout)
                        if os.name == "nt":
                            run_case(args.executable, "reset_eof", transport, version, 4, scheduler, layout)
                            run_case(args.executable, "reset_after_packet", transport, version, 4, scheduler, layout)


if __name__ == "__main__":
    main()
