"""Owned foreground OpenLDAP fixture; never install/start a system service."""
import argparse
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from ldap_peer import Observer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--tools", type=Path, required=True)
    parser.add_argument("--windows-client", action="store_true")
    parser.add_argument("--libpq-control", action="store_true")
    args = parser.parse_args()
    if args.windows_client and args.libpq_control:
        raise RuntimeError("libpq control runs in a separate native Linux process")
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(args.tools / "usr/lib/x86_64-linux-gnu")
    addresses = ["127.0.0.1"]
    if args.windows_client:
        result = subprocess.run(["ip", "-j", "-4", "address", "show", "dev", "eth0"],
                                check=True, capture_output=True, text=True)
        addresses.append(json.loads(result.stdout)[0]["addr_info"][0]["local"])
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        unavailable = reservation.getsockname()[1]
    referral = Observer()
    stall = Observer(("127.0.0.1", port), b"cn=normal,dc=weave,dc=test")
    host = addresses[-1]
    completed = False
    try:
        run_fixture(args, env, addresses, port, unavailable, host, referral, stall)
        completed = True
    finally:
        try:
            stall.close()
        finally:
            referral.close()
        print("LDAP observer evidence: " + json.dumps({"referral_connections": referral.accepted,
              "stall_connections": stall.accepted, "stalled_searches": stall.stalled,
              "zero_entries": stall.zero_entries,
              "operations": stall.operations}), flush=True)
        if completed and (referral.accepted != 0 or stall.stalled != 1 or stall.zero_entries != 1):
            raise RuntimeError("Referral target contacted or search timeout not exercised")
        print("Owned LDAP observers drained", flush=True)


def run_fixture(args, env, addresses, port, unavailable, host, referral, stall):
    with tempfile.TemporaryDirectory(prefix="weave-ldap-fixture-") as directory:
        root = Path(directory)
        data = root / "data"
        data.mkdir()
        config = root / "slapd.conf"
        config.write_text(
            f'include "{args.tools}/etc/ldap/schema/core.schema"\n'
            f'pidfile "{root}/server.pid"\nargsfile "{root}/server.args"\n'
            f'modulepath "{args.tools}/usr/lib/ldap"\nmoduleload back_mdb\n'
            f'database mdb\nmaxsize 10485760\nsuffix "dc=weave,dc=test"\n'
            f'directory "{data}"\naccess to * by * read\n')
        entries = ["dn: dc=weave,dc=test\nobjectClass: dcObject\nobjectClass: organization\ndc: weave\no: Weave"]
        valid_limit = ("application_name=" + "v" * 65000 + "\n") * 16 + "sslmode=disable\n"
        valid_limit += " " * (1024 * 1024 - 1 - len(valid_limit))
        cases = {
            "normal": ("description", "host=127.0.0.1\nuser=directory_user\nuser=ignored\ndbname=directory_database\n"
                       "password='directory secret'\nsslmode=disable\napplication_name=directory"),
            "empty": ("description", "password=\nuser=empty_user\nsslmode=disable"),
            "bad": ("description", "unknown=invalid"),
            "nested": ("description", "service=nested"),
            "nul": ("userPassword", "user=bad\0sslmode=disable"),
            "large": ("description", "x" * 65537),
            "aggregate": ("userPassword", "x" * (1024 * 1024 + 1)),
            "zero": ("description", "user=replace_me"),
            "utf8": ("description", "user=directory_user\napplication_name='caf\u00e9'\nsslmode=disable"),
            "valid-limit": ("description", valid_limit),
        }
        for name, (attribute, value) in cases.items():
            encoded = base64.b64encode(value.encode()).decode()
            entries.append(f"dn: cn={name},dc=weave,dc=test\nobjectClass: device\nobjectClass: extensibleObject\n"
                           f"cn: {name}\n{attribute}:: {encoded}")
        entries.append("dn: cn=missing,dc=weave,dc=test\nobjectClass: device\ncn: missing")
        entries.append("dn: cn=multi,dc=weave,dc=test\nobjectClass: device\ncn: multi\n"
                       "description: user=multi_user\ndescription: dbname=multi_database\n"
                       "description: sslmode=disable")
        many = "\n".join(f"description: application_name=value{index}" for index in range(1025))
        entries.append("dn: cn=many,dc=weave,dc=test\nobjectClass: device\ncn: many\n" + many)
        allowed = "\n".join(f"description: application_name=value{index}" for index in range(1024))
        entries.append("dn: cn=allowed,dc=weave,dc=test\nobjectClass: device\ncn: allowed\n" + allowed)
        separator_values = []
        for character in ["a", "b"]:
            value = ("application_name=" + character * 65000 + "\n") * 8
            value += " " * (512 * 1024 - len(value))
            separator_values.append("description:: " + base64.b64encode(value.encode()).decode())
        entries.append("dn: cn=separators,dc=weave,dc=test\nobjectClass: device\ncn: separators\n" +
                       "\n".join(separator_values))
        entries.append("dn: cn=referral,dc=weave,dc=test\nobjectClass: referral\nobjectClass: extensibleObject\n"
                       f"cn: referral\nref: ldap://{host}:{referral.port}/cn=normal,dc=weave,dc=test")
        entries.append("dn: cn=caf\u00e9,dc=weave,dc=test\nobjectClass: device\ncn: caf\u00e9\n"
                       "description: user=unicode_user\ndescription: sslmode=disable")
        ldif = root / "entries.ldif"
        ldif.write_text("\n\n".join(entries) + "\n\n")
        imported = subprocess.run([str(args.tools / "usr/sbin/slapadd"), "-f", str(config), "-l", str(ldif)],
                                  env=env, capture_output=True, text=True, timeout=30)
        if imported.returncode:
            print(imported.stdout, imported.stderr, flush=True)
            imported.check_returncode()
        listeners = " ".join(f"ldap://{address}:{port}/" for address in addresses)
        with (root / "server.log").open("w") as log:
            server = subprocess.Popen([str(args.tools / "usr/sbin/slapd"), "-f", str(config), "-h", listeners, "-d", "0"],
                                      env=env, stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if server.poll() is not None:
                        raise RuntimeError("Owned LDAP server exited: " + (root / "server.log").read_text())
                    with socket.socket() as readiness:
                        readiness.settimeout(.05)
                        if readiness.connect_ex(("127.0.0.1", port)) == 0:
                            break
                    time.sleep(.05)
                else:
                    raise RuntimeError("Owned LDAP readiness deadline")
                client_root = Path(__file__).resolve().parents[3] / "build" if args.windows_client else directory
                if args.windows_client:
                    Path(client_root).mkdir(exist_ok=True)
                with tempfile.TemporaryDirectory(prefix="weave-ldap-client-", dir=client_root) as client_directory:
                    client_path = client_directory
                    if args.windows_client:
                        client_path = subprocess.run(["wslpath", "-w", client_directory], check=True,
                                                     capture_output=True, text=True).stdout.strip()
                    command = [args.executable, client_path, f"ldap://{host}:{port}",
                               f"ldap://{host}:{unavailable}", "enabled", f"ldap://{host}:{stall.port}"]
                    subprocess.run(command, env=env, check=True, timeout=60)
                    if args.libpq_control:
                        subprocess.run(["python3", str(Path(__file__).with_name("ldap_libpq.py")), client_directory,
                                        f"ldap://{host}:{port}", f"ldap://{host}:{unavailable}"],
                                       env=env, check=True, timeout=60)
            finally:
                server.terminate()
                try:
                    server.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                print("Owned LDAP fixture drained", flush=True)


if __name__ == "__main__":
    main()
