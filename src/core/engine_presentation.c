/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* View selection, aspect policy, transitions, GUI composition, and final
 * software framebuffer geometry. */
#include "engine_internal.h"
#include "anygm_host.h"
#include "content_router.h"


/* Start-room values must not leak across content loads because the same numeric value can identify
 * unrelated rooms. The first boot follows the normal entry point; selecting a debug room and
 * choosing Restart still boots that room. */
int ensure_classic_phase(AnygmEngine *engine,size_t pixels){
  if(pixels==0 || pixels>SIZE_MAX/sizeof(uint32_t)) return 0;
  if(engine->classic_phase_cap>=pixels && engine->classic_phase_mem) return 1;
  uint32_t *next=realloc(engine->classic_phase_mem,pixels*sizeof(uint32_t));
  if(!next) return 0;
  engine->classic_phase_mem=next;
  engine->classic_phase_cap=pixels;
  return 1;
}
int ensure_primary_buffers(AnygmEngine *engine){
  const size_t pixels=(size_t)FB_MAX_W*(size_t)FB_MAX_H;
  if(!engine->fb){
    engine->fb=calloc(pixels,sizeof(*engine->fb));
    if(!engine->fb) return 0;
  }
  if(!engine->screen){
    engine->screen=calloc(pixels,sizeof(*engine->screen));
    if(!engine->screen){ free(engine->fb); engine->fb=NULL; return 0; }
  }
  return 1;
}
int ensure_scratch_buffer(AnygmEngine *engine,uint32_t **buffer){
  (void)engine;
  if(*buffer) return 1;
  *buffer=calloc((size_t)FB_MAX_W*(size_t)FB_MAX_H,sizeof(**buffer));
  return *buffer!=NULL;
}


/* Preserve the completed frame after a shutdown request instead of advancing a runtime which has
 * already fired its final event. These flags belong to the loaded runtime, not to the process. */

/* Optional presentation-pass fingerprint for diagnosing data-driven compositors. It samples the
 * whole software target only when explicitly enabled, so normal emulation pays no extra cost. */
void log_present_pass(AnygmEngine *engine,const char *pass, const uint32_t *px, int w, int h){
  if(!anygm_host_development_setting(&engine->host,"GML_LOG_PRESENT_PASSES") || !px || w<=0 || h<=0) return;
  const char *at=anygm_host_development_setting(&engine->host,"GML_LOG_PRESENT_FRAME");
  if(at && *at){ if(engine->vm.frame!=atol(at)) return; }
  else if(engine->vm.frame>8) return;
  size_t n=(size_t)w*(size_t)h, black=0, white=0, other=0;
  uint64_t hash=1469598103934665603ULL;
  for(size_t i=0;i<n;i++){
    uint32_t c=px[i]&0x00FFFFFFu;
    if(!c) black++; else if(c==0x00FFFFFFu) white++; else other++;
    hash^=(uint64_t)c; hash*=1099511628211ULL;
  }
  GmlRenderDiagnosticMetrics renderer;
  gml_render_diagnostic_metrics(&engine->render,&renderer);
  engine_logf(engine,ANYGM_LOG_DEBUG,"[presentpass] f%ld %-12s %dx%d black=%" PRIu64 " white=%" PRIu64 " other=%" PRIu64 " "
                 "center=%06x hash=%016llx shader=%d pending(fill=%d underlay=%d)\n",
          engine->vm.frame,pass,w,h,(uint64_t)black,(uint64_t)white,(uint64_t)other,
          px[(size_t)(h/2)*w+w/2]&0x00FFFFFFu,(unsigned long long)hash,
          renderer.active_shader,renderer.pending_fill,renderer.pending_underlay);
}

void classic_transition_reset(AnygmEngine *engine){
  engine->classic_transition.active=0;
  engine->classic_transition.phase=0;
  engine->classic_transition.phases=0;
  engine->classic_transition.width=0;
  engine->classic_transition.height=0;
}
void classic_transition_release(AnygmEngine *engine){
  free(engine->classic_transition.old_frame);
  memset(&engine->classic_transition,0,sizeof(engine->classic_transition));
}
int classic_transition_start(AnygmEngine *engine,unsigned width,unsigned height,int steps){
  if(!engine->have_presented_frame || !width || !height || width>FB_MAX_W || height>FB_MAX_H) return 0;
  size_t pixels=(size_t)width*height;
  if(pixels>SIZE_MAX/sizeof(uint32_t)) return 0;
  if(engine->classic_transition.capacity<pixels){
    uint32_t *next=realloc(engine->classic_transition.old_frame,pixels*sizeof(uint32_t));
    if(!next) return 0;
    engine->classic_transition.old_frame=next;
    engine->classic_transition.capacity=pixels;
  }
  memcpy(engine->classic_transition.old_frame,engine->screen,pixels*sizeof(uint32_t));
  /* Classic transition_steps are consumed in a tight presentation loop rather than as
   * room steps. Keep the brief duration at the room rate, but always use an odd picture count:
   * kind 21 reaches a fully black midpoint before revealing the new room. An even count only
   * darkens each side and incorrectly skips the defining black picture. */
  if(steps<1) steps=1;
  int phases=(steps+19)/20;
  if(phases<1) phases=1;
  if(phases>119) phases=119;
  if(!(phases&1)) phases++;
  if(anygm_host_development_setting(&engine->host,"GML_LOG_TRANSITION"))
    engine_logf(engine,ANYGM_LOG_DEBUG,"[transition] kind=21 steps=%d pictures=%d size=%ux%u\n",
            steps,phases,width,height);
  engine->classic_transition.width=width;
  engine->classic_transition.height=height;
  engine->classic_transition.phase=0;
  engine->classic_transition.phases=phases;
  engine->classic_transition.active=1;
  return 1;
}
static unsigned classic_transition_scale(unsigned channel,unsigned numerator,unsigned denominator){
  return (channel*numerator+denominator/2)/denominator;
}
void classic_transition_apply(AnygmEngine *engine,uint32_t *frame,unsigned width,unsigned height){
  ClassicTransition *t=&engine->classic_transition;
  if(!t->active) return;
  if(width!=t->width || height!=t->height || !t->old_frame){
    classic_transition_reset(engine);
    gml_set_global_scalar(&engine->vm,"transition_kind",0);
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
    gml_set_global_scalar(&engine->vm,"transition_kind",0);
  }
}



/* GameMaker's cursor_sprite is an authored software cursor, distinct from the host's window
 * cursor. A desktop host can make a missing implementation look correct in windowed mode by
 * overlaying its OS pointer, then expose the omission when fullscreen hides that pointer. Draw the
 * authored sprite last, in presented-frame coordinates, so it remains visible above transitions,
 * GUI and letterbox margins. Its logical size follows the application viewport scale. */
