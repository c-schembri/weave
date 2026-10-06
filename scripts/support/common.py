"""Shared automation helpers. No shell or third-party Python packages required."""

import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def read_json(path):
    raw = Path(path).read_bytes()
    # Archived Windows evidence can have either a UTF-8 or UTF-16 BOM.
    encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    return json.loads(raw.decode(encoding))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest().upper()


def file_hash(path):
    path = Path(path).resolve(strict=True)
    return {"Algorithm": "SHA256", "Hash": sha256(path), "Path": str(path)}


def run(arguments, *, cwd=ROOT, capture=False, timeout=None):
    return subprocess.run(
        [str(argument) for argument in arguments], cwd=cwd, check=True, timeout=timeout,
        text=True, stdout=subprocess.PIPE if capture else None,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )


def capture(arguments, *, cwd=ROOT):
    return run(arguments, cwd=cwd, capture=True, timeout=30).stdout.rstrip("\r\n")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite(value, minimum, name):
    require(type(value) in (int, float) and math.isfinite(value) and value >= minimum, f"Invalid {name}.")


def cli(main):
    try:
        return main() or 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
