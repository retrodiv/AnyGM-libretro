# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

DIAGNOSTICS ?= 0
# The optional hardware renderer. HARDWARE_RENDER=0 excludes the GPU objects and the libretro
# hardware bridge, omits the core option, and leaves a build with no unresolved graphics symbol.
HARDWARE_RENDER ?= 1

include Makefile.common

# A goal-less invocation has to build the core. The first rule in this file generates the macOS
# export list, so without this it becomes the default goal: `make` then writes that one file,
# reports success, and leaves every object untouched, which is indistinguishable from a build.
.DEFAULT_GOAL := all

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

# The libretro Apple templates set CROSS_COMPILE=1 as a boolean and name the target in
# LIBRETRO_APPLE_PLATFORM, while everywhere else CROSS_COMPILE is a toolchain prefix. Prefixing the
# compiler with "1" there would ask for a compiler called 1gcc, so the Apple convention is
# recognized and turned into the -target/-isysroot pair those builds actually need.
ANYGM_APPLE_CROSS := 0
ifneq (,$(filter osx ios tvos,$(platform)))
ifeq ($(CROSS_COMPILE),1)
ANYGM_APPLE_CROSS := 1
endif
endif

ifeq ($(ANYGM_APPLE_CROSS),1)
TOOLCHAIN_PREFIX :=
else
TOOLCHAIN_PREFIX := $(CROSS_COMPILE)
endif

ifeq ($(origin CC),default)
CC := $(TOOLCHAIN_PREFIX)gcc
endif
ifeq ($(origin AR),default)
AR := $(TOOLCHAIN_PREFIX)ar
endif
ifeq ($(origin CXX),default)
CXX := $(TOOLCHAIN_PREFIX)g++
endif

ifeq ($(ANYGM_APPLE_CROSS),1)
# The libretro osx-arm64 template exports the SDK path it found, which is the only source of one
# for a cross build on a host that has no Apple toolchain; a native host that can query its own
# installation falls back to xcodebuild.
ANYGM_APPLE_SYSROOT := $(if $(LIBRETRO_APPLE_ISYSROOT),$(LIBRETRO_APPLE_ISYSROOT),$(shell xcodebuild -version -sdk macosx Path 2>/dev/null))
APPLE_TARGET_FLAGS := $(if $(LIBRETRO_APPLE_PLATFORM),-target $(LIBRETRO_APPLE_PLATFORM)) \
	$(if $(ANYGM_APPLE_SYSROOT),-isysroot $(ANYGM_APPLE_SYSROOT))
CFLAGS += $(APPLE_TARGET_FLAGS)
CXXFLAGS += $(APPLE_TARGET_FLAGS)
LDFLAGS += $(APPLE_TARGET_FLAGS)
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
# The Android library keeps the libretro buildbot's platform-specific filename, which is how the
# published artifact is told apart from the Linux one it shares an extension with.
CORE_ARTIFACT_SUFFIX :=
CORE_PLATFORM_LDLIBS := -lm -pthread
CORE_PLATFORM_LDFLAGS := -Wl,--version-script=link.T -Wl,--no-undefined -Wl,--build-id=none
CORE_SHARED_FLAG := -shared
CORE_LINK_INPUTS := link.T
PIC_FLAGS := -fPIC -fvisibility=hidden

# Apple targets export their entry points through an explicit symbol list, because the Mach-O
# linker has no version script. The list is derived from the reviewed libretro ABI that the ELF
# export check compares against, so the two spellings of the same contract cannot drift apart;
# -exported_symbols_list takes no wildcards, so every entry point is written out with the leading
# underscore the Mach-O namespace uses.
MACHO_EXPORT_LIST := $(BUILD_DIR)/libretro.exports.macho
$(MACHO_EXPORT_LIST): tests/contract/libretro_exports.txt
	mkdir -p $(dir $@)
	sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$$/d' -e 's/^/_/' $< > $@
ifneq (,$(filter osx ios tvos,$(platform)))
CORE_LINK_INPUTS := $(MACHO_EXPORT_LIST)
endif

