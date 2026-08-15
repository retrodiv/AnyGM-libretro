/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static GmlVal read_background_slot_dimension(GmlVM *vm,const char *name,int index){
  unsigned char data[20]={0};
  fixture_word(data,0,(0x84u<<24)|(DT_INT16<<16)|(uint16_t)IT_SELF);
  fixture_word(data,1,(0x84u<<24)|(DT_INT16<<16)|(uint16_t)index);
  fixture_word(data,2,(OP_PUSH<<24)|(DT_VAR<<16));
  fixture_word(data,3,0);
  fixture_word(data,4,(OP_RET<<24)|(DT_VAR<<16));
  uint32_t reference_address=12;
  const char *reference_name=name;
  GmlCode code={0};
  code.name=(char*)"gml_Script_background_dimension_fixture";
  code.length=sizeof data;
  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.bytecode=15;
  win.code=&code;
  win.n_code=1;
  win.ref_addr=&reference_address;
  win.ref_name=&reference_name;
  win.n_refs=1;
  GmlWin *saved_win=vm->win;
  vm->win=&win;
  GmlVal result=gml_vm_run_code(vm,0,NULL,NULL,NULL,0);
  vm->win=saved_win;
  free(code.insn);
  free(code.insn_pc);
  free(code.branch_index);
  free(win.ref_hix);
  return result;
}


int expect_background_slot_dimensions(void){
  GmlBg background={0};
  GmlTpag texture_page={0};
  GmlRender render={0};
  background.tpag=0;
  texture_page.sw=11;
  texture_page.sh=13;
  texture_page.bw=37;
  texture_page.bh=41;
  render.bg=&background;
  render.n_bg=1;
  render.tpag=&texture_page;
  render.n_tpag=1;
  GmlVM vm={0};
  vm.render=&render;
  vm.cur_code_index=-1;
  gml_set_global_arr(&vm,"background_index",2,0);
  gml_set_global_arr(&vm,"background_index",3,-1);
  GmlVal width=read_background_slot_dimension(&vm,"background_width",2);
  GmlVal height=read_background_slot_dimension(&vm,"background_height",2);
  GmlVal missing=read_background_slot_dimension(&vm,"background_width",3);
  int ok=width.t==V_REAL && width.d==37 &&
         height.t==V_REAL && height.d==41 &&
         missing.t==V_REAL && missing.d==0;
  if(!ok)
    fprintf(stderr,"background slot dimensions mismatch: width=%.0f height=%.0f missing=%.0f\n",
            width.t==V_REAL?width.d:-1.0,
            height.t==V_REAL?height.d:-1.0,
            missing.t==V_REAL?missing.d:-1.0);
  gml_varmap_free(&vm.globals);
  return ok;
}


