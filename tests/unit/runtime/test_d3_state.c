/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gml_render.h"

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
