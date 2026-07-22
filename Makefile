# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

include Makefile.common

platform ?=
CROSS_COMPILE ?=
DEBUG ?= 0
STATIC_LINKING ?= 0
TEST ?=

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
TEST_CPPFLAGS := $(CPPFLAGS) -Itests/support
CFLAGS += -std=gnu11 -Wall -Wextra $(REPRODUCIBLE_CFLAGS)
CXXFLAGS += -std=c++11 -Wall -Wextra $(REPRODUCIBLE_CFLAGS)
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
CORE_PLATFORM_LDFLAGS := -Wl,--no-insert-timestamp
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

RUNTIME_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_RUNTIME_SOURCES))
LIBRETRO_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_LIBRETRO_SOURCES))
TEST_HOST_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/obj/%.o,$(ANYGM_TEST_HOST_SOURCES))
CORE_OBJECTS := $(RUNTIME_OBJECTS) $(LIBRETRO_OBJECTS)
UNIT_RUNTIME_OBJECTS := $(filter-out $(BUILD_DIR)/obj/src/core/engine.o,$(RUNTIME_OBJECTS)) $(TEST_HOST_OBJECTS)
RUNTIME_LIBRARY := $(BUILD_DIR)/libanygm_runtime.a
ifeq ($(STATIC_LINKING),1)
CORE_TARGET := $(CORE_BASENAME).a
else
CORE_TARGET := $(CORE_BASENAME).$(CORE_EXTENSION)
endif

TEST_DIR := $(BUILD_DIR)/tests
RUNTIME_TESTS := test_rng test_persistent_room test_d3_state test_ds_grid
CONTENT_TESTS := test_bytecode test_classic
COMPATIBILITY_TESTS := test_compatibility
CHECK_TARGETS := $(addprefix $(TEST_DIR)/,$(RUNTIME_TESTS) $(CONTENT_TESTS) $(COMPATIBILITY_TESTS))
INTEGRATION_TESTS := $(TEST_DIR)/test_engine_instances
CONTRACT_TESTS := $(TEST_DIR)/dummy_host $(TEST_DIR)/test_libretro_state_transport
SECURITY_TESTS := $(TEST_DIR)/test_content_security $(TEST_DIR)/test_state_security \
	$(TEST_DIR)/test_vfs $(TEST_DIR)/test_datafile_security \
	$(TEST_DIR)/test_bytecode_security
API_TEST_OBJECTS := $(TEST_DIR)/public_header_c.o $(TEST_DIR)/public_header_cpp.o

.PHONY: all core runtime check api-check contract-check integration-check security-check \
	sanitizer-check architecture-check isolation-check export-check diagnostic-tests clean

all: core

core: $(CORE_TARGET)

runtime: $(RUNTIME_LIBRARY)

ifeq ($(STATIC_LINKING),1)
$(CORE_TARGET): $(CORE_OBJECTS)
	$(AR) $(ARFLAGS) $@ $^
else
$(CORE_TARGET): $(CORE_OBJECTS) link.T
	$(CC) $(LDFLAGS) $(CORE_SHARED_FLAG) $(PIC_FLAGS) $(CORE_PLATFORM_LDFLAGS) -o $@ $(CORE_OBJECTS) $(LDLIBS) $(CORE_PLATFORM_LDLIBS)
endif

$(RUNTIME_LIBRARY): $(RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

$(BUILD_DIR)/obj/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PIC_FLAGS) -MMD -MP -c $< -o $@

$(LIBRETRO_OBJECTS): $(BUILD_DIR)/obj/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $(PIC_FLAGS) -MMD -MP -c $< -o $@

