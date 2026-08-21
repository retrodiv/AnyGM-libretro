/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "content_router.h"
#include "engine_internal.h"
#include "gml_builtin.h"
#include "gml_vm_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int save_state(AnygmEngine *engine,uint8_t **data,size_t *written){
  size_t capacity=anygm_state_size(engine);
  *data=malloc(capacity?capacity:1);
  if(!*data) return 0;
  return anygm_state_save(engine,*data,capacity,written)==ANYGM_OK;
}

static uint64_t read_u64(const uint8_t *data){
  uint64_t value=0;
  for(unsigned i=0;i<8;i++) value|=(uint64_t)data[i]<<(i*8);
  return value;
}

static void write_u32(uint8_t *data,uint32_t value){
  for(unsigned i=0;i<4;i++) data[i]=(uint8_t)(value>>(i*8));
}

static void write_u64(uint8_t *data,uint64_t value){
  for(unsigned i=0;i<8;i++) data[i]=(uint8_t)(value>>(i*8));
}

static uint64_t state_checksum(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){
    uint64_t word=read_u64(data);
    hash^=word; hash*=UINT64_C(1099511628211);
    data+=8; size-=8;
  }
  while(size--){ hash^=*data++; hash*=UINT64_C(1099511628211); }
  return hash;
}

static int virtual_monitor_geometry_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=15;
  engine.win.disp_w=288;
  engine.win.disp_h=216;
  engine.width=288;
  engine.height=216;
  engine.vm.win=&engine.win;
  engine.vm.render=&engine.render;
  engine.vm.window_w=1366;
  engine.vm.window_h=768;
  engine.config.present_logical_raster=0;
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,288);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,216);
  gml_vm_global_array_set(&engine.vm,"view_xport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,288);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,216);
  gml_render_application_surface_set_draw_enabled(&engine.render,0);
  GmlRenderControl monitor={.monitor_width=1920,.monitor_height=1080};
  engine.config.monitor_width=1920;
  engine.config.monitor_height=1080;
  gml_render_control_update(&engine.render,&monitor,GML_RENDER_CONTROL_MONITOR_SIZE);

  compute_present(&engine);

  GmlVal configured_display_w=gml_builtin_call(&engine.vm,"display_get_width",NULL,0);
  GmlVal configured_display_h=gml_builtin_call(&engine.vm,"display_get_height",NULL,0);
  GmlVal window_w=gml_builtin_call(&engine.vm,"window_get_width",NULL,0);
  GmlVal window_h=gml_builtin_call(&engine.vm,"window_get_height",NULL,0);
  GmlVal position=gml_builtin_call(&engine.vm,"application_get_position",NULL,0);
  GmlArr *position_values=position.t==V_ARR?(GmlArr*)position.arr:NULL;
  int position_ok=position_values && position_values->len==4 &&
                  position_values->data[0].d==0 && position_values->data[1].d==0 &&
                  position_values->data[2].d==1920 && position_values->data[3].d==1080;
  int ok=configured_display_w.t==V_REAL && configured_display_w.d==1920 &&
         configured_display_h.t==V_REAL && configured_display_h.d==1080 &&
         window_w.t==V_REAL && window_w.d==1920 &&
         window_h.t==V_REAL && window_h.d==1080 && position_ok &&
         engine.vm.window_w==1366 && engine.vm.window_h==768 &&
         engine.output_width==1920 && engine.output_height==1080 &&
         engine.gui_space_width==1920 && engine.gui_space_height==1080 &&
         engine.host_output_width==1920 && engine.host_output_height==1080 &&
         !engine.host_canvas_active;

  engine.vm.window_fullscreen=1;
  compute_present(&engine);
  GmlVal fullscreen_window_w=gml_builtin_call(&engine.vm,"window_get_width",NULL,0);
  GmlVal fullscreen_window_h=gml_builtin_call(&engine.vm,"window_get_height",NULL,0);
  GmlVal fullscreen_position=gml_builtin_call(&engine.vm,"application_get_position",NULL,0);
  GmlArr *fullscreen_position_values=
    fullscreen_position.t==V_ARR?(GmlArr*)fullscreen_position.arr:NULL;
  int fullscreen_position_ok=fullscreen_position_values && fullscreen_position_values->len==4 &&
                             fullscreen_position_values->data[0].d==0 &&
                             fullscreen_position_values->data[1].d==0 &&
                             fullscreen_position_values->data[2].d==1920 &&
                             fullscreen_position_values->data[3].d==1080;
  unsigned fullscreen_output_width=engine.output_width;
  unsigned fullscreen_output_height=engine.output_height;
  int fullscreen_gui_width=engine.gui_space_width;
  int fullscreen_gui_height=engine.gui_space_height;
  ok=ok && fullscreen_window_w.t==V_REAL && fullscreen_window_w.d==1920 &&
           fullscreen_window_h.t==V_REAL && fullscreen_window_h.d==1080 &&
           engine.output_width==1920 && engine.output_height==1080 &&
           engine.gui_space_width==1920 && engine.gui_space_height==1080 &&
           engine.host_output_width==1920 && engine.host_output_height==1080 &&
           !engine.host_canvas_active &&
           fullscreen_position_ok;

  engine.vm.window_fullscreen=0;
  compute_present(&engine);
  GmlVal restored_window_w=gml_builtin_call(&engine.vm,"window_get_width",NULL,0);
  GmlVal restored_window_h=gml_builtin_call(&engine.vm,"window_get_height",NULL,0);
  ok=ok && restored_window_w.t==V_REAL && restored_window_w.d==1920 &&
           restored_window_h.t==V_REAL && restored_window_h.d==1080 &&
           engine.vm.window_w==1366 && engine.vm.window_h==768 &&
           engine.output_width==1920 && engine.output_height==1080 &&
           engine.gui_space_width==1920 && engine.gui_space_height==1080 &&
           engine.host_output_width==1920 && engine.host_output_height==1080 &&
           !engine.host_canvas_active;

  monitor.monitor_width=0;
  monitor.monitor_height=0;
  engine.config.monitor_width=0;
  engine.config.monitor_height=0;
  gml_render_control_update(&engine.render,&monitor,GML_RENDER_CONTROL_MONITOR_SIZE);
  compute_present(&engine);
  GmlVal fallback_display_w=gml_builtin_call(&engine.vm,"display_get_width",NULL,0);
  GmlVal fallback_display_h=gml_builtin_call(&engine.vm,"display_get_height",NULL,0);
  GmlVal fallback_window_w=gml_builtin_call(&engine.vm,"window_get_width",NULL,0);
  GmlVal fallback_window_h=gml_builtin_call(&engine.vm,"window_get_height",NULL,0);
  ok=ok && fallback_display_w.t==V_REAL && fallback_display_w.d==1366 &&
           fallback_display_h.t==V_REAL && fallback_display_h.d==768 &&
           fallback_window_w.t==V_REAL && fallback_window_w.d==1366 &&
           fallback_window_h.t==V_REAL && fallback_window_h.d==768 &&
           engine.output_width==1366 && engine.output_height==768 &&
           engine.gui_space_width==1366 && engine.gui_space_height==768 &&
           engine.host_output_width==1366 && engine.host_output_height==768 &&
           !engine.host_canvas_active;
  uint32_t source_pixels[8]={
    0x102030,0x405060,0x708090,0xA0B0C0,
    0xC0B0A0,0x908070,0x605040,0x302010
  };
  uint32_t host_pixels[16];
  AnygmEngine frame={0};
  frame.screen=source_pixels;
  frame.host_screen=host_pixels;
  frame.output_width=4;
  frame.output_height=2;
  frame.host_output_width=4;
  frame.host_output_height=4;
  frame.host_canvas_active=1;
  frame.host_canvas_x=0;
  frame.host_canvas_y=1;
  frame.host_canvas_width=4;
  frame.host_canvas_height=2;
  const uint32_t *resolved_pixels=NULL;
  unsigned resolved_width=0,resolved_height=0;
  int frame_ok=resolve_host_frame(
    &frame,&resolved_pixels,&resolved_width,&resolved_height) &&
    resolved_pixels==host_pixels && resolved_width==4 && resolved_height==4 &&
    host_pixels[0]==0 && host_pixels[3]==0 &&
    !memcmp(host_pixels+4,source_pixels,sizeof source_pixels) &&
    host_pixels[12]==0 && host_pixels[15]==0;
  ok=ok && frame_ok;
  if(!ok)
    fprintf(stderr,
      "virtual monitor did not select coherent effective and host geometry:"
      " configured=%.0fx%.0f fallback=%.0fx%.0f window=%.0fx%.0f"
      " fullscreen=%.0fx%.0f fullscreen_output=%ux%u fullscreen_gui=%dx%d"
      " restored=%.0fx%.0f output=%ux%u gui=%dx%d host=%ux%u fit=%d,%d %dx%d"
      " position=%d fullscreen_position=%d frame=%d\n",
      configured_display_w.d,configured_display_h.d,
      fallback_display_w.d,fallback_display_h.d,window_w.d,window_h.d,
      fullscreen_window_w.d,fullscreen_window_h.d,
      fullscreen_output_width,fullscreen_output_height,
      fullscreen_gui_width,fullscreen_gui_height,
      restored_window_w.d,restored_window_h.d,
      engine.output_width,engine.output_height,
      engine.gui_space_width,engine.gui_space_height,
      engine.host_output_width,engine.host_output_height,
      engine.host_canvas_x,engine.host_canvas_y,
      engine.host_canvas_width,engine.host_canvas_height,
      position_ok,fullscreen_position_ok,frame_ok);
  gml_values_release(&position,1);
  gml_values_release(&fullscreen_position,1);
  gml_vm_free(&engine.vm);
  return ok;
}

