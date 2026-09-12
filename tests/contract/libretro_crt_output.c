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
    /* The dimension boundary alone is no longer enough: padding this wide raster crosses it. */
    {364,244,"None",364,640,480,0,25,640,429},
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
    {400,240,"4:3",320,320,240,0,0,320,240},
    {320,180,"None",320,320,240,0,30,320,180},
    {360,180,"None",360,640,480,0,80,640,320},
    {320,200,"None",320,320,240,0,20,320,200},
    {240,240,"None",240,320,240,40,0,240,240},
    {241,241,"None",241,322,241,40,0,241,241},
    {321,180,"None",321,321,241,0,30,321,180},
    {320,181,"None",320,320,240,0,29,320,181},
    {324,180,"None",324,324,243,0,31,324,180},
    {325,180,"None",325,325,244,0,32,325,180},
    {326,180,"None",326,640,480,0,63,640,353},
    /* Exact inclusive 4:3 +/-10% endpoints and their immediate pixel neighbours. */
    {239,200,"None",239,267,200,14,0,239,200},
    {240,200,"None",240,240,200,0,0,240,200},
    {241,200,"None",241,241,200,0,0,241,200},
    {219,150,"None",219,219,150,0,0,219,150},
    {220,150,"None",220,220,150,0,0,220,150},
    {221,150,"None",221,221,166,0,8,221,150},
    /* Aspect forcing precedes both the dimension and tolerance decisions. */
    {360,240,"4:3",320,320,240,0,0,320,240},
    {320,180,"4:3",240,240,180,0,0,240,180},
    {240,180,"16:9",320,320,240,0,30,320,180},
    {320,240,"None",320,320,240,0,0,320,240}
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
    if(cases[i].width_fit==(int)engine->output_width &&
       cases[i].height_fit==(int)engine->output_height){
      /* Native padding is a copy, not a resample; a solid colour alone cannot prove that. */
      for(unsigned y=0;y<engine->output_height;y++) for(unsigned x=0;x<engine->output_width;x++)
        engine->screen[(size_t)y*engine->output_width+x]=0xFF000000u|((x*73u+y*251u)&0xFFFFFFu);
      REQUIRE(resolve_host_frame(engine,&pixels,&width,&height),"compose patterned source");
      for(unsigned y=0;y<engine->output_height;y++) for(unsigned x=0;x<engine->output_width;x++)
        REQUIRE((pixels[(size_t)(y+cases[i].y)*width+x+cases[i].x]&0xFFFFFFu)==
                (engine->screen[(size_t)y*engine->output_width+x]&0xFFFFFFu),"native pixels unchanged");
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

static int aspect_tolerance_cases(void){
  static const struct {
    const char *aspect; unsigned height,lower,upper,forced;
  } cases[]={
    {"4:3",150,180,220,200}, {"16:9",90,144,176,160},
    {"16:10",100,144,176,160}, {"21:9",90,189,231,208}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++) for(int edge=0;edge<2;edge++)
    for(int offset=-1;offset<=1;offset++){
      AnygmSyntheticContent fixture={0};
      unsigned width=(unsigned)((int)(edge?cases[i].upper:cases[i].lower)+offset);
      int outside=edge?offset>0:offset<0;
      int ok=1;
      crt_on=0; logical_on=1; changed=0; negotiation=0; aspect=cases[i].aspect;
      REQUIRE(anygm_synthetic_sized_content_create(&fixture,width,cases[i].height),"aspect fixture");
      retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
      struct retro_game_info info={fixture.path,NULL,0,NULL};
      REQUIRE(retro_load_game(&info),"aspect load");
      retro_run();
      REQUIRE(video_width==(outside?cases[i].forced:width) && video_height==cases[i].height,
              "inclusive relative aspect tolerance");
done:
      retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
      if(!ok) return 0;
    }
  return 1;
}

static int crop_padding_case(void){
  AnygmSyntheticContent fixture={0};
  int ok=1;
  crt_on=logical_on=1; changed=0; negotiation=0; aspect="None";
  REQUIRE(anygm_synthetic_sized_content_create(&fixture,400,240),"crop fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"crop load");
  retro_run();
  AnygmEngine *engine=g_libretro.engine;
  for(unsigned y=0;y<240;y++) for(unsigned x=0;x<400;x++)
    engine->screen[(size_t)y*400+x]=0xFF000000u|((x*73u+y*251u)&0xFFFFFFu);
  static const struct { int horizontal,vertical,x,y; unsigned width,height; } cases[]={
    {40,0,0,0,320,240}, {40,30,0,30,320,180}, {80,0,40,0,240,240}, {40,30,0,30,320,180}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    engine->present_crop_left=engine->present_crop_right=cases[i].horizontal;
    engine->present_crop_top=engine->present_crop_bottom=cases[i].vertical;
    compute_present(engine);
    REQUIRE(engine->output_width==400 && engine->output_height==240,"crop preserves authored raster");
    const uint32_t *pixels=NULL; unsigned width=0,height=0;
    REQUIRE(resolve_host_frame(engine,&pixels,&width,&height),"crop frame");
    REQUIRE(width==320 && height==240,"crop precedes CRT size and aspect checks");
    for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++){
      int sx=(int)x-cases[i].x,sy=(int)y-cases[i].y;
      uint32_t expected=0;
      if(sx>=0 && sy>=0 && sx<(int)cases[i].width && sy<(int)cases[i].height)
        expected=engine->screen[(size_t)(sy+cases[i].vertical)*400+sx+cases[i].horizontal]&0xFFFFFFu;
      REQUIRE((pixels[(size_t)y*width+x]&0xFFFFFFu)==expected,
              "crop pixels and fresh bars after same-extent layout change");
    }
  }
done:
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int live_state_case(int response,unsigned source_width,unsigned source_height,
                           unsigned adjusted_width,unsigned adjusted_height){
  AnygmSyntheticContent fixture={0};
  uint8_t *ring=NULL,*native=NULL,*adjusted=NULL;
  size_t initial=0,capacity=0,state_size=0,written=0;
  int ok=1;
  crt_on=0; logical_on=1; changed=0; negotiation=response; aspect="None";
  video_width=video_height=geometry_width=geometry_height=0;
  REQUIRE(anygm_synthetic_sized_content_create(&fixture,source_width,source_height),"state fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"state load");
  initial=capacity=retro_serialize_size(); ring=malloc(capacity);
  REQUIRE(ring && initial && g_libretro.startup_ring_compact,"allocate compact ring before first frame");
  REQUIRE(snapshot(&ring,&capacity,initial),"cold snapshot");
  retro_run();
  REQUIRE(video_width==source_width && video_height==source_height &&
          snapshot(&ring,&capacity,initial),"first snapshot");
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
  REQUIRE(video_width==adjusted_width && video_height==adjusted_height &&
          geometry_width==adjusted_width && geometry_height==adjusted_height,
          "live CRT geometry notification");
  REQUIRE(snapshot(&ring,&capacity,initial),"CRT snapshot");
  REQUIRE(retro_unserialize(native,state_size),"restore native state while CRT enabled");
  retro_run();
  REQUIRE(video_width==adjusted_width && video_height==adjusted_height,"restored frame uses current CRT policy");
  logical_on=0; changed=1; retro_run();
  REQUIRE(video_width==1280 && video_height==960 && !g_libretro.engine->host_crt_active,
          "CRT ignored with logical raster off");
  REQUIRE(snapshot(&ring,&capacity,initial),"monitor snapshot");
  logical_on=1; changed=1; retro_run();
  REQUIRE(video_width==adjusted_width && video_height==adjusted_height,"remembered CRT selection");
  retro_reset();
  REQUIRE(snapshot(&ring,&capacity,initial),"cold Reset snapshot in original ring");
  retro_run();
  REQUIRE(video_width==adjusted_width && video_height==adjusted_height &&
          snapshot(&ring,&capacity,initial),"Reset frame snapshot");
  REQUIRE(retro_unserialize(ring,capacity),"same-execution rewind restore");
  retro_run();
  REQUIRE(video_width==adjusted_width && video_height==adjusted_height,"rewind presentation");
  crt_on=0; changed=1; retro_run();
  REQUIRE(video_width==source_width && video_height==source_height &&
          geometry_width==source_width && geometry_height==source_height,
          "live native geometry restored");
  REQUIRE(snapshot(&ring,&capacity,initial),"native snapshot after CRT");
  printf("CRT state transport: source=%ux%u negotiation=%d cold=%zu final=%zu\n",
         source_width,source_height,response,initial,capacity);
done:
  free(ring); free(native); free(adjusted);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  if(!geometry_cases()) return 1;
  if(!aspect_tolerance_cases()) return 1;
  if(!crop_padding_case()) return 1;
  for(int response=-1;response<=1;response++){
    if(!live_state_case(response,1920,1080,640,480) ||
       !live_state_case(response,320,180,320,240) ||
       !live_state_case(response,240,240,320,240) ||
       !live_state_case(response,360,180,640,480)) return 1;
  }
  puts("CRT output: thresholds, aspect, margins, live options, state and Reset passed");
  return 0;
}
