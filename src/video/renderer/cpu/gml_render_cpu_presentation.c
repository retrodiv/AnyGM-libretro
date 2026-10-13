/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_cpu_presentation.h"
#include "gml_render_internal.h"
#include "gml_render_presentation_axis.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_resources.h"
#include "gml_render_cpu_shader_pass.h"
#include "gml_render_cpu_plan.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A content program is an ordinary source-alpha draw when a CPU suffix consumes it, not an
 * opaque presentation copy. Reuse the established shader executor and surface compositor. The
 * record is eligible only for normal white, untested, unfogged drawing; later draw state must
 * neither alter this operation nor be changed by its materialization. */
static void present_content_shader(GmlRender *r,const GmlRenderDeferredPresentation *record,
                                    const uint32_t *source){
  GmlRenderDrawState saved={0},normal={0};
  const unsigned fields=GML_RENDER_DRAW_STATE_ALPHA_BLEND|GML_RENDER_DRAW_STATE_INTERPOLATION|
    GML_RENDER_DRAW_STATE_BLEND_MODE|GML_RENDER_DRAW_STATE_BLEND_EQUATION|
    GML_RENDER_DRAW_STATE_BLEND_EQUATION_ALPHA|GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE|
    GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK;
  uint32_t *saved_target=r->fb;
  int saved_width=r->fbw,saved_height=r->fbh,saved_shader=r->active_shader;
  int saved_fog=r->fog_flat,saved_vertex_valid=r->shade_vertex_colour_valid;
  float saved_vertex[4];
  memcpy(saved_vertex,r->shade_vertex_colour,sizeof saved_vertex);
  gml_render_draw_state_get(r,&saved);
  normal.alpha_blend=1; normal.interpolation=record->linear;
  normal.blend_equation=normal.blend_equation_alpha=1;
  normal.color_write_mask=15;
  gml_render_draw_state_update(r,&normal,fields);
  r->fb=record->target_pixels; r->fbw=record->target_width; r->fbh=record->target_height;
  r->active_shader=record->shader; r->fog_flat=0;
  r->shade_vertex_colour_valid=1;
  for(int channel=0;channel<4;channel++) r->shade_vertex_colour[channel]=1.0f;
  r->fb_opaque_known=0; r->fb_all_opaque=0; r->fb_all_transparent=0;
  /* The deferred accounting already counted this draw once. Its actual executor/compositor
   * records the completed operation below. */
  if(r->shaded_draws) r->shaded_draws--;
  double x=record->origin_x+r->cam_x,y=record->origin_y+r->cam_y;
  int shaded=r->shader_executor && gml_render_cpu_shader_pass_surface(
    r,(int)record->identity,source,record->source_width,record->source_height,
    0,0,record->source_width,record->source_height,x,y,record->extent_x,record->extent_y,
    0xFFFFFFu,1.0,&record->shader_geometry);
  if(!shaded)
    gml_render_compose_shaded_plane(r,source,record->source_width,record->source_height,
      0,0,record->source_width,record->source_height,x,y,record->extent_x,record->extent_y,
      0xFFFFFFu,1.0);
  r->fb=saved_target; r->fbw=saved_width; r->fbh=saved_height;
  r->active_shader=saved_shader; r->fog_flat=saved_fog;
  r->shade_vertex_colour_valid=saved_vertex_valid;
  memcpy(r->shade_vertex_colour,saved_vertex,sizeof saved_vertex);
  gml_render_draw_state_update(r,&saved,fields);
}

/* Write a recorded presentation: the full-target fill it was recorded with, if any, and then the
 * blit. This is the only implementation of that operation, so a frame that ends up on the processor
 * and a frame that does not cannot describe different pictures. */