static int live_monitor_override_policy(void){
  static const char program[]=
    "monitorview|240|4:3|7:3\n"
    "?monitor @application_w=$monitor_view_w\n"
    "?monitor @application_h=$monitor_view_h\n"
    "?monitor $fixture_half_extra=$monitor_extra_w*0.5\n"
    "?monitor view_wport[0]=$monitor_view_w\n"
    "?monitor camera[8]:width=$monitor_view_w\n"
    "?monitor camera[8]:height=$monitor_view_h\n"
    "?monitor camera[8]:x=$monitor_extra_w*-0.5\n"
    "view_wview[0]=$monitor_view_w\n"
    "view_hview[0]=$monitor_view_h\n"
    "view_wport[0]=$monitor_view_w\n"
    "view_hport[0]=$monitor_view_h\n";
  AnygmEngine engine={0};
  engine.win.bytecode=17;
  engine.win.disp_w=320;
  engine.win.disp_h=240;
  engine.base_width=320;
  engine.base_height=240;
  engine.width=320;
  engine.height=240;
  engine.vm.win=&engine.win;
  engine.vm.render=&engine.render;
  engine.config.content_overrides=1;
  engine.config.monitor_width=1920;
  engine.config.monitor_height=1080;
  gml_vm_global_array_set(&engine.vm,"__gml_camera_live",8,1);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_x",8,0);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_w",8,320);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_h",8,240);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,320);
  GmlRenderControl monitor={.monitor_width=1920,.monitor_height=1080};
  gml_render_control_update(&engine.render,&monitor,GML_RENDER_CONTROL_MONITOR_SIZE);
  if(!gml_render_application_surface_ensure_owned(&engine.render,320,240)){
    fputs("monitor override application-surface setup failed\n",stderr);
    gml_vm_free(&engine.vm);
    return 0;
  }
  char error[256]={0};
  if(!engine_boot_overrides_parse(program,engine.boot_cheats,&engine.boot_cheat_count,
                                  error,sizeof error)){
    fprintf(stderr,"monitor override program was rejected: %s\n",error);
    gml_render_free(&engine.render);
    gml_vm_free(&engine.vm);
    return 0;
  }

  engine.monitor_override_pending=1;
  apply_monitor_overrides(&engine);
  GmlRenderApplicationWriteView application={0};
  int ok=gml_render_application_surface_owned_view(&engine.render,&application) &&
    application.width==427 && application.height==240 &&
    gml_global_num(&engine.vm,"fixture_half_extra")==53.5 &&
    gml_global_arr(&engine.vm,"view_wport",0)==427 &&
    gml_global_arr(&engine.vm,"__gml_camera_x",8)==-53.5 &&
    gml_global_arr(&engine.vm,"__gml_camera_w",8)==427 &&
    gml_global_arr(&engine.vm,"__gml_camera_h",8)==240 &&
    engine.monitor_override_pending==0;

  /* The monitor scope is an edge. It must not freeze authored camera movement between changes. */
  gml_vm_global_array_set(&engine.vm,"__gml_camera_x",8,123);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,123);
  apply_monitor_overrides(&engine);
  apply_sticky_cheats(&engine);
  ok=ok && gml_global_arr(&engine.vm,"__gml_camera_x",8)==123 &&
           gml_global_arr(&engine.vm,"view_wport",0)==427;

  monitor.monitor_width=400;
  monitor.monitor_height=400;
  engine.config.monitor_width=400;
  engine.config.monitor_height=400;
  gml_render_control_update(&engine.render,&monitor,GML_RENDER_CONTROL_MONITOR_SIZE);
  engine.monitor_override_pending=1;
  apply_monitor_overrides(&engine);
  memset(&application,0,sizeof application);
  ok=ok && gml_render_application_surface_owned_view(&engine.render,&application) &&
    application.width==320 && application.height==240 &&
    gml_global_num(&engine.vm,"fixture_half_extra")==0 &&
    gml_global_arr(&engine.vm,"view_wport",0)==320 &&
    gml_global_arr(&engine.vm,"__gml_camera_x",8)==0 &&
    gml_global_arr(&engine.vm,"__gml_camera_w",8)==320;

  CheatSlot rejected[GML_MAX_CHEATS];
  int rejected_count=0;
  ok=ok && !engine_boot_overrides_parse(
    "monitorview|240|4:0|7:3\n",rejected,&rejected_count,error,sizeof error);
  rejected_count=0;
  ok=ok && !engine_boot_overrides_parse(
    "?monitor camera[9-8]:x=0\n",rejected,&rejected_count,error,sizeof error);
  if(!ok)
    fprintf(stderr,"live monitor override policy mismatch: app=%dx%d half=%.3f "
                   "port=%.3f camera=(%.3f,%.3f,%.3f)\n",
            application.width,application.height,
            gml_global_num(&engine.vm,"fixture_half_extra"),
            gml_global_arr(&engine.vm,"view_wport",0),
            gml_global_arr(&engine.vm,"__gml_camera_x",8),
            gml_global_arr(&engine.vm,"__gml_camera_w",8),
            gml_global_arr(&engine.vm,"__gml_camera_h",8));
  gml_render_free(&engine.render);
  gml_vm_free(&engine.vm);
  return ok;
}

static int host_canvas_scale_policy(void){
  enum {
    SOURCE_WIDTH=5,SOURCE_HEIGHT=3,
    HOST_WIDTH=31,HOST_HEIGHT=23
  };
  uint32_t source_pixels[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t host_pixels[HOST_WIDTH*HOST_HEIGHT];
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source_pixels[(size_t)y*SOURCE_WIDTH+x]=
      0xA0000000u|((uint32_t)(13+x*31+y*17)<<16)|
      ((uint32_t)(19+x*23+y*29)<<8)|(uint32_t)(7+x*37+y*11);
  memset(host_pixels,0x5A,sizeof host_pixels);
  AnygmEngine engine={0};
  engine.screen=source_pixels;
  engine.host_screen=host_pixels;
  engine.output_width=SOURCE_WIDTH;
  engine.output_height=SOURCE_HEIGHT;
  engine.host_output_width=HOST_WIDTH;
  engine.host_output_height=HOST_HEIGHT;
  engine.host_canvas_active=1;
  engine.host_canvas_x=3;
  engine.host_canvas_y=2;
  engine.host_canvas_width=25;
  engine.host_canvas_height=18;

  const uint32_t *resolved_pixels=NULL;
  unsigned resolved_width=0,resolved_height=0;
  for(int pass=0;pass<2;pass++){
    if(!resolve_host_frame(&engine,&resolved_pixels,&resolved_width,&resolved_height) ||
       resolved_pixels!=host_pixels || resolved_width!=HOST_WIDTH ||
       resolved_height!=HOST_HEIGHT){
      fputs("host canvas magnification did not resolve the configured framebuffer\n",stderr);
      return 0;
    }
    for(int y=0;y<HOST_HEIGHT;y++) for(int x=0;x<HOST_WIDTH;x++){
      uint32_t expected=0;
      if(x>=engine.host_canvas_x && x<engine.host_canvas_x+engine.host_canvas_width &&
         y>=engine.host_canvas_y && y<engine.host_canvas_y+engine.host_canvas_height){
        int canvas_x=x-engine.host_canvas_x;
        int canvas_y=y-engine.host_canvas_y;
        int source_x=(int)(((int64_t)(2*canvas_x+1)*SOURCE_WIDTH)/
                           (2*engine.host_canvas_width));
        int source_y=(int)(((int64_t)(2*canvas_y+1)*SOURCE_HEIGHT)/
                           (2*engine.host_canvas_height));
        expected=source_pixels[(size_t)source_y*SOURCE_WIDTH+source_x]&0xFFFFFFu;
      }
      if(host_pixels[(size_t)y*HOST_WIDTH+x]!=expected){
        fprintf(stderr,"host canvas magnification mismatch at %d,%d on pass %d: %08x != %08x\n",
                x,y,pass,host_pixels[(size_t)y*HOST_WIDTH+x],expected);
        return 0;
      }
    }
    for(size_t index=0;index<SOURCE_WIDTH*SOURCE_HEIGHT;index++)
      source_pixels[index]^=0x0055AA33u;
  }

  engine.host_canvas_x=1;
  engine.host_canvas_y=4;
  engine.host_canvas_width=20;
  engine.host_canvas_height=12;
  if(!resolve_host_frame(&engine,&resolved_pixels,&resolved_width,&resolved_height)){
    fputs("host canvas geometry change did not resolve\n",stderr);
    return 0;
  }
  for(int y=0;y<HOST_HEIGHT;y++) for(int x=0;x<HOST_WIDTH;x++){
    uint32_t expected=0;
    if(x>=engine.host_canvas_x && x<engine.host_canvas_x+engine.host_canvas_width &&
       y>=engine.host_canvas_y && y<engine.host_canvas_y+engine.host_canvas_height){
      int canvas_x=x-engine.host_canvas_x;
      int canvas_y=y-engine.host_canvas_y;
      int source_x=(int)(((int64_t)(2*canvas_x+1)*SOURCE_WIDTH)/
                         (2*engine.host_canvas_width));
      int source_y=(int)(((int64_t)(2*canvas_y+1)*SOURCE_HEIGHT)/
                         (2*engine.host_canvas_height));
      expected=source_pixels[(size_t)source_y*SOURCE_WIDTH+source_x]&0xFFFFFFu;
    }
    if(host_pixels[(size_t)y*HOST_WIDTH+x]!=expected){
      fprintf(stderr,"host canvas geometry-change mismatch at %d,%d: %08x != %08x\n",
              x,y,host_pixels[(size_t)y*HOST_WIDTH+x],expected);
      return 0;
    }
  }
  return 1;
}

typedef struct {
  const char *room_redirect;
} RoomRedirectFixture;

static const char *room_redirect_setting(void *userdata,const char *name){
  RoomRedirectFixture *fixture=(RoomRedirectFixture*)userdata;
  return !strcmp(name,"GML_REDIRECT_ROOM_ORDER")?fixture->room_redirect:NULL;
}

static int room_order_redirect_policy(void){
  uint8_t room_chunk[]={4,0,0,0};
  uint32_t order[]={0,1,2,3};
  RoomRedirectFixture fixture={.room_redirect="2:3"};
  AnygmEngine engine={0};
  engine.host.userdata=&fixture;
  engine.host.development_setting=room_redirect_setting;
  engine.win.room_order=order;
  engine.win.n_room_order=4;
  engine.win.data=room_chunk;
  engine.win.size=sizeof room_chunk;
  memcpy(engine.win.chunks[0].name,"ROOM",4);
  engine.win.chunks[0].off=0;
  engine.win.chunks[0].size=sizeof room_chunk;
  engine.win.n_chunks=1;
  int ok=core_opt_redirect_room_order(&engine) && order[2]==3;
  const char *invalid[]={"-1:2","2:-1","4:2","2:4","2:1junk","2147483648:1",NULL};
  for(int i=0;ok && invalid[i];i++){
    order[2]=2;
    fixture.room_redirect=invalid[i];
    ok=!core_opt_redirect_room_order(&engine) && order[2]==2;
  }
  fixture.room_redirect=NULL;
  ok=ok && !core_opt_redirect_room_order(&engine) && order[2]==2;
  if(!ok) fputs("room-order redirect accepted an invalid route or changed the wrong slot\n",stderr);
  return ok;
}

static int screen_refresh_present_latch_policy(void){
  AnygmEngine engine={0};
  engine.win.classic_version=800;
  engine.vm.win=&engine.win;
  gml_render_application_surface_set_draw_enabled(&engine.render,1);

  screen_refresh_present_latch_hook(&engine.vm,&engine);
  GmlRenderPresentationMetrics presentation={0};
  gml_render_presentation_metrics(&engine.render,&presentation);
  int ok=!engine.content_presented && presentation.application_draw_enabled;

  engine.render.content_composited_screen=1;
  screen_refresh_present_latch_hook(&engine.vm,&engine);
  gml_render_presentation_metrics(&engine.render,&presentation);
  ok=ok && engine.content_presented && !presentation.application_draw_enabled;

  if(!ok)
    fputs("screen_refresh did not distinguish a transparent scratch blit from a composed frame\n",stderr);
  return ok;
}

static int application_surface_port_scale_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=17;
  engine.vm.win=&engine.win;
  engine.vm.gui_w=320;
  engine.vm.gui_h=240;
  if(!application_surface_scales_full_view_port(&engine,1,0,0,320,240,640,480)){
    fputs("full GUI view port did not scale with the owned application surface\n",stderr);
    return 0;
  }
  if(application_surface_scales_full_view_port(&engine,2,0,0,320,240,640,480) ||
     application_surface_scales_full_view_port(&engine,1,8,0,320,240,640,480) ||
     application_surface_scales_full_view_port(&engine,1,0,0,300,240,640,480) ||
     application_surface_scales_full_view_port(&engine,1,0,0,320,240,640,360) ||
     application_surface_scales_full_view_port(&engine,1,0,0,320,240,320,240)){
    fputs("partial, inset, mismatched, or native view port selected application scaling\n",stderr);
    return 0;
  }
  engine.win.bytecode=15;
  if(application_surface_scales_full_view_port(&engine,1,0,0,320,240,640,480)){
    fputs("first-generation presentation selected current application scaling\n",stderr);
    return 0;
  }
  engine.win.bytecode=17;
  if(!default_application_surface_uses_full_view_port(
       &engine,1,0,0,640,480,320,240,320,240) ||
     !default_application_surface_uses_full_view_port(
       &engine,1,0,0,640,480,320,240,640,480) ||
     default_application_surface_uses_full_view_port(
       &engine,2,0,0,640,480,320,240,640,480) ||
     default_application_surface_uses_full_view_port(
       &engine,1,8,0,640,480,320,240,640,480) ||
     default_application_surface_uses_full_view_port(
       &engine,1,0,0,640,480,640,480,640,480) ||
     default_application_surface_uses_full_view_port(
       &engine,1,0,0,640,360,320,240,640,480)){
    fputs("default current application-surface viewport policy mismatch\n",stderr);
    return 0;
  }
  engine.win.bytecode=15;
  if(default_application_surface_uses_full_view_port(
       &engine,1,0,0,640,480,320,240,640,480)){
    fputs("first-generation content selected a current default application surface\n",stderr);
    return 0;
  }
  return 1;
}

