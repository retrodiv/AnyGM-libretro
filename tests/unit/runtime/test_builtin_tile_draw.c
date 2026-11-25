/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_vm_internal.h"
#include "gml_render_internal.h"
#include "gml_particle.h"
#include "anygm_compatibility.h"
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
  if(f->vm.win){
    free(f->vm.win->str_hix);
    free(f->vm.win->str_hix_hash);
  }
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
  f.map.x=f.map.y=0;
  f.map.visible=1; f.layer.visible=1; f.layer.x=3; f.layer.y=5;
  f.layer.script_begin=f.layer.script_end=-1;
  gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
  int ok=region(&f,3,5,colors);
  f.vm.win=NULL; cleanup(&f); return ok;
}
static int static_room_tile_scale(void){
  TileFixture f; setup(&f);
  uint8_t data[256]={0};
  char *strings[]={"room"}; uint32_t string_offsets[]={200};
  GmlWin win={.data=data,.size=sizeof data,.n_chunks=1,
    .strs=strings,.str_charoff=string_offsets,.n_strs=1};
  win.chunks[0]=(GmlChunk){"ROOM",16,200};
  word(data+16,1); word(data+20,32);
  word(data+32,200); word(data+40,16); word(data+44,16);
  word(data+84,100); /* room tile pointer */
  word(data+100,1); word(data+104,112);
  word(data+112,3); word(data+116,4); /* tile position */
  word(data+132,2); word(data+136,2); /* source extent */
  word(data+140,15); word(data+144,1); /* depth and id */
  word(data+148,0x40400000u); word(data+152,0x40000000u); /* 3 x 2 */
  word(data+156,0xffffffffu);
  f.vm.win=&win; f.render.win=&win;
  gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
  int ok=1;
  for(int y=0;y<16;y++) for(int x=0;x<16;x++){
    uint32_t expected=x>=3 && x<9 && y>=4 && y<8?0xff808080u:0;
    if(f.pixels[y*16+x]!=expected){
      fprintf(stderr,"static room tile pixel (%d,%d): %08x != %08x\n",
              x,y,f.pixels[y*16+x],expected);
      ok=0;
    }
  }
  f.vm.win=NULL; f.render.win=NULL; cleanup(&f); return ok;
}
static int local_map_position(void){
  TileFixture f; setup(&f); GmlWin win={0}; f.vm.win=&win;
  f.map.visible=f.layer.visible=1; f.layer.x=f.layer.y=1;
  f.layer.script_begin=f.layer.script_end=-1;
  GmlVal x[]={vreal(7),vreal(4)},y[]={vreal(7),vreal(6)};
  gml_builtin_call(&f.vm,"tilemap_x",x,2); gml_builtin_call(&f.vm,"tilemap_y",y,2);
  gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
  int ok=region(&f,5,7,colors);
  memset(f.pixels,0,sizeof f.pixels); f.layer.x=f.layer.y=2;
  gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
  ok &= region(&f,6,8,colors) && f.map.x==4 && f.map.y==6;
  memset(f.pixels,0,sizeof f.pixels);
  GmlVal explicit_position[]={vreal(7),vreal(3),vreal(5)};
  ok &= call(&f,"draw_tilemap",explicit_position,3,1) && region(&f,3,5,colors);
  f.vm.win=NULL; cleanup(&f); return ok;
}
static int moved_tilemap_draw(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f);
    GmlWin win={.bytecode=15,.classic_version=800};
    f.vm.win=&win; f.render.win=&win;
    GmlRtLayer layers[]={
      {.id=70,.used=1,.order=2,.x=2,.y=3,.depth=17,.visible=1,.script_begin=-1,.script_end=-1},
      {.id=71,.used=1,.order=3,.x=8,.y=5,.depth=4,.visible=1,.script_begin=-1,.script_end=-1,.name="destination"}
    };
    f.vm.rtl=layers; f.vm.n_rtl=2;
    f.map.x=1; f.map.y=2; f.map.cols=1; f.map.visible=1;
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    ok &= region(&f,3,5,colors);
    GmlVal move[]={vreal(f.map.id),cached?vreal(71):vstr("destination")};
    ok &= call(&f,"layer_element_move",move,2,cached);
    memset(f.pixels,0,sizeof f.pixels);
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    ok &= region(&f,9,7,colors);
    GmlVal create[]={vreal(0),vreal(0),vreal(0),vreal(2),vreal(2),vreal(1),vreal(2),vreal(4)};
    GmlVal tile=gml_builtin_call(&f.vm,"tile_add",create,8);
    ok &= tile.d>=0;
    memset(f.pixels,0,sizeof f.pixels);
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    ok &= region(&f,9,7,colors);
    move[0]=tile;
    ok &= call(&f,"layer_element_move",move,2,cached);
    memset(f.pixels,0,sizeof f.pixels);
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    const uint32_t grey[]={0xff808080u,0xff808080u,0xff808080u,0xff808080u};
    ok &= region(&f,9,7,grey);
    GmlVal elements=gml_builtin_call(&f.vm,"layer_get_all_elements",move+1,1);
    ok &= gml_val_array_length(elements)==2 &&
          gml_arr_get(elements,0).d==f.map.id && gml_arr_get(elements,1).d==tile.d;
    gml_values_release_owned(&elements,1);
    layers[1].visible=0;
    memset(f.pixels,0,sizeof f.pixels);
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    const uint32_t empty[4]={0}; ok &= region(&f,9,7,empty);
    cleanup(&f);
  }
  if(ok) puts("layer move: destination position, append order and visibility passed");
  return ok;
}
static int moved_authored_background(void){
  TileFixture f; setup(&f);
  uint8_t data[320]={0};
  char *strings[]={"room","authored"}; uint32_t offsets[]={260,280};
  memcpy(data+260,"room",5); memcpy(data+280,"authored",9);
  GmlWin win={.data=data,.size=sizeof data,.bytecode=17,.n_chunks=1,
              .strs=strings,.str_charoff=offsets,.n_strs=2};
  win.chunks[0]=(GmlChunk){"ROOM",16,244};
  word(data+16,1); word(data+20,32); word(data+32,260);
  word(data+40,16); word(data+44,16);
  word(data+120,128); word(data+128,1); word(data+132,144);
  word(data+144,280); word(data+152,1); word(data+176,1);
  word(data+180,1); word(data+188,UINT32_MAX); word(data+204,0xff0000ffu);
  f.vm.win=&win; f.render.win=&win;
  f.vm.rtl=NULL; f.vm.n_rtl=0; f.vm.tilemaps=NULL; f.vm.n_tilemaps=0;
  gml_vm_room_reload_layers_mode(&f.vm,0,1);
  GmlRtLayer *destination=gml_rt_layer_new(&f.vm);
  int ok=destination && f.vm.n_rte==1 && f.vm.rte[0].type==1;
  if(ok){
    destination->visible=0;
    GmlVal move[]={vreal(f.vm.rte[0].id),vreal(destination->id)};
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    gml_render_flush_pending_fill(&f.render);
    ok &= f.pixels[0]==0xffff0000u;
    gml_builtin_call(&f.vm,"layer_element_move",move,2);
    memset(f.pixels,0,sizeof f.pixels);
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    gml_render_flush_pending_fill(&f.render);
    ok &= f.pixels[0]==0;
    destination->visible=1;
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    gml_render_flush_pending_fill(&f.render);
    ok &= f.pixels[0]==0xffff0000u;
  }
  if(!ok) fputs("moved authored background must only draw from its current layer\n",stderr);
  free(f.vm.rtl);
  cleanup(&f);
  return ok;
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
      f.map.x=f.map.y=0;
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
static int legacy_dynamic_tiles(void){
  int ok=1;
  for(int generation=0;generation<4;generation++){
    TileFixture f; setup(&f);
    GmlWin win={.bytecode=generation?13+generation:16,
                .classic_version=generation?0:800};
    f.vm.win=&win; f.render.win=&win;
    f.vm.tilemaps=NULL; f.vm.n_tilemaps=0;
    f.layer.visible=1; f.layer.x=f.layer.y=0;
    f.layer.script_begin=f.layer.script_end=-1;
    GmlVal args[]={vreal(0),vreal(2),vreal(0),vreal(2),vreal(2),
                   vreal(3),vreal(5),vreal(17)};
    GmlVal tile=gml_builtin_call(&f.vm,"tile_add",args,8);
    int id=(int)tile.d;
    GmlRtElem *element=f.vm.n_rte?f.vm.rte:NULL;
    if(!element || !element->used || element->type!=7){
      fprintf(stderr,"legacy generation %d did not create a dynamic tile\n",generation);
      ok=0;
    }else{
      ok &= gml_builtin_call(&f.vm,"tile_exists",&tile,1).d==1;
      GmlVal point[]={vreal(17),vreal(4),vreal(6)};
      ok &= gml_builtin_call(&f.vm,"tile_layer_find",point,3).d==id;
      gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
      ok &= region(&f,3,5,colors);
      gml_builtin_call(&f.vm,"tile_delete",&tile,1);
      ok &= !element->used && gml_builtin_call(&f.vm,"tile_exists",&tile,1).d==0;
      ok &= gml_builtin_call(&f.vm,"tile_layer_find",point,3).d==-1;
      memset(f.pixels,0,sizeof f.pixels);
      gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
      const uint32_t empty[4]={0}; ok &= region(&f,3,5,empty);
    }
    f.vm.win=NULL; f.render.win=NULL; cleanup(&f);
  }
  return ok;
}
static int legacy_dynamic_tile_scale(void){
  int ok=1;
  for(int generation=0;generation<4;generation++) for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f);
    GmlWin win={.bytecode=generation?13+generation:16,
                .classic_version=generation?0:800};
    f.vm.win=&win; f.render.win=&win;
    f.vm.tilemaps=NULL; f.vm.n_tilemaps=0;
    f.layer.visible=1; f.layer.x=f.layer.y=0;
    f.layer.script_begin=f.layer.script_end=-1;
    for(int y=0;y<2;y++) for(int x=2;x<4;x++){
      int at=(y*6+x)*4;
      f.rgba[at]=255; f.rgba[at+1]=f.rgba[at+2]=0;
    }
    GmlVal args[]={vreal(0),vreal(2),vreal(0),vreal(2),vreal(2),
                   vreal(3),vreal(5),vreal(17)};
    GmlVal tile=gml_builtin_call(&f.vm,"tile_add",args,8);
    GmlRtElem *element=gml_rt_elem_find(&f.vm,(int)tile.d);
    GmlVal scale[]={tile,vreal(2),vreal(0.5)};
    ok &= call(&f,"tile_set_scale",scale,3,cached);
    if(!element || element->xs!=2 || element->ys!=0.5){
      fprintf(stderr,"legacy generation %d cached %d did not apply tile scale\n",
              generation,cached); ok=0;
    }
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    for(int y=0;y<16;y++) for(int x=0;x<16;x++){
      uint32_t want=x>=3 && x<7 && y==5?0xffff0000:0;
      if(f.pixels[y*16+x]!=want){
        fprintf(stderr,"scaled legacy tile pixel (%d,%d): %08x != %08x\n",
                x,y,f.pixels[y*16+x],want); ok=0;
      }
    }
    scale[0]=vreal(-1); scale[1]=scale[2]=vreal(9);
    call(&f,"tile_set_scale",scale,3,cached);
    ok &= element && element->xs==2 && element->ys==0.5;
    f.vm.win=NULL; f.render.win=NULL; cleanup(&f);
  }
  return ok;
}
static GmlVal tile_query(TileFixture *f,const char *name,GmlVal arg,int cached){
  if(!cached) return gml_builtin_call(&f->vm,name,&arg,1);
  int id=gml_builtin_fast_id(&f->vm,name);
  return id>0?gml_builtin_call_fast_id(&f->vm,id,name,&arg,1):vundef();
}
static int tile_query_ids(TileFixture *f,double depth,const double *expected,int count,int cached){
  GmlVal result=tile_query(f,"tile_get_ids_at_depth",vreal(depth),cached);
  int ok=gml_val_array_length(result)==count;
  for(int i=0;i<count;i++){
    GmlVal value=gml_arr_get(result,i);
    ok &= value.t==V_REAL && value.d==expected[i];
  }
  gml_values_release_owned(&result,1);
  return ok;
}
static int legacy_tile_queries(void){
  static const double none[]={-1},authored_id=10000000,other_id=10000001;
  int ok=1;
  for(int generation=0;generation<4;generation++) for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f);
    uint8_t data[288]={0};
    char *strings[]={"room"}; uint32_t string_offsets[]={240};
    GmlWin win={.data=data,.size=sizeof data,.n_chunks=1,
      .strs=strings,.str_charoff=string_offsets,.n_strs=1,
      .bytecode=generation?13+generation:16,.classic_version=generation?0:800};
    win.chunks[0]=(GmlChunk){"ROOM",16,224};
    word(data+16,1); word(data+20,32); word(data+32,240);
    word(data+40,16); word(data+44,16); word(data+84,100);
    word(data+100,3); word(data+104,120); word(data+108,168); word(data+112,280);
    word(data+120,3); word(data+124,4);
    word(data+128,5); word(data+132,7); word(data+136,11);
    word(data+148,17); word(data+152,(uint32_t)authored_id);
    word(data+196,21); word(data+200,(uint32_t)other_id);
    f.vm.win=&win;
    GmlVal args[]={vreal(3),vreal(13),vreal(19),vreal(2),vreal(2),
                   vreal(27),vreal(29),vreal(17)};
    GmlVal tile=gml_builtin_call(&f.vm,"tile_add",args,8);
    GmlRtElem *element=gml_rt_elem_find(&f.vm,(int)tile.d);
    if(!element){ cleanup(&f); return 0; }
    element->visible=0;
    GmlVal authored=vreal(authored_id);
    ok &= tile_query(&f,"tile_get_background",authored,cached).d==5;
    ok &= tile_query(&f,"tile_get_left",authored,cached).d==7;
    ok &= tile_query(&f,"tile_get_top",authored,cached).d==11;
    ok &= tile_query(&f,"tile_get_background",tile,cached).d==3;
    ok &= tile_query(&f,"tile_get_left",tile,cached).d==13;
    ok &= tile_query(&f,"tile_get_top",tile,cached).d==19;
    const double both[]={authored_id,tile.d},dynamic[]={tile.d};
    ok &= tile_query_ids(&f,17,both,2,cached);
    ok &= tile_query_ids(&f,21,&other_id,1,cached);
    ok &= tile_query_ids(&f,99,none,1,cached);

    gml_tile_layer_hide(&f.vm,17,1);
    gml_tile_layer_depth(&f.vm,17,23);
    gml_tile_layer_shift(&f.vm,17,2.75,-1.75);
    ok &= tile_query_ids(&f,17,none,1,cached);
    ok &= tile_query_ids(&f,23,both,2,cached);
    gml_tile_layer_delete_at(&f.vm,23,5,3);
    ok &= tile_query_ids(&f,23,dynamic,1,cached);
    ok &= tile_query(&f,"tile_get_left",authored,cached).d==0;
    gml_tile_layer_delete(&f.vm,17);
    ok &= tile_query_ids(&f,23,none,1,cached);
    ok &= tile_query(&f,"tile_get_top",tile,cached).d==0;
    ok &= tile_query_ids(&f,21,&other_id,1,cached);
    cleanup(&f);
  }
  if(ok) puts("legacy tile queries: authored and dynamic sources, depth, visibility and deletion passed");
  return ok;
}
static int legacy_tile_pixels(TileFixture *f,unsigned authored,unsigned dynamic){
  for(int i=0;i<16*16;i++) f->pixels[i]=0xff000000u;
  gml_vm_draw(&f->vm);
  gml_render_flush_rotated_batch(&f->render);
  for(int y=0;y<16;y++) for(int x=0;x<16;x++){
    unsigned red=0;
    if(y>=5 && y<7){
      if(x>=3 && x<5) red=authored;
      if(x>=8 && x<10) red=dynamic;
    }
    uint32_t expected=0xff000000u|(red<<16);
    if(f->pixels[y*16+x]!=expected){
      fprintf(stderr,"legacy tile appearance (%d,%d): %08x != %08x\n",x,y,f->pixels[y*16+x],expected);
      return 0;
    }
  }
  return 1;
}
static int legacy_tile_appearance(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f);
    uint8_t data[320]={0};
    char *strings[]={"room"}; uint32_t string_offsets[]={288};
    GmlWin win={.data=data,.size=sizeof data,.n_chunks=1,.bytecode=15,.classic_version=800,
                .strs=strings,.str_charoff=string_offsets,.n_strs=1};
    win.chunks[0]=(GmlChunk){"ROOM",16,272};
    word(data+16,2); word(data+20,32); word(data+24,96);
    for(int room=32;room<=96;room+=64){
      word(data+room,288); word(data+room+8,16); word(data+room+12,16); word(data+room+16,60);
    }
    word(data+52,1); word(data+84,160);
    word(data+160,1); word(data+164,176);
    word(data+176,3); word(data+180,5); word(data+188,2);
    word(data+196,2); word(data+200,2); word(data+204,17); word(data+208,10000000);
    word(data+212,0x3f800000u); word(data+216,0x3f800000u); word(data+220,0xffffffffu);
    for(int y=0;y<2;y++) for(int x=2;x<4;x++){
      int at=(y*6+x)*4; f.rgba[at]=255; f.rgba[at+1]=f.rgba[at+2]=0;
    }
    f.vm.win=&win; f.render.win=&win;
    f.vm.tilemaps=NULL; f.vm.n_tilemaps=0;
    f.vm.rtl=NULL; f.vm.n_rtl=0;
    f.vm.room_index=-1;
    f.vm.room_stored=calloc(2,sizeof *f.vm.room_stored);
    f.vm.room_state_count=2;
    f.vm.particles=gml_particle_state_create(&f.vm);
    gml_vm_software3d_reset(&f.vm);
    if(!f.vm.room_stored || !f.vm.particles){ cleanup(&f); return 0; }
    gml_room_enter(&f.vm,0);
    GmlVal args[]={vreal(0),vreal(2),vreal(0),vreal(2),vreal(2),vreal(8),vreal(5),vreal(17)};
    GmlVal tile=gml_builtin_call(&f.vm,"tile_add",args,8);
    GmlVal authored[]={vreal(10000000),vreal(64.0/255.0)},dynamic[]={tile,vreal(192.0/255.0)};
    ok &= legacy_tile_pixels(&f,255,255);
    ok &= call(&f,"tile_set_alpha",authored,2,cached);
    ok &= call(&f,"tile_set_alpha",dynamic,2,cached);
    ok &= legacy_tile_pixels(&f,64,192);
    authored[1]=dynamic[1]=vreal(0);
    ok &= call(&f,"tile_set_visible",authored,2,cached);
    ok &= call(&f,"tile_set_visible",dynamic,2,cached);
    ok &= legacy_tile_pixels(&f,0,0);
    gml_room_enter(&f.vm,1);
    ok &= f.vm.room_index==1 && legacy_tile_pixels(&f,0,0);
    gml_room_enter(&f.vm,0);
    ok &= f.vm.room_index==0 && legacy_tile_pixels(&f,0,0);
    authored[1]=dynamic[1]=vreal(1);
    ok &= call(&f,"tile_set_visible",authored,2,cached);
    ok &= call(&f,"tile_set_visible",dynamic,2,cached);
    ok &= legacy_tile_pixels(&f,64,192);

    size_t size=gml_vm_state_size(&f.vm),written=0,used=0;
    void *state=malloc(size);
    int saved=state && gml_vm_state_save(&f.vm,state,size,&written);
    authored[1]=dynamic[1]=vreal(0);
    call(&f,"tile_set_alpha",authored,2,cached);
    call(&f,"tile_set_alpha",dynamic,2,cached);
    ok &= legacy_tile_pixels(&f,0,0);
    ok &= saved && gml_vm_state_load(&f.vm,state,written,&used) && used==written;
    ok &= legacy_tile_pixels(&f,64,192);
    free(state);
    *gml_varmap_put(&f.vm.globals,"room_persistent")=vreal(0);
    gml_room_enter(&f.vm,1);
    gml_room_enter(&f.vm,0);
    ok &= f.vm.room_index==0 && legacy_tile_pixels(&f,255,0);
    free(f.vm.rtl);
    cleanup(&f);
  }
  if(ok) puts("legacy tile appearance: alpha, visibility, room lifecycle and restore passed");
  return ok;
}
static int modern_dynamic_tile_resource(void){
  int ok=1;
  for(int revision=15;revision<=17;revision+=2){
    TileFixture f; setup(&f);
    AnygmCompatibilityProfile profile={.has_modern_layer_semantics=1};
    GmlWin win={.bytecode=revision,.compatibility=&profile};
    int frame=0;
    GmlSprite sprite={.w=6,.h=2,.n_frames=1,.frame=&frame};
    f.vm.win=&win; f.render.win=&win;
    f.vm.tilemaps=NULL; f.vm.n_tilemaps=0;
    f.render.spr=&sprite; f.render.n_spr=1;
    f.render.bg=NULL; f.render.n_bg=0;
    f.layer.visible=1; f.layer.x=f.layer.y=0;
    f.layer.script_begin=f.layer.script_end=-1;
    GmlVal args[]={vreal(70),vreal(3),vreal(5),vreal(0),
                   vreal(2),vreal(0),vreal(2),vreal(2)};
    GmlVal tile=gml_builtin_call(&f.vm,"layer_tile_create",args,8);
    ok &= gml_builtin_call(&f.vm,"layer_tile_exists",&tile,1).d==1;
    gml_vm_draw(&f.vm); gml_render_flush_rotated_batch(&f.render);
    ok &= region(&f,3,5,colors);
    f.vm.win=NULL; f.render.win=NULL; cleanup(&f);
  }
  return ok;
}
static int asset_tile_pixels(TileFixture *f,int x,int y,unsigned red){
  gml_render_set_pending_fill(&f->render,0xff000000u);
  gml_vm_draw(&f->vm); gml_render_flush_rotated_batch(&f->render);
  gml_render_flush_pending_fill(&f->render);
  uint32_t colour=0xff000000u|(red<<16);
  for(int yy=0;yy<16;yy++) for(int xx=0;xx<16;xx++){
    uint32_t want=xx>=x && xx<x+2 && yy>=y && yy<y+2?colour:0xff000000u;
    if(f->pixels[yy*16+xx]!=want){
      fprintf(stderr,"asset tile (%d,%d): %08x != %08x\n",xx,yy,f->pixels[yy*16+xx],want);
      return 0;
    }
  }
  return 1;
}
static int authored_asset_tile_handles(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    TileFixture f; setup(&f);
    uint8_t data[640]={0};
    char *strings[]={"room","empty","assets"}; uint32_t offsets[]={560,576,592};
    for(int i=0;i<3;i++) memcpy(data+offsets[i],strings[i],strlen(strings[i])+1);
    GmlWin win={.data=data,.size=sizeof data,.bytecode=17,.n_chunks=1,
                .strs=strings,.str_charoff=offsets,.n_strs=3};
    win.chunks[0]=(GmlChunk){"ROOM",16,544};
    word(data+16,2); word(data+20,32); word(data+24,136);
    for(int room=0;room<2;room++){
      int at=32+104*room;
      word(data+at,offsets[room]); word(data+at+8,16); word(data+at+12,16);
      word(data+at+16,60);
    }
    word(data+52,1); word(data+120,240);
    word(data+240,1); word(data+244,256);
    word(data+256,592); word(data+264,3); word(data+268,17);
    word(data+272,0x40000000u); word(data+276,0x40400000u); word(data+288,1);
    word(data+292,320); word(data+320,3); word(data+328,352); word(data+332,620);
    word(data+352,1); word(data+356,2); word(data+364,2);
    word(data+372,2); word(data+376,2);
    word(data+388,0x3f800000u); word(data+392,0x3f800000u); word(data+396,0x80ffffffu);
    int frame=0; GmlSprite sprite={.w=6,.h=2,.n_frames=1,.frame=&frame};
    for(int y=0;y<2;y++) for(int x=2;x<4;x++){
      int at=(y*6+x)*4; f.rgba[at]=255; f.rgba[at+1]=f.rgba[at+2]=0;
    }
    f.vm.win=&win; f.render.win=&win; f.render.spr=&sprite; f.render.n_spr=1;
    f.vm.tilemaps=NULL; f.vm.n_tilemaps=0; f.vm.rtl=NULL; f.vm.n_rtl=0;
    f.vm.room_index=-1; f.vm.room_state_count=2;
    f.vm.room_stored=calloc(2,sizeof *f.vm.room_stored);
    f.vm.particles=gml_particle_state_create(&f.vm); gml_vm_software3d_reset(&f.vm);
    if(!f.vm.room_stored || !f.vm.particles){ cleanup(&f); return 0; }
    gml_room_enter(&f.vm,0);
    ok &= asset_tile_pixels(&f,3,5,128);
    GmlVal source=vreal(f.vm.rtl[0].id);
    int enumerate=gml_builtin_fast_id(&f.vm,"layer_get_all_elements");
    GmlVal elements=cached?gml_builtin_call_fast_id(&f.vm,enumerate,"layer_get_all_elements",&source,1):
                           gml_builtin_call(&f.vm,"layer_get_all_elements",&source,1);
    ok &= gml_val_array_length(elements)==1;
    GmlVal tile=gml_arr_get(elements,0); gml_values_release_owned(&elements,1);
    ok &= tile.t==V_REAL && gml_builtin_call(&f.vm,"layer_get_element_type",&tile,1).d==7;
    ok &= gml_builtin_call(&f.vm,"layer_tile_get_x",&tile,1).d==1;
    ok &= gml_builtin_call(&f.vm,"layer_tile_get_alpha",&tile,1).d==128.0/255.0;
    elements=gml_builtin_call(&f.vm,"layer_get_all_elements",&source,1);
    ok &= gml_val_array_length(elements)==1 && gml_arr_get(elements,0).d==tile.d && f.vm.n_rte==1;
    gml_values_release_owned(&elements,1);
    ok &= asset_tile_pixels(&f,3,5,128);
    GmlRtLayer *destination=gml_rt_layer_new(&f.vm);
    if(!destination){ free(f.vm.rtl); cleanup(&f); return 0; }
    destination->x=8; destination->y=5; strcpy(destination->name,"destination");
    GmlVal move[]={tile,cached?vreal(destination->id):vstr("destination")};
    ok &= call(&f,"layer_element_move",move,2,cached) && asset_tile_pixels(&f,9,7,128);
    size_t size=gml_vm_state_size(&f.vm),written=0,used=0;
    void *state=malloc(size);
    int saved=state && gml_vm_state_save(&f.vm,state,size,&written);
    call(&f,"layer_tile_destroy",&tile,1,cached);
    ok &= asset_tile_pixels(&f,9,7,0);
    ok &= saved && gml_vm_state_load(&f.vm,state,written,&used) && used==written;
    free(state);
    ok &= asset_tile_pixels(&f,9,7,128);
    gml_room_enter(&f.vm,1); gml_room_enter(&f.vm,0);
    ok &= asset_tile_pixels(&f,9,7,128);
    call(&f,"layer_tile_destroy",&tile,1,cached);
    GmlVal create[]={source,vreal(6),vreal(8),vreal(0),vreal(2),vreal(0),vreal(2),vreal(2)};
    GmlVal replacement=gml_builtin_call(&f.vm,"layer_tile_create",create,8);
    ok &= replacement.d!=tile.d && f.vm.n_rte==1 && asset_tile_pixels(&f,8,11,255);
    *gml_varmap_put(&f.vm.globals,"room_persistent")=vreal(0);
    gml_room_enter(&f.vm,1); gml_room_enter(&f.vm,0);
    ok &= f.vm.n_rte==0 && asset_tile_pixels(&f,3,5,128);
    free(f.vm.rtl); cleanup(&f);
  }
  if(ok) puts("asset tile handles: enumeration, single drawing, movement, restore, persistence and slot reuse passed");
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
    {"static_room_tile_scale",static_room_tile_scale},
    {"local_map_position",local_map_position},{"moved_tilemap_draw",moved_tilemap_draw},
    {"moved_authored_background",moved_authored_background},
    {"prepared_layouts",prepared_layouts},{"scaled_map",scaled_map},
    {"bounded_empty_maps",bounded_empty_maps},{"rectangular_tile_extent",rectangular_tile_extent},
    {"legacy_dynamic_tiles",legacy_dynamic_tiles},
    {"legacy_dynamic_tile_scale",legacy_dynamic_tile_scale},
    {"legacy_tile_queries",legacy_tile_queries},
    {"legacy_tile_appearance",legacy_tile_appearance},
    {"modern_dynamic_tile_resource",modern_dynamic_tile_resource},
    {"authored_asset_tile_handles",authored_asset_tile_handles}};
  const AnygmTestGroup group={"tile_draw",cases,sizeof cases/sizeof cases[0]};
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2]; else if(argc!=1) return EXIT_FAILURE;
  AnygmTestResult result; anygm_test_run_groups(&group,1,filter,&result);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
