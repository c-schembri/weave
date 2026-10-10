import argparse
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import threading
import subprocess

spec = importlib.util.spec_from_file_location('peer', Path(__file__).with_name('results.py'))
peer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peer)
original = peer.session
PHASES = ['row_read', 'copy_read', 'copy_write', 'exchange_read', 'notification',
    'function', 'pipeline_duplex', 'pipeline_split', 'batch', 'encoding']
sessions = []
session_lock = threading.Lock()


def session(stream, mode):
    with session_lock:
        sessions.append(mode)
    mode = mode.split('@')[0]
    if mode.startswith('blocking_'):
        mode = mode.removeprefix('blocking_')
    if mode not in PHASES:
        return original(stream, mode)
    stream.settimeout(15)
    exact = peer.base.exact
    message = peer.base.message
    size = struct.unpack('!I', exact(stream, 4))[0]
    if not 8 <= size <= 1024:
        raise RuntimeError('invalid startup')
    exact(stream, size - 4)
    response = message(b'R', struct.pack('!I', 0)) + peer.base.status(b'UTF8') + message(b'Z', b'I')
    if mode == 'notification':
        stream.sendall(response + message(b'?', b''))
        if stream.recv(1):
            raise RuntimeError('unexpected notification request')
        return
    stream.sendall(response)
    while True:
        kind = exact(stream, 1)
        size = struct.unpack('!I', exact(stream, 4))[0]
        if not 4 <= size <= 4096:
            raise RuntimeError('invalid query bound')
        body = exact(stream, size - 4)
        if kind in (b'Q', b'F', b'S', b'H'):
            break
    if mode == 'copy_read':
        stream.sendall(message(b'H', b'\0\0\0') + message(b'?'))
    elif mode == 'copy_write':
        stream.sendall(message(b'G', b'\0\0\0'))
        if exact(stream, 1) != b'd':
            raise RuntimeError('missing COPY write')
        linger = struct.pack('HH' if os.name == 'nt' else 'ii', 1, 0)
        stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        return
    elif mode == 'row_read':
        stream.sendall(message(b'T', b'\0\0') + message(b'?'))
    else:
        stream.sendall(message(b'?'))
    while stream.recv(4096):
        pass


peer.base.session = session


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--runtime', action='store_true')
    args = parser.parse_args()
    formatted = subprocess.run([str(args.executable), 'format'], capture_output=True, text=True, timeout=15)
    print(json.dumps(dict(mode='format', returncode=formatted.returncode,
        stdout=formatted.stdout, stderr=formatted.stderr)), flush=True)
    if formatted.returncode:
        raise SystemExit(formatted.returncode)
    modes = ['normal', 'blocking', 'blocking_eof', 'blocking_duplicate_error', 'cancel', 'duplicate_error', 'after_error', 'invalid_state',
        'truncated_error', 'bad_ready', 'oversized', 'retained_resource', 'eof', 'extended_error',
        'extended_extra_error', 'missing', 'unexpected_copy']
    modes += PHASES
    scenarios = [mode for mode in modes if mode != 'blocking' and not mode.startswith('blocking_')]
    engines = ['affine', 'stealing'] if args.runtime else []
    if args.runtime and os.name == 'nt':
        engines += ['shared_affine', 'shared_stealing']
    modes += [scenario + '@' + engine for scenario in scenarios for engine in engines]
    for mode in modes:
        sessions.clear()
        result = peer.base.run(args.executable, mode)
        result['connections'] = len(sessions)
        result['expected_connections'] = 32 if '@' in mode else 1
        if '@' in mode:
            engine = mode.split('@')[1]
            layout = 'shared' if engine.startswith('shared') else 'sharded'
            scheduler = 'affine' if engine.endswith('affine') else 'stealing'
            expected = f'Runtime: workers=4 scheduler={scheduler} io={layout} roots=32'
            result['configuration_matches'] = expected in result['stdout']
            if not result['configuration_matches']:
                result['configuration_error'] = expected
        print(json.dumps(result), flush=True)
        if result['returncode'] or result['peer_errors'] or result['connections'] != result['expected_connections'] or not result.get('configuration_matches', True):
            raise SystemExit(1)


if __name__ == '__main__':
    main()
