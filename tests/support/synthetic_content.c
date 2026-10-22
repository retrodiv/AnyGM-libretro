/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "synthetic_content.h"

#include "gmlc_package.h"
#include "gmlc_project.h"
#include "stdio_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define anygm_test_unlink _unlink
#define anygm_test_rmdir _rmdir
#define anygm_test_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define anygm_test_unlink unlink
#define anygm_test_rmdir rmdir
#define anygm_test_mkdir(path) mkdir((path),0700)
#endif

static int create_fixture_directory(char *path,size_t path_size){
#ifdef _WIN32
  for(unsigned attempt=0;attempt<256;attempt++){
    snprintf(path,path_size,"build/synthetic-content-%ld-%u",(long)_getpid(),attempt);
    if(_mkdir(path)==0) return 1;
    if(errno!=EEXIST) return 0;
  }
  return 0;
#else
  snprintf(path,path_size,"build/synthetic-content-XXXXXX");
  return mkdtemp(path)!=NULL;
#endif
}

static int write_text(const char *path,const char *text){
  FILE *file=fopen(path,"wb");
  size_t size=strlen(text);
  int ok=file && fwrite(text,1,size,file)==size;
  if(file && fclose(file)!=0) ok=0;
  return ok;
}

static unsigned synthetic_path_hash(const char *path){
  uint32_t hash=2166136261u;
  for(;path&&*path;path++){
    hash^=(uint8_t)*path;
    hash*=16777619u;
  }
  return hash;
}

