/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "gml_render.h"
#include "memory_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition,label) do { if(!(condition)){ \
  fprintf(stderr,"engine video: %s (line %d)\n",label,__LINE__);goto done; \
} } while(0)

typedef struct {
  AnygmMemoryVfs memory;
  AnygmHostServices host;
  AnygmEngine *engine;
  uint8_t *payload;
} Fixture;

static uint64_t read64(const uint8_t *p){
  uint64_t value=0;
  for(int i=0;i<8;i++) value|=(uint64_t)p[i]<<(i*8);
  return value;
}
static void write64(uint8_t *p,uint64_t value){
  for(int i=0;i<8;i++) p[i]=(uint8_t)(value>>(i*8));
}
static uint64_t checksum(const uint8_t *p,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){hash^=read64(p);hash*=UINT64_C(1099511628211);p+=8;size-=8;}
  while(size--){hash^=*p++;hash*=UINT64_C(1099511628211);}
  return hash;
}
static int init(Fixture *fixture,const uint8_t *clip,size_t clip_size,int callback){
  AnygmSyntheticContent content={0};size_t payload_size=0;
  int created=callback?anygm_synthetic_video_wait_content_create(&content):
                       anygm_synthetic_draw_content_create(&content);
  int ok=created && anygm_synthetic_content_read(&content,&fixture->payload,&payload_size);
  if(created) anygm_synthetic_content_destroy(&content);
  anygm_memory_vfs_init(&fixture->memory,&fixture->host);
  AnygmContentSource source={0};source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_MEMORY;source.path="/content/data.win";
  source.data=fixture->payload;source.size=payload_size;
  return ok && anygm_memory_vfs_add_file(&fixture->memory,"/content/clip.avi",clip,clip_size) &&
    anygm_create(&fixture->host,&fixture->engine)==ANYGM_OK &&
    anygm_load(fixture->engine,&source,NULL)==ANYGM_OK;
}
static void destroy(Fixture *fixture){
  if(!fixture) return;
  anygm_destroy(fixture->engine);anygm_memory_vfs_destroy(&fixture->memory);
  free(fixture->payload);free(fixture);
}
static GmlVal call(AnygmEngine *engine,const char *name){
  return gml_builtin_call(&engine->vm,name,NULL,0);
}
static int frame(AnygmEngine *engine,AnygmFrameOutput *output){
  AnygmInputFrame input={0};input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  memset(output,0,sizeof *output);output->struct_size=sizeof *output;
  return anygm_run_frame(engine,&input,output)==ANYGM_OK;
}
static int open_paused(AnygmEngine *engine){
  GmlVal path=vstr("clip.avi");AnygmFrameOutput output;
  (void)gml_builtin_call(&engine->vm,"video_open",&path,1);
  if(!frame(engine,&output) || call(engine,"video_get_status").d!=2) return 0;
  GmlVal draw=call(engine,"video_draw");
  int surface=(int)gml_arr_get(draw,1).d;gml_values_release(&draw,1);
  (void)call(engine,"video_pause");
  return surface>=0 && frame(engine,&output) && call(engine,"video_get_status").d==3;
}
static uint8_t *save(AnygmEngine *engine,size_t *size){
  size_t capacity=anygm_state_size(engine);uint8_t *bytes=capacity?malloc(capacity):NULL;
  if(!bytes || anygm_state_save(engine,bytes,capacity,size)!=ANYGM_OK){free(bytes);return NULL;}
  return bytes;
}
static int replace_source(Fixture *fixture,int missing){
  if(missing)
    return fixture->host.path_remove(fixture->host.userdata,"/content/clip.avi")==ANYGM_OK;
  return anygm_memory_vfs_xor_byte(&fixture->memory,"/content/clip.avi",0,1);
}

/* Fail after renderer replacement, or after the target decoder has been reconstructed. The
 * old decoder must keep the original source alive for rollback, independently of the VFS. */
