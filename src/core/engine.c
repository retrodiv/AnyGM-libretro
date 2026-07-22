/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* AnyGM engine coordination. Loads supported content, advances the shared runtime implementation,
 * and publishes software video plus interleaved PCM through the framework-neutral public API. */
#include "anygm.h"
#include "gml_win.h"
#include "gml_vm.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "gmlc_package.h"
#include "gmlc_classic_project.h"
#include "gmlc_classic_import.h"
#include "gmlc_project.h"
#include "content_router.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"
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

/* Transitional aliases keep the mechanically imported algorithms intact
 * while making their owner explicit in every helper signature. */
#define g_host (engine->host)
#define g_config (engine->config)
#define g_input (engine->input)
#define g_frame_flags (engine->frame_flags)
#define g_last_error (engine->last_error)
#define g_language (engine->language)
#define g_region (engine->region)
#define g_language_tag (engine->language_tag)
#define g_audio_output (engine->audio_output)
#define g_audio_frames (engine->audio_frames)
#define g_win (engine->win)
#define g_vm (engine->vm)
#define g_render (engine->render)
#define g_audio (engine->audio)
#define g_loaded (engine->loaded)
#define g_content_facts (engine->content_facts)
#define g_compatibility (engine->compatibility)
#define g_content_fingerprint (engine->content_fingerprint)
#define g_compatibility_fingerprint (engine->compatibility_fingerprint)
#define g_full_game_on_initial_boot (engine->full_game_on_initial_boot)
#define g_fb (engine->fb)
#define g_screen (engine->screen)
#define g_guibuf (engine->gui_buffer)
#define g_appcrop (engine->app_crop)
#define g_classic_phase_mem (engine->classic_phase_mem)
#define g_classic_phase_cap (engine->classic_phase_cap)
#define g_w (engine->width)
#define g_h (engine->height)
#define g_base_w (engine->base_width)
#define g_base_h (engine->base_height)
#define g_aspect_force_mode (engine->aspect_force_mode)
#define g_aspect_force_active (engine->aspect_force_active)
#define g_aspect_cam_dx (engine->aspect_cam_dx)
#define g_aspect_cam_dy (engine->aspect_cam_dy)
#define g_aspect_gui_ox (engine->aspect_gui_ox)
#define g_aspect_gui_oy (engine->aspect_gui_oy)
#define g_aspect_draw_full_context (engine->aspect_draw_full_context)
#define g_aspect_draw_full_x (engine->aspect_draw_full_x)
#define g_aspect_draw_full_y (engine->aspect_draw_full_y)
#define g_bg (engine->background)
#define g_fps (engine->fps)
#define g_fps_room (engine->fps_room)
#define g_follow_player (engine->follow_player)
#define g_player_obj (engine->player_object)
#define g_audio_acc (engine->audio_accumulator)
#define g_start_room_opt (engine->start_room_option)
#define g_state_just_loaded (engine->state_just_loaded)
#define g_state_reapply (engine->state_reapply)
#define g_state_reapply_cap (engine->state_reapply_capacity)
#define g_state_reapply_size (engine->state_reapply_size)
#define g_runtime_ended (engine->runtime_ended)
#define g_shutdown_sent (engine->shutdown_sent)
#define g_classic_transition (engine->classic_transition)
#define g_have_presented_frame (engine->have_presented_frame)
#define g_prof (engine->profile)
#define g_profile_enabled (engine->profile_enabled)
#define g_diag (engine->diagnostics)
#define g_pad_cur (engine->pad_current)
#define g_pad_prev (engine->pad_previous)
#define g_key_cur (engine->key_current)
#define g_key_prev (engine->key_previous)
#define g_hw_key_cur (engine->hardware_key_current)
#define g_hw_key_prev (engine->hardware_key_previous)
#define g_evt_vk_cur (engine->event_vk_current)
#define g_evt_vk_prev (engine->event_vk_previous)
#define g_evt_key_cur (engine->event_key_current)
#define g_evt_key_prev (engine->event_key_previous)
#define g_axis_cur (engine->axis_current)
#define g_axis_prev (engine->axis_previous)
#define g_out_w (engine->output_width)
#define g_out_h (engine->output_height)
#define g_gui_ox (engine->gui_offset_x)
#define g_gui_oy (engine->gui_offset_y)
#define g_canvas_mode (engine->canvas_mode)
#define g_gui_sw (engine->gui_space_width)
#define g_gui_sh (engine->gui_space_height)
#define g_mouse_px (engine->mouse_pixel_x)
#define g_mouse_py (engine->mouse_pixel_y)
#define g_pointer_active (engine->pointer_active)
#define g_mb_cur (engine->mouse_button_current)
#define g_mb_prev (engine->mouse_button_previous)
#define g_mouse_wheel (engine->mouse_wheel)
#define g_present_mouse_valid (engine->present_mouse_valid)
#define g_present_mouse_x (engine->present_mouse_x)
#define g_present_mouse_y (engine->present_mouse_y)
#define g_present_mouse_w (engine->present_mouse_width)
#define g_present_mouse_h (engine->present_mouse_height)
#define g_present_mouse_src_w (engine->present_mouse_source_width)
#define g_present_mouse_src_h (engine->present_mouse_source_height)
#define g_aspect_event_view_stack (engine->aspect_event_view_stack)
#define g_aspect_event_view_sp (engine->aspect_event_view_stack_pointer)
#define g_aspect_event_view_overflow (engine->aspect_event_view_overflow)
#define g_cheats (engine->cheats)
#define g_cheat_n (engine->cheat_count)
#define g_boot_cheats (engine->boot_cheats)
#define g_boot_cheat_n (engine->boot_cheat_count)
#define g_menu (engine->menu)
#define g_introskip_set (engine->introskip_set)
#define g_introskip_on (engine->introskip_enabled)

static void engine_logf(AnygmEngine *engine,AnygmLogLevel level,const char *fmt,...){
  char message[2048];
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(message,sizeof message,fmt,ap);
  va_end(ap);
  if(g_host.log) g_host.log(g_host.userdata,level,message);
}

static void engine_errorf(AnygmEngine *engine,AnygmResult result,const char *fmt,...){
  (void)result;
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(g_last_error,sizeof g_last_error,fmt,ap);
  va_end(ap);
  engine_logf(engine,ANYGM_LOG_ERROR,"%s",g_last_error);
}

/* Start-room values must not leak across content loads because the same numeric value can identify
 * unrelated rooms. The first boot follows the normal entry point; selecting a debug room and
 * choosing Restart still boots that room. */
static int ensure_classic_phase(AnygmEngine *engine,size_t pixels){
  if(pixels==0 || pixels>SIZE_MAX/sizeof(uint32_t)) return 0;
  if(g_classic_phase_cap>=pixels && g_classic_phase_mem) return 1;
  uint32_t *next=realloc(g_classic_phase_mem,pixels*sizeof(uint32_t));
  if(!next) return 0;
  g_classic_phase_mem=next;
  g_classic_phase_cap=pixels;
  return 1;
}
static int ensure_primary_buffers(AnygmEngine *engine){
  const size_t pixels=(size_t)FB_MAX_W*(size_t)FB_MAX_H;
  if(!g_fb){
    g_fb=calloc(pixels,sizeof(*g_fb));
    if(!g_fb) return 0;
  }
  if(!g_screen){
    g_screen=calloc(pixels,sizeof(*g_screen));
    if(!g_screen){ free(g_fb); g_fb=NULL; return 0; }
  }
  return 1;
}
static int ensure_scratch_buffer(AnygmEngine *engine,uint32_t **buffer){
  if(*buffer) return 1;
  *buffer=calloc((size_t)FB_MAX_W*(size_t)FB_MAX_H,sizeof(**buffer));
  return *buffer!=NULL;
}
/* Draw events can mutate runtime state, including consuming RNG. A load-state frame is
 * rendered without a Step so rewind can display it, then the exact state is reapplied after
 * presentation to discard those render-only side effects before simulation resumes. */
static bool state_unserialize_impl(AnygmEngine *engine,const void *data, size_t size, int schedule_reapply);
static AnygmResult engine_run_frame(AnygmEngine *engine);
static void state_identity_refresh(AnygmEngine *engine);
static uint64_t state_current_config_fingerprint(AnygmEngine *engine);
static uint64_t state_hash_bytes(const void *data,size_t size);
/* Preserve the completed frame after a shutdown request instead of advancing a runtime which has
 * already fired its final event. These flags belong to the loaded runtime, not to the process. */

/* Optional presentation-pass fingerprint for diagnosing data-driven compositors. It samples the
 * whole software target only when explicitly enabled, so normal emulation pays no extra cost. */
static void log_present_pass(AnygmEngine *engine,const char *pass, const uint32_t *px, int w, int h){
  if(!anygm_host_development_setting(&g_host,"GML_LOG_PRESENT_PASSES") || !px || w<=0 || h<=0) return;
  const char *at=anygm_host_development_setting(&g_host,"GML_LOG_PRESENT_FRAME");
  if(at && *at){ if(g_vm.frame!=atol(at)) return; }
  else if(g_vm.frame>8) return;
  size_t n=(size_t)w*(size_t)h, black=0, white=0, other=0;
  uint64_t hash=1469598103934665603ULL;
  for(size_t i=0;i<n;i++){
    uint32_t c=px[i]&0x00FFFFFFu;
    if(!c) black++; else if(c==0x00FFFFFFu) white++; else other++;
    hash^=(uint64_t)c; hash*=1099511628211ULL;
  }
  engine_logf(engine,ANYGM_LOG_DEBUG,"[presentpass] f%ld %-12s %dx%d black=%" PRIu64 " white=%" PRIu64 " other=%" PRIu64 " "
                 "center=%06x hash=%016llx shader=%d pending(fill=%d underlay=%d)\n",
          g_vm.frame,pass,w,h,(uint64_t)black,(uint64_t)white,(uint64_t)other,
          px[(size_t)(h/2)*w+w/2]&0x00FFFFFFu,(unsigned long long)hash,
          g_render.active_shader,g_render.pending_fill,g_render.pending_underlay);
}

static void classic_transition_reset(AnygmEngine *engine){
  g_classic_transition.active=0;
  g_classic_transition.phase=0;
  g_classic_transition.phases=0;
  g_classic_transition.width=0;
  g_classic_transition.height=0;
}
static void classic_transition_release(AnygmEngine *engine){
  free(g_classic_transition.old_frame);
  memset(&g_classic_transition,0,sizeof(g_classic_transition));
}
static int classic_transition_start(AnygmEngine *engine,unsigned width,unsigned height,int steps){
  if(!g_have_presented_frame || !width || !height || width>FB_MAX_W || height>FB_MAX_H) return 0;
  size_t pixels=(size_t)width*height;
  if(pixels>SIZE_MAX/sizeof(uint32_t)) return 0;
  if(g_classic_transition.capacity<pixels){
    uint32_t *next=realloc(g_classic_transition.old_frame,pixels*sizeof(uint32_t));
    if(!next) return 0;
    g_classic_transition.old_frame=next;
    g_classic_transition.capacity=pixels;
  }
  memcpy(g_classic_transition.old_frame,g_screen,pixels*sizeof(uint32_t));
  /* Classic transition_steps are consumed in a tight presentation loop rather than as
   * room steps. Keep the brief duration at the room rate, but always use an odd picture count:
   * kind 21 reaches a fully black midpoint before revealing the new room. An even count only
   * darkens each side and incorrectly skips the defining black picture. */
  if(steps<1) steps=1;
  int phases=(steps+19)/20;
  if(phases<1) phases=1;
  if(phases>119) phases=119;
  if(!(phases&1)) phases++;
  if(anygm_host_development_setting(&g_host,"GML_LOG_TRANSITION"))
    engine_logf(engine,ANYGM_LOG_DEBUG,"[transition] kind=21 steps=%d pictures=%d size=%ux%u\n",
            steps,phases,width,height);
  g_classic_transition.width=width;
  g_classic_transition.height=height;
  g_classic_transition.phase=0;
  g_classic_transition.phases=phases;
  g_classic_transition.active=1;
  return 1;
}
static unsigned classic_transition_scale(unsigned channel,unsigned numerator,unsigned denominator){
  return (channel*numerator+denominator/2)/denominator;
}
static void classic_transition_apply(AnygmEngine *engine,uint32_t *frame,unsigned width,unsigned height){
  ClassicTransition *t=&g_classic_transition;
  if(!t->active) return;
  if(width!=t->width || height!=t->height || !t->old_frame){
    classic_transition_reset(engine);
    gml_set_global_scalar(&g_vm,"transition_kind",0);
    return;
  }
  unsigned den=(unsigned)t->phases;
  unsigned scale_den=den;
  unsigned twice=(unsigned)(2*t->phase+1);
  const uint32_t *source;
  unsigned numerator;
  if(twice<=den){
    source=t->old_frame;
    numerator=den-twice;
  } else {
    source=frame;
    numerator=twice-den;
  }
  /* The tight transition loop presents its edge pictures with 8-bit blend factors:
   * 230/255 on the old side and 229/255 on the new side. Using decimal 90% changes half-rounded
   * palette entries by one. Keep the black midpoint and inner samples unchanged. */
  if(t->phases>1 && t->phase==0){
    numerator=230;
    scale_den=255;
  } else if(t->phases>1 && t->phase==t->phases-1){
    numerator=229;
    scale_den=255;
  }
  size_t pixels=(size_t)width*height;
  for(size_t i=0;i<pixels;i++){
    uint32_t v=source[i];
    unsigned r=classic_transition_scale((v>>16)&255u,numerator,scale_den);
    unsigned g=classic_transition_scale((v>>8)&255u,numerator,scale_den);
    unsigned b=classic_transition_scale(v&255u,numerator,scale_den);
    frame[i]=(r<<16)|(g<<8)|b;
  }
  if(++t->phase>=t->phases){
    classic_transition_reset(engine);
    gml_set_global_scalar(&g_vm,"transition_kind",0);
  }
}
static void aspect_forced_camera(AnygmEngine *engine,double raw_x, double raw_y, double *out_x, double *out_y);

static int profile_enabled(AnygmEngine *engine){
  if(g_profile_enabled < 0) g_profile_enabled = anygm_host_development_setting(&g_host,"GML_PROFILE") != NULL;
  return g_profile_enabled;
}

static int substr_list_match(const char *list, const char *name){
  if(!list || !*list || !name) return 0;
  const char *p=list;
  while(*p){
    while(*p==' ' || *p=='\t' || *p==',' || *p==';') p++;
    const char *s=p;
    while(*p && *p!=',' && *p!=';') p++;
    const char *e=p;
    while(e>s && (e[-1]==' ' || e[-1]=='\t')) e--;
    if(e>s){
      char tok[128];
      size_t n=(size_t)(e-s);
      if(n>=sizeof tok) n=sizeof tok-1;
      memcpy(tok,s,n); tok[n]=0;
      if(strstr(name,tok)) return 1;
    }
  }
  return 0;
}
static double profile_now_ms(AnygmEngine *engine){
  return (double)anygm_host_monotonic_time_ns(engine?&engine->host:NULL)/1000000.0;
}
static void profile_report(AnygmEngine *engine,int force){
  if(!g_prof.frames || (!force && g_prof.frames < 300)) return;
  double f = (double)g_prof.frames;
  const char *fmt = "[profile] frames=%d avg_ms total=%.3f input=%.3f step=%.3f clear=%.3f draw=%.3f gui=%.3f video=%.3f audio=%.3f max=%.2fms@f%ld\n";
  engine_logf(engine,ANYGM_LOG_INFO,fmt,g_prof.frames,g_prof.total_ms/f,g_prof.input_ms/f,
              g_prof.step_ms/f,g_prof.clear_ms/f,g_prof.draw_ms/f,g_prof.gui_ms/f,
              g_prof.video_ms/f,g_prof.audio_ms/f,g_prof.max_ms,g_prof.max_frame);
  memset(&g_prof, 0, sizeof(g_prof));
}

static int  core_opt_mouse_mode(AnygmEngine *engine);   /* 0 auto / 1 absolute / 2 relative (defined below) */
static void apply_sticky_cheats(AnygmEngine *engine);
static void aspect_apply_program(AnygmEngine *engine);
static int  aspect_compositor_fullwidth_gen(AnygmEngine *engine);
static int  aspect_center_view_target_gen(AnygmEngine *engine);
static int  aspect_wide_gameplay_view_gen(AnygmEngine *engine);
static int  aspect_draw_full_view_gen(AnygmEngine *engine,const GmlInstance *in, const char *suffix);
static void menu_run(AnygmEngine *engine);
static void room_skip_hook(AnygmEngine *engine);
static void introskip_hook(AnygmEngine *engine);
static void run_selftest(AnygmEngine *engine);
/* Generic play-order room list for the "Start room" dropdown: "idx:name|idx:name|...". */
static char *build_room_list(const GmlWin *win){
  if(!win || win->n_room_order<=0) return NULL;
  int n=0;
  for(int i=0;i<win->n_room_order;i++){
    GmlRoom r; int ri=(int)win->room_order[i];
    const char *nm=(gml_room_get(win,ri,&r)==0 && r.name)? r.name : "?";
    n += 8 + (int)strlen(nm);
  }
  char *buf=malloc((size_t)n+1), *p=buf; if(!buf) return NULL;
  for(int i=0;i<win->n_room_order;i++){
    GmlRoom r; int ri=(int)win->room_order[i];
    const char *nm=(gml_room_get(win,ri,&r)==0 && r.name)? r.name : "?";
    p += snprintf(p, (size_t)(n-(p-buf))+1, "%d:%s%s", ri, nm, i+1<win->n_room_order?"|":"");
  }
  return buf;
}

/* ---- normalized input snapshot (held this frame plus prior-frame edges) ---- */
static int anygm_key_for_vk(int vk);
static int event_key_state_for_vk(AnygmEngine *engine,int vk, int prev);
static int dbg_key_enabled(AnygmEngine *engine){
  if(g_diag.key_enabled < 0) g_diag.key_enabled = anygm_host_development_setting(&g_host,"GML_DBG_KEY") != NULL;
  return g_diag.key_enabled;
}
static void dbg_key_log(AnygmEngine *engine,int vk, int edge, int cur, int prev, int out){
  if(!out || !dbg_key_enabled(engine)) return;
  engine_logf(engine,ANYGM_LOG_DEBUG,"[key] f%ld vk=%d edge=%d cur=%d prev=%d -> %d\n",g_vm.frame,vk,edge,cur,prev,out);
}
/* map a GM virtual-key code to a RetroPad button id (-1 = unmapped) */
static int vk_to_pad(AnygmEngine *engine,int vk){
  if(anygm_policy_uses_classic_runtime(&g_win)){
    switch(vk){
      case 37: return ANYGM_PAD_LEFT;
      case 39: return ANYGM_PAD_RIGHT;
      case 38: return ANYGM_PAD_UP;
      case 40: return ANYGM_PAD_DOWN;
      case 90: return ANYGM_PAD_FACE_BOTTOM;       /* Z */
      case 88: return ANYGM_PAD_FACE_RIGHT;       /* X */
      case 67: return ANYGM_PAD_FACE_LEFT;       /* C */
      case 86: return ANYGM_PAD_FACE_TOP;       /* V */
      case 13: case 77: return ANYGM_PAD_START; /* Enter / M */
      case 32: return ANYGM_PAD_SELECT;  /* Space */
      case 65: return ANYGM_PAD_LEFT_SHOULDER;       /* A */
      case 83: return ANYGM_PAD_RIGHT_SHOULDER;       /* S */
      default: return -1;
    }
  }
  switch(vk){
    case 37: return ANYGM_PAD_LEFT;   /* vk_left  */
    case 39: return ANYGM_PAD_RIGHT;  /* vk_right */
    case 38: return ANYGM_PAD_UP;     /* vk_up    */
    case 40: return ANYGM_PAD_DOWN;   /* vk_down  */
    case 90: case 32: return ANYGM_PAD_FACE_BOTTOM;   /* Z / space = button1: attack/confirm */
    case 88: return ANYGM_PAD_FACE_LEFT;            /* X = button2: jump                 */
    case 67: return ANYGM_PAD_FACE_RIGHT;            /* C keyboard alias on RetroPad A    */
    case 13: return ANYGM_PAD_START;        /* enter = start      */
    case 77: return ANYGM_PAD_START;        /* M (key_start arcade)*/
    case 16: return ANYGM_PAD_SELECT;       /* shift = coin/select */
    default: return -1;
  }
}
static int engine_input_key(void *userdata,int vk, int edge){
  AnygmEngine *engine=userdata;
  if(vk == 1 || vk == 0){   /* vk_anykey (1) / vk_nokey (0): aggregate over every input */
    int any = 0, anyp = 0;
    for(int i = 0; i < NPAD; i++){ any |= g_pad_cur[i]; anyp |= g_pad_prev[i]; }
    for(int i = 2; i < NKEY; i++){
      any |= g_key_cur[i] | g_hw_key_cur[i] | g_evt_vk_cur[i];
      anyp |= g_key_prev[i] | g_hw_key_prev[i] | g_evt_vk_prev[i];
    }  /* skip 0/1 (the sentinels) */
    for(int i = 1; i < ANYGM_KEY_LAST; i++){
      any |= g_evt_key_cur[i];
      anyp |= g_evt_key_prev[i];
    }
    int cur  = (vk == 1) ? any  : !any;    /* anykey = something down; nokey = nothing down */
    int prev = (vk == 1) ? anyp : !anyp;
    int out = edge==1 ? (cur && !prev) : edge==2 ? (!cur && prev) : cur;
    dbg_key_log(engine,vk, edge, cur, prev, out);
    return out;
  }
  int cur = 0, prev = 0;
  if(vk >= 0 && vk < NKEY){
    cur |= g_key_cur[vk] | g_hw_key_cur[vk] | g_evt_vk_cur[vk];
    prev |= g_key_prev[vk] | g_hw_key_prev[vk] | g_evt_vk_prev[vk];
  }
  cur |= event_key_state_for_vk(engine,vk, 0);
  prev |= event_key_state_for_vk(engine,vk, 1);
  int b = vk_to_pad(engine,vk);
  if(b >= 0){ cur |= g_pad_cur[b]; prev |= g_pad_prev[b]; }
  int out = edge==1 ? (cur && !prev) : edge==2 ? (!cur && prev) : cur;
  dbg_key_log(engine,vk, edge, cur, prev, out);
  return out;
}
static void engine_input_key_clear(void *userdata,int vk){
  AnygmEngine *engine=userdata;
  if(vk >= 0 && vk < NKEY){ g_key_cur[vk]=0; g_key_prev[vk]=0; }
}
static void engine_input_key_press(void *userdata,int vk){
  AnygmEngine *engine=userdata;
  if(vk >= 0 && vk < NKEY){ g_key_cur[vk]=1; g_key_prev[vk]=0; }
}
static void engine_input_key_release(void *userdata,int vk){
  AnygmEngine *engine=userdata;
  if(vk >= 0 && vk < NKEY){ g_key_cur[vk]=0; g_key_prev[vk]=1; }
}
static int anygm_key_for_vk(int vk){
  if(vk >= 'A' && vk <= 'Z') return ANYGM_KEY_a + (vk - 'A');
  if(vk >= '0' && vk <= '9') return vk;
  switch(vk){
    case 8: return ANYGM_KEY_BACKSPACE;
    case 9: return ANYGM_KEY_TAB;
    case 13: return ANYGM_KEY_RETURN;
    case 16: return ANYGM_KEY_LSHIFT;   /* handled specially with RSHIFT too */
    case 17: return ANYGM_KEY_LCTRL;    /* handled specially with RCTRL too */
    case 18: return ANYGM_KEY_LALT;     /* handled specially with RALT too */
    case 27: return ANYGM_KEY_ESCAPE;
    case 32: return ANYGM_KEY_SPACE;
    case 33: return ANYGM_KEY_PAGEUP;
    case 34: return ANYGM_KEY_PAGEDOWN;
    case 35: return ANYGM_KEY_END;
    case 36: return ANYGM_KEY_HOME;
    case 37: return ANYGM_KEY_LEFT;
    case 38: return ANYGM_KEY_UP;
    case 39: return ANYGM_KEY_RIGHT;
    case 40: return ANYGM_KEY_DOWN;
    case 45: return ANYGM_KEY_INSERT;
    case 46: return ANYGM_KEY_DELETE;
    default:
      if(vk >= 112 && vk <= 123) return ANYGM_KEY_F1 + (vk - 112);
      if(vk >= 32 && vk <= 126) return vk;
      return -1;
  }
}
static int event_key_state_for_vk(AnygmEngine *engine,int vk, int prev){
  const uint8_t *keys = prev ? g_evt_key_prev : g_evt_key_cur;
  int rk=anygm_key_for_vk(vk);
  int down = 0;
  if(rk >= 0 && rk < ANYGM_KEY_LAST) down |= keys[rk];
  if(vk == 16) down |= keys[ANYGM_KEY_RSHIFT] | keys[ANYGM_KEY_LSHIFT];
  else if(vk == 17) down |= keys[ANYGM_KEY_RCTRL] | keys[ANYGM_KEY_LCTRL];
  else if(vk == 18) down |= keys[ANYGM_KEY_RALT] | keys[ANYGM_KEY_LALT];
  return down;
}
static void poll_keyboard(AnygmEngine *engine){
  memcpy(g_hw_key_prev, g_hw_key_cur, sizeof(g_hw_key_cur));
  memcpy(g_evt_key_prev,g_evt_key_cur,sizeof g_evt_key_cur);
  memset(g_hw_key_cur, 0, sizeof(g_hw_key_cur));
  memcpy(g_evt_key_cur,g_input.keys,sizeof g_evt_key_cur);
  memset(g_evt_vk_cur,0,sizeof g_evt_vk_cur);
  static const unsigned char vk_poll[] = {
    8,9,13,16,17,18,27,32,33,34,35,36,37,38,39,40,45,46,
    '0','1','2','3','4','5','6','7','8','9',
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    112,113,114,115,116,117,118,119,120,121,122,123
  };
  for(size_t i=0; i<sizeof(vk_poll)/sizeof(vk_poll[0]); i++){
    int vk = vk_poll[i];
    int rk=anygm_key_for_vk(vk);
    if(rk < 0) continue;
    int down = g_input.keys[rk] != 0;
    if(vk == 16) down |= g_input.keys[ANYGM_KEY_RSHIFT] != 0;
    else if(vk == 17) down |= g_input.keys[ANYGM_KEY_RCTRL] != 0;
    else if(vk == 18) down |= g_input.keys[ANYGM_KEY_RALT] != 0;
    g_hw_key_cur[vk] = down ? 1 : 0;
  }
}
/* GM gamepad button constant (gp_face1=32769 …) -> RetroPad button id (-1 = unmapped) */
static int gp_to_pad(AnygmEngine *engine,int gp){
  /* GMS2 bytecode stores the button enum as 1..16, while older formats also accept the 0..15
   * indexes returned by button-count loops. Keep both layouts tied to the package generation. */
  if(anygm_policy_has_modern_function_values(&g_win)) {
    if(gp >= 1 && gp <= 16) gp += 32768;
  } else if(gp >= 0 && gp < 16) gp += 32769;
  switch(gp){
    case 32769: return ANYGM_PAD_FACE_BOTTOM;       /* gp_face1 */
    case 32770: return ANYGM_PAD_FACE_RIGHT;       /* gp_face2 */
    case 32771: return ANYGM_PAD_FACE_LEFT;       /* gp_face3 */
    case 32772: return ANYGM_PAD_FACE_TOP;       /* gp_face4 */
    case 32773: return ANYGM_PAD_LEFT_SHOULDER;       /* gp_shoulderl */
    case 32774: return ANYGM_PAD_RIGHT_SHOULDER;       /* gp_shoulderr */
    case 32775: return ANYGM_PAD_LEFT_TRIGGER;      /* gp_shoulderlb */
    case 32776: return ANYGM_PAD_RIGHT_TRIGGER;      /* gp_shoulderrb */
    case 32777: return ANYGM_PAD_SELECT;  /* gp_select */
    case 32778: return ANYGM_PAD_START;   /* gp_start */
    case 32779: return ANYGM_PAD_LEFT_STICK;      /* gp_stickl */
    case 32780: return ANYGM_PAD_RIGHT_STICK;      /* gp_stickr */
    case 32781: return ANYGM_PAD_UP;      /* gp_padu */
    case 32782: return ANYGM_PAD_DOWN;    /* gp_padd */
    case 32783: return ANYGM_PAD_LEFT;    /* gp_padl */
    case 32784: return ANYGM_PAD_RIGHT;   /* gp_padr */
    default: return -1;
  }
}
static int engine_input_gamepad(void *userdata,int button, int edge){
  AnygmEngine *engine=userdata;
  int b = gp_to_pad(engine,button); if(b < 0) return 0;
  int cur = g_pad_cur[b], prev = g_pad_prev[b];
  if(anygm_host_development_setting(&g_host,"GML_DBG_GP_STATE")){
    engine_logf(engine,ANYGM_LOG_DEBUG,"[gpstate] f%ld button=%d pad=%d edge=%d cur=%d prev=%d\n",
            g_vm.frame,button,b,edge,cur,prev);
  }
  switch(edge){ case 1: return cur && !prev; case 2: return !cur && prev; default: return cur; }
}
static int gp_axis_slot(int axis){
  switch(axis){
    case 0: case 32785: return 0;  /* gp_axislh */
    case 1: case 32786: return 1;  /* gp_axislv */
    case 2: case 32787: return 2;  /* gp_axisrh */
    case 3: case 32788: return 3;  /* gp_axisrv */
    default: return -1;
  }
}
static double engine_input_gamepad_axis(void *userdata,int device, int axis){
  AnygmEngine *engine=userdata;
  int slot = gp_axis_slot(axis);
  if(device != 0 || slot < 0) return 0.0;
  return g_axis_cur[slot];
}

