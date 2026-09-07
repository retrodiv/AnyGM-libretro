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
    AnygmSyntheticContent *fixture,int anchor_script,int list_fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char startup[192],create[192],step[192],anchor_call[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(anchor_call,sizeof anchor_call,"%s/anchor-call.gml",fixture->directory);
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
     (anchor_script && !write_text(anchor_call,"global.fixture_anchor_calls += 1;\n"))){
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
  object.visible=0;
  object.events=events;
  object.n_events=object.cap_events=list_fixture?2:1;
  if(list_fixture){
    events[0].event_type=0;
    events[0].event_number=0;
    events[0].source_path=create;
  }
  events[list_fixture?1:0].event_type=3;
  events[list_fixture?1:0].event_number=0;
  events[list_fixture?1:0].source_path=step;

  room.id=room.name=(char *)"room_fixture";
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

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}
int anygm_synthetic_content_create(AnygmSyntheticContent *fixture){
  return synthetic_content_create(fixture,0,0);
}
int anygm_synthetic_list_override_content_create(AnygmSyntheticContent *fixture){
  return synthetic_content_create(fixture,0,1);
}
int anygm_synthetic_anchor_script_content_create(AnygmSyntheticContent *fixture){
  return synthetic_content_create(fixture,1,0);
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
                 "while (global.fixture_release == 0) { sleep(10); }\n"
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

static int synthetic_framebuffer_content_create(
    AnygmSyntheticContent *fixture,int multiview,int clear_view_background,int classic){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char create[192],step[192];
  snprintf(create,sizeof create,"%s/retain-create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/retain-step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,"global.fixture_frame = 0;\n") ||
     !write_text(step,
                 "if (global.fixture_frame >= 1) room_goto(1);\n"
                 "global.fixture_frame += 1;\n")){
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
  GmlcRoom rooms[2]={0};
  GmlcRoomInstance instance={0};
  int room_order[2]={0,1};
  project.name=(char *)"neutral-framebuffer-fixture";
  project.objects=&object;
  project.n_objects=project.cap_objects=1;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;
  if(classic){
    project.classic_version=800;
    /* Distinct from both room colors so a cleared classic frame cannot be mistaken for a
     * retained one or for the room the fixture painted first. */
    project.classic_outside_color=0x00204060u;
  }

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0;
  object.events=events;
  object.n_events=object.cap_events=2;
  events[0].event_type=0;
  events[0].event_number=0;
  events[0].source_path=create;
  events[1].event_type=3;
  events[1].event_number=0;
  events[1].source_path=step;

  rooms[0].id=rooms[0].name=(char *)"room_painted";
  rooms[0].width=64;
  rooms[0].height=48;
  rooms[0].speed=60;
  rooms[0].background_color=0xFF336699u;
  rooms[0].draw_background_color=1;
  rooms[0].instances=&instance;
  rooms[0].n_instances=rooms[0].cap_instances=1;
  instance.id=instance.name=(char *)"instance_fixture";
  instance.object_id=0;
  instance.instance_id=100000;
  instance.sx=instance.sy=1.0f;
  instance.color=0xFFFFFFFFu;

  rooms[1].id=rooms[1].name=(char *)"room_retained";
  rooms[1].width=64;
  rooms[1].height=48;
  rooms[1].speed=60;
  rooms[1].background_color=0xFF000000u;
  rooms[1].draw_background_color=0;
  rooms[1].clear_view_background=clear_view_background;
  if(multiview){
    for(int room=0;room<2;room++){
      rooms[room].view_enabled=1;
      rooms[room].n_views=2;
      for(int view=0;view<2;view++){
        rooms[room].views[view].visible=1;
        rooms[room].views[view].xview=view*32;
        rooms[room].views[view].wview=rooms[room].views[view].wport=32;
        rooms[room].views[view].hview=rooms[room].views[view].hport=48;
        rooms[room].views[view].xport=view*32;
        rooms[room].views[view].hspeed=rooms[room].views[view].vspeed=-1;
        rooms[room].views[view].object_id=-1;
      }
    }
  }

  char error[256]={0};
  if(!gmlc_package_write_structural(&project,fixture->path,error,sizeof error)){
    fprintf(stderr,"synthetic framebuffer package failed: %s\n",error);
    anygm_synthetic_content_destroy(fixture);
    return 0;
  }
  return 1;
}

int anygm_synthetic_framebuffer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,0);
}

int anygm_synthetic_multiview_framebuffer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,0);
}

int anygm_synthetic_clear_view_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,1,0);
}

int anygm_synthetic_multiview_clear_view_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,1,0);
}

int anygm_synthetic_classic_framebuffer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,0,0,1);
}

int anygm_synthetic_classic_multiview_framebuffer_content_create(
    AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1,0,1);
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
  snprintf(save_path,sizeof save_path,"%s/anygm/%s-%08x",fixture->directory,label,
           synthetic_path_hash(fixture->path));
  char transition_path[320];
  snprintf(transition_path,sizeof transition_path,"%s/transition.ini",save_path);
  anygm_test_unlink(transition_path);
  anygm_test_rmdir(save_path);
  snprintf(save_root,sizeof save_root,"%s/anygm",fixture->directory);
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