static int rollback_case(const uint8_t *clip,size_t clip_size,int missing,int late){
  Fixture *fixture=calloc(1,sizeof *fixture);int ok=0;
  uint8_t *target=NULL,*baseline=NULL,*after=NULL;
  size_t target_size=0,baseline_size=0,after_size=0;
  CHECK(fixture && init(fixture,clip,clip_size,0),"rollback fixture");
  AnygmEngine *engine=fixture->engine;
  if(!late) target=save(engine,&target_size);
  CHECK(open_paused(engine),"rollback playback");
  if(late) target=save(engine,&target_size);
  baseline=save(engine,&baseline_size);
  CHECK(target && baseline && replace_source(fixture,missing),"rollback source control");
  size_t vm_offset=112+(size_t)read64(target+64)+(size_t)read64(target+72);
  size_t audio_offset=vm_offset+(size_t)read64(target+80);
  CHECK(vm_offset<target_size && target_size-vm_offset>=12 && audio_offset<target_size,
        "root section bounds");
  if(late) target[audio_offset]^=1; /* audio magic: reject after video decoder reconstruction */
  else memset(target+vm_offset+8,0xff,4); /* impossible instance count after renderer replacement */
  write64(target+56,checksum(target+112,target_size-112));
  CHECK(anygm_state_load(engine,target,target_size)==ANYGM_ERROR_STATE_MISMATCH,
        "malformed later section is rejected");
  after=save(engine,&after_size);
  CHECK(after && after_size==baseline_size && !memcmp(after,baseline,baseline_size) &&
        call(engine,"video_get_status").d==3,"rejected load preserves exact live root and media");
  ok=1;
done:
  if(!ok) fprintf(stderr,"engine video rollback control: missing=%d late=%d\n",missing,late);
  free(target);free(baseline);free(after);destroy(fixture);return ok;
}

static int continuation(AnygmEngine *engine,int16_t *pcm,uint32_t *pixels){
  (void)call(engine,"video_resume");
  for(int index=0;index<40;index++){
    AnygmFrameOutput output;int width=0,height=0;
    if(!frame(engine,&output) || output.audio_frames!=735 || !output.audio) return 0;
    memcpy(pcm+index*1470,output.audio,1470*sizeof(*pcm));
    GmlVal draw=call(engine,"video_draw");
    const uint32_t *image=gml_surface_pixels_read(&engine->render,(int)gml_arr_get(draw,1).d,
                                                 &width,&height);
    int valid=image && width==16 && height==16;
    if(valid) memcpy(pixels+index*256,image,256*sizeof(*pixels));
    gml_values_release(&draw,1);
    if(!valid) return 0;
  }
  return 1;
}
static void close_in_draw(GmlVM *vm,GmlInstance *instance,const char *suffix,int begin,void *user){
  (void)instance;(void)suffix;
  int *calls=user;
  if(begin && !*calls){(*calls)++;gml_builtin_video_close(vm);}
}
static int restore_case(const uint8_t *clip,size_t clip_size,int redraw){
  Fixture *fixture=calloc(1,sizeof *fixture);int ok=0,calls=0;
  uint8_t *state=NULL,*baseline=NULL,*after=NULL;
  size_t state_size=0,baseline_size=0,after_size=0;
  int16_t *first_pcm=malloc(40*1470*sizeof(int16_t)),*repeat_pcm=malloc(40*1470*sizeof(int16_t));
  uint32_t *first_pixels=malloc(40*256*sizeof(uint32_t)),*repeat_pixels=malloc(40*256*sizeof(uint32_t));
  CHECK(fixture && first_pcm && repeat_pcm && first_pixels && repeat_pixels &&
        init(fixture,clip,clip_size,0),"restore fixture");
  AnygmEngine *engine=fixture->engine;
  CHECK(open_paused(engine),"restore playback");
  state=save(engine,&state_size);
  CHECK(state && continuation(engine,first_pcm,first_pixels),"original engine A/V continuation");
  CHECK(replace_source(fixture,1) && anygm_state_load(engine,state,state_size)==ANYGM_OK &&
        engine->state_video_source && call(engine,"video_get_status").d==3,
        "restore uses immutable media after source removal");
  if(redraw){
    /* Select the redraw branch used when presentation geometry changes. Its Draw callback
     * deliberately closes playback before the canonical post-frame state is reapplied. */
    engine->state_frame_available=0;
    engine->vm.draw_event_hook=close_in_draw;engine->vm.draw_event_hook_user=&calls;
  }
  AnygmFrameOutput output;
  CHECK(frame(engine,&output) && !engine->state_video_source &&
        call(engine,"video_get_status").d==3 && (!redraw || calls==1),
        "presentation consumes the source checkpoint and retains paused playback");
  CHECK(continuation(engine,repeat_pcm,repeat_pixels) &&
        !memcmp(first_pcm,repeat_pcm,40*1470*sizeof(int16_t)) &&
        !memcmp(first_pixels,repeat_pixels,40*256*sizeof(uint32_t)),
        "public state restore preserves forty engine video and audio frames");
  (void)call(engine,"video_close");
  baseline=save(engine,&baseline_size);
  CHECK(baseline && anygm_state_load(engine,state,state_size)==ANYGM_ERROR_STATE_MISMATCH,
        "unavailable uncached source rejects restore");
  after=save(engine,&after_size);
  CHECK(after && after_size==baseline_size && !memcmp(after,baseline,baseline_size),
        "missing media rejection is transactional");
  CHECK(anygm_memory_vfs_add_file(&fixture->memory,"/content/clip.avi",clip,clip_size) &&
        anygm_state_load(engine,state,state_size)==ANYGM_OK && engine->state_video_source &&
        anygm_reset(engine)==ANYGM_OK && !engine->state_video_source &&
        call(engine,"video_get_status").d==0,"Reset releases pending media restore ownership");
  ok=1;
done:
  if(!ok) fprintf(stderr,"engine video restore control: redraw=%d\n",redraw);
  free(first_pcm);free(repeat_pcm);free(first_pixels);free(repeat_pixels);
  free(state);free(baseline);free(after);destroy(fixture);return ok;
}