/* ---- mouse/pointer: absolute pointer plus relative mouse deltas and buttons. Position is kept in
 * presented-frame pixels; gml_input_mouse maps it to room, GUI, and window coordinate spaces for
 * the GM mouse builtins. ---- */
/* Previous frame's application-surface placement inside the published framebuffer. Pointer
 * coordinates arrive in framebuffer pixels, whereas runtime mouse coordinates live first in the
 * application surface and then in the active room view. */
static void poll_mouse(AnygmEngine *engine){
  memcpy(g_mb_prev, g_mb_cur, sizeof(g_mb_cur));
  g_mouse_wheel=g_input.wheel_delta;
  int px=g_input.pointer_x,py=g_input.pointer_y;
  int pp=g_input.pointer_pressed?1:0;
  int dx=g_input.mouse_delta_x,dy=g_input.mouse_delta_y;
  unsigned ow = g_out_w ? g_out_w : g_w, oh = g_out_h ? g_out_h : g_h;
  int mmode = core_opt_mouse_mode(engine);              /* 0 auto, 1 absolute, 2 relative */
  int pointer_signal=(px>=0 && py>=0) || pp;
  if(pointer_signal) g_pointer_active = 1;
  int use_pointer = mmode != 2 && (mmode == 1 || g_pointer_active);
  if(use_pointer && px>=0 && py>=0){
    g_mouse_px=px;
    g_mouse_py=py;
  } else if(mmode != 1 && (dx || dy)){            /* RELATIVE — accumulate deltas from last position */
    if(g_mouse_px < 0){ g_mouse_px = ow / 2.0; g_mouse_py = oh / 2.0; }
    g_mouse_px += dx; g_mouse_py += dy;
  }
  if(g_mouse_px >= 0){                             /* clamp to the frame once we have any position */
    if(g_mouse_px > ow) g_mouse_px = ow; if(g_mouse_px < 0) g_mouse_px = 0;
    if(g_mouse_py > oh) g_mouse_py = oh; if(g_mouse_py < 0) g_mouse_py = 0;
  }
  g_mb_cur[0]=(g_input.mouse_buttons[0]!=0) || pp;
  g_mb_cur[1]=g_input.mouse_buttons[1]!=0;
  g_mb_cur[2]=g_input.mouse_buttons[2]!=0;
}
/* Fill GM-space mouse state. Spaces: room (view transform), GUI (display_set_gui_size space),
 * window (presented-frame px). held/pressed/released are bitmasks: bit0=left,1=right,2=middle. */
static void engine_input_mouse(void *userdata,double *rx, double *ry, double *gx, double *gy, double *wx, double *wy,
                     int *held, int *pressed, int *released, int *wheel){
  AnygmEngine *engine=userdata;
  double sx = g_mouse_px < 0 ? 0 : g_mouse_px, sy = g_mouse_py < 0 ? 0 : g_mouse_py;
  unsigned ow = g_out_w ? g_out_w : (g_w ? g_w : 1), oh = g_out_h ? g_out_h : (g_h ? g_h : 1);
  double appx=sx, appy=sy;
  if(g_present_mouse_valid && g_present_mouse_w>0 && g_present_mouse_h>0 &&
     g_present_mouse_src_w>0 && g_present_mouse_src_h>0){
    appx=(sx-g_present_mouse_x)*(double)g_present_mouse_src_w/g_present_mouse_w;
    appy=(sy-g_present_mouse_y)*(double)g_present_mouse_src_h/g_present_mouse_h;
  }
  if(wx) *wx = sx; if(wy) *wy = sy;
  if(gx || gy){
    double gxx, gyy;
    if(g_canvas_mode){ gxx = sx - g_gui_ox; gyy = sy - g_gui_oy; }   /* canvas is 1:1 GUI space */
    else if(g_aspect_force_active){
      int full = aspect_compositor_fullwidth_gen(engine);
      gxx = sx - (full ? 0 : g_gui_ox);
      gyy = sy - (full ? 0 : g_gui_oy);
    }
    else { gxx = sx * (g_gui_sw > 0 ? g_gui_sw : (int)ow) / (double)ow;
           gyy = sy * (g_gui_sh > 0 ? g_gui_sh : (int)oh) / (double)oh; }
    if(gx) *gx = gxx; if(gy) *gy = gyy;
  }
  if(rx || ry){
    double vx = gml_global_arr(&g_vm, "view_xview", 0), vy = gml_global_arr(&g_vm, "view_yview", 0);
    double wv = gml_global_arr(&g_vm, "view_wview", 0), hv = gml_global_arr(&g_vm, "view_hview", 0);
    double sx2 = appx, sy2 = appy;
    double roomx, roomy;
    if(g_aspect_force_active){
      double camx, camy;
      aspect_forced_camera(engine,vx, vy, &camx, &camy);
      roomx = camx + sx2 * (double)g_w /
              (double)(g_present_mouse_src_w>0?g_present_mouse_src_w:(int)(ow?ow:1));
      roomy = camy + sy2 * (double)g_h /
              (double)(g_present_mouse_src_h>0?g_present_mouse_src_h:(int)(oh?oh:1));
    } else if(wv > 0 && hv > 0){
      double xp=gml_global_arr(&g_vm,"view_xport",0), yp=gml_global_arr(&g_vm,"view_yport",0);
      double wp=gml_global_arr(&g_vm,"view_wport",0), hp=gml_global_arr(&g_vm,"view_hport",0);
      if(wp<=0) wp=g_present_mouse_src_w>0?g_present_mouse_src_w:(int)(ow?ow:1);
      if(hp<=0) hp=g_present_mouse_src_h>0?g_present_mouse_src_h:(int)(oh?oh:1);
      roomx = vx + (sx2-xp) * wv / wp;
      roomy = vy + (sy2-yp) * hv / hp;
    } else { roomx = sx2; roomy = sy2; }
    if(anygm_host_development_setting(&g_host,"GML_LOG_MOUSE_COORDS")){
      if(g_diag.mouse_frame!=g_vm.frame){
        g_diag.mouse_frame=g_vm.frame;
        engine_logf(engine,ANYGM_LOG_DEBUG,"[mouse-coords] f%ld frame=%.1f,%.1f app=%.1f,%.1f room=%.1f,%.1f present=%d,%d %dx%d src=%dx%d\n",
          g_vm.frame,sx,sy,appx,appy,roomx,roomy,
          g_present_mouse_x,g_present_mouse_y,g_present_mouse_w,g_present_mouse_h,
          g_present_mouse_src_w,g_present_mouse_src_h);
      }
    }
    if(rx) *rx = roomx; if(ry) *ry = roomy;
  }
  int h = (g_mb_cur[0] ? 1 : 0) | (g_mb_cur[1] ? 2 : 0) | (g_mb_cur[2] ? 4 : 0);
  int p = ((g_mb_cur[0] && !g_mb_prev[0]) ? 1 : 0) | ((g_mb_cur[1] && !g_mb_prev[1]) ? 2 : 0) |
          ((g_mb_cur[2] && !g_mb_prev[2]) ? 4 : 0);
  int r = ((!g_mb_cur[0] && g_mb_prev[0]) ? 1 : 0) | ((!g_mb_cur[1] && g_mb_prev[1]) ? 2 : 0) |
          ((!g_mb_cur[2] && g_mb_prev[2]) ? 4 : 0);
  if(held) *held = h; if(pressed) *pressed = p; if(released) *released = r;
  if(wheel) *wheel = g_mouse_wheel;
}
static void engine_input_mouse_set(void *userdata,double x, double y){
  AnygmEngine *engine=userdata;
  unsigned ow=g_out_w?g_out_w:(g_w?g_w:1), oh=g_out_h?g_out_h:(g_h?g_h:1);
  g_mouse_px=x<0?0:(x>(double)ow?ow:x);
  g_mouse_py=y<0?0:(y>(double)oh?oh:y);
}

/* GameMaker's cursor_sprite is an authored software cursor, distinct from the host's window
 * cursor. A desktop host can make a missing implementation look correct in windowed mode by
 * overlaying its OS pointer, then expose the omission when fullscreen hides that pointer. Draw the
 * authored sprite last, in presented-frame coordinates, so it remains visible above transitions,
 * GUI and letterbox margins. Its logical size follows the application viewport scale. */
static void draw_game_cursor(AnygmEngine *engine,unsigned width,unsigned height){
  int sprite=(int)lround(gml_global_num(&g_vm,"cursor_sprite"));
  if(sprite<0 || sprite>=g_render.n_spr || g_mouse_px<0 || g_mouse_py<0 || !width || !height)
    return;
  double scale_x=1.0,scale_y=1.0;
  if(g_present_mouse_valid && g_present_mouse_src_w>0 && g_present_mouse_src_h>0 &&
     g_present_mouse_w>0 && g_present_mouse_h>0){
    scale_x=(double)g_present_mouse_w/g_present_mouse_src_w;
    scale_y=(double)g_present_mouse_h/g_present_mouse_src_h;
  }
  gml_render_begin(&g_render,g_screen,(int)width,(int)height,0.0,0.0);
  g_render.alphablend=1;
  g_render.alpha_test_enable=0;
  g_render.color_write_mask=0x0F;
  gml_draw_sprite_ext(&g_render,sprite,0,g_mouse_px,g_mouse_py,
                      scale_x,scale_y,0.0,0xFFFFFF,1.0);
  if(anygm_host_development_setting(&g_host,"GML_LOG_CURSOR")){ if(g_diag.cursor_frame!=g_vm.frame && (g_vm.frame<4 || g_vm.frame%120==0)){
      g_diag.cursor_frame=g_vm.frame;
      engine_logf(engine,ANYGM_LOG_DEBUG,"[cursor] f%ld sprite=%d frame=%.1f,%.1f scale=%.4f,%.4f\n",
              g_vm.frame,sprite,g_mouse_px,g_mouse_py,scale_x,scale_y);
    }
  }
}

/* GameMaker color (0x00BBGGRR) -> XRGB8888 (0x00RRGGBB).
 * High byte = coverage (0xFF = drawn): the room-bg fill counts as drawn so view-surface
 * mirrors of the frame composite as opaque, not "cleared-to-transparent" (see draw_clear_alpha). */
