/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer lifecycle, frame coordination, draw state, and software draw facade. */
#include "gml_render.h"
#include "gml_render_plan.h"
#include "gml_render_cpu_presentation.h"
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_blit_internal.h"
#include "gml_render_cpu_dispatch_internal.h"
#include "gml_render_cpu_pixel_internal.h"
#include "gml_render_cpu_sampling_internal.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_cpu_shader_pass.h"
#include "gml_render_cpu_workers.h"
#include "gml_render_content.h"
#include "gml_render_shaders.h"
#include "anygm_compatibility.h"
#include "gml_font_raster.h"
#include "anygm_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
int rprof_enabled(void){ return 0; }
const char *render_setting(const GmlRender *r,const char *name){
  return anygm_host_development_setting(r&&r->win?r->win->host:NULL,name);
}

/* Cardinal rotations must stay exactly on the pixel lattice.  libm leaves tiny residuals for
 * sin/cos(90*n) (for example cos(270) ~= -1.8e-16); inverse texture mapping then floors a sample
 * on the wrong side of an integer boundary and shifts the quadrants with a negative axis by one
 * pixel.  Hardware vertex transforms preserve these literal cardinal matrices. */
void render_rotation_sincos(double degrees,double *cosine,double *sine){
  double quadrant=nearbyint(degrees/90.0);
  if(fabs(degrees-quadrant*90.0)<1e-10){
    switch(((int)quadrant%4+4)%4){
      case 0: *cosine=1.0;  *sine=0.0;  return;
      case 1: *cosine=0.0;  *sine=1.0;  return;
      case 2: *cosine=-1.0; *sine=0.0;  return;
      default:*cosine=0.0;  *sine=-1.0; return;
    }
  }
  double radians=degrees*M_PI/180.0;
  *cosine=cos(radians);
  *sine=sin(radians);
}
/* Modern format semantics rasterize a cardinally rotated textured rectangle as a half-open cell. When a
 * source basis points toward a negative destination axis, its outer integer edge belongs to the
 * preceding pixel.  Applying that one-pixel anchor correction also makes the four 90-degree
 * quadrants meet without doubled/separated seams.  Arbitrary-angle coverage keeps the ordinary
 * pixel-centre inverse map below. */
void render_modern_cardinal_anchor(const GmlRender *r,double degrees,
                                  double xs,double ys,double cosine,double sine,
                                  double *x,double *y){
  double quadrant=nearbyint(degrees/90.0);
  if(!r || r->classic || fabs(degrees-quadrant*90.0)>=1e-10) return;
  if(xs*cosine < -1e-12 || ys*sine < -1e-12) *x-=1.0;
  if(-xs*sine < -1e-12 || ys*cosine < -1e-12) *y-=1.0;
}
double rprof_now(void){
  return 0.0;
}
int rprof_tpag_id(GmlRender *r, GmlTpag *t){
  if(!r || !t || !r->tpag || r->n_tpag<=0) return -1;
  uintptr_t pointer=(uintptr_t)t, begin=(uintptr_t)r->tpag;
  uintptr_t end=begin+(uintptr_t)r->n_tpag*sizeof(GmlTpag);
  return pointer>=begin && pointer<end ? (int)((pointer-begin)/sizeof(GmlTpag)) : -1;
}
void rprof_add(const char *label, GmlRender *r, GmlTpag *t, double ms, unsigned long long pixels){
  (void)label; (void)r; (void)t; (void)ms; (void)pixels;
}

uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
uint16_t u16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }
uint32_t be32(const uint8_t *d){ return (uint32_t)d[0]<<24|(uint32_t)d[1]<<16|(uint32_t)d[2]<<8|(uint32_t)d[3]; }
uint32_t gml_render_named_tpag_ptr(GmlRender *r, const char *name){
  if(!r || !name || !*name || !r->tpag_ptr) return 0;
  for(int i=0;i<r->n_spr;i++){
    GmlSprite *s=&r->spr[i];
    if(!s->name || strcmp(s->name,name) || !s->frame || s->n_frames<=0) continue;
    int ti=s->frame[0];
    return ti>=0 && ti<r->n_tpag ? r->tpag_ptr[ti] : 0;
  }
  return 0;
}

/* Exact sprite-name lookup uses a lazy open-addressed index. Rebuild on sprite-name changes
 * and retain first-match order when multiple sprites share one name. */
static uint32_t render_strhash(const char *s){
  uint32_t h=2166136261u; while(*s){ h^=(uint8_t)*s++; h*=16777619u; } return h;
}
static void render_named_sprite_reindex(GmlRender *r){
  uint32_t cap=1; while(cap < (uint32_t)(r->n_spr<0?0:r->n_spr)*2u+1u) cap<<=1;
  int32_t *hix=malloc((size_t)cap*sizeof(int32_t));
  if(!hix) return;
  for(uint32_t i=0;i<cap;i++) hix[i]=-1;
  for(int i=0;i<r->n_spr;i++){
    if(!r->spr[i].name) continue;
    uint32_t h=render_strhash(r->spr[i].name)&(cap-1);
    while(hix[h]>=0) h=(h+1)&(cap-1);   /* first insertion wins on a name shared by duplicates */
    hix[h]=i;
  }
  free(r->spr_name_hix);
  r->spr_name_hix=hix; r->spr_name_hix_cap=cap; r->spr_name_hix_built_gen=r->spr_name_gen;
}
int gml_render_named_sprite(GmlRender *r, const char *name){
  if(!r || !name || !*name) return -1;
  if(!r->spr_name_hix || r->spr_name_hix_built_gen!=r->spr_name_gen) render_named_sprite_reindex(r);
  if(!r->spr_name_hix){
    for(int i=0;i<r->n_spr;i++)
      if(r->spr[i].name && !strcmp(r->spr[i].name,name)) return i;
    return -1;
  }
  uint32_t h=render_strhash(name)&(r->spr_name_hix_cap-1);
  for(uint32_t probe=0; probe<r->spr_name_hix_cap; probe++){
    int32_t i=r->spr_name_hix[h];
    if(i<0) return -1;
    if(i<r->n_spr && r->spr[i].name && !strcmp(r->spr[i].name,name)) return i;
    h=(h+1)&(r->spr_name_hix_cap-1);
  }
  return -1;
}

double gml_sprite_animation_delta(GmlRender *r, int sprite, double image_speed, double game_fps){
  if(!r || sprite<0 || sprite>=r->n_spr) return image_speed;
  GmlSprite *s=&r->spr[sprite];
  if(!s->playback_speed_valid) return image_speed;
  double delta=image_speed*(double)s->playback_speed;
  if(s->playback_speed_type==0){
    if(!(game_fps>0.0) || !isfinite(game_fps)) game_fps=60.0;
    delta/=game_fps;
  }
  return delta;
}
/* whether sprite's COLLISION MASK is solid at sprite-local (lx,ly). Asset sprites without a
 * serialized 1bpp mask use their bounding box; runtime sprites can still use alpha precision
 * through sprite_collision_mask(kind=bboxkind_precise). */
