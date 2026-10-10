import argparse
import importlib.util
import json
import os
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location('peer', Path(__file__).with_name('notifications.py'))
peer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peer)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--runtime', action='store_true')
    args = parser.parse_args()
    modes = ['context', 'blocking']
    if args.runtime:
        modes += ['affine', 'stealing']
        if os.name == 'nt':
            modes += ['shared_affine', 'shared_stealing']
    for mode in modes:
        result = peer.run(args.executable, mode)
        print(json.dumps(result), flush=True)
        assert result['returncode'] == 0 and not result['stderr'] and not result['peer_errors']
        assert re.search(r'Application-result attachment controls passed: [1-9]\d* checks', result['stdout'])
        expected = {'context': 3, 'blocking': 2}.get(mode, 96)
        assert result['connections'] == expected


if __name__ == '__main__':
    main()