ifneq (,$(findstring win,$(platform)))
CORE_EXTENSION := dll
CORE_PLATFORM_LDLIBS := -lm -lgdi32 -luser32
CORE_PLATFORM_LDFLAGS := -Wl,--no-insert-timestamp -static-libgcc -static-libstdc++
endif
ifneq (,$(findstring android,$(platform)))
CORE_ARTIFACT_SUFFIX := _android
# A device with 16 KiB pages refuses a library whose load segments are aligned to 4 KiB, and the
# alignment is recorded in the file rather than applied by the loader, so it is a link-time
# property of the artifact itself. The C++ runtime is linked statically for the same reason the
# Windows build does it: the core then depends on the platform's own libraries only.
CORE_PLATFORM_LDFLAGS := -Wl,--version-script=link.T -Wl,--no-undefined -Wl,--build-id=none \
	-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -static-libstdc++
endif
ifneq (,$(findstring osx,$(platform)))
CORE_EXTENSION := dylib
CORE_PLATFORM_LDFLAGS := -dynamiclib -Wl,-exported_symbols_list,$(MACHO_EXPORT_LIST)
CORE_SHARED_FLAG :=
endif
ifneq (,$(findstring ios,$(platform)))
CORE_EXTENSION := dylib
CORE_PLATFORM_LDFLAGS := -dynamiclib -Wl,-exported_symbols_list,$(MACHO_EXPORT_LIST)
CORE_SHARED_FLAG :=
endif
ifneq (,$(findstring tvos,$(platform)))
CORE_EXTENSION := dylib
CORE_PLATFORM_LDFLAGS := -dynamiclib -Wl,-exported_symbols_list,$(MACHO_EXPORT_LIST)
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
DELTA_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_DELTA_SOURCES))
VENDORED_LZMA_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_LZMA_SOURCES))
VENDORED_XDELTA_OBJECT := $(BUILD_DIR)/obj/src/third_party/xdelta3/xdelta3.o
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
CORE_TARGET := $(CORE_BASENAME)$(CORE_ARTIFACT_SUFFIX).$(CORE_EXTENSION)
endif

TEST_DIR := $(BUILD_DIR)/tests
RUNTIME_TESTS := test_vm_conv_bool test_vm_unset_reads test_rng test_code_coverage test_persistent_room test_d3_state test_ds_grid test_builtin_sequence_copy test_builtin_sequence_codecs test_builtin_map_access test_builtin_contact_action test_builtin_delayed_calls test_builtin_resource_queries test_builtin_animation_queries test_builtin_buffer_hash test_builtin_draw_queries test_builtin_tile_draw test_builtin_tilemap_position test_builtin_paths test_builtin_collision_lists \
	test_builtin_dispatch test_builtin_state test_vm_hotpath test_builtin_args test_vm_gc test_builtin_string_format test_builtin_string_search test_builtin_value_math test_builtin_extrema test_builtin_filename_parts test_builtin_map_arrays test_builtin_struct_exists test_builtin_clipboard test_builtin_instance_names test_builtin_grid_roundtrip test_builtin_gpu_zstate test_builtin_tilemap_tileset test_stacktop_scope test_value_compare
VIDEO_RENDERER_TESTS := test_renderer_effects test_renderer_postprocess test_renderer_surfaces \
	test_renderer_tiles test_renderer_primitives test_renderer_assets test_renderer_fonts \
	test_renderer_texture_shells test_render_plan
ifneq ($(HARDWARE_RENDER),0)
VIDEO_RENDERER_TESTS += test_gpu
endif
CONTENT_TESTS := test_bytecode test_package test_classic test_sprite_masks test_content_transform test_content_config test_content_delta test_content_patch
MEDIA_TESTS := test_hash test_image_codec test_font_raster
AUDIO_TESTS := test_mp3_detect
COMPATIBILITY_TESTS := test_compatibility
CHECK_TARGETS := $(addprefix $(TEST_DIR)/,$(RUNTIME_TESTS) $(VIDEO_RENDERER_TESTS) \
	$(CONTENT_TESTS) $(MEDIA_TESTS) $(AUDIO_TESTS) $(COMPATIBILITY_TESTS))
INTEGRATION_TESTS := $(TEST_DIR)/test_engine_instances $(TEST_DIR)/test_mouse_input $(TEST_DIR)/test_host_setting_budget $(TEST_DIR)/test_engine_blocking_wait \
	$(TEST_DIR)/test_font_state \
	$(TEST_DIR)/test_engine_content_config \
	$(TEST_DIR)/test_engine_content_patch \
	$(TEST_DIR)/test_engine_payload_names \
	$(TEST_DIR)/test_engine_scoped_overrides \
	$(TEST_DIR)/test_engine_composed_raster