static uint32_t gm_to_xrgb(uint32_t c) {
  uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static int core_opt_god(AnygmEngine *engine) {
  return g_config.god_mode?1:0;
}
static int core_opt_resolution(AnygmEngine *engine,int height) {
  uint32_t value=height?g_config.present_height:g_config.present_width;
  uint32_t maximum=height?FB_MAX_H:FB_MAX_W;
  return (int)(value>maximum?maximum:value);
}
static int core_opt_embedded_shaders(AnygmEngine *engine) {
  return g_config.embedded_shaders?1:0;
}
/* CRT aperture-mask toggle. The mask is per-raster-pixel chroma and assumes integer scaling.
 * Turning it off substitutes the neutral average so
 * scaled presentations keep scanlines without two-tone banding. GML_CRT_MASK=0 for headless. */
static int core_opt_crt_mask(AnygmEngine *engine) {
  return g_config.crt_mask?1:0;
}
/* Per-component CRT toggles. Curvature and vignette also accept Auto to follow shader uniforms. */
static int core_opt_onoff(AnygmEngine *engine,const char *key, const char *env, int def) {
  (void)env;
  if(!strcmp(key,"anygm_crt_scanlines")) return g_config.crt_scanlines?1:0;
  if(!strcmp(key,"anygm_crt_gamma")) return g_config.crt_gamma?1:0;
  return def;
}
static int core_opt_crt_tristate(AnygmEngine *engine,const char *key, const char *env) { /* -1 auto / 0 off / 1 on */
  (void)env;
  if(!strcmp(key,"anygm_crt_curvature")) return g_config.crt_curvature;
  if(!strcmp(key,"anygm_crt_vignette")) return g_config.crt_vignette;
  return -1;
}
static int core_opt_mouse_mode(AnygmEngine *engine) {
  return g_config.mouse_mode<=2u?(int)g_config.mouse_mode:0;
}
static int core_opt_aspect_force(AnygmEngine *engine) {
  return g_config.aspect_mode<=GMC_ASPECT_FORCE_21_9?(int)g_config.aspect_mode:GMC_ASPECT_FORCE_NONE;
}
static void clamp_camera_to_current_room(AnygmEngine *engine,double *x, double *y, int center_when_smaller) {
  if (!x || !y) return;
  GmlRoom rm;
  if (gml_room_get(&g_win, g_vm.room_index, &rm) != 0) return;
  if (rm.width > 0) {
    double maxx = (double)rm.width - (double)g_w;
    if (maxx <= 0.0) {
      *x = center_when_smaller ? maxx * 0.5 : 0.0;
    } else {
      if (*x < 0.0) *x = 0.0;
      if (*x > maxx) *x = maxx;
    }
  }
  if (rm.height > 0) {
    double maxy = (double)rm.height - (double)g_h;
    if (maxy <= 0.0) {
      *y = center_when_smaller ? maxy * 0.5 : 0.0;
    } else {
      if (*y < 0.0) *y = 0.0;
      if (*y > maxy) *y = maxy;
    }
  }
}
static void aspect_forced_camera(AnygmEngine *engine,double raw_x, double raw_y, double *out_x, double *out_y) {
  double x = raw_x, y = raw_y;
  if (g_aspect_force_active) {
    int centered_x = 0, centered_y = 0;
    if (aspect_center_view_target_gen(engine)) {
      int target_obj = (int)gml_global_arr(&g_vm, "view_object", 0);
      GmlInstance *target = target_obj >= 0 ? gml_find_instance(&g_vm, target_obj) : NULL;
      if (target) {
        if (g_w != g_base_w) { x = target->x - (double)g_w * 0.5; centered_x = 1; }
        if (g_h != g_base_h) { y = target->y - (double)g_h * 0.5; centered_y = 1; }
        clamp_camera_to_current_room(engine,&x, &y, 1);
      }
    }
    /* Clamp the supplied top-left coordinate in the original view space,
     * then add half of the extra forced-aspect extent on each side. Clamping after adding the
     * negative margin pins a left-edge camera back to zero and shifts the whole scene right. */
    GmlRoom rm;
    if (gml_room_get(&g_win, g_vm.room_index, &rm) == 0) {
      if (!centered_x && rm.width > 0 && g_base_w > 0) {
        double maxx = (double)rm.width - (double)g_base_w;
        if (maxx <= 0.0) x = maxx * 0.5;
        else { if (x < 0.0) x = 0.0; if (x > maxx) x = maxx; }
      }
      if (!centered_y && rm.height > 0 && g_base_h > 0) {
        double maxy = (double)rm.height - (double)g_base_h;
        if (maxy <= 0.0) y = maxy * 0.5;
        else { if (y < 0.0) y = 0.0; if (y > maxy) y = maxy; }
      }
    }
    if (!centered_x) x += g_aspect_cam_dx;
    if (!centered_y) y += g_aspect_cam_dy;
    /* A data-declared wide gameplay view must never expose coordinates beyond a real room edge.
     * Keeping the render camera and gameplay rectangle on the same clamped origin also avoids
     * changing coordinate systems partway through the leading-edge scroll interval. */
    if (aspect_wide_gameplay_view_gen(engine))
      clamp_camera_to_current_room(engine,&x, &y, 1);
  }
  if (out_x) *out_x = x;
  if (out_y) *out_y = y;
}
static int core_opt_gamepad_connected(AnygmEngine *engine) {
  return g_config.gamepad_connected?1:0;
}
static int core_opt_fast_alpha_cull(AnygmEngine *engine) {
  return g_config.fast_alpha_cull>32u?32:(int)g_config.fast_alpha_cull;
}
static int engine_input_gamepad_connected(void *userdata,int device) {
  AnygmEngine *engine=userdata;
  return device == 0 && core_opt_gamepad_connected(engine);
}
static int engine_input_gamepad_device_count(void *userdata) {
  AnygmEngine *engine=userdata;
  return (int)ANYGM_MAX_GAMEPADS;
}
static uint16_t rumble_strength(double v) {
  if (!isfinite(v) || v <= 0.0) return 0;
  if (v >= 1.0) return 0xffffu;
  return (uint16_t)(v * 65535.0 + 0.5);
}
static void engine_input_gamepad_set_vibration(void *userdata,int device, double low, double high) {
  AnygmEngine *engine=userdata;
  if(!g_host.rumble || device<0) return;
  g_host.rumble(g_host.userdata,(uint32_t)device,rumble_strength(low),rumble_strength(high));
}
static int core_opt_start_room(AnygmEngine *engine,int *room_idx) {
  if(!room_idx || g_config.start_room<0) return 0;
  *room_idx=g_config.start_room;
  return 1;
}
static void setup_platform_locale(AnygmEngine *engine,GmlVM *vm){
  snprintf(vm->os_language,sizeof vm->os_language,"%s",g_language);
  snprintf(vm->os_region,sizeof vm->os_region,"%s",g_region);
  snprintf(vm->language_tag,sizeof vm->language_tag,"%s",g_language_tag);
}

static double explicit_game_speed_fps(AnygmEngine *engine) {
  GmlVal *p = gml_varmap_get(&g_vm.globals, "__game_speed_fps");
  double fps = 0.0;
  if(p){
    if(p->t == V_REAL) fps = p->d;
    else if(p->t == V_STR && p->s) fps = atof(p->s);
  }
  return fps > 0.0 ? fps : 0.0;
}
static double cur_room_fps(AnygmEngine *engine) {
  /* Keep the reported frame rate stable. GMS2 games change room_speed mid-play for effects
   * (death slow-mo, hitstop, a time-scale effect). Reflecting those changes through SET_SYSTEM_AV_INFO
   * makes the host reconfigure and stutter, so GMS2 defaults to 60 unless game_set_speed()
   * explicitly asks for a different global cadence. Older data.win versions keep using room.speed. */
  double fps = explicit_game_speed_fps(engine);
  if(fps > 0.0) return fps;
  if(anygm_policy_has_modern_layer_semantics(&g_win)) return 60.0;
  GmlRoom r;
  if (gml_room_get(&g_win, g_vm.room_index, &r) == 0 && r.speed > 0) return (double)r.speed;
  return 60.0;
}
typedef struct {
  int index, visible, camera;
  double x, y, w, h;
  int px, py, pw, ph;
} GmlPresentView;

/* Resolve a view through its opaque GMS camera handle.  Camera resources are maintained by the VM
 * builtins in reserved, serialized global arrays; rooms and classic games that only use view_*
 * variables fall back to those values unchanged. */
static int present_view_get(AnygmEngine *engine,int index, GmlPresentView *out) {
  if (!out || index < 0 || index >= 8) return 0;
  memset(out, 0, sizeof(*out));
  out->index = index;
  out->visible = gml_global_arr(&g_vm, "view_visible", index) >= 0.5;
  out->camera = (int)lround(gml_global_arr(&g_vm, "view_camera", index));
  int live = out->camera >= 0 && out->camera < 64 &&
             gml_global_arr(&g_vm, "__gml_camera_live", out->camera) >= 0.5;
  if (live) {
    out->x = gml_global_arr(&g_vm, "__gml_camera_x", out->camera);
    out->y = gml_global_arr(&g_vm, "__gml_camera_y", out->camera);
    out->w = gml_global_arr(&g_vm, "__gml_camera_w", out->camera);
    out->h = gml_global_arr(&g_vm, "__gml_camera_h", out->camera);
  } else {
    out->camera = index;
    out->x = gml_global_arr(&g_vm, "view_xview", index);
    out->y = gml_global_arr(&g_vm, "view_yview", index);
    out->w = gml_global_arr(&g_vm, "view_wview", index);
    out->h = gml_global_arr(&g_vm, "view_hview", index);
  }
  out->px = (int)lround(gml_global_arr(&g_vm, "view_xport", index));
  out->py = (int)lround(gml_global_arr(&g_vm, "view_yport", index));
  out->pw = (int)lround(gml_global_arr(&g_vm, "view_wport", index));
  out->ph = (int)lround(gml_global_arr(&g_vm, "view_hport", index));
  if (out->pw <= 0 && out->w > 0) out->pw = (int)lround(out->w);
  if (out->ph <= 0 && out->h > 0) out->ph = (int)lround(out->h);
  return out->visible && out->w > 0 && out->h > 0 && out->pw > 0 && out->ph > 0;
}

static int present_view_count(AnygmEngine *engine,GmlPresentView views[8], int *canvas_w, int *canvas_h) {
  int count = 0, maxx = 0, maxy = 0;
  for (int i = 0; i < 8; i++) {
    GmlPresentView v;
    if (!present_view_get(engine,i, &v)) continue;
    if (views) views[count] = v;
    count++;
    if (v.px + v.pw > maxx) maxx = v.px + v.pw;
    if (v.py + v.ph > maxy) maxy = v.py + v.ph;
  }
  if (canvas_w) *canvas_w = maxx;
  if (canvas_h) *canvas_h = maxy;
  return count;
}

/* A runtime-owned application-surface resize can leave the room's authored view port at the
 * export-time desktop size.  If there is one full-origin view and the resized application
 * surface, logical view and explicit GUI all agree, that old larger port is no longer a literal
 * inset: the presentation contract maps the complete view to the complete resized surface/window.  Keeping the
 * stale port clips the lower/right part whenever the runtime desktop has another aspect ratio. */
static int stale_full_view_port(AnygmEngine *engine,int view_count, int px, int py, int pw, int ph,
                                int logical_w, int logical_h, int app_w, int app_h) {
  if (view_count != 1 || px != 0 || py != 0 || pw <= 0 || ph <= 0 ||
      logical_w <= 0 || logical_h <= 0 || app_w <= 0 || app_h <= 0)
    return 0;
  if (abs(logical_w - app_w) > 1 || abs(logical_h - app_h) > 1)
    return 0;
  if (g_vm.gui_w <= 0 || g_vm.gui_h <= 0 ||
      abs(g_vm.gui_w - app_w) > 1 || abs(g_vm.gui_h - app_h) > 1)
    return 0;
  return pw >= app_w && ph >= app_h && (pw > app_w || ph > app_h);
}

/* native render size = the current room's view region (view_wview). Games render the world 1:1 into
 * that and scale it to the window through the declared view port.
 * Multiple simultaneous views instead form one application-surface canvas from their port union;
 * each camera is rendered and composited into that canvas later.  When views are off, GM draws the
 * whole room, so fall back to room size. Always clamped to the fb. */
static void cur_room_base_res(AnygmEngine *engine,unsigned *ow, unsigned *oh) {
  GmlPresentView views[8]; int canvas_w = 0, canvas_h = 0;
  int view_count = present_view_count(engine,views, &canvas_w, &canvas_h);
  unsigned w, h;
  if (view_count > 1 && canvas_w > 0 && canvas_h > 0) {
    w = (unsigned)canvas_w; h = (unsigned)canvas_h;
  } else if (view_count == 1) {
    w = (unsigned)lround(views[0].w); h = (unsigned)lround(views[0].h);
  }
  else {
    GmlRoom r;
    if (gml_room_get(&g_win, g_vm.room_index, &r) == 0 && r.width > 0 && r.height > 0) { w = r.width; h = r.height; }
    else { w = g_win.disp_w ? g_win.disp_w : 288; h = g_win.disp_h ? g_win.disp_h : 216; }
  }
  if (w > FB_MAX_W) w = FB_MAX_W;
  if (h > FB_MAX_H) h = FB_MAX_H;
  *ow = w; *oh = h;
}
static unsigned round_to_multiple_of_8(double v) {
  if (!isfinite(v) || v < 8.0) return 8;
  unsigned out = (unsigned)lround(v / 8.0) * 8u;
  if (out < 8) out = 8;
  return out;
}
static long gui_margin_tolerance(int logical) {
  long tol = logical > 0 ? logical / 50 : 0;  /* two percent, with an 8px floor */
  return tol > 8 ? tol : 8;
}
static int gui_canvas_has_margin(long canvas_w, long canvas_h, int gui_w, int gui_h) {
  return canvas_w > gui_w + gui_margin_tolerance(gui_w) ||
         canvas_h > gui_h + gui_margin_tolerance(gui_h);
}
static int gui_window_near_native(int win_w, int win_h, int gui_w, int gui_h) {
  long xtol = gui_w > 0 ? gui_w / 20 : 0;  /* display_set_gui_size may differ slightly from the window */
  long ytol = gui_h > 0 ? gui_h / 20 : 0;
  if (xtol < 8) xtol = 8;
  if (ytol < 8) ytol = 8;
  return labs((long)win_w - gui_w) <= xtol && labs((long)win_h - gui_h) <= ytol;
}
static int aspect_canvas_present_res(AnygmEngine *engine,unsigned base_w, unsigned base_h,
                                     unsigned *out_w, unsigned *out_h) {
  if (!out_w || !out_h || g_vm.gui_w <= 0 || g_vm.gui_h <= 0) return 0;
  double vvis = gml_global_arr(&g_vm, "view_visible", 0);
  double port_w = 0.0, port_h = 0.0;
  if (vvis >= 0.5) {
    port_w = gml_global_arr(&g_vm, "view_wport", 0);
    port_h = gml_global_arr(&g_vm, "view_hport", 0);
  }
  int gw = g_vm.gui_w > 0 ? g_vm.gui_w : (port_w > 0 ? (int)port_w : (int)base_w);
  int gh = g_vm.gui_h > 0 ? g_vm.gui_h : (port_h > 0 ? (int)port_h : (int)base_h);
  if (gw < 16) gw = (int)base_w;
  if (gh < 16) gh = (int)base_h;
  if (gw <= 0 || gh <= 0) return 0;
  int win_w = g_vm.window_w > 0 ? g_vm.window_w : (int)(g_win.disp_w ? g_win.disp_w : base_w);
  int win_h = g_vm.window_h > 0 ? g_vm.window_h : (int)(g_win.disp_h ? g_win.disp_h : base_h);
  if (win_w <= 0 || win_h <= 0) return 0;
  double s = (double)win_w / (double)gw, s2 = (double)win_h / (double)gh;
  if (s2 < s) s = s2;
  if (s < 1e-6) return 0;
  long cw = lround((double)win_w / s), ch = lround((double)win_h / s);
  if (cw > FB_MAX_W) cw = FB_MAX_W;
  if (ch > FB_MAX_H) ch = FB_MAX_H;
  if (gui_canvas_has_margin(cw, ch, gw, gh)) {
    *out_w = (unsigned)cw;
    *out_h = (unsigned)ch;
    return *out_w > 0 && *out_h > 0;
  }
  return 0;
}
static void apply_aspect_force_to_res(AnygmEngine *engine,unsigned base_w, unsigned base_h,
                                      unsigned *ow, unsigned *oh) {
  unsigned w = base_w, h = base_h;
  int mode = core_opt_aspect_force(engine);
  g_aspect_force_mode = mode;
  g_aspect_force_active = 0;
  g_aspect_cam_dx = g_aspect_cam_dy = 0.0;
  g_aspect_gui_ox = g_aspect_gui_oy = 0;
  if (mode != GMC_ASPECT_FORCE_NONE && base_w > 0 && base_h > 0) {
    const double target = mode == GMC_ASPECT_FORCE_21_9 ? (21.0 / 9.0) :
                          mode == GMC_ASPECT_FORCE_16_9 ? (16.0 / 9.0) : (4.0 / 3.0);
    double ratio = (double)base_w / (double)base_h;
    unsigned present_w = 0, present_h = 0;
    int already_target = fabs(ratio - target) <= 0.2;
    if (!already_target &&
        aspect_canvas_present_res(engine,base_w, base_h, &present_w, &present_h)) {
      double present_ratio = (double)present_w / (double)present_h;
      already_target = fabs(present_ratio - target) <= 0.2;
    }
    if (!already_target) {
      unsigned target_w = round_to_multiple_of_8((double)base_h * target);
      if (target_w > FB_MAX_W) target_w = FB_MAX_W & ~7u;
      if (target_w < 8) target_w = 8;
      if (target_w != base_w) {
        w = target_w;
        g_aspect_force_active = 1;
        g_aspect_cam_dx = ((double)base_w - (double)w) * 0.5;
      }
    }
  }
  if ((w != base_w || h != base_h) && !g_aspect_force_active) {
    g_aspect_force_active = 1;
    g_aspect_cam_dx = ((double)base_w - (double)w) * 0.5;
    g_aspect_cam_dy = ((double)base_h - (double)h) * 0.5;
  }
  if (w > FB_MAX_W) w = FB_MAX_W;
  if (h > FB_MAX_H) h = FB_MAX_H;
  if (w < 8) w = 8;
  if (h < 8) h = 8;
  if (w == base_w && h == base_h) {
    g_aspect_force_active = 0;
    g_aspect_cam_dx = g_aspect_cam_dy = 0.0;
    g_aspect_gui_ox = g_aspect_gui_oy = 0;
  }
  *ow = w; *oh = h;
}
static void cur_room_res(AnygmEngine *engine,unsigned *ow, unsigned *oh) {
  unsigned bw, bh;
  cur_room_base_res(engine,&bw, &bh);
  g_base_w = bw; g_base_h = bh;
  apply_aspect_force_to_res(engine,bw, bh, ow, oh);
}
static void cur_classic_room_window_res(AnygmEngine *engine,unsigned *ow, unsigned *oh) {
  GmlRoom room;
  int room_w=(int)(g_win.disp_w?g_win.disp_w:288);
  int room_h=(int)(g_win.disp_h?g_win.disp_h:216);
  int visible[8], xport[8], yport[8], wport[8], hport[8];
  int window_w, window_h;
  if(gml_room_get(&g_win,g_vm.room_index,&room)==0){
    if(room.width>0) room_w=(int)room.width;
    if(room.height>0) room_h=(int)room.height;
  }
  for(int i=0;i<8;i++){
    visible[i]=gml_global_arr(&g_vm,"view_visible",i)>=0.5;
    xport[i]=(int)lround(gml_global_arr(&g_vm,"view_xport",i));
    yport[i]=(int)lround(gml_global_arr(&g_vm,"view_yport",i));
    wport[i]=(int)lround(gml_global_arr(&g_vm,"view_wport",i));
    hport[i]=(int)lround(gml_global_arr(&g_vm,"view_hport",i));
  }
  int fixed_scale=g_win.classic_scaling;
  /* A classic room_set_view call replaces the authored port at runtime. At 100% scaling that
   * runtime port is the new window extent; the project's startup display dimensions no longer
   * pin it. Static rooms retain the configured fixed-scale behaviour. */
  if(g_vm.view_ovr){
    int first=g_vm.room_index*8;
    for(int i=0;i<8 && first+i<g_vm.n_view_ovr;i++)
      if(first+i>=0 && g_vm.view_ovr[first+i].full){ fixed_scale=-1; break; }
  }
  gml_classic_room_window_size(room_w,room_h,fixed_scale,
                               (int)g_win.disp_w,(int)g_win.disp_h,
                               visible,xport,yport,wport,hport,
                               &window_w,&window_h);
  if(window_w>FB_MAX_W) window_w=FB_MAX_W;
  if(window_h>FB_MAX_H) window_h=FB_MAX_H;
  *ow=(unsigned)window_w;
  *oh=(unsigned)window_h;
}
/* ---- presentation canvas (window/GUI layer) ----
 * GM presents content inside a window: the view renders to the application surface, the GUI
 * canvas (display_set_gui_size) is scaled uniformly to fit the window and centered, and GUI
 * drawing may intentionally spill outside the canvas rectangle into the window margins. When
 * window and GUI geometry differ, present a window-shaped logical canvas with the GUI pass offset
 * to the centered canvas rectangle. The result is derived from window_set_size and
 * display_set_gui_size. */
static void aspect_hud_rect(AnygmEngine *engine,int *out_x, int *out_y, int *out_w, int *out_h) {
  int hw = (g_base_w > 0 && g_base_w < g_w) ? (int)g_base_w : (int)g_w;
  int hh = (g_base_h > 0 && g_base_h < g_h) ? (int)g_base_h : (int)g_h;
  GmlRoom rm;
  if (gml_room_get(&g_win, g_vm.room_index, &rm) == 0) {
    if (rm.width > 0 && rm.width < (uint32_t)hw) hw = (int)rm.width;
    if (rm.height > 0 && rm.height < (uint32_t)hh) hh = (int)rm.height;
  }
  if (hw < 1) hw = (int)g_w;
  if (hh < 1) hh = (int)g_h;
  int hx = (int)lround(((double)g_w - (double)hw) * 0.5);
  int hy = (int)lround(((double)g_h - (double)hh) * 0.5);
  if (hx < 0) hx = 0;
  if (hy < 0) hy = 0;
  if (hx + hw > (int)g_w) hw = (int)g_w - hx;
  if (hy + hh > (int)g_h) hh = (int)g_h - hy;
  if (out_x) *out_x = hx;
  if (out_y) *out_y = hy;
  if (out_w) *out_w = hw;
  if (out_h) *out_h = hh;
}
static void compute_present(AnygmEngine *engine) {
  g_render.presentation_w = 0;
  g_render.presentation_h = 0;
  /* GUI drawing space: explicit (display_set_gui_size) or the enabled view port, falling back to
   * the view. A disabled view can still carry a default port in its data, which must not affect
   * presentation geometry. */
  double vvis = gml_global_arr(&g_vm, "view_visible", 0);
  double pw = 0, ph = 0;
  if (vvis >= 0.5) { pw = gml_global_arr(&g_vm, "view_wport", 0); ph = gml_global_arr(&g_vm, "view_hport", 0); }
  int gw = g_vm.gui_w > 0 ? g_vm.gui_w : (pw > 0 ? (int)pw : (int)g_w);
  int gh = g_vm.gui_h > 0 ? g_vm.gui_h : (ph > 0 ? (int)ph : (int)g_h);
  if (gw < 16) gw = g_w; if (gh < 16) gh = g_h;
  if (gw > FB_MAX_W) gw = FB_MAX_W;
  if (gh > FB_MAX_H) gh = FB_MAX_H;
  g_gui_sw = gw; g_gui_sh = gh;
  /* Canvas mode applies only when content declares a GUI canvas smaller than its window, making
   * the surrounding margins intentional drawing area. A large window without a declared GUI is
   * display scaling, so the host receives the native view. */
  int gui_window_mode = 0;
  g_canvas_mode = 0; g_out_w = g_w; g_out_h = g_h; g_gui_ox = g_gui_oy = 0;
  if (g_vm.gui_w > 0 && g_vm.gui_h > 0) {
    int win_w = g_vm.window_w > 0 ? g_vm.window_w : (int)(g_win.disp_w ? g_win.disp_w : g_w);
    int win_h = g_vm.window_h > 0 ? g_vm.window_h : (int)(g_win.disp_h ? g_win.disp_h : g_h);
    double s = (double)win_w / gw, s2 = (double)win_h / gh;
    if (s2 < s) s = s2;
    if (s < 1e-6) s = 1.0;
    long cw = lround(win_w / s), ch = lround(win_h / s);
    if (cw > FB_MAX_W) cw = FB_MAX_W;
    if (ch > FB_MAX_H) ch = FB_MAX_H;
    if (gui_canvas_has_margin(cw, ch, gw, gh)) {   /* real margin -> present the window canvas */
      g_canvas_mode = 1; g_out_w = (unsigned)cw; g_out_h = (unsigned)ch;
      g_gui_ox = (int)((cw - gw) / 2); g_gui_oy = (int)((ch - gh) / 2);
      if (g_gui_ox < 0) g_gui_ox = 0;
      if (g_gui_oy < 0) g_gui_oy = 0;
    } else if (gui_window_near_native(win_w, win_h, gw, gh) &&
               win_w <= FB_MAX_W && win_h <= FB_MAX_H) {
      /* A near-native GUI size is a coordinate system stretched to the window, not a request
       * for a small uniform-fit canvas. Preserve the declared window pixels. */
      gui_window_mode = 1;
      g_out_w = (unsigned)win_w; g_out_h = (unsigned)win_h;
    }
  }
  /* When a port is larger than its view, present at GUI/port resolution rather than downscaling
   * port-space GUI and text to the smaller view. The world view is scaled into the port rectangle,
   * matching the declared window. GML_PRESENT_VIEW=1 forces view-resolution presentation for
   * diagnostics. This affects presentation only, not simulation or serialized state. */
  { if (g_diag.force_present_view < 0) g_diag.force_present_view = anygm_host_development_setting(&g_host,"GML_PRESENT_VIEW") ? 1 : 0;
    if (!g_diag.force_present_view && !g_canvas_mode && !gui_window_mode
        && g_gui_sw >= (int)g_out_w && g_gui_sh >= (int)g_out_h
        && (g_gui_sw > (int)g_out_w || g_gui_sh > (int)g_out_h)
        && g_gui_sw <= FB_MAX_W && g_gui_sh <= FB_MAX_H) {
      g_out_w = (unsigned)g_gui_sw; g_out_h = (unsigned)g_gui_sh; g_gui_ox = g_gui_oy = 0;
    } }
  if (g_aspect_force_active) {
    int logical_gw = g_vm.gui_w > 0 ? g_vm.gui_w : (int)g_base_w;
    int logical_gh = g_vm.gui_h > 0 ? g_vm.gui_h : (int)g_base_h;
    if (logical_gw < 16) logical_gw = (int)g_base_w;
    if (logical_gh < 16) logical_gh = (int)g_base_h;
    g_canvas_mode = 0;
    g_out_w = g_w;
    g_out_h = g_h;
    g_gui_sw = (int)g_w;
    g_gui_sh = (int)g_h;
    g_gui_ox = (int)lround(((double)g_w - (double)logical_gw) * 0.5) + g_aspect_gui_ox;
    g_gui_oy = (int)lround(((double)g_h - (double)logical_gh) * 0.5) + g_aspect_gui_oy;
  }
  /* Classic presentation keeps the window/port size stable when game code changes view_wview or
   * view_hview: those variables zoom the camera, they do not resize the host window. The world
   * is still rendered at the logical view extent above and is presented into this fixed window. */
  if (anygm_policy_uses_classic_runtime(&g_win) && !g_aspect_force_active &&
      g_render.resolution_w <= 0 && g_render.resolution_h <= 0) {
    unsigned room_window_w, room_window_h;
    cur_classic_room_window_res(engine,&room_window_w, &room_window_h);
    int window_w = g_vm.window_w > 0 ? g_vm.window_w : (int)room_window_w;
    int window_h = g_vm.window_h > 0 ? g_vm.window_h : (int)room_window_h;
    if (window_w > 0 && window_h > 0 && window_w <= FB_MAX_W && window_h <= FB_MAX_H) {
      g_canvas_mode = 0;
      g_out_w = (unsigned)window_w;
      g_out_h = (unsigned)window_h;
      g_gui_sw = window_w;
      g_gui_sh = window_h;
      g_gui_ox = g_gui_oy = 0;
    }
  }
  /* A Studio game can explicitly own the final window raster in either of two ways: resize the
   * application surface and keep automatic presentation, or disable automatic presentation and
   * compose the application surface plus decoration itself in Post-Draw. In the latter case the
   * explicit window is the drawing coordinate system even when the application surface remains at
   * the smaller view size. Preserve that authored raster; otherwise the Post-Draw margins are
   * clipped to the central view. The signals are runtime semantics, independent of game identity. */
  if (anygm_policy_has_modern_layer_semantics(&g_win) && !g_aspect_force_active && !g_canvas_mode &&
      !g_render.app_draw_enable &&
      g_vm.gui_w <= 0 && g_vm.gui_h <= 0 &&
      g_vm.window_w > 0 && g_vm.window_h > 0 &&
      g_vm.window_w <= FB_MAX_W && g_vm.window_h <= FB_MAX_H) {
    g_out_w = (unsigned)g_vm.window_w;
    g_out_h = (unsigned)g_vm.window_h;
    g_gui_sw = g_vm.window_w;
    g_gui_sh = g_vm.window_h;
    g_gui_ox = g_gui_oy = 0;
    g_render.presentation_w = g_vm.window_w;
    g_render.presentation_h = g_vm.window_h;
  }
  /* A Studio game can explicitly resize application_surface to the window and keep the normal
   * automatic presentation path.  That surface is the final presentation raster, not an oversized
   * internal effect buffer: presenting the smaller room view instead box-reduces the authored
   * window pixels (pixel-art edges become 1/3 and 2/3 grey at a 3x window scale).  Preserve the
   * explicit raster when all three independent signals agree: owned surface, matching window,
   * and automatic app-surface drawing.  Self-compositing games disable app_draw_enable and keep
   * their existing logical/GUI path; explicit GUI canvases and aspect overrides are likewise
   * left untouched. */
  if (anygm_policy_has_modern_layer_semantics(&g_win) && !g_aspect_force_active && !g_canvas_mode &&
      g_render.app_surface_owned && g_render.app_draw_enable &&
      g_vm.gui_w <= 0 && g_vm.gui_h <= 0 &&
      g_render.app_w > 0 && g_render.app_h > 0 &&
      g_render.app_w <= FB_MAX_W && g_render.app_h <= FB_MAX_H &&
      g_vm.window_w == g_render.app_w && g_vm.window_h == g_render.app_h) {
    g_out_w = (unsigned)g_render.app_w;
    g_out_h = (unsigned)g_render.app_h;
    g_gui_sw = g_render.app_w;
    g_gui_sh = g_render.app_h;
    g_gui_ox = g_gui_oy = 0;
    g_render.presentation_w = g_render.app_w;
    g_render.presentation_h = g_render.app_h;
  }
  /* An explicit resolution is the final framebuffer and GUI/compositor space. Aspect forcing still
   * controls the logical room/view width independently. Fast-forward must not change geometry. */
  if (g_render.resolution_w > 0 || g_render.resolution_h > 0) {
    int base_w = (g_render.aspect_fullwidth && g_render.aspect_wide_w > 0)
               ? g_render.aspect_wide_w : (int)(g_win.disp_w ? g_win.disp_w : g_w);
    int base_h = (g_render.aspect_fullwidth && g_render.aspect_wide_h > 0)
               ? g_render.aspect_wide_h : (int)(g_win.disp_h ? g_win.disp_h : g_h);
    int target_h = g_render.resolution_h > 0 ? g_render.resolution_h : base_h;
    int target_w = g_render.resolution_w > 0 ? g_render.resolution_w : base_w;
    /* Aspect Ratio Force composes with explicit resolution instead of being overwritten by it.
     * The vertical axis is authoritative and the effective width is derived from it; the raw
     * width option deliberately remains untouched so the host still shows what was selected. */
    if (g_aspect_force_mode != GMC_ASPECT_FORCE_NONE && target_h > 0) {
      double ratio = g_aspect_force_mode == GMC_ASPECT_FORCE_21_9 ? (21.0 / 9.0) :
                     g_aspect_force_mode == GMC_ASPECT_FORCE_16_9 ? (16.0 / 9.0) : (4.0 / 3.0);
      target_w = (int)round_to_multiple_of_8((double)target_h * ratio);
      if (target_w > FB_MAX_W) target_w = FB_MAX_W & ~7;
    }
    if (target_w > 0 && target_h > 0 && target_w <= FB_MAX_W && target_h <= FB_MAX_H) {
      g_canvas_mode = 0;
      g_gui_sw = target_w; g_gui_sh = target_h;
      g_out_w = (unsigned)target_w; g_out_h = (unsigned)target_h;
      g_gui_ox = g_gui_oy = 0;
      g_render.presentation_w = target_w;
      g_render.presentation_h = target_h;
    }
  }
}
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
static void aspect_view_overlay_begin(AnygmEngine *engine,AspectViewOverlay *ov, int center_hud, int view_mode) {
  memset(ov, 0, sizeof(*ov));
  if (!g_aspect_force_active) return;
  ov->active = 1;
  ov->room = g_vm.room_index;
  ov->xview = gml_global_arr(&g_vm, "view_xview", 0);
  ov->yview = gml_global_arr(&g_vm, "view_yview", 0);
  ov->wview = gml_global_arr(&g_vm, "view_wview", 0);
  ov->hview = gml_global_arr(&g_vm, "view_hview", 0);
  ov->wport = gml_global_arr(&g_vm, "view_wport", 0);
  ov->hport = gml_global_arr(&g_vm, "view_hport", 0);

  aspect_forced_camera(engine,ov->xview, ov->yview, &ov->render_x, &ov->render_y);
  /* Camera controllers need one coherent coordinate system: a forced extent paired with its
   * forced origin. Mixing a widened extent with the native origin makes a centered follower
   * subtract half the extra width twice. Near the leading native edge, however, many games clamp
   * xview/yview to zero. Keep the native extent only inside that half-margin band; once scrolling
   * begins, switching to forced coordinates is continuous because the forced origin is then zero.
   * This preserves the intended edge clamp while keeping freely scrolling subjects centered. */
  int forced_x = view_mode != ASPECT_VIEW_NATIVE;
  int forced_y = view_mode != ASPECT_VIEW_NATIVE;
  if (view_mode == ASPECT_VIEW_TRACKING && !aspect_wide_gameplay_view_gen(engine)) {
    if (g_w > g_base_w && g_aspect_cam_dx < 0.0 &&
        ov->xview <= -g_aspect_cam_dx + 0.001)
      forced_x = 0;
    if (g_h > g_base_h && g_aspect_cam_dy < 0.0 &&
        ov->yview <= -g_aspect_cam_dy + 0.001)
      forced_y = 0;
  }
  double gx = forced_x ? ov->render_x : ov->xview;
  double gy = forced_y ? ov->render_y : ov->yview;
  double gw = (!forced_x && view_mode == ASPECT_VIEW_TRACKING) ? (double)g_base_w : (double)g_w;
  double gh = (!forced_y && view_mode == ASPECT_VIEW_TRACKING) ? (double)g_base_h : (double)g_h;
  if (center_hud) {
    int hx, hy, hw, hh;
    aspect_hud_rect(engine,&hx, &hy, &hw, &hh);
    gx = ov->render_x + (double)hx;
    gy = ov->render_y + (double)hy;
    gw = (double)hw;
    gh = (double)hh;
  }
  ov->map_dx = gx - ov->xview;
  ov->map_dy = gy - ov->yview;
  /* With an always-wide gameplay rectangle, retain the nominal native-to-forced translation even
   * while the render camera is clamped at a room edge. Otherwise the edge clamp collapses the
   * translation to zero; the first positive camera write is then hidden behind the half-margin and
   * reappears as a jump when it finally crosses that margin. A target-centered view does not use
   * the native camera origin and therefore keeps its direct mapping. */
  if (aspect_wide_gameplay_view_gen(engine) && !aspect_center_view_target_gen(engine)) {
    if (forced_x && g_w != g_base_w) ov->map_dx = g_aspect_cam_dx;
    if (forced_y && g_h != g_base_h) ov->map_dy = g_aspect_cam_dy;
  }
  gml_set_global_arr(&g_vm, "view_xview", 0, gx);
  gml_set_global_arr(&g_vm, "view_yview", 0, gy);
  if (ov->wview > 0.0) gml_set_global_arr(&g_vm, "view_wview", 0, gw);
  if (ov->hview > 0.0) gml_set_global_arr(&g_vm, "view_hview", 0, gh);
  if (ov->wport > 0.0) gml_set_global_arr(&g_vm, "view_wport", 0, gw);
  if (ov->hport > 0.0) gml_set_global_arr(&g_vm, "view_hport", 0, gh);
}
static void aspect_view_overlay_end(AnygmEngine *engine,AspectViewOverlay *ov, int preserve_camera_writes) {
  if (!ov->active) return;
  if (g_vm.room_index != ov->room) return;
  double raw_x = ov->xview, raw_y = ov->yview;
  if (preserve_camera_writes) {
    raw_x = gml_global_arr(&g_vm, "view_xview", 0) - ov->map_dx;
    raw_y = gml_global_arr(&g_vm, "view_yview", 0) - ov->map_dy;
  }
  gml_set_global_arr(&g_vm, "view_xview", 0, raw_x);
  gml_set_global_arr(&g_vm, "view_yview", 0, raw_y);
  gml_set_global_arr(&g_vm, "view_wview", 0, ov->wview);
  gml_set_global_arr(&g_vm, "view_hview", 0, ov->hview);
  gml_set_global_arr(&g_vm, "view_wport", 0, ov->wport);
  gml_set_global_arr(&g_vm, "view_hport", 0, ov->hport);
}
static void aspect_draw_event_hook(GmlVM *vm, GmlInstance *in, const char *suffix,
                                   int begin, void *user) {
  AnygmEngine *engine=user;
  GmlVM *ctx = vm ? vm : &g_vm;
  if (!begin) {
    if (g_aspect_event_view_overflow > 0) {
      g_aspect_event_view_overflow--;
      return;
    }
    if (g_aspect_event_view_sp <= 0) return;
    AspectEventViewOverlay ov = g_aspect_event_view_stack[--g_aspect_event_view_sp];
    if (!ov.active || ctx->room_index != ov.room) return;
    gml_set_global_arr(ctx, "view_xview", 0, ov.xview);
    gml_set_global_arr(ctx, "view_yview", 0, ov.yview);
    gml_set_global_arr(ctx, "view_wview", 0, ov.wview);
    gml_set_global_arr(ctx, "view_hview", 0, ov.hview);
    gml_set_global_arr(ctx, "view_wport", 0, ov.wport);
    gml_set_global_arr(ctx, "view_hport", 0, ov.hport);
    return;
  }

  if (g_aspect_event_view_sp >= ASPECT_EVENT_VIEW_STACK_MAX) {
    g_aspect_event_view_overflow++;
    return;
  }
  AspectEventViewOverlay ov;
  memset(&ov, 0, sizeof(ov));
  int custom_draw = GMC_ASPECT_DRAW_DEFAULT;
  if (g_aspect_force_active && g_aspect_draw_full_context)
    custom_draw = aspect_draw_full_view_gen(engine,in, suffix);
  if (g_aspect_force_active && g_aspect_draw_full_context &&
      custom_draw != GMC_ASPECT_DRAW_DEFAULT) {
    ov.active = 1;
    ov.room = ctx->room_index;
    ov.xview = gml_global_arr(ctx, "view_xview", 0);
    ov.yview = gml_global_arr(ctx, "view_yview", 0);
    ov.wview = gml_global_arr(ctx, "view_wview", 0);
    ov.hview = gml_global_arr(ctx, "view_hview", 0);
    ov.wport = gml_global_arr(ctx, "view_wport", 0);
    ov.hport = gml_global_arr(ctx, "view_hport", 0);
    gml_set_global_arr(ctx, "view_xview", 0, g_aspect_draw_full_x);
    gml_set_global_arr(ctx, "view_yview", 0, g_aspect_draw_full_y);
    gml_set_global_arr(ctx, "view_wview", 0, (double)g_w);
    gml_set_global_arr(ctx, "view_hview", 0, (double)g_h);
    gml_set_global_arr(ctx, "view_wport", 0, (double)g_w);
    gml_set_global_arr(ctx, "view_hport", 0, (double)g_h);
    if (custom_draw == GMC_ASPECT_DRAW_FULL_VIEW_BACKDROP)
      gml_render_set_pending_fill(&g_render, 0);
  }
  g_aspect_event_view_stack[g_aspect_event_view_sp++] = ov;
}
static void sync_room_fps(AnygmEngine *engine,int publish_changes) {
  int room = g_vm.room_index;
  double fps = cur_room_fps(engine);
  unsigned nw, nh; cur_room_res(engine,&nw, &nh);
  unsigned pw = g_out_w, ph = g_out_h;
  unsigned prev_w = g_w, prev_h = g_h;
  g_w = nw; g_h = nh;
  compute_present(engine);
  if (room == g_fps_room && fps == g_fps && nw == prev_w && nh == prev_h &&
      g_out_w == pw && g_out_h == ph) { g_fps_room = room; return; }
  g_fps_room = room;
  int fps_changed = (fps != g_fps);
  int geom_changed = (nw != prev_w) || (nh != prev_h) || (g_out_w != pw) || (g_out_h != ph);
  int changed = fps_changed || geom_changed;
  if(changed && anygm_host_development_setting(&g_host,"GML_LOG_AV"))
    engine_logf(engine,ANYGM_LOG_DEBUG, "[av] room=%d bytecode=%u fps=%.2f size=%ux%u out=%ux%u window=%dx%d gui=%dx%d\n",
      room, (unsigned)g_win.bytecode, fps, nw, nh, g_out_w, g_out_h,
      g_vm.window_w,g_vm.window_h,g_vm.gui_w,g_vm.gui_h);
  if (fps_changed) g_audio_acc = 0.0;
  g_fps = fps;
  if(changed && publish_changes){
    if(fps_changed) g_frame_flags|=ANYGM_FRAME_TIMING_CHANGED;
    if(geom_changed) g_frame_flags|=ANYGM_FRAME_GEOMETRY_CHANGED;
  }
}

static uint32_t cur_room_bg(AnygmEngine *engine) {
  GmlRoom r;
  if (anygm_policy_uses_classic_runtime(&g_win)) return gm_to_xrgb(g_win.classic_outside_color);
  if (gml_room_get(&g_win, g_vm.room_index, &r) == 0) return gm_to_xrgb(r.bgcolor);
  return 0;
}
static void draw_runtime_backgrounds(AnygmEngine *engine,int want_fg) {
  for (int i = 0; i < 8; i++) {
    int visible = gml_global_arr(&g_vm, "background_visible", i) >= 0.5;
    int fg = gml_global_arr(&g_vm, "background_foreground", i) >= 0.5;
    int bg = (int)gml_global_arr(&g_vm, "background_index", i);
    if (!visible || fg != want_fg || bg < 0) continue;
    int ht = gml_global_arr(&g_vm, "background_htiled", i) >= 0.5;
    int vt = gml_global_arr(&g_vm, "background_vtiled", i) >= 0.5;
    double alpha = gml_global_arr(&g_vm, "background_alpha", i);
    if (alpha <= 0) alpha = 1.0;
    gml_draw_background_tiled_ext(&g_render, bg,
                                  gml_global_arr(&g_vm, "background_x", i),
                                  gml_global_arr(&g_vm, "background_y", i),
                                  1.0, 1.0, 0xFFFFFF, alpha, ht, vt);
  }
}
static int aspect_visible_room_rect(AnygmEngine *engine,double cam_x, double cam_y,
                                    int *out_x, int *out_y, int *out_w, int *out_h) {
  if (!g_aspect_force_active || !g_w || !g_h) return 0;
  GmlRoom rm;
  if (gml_room_get(&g_win, g_vm.room_index, &rm) != 0) return 0;
  if (rm.width == 0 || rm.height == 0) return 0;
  if (rm.width >= g_w && rm.height >= g_h) return 0;

  int x0 = 0, y0 = 0, x1 = (int)g_w, y1 = (int)g_h;
  if (rm.width < g_w) {
    x0 = (int)lround(-cam_x);
    x1 = x0 + (int)rm.width;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)g_w) x1 = (int)g_w;
  }
  if (rm.height < g_h) {
    y0 = (int)lround(-cam_y);
    y1 = y0 + (int)rm.height;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)g_h) y1 = (int)g_h;
  }
  if (out_x) *out_x = x0;
  if (out_y) *out_y = y0;
  if (out_w) *out_w = x1 - x0;
  if (out_h) *out_h = y1 - y0;
  return 1;
}
static void aspect_mask_outside_room(AnygmEngine *engine,double cam_x, double cam_y) {
  int x0, y0, rw, rh;
  if (!aspect_visible_room_rect(engine,cam_x, cam_y, &x0, &y0, &rw, &rh)) return;
  int x1 = x0 + rw, y1 = y0 + rh;

  const uint32_t black = 0xFF000000u;
  int have_room_rect = rw > 0 && rh > 0;
  for (unsigned yy = 0; yy < g_h; yy++) {
    uint32_t *row = g_fb + (size_t)yy * g_w;
    if (!have_room_rect || (int)yy < y0 || (int)yy >= y1) {
      for (unsigned xx = 0; xx < g_w; xx++) row[xx] = black;
      continue;
    }
    for (int xx = 0; xx < x0; xx++) row[xx] = black;
    for (unsigned xx = (unsigned)x1; xx < g_w; xx++) row[xx] = black;
  }
  g_render.fb_all_transparent = 0;
}

static void setup_display(AnygmEngine *engine) {
  /* fixed framebuffer = native display size; rooms are viewed through it */
  g_w = g_win.disp_w ? g_win.disp_w : 288;
  g_h = g_win.disp_h ? g_win.disp_h : 216;
  if (g_w > FB_MAX_W) g_w = FB_MAX_W;
  if (g_h > FB_MAX_H) g_h = FB_MAX_H;
}

static void compose_view_rect(const uint32_t *src, int sw, int sh,
                              uint32_t *dst, int dw, int dh,
                              int dx, int dy, int rw, int rh) {
  if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || rw <= 0 || rh <= 0) return;
  int x0 = dx < 0 ? 0 : dx, y0 = dy < 0 ? 0 : dy;
  int x1 = dx + rw, y1 = dy + rh;
  if (x1 > dw) x1 = dw;
  if (y1 > dh) y1 = dh;
  if (x0 >= x1 || y0 >= y1) return;
  if (sw == rw && sh == rh) {
    int sx = x0 - dx;
    for (int y = y0; y < y1; y++)
      memcpy(dst + (size_t)y * dw + x0,
             src + (size_t)(y - dy) * sw + sx,
             (size_t)(x1 - x0) * sizeof(uint32_t));
    return;
  }
  /* View ports use a point-sampled fixed-function path when interpolation is off.
   * Raster samples live at pixel centres: floor((dst + 0.5) * src / dst_size).  Anchoring the
   * ratio at the destination edge instead shifts non-integer scales by one pixel at many colour
   * boundaries (integer multiples happen to conceal the error). */
  for (int y = y0; y < y1; y++) {
    int sy = (int)(((int64_t)(2 * (y - dy) + 1) * sh) / ((int64_t)2 * rh));
    if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
    uint32_t *drow = dst + (size_t)y * dw;
    const uint32_t *srow = src + (size_t)sy * sw;
    for (int x = x0; x < x1; x++) {
      int sx = (int)(((int64_t)(2 * (x - dx) + 1) * sw) / ((int64_t)2 * rw));
      if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
      drow[x] = srow[sx];
    }
  }
}

