/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic.h"
#include "gmlc_classic_import.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static void put_u32le(unsigned char *p, unsigned value){
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

static int expect_header(unsigned version){
  unsigned char data[28] = {0};
  put_u32le(data, GMLC_CLASSIC_MAGIC);
  put_u32le(data + 4, version);
  put_u32le(data + 8, 0x12345678u);
  for(int i = 0; i < 16; ++i) data[12 + i] = (unsigned char)(0xa0 + i);
  GmlcClassicHeader h;
  char err[128];
  if(!gmlc_classic_probe(data, sizeof(data), &h, err, sizeof(err))){
    fprintf(stderr, "probe %u failed: %s\n", version, err);
    return 0;
  }
  int encrypted = version == 701 || version == 702;
  if((unsigned)h.version != version ||
     (!encrypted && (h.game_id != 0x12345678u || memcmp(h.guid, data + 12, 16))) ||
     (encrypted && h.game_id != 0)){
    fprintf(stderr, "probe %u returned incorrect fields\n", version);
    return 0;
  }
  return 1;
}

static int expect_rejected(unsigned magic, unsigned version, size_t size){
  unsigned char data[28] = {0};
  put_u32le(data, magic);
  put_u32le(data + 4, version);
  GmlcClassicHeader h;
  char err[128];
  return !gmlc_classic_probe(data, size, &h, err, sizeof(err)) && err[0];
}



static int expect_gm7_decode(void){
  unsigned char plain[64] = {0};
  put_u32le(plain, GMLC_CLASSIC_MAGIC);
  put_u32le(plain + 4, 701);
  for(size_t i = 8; i < sizeof(plain); ++i) plain[i] = (unsigned char)(i * 3 + 1);
  size_t encoded_size = 0;
  unsigned char *encoded = encode_gm7(plain, sizeof(plain), &encoded_size);
  if(!encoded) return 0;
  uint8_t *decoded = NULL;
  size_t decoded_size = 0;
  char err[128];
  int ok = (0 /* This operation is unavailable. */) &&
           decoded_size == sizeof(plain) && !memcmp(decoded, plain, sizeof(plain));
  if(!ok) fprintf(stderr, "GM7 decode fixture failed: %s\n", err);
  free(decoded);
  free(encoded);
  return ok;
}

typedef struct {
  unsigned char data[1024];
  size_t size;
} Fixture;

static void fixture_u32(Fixture *f, unsigned value){
  put_u32le(f->data + f->size, value);
  f->size += 4;
}

static void fixture_zero(Fixture *f, size_t count){
  memset(f->data + f->size, 0, count);
  f->size += count;
}

static void fixture_string(Fixture *f, const char *text){
  size_t length = text ? strlen(text) : 0;
  fixture_u32(f, (unsigned)length);
  if(length){
    memcpy(f->data + f->size, text, length);
    f->size += length;
  }
}

static void fixture_double(Fixture *f, double value){
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  for(unsigned i = 0; i < 8; ++i) f->data[f->size++] = (unsigned char)(bits >> (i * 8));
}

static void fixture_compressed(Fixture *f, const unsigned char *raw, int raw_size){
  int compressed_size = 0;
  unsigned char *compressed = stbi_zlib_compress((unsigned char*)raw, raw_size, &compressed_size, 8);
  if(!compressed || compressed_size <= 0 || f->size + 4 + (size_t)compressed_size > sizeof(f->data)) abort();
  fixture_u32(f, (unsigned)compressed_size);
  memcpy(f->data + f->size, compressed, (size_t)compressed_size);
  f->size += (size_t)compressed_size;
  STBIW_FREE(compressed);
}

static void fixture_legacy_bmp_image(Fixture *f){
  unsigned char bmp[62] = {0};
  bmp[0]='B'; bmp[1]='M';
  put_u32le(bmp+2,sizeof(bmp)); put_u32le(bmp+10,54); put_u32le(bmp+14,40);
  put_u32le(bmp+18,2); put_u32le(bmp+22,1);
  bmp[26]=1; bmp[28]=24; put_u32le(bmp+34,8);
  /* Bottom-up BGR row: magenta transparency key, then RGB(1,2,3), plus padding. */
  bmp[54]=255; bmp[55]=0; bmp[56]=255;
  bmp[57]=3; bmp[58]=2; bmp[59]=1;
  fixture_u32(f,10);
  fixture_compressed(f,bmp,sizeof(bmp));
}

static Fixture inventory_fixture(unsigned container_version){
  Fixture f = {{0}, 0};
  fixture_u32(&f, GMLC_CLASSIC_MAGIC);
  fixture_u32(&f, container_version);
  fixture_u32(&f, 7);
  fixture_zero(&f, 16);
  fixture_u32(&f, 800); /* settings version */
  unsigned char settings[14 * 4] = {0};
  put_u32le(settings + 4, 1);      /* interpolation */
  put_u32le(settings + 16, 200);   /* fixed two-times scaling */
  fixture_compressed(&f, settings, sizeof(settings));
  fixture_u32(&f, 800); fixture_u32(&f, 0); fixture_zero(&f, 8); /* triggers */
  fixture_u32(&f, 800); fixture_u32(&f, 0); fixture_zero(&f, 8); /* constants */
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, 800);
    fixture_u32(&f, type);
    for(unsigned slot = 0; slot < type; ++slot) fixture_u32(&f, 0);
  }
  fixture_u32(&f, 100123);
  fixture_u32(&f, 1000456);
  return f;
}

