/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_ENGINE_INTERNAL_H
#define ANYGM_ENGINE_INTERNAL_H

#include "anygm.h"
#include "anygm_compatibility.h"
#include "gml_audio.h"
#include "gml_render.h"
#include "gml_render_plan.h"
#if ANYGM_HARDWARE_RENDER
#include "gml_gpu.h"
#else
/* Without the hardware backend the engine still carries the pointer, always null, so the
 * lifecycle entry points keep one shape and the removal stays a deletion of bounded modules. */
typedef struct GmlGpu GmlGpu;
#endif
#include "gml_vm.h"
#include "gml_win.h"

/* Aspect-force modes and Draw-GUI routing modes consumed by neutral runtime overrides. */
enum {
  GMC_ASPECT_FORCE_NONE = 0,
  GMC_ASPECT_FORCE_4_3  = 1,
  GMC_ASPECT_FORCE_16_9 = 2,
  GMC_ASPECT_FORCE_21_9 = 3,
  GMC_ASPECT_FORCE_16_10 = 4
};

/* One shape per mode, resolved in one place: the ratio is needed both when the forced frame is
 * measured and when its width is derived, and a mode added to only one of those reads as the
 * shape it is not. */
static inline void gmc_aspect_force_fraction(int mode,unsigned *numerator,unsigned *denominator){
  *numerator=mode==GMC_ASPECT_FORCE_21_9?21u:
             (mode==GMC_ASPECT_FORCE_16_9 || mode==GMC_ASPECT_FORCE_16_10)?16u:4u;
  *denominator=mode==GMC_ASPECT_FORCE_21_9 || mode==GMC_ASPECT_FORCE_16_9?9u:
               mode==GMC_ASPECT_FORCE_16_10?10u:3u;
}
static inline double gmc_aspect_force_ratio(int mode){
  unsigned numerator,denominator;
  gmc_aspect_force_fraction(mode,&numerator,&denominator);
  return (double)numerator/denominator;
}
enum {
  GMC_ASPECT_DRAW_DEFAULT            = 0,
  GMC_ASPECT_DRAW_FULL_VIEW          = 1,
  GMC_ASPECT_DRAW_FULL_VIEW_BACKDROP = 2,
  GMC_ASPECT_DRAW_VISIBLE_VIEW       = 3
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
  double present_ms,frame_ms;
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
  long state_digest_frame;
  int camera_frame;
  int draw_frame;
  int shader_count;
  int present_frame;
  double profile_spike_ms;
  size_t state_profile_last_total;
  long state_overflow_count;
  int state_frame_dropped_reported;
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

typedef enum {
  CK_NONE=0, CK_ROOM, CK_GARR, CK_GSCALAR, CK_INST, CK_INST_SET, CK_ENGINE, CK_ROUTE,
  CK_CAMERA, CK_SURFACE, CK_MONITOR_VIEW, CK_ALARM_PAUSE, CK_SCRIPT, CK_LIST_SET,
  CK_DRAW_HOLD, CK_SURFACE_CANVAS
} CheatKind;
typedef enum {
  TK_LIT=0, TK_BASE_W, TK_BASE_H, TK_FORCED_W, TK_FORCED_H, TK_EXTRA_W, TK_EXTRA_H,
  TK_MONITOR_W, TK_MONITOR_H, TK_MONITOR_VIEW_W, TK_MONITOR_VIEW_H,
  TK_MONITOR_EXTRA_W, TK_MONITOR_EXTRA_H, TK_VIEW_W, TK_VIEW_H, TK_GLOBAL
} CheatTok;
typedef enum { EF_WINDOW_W=0, EF_WINDOW_H, EF_GUI_W, EF_GUI_H, EF_FBW, EF_FBH,
               EF_APPLICATION_W, EF_APPLICATION_H,
               EF_PRESENT_SHIFT_X, EF_PRESENT_SHIFT_Y,
               EF_PRESENT_CROP_LEFT, EF_PRESENT_CROP_TOP,
               EF_PRESENT_CROP_RIGHT, EF_PRESENT_CROP_BOTTOM,
               EF_COMPOSITOR, EF_CENTER_VIEW_TARGET, EF_WIDE_GAMEPLAY_VIEW } EngField;
typedef enum { CF_X=0, CF_Y, CF_WIDTH, CF_HEIGHT } CameraField;
typedef struct { CheatTok tok; double lit; char name[64]; char op[6]; double num[6]; int nop; } CheatVal;
typedef struct {
  CheatKind kind;
  int scope_aspect;
  int scope_monitor;
  int scope_gameres;
  /* A value scope: the directive applies only while this content global is non-zero. Empty for a
   * directive that carries no such condition. */
  char scope_global[64];
  int scope_mode;
  char obj[64];
  char var[64];
  int idx,idx2,has_index;
  EngField eng;
  CameraField camera_field;
  uint64_t camera_mask;
  int route_mode;
  CheatVal val,val2;
  double monitor_height,monitor_min_aspect,monitor_max_aspect;
} CheatAct;
/* saved/saved_valid hold what the target read before this slot was armed, so disarming it can
 * put the value back instead of leaving the last forced write behind. See cheat_slot_capture.
 *
 * One caller-supplied code may carry several `;`-separated directives. Rather than growing every
 * slot to hold N actions — the passes below run per instance per frame and the table is copied
 * through a stack-resident prepared-content struct — a chained code keeps its first directive here
 * and parks the rest in free slots of the same table. continuation_of is 0 for a slot the caller
 * addressed directly and 1 + the owner's index for one of those parked directives, so releasing a
 * chain is a scan and every pass still sees exactly one action per slot. */
typedef struct {
  int enabled; char code[128]; CheatAct act; double saved; double saved2; int saved_valid;
  int applied; int continuation_of;
  /* A draw-phase hold is not a freeze: it writes its value for the length of one frame's drawing
   * and puts back what content left there. These carry that one frame, separately from the
   * arm-time capture above, which a hold never uses. */
  double held; int held_valid;
} CheatSlot;

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
  /* Set when the load took these from the host's locale service; a reset then asks it again. */
  int locale_from_host;
  char content_cache_directory[1024];
  /* Host configuration path, never part of serialized state. */
  char content_system_directory[1024];
  /* The frontend-selected launch path remains the anchor for the whole session. Internal content
   * replacement changes current_content_path, but a state for the original payload still has to
   * be reconstructible from the same launch envelope. */
  char content_launch_path[1024];
  char current_content_path[1024];
  char content_program_directory[512];
  /* Portable path of the active replacement relative to content_program_directory. Empty names
   * the launch payload itself; absolute machine paths never enter a state. */
  char state_content_locator[1024];
  char launch_parameters[GML_GAME_CHANGE_TEXT_MAX];
  int game_change_depth;
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
  uint32_t *fb,*screen,*gui_buffer,*app_crop,*host_screen;
  int content_presented;
  /* Content that composes its own screen through a surface leaves that surface bound when its step
   * events end. The draw phase then belongs to the surface rather than to the world buffer, and the
   * frame presents what the content itself put on the base canvas. The binding itself is what
   * carries this to the next frame, whose step events compose into the presented raster and which
   * therefore has to name that raster before running them. */
  int composed_frame;
  /* Latched for the current load after a frame uses explicit composition.
   * Presentation geometry is resolved before the step phase updates the frame-scoped flag,
   * so the latched value selects the next frame's raster. */
  int classic_compositor;
  uint32_t *classic_phase_mem;
  size_t classic_phase_cap;
  unsigned width,height,base_width,base_height;
  int aspect_force_mode,aspect_force_active;
  double aspect_cam_dx,aspect_cam_dy;
  int aspect_gui_ox,aspect_gui_oy,aspect_draw_full_context;
  double aspect_draw_full_x,aspect_draw_full_y;
  /* Set when the core, not content, owns a first-generation application surface, and the extent
   * that surface had before a widened window was reported, so withdrawing the widening restores
   * exactly the raster that was in place. */
  int first_generation_app_owned;
  int wide_app_restore_width,wide_app_restore_height;
  uint32_t background;
  double fps;
  int fps_room,follow_player,player_object;
  double audio_accumulator;
  int state_just_loaded;
  /* The first advancing frame after a load begins from input continuity: a key held now was held
   * as far as edge detection is concerned, or a load re-presses every held button. */
  int input_continuity_pending;
  int state_frame_available;
  unsigned state_frame_width,state_frame_height;
  /* Set only while a save is being retried without its completed frame, because the buffer the
   * host offered cannot hold one. Never set while a state is measured: the answer a frontend sizes
   * itself from has to describe the whole state. */
  int state_omit_frame;
  uint8_t *state_reapply;
  size_t state_reapply_capacity,state_reapply_size;
  /* Peak serialized size for this content: what the namespace cache remembered at load, the
   * largest save observed since, and the value most recently persisted back. The cache entry is
   * untrusted input: reads validate and clamp before believing it. It exists only when the host
   * supplied a save root and the per-content namespace is in use — without one, win.save_dir
   * falls back to the content directory, and a cache file must never appear beside content. */
  int state_peak_enabled;
  size_t state_peak_hint,state_peak_persisted;
  size_t state_resume_peak_hint,state_resume_peak_persisted;
  int runtime_ended,shutdown_sent;
  ClassicTransition classic_transition;
  int have_presented_frame;
  CoreProfile profile;
  int profile_enabled;
  EngineDiagnostics diagnostics;
  /* One row per pad port. Core overrides and the keyboard bridge use row zero; explicit
   * device queries can reach every row. */
  uint8_t pad_current[ANYGM_MAX_GAMEPADS][NPAD],pad_previous[ANYGM_MAX_GAMEPADS][NPAD];
  uint8_t key_current[NKEY],key_previous[NKEY];
  /* A simulated key pressed and released in one step must retain its press edge.
   * raised records the current frame's edge and carry records only edges
   * cancelled by a release in that frame. A held press uses its existing edge.
   * These transient flags are not serialized between frames. */
  uint8_t key_press_raised[NKEY],key_press_carry[NKEY];
  /* key_press_step classifies a simulated press in the running step as a repeat or
   * new edge. key_release_defer counts polls owed by its paired release so a
   * held-key query in the following frame can observe it. Both are transient. */
  uint8_t key_press_step[NKEY],key_release_defer[NKEY];
  /* io_clear and keyboard_clear suppress a held key until all input sources next report it up. A later press is then observed normally. */
  uint8_t key_cleared[NKEY];
  uint8_t event_key_cleared[ANYGM_KEY_LAST];
  uint8_t hardware_key_current[NKEY],hardware_key_previous[NKEY];
  uint8_t event_vk_current[NKEY],event_vk_previous[NKEY];
  uint8_t event_key_current[ANYGM_KEY_LAST],event_key_previous[ANYGM_KEY_LAST];
  double axis_current[ANYGM_MAX_GAMEPADS][4],axis_previous[ANYGM_MAX_GAMEPADS][4];
  unsigned output_width,output_height;
  /* The game composes into output_width/output_height. Most presentation paths adopt the virtual
   * monitor as that effective window extent; a path which deliberately retains a narrower raster
   * can still wrap its completed frame in the distinct host framebuffer below. */
  unsigned host_output_width,host_output_height;
  int host_canvas_active,host_canvas_x,host_canvas_y,host_canvas_width,host_canvas_height;
  int host_crt_active;
  /* Whole-pixel offset applied to the delivered frame, and the buffer that holds the offset copy
   * so the completed frame the state carries is never moved. Declared by a ?gameres directive and
   * therefore zero unless the logical-raster presentation is selected and an anchor asks for it. */
  int present_shift_x,present_shift_y;
  uint32_t *present_shift_screen;
  size_t present_shift_cap;
  /* Insets removed from the completed composition before host scaling. The compositor and state
   * retain their full raster; only the frame transport and pointer mapping see this source view. */
  int present_crop_left,present_crop_top,present_crop_right,present_crop_bottom;
  /* Geometry the host scratch buffer was last cleared for. The canvas rectangle is rewritten in
   * full every frame, so the margins outside it survive until one of these changes. */
  int host_clear_valid;
  unsigned host_clear_host_width,host_clear_host_height;
  int host_clear_canvas_x,host_clear_canvas_y,host_clear_canvas_width,host_clear_canvas_height;
  int gui_offset_x,gui_offset_y,canvas_mode,gui_space_width,gui_space_height;
  /* Initial logical GUI dimensions, independent of later room changes. */
  int gui_space_boot_width,gui_space_boot_height;
  /* Derived presentation state: content owns the non-native window raster while automatic
   * application-surface drawing is disabled. The screen-stage GUI must use that same raster for
   * both its physical target and its logical coordinates. */
  int screen_stage_window_raster;
  double mouse_pixel_x,mouse_pixel_y;
  int pointer_active;
  /* Track a virtual cursor warp against absolute host pointer readings. */
  int mouse_warped;
  double mouse_host_x,mouse_host_y;
  uint8_t mouse_button_current[3],mouse_button_previous[3];
  /* Held suppression is canonical; cleared edge observations last only this poll. */
  uint8_t mouse_button_cleared,mouse_button_edges_cleared;
  int mouse_wheel;
  int present_mouse_valid,present_mouse_x,present_mouse_y,present_mouse_width,present_mouse_height;
  int present_mouse_source_width,present_mouse_source_height;
  AspectEventViewOverlay aspect_event_view_stack[ASPECT_EVENT_VIEW_STACK_MAX];
  int aspect_event_view_stack_pointer,aspect_event_view_overflow;
  CheatSlot cheats[GML_MAX_CHEATS],boot_cheats[GML_MAX_CHEATS];
  int cheat_count,boot_cheat_count;
  /* Room the override passes last captured against; see cheat_room_scope_refresh. */
  int override_room;
  /* Verbatim override text the loaded content's anchor carried; hashed into the state identity
   * while the content-override channel is active. boot_cheats holds its parsed form. */
  char content_overrides_text[4096];
  char launch_overrides_text[4096];
  MenuState menu;
  uint8_t introskip_set[1024/8];
  int introskip_enabled;
  uint8_t introauto_set[1024/8];      /* rooms an anchor answers without waiting for a button */
  /* A configured virtual-monitor edge is consumed once by ?monitor override directives. The
   * program is external content data; the core retains only this transient scheduling latch. */
  int monitor_override_pending;
  /* Where the canonical pixels of the completed frame currently live. Anything that needs those
   * pixels — serialization, a CPU frame callback, a pixel diagnostic — goes through
   * engine_materialize_completed_frame first rather than reading a buffer that may be stale. */
  uint32_t frame_authority;
  /* The plan the last host presentation was described by, retained so the frame can be rebuilt on
   * the CPU without running game code. Never serialized. */
  GmlRenderPlan host_plan;
  uint32_t host_plan_valid;
  /* Advances once per completed frame. It is what a GPU mirror of the completed frame is keyed on,
   * because the buffer address stays the same while its pixels do not. */
  uint32_t host_frame_generation;
  /* The optional host graphics target. A derived cache with a lifecycle: never serialized, never
   * part of the state configuration fingerprint, and safe to drop and rebuild at any point. */
  GmlGpu *gpu;
  /* How many times the canonical CPU pixels of a completed frame had to be rebuilt because
   * something needed them after a pass produced only the host target. */
  uint32_t frame_materializations;
  /* Which accelerated presentation carried a frame. Two shapes qualify and they remove different
   * work, so a diagnostic that reported only "accelerated" could not say which one a session
   * actually reached. */
  uint32_t screen_pass_frames;
  /* Content programs executed off-screen and read back in the middle of a frame; the time the
   * first passes took, which decides whether this device may keep doing it; and the refusal. */
  uint32_t readback_pass_count;
  uint64_t readback_pass_ns;
  uint32_t readback_pass_measured;
  uint32_t readback_frames_measured;
  uint64_t readback_frame_ns;
  int readback_frame_spent;
  int readback_is_pipelined;
  int readback_refused;
  uint32_t canvas_pass_frames;
};

