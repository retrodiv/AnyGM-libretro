/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "synthetic_content.h"
#include "gmlc_package.h"
#include "gmlc_project.h"
#include "gml_win.h"
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

static uint32_t read_u32le(const uint8_t *data,size_t size,size_t offset,int *ok){
  if(offset>size || size-offset<4){
    *ok=0;
    return 0;
  }
  return (uint32_t)data[offset] |
         ((uint32_t)data[offset+1]<<8) |
         ((uint32_t)data[offset+2]<<16) |
         ((uint32_t)data[offset+3]<<24);
}

static int read_bytes(const char *path,uint8_t **data,size_t *size){
  FILE *file=fopen(path,"rb");
  if(!file || fseek(file,0,SEEK_END)!=0){
    if(file) fclose(file);
    return 0;
  }
  long length=ftell(file);
  if(length<0 || fseek(file,0,SEEK_SET)!=0){
    fclose(file);
    return 0;
  }
  uint8_t *bytes=(uint8_t *)malloc((size_t)length ? (size_t)length : 1);
  int ok=bytes && fread(bytes,1,(size_t)length,file)==(size_t)length;
  if(fclose(file)!=0) ok=0;
  if(!ok){
    free(bytes);
    return 0;
  }
  *data=bytes;
  *size=(size_t)length;
  return 1;
}

static int expect_tileset_source_indices(const char *directory){
  char package_path[256];
  snprintf(package_path,sizeof package_path,"%s/tileset-indices.win",directory);
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcTileset tileset={0};
  tileset.id=tileset.name=(char *)"neutral_tileset";
  tileset.sprite_id=-1;
  tileset.tile_width=tileset.tile_height=16;
  tileset.columns=2;
  tileset.tile_count=4;
  GmlcProject project={0};
  project.host=&services;
  project.name=(char *)"neutral-tileset-indices";
  project.tilesets=&tileset;
  project.n_tilesets=project.cap_tilesets=1;
  char error[256]={0};
  uint8_t *data=NULL;
  size_t size=0;
  int ok=gmlc_package_write_structural(&project,package_path,error,sizeof error) &&
         read_bytes(package_path,&data,&size);
  if(!ok){
    fprintf(stderr,"tileset source-index package failed: %s\n",error);
  }else{
    size_t chunk=8, background=0;
    while(chunk<=size && size-chunk>=8){
      int bounded=1;
      uint32_t chunk_size=read_u32le(data,size,chunk+4,&bounded);
      size_t next=chunk+8+(size_t)chunk_size;
      if(!bounded || next<chunk || next>size){ ok=0; break; }
      if(!memcmp(data+chunk,"BGND",4)){ background=chunk+8; break; }
      chunk=next;
    }
    int bounded=background!=0;
    uint32_t count=bounded?read_u32le(data,size,background,&bounded):0;
    uint32_t record=count==1?read_u32le(data,size,background+4,&bounded):0;
    uint32_t ids[4]={0};
    for(size_t i=0;i<4 && bounded;i++)
      ids[i]=read_u32le(data,size,(size_t)record+64+i*4,&bounded);
    ok=bounded && count==1 && ids[0]==0 && ids[1]==1 && ids[2]==2 && ids[3]==3;
    if(!ok){
      fprintf(stderr,"tileset source indices were [%" PRIu32 ", %" PRIu32 ", %" PRIu32 ", %" PRIu32 "]\n",
              ids[0],ids[1],ids[2],ids[3]);
    }
  }
  free(data);
  remove(package_path);
  return ok;
}

