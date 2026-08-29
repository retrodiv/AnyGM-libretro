# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

DIAGNOSTICS ?= 0
# The optional hardware renderer. HARDWARE_RENDER=0 excludes the GPU objects and the libretro
# hardware bridge, omits the core option, and leaves a build with no unresolved graphics symbol.
HARDWARE_RENDER ?= 1

include Makefile.common

platform ?=
CROSS_COMPILE ?=
DEBUG ?= 0
STATIC_LINKING ?= 0
TEST ?=

ifeq ($(DIAGNOSTICS),1)
CPPFLAGS += -DANYGM_DIAGNOSTICS=1
endif

ifeq ($(HARDWARE_RENDER),0)
CPPFLAGS += -DANYGM_HARDWARE_RENDER=0
else
CPPFLAGS += -DANYGM_HARDWARE_RENDER=1
endif

ifeq ($(platform),)
platform := unix
ifneq (,$(findstring MINGW,$(shell uname -s 2>/dev/null)))
platform := win
else ifneq (,$(findstring Darwin,$(shell uname -s 2>/dev/null)))
platform := osx
endif
endif

BUILD_DIR ?= build/$(platform)

ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
ifeq ($(origin AR),default)
AR := $(CROSS_COMPILE)ar
endif
ifeq ($(origin CXX),default)
CXX := $(CROSS_COMPILE)g++
endif

ifeq ($(origin ARFLAGS),default)
ARFLAGS := rcs
endif
REPRODUCIBLE_CFLAGS ?= -ffile-prefix-map=$(CURDIR)=. -fdebug-prefix-map=$(CURDIR)=.

CPPFLAGS += $(addprefix -I,$(ANYGM_RUNTIME_INCLUDE_DIRS))
LIBRETRO_CPPFLAGS := $(CPPFLAGS) $(addprefix -I,$(ANYGM_LIBRETRO_INCLUDE_DIRS))
TEST_CPPFLAGS := $(CPPFLAGS) -Itests/support -Itests/unit/content/classic
CFLAGS += -std=gnu11 -Wall -Wextra $(REPRODUCIBLE_CFLAGS)
CXXFLAGS += -std=c++11 -Wall -Wextra $(REPRODUCIBLE_CFLAGS)
CXXFLAGS += -fno-exceptions -fno-rtti -fcheck-new
ifeq ($(DEBUG),1)
CFLAGS += -O0 -g3
else
CFLAGS += -O2
endif

CORE_BASENAME := $(TARGET_NAME)_libretro
CORE_EXTENSION := so
CORE_PLATFORM_LDLIBS := -lm -pthread
CORE_PLATFORM_LDFLAGS := -Wl,--version-script=link.T -Wl,--no-undefined -Wl,--build-id=none
CORE_SHARED_FLAG := -shared
PIC_FLAGS := -fPIC -fvisibility=hidden

ifneq (,$(findstring win,$(platform)))
CORE_EXTENSION := dll
CORE_PLATFORM_LDLIBS := -lm -lgdi32 -luser32
CORE_PLATFORM_LDFLAGS := -Wl,--no-insert-timestamp -static-libgcc -static-libstdc++
endif
ifneq (,$(findstring osx,$(platform)))
CORE_EXTENSION := dylib
CORE_PLATFORM_LDFLAGS := -dynamiclib
CORE_SHARED_FLAG :=
endif
ifneq (,$(findstring ios,$(platform)))
CORE_EXTENSION := dylib
CORE_PLATFORM_LDFLAGS := -dynamiclib
CORE_SHARED_FLAG :=
endif
ifneq (,$(findstring tvos,$(platform)))
CORE_EXTENSION := dylib
CORE_PLATFORM_LDFLAGS := -dynamiclib
CORE_SHARED_FLAG :=
endif

ANYGM_TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq (,$(findstring aarch64,$(ANYGM_TARGET_TRIPLE)))
ifneq (,$(findstring linux,$(ANYGM_TARGET_TRIPLE)))
# See src/host/anygm_libm_compat.c: pins sqrtf/atan2f to an older glibc
# symbol version than a newer aarch64-linux cross toolchain binds by default.
CORE_PLATFORM_LDFLAGS += -Wl,--wrap=sqrtf -Wl,--wrap=atan2f
endif
endif