#define ANYGM_ENGINE_GUARD 0x45474E41u
enum { ENGINE_EMPTY=0,ENGINE_PREPARED=1,ENGINE_LOADED=2 };

/* Narrow core-internal interfaces shared by coordinator owners. */
void engine_logf(AnygmEngine *engine,AnygmLogLevel level,const char *fmt,...);
void classic_transition_reset(AnygmEngine *engine);
void aspect_draw_event_hook(GmlVM *vm,GmlInstance *instance,const char *suffix,
                            int begin,void *user);
void screen_redraw_room_layer_hook(GmlVM *vm,int foreground,void *user);
void screen_refresh_present_latch_hook(GmlVM *vm,void *user);
void sync_room_fps(AnygmEngine *engine,int publish_changes);
void screen_stage_gui_geometry(
  const AnygmEngine *engine,const GmlRenderPresentationMetrics *presentation,
  int window_width,int window_height,int *target_width,int *target_height,
  int *logical_width,int *logical_height);
void aspect_forced_camera(AnygmEngine *engine,double raw_x,double raw_y,
                          double *out_x,double *out_y);
int aspect_compositor_fullwidth_gen(AnygmEngine *engine);
int aspect_surface_canvas_size(AnygmEngine *engine,int *width,int *height);
void aspect_surface_canvas_update(AnygmEngine *engine,double camera_x);
int aspect_center_view_target_gen(AnygmEngine *engine);
int aspect_wide_gameplay_view_gen(AnygmEngine *engine);
int aspect_draw_full_view_gen(AnygmEngine *engine,const GmlInstance *instance,
                              const char *suffix);
