# OpenXR headers

`include/openxr/openxr.h`, `openxr_platform.h` and `openxr_platform_defines.h`
from the OpenXR SDK 1.1.49 (KhronosGroup/OpenXR-SDK), unmodified.
Copyright 2017-2025 The Khronos Group Inc.; Apache-2.0 OR MIT (see the SPDX
line in each header).

Only the PSX_VR build (the Pico 4 VR APK, `vr_openxr.c`) uses them. The
OpenXR loader itself is not vendored: `tools/android/make_apk.py --vr`
downloads `openxr_loader_for_android-1.1.49.aar` (Apache-2.0) from Maven
Central, checked against its SHA-1 and SHA-256, and puts its
`libopenxr_loader.so` in the APK.