ifneq ($(HARDWARE_RENDER),0)
INTEGRATION_TESTS += $(TEST_DIR)/test_graphics_state
endif
CONTRACT_TESTS := $(TEST_DIR)/dummy_host $(TEST_DIR)/test_builtin_file_io $(TEST_DIR)/test_libretro_state_transport \
	$(TEST_DIR)/test_libretro_tilemap_state \
	$(TEST_DIR)/test_libretro_crt_output \
	$(TEST_DIR)/test_libretro_vfs_transport $(TEST_DIR)/test_libretro_option_defaults \
	$(TEST_DIR)/test_libretro_keyboard_source $(TEST_DIR)/test_libretro_gamepad_ports \
	$(TEST_DIR)/test_libretro_locale_variables $(TEST_DIR)/test_libretro_wall_time \
	$(TEST_DIR)/test_libretro_content_directories $(TEST_DIR)/test_libretro_cache_vfs
ifneq ($(HARDWARE_RENDER),0)
CONTRACT_TESTS += $(TEST_DIR)/test_libretro_hardware_render
endif
SECURITY_TESTS := $(TEST_DIR)/test_content_security $(TEST_DIR)/test_state_security \
	$(TEST_DIR)/test_vfs $(TEST_DIR)/test_datafile_security \
	$(TEST_DIR)/test_bytecode_security $(TEST_DIR)/test_fmod_security
API_TEST_OBJECTS := $(TEST_DIR)/public_header_c.o $(TEST_DIR)/public_header_cpp.o

.PHONY: all core runtime check warnings-check api-check contract-check integration-check security-check \
	sanitizer-check architecture-check isolation-check export-check diagnostic-tests \
	provenance-check builtin-registry-check notices-check audio-data-check \
	diagnostics-check diagnostics-check-internal \
	clean

all: core

core: $(CORE_TARGET)

runtime: $(RUNTIME_LIBRARY)

ifeq ($(STATIC_LINKING),1)
$(CORE_TARGET): $(CORE_OBJECTS)
	$(AR) $(ARFLAGS) $@ $^
else
$(CORE_TARGET): $(CORE_OBJECTS) $(CORE_LINK_INPUTS)
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

$(VENDORED_LZMA_OBJECTS): CPPFLAGS += $(ANYGM_LZMA_CPPFLAGS)
$(VENDORED_XDELTA_OBJECT): CPPFLAGS += -Isrc/third_party/liblzma/api
# Upstream shares encoder/decoder helpers and switch-based state machines.
# Isolate only these imported-code diagnostics; owned wrappers retain -Werror.
$(VENDORED_XDELTA_OBJECT): CFLAGS += -Wno-unused-parameter -Wno-unused-function -Wno-implicit-fallthrough

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

$(TEST_DIR)/test_builtin_sequence_copy: tests/unit/runtime/test_builtin_sequence_copy.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_sequence_codecs: tests/unit/runtime/test_builtin_sequence_codecs.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_map_access: tests/unit/runtime/test_builtin_map_access.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_contact_action: tests/unit/runtime/test_builtin_contact_action.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_delayed_calls: tests/unit/runtime/test_builtin_delayed_calls.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_resource_queries: tests/unit/runtime/test_builtin_resource_queries.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_animation_queries: tests/unit/runtime/test_builtin_animation_queries.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_buffer_hash: tests/unit/runtime/test_builtin_buffer_hash.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_draw_queries: tests/unit/runtime/test_builtin_draw_queries.c \
  tests/unit/media/font_test_fixture.h tests/support/memory_vfs.c \
  tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $(filter-out %.h,$^) -o $@ -lm -pthread $(CXX_RUNTIME_LDLIBS)

$(TEST_DIR)/test_builtin_paths: tests/unit/runtime/test_builtin_paths.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_tile_draw: tests/unit/runtime/test_builtin_tile_draw.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_collision_lists: tests/unit/runtime/test_builtin_collision_lists.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_file_io: tests/contract/test_builtin_file_io.c tests/support/memory_vfs.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
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