static int first_generation_application_surface_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=16;
  engine.win.disp_w=640;
  engine.win.disp_h=480;
  engine.vm.win=&engine.win;
  uint32_t *borrowed=calloc((size_t)320*240,sizeof(*borrowed));
  if(!borrowed) return 0;
  borrowed[0]=0xFF336699u;
  gml_render_application_surface_bind(&engine.render,borrowed,320,240,1);
  int ok=gml_render_application_surface_ensure_owned(&engine.render,640,480);
  GmlRenderApplicationWriteView view={0};
  ok=ok && gml_render_application_surface_owned_view(&engine.render,&view) &&
    view.width==640 && view.height==480 && view.pixels &&
    view.pixels[0]==0xFF336699u &&
    application_surface_matches_first_generation_view_port(
      &engine,1,0,0,640,480,view.width,view.height) &&
    !application_surface_matches_first_generation_view_port(
      &engine,2,0,0,640,480,view.width,view.height) &&
    !application_surface_matches_first_generation_view_port(
      &engine,1,8,0,640,480,view.width,view.height) &&
    !application_surface_matches_first_generation_view_port(
      &engine,1,0,0,320,240,view.width,view.height);
  engine.vm.gui_w=500;
  engine.vm.gui_h=380;
  ok=ok && application_surface_matches_first_generation_view_port(
    &engine,1,0,0,480,360,view.width,view.height);
  engine.vm.gui_w=300;
  engine.vm.gui_h=240;
  ok=ok && !application_surface_matches_first_generation_view_port(
    &engine,1,0,0,480,360,view.width,view.height);
  engine.win.bytecode=17;
  ok=ok && !application_surface_matches_first_generation_view_port(
    &engine,1,0,0,640,480,view.width,view.height);
  if(!ok)
    fprintf(stderr,
      "first-generation application surface policy mismatch: owned=%d size=%dx%d pixel=%08x\n",
      view.pixels!=NULL,view.width,view.height,view.pixels?view.pixels[0]:0);
  free(engine.render.app_surface_owned);
  engine.render.app_surface_owned=NULL;
  free(borrowed);
  gml_vm_free(&engine.vm);
  return ok;
}

static int first_generation_dynamic_camera_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=16;
  engine.vm.win=&engine.win;
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_camera",0,0);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,3);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,5);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,320);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,180);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,640);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,360);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_live",0,1);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_x",0,37);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_y",0,59);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_w",0,400);
  gml_vm_global_array_set(&engine.vm,"__gml_camera_h",0,300);
  GmlPresentView view={0};
  int ok=present_view_get(&engine,0,&view) && view.camera==0 &&
    view.x==37 && view.y==59 && view.w==400 && view.h==300 &&
    view.pw==640 && view.ph==360;
  if(!ok)
    fprintf(stderr,
      "first-generation dynamic camera was not selected: camera=%d rect=(%.0f,%.0f %.0fx%.0f)\n",
      view.camera,view.x,view.y,view.w,view.h);
  gml_vm_free(&engine.vm);
  return ok;
}

static int explicit_window_screen_stage_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=17;
  engine.win.disp_w=640;
  engine.win.disp_h=480;
  engine.width=320;
  engine.height=240;
  engine.vm.win=&engine.win;
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,320);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,240);
  gml_vm_global_array_set(&engine.vm,"view_xport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,640);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,320);
  gml_render_application_surface_set_draw_enabled(&engine.render,1);
  compute_present(&engine);
  int ok=engine.gui_space_width==640 && engine.gui_space_height==320 &&
         engine.output_width==640 && engine.output_height==480;
  if(!ok)
    fprintf(stderr,
      "explicit window did not retain the port raster for final scaling:"
      " gui=%dx%d output=%ux%u\n",
      engine.gui_space_width,engine.gui_space_height,
      engine.output_width,engine.output_height);
  gml_vm_free(&engine.vm);
  return ok;
}

static int first_generation_window_raster_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=15;
  engine.win.disp_w=288;
  engine.win.disp_h=216;
  engine.width=288;
  engine.height=216;
  engine.vm.win=&engine.win;
  engine.vm.window_w=1366;
  engine.vm.window_h=768;
  engine.config.present_logical_raster=0;
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,288);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,216);
  gml_vm_global_array_set(&engine.vm,"view_xport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,288);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,216);
  gml_render_application_surface_set_draw_enabled(&engine.render,0);
  compute_present(&engine);
  GmlRenderPresentationMetrics presentation={0};
  gml_render_presentation_metrics(&engine.render,&presentation);
  int target_width=1024,target_height=768,logical_width=0,logical_height=0;
  screen_stage_gui_geometry(
    &engine,&presentation,(int)engine.output_width,(int)engine.output_height,
    &target_width,&target_height,&logical_width,&logical_height);
  int ok=engine.screen_stage_window_raster &&
         engine.gui_space_width==1366 && engine.gui_space_height==768 &&
         engine.output_width==1366 && engine.output_height==768 &&
         presentation.effective_width==1366 && presentation.effective_height==768 &&
         target_width==1366 && target_height==768 &&
         logical_width==1366 && logical_height==768;
  if(!ok)
    fprintf(stderr,
      "first-generation content-owned window raster was not coherent:"
      " flag=%d gui=%dx%d output=%ux%u effective=%dx%d target=%dx%d logical=%dx%d\n",
      engine.screen_stage_window_raster,
      engine.gui_space_width,engine.gui_space_height,
      engine.output_width,engine.output_height,
      presentation.effective_width,presentation.effective_height,
      target_width,target_height,
      logical_width,logical_height);
  gml_vm_free(&engine.vm);
  return ok;
}

static int modern_self_compositor_window_raster_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=17;
  engine.win.disp_w=240;
  engine.win.disp_h=320;
  engine.width=240;
  engine.height=320;
  engine.vm.win=&engine.win;
  engine.config.present_logical_raster=0;
  engine.config.monitor_width=1920;
  engine.config.monitor_height=1080;
  GmlRenderControl monitor={.monitor_width=1920,.monitor_height=1080};
  gml_render_control_update(&engine.render,&monitor,GML_RENDER_CONTROL_MONITOR_SIZE);
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,240);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,320);
  gml_vm_global_array_set(&engine.vm,"view_xport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,240);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,320);
  gml_render_application_surface_set_draw_enabled(&engine.render,0);
  compute_present(&engine);
  GmlRenderPresentationMetrics presentation={0};
  gml_render_presentation_metrics(&engine.render,&presentation);
  int target_width=810,target_height=1080,logical_width=0,logical_height=0;
  screen_stage_gui_geometry(
    &engine,&presentation,(int)engine.output_width,(int)engine.output_height,
    &target_width,&target_height,&logical_width,&logical_height);
  int ok=engine.screen_stage_window_raster &&
         engine.gui_space_width==1920 && engine.gui_space_height==1080 &&
         engine.output_width==1920 && engine.output_height==1080 &&
         presentation.effective_width==1920 && presentation.effective_height==1080 &&
         target_width==1920 && target_height==1080 &&
         logical_width==1920 && logical_height==1080;
  if(!ok)
    fprintf(stderr,
      "modern self-compositor did not retain window coordinates:"
      " flag=%d gui=%dx%d output=%ux%u effective=%dx%d target=%dx%d logical=%dx%d\n",
      engine.screen_stage_window_raster,
      engine.gui_space_width,engine.gui_space_height,
      engine.output_width,engine.output_height,
      presentation.effective_width,presentation.effective_height,
      target_width,target_height,
      logical_width,logical_height);
  gml_vm_free(&engine.vm);
  return ok;
}

static int automatic_surface_monitor_fit_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=16;
  engine.win.disp_w=640;
  engine.win.disp_h=480;
  engine.width=640;
  engine.height=480;
  engine.vm.win=&engine.win;
  engine.vm.render=&engine.render;
  engine.vm.window_w=640;
  engine.vm.window_h=480;
  engine.config.present_logical_raster=0;
  engine.config.monitor_width=1920;
  engine.config.monitor_height=1080;
  GmlRenderControl monitor={.monitor_width=1920,.monitor_height=1080};
  gml_render_control_update(&engine.render,&monitor,GML_RENDER_CONTROL_MONITOR_SIZE);
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,640);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,480);
  gml_vm_global_array_set(&engine.vm,"view_xport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,640);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,480);
  gml_render_application_surface_set_draw_enabled(&engine.render,1);

  compute_present(&engine);

  GmlVal display_width=gml_builtin_call(&engine.vm,"display_get_width",NULL,0);
  GmlVal display_height=gml_builtin_call(&engine.vm,"display_get_height",NULL,0);
  GmlVal window_width=gml_builtin_call(&engine.vm,"window_get_width",NULL,0);
  GmlVal window_height=gml_builtin_call(&engine.vm,"window_get_height",NULL,0);
  int ok=display_width.t==V_REAL && display_width.d==1920 &&
         display_height.t==V_REAL && display_height.d==1080 &&
         window_width.t==V_REAL && window_width.d==1920 &&
         window_height.t==V_REAL && window_height.d==1080 &&
         engine.vm.window_w==640 && engine.vm.window_h==480 &&
         !engine.screen_stage_window_raster &&
         engine.output_width==640 && engine.output_height==480 &&
         engine.gui_space_width==640 && engine.gui_space_height==480 &&
         engine.host_output_width==1920 && engine.host_output_height==1080 &&
         engine.host_canvas_active &&
         engine.host_canvas_x==240 && engine.host_canvas_y==0 &&
         engine.host_canvas_width==1440 && engine.host_canvas_height==1080;
  if(!ok)
    fprintf(stderr,
      "automatic surface did not retain aspect inside the virtual monitor:"
      " queries=%gx%g/%gx%g flag=%d output=%ux%u gui=%dx%d"
      " host=%ux%u fit=(%d,%d %dx%d)\n",
      display_width.d,display_height.d,window_width.d,window_height.d,
      engine.screen_stage_window_raster,
      engine.output_width,engine.output_height,
      engine.gui_space_width,engine.gui_space_height,
      engine.host_output_width,engine.host_output_height,
      engine.host_canvas_x,engine.host_canvas_y,
      engine.host_canvas_width,engine.host_canvas_height);
  gml_vm_free(&engine.vm);
  return ok;
}

