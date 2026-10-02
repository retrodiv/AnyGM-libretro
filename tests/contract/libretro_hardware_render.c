/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* What the adapter asks the frontend for, what it does with the answer, and which video callback
 * carries the frame.
 *
 * Every scenario here is one a real frontend produces: the settings left alone, a request with no
 * context available, a host that prefers embedded graphics, a host that prefers desktop graphics,
 * a context that arrives synchronously or later, one that arrives again without going away in
 * between, and a context that is taken away properly. The engine is stubbed, so what is under test
 * is the translation and nothing else. */
#define _POSIX_C_SOURCE 200809L
#include "libretro_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void retro_set_environment(retro_environment_t callback);
void retro_set_video_refresh(retro_video_refresh_t callback);
void retro_set_audio_sample(retro_audio_sample_t callback);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback);
void retro_set_input_poll(retro_input_poll_t callback);
void retro_init(void);
void retro_deinit(void);
void retro_run(void);
size_t retro_serialize_size(void);
bool retro_serialize(void *data,size_t size);
bool retro_unserialize(const void *data,size_t size);
bool retro_load_game(const struct retro_game_info *info);
void retro_unload_game(void);
void retro_get_system_av_info(struct retro_system_av_info *info);

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"hardware render contract failed: %s\n",label); \
    return 1; \
  } \
}while(0)

static const char *option_value;
static const char *shader_option_value;
static const char *scene_option_value;
static int content_candidate;
static int preferred_available;
static unsigned preferred_context;
static int accept_desktop;
static int accept_embedded;
static int reset_during_request;
static struct retro_hw_render_callback declared;
static int declared_count;
static unsigned rejected_requests;
static unsigned sequence;
static unsigned prepare_sequence;
static unsigned request_sequence;
static unsigned start_sequence;
static unsigned start_calls;
static AnygmConfigDelta latest_config;
static unsigned geometry_updates;
static unsigned system_av_updates;
static struct retro_game_geometry latest_geometry;

static int reset_calls;
static int destroy_calls;
static uint32_t destroy_current;
static AnygmGraphicsContext adopted;
static AnygmResult reset_result;

static uint32_t frame_flags;
static const void *last_video_pixels;
static unsigned last_video_width;
static int video_calls;
static AnygmResult frame_result;
static uint32_t scheduled_steps;
static unsigned audio_calls;
static uint64_t simulated_ns;
static unsigned clock_calls,trace_calls;
static unsigned clock_fail_at;
static uint64_t clock_fail_value;
static char trace_message[768];
static int simulate_cost;
static unsigned state_trace_calls;
static char state_trace_message[768];
static int serialization_reply;
static int simulate_state_cost;
static size_t state_written;
static AnygmResult state_save_result,state_resume_result,state_load_result;

static uint64_t profile_clock(void *userdata){
  if(userdata!=&simulated_ns) return 0;
  clock_calls++;
  if(clock_calls==clock_fail_at) return clock_fail_value;
  return simulated_ns;
}

static void log_callback(enum retro_log_level level,const char *format,...){
  (void)level;
  char message[768];
  va_list arguments;
  va_start(arguments,format);
  vsnprintf(message,sizeof message,format,arguments);
  va_end(arguments);
  if(!strncmp(message,"[libretro-frame] ",17)){
    snprintf(trace_message,sizeof trace_message,"%s",message);
    trace_calls++;
    if(simulate_cost) simulated_ns+=5000000u; /* Deliberately outside the measured call. */
  }
  if(!strncmp(message,"[libretro-state] ",17)){
    snprintf(state_trace_message,sizeof state_trace_message,"%s",message);
    state_trace_calls++;
    if(simulate_state_cost) simulated_ns+=5000000u;
  }
}

static void poll_callback(void){ if(simulate_cost) simulated_ns+=1000000u; }
static size_t audio_callback(const int16_t *data,size_t frames){
  (void)data;
  audio_calls++;
  if(simulate_cost) simulated_ns+=3000000u;
  return frames;
}
static void audio_sample_callback(int16_t left,int16_t right){
  (void)left; (void)right;
  audio_calls++;
  if(simulate_cost) simulated_ns+=1500000u;
}

static int profile_setting(const char *value){
#ifdef _WIN32
  return _putenv_s("GML_PROFILE_LIBRETRO_FRAME",value?value:"")==0;
#else
  return value?setenv("GML_PROFILE_LIBRETRO_FRAME",value,1)==0:
               unsetenv("GML_PROFILE_LIBRETRO_FRAME")==0;
#endif
}

/* Values the adapter must forward untouched. */
static int proc_requests;
static int framebuffer_requests;

static retro_proc_address_t frontend_get_proc(const char *name){
  (void)name;
  proc_requests++;
  return (retro_proc_address_t)(void(*)(void))retro_deinit;
}

static uintptr_t frontend_get_framebuffer(void){
  framebuffer_requests++;
  return 42u;
}