static int expect_classic_room_order_display_extent(const char *directory){
  char package_path[256];
  snprintf(package_path,sizeof package_path,"%s/classic-room-order.win",directory);
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcRoom rooms[2]={0};
  rooms[0].id=rooms[0].name=(char *)"room_storage_first";
  rooms[0].width=320;
  rooms[0].height=240;
  rooms[0].speed=30;
  rooms[1].id=rooms[1].name=(char *)"room_boot_first";
  rooms[1].width=800;
  rooms[1].height=600;
  rooms[1].speed=30;
  int room_order[2]={1,0};
  GmlcProject project={0};
  project.host=&services;
  project.name=(char *)"neutral-classic-room-order";
  project.classic_version=530;
  project.classic_scaling=100;
  project.rooms=rooms;
  project.n_rooms=project.cap_rooms=2;
  project.room_order=room_order;
  project.n_room_order=2;
  char error[256]={0};
  uint8_t *data=NULL;
  size_t size=0;
  GmlWin win={0};
  int ok=gmlc_package_write_structural(&project,package_path,error,sizeof error) &&
         read_bytes(package_path,&data,&size) &&
         gml_win_from_mem(&win,data,size,0)==0;
  if(!ok){
    fprintf(stderr,"classic room-order display fixture failed: %s\n",
            error[0]?error:gml_win_last_load_error());
  }else{
    ok=win.disp_w==800 && win.disp_h==600 && win.n_room_order==2 &&
       win.room_order[0]==1 && win.room_order[1]==0;
    if(!ok)
      fprintf(stderr,"classic display ignored boot room order: %ux%u\n",win.disp_w,win.disp_h);
  }
  gml_win_free(&win);
  free(data);
  remove(package_path);
  return ok;
}

/* The package stores one signed display-scaling word in OPTN. A synthetic structural
 * package supplies its surrounding layout; each policy value is written into that one
 * word to check its offset and signed roundtrip through the reader. */
static int expect_optn_scale_is_read(const char *directory){
  static const int32_t policies[]={-1,0,100};
  char package_path[256];
  snprintf(package_path,sizeof package_path,"%s/optn-scale.win",directory);
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlcRoom room={0};
  room.id=room.name=(char *)"room_only";
  room.width=320;
  room.height=240;
  room.speed=30;
  int room_order[1]={0};
  GmlcProject project={0};
  project.host=&services;
  project.name=(char *)"neutral-scaling-policy";
  project.rooms=&room;
  project.n_rooms=project.cap_rooms=1;
  project.room_order=room_order;
  project.n_room_order=1;
  char error[256]={0};
  uint8_t *data=NULL;
  size_t size=0;
  GmlWin win={0};
  uint32_t scale_offset=0;
  int ok=gmlc_package_write_structural(&project,package_path,error,sizeof error) &&
         read_bytes(package_path,&data,&size) &&
         gml_win_from_mem(&win,data,size,0)==0;
  if(!ok){
    fprintf(stderr,"scaling-policy fixture failed: %s\n",
            error[0]?error:gml_win_last_load_error());
  }else{
    for(int i=0;i<win.n_chunks;i++)
      if(!strcmp(win.chunks[i].name,"OPTN") && win.chunks[i].size>=20u)
        scale_offset=win.chunks[i].off+16u;   /* marker, unknown word, 64-bit flags, then scale */
    ok=scale_offset!=0;
    if(!ok) fputs("the structural package carries no readable OPTN\n",stderr);
  }
  gml_win_free(&win);
  for(size_t index=0;ok && index<sizeof policies/sizeof policies[0];index++){
    int32_t wanted=policies[index];
    uint32_t bits=(uint32_t)wanted;
    data[scale_offset+0]=(uint8_t)bits;
    data[scale_offset+1]=(uint8_t)(bits>>8);
    data[scale_offset+2]=(uint8_t)(bits>>16);
    data[scale_offset+3]=(uint8_t)(bits>>24);
    GmlWin restated={0};
    ok=gml_win_from_mem(&restated,data,size,0)==0 && restated.option_scaling==wanted;
    if(!ok)
      fprintf(stderr,"a scaling policy of %d was read back as %d\n",
              wanted,restated.option_scaling);
    gml_win_free(&restated);
  }
  free(data);
  remove(package_path);
  return ok;
}