int gml_sprite_collision(GmlRender *r, int sprite, int frame, int lx, int ly){
  if(sprite<0||sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(lx<0||ly<0||lx>=s->w||ly>=s->h) return 0;
  if(lx<s->ml||lx>s->mr||ly<s->mt||ly>s->mb) return 0;
  if(s->collision_kind==1) return 1;
  if(s->collision_kind==2){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    double dx=(lx-cx)/hw, dy=(ly-cy)/hh;
    return dx*dx+dy*dy<=1.0;
  }
  if(s->collision_kind==3){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    return fabs((lx-cx)/hw)+fabs((ly-cy)/hh)<=1.0;
  }
  if(s->mask && s->mask_count>0){
    /* Select the displayed subimage's mask, wrapping out-of-range indices. */
    int mi=0;
    if(s->mask_count>1){ mi=frame%s->mask_count; if(mi<0) mi+=s->mask_count; }
    const uint8_t *m=s->mask + (size_t)mi*s->mask_rowb*s->h;
    return (m[(size_t)ly*s->mask_rowb + lx/8] >> (7-(lx%8))) & 1;
  }
  if(s->runtime_rgba) return gml_sprite_alpha(r,sprite,frame,lx,ly)>s->collision_tolerance;
  return 1;
}


static int env_fast_alpha_cull(const GmlRender *r){
  const char *e=render_setting(r,"GML_FAST_ALPHA_CULL");
  int v=e?atoi(e):0;
  if(v<0) v=0;
  if(v>32) v=32;
  return v;
}
int gml_render_init(GmlRender *r, GmlWin *win){
  memset(r,0,sizeof(*r)); r->win=win;
  r->surface_draw_logging=-1;
  r->classic=win&&anygm_policy_uses_classic_runtime(win);
  /* GM6/7/8 starts the shared drawing colour at black; Studio starts it at white. The state is
   * intentionally persistent across Draw events, so choosing the right initial value matters to
   * every project which draws text or primitives without an explicit draw_set_colour call. */
  r->color=(win && anygm_policy_uses_classic_runtime(win))?0:0xFFFFFF;
  r->alpha=1; r->font=-1; r->alphablend=1; r->circle_precision=24; r->color_write_mask=0x0F;
  r->alpha_test_enable=0; r->alpha_test_ref=0;
  r->blend_equation=r->blend_equation_alpha=1;
  r->app_draw_enable=1; r->next_surface_id=1;
  r->shader_report_all_compiled=1;
  for(int i=0;i<GML_SHADED_FRAME_CACHE;i++) r->shaded_frame[i].atlas=-1;
  gml_render_texture_filter_all(r,anygm_policy_classic_interpolate(win));
  r->composites_app=0;
  r->fast_alpha_cull=env_fast_alpha_cull(r);
  r->lut_pal_sprite=-1; r->lut_pal_frame=0;
  parse_txtr(r); gml_render_content_init(r); parse_font(r); build_default_font(r);
  gml_render_materialize_scaled_pages(r);
  parse_shader_palettes(r);
  r->base_n_spr=r->n_spr;
  return 0;
}
void gml_render_bind_software3d(GmlRender *r,GmlSoftware3D *software3d){
  if(r) r->software3d=software3d;
}
void gml_render_rebind_content(GmlRender *r,GmlWin *win){
  if(r){
    r->win=win;
    gml_render_resource_bind_dispatch(r->app_surface_resource,r);
    gml_render_resource_bind_dispatch(r->base_scene_resource,r);
    gml_render_resource_bind_dispatch(r->retained_base_resource,r);
    gml_render_resource_bind_dispatch(r->base_history_resource,r);
    gml_render_resource_bind_dispatch(r->retained_base_history_resource,r);
    for(int index=0;index<GML_MAX_SURFACES;index++)
      gml_render_resource_bind_dispatch(r->surface[index].resource,r);
  }
}
GmlSoftware3D *gml_render_software3d(GmlRender *r){
  return gml_render_cpu_backend_software3d(r);
}
void gml_render_set_frame(GmlRender *r,long frame){
  if(r) r->frame=frame;
  /* Emit the renderer's settled state once per frame when explicitly enabled.
   * The record precedes drawing and includes presentation metrics. */
  if(r && render_setting(r,"GML_LOG_RENDER_STATE")){
    GmlRenderDrawState draw={0};
    GmlRenderPresentationMetrics metrics={0};
    gml_render_draw_state_get(r,&draw);
    gml_render_presentation_metrics(r,&metrics);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[renderstate] f%ld color=%06x alpha=%.6f font=%d halign=%d valign=%d blend=%d/%d/%d "
      "alphatest=%d/%d mask=%u interp=%d circle=%d | app=%dx%d owned=%d draw=%d gui=%d/%dx%d "
      "eff=%dx%d wide=%d/%dx%d phase=%d surfaces=%d atlas_bytes=%zu\n",
      frame,(unsigned)draw.color,draw.alpha,draw.font,draw.horizontal_alignment,
      draw.vertical_alignment,draw.alpha_blend,draw.blend_mode,draw.blend_equation,
      draw.alpha_test_enable,draw.alpha_test_reference,draw.color_write_mask,
      draw.interpolation,draw.circle_precision,
      metrics.application_width,metrics.application_height,metrics.application_owned,
      metrics.application_draw_enabled,metrics.gui_pass_active,metrics.gui_base_width,
      metrics.gui_base_height,metrics.effective_width,metrics.effective_height,
      metrics.wide_aspect_active,metrics.wide_width,metrics.wide_height,
      metrics.application_phase_active,r->next_surface_id,r->atlas_decoded_bytes);
  }
}
void gml_render_control_update(GmlRender *r,const GmlRenderControl *control,
                               unsigned fields){
  if(!r || !control) return;
  if(fields&GML_RENDER_CONTROL_MONITOR_SIZE){
    r->monitor_w=control->monitor_width;
    r->monitor_h=control->monitor_height;
  }
  if(fields&GML_RENDER_CONTROL_SHADERS){
    r->shader_report_all_compiled=control->shader_report_all_compiled;
    r->shader_device_expected=control->shader_device_expected;
  }
  if(fields&GML_RENDER_CONTROL_FAST_ALPHA)
    r->fast_alpha_cull=control->fast_alpha_cull;
  if(fields&GML_RENDER_CONTROL_WIDE_ASPECT){
    r->aspect_fullwidth=control->wide_aspect_active;
    r->aspect_wide_w=control->wide_width;
    r->aspect_wide_h=control->wide_height;
  }
  if(fields&GML_RENDER_CONTROL_SURFACE_CANVAS){
    r->surface_canvas=control->surface_canvas;
    r->surface_canvas_width=control->surface_canvas_width;
    r->surface_canvas_height=control->surface_canvas_height;
    r->surface_canvas_world_width=control->surface_canvas_world_width;
    r->surface_canvas_room_x=control->surface_canvas_room_x;
    r->surface_canvas_room_width=control->surface_canvas_room_width;
  }
}
int gml_render_resource_metrics(const GmlRender *r,
                                GmlRenderResourceMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->atlas_count=r->n_atlas;
    metrics->sprite_count=r->n_spr;
    metrics->texture_page_count=r->n_tpag;
    metrics->completed_screen_cpu_reads=r->completed_screen_cpu_reads;
    metrics->completed_screen_cpu_materializations=r->completed_screen_cpu_materializations;
  }
  return 1;
}
int gml_render_diagnostic_metrics(const GmlRender *r,
                                  GmlRenderDiagnosticMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->active_shader=r->active_shader;
    metrics->pending_fill=r->pending_fill;
    metrics->pending_underlay=r->pending_underlay;
  }
  return 1;
}
/* The world rectangle the target currently shows.
 *
 * Target metrics are in target pixels, and a view that does not match the target introduces a
 * scale between those and the coordinates content is authored in. Anything that selects world-space
 * content by what is visible — the tile grid above all — needs the rectangle in authored units, and
 * only the renderer knows the transform relating the two. Reading the target metrics directly and
 * dividing by a cell size in authored units mixes the two spaces and selects the wrong region. */
