/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"
#include "gml_image_codec.h"
#include "gml_vm_internal.h"


void free_extension_fixture(GmlVM *vm){
  if(!vm) return;
  gml_colgrid_invalidate(vm);
  free(vm->cg_off);
  free(vm->cg_items);
  free(vm->cg_overlay);
  free(vm->inst);
  vm->cg_off=NULL; vm->cg_items=NULL; vm->cg_overlay=NULL; vm->inst=NULL;
}


void store_u32le(uint8_t *dst,uint32_t value){
  dst[0]=(uint8_t)value;
  dst[1]=(uint8_t)(value>>8);
  dst[2]=(uint8_t)(value>>16);
  dst[3]=(uint8_t)(value>>24);
}

typedef struct {
  const uint8_t *bytes;
  size_t size,position;
  int path_matched;
  unsigned opens,closes;
} ExternalTextureHost;


static void *external_texture_open(void *userdata,const char *path,AnygmFileMode mode){
  ExternalTextureHost *host=userdata;
  if(mode!=ANYGM_FILE_READ ||
     strcmp(path,"root/pack/neutral_group_0.yytex")) return NULL;
  host->position=0;
  host->path_matched=1;
  host->opens++;
  return host;
}


static size_t external_texture_read(void *userdata,void *file,void *data,size_t size){
  ExternalTextureHost *host=userdata;
  (void)file;
  size_t available=host->size-host->position;
  if(size>available) size=available;
  if(size) memcpy(data,host->bytes+host->position,size);
  host->position+=size;
  return size;
}


static int64_t external_texture_seek(void *userdata,void *file,int64_t offset,
                                     AnygmSeekOrigin origin){
  ExternalTextureHost *host=userdata;
  (void)file;
  int64_t base=origin==ANYGM_SEEK_START?0:
               origin==ANYGM_SEEK_CURRENT?(int64_t)host->position:
               origin==ANYGM_SEEK_END?(int64_t)host->size:-1;
  if(base<0 || offset < -base || (uint64_t)(base+offset)>host->size) return -1;
  host->position=(size_t)(base+offset);
  return base+offset;
}


static void external_texture_close(void *userdata,void *file){
  ExternalTextureHost *host=userdata;
  (void)file;
  host->closes++;
}


int external_texture_group_fixture(void){
  static const uint8_t encoded[]={
    'f','i','o','q',1,0,1,0,0,0,0,0,0xfe,17,34,51
  };
  uint8_t data[512]={0};
  char *strings[]={"neutral_group","pack",".yytex"};
  uint32_t string_offsets[]={400,404,408};
  GmlWin win={0};
  GmlRender render={0};
  ExternalTextureHost memory={
    .bytes=encoded,
    .size=sizeof encoded
  };
  AnygmHostServices host={
    .struct_size=sizeof host,
    .abi_version=ANYGM_HOST_SERVICES_VERSION,
    .userdata=&memory,
    .file_open=external_texture_open,
    .file_read=external_texture_read,
    .file_seek=external_texture_seek,
    .file_close=external_texture_close
  };

  win.data=data;
  win.size=sizeof data;
  win.strs=strings;
  win.str_charoff=string_offsets;
  win.n_strs=3;
  win.host=&host;
  snprintf(win.content_dir,sizeof win.content_dir,"root");
  memcpy(win.chunks[0].name,"TXTR",4);
  win.chunks[0].off=0;
  win.chunks[0].size=48;
  memcpy(win.chunks[1].name,"TGIN",4);
  win.chunks[1].off=64;
  win.chunks[1].size=80;
  win.n_chunks=2;

  store_u32le(data+0,1);             /* TXTR count */
  store_u32le(data+4,16);            /* texture record */
  store_u32le(data+16+8,sizeof encoded);
  store_u32le(data+16+12,1);         /* width */
  store_u32le(data+16+16,1);         /* height */
  store_u32le(data+16+20,0);         /* index in group */
  store_u32le(data+16+24,0);         /* external blob */

  store_u32le(data+64,1);            /* TGIN format */
  store_u32le(data+68,1);            /* group count */
  store_u32le(data+72,80);           /* group record */
  store_u32le(data+80,400);          /* group name */
  store_u32le(data+84,404);          /* directory */
  store_u32le(data+88,408);          /* extension */
  store_u32le(data+92,2);            /* separate textures */
  store_u32le(data+96,112);          /* texture-page list */
  store_u32le(data+112,1);
  store_u32le(data+116,0);

  render.win=&win;
  parse_txtr(&render);
  uint8_t *pixels=gml_render_warm_atlas(&render,0)?atlas_pixels(&render,0):NULL;
  int ok=render.n_atlas==1 && render.atlas && render.atlas[0].external_blob &&
         render.atlas[0].external_size==sizeof encoded &&
         render.atlas[0].w==1 && render.atlas[0].h==1 &&
         pixels && pixels[0]==17 && pixels[1]==34 && pixels[2]==51 && pixels[3]==255 &&
         memory.path_matched && memory.opens==1 && memory.closes==1;
  gml_render_free(&render);
  if(!ok){
    fprintf(stderr,"external texture group fixture load mismatch\n");
    return 0;
  }

  strings[1]="../pack";
  GmlRender traversal={0};
  traversal.win=&win;
  parse_txtr(&traversal);
  int traversal_rejected=traversal.n_atlas==1 &&
                         !gml_render_warm_atlas(&traversal,0) &&
                         memory.opens==1 && memory.closes==1;
  gml_render_free(&traversal);
  if(!traversal_rejected){
    fprintf(stderr,"external texture group fixture accepted traversal\n");
    return 0;
  }

  strings[1]="pack";
  store_u32le(data+16+8,64u*1024u*1024u+1u);
  GmlRender oversized={0};
  oversized.win=&win;
  parse_txtr(&oversized);
  int oversized_rejected=oversized.n_atlas==1 &&
                         !gml_render_warm_atlas(&oversized,0) &&
                         memory.opens==1 && memory.closes==1;
  gml_render_free(&oversized);
  if(!oversized_rejected){
    fprintf(stderr,"external texture group fixture accepted oversized sidecar\n");
    return 0;
  }
  return 1;
}

