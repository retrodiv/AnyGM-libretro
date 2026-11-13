/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "gml_render_internal.h"
#include "gml_render_state.h"
#include "gml_audio.h"
#include "memory_vfs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do { if(!(condition)){ \
  fprintf(stderr,"video builtin: %s (line %d)\n",label,__LINE__);return 1; \
} } while(0)
static GmlVal call(GmlVM *vm,const char *name,GmlVal *args,int count,int fast){
  if(!fast) return gml_builtin_call(vm,name,args,count);
  int id=gml_builtin_fast_id(vm,name);
  return id>=0?gml_builtin_call_fast_id(vm,id,name,args,count):vundef();
}
static double number(GmlVM *vm,const char *name,int fast){
  GmlVal value=call(vm,name,NULL,0,fast);
  return value.t==V_REAL?value.d:NAN;
}
static int restore_parts(GmlVM *vm,uint8_t *states[3],const size_t sizes[3]){
  GmlRender *render=vm->render;GmlAudio *audio=vm->audio;size_t used=0;
  gml_builtin_video_close(vm);
  REQUIRE(gml_render_state_load(render,states[1],sizes[1],&used) && used==sizes[1],"renderer restore");
  REQUIRE(gml_vm_state_load(vm,states[0],sizes[0],&used) && used==sizes[0],"VM restore");
  REQUIRE(gml_audio_state_load(audio,states[2],sizes[2],&used) && used==sizes[2],"mixer restore");
  REQUIRE(gml_builtin_video_finish_restore(vm),"restored media resource adoption");
  return 0;
}
static int continuation(GmlVM *vm,int fast,int16_t *pcm,uint32_t *pixels){
  (void)call(vm,"video_resume",NULL,0,fast);
  for(int frame=0;frame<40;frame++){
    gml_builtin_video_tick(vm);gml_audio_mix(vm->audio,pcm+frame*1470,735);
    GmlVal draw=call(vm,"video_draw",NULL,0,fast);int width=0,height=0;
    const uint32_t *image=gml_surface_pixels_read(vm->render,(int)gml_arr_get(draw,1).d,
                                                &width,&height);
    REQUIRE(image && width==16 && height==16,"continuation video image");
    memcpy(pixels+frame*256,image,256*sizeof(*pixels));gml_values_release(&draw,1);
  }
  return 0;
}
static int state_roundtrip(GmlVM *vm,int fast){
  GmlRender *render=vm->render;GmlAudio *audio=vm->audio;
  double status=number(vm,"video_get_status",fast),position=number(vm,"video_get_position",fast);
  size_t sizes[]={gml_vm_state_size(vm),gml_render_state_size(render,0),gml_audio_state_size(audio)};
  uint8_t *states[3]={NULL,NULL,NULL};size_t written=0;
  for(int i=0;i<3;i++){
    states[i]=malloc(sizes[i]);REQUIRE(states[i] && sizes[i],"video state allocation");
  }
  REQUIRE(gml_vm_state_save(vm,states[0],sizes[0],&written) && written==sizes[0],"VM snapshot");
  REQUIRE(gml_render_state_save(render,0,states[1],sizes[1],&written) && written==sizes[1],
          "renderer snapshot");
  REQUIRE(gml_audio_state_save(audio,states[2],sizes[2],&written) && written==sizes[2],"mixer snapshot");
  int16_t *first_pcm=malloc(40*1470*sizeof(int16_t)),*repeat_pcm=malloc(40*1470*sizeof(int16_t));
  uint32_t *first_pixels=malloc(40*256*sizeof(uint32_t)),*repeat_pixels=malloc(40*256*sizeof(uint32_t));
  REQUIRE(first_pcm && repeat_pcm && first_pixels && repeat_pixels,"continuation controls allocate");
  REQUIRE(!continuation(vm,fast,first_pcm,first_pixels),"original A/V continuation");
  REQUIRE(!restore_parts(vm,states,sizes),"all owners restore");
  REQUIRE(number(vm,"video_get_status",fast)==status &&
          number(vm,"video_get_position",fast)==position,"loaded playback survives canonical state");
  for(int i=0;i<3;i++){
    uint8_t *repeat=malloc(sizes[i]);int saved=0;
    REQUIRE(repeat,"canonical re-save allocation");
    if(i==0) saved=gml_vm_state_save(vm,repeat,sizes[i],&written);
    if(i==1) saved=gml_render_state_save(render,0,repeat,sizes[i],&written);
    if(i==2) saved=gml_audio_state_save(audio,repeat,sizes[i],&written);
    REQUIRE(saved && written==sizes[i] && !memcmp(repeat,states[i],sizes[i]),
            "canonical renderer/VM/audio re-save is byte-identical");free(repeat);
  }
  REQUIRE(!continuation(vm,fast,repeat_pcm,repeat_pixels) &&
          !memcmp(first_pcm,repeat_pcm,40*1470*sizeof(int16_t)) &&
          !memcmp(first_pixels,repeat_pixels,40*256*sizeof(uint32_t)),
          "forty restored frames and PCM windows exactly match original continuation");
  REQUIRE(!restore_parts(vm,states,sizes),"return to parked playback for remaining controls");
  free(first_pcm);free(repeat_pcm);free(first_pixels);free(repeat_pixels);
  for(int i=0;i<3;i++) free(states[i]);
  return 0;
}
int main(int argc,char **argv){
  REQUIRE(argc==2,"authored video argument");
  FILE *file=fopen(argv[1],"rb");
  REQUIRE(file && !fseek(file,0,SEEK_END),"input opens");
  long size=ftell(file);
  REQUIRE(size>0 && size<=1048576 && !fseek(file,0,SEEK_SET),"bounded input");
  uint8_t *bytes=malloc((size_t)size);
  REQUIRE(bytes && fread(bytes,1,(size_t)size,file)==(size_t)size,"input read");fclose(file);
  for(int fast=0;fast<2;fast++){
    AnygmMemoryVfs memory;AnygmHostServices host;GmlWin content={0};
    anygm_memory_vfs_init(&memory,&host);
    REQUIRE(anygm_memory_vfs_add_file(&memory,"/media/clip.bin",bytes,(size_t)size),"VFS input");
    strcpy(content.content_dir,"/media");content.host=&host;content.game_speed=60;
    GmlRender render={0};GmlVM vm={0};
    REQUIRE(!gml_vm_init(&vm,&content,NULL),"VM initializes canonical owners");
    vm.host=&host;vm.render=&render;
    REQUIRE(!gml_render_init(&render,&content),"renderer");
    GmlAudio *audio=gml_audio_create(&content);vm.audio=audio;
    REQUIRE(audio && number(&vm,"video_get_status",fast)==0,"closed slot");
    GmlVal path=vstr("clip.bin"),zero=vreal(0),one=vreal(1);
    (void)call(&vm,"video_open",&path,1,fast);
    REQUIRE(number(&vm,"video_get_status",fast)==1 &&
            number(&vm,"video_get_format",fast)==0 &&
            number(&vm,"video_get_duration",fast)>=1499,"opened VFS source and metadata");
    gml_builtin_video_tick(&vm);
    REQUIRE(number(&vm,"video_get_status",fast)==2 &&
            number(&vm,"video_get_position",fast)==0,"prepared frame");
    GmlVal draw=call(&vm,"video_draw",NULL,0,fast);
    REQUIRE(gml_val_array_length(draw)==2 && gml_arr_get(draw,0).d==0 &&
            gml_surface_exists(&render,(int)gml_arr_get(draw,1).d),"public surface pair");
    gml_values_release(&draw,1);
    (void)call(&vm,"video_pause",NULL,0,fast);gml_builtin_video_tick(&vm);
    REQUIRE(number(&vm,"video_get_status",fast)==3 &&
            number(&vm,"video_get_position",fast)==0,"GML pause");
    REQUIRE(!state_roundtrip(&vm,fast),"paused video state roundtrip");
    (void)call(&vm,"video_resume",NULL,0,fast);
    (void)call(&vm,"video_set_volume",&zero,1,fast);
    int16_t pcm[1470];gml_audio_mix(audio,pcm,735);
    for(int i=0;i<1470;i++) REQUIRE(!pcm[i],"GML volume controls actual voice");
    (void)call(&vm,"video_set_volume",&one,1,fast);
    (void)call(&vm,"video_enable_loop",&one,1,fast);
    for(int frame=0;frame<100;frame++){
      gml_builtin_video_tick(&vm);gml_audio_mix(audio,pcm,735);
    }
    REQUIRE(number(&vm,"video_get_status",fast)==2 &&
            number(&vm,"video_get_position",fast)<200,"GML looping");
    (void)call(&vm,"video_enable_loop",&zero,1,fast);
    for(int frame=0;frame<100;frame++){
      gml_builtin_video_tick(&vm);gml_audio_mix(audio,pcm,735);
    }
    draw=call(&vm,"video_draw",NULL,0,fast);
    REQUIRE(number(&vm,"video_get_status",fast)==0 && gml_arr_get(draw,0).d==-2 &&
            !gml_audio_queue_event_count(audio),"GML end and private mixer events");
    gml_values_release(&draw,1);
    (void)call(&vm,"video_close",NULL,0,fast);
    REQUIRE(number(&vm,"video_get_duration",fast)==0 &&
            number(&vm,"video_get_position",fast)==0,"close clears metadata");
    (void)call(&vm,"video_open",&path,1,fast);
    REQUIRE(number(&vm,"video_get_status",fast)==1,"second open");
    path=vstr("missing.bin");(void)call(&vm,"video_open",&path,1,fast);
    REQUIRE(number(&vm,"video_get_status",fast)==0,"failed replacement releases old slot");
    gml_builtin_video_close(&vm);gml_vm_free(&vm);
    gml_audio_free(audio);gml_render_free(&render);anygm_memory_vfs_destroy(&memory);
  }
  free(bytes);puts("video builtin: eleven APIs, exact registry and cached dispatch, VFS and mixer pass");
  return 0;
}
