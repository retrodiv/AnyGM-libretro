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
#else
#include <unistd.h>
#define anygm_test_unlink unlink
#define anygm_test_rmdir rmdir
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
                 "randomize();\n"
                 "global.fixture_datetime = date_current_datetime();\n") ||
     !write_text(step,
                 "global.fixture_counter += 1;\n"
                 "global.fixture_clock = current_time;\n")){
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

static int synthetic_framebuffer_content_create(AnygmSyntheticContent *fixture,int multiview){
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
  return synthetic_framebuffer_content_create(fixture,0);
}

int anygm_synthetic_multiview_framebuffer_content_create(AnygmSyntheticContent *fixture){
  return synthetic_framebuffer_content_create(fixture,1);
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
  snprintf(path,sizeof path,"%s/data.win",fixture->directory);
  anygm_test_unlink(path);
  const char *label=strrchr(fixture->directory,'/');
  label=label?label+1:fixture->directory;
  char save_path[256],save_root[256];
  snprintf(save_path,sizeof save_path,"%s/anygm/%s-%08x",fixture->directory,label,
           synthetic_path_hash(fixture->path));
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
