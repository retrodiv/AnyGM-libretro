/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"
#include "gml_image_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_GIF
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static AnygmHostServices fixture_host;
void classic_fixture_host_init(void){
  fixture_host.struct_size=sizeof fixture_host;
  fixture_host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&fixture_host);
}

AnygmHostServices *classic_fixture_host(void){
  return &fixture_host;
}

unsigned char *classic_fixture_load_png(const char *path, int *width,
                                        int *height, int *components){
  return stbi_load(path,width,height,components,4);
}

void classic_fixture_free_image(void *image){
  stbi_image_free(image);
}

void fixture_project_init(GmlcProject *project){
  gmlc_project_init(project);
  project->host=&fixture_host;
}

void fixture_project_clear(GmlcProject *project){
  memset(project,0,sizeof *project);
  project->host=&fixture_host;
}

void put_u32le(unsigned char *p, unsigned value){
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

unsigned get_u32le(const unsigned char *p){
  return (unsigned)p[0] | (unsigned)p[1] << 8 |
         (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* Fixtures exercise the parser with authored normalized images. */
unsigned char *fixture_project_image(const unsigned char *plain,size_t plain_size,
                                      size_t *image_size){
  unsigned char *image=(unsigned char*)malloc(plain_size?plain_size:1u);
  if(!image) return NULL;
  if(plain_size) memcpy(image,plain,plain_size);
  *image_size=plain_size;
  return image;
}

void fixture_compressed(Fixture *f, const unsigned char *raw, int raw_size);

void fixture_u32(Fixture *f, unsigned value){
  put_u32le(f->data + f->size, value);
  f->size += 4;
}

void fixture_zero(Fixture *f, size_t count){
  memset(f->data + f->size, 0, count);
  f->size += count;
}

void fixture_string(Fixture *f, const char *text){
  size_t length = text ? strlen(text) : 0;
  /* Every other field here is a fixed handful of bytes, but an embedded source is as long as its
   * author made it. Refuse loudly rather than write past the buffer and report a container defect
   * somewhere else entirely. */
  if(f->size + 4 + length > sizeof(f->data)) abort();
  fixture_u32(f, (unsigned)length);
  if(length){
    memcpy(f->data + f->size, text, length);
    f->size += length;
  }
}

void fixture_double(Fixture *f, double value){
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  for(unsigned i = 0; i < 8; ++i) f->data[f->size++] = (unsigned char)(bits >> (i * 8));
}

void fixture_compressed(Fixture *f, const unsigned char *raw, int raw_size){
  GmlMediaBuffer compressed={0};
  if(raw_size<0 || !gml_deflate_encode_zlib(raw,(size_t)raw_size,&compressed) ||
     f->size+4u+compressed.size>sizeof f->data) abort();
  fixture_u32(f,(unsigned)compressed.size);
  memcpy(f->data+f->size,compressed.data,compressed.size);
  f->size+=compressed.size;
  gml_media_buffer_release(&compressed);
}

Fixture game_information_fixture(void){
  Fixture f={{0},0};
  fixture_u32(&f,0xFF000018u); fixture_u32(&f,1);
  fixture_string(&f,"Information");
  fixture_u32(&f,(unsigned)-1); fixture_u32(&f,(unsigned)-1);
  fixture_u32(&f,600); fixture_u32(&f,400);
  fixture_u32(&f,1); fixture_u32(&f,1); fixture_u32(&f,0); fixture_u32(&f,1);
  fixture_double(&f,40000.0);
  fixture_string(&f,"{\\rtf1\\ansi\\pard\\qc\\b\\fs32 Generic information\\par\\b0\\fs24 Neutral fixture text\\par}");
  return f;
}

Fixture manifest_fixture(unsigned container_version){
  return manifest_fixture_source(container_version,NULL);
}

Fixture manifest_fixture_source(unsigned container_version, const char *gml){
  Fixture f = {{0}, 0};
  fixture_u32(&f, GMLC_CLASSIC_MAGIC); fixture_u32(&f, container_version);
  fixture_u32(&f, 9); fixture_zero(&f, 16);
  fixture_u32(&f, 800); fixture_u32(&f, 0);
  fixture_u32(&f, 800); fixture_u32(&f, 1);
  { Fixture trigger={{0},0};
    fixture_u32(&trigger,1); fixture_u32(&trigger,800);
    fixture_string(&trigger,"fixture_trigger"); fixture_string(&trigger,"global.ready");
    fixture_u32(&trigger,1); fixture_string(&trigger,"fixture_trigger_constant");
    fixture_compressed(&f,trigger.data,(int)trigger.size); }
  fixture_zero(&f, 8);
  fixture_u32(&f, 800); fixture_u32(&f, 2);
  fixture_string(&f,"fixture_number"); fixture_string(&f,"6*7");
  fixture_string(&f,"fixture_text"); fixture_string(&f,"\"ready\"");
  fixture_zero(&f, 8);
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, 800); fixture_u32(&f, 1);
    if(type == GMLC_CLASSIC_SCRIPT){
      unsigned char raw[128] = {0};
      size_t n = 0;
      put_u32le(raw + n, 1); n += 4;
      const char name[] = "resource_script";
      put_u32le(raw + n, sizeof(name) - 1); n += 4;
      memcpy(raw + n, name, sizeof(name) - 1); n += sizeof(name) - 1;
      n += 8;
      put_u32le(raw + n, 800); n += 4;
      const char source[] = "return 7;";
      put_u32le(raw + n, sizeof(source) - 1); n += 4;
      memcpy(raw + n, source, sizeof(source) - 1); n += sizeof(source) - 1;
      fixture_compressed(&f, raw, (int)n);
    } else {
      const unsigned char absent[4] = {0, 0, 0, 0};
      fixture_compressed(&f, absent, sizeof(absent));
    }
  }
  fixture_u32(&f, 100001); fixture_u32(&f, 1000001);
  fixture_u32(&f, 800); fixture_u32(&f, 1); /* included files */
  { Fixture included={{0},0}; const unsigned char contents[]={4,5,6,7};
    fixture_zero(&included,8); fixture_u32(&included,800);
    fixture_string(&included,"fixture.dat"); fixture_string(&included,"source/fixture.dat");
    fixture_u32(&included,1); fixture_u32(&included,sizeof(contents)); fixture_u32(&included,1);
    fixture_u32(&included,sizeof(contents)); memcpy(included.data+included.size,contents,sizeof(contents)); included.size+=sizeof(contents);
    fixture_u32(&included,2); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,0); fixture_u32(&included,0);
    fixture_compressed(&f,included.data,(int)included.size); }
  fixture_u32(&f, 700); fixture_u32(&f, 1); /* extensions */
  fixture_string(&f,"fixture_extension");
  fixture_u32(&f, 800); /* game information */
  { Fixture information=game_information_fixture();
    fixture_compressed(&f,information.data,(int)information.size); }
  fixture_u32(&f, 500); fixture_u32(&f, gml ? 2 : 1); /* library code */
  fixture_string(&f,"global.fixture_started = 1;");
  if(gml) fixture_string(&f,gml);
  fixture_u32(&f, 700); fixture_u32(&f, 0); /* executable rooms */
  return f;
}