static bool environment_callback(unsigned command,void *data){
  switch(command){
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback*)data)->log=log_callback;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:{
      struct retro_variable *variable=data;
      if(!variable || !variable->key) return false;
      if(!strcmp(variable->key,"anygm_hybrid_gpu")){
        variable->value=option_value;
        return option_value!=NULL;
      }
      if(!strcmp(variable->key,"anygm_scene_renderer")){
        variable->value=scene_option_value;
        return scene_option_value!=NULL;
      }
      if(!strcmp(variable->key,"anygm_content_shader_readback")){
        variable->value=shader_option_value;
        return shader_option_value!=NULL;
      }
      variable->value=NULL;
      return false; }
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
      if(!preferred_available || !data) return false;
      *(unsigned*)data=preferred_context;
      return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:{
      /* A frontend that accepts fills the two resolver fields in the record the core handed it,
       * which is how the core reaches its entry points and its target. */
      struct retro_hw_render_callback *request=data;
      if(!request) return false;
      if(!request_sequence) request_sequence=++sequence;
      declared_count++;
      if((request->context_type==RETRO_HW_CONTEXT_OPENGL_CORE && accept_desktop) ||
         (request->context_type==RETRO_HW_CONTEXT_OPENGLES3 && accept_embedded)){
        request->get_proc_address=frontend_get_proc;
        request->get_current_framebuffer=frontend_get_framebuffer;
        declared=*request;
        if(reset_during_request) request->context_reset();
        return true;
      }
      rejected_requests++;
      return false; }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return true;
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
      if(serialization_reply<0) return false;
      if(serialization_reply>0 && data)
        *(uint64_t*)data|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
      return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
      geometry_updates++;
      if(data) latest_geometry=*(const struct retro_game_geometry*)data;
      return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      system_av_updates++;
      if(data) latest_geometry=((const struct retro_system_av_info*)data)->geometry;
      return true;
    default:
      return false;
  }
}

static void video_callback(const void *data,unsigned width,unsigned height,size_t pitch){
  (void)height;
  (void)pitch;
  video_calls++;
  last_video_pixels=data;
  last_video_width=width;
  if(simulate_cost) simulated_ns+=2000000u;
}

/* The stubbed engine. */
AnygmResult anygm_create(const AnygmHostServices *services,AnygmEngine **engine){
  (void)services;
  if(!engine) return ANYGM_ERROR_INVALID_ARGUMENT;
  *engine=(AnygmEngine*)(uintptr_t)1u;
  return ANYGM_OK;
}
void anygm_destroy(AnygmEngine *engine){ (void)engine; }
AnygmResult anygm_load_prepare(AnygmEngine *engine,const AnygmContentSource *source,
                               const AnygmLoadConfig *config,AnygmContentInfo *info){
  (void)engine; (void)source; (void)config;
  prepare_sequence=++sequence;
  if(info) info->flags=content_candidate?ANYGM_CONTENT_GLSL_DEVICE_CANDIDATE:0u;
  return ANYGM_OK;
}
AnygmResult anygm_load_start(AnygmEngine *engine){
  (void)engine;
  start_calls++;
  if(!start_sequence) start_sequence=++sequence;
  return ANYGM_OK;
}
AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  (void)engine; (void)source; (void)config;
  return ANYGM_OK;
}
void anygm_unload(AnygmEngine *engine){ (void)engine; }
AnygmResult anygm_reset(AnygmEngine *engine){ (void)engine; return ANYGM_OK; }
AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info){
  (void)engine;
  if(!info) return ANYGM_ERROR_INVALID_ARGUMENT;
  info->base_width=320;
  info->base_height=240;
  info->max_width=3840;
  info->max_height=2160;
  info->aspect_ratio=4.0/3.0;
  info->frames_per_second=60.0;
  info->audio_rate=44100;
  return ANYGM_OK;
}
AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output){
  (void)engine; (void)input;
  if(!output) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(simulate_cost) simulated_ns+=7000000u;
  if(frame_result!=ANYGM_OK) return frame_result;
  static const int16_t audio[4]={1,2,3,4};
  output->pixels=(const void*)(uintptr_t)0x1000u;
  output->width=320;
  output->height=240;
  output->pitch=320*4;
  output->pixel_format=ANYGM_PIXEL_XRGB8888;
  output->flags=frame_flags;
  output->audio=audio;
  output->audio_frames=2;
  return ANYGM_OK;
}
AnygmResult anygm_schedule_frame(AnygmEngine *engine,double hz,uint32_t *steps){
  (void)engine; (void)hz; *steps=scheduled_steps; return ANYGM_OK;
}
AnygmResult anygm_repeat_frame(AnygmEngine *engine,AnygmFrameOutput *output){
  AnygmResult result=anygm_run_frame(engine,NULL,output);
  if(result==ANYGM_OK) output->audio_frames=0;
  return result;
}
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine;
  if(delta) latest_config=*delta;
  return ANYGM_OK;
}
AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression){
  (void)engine; (void)slot; (void)enabled; (void)expression; return ANYGM_OK;
}
size_t anygm_state_size(AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_resume_size(AnygmEngine *engine){
  (void)engine;
  if(simulate_state_cost) simulated_ns+=3000000u;
  return 0;
}
size_t anygm_state_capacity_hint(const AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_resume_capacity_hint(const AnygmEngine *engine){ (void)engine; return 0; }
uint32_t anygm_state_capacity_flags(const AnygmEngine *engine){ (void)engine; return 0; }
AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written){
  (void)engine; (void)data; (void)capacity;
  if(simulate_state_cost) simulated_ns+=4000000u;
  if(written) *written=state_written;
  return state_save_result;
}
AnygmResult anygm_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,
                                        size_t *written){
  (void)engine; (void)data; (void)capacity;
  if(simulate_state_cost) simulated_ns+=6000000u;
  if(written) *written=state_written;
  return state_resume_result;
}
AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size){
  (void)engine; (void)data; (void)size;
  if(simulate_state_cost) simulated_ns+=7000000u;
  return state_load_result;
}
size_t anygm_get_last_error(const AnygmEngine *engine,char *buffer,size_t capacity){
  (void)engine;
  if(!buffer || !capacity) return 0;
  snprintf(buffer,capacity,"stub");
  return strlen(buffer);
}
AnygmResult anygm_get_room_count(const AnygmEngine *engine,uint32_t *count){
  (void)engine;
  if(count) *count=0;
  return ANYGM_OK;
}
AnygmResult anygm_get_room_name(const AnygmEngine *engine,uint32_t index,char *name,size_t capacity){
  (void)engine; (void)index;
  if(name && capacity) name[0]='\0';
  return ANYGM_ERROR_INVALID_ARGUMENT;
}
uint32_t anygm_api_version(void){ return ANYGM_API_VERSION; }