typedef struct {
  const uint8_t *bytes;
  size_t size,position;
  unsigned opens,closes;
} RuntimeSpriteHost;


static void *runtime_sprite_open(void *userdata,const char *path,AnygmFileMode mode){
  RuntimeSpriteHost *host=userdata;
  if(mode!=ANYGM_FILE_READ || strcmp(path,"root/neutral_mask.png")) return NULL;
  host->position=0;
  host->opens++;
  return host;
}


static size_t runtime_sprite_read(void *userdata,void *file,void *data,size_t size){
  RuntimeSpriteHost *host=userdata;
  (void)file;
  size_t available=host->size-host->position;
  if(size>available) size=available;
  if(size) memcpy(data,host->bytes+host->position,size);
  host->position+=size;
  return size;
}


static int64_t runtime_sprite_seek(void *userdata,void *file,int64_t offset,
                                   AnygmSeekOrigin origin){
  RuntimeSpriteHost *host=userdata;
  (void)file;
  int64_t base=origin==ANYGM_SEEK_START?0:
               origin==ANYGM_SEEK_CURRENT?(int64_t)host->position:
               origin==ANYGM_SEEK_END?(int64_t)host->size:-1;
  if(base<0 || offset < -base || (uint64_t)(base+offset)>host->size) return -1;
  host->position=(size_t)(base+offset);
  return base+offset;
}


static void runtime_sprite_close(void *userdata,void *file){
  RuntimeSpriteHost *host=userdata;
  (void)file;
  host->closes++;
}