static int global_text(GmlVM *vm,const char *name,const char *text){
  GmlVal *value=gml_varmap_get(&vm->globals,name);
  return value && value->t==V_STR && !strcmp(value->s,text);
}
static int callback_case(const uint8_t *clip,size_t clip_size){
  Fixture *fixture=calloc(1,sizeof *fixture);int ok=0;
  uint8_t *state=NULL,*after=NULL;size_t state_size=0,after_size=0;
  CHECK(fixture && init(fixture,clip,clip_size,1),"callback fixture");
  AnygmEngine *engine=fixture->engine;GmlVM *vm=&engine->vm;
  const char *names[]={"video_status_closed","video_status_preparing","video_status_playing",
                       "video_status_paused","video_format_rgba","video_format_yuv"};
  const int expected[]={0,1,2,3,0,1};
  GmlVal *constants=gml_varmap_get(&vm->globals,"fixture_constants");
  CHECK(constants && gml_val_array_length(*constants)==6,"source compiler resolves video constants");
  for(int i=0;i<6;i++){
    unsigned long reads=gml_vm_unset_reads(vm);
    GmlVal value=gml_vm_identifier_get(vm,names[i]);
    CHECK(value.t==V_REAL && value.d==expected[i] &&
          gml_arr_get(*constants,i).d==expected[i] && gml_vm_unset_reads(vm)==reads,
          "runtime and compiled video constant values");
  }
  const char *buffer_names[]={"buffer_fixed","buffer_u8","buffer_u16","buffer_s16"};
  const int buffer_values[]={0,1,3,4};
  for(int i=0;i<4;i++){
    unsigned long reads=gml_vm_unset_reads(vm);
    GmlVal value=gml_vm_identifier_get(vm,buffer_names[i]);
    CHECK(value.t==V_REAL && value.d==buffer_values[i] && gml_vm_unset_reads(vm)==reads,
          "media fixture buffer names resolve without unset fallback");
  }
  *gml_varmap_put(&vm->globals,"async_load")=vreal(777);
  GmlVal path=vstr("clip.avi");
  (void)gml_builtin_call(vm,"video_open",&path,1);
  gml_builtin_video_tick(vm);
  int map=vm->wait.async_map;
  CHECK(vm->wait.active && map>0 && gml_broadcast_map_live(vm,map) &&
        gml_global_num(vm,"async_load")==777 &&
        gml_global_num(vm,"fixture_entries")==1 &&
        global_text(vm,"fixture_before","video_start"),"Social callback parks with its real payload");
  CHECK(!gml_vm_wait_resume(vm) && vm->wait.active && vm->wait.async_map==map &&
        gml_global_num(vm,"async_load")==777 && gml_broadcast_map_live(vm,map),
        "repeated sleep retains payload and restores enclosing async_load");
  state=save(engine,&state_size);CHECK(state,"parked callback public snapshot");
  gml_vm_wait_cancel(vm);
  CHECK(!vm->wait.active && !gml_broadcast_map_live(vm,map),"callback cancellation releases payload");
  CHECK(anygm_state_load(engine,state,state_size)==ANYGM_OK && vm->wait.active &&
        vm->wait.async_map==map && gml_broadcast_map_live(vm,map),"public restore retains parked payload");
  after=save(engine,&after_size);
  CHECK(after && after_size==state_size && !memcmp(after,state,state_size),
        "parked callback root re-save is byte-identical");
  CHECK(!gml_vm_wait_resume(vm) && vm->wait.async_map==map && gml_broadcast_map_live(vm,map),
        "restored callback can park again");
  gml_set_global_scalar(vm,"fixture_release",1);
  CHECK(gml_vm_wait_resume(vm) && !vm->wait.active && !gml_broadcast_map_live(vm,map) &&
        gml_global_num(vm,"fixture_entries")==1 && gml_global_num(vm,"fixture_resumed")==1 &&
        gml_global_num(vm,"fixture_status")==0 && gml_global_num(vm,"async_load")==777 &&
        global_text(vm,"fixture_after","video_start"),"resumed callback sees payload then releases it");
  CHECK(anygm_state_load(engine,state,state_size)==ANYGM_OK && vm->wait.async_map==map &&
        anygm_reset(engine)==ANYGM_OK && !vm->wait.active && !vm->wait.async_map &&
        !gml_broadcast_map_live(vm,map),"Reset cancels the restored callback owner");
  ok=1;
done:
  free(state);free(after);destroy(fixture);return ok;
}

