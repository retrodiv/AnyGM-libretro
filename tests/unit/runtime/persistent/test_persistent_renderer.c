/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"
#include "gml_render.h"
#include "gml_render_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



static int expect_renderer_semantics_exit_code(void){
  {
    /* Wide bytecode-17 room-instance records carry a distinct placement-variable override after
     * the transform fields. Earlier record layouts must not interpret that byte position. */
    uint8_t record[48]={0};
    fixture_w32(record,44,7);
    GmlWin record_win={0}; record_win.data=record; record_win.size=sizeof record;
    record_win.bytecode=17;
    GmlVM record_vm={0}; record_vm.win=&record_win; record_vm.room_rec_stride=48;
    int modern=gml_room_instance_precreate_code(&record_vm,0);
    record_win.bytecode=15;
    int legacy=gml_room_instance_precreate_code(&record_vm,0);
    record_win.bytecode=17; record_vm.room_rec_stride=40;
    int narrow=gml_room_instance_precreate_code(&record_vm,0);
    if(modern!=7 || legacy!=-1 || narrow!=-1){
      fprintf(stderr,"room instance precreate field mismatch: modern=%d legacy=%d narrow=%d\n",
        modern,legacy,narrow); return 1;
    }
  }
  {
    GmlSprite sprite={0}; GmlRender render={0};
    render.spr=&sprite; render.n_spr=1;
    sprite.n_frames=1;
    sprite.playback_speed_valid=1; sprite.playback_speed=15.0f; sprite.playback_speed_type=0;
    double per_second=gml_sprite_animation_delta(&render,0,1.0,60.0);
    sprite.playback_speed=0.5f; sprite.playback_speed_type=1;
    double per_frame=gml_sprite_animation_delta(&render,0,1.0,60.0);
    GmlVM sprite_vm={0}; sprite_vm.render=&render;
    GmlVal sprite_id=vreal(0);
    GmlVal speed_type=gml_builtin_call(&sprite_vm,"sprite_get_speed_type",&sprite_id,1);
    GmlVal set_speed[3]={sprite_id,vreal(12),vreal(0)};
    (void)gml_builtin_call(&sprite_vm,"sprite_set_speed",set_speed,3);
    GmlVal speed=gml_builtin_call(&sprite_vm,"sprite_get_speed",&sprite_id,1);
    sprite.playback_speed_valid=0;
    double legacy=gml_sprite_animation_delta(&render,0,0.75,60.0);
    if(fabs(per_second-0.25)>1e-12 || fabs(per_frame-0.5)>1e-12 || fabs(legacy-0.75)>1e-12 ||
       speed_type.t!=V_REAL || speed_type.d!=1 || speed.t!=V_REAL || speed.d!=12){
      fprintf(stderr,"sprite playback cadence mismatch: fps=%.6f frame=%.6f legacy=%.6f\n",
        per_second,per_frame,legacy); return 1;
    }
  }
  {
    GmlRender render={0};
    render.n_fonts=1; render.n_atlas=1;
    render.atlas=calloc(1,sizeof(*render.atlas));
    if(!render.atlas) return 1;
    render.fonts[0].real=1; render.fonts[0].runtime_owned=1; render.fonts[0].atlas=0;
    render.fonts[0].glyphs=calloc(1,sizeof(*render.fonts[0].glyphs));
    render.atlas[0].px=calloc(4,4);
    if(!render.fonts[0].glyphs || !render.atlas[0].px) return 1;
    gml_font_delete(&render,0);
    if(render.fonts[0].glyphs || render.fonts[0].sprite!=-1 || render.fonts[0].atlas!=-1 ||
       render.atlas[0].px){
      fprintf(stderr,"runtime font deletion retained owned storage\n"); return 1;
    }
    free(render.atlas);
  }
  {
    /* Vertical centring uses the complete glyph-cell extent while line_height remains the
     * authored line advance. A font can legitimately have descenders taller than its nominal
     * em; centring only the advance moves every visible glyph down. */
    GmlRender render={0}; GmlGlyph glyph={0}; GmlAtlas atlas={0};
    uint32_t framebuffer[16*16]={0}; uint8_t pixels[8*4];
    memset(pixels,255,sizeof(pixels));
    render.fbw=render.fbh=16; render.fb=render.base_fb=framebuffer;
    render.n_fonts=1; render.n_atlas=1; render.atlas=&atlas;
    render.font=0; render.valign=1; render.color=0xFFFFFF; render.alpha=1;
    render.alphablend=1; render.software_overlay=1;
    atlas.px=pixels; atlas.w=1; atlas.h=8; atlas.decode_attempted=1;
    memset(render.fonts[0].glyph_by_char,0xFF,sizeof(render.fonts[0].glyph_by_char));
    render.fonts[0].real=1; render.fonts[0].atlas=0;
    render.fonts[0].line_height=4; render.fonts[0].align_height=8;
    render.fonts[0].glyphs=&glyph; render.fonts[0].n_glyphs=1;
    render.fonts[0].glyphs_sorted=1; render.fonts[0].glyph_by_char['A']=0;
    glyph.ch='A'; glyph.w=1; glyph.h=8; glyph.shift=1;
    gml_draw_text(&render,8,10,"A");
    int min_y=16,max_y=-1;
    for(int y=0;y<16;y++) for(int x=0;x<16;x++) if(framebuffer[y*16+x]&0xFFFFFFu){
      if(y<min_y) min_y=y;
      if(y>max_y) max_y=y;
    }
    if(min_y!=6 || max_y!=13){
      fprintf(stderr,"real-font centred extent mismatch: y=%d..%d\n",min_y,max_y); return 1;
    }
  }
  {
    /* Modern FONT records place their glyph cell below the authored text origin by a stored
     * ascender offset.  The offset is part of the font metric, including for top alignment. */
    GmlRender render={0}; GmlGlyph glyph={0}; GmlAtlas atlas={0};
    uint32_t framebuffer[8*8]={0}; uint8_t pixels[4]={255,255,255,255};
    render.fbw=render.fbh=8; render.fb=render.base_fb=framebuffer;
    render.n_fonts=1; render.n_atlas=1; render.atlas=&atlas;
    render.font=0; render.color=0xFFFFFF; render.alpha=1;
    render.alphablend=1; render.software_overlay=1;
    atlas.px=pixels; atlas.w=atlas.h=1; atlas.decode_attempted=1;
    memset(render.fonts[0].glyph_by_char,0xFF,sizeof(render.fonts[0].glyph_by_char));
    render.fonts[0].real=1; render.fonts[0].atlas=0; render.fonts[0].line_height=1;
    render.fonts[0].align_height=1; render.fonts[0].ascender_offset=2;
    render.fonts[0].glyphs=&glyph; render.fonts[0].n_glyphs=1;
    render.fonts[0].glyphs_sorted=1; render.fonts[0].glyph_by_char['A']=0;
    glyph.ch='A'; glyph.w=glyph.h=glyph.shift=1;
    gml_draw_text(&render,3,4,"A");
    if(!(framebuffer[2*8+3]&0xFFFFFFu) || (framebuffer[4*8+3]&0xFFFFFFu)){
      fprintf(stderr,"real-font ascender offset was not applied to the text origin\n"); return 1;
    }
  }
  {
    /* Extended transformed text must apply its unscaled wrap width and custom line separation;
     * these arguments are shared by the plain and colour variants. */
    GmlRender render={0}; GmlGlyph glyphs[2]={{0}}; GmlAtlas atlas={0};
    uint32_t framebuffer[12*12]={0}; uint8_t pixel[4]={255,255,255,255};
    gml_render_begin(&render,framebuffer,12,12,0,0);
    render.n_fonts=1; render.n_atlas=1; render.atlas=&atlas;
    render.font=0; render.color=0xFFFFFF; render.alpha=1; render.alphablend=1;
    render.software_overlay=1;
    atlas.px=pixel; atlas.w=atlas.h=1; atlas.decode_attempted=1;
    memset(render.fonts[0].glyph_by_char,0xFF,sizeof(render.fonts[0].glyph_by_char));
    render.fonts[0].real=1; render.fonts[0].atlas=0; render.fonts[0].line_height=1;
    render.fonts[0].align_height=1; render.fonts[0].glyphs=glyphs;
    render.fonts[0].n_glyphs=2; render.fonts[0].glyphs_sorted=1;
    render.fonts[0].glyph_by_char['A']=0; render.fonts[0].glyph_by_char[' ']=1;
    glyphs[0].ch='A'; glyphs[1].ch=' '; glyphs[0].w=glyphs[1].w=1;
    glyphs[0].h=glyphs[1].h=1; glyphs[0].shift=glyphs[1].shift=1;
    gml_draw_text_ext_transformed(&render,1,1,"AA AA",4,2,1,1,0,0xFFFFFF,1);
    if(!(framebuffer[1*12+1]&0xFFFFFFu) || !(framebuffer[5*12+1]&0xFFFFFFu) ||
       (framebuffer[1*12+4]&0xFFFFFFu)){
      fprintf(stderr,"extended transformed text did not wrap at the declared width: %08x %08x %08x\n",
              framebuffer[1*12+1],framebuffer[5*12+1],framebuffer[1*12+4]); return 1;
    }
  }
  {
    /* Colour circles and ellipses are radial gradients, not flat fills using only the centre
     * colour.  Exercise the public dispatch because cached and uncached bytecode calls share it. */
    GmlVM draw_vm={0}; GmlRender render={0}; uint32_t framebuffer[17*17];
    for(size_t i=0;i<sizeof framebuffer/sizeof *framebuffer;i++) framebuffer[i]=0xFF000000u;
    gml_render_begin(&render,framebuffer,17,17,0,0);
    render.alpha=1; render.alphablend=1; render.circle_precision=24;
    draw_vm.render=&render;
    GmlVal circle_args[6]={vreal(8),vreal(8),vreal(6),vreal(0x0000FF),
                           vreal(0xFF0000),vreal(0)};
    (void)gml_builtin_call(&draw_vm,"draw_circle_colour",circle_args,6);
    uint32_t centre=framebuffer[8*17+8],edge=framebuffer[8*17+13],outside=framebuffer[8*17+15];
    int centre_red=(centre>>16)&255,centre_blue=centre&255;
    int edge_red=(edge>>16)&255,edge_blue=edge&255;
    if(centre_red<=centre_blue || edge_blue<=edge_red || (outside&0xFFFFFFu)){
      fprintf(stderr,"radial colour primitive mismatch: centre=%08x edge=%08x outside=%08x\n",
              centre,edge,outside); return 1;
    }
  }
  {
    /* The basic bm_subtract preset uses (bm_zero,bm_inv_src_colour).  It scales each destination
     * channel independently; it must not be confused with the separate subtract blend equation. */
    GmlVM draw_vm={0}; GmlRender render={0}; uint32_t framebuffer=0xFF80C840u;
    gml_render_begin(&render,&framebuffer,1,1,0,0);
    render.alpha=1; render.alphablend=1; draw_vm.render=&render;
    GmlVal mode=vreal(3),point[3]={vreal(0),vreal(0),vreal(0xC08040)};
    (void)gml_builtin_call(&draw_vm,"gpu_set_blendmode",&mode,1);
    (void)gml_builtin_call(&draw_vm,"draw_point_colour",point,3);
    if(framebuffer!=0xFF606410u){
      fprintf(stderr,"bm_subtract inverse-source blend mismatch: %08x\n",framebuffer); return 1;
    }
  }
  return 0;
}


int expect_renderer_semantics(void){
  return expect_renderer_semantics_exit_code()==0;
}
