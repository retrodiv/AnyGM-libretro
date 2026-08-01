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
    {"early_native_layer_animation",expect_early_native_layer_animation},
    {"room_camera_reservation",expect_room_camera_reservation},
    {"event_boundary_room_transition",expect_event_boundary_room_transition},
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
    {"group_gain",expect_audio_group_gain},
  };
  static const AnygmTestCase render_cases[]={
    {"font_primitive_blend",expect_renderer_semantics},
  };
  static const AnygmTestCase io_cases[]={
    {"save_overlay_sandbox",expect_file_sandbox_case},
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