static int synthetic_content_create(
    AnygmSyntheticContent *fixture,int anchor_script,int list_fixture,unsigned width,unsigned height){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char startup[192],create[192],step[192],anchor_call[192],draw[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(anchor_call,sizeof anchor_call,"%s/anchor-call.gml",fixture->directory);
  snprintf(draw,sizeof draw,"%s/draw.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  const char *startup_text=list_fixture?
    "global.fixture_counter = 0;\n"
    "global.fixture_presses = 0;\n"
    "global.fixture_global_list = ds_list_create();\n"
    "ds_list_add(global.fixture_global_list, 0, 0);\n"
    "randomize();\n"
    "global.fixture_datetime = date_current_datetime();\n":
    "global.fixture_counter = 0;\n"
    "global.fixture_presses = 0;\n"
    "randomize();\n"
    "global.fixture_datetime = date_current_datetime();\n";
  const char *step_text=list_fixture?
    "global.fixture_counter += 1;\n"
    "if (global.fixture_counter == 2) ds_list_replace(fixture_lists[0], 1, 7);\n"
    "if (global.fixture_counter == 2) ds_list_replace(global.fixture_global_list, 1, 8);\n"
    "global.fixture_clock = current_time;\n"
    "global.fixture_list_value = ds_list_find_value(fixture_lists[0], 1);\n"
    "global.fixture_global_list_value = ds_list_find_value(global.fixture_global_list, 1);\n"
    "if (keyboard_check_pressed(vk_anykey)) global.fixture_presses += 1;\n":
    "global.fixture_counter += 1;\n"
    "global.fixture_clock = current_time;\n"
    "if (keyboard_check_pressed(vk_anykey)) global.fixture_presses += 1;\n";
  if(!write_text(startup,startup_text) ||
     (list_fixture && !write_text(create,
                  "fixture_list = ds_list_create();\n"
                  "ds_list_add(fixture_list, 0, 0);\n"
                  "fixture_lists[0] = fixture_list;\n")) ||
     !write_text(step,step_text) ||
     (anchor_script && !write_text(anchor_call,anchor_script==2?
                  "global.fixture_anchor_calls += 1;\n"
                  "global.fixture_after_arg = argument0;\n"
                  "global.fixture_after_self = id;\n"
                  "if (global.fixture_redraw) screen_redraw();\n"
                  "draw_set_color(255);\n"
                  "draw_rectangle(argument0,16,argument0+7,23,0);\n":
                  "global.fixture_anchor_calls += 1;\n")) ||
     (anchor_script==2 && !write_text(draw,
                  "draw_set_color(16777215); draw_rectangle(0,0,63,47,0);\n"
                  "draw_set_color(65280); draw_rectangle(56,0,63,7,0);\n"
                  "if (global.fixture_hide) visible = 0.25;\n"
                  "if (global.fixture_destroy) instance_destroy();\n"))){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[2]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"neutral-engine-fixture";
  project.classic_version=800;
  project.classic_scaling=0;
  project.startup_code_path=startup;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order;
  project.n_room_order=1;
  GmlcScript script={0};
  if(anchor_script){
    script.id=script.name=(char *)"anchor_call";
    script.source_path=anchor_call;
    project.scripts=&script;
    project.n_scripts=project.cap_scripts=1;
  }

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=anchor_script==2;
  object.events=events;
  object.n_events=object.cap_events=(list_fixture || anchor_script==2)?2:1;
  if(list_fixture){
    events[0].event_type=0;
    events[0].event_number=0;
    events[0].source_path=create;
  }
  events[list_fixture?1:0].event_type=3;
  events[list_fixture?1:0].event_number=0;
  events[list_fixture?1:0].source_path=step;
  if(anchor_script==2){
    events[1].event_type=8;
    events[1].event_number=0;
    events[1].source_path=draw;
  }

  room.id=room.name=(char *)"room_fixture";
  room.width=width;
  room.height=height;
  room.speed=60;
  room.draw_background_color=1;
  room.instances=&instance;
  room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}
int anygm_synthetic_content_create(AnygmSyntheticContent *fixture){
  return synthetic_content_create(fixture,0,0,64,48);
}
int anygm_synthetic_sized_content_create(AnygmSyntheticContent *fixture,unsigned width,unsigned height){
  if(!width || !height || width>3840 || height>2160) return 0;
  return synthetic_content_create(fixture,0,0,width,height);
}
int anygm_synthetic_list_override_content_create(AnygmSyntheticContent *fixture){
  return synthetic_content_create(fixture,0,1,64,48);
}
int anygm_synthetic_anchor_script_content_create(AnygmSyntheticContent *fixture){
  return synthetic_content_create(fixture,1,0,64,48);
}
int anygm_synthetic_after_draw_content_create(AnygmSyntheticContent *fixture){
  return synthetic_content_create(fixture,2,0,64,48);
}
int anygm_synthetic_after_draw_anchor_content_create(AnygmSyntheticContent *fixture){
  if(!anygm_synthetic_after_draw_content_create(fixture)) return 0;
  snprintf(fixture->path,sizeof fixture->path,"%s/draw-policy.anygm",fixture->directory);
  if(!write_text(fixture->path,
     "[anygm]\npayload=data.win\n[overrides]\n"
     "?gameres afterdraw|obj_fixture|anchor_call|16\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_tilemap_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  char startup[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  if(!write_text(startup,"exit;\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services; services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcRoom rooms[2]={0}; GmlcRoomLayer layers[2]={0};
  uint32_t cells[4]={0}; int order[]={0,1};
  GmlcProject project={0};
  project.host=&services; project.name=(char*)"neutral-tilemap-state";
  project.startup_code_path=startup;
  project.rooms=rooms; project.n_rooms=project.cap_rooms=2;
  project.room_order=order; project.n_room_order=2;
  rooms[0].id=rooms[0].name=(char*)"room_maps";
  rooms[1].id=rooms[1].name=(char*)"room_empty";
  for(int i=0;i<2;i++){
    rooms[i].width=1280; rooms[i].height=960; rooms[i].speed=60;
    rooms[i].draw_background_color=1;
    layers[i].id=layers[i].name=i?(char*)"map_second":(char*)"map_first";
    layers[i].type=4; layers[i].layer_id=10+i; layers[i].depth=17+i;
    layers[i].visible=1; layers[i].x=11; layers[i].y=13;
    layers[i].tile_tileset_id=-1; layers[i].tile_cols=layers[i].tile_rows=2;
    layers[i].tile_data=cells;
  }
  rooms[0].layers=layers; rooms[0].n_layers=rooms[0].cap_layers=2;
  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic tilemap package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  return 1;
}

int anygm_synthetic_cold_room_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  char startup[192],create[192],first[192],second[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(create,sizeof create,"%s/anchor-call.gml",fixture->directory);
  snprintf(first,sizeof first,"%s/step.gml",fixture->directory);
  snprintf(second,sizeof second,"%s/draw.gml",fixture->directory);
  if(!write_text(startup,"global.fixture_boot_visits=0; global.fixture_target_visits=0;\n") ||
     !write_text(create,"global.fixture_boot_marker=37;\n") ||
     !write_text(first,"global.fixture_boot_visits+=1;\n") ||
     !write_text(second,"global.fixture_target_visits+=1;\n"
                         "global.fixture_target_saw_boot=variable_global_exists(\"fixture_boot_marker\");\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services; services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcProject project={0}; GmlcRoom rooms[2]={0}; int order[]={0,1};
  GmlcObject object={0}; GmlcObjectEvent event={0}; GmlcRoomInstance instance={0};
  project.host=&services; project.name=(char*)"neutral-cold-room-entry";
  project.startup_code_path=startup;
  project.rooms=rooms; project.n_rooms=project.cap_rooms=2;
  project.room_order=order; project.n_room_order=2;
  project.objects=&object; project.n_objects=project.cap_objects=1;
  object.id=object.name=(char*)"obj_boot_controller";
  object.sprite_id=object.mask_id=object.parent_id=-1; object.persistent=1;
  object.events=&event; object.n_events=object.cap_events=1;
  event.event_type=0; event.source_path=create;
  rooms[0].id=rooms[0].name=(char*)"room_boot"; rooms[0].creation_code_path=first;
  rooms[1].id=rooms[1].name=(char*)"room_target"; rooms[1].creation_code_path=second;
  for(int i=0;i<2;i++){
    rooms[i].width=64; rooms[i].height=48; rooms[i].speed=60;
    rooms[i].draw_background_color=1;
  }
  rooms[0].instances=&instance; rooms[0].n_instances=rooms[0].cap_instances=1;
  instance.id=instance.name=(char*)"instance_boot_controller";
  instance.object_id=0; instance.instance_id=100000;
  instance.sx=instance.sy=1; instance.color=0xFFFFFFFFu;
  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic cold-room package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  return 1;
}

static int synthetic_classic_present_content_create(AnygmSyntheticContent *fixture,int compositing){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char startup[192],create[192],step[192],endstep[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(endstep,sizeof endstep,"%s/endstep.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  /* The synthetic compositor blits a 64x48 surface at scale 2 into the room's authored
   * 128x96 port. */
  if(!write_text(startup,"global.fixture_frames = 0;\n") ||
     !write_text(create,
                 compositing ? "canvas_scale = 2;\ncanvas = surface_create(64,48);\n"
                             : "canvas_scale = 2;\n") ||
     !write_text(step,
                 compositing
                   ? "global.fixture_frames += 1;\n"
                     "if (surface_exists(canvas)) {\n"
                     "  surface_reset_target();\n"
                     "  draw_clear(0);\n"
                     "  draw_surface_stretched(canvas, 0, 0, 64*canvas_scale, 48*canvas_scale);\n"
                     "}\n"
                   : "global.fixture_frames += 1;\n") ||
     !write_text(endstep,
                 compositing ? "if (surface_exists(canvas)) surface_set_target(canvas);\n"
                             : "\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[3]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"classic-present-fixture";
  project.classic_version=800;
  project.classic_scaling=0;
  project.startup_code_path=startup;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order;
  project.n_room_order=1;

  object.id=object.name=(char *)"obj_presenter";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0;
  object.events=events;
  object.n_events=object.cap_events=3;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=3; events[1].event_number=0; events[1].source_path=step;
  events[2].event_type=3; events[2].event_number=2; events[2].source_path=endstep;

  room.id=room.name=(char *)"room_presented";
  room.width=64;
  room.height=48;
  room.speed=60;
  room.draw_background_color=1;
  room.view_enabled=1;
  room.n_views=1;
  room.views[0].visible=1;
  room.views[0].wview=64;
  room.views[0].hview=48;
  room.views[0].wport=128;
  room.views[0].hport=96;
  room.views[0].hspeed=room.views[0].vspeed=-1;
  room.views[0].object_id=-1;
  room.instances=&instance;
  room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_presenter";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic classic present package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}
int anygm_synthetic_classic_compositor_content_create(AnygmSyntheticContent *fixture){
  return synthetic_classic_present_content_create(fixture,1);
}
int anygm_synthetic_classic_plain_content_create(AnygmSyntheticContent *fixture){
  return synthetic_classic_present_content_create(fixture,0);
}

int anygm_synthetic_scoped_override_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char startup[192],create[192],step[192],draw[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(draw,sizeof draw,"%s/draw.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  /* A band across the top rather than a flat field: a picture whose rows are all alike cannot show
   * a one-row presentation shift, so a case asserting one against it could never fail. */
  /* Two synthetic markers expose the draw-phase and step-phase readings
   * of the same global in one frame. */
  if(!write_text(startup,
                 "global.advance = 0;\n"
                 "global.overlay_like = 1;\n"
                 "global.step_saw = -1;\n") ||
     !write_text(create,"scale = 3;\n") ||
     !write_text(draw,
                 "draw_set_color(16777215);\n"
                 "draw_rectangle(0,0,63,7,0);\n"
                 "if (global.overlay_like == 0)"
                 " { draw_set_color(255); draw_rectangle(0,0,7,7,0); }\n"
                 "if (global.step_saw == 1)"
                 " { draw_set_color(65280); draw_rectangle(56,0,63,7,0); }\n") ||
     !write_text(step,
                 "global.step_saw = global.overlay_like;\n"
                 "if (global.advance == 1) { global.advance = 0; room_goto_next(); }\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[3]={0};
  GmlcRoom rooms[2]={0};
  GmlcRoomInstance instance={0};
  int room_order[2]={0,1};
  project.name=(char *)"scoped-override-fixture";
  project.classic_version=800;
  project.classic_scaling=0;
  project.startup_code_path=startup;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;

  object.id=object.name=(char *)"obj_canvas";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=1;
  object.events=events;
  object.n_events=object.cap_events=3;
  events[0].event_type=0;
  events[0].event_number=0;
  events[0].source_path=create;
  events[1].event_type=3;
  events[1].event_number=0;
  events[1].source_path=step;
  events[2].event_type=8;
  events[2].event_number=0;
  events[2].source_path=draw;

  instance.id=instance.name=(char *)"instance_canvas";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  static const int port_width[2]={128,192};
  static const int port_height[2]={96,144};
  static char *const room_name[2]={(char *)"room_near",(char *)"room_far"};
  for(int room=0;room<2;room++){
    rooms[room].id=rooms[room].name=room_name[room];
    rooms[room].width=64;
    rooms[room].height=48;
    rooms[room].speed=60;
    rooms[room].draw_background_color=1;
    rooms[room].view_enabled=1;
    rooms[room].n_views=1;
    rooms[room].views[0].visible=1;
    rooms[room].views[0].wview=64;
    rooms[room].views[0].hview=48;
    rooms[room].views[0].wport=port_width[room];
    rooms[room].views[0].hport=port_height[room];
    rooms[room].views[0].hspeed=rooms[room].views[0].vspeed=-1;
    rooms[room].views[0].object_id=-1;
  }
  /* Only the first room carries the instance: the override has to survive a room change, not the
   * object that happened to set the value. */
  rooms[0].instances=&instance;
  rooms[0].n_instances=rooms[0].cap_instances=1;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic scoped-override package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_alarm_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192],alarm[192];
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(alarm,sizeof alarm,"%s/alarm1.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,
                 "global.fixture_alarm_ticks = 0;\n"
                 "alarm[1] = 60;\n") ||
     !write_text(alarm,
                 "global.fixture_alarm_ticks += 1;\n"
                 "alarm[1] = 60;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[2]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"alarm-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0; object.events=events; object.n_events=object.cap_events=2;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=2; events[1].event_number=1; events[1].source_path=alarm;
  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1; room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1.0f; instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"alarm package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_alarm_phase_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char spawner_create[192],spawner_begin[192],spawner_alarm[192];
  char hole_create[192],keeper_create[192],late_create[192],late_alarm[192];
  snprintf(spawner_create,sizeof spawner_create,"%s/spawner_create.gml",fixture->directory);
  snprintf(spawner_begin,sizeof spawner_begin,"%s/spawner_begin.gml",fixture->directory);
  snprintf(spawner_alarm,sizeof spawner_alarm,"%s/spawner_alarm.gml",fixture->directory);
  snprintf(hole_create,sizeof hole_create,"%s/hole_create.gml",fixture->directory);
  snprintf(keeper_create,sizeof keeper_create,"%s/keeper_create.gml",fixture->directory);
  snprintf(late_create,sizeof late_create,"%s/late_create.gml",fixture->directory);
  snprintf(late_alarm,sizeof late_alarm,"%s/late_alarm.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(spawner_create,
                 "global.fixture_frame = 0;\n"
                 "global.fixture_spawned_frame = -1;\n"
                 "global.fixture_late_alarm_frame = -1;\n"
                 "alarm[1] = 3;\n") ||
     /* Begin Step runs before alarms, so this number is the frame the alarm phase belongs to. */
     !write_text(spawner_begin,
                 "global.fixture_frame += 1;\n") ||
     /* Object 2 is obj_late: a structural package numbers objects in declaration order, and a
      * literal keeps the fixture independent of how a resource name resolves. */
     !write_text(spawner_alarm,
                 "global.fixture_spawned_frame = global.fixture_frame;\n"
                 "instance_create(0, 0, 2);\n") ||
     /* The hole. Destroying it during room entry frees a pool slot above the spawner's, which the
      * allocator hands to the next instance created inside a step - and that is the arrangement
      * that lets a phase bounded by an index reach an instance created after it started. */
     !write_text(hole_create,
                 "instance_destroy();\n") ||
     /* Placed after the hole so the freed slot is never the last one: a trailing dead slot is
      * trimmed off the pool instead of being kept as a hole, and then there is nothing to reuse. */
     !write_text(keeper_create,
                 "global.fixture_keeper = 1;\n") ||
     !write_text(late_create,
                 "alarm[0] = 1;\n") ||
     !write_text(late_alarm,
                 "global.fixture_late_alarm_frame = global.fixture_frame;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject objects[4]={{0},{0},{0},{0}};
  GmlcObjectEvent spawner_events[3]={0};
  GmlcObjectEvent hole_events[1]={0};
  GmlcObjectEvent keeper_events[1]={0};
  GmlcObjectEvent late_events[2]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instances[3]={{0},{0},{0}};
  int room_order=0;
  project.name=(char *)"alarm-phase-fixture";
  project.objects=objects; project.n_objects=project.cap_objects=4;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  objects[0].id=objects[0].name=(char *)"obj_spawner";
  objects[0].sprite_id=objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=0;
  objects[0].events=spawner_events;
  objects[0].n_events=objects[0].cap_events=3;
  spawner_events[0].event_type=0; spawner_events[0].event_number=0;
  spawner_events[0].source_path=spawner_create;
  spawner_events[1].event_type=3; spawner_events[1].event_number=1;
  spawner_events[1].source_path=spawner_begin;
  spawner_events[2].event_type=2; spawner_events[2].event_number=1;
  spawner_events[2].source_path=spawner_alarm;

  objects[1].id=objects[1].name=(char *)"obj_hole";
  objects[1].sprite_id=objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=0;
  objects[1].events=hole_events;
  objects[1].n_events=objects[1].cap_events=1;
  hole_events[0].event_type=0; hole_events[0].event_number=0;
  hole_events[0].source_path=hole_create;

  objects[3].id=objects[3].name=(char *)"obj_keeper";
  objects[3].sprite_id=objects[3].mask_id=objects[3].parent_id=-1;
  objects[3].visible=0;
  objects[3].events=keeper_events;
  objects[3].n_events=objects[3].cap_events=1;
  keeper_events[0].event_type=0; keeper_events[0].event_number=0;
  keeper_events[0].source_path=keeper_create;

  objects[2].id=objects[2].name=(char *)"obj_late";
  objects[2].sprite_id=objects[2].mask_id=objects[2].parent_id=-1;
  objects[2].visible=0;
  objects[2].events=late_events;
  objects[2].n_events=objects[2].cap_events=2;
  late_events[0].event_type=0; late_events[0].event_number=0;
  late_events[0].source_path=late_create;
  late_events[1].event_type=2; late_events[1].event_number=0;
  late_events[1].source_path=late_alarm;

  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1;
  room.instances=instances; room.n_instances=room.cap_instances=3;
  instances[0].id=instances[0].name=(char *)"instance_spawner";
  instances[0].object_id=0; instances[0].instance_id=100000;
  instances[0].sx=instances[0].sy=1.0f; instances[0].color=0xFFFFFFFFu;
  instances[1].id=instances[1].name=(char *)"instance_hole";
  instances[1].object_id=1; instances[1].instance_id=100001;
  instances[1].sx=instances[1].sy=1.0f; instances[1].color=0xFFFFFFFFu;
  instances[2].id=instances[2].name=(char *)"instance_keeper";
  instances[2].object_id=3; instances[2].instance_id=100002;
  instances[2].sx=instances[2].sy=1.0f; instances[2].color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"alarm-phase package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_mouse_order_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char boot[192],press[192],held[192];
  snprintf(boot,sizeof boot,"%s/mouse_boot.gml",fixture->directory);
  snprintf(press,sizeof press,"%s/mouse_press.gml",fixture->directory);
  snprintf(held,sizeof held,"%s/mouse_held.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(boot,
                 "global.fixture_order_seq = 1;\n"
                 "global.fixture_press_order = 0;\n"
                 "global.fixture_held_order = 0;\n") ||
     !write_text(press,
                 "if (global.fixture_press_order == 0)\n"
                 "{\n"
                 "    global.fixture_press_order = global.fixture_order_seq;\n"
                 "    global.fixture_order_seq += 1;\n"
                 "}\n") ||
     !write_text(held,
                 "if (global.fixture_held_order == 0)\n"
                 "{\n"
                 "    global.fixture_held_order = global.fixture_order_seq;\n"
                 "    global.fixture_order_seq += 1;\n"
                 "}\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject objects[2]={{0},{0}};
  GmlcObjectEvent press_events[2]={0};
  GmlcObjectEvent held_events[1]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instances[2]={{0},{0}};
  int room_order=0;
  project.name=(char *)"mouse-order-fixture";
  project.objects=objects; project.n_objects=project.cap_objects=2;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  /* The global-press responder is placed first, so instance order and subtype order disagree:
   * subtype 50 (global button held) precedes subtype 53 (global button pressed), while this
   * instance precedes the other one. */
  objects[0].id=objects[0].name=(char *)"obj_press_watcher";
  objects[0].sprite_id=objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=0;
  objects[0].events=press_events;
  objects[0].n_events=objects[0].cap_events=2;
  press_events[0].event_type=0; press_events[0].event_number=0;
  press_events[0].source_path=boot;
  press_events[1].event_type=6; press_events[1].event_number=53;
  press_events[1].source_path=press;

  objects[1].id=objects[1].name=(char *)"obj_held_watcher";
  objects[1].sprite_id=objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=0;
  objects[1].events=held_events;
  objects[1].n_events=objects[1].cap_events=1;
  held_events[0].event_type=6; held_events[0].event_number=50;
  held_events[0].source_path=held;

  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1;
  room.instances=instances; room.n_instances=room.cap_instances=2;
  instances[0].id=instances[0].name=(char *)"instance_press";
  instances[0].object_id=0; instances[0].instance_id=100000;
  instances[0].sx=instances[0].sy=1.0f; instances[0].color=0xFFFFFFFFu;
  instances[1].id=instances[1].name=(char *)"instance_held";
  instances[1].object_id=1; instances[1].instance_id=100001;
  instances[1].sx=instances[1].sy=1.0f; instances[1].color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"mouse-order package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_extension_init_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char init[192],create[192];
  snprintf(init,sizeof init,"%s/extension_init.gml",fixture->directory);
  snprintf(create,sizeof create,"%s/extension_create.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  /* Use a nonzero sentinel so an unset global cannot satisfy the fixture. */
  if(!write_text(init,
                 "global.fixture_library_handle = -4;\n") ||
     /* Record the value observed by the first room's Create event. */
     !write_text(create,
                 "global.fixture_handle_at_create = global.fixture_library_handle;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[1]={0};
  GmlcScript script={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"extension-init-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.scripts=&script; project.n_scripts=project.cap_scripts=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;
  project.extension_init_script=(char *)"library_init";

  script.id=script.name=(char *)"library_init";
  script.source_path=init;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0; object.events=events; object.n_events=object.cap_events=1;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1; room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1.0f; instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"extension-init package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_blocking_wait_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192],alarm[192],step[192];
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(alarm,sizeof alarm,"%s/alarm0.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,
                 "global.fixture_steps = 0;\n"
                 "global.fixture_release = 0;\n"
                 "global.fixture_resumed = 0;\n"
                 "alarm[0] = 1;\n") ||
     !write_text(alarm,
                 "var saved_text = keyboard_string;\n"
                 "var reflected_text = variable_global_get(\"keyboard_string\");\n"
                 "while (global.fixture_release == 0) { sleep(10); }\n"
                 "global.fixture_saved_text = saved_text;\n"
                 "global.fixture_reflected_text = reflected_text;\n"
                 "global.fixture_resumed = 1;\n") ||
     !write_text(step,
                 "global.fixture_steps += 1;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[3]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"blocking-wait-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0; object.events=events; object.n_events=object.cap_events=3;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=2; events[1].event_number=0; events[1].source_path=alarm;
  events[2].event_type=3; events[2].event_number=0; events[2].source_path=step;
  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1; room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1.0f; instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"blocking wait package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_simulated_key_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192],step[192];
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,
                 "global.fixture_ticks = 0;\n"
                 "keyboard_key_press(vk_right);\n") ||
     !write_text(step,
                 "if (keyboard_check(vk_right)) global.fixture_ticks += 1;\n"
                 "if (global.fixture_ticks == 3) keyboard_key_release(vk_right);\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[2]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"simulated-key-lifetime-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0; object.events=events; object.n_events=object.cap_events=2;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=3; events[1].event_number=0; events[1].source_path=step;
  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1; room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1.0f; instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"simulated-key package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_bridged_key_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192],step[192],press[192];
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(press,sizeof press,"%s/press.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,
                 "global.fixture_steps = 0;\n"
                 "global.fixture_presses = 0;\n"
                 "global.fixture_press_step = -1;\n") ||
     !write_text(step,
                 "global.fixture_steps += 1;\n"
                 "if (global.fixture_steps == 1)\n"
                 "{\n"
                 "    keyboard_key_press(vk_right);\n"
                 "    keyboard_key_release(vk_right);\n"
                 "}\n") ||
     !write_text(press,
                 "global.fixture_presses += 1;\n"
                 "global.fixture_press_step = global.fixture_steps;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[3]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"bridged-key-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0; object.events=events; object.n_events=object.cap_events=3;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=3; events[1].event_number=0; events[1].source_path=step;
  events[2].event_type=9; events[2].event_number=39; events[2].source_path=press;
  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1; room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1.0f; instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"bridged-key package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_bridged_hold_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192],begin[192],step[192],press[192];
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(begin,sizeof begin,"%s/begin.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(press,sizeof press,"%s/press.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,
                 "global.fixture_steps = 0;\n"
                 "global.fixture_held = 0;\n"
                 "global.fixture_pressed = 0;\n") ||
     !write_text(begin,
                 "if (keyboard_check(vk_right)) global.fixture_held += 1;\n") ||
     !write_text(step,
                 "global.fixture_steps += 1;\n"
                 "if (global.fixture_steps <= 5 || global.fixture_steps == 8)\n"
                 "{\n"
                 "    keyboard_key_press(vk_right);\n"
                 "    keyboard_key_release(vk_right);\n"
                 "}\n") ||
     !write_text(press,
                 "global.fixture_pressed += 1;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[4]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"bridged-hold-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0; object.events=events; object.n_events=object.cap_events=4;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=3; events[1].event_number=1; events[1].source_path=begin;
  events[2].event_type=3; events[2].event_number=0; events[2].source_path=step;
  events[3].event_type=9; events[3].event_number=39; events[3].source_path=press;
  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1; room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1.0f; instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"bridged-hold package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_gpu_scene_route_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);

  uint8_t pixels[8u*8u*4u];
  for(int y=0;y<8;y++) for(int x=0;x<8;x++){
    size_t offset=((size_t)y*8u+(size_t)x)*4u;
    pixels[offset]=(uint8_t)(32+x*23);
    pixels[offset+1u]=(uint8_t)(48+y*20);
    pixels[offset+2u]=(uint8_t)((x+y)&1?224:96);
    pixels[offset+3u]=255u;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcProject project={0};
  GmlcSprite sprite={0};
  GmlcObject objects[2]={{0}};
  GmlcObjectEvent events[2]={{0}};
  GmlcRoom rooms[2]={{0}};
  GmlcRoomInstance instances[2]={{0}};
  int room_order[2]={0,1};
  char *frame=NULL,*moving_step=NULL,*second_step=NULL;
  project.host=&services;
  project.name=(char *)"neutral-resident-scene-route";
  project.sprites=&sprite; project.n_sprites=project.cap_sprites=1;
  project.objects=objects; project.n_objects=project.cap_objects=2;
  project.rooms=rooms; project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order; project.n_room_order=2;
  frame=gmlc_project_add_memory_file(&project,"sprite",GMLC_MEMORY_RGBA,
                                     pixels,sizeof pixels,8,8);
  moving_step=gmlc_project_add_memory_file(&project,"moving-step",GMLC_MEMORY_TEXT,
    "if (keyboard_check(vk_right)) x += 1;\n"
    "if (x >= 42) room_goto_next();\n",
    sizeof("if (keyboard_check(vk_right)) x += 1;\n"
           "if (x >= 42) room_goto_next();\n")-1u,0,0);
  second_step=gmlc_project_add_memory_file(&project,"second-step",GMLC_MEMORY_TEXT,
    "if (keyboard_check(vk_right)) x += 1;\n",
    sizeof("if (keyboard_check(vk_right)) x += 1;\n")-1u,0,0);
  if(frame && moving_step && second_step){
    sprite.id=sprite.name=(char *)"sprite_fixture";
    sprite.runtime_id=0;
    sprite.width=sprite.height=8;
    sprite.bbox_right=sprite.bbox_bottom=7;
    sprite.n_frames=1;
    sprite.frame_paths=&frame;
    for(int index=0;index<2;index++){
      objects[index].id=objects[index].name=index?(char *)"object_second":(char *)"object_first";
      objects[index].sprite_id=0;
      objects[index].mask_id=objects[index].parent_id=-1;
      objects[index].visible=1;
      objects[index].events=&events[index];
      objects[index].n_events=objects[index].cap_events=1;
      events[index].event_type=3;
      events[index].event_number=0;
      events[index].source_path=index?second_step:moving_step;
      rooms[index].id=rooms[index].name=index?(char *)"room_second":(char *)"room_first";
      rooms[index].width=128; rooms[index].height=96; rooms[index].speed=60;
      rooms[index].draw_background_color=1;
      rooms[index].background_color=index?0x00403020u:0x00102030u;
      rooms[index].view_enabled=1;
      rooms[index].n_views=1;
      rooms[index].views[0].visible=1;
      rooms[index].views[0].wview=rooms[index].views[0].wport=128;
      rooms[index].views[0].hview=rooms[index].views[0].hport=96;
      rooms[index].views[0].hspeed=rooms[index].views[0].vspeed=-1;
      rooms[index].views[0].object_id=-1;
      rooms[index].instances=&instances[index];
      rooms[index].n_instances=rooms[index].cap_instances=1;
      instances[index].id=instances[index].name=
        index?(char *)"instance_second":(char *)"instance_first";
      instances[index].object_id=index;
      instances[index].instance_id=100000+index;
      instances[index].x=index?64:16;
      instances[index].y=index?56:24;
      instances[index].sx=instances[index].sy=1.0f;
      instances[index].color=0xFFFFFFFFu;
    }
  }
  char error[256]={0};
  int ok=frame && moving_step && second_step &&
    gmlc_package_write_structural(&project,fixture->path,error,sizeof error);
  if(!ok) fprintf(stderr,"neutral scene route package failed: %s\n",error);
  for(int index=0;index<project.n_memory_files;index++) free(project.memory_files[index].data);
  free(project.memory_files);
  free(frame); free(moving_step); free(second_step);
  if(!ok) anygm_synthetic_content_destroy(fixture);
  return ok;
}

int anygm_synthetic_gpu_dense_scene_route_content_create(AnygmSyntheticContent *fixture){
  enum { SPRITE_SIDE=64, DECORATIONS=1000, ROOM_WIDTH=1280, ROOM_HEIGHT=720 };
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  uint8_t pixels[SPRITE_SIDE*SPRITE_SIDE*4u];
  for(int y=0;y<SPRITE_SIDE;y++) for(int x=0;x<SPRITE_SIDE;x++){
    size_t offset=((size_t)y*SPRITE_SIDE+(size_t)x)*4u;
    pixels[offset]=(uint8_t)(32+(x*3)%192);
    pixels[offset+1u]=(uint8_t)(48+(y*2)%160);
    pixels[offset+2u]=(uint8_t)((x+y)&1?224:96);
    pixels[offset+3u]=255u;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcProject project={0};
  GmlcSprite sprite={0};
  GmlcObject objects[3]={{0}};
  GmlcObjectEvent events[2]={{0}};
  GmlcRoom rooms[2]={{0}};
  GmlcRoomInstance *instances[2]={0};
  int room_order[2]={0,1};
  char *frame=NULL,*moving_step=NULL,*second_step=NULL;
  project.host=&services;
  project.name=(char *)"neutral-dense-resident-scene-route";
  project.sprites=&sprite; project.n_sprites=project.cap_sprites=1;
  project.objects=objects; project.n_objects=project.cap_objects=3;
  project.rooms=rooms; project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order; project.n_room_order=2;
  frame=gmlc_project_add_memory_file(&project,"sprite",GMLC_MEMORY_RGBA,
                                     pixels,sizeof pixels,SPRITE_SIDE,SPRITE_SIDE);
  moving_step=gmlc_project_add_memory_file(&project,"moving-step",GMLC_MEMORY_TEXT,
    "if (keyboard_check(vk_right)) x += 1;\n"
    "if (x >= 42) room_goto_next();\n",
    sizeof("if (keyboard_check(vk_right)) x += 1;\n"
           "if (x >= 42) room_goto_next();\n")-1u,0,0);
  second_step=gmlc_project_add_memory_file(&project,"second-step",GMLC_MEMORY_TEXT,
    "if (keyboard_check(vk_right)) x += 1;\n",
    sizeof("if (keyboard_check(vk_right)) x += 1;\n")-1u,0,0);
  for(int index=0;index<2;index++)
    instances[index]=(GmlcRoomInstance*)calloc(DECORATIONS+1u,sizeof *instances[index]);
  if(frame && moving_step && second_step && instances[0] && instances[1]){
    sprite.id=sprite.name=(char *)"sprite_fixture";
    sprite.runtime_id=0;
    sprite.width=sprite.height=SPRITE_SIDE;
    sprite.bbox_right=sprite.bbox_bottom=SPRITE_SIDE-1;
    sprite.n_frames=1;
    sprite.frame_paths=&frame;
    for(int index=0;index<3;index++){
      objects[index].id=objects[index].name=index==0?(char *)"object_first":
        index==1?(char *)"object_second":(char *)"object_decoration";
      objects[index].sprite_id=0;
      objects[index].mask_id=objects[index].parent_id=-1;
      objects[index].visible=1;
      if(index<2){
        objects[index].events=&events[index];
        objects[index].n_events=objects[index].cap_events=1;
        events[index].event_type=3;
        events[index].event_number=0;
        events[index].source_path=index?second_step:moving_step;
      }
    }
    for(int index=0;index<2;index++){
      rooms[index].id=rooms[index].name=index?(char *)"room_second":(char *)"room_first";
      rooms[index].width=ROOM_WIDTH; rooms[index].height=ROOM_HEIGHT; rooms[index].speed=60;
      rooms[index].draw_background_color=1;
      rooms[index].background_color=index?0x00403020u:0x00102030u;
      rooms[index].view_enabled=1;
      rooms[index].n_views=1;
      rooms[index].views[0].visible=1;
      rooms[index].views[0].wview=rooms[index].views[0].wport=ROOM_WIDTH;
      rooms[index].views[0].hview=rooms[index].views[0].hport=ROOM_HEIGHT;
      rooms[index].views[0].hspeed=rooms[index].views[0].vspeed=-1;
      rooms[index].views[0].object_id=-1;
      rooms[index].instances=instances[index];
      rooms[index].n_instances=rooms[index].cap_instances=DECORATIONS+1;
      for(int item=0;item<=DECORATIONS;item++){
        GmlcRoomInstance *instance=&instances[index][item];
        instance->id=instance->name=item?(char *)"instance_decoration":
          index?(char *)"instance_second":(char *)"instance_first";
        instance->object_id=item?2:index;
        instance->instance_id=200000+index*(DECORATIONS+1)+item;
        instance->x=item?(item*37)%(ROOM_WIDTH-SPRITE_SIDE):index?64:16;
        instance->y=item?(item*53)%(ROOM_HEIGHT-SPRITE_SIDE):index?56:24;
        instance->sx=instance->sy=1.0f;
        instance->color=0xFFFFFFFFu;
      }
    }
  }
  char error[256]={0};
  int ok=frame && moving_step && second_step && instances[0] && instances[1] &&
    gmlc_package_write_structural(&project,fixture->path,error,sizeof error);
  if(!ok) fprintf(stderr,"neutral dense scene route package failed: %s\n",error);
  for(int index=0;index<project.n_memory_files;index++) free(project.memory_files[index].data);
  free(project.memory_files);
  free(frame); free(moving_step); free(second_step);
  free(instances[0]); free(instances[1]);
  if(!ok) anygm_synthetic_content_destroy(fixture);
  return ok;
}

int anygm_synthetic_draw_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char pre_draw[192],draw[192],post_draw[192];
  snprintf(pre_draw,sizeof pre_draw,"%s/draw-pre.gml",fixture->directory);
  snprintf(draw,sizeof draw,"%s/draw-normal.gml",fixture->directory);
  snprintf(post_draw,sizeof post_draw,"%s/draw-post.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(pre_draw,
                 "global.fixture_pre_draw += 1;\n"
                 "view_set_visible(0, false);\n"
                 "view_set_visible(1, true);\n") ||
     !write_text(draw,
                 "global.fixture_draw += 1;\n"
                 "global.fixture_view_sum += view_current + 1;\n") ||
     !write_text(post_draw,
                 "global.fixture_post_draw += 1;\n"
                 "view_set_visible(0, true);\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[3]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"neutral-draw-schedule-fixture";
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order;
  project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=1;
  object.events=events;
  object.n_events=object.cap_events=3;
  events[0].event_type=8;
  events[0].event_number=76;
  events[0].source_path=pre_draw;
  events[1].event_type=8;
  events[1].event_number=0;
  events[1].source_path=draw;
  events[2].event_type=8;
  events[2].event_number=77;
  events[2].source_path=post_draw;

  room.id=room.name=(char *)"room_fixture";
  room.width=64;
  room.height=48;
  room.speed=60;
  room.draw_background_color=1;
  room.view_enabled=1;
  room.n_views=2;
  for(int view=0;view<2;view++){
    room.views[view].visible=view==0;
    room.views[view].wview=room.views[view].wport=64;
    room.views[view].hview=room.views[view].hport=48;
    room.views[view].hspeed=room.views[view].vspeed=-1;
    room.views[view].object_id=-1;
  }
  room.instances=&instance;
  room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic draw package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_view_surface_target_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  char create[192],draw[192],gui[192];
  snprintf(create,sizeof create,"%s/startup.gml",fixture->directory);
  snprintf(draw,sizeof draw,"%s/draw-normal.gml",fixture->directory);
  snprintf(gui,sizeof gui,"%s/draw-post.gml",fixture->directory);
  if(!write_text(create,
       "global.fixture_surface=surface_create(64,48);\n"
       "view_set_surface_id(0,global.fixture_surface);\n"
       "surface_set_target(global.fixture_surface);\n"
       "draw_clear_alpha(16777215,0); surface_reset_target();\n"
       "window_set_size(64,48); display_set_gui_size(64,48);\n") ||
     !write_text(draw,
       "global.fixture_draw_target=surface_get_target();\n"
       "draw_set_colour(255); draw_rectangle(24,16,32,24,false);\n") ||
     !write_text(gui,
       "global.fixture_bound_target=view_get_surface_id(0);\n"
       "draw_clear(16711680); draw_surface(global.fixture_surface,0,0);\n"
       "surface_set_target(global.fixture_surface);\n"
       "draw_clear_alpha(16777215,0); surface_reset_target();\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcProject project={0};
  GmlcObject object={0};
  GmlcObjectEvent events[3]={{0}};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.host=&services;
  project.name=(char *)"neutral-view-surface-target";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;
  object.id=object.name=(char *)"object_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=1; object.events=events; object.n_events=object.cap_events=3;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=8; events[1].event_number=0; events[1].source_path=draw;
  events[2].event_type=8; events[2].event_number=64; events[2].source_path=gui;
  room.id=room.name=(char *)"room_fixture";
  room.width=128; room.height=96; room.speed=60;
  room.draw_background_color=0;
  room.view_enabled=1; room.n_views=1;
  room.views[0].visible=1;
  room.views[0].xview=20; room.views[0].yview=12;
  room.views[0].wview=room.views[0].wport=64;
  room.views[0].hview=room.views[0].hport=48;
  room.views[0].hspeed=room.views[0].vspeed=-1;
  room.views[0].object_id=-1;
  room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture";
  instance.object_id=0; instance.instance_id=100000;
  instance.sx=instance.sy=1.0f; instance.color=0xFFFFFFFFu;
  char error[256]={0};
  int ok=gmlc_package_write_structural(&project,fixture->path,error,sizeof error);
  if(!ok){
    fprintf(stderr,"view surface target package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
  }
  return ok;
}

int anygm_synthetic_shifted_port_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;
  char create[192],step[192],draw[192];
  snprintf(create,sizeof create,"%s/port-create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/port-step.gml",fixture->directory);
  snprintf(draw,sizeof draw,"%s/port-draw.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,"surface_resize(application_surface,64,48);\n") ||
     !write_text(step,"view_yport[0] = global.fixture_shift;\n") ||
     !write_text(draw,
       "draw_set_color(c_white);\n"
       "draw_rectangle(10,20,19,29,false);\n"
       "draw_set_color(c_lime);\n"
       "draw_rectangle(30,6-global.fixture_shift,39,9-global.fixture_shift,false);\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services; services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcProject project={0}; GmlcObject object={0}; GmlcObjectEvent events[3]={0};
  GmlcRoom room={0}; GmlcRoomInstance instance={0}; int order=0;
  project.host=&services; project.name=(char *)"neutral-shifted-port";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&order; project.n_room_order=1;
  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1; object.visible=1;
  object.events=events; object.n_events=object.cap_events=3;
  events[0].event_type=0; events[0].source_path=create;
  events[1].event_type=3; events[1].source_path=step;
  events[2].event_type=8; events[2].source_path=draw;
  room.id=room.name=(char *)"room_fixture";
  room.width=64; room.height=48; room.speed=60; room.draw_background_color=1;
  room.view_enabled=1; room.n_views=1; room.views[0].visible=1;
  room.views[0].wview=room.views[0].wport=64;
  room.views[0].hview=room.views[0].hport=48;
  room.views[0].hspeed=room.views[0].vspeed=-1; room.views[0].object_id=-1;
  room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1; instance.color=0xFFFFFFFFu;
  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"shifted-port fixture: %s\n",error);
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  return 1;
}

static int synthetic_window_gui_content_create(AnygmSyntheticContent *fixture,int explicit_gui_size){
  int translated_world=explicit_gui_size==17;
  int empty_indirect=explicit_gui_size==15;
  int automatic_gui=(explicit_gui_size>=9 && explicit_gui_size<=12) || empty_indirect;
  int forced_owned=explicit_gui_size==13 || explicit_gui_size==14 || explicit_gui_size==16;
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;
  char create[192],gui[192],draw[192];
  snprintf(create,sizeof create,"%s/window-create.gml",fixture->directory);
  snprintf(gui,sizeof gui,"%s/window-gui.gml",fixture->directory);
  snprintf(draw,sizeof draw,"%s/window-draw.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,translated_world?
      "window_set_size(64,48);\n"
      "application_surface_draw_enable(false);\n"
      "view_xport[0]=1; view_yport[0]=2;\n":explicit_gui_size==16?
      "window_set_size(128,96);\n"
      "application_surface_draw_enable(false);\n":empty_indirect?
      "window_set_size(128,96);\n"
      "surface_resize(application_surface,64,48);\n"
      "display_set_gui_size(64,48);\n"
      "texture_set_interpolation(true);\n":explicit_gui_size==4 || explicit_gui_size==5?
      "window_set_size(64,48);\n"
      "display_set_gui_size(128,96);\n"
      "application_surface_draw_enable(false);\n":explicit_gui_size==3 || explicit_gui_size==8 || forced_owned?
      "window_set_size(128,96);\n":explicit_gui_size==6 || explicit_gui_size==7?
      "window_set_size(64,48);\n"
      "application_surface_draw_enable(false);\n":explicit_gui_size==2?
      "window_set_size(64,48);\n"
      "surface_resize(application_surface,64,48);\n"
      "canvas=surface_create(64,48);\n"
      "application_surface_draw_enable(false);\n":automatic_gui?
      "window_set_size(64,48);\n"
      "texture_set_interpolation(true);\n":explicit_gui_size?
      "window_set_size(64,48);\n"
      "surface_resize(application_surface,64,48);\n"
      "application_surface_draw_enable(false);\n":
      "window_set_size(320,180);\n"
      "surface_resize(application_surface,64,48);\n"
      "application_surface_draw_enable(false);\n") ||
     !write_text(gui,translated_world?
      "draw_surface(application_surface,0,0);\n"
      "draw_set_color(c_white); draw_rectangle(3,3,6,6,false);\n":explicit_gui_size==16?
      "draw_clear(c_blue);\n"
      "draw_surface(application_surface,0,0);\n":empty_indirect?
      "draw_set_color(c_white);\n":explicit_gui_size==5?
      "draw_clear(c_blue);\n"
      "draw_set_alpha(0.5);\n"
      "draw_sprite(spr_fixture,0,12,12);\n"
      "draw_set_alpha(1);\n"
      "draw_set_color(c_white); draw_rectangle(8,6,11,9,false);\n":
      explicit_gui_size==3 || explicit_gui_size==4 || explicit_gui_size==6?
      "draw_clear(c_blue);\n"
      "draw_sprite(spr_fixture,0,12,12);\n"
      "draw_set_color(c_white); draw_rectangle(8,6,11,9,false);\n":explicit_gui_size==7 || explicit_gui_size==8 || forced_owned?
      "draw_clear(c_blue);\n"
      "draw_sprite(spr_fixture,0,12,12);\n":explicit_gui_size==9 || explicit_gui_size==11?
      "draw_sprite_stretched(spr_fixture,0,12,12,16,16);\n":explicit_gui_size==10 || explicit_gui_size==12?
      "draw_sprite_stretched_ext(spr_fixture,0,12,12,16,16,c_gray,0.7);\n":explicit_gui_size==2?
      "display_set_gui_size(64,48); draw_clear(c_black);\n"
      "draw_surface(canvas,0,0);\n":explicit_gui_size?
      "var w = window_get_width(); var h = window_get_height();\n"
      "display_set_gui_size(w,h);\n"
      "draw_set_color(c_blue); draw_rectangle(0,0,w-1,h-1,false);\n"
      "draw_set_color(c_white); draw_rectangle(w/2-2,20,w/2+1,23,false);\n":
      "var w = window_get_width(); var h = window_get_height();\n"
      "draw_set_color(c_blue); draw_rectangle(0,0,w-1,h-1,false);\n"
      "draw_set_color(c_white); draw_rectangle(w/2-2,20,w/2+1,23,false);\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  if(explicit_gui_size==2 && !write_text(draw,
      "surface_set_target(canvas); draw_clear(c_black);\n"
      "draw_set_color(c_white); draw_rectangle(2,2,5,5,false);\n"
      "draw_surface_stretched(application_surface,0,8,64,40);\n"
      "surface_reset_target();\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  if(empty_indirect && !write_text(draw,
      "draw_set_color(c_red); draw_rectangle(12,12,19,19,false);\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  if(explicit_gui_size==16 && !write_text(draw,"draw_clear(c_red);\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  if(translated_world && !write_text(draw,
      "draw_clear(c_red);\n"
      "draw_set_color(c_blue); draw_rectangle(20,12,27,19,false);\n")){
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  AnygmHostServices services={0};
  services.struct_size=sizeof services; services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcProject project={0}; GmlcObject object={0}; GmlcObjectEvent events[3]={0};
  GmlcRoom room={0}; GmlcRoomInstance instance={0}; int order=0;
  GmlcSprite sprite={0}; char *sprite_frame=NULL;
  project.host=&services; project.name=(char *)"neutral-window-gui";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&order; project.n_room_order=1;
  if(explicit_gui_size==3 || explicit_gui_size==4 || explicit_gui_size==5 ||
     explicit_gui_size==6 || explicit_gui_size==7 || explicit_gui_size==8 ||
     automatic_gui || forced_owned){
    uint8_t pixels[8u*8u*4u];
    for(int y=0;y<8;y++) for(int x=0;x<8;x++){
      size_t pixel=((size_t)y*8u+(size_t)x)*4u;
      pixels[pixel]=(uint8_t)(32+x*16);
      pixels[pixel+1u]=(uint8_t)(48+y*12);
      pixels[pixel+2u]=192u;
      pixels[pixel+3u]=255u;
    }
    sprite_frame=gmlc_project_add_memory_file(&project,"sprite",GMLC_MEMORY_RGBA,
                                                pixels,sizeof pixels,8,8);
    if(!sprite_frame){
      free(project.memory_files);
      anygm_synthetic_content_destroy(fixture);
      return 0;
    }
    sprite.id=sprite.name=(char *)"spr_fixture";
    sprite.runtime_id=0;
    sprite.width=sprite.height=8;
    sprite.bbox_right=sprite.bbox_bottom=7;
    sprite.n_frames=1;
    sprite.frame_paths=&sprite_frame;
    project.sprites=&sprite;
    project.n_sprites=project.cap_sprites=1;
  }
  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1; object.visible=1;
  object.events=events; object.n_events=object.cap_events=2;
  events[0].event_type=0; events[0].source_path=create;
  events[1].event_type=8; events[1].event_number=64; events[1].source_path=gui;
  if(explicit_gui_size==2 || empty_indirect || explicit_gui_size==16 || translated_world){
    object.n_events=object.cap_events=3;
    events[2].event_type=8; events[2].source_path=draw;
  }
  room.id=room.name=(char *)"room_fixture";
  room.width=64; room.height=48; room.speed=60; room.draw_background_color=1;
  room.view_enabled=1; room.n_views=1; room.views[0].visible=1;
  room.views[0].wview=room.views[0].wport=64;
  room.views[0].hview=room.views[0].hport=48;
  if(empty_indirect){
    /* The application does not own the larger window/view-port raster. */
    room.width=room.views[0].wview=room.views[0].wport=128;
    room.height=room.views[0].hview=room.views[0].hport=96;
  }
  if(explicit_gui_size==2){
    room.height=room.views[0].hview=40;
    room.background_color=0xFF0000;
  }
  if(automatic_gui) room.background_color=0x00FF00;
  if(explicit_gui_size==11 || explicit_gui_size==12){
    room.view_enabled=0; room.views[0].visible=0;
  }
  if(forced_owned){
    room.width=explicit_gui_size==13?256:64;
    room.height=256;
  }
  room.views[0].hspeed=room.views[0].vspeed=-1; room.views[0].object_id=-1;
  room.instances=&instance; room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture"; instance.object_id=0;
  instance.instance_id=100000; instance.sx=instance.sy=1; instance.color=0xFFFFFFFFu;
  char error[256]={0};
  int written=gmlc_package_write_structural(&project,fixture->path,error,sizeof error);
  for(int index=0;index<project.n_memory_files;index++) free(project.memory_files[index].data);
  free(project.memory_files);
  free(sprite_frame);
  if(!written){
    fprintf(stderr,"window-GUI fixture: %s\n",error);
    anygm_synthetic_content_destroy(fixture); return 0;
  }
  return 1;
}

int anygm_synthetic_window_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,0);
}

int anygm_synthetic_window_gui_size_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,1);
}

int anygm_synthetic_indirect_gui_upscale_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,3);
}

int anygm_synthetic_resident_indirect_gui_upscale_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,8);
}

int anygm_synthetic_forced_owned_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,13);
}

int anygm_synthetic_forced_narrow_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,14);
}

int anygm_synthetic_forced_narrow_overdraw_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,16);
}

int anygm_synthetic_translated_world_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,17);
}

int anygm_synthetic_indirect_gui_reduce_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,4);
}

int anygm_synthetic_indirect_gui_alpha_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,5);
}

int anygm_synthetic_direct_gui_sprite_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,6);
}
int anygm_synthetic_resident_direct_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,7);
}

int anygm_synthetic_automatic_direct_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,9);
}

int anygm_synthetic_automatic_alpha_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,10);
}

int anygm_synthetic_automatic_viewless_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,11);
}

int anygm_synthetic_automatic_viewless_alpha_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,12);
}

