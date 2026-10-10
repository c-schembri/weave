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
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise EOFError()
        data.extend(chunk)
    return bytes(data)


def message(kind, body=b''):
    return kind + struct.pack('!I', len(body) + 4) + body


def row(native, readonly):
    names = [b'transaction_read_only'] if native else [b'read_only', b'recovery']
    values = [b'on' if readonly else b'off']
    if not native:
        values.append(b'f')
    columns = struct.pack('!H', len(names))
    for name in names:
        columns += name + b'\0' + struct.pack('!IhIhih', 0, 0, 25, -1, -1, 0)
    data = struct.pack('!H', len(values))
    for value in values:
        data += struct.pack('!I', len(value)) + value
    return message(b'T', columns) + message(b'D', data) + message(b'C', b'SELECT 1\0')


def session(stream, mode, native):
    stream.settimeout(15)
    size = struct.unpack('!I', exact(stream, 4))[0]
    if not 8 <= size <= 4096:
        raise RuntimeError('bad startup length')
    exact(stream, size - 4)
    stream.sendall(message(b'R', struct.pack('!I', 0)) + message(b'Z', b'I'))
    while True:
        try:
            kind = exact(stream, 1)
        except EOFError:
            return
        size = struct.unpack('!I', exact(stream, 4))[0]
        if not 4 <= size <= 4096:
            raise RuntimeError('bad request length')
        body = exact(stream, size - 4)
        if kind == b'X' and not body:
            return
        if kind != b'Q' or not body.endswith(b'\0') or mode == 'any':
            raise RuntimeError('unexpected request')
        if native:
            if body not in [b'SHOW transaction_read_only\0', b'SHOW default_transaction_read_only\0']:
                raise RuntimeError(f'unexpected native query {body!r}')
        elif body != b"SELECT pg_catalog.current_setting('default_transaction_read_only'), pg_catalog.pg_is_in_recovery()\0":
            raise RuntimeError('unexpected Weave query')
        if mode == 'eof':
            return
        error = message(b'E', b'SERROR\0VERROR\0C42501\0Mtarget policy denied\0Dindependent detail\0Hindependent hint\0\0')
        if mode in ['cancel', 'cancel_after_sql']:
            response = error if mode == 'cancel_after_sql' else b''
            response += message(b'N', b'SNOTICE\0MPENDING\0\0')
            stream.sendall(response)
            if stream.recv(1):
                raise RuntimeError('cancelled task sent another request')
            return
        if mode == 'malformed':
            response = message(b'Z', b'X')
        elif mode == 'protocol_after_sql':
            response = error + message(b'Z', b'X')
        elif mode == 'sql':
            response = error + message(b'Z', b'I')
        else:
            response = row(native, mode == 'success') + message(b'Z', b'I')
        stream.sendall(response)


def run(executable, mode, engine):
    stop = threading.Event()
    workers = []
    errors = []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(128)
        listener.settimeout(0.1)

        def accept():
            while not stop.is_set():
                try:
                    stream, _ = listener.accept()
                except socket.timeout:
                    continue

                def client(connection):
                    try:
                        with connection:
                            session(connection, mode, engine == 'native')
                    except Exception as error:
                        errors.append(repr(error))

                worker = threading.Thread(target=client, args=(stream,))
                workers.append(worker)
                worker.start()

        thread = threading.Thread(target=accept)
        thread.start()
        environment = dict(os.environ)
        for key in list(environment):
            if key.upper().startswith('PG'):
                del environment[key]
        environment['PGPASSFILE'] = os.devnull
        environment['LC_ALL'] = 'C'
        try:
            args = [str(executable), str(listener.getsockname()[1]), mode, engine]
            result = subprocess.run(args, env=environment, capture_output=True, text=True, timeout=45)
        finally:
            stop.set()
            thread.join(timeout=20)
            for worker in workers:
                worker.join(timeout=20)
        if thread.is_alive() or any(worker.is_alive() for worker in workers):
            raise RuntimeError('peer did not drain')
    retry = mode == 'reject' or (engine == 'native' and mode == 'sql')
    expected = (16 if engine in ['affine', 'stealing'] else 1) * (2 if retry else 1)
    record = dict(mode=mode, engine=engine, returncode=result.returncode,
                  stdout=result.stdout, stderr=result.stderr, peer_errors=errors,
                  connections=len(workers), expected_connections=expected)
    print(json.dumps(record), flush=True)
    if result.returncode or errors or len(workers) != expected:
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--runtime', action='store_true')
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    engines = ['native'] if args.native else ['context', 'blocking']
    if args.runtime and not args.native:
        engines += ['affine', 'stealing']
    for engine in engines:
        modes = ['sql', 'protocol_after_sql', 'malformed', 'eof', 'success', 'reject', 'any']
        if engine in ['context', 'affine', 'stealing']:
            modes += ['cancel', 'cancel_after_sql']
        if engine == 'native':
            modes = ['sql', 'success', 'reject', 'any']
        for mode in modes:
            run(args.executable, mode, engine)


if __name__ == '__main__':
    main()