int build_executable_fixture(Fixture *executable){
  Fixture decoded={{0},0};
  fixture_u32(&decoded,0); /* leading junk */
  fixture_u32(&decoded,1); fixture_u32(&decoded,0x13572468); fixture_zero(&decoded,16);
  fixture_u32(&decoded,700); fixture_u32(&decoded,1); /* extensions */
  fixture_u32(&decoded,700); fixture_string(&decoded,"fixture_executable_extension"); fixture_string(&decoded,"fixture_folder");
  fixture_u32(&decoded,1); fixture_u32(&decoded,700); fixture_string(&decoded,"fixture.bin");
  fixture_u32(&decoded,4); fixture_string(&decoded,"global.fixture_extension_started = 1;"); fixture_string(&decoded,"");
  fixture_u32(&decoded,1); /* functions */
  fixture_u32(&decoded,700); fixture_string(&decoded,"fixture_function");
  fixture_string(&decoded,"fixture_external");
  for(unsigned word=0;word<GMLC_CLASSIC_EXTENSION_FUNCTION_WORDS;word++)
    fixture_u32(&decoded,100u+word);
  fixture_u32(&decoded,1); /* constants */
  fixture_u32(&decoded,700); fixture_string(&decoded,"fixture_extension_constant"); fixture_string(&decoded,"7*6");
  fixture_u32(&decoded,4); fixture_u32(&decoded,0); /* encrypted data envelope and seed */
  fixture_u32(&decoded,800); fixture_u32(&decoded,1); /* triggers */
  { Fixture trigger={{0},0};
    fixture_u32(&trigger,1); fixture_u32(&trigger,800);
    fixture_string(&trigger,"fixture_executable_trigger"); fixture_string(&trigger,"global.ready");
    fixture_u32(&trigger,2); fixture_string(&trigger,"fixture_executable_trigger_constant");
    fixture_compressed(&decoded,trigger.data,(int)trigger.size); }
  fixture_u32(&decoded,800); fixture_u32(&decoded,1); /* constants */
  fixture_string(&decoded,"fixture_executable_constant"); fixture_string(&decoded,"21*2");
  for(unsigned type=0;type<GMLC_CLASSIC_RESOURCE_TYPES;type++){
    fixture_u32(&decoded,800);
    if(type==GMLC_CLASSIC_SCRIPT){
      Fixture script={{0},0};
      fixture_u32(&decoded,1);
      fixture_u32(&script,1);
      fixture_string(&script,"fixture_script");
      fixture_u32(&script,800);
      fixture_string(&script,"exit;");
      fixture_compressed(&decoded,script.data,(int)script.size);
    } else if(type==GMLC_CLASSIC_SPRITE){
      Fixture sprite={{0},0};
      fixture_u32(&decoded,1);
      fixture_u32(&sprite,1); fixture_string(&sprite,"fixture_empty_sprite");
      fixture_u32(&sprite,800); fixture_u32(&sprite,0); fixture_u32(&sprite,0);
      fixture_u32(&sprite,0); /* frames */
      fixture_u32(&sprite,0); /* compiled empty-sprite collision flag */
      fixture_compressed(&decoded,sprite.data,(int)sprite.size);
    } else if(type==GMLC_CLASSIC_BACKGROUND){
      Fixture background={{0},0}; const unsigned char bgra[]={3,2,1,255};
      fixture_u32(&decoded,1);
      fixture_u32(&background,1); fixture_string(&background,"fixture_executable_background");
      fixture_u32(&background,800); fixture_u32(&background,800);
      fixture_u32(&background,1); fixture_u32(&background,1); fixture_u32(&background,sizeof(bgra));
      memcpy(background.data+background.size,bgra,sizeof(bgra)); background.size+=sizeof(bgra);
      fixture_compressed(&decoded,background.data,(int)background.size);
    } else if(type==GMLC_CLASSIC_FONT){
      Fixture font={{0},0};
      fixture_u32(&decoded,1);
      fixture_u32(&font,1); fixture_string(&font,"fixture_compiled_font"); fixture_u32(&font,800);
      fixture_string(&font,"fixture face"); fixture_u32(&font,10); fixture_u32(&font,1);
      fixture_u32(&font,0); fixture_u32(&font,65); fixture_u32(&font,66);
      for(unsigned entry=0;entry<256u*6u;entry++){
        unsigned value=0;
        if(entry==65u*6u+2u || entry==65u*6u+3u) value=1;
        else if(entry==65u*6u+4u) value=3;
        else if(entry==66u*6u) value=1;
        else if(entry==66u*6u+2u || entry==66u*6u+3u) value=1;
        else if(entry==66u*6u+4u) value=4;
        else if(entry==66u*6u+5u) value=(unsigned)-1;
        fixture_u32(&font,value);
      }
      fixture_u32(&font,2); fixture_u32(&font,1); fixture_u32(&font,2);
      font.data[font.size++]=17; font.data[font.size++]=231;
      fixture_compressed(&decoded,font.data,(int)font.size);
    } else if(type==GMLC_CLASSIC_ROOM){
      Fixture room={{0},0};
      fixture_u32(&decoded,1);
      fixture_u32(&room,1); fixture_string(&room,"fixture_executable_room"); fixture_u32(&room,800);
      fixture_string(&room,""); fixture_u32(&room,320); fixture_u32(&room,240);
      fixture_u32(&room,30); fixture_u32(&room,0); fixture_u32(&room,0); fixture_u32(&room,1);
      fixture_string(&room,""); fixture_u32(&room,0);
      fixture_u32(&room,0); fixture_u32(&room,0);
      fixture_u32(&room,1); fixture_u32(&room,10); fixture_u32(&room,20);
      fixture_u32(&room,0); fixture_u32(&room,100001);
      fixture_string(&room,"x += 1;");
      fixture_u32(&room,1);
      fixture_u32(&room,30); fixture_u32(&room,40); fixture_u32(&room,0);
      fixture_u32(&room,0); fixture_u32(&room,0); fixture_u32(&room,1);
      fixture_u32(&room,1); fixture_u32(&room,100); fixture_u32(&room,1000001);
      fixture_compressed(&decoded,room.data,(int)room.size);
    } else fixture_u32(&decoded,0);
  }
  fixture_u32(&decoded,100000); fixture_u32(&decoded,1000001);
  fixture_u32(&decoded,800); fixture_u32(&decoded,1); /* includes */
  { Fixture included={{0},0}; const unsigned char contents[]={10,11};
    fixture_u32(&included,800); fixture_string(&included,"executable.dat"); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,sizeof(contents)); fixture_u32(&included,1);
    fixture_u32(&included,sizeof(contents)); memcpy(included.data+included.size,contents,sizeof(contents)); included.size+=sizeof(contents);
    fixture_u32(&included,1); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,0); fixture_u32(&included,1);
    fixture_compressed(&decoded,included.data,(int)included.size); }
  fixture_u32(&decoded,800); /* help */
  { Fixture information=game_information_fixture();
    fixture_u32(&decoded,(unsigned)information.size);
    memcpy(decoded.data+decoded.size,information.data,information.size);
    decoded.size+=information.size; }
  fixture_u32(&decoded,500); fixture_u32(&decoded,1); /* library code */
  fixture_string(&decoded,"global.fixture_executable_started = 1;");
  fixture_u32(&decoded,700); fixture_u32(&decoded,1); fixture_u32(&decoded,0); /* room order */

  memset(executable,0,sizeof(*executable));
  executable->data[0]='M'; executable->data[1]='Z'; executable->size=16;
  fixture_u32(executable,20); /* embedded-data self pointer */
  fixture_u32(executable,GMLC_CLASSIC_MAGIC); fixture_u32(executable,800);
  fixture_u32(executable,0); fixture_u32(executable,800);
  { Fixture settings={{0},0};
    for(unsigned field=0;field<14;field++) fixture_u32(&settings,0);
    fixture_zero(&settings,9u*4u); /* shortcuts and process settings */
    fixture_u32(&settings,0); /* loading bar */
    fixture_u32(&settings,0); /* custom loading image */
    fixture_zero(&settings,3u*4u); /* loading image settings */
    fixture_zero(&settings,4u*4u); /* error settings */
    fixture_u32(&settings,0); fixture_u32(&settings,1); /* WebGL, creation order */
    fixture_compressed(executable,settings.data,(int)settings.size); }
  fixture_u32(executable,0); fixture_u32(executable,0); /* wrapper strings */
  /* The test program returns the authored resource stream unchanged. */
  if(executable->size+decoded.size>sizeof(executable->data)) return 0;
  memcpy(executable->data+executable->size,decoded.data,decoded.size);
  executable->size+=decoded.size;
  return 1;
}