void draw_game_cursor(AnygmEngine *engine,unsigned width,unsigned height){
  int sprite=(int)lround(gml_global_num(&engine->vm,"cursor_sprite"));
  if(!gml_sprite_exists(&engine->render,sprite) || engine->mouse_pixel_x<0 || engine->mouse_pixel_y<0 || !width || !height)
    return;
  double scale_x=1.0,scale_y=1.0;
  if(engine->present_mouse_valid && engine->present_mouse_source_width>0 && engine->present_mouse_source_height>0 &&
     engine->present_mouse_width>0 && engine->present_mouse_height>0){
    scale_x=(double)engine->present_mouse_width/engine->present_mouse_source_width;
    scale_y=(double)engine->present_mouse_height/engine->present_mouse_source_height;
  }
  gml_render_begin(&engine->render,engine->screen,(int)width,(int)height,0.0,0.0);
  GmlRenderDrawState draw_state={
    .alpha_blend=1,
    .alpha_test_enable=0,
    .color_write_mask=0x0F
  };
  gml_render_draw_state_update(&engine->render,&draw_state,
    GML_RENDER_DRAW_STATE_ALPHA_BLEND |
    GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE |
    GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK);
  gml_draw_sprite_ext(&engine->render,sprite,0,engine->mouse_pixel_x,engine->mouse_pixel_y,
                      scale_x,scale_y,0.0,0xFFFFFF,1.0);
  if(anygm_host_development_setting(&engine->host,"GML_LOG_CURSOR")){ if(engine->diagnostics.cursor_frame!=engine->vm.frame && (engine->vm.frame<4 || engine->vm.frame%120==0)){
      engine->diagnostics.cursor_frame=engine->vm.frame;
      engine_logf(engine,ANYGM_LOG_DEBUG,"[cursor] f%ld sprite=%d frame=%.1f,%.1f scale=%.4f,%.4f\n",
              engine->vm.frame,sprite,engine->mouse_pixel_x,engine->mouse_pixel_y,scale_x,scale_y);
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


int core_opt_resolution(AnygmEngine *engine,int height) {
  uint32_t value=height?engine->config.present_height:engine->config.present_width;
  uint32_t maximum=height?FB_MAX_H:FB_MAX_W;
  return (int)(value>maximum?maximum:value);
}
int core_opt_embedded_shaders(AnygmEngine *engine) {
  return engine->config.embedded_shaders?1:0;
}
/* CRT aperture-mask toggle. The mask is per-raster-pixel chroma and assumes integer scaling.
 * Turning it off substitutes the neutral average so
 * scaled presentations keep scanlines without two-tone banding. GML_CRT_MASK=0 for headless. */
int core_opt_crt_mask(AnygmEngine *engine) {
  return engine->config.crt_mask?1:0;
}
/* Per-component CRT toggles. Curvature and vignette also accept Auto to follow shader uniforms. */
int core_opt_onoff(AnygmEngine *engine,const char *key, const char *env, int def) {
  (void)env;
  if(!strcmp(key,"anygm_crt_scanlines")) return engine->config.crt_scanlines?1:0;
  if(!strcmp(key,"anygm_crt_gamma")) return engine->config.crt_gamma?1:0;
  return def;
}
int core_opt_crt_tristate(AnygmEngine *engine,const char *key, const char *env) { /* -1 auto / 0 off / 1 on */
  (void)env;
  if(!strcmp(key,"anygm_crt_curvature")) return engine->config.crt_curvature;
  if(!strcmp(key,"anygm_crt_vignette")) return engine->config.crt_vignette;
  return -1;
}
static int core_opt_aspect_force(AnygmEngine *engine) {
  return engine->config.aspect_mode<=GMC_ASPECT_FORCE_16_10?(int)engine->config.aspect_mode:GMC_ASPECT_FORCE_NONE;
}
void clamp_camera_to_current_room(AnygmEngine *engine,double *x, double *y, int center_when_smaller) {
  if (!x || !y) return;
  GmlRoom rm;
  if (gml_vm_room_get(&engine->vm, engine->vm.room_index, &rm) != 0) return;
  if (rm.width > 0) {
    double maxx = (double)rm.width - (double)engine->width;
    if (maxx <= 0.0) {
      *x = center_when_smaller ? maxx * 0.5 : 0.0;
    } else {
      if (*x < 0.0) *x = 0.0;
      if (*x > maxx) *x = maxx;
    }
  }
  if (rm.height > 0) {
    double maxy = (double)rm.height - (double)engine->height;
    if (maxy <= 0.0) {
      *y = center_when_smaller ? maxy * 0.5 : 0.0;
    } else {
      if (*y < 0.0) *y = 0.0;
      if (*y > maxy) *y = maxy;
    }
  }
}
void aspect_forced_camera(AnygmEngine *engine,double raw_x, double raw_y, double *out_x, double *out_y) {
  double x = raw_x, y = raw_y;
  if (engine->aspect_force_active) {
    int centered_x = 0, centered_y = 0;
    if (aspect_center_view_target_gen(engine)) {
      int target_obj = (int)gml_global_arr(&engine->vm, "view_object", 0);
      GmlInstance *target = target_obj >= 0 ? gml_find_instance(&engine->vm, target_obj) : NULL;
      if (target) {
        if (engine->width != engine->base_width) { x = target->x - (double)engine->width * 0.5; centered_x = 1; }
        if (engine->height != engine->base_height) { y = target->y - (double)engine->height * 0.5; centered_y = 1; }
        clamp_camera_to_current_room(engine,&x, &y, 1);
      }
    }
    /* Clamp the supplied top-left coordinate in the original view space,
     * then add half of the extra forced-aspect extent on each side. Clamping after adding the
     * negative margin pins a left-edge camera back to zero and shifts the whole scene right. */
    GmlRoom rm;
    if (gml_vm_room_get(&engine->vm, engine->vm.room_index, &rm) == 0) {
      if (!centered_x && rm.width > 0 && engine->base_width > 0) {
        double maxx = (double)rm.width - (double)engine->base_width;
        if (maxx <= 0.0) x = maxx * 0.5;
        else { if (x < 0.0) x = 0.0; if (x > maxx) x = maxx; }
      }
      if (!centered_y && rm.height > 0 && engine->base_height > 0) {
        double maxy = (double)rm.height - (double)engine->base_height;
        if (maxy <= 0.0) y = maxy * 0.5;
        else { if (y < 0.0) y = 0.0; if (y > maxy) y = maxy; }
      }
    }
    if (!centered_x) x += engine->aspect_cam_dx;
    if (!centered_y) y += engine->aspect_cam_dy;
    /* A data-declared wide gameplay view must never expose coordinates beyond a real room edge.
     * Keeping the render camera and gameplay rectangle on the same clamped origin also avoids
     * changing coordinate systems partway through the leading-edge scroll interval. */
    if (aspect_wide_gameplay_view_gen(engine))
      clamp_camera_to_current_room(engine,&x, &y, 1);
  }
  if (out_x) *out_x = x;
  if (out_y) *out_y = y;
}
int core_opt_fast_alpha_cull(AnygmEngine *engine) {
  return engine->config.fast_alpha_cull>32u?32:(int)engine->config.fast_alpha_cull;
}



static double explicit_game_speed_fps(AnygmEngine *engine) {
  GmlVal *p = gml_varmap_get(&engine->vm.globals, "__game_speed_fps");
  double fps = 0.0;
  if(p){
    if(p->t == V_REAL) fps = p->d;
    else if(p->t == V_STR && p->s) fps = atof(p->s);
  }
  return fps > 0.0 ? fps : 0.0;
}
static double cur_room_fps(AnygmEngine *engine) {
  /* Keep the reported frame rate stable. Studio projects can change room_speed mid-play for
   * effects, so modern content uses its GEN8 global cadence instead of repeatedly reconfiguring
   * the host. game_set_speed() remains authoritative when code explicitly changes that cadence.
   *
   * Revision-16 Studio 1 packages can carry the later render-target OPTN bit even though their
   * GEN8 layout has no global cadence. Presentation is second-generation for those packages, but
   * scheduling still uses the authored room speed. Early revision-15 Studio 2 and revision-17
   * packages retain the modern 60 Hz default when GEN8 supplies no speed. */
  double fps = explicit_game_speed_fps(engine);
  if(fps > 0.0) return fps;
  if(anygm_policy_has_modern_layer_semantics(&engine->win)){
    if(engine->win.game_speed > 0.0) return engine->win.game_speed;
    if(!anygm_policy_uses_room_speed_cadence(&engine->win)) return 60.0;
    fps = gml_room_speed(&engine->vm);
    if(fps > 0.0) return fps;
  }
  GmlRoom r;
  if (gml_vm_room_get(&engine->vm, engine->vm.room_index, &r) == 0 && r.speed > 0) return (double)r.speed;
  if(engine->win.game_speed > 0.0) return engine->win.game_speed;
  return 60.0;
}
/* Resolve a view through its opaque GMS camera handle.  Camera resources are maintained by the VM
 * builtins in reserved, serialized global arrays; rooms and classic games that only use view_*
 * variables fall back to those values unchanged. */
int present_view_get(AnygmEngine *engine,int index, GmlPresentView *out) {
  if (!out || index < 0 || index >= 8) return 0;
  memset(out, 0, sizeof(*out));
  out->index = index;
  out->visible = gml_global_arr(&engine->vm, "view_visible", index) >= 0.5;
  out->camera = (int)lround(gml_global_arr(&engine->vm, "view_camera", index));
  int live = out->camera >= 0 && out->camera < GML_CAMERA_LIMIT &&
             gml_global_arr(&engine->vm, "__gml_camera_live", out->camera) >= 0.5;
  if (live) {
    out->x = gml_global_arr(&engine->vm, "__gml_camera_x", out->camera);
    out->y = gml_global_arr(&engine->vm, "__gml_camera_y", out->camera);
    out->w = gml_global_arr(&engine->vm, "__gml_camera_w", out->camera);
    out->h = gml_global_arr(&engine->vm, "__gml_camera_h", out->camera);
  } else {
    out->camera = index;
    out->x = gml_global_arr(&engine->vm, "view_xview", index);
    out->y = gml_global_arr(&engine->vm, "view_yview", index);
    out->w = gml_global_arr(&engine->vm, "view_wview", index);
    out->h = gml_global_arr(&engine->vm, "view_hview", index);
  }
  out->px = (int)lround(gml_global_arr(&engine->vm, "view_xport", index));
  out->py = (int)lround(gml_global_arr(&engine->vm, "view_yport", index));
  out->pw = (int)lround(gml_global_arr(&engine->vm, "view_wport", index));
  out->ph = (int)lround(gml_global_arr(&engine->vm, "view_hport", index));
  if (out->pw <= 0 && out->w > 0) out->pw = (int)lround(out->w);
  if (out->ph <= 0 && out->h > 0) out->ph = (int)lround(out->h);
  return out->visible && out->w > 0 && out->h > 0 && out->pw > 0 && out->ph > 0;
}

int present_view_count(AnygmEngine *engine,GmlPresentView views[8], int *canvas_w, int *canvas_h) {
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
int stale_full_view_port(AnygmEngine *engine,int view_count, int px, int py, int pw, int ph,
                                int logical_w, int logical_h, int app_w, int app_h) {
  if (view_count != 1 || px != 0 || py != 0 || pw <= 0 || ph <= 0 ||
      logical_w <= 0 || logical_h <= 0 || app_w <= 0 || app_h <= 0)
    return 0;
  if (abs(logical_w - app_w) > 1 || abs(logical_h - app_h) > 1)
    return 0;
  if (engine->vm.gui_w <= 0 || engine->vm.gui_h <= 0 ||
      abs(engine->vm.gui_w - app_w) > 1 || abs(engine->vm.gui_h - app_h) > 1)
    return 0;
  return pw >= app_w && ph >= app_h && (pw > app_w || ph > app_h);
}

/* A view port is declared in presentation coordinates. When an explicit GUI canvas covers that
 * complete port but application_surface uses a same-aspect render raster, scale the port with the
 * application surface. Keeping it unscaled would draw the view into only one corner before the
 * complete application surface is presented to the window. */
int application_surface_scales_full_view_port(
  AnygmEngine *engine,int view_count,int px,int py,int pw,int ph,
  int app_w,int app_h) {
  if(!engine || !anygm_policy_has_modern_layer_semantics(&engine->win) ||
     view_count!=1 || px!=0 || py!=0 || pw<=0 || ph<=0 || app_w<=0 || app_h<=0 ||
     engine->vm.gui_w<=0 || engine->vm.gui_h<=0 ||
     abs(pw-engine->vm.gui_w)>1 || abs(ph-engine->vm.gui_h)>1 ||
     (pw==app_w && ph==app_h))
    return 0;
  return fabs((double)app_w/(double)app_h-
              (double)engine->vm.gui_w/(double)engine->vm.gui_h)<0.0005;
}

/* First-generation presentation initializes application_surface at the exported display raster,
 * while a sole view can retain a smaller logical camera and scale into that complete surface. */
int application_surface_matches_first_generation_view_port(
  AnygmEngine *engine,int view_count,int px,int py,int pw,int ph,
  int app_w,int app_h) {
  if(!engine || !anygm_policy_uses_first_generation_studio(&engine->win) ||
     view_count!=1 || px!=0 || py!=0 || pw<=0 || ph<=0 ||
     app_w<=0 || app_h<=0)
    return 0;
  if(pw==app_w && ph==app_h) return 1;
  if(engine->win.disp_w && abs(app_w-(int)engine->win.disp_w)>1) return 0;
  if(engine->win.disp_h && abs(app_h-(int)engine->win.disp_h)>1) return 0;
  /* A sole same-aspect viewport can occupy a smaller area inside an owned application target.
   * An independently declared GUI extent between viewport and target sizes distinguishes that
   * scale from a deliberate inset port; all three extents must remain in a rounded aspect band. */
  if(engine->vm.gui_w<=0 || engine->vm.gui_h<=0 ||
     engine->vm.gui_w<pw || engine->vm.gui_h<ph ||
     engine->vm.gui_w>app_w || engine->vm.gui_h>app_h)
    return 0;
  double app_ratio=(double)app_w/(double)app_h;
  double port_ratio=(double)pw/(double)ph;
  double gui_ratio=(double)engine->vm.gui_w/(double)engine->vm.gui_h;
  double gui_tolerance=1.0/(double)(engine->vm.gui_h<engine->vm.gui_w
                                    ? engine->vm.gui_h:engine->vm.gui_w);
  return fabs(port_ratio-app_ratio)<0.0005 &&
         fabs(gui_ratio-app_ratio)<=gui_tolerance*16.0;
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
    if (gml_vm_room_get(&engine->vm, engine->vm.room_index, &r) == 0 && r.width > 0 && r.height > 0) { w = r.width; h = r.height; }
    else { w = engine->win.disp_w ? engine->win.disp_w : 288; h = engine->win.disp_h ? engine->win.disp_h : 216; }
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
  if (!out_w || !out_h || engine->vm.gui_w <= 0 || engine->vm.gui_h <= 0) return 0;
  double vvis = gml_global_arr(&engine->vm, "view_visible", 0);
  double port_w = 0.0, port_h = 0.0;
  if (vvis >= 0.5) {
    port_w = gml_global_arr(&engine->vm, "view_wport", 0);
    port_h = gml_global_arr(&engine->vm, "view_hport", 0);
  }
  int gw = engine->vm.gui_w > 0 ? engine->vm.gui_w : (port_w > 0 ? (int)port_w : (int)base_w);
  int gh = engine->vm.gui_h > 0 ? engine->vm.gui_h : (port_h > 0 ? (int)port_h : (int)base_h);
  if (gw < 16) gw = (int)base_w;
  if (gh < 16) gh = (int)base_h;
  if (gw <= 0 || gh <= 0) return 0;
  int win_w = engine->vm.window_w > 0 ? engine->vm.window_w : (int)(engine->win.disp_w ? engine->win.disp_w : base_w);
  int win_h = engine->vm.window_h > 0 ? engine->vm.window_h : (int)(engine->win.disp_h ? engine->win.disp_h : base_h);
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
  engine->aspect_force_mode = mode;
  engine->aspect_force_active = 0;
  engine->aspect_cam_dx = engine->aspect_cam_dy = 0.0;
  engine->aspect_gui_ox = engine->aspect_gui_oy = 0;
  if (mode != GMC_ASPECT_FORCE_NONE && base_w > 0 && base_h > 0) {
    const double target = gmc_aspect_force_ratio(mode);
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
        engine->aspect_force_active = 1;
        engine->aspect_cam_dx = ((double)base_w - (double)w) * 0.5;
      }
    }
  }
  if ((w != base_w || h != base_h) && !engine->aspect_force_active) {
    engine->aspect_force_active = 1;
    engine->aspect_cam_dx = ((double)base_w - (double)w) * 0.5;
    engine->aspect_cam_dy = ((double)base_h - (double)h) * 0.5;
  }
  if (w > FB_MAX_W) w = FB_MAX_W;
  if (h > FB_MAX_H) h = FB_MAX_H;
  if (w < 8) w = 8;
  if (h < 8) h = 8;
  if (w == base_w && h == base_h) {
    engine->aspect_force_active = 0;
    engine->aspect_cam_dx = engine->aspect_cam_dy = 0.0;
    engine->aspect_gui_ox = engine->aspect_gui_oy = 0;
  }
  *ow = w; *oh = h;
}
static void cur_room_res(AnygmEngine *engine,unsigned *ow, unsigned *oh) {
  unsigned bw, bh;
  cur_room_base_res(engine,&bw, &bh);
  engine->base_width = bw; engine->base_height = bh;
  apply_aspect_force_to_res(engine,bw, bh, ow, oh);
}
static void cur_classic_room_window_res(AnygmEngine *engine,unsigned *ow, unsigned *oh) {
  GmlRoom room;
  int room_w=(int)(engine->win.disp_w?engine->win.disp_w:288);
  int room_h=(int)(engine->win.disp_h?engine->win.disp_h:216);
  int visible[8], xport[8], yport[8], wport[8], hport[8];
  int window_w, window_h;
  if(gml_vm_room_get(&engine->vm,engine->vm.room_index,&room)==0){
    if(room.width>0) room_w=(int)room.width;
    if(room.height>0) room_h=(int)room.height;
  }
  for(int i=0;i<8;i++){
    visible[i]=gml_global_arr(&engine->vm,"view_visible",i)>=0.5;
    xport[i]=(int)lround(gml_global_arr(&engine->vm,"view_xport",i));
    yport[i]=(int)lround(gml_global_arr(&engine->vm,"view_yport",i));
    wport[i]=(int)lround(gml_global_arr(&engine->vm,"view_wport",i));
    hport[i]=(int)lround(gml_global_arr(&engine->vm,"view_hport",i));
  }
  int fixed_scale=engine->win.classic_scaling;
  /* A classic room_set_view call replaces the authored port at runtime. At 100% scaling that
   * runtime port is the new window extent; the project's startup display dimensions no longer
   * pin it. Static rooms retain the configured fixed-scale behaviour. */
  if(engine->vm.view_ovr){
    int first=engine->vm.room_index*8;
    for(int i=0;i<8 && first+i<engine->vm.n_view_ovr;i++)
      if(first+i>=0 && engine->vm.view_ovr[first+i].full){ fixed_scale=-1; break; }
  }
  gml_classic_room_window_size(room_w,room_h,fixed_scale,
                               (int)engine->win.disp_w,(int)engine->win.disp_h,
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
void aspect_hud_rect(AnygmEngine *engine,int *out_x, int *out_y, int *out_w, int *out_h) {
  int hw = (engine->base_width > 0 && engine->base_width < engine->width) ? (int)engine->base_width : (int)engine->width;
  int hh = (engine->base_height > 0 && engine->base_height < engine->height) ? (int)engine->base_height : (int)engine->height;
  GmlRoom rm;
  if (gml_vm_room_get(&engine->vm, engine->vm.room_index, &rm) == 0) {
    if (rm.width > 0 && rm.width < (uint32_t)hw) hw = (int)rm.width;
    if (rm.height > 0 && rm.height < (uint32_t)hh) hh = (int)rm.height;
  }
  if (hw < 1) hw = (int)engine->width;
  if (hh < 1) hh = (int)engine->height;
  int hx = (int)lround(((double)engine->width - (double)hw) * 0.5);
  int hy = (int)lround(((double)engine->height - (double)hh) * 0.5);
  if (hx < 0) hx = 0;
  if (hy < 0) hy = 0;
  if (hx + hw > (int)engine->width) hw = (int)engine->width - hx;
  if (hy + hh > (int)engine->height) hh = (int)engine->height - hy;
  if (out_x) *out_x = hx;
  if (out_y) *out_y = hy;
  if (out_w) *out_w = hw;
  if (out_h) *out_h = hh;
}
int screen_stage_uses_requested_raster(
  const GmlWin *content,const GmlRenderPresentationMetrics *presentation,
  int logical_width,int logical_height,int target_width,int target_height){
  if(!content || !presentation || logical_width<=0 || logical_height<=0 ||
     target_width<=0 || target_height<=0 ||
     anygm_policy_uses_classic_runtime(content) ||
     anygm_policy_has_modern_layer_semantics(content) ||
     presentation->application_draw_enabled ||
     presentation->requested_width!=target_width ||
     presentation->requested_height!=target_height ||
     target_width%logical_width || target_height%logical_height)
    return 0;
  int scale_x=target_width/logical_width;
  int scale_y=target_height/logical_height;
  return scale_x>1 && scale_x==scale_y;
}
void compute_present(AnygmEngine *engine) {
  GmlRenderPresentationMetrics renderer;
  gml_render_presentation_metrics(&engine->render,&renderer);
  int effective_width=0,effective_height=0;
  /* GUI drawing space: explicit (display_set_gui_size) or the enabled view port, falling back to
   * the view. A disabled view can still carry a default port in its data, which must not affect
   * presentation geometry. */
  double vvis = gml_global_arr(&engine->vm, "view_visible", 0);
  double pw = 0, ph = 0;
  if (vvis >= 0.5) { pw = gml_global_arr(&engine->vm, "view_wport", 0); ph = gml_global_arr(&engine->vm, "view_hport", 0); }
  int gw = engine->vm.gui_w > 0 ? engine->vm.gui_w : (pw > 0 ? (int)pw : (int)engine->width);
  int gh = engine->vm.gui_h > 0 ? engine->vm.gui_h : (ph > 0 ? (int)ph : (int)engine->height);
  if (engine->vm.gui_w <= 0 && engine->vm.gui_h <= 0 &&
      present_view_count(engine,NULL,NULL,NULL) > 1) {
    gw = (int)engine->width;
    gh = (int)engine->height;
  }
  if (gw < 16) gw = engine->width;
  if (gh < 16) gh = engine->height;
  if (gw > FB_MAX_W) gw = FB_MAX_W;
  if (gh > FB_MAX_H) gh = FB_MAX_H;
  engine->gui_space_width = gw; engine->gui_space_height = gh;
  /* Canvas mode applies only when content declares a GUI canvas smaller than its window, making
   * the surrounding margins intentional drawing area. A large window without a declared GUI is
   * display scaling, so the host receives the native view. */
  int gui_window_mode = 0;
  engine->canvas_mode = 0; engine->output_width = engine->width; engine->output_height = engine->height; engine->gui_offset_x = engine->gui_offset_y = 0;
  if (!engine->vm.gui_maximise_active && engine->vm.gui_w > 0 && engine->vm.gui_h > 0) {
    int win_w = engine->vm.window_w > 0 ? engine->vm.window_w : (int)(engine->win.disp_w ? engine->win.disp_w : engine->width);
    int win_h = engine->vm.window_h > 0 ? engine->vm.window_h : (int)(engine->win.disp_h ? engine->win.disp_h : engine->height);
    double s = (double)win_w / gw, s2 = (double)win_h / gh;
    if (s2 < s) s = s2;
    if (s < 1e-6) s = 1.0;
    long cw = lround(win_w / s), ch = lround(win_h / s);
    if (cw > FB_MAX_W) cw = FB_MAX_W;
    if (ch > FB_MAX_H) ch = FB_MAX_H;
    if (gui_canvas_has_margin(cw, ch, gw, gh)) {   /* real margin -> present the window canvas */
      engine->canvas_mode = 1; engine->output_width = (unsigned)cw; engine->output_height = (unsigned)ch;
      engine->gui_offset_x = (int)((cw - gw) / 2); engine->gui_offset_y = (int)((ch - gh) / 2);
      if (engine->gui_offset_x < 0) engine->gui_offset_x = 0;
      if (engine->gui_offset_y < 0) engine->gui_offset_y = 0;
    } else if (gui_window_near_native(win_w, win_h, gw, gh) &&
               win_w <= FB_MAX_W && win_h <= FB_MAX_H) {
      /* A near-native GUI size is a coordinate system stretched to the window, not a request
       * for a small uniform-fit canvas. Preserve the declared window pixels. */
      gui_window_mode = 1;
      engine->output_width = (unsigned)win_w; engine->output_height = (unsigned)win_h;
    }
  }
  /* When a port is larger than its view, present at GUI/port resolution rather than downscaling
   * port-space GUI and text to the smaller view. The world view is scaled into the port rectangle,
   * matching the declared window. GML_PRESENT_VIEW=1 forces view-resolution presentation for
   * diagnostics. This affects presentation only, not simulation or serialized state. */
  { if (engine->diagnostics.force_present_view < 0) engine->diagnostics.force_present_view = anygm_host_development_setting(&engine->host,"GML_PRESENT_VIEW") ? 1 : 0;
    if (!engine->vm.gui_maximise_active && !engine->diagnostics.force_present_view && !engine->canvas_mode && !gui_window_mode
        && engine->gui_space_width >= (int)engine->output_width && engine->gui_space_height >= (int)engine->output_height
        && (engine->gui_space_width > (int)engine->output_width || engine->gui_space_height > (int)engine->output_height)
        && engine->gui_space_width <= FB_MAX_W && engine->gui_space_height <= FB_MAX_H) {
      engine->output_width = (unsigned)engine->gui_space_width; engine->output_height = (unsigned)engine->gui_space_height; engine->gui_offset_x = engine->gui_offset_y = 0;
    } }
  /* A non-uniform declared or runtime window size can coexist with one view port that remains
   * the screen-stage raster. Exposing only the port would discard the final presentation
   * transform, so retain the port as GUI space and let the indirect presentation path scale it. */
  int screen_stage_window_w = engine->vm.window_w > 0
                            ? engine->vm.window_w : (int)engine->win.disp_w;
  int screen_stage_window_h = engine->vm.window_h > 0
                            ? engine->vm.window_h : (int)engine->win.disp_h;
  /* The window extent is a request from content, not a host measurement: no host reports its
   * presentation window to the engine. When the window is only larger than the view, matching it
   * here costs a full software upscale carrying no detail the host would not produce itself while
   * scaling to the display. Content that owns its window raster keeps the dedicated path. */
  if (!engine->config.present_logical_raster &&
      !anygm_policy_uses_classic_runtime(&engine->win) &&
      !engine->vm.gui_maximise_active && !engine->canvas_mode && !gui_window_mode &&
      engine->vm.gui_w <= 0 && engine->vm.gui_h <= 0 &&
      present_view_count(engine,NULL,NULL,NULL) == 1 &&
      screen_stage_window_w > 0 && screen_stage_window_h > 0 &&
      screen_stage_window_w <= FB_MAX_W && screen_stage_window_h <= FB_MAX_H &&
      engine->gui_space_width == (int)engine->output_width &&
      engine->gui_space_height == (int)engine->output_height &&
      (screen_stage_window_w != (int)engine->output_width ||
       screen_stage_window_h != (int)engine->output_height)) {
    engine->output_width = (unsigned)screen_stage_window_w;
    engine->output_height = (unsigned)screen_stage_window_h;
  }
  if (engine->aspect_force_active) {
    int logical_gw = engine->vm.gui_w > 0 ? engine->vm.gui_w : (int)engine->base_width;
    int logical_gh = engine->vm.gui_h > 0 ? engine->vm.gui_h : (int)engine->base_height;
    if (logical_gw < 16) logical_gw = (int)engine->base_width;
    if (logical_gh < 16) logical_gh = (int)engine->base_height;
    engine->canvas_mode = 0;
    engine->output_width = engine->width;
    engine->output_height = engine->height;
    engine->gui_space_width = (int)engine->width;
    engine->gui_space_height = (int)engine->height;
    engine->gui_offset_x = (int)lround(((double)engine->width - (double)logical_gw) * 0.5) + engine->aspect_gui_ox;
    engine->gui_offset_y = (int)lround(((double)engine->height - (double)logical_gh) * 0.5) + engine->aspect_gui_oy;
  }
  /* Classic presentation keeps the window/port size stable when game code changes view_wview or
   * view_hview: those variables zoom the camera, they do not resize the host window. The world
   * is still rendered at the logical view extent above and is presented into this fixed window. */
  if (anygm_policy_uses_classic_runtime(&engine->win) && !engine->aspect_force_active &&
      renderer.requested_width <= 0 && renderer.requested_height <= 0) {
    unsigned room_window_w, room_window_h;
    cur_classic_room_window_res(engine,&room_window_w, &room_window_h);
    int window_w = engine->vm.window_w > 0 ? engine->vm.window_w : (int)room_window_w;
    int window_h = engine->vm.window_h > 0 ? engine->vm.window_h : (int)room_window_h;
    if (window_w > 0 && window_h > 0 && window_w <= FB_MAX_W && window_h <= FB_MAX_H) {
      engine->canvas_mode = 0;
      engine->output_width = (unsigned)window_w;
      engine->output_height = (unsigned)window_h;
      engine->gui_space_width = window_w;
      engine->gui_space_height = window_h;
      engine->gui_offset_x = engine->gui_offset_y = 0;
    }
  }
  /* A Studio game can explicitly own the final window raster in either of two ways: resize the
   * application surface and keep automatic presentation, or disable automatic presentation and
   * compose the application surface plus decoration itself in Post-Draw. In the latter case the
   * explicit window is the drawing coordinate system even when the application surface remains at
   * the smaller view size. Preserve that authored raster; otherwise the Post-Draw margins are
   * clipped to the central view. The signals are runtime semantics, independent of game identity. */
  if (anygm_policy_has_modern_layer_semantics(&engine->win) && !engine->aspect_force_active && !engine->canvas_mode &&
      !renderer.application_draw_enabled &&
      engine->vm.gui_w <= 0 && engine->vm.gui_h <= 0 &&
      engine->vm.window_w > 0 && engine->vm.window_h > 0 &&
      engine->vm.window_w <= FB_MAX_W && engine->vm.window_h <= FB_MAX_H) {
    engine->output_width = (unsigned)engine->vm.window_w;
    engine->output_height = (unsigned)engine->vm.window_h;
    engine->gui_space_width = engine->vm.window_w;
    engine->gui_space_height = engine->vm.window_h;
    engine->gui_offset_x = engine->gui_offset_y = 0;
    effective_width = engine->vm.window_w;
    effective_height = engine->vm.window_h;
  }
  /* A Studio game can explicitly resize application_surface to the window and keep the normal
   * automatic presentation path.  That surface is the final presentation raster, not an oversized
   * internal effect buffer: presenting the smaller room view instead box-reduces the authored
   * window pixels (pixel-art edges become 1/3 and 2/3 grey at a 3x window scale).  Preserve the
   * explicit raster when all three independent signals agree: owned surface, matching window,
   * and automatic app-surface drawing.  Self-compositing games disable app_draw_enable and keep
   * their existing logical/GUI path; explicit GUI canvases and aspect overrides are likewise
   * left untouched. */
  if (anygm_policy_has_modern_layer_semantics(&engine->win) && !engine->aspect_force_active && !engine->canvas_mode &&
      renderer.application_owned && renderer.application_draw_enabled &&
      engine->vm.gui_w <= 0 && engine->vm.gui_h <= 0 &&
      renderer.application_width > 0 && renderer.application_height > 0 &&
      renderer.application_width <= FB_MAX_W && renderer.application_height <= FB_MAX_H &&
      engine->vm.window_w == renderer.application_width &&
      engine->vm.window_h == renderer.application_height) {
    engine->output_width = (unsigned)renderer.application_width;
    engine->output_height = (unsigned)renderer.application_height;
    engine->gui_space_width = renderer.application_width;
    engine->gui_space_height = renderer.application_height;
    engine->gui_offset_x = engine->gui_offset_y = 0;
    effective_width = renderer.application_width;
    effective_height = renderer.application_height;
  }
  /* An explicit resolution is the final framebuffer and GUI/compositor space. Aspect forcing still
   * controls the logical room/view width independently. Fast-forward must not change geometry. */
  if (renderer.requested_width > 0 || renderer.requested_height > 0) {
    int base_w = (renderer.wide_aspect_active && renderer.wide_width > 0)
               ? renderer.wide_width : (int)(engine->win.disp_w ? engine->win.disp_w : engine->width);
    int base_h = (renderer.wide_aspect_active && renderer.wide_height > 0)
               ? renderer.wide_height : (int)(engine->win.disp_h ? engine->win.disp_h : engine->height);
    int target_h = renderer.requested_height > 0 ? renderer.requested_height : base_h;
    int target_w = renderer.requested_width > 0 ? renderer.requested_width : base_w;
    /* Aspect Ratio Force composes with explicit resolution instead of being overwritten by it.
     * The vertical axis is authoritative and the effective width is derived from it; the raw
     * width option deliberately remains untouched so the host still shows what was selected. */
    if (engine->aspect_force_mode != GMC_ASPECT_FORCE_NONE && target_h > 0) {
      double ratio = gmc_aspect_force_ratio(engine->aspect_force_mode);
      target_w = (int)round_to_multiple_of_8((double)target_h * ratio);
      if (target_w > FB_MAX_W) target_w = FB_MAX_W & ~7;
    }
    if (target_w > 0 && target_h > 0 && target_w <= FB_MAX_W && target_h <= FB_MAX_H) {
      engine->canvas_mode = 0;
      engine->gui_space_width = target_w; engine->gui_space_height = target_h;
      engine->output_width = (unsigned)target_w; engine->output_height = (unsigned)target_h;
      engine->gui_offset_x = engine->gui_offset_y = 0;
      effective_width = target_w;
      effective_height = target_h;
    }
  }
  /* A maximised GUI is screen-relative and its explicit scale is already the logical-to-screen
   * transform.  The GUI canvas must therefore neither select a larger host resolution nor be
   * rendered off-screen and reduced a second time. */
  if (engine->vm.gui_maximise_active) {
    engine->canvas_mode=0;
    engine->gui_space_width=(int)engine->output_width;
    engine->gui_space_height=(int)engine->output_height;
    engine->gui_offset_x=engine->gui_offset_y=0;
  }
  gml_render_presentation_effective_set(&engine->render,effective_width,effective_height);
}
void aspect_view_overlay_begin(AnygmEngine *engine,AspectViewOverlay *ov, int center_hud, int view_mode) {
  memset(ov, 0, sizeof(*ov));
  if (!engine->aspect_force_active) return;
  ov->active = 1;
  ov->room = engine->vm.room_index;
  ov->xview = gml_global_arr(&engine->vm, "view_xview", 0);
  ov->yview = gml_global_arr(&engine->vm, "view_yview", 0);
  ov->wview = gml_global_arr(&engine->vm, "view_wview", 0);
  ov->hview = gml_global_arr(&engine->vm, "view_hview", 0);
  ov->wport = gml_global_arr(&engine->vm, "view_wport", 0);
  ov->hport = gml_global_arr(&engine->vm, "view_hport", 0);

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
    if (engine->width > engine->base_width && engine->aspect_cam_dx < 0.0 &&
        ov->xview <= -engine->aspect_cam_dx + 0.001)
      forced_x = 0;
    if (engine->height > engine->base_height && engine->aspect_cam_dy < 0.0 &&
        ov->yview <= -engine->aspect_cam_dy + 0.001)
      forced_y = 0;
  }
  double gx = forced_x ? ov->render_x : ov->xview;
  double gy = forced_y ? ov->render_y : ov->yview;
  double gw = (!forced_x && view_mode == ASPECT_VIEW_TRACKING) ? (double)engine->base_width : (double)engine->width;
  double gh = (!forced_y && view_mode == ASPECT_VIEW_TRACKING) ? (double)engine->base_height : (double)engine->height;
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
    if (forced_x && engine->width != engine->base_width) ov->map_dx = engine->aspect_cam_dx;
    if (forced_y && engine->height != engine->base_height) ov->map_dy = engine->aspect_cam_dy;
  }
  gml_set_global_arr(&engine->vm, "view_xview", 0, gx);
  gml_set_global_arr(&engine->vm, "view_yview", 0, gy);
  if (ov->wview > 0.0) gml_set_global_arr(&engine->vm, "view_wview", 0, gw);
  if (ov->hview > 0.0) gml_set_global_arr(&engine->vm, "view_hview", 0, gh);
  if (ov->wport > 0.0) gml_set_global_arr(&engine->vm, "view_wport", 0, gw);
  if (ov->hport > 0.0) gml_set_global_arr(&engine->vm, "view_hport", 0, gh);
}
void aspect_view_overlay_end(AnygmEngine *engine,AspectViewOverlay *ov, int preserve_camera_writes) {
  if (!ov->active) return;
  if (engine->vm.room_index != ov->room) return;
  double raw_x = ov->xview, raw_y = ov->yview;
  if (preserve_camera_writes) {
    raw_x = gml_global_arr(&engine->vm, "view_xview", 0) - ov->map_dx;
    raw_y = gml_global_arr(&engine->vm, "view_yview", 0) - ov->map_dy;
  }
  gml_set_global_arr(&engine->vm, "view_xview", 0, raw_x);
  gml_set_global_arr(&engine->vm, "view_yview", 0, raw_y);
  gml_set_global_arr(&engine->vm, "view_wview", 0, ov->wview);
  gml_set_global_arr(&engine->vm, "view_hview", 0, ov->hview);
  gml_set_global_arr(&engine->vm, "view_wport", 0, ov->wport);
  gml_set_global_arr(&engine->vm, "view_hport", 0, ov->hport);
}
void aspect_draw_event_hook(GmlVM *vm, GmlInstance *in, const char *suffix,
                            int begin, void *user) {
  AnygmEngine *engine=user;
  GmlVM *ctx = vm ? vm : &engine->vm;
  if (!begin) {
    if (engine->aspect_event_view_overflow > 0) {
      engine->aspect_event_view_overflow--;
      return;
    }
    if (engine->aspect_event_view_stack_pointer <= 0) return;
    AspectEventViewOverlay ov = engine->aspect_event_view_stack[--engine->aspect_event_view_stack_pointer];
    if (!ov.active || ctx->room_index != ov.room) return;
    gml_set_global_arr(ctx, "view_xview", 0, ov.xview);
    gml_set_global_arr(ctx, "view_yview", 0, ov.yview);
    gml_set_global_arr(ctx, "view_wview", 0, ov.wview);
    gml_set_global_arr(ctx, "view_hview", 0, ov.hview);
    gml_set_global_arr(ctx, "view_wport", 0, ov.wport);
    gml_set_global_arr(ctx, "view_hport", 0, ov.hport);
    return;
  }

  if (engine->aspect_event_view_stack_pointer >= ASPECT_EVENT_VIEW_STACK_MAX) {
    engine->aspect_event_view_overflow++;
    return;
  }
  AspectEventViewOverlay ov;
  memset(&ov, 0, sizeof(ov));
  int custom_draw = GMC_ASPECT_DRAW_DEFAULT;
  if (engine->aspect_force_active && engine->aspect_draw_full_context)
    custom_draw = aspect_draw_full_view_gen(engine,in, suffix);
  if (engine->aspect_force_active && engine->aspect_draw_full_context &&
      custom_draw != GMC_ASPECT_DRAW_DEFAULT) {
    ov.active = 1;
    ov.room = ctx->room_index;
    ov.xview = gml_global_arr(ctx, "view_xview", 0);
    ov.yview = gml_global_arr(ctx, "view_yview", 0);
    ov.wview = gml_global_arr(ctx, "view_wview", 0);
    ov.hview = gml_global_arr(ctx, "view_hview", 0);
    ov.wport = gml_global_arr(ctx, "view_wport", 0);
    ov.hport = gml_global_arr(ctx, "view_hport", 0);
    gml_set_global_arr(ctx, "view_xview", 0, engine->aspect_draw_full_x);
    gml_set_global_arr(ctx, "view_yview", 0, engine->aspect_draw_full_y);
    gml_set_global_arr(ctx, "view_wview", 0, (double)engine->width);
    gml_set_global_arr(ctx, "view_hview", 0, (double)engine->height);
    gml_set_global_arr(ctx, "view_wport", 0, (double)engine->width);
    gml_set_global_arr(ctx, "view_hport", 0, (double)engine->height);
    if (custom_draw == GMC_ASPECT_DRAW_FULL_VIEW_BACKDROP)
      gml_render_set_pending_fill(&engine->render, 0);
  }
  engine->aspect_event_view_stack[engine->aspect_event_view_stack_pointer++] = ov;
}
void sync_room_fps(AnygmEngine *engine,int publish_changes) {
  int room = engine->vm.room_index;
  double fps = cur_room_fps(engine);
  unsigned nw, nh; cur_room_res(engine,&nw, &nh);
  unsigned pw = engine->output_width, ph = engine->output_height;
  unsigned prev_w = engine->width, prev_h = engine->height;
  engine->width = nw; engine->height = nh;
  compute_present(engine);
  if (room == engine->fps_room && fps == engine->fps && nw == prev_w && nh == prev_h &&
      engine->output_width == pw && engine->output_height == ph) { engine->fps_room = room; return; }
  engine->fps_room = room;
  int fps_changed = (fps != engine->fps);
  int geom_changed = (nw != prev_w) || (nh != prev_h) || (engine->output_width != pw) || (engine->output_height != ph);
  int changed = fps_changed || geom_changed;
  if(changed && anygm_host_development_setting(&engine->host,"GML_LOG_AV"))
    engine_logf(engine,ANYGM_LOG_DEBUG, "[av] room=%d bytecode=%u fps=%.2f size=%ux%u out=%ux%u window=%dx%d gui=%dx%d\n",
      room, (unsigned)engine->win.bytecode, fps, nw, nh, engine->output_width, engine->output_height,
      engine->vm.window_w,engine->vm.window_h,engine->vm.gui_w,engine->vm.gui_h);
  if (fps_changed) engine->audio_accumulator = 0.0;
  engine->fps = fps;
  if(changed && publish_changes){
    if(fps_changed) engine->frame_flags|=ANYGM_FRAME_TIMING_CHANGED;
    if(geom_changed) engine->frame_flags|=ANYGM_FRAME_GEOMETRY_CHANGED;
  }
}

uint32_t cur_room_bg(AnygmEngine *engine) {
  if (anygm_policy_uses_classic_runtime(&engine->win)) return gm_to_xrgb(engine->win.classic_outside_color);
  return gm_to_xrgb(gml_vm_room_background_argb(&engine->vm));
}
int room_clears_application_surface(const GmlWin *content, const GmlRoom *room) {
  /* Generations that keep the completed frame let the room decide; the rest clear every frame and
   * treat the room fields as the choice of paint over that clear, not as the choice to clear. */
  if (!anygm_policy_preserves_frame_without_background_clear(content)) return 1;
  return room &&
    (room->draw_bg || (room->flags&GML_ROOM_FLAG_CLEAR_VIEW_BACKGROUND));
}
/* The coordinator draws room layers around the instance pass, including on an
 * explicit redraw into the current target. Respect the current room and any
 * content-owned background renderer. */
void screen_redraw_room_layer_hook(GmlVM *vm,int foreground,void *user) {
  AnygmEngine *engine=user;
  if (!engine) return;
  (void)vm;
  GmlRoom room;
  if (gml_vm_room_get(&engine->vm, engine->vm.room_index, &room) != 0) return;
  const char *bg_renderer = anygm_host_development_setting(&engine->host,"GML_BG_RENDERER_OBJ");
  int object = (bg_renderer && *bg_renderer)
    ? gml_object_index_by_name(&engine->vm, bg_renderer) : -1;
  if (object >= 0 && gml_find_instance(&engine->vm, object) != NULL) return;
  draw_runtime_backgrounds(engine, foreground);
}
/* A refresh after content composition latches the current screen for this
 * frame; a bare refresh does not suppress coordinator presentation. */
void screen_refresh_present_latch_hook(GmlVM *vm,void *user) {
  AnygmEngine *engine=user; (void)vm;
  if (!engine || !anygm_policy_uses_classic_runtime(&engine->win)) return;
  if (gml_render_content_composited_screen(&engine->render)) engine->content_presented=1;
}
void draw_runtime_backgrounds(AnygmEngine *engine,int want_fg) {
  for (int i = 0; i < 8; i++) {
    int visible = gml_global_arr(&engine->vm, "background_visible", i) >= 0.5;
    int fg = gml_global_arr(&engine->vm, "background_foreground", i) >= 0.5;
    int bg = (int)gml_global_arr(&engine->vm, "background_index", i);
    if (!visible || fg != want_fg || bg < 0) continue;
    int ht = gml_global_arr(&engine->vm, "background_htiled", i) >= 0.5;
    int vt = gml_global_arr(&engine->vm, "background_vtiled", i) >= 0.5;
    double alpha = gml_global_arr(&engine->vm, "background_alpha", i);
    if (alpha <= 0) alpha = 1.0;
    gml_draw_background_tiled_ext(&engine->render, bg,
                                  gml_global_arr(&engine->vm, "background_x", i),
                                  gml_global_arr(&engine->vm, "background_y", i),
                                  1.0, 1.0, 0xFFFFFF, alpha, ht, vt);
  }
}
static int aspect_visible_room_rect(AnygmEngine *engine,double cam_x, double cam_y,
                                    int *out_x, int *out_y, int *out_w, int *out_h) {
  if (!engine->aspect_force_active || !engine->width || !engine->height) return 0;
  GmlRoom rm;
  if (gml_vm_room_get(&engine->vm, engine->vm.room_index, &rm) != 0) return 0;
  if (rm.width == 0 || rm.height == 0) return 0;
  if (rm.width >= engine->width && rm.height >= engine->height) return 0;

  int x0 = 0, y0 = 0, x1 = (int)engine->width, y1 = (int)engine->height;
  if (rm.width < engine->width) {
    x0 = (int)lround(-cam_x);
    x1 = x0 + (int)rm.width;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)engine->width) x1 = (int)engine->width;
  }
  if (rm.height < engine->height) {
    y0 = (int)lround(-cam_y);
    y1 = y0 + (int)rm.height;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)engine->height) y1 = (int)engine->height;
  }
  if (out_x) *out_x = x0;
  if (out_y) *out_y = y0;
  if (out_w) *out_w = x1 - x0;
  if (out_h) *out_h = y1 - y0;
  return 1;
}
void aspect_mask_outside_room(AnygmEngine *engine,double cam_x, double cam_y) {
  int x0, y0, rw, rh;
  if (!aspect_visible_room_rect(engine,cam_x, cam_y, &x0, &y0, &rw, &rh)) return;
  int x1 = x0 + rw, y1 = y0 + rh;

  const uint32_t black = 0xFF000000u;
  int have_room_rect = rw > 0 && rh > 0;
  for (unsigned yy = 0; yy < engine->height; yy++) {
    uint32_t *row = engine->fb + (size_t)yy * engine->width;
    if (!have_room_rect || (int)yy < y0 || (int)yy >= y1) {
      for (unsigned xx = 0; xx < engine->width; xx++) row[xx] = black;
      continue;
    }
    for (int xx = 0; xx < x0; xx++) row[xx] = black;
    for (unsigned xx = (unsigned)x1; xx < engine->width; xx++) row[xx] = black;
  }
  GmlRenderTargetCoverage coverage={.all_transparent=0};
  gml_render_target_coverage_update(&engine->render,&coverage,
                                    GML_RENDER_COVERAGE_ALL_TRANSPARENT);
}