int gml_render_world_view(const GmlRender *r,double *x,double *y,
                          double *width,double *height){
  if(!r) return 0;
  double scale_x=r->world_transform_active && r->world_scale_x>0.0 ? r->world_scale_x : 1.0;
  double scale_y=r->world_transform_active && r->world_scale_y>0.0 ? r->world_scale_y : 1.0;
  if(x) *x=r->cam_x/scale_x;
  if(y) *y=r->cam_y/scale_y;
  if(width) *width=(double)r->fbw/scale_x;
  if(height) *height=(double)r->fbh/scale_y;
  return 1;
}
int gml_render_target_metrics(const GmlRender *r,GmlRenderTargetMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->width=r->fbw; metrics->height=r->fbh;
    metrics->camera_x=r->cam_x; metrics->camera_y=r->cam_y;
  }
  return 1;
}
void gml_render_target_metrics_update(GmlRender *r,
                                      const GmlRenderTargetMetrics *metrics,
                                      unsigned fields){
  if(!r || !metrics) return;
  if(fields&GML_RENDER_TARGET_WIDTH) r->fbw=metrics->width;
  if(fields&GML_RENDER_TARGET_HEIGHT) r->fbh=metrics->height;
  if(fields&GML_RENDER_TARGET_CAMERA){
    r->cam_x=metrics->camera_x;
    r->cam_y=metrics->camera_y;
  }
}
int gml_render_presentation_metrics(const GmlRender *r,
                                    GmlRenderPresentationMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->monitor_width=r->monitor_w;
    metrics->monitor_height=r->monitor_h;
    metrics->effective_width=r->presentation_w;
    metrics->effective_height=r->presentation_h;
    metrics->wide_aspect_active=r->aspect_fullwidth;
    metrics->wide_width=r->aspect_wide_w;
    metrics->wide_height=r->aspect_wide_h;
    metrics->gui_pass_active=r->gui_pass_active;
    metrics->gui_base_width=r->gui_base_logical_w;
    metrics->gui_base_height=r->gui_base_logical_h;
    metrics->application_width=r->app_w;
    metrics->application_height=r->app_h;
    metrics->application_owned=r->app_surface_owned!=NULL;
    metrics->application_draw_enabled=r->app_draw_enable;
    metrics->interpolation=r->interp;
    metrics->application_phase_active=r->app_phase_y!=NULL;
  }
  return 1;
}
void gml_render_presentation_effective_set(GmlRender *r,int width,int height){
  if(!r) return;
  r->presentation_w=width;
  r->presentation_h=height;
}
void gml_render_sample_planes_update(GmlRender *r,
                                     const GmlRenderSamplePlanes *planes,
                                     unsigned fields){
  if(!r || !planes) return;
  if(fields&GML_RENDER_SAMPLE_PLANES_APPLICATION){
    r->app_phase_y=planes->application_vertical;
    for(int i=0;i<3;i++)
      r->app_interp_phase[i]=planes->application_interpolated[i];
  }
  if(fields&GML_RENDER_SAMPLE_PLANES_CLASSIC){
    r->classic_phase_y=planes->classic_vertical;
    for(int i=0;i<3;i++)
      r->classic_interp_phase[i]=planes->classic_interpolated[i];
  }
}
int gml_render_target_coverage(const GmlRender *r,
                               GmlRenderTargetCoverage *coverage){
  if(coverage) memset(coverage,0,sizeof(*coverage));
  if(!r) return 0;
  if(coverage){
    coverage->opaque_known=r->fb_opaque_known;
    coverage->all_opaque=r->fb_all_opaque;
    coverage->all_transparent=r->fb_all_transparent;
  }
  return 1;
}
void gml_render_target_coverage_update(GmlRender *r,
                                       const GmlRenderTargetCoverage *coverage,
                                       unsigned fields){
  if(!r || !coverage) return;
  if(fields&GML_RENDER_COVERAGE_OPAQUE_KNOWN)
    r->fb_opaque_known=coverage->opaque_known;
  if(fields&GML_RENDER_COVERAGE_ALL_OPAQUE)
    r->fb_all_opaque=coverage->all_opaque;
  if(fields&GML_RENDER_COVERAGE_ALL_TRANSPARENT)
    r->fb_all_transparent=coverage->all_transparent;
}
void gml_render_application_surface_bind(GmlRender *r,uint32_t *pixels,
                                         int width,int height,int opaque){
  if(!r) return;
  int changed=r->app_surface!=pixels || r->app_w!=width || r->app_h!=height;
  if(changed && !gml_render_application_scene_flush(r)) return;
  r->app_surface=pixels;
  r->app_w=width;
  r->app_h=height;
  r->app_surface_opaque=opaque!=0;
  if(changed) r->app_presentation_coverage_active=0;
}
int gml_render_application_surface_owned_extent(const GmlRender *r,int *width,int *height){
  if(width) *width=0;
  if(height) *height=0;
  if(!r || !r->app_surface_owned || r->app_w<=0 || r->app_h<=0 ||
     (r->app_surface_resource &&
      (r->app_surface_resource_width!=r->app_w ||
       r->app_surface_resource_height!=r->app_h))) return 0;
  if(width) *width=r->app_w;
  if(height) *height=r->app_h;
  return 1;
}
int gml_render_begin_owned_application(GmlRender *r,double camx,double camy,
                                       int retain_target){
  if(!gml_render_application_surface_owned_extent(r,NULL,NULL)) return 0;
  if(!gml_render_application_scene_flush(r)) return 0;
  if(r->app_surface_resource){
    uint32_t *pixels=NULL;
    if(!gml_render_resource_acquire_cpu_write(r->app_surface_resource,&pixels) ||
       pixels!=r->app_surface_owned ||
       !gml_render_resource_commit_cpu_write(r->app_surface_resource)) return 0;
  }
  if(retain_target)
    gml_render_begin_retaining_target(r,r->app_surface_owned,r->app_w,r->app_h,camx,camy);
  else
    gml_render_begin(r,r->app_surface_owned,r->app_w,r->app_h,camx,camy);
  return 1;
}
int gml_render_begin_resident_owned_application(GmlRender *r,double camx,double camy,
                                                int retain_target){
  if(!r) return 0;
  if(!gml_render_device(r))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_NO_DEVICE);
  if(!gml_render_application_surface_owned_extent(r,NULL,NULL))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_OWNED_EXTENT);
  if(r->rotated_batch_count || r->rotated_batch_building)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_ROTATED_BATCH);
  if(r->pending_fill)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_PENDING_FILL);
  if(r->pending_underlay)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_PENDING_UNDERLAY);
  if(r->pending_presentation)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_PENDING_PRESENTATION);
  for(int index=0;index<r->target_sp;index++)
    if(r->target_stack[index].pending_fill ||
       r->target_stack[index].pending_underlay)
      return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_STACK_PENDING);
  if(retain_target && r->target_id>0 && r->target_sp>0 &&
     (r->target_id>GML_MAX_SURFACES ||
      !r->surface[r->target_id-1].resource))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_TARGET_RESOURCE);
  if(!gml_render_application_resource_ensure(r))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_RESOURCE);
  if(retain_target)
    gml_render_begin_retaining_resident_target(
      r,r->app_surface_owned,r->app_w,r->app_h,camx,camy);
  else
    gml_render_begin(r,r->app_surface_owned,r->app_w,r->app_h,camx,camy);
  r->resident_scene_pass=1;
  r->resident_application_pass=gml_render_scene_target_matches(r);
  if(r->resident_application_pass) gml_render_dispatch_resident_admitted(r);
  else gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_TARGET_RESOURCE);
  return 1;
}
static int begin_resident_base(GmlRender *r,uint32_t *pixels,int width,int height,
                               double camx,double camy,int retain_target,int numbered_only){
  if(!r) return 0;
  if(!pixels || width<=0 || height<=0)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_INVALID_TARGET);
  if(!gml_render_device(r))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_NO_DEVICE);
  if(numbered_only && (r->target_id<=0 || r->target_sp<=0))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_TARGET_RESOURCE);
  if(r->rotated_batch_count || r->rotated_batch_building)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_ROTATED_BATCH);
  if(!retain_target && r->pending_fill)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_PENDING_FILL);
  if(!retain_target && r->target_sp)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_TARGET_STACK);
  if(r->pending_underlay)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_PENDING_UNDERLAY);
  if(r->pending_presentation)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_PENDING_PRESENTATION);
  if(!gml_render_application_scene_flush(r))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_SCENE_FLUSH);
  if(!numbered_only && gml_render_base_resource_select(r,pixels)<0)
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_TARGET_POLICY);
  if(retain_target && r->target_sp>0 &&
     (r->target_id<=0 || r->target_id>GML_MAX_SURFACES ||
      !gml_render_surface_resource_ensure(r,r->target_id)))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_TARGET_RESOURCE);
  if(!numbered_only && r->base_scene_resource &&
     (r->base_scene_resource_pixels!=pixels ||
      r->base_scene_resource_width!=width ||
      r->base_scene_resource_height!=height)){
    /* Same allocation means raw-linear reinterpretation, including the inactive tail. */
    if(r->base_scene_resource_pixels!=pixels ||
       !gml_render_base_resource_resize(r,width,height)){
      if(r->base_scene_resource_pixels==pixels &&
         (!gml_render_base_history_observe(r) ||
          !gml_render_resource_acquire_cpu_read(r->base_scene_resource,NULL)))
        return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_CPU_OBSERVE);
      gml_render_base_resource_dispose(r);
    }
  }
  if(retain_target)
    gml_render_begin_retaining_resident_target(r,pixels,width,height,camx,camy);
  else
    gml_render_begin(r,pixels,width,height,camx,camy);
  if(numbered_only){
    /* Draw metadata is not a new SCREEN owner. Keep the independent composition
     * alive; an authored reset to this CPU base must not borrow its device image. */
    r->target_stack[0].resident_pass=0;
  } else {
    r->base_scene_resource_pixels=pixels;
    r->base_scene_resource_width=width;
    r->base_scene_resource_height=height;
  }
  if(!gml_render_scene_target_resource_ensure(r))
    return gml_render_dispatch_resident_reject(r,GML_RENDER_RESIDENT_RESOURCE);
  r->resident_application_pass=1;
  r->resident_scene_pass=1;
  gml_render_dispatch_resident_admitted(r);
  return 1;
}
int gml_render_begin_resident_base(GmlRender *r,uint32_t *pixels,int width,int height,
                                   double camx,double camy){
  return begin_resident_base(r,pixels,width,height,camx,camy,0,0);
}
int gml_render_begin_resident_base_retaining_target(GmlRender *r,uint32_t *pixels,
                                                   int width,int height,double camx,double camy){
  return begin_resident_base(r,pixels,width,height,camx,camy,1,0);
}
int gml_render_begin_resident_numbered_target(GmlRender *r,uint32_t *cpu_base,
                                             int width,int height,double camx,double camy){
  return begin_resident_base(r,cpu_base,width,height,camx,camy,1,1);
}
int gml_render_application_surface_owned_clear(
  GmlRender *r,uint32_t color,GmlRenderApplicationWriteView *view){
  if(view) memset(view,0,sizeof(*view));
  if(!gml_render_application_surface_owned_extent(r,NULL,NULL)) return 0;
  if(!gml_render_application_scene_flush(r)) return 0;
  if(r->app_surface_resource){
    uint32_t *pixels=NULL;
    if(!gml_render_resource_acquire_cpu_write(r->app_surface_resource,&pixels) ||
       pixels!=r->app_surface_owned ||
       !gml_render_resource_commit_cpu_write(r->app_surface_resource)) return 0;
  }
  size_t count=(size_t)r->app_w*(size_t)r->app_h;
  for(size_t i=0;i<count;i++) r->app_surface_owned[i]=color;
  if(count) gml_render_dispatch_cpu_composition(r,GML_RENDER_SCENE_PRIMITIVE);
  r->app_presentation_coverage_active=0;
  if(view){
    view->pixels=r->app_surface_owned;
    view->width=r->app_w;
    view->height=r->app_h;
  }
  return 1;
}
int gml_render_application_surface_owned_view(
  GmlRender *r,GmlRenderApplicationWriteView *view){
  if(view) memset(view,0,sizeof(*view));
  if(!gml_render_application_surface_owned_extent(r,NULL,NULL)) return 0;
  if(!gml_render_application_scene_flush(r)) return 0;
  if(r->app_surface_resource){
    uint32_t *pixels=NULL;
    if(!gml_render_resource_acquire_cpu_write(r->app_surface_resource,&pixels) ||
       pixels!=r->app_surface_owned ||
       !gml_render_resource_commit_cpu_write(r->app_surface_resource)) return 0;
  }
  if(view){
    view->pixels=r->app_surface_owned;
    view->width=r->app_w;
    view->height=r->app_h;
  }
  return 1;
}
int gml_render_application_surface_owned_read(
  GmlRender *r,GmlRenderApplicationReadView *view){
  if(view) memset(view,0,sizeof(*view));
  if(!gml_render_application_surface_owned_extent(r,NULL,NULL)) return 0;
  gml_render_prepare_draw(r);
  if(!gml_render_application_scene_flush(r)) return 0;
  if(r->app_surface_resource){
    const uint32_t *pixels=NULL;
    if(!gml_render_resource_acquire_cpu_read(r->app_surface_resource,&pixels) ||
       pixels!=r->app_surface_owned) return 0;
  }
  if(view){
    view->pixels=r->app_surface_owned;
    view->width=r->app_w;
    view->height=r->app_h;
  }
  return 1;
}
int gml_render_application_surface_select_owned(GmlRender *r,int opaque){
  if(!r || !r->app_surface_owned) return 0;
  int changed=r->app_surface!=r->app_surface_owned;
  r->app_surface=r->app_surface_owned;
  r->app_surface_opaque=opaque!=0;
  if(changed) r->app_presentation_coverage_active=0;
  return 1;
}
int gml_render_surface_mirror_pixels(GmlRender *r,int destination,
                                     const uint32_t *pixels,int width,int height,
                                     int opaque){
  if(!r || destination<=0 || !pixels || width<=0 || height<=0 ||
     !gml_surface_exists(r,destination)) return 0;
  uint32_t *saved_pixels=r->app_surface;
  int saved_width=r->app_w;
  int saved_height=r->app_h;
  int saved_opaque=r->app_surface_opaque;
  int saved_presentation_coverage_active=r->app_presentation_coverage_active;
  /* The mirror borrows this application source only for the nested draw; the
   * binding API also serves writable targets, but no source write occurs here. */
  gml_render_application_surface_bind(r,(uint32_t*)(uintptr_t)pixels,width,height,opaque);
  int mirrored=0;
  if(gml_surface_set_target(r,destination)){
    gml_draw_surface_stretched(r,0,0,0,
      gml_surface_width(r,destination),gml_surface_height(r,destination),
      0xFFFFFF,1.0);
    gml_surface_reset_target(r);
    mirrored=1;
  }
  gml_render_application_surface_bind(
    r,saved_pixels,saved_width,saved_height,saved_opaque);
  r->app_presentation_coverage_active=saved_presentation_coverage_active;
  return mirrored;
}
/* Fully fogged fragments use the fog colour after texture and diffuse colour,
 * retaining sprite coverage. The fixed-function owner resolves whether the
 * current depth is fully fogged before selecting this raster shortcut. */
