/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_primitives.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"renderer surfaces failed: %s\n",label); \
    return 1; \
  } \
}while(0)

static uint64_t pixel_hash_update(uint64_t hash,const uint32_t *pixels,size_t count){
  for(size_t index=0;index<count;index++){
    uint32_t value=pixels[index];
    for(unsigned shift=0;shift<32;shift+=8){
      hash^=(value>>shift)&0xFFu;
      hash*=UINT64_C(1099511628211);
    }
  }
  return hash;
}

static uint64_t pixel_hash(const uint32_t *pixels,size_t count){
  return pixel_hash_update(UINT64_C(1469598103934665603),pixels,count);
}

static int presentation_latch_coverage_case(void){
  enum { WIDTH=4,HEIGHT=3 };
  GmlRender render;
  uint32_t frame[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  memset(frame,0,sizeof frame);
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  int source=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(source==1 && surface_known_transparent(&render,source),
          "presentation transparent source");
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(!gml_render_content_composited_screen(&render),
          "transparent scratch surface does not latch presentation");

  REQUIRE(gml_surface_set_target(&render,source),
          "presentation authored transparent source target");
  gml_render_primitive_rectangle(&render,0,0,0,0,0xFFFFFFu,0);
  gml_render_set_pending_fill(&render,0);
  gml_surface_reset_target(&render);
  REQUIRE(surface_known_transparent(&render,source),
          "presentation authored source resolves transparent");
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(gml_render_content_composited_screen(&render),
          "authored transparent surface latches presentation");

  gml_render_clear_content_composited_screen(&render);
  REQUIRE(gml_surface_set_target(&render,source),
          "presentation opaque source target");
  gml_render_clear(&render,0x000000u,1.0);
  gml_surface_reset_target(&render);
  REQUIRE(surface_known_opaque(&render,source),
          "presentation opaque source coverage");
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(gml_render_content_composited_screen(&render),
          "opaque black surface latches presentation");

  gml_render_clear_content_composited_screen(&render);
  REQUIRE(!gml_render_content_composited_screen(&render),
          "presentation latch clears between frames");
  gml_surface_free(&render,source);
  return 0;
}

static int composition_cases(void){
  GmlRender render;
  uint32_t frame[12*10];
  memset(&render,0,sizeof render);
  for(int y=0;y<10;y++) for(int x=0;x<12;x++){
    unsigned red=(unsigned)(9+x*11+y*3);
    unsigned green=(unsigned)(21+x*5+y*13);
    unsigned blue=(unsigned)(33+x*7+y*17);
    frame[(size_t)y*12+x]=0xFF000000u|(red<<16)|(green<<8)|blue;
  }
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=12;
  render.fbh=render.base_fbh=10;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  render.fb_opaque_known=1;
  render.fb_all_opaque=1;

  int source=gml_surface_create(&render,4,3);
  int target=gml_surface_create(&render,7,6);
  REQUIRE(source==1 && target==2,"composition surface ids");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"composition source pixels");
  static const uint32_t authored[12]={
    0xFF102030u,0xC0503020u,0x804080C0u,0x00000000u,
    0xFF90A020u,0x4010D080u,0xFFA060E0u,0xC0F02070u,
    0x00203040u,0xFF4060A0u,0x8080C040u,0xFFFFFFFFu
  };
  memcpy(source_pixels,authored,sizeof authored);
  render.surface[0].opaque_known=0;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;

  gml_draw_surface_stretched(&render,source,1.25,1.0,7.0,5.0,0xD0A080u,0.75);
  render.interp=1;
  gml_draw_surface_stretched(&render,source,3.0,0.0,8.0,7.0,0xFFFFFFu,0.6);

  struct GmlShaderPal palette;
  memset(&palette,0,sizeof palette);
  palette.has=1;
  palette.alpha_discard=1;
  palette.alpha_discard_inclusive=1;
  palette.alpha_discard_cutoff=0.25f;
  palette.L[0]=240; palette.L[1]=224; palette.L[2]=208;
  palette.M[0]=176; palette.M[1]=48;  palette.M[2]=32;
  palette.D[0]=12;  palette.D[1]=20;  palette.D[2]=28;
  palette.S[0]=40;  palette.S[1]=96;  palette.S[2]=208;
  render.shader_pal=&palette;
  render.n_shader_pal=1;
  render.active_shader=0;
  gml_draw_surface_part_ext(&render,source,0.5,0.0,3.0,3.0,
                            0.5,5.0,1.8,1.2,0xFFFFFFu,0.85);
  render.active_shader=-1;
  render.shader_pal=NULL;
  render.n_shader_pal=0;

  render.interp=0;
  gml_draw_surface_ext(&render,source,8.0,4.0,1.15,0.9,33.0,0x80C0FFu,0.7);
  render.blendmode=3;
  gml_draw_surface_ext(&render,source,6.0,2.0,1.0,1.0,90.0,0xC08060u,0.8);
  render.blendmode=0;
  render.color_write_mask=0x05;
  gml_draw_surface_part_ext(&render,source,1.0,0.0,3.0,2.0,
                            7.0,7.0,1.0,1.0,0xE0B060u,1.0);
  render.color_write_mask=0x0F;

  REQUIRE(gml_surface_set_target(&render,target),"composition set target");
  render.interp=1;
  gml_draw_surface_stretched(&render,source,-0.5,0.25,8.0,6.5,0xFFFFFFu,0.9);
  gml_surface_reset_target(&render);
  int target_width=0,target_height=0;
  const uint32_t *target_pixels=
    gml_surface_pixels_read(&render,target,&target_width,&target_height);
  REQUIRE(target_pixels!=NULL && target_width==7 && target_height==6,
          "composition target pixels");

  uint64_t hash=pixel_hash(frame,sizeof frame/sizeof frame[0]);
  hash=pixel_hash_update(hash,target_pixels,(size_t)target_width*target_height);
  if(hash!=UINT64_C(0x1006bcd38a65037c)){
    fprintf(stderr,"renderer surface composition hash: %016llx\n",
            (unsigned long long)hash);
    gml_surface_free(&render,source);
    gml_surface_free(&render,target);
    return 1;
  }
  gml_surface_free(&render,source);
  gml_surface_free(&render,target);
  return 0;
}