/* Render every visible GMS view once, in view-index order, then composite each camera into its
 * declared port on the application surface.  The single-view path stays untouched for the common
 * case; this branch pays the extra pass only for games that actually expose multiple viewports. */
static int render_multiview_application(AnygmEngine *engine) {
  GmlPresentView views[8]; int canvas_w = 0, canvas_h = 0;
  int count = present_view_count(engine,views, &canvas_w, &canvas_h);
  if (count <= 1 || canvas_w != (int)g_w || canvas_h != (int)g_h) return 0;
  memset(g_fb, 0, (size_t)g_w * g_h * sizeof(uint32_t));
  for (unsigned i = 0; i < g_w * g_h; i++) g_fb[i] = 0xFF000000u;

  GmlRoom rm;
  int have_room = gml_room_get(&g_win, g_vm.room_index, &rm) == 0;
  const char *bg_renderer = anygm_host_development_setting(&g_host,"GML_BG_RENDERER_OBJ");
  int pobj = (bg_renderer && *bg_renderer) ? gml_object_index_by_name(&g_vm, bg_renderer) : -1;
  int gml_draws_bg = pobj >= 0 && gml_find_instance(&g_vm, pobj) != NULL;

  for (int i = 0; i < count; i++) {
    GmlPresentView *v = &views[i];
    int vw = (int)lround(v->w), vh = (int)lround(v->h);
    if (vw <= 0 || vh <= 0 || vw > FB_MAX_W || vh > FB_MAX_H) continue;
    *gml_varmap_put(&g_vm.globals, "view_current") = vreal(v->index);
    g_render.app_phase_y = NULL;
    g_render.app_interp_phase[0] = NULL;
    g_render.app_interp_phase[1] = NULL;
    g_render.app_interp_phase[2] = NULL;
    gml_render_begin(&g_render, g_screen, vw, vh, v->x, v->y);
    gml_render_set_pending_fill(&g_render, g_bg);
    gml_vm_draw_pass(&g_vm, "Draw_76");
    if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,0);
    gml_vm_draw_pass(&g_vm, "Draw_72");
    gml_vm_draw(&g_vm);
    gml_vm_draw_pass(&g_vm, "Draw_73");
    if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,1);
    gml_render_flush_pending_fill(&g_render);
    compose_view_rect(g_screen, vw, vh, g_fb, (int)g_w, (int)g_h,
                      v->px, v->py, v->pw, v->ph);
  }
  *gml_varmap_put(&g_vm.globals, "view_current") = vreal(7);
  g_render.fb_all_transparent = 0;
  g_render.fb_all_opaque = 1;
  g_render.fb_opaque_known = 1;
  if (anygm_host_development_setting(&g_host,"GML_LOG_VIEW")) {
    if (g_diag.multiview_frame != g_vm.frame && (g_vm.frame < 4 || g_vm.frame % 120 == 0)) {
      g_diag.multiview_frame = g_vm.frame;
      engine_logf(engine,ANYGM_LOG_DEBUG, "[multiview] f%ld views=%d canvas=%dx%d", g_vm.frame, count, canvas_w, canvas_h);
      for (int i = 0; i < count; i++) engine_logf(engine,ANYGM_LOG_DEBUG, " v%d cam=%d world=(%.0f,%.0f %.0fx%.0f) port=(%d,%d %dx%d)",
        views[i].index, views[i].camera, views[i].x, views[i].y, views[i].w, views[i].h,
        views[i].px, views[i].py, views[i].pw, views[i].ph);
      engine_logf(engine,ANYGM_LOG_DEBUG,"\n");
    }
  }
  return 1;
}

static void content_router_log(void *userdata,int level,const char *message){
  AnygmEngine *engine=userdata;
  AnygmLogLevel mapped=level==ANYGM_CONTENT_LOG_ERROR?ANYGM_LOG_ERROR:
                       level==ANYGM_CONTENT_LOG_WARN?ANYGM_LOG_WARN:ANYGM_LOG_INFO;
  engine_logf(engine,mapped,"[anygm] %s\n",message?message:"");
}

static void boot_runtime(AnygmEngine *engine);

static AnygmResult engine_load_content(AnygmEngine *engine,const AnygmContentSource *source,
                                       const AnygmLoadConfig *config) {
  g_state_just_loaded = 0;
  int path_source=source && source->kind==ANYGM_CONTENT_PATH;
  int memory_source=source && source->kind==ANYGM_CONTENT_MEMORY;
  if((path_source && (!source->path || !source->path[0])) ||
     (memory_source && (!source->data || !source->size)) ||
     (!path_source && !memory_source)){
    engine_errorf(engine,ANYGM_ERROR_INVALID_ARGUMENT,"A readable path or memory image is required");
    return ANYGM_ERROR_INVALID_ARGUMENT;
  }
  snprintf(g_language,sizeof g_language,"%s",config&&config->language&&config->language[0]?config->language:"en");
  snprintf(g_region,sizeof g_region,"%s",config&&config->region&&config->region[0]?config->region:"us");
  snprintf(g_language_tag,sizeof g_language_tag,"%s",config&&config->language_tag&&config->language_tag[0]?config->language_tag:"en-US");
  char loaded_path[1024]={0};
  int load_rc=0;
  int classic_input=0;
  if(path_source){
    char content[1024];
    size_t plen=strlen(source->path);
    classic_input=
        (plen>4 && !strcasecmp(source->path+plen-4,".gmk")) ||
        (plen>5 && !strcasecmp(source->path+plen-5,".gm81")) ||
        (plen>4 && !strcasecmp(source->path+plen-4,".gm6")) ||
        (plen>4 && !strcasecmp(source->path+plen-4,".exe"));
    AnygmContentRouter router={0};
    router.host=&g_host;
    router.cache_directory=source->cache_directory;
    router.log=content_router_log;
    router.log_userdata=engine;
    if(!anygm_content_resolve_path(&router,source->path,content,sizeof content)){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Failed to resolve content path: %s",source->path);
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    load_rc=anygm_content_load_win(&router,&g_win,content,loaded_path,sizeof loaded_path);
    if(!load_rc){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"Failed to load content: %s",content);
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    if(load_rc==2)
      engine_logf(engine,ANYGM_LOG_WARN,
                  "The selected payload has no executable code; using sibling payload: %s\n",
                  loaded_path);
    if(classic_input)
      anygm_content_path_parent(source->path,g_win.content_dir,sizeof g_win.content_dir);
  } else {
    if(gml_win_from_mem(&g_win,(uint8_t *)(uintptr_t)source->data,source->size,0)!=0){
      engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,
                    "The memory source is not a supported normalized content image");
      return ANYGM_ERROR_INVALID_CONTENT;
    }
    g_win.host=&g_host;
    snprintf(loaded_path,sizeof loaded_path,"%s",
             source->path&&source->path[0]?source->path:"memory image");
    if(source->path&&source->path[0])
      anygm_content_path_parent(source->path,g_win.content_dir,sizeof g_win.content_dir);
  }
  /* Give each content path a stable writable namespace under the host-provided root. */
  {
    const char *base=source->save_directory;
    if(base){
      const char *identity=source->path&&source->path[0]?source->path:NULL;
      char stem[128];
      if(identity) anygm_content_path_stem(identity,stem,sizeof stem);
      else snprintf(stem,sizeof stem,"memory");
      for(char *c=stem;*c;c++)
        if(!(isalnum((unsigned char)*c) || *c=='-' || *c=='_' || *c=='.')) *c='_';
      if(!stem[0]) snprintf(stem,sizeof stem,"content");
      uint32_t namespace_hash=identity?anygm_content_path_hash(identity):
        (uint32_t)state_hash_bytes(source->data,source->size);
      snprintf(g_win.save_dir,sizeof g_win.save_dir,"%s/%s-%08x-anygm-save",
               base,stem,namespace_hash);
      anygm_content_directory_create(&g_host,g_win.save_dir);
    } else {
      snprintf(g_win.save_dir,sizeof g_win.save_dir,"%s",g_win.content_dir);
    }
  }
  if (g_win.n_code <= 0) {
    engine_errorf(engine,ANYGM_ERROR_INVALID_CONTENT,"The payload has no executable code: %s",loaded_path);
    gml_win_free(&g_win);
    return ANYGM_ERROR_INVALID_CONTENT;
  }
  char compatibility_error[256]={0};
  if(!anygm_content_facts_detect(&g_win,&g_content_facts,compatibility_error,
                                 sizeof compatibility_error) ||
     !anygm_compatibility_resolve(&g_content_facts,&g_compatibility,compatibility_error,
                                  sizeof compatibility_error)){
    engine_errorf(engine,ANYGM_ERROR_UNSUPPORTED,"Unsupported content semantics: %s",
                  compatibility_error[0]?compatibility_error:"unknown compatibility facts");
    gml_win_free(&g_win);
    return ANYGM_ERROR_UNSUPPORTED;
  }
  g_win.compatibility=&g_compatibility;
  state_identity_refresh(engine);
  g_loaded = 1;
  engine_logf(engine,ANYGM_LOG_INFO,"Loaded content: bytecode=%u rooms=%d code=%d\n",
              g_win.bytecode,gml_room_count(&g_win),g_win.n_code);
  g_full_game_on_initial_boot=0;
  boot_runtime(engine);
  run_selftest(engine);
  engine_logf(engine,ANYGM_LOG_INFO,"Runtime booted: atlases=%d sprites=%d texture-pages=%d\n",
              g_render.n_atlas,g_render.n_spr,g_render.n_tpag);
  return ANYGM_OK;
}
/* Cold-boot the runtime from the already-loaded data.win: fresh VM/render/audio + the same
 * configured start-room logic as first load. */
static void boot_runtime(AnygmEngine *engine) {
  /* A reset is a cold boot. Keep engine time on the same timeline as an initial load. */
  { g_vm.frame = 0; }
  g_state_reapply_size = 0;
  memset(g_pad_cur, 0, sizeof(g_pad_cur));
  memset(g_pad_prev, 0, sizeof(g_pad_prev));
  memset(g_axis_cur, 0, sizeof(g_axis_cur));
  memset(g_axis_prev, 0, sizeof(g_axis_prev));
  memset(g_key_cur, 0, sizeof(g_key_cur));
  memset(g_key_prev, 0, sizeof(g_key_prev));
  memset(g_hw_key_cur, 0, sizeof(g_hw_key_cur));
  memset(g_hw_key_prev, 0, sizeof(g_hw_key_prev));
  memset(g_evt_vk_cur, 0, sizeof(g_evt_vk_cur));
  memset(g_evt_vk_prev, 0, sizeof(g_evt_vk_prev));
  memset(g_evt_key_cur, 0, sizeof(g_evt_key_cur));
  memset(g_evt_key_prev, 0, sizeof(g_evt_key_prev));
  g_mouse_px = -1; g_mouse_py = -1;
  g_pointer_active = 0;
  memset(g_mb_cur, 0, sizeof(g_mb_cur));
  memset(g_mb_prev, 0, sizeof(g_mb_prev));
  g_mouse_wheel = 0;
  g_present_mouse_valid = 0;
  g_follow_player = 0; g_player_obj = -1; g_audio_acc = 0.0; g_fps = 60.0; g_fps_room = -1;
  g_state_just_loaded = 0;
  g_runtime_ended = 0;
  g_shutdown_sent = 0;
  classic_transition_reset(engine);
  g_have_presented_frame = 0;
  setup_display(engine);
  gml_vm_init(&g_vm, &g_win,&g_host);
  g_vm.input.userdata=engine;
  g_vm.input.key=engine_input_key;
  g_vm.input.key_clear=engine_input_key_clear;
  g_vm.input.key_press=engine_input_key_press;
  g_vm.input.key_release=engine_input_key_release;
  g_vm.input.gamepad=engine_input_gamepad;
  g_vm.input.gamepad_connected=engine_input_gamepad_connected;
  g_vm.input.gamepad_device_count=engine_input_gamepad_device_count;
  g_vm.input.gamepad_axis=engine_input_gamepad_axis;
  g_vm.input.gamepad_vibration=engine_input_gamepad_set_vibration;
  g_vm.input.mouse=engine_input_mouse;
  g_vm.input.mouse_set=engine_input_mouse_set;
  setup_platform_locale(engine,&g_vm);
  gml_render_init(&g_render, &g_win);
  /* application_surface exists before the first Create event. GML may resize it there; the
   * renderer promotes the borrowed framebuffer to an independently owned surface when that
   * happens. */
  g_render.app_surface = g_fb;
  g_render.app_w = (int)g_w;
  g_render.app_h = (int)g_h;
  g_render.resolution_w = core_opt_resolution(engine,0);
  g_render.resolution_h = core_opt_resolution(engine,1);
  g_render.crt_shader_enable = core_opt_embedded_shaders(engine);
  g_render.crt_mask_enable = core_opt_crt_mask(engine);
  g_render.crt_scanlines_enable = core_opt_onoff(engine,"anygm_crt_scanlines", "ANYGM_CRT_SCANLINES", 1);
  g_render.crt_gamma_enable     = core_opt_onoff(engine,"anygm_crt_gamma",     "ANYGM_CRT_GAMMA",     1);
  g_render.crt_curvature        = core_opt_crt_tristate(engine,"anygm_crt_curvature", "ANYGM_CRT_CURVATURE");
  g_render.crt_vignette         = core_opt_crt_tristate(engine,"anygm_crt_vignette",  "ANYGM_CRT_VIGNETTE");
  g_vm.render = &g_render;
  g_vm.draw_event_hook = aspect_draw_event_hook;
  g_vm.draw_event_hook_user = engine;
  g_audio = gml_audio_create(&g_win);
  g_vm.audio = g_audio;
  /* Boot the normal entry point unless the host supplied a neutral start-room override. */
  int start_order=0,selected_room=-1,spawn=0;
  double spawn_x=64.0,spawn_y=100.0;
  char spawn_object[128]={0};
  const char *spawn_setting=anygm_host_development_setting(&g_host,"GML_SPAWN_OBJ");
  if(spawn_setting && spawn_setting[0]){
    spawn=1;
    const char *colon=strchr(spawn_setting,':');
    size_t length=colon?(size_t)(colon-spawn_setting):strlen(spawn_setting);
    if(length>=sizeof spawn_object) length=sizeof spawn_object-1;
    memcpy(spawn_object,spawn_setting,length);
    if(colon){
      const char *comma=strchr(colon+1,',');
      if(comma){ spawn_x=atof(colon+1); spawn_y=atof(comma+1); }
    }
  }else{
    const char *position=anygm_host_development_setting(&g_host,"GML_SPAWN_PLAYER");
    const char *object=anygm_host_development_setting(&g_host,"GML_SPAWN_PLAYER_OBJ");
    if(position && object && object[0]){
      spawn=1;
      snprintf(spawn_object,sizeof spawn_object,"%s",object);
      const char *comma=strchr(position,',');
      if(comma){ spawn_x=atof(position); spawn_y=atof(comma+1); }
    }
  }
  int initial_boot_guard = g_full_game_on_initial_boot;
  if(!initial_boot_guard) core_opt_start_room(engine,&selected_room);
  g_full_game_on_initial_boot = 0;
  /* always run the first room before a selected start room so game globals, fonts, and
   * input-controller instances are initialized. */
  if (start_order != 0 || selected_room >= 0) gml_vm_goto_room_order(&g_vm, 0);
  if (selected_room >= 0) gml_room_enter(&g_vm, selected_room);
  else gml_vm_goto_room_order(&g_vm, start_order);
  sync_room_fps(engine,0);
  g_vm.god_mode = core_opt_god(engine);
  if(spawn && spawn_object[0]){
    g_player_obj=gml_object_index_by_name(&g_vm,spawn_object);
    const char *suppress=anygm_host_development_setting(&g_host,"GML_SPAWN_SUPPRESS");
    if(suppress && suppress[0]) for(int i=0;i<g_vm.inst_count;i++){
      GmlInstance *instance=&g_vm.inst[i];
      if(!instance->active || instance->obj<0) continue;
      if(substr_list_match(suppress,g_vm.objects[instance->obj].name)) instance->active=0;
    }
    if(g_player_obj>=0){
      GmlInstance *player=NULL;
      for(int i=0;i<g_vm.inst_count;i++){
        GmlInstance *instance=&g_vm.inst[i];
        if(!instance->active || instance->marked || instance->obj<0 ||
           !gml_object_is(&g_vm,instance->obj,g_player_obj)) continue;
        if(!player) player=instance;
        else instance->active=0;
      }
      if(!player) player=gml_instance_create(&g_vm,spawn_x,spawn_y,g_player_obj);
      if(player){
        player->x=player->xprevious=player->xstart=spawn_x;
        player->y=player->yprevious=player->ystart=spawn_y;
        player->hspeed=player->vspeed=player->speed=0.0;
      }
      g_follow_player=1;
    }
    engine_logf(engine,ANYGM_LOG_INFO,
                "Development spawn configured object %d at %.3f,%.3f\n",
                g_player_obj,spawn_x,spawn_y);
  }
  g_bg = cur_room_bg(engine);
}
static void engine_unload(AnygmEngine *engine){
  if(!g_loaded) return;
  profile_report(engine,1);
  gml_audio_free(g_audio); g_audio=NULL; g_vm.audio=NULL;
  gml_vm_free(&g_vm);
  gml_render_free(&g_render);
  gml_win_free(&g_win);
  classic_transition_release(engine);
  g_have_presented_frame=0; g_audio_acc=0.0; g_fps=60.0; g_fps_room=-1;
  g_state_just_loaded=0; g_state_reapply_size=0;
  g_content_fingerprint=0; g_compatibility_fingerprint=0;
  g_runtime_ended=0; g_shutdown_sent=0; g_loaded=0;
}

