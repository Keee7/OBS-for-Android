#!/usr/bin/env python3
"""Produce an isolated Qt 6.12 Android jar with OBS runtime fixes.

Both the input Qt installation and the qtbase source tree are read only.
Run after androiddeployqt has staged its jar and before Gradle builds the APK;
replace only that staged copy.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PATCH_DIR = Path(__file__).resolve().parent
JAVA_REL_DIR = Path('src/android/jar/src/org/qtproject/qt/android')
JAVA_PATCHES = (
    ('QtInputDelegate.java', PATCH_DIR / 'qt-6.12.0-ime.patch'),
    ('QtRootLayout.java', PATCH_DIR / 'qt-6.12.0-root-layout.patch'),
    ('QtWindowInsetsController.java', PATCH_DIR / 'qt-6.12.0-window-insets.patch'),
)
FOCUS = ROOT / 'obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsImeFocus.java'
QT_VERSION = '6.12.0'
ORIGINAL_CLASS_HASHES = {
    'org/qtproject/qt/android/QtInputDelegate.class': 'a8c74ad15a1a2a96a95e3363b09a7196906ca1e4b98335c4d70386de57a49fad',
    'org/qtproject/qt/android/QtRootLayout.class': '80f444387fed3baa0dbe6dce2720f01eab4f3119f839c9f6d18994daed5ab635',
    'org/qtproject/qt/android/QtWindowInsetsController.class': '4080f9b3585f52a0700ca1f862df69ca2b271a9453f0d2efe576f7163815ce95',
}
CLASS_PREFIXES = tuple(name.removesuffix('.class') for name in ORIGINAL_CLASS_HASHES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-version', required=True)
    parser.add_argument('--jar', required=True, type=Path)
    parser.add_argument('--qtbase-src', required=True, type=Path)
    parser.add_argument('--android-jar', required=True, type=Path)
    parser.add_argument('--javac', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.qt_version != QT_VERSION:
        parser.error(f'this patch is limited to Qt {QT_VERSION}')
    if args.jar.resolve() == args.output.resolve():
        parser.error('output must differ from the input jar')
    upstream_java_dir = args.qtbase_src / JAVA_REL_DIR
    for name, patch in JAVA_PATCHES:
        if not (upstream_java_dir / name).is_file():
            parser.error(f'missing upstream Qt Java source: {upstream_java_dir / name}')
        if not patch.is_file():
            parser.error(f'missing Qt Java patch: {patch}')
    with zipfile.ZipFile(args.jar) as jar:
        for name, expected in ORIGINAL_CLASS_HASHES.items():
            if hashlib.sha256(jar.read(name)).hexdigest() != expected:
                parser.error(f'{name} does not match the pinned Qt {QT_VERSION} binary')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='obs-qt-ime-', dir=args.output.parent) as tmp:
            tmp = Path(tmp)
            src_root = tmp / 'qtbase'
            java_dir = src_root / JAVA_REL_DIR
            java_dir.mkdir(parents=True)
            for name, _ in JAVA_PATCHES:
                shutil.copyfile(upstream_java_dir / name, java_dir / name)
            subprocess.run(['git', 'init', '-q'], cwd=src_root, check=True)
            subprocess.run(
                ['git', '-c', 'core.autocrlf=false', '-c', 'core.longpaths=true', 'apply',
                 *(str(patch) for _, patch in JAVA_PATCHES)],
                cwd=src_root, check=True,
            )
            classes = tmp / 'classes'
            classes.mkdir()
            subprocess.run([str(args.javac), '-encoding', 'UTF-8', '-source', '17', '-target', '17',
                            '-classpath', os.pathsep.join((str(args.jar.resolve()), str(args.android_jar.resolve()))),
                            '-d', str(classes), *(str(java_dir / name) for name, _ in JAVA_PATCHES), str(FOCUS)], check=True)
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
