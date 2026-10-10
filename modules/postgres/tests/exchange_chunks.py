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
import traceback


def packet(kind, body=b''):
    return kind.encode() + struct.pack('!I', len(body) + 4) + body


def read(stream, count):
    result = b''
    while len(result) < count:
        part = stream.recv(count - len(result))
        if not part:
            raise EOFError()
        result += part
    return result


def description():
    columns = [('n', 23, 4), ('payload', 25, -1), ('nullable', 25, -1)]
    body = struct.pack('!H', len(columns))
    for name, oid, size in columns:
        body += name.encode() + b'\0' + struct.pack('!IHIhiH', 0, 0, oid, size, -1, 0)
    return packet('T', body)


def data_rows(first, last, payload=120):
    result = b''
    for number in range(first, last + 1):
        values = [str(number).encode(), b'x' * payload, None if number % 2 == 0 else b'']
        body = struct.pack('!H', len(values))
        for value in values:
            body += struct.pack('!i', -1 if value is None else len(value))
            if value is not None:
                body += value
        result += packet('D', body)
    return result


def rows(count, payload=120):
    return description() + data_rows(1, count, payload)


def tuples(count, payload=120):
    return rows(count, payload) + packet('C', f'SELECT {count}'.encode() + b'\0')


def binary_rows():
    columns = [(b'n', 411, 2, 23, 4, -1, 1),
        (b'payload', 411, 3, 17, -1, -1, 1),
        (b'caf\xc3\xa9', 0, 0, 25, -1, 7, 0)]
    body = struct.pack('!H', len(columns))
    for name, table, attribute, oid, size, modifier, format_code in columns:
        body += name + b'\0' + struct.pack('!IHIhiH', table, attribute, oid, size, modifier, format_code)
    response = packet('T', body)
    for number in range(1, 6):
        values = [struct.pack('!i', number), b'a\0b\xff', None if number % 2 == 0 else b'caf\xc3\xa9']
        body = struct.pack('!H', len(values))
        for value in values:
            body += struct.pack('!i', -1 if value is None else len(value))
            if value is not None:
                body += value
        response += packet('D', body)
    return response + packet('C', b'SELECT 5\0')


def peer(stream, progress):
    startup = struct.unpack('!I', read(stream, 4))[0]
    assert 8 <= startup <= 4096
    body = read(stream, startup - 4)
    version = struct.unpack('!I', body[:4])[0]
    assert version in (196608, 196610), (version, body.hex())
    fields = body[4:].split(b'\0')
    assert fields[-2:] == [b'', b''], body.hex()
    options = dict(zip(fields[::2], fields[1::2]))
    assert options[b'user'] == b'test' and options[b'database'] == b'test', body.hex()
    stream.sendall(packet('R', struct.pack('!I', 0)) + packet('S', b'client_encoding\0UTF8\0') + packet('Z', b'I'))
    wire = []
    while True:
        try:
            kind = read(stream, 1)
        except (EOFError, ConnectionResetError):
            assert wire in (['BIG'], ['WAIT'], ['BAD'], ['BAD_COMMAND'])
            return wire + ['!']
        length = struct.unpack('!I', read(stream, 4))[0]
        assert 4 <= length <= 4096
        body = read(stream, length - 4)
        if kind == b'X':
            assert not body
            return wire + ['X']
        assert kind == b'Q' and body.endswith(b'\0') and body.count(b'\0') == 1
        sql = body[:-1].decode()
        wire.append(sql)
        if sql == 'FIVE':
            response = tuples(5)
        elif sql == 'ZERO':
            response = tuples(0)
        elif sql == 'MULTI':
            response = tuples(5) + packet('C', b'DO\0') + tuples(0)
        elif sql == 'ERROR':
            response = rows(3) + packet('E', b'SERROR\0VERROR\0C22012\0Mdivision by zero\0\0')
        elif sql == '':
            response = packet('I')
        elif sql == 'COPY':
            response = packet('H', struct.pack('!BHH', 0, 1, 0)) + packet('d', b'hi\n') + packet('c')
            response += packet('C', b'COPY 1\0') + tuples(5)
        elif sql == 'LIMIT':
            response = tuples(100, 200)
        elif sql == 'BIG':
            response = tuples(1, 1700)
        elif sql == 'NO_COLUMNS':
            response = packet('T', struct.pack('!H', 0)) + packet('D', struct.pack('!H', 0)) * 5
            response += packet('C', b'SELECT 5\0')
        elif sql == 'BINARY':
            response = binary_rows()
        elif sql == 'DEFER':
            response = tuples(2)
        elif sql == 'EARLY':
            stream.sendall(rows(2))
            assert progress.wait(10), 'application did not receive the withheld-tail prefix'
            response = data_rows(3, 5) + packet('C', b'SELECT 5\0')
        elif sql == 'EOF':
            stream.sendall(rows(2))
            stream.shutdown(socket.SHUT_WR)
            return wire + ['!']
        elif sql == 'BAD':
            response = rows(2) + packet('D', struct.pack('!Hi', 1, 0))
        elif sql == 'BAD_COMMAND':
            response = rows(2) + packet('C', b'SELECT 2')
        elif sql == 'WAIT':
            stream.sendall(rows(2) + packet('N', b'SNOTICE\0VNOTICE\0C00000\0MPENDING\0\0'))
            continue
        else:
            raise AssertionError(sql)
        stream.sendall(response + packet('Z', b'I'))