int expect_legacy_sprite_text_builtin(void){
  GmlVM vm={0}; GmlRender render={0}; GmlSprite sprite={0};
  GmlTpag pages[2]={{0}}; GmlAtlas atlas={0};
  uint32_t framebuffer[24*26]={0};
  uint8_t pixels[2*4]={255,255,255,255,255,255,255,255};
  int frames[2]={0,1};
  gml_render_begin(&render,framebuffer,24,26,0,0);
  render.spr=&sprite; render.n_spr=1; render.tpag=pages; render.n_tpag=2;
  render.atlas=&atlas; render.n_atlas=1; render.font=7;
  render.color=0; render.alpha=1; render.alphablend=1; render.software_overlay=1;
  sprite.w=3; sprite.h=2; sprite.n_frames=2; sprite.frame=frames;
  pages[0].sx=0; pages[0].sw=pages[0].sh=pages[0].bw=pages[0].bh=1; pages[0].atlas=0;
  pages[1].sx=1; pages[1].sw=pages[1].sh=pages[1].bw=pages[1].bh=1; pages[1].atlas=0;
  atlas.px=pixels; atlas.w=2; atlas.h=1; atlas.decode_attempted=1;
  vm.render=&render;
  GmlVal args[8]={vreal(1),vreal(1),vstr("  AB  AB    AB\r\nAB"),vreal(5),vreal(18),vreal(0),
                  vreal('A'),vreal(1)};
  (void)gml_builtin_call(&vm,"draw_text_sprite",args,8);
  int ok=(framebuffer[1*24+7]&0xFFFFFFu) && (framebuffer[1*24+10]&0xFFFFFFu) &&
         (framebuffer[6*24+1]&0xFFFFFFu) && (framebuffer[6*24+4]&0xFFFFFFu) &&
         (framebuffer[11*24+1]&0xFFFFFFu) && (framebuffer[11*24+4]&0xFFFFFFu) &&
         (framebuffer[21*24+1]&0xFFFFFFu) && (framebuffer[21*24+4]&0xFFFFFFu) &&
         !(framebuffer[1*24+1]&0xFFFFFFu) && !(framebuffer[16*24+1]&0xFFFFFFu) &&
         !(framebuffer[1*24+8]&0xFFFFFFu) && render.font==7 && render.n_fonts==0;
  if(!ok)
    fprintf(stderr,"legacy sprite text mismatch: first=%08x,%08x second=%08x,%08x third=%08x,%08x crlf=%08x,%08x leading=%08x blank=%08x gap=%08x font=%d count=%d\n",
            framebuffer[1*24+7],framebuffer[1*24+10],framebuffer[6*24+1],
            framebuffer[6*24+4],framebuffer[11*24+1],framebuffer[11*24+4],
            framebuffer[21*24+1],framebuffer[21*24+4],framebuffer[1*24+1],
            framebuffer[16*24+1],framebuffer[1*24+8],render.font,render.n_fonts);
  memset(framebuffer,0,sizeof framebuffer);
  GmlVal spaced[8]={vreal(1),vreal(1),vstr("A    B"),vreal(5),vreal(9),vreal(0),
                    vreal('A'),vreal(1)};
  (void)gml_builtin_call(&vm,"draw_text_sprite",spaced,8);
  int whitespace_wrap=(framebuffer[6*24+1]&0xFFFFFFu) &&
                      !(framebuffer[6*24+7]&0xFFFFFFu);
  if(!whitespace_wrap)
    fprintf(stderr,"legacy sprite text whitespace wrap mismatch: origin=%08x shifted=%08x\n",
            framebuffer[6*24+1],framebuffer[6*24+7]);
  ok=ok&&whitespace_wrap;
  GmlValueFreeContext free_context={0};
  gml_val_free(&free_context,args[2]);
  gml_val_free(&free_context,spaced[2]);
  return ok;
}

int expect_legacy_sprite_assign_builtin(void){
  GmlRender render={0};
  render.spr=calloc(2,sizeof(*render.spr));
  render.tpag=calloc(2,sizeof(*render.tpag));
  render.atlas=calloc(1,sizeof(*render.atlas));
  if(!render.spr || !render.tpag || !render.atlas){
    gml_render_free(&render);
    return 0;
  }
  render.n_spr=render.base_n_spr=render.spr_cap=2;
  render.n_tpag=2;
  render.n_atlas=1;
  render.atlas[0].px=malloc(8);
  if(!render.atlas[0].px){ gml_render_free(&render); return 0; }
  memcpy(render.atlas[0].px,(uint8_t[]){255,0,0,255,0,255,0,255},8);
  render.atlas[0].w=2;
  render.atlas[0].h=1;
  render.atlas[0].decode_attempted=1;
  for(int index=0;index<2;index++){
    render.spr[index].w=render.spr[index].h=1;
    render.spr[index].n_frames=1;
    render.spr[index].frame=malloc(sizeof(int));
    if(!render.spr[index].frame){ gml_render_free(&render); return 0; }
    render.spr[index].frame[0]=index;
    render.tpag[index].sx=index;
    render.tpag[index].sw=render.tpag[index].sh=1;
    render.tpag[index].bw=render.tpag[index].bh=1;
    render.tpag[index].atlas=0;
  }
  render.spr[1].originx=3;
  render.spr[1].originy=4;
  GmlVM vm={0};
  vm.render=&render;
  GmlVal args[2]={vreal(0),vreal(1)};
  GmlVal assigned=gml_builtin_call(&vm,"sprite_assign",args,2);
  uint32_t assigned_color=render.spr[0].runtime_rgba?
    ((uint32_t)render.spr[0].runtime_rgba[0]<<16)|
    ((uint32_t)render.spr[0].runtime_rgba[1]<<8)|
    render.spr[0].runtime_rgba[2]:0;
  int ok=assigned.t==V_REAL && assigned.d==1 &&
         assigned_color==0x00FF00u &&
         render.spr[0].originx==3 && render.spr[0].originy==4;
  if(!ok)
    fprintf(stderr,"legacy sprite assignment mismatch: result=%.0f pixel=%06x origin=%d,%d\n",
            assigned.t==V_REAL?assigned.d:-1.0,assigned_color,
            render.spr[0].originx,render.spr[0].originy);
  gml_render_free(&render);
  return ok;
}

