/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_ENGINE_INTERNAL_H
#define ANYGM_ENGINE_INTERNAL_H

#include "anygm.h"
#include "anygm_compatibility.h"
#include "gml_audio.h"
#include "gml_render.h"
#include "gml_vm.h"
#include "gml_win.h"

/* Aspect-force modes and Draw-GUI routing modes consumed by neutral runtime overrides. */
enum {
  GMC_ASPECT_FORCE_NONE = 0,
  GMC_ASPECT_FORCE_4_3  = 1,
  GMC_ASPECT_FORCE_16_9 = 2,
  GMC_ASPECT_FORCE_21_9 = 3
};
enum {
  GMC_ASPECT_DRAW_DEFAULT            = 0,
  GMC_ASPECT_DRAW_FULL_VIEW          = 1,
  GMC_ASPECT_DRAW_FULL_VIEW_BACKDROP = 2
};
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include <inttypes.h>
#include <ctype.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif
#include "gml_render_internal.h"
/* Maximum selectable presentation size. Native room/view rendering remains smaller when appropriate;
 * the larger buffers are used only by the final GUI/compositor pass. */
#define FB_MAX_W 3840
#define FB_MAX_H 2160
#define NPAD 16
#define NKEY 256
#define GML_MAX_CHEATS 64
#define ASPECT_EVENT_VIEW_STACK_MAX 16

typedef struct {
  uint32_t *old_frame;
  size_t capacity;
  unsigned width, height;
  int active, phase, phases;
} ClassicTransition;
typedef struct {
  int frames;
  double total_ms, input_ms, step_ms, clear_ms, draw_ms, gui_ms, video_ms, audio_ms;
  double max_ms; long max_frame;
} CoreProfile;
typedef struct {
  int key_enabled;
  int force_present_view;
  long mouse_frame;
  long cursor_frame;
  long multiview_frame;
  int pad_frame;
  int player_frame;
  int rng_frame;
  int object_frame;
  int camera_frame;
  int draw_frame;
  int shader_count;
  int present_frame;
  double profile_spike_ms;
  size_t state_profile_last_total;
  long state_overflow_count;
  int state_overflow_warned;
  int state_time_prints;
} EngineDiagnostics;
typedef struct {
  int active;
  int room;
  double xview, yview, wview, hview, wport, hport;
} AspectEventViewOverlay;

typedef struct {
  int index, visible, camera;
  double x, y, w, h;
  int px, py, pw, ph;
} GmlPresentView;

typedef struct {
  int active;
  int room;
  double xview, yview, wview, hview, wport, hport;
  double render_x, render_y;
  double map_dx, map_dy;
} AspectViewOverlay;
enum {
  ASPECT_VIEW_NATIVE = 0,
  ASPECT_VIEW_FORCED = 1,
  ASPECT_VIEW_TRACKING = 2
};

typedef enum { CK_NONE=0, CK_ROOM, CK_GARR, CK_GSCALAR, CK_INST, CK_ENGINE, CK_ROUTE } CheatKind;
typedef enum { TK_LIT=0, TK_BASE_W, TK_BASE_H, TK_FORCED_W, TK_FORCED_H, TK_EXTRA_W, TK_EXTRA_H } CheatTok;
typedef enum { EF_WINDOW_W=0, EF_WINDOW_H, EF_GUI_W, EF_GUI_H, EF_FBW, EF_FBH,
               EF_COMPOSITOR, EF_CENTER_VIEW_TARGET, EF_WIDE_GAMEPLAY_VIEW } EngField;
typedef struct { CheatTok tok; double lit; char op[6]; double num[6]; int nop; } CheatVal;
typedef struct {
  CheatKind kind;
  int scope_aspect;
  int scope_mode;
  char obj[64];
  char var[64];
  int idx;
  EngField eng;
  int route_mode;
  CheatVal val;
} CheatAct;
typedef struct { int enabled; char code[128]; CheatAct act; } CheatSlot;

