import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import queue
import socket
import ssl
import struct
import subprocess
import threading


def integer(value):
    return struct.pack('!I', value)


def frame(kind, body):
    return kind.encode() + integer(4 + len(body)) + body


def read_exactly(client, count):
    result = bytearray()
    while len(result) < count:
        chunk = client.recv(count - len(result))
        if not chunk:
            raise EOFError()
        result.extend(chunk)
    return bytes(result)


def error(state):
    return frame('E', b'SFATAL\0VFATAL\0C' + state.encode() + b'\0Mhealth fixture\0\0')


def serve(client, mode, engine, certificates):
    client.settimeout(5)
    size = struct.unpack('!I', read_exactly(client, 4))[0]
    packet = read_exactly(client, size - 4)
    if mode in ('tls12', 'tls13', 'tls_oauth', 'mtls12', 'mtls13', 'untrusted', 'expired', 'mtls_missing') or mode.startswith('tls13_'):
        assert size == 8 and packet == integer(80877103)
        client.sendall(b'S')
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        version = ssl.TLSVersion.TLSv1_2 if mode.endswith('12') else ssl.TLSVersion.TLSv1_3
        tls.minimum_version = tls.maximum_version = version
        expired = mode in ('expired', 'tls13_expired')
        tls.load_cert_chain(certificates[6] if expired else certificates[1], certificates[2])
        if mode.startswith('mtls'):
            tls.verify_mode = ssl.CERT_REQUIRED
            tls.load_verify_locations(cafile=certificates[0])
        try:
            with tls.wrap_socket(client, server_side=True) as secured:
                inner = mode.removeprefix('tls13_') if mode.startswith('tls13_') else 'password'
                if mode == 'tls_oauth':
                    inner = 'oauth'
                serve(secured, inner, engine, certificates)
            assert mode not in ('untrusted', 'expired', 'mtls_missing', 'tls13_expired')
        except ssl.SSLError as failure:
            assert mode in ('untrusted', 'expired', 'mtls_missing', 'tls13_expired')
            if mode == 'mtls_missing':
                assert failure.reason == 'PEER_DID_NOT_RETURN_A_CERTIFICATE', repr(failure)
        return
    if mode == 'ssl_refusal':
        assert size == 8 and packet == integer(80877103)
        client.sendall(b'N')
    else:
        assert 8 <= size <= 1024 and packet[:4] in (integer(196608), integer(196610))
        assert packet[-1:] == b'\0'
        fields = packet[4:-1].split(b'\0')
        assert fields[-1] == b'' and len(fields[:-1]) % 2 == 0
        parameters = dict(zip(fields[::2], fields[1::2]))
        assert parameters[b'user'] == b'probe' and parameters[b'database'] == b'probe'
        assert b's' * 96 not in packet
        auth = {'auth_ok': (0, b''), 'password': (3, b''), 'md5': (5, b'abcd'),
                'gss': (7, b''), 'sspi': (9, b''), 'scram': (10, b'SCRAM-SHA-256\0\0'),
                'oauth': (10, b'OAUTHBEARER\0\0'), 'unknown_sasl': (10, b'WEAVE-PROBE\0\0'),
                'retry': (3, b''), 'malformed_auth': (0, b'x'), 'malformed_md5': (5, b'abc'),
                'unterminated_sasl': (10, b'SCRAM-SHA-256\0'), 'empty_sasl': (10, b'\0'),
                'bad_sasl': (10, b'bad mechanism\0\0'), 'sasl_trailing': (10, b'SCRAM-SHA-256\0\0x'),
                'unsolicited_continue': (11, b'x'), 'unknown_auth': (999, b'')}
        if mode in auth:
            code, data = auth[mode]
            response = frame('R', integer(code) + data)
            if engine == 'native' and mode == 'auth_ok':
                response += frame('Z', b'I')
            client.sendall(response)
        elif mode in ('reject', 'bad_user', 'bad_db', 'sql_other'):
            state = {'reject': '57P03', 'bad_user': '28000', 'bad_db': '3D000', 'sql_other': '53300'}[mode]
            client.sendall(error(state))
        elif mode == 'eof':
            return
        elif mode == 'ready_first':
            client.sendall(frame('Z', b'I'))
        elif mode == 'malformed_error':
            client.sendall(frame('E', b'SFATAL\0C57P03\0Munterminated'))
        elif mode == 'malformed_no_state':
            client.sendall(frame('E', b'SFATAL\0Mhealth fixture\0\0'))
        elif mode == 'malformed_zero_state':
            client.sendall(error('00000'))
        elif mode == 'wrong_sqlstate':
            client.sendall(error('!7P03'))
        elif mode == 'huge_frame':
            client.sendall(b'R' + integer(1029))
        elif mode == 'short_header':
            client.sendall(b'R\0')
            return
        elif mode == 'short_body':
            client.sendall(b'R' + integer(8) + b'\0')
            return
        elif mode == 'timeout_header':
            client.sendall(b'R\0')
        elif mode == 'timeout_body':
            client.sendall(b'R' + integer(8) + b'\0')
        elif mode == 'negotiation':
            client.sendall(frame('v', integer(0) + integer(0)) + frame('R', integer(3)))
        elif mode == 'bad_version':
            client.sendall(frame('v', integer(3) + integer(0)))
        else:
            assert mode in ('silent', 'cancel'), mode
    received = bytearray()
    try:
        while chunk := client.recv(4096):
            received.extend(chunk)
    except ConnectionResetError:
        pass
    assert not received or (engine == 'native' and received == b'X\0\0\0\4'), bytes(received)