static int expect_inventory(unsigned version){
  Fixture f = inventory_fixture(version);
  GmlcClassicInventory in;
  char err[128];
  if(!gmlc_classic_inventory(f.data, f.size, &in, err, sizeof(err))){
    fprintf(stderr, "inventory %u failed: %s\n", version, err);
    return 0;
  }
  if(in.payload_end != f.size || in.last_instance_id != 100123 || in.last_tile_id != 1000456 ||
     in.settings.interpolate != 1 || in.settings.scaling != 200)
    return 0;
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type)
    if(in.resource_slots[type] != type) return 0;
  return 1;
}

static Fixture manifest_fixture(unsigned container_version){
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
  fixture_u32(&f, 800); fixture_u32(&f, 0); /* game information */
  fixture_u32(&f, 500); fixture_u32(&f, 1); /* library code */
  fixture_string(&f,"global.fixture_started = 1;");
  fixture_u32(&f, 700); fixture_u32(&f, 0); /* executable rooms */
  return f;
}

static int expect_manifest(void){
  Fixture f = manifest_fixture(800);
  GmlcClassicManifest manifest;
  char err[128];
  if(!gmlc_classic_manifest(f.data, f.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "manifest failed: %s\n", err);
    return 0;
  }
  int ok = manifest.existing[GMLC_CLASSIC_SCRIPT] == 1 &&
           manifest.slots[GMLC_CLASSIC_SCRIPT][0].exists &&
           manifest.slots[GMLC_CLASSIC_SCRIPT][0].version == 800 &&
           !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].name, "resource_script") &&
           !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].source, "return 7;") &&
           manifest.trigger_def_count==1 && manifest.trigger_defs[0].exists &&
           !strcmp(manifest.trigger_defs[0].condition,"global.ready") &&
           !strcmp(manifest.trigger_defs[0].constant_name,"fixture_trigger_constant") &&
           manifest.trigger_defs[0].moment==1 &&
           manifest.constant_def_count==2 &&
           !strcmp(manifest.constant_defs[0].name,"fixture_number") &&
           !strcmp(manifest.constant_defs[0].value,"6*7") &&
           manifest.included_file_count==1 && manifest.included_files[0].data_size==4 &&
           !strcmp(manifest.included_files[0].file_name,"fixture.dat") &&
           manifest.included_files[0].export_mode==2 &&
           manifest.extension_count==1 &&
           !strcmp(manifest.extension_names[0],"fixture_extension") &&
           manifest.library_creation_code_count==1 &&
           !strcmp(manifest.library_creation_code[0],"global.fixture_started = 1;");
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type)
    if(type != GMLC_CLASSIC_SCRIPT && manifest.existing[type]) ok = 0;
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_manifest_810(void){
  Fixture f = manifest_fixture(810);
  GmlcClassicManifest manifest;
  char err[128];
  if(!gmlc_classic_manifest(f.data, f.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "manifest 810 failed: %s\n", err);
    return 0;
  }
  int ok = manifest.inventory.header.version == GMLC_CLASSIC_GM81 &&
           manifest.existing[GMLC_CLASSIC_SCRIPT] == 1;
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int build_executable_fixture(Fixture *executable){
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
    } else fixture_u32(&decoded,0);
  }
  fixture_u32(&decoded,100000); fixture_u32(&decoded,1000000);
  fixture_u32(&decoded,800); fixture_u32(&decoded,1); /* includes */
  { Fixture included={{0},0}; const unsigned char contents[]={10,11};
    fixture_u32(&included,800); fixture_string(&included,"executable.dat"); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,sizeof(contents)); fixture_u32(&included,1);
    fixture_u32(&included,sizeof(contents)); memcpy(included.data+included.size,contents,sizeof(contents)); included.size+=sizeof(contents);
    fixture_u32(&included,1); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,0); fixture_u32(&included,1);
    fixture_compressed(&decoded,included.data,(int)included.size); }
  fixture_u32(&decoded,800); fixture_u32(&decoded,0); /* help */
  fixture_u32(&decoded,500); fixture_u32(&decoded,1); /* library code */
  fixture_string(&decoded,"global.fixture_executable_started = 1;");
  fixture_u32(&decoded,700); fixture_u32(&decoded,0); /* room order */

  memset(executable,0,sizeof(*executable));
  executable->data[0]='M'; executable->data[1]='Z'; executable->size=16;
  fixture_u32(executable,20); /* embedded-data self pointer */
  fixture_u32(executable,GMLC_CLASSIC_MAGIC); fixture_u32(executable,800);
  fixture_u32(executable,0); fixture_u32(executable,800);
  fixture_u32(executable,0); /* settings */
  fixture_u32(executable,0); fixture_u32(executable,0); /* wrapper strings */
  fixture_u32(executable,0); fixture_u32(executable,0); /* junk counts */
  for(unsigned i=0;i<256;i++) executable->data[executable->size++]=(unsigned char)i;
  fixture_u32(executable,(unsigned)decoded.size+1);
  unsigned char previous=0;
  executable->data[executable->size++]=previous;
  for(size_t i=0;i<decoded.size;i++){
    previous=(unsigned char)(previous+decoded.data[i]+(unsigned char)(i+1));
    executable->data[executable->size++]=previous;
  }
  return 1;
}

