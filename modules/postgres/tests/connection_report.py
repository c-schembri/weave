import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import tempfile


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


def session(stream, mode, native):
    stream.settimeout(15)
    size = struct.unpack('!I', exact(stream, 4))[0]
    body = exact(stream, size - 4)
    if mode in ['tls', 'gss', 'gss_native']:
        request = 80877103 if mode == 'tls' else 80877104
        if body != struct.pack('!I', request):
            raise RuntimeError('expected security request')
        stream.sendall(b'G' if mode == 'gss_native' else b'N')
        if stream.recv(1):
            raise RuntimeError('security rejection leaked startup or proof')
        return
    if not 8 <= size <= 4096:
        raise RuntimeError('bad startup size')
    if mode == 'reset_fail' and b'user\0fail\0' in body:
        mode = 'sql'
    if mode == 'eof':
        return
    if mode == 'deadline':
        if stream.recv(1):
            raise RuntimeError('deadline leaked authentication')
        return
    if mode == 'cancel':
        stream.sendall(message(b'N', b'SNOTICE\0MCANCEL\0\0'))
        if stream.recv(1):
            raise RuntimeError('cancel leaked authentication')
        return
    if mode == 'malformed':
        stream.sendall(message(b'R', b'\0'))
        return
    if mode == 'password':
        stream.sendall(message(b'R', struct.pack('!I', 3)))
        if stream.recv(1):
            raise RuntimeError('cleartext password leaked')
        return
    if mode in ['sql', 'huge']:
        text = b'denied' if mode == 'sql' else b'x' * (1024 * 1024 + 100)
        stream.sendall(message(b'E', b'SFATAL\0V FATAL\0C28000\0M' + text + b'\0\0'))
        return
    stream.sendall(message(b'R', struct.pack('!I', 0)) + message(b'Z', b'I'))
    while True:
        try:
            kind = exact(stream, 1)
        except EOFError:
            return
        size = struct.unpack('!I', exact(stream, 4))[0]
        body = exact(stream, size - 4)
        if kind == b'X' and not body:
            return
        if kind != b'Q' or not body.endswith(b'\0'):
            raise RuntimeError('unexpected request')
        if mode == 'target_eof':
            return
        if mode in ['target_sql', 'target_protocol', 'target_cancel_after_sql']:
            response = message(b'E', b'SERROR\0VERROR\0C42501\0Mdenied\0\0')
        else:
            names = [b'readonly'] if native else [b'readonly', b'recovery']
            values = [b'off']
            if not native:
                values.append(b'f')
            columns = struct.pack('!H', len(names))
            for name in names:
                columns += name + b'\0' + struct.pack('!IhIhih', 0, 0, 25, -1, -1, 0)
            row = struct.pack('!H', len(values))
            for value in values:
                row += struct.pack('!I', len(value)) + value
            response = message(b'T', columns) + message(b'D', row) + message(b'C', b'SELECT 1\0')
        if mode == 'target_cancel_after_sql':
            stream.sendall(response + message(b'N', b'SNOTICE\0MCANCEL\0\0'))
            if stream.recv(1):
                raise RuntimeError('cancel leaked next request')
            return
        stream.sendall(response + message(b'Z', b'X' if mode == 'target_protocol' else b'I'))
        if mode == 'target_protocol':
            if stream.recv(1):
                raise RuntimeError('protocol failure leaked next request')
            return


def run(executable, mode, engine):
    stop = threading.Event()
    errors = []
    workers = []
    with socket.socket() as listener, socket.socket() as refused:
        listener.bind(('127.0.0.1', 0))
        listener.listen(128)
        listener.settimeout(0.1)
        refused.bind(('127.0.0.1', 0))

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
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith('PG')}
        env.update(PGPASSFILE=os.devnull, LC_ALL='C')
        try:
            with tempfile.TemporaryDirectory(prefix='weave-report-cache-') as temporary:
                argv = [str(executable), str(listener.getsockname()[1]),
                    str(refused.getsockname()[1]), mode, engine]
                if mode == 'gss_native':
                    argv.append('FILE:' + str(Path(temporary) / 'missing'))
                result = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=90)
        finally:
            stop.set()
            thread.join(timeout=20)
            for worker in workers:
                worker.join(timeout=20)
        if thread.is_alive() or any(worker.is_alive() for worker in workers):
            raise RuntimeError('fixture did not drain')
    roots = 16 if engine in ['affine', 'stealing', 'shared'] else 1
    count = 0 if mode in ['refused', 'invalid', 'addresses_refused'] else 1
    if mode in ['target_reject', 'reset_success', 'reset_fail']:
        count = 2
    if mode == 'prefer':
        count = 3
    if engine == 'native' and mode == 'target_sql':
        count = 2
    record = dict(mode=mode, engine=engine, returncode=result.returncode, stdout=result.stdout,
        stderr=result.stderr, peer_errors=errors, connections=len(workers), expected_connections=roots * count)
    print(json.dumps(record), flush=True)
    if result.returncode or errors or len(workers) != roots * count:
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--runtime', action='store_true')
    parser.add_argument('--gss', action='store_true')
    args = parser.parse_args()
    if not args.native:
        formatted = subprocess.run([str(args.executable), 'format'], capture_output=True, text=True, timeout=15)
        print(json.dumps(dict(mode='format', returncode=formatted.returncode,
            stdout=formatted.stdout, stderr=formatted.stderr)), flush=True)
        if formatted.returncode:
            raise SystemExit(formatted.returncode)
    engines = ['native'] if args.native else ['context', 'blocking']
    if args.runtime and not args.native:
        engines += ['affine', 'stealing']
        if os.name == 'nt':
            engines += ['shared']
    for engine in engines:
        modes = ['success', 'refused', 'addresses_refused', 'fallback', 'sql', 'malformed', 'eof', 'password', 'target_sql',
            'target_reject', 'target_eof', 'target_protocol', 'prefer', 'tls', 'invalid', 'deadline', 'huge',
            'reset_success', 'reset_fail']
        if args.gss:
            modes += ['gss']
            if os.name != 'nt':
                modes += ['gss_native']
        if engine != 'blocking':
            modes += ['cancel', 'target_cancel_after_sql', 'busy']
        if args.native:
            modes = ['success', 'refused', 'addresses_refused', 'fallback', 'sql', 'eof', 'target_sql', 'target_reject', 'prefer']
        for mode in modes:
            run(args.executable, mode, engine)


if __name__ == '__main__':
    main()
