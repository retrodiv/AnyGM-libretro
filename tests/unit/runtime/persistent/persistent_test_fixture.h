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
int expect_builtin_numeric_constants(void);
int expect_smooth_path_midpoint_interpolation(void);
int expect_early_native_layer_animation(void);
int expect_authored_long_layer_background_binding(void);
int expect_retired_builtin_script_shadow(void);
int expect_deactivated_instance_reference(void);
int expect_bounding_box_far_edges_by_generation(void);
int expect_room_camera_reservation(void);
int expect_revision16_room_uses_legacy_view(void);
int expect_room_order_boundaries(void);
int expect_alarm_dispatch_order(void);
int expect_automatic_motion_order(void);
int expect_event_boundary_room_transition(void);
int expect_room_transition_animation_phase(void);
int expect_frozen_animation_wrap_fires_animation_end(void);
int expect_event_starts_with_the_relative_flag_clear(void);
int expect_stopped_mover_restored_from_solid(void);
int expect_stationary_embed_nudge_reverted(void);
int expect_classic_timeline_index_activation(void);
int expect_frame_clock_ignores_host_time(void);
int expect_sequence_asset_keys_and_parameters(void);
int expect_persistent_lifecycle(void);
int expect_ds_list_text_roundtrip(void);
int expect_ds_priority_lookup_mutation(void);
int expect_audio_group_paths(void);
int expect_flagged_external_sound_precedes_embedded_audio_id(void);
int expect_streamed_sound_without_sidecar_plays_embedded_compressed_blob(void);
int expect_embedded_eight_bit_wave_matches_its_sixteen_bit_signal(void);
int expect_embedded_ms_adpcm_wave_matches_its_sixteen_bit_signal(void);
int expect_classic_sound_drops_its_trailing_frame(void);
int expect_audio_group_gain(void);
int expect_classic_dynamic_sound_lifecycle(void);
int expect_dynamic_audio_extension_state(void);
int expect_saudio_portable_playback(void);
int expect_faudio_gms_portable_playback(void);
int expect_wwise_portable_bank_state(void);
int expect_generic_external_audio_restore(void);
int expect_state_load_releases_later_dynamic_sounds(void);
int expect_state_load_ignores_unmatched_dynamic_records(void);
int expect_file_sandbox(GmlVM *vm,const char *save_dir);
int expect_file_sandbox_case(void);
int expect_portable_extension_io(void);
int expect_native_timeline_import(const char *path);
int expect_timeline_case(void);
int expect_vm_state_case(void);
int expect_background_slot_dimensions(void);
int expect_classic_view_array_aliases(void);
int expect_classic_hollow_rectangle(void);
int expect_legacy_sprite_text_builtin(void);
int expect_centred_real_font_line_starts_on_a_whole_pixel(void);
int expect_carriage_return_and_line_feed_are_one_break(void);
int expect_a_wrapped_line_drops_the_space_it_broke_at(void);
int expect_a_font_kerning_pair_moves_the_pen(void);
int expect_legacy_sprite_assign_builtin(void);
int expect_runtime_sprite_state_preserves_collision_extent(void);
int expect_renderer_semantics(void);

#endif