static int expect_executable_manifest(void){
  Fixture executable;
  if(!build_executable_fixture(&executable)) return 0;
  GmlcClassicManifest manifest;
  char err[256]={0};
  int ok=gmlc_classic_manifest(executable.data,executable.size,&manifest,err,sizeof(err));
  if(!ok) fprintf(stderr,"executable manifest failed: %s\n",err);
  if(ok){
    ok=manifest.inventory.header.version==GMLC_CLASSIC_GM8 &&
       manifest.inventory.header.game_id==0x13572468 && manifest.room_order_count==0 &&
       manifest.extension_count==1 && !strcmp(manifest.extension_names[0],"fixture_executable_extension") &&
       manifest.existing[GMLC_CLASSIC_SCRIPT]==1 &&
       !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].source,"exit;") &&
       manifest.trigger_def_count==1 && manifest.constant_def_count==2 &&
       !strcmp(manifest.constant_defs[0].value,"7*6") && !strcmp(manifest.constant_defs[1].value,"21*2") &&
       manifest.included_file_count==1 && manifest.included_files[0].data_size==2 &&
       !strcmp(manifest.included_files[0].file_name,"executable.dat") &&
       manifest.library_creation_code_count==2;
    gmlc_classic_manifest_free(&manifest);
  }
  return ok;
}

static Fixture legacy_fixture(unsigned container_version){
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
    fixture_u32(&f, 0);
  }
  fixture_u32(&f, 100000); fixture_u32(&f, 1000000);
  return f;
}

static int build_project_fixture(unsigned version, Fixture *out){
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

static int expect_legacy_manifest(unsigned version){
  Fixture plain = legacy_fixture(version);
  const unsigned char *data = plain.data;
  size_t size = plain.size;
  unsigned char *encoded = NULL;
  if(version == 701 || version == 702){
    encoded = encode_gm7(plain.data, plain.size, &size);
    if(!encoded) return 0;
    data = encoded;
  }
  GmlcClassicManifest manifest;
  char err[128];
  int ok = gmlc_classic_manifest(data, size, &manifest, err, sizeof(err));
  if(!ok) fprintf(stderr, "legacy manifest %u failed: %s\n", version, err);
  if(ok){
    ok = manifest.inventory.settings_version == (version == 600 ? 600u : 702u) &&
         manifest.inventory.last_instance_id == 100000 &&
         manifest.inventory.last_tile_id == 1000000;
    gmlc_classic_manifest_free(&manifest);
  }
  free(encoded);
  return ok;
}

static int expect_script_import(void){
  Fixture fixture = manifest_fixture(800);
  GmlcClassicManifest manifest;
  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_script_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  if(!gmlc_classic_manifest(fixture.data, fixture.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "script import manifest failed: %s\n", err);
    return 0;
  }
  int ok = gmlc_classic_import_scripts(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "script import failed: %s\n", err);
  if(ok){
    FILE *file = fopen(project.scripts[0].source_path, "rb");
    char text[32] = {0};
    size_t got = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    if(file) fclose(file);
    ok = project.n_scripts == 1 && project.n_script_order == 1 && got == 9 &&
         !strcmp(project.scripts[0].name, "resource_script") && !strcmp(text, "return 7;");
    remove(project.scripts[0].source_path);
  }
  for(int i = 0; i < project.n_scripts; ++i){
    free(project.scripts[i].id); free(project.scripts[i].name); free(project.scripts[i].source_path);
  }
  free(project.scripts);
  for(int i = 0; i < project.n_script_order; ++i) free(project.script_order_ids[i]);
  free(project.script_order_ids);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_sprite_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_SPRITE] = 1;
  manifest.existing[GMLC_CLASSIC_SPRITE] = 1;
  manifest.slots[GMLC_CLASSIC_SPRITE] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_SPRITE]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_SPRITE][0];
  slot->exists = 1;
  slot->name = strdup("resource_sprite");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 1); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
  fixture_u32(&payload, 800); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
  const unsigned char bgra[8] = {3, 2, 1, 255, 6, 5, 4, 128};
  fixture_u32(&payload, sizeof(bgra));
  memcpy(payload.data + payload.size, bgra, sizeof(bgra)); payload.size += sizeof(bgra);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_sprite_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_sprites(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "sprite import failed: %s\n", err);
  if(ok){
    struct stat st;
    ok = project.n_sprites == 1 && project.sprites[0].runtime_id == 0 &&
         project.sprites[0].width == 2 && project.sprites[0].height == 1 &&
         project.sprites[0].xorig == 1 && project.sprites[0].yorig == 2 &&
         project.sprites[0].bbox_right == 1 &&
         !stat(project.sprites[0].frame_paths[0], &st) && st.st_size > 0;
    remove(project.sprites[0].frame_paths[0]);
  }
  for(int i = 0; i < project.n_sprites; ++i){
    free(project.sprites[i].id); free(project.sprites[i].name);
    for(int frame = 0; frame < project.sprites[i].n_frames; ++frame) free(project.sprites[i].frame_paths[frame]);
    free(project.sprites[i].frame_paths);
  }
  free(project.sprites);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_sound_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_SOUND] = 1;
  manifest.existing[GMLC_CLASSIC_SOUND] = 1;
  manifest.slots[GMLC_CLASSIC_SOUND] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_SOUND]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_SOUND][0];
  slot->exists = 1; slot->name = strdup("resource_sound");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 0); fixture_string(&payload, ".wav"); fixture_string(&payload, "fixture.wav");
  fixture_u32(&payload, 1);
  const unsigned char audio[] = {'R','I','F','F'};
  fixture_u32(&payload, sizeof(audio)); memcpy(payload.data + payload.size, audio, sizeof(audio)); payload.size += sizeof(audio);
  fixture_u32(&payload, 0); fixture_double(&payload, 0.5); fixture_double(&payload, 0.0); fixture_u32(&payload, 1);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_sound_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_sounds(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "sound import failed: %s\n", err);
  if(ok){
    FILE *file = fopen(project.sounds[0].data_path, "rb");
    unsigned char got[4] = {0};
    size_t count = file ? fread(got, 1, sizeof(got), file) : 0;
    if(file) fclose(file);
    ok = project.n_sounds == 1 && count == sizeof(got) && !memcmp(got, audio, sizeof(audio)) &&
         project.sounds[0].volume > 0.49f && project.sounds[0].volume < 0.51f;
    remove(project.sounds[0].data_path);
  }
  for(int i = 0; i < project.n_sounds; ++i){
    free(project.sounds[i].id); free(project.sounds[i].name); free(project.sounds[i].data_path);
  }
  free(project.sounds);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int setup_legacy_slot(GmlcClassicManifest *manifest, GmlcClassicResourceType type,
                             const char *name, const Fixture *payload){
  memset(manifest,0,sizeof(*manifest));
  manifest->inventory.resource_slots[type]=1;
  manifest->existing[type]=1;
  manifest->slots[type]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest->slots[type]) return 0;
  GmlcClassicResourceSlot *slot=&manifest->slots[type][0];
  slot->exists=1; slot->legacy_layout=1; slot->version=600; slot->name=strdup(name);
  slot->payload=(uint8_t*)malloc(payload->size); slot->payload_size=payload->size;
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(manifest); return 0; }
  memcpy(slot->payload,payload->data,payload->size);
  return 1;
}

