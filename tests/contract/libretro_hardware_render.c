/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* What the adapter asks the frontend for, what it does with the answer, and which video callback
 * carries the frame.
 *
 * Every scenario here is one a real frontend produces: the setting left alone, the setting turned
 * on with no context available, a host that prefers embedded graphics, a host that prefers desktop
 * graphics, a context that arrives and then arrives again without going away in between, and a
 * context that is taken away properly. The engine is stubbed, so what is under test is the
 * translation and nothing else. */
#include "libretro_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void retro_set_environment(retro_environment_t callback);
void retro_set_video_refresh(retro_video_refresh_t callback);
void retro_init(void);
void retro_deinit(void);
void retro_run(void);
bool retro_load_game(const struct retro_game_info *info);
void retro_unload_game(void);

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"hardware render contract failed: %s\n",label); \
    return 1; \
  } \
}while(0)

static const char *option_value;
static int preferred_available;
static unsigned preferred_context;
static int accept_desktop;
static int accept_embedded;
static struct retro_hw_render_callback declared;
static int declared_count;
static unsigned rejected_requests;

static int reset_calls;
static int destroy_calls;
static uint32_t destroy_current;
static AnygmGraphicsContext adopted;
static AnygmResult reset_result;

static uint32_t frame_flags;
static const void *last_video_pixels;
static unsigned last_video_width;
static int video_calls;

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
    case RETRO_ENVIRONMENT_GET_VARIABLE:{
      struct retro_variable *variable=data;
      if(!variable || !variable->key) return false;
      if(!strcmp(variable->key,"anygm_hybrid_gpu")){
        variable->value=option_value;
        return option_value!=NULL;
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
      declared_count++;
      if((request->context_type==RETRO_HW_CONTEXT_OPENGL_CORE && accept_desktop) ||
         (request->context_type==RETRO_HW_CONTEXT_OPENGLES3 && accept_embedded)){
        request->get_proc_address=frontend_get_proc;
        request->get_current_framebuffer=frontend_get_framebuffer;
        declared=*request;
        return true;
      }
      rejected_requests++;
      return false; }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
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
}

/* The stubbed engine. */
AnygmResult anygm_create(const AnygmHostServices *services,AnygmEngine **engine){
  (void)services;
  if(!engine) return ANYGM_ERROR_INVALID_ARGUMENT;
  *engine=(AnygmEngine*)(uintptr_t)1u;
  return ANYGM_OK;
}
void anygm_destroy(AnygmEngine *engine){ (void)engine; }
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
  info->max_width=320;
  info->max_height=240;
  info->aspect_ratio=4.0/3.0;
  info->frames_per_second=60.0;
  info->audio_rate=44100;
  return ANYGM_OK;
}
AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output){
  (void)engine; (void)input;
  if(!output) return ANYGM_ERROR_INVALID_ARGUMENT;
  output->pixels=(const void*)(uintptr_t)0x1000u;
  output->width=320;
  output->height=240;
  output->pitch=320*4;
  output->pixel_format=ANYGM_PIXEL_XRGB8888;
  output->flags=frame_flags;
  return ANYGM_OK;
}
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine; (void)delta; return ANYGM_OK;
}
AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression){
  (void)engine; (void)slot; (void)enabled; (void)expression; return ANYGM_OK;
}
size_t anygm_state_size(AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_resume_size(AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_capacity_hint(const AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_resume_capacity_hint(const AnygmEngine *engine){ (void)engine; return 0; }
AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t capacity,size_t *written){
  (void)engine; (void)data; (void)capacity;
  if(written) *written=0;
  return ANYGM_OK;
}
AnygmResult anygm_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,
                                        size_t *written){
  (void)engine; (void)data; (void)capacity;
  if(written) *written=0;
  return ANYGM_OK;
}
AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size){
  (void)engine; (void)data; (void)size; return ANYGM_OK;
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
  preferred_available=available;
  preferred_context=preferred;
  accept_desktop=desktop;
  accept_embedded=embedded;
  declared_count=0;
  rejected_requests=0;
  reset_calls=0;
  destroy_calls=0;
  destroy_current=0xFFFFFFFFu;
  reset_result=ANYGM_OK;
  frame_flags=0;
  video_calls=0;
  last_video_pixels=NULL;
  proc_requests=0;
  framebuffer_requests=0;
  memset(&declared,0,sizeof declared);
  memset(&adopted,0,sizeof adopted);
  retro_set_environment(environment_callback);
  retro_set_video_refresh(video_callback);
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
  finish();
  begin("None",0,0,1,1);
  REQUIRE(load(),"content loads with the setting at None");
  REQUIRE(declared_count==0,"None asks for no graphics context");
  REQUIRE(reset_calls==0,"no context is adopted");
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
  REQUIRE(destroy_calls==0,"a repeated reset is not a destroy");
  declared.context_destroy();
  REQUIRE(destroy_calls==1,"the destroy reached the engine");
  REQUIRE(destroy_current==1u,"a controlled destroy says the context is still current");
  finish();
  return 0;
}

static int failed_adoption_keeps_software(void){
  begin("OpenGL",0,0,1,1);
  REQUIRE(load(),"content loads");
  reset_result=ANYGM_ERROR_UNSUPPORTED;
  declared.context_reset();
  REQUIRE(reset_calls==1,"the reset was attempted");
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

int main(void){
  if(option_none_negotiates_nothing()) return EXIT_FAILURE;
  if(rejection_keeps_software()) return EXIT_FAILURE;
  if(desktop_is_requested_first()) return EXIT_FAILURE;
  if(embedded_preference_is_followed()) return EXIT_FAILURE;
  if(context_lifecycle_reaches_the_engine()) return EXIT_FAILURE;
  if(failed_adoption_keeps_software()) return EXIT_FAILURE;
  if(sentinel_selects_the_target()) return EXIT_FAILURE;
  if(unload_releases_without_a_current_context()) return EXIT_FAILURE;
  printf("libretro hardware render contract: ok\n");
  return EXIT_SUCCESS;
}