/* Apply the current neutral configuration before the frame. */
static void poll_option_updates(AnygmEngine *engine) {
  g_render.resolution_w         = core_opt_resolution(engine,0);
  g_render.resolution_h         = core_opt_resolution(engine,1);
  g_render.crt_shader_enable    = core_opt_embedded_shaders(engine);
  g_render.crt_mask_enable      = core_opt_crt_mask(engine);
  g_render.crt_scanlines_enable = core_opt_onoff(engine,"anygm_crt_scanlines", "ANYGM_CRT_SCANLINES", 1);
  g_render.crt_gamma_enable     = core_opt_onoff(engine,"anygm_crt_gamma",     "ANYGM_CRT_GAMMA",     1);
  g_render.crt_curvature        = core_opt_crt_tristate(engine,"anygm_crt_curvature", "ANYGM_CRT_CURVATURE");
  g_render.crt_vignette         = core_opt_crt_tristate(engine,"anygm_crt_vignette",  "ANYGM_CRT_VIGNETTE");
}
/* Fast-forward is an optimization hint only. */
static void poll_fast_forward(AnygmEngine *engine) {
  g_render.crt_ff=g_config.fast_forward?1:0;
}
static AnygmResult engine_run_frame(AnygmEngine *engine) {
  g_audio_frames=0;
  /* Keep supplying the last completed picture and silence after the shutdown request without
   * advancing Step, Draw, particles, or animation. */
  if(g_runtime_ended){
    memset(g_audio_output,0,sizeof g_audio_output);
    g_audio_acc+=44100.0/(g_fps>0.0?g_fps:60.0);
    int nf=(int)g_audio_acc; g_audio_acc-=nf;
    if(nf>4096) nf=4096;
    if(nf>0) g_audio_frames=(size_t)nf;
    g_frame_flags|=ANYGM_FRAME_SHUTDOWN_REQUESTED;
    return ANYGM_OK;
  }
  poll_option_updates(engine);   /* live resolution/CRT option changes (no restart) */
  poll_fast_forward(engine);     /* optimization hint; never changes presentation state */
  int prof = profile_enabled(engine);
  double t_total = prof ? profile_now_ms(engine) : 0.0;
  double sp_step = g_prof.step_ms, sp_draw = g_prof.draw_ms, sp_gui = g_prof.gui_ms, sp_video = g_prof.video_ms;
  double t0 = t_total, t1 = t_total;
  /* Snapshot neutral input, retaining the previous frame for edge detection. */
  memcpy(g_pad_prev, g_pad_cur, sizeof(g_pad_cur));
  memcpy(g_key_prev, g_key_cur, sizeof(g_key_cur));
  memcpy(g_axis_prev, g_axis_cur, sizeof(g_axis_cur));
  for (int b = 0; b < NPAD; b++)
    g_pad_cur[b]=g_input.gamepad_buttons[0][b]?1:0;
  poll_keyboard(engine);
  for(int axis=0;axis<4;axis++) g_axis_cur[axis]=g_input.gamepad_axes[0][axis];
  poll_mouse(engine);
  int room_before_step = g_vm.room_index;
  unsigned transition_old_w = g_out_w ? g_out_w : g_w;
  unsigned transition_old_h = g_out_h ? g_out_h : g_h;
  int state_restore_frame = g_state_just_loaded;
  int run_step = !state_restore_frame && !g_classic_transition.active;
  if(state_restore_frame){
    memset(g_pad_cur, 0, sizeof(g_pad_cur));
    memset(g_pad_prev, 0, sizeof(g_pad_prev));
    memset(g_key_cur, 0, sizeof(g_key_cur));
    memset(g_key_prev, 0, sizeof(g_key_prev));
    memset(g_hw_key_cur, 0, sizeof(g_hw_key_cur));
    memset(g_hw_key_prev, 0, sizeof(g_hw_key_prev));
    memset(g_evt_vk_cur, 0, sizeof(g_evt_vk_cur));
    memset(g_evt_vk_prev, 0, sizeof(g_evt_vk_prev));
    memset(g_evt_key_cur, 0, sizeof(g_evt_key_cur));
    memset(g_evt_key_prev, 0, sizeof(g_evt_key_prev));
    memset(g_axis_cur, 0, sizeof(g_axis_cur));
    memset(g_axis_prev, 0, sizeof(g_axis_prev));
    memset(g_mb_cur, 0, sizeof(g_mb_cur));
    memset(g_mb_prev, 0, sizeof(g_mb_prev));
    g_mouse_wheel = 0;
  }
  if (anygm_host_development_setting(&g_host,"GML_DBG_PAD")) { g_diag.pad_frame++;
    unsigned bits = 0; for (int b = 0; b < NPAD; b++) if (g_pad_cur[b]) bits |= 1u << b;
    if (bits) engine_logf(engine,ANYGM_LOG_DEBUG, "[pad] f%d bits=%04x\n", g_diag.pad_frame, bits); }
  if(prof){ t1 = profile_now_ms(engine); g_prof.input_ms += t1 - t0; t0 = t1; }
  g_state_just_loaded = 0;
  sync_room_fps(engine,1);
  aspect_apply_program(engine);
  sync_room_fps(engine,1);
  /* For a full-width compositor under a forced-wide aspect, report widened window dimensions so
   * its CRT surface spans the whole frame. Set before
   * the step so the window-size-change trigger fires with the (already-widened) view in scope. */
  { int fw = g_aspect_force_active && aspect_compositor_fullwidth_gen(engine)
             && ((g_base_w > 0 && (int)g_base_w < (int)g_w) || (g_base_h > 0 && (int)g_base_h < (int)g_h));
    g_render.aspect_fullwidth = fw;
    g_render.aspect_wide_w = fw ? (int)g_w : 0;
    g_render.aspect_wide_h = fw ? (int)g_h : 0; }
  if(run_step){
    AspectViewOverlay step_ov;
    aspect_view_overlay_begin(engine,&step_ov, 0, ASPECT_VIEW_TRACKING);
    gml_vm_step(&g_vm);
    if(anygm_policy_uses_classic_runtime(&g_win) && room_before_step>=0 && g_vm.room_index!=room_before_step){
      int kind=(int)lround(gml_global_num(&g_vm,"transition_kind"));
      int steps=(int)lround(gml_global_num(&g_vm,"transition_steps"));
      if(kind!=21 || !classic_transition_start(engine,transition_old_w,transition_old_h,steps))
        gml_set_global_scalar(&g_vm,"transition_kind",0);
    }
    aspect_view_overlay_end(engine,&step_ov, 1);
    sync_room_fps(engine,1);
    apply_sticky_cheats(engine);   /* generic freeze cheats (user-supplied global writes) */
    menu_run(engine);              /* generic pause-menu editor (inject entries + handle input) */
    room_skip_hook(engine);        /* generic room-skip button (Select/Start, any room) */
    introskip_hook(engine);        /* A/B skip, only in a user-supplied intro-room list (GML_INTROSKIP) */
  }
  /* The VM has already fired the final event; translate its lifecycle request to engine output. */
  if(g_vm.game_end){
    if(g_vm.game_end == 2){
      g_vm.game_end = 0;
      boot_runtime(engine);
    } else {
      g_runtime_ended = 1;
      if(!g_shutdown_sent){
        g_shutdown_sent = 1;
        g_frame_flags|=ANYGM_FRAME_SHUTDOWN_REQUESTED;
      }
    }
  }
  if (anygm_host_development_setting(&g_host,"GML_LOG_PLAYER")) {
    const char *log_obj = anygm_host_development_setting(&g_host,"GML_LOG_PLAYER");
    int po = g_player_obj;
    if (log_obj && *log_obj && strcmp(log_obj, "1")) po = gml_object_index_by_name(&g_vm, log_obj);
    GmlInstance *p = po >= 0 ? gml_find_instance(&g_vm, po) : NULL;
    if (p) engine_logf(engine,ANYGM_LOG_DEBUG, "[player] f%d x=%.1f y=%.1f hsp=%.2f vsp=%.2f img=%.1f spr=%d\n",
                   g_diag.player_frame, p->x, p->y, p->hspeed, p->vspeed, p->image_index, (int)p->sprite_index);
    g_diag.player_frame++;
  }
  {
    const char *target = anygm_host_development_setting(&g_host,"GML_LOG_RNG_FRAME");
    if (target && atoi(target) == g_diag.rng_frame)
      engine_logf(engine,ANYGM_LOG_DEBUG, "[rng] seed=%u\n", anygm_policy_uses_classic_runtime(g_vm.win)
        ? g_vm.rng_classic_state : g_vm.rng_state);
    g_diag.rng_frame++;
  }
  {
    const char *needle = anygm_host_development_setting(&g_host,"GML_LOG_OBJ");
    if (needle && *needle) {
      for (int i = 0; i < g_vm.inst_count; i++) {
        GmlInstance *o = &g_vm.inst[i];
        if (!o->active || o->marked || o->obj < 0 || o->obj >= g_vm.n_objects) continue;
        const char *on = g_vm.objects[o->obj].name;
        if (on && (!strcmp(needle, "*") || strstr(on, needle))) {
          char dn[160];
          snprintf(dn, sizeof dn, "gml_Object_%s_Draw_0", on);
          engine_logf(engine,ANYGM_LOG_DEBUG, "[obj] f%d %s obj=%d id=%u x=%.1f y=%.1f hsp=%.2f vsp=%.2f dir=%.1f img=%.1f spr=%d solid=%.0f vis=%.0f draw=%d xs=%.1f al=[%.0f %.0f]\n",
                  g_diag.object_frame, on, o->obj, o->id, o->x, o->y, o->hspeed, o->vspeed, o->direction, o->image_index,
                  (int)o->sprite_index, o->solid, o->visible, gml_code_index_by_name(&g_win, dn) >= 0,
                  o->image_xscale, o->alarm[0], o->alarm[1]);
        }
      }
      g_diag.object_frame++;
    }
  }
  if(prof){ t1 = profile_now_ms(engine); g_prof.step_ms += t1 - t0; t0 = t1; }
  AspectViewOverlay draw_ov;
  if (!g_follow_player) aspect_view_overlay_begin(engine,&draw_ov, 1, ASPECT_VIEW_FORCED);
  else memset(&draw_ov, 0, sizeof(draw_ov));
  g_bg = cur_room_bg(engine);
  double cam_x, cam_y;
  if (g_follow_player) {
    GmlInstance *pl = gml_find_instance(&g_vm, g_player_obj);
    GmlRoom rmc; int hr = (gml_room_get(&g_win, g_vm.room_index, &rmc) == 0);
    if (pl && hr) {
      cam_x = pl->x - g_w / 2.0; cam_y = pl->y - g_h / 2.0;
      clamp_camera_to_current_room(engine,&cam_x, &cam_y, g_aspect_force_active);
    } else { cam_x = cam_y = 0; }
  } else {
    if (draw_ov.active) {
      cam_x = draw_ov.render_x;
      cam_y = draw_ov.render_y;
    } else {
      /* Studio cameras are opaque handles bound through view_camera[].  Their world rectangle
       * lives in the camera resource, while the legacy view_xview/yview arrays may remain at the
       * room defaults forever.  The multiview compositor already resolves this indirection; the
       * common single-view path must use the same resolved rectangle or every world instance is
       * rendered relative to (0,0) even though camera_get_view_x/y report the moving camera. */
      GmlPresentView primary;
      if (present_view_get(engine,0, &primary)) {
        cam_x = primary.x;
        cam_y = primary.y;
      } else {
        cam_x = gml_global_arr(&g_vm, "view_xview", 0);
        cam_y = gml_global_arr(&g_vm, "view_yview", 0);
      }
    }
    if (!draw_ov.active && g_aspect_force_active)
      aspect_forced_camera(engine,cam_x, cam_y, &cam_x, &cam_y);
  }
  if (anygm_host_development_setting(&g_host,"GML_LOG_CAM")) {   /* aligns with the host dump frame index */
    int bound = (int)lround(gml_global_arr(&g_vm, "view_camera", 0));
    int bound_live = bound >= 0 && bound < 64 &&
                     gml_global_arr(&g_vm, "__gml_camera_live", bound) >= 0.5;
    engine_logf(engine,ANYGM_LOG_DEBUG, "[cam] %d %d %d %d base=%ux%u forced=%ux%u delta=%.1f,%.1f view0=%.0f "
                    "bound=%d live=%d resource=(%.0f,%.0f %.0fx%.0f) port=(%.0f,%.0f)\n",
            g_diag.camera_frame++, g_vm.room_index, (int)cam_x, (int)cam_y,
            g_base_w, g_base_h, g_w, g_h, g_aspect_cam_dx, g_aspect_cam_dy,
            gml_global_arr(&g_vm, "view_visible", 0), bound, bound_live,
            bound_live ? gml_global_arr(&g_vm, "__gml_camera_x", bound) : 0.0,
            bound_live ? gml_global_arr(&g_vm, "__gml_camera_y", bound) : 0.0,
            bound_live ? gml_global_arr(&g_vm, "__gml_camera_w", bound) : 0.0,
            bound_live ? gml_global_arr(&g_vm, "__gml_camera_h", bound) : 0.0,
            gml_global_arr(&g_vm, "view_wport", 0), gml_global_arr(&g_vm, "view_hport", 0)); }
  if(prof){ t1 = profile_now_ms(engine); g_prof.clear_ms += t1 - t0; t0 = t1; }
  g_aspect_draw_full_context = g_aspect_force_active;
  g_aspect_draw_full_x = cam_x;
  g_aspect_draw_full_y = cam_y;
  g_aspect_event_view_sp = 0;
  g_aspect_event_view_overflow = 0;
  g_render.fast_alpha_cull = core_opt_fast_alpha_cull(engine);
  if(!g_out_w || !g_out_h) compute_present(engine);
  g_render.app_phase_y=NULL;
  g_render.app_interp_phase[0]=NULL;
  g_render.app_interp_phase[1]=NULL;
  g_render.app_interp_phase[2]=NULL;
  size_t classic_pixels=(size_t)g_w*(size_t)g_h;
  int classic_phase = anygm_policy_classic_modern_presentation(&g_win) && !g_render.interp &&
                      g_out_w==g_w*2 && g_out_h==g_h*2 &&
                      !g_canvas_mode && !g_aspect_force_active &&
                      ensure_classic_phase(engine,classic_pixels);
  int classic_interp_phase = anygm_policy_classic_modern_presentation(&g_win) && g_render.interp &&
                             g_out_w==g_w*2 && g_out_h==g_h*2 &&
                             !g_canvas_mode && !g_aspect_force_active &&
                             classic_pixels<=SIZE_MAX/3 &&
                             ensure_classic_phase(engine,classic_pixels*3);
  int view_surface = (int)gml_global_arr(&g_vm, "view_surface_id", 0);
  int multiview_rendered = !g_aspect_force_active && render_multiview_application(engine);
  if (!multiview_rendered) {
  gml_render_begin(&g_render, g_fb, g_w, g_h, cam_x, cam_y);
  if(classic_phase) g_render.classic_phase_y=g_classic_phase_mem;
  if(classic_interp_phase){
    g_render.classic_interp_phase[0]=g_classic_phase_mem;
    g_render.classic_interp_phase[1]=g_classic_phase_mem+classic_pixels;
    g_render.classic_interp_phase[2]=g_classic_phase_mem+classic_pixels*2;
  }
  gml_render_set_pending_fill(&g_render, g_bg);
  GmlRoom rm;
  int have_room = (gml_room_get(&g_win, g_vm.room_index, &rm) == 0);
  /* Some games draw room backgrounds themselves from GML. When a launcher supplies that renderer
   * object's name, defer to it and avoid double-drawing the engine's static fallback. */
  const char *bg_renderer = anygm_host_development_setting(&g_host,"GML_BG_RENDERER_OBJ");
  int pobj = (bg_renderer && *bg_renderer) ? gml_object_index_by_name(&g_vm, bg_renderer) : -1;
  int gml_draws_bg = (pobj >= 0 && gml_find_instance(&g_vm, pobj) != NULL);
  gml_vm_draw_pass(&g_vm, "Draw_76");   /* Pre-Draw (GMS2 ev 76): before the room render */
  if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,0);
  gml_vm_draw_pass(&g_vm, "Draw_72");   /* Draw Begin */
  gml_vm_draw(&g_vm);   /* instances + room tiles, interleaved by depth */
  gml_vm_draw_pass(&g_vm, "Draw_73");   /* Draw End */
  if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,1);
  g_aspect_draw_full_context = 0;
  g_aspect_event_view_sp = 0;
  g_aspect_event_view_overflow = 0;
  gml_render_flush_pending_fill(&g_render);
  aspect_mask_outside_room(engine,cam_x, cam_y);
  if(prof){ t1 = profile_now_ms(engine); g_prof.draw_ms += t1 - t0; t0 = t1; }
  if(anygm_host_development_setting(&g_host,"GML_LOG_DRAW")){ int nz=0; for(unsigned i=0;i<g_w*g_h;i++) if(g_fb[i]&0xFFFFFF) nz++;
    if(g_diag.draw_frame<3||g_diag.draw_frame%200==0){ int live=0,deactivated=0,dormant=0,highest=-1;
      for(int i=0;i<g_vm.inst_count;i++){ GmlInstance *in=&g_vm.inst[i];
        if(in->active&&!in->marked){ live++; highest=i; }
        else if(in->deactivated){ deactivated++; highest=i; }
        else if(in->room_dormant){ dormant++; highest=i; } }
      engine_logf(engine,ANYGM_LOG_DEBUG,"[draw] f=%d room=%d inst=%d live=%d deact=%d dormant=%d high=%d res=%ux%u fb_nonblack=%d app_draw_en=%d\n",
              g_diag.draw_frame,g_vm.room_index,g_vm.inst_count,live,deactivated,dormant,highest,g_w,g_h,nz,g_render.app_draw_enable); }
    g_diag.draw_frame++; }
  /* Studio view-to-surface: when view 0 targets a surface, mirror the rendered frame into it so
   * Draw GUI can composite the surface. */
  {
    int vs = view_surface;
    if (vs > 0 && gml_surface_exists(&g_render, vs)) {
      uint32_t *saved_app = g_render.app_surface;
      int saved_app_w = g_render.app_w, saved_app_h = g_render.app_h;
      g_render.app_surface = g_fb; g_render.app_w = g_w; g_render.app_h = g_h;
      g_render.app_surface_opaque = g_render.fb_opaque_known && g_render.fb_all_opaque;
      if (gml_surface_set_target(&g_render, vs)) {
        double scx = g_render.cam_x, scy = g_render.cam_y;   /* absolute blit: shield from camera */
        g_render.cam_x = g_render.cam_y = 0;
        gml_draw_surface_stretched(&g_render, 0, 0, 0,
          gml_surface_width(&g_render, vs), gml_surface_height(&g_render, vs), 0xFFFFFF, 1.0);
        g_render.cam_x = scx; g_render.cam_y = scy;
        gml_surface_reset_target(&g_render);
      }
      g_render.app_surface = saved_app;
      g_render.app_w = saved_app_w;
      g_render.app_h = saved_app_h;
      if(anygm_host_development_setting(&g_host,"GML_LOG_SHADER")){
        int sw=0,sh=0; uint32_t *pixels=gml_surface_pixels_read(&g_render,vs,&sw,&sh);
        if(g_diag.shader_count++<8) engine_logf(engine,ANYGM_LOG_DEBUG,"[shader] f%ld mirrored view surface=%d %dx%d center=%08x\n",
          g_vm.frame,vs,sw,sh,pixels&&sw>0&&sh>0?pixels[(size_t)(sh/2)*sw+sw/2]:0);
      }
    }
  }
  }
  /* Draw GUI pass: g_fb is now the application surface. Run Draw GUI events into g_screen so a
   * presentation object can composite the frame and overlays. If nothing draws the surface,
   * auto-blit it. */
  if(g_render.app_surface_owned){
    int aw=g_render.app_w, ah=g_render.app_h;
    uint32_t clear=g_bg;
    for(int i=0;i<aw*ah;i++) g_render.app_surface_owned[i]=clear;
    if(!(view_surface>0 && gml_surface_exists(&g_render,view_surface))){
      GmlPresentView v;
      if(present_view_get(engine,0,&v)){
        int dx=v.px, dy=v.py, dw=v.pw, dh=v.ph;
        /* A sole view which covered the logical application surface before an explicit resize
         * continues to cover the resized surface.  Its room/view coordinates stay logical while
         * the complete viewport scales to the new render resolution. Preserving the
         * old pixel-sized port here instead placed the whole scene in one corner of the larger
         * surface before presentation.  Multi-view and deliberately inset ports retain their
         * authored rectangles. */
        int one_view = present_view_count(engine,NULL,NULL,NULL)==1;
        int port_is_logical = abs(dw-(int)g_w)<=1 && abs(dh-(int)g_h)<=1;
        /* A room can retain a full-window port authored for a larger display after game code has
         * selected a smaller window/application surface (for example 1920x1080 -> 960x540). The
         * full-origin viewport scales with the window; treating its raw pixel size as
         * a literal rectangle inside the smaller application surface samples only the upper-left
         * quadrant and produces a false zoom. Only normalize an oversized, same-aspect, sole view;
         * smaller/inset and multi-view ports keep their authored rectangles. */
        int oversized_full_port = dw>0 && dh>0 && aw>0 && ah>0 &&
                                  dw>=aw && dh>=ah &&
                                  fabs((double)dw/(double)dh-(double)aw/(double)ah)<0.0005;
        int resized_full_port = stale_full_view_port(engine,one_view,dx,dy,dw,dh,
                                                     (int)g_w,(int)g_h,aw,ah);
        int full_logical_view = one_view && dx==0 && dy==0 &&
                                (port_is_logical || oversized_full_port || resized_full_port);
        if(full_logical_view){ dx=dy=0; dw=aw; dh=ah; }
        compose_view_rect(g_fb,(int)g_w,(int)g_h,g_render.app_surface_owned,aw,ah,
                          dx,dy,dw,dh);
      } else
        compose_view_rect(g_fb,(int)g_w,(int)g_h,g_render.app_surface_owned,aw,ah,
                          0,0,aw,ah);
    }
    g_render.app_surface=g_render.app_surface_owned;
    g_render.app_surface_opaque=1;
    g_render.app_phase_y=NULL;
    g_render.app_interp_phase[0]=NULL;
    g_render.app_interp_phase[1]=NULL;
    g_render.app_interp_phase[2]=NULL;
  } else {
    g_render.app_surface = g_fb; g_render.app_w = g_w; g_render.app_h = g_h;
    g_render.app_surface_opaque = g_render.fb_opaque_known && g_render.fb_all_opaque;
  }
  if(!g_render.app_surface_owned && classic_phase) g_render.app_phase_y=g_classic_phase_mem;
  if(!g_render.app_surface_owned && classic_interp_phase){
    g_render.app_interp_phase[0]=g_classic_phase_mem;
    g_render.app_interp_phase[1]=g_classic_phase_mem+classic_pixels;
    g_render.app_interp_phase[2]=g_classic_phase_mem+classic_pixels*2;
  }
  if (!g_out_w || !g_out_h) compute_present(engine);
  unsigned ow = g_out_w, oh = g_out_h;
	  /* GUI target: the output canvas directly when GUI space matches it (canvas mode, or GUI==view);
	   * otherwise draw the GUI at its own dimensions into a scratch buffer and downscale to the
	   * native output. */
		  int aspect_gui_x = 0, aspect_gui_y = 0, aspect_gui_w = 0, aspect_gui_h = 0;
		  int aspect_gui_center = 0;
		  /* Games the aspect hook flags as full-width-compositor opt out of the centered-4:3 GUI pass:
		   * their Draw-GUI compositor runs at the full width instead, so the CRT covers the entire
		   * 16:9/21:9 frame rather than a centered 4:3 sub-rect. */
		  int compositor_fullwidth = aspect_compositor_fullwidth_gen(engine);
		  if (g_aspect_force_active && !compositor_fullwidth &&
		      ((g_base_w > 0 && g_base_w < g_w) || (g_base_h > 0 && g_base_h < g_h))) {
		    aspect_hud_rect(engine,&aspect_gui_x, &aspect_gui_y, &aspect_gui_w, &aspect_gui_h);
		    aspect_gui_center = aspect_gui_w > 0 && aspect_gui_h > 0;
		  }
		  if (aspect_gui_center) {
		    if (!ensure_scratch_buffer(engine,&g_appcrop)) {
		      engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the application crop buffer");
		      return ANYGM_ERROR_OUT_OF_MEMORY;
		    }
		    for (int yy = 0; yy < aspect_gui_h; yy++) {
		      memcpy(g_appcrop + (size_t)yy * aspect_gui_w,
		             g_fb + (size_t)(aspect_gui_y + yy) * g_w + aspect_gui_x,
	             (size_t)aspect_gui_w * sizeof(uint32_t));
	    }
	    g_render.app_surface = g_appcrop;
	    g_render.app_w = aspect_gui_w;
	    g_render.app_h = aspect_gui_h;
	    g_render.app_surface_opaque = g_render.fb_opaque_known && g_render.fb_all_opaque;
		    aspect_view_overlay_end(engine,&draw_ov, 0);
		    draw_ov.active = 0;
		  }
		  int gsw = aspect_gui_center ? aspect_gui_w : (g_gui_sw > 0 ? g_gui_sw : (int)ow);
		  int gsh = aspect_gui_center ? aspect_gui_h : (g_gui_sh > 0 ? g_gui_sh : (int)oh);
			  int gui_indirect = !aspect_gui_center && !g_canvas_mode && (gsw != (int)ow || gsh != (int)oh);
			  if ((aspect_gui_center || gui_indirect) && !ensure_scratch_buffer(engine,&g_guibuf)) {
			    engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the GUI scratch buffer");
			    return ANYGM_ERROR_OUT_OF_MEMORY;
			  }
			  uint32_t *gtarget = (aspect_gui_center || gui_indirect) ? g_guibuf : g_screen;
			  int gtw = (aspect_gui_center || gui_indirect) ? gsw : (int)ow;
			  int gth = (aspect_gui_center || gui_indirect) ? gsh : (int)oh;
			  gml_render_begin(&g_render, gtarget, gtw, gth,
		                     (g_aspect_force_active || aspect_gui_center) ? 0.0 : -(double)g_gui_ox,
		                     (g_aspect_force_active || aspect_gui_center) ? 0.0 : -(double)g_gui_oy);
	  gml_render_set_pending_fill(&g_render, 0);
	  /* The app surface normally presents into the view PORT fitted into GUI space. A classic
	   * runtime window resize preserves the declared port rectangle and clears the new margins. */
	  double pvis_ = gml_global_arr(&g_vm, "view_visible", 0);
	  double pw_ = 0, ph_ = 0;
	  int port_x_ = 0, port_y_ = 0;
	  if (pvis_ >= 0.5) {
	    port_x_ = (int)lround(gml_global_arr(&g_vm,"view_xport",0));
	    port_y_ = (int)lround(gml_global_arr(&g_vm,"view_yport",0));
	    pw_ = gml_global_arr(&g_vm, "view_wport", 0);
	    ph_ = gml_global_arr(&g_vm, "view_hport", 0);
	  }
	  if (g_render.app_surface_owned &&
	      stale_full_view_port(engine,present_view_count(engine,NULL,NULL,NULL),port_x_,port_y_,
	                           (int)lround(pw_),(int)lround(ph_),(int)g_w,(int)g_h,
	                           g_render.app_w,g_render.app_h)) {
	    port_x_ = port_y_ = 0;
	    pw_ = g_render.app_w;
	    ph_ = g_render.app_h;
	  }
	  /* compute_present(engine) may have selected an explicitly owned, window-sized application
	   * surface as the final Studio raster.  Its view port describes the world inside that
	   * already-composed surface; fitting the complete surface through the view's slightly
	   * different aspect would introduce a one-pixel border. */
	  if (!g_aspect_force_active && anygm_policy_has_modern_layer_semantics(&g_win) &&
	      g_render.app_surface_owned && g_render.app_draw_enable &&
	      g_vm.gui_w <= 0 && g_vm.gui_h <= 0 &&
	      g_render.app_w == (int)ow && g_render.app_h == (int)oh &&
	      g_vm.window_w == g_render.app_w && g_vm.window_h == g_render.app_h) {
	    port_x_ = port_y_ = 0;
	    pw_ = g_render.app_w;
	    ph_ = g_render.app_h;
	  }
		  if (aspect_gui_center) { pw_ = aspect_gui_w; ph_ = aspect_gui_h; }
	  else if (g_aspect_force_active) { pw_ = g_w; ph_ = g_h; }
  if (pw_ <= 0 || ph_ <= 0) { pw_ = g_w; ph_ = g_h; }
	  int prw, prh, prx, pry;
	  if (g_canvas_mode) { prw = gsw; prh = gsh; prx = 0; pry = 0; }
	  else {
	    int explicit_window_ = g_vm.window_w>0 && g_vm.window_h>0;
	    if (!gml_classic_present_explicit_port(&g_win,explicit_window_,gtw,gth,
	                                            port_x_,port_y_,(int)lround(pw_),(int)lround(ph_),
	                                            &prx,&pry,&prw,&prh)) {
	      double fit = (double)gtw / pw_;
	      if (ph_ * fit > (double)gth) fit = (double)gth / ph_;
	      /* Studio 2 truncates the fitted viewport extent and biases an odd remainder toward the
	       * positive axis. Studio 1 rounds the extent and uses the older integer centring phase.
	       * Keeping this generation distinction avoids a one-pixel screen-stage stretch in
	       * modern projects without shifting otherwise identical pre-bytecode-17 presentations. */
	      if (anygm_policy_has_modern_layer_semantics(&g_win)) {
	        prw = (int)floor(pw_ * fit + 1e-9); prh = (int)floor(ph_ * fit + 1e-9);
	        prx = (gtw - prw + 1) / 2; pry = (gth - prh + 1) / 2;
	      } else {
	        prw = (int)lround(pw_ * fit); prh = (int)lround(ph_ * fit);
	        prx = (gtw - prw + 1) / 2; pry = (gth - prh + 1) / 2;
	      }
	    }
	  }
	  /* Preserve the classic fixed-function viewport phase when the logical
	   * view is presented into a larger port. */
  if (!g_canvas_mode && !g_aspect_force_active && !g_render.app_phase_y)
	    gml_classic_present_adjust(&g_win,(int)g_w,(int)g_h,prw,prh,g_render.interp,&prx,&pry);
	  g_present_mouse_valid=prw>0 && prh>0 && pw_>0 && ph_>0;
	  g_present_mouse_x=prx; g_present_mouse_y=pry;
	  g_present_mouse_w=prw; g_present_mouse_h=prh;
	  g_present_mouse_src_w=(int)lround(pw_);
	  g_present_mouse_src_h=(int)lround(ph_);
	  /* Without an explicit display_set_gui_size, Studio's screen-stage projection is the logical
	   * application surface fitted into its viewport. A self-compositor relies on both pieces: its
	   * ordinary coordinates scale with the fitted viewport, while negative/overflowing coordinates
	   * draw into the window margins. Begin with the viewport as the physical GUI base, then expose
	   * the application-surface dimensions as its logical coordinate system. */
	  int screen_default_gui=!g_render.app_draw_enable && !g_canvas_mode &&
	                         !g_aspect_force_active && g_vm.gui_w<=0 && g_vm.gui_h<=0 &&
	                         prw>0 && prh>0;
	  if(screen_default_gui){
	    gml_render_gui_begin(&g_render,prw,prh);
	    /* A self-compositor can own an application surface that already is the explicit window
	     * raster while its camera remains much smaller. Post-Draw addresses that complete raster
	     * in window coordinates; applying the camera-to-port scale again turns a full-surface blit
	     * into an oversized, clipped image. A compositor which keeps a logical application surface
	     * still uses the camera-sized coordinate system below. */
	    int owned_window_raster=g_render.app_surface_owned &&
	                            g_render.app_w>0 && g_render.app_h>0 &&
	                            g_render.app_w==g_vm.window_w && g_render.app_h==g_vm.window_h &&
	                            g_render.app_w==gtw && g_render.app_h==gth;
	    gml_render_gui_set_size(&g_render,
	      owned_window_raster?g_render.app_w:(int)g_w,
	      owned_window_raster?g_render.app_h:(int)g_h);
	  } else {
	  gml_render_gui_begin(&g_render,gsw,gsh);
	    /* display_set_gui_size() is normally called from Create/room setup, before the GUI pass
	     * starts. Seed the renderer from that persistent VM state as well as accepting live changes
	     * during Draw GUI. */
	    if(anygm_policy_has_modern_layer_semantics(&g_win) && g_vm.gui_w>0 && g_vm.gui_h>0)
	      gml_render_gui_set_size(&g_render,g_vm.gui_w,g_vm.gui_h);
	  }
	  /* GM screen-stage events: Pre-Draw -> [default app-surface blit] -> Post-Draw -> GUI.
	   * Content that composites the application surface itself, for example through a presentation
	   * object applying a palette shader in Post-Draw) disable the default blit and draw here.
	   * Post-Draw remains anchored to the fitted application viewport: negative coordinates may
	   * intentionally spill into the surrounding window margins. Applying that viewport origin is
	   * what keeps a self-compositor centered when its explicit window is wider than the view. */
		  log_present_pass(engine,"gui-begin",gtarget,gtw,gth);
		  double screen_cam_x=g_render.cam_x,screen_cam_y=g_render.cam_y;
		  int screen_viewport_offset=screen_default_gui && (prx!=0 || pry!=0);
		  if(screen_viewport_offset){ g_render.cam_x=-(double)prx; g_render.cam_y=-(double)pry; }
		  gml_vm_draw_pass(&g_vm, "Draw_77");   /* Post-Draw (GMS2 event 77) can replace the default
		                                         * application-surface blit with a custom composite. */
		  log_present_pass(engine,"post-draw",gtarget,gtw,gth);
		  if (g_render.app_draw_enable || anygm_host_development_setting(&g_host,"GML_NO_CRT"))
		    gml_render_set_pending_underlay(&g_render, prx, pry, prw, prh);
      if (g_aspect_force_active)
        gml_render_flush_pending_underlay(&g_render);
		      if (aspect_gui_center) {
		        g_render.cam_x = 0.0;
		        g_render.cam_y = 0.0;
	      } else if (g_aspect_force_active) {
	        /* A full-width compositor draws in the forced framebuffer's own coordinate space.
	         * Retaining the centered native-GUI offset translates shader surface draws and clips
	         * the trailing edge. Non-compositor GUI passes keep the native centered offset. */
	        g_render.cam_x = compositor_fullwidth ? 0.0 : -(double)g_gui_ox;
	        g_render.cam_y = compositor_fullwidth ? 0.0 : -(double)g_gui_oy;
	      }
		  if (!anygm_host_development_setting(&g_host,"GML_NO_CRT")) {
		    gml_vm_draw_pass(&g_vm, "Draw_74");  /* Draw GUI Begin */
		    log_present_pass(engine,"gui-pre",gtarget,gtw,gth);
		    gml_vm_draw_pass(&g_vm, "Draw_65");
		    gml_vm_draw_gui(&g_vm);
		    log_present_pass(engine,"gui",gtarget,gtw,gth);
		    gml_vm_draw_pass(&g_vm, "Draw_66");
		    gml_vm_draw_pass(&g_vm, "Draw_75");  /* Draw GUI End */
		    log_present_pass(engine,"gui-end",gtarget,gtw,gth);
		  }
  if(screen_viewport_offset){ g_render.cam_x=screen_cam_x; g_render.cam_y=screen_cam_y; }
  gml_render_gui_end(&g_render);
  gml_render_flush_pending_underlay(&g_render);
  gml_render_flush_pending_fill(&g_render);
  log_present_pass(engine,"flushed",gtarget,gtw,gth);
  if (anygm_host_development_setting(&g_host,"GML_LOG_PRESENT")) {
    if (g_diag.present_frame++ % 120 == 0) engine_logf(engine,ANYGM_LOG_DEBUG, "[present] out=%ux%u canvas=%d gui=%dx%d indirect=%d port=%dx%d prect=(%d,%d %dx%d) off=(%d,%d)\n",
      ow, oh, g_canvas_mode, gsw, gsh, gui_indirect, (int)pw_, (int)ph_, prx, pry, prw, prh, g_gui_ox, g_gui_oy); }
  /* Fallback: content can disable the automatic application-surface blit
   * intending to composite it itself in a Draw GUI event. If that compositor paints nothing to the
   * screen here (unsupported GUI-space transform, absent object, etc.) the frame would be black — so
   * if the screen is still empty but the app-surface has pixels, blit it so the render isn't lost. */
  if (!g_render.app_draw_enable && !anygm_host_development_setting(&g_host,"GML_NO_CRT")) {
    int screen_empty = 1;
    for (int i = 0; i < gtw * gth; i++) if (gtarget[i] & 0xFFFFFF) { screen_empty = 0; break; }
    if (screen_empty) {
      int fb_has = 0;
      for (unsigned i = 0; i < g_w * g_h; i++) if (g_fb[i] & 0xFFFFFF) { fb_has = 1; break; }
	      if (fb_has) {
          if (g_aspect_force_active) {
            double scx = g_render.cam_x, scy = g_render.cam_y;
            g_render.cam_x = g_render.cam_y = 0.0;
            gml_draw_surface_stretched(&g_render, 0, prx, pry, prw, prh, 0xFFFFFF, 1.0);
            g_render.cam_x = scx; g_render.cam_y = scy;
          } else {
	          gml_draw_surface_stretched(&g_render, 0, prx, pry, prw, prh, 0xFFFFFF, 1.0);
          }
	      }
	    }
	  }
			  if (aspect_gui_center) {
			    if (ow == g_w && oh == g_h)
			      memcpy(g_screen, g_fb, (size_t)ow * oh * sizeof(uint32_t));
			    else
			      memset(g_screen, 0, (size_t)ow * oh * sizeof(uint32_t));
			    for (int yy = 0; yy < aspect_gui_h && aspect_gui_y + yy < (int)oh; yy++) {
			      if (aspect_gui_x >= (int)ow) continue;
		      int copy_w = aspect_gui_w;
		      if (aspect_gui_x + copy_w > (int)ow) copy_w = (int)ow - aspect_gui_x;
		      if (copy_w > 0)
		        memcpy(g_screen + (size_t)(aspect_gui_y + yy) * ow + aspect_gui_x,
		               g_guibuf + (size_t)yy * aspect_gui_w,
		               (size_t)copy_w * sizeof(uint32_t));
		    }
	  } else if (gui_indirect) {
		    if (ow >= (unsigned)gtw && oh >= (unsigned)gth) {
		      /* The generic box path degenerates to one source pixel during an upscale. Walk the
		       * pixel-centred floor((x+.5)*src/dst) mapping incrementally, without per-pixel
		       * division or averaging. The doubled accumulator preserves half steps for odd sizes. */
		      unsigned sy = 0, yacc = (unsigned)gth;
		      for (unsigned oy2 = 0; oy2 < oh; oy2++) {
		        const uint32_t *src = g_guibuf + (size_t)sy * gtw;
		        uint32_t *dst = g_screen + (size_t)oy2 * ow;
		        unsigned sx = 0, xacc = (unsigned)gtw;
		        for (unsigned ox2 = 0; ox2 < ow; ox2++) {
		          dst[ox2] = src[sx] & 0xFFFFFFu;
		          xacc += (unsigned)gtw * 2u;
		          if (xacc >= ow * 2u) { xacc -= ow * 2u; sx++; }
		        }
		        yacc += (unsigned)gth * 2u;
		        if (yacc >= oh * 2u) { yacc -= oh * 2u; sy++; }
		      }
		    } else {
		      /* Box-downscale the GUI-space frame onto the native output. */
		      for (unsigned oy2 = 0; oy2 < oh; oy2++) {
        unsigned sy0 = oy2 * gth / oh, sy1 = (oy2 + 1) * gth / oh; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (unsigned ox2 = 0; ox2 < ow; ox2++) {
          unsigned sx0 = ox2 * gtw / ow, sx1 = (ox2 + 1) * gtw / ow; if (sx1 <= sx0) sx1 = sx0 + 1;
          unsigned rs = 0, gs = 0, bs = 0, n = 0;
          for (unsigned sy2 = sy0; sy2 < sy1 && sy2 < (unsigned)gth; sy2++)
            for (unsigned sx2 = sx0; sx2 < sx1 && sx2 < (unsigned)gtw; sx2++) {
              uint32_t v = g_guibuf[sy2 * gtw + sx2];
              rs += (v >> 16) & 0xFF; gs += (v >> 8) & 0xFF; bs += v & 0xFF; n++;
            }
          if (!n) n = 1;
          g_screen[oy2 * ow + ox2] = ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
	        }
	      }
	    }
	  }
	  classic_transition_apply(engine,g_screen,ow,oh);
	  if(g_vm.classic_info_active && g_win.classic_game_information_size)
	    gml_draw_classic_game_information(&g_render,g_screen,(int)ow,(int)oh,
	                                      g_win.classic_game_information,
	                                      g_win.classic_game_information_size);
	  if(prof){ t1 = profile_now_ms(engine); g_prof.gui_ms += t1 - t0; t0 = t1; }
  aspect_view_overlay_end(engine,&draw_ov, 0);
  draw_game_cursor(engine,ow,oh);
  if(run_step) gml_vm_post_draw(&g_vm);
  g_have_presented_frame=1;
  if(prof){ t1 = profile_now_ms(engine); g_prof.video_ms += t1 - t0; t0 = t1; }
  /* audio: emit ~44100/fps stereo frames this tick, mixing active voices (acc handles the remainder) */
  {
    g_audio_acc += 44100.0 / g_fps; int nf = (int)g_audio_acc; g_audio_acc -= nf;
    if (nf > 4096) nf = 4096;
    if(nf>0){ gml_audio_mix(g_audio,g_audio_output,nf); g_audio_frames=(size_t)nf; }
  }
  if(prof){
    t1 = profile_now_ms(engine);
    g_prof.audio_ms += t1 - t0;
    g_prof.total_ms += t1 - t_total;
    { double ft = t1 - t_total;
      if(ft > g_prof.max_ms){ g_prof.max_ms = ft; g_prof.max_frame = g_vm.frame; }
      /* GML_PROFILE_SPIKE=<ms>: dump the phase split of any frame that exceeds the threshold */
      if(g_diag.profile_spike_ms < -1){ const char *sp=anygm_host_development_setting(&g_host,"GML_PROFILE_SPIKE"); g_diag.profile_spike_ms = sp? atof(sp) : -1; }
      if(g_diag.profile_spike_ms > 0 && ft > g_diag.profile_spike_ms)
        engine_logf(engine,ANYGM_LOG_DEBUG, "[spike] f%ld total=%.2fms step=%.2f draw=%.2f gui=%.2f video=%.2f\n",
                g_vm.frame, ft, g_prof.step_ms - sp_step, g_prof.draw_ms - sp_draw,
                g_prof.gui_ms - sp_gui, g_prof.video_ms - sp_video); }
    g_prof.frames++;
    profile_report(engine,0);
  }
  /* Snapshot key state after the step consumed it. */
  memcpy(g_evt_vk_prev, g_evt_vk_cur, sizeof(g_evt_vk_cur));
  memcpy(g_evt_key_prev, g_evt_key_cur, sizeof(g_evt_key_cur));
  if(state_restore_frame && g_state_reapply_size){
    size_t restore_size=g_state_reapply_size;
    g_state_reapply_size=0;
    if(!state_unserialize_impl(engine,g_state_reapply,restore_size,0))
      engine_logf(engine,ANYGM_LOG_ERROR,"[anygm] could not reapply loaded state after presentation\n");
    /* state_unserialize_impl deliberately leaves the composited pixels alone.  They are the
     * visible rewind/load frame even though the simulation was put back at its pre-Draw state. */
    g_state_just_loaded=0;
    g_have_presented_frame=1;
  }
  return ANYGM_OK;
}

