#!/usr/bin/env python3
"""Sign an aligned APK using a private, external release-signing configuration.

Accept JSON, KEY=value, or labelled lines for ANDROID_KEYSTORE_BASE64,
ANDROID_KEY_ALIAS, ANDROID_KEYSTORE_PASSWORD and ANDROID_KEY_PASSWORD.
The base64 value may instead name a file containing the base64-encoded store.
Passwords are passed to apksigner through its environment interface.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

KEYS = ('ANDROID_KEYSTORE_BASE64', 'ANDROID_KEY_ALIAS',
        'ANDROID_KEYSTORE_PASSWORD', 'ANDROID_KEY_PASSWORD')


def read_config(path):
    text = Path(path).read_text(encoding='utf-8-sig')
    if text.lstrip().startswith('{'):
        values = json.loads(text)
    else:
        values = {}
        for key in KEYS:
            line = next((line for line in text.splitlines() if key in line), '')
            tail = line.split(key, 1)[-1].strip(' \t`|=:：')
            if key == KEYS[0]:
                candidates = []
                for extension in ('txt', 'b64', 'base64', 'jks', 'keystore'):
                    candidates.extend(re.findall(r'[A-Za-z]:[\\/][^`\r\n]*?\.' + extension, tail))
                existing = next((p for p in candidates if Path(p).is_file()), None)
                values[key] = existing or tail.strip('"\'')
            else:
                tokens = re.split(r'[\s`|]+', tail)
                values[key] = tokens[0].strip('"\'') if tokens else ''
    if any(not values.get(key) for key in KEYS):
        raise ValueError('Signing configuration is missing a required field')
    encoded = values[KEYS[0]]
    # Long inline base64 must never be treated as a filesystem name.
    if len(encoded) < 1024 and Path(encoded).is_file():
        encoded = Path(encoded).read_text(encoding='utf-8-sig')
    store = base64.b64decode(''.join(encoded.split()), validate=True)
    return store, values[KEYS[1]], values[KEYS[2]], values[KEYS[3]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--tools-root', type=Path, default=Path(__file__).resolve().parents[2] / 'build-tools')
    args = parser.parse_args()
    try:
        store, alias, store_pass, key_pass = read_config(args.config)
    except Exception:
        parser.exit(2, 'Cannot load release signing configuration; no credentials were printed.\n')
    env = os.environ.copy()
    env['OBS_APK_STORE_PASS'] = store_pass
    env['OBS_APK_KEY_PASS'] = key_pass
    java = args.tools_root / 'jdk-17.0.12/bin/java.exe'
    jar = args.tools_root / 'android-sdk/build-tools/36.0.0/lib/apksigner.jar'
    with tempfile.TemporaryDirectory(prefix='obs-release-sign-') as tmp:
        key = Path(tmp) / 'release.jks'
        key.write_bytes(store)
        command = [str(java), '-jar', str(jar), 'sign', '--ks', str(key),
                   '--ks-key-alias', alias, '--ks-pass', 'env:OBS_APK_STORE_PASS',
                   '--key-pass', 'env:OBS_APK_KEY_PASS', '--out', str(args.output), str(args.input)]
        result = subprocess.run(command, env=env, capture_output=True, text=True, errors='replace')
        if result.returncode:
            message = result.stderr
            for secret in (store_pass, key_pass, alias):
                message = message.replace(secret, '<redacted>')
            parser.exit(1, 'Release signing failed: ' + message[:1000] + '\n')
    print('Release signing completed; credentials stayed outside the repository and command line.')


if __name__ == '__main__':
    main()