int anygm_synthetic_automatic_empty_indirect_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,15);
}

int anygm_synthetic_surface_canvas_content_create(AnygmSyntheticContent *fixture){
  return synthetic_window_gui_content_create(fixture,2);
}

int anygm_synthetic_background_color_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192];
  snprintf(create,sizeof create,"%s/background-create.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,
                 "background_color = make_color_rgb(17, 34, 51);\n"
                 "global.fixture_background_alias = background_colour;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent event={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"neutral-background-color-fixture";
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order;
  project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0;
  object.events=&event;
  object.n_events=object.cap_events=1;
  event.event_type=0;
  event.event_number=0;
  event.source_path=create;

  room.id=room.name=(char *)"room_fixture";
  room.width=64;
  room.height=48;
  room.speed=60;
  room.background_color=0xFF000000u;
  room.draw_background_color=1;
  room.instances=&instance;
  room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic background-color package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

static int synthetic_framebuffer_content_create_variant(
    AnygmSyntheticContent *fixture,int multiview,int clear_view_background,int classic,
    int gpu_sprite,int viewport_variant){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  int classic_root_handoff=gpu_sprite>=62 && gpu_sprite<=65;
  int classic_native_step=(gpu_sprite>=51 && gpu_sprite<=54) || gpu_sprite==61 ||
                          classic_root_handoff;
  int classic_step_composition=(gpu_sprite>=41 && gpu_sprite<=43) || classic_native_step;
  int classic_retained_target=gpu_sprite==38 || gpu_sprite==39 || classic_step_composition;
  int retained_step_target=gpu_sprite==5 || gpu_sprite==6 || classic_retained_target;
  int nested_step_target=gpu_sprite==6;
  int step_clear=gpu_sprite==46;
  int assigned_view_surface=gpu_sprite==7;
  int canvas_geometry=gpu_sprite==8;
  int window_cursor=gpu_sprite==45;
  int window_geometry=gpu_sprite==9 || window_cursor;
  int presentation_filter=gpu_sprite>=10 && gpu_sprite<=12;
  int ended_frame=gpu_sprite==13;
  int classic_redraw=gpu_sprite==14;
  int classic_transition=gpu_sprite==15;
  int cursor_frame=gpu_sprite==16;
  int moving_sound=gpu_sprite==17 || gpu_sprite==18 || gpu_sprite==20;
  int borrowed_resize=gpu_sprite==40;
  int reduced_view_resize=gpu_sprite==57;
  int viewless_resize=gpu_sprite==55 || gpu_sprite==56 || reduced_view_resize;
  int classic_identity_present=gpu_sprite==44;
  int classic_owned_gui=gpu_sprite==47;
  int classic_manual_application=gpu_sprite>=48 && gpu_sprite<=50;
  int classic_sample_root=gpu_sprite>=58 && gpu_sprite<=60;
  int point_function=gpu_sprite==78;
  int viewport_matrix=viewport_variant>=0;
  static const int viewport_ports[][2]={{64,48},{128,96},{32,24},{96,72},
    {96,48},{64,72},{32,48},{64,24},{96,24},{48,96},{1,1},{129,97}};
  static const int viewport_generations[]={530,701,800};
  unsigned viewport_geometry=viewport_matrix?(unsigned)viewport_variant/2u%12u:0u;
  unsigned viewport_generation=viewport_matrix?(unsigned)viewport_variant/24u:0u;
  char viewport_create[256],viewport_step[1024],viewport_draw[1024];
  if(viewport_matrix){
    if(viewport_variant>=72) return 0;
    snprintf(viewport_create,sizeof viewport_create,
      "global.fixture_frame=0; window_set_size(128,96); display_set_gui_size(128,96);\n"
      "texture_set_interpolation(false);\n");
    snprintf(viewport_step,sizeof viewport_step,
      "global.fixture_frame+=1; global.fixture_rng=irandom(65535);\n"
      "if(room==0 && global.fixture_frame==2) room_goto(1);\n"
      "if(global.fixture_frame==3){view_wview[0]=67; view_hview[0]=49;}\n"
      "if(global.fixture_frame==5){view_wview[0]=61; view_hview[0]=46;"
        "view_wport[0]=%d; view_hport[0]=%d;}\n"
      "if(global.fixture_frame==7){view_wview[0]=64; view_hview[0]=48;"
        "view_wport[0]=%d; view_hport[0]=%d;}\n",
      viewport_ports[viewport_geometry][0]+1,
      viewport_ports[viewport_geometry][1]>1?viewport_ports[viewport_geometry][1]-1:1,
      viewport_ports[viewport_geometry][0],viewport_ports[viewport_geometry][1]);
    snprintf(viewport_draw,sizeof viewport_draw,
      "global.fixture_application_width=surface_get_width(application_surface);\n"
      "global.fixture_application_height=surface_get_height(application_surface);\n"
      "draw_set_alpha(%s);\n"
      "for(var i=0;i<view_wview[0];i+=2){\n"
      " draw_set_color((i mod 4)==0?c_red:c_lime);\n"
      " draw_rectangle(i,0,i,view_hview[0]-1,false);}\n"
      "draw_set_alpha(1); draw_point_color(view_wview[0]-1,view_hview[0]-1,c_white);\n",
      (viewport_variant&1)?"0.61":"1");
  }
  int classic_native_borrowed=gpu_sprite>=66 && gpu_sprite<=78;
  unsigned borrowed_variant=classic_native_borrowed && !point_function?(unsigned)(gpu_sprite-66):0u;
  int single_classic_view=(gpu_sprite>=21 && gpu_sprite<=37) || borrowed_resize || classic_manual_application || reduced_view_resize ||
                          (classic_native_borrowed && (borrowed_variant&2u));
  char create[192],step[192],draw[192],draw_begin[192],draw_gui[192];
  snprintf(create,sizeof create,"%s/retain-create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/retain-step.gml",fixture->directory);
  snprintf(draw,sizeof draw,"%s/retain-draw.gml",fixture->directory);
  snprintf(draw_begin,sizeof draw_begin,"%s/retain-draw-begin.gml",fixture->directory);
  snprintf(draw_gui,sizeof draw_gui,"%s/retain-draw-gui.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,viewport_matrix?viewport_create:classic_owned_gui?
       "global.fixture_frame = 0;\n"
       "surface_resize(application_surface,64,48);\n":
       classic_manual_application?
       "global.fixture_frame = 0;\n"
       "application_surface_draw_enable(false);\n"
       "global.fixture_recovery = view_wport[0] == view_wview[0];\n"
       "global.fixture_surface = surface_create(16,12);\n":
       borrowed_resize?
       "global.fixture_frame = 0;\n"
       "view_wview[0] = 128; view_hview[0] = 96;\n"
       "view_wport[0] = 128; view_hport[0] = 96;\n"
       "window_set_size(128,96);\n":
       gpu_sprite==22 || classic_identity_present?
       "global.fixture_frame = 0;\n"
       "texture_set_interpolation(true);\n":
       gpu_sprite==20?
       "global.fixture_frame = 0;\n"
       "audio_play_sound(snd_fixture,1,true);\n"
       "game_set_speed(30,0);\n"
       "global.fixture_display_result = 1;\n"
       "if (display_get_frequency() != 50) global.fixture_display_result = display_set_frequency(50);\n":
       moving_sound?
       "global.fixture_frame = 0;\n"
       "audio_play_sound(snd_fixture,1,true);\n":
       nested_step_target?
       "global.fixture_frame = 0;\n"
       "global.fixture_surface = surface_create(8,8);\n"
       "global.fixture_inner = surface_create(8,8);\n":
       step_clear?
       "global.fixture_frame = 0;\n"
       "global.fixture_surface = surface_create(8,8);\n":
       assigned_view_surface?
       "global.fixture_frame = 0;\n"
       "global.fixture_seen = 0;\n"
       "global.fixture_surface = surface_create(8,8);\n"
       "global.fixture_inner = surface_create(8,8);\n"
       "view_set_surface_id(0,global.fixture_surface);\n"
       "view_set_surface_id(1,global.fixture_inner);\n":
       canvas_geometry?
       "global.fixture_frame = 0;\n"
       "window_set_size(128,96);\n"
       "display_set_gui_size(64,32);\n"
       "application_surface_draw_enable(false);\n":
       window_cursor?
       "global.fixture_frame = 0;\n"
       "window_set_size(128,96);\n"
       "application_surface_draw_enable(false);\n"
       "cursor_sprite = spr_fixture;\n":
       window_geometry?
       "global.fixture_frame = 0;\n"
       "window_set_size(128,96);\n"
       "application_surface_draw_enable(false);\n":
       cursor_frame?
       "global.fixture_frame = 0;\n"
       "cursor_sprite = spr_fixture;\n":
       classic_redraw?
       "global.fixture_frame = 0;\n"
       "global.fixture_seen = 0;\n":
       classic_root_handoff?
       "global.fixture_frame = 0;\n"
       "global.fixture_surface = -1;\n":
       classic_native_step?
       "global.fixture_frame = 0;\n"
       "global.fixture_surface = surface_create(16,12);\n"
       "surface_set_target(global.fixture_surface);\n"
       "draw_clear(c_blue);\n":
       classic_step_composition?
       "global.fixture_frame = 0;\n"
       "global.fixture_surface = surface_create(16,12);\n":
       retained_step_target?
       "global.fixture_frame = 0;\n"
       "global.fixture_surface = surface_create(8,8);\n":
       gpu_sprite==3 || gpu_sprite==4?
       "global.fixture_frame = 0;\n"
       "view_xview[0] = 0.5; view_xview[1] = 32.25;\n":
       "global.fixture_frame = 0;\n") ||
     !write_text(step,viewport_matrix?viewport_step:classic_native_borrowed?
                 "global.fixture_frame += 1;\n"
                 "global.fixture_rng = irandom(65535);\n"
                 "if (room == 0 && global.fixture_frame == 3) room_goto(1);\n":
                 classic_root_handoff?
                 "global.fixture_frame += 1;\n"
                 "global.fixture_rng = irandom(65535);\n"
                 "if (global.fixture_frame == 4 || global.fixture_frame == 10) {\n"
                 "  global.fixture_surface = surface_create(16,12);\n"
                 "  surface_set_target(global.fixture_surface); draw_clear(c_blue);\n"
                 "} else if (global.fixture_frame == 8) {\n"
                 "  surface_reset_target(); surface_free(global.fixture_surface);\n"
                 "  global.fixture_surface = -1;\n"
                 "} else if (surface_exists(global.fixture_surface)) {\n"
                 "  surface_reset_target(); draw_clear(c_green);\n"
                 "  draw_surface_stretched(global.fixture_surface,0,0,64,48);\n"
                 "  surface_set_target(global.fixture_surface);\n"
                 "}\n":gpu_sprite==59?
                 "global.fixture_frame += 1;\n"
                 "global.fixture_rng = irandom(65535);\n"
                 "if (room == 0 && global.fixture_frame == 4) {\n"
                 "  window_set_size(160,120); room_goto(1);\n"
                 "}\n":classic_sample_root?
                 "global.fixture_frame += 1;\n"
                 "global.fixture_rng = irandom(65535);\n"
                 "if (room == 0 && global.fixture_frame == 4) room_goto(1);\n":
                 classic_manual_application?
                 "global.fixture_frame += 1;\n"
                 "if (global.fixture_frame > 1) {\n"
                 "  global.fixture_step_alpha = draw_get_alpha();\n"
                 "  global.fixture_step_colour = draw_get_color();\n"
                 "  draw_surface_stretched(global.fixture_surface,4,6,16,12);\n"
                 "}\n":
                 borrowed_resize?
                 "global.fixture_frame += 1;\n"
                 "if (global.fixture_frame == 4) {\n"
                 "  view_wview[0] = 80; view_hview[0] = 60;\n"
                 "  view_wport[0] = 80; view_hport[0] = 60;\n"
                 "  window_set_size(80,60);\n"
                 "}\n"
                 "if (global.fixture_frame == 7) {\n"
                 "  view_wview[0] = 64; view_hview[0] = 48;\n"
                 "  view_wport[0] = 64; view_hport[0] = 48;\n"
                 "  window_set_size(64,48);\n"
                 "}\n":
                 gpu_sprite==29?
                 "global.fixture_frame += 1;\n"
                 "if (global.fixture_frame == 4) texture_set_interpolation(true);\n":
                 gpu_sprite==26?
                 "if (global.fixture_frame >= 3) room_goto(1);\n"
                 "global.fixture_frame += 1;\n":
                 moving_sound?
                 "global.fixture_frame += 1;\n":
                 nested_step_target?
                 "surface_set_target(global.fixture_surface);\n"
                 "draw_clear(c_green);\n"
                 "surface_set_target(global.fixture_inner);\n":
                 step_clear?
                 "global.fixture_frame += 1;\n"
                 "surface_set_target(global.fixture_surface);\n"
                 "draw_clear(c_green);\n"
                 "surface_reset_target();\n":
                 ended_frame?
                 "global.fixture_frame += 1;\n"
                 "if (global.fixture_frame == 2) game_end();\n":
                 classic_redraw?
                 "global.fixture_frame += 1;\n"
                 "if (global.fixture_frame == 1) { screen_redraw(); screen_refresh(); }\n":
                 classic_transition?
                 "if (global.fixture_frame == 1) {\n"
                 "  transition_kind = 21; transition_steps = 100; room_goto(1);\n"
                 "}\n"
                 "global.fixture_frame += 1;\n":
                 assigned_view_surface || canvas_geometry || window_geometry || presentation_filter?
                 "global.fixture_frame += 1;\n":
                 classic_step_composition?
                 "global.fixture_frame += 1;\n"
                 "if (surface_get_target() != -1) surface_reset_target();\n"
                 "draw_clear(c_green);\n"
                 "draw_surface_stretched(global.fixture_surface,0,0,64,48);\n"
                 "surface_set_target(global.fixture_surface);\n":
                 retained_step_target?
                 "surface_set_target(global.fixture_surface);\n":
                 "if (global.fixture_frame >= 1) room_goto(1);\n"
                 "global.fixture_frame += 1;\n") ||
     (gpu_sprite && !write_text(draw,viewport_matrix?viewport_draw:point_function?
       "global.fixture_application_width = surface_get_width(application_surface);\n"
       "global.fixture_application_height = surface_get_height(application_surface);\n"
       "draw_set_color(c_gray); draw_point(9+global.fixture_frame,3);\n"
       "draw_point_color(3,4,c_red); draw_point_colour(4,5,c_blue);\n"
       "draw_set_alpha(0.41); draw_point(5,6); draw_set_alpha(1);\n"
       "draw_sprite(spr_fixture,0,8+global.fixture_frame,12);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       classic_native_borrowed?
       "global.fixture_application_width = surface_get_width(application_surface);\n"
       "global.fixture_application_height = surface_get_height(application_surface);\n"
       "draw_sprite(spr_fixture,0,8+global.fixture_frame,12);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       classic_sample_root?
       "global.fixture_application_width = surface_get_width(application_surface);\n"
       "global.fixture_application_height = surface_get_height(application_surface);\n"
       "draw_sprite_ext(spr_fixture,0,8.25,12.75,1.5,1,0,c_white,0.61);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n"
       "draw_set_alpha(0.61); draw_rectangle(3,4,7,9,false); draw_set_alpha(1);\n":
       viewless_resize?
       "d3d_start();\n"
       "d3d_set_projection_ortho(0,0,64,48,0);\n"
       "d3d_set_hidden(true);\n"
       "draw_sprite(spr_fixture,0,24,16);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       classic_manual_application?
       "global.fixture_application_width = surface_get_width(application_surface);\n"
       "global.fixture_application_height = surface_get_height(application_surface);\n"
       "draw_sprite(spr_fixture,0,48,16);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n"
       "draw_set_color(c_red); draw_rectangle(8,12,20,24,false);\n"
       "surface_set_target(global.fixture_surface);\n"
       "if (global.fixture_frame mod 2 == 0) draw_clear(c_blue);\n"
       "else draw_clear(c_lime);\n"
       "surface_reset_target();\n":
       step_clear?
       "draw_surface(global.fixture_surface,8,8);\n"
       "draw_sprite(spr_fixture,0,32,16);\n":
       gpu_sprite==42?
       "surface_reset_target();\n"
       "draw_clear(c_red);\n"
       "draw_set_color(c_white); draw_rectangle(0,0,2,2,false);\n"
       "surface_set_target(global.fixture_surface);\n"
       "draw_sprite(spr_fixture,0,1.375,1.5);\n":
       classic_retained_target?
       "draw_sprite(spr_fixture,0,1.375,1.5);\n":
       moving_sound?
       "draw_sprite(spr_fixture,0,8+global.fixture_frame,12);\n":
       classic_redraw?
       "global.fixture_seen += 1;\n"
       "draw_sprite(spr_fixture,0,8,12);\n":
       gpu_sprite==31?
       "draw_sprite_ext(spr_fixture,0,48,16,1,1,0,c_white,0);\n"
       "draw_sprite(spr_fixture,0,48,16);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==37?
       "draw_sprite(spr_fixture,0,48,16);\n"
       "draw_set_color(c_black); draw_set_alpha(0.2); draw_rectangle(0,0,65,49,false);\n"
       "draw_set_alpha(0.4); draw_rectangle(30,20,20,12,false);\n"
       "draw_set_color(c_blue); draw_set_alpha(0.6000000000000001); draw_rectangle(-4,-3,7,8,false);\n"
       "draw_set_color(c_red); draw_set_alpha(0.8); draw_rectangle(12,4,12,4,false);\n"
       "draw_set_alpha(1); draw_sprite(spr_fixture,0,24,8);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==32?
       "draw_set_color(c_black); draw_rectangle(0,0,65,49,false);\n"
       "draw_set_color(c_red); draw_rectangle(-4,-3,7,8,false);\n"
       "draw_set_color(c_lime); draw_rectangle(30,20,20,12,false);\n"
       "draw_set_color(c_blue); draw_rectangle(12,4,12,4,false);\n"
       "draw_sprite(spr_fixture,0,48,16);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==33?
       "draw_sprite(spr_fixture,0,47.375,16.5);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==34?
       "draw_sprite_ext(spr_fixture,0,47.375,16.5,1,1,0,c_white,0.07692307692307693);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==36?
       "draw_sprite(spr_fixture,0,48,16);\n"
       "d3d_start(); d3d_set_projection_ortho(0,0,32,24,0);\n"
       "draw_sprite(spr_fixture,0,8,4);\n"
       "d3d_end();\n":
       gpu_sprite==35?
       "draw_sprite(spr_fixture,0,48,16);\n"
       "draw_sprite_part_ext(spr_fixture,0,0,0,7,6,8,4,1,1,c_white,0);\n"
       "draw_sprite_part_ext(spr_fixture,0,1,1,6,6,20,24,1,1,c_white,-1);\n"
       "draw_sprite(spr_fixture,0,24,8);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==27?
       "global.fixture_application_width = surface_get_width(application_surface);\n"
       "global.fixture_application_height = surface_get_height(application_surface);\n"
       "draw_sprite(spr_fixture,0,48,16);\n"
       "global.fixture_pixel = surface_getpixel(application_surface,48,16);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==30?
       "draw_sprite_ext(spr_fixture,0,48,16,1,1,0,c_white,0.5);\n":
       classic_identity_present?
       "draw_clear(c_black);\n"
       "draw_set_color(c_red); draw_rectangle(1,2,10,8,false);\n"
       "draw_set_color(c_lime); draw_rectangle(31,17,45,29,false);\n":
       single_classic_view?
       "global.fixture_application_width = surface_get_width(application_surface);\n"
       "global.fixture_application_height = surface_get_height(application_surface);\n"
       "draw_sprite(spr_fixture,0,48,16);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n":
       gpu_sprite==19 || classic_owned_gui?
       "draw_sprite(spr_fixture,0,48,16);\n"
       "draw_set_color(c_white); draw_text(8,30,\"GPU\");\n"
       "draw_set_color(c_red); draw_rectangle(8,12,20,24,false);\n":
       gpu_sprite==4?
       "if (view_current == 0) draw_sprite_ext(spr_fixture,0,8,12,0.7,0.7,0,c_white,1);\n"
       "else draw_sprite_ext(spr_fixture,0,48,16,0.7,0.7,0,c_white,1);\n":
       "if (view_current == 0) draw_sprite(spr_fixture,0,8,12);\n"
       "else draw_sprite(spr_fixture,0,48,16);\n")) ||
     ((retained_step_target || assigned_view_surface) &&
      !write_text(draw_begin,classic_step_composition?
         "if (global.fixture_frame mod 2 == 0) draw_clear(c_blue);\n"
         "else draw_clear(c_lime);\n":
         classic_retained_target?
         "draw_clear(c_blue);\n":
         assigned_view_surface?
         "global.fixture_seen += 1;\n"
         "if (view_current == 0) draw_clear(c_blue);\n"
         "else draw_clear(c_green);\n":
         nested_step_target?
         "if (view_current == 0) {\n"
         "  draw_clear(c_blue); surface_reset_target();\n"
         "  draw_set_color(c_red); draw_rectangle(0,0,3,3,false);\n"
         "  surface_reset_target();\n"
         "}\n":
         "if (view_current == 0) {\n"
         "  draw_clear(c_blue); surface_reset_target();\n"
         "}\n")) ||
     ((retained_step_target || assigned_view_surface || canvas_geometry || window_geometry ||
       gpu_sprite==28 || classic_owned_gui || classic_manual_application || classic_native_borrowed || viewport_matrix) &&
      !write_text(draw_gui,viewport_matrix?
         "draw_set_color(c_blue); draw_rectangle(8,8,10,10,false);\n"
         "draw_point_colour(12,8,c_white);\n":point_function?
         "draw_set_color(c_red); draw_point(24,14);\n"
         "draw_point_color(25,15,c_blue); draw_point_colour(26,16,c_lime);\n":
         classic_native_borrowed?
         (borrowed_variant>=4u && borrowed_variant<8u?
          "draw_sprite_ext(spr_fixture,0,20,12,1,1,0,c_white,0.375);\n"
          "draw_set_color(c_red); draw_set_alpha(0.5);\n"
          "draw_rectangle(24,14,35,20,false); draw_set_alpha(1);\n":
          "draw_sprite(spr_fixture,0,20,12);\n"
          "draw_set_color(c_red); draw_rectangle(24,14,35,20,false);\n"):
         classic_manual_application?
         "if (!global.fixture_recovery || global.fixture_frame mod 3 != 0) draw_surface(global.fixture_surface,20,8);\n"
         "draw_set_color(c_fuchsia); draw_set_alpha(0.375);\n"
         "if (!global.fixture_recovery || global.fixture_frame mod 3 != 0) draw_set_blend_mode_ext(bm_one,bm_zero);\n":
         classic_step_composition?
         "global.fixture_seen = surface_get_target();\n":
         classic_retained_target?
         "surface_reset_target();\n"
         "draw_surface(global.fixture_surface,0,0);\n":
         gpu_sprite==28?
         "draw_set_color(c_red); draw_rectangle(4,4,12,12,false);\n":
         classic_owned_gui?
         "draw_sprite(spr_fixture,0,12,8);\n":
         canvas_geometry?
         "draw_surface_stretched(application_surface,0,0,64,32);\n"
         "draw_sprite(spr_fixture,0,12,12);\n"
         "draw_set_color(c_white); draw_rectangle(0,0,3,3,false);\n":
         window_geometry?
         "draw_surface_stretched(application_surface,0,0,128,96);\n"
         "draw_sprite(spr_fixture,0,12,12);\n"
         "draw_set_color(c_white); draw_rectangle(0,0,3,3,false);\n":
         assigned_view_surface?
         "draw_surface_stretched(global.fixture_surface,0,0,8,8);\n"
         "draw_surface_stretched(global.fixture_inner,8,0,8,8);\n":
         nested_step_target?
         "draw_surface(global.fixture_surface,0,0);\n"
         "draw_surface(global.fixture_inner,8,0);\n":
         "draw_surface(global.fixture_surface,0,0);\n"))){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[5]={0};
  GmlcRoom rooms[2]={0};
  GmlcRoomInstance instance={0};
  GmlcSprite sprite={0}; char *sprite_frame=NULL;
  GmlcSound sound={0}; char *sound_blob=NULL;
  int room_order[2]={0,1};
  project.name=(char *)"neutral-framebuffer-fixture";
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;
  if(gpu_sprite){
    uint8_t pixels[8u*8u*4u];
    for(int y=0;y<8;y++) for(int x=0;x<8;x++){
      size_t at=((size_t)y*8u+(size_t)x)*4u;
      pixels[at]=(uint8_t)(32+x*16);
      pixels[at+1u]=(uint8_t)(48+y*12);
      pixels[at+2u]=192u;
      pixels[at+3u]=255u;
      if(gpu_sprite==33 || gpu_sprite==34){
        if(x==0 || x==7 || y==0 || y==7 || (x+y)%3==0) pixels[at+3u]=0u;
        else if(gpu_sprite==34 && (x+y)%3==1) pixels[at+3u]=127u;
      }
    }
    sprite_frame=gmlc_project_add_memory_file(&project,"sprite",GMLC_MEMORY_RGBA,
                                                pixels,sizeof pixels,8,8);
    if(!sprite_frame){
      free(project.memory_files);
      anygm_synthetic_content_destroy(fixture);
      return 0;
    }
    sprite.id=sprite.name=(char *)"spr_fixture";
    sprite.runtime_id=0;
    sprite.width=sprite.height=8;
    sprite.bbox_right=sprite.bbox_bottom=7;
    sprite.n_frames=1;
    sprite.frame_paths=&sprite_frame;
    project.sprites=&sprite;
    project.n_sprites=project.cap_sprites=1;
  }
  if(moving_sound){
    static const uint8_t wav_header[44]={
      'R','I','F','F',0xA4,0x3E,0,0,'W','A','V','E','f','m','t',' ',
      16,0,0,0,1,0,1,0,0x40,0x1F,0,0,0x80,0x3E,0,0,
      2,0,16,0,'d','a','t','a',0x80,0x3E,0,0
    };
    uint8_t wav[44u+16000u];
    memcpy(wav,wav_header,sizeof wav_header);
    for(size_t sample=0;sample<8000u;sample++){
      uint16_t value=(sample/20u)&1u?UINT16_C(0xE0C0):UINT16_C(0x1F40);
      wav[44u+sample*2u]=(uint8_t)value;
      wav[45u+sample*2u]=(uint8_t)(value>>8);
    }
    sound_blob=gmlc_project_add_memory_file(&project,"tone.wav",GMLC_MEMORY_BLOB,
                                             wav,sizeof wav,0,0);
    if(!sound_blob){
      for(int index=0;index<project.n_memory_files;index++) free(project.memory_files[index].data);
      free(project.memory_files);
      free(sprite_frame);
      anygm_synthetic_content_destroy(fixture);
      return 0;
    }
    sound.id=sound.name=(char *)"snd_fixture";
    sound.data_path=sound_blob;
    sound.volume=sound.pitch=1.0f;
    project.sounds=&sound;
    project.n_sounds=project.cap_sounds=1;
  }
  if(classic){
    project.classic_version=gpu_sprite==19 || classic_owned_gui ||
                            (single_classic_view && gpu_sprite!=24)?530:800;
    if(gpu_sprite==39 || gpu_sprite==43) project.classic_version=810;
    if(gpu_sprite==41 || gpu_sprite==42) project.classic_version=701;
    if(classic_native_step){
      project.classic_version=gpu_sprite==51?800:810;
      project.classic_scaling=gpu_sprite==53 || gpu_sprite==61 ||
                              gpu_sprite==63 || gpu_sprite==65?200:100;
      project.classic_interpolate=gpu_sprite==54 || gpu_sprite==61 ||
                                  (classic_root_handoff && gpu_sprite>=64);
    }
    if(classic_sample_root){
      project.classic_version=810;project.classic_scaling=200;
      project.classic_interpolate=gpu_sprite==60;
    }
    if(classic_manual_application) project.classic_version=gpu_sprite==50?810:701;
    if(classic_identity_present) project.classic_version=701;
    if(viewless_resize) project.classic_version=gpu_sprite==55?530:701;
    if(classic_native_borrowed){
      project.classic_version=(borrowed_variant&1u)?810:800;
      project.classic_scaling=borrowed_variant>=10u?300:100;
      project.classic_interpolate=borrowed_variant==8u || borrowed_variant==9u;
    }
    if(viewport_matrix) project.classic_version=viewport_generations[viewport_generation];
    /* Distinct from both room colors so a cleared classic frame cannot be mistaken for a
     * retained one or for the room the fixture painted first. */
    project.classic_outside_color=0x00204060u;
  }

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=gpu_sprite;
  object.events=events;
  object.n_events=object.cap_events=retained_step_target || assigned_view_surface?5:
                                     canvas_geometry || window_geometry || gpu_sprite==28 || classic_owned_gui || classic_manual_application || classic_native_borrowed || viewport_matrix?4:gpu_sprite?3:2;
  events[0].event_type=0;
  events[0].event_number=0;
  events[0].source_path=create;
  events[1].event_type=3;
  events[1].event_number=0;
  events[1].source_path=step;
  if(gpu_sprite){
    events[2].event_type=8;
    events[2].source_path=draw;
  }
  if(retained_step_target || assigned_view_surface){
    events[3].event_type=8;
    events[3].event_number=72;
    events[3].source_path=draw_begin;
    events[4].event_type=8;
    events[4].event_number=64;
    events[4].source_path=draw_gui;
  } else if(canvas_geometry || window_geometry || gpu_sprite==28 || classic_owned_gui || classic_manual_application || classic_native_borrowed || viewport_matrix){
    events[3].event_type=8;
    events[3].event_number=64;
    events[3].source_path=draw_gui;
  }

  rooms[0].id=rooms[0].name=(char *)"room_painted";
  rooms[0].width=gpu_sprite==10?64:gpu_sprite==11?400:gpu_sprite==12?128:
                 gpu_sprite==18?1920:64;
  rooms[0].height=gpu_sprite==10?32:gpu_sprite==11?300:gpu_sprite==12?96:
                  gpu_sprite==18?1080:48;
  rooms[0].speed=60;
  rooms[0].background_color=0xFF336699u;
  rooms[0].draw_background_color=assigned_view_surface || classic_retained_target?0:1;
  rooms[0].instances=&instance;
  rooms[0].n_instances=rooms[0].cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  rooms[1].id=rooms[1].name=(char *)"room_retained";
  rooms[1].width=rooms[0].width;
  rooms[1].height=rooms[0].height;
  if(viewless_resize){rooms[1].width=128;rooms[1].height=96;}
  if(gpu_sprite==59){rooms[1].width=80;rooms[1].height=60;}
  rooms[1].speed=60;
  rooms[1].background_color=classic_transition?0xFF000080u:0xFF000000u;
  rooms[1].draw_background_color=classic_transition?1:0;
  if(classic_sample_root) rooms[1].draw_background_color=1;
  if(classic_native_borrowed) rooms[1].draw_background_color=(borrowed_variant&4u)==0u;
  rooms[1].clear_view_background=clear_view_background;
  if(gpu_sprite && !classic_transition){
    rooms[1].instances=&instance;
    rooms[1].n_instances=rooms[1].cap_instances=1;
  }
  if(multiview){
    for(int room=0;room<2;room++){
      rooms[room].view_enabled=1;
      rooms[room].n_views=2;
      for(int view=0;view<2;view++){
        rooms[room].views[view].visible=1;
        rooms[room].views[view].xview=view*32;
        rooms[room].views[view].wview=32;
        rooms[room].views[view].wport=gpu_sprite>=2 && gpu_sprite<=4 && view==0?28:32;
        rooms[room].views[view].hview=rooms[room].views[view].hport=48;
        rooms[room].views[view].xport=gpu_sprite>=2 && gpu_sprite<=4 && view==0?4:view*32;
        rooms[room].views[view].hspeed=rooms[room].views[view].vspeed=-1;
        rooms[room].views[view].object_id=-1;
      }
    }
  }
  if(single_classic_view){
    for(int room=0;room<2;room++){
      if(gpu_sprite==26 && room==1){
        rooms[room].width=80;
        rooms[room].height=60;
      }
      rooms[room].view_enabled=1;
      rooms[room].n_views=1;
      rooms[room].views[0].visible=1;
      rooms[room].views[0].wview=rooms[room].width;
      rooms[room].views[0].hview=rooms[room].height;
      rooms[room].views[0].wport=borrowed_resize || gpu_sprite==48 || classic_native_borrowed?rooms[room].width:
                               gpu_sprite==23?96:rooms[room].width*2;
      rooms[room].views[0].hport=borrowed_resize || gpu_sprite==48 || classic_native_borrowed?rooms[room].height:
                               gpu_sprite==23?72:rooms[room].height*2;
      if(viewport_matrix){
        rooms[room].views[0].wport=viewport_ports[viewport_geometry][0];
        rooms[room].views[0].hport=viewport_ports[viewport_geometry][1];
      }
      if(reduced_view_resize){
        rooms[room].views[0].wport=rooms[room].width/2;
        rooms[room].views[0].hport=rooms[room].height/2;
      }
      rooms[room].views[0].xport=gpu_sprite==25?4:0;
      rooms[room].views[0].hspeed=rooms[room].views[0].vspeed=-1;
      rooms[room].views[0].object_id=-1;
    }
  }

  char error[256]={0};
  int written=gmlc_package_write_structural(&project,fixture->path,error,sizeof error);
  for(int index=0;index<project.n_memory_files;index++) free(project.memory_files[index].data);
  free(project.memory_files);
  free(sprite_frame);
  free(sound_blob);
  if(!written){
    fprintf(stderr,"synthetic framebuffer package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

static int synthetic_framebuffer_content_create(
    AnygmSyntheticContent *fixture,int multiview,int clear_view_background,int classic,int gpu_sprite){
  return synthetic_framebuffer_content_create_variant(
    fixture,multiview,clear_view_background,classic,gpu_sprite,-1);
}

int anygm_synthetic_classic_viewport_content_create(AnygmSyntheticContent *fixture,unsigned variant){
  if(variant>=72u) return 0;
  return synthetic_framebuffer_content_create_variant(fixture,0,0,1,21,(int)variant);
}

int anygm_synthetic_framebuffer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,0);
}

int anygm_synthetic_moving_sound_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,17);
}