int classic_sprite_replace_fixture(void){
  static const uint8_t source_rgba[]={
    255,0,255,255, 255,0,255,255, 255,0,255,255,
    255,0,255,255,  17,34,51,255, 255,0,255,255,
    255,0,255,255, 255,0,255,255, 255,0,255,255
  };
  GmlMediaBuffer png={0};
  if(!gml_image_encode_png(source_rgba,3,3,4,12,&png)){
    fprintf(stderr,"classic sprite replacement PNG creation failed\n");
    return 0;
  }
  RuntimeSpriteHost memory={.bytes=png.data,.size=png.size};
  AnygmHostServices host={
    .struct_size=sizeof host,
    .abi_version=ANYGM_HOST_SERVICES_VERSION,
    .userdata=&memory,
    .file_open=runtime_sprite_open,
    .file_read=runtime_sprite_read,
    .file_seek=runtime_sprite_seek,
    .file_close=runtime_sprite_close
  };
  GmlWin win={0};
  GmlVM vm={0};
  GmlRender render={0};
  win.classic_version=800;
  win.host=&host;
  snprintf(win.content_dir,sizeof win.content_dir,"root");
  vm.win=&win;
  vm.host=&host;
  vm.render=&render;
  render.win=&win;
  uint8_t *base_rgba=malloc(4);
  if(!base_rgba){
    gml_media_buffer_release(&png);
    return 0;
  }
  memset(base_rgba,255,4);
  int sprite=gml_sprite_append_from_rgba(
      &render,base_rgba,1,1,0,0,"neutral_replace_target");
  GmlVal classic_args[]={
    vreal(sprite),vstr("neutral_mask.png"),vreal(1),
    vreal(1),vreal(1),vreal(0),vreal(1),vreal(6),vreal(7)
  };
  GmlVal classic_result=call_values(
      &vm,"sprite_replace",classic_args,
      (int)(sizeof classic_args/sizeof classic_args[0]));
  GmlRenderSpriteMetrics metrics={0};
  int ok=sprite>=0 && classic_result.t==V_REAL && classic_result.d==1 &&
         gml_render_sprite_metrics(&render,sprite,&metrics) &&
         metrics.width==3 && metrics.height==3 &&
         metrics.origin_x==6 && metrics.origin_y==7 &&
         metrics.collision_left==1 && metrics.collision_top==1 &&
         metrics.collision_right==1 && metrics.collision_bottom==1;
  if(!ok)
    fprintf(stderr,
            "classic sprite replacement signature mismatch: result=%g size=%dx%d "
            "origin=(%d,%d) bbox=(%d,%d,%d,%d)\n",
            classic_result.d,metrics.width,metrics.height,
            metrics.origin_x,metrics.origin_y,
            metrics.collision_left,metrics.collision_top,
            metrics.collision_right,metrics.collision_bottom);

  GmlVal compatible_classic_args[]={
    vreal(sprite),vstr("neutral_mask.png"),vreal(1),
    vreal(1),vreal(0),vreal(8),vreal(9)
  };
  GmlVal compatible_classic_result=call_values(
      &vm,"sprite_replace",compatible_classic_args,
      (int)(sizeof compatible_classic_args/sizeof compatible_classic_args[0]));
  memset(&metrics,0,sizeof metrics);
  int compatible_classic_ok=
      compatible_classic_result.t==V_REAL && compatible_classic_result.d==1 &&
      gml_render_sprite_metrics(&render,sprite,&metrics) &&
      metrics.origin_x==8 && metrics.origin_y==9 &&
      !gml_sprite_collision(&render,sprite,0,0,0) &&
      gml_sprite_collision(&render,sprite,0,1,1);
  ok=ok && compatible_classic_ok;
  if(!compatible_classic_ok)
    fprintf(stderr,
            "compatible classic sprite replacement signature mismatch: result=%g "
            "origin=(%d,%d) corner=%d centre=%d\n",
            compatible_classic_result.d,metrics.origin_x,metrics.origin_y,
            gml_sprite_collision(&render,sprite,0,0,0),
            gml_sprite_collision(&render,sprite,0,1,1));

  win.classic_version=0;
  GmlVal modern_args[]={
    vreal(sprite),vstr("neutral_mask.png"),vreal(1),
    vreal(1),vreal(0),vreal(4),vreal(5)
  };
  GmlVal modern_result=call_values(
      &vm,"sprite_replace",modern_args,
      (int)(sizeof modern_args/sizeof modern_args[0]));
  memset(&metrics,0,sizeof metrics);
  ok=ok && modern_result.t==V_REAL && modern_result.d==1 &&
     gml_render_sprite_metrics(&render,sprite,&metrics) &&
     metrics.origin_x==4 && metrics.origin_y==5 &&
     memory.opens==3 && memory.closes==3;
  if(modern_result.t!=V_REAL || modern_result.d!=1 ||
     metrics.origin_x!=4 || metrics.origin_y!=5 ||
     memory.opens!=3 || memory.closes!=3)
    fprintf(stderr,
            "modern sprite replacement signature changed: result=%g "
            "origin=(%d,%d) opens=%u closes=%u\n",
            modern_result.d,metrics.origin_x,metrics.origin_y,
            memory.opens,memory.closes);
  gml_vm_free(&vm);
  gml_render_free(&render);
  gml_media_buffer_release(&png);
  return ok;
}


