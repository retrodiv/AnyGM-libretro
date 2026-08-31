/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The deferred terminal presentation: whether the frame's last operation may be recorded instead of
 * written, the record itself, and the one kernel that writes it.
 *
 * This owner keeps recording separate from the composition kernels. Each composition
 * owner adds one call and retains its existing kernel body.
 *
 * Two sampling conventions reach this, and they are not interchangeable — destination pixel centres
 * for a content-owned presentation, the leading output edge for the runtime's automatic one. They
 * agree on integer scales and disagree by a texel almost everywhere else, which is why both are
 * named and why one function owns both expressions. */
#include "gml_render.h"
#include "gml_render_backend.h"
#include "gml_render_internal.h"

#include "anygm_compatibility.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Whether the automatic application-surface presentation below would select exactly one source
 * texel per destination pixel at the leading output edge, and may therefore be recorded instead of
 * written.
 *
 * The two classic families the kernel handles separately sample differently and are excluded by
 * name rather than by hope, and so is an interpolated reduction, which averages rather than
 * selects. The clipped rectangle comes back because the caller needs it to decide whether anything
 * already in the target could survive.
 */
static int presentation_deferral_candidate(GmlRender *r, const uint32_t *src, int sw, int sh,
                                           int x0, int y0, int W, int H, int source_all_opaque,
                                           int *out_x0, int *out_y0, int *out_x1, int *out_y1){
  int px0,py0,px1,py1;
  if(!r || !src || !r->fb || sw<=0 || sh<=0 || W<=0 || H<=0) return 0;
  if(!r->presentation_deferral_enabled || !source_all_opaque) return 0;
  if(r->target_sp!=0 || r->target_id>=0 || r->fb!=r->base_fb || src==r->fb) return 0;
  if(src!=r->app_surface || r->color_write_mask!=0x0F) return 0;
  if(r->pending_underlay || r->pending_presentation) return 0;
  if(r->classic && r->interp && ((W==sw*2 && H==sh*2 &&
                                  r->app_interp_phase[0] && r->app_interp_phase[1] &&
                                  r->app_interp_phase[2]) || (W>sw && H>sh))) return 0;
  if(r->classic && r->win && anygm_policy_classic_modern_presentation(r->win) &&
     r->win->classic_scaling<0 && W>=sw && H>=sh) return 0;
  if(r->interp && !(W>=sw && H>=sh)) return 0;
  px0 = x0 < 0 ? -x0 : 0;
  py0 = y0 < 0 ? -y0 : 0;
  px1 = x0 + W > r->fbw ? r->fbw - x0 : W;
  py1 = y0 + H > r->fbh ? r->fbh - y0 : H;
  if(px0 >= px1 || py0 >= py1) return 0;
  *out_x0=px0;
  *out_y0=py0;
  *out_x1=px1;
  *out_y1=py1;
  return 1;
}



/* The source index a written position selects, in whichever convention the record was made under.
 * One expression per rule, in one place: an accelerated execution builds its integer maps from the
 * same statements, so the two cannot select different texels.
 *
 * `local` counts from the first written position and `absolute` is the target coordinate; the
 * pixel-centre rule needs the second and the leading-edge rule needs the first. */
static int presentation_source_index(const GmlRenderDeferredPresentation *record,
                                     int vertical,int local,int absolute){
  const int source_extent=vertical?record->source_height:record->source_width;
  int index;
  if(record->sampling_rule==GML_RENDER_PRESENTATION_LEADING_EDGE){
    const int destination_extent=(int)(vertical?record->extent_y:record->extent_x);
    const int offset=vertical?record->local_offset_y:record->local_offset_x;
    if(destination_extent<=0) return 0;
    index=(int)(((int64_t)(offset+local)*(int64_t)source_extent)/(int64_t)destination_extent);
  } else {
    const double origin=vertical?record->origin_y:record->origin_x;
    const double extent=vertical?record->extent_y:record->extent_x;
    if(extent<=0.0) return 0;
    index=(int)floor((((double)absolute+0.5)-origin)*(double)source_extent/extent);
  }
  if(index<0) index=0;
  if(index>=source_extent) index=source_extent-1;
  return index;
}

