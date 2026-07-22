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

int anygm_synthetic_content_create(AnygmSyntheticContent *fixture){
  if(!fixture) return 0;
  memset(fixture,0,sizeof *fixture);
  if(!create_fixture_directory(fixture->directory,sizeof fixture->directory)) return 0;

  char startup[192],step[192];
  snprintf(startup,sizeof startup,"%s/startup.gml",fixture->directory);
  snprintf(step,sizeof step,"%s/step.gml",fixture->directory);
  snprintf(fixture->path,sizeof fixture->path,"%s/content.win",fixture->directory);
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

void anygm_synthetic_content_destroy(AnygmSyntheticContent *fixture){
  if(!fixture || !fixture->directory[0]) return;
  char path[256];
  snprintf(path,sizeof path,"%s/startup.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/step.gml",fixture->directory);
  anygm_test_unlink(path);
  snprintf(path,sizeof path,"%s/content.win",fixture->directory);
  anygm_test_unlink(path);
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