int expect_runtime_sprite_state_preserves_collision_extent(void){
  GmlRender render={0};
  render.alpha=1;
  render.font=-1;
  render.alphablend=1;
  render.circle_precision=24;
  render.app_draw_enable=1;
  render.next_surface_id=1;
  uint8_t *rgba=calloc(7u*5u,4u);
  render.spr=calloc(1,sizeof(*render.spr));
  render.n_spr=render.base_n_spr=render.spr_cap=1;
  int sprite=0;
  if(!rgba || !render.spr ||
     !gml_sprite_replace_from_rgba_frames(&render,sprite,rgba,7,5,1,2,3)){
    free(rgba);
    free(render.spr);
    return 0;
  }
  render.spr[sprite].ml=0;
  render.spr[sprite].mt=0;
  render.spr[sprite].mr=7;
  render.spr[sprite].mb=5;
  render.spr[sprite].collision_kind=1;
  render.spr[sprite].collision_tolerance=58;

  size_t size=gml_render_state_size(&render,0),written=0,used=0,repeated=0;
  uint8_t *before=malloc(size?size:1u);
  uint8_t *after=malloc(size?size:1u);
  int ok=before && after &&
    gml_render_state_save(&render,0,before,size,&written) && written==size &&
    gml_render_state_load(&render,before,written,&used) && used==written &&
    render.spr[sprite].mr==7 && render.spr[sprite].mb==5 &&
    gml_render_state_save(&render,0,after,size,&repeated) && repeated==written &&
    !memcmp(before,after,written);
  if(!ok){
    size_t difference=0;
    while(difference<written && difference<repeated && before && after &&
          before[difference]==after[difference]) difference++;
    fprintf(stderr,"runtime sprite state extent mismatch: right=%d bottom=%d size=%zu/%zu/%zu diff=%zu values=%u/%u\n",
            sprite>=0 && sprite<render.n_spr?render.spr[sprite].mr:-1,
            sprite>=0 && sprite<render.n_spr?render.spr[sprite].mb:-1,
            size,written,repeated,difference,
            difference<written && before?before[difference]:0,
            difference<repeated && after?after[difference]:0);
  }
  free(before);
  free(after);
  gml_render_free(&render);
  return ok;
}



