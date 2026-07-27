/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "synthetic_content.h"
#include "gmlc_project.h"
#include "stdio_vfs.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t package_digest(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(14695981039346656037);
  for(size_t i=0;i<size;i++){
    hash^=data[i];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static int write_bytes(const char *path,const uint8_t *data,size_t size){
  FILE *file=fopen(path,"wb");
  int ok=file && fwrite(data,1,size,file)==size;
  if(file && fclose(file)!=0) ok=0;
  return ok;
}

static int write_text(const char *path,const char *text){
  return write_bytes(path,(const uint8_t *)text,strlen(text));
}

static int expect_modern_project_room_schema(const char *directory){
  char yyp[256], sprite[256], object[256], font[256], font_png[256];
  char first_room[256], second_room[256];
  snprintf(yyp,sizeof(yyp),"%s/project.yyp",directory);
  snprintf(sprite,sizeof(sprite),"%s/sprite.yy",directory);
  snprintf(object,sizeof(object),"%s/object.yy",directory);
  snprintf(font,sizeof(font),"%s/font.yy",directory);
  snprintf(font_png,sizeof(font_png),"%s/font_neutral.png",directory);
  snprintf(first_room,sizeof(first_room),"%s/first-room.yy",directory);
  snprintf(second_room,sizeof(second_room),"%s/second-room.yy",directory);
  const char *project_text=
    "{"
    "\"name\":\"neutral-modern-project\","
    "\"resources\":["
      "{\"Value\":{\"id\":\"sprite_neutral\",\"resourceType\":\"GMSprite\",\"resourcePath\":\"sprite.yy\"}},"
      "{\"Value\":{\"id\":\"font_neutral\",\"resourceType\":\"GMFont\",\"resourcePath\":\"font.yy\"}},"
      "{\"Value\":{\"id\":\"object_neutral\",\"resourceType\":\"GMObject\",\"resourcePath\":\"object.yy\"}},"
      "{\"Value\":{\"id\":\"room_second\",\"resourceType\":\"GMRoom\",\"resourcePath\":\"second-room.yy\"}},"
      "{\"Value\":{\"id\":\"room_first\",\"resourceType\":\"GMRoom\",\"resourcePath\":\"first-room.yy\"}}"
    "],"
    "\"RoomOrderNodes\":["
      "{\"roomId\":{\"name\":\"room_first\",\"path\":\"rooms/room_first/room_first.yy\"}},"
      "{\"roomId\":{\"name\":\"room_second\",\"path\":\"rooms/room_second/room_second.yy\"}}"
    "]"
    "}";
  const char *sprite_text=
    "{"
    "\"name\":\"sprite_neutral\",\"width\":16,\"height\":8,"
    "\"frames\":[],\"resourceType\":\"GMSprite\",\"resourceVersion\":\"2.0\""
    "}";
  const char *object_text=
    "{"
    "\"name\":\"object_neutral\","
    "\"spriteId\":{\"name\":\"sprite_neutral\",\"path\":\"sprites/sprite_neutral/sprite_neutral.yy\"},"
    "\"spriteMaskId\":null,\"parentObjectId\":null,\"eventList\":[],"
    "\"visible\":true,\"resourceType\":\"GMObject\",\"resourceVersion\":\"2.0\""
    "}";
  const char *font_text=
    "{"
    "\"name\":\"font_neutral\",\"size\":12,"
    "\"glyphs\":{\"65\":{\"character\":65,\"x\":2,\"y\":3,\"w\":4,\"h\":5,\"shift\":6,\"offset\":1}},"
    "\"resourceType\":\"GMFont\",\"resourceVersion\":\"2.0\""
    "}";
  const uint8_t font_png_header[24]={
    0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A,
    0,0,0,13,'I','H','D','R',
    0,0,0,16,0,0,0,8
  };
  const char *first_room_text=
    "{"
    "\"name\":\"room_first\","
    "\"roomSettings\":{\"Width\":320,\"Height\":180},"
    "\"viewSettings\":{\"enableViews\":true},"
    "\"views\":[{\"visible\":true,\"wview\":320,\"hview\":180,\"wport\":320,\"hport\":180,"
      "\"objectId\":{\"name\":\"object_neutral\",\"path\":\"objects/object_neutral/object_neutral.yy\"}}],"
    "\"layers\":["
      "{\"resourceType\":\"GMRInstanceLayer\",\"name\":\"Gameplay\",\"depth\":0,\"visible\":true,"
        "\"instances\":[{\"name\":\"instance_neutral\","
          "\"objectId\":{\"name\":\"object_neutral\",\"path\":\"objects/object_neutral/object_neutral.yy\"},"
          "\"x\":10,\"y\":20,\"scaleX\":1.5,\"scaleY\":0.5,\"rotation\":12,"
          "\"colour\":4294901760,\"ignore\":false}],\"layers\":[]},"
      "{\"resourceType\":\"GMRBackgroundLayer\",\"name\":\"Backdrop\",\"depth\":100,\"visible\":true,"
        "\"spriteId\":{\"name\":\"sprite_neutral\",\"path\":\"sprites/sprite_neutral/sprite_neutral.yy\"},"
        "\"colour\":4278190080,\"layers\":[]}"
    "],"
    "\"resourceType\":\"GMRoom\",\"resourceVersion\":\"2.0\""
    "}";
  const char *second_room_text=
    "{"
    "\"name\":\"room_second\",\"roomSettings\":{\"Width\":64,\"Height\":48},"
    "\"layers\":[],\"resourceType\":\"GMRoom\",\"resourceVersion\":\"2.0\""
    "}";
  GmlcProject project={0};
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  char error[256]={0};
  int ok=write_text(yyp,project_text) &&
         write_text(sprite,sprite_text) &&
         write_text(font,font_text) &&
         write_bytes(font_png,font_png_header,sizeof font_png_header) &&
         write_text(object,object_text) &&
         write_text(first_room,first_room_text) &&
         write_text(second_room,second_room_text) &&
         gmlc_project_load_yyp(&project,&services,yyp,error,sizeof error);
  if(!ok){
    fprintf(stderr,"modern project fixture failed to load: %s\n",error);
  }else{
    const GmlcRoom *room=&project.rooms[0];
    const GmlcRoomInstance *instance=room->n_instances?&room->instances[0]:NULL;
    ok=project.n_sprites==1 &&
       project.n_fonts==1 &&
       project.fonts[0].n_glyphs==1 &&
       project.fonts[0].glyphs[0].ch==65 &&
       project.fonts[0].glyphs[0].shift==6 &&
       project.n_objects==1 &&
       project.objects[0].sprite_id==0 &&
       project.n_rooms==2 &&
       !strcmp(project.rooms[0].name,"room_first") &&
       !strcmp(project.rooms[1].name,"room_second") &&
       room->n_layers==2 &&
       room->layers[0].type==2 &&
       !strcmp(room->layers[0].id,"Gameplay") &&
       room->layers[0].n_instance_ids==1 &&
       room->layers[1].type==1 &&
       room->layers[1].bg_sprite_id==0 &&
       room->layers[1].bg_color==UINT32_C(0xFF000000) &&
       room->views[0].object_id==0 &&
       room->n_instances==1 &&
       instance &&
       !strcmp(instance->id,"instance_neutral") &&
       instance->object_id==0 &&
       instance->x==10 &&
       instance->y==20 &&
       instance->sx==1.5f &&
       instance->sy==0.5f &&
       instance->rotation==12.0f &&
       instance->color==UINT32_C(0xFFFF0000);
    if(!ok) fputs("modern project room schema was not normalized\n",stderr);
  }
  gmlc_project_free(&project);
  remove(second_room);
  remove(first_room);
  remove(object);
  remove(font_png);
  remove(font);
  remove(sprite);
  remove(yyp);
  return ok;
}

int main(int argc,char **argv){
  AnygmSyntheticContent first={0}, second={0};
  uint8_t *first_bytes=NULL, *second_bytes=NULL;
  size_t first_size=0, second_size=0;
  int ok=anygm_synthetic_content_create(&first)
      && anygm_synthetic_content_create(&second)
      && anygm_synthetic_content_read(&first,&first_bytes,&first_size)
      && anygm_synthetic_content_read(&second,&second_bytes,&second_size);
  if(!ok){
    fputs("could not create structural package fixtures\n",stderr);
  }else if(first_size!=second_size ||
           memcmp(first_bytes,second_bytes,first_size)!=0){
    fprintf(stderr,
            "structural package output is not byte-identical: %zu versus %zu bytes\n",
            first_size,second_size);
    ok=0;
  }

  uint64_t digest=ok?package_digest(first_bytes,first_size):0;
  if(ok && (first_size!=2968 ||
            digest!=UINT64_C(0x78208d57266661e0))){
    fprintf(stderr,
            "structural package golden changed: %zu bytes, fnv64=%016" PRIx64 "\n",
            first_size,digest);
    ok=0;
  }
  if(ok && argc==2 && !write_bytes(argv[1],first_bytes,first_size)){
    fprintf(stderr,"could not retain structural package fixture at %s\n",argv[1]);
    ok=0;
  }else if(argc>2){
    fputs("usage: test_package [output-path]\n",stderr);
    ok=0;
  }
  if(ok && !expect_modern_project_room_schema(first.directory)) ok=0;

  free(first_bytes);
  free(second_bytes);
  anygm_synthetic_content_destroy(&first);
  anygm_synthetic_content_destroy(&second);
  if(ok)
    printf("structural package bytes: ok (%zu bytes, fnv64=%016" PRIx64 ")\n",
           first_size,digest);
  return ok?0:1;
}