int sprite_instance_metric_scale_fixture(void){
  GmlVM vm={0};
  GmlRender render={0};
  GmlInstance instance={0};
  uint8_t *rgba=calloc(5u*3u,4u);
  if(!rgba) return 0;
  int sprite=gml_sprite_append_from_rgba(
      &render,rgba,5,3,2,1,"neutral_metric_sprite");
  if(sprite<0){
    gml_render_free(&render);
    return 0;
  }
  vm.render=&render;
  instance.sprite_index=sprite;
  instance.image_xscale=-2.5;
  instance.image_yscale=4.0;
  double width=gml_inst_var_get(&vm,&instance,"sprite_width");
  double height=gml_inst_var_get(&vm,&instance,"sprite_height");
  double xoffset=gml_inst_var_get(&vm,&instance,"sprite_xoffset");
  double yoffset=gml_inst_var_get(&vm,&instance,"sprite_yoffset");
  int ok=width==12.5 && height==12.0 && xoffset==2.0 && yoffset==1.0;
  if(!ok)
    fprintf(stderr,
            "scaled sprite instance metrics mismatch: size=(%g,%g) origin=(%g,%g)\n",
            width,height,xoffset,yoffset);
  gml_render_free(&render);
  return ok;
}


size_t classic_information_record(uint8_t *dst,size_t capacity){
  const char caption[]="Information";
  const char text[]="{\\rtf1\\ansi\\pard\\qc\\b\\fs32 Generic information\\par\\b0\\fs24 Neutral \\ul underlined\\ulnone  fixture text\\par}";
  size_t need=8+4+sizeof(caption)-1+8*4+8+4+sizeof(text)-1;
  if(!dst || capacity<need) return 0;
  size_t at=0;
#define INFO_U32(v) do{ store_u32le(dst+at,(uint32_t)(v)); at+=4; }while(0)
  INFO_U32(0xFF000018u); INFO_U32(1);
  INFO_U32(sizeof(caption)-1); memcpy(dst+at,caption,sizeof(caption)-1); at+=sizeof(caption)-1;
  INFO_U32((uint32_t)-1); INFO_U32((uint32_t)-1); INFO_U32(600); INFO_U32(400);
  INFO_U32(1); INFO_U32(1); INFO_U32(0); INFO_U32(1);
  memset(dst+at,0,8); at+=8;
  INFO_U32(sizeof(text)-1); memcpy(dst+at,text,sizeof(text)-1); at+=sizeof(text)-1;
#undef INFO_U32
  return at;
}


static void add_named_asset_chunk(GmlWin *win,uint8_t *data,const char *tag,uint32_t base,
                                  uint32_t name_offset,int versioned){
  GmlChunk *chunk=&win->chunks[win->n_chunks++];
  memcpy(chunk->name,tag,4); chunk->name[4]=0;
  chunk->off=base; chunk->size=80;
  if(versioned){
    store_u32le(data+base,1);
    store_u32le(data+base+4,1);
    store_u32le(data+base+8,base+16);
  }else{
    store_u32le(data+base,1);
    store_u32le(data+base+4,base+16);
  }
  store_u32le(data+base+16,name_offset);
}


