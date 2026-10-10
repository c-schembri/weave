import argparse
import base64
import collections
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import tempfile

# name, selected method, password requested, missing, authentication complete, connection success
CASES = [
    ('trust', 0, False, False, True, True), ('denied', 0, False, False, False, False),
    ('unknown', 0, False, False, False, False), ('truncated', 0, False, False, False, False),
    ('password_policy', 1, True, False, False, False), ('password_missing', 1, True, True, False, False),
    ('md5_success', 2, True, False, True, True), ('md5_reject', 2, True, False, False, False),
    ('md5_missing', 2, True, True, False, False), ('scram_success', 3, True, False, True, True),
    ('scram_reject', 3, True, False, False, False), ('scram_missing', 3, True, True, False, False),
    ('scram_keys', 3, True, True, True, True), ('scram_unknown', 3, False, False, False, False),
    ('scram_malformed', 3, False, False, False, False), ('scram_bad_nonce', 3, True, False, False, False),
    ('scram_bad_final', 3, True, False, False, False), ('scram_eof', 3, True, False, False, False),
    ('after_auth_denied', 3, True, False, True, False), ('trust_then_denied', 0, False, False, True, False),
    ('reset_success', 3, True, False, True, True), ('reset_reject', 3, True, False, False, False),
    ('reset_keys', 3, True, True, True, True), ('scram_cancel', 3, True, False, False, False),
]
NATIVE_EXCLUDED = {'scram_malformed', 'scram_cancel'}


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        data = stream.recv(size - len(result))
        if not data:
            raise EOFError
        result.extend(data)
    return bytes(result)


def message(kind, data=b''):
    return kind + struct.pack('!I', len(data) + 4) + data


def auth(method, data=b''):
    return message(b'R', struct.pack('!I', method) + data)


def receive(stream):
    kind = exact(stream, 1)
    size = struct.unpack('!I', exact(stream, 4))[0]
    if not 4 <= size <= 4096:
        raise RuntimeError('Unexpected frontend size')
    return kind, exact(stream, size - 4)


def error():
    return message(b'E', b'SFATAL\0C28P01\0Mfixture authentication rejected\0\0')


def ready():
    return message(b'S', b'client_encoding\0UTF8\0') + message(b'Z', b'I')


def finish(stream):
    kind, body = receive(stream)
    if kind != b'X' or body:
        raise RuntimeError('Successful session did not finish')


