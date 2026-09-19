# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
#
# ndk-build application settings for the libretro Android runner. The C++ runtime selection is the
# repository Makefile's, because that is what compiles and links the core.
APP_ABI := arm64-v8a
APP_PLATFORM := android-24
APP_STL := none
APP_OPTIM := release
