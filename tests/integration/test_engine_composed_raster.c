/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The paired synthetic fixtures distinguish two presentation policies. An explicitly
 * composed frame retains its authored raster and is fitted uniformly into the virtual
 * monitor. An otherwise identical ordinary frame rasterizes at the monitor extent. Both
 * directions are asserted so the exception cannot silently become the general rule. */
#include "anygm.h"
#include "engine_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MONITOR_W 1920
#define MONITOR_H 1080
/* Both fixtures use a 64x48 view and an authored 128x96 port. */
#define COMPOSED_W 128
#define COMPOSED_H 96
/* Exercise every synthetic forced-aspect mode carried by the configuration. */
static const uint32_t k_aspect_modes[]={1u,2u,3u,4u};
static const char *const k_aspect_names[]={"4:3","16:9","16:10","21:9"};

static int run_frames(AnygmEngine *engine,int count){
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  for(int frame=0;frame<count;frame++){
    AnygmFrameOutput output={0};
    output.struct_size=sizeof output;
    if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK) return 0;
  }
  return 1;
}

static int select_virtual_monitor(AnygmEngine *engine){
  AnygmConfigDelta delta={0};
  delta.struct_size=sizeof delta;
  delta.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER|ANYGM_CONFIG_MONITOR_WIDTH|
               ANYGM_CONFIG_MONITOR_HEIGHT;
  delta.values.struct_size=sizeof delta.values;
  delta.values.present_logical_raster=0;
  delta.values.monitor_width=MONITOR_W;
  delta.values.monitor_height=MONITOR_H;
  return anygm_set_config(engine,&delta)==ANYGM_OK;
}

static int force_aspect(AnygmEngine *engine,uint32_t mode){
  AnygmConfigDelta delta={0};
  delta.struct_size=sizeof delta;
  delta.fields=ANYGM_CONFIG_ASPECT_MODE;
  delta.values.struct_size=sizeof delta.values;
  delta.values.aspect_mode=mode;
  return anygm_set_config(engine,&delta)==ANYGM_OK;
}

static AnygmEngine *load(const AnygmSyntheticContent *fixture,AnygmHostServices *services){
  AnygmEngine *engine=NULL;
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=(char *)fixture->path;
  if(anygm_create(services,&engine)!=ANYGM_OK) return NULL;
  if(anygm_load(engine,&source,NULL)!=ANYGM_OK){ anygm_destroy(engine); return NULL; }
  return engine;
}

/* The composed raster reaches the monitor by being fitted into it: centred, keeping its own shape,
 * and as large as that allows. Stated as those three properties rather than by recomputing the
 * engine's own arithmetic, so the guard measures the behaviour instead of restating the code. */