$(TEST_DIR)/test_builtin_instance_names: tests/unit/runtime/test_builtin_instance_names.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_grid_roundtrip: tests/unit/runtime/test_builtin_grid_roundtrip.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_gpu_zstate: tests/unit/runtime/test_builtin_gpu_zstate.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_tilemap_tileset: tests/unit/runtime/test_builtin_tilemap_tileset.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_tilemap_position: tests/unit/runtime/test_builtin_tilemap_position.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_builtin_value_math: tests/unit/runtime/test_builtin_value_math.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_builtin_extrema: tests/unit/runtime/test_builtin_extrema.c tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

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

$(TEST_DIR)/test_renderer_postprocess: tests/unit/video/renderer/test_renderer_postprocess.c \
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
	$(ANYGM_INPUT_TRANSFORM_SOURCES) $(DELTA_OBJECTS) src/media/gml_hash.c \
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

$(TEST_DIR)/test_content_delta: tests/unit/content/test_content_delta.c \
	tests/support/anygm_test_runner.c tests/fixtures/vcdiff/fixtures.h $(DELTA_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

$(TEST_DIR)/test_content_transform: tests/unit/content/test_content_transform.c \
	$(ANYGM_INPUT_TRANSFORM_SOURCES) $(DELTA_OBJECTS) src/media/gml_hash.c src/media/gml_image_codec.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_content_config: tests/unit/content/test_content_config.c \
	$(ANYGM_INPUT_TRANSFORM_SOURCES) $(DELTA_OBJECTS) \
	src/media/gml_hash.c src/media/gml_image_codec.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_content_patch: tests/unit/content/test_content_patch.c tests/support/anygm_test_runner.c \
	$(ANYGM_INPUT_TRANSFORM_SOURCES) $(DELTA_OBJECTS) src/media/gml_hash.c src/media/gml_image_codec.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

# A neutral development boundary for explicit external configuration consumers.
# First-party entry points are visible; vendored implementation objects stay hidden.
CONTENT_TRANSFORM_LIBRARY ?= $(BUILD_DIR)/libanygm_content_transform.so
.PHONY: content-transform-library
content-transform-library: $(CONTENT_TRANSFORM_LIBRARY)
$(CONTENT_TRANSFORM_LIBRARY): $(ANYGM_INPUT_TRANSFORM_SOURCES) \
	src/content/container/content_delta.c $(filter-out %/content_delta.o,$(DELTA_OBJECTS)) \
	src/media/gml_hash.c src/media/gml_image_codec.c src/host/anygm_notices.c \
	$(wildcard src/content/container/content_*.h) src/generated/anygm_third_party_notices.h
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(filter %.c %.o,$^) -o $@ -lm

$(TEST_DIR)/test_mp3_detect: tests/unit/audio/test_mp3_detect.c \
	src/audio/codecs/gml_mp3.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_font_raster: tests/unit/media/test_font_raster.c \
	tests/unit/media/font_test_fixture.h src/media/gml_font_raster.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@ -lm

$(TEST_DIR)/test_renderer_fonts: tests/unit/video/renderer/test_renderer_fonts.c \
	tests/unit/media/font_test_fixture.h tests/support/memory_vfs.c \
	tests/support/anygm_test_runner.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $(filter-out %.h,$^) -o $@ -lm -pthread $(CXX_RUNTIME_LDLIBS)

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
	$(TEST_DIR)/test_content_config
	$(TEST_DIR)/test_content_delta
	$(TEST_DIR)/test_content_patch
	$(TEST_DIR)/test_hash
	$(TEST_DIR)/test_image_codec
	$(TEST_DIR)/test_font_raster
	$(TEST_DIR)/test_mp3_detect
	$(TEST_DIR)/test_compatibility
	$(TEST_DIR)/test_renderer_effects
	$(TEST_DIR)/test_renderer_postprocess
	$(TEST_DIR)/test_renderer_surfaces
	$(TEST_DIR)/test_renderer_assets
	$(TEST_DIR)/test_renderer_fonts
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
	$(TEST_DIR)/test_builtin_instance_names
	$(TEST_DIR)/test_builtin_grid_roundtrip
	$(TEST_DIR)/test_builtin_gpu_zstate
	$(TEST_DIR)/test_builtin_tilemap_tileset
	$(TEST_DIR)/test_builtin_tilemap_position
	$(TEST_DIR)/test_builtin_value_math
	$(TEST_DIR)/test_builtin_filename_parts
	$(TEST_DIR)/test_builtin_map_arrays
	$(TEST_DIR)/test_builtin_sequence_copy
	$(TEST_DIR)/test_builtin_sequence_codecs
	$(TEST_DIR)/test_builtin_map_access
	$(TEST_DIR)/test_builtin_contact_action
	$(TEST_DIR)/test_builtin_delayed_calls
	$(TEST_DIR)/test_builtin_resource_queries
	$(TEST_DIR)/test_builtin_animation_queries
	$(TEST_DIR)/test_builtin_buffer_hash
	$(TEST_DIR)/test_builtin_draw_queries
	$(TEST_DIR)/test_builtin_tile_draw
	$(TEST_DIR)/test_builtin_paths
	$(TEST_DIR)/test_builtin_collision_lists
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
		$(if $(filter persistent_room,$(TEST)),$(PERSISTENT_TEST_ARGS),) \
		$(if $(filter fmod_security,$(TEST)),$(FMOD_TEST_ARGS),)
endif

# Target-specific variables propagate to the production objects. A fresh checkout compiles the
# complete core with warnings as errors; incremental runs rebuild each changed source.
warnings-check: CFLAGS += -Werror
warnings-check: core

architecture-check: builtin-registry-check provenance-check notices-check audio-data-check delta-vendor-check version-policy-check
	tests/architecture/check_compatibility_boundaries.sh
	tests/architecture/check_code_map.sh
	tests/architecture/check_graphics_boundaries.sh
	python3 tests/architecture/check_content_boundaries.py
	python3 tests/architecture/check_cab_boundaries.py
	python3 tests/architecture/check_nsis_boundaries.py
	tests/architecture/check_numeric_conversions.sh

.PHONY: delta-vendor-check
delta-vendor-check:
	python3 tests/architecture/check_delta_vendor.py

# The project's version is written in exactly one place, and every publishable copy of the metadata
# says "Git" instead of a number. The check also exercises the one documented increment.
.PHONY: version-policy-check
version-policy-check:
	python3 tests/architecture/check_version_policy.py

# Check the internal files named by provenance directives against their recorded digests.
provenance-check:
	python3 tests/architecture/check_provenance.py

audio-data-check:
	python3 tests/architecture/check_audio_data.py

# Verify that the embedded notice table matches every local notice input.
notices-check:
	python3 tests/architecture/check_notices.py check

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
	$(TEST_DIR)/test_mouse_input
	$(TEST_DIR)/test_font_state
	$(TEST_DIR)/test_engine_content_config
	$(TEST_DIR)/test_engine_content_patch
	$(TEST_DIR)/test_engine_payload_names
	$(TEST_DIR)/test_host_setting_budget
	$(if $(filter-out 0,$(HARDWARE_RENDER)),$(TEST_DIR)/test_graphics_state,true)

contract-check: $(CONTRACT_TESTS)
	$(TEST_DIR)/dummy_host
	$(TEST_DIR)/test_builtin_file_io
	$(TEST_DIR)/test_libretro_state_transport
	$(TEST_DIR)/test_libretro_tilemap_state
	$(TEST_DIR)/test_libretro_crt_output
	$(TEST_DIR)/test_libretro_vfs_transport
	$(TEST_DIR)/test_libretro_option_defaults
	$(TEST_DIR)/test_libretro_keyboard_source
	$(TEST_DIR)/test_libretro_gamepad_ports
	$(TEST_DIR)/test_libretro_locale_variables
	$(TEST_DIR)/test_libretro_wall_time
	$(TEST_DIR)/test_libretro_content_directories
	$(TEST_DIR)/test_libretro_cache_vfs
	$(if $(filter-out 0,$(HARDWARE_RENDER)),$(TEST_DIR)/test_libretro_hardware_render,true)

security-check: $(SECURITY_TESTS)
	$(TEST_DIR)/test_content_security
	$(TEST_DIR)/test_state_security
	$(TEST_DIR)/test_vfs
	$(TEST_DIR)/test_datafile_security
	$(TEST_DIR)/test_bytecode_security
	$(TEST_DIR)/test_fmod_security

$(TEST_DIR)/test_fmod_security: tests/fuzz/test_fmod_security.c \
	src/audio/banks/gml_fmod.c tests/support/anygm_test_runner.c \
	tests/support/memory_vfs.c src/host/anygm_host.c src/host/anygm_vfs.c \
	$(TEST_DIR)/stb_vorbis_fmod_test.o
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) -DSTB_VORBIS_NO_STDIO \
		-DSTB_VORBIS_NO_PUSHDATA_API tests/fuzz/test_fmod_security.c \
		tests/support/anygm_test_runner.c tests/support/memory_vfs.c \
		src/host/anygm_host.c src/host/anygm_vfs.c \
		$(TEST_DIR)/stb_vorbis_fmod_test.o -o $@ -lm