def session(stream, mode, native, events, ordinal):
    stream.settimeout(15)
    size = struct.unpack('!I', exact(stream, 4))[0]
    if not 8 <= size <= 4096:
        raise RuntimeError('Invalid startup size')
    startup = exact(stream, size - 4)
    if startup[:4] not in (struct.pack('!I', 196608), struct.pack('!I', 196610)):
        raise RuntimeError('Unexpected negotiation request')
    events.append('startup')
    if mode.startswith('reset'):
        fields = startup[4:].split(b'\0')
        if fields[-2:] != [b'', b'']:
            raise RuntimeError('Invalid Startup parameters')
        options = dict(zip(fields[:-2:2], fields[1:-2:2]))
        initial = ordinal == 0 if native else options.get(b'application_name') == b'auth-info-initial'
        if initial:
            stream.sendall(auth(0) + ready())
            try:
                kind, body = receive(stream)
                if kind != b'X' or body:
                    raise RuntimeError('Unexpected request before reset')
                events.append('retired_terminate')
            except EOFError:
                events.append('retired_eof')
            return
        if not native and options.get(b'application_name') != b'auth-info-reset':
            raise RuntimeError('Reset did not replace effective options')
        mode = {'reset_success': 'scram_success', 'reset_keys': 'scram_keys', 'reset_reject': 'scram_reject'}[mode]
    if mode == 'trust':
        stream.sendall(auth(0) + ready())
        finish(stream)
    elif mode == 'denied':
        stream.sendall(error())
    elif mode == 'unknown':
        stream.sendall(auth(999))
    elif mode == 'truncated':
        stream.sendall(message(b'R', b'\0\0'))
    elif mode == 'trust_then_denied':
        stream.sendall(auth(0) + error())
    elif mode.startswith('password'):
        stream.sendall(auth(3))
        if native and mode == 'password_policy':
            kind, body = receive(stream)
            if kind != b'p' or body != b'secret-source\0':
                raise RuntimeError('Invalid native cleartext response')
            stream.sendall(error())
        else:
            if stream.recv(1):
                raise RuntimeError('Rejected password challenge leaked a response')
    elif mode.startswith('md5'):
        salt = b'1234'
        stream.sendall(auth(5, salt))
        if native and mode == 'md5_missing':
            if stream.recv(1):
                raise RuntimeError('Missing native password sent bytes')
        else:
            kind, body = receive(stream)
            password = b'' if mode == 'md5_missing' else b'secret-source'
            inner = hashlib.md5(password + b'test').hexdigest().encode()
            expected = b'md5' + hashlib.md5(inner + salt).hexdigest().encode() + b'\0'
            if kind != b'p' or body != expected:
                raise RuntimeError('Invalid MD5 response')
            if mode == 'md5_success':
                stream.sendall(auth(0) + ready())
                finish(stream)
            else:
                stream.sendall(error())
    else:
        mechanisms = b'NOT-SUPPORTED\0\0' if mode == 'scram_unknown' else b'SCRAM-SHA-256\0\0'
        if mode == 'scram_malformed':
            mechanisms = b'SCRAM-SHA-256\0'
        stream.sendall(auth(10, mechanisms))
        if mode in ('scram_unknown', 'scram_malformed') or (native and mode == 'scram_missing'):
            if stream.recv(1):
                raise RuntimeError('Rejected SASL challenge sent bytes')
        else:
            kind, initial = receive(stream)
            name, rest = initial.split(b'\0', 1)
            length = struct.unpack('!I', rest[:4])[0]
            client_first = rest[4:]
            if kind != b'p' or name != b'SCRAM-SHA-256' or length != len(client_first):
                raise RuntimeError('Invalid SASL initial response')
            if not client_first.startswith(b'n,,n=,r='):
                raise RuntimeError('Unexpected SCRAM initial grammar')
            events.append('sasl_initial')
            if mode == 'scram_cancel':
                stream.sendall(message(b'N', b'SNOTICE\0MCANCEL\0\0'))
                if stream.recv(1):
                    raise RuntimeError('Cancellation produced further authentication bytes')
                events.append('cancel_drained')
                return
            if mode == 'scram_eof':
                return
            if mode in ('scram_reject', 'scram_missing'):
                stream.sendall(error())
            else:
                first = client_first[3:]
                nonce = first.split(b',r=', 1)[1] + b'peer'
                if mode == 'scram_bad_nonce':
                    nonce = b'wrong-peer-nonce'
                salt = b'metadata-salt'
                server_first = b'r=' + nonce + b',s=' + base64.b64encode(salt) + b',i=4096'
                stream.sendall(auth(11, server_first))
                if mode == 'scram_bad_nonce':
                    if stream.recv(1):
                        raise RuntimeError('Invalid nonce produced a proof')
                else:
                    kind, client_final = receive(stream)
                    bare, proof = client_final.rsplit(b',p=', 1)
                    if kind != b'p' or bare != b'c=biws,r=' + nonce:
                        raise RuntimeError('Invalid SCRAM final grammar')
                    salted = hashlib.pbkdf2_hmac('sha256', b'secret-source', salt, 4096)
                    client_key = hmac.digest(salted, b'Client Key', 'sha256')
                    server_key = hmac.digest(salted, b'Server Key', 'sha256')
                    transcript = first + b',' + server_first + b',' + bare
                    signature = hmac.digest(hashlib.sha256(client_key).digest(), transcript, 'sha256')
                    expected = bytes(a ^ b for a, b in zip(client_key, signature))
                    if not hmac.compare_digest(base64.b64decode(proof, validate=True), expected):
                        raise RuntimeError('Incorrect proof or passthrough key use')
                    events.append('sasl_proof')
                    signature = hmac.digest(server_key, transcript, 'sha256')
                    if mode == 'scram_bad_final':
                        stream.sendall(auth(12, b'v=' + base64.b64encode(bytes(32))))
                        if stream.recv(1):
                            raise RuntimeError('Invalid server proof produced bytes')
                    else:
                        response = auth(12, b'v=' + base64.b64encode(signature)) + auth(0)
                        response += error() if mode == 'after_auth_denied' else ready()
                        stream.sendall(response)
                        if mode != 'after_auth_denied':
                            finish(stream)
    events.append('done')