AnygmResult anygm_graphics_context_reset(AnygmEngine *engine,const AnygmGraphicsContext *context){
  (void)engine;
  ++sequence;
  reset_calls++;
  if(context) adopted=*context;
  return reset_result;
}
void anygm_graphics_context_destroy(AnygmEngine *engine,uint32_t context_is_current){
  (void)engine;
  destroy_calls++;
  destroy_current=context_is_current;
}

static void begin(const char *value,int available,unsigned preferred,int desktop,int embedded){
  option_value=value;
  shader_option_value=NULL;
  scene_option_value=NULL;
  content_candidate=0;
  preferred_available=available;
  preferred_context=preferred;
  accept_desktop=desktop;
  accept_embedded=embedded;
  reset_during_request=0;
  declared_count=0;
  rejected_requests=0;
  sequence=0;
  prepare_sequence=0;
  request_sequence=0;
  start_sequence=0;
  start_calls=0;
  memset(&latest_config,0,sizeof latest_config);
  geometry_updates=0;
  system_av_updates=0;
  memset(&latest_geometry,0,sizeof latest_geometry);
  reset_calls=0;
  destroy_calls=0;
  destroy_current=0xFFFFFFFFu;
  reset_result=ANYGM_OK;
  frame_flags=0;
  video_calls=0;
  audio_calls=0;
  frame_result=ANYGM_OK;
  scheduled_steps=1;
  simulated_ns=1000000000u;
  clock_calls=trace_calls=0;
  clock_fail_at=0;
  clock_fail_value=0;
  trace_message[0]='\0';
  simulate_cost=0;
  state_trace_calls=0;
  state_trace_message[0]='\0';
  simulate_state_cost=0;
  state_written=0;
  state_save_result=state_resume_result=state_load_result=ANYGM_OK;
  last_video_pixels=NULL;
  proc_requests=0;
  framebuffer_requests=0;
  memset(&declared,0,sizeof declared);
  memset(&adopted,0,sizeof adopted);
  retro_set_environment(environment_callback);
  retro_set_video_refresh(video_callback);
  retro_set_audio_sample(NULL);
  retro_set_audio_sample_batch(NULL);
  retro_set_input_poll(NULL);
  retro_init();
}

static void finish(void){
  retro_unload_game();
  retro_deinit();
}

static bool load(void){
  struct retro_game_info info;
  memset(&info,0,sizeof info);
  info.path="content";
  return retro_load_game(&info);
}

static int option_none_negotiates_nothing(void){
  begin(NULL,0,0,1,1);
  REQUIRE(load(),"content loads with the setting unset");
  REQUIRE(declared_count==0,"an unset setting asks for no graphics context");
  REQUIRE(start_calls==1,"software content starts during load");
  finish();
  begin("None",0,0,1,1);
  REQUIRE(load(),"content loads with the setting at None");
  REQUIRE(declared_count==0,"None asks for no graphics context");
  REQUIRE(reset_calls==0,"no context is adopted");
  REQUIRE(start_calls==1,"None starts without waiting for a context");
  finish();
  /* The setting is a selector, so it can hold a backend this build does not implement -- a newer
   * core's saved value read back by an older one. That must fall to the software renderer rather
   * than to whichever backend this build happens to have. */
  begin("Vulkan",0,0,1,1);
  REQUIRE(load(),"content loads with a backend this build does not implement");
  REQUIRE(declared_count==0,"an unimplemented backend asks for no graphics context");
  REQUIRE(reset_calls==0,"no context is adopted");
  finish();
  return 0;
}

