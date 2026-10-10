"""Independent SCRAM/MD5 derivation for the public synchronous password API."""
import argparse
import base64
import hashlib
import hmac
import json
import subprocess


def scram(verifier, password, iterations):
    algorithm, parameters, keys = verifier.split("$")
    count, encoded = parameters.split(":")
    salt = base64.b64decode(encoded, validate=True)
    stored, server = (base64.b64decode(value, validate=True) for value in keys.split(":"))
    if algorithm != "SCRAM-SHA-256" or int(count) != iterations or len(salt) != 16:
        raise RuntimeError("Unexpected SCRAM parameters")
    salted = hashlib.pbkdf2_hmac("sha256", password, salt, iterations)
    client_key = hmac.digest(salted, b"Client Key", "sha256")
    if not hmac.compare_digest(stored, hashlib.sha256(client_key).digest()) or not hmac.compare_digest(
            server, hmac.digest(salted, b"Server Key", "sha256")):
        raise RuntimeError("Independent SCRAM derivation disagrees")


def verify(output, baseline=False):
    original = [b"pencil", b"", b"\xc2\xaa", b"I\xc2\xadX", b"\x07", b"\xff", b"\xd8\xa7x",
                b"\xc8\xa1", b"O'Reilly\\secret\n"]
    # RFC4013 mappings; PostgreSQL falls back to raw bytes for these remaining inputs.
    prepared = [b"pencil", b"", b"a", b"IX", *original[4:]]
    entries = [json.loads(line) for line in output.splitlines()]
    if len(entries) != len(original) + (0 if baseline else 1):
        raise RuntimeError("CPU controls missing")
    for index, (raw, normalized) in enumerate(zip(original, prepared)):
        value = entries[index]
        if value["case"] != index or value["md5"] != "md5" + hashlib.md5(raw + b"user").hexdigest():
            raise RuntimeError("MD5 verifier differs")
        scram(value["scram"], normalized, 4096)
    if not baseline:
        if entries[-1]["checks"] != 18:
            raise RuntimeError("Native validation controls missing")
        scram(entries[-1]["custom"], b"pencil", 8192)
    return {"independently_verified_scram": len(original) + (0 if baseline else 1),
            "independently_verified_md5": len(original), "native_checks": None if baseline else 18}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    args = parser.parse_args()
    result = subprocess.run([args.executable], capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise RuntimeError(f"Native password controls failed ({result.returncode}): {result.stdout}\n{result.stderr}")
    print(json.dumps(verify(result.stdout)), flush=True)


if __name__ == "__main__":
    main()