int build_legacy_executable_fixture_source(Fixture *executable,const char *text){
  Fixture decoded={{0},0},envelope={{0},0};
  fixture_u32(&decoded,17); fixture_u32(&decoded,0x24681357); fixture_zero(&decoded,16);
  fixture_u32(&decoded,700); fixture_u32(&decoded,0); /* extensions */
  for(unsigned type=0;type<GMLC_CLASSIC_RESOURCE_TYPES;type++){
    fixture_u32(&decoded,type==GMLC_CLASSIC_FONT?540:type==GMLC_CLASSIC_ROOM?420:400);
    if(type==GMLC_CLASSIC_SPRITE){
      Fixture sprite={{0},0};
      const unsigned char bgra[]={0,192,0,0, 0,192,0,255};
      fixture_u32(&decoded,2); fixture_u32(&sprite,1);
      fixture_string(&sprite,"fixture_legacy_executable_sprite"); fixture_u32(&sprite,542);
      fixture_u32(&sprite,2); fixture_u32(&sprite,1);
      fixture_u32(&sprite,0); fixture_u32(&sprite,1); fixture_u32(&sprite,0); fixture_u32(&sprite,0);
      fixture_u32(&sprite,1); fixture_u32(&sprite,0); fixture_u32(&sprite,1);
      fixture_u32(&sprite,0); fixture_u32(&sprite,1); fixture_u32(&sprite,0); fixture_u32(&sprite,0);
      fixture_u32(&sprite,1); /* frames */
      fixture_u32(&sprite,540); fixture_u32(&sprite,1); fixture_u32(&sprite,2); fixture_u32(&sprite,1);
      fixture_compressed(&sprite,bgra,sizeof(bgra));
      memcpy(decoded.data+decoded.size,sprite.data,sprite.size); decoded.size+=sprite.size;
      /* GameMaker 5 writes the same thirteen fields in a different order: the bounding-box mode
       * follows the transparency flag, where 6/7 keep smoothing and preloading, and the precise
       * flag sits two places earlier. Read with the 6/7 order this sprite reports an automatic
       * box and precise collision — the opposite of both authored values. It carries no frame so
       * the memory-file indices the rest of this fixture asserts on stay put. */
      Fixture legacy5={{0},0};
      fixture_u32(&legacy5,1);
      fixture_string(&legacy5,"fixture_legacy_gm5_sprite"); fixture_u32(&legacy5,400);
      fixture_u32(&legacy5,2); fixture_u32(&legacy5,1);
      fixture_u32(&legacy5,0); fixture_u32(&legacy5,1); fixture_u32(&legacy5,0); fixture_u32(&legacy5,0);
      fixture_u32(&legacy5,1); fixture_u32(&legacy5,2); fixture_u32(&legacy5,0);
      fixture_u32(&legacy5,0); fixture_u32(&legacy5,1); fixture_u32(&legacy5,0); fixture_u32(&legacy5,0);
      fixture_u32(&legacy5,0); /* frames */
      memcpy(decoded.data+decoded.size,legacy5.data,legacy5.size); decoded.size+=legacy5.size;
    } else if(type==GMLC_CLASSIC_BACKGROUND){
      Fixture background={{0},0}; const unsigned char bgra[]={13,12,11,255,19,18,17,255};
      fixture_u32(&decoded,1); fixture_u32(&background,1);
      fixture_string(&background,"fixture_legacy_executable_background"); fixture_u32(&background,543);
      fixture_u32(&background,2); fixture_u32(&background,1); fixture_u32(&background,1);
      fixture_u32(&background,0); fixture_u32(&background,1); fixture_u32(&background,1);
      fixture_u32(&background,540); fixture_u32(&background,1);
      fixture_u32(&background,2); fixture_u32(&background,1);
      fixture_compressed(&background,bgra,sizeof(bgra));
      memcpy(decoded.data+decoded.size,background.data,background.size); decoded.size+=background.size;
    } else if(type==GMLC_CLASSIC_PATH){
      fixture_u32(&decoded,1); fixture_u32(&decoded,1);
      fixture_string(&decoded,"fixture_legacy_executable_path"); fixture_u32(&decoded,530);
      fixture_u32(&decoded,1); fixture_u32(&decoded,1); fixture_u32(&decoded,4);
      fixture_u32(&decoded,2);
      fixture_double(&decoded,1.5); fixture_double(&decoded,2.5); fixture_double(&decoded,100.0);
      fixture_double(&decoded,9.5); fixture_double(&decoded,8.5); fixture_double(&decoded,50.0);
    } else if(type==GMLC_CLASSIC_SCRIPT){
      fixture_u32(&decoded,1); fixture_u32(&decoded,1);
      fixture_string(&decoded,"fixture_legacy_executable_script"); fixture_u32(&decoded,500);
      Fixture source={{0},0};
      if(!text || strlen(text)>sizeof source.data) return 0;
      source.size=strlen(text);
      memcpy(source.data,text,source.size);
      fixture_compressed(&decoded,source.data,(int)source.size);
    } else if(type==GMLC_CLASSIC_FONT){
      fixture_u32(&decoded,1); fixture_u32(&decoded,1);
      fixture_string(&decoded,"fixture_legacy_executable_font"); fixture_u32(&decoded,540);
      fixture_string(&decoded,"fixture face");
      fixture_u32(&decoded,10); fixture_u32(&decoded,0); fixture_u32(&decoded,0);
      fixture_u32(&decoded,65); fixture_u32(&decoded,66);
      for(unsigned entry=0;entry<256u*6u;entry++){
        unsigned value=0;
        if(entry==65u*6u+2u || entry==65u*6u+3u) value=1;
        else if(entry==65u*6u+4u) value=3;
        else if(entry==66u*6u) value=1;
        else if(entry==66u*6u+2u || entry==66u*6u+3u) value=1;
        else if(entry==66u*6u+4u) value=4;
        else if(entry==66u*6u+5u) value=(unsigned)-1;
        fixture_u32(&decoded,value);
      }
      fixture_u32(&decoded,2); fixture_u32(&decoded,1);
      { const unsigned char alpha[]={23,211};
        fixture_compressed(&decoded,alpha,sizeof(alpha)); }
    } else if(type==GMLC_CLASSIC_ROOM){
      fixture_u32(&decoded,1); fixture_u32(&decoded,1);
      fixture_string(&decoded,"fixture_legacy_executable_room"); fixture_u32(&decoded,541);
      fixture_string(&decoded,"Fixture");
      fixture_u32(&decoded,320); fixture_u32(&decoded,240); fixture_u32(&decoded,60);
      fixture_u32(&decoded,0); fixture_u32(&decoded,0x112233); fixture_u32(&decoded,1);
      fixture_string(&decoded,"global.room_started = 1;");
      fixture_u32(&decoded,0); /* backgrounds */
      fixture_u32(&decoded,0); fixture_u32(&decoded,0); /* views */
      fixture_u32(&decoded,1); fixture_u32(&decoded,10); fixture_u32(&decoded,20);
      fixture_u32(&decoded,0); fixture_u32(&decoded,100001);
      fixture_string(&decoded,"global.instance_started = 1;");
      fixture_u32(&decoded,1); /* tiles */
      fixture_u32(&decoded,30); fixture_u32(&decoded,40); fixture_u32(&decoded,0);
      fixture_u32(&decoded,0); fixture_u32(&decoded,0); fixture_u32(&decoded,2);
      fixture_u32(&decoded,1); fixture_u32(&decoded,100); fixture_u32(&decoded,1000001);
    } else fixture_u32(&decoded,0);
  }
  fixture_u32(&decoded,100001); fixture_u32(&decoded,1000001);
  /* One included file whose data is compressed the way a GameMaker 6/7 executable stores it. The
   * record itself is not compressed; only the file's own bytes are. */
  fixture_u32(&decoded,620); fixture_u32(&decoded,1); /* includes */
  fixture_u32(&decoded,620);
  fixture_string(&decoded,"track1.ogg"); fixture_string(&decoded,"source/track1.ogg");
  fixture_u32(&decoded,1); fixture_u32(&decoded,8); fixture_u32(&decoded,1);
  fixture_compressed(&decoded,(const unsigned char *)"OggScass",8);
  fixture_u32(&decoded,2); fixture_string(&decoded,"");
  fixture_u32(&decoded,1); fixture_u32(&decoded,0); fixture_u32(&decoded,0);
  fixture_u32(&decoded,600); fixture_u32(&decoded,0xffffff); fixture_u32(&decoded,1);
  fixture_string(&decoded,"Game Information"); fixture_zero(&decoded,8u*4u); fixture_u32(&decoded,0);
  fixture_u32(&decoded,500); fixture_u32(&decoded,1);
  fixture_string(&decoded,"global.legacy_executable_started = 1;");
  fixture_u32(&decoded,700); fixture_u32(&decoded,1); fixture_u32(&decoded,0);

  envelope=decoded;

  memset(executable,0,sizeof(*executable)); executable->data[0]='M'; executable->data[1]='Z';
  executable->size=16;
  fixture_u32(executable,GMLC_CLASSIC_MAGIC); fixture_u32(executable,700);
  fixture_u32(executable,0); fixture_u32(executable,702);
  for(unsigned field=0;field<14;field++)
    fixture_u32(executable,field==1?1:field==4?150:0);
  fixture_compressed(executable,envelope.data,(int)envelope.size);
  return 1;
}

