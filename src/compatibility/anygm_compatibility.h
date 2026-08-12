/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Structural content facts and the immutable runtime policy resolved from them. Parsers may
 * inspect raw revisions; runtime code consumes named policy fields through this seam.
 */
#ifndef ANYGM_COMPATIBILITY_H
#define ANYGM_COMPATIBILITY_H

#include <stddef.h>
#include <stdint.h>
#include "gml_win.h"

enum { ANYGM_COMPATIBILITY_SCHEMA=1 };

typedef enum AnygmDiagnosticFamily {
  ANYGM_FAMILY_CLASSIC=1,
  ANYGM_FAMILY_STUDIO_FIRST=2,
  ANYGM_FAMILY_STUDIO_SECOND=3
} AnygmDiagnosticFamily;

typedef enum AnygmComparisonPolicy {
  ANYGM_COMPARISON_EXACT=1,
  ANYGM_COMPARISON_CLASSIC_EPSILON=2,
  ANYGM_COMPARISON_STUDIO_EPSILON=3
} AnygmComparisonPolicy;

typedef enum AnygmAlarmDispatchPolicy {
  ANYGM_ALARM_DISPATCH_STANDARD=1,
  ANYGM_ALARM_DISPATCH_RESOURCE_MAJOR=2
} AnygmAlarmDispatchPolicy;

typedef enum AnygmAlarmThresholdPolicy {
  ANYGM_ALARM_TRIGGER_BELOW_ZERO=1,
  ANYGM_ALARM_TRIGGER_AT_ZERO=2
} AnygmAlarmThresholdPolicy;

typedef enum AnygmInstanceIterationPolicy {
  ANYGM_INSTANCE_ITERATION_LIVE=1,
  ANYGM_INSTANCE_ITERATION_FRAME_SNAPSHOT=2
} AnygmInstanceIterationPolicy;

typedef enum AnygmCollisionTransactionPolicy {
  ANYGM_COLLISION_CURRENT_COORDINATES=1,
  ANYGM_COLLISION_PREVIOUS_COORDINATES=2
} AnygmCollisionTransactionPolicy;

typedef enum AnygmBlendPolicy {
  ANYGM_BLEND_CLASSIC=1,
  ANYGM_BLEND_STUDIO_FIRST=2,
  ANYGM_BLEND_STUDIO_SECOND=3
} AnygmBlendPolicy;

/* Some supported content references native extension libraries. AnyGM never loads native code;
 * this policy names the portable replacement contract used by the runtime.
 * PORTABLE libraries have a functional adapter, NOOP libraries intentionally expose an offline
 * API, DEPENDENCY libraries are native implementation details of one of those adapters, and KEEP
 * means that no generic replacement is claimed. */
typedef enum AnygmExternalLibraryPolicy {
  ANYGM_EXTERNAL_LIBRARY_KEEP=0,
  ANYGM_EXTERNAL_LIBRARY_PORTABLE=1,
  ANYGM_EXTERNAL_LIBRARY_NOOP=2,
  ANYGM_EXTERNAL_LIBRARY_DEPENDENCY=3
} AnygmExternalLibraryPolicy;

typedef struct AnygmContentFacts {
  uint32_t schema_version;
  uint32_t classic_revision;
  uint32_t bytecode_revision;
  uint64_t option_flags;
  uint32_t has_room_layers;
  uint32_t classic_scaling;
  uint32_t classic_interpolate;
  uint32_t classic_swap_creation_events;
  uint32_t classic_executable_layout;
} AnygmContentFacts;

typedef struct AnygmCompatibilityProfile {
  uint32_t schema_version;
  AnygmDiagnosticFamily diagnostic_family;
  AnygmComparisonPolicy comparison;
  AnygmAlarmDispatchPolicy alarm_dispatch;
  AnygmAlarmThresholdPolicy alarm_threshold;
  AnygmInstanceIterationPolicy instance_iteration;
  AnygmCollisionTransactionPolicy solid_collision_transaction;
  AnygmBlendPolicy blend;
  uint32_t uses_classic_runtime;
  uint32_t has_modern_function_values;
  uint32_t has_modern_struct_semantics;
  uint32_t has_modern_layer_semantics;
  uint32_t has_modern_screen_stage;
  uint32_t uses_room_speed_cadence;
  uint32_t uses_legacy_room_cameras;
  uint32_t advances_animation_before_step;
  uint32_t preserves_frame_without_background_clear;
  uint32_t path_motion_owns_velocity;
  uint32_t round_transformed_collision_bounds;
  uint32_t creation_code_before_create;
  uint32_t classic_presentation;
  uint32_t classic_modern_presentation;
  uint32_t classic_interpolate;
  uint32_t classic_scaling;
  uint32_t classic_executable_layout;
  uint32_t uses_limited_random_seed_expansion;
  uint32_t legacy_view_slots;
  double default_comparison_epsilon;
  uint64_t fingerprint;
} AnygmCompatibilityProfile;

int anygm_content_facts_detect(const GmlWin *content,AnygmContentFacts *facts,
                               char *error,size_t error_size);
int anygm_compatibility_resolve(const AnygmContentFacts *facts,
                                AnygmCompatibilityProfile *profile,
                                char *error,size_t error_size);
AnygmExternalLibraryPolicy anygm_external_library_policy(const char *library);
const char *anygm_external_library_policy_name(AnygmExternalLibraryPolicy policy);

