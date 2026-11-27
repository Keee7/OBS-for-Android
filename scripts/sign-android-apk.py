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


def load_toolchain_versions():
    env_file = Path(__file__).with_name('toolchain-versions.env')
    versions = {}
    if env_file.is_file():
        for raw in env_file.read_text(encoding='utf-8').splitlines():
            line = raw.strip()
            if line and not line.startswith('#') and '=' in line:
                k, v = line.split('=', 1)
                versions[k.strip()] = v.strip()
    return versions


def java_major_version(java_home):
    release_file = Path(java_home) / 'release'
    if not release_file.is_file():
        return None
    for line in release_file.read_text(encoding='utf-8', errors='replace').splitlines():
        m = re.match(r'^JAVA_VERSION="(\d+)', line.strip())
        if m:
            return m.group(1)
    return None


def find_java_exe(tools_root, jdk_version):
    java_home = os.environ.get('JAVA_HOME')
    if java_home:
        candidate = Path(java_home) / 'bin/java.exe'
        if candidate.is_file():
            major = java_major_version(java_home)
            if major is None or major in (str(jdk_version), '17'):
                return candidate
    for pattern in (f'jdk-{jdk_version}*/bin/java.exe', 'jdk-17*/bin/java.exe'):
        matches = sorted(tools_root.glob(pattern), reverse=True)
        if matches:
            return matches[0]
    return tools_root / f'jdk-{jdk_version}/bin/java.exe'


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
    versions = load_toolchain_versions()
    java = find_java_exe(args.tools_root, versions.get('JDK_VERSION', '21'))
    build_tools = versions.get('BUILD_TOOLS_VERSION', '36.0.0')
    jar = None
    for env_var in ('ANDROID_SDK_ROOT', 'ANDROID_HOME'):
        sdk_env = os.environ.get(env_var)
        if sdk_env:
            candidate = Path(sdk_env) / f'build-tools/{build_tools}/lib/apksigner.jar'
            if candidate.is_file():
                jar = candidate
                break
    if jar is None:
        jar = args.tools_root / f'android-sdk/build-tools/{build_tools}/lib/apksigner.jar'
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