void gml_render_set_flat_fog(GmlRender *r,int enabled,uint32_t colour){
  gml_render_cpu_backend_set_flat_fog(r,enabled,colour);
}
int gml_render_draw_state_get(const GmlRender *r,GmlRenderDrawState *state){
  if(!state) return 0;
  memset(state,0,sizeof(*state));
  state->color=0xFFFFFFu;
  state->alpha=1.0;
  state->font=-1;
  state->alpha_blend=1;
  state->circle_precision=24;
  state->blend_equation=1;
  state->blend_equation_alpha=1;
  state->color_write_mask=0x0Fu;
  if(!r) return 0;
  state->color=r->color;
  state->alpha=r->alpha;
  state->font=r->font;
  state->horizontal_alignment=r->halign;
  state->vertical_alignment=r->valign;
  state->alpha_blend=r->alphablend;
  state->circle_precision=r->circle_precision;
  state->interpolation=r->interp;
  state->blend_mode=r->blendmode;
  state->blend_equation=r->blend_equation;
  state->blend_equation_alpha=r->blend_equation_alpha;
  state->alpha_test_enable=r->alpha_test_enable;
  state->alpha_test_reference=r->alpha_test_ref;
  state->color_write_mask=r->color_write_mask;
  return 1;
}
void gml_render_draw_state_update(GmlRender *r,const GmlRenderDrawState *state,
                                  unsigned fields){
  if(!r || !state) return;
  if(fields&GML_RENDER_DRAW_STATE_COLOR) r->color=state->color;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA) r->alpha=state->alpha;
  if(fields&GML_RENDER_DRAW_STATE_FONT) r->font=state->font;
  if(fields&GML_RENDER_DRAW_STATE_HORIZONTAL_ALIGNMENT)
    r->halign=state->horizontal_alignment;
  if(fields&GML_RENDER_DRAW_STATE_VERTICAL_ALIGNMENT)
    r->valign=state->vertical_alignment;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA_BLEND) r->alphablend=state->alpha_blend;
  if(fields&GML_RENDER_DRAW_STATE_CIRCLE_PRECISION)
    r->circle_precision=state->circle_precision;
  if(fields&GML_RENDER_DRAW_STATE_INTERPOLATION) r->interp=state->interpolation;
  if(fields&GML_RENDER_DRAW_STATE_BLEND_MODE) r->blendmode=state->blend_mode;
  if(fields&GML_RENDER_DRAW_STATE_BLEND_EQUATION)
    r->blend_equation=state->blend_equation;
  if(fields&GML_RENDER_DRAW_STATE_BLEND_EQUATION_ALPHA)
    r->blend_equation_alpha=state->blend_equation_alpha;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE)
    r->alpha_test_enable=state->alpha_test_enable;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA_TEST_REFERENCE)
    r->alpha_test_ref=(uint8_t)state->alpha_test_reference;
  if(fields&GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK)
    r->color_write_mask=(uint8_t)state->color_write_mask;
}
int gml_render_gpu_state_push(GmlRender *r){
  if(!r || r->gpu_state_sp>=16) return 0;
  struct GmlGpuState *state=&r->gpu_state_stack[r->gpu_state_sp++];
  state->alphablend=r->alphablend;
  state->alpha_test_enable=r->alpha_test_enable;
  state->alpha_test_ref=r->alpha_test_ref;
  state->blendmode=r->blendmode;
  state->blend_equation=r->blend_equation;
  state->blend_equation_alpha=r->blend_equation_alpha;
  state->interp=r->interp;
  memcpy(state->texture_filter,r->texture_filter,sizeof r->texture_filter);
  state->color_write_mask=r->color_write_mask;
  return 1;
}
int gml_render_gpu_state_pop(GmlRender *r){
  if(!r || r->gpu_state_sp<=0) return 0;
  struct GmlGpuState *state=&r->gpu_state_stack[--r->gpu_state_sp];
  r->alphablend=state->alphablend;
  r->alpha_test_enable=state->alpha_test_enable;
  r->alpha_test_ref=state->alpha_test_ref;
  r->blendmode=state->blendmode;
  r->blend_equation=state->blend_equation;
  r->blend_equation_alpha=state->blend_equation_alpha;
  r->interp=state->interp;
  memcpy(r->texture_filter,state->texture_filter,sizeof r->texture_filter);
  r->color_write_mask=state->color_write_mask;
  return 1;
}
void gml_render_application_surface_set_draw_enabled(GmlRender *r,int enabled){
  if(r) r->app_draw_enable=enabled;
}
void gml_render_application_surface_presentation_settle(GmlRender *r,int settled){
  if(r) r->app_draw_presentation_settled=settled?1:0;
}
int gml_render_application_surface_presentation_settled(const GmlRender *r){
  return r?r->app_draw_presentation_settled:0;
}
int gml_render_target_pixel(GmlRender *r,int x,int y,uint32_t *pixel){
  const uint32_t *current;
  int width,height;
  if(pixel) *pixel=0;
  if(!r || !pixel) return 0;
  gml_render_maybe_prepare_draw(r);
  if(!r->fb || x<0 || y<0 || x>=r->fbw || y>=r->fbh) return 0;
  current=r->fb;
  width=r->fbw;
  height=r->fbh;
  if(r->fb==r->app_surface_owned && r->app_surface_resource)
    current=surface_pixels_observe(r,0,&width,&height);
  else if(r->fb==r->base_scene_resource_pixels && r->base_scene_resource){
    if(!gml_render_resource_acquire_cpu_read(r->base_scene_resource,&current)) return 0;
  }
  else if(r->target_id>0 && r->target_id<=GML_MAX_SURFACES &&
          r->surface[r->target_id-1].resource)
    current=surface_pixels_observe(r,r->target_id,&width,&height);
  if(!current || current!=r->fb || width!=r->fbw || height!=r->fbh) return 0;
  *pixel=current[(size_t)y*width+x];
  return 1;
}
/* Whether this renderer is rasterizing under the classic conventions. Builtins see GmlRender as an
 * incomplete type, and primitive coordinate rules differ between the generations, so the ones that
 * have to choose a rule ask here rather than keeping a second copy of the flag. */
int gml_render_is_classic(const GmlRender *r){
  return r?r->classic:0;
}

static int sprite_frame_plane(GmlRender *r,int sprite,int frame,uint32_t **plane,
                               size_t *capacity,int *width,int *height,const uint32_t **pixels){
  if(!r || !pixels || sprite<0 || sprite>=r->n_spr) return 0;
  {
    GmlSprite *s=&r->spr[sprite];
    int w=s->w,h=s->h;
    if(w<=0 || h<=0 || s->n_frames<=0) return 0;
    if(frame<0 || frame>=s->n_frames) frame=0;
    {
      size_t count=(size_t)w*(size_t)h;
      const uint8_t *rgba=NULL;
      int atlas_w=0,ax0=0,ay0=0,lx0=0,ly0=0,lw=0,lh=0;
      if(count>16777216u) return 0;
      if(count>*capacity){
        uint32_t *grown=realloc(*plane,count*sizeof *grown);
        if(!grown) return 0;
        *plane=grown;
        *capacity=count;
      }
      memset(*plane,0,count*sizeof **plane);
      if(s->runtime_rgba){ rgba=s->runtime_rgba; atlas_w=w; lw=w; lh=h; }
      else if(s->frame){
        int ti=s->frame[frame];
        if(ti>=0 && ti<r->n_tpag){
          GmlTpag *t=&r->tpag[ti];
          if(t->atlas>=0 && t->atlas<r->n_atlas){
            uint8_t *ap=atlas_pixels(r,t->atlas);
            GmlAtlas *a=&r->atlas[t->atlas];
            if(ap && a->w>0 && a->h>0){
              rgba=ap; atlas_w=a->w; ax0=t->sx; ay0=t->sy; lx0=t->tx; ly0=t->ty;
              lw=t->sw; lh=t->sh;
              if(ax0<0 || ay0<0 || ax0+lw>a->w || ay0+lh>a->h) rgba=NULL;
            }
          }
        }
      }
      if(rgba){
        for(int y=0;y<lh;y++){
          int dy=ly0+y; if(dy<0 || dy>=h) continue;
          for(int x=0;x<lw;x++){
            int dx=lx0+x; if(dx<0 || dx>=w) continue;
            const uint8_t *px=rgba+((size_t)(ay0+y)*atlas_w+(ax0+x))*4u;
            (*plane)[(size_t)dy*w+dx]=
              ((uint32_t)px[3]<<24)|((uint32_t)px[0]<<16)|((uint32_t)px[1]<<8)|(uint32_t)px[2];
          }
        }
      }
      if(width) *width=w;
      if(height) *height=h;
      *pixels=*plane;
      return 1;
    }
  }
}
int gml_render_sprite_frame_plane(GmlRender *r,int sprite,int frame,int *width,int *height,
                                  const uint32_t **pixels){
  if(!r) return 0;
  return sprite_frame_plane(r,sprite,frame,&r->content_sampler_plane,
                              &r->content_sampler_plane_capacity,width,height,pixels);
}
int gml_render_sprite_frame_plane_owned(GmlRender *r,int sprite,int frame,int *width,int *height,
                                        uint32_t **pixels){
  uint32_t *plane=NULL;
  size_t capacity=0;
  const uint32_t *view=NULL;
  if(!pixels) return 0;
  *pixels=NULL;
  if(!sprite_frame_plane(r,sprite,frame,&plane,&capacity,width,height,&view)){
    free(plane);
    return 0;
  }
  *pixels=plane;
  return 1;
}
/* An atlas rectangle as one ARGB plane in renderer-owned scratch. A sprite frame and a font glyph
 * are both this, which is why one function serves both. Valid until the next call. */
int gml_render_atlas_rect_plane(GmlRender *r,int atlas,int sx,int sy,int w,int h,
                                const uint32_t **pixels){
  if(!r || !pixels || atlas<0 || atlas>=r->n_atlas || w<=0 || h<=0) return 0;
  {
    size_t count=(size_t)w*(size_t)h;
    uint8_t *ap=atlas_pixels(r,atlas);
    GmlAtlas *a=&r->atlas[atlas];
    if(!ap || a->w<=0 || a->h<=0) return 0;
    if(sx<0 || sy<0 || sx+w>a->w || sy+h>a->h) return 0;
    if(count>16777216u) return 0;
    if(count>r->content_sampler_plane_capacity){
      uint32_t *grown=realloc(r->content_sampler_plane,count*sizeof *grown);
      if(!grown) return 0;
      r->content_sampler_plane=grown;
      r->content_sampler_plane_capacity=count;
    }
    for(int y=0;y<h;y++)
      for(int x=0;x<w;x++){
        const uint8_t *px=ap+((size_t)(sy+y)*a->w+(sx+x))*4u;
        r->content_sampler_plane[(size_t)y*w+x]=
          ((uint32_t)px[3]<<24)|((uint32_t)px[0]<<16)|((uint32_t)px[1]<<8)|(uint32_t)px[2];
      }
    *pixels=r->content_sampler_plane;
    return 1;
  }
}