static int rejection_keeps_software(void){
  begin("OpenGL",0,0,0,0);
  REQUIRE(load(),"content still loads when no context is available");
  REQUIRE(declared_count==2,"both families are offered before giving up");
  REQUIRE(rejected_requests==2,"the frontend refused both");
  REQUIRE(reset_calls==0,"nothing was adopted");
  REQUIRE(start_calls==1,"a rejected request starts the software runtime");
  /* The ordinary pixel callback still carries the frame. */
  retro_run();
  REQUIRE(video_calls==1,"a frame was presented");
  REQUIRE(last_video_pixels==(const void*)(uintptr_t)0x1000u,"the CPU frame was presented");
  finish();
  return 0;
}

static int desktop_is_requested_first(void){
  begin("OpenGL",0,0,1,1);
  REQUIRE(load(),"content loads");
  REQUIRE(declared_count==1,"the first request was accepted");
  REQUIRE(prepare_sequence && request_sequence && prepare_sequence<request_sequence,
          "content is inspected before a context is requested");
  REQUIRE(start_calls==0,"authored events wait for the accepted context");
  REQUIRE(declared.context_type==RETRO_HW_CONTEXT_OPENGL_CORE,"desktop graphics were requested");
  REQUIRE(declared.version_major==3 && declared.version_minor==3,"the requested version is stated");
  REQUIRE(declared.bottom_left_origin,"the frame is declared in the framebuffer's own row order");
  REQUIRE(!declared.cache_context,"the context is not cached, so every reset is announced");
  REQUIRE(!declared.depth && !declared.stencil,"no depth or stencil buffer is requested");
  REQUIRE(declared.context_reset && declared.context_destroy,"both lifecycle callbacks are set");
  finish();
  return 0;
}

static int embedded_preference_is_followed(void){
  begin("OpenGL",1,RETRO_HW_CONTEXT_OPENGLES3,1,1);
  REQUIRE(load(),"content loads");
  REQUIRE(declared.context_type==RETRO_HW_CONTEXT_OPENGLES3,"the stated preference was followed");
  REQUIRE(declared.version_major==3,"an embedded major version is requested");
  finish();
  /* A host that prefers embedded graphics but cannot provide them still gets asked for the other
   * family before the core gives up. */
  begin("OpenGL",1,RETRO_HW_CONTEXT_OPENGLES3,1,0);
  REQUIRE(load(),"content loads");
  REQUIRE(declared_count==2,"the second family was offered");
  REQUIRE(declared.context_type==RETRO_HW_CONTEXT_OPENGL_CORE,"the fallback family was accepted");
  finish();
  return 0;
}

static int context_lifecycle_reaches_the_engine(void){
  begin("OpenGL",0,0,1,1);
  REQUIRE(load(),"content loads");
  REQUIRE(declared.get_proc_address && declared.get_current_framebuffer,
          "the frontend filled in its resolvers");
  declared.context_reset();
  REQUIRE(reset_calls==1,"the reset reached the engine");
  REQUIRE(start_calls==1 && start_sequence>request_sequence,
          "authored events start only after context adoption");
  REQUIRE(adopted.struct_size==sizeof adopted,"the context declares its own size");
  REQUIRE(adopted.api==ANYGM_GRAPHICS_OPENGL_CORE,"the graphics family was translated");
  REQUIRE(adopted.get_proc_address && adopted.get_current_framebuffer,
          "both callbacks were supplied");
  REQUIRE(adopted.get_proc_address(adopted.userdata,"glClear")!=NULL,
          "entry points are resolved through the frontend");
  REQUIRE(proc_requests==1,"the request reached the frontend unchanged");
  REQUIRE(adopted.get_current_framebuffer(adopted.userdata)==42u,
          "the current target is the frontend's answer");
  REQUIRE(framebuffer_requests==1,"the target is asked for, not remembered");
  /* A second reset with no destroy in between: the frontend is telling the core its previous
   * context is gone. */
  declared.context_reset();
  REQUIRE(reset_calls==2,"a repeated reset is forwarded");
  REQUIRE(start_calls==1,"a context recreation does not boot content twice");
  REQUIRE(destroy_calls==0,"a repeated reset is not a destroy");
  declared.context_destroy();
  REQUIRE(destroy_calls==1,"the destroy reached the engine");
  REQUIRE(destroy_current==1u,"a controlled destroy says the context is still current");
  finish();
  return 0;
}

