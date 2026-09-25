/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared sprite source and subrectangle preparation. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_blit.h"

void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y){
  gml_draw_sprite_ext(r,sprite,subimg,x,y,1,1,0,0xFFFFFF,
                      r && !r->classic ? r->alpha : 1.0);
}

void gml_draw_sprite_part_ext(GmlRender *r, int sprite, int subimg, double sx, double sy,
                              double sw, double sh, double x, double y,
                              double xs, double ys, uint32_t blend, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(sprite<0||sprite>=r->n_spr) return;
  if(gml_d3_draw_sprite_part_2d(r,sprite,subimg,sx,sy,sw,sh,x,y,xs,ys,blend,alpha)) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  /* A region of a sprite drawn through a program this renderer does not execute reaches the same
   * executor; unflipped only, because the composed plane is axis-aligned. */
  if(xs!=0 && ys!=0 && r->active_shader>=0 && r->shader_executor &&
     gml_render_shader_content_candidate(r,r->active_shader)){
    int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
    if(gml_render_shade_target_sprite_part(r,sprite,sub,sx,sy,sw,sh,
                                           x-r->cam_x,y-r->cam_y,sw*xs,sh*ys,blend,alpha)) return;
  }
  if(s->runtime_rgba){
    const uint8_t *fr=runtime_frame_rgba(s,subimg); if(!fr) return;
    gml_render_cpu_blit_rgba_region(r,fr,s->w,s->h,sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,sw*xs,sh*ys,blend,alpha);
    return;
  }
  int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  gml_render_cpu_blit_tpag_part(r,&r->tpag[ti],sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,xs,ys,blend,alpha);
}