int gml_render_surface_plane(GmlRender *r,int surface,int *width,int *height,
                             const uint32_t **pixels){
  int w=0,h=0;
  const uint32_t *px;
  if(!r || !pixels) return 0;
  px=surface_pixels_observe(r,surface,&w,&h);
  if(!px || w<=0 || h<=0) return 0;
  if(width) *width=w;
  if(height) *height=h;
  *pixels=px;
  return 1;
}

void gml_render_texture_page_cache_clear(GmlRender *render,GmlTpag *page){
  if(!page) return;
  int sx=page->sx,sy=page->sy,sw=page->sw,sh=page->sh;
  int tx=page->tx,ty=page->ty,bw=page->bw,bh=page->bh,atlas=page->atlas;
  free(page->alpha_row_min);
  free(page->alpha_row_max);
  free(page->alpha_qrow_min);
  free(page->alpha_qrow_max);
  free(page->alpha_qrow_built);
  free(page->alpha_runs);
  free(page->alpha8_cache);
  free(page->argb_cache);
  free(page->solid_blur_alpha_cache);
  for(int phase=0;phase<3;phase++) free(page->interp_phase_cache[phase]);
  free(page->fast8_draw_cache);
  free(page->interp_draw_cache);
  free(page->interp_draw_runs);
  if(render){
    if(page->interp_draw_cache_bytes<=render->interp_draw_cache_bytes)
      render->interp_draw_cache_bytes-=page->interp_draw_cache_bytes;
    else
      render->interp_draw_cache_bytes=0;
  }
  memset(page,0,sizeof(*page));
  page->sx=sx; page->sy=sy; page->sw=sw; page->sh=sh;
  page->tx=tx; page->ty=ty; page->bw=bw; page->bh=bh; page->atlas=atlas;
}
void gml_render_interpolated_subrect_cache_clear(GmlRender *render){
  if(!render) return;
  for(int index=0;index<render->interp_subrect_count;index++)
    for(int phase=0;phase<3;phase++)
      free(render->interp_subrect_cache[index].phase[phase]);
  free(render->interp_subrect_cache);
  render->interp_subrect_cache=NULL;
  render->interp_subrect_count=0;
  render->interp_subrect_capacity=0;
}
void gml_render_free(GmlRender *r){
  gml_render_flush_rotated_batch(r);
  /* Pending draws can borrow numbered sources. Finish while their allocations still exist;
   * a failed teardown submission must not survive into the later target-resource retirement. */
  (void)gml_render_application_scene_flush(r);
  gml_render_application_scene_queue_dispose(r);
  atlas_pool_free(r);   /* before atlas teardown: workers read r->atlas/win */
  gml_row_pool_free(r);  /* compositor workers must stop before their renderer workspaces vanish */
  gml_render_atlas_scene_retire_all(r);
  for(int i=0;i<GML_MAX_SURFACES;i++){
    gml_render_surface_resource_dispose(r,i+1);
    free(r->surface[i].px); free(r->surface[i].rle);
  }
  for(int i=0;i<GML_MAX_FONTS;i++){
    if(r->fonts[i].runtime_face) gml_font_raster_face_close(r->fonts[i].runtime_face);
    free(r->fonts[i].map); free(r->fonts[i].glyphs); free(r->fonts[i].kerning);
    free(r->fonts[i].runtime_source_path);
  }
  free(r->default_font.map); free(r->default_font.glyphs);
  for(int i=0;i<r->n_spr;i++){
    (void)gml_render_runtime_sprite_scene_retire(r,&r->spr[i]);
    gml_render_sprite_cache_free(&r->spr[i]);
    gml_render_sprite_state_cache_clear(&r->spr[i]);
    gml_render_free_spine(r->spr[i].spine);
    free(r->spr[i].runtime_rgba);
    free(r->spr[i].runtime_mask);
    free(r->spr[i].runtime_row_min);
    free(r->spr[i].runtime_row_max);
    free(r->spr[i].owned_name);
    free(r->spr[i].runtime_source_path);
  }
  for(int i=0;i<r->n_tpag;i++){
    gml_render_texture_page_cache_clear(r,&r->tpag[i]);
  }
  gml_render_interpolated_subrect_cache_clear(r);
  for(int i=0;i<r->n_atlas;i++){
    free(r->atlas[i].px);
    free(r->atlas[i].external_blob);
  }
  for(int i=0;i<r->n_spr;i++) free(r->spr[i].frame);
  free(r->spr_name_hix); r->spr_name_hix=NULL; r->spr_name_hix_cap=0;
  free(r->classic_info_native_pixels);
  free(r->layer_noise_rgb); r->layer_noise_rgb=NULL;
  if(render_setting(r,"GML_SHADER_ACCOUNT")) gml_render_shader_account_report(r);
  free(r->shader_account); r->shader_account=NULL; r->shader_account_shaders=0;
  for(int i=0;i<GML_SHADED_FRAME_CACHE;i++){ free(r->shaded_frame[i].px); r->shaded_frame[i].px=NULL; }
  free(r->shaded_bytes); r->shaded_bytes=NULL; r->shaded_bytes_capacity=0;
  free(r->run_plane); r->run_plane=NULL; r->run_plane_capacity=0;
  free(r->content_sampler_plane);
  free(r->shaded_plane); r->shaded_plane=NULL; r->shaded_plane_capacity=0; r->content_sampler_plane=NULL; r->content_sampler_plane_capacity=0;
  free(r->layer_filter_src); free(r->layer_filter_work); free(r->layer_filter_aux);
  free(r->app_presentation_coverage);
  free(r->app_presentation_alpha_scratch);
  r->app_presentation_coverage=r->app_presentation_alpha_scratch=NULL;
  r->app_presentation_coverage_capacity=0;
  r->app_presentation_coverage_active=0;
  free(r->color_write_scratch); r->color_write_scratch=NULL;
  r->color_write_scratch_capacity=0;
  r->layer_filter_src=r->layer_filter_work=r->layer_filter_aux=NULL;
  r->layer_filter_capacity=0; r->layer_filter_active=0;
  gml_render_application_resource_dispose(r);
  gml_render_classic_phase_resource_dispose(r);
  gml_render_base_resource_dispose(r);
  gml_render_retained_base_dispose(r);
  /* The shared batch queue can belong to a SCREEN-only pass with no owned
   * application resource to retire it. Both resources are flushed above. */
  gml_render_application_scene_queue_dispose(r);
  free(r->app_surface_owned); r->app_surface_owned=NULL;
  free(r->rotated_batch); r->rotated_batch=NULL;
  r->rotated_batch_count=r->rotated_batch_capacity=0;
  free(r->atlas); free(r->spr); free(r->tpag); free(r->bg); free(r->shader_pal);
  free(r->tpag_ptr); r->tpag_ptr=NULL;
}
void gml_render_begin(GmlRender *r, uint32_t *fb, int w, int h, double cx, double cy){
  if(!gml_render_application_scene_flush(r)) return;
  gml_render_flush_rotated_batch(r);
  if(r->resident_application_pass && r->dispatch_frame_accounting &&
     r->dispatch_frame_accounting->pending_cause==GML_RENDER_RESIDENT_UNKNOWN)
    gml_render_dispatch_resident_lost(r,GML_RENDER_RESIDENT_PASS_RESTART);
  r->resident_application_pass=0;
  r->resident_scene_pass=0;
  r->fb=fb; r->fbw=w; r->fbh=h; r->base_fb=fb; r->base_fbw=w; r->base_fbh=h;
  r->gui_pass_active=0;
  r->gui_base_logical_w=r->gui_base_logical_h=0;
  r->gui_logical_w=r->gui_logical_h=0;
  r->gui_scale_x=r->gui_scale_y=1.0;
  r->gui_maximise_active=0;
  r->gui_maximise_xscale=r->gui_maximise_yscale=0.0;
  r->gui_maximise_xoffset=r->gui_maximise_yoffset=0.0;
  r->classic_phase_y=NULL;
  r->classic_interp_phase[0]=NULL;
  r->classic_interp_phase[1]=NULL;
  r->classic_interp_phase[2]=NULL;
  r->target_sp=0; r->target_id=-1;
  r->world_transform_active=0;
  r->world_scale_x=r->world_scale_y=1.0;
  r->projection_cam_x=cx; r->projection_cam_y=cy; r->cam_x=cx; r->cam_y=cy;
  gml_d3_sync_render_camera(r);
  r->alphablend=1;
  r->blendmode=0;
  r->fog_flat=0;
  r->blend_equation=r->blend_equation_alpha=1; r->gpu_state_sp=0; r->active_shader=-1;
  /* The fixed-function renderer restores normal source-alpha drawing when it begins a target
   * pass. A draw_enable_alphablend(false) still affects the rest of its current pass, but it must
   * not leak from the screen stage into the next frame's world pass. */
  r->fb_opaque_known=0; r->fb_all_opaque=0; r->fb_all_transparent=0;
  r->pending_underlay=0; r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0; r->pending_fill_color=0;
  /* The target is about to be rebuilt, so a presentation recorded against the previous one
   * describes pixels that no longer belong anywhere. */
  r->pending_presentation=0;
  if(r->app_surface && fb==r->app_surface)
    r->app_presentation_coverage_active=0;
}
int gml_render_content_composited_screen(GmlRender *r){ return r?r->content_composited_screen:0; }
void gml_render_clear_content_composited_screen(GmlRender *r){
  if(r){
    r->content_composited_screen=0;
    r->content_authored_surfaces=0;
  }
}
void gml_render_set_pending_underlay(GmlRender *r, int x, int y, int w, int h){
  gml_render_flush_rotated_batch(r);
  if(!r || !r->app_surface || w<=0 || h<=0){
    if(r) r->pending_underlay=0;
    return;
  }
  r->pending_underlay=1;
  r->underlay_x=x;
  r->underlay_y=y;
  r->underlay_w=w;
  r->underlay_h=h;
  /* Post-Draw selects the program for automatic presentation. Complete that operation now,
   * before GUI events can reset its program, uniforms, samplers or draw state. Unshaded
   * presentation keeps its established deferral and resident composition paths. */
  if(r->active_shader>=0) gml_render_flush_pending_underlay(r);
}
void gml_render_cancel_pending_underlay(GmlRender *r){
  if(!r) return;
  r->pending_underlay=0;
  r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
}
void gml_render_cancel_pending_fill(GmlRender *r){
  if(!r) return;
  r->pending_fill=0;
  r->pending_fill_color=0;
}
void gml_render_set_pending_fill(GmlRender *r, uint32_t color){
  if(!r || !r->fb || r->fbw<=0 || r->fbh<=0) return;
  gml_render_flush_rotated_batch(r);
  gml_render_cancel_pending_underlay(r);
  /* An explicitly owned sample pair clears together. Rejected preparation
   * must observe both images before the established CPU suffix writes them. */
  if(r->classic_phase_resource && r->target_sp==0 && r->fb==r->base_fb){
    if(gml_render_resident_classic_phase_clear(r,color)) return;
    if(!gml_render_resident_application_cpu_barrier(r)) return;
  }
  /* Preserve the fact that content authored the bound surface before replacing it with a deferred
   * transparent clear. The surface compositor consumes this frame-local signal after the target
   * closes; coverage alone correctly describes the final transparent pixels but loses that intent. */
  if(r->target_id>0 && r->target_id<=GML_MAX_SURFACES && !r->fb_all_transparent)
    r->content_authored_surfaces|=UINT64_C(1)<<(r->target_id-1);
  r->pending_fill=1;
  r->pending_fill_color=color;
  if(r->app_surface && r->fb==r->app_surface)
    r->app_presentation_coverage_active=0;
  r->fb_opaque_known=1;
  r->fb_all_opaque=((color>>24)==255u);
  r->fb_all_transparent=((color>>24)==0u);
  int phase_written=0;
  if(r->target_sp==0 && r->classic_phase_y){
    size_t n=(size_t)r->fbw*(size_t)r->fbh;
    uint32_t *p=r->classic_phase_y; size_t left=n;
    while(left>0){
      int run=left>(size_t)INT_MAX?INT_MAX:(int)left;
      gml_render_cpu_backend_fill_xrgb(p,run,color); p+=run; left-=(size_t)run;
    }
    phase_written=1;
  }
  if(r->target_sp==0){
    size_t n=(size_t)r->fbw*(size_t)r->fbh;
    for(int q=0;q<3;q++) if(r->classic_interp_phase[q]){
      uint32_t *p=r->classic_interp_phase[q]; size_t left=n;
      while(left>0){
        int run=left>(size_t)INT_MAX?INT_MAX:(int)left;
        gml_render_cpu_backend_fill_xrgb(p,run,color); p+=run; left-=(size_t)run;
      }
      phase_written=1;
    }
  }
  if(phase_written && r->dispatch_metrics.enabled)
    gml_render_dispatch_cpu_record(r,GML_RENDER_SCENE_PRIMITIVE,
                                     gml_render_dispatch_cpu_reason(r),1u);
}
static void render_flush_pending_fill(GmlRender *r,int independent_allowed){
  gml_render_flush_rotated_batch(r);
  if(!r || !r->pending_fill || !r->fb || r->fbw<=0 || r->fbh<=0) return;
  unsigned reason=r->dispatch_metrics.enabled
    ?gml_render_dispatch_cpu_reason(r):GML_RENDER_DISPATCH_UNQUALIFIED;
  /* A bound persistent image has an identity before world admission. A clear
   * needs neither its predecessor pixels nor a different SCREEN/GUI owner. */
  int independent=independent_allowed && !r->resident_application_pass && r->scene_device_enabled &&
    gml_render_device(r) && r->target_id>=0 &&
    (r->target_id>0 || r->app_surface==r->app_surface_owned);
  if((r->resident_application_pass || independent) && gml_render_scene_target_matches(r)){
    GmlRenderResource *resource=gml_render_scene_target_resource_ensure(r);
    if(!resource) return;
    if(resource){
      /* The clear is submitted after the queued prefix publishes its versions. */
      GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,resource);
      if(image.content_version<UINT64_MAX){
        GmlRenderDrawOp operation={0};
        operation.opcode=GML_RENDER_OP_CLEAR;
        operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
        operation.target.image=image;
        operation.target.image.content_version++;
        operation.target.storage_version=1u;
        operation.target.width=(uint32_t)r->fbw;
        operation.target.height=(uint32_t)r->fbh;
        operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
        operation.target.region=(GmlRenderRect){0,0,(uint32_t)r->fbw,(uint32_t)r->fbh};
        operation.payload.clear_colour=r->pending_fill_color;
        GmlRenderDrawBatch batch={&operation,1u,NULL,0u};
        GmlRenderResourceExecution result;
        if(independent){
          if(!gml_render_application_scene_flush(r)) return;
          result=gml_render_resource_submit_deferred(resource,gml_render_device(r),
                                                     &batch,NULL,0u);
          gml_render_dispatch_batch_record(r,&batch,result);
          if(result!=GML_RENDER_RESOURCE_EXEC_FAILED && r->fb==r->app_surface_owned)
            r->app_surface_opaque=r->app_presentation_coverage_active=0;
        }else result=gml_render_application_scene_submit(r,&batch,NULL,0u);
        if(result!=GML_RENDER_RESOURCE_EXEC_FAILED){
          r->pending_fill=0;
          return;
        }
      }
      uint32_t *pixels=NULL;
      if(!gml_render_resource_acquire_cpu_write(resource,&pixels) ||
         pixels!=r->fb || !gml_render_resource_commit_cpu_write(resource))
        return;
    }
  }
  if(r->target_id>0 && r->target_id<=GML_MAX_SURFACES){
    GmlSurface *surface=&r->surface[r->target_id-1];
    if(surface->live && surface->resource && r->fb==surface->px){
      int width=0,height=0;
      if(surface->w!=r->fbw || surface->h!=r->fbh ||
         surface_pixels(r,r->target_id,&width,&height)!=r->fb ||
         width!=r->fbw || height!=r->fbh) return;
    }
  }
  uint32_t color=r->pending_fill_color;
  r->pending_fill=0;
  r->fb_opaque_known=1;
  r->fb_all_opaque=((color>>24)==255u);
  r->fb_all_transparent=((color>>24)==0u);
  size_t n=(size_t)r->fbw*(size_t)r->fbh;
  uint32_t *p=r->fb;
  while(n>0){
    int run=n>(size_t)INT_MAX?INT_MAX:(int)n;
    gml_render_cpu_backend_fill_xrgb(p,run,color);
    p+=run;
    n-=(size_t)run;
  }
  if(r->dispatch_metrics.enabled)
    gml_render_dispatch_cpu_record(r,GML_RENDER_SCENE_PRIMITIVE,reason,1u);
}
void gml_render_flush_pending_fill(GmlRender *r){
  render_flush_pending_fill(r,1);
}
/* This entry only resolves a clear for an already selected software suffix;
 * it cannot admit or submit a GPU clear through either target policy. */