int asset_lookup_fixture(void){
  uint8_t data[1024]={0};
  char *names[]={
    "neutral_object", "neutral_sprite", "neutral_sound", "neutral_room",
    "neutral_path", "neutral_script", "neutral_font", "neutral_timeline",
    "neutral_shader", "neutral_sequence", "neutral_curve", "neutral_particles",
    "neutral_tileset"
  };
  uint32_t offsets[13];
  for(int i=0;i<13;i++) offsets[i]=800u+(uint32_t)i*4u;
  GmlWin win={0}; GmlVM vm={0}; GmlRender render={0};
  GmlSprite sprite={0}; GmlObject object={0}; GmlTimeline timeline={0};
  win.data=data; win.size=sizeof(data); win.strs=names; win.str_charoff=offsets; win.n_strs=13;
  add_named_asset_chunk(&win,data,"SOND",  0,offsets[2],0);
  add_named_asset_chunk(&win,data,"ROOM", 80,offsets[3],0);
  add_named_asset_chunk(&win,data,"PATH",160,offsets[4],0);
  add_named_asset_chunk(&win,data,"SCPT",240,offsets[5],0);
  add_named_asset_chunk(&win,data,"FONT",320,offsets[6],0);
  add_named_asset_chunk(&win,data,"SHDR",400,offsets[8],0);
  add_named_asset_chunk(&win,data,"SEQN",480,offsets[9],1);
  add_named_asset_chunk(&win,data,"ACRV",560,offsets[10],1);
  add_named_asset_chunk(&win,data,"PSYS",640,offsets[11],1);
  add_named_asset_chunk(&win,data,"BGND",720,offsets[12],0);
  sprite.name=names[1]; render.spr=&sprite; render.n_spr=1;
  object.name=names[0]; timeline.name=names[7];
  vm.win=&win; vm.render=&render; vm.objects=&object; vm.n_objects=1;
  vm.timelines=&timeline; vm.n_timelines=1;
  /* Path names belong to the loaded resource table, including deletion identity.
   * Load the authored record through its owner before querying that table. */
  gml_vm_paths_reset_authored(&vm);
  if(vm.n_paths!=1 || vm.n_authored_paths!=1 || !vm.paths[0].name ||
     strcmp(vm.paths[0].name,names[4])){
    fprintf(stderr,"asset lookup fixture did not load its authored path\n");
    gml_vm_paths_clear(&vm);
    return 0;
  }
  static const int expected_type[]={0,1,2,3,4,5,6,7,8,9,10,11,13};
  for(int i=0;i<13;i++){
    GmlVal name=vstr(names[i]);
    GmlVal index=call_values(&vm,"asset_get_index",&name,1);
    GmlVal type=call_values(&vm,"asset_get_type",&name,1);
    if(index.t!=V_REAL || index.d!=0 || type.t!=V_REAL || type.d!=expected_type[i]){
      fprintf(stderr,"asset lookup fixture mismatch at %d: index=%g type=%g\n",i,index.d,type.d);
      gml_vm_paths_clear(&vm);
      return 0;
    }
  }
  GmlVal absent=vstr("neutral_absent");
  if(call_values(&vm,"asset_get_index",&absent,1).d!=-1 ||
     call_values(&vm,"asset_get_type",&absent,1).d!=-1){
    fprintf(stderr,"asset lookup fixture missing-resource mismatch\n");
    gml_vm_paths_clear(&vm);
    return 0;
  }
  gml_vm_paths_clear(&vm);
  GmlGlyph font_glyphs[]={
    {.sx=2,.sy=3,.w=4,.h=5,.shift=6,.offset=1,.ch='A'},
    {.sx=6,.sy=3,.w=2,.h=5,.shift=3,.offset=0,.ch=' '}
  };
  uint8_t font_pixels[8*8*4]={0};
  GmlAtlas font_atlas={.px=font_pixels,.w=8,.h=8,.decode_attempted=1};
  render.atlas=&font_atlas; render.n_atlas=1;
  render.n_fonts=2;
  render.fonts[0].real=1; render.fonts[0].atlas=0; render.fonts[0].line_height=19;
  render.fonts[0].ascender=14; render.fonts[0].ascender_offset=3;
  render.fonts[0].sdf_spread=8;
  render.fonts[0].glyphs=font_glyphs; render.fonts[0].n_glyphs=2;
  render.fonts[1].sprite=0;
  /* Detach stack-backed fixture tables before APIs allocate VM structs. */
  vm.objects=NULL; vm.n_objects=0;
  vm.timelines=NULL; vm.n_timelines=0;
  GmlVal embedded_font=vreal(0),runtime_font=vreal(1);
  GmlVal default_font=vreal(-1),absent_font=vreal(2);
  if(call_values(&vm,"font_exists",&embedded_font,1).d!=1 ||
     call_values(&vm,"font_exists",&runtime_font,1).d!=1 ||
     call_values(&vm,"font_exists",&default_font,1).d!=0 ||
     call_values(&vm,"font_exists",&absent_font,1).d!=0){
    fprintf(stderr,"font existence fixture mismatch\n");
    return 0;
  }
  GmlVal font_name=call_values(&vm,"font_get_name",&embedded_font,1);
  GmlVal font_texture=call_values(&vm,"font_get_texture",&embedded_font,1);
  GmlVal font_uvs=call_values(&vm,"font_get_uvs",&embedded_font,1);
  GmlVal texture_width=call_values(&vm,"texture_get_width",&font_texture,1);
  GmlVal texel_width=call_values(&vm,"texture_get_texel_width",&font_texture,1);
  GmlVal texel_height=call_values(&vm,"texture_get_texel_height",&font_texture,1);
  GmlVal font_info=call_values(&vm,"font_get_info",&embedded_font,1);
  GmlInstance *info=gml_struct_find(&vm,(unsigned)font_info.d);
  GmlVal *glyphs_value=info?gml_varmap_get(&info->vars,"glyphs"):NULL;
  GmlInstance *glyphs=glyphs_value?gml_struct_find(&vm,(unsigned)glyphs_value->d):NULL;
  GmlVal *glyph_value=glyphs?gml_varmap_get(&glyphs->vars,"A"):NULL;
  GmlVal *space_value=glyphs?gml_varmap_get(&glyphs->vars," "):NULL;
  GmlInstance *glyph=glyph_value?gml_struct_find(&vm,(unsigned)glyph_value->d):NULL;
  GmlVal *character=glyph?gml_varmap_get(&glyph->vars,"char"):NULL;
  GmlVal *width=glyph?gml_varmap_get(&glyph->vars,"w"):NULL;
  GmlVal *ascender=info?gml_varmap_get(&info->vars,"ascender"):NULL;
  GmlVal *ascender_offset=info?gml_varmap_get(&info->vars,"ascenderOffset"):NULL;
  GmlVal *sdf_enabled=info?gml_varmap_get(&info->vars,"sdfEnabled"):NULL;
  GmlVal *sdf_spread=info?gml_varmap_get(&info->vars,"sdfSpread"):NULL;
  GmlRenderTextureMetrics texture_metrics={0};
  if(font_name.t!=V_STR || strcmp(font_name.s,"neutral_font") ||
     font_texture.t!=V_REAL ||
     !gml_render_texture_metrics(&render,(int)font_texture.d,&texture_metrics) ||
     texture_metrics.kind!=GML_RENDER_TEXTURE_FONT || texture_metrics.width!=8 ||
     font_uvs.t!=V_ARR || gml_val_array_length(font_uvs)!=4 ||
     gml_arr_get(font_uvs,0).d!=0 || gml_arr_get(font_uvs,2).d!=1 ||
     texture_width.d!=1 || texel_width.d!=0.125 || texel_height.d!=0.125 ||
     !info || !glyphs || !glyph || !space_value ||
     !character || character->d!='A' || !width || width->d!=4 ||
     !ascender || ascender->d!=14 || !ascender_offset || ascender_offset->d!=3 ||
     !sdf_enabled || sdf_enabled->d!=1 || !sdf_spread || sdf_spread->d!=8){
    fprintf(stderr,"font metadata fixture mismatch\n");
    gml_vm_free(&vm);
    return 0;
  }
  gml_font_delete(&render,1);
  if(call_values(&vm,"font_exists",&runtime_font,1).d!=0){
    fprintf(stderr,"deleted font remained live\n");
    gml_vm_free(&vm);
    return 0;
  }
  gml_vm_free(&vm);
  return 1;
}


