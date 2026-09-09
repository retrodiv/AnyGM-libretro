/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"


static int software3d_language_case(void){
  return software3d_raster_run_through(0);
}


static int software3d_particles_case(void){
  return software3d_raster_run_through(1);
}


static int software3d_projection_case(void){
  return software3d_raster_run_through(2);
}


static int software3d_assets_surfaces_case(void){
  return software3d_raster_run_through(3);
}


static int software3d_vm_draw_case(void){
  return software3d_raster_run_through(4);
}


static int software3d_blend_shader_case(void){
  return software3d_raster_run_through(5);
}


static int software3d_primitives_models_case(void){
  return software3d_raster_run_through(6);
}


int main(int argc,char **argv){
  const char *filter=NULL;
  for(int index=1;index<argc;++index){
    if(strcmp(argv[index],"--case")) continue;
    if(index+1>=argc){
      fprintf(stderr,"--case requires a filter\n");
      return EXIT_FAILURE;
    }
    filter=argv[++index];
  }
  static const AnygmTestCase boundary_cases[]={
    {"matrix_inverse",matrix_inverse_fixture},
    {"surface_tiled",surface_tiled_fixture},
    {"software3d_operations",software3d_operation_boundary_fixture},
    {"renderer_state",renderer_state_boundary_fixture},
    {"asset_lookup",asset_lookup_fixture},
    {"external_texture_group",external_texture_group_fixture},
    {"classic_sprite_replace",classic_sprite_replace_fixture},
    {"sprite_instance_metric_scale",sprite_instance_metric_scale_fixture},
  };
  static const AnygmTestCase raster_cases[]={
    {"language_and_font",software3d_language_case},
    {"particles",software3d_particles_case},
    {"projection",software3d_projection_case},
    {"assets_and_surfaces",software3d_assets_surfaces_case},
    {"vm_draw",software3d_vm_draw_case},
    {"blend_and_shader",software3d_blend_shader_case},
    {"primitives_and_models",software3d_primitives_models_case},
  };
  static const AnygmTestCase state_cases[]={
    {"canonical_roundtrip",software3d_state_roundtrip_case},
  };
  const AnygmTestGroup groups[]={
    {"boundary",boundary_cases,sizeof boundary_cases/sizeof boundary_cases[0]},
    {"raster",raster_cases,sizeof raster_cases/sizeof raster_cases[0]},
    {"state",state_cases,sizeof state_cases/sizeof state_cases[0]},
  };
  AnygmTestResult result;
  anygm_test_run_groups(groups,sizeof groups/sizeof groups[0],filter,&result);
  printf("software D3 fixtures: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
