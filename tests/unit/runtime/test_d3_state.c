/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gml_vm.h"
#include "gml_render.h"
#include "gml_particle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fixture_key=-1;
static int fixture_key_edge=-1;
int gml_input_key(int key, int edge){ return key==fixture_key && edge==fixture_key_edge; }
int gml_input_gamepad(int button, int edge){ (void)button; (void)edge; return 0; }
GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);
int gml_builtin_fast_id(const char *name);

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

static void free_extension_fixture(GmlVM *vm){
  if(!vm) return;
  gml_colgrid_invalidate(vm);
  free(vm->cg_off);
  free(vm->cg_items);
  free(vm->cg_overlay);
  free(vm->inst);
  vm->cg_off=NULL; vm->cg_items=NULL; vm->cg_overlay=NULL; vm->inst=NULL;
}

static void store_u32le(uint8_t *dst,uint32_t value){
  dst[0]=(uint8_t)value;
  dst[1]=(uint8_t)(value>>8);
  dst[2]=(uint8_t)(value>>16);
  dst[3]=(uint8_t)(value>>24);
}

static size_t classic_information_record(uint8_t *dst,size_t capacity){
  const char caption[]="Information";
  const char text[]="{\\rtf1\\ansi\\pard\\qc\\b\\fs32 Generic information\\par\\b0\\fs24 Neutral \\ul underlined\\ulnone  fixture text\\par}";
  size_t need=8+4+sizeof(caption)-1+8*4+8+4+sizeof(text)-1;
  if(!dst || capacity<need) return 0;
  size_t at=0;
#define INFO_U32(v) do{ store_u32le(dst+at,(uint32_t)(v)); at+=4; }while(0)
  INFO_U32(0xFF000018u); INFO_U32(1);
  INFO_U32(sizeof(caption)-1); memcpy(dst+at,caption,sizeof(caption)-1); at+=sizeof(caption)-1;
  INFO_U32((uint32_t)-1); INFO_U32((uint32_t)-1); INFO_U32(600); INFO_U32(400);
  INFO_U32(1); INFO_U32(1); INFO_U32(0); INFO_U32(1);
  memset(dst+at,0,8); at+=8;
  INFO_U32(sizeof(text)-1); memcpy(dst+at,text,sizeof(text)-1); at+=sizeof(text)-1;
#undef INFO_U32
  return at;
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
    enum { INFO_W=320, INFO_H=180 };
    static uint32_t information_pixels[INFO_W*INFO_H];
    uint8_t information[256];
    size_t information_size=classic_information_record(information,sizeof(information));
    GmlWin win={0}; GmlRender initialized;
    win.classic_version=800;
    if(!information_size || gml_render_init(&initialized,&win)!=0){
      fprintf(stderr,"classic information fixture initialization failed\n");
      return 0;
    }
    initialized.color=0x123456u; initialized.alpha=0.375;
    initialized.halign=2; initialized.valign=2; initialized.font=7;
    initialized.alphablend=0;
    memset(information_pixels,0,sizeof(information_pixels));
    gml_draw_classic_game_information(&initialized,information_pixels,INFO_W,INFO_H,
                                      information,information_size);
    int ink=0;
    for(int i=0;i<INFO_W*INFO_H;i++)
      if(information_pixels[i]!=0xFFFFFFu && information_pixels[i]!=0xAEAEAEu) ink++;
    if(information_pixels[0]!=0xAEAEAEu ||
       information_pixels[INFO_W*INFO_H-1]!=0xAEAEAEu ||
       information_pixels[INFO_W+1]!=0xFFFFFFu || ink<20 ||
       initialized.color!=0x123456u || initialized.alpha!=0.375 ||
       initialized.halign!=2 || initialized.valign!=2 || initialized.font!=7 ||
       initialized.alphablend!=0){
      fprintf(stderr,"classic information software render mismatch: edge=%06x,%06x inside=%06x ink=%d state=%06x,%.3f,%d,%d,%d,%d font=%d atlas=%d\n",
              information_pixels[0]&0xFFFFFFu,
              information_pixels[INFO_W*INFO_H-1]&0xFFFFFFu,
              information_pixels[INFO_W+1]&0xFFFFFFu,ink,
              initialized.color&0xFFFFFFu,initialized.alpha,initialized.halign,
              initialized.valign,initialized.font,initialized.alphablend,
              initialized.default_font.n_glyphs,initialized.n_atlas);
      gml_render_free(&initialized);
      return 0;
    }
    information_pixels[0]=0x123456u;
    gml_draw_classic_game_information(&initialized,information_pixels,INFO_W,INFO_H,
                                      information,11);
    if(information_pixels[0]!=0x123456u){
      fprintf(stderr,"invalid classic information record was rendered\n");
      gml_render_free(&initialized);
      return 0;
    }
    gml_render_free(&initialized);

    GmlVM modal={0};
    win.classic_game_information=information;
    win.classic_game_information_size=information_size;
    modal.win=&win;
    (void)call_values(&modal,"show_info",NULL,0);
    if(!modal.classic_info_active){
      fprintf(stderr,"classic information modal did not activate\n");
      return 0;
    }
    fixture_key=1; fixture_key_edge=1;
    gml_vm_step(&modal);
    fixture_key=fixture_key_edge=-1;
    if(modal.classic_info_active){
      fprintf(stderr,"classic information modal did not dismiss\n");
      return 0;
    }
    (void)call_values(&modal,"action_show_info",NULL,0);
    if(!modal.classic_info_active){
      fprintf(stderr,"classic information action did not activate\n");
      return 0;
    }
    modal.classic_info_active=0; win.classic_version=0;
    (void)call_values(&modal,"show_info",NULL,0);
    if(modal.classic_info_active){
      fprintf(stderr,"classic information leaked into modern content\n");
      return 0;
    }
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
    {
      uint32_t phase_x[4]={0xFF00FF00u,0xFF008800u,0xFF004400u,0xFF002200u};
      uint32_t phase_y[4]={0xFFFFFF00u,0xFF888800u,0xFF444400u,0xFF222200u};
      uint32_t phase_xy[4]={0xFF00FFFFu,0xFF008888u,0xFF004444u,0xFF002222u};
      const uint32_t expected[16]={
        source[0],phase_x[0],source[1],phase_x[1],
        phase_y[0],phase_xy[0],phase_y[1],phase_xy[1],
        source[2],phase_x[2],source[3],phase_x[3],
        phase_y[2],phase_xy[2],phase_y[3],phase_xy[3]
      };
      memset(target,0,sizeof(target));
      render.app_interp_phase[0]=phase_x;
      render.app_interp_phase[1]=phase_y;
      render.app_interp_phase[2]=phase_xy;
      gml_render_begin(&render,target,4,4,0,0);
      gml_render_set_pending_underlay(&render,0,0,4,4);
      gml_render_flush_pending_underlay(&render);
      if(memcmp(target,expected,sizeof(expected))!=0){
        fprintf(stderr,"classic interpolated phase-plane presentation mismatch\n");
        return 0;
      }
      memset(phase_x,0,sizeof(phase_x));
      memset(phase_y,0,sizeof(phase_y));
      memset(phase_xy,0,sizeof(phase_xy));
      gml_render_begin(&render,target,2,2,0,0);
      render.classic_interp_phase[0]=phase_x;
      render.classic_interp_phase[1]=phase_y;
      render.classic_interp_phase[2]=phase_xy;
      gml_render_set_pending_fill(&render,0xFF123456u);
      for(int i=0;i<4;i++) if(phase_x[i]!=0xFF123456u ||
                                phase_y[i]!=0xFF123456u ||
                                phase_xy[i]!=0xFF123456u){
        fprintf(stderr,"classic interpolated phase-plane clear mismatch\n");
        return 0;
      }
      render.classic_interp_phase[0]=NULL;
      render.classic_interp_phase[1]=NULL;
      render.classic_interp_phase[2]=NULL;
      render.app_interp_phase[0]=NULL;
      render.app_interp_phase[1]=NULL;
      render.app_interp_phase[2]=NULL;
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

  {
    int visible[8]={0}, xport[8]={0}, yport[8]={0};
    int wport[8]={0}, hport[8]={0};
    int w=0,h=0;
    gml_classic_room_window_size(550,400,-1,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=550 || h!=400){
      fprintf(stderr,"viewless classic room window mismatch\n");
      return 0;
    }
    visible[0]=1; wport[0]=320; hport[0]=240;
    gml_classic_room_window_size(800,600,-1,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=320 || h!=240){
      fprintf(stderr,"single-port classic room window mismatch\n");
      return 0;
    }
    visible[1]=1; xport[1]=320; yport[1]=240; wport[1]=320; hport[1]=240;
    gml_classic_room_window_size(800,600,-1,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=640 || h!=480){
      fprintf(stderr,"multi-port classic room window mismatch\n");
      return 0;
    }
    gml_classic_room_window_size(320,240,200,640,480,
                                 visible,xport,yport,wport,hport,&w,&h);
    if(w!=640 || h!=480){
      fprintf(stderr,"fixed-scale classic window mismatch\n");
      return 0;
    }
  }

  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  gml_part_reset_all(); gml_part_bind_vm(&vm);
  gml_effect_create(1,10,0,0,0,0xFFFF00);
  for(int i=0;i<5;i++) gml_part_update_all();
  gml_part_system_draw_all(&render);
  int rain_pixels=0,rain_far_pixels=0,rain_right_pixels=0,rain_alpha_pixels=0;
  for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++) if(pixels[y*WIDTH+x]&0x00FFFFFFu){
    rain_pixels++; if(x>8) rain_far_pixels++;
    if(x>56) rain_right_pixels++;
    if(pixels[y*WIDTH+x]>>24) rain_alpha_pixels++;
  }
  if(rain_pixels<4 || rain_far_pixels<4 || rain_right_pixels<4 || rain_alpha_pixels!=rain_pixels){
    fprintf(stderr,"software rain effect distribution mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  {
    int system=gml_part_system_create();
    int type=gml_part_type_create();
    gml_part_type_shape(type,5);
    gml_part_type_size(type,.5,.5,0,0);
    gml_part_type_alpha(type,1,1,1,1);
    gml_part_particles_create_color(system,32,24,type,0x0000FF,1);
    gml_part_system_draw_all(&render);
    int ring_pixels=0,ring_mass=0,minx=WIDTH,maxx=-1,miny=HEIGHT,maxy=-1;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++){
      int red=(pixels[y*WIDTH+x]>>16)&0xFF;
      if(red){
        ring_pixels++; ring_mass+=red;
        if(x<minx)minx=x; if(x>maxx)maxx=x;
        if(y<miny)miny=y; if(y>maxy)maxy=y;
      }
    }
    if(ring_pixels!=264 || ring_mass!=25004 ||
       minx!=18 || maxx!=45 || miny!=10 || maxy!=37){
      fprintf(stderr,"classic filtered particle ring mismatch: pixels=%d mass=%d span=(%d,%d)-(%d,%d)\n",
              ring_pixels,ring_mass,minx,miny,maxx,maxy);
      return 0;
    }
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
    int line_pixels=0,minx=WIDTH,maxx=-1,miny=HEIGHT,maxy=-1;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++)
      if(pixels[y*WIDTH+x]&0x00FFFFFFu){
        line_pixels++;
        if(x<minx)minx=x; if(x>maxx)maxx=x;
        if(y<miny)miny=y; if(y>maxy)maxy=y;
      }
    int edge_red=(pixels[24*WIDTH+4]>>16)&0xFF;
    int core_red=(pixels[24*WIDTH+8]>>16)&0xFF;
    int top_red=(pixels[19*WIDTH+32]>>16)&0xFF;
    if(line_pixels!=560 || minx!=4 || maxx!=59 || miny!=19 || maxy!=28 ||
       edge_red<50 || edge_red>52 || core_red!=255 || top_red<76 || top_red>78){
      fprintf(stderr,"classic particle line cell geometry mismatch\n");
      return 0;
    }
  }
  gml_part_reset_all();

  memset(pixels,0,sizeof(pixels));
  gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  {
    int system=gml_part_system_create();
    int type=gml_part_type_create();
    gml_part_type_shape(type,3);
    gml_part_type_size(type,.2,.2,0,0);
    gml_part_type_orientation(type,260,260,0,0,0);
    gml_part_type_alpha(type,1,1,1,1);
    gml_part_particles_create_color(system,32,24,type,0x0000FF,1);
    gml_part_system_draw_all(&render);
    int line_pixels=0,minx=WIDTH,maxx=-1,miny=HEIGHT,maxy=-1;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++)
      if(pixels[y*WIDTH+x]&0x00FFFFFFu){
        line_pixels++;
        if(x<minx)minx=x; if(x>maxx)maxx=x;
        if(y<miny)miny=y; if(y>maxy)maxy=y;
      }
    if(line_pixels!=23 || minx!=31 || maxx!=34 || miny!=18 || maxy!=29){
      fprintf(stderr,"classic small rotated particle line geometry mismatch\n");
      return 0;
    }
  }
  gml_part_reset_all();

  {
    static const int shapes[3]={4,8,9};
    static const int min_pixels[3]={180,220,60};
    for(int k=0;k<3;k++){
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF202020u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      int system=gml_part_system_create();
      int type=gml_part_type_create();
      gml_part_type_shape(type,shapes[k]);
      gml_part_type_size(type,.5,.5,0,0);
      gml_part_type_orientation(type,0,0,0,0,0);
      gml_part_type_alpha(type,1,1,1,1);
      gml_part_particles_create_color(system,32,24,type,0x40A0FF,1);
      gml_part_system_draw_all(&render);
      int changed=0,minx=WIDTH,maxx=-1,miny=HEIGHT,maxy=-1;
      for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++)
        if(pixels[y*WIDTH+x]!=0xFF202020u){
          changed++;
          if(x<minx)minx=x;
          if(x>maxx)maxx=x;
          if(y<miny)miny=y;
          if(y>maxy)maxy=y;
        }
      if(changed<min_pixels[k] || maxx-minx<18 || maxy-miny<18){
        fprintf(stderr,"classic glint shape coverage mismatch: shape=%d pixels=%d span=%dx%d\n",
                shapes[k],changed,maxx-minx+1,maxy-miny+1);
        return 0;
      }
      gml_part_reset_all();
    }
  }

  {
    static const double scales[3]={.10,.15,.20};
    static const int min_pixels[3]={28,65,113};
    static const int max_pixels[3]={36,77,129};
    for(int k=0;k<3;k++){
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF202020u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      int system=gml_part_system_create();
      int type=gml_part_type_create();
      gml_part_type_shape(type,8);
      gml_part_type_size(type,scales[k],scales[k],0,0);
      gml_part_type_orientation(type,0,0,0,0,0);
      gml_part_type_alpha(type,1,1,1,1);
      gml_part_particles_create_color(system,32,24,type,0x40FFA0,1);
      gml_part_system_draw_all(&render);
      int changed=0;
      for(int i=0;i<WIDTH*HEIGHT;i++) if(pixels[i]!=0xFF202020u) changed++;
      if(changed<min_pixels[k] || changed>max_pixels[k]){
        fprintf(stderr,"classic small flare footprint mismatch: scale=%.2f pixels=%d expected=%d..%d\n",
                scales[k],changed,min_pixels[k],max_pixels[k]);
        return 0;
      }
      gml_part_reset_all();
    }
  }

  {
    static const double scales[3]={.5,.5,.2};
    static const double angles[3]={0,30,0};
    static const int min_pixels[3]={680,670,100};
    static const int max_pixels[3]={730,720,120};
    static const int min_span_x[3]={28,30,10};
    static const int min_span_y[3]={30,28,10};
    for(int k=0;k<3;k++){
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF504030u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      int system=gml_part_system_create();
      int type=gml_part_type_create();
      gml_part_type_shape(type,13);
      gml_part_type_size(type,scales[k],scales[k],0,0);
      gml_part_type_orientation(type,angles[k],angles[k],0,0,0);
      gml_part_type_alpha(type,1,.6,.6,.6);
      gml_part_particles_create_color(system,32,24,type,0xFFFFFF,1);
      gml_part_system_draw_all(&render);
      int changed=0,minx=WIDTH,maxx=-1,miny=HEIGHT,maxy=-1;
      for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++)
        if(pixels[y*WIDTH+x]!=0xFF504030u){
          changed++;
          if(x<minx)minx=x;
          if(x>maxx)maxx=x;
          if(y<miny)miny=y;
          if(y>maxy)maxy=y;
        }
      if(changed<min_pixels[k] || changed>max_pixels[k] ||
         maxx-minx+1<min_span_x[k] || maxy-miny+1<min_span_y[k]){
        fprintf(stderr,"classic six-lobed particle footprint mismatch: scale=%.2f angle=%.0f pixels=%d span=%dx%d\n",
                scales[k],angles[k],changed,maxx-minx+1,maxy-miny+1);
        return 0;
      }
      gml_part_reset_all();
    }
  }

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
  for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFFB0B0B0u;
  gml_part_update_all();
  gml_part_system_draw_all(&render);
  int explosion_pixels=0,explosion_minx=WIDTH,explosion_maxx=-1;
  int explosion_miny=HEIGHT,explosion_maxy=-1;
  for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++)
    if(pixels[y*WIDTH+x]!=0xFFB0B0B0u){
      explosion_pixels++;
      if(x<explosion_minx) explosion_minx=x;
      if(x>explosion_maxx) explosion_maxx=x;
      if(y<explosion_miny) explosion_miny=y;
      if(y>explosion_maxy) explosion_maxy=y;
    }
  if(explosion_pixels<80 || explosion_maxx-explosion_minx<10 || explosion_maxy-explosion_miny<10){
    fprintf(stderr,"classic explosion shape coverage mismatch: pixels=%d span=%dx%d\n",
            explosion_pixels,explosion_maxx-explosion_minx+1,explosion_maxy-explosion_miny+1);
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

  gml_effect_create(1,7,32,24,0,0x40A0FF);
  gml_effect_create(1,8,32,24,0,0x40A0FF);
  if(gml_part_system_count(1)!=2){
    fprintf(stderr,"shrinking glint particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_effect_create(1,4,32,24,0,0x808080);
  if(gml_part_system_count(1)!=6){
    fprintf(stderr,"small smoke particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_effect_create(1,5,32,24,1,0x808080);
  if(gml_part_system_count(1)!=11){
    fprintf(stderr,"medium rising smoke particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_effect_create(1,9,32,24,2,0xFFFFFF);
  if(gml_part_system_count(1)!=1){
    fprintf(stderr,"large cloud particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  gml_effect_create(1,11,32,24,2,0xFFFFFF);
  if(gml_part_system_count(1)!=7){
    fprintf(stderr,"large snowfall particle count mismatch\n");
    return 0;
  }
  gml_part_reset_all();

  {
    gml_d3_reset();
    memset(pixels,0,sizeof(pixels));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    int system=gml_part_system_create();
    int type=gml_part_type_create();
    gml_part_type_size(type,1,1,0,0);
    gml_part_type_speed(type,8,8,0,0);
    gml_part_type_direction(type,0,0,0,90);
    gml_part_particles_create(system,20,20,type,1);
    size_t particle_state_size=gml_part_state_size(),particle_written=0,particle_used=0;
    void *particle_state=malloc(particle_state_size);
    if(!particle_state || !gml_part_state_save(particle_state,particle_state_size,&particle_written) ||
       particle_written!=particle_state_size){
      fprintf(stderr,"particle wiggle phase state save failed\n");
      free(particle_state);
      return 0;
    }
    gml_part_update_all();
    gml_part_system_draw_all(&render);
    uint32_t expected_pixels[WIDTH*HEIGHT];
    memcpy(expected_pixels,pixels,sizeof(expected_pixels));
    if(colored_pixels(pixels,WIDTH*HEIGHT)<1 || pixels[20*WIDTH+28]!=0){
      int first=-1;
      for(int i=0;i<WIDTH*HEIGHT;i++) if(pixels[i]){ first=i; break; }
      fprintf(stderr,"particle direction wiggle phase mismatch: first=(%d,%d) count=%d straight=%08x\n",
              first<0?-1:first%WIDTH,first<0?-1:first/WIDTH,
              colored_pixels(pixels,WIDTH*HEIGHT),pixels[20*WIDTH+28]);
      free(particle_state);
      return 0;
    }
    if(!gml_part_state_load(particle_state,particle_written,&particle_used) ||
       particle_used!=particle_written){
      fprintf(stderr,"particle wiggle phase state load failed\n");
      free(particle_state);
      return 0;
    }
    free(particle_state);
    memset(pixels,0,sizeof(pixels));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    gml_part_update_all();
    gml_part_system_draw_all(&render);
    if(memcmp(pixels,expected_pixels,sizeof(expected_pixels))){
      fprintf(stderr,"particle wiggle phase state roundtrip mismatch\n");
      return 0;
    }
    gml_part_reset_all();
  }

  {
    memset(pixels,0,sizeof(pixels));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    gml_part_reset_all();
    gml_effect_create(1,3,32,24,0,0x0080FF);
    for(int i=0;i<10;i++) gml_part_update_all();
    size_t particle_state_size=gml_part_state_size(),particle_written=0,particle_used=0;
    void *particle_state=malloc(particle_state_size);
    if(!particle_state || !gml_part_state_save(particle_state,particle_state_size,&particle_written) ||
       particle_written!=particle_state_size){
      fprintf(stderr,"built-in effect identity state save failed\n");
      free(particle_state);
      return 0;
    }
    gml_effect_create(1,3,32,24,0,0xFF8000);
    for(int i=0;i<12;i++) gml_part_update_all();
    gml_part_system_draw_all(&render);
    uint32_t expected_pixels[WIDTH*HEIGHT];
    memcpy(expected_pixels,pixels,sizeof(expected_pixels));
    if(!gml_part_state_load(particle_state,particle_written,&particle_used) ||
       particle_used!=particle_written){
      fprintf(stderr,"built-in effect identity state load failed\n");
      free(particle_state);
      return 0;
    }
    free(particle_state);
    memset(pixels,0,sizeof(pixels));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    gml_effect_create(1,3,32,24,0,0xFF8000);
    for(int i=0;i<12;i++) gml_part_update_all();
    gml_part_system_draw_all(&render);
    if(memcmp(pixels,expected_pixels,sizeof(expected_pixels))){
      fprintf(stderr,"built-in effect identity state roundtrip mismatch\n");
      return 0;
    }
    gml_part_reset_all();
  }

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
  {
    /* Classic fixed-function filtering uses rounded eight-bit fractions and two rounded lerps.
     * The blue component also proves an implicit white vertex colour is not reduced to 254. */
    GmlWin classic_win={0}; classic_win.classic_version=800; vm.win=&classic_win;
    surface_data->px[0]=0xFF0A141Eu; surface_data->px[1]=0xFF6E7882u;
    surface_data->px[2]=0xFFD2DCE6u; surface_data->px[3]=0xFFFAF0C8u;
    render.color=0xFFFFFFu;
    const double classic_filtered_point[]={30,20,0,.72,.28};
    memset(pixels,0,sizeof(pixels));
    call_numbers(&vm,"d3d_primitive_begin_texture",interpolated_begin,2);
    call_numbers(&vm,"d3d_vertex_texture",classic_filtered_point,5);
    call_numbers(&vm,"d3d_primitive_end",NULL,0);
    vm.win=NULL;
    if((pixels[21*WIDTH+31]&0x00FFFFFFu)!=0x707981u){
      int first=-1;
      for(int i=0;i<WIDTH*HEIGHT;i++) if(pixels[i]&0x00FFFFFFu){ first=i; break; }
      fprintf(stderr,"software classic fixed-function filter mismatch: %06x first=(%d,%d) %06x\n",
        pixels[21*WIDTH+31]&0x00FFFFFFu,first<0?-1:first%WIDTH,first<0?-1:first/WIDTH,
        first<0?0:pixels[first]&0x00FFFFFFu);
      return 0;
    }
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
    const double rectangle[]={10,10,10,10,0};
    render.classic=1; render.color=0x0000FFu; render.alpha=.4;
    memset(pixels,0,sizeof(pixels));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    pixels[10*WIDTH+10]=0xFFE9D6BDu;
    call_numbers(&vm,"draw_rectangle",rectangle,5);
    if(pixels[10*WIDTH+10]!=0xFFF28071u){
      fprintf(stderr,"software classic primitive alpha rounding mismatch: %08x\n",pixels[10*WIDTH+10]);
      return 0;
    }
    render.alpha=.5;
    pixels[10*WIDTH+10]=0xFFEAD5BDu;
    call_numbers(&vm,"draw_rectangle",rectangle,5);
    if(pixels[10*WIDTH+10]!=0xFFF46A5Eu){
      fprintf(stderr,"software classic primitive fixed-alpha tie mismatch: %08x\n",pixels[10*WIDTH+10]);
      return 0;
    }
    render.classic=0;
    render.alpha=.4;
    memset(pixels,0,sizeof(pixels));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    pixels[10*WIDTH+10]=0xFFE9D6BDu;
    call_numbers(&vm,"draw_rectangle",rectangle,5);
    if(pixels[10*WIDTH+10]!=0xFFF18071u){
      fprintf(stderr,"software modern primitive alpha rounding changed: %08x\n",pixels[10*WIDTH+10]);
      return 0;
    }
    {
      const double outline_rectangle[]={8,8,12,12,1};
      render.classic=1; render.color=0x0000FFu; render.alpha=1;
      memset(pixels,0,sizeof(pixels));
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      gml_render_set_pending_fill(&render,0xFF123456u);
      call_numbers(&vm,"draw_rectangle",outline_rectangle,5);
      gml_render_flush_pending_fill(&render);
      if(pixels[8*WIDTH+8]!=0xFFFF0000u || pixels[10*WIDTH+10]!=0xFF123456u){
        fprintf(stderr,"software outlined primitive deferred-clear ordering mismatch: edge=%08x centre=%08x\n",
          pixels[8*WIDTH+8],pixels[10*WIDTH+10]);
        return 0;
      }
    }
    render.color=0xFFFFFFu; render.alpha=1;
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

    uint8_t *animated_rgba=malloc(4*4);
    if(!animated_rgba){
      fprintf(stderr,"software action sprite fixture allocation mismatch\n");
      return 0;
    }
    memset(animated_rgba,255,4*4);
    int animated_sprite=gml_sprite_append_from_rgba_frames(
      &render,animated_rgba,1,1,4,0,0,"<action-animation>");
    if(animated_sprite<0){
      fprintf(stderr,"software action sprite fixture creation mismatch\n");
      return 0;
    }
    relative_self.sprite_index=runtime_sprite;
    relative_self.image_index=2.25;
    relative_self.image_speed=0;
    GmlVal keep_frame[3]={vreal(animated_sprite),vreal(-1),vreal(.5)};
    (void)call_values(&vm,"action_sprite_set",keep_frame,3);
    if(relative_self.sprite_index!=animated_sprite || relative_self.image_index!=2.25 ||
       relative_self.image_speed!=.5){
      fprintf(stderr,"software action sprite retained-frame mismatch\n");
      return 0;
    }
    relative_self.image_index=8.25;
    (void)call_values(&vm,"action_sprite_set",keep_frame,3);
    if(relative_self.image_index!=0){
      fprintf(stderr,"software action sprite out-of-range reset mismatch\n");
      return 0;
    }
    GmlVal select_frame[3]={vreal(animated_sprite),vreal(3),vreal(0)};
    (void)call_values(&vm,"action_sprite_set",select_frame,3);
    if(relative_self.image_index!=3 || relative_self.image_speed!=0){
      fprintf(stderr,"software action sprite explicit-frame mismatch\n");
      return 0;
    }
    vm.cur_self=NULL;
  }
  for(int i=0;i<4;i++) surface_data->px[i]=0xFFFFFFFFu;
  int depth_sprite=gml_sprite_create_from_surface(&render,surface,0,0,2,2,0,0,0,0);
  {
    uint8_t room_data[128]={0};
    GmlWin draw_win={0}; GmlVM draw_vm={0};
    GmlObject draw_object={0}; GmlInstance draw_instance={0};
    store_u32le(room_data,1);
    store_u32le(room_data+4,16);
    store_u32le(room_data+16+8,WIDTH);
    store_u32le(room_data+16+12,HEIGHT);
    store_u32le(room_data+16+16,30);
    draw_win.data=room_data; draw_win.size=sizeof(room_data);
    draw_win.n_chunks=1; memcpy(draw_win.chunks[0].name,"ROOM",5);
    draw_win.chunks[0].off=0; draw_win.chunks[0].size=sizeof(room_data);
    draw_win.classic_version=800;
    draw_object.name="neutral_default_draw"; draw_object.parent=-1;
    draw_vm.win=&draw_win; draw_vm.render=&render; draw_vm.room_index=0;
    draw_vm.objects=&draw_object; draw_vm.n_objects=1;
    draw_vm.inst=&draw_instance; draw_vm.inst_count=draw_vm.inst_cap=1;
    draw_instance.active=1; draw_instance.obj=0; draw_instance.visible=1;
    draw_instance.sprite_index=depth_sprite; draw_instance.mask_index=-1;
    draw_instance.x=4; draw_instance.y=4;
    draw_instance.image_xscale=draw_instance.image_yscale=1;
    draw_instance.image_alpha=.75; draw_instance.image_blend=0xFFFFFF;
    draw_instance.draw_layer_order=-1;
    for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
    render.classic=1;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    render.alpha=.25;
    gml_vm_draw(&draw_vm);
    unsigned automatic=(pixels[4*WIDTH+4]>>16)&255u;
    for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    render.alpha=.25;
    draw_vm.cur_self=&draw_instance;
    (void)call_values(&draw_vm,"draw_self",NULL,0);
    draw_vm.cur_self=NULL;
    unsigned self_draw=(pixels[4*WIDTH+4]>>16)&255u;
    for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    render.alpha=.25;
    draw_vm.cur_self=&draw_instance;
    (void)call_values(&draw_vm,"draw_full_sprite",NULL,0);
    draw_vm.cur_self=NULL;
    unsigned full_sprite=(pixels[4*WIDTH+4]>>16)&255u;
    for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    render.alpha=.25;
    gml_draw_sprite(&render,depth_sprite,0,4,4);
    unsigned classic_basic=(pixels[4*WIDTH+4]>>16)&255u;
    for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    gml_draw_sprite_ext(&render,depth_sprite,0,4,4,1,1,0,0xFFFFFF,.25);
    unsigned explicit_alpha=(pixels[4*WIDTH+4]>>16)&255u;
    for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
    render.classic=0;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    render.alpha=.25;
    gml_draw_sprite(&render,depth_sprite,0,4,4);
    unsigned modern_basic=(pixels[4*WIDTH+4]>>16)&255u;
    if(automatic<185 || automatic>195 || self_draw!=automatic || full_sprite!=255 ||
       gml_builtin_fast_id("draw_full_sprite")!=gml_builtin_fast_id("draw_self") || classic_basic!=255 ||
       explicit_alpha<55 || explicit_alpha>70 || modern_basic<55 || modern_basic>70){
      fprintf(stderr,"default/self/full/basic/explicit draw alpha isolation mismatch: automatic=%u self=%u full=%u classic=%u explicit=%u modern=%u\n",
        automatic,self_draw,full_sprite,classic_basic,explicit_alpha,modern_basic);
      return 0;
    }
    {
      uint32_t shadow_pixels[WIDTH*HEIGHT];
      const double simple_geometry[]={3,4,5,9,0x404040,0x404040,0};
      const double extended_geometry[]={3,4,5,7,0x404040,0x404040,0};
      GmlVal extended_args[2]={vreal(2),vreal(.25)};
      GmlVal invalid_arg=vreal(9);
      render.classic=1;
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      render.alpha=.9; draw_vm.cur_self=&draw_instance;
      (void)call_values(&draw_vm,"draw_shadow",NULL,0);
      draw_vm.cur_self=NULL;
      memcpy(shadow_pixels,pixels,sizeof(shadow_pixels));
      if(render.alpha!=1){
        fprintf(stderr,"legacy simple shadow alpha reset mismatch: %.6f\n",render.alpha);
        return 0;
      }
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      render.alpha=.5;
      call_numbers(&draw_vm,"draw_ellipse_color",simple_geometry,7);
      if(memcmp(shadow_pixels,pixels,sizeof(shadow_pixels))){
        fprintf(stderr,"legacy simple shadow geometry mismatch\n");
        return 0;
      }
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      render.alpha=.8; draw_vm.cur_self=&draw_instance;
      (void)call_values(&draw_vm,"draw_shadow_ext",extended_args,2);
      draw_vm.cur_self=NULL;
      memcpy(shadow_pixels,pixels,sizeof(shadow_pixels));
      if(render.alpha!=1){
        fprintf(stderr,"legacy extended shadow alpha reset mismatch: %.6f\n",render.alpha);
        return 0;
      }
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      render.alpha=.25;
      call_numbers(&draw_vm,"draw_ellipse_color",extended_geometry,7);
      if(memcmp(shadow_pixels,pixels,sizeof(shadow_pixels)) ||
         gml_builtin_fast_id("draw_shadow")<0 ||
         gml_builtin_fast_id("draw_shadow")!=gml_builtin_fast_id("draw_shadow_ext")){
        fprintf(stderr,"legacy extended shadow geometry/dispatch mismatch\n");
        return 0;
      }
      for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF000000u;
      gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
      render.alpha=.3; draw_vm.cur_self=&draw_instance;
      (void)call_values(&draw_vm,"draw_shadow",&invalid_arg,1);
      draw_vm.cur_self=NULL;
      if(render.alpha!=.3 || colored_pixels(pixels,WIDTH*HEIGHT)!=0){
        fprintf(stderr,"legacy shadow invalid-arity guard mismatch\n");
        return 0;
      }
    }
    render.classic=0;
    render.alpha=1;
  }
  {
    GmlVM extension_vm={0};
    GmlWin extension_win={0};
    GmlObject extension_object={0};
    GmlVal create_args[3]={vreal(12.5),vreal(34.5),vreal(0)};
    GmlVal invalid_arg=vreal(0);
    extension_object.name="neutral_extension_object";
    extension_object.parent=-1;
    extension_object.sprite_index=extension_object.mask_index=-1;
    extension_object.visible=1;
    extension_vm.win=&extension_win;
    extension_vm.objects=&extension_object;
    extension_vm.n_objects=1;
    extension_vm.inst=calloc(4,sizeof(*extension_vm.inst));
    extension_vm.inst_cap=4;
    extension_vm.next_id=100001;
    if(!extension_vm.inst){
      fprintf(stderr,"legacy extension instance fixture allocation mismatch\n");
      return 0;
    }
    GmlVal create_result=call_values(&extension_vm,"crear",create_args,3);
    if(create_result.d!=0 || extension_vm.inst_count!=1 ||
       extension_vm.inst[0].x!=12.5 || extension_vm.inst[0].y!=34.5 ||
       extension_vm.inst[0].obj!=0 ||
       gml_builtin_fast_id("crear")<0){
      fprintf(stderr,"legacy extension instance-create alias mismatch\n");
      free(extension_vm.inst);
      return 0;
    }
    (void)call_values(&extension_vm,"crear",&invalid_arg,1);
    if(extension_vm.inst_count!=1){
      fprintf(stderr,"legacy extension instance-create arity mismatch\n");
      free(extension_vm.inst);
      return 0;
    }
    extension_vm.cur_self=&extension_vm.inst[0];
    extension_vm.cur_self->y=250;
    extension_vm.cur_self->depth=77;
    (void)call_values(&extension_vm,"depthy",NULL,0);
    if(extension_vm.cur_self->depth!=-2.5 || gml_builtin_fast_id("depthy")<0){
      fprintf(stderr,"legacy extension depth-by-y mismatch: %.6f\n",extension_vm.cur_self->depth);
      free(extension_vm.inst);
      return 0;
    }
    extension_vm.cur_self->depth=19;
    (void)call_values(&extension_vm,"depthy",&invalid_arg,1);
    if(extension_vm.cur_self->depth!=19){
      fprintf(stderr,"legacy extension depth-by-y arity mismatch\n");
      free(extension_vm.inst);
      return 0;
    }
    {
      GmlVal movement_args[6]={vreal(10),vreal(20),vreal(30),vreal(3),vreal(.5),vreal(1)};
      GmlInstance *moving=extension_vm.cur_self;
      moving->vspeed=0; moving->hspeed=0; moving->image_index=4;
      fixture_key=37; fixture_key_edge=0;
      (void)call_values(&extension_vm,"move_rpg",movement_args,5);
      if(moving->hspeed!=-3 || moving->vspeed!=0 || moving->sprite_index!=20 ||
         moving->image_xscale!=-1 || moving->image_speed!=.5 || moving->speed!=3 ||
         moving->direction!=180 || gml_builtin_fast_id("move_rpg")<0){
        fprintf(stderr,"legacy extension cursor movement mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      fixture_key_edge=2;
      (void)call_values(&extension_vm,"move_rpg",movement_args,5);
      if(moving->hspeed!=0 || moving->image_speed!=0 || moving->image_index!=0 || moving->speed!=0){
        fprintf(stderr,"legacy extension movement release mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      fixture_key='D'; fixture_key_edge=0;
      (void)call_values(&extension_vm,"move_rpg",movement_args,6);
      if(moving->hspeed!=3 || moving->sprite_index!=20 || moving->image_xscale!=1 || moving->image_speed!=.5){
        fprintf(stderr,"legacy extension letter movement mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      fixture_key=fixture_key_edge=-1;
      moving->hspeed=7;
      (void)call_values(&extension_vm,"move_rpg",movement_args,4);
      if(moving->hspeed!=7){
        fprintf(stderr,"legacy extension movement arity mismatch\n");
        free(extension_vm.inst);
        return 0;
      }

      GmlVal direction_args[4]={vreal(20),vreal(10),vreal(30),vreal(.25)};
      moving->direction=0; moving->image_xscale=-1;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=20 || moving->image_xscale!=1 || moving->image_speed!=.25 ||
         gml_builtin_fast_id("direction_rpg")<0){
        fprintf(stderr,"legacy extension right-facing animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=90;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=10){
        fprintf(stderr,"legacy extension upward animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=180;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=20 || moving->image_xscale!=-1){
        fprintf(stderr,"legacy extension left-facing animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=270;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=30){
        fprintf(stderr,"legacy extension downward animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=270; moving->y=0; moving->vspeed=9;
      (void)call_values(&extension_vm,"friction_platform",NULL,0);
      if(moving->y!=12 || moving->vspeed!=0 || gml_builtin_fast_id("friction_platform")<0){
        fprintf(stderr,"legacy extension platform-contact mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      (void)call_values(&extension_vm,"keyboard_wait",NULL,0);
      (void)call_values(&extension_vm,"destruir",NULL,0);
      if(!moving->marked || gml_builtin_fast_id("destruir")<0){
        fprintf(stderr,"legacy extension destroy-self mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
    }
    free_extension_fixture(&extension_vm);
  }
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
  {
    enum { PROJECTED_W=62, PROJECTED_H=47 };
    uint32_t phase[3][WIDTH*HEIGHT];
    const uint8_t rgba[16]={
      255,0,0,255, 0,255,0,255,
      0,0,255,255, 255,255,255,255
    };
    memcpy(render.atlas[0].px,rgba,sizeof(rgba));
    memset(pixels,0,sizeof(pixels)); memset(phase,0,sizeof(phase));
    render.interp=1;
    gml_render_begin(&render,pixels,PROJECTED_W,PROJECTED_H,340,0);
    for(int q=0;q<3;q++) render.classic_interp_phase[q]=phase[q];
    gml_draw_background_ext(&render,0,340,0,1,1,0xFFFFFF,1);
    if(render.interp_subrect_count!=1 || render.interp_subrect_bytes!=48 ||
       !render.interp_subrect_cache[0].projected_x ||
       render.interp_subrect_cache[0].projected_y ||
       !render.interp_subrect_cache[0].phase[0] ||
       !render.interp_subrect_cache[0].phase[1] ||
       !render.interp_subrect_cache[0].phase[2] ||
       phase[0][0]!=render.interp_subrect_cache[0].phase[0][1] ||
       phase[1][0]!=render.interp_subrect_cache[0].phase[1][2] ||
       phase[2][0]!=render.interp_subrect_cache[0].phase[2][3]){
      fprintf(stderr,"software classic projected interpolation cache mismatch\n");
      return 0;
    }
    size_t cached_bytes=render.interp_subrect_bytes;
    render.interp_subrect_bytes=16u*1024u*1024u;
    memset(pixels,0,sizeof(pixels)); memset(phase,0,sizeof(phase));
    gml_render_begin(&render,pixels,PROJECTED_W,PROJECTED_H,340,0);
    for(int q=0;q<3;q++) render.classic_interp_phase[q]=phase[q];
    gml_draw_background_ext(&render,0,340,0,1,1,0xFFFFFF,1);
    if(render.interp_subrect_count!=1 || render.interp_subrect_bytes!=16u*1024u*1024u){
      fprintf(stderr,"software classic projected interpolation cache reuse mismatch\n");
      return 0;
    }
    render.interp_subrect_bytes=cached_bytes;
    render.interp=0;
    free(render.tpag[0].alpha_row_min); render.tpag[0].alpha_row_min=NULL;
    free(render.tpag[0].alpha_row_max); render.tpag[0].alpha_row_max=NULL;
    free(render.tpag[0].alpha_runs); render.tpag[0].alpha_runs=NULL;
    free(render.tpag[0].argb_cache); render.tpag[0].argb_cache=NULL;
    render.tpag[0].alpha_scanned=0;
    render.tpag[0].alpha_runs_built=0;
    render.tpag[0].alpha_run_count=0;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  }
  {
    uint32_t phase[3][WIDTH*HEIGHT];
    uint8_t *fringe_atlas=(uint8_t*)realloc(render.atlas[0].px,12);
    if(!fringe_atlas){
      fprintf(stderr,"software classic transparent-fringe allocation mismatch\n");
      return 0;
    }
    render.atlas[0].px=fringe_atlas;
    render.atlas[0].w=3; render.atlas[0].h=1;
    const uint8_t rgba[12]={
      200,0,0,0, 100,100,100,255, 0,200,0,0
    };
    memcpy(render.atlas[0].px,rgba,sizeof(rgba));
    render.tpag[0].sx=1; render.tpag[0].sy=0;
    render.tpag[0].sw=render.tpag[0].sh=1;
    render.tpag[0].tx=1; render.tpag[0].ty=0;
    render.tpag[0].bw=3; render.tpag[0].bh=1;
    for(int i=0;i<WIDTH*HEIGHT;i++) pixels[i]=0xFF202020u;
    for(int q=0;q<3;q++) for(int i=0;i<WIDTH*HEIGHT;i++) phase[q][i]=0xFF202020u;
    render.interp=1; render.blendmode=0; render.alphablend=1;
    gml_render_begin(&render,pixels,WIDTH,32,0,0);
    for(int q=0;q<3;q++) render.classic_interp_phase[q]=phase[q];
    gml_draw_background_ext(&render,0,4,4,1,1,0xFFFFFF,1);
    if(phase[0][4*WIDTH+4]!=0xFF5B2929u ||
       phase[0][4*WIDTH+5]!=0xFF295B29u ||
       !render.tpag[0].interp_phase_cache[0]){
      fprintf(stderr,"software classic transparent-fringe interpolation mismatch: %08x %08x\n",
              phase[0][4*WIDTH+4],phase[0][4*WIDTH+5]);
      return 0;
    }
    for(int q=0;q<3;q++){
      render.classic_interp_phase[q]=NULL;
      free(render.tpag[0].interp_phase_cache[q]);
      render.tpag[0].interp_phase_cache[q]=NULL;
    }
    render.interp=0;
    uint8_t *restored_atlas=(uint8_t*)realloc(render.atlas[0].px,16);
    if(!restored_atlas){
      fprintf(stderr,"software classic atlas restore allocation mismatch\n");
      return 0;
    }
    render.atlas[0].px=restored_atlas;
    render.atlas[0].w=render.atlas[0].h=2;
    render.tpag[0].sx=render.tpag[0].sy=0;
    render.tpag[0].sw=render.tpag[0].sh=2;
    render.tpag[0].tx=render.tpag[0].ty=0;
    render.tpag[0].bw=render.tpag[0].bh=2;
    free(render.tpag[0].alpha_row_min); render.tpag[0].alpha_row_min=NULL;
    free(render.tpag[0].alpha_row_max); render.tpag[0].alpha_row_max=NULL;
    free(render.tpag[0].alpha_runs); render.tpag[0].alpha_runs=NULL;
    free(render.tpag[0].argb_cache); render.tpag[0].argb_cache=NULL;
    render.tpag[0].alpha_scanned=0;
    render.tpag[0].alpha_runs_built=0;
    render.tpag[0].alpha_run_count=0;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
  }
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
  {
    uint8_t atlas_backup[16];
    memcpy(atlas_backup,render.atlas[0].px,sizeof(atlas_backup));
    for(int i=0;i<4;i++){
      render.atlas[0].px[i*4]=render.atlas[0].px[i*4+1]=render.atlas[0].px[i*4+2]=0;
      render.atlas[0].px[i*4+3]=1;
    }
    pixels[5*WIDTH+5]=0xFF858585u;
    gml_draw_background_ext(&render,0,5,5,1,1,0xFFFFFF,.5);
    if(pixels[5*WIDTH+5]!=0xFF848484u){
      fprintf(stderr,"software classic draw-alpha quantization mismatch: %08x\n",pixels[5*WIDTH+5]);
      return 0;
    }
    memcpy(render.atlas[0].px,atlas_backup,sizeof(atlas_backup));
    free(render.tpag[0].alpha_row_min); render.tpag[0].alpha_row_min=NULL;
    free(render.tpag[0].alpha_row_max); render.tpag[0].alpha_row_max=NULL;
    free(render.tpag[0].alpha_runs); render.tpag[0].alpha_runs=NULL;
    free(render.tpag[0].argb_cache); render.tpag[0].argb_cache=NULL;
    render.tpag[0].alpha_scanned=0;
    render.tpag[0].alpha_runs_built=0;
    render.tpag[0].alpha_run_count=0;
  }
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
  {
    uint32_t phase[3][WIDTH*HEIGHT];
    render.classic=1;
    render.interp=0;
    memset(pixels,0,sizeof(pixels));
    memset(phase,0,sizeof(phase));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    render.classic_phase_y=phase[0];
    gml_draw_sprite_ext(&render,flipped_sprite,0,24,20,3,3,37,0xFFFFFF,1);
    if(colored_pixels(pixels,WIDTH*HEIGHT)==0 || colored_pixels(phase[0],WIDTH*HEIGHT)==0){
      fprintf(stderr,"software classic rotated sprite phase mismatch\n");
      return 0;
    }
    render.interp=1;
    memset(pixels,0,sizeof(pixels));
    memset(phase,0,sizeof(phase));
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    for(int q=0;q<3;q++) render.classic_interp_phase[q]=phase[q];
    gml_draw_sprite_ext(&render,flipped_sprite,0,24,20,3,3,37,0xFFFFFF,1);
    if(colored_pixels(pixels,WIDTH*HEIGHT)==0 ||
       colored_pixels(phase[0],WIDTH*HEIGHT)==0 ||
       colored_pixels(phase[1],WIDTH*HEIGHT)==0 ||
       colored_pixels(phase[2],WIDTH*HEIGHT)==0){
      fprintf(stderr,"software classic rotated sprite interpolation phase mismatch\n");
      return 0;
    }
    for(int q=0;q<3;q++) render.classic_interp_phase[q]=NULL;
    render.interp=0;
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
    fprintf(stderr,"software D3 2D rectangle depth mismatch: pixel=%08x alpha=%.6f classic=%d\n",
      pixels[36*WIDTH+40],render.alpha,render.classic);
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

  {
    uint32_t phase[3][WIDTH*HEIGHT];
    int phase_surface=gml_surface_create(&render,2,2);
    if(phase_surface<=0){
      fprintf(stderr,"software classic interpolation surface create mismatch\n");
      return 0;
    }
    GmlSurface *phase_surface_data=&render.surface[phase_surface-1];
    memset(pixels,0,sizeof(pixels)); memset(phase,0,sizeof(phase));
    phase_surface_data->px[0]=0xFF7F405Fu; phase_surface_data->px[1]=0xFFA060E0u;
    phase_surface_data->px[2]=0xFF20C0FFu; phase_surface_data->px[3]=0xFFFF0010u;
    phase_surface_data->dirty=1; phase_surface_data->opaque_known=0;
    phase_surface_data->all_opaque=0; phase_surface_data->all_transparent=0;
    gml_d3_reset(); render.classic=1; render.interp=1;
    gml_render_begin(&render,pixels,WIDTH,HEIGHT,0,0);
    for(int q=0;q<3;q++) render.classic_interp_phase[q]=phase[q];
    gml_draw_surface_stretched(&render,phase_surface,8,8,2,2,0xFFFFFF,1);
    if(pixels[8*WIDTH+8]!=0xFF7F405Fu ||
       phase[0][8*WIDTH+7]!=0xFF7F405Fu || phase[0][8*WIDTH+8]!=0xFF8F509Fu ||
       phase[1][7*WIDTH+8]!=0xFF7F405Fu || phase[1][8*WIDTH+8]!=0xFF4F80AFu ||
       phase[2][7*WIDTH+7]!=0xFF7F405Fu || phase[2][8*WIDTH+8]!=0xFF8F5893u){
      fprintf(stderr,"software classic surface interpolation phase mismatch: %08x %08x %08x %08x %08x %08x %08x\n",
              pixels[8*WIDTH+8],phase[0][8*WIDTH+7],phase[0][8*WIDTH+8],
              phase[1][7*WIDTH+8],phase[1][8*WIDTH+8],
              phase[2][7*WIDTH+7],phase[2][8*WIDTH+8]);
      return 0;
    }
    for(int q=0;q<3;q++) render.classic_interp_phase[q]=NULL;
    render.classic=0; render.interp=0;
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
  gml_vm_free(&vm);
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
  vm.classic_info_active=1;
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  void *state=malloc(size);
  if(!state || !gml_vm_state_save(&vm,state,size,&written) || written!=size){
    fprintf(stderr,"software D3 state save failed\n");
    free(state); return 1;
  }
  vm.classic_info_active=0;
  gml_d3_reset();
  if(state_matches(flags,values,colors)){
    fprintf(stderr,"software D3 reset did not clear state\n");
    free(state); return 1;
  }
  if(!gml_vm_state_load(&vm,state,written,&used) || used!=written ||
     !vm.classic_info_active ||
     !state_matches(flags,values,colors)){
    fprintf(stderr,"software D3 savestate roundtrip mismatch\n");
    free(state); return 1;
  }
  free(state);
  int raster_ok=raster_fixtures();
  gml_vm_free(&vm);
  if(!raster_ok) return 1;
  puts("software D3 state fixtures: ok");
  return 0;
}
