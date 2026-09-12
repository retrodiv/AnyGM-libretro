/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"
#include "engine_internal.h"
#include "synthetic_content.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int crt_on=1,logical_on=1,changed,negotiation;
static const char *aspect="None";
static unsigned video_width,video_height,geometry_width,geometry_height;

static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS){
    if(negotiation>0) *(uint64_t*)data|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    return negotiation>=0;
  }
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
  if(command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO || command==RETRO_ENVIRONMENT_SET_GEOMETRY){
    const struct retro_game_geometry *g=command==RETRO_ENVIRONMENT_SET_GEOMETRY
      ?data:&((const struct retro_system_av_info*)data)->geometry;
    geometry_width=g->base_width; geometry_height=g->base_height;
    return true;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE){
    *(bool*)data=changed!=0; changed=0; return true;
  }
  if(command==RETRO_ENVIRONMENT_GET_VARIABLE){
    struct retro_variable *v=data;
    v->value=NULL;
    if(!strcmp(v->key,"anygm_render_game_resolution")) v->value=logical_on?"On":"Off";
    if(!strcmp(v->key,"anygm_adjust_crt_tv")) v->value=crt_on?"On":"Off";
    if(!strcmp(v->key,"anygm_aspect_ratio_force")) v->value=aspect;
    if(!strcmp(v->key,"anygm_width_resolution")) v->value="1280";
    if(!strcmp(v->key,"anygm_height_resolution")) v->value="960";
    return v->value!=NULL;
  }
  return false;
}

static void video(const void *pixels,unsigned width,unsigned height,size_t pitch){
  if(pixels && pitch==width*sizeof(uint32_t)){ video_width=width; video_height=height; }
}

#define REQUIRE(condition,label) do { if(!(condition)){ \
  fprintf(stderr,"CRT output failed: %s (%ux%u, aspect %s, CRT %d, logical %d)\n", \
          label,video_width,video_height,aspect,crt_on,logical_on); ok=0; goto done; \
} } while(0)

static int geometry_cases(void){
  static const struct {
    unsigned width,height; const char *aspect; unsigned source_width;
    unsigned host_width,host_height; int x,y,width_fit,height_fit;
  } cases[]={
    {320,240,"None",320,320,240,0,0,320,240},
    {364,244,"None",364,364,244,0,0,364,244},
    {365,244,"None",365,640,480,0,26,640,427},
    {320,245,"None",320,640,480,7,0,626,480},
    {400,200,"None",400,640,480,0,80,640,320},
    {240,320,"None",240,640,480,140,0,360,480},
    {640,480,"None",640,640,480,0,0,640,480},
    {1280,720,"None",1280,640,480,0,60,640,360},
    {1920,1080,"None",1920,640,480,0,60,640,360},
    {320,240,"16:9",424,640,480,0,59,640,362},
    {320,240,"16:10",384,640,480,0,40,640,400},
    {320,240,"21:9",560,640,480,0,103,640,274},
    {400,240,"4:3",320,320,240,0,0,320,240}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    AnygmSyntheticContent fixture={0};
    int ok=1;
    crt_on=logical_on=1; changed=0; negotiation=0; aspect=cases[i].aspect;
    video_width=video_height=geometry_width=geometry_height=0;
    REQUIRE(anygm_synthetic_sized_content_create(&fixture,cases[i].width,cases[i].height),"fixture");
    retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
    struct retro_game_info info={fixture.path,NULL,0,NULL};
    REQUIRE(retro_load_game(&info),"load");
    struct retro_system_av_info av={0}; retro_get_system_av_info(&av);
    REQUIRE(av.geometry.base_width==cases[i].host_width && av.geometry.base_height==cases[i].host_height,
            "cold AV geometry");
    REQUIRE(fabs(av.geometry.aspect_ratio-(double)cases[i].host_width/cases[i].host_height)<1e-6,
            "frontend display aspect");
    retro_run();
    AnygmEngine *engine=g_libretro.engine;
    REQUIRE(video_width==cases[i].host_width && video_height==cases[i].host_height,"delivered geometry");
    REQUIRE(engine->output_width==cases[i].source_width && engine->output_height==cases[i].height,
            "authored or forced raster retained before CRT");
    REQUIRE(engine->host_canvas_x==cases[i].x && engine->host_canvas_y==cases[i].y &&
            engine->host_canvas_width==cases[i].width_fit && engine->host_canvas_height==cases[i].height_fit,
            "centered aspect fit");
    for(size_t p=0;p<(size_t)engine->output_width*engine->output_height;p++) engine->screen[p]=0xFFA04020u;
    const uint32_t *pixels=NULL; unsigned width=0,height=0;
    REQUIRE(resolve_host_frame(engine,&pixels,&width,&height),"compose colored source");
    for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++){
      int inside=(int)x>=cases[i].x && (int)x<cases[i].x+cases[i].width_fit &&
                 (int)y>=cases[i].y && (int)y<cases[i].y+cases[i].height_fit;
      REQUIRE((pixels[(size_t)y*width+x]&0xFFFFFFu)==(inside?0xA04020u:0u),"image and black bars");
    }
done:
    retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
    if(!ok) return 0;
  }
  return 1;
}

