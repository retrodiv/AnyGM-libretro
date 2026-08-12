/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm_compatibility.h"

#include <stdio.h>
#include <string.h>

static int resolve(uint32_t classic,uint32_t bytecode,uint64_t option_flags,int room_layers,
                   AnygmCompatibilityProfile *profile){
  GmlWin content={0};
  content.classic_version=(int)classic;
  content.bytecode=(uint8_t)bytecode;
  content.option_flags=option_flags;
  content.has_room_layers=room_layers;
  AnygmContentFacts facts={0};
  char error[128]={0};
  return anygm_content_facts_detect(&content,&facts,error,sizeof error) &&
         anygm_compatibility_resolve(&facts,profile,error,sizeof error);
}

int main(void){
  if(anygm_external_library_policy("C:\\game\\SGAudio.DLL")!=
       ANYGM_EXTERNAL_LIBRARY_PORTABLE ||
     anygm_external_library_policy("gmSteam.dll")!=ANYGM_EXTERNAL_LIBRARY_NOOP ||
     anygm_external_library_policy("fmodex.dll")!=ANYGM_EXTERNAL_LIBRARY_DEPENDENCY ||
     anygm_external_library_policy("CustomLighting.dll")!=ANYGM_EXTERNAL_LIBRARY_KEEP ||
     strcmp(anygm_external_library_policy_name(ANYGM_EXTERNAL_LIBRARY_NOOP),"noop")){
    fputs("external library policy mismatch\n",stderr);
    return 1;
  }
  AnygmCompatibilityProfile classic_early={0},classic_late={0};
  AnygmCompatibilityProfile first_early={0},first_late={0},second={0},flagged={0},beta={0};
  AnygmCompatibilityProfile flagged_without_layers={0};
  if(!resolve(600,16,0,0,&classic_early) || !resolve(800,16,0,0,&classic_late) ||
     !resolve(0,14,0,0,&first_early) || !resolve(0,16,0,0,&first_late) ||
     !resolve(0,17,0,0,&second) || !resolve(0,15,UINT64_C(0x08000000),0,&flagged) ||
     !resolve(0,15,UINT64_C(0x00400000),1,&beta) ||
     !resolve(0,15,UINT64_C(0x00400000),0,&flagged_without_layers)){
    fputs("known compatibility facts were rejected\n",stderr);
    return 1;
  }
  if(!classic_early.uses_classic_runtime || classic_early.classic_modern_presentation ||
     !classic_late.classic_modern_presentation ||
     classic_early.alarm_dispatch!=ANYGM_ALARM_DISPATCH_RESOURCE_MAJOR ||
     classic_early.preserves_frame_without_background_clear ||
     classic_late.preserves_frame_without_background_clear ||
     classic_early.blend!=ANYGM_BLEND_CLASSIC){
    fputs("classic policy resolution mismatch\n",stderr);
    return 1;
  }
  if(first_early.diagnostic_family!=ANYGM_FAMILY_STUDIO_FIRST ||
     first_early.comparison!=ANYGM_COMPARISON_STUDIO_EPSILON ||
     first_early.alarm_dispatch!=ANYGM_ALARM_DISPATCH_STANDARD ||
     first_early.alarm_threshold!=ANYGM_ALARM_TRIGGER_AT_ZERO ||
     first_early.instance_iteration!=ANYGM_INSTANCE_ITERATION_LIVE ||
     !first_early.uses_limited_random_seed_expansion ||
     !first_early.preserves_frame_without_background_clear ||
     first_early.solid_collision_transaction!=ANYGM_COLLISION_CURRENT_COORDINATES){
    fputs("first-generation early policy resolution mismatch\n",stderr);
    return 1;
  }
  if(first_late.alarm_dispatch!=ANYGM_ALARM_DISPATCH_RESOURCE_MAJOR ||
     first_late.alarm_threshold!=ANYGM_ALARM_TRIGGER_AT_ZERO ||
     !first_late.round_transformed_collision_bounds ||
     first_late.has_modern_function_values || !first_late.uses_room_speed_cadence ||
     !first_late.uses_limited_random_seed_expansion ||
     !first_late.uses_legacy_room_cameras){
    fputs("first-generation late policy resolution mismatch\n",stderr);
    return 1;
  }
  if(beta.diagnostic_family!=ANYGM_FAMILY_STUDIO_FIRST ||
     beta.has_modern_function_values || beta.has_modern_struct_semantics ||
     !beta.has_modern_layer_semantics ||
     /* Rendering-format generation does not carry the screen stage; this
      * synthetic early profile keeps the screen stage with instruction encoding. */
     beta.has_modern_screen_stage ||
     beta.blend!=ANYGM_BLEND_STUDIO_SECOND || beta.uses_room_speed_cadence ||
     beta.uses_legacy_room_cameras){
    fputs("early second-generation presentation policy mismatch\n",stderr);
    return 1;
  }
  if(flagged_without_layers.has_modern_layer_semantics ||
     flagged_without_layers.blend!=ANYGM_BLEND_STUDIO_FIRST){
    fputs("layerless flagged Studio package selected second-generation rendering\n",stderr);
    return 1;
  }
  if(second.diagnostic_family!=ANYGM_FAMILY_STUDIO_SECOND ||
     !second.has_modern_function_values || !second.has_modern_struct_semantics ||
     !second.has_modern_screen_stage ||
     second.instance_iteration!=ANYGM_INSTANCE_ITERATION_FRAME_SNAPSHOT ||
     second.alarm_dispatch!=ANYGM_ALARM_DISPATCH_STANDARD ||
     second.uses_limited_random_seed_expansion ||
     second.solid_collision_transaction!=ANYGM_COLLISION_PREVIOUS_COORDINATES ||
     !second.preserves_frame_without_background_clear ||
     second.blend!=ANYGM_BLEND_STUDIO_SECOND){
    fputs("second-generation policy resolution mismatch\n",stderr);
    return 1;
  }
  if(!flagged.round_transformed_collision_bounds || !classic_early.fingerprint ||
     classic_early.fingerprint==classic_late.fingerprint ||
     first_early.fingerprint==second.fingerprint){
    fputs("compatibility fingerprint mismatch\n",stderr);
    return 1;
  }
  AnygmCompatibilityProfile repeated={0};
  if(!resolve(0,17,0,0,&repeated) || memcmp(&repeated,&second,sizeof second)){
    fputs("compatibility resolution was not deterministic\n",stderr);
    return 1;
  }
  if(resolve(650,16,0,0,&repeated) || resolve(0,18,0,0,&repeated)){
    fputs("unknown compatibility facts were accepted\n",stderr);
    return 1;
  }
  puts("compatibility profiles: ok");
  return 0;
}