/* Write a recorded presentation: the full-target fill it was recorded with, if any, and then the
 * blit. This is the only implementation of that operation, so a frame that ends up on the processor
 * and a frame that does not cannot describe different pictures. */
void render_write_deferred_presentation(GmlRender *r){
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
    int span=x1-x0;
    int column_stack[1024];
    int *column;
    /* Cleared first: the kernel calls nothing that could re-enter, but a flush during a flush would
     * write the same pixels twice for no reason. */
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
        gml_render_backend_fill_xrgb(cursor,run,record.fill_color);
        cursor+=run;
        remaining-=(size_t)run;
      }
      r->fb_all_transparent=0;
    }
    if(!src || span<=0 || y1<=y0 || sw<=0 || sh<=0 || dw<=0.0 || dh<=0.0) return;
    column=span<=(int)(sizeof column_stack/sizeof *column_stack)?column_stack:
           (int*)malloc((size_t)span*sizeof(*column));
    if(!column){
      for(int y=y0;y<y1;y++){
        int sy=presentation_source_index(&record,1,y-y0,y);
        uint32_t *destination=target+(size_t)y*target_width+x0;
        for(int x=x0;x<x1;x++,destination++)
          *destination=src[(size_t)sy*record.source_pitch+
                           presentation_source_index(&record,0,x-x0,x)]|0xFF000000u;
      }
      return;
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
          for(int x=0;x<span;x++) destination[x]=source_row[column[x]]|0xFF000000u;
        }
        previous_sy=sy;
        previous_row=destination;
      }
    }
    if(column!=column_stack) free(column);
  }
}


/* Record the runtime's automatic application-surface presentation, together with the full-target
 * fill still deferred in front of it when there is one. Returns whether the caller may return
 * without writing anything. */
int render_record_deferred_underlay(GmlRender *r,const uint32_t *src,int sw,int sh,
                                    int x0,int y0,int W,int H,int source_all_opaque){
  int px0,py0,px1,py1;
  if(!presentation_deferral_candidate(r,src,sw,sh,x0,y0,W,H,source_all_opaque,
                                      &px0,&py0,&px1,&py1)) return 0;
  {
    int absolute_x0=x0+px0,absolute_y0=y0+py0;
    int covers=rect_covers_target(r,absolute_x0,absolute_y0,x0+px1,y0+py1);
    int captured_fill=r->pending_fill;
    uint32_t captured_colour=captured_fill?r->pending_fill_color:0u;
    if(!covers && !captured_fill) return 0;
    if(captured_fill) gml_render_cancel_pending_fill(r);
    gml_render_maybe_prepare_draw(r);
    r->presentation.sampling_rule=GML_RENDER_PRESENTATION_LEADING_EDGE;
    r->presentation.target_pixels=r->fb;
    r->presentation.target_width=r->fbw;
    r->presentation.target_height=r->fbh;
    r->presentation.source_pixels=src;
    r->presentation.source_width=sw;
    r->presentation.source_height=sh;
    r->presentation.source_pitch=sw;
    r->presentation.destination_x=absolute_x0;
    r->presentation.destination_y=absolute_y0;
    r->presentation.destination_width=px1-px0;
    r->presentation.destination_height=py1-py0;
    r->presentation.origin_x=0.0;
    r->presentation.origin_y=0.0;
    r->presentation.extent_x=(double)W;
    r->presentation.extent_y=(double)H;
    r->presentation.local_offset_x=px0;
    r->presentation.local_offset_y=py0;
    r->presentation.has_fill=captured_fill;
    r->presentation.fill_color=captured_colour;
    r->presentation.identity=0u;
    r->presentation.shader=-1;
    r->presentation.linear=0;
    r->presentation.generation++;
    r->pending_presentation=1;
    if(covers){
      r->fb_opaque_known=1;
      r->fb_all_opaque=1;
      r->fb_all_transparent=0;
    }
  }
  return 1;
}

/* The content-owned presentation: record it, and write it immediately when nothing can take the
 * deferred operation. One implementation of the operation, reached from both, so the deferred frame
 * and the written one cannot describe different pictures. */
