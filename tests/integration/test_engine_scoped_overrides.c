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

static int scoped_override_case(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_scoped_override_content_create(&fixture)){
    fputs("scoped overrides: fixture creation failed\n",stderr);
    return 0;
  }
  int ok=0;
  AnygmEngine *engine=NULL;
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  if(anygm_create(&services,&engine)!=ANYGM_OK ||
     anygm_load(engine,&source,NULL)!=ANYGM_OK){
    fputs("scoped overrides: engine load failed\n",stderr);
    goto done;
  }
  if(!run_frames(engine,1)) goto done;
  /* What the content itself produced, before any override exists. */
  if(!check_scale(engine,"authored",3) || !check_port(engine,"authored",128)) goto done;

  if(anygm_set_runtime_override(engine,0,1,"?gameres obj_canvas:scale=1")!=ANYGM_OK ||
     anygm_set_runtime_override(engine,1,1,"?gameres view_wport[0]=64")!=ANYGM_OK){
    fputs("scoped overrides: declaring the directives failed\n",stderr);
    goto done;
  }
  /* A declared directive with the scope closed is not a directive that applies. */
  if(!select_logical_raster(engine,0)) goto done;
  if(!check_scale(engine,"declared, scope closed",3) ||
     !check_port(engine,"declared, scope closed",128)) goto done;

  if(!select_logical_raster(engine,1)) goto done;
  if(!check_scale(engine,"scope open",1) || !check_port(engine,"scope open",64)) goto done;

  if(!select_logical_raster(engine,0)) goto done;
  if(!check_scale(engine,"scope reclosed",3) || !check_port(engine,"scope reclosed",128)) goto done;

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
  if(engine) anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

int main(void){
  if(!scoped_override_case()) return 1;
  puts("scoped overrides: ok");
  return 0;
}
