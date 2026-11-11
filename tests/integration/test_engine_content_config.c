/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "engine_internal.h"
#include "gml_hash.h"
#include "gml_image_codec.h"
#include "gml_builtin.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_text(const char *path,const char *text){
  FILE *file=fopen(path,"wb");
  assert(file && fwrite(text,1,strlen(text),file)==strlen(text));
  assert(!fclose(file));
}

static void write_bytes(const char *path,const void *bytes,size_t size){
  FILE *file=fopen(path,"wb");
  assert(file && fwrite(bytes,1,size,file)==size && !fclose(file));
}

static void frame(AnygmEngine *engine){
  AnygmInputFrame input={0}; AnygmFrameOutput output={0};
  input.struct_size=sizeof input; input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  assert(anygm_run_frame(engine,&input,&output)==ANYGM_OK);
}

static void merge_destinations(void){
  const char text[]="ostype|0\nintroskip|1\n$probe=1\n"
    "?gameres @window_w=64\n?aspect @window_w=128\n"
    "listset|global|items[0]|1|5\nlistset|global|items[1]|1|6\n"
    "call|configure\nmtoggle|Setting|global.flag|1\n"
    "ostype|6\n$probe=2\nintroskip|2\n?gameres @window_w=320\n"
    "listset|global|items[0]|1|9\ncall|configure\nmrange|Setting|global.flag|0|3|1\n"
    "ostype|1\n$probe=3\n";
  CheatSlot slots[GML_MAX_CHEATS]; int count=0; char error[256],effective[4096];
  assert(engine_boot_overrides_parse(text,slots,&count,error,sizeof error));
  assert(count==9 && engine_boot_overrides_text(slots,count,effective,sizeof effective));
  assert(strstr(effective,"ostype|1\n") && !strstr(effective,"ostype|6"));
  assert(strstr(effective,"$probe=3\n") && !strstr(effective,"$probe=1"));
  assert(strstr(effective,"?aspect @window_w=128") && strstr(effective,"?gameres @window_w=320"));
  assert(strstr(effective,"items[1]|1|6") && strstr(effective,"items[0]|1|9"));
  assert(strstr(effective,"mrange|Setting") && !strstr(effective,"mtoggle|Setting"));
  assert(!engine_boot_overrides_parse("$probe=1\nunknown|bad\n",slots,&count,error,sizeof error));
}

static void state_reserve_declarations(void){
  CheatSlot slots[GML_MAX_CHEATS];int count=0;char error[256];
  const char *invalid[]={"","0","-1","1.5","0x100000","1073741825",
                         "18446744073709551616","1junk"};
  char text[1024];
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
    snprintf(text,sizeof text,"statereserve|%s\n",invalid[i]);
    assert(!engine_boot_overrides_parse(text,slots,&count,error,sizeof error));
  }
  assert(engine_boot_overrides_parse("statereserve|1\nstatereserve|1073741824\n",
                                     slots,&count,error,sizeof error));
  assert(count==2);
  AnygmSyntheticContent fixture;assert(anygm_synthetic_content_create(&fixture));
  char anchor[256];
  snprintf(anchor,sizeof anchor,"%s/reserve.anygm",fixture.directory);
  const char *name=strrchr(fixture.path,'/');name=name?name+1:fixture.path;
  AnygmHostServices services={0};services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL;assert(anygm_create(&services,&engine)==ANYGM_OK);
  AnygmContentSource source={0};source.struct_size=sizeof source;
  /* Open the payload itself: the reserve must come from sibling discovery. */
  source.kind=ANYGM_CONTENT_PATH;source.path=fixture.path;
  source.system_directory=fixture.directory;source.cache_directory=fixture.directory;
  assert(anygm_state_capacity_reserve(NULL)==0 && anygm_state_capacity_reserve(engine)==0);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==0);
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "statereserve|4194304\nstatereserve|16777216\nstatereserve|1\n",name);
  write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  /* A later smaller declaration cannot withdraw the measured larger minimum. */
  assert(anygm_state_capacity_reserve(engine)==16u*1024u*1024u);
  frame(engine);
  size_t capacity=anygm_state_size(engine),written=0;
  uint8_t *state=malloc(capacity);assert(state);
  assert(anygm_state_save(engine,state,capacity,&written)==ANYGM_OK);
  assert(anygm_reset(engine)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==16u*1024u*1024u);
  AnygmConfigDelta delta={0};delta.struct_size=sizeof delta;
  delta.values.struct_size=sizeof delta.values;delta.fields=ANYGM_CONFIG_CONTENT_OVERRIDES;
  delta.values.content_overrides=0;assert(anygm_set_config(engine,&delta)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==0);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  delta.values.content_overrides=1;assert(anygm_set_config(engine,&delta)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==16u*1024u*1024u);
  anygm_unload(engine);assert(anygm_state_capacity_reserve(engine)==0);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "statereserve|8388608\nstatereserve|1\n",name);write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==8u*1024u*1024u);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==8u*1024u*1024u);
  anygm_unload(engine);
  assert(!remove(anchor));
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==0);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==0);
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "statereserve|4194304\n$semantic_marker=7\n",name);write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);frame(engine);
  free(state);capacity=anygm_state_size(engine);state=malloc(capacity);assert(state);
  assert(anygm_state_save(engine,state,capacity,&written)==ANYGM_OK);
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "statereserve|33554432\n$semantic_marker=7\n",name);write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==32u*1024u*1024u);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  assert(anygm_state_capacity_reserve(engine)==32u*1024u*1024u);
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "statereserve|4194304\n$semantic_marker=8\n",name);write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(anygm_state_load(engine,state,written)==ANYGM_ERROR_STATE_MISMATCH);
  anygm_destroy(engine);free(state);
  assert(!remove(anchor));anygm_synthetic_content_destroy(&fixture);
}

