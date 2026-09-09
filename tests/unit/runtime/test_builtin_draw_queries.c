/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_render_internal.h"
#include "anygm_test_runner.h"
#include "memory_vfs.h"
#include "../media/font_test_fixture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GmlVal query(GmlVM *vm,const char *name,GmlVal *args,int count,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  if(id<0){ *ok=0; return vundef(); }
  return gml_builtin_call_fast_id(vm,id,name,args,count);
}
static int real_is(GmlVal v,double number){ return v.t==V_REAL && v.d==number; }
static int string_is(GmlVal v,const char *text){
  int ok=v.t==V_STR && v.s && !strcmp(v.s,text);
  if(v.t==V_STR && v.d!=0) free((void *)v.s);
  else gml_values_release(&v,1);
  return ok;
}
static int circle_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlRender render={0}; GmlVM vm={.render=&render};
    int values[]={4,12,32,64};
    for(int i=0;i<4;i++){
      GmlVal arg=vreal(values[i]);
      gml_builtin_call(&vm,"draw_set_circle_precision",&arg,1);
      ok &= real_is(query(&vm,"draw_get_circle_precision",NULL,0,cached,&ok),values[i]);
      GmlRenderDrawState state;
      gml_render_draw_state_get(&render,&state);
      ok &= state.circle_precision==values[i];
    }
    vm.render=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static void word(uint8_t *data,int offset,uint32_t value){
  for(int i=0;i<4;i++) data[offset+i]=(uint8_t)(value>>(i*8));
}
static int shader_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    uint8_t bytes[64]={0};
    char first[]="shade_first",second[]="shade_second";
    char *names[]={first,second}; uint32_t offsets[]={40,52};
    GmlWin win={.data=bytes,.size=sizeof bytes,.n_chunks=1,
      .strs=names,.str_charoff=offsets,.n_strs=2};
    win.chunks[0]=(GmlChunk){"SHDR",8,12};
    word(bytes,8,2); word(bytes,12,24); word(bytes,16,28);
    word(bytes,24,40); word(bytes,28,52);
    GmlVM vm={.win=&win};
    GmlVal arg=vreal(0);
    GmlVal name=query(&vm,"shader_get_name",&arg,1,cached,&ok);
    first[0]='X';
    ok &= string_is(name,"shade_first"); /* own the returned name, not borrowed content */
    arg=vreal(1); ok &= string_is(query(&vm,"shader_get_name",&arg,1,cached,&ok),second);
    double bad[]={-1,2,NAN,INFINITY,1e100};
    for(int i=0;i<5;i++){
      arg=vreal(bad[i]); ok &= string_is(query(&vm,"shader_get_name",&arg,1,cached,&ok),"");
    }
    vm.win=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static int depth_matches(GmlVal result,const int *ids,int count){
  GmlArr *array=result.arr;
  int ok=result.t==V_ARR && array && gml_val_array_length(result)==(count?count:1);
  if(ok && !count) ok=real_is(array->data[0],-1);
  for(int i=0;ok && i<count;i++){
    int found=0;
    for(int j=0;j<count;j++) found+=real_is(array->data[j],ids[i]);
    ok &= found==1;
  }
  gml_values_release(&result,1); return ok;
}
static int layers_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0}; GmlVal args[]={vreal(12.5),vstr("first")};
    GmlVal first=gml_builtin_call(&vm,"layer_create",args,2);
    args[1]=vstr("second"); GmlVal second=gml_builtin_call(&vm,"layer_create",args,2);
    args[0]=vreal(-20); args[1]=vstr("other"); gml_builtin_call(&vm,"layer_create",args,2);
    int ids[]={(int)first.d,(int)second.d}; GmlVal depth=vreal(12.5);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),ids,2);
    GmlVal move[]={first,vreal(99)}; gml_builtin_call(&vm,"layer_depth",move,2);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),ids+1,1);
    gml_builtin_call(&vm,"layer_destroy",&second,1);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),NULL,0);
    depth=vreal(NAN);
    ok &= depth_matches(query(&vm,"layer_get_id_at_depth",&depth,1,cached,&ok),NULL,0);
    gml_vm_free(&vm);
  }
  return ok;
}
static int alpha_case(void){
  GmlRender render={0}; GmlVM vm={.render=&render}; int ok=1;
  for(int enabled=1;enabled>=0;enabled--){
    GmlVal arg=vreal(enabled);
    gml_builtin_call(&vm,"draw_set_alpha_test",&arg,1);
    ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestenable",NULL,0),enabled);
  }
  int references[]={17,128,255,0};
  for(int i=0;i<4;i++){
    GmlVal arg=vreal(references[i]);
    gml_builtin_call(&vm,"draw_set_alpha_test_ref_value",&arg,1);
    ok &= real_is(gml_builtin_call(&vm,"gpu_get_alphatestref",NULL,0),references[i]);
  }
  ok &= gml_builtin_fast_id(&vm,"draw_set_alpha_test")==-1 &&
        gml_builtin_fast_id(&vm,"draw_set_alpha_test_ref_value")==-1;
  vm.render=NULL; gml_vm_free(&vm); return ok;
}
static int background_names_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlBg backgrounds[2]={{.tpag=-1,.name="neutral_background_a"},
                          {.tpag=-1,.name="neutral_background_b"}};
    GmlRender render={.bg=backgrounds,.n_bg=2}; GmlVM vm={.render=&render};
    for(int i=0;i<2;i++){
      GmlVal arg=vreal(i);
      ok &= string_is(query(&vm,"background_name",&arg,1,cached,&ok),backgrounds[i].name);
      ok &= string_is(query(&vm,"background_get_name",&arg,1,cached,&ok),backgrounds[i].name);
    }
    const double invalid[]={-1,2,INFINITY,-INFINITY,NAN,1e100};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal arg=vreal(invalid[i]);
      ok &= string_is(query(&vm,"background_name",&arg,1,cached,&ok),"");
      ok &= string_is(query(&vm,"background_get_name",&arg,1,cached,&ok),"");
    }
    /* Keep the existing numeric-index truncation and missing-index fallback. */
    GmlVal fractional=vreal(0.75);
    ok &= string_is(query(&vm,"background_name",&fractional,1,cached,&ok),backgrounds[0].name);
    ok &= string_is(query(&vm,"background_get_name",&fractional,1,cached,&ok),backgrounds[0].name);
    ok &= string_is(query(&vm,"background_name",NULL,0,cached,&ok),backgrounds[0].name);
    ok &= string_is(query(&vm,"background_get_name",NULL,0,cached,&ok),backgrounds[0].name);
    ok &= !strcmp(backgrounds[0].name,"neutral_background_a") &&
          !strcmp(backgrounds[1].name,"neutral_background_b");
    vm.render=NULL; gml_vm_free(&vm);
  }
  return ok;
}
static int background_part_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlWin win={0}; GmlRender render;
    if(gml_render_init(&render,&win)!=0) return 0;
    int atlas_id=render.n_atlas;
    GmlAtlas *atlases=realloc(render.atlas,(size_t)(atlas_id+1)*sizeof *atlases);
    if(!atlases) abort();
    render.atlas=atlases; render.n_atlas++;
    memset(&render.atlas[atlas_id],0,sizeof render.atlas[atlas_id]);
    render.tpag=calloc(1,sizeof *render.tpag); render.n_tpag=1;
    render.bg=calloc(1,sizeof *render.bg); render.n_bg=1;
    if(!render.atlas || !render.tpag || !render.bg) abort();
    render.atlas[atlas_id].px=malloc(4*4*4);
    render.atlas[atlas_id].w=render.atlas[atlas_id].h=4;
    if(!render.atlas[atlas_id].px) abort();
    for(int y=0;y<4;y++) for(int x=0;x<4;x++){
      uint8_t *pixel=render.atlas[atlas_id].px+(y*4+x)*4;
      pixel[0]=(uint8_t)(x*20+7); pixel[1]=(uint8_t)(y*30+11); pixel[2]=13; pixel[3]=255;
    }
    render.tpag[0]=(GmlTpag){.sw=4,.sh=4,.tw=4,.th=4,.bw=4,.bh=4,.atlas=atlas_id};
    uint32_t frame[16*16]={0};
    gml_render_begin(&render,frame,16,16,0,0);
    GmlVM vm={.render=&render};
    GmlVal args[]={vreal(0),vreal(1),vreal(2),vreal(2),vreal(1),vreal(5),vreal(6)};
    query(&vm,"draw_background_part",args,7,cached,&ok);
    ok &= (frame[6*16+5]&0xFFFFFFu)==0x1B470Du &&
          (frame[6*16+6]&0xFFFFFFu)==0x2F470Du && (frame[6*16+7]&0xFFFFFFu)==0;
    vm.render=NULL; gml_vm_free(&vm); gml_render_free(&render);
  }
  return ok;
}
static int font_styles_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    uint8_t bytes[576]={0};
    GmlWin win={.data=bytes,.size=sizeof bytes,.bytecode=15,.n_chunks=1};
    win.chunks[0]=(GmlChunk){"FONT",8,24};
    word(bytes,8,4);
    for(int i=0;i<4;i++){
      int record=64+i*96,glyph=record+60;
      word(bytes,12+i*4,(uint32_t)record);
      word(bytes,record+8,12);
      word(bytes,record+12,(uint32_t)(i&1));
      word(bytes,record+16,(uint32_t)(i>>1));
      word(bytes,record+28,512);
      word(bytes,record+32,0x3F800000u); word(bytes,record+36,0x3F800000u);
      word(bytes,record+40,1); word(bytes,record+44,(uint32_t)glyph);
      word(bytes,glyph,'A'); word(bytes,glyph+4,2u<<16);
      word(bytes,glyph+8,3u|(4u<<16));
    }
    GmlRender render={.win=&win};
    parse_font(&render);
    GmlVM vm={.render=&render,.win=&win};
    for(int i=0;i<4;i++){
      GmlVal arg=vreal(i);
      ok &= real_is(query(&vm,"font_get_bold",&arg,1,cached,&ok),i&1);
      ok &= real_is(query(&vm,"font_get_italic",&arg,1,cached,&ok),i>>1);
      render.font=i;
      ok &= gml_text_width(&render,"A")==4;
    }
    GmlVal fraction=vreal(1.75);
    ok &= real_is(query(&vm,"font_get_bold",&fraction,1,cached,&ok),1);
    const double invalid[]={-1,4,NAN,INFINITY,1e100};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal arg=vreal(invalid[i]);
      ok &= real_is(query(&vm,"font_get_bold",&arg,1,cached,&ok),0);
      ok &= real_is(query(&vm,"font_get_italic",&arg,1,cached,&ok),0);
    }
    ok &= real_is(query(&vm,"font_get_bold",NULL,0,cached,&ok),0);
    vm.render=NULL;
    GmlVal arg=vreal(1);
    ok &= real_is(query(&vm,"font_get_italic",&arg,1,cached,&ok),0);
    vm.win=NULL; gml_vm_free(&vm); gml_render_free(&render);
  }
  return ok;
}