# The test links the unchanged imported implementation separately so its
# upstream warnings cannot mask warnings in the FSB5 reader under test.
$(TEST_DIR)/stb_vorbis_fmod_test.o: src/third_party/stb/stb_vorbis.c
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) -w -DSTB_VORBIS_NO_STDIO \
		-DSTB_VORBIS_NO_PUSHDATA_API -c $< -o $@

$(TEST_DIR)/test_content_security: tests/fuzz/test_content_security.c \
	tests/unit/content/classic/classic_test_fixture.c \
	tests/support/vcdiff_fixture.c \
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
	@if [ "$(CORE_EXTENSION)" != "so" ] && [ "$(CORE_EXTENSION)" != "dylib" ]; then \
		printf '%s\n' "export-check is available for native Unix and Apple shared builds"; \
	else \
		tests/contract/check_libretro_exports.sh "$(CORE_TARGET)"; \
	fi

$(TEST_DIR)/test_font_state: tests/integration/test_font_state.c \
	tests/unit/media/font_test_fixture.h tests/support/memory_vfs.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $(filter-out %.h,$^) -o $@ -lm -pthread $(CXX_RUNTIME_LDLIBS)

$(TEST_DIR)/test_engine_instances: tests/integration/test_engine_instances.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_mouse_input: tests/integration/test_mouse_input.c \
	tests/support/synthetic_content.c tests/support/anygm_test_runner.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
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