static int screen_raster_part_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET_WIDTH=8,TARGET_HEIGHT=6 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  GmlRender render;
  memset(&render,0,sizeof render);
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(31+x*41+y*17)<<16)|
      ((uint32_t)(23+x*13+y*47)<<8)|(uint32_t)(11+x*29+y*19);
  memset(target,0x5A,sizeof target);
  render.app_surface=source;
  render.app_w=SOURCE_WIDTH;
  render.app_h=SOURCE_HEIGHT;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,SOURCE_WIDTH,SOURCE_HEIGHT);
  gml_draw_surface_part_ext(&render,0,0.0,0.0,SOURCE_WIDTH,SOURCE_HEIGHT,
                            0.0,0.0,2.0,2.0,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    uint32_t expected=source[(size_t)(y/2)*SOURCE_WIDTH+x/2];
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,"renderer screen raster part mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      return 1;
    }
  }
  return 0;
}

static int first_generation_point_sampling_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET_WIDTH=13,TARGET_HEIGHT=10 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  GmlRender render;
  GmlWin content={0};
  memset(&render,0,sizeof render);
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(31+x*41+y*17)<<16)|
      ((uint32_t)(23+x*13+y*47)<<8)|(uint32_t)(11+x*29+y*19);
  memset(target,0,sizeof target);
  content.bytecode=14;
  render.win=&content;
  render.app_surface=source;
  render.app_w=SOURCE_WIDTH;
  render.app_h=SOURCE_HEIGHT;
  render.app_surface_opaque=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=0;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_draw_surface_stretched(&render,0,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    int source_x=(int)(((int64_t)(2*x+1)*SOURCE_WIDTH)/(2*TARGET_WIDTH));
    int source_y=(int)(((int64_t)(2*y+1)*SOURCE_HEIGHT)/(2*TARGET_HEIGHT));
    uint32_t expected=source[(size_t)source_y*SOURCE_WIDTH+source_x];
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,"renderer first-generation point sample mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      return 1;
    }
  }
  return 0;
}