typedef struct { uint8_t *data; size_t cap, pos; int ok; } CoreW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; } CoreR;
static void cw_raw(CoreW *s, const void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->data){ if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(s->data+s->pos,p,n); else s->ok=0; }
  s->pos+=n;
}
static void cr_raw(CoreR *s, void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ memset(p,0,n); s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(p,s->data+s->pos,n);
  else { memset(p,0,n); s->ok=0; }
  s->pos+=n;
}
static void cw_u32(CoreW *s, uint32_t v){
  uint8_t bytes[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
  cw_raw(s,bytes,sizeof bytes);
}
static void cw_u64(CoreW *s, uint64_t v){
  uint8_t bytes[8];
  for(unsigned i=0;i<8;i++) bytes[i]=(uint8_t)(v>>(i*8));
  cw_raw(s,bytes,sizeof bytes);
}
static void cw_i64(CoreW *s, int64_t v){ cw_u64(s,(uint64_t)v); }
static void cw_i32(CoreW *s, int v){ cw_u32(s,(uint32_t)(int32_t)v); }
static void cw_d(CoreW *s, double v){ uint64_t bits=0; memcpy(&bits,&v,sizeof bits); cw_u64(s,bits); }
static uint32_t cr_u32(CoreR *s){
  uint8_t bytes[4]={0}; cr_raw(s,bytes,sizeof bytes);
  return (uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
}
static uint64_t cr_u64(CoreR *s){
  uint8_t bytes[8]={0}; cr_raw(s,bytes,sizeof bytes); uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)bytes[i]<<(i*8);
  return value;
}
static int64_t cr_i64(CoreR *s){ return (int64_t)cr_u64(s); }
static int cr_i32(CoreR *s){ return (int)(int32_t)cr_u32(s); }
static double cr_d(CoreR *s){ uint64_t bits=cr_u64(s); double value=0; memcpy(&value,&bits,sizeof value); return value; }
static void state_store_u32(uint8_t *destination,uint32_t value){
  destination[0]=(uint8_t)value;
  destination[1]=(uint8_t)(value>>8);
  destination[2]=(uint8_t)(value>>16);
  destination[3]=(uint8_t)(value>>24);
}

enum {
  ANYGM_STATE_HEADER_SIZE=112,
  ANYGM_STATE_ENCODING_LITTLE_ENDIAN_IEEE754=1
};
#define ANYGM_STATE_MAGIC UINT32_C(0x54534741)

typedef struct {
  uint64_t total_size;
  uint64_t content_fingerprint;
  uint64_t compatibility_fingerprint;
  uint64_t config_fingerprint;
  uint64_t payload_checksum;
  uint64_t core_size;
  uint64_t render_size;
  uint64_t vm_size;
  uint64_t audio_size;
  uint64_t payload_size;
} AnygmStateHeader;

static uint64_t state_hash_bytes(const void *data,size_t size){
  const uint8_t *bytes=data;
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){
    uint64_t word=(uint64_t)bytes[0]|((uint64_t)bytes[1]<<8)|((uint64_t)bytes[2]<<16)|
                  ((uint64_t)bytes[3]<<24)|((uint64_t)bytes[4]<<32)|((uint64_t)bytes[5]<<40)|
                  ((uint64_t)bytes[6]<<48)|((uint64_t)bytes[7]<<56);
    hash^=word; hash*=UINT64_C(1099511628211);
    bytes+=8; size-=8;
  }
  while(size--){ hash^=*bytes++; hash*=UINT64_C(1099511628211); }
  return hash;
}

static uint64_t state_current_config_fingerprint(AnygmEngine *engine){
  uint8_t encoded[32]={0};
  CoreW writer={encoded,sizeof encoded,0,1};
  cw_u32(&writer,g_config.present_width);
  cw_u32(&writer,g_config.present_height);
  cw_u32(&writer,g_config.aspect_mode);
  cw_u32(&writer,g_config.god_mode);
  return writer.ok?state_hash_bytes(encoded,writer.pos):0;
}

static void state_identity_refresh(AnygmEngine *engine){
  g_content_fingerprint=state_hash_bytes(g_win.data,g_win.size);
  g_compatibility_fingerprint=g_compatibility.fingerprint;
}

static void state_write_render(AnygmEngine *engine,CoreW *s){
  cw_i32(s,g_render.n_fonts);
  for(int i=0;i<GML_MAX_FONTS;i++){
    GmlFont *f=&g_render.fonts[i];
    cw_i32(s,f->sprite); cw_i32(s,f->first);
    cw_i32(s,f->prop); cw_i32(s,f->sep);
    int map_len=(f->map && f->map_len>0) ? f->map_len : 0;
    cw_i32(s,map_len);
    for(int j=0;j<map_len;j++) cw_u32(s,f->map[j]);
  }
  cw_i32(s,g_render.app_draw_enable); cw_u32(s,g_render.color); cw_d(s,g_render.alpha);
  cw_i32(s,g_render.halign); cw_i32(s,g_render.valign); cw_i32(s,g_render.font);
  cw_i32(s,g_render.alphablend);
  cw_i32(s,g_render.circle_precision);
  cw_i32(s,g_render.next_surface_id);
  /* The surface assigned to view 0 (view_surface_id) is derived data:
   * anygm_run_frame mirrors the freshly rendered frame into it every frame, after the room pass and
   * before any Draw GUI reads it. Serializing this derived surface needlessly increases every
   * state and rewind delta. Skip its pixels: on load it starts transparent and is repopulated
   * mid-pipeline in the first anygm_run_frame, before the compositor can sample it. */
  int view_surf = (int)gml_global_arr(&g_vm, "view_surface_id", 0);
  for(int i=0;i<GML_MAX_SURFACES;i++){
    GmlSurface *sf=&g_render.surface[i];
    cw_i32(s,sf->live); cw_i32(s,sf->w); cw_i32(s,sf->h);
    if(sf->live && sf->px && sf->w>0 && sf->h>0){
      size_t n=(size_t)sf->w*sf->h;
      if(view_surf>0 && i==view_surf-1){
        cw_u32(s,1); cw_u32(s,(uint32_t)n); cw_u32(s,0);   /* one transparent run */
        continue;
      }
      /* RLE stores (run,value) pairs because mostly uniform surfaces otherwise dominate state
       * size and rewind deltas. Encoded bytes are cached per surface and rebuilt only after a draw
       * marks the surface dirty, avoiding a scan of every unchanged pixel on each snapshot. */
      if(sf->dirty || !sf->rle){
        size_t need=8;   /* worst grows below */
        size_t nr=0, pos=4;
        if(sf->rle_cap<16){ uint8_t *np=realloc(sf->rle,4096); if(!np){ s->ok=0; continue; } sf->rle=np; sf->rle_cap=4096; }
        for(size_t k=0;k<n;){
          uint32_t v=sf->px[k]; size_t j=k+1;
          while(j<n && sf->px[j]==v && j-k<0xFFFFFFFFu) j++;
          if(pos+8>sf->rle_cap){ size_t nc=sf->rle_cap*2; uint8_t *np=realloc(sf->rle,nc);
            if(!np){ s->ok=0; break; } sf->rle=np; sf->rle_cap=nc; }
          uint32_t run=(uint32_t)(j-k);
          state_store_u32(sf->rle+pos,run);
          state_store_u32(sf->rle+pos+4,v);
          pos+=8; nr++; k=j;
        }
        (void)need;
        state_store_u32(sf->rle,(uint32_t)nr);
        sf->rle_len=pos; sf->dirty=0;
      }
      cw_raw(s,sf->rle,sf->rle_len);
    }
  }
  int runtime_sprites=0;
  for(int i=0;i<g_render.n_spr;i++) if(g_render.spr[i].runtime_rgba) runtime_sprites++;
  cw_i32(s,runtime_sprites);
  for(int i=0;i<g_render.n_spr;i++) if(g_render.spr[i].runtime_rgba){
    GmlSprite *sp=&g_render.spr[i];
    cw_i32(s,i); cw_i32(s,sp->runtime_extra);
    int frames=sp->n_frames>0?sp->n_frames:1;
    cw_i32(s,sp->w); cw_i32(s,sp->h); cw_i32(s,frames); cw_i32(s,sp->originx); cw_i32(s,sp->originy);
    cw_i32(s,sp->ml); cw_i32(s,sp->mt); cw_i32(s,sp->mr); cw_i32(s,sp->mb);
    cw_i32(s,sp->collision_kind); cw_i32(s,sp->collision_tolerance);
    if(sp->runtime_source_path && sp->runtime_source_path[0]){
      size_t plen=strlen(sp->runtime_source_path);
      if(plen>4095) plen=4095;
      cw_i32(s,1);
      cw_i32(s,(int)plen);
      cw_raw(s,sp->runtime_source_path,plen);
      cw_i32(s,sp->runtime_source_imgnum);
      cw_i32(s,sp->runtime_source_removeback);
    } else {
      cw_i32(s,0);
      cw_raw(s,sp->runtime_rgba,(size_t)sp->w*sp->h*frames*4);
    }
  }
}
static void state_profile_maybe_log(AnygmEngine *engine,size_t total, size_t rendern, size_t vmn, size_t audn){
  if(!anygm_host_development_setting(&g_host,"GML_STATE_PROFILE")) return;
  if(total <= g_diag.state_profile_last_total) return;
  g_diag.state_profile_last_total = total;
  size_t surface_bytes = 0, sprite_bytes = 0, file_sprite_bytes = 0;
  int surfaces = 0, runtime_sprites = 0, file_runtime_sprites = 0;
  for(int i=0;i<GML_MAX_SURFACES;i++){
    GmlSurface *sf=&g_render.surface[i];
    if(sf->live && sf->px && sf->w>0 && sf->h>0){
      surfaces++;
      surface_bytes += (size_t)sf->w * (size_t)sf->h * sizeof(uint32_t);
    }
  }
  for(int i=0;i<g_render.n_spr;i++){
    GmlSprite *sp=&g_render.spr[i];
    if(sp->runtime_rgba && sp->w>0 && sp->h>0 && sp->n_frames>0){
      runtime_sprites++;
      size_t bytes = (size_t)sp->w * (size_t)sp->h * (size_t)sp->n_frames * 4;
      if(sp->runtime_source_path && sp->runtime_source_path[0]){ file_runtime_sprites++; file_sprite_bytes += bytes; }
      else sprite_bytes += bytes;
    }
  }
  engine_logf(engine,ANYGM_LOG_INFO,
           "[state-profile] total=%llu render=%llu vm=%llu audio=%llu surfaces=%d/%llu runtime_sprites=%d raw/%llu file=%d/%llu inst=%d globals=%d\n",
           (unsigned long long)total, (unsigned long long)rendern,
           (unsigned long long)vmn, (unsigned long long)audn,
           surfaces, (unsigned long long)surface_bytes,
           runtime_sprites, (unsigned long long)sprite_bytes,
           file_runtime_sprites, (unsigned long long)file_sprite_bytes,
           g_vm.inst_count,g_vm.globals.len);
}
/* Read font-pool records [from,to) into g_render.fonts. Returns 0 on parse error. */
static int state_read_font_records(AnygmEngine *engine,CoreR *s, int from, int to){
  for(int i=from;i<to;i++){
    g_render.fonts[i].sprite=cr_i32(s); g_render.fonts[i].first=cr_i32(s);
    g_render.fonts[i].prop=cr_i32(s); g_render.fonts[i].sep=cr_i32(s);
    int raw_len=cr_i32(s);
    if(raw_len<0 || raw_len>4096){ s->ok=0; return 0; }
    if(raw_len>0){
      g_render.fonts[i].map=malloc((size_t)raw_len*sizeof(uint32_t));
      if(!g_render.fonts[i].map){ s->ok=0; return 0; }
    }
    for(int j=0;j<raw_len;j++) g_render.fonts[i].map[j]=cr_u32(s);
    g_render.fonts[i].map_len=raw_len;
  }
  return s->ok;
}
static int state_read_render(AnygmEngine *engine,CoreR *s){
  int nf=cr_i32(s);
  if(nf<0 || nf>GML_MAX_FONTS) s->ok=0;
  for(int i=0;i<GML_MAX_FONTS;i++){
    free(g_render.fonts[i].map);
    g_render.fonts[i].map=NULL;
    g_render.fonts[i].map_len=0;
    g_render.fonts[i].sprite=-1; g_render.fonts[i].first=0;
    g_render.fonts[i].prop=0; g_render.fonts[i].sep=0;
  }
  if(!state_read_font_records(engine,s,0,GML_MAX_FONTS)) return 0;
  g_render.n_fonts=nf;
  gml_render_rebuild_font_maps(&g_render);
  if(anygm_host_development_setting(&g_host,"GML_LOG_STATE")) engine_logf(engine,ANYGM_LOG_DEBUG,"[state] render: fonts ok (nf=%d pos=%" PRIu64 " ok=%d)\n",nf,(uint64_t)s->pos,s->ok);
  g_render.app_draw_enable=cr_i32(s); g_render.color=cr_u32(s); g_render.alpha=cr_d(s);
  g_render.halign=cr_i32(s); g_render.valign=cr_i32(s); g_render.font=cr_i32(s);
  g_render.alphablend=cr_i32(s)?1:0;
  g_render.circle_precision=cr_i32(s);
  if(g_render.circle_precision<4) g_render.circle_precision=4;
  if(g_render.circle_precision>64) g_render.circle_precision=64;
  g_render.circle_precision=(g_render.circle_precision/4)*4;
  if(g_render.circle_precision<4) g_render.circle_precision=4;
  g_render.next_surface_id=cr_i32(s);
  if(g_render.next_surface_id<1 || g_render.next_surface_id>GML_MAX_SURFACES) g_render.next_surface_id=1;
  for(int i=0;i<GML_MAX_SURFACES;i++){
    free(g_render.surface[i].px); free(g_render.surface[i].rle);
    memset(&g_render.surface[i],0,sizeof(g_render.surface[i]));
  }
  for(int i=0;i<GML_MAX_SURFACES;i++){
    int live=cr_i32(s), w=cr_i32(s), h=cr_i32(s);
    if(live){
      if(w<=0 || h<=0 || w>4096 || h>4096){ s->ok=0; return 0; }
      size_t n=(size_t)w*h;
      g_render.surface[i].px=malloc(n*sizeof(uint32_t));
      if(!g_render.surface[i].px){ s->ok=0; return 0; }
      g_render.surface[i].live=1; g_render.surface[i].w=w; g_render.surface[i].h=h;
      int all_opaque = 1, all_transparent = 1;
      uint32_t nrun=cr_u32(s); size_t k=0;
      for(uint32_t r2=0; r2<nrun && s->ok; r2++){
        uint32_t run=cr_u32(s), v=cr_u32(s);
        if(run>n-k){ s->ok=0; break; }
        if((v>>24)!=255u) all_opaque = 0;
        if((v>>24)!=0u) all_transparent = 0;
        for(uint32_t q=0;q<run;q++) g_render.surface[i].px[k++]=v;
        g_render.surface[i].dirty=1;
      }
      if(k!=n) { all_opaque = 0; memset(g_render.surface[i].px+k,0,(n-k)*sizeof(uint32_t)); if(s->ok && k>0) s->ok=1; }
      g_render.surface[i].opaque_known=1;
      g_render.surface[i].all_opaque=all_opaque;
      g_render.surface[i].all_transparent=all_transparent;
    }
  }
  if(anygm_host_development_setting(&g_host,"GML_LOG_STATE")) engine_logf(engine,ANYGM_LOG_DEBUG,"[state] render: surfaces ok (nsurf=%d pos=%" PRIu64 " ok=%d)\n",GML_MAX_SURFACES,(uint64_t)s->pos,s->ok);
  uint8_t *seen_runtime = NULL;
  int seen_cap = 0;
  int runtime_sprites=cr_i32(s);
  if(anygm_host_development_setting(&g_host,"GML_LOG_STATE")) engine_logf(engine,ANYGM_LOG_DEBUG,"[state] render: runtime_sprites=%d (pos=%" PRIu64 ")\n",runtime_sprites,(uint64_t)s->pos);
  if(runtime_sprites<0 || runtime_sprites>4096){ s->ok=0; return 0; }
  seen_cap = g_render.spr_cap + runtime_sprites + 16;
  if(seen_cap < g_render.n_spr + runtime_sprites + 16) seen_cap = g_render.n_spr + runtime_sprites + 16;
  seen_runtime = calloc((size_t)(seen_cap>0?seen_cap:1),1);
  if(!seen_runtime){ s->ok=0; return 0; }
  for(int n=0;n<runtime_sprites;n++){
    int id=cr_i32(s), extra=cr_i32(s);
    int w=cr_i32(s), h=cr_i32(s), frames=cr_i32(s), ox=cr_i32(s), oy=cr_i32(s);
    int ml=cr_i32(s), mt=cr_i32(s), mr=cr_i32(s), mb=cr_i32(s);
    int kind=cr_i32(s), tolerance=cr_i32(s);
    if(id<0 || w<=0 || h<=0 || frames<=0 || frames>4096 || w>4096 || h>4096){ s->ok=0; return 0; }
    if(id>=seen_cap){
      int nc=id+256;
      uint8_t *ns=realloc(seen_runtime,(size_t)nc);
      if(!ns){ free(seen_runtime); s->ok=0; return 0; }
      memset(ns+seen_cap,0,(size_t)(nc-seen_cap));
      seen_runtime=ns; seen_cap=nc;
    }
    seen_runtime[id]=1;
    int mode = cr_i32(s);
    if(mode==1){
      int plen=cr_i32(s);
      if(plen<0 || plen>4095){ free(seen_runtime); s->ok=0; return 0; }
      char *path=malloc((size_t)plen+1);
      if(!path){ free(seen_runtime); s->ok=0; return 0; }
      cr_raw(s,path,(size_t)plen); path[plen]=0;
      int imgnum=cr_i32(s), removeback=cr_i32(s);
      int got=-1;
      if(id>=0 && id<g_render.n_spr){
        GmlSprite *cur=&g_render.spr[id];
        if(cur->runtime_rgba && cur->runtime_source_path && !strcmp(cur->runtime_source_path,path) &&
           cur->w==w && cur->h==h && cur->n_frames==frames && cur->originx==ox && cur->originy==oy){
          got=id;
        }
      }
      if(got<0){
        if(extra || id>=g_render.base_n_spr){
          if(id>=0 && id<g_render.n_spr && g_render.spr[id].runtime_extra) gml_sprite_delete(&g_render,id);
          got=gml_sprite_add_file(&g_render,path,imgnum,removeback,ox,oy);
        } else {
          got=gml_sprite_replace_from_file(&g_render,id,path,imgnum,removeback,0,ox,oy) ? id : -1;
        }
      }
      free(path);
      if(got!=id || got<0 || got>=g_render.n_spr){ free(seen_runtime); s->ok=0; return 0; }
      GmlSprite *chk=&g_render.spr[got];
      if(chk->w!=w || chk->h!=h || chk->n_frames!=frames){ free(seen_runtime); s->ok=0; return 0; }
    } else if(mode==0){
      uint64_t pixn=(uint64_t)w*(uint64_t)h*(uint64_t)frames;
      size_t rem=s->pos<=s->cap ? s->cap-s->pos : 0;
      if(pixn > SIZE_MAX/4 || (size_t)pixn*4 > rem){ free(seen_runtime); s->ok=0; return 0; }
      size_t bytes=(size_t)pixn*4;
      uint8_t *rgba=malloc(bytes);
      if(!rgba){ free(seen_runtime); s->ok=0; return 0; }
      cr_raw(s,rgba,bytes);
      if(extra || id>=g_render.base_n_spr){
        if(id>=0 && id<g_render.n_spr && g_render.spr[id].runtime_extra) gml_sprite_delete(&g_render,id);
        int got=gml_sprite_append_from_rgba_frames(&g_render,rgba,w,h,frames,ox,oy,"<state-sprite>");
        if(got!=id){ free(seen_runtime); s->ok=0; return 0; }
      } else if(!gml_sprite_replace_from_rgba_frames(&g_render,id,rgba,w,h,frames,ox,oy)){
        free(rgba); free(seen_runtime); s->ok=0; return 0;
      }
    } else {
      free(seen_runtime); s->ok=0; return 0;
    }
    if(id>=0 && id<g_render.n_spr){
      GmlSprite *sp=&g_render.spr[id];
      if(kind<0 || kind>3) kind=0;
      if(tolerance<0) tolerance=0;
      if(tolerance>255) tolerance=255;
      if(ml<0) ml=0;
      if(mt<0) mt=0;
      if(mr>=w) mr=w-1;
      if(mb>=h) mb=h-1;
      sp->ml=ml; sp->mt=mt; sp->mr=mr; sp->mb=mb;
      sp->collision_kind=kind; sp->collision_tolerance=tolerance;
    }
  }
  int base=g_render.base_n_spr>0?g_render.base_n_spr:g_render.n_spr;
  if(base>g_render.n_spr) base=g_render.n_spr;
  for(int i=g_render.n_spr-1;i>=base;i--)
    if(g_render.spr[i].runtime_extra && (i>=seen_cap || !seen_runtime[i]))
      gml_sprite_delete(&g_render,i);
  for(int i=0;i<base;i++) if(g_render.spr[i].runtime_rgba && (i>=seen_cap || !seen_runtime[i])){
    GmlSprite *sp=&g_render.spr[i];
    free(sp->runtime_rgba); free(sp->runtime_row_min); free(sp->runtime_row_max); free(sp->runtime_source_path);
    sp->runtime_rgba=NULL; sp->runtime_row_min=NULL; sp->runtime_row_max=NULL; sp->runtime_source_path=NULL; sp->runtime_owned=0; sp->runtime_extra=0;
    sp->runtime_source_imgnum=0; sp->runtime_source_removeback=0;
  }
  free(seen_runtime);
  return s->ok;
}
static void state_header_write(uint8_t *destination,const AnygmStateHeader *header){
  CoreW writer={destination,ANYGM_STATE_HEADER_SIZE,0,1};
  cw_u32(&writer,ANYGM_STATE_MAGIC);
  cw_u32(&writer,ANYGM_STATE_SCHEMA);
  cw_u32(&writer,ANYGM_STATE_HEADER_SIZE);
  cw_u32(&writer,ANYGM_STATE_ENCODING_LITTLE_ENDIAN_IEEE754);
  cw_u64(&writer,header->total_size);
  cw_u64(&writer,header->content_fingerprint);
  cw_u32(&writer,ANYGM_COMPATIBILITY_SCHEMA);
  cw_u32(&writer,0);
  cw_u64(&writer,header->compatibility_fingerprint);
  cw_u64(&writer,header->config_fingerprint);
  cw_u64(&writer,header->payload_checksum);
  cw_u64(&writer,header->core_size);
  cw_u64(&writer,header->render_size);
  cw_u64(&writer,header->vm_size);
  cw_u64(&writer,header->audio_size);
  cw_u64(&writer,header->payload_size);
  cw_u64(&writer,0);
}

static int state_header_read(AnygmEngine *engine,const void *data,size_t size,
                             AnygmStateHeader *header){
  if(!data || !header || size<ANYGM_STATE_HEADER_SIZE) return 0;
  CoreR reader={(const uint8_t*)data,ANYGM_STATE_HEADER_SIZE,0,1};
  uint32_t magic=cr_u32(&reader);
  uint32_t schema=cr_u32(&reader);
  uint32_t header_size=cr_u32(&reader);
  uint32_t encoding=cr_u32(&reader);
  header->total_size=cr_u64(&reader);
  header->content_fingerprint=cr_u64(&reader);
  uint32_t compatibility_schema=cr_u32(&reader);
  uint32_t flags=cr_u32(&reader);
  header->compatibility_fingerprint=cr_u64(&reader);
  header->config_fingerprint=cr_u64(&reader);
  header->payload_checksum=cr_u64(&reader);
  header->core_size=cr_u64(&reader);
  header->render_size=cr_u64(&reader);
  header->vm_size=cr_u64(&reader);
  header->audio_size=cr_u64(&reader);
  header->payload_size=cr_u64(&reader);
  uint64_t reserved=cr_u64(&reader);
  if(!reader.ok || reader.pos!=ANYGM_STATE_HEADER_SIZE || magic!=ANYGM_STATE_MAGIC ||
     schema!=ANYGM_STATE_SCHEMA || header_size!=ANYGM_STATE_HEADER_SIZE ||
     encoding!=ANYGM_STATE_ENCODING_LITTLE_ENDIAN_IEEE754 ||
     compatibility_schema!=ANYGM_COMPATIBILITY_SCHEMA || flags || reserved) return 0;
  if(header->total_size<ANYGM_STATE_HEADER_SIZE || header->total_size>size ||
     header->payload_size!=header->total_size-ANYGM_STATE_HEADER_SIZE) return 0;
  const uint8_t *all_bytes=data;
  for(size_t i=(size_t)header->total_size;i<size;i++) if(all_bytes[i]) return 0;
  uint64_t sections=header->core_size;
  if(UINT64_MAX-sections<header->render_size) return 0; sections+=header->render_size;
  if(UINT64_MAX-sections<header->vm_size) return 0; sections+=header->vm_size;
  if(UINT64_MAX-sections<header->audio_size) return 0; sections+=header->audio_size;
  if(sections!=header->payload_size || header->payload_size>SIZE_MAX) return 0;
  if(header->content_fingerprint!=g_content_fingerprint ||
     header->compatibility_fingerprint!=g_compatibility_fingerprint ||
     header->config_fingerprint!=state_current_config_fingerprint(engine)) return 0;
  const uint8_t *payload=(const uint8_t*)data+ANYGM_STATE_HEADER_SIZE;
  return state_hash_bytes(payload,(size_t)header->payload_size)==header->payload_checksum;
}