int anygm_synthetic_display_cadence_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,20);
}

int anygm_synthetic_large_moving_sound_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,18);
}

int anygm_synthetic_multiview_framebuffer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,0);
}

int anygm_synthetic_multiview_gpu_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,1);
}

int anygm_synthetic_multiview_offset_gpu_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,2);
}

int anygm_synthetic_multiview_fractional_gpu_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,3);
}

int anygm_synthetic_multiview_fractional_scale_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,4);
}

int anygm_synthetic_multiview_step_target_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,5);
}

int anygm_synthetic_multiview_nested_target_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,6);
}

int anygm_synthetic_multiview_assigned_surface_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,7);
}

int anygm_synthetic_multiview_canvas_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0,8);
}

int anygm_synthetic_window_raster_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,9);
}

int anygm_synthetic_crt_native_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,10);
}

int anygm_synthetic_crt_scaled_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,11);
}

int anygm_synthetic_host_box_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,12);
}

int anygm_synthetic_ended_frame_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,13);
}

int anygm_synthetic_cursor_frame_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,16);
}

int anygm_synthetic_window_cursor_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,45);
}

int anygm_synthetic_classic_redraw_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,14);
}

int anygm_synthetic_classic_viewless_draw_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,19);
}

int anygm_synthetic_classic_viewless_resize_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,55);
}