int core_opt_god(AnygmEngine *engine);
int core_opt_start_room(AnygmEngine *engine,int *room_index);
int core_opt_redirect_room_order(AnygmEngine *engine);
void engine_override_reset(AnygmEngine *engine);
void engine_override_set(AnygmEngine *engine,unsigned slot,bool enabled,const char *expression);
/* Content-override channel: directives a content anchor declared, parsed into boot slots that
 * survive host cheat resets. Parsing is fail-closed: an unrecognized or one-shot directive
 * rejects the whole block, and the caller fails the load rather than dropping lines. */
int engine_boot_overrides_text(const CheatSlot *slots,int count,char *text,size_t capacity);
int engine_boot_overrides_parse(const char *text,CheatSlot *slots,int *count,
                                char *error,size_t error_capacity);
/* A state carries the values a scoped override forced into it. Loading one re-opens the question
 * of whether those writes still apply, which only the live scope can answer. */
void engine_overrides_room_scope_apply(AnygmEngine *engine);
void engine_overrides_presentation_apply(AnygmEngine *engine);
void engine_overrides_draw_hold_begin(AnygmEngine *engine);
void engine_overrides_draw_hold_end(AnygmEngine *engine);
void engine_overrides_prepare_state_load(AnygmEngine *engine);
void engine_overrides_note_state_load(AnygmEngine *engine);
int engine_boot_cheats_active(AnygmEngine *engine);
void engine_override_menu_refresh(AnygmEngine *engine);
void apply_sticky_cheats(AnygmEngine *engine);
void apply_monitor_overrides(AnygmEngine *engine);
void aspect_apply_program(AnygmEngine *engine);
void menu_run(AnygmEngine *engine);
void room_skip_hook(AnygmEngine *engine);
void introskip_hook(AnygmEngine *engine);
int  engine_overrides_declared_os_type(AnygmEngine *engine);

