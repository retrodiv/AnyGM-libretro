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
  int pre_draw=argc==2 && !strcmp(argv[1],"--keep-pre-draw");
  int gui_up=argc==2 && !strcmp(argv[1],"--keep-gui-upscale");
  int gui_resident_up=argc==2 && !strcmp(argv[1],"--keep-gui-resident-upscale");
  int forced_owned=argc==2 && !strcmp(argv[1],"--keep-forced-owned");
  int forced_narrow=argc==2 && !strcmp(argv[1],"--keep-forced-narrow");
  int gui_reduce=argc==2 && !strcmp(argv[1],"--keep-gui-reduce");
  int gui_automatic=argc==2 && !strcmp(argv[1],"--keep-gui-automatic");
  int gui_automatic_alpha=argc==2 && !strcmp(argv[1],"--keep-gui-automatic-alpha");
  int gui_automatic_viewless=argc==2 && !strcmp(argv[1],"--keep-gui-automatic-viewless");
  int gui_automatic_viewless_alpha=argc==2 && !strcmp(argv[1],"--keep-gui-automatic-viewless-alpha");
  int gui_automatic_empty=argc==2 && !strcmp(argv[1],"--keep-gui-automatic-empty");
  int multiview=argc==2 && !strcmp(argv[1],"--keep-multiview");
  int multiview_offset=argc==2 && !strcmp(argv[1],"--keep-multiview-offset");
  int multiview_fractional=argc==2 && !strcmp(argv[1],"--keep-multiview-fractional");
  int multiview_step=argc==2 && !strcmp(argv[1],"--keep-multiview-step-target");
  int multiview_nested=argc==2 && !strcmp(argv[1],"--keep-multiview-nested-target");
  int multiview_assigned=argc==2 && !strcmp(argv[1],"--keep-multiview-assigned-surface");
  int canvas_geometry=argc==2 && !strcmp(argv[1],"--keep-canvas-geometry");
  int window_raster=argc==2 && !strcmp(argv[1],"--keep-window-raster");
  int centered_gui=argc==2 && !strcmp(argv[1],"--keep-centered-gui");
  int crt_native=argc==2 && !strcmp(argv[1],"--keep-crt-native");
  int crt_scaled=argc==2 && !strcmp(argv[1],"--keep-crt-scaled");
  int host_box=argc==2 && !strcmp(argv[1],"--keep-host-box");
  int ended_frame=argc==2 && !strcmp(argv[1],"--keep-ended-frame");
  int cursor_frame=argc==2 && !strcmp(argv[1],"--keep-cursor-frame");
  int classic_redraw=argc==2 && !strcmp(argv[1],"--keep-classic-redraw");
  int classic_viewless=argc==2 && !strcmp(argv[1],"--keep-classic-viewless");
  int classic_viewless_resize=argc==2 && !strcmp(argv[1],"--keep-classic-viewless-resize");
  int classic_viewless_ortho_resize=argc==2 && !strcmp(argv[1],"--keep-classic-viewless-ortho-resize");
  int classic_reduced_view_resize=argc==2 && !strcmp(argv[1],"--keep-classic-reduced-view-resize");
  int classic_owned_gui=argc==2 && !strcmp(argv[1],"--keep-classic-owned-gui");
  int classic_manual=argc==2 && !strcmp(argv[1],"--keep-classic-manual-application");
  int classic_manual_scaled=argc==2 && !strcmp(argv[1],"--keep-classic-manual-application-scaled");
  int classic_late_manual=argc==2 && !strcmp(argv[1],"--keep-classic-late-manual-application");
  int classic_single_view=argc==2 && !strcmp(argv[1],"--keep-classic-single-view");
  int classic_identity_present=argc==2 && !strcmp(argv[1],"--keep-classic-identity-present");
  int classic_geometry=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-geometry");
  int classic_borrowed_resize=argc==2 && !strcmp(argv[1],"--keep-classic-borrowed-resize");
  int classic_step_composition=argc==2 && !strcmp(argv[1],"--keep-classic-step-composition");
  int classic_step_base_writer=argc==2 && !strcmp(argv[1],"--keep-classic-step-base-writer");
  int classic_late_step_composition=argc==2 && !strcmp(argv[1],"--keep-classic-late-step-composition");
  int classic_native_step=argc==2 && !strcmp(argv[1],"--keep-classic-native-step");
  int classic_late_native_step=argc==2 && !strcmp(argv[1],"--keep-classic-late-native-step");
  int classic_scaled_step=argc==2 && !strcmp(argv[1],"--keep-classic-scaled-step");
  int classic_filtered_step=argc==2 && !strcmp(argv[1],"--keep-classic-filtered-step");
  int classic_read=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-read");
  int classic_cpu_gui=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-cpu-gui");
  int classic_filter_transition=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-filter-transition");
  int classic_partial_alpha=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-partial-alpha");
  int classic_rectangle=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-rectangle");
  int classic_alpha_rectangle=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-alpha-rectangle");
  int classic_binary=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-binary");
  int classic_source_alpha=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-source-alpha");
  int classic_zero_alpha_part=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-zero-alpha-part");
  int classic_software3d=argc==2 && !strcmp(argv[1],"--keep-classic-single-view-software3d");
  int classic_transition=argc==2 && !strcmp(argv[1],"--keep-classic-transition");
  int large_moving_sound=argc==2 && !strcmp(argv[1],"--keep-large-moving-sound");
  if(!(pre_draw?anygm_synthetic_draw_content_create(&fixture):
       large_moving_sound?anygm_synthetic_large_moving_sound_content_create(&fixture):
       forced_owned?anygm_synthetic_forced_owned_content_create(&fixture):
       forced_narrow?anygm_synthetic_forced_narrow_content_create(&fixture):
       gui_up?anygm_synthetic_indirect_gui_upscale_content_create(&fixture):
       gui_resident_up?anygm_synthetic_resident_indirect_gui_upscale_content_create(&fixture):
       gui_reduce?anygm_synthetic_indirect_gui_reduce_content_create(&fixture):
       gui_automatic?anygm_synthetic_automatic_direct_gui_content_create(&fixture):
       gui_automatic_alpha?anygm_synthetic_automatic_alpha_gui_content_create(&fixture):
       gui_automatic_viewless?anygm_synthetic_automatic_viewless_gui_content_create(&fixture):
       gui_automatic_viewless_alpha?anygm_synthetic_automatic_viewless_alpha_gui_content_create(&fixture):
       gui_automatic_empty?anygm_synthetic_automatic_empty_indirect_gui_content_create(&fixture):
       ended_frame?anygm_synthetic_ended_frame_content_create(&fixture):
       cursor_frame?anygm_synthetic_cursor_frame_content_create(&fixture):
       classic_redraw?anygm_synthetic_classic_redraw_content_create(&fixture):
       classic_viewless?anygm_synthetic_classic_viewless_draw_content_create(&fixture):
       classic_viewless_resize?anygm_synthetic_classic_viewless_resize_content_create(&fixture):
       classic_viewless_ortho_resize?anygm_synthetic_classic_viewless_ortho_resize_content_create(&fixture):
       classic_reduced_view_resize?anygm_synthetic_classic_reduced_view_resize_content_create(&fixture):
       classic_owned_gui?anygm_synthetic_classic_owned_gui_content_create(&fixture):
       classic_manual?anygm_synthetic_classic_manual_application_content_create(&fixture):
       classic_manual_scaled?anygm_synthetic_classic_manual_application_scaled_content_create(&fixture):
       classic_late_manual?anygm_synthetic_classic_late_manual_application_content_create(&fixture):
       classic_single_view?anygm_synthetic_classic_single_view_draw_content_create(&fixture):
       classic_identity_present?anygm_synthetic_classic_identity_present_content_create(&fixture):
       classic_geometry?anygm_synthetic_classic_single_view_geometry_content_create(&fixture):
       classic_borrowed_resize?anygm_synthetic_classic_borrowed_resize_content_create(&fixture):
       classic_step_composition?anygm_synthetic_classic_step_composition_content_create(&fixture):
       classic_step_base_writer?anygm_synthetic_classic_step_base_writer_content_create(&fixture):
       classic_late_step_composition?anygm_synthetic_classic_late_step_composition_content_create(&fixture):
       classic_native_step?anygm_synthetic_classic_native_step_content_create(&fixture):
       classic_late_native_step?anygm_synthetic_classic_late_native_step_content_create(&fixture):
       classic_scaled_step?anygm_synthetic_classic_scaled_step_content_create(&fixture):
       classic_filtered_step?anygm_synthetic_classic_filtered_step_content_create(&fixture):
       classic_read?anygm_synthetic_classic_single_view_read_content_create(&fixture):
       classic_cpu_gui?anygm_synthetic_classic_single_view_cpu_gui_content_create(&fixture):
       classic_filter_transition?anygm_synthetic_classic_single_view_filter_transition_content_create(&fixture):
       classic_partial_alpha?anygm_synthetic_classic_single_view_partial_alpha_content_create(&fixture):
       classic_rectangle?anygm_synthetic_classic_single_view_rectangle_content_create(&fixture):
       classic_alpha_rectangle?anygm_synthetic_classic_single_view_alpha_rectangle_content_create(&fixture):
       classic_binary?anygm_synthetic_classic_single_view_binary_content_create(&fixture):
       classic_source_alpha?anygm_synthetic_classic_single_view_source_alpha_content_create(&fixture):
       classic_zero_alpha_part?anygm_synthetic_classic_single_view_zero_alpha_part_content_create(&fixture):
       classic_software3d?anygm_synthetic_classic_single_view_software3d_content_create(&fixture):
       classic_transition?anygm_synthetic_classic_transition_content_create(&fixture):
       crt_native?anygm_synthetic_crt_native_content_create(&fixture):
       crt_scaled?anygm_synthetic_crt_scaled_content_create(&fixture):
       host_box?anygm_synthetic_host_box_content_create(&fixture):
       canvas_geometry?anygm_synthetic_multiview_canvas_content_create(&fixture):
       window_raster?anygm_synthetic_window_raster_content_create(&fixture):
       centered_gui?anygm_synthetic_direct_gui_sprite_content_create(&fixture):
       multiview_assigned?anygm_synthetic_multiview_assigned_surface_content_create(&fixture):
       multiview_nested?anygm_synthetic_multiview_nested_target_content_create(&fixture):
       multiview_step?anygm_synthetic_multiview_step_target_content_create(&fixture):
       multiview_fractional?anygm_synthetic_multiview_fractional_gpu_content_create(&fixture):
       multiview_offset?anygm_synthetic_multiview_offset_gpu_content_create(&fixture):
       multiview?anygm_synthetic_multiview_gpu_content_create(&fixture):
       dense?anygm_synthetic_gpu_dense_scene_route_content_create(&fixture):
             anygm_synthetic_gpu_scene_route_content_create(&fixture))) return 1;
  if(pre_draw || forced_owned || forced_narrow || gui_up || gui_resident_up || gui_reduce || gui_automatic || gui_automatic_alpha ||
     gui_automatic_viewless || gui_automatic_viewless_alpha || gui_automatic_empty ||
     multiview || multiview_offset ||
     multiview_fractional || multiview_step || multiview_nested || multiview_assigned ||
     canvas_geometry || window_raster || centered_gui ||
     crt_native || crt_scaled || host_box || ended_frame || cursor_frame ||
     classic_redraw || classic_viewless || classic_viewless_resize || classic_viewless_ortho_resize || classic_reduced_view_resize ||
     classic_owned_gui || classic_manual || classic_manual_scaled ||
     classic_late_manual || classic_single_view || classic_identity_present ||
     classic_geometry || classic_borrowed_resize ||
     classic_step_composition || classic_step_base_writer || classic_late_step_composition ||
     classic_native_step || classic_late_native_step || classic_scaled_step || classic_filtered_step ||
     classic_read || classic_cpu_gui || classic_filter_transition || classic_partial_alpha ||
     classic_rectangle || classic_alpha_rectangle || classic_binary || classic_source_alpha || classic_zero_alpha_part ||
     classic_software3d ||
     classic_transition || large_moving_sound){
    puts(fixture.path);
    return 0;
  }
  if(argc==2 && (!strcmp(argv[1],"--keep") || !strcmp(argv[1],"--keep-dense"))){
    puts(fixture.path);
    return 0;
  }
  if(argc!=1 && !(argc==2 && dense)){
    fputs("usage: test_gpu_scene_route [--dense|--keep|--keep-dense|--keep-pre-draw|"
          "--keep-gui-upscale|--keep-gui-resident-upscale|--keep-forced-owned|--keep-forced-narrow|"
          "--keep-gui-reduce|--keep-multiview|"
          "--keep-gui-automatic|--keep-gui-automatic-alpha|"
          "--keep-gui-automatic-empty|"
          "--keep-gui-automatic-viewless|--keep-gui-automatic-viewless-alpha|"
          "--keep-multiview-offset|--keep-multiview-fractional|"
          "--keep-multiview-step-target|--keep-multiview-nested-target|"
          "--keep-multiview-assigned-surface|--keep-canvas-geometry|"
          "--keep-window-raster|--keep-centered-gui|--keep-crt-native|"
          "--keep-crt-scaled|--keep-host-box|--keep-ended-frame|"
          "--keep-cursor-frame|"
          "--keep-classic-redraw|--keep-classic-viewless|--keep-classic-single-view|"
          "--keep-classic-owned-gui|"
          "--keep-classic-manual-application|--keep-classic-manual-application-scaled|"
          "--keep-classic-late-manual-application|"
          "--keep-classic-identity-present|"
          "--keep-classic-single-view-geometry|"
          "--keep-classic-borrowed-resize|"
          "--keep-classic-step-composition|--keep-classic-step-base-writer|"
          "--keep-classic-late-step-composition|"
          "--keep-classic-single-view-read|--keep-classic-single-view-cpu-gui|"
          "--keep-classic-single-view-filter-transition|--keep-classic-single-view-partial-alpha|"
          "--keep-classic-single-view-rectangle|--keep-classic-single-view-binary|"
          "--keep-classic-single-view-alpha-rectangle|"
          "--keep-classic-single-view-source-alpha|"
          "--keep-classic-single-view-zero-alpha-part|"
          "--keep-classic-single-view-software3d|"
          "--keep-classic-transition|"
          "--keep-large-moving-sound]\n",stderr);
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