typedef enum { MI_TOGGLE, MI_RANGE, MI_WARP } MenuItemKind;
typedef struct {
  MenuItemKind kind; int from_boot;
  char label[28];
  char tobj[48], tvar[48];
  double onval;
  int vmin, vmax, value, on;
  char roomcsv[128]; int rooms[24], nrooms, resolved, warpsel;
} MenuItem;
typedef struct {
  int active, is2d, lift;
  char obj[48], labelvar[48], idxvar[48];
  MenuItem items[16]; int nitems;
  struct { char var[48]; double val; } tweaks[8]; int ntweaks;
  int open_prev, base;
} MenuState;

struct AnygmEngine {
  uint32_t guard;
  uint32_t lifecycle;
  AnygmHostServices host;
  AnygmConfig config;
  AnygmInputFrame input;
  uint32_t frame_flags;
  char last_error[512];
  char language[16],region[16],language_tag[32];
  int16_t audio_output[4096*2];
  size_t audio_frames;
  GmlWin win;
  GmlVM vm;
  GmlRender render;
  GmlAudio *audio;
  int loaded;
  AnygmContentFacts content_facts;
  AnygmCompatibilityProfile compatibility;
  uint64_t content_fingerprint;
  uint64_t compatibility_fingerprint;
  int full_game_on_initial_boot;
  uint32_t *fb,*screen,*gui_buffer,*app_crop;
  uint32_t *classic_phase_mem;
  size_t classic_phase_cap;
  unsigned width,height,base_width,base_height;
  int aspect_force_mode,aspect_force_active;
  double aspect_cam_dx,aspect_cam_dy;
  int aspect_gui_ox,aspect_gui_oy,aspect_draw_full_context;
  double aspect_draw_full_x,aspect_draw_full_y;
  uint32_t background;
  double fps;
  int fps_room,follow_player,player_object;
  double audio_accumulator;
  char start_room_option[65536];
  int state_just_loaded;
  uint8_t *state_reapply;
  size_t state_reapply_capacity,state_reapply_size;
  int runtime_ended,shutdown_sent;
  ClassicTransition classic_transition;
  int have_presented_frame;
  CoreProfile profile;
  int profile_enabled;
  EngineDiagnostics diagnostics;
  uint8_t pad_current[NPAD],pad_previous[NPAD];
  uint8_t key_current[NKEY],key_previous[NKEY];
  uint8_t hardware_key_current[NKEY],hardware_key_previous[NKEY];
  uint8_t event_vk_current[NKEY],event_vk_previous[NKEY];
  uint8_t event_key_current[ANYGM_KEY_LAST],event_key_previous[ANYGM_KEY_LAST];
  double axis_current[4],axis_previous[4];
  unsigned output_width,output_height;
  int gui_offset_x,gui_offset_y,canvas_mode,gui_space_width,gui_space_height;
  double mouse_pixel_x,mouse_pixel_y;
  int pointer_active;
  uint8_t mouse_button_current[3],mouse_button_previous[3];
  int mouse_wheel;
  int present_mouse_valid,present_mouse_x,present_mouse_y,present_mouse_width,present_mouse_height;
  int present_mouse_source_width,present_mouse_source_height;
  AspectEventViewOverlay aspect_event_view_stack[ASPECT_EVENT_VIEW_STACK_MAX];
  int aspect_event_view_stack_pointer,aspect_event_view_overflow;
  CheatSlot cheats[GML_MAX_CHEATS],boot_cheats[GML_MAX_CHEATS];
  int cheat_count,boot_cheat_count;
  MenuState menu;
  uint8_t introskip_set[1024/8];
  int introskip_enabled;
};

#define ANYGM_ENGINE_GUARD 0x45474E41u
enum { ENGINE_EMPTY=0,ENGINE_LOADED=1 };

/* Narrow core-internal interfaces shared by coordinator owners. */
void engine_logf(AnygmEngine *engine,AnygmLogLevel level,const char *fmt,...);
void classic_transition_reset(AnygmEngine *engine);
void aspect_draw_event_hook(GmlVM *vm,GmlInstance *instance,const char *suffix,
                            int begin,void *user);
void sync_room_fps(AnygmEngine *engine,int publish_changes);
int screen_stage_uses_requested_raster(
  const GmlWin *content,const GmlRenderPresentationMetrics *presentation,
  int logical_width,int logical_height,int target_width,int target_height);
