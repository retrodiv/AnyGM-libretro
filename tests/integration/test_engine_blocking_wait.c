/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* A synthetic event loops on one wait call site until a global changes between frames. The guard checks frozen simulation, continuation and same-run state restoration using authored synthetic content only. */
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

static int save_state(AnygmEngine *engine,uint8_t **data,size_t *written){
  size_t capacity=anygm_state_size(engine);
  *data=malloc(capacity?capacity:1);
  if(!*data) return 0;
  return anygm_state_save(engine,*data,capacity,written)==ANYGM_OK;
}

static int blocking_wait_case(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_blocking_wait_content_create(&fixture)){
    fputs("blocking wait: fixture creation failed\n",stderr);
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

  int failed=0;
  uint8_t *state=NULL;
  size_t state_size=0;
  if(anygm_create(&services,&engine)!=ANYGM_OK ||
     anygm_load(engine,&source,NULL)!=ANYGM_OK){
    fputs("blocking wait: engine load failed\n",stderr);
    if(engine) anygm_destroy(engine);
    anygm_synthetic_content_destroy(&fixture);
    return 0;
  }

  /* Enough frames for the alarm to fire and the wait to begin. */
  if(!run_frames(engine,4)){
    fputs("blocking wait: opening frames failed\n",stderr);
    failed=1;
    goto done;
  }
  double held_steps=gml_global_num(&engine->vm,"fixture_steps");
  if(held_steps<1.0){
    fprintf(stderr,"blocking wait: the fixture never stepped (%g)\n",held_steps);
    failed=1;
    goto done;
  }
  if(gml_global_num(&engine->vm,"fixture_resumed")!=0.0){
    fputs("blocking wait: the event finished without its condition ever changing\n",stderr);
    failed=1;
    goto done;
  }

  /* While a wait is outstanding no event runs, so the step counter cannot move. */
  if(!run_frames(engine,10)){
    fputs("blocking wait: held frames failed\n",stderr);
    failed=1;
    goto done;
  }
  if(gml_global_num(&engine->vm,"fixture_steps")!=held_steps){
    fprintf(stderr,
            "blocking wait: the simulation advanced while the wait was outstanding "
            "(%g steps, expected %g)\n",
            gml_global_num(&engine->vm,"fixture_steps"),held_steps);
    failed=1;
    goto done;
  }

  /* A state written while the wait is outstanding has to carry the suspended run: restoring it
   * must produce a run that is still waiting, not one that has silently dropped the event. */
  if(!save_state(engine,&state,&state_size)){
    fputs("blocking wait: state save failed\n",stderr);
    failed=1;
    goto done;
  }

  /* Change the condition from outside the simulation, exactly as a later frame's input would. */
  gml_set_global_scalar(&engine->vm,"fixture_release",1.0);
  if(!run_frames(engine,2)){
    fputs("blocking wait: resuming frames failed\n",stderr);
    failed=1;
    goto done;
  }
  if(gml_global_num(&engine->vm,"fixture_resumed")!=1.0){
    fputs("blocking wait: the event did not continue past its wait\n",stderr);
    failed=1;
    goto done;
  }
  if(gml_global_num(&engine->vm,"fixture_steps")<=held_steps){
    fputs("blocking wait: the simulation did not resume after the wait ended\n",stderr);
    failed=1;
    goto done;
  }

  /* Back to the saved frame: the restored run is waiting again, holds the simulation again, and
   * finishes when its condition changes again. */
  if(anygm_state_load(engine,state,state_size)!=ANYGM_OK){
    fputs("blocking wait: state load failed\n",stderr);
    failed=1;
    goto done;
  }
  if(gml_global_num(&engine->vm,"fixture_resumed")!=0.0 ||
     gml_global_num(&engine->vm,"fixture_release")!=0.0){
    fputs("blocking wait: the loaded state did not return to the waiting frame\n",stderr);
    failed=1;
    goto done;
  }
  double restored_steps=gml_global_num(&engine->vm,"fixture_steps");
  if(!run_frames(engine,6)){
    fputs("blocking wait: restored held frames failed\n",stderr);
    failed=1;
    goto done;
  }
  if(gml_global_num(&engine->vm,"fixture_steps")!=restored_steps ||
     gml_global_num(&engine->vm,"fixture_resumed")!=0.0){
    fputs("blocking wait: the restored state did not carry the suspended run\n",stderr);
    failed=1;
    goto done;
  }
  gml_set_global_scalar(&engine->vm,"fixture_release",1.0);
  if(!run_frames(engine,2)){
    fputs("blocking wait: restored resuming frames failed\n",stderr);
    failed=1;
    goto done;
  }
  if(gml_global_num(&engine->vm,"fixture_resumed")!=1.0){
    fputs("blocking wait: the restored run did not continue past its wait\n",stderr);
    failed=1;
    goto done;
  }

 done:
  free(state);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  if(!failed) puts("blocking input wait spans frames: ok");
  return !failed;
}

int main(void){
  return blocking_wait_case()?0:1;
}