static void config_text(char *out,size_t capacity,const char *digest,int base,int selected){
  int length=snprintf(out,capacity,
    "[overrides]\nostype|0\n$anygm_probe=%d\n$retained=7\n"
    "[sha256:%s.overrides]\n$anygm_probe=%d\nostype|1\n",base,digest,selected);
  assert(length>0 && (size_t)length<capacity);
}

static void lifecycle(void){
  AnygmSyntheticContent fixture;
  assert(anygm_synthetic_content_create(&fixture));
  uint8_t *content=NULL,digest[32]; size_t content_size=0;
  assert(anygm_synthetic_content_read(&fixture,&content,&content_size));
  gml_sha256(content,content_size,digest);
  char hex[65],ini[256],anchor[256],text[1024];
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  snprintf(ini,sizeof ini,"%s/AnyGM.ini",fixture.directory);
  snprintf(anchor,sizeof anchor,"%s/content.anygm",fixture.directory);
  const char *name=strrchr(fixture.path,'/'); name=name?name+1:fixture.path;
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "$anygm_probe=2\nostype|6\n$anchor_only=9\ngamepad|keyboard\nkeybind|1|65|b\n",name);
  write_text(anchor,text);
  config_text(text,sizeof text,hex,1,3); write_text(ini,text);
  AnygmHostServices services={0}; services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL; assert(anygm_create(&services,&engine)==ANYGM_OK);
  AnygmContentSource source={0}; source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH; source.path=anchor;
  source.system_directory=fixture.directory; source.cache_directory=fixture.directory;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(engine->vm.os_type_declared==1);
  assert(engine_overrides_keyboard_emulation(engine));
  unsigned bound_port=ANYGM_MAX_GAMEPADS;
  assert(engine_overrides_key_binding(engine,65,&bound_port)==ANYGM_PAD_FACE_BOTTOM && bound_port==0);
  frame(engine);
  assert(gml_global_num(&engine->vm,"anygm_probe")==3);
  assert(gml_global_num(&engine->vm,"retained")==7);
  assert(gml_global_num(&engine->vm,"anchor_only")==9);
  assert(strstr(engine->launch_overrides_text,"$anygm_probe=2") &&
         !strstr(engine->launch_overrides_text,"$anygm_probe=3"));
  size_t capacity=anygm_state_size(engine),written=0;
  uint8_t *state=malloc(capacity),*before=malloc(capacity),*after=malloc(capacity);
  assert(state && before && after);
  assert(anygm_state_save(engine,state,capacity,&written)==ANYGM_OK);
  assert(anygm_reset(engine)==ANYGM_OK); frame(engine);
  assert(gml_global_num(&engine->vm,"anygm_probe")==3);
  assert(engine_overrides_keyboard_emulation(engine));
  assert(engine_overrides_key_binding(engine,65,NULL)==ANYGM_PAD_FACE_BOTTOM);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  assert(engine_overrides_keyboard_emulation(engine));
  assert(engine_overrides_key_binding(engine,65,NULL)==ANYGM_PAD_FACE_BOTTOM);

  /* A binding-only change also rejects the earlier identity without changing the live state. */
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "$anygm_probe=2\nostype|6\n$anchor_only=9\ngamepad|keyboard\nkeybind|1|65|a\n",name);
  write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(engine_overrides_key_binding(engine,65,NULL)==ANYGM_PAD_FACE_RIGHT);
  size_t binding_before=0,binding_after=0;
  assert(anygm_state_save(engine,before,capacity,&binding_before)==ANYGM_OK);
  assert(anygm_state_load(engine,state,written)==ANYGM_ERROR_STATE_MISMATCH);
  assert(anygm_state_save(engine,after,capacity,&binding_after)==ANYGM_OK);
  assert(binding_before==binding_after && !memcmp(before,after,binding_after));

  /* Changing only the source port is also an incompatible effective content identity. */
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "$anygm_probe=2\nostype|6\n$anchor_only=9\ngamepad|keyboard\nkeybind|2|65|b\n",name);
  write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(engine_overrides_key_binding(engine,65,&bound_port)==ANYGM_PAD_FACE_BOTTOM && bound_port==1);
  assert(anygm_state_save(engine,before,capacity,&binding_before)==ANYGM_OK);
  assert(anygm_state_load(engine,state,written)==ANYGM_ERROR_STATE_MISMATCH);
  assert(anygm_state_save(engine,after,capacity,&binding_after)==ANYGM_OK);
  assert(binding_before==binding_after && !memcmp(before,after,binding_after));

  /* The effective input declaration belongs to the existing content-state identity. */
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "$anygm_probe=2\nostype|6\n$anchor_only=9\ngamepad|auto\nkeybind|1|65|b\n",name);
  write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(!engine_overrides_keyboard_emulation(engine));
  assert(anygm_state_load(engine,state,written)==ANYGM_ERROR_STATE_MISMATCH);
  anygm_unload(engine);
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "$anygm_probe=2\nostype|6\n$anchor_only=9\ngamepad|keyboard\nkeybind|1|65|b\n",name);
  write_text(anchor,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);

  /* A hidden default has no part in the effective state identity. */
  anygm_unload(engine);
  config_text(text,sizeof text,hex,8,3); write_text(ini,text);
  source.path=fixture.path;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  anygm_unload(engine);
  config_text(text,sizeof text,hex,8,4); write_text(ini,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK); frame(engine);
  assert(gml_global_num(&engine->vm,"anygm_probe")==4);
  size_t previous_size=0,next_size=0;
  assert(anygm_state_save(engine,before,capacity,&previous_size)==ANYGM_OK);
  assert(anygm_state_load(engine,state,written)==ANYGM_ERROR_STATE_MISMATCH);
  assert(anygm_state_save(engine,after,capacity,&next_size)==ANYGM_OK);
  assert(previous_size==next_size && !memcmp(before,after,next_size));

  AnygmConfigDelta delta={0}; delta.struct_size=sizeof delta;
  delta.values.struct_size=sizeof delta.values; delta.fields=ANYGM_CONFIG_CONTENT_OVERRIDES;
  delta.values.content_overrides=0;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);
  assert(!engine_overrides_keyboard_emulation(engine));
  assert(engine_overrides_key_binding(engine,65,NULL)<0);
  assert(anygm_state_load(engine,state,written)==ANYGM_ERROR_STATE_MISMATCH);
  anygm_unload(engine);
  delta.values.content_overrides=1; assert(anygm_set_config(engine,&delta)==ANYGM_OK);
  hex[0]=hex[0]=='0'?'1':'0';
  config_text(text,sizeof text,hex,8,4); write_text(ini,text);
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK); frame(engine);
  assert(engine->vm.os_type_declared==6 && gml_global_num(&engine->vm,"anygm_probe")==2);
  anygm_unload(engine);
  gml_sha256(content,content_size,digest);
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  config_text(text,sizeof text,hex,1,3); write_text(ini,text);
  source.kind=ANYGM_CONTENT_MEMORY; source.data=content; source.size=content_size;
  source.path=NULL;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK); frame(engine);
  assert(engine->vm.os_type_declared==1 && gml_global_num(&engine->vm,"anygm_probe")==3);
  assert(!engine->launch_overrides_text[0]);
  anygm_unload(engine);
  write_text(ini,"[overrides]\nunknown|bad\n");
  assert(anygm_load(engine,&source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  anygm_destroy(engine);
  free(state); free(before); free(after); free(content);
  assert(!remove(anchor) && !remove(ini));
  anygm_synthetic_content_destroy(&fixture);
}

static void replacement(void){
  AnygmSyntheticContent fixture,child={0};
  assert(anygm_synthetic_game_change_content_create(&fixture));
  snprintf(child.path,sizeof child.path,"%s/secondary/data.win",fixture.directory);
  uint8_t *root_bytes=NULL,*child_bytes=NULL,digest[32]; size_t root_size=0,child_size=0;
  assert(anygm_synthetic_content_read(&fixture,&root_bytes,&root_size));
  assert(anygm_synthetic_content_read(&child,&child_bytes,&child_size));
  char root_hex[65],child_hex[65],ini[256],anchor[256],text[1024];
  gml_sha256(root_bytes,root_size,digest);
  for(size_t i=0;i<32;i++) snprintf(root_hex+i*2,3,"%02x",digest[i]);
  gml_sha256(child_bytes,child_size,digest);
  for(size_t i=0;i<32;i++) snprintf(child_hex+i*2,3,"%02x",digest[i]);
  assert(strcmp(root_hex,child_hex));
  snprintf(ini,sizeof ini,"%s/AnyGM.ini",fixture.directory);
  snprintf(anchor,sizeof anchor,"%s/launch.anygm",fixture.directory);
  snprintf(text,sizeof text,"[overrides]\n$default_marker=5\n"
    "[sha256:%s.overrides]\nostype|6\n$root_only=11\n"
    "[sha256:%s.overrides]\nostype|1\n$child_only=12\n",root_hex,child_hex);
  write_text(ini,text);
  write_text(anchor,"[anygm]\npayload=data.win\n[overrides]\n$launch_marker=7\n");
  AnygmHostServices services={0}; services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION; anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL; assert(anygm_create(&services,&engine)==ANYGM_OK);
  AnygmContentSource source={0}; source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH; source.path=anchor;
  source.system_directory=fixture.directory; source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK && engine->vm.os_type_declared==6);
  size_t root_capacity=anygm_state_size(engine),root_written=0;
  uint8_t *root_state=malloc(root_capacity); assert(root_state);
  assert(anygm_state_save(engine,root_state,root_capacity,&root_written)==ANYGM_OK);
  frame(engine); frame(engine);
  assert(engine->vm.os_type_declared==1);
  assert(gml_global_num(&engine->vm,"launch_marker")==7);
  assert(gml_global_num(&engine->vm,"default_marker")==5);
  assert(gml_global_num(&engine->vm,"child_only")==12);
  assert(!strstr(engine->content_overrides_text,"root_only"));
  size_t child_capacity=anygm_state_size(engine),child_written=0;
  uint8_t *child_state=malloc(child_capacity); assert(child_state);
  assert(anygm_state_save(engine,child_state,child_capacity,&child_written)==ANYGM_OK);
  assert(anygm_state_load(engine,root_state,root_written)==ANYGM_OK);
  assert(engine->vm.os_type_declared==6);
  assert(anygm_state_load(engine,child_state,child_written)==ANYGM_OK);
  assert(engine->vm.os_type_declared==1 && gml_global_num(&engine->vm,"child_only")==12);
  anygm_destroy(engine);
  free(root_state); free(child_state); free(root_bytes); free(child_bytes);
  assert(!remove(anchor) && !remove(ini));
  anygm_synthetic_content_destroy(&fixture);
}