int ensure_classic_phase(AnygmEngine *engine,size_t pixels);
int ensure_primary_buffers(AnygmEngine *engine);
int ensure_scratch_buffer(AnygmEngine *engine,uint32_t **buffer);
int resolve_host_frame(AnygmEngine *engine,const uint32_t **pixels,
                       unsigned *width,unsigned *height);
/* The extent the host is presented, which is the completed frame's own extent unless a distinct
 * host framebuffer wraps it. */
void engine_host_extent(const AnygmEngine *engine,unsigned *width,unsigned *height);
/* Resolve the valid source rectangle selected from the completed frame. A zero return means the
 * whole frame; invalid or empty inset combinations fail closed to that whole frame as well. */
int engine_present_crop_rect(const AnygmEngine *engine,unsigned source_width,
                             unsigned source_height,unsigned *x,unsigned *y,
                             unsigned *width,unsigned *height);
/* Describe the final host presentation as a neutral plan against the given target class. */
int engine_build_host_plan(AnygmEngine *engine,GmlRenderPlan *plan,uint32_t target,
                           unsigned host_width,unsigned host_height,int include_clear);
/* Where the canonical completed-frame pixels are. A frame presented only through a host graphics
 * target is reconstructible from the retained plan; nothing may read stale pixels instead. */
