#!/usr/bin/env python3
"""Reject APKs that accidentally package the pristine Qt Android runtime."""
import argparse
import re
from zipfile import ZipFile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('apk')
args = parser.parse_args()
with ZipFile(args.apk) as apk:
    dex = [apk.read(name) for name in apk.namelist() if re.fullmatch(r'classes\d*\.dex', name)]
    platform = apk.read('lib/arm64-v8a/libplugins_platforms_qtforandroid_arm64-v8a.so')
    if b'Accessibility query timed out waiting for Qt' not in platform:
        parser.exit(1, 'FAIL: Qt platform plugin is missing the accessibility patch\n')
    if b'QAndroidRhiBackingStore: Failed to build graphics pipeline' not in platform:
        parser.exit(1, 'FAIL: Qt platform plugin is missing the partial backingstore patch\n')
for marker in (b'm_lastShownFocusGeneration', b'dismissEditingHandles', b'reportReadySize', b'hideLandscapeStatusBar',
               b'Lcom/obsproject/studio/ObsImeFocus;'):
    if not any(marker in blob for blob in dex):
        parser.exit(1, 'FAIL: Android Qt runtime patch missing: ' + marker.decode() + '\n')
print('PASS: packaged Qt Android accessibility, backingstore, IME, root layout, and status bar fixes.')
