# Attribution and source provenance

- This repository continues the Android port at [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android), based on its commit `307d9d2f5f49c1b91c2ebf3e6bf99b949ba3f917`. The original Git history is retained.
- The embedded `obs-studio/` source originates from [OBS Studio](https://github.com/obsproject/obs-studio), with the original port identifying OBS 32.2.1 / commit `57bfcf10a` as its baseline. See [OBS AUTHORS](obs-studio/AUTHORS) and [OBS COPYING](obs-studio/COPYING).
- This Android port and its new port code follow **GPL-2.0-or-later**. The full GPLv2 text is preserved in [LICENSE](LICENSE) and OBS's own license files. Individual bundled components retain their own notices and licenses.
- OBS names and logos identify the underlying project; this community port does not claim official endorsement. `screenshot.png` and `Live_Stream_Test.JPG` are inherited from the original Android port.
- Qt, FFmpeg, x264, Mbed TLS, curl, FreeType, libjpeg-turbo, SIMDe, uthash, Jansson, libusb/libuvc, SpeexDSP, RNNoise and other dependencies retain their respective upstream licenses. Dependencies are obtained/built separately; inspect their license files for the versions and build options you distribute.
- The supplied debug keystore is public development material inherited from the original port. It is not a private release-signing credential.

No VTube Studio binaries, models or proprietary SDK are included. The transparent-capture setting handles Alpha in frames supplied to OBS and is not a VTube Studio plugin or an NDI/Spout2 implementation.
