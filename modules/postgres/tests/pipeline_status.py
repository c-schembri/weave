import argparse
import collections
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


def message(kind, body=b''):
    return kind + struct.pack('!I', len(body) + 4) + body


def command(tag):
    return message(b'C', tag.encode() + b'\0')


def columns():
    body = struct.pack('!H', 1) + b'value\0' + struct.pack('!IHIhIH', 0, 0, 25, -1, 0xffffffff, 0)
    return message(b'T', body)


def row():
    return message(b'D', struct.pack('!HI', 1, 5) + b'value')


def session(stream, events):
    stream.settimeout(15)
    size = struct.unpack('!I', exact(stream, 4))[0]
    if not 8 <= size <= 4096:
        raise RuntimeError('Invalid startup')
    body = exact(stream, size - 4)
    if b'user\0test\0' not in body or b'database\0test\0' not in body:
        raise RuntimeError('Unexpected login')
    stream.sendall(message(b'R', struct.pack('!I', 0)) +
        message(b'S', b'client_encoding\0UTF8\0') + message(b'Z', b'I'))
    transaction = b'I'
    statements = {}
    sql = 'NOOP'
    aborted = False
    waiting = False
    terminal = None

    def execute(text):
        nonlocal transaction, aborted
        events.append(text)
        if text == 'BEGIN':
            transaction = b'T'
            return command('BEGIN')
        if text in ('COMMIT', 'ROLLBACK'):
            transaction = b'I'
            return command(text)
        if text == 'ERROR':
            transaction = b'E' if transaction != b'I' else b'I'
            aborted = True
            return message(b'E', b'SERROR\0C22012\0Mfixture failure\0\0')
        if text == 'ROWS':
            return row() + command('SELECT 1')
        if text in ('NOTICE', 'NOOP', 'EXCHANGE'):
            notice = message(b'N', b'SNOTICE\0Mfixture notice\0\0') if text == 'NOTICE' else b''
            return notice + command('SELECT 0')
        raise RuntimeError('Unexpected SQL: ' + text)

    while True:
        try:
            kind = exact(stream, 1)
        except (EOFError, ConnectionResetError):
            events.append('closed')
            return
        size = struct.unpack('!I', exact(stream, 4))[0]
        if not 4 <= size <= 4096:
            raise RuntimeError('Invalid frame')
        body = exact(stream, size - 4)
        response = b''
        if kind == b'X' and not body:
            events.append('terminate')
            return
        if kind == b'Q':
            text = body[:-1].decode()
            if not body.endswith(b'\0'):
                raise RuntimeError('Unterminated query')
            if text == 'EOF':
                events.append('EOF')
                return
            if text == 'BAD':
                events.append('BAD')
                stream.sendall(message(b'Z', b'?'))
                continue
            if text == 'COPY':
                events.append('COPY')
                response = message(b'H', struct.pack('!BHH', 0, 1, 0))
                response += message(b'd', b'value\n') + message(b'c') + command('COPY 1')
            else:
                response = (columns() if text == 'ROWS' else b'') + execute(text)
            response += message(b'Z', transaction)
            aborted = False
            if text == 'NOOP':
                response += message(b'A', struct.pack('!I', 42) + b'fixture\0notification\0')
        elif kind == b'S':
            response = message(b'Z', transaction)
            aborted = False
            events.append('sync')
        elif kind == b'H':
            events.append('flush')
            if terminal == 'EOF':
                return
            if terminal == 'BAD':
                response = message(b'?')
                terminal = None
            elif waiting:
                response = message(b'N', b'SNOTICE\0MPENDING\0\0')
                waiting = False
        elif not aborted and kind == b'P':
            name, query, _ = body.split(b'\0', 2)
            statements[name] = query.decode()
            sql = query.decode()
            response = message(b'1')
        elif not aborted and kind == b'B':
            _, name, _ = body.split(b'\0', 2)
            sql = statements[name]
            response = message(b'2')
        elif not aborted and kind == b'D':
            response = columns() if sql == 'ROWS' else message(b'n')
        elif not aborted and kind == b'E':
            if sql == 'EOF':
                events.append('EOF')
                terminal = 'EOF'
            elif sql == 'BAD':
                events.append('BAD')
                terminal = 'BAD'
            elif sql == 'WAIT':
                events.append('WAIT')
                waiting = True
            else:
                response = execute(sql)
        elif not aborted and kind == b'C':
            response = message(b'3')
        elif not aborted:
            raise RuntimeError('Unexpected request: ' + repr(kind))
        if response:
            stream.sendall(response)


def run(executable, engine, mode):
    roots = 1 if engine in ('context', 'blocking', 'native') else 16
    connections = roots * (2 if mode == 'normal' and engine != 'native' else 1)
    workers, errors, events = [], [], []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(128)
        listener.settimeout(15)

        def handle(stream):
            try:
                with stream:
                    session(stream, events)
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
        environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        if engine == 'native':
            environment.pop('ASAN_OPTIONS', None)
        try:
            result = subprocess.run([str(executable), str(listener.getsockname()[1]), engine, mode],
                env=environment, capture_output=True, text=True, timeout=60)
        finally:
            acceptor.join(timeout=20)
            for worker in workers:
                worker.join(timeout=20)
        assert not acceptor.is_alive() and not any(worker.is_alive() for worker in workers)
    record = dict(engine=engine, mode=mode, returncode=result.returncode, stdout=result.stdout,
        stderr=result.stderr, peer_errors=errors, roots=roots, connections=len(workers),
        expected_connections=connections, events=dict(collections.Counter(events)))
    print(json.dumps(record), flush=True)
    assert result.returncode == 0 and not result.stderr and not errors and len(workers) == connections
    assert 'pipeline status controls passed:' in result.stdout.lower()
    asynchronous = engine not in ('blocking', 'native')
    expected = {'BEGIN': roots, 'ERROR': 2 * roots, 'ROLLBACK': roots}
    if asynchronous:
        expected['NOOP'] = 2 * roots
    expected['flush'] = roots * (4 if asynchronous else 3)
    expected['sync'] = roots * (4 if asynchronous else 2)
    if mode == 'normal':
        expected['terminate'] = roots
        if engine != 'native':
            expected['closed'] = roots
    elif mode in ('eof', 'bad'):
        expected['flush'] += roots
        expected[mode.upper()] = roots
        if mode == 'bad':
            expected['terminate' if engine == 'native' else 'closed'] = roots
    elif mode == 'drop':
        expected['NOOP'] = expected.get('NOOP', 0) + roots
        expected['flush'] += roots
        expected['closed'] = roots
    else:
        if asynchronous:
            expected['WAIT'] = roots
            expected['flush'] += roots
        expected['closed'] = roots
    assert record['events'] == expected, (record['events'], expected)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--runtime', action='store_true')
    parser.add_argument('--native-only', action='store_true')
    args = parser.parse_args()
    if args.native_only and not args.native:
        parser.error('--native-only requires --native')
    if not args.native_only and not args.executable:
        parser.error('provide --executable or --native-only with --native')
    engines = ['context', 'blocking']
    if args.runtime:
        engines += ['affine', 'stealing']
        if os.name == 'nt':
            engines += ['shared_affine', 'shared_stealing']
    modes = ('normal', 'eof', 'bad', 'cancel', 'drop')
    for engine in ([] if args.native_only else engines):
        for mode in modes:
            run(args.executable, engine, mode)
    if args.native:
        for mode in ('normal', 'eof', 'bad'):
            run(args.native, 'native', mode)


if __name__ == '__main__':
    main()