static int expect_legacy_media_import(void){
  char err[256],dir[128]="tmp/classic_legacy_media_fixture";
  err[0]='\0';
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=1;
  {
    Fixture payload={{0},0};
    fixture_u32(&payload,0); fixture_string(&payload,".wav"); fixture_string(&payload,"fixture.wav");
    fixture_u32(&payload,1); const unsigned char raw[]={'R','I','F','F'}; fixture_compressed(&payload,raw,sizeof(raw));
    fixture_u32(&payload,0); fixture_double(&payload,0.75); fixture_double(&payload,0); fixture_u32(&payload,1);
    GmlcClassicManifest manifest; GmlcProject project; memset(&project,0,sizeof(project));
    ok=setup_legacy_slot(&manifest,GMLC_CLASSIC_SOUND,"legacy_sound",&payload) &&
       gmlc_classic_import_sounds(&manifest,&project,dir,err,sizeof(err));
    if(ok){ unsigned char got[4]={0}; FILE *file=fopen(project.sounds[0].data_path,"rb");
      size_t count=file?fread(got,1,4,file):0; if(file) fclose(file);
      ok=count==4 && !memcmp(got,raw,4) && project.sounds[0].volume>0.74f;
      remove(project.sounds[0].data_path);
    }
    for(int i=0;i<project.n_sounds;i++){ free(project.sounds[i].id); free(project.sounds[i].name); free(project.sounds[i].data_path); }
    free(project.sounds); gmlc_classic_manifest_free(&manifest);
  }
  {
    Fixture payload={{0},0};
    const unsigned fields[13]={2,1,0,1,0,0,1,0,1,0,1,1,0};
    for(int i=0;i<13;i++) fixture_u32(&payload,fields[i]);
    fixture_u32(&payload,1); fixture_legacy_bmp_image(&payload);
    GmlcClassicManifest manifest; GmlcProject project; memset(&project,0,sizeof(project));
    int stage=setup_legacy_slot(&manifest,GMLC_CLASSIC_SPRITE,"legacy_sprite",&payload) &&
      gmlc_classic_import_sprites(&manifest,&project,dir,err,sizeof(err));
    if(stage){ struct stat st; stage=project.n_sprites==1 && project.sprites[0].width==2 &&
      project.sprites[0].xorig==1 && !stat(project.sprites[0].frame_paths[0],&st) && st.st_size>0;
      remove(project.sprites[0].frame_paths[0]); }
    ok=ok&&stage;
    for(int i=0;i<project.n_sprites;i++){ free(project.sprites[i].id); free(project.sprites[i].name);
      for(int f=0;f<project.sprites[i].n_frames;f++) free(project.sprites[i].frame_paths[f]);
      free(project.sprites[i].frame_paths); }
    free(project.sprites); gmlc_classic_manifest_free(&manifest);
  }
  {
    Fixture payload={{0},0};
    const unsigned fields[12]={2,1,1,0,1,1,1,1,0,0,0,0};
    for(int i=0;i<12;i++) fixture_u32(&payload,fields[i]);
    fixture_u32(&payload,1); fixture_legacy_bmp_image(&payload);
    GmlcClassicManifest manifest; GmlcProject project; memset(&project,0,sizeof(project));
    int stage=setup_legacy_slot(&manifest,GMLC_CLASSIC_BACKGROUND,"legacy_background",&payload) &&
      gmlc_classic_import_backgrounds(&manifest,&project,dir,err,sizeof(err));
    if(stage){ struct stat st; stage=project.n_tilesets==1 && project.n_sprites==1 &&
      project.tilesets[0].tile_width==1 && !stat(project.sprites[0].frame_paths[0],&st) && st.st_size>0;
      remove(project.sprites[0].frame_paths[0]); }
    ok=ok&&stage;
    for(int i=0;i<project.n_sprites;i++){ free(project.sprites[i].id); free(project.sprites[i].name);
      for(int f=0;f<project.sprites[i].n_frames;f++) free(project.sprites[i].frame_paths[f]);
      free(project.sprites[i].frame_paths); }
    for(int i=0;i<project.n_tilesets;i++){ free(project.tilesets[i].id); free(project.tilesets[i].name); }
    free(project.sprites); free(project.tilesets); gmlc_classic_manifest_free(&manifest);
  }
#ifndef _WIN32
  rmdir(dir);
#endif
  if(!ok) fprintf(stderr,"legacy media import failed: %s\n",err);
  return ok;
}

