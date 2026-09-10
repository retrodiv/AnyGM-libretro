/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_render_internal.h"
#include "anygm_test_runner.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GmlVM vm; GmlRender render; GmlAtlas atlas; GmlTpag page; GmlBg background;
  GmlTileMap map; GmlRtLayer layer;
  uint32_t pixels[16*16];
  uint8_t rgba[6*3*4],ids[6*4],cells[2*4];
} TileFixture;
static const uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
static void word(uint8_t *out,uint32_t value){
  for(int i=0;i<4;i++) out[i]=(uint8_t)(value>>(8*i));
}
static void setup(TileFixture *f){
  memset(f,0,sizeof *f);
  for(int y=0;y<2;y++) for(int x=0;x<6;x++){
    uint32_t c=x<2?0xff808080:colors[y*2+(x%2)]; int at=(y*6+x)*4;
    f->rgba[at]=(uint8_t)(c>>16); f->rgba[at+1]=(uint8_t)(c>>8);
    f->rgba[at+2]=(uint8_t)c; f->rgba[at+3]=255;
  }
  const unsigned mapping[]={0,0,1,2,2,0};
  for(int i=0;i<6;i++) word(f->ids+4*i,mapping[i]);
  word(f->cells,1); word(f->cells+4,2);
  f->atlas=(GmlAtlas){.px=f->rgba,.w=6,.h=2};
  f->page=(GmlTpag){.atlas=0,.sw=6,.bw=6,.sh=2,.bh=2,
    .alpha_scanned=1,.alpha_max=255,.ax1=5,.ay1=1};
  f->background=(GmlBg){.tpag=0,.tile_w=2,.tile_h=2,.tile_columns=3,
    .tile_count=3,.tile_items_per_tile=2,.tile_frame_length_us=100000,.tile_ids=f->ids};
  f->render=(GmlRender){.fb=f->pixels,.base_fb=f->pixels,.fbw=16,.base_fbw=16,
    .fbh=16,.base_fbh=16,.atlas=&f->atlas,.n_atlas=1,.tpag=&f->page,.n_tpag=1,
    .bg=&f->background,.n_bg=1,.alpha=1,.color=0xffffff,.alphablend=1,
    .color_write_mask=15,.active_shader=-1,.target_id=-1};
  f->map=(GmlTileMap){.id=7,.used=1,.order=2,.x=40,.y=50,.depth=17,
    .tileset=0,.tw=2,.th=2,.cols=2,.rows=1,.tiles=f->cells};
  f->layer=(GmlRtLayer){.id=70,.used=1,.order=2,.x=11,.y=13,.depth=17};
  f->vm=(GmlVM){.render=&f->render,.tilemaps=&f->map,.n_tilemaps=1,
    .rtl=&f->layer,.n_rtl=1,.frame=6};
  *gml_varmap_put(&f->vm.globals,"room_speed")=vreal(60);
}
static void cleanup(TileFixture *f){
  f->vm.render=NULL; f->vm.tilemaps=NULL; f->vm.n_tilemaps=0;
  f->vm.rtl=NULL; f->vm.n_rtl=0; gml_vm_free(&f->vm);
  gml_render_texture_page_cache_clear(&f->render,&f->page);
  free(f->render.rotated_batch);
}
static int call(TileFixture *f,const char *name,GmlVal *args,int n,int cached){
  if(cached){
    int id=gml_builtin_fast_id(&f->vm,name); if(id<0) return 0;
    gml_builtin_call_fast_id(&f->vm,id,name,args,n);
  }else gml_builtin_call(&f->vm,name,args,n);
  gml_render_flush_rotated_batch(&f->render); return 1;
}
static int region(TileFixture *f,int x,int y,const uint32_t expected[4]){
  for(int yy=0;yy<16;yy++) for(int xx=0;xx<16;xx++){
    uint32_t want=xx>=x && xx<x+2 && yy>=y && yy<y+2?expected[(yy-y)*2+xx-x]:0;
    if(f->pixels[yy*16+xx]!=want){
      fprintf(stderr,"tile pixel (%d,%d): %08x != %08x\n",xx,yy,
              f->pixels[yy*16+xx],want); return 0;
    }
  }
  return 1;
}
static int transforms(void){
  static const unsigned order[8][4]={{0,1,2,3},{1,0,3,2},{2,3,0,1},{3,2,1,0},
    {2,0,3,1},{3,1,2,0},{0,2,1,3},{1,3,0,2}};
  int ok=1;
  for(int cached=0;cached<2;cached++) for(int flags=0;flags<8;flags++){
    TileFixture f; setup(&f);
    GmlVal args[]={vreal(0),vreal(1u|((unsigned)flags<<28)),vreal(1),vreal(3),vreal(5)};
    uint32_t expected[4]; for(int i=0;i<4;i++) expected[i]=colors[order[flags][i]];
    ok &= call(&f,"draw_tile",args,5,cached) && region(&f,3,5,expected); cleanup(&f);
  }
  return ok;
}
static int draw_state_and_animation(void){
  int ok=1; const uint32_t tinted[]={0xff000000,0xff00ff00,0xff000000,0xff00ff00},empty[4]={0};
  for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f); f.render.color=0x00ff00;
    GmlVal args[]={vreal(0),vreal(1),vreal(0),vreal(3),vreal(5)};
    ok &= call(&f,"draw_tile",args,5,cached) && region(&f,3,5,tinted);
    memset(f.pixels,0,sizeof f.pixels); f.render.alpha=0;
    ok &= call(&f,"draw_tile",args,5,cached) && region(&f,3,5,empty);
    f.render.alpha=1; args[1]=vreal(2); args[2]=vreal(1);
    ok &= call(&f,"draw_tile",args,5,cached) && region(&f,3,5,empty);
    args[1]=vreal(0); args[2]=vreal(0);
    ok &= call(&f,"draw_tile",args,5,cached) && region(&f,3,5,empty); cleanup(&f);
  }
  return ok;
}
static int explicit_map(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f); f.render.alpha=0; f.render.color=0;
    GmlTileMap before=f.map; GmlRtLayer layer_before=f.layer;
    GmlVal args[]={vreal(7),vreal(3),vreal(5)};
    ok &= call(&f,"draw_tilemap",args,3,cached) && region(&f,3,5,colors);
    ok &= !memcmp(&before,&f.map,sizeof before) && !memcmp(&layer_before,&f.layer,sizeof layer_before);
    ok &= f.render.alpha==0 && f.render.color==0;
    memset(f.pixels,0,sizeof f.pixels); f.render.cam_x=2; f.render.cam_y=1;
    ok &= call(&f,"draw_tilemap",args,3,cached) && region(&f,1,4,colors); cleanup(&f);
  }
  return ok;
}
static int invalid_arguments(void){
  int ok=1; const uint32_t empty[4]={0};
  const double invalid[]={NAN,INFINITY,-INFINITY,1e100,-1e100};
  for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f);
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      for(int arg=0;arg<5;arg++){
        GmlVal args[]={vreal(0),vreal(1),vreal(0),vreal(3),vreal(5)}; args[arg]=vreal(invalid[i]);
        ok &= call(&f,"draw_tile",args,5,cached) && region(&f,3,5,empty);
      }
      for(int arg=0;arg<3;arg++){
        GmlVal args[]={vreal(7),vreal(3),vreal(5)}; args[arg]=vreal(invalid[i]);
        ok &= call(&f,"draw_tilemap",args,3,cached) && region(&f,3,5,empty);
      }
    }
    ok &= call(&f,"draw_tile",NULL,0,cached) && call(&f,"draw_tilemap",NULL,0,cached);
    ok &= region(&f,3,5,empty); cleanup(&f);
  }
  return ok;
}
static int automatic_map(void){
  TileFixture f; setup(&f); GmlWin win={0}; f.vm.win=&win;
  f.map.visible=1; f.layer.visible=1; f.layer.x=3; f.layer.y=5;
  f.layer.script_begin=f.layer.script_end=-1;
  gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
  int ok=region(&f,3,5,colors);
  f.vm.win=NULL; cleanup(&f); return ok;
}
static int prepared_layouts(void){
  TileFixture f; setup(&f);
  f.page.bw=28; f.page.bh=33;
  f.background.tile_w=2; f.background.tile_h=3;
  f.background.tile_border_x=1; f.background.tile_border_y=2;
  f.background.tile_separation_x=3; f.background.tile_separation_y=4;
  f.background.tile_columns=4;
  GmlRenderTilesetLayout layout; GmlRenderTileSource source;
  int ok=gml_render_tileset_layout(&f.render,0,1,1,&layout);
  ok &= layout.pitch_x==7 && layout.pitch_y==11 && layout.columns==4;
  ok &= gml_render_tileset_source(&f.render,&layout,1,1,&source);
  ok &= source.x==15 && source.y==2 && source.width==2 && source.height==3;
  ok &= !gml_render_tileset_source(&f.render,&layout,2,1,&source);
  /* The legacy layout has no map: its first nonempty index selects cell zero. */
  f.background.tile_ids=NULL; f.background.tile_w=0; f.background.tile_h=0;
  f.background.tile_columns=0;
  ok &= gml_render_tileset_layout(&f.render,0,2,3,&layout) && layout.columns==4;
  ok &= gml_render_tileset_source(&f.render,&layout,1,0,&source);
  ok &= source.x==1 && source.y==2 && source.width==2 && source.height==3;
  f.background.tile_w=INT_MAX; f.background.tile_h=3;
  ok &= !gml_render_tileset_layout(&f.render,0,2,3,&layout);
  cleanup(&f); return ok;
}
static int scaled_map(void){
  int ok=1;
  for(int automatic=0;automatic<2;automatic++){
    TileFixture f; setup(&f); GmlWin win={0}; f.vm.win=&win;
    f.render.world_transform_active=1; f.render.world_scale_x=2; f.render.world_scale_y=3;
    f.render.cam_x=6; f.render.cam_y=9;
    if(automatic){
      f.map.visible=1; f.layer.visible=1; f.layer.x=4; f.layer.y=4;
      f.layer.script_begin=f.layer.script_end=-1; gml_vm_draw(&f.vm);
      gml_render_flush_rotated_batch(&f.render);
    }else{
      GmlVal args[]={vreal(7),vreal(4),vreal(4)};
      ok &= call(&f,"draw_tilemap",args,3,1);
    }
    for(int y=0;y<16;y++) for(int x=0;x<16;x++){
      uint32_t want=x>=2 && x<6 && y>=3 && y<9?colors[((y-3)/3)*2+(x-2)/2]:0;
      if(f.pixels[y*16+x]!=want){
        fprintf(stderr,"scaled map pixel (%d,%d): %08x != %08x\n",x,y,f.pixels[y*16+x],want);
        ok=0;
      }
    }
    f.vm.win=NULL; cleanup(&f);
  }
  return ok;
}
static int bounded_empty_maps(void){
  int ok=1; const uint32_t empty[4]={0};
  for(int sign=-1;sign<=1;sign+=2){
    TileFixture f; setup(&f); GmlWin win={0}; f.vm.win=&win;
    f.map.visible=1; f.layer.visible=1; f.layer.x=sign*1e100; f.layer.y=sign*1e100;
    f.layer.script_begin=f.layer.script_end=-1;
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    ok &= region(&f,0,0,empty);
    f.vm.win=NULL; cleanup(&f);
  }
  return ok;
}
static int rectangular_tile_extent(void){
  int ok=1;
  for(int scaled=0;scaled<2;scaled++) for(int flags=0;flags<8;flags++){
    TileFixture f; setup(&f);
    memset(f.rgba,255,sizeof f.rgba);
    f.atlas.h=3; f.page.sh=f.page.bh=3; f.page.ay1=2;
    f.background.tile_h=3; f.map.th=3;
    int xs=scaled?2:1,ys=scaled?3:1;
    f.render.world_transform_active=1; f.render.world_scale_x=xs; f.render.world_scale_y=ys;
    GmlVal args[]={vreal(0),vreal(1u|((unsigned)flags<<28)),vreal(1),vreal(2),vreal(1)};
    ok &= call(&f,"draw_tile",args,5,1);
    for(int y=0;y<16;y++) for(int x=0;x<16;x++){
      uint32_t want=x>=2*xs && x<4*xs && y>=ys && y<4*ys?0xffffffff:0;
      if(f.pixels[y*16+x]!=want){
        fprintf(stderr,"rectangular tile: scaled=%d flags=%d (%d,%d) %08x != %08x\n",
          scaled,flags,x,y,f.pixels[y*16+x],want); ok=0;
      }
    }
    cleanup(&f);
  }
  return ok;
}
int main(int argc,char **argv){
  const AnygmTestCase cases[]={{"transforms",transforms},
    {"draw_state_and_animation",draw_state_and_animation},{"explicit_map",explicit_map},
    {"invalid_arguments",invalid_arguments},{"automatic_animated_empty",automatic_map},
    {"prepared_layouts",prepared_layouts},{"scaled_map",scaled_map},
    {"bounded_empty_maps",bounded_empty_maps},{"rectangular_tile_extent",rectangular_tile_extent}};
  const AnygmTestGroup group={"tile_draw",cases,sizeof cases/sizeof cases[0]};
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2]; else if(argc!=1) return EXIT_FAILURE;
  AnygmTestResult result; anygm_test_run_groups(&group,1,filter,&result);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
