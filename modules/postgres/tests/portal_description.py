"""Independent portal description wire, failure and cancellation controls."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct

spec = importlib.util.spec_from_file_location("peer_base", Path(__file__).with_name("encoding_session.py"))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def frame(stream):
    kind = base.exact(stream, 1)
    size = struct.unpack("!I", base.exact(stream, 4))[0]
    if not 4 <= size <= 2048:
        raise RuntimeError("Invalid request bound")
    return kind, base.exact(stream, size - 4)


def session(stream, mode):
    stream.settimeout(15)
    size = struct.unpack("!I", base.exact(stream, 4))[0]
    if not 8 <= size <= 1024:
        raise RuntimeError("Invalid startup")
    base.exact(stream, size - 4)
    message = base.message
    stream.sendall(message(b"R", struct.pack("!I", 0)) + base.status(b"UTF8") + message(b"Z", b"I"))
    row = message(b"T", struct.pack("!H", 1) + b"value\0" + struct.pack("!IhIhih", 16384, 2, 23, 4, -1, 1))
    descriptions = 0
    while True:
        try:
            kind, body = frame(stream)
        except EOFError:
            return
        if kind == b"X" and not body:
            return
        if kind == b"Q" and body == b"NOOP\0":
            stream.sendall(message(b"C", b"SELECT 1\0") + message(b"Z", b"I"))
            continue
        expected = b"Pprobe\0" if descriptions == 0 else b"P\0"
        if kind != b"D" or body != expected or frame(stream) != (b"S", b""):
            raise RuntimeError("Unexpected portal request or Sync")
        descriptions += 1
        if mode == "cancel":
            stream.sendall(message(b"N", b"SNOTICE\0MPENDING\0\0"))
            if stream.recv(1):
                raise RuntimeError("Cancelled description wrote another request")
            return
        malformed = {
            "missing": b"",
            "duplicate": row + row,
            "duplicate_nodata": message(b"n") + message(b"n"),
            "parameters": message(b"t", struct.pack("!HI", 1, 23)) + row,
            "data": row + message(b"D", struct.pack("!Hi", 1, 4) + b"test"),
            "command": message(b"C", b"SELECT 1\0"),
            "empty_query": message(b"I"),
            "parse_complete": message(b"1"),
            "bind_complete": message(b"2"),
            "suspended": row + message(b"s"),
            "nodata_body": message(b"n", b"x"),
            "bad_columns": message(b"T", b"\x00\x01broken"),
            "oversized": message(b"n", b"x" * 1025),
        }
        if mode == "sql_error":
            response = message(b"E", b"SERROR\0C34000\0MNo such portal\0\0")
        elif mode in malformed:
            response = malformed[mode]
        else:
            response = row if descriptions == 1 else message(b"n")
        stream.sendall(response + message(b"Z", b"I"))


base.session = session


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--runtime", action="store_true")
    args = parser.parse_args()
    modes = ["normal", "blocking", "sql_error", "cancel", "missing", "duplicate", "duplicate_nodata",
             "parameters", "data", "command", "empty_query", "parse_complete", "bind_complete", "suspended",
             "nodata_body", "bad_columns", "oversized"]
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
