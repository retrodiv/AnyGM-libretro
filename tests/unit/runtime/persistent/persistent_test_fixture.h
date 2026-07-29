/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_PERSISTENT_TEST_FIXTURE_H
#define ANYGM_PERSISTENT_TEST_FIXTURE_H

#include "gml_vm.h"
#include "anygm_test_runner.h"

#include <stddef.h>
#include <stdint.h>

extern int fixture_joystick_button3;

const char *fixture_development_setting(void *userdata,const char *name);
void fixture_attach_input(GmlVM *vm);
int fixture_write_text(const char *path,const char *text);
int fixture_write_large_ini(const char *path);
int fixture_read_text(const char *path,char *out,size_t cap);
GmlInstance *find_slot(GmlVM *vm,uint32_t id);
uint32_t fixture_u32(const uint8_t *data,size_t offset);
void fixture_w32(uint8_t *data,size_t offset,uint32_t value);
double global_array_value(GmlVM *vm,const char *name,int index);
void fixture_word(unsigned char *data,int index,uint32_t word);

int expect_hash_layer_gpu_gap_closure(void);
int expect_array_function_gap_closure(void);
int expect_typed_stack_dup(void);
int expect_member_callable_receiver(void);
int expect_early_native_layer_animation(void);
int expect_room_camera_reservation(void);
int expect_persistent_lifecycle(void);
int expect_ds_list_text_roundtrip(void);
int expect_ds_priority_lookup_mutation(void);
int expect_audio_group_paths(void);
int expect_audio_group_gain(void);
int expect_file_sandbox(GmlVM *vm,const char *save_dir);
int expect_file_sandbox_case(void);
int expect_native_timeline_import(const char *path);
int expect_timeline_case(void);
int expect_vm_state_case(void);
int expect_renderer_semantics(void);

#endif
