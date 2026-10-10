import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading


def message(kind, body=b''):
    return kind + struct.pack('!I', len(body) + 4) + body


def exact(stream, count):
    output = bytearray()
    while len(output) < count:
        data = stream.recv(count - len(output))
        if not data:
            raise EOFError
        output.extend(data)
    return bytes(output)


def cases():
    samples = []
    common = dict(S='ERROR', V='ERROR', C='42601', M='primary', D='detail', H='hint', W='context',
        s='schema', t='table', c='column', d='type', n='constraint', R='function', F='source.cpp', L='123')

    def add(text, fields, encoding=6, encoding_name='UTF8', fatal=True):
        query = (f'PROBE {len(samples)} ' + text).encode('ascii')
        body = b''.join(code.encode('ascii') + (value.encode('utf8') if isinstance(value, str) else value) + b'\0'
            for code, value in fields.items()) + b'\0'
        samples.append(dict(query=query, body=body, encoding=encoding, encoding_name=encoding_name, fatal=fatal))

    add('plain', common)
    for severity, fatal in [('NOTICE', False), ('WARNING', False), ('INFO', False), ('ERROR', True), ('FATAL', True), ('PANIC', True)]:
        fields = dict(common, S=severity, V=severity)
        fields['C'] = '42601' if fatal else '00000'
        add('severity', fields, fatal=fatal)
    add('localized', dict(common, S='ERREUR'))
    add('missing state', {'S': 'NOTICE', 'M': 'notice', 'W': 'context'}, fatal=False)
    add('empty values', dict(common, D='', H='', W='', s='', t='', c='', d='', n='', R='', F='', L=''))
    for removed in [('R',), ('F',), ('L',), ('F', 'L'), ('R', 'F'), ('R', 'L'), ('R', 'F', 'L')]:
        add('source', {key: value for key, value in common.items() if key not in removed})
    add('unknown', dict(common, X='unknown field'))

    for text in ['', 'abc', 'a\tb', 'a\r\nb', 'a\rb\nc', 'x' * 200, '\n', '\r\n', 'a\n' * 15]:
        # Every character position, EOF and one beyond; include the generated command prefix.
        prefix = len(f'PROBE {len(samples)} ')
        for position in range(1, len(text) + prefix + 3):
            add(text, dict(common, P=str(position)))
    for text in ['abc', 'a\tb', 'a\r\nb', 'x' * 200, '\u4e2d\u4e2dabc', 'e\u0301xy', '\U0001f600xyz',
        '\u4e2d' * 90 + 'abc', '\u4e2d' * 25 + 'x' * 100, ('\U0001f600\u0301' * 60) + 'x']:
        for position in range(1, len(text) + 3):
            add('internal', dict(common, p=str(position), q=text))
    add('both positions', dict(common, P='2', p='1', q='internal'))
    add('internal no query', dict(common, p='3'))
    add('position zero padding', dict(common, P='0002'))

    names = ['SQL_ASCII', 'EUC_JP', 'EUC_CN', 'EUC_KR', 'EUC_TW', 'EUC_JIS_2004', 'UTF8', 'MULE_INTERNAL',
        'LATIN1', 'LATIN2', 'LATIN3', 'LATIN4', 'LATIN5', 'LATIN6', 'LATIN7', 'LATIN8', 'LATIN9', 'LATIN10',
        'WIN1256', 'WIN1258', 'WIN866', 'WIN874', 'KOI8R', 'WIN1251', 'WIN1252', 'ISO_8859_5', 'ISO_8859_6',
        'ISO_8859_7', 'ISO_8859_8', 'WIN1250', 'WIN1253', 'WIN1254', 'WIN1255', 'WIN1257', 'KOI8U', 'SJIS',
        'BIG5', 'GBK', 'UHC', 'GB18030', 'JOHAB', 'SHIFT_JIS_2004']
    characters = {1: b'\xa1\xa1', 2: b'\xa1\xa1', 3: b'\xa1\xa1', 4: b'\x8e\xa1\xa1\xa1',
        5: b'\xa1\xa1', 6: b'\xe4\xb8\xad', 7: b'\x81\xa1', 35: b'\x83\x41', 36: b'\x81\x41',
        37: b'\x81\x41', 38: b'\x81\x41', 39: b'\x81\x30\x81\x30', 40: b'\xa1\xa1', 41: b'\x83\x41'}
    for encoding, name in enumerate(names):
        character = characters.get(encoding, b'\xc0')
        for position in [1, 2, 5, 30, 55, 65, 81, 82, 83]:
            add('encoding', dict(common, p=str(position), q=character * 80 + b'xy'), encoding, name)
    return samples