static int synchronous_context_starts_once(void){
  begin("OpenGL",0,0,1,1);
  reset_during_request=1;
  REQUIRE(load(),"content loads when the frontend creates the context synchronously");
  REQUIRE(reset_calls==1 && start_calls==1,
          "a synchronous context starts the prepared runtime exactly once");
  retro_run();
  REQUIRE(video_calls==1,"the synchronously started runtime presents a frame");
  finish();
  return 0;
}

static int provisional_av_is_corrected(void){
  struct retro_system_av_info av;
  begin("OpenGL",0,0,1,1);
  REQUIRE(load(),"content prepares before the deferred AV query");
  memset(&av,0,sizeof av);
  retro_get_system_av_info(&av);
  REQUIRE(av.geometry.base_width==288u && av.geometry.base_height==216u,
          "a prepared runtime exposes the bounded fallback geometry");
  declared.context_reset();
  REQUIRE(start_calls==1,"the deferred context starts content after the AV query");
  retro_run();
  REQUIRE(geometry_updates==1 && system_av_updates==0 &&
          latest_geometry.base_width==320u && latest_geometry.base_height==240u,
          "the first frame replaces provisional geometry with the started runtime's geometry");
  finish();
  return 0;
}

static int failed_adoption_keeps_software(void){
  begin("OpenGL",0,0,1,1);
  REQUIRE(load(),"content loads");
  reset_result=ANYGM_ERROR_UNSUPPORTED;
  declared.context_reset();
  REQUIRE(reset_calls==1,"the reset was attempted");
  REQUIRE(start_calls==1,"failed adoption starts the documented software fallback");
  /* The engine reports a CPU frame, so the ordinary callback carries it even though the frontend
   * granted a context. */
  frame_flags=0;
  retro_run();
  REQUIRE(last_video_pixels==(const void*)(uintptr_t)0x1000u,
          "a software frame under an accepted context is still presented");
  finish();
  return 0;
}

static int sentinel_selects_the_target(void){
  begin("OpenGL",0,0,1,1);
  REQUIRE(load(),"content loads");
  declared.context_reset();
  frame_flags=ANYGM_FRAME_HARDWARE_TARGET;
  retro_run();
  REQUIRE(video_calls==1,"a frame was presented");
  REQUIRE(last_video_pixels==RETRO_HW_FRAME_BUFFER_VALID,
          "a hardware frame is announced with the sentinel");
  REQUIRE(last_video_width==320,"the announced extent is the rendered one");
  frame_flags=0;
  retro_run();
  REQUIRE(last_video_pixels==(const void*)(uintptr_t)0x1000u,
          "a CPU frame in the same session uses the pixel callback");
  finish();
  return 0;
}

static int content_glsl_requests_only_for_candidates(void){
  begin("None",0,0,1,1);
  shader_option_value="Performance";
  content_candidate=0;
  REQUIRE(load(),"content with no GLSL candidate loads");
  REQUIRE(declared_count==0,"GLSL mode alone requests no context without a candidate");
  REQUIRE(start_calls==1,"candidate-free content starts immediately");
  finish();

  begin("None",0,0,1,1);
  shader_option_value="Exact";
  content_candidate=1;
  REQUIRE(load(),"content with a GLSL candidate prepares");
  REQUIRE(declared_count==1,"GLSL mode requests a context for a candidate");
  REQUIRE(start_calls==0,"candidate content waits for context adoption");
  REQUIRE(latest_config.values.content_shader_device_expected==1u &&
          latest_config.values.hybrid_gpu_presentation==0u,
          "GLSL can select the device without hybrid presentation");
  declared.context_reset();
  REQUIRE(start_calls==1,"candidate content starts after adoption");
  finish();

  begin("None",0,0,0,0);
  shader_option_value="Performance";
  content_candidate=1;
  REQUIRE(load(),"candidate content falls back after rejection");
  REQUIRE(start_calls==1 && latest_config.values.content_shader_device_expected==0u &&
          latest_config.values.content_shader_readback==ANYGM_SHADER_READBACK_NEVER &&
          latest_config.values.report_all_shaders_compiled==1u,
          "a rejected GLSL request uses Off but report available");
  finish();

  begin("OpenGL",0,0,1,1);
  shader_option_value="Off";
  content_candidate=1;
  REQUIRE(load(),"hybrid-only content prepares");
  REQUIRE(latest_config.values.content_shader_device_expected==0u &&
          latest_config.values.hybrid_gpu_presentation==1u &&
          latest_config.values.report_all_shaders_compiled==0u,
          "hybrid presentation does not enable GLSL or change strict Off reporting");
  declared.context_reset();
  finish();
  return 0;
}

