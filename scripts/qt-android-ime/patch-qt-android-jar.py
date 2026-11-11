#!/usr/bin/env python3
"""Produce an isolated Qt 6.11.1 Android jar with the OBS IME fix.

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
FOCUS = ROOT / 'obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsImeFocus.java'
PREFIX = 'org/qtproject/qt/android/QtInputDelegate'
ORIGINAL_CLASS_HASHES = {
    PREFIX + '.class': '72a86e3de3ac323b3f82e83e570390d83454f6dbc50ff0bc0b9a1422f0e4166a',
    'org/qtproject/qt/android/QtRootLayout.class': '8716ce20ca6321d57de99824e91103539f41bcaa1363e208f53aff4a26e5c3e5',
}
CLASS_PREFIXES = (PREFIX, 'org/qtproject/qt/android/QtRootLayout')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-version', required=True)
    parser.add_argument('--jar', required=True, type=Path)
    parser.add_argument('--android-jar', required=True, type=Path)
    parser.add_argument('--javac', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.qt_version != '6.11.1':
        parser.error('this patch is limited to Qt 6.11.1')
    if args.jar.resolve() == args.output.resolve():
        parser.error('output must differ from the input jar')
    with zipfile.ZipFile(args.jar) as jar:
        for name, expected in ORIGINAL_CLASS_HASHES.items():
            if hashlib.sha256(jar.read(name)).hexdigest() != expected:
                parser.error(name + ' does not match the pinned Qt 6.11.1 binary')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='obs-qt-ime-', dir=args.output.parent) as tmp:
            tmp = Path(tmp)
            classes = tmp / 'classes'
            classes.mkdir()
            subprocess.run([str(args.javac), '-encoding', 'UTF-8', '-source', '17', '-target', '17',
                            '-classpath', os.pathsep.join((str(args.jar.resolve()), str(args.android_jar.resolve()))),
                            '-d', str(classes), str(SOURCE), str(ROOT_LAYOUT), str(FOCUS)], check=True)
            replacements = {p.relative_to(classes).as_posix(): p.read_bytes()
                            for p in classes.rglob('*.class')
                            if p.relative_to(classes).as_posix().startswith(CLASS_PREFIXES)}
            if not set(ORIGINAL_CLASS_HASHES).issubset(replacements):
                raise RuntimeError('javac did not produce both patched Qt classes')
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