static int capacity_case(const uint8_t *clip,size_t clip_size){
  Fixture *fixture=calloc(1,sizeof *fixture);uint8_t *state=NULL;int ok=0;
  CHECK(fixture && init(fixture,clip,clip_size,0),"large media capacity fixture");
  AnygmEngine *engine=fixture->engine;GmlVal path=vstr("clip.avi");
  (void)gml_builtin_call(&engine->vm,"video_open",&path,1);
  CHECK(call(engine,"video_get_status").d==1,"large media metadata is ready before the first frame");
  size_t required=anygm_state_resume_size(engine),hint=anygm_state_resume_capacity_hint(engine);
  if(required>hint) hint=required;
  const size_t quantum=1024u*1024u;
  size_t offered=(hint+quantum-1)/quantum*quantum;
  if(offered<4*quantum) offered=4*quantum;
  AnygmFrameOutput output;
  CHECK(frame(engine,&output),"first large media frame");
  GmlVal draw=call(engine,"video_draw");int surface=(int)gml_arr_get(draw,1).d;
  gml_values_release(&draw,1);
  CHECK(surface>0 && gml_surface_width(&engine->render,surface)==1920 &&
        gml_surface_height(&engine->render,surface)==1080,"large decoded surface");
  size_t current=anygm_state_size(engine),written=0;
  fprintf(stderr,"video cold ring: offered=%zu current=%zu hint=%zu\n",offered,current,hint);
  CHECK(current<=offered,"pre-frame capacity covers the first noisy decoded picture");
  state=malloc(offered);
  CHECK(state && anygm_state_save(engine,state,offered,&written)==ANYGM_OK,
        "frontend-admitted cold slot records large playback");
  (void)call(engine,"video_close");
  CHECK(anygm_state_load(engine,state,written)==ANYGM_OK && frame(engine,&output) &&
        call(engine,"video_get_status").d==2,"large movie restores through the same fixed slot");
  const char **saved_names=engine->win.ref_name;uint8_t *saved_kinds=engine->win.ref_kind;
  int saved_count=engine->win.n_refs;const char *names[]={"video_open","video_draw"};
  uint8_t kinds[]={GML_REF_FUNCTION,GML_REF_FUNCTION};
  engine->win.ref_name=names;engine->win.ref_kind=kinds;engine->win.n_refs=2;
  uint32_t flags=anygm_state_capacity_flags(engine);
  engine->win.ref_name=saved_names;engine->win.ref_kind=saved_kinds;engine->win.n_refs=saved_count;
  CHECK(flags&ANYGM_STATE_CAPACITY_DYNAMIC_SURFACES,"media calls declare later surface allocation");
  ok=1;
done:
  free(state);destroy(fixture);return ok;
}