static int scene_renderer_requests_context_independently(void){
  begin("None",0,0,1,1);
  scene_option_value="unrecognized";
  REQUIRE(load(),"unknown scene setting leaves the CPU path available");
  REQUIRE(declared_count==0 && start_calls==1 &&
          latest_config.values.scene_renderer==ANYGM_SCENE_RENDERER_CPU,
          "unknown scene setting requests no graphics context");
  finish();

  begin("None",0,0,1,1);
  scene_option_value="OpenGL";
  REQUIRE(load(),"scene-only content prepares");
  REQUIRE(declared_count==1 && start_calls==0,
          "scene option requests a context without hybrid presentation or GLSL");
  REQUIRE(latest_config.values.scene_renderer==ANYGM_SCENE_RENDERER_OPENGL &&
          latest_config.values.hybrid_gpu_presentation==0u &&
          latest_config.values.content_shader_device_expected==0u,
          "scene intent remains independent of other graphics options");
  declared.context_reset();
  REQUIRE(start_calls==1,"scene content starts after context adoption");
  finish();

  begin("None",0,0,0,0);
  scene_option_value="OpenGL";
  REQUIRE(load(),"scene content starts when the frontend refuses hardware rendering");
  REQUIRE(declared_count>=1 && start_calls==1,
          "a refused scene context starts the software path");
  REQUIRE(latest_config.values.scene_renderer==ANYGM_SCENE_RENDERER_OPENGL &&
          latest_config.values.hybrid_gpu_presentation==0u,
          "the requested scene policy survives refusal for fallback diagnostics");
  finish();
  return 0;
}

static int unload_releases_without_a_current_context(void){
  begin("OpenGL",0,0,1,1);
  REQUIRE(load(),"content loads");
  declared.context_reset();
  REQUIRE(reset_calls==1,"context adopted");
  retro_unload_game();
  REQUIRE(destroy_calls==1,"unloading releases what was adopted");
  REQUIRE(destroy_current==0u,
          "content going away does not promise the context is still current");
  /* Reloading negotiates again from the setting as it stands now. */
  destroy_calls=0;
  option_value="None";
  declared_count=0;
  REQUIRE(load(),"content loads again");
  REQUIRE(declared_count==0,"the setting is read again at load");
  retro_deinit();
  return 0;
}