int build_legacy_executable_fixture(Fixture *executable){
  return build_legacy_executable_fixture_source(executable,"return 42;");
}

int build_gm6_executable_fixture(Fixture *executable){
  Fixture game={{0},0},plain={{0},0},envelope={{0},0};
  fixture_u32(&game,0); fixture_u32(&game,0x31415926); fixture_zero(&game,16);
  fixture_u32(&game,GMLC_CLASSIC_GM6);
  for(unsigned field=0;field<20;field++)
    fixture_u32(&game,field==1?1u:field==4?150u:0u);
  fixture_u32(&game,0); /* no paired loading-bar images */
  fixture_u32(&game,0); /* no custom loading image */
  fixture_zero(&game,7u*4u); fixture_u32(&game,1); /* error settings and constants */
  fixture_string(&game,"fixture_gm6_constant"); fixture_string(&game,"6*7");
  for(unsigned type=0;type<GMLC_CLASSIC_RESOURCE_TYPES;type++){
    fixture_u32(&game,400); fixture_u32(&game,0);
  }
  fixture_u32(&game,100001); fixture_u32(&game,1000001);
  fixture_u32(&game,600); fixture_zero(&game,2u*4u);
  fixture_string(&game,"Game Maker 6 fixture"); fixture_zero(&game,8u*4u);
  fixture_u32(&game,0); /* empty game-information blob */
  fixture_u32(&game,500); fixture_u32(&game,1);
  fixture_string(&game,"global.gm6_executable_started = 1;");
  fixture_u32(&game,600); fixture_u32(&game,0); /* room order */

  fixture_u32(&plain,33); fixture_u32(&plain,33);
  fixture_u32(&plain,GMLC_CLASSIC_MAGIC); fixture_u32(&plain,GMLC_CLASSIC_GM6);
  memcpy(plain.data+plain.size,game.data,game.size); plain.size+=game.size;

  envelope=plain;

  memset(executable,0,sizeof(*executable)); executable->data[0]='M'; executable->data[1]='Z';
  executable->size=64;
  fixture_u32(executable,GMLC_CLASSIC_MAGIC); fixture_u32(executable,GMLC_CLASSIC_GM6);
  fixture_zero(executable,3u*4u);
  unsigned char renderer[68]={0};
  renderer[0]='M'; renderer[1]='Z'; renderer[60]=64; renderer[64]='P'; renderer[65]='E';
  fixture_string(executable,"renderer.bin");
  fixture_compressed(executable,renderer,sizeof(renderer));
  fixture_string(executable,"before.dat");
  fixture_compressed(executable,(const unsigned char *)"before",6);
  fixture_string(executable,"READY");
  fixture_compressed(executable,envelope.data,(int)envelope.size);
  fixture_string(executable,"after.dat");
  fixture_compressed(executable,(const unsigned char *)"after",5);
  return 1;
}

