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
for marker in (b'm_lastShownFocusGeneration', b'dismissEditingHandles', b'reportReadySize',
               b'Lcom/obsproject/studio/ObsImeFocus;'):
    if not any(marker in blob for blob in dex):
        parser.exit(1, 'FAIL: Android Qt runtime patch missing: ' + marker.decode() + '\n')
print('PASS: IME and root-layout Qt runtime patches are present in APK Dex.')