void gml_render_cpu_present_image(GmlRender *r){
  if(!r || !r->pending_presentation) return;
  {
    const GmlRenderDeferredPresentation record=r->presentation;
    const uint32_t *src=record.source_pixels;
    uint32_t *target=record.target_pixels;
    const int target_width=record.target_width;
    const int sw=record.source_width,sh=record.source_height;
    const int x0=record.destination_x,y0=record.destination_y;
    const int x1=x0+record.destination_width,y1=y0+record.destination_height;
    const double dw=record.extent_x,dh=record.extent_y;
    const uint32_t source_mask=record.cpu_rgb_only?0x00FFFFFFu:UINT32_MAX;
    const uint32_t output_alpha=record.cpu_rgb_only?0u:0xFF000000u;
    int span=x1-x0;
    int column_stack[1024];
    int *column;
    GmlRenderPlan box_plan;
    /* Resolve a resident source before consuming the record. A failed no-context replay must not
     * silently turn a pending presentation into an empty completed frame. */
    if(record.resident_image.resource_class){
      GmlRenderResource *resource=record.resident_image.resource_class==GML_RENDER_RESOURCE_SCREEN
        ?r->base_scene_resource:r->app_surface_resource;
      GmlRenderImageRef current=gml_render_resource_image_ref(resource);
      if(current.resource_class!=record.resident_image.resource_class ||
         current.identity!=record.resident_image.identity ||
         current.incarnation!=record.resident_image.incarnation ||
         current.content_version!=record.resident_image.content_version ||
         !gml_render_resource_acquire_cpu_read(resource,&src)) return;
    }
    if(record.sampling_rule==GML_RENDER_PRESENTATION_BOX){
      GmlPlanImage image={.image_class=GML_PLAN_IMAGE_COMPLETED_FRAME,
        .width=(uint32_t)sw,.height=(uint32_t)sh,
        .pitch_pixels=(uint32_t)record.source_pitch,
        .pixel_format=GML_PLAN_PIXEL_XRGB8888,.opaque=1u,.cpu_pixels=src};
      gml_render_plan_reset(&box_plan,GML_PLAN_TARGET_CPU_FRAME,
        (uint32_t)target_width,(uint32_t)record.target_height);
      uint32_t source=gml_render_plan_add_image(&box_plan,&image);
      if(source==GML_PLAN_NO_IMAGE || record.shader>=0 || !record.cpu_rgb_only ||
         !gml_render_plan_add_blit_box(&box_plan,source,
           (GmlPlanRect){x0,y0,(uint32_t)span,(uint32_t)(y1-y0)},0u) ||
         !gml_render_plan_validate(&box_plan)) return;
    }
    /* Cleared before writes: the kernel calls nothing that could re-enter, but a flush during a
     * flush would write the same pixels twice for no reason. */
    r->pending_presentation=0;
    if(!target || target_width<=0 || record.target_height<=0) return;
    if(record.has_fill){
      /* The same fill the deferred full-target overwrite would have written, including the
       * coverage it leaves behind. */
      size_t remaining=(size_t)target_width*(size_t)record.target_height;
      uint32_t *cursor=target;
      r->fb_opaque_known=1;
      r->fb_all_opaque=((record.fill_color>>24)==255u);
      r->fb_all_transparent=((record.fill_color>>24)==0u);
      while(remaining>0){
        int run=remaining>(size_t)INT_MAX?INT_MAX:(int)remaining;
        gml_render_cpu_backend_fill_xrgb(cursor,run,record.fill_color);
        cursor+=run;
        remaining-=(size_t)run;
      }
      r->fb_all_transparent=0;
      if(r->dispatch_metrics.enabled)
        gml_render_dispatch_cpu_record(r,GML_RENDER_SCENE_PRIMITIVE,
                                         gml_render_dispatch_cpu_reason(r),1u);
    }
    if(!src || span<=0 || y1<=y0 || sw<=0 || sh<=0 || dw<=0.0 || dh<=0.0)
      goto materialized;
    if(record.shader>=0){
      present_content_shader(r,&record,src);
      goto materialized;
    }
    /* Both allocation outcomes below execute a complete copy, with no callback
     * or early rejection. This is a separate draw even when a later primitive
     * triggered its materialization inside that primitive's coverage scope. */
    if(r->dispatch_metrics.enabled)
      gml_render_dispatch_cpu_record(r,GML_RENDER_SCENE_SURFACE,
                                       gml_render_dispatch_cpu_reason(r),1u);
    if(record.sampling_rule==GML_RENDER_PRESENTATION_BOX){
      /* Reuse the existing integer channel sums and floor division, including repeated
       * zero-width footprints on an enlarged axis. No separate CPU sampling algorithm. */
      (void)gml_render_cpu_plan_execute(&box_plan,target,(uint32_t)target_width);
      goto materialized;
    }
    column=span<=(int)(sizeof column_stack/sizeof *column_stack)?column_stack:
           (int*)malloc((size_t)span*sizeof(*column));
    if(!column){
      for(int y=y0;y<y1;y++){
        int sy=presentation_source_index(&record,1,y-y0,y);
        uint32_t *destination=target+(size_t)y*target_width+x0;
        for(int x=x0;x<x1;x++,destination++)
          *destination=(src[(size_t)sy*record.source_pitch+
                           presentation_source_index(&record,0,x-x0,x)]&source_mask)|output_alpha;
      }
      goto materialized;
    }
    for(int x=x0;x<x1;x++) column[x-x0]=presentation_source_index(&record,0,x-x0,x);
    {
      int previous_sy=-1;
      const uint32_t *previous_row=NULL;
      for(int y=y0;y<y1;y++){
        int sy=presentation_source_index(&record,1,y-y0,y);
        uint32_t *destination=target+(size_t)y*target_width+x0;
        if(sy==previous_sy){
          memcpy(destination,previous_row,(size_t)span*sizeof(*destination));
          continue;
        }
        {
          const uint32_t *source_row=src+(size_t)sy*record.source_pitch;
          for(int x=0;x<span;x++) destination[x]=(source_row[column[x]]&source_mask)|output_alpha;
        }
        previous_sy=sy;
        previous_row=destination;
      }
    }
    if(column!=column_stack) free(column);
materialized:
    /* This kernel replaced the deferred target on the processor. Retiring its
     * previous SCREEN residence prevents a later observer from restoring an
     * older device clear over the newly completed pixels. */
    gml_render_completed_screen_publish(r,target,target_width,record.target_height);
  }
}