enum {
  ENGINE_FRAME_CPU_MATERIALIZED=0,
  ENGINE_FRAME_GPU_PRESENTED_CPU_RECONSTRUCTIBLE=1,
  ENGINE_FRAME_NONE=2
};
/* Bring the canonical CPU pixels of the completed frame up to date, by replaying the retained plan
 * through the exact software executor when they are not. Runs no game code, no Draw event, no
 * audio, and consumes nothing from the random sequence. Returns zero only when the frame genuinely
 * cannot be produced, and never leaves a caller reading stale pixels. */
int engine_materialize_completed_frame(AnygmEngine *engine);
/* Put the completed frame on the host graphics target when one has been adopted. Returns zero when
 * there is no target or the pass could not be executed exactly, and the caller then presents the
 * CPU frame exactly as it always has. */
int engine_present_hardware_frame(AnygmEngine *engine,const uint32_t *pixels,
                                  unsigned width,unsigned height);
/* Produce the final host presentation directly on the graphics target, from the completed frame
 * rather than from a host-sized copy of it. Returns zero when there is no target or the pass is not
 * one the device reproduces exactly, and the caller then takes the ordinary software path. */
int engine_present_hardware_canvas(AnygmEngine *engine,unsigned *width,unsigned *height);
/* Execute the frame's last operation on the graphics target instead of writing the completed frame
 * on the processor. Returns zero unless the pass has the narrow structural shape that is
 * reproduced exactly, and the completed frame is then produced the way it always was. */
