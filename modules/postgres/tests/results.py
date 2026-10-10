"""Independent result envelope, malformed error and cancellation controls."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location("peer_base", Path(__file__).with_name("encoding_session.py"))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def session(stream, mode):
    stream.settimeout(15)
    size = struct.unpack("!I", base.exact(stream, 4))[0]
    if not 8 <= size <= 1024:
        raise RuntimeError("Invalid startup")
    base.exact(stream, size - 4)
    message = base.message
    stream.sendall(message(b"R", struct.pack("!I", 0)) + base.status(b"UTF8") + message(b"Z", b"I"))
    error = message(b"E", b"SERROR\0VERROR\0C22012\0Mretained failure\0Hhint\0\0")
    while True:
        try:
            kind = base.exact(stream, 1)
        except EOFError:
            return
        size = struct.unpack("!I", base.exact(stream, 4))[0]
        if not 4 <= size <= 4096:
            raise RuntimeError("Invalid query bound")
        body = base.exact(stream, size - 4)
        if kind == b"X" and not body:
            return
        if mode.startswith("extended") and kind == b"P":
            if b"PROBE\0" not in body:
                raise RuntimeError("Unexpected extended SQL")
            for expected in (b"B", b"D", b"E", b"S"):
                actual = base.exact(stream, 1)
                size = struct.unpack("!I", base.exact(stream, 4))[0]
                if actual != expected or not 4 <= size <= 4096:
                    raise RuntimeError("Unexpected extended request framing")
                base.exact(stream, size - 4)
            response = message(b"1") + message(b"2") + message(b"T", struct.pack("!H", 0))
            if mode == "extended_extra_error":
                response += message(b"C", b"SELECT 0\0")
            stream.sendall(response + error + message(b"Z", b"I"))
            continue
        if kind != b"Q" or body not in (b"PROBE\0", b"NOOP\0"):
            raise RuntimeError("Unexpected query")
        if body == b"NOOP\0":
            stream.sendall(message(b"C", b"SET\0") + message(b"Z", b"I"))
            continue
        if mode == "cancel":
            stream.sendall(error + message(b"N", b"SNOTICE\0MPENDING\0\0"))
            if stream.recv(1):
                raise RuntimeError("Cancelled envelope sent another request")
            return
        if mode == "eof":
            return
        responses = {
            "missing": b"",
            "unexpected_copy": message(b"G", b"\0\0\0"),
            "duplicate_error": error + error,
            "after_error": error + message(b"C", b"SET\0"),
            "invalid_state": message(b"E", b"SERROR\0C00000\0Mbad state\0\0"),
            "truncated_error": message(b"E", b"SERROR\0C22012\0Munterminated"),
            "bad_ready": error + message(b"Z", b"X"),
            "oversized": message(b"E", b"x" * 4097),
            "retained_resource": message(b"C", b"SET\0") * 7 + message(b"E", b"SERROR\0C22012\0M" + b"x" * 3500 + b"\0\0"),
        }
        normal = message(b"C", b"SET\0") + message(b"I") + message(b"T", struct.pack("!H", 0))
        normal += message(b"D", struct.pack("!H", 0)) + message(b"C", b"SELECT 1\0") + error
        stream.sendall(responses.get(mode, normal) + message(b"Z", b"I"))


base.session = session


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["normal", "blocking", "cancel", "duplicate_error", "after_error", "invalid_state",
             "truncated_error", "bad_ready", "oversized", "retained_resource", "eof", "extended_error", "extended_extra_error",
             "missing", "unexpected_copy"]
    if args.runtime:
        modes += ["affine", "stealing"]
        if os.name == "nt":
            modes += ["shared_affine", "shared_stealing"]
    for mode in modes:
        result = base.run(args.executable, mode)
        print(json.dumps(result), flush=True)
        if result["returncode"] or result["peer_errors"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