def control(executable, version, mode, engine, certificates):
    simultaneous = 16 if engine in ('affine', 'stealing', 'shared_affine', 'shared_stealing') else 1
    no_attempt = mode.startswith('invalid_') or mode in ('unstarted', 'pre_cancel', 'refused')
    recovery = {
        'recover_ready': ('reject', 'password'),
        'recover_reject': ('reject', 'reject'),
        'recover_refused': ('reject', None),
        'recover_eof': ('reject', 'eof'),
        'eof_ready': ('eof', 'password'),
        'recover_bad_user': ('reject', 'bad_user'),
        'recover_protocol': ('reject', 'malformed_auth'),
        'recover_tls_ready': ('tls13_reject', 'tls13'),
        'recover_tls_expired': ('tls13_reject', 'tls13_expired'),
        'random_ready': ('reject', 'password'),
        'timeout_ready': ('silent', 'password'),
        'cancel_recovery': ('reject', 'silent'),
        'protocol_ready': ('malformed_auth', 'password'),
        'resource_ready': ('huge_frame', 'password'),
        'secure_failure_ready': ('tls13_expired', 'tls13'),
        'ssl_refusal_ready': ('ssl_refusal', 'tls13'),
    }
    first, second = recovery.get(mode, (mode, None))
    expected = [0 if no_attempt else simultaneous, simultaneous if second else 0]
    if mode in ('protocol_ready', 'resource_ready', 'secure_failure_ready', 'ssl_refusal_ready'):
        expected[1] = 0
    if mode == 'eof_ready' and engine == 'native':
        # libpq stops on startup EOF; Weave's unauthenticated probe retries.
        expected[1] = 0
    errors = []
    connections = [[], []]
    workers = []
    stopping = threading.Event()
    with ExitStack() as owned:
        closed = owned.enter_context(socket.socket())
        listeners = [owned.enter_context(socket.socket()) for _ in range(2)]
        closed.bind(('127.0.0.1', 0))
        closed_port = closed.getsockname()[1]
        for listener in listeners:
            listener.bind(('127.0.0.1', 0))
            listener.listen(128)
            listener.settimeout(0.1)
        port, next_port = [listener.getsockname()[1] for listener in listeners]

        def worker(client, response):
            try:
                with client:
                    assert response is not None
                    serve(client, response, engine, certificates)
            except Exception as failure:
                errors.append(repr(failure))

        def accept(index, response):
            listener = listeners[index]
            while not stopping.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                connections[index].append(client)
                thread = threading.Thread(target=worker, args=(client, response))
                workers.append(thread)
                thread.start()

        acceptors = [threading.Thread(target=accept, args=(index, response))
                     for index, response in enumerate((first, second))]
        for acceptor in acceptors:
            acceptor.start()
        env = {name: value for name, value in os.environ.items() if not name.startswith('PG')}
        env.update(PGPASSFILE=os.devnull, LC_ALL='C')
        try:
            ca = certificates[5] if mode == 'untrusted' else certificates[0]
            result = subprocess.run([executable, str(port), mode, engine, str(version), str(closed_port),
                                     ca, certificates[3], certificates[4], str(next_port)],
                                    env=env, capture_output=True, text=True, timeout=30)
        finally:
            stopping.set()
            for acceptor in acceptors:
                acceptor.join(5)
            for thread in workers:
                thread.join(6)
            assert not any(acceptor.is_alive() for acceptor in acceptors)
            assert not any(thread.is_alive() for thread in workers)
        observed = [len(endpoint) for endpoint in connections]
        if mode == 'random_ready':
            assert 0 <= observed[0] <= simultaneous and observed[1] == simultaneous
            expected[0] = observed[0]
        record = {'mode': mode, 'engine': engine, 'returncode': result.returncode,
                  'stdout': result.stdout, 'stderr': result.stderr, 'peer_errors': errors,
                  'connections': sum(observed), 'expected_connections': sum(expected),
                  'endpoints': observed, 'expected_endpoints': expected}
        print(json.dumps(record), flush=True)
        assert result.returncode == 0 and not errors and observed == expected


