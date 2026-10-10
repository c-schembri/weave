"""Exercise the exact loader with a compiled-in, exclusively owned fixture path."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--directory", required=True)
    args = parser.parse_args()
    directory = Path(args.directory).resolve()
    if not directory.name.startswith("service-fixture-"):
        raise RuntimeError("Unexpected owned fixture path")
    # Exclusive creation prevents another configuration/run from sharing or deleting this fixture.
    directory.mkdir()
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("PG")}
    try:
        subprocess.run([args.executable, str(directory)], env=environment, check=True, timeout=20)
    finally:
        for path in sorted(directory.rglob("*"), key=lambda item: len(item.parts), reverse=True):
            if path.is_symlink() or path.is_file():
                path.unlink()
            else:
                path.rmdir()
        directory.rmdir()


if __name__ == "__main__":
    main()