void render_present_first_generation(GmlRender *r,int surf,const uint32_t *src,int sw,int sh,
                                     int x0,int y0,int x1,int y1,
                                     double dx,double dy,double dw,double dh){
  int covers=rect_covers_target(r,x0,y0,x1,y1);
  int deferrable=r->presentation_deferral_enabled && !r->pending_underlay;
  int captured_fill=deferrable && r->pending_fill;
  uint32_t captured_colour=captured_fill?r->pending_fill_color:0u;
  if(deferrable && !captured_fill && !covers) deferrable=0;
  if(deferrable && captured_fill) gml_render_cancel_pending_fill(r);
  gml_render_maybe_prepare_draw(r);
  r->presentation.sampling_rule=GML_RENDER_PRESENTATION_PIXEL_CENTRE;
  r->presentation.target_pixels=r->fb;
  r->presentation.target_width=r->fbw;
  r->presentation.target_height=r->fbh;
  r->presentation.source_pixels=src;
  r->presentation.source_width=sw;
  r->presentation.source_height=sh;
  r->presentation.source_pitch=sw;
  r->presentation.destination_x=x0;
  r->presentation.destination_y=y0;
  r->presentation.destination_width=x1-x0;
  r->presentation.destination_height=y1-y0;
  r->presentation.origin_x=dx;
  r->presentation.origin_y=dy;
  r->presentation.extent_x=dw;
  r->presentation.extent_y=dh;
  r->presentation.local_offset_x=0;
  r->presentation.local_offset_y=0;
  r->presentation.has_fill=captured_fill;
  r->presentation.fill_color=captured_colour;
  r->presentation.identity=(uint32_t)surf;
  r->presentation.shader=-1;
  r->presentation.linear=0;
  r->presentation.generation++;
  r->pending_presentation=1;
  if(covers){
    r->fb_opaque_known=1;
    r->fb_all_opaque=1;
    r->fb_all_transparent=0;
  }
  if(!deferrable) render_write_deferred_presentation(r);
}

/* A surface drawn through the content's own program as the frame's last operation. Recorded only:
 * when something other than the processor takes the record it executes the program, and when the
 * processor writes it, it writes the plain blit, which is what this runtime draws for a program it
 * does not execute. Returns whether the draw was taken; when it was not, the caller draws it. */
int render_present_content_shader(GmlRender *r,int surf,const uint32_t *src,int sw,int sh,
                                  int x0,int y0,int x1,int y1,
                                  double dx,double dy,double dw,double dh,int shader){
  int covers=rect_covers_target(r,x0,y0,x1,y1);
  int deferrable=r->presentation_deferral_enabled && !r->pending_underlay;
  int captured_fill=deferrable && r->pending_fill;
  uint32_t captured_colour=captured_fill?r->pending_fill_color:0u;
  if(!deferrable) return 0;
  if(!captured_fill && !covers) return 0;
  if(captured_fill) gml_render_cancel_pending_fill(r);
  gml_render_maybe_prepare_draw(r);
  r->presentation.sampling_rule=GML_RENDER_PRESENTATION_PIXEL_CENTRE;
  r->presentation.target_pixels=r->fb;
  r->presentation.target_width=r->fbw;
  r->presentation.target_height=r->fbh;
  r->presentation.source_pixels=src;
  r->presentation.source_width=sw;
  r->presentation.source_height=sh;
  r->presentation.source_pitch=sw;
  r->presentation.destination_x=x0;
  r->presentation.destination_y=y0;
  r->presentation.destination_width=x1-x0;
  r->presentation.destination_height=y1-y0;
  r->presentation.origin_x=dx;
  r->presentation.origin_y=dy;
  r->presentation.extent_x=dw;
  r->presentation.extent_y=dh;
  r->presentation.local_offset_x=0;
  r->presentation.local_offset_y=0;
  r->presentation.has_fill=captured_fill;
  r->presentation.fill_color=captured_colour;
  r->presentation.identity=(uint32_t)surf;
  r->presentation.shader=shader;
  /* Recorded for the device to run: the accounting counts it as reaching the device, because it
   * does — the engine executes it at the presentation rather than here. */
  r->shaded_draws++;
  r->presentation.linear=r->interp?1:0;
  r->presentation.generation++;
  r->pending_presentation=1;
  if(covers){
    r->fb_opaque_known=1;
    r->fb_all_opaque=1;
    r->fb_all_transparent=0;
  }
  return 1;
}