RUNTIME_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_RUNTIME_SOURCES)) \
	$(patsubst %.cpp,$(BUILD_DIR)/obj/%.o,$(ANYGM_RUNTIME_CXX_SOURCES))
CORE_RUNTIME_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_CORE_SOURCES))
LIBRETRO_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_LIBRETRO_SOURCES))
TEST_HOST_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_TEST_HOST_SOURCES))
VENDORED_BZIP2_OBJECTS := $(filter $(BUILD_DIR)/obj/src/third_party/bzip2/%,$(RUNTIME_OBJECTS))
VENDORED_PXTONE_OBJECTS := $(filter $(BUILD_DIR)/obj/src/third_party/pxtone/%,$(RUNTIME_OBJECTS))
VENDORED_WW2OGG_OBJECTS := $(filter $(BUILD_DIR)/obj/src/third_party/ww2ogg/%,$(RUNTIME_OBJECTS))
WWISE_ADAPTER_OBJECT := $(BUILD_DIR)/obj/src/audio/banks/gml_wwise.o
PXTONE_ADAPTER_OBJECT := $(BUILD_DIR)/obj/src/audio/codecs/gml_pxtone.o
CORE_OBJECTS := $(RUNTIME_OBJECTS) $(LIBRETRO_OBJECTS)
UNIT_RUNTIME_OBJECTS := $(filter-out $(CORE_RUNTIME_OBJECTS),$(RUNTIME_OBJECTS)) $(TEST_HOST_OBJECTS)
RUNTIME_LIBRARY := $(BUILD_DIR)/libanygm_runtime.a
ifeq ($(STATIC_LINKING),1)
CORE_TARGET := $(CORE_BASENAME).a
else
CORE_TARGET := $(CORE_BASENAME).$(CORE_EXTENSION)
endif

TEST_DIR := $(BUILD_DIR)/tests
RUNTIME_TESTS := test_rng test_code_coverage test_persistent_room test_d3_state test_ds_grid \
	test_builtin_dispatch test_builtin_state test_vm_hotpath test_builtin_args test_vm_gc test_builtin_string_format test_builtin_string_search test_builtin_value_math test_builtin_filename_parts test_builtin_map_arrays test_builtin_struct_exists test_builtin_clipboard test_stacktop_scope test_value_compare
VIDEO_RENDERER_TESTS := test_renderer_effects test_renderer_crt test_renderer_surfaces \
	test_renderer_tiles test_renderer_primitives test_renderer_assets \
	test_renderer_texture_shells test_render_plan
ifneq ($(HARDWARE_RENDER),0)
VIDEO_RENDERER_TESTS += test_gpu
endif
CONTENT_TESTS := test_bytecode test_package test_classic test_sprite_masks
MEDIA_TESTS := test_hash test_image_codec test_font_raster
AUDIO_TESTS := test_mp3_detect
COMPATIBILITY_TESTS := test_compatibility
CHECK_TARGETS := $(addprefix $(TEST_DIR)/,$(RUNTIME_TESTS) $(VIDEO_RENDERER_TESTS) \
	$(CONTENT_TESTS) $(MEDIA_TESTS) $(AUDIO_TESTS) $(COMPATIBILITY_TESTS))
INTEGRATION_TESTS := $(TEST_DIR)/test_engine_instances $(TEST_DIR)/test_host_setting_budget $(TEST_DIR)/test_engine_blocking_wait \
	$(TEST_DIR)/test_engine_scoped_overrides \
	$(TEST_DIR)/test_engine_composed_raster
ifneq ($(HARDWARE_RENDER),0)
INTEGRATION_TESTS += $(TEST_DIR)/test_graphics_state
endif
CONTRACT_TESTS := $(TEST_DIR)/dummy_host $(TEST_DIR)/test_libretro_state_transport \
	$(TEST_DIR)/test_libretro_vfs_transport $(TEST_DIR)/test_libretro_option_defaults \
	$(TEST_DIR)/test_libretro_keyboard_source $(TEST_DIR)/test_libretro_gamepad_ports \
	$(TEST_DIR)/test_libretro_locale_variables $(TEST_DIR)/test_libretro_wall_time \
	$(TEST_DIR)/test_libretro_content_directories
ifneq ($(HARDWARE_RENDER),0)
CONTRACT_TESTS += $(TEST_DIR)/test_libretro_hardware_render
endif
SECURITY_TESTS := $(TEST_DIR)/test_content_security $(TEST_DIR)/test_state_security \
	$(TEST_DIR)/test_vfs $(TEST_DIR)/test_datafile_security \
	$(TEST_DIR)/test_bytecode_security
