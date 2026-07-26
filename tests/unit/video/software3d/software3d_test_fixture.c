/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"

int fixture_key=-1;
int fixture_key_edge=-1;
uint32_t fixture_network_connected;
double fixture_mouse_x,fixture_mouse_y,fixture_mouse_set_x,fixture_mouse_set_y;

const double software3d_ortho[]={
  0,0,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0
};
const double software3d_enable[]={1};
const double software3d_disable[]={0};

uint32_t fixture_capability(void *userdata,uint32_t capability){
  (void)userdata;
  return capability==ANYGM_HOST_CAPABILITY_NETWORK_CONNECTED?fixture_network_connected:0;
}

static int fixture_input_key(void *userdata,int key, int edge){
  (void)userdata;
  if(key!=fixture_key) return 0;
  /* A pressed edge also means the physical key is currently held. */
  return edge==fixture_key_edge || (fixture_key_edge==1 && edge==0);
}

static int fixture_input_gamepad(void *userdata,int button, int edge){
  (void)userdata; (void)button; (void)edge; return 0;
}

static void fixture_input_mouse(void *userdata,double *rx,double *ry,double *gx,double *gy,
                                double *wx,double *wy,int *held,int *pressed,
                                int *released,int *wheel){
  (void)userdata;
  if(rx) *rx=fixture_mouse_x;
  if(ry) *ry=fixture_mouse_y;
  if(gx) *gx=fixture_mouse_x;
  if(gy) *gy=fixture_mouse_y;
  if(wx) *wx=fixture_mouse_x;
  if(wy) *wy=fixture_mouse_y;
  if(held) *held=0;
  if(pressed) *pressed=0;
  if(released) *released=0;
  if(wheel) *wheel=0;
}

static void fixture_input_mouse_set(void *userdata,double x,double y){
  (void)userdata; fixture_mouse_set_x=x; fixture_mouse_set_y=y;
}

void fixture_attach_input(GmlVM *vm){
  vm->input.key=fixture_input_key;
  vm->input.gamepad=fixture_input_gamepad;
  vm->input.mouse=fixture_input_mouse;
  vm->input.mouse_set=fixture_input_mouse_set;
}

void call_numbers(GmlVM *vm,const char *name,const double *numbers,int count){
  GmlVal args[16];
  for(int i=0;i<count;i++) args[i]=vreal(numbers[i]);
  (void)gml_builtin_call(vm,name,args,count);
}


GmlVal call_values(GmlVM *vm,const char *name,GmlVal *args,int count){
  return gml_builtin_call(vm,name,args,count);
}


int colored_pixels(const uint32_t *pixels,int count){
  int colored=0;
  for(int i=0;i<count;i++) if((pixels[i]&0x00FFFFFFu)!=0) colored++;
  return colored;
}


static int software3d_raster_fixture_init(Software3dRasterFixture *fixture){
  memset(fixture,0,sizeof *fixture);
  memset(&fixture->render,0,sizeof(fixture->render)); memset(&fixture->vm,0,sizeof(fixture->vm));
  fixture->file_services.struct_size=sizeof fixture->file_services;
  fixture->file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&fixture->file_services);
  fixture->vm.host=&fixture->file_services;
  fixture_attach_input(&fixture->vm);
  gml_keyboard_unset_map(&fixture->vm);
  fixture->render.color=0xFFFFFFu; fixture->render.alpha=1; fixture->render.alphablend=1;
  fixture->render.color_write_mask=0x0F;
  fixture->render.blend_equation=fixture->render.blend_equation_alpha=1;
  fixture->render.next_surface_id=1;
  fixture->vm.render=&fixture->render;
  fixture->vm.particles=gml_particle_state_create(&fixture->vm);
  if(!fixture->vm.particles){
    fprintf(stderr,"particle fixture state allocation failed\n");
    return 0;
  }
  gml_vm_software3d_reset(&fixture->vm);
  return 1;
}


static void software3d_raster_fixture_free(Software3dRasterFixture *fixture){
  gml_vm_software3d_reset(&fixture->vm);
  gml_render_free(&fixture->render);
  gml_vm_free(&fixture->vm);
}


int software3d_raster_run_through(size_t final_stage){
  static const Software3dRasterStage stages[]={
    software3d_case_language,
    software3d_case_particles,
    software3d_case_projection,
    software3d_case_assets,
    software3d_case_vm_draw,
    software3d_case_blend_shader,
    software3d_case_primitive_model,
  };
  Software3dRasterFixture fixture;
  if(final_stage>=sizeof stages/sizeof stages[0]) return 0;
  if(!software3d_raster_fixture_init(&fixture)) return 0;
  int ok=1;
  for(size_t stage=0;stage<=final_stage;++stage){
    if(!stages[stage](&fixture)){
      ok=0;
      break;
    }
  }
  software3d_raster_fixture_free(&fixture);
  return ok;
}

