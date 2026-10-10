import argparse
import collections
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import threading


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.recv(size - len(result))
        if not part:
            raise EOFError
        result.extend(part)
    return bytes(result)


def message(kind, body=b''):
    return kind + struct.pack('!I', len(body) + 4) + body


def session(stream):
    stream.settimeout(15)
    size = struct.unpack('!I', exact(stream, 4))[0]
    assert 8 <= size <= 4096
    startup = exact(stream, size - 4)
    assert b'user\0test\0' in startup and b'database\0test\0' in startup
    stream.sendall(message(b'R', struct.pack('!I', 0)) +
        message(b'S', b'client_encoding\0UTF8\0') + message(b'Z', b'I'))
    statements = {}
    sql = None
    aborted = False
    pending = bytearray()
    wire = ''
    terminal = None
    eof_flushes = 0
    while True:
        try:
            kind = exact(stream, 1)
        except (EOFError, ConnectionResetError):
            assert terminal in ('WAIT', 'BAD')
            return wire + '!'
        size = struct.unpack('!I', exact(stream, 4))[0]
        assert 4 <= size <= 4096
        body = exact(stream, size - 4)
        wire += kind.decode()
        if kind == b'X':
            assert not body and not pending
            return wire
        if kind == b'H':
            assert not body
            if terminal == 'EOF':
                eof_flushes += 1
                if eof_flushes == 1:
                    continue
                stream.shutdown(socket.SHUT_WR)
                return wire + '!'
            if pending:
                stream.sendall(pending)
                pending.clear()
        elif kind == b'S':
            assert not body
            pending.extend(message(b'Z', b'I'))
            stream.sendall(pending)
            pending.clear()
            aborted = False
        elif aborted:
            assert kind in (b'P', b'B', b'D', b'E')
        elif kind == b'P':
            name, query, rest = body.split(b'\0', 2)
            assert rest == b'\0\0'
            sql = query.decode()
            assert sql in ('NOOP', 'ERROR', 'WAIT', 'EOF', 'BAD')
            statements[name] = sql
            pending.extend(message(b'1'))
        elif kind == b'B':
            _, name, _ = body.split(b'\0', 2)
            sql = statements[name]
            pending.extend(message(b'2'))
        elif kind == b'D':
            assert body == b'P\0'
            pending.extend(message(b'n'))
        elif kind == b'E':
            assert body == b'\0\0\0\0\0'
            if sql == 'ERROR':
                pending.extend(message(b'E', b'SERROR\0C22012\0Mfixture failure\0\0'))
                aborted = True
            elif sql == 'WAIT':
                terminal = sql
                pending.extend(message(b'N', b'SNOTICE\0MPENDING\0\0'))
            elif sql == 'BAD':
                terminal = sql
                pending.extend(message(b'?'))
            elif sql == 'EOF':
                terminal = sql
            else:
                pending.extend(message(b'C', b'SELECT 0\0'))
        else:
            raise AssertionError('Unexpected frame: ' + repr(kind))


def run(executable, mode):
    engine, _, scenario = mode.partition(':')
    scenario = scenario or 'normal'
    roots = 1 if engine in ('context', 'blocking', 'native') else (32 if scenario == 'normal' else 16)
    connections = roots * (1 if engine in ('blocking', 'native') or scenario != 'normal' else 2)
    workers, errors, wires = [], [], []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(128)
        listener.settimeout(15)

        def handle(stream):
            try:
                with stream:
                    wires.append(session(stream))
            except Exception as error:
                errors.append(repr(error))

        def accept():
            try:
                for _ in range(connections):
                    stream, _ = listener.accept()
                    worker = threading.Thread(target=handle, args=(stream,))
                    workers.append(worker)
                    worker.start()
            except Exception as error:
                errors.append(repr(error))

        acceptor = threading.Thread(target=accept)
        acceptor.start()
        try:
            environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
            if engine == 'native':
                environment.pop('ASAN_OPTIONS', None)
            result = subprocess.run([str(executable), str(listener.getsockname()[1]), mode],
                capture_output=True, text=True, timeout=60, env=environment)
        finally:
            acceptor.join(timeout=20)
            for worker in workers:
                worker.join(timeout=20)
        assert not acceptor.is_alive() and not any(worker.is_alive() for worker in workers)
    record = dict(mode=mode, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr,
        connections=len(workers), peer_errors=errors, wire_sequences=dict(collections.Counter(wires)))
    print(json.dumps(record), flush=True)
    assert result.returncode == 0 and not result.stderr and not errors and len(workers) == connections
    if scenario != 'normal':
        assert re.search(r'Queueable Flush controls passed: [1-9]\d* checks', result.stdout)
        sequence = {'cancel_before': 'HHX', 'cancel_after': 'PBDEHH!', 'eof': 'PBDEHH!', 'bad': 'PBDEHH!'}[scenario]
        expected = {sequence: roots}
    elif mode == 'native':
        assert re.search(r'Native queueable Flush controls passed: [1-9]\d* checks; libpq: [1-9]\d*', result.stdout)
        expected = {'HPBDEHPBDESHPBDEHPBDEHHSHX': 1}
    elif mode == 'blocking':
        assert re.search(r'Queueable Flush controls passed: [1-9]\d* checks', result.stdout)
        expected = {'HHHHPBDEHSHX': 1}
    else:
        assert re.search(r'Queueable Flush controls passed: [1-9]\d* checks', result.stdout)
        expected = {'HHHHPBDEHPBDESHHPBDEHPBDEHHHSHX': roots, 'H' * 205 + 'PBDEHHSHX': roots}
    assert record['wire_sequences'] == expected, (record['wire_sequences'], expected)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--runtime', action='store_true')
    args = parser.parse_args()
    if not args.executable and not args.native:
        parser.error('provide --executable or --native')
    if args.runtime and not args.executable:
        parser.error('--runtime requires --executable')

    modes = ['context', 'blocking'] if args.executable else []
    if args.runtime:
        modes += ['affine', 'stealing']
        if os.name == 'nt':
            modes += ['shared_affine', 'shared_stealing']
    for mode in modes:
        run(args.executable, mode)
    scenarios = ['cancel_before', 'cancel_after', 'eof', 'bad']
    for mode in modes:
        for scenario in scenarios:
            if mode != 'blocking' or scenario in ('eof', 'bad'):
                run(args.executable, mode + ':' + scenario)
    if args.native:
        run(args.native, 'native')


if __name__ == '__main__':
    main()