void aspect_forced_camera(AnygmEngine *engine,double raw_x,double raw_y,
                          double *out_x,double *out_y);
int aspect_compositor_fullwidth_gen(AnygmEngine *engine);
int aspect_center_view_target_gen(AnygmEngine *engine);
int aspect_wide_gameplay_view_gen(AnygmEngine *engine);
int aspect_draw_full_view_gen(AnygmEngine *engine,const GmlInstance *instance,
                              const char *suffix);
int core_opt_god(AnygmEngine *engine);
int core_opt_start_room(AnygmEngine *engine,int *room_index);
void engine_override_reset(AnygmEngine *engine);
void engine_override_set(AnygmEngine *engine,unsigned slot,bool enabled,const char *expression);
void apply_sticky_cheats(AnygmEngine *engine);
void aspect_apply_program(AnygmEngine *engine);
void menu_run(AnygmEngine *engine);
void room_skip_hook(AnygmEngine *engine);
void introskip_hook(AnygmEngine *engine);

int ensure_classic_phase(AnygmEngine *engine,size_t pixels);
int ensure_primary_buffers(AnygmEngine *engine);
int ensure_scratch_buffer(AnygmEngine *engine,uint32_t **buffer);
void log_present_pass(AnygmEngine *engine,const char *pass,const uint32_t *pixels,
                      int width,int height);
void classic_transition_release(AnygmEngine *engine);
int classic_transition_start(AnygmEngine *engine,unsigned width,unsigned height,int steps);
void classic_transition_apply(AnygmEngine *engine,uint32_t *frame,unsigned width,unsigned height);
void draw_game_cursor(AnygmEngine *engine,unsigned width,unsigned height);
int core_opt_resolution(AnygmEngine *engine,int height);
int core_opt_embedded_shaders(AnygmEngine *engine);
int core_opt_crt_mask(AnygmEngine *engine);
int core_opt_onoff(AnygmEngine *engine,const char *key,const char *environment,int default_value);
int core_opt_crt_tristate(AnygmEngine *engine,const char *key,const char *environment);
void clamp_camera_to_current_room(AnygmEngine *engine,double *x,double *y,int center_when_smaller);
int core_opt_fast_alpha_cull(AnygmEngine *engine);
int present_view_get(AnygmEngine *engine,int index,GmlPresentView *out);
int present_view_count(AnygmEngine *engine,GmlPresentView views[8],int *canvas_width,int *canvas_height);
int stale_full_view_port(AnygmEngine *engine,int view_count,int x,int y,int width,int height,
                         int logical_width,int logical_height,int app_width,int app_height);
void aspect_hud_rect(AnygmEngine *engine,int *x,int *y,int *width,int *height);
void compute_present(AnygmEngine *engine);
void aspect_view_overlay_begin(AnygmEngine *engine,AspectViewOverlay *overlay,
                               int center_hud,int view_mode);
void aspect_view_overlay_end(AnygmEngine *engine,AspectViewOverlay *overlay,
                             int preserve_camera_writes);
uint32_t cur_room_bg(AnygmEngine *engine);
void draw_runtime_backgrounds(AnygmEngine *engine,int foreground);
void aspect_mask_outside_room(AnygmEngine *engine,double camera_x,double camera_y);
void setup_display(AnygmEngine *engine);
void compose_view_rect(const uint32_t *source,int source_width,int source_height,
                       uint32_t *destination,int destination_width,int destination_height,
                       int x,int y,int width,int height);
int render_multiview_application(AnygmEngine *engine);
void content_router_log(void *userdata,int level,const char *message);

void engine_input_bind(AnygmEngine *engine);
void engine_input_poll_keyboard(AnygmEngine *engine);
void engine_input_poll_mouse(AnygmEngine *engine);

uint64_t state_hash_bytes(const void *data,size_t size);
void state_identity_refresh(AnygmEngine *engine);
bool state_unserialize_impl(AnygmEngine *engine,const void *data,size_t size,
                            int schedule_reapply);
size_t engine_state_size(AnygmEngine *engine);
bool engine_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written);
bool engine_state_load(AnygmEngine *engine,const void *data,size_t size);

#endif