API_TEST_OBJECTS := $(TEST_DIR)/public_header_c.o $(TEST_DIR)/public_header_cpp.o

.PHONY: all core runtime check warnings-check api-check contract-check integration-check security-check \
	sanitizer-check architecture-check isolation-check export-check diagnostic-tests \
	diagnostics-check diagnostics-check-internal \
	clean

all: core

core: $(CORE_TARGET)

runtime: $(RUNTIME_LIBRARY)

ifeq ($(STATIC_LINKING),1)
$(CORE_TARGET): $(CORE_OBJECTS)
	$(AR) $(ARFLAGS) $@ $^
else
$(CORE_TARGET): $(CORE_OBJECTS) link.T
	$(CXX) $(LDFLAGS) $(CORE_SHARED_FLAG) $(PIC_FLAGS) $(CORE_PLATFORM_LDFLAGS) -o $@ $(CORE_OBJECTS) $(LDLIBS) $(CORE_PLATFORM_LDLIBS)
endif

$(RUNTIME_LIBRARY): $(RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

$(BUILD_DIR)/obj/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PIC_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PIC_FLAGS) -MMD -MP -c $< -o $@

$(LIBRETRO_OBJECTS): $(BUILD_DIR)/obj/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $(PIC_FLAGS) -MMD -MP -c $< -o $@

# Keep warnings from imported bzip2 sources isolated without weakening diagnostics for owned code.
$(VENDORED_BZIP2_OBJECTS): CFLAGS += -Wno-unused-parameter -Wno-implicit-fallthrough

# Keep imported pxtone warnings isolated without weakening diagnostics for owned runtime code.
# The adapter includes upstream headers, whose declarations trigger these two diagnostics, but
# its own implementation remains covered by every other warning (and -Werror).
$(VENDORED_PXTONE_OBJECTS): CXXFLAGS += -w
$(PXTONE_ADAPTER_OBJECT): CXXFLAGS += -Wno-unused-parameter -Wno-extra
$(VENDORED_WW2OGG_OBJECTS): CXXFLAGS += -w -fexceptions
$(WWISE_ADAPTER_OBJECT): CXXFLAGS += -fexceptions -Wno-unused-function

# Runtime tests compile their fixtures as C, so keep the C driver and add the
# platform C++ runtime explicitly for portable C++ codecs.
CXX_RUNTIME_LDLIBS := -lstdc++
ifneq (,$(filter osx ios tvos,$(platform)))
CXX_RUNTIME_LDLIBS := -lc++
endif
define link_runtime_test
	$(CC) $(1) $(CFLAGS) $^ -o $@ -lm -pthread $(CXX_RUNTIME_LDLIBS)
endef