def run(executable, mode):
    engine, _, scenario = mode.partition(':')
    roots = 1 if engine in ('context', 'blocking', 'native') else 16
    connections = roots if scenario or engine == 'native' else roots * 3
    errors = []
    wires = []
    workers = []
    progress = threading.Event()
    prefixes = []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(512)
        listener.settimeout(30)

        def handle(stream):
            try:
                with stream:
                    stream.settimeout(30)
                    wires.append(peer(stream, progress))
            except Exception as error:
                errors.append(traceback.format_exc())

        def accept():
            try:
                for _ in range(connections):
                    stream, _ = listener.accept()
                    worker = threading.Thread(target=handle, args=(stream,))
                    workers.append(worker)
                    worker.start()
            except Exception as error:
                errors.append(traceback.format_exc())

        acceptor = threading.Thread(target=accept)
        acceptor.start()
        environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
        if engine == 'native':
            environment.pop('ASAN_OPTIONS', None)
        try:
            if scenario == 'early':
                process = subprocess.Popen([str(executable), str(listener.getsockname()[1]), mode],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment)
                output = []

                def observe():
                    for line in process.stdout:
                        output.append(line)
                        if line.strip() == 'Chunk prefix observed':
                            prefixes.append(line)
                            if len(prefixes) == roots:
                                progress.set()

                observer = threading.Thread(target=observe)
                observer.start()
                try:
                    process.wait(timeout=60)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    observer.join(timeout=5)
                assert not observer.is_alive()
                stdout = ''.join(output)
                stderr = process.stderr.read()
                process.stdout.close()
                process.stderr.close()
            else:
                process = subprocess.run([str(executable), str(listener.getsockname()[1]), mode],
                    capture_output=True, text=True, timeout=60, env=environment)
                stdout, stderr = process.stdout, process.stderr
        finally:
            acceptor.join(timeout=35)
            for worker in workers:
                worker.join(timeout=35)
        assert not acceptor.is_alive() and not any(worker.is_alive() for worker in workers)
    record = dict(mode=mode, returncode=process.returncode, stdout=stdout, stderr=stderr,
        prefixes_observed=len(prefixes), tail_released=progress.is_set(),
        connections=len(workers), peer_errors=errors, wire_sequences=dict(collections.Counter('|'.join(wire) for wire in wires)))
    print(json.dumps(record), flush=True)
    assert process.returncode == 0 and not stderr and not errors and len(workers) == connections
    marker = 'Native exchange chunks' if engine == 'native' else 'Exchange chunks'
    assert re.search(marker + r' controls passed: [1-9]\d* checks', stdout)
    regular = '|'.join(['FIVE'] * 6 + ['ZERO', 'MULTI', 'ERROR', '', 'COPY', 'NO_COLUMNS', 'BINARY'])
    if scenario == 'early':
        assert len(prefixes) == roots and progress.is_set()
        expected = {'EARLY|X': roots}
    elif scenario:
        query = {'cancel': 'WAIT', 'bad': 'BAD', 'eof': 'EOF', 'bad_command': 'BAD_COMMAND'}[scenario]
        expected = {query + '|!': roots}
    elif engine == 'blocking':
        expected = {regular + '|X': 1, 'LIMIT|X': 1, 'BIG|!': 1}
    elif engine == 'native':
        expected = {regular + '|X': 1}
    else:
        expected = {regular + '|DEFER|X': roots, 'LIMIT|X': roots, 'BIG|!': roots}
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
    if args.native:
        run(args.native, 'native')
    for mode in modes:
        run(args.executable, mode + ':early')
        scenarios = ['eof', 'bad', 'bad_command']
        if mode != 'blocking':
            scenarios.insert(0, 'cancel')
        for scenario in scenarios:
            run(args.executable, mode + ':' + scenario)
    if args.native:
        run(args.native, 'native:early')


if __name__ == '__main__':
    main()