static int expect_background_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_BACKGROUND] = 1;
  manifest.existing[GMLC_CLASSIC_BACKGROUND] = 1;
  manifest.slots[GMLC_CLASSIC_BACKGROUND] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_BACKGROUND]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_BACKGROUND][0];
  slot->exists = 1; slot->name = strdup("resource_background");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 1); fixture_u32(&payload, 1); fixture_u32(&payload, 1);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 800); fixture_u32(&payload, 1); fixture_u32(&payload, 1);
  fixture_u32(&payload, 4);
  payload.data[payload.size++] = 3; payload.data[payload.size++] = 2;
  payload.data[payload.size++] = 1; payload.data[payload.size++] = 255;
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_background_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_backgrounds(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "background import failed: %s\n", err);
  if(ok) ok = project.n_sprites == 1 && project.sprites[0].runtime_id == -1 &&
              project.n_tilesets == 1 && project.tilesets[0].sprite_id == 0;
  if(project.n_sprites){
    remove(project.sprites[0].frame_paths[0]);
    free(project.sprites[0].frame_paths[0]); free(project.sprites[0].frame_paths);
    free(project.sprites[0].id); free(project.sprites[0].name);
  }
  free(project.sprites);
  if(project.n_tilesets){ free(project.tilesets[0].id); free(project.tilesets[0].name); }
  free(project.tilesets);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_font_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]=1;
  manifest.existing[GMLC_CLASSIC_FONT]=1;
  manifest.slots[GMLC_CLASSIC_FONT]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_FONT]) return 0;
  GmlcClassicResourceSlot *slot=&manifest.slots[GMLC_CLASSIC_FONT][0];
  slot->exists=1; slot->name=strdup("resource_font");
  Fixture payload={{0},0};
  fixture_string(&payload,"sans"); fixture_u32(&payload,12); fixture_u32(&payload,0);
  fixture_u32(&payload,0); fixture_u32(&payload,32); fixture_u32(&payload,127);
  slot->payload=(uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload,payload.data,payload.size); slot->payload_size=payload.size;
  GmlcProject project; memset(&project,0,sizeof(project));
  char err[256],dir[128]="tmp/classic_font_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=gmlc_classic_import_fonts(&manifest,&project,dir,err,sizeof(err));
  if(!ok) fprintf(stderr,"font import failed: %s\n",err);
  if(ok) ok=project.n_fonts==1 && project.fonts[0].n_glyphs==96 && project.fonts[0].png_path;
  if(project.n_fonts){
    remove(project.fonts[0].png_path); free(project.fonts[0].id); free(project.fonts[0].name);
    free(project.fonts[0].png_path); free(project.fonts[0].glyphs);
  }
  free(project.fonts); gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_path_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_PATH] = 1;
  manifest.existing[GMLC_CLASSIC_PATH] = 1;
  manifest.slots[GMLC_CLASSIC_PATH] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_PATH]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_PATH][0];
  slot->exists = 1; slot->name = strdup("resource_path");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 1); fixture_u32(&payload, 1); fixture_u32(&payload, 4);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 16); fixture_u32(&payload, 16);
  fixture_u32(&payload, 2);
  fixture_double(&payload, 1.5); fixture_double(&payload, 2.5); fixture_double(&payload, 100.0);
  fixture_double(&payload, 9.5); fixture_double(&payload, 8.5); fixture_double(&payload, 50.0);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;
  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256];
  int ok = gmlc_classic_import_paths(&manifest, &project, err, sizeof(err));
  if(!ok) fprintf(stderr, "path import failed: %s\n", err);
  if(ok) ok = project.n_paths == 1 && project.paths[0].kind == 1 && project.paths[0].closed &&
              project.paths[0].precision == 4 && project.paths[0].n_points == 2 &&
              project.paths[0].points[1].x > 9.49f && project.paths[0].points[1].speed == 50.0f;
  for(int i = 0; i < project.n_paths; ++i){
    free(project.paths[i].id); free(project.paths[i].name); free(project.paths[i].points);
  }
  free(project.paths);
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_timeline_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_TIMELINE]=1;
  manifest.existing[GMLC_CLASSIC_TIMELINE]=1;
  manifest.slots[GMLC_CLASSIC_TIMELINE]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_TIMELINE]) return 0;
  GmlcClassicResourceSlot *slot=&manifest.slots[GMLC_CLASSIC_TIMELINE][0];
  slot->exists=1; slot->name=strdup("resource_timeline");
  Fixture payload={{0},0};
  fixture_u32(&payload,1);  /* one moment */
  fixture_u32(&payload,12); /* moment position */
  fixture_u32(&payload,400); fixture_u32(&payload,1); /* action-list version/count */
  fixture_u32(&payload,440); fixture_u32(&payload,1); fixture_u32(&payload,603);
  fixture_u32(&payload,7); fixture_u32(&payload,0); fixture_u32(&payload,0);
  fixture_u32(&payload,0); fixture_u32(&payload,2);
  fixture_string(&payload,""); fixture_string(&payload,"");
  fixture_u32(&payload,1); fixture_u32(&payload,8);
  for(int i=0;i<8;i++) fixture_u32(&payload,0);
  fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0); fixture_u32(&payload,8);
  fixture_string(&payload,"x += 2;");
  for(int i=1;i<8;i++) fixture_string(&payload,"");
  fixture_u32(&payload,0);
  slot->payload=(uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload,payload.data,payload.size); slot->payload_size=payload.size;

  GmlcProject project; memset(&project,0,sizeof(project));
  char err[256],dir[128]="tmp/classic_timeline_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=gmlc_classic_import_timelines(&manifest,&project,dir,err,sizeof(err));
  if(!ok) fprintf(stderr,"timeline import failed: %s\n",err);
  if(ok){
    char source[64]={0};
    FILE *file=fopen(project.timelines[0].moments[0].source_path,"rb");
    size_t got=file?fread(source,1,sizeof(source)-1,file):0;
    if(file) fclose(file);
    ok=project.n_timelines==1 && project.timelines[0].n_moments==1 &&
       project.timelines[0].moments[0].step==12 && got && strstr(source,"x += 2;");
    remove(project.timelines[0].moments[0].source_path);
  }
  for(int i=0;i<project.n_timelines;i++){
    free(project.timelines[i].id); free(project.timelines[i].name);
    for(int m=0;m<project.timelines[i].n_moments;m++) free(project.timelines[i].moments[m].source_path);
    free(project.timelines[i].moments);
  }
  free(project.timelines); gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_object_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_OBJECT] = 1;
  manifest.existing[GMLC_CLASSIC_OBJECT] = 1;
  manifest.slots[GMLC_CLASSIC_OBJECT] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_OBJECT]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_OBJECT][0];
  slot->exists = 1; slot->name = strdup("resource_object");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  fixture_u32(&payload, (unsigned)-10); fixture_u32(&payload, 0); fixture_u32(&payload, (unsigned)-100);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); /* fields + final event type */
  fixture_u32(&payload, 0); /* Create subtype */
  fixture_u32(&payload, 400); fixture_u32(&payload, 1);
  fixture_u32(&payload, 440); fixture_u32(&payload, 1); fixture_u32(&payload, 603);
  fixture_u32(&payload, 7); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 2);
  fixture_string(&payload, ""); fixture_string(&payload, "");
  fixture_u32(&payload, 1); fixture_u32(&payload, 8);
  for(int i = 0; i < 8; ++i) fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 8);
  fixture_string(&payload, "x = 4;");
  for(int i = 1; i < 8; ++i) fixture_string(&payload, "");
  fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); /* end Create event list */
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_object_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_objects(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "object import failed: %s\n", err);
  if(ok){
    char source[64] = {0};
    FILE *file = fopen(project.objects[0].events[0].source_path, "rb");
    size_t got = file ? fread(source, 1, sizeof(source) - 1, file) : 0;
    if(file) fclose(file);
    ok = project.n_objects == 1 && project.objects[0].depth == -10 && project.objects[0].n_events == 1 &&
         project.objects[0].events[0].event_type == 0 && got && strstr(source, "x = 4;");
    remove(project.objects[0].events[0].source_path);
  }
  for(int i = 0; i < project.n_objects; ++i){
    free(project.objects[i].id); free(project.objects[i].name);
    for(int event = 0; event < project.objects[i].n_events; ++event){
      free(project.objects[i].events[event].id); free(project.objects[i].events[event].source_path);
    }
    free(project.objects[i].events);
  }
  free(project.objects);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_room_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_ROOM] = 1;
  manifest.inventory.last_instance_id = 100001;
  manifest.existing[GMLC_CLASSIC_ROOM] = 1;
  manifest.slots[GMLC_CLASSIC_ROOM] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_ROOM]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_ROOM][0];
  slot->exists = 1; slot->name = strdup("resource_room");
  Fixture payload = {{0}, 0};
  fixture_string(&payload, "caption");
  fixture_u32(&payload, 320); fixture_u32(&payload, 240); fixture_u32(&payload, 16);
  fixture_u32(&payload, 16); fixture_u32(&payload, 0); fixture_u32(&payload, 30);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0x00112233); fixture_u32(&payload, 1);
  fixture_string(&payload, "global.room_ready = 1;");
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  for(int field = 0; field < 14; ++field) fixture_u32(&payload, field == 3 ? 320 : field == 4 ? 240 : 0);
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 12); fixture_u32(&payload, 34); fixture_u32(&payload, 0); fixture_u32(&payload, 100001);
  fixture_string(&payload, "x += 1;"); fixture_u32(&payload, 0);
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 2); fixture_u32(&payload, 3); fixture_u32(&payload, 16);
  fixture_u32(&payload, 16); fixture_u32(&payload, 100); fixture_u32(&payload, 1000001); fixture_u32(&payload, 0);
  for(int field = 0; field < 14; ++field) fixture_u32(&payload, 0);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_room_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_rooms(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "room import failed: %s\n", err);
  if(ok) ok = project.n_rooms == 1 && project.rooms[0].width == 320 &&
              project.rooms[0].n_backgrounds == 1 && project.rooms[0].n_instances == 1 &&
              project.rooms[0].instances[0].instance_id == 100001 && project.rooms[0].n_tiles == 1 &&
              project.rooms[0].tiles[0].depth == 100 && project.next_instance_id == 100002;
  if(project.n_rooms){
    GmlcRoom *room = &project.rooms[0];
    if(room->creation_code_path) remove(room->creation_code_path);
    free(room->creation_code_path); free(room->id); free(room->name);
    for(int i = 0; i < room->n_instances; ++i){
      if(room->instances[i].creation_code_path) remove(room->instances[i].creation_code_path);
      free(room->instances[i].creation_code_path); free(room->instances[i].id); free(room->instances[i].name);
    }
    free(room->instances); free(room->backgrounds); free(room->tiles);
  }
  free(project.rooms);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static void discard_imported_objects(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_objects; ++i){
    free(project->objects[i].id);
    free(project->objects[i].name);
    for(int event = 0; event < project->objects[i].n_events; ++event){
      free(project->objects[i].events[event].id);
      free(project->objects[i].events[event].collision_id);
      if(remove_sources && project->objects[i].events[event].source_path)
        remove(project->objects[i].events[event].source_path);
      free(project->objects[i].events[event].source_path);
    }
    free(project->objects[i].events);
  }
  free(project->objects);
  project->objects = NULL;
  project->n_objects = project->cap_objects = 0;
}