static int whole_call_profile_contract(void){
  REQUIRE(profile_setting(NULL),"clear optional frame profiling");
  begin(NULL,0,0,1,1);
  REQUIRE(load(),"default profile content loads");
  g_libretro.frame_profile_clock=profile_clock;
  g_libretro.frame_profile_userdata=&simulated_ns;
  retro_run();
  REQUIRE(!g_libretro.frame_profile_enabled && !clock_calls && !trace_calls,
          "disabled profiling neither samples nor logs");
  retro_unload_game();
  REQUIRE(profile_setting("1"),"enable optional frame profiling");
  REQUIRE(load(),"reload re-reads profile setting");
  REQUIRE(g_libretro.frame_profile_enabled,"loaded session enables profiling");
  g_libretro.frame_profile_clock=profile_clock;
  g_libretro.frame_profile_userdata=&simulated_ns;
  simulate_cost=1;
  retro_set_input_poll(poll_callback);
  retro_set_audio_sample_batch(audio_callback);
  frame_flags=ANYGM_FRAME_HARDWARE_TARGET;
  retro_run();
  REQUIRE(trace_calls==1 && strstr(trace_message,"tick=1 valid=1 total_ns=13000000 ") &&
          strstr(trace_message,"video_ns=2000000 audio_ns=3000000 ") &&
          strstr(trace_message,"period_valid=0 period_ns=0 ") &&
          strstr(trace_message,"logical_steps=1 result=0"),
          "whole call includes input, sealed engine return, video and audio");
  REQUIRE(last_video_pixels==RETRO_HW_FRAME_BUFFER_VALID && audio_calls==1,
          "profiling preserves actual hardware and audio callbacks");
  simulated_ns+=17000000u;
  retro_run();
  REQUIRE(trace_calls==2 && strstr(trace_message,"tick=2 valid=1 total_ns=13000000 ") &&
          strstr(trace_message,"period_valid=1 period_ns=35000000 "),
          "entry period includes outside-call and prior trace-log time");
  retro_set_audio_sample_batch(NULL);
  retro_set_audio_sample(audio_sample_callback);
  retro_run();
  REQUIRE(strstr(trace_message,"audio_ns=3000000 ") && audio_calls==4,
          "sample callbacks retain their full aggregate cost");
  g_libretro.display_active_hz=60;
  scheduled_steps=2;
  retro_run();
  REQUIRE(strstr(trace_message,"total_ns=23000000 ") &&
          strstr(trace_message,"video_ns=2000000 audio_ns=6000000 ") &&
          strstr(trace_message,"logical_steps=2 result=0"),
          "one frontend tick includes both logical steps and only final video");
  scheduled_steps=0;
  retro_run();
  REQUIRE(strstr(trace_message,"total_ns=10000000 ") &&
          strstr(trace_message,"video_ns=2000000 audio_ns=0 ") &&
          strstr(trace_message,"logical_steps=0 result=0"),
          "zero-step repetition has its own tick without invented audio or steps");
  g_libretro.display_active_hz=0;
  scheduled_steps=1;
  frame_result=ANYGM_ERROR_INTERNAL;
  retro_run();
  REQUIRE(trace_calls==6 && strstr(trace_message,"total_ns=10000000 ") &&
          strstr(trace_message,"video_ns=2000000 audio_ns=0 ") &&
          strstr(trace_message,"logical_steps=0 result="),
          "failed frame includes duplicate callback but no nonexistent audio");
  frame_result=ANYGM_OK;
  clock_fail_at=clock_calls+2;
  retro_run();
  REQUIRE(strstr(trace_message,"valid=0 total_ns=0 video_ns=0 audio_ns=0 "),
          "a failed callback clock invalidates the whole sample rather than hiding its cost");
  clock_fail_at=clock_calls+2;
  clock_fail_value=1;
  retro_run();
  REQUIRE(strstr(trace_message,"valid=0 total_ns=0 video_ns=0 audio_ns=0 "),
          "a backward callback-start clock cannot inflate callback cost");
  clock_fail_at=0;
  g_libretro.audio_enabled=false;
  retro_set_video_refresh(NULL);
  retro_run();
  REQUIRE(strstr(trace_message,"total_ns=8000000 video_ns=0 audio_ns=0 "),
          "absent video and disabled audio add no callback duration");
  g_libretro.audio_enabled=true;
  retro_set_video_refresh(video_callback);
  simulated_ns=0;
  simulate_cost=0;
  retro_run();
  REQUIRE(strstr(trace_message,"valid=0 total_ns=0 ") &&
          strstr(trace_message,"period_valid=0 period_ns=0 "),
          "unavailable clock does not manufacture a duration");
  simulated_ns=1000;
  retro_run();
  REQUIRE(strstr(trace_message,"valid=1 ") && strstr(trace_message,"period_valid=0 "),
          "clock recovery starts a fresh period");
  simulated_ns=500;
  retro_run();
  REQUIRE(strstr(trace_message,"period_valid=0 period_ns=0 "),
          "backward entry clock never wraps the period");
  g_libretro.frame_profile_clock=NULL;
  retro_run();
  REQUIRE(strstr(trace_message,"valid=0 total_ns=0 "),"null clock is rejected safely");
  retro_unload_game();
  unsigned previous_traces=trace_calls;
  retro_run();
  REQUIRE(trace_calls==previous_traces,"unloaded ticks are not content samples");
  REQUIRE(load(),"profile session reloads again");
  g_libretro.frame_profile_clock=profile_clock;
  g_libretro.frame_profile_userdata=&simulated_ns;
  retro_run();
  REQUIRE(strstr(trace_message,"tick=1 valid=1 ") && strstr(trace_message,"period_valid=0 "),
          "reloaded content resets tick identity and previous entry");
  retro_unload_game();
  REQUIRE(profile_setting("0"),"explicitly disable optional profile");
  REQUIRE(load() && !g_libretro.frame_profile_enabled,"zero is disabled on a fresh load");
  finish();
  REQUIRE(profile_setting(NULL),"restore optional profile environment");
  return 0;
}

