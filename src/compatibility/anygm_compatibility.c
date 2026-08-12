/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm_compatibility.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int compatibility_ascii_equal(const char *left,const char *right){
  if(!left || !right) return 0;
  while(*left && *right){
    if(tolower((unsigned char)*left)!=tolower((unsigned char)*right)) return 0;
    left++; right++;
  }
  return *left==0 && *right==0;
}

AnygmExternalLibraryPolicy anygm_external_library_policy(const char *library){
  if(!library || !*library) return ANYGM_EXTERNAL_LIBRARY_KEEP;
  const char *base=library;
  for(const char *cursor=library;*cursor;cursor++)
    if(*cursor=='/' || *cursor=='\\') base=cursor+1;
  static const char *const portable[]={
    "SGAudio.dll","supersound.dll","saudio.dll","bgm.dll",
    "GMFMODSimple.dll","GMXInput.dll","pxwrap.dll"
  };
  static const char *const noop[]={
    "CleanMem.dll","gmSteam.dll","gmSteamInitOnly.dll","Steam.dll","Steamworks.dll"
  };
  static const char *const dependency[]={
    "wrap_oal.dll","OpenAL32.dll","fmodex.dll","libvorbis.dll",
    "libvorbisfile.dll","libogg.dll","bass.dll","pxtone.dll",
    "steam_api.dll","steamclient.dll","tier0_s.dll","vstdlib_s.dll",
    "cg.dll","cgGL.dll"
  };
  for(size_t i=0;i<sizeof(portable)/sizeof(portable[0]);i++)
    if(compatibility_ascii_equal(base,portable[i])) return ANYGM_EXTERNAL_LIBRARY_PORTABLE;
  for(size_t i=0;i<sizeof(noop)/sizeof(noop[0]);i++)
    if(compatibility_ascii_equal(base,noop[i])) return ANYGM_EXTERNAL_LIBRARY_NOOP;
  for(size_t i=0;i<sizeof(dependency)/sizeof(dependency[0]);i++)
    if(compatibility_ascii_equal(base,dependency[i])) return ANYGM_EXTERNAL_LIBRARY_DEPENDENCY;
  return ANYGM_EXTERNAL_LIBRARY_KEEP;
}

const char *anygm_external_library_policy_name(AnygmExternalLibraryPolicy policy){
  switch(policy){
    case ANYGM_EXTERNAL_LIBRARY_PORTABLE: return "portable";
    case ANYGM_EXTERNAL_LIBRARY_NOOP: return "noop";
    case ANYGM_EXTERNAL_LIBRARY_DEPENDENCY: return "dependency";
    default: return "keep";
  }
}

static void compatibility_error(char *error,size_t error_size,const char *format,...){
  if(!error || !error_size) return;
  va_list arguments;
  va_start(arguments,format);
  vsnprintf(error,error_size,format,arguments);
  va_end(arguments);
}

static int supported_classic_revision(uint32_t revision){
  return revision==530 || revision==600 || revision==701 || revision==702 ||
         revision==800 || revision==810;
}