void fixture_legacy_room(Fixture *f, const char *name){
  fixture_u32(f,1); fixture_string(f,name); fixture_u32(f,541);
  fixture_string(f,"");
  fixture_u32(f,320); fixture_u32(f,240); fixture_u32(f,16); fixture_u32(f,16);
  fixture_u32(f,0); fixture_u32(f,30); fixture_u32(f,0); fixture_u32(f,0); fixture_u32(f,1);
  fixture_string(f,"");
  fixture_u32(f,0); /* backgrounds */
  fixture_u32(f,0); fixture_u32(f,0); /* views enabled/count */
  fixture_u32(f,0); /* instances */
  fixture_u32(f,0); /* tiles */
  for(unsigned i=0;i<14;i++) fixture_u32(f,0);
}

static void fixture_object_payload(Fixture *f, const FixtureObject *object);
static void fixture_room_payload(Fixture *f, const FixtureProgram *program);

static void fixture_legacy_resource(Fixture *f,const char *name,unsigned version,
                                    const Fixture *payload){
  fixture_u32(f,1);
  fixture_string(f,name);
  fixture_u32(f,version);
  if(f->size+payload->size>sizeof(f->data)) abort();
  memcpy(f->data+f->size,payload->data,payload->size);
  f->size+=payload->size;
}