static int first_generation_high_magnification_case(void){
  enum { SOURCE_WIDTH=5,SOURCE_HEIGHT=3,TARGET_WIDTH=1103,TARGET_HEIGHT=19 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  GmlRender render;
  GmlWin content={0};
  memset(&render,0,sizeof render);
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(17+x*29+y*31)<<16)|
      ((uint32_t)(23+x*37+y*19)<<8)|(uint32_t)(31+x*13+y*47);
  memset(target,0xA5,sizeof target);
  content.bytecode=14;
  render.win=&content;
  render.app_surface=source;
  render.app_w=SOURCE_WIDTH;
  render.app_h=SOURCE_HEIGHT;
  render.app_surface_opaque=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=0;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_draw_surface_stretched(&render,0,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    int source_x=(int)(((int64_t)(2*x+1)*SOURCE_WIDTH)/(2*TARGET_WIDTH));
    int source_y=(int)(((int64_t)(2*y+1)*SOURCE_HEIGHT)/(2*TARGET_HEIGHT));
    uint32_t expected=source[(size_t)source_y*SOURCE_WIDTH+source_x];
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,
              "renderer first-generation high magnification mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      return 1;
    }
  }
  return 0;
}

static int opaque_integer_scale_case(void){
  enum { SOURCE_WIDTH=5,SOURCE_HEIGHT=3,TARGET_WIDTH=20,TARGET_HEIGHT=12 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t opaque_target[TARGET_WIDTH*TARGET_HEIGHT];
  uint32_t unknown_target[TARGET_WIDTH*TARGET_HEIGHT];
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(17+x*31+y*7)<<16)|
      ((uint32_t)(29+x*11+y*37)<<8)|(uint32_t)(41+x*23+y*13);

  for(int pass=0;pass<2;pass++){
    GmlRender render;
    uint32_t *target=pass?unknown_target:opaque_target;
    memset(&render,0,sizeof render);
    for(size_t index=0;index<TARGET_WIDTH*TARGET_HEIGHT;index++)
      target[index]=0xFF102030u+(uint32_t)index;
    render.app_surface=source;
    render.app_w=SOURCE_WIDTH;
    render.app_h=SOURCE_HEIGHT;
    render.app_surface_opaque=pass?0:1;
    render.alphablend=1;
    render.color_write_mask=0x0F;
    render.blend_equation=1;
    render.blend_equation_alpha=1;
    render.app_draw_enable=1;
    render.active_shader=-1;
    render.lut_pal_sprite=-1;
    gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
    gml_render_set_pending_underlay(&render,0,0,TARGET_WIDTH,TARGET_HEIGHT);
    gml_render_flush_pending_underlay(&render);
    for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
      uint32_t expected=source[(size_t)(y/(TARGET_HEIGHT/SOURCE_HEIGHT))*SOURCE_WIDTH+
                               x/(TARGET_WIDTH/SOURCE_WIDTH)];
      if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
        fprintf(stderr,"renderer opaque integer scale mismatch at %d,%d: %08x != %08x\n",
                x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
        return 1;
      }
    }
  }
  REQUIRE(!memcmp(opaque_target,unknown_target,sizeof opaque_target),
          "opaque integer scale equivalence");
  return 0;
}