static uint64_t profile_hash_bytes(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t i=0;i<size;i++){
    hash^=data[i];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static void profile_encode_u32(uint8_t **cursor,uint32_t value){
  for(unsigned i=0;i<4;i++) *(*cursor)++=(uint8_t)(value>>(i*8));
}

static void profile_encode_u64(uint8_t **cursor,uint64_t value){
  for(unsigned i=0;i<8;i++) *(*cursor)++=(uint8_t)(value>>(i*8));
}

static uint64_t compatibility_fingerprint(const AnygmCompatibilityProfile *profile){
  uint8_t encoded[160]={0};
  uint8_t *cursor=encoded;
#define ENCODE_FIELD(field) profile_encode_u32(&cursor,(uint32_t)profile->field)
  ENCODE_FIELD(schema_version);
  ENCODE_FIELD(diagnostic_family);
  ENCODE_FIELD(comparison);
  ENCODE_FIELD(alarm_dispatch);
  ENCODE_FIELD(alarm_threshold);
  ENCODE_FIELD(instance_iteration);
  ENCODE_FIELD(solid_collision_transaction);
  ENCODE_FIELD(blend);
  ENCODE_FIELD(uses_classic_runtime);
  ENCODE_FIELD(has_modern_function_values);
  ENCODE_FIELD(has_modern_struct_semantics);
  ENCODE_FIELD(has_modern_layer_semantics);
  ENCODE_FIELD(has_modern_screen_stage);
  ENCODE_FIELD(uses_room_speed_cadence);
  ENCODE_FIELD(uses_legacy_room_cameras);
  ENCODE_FIELD(advances_animation_before_step);
  ENCODE_FIELD(preserves_frame_without_background_clear);
  ENCODE_FIELD(path_motion_owns_velocity);
  ENCODE_FIELD(round_transformed_collision_bounds);
  ENCODE_FIELD(creation_code_before_create);
  ENCODE_FIELD(classic_presentation);
  ENCODE_FIELD(classic_modern_presentation);
  ENCODE_FIELD(classic_interpolate);
  ENCODE_FIELD(classic_scaling);
  ENCODE_FIELD(classic_executable_layout);
  ENCODE_FIELD(uses_limited_random_seed_expansion);
  ENCODE_FIELD(legacy_view_slots);
#undef ENCODE_FIELD
  uint64_t epsilon=0;
  memcpy(&epsilon,&profile->default_comparison_epsilon,sizeof epsilon);
  profile_encode_u64(&cursor,epsilon);
  return profile_hash_bytes(encoded,(size_t)(cursor-encoded));
}

int anygm_content_facts_detect(const GmlWin *content,AnygmContentFacts *facts,
                               char *error,size_t error_size){
  if(error&&error_size) error[0]=0;
  if(!content || !facts){
    compatibility_error(error,error_size,"content facts require a parsed content image");
    return 0;
  }
  memset(facts,0,sizeof *facts);
  facts->schema_version=ANYGM_COMPATIBILITY_SCHEMA;
  facts->classic_revision=content->classic_version>0?(uint32_t)content->classic_version:0;
  facts->bytecode_revision=content->bytecode;
  facts->option_flags=content->option_flags;
  facts->has_room_layers=content->has_room_layers?1u:0u;
  facts->classic_scaling=content->classic_scaling>0?(uint32_t)content->classic_scaling:0;
  facts->classic_interpolate=content->classic_interpolate?1u:0u;
  facts->classic_swap_creation_events=content->classic_swap_creation_events?1u:0u;
  facts->classic_executable_layout=content->classic_executable_layout?1u:0u;
  if(facts->classic_revision && !supported_classic_revision(facts->classic_revision)){
    compatibility_error(error,error_size,"unsupported classic container revision %u",
                        facts->classic_revision);
    return 0;
  }
  if(facts->bytecode_revision<13 || facts->bytecode_revision>17){
    compatibility_error(error,error_size,"unsupported bytecode revision %u",
                        facts->bytecode_revision);
    return 0;
  }
  return 1;
}

int anygm_compatibility_resolve(const AnygmContentFacts *facts,
                                AnygmCompatibilityProfile *profile,
                                char *error,size_t error_size){
  if(error&&error_size) error[0]=0;
  if(!facts || !profile || facts->schema_version!=ANYGM_COMPATIBILITY_SCHEMA){
    compatibility_error(error,error_size,"unsupported compatibility facts schema");
    return 0;
  }
  memset(profile,0,sizeof *profile);
  profile->schema_version=ANYGM_COMPATIBILITY_SCHEMA;
  int classic=facts->classic_revision!=0;
  int modern=facts->bytecode_revision>=17;
  /* The ROOM layer list is the structural rendering-generation fact. An
   * OPTN capability bit also occurs in packages without this layout. Keep
   * language and value semantics tied to instruction encoding while resolving
   * layer and pixel policies independently from the effective ROOM layout. */
  int second_generation_rendering=!classic &&
    (modern || facts->has_room_layers);
  profile->diagnostic_family=classic?ANYGM_FAMILY_CLASSIC:
    (modern?ANYGM_FAMILY_STUDIO_SECOND:ANYGM_FAMILY_STUDIO_FIRST);
  profile->uses_classic_runtime=classic;
  profile->has_modern_function_values=modern;
  profile->has_modern_struct_semantics=modern;
  profile->has_modern_layer_semantics=second_generation_rendering;
  /* The screen stage stays with the instruction encoding: see the policy's own comment. */
  profile->has_modern_screen_stage=modern;
  profile->uses_room_speed_cadence=!classic&&facts->bytecode_revision==16;
  profile->uses_legacy_room_cameras=!classic&&facts->bytecode_revision==16;
  profile->comparison=classic?ANYGM_COMPARISON_CLASSIC_EPSILON:
    ANYGM_COMPARISON_STUDIO_EPSILON;
  profile->default_comparison_epsilon=classic?1e-13:1e-5;
  profile->alarm_dispatch=(classic||facts->bytecode_revision==16)?
    ANYGM_ALARM_DISPATCH_RESOURCE_MAJOR:ANYGM_ALARM_DISPATCH_STANDARD;
  profile->alarm_threshold=ANYGM_ALARM_TRIGGER_AT_ZERO;
  profile->instance_iteration=(!classic&&modern)?ANYGM_INSTANCE_ITERATION_FRAME_SNAPSHOT:
    ANYGM_INSTANCE_ITERATION_LIVE;
  profile->solid_collision_transaction=(classic||modern)?ANYGM_COLLISION_PREVIOUS_COORDINATES:
    ANYGM_COLLISION_CURRENT_COORDINATES;
  profile->blend=classic?ANYGM_BLEND_CLASSIC:
    (second_generation_rendering?ANYGM_BLEND_STUDIO_SECOND:ANYGM_BLEND_STUDIO_FIRST);
  profile->advances_animation_before_step=classic;
  profile->preserves_frame_without_background_clear=!classic;
  profile->path_motion_owns_velocity=classic||modern;
  profile->round_transformed_collision_bounds=classic || facts->bytecode_revision==16 ||
    (facts->option_flags&UINT64_C(0x08000000));
  profile->creation_code_before_create=classic&&!facts->classic_swap_creation_events;
  profile->classic_presentation=classic;
  profile->classic_modern_presentation=classic&&facts->classic_revision>=800;
  /* Sprite-sampling interpolation. Classic content declares it in its CLSC record;
   * Studio packages use the "interpolate colours between pixels" option bit. */
  profile->classic_interpolate=classic
    ? facts->classic_interpolate
    : (facts->option_flags&UINT64_C(0x2))!=0;
  profile->classic_scaling=classic?facts->classic_scaling:0;
  profile->classic_executable_layout=classic&&facts->classic_executable_layout;
  profile->uses_limited_random_seed_expansion=!classic&&!modern;
  profile->legacy_view_slots=classic?8u:1u;
  profile->fingerprint=compatibility_fingerprint(profile);
  return 1;
}
