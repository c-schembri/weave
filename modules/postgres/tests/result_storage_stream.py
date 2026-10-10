import argparse
import importlib.util
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import traceback


spec = importlib.util.spec_from_file_location("chunks", Path(__file__).with_name("exchange_chunks.py"))
chunks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chunks)


def peer(stream):
    startup = struct.unpack("!I", chunks.read(stream, 4))[0]
    assert 8 <= startup <= 4096
    body = chunks.read(stream, startup - 4)
    assert struct.unpack("!I", body[:4])[0] in (196608, 196610)
    fields = body[4:].split(b"\0")
    assert fields[-2:] == [b"", b""]
    options = dict(zip(fields[::2], fields[1::2]))
    assert options[b"user"] == b"test" and options[b"database"] == b"test"
    stream.sendall(chunks.packet("R", struct.pack("!I", 0)) + chunks.packet("Z", b"I"))
    wire = []
    while True:
        kind = chunks.read(stream, 1)
        length = struct.unpack("!I", chunks.read(stream, 4))[0]
        assert 4 <= length <= 4096
        body = chunks.read(stream, length - 4)
        if kind == b"X":
            assert not body and wire == ["BUFFERED", "STREAM"]
            return
        assert kind == b"Q" and body.endswith(b"\0") and body.count(b"\0") == 1
        sql = body[:-1].decode()
        wire.append(sql)
        assert sql in ("BUFFERED", "STREAM")
        count = 1 if sql == "BUFFERED" else 1024
        stream.sendall(chunks.tuples(count) + chunks.packet("Z", b"I"))


def run(executable, mode):
    roots = 1 if mode in ("context", "blocking") else 16
    errors = []
    workers = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(128)
        listener.settimeout(20)

        def handle(stream):
            try:
                with stream:
                    stream.settimeout(20)
                    peer(stream)
            except Exception:
                errors.append(traceback.format_exc())

        def accept():
            try:
                for _ in range(roots):
                    stream, _ = listener.accept()
                    worker = threading.Thread(target=handle, args=(stream,))
                    workers.append(worker)
                    worker.start()
            except Exception:
                errors.append(traceback.format_exc())

        acceptor = threading.Thread(target=accept)
        acceptor.start()
        environment = {key: value for key, value in os.environ.items() if not key.startswith("PG")}
        process = subprocess.Popen(
            [str(executable), str(listener.getsockname()[1]), mode],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment
        )
        try:
            stdout, stderr = process.communicate(timeout=60)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            acceptor.join(timeout=25)
            for worker in workers:
                worker.join(timeout=25)
        assert not acceptor.is_alive() and all(not worker.is_alive() for worker in workers)
        assert process.returncode == 0, (process.returncode, stdout, stderr, errors)
        assert not errors, "\n".join(errors)
        assert not stderr and "Result memory streaming:" in stdout, (stdout, stderr)
        print(mode, roots, stdout.strip(), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["context", "blocking"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    for mode in modes:
        run(args.executable.resolve(), mode)


if __name__ == "__main__":
    main()