static int point_downscale_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=4,TARGET_WIDTH=2,TARGET_HEIGHT=2 };
  GmlRender render;
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  memset(&render,0,sizeof render);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,SOURCE_WIDTH,SOURCE_HEIGHT);
  REQUIRE(source==1,"point downscale surface id");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"point downscale source pixels");
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++){
    uint32_t value=(uint32_t)(16+y*40+x*8);
    source_pixels[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|(value<<16)|(value<<8)|value;
  }
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=1;
  render.surface[0].all_transparent=0;

  memset(target,0,sizeof target);
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_draw_surface_stretched(&render,source,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  static const int point_source[4][2]={{0,0},{2,0},{0,2},{2,2}};
  for(int index=0;index<4;index++){
    int x=point_source[index][0],y=point_source[index][1];
    uint32_t expected=source_pixels[(size_t)y*SOURCE_WIDTH+x];
    if(target[index]!=expected){
      fprintf(stderr,"renderer point downscale mismatch at %d: %08x != %08x\n",
              index,target[index],expected);
      gml_surface_free(&render,source);
      return 1;
    }
  }

  memset(target,0,sizeof target);
  render.interp=1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_draw_surface_stretched(&render,source,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  static const uint32_t filtered[4]={
    0xFF282828u,0xFF383838u,0xFF787878u,0xFF888888u
  };
  REQUIRE(!memcmp(target,filtered,sizeof filtered),"filtered downscale average");
  gml_surface_free(&render,source);
  return 0;
}

static int opaque_near_identity_reduction_case(void){
  enum { SOURCE_WIDTH=5,SOURCE_HEIGHT=4,TARGET_WIDTH=4,TARGET_HEIGHT=3 };
  GmlRender render;
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  memset(&render,0,sizeof render);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,SOURCE_WIDTH,SOURCE_HEIGHT);
  REQUIRE(source==1,"near-identity reduction surface id");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"near-identity reduction source pixels");
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++){
    uint32_t value=(uint32_t)(13+y*47+x*9);
    source_pixels[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|(value<<16)|(value<<8)|value;
  }
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=1;
  render.surface[0].all_transparent=0;

  for(int pass=0;pass<3;pass++){
    int known=pass!=0;
    render.surface[0].opaque_known=known;
    render.surface[0].all_opaque=known;
    memset(target,0x4C,sizeof target);
    gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
    render.alpha_test_enable=pass==2;
    render.alpha_test_ref=254;
    gml_draw_surface_stretched(&render,source,0.0,0.0,
                               TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
    for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
      int source_x=(int)(((int64_t)x*SOURCE_WIDTH)/TARGET_WIDTH);
      int source_y=(int)(((int64_t)y*SOURCE_HEIGHT)/TARGET_HEIGHT);
      uint32_t expected=source_pixels[(size_t)source_y*SOURCE_WIDTH+source_x];
      if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
        fprintf(stderr,"renderer near-identity reduction mismatch at %d,%d: %08x != %08x\n",
                x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
        gml_surface_free(&render,source);
        return 1;
      }
    }
    REQUIRE(render.fb_opaque_known && render.fb_all_opaque,
            "near-identity reduction opaque coverage");
  }
  gml_surface_free(&render,source);
  return 0;
}

static int zero_reference_alpha_test_case(void){
  enum { WIDTH=4,HEIGHT=3 };
  GmlRender render;
  uint32_t baseline[WIDTH*HEIGHT],zero_test[WIDTH*HEIGHT],threshold_test[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(source==1,"zero-reference alpha-test surface id");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"zero-reference alpha-test source pixels");
  for(int x=0;x<WIDTH;x++){
    source_pixels[x]=0x00102030u+(uint32_t)x;
    source_pixels[WIDTH+x]=0x80406080u+(uint32_t)x;
    source_pixels[WIDTH*2+x]=0xFFA0C0E0u+(uint32_t)x;
  }
  render.surface[0].opaque_known=0;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;

  for(size_t index=0;index<WIDTH*HEIGHT;index++)
    baseline[index]=zero_test[index]=threshold_test[index]=0xFF182838u;
  gml_render_begin(&render,baseline,WIDTH,HEIGHT,0.0,0.0);
  render.alpha_test_enable=0;
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  gml_render_begin(&render,zero_test,WIDTH,HEIGHT,0.0,0.0);
  render.alpha_test_enable=1;
  render.alpha_test_ref=0;
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(!memcmp(baseline,zero_test,sizeof baseline),
          "zero-reference alpha-test equivalence");

  gml_render_begin(&render,threshold_test,WIDTH,HEIGHT,0.0,0.0);
  render.alpha_test_enable=1;
  render.alpha_test_ref=128;
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  for(int x=0;x<WIDTH;x++){
    REQUIRE(threshold_test[x]==0xFF182838u,
            "transparent row remains below alpha threshold");
    REQUIRE(threshold_test[WIDTH+x]==0xFF182838u,
            "threshold row is discarded");
    REQUIRE(threshold_test[WIDTH*2+x]==source_pixels[WIDTH*2+x],
            "opaque row survives alpha threshold");
  }
  gml_surface_free(&render,source);
  return 0;
}