static int check_uniform_fit(AnygmEngine *engine){
  int w=engine->host_canvas_width, h=engine->host_canvas_height;
  int x=engine->host_canvas_x, y=engine->host_canvas_y;
  if(w<=0 || h<=0 || w>MONITOR_W || h>MONITOR_H){
    fprintf(stderr,"composed raster: canvas %dx%d does not fit the monitor %dx%d\n",
            w,h,MONITOR_W,MONITOR_H);
    return 0;
  }
  /* A fitted synthetic frame is centred rather than left at the origin. */
  if(x!=(MONITOR_W-w)/2 || y!=(MONITOR_H-h)/2){
    fprintf(stderr,"composed raster: canvas at (%d,%d), expected centred at (%d,%d)\n",
            x,y,(MONITOR_W-w)/2,(MONITOR_H-h)/2);
    return 0;
  }
  /* Its own shape, within the rounding a whole pixel allows. */
  if(labs((long)w*COMPOSED_H-(long)h*COMPOSED_W) > (long)COMPOSED_W+COMPOSED_H){
    fprintf(stderr,"composed raster: canvas %dx%d does not carry the composed shape %dx%d\n",
            w,h,COMPOSED_W,COMPOSED_H);
    return 0;
  }
  /* As large as that shape allows: one axis has to reach the monitor's edge, or the picture is
   * needlessly small inside it. */
  if(w!=MONITOR_W && h!=MONITOR_H){
    fprintf(stderr,"composed raster: canvas %dx%d touches neither edge of %dx%d\n",
            w,h,MONITOR_W,MONITOR_H);
    return 0;
  }
  return 0==0;
}
static int compositing_case(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_classic_compositor_content_create(&fixture)){
    fputs("composed raster: compositing fixture creation failed\n",stderr);
    return 0;
  }
  int ok=0;
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=load(&fixture,&services);
  if(!engine){ fputs("composed raster: engine load failed\n",stderr); goto done; }
  if(!select_virtual_monitor(engine) || !run_frames(engine,8)) goto done;
  if(!engine->classic_compositor){
    fputs("composed raster: the fixture did not register as composing its own frame; the rest of "
          "this case would pass for the wrong reason\n",stderr);
    goto done;
  }
  /* Explicit composition retains the fixture's authored raster. */
  if(engine->output_width!=COMPOSED_W || engine->output_height!=COMPOSED_H){
    fprintf(stderr,"composed raster: rasterized at %ux%u, expected the composed %dx%d\n",
            engine->output_width,engine->output_height,COMPOSED_W,COMPOSED_H);
    goto done;
  }
  if(engine->host_output_width!=MONITOR_W || engine->host_output_height!=MONITOR_H){
    fprintf(stderr,"composed raster: delivered %ux%u, expected the monitor %dx%d\n",
            engine->host_output_width,engine->host_output_height,MONITOR_W,MONITOR_H);
    goto done;
  }
  if(!check_uniform_fit(engine)) goto done;
  ok=1;
done:
  if(engine) anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int plain_case(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_classic_plain_content_create(&fixture)){
    fputs("composed raster: plain fixture creation failed\n",stderr);
    return 0;
  }
  int ok=0;
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=load(&fixture,&services);
  if(!engine){ fputs("composed raster: engine load failed\n",stderr); goto done; }
  if(!select_virtual_monitor(engine) || !run_frames(engine,8)) goto done;
  if(engine->classic_compositor){
    fputs("composed raster: the plain fixture registered as composing its own frame\n",stderr);
    goto done;
  }
  /* The ordinary fixture rasterizes at the configured monitor extent. */
  if(engine->output_width!=MONITOR_W || engine->output_height!=MONITOR_H){
    fprintf(stderr,"composed raster: plain content rasterized at %ux%u, expected the monitor "
                   "%dx%d\n",engine->output_width,engine->output_height,MONITOR_W,MONITOR_H);
    goto done;
  }
  ok=1;
done:
  if(engine) anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

/* Every forced-aspect mode leaves explicit composition at its authored raster and reports
 * forcing inactive. */
static int forced_aspect_case(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_classic_compositor_content_create(&fixture)){
    fputs("composed raster: compositing fixture creation failed\n",stderr);
    return 0;
  }
  int ok=0;
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *engine=load(&fixture,&services);
  if(!engine){ fputs("composed raster: engine load failed\n",stderr); goto done; }
  if(!run_frames(engine,4)) goto done;
  if(!engine->classic_compositor){
    fputs("composed raster: the fixture did not register as composing its own frame\n",stderr);
    goto done;
  }
  for(size_t i=0;i<sizeof k_aspect_modes/sizeof *k_aspect_modes;i++){
    if(!force_aspect(engine,k_aspect_modes[i]) || !run_frames(engine,4)) goto done;
    if(engine->aspect_force_active){
      fprintf(stderr,"composed raster: %s is active over content that composes its own frame; "
                     "there is no camera for it to widen\n",k_aspect_names[i]);
      goto done;
    }
    if(engine->output_width!=COMPOSED_W || engine->output_height!=COMPOSED_H){
      fprintf(stderr,"composed raster: under %s the raster is %ux%u, expected the composed "
                     "%dx%d\n",k_aspect_names[i],engine->output_width,engine->output_height,
              COMPOSED_W,COMPOSED_H);
      goto done;
    }
  }
  ok=1;
done:
  if(engine) anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  if(!compositing_case()) return 1;
  if(!plain_case()) return 1;
  if(!forced_aspect_case()) return 1;
  puts("composed raster: ok");
  return 0;
}