int anygm_synthetic_classic_viewless_ortho_resize_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,56);
}

int anygm_synthetic_classic_reduced_view_resize_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,57);
}

int anygm_synthetic_classic_owned_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,47);
}
int anygm_synthetic_classic_manual_application_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,48);
}
int anygm_synthetic_classic_manual_application_scaled_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,49);
}
int anygm_synthetic_classic_late_manual_application_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,50);
}

int anygm_synthetic_classic_single_view_draw_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,21);
}
int anygm_synthetic_classic_identity_present_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,44);
}
int anygm_synthetic_classic_single_view_filtered_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,22);
}
int anygm_synthetic_classic_single_view_fractional_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,23);
}
int anygm_synthetic_classic_single_view_modern_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,24);
}
int anygm_synthetic_classic_native_borrowed_content_create(AnygmSyntheticContent *fixture,unsigned variant){
  if(variant>=13u) return 0;
  return synthetic_framebuffer_content_create(fixture,0,0,1,66+(int)variant);
}
int anygm_synthetic_classic_single_view_offset_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,25);
}
int anygm_synthetic_classic_single_view_geometry_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,26);
}
int anygm_synthetic_classic_borrowed_resize_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,40);
}
int anygm_synthetic_classic_single_view_read_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,27);
}
int anygm_synthetic_classic_single_view_cpu_gui_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,28);
}
int anygm_synthetic_classic_single_view_filter_transition_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,29);
}
int anygm_synthetic_classic_single_view_partial_alpha_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,30);
}
int anygm_synthetic_classic_single_view_zero_alpha_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,31);
}

