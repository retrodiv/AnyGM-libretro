/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Classic information-page glyph pixels; text layout and selection remain shared. */
#include "gml_render_internal.h"
#include "gml_render_cpu_text.h"

#include <math.h>
#include <stdint.h>

/* ClearType uses a separate coverage value for each LCD channel.  The bundled
 * classic-info atlases retain those three coverages; apply them to the requested
 * foreground colour over the already-painted information-page background. */
static int classic_info_gdi_black_component(int value){
  /* Match the integer endpoints of GDI's six-level ClearType transfer ramp. */
  if(value==15) return 14;
  if(value==26) return 25;
  if(value==46) return 45;
  if(value==56) return 54;
  if(value==29) return 28;
  if(value==72) return 71;
  return value;
}

int gml_render_cpu_classic_info_subpixel_glyph(GmlRender *r,GmlFont *font,GmlGlyph *glyph,
                                       double x,double y,double xscale,double yscale,
                                       uint32_t color,double alpha){
  if(!r||!font||!glyph||!font->subpixel||font->atlas<0||font->atlas>=r->n_atlas||
     alpha<0.999||!r->alphablend||r->blendmode!=0||xscale<=0||yscale<=0) return 0;
  GmlAtlas *atlas=&r->atlas[font->atlas];
  if(!atlas_pixels(r,font->atlas)) return 0;
  int x0=(int)floor(x+0.5),y0=(int)floor(y+0.5);
  int destination_width=(int)ceil(glyph->w*xscale);
  int destination_height=(int)ceil(glyph->h*yscale);
  int first_x=x0<0?-x0:0,first_y=y0<0?-y0:0;
  int last_x=destination_width,last_y=destination_height;
  if(x0+last_x>r->fbw) last_x=r->fbw-x0;
  if(y0+last_y>r->fbh) last_y=r->fbh-y0;
  if(first_x>=last_x||first_y>=last_y) return 1;
  int foreground_r=color&255,foreground_g=(color>>8)&255,foreground_b=(color>>16)&255;
  unsigned reason=r->dispatch_metrics.enabled
    ?gml_render_dispatch_cpu_reason(r):GML_RENDER_DISPATCH_UNQUALIFIED;
  int written=0;
  gml_render_maybe_prepare_draw(r);
  for(int yy=first_y;yy<last_y;yy++){
    uint32_t *destination=r->fb+(size_t)(y0+yy)*r->fbw+x0+first_x;
    int source_y=(int)floor((yy+0.5)/yscale);
    if(source_y<0) source_y=0;
    if(source_y>=glyph->h) source_y=glyph->h-1;
    for(int xx=first_x;xx<last_x;xx++,destination++){
      int source_x=(int)floor((xx+0.5)/xscale);
      if(source_x<0) source_x=0;
      if(source_x>=glyph->w) source_x=glyph->w-1;
      const uint8_t *source=atlas->px+
        ((size_t)(glyph->sy+source_y)*atlas->w+glyph->sx+source_x)*4;
      if(!source[3]) continue;
      written=1;
      int coverage_r=255-source[0],coverage_g=255-source[1],coverage_b=255-source[2];
      uint32_t previous=*destination;
      int background_r=(previous>>16)&255,background_g=(previous>>8)&255,background_b=previous&255;
      int output_r=(foreground_r*coverage_r+background_r*(255-coverage_r)+127)/255;
      int output_g=(foreground_g*coverage_g+background_g*(255-coverage_g)+127)/255;
      int output_b=(foreground_b*coverage_b+background_b*(255-coverage_b)+127)/255;
      if(foreground_r==0) output_r=classic_info_gdi_black_component(output_r);
      if(foreground_g==0) output_g=classic_info_gdi_black_component(output_g);
      if(foreground_b==0) output_b=classic_info_gdi_black_component(output_b);
      *destination=0xFF000000u|((uint32_t)output_r<<16)|((uint32_t)output_g<<8)|(uint32_t)output_b;
    }
  }
  if(written && r->dispatch_metrics.enabled)
    gml_render_dispatch_cpu_record(r,GML_RENDER_SCENE_TEXT,reason,1u);
  return 1;
}