$(TEST_DIR)/test_engine_content_config: tests/integration/test_engine_content_config.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_engine_content_patch: tests/integration/test_engine_content_patch.c \
	tests/support/vcdiff_fixture.c tests/support/anygm_test_runner.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(TEST_CPPFLAGS))

$(TEST_DIR)/test_engine_payload_names: tests/integration/test_engine_payload_names.c \
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
	src/adapters/libretro/libretro_entry.c src/adapters/libretro/libretro_context.c \
	src/adapters/libretro/libretro_cache.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_tilemap_state: tests/contract/libretro_tilemap_state.c \
	tests/support/synthetic_content.c $(CORE_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) -Itests/support $(CFLAGS) $^ -o $@ -lm -pthread $(CXX_RUNTIME_LDLIBS)

$(TEST_DIR)/test_libretro_option_defaults: tests/contract/libretro_option_defaults.c \
	src/adapters/libretro/libretro_options.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_crt_output: tests/contract/libretro_crt_output.c \
	tests/support/synthetic_content.c $(CORE_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) -Itests/support $(CFLAGS) $^ -o $@ -lm -pthread $(CXX_RUNTIME_LDLIBS)

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
	src/adapters/libretro/libretro_entry.c src/adapters/libretro/libretro_context.c \
	src/adapters/libretro/libretro_cache.c src/adapters/libretro/libretro_vfs.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_vfs_transport: tests/contract/libretro_vfs_transport.c \
	src/adapters/libretro/libretro_vfs.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_libretro_cache_vfs: tests/contract/libretro_cache_vfs.c \
	src/adapters/libretro/libretro_cache.c src/adapters/libretro/libretro_vfs.c
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
		$(CORE_BASENAME).dylib $(CORE_BASENAME).a $(CORE_BASENAME)_android.so

-include $(RUNTIME_OBJECTS:.o=.d) $(LIBRETRO_OBJECTS:.o=.d) $(TEST_HOST_OBJECTS:.o=.d)

$(TEST_DIR)/test_vm_unset_reads: tests/unit/runtime/test_vm_unset_reads.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))

$(TEST_DIR)/test_vm_conv_bool: tests/unit/runtime/test_vm_conv_bool.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(call link_runtime_test,$(CPPFLAGS))