int anygm_synthetic_classic_single_view_rectangle_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,32);
}
int anygm_synthetic_classic_single_view_alpha_rectangle_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,37);
}
int anygm_synthetic_classic_single_view_binary_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,33);
}
int anygm_synthetic_classic_single_view_source_alpha_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,34);
}
int anygm_synthetic_classic_single_view_zero_alpha_part_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,35);
}
int anygm_synthetic_classic_single_view_software3d_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,36);
}

int anygm_synthetic_classic_retained_target_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,38);
}
int anygm_synthetic_classic_late_retained_target_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,39);
}
int anygm_synthetic_classic_step_composition_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,41);
}
int anygm_synthetic_classic_step_base_writer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,42);
}
int anygm_synthetic_classic_late_step_composition_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,43);
}
int anygm_synthetic_classic_native_step_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,51);
}
int anygm_synthetic_classic_late_native_step_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,52);
}
int anygm_synthetic_classic_scaled_step_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,53);
}
int anygm_synthetic_classic_filtered_step_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,54);
}
int anygm_synthetic_classic_scaled_filtered_step_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,61);
}
int anygm_synthetic_classic_root_handoff_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,62);
}
int anygm_synthetic_classic_scaled_root_handoff_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,63);
}
int anygm_synthetic_classic_filtered_root_handoff_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,64);
}
int anygm_synthetic_classic_scaled_filtered_root_handoff_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,65);
}
int anygm_synthetic_classic_sample_root_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,58);
}
int anygm_synthetic_classic_sample_resize_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,59);
}
int anygm_synthetic_classic_sample_filtered_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,60);
}
int anygm_synthetic_step_clear_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0,46);
}

