#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cross-platform Android APK packaging helper shared by CI, WSL, and Windows builds."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


REQUIRED_PLUGINS = ("android-mediacodec.so", "android-browser.so")


def _host_prebuilt_tag() -> str:
    return "windows-x86_64" if os.name == "nt" else "linux-x86_64"


def _exe_suffix() -> str:
    return ".exe" if os.name == "nt" else ""


def _run(cmd: list[str], cwd: Path | None = None) -> None:
    print(f"+ {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True)


def _find_obs_plugins_dir(rundir: Path) -> Path:
    if not rundir.is_dir():
        raise FileNotFoundError(f"OBS rundir does not exist: {rundir}")
    for candidate in sorted(rundir.rglob("obs-plugins")):
        if candidate.is_dir() and any(candidate.glob("*.so")):
            return candidate
    raise FileNotFoundError(f"No non-empty obs-plugins directory found under: {rundir}")


def stage_plugins(build_dir: Path, package_source: Path, ndk: Path, scripts_dir: Path) -> Path:
    plug_src = _find_obs_plugins_dir(build_dir / "rundir")
    modules = sorted(p for p in plug_src.glob("*.so") if p.is_file())
    if not modules:
        raise RuntimeError(f"Plugin directory is empty: {plug_src}")

    for mod in modules:
        if mod.name.startswith(("libobs_", "libQt")):
            raise RuntimeError(f"Non-plugin library leaked into obs-plugins: {mod.name}")

    module_names = {m.name for m in modules}
    missing = [req for req in REQUIRED_PLUGINS if req not in module_names]
    if missing:
        raise RuntimeError(f"Required built plugin(s) missing: {', '.join(missing)}")

    # Bundle plugins under assets/obs-plugins so AndroidRuntimeBootstrap extracts
    # them into $OBS_ROOT_PATH/lib at runtime (APK lib/<abi> only unpacks lib*.so).
    pkg_assets = package_source / "assets" / "obs-plugins"
    if pkg_assets.exists():
        shutil.rmtree(pkg_assets)
    pkg_assets.mkdir(parents=True, exist_ok=True)

    for mod in modules:
        shutil.copy2(mod, pkg_assets / mod.name)

    strip_bin = (
        ndk
        / "toolchains"
        / "llvm"
        / "prebuilt"
        / _host_prebuilt_tag()
        / "bin"
        / f"llvm-strip{_exe_suffix()}"
    )
    if strip_bin.is_file():
        for staged in sorted(pkg_assets.glob("*.so")):
            subprocess.run(
                [str(strip_bin), "--strip-unneeded", str(staged)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )

    _run([sys.executable, str(scripts_dir / "android-plugin-bundle.py"), "generate", str(pkg_assets)])
    print(f"Staged {len(modules)} plugins into {pkg_assets}", flush=True)
    return pkg_assets


def clean_android_intermediates(frontend_dir: Path, android_build: Path, abi: str) -> None:
    libs_abi = android_build / "libs" / abi
    if libs_abi.is_dir():
        for so_file in libs_abi.glob("*.so"):
            if so_file.is_file() and not so_file.name.startswith("lib"):
                so_file.unlink()
    libs_abi.mkdir(parents=True, exist_ok=True)

    app_lib = frontend_dir / f"libobs_{abi}.so"
    if not app_lib.is_file():
        app_libs = sorted(frontend_dir.glob("libobs_*.so"))
        app_lib = app_libs[0] if app_libs else app_lib
    if app_lib.is_file():
        shutil.copy2(app_lib, libs_abi / app_lib.name)

    for rel in (
        Path("obs-studio.apk"),
        Path("build/outputs/apk"),
        Path("build/intermediates/merged_jni_libs"),
        Path("build/intermediates/merged_native_libs"),
        Path("build/intermediates/stripped_native_libs"),
    ):
        target = android_build / rel
        if target.is_dir():
            shutil.rmtree(target)
        elif target.is_file():
            target.unlink()


def locate_unsigned_apk(android_build: Path, fallback_apk: Path) -> Path:
    candidates: list[Path] = []
    release_dir = android_build / "build" / "outputs" / "apk" / "release"
    if release_dir.is_dir():
        candidates = sorted(
            (p for p in release_dir.rglob("*-unsigned.apk") if p.is_file()),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
    if not candidates and android_build.is_dir():
        candidates = sorted(
            (p for p in android_build.rglob("*-unsigned.apk") if p.is_file()),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
    if candidates:
        return candidates[0]
    if fallback_apk.is_file():
        return fallback_apk
    raise FileNotFoundError(f"Unsigned APK not found under {android_build}")


def package_apk(args: argparse.Namespace) -> Path:
    scripts_dir = Path(__file__).resolve().parent
    build_dir = args.build_dir.resolve()
    frontend_dir = build_dir / "frontend"
    android_build = frontend_dir / "android-build"
    deploy_json = frontend_dir / "android-obs-studio-deployment-settings.json"
    if not deploy_json.is_file():
        raise FileNotFoundError(f"Deployment settings JSON missing: {deploy_json}")

    stage_plugins(build_dir, args.package_source.resolve(), args.ndk.resolve(), scripts_dir)

    if args.clean_intermediates:
        clean_android_intermediates(frontend_dir, android_build, args.abi)

    deploy_bin = args.qt_host_dir.resolve() / "bin" / f"androiddeployqt{_exe_suffix()}"
    apk_placeholder = android_build / "obs-studio.apk"
    _run(
        [
            str(deploy_bin),
            "--input",
            str(deploy_json),
            "--output",
            str(android_build),
            "--apk",
            str(apk_placeholder),
            "--depfile",
            str(android_build / "obs-studio.d"),
            "--builddir",
            str(frontend_dir),
            "--android-platform",
            args.android_platform,
            "--release",
            "--aux-mode",
        ],
        cwd=frontend_dir,
    )

    _run(
        [
            sys.executable,
            str(scripts_dir / "stage-qt-gradle-properties.py"),
            "--deployment-settings",
            str(deploy_json),
            "--output",
            str(android_build),
            "--android-platform",
            args.android_platform,
            "--sdk",
            str(args.sdk.resolve()),
            "--build-tools",
            args.build_tools,
        ]
    )

    patched_qt_jar = build_dir / "qt-ime-patched" / "Qt6Android.jar"
    _run(
        [
            sys.executable,
            str(scripts_dir / "qt-android-ime" / "patch-qt-android-jar.py"),
            "--qt-version",
            args.qt_version,
            "--jar",
            str(args.qt_android_dir.resolve() / "jar" / "Qt6Android.jar"),
            "--qtbase-src",
            str(build_dir / "_deps" / "obs_qtbase_android-src"),
            "--android-jar",
            str(args.sdk.resolve() / "platforms" / args.android_platform / "android.jar"),
            "--javac",
            str(args.java_home.resolve() / "bin" / f"javac{_exe_suffix()}"),
            "--output",
            str(patched_qt_jar),
        ]
    )

    shutil.copy2(patched_qt_jar, android_build / "libs" / "Qt6Android.jar")
    qpa_name = f"libplugins_platforms_qtforandroid_{args.abi}.so"
    qpa_src = build_dir / "qt-android-platform" / qpa_name
    qpa_dst_dir = android_build / "libs" / args.abi
    qpa_dst_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(qpa_src, qpa_dst_dir / qpa_name)

    gradle_cmd = [str(args.gradle.resolve()), "--no-daemon"]
    if args.gradle_parallel:
        gradle_cmd.append("--parallel")
    gradle_cmd.extend(["-p", str(android_build), "assembleRelease"])
    if args.skip_lint:
        gradle_cmd.extend(["-x", "lintVitalRelease"])
    _run(gradle_cmd)

    unsigned_apk = locate_unsigned_apk(android_build, apk_placeholder)
    if args.verify_unsigned:
        _run([sys.executable, str(scripts_dir / "qt-android-ime" / "verify-runtime-apk.py"), str(unsigned_apk)])
        _run([sys.executable, str(scripts_dir / "android-plugin-bundle.py"), "verify", str(unsigned_apk)])
        verify_cmd = [
            sys.executable,
            str(scripts_dir / "verify-android-apk.py"),
            str(unsigned_apk),
            "--aapt",
            str(args.sdk.resolve() / "build-tools" / args.build_tools / f"aapt{_exe_suffix()}"),
            "--readelf",
            str(
                args.ndk.resolve()
                / "toolchains"
                / "llvm"
                / "prebuilt"
                / _host_prebuilt_tag()
                / "bin"
                / f"llvm-readelf{_exe_suffix()}"
            ),
        ]
        if args.expected_package:
            verify_cmd.extend(["--expected-package", args.expected_package])
        _run(verify_cmd)

    if args.unsigned_apk_out:
        out_path = args.unsigned_apk_out.resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(str(unsigned_apk) + "\n", encoding="utf-8", newline="\n")

    print(f"Unsigned APK: {unsigned_apk}", flush=True)
    return unsigned_apk


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--package-source", type=Path, required=True)
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--qt-android-dir", type=Path, required=True)
    parser.add_argument("--qt-host-dir", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--gradle", type=Path, required=True)
    parser.add_argument("--android-platform", required=True)
    parser.add_argument("--build-tools", required=True)
    parser.add_argument("--unsigned-apk-out", type=Path)
    parser.add_argument("--expected-package")
    parser.add_argument("--clean-intermediates", action="store_true")
    parser.add_argument("--gradle-parallel", action="store_true")
    parser.add_argument("--skip-lint", action="store_true")
    parser.add_argument("--verify-unsigned", action="store_true")
    return parser.parse_args()


def main() -> None:
    try:
        package_apk(parse_args())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"FATAL: {exc}", file=sys.stderr, flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
