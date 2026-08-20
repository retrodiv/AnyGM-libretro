/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* An override that a configuration switch turns on has to turn off again.
 *
 * A freeze is a write repeated every frame. Stopping the writes stops forcing a value but does not
 * give the previous one back, and the values this scope exists to override are ones the content
 * computes once during startup and never revisits — so a scope that merely stopped applying would
 * leave the forced value in place and the player would see a switch that only works in one
 * direction. Two rules follow, and both are measured here.
 *
 * The first is the round trip: while the scope is closed the target holds exactly what it held
 * before the scope ever opened, whether the target is a variable on an instance or an element of a
 * legacy view array.
 *
 * The second is that a capture of a room-owned global belongs to the room it was read in. Entering
 * a room rewrites the view arrays from that room's own record, so a reading taken in one room
 * describes no other, and restoring it after a room change would hand back a number that never
 * belonged to the room being played. The fixture's two rooms carry deliberately different ports so
 * a capture held across the change is visible as the wrong one rather than as no change at all.
 */
#include "anygm.h"
#include "engine_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int run_frame_extent(AnygmEngine *engine,unsigned want_width,unsigned want_height,
                            const char *stage){
  AnygmInputFrame input={0};
  AnygmFrameOutput output={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  if(anygm_run_frame(engine,&input,&output)!=ANYGM_OK){
    fprintf(stderr,"scoped overrides: %s: frame failed\n",stage);
    return 0;
  }
  if(output.width!=want_width || output.height!=want_height){
    fprintf(stderr,"scoped overrides: %s: extent is %ux%u, expected %ux%u\n",
            stage,output.width,output.height,want_width,want_height);
    return 0;
  }
  return 1;
}

static int save_state(AnygmEngine *engine,uint8_t **data,size_t *written){
  size_t capacity=anygm_state_size(engine);
  *data=malloc(capacity?capacity:1);
  if(!*data) return 0;
  if(anygm_state_save(engine,*data,capacity,written)!=ANYGM_OK){
    free(*data);
    *data=NULL;
    return 0;
  }
  return 1;
}

static int select_logical_raster(AnygmEngine *engine,int on){
  AnygmConfigDelta delta={0};
  delta.struct_size=sizeof delta;
  delta.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER;
  delta.values.struct_size=sizeof delta.values;
  delta.values.present_logical_raster=on?1u:0u;
  return anygm_set_config(engine,&delta)==ANYGM_OK && run_frames(engine,1);
}

static int check_scale(AnygmEngine *engine,const char *stage,double want){
  double value=-1;
  if(!gml_inst_var_first_real(&engine->vm,"obj_canvas","scale",&value)){
    fprintf(stderr,"scoped overrides: %s: obj_canvas.scale is unreadable\n",stage);
    return 0;
  }
  if(value!=want){
    fprintf(stderr,"scoped overrides: %s: obj_canvas.scale is %g, expected %g\n",stage,value,want);
    return 0;
  }
  return 1;
}

static int check_port(AnygmEngine *engine,const char *stage,double want){
  double value=gml_global_arr(&engine->vm,"view_wport",0);
  if(value!=want){
    fprintf(stderr,"scoped overrides: %s: view_wport[0] is %g, expected %g\n",stage,value,want);
    return 0;
  }
  return 1;
}

static int set_outer_geometry(AnygmEngine *engine,int width,int height){
  engine->vm.window_w=width;
  engine->vm.window_h=height;
  return gml_render_application_surface_ensure_owned(&engine->render,width,height);
}

static int check_outer_geometry(AnygmEngine *engine,const char *stage,int want_width,
                                int want_height){
  GmlRenderPresentationMetrics presentation={0};
  if(!gml_render_presentation_metrics(&engine->render,&presentation)){
    fprintf(stderr,"scoped overrides: %s: presentation metrics are unavailable\n",stage);
    return 0;
  }
  if(engine->vm.window_w!=want_width || engine->vm.window_h!=want_height ||
     presentation.application_width!=want_width ||
     presentation.application_height!=want_height){
    fprintf(stderr,
            "scoped overrides: %s: window=%dx%d application=%dx%d, expected %dx%d\n",
            stage,engine->vm.window_w,engine->vm.window_h,
            presentation.application_width,presentation.application_height,
            want_width,want_height);
    return 0;
  }
  return 1;
}

static int declare_scoped_overrides(AnygmEngine *engine){
  static const char *const directives[]={
    "?gameres obj_canvas:scale=1",
    "?gameres view_wport[0]=64",
    "?gameres view_hport[0]=48",
    "?gameres @window_w=64",
    "?gameres @window_h=48",
    "?gameres @application_w=64",
    "?gameres @application_h=48"
  };
  for(unsigned i=0;i<sizeof directives/sizeof directives[0];i++)
    if(anygm_set_runtime_override(engine,i,1,directives[i])!=ANYGM_OK) return 0;
  return 1;
}

static AnygmEngine *create_loaded_engine(const AnygmHostServices *services,
                                         const AnygmContentSource *source){
  AnygmEngine *engine=NULL;
  if(anygm_create(services,&engine)!=ANYGM_OK) return NULL;
  AnygmConfigDelta delta={0};
  delta.struct_size=sizeof delta;
  delta.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER;
  delta.values.struct_size=sizeof delta.values;
  delta.values.present_logical_raster=0;
  if(anygm_set_config(engine,&delta)!=ANYGM_OK ||
     anygm_load(engine,source,NULL)!=ANYGM_OK || !run_frames(engine,1)){
    anygm_destroy(engine);
    return NULL;
  }
  return engine;
}

static int scoped_override_case(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_scoped_override_content_create(&fixture)){
    fputs("scoped overrides: fixture creation failed\n",stderr);
    return 0;
  }
  int ok=0;
  AnygmEngine *engine=NULL,*state_engine=NULL;
  uint8_t *state_off=NULL,*state_on=NULL;
  size_t state_off_size=0,state_on_size=0;
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  engine=create_loaded_engine(&services,&source);
  if(!engine){
    fputs("scoped overrides: engine load failed\n",stderr);
    goto done;
  }
  /* What the content itself produced, before any override exists. */
  if(!check_scale(engine,"authored",3) || !check_port(engine,"authored",128)) goto done;
  if(!set_outer_geometry(engine,320,240)) goto done;

  if(!declare_scoped_overrides(engine)){
    fputs("scoped overrides: declaring the directives failed\n",stderr);
    goto done;
  }
  /* A declared directive with the scope closed is not a directive that applies. */
  if(!select_logical_raster(engine,0)) goto done;
  if(!check_scale(engine,"declared, scope closed",3) ||
     !check_port(engine,"declared, scope closed",128) ||
     !check_outer_geometry(engine,"declared, scope closed",320,240)) goto done;

  if(!save_state(engine,&state_off,&state_off_size)) goto done;

  if(!select_logical_raster(engine,1)) goto done;
  if(!check_scale(engine,"scope open",1) || !check_port(engine,"scope open",64) ||
     !check_outer_geometry(engine,"scope open",64,48) ||
     !run_frame_extent(engine,64,48,"scope open")) goto done;
  if(!save_state(engine,&state_on,&state_on_size)) goto done;

  if(!select_logical_raster(engine,0)) goto done;
  if(!check_scale(engine,"scope reclosed",3) || !check_port(engine,"scope reclosed",128) ||
     !check_outer_geometry(engine,"scope reclosed",320,240)) goto done;

  /* The live host owns presentation policy across a state load. A fresh engine at a different
   * outer size must not import the completed frame or cached window geometry of the state whose
   * scope was open, and the reverse load must immediately compose at the live logical raster. */
  state_engine=create_loaded_engine(&services,&source);
  if(!state_engine || !set_outer_geometry(state_engine,400,300) ||
     !declare_scoped_overrides(state_engine)){
    fputs("scoped overrides: state engine setup failed\n",stderr);
    goto done;
  }
  if(anygm_state_load(state_engine,state_on,state_on_size)!=ANYGM_OK ||
     !run_frame_extent(state_engine,400,300,"open state loaded with scope closed") ||
     !check_outer_geometry(state_engine,"open state loaded with scope closed",400,300) ||
     !select_logical_raster(state_engine,1) ||
     !run_frame_extent(state_engine,64,48,"scope reopened after state load") ||
     anygm_state_load(state_engine,state_off,state_off_size)!=ANYGM_OK ||
     !run_frame_extent(state_engine,64,48,"closed state loaded with scope open") ||
     !check_outer_geometry(state_engine,"closed state loaded with scope open",64,48) ||
     !select_logical_raster(state_engine,0) ||
     !check_outer_geometry(state_engine,"scope closed after reverse state load",400,300)){
    goto done;
  }
  anygm_destroy(state_engine);
  state_engine=NULL;

  /* Re-open, then change room while it is open: the capture the next close hands back has to be
   * the incoming room's port, not the one the first room happened to carry. */
  if(!select_logical_raster(engine,1)) goto done;
  if(!check_port(engine,"scope reopened",64)) goto done;
  gml_set_global_scalar(&engine->vm,"advance",1);
  if(!run_frames(engine,3)) goto done;
  if(engine->vm.room_index!=1){
    fprintf(stderr,"scoped overrides: room did not advance (index %d)\n",engine->vm.room_index);
    goto done;
  }
  if(!check_port(engine,"second room, scope open",64)) goto done;
  if(!select_logical_raster(engine,0)) goto done;
  if(!check_port(engine,"second room, scope closed",192)) goto done;
  ok=1;
done:
  if(state_engine) anygm_destroy(state_engine);
  if(engine) anygm_destroy(engine);
  free(state_off);
  free(state_on);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  if(!scoped_override_case()) return 1;
  puts("scoped overrides: ok");
  return 0;
}
