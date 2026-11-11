#!/usr/bin/env python3
"""Generate and verify the Android built-in plugin manifest."""
import argparse
import hashlib
from pathlib import Path
import re
from zipfile import ZipFile

ASSETS = 'assets/obs-plugins/'
NAME = re.compile(r'[a-zA-Z0-9_][a-zA-Z0-9_.-]*[.]so')
REQUIRED = {'android-browser.so', 'android-mediacodec.so', 'android-capture.so',
            'obs-ffmpeg.so', 'obs-outputs.so', 'obs-x264.so'}


def describe(name, stream):
    if not NAME.fullmatch(name):
        raise ValueError(f'Invalid plugin filename: {name!r}')
    header = stream.read(20)
    if (len(header) != 20 or header[:6] != b'\x7fELF\x02\x01'
            or header[16:20] != b'\x03\x00\xb7\x00'):
        raise ValueError(f'Expected an AArch64 shared object: {name}')
    digest = hashlib.sha256(header)
    size = len(header)
    for block in iter(lambda: stream.read(128 * 1024), b''):
        digest.update(block)
        size += len(block)
    return f'{name} {digest.hexdigest()} {size}\n'


def check_required(names):
    missing = REQUIRED.difference(names)
    if missing:
        raise ValueError('Missing bundled plugins: ' + ', '.join(sorted(missing)))


def generate(directory):
    plugins = sorted(directory.glob('*.so'))
    check_required(p.name for p in plugins)
    entries = []
    for plugin in plugins:
        with plugin.open('rb') as stream:
            entries.append(describe(plugin.name, stream))
    (directory / 'MANIFEST').write_bytes(''.join(entries).encode('ascii'))


def verify(apk):
    with ZipFile(apk) as package:
        assets = [n for n in package.namelist() if n.startswith(ASSETS)]
        if len(assets) != len(set(assets)):
            raise ValueError('Duplicate bundled plugin entries in APK')
        plugins = sorted(n for n in assets if n.endswith('.so'))
        check_required(n.removeprefix(ASSETS) for n in plugins)
        entries = []
        for plugin in plugins:
            with package.open(plugin) as stream:
                entries.append(describe(plugin.removeprefix(ASSETS), stream))
        if package.read(ASSETS + 'MANIFEST') != ''.join(entries).encode('ascii'):
            raise ValueError('Bundled plugin manifest does not match APK contents')
    print(f'PASS: verified {len(plugins)} bundled Android plugins')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('generate', 'verify'))
    parser.add_argument('path', type=Path)
    args = parser.parse_args()
    try:
        (generate if args.action == 'generate' else verify)(args.path)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()

