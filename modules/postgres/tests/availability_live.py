"""Fresh PostgreSQL availability fixture, optionally with an owned Kerberos realm."""
import argparse
from collections import Counter
from contextlib import ExitStack
import json
import os
from pathlib import Path
import queue
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


def run(command, environment, data=None):
    try:
        result = subprocess.run(command, env=environment, input=data, capture_output=True, text=True, timeout=45)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Fixture command timed out: {Path(command[0]).name}") from None
    if result.returncode:
        # Setup arguments may contain credentials. Do not print their command line.
        raise RuntimeError(f"Fixture command failed: {Path(command[0]).name}: {result.stderr}")
    return result


def free_port():
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        return reservation.getsockname()[1]


def config_path(path):
    return str(path).replace('\\', '/').replace("'", "''")


def validate_server_log(report, kerberos):
    lines = report['server_log'].splitlines()
    statements = [line for line in lines if 'LOG:  statement:' in line]
    if len(statements) != (7 if kerberos else 6) or any('|psql|' not in line for line in statements):
        raise RuntimeError('Unexpected query execution or incomplete fixture SQL evidence')

    authorized = []
    for line in lines:
        if 'connection authorized:' in line:
            match = re.search(r'application_name=(health_\S+)', line)
            if match:
                authorized.append(match.group(1))
    counts = Counter(authorized)
    if not counts:
        raise RuntimeError('Missing positive authorization controls')

    allowed = set()
    for control in report['controls']:
        mode, engine = control['mode'], control['engine']
        application = f'health_{mode}_{engine}'
        permitted = mode in ('trust', 'local', 'gss_require', 'gss_prefer', 'gss_capture_default', 'gss_reuse',
                             'gss_unwrap_cancel') or (mode == 'gss_auth' and engine == 'native')
        if not permitted:
            continue
        allowed.add(application)
        if mode == 'gss_unwrap_cancel':
            continue
        expected = 4 if mode == 'gss_reuse' else 1
        if engine not in ('context', 'blocking', 'native'):
            expected = 16
        if counts[application] != expected:
            raise RuntimeError(f'Incomplete positive authorization evidence: {application}')
    if set(counts) - allowed:
        raise RuntimeError('An availability probe answered a PostgreSQL authentication challenge')

    denials = [line for line in lines if 'connection requires a valid client certificate' in line]
    if not denials:
        raise RuntimeError('Missing actual client-certificate rejection')
    report['authorization_counts'] = dict(counts)
    report['fixture_statement_count'] = len(statements)
    report['client_certificate_denials'] = len(denials)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    parser.add_argument('--baseline')
    parser.add_argument('--libpq-version', type=int)
    parser.add_argument('--server-bin', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--runtime', action='store_true')
    parser.add_argument('--windows-client', action='store_true')
    parser.add_argument('--kerberos', action='store_true')
    parser.add_argument('--delay', type=Path)
    parser.add_argument('--asan', type=Path)
    parser.add_argument('--kdc', type=Path)
    parser.add_argument('--admin', type=Path)
    parser.add_argument('--database', type=Path)
    parser.add_argument('--kinit', type=Path)
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('The disposable backend fixture requires Linux; Windows clients can run through WSL')
    if args.output and args.output.exists():
        raise RuntimeError('Refusing to overwrite evidence')
    if bool(args.baseline) != bool(args.libpq_version):
        parser.error('The native baseline requires its explicit expected libpq version')
    if args.windows_client and args.kerberos:
        parser.error('Windows domain qualification cannot use a Linux credential cache')
    if args.kerberos and not args.delay:
        parser.error('Kerberos qualification requires the owned provider-delay control')
    if args.kerberos and not all((args.kdc, args.admin, args.database, args.kinit)):
        parser.error('Kerberos qualification requires explicit native fixture tool paths')
    report = {'success': False, 'controls': [], 'fixture': '', 'certificates': '', 'fixture_absent': False,
              'certificates_absent': False, 'server_stopped': False, 'kdc_stopped': False}
    environment = {k: v for k, v in os.environ.items() if not k.upper().startswith(('PG', 'KRB5', 'LD_PRELOAD', 'WEAVE_PROBE_'))}
    environment.update(PGPASSFILE=os.devnull, LC_ALL='C')

    def checkpoint():
        if args.output:
            args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')

    checkpoint()
    helper = subprocess.Popen([args.executable, 'fixture'], env=environment, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    paths = queue.Queue()

    def read_paths():
        for _ in range(7):
            paths.put(helper.stdout.readline().strip())

    reader = threading.Thread(target=read_paths)
    reader.start()
    certificates = []
    server_certificates = []
    started = False
    kdc = None
    kdc_log = None
    stopping = threading.Event()
    forbidden_connections = []
    forbidden_worker = None
    pg_ctl = str(args.server_bin / 'pg_ctl')
    fixture_directory = ''
    try:
        certificates = [paths.get(timeout=30) for _ in range(7)]
        if not all(certificates):
            raise RuntimeError('Certificate helper failed')
        reader.join(5)
        if reader.is_alive():
            raise RuntimeError('Certificate helper output did not drain')
        server_certificates = certificates
        if args.windows_client:
            server_certificates = [run(['wslpath', '-u', p], environment).stdout.strip() for p in certificates]
        else:
            for private_key in (certificates[2], certificates[4]):
                Path(private_key).chmod(0o600)
        report['certificates'] = str(Path(server_certificates[0]).parent)

        with tempfile.TemporaryDirectory(prefix='weave-health-live-') as directory, ExitStack() as owned:
            root = Path(directory)
            fixture_directory = str(root)
            report['fixture'] = fixture_directory
            checkpoint()
            closed = owned.enter_context(socket.socket())
            closed.bind(('0.0.0.0' if args.windows_client else '127.0.0.1', 0))
            closed_port = closed.getsockname()[1]
            forbidden = owned.enter_context(socket.socket())
            forbidden.bind(('127.0.0.1', 0))
            forbidden.listen(128)
            forbidden.settimeout(0.1)
            forbidden_port = forbidden.getsockname()[1]

            def accept_forbidden():
                while not stopping.is_set():
                    try:
                        client, _ = forbidden.accept()
                    except socket.timeout:
                        continue
                    with client:
                        forbidden_connections.append(client.getpeername())

            forbidden_worker = threading.Thread(target=accept_forbidden)
            forbidden_worker.start()

            def stop_observer():
                stopping.set()
                forbidden_worker.join(5)
                if forbidden_worker.is_alive():
                    raise RuntimeError('Forbidden-endpoint observer did not drain')

            owned.callback(stop_observer)
            address = '127.0.0.1'
            clients = ['127.0.0.1']
            if args.windows_client:
                interfaces = json.loads(run(['ip', '-j', '-4', 'address', 'show', 'dev', 'eth0'], environment).stdout)
                address = interfaces[0]['addr_info'][0]['local']
                routes = json.loads(run(['ip', '-j', '-4', 'route', 'show', 'default'], environment).stdout)
                clients.append(routes[0]['gateway'])
            port = free_port()
            data = root / 'data'
            password = secrets.token_urlsafe(24)
            cache = str(root)
            if args.kerberos:
                kdc_port = free_port()
                (root / 'krb5.conf').write_text(
                    '[libdefaults]\n default_realm = WEAVE.TEST\n dns_lookup_kdc = false\n dns_lookup_realm = false\n'
                    ' rdns = false\n dns_canonicalize_hostname = false\n udp_preference_limit = 1\n qualify_shortname = ""\n'
                    f'[realms]\n WEAVE.TEST = {{\n kdc = 127.0.0.1:{kdc_port}\n }}\n'
                    '[domain_realm]\n localhost = WEAVE.TEST\n')
                (root / 'kdc.conf').write_text(
                    f'[kdcdefaults]\n kdc_ports = {kdc_port}\n kdc_tcp_ports = {kdc_port}\n'
                    f'[realms]\n WEAVE.TEST = {{\n database_name = {root}/principal\n key_stash_file = {root}/stash\n'
                    f' acl_file = {root}/acl\n }}\n')
                cache = f'FILE:{root}/client.ccache'
                environment.update(KRB5_CONFIG=str(root / 'krb5.conf'), KRB5_KDC_PROFILE=str(root / 'kdc.conf'),
                                   KRB5CCNAME=cache, KRB5_KTNAME=f'FILE:{root}/server.keytab')
                run([str(args.database), 'create', '-s', '-r', 'WEAVE.TEST', '-P', secrets.token_urlsafe(32)], environment)
                run([str(args.admin), '-r', 'WEAVE.TEST', '-q', f'addprinc -pw {password} client@WEAVE.TEST'], environment)
                run([str(args.admin), '-r', 'WEAVE.TEST', '-q', 'addprinc -randkey postgres/localhost@WEAVE.TEST'], environment)
                run([str(args.admin), '-r', 'WEAVE.TEST', '-q', f'ktadd -k {root}/server.keytab postgres/localhost@WEAVE.TEST'], environment)
                (root / 'server.keytab').chmod(0o600)
                kdc_log = (root / 'kdc.log').open('w')
                kdc = subprocess.Popen([str(args.kdc), '-n', '-r', 'WEAVE.TEST'], env=environment,
                                       stdout=kdc_log, stderr=kdc_log)
                for _ in range(100):
                    if kdc.poll() is not None:
                        raise RuntimeError('Owned KDC exited')
                    with socket.socket() as probe:
                        if probe.connect_ex(('127.0.0.1', kdc_port)) == 0:
                            break
                    time.sleep(0.02)
                else:
                    raise RuntimeError('Owned KDC did not start')
                run([str(args.kinit), '-f', 'client@WEAVE.TEST'], environment, password + '\n')

            run([str(args.server_bin / 'initdb'), '-D', str(data), '-U', 'administrator', '--auth=trust', '--no-locale'], environment)
            private_key = root / 'server.key'
            shutil.copyfile(server_certificates[2], private_key)
            private_key.chmod(0o600)
            config = (f"\nport={port}\nlisten_addresses='127.0.0.1{',' + address if args.windows_client else ''}'\n"
                      f"unix_socket_directories='{config_path(root)}'\nssl=on\n"
                      f"ssl_cert_file='{config_path(server_certificates[1])}'\nssl_key_file='{config_path(private_key)}'\n"
                      f"ssl_ca_file='{config_path(server_certificates[0])}'\nmax_connections=100\n"
                      "log_connections='all'\nlog_statement='all'\nlog_line_prefix='%p|%a|%u|%d| '\n")
            if args.kerberos:
                config += f"krb_server_keyfile='{config_path(root / 'server.keytab')}'\n"
            with (data / 'postgresql.conf').open('a') as stream:
                stream.write(config)
            hba = 'local all all trust\n'
            for client in clients:
                hba += (f'host all blocked {client}/32 reject\n'
                        f'host all administrator {client}/32 trust\n')
            if args.kerberos:
                hba += ('hostgssenc all client 127.0.0.1/32 gss include_realm=0 krb_realm=WEAVE.TEST\n'
                        'hostnogssenc all client 127.0.0.1/32 gss include_realm=0 krb_realm=WEAVE.TEST\n')
            for client in clients:
                hba += (f'hostssl all weave_password {client}/32 password clientcert=verify-ca\n'
                        f'hostssl all all {client}/32 scram-sha-256 clientcert=verify-ca\n'
                        f'hostnossl all weave_md5 {client}/32 md5\n'
                        f'hostnossl all all {client}/32 scram-sha-256\n')
            (data / 'pg_hba.conf').write_text(hba)
            try:
                started = True
                run([pg_ctl, '-D', str(data), '-l', str(root / 'server.log'), '-w', 'start'], environment)
                roles = (f"CREATE ROLE weave LOGIN PASSWORD '{password}';\n"
                         f"CREATE ROLE weave_password LOGIN PASSWORD '{password}';\n"
                         "CREATE ROLE client LOGIN;\nSET password_encryption='md5';\n"
                         f"CREATE ROLE weave_md5 LOGIN PASSWORD '{password}';\n")
                psql = str(args.server_bin / 'psql')
                run([psql, '-h', str(root), '-p', str(port), '-U', 'administrator', '-d', 'postgres',
                     '-X', '-v', 'ON_ERROR_STOP=1'], environment, roles)
                healthy = run([psql, '-h', str(root), '-p', str(port), '-U', 'administrator', '-d', 'postgres',
                               '-X', '-Atc', 'SELECT 42'], environment)
                if healthy.stdout.strip() != '42':
                    raise RuntimeError('Real backend fixture control failed')
                if args.kerberos:
                    protected = run([psql, f'host=localhost hostaddr=127.0.0.1 port={port} user=client dbname=postgres '
                                     'gssencmode=require sslmode=disable require_auth=gss', '-X', '-Atc',
                                     'SELECT encrypted FROM pg_stat_gssapi WHERE pid=pg_backend_pid()'], environment)
                    if protected.stdout.strip() != 't':
                        raise RuntimeError('Protected backend fixture control failed')
                    report['native_protected_transport'] = True
                modes = ['scram', 'md5', 'password', 'tls', 'mtls', 'no_certificate', 'untrusted',
                         'wrong_user', 'wrong_database', 'trust', 'hba_reject', 'refused', 'retry', 'unstarted']
                if not args.windows_client:
                    modes += ['local', 'local_wrong_user']
                engines = ['context', 'blocking']
                if args.runtime:
                    engines += ['affine', 'stealing']
                    if args.windows_client:
                        engines += ['shared_affine', 'shared_stealing']
                combinations = [(mode, engine) for engine in engines for mode in modes]
                combinations += [('pre_cancel', engine) for engine in engines if engine != 'blocking']
                if args.baseline:
                    combinations += [(mode, 'native') for mode in modes if mode not in ('unstarted', 'local_wrong_user')]
                if args.kerberos:
                    gss_modes = ['gss_auth', 'gss_require', 'gss_prefer', 'gss_missing', 'gss_wrong_service', 'gss_capture_default']
                    combinations += [(mode, engine) for engine in engines for mode in gss_modes]
                    if args.baseline:
                        combinations += [(mode, 'native') for mode in gss_modes if mode != 'gss_capture_default']
                    combinations += [('gss_reuse', engine) for engine in ('context', 'blocking')]
                    combinations += [(mode, 'context') for mode in ('gss_cancel', 'gss_deadline', 'gss_wrap_cancel', 'gss_unwrap_cancel')]
                for mode, engine in combinations:
                    user = {'md5': 'weave_md5', 'password': 'weave_password', 'trust': 'administrator',
                            'wrong_user': 'absent_role', 'hba_reject': 'blocked', 'local': 'administrator',
                            'local_wrong_user': 'administrator'}.get(mode, 'client' if mode.startswith('gss_') else 'weave')
                    ca = certificates[5] if mode == 'untrusted' else certificates[0]
                    current_cache = f'FILE:{root}/absent' if mode == 'gss_missing' else cache
                    if mode.startswith('local'):
                        current_cache = str(root)
                    child_environment = dict(environment)
                    if mode == 'gss_missing':
                        child_environment['KRB5CCNAME'] = current_cache
                    if mode in ('gss_auth', 'gss_cancel', 'gss_deadline', 'gss_wrap_cancel', 'gss_unwrap_cancel') and engine != 'native':
                        child_environment['LD_PRELOAD'] = f'{args.asan}:{args.delay}' if args.asan else str(args.delay)
                        child_environment['WEAVE_PROBE_RECORD_DELAY' if mode in ('gss_wrap_cancel', 'gss_unwrap_cancel') else 'WEAVE_PROBE_DELAY'] = '1'
                    before = len(forbidden_connections)
                    executable = args.baseline if engine == 'native' else args.executable
                    result = subprocess.run([executable, str(port), mode, engine, str(closed_port), ca,
                                             certificates[3], certificates[4], user, address, current_cache,
                                             str(args.libpq_version or 0), str(forbidden_port)], env=child_environment,
                                            capture_output=True, text=True, timeout=45)
                    record = {'mode': mode, 'engine': engine, 'returncode': result.returncode,
                              'stdout': result.stdout, 'stderr': result.stderr,
                              'forbidden_connections': len(forbidden_connections) - before}
                    report['controls'].append(record)
                    checkpoint()
                    print(json.dumps(record), flush=True)
                    if result.returncode or record['forbidden_connections']:
                        raise RuntimeError(f'Availability control failed: {mode}/{engine}')
                actual = [(control['mode'], control['engine']) for control in report['controls']]
                if not actual or actual != combinations or len(set(actual)) != len(actual):
                    raise RuntimeError('Incomplete or duplicate control inventory')
                report['profiles_passed'] = True
            finally:
                if started or (data / 'postmaster.pid').exists():
                    run([pg_ctl, '-D', str(data), '-m', 'immediate', '-w', 'stop'], environment)
                    report['server_stopped'] = True
                started = False
                server_log = root / 'server.log'
                if server_log.exists():
                    report['server_log'] = server_log.read_text(errors='replace').replace(password, '<fixture-password>')
                stop_observer()
                forbidden.setblocking(False)
                while True:
                    try:
                        client, _ = forbidden.accept()
                    except BlockingIOError:
                        break
                    with client:
                        forbidden_connections.append(client.getpeername())
                report['total_forbidden_connections'] = len(forbidden_connections)
                if kdc and kdc.poll() is None:
                    kdc.terminate()
                    try:
                        kdc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        kdc.kill()
                        kdc.wait(timeout=10)
                if kdc:
                    report['kdc_stopped'] = kdc.poll() is not None
                if kdc_log:
                    kdc_log.close()
                    kdc_log = None
                checkpoint()
        report['fixture_absent'] = not Path(fixture_directory).exists()
    finally:
        stopping.set()
        if forbidden_worker:
            forbidden_worker.join(5)
        if kdc and kdc.poll() is None:
            kdc.terminate()
            kdc.wait(timeout=10)
        if kdc_log:
            kdc_log.close()
        try:
            output, error = helper.communicate('\n', timeout=10)
        except subprocess.TimeoutExpired:
            helper.kill()
            output, error = helper.communicate(timeout=10)
        reader.join(5)
        report['certificate_helper_exit'] = helper.returncode
        report['certificate_helper_stderr'] = error
        report['certificates_absent'] = bool(server_certificates) and not Path(server_certificates[0]).parent.exists()
        if fixture_directory:
            report['fixture_absent'] = not Path(fixture_directory).exists()
        checkpoint()
    if helper.returncode or not report['certificates_absent'] or not report['fixture_absent'] or not report['server_stopped']:
        raise RuntimeError('Owned fixture cleanup failed')
    if args.kerberos and not report['kdc_stopped']:
        raise RuntimeError('Owned KDC did not drain')
    if report['total_forbidden_connections']:
        raise RuntimeError('Unexpected security-error host failover')
    validate_server_log(report, args.kerberos)
    report['success'] = True
    checkpoint()


if __name__ == '__main__':
    main()
