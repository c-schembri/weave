import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

import session_metadata


def run_values(executable, mode, directory, environment):
    result = subprocess.run([str(executable), mode, directory], env=environment,
        capture_output=True, text=True, timeout=60)
    print(json.dumps(dict(kind='values', mode=mode, returncode=result.returncode,
        stdout=result.stdout, stderr=result.stderr)), flush=True)
    assert result.returncode == 0 and not result.stderr
    assert 'Configuration values passed:' in result.stdout
    secrets = ('q' * 177, 'p' * 127, 'h' * 137, 't' * 147, 's' * 157)
    assert not any(secret in result.stdout + result.stderr for secret in secrets)


def run_native(executable, mode, directory, environment):
    environment = dict(environment)
    environment.pop('ASAN_OPTIONS', None)
    environment['PGSYSCONFDIR'] = directory
    if mode == 'builtin':
        environment = {key: value for key, value in environment.items() if not key.startswith('PG')}
    elif mode == 'missing':
        environment['PGSERVICE'] = 'missing'

    result = subprocess.run([str(executable), mode, 'defaults'], env=environment,
        capture_output=True, text=True, timeout=60)
    print(json.dumps(dict(kind='native', mode=mode, returncode=result.returncode,
        stdout=result.stdout, stderr=result.stderr)), flush=True)
    assert result.returncode == 0 and not result.stderr
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    assert len(rows) == 1 and rows[0]['kind'] == 'native-defaults'
    assert rows[0]['mode'] == mode.encode().hex() and len(rows[0]['fields']) == 41
    secrets = ('q' * 177, 'p' * 137)
    assert not any(secret in result.stdout + result.stderr for secret in secrets)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--values', type=Path)
    parser.add_argument('--session', type=Path)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--runtime', action='store_true')
    args = parser.parse_args()
    if not ((args.values and args.session) or args.native):
        parser.error('Provide both --values and --session, or --native')

    environment = {key: value for key, value in os.environ.items() if not key.startswith('PG')}
    with tempfile.TemporaryDirectory(prefix='weave-config-info-') as directory:
        root = Path(directory)
        service = ('[selected]\nhost=snapshot-host\nport=6543\nuser=snapshot-user\n'
            'dbname=snapshot-database\napplication_name=snapshot-app\n'
            'options=-c work_mem=4096\nconnect_timeout=7\nsslmode=disable\n')
        (root / 'passwords').write_text('snapshot-host:6543:snapshot-database:snapshot-user:' + 'q' * 177 + '\n')
        (root / 'passwords').chmod(0o600)
        (root / 'system.conf').write_text(service)
        (root / 'pg_service.conf').write_text(service)
        environment.update(HOME=directory, USERPROFILE=directory, APPDATA=directory,
            PGSERVICE='selected', PGSERVICEFILE=str(root / 'user.conf'), PGPASSFILE=str(root / 'passwords'),
            PGHOST='must-not-use.invalid', PGUSER='must-not-use', PGPASSWORD='q' * 177)

        modes = ('direct', 'builtin', 'user', 'system', 'environment', 'missing')
        for mode in modes:
            (root / 'user.conf').write_text('[other]\nuser=other\n' if mode == 'system' else service)
            if args.values:
                run_values(args.values, mode, directory, environment)
            if args.native and mode != 'direct':
                run_native(args.native, mode, directory, environment)

    engines = ['context', 'blocking']
    if args.runtime:
        engines += ['affine', 'stealing']
        if os.name == 'nt':
            engines += ['shared_affine', 'shared_stealing']
    case = ('configuration', '18.4', 180004)
    if args.session:
        for engine in engines:
            session_metadata.run(args.session, engine, case)
    if args.native:
        session_metadata.run(args.native, 'native', case)


if __name__ == '__main__':
    main()