static int expect_modern_project_room_schema(const char *directory){
  char yyp[256], sprite[256], sprite_png[256], object[256], font[256], font_png[256];
  char tileset[256], tileset_output[256];
  char first_room[256], second_room[256];
  snprintf(yyp,sizeof(yyp),"%s/project.yyp",directory);
  snprintf(sprite,sizeof(sprite),"%s/sprite.yy",directory);
  snprintf(sprite_png,sizeof(sprite_png),"%s/sprite-frame.png",directory);
  snprintf(object,sizeof(object),"%s/object.yy",directory);
  snprintf(font,sizeof(font),"%s/font.yy",directory);
  snprintf(font_png,sizeof(font_png),"%s/font_neutral.png",directory);
  snprintf(tileset,sizeof(tileset),"%s/tileset.yy",directory);
  snprintf(tileset_output,sizeof(tileset_output),"%s/output_tileset.png",directory);
  snprintf(first_room,sizeof(first_room),"%s/first-room.yy",directory);
  snprintf(second_room,sizeof(second_room),"%s/second-room.yy",directory);
  const char *project_text=
    "{"
    "\"name\":\"neutral-modern-project\","
    "\"resources\":["
      "{\"Value\":{\"id\":\"sprite_neutral\",\"resourceType\":\"GMSprite\",\"resourcePath\":\"sprite.yy\"}},"
      "{\"Value\":{\"id\":\"tileset_neutral\",\"resourceType\":\"GMTileSet\",\"resourcePath\":\"tileset.yy\"}},"
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
    "\"name\":\"sprite_neutral\",\"width\":128,\"height\":128,"
    "\"frames\":[{\"id\":\"sprite-frame\"}],\"resourceType\":\"GMSprite\",\"resourceVersion\":\"2.0\""
    "}";
  const char *tileset_text=
    "{"
    "\"name\":\"tileset_neutral\","
    "\"spriteId\":\"sprite_neutral\",\"sprite_no_export\":true,"
    "\"tilewidth\":32,\"tileheight\":32,\"out_tilehborder\":2,\"out_tilevborder\":2,"
    "\"out_columns\":4,\"tile_count\":16,"
    "\"resourceType\":\"GMTileSet\",\"resourceVersion\":\"1.0\""
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
  uint8_t sprite_png_header[24];
  uint8_t tileset_png_header[24];
  memcpy(sprite_png_header,font_png_header,sizeof sprite_png_header);
  memcpy(tileset_png_header,font_png_header,sizeof tileset_png_header);
  sprite_png_header[19]=128;
  sprite_png_header[23]=128;
  tileset_png_header[18]=tileset_png_header[22]=0;
  tileset_png_header[19]=tileset_png_header[23]=144;
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
         write_bytes(sprite_png,sprite_png_header,sizeof sprite_png_header) &&
         write_text(tileset,tileset_text) &&
         write_bytes(tileset_output,tileset_png_header,sizeof tileset_png_header) &&
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
       project.n_tilesets==1 &&
       project.tilesets[0].columns==4 &&
       project.tilesets[0].tile_count==16 &&
       project.sprites[0].width==144 &&
       project.sprites[0].height==144 &&
       project.sprites[0].n_frames==1 &&
       !strcmp(project.sprites[0].frame_paths[0],tileset_output) &&
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
  remove(tileset_output);
  remove(tileset);
  remove(sprite_png);
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
  if(ok && (first_size!=3096 ||
            digest!=UINT64_C(0x0cb45aa3a9954196))){
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
  if(ok && !expect_tileset_source_indices(first.directory)) ok=0;
  if(ok && !expect_classic_room_order_display_extent(first.directory)) ok=0;
  if(ok && !expect_optn_scale_is_read(first.directory)) ok=0;

  free(first_bytes);
  free(second_bytes);
  anygm_synthetic_content_destroy(&first);
  anygm_synthetic_content_destroy(&second);
  if(ok)
    printf("structural package bytes: ok (%zu bytes, fnv64=%016" PRIx64 ")\n",
           first_size,digest);
  return ok?0:1;
}