int engine_present_hardware_screen(AnygmEngine *engine,unsigned *width,unsigned *height);
/* The renderer's mid-frame content-program executor; installed while a graphics context is active. */
int engine_execute_content_shader(void *context,const GmlRenderShaderRequest *request);
/* The read-back budget: whether `passes` read-backs that took `total_ns` between them are too slow
 * for the device to keep running content programs in the middle of a frame. Pure, so the policy
 * can be tested without a device. */
/* The read-back budget is a per-frame one, because what a frame can afford is a frame's time and
 * not a pass's: one pass costing three milliseconds fits, and six costing three each do not. It
 * decides only after enough whole frames to be a measurement rather than a first-frame accident. */
int engine_readback_over_budget(uint64_t frame_total_ns,uint32_t frames);
/* Close the frame the read-back budget is measuring. Called once where a frame begins. */
void engine_readback_open_frame(AnygmEngine *engine);
enum {
  ENGINE_READBACK_CALIBRATION_FRAMES=8,
  ENGINE_READBACK_BUDGET_US_PER_FRAME=8000
};
/* Report the opt-in hardware counters once, as one bounded content-neutral line. */
void engine_graphics_report(AnygmEngine *engine);
/* Whether a host graphics target is adopted right now. The one question core coordination asks
 * about the feature, so it has an answer in a build without the backend as well. */