static int snapshot(uint8_t **ring,size_t *capacity,size_t initial){
  size_t answer=retro_serialize_size();
  if(!answer || (negotiation<=0 && answer!=initial)) return 0;
  if(answer>*capacity){
    if(negotiation<=0) return 0;
    uint8_t *grown=realloc(*ring,answer);
    if(!grown) return 0;
    *ring=grown; *capacity=answer;
  }
  return retro_serialize(*ring,*capacity); /* Only after the frontend capacity check. */
}

static int live_state_case(int response){
  AnygmSyntheticContent fixture={0};
  uint8_t *ring=NULL,*native=NULL,*adjusted=NULL;
  size_t initial=0,capacity=0,state_size=0,written=0;
  int ok=1;
  crt_on=0; logical_on=1; changed=0; negotiation=response; aspect="None";
  video_width=video_height=geometry_width=geometry_height=0;
  REQUIRE(anygm_synthetic_sized_content_create(&fixture,1920,1080),"state fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"state load");
  initial=capacity=retro_serialize_size(); ring=malloc(capacity);
  REQUIRE(ring && initial && g_libretro.startup_ring_compact,"allocate compact ring before first frame");
  REQUIRE(snapshot(&ring,&capacity,initial),"cold snapshot");
  retro_run();
  REQUIRE(video_width==1920 && video_height==1080 && snapshot(&ring,&capacity,initial),"first snapshot");
  state_size=anygm_state_size(g_libretro.engine);
  native=malloc(state_size); adjusted=malloc(state_size);
  REQUIRE(native && adjusted &&
          anygm_state_save(g_libretro.engine,native,state_size,&written)==ANYGM_OK && written==state_size,
          "native state");
  crt_on=1;
  libretro_options_apply(false);
  REQUIRE(anygm_state_size(g_libretro.engine)==state_size &&
          anygm_state_save(g_libretro.engine,adjusted,state_size,&written)==ANYGM_OK &&
          written==state_size && !memcmp(native,adjusted,state_size),"CRT leaves canonical bytes unchanged");
  retro_run();
  REQUIRE(video_width==640 && video_height==480 && geometry_width==640 && geometry_height==480,
          "live CRT geometry notification");
  REQUIRE(snapshot(&ring,&capacity,initial),"CRT snapshot");
  REQUIRE(retro_unserialize(native,state_size),"restore native state while CRT enabled");
  retro_run();
  REQUIRE(video_width==640 && video_height==480,"restored frame uses current CRT policy");
  logical_on=0; changed=1; retro_run();
  REQUIRE(video_width==1280 && video_height==960 && !g_libretro.engine->host_crt_active,
          "CRT ignored with logical raster off");
  REQUIRE(snapshot(&ring,&capacity,initial),"monitor snapshot");
  logical_on=1; changed=1; retro_run();
  REQUIRE(video_width==640 && video_height==480,"remembered CRT selection");
  retro_reset();
  REQUIRE(snapshot(&ring,&capacity,initial),"cold Reset snapshot in original ring");
  retro_run();
  REQUIRE(video_width==640 && video_height==480 && snapshot(&ring,&capacity,initial),"Reset frame snapshot");
  REQUIRE(retro_unserialize(ring,capacity),"same-execution rewind restore");
  retro_run();
  REQUIRE(video_width==640 && video_height==480,"rewind presentation");
  crt_on=0; changed=1; retro_run();
  REQUIRE(video_width==1920 && video_height==1080 && geometry_width==1920 && geometry_height==1080,
          "live native geometry restored");
  REQUIRE(snapshot(&ring,&capacity,initial),"native snapshot after CRT");
  printf("CRT state transport: negotiation=%d cold=%zu final=%zu\n",response,initial,capacity);
done:
  free(ring); free(native); free(adjusted);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  if(!geometry_cases()) return 1;
  for(int response=-1;response<=1;response++) if(!live_state_case(response)) return 1;
  puts("CRT output: thresholds, aspect, margins, live options, state and Reset passed");
  return 0;
}