static void memory_pipeline(void){
  AnygmSyntheticContent fixture;
  assert(anygm_synthetic_content_create(&fixture));
  uint8_t *content=NULL,digest[32]; size_t content_size=0;
  assert(anygm_synthetic_content_read(&fixture,&content,&content_size));
  GmlMediaBuffer compressed={0};
  assert(gml_deflate_encode_zlib(content,content_size,&compressed));
  uint8_t *wrapped=malloc(compressed.size+4); assert(wrapped);
  memcpy(wrapped,"WRAP",4); memcpy(wrapped+4,compressed.data,compressed.size);
  gml_sha256(wrapped,compressed.size+4,digest);
  char hex[65],ini[256],path[256],companion[256],config[1024];
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  snprintf(ini,sizeof ini,"%s/AnyGM.ini",fixture.directory);
  snprintf(path,sizeof path,"%s/content.bin",fixture.directory);
  snprintf(companion,sizeof companion,"%s/data.alternate.win",fixture.directory);
  snprintf(config,sizeof config,
    "[transforms]\nunwrap=buffer unwrap(){ if(input_size<4) reject(); return slice(4,input_size-4); }\n"
    "[sha256:%s.pipelines]\ninput=unwrap|builtin.zlib:%zu\n"
    "[sha256:%s.overrides]\n$anygm_probe=23\n",hex,content_size,hex);
  write_text(ini,config);
  AnygmHostServices services={0}; services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION; anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL; assert(anygm_create(&services,&engine)==ANYGM_OK);
  AnygmContentSource source={0}; source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_MEMORY; source.data=wrapped; source.size=compressed.size+4;
  source.system_directory=fixture.directory;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(engine->win.data!=wrapped && engine->win.owns);
  assert(engine->win.size==content_size && !memcmp(engine->win.data,content,content_size));
  frame(engine); assert(gml_global_num(&engine->vm,"anygm_probe")==23);
  assert(!memcmp(wrapped,"WRAP",4) && !memcmp(wrapped+4,compressed.data,compressed.size));
  anygm_unload(engine);
  write_bytes(path,wrapped,compressed.size+4);
  source.kind=ANYGM_CONTENT_PATH; source.path=path; source.cache_directory=fixture.directory;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK); frame(engine);
  assert(gml_global_num(&engine->vm,"anygm_probe")==23);
  assert(!strcmp(engine->win.content_dir,fixture.directory));
  anygm_unload(engine);
  write_bytes(companion,content,content_size);
  const uint8_t empty_wrapped[]={'W','R','A','P','F','O','R','M',0,0,0,0};
  write_bytes(path,empty_wrapped,sizeof empty_wrapped);
  write_text(ini,"[transforms]\ninput=buffer unwrap(){return slice(4,input_size-4);}\n");
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(!strcmp(engine->current_content_path,companion) && !strcmp(engine->win.content_dir,fixture.directory));
  anygm_unload(engine); assert(!remove(companion));
  write_text(ini,config);
  /* An unmatched selector does not run the pipeline; the original normalized
   * image stays borrowed, with no source-format or transformation dependency. */
  source.kind=ANYGM_CONTENT_MEMORY; source.path=NULL;
  source.data=content; source.size=content_size;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
  assert(engine->win.data==content && !engine->win.owns);
  anygm_unload(engine);
  source.data=wrapped; source.size=compressed.size+4;
  wrapped[source.size-1]^=1;
  write_text(ini,"[pipelines]\ninput=builtin.zlib:64\n");
  assert(anygm_load(engine,&source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  assert(!engine->win.data && !memcmp(wrapped,"WRAP",4));
  write_text(ini,"[transforms]\ninput=buffer empty(){return slice(0,0);}\n");
  assert(anygm_load(engine,&source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  assert(!engine->win.data);
  anygm_destroy(engine);
  assert(!remove(ini) && !remove(path));
  free(wrapped); free(content); gml_media_buffer_release(&compressed);
  anygm_synthetic_content_destroy(&fixture);
}

static void candidate_inputs(void){
  AnygmSyntheticContent fixture;
  assert(anygm_synthetic_content_create(&fixture));
  uint8_t *content=NULL,digest[32]; size_t size=0;
  assert(anygm_synthetic_content_read(&fixture,&content,&size));
  uint8_t *container=malloc(size*2+8); assert(container);
  memcpy(container,"FORM\1\0\0\0",8); memcpy(container+8,content,size);
  gml_sha256(container,size+8,digest);
  char ini[256],path[256],config[2048],hex[65];
  for(size_t i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
  snprintf(ini,sizeof ini,"%s/AnyGM.ini",fixture.directory);
  snprintf(path,sizeof path,"%s/indexed.bin",fixture.directory);
  snprintf(config,sizeof config,"[transforms]\ninput=buffer copy(){return slice(0,input_size);}\n"
    "[sha256:%s.transforms]\ninput.probe=buffer ranges(){write64(scratch,8,8);"
    "write64(scratch,80,8);write64(scratch,88,input_size-8);return scratch_slice(0,160);}\n"
    "[sha256:%s.overrides]\n$anygm_probe=29\n",hex,hex);
  write_text(ini,config); write_bytes(path,container,size+8);
  AnygmHostServices services={0}; services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION; anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL; assert(anygm_create(&services,&engine)==ANYGM_OK);
  AnygmContentSource source={0}; source.struct_size=sizeof source;
  source.system_directory=fixture.directory; source.cache_directory=fixture.directory;
  source.kind=ANYGM_CONTENT_MEMORY; source.data=container; source.size=size+8;
  for(int path_input=0;path_input<2;path_input++){
    if(path_input){ source.kind=ANYGM_CONTENT_PATH; source.path=path; }
    assert(anygm_load(engine,&source,NULL)==ANYGM_OK);
    assert(engine->win.size==size && !memcmp(engine->win.data,content,size));
    frame(engine); assert(gml_global_num(&engine->vm,"anygm_probe")==29);
    anygm_unload(engine);
  }
  assert(!memcmp(container,"FORM\1\0\0\0",8) && !memcmp(container+8,content,size));
  /* Both valid ranges must reject before either image becomes the live engine. */
  memcpy(container,content,size); memcpy(container+size,content,size);
  snprintf(config,sizeof config,"[transforms]\ninput=buffer copy(){return slice(0,input_size);}\n"
    "input.probe=buffer ranges(){write64(scratch,8,%zu);write64(scratch,80,%zu);"
    "write64(scratch,88,%zu);return scratch_slice(0,160);}\n",size,size,size);
  write_text(ini,config); write_bytes(path,container,size*2);
  source.kind=ANYGM_CONTENT_MEMORY; source.path=NULL; source.size=size*2;
  for(int path_input=0;path_input<2;path_input++){
    if(path_input){ source.kind=ANYGM_CONTENT_PATH; source.path=path; }
    assert(anygm_load(engine,&source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
    assert(!engine->win.data);
  }
  anygm_destroy(engine); free(container); free(content);
  assert(!remove(ini) && !remove(path)); anygm_synthetic_content_destroy(&fixture);
}

static void renderer_declarations(void){
  CheatSlot slots[GML_MAX_CHEATS];int count=0;char error[256],effective[4096];
  assert(engine_boot_overrides_parse("renderer|cpu\nrenderer|opengl\nrenderer|auto\n",
                                     slots,&count,error,sizeof error));
  assert(count==1 && engine_boot_overrides_text(slots,count,effective,sizeof effective));
  assert(!strcmp(effective,"renderer|auto\n"));
  const char *invalid[]={"renderer|","renderer|GPU","renderer|opengl|cpu",
                         "?crt renderer|opengl","?crt setcall|setting|nan|anchor_call",
                         "setcall|setting|1junk|anchor_call","setcall||1|anchor_call",
                         "setcall|setting|1|","setcall|setting|1|anchor_call|extra",
                         "setcall|setting|1|anchor_call||refresh",
                         "setcall|setting|1|anchor_call|obj_fixture|refresh|extra",
                         "sync|obj_fixture:x=1","?crt sync|call|anchor_call",
                         "?crt sync|obj_fixture:health=7",
                         "?crt ?instance. sync|obj_fixture:x=1",
                         "?crt layer_x|obj_fixture|layer|1",
                         "?crt sync|layer_x|obj_fixture||1"};
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++)
    assert(!engine_boot_overrides_parse(invalid[i],slots,&count,error,sizeof error));
  assert(engine_boot_overrides_parse("?crt setcall|setting|1|apply_a\n"
    "?crt setcall|setting|2|apply_b|obj_fixture|refresh\n",
    slots,&count,error,sizeof error) && count==1);
  AnygmSyntheticContent fixture;assert(anygm_synthetic_content_create(&fixture));
  char anchor[256],text[512];
  snprintf(anchor,sizeof anchor,"%s/renderer.anygm",fixture.directory);
  const char *name=strrchr(fixture.path,'/');name=name?name+1:fixture.path;
  AnygmHostServices host={0};host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;anygm_stdio_vfs_services_init(&host);
  AnygmEngine *engine=NULL;assert(anygm_create(&host,&engine)==ANYGM_OK);
  AnygmContentSource source={0};source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;source.path=anchor;
  source.system_directory=fixture.directory;source.cache_directory=fixture.directory;
  const char *values[]={"cpu","opengl","opengl_nofallback","auto"};
  for(unsigned host_scene=0;host_scene<=2;host_scene++){
    for(unsigned enabled=0;enabled<=1;enabled++){
      for(unsigned declared=0;declared<4;declared++){
        snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\nrenderer|%s\n",
                 name,values[declared]);write_text(anchor,text);
        AnygmConfigDelta delta={0};delta.struct_size=sizeof delta;
        delta.values.struct_size=sizeof delta.values;
        delta.fields=ANYGM_CONFIG_SCENE_RENDERER|ANYGM_CONFIG_CONTENT_OVERRIDES;
        delta.values.scene_renderer=host_scene;delta.values.content_overrides=enabled;
        assert(anygm_set_config(engine,&delta)==ANYGM_OK);
        AnygmContentInfo info={0};info.struct_size=sizeof info;
        assert(anygm_load_prepare(engine,&source,NULL,&info)==ANYGM_OK);
        unsigned want=enabled && declared<3?declared:host_scene;
        assert(!!(info.flags&ANYGM_CONTENT_SCENE_OPENGL)==(want!=0));
        assert(!!(info.flags&ANYGM_CONTENT_SCENE_NO_FALLBACK)==(want==2));
        assert(engine->scene_renderer_session==(want?ANYGM_SCENE_RENDERER_OPENGL:0));
        assert(engine->scene_skip_cpu_draws_session==(want==2));
        /* A live option change cannot renegotiate the prepared session. */
        delta.values.scene_renderer=2-host_scene;
        delta.values.content_overrides=!enabled;
        assert(anygm_set_config(engine,&delta)==ANYGM_OK);
        assert(engine->scene_renderer_session==(want?ANYGM_SCENE_RENDERER_OPENGL:0));
        assert(engine->scene_skip_cpu_draws_session==(want==2));
        anygm_unload(engine);
      }
    }
  }
  anygm_destroy(engine);assert(!remove(anchor));anygm_synthetic_content_destroy(&fixture);
}

static void crt_setting_callback(void){
  AnygmSyntheticContent fixture;assert(anygm_synthetic_anchor_script_content_create(&fixture));
  char anchor[256],text[512];
  snprintf(anchor,sizeof anchor,"%s/setting.anygm",fixture.directory);
  const char *name=strrchr(fixture.path,'/');name=name?name+1:fixture.path;
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
           "?crt setcall|fixture_setting|1|anchor_call\n",name);write_text(anchor,text);
  AnygmHostServices host={0};host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;anygm_stdio_vfs_services_init(&host);
  AnygmEngine *engine=NULL;assert(anygm_create(&host,&engine)==ANYGM_OK);
  AnygmContentSource source={0};source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;source.path=anchor;
  source.system_directory=fixture.directory;source.cache_directory=fixture.directory;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==0);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==0);
  AnygmConfigDelta delta={0};delta.struct_size=sizeof delta;
  delta.values.struct_size=sizeof delta.values;
  delta.fields=ANYGM_CONFIG_ADJUST_CRT_TV|ANYGM_CONFIG_PRESENT_LOGICAL_RASTER;
  delta.values.adjust_crt_tv=1;delta.values.present_logical_raster=1;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==1);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==1);
  for(int i=0;i<4;i++) frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==1);
  size_t capacity=anygm_state_size(engine),on_size=0,off_size=0;
  uint8_t *on=malloc(capacity),*off=malloc(capacity);assert(on && off);
  assert(anygm_state_save(engine,on,capacity,&on_size)==ANYGM_OK);
  delta.values.adjust_crt_tv=0;assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==0);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==2);
  assert(anygm_state_save(engine,off,capacity,&off_size)==ANYGM_OK);
  assert(anygm_state_load(engine,on,on_size)==ANYGM_OK);
  assert(gml_global_num(&engine->vm,"fixture_setting")==0);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==2);
  delta.values.adjust_crt_tv=1;assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(anygm_state_load(engine,on,on_size)==ANYGM_OK);
  assert(gml_global_num(&engine->vm,"fixture_setting")==1);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==1);
  assert(anygm_state_load(engine,off,off_size)==ANYGM_OK);
  assert(gml_global_num(&engine->vm,"fixture_setting")==1);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==3);
  delta.fields=ANYGM_CONFIG_CONTENT_OVERRIDES;delta.values.content_overrides=0;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==0);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==4);
  delta.values.content_overrides=1;assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==1);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==5);
  assert(anygm_reset(engine)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==1);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==1);
  delta.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER;delta.values.present_logical_raster=0;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==0);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==2);
  delta.values.present_logical_raster=1;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_setting")==1);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==3);
  anygm_destroy(engine);free(on);free(off);
  assert(!remove(anchor));anygm_synthetic_content_destroy(&fixture);
}

