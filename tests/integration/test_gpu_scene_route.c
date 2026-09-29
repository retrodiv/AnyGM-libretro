/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Complete neutral content routes for host GPU/CPU comparison. The default invocation checks
 * movement and the room transition through the ordinary CPU engine. --dense checks the larger
 * draw-heavy variant; --keep and --keep-dense print package paths for real libretro hosts. */
#include "anygm.h"
#include "engine_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t frame_digest(const AnygmFrameOutput *output){
  uint64_t hash=UINT64_C(14695981039346656037);
  for(unsigned y=0;y<output->height;y++){
    const uint8_t *row=(const uint8_t*)output->pixels+(size_t)y*output->pitch;
    for(size_t byte=0;byte<(size_t)output->width*4u;byte++){
      hash^=row[byte];
      hash*=UINT64_C(1099511628211);
    }
  }
  return hash;
}

int main(int argc,char **argv){
  AnygmSyntheticContent fixture={0};
  int dense=argc==2 && (!strcmp(argv[1],"--keep-dense") || !strcmp(argv[1],"--dense"));
  int gui_up=argc==2 && !strcmp(argv[1],"--keep-gui-upscale");
  int gui_reduce=argc==2 && !strcmp(argv[1],"--keep-gui-reduce");
  int multiview=argc==2 && !strcmp(argv[1],"--keep-multiview");
  int multiview_offset=argc==2 && !strcmp(argv[1],"--keep-multiview-offset");
  int multiview_fractional=argc==2 && !strcmp(argv[1],"--keep-multiview-fractional");
  int multiview_step=argc==2 && !strcmp(argv[1],"--keep-multiview-step-target");
  int multiview_nested=argc==2 && !strcmp(argv[1],"--keep-multiview-nested-target");
  int multiview_assigned=argc==2 && !strcmp(argv[1],"--keep-multiview-assigned-surface");
  if(!(gui_up?anygm_synthetic_indirect_gui_upscale_content_create(&fixture):
       gui_reduce?anygm_synthetic_indirect_gui_reduce_content_create(&fixture):
       multiview_assigned?anygm_synthetic_multiview_assigned_surface_content_create(&fixture):
       multiview_nested?anygm_synthetic_multiview_nested_target_content_create(&fixture):
       multiview_step?anygm_synthetic_multiview_step_target_content_create(&fixture):
       multiview_fractional?anygm_synthetic_multiview_fractional_gpu_content_create(&fixture):
       multiview_offset?anygm_synthetic_multiview_offset_gpu_content_create(&fixture):
       multiview?anygm_synthetic_multiview_gpu_content_create(&fixture):
       dense?anygm_synthetic_gpu_dense_scene_route_content_create(&fixture):
             anygm_synthetic_gpu_scene_route_content_create(&fixture))) return 1;
  if(gui_up || gui_reduce || multiview || multiview_offset ||
     multiview_fractional || multiview_step || multiview_nested || multiview_assigned){
    puts(fixture.path);
    return 0;
  }
  if(argc==2 && (!strcmp(argv[1],"--keep") || !strcmp(argv[1],"--keep-dense"))){
    puts(fixture.path);
    return 0;
  }
  if(argc!=1 && !(argc==2 && dense)){
    fputs("usage: test_gpu_scene_route [--dense|--keep|--keep-dense|"
          "--keep-gui-upscale|--keep-gui-reduce|--keep-multiview|"
          "--keep-multiview-offset|--keep-multiview-fractional|"
          "--keep-multiview-step-target|--keep-multiview-nested-target|"
          "--keep-multiview-assigned-surface]\n",stderr);
    anygm_synthetic_content_destroy(&fixture);
    return 1;
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
  source.save_directory=fixture.directory;
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  uint64_t first=0,moved=0,second=0,second_moved=0;
  int transition_frame=-1;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  input.keys[ANYGM_KEY_RIGHT]=1;
  for(int frame=0;ok && frame<50;frame++){
    AnygmFrameOutput output={0};
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK && output.pixels &&
       output.width==(dense?1280u:128u) && output.height==(dense?720u:96u) &&
       output.pitch>=(dense?1280u:128u)*4u;
    if(!ok) break;
    if(frame==0) first=frame_digest(&output);
    if(frame==10) moved=frame_digest(&output);
    if(engine->vm.room_index==1 && transition_frame<0){
      transition_frame=frame;
      second=frame_digest(&output);
    }
    if(transition_frame>=0 && frame==transition_frame+5)
      second_moved=frame_digest(&output);
  }
  ok=ok && first && moved && first!=moved && transition_frame>=20 &&
     transition_frame<40 && second && second_moved && second!=second_moved &&
     engine->vm.room_index==1;
  if(!ok)
    fprintf(stderr,"neutral scene route failed: transition=%d first=%llx moved=%llx second=%llx later=%llx room=%d\n",
      transition_frame,(unsigned long long)first,(unsigned long long)moved,
      (unsigned long long)second,(unsigned long long)second_moved,
      engine?engine->vm.room_index:-1);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok?0:1;
}