static int expect_renderer_semantics_exit_code(void){
  {
    /* Narrow room-instance records end immediately after rotation. Their transform fields must
     * not be skipped merely because newer records append image playback and override fields. */
    uint8_t record[36]={0};
    fixture_w32(record,20,0x3fc00000u);  /* 1.5f */
    fixture_w32(record,24,0x40200000u);  /* 2.5f */
    fixture_w32(record,28,0xff123456u);
    fixture_w32(record,32,0x42100000u);  /* 36.0f */
    GmlWin record_win={0}; record_win.data=record; record_win.size=sizeof record;
    GmlVM record_vm={0}; record_vm.win=&record_win; record_vm.room_rec_stride=36;
    GmlInstance instance={0};
    instance.image_xscale=instance.image_yscale=1.0;
    instance.image_blend=16777215.0;
    gml_vm_instances_apply_room_transform(&record_vm,&instance,0);
    if(fabs(instance.image_xscale-1.5)>1e-12 ||
       fabs(instance.image_yscale-2.5)>1e-12 ||
       fabs(instance.image_blend-0x123456)>1e-12 ||
       fabs(instance.image_angle-36.0)>1e-12){
      fprintf(stderr,"narrow room instance transform mismatch: scale=(%.2f,%.2f) blend=%.0f angle=%.2f\n",
        instance.image_xscale,instance.image_yscale,instance.image_blend,instance.image_angle);
      return 1;
    }
  }
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
    /* Repeated equivalent sprite-font creation must retain a usable resource at the bounded
     * pool limit. A distinct request still fails instead of aliasing unrelated glyph metrics. */
    GmlSprite sprite={0}; GmlRender render={0};
    sprite.n_frames=1; render.spr=&sprite; render.n_spr=1;
    int id=-1;
    for(int i=0;i<GML_MAX_FONTS;i++) id=gml_font_add_sprite(&render,0,33,1,4);
    int repeated=gml_font_add_sprite(&render,0,33,1,4);
    int distinct=gml_font_add_sprite(&render,0,34,1,4);
    if(id!=GML_MAX_FONTS-1 || repeated!=id || distinct!=-1 ||
       render.n_fonts!=GML_MAX_FONTS){
      fprintf(stderr,"sprite-font saturation mismatch: id=%d repeated=%d distinct=%d count=%d\n",
              id,repeated,distinct,render.n_fonts); return 1;
    }
  }
  {
    GmlSprite sprite={0}; GmlRender render={0}; uint32_t mapping[2]={'A','B'};
    sprite.n_frames=1; render.spr=&sprite; render.n_spr=1;
    render.n_fonts=GML_MAX_FONTS;
    render.fonts[GML_MAX_FONTS-1]=(GmlFont){
      .sprite=0,.prop=1,.sep=2,.map=mapping,.map_len=2
    };
    int repeated=gml_font_add_sprite_ext(&render,0,"AB",1,2);
    int distinct=gml_font_add_sprite_ext(&render,0,"AC",1,2);
    if(repeated!=GML_MAX_FONTS-1 || distinct!=-1){
      fprintf(stderr,"mapped sprite-font saturation mismatch: repeated=%d distinct=%d\n",
              repeated,distinct); return 1;
    }
  }
  {
    /* Sprite fonts retain the source sprite origin. A fixed-cell font also
     * retains the texture-page horizontal offset within the full cell;
     * a proportional font advances by cropped width without adding that
     * horizontal offset again. Both retain vertical offset. A character
     * outside the mapped range remains a blank full cell. */
    GmlRender render={0}; GmlSprite sprite={0}; GmlTpag page={0}; GmlAtlas atlas={0};
    uint32_t framebuffer[16*8]={0}; uint8_t pixel[4]={255,255,255,255}; int frame=0;
    gml_render_begin(&render,framebuffer,16,8,0,0);
    render.spr=&sprite; render.n_spr=1; render.tpag=&page; render.n_tpag=1;
    render.atlas=&atlas; render.n_atlas=1; render.font=0; render.n_fonts=1;
    render.color=0xFFFFFF; render.alpha=1; render.alphablend=1; render.software_overlay=1;
    sprite.w=sprite.h=8; sprite.originx=3; sprite.originy=2;
    sprite.n_frames=1; sprite.frame=&frame;
    page.sw=page.sh=1; page.tx=page.ty=1; page.bw=page.bh=8; page.atlas=0;
    atlas.px=pixel; atlas.w=atlas.h=1; atlas.decode_attempted=1;
    render.fonts[0]=(GmlFont){.sprite=0,.first='A',.prop=1};
    gml_draw_text(&render,4,4,"A A");
    int width=gml_text_width(&render,"A A");
    if(width!=10 || !(framebuffer[3*16+1]&0xFFFFFFu) ||
       !(framebuffer[3*16+10]&0xFFFFFFu) || (framebuffer[4*16+4]&0xFFFFFFu)){
      fprintf(stderr,"sprite-font origin or blank-cell mismatch: width=%d pixels=%08x,%08x,%08x\n",
              width,framebuffer[3*16+2],framebuffer[3*16+11],framebuffer[4*16+4]);
      return 1;
    }
  }
  {
    /* Compact FONT records do not serialize a separate line advance. When their integer em size
     * is smaller than the packed glyph cell, multiline layout advances by the complete cell. */
    uint8_t data[160]={0}; GmlWin win={0}; GmlRender render={0};
    const uint32_t font_record=32,texture_record=96,glyph_record=128;
    fixture_w32(data,0,1);
    fixture_w32(data,4,font_record);
    fixture_w32(data,font_record+8,6);
    fixture_w32(data,font_record+28,texture_record);
    fixture_w32(data,font_record+40,1);
    fixture_w32(data,font_record+44,glyph_record);
    fixture_w32(data,glyph_record+0,'A');
    fixture_w32(data,glyph_record+4,1u<<16);
    fixture_w32(data,glyph_record+8,8u|(1u<<16));
    win.data=data; win.size=sizeof data; win.bytecode=16; win.n_chunks=1;
    memcpy(win.chunks[0].name,"FONT",4);
    win.chunks[0].off=0; win.chunks[0].size=sizeof data;
    render.win=&win;
    parse_font(&render);
    if(render.n_fonts!=1 || render.fonts[0].n_glyphs!=1 ||
       render.fonts[0].line_height!=8 || render.fonts[0].align_height!=8){
      fprintf(stderr,"compact font line advance mismatch: fonts=%d glyphs=%d line=%d align=%d\n",
              render.n_fonts,render.fonts[0].n_glyphs,render.fonts[0].line_height,
              render.fonts[0].align_height);
      gml_render_free(&render); return 1;
    }
    gml_render_free(&render);
  }
  {
    /* GameMaker's right alignment treats the text origin as the inclusive rightmost pixel of a
     * sprite-font line. A one-pixel glyph drawn at x=4 therefore covers x=4, not x=3. */
    GmlRender render={0}; GmlSprite sprite={0}; GmlTpag page={0}; GmlAtlas atlas={0};
    uint32_t framebuffer[8]={0}; uint8_t pixel[4]={255,255,255,255}; int frame=0;
    gml_render_begin(&render,framebuffer,8,1,0,0);
    render.spr=&sprite; render.n_spr=1; render.tpag=&page; render.n_tpag=1;
    render.atlas=&atlas; render.n_atlas=1; render.font=0; render.n_fonts=1;
    render.color=0xFFFFFF; render.alpha=1; render.alphablend=1; render.software_overlay=1;
    render.halign=2;
    sprite.w=sprite.h=1; sprite.n_frames=1; sprite.frame=&frame;
    page.sw=page.sh=page.bw=page.bh=1; page.atlas=0;
    atlas.px=pixel; atlas.w=atlas.h=1; atlas.decode_attempted=1;
    render.fonts[0]=(GmlFont){.sprite=0,.first='A',.prop=0};
    gml_draw_text(&render,4,0,"A");
    if(!(framebuffer[4]&0xFFFFFFu) || (framebuffer[3]&0xFFFFFFu)){
      fprintf(stderr,"sprite-font right alignment did not include its origin pixel\n"); return 1;
    }
  }
  {
    /* Modern FONT records expose ascender and distance-field metadata alongside the authored
     * line advance. A zero spread remains meaningful because font_get_info must still publish it. */
    uint8_t data[192]={0}; GmlWin win={0}; GmlRender render={0};
    const uint32_t font_record=32,texture_record=120,glyph_record=160;
    fixture_w32(data,0,1);
    fixture_w32(data,4,font_record);
    fixture_w32(data,font_record+8,12);
    fixture_w32(data,font_record+28,texture_record);
    fixture_w32(data,font_record+40,2);
    fixture_w32(data,font_record+44,10);
    fixture_w32(data,font_record+48,8);
    fixture_w32(data,font_record+52,12);
    fixture_w32(data,font_record+56,1);
    fixture_w32(data,font_record+60,glyph_record);
    fixture_w32(data,glyph_record+0,'A');
    fixture_w32(data,glyph_record+4,1u<<16);
    fixture_w32(data,glyph_record+8,8u|(1u<<16));
    win.data=data; win.size=sizeof data; win.bytecode=17; win.n_chunks=1;
    memcpy(win.chunks[0].name,"FONT",4);
    win.chunks[0].off=0; win.chunks[0].size=sizeof data;
    render.win=&win;
    parse_font(&render);
    if(render.n_fonts!=1 || render.fonts[0].n_glyphs!=1 ||
       render.fonts[0].line_height!=12 || render.fonts[0].ascender!=10 ||
       render.fonts[0].ascender_offset!=2 || render.fonts[0].sdf_spread!=8){
      fprintf(stderr,"modern font metadata mismatch: fonts=%d glyphs=%d line=%d ascender=%d offset=%d spread=%d\n",
              render.n_fonts,render.fonts[0].n_glyphs,render.fonts[0].line_height,
              render.fonts[0].ascender,render.fonts[0].ascender_offset,
              render.fonts[0].sdf_spread);
      gml_render_free(&render); return 1;
    }
    gml_render_free(&render);
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
    /* First-generation point and line primitives use pixel-centre quantisation after the logical
     * view has been scaled. Later generations retain the ordinary floor mapping. */
    GmlVM draw_vm={0}; GmlRender render={0}; GmlWin win={0};
    uint32_t framebuffer[20*20];
    for(size_t i=0;i<sizeof framebuffer/sizeof *framebuffer;i++)
      framebuffer[i]=UINT32_C(0xFF000000);
    gml_render_begin(&render,framebuffer,20,20,0,0);
    gml_render_world_set_logical_extent(&render,10,10);
    render.color=0xFFFFFF; render.alpha=1; render.alphablend=1;
    draw_vm.render=&render; draw_vm.win=&win; win.bytecode=16;
    GmlVal point[3]={vreal(3),vreal(4),vreal(0x0000FF)};
    (void)gml_builtin_call(&draw_vm,"draw_point_colour",point,3);
    if(framebuffer[9*20+7]!=UINT32_C(0xFFFF0000) ||
       framebuffer[8*20+6]!=UINT32_C(0xFF000000)){
      fprintf(stderr,"first-generation primitive quantisation mismatch: new=%08x old=%08x\n",
              framebuffer[9*20+7],framebuffer[8*20+6]); return 1;
    }
    for(size_t i=0;i<sizeof framebuffer/sizeof *framebuffer;i++)
      framebuffer[i]=UINT32_C(0xFF000000);
    win.bytecode=17;
    (void)gml_builtin_call(&draw_vm,"draw_point_colour",point,3);
    if(framebuffer[8*20+6]!=UINT32_C(0xFFFF0000) ||
       framebuffer[9*20+7]!=UINT32_C(0xFF000000)){
      fprintf(stderr,"later Studio primitive quantisation changed: floor=%08x shifted=%08x\n",
              framebuffer[8*20+6],framebuffer[9*20+7]); return 1;
    }
    for(size_t i=0;i<sizeof framebuffer/sizeof *framebuffer;i++)
      framebuffer[i]=UINT32_C(0xFF000000);
    win.bytecode=16;
    GmlVal subpixel_line[6]={vreal(1.25),vreal(2.25),vreal(3.25),vreal(2.25),
                             vreal(0x0000FF),vreal(0xFF0000)};
    (void)gml_builtin_call(&draw_vm,"draw_line_colour",subpixel_line,6);
    if(framebuffer[6*20+4]!=UINT32_C(0xFFFF0000) ||
       framebuffer[6*20+7]==UINT32_C(0xFF000000) ||
       framebuffer[6*20+8]!=UINT32_C(0xFF000000) ||
       framebuffer[5*20+4]!=UINT32_C(0xFF000000)){
      fprintf(stderr,
              "first-generation subpixel line mismatch: first=%08x body=%08x exit=%08x adjacent=%08x\n",
              framebuffer[6*20+4],framebuffer[6*20+7],
              framebuffer[6*20+8],framebuffer[5*20+4]); return 1;
    }
    for(size_t i=0;i<sizeof framebuffer/sizeof *framebuffer;i++)
      framebuffer[i]=UINT32_C(0xFF000000);
    GmlVal clipped_rectangle[5]={vreal(2),vreal(3),vreal(7),vreal(12),vreal(1)};
    (void)gml_builtin_call(&draw_vm,"draw_rectangle",clipped_rectangle,5);
    if(framebuffer[7*20+5]!=UINT32_C(0xFFFFFFFF) ||
       framebuffer[7*20+15]!=UINT32_C(0xFFFFFFFF) ||
       framebuffer[19*20+5]!=UINT32_C(0xFFFFFFFF) ||
       framebuffer[19*20+15]!=UINT32_C(0xFFFFFFFF) ||
       framebuffer[19*20+10]!=UINT32_C(0xFF000000) ||
       framebuffer[6*20+5]!=UINT32_C(0xFF000000)){
      fprintf(stderr,
              "first-generation clipped rectangle mismatch: top=%08x,%08x sides=%08x,%08x clipped=%08x prior=%08x\n",
              framebuffer[7*20+5],framebuffer[7*20+15],
              framebuffer[19*20+5],framebuffer[19*20+15],
              framebuffer[19*20+10],framebuffer[6*20+5]); return 1;
    }
  }
  {
    /* Line colour builtins interpolate both endpoint colours; the second colour must survive
     * language dispatch instead of collapsing the primitive to a flat first-colour line. */
    GmlVM draw_vm={0}; GmlRender render={0}; GmlWin win={0};
    uint32_t framebuffer[8*8];
    for(size_t i=0;i<sizeof framebuffer/sizeof *framebuffer;i++)
      framebuffer[i]=UINT32_C(0xFF000000);
    gml_render_begin(&render,framebuffer,8,8,0,0);
    render.alpha=1; render.alphablend=1;
    draw_vm.render=&render; draw_vm.win=&win; win.bytecode=17;
    GmlVal line[6]={vreal(1),vreal(3),vreal(5),vreal(3),
                    vreal(0x0000FF),vreal(0xFF0000)};
    (void)gml_builtin_call(&draw_vm,"draw_line_colour",line,6);
    uint32_t first=framebuffer[3*8+1],middle=framebuffer[3*8+3],last=framebuffer[3*8+5];
    if(first!=UINT32_C(0xFFFF0000) || last!=UINT32_C(0xFF0000FF) ||
       middle==first || middle==last){
      fprintf(stderr,"line endpoint gradient mismatch: first=%08x middle=%08x last=%08x\n",
              first,middle,last); return 1;
    }
  }
  {
    /* The basic maximum preset uses source alpha and inverse source colour factors.  It is not
     * normal alpha blending and must remain distinct from the independent maximum equation. */
    GmlVM draw_vm={0}; GmlRender render={0}; uint32_t framebuffer=0xFF204080u;
    gml_render_begin(&render,&framebuffer,1,1,0,0);
    render.alpha=0.5; render.alphablend=1; render.color_write_mask=0x0F;
    draw_vm.render=&render;
    GmlVal mode=vreal(2),point[3]={vreal(0),vreal(0),vreal(0xC08040)};
    (void)gml_builtin_call(&draw_vm,"gpu_set_blendmode",&mode,1);
    (void)gml_builtin_call(&draw_vm,"draw_point_colour",point,3);
    GmlVal active=gml_builtin_call(&draw_vm,"gpu_get_blendmode",NULL,0);
    if(framebuffer!=0xFF386080u || active.t!=V_REAL || active.d!=2.0){
      fprintf(stderr,"bm_max preset mismatch: pixel=%08x mode=%.0f\n",framebuffer,active.d);
      return 1;
    }
    framebuffer=0xFF204080u;
    GmlVal factors[2]={vreal(5),vreal(4)};
    (void)gml_builtin_call(&draw_vm,"gpu_set_blendmode_ext",factors,2);
    (void)gml_builtin_call(&draw_vm,"draw_point_colour",point,3);
    active=gml_builtin_call(&draw_vm,"gpu_get_blendmode",NULL,0);
    if(framebuffer!=0xFF386080u || active.t!=V_REAL || active.d!=2.0){
      fprintf(stderr,"bm_max extended factors mismatch: pixel=%08x mode=%.0f\n",
              framebuffer,active.d);
      return 1;
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
    GmlVal active=gml_builtin_call(&draw_vm,"gpu_get_blendmode",NULL,0);
    if(framebuffer!=0xFF606410u || active.t!=V_REAL || active.d!=3.0){
      fprintf(stderr,"bm_subtract inverse-source blend mismatch: pixel=%08x mode=%.0f\n",
              framebuffer,active.d); return 1;
    }
  }
  {
    /* Fixed-function colour factors still run for a sampled texel whose alpha is zero. The alpha
     * controls coverage only; bm_subtract's RGB factors are zero and inverse-source-colour. */
    GmlRender render={0}; GmlSprite sprite={0}; GmlTpag page={0}; GmlAtlas atlas={0};
    uint32_t framebuffer=0xFF80C840u; uint8_t pixel[4]={64,128,192,0}; int frame=0;
    gml_render_begin(&render,&framebuffer,1,1,0,0);
    render.spr=&sprite; render.n_spr=1; render.tpag=&page; render.n_tpag=1;
    render.atlas=&atlas; render.n_atlas=1; render.alpha=1; render.alphablend=1;
    render.color_write_mask=0x0F; render.blendmode=2;
    sprite.w=sprite.h=1; sprite.n_frames=1; sprite.frame=&frame;
    page.sw=page.sh=page.bw=page.bh=1; page.atlas=0;
    atlas.px=pixel; atlas.w=atlas.h=1; atlas.decode_attempted=1;
    for(int interpolation=0;interpolation<=1;interpolation++){
      framebuffer=0xFF80C840u; render.interp=interpolation;
      gml_draw_sprite(&render,0,0,0,0);
      if(framebuffer!=0xFF606410u){
        fprintf(stderr,"transparent bm_subtract texture did not apply RGB factors "
                "with interpolation %d: pixel=%08x\n",interpolation,framebuffer); return 1;
      }
    }
  }
  {
    /* The application surface is sampled later by Draw GUI, so its alpha is real render-target
     * state even though the host binds it without surface_set_target().  A translucent normal mask
     * followed by bm_subtract must leave both coverage operations for that later composite. */
    GmlRender render={0}; GmlSprite sprite={0}; GmlTpag page={0}; GmlAtlas atlas={0};
    GmlWin win={0};
    uint32_t application=0xFF009DAEu, screen=0; uint8_t black[4]={0,0,0,255}; int frame=0;
    gml_render_begin(&render,&application,1,1,0,0);
    render.win=&win; win.bytecode=14;
    gml_render_application_surface_bind(&render,&application,1,1,1);
    render.spr=&sprite; render.n_spr=1; render.tpag=&page; render.n_tpag=1;
    render.atlas=&atlas; render.n_atlas=1; render.alpha=1; render.alphablend=1;
    render.color_write_mask=0x0F;
    sprite.w=sprite.h=1; sprite.n_frames=1; sprite.frame=&frame;
    page.sw=page.sh=page.bw=page.bh=1; page.atlas=0;
    atlas.px=black; atlas.w=atlas.h=1; atlas.decode_attempted=1;
    gml_draw_sprite_ext(&render,0,0,0,0,1,1,0,0xFFFFFF,0.04);
    render.blendmode=2;
    gml_draw_sprite_ext(&render,0,0,0,0,1,1,0,0xFFFFFF,0.10);
    if(application!=0xDD0097A7u){
      fprintf(stderr,"application-surface alpha blend mismatch: pixel=%08x\n",application);
      return 1;
    }
    gml_render_begin(&render,&screen,1,1,0,0);
    gml_render_application_surface_bind(&render,&application,1,1,0);
    render.alphablend=1; render.blendmode=0;
    gml_draw_surface_stretched(&render,0,0,0,1,1,0xFFFFFF,1.0);
    if(screen!=0xFF008391u){
      fprintf(stderr,"application-surface composite ignored retained alpha: pixel=%08x\n",screen);
      return 1;
    }
  }
  return 0;
}


int expect_renderer_semantics(void){
  return expect_renderer_semantics_exit_code()==0;
}