static void state_write(AnygmEngine *engine,CoreW *s){
  uint8_t empty_header[ANYGM_STATE_HEADER_SIZE]={0};
  cw_raw(s,empty_header,sizeof empty_header);
  size_t core_start=s->pos;
  cw_u32(s,g_w); cw_u32(s,g_h); cw_u32(s,g_bg); cw_d(s,g_fps);
  cw_i32(s,g_follow_player); cw_i32(s,g_player_obj); cw_d(s,g_audio_acc);
  { cw_i64(s,(int64_t)g_vm.frame); }
  cw_raw(s,g_pad_cur,sizeof(g_pad_cur)); cw_raw(s,g_pad_prev,sizeof(g_pad_prev));
  cw_raw(s,g_key_cur,sizeof(g_key_cur)); cw_raw(s,g_key_prev,sizeof(g_key_prev));
  size_t coren=s->pos-core_start;
  size_t render_start=s->pos;
  state_write_render(engine,s);
  size_t rendern=s->pos-render_start;
  size_t vmn=0, audn=0;
  if(s->data){
    size_t wr=0, avail=s->pos<=s->cap? s->cap-s->pos : 0;
    uint8_t *dst=s->data+(s->pos<=s->cap? s->pos : s->cap);
    if(!gml_vm_state_save(&g_vm,dst,avail,&wr)) s->ok=0;
    vmn=wr;
    s->pos+=wr;
  } else {
    vmn = gml_vm_state_size(&g_vm);
    s->pos+=vmn;
  }
  if(s->data){
    size_t wr=0, avail=s->pos<=s->cap? s->cap-s->pos : 0;
    uint8_t *dst=s->data+(s->pos<=s->cap? s->pos : s->cap);
    if(!gml_audio_state_save(g_audio,dst,avail,&wr)) s->ok=0;
    audn=wr;
    s->pos+=wr;
  } else {
    audn = gml_audio_state_size(g_audio);
    s->pos+=audn;
  }
  if(s->data && s->ok && s->pos<=s->cap){
    AnygmStateHeader header={0};
    header.total_size=s->pos;
    header.content_fingerprint=g_content_fingerprint;
    header.compatibility_fingerprint=g_compatibility_fingerprint;
    header.config_fingerprint=state_current_config_fingerprint(engine);
    header.core_size=coren;
    header.render_size=rendern;
    header.vm_size=vmn;
    header.audio_size=audn;
    header.payload_size=s->pos-ANYGM_STATE_HEADER_SIZE;
    header.payload_checksum=state_hash_bytes(s->data+ANYGM_STATE_HEADER_SIZE,
                                             (size_t)header.payload_size);
    state_header_write(s->data,&header);
  }
  state_profile_maybe_log(engine,s->pos, rendern, vmn, audn);
}
static size_t engine_state_size(AnygmEngine *engine){
  if(!g_loaded) return 0;
  CoreW measure={0};
  measure.ok=1;
  state_write(engine,&measure);
  return measure.ok?measure.pos:0;
}
static bool engine_state_save(AnygmEngine *engine,void *d,size_t n,size_t *written){
  if(!g_loaded || !d) return false;
  CoreW s={(uint8_t*)d,n,0,1}; state_write(engine,&s);
  if(written) *written=s.pos;
  if(s.pos > n){
    /* Log the first few overflows and then a heartbeat without flooding a host that retries. */
    g_diag.state_overflow_count++;
    if(g_diag.state_overflow_count <= 3 || g_diag.state_overflow_count % 600 == 0)
      engine_logf(engine,ANYGM_LOG_WARN,
                  "[anygm] state needs %llu bytes but the supplied buffer holds %llu (x%ld)\n",
                  (unsigned long long)s.pos,(unsigned long long)n,
                  g_diag.state_overflow_count);
  }
  return s.ok && s.pos<=n;
}
static bool state_unserialize_impl(AnygmEngine *engine,const void *d, size_t n, int schedule_reapply){
  if(!g_loaded || !d) return false;
  AnygmStateHeader header={0};
  if(!state_header_read(engine,d,n,&header)) return false;
  uint64_t tp0=0,tp1=0,tp2=0,tp3=0;
  int st_time=anygm_host_development_setting(&g_host,"GML_DBG_STATE_TIME")!=NULL;
  if(st_time) tp0=anygm_host_monotonic_time_ns(&g_host);
  const uint8_t *payload=(const uint8_t*)d+ANYGM_STATE_HEADER_SIZE;
  size_t core_size=(size_t)header.core_size;
  size_t render_size=(size_t)header.render_size;
  size_t vm_size=(size_t)header.vm_size;
  size_t audio_size=(size_t)header.audio_size;
  CoreR core={payload,core_size,0,1};
  g_w=cr_u32(&core); g_h=cr_u32(&core); g_bg=cr_u32(&core); g_fps=cr_d(&core);
  if(g_w>FB_MAX_W || g_h>FB_MAX_H || g_w==0 || g_h==0 || !isfinite(g_fps) || g_fps<=0) return false;
  g_follow_player=cr_i32(&core); g_player_obj=cr_i32(&core); g_audio_acc=cr_d(&core);
  if(!isfinite(g_audio_acc)) return false;
  int64_t frame=cr_i64(&core);
  if(frame<0) frame=0;
  if(frame>(int64_t)LONG_MAX) frame=(int64_t)LONG_MAX;
  { g_vm.frame=(long)frame; }
  cr_raw(&core,g_pad_cur,sizeof(g_pad_cur)); cr_raw(&core,g_pad_prev,sizeof(g_pad_prev));
  cr_raw(&core,g_key_cur,sizeof(g_key_cur)); cr_raw(&core,g_key_prev,sizeof(g_key_prev));
  if(!core.ok || core.pos!=core.cap) return false;
  memset(g_pad_cur,0,sizeof(g_pad_cur));
  memset(g_pad_prev,0,sizeof(g_pad_prev));
  memset(g_key_cur,0,sizeof(g_key_cur));
  memset(g_key_prev,0,sizeof(g_key_prev));
  memset(g_hw_key_cur,0,sizeof(g_hw_key_cur));
  memset(g_hw_key_prev,0,sizeof(g_hw_key_prev));
  memset(g_evt_vk_cur,0,sizeof(g_evt_vk_cur));
  memset(g_evt_vk_prev,0,sizeof(g_evt_vk_prev));
  memset(g_evt_key_cur,0,sizeof(g_evt_key_cur));
  memset(g_evt_key_prev,0,sizeof(g_evt_key_prev));
  memset(g_axis_cur,0,sizeof(g_axis_cur));
  memset(g_axis_prev,0,sizeof(g_axis_prev));
  memset(g_mb_cur,0,sizeof(g_mb_cur));
  memset(g_mb_prev,0,sizeof(g_mb_prev));
  g_mouse_wheel = 0;
  size_t offset=core_size;
  CoreR render={payload+offset,render_size,0,1};
  if(!state_read_render(engine,&render) || !render.ok || render.pos!=render.cap){
    if(anygm_host_development_setting(&g_host,"GML_LOG_STATE"))
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state] render section rejected (used=%" PRIu64 " size=%" PRIu64 " ok=%d)\n",
              (uint64_t)render.pos,(uint64_t)render.cap,render.ok);
    return false;
  }
  offset+=render_size;
  if(st_time) tp1=anygm_host_monotonic_time_ns(&g_host);
  size_t used=0;
  if(!gml_vm_state_load(&g_vm,payload+offset,vm_size,&used) || used!=vm_size){
    if(anygm_host_development_setting(&g_host,"GML_LOG_STATE"))
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state] VM section rejected (used=%" PRIu64 " size=%" PRIu64 ")\n",(uint64_t)used,(uint64_t)vm_size);
    return false;
  }
  offset+=vm_size;
  if(st_time) tp2=anygm_host_monotonic_time_ns(&g_host);
  used=0;
  if(!gml_audio_state_load(g_audio,payload+offset,audio_size,&used) || used!=audio_size){
    if(anygm_host_development_setting(&g_host,"GML_LOG_STATE"))
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state] audio section rejected (used=%" PRIu64 " size=%" PRIu64 ")\n",(uint64_t)used,(uint64_t)audio_size);
    return false;
  }
  offset+=audio_size;
  if(anygm_host_development_setting(&g_host,"GML_REENTER_ROOM")){ extern void gml_room_enter(GmlVM*,int); if(g_vm.room_index>=0) gml_room_enter(&g_vm,g_vm.room_index); }
  if(st_time){ tp3=anygm_host_monotonic_time_ns(&g_host);
    #define MS(a,b) ((double)((b)-(a))/1000000.0)
    if(g_diag.state_time_prints++<8)
      engine_logf(engine,ANYGM_LOG_DEBUG,"[state-time] render=%.2fms vm=%.2fms audio=%.2fms\n",
        MS(tp0,tp1), MS(tp1,tp2), MS(tp2,tp3));
    #undef MS
  }
  g_vm.render=&g_render; g_vm.audio=g_audio;
  g_vm.draw_event_hook = aspect_draw_event_hook;
  g_vm.draw_event_hook_user = engine;
  /* The restored VM decides whether this is an ended state. Leave one presentation pass available
   * so its framebuffer can be reconstructed, then the normal Game End handling freezes it. */
  g_runtime_ended = 0;
  g_shutdown_sent = 0;
  g_fps_room = -1;
  sync_room_fps(engine,1);
  classic_transition_reset(engine);
  g_have_presented_frame = 0;
  g_state_just_loaded = schedule_reapply ? 1 : 0;
  return offset==(size_t)header.payload_size;
}
static bool engine_state_load(AnygmEngine *engine,const void *d,size_t n){
  if(!g_loaded || !d) return false;
  AnygmStateHeader target_header={0};
  if(!state_header_read(engine,d,n,&target_header)) return false;
  size_t target_size=(size_t)target_header.total_size;

  CoreW measure={0}; measure.ok=1;
  state_write(engine,&measure);
  if(!measure.ok || !measure.pos) return false;
  size_t snapshot_size=measure.pos;
  uint8_t *snapshot=malloc(snapshot_size);
  if(!snapshot) return false;
  CoreW snapshot_writer={snapshot,snapshot_size,0,1};
  state_write(engine,&snapshot_writer);
  if(!snapshot_writer.ok || snapshot_writer.pos!=snapshot_size){
    free(snapshot);
    return false;
  }

  int prior_just_loaded=g_state_just_loaded;
  size_t prior_reapply_size=g_state_reapply_size;
  if(!state_unserialize_impl(engine,d,target_size,1)){
    int restored=state_unserialize_impl(engine,snapshot,snapshot_size,0);
    g_state_just_loaded=prior_just_loaded;
    g_state_reapply_size=prior_reapply_size;
    free(snapshot);
    if(!restored)
      engine_logf(engine,ANYGM_LOG_ERROR,"[anygm] state rollback failed after a rejected load\n");
    return false;
  }
  if(target_size>g_state_reapply_cap){
    uint8_t *next=realloc(g_state_reapply,target_size?target_size:1);
    if(!next){
      int restored=state_unserialize_impl(engine,snapshot,snapshot_size,0);
      g_state_just_loaded=prior_just_loaded;
      g_state_reapply_size=prior_reapply_size;
      free(snapshot);
      if(!restored)
        engine_logf(engine,ANYGM_LOG_ERROR,"[anygm] state rollback failed after an allocation error\n");
      return false;
    }
    g_state_reapply=next;
    g_state_reapply_cap=target_size;
  }
  memcpy(g_state_reapply,d,target_size);
  g_state_reapply_size=target_size;
  free(snapshot);
  return true;
}
/* ============================================================================================
 * Generic data-driven cheat engine.
 *
 * A cheat code is one caller-supplied action per line:
 *
 *   room=N                      one-shot: warp to room index N
 *   name=V   | name[i]=V        freeze global ARRAY element   (GM8 / indexed reads)
 *   $name=V                     freeze global SCALAR          (GMS scalar reads; avoids V_ARR->0)
 *   obj:var=V | obj:var[i]=V    freeze a numeric var on every instance of object `obj`
 *   @field=V                    engine state and data-declared aspect behavior
 *   obj@suffix->mode            Draw-GUI route: mode = full_view | backdrop | default
 *
 * A line may carry a SCOPE prefix:
 *   ?aspect      ...            only while an Aspect Ratio Force is active
 *   ?aspect=4:3  ...            only while that specific force mode is active (4:3|16:9|21:9)
 * Un-scoped freezes run every frame after the step (god mode, lives, ...). Aspect-scoped writes
 * run from the aspect hook while a force is active (screensizer / view / engine reshaping).
 *
 * A value V is a NUMBER or a token expression, evaluated left-to-right with * / + - :
 *   $base_w  $base_h            the native room resolution
 *   $forced_w $forced_h         the forced-aspect framebuffer resolution
 *   $extra_w $extra_h           forced minus base (the added width / height)
 * e.g.  some_object:some_width=$forced_w            view_wport[0]=$forced_w
 *       some_object:some_x=$forced_w-115            some_object:some_offset=$extra_w*0.5
 * ============================================================================================ */
#define CHEAT_NAMECH(c) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z')||((c)>='0'&&(c)<='9')||(c)=='_')

static void cheat_parse_val(const char *s, CheatVal *v){
  memset(v,0,sizeof *v);
  while(*s==' ') s++;
  if(*s=='$'){
    s++; const char *n=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t L=(size_t)(s-n);
    if     (L==6 && !strncmp(n,"base_w",6))   v->tok=TK_BASE_W;
    else if(L==6 && !strncmp(n,"base_h",6))   v->tok=TK_BASE_H;
    else if(L==8 && !strncmp(n,"forced_w",8)) v->tok=TK_FORCED_W;
    else if(L==8 && !strncmp(n,"forced_h",8)) v->tok=TK_FORCED_H;
    else if(L==7 && !strncmp(n,"extra_w",7))  v->tok=TK_EXTRA_W;
    else if(L==7 && !strncmp(n,"extra_h",7))  v->tok=TK_EXTRA_H;
    else { v->tok=TK_LIT; v->lit=0; return; }
    while(*s && v->nop<6){
      while(*s==' ') s++;
      char o=*s; if(o!='*'&&o!='/'&&o!='+'&&o!='-') break;
      s++; char *end; double num=strtod(s,&end); if(end==s) break;
      v->op[v->nop]=o; v->num[v->nop]=num; v->nop++; s=end;
    }
  } else { v->tok=TK_LIT; v->lit=atof(s); }
}
static double cheat_val_eval(AnygmEngine *engine,const CheatVal *v){
  double x;
  switch(v->tok){
    case TK_BASE_W:   x=(double)g_base_w; break;
    case TK_BASE_H:   x=(double)g_base_h; break;
    case TK_FORCED_W: x=(double)g_w; break;
    case TK_FORCED_H: x=(double)g_h; break;
    case TK_EXTRA_W:  x=(double)g_w-(double)g_base_w; break;
    case TK_EXTRA_H:  x=(double)g_h-(double)g_base_h; break;
    default:          return v->lit;
  }
  for(int i=0;i<v->nop;i++){ double n=v->num[i];
    switch(v->op[i]){ case '*': x*=n; break; case '/': if(n!=0) x/=n; break;
                      case '+': x+=n; break; case '-': x-=n; break; } }
  return x;
}
static void cheat_parse(const char *code, CheatAct *a){
  memset(a,0,sizeof *a);
  const char *s=code; while(*s==' '||*s=='\t') s++;
  if(!strncmp(s,"?aspect",7)){
    a->scope_aspect=1; s+=7;
    if(*s=='='){ s++;
      if     (!strncmp(s,"4:3",3)) { a->scope_mode=GMC_ASPECT_FORCE_4_3;  s+=3; }
      else if(!strncmp(s,"16:9",4)){ a->scope_mode=GMC_ASPECT_FORCE_16_9; s+=4; }
      else if(!strncmp(s,"21:9",4)){ a->scope_mode=GMC_ASPECT_FORCE_21_9; s+=4; }
    }
    while(*s==' '||*s=='\t') s++;
  }
  if(*s=='@'){                                   /* engine: @field=V */
    s++; const char *n=s; while(*s && *s!='=' && *s!=' ') s++;
    size_t L=(size_t)(s-n); char f[32]; if(L>=sizeof f) L=sizeof f-1; memcpy(f,n,L); f[L]=0;
    a->kind=CK_ENGINE;
    if     (!strcmp(f,"window_w")) a->eng=EF_WINDOW_W;
    else if(!strcmp(f,"window_h")) a->eng=EF_WINDOW_H;
    else if(!strcmp(f,"gui_w"))    a->eng=EF_GUI_W;
    else if(!strcmp(f,"gui_h"))    a->eng=EF_GUI_H;
    else if(!strcmp(f,"fbw"))      a->eng=EF_FBW;
    else if(!strcmp(f,"fbh"))      a->eng=EF_FBH;
    else if(!strcmp(f,"compositor_fullwidth")) a->eng=EF_COMPOSITOR;
    else if(!strcmp(f,"center_view_target")) a->eng=EF_CENTER_VIEW_TARGET;
    else if(!strcmp(f,"wide_gameplay_view")) a->eng=EF_WIDE_GAMEPLAY_VIEW;
    else { a->kind=CK_NONE; return; }
    while(*s==' ') s++;
    if(*s=='='){ s++; cheat_parse_val(s,&a->val); } else { a->val.tok=TK_LIT; a->val.lit=1; }
    return;
  }
  if(*s=='$'){                                   /* global scalar: $name=V */
    s++; const char *n=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t L=(size_t)(s-n); if(L==0 || L>=sizeof a->obj){ a->kind=CK_NONE; return; }
    memcpy(a->obj,n,L); a->obj[L]=0;
    while(*s==' ') s++; if(*s!='='){ a->kind=CK_NONE; return; }
    s++; cheat_parse_val(s,&a->val); a->kind=CK_GSCALAR; return;
  }
  const char *n=s; while(*s && CHEAT_NAMECH(*s)) s++;      /* leading NAME */
  size_t L=(size_t)(s-n); if(L==0){ a->kind=CK_NONE; return; }
  if(L>=sizeof a->obj) L=sizeof a->obj-1; memcpy(a->obj,n,L); a->obj[L]=0;
  if(!strcmp(a->obj,"room") && *s=='='){ a->kind=CK_ROOM; return; }   /* one-shot, via gml_cheat_apply */
  if(*s==':'){                                   /* instance: obj:var[idx]=V */
    s++; const char *vn=s; while(*s && CHEAT_NAMECH(*s)) s++;
    size_t VL=(size_t)(s-vn); if(VL>=sizeof a->var) VL=sizeof a->var-1; memcpy(a->var,vn,VL); a->var[VL]=0;
    if(*s=='['){ s++; a->idx=atoi(s); while(*s && *s!=']') s++; if(*s==']') s++; }
    while(*s==' ') s++; if(*s!='='){ a->kind=CK_NONE; return; }
    s++; cheat_parse_val(s,&a->val); a->kind=CK_INST; return;
  }
  if(*s=='@'){                                   /* route: obj@suffix->mode */
    s++; const char *sf=s; while(*s && *s!='-' && *s!=' ') s++;
    size_t SL=(size_t)(s-sf); if(SL>=sizeof a->var) SL=sizeof a->var-1; memcpy(a->var,sf,SL); a->var[SL]=0;
    while(*s==' ') s++;
    if(s[0]=='-' && s[1]=='>'){ s+=2; while(*s==' ') s++;
      if     (!strncmp(s,"backdrop",8))  a->route_mode=GMC_ASPECT_DRAW_FULL_VIEW_BACKDROP;
      else if(!strncmp(s,"full_view",9)) a->route_mode=GMC_ASPECT_DRAW_FULL_VIEW;
      else                                a->route_mode=GMC_ASPECT_DRAW_DEFAULT;
      a->kind=CK_ROUTE; return;
    }
    a->kind=CK_NONE; return;
  }
  if(*s=='['){ s++; a->idx=atoi(s); while(*s && *s!=']') s++; if(*s==']') s++; }  /* global array */
  while(*s==' ') s++; if(*s!='='){ a->kind=CK_NONE; return; }
  s++; cheat_parse_val(s,&a->val); a->kind=CK_GARR;
}
static void cheat_apply_one(AnygmEngine *engine,const CheatAct *a){
  switch(a->kind){
    case CK_GSCALAR: gml_set_global_scalar(&g_vm, a->obj, cheat_val_eval(engine,&a->val)); break;
    case CK_GARR:    gml_set_global_arr(&g_vm, a->obj, a->idx, cheat_val_eval(engine,&a->val)); break;
    case CK_INST:    gml_set_inst_var_all(&g_vm, a->obj, a->var, cheat_val_eval(engine,&a->val)); break;
    case CK_ENGINE: { int iv=(int)cheat_val_eval(engine,&a->val);
      switch(a->eng){
        case EF_WINDOW_W: g_vm.window_w=iv; break;
        case EF_WINDOW_H: g_vm.window_h=iv; break;
        case EF_GUI_W:    g_vm.gui_w=iv; break;
        case EF_GUI_H:    g_vm.gui_h=iv; break;
        case EF_FBW:      g_render.fbw=iv; break;
        case EF_FBH:      g_render.fbh=iv; break;
        case EF_COMPOSITOR: case EF_CENTER_VIEW_TARGET: case EF_WIDE_GAMEPLAY_VIEW: break; /* queried by aspect helpers */
      }
      break; }
    default: break;   /* CK_ROOM handled at set-time; CK_ROUTE consulted in the draw hook */
  }
}
/* aspect_phase: 0 = normal post-step sticky pass, 1 = aspect reshaping pass. */
static int cheat_scope_ok(AnygmEngine *engine,const CheatAct *a, int aspect_phase){
  if(a->scope_aspect){
    if(!aspect_phase || !g_aspect_force_active) return 0;
    if(a->scope_mode && a->scope_mode!=g_aspect_force_mode) return 0;
    return 1;
  }
  return !aspect_phase;
}
/* ============================================================================================
 * Generic pause-menu editor. Configuration uses the caller-supplied .cht data and `|` separators
 * so labels may contain spaces:
 *   menu|<object>|<labelvar>|<indexvar>|<1d|2d>   declare which menu to edit (1D string array or 2D)
 *   mtoggle|<LABEL>|<target>|<onval>              ON/OFF entry; when ON, freezes target=onval
 *   mrange|<LABEL>|<target>|<min>|<max>|[start]   left/right adjusts target in [min,max] (frozen)
 *   mwarp|<LABEL>|<roomName1,roomName2,...>        left/right picks a room; confirm warps there
 * <target> = $global  or  obj:var . Labels render live: "GOD MODE: ON", "LIVES: 9", "HEARTS: 3".
 * Only the boot path builds the menu, so API-level runtime override resets do not remove it. */
static void menu_target_parse(const char *s, char *obj, char *var){
  obj[0]=0; var[0]=0;
  if(*s=='$'){ snprintf(var,48,"%s",s+1); return; }
  const char *c=strchr(s,':');
  if(c){ int L=(int)(c-s); if(L>47) L=47; memcpy(obj,s,(size_t)L); obj[L]=0; snprintf(var,48,"%s",c+1); }
  else snprintf(var,48,"%s",s);
}
static int menu_split(const char *code, char f[6][128]){
  int n=0; const char *s=code;
  while(n<6){
    const char *bar=strchr(s,'|');
    int L = bar ? (int)(bar-s) : (int)strlen(s); if(L>127) L=127;
    memcpy(f[n],s,(size_t)L); f[n][L]=0; n++;
    if(!bar) break; s=bar+1;
  }
  return n;
}
/* Returns 1 if the line was a menu directive (and consumed it), 0 otherwise. */
static int menu_directive_parse(AnygmEngine *engine,const char *code){
  while(*code==' '||*code=='\t') code++;
  char f[6][128];
  if(!strncmp(code,"menu|",5)){
    int n=menu_split(code,f);
    if(n>=4){
      snprintf(g_menu.obj,48,"%s",f[1]); snprintf(g_menu.labelvar,48,"%s",f[2]); snprintf(g_menu.idxvar,48,"%s",f[3]);
      g_menu.is2d = (n>=5 && (!strcmp(f[4],"2d")||!strcmp(f[4],"2D")));
      g_menu.lift = (n>=6) ? atoi(f[5]) : 0;   /* pixels to raise the menu on open, so extra rows fit */
      g_menu.active=1;
    }
    return 1;
  }
  if(!strncmp(code,"mset|",5)){        /* force an instance var on the menu object while it is open */
    int n=menu_split(code,f);
    if(n>=3 && g_menu.ntweaks<8){ snprintf(g_menu.tweaks[g_menu.ntweaks].var,48,"%s",f[1]); g_menu.tweaks[g_menu.ntweaks].val=atof(f[2]); g_menu.ntweaks++; }
    return 1;
  }
  if(!strncmp(code,"mtoggle|",8) || !strncmp(code,"mrange|",7) || !strncmp(code,"mwarp|",6)){
    if(g_menu.nitems>=16) return 1;
    int n=menu_split(code,f);
    MenuItem *it=&g_menu.items[g_menu.nitems]; memset(it,0,sizeof *it); it->from_boot=1;
    snprintf(it->label,sizeof it->label,"%s",f[1]);
    if(!strcmp(f[0],"mtoggle") && n>=4){ it->kind=MI_TOGGLE; menu_target_parse(f[2],it->tobj,it->tvar); it->onval=atof(f[3]); }
    else if(!strcmp(f[0],"mrange") && n>=5){ it->kind=MI_RANGE; menu_target_parse(f[2],it->tobj,it->tvar);
      it->vmin=atoi(f[3]); it->vmax=atoi(f[4]); it->value = (n>=6)?atoi(f[5]):it->vmin;
      if(it->value<it->vmin) it->value=it->vmin; if(it->value>it->vmax) it->value=it->vmax; }
    else if(!strcmp(f[0],"mwarp") && n>=3){ it->kind=MI_WARP; snprintf(it->roomcsv,sizeof it->roomcsv,"%s",f[2]); }
    else return 1;
    g_menu.nitems++;
    return 1;
  }
  return 0;
}
static int menu_decl_code(const char *code){
  if(!code) return 0;
  while(*code==' '||*code=='\t') code++;
  return !strncmp(code,"menu|",5);
}
/* Rebuild the editor from enabled cheat slots. A host-supplied menu declaration replaces the
 * boot menu declaration and its directives; ordinary host cheats still layer over boot cheats.
 * This keeps manual override-file loading functional without duplicating boot rows. */
static void menu_rebuild(AnygmEngine *engine){
  int host_menu=0;
  for(int i=0;i<g_cheat_n;i++)
    if(g_cheats[i].enabled && menu_decl_code(g_cheats[i].code)){ host_menu=1; break; }
  memset(&g_menu,0,sizeof g_menu);
  const CheatSlot *arr=host_menu?g_cheats:g_boot_cheats;
  int n=host_menu?g_cheat_n:g_boot_cheat_n;
  for(int i=0;i<n;i++) if(arr[i].enabled) menu_directive_parse(engine,arr[i].code);
}
static void engine_override_reset(AnygmEngine *engine){
  g_cheat_n = 0; memset(g_cheats, 0, sizeof g_cheats);
  menu_rebuild(engine);
}
static void engine_override_set(AnygmEngine *engine,unsigned i,bool e,const char *c){
  if(!c || i >= GML_MAX_CHEATS) return;
  if((int)i >= g_cheat_n) g_cheat_n = (int)i + 1;
  g_cheats[i].enabled = e ? 1 : 0;
  snprintf(g_cheats[i].code, sizeof g_cheats[i].code, "%s", c);
  cheat_parse(g_cheats[i].code, &g_cheats[i].act);
  menu_rebuild(engine);
  if(e && g_loaded && g_cheats[i].act.kind==CK_ROOM)
    gml_cheat_apply(&g_vm, g_cheats[i].code);   /* fire the one-shot warp now */
}
/* Consumer passes run over both API-provided overrides and boot overrides. Boot entries remain
 * active when the caller resets the API-provided slots. */
static void cheat_sticky_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind==CK_ROOM || a->kind==CK_NONE) continue;
    if(!cheat_scope_ok(engine,a,0)) continue;
    cheat_apply_one(engine,a);
  }
}
/* Re-apply un-scoped freeze cheats — called every frame after the game step. */
static void apply_sticky_cheats(AnygmEngine *engine){
  cheat_sticky_pass(engine,g_cheats, g_cheat_n);
  cheat_sticky_pass(engine,g_boot_cheats, g_boot_cheat_n);
}
static void cheat_aspect_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(!a->scope_aspect || a->kind==CK_ROUTE) continue;
    if(!cheat_scope_ok(engine,a,1)) continue;
    cheat_apply_one(engine,a);
  }
}
/* Apply aspect-scoped writes to screen, view, and engine values on every forced-aspect frame. The
 * executable core contains only the generic interpreter; content-specific values are external. */
static void aspect_apply_program(AnygmEngine *engine){
  if(!g_aspect_force_active) return;
  cheat_aspect_pass(engine,g_cheats, g_cheat_n);
  cheat_aspect_pass(engine,g_boot_cheats, g_boot_cheat_n);
}
static int cheat_compositor_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ENGINE || a->eng!=EF_COMPOSITOR || !a->scope_aspect) continue;
    if(a->scope_mode && a->scope_mode!=g_aspect_force_mode) continue;
    if(cheat_val_eval(engine,&a->val)!=0.0) return 1;
  }
  return 0;
}
/* Any enabled `?aspect @compositor_fullwidth=1` for the active mode runs the Draw-GUI
 * compositor at the full forced-wide resolution instead of a centered sub-rect. */