static int state_call_profile_contract(void){
  uint8_t state[64];
  serialization_reply=0;
  REQUIRE(profile_setting(NULL),"clear state call profiling");
  begin(NULL,0,0,1,1);
  REQUIRE(load(),"unprofiled state session loads");
  g_libretro.frame_profile_clock=profile_clock;
  g_libretro.frame_profile_userdata=&simulated_ns;
  REQUIRE(retro_serialize_size()>0 && retro_serialize(state,sizeof state) &&
          retro_unserialize(state,sizeof state),"unprofiled state operations work");
  REQUIRE(!clock_calls && !state_trace_calls,"disabled state profiling does not sample or log");
  finish();
  for(int reply=-1;reply<=1;reply++){
    REQUIRE(profile_setting("1"),"enable existing call-boundary profiler");
    serialization_reply=reply;
    begin(NULL,0,0,1,1);
    REQUIRE(load(),"profiled state session loads");
    g_libretro.frame_profile_clock=profile_clock;
    g_libretro.frame_profile_userdata=&simulated_ns;
    simulate_state_cost=1;
    REQUIRE(retro_serialize_size()==4194304u,"capacity answer is preserved");
    REQUIRE(state_trace_calls==1 && strstr(state_trace_message,"op=size tick=0 valid=1 ") &&
            strstr(state_trace_message,"total_ns=3000000 capacity=0 bytes=4194304 success=1"),
            "cold query records the actual answer and excludes its own log cost");
    REQUIRE(g_libretro.variable_state_supported==(reply>0) &&
            strstr(state_trace_message,reply>0?"variable=1":"variable=0"),
            "only explicit frontend acknowledgement permits variable sizes");
    REQUIRE(retro_serialize_size()==4194304u,"repeated capacity answer is preserved");
    REQUIRE(strstr(state_trace_message,reply>0?"total_ns=3000000 ":"total_ns=0 "),
            "cached fixed query is distinguished from variable remeasurement");
    state_written=32;
    memset(state,0xA5,sizeof state);
    REQUIRE(retro_serialize(state,sizeof state),"small supplied slot is accepted");
    REQUIRE(strstr(state_trace_message,"op=save tick=0 valid=1 total_ns=6000000 ") &&
            strstr(state_trace_message,"capacity=64 bytes=32 success=1"),
            "actual compact write reports offered capacity and logical written bytes");
    for(size_t i=32;i<sizeof state;i++) REQUIRE(!state[i],"successful slot padding is preserved");
    state_resume_result=ANYGM_ERROR_INTERNAL;
    REQUIRE(!retro_serialize(state,sizeof state),"failed writer remains rejected");
    REQUIRE(strstr(state_trace_message,"capacity=64 bytes=32 success=0"),
            "failed serialization is reported without converting it to success");
    state_resume_result=ANYGM_OK;
    state_written=sizeof state+1u;
    REQUIRE(!retro_serialize(state,sizeof state),"oversized reported write stays rejected");
    REQUIRE(strstr(state_trace_message,"capacity=64 bytes=65 success=0"),
            "overflow is visible even when the writer reports success");
    state_written=32;
    g_libretro.frame_completed=true;
    g_libretro.startup_ring_compact=true;
    g_libretro.startup_resume_capacity=sizeof state;
    state_save_result=ANYGM_ERROR_INTERNAL;
    REQUIRE(retro_serialize(state,sizeof state),"complete-first retry keeps its policy");
    REQUIRE(strstr(state_trace_message,"total_ns=10000000 ") &&
            strstr(state_trace_message,"success=1"),"envelope includes both attempted writers");
    REQUIRE(retro_unserialize(state,sizeof state),"successful load remains accepted");
    REQUIRE(strstr(state_trace_message,"op=load tick=0 valid=1 total_ns=7000000 ") &&
            strstr(state_trace_message,"capacity=64 bytes=64 success=1"),
            "load reports its actual supplied bytes and return value");
    state_load_result=ANYGM_ERROR_INTERNAL;
    REQUIRE(!retro_unserialize(state,sizeof state),"rejected load remains rejected");
    REQUIRE(strstr(state_trace_message,"success=0"),"load rejection is visible");
    state_load_result=ANYGM_OK;
    g_libretro.reset_pending_frame=true;
    REQUIRE(!retro_unserialize(state,sizeof state),"pre-reset compact slot stays rejected");
    REQUIRE(strstr(state_trace_message,"success=0"),"pre-reset early refusal is recorded");
    g_libretro.reset_pending_frame=false;
    clock_fail_at=clock_calls+2;
    REQUIRE(retro_unserialize(state,sizeof state),"clock failure does not change load success");
    REQUIRE(strstr(state_trace_message,"valid=0 total_ns=0 ") &&
            strstr(state_trace_message,"success=1"),"invalid clock is not a transport rejection");
    clock_fail_at=0;
    clock_fail_value=1;
    clock_fail_at=clock_calls+2;
    REQUIRE(retro_unserialize(state,sizeof state),"backward clock does not change load success");
    REQUIRE(strstr(state_trace_message,"valid=0 total_ns=0 "),"backward duration never wraps");
    finish();
    unsigned previous=state_trace_calls;
    REQUIRE(!retro_serialize_size() && !retro_serialize(state,sizeof state) &&
            !retro_unserialize(state,sizeof state),"unloaded state calls stay rejected");
    REQUIRE(state_trace_calls==previous,"unloaded session produces no state samples");
  }
  REQUIRE(profile_setting(NULL),"restore call-boundary profiler environment");
  return 0;
}

int main(void){
  if(option_none_negotiates_nothing()) return EXIT_FAILURE;
  if(rejection_keeps_software()) return EXIT_FAILURE;
  if(desktop_is_requested_first()) return EXIT_FAILURE;
  if(embedded_preference_is_followed()) return EXIT_FAILURE;
  if(context_lifecycle_reaches_the_engine()) return EXIT_FAILURE;
  if(synchronous_context_starts_once()) return EXIT_FAILURE;
  if(provisional_av_is_corrected()) return EXIT_FAILURE;
  if(failed_adoption_keeps_software()) return EXIT_FAILURE;
  if(sentinel_selects_the_target()) return EXIT_FAILURE;
  if(content_glsl_requests_only_for_candidates()) return EXIT_FAILURE;
  if(scene_renderer_requests_context_independently()) return EXIT_FAILURE;
  if(unload_releases_without_a_current_context()) return EXIT_FAILURE;
  if(whole_call_profile_contract()) return EXIT_FAILURE;
  if(state_call_profile_contract()) return EXIT_FAILURE;
  printf("libretro hardware render contract: ok\n");
  return EXIT_SUCCESS;
}
