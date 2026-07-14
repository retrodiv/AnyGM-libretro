/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gml_render.h"
#include "gml_particle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gml_input_key(int key, int edge){ (void)key; (void)edge; return 0; }
int gml_input_gamepad(int button, int edge){ (void)button; (void)edge; return 0; }
GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);

static void call_numbers(GmlVM *vm,const char *name,const double *numbers,int count){
  GmlVal args[16];
  for(int i=0;i<count;i++) args[i]=vreal(numbers[i]);
  (void)gml_builtin_call(vm,name,args,count);
}

static GmlVal call_values(GmlVM *vm,const char *name,GmlVal *args,int count){
  return gml_builtin_call(vm,name,args,count);
}

static int colored_pixels(const uint32_t *pixels,int count){
  int colored=0;
  for(int i=0;i<count;i++) if((pixels[i]&0x00FFFFFFu)!=0) colored++;
  return colored;
}

static int raster_fixtures(void){
  enum { WIDTH=64, HEIGHT=48 };
  uint32_t pixels[WIDTH*HEIGHT];
  GmlRender render; GmlVM vm;
  memset(&render,0,sizeof(render)); memset(&vm,0,sizeof(vm));
  render.color=0xFFFFFFu; render.alpha=1; render.alphablend=1;
  render.next_surface_id=1;
  vm.render=&render;

  {
    GmlWin settings={0};
    GmlRender initialized;
    settings.classic_version=800;
    settings.classic_interpolate=1;
    if(gml_render_init(&initialized,&settings)!=0 || !initialized.interp){
      fprintf(stderr,"classic texture interpolation setting was ignored\n");
      return 0;
    }
    gml_render_free(&initialized);
    settings.classic_version=0;
    if(gml_render_init(&initialized,&settings)!=0 || initialized.interp){
      fprintf(stderr,"classic texture interpolation leaked into modern content\n");
      return 0;
    }
    gml_render_free(&initialized);
  }

  {
    GmlWin win={0};
    win.classic_version=800;
    vm.win=&win;
    render.presentation_w=640; render.presentation_h=480;
    if(call_values(&vm,"display_get_width",NULL,0).d!=1280 ||
       call_values(&vm,"display_get_height",NULL,0).d!=720 ||
       call_values(&vm,"window_get_width",NULL,0).d!=640 ||
       call_values(&vm,"window_get_height",NULL,0).d!=480){
      fprintf(stderr,"classic virtual display/window size mismatch\n");
      return 0;
    }
    win.classic_version=0;
    if(call_values(&vm,"display_get_width",NULL,0).d!=640 ||
       call_values(&vm,"display_get_height",NULL,0).d!=480){
      fprintf(stderr,"modern presentation display size changed\n");
      return 0;
    }
    render.presentation_w=render.presentation_h=0;
    vm.win=NULL;
  }

  {
    GmlWin win={0};
    uint32_t source[2]={0xFFFF0000u,0xFF0000FFu};
    uint32_t target[3]={0,0,0};
    render.app_surface=source; render.app_w=2; render.app_h=1;
    render.app_surface_opaque=1; render.classic=1; render.win=&win;
    win.classic_version=800; win.classic_scaling=-1;
    gml_render_begin(&render,target,3,1,0,0);
    gml_render_set_pending_underlay(&render,0,0,3,1);
    gml_render_flush_pending_underlay(&render);
    if(target[0]!=0xFFFF0000u || target[1]!=0xFF0000FFu || target[2]!=0xFF0000FFu){
      fprintf(stderr,"classic centre-sampled presentation mismatch\n");
      return 0;
    }
    {
      uint32_t fractional_source[3]={0xFFFF0000u,0xFF00FF00u,0xFF0000FFu};
      uint32_t fractional_target[4]={0,0,0,0};
      render.app_surface=fractional_source; render.app_w=3;
      gml_render_begin(&render,fractional_target,4,1,0,0);
      gml_render_set_pending_underlay(&render,0,0,4,1);
      gml_render_flush_pending_underlay(&render);
      if(fractional_target[0]!=0xFFFF0000u || fractional_target[1]!=0xFF00FF00u ||
         fractional_target[2]!=0xFF0000FFu || fractional_target[3]!=0xFF0000FFu){
        fprintf(stderr,"classic fractional presentation phase mismatch\n");
        return 0;
      }
      render.app_surface=source; render.app_w=2;
    }
    memset(target,0,sizeof(target)); render.classic=0;
    gml_render_begin(&render,target,3,1,0,0);
    gml_render_set_pending_underlay(&render,0,0,3,1);
    gml_render_flush_pending_underlay(&render);
    if(target[0]!=0xFFFF0000u || target[1]!=0xFFFF0000u || target[2]!=0xFF0000FFu){
      fprintf(stderr,"modern leading-edge presentation changed\n");
      return 0;
    }
    memset(target,0,sizeof(target)); render.classic=1; win.classic_scaling=0;
    gml_render_begin(&render,target,3,1,0,0);
    gml_render_set_pending_underlay(&render,0,0,3,1);
    gml_render_flush_pending_underlay(&render);
    if(target[0]!=0xFFFF0000u || target[1]!=0xFFFF0000u || target[2]!=0xFF0000FFu){
      fprintf(stderr,"classic full-scale presentation changed\n");
      return 0;
    }
    render.app_surface=NULL; render.app_w=render.app_h=0; render.classic=0; render.win=NULL;
    win.classic_version=0; win.classic_scaling=0;
  }

  {
    GmlWin win={0};
    uint32_t source[4]={0xFFFF0000u,0xFF0000FFu,0xFFFF0000u,0xFF0000FFu};
    uint32_t target[16]={0};
    render.app_surface=source; render.app_w=2; render.app_h=2;
    render.app_surface_opaque=1; render.classic=1; render.interp=1; render.win=&win;
    win.classic_version=800; win.classic_scaling=0; win.classic_interpolate=1;
    gml_render_begin(&render,target,4,4,0,0);
    gml_render_set_pending_underlay(&render,0,0,4,4);
    gml_render_flush_pending_underlay(&render);
    for(int y=0;y<4;y++){
      if(target[y*4+0]!=0xFFFF0000u || target[y*4+1]!=0xFF800080u ||
         target[y*4+2]!=0xFF0000FFu || target[y*4+3]!=0xFF0000FFu){
        fprintf(stderr,"classic interpolated presentation mismatch\n");
        return 0;
      }
    }
    render.app_surface=NULL; render.app_w=render.app_h=0;
    render.classic=0; render.interp=0; render.win=NULL;
  }

  {
    GmlWin win={0};
    int x=0,y=0;
    win.classic_version=800; win.classic_scaling=-1;
    gml_classic_present_adjust(&win,250,180,500,360,0,&x,&y);
    if(x!=-1 || y!=-1){
      fprintf(stderr,"classic doubled presentation origin mismatch\n");
      return 0;
    }
    x=0; y=0; win.classic_interpolate=1;
    gml_classic_present_adjust(&win,250,180,500,360,1,&x,&y);
    if(x!=0 || y!=0){
      fprintf(stderr,"classic interpolated presentation origin mismatch\n");
      return 0;
    }
    win.classic_interpolate=0;
    x=3; y=4;
    gml_classic_present_adjust(&win,250,180,512,362,0,&x,&y);
    if(x!=3 || y!=4){
      fprintf(stderr,"classic centred fractional presentation changed\n");
      return 0;
    }
    win.classic_scaling=0;
    gml_classic_present_adjust(&win,250,180,512,362,0,&x,&y);
    if(x!=2 || y!=3){
      fprintf(stderr,"classic fixed fractional presentation mismatch\n");
      return 0;
    }
    win.classic_version=701;
    gml_classic_present_adjust(&win,320,240,641,481,0,&x,&y);
    if(x!=1 || y!=2){
      fprintf(stderr,"legacy fractional presentation mismatch\n");
      return 0;
    }
    gml_classic_present_adjust(&win,320,240,320,240,0,&x,&y);
    if(x!=1 || y!=2){
      fprintf(stderr,"native classic presentation changed\n");
      return 0;
    }
  }

  {
    GmlWin win={0};
    int x=9,y=9,w=9,h=9;
    win.classic_version=701;
    if(!gml_classic_present_explicit_port(&win,1,480,432,0,0,160,144,
                                          &x,&y,&w,&h) ||
       x!=0 || y!=0 || w!=160 || h!=144){
      fprintf(stderr,"classic explicit-window port mismatch\n");
      return 0;
    }
    x=y=w=h=9;
    if(gml_classic_present_explicit_port(&win,0,480,432,0,0,160,144,
                                         &x,&y,&w,&h) ||
       x!=9 || y!=9 || w!=9 || h!=9){
      fprintf(stderr,"implicit classic window changed port\n");
      return 0;
    }
    win.classic_version=0;
    if(gml_classic_present_explicit_port(&win,1,480,432,5,7,160,144,
                                         &x,&y,&w,&h)){
      fprintf(stderr,"modern explicit window used classic port\n");
      return 0;
    }
  }

  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  gml_part_reset_all(); gml_part_bind_vm(&vm);
  gml_effect_create(1,10,0,0,0,0xFFFF00);
  for(int i=0;i<5;i++) gml_part_update_all();
  gml_part_system_draw_all(&render);
  int rain_pixels=0,rain_far_pixels=0,rain_alpha_pixels=0;
  for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++) if(pixels[y*WIDTH+x]&0x00FFFFFFu){
    rain_pixels++; if(x>8) rain_far_pixels++;
    if(pixels[y*WIDTH+x]>>24) rain_alpha_pixels++;
  }
  if(rain_pixels<4 || rain_far_pixels<4 || rain_alpha_pixels!=rain_pixels){
    fprintf(stderr,"software rain effect distribution mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  {
    int system=gml_part_system_create();
    int type=gml_part_type_create();
    gml_part_type_shape(type,3);
    gml_part_type_size(type,1,1,0,0);
    gml_part_type_orientation(type,0,0,0,0,0);
    gml_part_type_alpha(type,1,1,1,1);
    gml_part_particles_create_color(system,32,24,type,0x0000FF,1);
    gml_part_system_draw_all(&render);
    int line_pixels=0,line_off_axis=0;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++)
      if(pixels[y*WIDTH+x]&0x00FFFFFFu){ line_pixels++; if(y!=24) line_off_axis++; }
    int edge_red=(pixels[24*WIDTH+5]>>16)&0xFF;
    int core_red=(pixels[24*WIDTH+6]>>16)&0xFF;
    if(line_pixels!=55 || line_off_axis || edge_red<126 || edge_red>129 || core_red!=255){
      fprintf(stderr,"classic particle line cell geometry mismatch\n");
      return 0;
    }
  }
  gml_part_reset_all();

  gml_effect_create(1,3,32,24,0,0x40A0FF);
  if(gml_part_system_count(1)!=75){
    fprintf(stderr,"small firework particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_effect_create(1,0,32,24,0,0x40A0FF);
  if(gml_part_system_count(1)!=21){
    fprintf(stderr,"small explosion particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_effect_create(1,1,32,24,0,0x40A0FF);
  gml_effect_create(1,2,32,24,0,0x40A0FF);
  if(gml_part_system_count(1)!=2){
    fprintf(stderr,"expanding wave particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_effect_create(1,6,32,24,0,0x40A0FF);
  if(gml_part_system_count(1)!=1){
    fprintf(stderr,"shrinking star particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_d3_reset();
  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  int motion_system=gml_part_system_create();
  int motion_type=gml_part_type_create();
  gml_part_type_size(motion_type,0,0,0,0);
  gml_part_type_speed(motion_type,1,1,2,0);
  gml_part_type_direction(motion_type,0,0,0,0);
  gml_part_particles_create(motion_system,10,10,motion_type,1);
  gml_part_update_all();
  gml_part_system_draw_all(&render);
  if((pixels[10*WIDTH+13]&0x00FFFFFFu)==0 || pixels[10*WIDTH+11]!=0){
    int first=-1;
    for(int i=0;i<WIDTH*HEIGHT;i++) if(pixels[i]){ first=i; break; }
    fprintf(stderr,"particle speed increment phase mismatch: x13=%08x x11=%08x first=(%d,%d) count=%d\n",
      pixels[10*WIDTH+13],pixels[10*WIDTH+11],first<0?-1:first%WIDTH,
      first<0?-1:first/WIDTH,colored_pixels(pixels,WIDTH*HEIGHT));
    return 0;
  }
  gml_part_reset_all();

  gml_d3_reset();
  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  GmlVal start_result=call_values(&vm,"d3d_start",NULL,0);
  int default_flags[GML_D3_STATE_FLAG_COUNT];
  double default_values[GML_D3_STATE_VALUE_COUNT];
  uint32_t default_colors[GML_D3_STATE_COLOR_COUNT];
  gml_d3_state_get(default_flags,default_values,default_colors);
  if(start_result.t!=V_REAL || start_result.d!=1 || !default_flags[0] ||
     !default_flags[1] || !default_flags[21] || !default_flags[24] ||
     default_flags[20] || default_values[0]!=WIDTH*.5 ||
     default_values[1]!=HEIGHT*.5 || default_values[2]!=WIDTH ||
     default_values[9]!=0 || default_values[10]!=0 || default_values[11]!=-1 ||
     fabs(default_values[51]-1)>1e-12 || fabs(default_values[52]-32000)>1e-12){
    fprintf(stderr,"software D3 historical start defaults mismatch\n");
    return 0;
  }
  GmlVal end_result=call_values(&vm,"d3d_end",NULL,0);
  gml_d3_state_get(default_flags,default_values,default_colors);
  if(end_result.t!=V_REAL || end_result.d!=1 || default_flags[0]){
    fprintf(stderr,"software D3 historical end result mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_start",NULL,0);
  const double ortho[]={0,0,WIDTH,HEIGHT,0};
  const double enable[]={1};
  const double disable[]={0};
  const double floor_args[]={8,6,0,24,18,0,-1,1,1};
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  call_numbers(&vm,"d3d_set_culling",enable,1);
  call_numbers(&vm,"d3d_draw_floor",floor_args,9);
  if((pixels[10*WIDTH+10]&0x00FFFFFFu)==0 || pixels[4*WIDTH+4]!=0 ||
     colored_pixels(pixels,WIDTH*HEIGHT)<150){
    fprintf(stderr,"software D3 orthographic raster mismatch\n");
    return 0;
  }

  /* A floor crossing the perspective near plane must remain a continuous projected polygon.
   * This is the common outdoor-camera shape; a bad clipped-fan depth interpolation used to leave
   * whole alternating scanline bands untouched. */
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  const double perspective[]={32,14,5,32,24,0,0,0,1};
  const double perspective_floor[]={0,0,0,64,48,0,-1,5,5};
  call_numbers(&vm,"d3d_set_projection",perspective,9);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  call_numbers(&vm,"d3d_draw_floor",perspective_floor,9);
  int first_row=-1,last_row=-1,empty_inside=0;
  for(int y=0;y<HEIGHT;y++){
    int row=colored_pixels(pixels+y*WIDTH,WIDTH);
    if(row){ if(first_row<0) first_row=y; last_row=y; }
  }
  if(first_row>=0) for(int y=first_row;y<=last_row;y++)
    if(colored_pixels(pixels+y*WIDTH,WIDTH)==0) empty_inside++;
  if(first_row<0 || empty_inside){
    fprintf(stderr,"software D3 perspective floor has %d empty interior rows (%d..%d)\n",
            empty_inside,first_row,last_row);
    return 0;
  }
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);

  call_numbers(&vm,"d3d_transform_set_identity",NULL,0);
  call_numbers(&vm,"d3d_transform_stack_push",NULL,0);
  const double translation[]={16,8,0};
  call_numbers(&vm,"d3d_transform_add_translation",translation,3);
  int transform_flags[GML_D3_STATE_FLAG_COUNT];
  double transform_values[GML_D3_STATE_VALUE_COUNT];
  uint32_t transform_colors[GML_D3_STATE_COLOR_COUNT];
  gml_d3_state_get(transform_flags,transform_values,transform_colors);
  if(transform_flags[25]!=1 || transform_values[68]!=16 || transform_values[69]!=8){
    fprintf(stderr,"software D3 transform stack push mismatch\n");
    return 0;
  }
  memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_draw_floor",floor_args,9);
  if((pixels[18*WIDTH+26]&0x00FFFFFFu)==0 || pixels[10*WIDTH+10]!=0){
    fprintf(stderr,"software D3 translated raster mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_transform_stack_pop",NULL,0);
  gml_d3_state_get(transform_flags,transform_values,transform_colors);
  if(transform_flags[25]!=0 || transform_values[68]!=0 || transform_values[69]!=0){
    fprintf(stderr,"software D3 transform stack pop mismatch\n");
    return 0;
  }

  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  const double triangle_kind[]={4};
  const double vertex_a[]={8,8,0,0x0000FF,1};
  const double vertex_b[]={56,8,0,0x00FF00,1};
  const double vertex_c[]={32,40,0,0xFF0000,1};
  call_numbers(&vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&vm,"d3d_vertex_color",vertex_a,5);
  call_numbers(&vm,"d3d_vertex_color",vertex_b,5);
  call_numbers(&vm,"d3d_vertex_color",vertex_c,5);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  uint32_t center=pixels[20*WIDTH+32]&0x00FFFFFFu;
  if(colored_pixels(pixels,WIDTH*HEIGHT)<600 || ((center>>16)&255)<20 ||
     ((center>>8)&255)<20 || (center&255)<20){
    fprintf(stderr,"software D3 immediate triangle mismatch: center=%06x\n",center);
    return 0;
  }

  int surface=gml_surface_create(&render,2,2);
  if(surface<=0){ fprintf(stderr,"software D3 texture surface create mismatch\n"); return 0; }
  GmlSurface *surface_data=&render.surface[surface-1];
  surface_data->px[0]=0xFFFF0000u; surface_data->px[1]=0xFF00FF00u;
  surface_data->px[2]=0xFF0000FFu; surface_data->px[3]=0xFFFFFFFFu;
  GmlVal surface_arg=vreal(surface);
  GmlVal surface_texture=call_values(&vm,"surface_get_texture",&surface_arg,1);
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection",perspective,9);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  double textured_floor[]={0,0,0,64,48,0,surface_texture.d,5,5};
  call_numbers(&vm,"d3d_draw_floor",textured_floor,9);
  first_row=last_row=-1; empty_inside=0;
  for(int y=0;y<HEIGHT;y++){
    int row=colored_pixels(pixels+y*WIDTH,WIDTH);
    if(row){ if(first_row<0) first_row=y; last_row=y; }
  }
  if(first_row>=0) for(int y=first_row;y<=last_row;y++)
    if(colored_pixels(pixels+y*WIDTH,WIDTH)==0) empty_inside++;
  if(first_row<0 || empty_inside){
    fprintf(stderr,"software D3 textured floor has %d empty interior rows (%d..%d)\n",
            empty_inside,first_row,last_row);
    return 0;
  }
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  double textured_begin[2]={4,surface_texture.d};
  const double textured_a[]={8,8,0,0,0};
  const double textured_b[]={56,8,0,1,0};
  const double textured_c[]={56,40,0,1,1};
  const double textured_d[]={8,40,0,0,1};
  memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_primitive_begin_texture",textured_begin,2);
  call_numbers(&vm,"d3d_vertex_texture",textured_a,5);
  call_numbers(&vm,"d3d_vertex_texture",textured_b,5);
  call_numbers(&vm,"d3d_vertex_texture",textured_c,5);
  call_numbers(&vm,"d3d_vertex_texture",textured_a,5);
  call_numbers(&vm,"d3d_vertex_texture",textured_c,5);
  call_numbers(&vm,"d3d_vertex_texture",textured_d,5);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  if((pixels[14*WIDTH+16]&0x00FFFFFFu)!=0xFF0000u ||
     (pixels[14*WIDTH+48]&0x00FFFFFFu)!=0x00FF00u ||
     (pixels[34*WIDTH+16]&0x00FFFFFFu)!=0x0000FFu){
    fprintf(stderr,"software D3 surface texture mismatch: %06x %06x %06x\n",
      pixels[14*WIDTH+16]&0xFFFFFFu,pixels[14*WIDTH+48]&0xFFFFFFu,pixels[34*WIDTH+16]&0xFFFFFFu);
    return 0;
  }
  surface_data->px[0]=0xFF020000u;
  surface_data->px[1]=surface_data->px[2]=surface_data->px[3]=0xFF000000u;
  call_numbers(&vm,"texture_set_interpolation",enable,1);
  const double interpolated_point[]={30,20,0,.5,.5,0x0000FE,1};
  memset(pixels,0,sizeof(pixels));
  double interpolated_begin[2]={1,surface_texture.d};
  call_numbers(&vm,"d3d_primitive_begin_texture",interpolated_begin,2);
  call_numbers(&vm,"d3d_vertex_texture_color",interpolated_point,7);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  if((pixels[20*WIDTH+30]&0x00FFFFFFu)!=0){
    fprintf(stderr,"software D3 bilinear modulation quantized early: %06x\n",
      pixels[20*WIDTH+30]&0x00FFFFFFu);
    return 0;
  }
  call_numbers(&vm,"texture_set_interpolation",disable,1);
  surface_data->px[0]=0xFFFF0000u; surface_data->px[1]=0xFF00FF00u;
  surface_data->px[2]=0xFF0000FFu; surface_data->px[3]=0xFFFFFFFFu;
  int runtime_sprite=gml_sprite_create_from_surface(&render,surface,0,0,2,2,0,0,0,0);
  GmlVal sprite_args[2]={vreal(runtime_sprite),vreal(0)};
  GmlVal sprite_texture=call_values(&vm,"sprite_get_texture",sprite_args,2);
  double sprite_begin[2]={1,sprite_texture.d};
  const double sprite_point[]={30,20,0,.25,.25};
  memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_primitive_begin_texture",sprite_begin,2);
  call_numbers(&vm,"d3d_vertex_texture",sprite_point,5);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  if((pixels[20*WIDTH+30]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 runtime sprite texture mismatch\n");
    return 0;
  }
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  GmlVal positioned[11]={vreal(runtime_sprite),vreal(0),
    vreal(10),vreal(10),vreal(18),vreal(10),vreal(20),vreal(18),vreal(8),vreal(18),vreal(1)};
  (void)call_values(&vm,"draw_sprite_pos",positioned,11);
  if((pixels[10*WIDTH+10]&0x00FFFFFFu)!=0xFF0000u ||
     (pixels[10*WIDTH+17]&0x00FFFFFFu)!=0x00FF00u ||
     (pixels[17*WIDTH+9]&0x00FFFFFFu)!=0x0000FFu ||
     (pixels[17*WIDTH+18]&0x00FFFFFFu)!=0xFFFFFFu || pixels[5*WIDTH+5]!=0){
    fprintf(stderr,"software positioned sprite quad mismatch: %06x %06x %06x %06x\n",
      pixels[10*WIDTH+10]&0xFFFFFFu,pixels[10*WIDTH+17]&0xFFFFFFu,
      pixels[17*WIDTH+9]&0xFFFFFFu,pixels[17*WIDTH+18]&0xFFFFFFu);
    return 0;
  }
  uint8_t *round_rgba=malloc(4);
  if(!round_rgba){
    fprintf(stderr,"software runtime alpha fixture allocation mismatch\n");
    return 0;
  }
  round_rgba[0]=round_rgba[1]=round_rgba[2]=0; round_rgba[3]=1;
  int round_sprite=gml_sprite_append_from_rgba(&render,round_rgba,1,1,0,0,"<runtime-alpha>");
  if(round_sprite<0){
    fprintf(stderr,"software runtime alpha fixture creation mismatch\n");
    return 0;
  }
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  render.classic=1;
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  pixels[10*WIDTH+10]=0xFF4984ACu;
  gml_draw_sprite_ext(&render,round_sprite,0,10,10,1,1,0,0xFFFFFF,1);
  if(pixels[10*WIDTH+10]!=0xFF4983ABu){
    fprintf(stderr,"software classic runtime alpha rounding mismatch\n");
    return 0;
  }
  render.classic=0; memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  pixels[10*WIDTH+10]=0xFF4984ACu;
  gml_draw_sprite_ext(&render,round_sprite,0,10,10,1,1,0,0xFFFFFF,1);
  if(pixels[10*WIDTH+10]!=0xFF4883ABu){
    fprintf(stderr,"software modern runtime alpha rounding changed\n");
    return 0;
  }
  {
    GmlInstance relative_self={0}; relative_self.x=13; relative_self.y=17;
    GmlVal relative_on=vreal(1),relative_off=vreal(0);
    GmlVal action_sprite_args[4]={vreal(runtime_sprite),vreal(2),vreal(3),vreal(0)};
    vm.cur_self=&relative_self;
    memset(pixels,0,sizeof(pixels));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    (void)call_values(&vm,"action_set_relative",&relative_on,1);
    (void)call_values(&vm,"action_draw_sprite",action_sprite_args,4);
    if((pixels[20*WIDTH+15]&0x00FFFFFFu)!=0xFF0000u || pixels[3*WIDTH+2]!=0){
      fprintf(stderr,"software relative action sprite position mismatch\n");
      return 0;
    }
    *gml_varmap_put(&vm.globals,"lives")=vreal(2);
    GmlVal life_args[3]={vreal(2),vreal(3),vreal(runtime_sprite)};
    memset(pixels,0,sizeof(pixels));
    (void)call_values(&vm,"action_draw_life_images",life_args,3);
    if((pixels[20*WIDTH+15]&0x00FFFFFFu)!=0xFF0000u ||
       (pixels[20*WIDTH+17]&0x00FFFFFFu)!=0xFF0000u){
      fprintf(stderr,"software relative action life-image position mismatch\n");
      return 0;
    }
    *gml_varmap_put(&vm.globals,"health")=vreal(50);
    GmlVal health_args[6]={vreal(1),vreal(2),vreal(21),vreal(6),vreal(0),vreal(0)};
    memset(pixels,0,sizeof(pixels));
    (void)call_values(&vm,"action_draw_health",health_args,6);
    if((pixels[21*WIDTH+18]&0x00FFFFFFu)!=0xFFFF00u || pixels[21*WIDTH+30]!=0){
      fprintf(stderr,"software relative action health-bar mismatch\n");
      return 0;
    }
    (void)call_values(&vm,"action_set_relative",&relative_off,1);
    vm.cur_self=NULL;
  }
  for(int i=0;i<4;i++) surface_data->px[i]=0xFFFFFFFFu;
  int depth_sprite=gml_sprite_create_from_surface(&render,surface,0,0,2,2,0,0,0,0);
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  call_numbers(&vm,"d3d_set_hidden",enable,1);
  const double far_depth[]={10},farther_depth[]={20},near_depth[]={-10};
  call_numbers(&vm,"d3d_set_depth",far_depth,1);
  gml_draw_sprite_ext(&render,depth_sprite,0,24,12,8,8,0,0x0000FF,1);
  call_numbers(&vm,"d3d_set_depth",farther_depth,1);
  gml_draw_sprite_ext(&render,depth_sprite,0,24,12,8,8,0,0x00FF00,1);
  if((pixels[18*WIDTH+30]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D sprite far-depth mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_set_depth",near_depth,1);
  gml_draw_sprite_ext(&render,depth_sprite,0,24,12,8,8,0,0x00FF00,1);
  if((pixels[18*WIDTH+30]&0x00FFFFFFu)!=0x00FF00u){
    fprintf(stderr,"software D3 2D sprite near-depth mismatch\n");
    return 0;
  }
  render.atlas=calloc(1,sizeof(*render.atlas)); render.tpag=calloc(1,sizeof(*render.tpag));
  render.bg=calloc(1,sizeof(*render.bg)); render.n_atlas=render.n_tpag=render.n_bg=1;
  if(!render.atlas || !render.tpag || !render.bg || !(render.atlas[0].px=malloc(16))){
    fprintf(stderr,"software D3 background texture allocation mismatch\n");
    return 0;
  }
  render.atlas[0].w=render.atlas[0].h=2;
  for(int i=0;i<4;i++){
    render.atlas[0].px[i*4]=render.atlas[0].px[i*4+1]=render.atlas[0].px[i*4+2]=render.atlas[0].px[i*4+3]=255;
  }
  render.tpag[0].atlas=0; render.tpag[0].sw=render.tpag[0].sh=2;
  render.tpag[0].bw=render.tpag[0].bh=2; render.bg[0].tpag=0;
  render.classic=1; gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  for(int i=0;i<4;i++){
    render.atlas[0].px[i*4]=render.atlas[0].px[i*4+1]=render.atlas[0].px[i*4+2]=192;
    render.atlas[0].px[i*4+3]=254;
  }
  pixels[5*WIDTH+5]=0xFF8F4848u;
  gml_draw_background_part_ext(&render,0,0,0,2,2,5,5,1,1,0xFFFFFF,1);
  if(pixels[5*WIDTH+5]!=0xFFC0BFBFu){
    fprintf(stderr,"software classic atlas fixed-point blend mismatch: %08x\n",pixels[5*WIDTH+5]);
    return 0;
  }
  for(int i=0;i<4;i++){
    render.atlas[0].px[i*4]=255;
    render.atlas[0].px[i*4+1]=64;
    render.atlas[0].px[i*4+2]=64;
    render.atlas[0].px[i*4+3]=111;
  }
  pixels[5*WIDTH+5]=0xFF46AB70u;
  gml_draw_background_ext(&render,0,5,5,1,1,0xFFFFFF,1);
  if(pixels[5*WIDTH+5]!=0xFF977D5Bu){
    fprintf(stderr,"software classic cached atlas blend mismatch: %08x\n",pixels[5*WIDTH+5]);
    return 0;
  }
  free(render.tpag[0].alpha_row_min); render.tpag[0].alpha_row_min=NULL;
  free(render.tpag[0].alpha_row_max); render.tpag[0].alpha_row_max=NULL;
  free(render.tpag[0].alpha_runs); render.tpag[0].alpha_runs=NULL;
  free(render.tpag[0].argb_cache); render.tpag[0].argb_cache=NULL;
  render.tpag[0].alpha_scanned=0;
  render.tpag[0].alpha_runs_built=0;
  render.tpag[0].alpha_run_count=0;
  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  for(int i=0;i<4;i++) render.atlas[0].px[i*4+3]=255;
  render.atlas[0].px[0]=0;   render.atlas[0].px[1]=16; render.atlas[0].px[2]=255;
  render.atlas[0].px[4]=255; render.atlas[0].px[5]=0;  render.atlas[0].px[6]=0;
  pixels[5*WIDTH+5]=pixels[5*WIDTH+6]=0xFFC0C0C0u;
  gml_draw_background_ext(&render,0,5.2,5.2,.75,1,0xFFFFFF,231.0/255.0);
  if(pixels[5*WIDTH+5]!=0xFF1220F9u || pixels[5*WIDTH+6]!=0xFFF91212u){
    fprintf(stderr,"software classic scaled atlas phase/blend mismatch: %08x %08x\n",
            pixels[5*WIDTH+5],pixels[5*WIDTH+6]);
    return 0;
  }
  pixels[8*WIDTH+6]=0xFFC0C0C0u;
  gml_draw_background_ext(&render,0,5.5,8,.5,1,0xFFFFFF,1);
  if(pixels[8*WIDTH+6]!=0xFFFF0000u){
    fprintf(stderr,"software classic reciprocal-scale texel tie mismatch: %08x\n",
            pixels[8*WIDTH+6]);
    return 0;
  }
  for(int i=0;i<4;i++)
    render.atlas[0].px[i*4]=render.atlas[0].px[i*4+1]=render.atlas[0].px[i*4+2]=render.atlas[0].px[i*4+3]=255;
  int flipped_sprite=render.n_spr++;
  GmlSprite *grown_sprites=realloc(render.spr,(size_t)render.n_spr*sizeof(*render.spr));
  if(!grown_sprites){
    fprintf(stderr,"software sprite flip allocation mismatch\n");
    return 0;
  }
  render.spr=grown_sprites;
  GmlSprite *flipped=&render.spr[flipped_sprite];
  memset(flipped,0,sizeof(*flipped));
  flipped->w=flipped->h=2; flipped->n_frames=1;
  flipped->frame=malloc(sizeof(*flipped->frame));
  if(!flipped->frame){
    fprintf(stderr,"software sprite flip frame allocation mismatch\n");
    return 0;
  }
  flipped->frame[0]=0;
  render.classic=1;
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  gml_draw_sprite_ext(&render,flipped_sprite,0,20,20,-1,1,0,0xFFFFFF,1);
  gml_draw_sprite_ext(&render,flipped_sprite,0,30,30,1,-1,0,0xFFFFFF,1);
  if((pixels[20*WIDTH+18]&0x00FFFFFFu)==0 ||
     (pixels[20*WIDTH+19]&0x00FFFFFFu)==0 || pixels[20*WIDTH+20]!=0 ||
     (pixels[28*WIDTH+30]&0x00FFFFFFu)==0 ||
     (pixels[29*WIDTH+30]&0x00FFFFFFu)==0 || pixels[30*WIDTH+30]!=0){
    fprintf(stderr,"software negative sprite scale anchor mismatch\n");
    return 0;
  }
  render.classic=0;
  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  gml_draw_sprite_ext(&render,flipped_sprite,0,20,20,-1,1,0,0xFFFFFF,1);
  gml_draw_sprite_ext(&render,flipped_sprite,0,30,30,1,-1,0,0xFFFFFF,1);
  if((pixels[20*WIDTH+19]&0x00FFFFFFu)==0 ||
     (pixels[20*WIDTH+20]&0x00FFFFFFu)==0 || pixels[20*WIDTH+18]!=0 ||
     (pixels[29*WIDTH+30]&0x00FFFFFFu)==0 ||
     (pixels[30*WIDTH+30]&0x00FFFFFFu)==0 || pixels[28*WIDTH+30]!=0){
    fprintf(stderr,"software modern negative sprite scale anchor mismatch\n");
    return 0;
  }
  render.classic=1;
  render.tpag[0].tx=render.tpag[0].ty=1;
  render.tpag[0].bw=render.tpag[0].bh=4;
  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  gml_draw_background_tiled(&render,0,0,0,1,1);
  if((pixels[1*WIDTH+1]&0x00FFFFFFu)==0 ||
     (pixels[2*WIDTH+2]&0x00FFFFFFu)==0 ||
     (pixels[1*WIDTH+5]&0x00FFFFFFu)==0 ||
     (pixels[2*WIDTH+6]&0x00FFFFFFu)==0 ||
     pixels[1*WIDTH+3]!=0 || pixels[1*WIDTH+4]!=0){
    fprintf(stderr,"software trimmed tiled background period mismatch\n");
    return 0;
  }
  render.classic=0;
  render.tpag[0].tx=render.tpag[0].ty=0;
  render.tpag[0].bw=render.tpag[0].bh=2;
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_hidden",enable,1);
  call_numbers(&vm,"d3d_set_depth",far_depth,1);
  gml_draw_background_ext(&render,0,40,10,8,8,0x0000FF,1);
  call_numbers(&vm,"d3d_set_depth",farther_depth,1);
  gml_draw_background_ext(&render,0,40,10,8,8,0x00FF00,1);
  if((pixels[16*WIDTH+46]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D background depth mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_set_depth",far_depth,1);
  gml_draw_sprite_part_ext(&render,depth_sprite,0,0,0,1,1,10,30,8,8,0x0000FF,1);
  call_numbers(&vm,"d3d_set_depth",farther_depth,1);
  gml_draw_sprite_part_ext(&render,depth_sprite,0,0,0,1,1,10,30,8,8,0x00FF00,1);
  if((pixels[34*WIDTH+14]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D sprite-part depth mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_set_depth",far_depth,1);
  gml_d3_draw_atlas_part_2d(&render,0,0,0,1,1,22,30,8,8,0x0000FF,1);
  call_numbers(&vm,"d3d_set_depth",farther_depth,1);
  gml_d3_draw_atlas_part_2d(&render,0,0,0,1,1,22,30,8,8,0x00FF00,1);
  if((pixels[34*WIDTH+26]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D atlas-part depth mismatch\n");
    return 0;
  }
  const double rectangle_2d[]={34,30,50,44,0};
  call_numbers(&vm,"d3d_set_depth",far_depth,1); render.color=0x0000FFu;
  call_numbers(&vm,"draw_rectangle",rectangle_2d,5);
  call_numbers(&vm,"d3d_set_depth",farther_depth,1); render.color=0x00FF00u;
  call_numbers(&vm,"draw_rectangle",rectangle_2d,5);
  if((pixels[36*WIDTH+40]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D rectangle depth mismatch\n");
    return 0;
  }
  const double circle_2d[]={42,37,5,0};
  call_numbers(&vm,"d3d_set_depth",near_depth,1); render.color=0xFF0000u;
  call_numbers(&vm,"draw_circle",circle_2d,4);
  if((pixels[37*WIDTH+42]&0x00FFFFFFu)!=0x0000FFu){
    fprintf(stderr,"software D3 2D circle depth mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_set_depth",far_depth,1);
  gml_draw_surface_stretched(&render,surface,2,30,8,8,0x0000FF,1);
  call_numbers(&vm,"d3d_set_depth",farther_depth,1);
  gml_draw_surface_stretched(&render,surface,2,30,8,8,0x00FF00,1);
  if((pixels[34*WIDTH+6]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D surface depth mismatch\n");
    return 0;
  }
  const double draw_primitive_kind[]={4};
  const double draw_vertex_a[]={52,30,0x0000FF,1};
  const double draw_vertex_b[]={62,30,0x0000FF,1};
  const double draw_vertex_c[]={57,44,0x0000FF,1};
  call_numbers(&vm,"d3d_set_depth",far_depth,1);
  call_numbers(&vm,"draw_primitive_begin",draw_primitive_kind,1);
  call_numbers(&vm,"draw_vertex_color",draw_vertex_a,4);
  call_numbers(&vm,"draw_vertex_color",draw_vertex_b,4);
  call_numbers(&vm,"draw_vertex_color",draw_vertex_c,4);
  call_numbers(&vm,"draw_primitive_end",NULL,0);
  if((pixels[35*WIDTH+57]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D immediate primitive mismatch\n");
    return 0;
  }
  render.color=0xFFFFFFu;
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);

  memset(pixels,0,sizeof(pixels));
  const double point_kind[]={1};
  const double point_a[]={10,12,0,0x0000FF,1};
  const double point_b[]={20,14,0,0x00FF00,1};
  call_numbers(&vm,"d3d_primitive_begin",point_kind,1);
  call_numbers(&vm,"d3d_vertex_color",point_a,5);
  call_numbers(&vm,"d3d_vertex_color",point_b,5);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  if((pixels[12*WIDTH+10]&0x00FFFFFFu)!=0xFF0000u ||
     (pixels[14*WIDTH+20]&0x00FFFFFFu)!=0x00FF00u ||
     colored_pixels(pixels,WIDTH*HEIGHT)!=2){
    fprintf(stderr,"software D3 immediate point-list mismatch: a=%06x b=%06x count=%d\n",
            pixels[12*WIDTH+10]&0x00FFFFFFu,pixels[14*WIDTH+20]&0x00FFFFFFu,
            colored_pixels(pixels,WIDTH*HEIGHT));
    return 0;
  }

  memset(pixels,0,sizeof(pixels));
  const double line_kind[]={2};
  const double line_a[]={4,4,0,0x0000FF,1};
  const double line_b[]={60,40,0,0xFF0000,1};
  call_numbers(&vm,"d3d_primitive_begin",line_kind,1);
  call_numbers(&vm,"d3d_vertex_color",line_a,5);
  call_numbers(&vm,"d3d_vertex_color",line_b,5);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  center=pixels[22*WIDTH+32]&0x00FFFFFFu;
  if(colored_pixels(pixels,WIDTH*HEIGHT)<50 || (center&255)<40 || ((center>>16)&255)<40){
    fprintf(stderr,"software D3 immediate line-list mismatch: center=%06x\n",center);
    return 0;
  }

  memset(pixels,0,sizeof(pixels));
  const double strip_kind[]={3};
  const double strip_a[]={8,8,0,0xFFFFFF,1};
  const double strip_b[]={8,32,0,0xFFFFFF,1};
  const double strip_c[]={40,32,0,0xFFFFFF,1};
  call_numbers(&vm,"d3d_primitive_begin",strip_kind,1);
  call_numbers(&vm,"d3d_vertex_color",strip_a,5);
  call_numbers(&vm,"d3d_vertex_color",strip_b,5);
  call_numbers(&vm,"d3d_vertex_color",strip_c,5);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  if((pixels[20*WIDTH+8]&0x00FFFFFFu)!=0xFFFFFFu ||
     (pixels[32*WIDTH+24]&0x00FFFFFFu)!=0xFFFFFFu){
    fprintf(stderr,"software D3 immediate line-strip mismatch\n");
    return 0;
  }

  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  call_numbers(&vm,"d3d_set_lighting",enable,1);
  const double normal_a[]={8,8,0,0,0,1,0xFFFFFF,1};
  const double normal_b[]={56,8,0,0,0,1,0xFFFFFF,1};
  const double normal_c[]={32,40,0,0,0,1,0xFFFFFF,1};
  call_numbers(&vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_a,8);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_b,8);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_c,8);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  uint32_t dark=pixels[20*WIDTH+32]&0x00FFFFFFu;
  const double directional[]={0,0,0,-1,0xFFFFFF};
  const double light_enable[]={0,1};
  call_numbers(&vm,"d3d_light_define_direction",directional,5);
  call_numbers(&vm,"d3d_light_enable",light_enable,2);
  memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_a,8);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_b,8);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_c,8);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  uint32_t bright=pixels[20*WIDTH+32]&0x00FFFFFFu;
  int dark_sum=(dark&255)+((dark>>8)&255)+((dark>>16)&255);
  int bright_sum=(bright&255)+((bright>>8)&255)+((bright>>16)&255);
  if(dark_sum>=200 || bright_sum<700){
    fprintf(stderr,"software D3 normal lighting mismatch: dark=%06x bright=%06x\n",dark,bright);
    return 0;
  }
  const double light_disable[]={0,0};
  const double ambient_white[]={0xFFFFFF};
  call_numbers(&vm,"d3d_light_enable",light_disable,2);
  call_numbers(&vm,"d3d_light_define_ambient",ambient_white,1);
  memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_primitive_begin",triangle_kind,1);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_a,8);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_b,8);
  call_numbers(&vm,"d3d_vertex_normal_color",normal_c,8);
  call_numbers(&vm,"d3d_primitive_end",NULL,0);
  uint32_t ambient=pixels[20*WIDTH+32]&0x00FFFFFFu;
  int ambient_sum=(ambient&255)+((ambient>>8)&255)+((ambient>>16)&255);
  if(ambient_sum<700){
    fprintf(stderr,"software D3 ambient lighting mismatch: pixel=%06x\n",ambient);
    return 0;
  }

  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  GmlVal created=call_values(&vm,"d3d_model_create",NULL,0);
  if(created.t!=V_REAL || created.d!=0){
    fprintf(stderr,"software D3 model create mismatch\n");
    return 0;
  }
  const double model_begin[]={0,4};
  const double model_a[]={0,8,8,0,0x0000FF,1};
  const double model_b[]={0,56,8,0,0x00FF00,1};
  const double model_c[]={0,32,40,0,0xFF0000,1};
  const double model_id[]={0};
  const double model_draw[]={0,0,0,0,-1};
  call_numbers(&vm,"d3d_model_primitive_begin",model_begin,2);
  call_numbers(&vm,"d3d_model_vertex_color",model_a,6);
  call_numbers(&vm,"d3d_model_vertex_color",model_b,6);
  call_numbers(&vm,"d3d_model_vertex_color",model_c,6);
  call_numbers(&vm,"d3d_model_primitive_end",model_id,1);
  call_numbers(&vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(pixels,WIDTH*HEIGHT)<600){
    fprintf(stderr,"software D3 model draw mismatch\n");
    return 0;
  }
  const char *model_path="/tmp/gml_d3_model_fixture.bin";
  GmlVal file_args[2]={vreal(0),vstr(model_path)};
  if(call_values(&vm,"d3d_model_save",file_args,2).d!=1){
    fprintf(stderr,"software D3 model save mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_model_clear",model_id,1);
  if(call_values(&vm,"d3d_model_load",file_args,2).d!=1){
    fprintf(stderr,"software D3 model load mismatch\n");
    remove(model_path); return 0;
  }
  remove(model_path);
  size_t model_state_size=gml_vm_state_size(&vm),model_written=0,model_used=0;
  void *model_state=malloc(model_state_size);
  if(!model_state || !gml_vm_state_save(&vm,model_state,model_state_size,&model_written)){
    fprintf(stderr,"software D3 model state save mismatch\n");
    free(model_state); return 0;
  }
  call_numbers(&vm,"d3d_model_destroy",model_id,1);
  if(!gml_vm_state_load(&vm,model_state,model_written,&model_used) || model_used!=model_written){
    fprintf(stderr,"software D3 model state load mismatch\n");
    free(model_state); return 0;
  }
  free(model_state); memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(pixels,WIDTH*HEIGHT)<600){
    fprintf(stderr,"software D3 restored model draw mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_model_clear",model_id,1);
  const double model_floor[]={0,8,6,0,24,18,0,1,1};
  call_numbers(&vm,"d3d_model_floor",model_floor,9);
  memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(pixels,WIDTH*HEIGHT)<150){
    fprintf(stderr,"software D3 generated model shape mismatch\n");
    return 0;
  }
  FILE *legacy=fopen(model_path,"w");
  int legacy_ok=legacy&&fprintf(legacy,"100\n1\n15 8 6 0 24 18 0 1 1\n")>=0;
  if(legacy && fclose(legacy)!=0) legacy_ok=0;
  if(!legacy_ok){
    fprintf(stderr,"software D3 legacy model fixture write mismatch\n");
    return 0;
  }
  if(call_values(&vm,"d3d_model_load",file_args,2).d!=1){
    fprintf(stderr,"software D3 legacy model load mismatch\n");
    remove(model_path); return 0;
  }
  remove(model_path); memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_model_draw",model_draw,5);
  if(colored_pixels(pixels,WIDTH*HEIGHT)<150){
    fprintf(stderr,"software D3 legacy model raster mismatch\n");
    return 0;
  }

  const double projection[]={0,-10,0, 0,0,0, 0,0,1};
  const double front_wall[]={-2,0,-2, 2,0,2, -1,1,1};
  const double back_wall[]={2,0,-2, -2,0,2, -1,1,1};
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection",projection,9);
  call_numbers(&vm,"d3d_set_hidden",disable,1);
  call_numbers(&vm,"d3d_set_culling",enable,1);
  call_numbers(&vm,"d3d_draw_wall",front_wall,9);
  int front_count=colored_pixels(pixels,WIDTH*HEIGHT);
  if(front_count<100){
    fprintf(stderr,"software D3 front face was culled\n");
    return 0;
  }

  memset(pixels,0,sizeof(pixels));
  call_numbers(&vm,"d3d_draw_wall",back_wall,9);
  if(colored_pixels(pixels,WIDTH*HEIGHT)!=0){
    fprintf(stderr,"software D3 back face was not culled\n");
    return 0;
  }

  const double far_wall[]={-2,4,-2, 2,4,2, -1,1,1};
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection",projection,9);
  call_numbers(&vm,"d3d_set_hidden",enable,1);
  call_numbers(&vm,"d3d_set_zwriteenable",disable,1);
  render.color=0x0000FFu;
  call_numbers(&vm,"d3d_draw_wall",front_wall,9);
  render.color=0x00FF00u;
  call_numbers(&vm,"d3d_draw_wall",far_wall,9);
  if((pixels[(HEIGHT/2)*WIDTH+WIDTH/2]&0x00FFFFFFu)!=0x00FF00u){
    fprintf(stderr,"software D3 disabled z-write mismatch\n");
    return 0;
  }
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection",projection,9);
  call_numbers(&vm,"d3d_set_hidden",enable,1);
  render.color=0x0000FFu;
  call_numbers(&vm,"d3d_draw_wall",front_wall,9);
  render.color=0x00FF00u;
  call_numbers(&vm,"d3d_draw_wall",far_wall,9);
  if((pixels[(HEIGHT/2)*WIDTH+WIDTH/2]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 enabled z-write mismatch\n");
    return 0;
  }

  const double projection_ext[]={0,-10,0, 0,0,0, 0,0,1, 45, (double)WIDTH/HEIGHT, 1,12};
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection_ext",projection_ext,13);
  render.color=0xFFFFFFu;
  call_numbers(&vm,"d3d_draw_wall",far_wall,9);
  if(colored_pixels(pixels,WIDTH*HEIGHT)!=0){
    fprintf(stderr,"software D3 far clipping mismatch\n");
    return 0;
  }
  call_numbers(&vm,"d3d_draw_wall",front_wall,9);
  if(colored_pixels(pixels,WIDTH*HEIGHT)<100){
    fprintf(stderr,"software D3 extended projection mismatch\n");
    return 0;
  }

  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection",projection,9);
  const double fog[]={1,0xFFFFFF,0,5};
  call_numbers(&vm,"d3d_set_fog",fog,4);
  render.color=0;
  call_numbers(&vm,"d3d_draw_wall",front_wall,9);
  if((pixels[(HEIGHT/2)*WIDTH+WIDTH/2]&0x00FFFFFFu)!=0xFFFFFFu){
    fprintf(stderr,"software D3 fog mismatch: pixel=%08x\n",pixels[(HEIGHT/2)*WIDTH+WIDTH/2]);
    return 0;
  }

  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection",projection,9);
  render.color=0xFFFFFFu;
  const double cone[]={-2,0,-2, 2,4,2, -1,1,1,1,12};
  call_numbers(&vm,"d3d_draw_cone",cone,11);
  if(colored_pixels(pixels,WIDTH*HEIGHT)<50){
    fprintf(stderr,"software D3 cone raster mismatch\n");
    return 0;
  }
  gml_d3_reset();
  gml_render_free(&render);
  return 1;
}

static int state_matches(const int expected_flags[GML_D3_STATE_FLAG_COUNT],
                         const double expected_values[GML_D3_STATE_VALUE_COUNT],
                         const uint32_t expected_colors[GML_D3_STATE_COLOR_COUNT]){
  int flags[GML_D3_STATE_FLAG_COUNT];
  double values[GML_D3_STATE_VALUE_COUNT];
  uint32_t colors[GML_D3_STATE_COLOR_COUNT];
  gml_d3_state_get(flags,values,colors);
  return !memcmp(flags,expected_flags,sizeof(flags)) &&
         !memcmp(values,expected_values,sizeof(values)) &&
         !memcmp(colors,expected_colors,sizeof(colors));
}

int main(void){
  int flags[GML_D3_STATE_FLAG_COUNT]={0};
  double values[GML_D3_STATE_VALUE_COUNT]={0};
  uint32_t colors[GML_D3_STATE_COLOR_COUNT]={0};
  flags[0]=1; flags[1]=1; flags[2]=1; flags[19]=1; flags[20]=1;
  for(int i=0;i<8;i++){
    flags[3+i*2]=1; flags[4+i*2]=(i&1)==0;
    colors[i]=0x010203u*(uint32_t)(i+1);
  }
  colors[8]=0x123456u;
  for(int i=0;i<GML_D3_STATE_VALUE_COUNT;i++) values[i]=(double)(i+1)*1.25;
  values[44]=-13.5; values[45]=27.25; values[46]=640; values[47]=360; values[48]=33;
  gml_d3_state_set(flags,values,colors);
  if(!state_matches(flags,values,colors)){
    fprintf(stderr,"software D3 direct state mismatch\n");
    return 1;
  }

  GmlWin win; GmlVM vm;
  memset(&win,0,sizeof(win)); memset(&vm,0,sizeof(vm)); vm.win=&win;
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  void *state=malloc(size);
  if(!state || !gml_vm_state_save(&vm,state,size,&written) || written!=size){
    fprintf(stderr,"software D3 state save failed\n");
    free(state); return 1;
  }
  gml_d3_reset();
  if(state_matches(flags,values,colors)){
    fprintf(stderr,"software D3 reset did not clear state\n");
    free(state); return 1;
  }
  if(!gml_vm_state_load(&vm,state,written,&used) || used!=written ||
     !state_matches(flags,values,colors)){
    fprintf(stderr,"software D3 savestate roundtrip mismatch\n");
    free(state); return 1;
  }
  free(state);
  if(!raster_fixtures()) return 1;
  puts("software D3 state fixtures: ok");
  return 0;
}