static void crt_setting_layout(void){
  AnygmSyntheticContent fixture;assert(anygm_synthetic_setting_content_create(&fixture));
  char anchor[256],text[1024];
  snprintf(anchor,sizeof anchor,"%s/layout.anygm",fixture.directory);
  const char *name=strrchr(fixture.path,'/');name=name?name+1:fixture.path;
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "?crt setcall|fixture_setting|1|anchor_call|obj_fixture|anchor_call\n"
    "?crt sync|obj_fixture:x=$port_w/2\n"
    "?crt sync|layer_x|obj_fixture|panel_layer|$port_w/2-16\n",name);
  write_text(anchor,text);
  AnygmHostServices host={0};host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;anygm_stdio_vfs_services_init(&host);
  AnygmEngine *engine=NULL;assert(anygm_create(&host,&engine)==ANYGM_OK);
  AnygmContentSource source={0};source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;source.path=anchor;
  source.system_directory=fixture.directory;source.cache_directory=fixture.directory;
  assert(anygm_load(engine,&source,NULL)==ANYGM_OK);frame(engine);
  int object=gml_object_index_by_name(&engine->vm,"obj_fixture");
  GmlInstance *instance=gml_find_instance(&engine->vm,object);assert(instance);
  unsigned instance_id=instance->id;
  gml_set_inst_var_all(&engine->vm,"obj_fixture","x",7);
  GmlVal args[]={vreal(0),vstr("panel")};
  GmlVal handle=gml_builtin_call(&engine->vm,"layer_create",args,2);
  assert(handle.t==V_REAL && handle.d>=0);
  int layer_id=(int)handle.d;
  gml_set_inst_var_all(&engine->vm,"obj_fixture","panel_layer",handle.d);
  assert(gml_rt_layer_find(&engine->vm,layer_id));
  frame(engine);
  assert(gml_find_instance(&engine->vm,object)->x==7);
  AnygmConfigDelta delta={0};delta.struct_size=sizeof delta;
  delta.values.struct_size=sizeof delta.values;
  delta.fields=ANYGM_CONFIG_ADJUST_CRT_TV|ANYGM_CONFIG_PRESENT_LOGICAL_RASTER;
  delta.values.adjust_crt_tv=1;delta.values.present_logical_raster=1;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==2);
  assert(gml_global_num(&engine->vm,"fixture_setting_self")==instance_id);
  assert(gml_find_instance(&engine->vm,object)->x==32);
  assert(gml_rt_layer_find(&engine->vm,layer_id)->x==16);
  frame(engine);assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==2);
  size_t capacity=anygm_state_size(engine),written=0,again_size=0;
  uint8_t *state=malloc(capacity),*again=malloc(capacity);assert(state && again);
  assert(anygm_state_save(engine,state,capacity,&written)==ANYGM_OK);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  assert(anygm_state_save(engine,again,capacity,&again_size)==ANYGM_OK);
  assert(written==again_size && !memcmp(state,again,written));
  /* Consume the restored completed picture before starting a new live layout transition. */
  frame(engine);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==2);
  gml_set_global_arr(&engine->vm,"view_wview",0,256);
  gml_set_global_arr(&engine->vm,"view_wport",0,128);
  delta.values.adjust_crt_tv=0;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_find_instance(&engine->vm,object)->x==64);
  assert(gml_rt_layer_find(&engine->vm,layer_id)->x==48);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==4);
  gml_set_inst_var_all(&engine->vm,"obj_fixture","x",19);frame(engine);
  assert(gml_find_instance(&engine->vm,object)->x==19);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==4);
  assert(anygm_state_load(engine,state,written)==ANYGM_OK);
  assert(gml_global_num(&engine->vm,"fixture_setting")==0);
  assert(gml_global_num(&engine->vm,"fixture_anchor_calls")==4);
  assert(gml_find_instance(&engine->vm,object)->x==32);
  assert(gml_rt_layer_find(&engine->vm,layer_id)->x==16);
  frame(engine);
  int owner_object=gml_object_index_by_name(&engine->vm,"obj_layout_owner");
  assert(owner_object>=0);
  GmlInstance *owner=gml_instance_create(&engine->vm,0,0,owner_object);assert(owner);
  unsigned owner_id=owner->id;
  char guard[128];snprintf(guard,sizeof guard,"?crt ?instance.%s sync|obj_fixture:y=$port_h",
                            engine->vm.objects[owner_object].name);
  gml_set_global_arr(&engine->vm,"view_hport",0,64);
  assert(anygm_set_runtime_override(engine,0,1,guard)==ANYGM_OK);
  delta.values.adjust_crt_tv=1;
  assert(anygm_set_config(engine,&delta)==ANYGM_OK);frame(engine);
  assert(gml_find_instance(&engine->vm,object)->y==64);
  gml_instance_destroy(&engine->vm,gml_vm_instance_by_id(&engine->vm,owner_id));
  gml_set_inst_var_all(&engine->vm,"obj_fixture","y",19);frame(engine);
  /* A layout owner disappearing must not recenter an incoming shared target during cleanup. */
  assert(gml_find_instance(&engine->vm,object)->y==19);
  anygm_destroy(engine);free(state);free(again);
  assert(!remove(anchor));anygm_synthetic_content_destroy(&fixture);
}

int main(void){
  renderer_declarations();
  crt_setting_callback();
  crt_setting_layout();
  state_reserve_declarations();
  candidate_inputs();
  memory_pipeline();
  merge_destinations(); lifecycle(); replacement();
  puts("Layered content overrides, payload selection, Reset and state identity: ok");
  return 0;
}