def run(executable, engine, case):
    mode, method, requested, missing, complete, success = case
    native = engine == 'native'
    roots = 1 if engine in ('context', 'blocking', 'native') else 16
    workers, events, errors = [], [], []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(128)
        listener.settimeout(20)

        def handle(stream, ordinal):
            try:
                with stream:
                    session(stream, mode, native, events, ordinal)
            except Exception as failure:
                errors.append(repr(failure))

        def accept():
            try:
                for ordinal in range(roots * (2 if mode.startswith('reset') else 1)):
                    stream, _ = listener.accept()
                    worker = threading.Thread(target=handle, args=(stream, ordinal))
                    workers.append(worker)
                    worker.start()
            except Exception as failure:
                errors.append(repr(failure))

        acceptor = threading.Thread(target=accept)
        acceptor.start()
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith('PG')}
        if native:
            env.pop('ASAN_OPTIONS', None)
        try:
            with tempfile.TemporaryDirectory(prefix='weave-pg-auth-info-') as directory:
                password_file = Path(directory) / 'empty.pgpass'
                password_file.write_bytes(b'')
                password_file.chmod(0o600)
                arguments = [str(executable), str(listener.getsockname()[1]), engine, mode]
                if native:
                    arguments.append(str(password_file))
                result = subprocess.run(arguments, env=env, capture_output=True, text=True, timeout=60)
            assert not password_file.exists()
        finally:
            acceptor.join(timeout=25)
            for worker in workers:
                worker.join(timeout=20)
        assert not acceptor.is_alive() and not any(worker.is_alive() for worker in workers)
    records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    expected = dict(success=success, requested=requested, missing=missing)
    if not native:
        expected.update(method=method, complete=complete)
    actual = collections.Counter(json.dumps(record, sort_keys=True) for record in records)
    wanted = collections.Counter({json.dumps(expected, sort_keys=True): roots})
    evidence = dict(mode=mode, engine=engine, returncode=result.returncode, stdout=result.stdout,
        stderr=result.stderr, peer_errors=errors, connections=len(workers), matches=actual == wanted,
        events=dict(collections.Counter(events)), password_file_removed=not password_file.exists())
    print(json.dumps(evidence), flush=True)
    connections = roots * (2 if mode.startswith('reset') else 1)
    assert result.returncode == 0 and not result.stderr and not errors and len(workers) == connections
    assert actual == wanted and events.count('startup') == connections
    if mode.startswith('reset'):
        assert events.count('retired_eof') + events.count('retired_terminate') == roots
    if mode == 'scram_cancel':
        assert events.count('cancel_drained') == roots
    if not native:
        assert 'Authentication metadata passed:' in result.stdout
        assert 'OpenSSL: OpenSSL ' in result.stdout
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
            if args.native and case[0] in NATIVE_EXCLUDED:
                continue
            if engine == 'blocking' and case[0] == 'scram_cancel':
                continue
            run(args.executable, engine, case)


if __name__ == '__main__':
    main()
