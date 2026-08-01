/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "content_router.h"
#include "engine_internal.h"
#include "gml_vm_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int save_state(AnygmEngine *engine,uint8_t **data,size_t *written){
  size_t capacity=anygm_state_size(engine);
  *data=malloc(capacity?capacity:1);
  if(!*data) return 0;
  return anygm_state_save(engine,*data,capacity,written)==ANYGM_OK;
}

static uint64_t read_u64(const uint8_t *data){
  uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)data[i]<<(i*8);
  return value;
}

static void write_u32(uint8_t *data,uint32_t value){
  for(unsigned i=0;i<4;i++) data[i]=(uint8_t)(value>>(i*8));
}

static void write_u64(uint8_t *data,uint64_t value){
  for(unsigned i=0;i<8;i++) data[i]=(uint8_t)(value>>(i*8));
}

static uint64_t state_checksum(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){
    uint64_t word=read_u64(data);
    hash^=word; hash*=UINT64_C(1099511628211);
    data+=8; size-=8;
  }
  while(size--){ hash^=*data++; hash*=UINT64_C(1099511628211); }
  return hash;
}

static int screen_stage_raster_policy(void){
  GmlWin content={0};
  GmlRenderPresentationMetrics presentation={0};
  content.bytecode=15;
  presentation.requested_width=576;
  presentation.requested_height=432;
  if(!screen_stage_uses_requested_raster(&content,&presentation,288,216,576,432)){
    fputs("first-generation requested screen raster was not recognized\n",stderr);
    return 0;
  }
  presentation.application_draw_enabled=1;
  if(screen_stage_uses_requested_raster(&content,&presentation,288,216,576,432)){
    fputs("automatic application drawing selected a screen raster\n",stderr);
    return 0;
  }
  presentation.application_draw_enabled=0;
  content.classic_version=800;
  if(screen_stage_uses_requested_raster(&content,&presentation,288,216,576,432)){
    fputs("classic content selected a Studio screen raster\n",stderr);
    return 0;
  }
  content.classic_version=0;
  content.bytecode=17;
  if(screen_stage_uses_requested_raster(&content,&presentation,288,216,576,432)){
    fputs("current layer semantics selected a first-generation screen raster\n",stderr);
    return 0;
  }
  content.bytecode=15;
  if(screen_stage_uses_requested_raster(&content,&presentation,288,216,600,432) ||
     screen_stage_uses_requested_raster(&content,&presentation,288,216,288,216)){
    fputs("non-uniform or native presentation selected a scaled screen raster\n",stderr);
    return 0;
  }
  return 1;
}

static int first_generation_dynamic_camera_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=16;
  engine.vm.win=&engine.win;
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_camera",0,0);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,3);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,5);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,320);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,180);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,640);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,360);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_live",0,1);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_x",0,37);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_y",0,59);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_w",0,400);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_h",0,300);
  GmlPresentView view={0};
  int ok=present_view_get(&engine,0,&view) && view.camera==0 &&
    view.x==37 && view.y==59 && view.w==400 && view.h==300 &&
    view.pw==640 && view.ph==360;
  if(!ok)
    fprintf(stderr,
      "first-generation dynamic camera was not selected: camera=%d rect=(%.0f,%.0f %.0fx%.0f)\n",
      view.camera,view.x,view.y,view.w,view.h);
  gml_vm_free(&engine.vm);
  return ok;
}