int pushref_function_fixture(void){
  /* caller: pushref(callback), callv(), ret; callback: push 73, ret. The deliberately plain raw
   * reference proves that the FUNC occurrence, rather than its low bits, makes it callable. */
  GmlWin win={0}; GmlVM vm={0};
  win.bytecode=17;
  win.size=24;
  win.owns=1;
  win.data=calloc(win.size,1);
  win.n_code=2;
  win.code=calloc((size_t)win.n_code,sizeof(*win.code));
  win.n_refs=1;
  win.ref_addr=calloc(1,sizeof(*win.ref_addr));
  win.ref_name=calloc(1,sizeof(*win.ref_name));
  if(!win.data || !win.code || !win.ref_addr || !win.ref_name){
    gml_win_free(&win);
    fprintf(stderr,"pushref function fixture allocation failed\n");
    return 0;
  }
  store_u32le(win.data+0,0xFF02FFF5u);  /* break.i32 -11 */
  store_u32le(win.data+4,32u);          /* raw reference payload */
  store_u32le(win.data+8,0x99050000u);  /* callv.v 0 */
  store_u32le(win.data+12,0x9C050000u); /* ret.v */
  store_u32le(win.data+16,0x84000049u); /* push.e 73 */
  store_u32le(win.data+20,0x9C050000u); /* ret.v */
  win.code[0]=(GmlCode){.name="gml_Script_neutral_caller",.start=0,.length=16};
  win.code[1]=(GmlCode){.name="gml_Script_neutral_callback",.start=16,.length=8};
  win.ref_addr[0]=4;
  win.ref_name[0]=win.code[1].name;
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    fprintf(stderr,"pushref VM initialization failed\n");
    return 0;
  }
  GmlVal result=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  int ok=result.t==V_REAL && result.d==73;
  if(!ok) fprintf(stderr,"pushref function dispatch mismatch: %.17g\n",result.d);
  gml_vm_free(&vm);
  gml_win_free(&win);
  return ok;
}


