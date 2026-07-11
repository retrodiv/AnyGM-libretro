/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gml_render.h"

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
  vm.render=&render;

  gml_d3_reset();
  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  const double ortho[]={0,0,WIDTH,HEIGHT,0};
  const double enable[]={1};
  const double floor_args[]={8,6,0,24,18,0,-1,1,1};
  call_numbers(&vm,"d3d_set_projection_ortho",ortho,5);
  call_numbers(&vm,"d3d_set_culling",enable,1);
  call_numbers(&vm,"d3d_draw_floor",floor_args,9);
  if((pixels[10*WIDTH+10]&0x00FFFFFFu)==0 || pixels[4*WIDTH+4]!=0 ||
     colored_pixels(pixels,WIDTH*HEIGHT)<150){
    fprintf(stderr,"software D3 orthographic raster mismatch\n");
    return 0;
  }

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

  const double projection[]={0,-10,0, 0,0,0, 0,0,1};
  const double front_wall[]={-2,0,-2, 2,0,2, -1,1,1};
  const double back_wall[]={2,0,-2, -2,0,2, -1,1,1};
  gml_d3_reset(); memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  call_numbers(&vm,"d3d_start",NULL,0);
  call_numbers(&vm,"d3d_set_projection",projection,9);
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

  const double disable[]={0};
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