static Fixture legacy_fixture_build(unsigned container_version, int sparse_rooms, const char *gml,
                                    const FixtureProgram *program){
  Fixture f = {{0}, 0};
  int gm53 = container_version == 530;
  int gm7 = container_version == 701 || container_version == 702;
  fixture_u32(&f, GMLC_CLASSIC_MAGIC); fixture_u32(&f, container_version);
  if(gm53) fixture_u32(&f,0); /* reserved */
  fixture_u32(&f, 42); fixture_zero(&f, 16);
  fixture_u32(&f, gm53 ? 530 : gm7 ? 702 : 600);
  for(unsigned i = 0; i < (gm53 || gm7 ? 22u : 20u); ++i)
    fixture_u32(&f,gm53 && i==3?100:0);
  fixture_u32(&f, 0); /* loading bar */
  fixture_u32(&f, 0); /* custom loading image */
  fixture_u32(&f, 0); fixture_u32(&f, 255); fixture_u32(&f, 1);
  fixture_u32(&f, 0); /* icon blob */
  for(unsigned i = 0; i < 4; ++i) fixture_u32(&f, 0);
  fixture_string(&f, "");
  if(gm7) fixture_string(&f, "100"); else fixture_u32(&f, 100);
  fixture_zero(&f, 8);
  fixture_string(&f, "");
  fixture_u32(&f, 0); /* constants */
  if(gm7){
    for(unsigned i = 0; i < 4; ++i) fixture_u32(&f, i == 0 ? 1 : 0);
    for(unsigned i = 0; i < 4; ++i) fixture_string(&f, "");
  } else if(!gm53) {
    fixture_u32(&f, 0); /* includes */
    fixture_u32(&f, 0); fixture_u32(&f, 0); fixture_u32(&f, 0);
  }
  const unsigned section_versions[GMLC_CLASSIC_RESOURCE_TYPES] =
    {400, 400, 400, 420, 400, 540, 500, 400, 420};
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, gm53 && type==GMLC_CLASSIC_FONT?440:section_versions[type]);
    if(program && type==GMLC_CLASSIC_OBJECT){
      fixture_u32(&f,(unsigned)program->object_count);
      for(int i=0;i<program->object_count;i++){
        Fixture payload={{0},0};
        fixture_object_payload(&payload,&program->objects[i]);
        fixture_legacy_resource(&f,program->objects[i].name,430,&payload);
      }
    } else if(program && type==GMLC_CLASSIC_ROOM){
      Fixture payload={{0},0};
      fixture_room_payload(&payload,program);
      fixture_u32(&f,1);
      fixture_legacy_resource(&f,"fixture_room",541,&payload);
    } else if(sparse_rooms && type==GMLC_CLASSIC_ROOM){
      fixture_u32(&f,8);
      fixture_u32(&f,0); fixture_u32(&f,0);
      fixture_legacy_room(&f,"resource_room_two");
      fixture_u32(&f,0); fixture_u32(&f,0); fixture_u32(&f,0);
      fixture_legacy_room(&f,"resource_room_six");
      fixture_u32(&f,0);
    } else fixture_u32(&f, 0);
  }
  fixture_u32(&f, 100000); fixture_u32(&f, 1000000);
  /* The tail is optional and the plain fixture ends here. Either a sparse room order or an
   * embedded source needs it, and both need all of it: the sections are positional, so library
   * code cannot be reached without writing the game information that precedes it. */
  if(sparse_rooms || gml || program){
    if(gm7){
      fixture_u32(&f,620); fixture_u32(&f,0); /* included files */
      fixture_u32(&f,700); fixture_u32(&f,0); /* extensions */
    }
    fixture_u32(&f,600); fixture_u32(&f,0x00ffffff); fixture_u32(&f,1);
    fixture_string(&f,"Game Information");
    fixture_u32(&f,(unsigned)-1); fixture_u32(&f,(unsigned)-1);
    fixture_u32(&f,600); fixture_u32(&f,400);
    fixture_u32(&f,1); fixture_u32(&f,1); fixture_u32(&f,0); fixture_u32(&f,1);
    fixture_string(&f,"");
    fixture_u32(&f,500); fixture_u32(&f, gml || (program && program->startup) ? 1 : 0); /* library code */
    if(gml) fixture_string(&f,gml);
    else if(program && program->startup) fixture_string(&f,program->startup);
    if(sparse_rooms){
      fixture_u32(&f,700); fixture_u32(&f,2); /* explicit room order */
      fixture_u32(&f,6); fixture_u32(&f,2);
    } else if(program){
      fixture_u32(&f,700); fixture_u32(&f,1);
      fixture_u32(&f,0);
    } else {
      fixture_u32(&f,700); fixture_u32(&f,0); /* no rooms to order */
    }
  }
  return f;
}

Fixture legacy_fixture_variant(unsigned container_version, int sparse_rooms){
  return legacy_fixture_build(container_version,sparse_rooms,NULL,NULL);
}

Fixture legacy_fixture_source(unsigned container_version, const char *gml){
  return legacy_fixture_build(container_version,0,gml,NULL);
}

Fixture legacy_fixture(unsigned container_version){
  return legacy_fixture_variant(container_version,0);
}

int build_gm53_executable_fixture(Fixture *executable){
  if(!executable) return 0;
  Fixture project=legacy_fixture(530);
  memset(executable,0,sizeof(*executable));
  fixture_zero(executable,64); executable->data[0]='M'; executable->data[1]='Z';
  if(executable->size+project.size>sizeof(executable->data)) return 0;
  memcpy(executable->data+executable->size,project.data,project.size);
  executable->size+=project.size;
  return 1;
}

/* One Execute Code action. The source is stored in the first argument when the action's own code
 * field is empty, so nothing is compiled here either. */
static void fixture_code_action_list(Fixture *f, const char *source){
  fixture_u32(f,400); /* action-list version */
  fixture_u32(f,1);   /* one action */
  fixture_u32(f,440); /* action version */
  fixture_u32(f,1);   /* library */
  fixture_u32(f,603); /* action id */
  fixture_u32(f,7);   /* kind: execute code */
  fixture_u32(f,0);   /* may be relative */
  fixture_u32(f,0);   /* is a question */
  fixture_u32(f,0);   /* applies to a target */
  fixture_u32(f,2);   /* type: code */
  fixture_string(f,""); /* function name */
  fixture_string(f,""); /* code: empty, so the source is the argument below */
  fixture_u32(f,1);   /* used arguments */
  fixture_u32(f,1);   /* argument kinds */
  fixture_u32(f,1);
  fixture_u32(f,(unsigned)-1); /* target */
  fixture_u32(f,0);   /* relative */
  fixture_u32(f,1);   /* arguments */
  fixture_string(f,source?source:"");
  fixture_u32(f,0);   /* negated */
}

static void fixture_authored_action_list(Fixture *f, const FixtureEvent *event){
  fixture_u32(f,400);
  fixture_u32(f,(unsigned)event->action_count);
  for(int i=0;i<event->action_count;i++){
    const FixtureAction *a=&event->actions[i];
    int code=a->kind==7, question=a->kind==0;
    fixture_u32(f,440); fixture_u32(f,1); fixture_u32(f,code?603:0);
    fixture_u32(f,a->kind); fixture_u32(f,0); fixture_u32(f,question);
    fixture_u32(f,1); fixture_u32(f,code || question?2:0);
    fixture_string(f,""); fixture_string(f,question?a->source:"");
    fixture_u32(f,code); fixture_u32(f,code);
    if(code) fixture_u32(f,1);
    fixture_u32(f,(unsigned)a->target); fixture_u32(f,0);
    fixture_u32(f,code);
    if(code) fixture_string(f,a->source);
    fixture_u32(f,a->negate);
  }
}