static int aspect_compositor_fullwidth_gen(AnygmEngine *engine){
  return cheat_compositor_pass(engine,g_cheats, g_cheat_n)
      || cheat_compositor_pass(engine,g_boot_cheats, g_boot_cheat_n);
}
static int cheat_center_target_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ENGINE || a->eng!=EF_CENTER_VIEW_TARGET || !a->scope_aspect) continue;
    if(a->scope_mode && a->scope_mode!=g_aspect_force_mode) continue;
    if(cheat_val_eval(engine,&a->val)!=0.0) return 1;
  }
  return 0;
}
static int aspect_center_view_target_gen(AnygmEngine *engine){
  return cheat_center_target_pass(engine,g_cheats, g_cheat_n)
      || cheat_center_target_pass(engine,g_boot_cheats, g_boot_cheat_n);
}
static int cheat_wide_gameplay_pass(AnygmEngine *engine,const CheatSlot *arr, int n){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ENGINE || a->eng!=EF_WIDE_GAMEPLAY_VIEW || !a->scope_aspect) continue;
    if(a->scope_mode && a->scope_mode!=g_aspect_force_mode) continue;
    if(cheat_val_eval(engine,&a->val)!=0.0) return 1;
  }
  return 0;
}
static int aspect_wide_gameplay_view_gen(AnygmEngine *engine){
  return cheat_wide_gameplay_pass(engine,g_cheats, g_cheat_n)
      || cheat_wide_gameplay_pass(engine,g_boot_cheats, g_boot_cheat_n);
}
static int cheat_route_pass(AnygmEngine *engine,const CheatSlot *arr, int n, const char *obj, const char *suffix){
  for(int i=0;i<n;i++){
    if(!arr[i].enabled) continue;
    const CheatAct *a=&arr[i].act;
    if(a->kind!=CK_ROUTE) continue;
    if(a->scope_mode && a->scope_mode!=g_aspect_force_mode) continue;
    if(strcmp(a->obj,obj) || strcmp(a->var,suffix)) continue;
    return a->route_mode;
  }
  return -1;
}
/* Draw-GUI route table: return the routing mode for an instance object and event suffix, or the
 * default. Routing applies only when the frame is widened. */
static int aspect_draw_full_view_gen(AnygmEngine *engine,const GmlInstance *in, const char *suffix){
  if(!in || !suffix || g_w<=g_base_w) return GMC_ASPECT_DRAW_DEFAULT;
  const char *obj = (in->obj>=0 && in->obj<g_vm.n_objects) ? g_vm.objects[in->obj].name : NULL;
  if(!obj) return GMC_ASPECT_DRAW_DEFAULT;
  int r = cheat_route_pass(engine,g_cheats, g_cheat_n, obj, suffix);
  if(r < 0) r = cheat_route_pass(engine,g_boot_cheats, g_boot_cheat_n, obj, suffix);
  return r < 0 ? GMC_ASPECT_DRAW_DEFAULT : r;
}
/* Fresh press this frame from either the RetroPad button or a keyboard key. */
static int menu_edge(AnygmEngine *engine,int pad, int vk){
  int cur  = g_pad_cur[pad]  | ((vk>=0&&vk<NKEY)?(g_key_cur[vk]|g_hw_key_cur[vk]|g_evt_vk_cur[vk]):0);
  int prev = g_pad_prev[pad] | ((vk>=0&&vk<NKEY)?(g_key_prev[vk]|g_hw_key_prev[vk]|g_evt_vk_prev[vk]):0);
  return cur && !prev;
}
static void menu_set_target(AnygmEngine *engine,const MenuItem *it, double v){
  if(it->tobj[0]) gml_set_inst_var_all(&g_vm, it->tobj, it->tvar, v);
  else            gml_set_global_scalar(&g_vm, it->tvar, v);
}
/* Apply enabled item effects every frame. While the configured pause menu is open, inject entries
 * and handle left, right, and confirm input. */
static void menu_run(AnygmEngine *engine){
  if(!g_menu.active || !g_loaded) return;
  /* Toggles (god mode) freeze their target every frame while ON. Ranges are set-on-change (below),
   * not frozen, so LIVES/HEARTS act as setters/refills you can still play against. */
  for(int i=0;i<g_menu.nitems;i++){ MenuItem *it=&g_menu.items[i]; if(it->kind==MI_TOGGLE && it->on) menu_set_target(engine,it,it->onval); }

  int oi=gml_object_index_by_name(&g_vm,g_menu.obj);
  GmlInstance *in = oi>=0 ? gml_find_instance(&g_vm,oi) : NULL;
  if(!in){ g_menu.open_prev=0; return; }
  if(!g_menu.open_prev){
    g_menu.base = gml_inst_array_count(in,g_menu.labelvar);
    if(g_menu.lift) in->y -= (double)g_menu.lift;   /* raise the menu once so the injected rows fit on screen */
    g_menu.open_prev=1;
  }
  /* Force configured layout variables every frame so cached menu layouts retain the added rows. */
  for(int t=0;t<g_menu.ntweaks;t++) gml_set_inst_var_all(&g_vm, g_menu.obj, g_menu.tweaks[t].var, g_menu.tweaks[t].val);
  int base=g_menu.base;
  if(base<0 || base>200) return;

  int sel = gml_inst_get_num(in,g_menu.idxvar);
  int mi = sel - base;
  int left  = menu_edge(engine,ANYGM_PAD_LEFT, 37);
  int right = menu_edge(engine,ANYGM_PAD_RIGHT, 39);
  int conf  = menu_edge(engine,ANYGM_PAD_FACE_RIGHT, 13) || menu_edge(engine,ANYGM_PAD_FACE_BOTTOM, -1);
  if(mi>=0 && mi<g_menu.nitems){
    MenuItem *it=&g_menu.items[mi];
    if(it->kind==MI_TOGGLE){ if(conf) it->on=!it->on; }
    else if(it->kind==MI_RANGE){
      int old=it->value;
      if(left  && it->value>it->vmin) it->value--;
      if(right && it->value<it->vmax) it->value++;
      if(it->value!=old || conf) menu_set_target(engine,it,(double)it->value);   /* set / refill on change or confirm */
    }
    else if(it->kind==MI_WARP){
      if(!it->resolved){
        char csv[128]; snprintf(csv,sizeof csv,"%s",it->roomcsv); char *sv=NULL;
        for(char *t=strtok_r(csv,",",&sv); t && it->nrooms<24; t=strtok_r(NULL,",",&sv)){
          int ri=gml_room_index_by_name(g_vm.win,t); if(ri>=0) it->rooms[it->nrooms++]=ri;
        }
        it->resolved=1;
      }
      if(it->nrooms>0){
        if(left)  it->warpsel=(it->warpsel-1+it->nrooms)%it->nrooms;
        if(right) it->warpsel=(it->warpsel+1)%it->nrooms;
        if(conf){ char rb[24]; snprintf(rb,sizeof rb,"room=%d",it->rooms[it->warpsel]); gml_cheat_apply(&g_vm,rb); }
      }
    }
  }
  /* (re)write the injected entries at rows [base .. base+nitems-1] every frame so labels stay live */
  for(int k=0;k<g_menu.nitems && base+k<254;k++){
    MenuItem *it=&g_menu.items[k];
    char buf[40];
    if(it->kind==MI_TOGGLE)      snprintf(buf,sizeof buf,"%s: %s", it->label, it->on?"ON":"OFF");
    else if(it->kind==MI_RANGE)  snprintf(buf,sizeof buf,"%s: %d", it->label, it->value);
    else                          snprintf(buf,sizeof buf,"%s: %d", it->label, it->warpsel+1);
    int row=base+k;
    gml_inst_array_set_str(in,g_menu.labelvar,g_menu.is2d,row,0,buf);
    if(g_menu.is2d) gml_inst_array_set_num(in,g_menu.labelvar,g_menu.is2d,row,1, 900.0+k);   /* ignored sentinel code */
  }
}
/* Generic room skip: Select or Start advances to the next room in play order on a fresh press.
 * Face buttons remain untouched. */
static void room_skip_hook(AnygmEngine *engine){
  int btn=-1;
  if(g_config.room_skip_button==1u) btn=ANYGM_PAD_SELECT;
  else if(g_config.room_skip_button==2u) btn=ANYGM_PAD_START;
  if(btn < 0) return;
  if(g_pad_cur[btn] && !g_pad_prev[btn] && g_vm.pending_room < 0){
    int room = g_vm.room_index, ord = -1;
    for(int i=0;i<g_win.n_room_order;i++) if((int)g_win.room_order[i]==room){ ord=i; break; }
    if(ord>=0 && ord+1<g_win.n_room_order) gml_vm_goto_room_order(&g_vm, ord+1);
  }
}
/* GML_SELFTEST checks pure builtins with known inputs. */
static void run_selftest(AnygmEngine *engine){
  if(!anygm_host_development_setting(&g_host,"GML_SELFTEST")) return;
  extern GmlVal gml_builtin_call(GmlVM*, const char*, GmlVal*, int);
  int pass=0, tot=0; GmlVal a[12];
  #define BI(nm,cnt) gml_builtin_call(&g_vm,(nm),a,(cnt))
  #define CHK(desc,cond) do{ tot++; if(cond) pass++; else engine_logf(engine,ANYGM_LOG_DEBUG,"[selftest] FAIL %s\n",desc); }while(0)
  a[0]=vreal(200); a[1]=vreal(100); a[2]=vreal(50);
  int col=(int)BI("make_color_rgb",3).d;
  CHK("make_color legacy alias", (int)BI("make_color",3).d==col);
  a[0]=vreal(col);
  CHK("color_get_red",   (int)BI("color_get_red",1).d==200);
  CHK("color_get_green", (int)BI("color_get_green",1).d==100);
  CHK("color_get_blue",  (int)BI("color_get_blue",1).d==50);
  a[0]=vreal(5);a[1]=vreal(5);a[2]=vreal(0);a[3]=vreal(0);a[4]=vreal(10);a[5]=vreal(10);
  CHK("point_in_rectangle in",  (int)BI("point_in_rectangle",6).d==1);
  a[0]=vreal(15);
  CHK("point_in_rectangle out", (int)BI("point_in_rectangle",6).d==0);
  a[0]=vreal(2);a[1]=vreal(2);a[2]=vreal(0);a[3]=vreal(0);a[4]=vreal(10);a[5]=vreal(0);a[6]=vreal(0);a[7]=vreal(10);
  CHK("point_in_triangle", (int)BI("point_in_triangle",8).d==1);
  a[0]=vreal(10);a[1]=vreal(350);
  CHK("angle_difference",  (int)BI("angle_difference",2).d==20);
  a[0]=vstr("Hello, World!"); GmlVal enc=BI("base64_encode",1);
  CHK("base64_encode", enc.t==V_STR && enc.s && !strcmp(enc.s,"SGVsbG8sIFdvcmxkIQ=="));
  a[0]=enc; GmlVal dec=BI("base64_decode",1);
  CHK("base64_roundtrip", dec.t==V_STR && dec.s && !strcmp(dec.s,"Hello, World!"));
  a[0]=vstr("SGk="); GmlVal b64buf=BI("buffer_base64_decode",1);
  a[0]=b64buf; CHK("buffer_base64_decode size", (int)BI("buffer_get_size",1).d==2);
  a[0]=b64buf; a[1]=vreal(0); a[2]=vreal(0); BI("buffer_seek",3);
  a[0]=b64buf; a[1]=vreal(1); GmlVal b64a=BI("buffer_read",2);
  a[0]=b64buf; a[1]=vreal(1); GmlVal b64b=BI("buffer_read",2);
  CHK("buffer_base64_decode bytes", (int)b64a.d=='H' && (int)b64b.d=='i');
  a[0]=b64buf; BI("buffer_delete",1);
  a[0]=vstr("{\"a\":1,\"b\":\"x\"}"); GmlVal jsobj=BI("json_parse",1);
  CHK("json_parse struct", jsobj.t==V_REAL && GML_IS_STRUCT_ID(jsobj.d));
  a[0]=jsobj; a[1]=vstr("a"); GmlVal jsa=BI("variable_struct_get",2);
  CHK("variable_struct_get", jsa.t==V_REAL && (int)jsa.d==1);
  a[0]=jsobj; a[1]=vstr("c"); a[2]=vreal(3); BI("variable_struct_set",3);
  a[0]=jsobj; a[1]=vstr("c");
  CHK("variable_struct_exists", (int)BI("variable_struct_exists",2).d==1);
  a[0]=jsobj; GmlVal jss=BI("json_stringify",1);
  CHK("json_stringify struct", jss.t==V_STR && jss.s && strstr(jss.s,"\"a\":1") && strstr(jss.s,"\"c\":3"));
  a[0]=jsobj; a[1]=vstr("c"); BI("variable_struct_remove",2);
  CHK("variable_struct_remove", (int)BI("variable_struct_exists",2).d==0);
  a[0]=vstr("[1,\"z\",true]"); GmlVal jsarr=BI("json_parse",1); a[0]=jsarr; GmlVal arrs=BI("json_stringify",1);
  CHK("json_parse array", arrs.t==V_STR && arrs.s && !strcmp(arrs.s,"[1,\"z\",1]"));
  a[0]=vstr("[3,1,2]"); GmlVal sortarr=BI("json_parse",1);
  a[0]=sortarr; a[1]=vreal(1); BI("array_sort",2); a[0]=sortarr; GmlVal sorts=BI("json_stringify",1);
  CHK("array_sort ascending", sorts.t==V_STR && sorts.s && !strcmp(sorts.s,"[1,2,3]"));
  a[0]=vstr("gml_selftest_global"); a[1]=vreal(42); BI("variable_global_set",2);
  a[0]=vstr("gml_selftest_global"); GmlVal ggot=BI("variable_global_get",1);
  CHK("variable_global_get", ggot.t==V_REAL && (int)ggot.d==42);
  a[0]=vstr("AZ"); a[1]=vreal(2); CHK("string_byte_at", (int)BI("string_byte_at",2).d==90);
  a[0]=vstr("alpha beta gamma delta");
  double text_plain_w=BI("string_width",1).d;
  a[0]=vstr("alpha beta gamma delta"); a[1]=vreal(-1); a[2]=vreal(text_plain_w*0.60);
  double text_wrap_w=BI("string_width_ext",3).d;
  a[0]=vstr("alpha beta gamma delta");
  double text_plain_h=BI("string_height",1).d;
  a[0]=vstr("alpha beta gamma delta"); a[1]=vreal(-1); a[2]=vreal(text_plain_w*0.60);
  double text_wrap_h=BI("string_height_ext",3).d;
  CHK("string_width_ext wraps", text_plain_w>0 && text_wrap_w>0 && text_wrap_w<text_plain_w);
  CHK("string_height_ext wraps", text_plain_h>0 && text_wrap_h>text_plain_h);
  a[0]=vreal(1);a[1]=vreal(1);a[2]=vreal(2);a[3]=vreal(2);a[4]=vreal(0);a[5]=vreal(0);a[6]=vreal(5);a[7]=vreal(0);a[8]=vreal(0);a[9]=vreal(5);
  CHK("rectangle_in_triangle", (int)BI("rectangle_in_triangle",10).d==1);
  GmlVal lid=BI("ds_list_create",0);
  a[0]=lid; a[1]=vreal(3); a[2]=vreal(1); BI("ds_list_add",3);
  a[0]=lid; a[1]=vreal(1); a[2]=vreal(2); BI("ds_list_replace",3);
  a[0]=lid; a[1]=vreal(0); BI("ds_list_sort",2);
  a[0]=lid; a[1]=vreal(0); CHK("ds_list_sort", (int)BI("ds_list_find_value",2).d==3);
  GmlVal mid=BI("ds_map_create",0); a[0]=mid; CHK("ds_map_empty true", (int)BI("ds_map_empty",1).d==1);
  a[0]=mid; a[1]=vstr("k"); a[2]=vreal(7); BI("ds_map_set",3);
  a[0]=mid; CHK("ds_map_empty false", (int)BI("ds_map_empty",1).d==0);
  a[0]=vreal(3); a[1]=vreal(3); GmlVal gid=BI("ds_grid_create",2);
  a[0]=gid; a[1]=vreal(1); a[2]=vreal(2); a[3]=vreal(5); BI("ds_grid_set_post",4);
  a[0]=gid; a[1]=vreal(1); a[2]=vreal(2); a[3]=vreal(2); BI("ds_grid_add",4);
  a[0]=gid; a[1]=vreal(1); a[2]=vreal(2); CHK("ds_grid_add", (int)BI("ds_grid_get",3).d==7);
  a[0]=gid; a[1]=vreal(0); a[2]=vreal(0); a[3]=vreal(2); a[4]=vreal(2); a[5]=vreal(7);
  CHK("ds_grid_value_x", (int)BI("ds_grid_value_x",6).d==1);
  CHK("ds_grid_value_y", (int)BI("ds_grid_value_y",6).d==2);
  GmlInstance *ti=NULL;
  for(int i=0;i<g_vm.inst_count;i++) if(g_vm.inst[i].active && !g_vm.inst[i].marked){ ti=&g_vm.inst[i]; break; }
  if(ti){
    a[0]=vreal((double)ti->id); a[1]=vstr("x");
    CHK("variable_instance_exists builtin", (int)BI("variable_instance_exists",2).d==1);
    a[0]=vreal((double)ti->id); a[1]=vstr("gml_selftest_field"); a[2]=vstr("ok"); BI("variable_instance_set",3);
    a[0]=vreal((double)ti->id); a[1]=vstr("gml_selftest_field"); GmlVal igot=BI("variable_instance_get",2);
    CHK("variable_instance_get", igot.t==V_STR && igot.s && !strcmp(igot.s,"ok"));
  } else CHK("variable_instance target", 0);
  engine_logf(engine,ANYGM_LOG_DEBUG,"[selftest] %d/%d builtin checks passed\n", pass, tot);
  #undef CHK
  #undef BI
}
/* GML_INTROSKIP="1,2,4,5" or "1-4,9": in the listed room indices, A or B advances to the next
 * room in play order. The room list is supplied at launch. */
static void introskip_parse(AnygmEngine *engine,const char *s){
  memset(g_introskip_set, 0, sizeof g_introskip_set);
  g_introskip_on = (s && *s) ? 1 : 0;
  if(!g_introskip_on) return;
  while(*s){
    while(*s==' '||*s==',') s++;
    if(!*s) break;
    int a=atoi(s); while(*s && *s!=',' && *s!='-' && *s!=' ') s++;
    int b=a;
    if(*s=='-'){ s++; b=atoi(s); while(*s && *s!=',' && *s!=' ') s++; }
    if(a>b){ int t=a; a=b; b=t; }
    for(int r=a; r<=b && r<1024; r++) if(r>=0) g_introskip_set[r>>3] |= (uint8_t)(1u<<(r&7));
  }
}
static void introskip_hook(AnygmEngine *engine){
  if(g_introskip_on < 0) introskip_parse(engine,anygm_host_development_setting(&g_host,"GML_INTROSKIP"));
  if(!g_introskip_on) return;
  int room = g_vm.room_index;
  if(room < 0 || room >= 1024) return;
  if(!(g_introskip_set[room>>3] & (1u<<(room&7)))) return;   /* not a listed intro room */
  int a = g_pad_cur[ANYGM_PAD_FACE_RIGHT] && !g_pad_prev[ANYGM_PAD_FACE_RIGHT];
  int b = g_pad_cur[ANYGM_PAD_FACE_BOTTOM] && !g_pad_prev[ANYGM_PAD_FACE_BOTTOM];
  if((a||b) && g_vm.pending_room < 0){
    int ord=-1; for(int i=0;i<g_win.n_room_order;i++) if((int)g_win.room_order[i]==room){ ord=i; break; }
    if(ord>=0 && ord+1<g_win.n_room_order) gml_vm_goto_room_order(&g_vm, ord+1);
  }
}

uint32_t anygm_api_version(void){
  return ANYGM_API_VERSION;
}

AnygmResult anygm_create(const AnygmHostServices *services,AnygmEngine **out_engine){
  if(!out_engine) return ANYGM_ERROR_INVALID_ARGUMENT;
  *out_engine=NULL;
  if(services && (services->abi_version!=ANYGM_HOST_SERVICES_VERSION ||
                  services->struct_size<sizeof(AnygmHostServices)))
    return ANYGM_ERROR_INCOMPATIBLE_ABI;
  AnygmEngine *engine=calloc(1,sizeof *engine);
  if(!engine) return ANYGM_ERROR_OUT_OF_MEMORY;
  engine->guard=ANYGM_ENGINE_GUARD;
  if(services) memcpy(&g_host,services,sizeof g_host);
  else memset(&g_host,0,sizeof g_host);
  memset(&g_config,0,sizeof g_config);
  g_config.struct_size=sizeof g_config;
  g_config.embedded_shaders=1;
  g_config.crt_mask=1;
  g_config.crt_scanlines=1;
  g_config.crt_gamma=1;
  g_config.crt_curvature=-1;
  g_config.crt_vignette=-1;
  g_config.fast_alpha_cull=24;
  g_config.start_room=-1;
  snprintf(g_language,sizeof g_language,"en");
  snprintf(g_region,sizeof g_region,"us");
  snprintf(g_language_tag,sizeof g_language_tag,"en-US");
  g_w=288; g_h=216; g_base_w=288; g_base_h=216;
  g_fps=60.0; g_fps_room=-1;
  g_player_obj=-1;
  g_mouse_px=-1.0; g_mouse_py=-1.0;
  g_profile_enabled=-1;
  g_diag.key_enabled=-1;
  g_diag.force_present_view=-1;
  g_diag.mouse_frame=-1;
  g_diag.cursor_frame=-1;
  g_diag.multiview_frame=-1;
  g_diag.profile_spike_ms=-2.0;
  g_introskip_on=-1;
  g_last_error[0]=0;
  *out_engine=engine;
  return ANYGM_OK;
}

void anygm_destroy(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return;
  if(engine->lifecycle==ENGINE_LOADED) anygm_unload(engine);
  free(g_state_reapply);
  free(g_classic_phase_mem);
  free(g_fb);
  free(g_screen);
  free(g_guibuf);
  free(g_appcrop);
  engine->guard=0;
  free(engine);
}

AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || !source ||
     source->struct_size<sizeof(AnygmContentSource)) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(config && config->struct_size<sizeof(AnygmLoadConfig)) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(engine->lifecycle!=ENGINE_EMPTY)
    return ANYGM_ERROR_INVALID_STATE;
  if(!ensure_primary_buffers(engine)){
    engine_errorf(engine,ANYGM_ERROR_OUT_OF_MEMORY,"Could not allocate the primary frame buffers");
    return ANYGM_ERROR_OUT_OF_MEMORY;
  }
  AnygmLoadConfig resolved_config={0};
  char host_language[16]={0},host_region[16]={0},host_tag[32]={0};
  const AnygmLoadConfig *load_config=config;
  if(!config && g_host.locale){
    resolved_config.struct_size=sizeof resolved_config;
    if(g_host.locale(g_host.userdata,host_language,sizeof host_language,
                     host_region,sizeof host_region,host_tag,sizeof host_tag)==ANYGM_OK){
      resolved_config.language=host_language[0]?host_language:"en";
      resolved_config.region=host_region[0]?host_region:"us";
      resolved_config.language_tag=host_tag[0]?host_tag:"en-US";
      load_config=&resolved_config;
    }
  }
  AnygmResult result=engine_load_content(engine,source,load_config);
  if(result==ANYGM_OK) engine->lifecycle=ENGINE_LOADED;
  return result;
}

void anygm_unload(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return;
  engine_unload(engine);
  engine_override_reset(engine);
  engine->lifecycle=ENGINE_EMPTY;
}

AnygmResult anygm_reset(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED)
    return ANYGM_ERROR_INVALID_STATE;
  gml_audio_free(g_audio); g_audio=NULL; g_vm.audio=NULL;
  gml_vm_free(&g_vm);
  gml_render_free(&g_render);
  boot_runtime(engine);
  return ANYGM_OK;
}

AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !info)
    return ANYGM_ERROR_INVALID_STATE;
  if(info->struct_size<sizeof *info) return ANYGM_ERROR_INCOMPATIBLE_ABI;
  info->base_width=g_out_w?g_out_w:g_w;
  info->base_height=g_out_h?g_out_h:g_h;
  info->max_width=FB_MAX_W;
  info->max_height=FB_MAX_H;
  info->aspect_ratio=info->base_height?(double)info->base_width/info->base_height:4.0/3.0;
  info->frames_per_second=g_fps;
  info->audio_rate=44100;
  return ANYGM_OK;
}

AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !output)
    return ANYGM_ERROR_INVALID_STATE;
  if(output->struct_size<sizeof *output || (input && input->struct_size<sizeof *input))
    return ANYGM_ERROR_INCOMPATIBLE_ABI;
  if(input) memcpy(&g_input,input,sizeof g_input);
  else { memset(&g_input,0,sizeof g_input); g_input.pointer_x=-1; g_input.pointer_y=-1; }
  g_frame_flags=0;
  AnygmResult result=engine_run_frame(engine);
  if(result!=ANYGM_OK) return result;
  output->pixels=g_screen;
  output->width=g_out_w?g_out_w:g_w;
  output->height=g_out_h?g_out_h:g_h;
  output->pitch=(size_t)output->width*sizeof(uint32_t);
  output->pixel_format=ANYGM_PIXEL_XRGB8888;
  output->audio=g_audio_output;
  output->audio_frames=g_audio_frames;
  output->audio_rate=44100;
  output->flags=g_frame_flags;
  return ANYGM_OK;
}

AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || !delta) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(delta->struct_size<sizeof *delta || delta->values.struct_size<sizeof delta->values)
    return ANYGM_ERROR_INCOMPATIBLE_ABI;
  uint64_t f=delta->fields;
  if(f&ANYGM_CONFIG_PRESENT_WIDTH) g_config.present_width=delta->values.present_width;
  if(f&ANYGM_CONFIG_PRESENT_HEIGHT) g_config.present_height=delta->values.present_height;
  if(f&ANYGM_CONFIG_ASPECT_MODE) g_config.aspect_mode=delta->values.aspect_mode;
  if(f&ANYGM_CONFIG_MOUSE_MODE) g_config.mouse_mode=delta->values.mouse_mode;
  if(f&ANYGM_CONFIG_ROOM_SKIP_BUTTON) g_config.room_skip_button=delta->values.room_skip_button;
  if(f&ANYGM_CONFIG_GOD_MODE) g_config.god_mode=delta->values.god_mode;
  if(f&ANYGM_CONFIG_CRT_MASK) g_config.crt_mask=delta->values.crt_mask;
  if(f&ANYGM_CONFIG_CRT_SCANLINES) g_config.crt_scanlines=delta->values.crt_scanlines;
  if(f&ANYGM_CONFIG_CRT_GAMMA) g_config.crt_gamma=delta->values.crt_gamma;
  if(f&ANYGM_CONFIG_CRT_CURVATURE) g_config.crt_curvature=delta->values.crt_curvature;
  if(f&ANYGM_CONFIG_CRT_VIGNETTE) g_config.crt_vignette=delta->values.crt_vignette;
  if(f&ANYGM_CONFIG_EMBEDDED_SHADERS) g_config.embedded_shaders=delta->values.embedded_shaders;
  if(f&ANYGM_CONFIG_GAMEPAD_CONNECTED) g_config.gamepad_connected=delta->values.gamepad_connected;
  if(f&ANYGM_CONFIG_FAST_ALPHA_CULL) g_config.fast_alpha_cull=delta->values.fast_alpha_cull;
  if(f&ANYGM_CONFIG_FAST_FORWARD) g_config.fast_forward=delta->values.fast_forward;
  if(f&ANYGM_CONFIG_START_ROOM) g_config.start_room=delta->values.start_room;
  if(engine->lifecycle==ENGINE_LOADED){
    poll_option_updates(engine);
    g_vm.god_mode=core_opt_god(engine);
  }
  return ANYGM_OK;
}

AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED)
    return ANYGM_ERROR_INVALID_STATE;
  if(slot>=GML_MAX_CHEATS ||
     (enabled && (!expression || !expression[0] ||
                  strlen(expression)>ANYGM_MAX_RUNTIME_OVERRIDE_EXPRESSION)))
    return ANYGM_ERROR_INVALID_ARGUMENT;
  engine_override_set(engine,slot,enabled!=0,expression?expression:"");
  return ANYGM_OK;
}

size_t anygm_state_size(AnygmEngine *engine){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED) return 0;
  return engine_state_size(engine);
}

AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written){
  if(written) *written=0;
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !data)
    return ANYGM_ERROR_INVALID_STATE;
  return engine_state_save(engine,data,capacity,written)?ANYGM_OK:ANYGM_ERROR_OUT_OF_MEMORY;
}

AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD || engine->lifecycle!=ENGINE_LOADED || !data)
    return ANYGM_ERROR_INVALID_STATE;
  return engine_state_load(engine,data,size)?ANYGM_OK:ANYGM_ERROR_STATE_MISMATCH;
}

size_t anygm_get_last_error(const AnygmEngine *engine,char *message,size_t capacity){
  if(!engine || engine->guard!=ANYGM_ENGINE_GUARD) return 0;
  size_t required=strlen(g_last_error)+1;
  if(message && capacity){
    size_t copy=required<capacity?required:capacity;
    memcpy(message,g_last_error,copy-1);
    message[copy-1]=0;
  }
  return required;
}