int anygm_synthetic_classic_transition_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,15);
}

int anygm_synthetic_clear_view_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,1,0,0);
}

int anygm_synthetic_multiview_clear_view_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,1,0,0);
}

int anygm_synthetic_classic_framebuffer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1,0);
}

int anygm_synthetic_classic_multiview_framebuffer_content_create(
    AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,1,0);
}

/* The first placed instance deactivates the second before its Create. The second must complete
 * creation once, remain inactive, and retain its variables after activation. */
/* A room whose placement list names an object the project does not define. The record still
 * carries a position and an instance id, which is what a project that deleted an object leaves
 * behind, and the runtime has to put nothing there: there is no object to give the placement a
 * sprite, a depth or an event. */
int anygm_synthetic_undefined_placement_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192];
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,"global.fixture_created = 1;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent events[1]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instances[2]={0};
  int room_order=0;
  project.name=(char *)"undefined-placement-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  object.id=object.name=(char *)"obj_defined";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0;
  object.events=events; object.n_events=object.cap_events=1;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;

  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1;
  room.instances=instances; room.n_instances=room.cap_instances=2;
  instances[0].id=instances[0].name=(char *)"instance_defined"; instances[0].object_id=0;
  instances[0].instance_id=100000; instances[0].sx=instances[0].sy=1.0f;
  instances[0].color=0xFFFFFFFFu;
  /* The placement the project no longer defines. */
  instances[1].id=instances[1].name=(char *)"instance_undefined"; instances[1].object_id=-1;
  instances[1].instance_id=100001; instances[1].sx=instances[1].sy=1.0f;
  instances[1].color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"undefined-placement package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_room_deactivation_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char warden_create[192],warden_step[192],sleeper_create[192],sleeper_step[192];
  snprintf(warden_create,sizeof warden_create,"%s/warden_create.gml",fixture->directory);
  snprintf(warden_step,sizeof warden_step,"%s/warden_step.gml",fixture->directory);
  snprintf(sleeper_create,sizeof sleeper_create,"%s/sleeper_create.gml",fixture->directory);
  snprintf(sleeper_step,sizeof sleeper_step,"%s/sleeper_step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(warden_create,
                 "global.fixture_ticks = 0;\n"
                 "global.fixture_sleeper_created = 0;\n"
                 "global.fixture_sleeper_steps = 0;\n"
                 "global.fixture_sleeper_endurance = -1;\n"
                 "instance_deactivate_all(true);\n") ||
     !write_text(warden_step,
                 "global.fixture_ticks += 1;\n"
                 "if (global.fixture_ticks == 3) instance_activate_all();\n") ||
     !write_text(sleeper_create,
                 "global.fixture_sleeper_created += 1;\n"
                 "fixture_endurance = 150;\n") ||
     !write_text(sleeper_step,
                 "global.fixture_sleeper_steps += 1;\n"
                 "global.fixture_sleeper_endurance = fixture_endurance;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject objects[2]={0};
  GmlcObjectEvent warden_events[2]={0};
  GmlcObjectEvent sleeper_events[2]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instances[2]={0};
  int room_order=0;
  project.name=(char *)"room-deactivation-fixture";
  project.objects=objects; project.n_objects=project.cap_objects=2;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  objects[0].id=objects[0].name=(char *)"obj_warden";
  objects[0].sprite_id=objects[0].mask_id=objects[0].parent_id=-1;
  objects[0].visible=0;
  objects[0].events=warden_events; objects[0].n_events=objects[0].cap_events=2;
  warden_events[0].event_type=0; warden_events[0].event_number=0;
  warden_events[0].source_path=warden_create;
  warden_events[1].event_type=3; warden_events[1].event_number=0;
  warden_events[1].source_path=warden_step;

  objects[1].id=objects[1].name=(char *)"obj_sleeper";
  objects[1].sprite_id=objects[1].mask_id=objects[1].parent_id=-1;
  objects[1].visible=0;
  objects[1].events=sleeper_events; objects[1].n_events=objects[1].cap_events=2;
  sleeper_events[0].event_type=0; sleeper_events[0].event_number=0;
  sleeper_events[0].source_path=sleeper_create;
  sleeper_events[1].event_type=3; sleeper_events[1].event_number=0;
  sleeper_events[1].source_path=sleeper_step;

  room.id=room.name=(char *)"room_fixture"; room.width=64; room.height=48; room.speed=60;
  room.draw_background_color=1;
  room.instances=instances; room.n_instances=room.cap_instances=2;
  /* Room order is what puts the deactivation before the sleeper's own Create. */
  instances[0].id=instances[0].name=(char *)"instance_warden"; instances[0].object_id=0;
  instances[0].instance_id=100000; instances[0].sx=instances[0].sy=1.0f;
  instances[0].color=0xFFFFFFFFu;
  instances[1].id=instances[1].name=(char *)"instance_sleeper"; instances[1].object_id=1;
  instances[1].instance_id=100001; instances[1].sx=instances[1].sy=1.0f;
  instances[1].color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"room-deactivation package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_game_change_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char root_step[256],root_end[256],child_start[512],child_directory[256],child_path[512];
  snprintf(root_step,sizeof root_step,"%s/change-step.gml",fixture->directory);
  snprintf(root_end,sizeof root_end,"%s/change-end.gml",fixture->directory);
  snprintf(child_directory,sizeof child_directory,"%s/secondary",fixture->directory);
  snprintf(child_start,sizeof child_start,"%s/startup.gml",child_directory);
  snprintf(child_path,sizeof child_path,"%s/data.win",child_directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(anygm_test_mkdir(child_directory)!=0 ||
     !write_text(root_step,
                 "game_change(\"/secondary\", \"-game data.win child_marker \\\"quoted value\\\"\");\n") ||
     !write_text(root_end,
                 "ini_open(\"transition.ini\");\n"
                 "ini_write_real(\"state\", \"ended\", 1);\n"
                 "ini_close();\n") ||
     !write_text(child_start,
                 "ini_open(\"transition.ini\");\n"
                 "global.fixture_end_observed = ini_read_real(\"state\", \"ended\", 0);\n"
                 "ini_close();\n"
                 "global.fixture_anchor_marker = 3;\n"
                 "global.fixture_child_marker = 1;\n"
                 "global.fixture_parameter_count = parameter_count();\n"
                 "global.fixture_parameter_three = parameter_string(3);\n"
                 "global.fixture_parameter_four = parameter_string(4);\n"
                 "global.fixture_working_directory = working_directory;\n"
                 "global.fixture_program_directory = program_directory;\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  GmlcObject object={0};
  GmlcObjectEvent events[2]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0;
  object.events=events;
  object.n_events=object.cap_events=2;
  events[0].event_type=3;
  events[0].event_number=0;
  events[0].source_path=root_step;
  events[1].event_type=7;
  events[1].event_number=3;
  events[1].source_path=root_end;
  room.id=room.name=(char *)"room_primary";
  room.width=64;
  room.height=48;
  room.speed=60;
  room.draw_background_color=1;
  room.instances=&instance;
  room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;
  GmlcProject project={0};
  project.host=&file_services;
  project.name=(char *)"neutral-primary-fixture";
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order;
  project.n_room_order=1;
  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic primary package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  object.events=NULL;
  object.n_events=object.cap_events=0;
  room.id=room.name=(char *)"room_secondary";
  room.width=80;
  room.height=50;
  project.name=(char *)"neutral-secondary-fixture";
  project.startup_code_path=child_start;
  if(!gmlc_package_write_structural(&project,child_path,error,sizeof error)){
    fprintf(stderr,"synthetic secondary package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_game_restart_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char startup[192],step[192];
  snprintf(startup,sizeof startup,"%s/restart-startup.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/restart-step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(startup,"global.fixture_restart_boot = 1;\n") ||
     !write_text(step,"game_restart();\n")){
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }

  GmlcProject project={0};
  AnygmHostServices file_services={0};
  file_services.struct_size=sizeof file_services;
  file_services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&file_services);
  project.host=&file_services;
  GmlcObject object={0};
  GmlcObjectEvent event={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"neutral-game-restart-fixture";
  project.startup_code_path=startup;
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order;
  project.n_room_order=1;

  object.id=object.name=(char *)"obj_restart_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0;
  object.events=&event;
  object.n_events=object.cap_events=1;
  event.event_type=3;
  event.event_number=0;
  event.source_path=step;

  room.id=room.name=(char *)"room_restart_fixture";
  room.width=64;
  room.height=48;
  room.speed=60;
  room.draw_background_color=1;
  room.instances=&instance;
  room.n_instances=room.cap_instances=1;
  instance.id=instance.name=(char *)"instance_restart_fixture";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic game-restart package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

void anygm_synthetic_content_destroy(AnygmSyntheticContent *fixture){
  if(!fixture || !fixture->directory[0]) return;
  char path[256];
  snprintf(path,sizeof path,"%s/draw-policy.anygm",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/anchor-call.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/draw.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/startup.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/step.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/draw-pre.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/draw-normal.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/draw-post.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/background-create.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/retain-create.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/retain-step.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/retain-draw.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/retain-draw-begin.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/retain-draw-gui.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/change-step.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/change-end.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/restart-startup.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/restart-step.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/secondary/startup.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/secondary/data.win",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/secondary",fixture->directory);
  anygm_test_rmdir(path);
  snprintf(path,sizeof path,"%s/data.win",fixture->directory);
  anygm_test_unlink(path);
  const char *label=strrchr(fixture->directory,'/');
  label=label?label+1:fixture->directory;
  char save_path[256],save_root[256];
  snprintf(save_path,sizeof save_path,"%s/AnyGM/%s-%08x",fixture->directory,label,
           synthetic_path_hash(fixture->path));
  char transition_path[320];
  snprintf(transition_path,sizeof transition_path,"%s/transition.ini",save_path);
  anygm_test_unlink(transition_path);
  anygm_test_rmdir(save_path);
  snprintf(save_root,sizeof save_root,"%s/AnyGM",fixture->directory);
  anygm_test_rmdir(save_root);
  anygm_test_rmdir(fixture->directory);
  memset(fixture,0,sizeof *fixture);
}

int anygm_synthetic_content_read(const AnygmSyntheticContent *fixture,uint8_t **data,size_t *size){
  if(!fixture || !data || !size) return 0;
  *data=NULL;
  *size=0;
  FILE *file=fopen(fixture->path,"rb");
  if(!file || fseek(file,0,SEEK_END)!=0){ if(file) fclose(file); return 0; }
  long end=ftell(file);
  if(end<=0 || fseek(file,0,SEEK_SET)!=0){ fclose(file); return 0; }
  uint8_t *bytes=malloc((size_t)end);
  if(!bytes){ fclose(file); return 0; }
  int ok=fread(bytes,1,(size_t)end,file)==(size_t)end && fclose(file)==0;
  if(!ok){ free(bytes); return 0; }
  *data=bytes;
  *size=(size_t)end;
  return 1;
}
