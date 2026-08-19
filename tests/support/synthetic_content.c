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

int anygm_synthetic_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char startup[192],step[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(startup,
                 "global.fixture_counter = 0;\n"
                 "global.fixture_presses = 0;\n"
                 "randomize();\n"
                 "global.fixture_datetime = date_current_datetime();\n") ||
     !write_text(step,
                 "global.fixture_counter += 1;\n"
                 "global.fixture_clock = current_time;\n"
                 "if (keyboard_check_pressed(vk_anykey)) global.fixture_presses += 1;\n")){
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

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0;
  object.events=&event;
  object.n_events=object.cap_events=1;
  event.event_type=3;
  event.event_number=0;
  event.source_path=step;

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

  char startup[192],create[192],step[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(startup,"global.advance = 0;\n") ||
     !write_text(create,"scale = 3;\n") ||
     !write_text(step,
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
  GmlcObjectEvent events[2]={0};
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
  object.visible=0;
  object.events=events;
  object.n_events=object.cap_events=2;
  events[0].event_type=0;
  events[0].event_number=0;
  events[0].source_path=create;
  events[1].event_type=3;
  events[1].event_number=0;
  events[1].source_path=step;

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

  char create[192],begin[192],step[192];
  snprintf(create,sizeof create,"%s/create.gml",fixture->directory);
  snprintf(begin,sizeof begin,"%s/begin.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/data.win",fixture->directory);
  if(!write_text(create,
                 "global.fixture_steps = 0;\n"
                 "global.fixture_held = 0;\n") ||
     !write_text(begin,
                 "if (keyboard_check(vk_right)) global.fixture_held += 1;\n") ||
     !write_text(step,
                 "global.fixture_steps += 1;\n"
                 "if (global.fixture_steps <= 5)\n"
                 "{\n"
                 "    keyboard_key_press(vk_right);\n"
                 "    keyboard_key_release(vk_right);\n"
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
  GmlcObject object={0};
  GmlcObjectEvent events[3]={0};
  GmlcRoom room={0};
  GmlcRoomInstance instance={0};
  int room_order=0;
  project.name=(char *)"bridged-hold-fixture";
  project.objects=&object; project.n_objects=project.cap_objects=1;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&room_order; project.n_room_order=1;

  object.id=object.name=(char *)"obj_fixture";
  object.sprite_id=object.mask_id=object.parent_id=-1;
  object.visible=0; object.events=events; object.n_events=object.cap_events=3;
  events[0].event_type=0; events[0].event_number=0; events[0].source_path=create;
  events[1].event_type=3; events[1].event_number=1; events[1].source_path=begin;
  events[2].event_type=3; events[2].event_number=0; events[2].source_path=step;
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