static int first_generation_oversized_gui_policy(void){
  AnygmEngine engine={0};
  engine.win.bytecode=14;
  engine.win.disp_w=256;
  engine.win.disp_h=192;
  engine.width=256;
  engine.height=192;
  engine.vm.win=&engine.win;
  engine.vm.gui_w=288;
  engine.vm.gui_h=216;
  gml_vm_global_array_set(&engine.vm,"view_visible",0,1);
  gml_vm_global_array_set(&engine.vm,"view_xview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yview",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wview",0,256);
  gml_vm_global_array_set(&engine.vm,"view_hview",0,192);
  gml_vm_global_array_set(&engine.vm,"view_xport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_yport",0,0);
  gml_vm_global_array_set(&engine.vm,"view_wport",0,256);
  gml_vm_global_array_set(&engine.vm,"view_hport",0,192);
  compute_present(&engine);
  int ok=engine.gui_space_width==288 && engine.gui_space_height==216 &&
         engine.output_width==256 && engine.output_height==192;
  if(!ok)
    fprintf(stderr,
      "first-generation oversized GUI replaced the authored presentation raster:"
      " gui=%dx%d output=%ux%u\n",
      engine.gui_space_width,engine.gui_space_height,
      engine.output_width,engine.output_height);
  gml_vm_free(&engine.vm);
  return ok;
}

static int draw_schedule_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_draw_content_create(&fixture)){
    fputs("draw schedule fixture creation failed\n",stderr);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  double pre=ok?gml_global_num(&engine->vm,"fixture_pre_draw"):0;
  double draw=ok?gml_global_num(&engine->vm,"fixture_draw"):0;
  double post=ok?gml_global_num(&engine->vm,"fixture_post_draw"):0;
  double view_sum=ok?gml_global_num(&engine->vm,"fixture_view_sum"):0;
  ok=ok && pre==2 && draw==2 && post==2 && view_sum==4;
  if(!ok)
    fprintf(stderr,
      "draw schedule mismatch: pre=%.0f draw=%.0f post=%.0f view-sum=%.0f\n",
      pre,draw,post,view_sum);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

/* An alarm pause holds the countdown still instead of writing a value into it. The property worth
 * asserting is not that the ticks stop — a value freeze does that too — but what disarming leaves:
 * the counter must carry on from its remainder, so the first tick after the pause arrives sooner
 * than a full period. A design that captured and restored a value would restart the period here. */
static int alarm_pause_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_alarm_content_create(&fixture)){
    fprintf(stderr,"alarm-pause fixture creation failed\n");
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
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;

  /* Run to the first tick so the phase is known — the alarm has just been re-armed to 60 — then
   * thirty frames into the next period, which is where a partial remainder is worth holding. */
  int ticked=0;
  for(int frame=0;ok && frame<200 && !ticked;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
    ticked=ok && gml_global_num(&engine->vm,"fixture_alarm_ticks")>=1;
  }
  if(ok && !ticked){ fprintf(stderr,"alarm pause: fixture never ticked\n"); ok=0; }
  for(int frame=0;ok && frame<30;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }

  /* Arming takes effect on the following frame, because the pause table is rebuilt by the sticky
   * pass after the step — the same latency every freeze in this engine has. Settle past it before
   * reading the count that must then stay put. */
  ok=ok && anygm_set_runtime_override(engine,0,1u,"alarmpause|obj_fixture|1")==ANYGM_OK;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  double paused=ok?gml_global_num(&engine->vm,"fixture_alarm_ticks"):0;
  for(int frame=0;ok && frame<300;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  double held=ok?gml_global_num(&engine->vm,"fixture_alarm_ticks"):0;
  if(ok && held!=paused){
    fprintf(stderr,"alarm pause: countdown kept running (%.0f -> %.0f over 300 frames)\n",
            paused,held);
    ok=0;
  }

  /* Disarm and count the frames the held remainder still needs. Landing inside the period is the
   * assertion: a mechanism that restored a captured value would spend a whole 60 again. */
  ok=ok && anygm_set_runtime_override(engine,0,0u,NULL)==ANYGM_OK;
  int resumed_after=-1;
  for(int frame=1;ok && frame<=60;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
    if(ok && gml_global_num(&engine->vm,"fixture_alarm_ticks")>held){ resumed_after=frame; break; }
  }
  if(ok && resumed_after<0){
    fprintf(stderr,"alarm pause: countdown never resumed within a full period\n");
    ok=0;
  } else if(ok && resumed_after>=60){
    fprintf(stderr,"alarm pause: countdown restarted its period instead of resuming (%d frames)\n",
            resumed_after);
    ok=0;
  }
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

/* One frontend entry may carry several `;`-separated directives, so a cheat that needs a set of
 * flags is one toggle rather than one per flag. The chain has to behave like the entries it
 * replaces in both directions: every directive frozen while it is armed, and every directive's
 * previous value put back when it is dropped — including the ones parked in continuation slots,
 * which is where a chain can silently leak a forced value. */
static int chained_override_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_draw_content_create(&fixture)){
    fputs("chained-override fixture creation failed\n",stderr);
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
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;

  /* Seed the last target so restoring has a value to return to that is not the empty default. */
  if(ok) gml_set_global_scalar(&engine->vm,"fixture_chain_e",7);

  ok=ok && anygm_set_runtime_override(
             engine,0,1u,
             "$fixture_chain_a=1;$fixture_chain_b=2;$fixture_chain_c=3;"
             "$fixture_chain_d=4;$fixture_chain_e=5")==ANYGM_OK;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  static const char *const names[5]={"fixture_chain_a","fixture_chain_b","fixture_chain_c",
                                     "fixture_chain_d","fixture_chain_e"};
  for(int n=0;ok && n<5;n++){
    double held=gml_global_num(&engine->vm,names[n]);
    if(held!=n+1){
      fprintf(stderr,"chained override: %s froze at %.0f, expected %d\n",names[n],held,n+1);
      ok=0;
    }
  }

  /* Drop the entry the way the frontend does on Apply Changes with the box cleared. */
  ok=ok && anygm_set_runtime_override(engine,0,0u,NULL)==ANYGM_OK;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  static const double restored_to[5]={0,0,0,0,7};
  for(int n=0;ok && n<5;n++){
    double back=gml_global_num(&engine->vm,names[n]);
    if(back!=restored_to[n]){
      fprintf(stderr,"chained override: %s stayed at %.0f after the entry was dropped, "
                     "expected %.0f\n",names[n],back,restored_to[n]);
      ok=0;
    }
  }

  /* Re-arming has to work a second time: the first arm consumed the slots the chain parks in. */
  ok=ok && anygm_set_runtime_override(
             engine,0,1u,"$fixture_chain_a=9;$fixture_chain_b=9")==ANYGM_OK;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  if(ok && (gml_global_num(&engine->vm,"fixture_chain_a")!=9 ||
            gml_global_num(&engine->vm,"fixture_chain_b")!=9)){
    fprintf(stderr,"chained override: re-armed chain froze %.0f/%.0f, expected 9/9\n",
            gml_global_num(&engine->vm,"fixture_chain_a"),
            gml_global_num(&engine->vm,"fixture_chain_b"));
    ok=0;
  }
  /* A directive the shortened chain no longer carries must not still be held by a stale slot. */
  if(ok && gml_global_num(&engine->vm,"fixture_chain_c")!=0){
    fprintf(stderr,"chained override: dropped directive kept writing (%.0f)\n",
            gml_global_num(&engine->vm,"fixture_chain_c"));
    ok=0;
  }
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int anchor_script_override_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_anchor_script_content_create(&fixture)){
    fputs("anchor script override: fixture creation failed\n",stderr);
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
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK &&
         anygm_set_runtime_override(engine,0,1u,"call|anchor_call")==ANYGM_OK;
  for(int frame=0;ok && frame<3;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  double calls=ok?gml_global_num(&engine->vm,"fixture_anchor_calls"):-1;
  if(calls!=1){
    fprintf(stderr,"anchor script override: called %.0f times, expected one\n",calls);
    ok=0;
  }
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int framebuffer_retention_case(
    int (*create_fixture)(AnygmSyntheticContent *),const char *label){
  AnygmSyntheticContent fixture;
  if(!create_fixture(&fixture)){
    fprintf(stderr,"%s framebuffer-retention fixture creation failed\n",label);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     output.pixels && output.width==64 && output.height==48;
  size_t pixels=(size_t)output.width*output.height;
  uint32_t *painted=ok?malloc(pixels*sizeof(*painted)):NULL;
  ok=ok && painted!=NULL;
  if(ok) memcpy(painted,output.pixels,pixels*sizeof(*painted));
  size_t unexpected=0;
  if(ok) for(size_t i=0;i<pixels;i++)
    if((painted[i]&0xFFFFFFu)!=0x996633u) unexpected++;
  size_t framebuffer_unexpected=0;
  if(ok) for(size_t i=0;i<(size_t)engine->width*engine->height;i++)
    if((engine->fb[i]&0xFFFFFFu)!=0x996633u) framebuffer_unexpected++;
  ok=ok && unexpected==0;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     engine->vm.room_index==1 && output.pixels &&
     !memcmp(painted,output.pixels,pixels*sizeof(*painted));
  if(!ok){
    int canvas_width=0,canvas_height=0;
    int view_count=engine?present_view_count(engine,NULL,&canvas_width,&canvas_height):0;
    fprintf(stderr,
      "%s room did not present and retain the complete application framebuffer"
      " (%zu unexpected output pixels, %zu unexpected application pixels,"
      " render=%ux%u, output=%ux%u, views=%d, canvas=%dx%d)\n",
      label,unexpected,framebuffer_unexpected,engine?engine->width:0,engine?engine->height:0,
      output.width,output.height,view_count,canvas_width,canvas_height);
  }
  free(painted);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

/* Classic generations clear the drawing target every frame, so disabling background drawing must
 * show the outside color rather than the frame the previous room completed. Without that clear,
 * half-transparent drawing accumulates towards opacity over consecutive frames. */
static int framebuffer_clear_case(
    int (*create_fixture)(AnygmSyntheticContent *),const char *label){
  AnygmSyntheticContent fixture;
  if(!create_fixture(&fixture)){
    fprintf(stderr,"%s framebuffer-clear fixture creation failed\n",label);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     output.pixels && output.width==64 && output.height==48;
  size_t pixels=(size_t)output.width*output.height;
  const uint32_t *presented=ok?(const uint32_t *)output.pixels:NULL;
  size_t painted_unexpected=0;
  if(presented) for(size_t i=0;i<pixels;i++)
    if((presented[i]&0xFFFFFFu)!=0x996633u) painted_unexpected++;
  ok=ok && painted_unexpected==0;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     engine->vm.room_index==1 && output.pixels;
  /* 0x00204060 is authored as a classic BGR colour, so its presented value is 0x604020. */
  presented=ok?(const uint32_t *)output.pixels:NULL;
  size_t cleared_unexpected=0;
  if(presented) for(size_t i=0;i<pixels;i++)
    if((presented[i]&0xFFFFFFu)!=0x604020u) cleared_unexpected++;
  ok=ok && cleared_unexpected==0;
  if(!ok)
    fprintf(stderr,
      "%s classic room with background drawing disabled did not clear the framebuffer"
      " (%zu unexpected painted pixels, %zu unexpected cleared pixels, room=%d,"
      " output=%ux%u)\n",
      label,painted_unexpected,cleared_unexpected,engine?engine->vm.room_index:-1,
      output.width,output.height);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int framebuffer_retention_policy(void){
  return framebuffer_retention_case(anygm_synthetic_framebuffer_content_create,"single-view") &&
         framebuffer_retention_case(
           anygm_synthetic_multiview_framebuffer_content_create,"multi-view") &&
         framebuffer_clear_case(
           anygm_synthetic_classic_framebuffer_content_create,"classic single-view") &&
         framebuffer_clear_case(
           anygm_synthetic_classic_multiview_framebuffer_content_create,"classic multi-view");
}

static int background_color_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_background_color_content_create(&fixture)){
    fputs("background-color fixture creation failed\n",stderr);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  ok=ok && output.pixels && output.width==64 && output.height==48;
  double canonical=ok?gml_global_num(&engine->vm,"background_color"):0;
  double alias=ok?gml_global_num(&engine->vm,"fixture_background_alias"):0;
  size_t unexpected=0;
  const uint32_t *pixels=output.pixels;
  GmlRoom room={0};
  int have_room=engine && gml_vm_room_get(&engine->vm,engine->vm.room_index,&room)==0;
  if(ok) for(size_t i=0;i<(size_t)output.width*output.height;i++)
    if(pixels[i]!=0xFF112233u) unexpected++;
  ok=ok && canonical==0x00332211u && alias==canonical && unexpected==0;
  uint8_t *state=NULL;
  size_t state_size=0;
  ok=ok && save_state(engine,&state,&state_size);
  if(ok) *gml_varmap_put(&engine->vm.globals,"background_color")=vreal(0);
  ok=ok && anygm_state_load(engine,state,state_size)==ANYGM_OK &&
     gml_global_num(&engine->vm,"background_color")==canonical &&
     cur_room_bg(engine)==0xFF112233u;
  if(!ok)
    fprintf(stderr,
      "background-color routing mismatch: canonical=0x%08x alias=0x%08x engine=0x%08x application=0x%08x first=0x%08x draw=%d unexpected=%zu\n",
      (unsigned)canonical,(unsigned)alias,engine?engine->background:0,
      engine&&engine->fb?(unsigned)engine->fb[0]:0,pixels?(unsigned)pixels[0]:0,
      have_room?room.draw_bg:-1,unexpected);
  free(state);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int clear_view_background_case(
    int (*create_fixture)(AnygmSyntheticContent *),const char *label){
  AnygmSyntheticContent fixture;
  if(!create_fixture(&fixture)){
    fprintf(stderr,"%s clear-view fixture creation failed\n",label);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     engine->vm.room_index==1 && output.pixels &&
     output.width==64 && output.height==48;
  const uint32_t *pixels=(const uint32_t *)output.pixels;
  for(size_t i=0;ok && i<(size_t)output.width*output.height;i++)
    ok=(pixels[i]&0xFFFFFFu)==0;
  if(!ok)
    fprintf(stderr,
      "%s room clear-view flag did not clear the retained application surface\n",
      label);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int clear_view_background_policy(void){
  return clear_view_background_case(anygm_synthetic_clear_view_content_create,"single-view") &&
         clear_view_background_case(
           anygm_synthetic_multiview_clear_view_content_create,"multi-view");
}

/* A deactivated pending room instance finishes Create once, remains inactive, and retains its
 * authored variable after activation. */
static int room_start_deactivation_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_room_deactivation_content_create(&fixture)){
    fputs("room-deactivation fixture creation failed\n",stderr);
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
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK &&
         anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  double created=ok?gml_global_num(&engine->vm,"fixture_sleeper_created"):-1;
  double asleep_steps=ok?gml_global_num(&engine->vm,"fixture_sleeper_steps"):-1;
  ok=ok && created==1 && asleep_steps==0;
  for(int frame=0;ok && frame<4;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  double woken_steps=ok?gml_global_num(&engine->vm,"fixture_sleeper_steps"):-1;
  double endurance=ok?gml_global_num(&engine->vm,"fixture_sleeper_endurance"):-1;
  ok=ok && gml_global_num(&engine->vm,"fixture_sleeper_created")==1 &&
     woken_steps>0 && endurance==150;
  if(!ok){
    char error[512]={0};
    if(engine) anygm_get_last_error(engine,error,sizeof error);
    fprintf(stderr,
      "room-start deactivation mismatch: error=%s created=%.0f asleep_steps=%.0f "
      "woken_steps=%.0f endurance=%.0f\n",
      error,created,asleep_steps,woken_steps,endurance);
  }
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int game_change_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_game_change_content_create(&fixture)){
    fputs("game-change fixture creation failed\n",stderr);
    return 0;
  }
  char launch_path[256];
  snprintf(launch_path,sizeof launch_path,"%s/launch.anygm",fixture.directory);
  FILE *launch=fopen(launch_path,"wb");
  static const char launch_text[]=
    "[anygm]\n"
    "payload=data.win\n"
    "[overrides]\n"
    "?gameres $fixture_anchor_marker=7\n";
  int launch_ok=launch &&
    fwrite(launch_text,1,sizeof launch_text-1,launch)==sizeof launch_text-1;
  if(launch && fclose(launch)!=0) launch_ok=0;
  if(!launch_ok){
    remove(launch_path);
    anygm_synthetic_content_destroy(&fixture);
    fputs("game-change launch anchor creation failed\n",stderr);
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
  source.path=launch_path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  AnygmConfigDelta presentation={0};
  presentation.struct_size=sizeof presentation;
  presentation.fields=ANYGM_CONFIG_PRESENT_LOGICAL_RASTER;
  presentation.values.struct_size=sizeof presentation.values;
  presentation.values.present_logical_raster=1;
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_set_config(engine,&presentation)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  char original_save_directory[512]={0};
  char original_program_directory[1024]={0};
  if(ok){
    snprintf(original_save_directory,sizeof original_save_directory,"%s",engine->win.save_dir);
    snprintf(original_program_directory,sizeof original_program_directory,"%s/",
             engine->win.content_dir);
  }
  uint8_t *root_state=NULL;
  size_t root_state_size=0;
  ok=ok && save_state(engine,&root_state,&root_state_size);
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  GmlVal *parameter_three=ok?gml_varmap_get(&engine->vm.globals,
                                             "fixture_parameter_three"):NULL;
  GmlVal *parameter_four=ok?gml_varmap_get(&engine->vm.globals,
                                            "fixture_parameter_four"):NULL;
  GmlVal *working_directory=ok?gml_varmap_get(&engine->vm.globals,
                                               "fixture_working_directory"):NULL;
  GmlVal *program_directory=ok?gml_varmap_get(&engine->vm.globals,
                                               "fixture_program_directory"):NULL;
  AnygmAvInfo av_info={0};
  av_info.struct_size=sizeof av_info;
  ok=ok && anygm_get_av_info(engine,&av_info)==ANYGM_OK &&
     output.pixels && output.width==80 && output.height==50 &&
     av_info.base_width==80 && av_info.base_height==50 &&
     gml_global_num(&engine->vm,"fixture_child_marker")==1 &&
     gml_global_num(&engine->vm,"fixture_anchor_marker")==7 &&
     gml_global_num(&engine->vm,"fixture_end_observed")==1 &&
     gml_global_num(&engine->vm,"fixture_parameter_count")==4 &&
     parameter_three && parameter_three->t==V_STR && parameter_three->s &&
     !strcmp(parameter_three->s,"child_marker") &&
     parameter_four && parameter_four->t==V_STR && parameter_four->s &&
     !strcmp(parameter_four->s,"quoted value") &&
     working_directory && working_directory->t==V_STR && working_directory->s &&
     strstr(working_directory->s,"secondary/") &&
     program_directory && program_directory->t==V_STR && program_directory->s &&
     !strcmp(program_directory->s,original_program_directory) &&
     !strcmp(engine->win.save_dir,original_save_directory) &&
     strstr(engine->current_content_path,"secondary/data.win");
  uint8_t *state=NULL;
  size_t state_size=0;
  ok=ok && save_state(engine,&state,&state_size);
  AnygmEngine *cold_engine=NULL;
  AnygmResult cold_load_result=ANYGM_ERROR_INVALID_STATE;
  ok=ok && anygm_create(&services,&cold_engine)==ANYGM_OK &&
     anygm_set_config(cold_engine,&presentation)==ANYGM_OK &&
     anygm_load(cold_engine,&source,NULL)==ANYGM_OK;
  if(ok) cold_load_result=anygm_state_load(cold_engine,state,state_size);
  output.struct_size=sizeof output;
  ok=ok && cold_load_result==ANYGM_OK &&
     anygm_run_frame(cold_engine,&input,&output)==ANYGM_OK &&
     output.width==80 && output.height==50 &&
     gml_global_num(&cold_engine->vm,"fixture_child_marker")==1 &&
     gml_global_num(&cold_engine->vm,"fixture_anchor_marker")==7 &&
     gml_global_num(&cold_engine->vm,"fixture_parameter_count")==4 &&
     !strcmp(cold_engine->win.save_dir,original_save_directory) &&
     !strcmp(cold_engine->state_content_locator,"secondary/data.win") &&
     !strcmp(cold_engine->launch_parameters,
             "-game data.win child_marker \"quoted value\"") &&
     strstr(cold_engine->current_content_path,"secondary/data.win");
  ok=ok && anygm_state_load(cold_engine,root_state,root_state_size)==ANYGM_OK &&
     !cold_engine->state_content_locator[0] &&
     !cold_engine->launch_parameters[0] &&
     !strcmp(cold_engine->current_content_path,fixture.path) &&
     anygm_state_load(cold_engine,state,state_size)==ANYGM_OK;
  presentation.values.present_logical_raster=0;
  output.struct_size=sizeof output;
  ok=ok && anygm_set_config(engine,&presentation)==ANYGM_OK &&
     anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     gml_global_num(&engine->vm,"fixture_anchor_marker")==3 &&
     anygm_state_load(engine,state,state_size)==ANYGM_OK;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     gml_global_num(&engine->vm,"fixture_anchor_marker")==3;
  presentation.values.present_logical_raster=1;
  output.struct_size=sizeof output;
  ok=ok && anygm_set_config(engine,&presentation)==ANYGM_OK &&
     anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     gml_global_num(&engine->vm,"fixture_anchor_marker")==7;
  if(ok){
    char child_path[sizeof engine->current_content_path];
    snprintf(child_path,sizeof child_path,"%s",engine->current_content_path);
    GmlVal escape_args[]={vstr(".."),vstr("-game data.win")};
    (void)gml_builtin_call(&engine->vm,"game_change",escape_args,2);
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_ERROR_INVALID_CONTENT &&
       !strcmp(engine->current_content_path,child_path) &&
       gml_global_num(&engine->vm,"fixture_child_marker")==1;
    output.struct_size=sizeof output;
    ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
       !strcmp(engine->current_content_path,child_path);
  }
  if(!ok){
    char error[512]={0};
    if(engine) anygm_get_last_error(engine,error,sizeof error);
    fprintf(stderr,
      "game-change lifecycle mismatch: error=%s output=%ux%u room=%d marker=%.0f "
      "anchor=%.0f end=%.0f params=%.0f save=%s cold-load=%d\n",
      error,output.width,output.height,engine?engine->vm.room_index:-1,
      engine?gml_global_num(&engine->vm,"fixture_child_marker"):0,
      engine?gml_global_num(&engine->vm,"fixture_anchor_marker"):0,
      engine?gml_global_num(&engine->vm,"fixture_end_observed"):0,
      engine?gml_global_num(&engine->vm,"fixture_parameter_count"):0,
      engine?engine->win.save_dir:"",(int)cold_load_result);
  }
  anygm_destroy(cold_engine);
  free(root_state);
  free(state);
  anygm_destroy(engine);
  remove(launch_path);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int game_restart_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_game_restart_content_create(&fixture)){
    fputs("game-restart fixture creation failed\n",stderr);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  for(int frame=0;ok && frame<16;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
       engine->vm.game_end==0 && engine->vm.room_index==0 &&
       gml_global_num(&engine->vm,"fixture_restart_boot")==1;
  }
  if(!ok) fputs("game restart did not produce a clean replacement runtime\n",stderr);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int state_input_history_roundtrip(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)){
    fputs("input-history fixture creation failed\n",stderr);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK &&
         anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  input.connected_gamepads=1;
  input.gamepad_buttons[0][ANYGM_PAD_RIGHT]=1;
  AnygmFrameOutput output={0};
  output.struct_size=sizeof output;
  for(int frame=0;ok && frame<2;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  uint8_t *state=NULL;
  size_t state_size=0;
  ok=ok && engine->pad_current[0][ANYGM_PAD_RIGHT] &&
     engine->pad_previous[0][ANYGM_PAD_RIGHT] &&
     save_state(engine,&state,&state_size);
  input.gamepad_buttons[0][ANYGM_PAD_RIGHT]=0;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     !engine->pad_current[0][ANYGM_PAD_RIGHT] &&
     engine->pad_previous[0][ANYGM_PAD_RIGHT] &&
     anygm_state_load(engine,state,state_size)==ANYGM_OK &&
     engine->pad_current[0][ANYGM_PAD_RIGHT] &&
     engine->pad_previous[0][ANYGM_PAD_RIGHT];
  input.gamepad_buttons[0][ANYGM_PAD_RIGHT]=1;
  output.struct_size=sizeof output;
  ok=ok && anygm_run_frame(engine,&input,&output)==ANYGM_OK &&
     engine->pad_current[0][ANYGM_PAD_RIGHT] &&
     engine->pad_previous[0][ANYGM_PAD_RIGHT];
  free(state);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  if(!ok) fputs("state roundtrip did not preserve the input edge baseline\n",stderr);
  return ok;
}

static int raw_gamepad_button_layout_policy(void){
  /* Earlier profiles map raw 0..15 button indexes to XInput digital-button
   * bit positions; modern profiles use 1..16 button enums. This synthetic
   * frame checks both interpretations. */
  AnygmEngine legacy={0};
  engine_input_bind(&legacy);
  legacy.pad_current[0][ANYGM_PAD_START]=1;
  legacy.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  int ok=legacy.vm.input.gamepad(legacy.vm.input.userdata,0,4,0) &&
     legacy.vm.input.gamepad(legacy.vm.input.userdata,0,32778,0) &&
     legacy.vm.input.gamepad(legacy.vm.input.userdata,0,12,0) &&
     !legacy.vm.input.gamepad(legacy.vm.input.userdata,0,0,0) &&
     !legacy.vm.input.gamepad(legacy.vm.input.userdata,0,14,0) &&
     !legacy.vm.input.gamepad(legacy.vm.input.userdata,0,10,0);

  AnygmEngine modern={0};
  modern.win.bytecode=17;
  engine_input_bind(&modern);
  modern.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  ok=ok && modern.vm.input.gamepad(modern.vm.input.userdata,0,1,0) &&
     !modern.vm.input.gamepad(modern.vm.input.userdata,0,4,0);
  if(!ok)
    fputs("raw gamepad button indexes did not follow profile layouts\n",stderr);
  return ok;
}

static int input_binding_ownership_policy(void){
  AnygmEngine keyboard_only={0};
  keyboard_only.win.classic_version=800;
  engine_input_bind(&keyboard_only);
  keyboard_only.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  int ok=keyboard_only.vm.input.key(keyboard_only.vm.input.userdata,'Z',0) &&
         keyboard_only.vm.input.key(keyboard_only.vm.input.userdata,1,0);

  /* Content that never references a pad builtin cannot hear the pad any other way: the RetroPad
   * stays a keyboard even while the frontend reports a pad connected. */
  AnygmEngine keyboard_only_pad_connected={0};
  keyboard_only_pad_connected.win.classic_version=800;
  keyboard_only_pad_connected.config.gamepad_connected=1;
  engine_input_bind(&keyboard_only_pad_connected);
  keyboard_only_pad_connected.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  ok=ok &&
     keyboard_only_pad_connected.vm.input.key(
         keyboard_only_pad_connected.vm.input.userdata,'Z',0) &&
     keyboard_only_pad_connected.vm.input.key(
         keyboard_only_pad_connected.vm.input.userdata,1,0);

  /* Content that reads the pad owns it while one is connected: no phantom keys. */
  static uint32_t pad_ref_addr=0;
  static const char *pad_ref_name="joystick_check_button";
  static uint8_t pad_ref_kind=GML_REF_FUNCTION;
  AnygmEngine gamepad_mode={0};
  gamepad_mode.win.classic_version=800;
  gamepad_mode.win.ref_addr=&pad_ref_addr;
  gamepad_mode.win.ref_name=&pad_ref_name;
  gamepad_mode.win.ref_kind=&pad_ref_kind;
  gamepad_mode.win.n_refs=1;
  gamepad_mode.config.gamepad_connected=1;
  engine_input_bind(&gamepad_mode);
  gamepad_mode.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  ok=ok &&
     !gamepad_mode.vm.input.key(gamepad_mode.vm.input.userdata,'Z',0) &&
     !gamepad_mode.vm.input.key(gamepad_mode.vm.input.userdata,1,0) &&
     gamepad_mode.vm.input.gamepad(gamepad_mode.vm.input.userdata,0,32769,0);

  gamepad_mode.event_key_current[ANYGM_KEY_z]=1;
  ok=ok && gamepad_mode.vm.input.key(gamepad_mode.vm.input.userdata,'Z',0) &&
     gamepad_mode.vm.input.key(gamepad_mode.vm.input.userdata,1,0);

  /* A synthetic classic reference table follows the same ownership policy
   * as a modern reference table when it contains a joystick builtin. */
  AnygmEngine classic_auto={0};
  classic_auto.win.classic_version=800;
  classic_auto.win.ref_addr=&pad_ref_addr;
  classic_auto.win.ref_name=&pad_ref_name;
  classic_auto.win.ref_kind=&pad_ref_kind;
  classic_auto.win.n_refs=1;
  classic_auto.config.gamepad_connected=ANYGM_GAMEPAD_AUTO;
  engine_input_bind(&classic_auto);
  classic_auto.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  ok=ok &&
     !classic_auto.vm.input.key(classic_auto.vm.input.userdata,'Z',0) &&
     classic_auto.vm.input.gamepad_connected(classic_auto.vm.input.userdata,0);

  /* A synthetic classic table with no pad reference retains keyboard mapping. */
  AnygmEngine classic_auto_keyboard={0};
  classic_auto_keyboard.win.classic_version=800;
  classic_auto_keyboard.config.gamepad_connected=ANYGM_GAMEPAD_AUTO;
  engine_input_bind(&classic_auto_keyboard);
  classic_auto_keyboard.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  ok=ok &&
     classic_auto_keyboard.vm.input.key(classic_auto_keyboard.vm.input.userdata,'Z',0) &&
     !classic_auto_keyboard.vm.input.gamepad_connected(
         classic_auto_keyboard.vm.input.userdata,0);

  /* A modern title under Auto answers connected exactly when it can hear a pad. */
  static uint32_t modern_ref_addr=0;
  static const char *modern_ref_name="gamepad_button_check";
  static uint8_t modern_ref_kind=GML_REF_FUNCTION;
  AnygmEngine modern_auto={0};
  modern_auto.win.ref_addr=&modern_ref_addr;
  modern_auto.win.ref_name=&modern_ref_name;
  modern_auto.win.ref_kind=&modern_ref_kind;
  modern_auto.win.n_refs=1;
  modern_auto.config.gamepad_connected=ANYGM_GAMEPAD_AUTO;
  engine_input_bind(&modern_auto);
  modern_auto.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  ok=ok &&
     !modern_auto.vm.input.key(modern_auto.vm.input.userdata,'Z',0) &&
     modern_auto.vm.input.gamepad_connected(modern_auto.vm.input.userdata,0) &&
     modern_auto.vm.input.gamepad(modern_auto.vm.input.userdata,0,32769,0);

  AnygmEngine modern_auto_keyboard={0};
  modern_auto_keyboard.config.gamepad_connected=ANYGM_GAMEPAD_AUTO;
  engine_input_bind(&modern_auto_keyboard);
  modern_auto_keyboard.pad_current[0][ANYGM_PAD_FACE_BOTTOM]=1;
  ok=ok &&
     modern_auto_keyboard.vm.input.key(modern_auto_keyboard.vm.input.userdata,'Z',0) &&
     !modern_auto_keyboard.vm.input.gamepad_connected(
         modern_auto_keyboard.vm.input.userdata,0);

  if(!ok)
    fputs("RetroPad ownership did not follow what the content can hear\n",stderr);
  return ok;
}

static int simulated_key_lifetime_policy(void){
  AnygmEngine engine={0};
  engine_input_bind(&engine);

  /* A simulated press remains held across polls until the matching release. */
  engine.vm.input.key_press(engine.vm.input.userdata,39);
  int ok=engine.vm.input.key(engine.vm.input.userdata,39,0) &&
         engine.vm.input.key(engine.vm.input.userdata,39,1);
  memcpy(engine.key_previous,engine.key_current,sizeof engine.key_current);
  engine_input_poll_keyboard(&engine);
  ok=ok && engine.vm.input.key(engine.vm.input.userdata,39,0) &&
     !engine.vm.input.key(engine.vm.input.userdata,39,1);
  engine.vm.input.key_release(engine.vm.input.userdata,39);
  ok=ok && !engine.vm.input.key(engine.vm.input.userdata,39,0) &&
     engine.vm.input.key(engine.vm.input.userdata,39,2);

  /* A simulated key raised while the same physical key is down also follows its falling edge. */
  memset(&engine,0,sizeof engine);
  engine_input_bind(&engine);
  engine.input.keys[ANYGM_KEY_RIGHT]=1;
  engine_input_poll_keyboard(&engine);
  engine.vm.input.key_press(engine.vm.input.userdata,39);
  memcpy(engine.key_previous,engine.key_current,sizeof engine.key_current);
  engine.input.keys[ANYGM_KEY_RIGHT]=0;
  engine_input_poll_keyboard(&engine);
  ok=ok && !engine.vm.input.key(engine.vm.input.userdata,39,0) &&
     engine.vm.input.key(engine.vm.input.userdata,39,2);

  /* The synthetic latch has no repeat edge. A later physical-key transition
   * of the same virtual key still produces a press edge. */
  memset(&engine,0,sizeof engine);
  engine_input_bind(&engine);
  engine.vm.input.key_press(engine.vm.input.userdata,'Z');
  memcpy(engine.key_previous,engine.key_current,sizeof engine.key_current);
  engine_input_poll_keyboard(&engine);
  ok=ok && engine.vm.input.key(engine.vm.input.userdata,'Z',0) &&
     !engine.vm.input.key(engine.vm.input.userdata,'Z',1);
  engine.input.keys[ANYGM_KEY_z]=1;
  engine_input_poll_keyboard(&engine);
  ok=ok && engine.vm.input.key(engine.vm.input.userdata,'Z',1);
  if(!ok)
    fputs("simulated keyboard input did not retain and release its latched lifetime\n",stderr);
  return ok;
}

static int simulated_key_frame_lifetime_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_simulated_key_content_create(&fixture)){
    fputs("simulated-key frame fixture creation failed\n",stderr);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK && anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  for(int frame=0;ok && frame<5;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  ok=ok && gml_global_num(&engine->vm,"fixture_ticks")==3 &&
     !engine->vm.input.key(engine->vm.input.userdata,39,0);
  if(!ok)
    fprintf(stderr,"simulated key did not span engine frames until explicit release: ticks=%.0f\n",
            engine?gml_global_num(&engine->vm,"fixture_ticks"):-1.0);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

/* A synthetic press and release in one Step must deliver one Key Press event
 * in the following Step, even though the held key is already up. The event
 * records the Step count to distinguish delayed delivery from duplicates. */
static int bridged_key_press_delivery_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_bridged_key_content_create(&fixture)){
    fputs("bridged-key fixture creation failed\n",stderr);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK && anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  for(int frame=0;ok && frame<5;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  /* Key events run before the step, so the event seeing one completed step is the frame after the
   * one that raised the press. A second delivery would leave a later count behind. */
  ok=ok && gml_global_num(&engine->vm,"fixture_presses")==1 &&
     gml_global_num(&engine->vm,"fixture_press_step")==1 &&
     !engine->vm.input.key(engine->vm.input.userdata,39,0);
  if(!ok)
    fprintf(stderr,"a key pressed and released in one step was not delivered once to the next: "
                   "presses=%.0f step=%.0f\n",
            engine?gml_global_num(&engine->vm,"fixture_presses"):-1.0,
            engine?gml_global_num(&engine->vm,"fixture_press_step"):-1.0);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

/* A synthetic instance pairs press and release in Step, then checks held
 * input in Begin Step. The fixture counts how many following frames observe
 * the held key and checks that the key goes up after the pairs stop. */
static int bridged_key_hold_policy(void){
  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_bridged_hold_content_create(&fixture)){
    fputs("bridged-hold fixture creation failed\n",stderr);
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
  int ok=anygm_create(&services,&engine)==ANYGM_OK && anygm_load(engine,&source,NULL)==ANYGM_OK;
  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput output={0};
  for(int frame=0;ok && frame<10;frame++){
    output.struct_size=sizeof output;
    ok=anygm_run_frame(engine,&input,&output)==ANYGM_OK;
  }
  /* Five presses, five frames that read the key held, and the key up once the presses stop: the
   * count rising with the presses is what separates a key that survives its step from one that
   * never reaches Begin Step at all. */
  ok=ok && gml_global_num(&engine->vm,"fixture_held")==5 &&
     gml_global_num(&engine->vm,"fixture_steps")==10 &&
     !engine->vm.input.key(engine->vm.input.userdata,39,0);
  if(!ok)
    fprintf(stderr,"a key pressed and released once per step did not read held: held=%.0f steps=%.0f\n",
            engine?gml_global_num(&engine->vm,"fixture_held"):-1.0,
            engine?gml_global_num(&engine->vm,"fixture_steps"):-1.0);
  anygm_destroy(engine);
  anygm_synthetic_content_destroy(&fixture);
  return ok;
}

static int expect_rejected_unchanged(AnygmEngine *engine,const uint8_t *candidate,size_t size,
                                     const uint8_t *baseline,size_t baseline_size,
                                     const char *label){
  if(anygm_state_load(engine,candidate,size)!=ANYGM_ERROR_STATE_MISMATCH){
    fprintf(stderr,"%s was not rejected\n",label);
    return 0;
  }
  uint8_t *after=NULL;
  size_t after_size=0;
  int unchanged=save_state(engine,&after,&after_size) && after_size==baseline_size &&
                !memcmp(after,baseline,baseline_size);
  if(!unchanged && after && after_size==baseline_size){
    size_t first=0;
    while(first<baseline_size && after[first]==baseline[first]) first++;
    if(first<baseline_size)
      fprintf(stderr,"%s first state difference at %zu: %02x != %02x\n",
              label,first,after[first],baseline[first]);
    first=112;
    while(first<baseline_size && after[first]==baseline[first]) first++;
    if(first<baseline_size)
      fprintf(stderr,"%s first payload difference at %zu: %02x != %02x\n",
              label,first,after[first],baseline[first]);
    if(first<baseline_size){
      size_t from=first>16?first-16:0,to=first+32<baseline_size?first+32:baseline_size;
      fputs("after:   ",stderr); for(size_t i=from;i<to;i++) fprintf(stderr,"%02x",after[i]); fputc('\n',stderr);
      fputs("baseline:",stderr); for(size_t i=from;i<to;i++) fprintf(stderr,"%02x",baseline[i]); fputc('\n',stderr);
    }
  }
  free(after);
  if(!unchanged) fprintf(stderr,"%s changed the engine despite rejection\n",label);
  return unchanged;
}

int main(int argc,char **argv){
  if(argc==3 && !strcmp(argv[1],"--case")){
    if(!strcmp(argv[2],"virtual_monitor_geometry"))
      return virtual_monitor_geometry_policy()?0:1;
    if(!strcmp(argv[2],"live_monitor_override"))
      return live_monitor_override_policy()?0:1;
    if(!strcmp(argv[2],"host_canvas_scale"))
      return host_canvas_scale_policy()?0:1;
    if(!strcmp(argv[2],"screen_refresh_present_latch"))
      return screen_refresh_present_latch_policy()?0:1;
    if(!strcmp(argv[2],"application_surface_port_scale"))
      return application_surface_port_scale_policy()?0:1;
    if(!strcmp(argv[2],"first_generation_application_surface"))
      return first_generation_application_surface_policy()?0:1;
    if(!strcmp(argv[2],"game_restart"))
      return game_restart_policy()?0:1;
    if(!strcmp(argv[2],"alarm_pause"))
      return alarm_pause_policy()?0:1;
    if(!strcmp(argv[2],"chained_override"))
      return chained_override_policy()?0:1;
    if(!strcmp(argv[2],"anchor_script_override"))
      return anchor_script_override_policy()?0:1;
    if(!strcmp(argv[2],"first_generation_dynamic_camera"))
      return first_generation_dynamic_camera_policy()?0:1;
    if(!strcmp(argv[2],"explicit_window_screen_stage"))
      return explicit_window_screen_stage_policy()?0:1;
    if(!strcmp(argv[2],"first_generation_window_raster"))
      return first_generation_window_raster_policy()?0:1;
    if(!strcmp(argv[2],"modern_self_compositor_window_raster"))
      return modern_self_compositor_window_raster_policy()?0:1;
    if(!strcmp(argv[2],"automatic_surface_monitor_fit"))
      return automatic_surface_monitor_fit_policy()?0:1;
    if(!strcmp(argv[2],"first_generation_oversized_gui"))
      return first_generation_oversized_gui_policy()?0:1;
    if(!strcmp(argv[2],"background_color"))
      return background_color_policy()?0:1;
    if(!strcmp(argv[2],"multi_view_application_canvas"))
      return framebuffer_retention_case(
        anygm_synthetic_multiview_framebuffer_content_create,"multi-view")?0:1;
    if(!strcmp(argv[2],"game_change"))
      return game_change_policy()?0:1;
    if(!strcmp(argv[2],"room_start_deactivation"))
      return room_start_deactivation_policy()?0:1;
    if(!strcmp(argv[2],"input_binding_ownership"))
      return input_binding_ownership_policy()?0:1;
    if(!strcmp(argv[2],"raw_gamepad_button_layout"))
      return raw_gamepad_button_layout_policy()?0:1;
    if(!strcmp(argv[2],"simulated_key_lifetime"))
      return simulated_key_lifetime_policy()?0:1;
    if(!strcmp(argv[2],"simulated_key_frame_lifetime"))
      return simulated_key_frame_lifetime_policy()?0:1;
    if(!strcmp(argv[2],"bridged_key_press_delivery"))
      return bridged_key_press_delivery_policy()?0:1;
    if(!strcmp(argv[2],"bridged_key_hold"))
      return bridged_key_hold_policy()?0:1;
    fprintf(stderr,"unknown integration case: %s\n",argv[2]);
    return 1;
  }
  if(argc!=1){
    fputs("usage: test_engine_instances [--case screen_refresh_present_latch|"
          "virtual_monitor_geometry|live_monitor_override|host_canvas_scale|"
          "application_surface_port_scale|"
          "first_generation_application_surface|"
          "game_restart|"
          "anchor_script_override|"
          "first_generation_dynamic_camera|"
          "explicit_window_screen_stage|"
          "first_generation_window_raster|"
          "modern_self_compositor_window_raster|"
          "automatic_surface_monitor_fit|"
          "first_generation_oversized_gui|"
          "background_color|multi_view_application_canvas|game_change|"
          "input_binding_ownership|simulated_key_lifetime|simulated_key_frame_lifetime|"
          "bridged_key_press_delivery|bridged_key_hold|"
          "room_start_deactivation]\n",stderr);
    return 1;
  }
  if(!virtual_monitor_geometry_policy()) return 1;
  if(!live_monitor_override_policy()) return 1;
  if(!host_canvas_scale_policy()) return 1;
  if(!room_order_redirect_policy()) return 1;
  if(!screen_refresh_present_latch_policy()) return 1;
  if(!application_surface_port_scale_policy()) return 1;
  if(!first_generation_application_surface_policy()) return 1;
  if(!first_generation_dynamic_camera_policy()) return 1;
  if(!explicit_window_screen_stage_policy()) return 1;
  if(!first_generation_window_raster_policy()) return 1;
  if(!modern_self_compositor_window_raster_policy()) return 1;
  if(!automatic_surface_monitor_fit_policy()) return 1;
  if(!first_generation_oversized_gui_policy()) return 1;
  if(!draw_schedule_policy()) return 1;
  if(!chained_override_policy()) return 1;
  if(!anchor_script_override_policy()) return 1;
  if(!background_color_policy()) return 1;
  if(!framebuffer_retention_policy()) return 1;
  if(!clear_view_background_policy()) return 1;
  if(!game_restart_policy()) return 1;
  if(!game_change_policy()) return 1;
  if(!room_start_deactivation_policy()) return 1;
  if(!state_input_history_roundtrip()) return 1;
  if(!input_binding_ownership_policy()) return 1;
  if(!simulated_key_lifetime_policy()) return 1;
  if(!simulated_key_frame_lifetime_policy()) return 1;
  if(!bridged_key_press_delivery_policy()) return 1;
  if(!bridged_key_hold_policy()) return 1;
  if(!alarm_pause_policy()) return 1;
  char label[128];
  anygm_content_save_label("/library/fixture_bundle/data.win",label,sizeof label);
  if(strcmp(label,"fixture_bundle")){
    fprintf(stderr,"generic payload save label mismatch: %s\n",label);
    return 1;
  }
  anygm_content_save_label("C:\\library\\fixture_bundle\\data.alternate.win",label,sizeof label);
  if(strcmp(label,"fixture_bundle")){
    fprintf(stderr,"Windows-style generic payload save label mismatch: %s\n",label);
    return 1;
  }
  anygm_content_save_label("/library/fixture_bundle.zip",label,sizeof label);
  if(strcmp(label,"fixture_bundle")){
    fprintf(stderr,"container save label mismatch: %s\n",label);
    return 1;
  }
  /* One resolved content path keeps one writable namespace under either separator spelling;
   * a different path retains a separate namespace. */
  if(anygm_content_path_hash("D:\\library\\fixture_bundle\\data.win")!=
     anygm_content_path_hash("D:/library/fixture_bundle/data.win")){
    fprintf(stderr,"equivalent paths produced different hashes\n");
    return 1;
  }
  if(anygm_content_path_hash("D:/library/fixture_bundle/data.win")==
     anygm_content_path_hash("D:/library/other_bundle/data.win")){
    fprintf(stderr,"different paths produced the same hash\n");
    return 1;
  }

  AnygmSyntheticContent fixture;
  if(!anygm_synthetic_content_create(&fixture)) return 1;

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  AnygmEngine *first=NULL,*second=NULL;
  if(anygm_create(&services,&first)!=ANYGM_OK ||
     anygm_create(&services,&second)!=ANYGM_OK){
    fprintf(stderr,"engine creation failed\n");
    return 1;
  }

  AnygmContentSource source={0};
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=fixture.path;
  source.cache_directory=fixture.directory;
  source.save_directory=fixture.directory;
  if(anygm_load(first,&source,NULL)!=ANYGM_OK || anygm_load(second,&source,NULL)!=ANYGM_OK){
    char error[512]={0};
    anygm_get_last_error(first,error,sizeof error);
    fprintf(stderr,"engine load failed: %s\n",error);
    return 1;
  }
  const char *fixture_name=strrchr(fixture.directory,'/');
  fixture_name=fixture_name?fixture_name+1:fixture.directory;
  char save_path[384];
  snprintf(save_path,sizeof save_path,"%s/anygm/%s-%08x",fixture.directory,fixture_name,
           anygm_content_path_hash(fixture.path));
  AnygmFileInfo save_info={0};
  save_info.struct_size=sizeof save_info;
  if(services.file_stat(services.userdata,save_path,&save_info)!=ANYGM_OK ||
     !(save_info.flags&ANYGM_FILE_INFO_DIRECTORY)){
    fprintf(stderr,"content save namespace was not created: %s\n",save_path);
    return 1;
  }

  /* The boot-time state is the one state with no completed frame in it, and it is exactly what a
   * frontend measures when it sizes a rewind ring once, at load. The advised capacity taken at
   * that moment has to cover the states every later frame produces, or the ring silently stops
   * recording the run the moment the first frame presents. */
  size_t boot_state_size=anygm_state_size(first);
  size_t boot_capacity_hint=anygm_state_capacity_hint(first);
  if(!boot_state_size || !boot_capacity_hint){
    fprintf(stderr,"boot state size or capacity hint answered zero\n");
    return 1;
  }

  AnygmInputFrame input={0};
  input.struct_size=sizeof input;
  input.pointer_x=input.pointer_y=-1;
  AnygmFrameOutput first_output={0},second_output={0};
  first_output.struct_size=sizeof first_output;
  second_output.struct_size=sizeof second_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     anygm_run_frame(second,&input,&second_output)!=ANYGM_OK ||
     !first_output.pixels || !second_output.pixels || first_output.pixels==second_output.pixels){
    fprintf(stderr,"interleaved frame ownership failed\n");
    return 1;
  }

  if(anygm_state_size(first)>boot_state_size+boot_capacity_hint){
    fprintf(stderr,"a presented state (%zu) outgrew the boot-time answer (%zu + hint %zu)\n",
            anygm_state_size(first),boot_state_size,boot_capacity_hint);
    return 1;
  }

  uint8_t *first_state=NULL,*second_state=NULL;
  size_t first_written=0,second_written=0;
  if(!save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     first_written!=second_written || memcmp(first_state,second_state,first_written)){
    fprintf(stderr,"equal interleaved engine states diverged\n");
    return 1;
  }
  free(first_state); free(second_state);

  first_output.struct_size=sizeof first_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     !save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     (first_written==second_written && !memcmp(first_state,second_state,first_written))){
    fprintf(stderr,"independently advanced engine state was not isolated\n");
    return 1;
  }
  free(first_state); free(second_state);

  second_output.struct_size=sizeof second_output;
  if(anygm_run_frame(second,&input,&second_output)!=ANYGM_OK ||
     !save_state(first,&first_state,&first_written) ||
     !save_state(second,&second_state,&second_written) ||
     first_written!=second_written || memcmp(first_state,second_state,first_written)){
    fprintf(stderr,"resynchronized engine states diverged\n");
    return 1;
  }

  uint8_t *deterministic=NULL;
  size_t deterministic_size=0;
  if(!save_state(first,&deterministic,&deterministic_size) ||
     deterministic_size!=first_written || memcmp(deterministic,first_state,first_written)){
    fprintf(stderr,"repeated serialization was not deterministic\n");
    return 1;
  }
  /* The state carries content and compatibility fingerprints. The synthetic content embeds the
   * producer fingerprint, so this hash moves whenever reviewed producer behavior or policy changes,
   * and again whenever the serialized layout itself changes. */
  /* Schema 13 widened the serialized pad rows from one to one per port, which is exactly
   * 3 further ports x NPAD buttons x (held + previous) = 96 bytes and nothing else: the size
   * moved from 21954 to 22050 by precisely that, which is what says the layout change was the
   * intended one. */
  uint64_t deterministic_hash=state_checksum(deterministic,deterministic_size);
  if(deterministic_size!=22050 ||
     deterministic_hash!=UINT64_C(0x266e90d82d8ea492)){
    fprintf(stderr,"canonical engine state changed: size=%zu hash=%016llx\n",
            deterministic_size,(unsigned long long)deterministic_hash);
    return 1;
  }
  free(deterministic);

  uint8_t *damaged=malloc(first_written);
  if(!damaged) return 1;
  memcpy(damaged,first_state,first_written);
  write_u32(damaged+4,ANYGM_STATE_SCHEMA-1u);
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "older state schema")) return 1;
  if(!expect_rejected_unchanged(first,first_state,first_written-1,first_state,first_written,
                                "truncated state")) return 1;
  memcpy(damaged,first_state,first_written);
  damaged[112]^=1;
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "payload checksum mismatch")) return 1;

  memcpy(damaged,first_state,first_written);
  memset(damaged+112,0,1024);
  memcpy(damaged+112,"../data.win",11);
  write_u64(damaged+56,state_checksum(damaged+112,(size_t)read_u64(damaged+96)));
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "unsafe content locator")) return 1;

  memcpy(damaged,first_state,first_written);
  memset(damaged+112,0,1024);
  memcpy(damaged+112,"missing/data.win",16);
  write_u64(damaged+56,state_checksum(damaged+112,(size_t)read_u64(damaged+96)));
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "unavailable replacement content")) return 1;

  memcpy(damaged,first_state,first_written);
  uint64_t core_size=read_u64(damaged+64);
  uint64_t render_size=read_u64(damaged+72);
  uint64_t payload_size=read_u64(damaged+96);
  size_t frame_width_offset=112u+2048u+44u+
    sizeof first->pad_current+sizeof first->pad_previous+
    sizeof first->key_current+sizeof first->key_previous;
  if(frame_width_offset+12>112u+core_size) return 1;
  write_u32(damaged+frame_width_offset,UINT32_MAX);
  write_u64(damaged+56,state_checksum(damaged+112,(size_t)payload_size));
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "invalid completed frame")) return 1;

  memcpy(damaged,first_state,first_written);
  uint64_t vm_offset=112+core_size+render_size;
  if(vm_offset+12>first_written || payload_size>first_written-112) return 1;
  write_u32(damaged+(size_t)vm_offset+8,UINT32_MAX);
  write_u64(damaged+56,state_checksum(damaged+112,(size_t)payload_size));
  if(!expect_rejected_unchanged(first,damaged,first_written,first_state,first_written,
                                "invalid VM section")) return 1;
  free(damaged);

  first_output.struct_size=sizeof first_output;
  AnygmResult roundtrip_run=anygm_run_frame(first,&input,&first_output);
  AnygmResult roundtrip_load=roundtrip_run==ANYGM_OK?
    anygm_state_load(first,first_state,first_written):ANYGM_ERROR_INVALID_STATE;
  int roundtrip_save=roundtrip_load==ANYGM_OK?
    save_state(first,&deterministic,&deterministic_size):0;
  if(roundtrip_run!=ANYGM_OK || roundtrip_load!=ANYGM_OK || !roundtrip_save ||
     deterministic_size!=first_written || memcmp(deterministic,first_state,first_written)){
    if(deterministic)
      for(size_t index=112;index<(deterministic_size<first_written?
                              deterministic_size:first_written);index++)
        if(deterministic[index]!=first_state[index]){
          fprintf(stderr,"first state difference at %zu: %02x != %02x\n",index,
                  deterministic[index],first_state[index]);
          break;
        }
    if(deterministic && deterministic_size>=112 && first_written>=112)
      fprintf(stderr,"sections restored=%llu/%llu/%llu expected=%llu/%llu/%llu\n",
              (unsigned long long)read_u64(deterministic+64),
              (unsigned long long)read_u64(deterministic+72),
              (unsigned long long)read_u64(deterministic+80),
              (unsigned long long)read_u64(first_state+64),
              (unsigned long long)read_u64(first_state+72),
              (unsigned long long)read_u64(first_state+80));
    fprintf(stderr,"state roundtrip did not restore exact serialized state: run=%d load=%d "
                   "save=%d size=%zu expected=%zu\n",roundtrip_run,roundtrip_load,
            roundtrip_save,deterministic_size,first_written);
    return 1;
  }
  free(deterministic);

  /* A key held across a load boundary is a continuation, not a new press. The fixture counts
   * keyboard_check_pressed edges into a global, so the uninterrupted run and a run restored
   * mid-hold must serialize identically: on the defective core the load cleared the
   * edge-detection buffers and the held key minted one extra press on the first resumed frame. */
  {
    AnygmInputFrame held=input;
    held.keys[ANYGM_KEY_z]=1;
    AnygmFrameOutput held_output={0};
    held_output.struct_size=sizeof held_output;
    if(anygm_run_frame(first,&held,&held_output)!=ANYGM_OK ||
       anygm_run_frame(first,&held,&held_output)!=ANYGM_OK){
      fprintf(stderr,"held-key frames failed\n");
      return 1;
    }
    uint8_t *saved=NULL; size_t saved_size=0;
    if(!save_state(first,&saved,&saved_size)) return 1;
    if(anygm_run_frame(first,&held,&held_output)!=ANYGM_OK ||
       anygm_run_frame(first,&held,&held_output)!=ANYGM_OK){
      fprintf(stderr,"uninterrupted held frames failed\n");
      return 1;
    }
    uint8_t *uninterrupted=NULL; size_t uninterrupted_size=0;
    if(!save_state(first,&uninterrupted,&uninterrupted_size)) return 1;
    if(anygm_state_load(first,saved,saved_size)!=ANYGM_OK ||
       anygm_run_frame(first,&held,&held_output)!=ANYGM_OK || /* presentation-only pass */
       anygm_run_frame(first,&held,&held_output)!=ANYGM_OK ||
       anygm_run_frame(first,&held,&held_output)!=ANYGM_OK){
      fprintf(stderr,"restored held frames failed\n");
      return 1;
    }
    uint8_t *restored=NULL; size_t restored_size=0;
    if(!save_state(first,&restored,&restored_size)) return 1;
    if(restored_size!=uninterrupted_size ||
       memcmp(restored,uninterrupted,restored_size)){
      size_t limit=restored_size<uninterrupted_size?restored_size:uninterrupted_size;
      size_t at=112; while(at<limit && restored[at]==uninterrupted[at]) at++;
      fprintf(stderr,"a run restored mid-hold diverged from the uninterrupted one "
                     "(sizes %zu vs %zu, first difference at %zu)\n",
              restored_size,uninterrupted_size,at);
      return 1;
    }
    free(saved); free(uninterrupted); free(restored);
  }

  /* The frame slot's cost must not scale with the presentation canvas: an upscale manufactures
   * repeated rows and the row table removes them, so a monitor-sized state stays within the same
   * order as the native one instead of carrying megabytes of magnified pixels. */
  {
    uint8_t *native_state=NULL; size_t native_size=0;
    if(!save_state(second,&native_state,&native_size)) return 1;
    AnygmConfigDelta monitor={0};
    monitor.struct_size=sizeof monitor;
    monitor.fields=ANYGM_CONFIG_MONITOR_WIDTH|ANYGM_CONFIG_MONITOR_HEIGHT;
    monitor.values.struct_size=sizeof monitor.values;
    monitor.values.monitor_width=1920;
    monitor.values.monitor_height=1080;
    AnygmFrameOutput monitor_output={0};
    monitor_output.struct_size=sizeof monitor_output;
    if(anygm_set_config(second,&monitor)!=ANYGM_OK ||
       anygm_run_frame(second,&input,&monitor_output)!=ANYGM_OK){
      fprintf(stderr,"monitor-sized frame failed\n");
      return 1;
    }
    uint8_t *monitor_state=NULL; size_t monitor_size=0;
    if(!save_state(second,&monitor_state,&monitor_size)) return 1;
    if(monitor_size>native_size*4+65536){
      fprintf(stderr,"a monitor-sized state ballooned: %zu bytes against %zu native\n",
              monitor_size,native_size);
      return 1;
    }
    free(native_state); free(monitor_state);
  }

  /* A completed frame is observable state in its own right. Post Draw may remove a transient that
   * was visible in that frame, so loading the post-frame simulation and executing Draw again is
   * not an equivalent reconstruction. Preserve the pixels and present them once before advancing. */
  uint32_t retained_pixel=0x0013579Bu;
  first->screen[0]=retained_pixel;
  uint8_t *display_state=NULL;
  size_t display_state_size=0;
  if(!save_state(first,&display_state,&display_state_size)) return 1;
  first_output.struct_size=sizeof first_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK ||
     anygm_state_load(first,display_state,display_state_size)!=ANYGM_OK){
    fprintf(stderr,"completed-frame state setup failed\n");
    return 1;
  }
  first_output.struct_size=sizeof first_output;
  if(anygm_run_frame(first,&input,&first_output)!=ANYGM_OK || !first_output.pixels ||
     (((const uint32_t *)first_output.pixels)[0]&0x00FFFFFFu)!=retained_pixel){
    fprintf(stderr,"loaded state did not present its completed frame\n");
    return 1;
  }
  free(display_state);

  free(first_state); free(second_state);
  anygm_destroy(first);
  anygm_destroy(second);
  anygm_synthetic_content_destroy(&fixture);
  puts("independent engine instances: ok");
  return 0;
}