void setup_display(AnygmEngine *engine) {
  /* fixed framebuffer = native display size; rooms are viewed through it */
  engine->width = engine->win.disp_w ? engine->win.disp_w : 288;
  engine->height = engine->win.disp_h ? engine->win.disp_h : 216;
  if (engine->width > FB_MAX_W) engine->width = FB_MAX_W;
  if (engine->height > FB_MAX_H) engine->height = FB_MAX_H;
}

void compose_view_rect(const uint32_t *src, int sw, int sh,
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
    if (sy < 0) sy = 0;
    if (sy >= sh) sy = sh - 1;
    uint32_t *drow = dst + (size_t)y * dw;
    const uint32_t *srow = src + (size_t)sy * sw;
    for (int x = x0; x < x1; x++) {
      int sx = (int)(((int64_t)(2 * (x - dx) + 1) * sw) / ((int64_t)2 * rw));
      if (sx < 0) sx = 0;
      if (sx >= sw) sx = sw - 1;
      drow[x] = srow[sx];
    }
  }
}

static void seed_view_rect(const uint32_t *src,int sw,int sh,
                           uint32_t *dst,int dw,int dh,
                           int sx,int sy,int rw,int rh){
  if(!src || !dst || sw<=0 || sh<=0 || dw<=0 || dh<=0 || rw<=0 || rh<=0) return;
  int x0=sx<0?0:sx, y0=sy<0?0:sy;
  int x1=sx+rw, y1=sy+rh;
  if(x1>sw) x1=sw;
  if(y1>sh) y1=sh;
  if(x0>=x1 || y0>=y1) return;
  for(int y=y0;y<y1;y++){
    int dy=(int)(((int64_t)(2*(y-sy)+1)*dh)/((int64_t)2*rh));
    if(dy<0) dy=0;
    if(dy>=dh) dy=dh-1;
    for(int x=x0;x<x1;x++){
      int dx=(int)(((int64_t)(2*(x-sx)+1)*dw)/((int64_t)2*rw));
      if(dx<0) dx=0;
      if(dx>=dw) dx=dw-1;
      dst[(size_t)dy*dw+dx]=src[(size_t)y*sw+x];
    }
  }
}

