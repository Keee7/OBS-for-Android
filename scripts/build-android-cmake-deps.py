#!/usr/bin/env python3
"""Build OBS Android CMake dependencies with a Windows Android NDK.

Sources must already exist under --tools. This script does not download code.
Examples:
  python scripts/build-android-cmake-deps.py --ndk C:/android/ndk/29.0.14206865 --only jansson
  python scripts/build-android-cmake-deps.py --ndk C:/android/ndk/29.0.14206865 --only mbedtls,curl
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, default=ROOT / 'build-tools')
    parser.add_argument('--ndk', required=True, type=Path)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--api', type=int, default=29)
    parser.add_argument('--jobs', type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument('--only', default='jansson,libjpeg-turbo,freetype,mbedtls,curl',
                        help='comma-separated build order; curl requires installed mbedtls')
    args = parser.parse_args()
    tools = args.tools.absolute()
    prefix = tools / 'android-deps' / 'arm64-v8a'
    toolchain = args.ndk.absolute() / 'build/cmake/android.toolchain.cmake'
    if not toolchain.is_file():
        raise RuntimeError(f'Android NDK CMake toolchain missing: {toolchain}')
    builds = tools / 'android-cmake-builds'
    allowed = {'jansson', 'libjpeg-turbo', 'freetype', 'mbedtls', 'curl'}
    names = [n.strip() for n in args.only.split(',') if n.strip()]
    if not names or set(names) - allowed:
        raise RuntimeError(f'--only must contain names from {sorted(allowed)}')
    versions = {'jansson': 'android-cmake-sources/jansson-2.14', 'libjpeg-turbo': 'libjpeg-turbo-3.1.0',
                'freetype': 'freetype-VER-2-13-3', 'mbedtls': 'mbedtls-3.6.4', 'curl': 'curl-8.15.0'}
    env = os.environ.copy()
    env['PKG_CONFIG_LIBDIR'] = str(prefix / 'lib/pkgconfig')
    env['PKG_CONFIG_PATH'] = ''
    env['PKG_CONFIG_SYSROOT_DIR'] = ''
    env.pop('CFLAGS', None); env.pop('CXXFLAGS', None); env.pop('LDFLAGS', None)
    prefix.mkdir(parents=True, exist_ok=True)
    # Header-only dependencies are copied verbatim, with no generated stubs.
    include = prefix / 'include'
    include.mkdir(exist_ok=True)
    simde = tools / 'simde-0.8.2/simde'
    if simde.is_dir():
        shutil.copytree(simde, include / 'simde', dirs_exist_ok=True)
    uthash = tools / 'uthash-2.3.0/src'
    if uthash.is_dir():
        for header in uthash.glob('*.h'):
            shutil.copy2(header, include / header.name)
    for name in names:
        source = tools / versions[name]
        if not (source / 'CMakeLists.txt').is_file():
            raise RuntimeError(f'{name} source missing: {source}; fetch version listed in deps-android/versions.sh')
        if name == 'mbedtls' and not (source / 'library/CMakeLists.txt').is_file():
            raise RuntimeError('mbedtls checkout is incomplete: library/ is missing')
        if name == 'curl' and not (prefix / 'lib/libmbedtls.a').is_file():
            raise RuntimeError('build mbedtls first; libmbedtls.a is missing in Android prefix')
        flags = {
            'jansson': ['-DJANSSON_BUILD_DOCS=OFF', '-DJANSSON_WITHOUT_TESTS=ON', '-DJANSSON_EXAMPLES=OFF'],
            'libjpeg-turbo': ['-DCMAKE_INSTALL_LIBDIR=lib', '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON',
                              '-DWITH_JAVA=OFF', '-DWITH_ICC_LIB=OFF', '-DWITH_TURBOJPEG=ON'],
            'freetype': ['-DFT_DISABLE_ZLIB=ON', '-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_PNG=ON',
                         '-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BROTLI=ON'],
            'mbedtls': ['-DENABLE_TESTING=OFF', '-DENABLE_PROGRAMS=OFF', '-DGEN_FILES=OFF',
                        '-DMBEDTLS_FATAL_WARNINGS=OFF'],
            'curl': ['-DBUILD_CURL_EXE=OFF', '-DBUILD_LIBCURL_DOCS=OFF', '-DBUILD_MISC_DOCS=OFF',
                     '-DENABLE_CURL_MANUAL=OFF', '-DCURL_USE_LIBSSH2=OFF', '-DCURL_USE_LIBSSH=OFF',
                     '-DCURL_USE_LIBPSL=OFF', '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF',
                     '-DCURL_USE_OPENSSL=OFF', '-DCURL_USE_MBEDTLS=ON', '-DCMAKE_USE_MBEDTLS=ON',
                     f'-DMBEDTLS_INCLUDE_DIR={prefix / "include"}',
                     f'-DMBEDTLS_LIBRARY={prefix / "lib/libmbedtls.a"}',
                     f'-DMBEDX509_LIBRARY={prefix / "lib/libmbedx509.a"}',
                     f'-DMBEDCRYPTO_LIBRARY={prefix / "lib/libmbedcrypto.a"}'],
        }[name]
        build = builds / ('jansson-clean' if name == 'jansson' else name)
        cmd = [args.cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
               f'-DCMAKE_MAKE_PROGRAM={args.ninja}', f'-DCMAKE_TOOLCHAIN_FILE={toolchain}',
               '-DANDROID_ABI=arm64-v8a', f'-DANDROID_PLATFORM=android-{args.api}',
               '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release',
               f'-DCMAKE_INSTALL_PREFIX={prefix}', f'-DCMAKE_PREFIX_PATH={prefix}',
               '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
               '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH',
               '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH', '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH',
               '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF'] + flags
        for command in (cmd, [args.cmake, '--build', str(build), '--parallel', str(args.jobs)],
                        [args.cmake, '--install', str(build)]):
            print('Running:', subprocess.list2cmdline(command), flush=True)
            subprocess.run(command, env=env, check=True)
    print('Android dependency prefix:', prefix)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)
