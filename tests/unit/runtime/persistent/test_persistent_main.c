/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int main(int argc,char **argv){
  const char *filter=NULL;
  for(int index=1;index<argc;++index){
    if(strcmp(argv[index],"--case")) continue;
    if(index+1>=argc){
      fprintf(stderr,"--case requires a filter\n");
      return EXIT_FAILURE;
    }
    filter=argv[++index];
  }
  static const AnygmTestCase vm_cases[]={
    {"typed_stack",expect_typed_stack_dup},
    {"member_callable_receiver",expect_member_callable_receiver},
    {"builtin_numeric_constants",expect_builtin_numeric_constants},
    {"smooth_path_midpoint_interpolation",expect_smooth_path_midpoint_interpolation},
    {"early_native_layer_animation",expect_early_native_layer_animation},
    {"deactivated_instance_reference",expect_deactivated_instance_reference},
    {"inclusive_instance_bbox_fields",expect_inclusive_instance_bbox_fields},
    {"room_camera_reservation",expect_room_camera_reservation},
    {"revision16_legacy_room_view",expect_revision16_room_uses_legacy_view},
    {"room_order_boundaries",expect_room_order_boundaries},
    {"alarm_dispatch_order",expect_alarm_dispatch_order},
    {"automatic_motion_order",expect_automatic_motion_order},
    {"event_boundary_room_transition",expect_event_boundary_room_transition},
    {"room_transition_animation_phase",expect_room_transition_animation_phase},
    {"frozen_animation_wrap_end",expect_frozen_animation_wrap_fires_animation_end},
    {"stopped_mover_solid_restore",expect_stopped_mover_restored_from_solid},
    {"stationary_embed_nudge_reverted",expect_stationary_embed_nudge_reverted},
    {"classic_timeline_index_activation",expect_classic_timeline_index_activation},
    {"hash_layer_gpu",expect_hash_layer_gpu_gap_closure},
    {"array_functions",expect_array_function_gap_closure},
    {"room_lifecycle",expect_persistent_lifecycle},
  };
  static const AnygmTestCase ds_cases[]={
    {"text_roundtrip",expect_ds_list_text_roundtrip},
    {"priority_mutation",expect_ds_priority_lookup_mutation},
  };
  static const AnygmTestCase audio_cases[]={
    {"group_paths",expect_audio_group_paths},
    {"flagged_external_sound",expect_flagged_external_sound_precedes_embedded_audio_id},
    {"streamed_embedded_fallback",
     expect_streamed_sound_without_sidecar_plays_embedded_compressed_blob},
    {"group_gain",expect_audio_group_gain},
    {"classic_dynamic_sound_lifecycle",expect_classic_dynamic_sound_lifecycle},
    {"dynamic_extension_state",expect_dynamic_audio_extension_state},
    {"saudio_portable_playback",expect_saudio_portable_playback},
    {"faudio_gms_portable_playback",expect_faudio_gms_portable_playback},
    {"wwise_portable_bank_state",expect_wwise_portable_bank_state},
    {"generic_external_audio_restore",expect_generic_external_audio_restore},
    {"state_load_releases_later_sounds",expect_state_load_releases_later_dynamic_sounds},
    {"state_load_ignores_unmatched_records",expect_state_load_ignores_unmatched_dynamic_records},
  };
  static const AnygmTestCase render_cases[]={
    {"background_slot_dimensions",expect_background_slot_dimensions},
    {"classic_view_array_aliases",expect_classic_view_array_aliases},
    {"classic_hollow_rectangle",expect_classic_hollow_rectangle},
    {"legacy_sprite_text_builtin",expect_legacy_sprite_text_builtin},
    {"legacy_sprite_assign_builtin",expect_legacy_sprite_assign_builtin},
    {"runtime_sprite_state_collision_extent",expect_runtime_sprite_state_preserves_collision_extent},
    {"font_primitive_blend",expect_renderer_semantics},
  };
  static const AnygmTestCase io_cases[]={
    {"save_overlay_sandbox",expect_file_sandbox_case},
    {"portable_extensions",expect_portable_extension_io},
  };
  static const AnygmTestCase timeline_cases[]={
    {"native_import_and_step",expect_timeline_case},
  };
  static const AnygmTestCase state_cases[]={
    {"canonical_roundtrip",expect_vm_state_case},
  };
  const AnygmTestGroup groups[]={
    {"vm",vm_cases,sizeof vm_cases/sizeof vm_cases[0]},
    {"ds",ds_cases,sizeof ds_cases/sizeof ds_cases[0]},
    {"io",io_cases,sizeof io_cases/sizeof io_cases[0]},
    {"audio",audio_cases,sizeof audio_cases/sizeof audio_cases[0]},
    {"timeline",timeline_cases,sizeof timeline_cases/sizeof timeline_cases[0]},
    {"state",state_cases,sizeof state_cases/sizeof state_cases[0]},
    {"render",render_cases,sizeof render_cases/sizeof render_cases[0]},
  };
  AnygmTestResult result;
  anygm_test_run_groups(groups,sizeof groups/sizeof groups[0],filter,&result);
  printf("persistent room fixtures: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