int engine_graphics_active(const AnygmEngine *engine);
int engine_hybrid_presentation_active(const AnygmEngine *engine);
int engine_content_shaders_active(const AnygmEngine *engine);
int engine_graphics_device_expected(const AnygmEngine *engine);
/* Release everything created in the host graphics context. Objects are deleted only while that
 * context is still current; otherwise the handles are forgotten. */
void engine_graphics_release(AnygmEngine *engine,int context_is_current);
void log_present_pass(AnygmEngine *engine,const char *pass,const uint32_t *pixels,
                      int width,int height);
void classic_transition_release(AnygmEngine *engine);
int classic_transition_start(AnygmEngine *engine,unsigned width,unsigned height,int steps);
void classic_transition_apply(AnygmEngine *engine,uint32_t *frame,unsigned width,unsigned height);
void draw_game_cursor(AnygmEngine *engine,unsigned width,unsigned height);
int core_opt_monitor_size(AnygmEngine *engine,int height);
void clamp_camera_to_current_room(AnygmEngine *engine,double *x,double *y,int center_when_smaller);
int core_opt_fast_alpha_cull(AnygmEngine *engine);
int present_view_get(AnygmEngine *engine,int index,GmlPresentView *out);
int present_view_count(AnygmEngine *engine,GmlPresentView views[8],int *canvas_width,int *canvas_height);
int stale_full_view_port(AnygmEngine *engine,int view_count,int x,int y,int width,int height,
                         int logical_width,int logical_height,int app_width,int app_height);
int application_surface_scales_full_view_port(
  AnygmEngine *engine,int view_count,int x,int y,int width,int height,
  int app_width,int app_height);
int default_application_surface_uses_full_view_port(
  AnygmEngine *engine,int view_count,int x,int y,int width,int height,
  int logical_width,int logical_height,int app_width,int app_height);
int application_surface_matches_first_generation_view_port(
  AnygmEngine *engine,int view_count,int x,int y,int width,int height,
  int app_width,int app_height);
void aspect_hud_rect(AnygmEngine *engine,int *x,int *y,int *width,int *height);
void compute_present(AnygmEngine *engine);
void aspect_view_overlay_begin(AnygmEngine *engine,AspectViewOverlay *overlay,
                               int center_hud,int view_mode);
void aspect_view_overlay_end(AnygmEngine *engine,AspectViewOverlay *overlay,
                             int preserve_camera_writes);
uint32_t cur_room_bg(AnygmEngine *engine);
int room_clears_application_surface(const GmlWin *content, const GmlRoom *room);
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
void engine_input_release_cleared_keys(AnygmEngine *engine);

uint64_t state_hash_bytes(const void *data,size_t size);
void state_identity_refresh(AnygmEngine *engine);
int engine_state_content_locator_valid(const char *locator);
AnygmResult engine_state_stage_content(AnygmEngine *engine,const char *locator,
                                       const char *parameters,AnygmEngine **staged);
void engine_state_commit_staged_content(AnygmEngine *engine,AnygmEngine *staged);
size_t engine_state_frame_capacity(const AnygmEngine *engine);
void engine_state_peak_load(AnygmEngine *engine);
void engine_state_peak_note(AnygmEngine *engine,size_t written);
void engine_state_resume_peak_note(AnygmEngine *engine,size_t written);
void engine_state_peak_flush(AnygmEngine *engine);
bool state_unserialize_impl(AnygmEngine *engine,const void *data,size_t size,
                            int schedule_reapply);
size_t engine_state_size(AnygmEngine *engine);
size_t engine_state_resume_size(AnygmEngine *engine);
bool engine_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written);
bool engine_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,size_t *written);
bool engine_state_load(AnygmEngine *engine,const void *data,size_t size);

#endif
