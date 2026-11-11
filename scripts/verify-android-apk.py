#!/usr/bin/env python3
"""Inspect an Android test APK's manifest, native binaries and generated Java classes."""
import argparse
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from zipfile import BadZipFile, ZipFile

REQUIRED = {
    'libobs_arm64-v8a.so': set(),
    'liblibobs.so': {'obs_android_set_browser_host', 'obs_android_browser_create',
                     'obs_android_find_hardware_decoder', 'obs_android_set_decoder_query',
                     'obs_source_draw_async_frame', 'obs_android_set_screen_gpu_host',
                     'obs_android_screen_gpu_create', 'obs_android_screen_gpu_update', 'obs_android_screen_gpu_destroy',
                     'obs_android_storage_open', 'obs_android_set_storage_open',
                     'obs_android_screen_capture_error',
                     'obs_android_browser_touch', 'gs_android_encoder_surface_create',
                     'gs_android_encoder_surface_submit', 'gs_android_external_texture_create'},
    'libobs-opengl.so': {'device_android_encoder_surface_create', 'device_android_encoder_surface_submit',
                         'device_android_external_texture_create', 'device_android_external_texture_render'},
    'android-mediacodec.so': {'obs_module_load'},
    'android-browser.so': {'obs_module_load'},
    'android-screen.so': {'obs_module_load'},
    'android-capture.so': {'obs_module_load'},
    'obs-ffmpeg.so': {'obs_module_load'},
}
IMPORTED = {
    'android-capture.so': {'obs_source_output_video', 'obs_source_set_async_unbuffered'},
    'obs-ffmpeg.so': {'obs_android_find_hardware_decoder', 'obs_android_storage_open'},
    'libobs_arm64-v8a.so': {'obs_android_set_storage_open'},
    'android-screen.so': {'obs_android_screen_capture_error', 'obs_source_draw_async_frame',
                          'obs_android_screen_gpu_create', 'obs_android_screen_gpu_update', 'obs_android_screen_gpu_destroy'},
    'android-mediacodec.so': {'gs_android_encoder_surface_create', 'gs_android_encoder_surface_submit'},
    'android-browser.so': {'obs_android_browser_create', 'obs_android_browser_touch',
                           'gs_android_external_texture_create'},
}
PERMISSIONS = {'android.permission.INTERNET', 'android.permission.RECORD_AUDIO', 'android.permission.CAMERA',
               'android.permission.FOREGROUND_SERVICE', 'android.permission.FOREGROUND_SERVICE_MEDIA_PROJECTION'}
CLASSES = (b'Lcom/obsproject/studio/ObsCodecCapabilities;', b'Lcom/obsproject/studio/ObsBrowserHost;',
           b'Lcom/obsproject/studio/ObsStorageHost;', b'Lcom/obsproject/studio/ObsProjectionHost;',
           b'Lcom/obsproject/studio/ObsProjectionSession;', b'Lcom/obsproject/studio/ObsScreenGpuHost;')


def run_tool(argv):
    completed = subprocess.run([str(x) for x in argv], capture_output=True, text=True, check=True)
    return completed.stdout


def dynamic_symbols(text):
    defined, undefined = set(), set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(':') or not fields[0][:-1].isdigit():
            continue
        name = fields[7].split('@', 1)[0]
        (undefined if fields[6] == 'UND' else defined).add(name)
    return defined, undefined