static int draw_schedule_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_draw_content_create(&fixture)){
    fputs("draw schedule fixture creation failed\n",stderr);
    return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  double pre=ok?gml_global_num(&engine->vm,"fixture_pre_draw"):0;
  double draw=ok?gml_global_num(&engine->vm,"fixture_draw"):0;
  double post=ok?gml_global_num(&engine->vm,"fixture_post_draw"):0;
  double view_sum=ok?gml_global_num(&engine->vm,"fixture_view_sum"):0;
  ok=ok && pre==2 && draw==2 && post==2 && view_sum==4;
  if(!ok)
    fprintf(stderr,
      "draw schedule mismatch: pre=%.0f draw=%.0f post=%.0f view-sum=%.0f\n",
      pre,draw,post,view_sum);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int framebuffer_retention_case(
    int (*create_fixture)(AnygmSyntheticContent *),const char *label){
  AnygmSyntheticContent fixture;
  if(!create_fixture(&fixture)){
    fprintf(stderr,"%s framebuffer-retention fixture creation failed\n",label);
    return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     output.pixels && output.width==64 && output.height==48;
  size_t pixels=(size_t)output.width*output.height;
  uint32_t *painted=ok?malloc(pixels*sizeof(*painted)):NULL;
  ok=ok && painted!=NULL;
  if(ok) memcpy(painted,output.pixels,pixels*sizeof(*painted));
  size_t unexpected=0;
  if(ok) for(size_t i=0;i<pixels;i++)
    if((painted[i]&0xFFFFFFu)!=0x996633u) unexpected++;
  size_t framebuffer_unexpected=0;
  if(ok) for(size_t i=0;i<(size_t)engine->width*engine->height;i++)
    if((engine->fb[i]&0xFFFFFFu)!=0x996633u) framebuffer_unexpected++;
  ok=ok && unexpected==0;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     engine->vm.room_index==1 && output.pixels &&
     !memcmp(painted,output.pixels,pixels*sizeof(*painted));
  if(!ok){
    int canvas_width=0,canvas_height=0;
    int view_count=engine?present_view_count(engine,NULL,&canvas_width,&canvas_height):0;
    fprintf(stderr,
      "%s room did not present and retain the complete application framebuffer"
      " (%zu unexpected output pixels, %zu unexpected application pixels,"
      " render=%ux%u, output=%ux%u, views=%d, canvas=%dx%d)\n",
      label,unexpected,framebuffer_unexpected,engine?engine->width:0,engine?engine->height:0,
      output.width,output.height,view_count,canvas_width,canvas_height);
  }
  free(painted);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int framebuffer_retention_policy(void){
  return framebuffer_retention_case(anygm_synthetic_framebuffer_content_create,"single-view") &&
         framebuffer_retention_case(
           anygm_synthetic_multiview_framebuffer_content_create,"multi-view");
}

static int state_input_history_roundtrip(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)){
    fputs("input-history fixture creation failed\n",stderr);
    return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=NULL;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  input.connected_gamepads=1;
  input.gamepad_buttons[0][ANYGM_PAD_RIGHT]=1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  uint8_t *state=NULL;
  size_t state_size=0;
  ok=ok && engine->pad_current[ANYGM_PAD_RIGHT] &&
     engine->pad_previous[ANYGM_PAD_RIGHT] &&
     save_state(engine,&state,&state_size);
  input.gamepad_buttons[0][ANYGM_PAD_RIGHT]=0;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     !engine->pad_current[ANYGM_PAD_RIGHT] &&
     engine->pad_previous[ANYGM_PAD_RIGHT] &&
     anygm_state_load(engine,state,state_size)==ANYGM_OK &&
     engine->pad_current[ANYGM_PAD_RIGHT] &&
     engine->pad_previous[ANYGM_PAD_RIGHT];
  input.gamepad_buttons[0][ANYGM_PAD_RIGHT]=1;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     engine->pad_current[ANYGM_PAD_RIGHT] &&
     engine->pad_previous[ANYGM_PAD_RIGHT];
  free(state);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  if(!ok) fputs("state roundtrip did not preserve the input edge baseline\n",stderr);
  return ok;
}

static int expect_rejected_unchanged(AnygmEngine *engine,const uint8_t *candidate,size_t size,
                                     const uint8_t *baseline,size_t baseline_size,
                                     const char *label){
  if(anygm_state_load(engine,candidate,size)!=ANYGM_ERROR_STATE_MISMATCH){
    fprintf(stderr,"%s was not rejected\n",label);
    return 0;
  }
  uint8_t *after=NULL;
  size_t after_size=0;
  int unchanged=save_state(engine,&after,&after_size) && after_size==baseline_size &&
                !memcmp(after,baseline,baseline_size);
  if(!unchanged && after && after_size==baseline_size){
    size_t first=0;
    while(first<baseline_size && after[first]==baseline[first]) first++;
    if(first<baseline_size)
      fprintf(stderr,"%s first state difference at %zu: %02x != %02x\n",
              label,first,after[first],baseline[first]);
    first=112;
    while(first<baseline_size && after[first]==baseline[first]) first++;
    if(first<baseline_size)
      fprintf(stderr,"%s first payload difference at %zu: %02x != %02x\n",
              label,first,after[first],baseline[first]);
    if(first<baseline_size){
      size_t from=first>16?first-16:0,to=first+32<baseline_size?first+32:baseline_size;
      fputs("after:   ",stderr); for(size_t i=from;i<to;i++) fprintf(stderr,"%02x",after[i]); fputc('\n',stderr);
      fputs("baseline:",stderr); for(size_t i=from;i<to;i++) fprintf(stderr,"%02x",baseline[i]); fputc('\n',stderr);
    }
  }
  free(after);
  if(!unchanged) fprintf(stderr,"%s changed the engine despite rejection\n",label);
  return unchanged;
}

int main(int argc,char **argv){
  if(argc==3 && !strcmp(argv[1],"--case")){
    if(!strcmp(argv[2],"first_generation_dynamic_camera"))
      return first_generation_dynamic_camera_policy()?0:1;
    if(!strcmp(argv[2],"multi_view_application_canvas"))
      return framebuffer_retention_case(
        anygm_synthetic_multiview_framebuffer_content_create,"multi-view")?0:1;
    fprintf(stderr,"unknown integration case: %s\n",argv[2]);
    return 1;
  }
  if(argc!=1){
    fputs("usage: test_engine_instances [--case first_generation_dynamic_camera|"
          "multi_view_application_canvas]\n",stderr);
    return 1;
  }
  if(!screen_stage_raster_policy()) return 1;
  if(!first_generation_dynamic_camera_policy()) return 1;
  if(!draw_schedule_policy()) return 1;
  if(!framebuffer_retention_policy()) return 1;
  if(!state_input_history_roundtrip()) return 1;
  char label[128];
  anygm_content_save_label("/library/fixture_bundle/data.win",label,sizeof label);
  if(strcmp(label,"fixture_bundle")){
    fprintf(stderr,"generic payload save label mismatch: %s\n",label);
    return 1;
  }
  anygm_content_save_label("C:\\library\\fixture_bundle\\data.alternate.win",label,sizeof label);
  if(strcmp(label,"fixture_bundle")){
    fprintf(stderr,"Windows-style generic payload save label mismatch: %s\n",label);
    return 1;
  }
  anygm_content_save_label("/library/fixture_bundle.zip",label,sizeof label);
  if(strcmp(label,"fixture_bundle")){
    fprintf(stderr,"container save label mismatch: %s\n",label);
    return 1;
  }

  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)) return 1;

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *first=NULL,*second=NULL;
  if(anygm_create(&services,&first)!=ANYGM_OK ||
     anygm_create(&services,&second)!=ANYGM_OK){
    fprintf(stderr,"engine creation failed\n");
    return 1;
  }

  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  if(anygm_load(first,&source,NULL)!=ANYGM_OK || anygm_load(second,&source,NULL)!=ANYGM_OK){
    char error[512]={0};
    anygm_get_last_error(first,error,sizeof error);
    fprintf(stderr,"engine load failed: %s\n",error);
    return 1;
  }
  const char *fixture_name=strrchr(fixture.directory,'/');
  fixture_name=fixture_name?fixture_name+1:fixture.directory;
  char save_path[384];
  snprintf(save_path,sizeof save_path,"%s/anygm/%s-%08x",fixture.directory,fixture_name,
           anygm_content_path_hash(fixture.path));
  AnygmFileInfo save_info={0};
  save_info.struct_size=sizeof save_info;
  if(services.file_stat(services.userdata,save_path,&save_info)!=ANYGM_OK ||
     !(save_info.flags&ANYGM_FILE_INFO_DIRECTORY)){
    fprintf(stderr,"content save namespace was not created: %s\n",save_path);
    return 1;
  }

  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput first_output={0},second_output={0};
  first_output.struct_size=sizeof first_output;
  second_output.struct_size=sizeof second_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     anygm_run_frame(second,&input,&second_output)!=ANYGM_OK ||
     !first_output.pixels || !second_output.pixels || first_output.pixels==second_output.pixels){
    fprintf(stderr,"interleaved frame ownership failed\n");
    return 1;
  }

  uint8_t *first_state=NULL,*second_state=NULL;
  size_t first_written=0,second_written=0;
  if(!save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     first_written!=second_written || memcmp(first_state,second_state,first_written)){
    fprintf(stderr,"equal interleaved engine states diverged\n");
    return 1;
  }
  free(first_state); free(second_state);

  first_output.struct_size=sizeof first_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     !save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     (first_written==second_written && !memcmp(first_state,second_state,first_written))){
    fprintf(stderr,"independently advanced engine state was not isolated\n");
    return 1;
  }
  free(first_state); free(second_state);

  second_output.struct_size=sizeof second_output;
  if(anygm_run_frame(second,&input,&second_output)!=ANYGM_OK ||
     !save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     first_written!=second_written || memcmp(first_state,second_state,first_written)){
    fprintf(stderr,"resynchronized engine states diverged\n");
    return 1;
  }

  uint8_t *deterministic=NULL;
  size_t deterministic_size=0;
  if(!save_state(first,&deterministic,&deterministic_size) ||
     deterministic_size!=first_written || memcmp(deterministic,first_state,first_written)){
    fprintf(stderr,"repeated serialization was not deterministic\n");
    return 1;
  }
  uint64_t deterministic_hash=state_checksum(deterministic,deterministic_size);
  if(deterministic_size!=19490 ||
     deterministic_hash!=UINT64_C(0xe78de2424cc30a7b)){
    fprintf(stderr,"canonical engine state changed: size=%zu hash=%016llx\n",
            deterministic_size,(unsigned long long)deterministic_hash);
    return 1;
  }
  free(deterministic);

  uint8_t *damaged=malloc(first_written);
  if(!damaged) return 1;
  memcpy(damaged,first_state,first_written);
  write_u32(damaged+4,ANYGM_STATE_SCHEMA-1u);
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "older state schema")) return 1;
  if(!expect_rejected_unchanged(first,first_state,first_written-1,first_state,first_written,
                                "truncated state")) return 1;
  memcpy(damaged,first_state,first_written);
  damaged[112]^=1;
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "payload checksum mismatch")) return 1;

  memcpy(damaged,first_state,first_written);
  uint64_t core_size=read_u64(damaged+64);
  uint64_t render_size=read_u64(damaged+72);
  uint64_t payload_size=read_u64(damaged+96);
  uint64_t vm_offset=112+core_size+render_size;
  if(vm_offset+12>first_written || payload_size>first_written-112) return 1;
  write_u32(damaged+(size_t)vm_offset+8,UINT32_MAX);
  write_u64(damaged+56,state_checksum(damaged+112,(size_t)payload_size));
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "invalid VM section")) return 1;
  free(damaged);

  first_output.struct_size=sizeof first_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     anygm_state_load(first,first_state,first_written)!=ANYGM_OK ||
     !save_state(first,&deterministic,&deterministic_size) ||
     deterministic_size!=first_written || memcmp(deterministic,first_state,first_written)){
    fprintf(stderr,"state roundtrip did not restore exact serialized state\n");
    return 1;
  }
  free(deterministic);

  free(first_state); free(second_state);
  anygm_destroy(first);
  anygm_destroy(second);
  anygm_synthetic_content_destroy(&fixture);
  puts("independent engine instances: ok");
  return 0;
}
