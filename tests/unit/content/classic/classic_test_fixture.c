/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"

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
  int compressed_size = 0;
  unsigned char *compressed = stbi_zlib_compress((unsigned char*)raw, raw_size, &compressed_size, 8);
  if(!compressed || compressed_size <= 0 || f->size + 4 + (size_t)compressed_size > sizeof(f->data)) abort();
  fixture_u32(f, (unsigned)compressed_size);
  memcpy(f->data + f->size, compressed, (size_t)compressed_size);
  f->size += (size_t)compressed_size;
  STBIW_FREE(compressed);
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
  fixture_u32(&f, 500); fixture_u32(&f, 1); /* library code */
  fixture_string(&f,"global.fixture_started = 1;");
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
  fixture_u32(&decoded,0); fixture_u32(&decoded,1); /* functions, constants */
  fixture_u32(&decoded,700); fixture_string(&decoded,"fixture_extension_constant"); fixture_string(&decoded,"7*6");
  fixture_u32(&decoded,4); fixture_u32(&decoded,0); 
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
  fixture_u32(executable,0); /* settings */
  fixture_u32(executable,0); fixture_u32(executable,0); /* wrapper strings */
  fixture_u32(executable,0); fixture_u32(executable,0); /* junk counts */
  unsigned char table[256];
  for(unsigned i=0;i<256;i++){
    table[i]=(unsigned char)(i*73u+41u);
    executable->data[executable->size++]=table[i];
  }
  fixture_u32(executable,(unsigned)decoded.size);
  unsigned char encoded_data[sizeof(decoded.data)];
  memcpy(encoded_data,decoded.data,decoded.size);
  for(size_t i=0;i<decoded.size;i++){
    size_t offset=table[i&255u];
    size_t other=i>offset?i-offset:0;
    unsigned char swap=encoded_data[i]; encoded_data[i]=encoded_data[other]; encoded_data[other]=swap;
  }
  unsigned char previous=encoded_data[0];
  executable->data[executable->size++]=previous;
  for(size_t i=1;i<decoded.size;i++){
    previous=table[(unsigned char)(encoded_data[i]+previous+(unsigned char)i)];
    executable->data[executable->size++]=previous;
  }
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

Fixture legacy_fixture_variant(unsigned container_version, int sparse_rooms){
  Fixture f = {{0}, 0};
  int gm7 = container_version == 701 || container_version == 702;
  fixture_u32(&f, GMLC_CLASSIC_MAGIC); fixture_u32(&f, container_version);
  fixture_u32(&f, 42); fixture_zero(&f, 16);
  fixture_u32(&f, gm7 ? 702 : 600);
  for(unsigned i = 0; i < (gm7 ? 22u : 20u); ++i) fixture_u32(&f, 0);
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
  } else {
    fixture_u32(&f, 0); /* includes */
    fixture_u32(&f, 0); fixture_u32(&f, 0); fixture_u32(&f, 0);
  }
  const unsigned section_versions[GMLC_CLASSIC_RESOURCE_TYPES] =
    {400, 400, 400, 420, 400, 540, 500, 400, 420};
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, section_versions[type]);
    if(sparse_rooms && type==GMLC_CLASSIC_ROOM){
      fixture_u32(&f,8);
      fixture_u32(&f,0); fixture_u32(&f,0);
      fixture_legacy_room(&f,"resource_room_two");
      fixture_u32(&f,0); fixture_u32(&f,0); fixture_u32(&f,0);
      fixture_legacy_room(&f,"resource_room_six");
      fixture_u32(&f,0);
    } else fixture_u32(&f, 0);
  }
  fixture_u32(&f, 100000); fixture_u32(&f, 1000000);
  if(sparse_rooms){
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
    fixture_u32(&f,500); fixture_u32(&f,0); /* library code */
    fixture_u32(&f,700); fixture_u32(&f,2); /* explicit room order */
    fixture_u32(&f,6); fixture_u32(&f,2);
  }
  return f;
}

Fixture legacy_fixture(unsigned container_version){
  return legacy_fixture_variant(container_version,0);
}

int build_project_fixture(unsigned version, Fixture *out){
  if(version==600){ *out=legacy_fixture(version); return 1; }
  if(version==701 || version==702){
    Fixture plain=legacy_fixture(version);
    size_t encoded_size=0;
    unsigned char *encoded=encode_gm7(plain.data,plain.size,&encoded_size);
    if(!encoded || encoded_size>sizeof(out->data)){ free(encoded); return 0; }
    memset(out,0,sizeof(*out));
    memcpy(out->data,encoded,encoded_size);
    out->size=encoded_size;
    free(encoded);
    return 1;
  }
  if(version==800 || version==810){ *out=manifest_fixture(version); return 1; }
  return 0;
}