def check_elf_header(data):
    # EI_CLASS=2 (64-bit), EI_DATA=1 (little endian), e_machine=183 (AArch64).
    return len(data) >= 20 and data[:4] == b'\x7fELF' and data[4] == 2 and data[5] == 1 and struct.unpack_from('<H', data, 18)[0] == 183


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--tools-root', type=Path, default=Path(__file__).resolve().parents[2] / 'build-tools')
    parser.add_argument('--native-dir', type=Path, help='optional build tree: compare packaged libraries to built files')
    parser.add_argument('--expected-package', help='expected application ID; defaults to the source manifest package')
    args = parser.parse_args()
    sdk = args.tools_root / 'android-sdk' / 'build-tools' / '36.0.0'
    aapt = sdk / 'aapt.exe'
    readelf = args.tools_root / 'ndk' / 'android-ndk-r30' / 'toolchains' / 'llvm' / 'prebuilt' / 'windows-x86_64' / 'bin' / 'llvm-readelf.exe'
    errors = []
    try:
        print('APK SHA-256:', hashlib.sha256(args.apk.read_bytes()).hexdigest())
        badging = run_tool([aapt, 'dump', 'badging', args.apk])
        package = re.search(r"package: name='([^']+)' versionCode='([^']+)' versionName='([^']*)'", badging)
        manifest = ET.parse(Path(__file__).resolve().parents[1] /
                            'obs-studio/frontend/cmake/android/AndroidManifest.xml').getroot()
        android = '{http://schemas.android.com/apk/res/android}'
        expected_package = (args.expected_package or manifest.get('package'), manifest.get(android + 'versionCode'),
                            manifest.get(android + 'versionName'))
        if not package or package.groups() != expected_package:
            errors.append(f'package name/version differs from source manifest: {expected_package}')
        for label, expected in [('sdkVersion', '29'), ('targetSdkVersion', '35')]:
            if not re.search(r"^" + label + r":'" + expected + r"'$", badging, re.MULTILINE):
                errors.append(f'{label} is not {expected}')
        if "application-icon-" not in badging:
            errors.append('missing application launcher icon')
        for permission in sorted(PERMISSIONS):
            if f"uses-permission: name='{permission}'" not in badging:
                errors.append('missing permission ' + permission)
        with ZipFile(args.apk) as apk, tempfile.TemporaryDirectory(prefix='obs-apk-check-') as tmp:
            names = apk.namelist()
            if len(names) != len(set(names)):
                errors.append('APK contains duplicate ZIP entry names')
            dex = [apk.read(n) for n in names if re.fullmatch(r'classes\d*\.dex', n)]
            for name in CLASSES:
                if not any(name in item for item in dex):
                    errors.append('missing Java class ' + name.decode())
            if not any(b'onCapturedContentResize' in item for item in dex):
                errors.append('missing captured-content resize callback')
            libs = [n for n in names if (n.startswith('lib/arm64-v8a/') or n.startswith('assets/obs-plugins/')) and n.endswith('.so')]
            if not libs:
                errors.append('no arm64-v8a native libraries')
            for path in libs:
                payload = apk.read(path)
                short = path.rsplit('/', 1)[-1]
                if short == 'obs-ffmpeg.so' and b'Select a specific Android MediaCodec component' not in payload:
                    errors.append('FFmpeg named MediaCodec decoder patch missing from APK')
                if short == 'android-screen.so' and b'alpha_mode' not in payload:
                    errors.append('screen capture Alpha modes missing from APK')
                if short == 'android-mediacodec.so' and (b'dropped_load=' not in payload or b'event=bitrate-update-rejected' not in payload):
                    errors.append('MediaCodec stability/bitrate recovery code missing from APK')
                if short == 'android-capture.so' and b'yuv_low_latency' not in payload:
                    errors.append('UVC YUV low-latency mode missing from APK')
                if short == 'android-capture.so' and b'event=acquire_stopped' not in payload:
                    errors.append('UVC transport recovery diagnostics missing from APK')
                if short == 'android-capture.so' and b'yuv_unbuffered' not in payload:
                    errors.append('UVC independent cadence/buffering control missing from APK')
                if short == 'android-capture.so' and any(marker not in payload for marker in
                        (b'event=cadence_input', b'event=cadence_pts', b'event=probe_clock')):
                    errors.append('UVC input cadence diagnostics missing from APK')
                if short == 'liblibobs.so' and b'event=cadence_select' not in payload:
                    errors.append('OBS UVC frame-selection diagnostics missing from APK')
                digest = hashlib.sha256(payload).hexdigest()
                print(f'{path} SHA-256 {digest}')
                if not check_elf_header(payload):
                    errors.append(path + ' is not little-endian AArch64 ELF64')
                if args.native_dir:
                    # Qt/runtime binaries come from the SDK. Project binaries
                    # must match a fresh compiler output, never a packaging cache.
                    external = short.startswith(('libQt', 'libplugins_')) or short == 'libc++_shared.so'
                    if not external:
                        candidates = [p for p in args.native_dir.rglob(short) if p.is_file()
                                      and not any(part in ('android-build', 'android-package-source', 'rundir')
                                                  for part in p.relative_to(args.native_dir).parts)]
                        if not candidates:
                            errors.append(path + ' has no authoritative compiler output')
                        else:
                            strip = readelf.with_name('llvm-strip.exe')
                            matched = False
                            for index, candidate in enumerate(candidates):
                                if hashlib.sha256(candidate.read_bytes()).hexdigest() == digest:
                                    matched = True
                                    break
                                normalized = Path(tmp) / f'current-{index}-{short}'
                                run_tool([strip, '--strip-unneeded', '-o', normalized, candidate])
                                if hashlib.sha256(normalized.read_bytes()).hexdigest() == digest:
                                    matched = True
                                    break
                            if not matched:
                                errors.append(path + ' differs from authoritative compiler output (including fresh strip)')
                if short not in REQUIRED:
                    continue
                temp = Path(tmp) / short
                temp.write_bytes(payload)
                symbols, undefined = dynamic_symbols(run_tool([readelf, '--dyn-syms', '--wide', temp]))
                missing = REQUIRED[short] - symbols
                if missing:
                    errors.append(f'{short} missing exported symbols: {", ".join(sorted(missing))}')
                missing_imports = IMPORTED.get(short, set()) - undefined
                if missing_imports:
                    errors.append(f'{short} missing expected dynamic imports: {", ".join(sorted(missing_imports))}')
            for name in REQUIRED:
                if f'lib/arm64-v8a/{name}' not in names and f'assets/obs-plugins/{name}' not in names:
                    errors.append('missing arm64 library ' + name)
    except (OSError, ET.ParseError, BadZipFile, subprocess.CalledProcessError) as exc:
        print('FAIL: cannot inspect APK:', exc, file=sys.stderr)
        return 2
    for error in errors:
        print('FAIL:', error)
    if not errors:
        print('PASS: APK structure, metadata, native architecture and symbols checked.')
    print('This is not a runtime, hardware-encoding, streaming or security test.')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