static void discard_imported_backgrounds(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_sprites; ++i){
    free(project->sprites[i].id); free(project->sprites[i].name);
    for(int frame = 0; frame < project->sprites[i].n_frames; ++frame){
      if(remove_sources && project->sprites[i].frame_paths && project->sprites[i].frame_paths[frame])
        remove(project->sprites[i].frame_paths[frame]);
      free(project->sprites[i].frame_paths ? project->sprites[i].frame_paths[frame] : NULL);
    }
    free(project->sprites[i].frame_paths);
  }
  free(project->sprites);
  for(int i = 0; i < project->n_tilesets; ++i){
    free(project->tilesets[i].id); free(project->tilesets[i].name);
  }
  free(project->tilesets);
  memset(project, 0, sizeof(*project));
}

static void discard_imported_timelines(GmlcProject *project, int remove_sources){
  for(int i=0;i<project->n_timelines;i++){
    free(project->timelines[i].id); free(project->timelines[i].name);
    for(int m=0;m<project->timelines[i].n_moments;m++){
      if(remove_sources && project->timelines[i].moments[m].source_path)
        remove(project->timelines[i].moments[m].source_path);
      free(project->timelines[i].moments[m].source_path);
    }
    free(project->timelines[i].moments);
  }
  free(project->timelines);
  project->timelines=NULL; project->n_timelines=project->cap_timelines=0;
}