static int runtime_font_styles_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    AnygmMemoryVfs *memory=calloc(1,sizeof(*memory));
    if(!memory) return 0;
    AnygmHostServices host={0}; anygm_memory_vfs_init(memory,&host);
    uint8_t bytes[552]; size_t size=font_fixture_build(bytes);
    ok &= anygm_memory_vfs_add_file(memory,"/content/fixture.ttf",bytes,size);
    GmlWin win={.host=&host}; snprintf(win.content_dir,sizeof win.content_dir,"/content");
    GmlRender render={.win=&win}; GmlVM vm={.win=&win,.host=&host,.render=&render};
    for(int i=0;ok && i<4;i++){
      GmlVal args[]={vstr("fixture.ttf"),vreal(32),vreal(i&1),vreal(i>>1),vreal(65),vreal(65)};
      GmlVal font=query(&vm,"font_add",args,6,cached,&ok);
      ok &= real_is(font,i) && real_is(query(&vm,"font_get_bold",&font,1,cached,&ok),i&1) &&
        real_is(query(&vm,"font_get_italic",&font,1,cached,&ok),i>>1);
      query(&vm,"font_delete",&font,1,cached,&ok);
      ok &= real_is(query(&vm,"font_get_bold",&font,1,cached,&ok),0) &&
        real_is(query(&vm,"font_get_italic",&font,1,cached,&ok),0);
    }
    render.n_spr=1; render.spr=calloc(1,sizeof(*render.spr));
    if(!render.spr) ok=0;
    if(ok){
      render.spr[0].n_frames=1;
      GmlVal args[]={vreal(0),vstr("A"),vreal(1),vreal(0)};
      GmlVal font=query(&vm,"font_add_sprite_ext",args,4,cached,&ok);
      ok &= real_is(font,4) && real_is(query(&vm,"font_get_bold",&font,1,cached,&ok),0) &&
        real_is(query(&vm,"font_get_italic",&font,1,cached,&ok),0);
    }
    const double invalid[]={NAN,INFINITY,-INFINITY,1e100};
    for(size_t i=0;ok && i<sizeof invalid/sizeof invalid[0];i++){
      GmlVal args[]={vstr("fixture.ttf"),vreal(32),vreal(1),vreal(1),vreal(invalid[i]),vreal(65)};
      ok &= real_is(query(&vm,"font_add",args,6,cached,&ok),-1) && render.n_fonts==5;
      args[4]=vreal(65); args[5]=vreal(invalid[i]);
      ok &= real_is(query(&vm,"font_add",args,6,cached,&ok),-1) && render.n_fonts==5;
      args[5]=vreal(65); args[1]=vreal(invalid[i]);
      ok &= real_is(query(&vm,"font_add",args,6,cached,&ok),-1) && render.n_fonts==5;
    }
    vm.render=NULL; vm.win=NULL; gml_vm_free(&vm); gml_render_free(&render);
    anygm_memory_vfs_destroy(memory); free(memory);
  }
  return ok;
}

int main(void){
  const AnygmTestCase cases[]={{"circle_precision",circle_case},{"shader_asset_names",shader_case},
    {"all_live_layers_at_depth",layers_case},{"retired_alpha_controls",alpha_case},
    {"background_source_region",background_part_case},{"background_name_alias",background_names_case},
    {"authored_font_styles",font_styles_case},{"runtime_font_styles",runtime_font_styles_case}};
  const AnygmTestGroup group={"draw_queries",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  printf("draw queries: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