/* Optional CPU-cost diagnostic on this fixture's authored movie. Every measured load
 * uses an in-memory snapshot created in this execution; elapsed playback is not a
 * promise about arbitrary stream lengths. The final full state must remain exact. */
static int replay_cost_case(const uint8_t *clip,size_t clip_size){
  const int ages[]={1,31,61,85};
  for(unsigned at=0;at<sizeof ages/sizeof ages[0];at++){
    Fixture *fixture=calloc(1,sizeof *fixture);uint8_t *snapshot=NULL,*after=NULL;
    size_t size=0,after_size=0;int ok=0;
    CHECK(fixture && init(fixture,clip,clip_size,0),"replay cost fixture");
    AnygmEngine *engine=fixture->engine;GmlVal path=vstr("clip.avi");
    gml_builtin_call(&engine->vm,"video_open",&path,1);
    for(int i=0;i<ages[at];i++){
      AnygmFrameOutput output;CHECK(frame(engine,&output),"replay cost frame");
      GmlVal draw=call(engine,"video_draw");gml_values_release(&draw,1);
    }
    CHECK(call(engine,"video_get_status").d==2,"replay cost active movie");
    double position=call(engine,"video_get_position").d;
    snapshot=save(engine,&size);CHECK(snapshot,"replay cost snapshot");
    double total=0,maximum=0;
    for(int repetition=0;repetition<100;repetition++){
      clock_t start=clock();CHECK(start!=(clock_t)-1,"CPU clock available");
      AnygmResult result=anygm_state_load(engine,snapshot,size);clock_t end=clock();
      CHECK(result==ANYGM_OK && end!=(clock_t)-1 && end>=start,"repeated movie restore");
      double elapsed=(double)(end-start)*1000.0/CLOCKS_PER_SEC;
      total+=elapsed;if(elapsed>maximum)maximum=elapsed;
    }
    after=save(engine,&after_size);
    CHECK(after && size==after_size && !memcmp(snapshot,after,size),"repeated full state exact");
    printf("video replay: frames=%d position_ms=%.6f bytes=%zu restores=100 "
           "mean_cpu_ms=%.6f max_cpu_ms=%.6f exact=1\n",
           ages[at],position,size,total/100,maximum);
    ok=1;
done:
    free(after);free(snapshot);destroy(fixture);if(!ok)return 0;
  }
  return 1;
}

int main(int argc,char **argv){
  int capacity=argc==3 && !strcmp(argv[2],"--capacity");
  int replay_cost=argc==3 && !strcmp(argv[2],"--replay-cost");
  if(argc!=2 && !capacity && !replay_cost) return 1;
  FILE *input=fopen(argv[1],"rb");
  if(!input || fseek(input,0,SEEK_END)) return 1;
  long size=ftell(input);
  if(size<=0 || size>(capacity?32*1048576:1048576) || fseek(input,0,SEEK_SET)){
    fclose(input);return 1;
  }
  uint8_t *clip=malloc((size_t)size);
  int ok=clip && fread(clip,1,(size_t)size,input)==(size_t)size;fclose(input);
  if(capacity){
    ok=ok && capacity_case(clip,(size_t)size);free(clip);return ok?0:1;
  }
  if(replay_cost){
    ok=ok && replay_cost_case(clip,(size_t)size);free(clip);return ok?0:1;
  }
  for(int missing=0;missing<2;missing++) for(int late=0;late<2;late++)
    if(ok && !rollback_case(clip,(size_t)size,missing,late)) ok=0;
  for(int redraw=0;redraw<2;redraw++)
    if(ok && !restore_case(clip,(size_t)size,redraw)) ok=0;
  if(ok && !callback_case(clip,(size_t)size)) ok=0;
  free(clip);
  if(ok) puts("engine video: root rollback, immutable media, exact A/V, redraw, parked Social callback and Reset pass");
  return ok?0:1;
}
