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

static int crt_on=1,logical_on=1,changed,negotiation,vertical,small,keep_axis;
static const char *rotation;
static const char *aspect="None";
static unsigned video_width,video_height,geometry_width,geometry_height;
static float geometry_aspect;
static bool frontend_rotation_supported;
static unsigned frontend_rotation,frontend_rotation_requests;

static bool environment(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_SET_ROTATION){
    frontend_rotation=*(const unsigned*)data;
    frontend_rotation_requests++;
    return frontend_rotation_supported;
  }
  if(command==RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS){
    if(negotiation>0) *(uint64_t*)data|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
    return negotiation>=0;
  }
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
  if(command==RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO || command==RETRO_ENVIRONMENT_SET_GEOMETRY){
    const struct retro_game_geometry *g=command==RETRO_ENVIRONMENT_SET_GEOMETRY
      ?data:&((const struct retro_system_av_info*)data)->geometry;
    geometry_width=g->base_width; geometry_height=g->base_height; geometry_aspect=g->aspect_ratio;
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
    if(!strcmp(v->key,"anygm_crt_oversize_handling"))
      v->value=keep_axis?(small?"W:Keep H:240":"W:Keep H:480"):(small?"320x240":NULL);
    if(!strcmp(v->key,"anygm_os_screen_orientation")) v->value=vertical?"Vertical":NULL;
    if(!strcmp(v->key,"anygm_image_rotation")) v->value=rotation;
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

static int rotation_cases(void){
  static const char *const turns[]={NULL,"Left","Upside Down","Right"};
  AnygmSyntheticContent fixture={0};
  int ok=1;
  crt_on=0; logical_on=1; changed=0; negotiation=0; vertical=small=1; aspect="None";
  REQUIRE(anygm_synthetic_sized_content_create(&fixture,400,300),"rotation fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"rotation load");
  for(int crt=0;crt<2;crt++) for(vertical=0;vertical<2;vertical++)
    for(small=0;small<2;small++) for(unsigned turn=0;turn<4;turn++){
      unsigned w=400,h=300;
      bool portrait=vertical!=((turn&1u)!=0);
      crt_on=crt; rotation=turns[turn]; changed=1;
      retro_run();
      AnygmEngine *engine=g_libretro.engine;
      if(crt){
        w=small?320:640;
        h=small?240:480;
        if(portrait){ unsigned swap=w; w=h; h=swap; }
      }
      unsigned delivered_w=turn&1u?h:w,delivered_h=turn&1u?w:h;
      REQUIRE(video_width==delivered_w && video_height==delivered_h,
              "live CRT orientation and rotation matrix");
      struct retro_system_av_info av={0};
      retro_get_system_av_info(&av);
      REQUIRE(av.geometry.base_width==delivered_w && av.geometry.base_height==delivered_h &&
              fabs(av.geometry.aspect_ratio-(double)delivered_w/delivered_h)<1e-6,
              "rotated frontend geometry and aspect");
      REQUIRE(engine->output_width==400 && engine->output_height==300,
              "logical image dimensions remain authored");
      if(crt && portrait){
        for(size_t i=0;i<400u*300u;i++) engine->screen[i]=0xffa04020u;
        const uint32_t *pixels=NULL;
        unsigned width=0,height=0;
        REQUIRE(resolve_host_frame(engine,&pixels,&width,&height) &&
                (pixels[0]&0xffffffu)==0 &&
                (pixels[(size_t)(height/2)*width+width/2]&0xffffffu)==0xa04020u,
                "vertical canvas has centered black bars after rotation");
      }
      if(!crt || (!portrait && small)){
        for(unsigned y=0;y<300;y++) for(unsigned x=0;x<400;x++)
          engine->screen[(size_t)y*400+x]=0xff000000u|((x&1u)?255u:0u)|(y<150?0x4000u:0x800000u);
        const uint32_t *pixels=NULL;
        unsigned width=0,height=0;
        REQUIRE(resolve_host_frame(engine,&pixels,&width,&height) &&
                width==delivered_w && height==delivered_h,"resolve asymmetric rotated image");
        /* Check both ends of the fractional footprint in X and a different color at each Y end. */
        for(unsigned y=0;y<2;y++) for(unsigned x=0;x<2;x++){
          unsigned sx=x,sy=y?h-1u:0u,dx=sx,dy=sy;
          if(turn==1){ dx=sy; dy=w-1u-sx; }
          if(turn==2){ dx=w-1u-sx; dy=h-1u-sy; }
          if(turn==3){ dx=h-1u-sy; dy=sx; }
          uint32_t expected=(y?0x800000u:0x4000u)|(crt?(x?153u:51u):(x?255u:0u));
          REQUIRE((pixels[(size_t)dy*width+dx]&0xffffffu)==expected,
                  "rotation direction and fractional source coverage");
        }
        if(!crt){
          engine->input.pointer_x=turn==1?30:turn==2?379:turn==3?269:20;
          engine->input.pointer_y=turn==1?379:turn==2?269:turn==3?20:30;
          engine_input_poll_mouse(engine);
          REQUIRE(engine->mouse_pixel_x==20 && engine->mouse_pixel_y==30,
                  "pointer follows the displayed image");
        }
      }
    }
  crt_on=logical_on=0; vertical=small=1; rotation="Right"; changed=1;
  retro_run();
  REQUIRE(video_width==960 && video_height==1280 && !g_libretro.engine->host_crt_active,
          "CRT off rotates the configured monitor raster");
done:
  vertical=small=0; rotation=NULL;
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int frontend_portrait_case(bool supported){
  AnygmSyntheticContent fixture={0};
  int ok=1;
  const unsigned native_width=228,native_height=304;
  unsigned packed_width=supported?native_height:native_width;
  unsigned packed_height=supported?native_width:native_height;
  crt_on=logical_on=vertical=1; small=keep_axis=0; rotation=NULL; aspect="None";
  frontend_rotation_supported=supported;
  frontend_rotation=frontend_rotation_requests=0;
  REQUIRE(anygm_synthetic_sized_content_create(&fixture,native_width,native_height),"portrait fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"portrait load");
  struct retro_system_av_info av={0};
  retro_get_system_av_info(&av);
  REQUIRE(frontend_rotation==(supported?1u:0u) && frontend_rotation_requests==(supported?1u:2u),
          "request the paired turn before publishing geometry and clear a refused request");
  REQUIRE(av.geometry.base_width==packed_width && av.geometry.base_height==packed_height &&
          fabs(av.geometry.aspect_ratio-(double)native_width/native_height)<1e-6,
          "portrait startup geometry describes the buffer extent and the display aspect");
  size_t capacity=retro_serialize_size();
  retro_run();
  REQUIRE(video_width==packed_width && video_height==packed_height,"portrait delivery dimensions");
  AnygmEngine *engine=g_libretro.engine;
  REQUIRE(engine->output_width==native_width && engine->output_height==native_height &&
          !engine->host_canvas_active,"portrait retains the native image without a fit");
  for(unsigned y=0;y<native_height;y++) for(unsigned x=0;x<native_width;x++)
    engine->screen[(size_t)y*native_width+x]=0xff000000u|(y*native_width+x);
  const uint32_t *pixels=NULL;
  unsigned width=0,height=0;
  REQUIRE(resolve_host_frame(engine,&pixels,&width,&height) &&
          width==packed_width && height==packed_height,"packed portrait pixels");
  for(unsigned y=0;y<native_height;y++) for(unsigned x=0;x<native_width;x++){
    size_t placed=supported?(size_t)x*width+(native_height-1u-y):(size_t)y*width+x;
    REQUIRE(pixels[placed]==engine->screen[(size_t)y*native_width+x],
            "frontend turn cancels the transport turn exactly");
  }
  engine->input.pointer_x=20; engine->input.pointer_y=30;
  engine_input_poll_mouse(engine);
  REQUIRE(engine->mouse_pixel_x==20 && engine->mouse_pixel_y==30,"portrait pointer uses visible axes");
  rotation="Right"; changed=1; retro_run();
  REQUIRE(frontend_rotation==(supported?3u:0u) && video_width==(supported?640u:480u) &&
          video_height==(supported?480u:640u),
          "a quarter turn that cancels the system turn uses the horizontal CRT fit");
  REQUIRE(fabs(geometry_aspect-3.0/4.0)<1e-6,
          "the frontend receives the display aspect after its requested turn");
  int cancelled_fit_width=engine->host_canvas_width,cancelled_fit_height=engine->host_canvas_height;
  rotation=NULL; changed=1; retro_run();
  REQUIRE(frontend_rotation==(supported?1u:0u) && video_width==packed_width && video_height==packed_height,
          "returning to no rotation restores portrait transport");
  vertical=0; changed=1; retro_run();
  REQUIRE(frontend_rotation==0 && video_width==640 && video_height==480,
          "horizontal CRT restores the ordinary oversize path");
  REQUIRE(engine->host_canvas_width==cancelled_fit_width &&
          engine->host_canvas_height==cancelled_fit_height,
          "the cancelled system turn and horizontal CRT fit the same content rectangle");
  unsigned requests=frontend_rotation_requests;
  for(unsigned turn=1;turn<=3;turn+=2){
    rotation=turn==1?"Left":"Right"; changed=1; retro_run();
    REQUIRE(frontend_rotation_requests==requests && video_width==native_height &&
            video_height==native_width && geometry_width==video_width &&
            geometry_height==video_height && !engine->host_canvas_active,
            "a live image quarter turn on horizontal OS settings retains the native portrait raster");
    for(unsigned y=0;y<native_height;y++) for(unsigned x=0;x<native_width;x++)
      engine->screen[(size_t)y*native_width+x]=0xff000000u|(y*native_width+x);
    REQUIRE(resolve_host_frame(engine,&pixels,&width,&height),"resolve the manually turned raster");
    for(unsigned y=0;y<native_height;y++) for(unsigned x=0;x<native_width;x++){
      size_t placed=turn==1?(size_t)(native_width-1u-x)*width+y:
                            (size_t)x*width+(native_height-1u-y);
      REQUIRE(pixels[placed]==engine->screen[(size_t)y*native_width+x],
              "image quarter turns preserve each native sample");
    }
  }
  rotation=NULL;
  vertical=1; retro_reset(); retro_run();
  REQUIRE(frontend_rotation==(supported?1u:0u) && video_width==packed_width && video_height==packed_height &&
          retro_serialize_size()==capacity,"Reset and frontend ring preserve the portrait policy");
done:
  retro_unload_game();
  if(frontend_rotation!=0) ok=0;
  retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  frontend_rotation_supported=false;
  vertical=0; rotation=NULL;
  return ok;
}

static int vertical_native_cases(void){
  static const struct { unsigned width,height,expected_width,expected_height; } cases[]={
    {244,320,244,320}, {245,320,240,320}, {240,365,240,320},
    {180,320,240,320}, {180,326,240,320}, {240,348,240,348}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    AnygmSyntheticContent fixture={0};
    int ok=1;
    crt_on=logical_on=vertical=small=1; rotation=NULL; aspect="None"; negotiation=0; changed=0;
    REQUIRE(anygm_synthetic_sized_content_create(&fixture,cases[i].width,cases[i].height),
            "vertical fixture");
    retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
    struct retro_game_info info={fixture.path,NULL,0,NULL};
    REQUIRE(retro_load_game(&info),"vertical load");
    retro_run();
    REQUIRE(video_width==cases[i].expected_width && video_height==cases[i].expected_height,
            "vertical threshold and native padding");
    REQUIRE(g_libretro.engine->host_crt_area_filter==(i==1 || i==2 || i==4),
            "a native-size image keeps its pixels unless post-padding overflows");
done:
    vertical=small=0;
    retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
    if(!ok) return 0;
  }
  return 1;
}

static int keep_axis_cases(void){
  static const struct {
    unsigned sw,sh;
    int vertical,small;
    const char *rotation;
    unsigned width,height,x,y,fit_w,fit_h;
    double display_aspect;
  } cases[]={
    {1920,1080,0,0,NULL,1920,480,0,60,1920,360,4.0/3.0},
    {1920,480,0,0,NULL,1920,480,0,160,1920,160,4.0/3.0},
    {1920,1080,0,1,NULL,1920,240,0,30,1920,180,4.0/3.0},
    {1920,1080,0,0,"Left",2560,480,740,0,1080,480,4.0/3.0},
    {1080,1920,1,0,NULL,480,1920,60,0,360,1920,3.0/4.0},
    {1080,1920,1,1,NULL,240,1920,30,0,180,1920,3.0/4.0},
    {1080,1920,1,1,"Right",240,2560,0,740,240,1080,3.0/4.0},
    {400,600,0,0,NULL,800,480,200,0,400,480,4.0/3.0},
    {3840,1080,1,0,NULL,480,3840,0,1380,480,1080,1.0}
  };
  for(size_t i=0;i<sizeof cases/sizeof cases[0];i++){
    AnygmSyntheticContent fixture={0};
    int ok=1;
    crt_on=logical_on=keep_axis=1; changed=0; negotiation=0; aspect="None";
    vertical=cases[i].vertical; small=cases[i].small; rotation=cases[i].rotation;
    REQUIRE(anygm_synthetic_sized_content_create(&fixture,cases[i].sw,cases[i].sh),"Keep fixture");
    retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
    struct retro_game_info info={fixture.path,NULL,0,NULL};
    REQUIRE(retro_load_game(&info),"Keep load");
    struct retro_system_av_info av={0};
    retro_get_system_av_info(&av);
    REQUIRE(av.geometry.base_width==cases[i].width && av.geometry.base_height==cases[i].height &&
            av.geometry.max_width>=cases[i].width && av.geometry.max_height>=cases[i].height &&
            fabs(av.geometry.aspect_ratio-cases[i].display_aspect)<1e-6,
            "Keep declares the display aspect independently of raster dimensions");
    retro_run();
    AnygmEngine *engine=g_libretro.engine;
    REQUIRE(engine->output_width==cases[i].sw && engine->output_height==cases[i].sh,
            "Keep retains the authored raster before presentation");
    bool portrait=vertical!=(rotation!=NULL);
    for(unsigned y=0;y<cases[i].sh;y++) for(unsigned x=0;x<cases[i].sw;x++)
      engine->screen[(size_t)y*cases[i].sw+x]=0xff003300u+((portrait?y:x)&1u)*0x550000u;
    const uint32_t *pixels=NULL;
    unsigned width=0,height=0;
    REQUIRE(resolve_host_frame(engine,&pixels,&width,&height) &&
            width==cases[i].width && height==cases[i].height &&
            video_width==width && video_height==height,"Keep delivers the fixed and preserved axes");
    for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++){
      uint32_t expected=0;
      unsigned ix=x-cases[i].x,iy=y-cases[i].y;
      if(ix<cases[i].fit_w && iy<cases[i].fit_h){
        unsigned sample=vertical?iy:ix;
        expected=0x3300u+(sample&1u)*0x550000u;
      }
      REQUIRE((pixels[(size_t)y*width+x]&0xffffffu)==expected,
              "Keep preserves every sample on its variable axis and centers black margins");
    }
done:
    vertical=small=keep_axis=0; rotation=NULL;
    retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
    if(!ok) return 0;
  }
  return 1;
}

static int keep_aspect_reset_case(void){
  AnygmSyntheticContent fixture={0};
  int ok=1;
  crt_on=0; logical_on=keep_axis=1; changed=0; negotiation=0; aspect="None";
  REQUIRE(anygm_synthetic_sized_content_create(&fixture,1920,480),"aspect-only Reset fixture");
  retro_set_environment(environment); retro_set_video_refresh(video); retro_init();
  struct retro_game_info info={fixture.path,NULL,0,NULL};
  REQUIRE(retro_load_game(&info),"aspect-only Reset load");
  struct retro_system_av_info av={0};
  retro_get_system_av_info(&av);
  REQUIRE(av.geometry.aspect_ratio==4.0f,"native widescreen aspect before Reset");
  retro_run();
  REQUIRE(video_width==1920 && video_height==480,"native raster before Reset");
  geometry_aspect=0.0f;
  crt_on=1; retro_reset(); retro_run();
  REQUIRE(video_width==1920 && video_height==480 && fabs(geometry_aspect-4.0/3.0)<1e-6,
          "Reset announces an aspect change at the same extent");
  crt_on=0; changed=1; retro_run();
  REQUIRE(video_width==1920 && video_height==480 && geometry_aspect==4.0f,
          "live CRT disable restores the aspect at the same extent");
done:
  keep_axis=0;
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
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
  if(source_width==1920 && source_height==1080){
    keep_axis=1; changed=1; retro_run();
    REQUIRE(video_width==1920 && video_height==480 && snapshot(&ring,&capacity,initial),
            "the original ring accepts a Keep frame");
    REQUIRE(retro_unserialize(native,state_size),"restore the original frame under Keep");
    retro_run();
    REQUIRE(video_width==1920 && video_height==480 &&
            fabs(geometry_aspect-4.0/3.0)<1e-6,"restored frame uses Keep and its display aspect");
    keep_axis=0; changed=1; retro_run();
  }
  REQUIRE(retro_unserialize(native,state_size),"restore native state while CRT enabled");
  retro_run();
  REQUIRE(video_width==adjusted_width && video_height==adjusted_height,"restored frame uses current CRT policy");
  logical_on=0; changed=1; retro_run();
  REQUIRE(video_width==adjusted_width && video_height==adjusted_height &&
          g_libretro.engine->host_crt_active && g_libretro.engine->config.present_logical_raster,
          "CRT forces logical raster despite the stored off selection");
  REQUIRE(snapshot(&ring,&capacity,initial),"forced logical snapshot");
  crt_on=0; changed=1; retro_run();
  REQUIRE(video_width==1280 && video_height==960 && !g_libretro.engine->host_crt_active &&
          !g_libretro.engine->config.present_logical_raster,
          "disabling CRT restores the stored monitor raster");
  REQUIRE(snapshot(&ring,&capacity,initial),"monitor snapshot");
  crt_on=1; logical_on=1; changed=1; retro_run();
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
  rotation="Right"; vertical=small=1; changed=1; retro_run();
  REQUIRE(video_width==source_height && video_height==source_width &&
          geometry_width==source_height && geometry_height==source_width &&
          snapshot(&ring,&capacity,initial),"live rotation uses the existing frontend ring");
  REQUIRE(retro_unserialize(native,state_size),"restore unrotated state under rotation");
  retro_run();
  REQUIRE(video_width==source_height && video_height==source_width,
          "restored picture uses current rotation");
  rotation="Upside Down";
  retro_reset();
  REQUIRE(snapshot(&ring,&capacity,initial),"rotated cold Reset");
  retro_run();
  REQUIRE(video_width==source_width && video_height==source_height &&
          geometry_width==source_width && geometry_height==source_height &&
          snapshot(&ring,&capacity,initial),"Reset publishes the newly selected rotation");
  printf("CRT state transport: source=%ux%u negotiation=%d cold=%zu final=%zu\n",
         source_width,source_height,response,initial,capacity);
done:
  rotation=NULL; vertical=small=keep_axis=0;
  free(ring); free(native); free(adjusted);
  retro_unload_game(); retro_deinit(); anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  if(!geometry_cases()) return 1;
  if(!aspect_tolerance_cases()) return 1;
  if(!crop_padding_case() || !rotation_cases() || !vertical_native_cases() ||
     !keep_axis_cases() || !keep_aspect_reset_case() ||
     !frontend_portrait_case(false) || !frontend_portrait_case(true)) return 1;
  for(int response=-1;response<=1;response++){
    if(!live_state_case(response,1920,1080,640,480) ||
       !live_state_case(response,320,180,320,240) ||
       !live_state_case(response,240,240,320,240) ||
       !live_state_case(response,360,180,640,480)) return 1;
  }
  puts("CRT output: thresholds, aspect, margins, live options, state and Reset passed");
  return 0;
}
