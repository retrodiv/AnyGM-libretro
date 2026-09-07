/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "engine_internal.h"
#include "gml_hash.h"
#include "gml_image_codec.h"
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
  snprintf(ini,sizeof ini,"%s/anygm.ini",fixture.directory);
  snprintf(anchor,sizeof anchor,"%s/content.anygm",fixture.directory);
  const char *name=strrchr(fixture.path,'/'); name=name?name+1:fixture.path;
  snprintf(text,sizeof text,"[anygm]\npayload=%s\n[overrides]\n"
    "$anygm_probe=2\nostype|6\n$anchor_only=9\n",name);
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
  snprintf(ini,sizeof ini,"%s/anygm.ini",fixture.directory);
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
  snprintf(ini,sizeof ini,"%s/anygm.ini",fixture.directory);
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
  snprintf(ini,sizeof ini,"%s/anygm.ini",fixture.directory);
  snprintf(path,sizeof path,"%s/indexed.bin",fixture.directory);
  snprintf(config,sizeof config,"[transforms]\ninput=buffer copy(){return slice(0,input_size);}\n"
    "[sha256:%s.transforms]\ninput.probe=buffer ranges(){write64(scratch,8,8);"
    "write64(scratch,16,8);write64(scratch,24,input_size-8);return scratch_slice(0,32);}\n"
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
    "input.probe=buffer ranges(){write64(scratch,8,%zu);write64(scratch,16,%zu);"
    "write64(scratch,24,%zu);return scratch_slice(0,32);}\n",size,size,size);
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

int main(void){
  candidate_inputs();
  memory_pipeline();
  merge_destinations(); lifecycle(); replacement();
  puts("Layered content overrides, payload selection, Reset and state identity: ok");
  return 0;
}