$(TEST_DIR)/test_rng: tests/unit/runtime/test_rng.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_code_coverage: tests/unit/runtime/test_code_coverage.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_persistent_room: $(ANYGM_PERSISTENT_TEST_SOURCES) \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_d3_state: $(ANYGM_SOFTWARE3D_TEST_SOURCES) $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_args: tests/unit/runtime/test_builtin_args.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_map_arrays: tests/unit/runtime/test_builtin_map_arrays.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_struct_exists: tests/unit/runtime/test_builtin_struct_exists.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_stacktop_scope: tests/unit/runtime/test_stacktop_scope.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_vm_hotpath: tests/unit/runtime/test_vm_hotpath.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_ds_grid: tests/unit/runtime/test_ds_grid.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_string_format: tests/unit/runtime/test_builtin_string_format.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_string_search: tests/unit/runtime/test_builtin_string_search.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_clipboard: tests/unit/runtime/test_builtin_clipboard.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_value_math: tests/unit/runtime/test_builtin_value_math.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_filename_parts: tests/unit/runtime/test_builtin_filename_parts.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_value_compare: tests/unit/runtime/test_value_compare.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_vm_gc: tests/unit/runtime/test_vm_gc.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_dispatch: tests/unit/runtime/test_builtin_dispatch.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_state: tests/unit/runtime/test_builtin_state.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_renderer_effects: tests/unit/video/renderer/test_renderer_effects.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_renderer_crt: tests/unit/video/renderer/test_renderer_crt.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_renderer_surfaces: tests/unit/video/renderer/test_renderer_surfaces.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_renderer_assets: tests/unit/video/renderer/test_renderer_assets.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_renderer_texture_shells: tests/unit/video/renderer/test_renderer_texture_shells.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_renderer_tiles: tests/unit/video/renderer/test_renderer_tiles.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_renderer_primitives: tests/unit/video/renderer/test_renderer_primitives.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_render_plan: tests/unit/video/renderer/test_render_plan.c \
	tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_gpu: tests/unit/video/gpu/test_gpu.c \
	tests/support/anygm_test_runner.c tests/support/graphics_driver_fixture.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_bytecode: tests/unit/content/test_bytecode.c \
	src/host/anygm_host.c src/host/anygm_vfs.c \
	$(ANYGM_PROJECT_COMPILER_SOURCES) src/content/project/gmlc_project.c \
	src/content/project/gmlc_json.c src/content/project/gmlc_assets.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_package: tests/unit/content/test_package.c \
	tests/support/synthetic_content.c \
	src/host/anygm_host.c src/host/anygm_vfs.c src/host/stdio_vfs.c \
	src/content/datafile/gml_win.c src/content/bytecode/gml_bytecode.c \
	src/content/bytecode/gml_bc14.c src/content/bytecode/gml_bc15.c \
	src/content/bytecode/gml_bc17.c \
	$(ANYGM_MEDIA_SOURCES) $(ANYGM_PROJECT_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_sprite_masks: tests/unit/content/test_sprite_masks.c \
	$(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_classic: $(ANYGM_CLASSIC_TEST_SOURCES) \
	src/host/anygm_host.c src/host/anygm_vfs.c src/host/stdio_vfs.c \
	src/media/gml_image_codec.c src/media/gml_font_raster.c \
	src/content/datafile/gml_win.c src/content/bytecode/gml_bytecode.c \
	src/content/bytecode/gml_bc14.c src/content/bytecode/gml_bc15.c \
	src/content/bytecode/gml_bc17.c \
	src/content/classic/gmlc_classic.c $(ANYGM_CLASSIC_IMPORT_SOURCES) \
	src/content/project/gmlc_project.c src/content/project/gmlc_json.c \
	src/content/project/gmlc_assets.c
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_image_codec: tests/unit/media/test_image_codec.c \
	src/media/gml_image_codec.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_hash: tests/unit/media/test_hash.c src/media/gml_hash.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_mp3_detect: tests/unit/audio/test_mp3_detect.c \
	src/audio/codecs/gml_mp3.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_font_raster: tests/unit/media/test_font_raster.c \
	src/media/gml_font_raster.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_compatibility: tests/unit/compatibility/test_compatibility.c \
	src/compatibility/anygm_compatibility.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

ifeq ($(strip $(TEST)),)
check: CFLAGS += -Werror
check: CXXFLAGS += -Werror
check: warnings-check architecture-check api-check contract-check integration-check security-check $(CHECK_TARGETS)
	$(TEST_DIR)/test_bytecode
	$(TEST_DIR)/test_package
	$(TEST_DIR)/test_classic
	$(TEST_DIR)/test_sprite_masks
	$(TEST_DIR)/test_hash
	$(TEST_DIR)/test_image_codec
	$(TEST_DIR)/test_font_raster
	$(TEST_DIR)/test_mp3_detect
	$(TEST_DIR)/test_compatibility
	$(TEST_DIR)/test_renderer_effects
	$(TEST_DIR)/test_renderer_crt
	$(TEST_DIR)/test_renderer_surfaces
	$(TEST_DIR)/test_renderer_assets
	$(TEST_DIR)/test_renderer_texture_shells
	$(TEST_DIR)/test_renderer_tiles
	$(TEST_DIR)/test_renderer_primitives
	$(TEST_DIR)/test_render_plan
	$(if $(filter-out 0,$(HARDWARE_RENDER)),$(TEST_DIR)/test_gpu,true)
	$(TEST_DIR)/test_rng
	$(TEST_DIR)/test_persistent_room
	$(TEST_DIR)/test_d3_state
	$(TEST_DIR)/test_ds_grid
	$(TEST_DIR)/test_builtin_dispatch
	$(TEST_DIR)/test_builtin_state
	$(TEST_DIR)/test_vm_hotpath
	$(TEST_DIR)/test_builtin_args
	$(TEST_DIR)/test_vm_gc
	$(TEST_DIR)/test_builtin_string_format
	$(TEST_DIR)/test_builtin_string_search
	$(TEST_DIR)/test_builtin_clipboard
	$(TEST_DIR)/test_builtin_value_math
	$(TEST_DIR)/test_builtin_filename_parts
	$(TEST_DIR)/test_builtin_map_arrays
	$(TEST_DIR)/test_builtin_struct_exists \
	$(TEST_DIR)/test_value_compare \
	$(TEST_DIR)/test_stacktop_scope
else
FOCUSED_TEST_TARGET := $(TEST_DIR)/test_$(TEST)
check: CFLAGS += -Werror
check: CXXFLAGS += -Werror
check: architecture-check $(FOCUSED_TEST_TARGET)
	$(FOCUSED_TEST_TARGET) \
		$(if $(filter classic,$(TEST)),$(CLASSIC_TEST_ARGS),) \
		$(if $(filter d3_state,$(TEST)),$(D3_TEST_ARGS),) \
		$(if $(filter persistent_room,$(TEST)),$(PERSISTENT_TEST_ARGS),)
endif

# Target-specific variables propagate to the production objects. A fresh checkout compiles the
# complete core with warnings as errors; incremental runs rebuild each changed source.
warnings-check: CFLAGS += -Werror
warnings-check: core

architecture-check: builtin-registry-check
	tests/architecture/check_compatibility_boundaries.sh
	tests/architecture/check_code_map.sh
	tests/architecture/check_graphics_boundaries.sh
	python3 tests/architecture/check_content_boundaries.py
	python3 tests/architecture/check_cab_boundaries.py
	python3 tests/architecture/check_nsis_boundaries.py
	tests/architecture/check_numeric_conversions.sh

builtin-registry-check:
	python3 tests/architecture/check_builtin_registry.py check
	python3 tests/architecture/check_producer_fingerprint.py check

isolation-check:
	tests/architecture/check_runtime_isolation.sh

api-check: $(API_TEST_OBJECTS)

$(TEST_DIR)/public_header_c.o: tests/contract/public_header_c.c src/api/anygm.h
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_DIR)/public_header_cpp.o: tests/contract/public_header_cpp.cpp src/api/anygm.h
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

integration-check: $(INTEGRATION_TESTS)
	$(TEST_DIR)/test_engine_instances
	$(TEST_DIR)/test_host_setting_budget
	$(if $(filter-out 0,$(HARDWARE_RENDER)),$(TEST_DIR)/test_graphics_state,true)

contract-check: $(CONTRACT_TESTS)
	$(TEST_DIR)/dummy_host
	$(TEST_DIR)/test_libretro_state_transport
	$(TEST_DIR)/test_libretro_vfs_transport
	$(TEST_DIR)/test_libretro_option_defaults
	$(TEST_DIR)/test_libretro_keyboard_source
	$(TEST_DIR)/test_libretro_gamepad_ports
	$(TEST_DIR)/test_libretro_locale_variables
	$(TEST_DIR)/test_libretro_wall_time
	$(TEST_DIR)/test_libretro_content_directories
	$(if $(filter-out 0,$(HARDWARE_RENDER)),$(TEST_DIR)/test_libretro_hardware_render,true)

security-check: $(SECURITY_TESTS)
	$(TEST_DIR)/test_content_security
	$(TEST_DIR)/test_state_security
	$(TEST_DIR)/test_vfs
	$(TEST_DIR)/test_datafile_security
	$(TEST_DIR)/test_bytecode_security

$(TEST_DIR)/test_content_security: tests/fuzz/test_content_security.c \
	tests/support/synthetic_content.c \
	tests/support/memory_vfs.c \
	$(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_state_security: tests/fuzz/test_state_security.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_vfs: tests/unit/host/test_vfs.c src/host/anygm_vfs.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_datafile_security: tests/fuzz/test_datafile_security.c \
	tests/support/synthetic_content.c src/host/anygm_host.c src/host/anygm_vfs.c \
	src/host/stdio_vfs.c src/content/datafile/gml_win.c \
	src/content/bytecode/gml_bytecode.c src/content/bytecode/gml_bc14.c \
	src/content/bytecode/gml_bc15.c src/content/bytecode/gml_bc17.c \
	$(ANYGM_MEDIA_SOURCES) $(ANYGM_PROJECT_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_bytecode_security: tests/fuzz/test_bytecode_security.c \
	src/host/anygm_vfs.c src/content/datafile/gml_win.c \
	src/content/bytecode/gml_bytecode.c src/content/bytecode/gml_bc14.c \
	src/content/bytecode/gml_bc15.c src/content/bytecode/gml_bc17.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

sanitizer-check:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) -j1 BUILD_DIR=build/sanitize DEBUG=1 \
		CFLAGS="-std=gnu11 -Wall -Wextra -O0 -g3 $(REPRODUCIBLE_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,undefined" \
		security-check contract-check integration-check

export-check: core
	@if [ "$(CORE_EXTENSION)" != "so" ]; then \
		printf '%s\n' "export-check is available for native Unix shared builds"; \
	else \
		tests/contract/check_libretro_exports.sh "$(CORE_TARGET)"; \
	fi

$(TEST_DIR)/test_engine_instances: tests/integration/test_engine_instances.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_host_setting_budget: tests/integration/test_host_setting_budget.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_engine_blocking_wait: tests/integration/test_engine_blocking_wait.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_engine_scoped_overrides: tests/integration/test_engine_scoped_overrides.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_engine_composed_raster: tests/integration/test_engine_composed_raster.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_graphics_state: tests/integration/test_graphics_state.c \
	tests/support/synthetic_content.c tests/support/graphics_driver_fixture.c \
	$(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/dummy_host: tests/contract/dummy_host.c tests/support/synthetic_content.c \
	$(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_libretro_state_transport: tests/contract/libretro_state_transport.c \
	src/adapters/libretro/libretro_entry.c src/adapters/libretro/libretro_context.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_option_defaults: tests/contract/libretro_option_defaults.c \
	src/adapters/libretro/libretro_options.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_keyboard_source: tests/contract/libretro_keyboard_source.c \
	src/adapters/libretro/libretro_input.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_gamepad_ports: tests/contract/libretro_gamepad_ports.c \
	src/adapters/libretro/libretro_input.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_locale_variables: tests/contract/libretro_locale_variables.c \
	src/adapters/libretro/libretro_context.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_wall_time: tests/contract/libretro_wall_time.c \
	src/adapters/libretro/libretro_context.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_content_directories: tests/contract/libretro_content_directories.c \
	src/adapters/libretro/libretro_entry.c src/adapters/libretro/libretro_context.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_vfs_transport: tests/contract/libretro_vfs_transport.c \
	src/adapters/libretro/libretro_vfs.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_hardware_render: tests/contract/libretro_hardware_render.c \
	$(ANYGM_LIBRETRO_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

diagnostic-tests: $(TEST_DIR)/test_load $(TEST_DIR)/test_vm

diagnostics-check:
	$(MAKE) -j1 DIAGNOSTICS=1 BUILD_DIR=$(BUILD_DIR)/diagnostics \
		CORE_BASENAME=$(abspath $(BUILD_DIR)/diagnostics/anygm-diagnostics) \
		diagnostics-check-internal

diagnostics-check-internal: CFLAGS += -Werror
diagnostics-check-internal: core $(TEST_DIR)/test_vm_diagnostics
	$(TEST_DIR)/test_vm_diagnostics

$(TEST_DIR)/test_vm_diagnostics: tests/unit/runtime/test_vm_diagnostics.c \
	tests/support/anygm_test_runner.c src/host/anygm_host.c \
	src/runtime/vm/gml_value.c src/runtime/vm/gml_vm_diagnostics.c
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_load: tests/unit/content/test_load.c \
	src/host/anygm_vfs.c src/host/stdio_vfs.c \
	src/content/datafile/gml_win.c src/content/bytecode/gml_bytecode.c \
	src/content/bytecode/gml_bc14.c src/content/bytecode/gml_bc15.c \
	src/content/bytecode/gml_bc17.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_vm: tests/unit/runtime/test_vm.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

clean:
	tests/architecture/safe_clean.sh "$(BUILD_DIR)" \
		$(CORE_BASENAME).so $(CORE_BASENAME).dll \
		$(CORE_BASENAME).dylib $(CORE_BASENAME).a

-include $(RUNTIME_OBJECTS:.o=.d) $(LIBRETRO_OBJECTS:.o=.d) $(TEST_HOST_OBJECTS:.o=.d)