static int max_preset_surface_case(void){
  GmlRender render;
  uint32_t target=0xFF204080u;
  memset(&render,0,sizeof render);
  render.fb=render.base_fb=&target;
  render.fbw=render.base_fbw=1;
  render.fbh=render.base_fbh=1;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,1,1);
  REQUIRE(source==1,"maximum preset surface id");
  uint32_t *source_pixel=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixel!=NULL,"maximum preset surface pixel");
  *source_pixel=0x804080C0u;
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;
  render.blendmode=4;
  gml_draw_surface_stretched(&render,source,0.0,0.0,1.0,1.0,0xFFFFFFu,0.5);
  if(target!=0xFF284050u){
    fprintf(stderr,"renderer maximum preset surface mismatch: %08x\n",target);
    gml_surface_free(&render,source);
    return 1;
  }
  gml_surface_free(&render,source);
  return 0;
}

static int subtract_surface_coverage_case(void){
  enum { WIDTH=5,HEIGHT=5 };
  GmlRender render;
  uint32_t frame[WIDTH*HEIGHT],background[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  for(size_t index=0;index<WIDTH*HEIGHT;index++)
    frame[index]=background[index]=0xFF204060u+(uint32_t)index;
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  int mask=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(mask==1,"subtract mask surface id");
  REQUIRE(gml_surface_set_target(&render,mask),"subtract mask set target");
  gml_render_primitive_rectangle(&render,0,0,WIDTH-1,HEIGHT-1,0x40A020u,0);
  render.blendmode=2;
  gml_render_primitive_circle(&render,WIDTH/2,HEIGHT/2,2,2,0xFFFFFFu,0);
  render.blendmode=0;
  gml_surface_reset_target(&render);

  int mask_width=0,mask_height=0;
  const uint32_t *mask_pixels=
    gml_surface_pixels_read(&render,mask,&mask_width,&mask_height);
  REQUIRE(mask_pixels && mask_width==WIDTH && mask_height==HEIGHT,
          "subtract mask pixels");
  int holes=0,covered=0;
  for(size_t index=0;index<WIDTH*HEIGHT;index++){
    if(!(mask_pixels[index]>>24)) holes++;
    else if((mask_pixels[index]>>24)==255) covered++;
  }
  REQUIRE(holes>0 && covered>0,"subtract mask mixed coverage");
  REQUIRE(!surface_known_opaque(&render,mask),"subtract mask not opaque");

  gml_draw_surface_stretched(&render,mask,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  for(size_t index=0;index<WIDTH*HEIGHT;index++){
    if(!(mask_pixels[index]>>24) && frame[index]!=background[index]){
      fprintf(stderr,"renderer subtract mask hole mismatch at %zu: %08x != %08x\n",
              index,frame[index],background[index]);
      gml_surface_free(&render,mask);
      return 1;
    }
  }
  gml_surface_free(&render,mask);
  return 0;
}

static int masked_sprite_surface_case(void){
  enum { WIDTH=4,HEIGHT=3 };
  GmlRender render;
  uint32_t frame[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  for(size_t index=0;index<WIDTH*HEIGHT;index++) frame[index]=0xFF000000u;
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  render.n_spr=1;
  render.spr=calloc(1,sizeof(*render.spr));
  REQUIRE(render.spr!=NULL,"masked sprite allocation");
  render.spr[0].w=render.spr[0].h=1;
  render.spr[0].n_frames=1;
  render.spr[0].runtime_owned=1;
  render.spr[0].runtime_opaque=1;
  render.spr[0].runtime_rgba=malloc(4);
  REQUIRE(render.spr[0].runtime_rgba!=NULL,"masked sprite pixels");
  render.spr[0].runtime_rgba[0]=0;
  render.spr[0].runtime_rgba[1]=0;
  render.spr[0].runtime_rgba[2]=255;
  render.spr[0].runtime_rgba[3]=255;

  int surface=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(surface==1,"masked sprite surface id");
  REQUIRE(gml_surface_set_target(&render,surface),"masked sprite set target");
  gml_render_clear(&render,0xFFFFFFu,0.25);
  render.color_write_mask=0x08;
  gml_draw_sprite_ext(&render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  const uint32_t *surface_pixels_now=surface_pixels(&render,surface,NULL,NULL);
  REQUIRE(surface_pixels_now[0]==0x40FFFFFFu,
          "alpha-only sprite preserves surface rgb");
  REQUIRE(surface_pixels_now[WIDTH+1]==0xFFFFFFFFu,
          "alpha-only sprite updates selected alpha");
  render.color_write_mask=0x07;
  gml_draw_sprite_tiled_ext(&render,0,0,0.0,0.0,1.0,1.0,0xFFFFFFu,1.0);
  REQUIRE(surface_pixels_now[0]==0x400000FFu,
          "rgb-only tiled sprite preserves surface alpha");
  REQUIRE(surface_pixels_now[WIDTH+1]==0xFF0000FFu,
          "rgb-only tiled sprite preserves opaque mask alpha");
  render.color_write_mask=0x0F;
  gml_surface_reset_target(&render);
  gml_draw_surface_stretched(&render,surface,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  if(frame[0]!=0xFF00003Fu || frame[WIDTH+1]!=0xFF0000FFu){
    fprintf(stderr,"renderer masked surface composite mismatch: %08x / %08x\n",
            frame[0],frame[WIDTH+1]);
    gml_render_free(&render);
    return 1;
  }
  gml_render_free(&render);
  return 0;
}

static int world_raster_scale_case(void){
  enum { TARGET_WIDTH=12,TARGET_HEIGHT=8 };
  GmlRender render;
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  memset(&render,0,sizeof render);
  memset(target,0,sizeof target);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  int source=gml_surface_create(&render,1,1);
  int offscreen=gml_surface_create(&render,6,4);
  REQUIRE(source==1 && offscreen==2,"world raster surface ids");
  uint32_t *source_pixel=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixel!=NULL,"world raster source pixel");
  *source_pixel=0xFF30A070u;
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=1;
  render.surface[0].all_transparent=0;

  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,1.5,0.5);
  gml_render_world_set_logical_extent(&render,6,4);
  GmlRenderTargetMetrics metrics={0};
  REQUIRE(gml_render_target_metrics(&render,&metrics) &&
          metrics.width==TARGET_WIDTH && metrics.height==TARGET_HEIGHT &&
          metrics.camera_x==3.0 && metrics.camera_y==1.0,
          "world raster physical target metrics");
  double point_x=2.0,point_y=1.0,scale_x=1.5,scale_y=0.5;
  gml_render_draw_map_point(&render,&point_x,&point_y);
  gml_render_draw_map_scale(&render,&scale_x,&scale_y);
  REQUIRE(point_x==4.0 && point_y==2.0 && scale_x==3.0 && scale_y==1.0,
          "world raster coordinate transform");

  gml_draw_surface_stretched(&render,source,2.0,1.0,2.0,1.0,0xFFFFFFu,1.0);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    int covered=x>=1 && x<5 && y>=1 && y<3;
    uint32_t expected=covered?0xFF30A070u:0;
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,"renderer world raster mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      gml_surface_free(&render,source);
      gml_surface_free(&render,offscreen);
      return 1;
    }
  }

  REQUIRE(gml_surface_set_target(&render,offscreen),"world raster offscreen target");
  gml_draw_surface_stretched(&render,source,1.0,1.0,2.0,1.0,0xFFFFFFu,1.0);
  gml_surface_reset_target(&render);
  int offscreen_width=0,offscreen_height=0;
  const uint32_t *offscreen_pixels=
    gml_surface_pixels_read(&render,offscreen,&offscreen_width,&offscreen_height);
  REQUIRE(offscreen_pixels && offscreen_width==6 && offscreen_height==4,
          "world raster offscreen pixels");
  for(int y=0;y<offscreen_height;y++) for(int x=0;x<offscreen_width;x++){
    int covered=x>=1 && x<3 && y==1;
    uint32_t expected=covered?0xFF30A070u:0;
    if(offscreen_pixels[(size_t)y*offscreen_width+x]!=expected){
      fprintf(stderr,"renderer world raster offscreen mismatch at %d,%d\n",x,y);
      gml_surface_free(&render,source);
      gml_surface_free(&render,offscreen);
      return 1;
    }
  }
  gml_surface_free(&render,source);
  gml_surface_free(&render,offscreen);
  return 0;
}

static int replace_and_classic_present_case(void){
  /* The base canvas is wider than the surface so the whole-frame copy fast paths stand down
   * and the per-run kernel — the one the defect lived in — is what this case exercises. */
  enum { WIDTH=4,HEIGHT=3,BASEW=6,BASEH=5 };
  GmlRender render;
  uint32_t frame[BASEW*BASEH];
  memset(&render,0,sizeof render);
  memset(frame,0,sizeof frame);
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=BASEW;
  render.fbh=render.base_fbh=BASEH;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  /* A compositor draws a translucent pass into its surface: the colour is resolved there and
   * the destination-alpha arithmetic leaves partial coverage behind. */
  int source=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(source==1,"replace case surface id");
  REQUIRE(gml_surface_set_target(&render,source),"replace case surface target");
  gml_render_clear(&render,0x000000u,1.0);
  render.alpha=0.5;
  gml_render_primitive_rectangle(&render,1,1,1,1,0x302010u,0);
  render.alpha=1.0;
  gml_surface_reset_target(&render);
  uint32_t *pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(pixels!=NULL,"replace case surface pixels");
  uint32_t held=pixels[1*WIDTH+1];
  REQUIRE((held>>24)>0u && (held>>24)<255u,
          "replace case surface holds partial coverage");
  render.surface[0].opaque_known=0;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;

  /* (one, zero) copies what the surface holds, coverage included. */
  render.blendmode=6;
  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(frame[1*BASEW+1]==held,
          "one-zero surface copy lands the texel verbatim");
  render.blendmode=0;

  /* The classic automatic present models an alphaless backbuffer: the colour reaches the
   * screen untouched however much coverage the surface accumulated. */
  render.classic=1;
  render.app_surface=pixels;
  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE((frame[1*BASEW+1]&0x00FFFFFFu)==(held&0x00FFFFFFu),
          "classic present keeps the composed colour");
  REQUIRE((frame[1*BASEW+1]>>24)==0xFFu,
          "classic present writes an opaque screen pixel");

  /* An ordinary surface draw keeps blending by coverage: only the pad above is exempt. */
  render.app_surface=NULL;
  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE((frame[1*BASEW+1]&0x00FFFFFFu)!=(held&0x00FFFFFFu),
          "an ordinary surface draw still blends partial coverage");

  gml_surface_free(&render,source);
  return 0;
}

int main(void){
  GmlRender render;
  uint32_t base[8*6];
  memset(&render,0,sizeof render);
  memset(base,0,sizeof base);
  render.fb=render.base_fb=base;
  render.fbw=render.base_fbw=8;
  render.fbh=render.base_fbh=6;
  render.target_id=-1;
  render.next_surface_id=1;

  int source=gml_surface_create(&render,4,3);
  int destination=gml_surface_create(&render,6,5);
  REQUIRE(source==1 && destination==2,"surface ids");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"source pixels");
  for(int y=0;y<3;y++) for(int x=0;x<4;x++){
    unsigned red=(unsigned)(31+x*47+y*13);
    unsigned green=(unsigned)(17+x*19+y*53);
    unsigned blue=(unsigned)(7+x*29+y*37);
    source_pixels[(size_t)y*4+x]=0xFF000000u|(red<<16)|(green<<8)|blue;
  }

  render.projection_cam_x=4.0;
  render.projection_cam_y=5.0;
  render.cam_x=render.projection_cam_x;
  render.cam_y=render.projection_cam_y;
  REQUIRE(gml_surface_set_target(&render,source),"set target");
  REQUIRE(render.fb==source_pixels && render.fbw==4 && render.fbh==3 &&
          render.cam_x==0.0 && render.cam_y==0.0,"target state");
  render.fb_opaque_known=1;
  render.fb_all_opaque=1;
  render.fb_all_transparent=0;
  gml_surface_reset_target(&render);
  REQUIRE(render.fb==base && render.fbw==8 && render.fbh==6 &&
          render.cam_x==4.0 && render.cam_y==5.0 &&
          render.projection_cam_x==4.0 && render.projection_cam_y==5.0,
          "restored target state");
  REQUIRE(surface_known_opaque(&render,source),"opaque coverage");

  gml_surface_copy(&render,destination,1,1,source);
  REQUIRE(gml_surface_width(&render,destination)==6 &&
          gml_surface_height(&render,destination)==5,"copy dimensions");
  gml_surface_resize(&render,destination,4,4);
  int width=0,height=0;
  const uint32_t *destination_pixels=
    gml_surface_pixels_read(&render,destination,&width,&height);
  REQUIRE(destination_pixels!=NULL && width==4 && height==4,"resize dimensions");
  uint64_t hash=pixel_hash(destination_pixels,(size_t)width*height);
  if(hash!=UINT64_C(0x7d8e0da85e14b8af)){
    fprintf(stderr,"renderer surfaces hash: %016llx\n",(unsigned long long)hash);
    return 1;
  }

  gml_surface_free(&render,source);
  gml_surface_free(&render,destination);
  REQUIRE(!gml_surface_exists(&render,source) &&
          !gml_surface_exists(&render,destination),"freed surfaces");
  REQUIRE(presentation_latch_coverage_case()==0,
          "presentation latch coverage case");
  REQUIRE(composition_cases()==0,"composition cases");
  REQUIRE(screen_raster_part_case()==0,"screen raster part case");
  REQUIRE(first_generation_point_sampling_case()==0,
          "first-generation point sampling case");
  REQUIRE(first_generation_high_magnification_case()==0,
          "first-generation high magnification case");
  REQUIRE(opaque_integer_scale_case()==0,"opaque integer scale case");
  REQUIRE(point_downscale_case()==0,"point downscale case");
  REQUIRE(opaque_near_identity_reduction_case()==0,
          "opaque near-identity reduction case");
  REQUIRE(zero_reference_alpha_test_case()==0,
          "zero-reference alpha-test case");
  REQUIRE(max_preset_surface_case()==0,"maximum preset surface case");
  REQUIRE(subtract_surface_coverage_case()==0,"subtract surface coverage case");
  REQUIRE(masked_sprite_surface_case()==0,"masked sprite surface case");
  REQUIRE(world_raster_scale_case()==0,"world raster scale case");
  REQUIRE(replace_and_classic_present_case()==0,"replace and classic present case");
  puts("renderer surfaces: ok");
  return 0;
}