def session(stream, samples):
    stream.settimeout(30)
    size = struct.unpack('!I', exact(stream, 4))[0]
    if not 8 <= size <= 65536:
        raise RuntimeError('invalid startup')
    exact(stream, size - 4)
    stream.sendall(message(b'R', struct.pack('!I', 0)) + message(b'S', b'client_encoding\0UTF8\0') +
        message(b'S', b'server_version\0' + b'18.6\0') + message(b'Z', b'I'))
    while True:
        try:
            kind = exact(stream, 1)
        except EOFError:
            return
        size = struct.unpack('!I', exact(stream, 4))[0]
        if not 4 <= size <= 65536:
            raise RuntimeError('invalid request bound')
        body = exact(stream, size - 4)
        if kind == b'X' and not body:
            return
        if kind != b'Q' or not body.endswith(b'\0'):
            raise RuntimeError('invalid request')
        number = int(body.split(b' ', 2)[1])
        sample = samples[number]
        if body[:-1] != sample['query']:
            raise RuntimeError('query did not match fixture')
        status = message(b'S', b'client_encoding\0' + sample['encoding_name'].encode() + b'\0')
        error = message(b'E' if sample['fatal'] else b'N', sample['body'])
        command = b'' if sample['fatal'] else message(b'C', b'SET\0')
        stream.sendall(status + error + command + message(b'Z', b'I'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--libpq-version', required=True)
    args = parser.parse_args()
    samples = cases()
    fixture = struct.pack('!I', len(samples))
    for sample in samples:
        fixture += bytes([sample['encoding'], sample['fatal']]) + sample['query'] + b'\0'
        fixture += struct.pack('!I', len(sample['body'])) + sample['body']
    errors = []
    stop = threading.Event()
    clients = []
    with tempfile.TemporaryDirectory(prefix='weave-pg-diagnostic-') as temporary:
        path = Path(temporary) / 'cases.bin'
        path.write_bytes(fixture)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(0.2)

            def accept():
                while not stop.is_set():
                    try:
                        stream, _ = listener.accept()
                    except socket.timeout:
                        continue

                    def client(connection):
                        try:
                            with connection:
                                session(connection, samples)
                        except Exception as error:
                            errors.append(repr(error))
                    thread = threading.Thread(target=client, args=(stream,))
                    clients.append(thread)
                    thread.start()

            thread = threading.Thread(target=accept)
            thread.start()
            environment = dict(os.environ)
            for key in list(environment):
                if key.upper().startswith('PG'):
                    del environment[key]
            environment['PGPASSFILE'] = os.devnull
            environment['LC_ALL'] = 'C'
            try:
                result = subprocess.run([str(args.executable), str(listener.getsockname()[1]), str(path), args.libpq_version],
                    text=True, encoding="utf-8", errors="backslashreplace", env=environment, capture_output=True, timeout=180)
            finally:
                stop.set()
                thread.join(timeout=35)
                for client in clients:
                    client.join(timeout=35)
            if thread.is_alive() or any(client.is_alive() for client in clients):
                raise RuntimeError('fixture did not drain')
    print(json.dumps(dict(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr,
        peer_errors=errors, cases=len(samples), temporary=temporary, fixture_absent=not Path(temporary).exists())), flush=True)
    if result.returncode or errors or Path(temporary).exists():
        raise SystemExit(1)


if __name__ == '__main__':
    main()
