#!/usr/bin/env python3
"""Produce an isolated Qt 6.12 Android jar with OBS runtime fixes.

The input Qt installation is read only. Run after androiddeployqt has staged
its jar and before Gradle builds the APK; replace only that staged copy.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name('QtInputDelegate.java')
ROOT_LAYOUT = Path(__file__).with_name('QtRootLayout.java')
WINDOW_INSETS = Path(__file__).with_name('QtWindowInsetsController.java')
FOCUS = ROOT / 'obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsImeFocus.java'
PREFIX = 'org/qtproject/qt/android/QtInputDelegate'
QT_VERSION = '6.12.0'
ORIGINAL_CLASS_HASHES = {
    PREFIX + '.class': 'a8c74ad15a1a2a96a95e3363b09a7196906ca1e4b98335c4d70386de57a49fad',
    'org/qtproject/qt/android/QtRootLayout.class': '80f444387fed3baa0dbe6dce2720f01eab4f3119f839c9f6d18994daed5ab635',
    'org/qtproject/qt/android/QtWindowInsetsController.class': '4080f9b3585f52a0700ca1f862df69ca2b271a9453f0d2efe576f7163815ce95',
}
CLASS_PREFIXES = tuple(name.removesuffix('.class') for name in ORIGINAL_CLASS_HASHES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-version', required=True)
    parser.add_argument('--jar', required=True, type=Path)
    parser.add_argument('--android-jar', required=True, type=Path)
    parser.add_argument('--javac', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.qt_version != QT_VERSION:
        parser.error(f'this patch is limited to Qt {QT_VERSION}')
    if args.jar.resolve() == args.output.resolve():
        parser.error('output must differ from the input jar')
    with zipfile.ZipFile(args.jar) as jar:
        for name, expected in ORIGINAL_CLASS_HASHES.items():
            if hashlib.sha256(jar.read(name)).hexdigest() != expected:
                parser.error(f'{name} does not match the pinned Qt {QT_VERSION} binary')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='obs-qt-ime-', dir=args.output.parent) as tmp:
            tmp = Path(tmp)
            classes = tmp / 'classes'
            classes.mkdir()
            subprocess.run([str(args.javac), '-encoding', 'UTF-8', '-source', '17', '-target', '17',
                            '-classpath', os.pathsep.join((str(args.jar.resolve()), str(args.android_jar.resolve()))),
                            '-d', str(classes), str(SOURCE), str(ROOT_LAYOUT), str(WINDOW_INSETS), str(FOCUS)], check=True)
            replacements = {p.relative_to(classes).as_posix(): p.read_bytes()
                            for p in classes.rglob('*.class')
                            if p.relative_to(classes).as_posix().startswith(CLASS_PREFIXES)}
            if not set(ORIGINAL_CLASS_HASHES).issubset(replacements):
                raise RuntimeError('javac did not produce all patched Qt classes')
            output = tmp / 'Qt6Android.jar'
            with zipfile.ZipFile(output, 'w') as patched:
                for entry in jar.infolist():
                    if entry.filename.startswith(CLASS_PREFIXES) and entry.filename.endswith('.class'):
                        continue
                    patched.writestr(entry, jar.read(entry.filename))
                for name, data in sorted(replacements.items()):
                    patched.writestr(name, data, compress_type=zipfile.ZIP_DEFLATED)
            os.replace(output, args.output)
    print('Created patched Qt jar:', args.output)


if __name__ == '__main__':
    main()