$(TEST_DIR)/test_rng: tests/unit/runtime/test_rng.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_persistent_room: tests/unit/runtime/test_persistent_room.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_d3_state: tests/unit/runtime/test_d3_state.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_ds_grid: tests/unit/runtime/test_ds_grid.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_bytecode: tests/unit/content/test_bytecode.c \
	src/host/anygm_host.c src/host/anygm_vfs.c \
	src/content/project/gmlc_bytecode.c src/content/project/gmlc_project.c \
	src/content/project/gmlc_json.c src/content/project/gmlc_assets.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_classic: tests/unit/content/test_classic.c \
	src/host/anygm_host.c src/host/anygm_vfs.c src/host/stdio_vfs.c \
	src/content/datafile/gml_win.c src/content/bytecode/gml_bytecode.c \
	src/content/bytecode/gml_bc14.c src/content/bytecode/gml_bc15.c \
	src/content/bytecode/gml_bc17.c \
	src/content/classic/gmlc_classic.c src/content/classic/gmlc_classic_import.c \
	src/content/project/gmlc_project.c src/content/project/gmlc_json.c \
	src/content/project/gmlc_assets.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_compatibility: tests/unit/compatibility/test_compatibility.c \
	src/compatibility/anygm_compatibility.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

ifeq ($(strip $(TEST)),)
check: architecture-check api-check contract-check integration-check security-check $(CHECK_TARGETS)
	$(TEST_DIR)/test_bytecode
	$(TEST_DIR)/test_classic
	$(TEST_DIR)/test_compatibility
	$(TEST_DIR)/test_rng
	$(TEST_DIR)/test_persistent_room
	$(TEST_DIR)/test_d3_state
	$(TEST_DIR)/test_ds_grid
else
FOCUSED_TEST_TARGET := $(TEST_DIR)/test_$(TEST)
check: architecture-check $(FOCUSED_TEST_TARGET)
	$(FOCUSED_TEST_TARGET)
endif

architecture-check:
	tests/architecture/check_compatibility_boundaries.sh
	tests/architecture/check_code_map.sh

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

contract-check: $(CONTRACT_TESTS)
	$(TEST_DIR)/dummy_host
	$(TEST_DIR)/test_libretro_state_transport

security-check: $(SECURITY_TESTS)
	$(TEST_DIR)/test_content_security
	$(TEST_DIR)/test_state_security
	$(TEST_DIR)/test_vfs
	$(TEST_DIR)/test_datafile_security
	$(TEST_DIR)/test_bytecode_security

$(TEST_DIR)/test_content_security: tests/fuzz/test_content_security.c \
	$(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_state_security: tests/fuzz/test_state_security.c \
	tests/support/synthetic_content.c $(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_vfs: tests/unit/host/test_vfs.c src/host/anygm_vfs.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(TEST_DIR)/test_datafile_security: tests/fuzz/test_datafile_security.c \
	tests/support/synthetic_content.c src/host/anygm_host.c src/host/anygm_vfs.c \
	src/host/stdio_vfs.c src/content/datafile/gml_win.c \
	src/content/bytecode/gml_bytecode.c src/content/bytecode/gml_bc14.c \
	src/content/bytecode/gml_bc15.c src/content/bytecode/gml_bc17.c \
	$(ANYGM_PROJECT_SOURCES)
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
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/dummy_host: tests/contract/dummy_host.c tests/support/synthetic_content.c \
	$(RUNTIME_OBJECTS) $(TEST_HOST_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

$(TEST_DIR)/test_libretro_state_transport: tests/contract/libretro_state_transport.c \
	src/adapters/libretro/libretro_entry.c
	mkdir -p $(dir $@)
	$(CC) $(LIBRETRO_CPPFLAGS) $(CFLAGS) $^ -o $@

diagnostic-tests: $(TEST_DIR)/test_load $(TEST_DIR)/test_vm

$(TEST_DIR)/test_load: tests/unit/content/test_load.c \
	src/host/anygm_vfs.c src/host/stdio_vfs.c \
	src/content/datafile/gml_win.c src/content/bytecode/gml_bytecode.c \
	src/content/bytecode/gml_bc14.c src/content/bytecode/gml_bc15.c \
	src/content/bytecode/gml_bc17.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm

$(TEST_DIR)/test_vm: tests/unit/runtime/test_vm.c $(UNIT_RUNTIME_OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ -lm -pthread

clean:
	tests/architecture/safe_clean.sh "$(BUILD_DIR)" \
		$(CORE_BASENAME).so $(CORE_BASENAME).dll \
		$(CORE_BASENAME).dylib $(CORE_BASENAME).a

-include $(RUNTIME_OBJECTS:.o=.d) $(LIBRETRO_OBJECTS:.o=.d)
