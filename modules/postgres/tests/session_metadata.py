import argparse
import collections
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading

CASES = [
    ('absent', None, 0), ('empty', '', 0), ('18', '18.4', 180004), ('legacy', '9.6.2', 90602),
    ('legacy_devel', '9.6devel', 90600), ('devel', '19devel', 190000), ('beta', '19beta4', 190000),
    ('rc', '18rc1', 180000), ('vendor', '18.6 (Debian build)', 180006), ('unknown', 'unknown', 0),
    ('major', '18', 180000), ('three', '18.4.1', 180401), ('partial', '18.', 180000),
    ('partial_three', '18.4.', 180004), ('suffix', '18.4abc', 180004), ('leading', ' \t+18.4', 180004),
    ('component_space', '18. +4', 180004), ('before_dot', '18 .4', 180000),
    ('signed', '-18.4', 0), ('signed_minor', '18.-4', 0),
    ('overflow', '999999999999999999999999', 0), ('overflow_minor', '18.999999999999999999999999', 0),
    ('overflow_number', '214749.0', 0), ('boundary', '214748.3647', 2147483647),
]
DIFFERENT = {'signed', 'signed_minor', 'overflow', 'overflow_minor', 'overflow_number'}


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


def status(name, value):
    return message(b'S', name.encode() + b'\0' + value.encode() + b'\0')


def expected(phase, version, number):
    return dict(phase=phase, version=number, raw=None if version is None else version.encode().hex(),
        options=f'-c metadata.fixture={"reset" if phase == 2 else "initial"}'.encode().hex(),
        empty=('filled' if phase == 1 else '').encode().hex(),
        value=('' if phase == 1 else 'initial').encode().hex(), absent=None)


def session(stream, version, events):
    stream.settimeout(15)
    size = struct.unpack('!I', exact(stream, 4))[0]
    if not 8 <= size <= 4096:
        raise RuntimeError('Invalid startup size')
    startup = exact(stream, size - 4)
    fields = startup[4:].split(b'\0')
    if fields[-2:] != [b'', b'']:
        raise RuntimeError('Invalid startup termination')
    values = dict(zip(fields[:-2:2], fields[1:-2:2]))
    options = values.get(b'options')
    if options not in (b'-c metadata.fixture=initial', b'-c metadata.fixture=reset'):
        raise RuntimeError('Missing effective startup options')
    events.append(options.decode())
    response = message(b'R', struct.pack('!I', 0)) + status('client_encoding', 'UTF8')
    response += status('weave.fixture.empty', '') + status('weave.fixture.value', 'initial')
    if version is not None:
        response += status('server_version', version)
    stream.sendall(response + message(b'Z', b'I'))
    queried = False
    while True:
        try:
            kind = exact(stream, 1)
        except EOFError:
            if options.endswith(b'initial') and queried:
                events.append('retired_eof')
                return
            raise
        size = struct.unpack('!I', exact(stream, 4))[0]
        if not 4 <= size <= 4096:
            raise RuntimeError('Invalid query size')
        body = exact(stream, size - 4)
        if kind == b'X' and not body:
            events.append('retired_terminate' if options.endswith(b'initial') else 'finished_reset')
            return
        if kind != b'Q' or body != b'NOOP\0' or options.endswith(b'reset'):
            raise RuntimeError('Unexpected request reached peer')
        events.append('query')
        queried = True
        response = message(b'N', b'SNOTICE\0MMETADATA\0\0')
        response += status('weave.fixture.empty', 'filled') + status('weave.fixture.value', '')
        stream.sendall(response + message(b'C', b'SET\0') + message(b'Z', b'I'))


def run(executable, engine, case):
    name, version, number = case
    errors, events, workers = [], [], []
    roots = 1 if engine in ('context', 'blocking', 'native') else 16
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(128)
        listener.settimeout(20)

        def handle(stream):
            try:
                with stream:
                    session(stream, version, events)
            except Exception as error:
                errors.append(repr(error))

        def accept():
            try:
                for _ in range(2 * roots):
                    stream, _ = listener.accept()
                    worker = threading.Thread(target=handle, args=(stream,))
                    workers.append(worker)
                    worker.start()
            except Exception as error:
                errors.append(repr(error))

        acceptor = threading.Thread(target=accept)
        acceptor.start()
        environment = dict(os.environ)
        if engine == 'native':
            environment = {key: value for key, value in environment.items() if not key.startswith('PG')}
            environment.pop('ASAN_OPTIONS', None)
        try:
            result = subprocess.run([str(executable), str(listener.getsockname()[1]), engine],
                env=environment, capture_output=True, text=True, timeout=60)
        finally:
            acceptor.join(timeout=25)
            for worker in workers:
                worker.join(timeout=20)
        if acceptor.is_alive() or any(worker.is_alive() for worker in workers):
            raise RuntimeError('Owned peer threads did not drain')
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    reference = [expected(phase, version, number) for phase in range(3)]
    actual = collections.Counter(json.dumps(row, sort_keys=True) for row in rows)
    wanted = collections.Counter({json.dumps(row, sort_keys=True): roots for row in reference})
    record = dict(case=name, engine=engine, returncode=result.returncode, stdout=result.stdout,
        stderr=result.stderr, peer_errors=errors, connections=len(workers), rows=len(rows), matches=actual == wanted,
        events=dict(collections.Counter(events)))
    print(json.dumps(record), flush=True)
    assert result.returncode == 0 and not errors and len(workers) == roots * 2
    retirement = 'retired_terminate' if engine == 'native' else 'retired_eof'
    assert record['events'] == {
        '-c metadata.fixture=initial': roots, '-c metadata.fixture=reset': roots,
        'query': roots, retirement: roots, 'finished_reset': roots}
    assert record['matches']
    if engine != 'native':
        assert not result.stderr and 'Metadata controls passed:' in result.stdout
        assert 'OpenSSL: ' in result.stdout
        if roots > 1:
            scheduler = 'affine' if engine.endswith('affine') else 'stealing'
            layout = 'shared' if engine.startswith('shared') else 'sharded'
            assert f'Runtime: workers=4 scheduler={scheduler} io={layout} roots=16' in result.stdout
    else:
        assert 'libpq: ' in result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--runtime', action='store_true')
    args = parser.parse_args()
    engines = ['native'] if args.native else ['context', 'blocking']
    if not args.native and args.runtime:
        engines += ['affine', 'stealing']
    if not args.native and args.runtime and os.name == 'nt':
        engines += ['shared_affine', 'shared_stealing']
    for engine in engines:
        for case in CASES:
            if args.native and case[0] in DIFFERENT:
                continue
            run(args.executable, engine, case)


if __name__ == '__main__':
    main()
