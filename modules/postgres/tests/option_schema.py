"""Qualify static descriptors and isolated environment loading, without printing secrets."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

PARSED = {
    'host', 'hostaddr', 'port', 'user', 'dbname', 'password', 'scram_client_key', 'scram_server_key',
    'application_name', 'fallback_application_name', 'options', 'client_encoding', 'connect_timeout',
    'channel_binding', 'target_session_attrs', 'replication', 'keepalives', 'keepalives_idle',
    'keepalives_interval', 'keepalives_count', 'tcp_user_timeout', 'require_auth', 'oauth_issuer',
    'oauth_client_id', 'oauth_scope', 'oauth_client_secret', 'min_protocol_version', 'max_protocol_version',
    'sslmode', 'sslrootcert', 'sslcert', 'sslkey', 'sslpassword', 'sslcrl', 'sslcrldir', 'ssl_min_protocol_version',
    'ssl_max_protocol_version', 'sslnegotiation', 'sslsni', 'sslcertmode', 'gssencmode', 'krbsrvname', 'gsslib', 'gssdelegation', 'load_balance_hosts'}
LOADED = {'service', 'passfile', 'requirepeer'}
UNSUPPORTED = {'servicefile', 'sslcompression', 'ssl', 'requiressl'}
PARSED.add('sslkeylogfile')
UNRECOGNIZED = set()
SECRETS = {'password', 'sslpassword', 'scram_client_key', 'scram_server_key', 'oauth_client_secret', 'sslkeylogfile'}
DEFAULTS = dict(host='localhost', port='5432', user='', dbname='', password='', application_name='weave',
    options='', client_encoding='UTF8', connect_timeout='30', channel_binding='prefer', target_session_attrs='any',
    replication='false', keepalives='1', keepalives_idle='0', keepalives_interval='0', keepalives_count='0',
    tcp_user_timeout='0', min_protocol_version='3.0', max_protocol_version='3.2', sslmode='verify-full',
    sslrootcert='system', ssl_min_protocol_version='TLSv1.2', ssl_max_protocol_version='TLSv1.3', sslnegotiation='postgres', sslsni='1', sslcertmode='allow',
    gssencmode='disable', krbsrvname='postgres', gssdelegation='0', load_balance_hosts='disable')
ENVIRONMENT = dict(host='PGHOST', hostaddr='PGHOSTADDR', port='PGPORT', dbname='PGDATABASE', user='PGUSER',
    password='PGPASSWORD', passfile='PGPASSFILE', service='PGSERVICE', options='PGOPTIONS', application_name='PGAPPNAME',
    connect_timeout='PGCONNECT_TIMEOUT', client_encoding='PGCLIENTENCODING', target_session_attrs='PGTARGETSESSIONATTRS',
    load_balance_hosts='PGLOADBALANCEHOSTS', channel_binding='PGCHANNELBINDING', sslmode='PGSSLMODE', sslcert='PGSSLCERT',
    sslkey='PGSSLKEY', sslrootcert='PGSSLROOTCERT', sslcrl='PGSSLCRL', ssl_min_protocol_version='PGSSLMINPROTOCOLVERSION',
    ssl_max_protocol_version='PGSSLMAXPROTOCOLVERSION', require_auth='PGREQUIREAUTH', sslnegotiation='PGSSLNEGOTIATION',
    sslcompression='PGSSLCOMPRESSION', sslcertmode='PGSSLCERTMODE', sslcrldir='PGSSLCRLDIR', sslsni='PGSSLSNI',
    requirepeer='PGREQUIREPEER', gssencmode='PGGSSENCMODE', krbsrvname='PGKRBSRVNAME', gsslib='PGGSSLIB',
    gssdelegation='PGGSSDELEGATION', min_protocol_version='PGMINPROTOCOLVERSION', max_protocol_version='PGMAXPROTOCOLVERSION',
    servicefile='PGSERVICEFILE', requiressl='PGREQUIRESSL')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    parser.add_argument('--native')
    args = parser.parse_args()
    base = {key: value for key, value in os.environ.items() if not key.upper().startswith('PG')}

    def run(executable, arguments, environment):
        try:
            result = subprocess.run([executable, *arguments], env=environment, capture_output=True, text=True,
                timeout=30, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        except subprocess.TimeoutExpired as error:
            print(json.dumps(dict(executable=executable, arguments=arguments, timed_out=True,
                stdout=(error.stdout or b'').decode(errors='replace'),
                stderr=(error.stderr or b'').decode(errors='replace'))), flush=True)
            raise
        print(json.dumps(dict(executable=executable, arguments=arguments, exit=result.returncode,
            stdout=result.stdout, stderr=result.stderr)), flush=True)
        assert result.returncode == 0 and not result.stderr
        return result.stdout.splitlines()

    with tempfile.TemporaryDirectory(prefix='weave-option-schema-') as directory:
        base.update(HOME=directory, APPDATA=directory)
        weave = {}
        for line in run(args.executable, [], base):
            fields = line.split('\t')
            if fields[0] == 'option':
                _, key, variable, default, support, secret = fields
                assert key not in weave
                weave[key] = dict(environment=variable, default=None if default == '\\N' else default,
                    support=int(support), secret=bool(int(secret)))
        expected = {key: 0 for key in PARSED} | {key: 1 for key in LOADED} | {key: 2 for key in UNSUPPORTED}
        expected |= {key: 3 for key in UNRECOGNIZED}
        assert {key: value['support'] for key, value in weave.items()} == expected
        assert {key: value['environment'] for key, value in weave.items() if value['environment']} == ENVIRONMENT
        assert {key for key, value in weave.items() if value['secret']} == SECRETS
        assert all(value['default'] == DEFAULTS.get(key) for key, value in weave.items())

        if args.native:
            native_environment = dict(base)
            native_environment.pop('ASAN_OPTIONS', None)
            native = {}
            for line in run(args.native, [], native_environment):
                fields = line.split('\t')
                if fields[0] == 'option':
                    _, key, variable, default, display = fields
                    assert key not in native
                    native[key] = dict(environment=variable, default=None if default == '\\N' else default,
                        display=display)
            assert set(native) == set(weave) - {'servicefile', 'ssl', 'requiressl'}
            for key, value in native.items():
                assert value['environment'] == weave[key]['environment'], key
                if value['display'] == '*':
                    assert weave[key]['secret'], key
            differences = {key: dict(native=value['default'], weave=weave[key]['default'])
                for key, value in native.items() if value['default'] != weave[key]['default']}
            print(json.dumps(dict(schema_count=len(weave), native_count=len(native),
                default_differences=differences)), flush=True)
        else:
            service = Path(directory) / 'service.conf'
            service.write_text('[schema-service]\nuser=service-user\n', encoding='utf-8')
            password_file = Path(directory) / 'passwords'
            password_file.write_text('*:5432:*:*:file-password\n', encoding='utf-8')
            password_file.chmod(0o600)
            values = dict(host='schema-host', hostaddr='127.0.0.2', port='5437', user='schema-user', dbname='schema-db',
                password='schema-password', application_name='schema-app', options='-csearch_path=', client_encoding='LATIN1',
                connect_timeout='17', channel_binding='disable', target_session_attrs='read-write', require_auth='scram-sha-256',
                min_protocol_version='latest', max_protocol_version='3.0', sslmode='disable', sslrootcert='schema-root',
                sslcert='schema-cert', sslkey='schema-key', sslcrl='schema-crl', ssl_min_protocol_version='TLSv1.3',
                ssl_max_protocol_version='TLSv1.2', gssencmode='require', krbsrvname='schema-service',
                gsslib='sspi' if os.name == 'nt' else 'gssapi', gssdelegation='1', load_balance_hosts='random',
                service='schema-service', servicefile=str(service), passfile=str(password_file), sslcompression='0',
                sslcrldir='schema-crldir', sslsni='0', sslnegotiation='direct', sslcertmode='require', requiressl='1')
            if os.name == 'nt':
                values['requirepeer'] = 'schema-user'
            else:
                import pwd
                values['requirepeer'] = pwd.getpwuid(os.geteuid()).pw_name
            for key, variable in ENVIRONMENT.items():
                environment = dict(base)
                environment[variable] = values[key]
                if key in {'service', 'servicefile'}:
                    environment.update(PGSERVICE='schema-service', PGSERVICEFILE=str(service))
                lines = run(args.executable, [key], environment)
                assert len(lines) == 1 and lines[0].startswith('environment\t' + key + '\t')
    assert not Path(directory).exists()
    print('PASS static option schema' + (' and pinned native inventory' if args.native else ' and environment loading'),
        flush=True)


if __name__ == '__main__':
    main()