static void render_cpu_flush_pending_fill(GmlRender *r){
  if(r && !r->resident_application_pass) render_flush_pending_fill(r,0);
}
int gml_render_resident_application_cpu_barrier(GmlRender *r){
  uint32_t *pixels=NULL;
  GmlRenderResource *resource;
  if(!r) return 0;
  if(!gml_render_classic_phase_cpu_barrier(r)) return 0;
  if(!r->resident_application_pass){
    /* A child binding may have written the same allocation on GPU after this
     * CPU parent was suspended, or sampled it and made its GPU copy current. */
    GmlRenderJournalMetrics metrics={0};
    if((!r->resident_scene_pass && !(r->scene_device_enabled && r->target_id>=0)) ||
       !gml_render_scene_target_matches(r)) return 1;
    /* The suffix has already selected CPU. Resolve its pending clear with the
     * existing CPU writer, rather than submitting it only to read it straight
     * back. A later replay must not overwrite this CPU suffix either. */
    if(r->scene_device_enabled && r->target_id>=0 && r->pending_fill){
      render_cpu_flush_pending_fill(r);
      if(r->pending_fill) return 0;
    }
    resource=gml_render_scene_target_resource_ensure(r);
    if(!resource) return 0;
    gml_render_resource_journal_metrics(resource,&metrics);
    if(metrics.authority==GML_RENDER_RESOURCE_CPU_CURRENT) return 1;
  }
  if(!gml_render_scene_target_matches(r)) return 0;
  gml_render_flush_pending_fill(r);
  if(r->pending_fill) return 0;
  if(!gml_render_application_scene_flush(r)) return 0;
  resource=gml_render_scene_target_resource_ensure(r);
  if(!resource || !gml_render_resource_acquire_cpu_write(resource,&pixels) ||
     pixels!=r->fb || !gml_render_resource_commit_cpu_write(resource)) return 0;
  gml_render_dispatch_resident_lost(r,GML_RENDER_RESIDENT_CPU_BARRIER);
  r->resident_application_pass=0;
  return 1;
}
int gml_render_scene_flush(GmlRender *r){
  return gml_render_application_scene_flush(r) &&
    gml_render_checkpoint_pending_resources(r);
}
int gml_render_scene_submit_pending(GmlRender *r){
  return r && gml_render_application_scene_flush(r);
}
static GmlRenderResource *completed_screen_resource(const GmlRender *r,
                                                    const uint32_t *pixels,
                                                    int width,int height){
  int actual_width=0,actual_height=0;
  GmlRenderResource *resource=gml_render_base_resource_lookup(r,pixels,&actual_width,&actual_height);
  return width>0 && height>0 && actual_width==width && actual_height==height?resource:NULL;
}
int gml_render_completed_screen_read(GmlRender *r,uint32_t *pixels,int width,int height){
  const uint32_t *current=NULL;
  GmlRenderResource *resource;
  GmlRenderJournalMetrics metrics={0};
  if(!r || !pixels || width<=0 || height<=0 ||
     !gml_render_application_scene_flush(r)) return 0;
  resource=completed_screen_resource(r,pixels,width,height);
  if(!resource) return 1;
  /* A logical application borrow may expose more raw pixels than the active SCREEN extent. */
  if(!gml_render_base_history_observe(r)) return 0;
  gml_render_resource_journal_metrics(resource,&metrics);
  if(!gml_render_resource_acquire_cpu_read(resource,&current) || current!=pixels) return 0;
  r->completed_screen_cpu_reads++;
  if(metrics.cpu_version!=metrics.logical_version) r->completed_screen_cpu_materializations++;
  return 1;
}
static int completed_screen_suffix_allowed(GmlRender *r,uint32_t *pixels,
                                            int width,int height){
  GmlRenderImageRef image={0};
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(!r || !device || !device->context || !device->observe_on_demand ||
     !device->observe_on_demand(device->context) ||
     r->target_id!=-1 || r->target_sp || r->fb!=pixels ||
     r->fbw!=width || r->fbh!=height || r->pending_fill ||
     r->pending_underlay || r->pending_presentation ||
     gml_render_deferred_presentation(r,NULL) ||
     !gml_render_completed_screen_image(r,pixels,width,height,&image)) return 0;
  return 1;
}
int gml_render_resume_completed_screen_suffix(GmlRender *r,uint32_t *pixels,
                                              int width,int height){
  if(!completed_screen_suffix_allowed(r,pixels,width,height)) return 0;
  r->resident_scene_pass=1;
  r->resident_application_pass=1;
  gml_render_dispatch_resident_admitted(r);
  return 1;
}
int gml_render_begin_completed_screen_suffix(GmlRender *r,uint32_t *pixels,
                                             int width,int height){
  if(!completed_screen_suffix_allowed(r,pixels,width,height)) return 0;
  /* Beginning a pass changes mapping and draw metadata, not the resource's
   * current device bytes. The ordinary sprite owner may still refuse its suffix
   * and acquire those exact bytes through its established CPU barrier. */
  return gml_render_begin_resident_base(r,pixels,width,height,0.0,0.0);
}
int gml_render_completed_screen_write(GmlRender *r,uint32_t *pixels,int width,int height){
  uint32_t *current=NULL;
  GmlRenderResource *resource;
  if(!r || !pixels || width<=0 || height<=0 ||
     !gml_render_application_scene_flush(r)) return 0;
  if((r->base_scene_resource_pixels==pixels || r->retained_base_pixels==pixels) &&
     !gml_render_base_history_observe(r)) return 0;
  if(r->base_scene_resource && r->base_scene_resource_pixels==pixels &&
     (r->base_scene_resource_width!=width || r->base_scene_resource_height!=height)){
    if(!gml_render_resource_acquire_cpu_read(r->base_scene_resource,NULL)) return 0;
    gml_render_base_resource_dispose(r);
  }
  if(r->retained_base_resource && r->retained_base_pixels==pixels &&
     (r->retained_base_width!=width || r->retained_base_height!=height)){
    if(!gml_render_resource_acquire_cpu_read(r->retained_base_resource,NULL)) return 0;
    gml_render_retained_base_dispose(r);
  }
  resource=completed_screen_resource(r,pixels,width,height);
  if(resource && (!gml_render_resource_acquire_cpu_write(resource,&current) ||
                 current!=pixels || !gml_render_resource_commit_cpu_write(resource))) return 0;
  if(r->fb==pixels){
    if(r->resident_application_pass)
      gml_render_dispatch_resident_lost(r,GML_RENDER_RESIDENT_CPU_BARRIER);
    r->resident_application_pass=0;
  }
  return 1;
}
int gml_render_completed_screen_prepare_publish(GmlRender *r,uint32_t *pixels,
                                                int width,int height){
  if(!r || !pixels || width<=0 || height<=0 ||
     !gml_render_application_scene_flush(r)) return 0;
  return !completed_screen_resource(r,pixels,width,height) || gml_render_base_history_observe(r);
}
void gml_render_completed_screen_publish(GmlRender *r,uint32_t *pixels,int width,int height){
  if(!r || !pixels || width<=0 || height<=0) return;
  GmlRenderResource *resource=completed_screen_resource(r,pixels,width,height);
  if(resource==r->retained_base_resource && resource) gml_render_retained_base_dispose(r);
  else if(resource) gml_render_base_resource_dispose(r);
  gml_render_discard_deferred_presentation(r);
  if(r->fb==pixels){
    if(r->resident_application_pass)
      gml_render_dispatch_resident_lost(r,GML_RENDER_RESIDENT_CPU_BARRIER);
    r->resident_application_pass=0;
  }
}
int gml_render_completed_screen_image(GmlRender *r,const uint32_t *pixels,int width,int height,
                                      GmlRenderImageRef *image){
  GmlRenderResourceSource source={0};
  GmlRenderResource *resource;
  if(image) memset(image,0,sizeof *image);
  if(!r || !image || !gml_render_application_scene_flush(r) ||
     !(resource=completed_screen_resource(r,pixels,width,height)) ||
     !gml_render_resource_device_source_view(resource,gml_render_device(r),&source)) return 0;
  *image=source.image;
  return image->resource_class==GML_RENDER_RESOURCE_SCREEN;
}
int gml_render_defer_resident_screen_upscale(GmlRender *r,const uint32_t *source,
                                            int source_width,int source_height,
                                            uint32_t *target,int target_width,int target_height){
  GmlRenderDeferredPresentation record={0};
  GmlRenderImageRef image={0};
  if(!r || !source || !target || source==target ||
     source_width<=0 || source_height<=0 || target_width<=0 || target_height<=0 ||
     source_width>(int)GML_PLAN_MAX_EXTENT || source_height>(int)GML_PLAN_MAX_EXTENT ||
     target_width>(int)GML_PLAN_MAX_EXTENT || target_height>(int)GML_PLAN_MAX_EXTENT ||
     !r->presentation_deferral_enabled || r->pending_fill || r->pending_underlay ||
     r->pending_presentation || r->target_sp || r->target_id>=0 ||
     r->fb!=source || r->base_fb!=source ||
     !gml_render_completed_screen_image(r,source,source_width,source_height,&image)) return 0;
  /* R03_INDIRECT_GUI_PRESENT: the complete GUI raster is the source, not an
   * application substitute or a borrowed CPU plane uploaded for presentation. */
  record.sampling_rule=target_width<source_width || target_height<source_height
    ?GML_RENDER_PRESENTATION_BOX:GML_RENDER_PRESENTATION_PIXEL_CENTRE;
  record.target_pixels=target;
  record.target_width=record.destination_width=target_width;
  record.target_height=record.destination_height=target_height;
  record.source_pixels=source;
  record.source_width=record.source_pitch=source_width;
  record.source_height=source_height;
  record.extent_x=target_width;
  record.extent_y=target_height;
  record.identity=image.identity;
  record.generation=r->presentation.generation+1u;
  record.resident_image=image;
  record.shader=-1;
  record.cpu_rgb_only=1;
  r->presentation=record;
  r->pending_presentation=1;
  return 1;
}
int gml_render_completed_screen_has_rgb(GmlRender *r,uint32_t *pixels,int width,int height,
                                        int *has_rgb){
  GmlRenderImageRef image={0};
  const GmlRenderDeviceServices *device;
  if(has_rgb) *has_rgb=0;
  if(!r || !pixels || width<=0 || height<=0 || !has_rgb) return 0;
  device=gml_render_device(r);
  if(device && device->has_rgb &&
     gml_render_completed_screen_image(r,pixels,width,height,&image) &&
     device->has_rgb(device->context,image,has_rgb)) return 1;
  if(!gml_render_completed_screen_read(r,pixels,width,height)) return 0;
  for(size_t index=0;index<(size_t)width*(size_t)height;index++)
    if(pixels[index]&0xFFFFFFu){ *has_rgb=1; break; }
  return 1;
}
void gml_render_flush_pending_underlay(GmlRender *r){
  gml_render_flush_rotated_batch(r);
  if(!r || !r->pending_underlay) return;
  if(gml_render_compose_resident_underlay(r)) return;
  /* The internal compositor calls the CPU leaf directly, unlike public surface dispatch.
   * Materialise and leave a resident SCREEN before that leaf observes or writes it. */
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  int x=r->underlay_x, y=r->underlay_y, w=r->underlay_w, h=r->underlay_h;
  r->pending_underlay=0;
  /* This is the host's internal application-surface presentation, not a GML draw call.
   * Routing it through gml_draw_surface_stretched while d3d is active projects the already-rendered
   * frame through the active camera a second time, producing missing scanline bands. Keep the
   * copy and any content-program handoff in screen space, bypassing public draw projection. */
  if(w>0 && h>0){
    int sw=r->app_w,sh=r->app_h;
    if(sw>0 && sh>0){
      int saved_application_opaque=r->app_surface_opaque;
      int shader=r->active_shader;
      int content_program=shader>=0 && r->shader_executor && r->app_surface!=r->fb &&
        gml_render_shader_content_candidate(r,shader);
      if(content_program && surface_pixels_observe(r,0,NULL,NULL)!=r->app_surface) return;
      int first_generation=r->win && anygm_policy_uses_first_generation_studio(r->win);
      size_t count=(size_t)sw*(size_t)sh;
      int use_presentation_coverage=first_generation &&
        r->app_presentation_coverage_active &&
        r->app_presentation_coverage && r->app_presentation_alpha_scratch &&
        count<=r->app_presentation_coverage_capacity;
      if(use_presentation_coverage){
        /* This temporary alpha borrow must start from the current resource image.
         * A later surface read may replay queued GPU work over the borrowed bytes. */
        if(surface_pixels_observe(r,0,NULL,NULL)!=r->app_surface) return;
        /* Surface 0 retains the target alpha that explicit surface draws read.
         * Automatic first-generation presentation instead uses coverage authored by a
         * complete black mask. Borrow the alpha byte for that draw, then restore it. */
        for(size_t i=0;i<count;i++){
          r->app_presentation_alpha_scratch[i]=(uint8_t)(r->app_surface[i]>>24);
          r->app_surface[i]=(r->app_surface[i]&0x00ffffffu)|
                            ((uint32_t)r->app_presentation_coverage[i]<<24);
        }
        r->app_surface_opaque=0;
      } else if(first_generation){
        /* Partial texture coverage remains available to explicit surface-0 draws but does not
         * attenuate automatic first-generation presentation a second time. */
        r->app_surface_opaque=1;
      }
      GmlRenderDispatchCpuSuspension dispatch;
      gml_render_dispatch_cpu_scope_suspend(r,&dispatch);
      /* The CPU leaf subtracts the camera; the internal presentation rectangle already names
       * screen pixels. Compensate only that subtraction, without applying a world/D3 transform. */
      double dx=(double)x+r->cam_x,dy=(double)y+r->cam_y;
      float saved_vertex_colour[4];
      int saved_vertex_colour_valid=r->shade_vertex_colour_valid;
      memcpy(saved_vertex_colour,r->shade_vertex_colour,sizeof saved_vertex_colour);
      /* Automatic presentation carries white, not a tint left by the previous content draw. */
      for(int channel=0;channel<4;channel++) r->shade_vertex_colour[channel]=1.0f;
      r->shade_vertex_colour_valid=1;
      GmlRenderShaderGeometry geometry=gml_render_shader_geometry(r,x,y,w,h,0);
      int shaded=content_program && gml_render_cpu_shader_pass_surface(
        r,0,r->app_surface,sw,sh,0,0,sw,sh,dx,dy,w,h,0xFFFFFF,1.0,&geometry);
      memcpy(r->shade_vertex_colour,saved_vertex_colour,sizeof saved_vertex_colour);
      r->shade_vertex_colour_valid=saved_vertex_colour_valid;
      if(!shaded) draw_surface_region(r,0,0,0,sw,sh,dx,dy,w,h,0xFFFFFF,1.0);
      gml_render_dispatch_cpu_scope_resume(r,&dispatch);
      if(use_presentation_coverage)
        for(size_t i=0;i<count;i++)
          r->app_surface[i]=(r->app_surface[i]&0x00ffffffu)|
                            ((uint32_t)r->app_presentation_alpha_scratch[i]<<24);
      r->app_surface_opaque=saved_application_opaque;
      if(shader>=0)
        gml_render_shader_account(r,shader,GML_RENDER_SHADER_DRAW_SURFACE,shaded);
    }
  }
}
int gml_render_defer_resident_underlay(GmlRender *r){
  GmlRenderJournalMetrics metrics={0};
  GmlRenderResource *resource=NULL;
  GmlRenderImageRef image;
  int x,y,w,h;
  if(!r || !r->pending_underlay || !r->presentation_deferral_enabled ||
     !r->app_surface_opaque ||
     r->fb!=r->base_fb || r->target_sp || r->target_id>=0) return 0;
  if(r->app_surface==r->app_surface_owned) resource=r->app_surface_resource;
  else if(r->classic && r->app_surface==r->base_scene_resource_pixels &&
          r->app_w==r->base_scene_resource_width &&
          r->app_h==r->base_scene_resource_height) resource=r->base_scene_resource;
  if(!resource) return 0;
  /* Early classic integer presentation uses the same leading-edge samples. Interpolation at
   * 1:1 still selects that single texel. Temporal phase planes, filtered enlargement and
   * fractional ports retain their checked CPU path. */
  if(r->classic && (!r->win || anygm_policy_classic_modern_presentation(r->win) ||
     (r->interp && (r->underlay_w!=r->app_w || r->underlay_h!=r->app_h ||
       r->active_shader>=0 || (r->blendmode!=0 && r->blendmode!=6) ||
       r->blend_equation!=1 || r->blend_equation_alpha!=1 ||
       (r->alpha_test_enable && r->alpha_test_ref==255u))) ||
     r->app_w<=0 || r->app_h<=0 || r->underlay_w<=0 || r->underlay_h<=0 ||
     r->underlay_w%r->app_w || r->underlay_h%r->app_h ||
     r->classic_phase_y || r->app_phase_y)) return 0;
  if(r->classic)
    for(unsigned index=0;index<3u;index++)
      if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  if(!gml_render_application_scene_flush(r)) return 0;
  image=gml_render_resource_image_ref(resource);
  gml_render_resource_journal_metrics(resource,&metrics);
  if((image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      image.resource_class!=GML_RENDER_RESOURCE_SCREEN) ||
     image.content_version!=metrics.gpu_version ||
     (metrics.authority!=GML_RENDER_RESOURCE_GPU_RESIDENT &&
      metrics.authority!=GML_RENDER_RESOURCE_GPU_REPLAYABLE &&
      metrics.authority!=GML_RENDER_RESOURCE_BOTH_CURRENT)) return 0;
  x=r->underlay_x; y=r->underlay_y; w=r->underlay_w; h=r->underlay_h;
  r->pending_underlay=0;
  if(!render_record_deferred_underlay(r,r->app_surface,r->app_w,r->app_h,
                                     x,y,w,h,1)){
    r->pending_underlay=1;
    return 0;
  }
  r->presentation.resident_image=image;
  return 1;
}
int rect_covers_target(GmlRender *r, int x0, int y0, int x1, int y1){
  if(!r || r->fbw<=0 || r->fbh<=0) return 0;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  return x0<=0 && y0<=0 && x1>=r->fbw && y1>=r->fbh;
}
static int pending_underlay_covered(GmlRender *r, int x0, int y0, int x1, int y1){
  if(!r || !r->pending_underlay) return 0;
  int ux0=r->underlay_x, uy0=r->underlay_y;
  int ux1=ux0+r->underlay_w, uy1=uy0+r->underlay_h;
  if(ux0<0) ux0=0;
  if(uy0<0) uy0=0;
  if(ux1>r->fbw) ux1=r->fbw;
  if(uy1>r->fbh) uy1=r->fbh;
  if(ux1<=ux0 || uy1<=uy0) return 1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  return x0<=ux0 && y0<=uy0 && x1>=ux1 && y1>=uy1;
}
void gml_render_prepare_draw(GmlRender *r){
  gml_render_flush_pending_underlay(r);
  gml_render_flush_pending_fill(r);
  gml_render_cpu_present_image(r);
}