static void fixture_object_payload(Fixture *f, const FixtureObject *object){
  int last_event_type=0;
  for(int i=0;i<object->event_count;i++)
    if(object->events[i].event_type>last_event_type) last_event_type=object->events[i].event_type;
  fixture_u32(f,(unsigned)object->sprite);
  fixture_u32(f,(unsigned)(object->solid!=0));
  fixture_u32(f,1); /* visible */
  fixture_u32(f,0); /* depth */
  fixture_u32(f,(unsigned)(object->persistent!=0));
  fixture_u32(f,(unsigned)-100); /* parent: none */
  fixture_u32(f,(unsigned)-1);   /* mask: the sprite's own */
  fixture_u32(f,(unsigned)last_event_type);
  /* Every event type up to the highest used is written, each terminated by the sentinel, because
   * the reader walks the types in order rather than seeking. */
  for(int type=0;type<=last_event_type;type++){
    for(int i=0;i<object->event_count;i++){
      if(object->events[i].event_type!=type) continue;
      fixture_u32(f,(unsigned)object->events[i].event_number);
      if(object->events[i].actions) fixture_authored_action_list(f,&object->events[i]);
      else fixture_code_action_list(f,object->events[i].source);
    }
    fixture_u32(f,(unsigned)-1); /* end of this event type */
  }
}

/* One opaque square. Collision needs a mask and a mask comes from a sprite, so the smallest sprite
 * that lets a collision event fire is a filled rectangle with a manual bounding box. */
/* A second fully transparent frame makes authored and packed extents differ so a synthetic raster rule can exercise the margin. */
static void fixture_sprite_payload(Fixture *f, int size, int blank_frame){
  fixture_u32(f,0); fixture_u32(f,0); /* origin */
  fixture_u32(f,blank_frame?2u:1u);   /* frames */
  fixture_u32(f,800);                 /* frame version */
  fixture_u32(f,(unsigned)size); fixture_u32(f,(unsigned)size);
  unsigned bytes=(unsigned)size*(unsigned)size*4u;
  if(f->size+4+bytes>sizeof(f->data)) abort();
  fixture_u32(f,bytes);
  for(unsigned i=0;i<bytes;i++) f->data[f->size++]=255; /* opaque white, BGRA */
  if(blank_frame){
    fixture_u32(f,800);
    fixture_u32(f,(unsigned)size); fixture_u32(f,(unsigned)size);
    if(f->size+4+bytes>sizeof(f->data)) abort();
    fixture_u32(f,bytes);
    for(unsigned i=0;i<bytes;i++) f->data[f->size++]=0;   /* no colour and no coverage */
  }
  fixture_u32(f,1);                   /* collision kind: bounding box */
  fixture_u32(f,0);                   /* tolerance */
  fixture_u32(f,0);                   /* one mask for every frame */
  fixture_u32(f,2);                   /* bounding box: manual */
  fixture_u32(f,0);                          /* left */
  fixture_u32(f,(unsigned)(size-1));         /* right */
  fixture_u32(f,(unsigned)(size-1));         /* bottom */
  fixture_u32(f,0);                          /* top */
}

static void fixture_background_payload(Fixture *f,int size){
  fixture_u32(f,0); /* ordinary background, not an exported tileset sprite */
  fixture_u32(f,(unsigned)size); fixture_u32(f,(unsigned)size); /* tile dimensions */
  fixture_u32(f,0); fixture_u32(f,0); /* borders */
  fixture_u32(f,0); fixture_u32(f,0); /* separations */
  fixture_u32(f,800); /* image version */
  fixture_u32(f,(unsigned)size); fixture_u32(f,(unsigned)size);
  unsigned bytes=(unsigned)size*(unsigned)size*4u;
  if(f->size+4+bytes>sizeof(f->data)) abort();
  fixture_u32(f,bytes);
  for(unsigned i=0;i<bytes;i++) f->data[f->size++]=255;
}

static void fixture_room_payload(Fixture *f, const FixtureProgram *program){
  fixture_string(f,program->room_caption?program->room_caption:"");
  fixture_u32(f,(unsigned)program->room_width);
  fixture_u32(f,(unsigned)program->room_height);
  fixture_u32(f,16); fixture_u32(f,16); /* snap */
  fixture_u32(f,0);  /* isometric */
  fixture_u32(f,30); /* speed */
  fixture_u32(f,0);  /* persistent */
  fixture_u32(f,0);  /* background colour */
  fixture_u32(f,1);  /* draw the background colour */
  fixture_string(f,""); /* creation code */
  fixture_u32(f,0); /* backgrounds */
  fixture_u32(f,0); /* views enabled */
  fixture_u32(f,0); /* views */
  fixture_u32(f,(unsigned)program->instance_count);
  for(int i=0;i<program->instance_count;i++){
    fixture_u32(f,(unsigned)program->instances[i].x);
    fixture_u32(f,(unsigned)program->instances[i].y);
    fixture_u32(f,(unsigned)program->instances[i].object);
    fixture_u32(f,(unsigned)(100+i)); /* instance id, below the container's declared last id */
    fixture_string(f,"");             /* creation code */
    fixture_u32(f,0);                 /* locked */
  }
  fixture_u32(f,0); /* tiles */
  for(unsigned i=0;i<14;i++) fixture_u32(f,0); /* editor state */
}

/* A manifest resource is one zlib block holding its own existence flag, name, timestamp and
 * version before the payload. */
static void fixture_manifest_resource(Fixture *f, const char *name, unsigned version,
                                      const Fixture *payload){
  Fixture slot={{0},0};
  fixture_u32(&slot,1);
  fixture_string(&slot,name);
  fixture_zero(&slot,8); /* timestamp */
  fixture_u32(&slot,version);
  if(slot.size+payload->size>sizeof(slot.data)) abort();
  memcpy(slot.data+slot.size,payload->data,payload->size);
  slot.size+=payload->size;
  fixture_compressed(f,slot.data,(int)slot.size);
}