static void discard_imported_rooms(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_rooms; ++i){
    GmlcRoom *room = &project->rooms[i];
    if(remove_sources && room->creation_code_path) remove(room->creation_code_path);
    free(room->creation_code_path); free(room->id); free(room->name);
    for(int instance = 0; instance < room->n_instances; ++instance){
      if(remove_sources && room->instances[instance].creation_code_path)
        remove(room->instances[instance].creation_code_path);
      free(room->instances[instance].creation_code_path);
      free(room->instances[instance].id); free(room->instances[instance].name);
    }
    free(room->instances); free(room->backgrounds); free(room->tiles);
  }
  free(project->rooms);
  memset(project, 0, sizeof(*project));
}

int main(int argc, char **argv){
  if(argc==3 && !strcmp(argv[1],"--write-exe-fixture")){
    Fixture executable;
    if(!build_executable_fixture(&executable)) return 1;
    FILE *file=fopen(argv[2],"wb");
    int ok=file && fwrite(executable.data,1,executable.size,file)==executable.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write executable fixture: %s\n",argv[2]); return 1; }
    printf("wrote executable fixture: %s (%zu bytes)\n",argv[2],executable.size);
    return 0;
  }
  if(argc==4 && !strcmp(argv[1],"--write-project-fixture")){
    unsigned version=(unsigned)strtoul(argv[2],NULL,10);
    Fixture project;
    if(!build_project_fixture(version,&project)){
      fprintf(stderr,"unsupported project fixture version: %s\n",argv[2]);
      return 1;
    }
    FILE *file=fopen(argv[3],"wb");
    int ok=file && fwrite(project.data,1,project.size,file)==project.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write project fixture: %s\n",argv[3]); return 1; }
    printf("wrote project fixture %u: %s (%zu bytes)\n",version,argv[3],project.size);
    return 0;
  }
  const unsigned versions[] = {600, 701, 702, 800, 810};
  int passed = 0, failed = 0;
  for(size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i){
    if(expect_header(versions[i])) ++passed; else ++failed;
  }
  if(expect_rejected(0, 800, 28)) ++passed; else ++failed;
  if(expect_rejected(GMLC_CLASSIC_MAGIC, 999, 28)) ++passed; else ++failed;
  if(expect_rejected(GMLC_CLASSIC_MAGIC, 800, 27)) ++passed; else ++failed;

  if(expect_inventory(800)) ++passed; else ++failed;
  if(expect_inventory(810)) ++passed; else ++failed;
  if(expect_manifest()) ++passed; else ++failed;
  if(expect_manifest_810()) ++passed; else ++failed;
  if(expect_executable_manifest()) ++passed; else ++failed;
  if(expect_gm7_decode()) ++passed; else ++failed;
  if(expect_legacy_manifest(600)) ++passed; else ++failed;
  if(expect_legacy_manifest(701)) ++passed; else ++failed;
  if(expect_script_import()) ++passed; else ++failed;
  if(expect_sprite_import()) ++passed; else ++failed;
  if(expect_background_import()) ++passed; else ++failed;
  if(expect_font_import()) ++passed; else ++failed;
  if(expect_sound_import()) ++passed; else ++failed;
  if(expect_legacy_media_import()) ++passed; else ++failed;
  if(expect_path_import()) ++passed; else ++failed;
  if(expect_timeline_import()) ++passed; else ++failed;
  if(expect_object_import()) ++passed; else ++failed;
  if(expect_room_import()) ++passed; else ++failed;

  for(int i = 1; i < argc; ++i){
    GmlcClassicInventory in;
    GmlcClassicHeader h;
    GmlcClassicManifest manifest;
    char err[512];
    if(!gmlc_classic_manifest_file(argv[i], &manifest, err, sizeof(err))){
      fprintf(stderr, "%s: %s\n", argv[i], err);
      ++failed;
      continue;
    }
    in=manifest.inventory;
    h=in.header;
      printf("%s\t%u\t%s\tsettings=%u sounds=%u/%u sprites=%u/%u backgrounds=%u/%u paths=%u/%u scripts=%u/%u fonts=%u/%u timelines=%u/%u objects=%u/%u rooms=%u/%u\n",
             argv[i], (unsigned)h.version, gmlc_classic_version_name(h.version), in.settings_version,
             manifest.existing[GMLC_CLASSIC_SOUND], in.resource_slots[GMLC_CLASSIC_SOUND],
             manifest.existing[GMLC_CLASSIC_SPRITE], in.resource_slots[GMLC_CLASSIC_SPRITE],
             manifest.existing[GMLC_CLASSIC_BACKGROUND], in.resource_slots[GMLC_CLASSIC_BACKGROUND],
             manifest.existing[GMLC_CLASSIC_PATH], in.resource_slots[GMLC_CLASSIC_PATH],
             manifest.existing[GMLC_CLASSIC_SCRIPT], in.resource_slots[GMLC_CLASSIC_SCRIPT],
             manifest.existing[GMLC_CLASSIC_FONT], in.resource_slots[GMLC_CLASSIC_FONT],
             manifest.existing[GMLC_CLASSIC_TIMELINE], in.resource_slots[GMLC_CLASSIC_TIMELINE],
             manifest.existing[GMLC_CLASSIC_OBJECT], in.resource_slots[GMLC_CLASSIC_OBJECT],
             manifest.existing[GMLC_CLASSIC_ROOM], in.resource_slots[GMLC_CLASSIC_ROOM]);
      GmlcProject project;
      memset(&project, 0, sizeof(project));
      const char *object_dir = "tmp/classic_object_corpus";
#ifdef _WIN32
      _mkdir(object_dir);
#else
      mkdir(object_dir, 0777);
#endif
      if(!gmlc_classic_import_objects(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: object import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_objects(&project, 1);
      if(!gmlc_classic_import_timelines(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: timeline import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_timelines(&project, 1);
      if(h.version >= GMLC_CLASSIC_GM8 &&
         !gmlc_classic_import_backgrounds(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: background import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_backgrounds(&project, 1);
      if(!gmlc_classic_import_rooms(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: room import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_rooms(&project, 1);
      gmlc_classic_manifest_free(&manifest);
    ++passed;
  }
  printf("classic probe: passed=%d failed=%d\n", passed, failed);
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