int variable_hash_reference_fixture(void){
  /* A modern optimized member-name hash is serialized as a VARI reference on push.i32. Its raw
   * word is an occurrence-chain link, not the value that the VM must expose to hash APIs. */
  GmlWin win={0}; GmlVM vm={0};
  const char *name="neutral_field";
  win.bytecode=17; win.size=12; win.owns=1;
  win.data=calloc(win.size,1);
  win.n_code=1; win.code=calloc(1,sizeof(*win.code));
  win.n_refs=1; win.ref_addr=calloc(1,sizeof(*win.ref_addr));
  win.ref_name=calloc(1,sizeof(*win.ref_name));
  win.ref_kind=calloc(1,sizeof(*win.ref_kind));
  if(!win.data || !win.code || !win.ref_addr || !win.ref_name || !win.ref_kind){
    gml_win_free(&win);
    fprintf(stderr,"variable hash reference fixture allocation failed\n");
    return 0;
  }
  store_u32le(win.data+0,0xC0020000u);  /* push.i32 <VARI occurrence link> */
  store_u32le(win.data+4,0x00025D9Cu);
  store_u32le(win.data+8,0x9C050000u);  /* ret.v */
  win.code[0]=(GmlCode){.name="gml_Script_neutral_hash",.start=0,.length=12};
  win.ref_addr[0]=4; win.ref_name[0]=name; win.ref_kind[0]=GML_REF_VARIABLE;
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    fprintf(stderr,"variable hash VM initialization failed\n");
    return 0;
  }
  uint32_t expected=2166136261u;
  for(const unsigned char *cursor=(const unsigned char*)name;*cursor;cursor++){
    expected^=*cursor; expected*=16777619u;
  }
  GmlVal result=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  int ok=result.t==V_REAL && result.d==(double)expected;
  if(!ok) fprintf(stderr,"variable hash relocation mismatch: %.17g expected %u\n",
                  result.d,expected);
  gml_vm_free(&vm);
  gml_win_free(&win);
  return ok;
}


int member_function_self_fixture(void){
  /* A member field call keeps its receiver below the function value. The callback must observe
   * that receiver as self even when the field contains a plain function value rather than an
   * explicitly bound method struct. */
  GmlWin win={0}; GmlVM vm={0};
  win.bytecode=17;
  win.size=44;
  win.owns=1;
  win.data=calloc(win.size,1);
  win.n_code=2;
  win.code=calloc((size_t)win.n_code,sizeof(*win.code));
  win.n_refs=2;
  win.ref_addr=calloc((size_t)win.n_refs,sizeof(*win.ref_addr));
  win.ref_name=calloc((size_t)win.n_refs,sizeof(*win.ref_name));
  if(!win.data || !win.code || !win.ref_addr || !win.ref_name){
    gml_win_free(&win);
    fprintf(stderr,"member function fixture allocation failed\n");
    return 0;
  }
  win.code[0]=(GmlCode){.name="gml_Script_neutral_member_caller",.start=0,.length=32};
  win.code[1]=(GmlCode){.name="gml_Script_neutral_member_callback",.start=32,.length=12};
  win.ref_addr[0]=20; win.ref_name[0]="neutral_callback";
  win.ref_addr[1]=36; win.ref_name[1]="neutral_marker";
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    fprintf(stderr,"member function VM initialization failed\n");
    return 0;
  }
  GmlInstance *receiver=gml_struct_new(&vm);
  if(!receiver){
    gml_vm_free(&vm); gml_win_free(&win);
    fprintf(stderr,"member function fixture receiver allocation failed\n");
    return 0;
  }
  *gml_varmap_put(&receiver->vars,"neutral_callback")=vreal((double)(GML_FUNCVAL_TAG|1));
  *gml_varmap_put(&receiver->vars,"neutral_marker")=vreal(73);
  store_u32le(win.data+0,0xC0020000u);  /* push.i32 receiver */
  store_u32le(win.data+4,receiver->id);
  store_u32le(win.data+8,0x86058800u);  /* dup-swap receiver (zero args) */
  store_u32le(win.data+12,0x86050000u); /* retain receiver for StackTop field read */
  store_u32le(win.data+16,0xC005FFF7u); /* push.v stack.neutral_callback */
  store_u32le(win.data+20,0xA0000000u);
  store_u32le(win.data+24,0x99050000u); /* callv.v 0 */
  store_u32le(win.data+28,0x9C050000u); /* ret.v */
  store_u32le(win.data+32,0xC005FFFFu); /* push.v self.neutral_marker */
  store_u32le(win.data+36,0xA0000000u);
  store_u32le(win.data+40,0x9C050000u); /* ret.v */
  GmlVal result=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  int ok=result.t==V_REAL && result.d==73;
  if(!ok) fprintf(stderr,"member function self dispatch mismatch: %.17g\n",result.d);
  gml_vm_free(&vm);
  gml_win_free(&win);
  return ok;
}


