# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
#
# ndk-build entry point for the libretro Android runner. The library itself is built by the
# repository Makefile for platform=android, so Android shares one source list, one warning set and
# one feature selection with every other target; this file only compiles that library first and
# publishes it under the module name and filename the buildbot expects.
LOCAL_PATH := $(call my-dir)
ANYGM_SOURCE_ROOT := $(abspath $(LOCAL_PATH)/..)
ANYGM_ANDROID_BUILD := $(ANYGM_SOURCE_ROOT)/build/android-$(TARGET_ARCH_ABI)
ANYGM_ANDROID_CORE := $(ANYGM_ANDROID_BUILD)/anygm_libretro_android.so
# The published prebuilt path belongs to the Linux x86-64 build image the runner uses.
ANYGM_NDK_BIN := $(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin
ANYGM_ANDROID_API := 24

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
  ANYGM_ANDROID_TRIPLE := aarch64-linux-android
else ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
  ANYGM_ANDROID_TRIPLE := armv7a-linux-androideabi
else
  $(error Unsupported Android ABI: $(TARGET_ARCH_ABI))
endif

include $(CLEAR_VARS)
LOCAL_MODULE := retro
LOCAL_MODULE_FILENAME := libretro
LOCAL_SRC_FILES := $(ANYGM_ANDROID_CORE)
LOCAL_ALLOW_MISSING_PREBUILT := true
LOCAL_STRIP_MODULE := false
include $(PREBUILT_SHARED_LIBRARY)

.PHONY: anygm_android_core-$(TARGET_ARCH_ABI)
$(ANYGM_ANDROID_CORE): anygm_android_core-$(TARGET_ARCH_ABI)

# ndk-build includes this file once per ABI in the same make process. Freeze
# the recursive build inputs on each target before the next inclusion.
anygm_android_core-$(TARGET_ARCH_ABI): private ANYGM_ANDROID_BUILD := $(ANYGM_ANDROID_BUILD)
anygm_android_core-$(TARGET_ARCH_ABI): private ANYGM_ANDROID_CORE := $(ANYGM_ANDROID_CORE)
anygm_android_core-$(TARGET_ARCH_ABI): private ANYGM_ANDROID_TRIPLE := $(ANYGM_ANDROID_TRIPLE)

# The compiler variables ndk-build exports describe an ndk-build module, not this recursive make,
# so they are cleared rather than passed down.
anygm_android_core-$(TARGET_ARCH_ABI):
	+env -u CC -u CXX -u AR -u RANLIB -u LD -u STRIP -u OBJCOPY -u OBJDUMP \
	    $(MAKE) -C "$(ANYGM_SOURCE_ROOT)" platform=android \
	    BUILD_DIR="$(ANYGM_ANDROID_BUILD)" CORE_TARGET="$(ANYGM_ANDROID_CORE)" \
	    CC="$(ANYGM_NDK_BIN)/$(ANYGM_ANDROID_TRIPLE)$(ANYGM_ANDROID_API)-clang" \
	    CXX="$(ANYGM_NDK_BIN)/$(ANYGM_ANDROID_TRIPLE)$(ANYGM_ANDROID_API)-clang++" \
	    AR="$(ANYGM_NDK_BIN)/llvm-ar" \
	    core