/* Render every visible GMS view once, in view-index order, then composite each camera into its
 * declared port on the application surface. Pre-Draw and Post-Draw are screen-stage events owned
 * by the frame coordinator and remain outside this per-view sequence. */
int render_multiview_application(AnygmEngine *engine) {
  GmlPresentView views[8]; int canvas_w = 0, canvas_h = 0;
  int count = present_view_count(engine,views, &canvas_w, &canvas_h);
  if (count <= 1 || canvas_w != (int)engine->width || canvas_h != (int)engine->height) return 0;
  if(!ensure_scratch_buffer(engine,&engine->app_crop)) return 0;
  uint32_t *view_buffer=engine->app_crop;

  GmlRoom rm;
  int have_room = gml_vm_room_get(&engine->vm, engine->vm.room_index, &rm) == 0;
  int clear_background = !have_room || room_clears_application_surface(&engine->win, &rm);
  if(clear_background)
    for(unsigned i=0;i<engine->width*engine->height;i++) engine->fb[i]=0xFF000000u;
  const char *bg_renderer = anygm_host_development_setting(&engine->host,"GML_BG_RENDERER_OBJ");
  int pobj = (bg_renderer && *bg_renderer) ? gml_object_index_by_name(&engine->vm, bg_renderer) : -1;
  int gml_draws_bg = pobj >= 0 && gml_find_instance(&engine->vm, pobj) != NULL;

  for (int i = 0; i < count; i++) {
    GmlPresentView *v = &views[i];
    int vw = (int)lround(v->w), vh = (int)lround(v->h);
    if (vw <= 0 || vh <= 0 || vw > FB_MAX_W || vh > FB_MAX_H) continue;
    *gml_varmap_put(&engine->vm.globals, "view_current") = vreal(v->index);
    GmlRenderSamplePlanes planes={0};
    gml_render_sample_planes_update(&engine->render,&planes,
                                    GML_RENDER_SAMPLE_PLANES_APPLICATION);
    if(!clear_background)
      seed_view_rect(engine->fb,(int)engine->width,(int)engine->height,
                     view_buffer,vw,vh,v->px,v->py,v->pw,v->ph);
    gml_render_begin(&engine->render, view_buffer, vw, vh, v->x, v->y);
    if(clear_background)
      gml_render_set_pending_fill(&engine->render, engine->background);
    if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,0);
    gml_vm_draw_pass(&engine->vm, "Draw_72");
    gml_vm_draw(&engine->vm);
    gml_vm_draw_pass(&engine->vm, "Draw_73");
    if (have_room && !gml_draws_bg) draw_runtime_backgrounds(engine,1);
    gml_render_flush_pending_fill(&engine->render);
    compose_view_rect(view_buffer, vw, vh, engine->fb, (int)engine->width, (int)engine->height,
                      v->px, v->py, v->pw, v->ph);
  }
  *gml_varmap_put(&engine->vm.globals, "view_current") = vreal(0);
  GmlRenderTargetCoverage coverage={
    .opaque_known=1,
    .all_opaque=1,
    .all_transparent=0
  };
  gml_render_target_coverage_update(&engine->render,&coverage,GML_RENDER_COVERAGE_ALL);
  if (anygm_host_development_setting(&engine->host,"GML_LOG_VIEW")) {
    if (engine->diagnostics.multiview_frame != engine->vm.frame && (engine->vm.frame < 4 || engine->vm.frame % 120 == 0)) {
      engine->diagnostics.multiview_frame = engine->vm.frame;
      engine_logf(engine,ANYGM_LOG_DEBUG, "[multiview] f%ld views=%d canvas=%dx%d", engine->vm.frame, count, canvas_w, canvas_h);
      for (int i = 0; i < count; i++) engine_logf(engine,ANYGM_LOG_DEBUG, " v%d cam=%d world=(%.0f,%.0f %.0fx%.0f) port=(%d,%d %dx%d)",
        views[i].index, views[i].camera, views[i].x, views[i].y, views[i].w, views[i].h,
        views[i].px, views[i].py, views[i].pw, views[i].ph);
      engine_logf(engine,ANYGM_LOG_DEBUG,"\n");
    }
  }
  return 1;
}

void content_router_log(void *userdata,int level,const char *message){
  AnygmEngine *engine=userdata;
  AnygmLogLevel mapped=level==ANYGM_CONTENT_LOG_ERROR?ANYGM_LOG_ERROR:
                       level==ANYGM_CONTENT_LOG_WARN?ANYGM_LOG_WARN:ANYGM_LOG_INFO;
  engine_logf(engine,mapped,"[anygm] %s\n",message?message:"");
}
