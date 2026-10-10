"""Optional isolated libpq LDAP controls; never a performance benchmark."""
import ctypes
import ctypes.util
import os
from pathlib import Path
import sys
from urllib.parse import urlsplit


def main():
    directory, endpoint, unavailable = sys.argv[1:]
    for key in list(os.environ):
        if key.startswith("PG"):
            del os.environ[key]
    os.environ["HOME"] = directory
    file = Path(directory) / "libpq-service.conf"
    os.environ["PGSERVICEFILE"] = str(file)
    library = ctypes.CDLL(ctypes.util.find_library("pq"))
    library.PQconnectStart.argtypes = [ctypes.c_char_p]
    library.PQconnectStart.restype = ctypes.c_void_p
    library.PQfinish.argtypes = [ctypes.c_void_p]
    library.PQstatus.argtypes = [ctypes.c_void_p]
    library.PQstatus.restype = ctypes.c_int
    library.PQerrorMessage.argtypes = [ctypes.c_void_p]
    library.PQerrorMessage.restype = ctypes.c_char_p
    for function in ["PQuser", "PQdb", "PQpass"]:
        getattr(library, function).argtypes = [ctypes.c_void_p]
        getattr(library, function).restype = ctypes.c_char_p
    library.PQlibVersion.restype = ctypes.c_int
    checks = 0
    closed_port = urlsplit(unavailable).port

    def check(condition):
        nonlocal checks
        checks += 1
        if not condition:
            raise RuntimeError("libpq LDAP functional control failed: " + str(checks))

    def run(text, expected_user=None, expected_db=None, expected_password=None):
        file.write_text("[sample]\n" + text)
        # Numeric address and a closed port prevent any PostgreSQL startup or credential writes.
        options = f"service=sample hostaddr=127.0.0.1 port={closed_port} connect_timeout=1"
        connection = library.PQconnectStart(options.encode())
        check(bool(connection))
        try:
            if expected_user is None:
                check(library.PQstatus(connection) == 1)
                check(bool(library.PQerrorMessage(connection)))
                check(library.PQuser(connection) != b"fallback")
            else:
                check(library.PQuser(connection).decode() == expected_user)
                if expected_db is not None:
                    check(library.PQdb(connection).decode() == expected_db)
                if expected_password is not None:
                    check(library.PQpass(connection).decode() == expected_password)
        finally:
            library.PQfinish(connection)

    normal = endpoint + "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)\n"
    run(normal + "user=fallback\n", "directory_user", "directory_database", "directory secret")
    run("user=before\n" + normal, "before", "directory_database")
    for scope in ["one", "sub"]:
        run(endpoint + "/dc=weave,dc=test?description?" + scope + "?(cn=normal)\n", "directory_user")
    run(endpoint + "/cn=empty,dc=weave,dc=test?description?base?(objectClass=*)\n", "empty_user", expected_password="")
    run(endpoint + "/cn=multi,dc=weave,dc=test?description?base?(objectClass=*)\n", "multi_user", "multi_database")
    run(unavailable + "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)\nuser=fallback\n", "fallback")
    failures = ["cn=bad", "cn=absent", "dc=weave"]
    for base in failures:
        scope = "one" if base == "dc=weave" else "base"
        dn = base + (",dc=test" if base == "dc=weave" else ",dc=weave,dc=test")
        run(endpoint + "/" + dn + "?description?" + scope + "?(objectClass=*)\nuser=fallback\n")
    file.unlink()
    print(f"libpq {library.PQlibVersion()} LDAP functional controls passed: {checks} checks", flush=True)


if __name__ == "__main__":
    main()