void gml_render_set_deferred_presentation(GmlRender *r,int enabled){
  if(!r) return;
  if(!enabled) gml_render_cpu_present_image(r);
  r->presentation_deferral_enabled=enabled?1:0;
}

int gml_render_deferred_presentation(const GmlRender *r,GmlRenderDeferredPresentation *out){
  if(!r || !r->pending_presentation) return 0;
  if(out) *out=r->presentation;
  return 1;
}

void gml_render_flush_deferred_presentation(GmlRender *r){
  gml_render_cpu_present_image(r);
}

void gml_render_discard_deferred_presentation(GmlRender *r){
  if(!r) return;
  r->pending_presentation=0;
}
void gml_render_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1){
  if(!r) return;
  if(r->pending_underlay){
    if(pending_underlay_covered(r,x0,y0,x1,y1)) gml_render_cancel_pending_underlay(r);
    else gml_render_flush_pending_underlay(r);
  }
  if(r->pending_fill){
    if(rect_covers_target(r,x0,y0,x1,y1)) gml_render_cancel_pending_fill(r);
    else gml_render_flush_pending_fill(r);
  }
}
static inline void render_maybe_prepare_draw_local(GmlRender *r){
  if(r){
    if(!r->rotated_batch_building) gml_render_flush_rotated_batch(r);
    if(r->pending_underlay || r->pending_fill) gml_render_prepare_draw(r);
    if(r->pending_presentation) gml_render_cpu_present_image(r);
    r->fb_all_transparent=0;
  }
}
static inline void render_maybe_prepare_opaque_rect_local(GmlRender *r, int x0, int y0, int x1, int y1){
  if(r){
    if(r->pending_underlay || r->pending_fill) gml_render_prepare_opaque_rect(r,x0,y0,x1,y1);
    /* An opaque rectangle that covers the target would hide the deferred presentation, but one
     * that does not would sit on stale pixels. Writing it first is the answer to both. */
    if(r->pending_presentation) gml_render_cpu_present_image(r);
    r->fb_all_transparent=0;
  }
}
void gml_render_maybe_prepare_draw(GmlRender *r){
  render_maybe_prepare_draw_local(r);
}
void gml_render_maybe_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1){
  render_maybe_prepare_opaque_rect_local(r,x0,y0,x1,y1);
}
#define gml_render_maybe_prepare_draw render_maybe_prepare_draw_local
#define gml_render_maybe_prepare_opaque_rect render_maybe_prepare_opaque_rect_local
int gml_classic_present_explicit_port(const GmlWin *win, int explicit_window,
                                      int canvas_w, int canvas_h,
                                      int port_x, int port_y, int port_w, int port_h,
                                      int *target_x, int *target_y,
                                      int *target_w, int *target_h){
  if(!win || !anygm_policy_uses_classic_runtime(win) || !explicit_window ||
     canvas_w<=0 || canvas_h<=0 || port_w<=0 || port_h<=0 ||
     !target_x || !target_y || !target_w || !target_h) return 0;
  /* window_set_size changes the host window, not the view port.  Classic GM
   * therefore leaves a smaller port at its declared coordinates and clears
   * the rest of the window instead of fitting that port to the new window. */
  *target_x=port_x;
  *target_y=port_y;
  *target_w=port_w;
  *target_h=port_h;
  return 1;
}