parser = argparse.ArgumentParser()
parser.add_argument('--executable', required=True)
parser.add_argument('--libpq-version', type=int, default=0)
parser.add_argument('--runtime', action='store_true')
parser.add_argument('--native', action='store_true')
parser.add_argument('--mode')
parser.add_argument('--engine')
args = parser.parse_args()
modes = ['auth_ok', 'password', 'md5', 'gss', 'sspi', 'scram', 'oauth', 'unknown_sasl',
         'reject', 'bad_user', 'bad_db', 'sql_other', 'eof', 'silent', 'malformed_auth',
         'malformed_md5', 'unterminated_sasl', 'empty_sasl', 'bad_sasl', 'sasl_trailing',
         'unsolicited_continue', 'unknown_auth', 'ready_first', 'malformed_error', 'malformed_no_state',
         'malformed_zero_state', 'wrong_sqlstate',
         'huge_frame', 'short_header', 'short_body', 'timeout_header', 'timeout_body', 'negotiation',
         'bad_version', 'invalid_user', 'invalid_port', 'invalid_policy', 'unstarted', 'refused', 'retry', 'ssl_refusal',
         'tls12', 'tls13', 'tls_oauth', 'mtls12', 'mtls13', 'untrusted', 'expired', 'mtls_missing']
recovery_modes = ['recover_ready', 'recover_reject', 'recover_refused', 'recover_eof', 'eof_ready',
                  'recover_bad_user', 'recover_protocol', 'recover_tls_ready', 'recover_tls_expired',
                  'random_ready', 'timeout_ready', 'protocol_ready', 'resource_ready',
                  'secure_failure_ready', 'ssl_refusal_ready']
modes += recovery_modes
engines = [] if args.native else ['context', 'blocking']
if args.runtime and not args.native:
    engines += ['affine', 'stealing']
    if os.name == 'nt':
        engines += ['shared_affine', 'shared_stealing']
native_modes = ['auth_ok', 'password', 'md5', 'scram', 'unknown_sasl', 'reject', 'bad_user', 'bad_db',
                'sql_other', 'eof', 'refused', 'retry', 'tls12', 'tls13', 'mtls12', 'mtls13', 'untrusted',
                'expired', 'mtls_missing', 'ssl_refusal']
native_modes += ['recover_ready', 'recover_reject', 'recover_refused', 'recover_eof', 'eof_ready',
                 'recover_bad_user', 'recover_tls_ready', 'recover_tls_expired']
fixture = subprocess.Popen([args.executable, 'fixture'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
paths = queue.Queue()


def read_paths():
    for _ in range(7):
        paths.put(fixture.stdout.readline().strip())


reader = threading.Thread(target=read_paths)
reader.start()
certificates = []
try:
    certificates = [paths.get(timeout=30) for _ in range(7)]
    assert all(certificates)
    if os.name != 'nt':
        private_keys = (certificates[2], certificates[4])
        for private_key in private_keys:
            Path(private_key).chmod(0o600)
    reader.join(5)
    assert not reader.is_alive()
    for engine in engines:
        if args.engine and args.engine != engine:
            continue
        for mode in modes:
            if engine.startswith('shared_') and mode not in recovery_modes:
                continue
            if args.mode and args.mode != mode:
                continue
            control(args.executable, args.libpq_version, mode, engine, certificates)
        if engine != 'blocking' and not args.mode:
            for mode in ['cancel', 'pre_cancel', 'cancel_recovery']:
                control(args.executable, args.libpq_version, mode, engine, certificates)
    selected_native = native_modes if args.native else []
    for mode in selected_native:
        if args.engine and args.engine != 'native':
            continue
        if args.mode and args.mode != mode:
            continue
        control(args.executable, args.libpq_version, mode, 'native', certificates)
finally:
    try:
        stdout, stderr = fixture.communicate('\n', timeout=10)
    except subprocess.TimeoutExpired:
        fixture.kill()
        stdout, stderr = fixture.communicate(timeout=10)
    reader.join(5)
    directory = str(Path(certificates[0]).parent) if certificates else ''
    absent = bool(directory) and not Path(directory).exists()
    print(json.dumps({'fixture': directory, 'fixture_absent': absent, 'returncode': fixture.returncode,
                      'stderr': stderr, 'openssl': ssl.OPENSSL_VERSION}), flush=True)
    assert fixture.returncode == 0 and absent and not reader.is_alive()