/* Tests for low-level runtime modules often construct a minimal GmlWin directly. These accessors
 * retain deterministic structural fallbacks for such fixtures; a loaded AnygmEngine always owns
 * and attaches a fully resolved immutable profile. */
static inline const AnygmCompatibilityProfile *anygm_profile(const GmlWin *content){
  return content?content->compatibility:NULL;
}
static inline int anygm_policy_uses_classic_runtime(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->uses_classic_runtime:(content&&content->classic_version>0);
}
static inline int anygm_policy_has_modern_function_values(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->has_modern_function_values:(content&&content->bytecode>=17);
}
static inline int anygm_policy_has_modern_struct_semantics(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->has_modern_struct_semantics:(content&&content->bytecode>=17);
}
static inline int anygm_policy_has_modern_layer_semantics(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->has_modern_layer_semantics:(content&&content->bytecode>=17);
}
/* The presented raster is independent of layers, tilesets and blending.
 * A format with second-generation rendering data can retain a window that
 * describes display size rather than drawing coordinates. Screen-stage policy
 * therefore follows instruction encoding, not rendering-format generation. */
static inline int anygm_policy_has_modern_screen_stage(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->has_modern_screen_stage:
    (content&&!content->classic_version&&content->bytecode>=17);
}
static inline int anygm_policy_uses_room_speed_cadence(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->uses_room_speed_cadence:
    (content&&!content->classic_version&&content->bytecode==16);
}
static inline int anygm_policy_uses_legacy_room_cameras(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->uses_legacy_room_cameras:
    (content&&!content->classic_version&&content->bytecode==16);
}
static inline int anygm_policy_uses_first_generation_studio(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->diagnostic_family==ANYGM_FAMILY_STUDIO_FIRST:
    (content&&!content->classic_version&&content->bytecode>=14&&content->bytecode<17);
}
static inline int anygm_policy_classic_modern_presentation(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->classic_modern_presentation:(content&&content->classic_version>=800);
}
static inline int anygm_policy_round_collision_bounds(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->round_transformed_collision_bounds:
    (content&&(content->classic_version || content->bytecode==16 ||
               (content->option_flags&UINT64_C(0x08000000))));
}
static inline int anygm_policy_snapshot_instance_iteration(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->instance_iteration==ANYGM_INSTANCE_ITERATION_FRAME_SNAPSHOT:
    (content&&!content->classic_version&&content->bytecode>=17);
}
static inline int anygm_policy_alarm_at_zero(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->alarm_threshold==ANYGM_ALARM_TRIGGER_AT_ZERO:
    (content&&(content->classic_version||content->bytecode>=16));
}
static inline int anygm_policy_resource_major_alarm_dispatch(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->alarm_dispatch==ANYGM_ALARM_DISPATCH_RESOURCE_MAJOR:
    (content&&(content->classic_version||content->bytecode==16));
}
static inline double anygm_policy_default_comparison_epsilon(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->default_comparison_epsilon:
    ((content&&content->classic_version)?1e-13:1e-5);
}
static inline int anygm_policy_exact_comparisons(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->comparison==ANYGM_COMPARISON_EXACT:0;
}
static inline int anygm_policy_previous_solid_coordinates(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->solid_collision_transaction==ANYGM_COLLISION_PREVIOUS_COORDINATES:
    (content&&(content->classic_version||content->bytecode>=17));
}
static inline int anygm_policy_path_motion_owns_velocity(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->path_motion_owns_velocity:
    (content&&(content->classic_version||content->bytecode>=17));
}
static inline int anygm_policy_creation_code_before_create(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->creation_code_before_create:
    (content&&content->classic_version&&!content->classic_swap_creation_events);
}
static inline int anygm_policy_animation_before_step(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->advances_animation_before_step:(content&&content->classic_version);
}
/* A room that requests no background clear leaves the completed frame in place on generations
 * with frame retention. Classic generations clear the drawing target every frame; the room flag
 * only decides whether the room color is painted over that cleared target, so half-transparent
 * drawing must not accumulate across frames. */
static inline int anygm_policy_preserves_frame_without_background_clear(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->preserves_frame_without_background_clear:
    (content&&!content->classic_version);
}
/* Early Studio profiles retain a limited high-word seed expansion;
 * later profiles use the full unsigned high word. Classic uses a separate stream. */
static inline int anygm_policy_uses_limited_random_seed_expansion(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?(int)p->uses_limited_random_seed_expansion:
    (content&&!content->classic_version&&content->bytecode>=13&&content->bytecode<17);
}
static inline unsigned anygm_policy_legacy_view_slots(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  return p?p->legacy_view_slots:((content&&content->classic_version)?8u:1u);
}
static inline int anygm_policy_classic_interpolate(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  if(p) return (int)p->classic_interpolate;
  if(!content) return 0;
  return content->classic_version
    ? content->classic_interpolate!=0
    : (content->option_flags&UINT64_C(0x2))!=0;
}
static inline AnygmBlendPolicy anygm_policy_blend(const GmlWin *content){
  const AnygmCompatibilityProfile *p=anygm_profile(content);
  if(p) return p->blend;
  if(content&&content->classic_version) return ANYGM_BLEND_CLASSIC;
  return content&&content->bytecode>=17?ANYGM_BLEND_STUDIO_SECOND:ANYGM_BLEND_STUDIO_FIRST;
}

#endif