int member_function_argument_fixture(void){
  /* The implicit-self member form keeps arguments below @@This@@ and the function value:
   * [arg0, receiver, callback]. Verify that callv removes the receiver before collecting args. */
  GmlWin win={0}; GmlVM vm={0};
  win.bytecode=17;
  win.size=40;
  win.owns=1;
  win.data=calloc(win.size,1);
  win.n_code=2;
  win.code=calloc((size_t)win.n_code,sizeof(*win.code));
  win.n_refs=3;
  win.ref_addr=calloc((size_t)win.n_refs,sizeof(*win.ref_addr));
  win.ref_name=calloc((size_t)win.n_refs,sizeof(*win.ref_name));
  if(!win.data || !win.code || !win.ref_addr || !win.ref_name){
    gml_win_free(&win);
    fprintf(stderr,"member function argument fixture allocation failed\n");
    return 0;
  }
  win.code[0]=(GmlCode){.name="gml_Script_neutral_member_argument_caller",.start=0,.length=28};
  win.code[1]=(GmlCode){.name="gml_Script_neutral_member_argument_callback",.start=28,.length=12};
  win.ref_addr[0]=8;  win.ref_name[0]="@@This@@";
  win.ref_addr[1]=16; win.ref_name[1]="neutral_callback";
  win.ref_addr[2]=32; win.ref_name[2]="argument0";
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    fprintf(stderr,"member argument VM initialization failed\n");
    return 0;
  }
  GmlInstance *receiver=gml_struct_new(&vm);
  if(!receiver){
    gml_vm_free(&vm); gml_win_free(&win);
    fprintf(stderr,"member function argument fixture receiver allocation failed\n");
    return 0;
  }
  *gml_varmap_put(&receiver->vars,"neutral_callback")=vreal((double)(GML_FUNCVAL_TAG|1));
  store_u32le(win.data+0,0x84000005u);  /* push.e 5 (arg0) */
  store_u32le(win.data+4,0xD9020000u);  /* call @@This@@(0) */
  store_u32le(win.data+8,0u);
  store_u32le(win.data+12,0xC005FFFAu); /* push.v builtin.neutral_callback */
  store_u32le(win.data+16,0xA0000000u);
  store_u32le(win.data+20,0x99050001u); /* callv.v 1 */
  store_u32le(win.data+24,0x9C050000u); /* ret.v */
  store_u32le(win.data+28,0xC305FFFAu); /* push.v builtin.argument0 */
  store_u32le(win.data+32,0xA0000000u);
  store_u32le(win.data+36,0x9C050000u); /* ret.v */
  GmlVal result=gml_vm_run_code(&vm,0,receiver,receiver,NULL,0);
  int ok=result.t==V_REAL && result.d==5;
  if(!ok) fprintf(stderr,"member function argument dispatch mismatch: %.17g\n",result.d);
  gml_vm_free(&vm);
  gml_win_free(&win);
  return ok;
}