void gml_classic_room_window_size(int room_w, int room_h,
                                  int fixed_scale_pct,
                                  int configured_w, int configured_h,
                                  const int visible[8],
                                  const int xport[8], const int yport[8],
                                  const int wport[8], const int hport[8],
                                  int *window_w, int *window_h){
  int width=room_w>0?room_w:1;
  int height=room_h>0?room_h:1;
  int have_view=0;
  if(fixed_scale_pct>0 && configured_w>0 && configured_h>0){
    if(window_w) *window_w=configured_w;
    if(window_h) *window_h=configured_h;
    return;
  }
  if(visible && xport && yport && wport && hport){
    for(int i=0;i<8;i++) if(visible[i] && wport[i]>0 && hport[i]>0){
      int right=xport[i]+wport[i];
      int bottom=yport[i]+hport[i];
      if(!have_view){ width=right; height=bottom; have_view=1; }
      else { if(right>width) width=right; if(bottom>height) height=bottom; }
    }
  }
  if(width<1) width=1;
  if(height<1) height=1;
  if(window_w) *window_w=width;
  if(window_h) *window_h=height;
}

void gml_classic_present_adjust(const GmlWin *win, int source_w, int source_h,
                                int target_w, int target_h, int interpolated,
                                int *target_x, int *target_y){
  if(!win || !anygm_policy_uses_classic_runtime(win)) return;
  int gm8=anygm_policy_classic_modern_presentation(win);
  int centred_aspect=gm8 && win->classic_scaling<0;
  interpolated=gm8 && interpolated;
  if(target_x && source_w>0 && target_w>source_w){
    if(!interpolated && ((gm8 && target_w==source_w*2) || (!centred_aspect && target_w%source_w!=0)))
      (*target_x)--;
  }
  if(target_y && source_h>0 && target_h>source_h){
    if(!interpolated && ((gm8 && target_h==source_h*2) || (!centred_aspect && target_h%source_h!=0)))
      (*target_y)--;
  }
}