static void fixture_manifest_absent(Fixture *f){
  const unsigned char absent[4]={0,0,0,0};
  fixture_compressed(f,absent,sizeof(absent));
}

static Fixture manifest_fixture_program(unsigned container_version, const FixtureProgram *program){
  Fixture f = {{0}, 0};
  fixture_u32(&f, GMLC_CLASSIC_MAGIC); fixture_u32(&f, container_version);
  fixture_u32(&f, 9); fixture_zero(&f, 16);
  fixture_u32(&f, 800); fixture_u32(&f, 0); /* settings */
  fixture_u32(&f, 800); fixture_u32(&f, 0); /* triggers */
  fixture_zero(&f, 8);
  fixture_u32(&f, 800); fixture_u32(&f, 0); /* constants */
  fixture_zero(&f, 8);
  int sprites = program->sprite_size>0 ? 1 : 0;
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, 800);
    if(type == GMLC_CLASSIC_SPRITE && sprites){
      fixture_u32(&f, 1);
      Fixture payload={{0},0};
      fixture_sprite_payload(&payload,program->sprite_size,program->sprite_blank_frame);
      fixture_manifest_resource(&f,"fixture_square",800,&payload);
    } else if(type == GMLC_CLASSIC_BACKGROUND && program->background_size>0){
      fixture_u32(&f,program->second_background_size>0?2:1);
      Fixture payload={{0},0};
      fixture_background_payload(&payload,program->background_size);
      fixture_manifest_resource(&f,"fixture_background",800,&payload);
      if(program->second_background_size>0){
        payload.size=0;
        fixture_background_payload(&payload,program->second_background_size);
        fixture_manifest_resource(&f,"fixture_background_second",800,&payload);
      }
    } else if(type == GMLC_CLASSIC_TIMELINE && program->timeline_count){
      fixture_u32(&f,(unsigned)program->timeline_count);
      for(int i=0;i<program->timeline_count;i++){
        const FixtureTimeline *timeline=&program->timelines[i];
        Fixture payload={{0},0};
        fixture_u32(&payload,(unsigned)timeline->moment_count);
        for(int m=0;m<timeline->moment_count;m++){
          fixture_u32(&payload,(unsigned)timeline->moments[m].step);
          fixture_code_action_list(&payload,timeline->moments[m].source);
        }
        fixture_manifest_resource(&f,timeline->name,500,&payload);
      }
    } else if(type == GMLC_CLASSIC_OBJECT && program->object_count){
      fixture_u32(&f,(unsigned)program->object_count);
      for(int i=0;i<program->object_count;i++){
        Fixture payload={{0},0};
        fixture_object_payload(&payload,&program->objects[i]);
        fixture_manifest_resource(&f,program->objects[i].name,800,&payload);
      }
    } else if(type == GMLC_CLASSIC_ROOM){
      fixture_u32(&f,1);
      Fixture payload={{0},0};
      fixture_room_payload(&payload,program);
      fixture_manifest_resource(&f,"fixture_room",541,&payload);
    } else {
      fixture_u32(&f, 1);
      fixture_manifest_absent(&f);
    }
  }
  fixture_u32(&f, 100001); fixture_u32(&f, 1000001); /* last instance and tile ids */
  fixture_u32(&f, 800); fixture_u32(&f, 0); /* included files */
  fixture_u32(&f, 700); fixture_u32(&f, 0); /* extensions */
  fixture_u32(&f, 800);
  { Fixture information=game_information_fixture();
    fixture_compressed(&f,information.data,(int)information.size); }
  fixture_u32(&f, 500); fixture_u32(&f, program->startup ? 1 : 0);
  if(program->startup) fixture_string(&f,program->startup);
  fixture_u32(&f, 700); fixture_u32(&f, 1); /* room order */
  fixture_u32(&f, 0);
  return f;
}

int build_project_fixture_program(unsigned version, const FixtureProgram *program, Fixture *out){
  if(!program || program->room_width<=0 || program->room_height<=0) return 0;
  if(program->timeline_count<0 || program->timeline_count>8 ||
     (program->timeline_count && !program->timelines)) return 0;
  for(int i=0;i<program->timeline_count;i++){
    const FixtureTimeline *timeline=&program->timelines[i];
    if(!timeline->name || timeline->moment_count<0 || timeline->moment_count>64 ||
       (timeline->moment_count && !timeline->moments)) return 0;
    for(int m=0;m<timeline->moment_count;m++)
      if(timeline->moments[m].step<0 || !timeline->moments[m].source) return 0;
  }
  /* Each uncompressed image must fit the bounded fixture payload. */
  if(program->background_size<0 || program->background_size>63 ||
     program->second_background_size<0 || program->second_background_size>63 ||
     (program->second_background_size>0 && !program->background_size)) return 0;
  /* A first-party GM5 rule can use the legacy inline object and room records. The compact legacy
   * sprite record is deliberately not synthesized here; a program requiring one must continue to
   * use a manifest generation until that distinct record is represented explicitly. */
  if(version==530 && program->sprite_size==0 && program->background_size==0 &&
     program->timeline_count==0){
    *out=legacy_fixture_build(version,0,NULL,program);
    return 1;
  }
  if(version!=800 && version!=810) return 0;
  *out=manifest_fixture_program(version,program);
  return 1;
}

int build_project_fixture(unsigned version, Fixture *out){
  return build_project_fixture_source(version,NULL,out);
}

int build_project_fixture_source(unsigned version, const char *gml, Fixture *out){
  if(version==530 || version==600){ *out=legacy_fixture_source(version,gml); return 1; }
  if(version==701 || version==702){
    Fixture plain=legacy_fixture_source(version,gml);
    size_t encoded_size=0;
    unsigned char *encoded=fixture_project_image(plain.data,plain.size,&encoded_size);
    if(!encoded || encoded_size>sizeof(out->data)){ free(encoded); return 0; }
    memset(out,0,sizeof(*out));
    memcpy(out->data,encoded,encoded_size);
    out->size=encoded_size;
    free(encoded);
    return 1;
  }
  if(version==800 || version==810){ *out=manifest_fixture_source(version,gml); return 1; }
  return 0;
}
