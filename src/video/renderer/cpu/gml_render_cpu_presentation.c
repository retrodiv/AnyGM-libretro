/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_cpu_presentation.h"
#include "gml_render_internal.h"
#include "gml_render_presentation_axis.h"
#include "gml_render_cpu_backend.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
        gml_render_cpu_backend_fill_xrgb(cursor,run,record.fill_color);
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
